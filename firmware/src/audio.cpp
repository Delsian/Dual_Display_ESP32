#include <Arduino.h>
#include "audio.h"
#include "config.h"
#include "device_config.h"
#include "speech_test.h"
#include "audio_vad.h"
#include "speech_clip.h"

#if USE_AUDIO
#include <Wire.h>
#include <driver/i2s.h>
#include <esp_heap_caps.h>

namespace {
constexpr i2s_port_t AUDIO_PORT = I2S_NUM_0;
constexpr uint32_t SAMPLE_RATE = 16000; // Recording rate; clips use 24 kHz.
constexpr size_t FRAME_BYTES = 2 * sizeof(int16_t);
constexpr size_t CHUNK_BYTES = 256 * FRAME_BYTES;
constexpr size_t BUFFER_BYTES = SAMPLE_RATE * FRAME_BYTES * AUDIO_RECORD_SECONDS;
constexpr size_t PREROLL_BYTES = SAMPLE_RATE * FRAME_BYTES * AUDIO_VAD_PREROLL_MS / 1000;
constexpr uint32_t DEBOUNCE_MS = 20;
constexpr uint8_t MIC_ADDRESS = 0x40;
constexpr uint8_t SPEAKER_ADDRESS = 0x18;
static_assert(AUDIO_RECORD_SECONDS > 0, "Recording buffer must be nonempty");
static_assert(AUDIO_RECORD_SECONDS <= 5, "Voice recordings must not exceed five seconds");
static_assert(PREROLL_BYTES > 0 && PREROLL_BYTES < BUFFER_BYTES, "Invalid pre-roll size");
static_assert(AUDIO_OUTPUT_VOLUME >= 0 && AUDIO_OUTPUT_VOLUME <= 100, "Invalid audio volume");

uint8_t *record_buffer = nullptr;
TaskHandle_t audio_task_handle = nullptr;

struct RegisterValue {
  uint8_t reg;
  uint8_t value;
};

bool write_register(uint8_t address, uint8_t reg, uint8_t value) {
  Wire.beginTransmission(address);
  Wire.write(reg);
  Wire.write(value);
  if (Wire.endTransmission() == 0) return true;
  Serial.printf("Audio: I2C write failed at 0x%02x register 0x%02x\n", address, reg);
  return false;
}

template <size_t N>
bool configure_registers(uint8_t address, const RegisterValue (&values)[N]) {
  for (const auto &entry : values) {
    if (!write_register(address, entry.reg, entry.value)) return false;
  }
  return true;
}

bool init_codecs() {
  const uint32_t volume = device_config().audio_volume;
  // Fixed 16 kHz / 4.096 MHz MCLK setup from Waveshare's ES8311/ES7210
  // reference drivers: https://github.com/waveshareteam/ESP32-S3-DualEye-Touch-LCD-1.28
  if (!write_register(SPEAKER_ADDRESS, 0x00, 0x1f)) return false;
  delay(20);
  const RegisterValue speaker[] = {
    {0x00, 0x00}, {0x00, 0x80}, // Reset, power on, slave mode.
    {0x01, 0x3f}, // Enable clocks, external MCLK, normal polarity.
    {0x02, 0x00}, {0x03, 0x10}, {0x04, 0x10}, {0x05, 0x00},
    {0x06, 0x03}, {0x07, 0x00}, {0x08, 0xff}, // 256x MCLK dividers.
    {0x09, 0x0c}, {0x0a, 0x0c}, // Standard I2S, 16-bit.
    {0x0d, 0x01}, {0x0e, 0x02}, {0x12, 0x00}, {0x13, 0x10},
    {0x1c, 0x6a}, {0x37, 0x08}, // Analog power, DAC, bypass equalizer.
    {0x32, static_cast<uint8_t>(volume == 0 ? 0 : volume * 256 / 100 - 1)}
  };
  const RegisterValue microphone[] = {
    {0x00, 0xff}, {0x00, 0x32}, // Reset.
    {0x09, 0x30}, {0x0a, 0x30}, // Power-up timing.
    {0x23, 0x2a}, {0x22, 0x0a}, {0x21, 0x2a}, {0x20, 0x0a}, // HPF.
    {0x11, 0x60}, {0x12, 0x00}, // 16-bit standard I2S, MIC1/2 on SDOUT1.
    {0x40, 0xc3}, {0x41, 0x70}, {0x42, 0x70}, // Analog power, 2.87 V bias.
    {0x43, 0x1a}, {0x44, 0x1a}, {0x45, 0x1a}, {0x46, 0x1a}, // 30 dB gain.
    {0x47, 0x08}, {0x48, 0x08}, {0x49, 0x08}, {0x4a, 0x08},
    {0x07, 0x20}, {0x02, 0xc1}, {0x04, 0x01}, {0x05, 0x00}, // 16 kHz.
    {0x06, 0x04}, {0x4b, 0x0f}, {0x4c, 0x0f}, // Power on ADCs and PGAs.
    {0x00, 0x71}, {0x00, 0x41}, // Enable ADC.
    {0x1b, 0xd3}, {0x1c, 0xd3}, {0x1d, 0xd3}, {0x1e, 0xd3} // +10 dB.
  };
  return configure_registers(SPEAKER_ADDRESS, speaker) &&
         configure_registers(MIC_ADDRESS, microphone);
}

bool set_audio_rate(uint32_t rate) {
  // ES8311 uses the same 256x MCLK dividers at 16 and 24 kHz. RX is
  // discarded during clip playback; restore its 16 kHz clock before capture.
  if (i2s_set_clk(AUDIO_PORT, rate, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_STEREO) != ESP_OK ||
      i2s_zero_dma_buffer(AUDIO_PORT) != ESP_OK) {
    Serial.println("Audio: sample-rate change failed; audio task stopped.");
    return false;
  }
  return true;
}

void audio_task(void *) {
  enum class State { Idle, Recording, Playing, Draining };
  State state = State::Idle;
  bool stable_pressed = false;
  bool previous_raw = false;
  uint32_t changed_at = millis();
  size_t recorded = 0;
  size_t played = 0;
  size_t silence_written = 0;
  SpeechAudio *speech = nullptr; // Reply audio, possibly still downloading.
  uint8_t chunk[CHUNK_BYTES];
  const uint8_t silence[CHUNK_BYTES] = {};
  uint8_t *preroll = record_buffer + BUFFER_BYTES;
  size_t pre_write = 0, pre_used = 0;
  uint32_t last_blocked = millis();
  AudioVad vad(AUDIO_VAD_MIN_RMS, SAMPLE_RATE * AUDIO_VAD_START_MS / 1000,
               SAMPLE_RATE * AUDIO_VAD_SILENCE_MS / 1000);

  auto finish_recording = [&](const char *reason) {
    Serial.printf("Audio: stopped (%s), %u ms.\n", reason,
                  unsigned(recorded * 1000 / (SAMPLE_RATE * FRAME_BYTES)));
    vad.reset();
    pre_write = pre_used = 0;
    last_blocked = millis();
    #if AUDIO_AI_REPLY_TEST
    state = State::Idle;
    audio_reply_progress(recorded, true);
    #else
    played = 0;
    silence_written = 0;
    state = recorded ? State::Playing : State::Idle;
    digitalWrite(PIN_AUDIO_PA, recorded ? HIGH : LOW);
    #endif
  };

  for (;;) {
    // RX is drained in every state, so a new recording never includes old DMA data.
    size_t received = 0;
    esp_err_t result = i2s_read(AUDIO_PORT, chunk, sizeof(chunk), &received, pdMS_TO_TICKS(20));
    if (result != ESP_OK || received % FRAME_BYTES != 0) {
      Serial.println("Audio: microphone read failed; audio task stopped.");
      break;
    }

    bool raw_pressed = digitalRead(PIN_KEY1) == LOW;
    if (raw_pressed != previous_raw) {
      previous_raw = raw_pressed;
      changed_at = millis();
    }
    if (raw_pressed != stable_pressed && millis() - changed_at >= DEBOUNCE_MS) {
      stable_pressed = raw_pressed;
      if (stable_pressed && state != State::Recording) {
        #if AUDIO_AI_REPLY_TEST
        if (!audio_reply_available()) {
          Serial.println("Audio: waiting for network/request; recording not started.");
          continue;
        }
        #endif
        digitalWrite(PIN_AUDIO_PA, LOW);
        if (speech) {
          if (!set_audio_rate(SAMPLE_RATE)) break;
          received = 0; // This iteration's RX chunk used the playback clock.
        }
        i2s_zero_dma_buffer(AUDIO_PORT);
        release_speech_audio(speech);
        speech = nullptr;
        recorded = 0;
        vad.reset();
        pre_write = pre_used = 0;
        state = State::Recording;
        Serial.println("Audio: recording.");
        #if AUDIO_AI_REPLY_TEST
        begin_audio_reply(record_buffer); // Upload while recording.
        #endif
      } else if (!stable_pressed && state == State::Recording) {
        finish_recording("KEY1 released");
      }
    }

    if (state == State::Idle && !stable_pressed) {
      speech = take_speech_audio();
      if (speech) {
        played = 0;
        silence_written = 0;
        if (!set_audio_rate(SPEECH_CLIP_SAMPLE_RATE)) break;
        digitalWrite(PIN_AUDIO_PA, HIGH);
        state = State::Playing;
        Serial.println("Speech: playing AI voice.");
      }
    }

    bool started_from_voice = false;
    #if AUDIO_VOICE_ACTIVATION
    bool available = true;
    #if AUDIO_AI_REPLY_TEST
    available = audio_reply_available();
    #endif
    if (state != State::Idle || stable_pressed || !available) {
      last_blocked = millis();
      pre_write = pre_used = 0;
      if (state != State::Recording) vad.reset();
    } else if (millis() - last_blocked >= AUDIO_VAD_COOLDOWN_MS) {
      // Keep a circular history so activation includes the start of the phrase.
      for (size_t i = 0; i < received; ++i) {
        preroll[pre_write] = chunk[i];
        pre_write = (pre_write + 1) % PREROLL_BYTES;
      }
      pre_used = min(PREROLL_BYTES, pre_used + received);
      if (vad.process(chunk, received, AUDIO_AI_MIC_CHANNEL, false) == AudioVad::Start) {
        const size_t oldest = (pre_write + PREROLL_BYTES - pre_used) % PREROLL_BYTES;
        const size_t first = min(pre_used, PREROLL_BYTES - oldest);
        memcpy(record_buffer, preroll + oldest, first);
        memcpy(record_buffer + first, preroll, pre_used - first);
        recorded = pre_used;
        vad.reset();
        state = State::Recording;
        started_from_voice = true;
        Serial.println("Audio: voice detected; recording.");
        #if AUDIO_AI_REPLY_TEST
        if (begin_audio_reply(record_buffer)) audio_reply_progress(recorded, false);
        #endif
      }
    }
    #endif

    if (state == State::Recording && recorded < BUFFER_BYTES) {
      if (!started_from_voice) {
        size_t count = min(received, BUFFER_BYTES - recorded);
        memcpy(record_buffer + recorded, chunk, count);
        recorded += count;
        #if AUDIO_AI_REPLY_TEST
        if (recorded < BUFFER_BYTES) audio_reply_progress(recorded, false);
        #endif
      }
      if (recorded == BUFFER_BYTES) {
        finish_recording("5-second limit");
      } else if (!started_from_voice &&
                 vad.process(chunk, received, AUDIO_AI_MIC_CHANNEL, true) == AudioVad::Silence) {
        finish_recording("silence");
      }
    } else if (state == State::Playing || state == State::Draining) {
      size_t available = recorded;
      bool done = true;
      const uint8_t *play_buffer = speech ? speech_audio_data(speech, available, done) : record_buffer;
      if (state == State::Playing && played == available) {
        // Wait for more download; TX auto-clear plays silence meanwhile.
        if (!done) { vTaskDelay(1); continue; }
        state = State::Draining;
      }
      const uint8_t *data = state == State::Playing ? play_buffer + played : silence;
      size_t count = state == State::Playing ? min(CHUNK_BYTES, available - played) : CHUNK_BYTES;
      size_t written = 0;
      result = i2s_write(AUDIO_PORT, data, count, &written, pdMS_TO_TICKS(20));
      if (result != ESP_OK || written == 0 || written % FRAME_BYTES != 0) {
        Serial.println("Audio: speaker write failed; audio task stopped.");
        break;
      }
      if (state == State::Playing) {
        played += written;
      } else {
        silence_written += written;
        // Push more silence than the entire TX DMA ring before muting the PA.
        if (silence_written >= 5 * CHUNK_BYTES) {
          digitalWrite(PIN_AUDIO_PA, LOW);
          if (speech && !set_audio_rate(SAMPLE_RATE)) break;
          release_speech_audio(speech);
          speech = nullptr;
          state = State::Idle;
          Serial.println("Audio: playback complete.");
        }
      }
    }
    vTaskDelay(1);
  }
  digitalWrite(PIN_AUDIO_PA, LOW);
  release_speech_audio(speech);
  i2s_driver_uninstall(AUDIO_PORT);
  heap_caps_free(record_buffer);
  record_buffer = nullptr;
  // init_audio is a startup-only API; do not restart a failed task implicitly.
  vTaskDelete(nullptr);
}
} // namespace

bool init_audio() {
  if (audio_task_handle != nullptr) return true;
  pinMode(PIN_KEY1, INPUT_PULLUP);
  pinMode(PIN_AUDIO_PA, OUTPUT);
  digitalWrite(PIN_AUDIO_PA, LOW);

  // Shared with ToF: initialize before launching the audio task; codec writes
  // happen only here. 400 kHz is supported by both codecs and the ToF sensor.
  if (!Wire.begin(PIN_AUDIO_SDA, PIN_AUDIO_SCL) || !Wire.setClock(400000)) return false;
  record_buffer = static_cast<uint8_t *>(heap_caps_malloc(BUFFER_BYTES + PREROLL_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!record_buffer) {
    Serial.println("Audio: cannot allocate recording buffer in PSRAM.");
    return false;
  }

  i2s_config_t config = {};
  config.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_RX);
  config.sample_rate = SAMPLE_RATE;
  config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  config.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  config.dma_buf_count = 4;
  config.dma_buf_len = 256;
  config.tx_desc_auto_clear = true;
  config.mclk_multiple = I2S_MCLK_MULTIPLE_256;
  i2s_pin_config_t pins = {};
  pins.mck_io_num = PIN_AUDIO_MCLK;
  pins.bck_io_num = PIN_AUDIO_BCLK;
  pins.ws_io_num = PIN_AUDIO_LRCK;
  pins.data_out_num = PIN_AUDIO_DOUT;
  pins.data_in_num = PIN_AUDIO_DIN;

  bool installed = i2s_driver_install(AUDIO_PORT, &config, 0, nullptr) == ESP_OK;
  if (installed && i2s_set_pin(AUDIO_PORT, &pins) == ESP_OK &&
      i2s_zero_dma_buffer(AUDIO_PORT) == ESP_OK && init_codecs() &&
      xTaskCreate(audio_task, "audio", 4096, nullptr, 2, &audio_task_handle) == pdPASS) {
    #if AUDIO_AI_REPLY_TEST
    #if AUDIO_VOICE_ACTIVATION
    Serial.println("Audio ready: speak to ask AI; KEY1 overrides. Stops on silence or at 5 seconds.");
    #else
    Serial.println("Audio ready: hold KEY1 to ask AI; release, silence, or time limit ends recording.");
    #endif
    #else
    Serial.println("Audio ready: hold KEY1 to record, release to replay.");
    #endif
    return true;
  }
  if (installed) i2s_driver_uninstall(AUDIO_PORT);
  heap_caps_free(record_buffer);
  record_buffer = nullptr;
  Serial.println("Audio: initialization failed.");
  return false;
}
#else
bool init_audio() { return false; }
#endif
