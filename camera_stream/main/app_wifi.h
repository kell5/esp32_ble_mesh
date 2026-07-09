#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Bring up Wi-Fi in station mode and block until an IP is obtained.
esp_err_t app_wifi_connect(void);

// Forget stored WiFi credentials and reboot into SoftAP provisioning.
// Called at runtime (doorbell long-press or MQTT "reprovision" command).
void app_wifi_reset_provisioning(void);

#ifdef __cplusplus
}
#endif
