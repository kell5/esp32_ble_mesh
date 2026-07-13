| Supported Targets | ESP32 | ESP32-C3 | ESP32-C6 | ESP32-H2 | ESP32-S3 |
| ----------------- | ----- | -------- | -------- | -------- | -------- |

ESP BLE Mesh Node demo
==========================

## Farmely hardware profile

This copy is the minimum lighting-node baseline used by the Farmely integration:

- ESP32-WROOM drives its only onboard D2 LED on GPIO2.
- The composition contains one element with Configuration Server and Generic OnOff Server.
- BLE Mesh settings and Generic Server support are enabled for restart recovery.
- The ESP32 bench profile disables the brownout detector temporarily; production hardware must restore brownout protection after fixing its power supply.
- The current tested build target is ESP32-WROOM. ESP32-S3 N16R8 uses a GPIO48 WS2812 and requires the dedicated RMT/LED-strip implementation before it is flashed.

This demo shows how BLE Mesh device can be set up as a node with the following features:

- One element
- Two SIG models
	- **Configuration Server model**: The role of this model is mainly to configure Provisioner device’s AppKey and set up its relay function, TTL size, subscription, etc.
   - **OnOff Server model**: This model implements the most basic function of turning the lights on and off.

The default purpose of this demo is to enable the advertising function with 20-ms non-connectable interval in BLE 5.0. You can disable this function through menuconfig: `idf.py menuconfig --> Example Configuration --> This option facilitates sending with 20ms non-connectable interval...`

For a better demonstration effect, an RGB LED can be soldered onto the ESP32-DevKitC board, by connecting their corresponding GPIO pins are GPIO\_NUM\_25, GPIO\_NUM\_26, GPIO\_NUM\_27. Then you need to select the following option in menuconfig:
   `idf.py menuconfig --> Example Configuration --> Board selection for BLE Mesh --> ESP-WROOM-32`

Please check the [tutorial](tutorial/BLE_Mesh_Node_OnOff_Server_Example_Walkthrough.md) for more information about this example.
