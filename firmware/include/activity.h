#pragma once

// Restart the five-minute offline active window at boot and on spoken replies.
void restart_active_window();
bool device_active();
void set_device_sleeping(bool sleeping);
