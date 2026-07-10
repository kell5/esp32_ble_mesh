/* Mesh Internal Communication Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <inttypes.h>
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mesh.h"
#include "esp_mesh_internal.h"
#include "mqtt_client.h"
#include "mesh_light.h"
#include "nvs_flash.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#ifdef CONFIG_MESH_GATEWAY_PROVISIONING
#include "driver/gpio.h"
#include "esp_wifi_default.h"
#include "network_provisioning/manager.h"
#include "network_provisioning/scheme_ble.h"
#endif

/*******************************************************
 *                Macros
 *******************************************************/

/*******************************************************
 *                Constants
 *******************************************************/
#define RX_SIZE          (1500)
#define TX_SIZE          (1460)

/*******************************************************
 *                Variable Definitions
 *******************************************************/
static const char *MESH_TAG = "mesh_main";
static const uint8_t MESH_ID[6] = { 0x77, 0x77, 0x77, 0x77, 0x77, 0x77};
static uint8_t tx_buf[TX_SIZE] = { 0, };
static uint8_t rx_buf[RX_SIZE] = { 0, };
static bool is_running = true;
static bool is_mesh_connected = false;
static mesh_addr_t mesh_parent_addr;
static int mesh_layer = -1;
static esp_netif_t *netif_sta = NULL;

#define MQTT_BROKERADDRESS      "mqtt://121.40.131.194:1883"
#define MQTT_CLIENT_ID          "yskj_g169"
#define MQTT_USER_NAME          "yskj"
#define MQTT_PASSWD             "yskj@123"
/* Legacy global on/off (kept for backward compatibility) */
#define MQTT_SUBSCRIBE_TOPIC    "office/light/demo/cmd"
#define MQTT_PUBLISH_TOPIC      "office/light/demo/status"
/* Product topics: per-node status/command, group (all) command, gateway status */
#define MQTT_TOPIC_ALL_CMD      "office/light/all/cmd"
#define MQTT_TOPIC_NODE_CMD_SUB "office/light/node/+/cmd"
#define MQTT_TOPIC_NODE_PREFIX  "office/light/node/"
#define MQTT_TOPIC_NODE_SUFFIX  "/cmd"
#define MQTT_TOPIC_GATEWAY_STAT "office/light/gateway/status"

static esp_mqtt_client_handle_t s_mqtt_client = NULL;
static bool s_mqtt_started = false;

/*******************************************************
 *   Per-node registry (maintained on the root)
 *******************************************************/
#define MAX_NODES               CONFIG_MESH_ROUTE_TABLE_SIZE
#define NODE_OFFLINE_TIMEOUT_US (30LL * 1000 * 1000)  /* mark offline after 30s silence */

typedef struct {
    bool used;
    bool online;
    uint8_t mac[6];
    mesh_addr_t addr;      /* mesh address used to unicast control to this node */
    uint8_t on;            /* last known light state */
    uint8_t layer;
    uint8_t is_root;
    int64_t last_seen_us;
} node_entry_t;

static node_entry_t s_nodes[MAX_NODES];
static SemaphoreHandle_t s_nodes_mtx = NULL;

/* This device's own identity / state, shared with the status reporter task. */
static uint8_t s_self_mac[6] = {0};
static volatile uint8_t s_light_state = 0;
static mesh_addr_t s_root_addr = {0};
static volatile bool s_have_root_addr = false;

mesh_light_ctl_t light_on = {
    .cmd = MESH_CONTROL_CMD,
    .on = 1,
    .token_id = MESH_TOKEN_ID,
    .token_value = MESH_TOKEN_VALUE,
};

mesh_light_ctl_t light_off = {
    .cmd = MESH_CONTROL_CMD,
    .on = 0,
    .token_id = MESH_TOKEN_ID,
    .token_value = MESH_TOKEN_VALUE,
};

/*******************************************************
 *                Function Declarations
 *******************************************************/

/*******************************************************
 *          Per-node registry + status reporting
 *******************************************************/
static void node_id_str(const uint8_t mac[6], char *out, size_t max)
{
    snprintf(out, max, "node-%02X%02X%02X", mac[3], mac[4], mac[5]);
}

static inline void nodes_lock(void)   { if (s_nodes_mtx) xSemaphoreTake(s_nodes_mtx, portMAX_DELAY); }
static inline void nodes_unlock(void) { if (s_nodes_mtx) xSemaphoreGive(s_nodes_mtx); }

/* Must be called with the registry lock held. Returns index or -1 when full. */
static int node_find_or_add_locked(const uint8_t mac[6])
{
    int free_idx = -1;
    for (int i = 0; i < MAX_NODES; i++) {
        if (s_nodes[i].used && memcmp(s_nodes[i].mac, mac, 6) == 0) {
            return i;
        }
        if (!s_nodes[i].used && free_idx < 0) {
            free_idx = i;
        }
    }
    if (free_idx >= 0) {
        memset(&s_nodes[free_idx], 0, sizeof(s_nodes[free_idx]));
        s_nodes[free_idx].used = true;
        memcpy(s_nodes[free_idx].mac, mac, 6);
    }
    return free_idx;
}

/* Publish one node's status (retained) to office/light/node/<id>/status.
   Reads the entry outside the lock via a caller-provided snapshot. */
static void root_publish_node_status(const node_entry_t *n)
{
    if (!s_mqtt_client || !n) {
        return;
    }
    char id[16];
    node_id_str(n->mac, id, sizeof(id));
    char topic[64];
    snprintf(topic, sizeof(topic), MQTT_TOPIC_NODE_PREFIX "%s/status", id);
    char payload[192];
    snprintf(payload, sizeof(payload),
             "{\"id\":\"%s\",\"online\":%s,\"state\":\"%s\",\"layer\":%d,\"role\":\"%s\"}",
             id, n->online ? "true" : "false", n->on ? "on" : "off",
             n->layer, n->is_root ? "root" : "node");
    esp_mqtt_client_publish(s_mqtt_client, topic, payload, 0, 1, 1);
}

static void root_publish_gateway_status(void)
{
    if (!s_mqtt_client) {
        return;
    }
    int total = 0, online = 0;
    nodes_lock();
    for (int i = 0; i < MAX_NODES; i++) {
        if (s_nodes[i].used) {
            total++;
            if (s_nodes[i].online) {
                online++;
            }
        }
    }
    nodes_unlock();

    char root_id[16];
    node_id_str(s_self_mac, root_id, sizeof(root_id));
    char payload[224];
    snprintf(payload, sizeof(payload),
             "{\"online\":true,\"root\":\"%s\",\"layer\":%d,\"nodes\":%d,\"online_nodes\":%d,\"heap\":%" PRId32 "}",
             root_id, mesh_layer, total, online, esp_get_minimum_free_heap_size());
    esp_mqtt_client_publish(s_mqtt_client, MQTT_TOPIC_GATEWAY_STAT, payload, 0, 1, 1);
}

/* Root: record/refresh a node from an upstream status packet, then publish. */
static void root_handle_node_status(const mesh_addr_t *from, const mesh_light_status_t *st)
{
    if (!from || !st) {
        return;
    }
    node_entry_t snapshot;
    nodes_lock();
    int idx = node_find_or_add_locked(st->mac);
    if (idx < 0) {
        nodes_unlock();
        ESP_LOGW(MESH_TAG, "node registry full, dropping status");
        return;
    }
    s_nodes[idx].addr = *from;
    s_nodes[idx].on = st->on ? 1 : 0;
    s_nodes[idx].layer = st->layer;
    s_nodes[idx].is_root = st->is_root ? 1 : 0;
    s_nodes[idx].online = true;
    s_nodes[idx].last_seen_us = esp_timer_get_time();
    snapshot = s_nodes[idx];
    nodes_unlock();

    root_publish_node_status(&snapshot);
}

/* Root: refresh its own entry (role=root) and publish it. */
static void root_refresh_self(void)
{
    node_entry_t snapshot;
    nodes_lock();
    int idx = node_find_or_add_locked(s_self_mac);
    if (idx >= 0) {
        s_nodes[idx].on = s_light_state;
        s_nodes[idx].layer = mesh_layer;
        s_nodes[idx].is_root = 1;
        s_nodes[idx].online = true;
        s_nodes[idx].last_seen_us = esp_timer_get_time();
        snapshot = s_nodes[idx];
    } else {
        nodes_unlock();
        return;
    }
    nodes_unlock();
    root_publish_node_status(&snapshot);
}

/* Root: mark nodes offline that have been silent too long, and publish once. */
static void root_expire_stale_nodes(void)
{
    int64_t now = esp_timer_get_time();
    for (int i = 0; i < MAX_NODES; i++) {
        node_entry_t snapshot;
        bool changed = false;
        nodes_lock();
        if (s_nodes[i].used && s_nodes[i].online && !s_nodes[i].is_root &&
            (now - s_nodes[i].last_seen_us) > NODE_OFFLINE_TIMEOUT_US) {
            s_nodes[i].online = false;
            snapshot = s_nodes[i];
            changed = true;
        }
        nodes_unlock();
        if (changed) {
            root_publish_node_status(&snapshot);
        }
    }
}

/* Non-root: report this node's light state upstream to the root. */
static void send_status_upstream(void)
{
    if (esp_mesh_is_root() || !is_mesh_connected || !s_have_root_addr) {
        return;
    }
    mesh_light_status_t st = {
        .cmd = MESH_STATUS_CMD,
        .on = s_light_state,
        .layer = (uint8_t)mesh_layer,
        .is_root = 0,
    };
    memcpy(st.mac, s_self_mac, 6);
    mesh_data_t data = {
        .data = (uint8_t *)&st,
        .size = sizeof(st),
        .proto = MESH_PROTO_BIN,
        .tos = MESH_TOS_P2P,
    };
    /* Route the report up to the root as a normal P2P mesh packet. The root
       receives it via esp_mesh_recv(). MESH_DATA_TODS must NOT be used here:
       that flag targets the external distribution system and would only be
       delivered through esp_mesh_recv_toDS(), which the root does not read. */
    esp_err_t err = esp_mesh_send(&s_root_addr, &data, MESH_DATA_P2P, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGW(MESH_TAG, "status upstream send err:0x%x", err);
    }
}

/*******************************************************
 *                Function Definitions
 *******************************************************/
void esp_mesh_p2p_tx_main(void *arg)
{
    is_running = true;

    while (is_running) {
        if (esp_mesh_is_root()) {
            /* Root aggregates: refresh own entry, expire silent nodes, report gateway. */
            root_refresh_self();
            root_expire_stale_nodes();
            root_publish_gateway_status();
        } else {
            /* Non-root: periodic heartbeat of this node's light state to the root. */
            ESP_LOGI(MESH_TAG, "layer:%d, rtableSize:%d, %s", mesh_layer,
                     esp_mesh_get_routing_table_size(),
                     is_mesh_connected ? "NODE" : "DISCONNECT");
            send_status_upstream();
        }
        vTaskDelay(8 * 1000 / portTICK_PERIOD_MS);
    }
    vTaskDelete(NULL);
}

void esp_mesh_p2p_rx_main(void *arg)
{
    int recv_count = 0;
    esp_err_t err;
    mesh_addr_t from;
    int send_count = 0;
    mesh_data_t data;
    int flag = 0;
    data.data = rx_buf;
    data.size = RX_SIZE;
    is_running = true;

    while (is_running) {
        data.size = RX_SIZE;
        err = esp_mesh_recv(&from, &data, portMAX_DELAY, &flag, NULL, 0);
        if (err != ESP_OK || !data.size) {
            ESP_LOGE(MESH_TAG, "err:0x%x, size:%d", err, data.size);
            continue;
        }
        /* extract send count (legacy demo telemetry; only when packet is large enough) */
        if (data.size >= 26) {
            send_count = (data.data[25] << 24) | (data.data[24] << 16)
                         | (data.data[23] << 8) | data.data[22];
        }
        recv_count++;
        /* Dispatch by the leading command byte:
           - MESH_STATUS_CMD (upstream node report): only the root aggregates it.
           - otherwise treat as a light-control message and apply it locally. */
        if (data.size >= 1 && data.data[0] == MESH_STATUS_CMD) {
            if (esp_mesh_is_root() && data.size >= sizeof(mesh_light_status_t)) {
                root_handle_node_status(&from, (mesh_light_status_t *)data.data);
            }
            continue;
        }
        /* process light control */
        mesh_light_process(&from, data.data, data.size);
        if (data.size >= sizeof(mesh_light_ctl_t)) {
            mesh_light_ctl_t *c = (mesh_light_ctl_t *)data.data;
            if (c->cmd == MESH_CONTROL_CMD &&
                c->token_id == MESH_TOKEN_ID && c->token_value == MESH_TOKEN_VALUE) {
                s_light_state = c->on ? 1 : 0;
                /* report the freshly-applied state upstream for a snappy UI update */
                send_status_upstream();
            }
        }
        if (!(recv_count % 1)) {
            ESP_LOGW(MESH_TAG,
                     "[#RX:%d/%d][L:%d] parent:"MACSTR", receive from "MACSTR", size:%d, heap:%" PRId32 ", flag:%d[err:0x%x, proto:%d, tos:%d]",
                     recv_count, send_count, mesh_layer,
                     MAC2STR(mesh_parent_addr.addr), MAC2STR(from.addr),
                     data.size, esp_get_minimum_free_heap_size(), flag, err, data.proto,
                     data.tos);
        }
    }
    vTaskDelete(NULL);
}

static void mesh_apply_local_ctl(const mesh_light_ctl_t *ctl)
{
    if (!ctl) {
        return;
    }

    mesh_addr_t self = {0};
    mesh_light_process(&self, (uint8_t *)ctl, sizeof(*ctl));
    s_light_state = ctl->on ? 1 : 0;
}

/* Root: unicast an on/off control to a single node's mesh address. */
static void mesh_unicast_ctl(const mesh_addr_t *addr, bool on)
{
    if (!addr || !esp_mesh_is_root()) {
        return;
    }
    const mesh_light_ctl_t *ctl = on ? &light_on : &light_off;
    mesh_data_t data = {
        .data = tx_buf,
        .size = sizeof(*ctl),
        .proto = MESH_PROTO_BIN,
        .tos = MESH_TOS_P2P,
    };
    memcpy(tx_buf, ctl, sizeof(*ctl));
    esp_mesh_send(addr, &data, MESH_DATA_P2P, NULL, 0);
}

static void mesh_broadcast_ctl(const mesh_light_ctl_t *ctl)
{
    if (!ctl || !esp_mesh_is_root()) {
        return;
    }

    mesh_addr_t route_table[CONFIG_MESH_ROUTE_TABLE_SIZE];
    int route_table_size = 0;
    mesh_data_t data = {
        .data = tx_buf,
        .size = sizeof(*ctl),
        .proto = MESH_PROTO_BIN,
        .tos = MESH_TOS_P2P,
    };

    memcpy(tx_buf, ctl, sizeof(*ctl));
    esp_mesh_get_routing_table((mesh_addr_t *)&route_table, CONFIG_MESH_ROUTE_TABLE_SIZE * 6, &route_table_size);
    for (int i = 0; i < route_table_size; i++) {
        /* Skip our own entry: the root applies the state locally below. Sending
           the control to ourselves would loop it back into the RX task and race
           the local apply on the (single) LED hardware. */
        if (memcmp(route_table[i].addr, s_self_mac, 6) == 0) {
            continue;
        }
        esp_mesh_send(&route_table[i], &data, MESH_DATA_P2P, NULL, 0);
    }

    mesh_apply_local_ctl(ctl);
}

static void mqtt_publish_status(const char *event_name)
{
    if (!s_mqtt_client) {
        ESP_LOGW(MESH_TAG, "mqtt publish skipped: client is NULL");
        return;
    }

    char payload[256];
    int written = snprintf(payload, sizeof(payload),
                           "{\"event\":\"%s\",\"root\":%d,\"layer\":%d,\"heap\":%" PRId32 "}",
                           event_name ? event_name : "unknown",
                           esp_mesh_is_root() ? 1 : 0, mesh_layer,
                           esp_get_minimum_free_heap_size());
    if (written < 0) {
        ESP_LOGE(MESH_TAG, "mqtt payload snprintf error");
        return;
    }
    if (written >= sizeof(payload)) {
        ESP_LOGW(MESH_TAG, "mqtt payload truncated (%d/%d)", written, (int)sizeof(payload));
    }

    int msg_id = esp_mqtt_client_publish(s_mqtt_client, MQTT_PUBLISH_TOPIC, payload, 0, 1, 1);
    if (msg_id < 0) {
        ESP_LOGE(MESH_TAG, "mqtt publish failed, msg_id=%d", msg_id);
    } else {
        ESP_LOGI(MESH_TAG, "mqtt publish ok, msg_id=%d, event=%s", msg_id, event_name ? event_name : "unknown");
    }
}

/* Parse an "on"/"off" (case-insensitive) payload. Returns true on success. */
static bool parse_on_off(const char *payload, int len, bool *on)
{
    if (len == 2 && (strncasecmp(payload, "on", 2) == 0)) {
        *on = true;
        return true;
    }
    if (len == 3 && (strncasecmp(payload, "off", 3) == 0)) {
        *on = false;
        return true;
    }
    return false;
}

/* Group / legacy on-off: broadcast to the whole mesh and report the event. */
static void mqtt_handle_broadcast(bool on)
{
    mesh_broadcast_ctl(on ? &light_on : &light_off);
    mqtt_publish_status(on ? "cmd_on" : "cmd_off");
    root_refresh_self();
}

/* Addressed on-off for office/light/node/<id>/cmd. */
static void mqtt_handle_node_cmd(const char *id, bool on)
{
    char self_id[16];
    node_id_str(s_self_mac, self_id, sizeof(self_id));
    if (strcmp(id, self_id) == 0) {
        mesh_apply_local_ctl(on ? &light_on : &light_off);
        root_refresh_self();
        return;
    }

    mesh_addr_t target = {0};
    bool found = false;
    nodes_lock();
    for (int i = 0; i < MAX_NODES; i++) {
        if (s_nodes[i].used) {
            char cur[16];
            node_id_str(s_nodes[i].mac, cur, sizeof(cur));
            if (strcmp(cur, id) == 0) {
                target = s_nodes[i].addr;
                s_nodes[i].on = on ? 1 : 0;   /* optimistic; heartbeat will confirm */
                found = true;
                break;
            }
        }
    }
    nodes_unlock();

    if (found) {
        mesh_unicast_ctl(&target, on);
    } else {
        ESP_LOGW(MESH_TAG, "node cmd for unknown id: %s", id);
    }
}

/* Route an inbound MQTT command by topic. Only the root acts on commands. */
static void mqtt_dispatch(const char *topic, int topic_len, const char *payload, int len)
{
    if (!topic || !payload || len <= 0 || !esp_mesh_is_root()) {
        return;
    }
    bool on = false;
    bool have_on = parse_on_off(payload, len, &on);

    size_t all_len = strlen(MQTT_TOPIC_ALL_CMD);
    size_t demo_len = strlen(MQTT_SUBSCRIBE_TOPIC);
    size_t pre_len = strlen(MQTT_TOPIC_NODE_PREFIX);
    size_t suf_len = strlen(MQTT_TOPIC_NODE_SUFFIX);

    if ((topic_len == (int)all_len && strncmp(topic, MQTT_TOPIC_ALL_CMD, all_len) == 0) ||
        (topic_len == (int)demo_len && strncmp(topic, MQTT_SUBSCRIBE_TOPIC, demo_len) == 0)) {
        if (have_on) {
            mqtt_handle_broadcast(on);
        } else {
            ESP_LOGW(MESH_TAG, "unknown group cmd: %.*s", len, payload);
        }
        return;
    }

    /* office/light/node/<id>/cmd */
    if ((size_t)topic_len > pre_len + suf_len &&
        strncmp(topic, MQTT_TOPIC_NODE_PREFIX, pre_len) == 0 &&
        strncmp(topic + topic_len - suf_len, MQTT_TOPIC_NODE_SUFFIX, suf_len) == 0) {
        char id[24];
        int id_len = topic_len - (int)pre_len - (int)suf_len;
        if (id_len > 0 && id_len < (int)sizeof(id)) {
            memcpy(id, topic + pre_len, id_len);
            id[id_len] = '\0';
            if (have_on) {
                mqtt_handle_node_cmd(id, on);
            } else {
                ESP_LOGW(MESH_TAG, "unknown node cmd: %.*s", len, payload);
            }
        }
        return;
    }

    ESP_LOGW(MESH_TAG, "unhandled cmd topic: %.*s", topic_len, topic);
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        esp_mqtt_client_subscribe(s_mqtt_client, MQTT_SUBSCRIBE_TOPIC, 1);
        esp_mqtt_client_subscribe(s_mqtt_client, MQTT_TOPIC_ALL_CMD, 1);
        esp_mqtt_client_subscribe(s_mqtt_client, MQTT_TOPIC_NODE_CMD_SUB, 1);
        mqtt_publish_status("mqtt_connected");
        root_refresh_self();
        root_publish_gateway_status();
        break;
    case MQTT_EVENT_DATA:
        mqtt_dispatch(event->topic, event->topic_len, event->data, event->data_len);
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(MESH_TAG, "mqtt disconnected");
        break;
    default:
        break;
    }
}

static void mqtt_app_start(void)
{
    if (s_mqtt_started || !esp_mesh_is_root()) {
        return;
    }

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = MQTT_BROKERADDRESS,
        .credentials.client_id = MQTT_CLIENT_ID,
        .credentials.username = MQTT_USER_NAME,
        .credentials.authentication.password = MQTT_PASSWD,
    };

    s_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    if (!s_mqtt_client) {
        ESP_LOGE(MESH_TAG, "mqtt init failed");
        return;
    }

    esp_mqtt_client_register_event(s_mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(s_mqtt_client);
    s_mqtt_started = true;
    ESP_LOGI(MESH_TAG, "mqtt client started, waiting for connection...");
}

static void mqtt_app_stop(void)
{
    if (!s_mqtt_started || !s_mqtt_client) {
        return;
    }

    esp_mqtt_client_stop(s_mqtt_client);
    esp_mqtt_client_destroy(s_mqtt_client);
    s_mqtt_client = NULL;
    s_mqtt_started = false;
}

esp_err_t esp_mesh_comm_p2p_start(void)
{
    static bool is_comm_p2p_started = false;
    if (!is_comm_p2p_started) {
        is_comm_p2p_started = true;
        xTaskCreate(esp_mesh_p2p_tx_main, "MPTX", 3072, NULL, 5, NULL);
        xTaskCreate(esp_mesh_p2p_rx_main, "MPRX", 3072, NULL, 5, NULL);
    }
    return ESP_OK;
}

void mesh_event_handler(void *arg, esp_event_base_t event_base,
                        int32_t event_id, void *event_data)
{
    mesh_addr_t id = {0,};
    static uint16_t last_layer = 0;

    switch (event_id) {
    case MESH_EVENT_STARTED: {
        esp_mesh_get_id(&id);
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_MESH_STARTED>ID:"MACSTR"", MAC2STR(id.addr));
        is_mesh_connected = false;
        mesh_layer = esp_mesh_get_layer();
    }
    break;
    case MESH_EVENT_STOPPED: {
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_STOPPED>");
        is_mesh_connected = false;
        mesh_layer = esp_mesh_get_layer();
        mqtt_app_stop();
    }
    break;
    case MESH_EVENT_CHILD_CONNECTED: {
        mesh_event_child_connected_t *child_connected = (mesh_event_child_connected_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_CHILD_CONNECTED>aid:%d, "MACSTR"",
                 child_connected->aid,
                 MAC2STR(child_connected->mac));
    }
    break;
    case MESH_EVENT_CHILD_DISCONNECTED: {
        mesh_event_child_disconnected_t *child_disconnected = (mesh_event_child_disconnected_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_CHILD_DISCONNECTED>aid:%d, "MACSTR"",
                 child_disconnected->aid,
                 MAC2STR(child_disconnected->mac));
    }
    break;
    case MESH_EVENT_ROUTING_TABLE_ADD: {
        mesh_event_routing_table_change_t *routing_table = (mesh_event_routing_table_change_t *)event_data;
        ESP_LOGW(MESH_TAG, "<MESH_EVENT_ROUTING_TABLE_ADD>add %d, new:%d, layer:%d",
                 routing_table->rt_size_change,
                 routing_table->rt_size_new, mesh_layer);
    }
    break;
    case MESH_EVENT_ROUTING_TABLE_REMOVE: {
        mesh_event_routing_table_change_t *routing_table = (mesh_event_routing_table_change_t *)event_data;
        ESP_LOGW(MESH_TAG, "<MESH_EVENT_ROUTING_TABLE_REMOVE>remove %d, new:%d, layer:%d",
                 routing_table->rt_size_change,
                 routing_table->rt_size_new, mesh_layer);
    }
    break;
    case MESH_EVENT_NO_PARENT_FOUND: {
        mesh_event_no_parent_found_t *no_parent = (mesh_event_no_parent_found_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_NO_PARENT_FOUND>scan times:%d",
                 no_parent->scan_times);
    }
    /* TODO handler for the failure */
    break;
    case MESH_EVENT_PARENT_CONNECTED: {
        mesh_event_connected_t *connected = (mesh_event_connected_t *)event_data;
        esp_mesh_get_id(&id);
        mesh_layer = connected->self_layer;
        memcpy(&mesh_parent_addr.addr, connected->connected.bssid, 6);
        ESP_LOGI(MESH_TAG,
                 "<MESH_EVENT_PARENT_CONNECTED>layer:%d-->%d, parent:"MACSTR"%s, ID:"MACSTR", duty:%d",
                 last_layer, mesh_layer, MAC2STR(mesh_parent_addr.addr),
                 esp_mesh_is_root() ? "<ROOT>" :
                 (mesh_layer == 2) ? "<layer2>" : "", MAC2STR(id.addr), connected->duty);
        last_layer = mesh_layer;
        mesh_connected_indicator(mesh_layer);
        is_mesh_connected = true;
        if (esp_mesh_is_root()) {
            esp_netif_dhcpc_stop(netif_sta);
            esp_netif_dhcpc_start(netif_sta);
        }
        esp_mesh_comm_p2p_start();
    }
    break;
    case MESH_EVENT_PARENT_DISCONNECTED: {
        mesh_event_disconnected_t *disconnected = (mesh_event_disconnected_t *)event_data;
        ESP_LOGI(MESH_TAG,
                 "<MESH_EVENT_PARENT_DISCONNECTED>reason:%d",
                 disconnected->reason);
        is_mesh_connected = false;
        mesh_disconnected_indicator();
        mesh_layer = esp_mesh_get_layer();
        if (esp_mesh_is_root()) {
            mqtt_app_stop();
        }
    }
    break;
    case MESH_EVENT_LAYER_CHANGE: {
        mesh_event_layer_change_t *layer_change = (mesh_event_layer_change_t *)event_data;
        mesh_layer = layer_change->new_layer;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_LAYER_CHANGE>layer:%d-->%d%s",
                 last_layer, mesh_layer,
                 esp_mesh_is_root() ? "<ROOT>" :
                 (mesh_layer == 2) ? "<layer2>" : "");
        last_layer = mesh_layer;
        mesh_connected_indicator(mesh_layer);
    }
    break;
    case MESH_EVENT_ROOT_ADDRESS: {
        mesh_event_root_address_t *root_addr = (mesh_event_root_address_t *)event_data;
        memcpy(s_root_addr.addr, root_addr->addr, 6);
        s_have_root_addr = true;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_ROOT_ADDRESS>root address:"MACSTR"",
                 MAC2STR(root_addr->addr));
    }
    break;
    case MESH_EVENT_VOTE_STARTED: {
        mesh_event_vote_started_t *vote_started = (mesh_event_vote_started_t *)event_data;
        ESP_LOGI(MESH_TAG,
                 "<MESH_EVENT_VOTE_STARTED>attempts:%d, reason:%d, rc_addr:"MACSTR"",
                 vote_started->attempts,
                 vote_started->reason,
                 MAC2STR(vote_started->rc_addr.addr));
    }
    break;
    case MESH_EVENT_VOTE_STOPPED: {
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_VOTE_STOPPED>");
        break;
    }
    case MESH_EVENT_ROOT_SWITCH_REQ: {
        mesh_event_root_switch_req_t *switch_req = (mesh_event_root_switch_req_t *)event_data;
        ESP_LOGI(MESH_TAG,
                 "<MESH_EVENT_ROOT_SWITCH_REQ>reason:%d, rc_addr:"MACSTR"",
                 switch_req->reason,
                 MAC2STR( switch_req->rc_addr.addr));
    }
    break;
    case MESH_EVENT_ROOT_SWITCH_ACK: {
        /* new root */
        mesh_layer = esp_mesh_get_layer();
        esp_mesh_get_parent_bssid(&mesh_parent_addr);
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_ROOT_SWITCH_ACK>layer:%d, parent:"MACSTR"", mesh_layer, MAC2STR(mesh_parent_addr.addr));
    }
    break;
    case MESH_EVENT_TODS_STATE: {
        mesh_event_toDS_state_t *toDs_state = (mesh_event_toDS_state_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_TODS_REACHABLE>state:%d", *toDs_state);
    }
    break;
    case MESH_EVENT_ROOT_FIXED: {
        mesh_event_root_fixed_t *root_fixed = (mesh_event_root_fixed_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_ROOT_FIXED>%s",
                 root_fixed->is_fixed ? "fixed" : "not fixed");
    }
    break;
    case MESH_EVENT_ROOT_ASKED_YIELD: {
        mesh_event_root_conflict_t *root_conflict = (mesh_event_root_conflict_t *)event_data;
        ESP_LOGI(MESH_TAG,
                 "<MESH_EVENT_ROOT_ASKED_YIELD>"MACSTR", rssi:%d, capacity:%d",
                 MAC2STR(root_conflict->addr),
                 root_conflict->rssi,
                 root_conflict->capacity);
    }
    break;
    case MESH_EVENT_CHANNEL_SWITCH: {
        mesh_event_channel_switch_t *channel_switch = (mesh_event_channel_switch_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_CHANNEL_SWITCH>new channel:%d", channel_switch->channel);
    }
    break;
    case MESH_EVENT_SCAN_DONE: {
        mesh_event_scan_done_t *scan_done = (mesh_event_scan_done_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_SCAN_DONE>number:%d",
                 scan_done->number);
    }
    break;
    case MESH_EVENT_NETWORK_STATE: {
        mesh_event_network_state_t *network_state = (mesh_event_network_state_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_NETWORK_STATE>is_rootless:%d",
                 network_state->is_rootless);
    }
    break;
    case MESH_EVENT_STOP_RECONNECTION: {
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_STOP_RECONNECTION>");
    }
    break;
    case MESH_EVENT_FIND_NETWORK: {
        mesh_event_find_network_t *find_network = (mesh_event_find_network_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_FIND_NETWORK>new channel:%d, router BSSID:"MACSTR"",
                 find_network->channel, MAC2STR(find_network->router_bssid));
    }
    break;
    case MESH_EVENT_ROUTER_SWITCH: {
        mesh_event_router_switch_t *router_switch = (mesh_event_router_switch_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_ROUTER_SWITCH>new router:%s, channel:%d, "MACSTR"",
                 router_switch->ssid, router_switch->channel, MAC2STR(router_switch->bssid));
    }
    break;
    case MESH_EVENT_PS_PARENT_DUTY: {
        mesh_event_ps_duty_t *ps_duty = (mesh_event_ps_duty_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_PS_PARENT_DUTY>duty:%d", ps_duty->duty);
    }
    break;
    case MESH_EVENT_PS_CHILD_DUTY: {
        mesh_event_ps_duty_t *ps_duty = (mesh_event_ps_duty_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_PS_CHILD_DUTY>cidx:%d, "MACSTR", duty:%d", ps_duty->child_connected.aid-1,
                MAC2STR(ps_duty->child_connected.mac), ps_duty->duty);
    }
    break;
    default:
        ESP_LOGI(MESH_TAG, "unknown id:%" PRId32 "", event_id);
        break;
    }
}

void ip_event_handler(void *arg, esp_event_base_t event_base,
                      int32_t event_id, void *event_data)
{
    ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
    ESP_LOGI(MESH_TAG, "<IP_EVENT_STA_GOT_IP>IP:" IPSTR, IP2STR(&event->ip_info.ip));
    if (esp_mesh_is_root()) {
        mqtt_app_start();
    }

}

#ifdef CONFIG_MESH_GATEWAY_PROVISIONING
static void gateway_build_service_name(char *out, size_t size)
{
    uint8_t mac[6] = {0};
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_STA));
    snprintf(out, size, "%s%02X%02X%02X",
             CONFIG_MESH_PROV_NAME_PREFIX, mac[3], mac[4], mac[5]);
}

static void gateway_load_router_config(wifi_config_t *router_cfg)
{
    esp_netif_t *prov_netif = esp_netif_create_default_wifi_sta();
    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_cfg));

    network_prov_mgr_config_t prov_cfg = {
        .scheme = network_prov_scheme_ble,
        .scheme_event_handler = NETWORK_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM,
    };
    ESP_ERROR_CHECK(network_prov_mgr_init(prov_cfg));

    bool provisioned = false;
    ESP_ERROR_CHECK(network_prov_mgr_is_wifi_provisioned(&provisioned));
    if (!provisioned) {
        char service_name[32];
        gateway_build_service_name(service_name, sizeof(service_name));
        uint8_t service_uuid[] = {
            0xb4, 0xdf, 0x5a, 0x1c, 0x3f, 0x6b, 0xf4, 0xbf,
            0xea, 0x4a, 0x82, 0x03, 0x04, 0x90, 0x1a, 0x02,
        };
        ESP_ERROR_CHECK(network_prov_scheme_ble_set_service_uuid(service_uuid));
        ESP_LOGI(MESH_TAG, "gateway not provisioned -> BLE advertising as %s", service_name);
        ESP_ERROR_CHECK(network_prov_mgr_start_provisioning(
            NETWORK_PROV_SECURITY_1, CONFIG_MESH_PROV_POP, service_name, NULL));
        network_prov_mgr_wait();
    }

    ESP_ERROR_CHECK(esp_wifi_get_config(WIFI_IF_STA, router_cfg));
    ESP_LOGI(MESH_TAG, "using provisioned router SSID: %s", router_cfg->sta.ssid);
    ESP_ERROR_CHECK(network_prov_mgr_deinit());
    esp_wifi_stop();
    ESP_ERROR_CHECK(esp_wifi_deinit());
    esp_netif_destroy_default_wifi(prov_netif);
}

static void gateway_provisioning_reset_task(void *arg)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << CONFIG_MESH_PROV_RESET_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
    int64_t pressed_at = 0;
    while (true) {
        if (gpio_get_level(CONFIG_MESH_PROV_RESET_GPIO) == 0) {
            if (pressed_at == 0) pressed_at = esp_timer_get_time();
            if (esp_timer_get_time() - pressed_at >= 3000000) {
                ESP_LOGW(MESH_TAG, "resetting gateway WiFi provisioning");
                ESP_ERROR_CHECK(esp_wifi_restore());
                vTaskDelay(pdMS_TO_TICKS(300));
                esp_restart();
            }
        } else {
            pressed_at = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
#endif

void app_main(void)
{
    ESP_ERROR_CHECK(mesh_light_init());
    ESP_ERROR_CHECK(nvs_flash_init());
    /*  tcpip initialization */
    ESP_ERROR_CHECK(esp_netif_init());
    /*  event initialization */
    ESP_ERROR_CHECK(esp_event_loop_create_default());
#ifdef CONFIG_MESH_GATEWAY_PROVISIONING
    wifi_config_t gateway_router_cfg = {0};
    gateway_load_router_config(&gateway_router_cfg);
#endif
    /*  create network interfaces for mesh (only station instance saved for further manipulation, soft AP instance ignored */
    ESP_ERROR_CHECK(esp_netif_create_default_wifi_mesh_netifs(&netif_sta, NULL));
    /*  wifi initialization */
    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&config));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &ip_event_handler, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));
    ESP_ERROR_CHECK(esp_wifi_start());
    /* Node identity + registry lock for per-node status/commands. */
    s_nodes_mtx = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(esp_wifi_get_mac(WIFI_IF_STA, s_self_mac));
    ESP_LOGI(MESH_TAG, "node id: node-%02X%02X%02X",
             s_self_mac[3], s_self_mac[4], s_self_mac[5]);
    /*  mesh initialization */
    ESP_ERROR_CHECK(esp_mesh_init());
    ESP_ERROR_CHECK(esp_event_handler_register(MESH_EVENT, ESP_EVENT_ANY_ID, &mesh_event_handler, NULL));
    /*  set mesh topology */
    ESP_ERROR_CHECK(esp_mesh_set_topology(CONFIG_MESH_TOPOLOGY));
    /*  set mesh max layer according to the topology */
    ESP_ERROR_CHECK(esp_mesh_set_max_layer(CONFIG_MESH_MAX_LAYER));
    ESP_ERROR_CHECK(esp_mesh_set_vote_percentage(1));
    ESP_ERROR_CHECK(esp_mesh_set_xon_qsize(128));
#ifdef CONFIG_MESH_ENABLE_PS
    /* Enable mesh PS function */
    ESP_ERROR_CHECK(esp_mesh_enable_ps());
    /* better to increase the associate expired time, if a small duty cycle is set. */
    ESP_ERROR_CHECK(esp_mesh_set_ap_assoc_expire(60));
    /* better to increase the announce interval to avoid too much management traffic, if a small duty cycle is set. */
    ESP_ERROR_CHECK(esp_mesh_set_announce_interval(600, 3300));
#else
    /* Disable mesh PS function */
    ESP_ERROR_CHECK(esp_mesh_disable_ps());
    ESP_ERROR_CHECK(esp_mesh_set_ap_assoc_expire(10));
#endif
    mesh_cfg_t cfg = MESH_INIT_CONFIG_DEFAULT();
    /* mesh ID */
    memcpy((uint8_t *) &cfg.mesh_id, MESH_ID, 6);
    /* router */
#ifdef CONFIG_MESH_GATEWAY_PROVISIONING
    cfg.channel = 0;
    cfg.router.ssid_len = strnlen((const char *)gateway_router_cfg.sta.ssid,
                                  sizeof(gateway_router_cfg.sta.ssid));
    memcpy((uint8_t *)&cfg.router.ssid, gateway_router_cfg.sta.ssid,
           cfg.router.ssid_len);
    memcpy((uint8_t *)&cfg.router.password, gateway_router_cfg.sta.password,
           strnlen((const char *)gateway_router_cfg.sta.password,
                   sizeof(gateway_router_cfg.sta.password)));
#else
    cfg.channel = CONFIG_MESH_CHANNEL;
    cfg.router.ssid_len = strlen(CONFIG_MESH_ROUTER_SSID);
    memcpy((uint8_t *) &cfg.router.ssid, CONFIG_MESH_ROUTER_SSID, cfg.router.ssid_len);
    memcpy((uint8_t *) &cfg.router.password, CONFIG_MESH_ROUTER_PASSWD,
           strlen(CONFIG_MESH_ROUTER_PASSWD));
#endif
    /* mesh softAP */
    ESP_ERROR_CHECK(esp_mesh_set_ap_authmode(CONFIG_MESH_AP_AUTHMODE));
    cfg.mesh_ap.max_connection = CONFIG_MESH_AP_CONNECTIONS;
    cfg.mesh_ap.nonmesh_max_connection = CONFIG_MESH_NON_MESH_AP_CONNECTIONS;
    memcpy((uint8_t *) &cfg.mesh_ap.password, CONFIG_MESH_AP_PASSWD,
           strlen(CONFIG_MESH_AP_PASSWD));
    ESP_ERROR_CHECK(esp_mesh_set_config(&cfg));
    /* mesh start */
    ESP_ERROR_CHECK(esp_mesh_start());
#ifdef CONFIG_MESH_ENABLE_PS
    /* set the device active duty cycle. (default:10, MESH_PS_DEVICE_DUTY_REQUEST) */
    ESP_ERROR_CHECK(esp_mesh_set_active_duty_cycle(CONFIG_MESH_PS_DEV_DUTY, CONFIG_MESH_PS_DEV_DUTY_TYPE));
    /* set the network active duty cycle. (default:10, -1, MESH_PS_NETWORK_DUTY_APPLIED_ENTIRE) */
    ESP_ERROR_CHECK(esp_mesh_set_network_duty_cycle(CONFIG_MESH_PS_NWK_DUTY, CONFIG_MESH_PS_NWK_DUTY_DURATION, CONFIG_MESH_PS_NWK_DUTY_RULE));
#endif
#ifdef CONFIG_MESH_GATEWAY_PROVISIONING
    xTaskCreate(gateway_provisioning_reset_task, "gateway_prov_reset", 2048, NULL, 3, NULL);
#endif
    ESP_LOGI(MESH_TAG, "mesh starts successfully, heap:%" PRId32 ", %s<%d>%s, ps:%d",  esp_get_minimum_free_heap_size(),
             esp_mesh_is_root_fixed() ? "root fixed" : "root not fixed",
             esp_mesh_get_topology(), esp_mesh_get_topology() ? "(chain)":"(tree)", esp_mesh_is_ps_enabled());
}
