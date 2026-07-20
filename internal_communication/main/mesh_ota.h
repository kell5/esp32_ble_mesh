#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*mesh_ota_status_cb_t)(const char *status,
                                     const char *fw_version,
                                     const char *ota_msg_id,
                                     const char *detail);

void mesh_ota_set_status_callback(mesh_ota_status_cb_t cb);

esp_err_t mesh_ota_start(const char *url,
                         const char *sha256,
                         const char *fw_version,
                         const char *ota_msg_id);

esp_err_t mesh_ota_confirm_running(void);

#ifdef __cplusplus
}
#endif
