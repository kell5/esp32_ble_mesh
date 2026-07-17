#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Download and apply the firmware at `url`, then reboot. Runs in its own task.
esp_err_t app_ota_start(const char *url);

#ifdef __cplusplus
}
#endif
