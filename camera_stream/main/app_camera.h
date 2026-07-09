#pragma once

#include "esp_err.h"
#include "esp_camera.h"

// Streaming frame geometry, kept in one place so the HTTP/RTMP paths and the
// camera initialisation agree on resolution and frame rate.
#define APP_CAM_FRAME_SIZE FRAMESIZE_VGA
#define APP_CAM_WIDTH      640
#define APP_CAM_HEIGHT     480
#define APP_CAM_FPS        15

#ifdef __cplusplus
extern "C" {
#endif

// Initialise the OV3660 sensor in JPEG mode. Must be called after PSRAM is up.
esp_err_t app_camera_init(void);

#ifdef __cplusplus
}
#endif
