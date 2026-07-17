#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Start the MQTT client, subscribe to the light command/OTA topics and begin
// the periodic status heartbeat.
esp_err_t app_mqtt_start(void);

// Publish a full status report (legacy retained topic + unified envelope).
esp_err_t app_mqtt_publish_status(void);

#ifdef __cplusplus
}
#endif
