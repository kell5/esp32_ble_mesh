#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Configure the doorbell button GPIO interrupt and streaming state machine.
esp_err_t app_doorbell_init(void);

// Begin the doorbell session: notify the app over MQTT and start RTMP push,
// arming the inactivity timeout.
void app_doorbell_start_stream(void);

// End the doorbell session: stop RTMP push and notify the app. `reason` is a
// short tag published for diagnostics (e.g. "timeout", "hangup").
void app_doorbell_stop_stream(const char *reason);

// Handle a command received on doorbell/<id>/cmd (e.g. "hangup", "snapshot").
void app_doorbell_on_command(const char *cmd, int len);

#ifdef __cplusplus
}
#endif
