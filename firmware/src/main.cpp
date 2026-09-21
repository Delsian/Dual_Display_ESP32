/**
 * @file main.cpp
 * @author Intellar (https://github.com/intellar)
 * @brief Main firmware for the Parrot animated eye project.
 * @version 1.1
 *
 * @copyright Copyright (c) 2024
 *
 * @license See LICENSE.md for details.
 *
 */

#include <Arduino.h>
#include "config.h"
// #include "esp32-hal-log.h" // Disabled for debugging
#include "LittleFS.h"
// --- Libraries disabled for debugging ---
#include "drawing_tools.h"
#include "eye_logic.h"
#include "tof_sensor.h"
#include "audio.h"
#include "wifi_setup.h"
#include "device_config.h"
#include "ble_config.h"

// --- FPS Counter Variables ---
// --- LED Blink Configuration ---
#ifdef BOARD_WAVESHARE_DUALEYE
#define LED_PIN -1 // No user LED; GPIO48 is the left LCD reset.
#else
#define LED_PIN 48 // Built-in LED pin. Change if needed (e.g., LED_BUILTIN, 2, etc.)
#endif

// --- Battery Monitoring ---
#ifdef BOARD_WAVESHARE_DUALEYE
#define BATT_ADC_PIN 1
#define BATT_DIVIDER_RATIO 3.0f // 200K / 100K divider.
#else
#define BATT_ADC_PIN 4 // ADC pin for reading the battery voltage
#define BATT_DIVIDER_RATIO 2.0f
#endif


// --- FPS Counter Variables ---
static unsigned long last_fps_time = 0;
static int frame_count = 0;
static float current_fps = 0.0f;


// --- Debugging ---

/**
 * @brief Reads the battery voltage and returns it as a percentage.
 * @return The remaining battery percentage (0-100).
 */
int get_battery_percentage() {
  // Read the raw ADC value (0-4095)
  uint32_t raw_value = analogRead(BATT_ADC_PIN);

  // Convert the raw value to millivolts at the ADC pin
  // The reference voltage is approximately 3.3V (3300mV) for a maximum reading of 4095
  float adc_voltage = (raw_value / 4095.0) * 3300.0;

  // Account for the board's battery voltage divider.
  float battery_voltage = adc_voltage * BATT_DIVIDER_RATIO;

  // Map the battery voltage (3.2V-4.2V) to a percentage (0-100%)
  // map(value, input_min, input_max, output_min, output_max)
  int percentage = map(battery_voltage, 3200, 4200, 0, 100);
  return constrain(percentage, 0, 100); // Keep the value between 0 and 100
}
/**
 * @brief Initializes all subsystems.
 */
void setup() {
  // Add a fixed delay to give the serial monitor time to connect.
  delay(2000);

  Serial.begin(115200);
  // Wait for the serial port to connect. Required for the ESP32-S3 with native USB.
  // Add a timeout to avoid blocking if the monitor is not open.
  unsigned long start_time = millis();
  while (!Serial && (millis() - start_time < 2000)) {
    delay(100);
  }

  Serial.println("Booting " PROJECT_NAME " Firmware...");
  Serial.flush(); // Flush outgoing data
  // Initialize the LED pin as an output
  #if LED_PIN >= 0
    pinMode(LED_PIN, OUTPUT);
  #endif
  

  // Preserve configuration and assets if mounting fails; never auto-format.
  if (!LittleFS.begin(false)) {
    Serial.println("FATAL: LittleFS mount failed. Upload filesystem image. Halting.");
    Serial.flush();
    // If even formatting fails, there is a hardware or configuration problem.
    while (1) {
      #if LED_PIN >= 0
        digitalWrite(LED_PIN, !digitalRead(LED_PIN)); // Blink rapidly to signal a fatal error
      #endif
      delay(100);
    }
  }

  load_device_config();
  sleep(1);

  // --- Display and sensor initialization disabled for debugging ---
  // Initialize displays and load graphical assets
  init_tft();
  
  // Perform an initial clear of both physical screens to ensure a clean state
  clear_all_screens(TFT_BLACK);
  delay(50); // Short delay to ensure screens are cleared
  
  // Show splash screen to user while the rest initializes
  show_splash_screen();
  delay(1000); // Keep splash visible for a moment
  //
  // Initialize the ToF sensor (this part is slow)
  #if USE_TOF_SENSOR
    init_tof_sensor();
  #endif
  // --- End of disabled section ---

  #if USE_AUDIO
    if (!init_audio()) {
      Serial.println("Audio unavailable; continuing without recording/playback.");
    }
  #endif

  init_wifi();
  init_ble_config(get_battery_percentage());

  Serial.println("Initialization complete. Starting main loop.");
  Serial.flush();
}

// Forward declaration for the main application logic
void main_loop();

/**
 * @brief Main application loop.
 */
void loop() {
  // Call the main application logic
  main_loop();
}

void main_loop() {
  // --- FPS Calculation ---
  frame_count++;
  unsigned long current_millis = millis();
  static unsigned long last_battery_time = 0;
  if (current_millis - last_battery_time >= 10000UL) {
    last_battery_time = current_millis;
    update_ble_battery(get_battery_percentage());
  }
  if (current_millis - last_fps_time >= 1000) {
    // Calculate FPS over the last second
    current_fps = frame_count / ((current_millis - last_fps_time) / 1000.0f);
    last_fps_time = current_millis;
    frame_count = 0;
  }

  // --- 1. Sensor Update ---
  #if USE_TOF_SENSOR
    #if TOF_CALIBRATION_MODE
      // In calibration mode, force an update on every frame
      update_tof_sensor_data();
    #else
    update_tof_sensor_data();
    #endif
  #endif
  TofTarget target = get_tof_target();

  // --- 2. Eye Position Logic ---
  // Update the logical positions of the eyes based on the target
  update_eye_positions(target);

  // --- 3. Drawing ---
  for (int i = 0; i < NUM_EYES; i++) {
    select_screen(i);
    clear_buffer(TFT_BLACK);

    if (i == EYE_LEFT && draw_ble_pairing()) continue;
    if (i == EYE_RIGHT && draw_wifi_setup()) continue;

    // Get the final calculated position for the current eye
    EyePosition pos = get_eye_position(i);

    // Draw the eye at its final calculated position
    draw_eye_at_target(pos.x, pos.y, 0); // 0 = eyelid open

    // Optional: Draw the ToF debug grid on one of the screens
    #if USE_TOF_SENSOR && SHOW_TOF_DEBUG_GRID
      if (i == EYE_RIGHT) { // Draw only on the right eye screen
        const int16_t grid_size = 80;
        const int16_t grid_pos = (SCR_WD - grid_size) / 2;
        // Get raw sensor data for display
        const VL53L5CX_ResultsData* tof_data = get_tof_measurement_data();
        // Use the same 'target' that was used for the eye movement
       draw_tof_debug_grid(grid_pos, grid_pos, grid_size, tof_data, target.min_dist_pixel_x, target.min_dist_pixel_y);

        // --- Draw FPS Counter ---
        char fps_str[10];
        dtostrf(current_fps, 4, 1, fps_str); // Format float to string (width 4, 1 decimal)
        char display_str[15];

        // --- Draw Battery Level ---
        int batt_level = get_battery_percentage();

        sprintf(display_str, "FPS:%s B:%d%%", fps_str, batt_level);
        drawString_fb(display_str, 5, 5, TFT_WHITE);
      }
    #endif
  }

  // --- 4. Display Update ---
  // Push the completed framebuffers to the physical screens
  display_all_buffers();
}
