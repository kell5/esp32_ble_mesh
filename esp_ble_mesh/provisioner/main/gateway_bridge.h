#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

typedef esp_err_t (*farmely_bridge_command_handler_t)(const char *device_id,
                                                      bool on,
                                                      const char *message_id,
                                                      void *ctx);
typedef void (*farmely_bridge_sync_handler_t)(void *ctx);

/* Implemented by the mesh application. When yield=true the BLE Mesh
 * Provisioner bearers (and with them the mesh scan) are disabled so the
 * SoftAP provisioning window gets the 2.4 GHz radio; when yield=false the
 * Provisioner is re-enabled and node recovery restarts. */
void farmely_mesh_yield_radio(bool yield);

esp_err_t farmely_gateway_bridge_start(const char *gateway_id,
                                       farmely_bridge_command_handler_t command_handler,
                                       farmely_bridge_sync_handler_t sync_handler,
                                       void *ctx);
void farmely_gateway_bridge_publish_node_status(const char *device_id, bool on, bool online);
void farmely_gateway_bridge_publish_ack(const char *device_id, const char *message_id);
void farmely_gateway_bridge_publish_gateway_status(size_t node_count);
