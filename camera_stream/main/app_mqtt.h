#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Start the MQTT signalling client and subscribe to the doorbell command topic.
esp_err_t app_mqtt_start(void);

// Publish an event string to doorbell/<id>/event (e.g. "ringing").
esp_err_t app_mqtt_publish_event(const char *event);

#ifdef __cplusplus
}
#endif
