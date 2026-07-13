#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Bring up Wi-Fi in station mode and block until an IP is obtained.
esp_err_t app_wifi_connect(void);

// Return the Kconfig device ID. Production builds must assign it uniquely.
const char *app_wifi_get_device_id(void);

// Forget stored WiFi credentials and reboot into SoftAP provisioning.
// Called at runtime (doorbell long-press or MQTT "reprovision" command).
void app_wifi_reset_provisioning(void);

#ifdef __cplusplus
}
#endif
