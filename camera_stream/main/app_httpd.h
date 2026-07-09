#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Start the debug HTTP server: "/" info page and "/stream" MJPEG feed on port 81.
esp_err_t app_httpd_start(void);

#ifdef __cplusplus
}
#endif
