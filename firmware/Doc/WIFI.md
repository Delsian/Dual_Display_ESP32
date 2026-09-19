# Wi-Fi setup

On first boot, the right display shows an open `DualEye-XXXXXX` setup network.
No password is required to join it. The left eye continues animating. The same instructions are
printed on the USB serial monitor at 115200 baud.

1. Connect your phone to the displayed network; no password is required.
   Stay connected if the phone warns that this network has no Internet.
2. Open the captive portal, or browse to `http://192.168.4.1` manually.
3. Choose **Configure WiFi**, select your home network, enter its password,
   and save. Use a 2.4 GHz network; ESP32-S3 does not support 5 GHz Wi-Fi.
4. After a successful connection the setup network closes and both eyes
   resume. Your phone can reconnect to its usual network. The device prints
   its assigned IP address on serial.

Credentials are stored in ESP32 NVS and reused after power cycling. If the
saved network cannot be reached within 20 seconds at startup, setup opens
again. Setup closes after approximately three minutes, including when a phone
is attached; an in-progress connection attempt can delay closure briefly.
An incorrect password leaves the portal available for another attempt until
timeout. Submitted credentials replace the previously saved credentials.

To change networks or reopen a timed-out portal, send `wifi` followed by Enter
on the serial monitor (enable LF or CRLF line endings). KEY1 retains its audio
recording function. Without serial access, restart the device while its saved
network is unavailable to reopen setup.

After setup closes, the device retries a saved network every 30 seconds while
offline. A later router outage does not automatically reopen setup. Animation
and sensor processing continue independently of the network task.

Timeouts are configurable in `include/config.h`. The WiFiManager dependency is
pinned to 2.0.17 in each enabled PlatformIO environment. Networking runs in a
separate FreeRTOS task because WiFiManager can wait during scans and credential
submission even with its nonblocking portal option enabled.

## Hardware verification

- With no saved credentials, check the displayed open network and manual
  portal address from a phone; verify the left eye continues moving.
- Submit an incorrect password, then correct it before timeout; check the
  reported IP and that the access point closes on success.
- Power cycle and verify reconnection without setup.
- Switch off the router after connection, then restore it; verify reconnection
  while animation continues.
- Send `wifi` to change networks. Leave a phone connected without submitting
  credentials and verify timeout, then reopen with `wifi`.
- Verify audio recording/replay with KEY1 during provisioning.
