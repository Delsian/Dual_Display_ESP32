#include "device_log.h"
#include <Arduino.h>
#include "serial_commands.h"
#include "speech_test.h"

namespace {
void serial_task(void *) {
  char command[16] = {};
  size_t command_length = 0;
  bool command_overflow = false;
  DeviceLog.println("Send speech [N|off_K] followed by Enter to play a local clip (random if omitted).");

  for (;;) {
    // Bound serial work so a continuous input stream still yields to other tasks.
    for (int n = 0; n < 32 && Serial.available(); ++n) {
      const char c = Serial.read();
      if (c == '\r' || c == '\n') {
        command[command_length] = '\0';
        if (!command_overflow && (strcmp(command, "speech") == 0 ||
                                  strncmp(command, "speech ", 7) == 0)) {
          request_speech_test(command[6] ? command + 7 : "");
        }
        command_length = 0;
        command_overflow = false;
      } else if (command_length < sizeof(command) - 1) {
        command[command_length++] = c;
      } else {
        command_overflow = true;
      }
    }

    vTaskDelay(pdMS_TO_TICKS(20));
  }
}
} // namespace

void init_serial_commands() {
  // Clip decoding must not block the animation task.
  if (xTaskCreate(serial_task, "serial_commands", 8192, nullptr, 1, nullptr) != pdPASS) {
    DeviceLog.println("Serial command task allocation failed.");
  }
}
