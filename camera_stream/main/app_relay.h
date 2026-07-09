#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Start pushing camera JPEG frames to the configured MJPEG relay server over a
// long-lived HTTP POST (multipart/x-mixed-replace). The relay fans the frames
// out to viewers as a standard MJPEG stream. Safe to call when already
// streaming (no-op).
esp_err_t app_relay_start(void);

// Stop the relay push task if running.
void app_relay_stop(void);

bool app_relay_is_streaming(void);

#ifdef __cplusplus
}
#endif
