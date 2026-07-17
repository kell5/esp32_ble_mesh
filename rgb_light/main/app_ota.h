#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*app_ota_status_cb_t)(const char *status,
                                    const char *fw_version,
                                    const char *ota_msg_id,
                                    const char *detail);

void app_ota_set_status_callback(app_ota_status_cb_t cb);

// Download, verify and apply the firmware, then reboot. Runs in its own task.
esp_err_t app_ota_start(const char *url,
                        const char *sha256,
                        const char *fw_version,
                        const char *ota_msg_id);

// Called after MQTT is connected so the new image is only accepted after it can
// report itself to the cloud.
esp_err_t app_ota_confirm_running(void);

#ifdef __cplusplus
}
#endif
