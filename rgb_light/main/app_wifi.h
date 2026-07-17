#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Bring up Wi-Fi in station mode and block until an IP is obtained.
esp_err_t app_wifi_connect(void);

// Device id "rgb-XXXXXX" derived from the last 3 MAC bytes.
const char *app_wifi_get_device_id(void);

// Forget stored WiFi credentials and reboot into BLE provisioning.
void app_wifi_reset_provisioning(void);

#ifdef __cplusplus
}
#endif
