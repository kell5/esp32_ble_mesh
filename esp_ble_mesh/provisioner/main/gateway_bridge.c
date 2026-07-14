#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "cJSON.h"
#include "mqtt_client.h"

#include "gateway_bridge.h"

#define TAG "FARMELY_BRIDGE"

static esp_mqtt_client_handle_t s_mqtt;
static farmely_bridge_command_handler_t s_command_handler;
static farmely_bridge_sync_handler_t s_sync_handler;
static void *s_handler_ctx;
static char s_gateway_id[32];
static char s_gateway_topic[96];
static char s_gateway_lwt[384];
static char s_client_id[48];
static uint32_t s_boot_nonce;
static uint32_t s_sequence;
static bool s_mqtt_started;
static bool s_mqtt_connected;

static int64_t unix_timestamp(void)
{
    time_t now = time(NULL);
    return now > 1700000000 ? (int64_t)now : 0;
}

static void message_id(const char *device_id, char *out, size_t size)
{
    uint32_t sequence = __atomic_add_fetch(&s_sequence, 1, __ATOMIC_RELAXED);
    snprintf(out, size, "%s-%08" PRIx32 "-%" PRIu32,
             device_id, s_boot_nonce, sequence);
}

static bool valid_message_id(const char *value)
{
    if (!value || !value[0]) {
        return false;
    }
    for (const char *cursor = value; *cursor; cursor++) {
        if ((*cursor >= 'a' && *cursor <= 'z') ||
            (*cursor >= 'A' && *cursor <= 'Z') ||
            (*cursor >= '0' && *cursor <= '9') ||
            *cursor == '-' || *cursor == '_' || *cursor == '.' ||
            *cursor == ':') {
            continue;
        }
        return false;
    }
    return true;
}

static bool parse_command(const char *payload, bool *on,
                          char *incoming_message_id, size_t message_id_size)
{
    if (!strcasecmp(payload, "on") || !strcmp(payload, "1")) {
        *on = true;
        return true;
    }
    if (!strcasecmp(payload, "off") || !strcmp(payload, "0")) {
        *on = false;
        return true;
    }

    cJSON *root = cJSON_Parse(payload);
    if (!root) {
        return false;
    }

    cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    cJSON *container = cJSON_IsObject(data) ? data : root;
    cJSON *on_value = cJSON_GetObjectItemCaseSensitive(container, "on");
    cJSON *id_value = cJSON_GetObjectItemCaseSensitive(root, "msg_id");
    bool parsed = cJSON_IsBool(on_value);

    if (parsed) {
        *on = cJSON_IsTrue(on_value);
    }
    if (cJSON_IsString(id_value) && valid_message_id(id_value->valuestring)) {
        strlcpy(incoming_message_id, id_value->valuestring, message_id_size);
    }
    cJSON_Delete(root);
    return parsed;
}

static bool topic_device_id(const char *topic, char *device_id, size_t size)
{
    static const char normalized_prefix[] = "farmely/light/";
    static const char normalized_suffix[] = "/down/cmd";
    static const char legacy_prefix[] = "office/light/node/";
    static const char legacy_suffix[] = "/cmd";
    const char *start = NULL;
    size_t length = 0;

    if (!strcmp(topic, "office/light/all/cmd")) {
        strlcpy(device_id, "*", size);
        return true;
    }
    char gateway_topic[96];
    snprintf(gateway_topic, sizeof(gateway_topic),
             "farmely/gateway/%s/down/cmd", s_gateway_id);
    if (!strcmp(topic, gateway_topic)) {
        strlcpy(device_id, "*", size);
        return true;
    }

    if (!strncmp(topic, normalized_prefix, sizeof(normalized_prefix) - 1)) {
        start = topic + sizeof(normalized_prefix) - 1;
        size_t topic_length = strlen(topic);
        size_t suffix_length = sizeof(normalized_suffix) - 1;
        if (topic_length <= (size_t)(start - topic) + suffix_length ||
            strcmp(topic + topic_length - suffix_length, normalized_suffix)) {
            return false;
        }
        length = topic_length - (size_t)(start - topic) - suffix_length;
    } else if (!strncmp(topic, legacy_prefix, sizeof(legacy_prefix) - 1)) {
        start = topic + sizeof(legacy_prefix) - 1;
        size_t topic_length = strlen(topic);
        size_t suffix_length = sizeof(legacy_suffix) - 1;
        if (topic_length <= (size_t)(start - topic) + suffix_length ||
            strcmp(topic + topic_length - suffix_length, legacy_suffix)) {
            return false;
        }
        length = topic_length - (size_t)(start - topic) - suffix_length;
    }

    if (!start || length == 0 || length >= size) {
        return false;
    }
    memcpy(device_id, start, length);
    device_id[length] = '\0';
    return true;
}

static void dispatch_command(const esp_mqtt_event_handle_t event)
{
    char topic[128];
    char payload[512];
    char device_id[32];
    char incoming_message_id[96] = {0};
    bool on;

    if (event->topic_len <= 0 || event->topic_len >= (int)sizeof(topic) ||
        event->data_len <= 0 || event->data_len >= (int)sizeof(payload)) {
        return;
    }

    memcpy(topic, event->topic, event->topic_len);
    topic[event->topic_len] = '\0';
    memcpy(payload, event->data, event->data_len);
    payload[event->data_len] = '\0';

    if (!topic_device_id(topic, device_id, sizeof(device_id)) ||
        !parse_command(payload, &on, incoming_message_id,
                       sizeof(incoming_message_id))) {
        ESP_LOGW(TAG, "Ignoring command topic=%s", topic);
        return;
    }

    if (s_command_handler) {
        esp_err_t err = s_command_handler(device_id, on, incoming_message_id,
                                          s_handler_ctx);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Command rejected for %s: %s",
                     device_id, esp_err_to_name(err));
        }
    }
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)event_base;
    esp_mqtt_event_handle_t event = event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        s_mqtt_connected = true;
        esp_mqtt_client_subscribe(s_mqtt, "farmely/light/+/down/cmd", 1);
        esp_mqtt_client_subscribe(s_mqtt, "farmely/gateway/+/down/cmd", 1);
        esp_mqtt_client_subscribe(s_mqtt, "office/light/all/cmd", 1);
        esp_mqtt_client_subscribe(s_mqtt, "office/light/node/+/cmd", 1);
        if (s_sync_handler) {
            s_sync_handler(s_handler_ctx);
        }
        break;
    case MQTT_EVENT_DATA:
        dispatch_command(event);
        break;
    case MQTT_EVENT_DISCONNECTED:
        s_mqtt_connected = false;
        break;
    default:
        break;
    }
}

static void mqtt_start(void)
{
    if (s_mqtt_started || CONFIG_FARMELY_MQTT_URI[0] == '\0') {
        return;
    }

    snprintf(s_client_id, sizeof(s_client_id), "farmely-%s", s_gateway_id);
    snprintf(s_gateway_topic, sizeof(s_gateway_topic),
             "farmely/gateway/%s/up/status", s_gateway_id);
    snprintf(s_gateway_lwt, sizeof(s_gateway_lwt),
             "{\"v\":1,\"msg_id\":\"%s-lwt\",\"ts\":0,\"type\":\"status\","
             "\"data\":{\"online\":false,\"type\":\"gateway\","
             "\"capabilities\":[\"ble_mesh_bridge\",\"on_off\"]}}",
             s_gateway_id);

    esp_mqtt_client_config_t config = {
        .broker.address.uri = CONFIG_FARMELY_MQTT_URI,
        .credentials.client_id = s_client_id,
        .session.last_will.topic = s_gateway_topic,
        .session.last_will.msg = s_gateway_lwt,
        .session.last_will.qos = 1,
        .session.last_will.retain = 1,
    };
    if (CONFIG_FARMELY_MQTT_USERNAME[0]) {
        config.credentials.username = CONFIG_FARMELY_MQTT_USERNAME;
    }
    if (CONFIG_FARMELY_MQTT_PASSWORD[0]) {
        config.credentials.authentication.password = CONFIG_FARMELY_MQTT_PASSWORD;
    }

    s_mqtt = esp_mqtt_client_init(&config);
    if (!s_mqtt) {
        return;
    }
    esp_mqtt_client_register_event(s_mqtt, ESP_EVENT_ANY_ID,
                                   mqtt_event_handler, NULL);
    if (esp_mqtt_client_start(s_mqtt) == ESP_OK) {
        s_mqtt_started = true;
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT &&
               event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_mqtt_connected = false;
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        mqtt_start();
    }
}

esp_err_t farmely_gateway_bridge_start(const char *gateway_id,
                                       farmely_bridge_command_handler_t command_handler,
                                       farmely_bridge_sync_handler_t sync_handler,
                                       void *ctx)
{
    if (!gateway_id || !gateway_id[0] || !command_handler) {
        return ESP_ERR_INVALID_ARG;
    }
    if (CONFIG_FARMELY_WIFI_SSID[0] == '\0') {
        ESP_LOGW(TAG, "Wi-Fi is not configured; BLE Mesh remains active");
        return ESP_ERR_NOT_SUPPORTED;
    }

    strlcpy(s_gateway_id, gateway_id, sizeof(s_gateway_id));
    s_command_handler = command_handler;
    s_sync_handler = sync_handler;
    s_handler_ctx = ctx;
    s_boot_nonce = esp_random();

    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t wifi_init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               wifi_event_handler, NULL));

    wifi_config_t wifi_config = {0};
    strlcpy((char *)wifi_config.sta.ssid, CONFIG_FARMELY_WIFI_SSID,
            sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, CONFIG_FARMELY_WIFI_PASSWORD,
            sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    return esp_wifi_start();
}

void farmely_gateway_bridge_publish_node_status(const char *device_id,
                                                bool on, bool online)
{
    if (!s_mqtt_connected || !device_id) {
        return;
    }

    char id[64];
    char topic[96];
    char payload[512];
    message_id(device_id, id, sizeof(id));
    snprintf(topic, sizeof(topic), "farmely/light/%s/up/status", device_id);
    snprintf(payload, sizeof(payload),
             "{\"v\":1,\"msg_id\":\"%s\",\"ts\":%" PRId64 ","
             "\"type\":\"status\",\"data\":{\"online\":%s,\"on\":%s,"
             "\"state\":\"%s\",\"type\":\"light_bulb\","
             "\"capabilities\":[\"on_off\"]}}",
             id, unix_timestamp(), online ? "true" : "false",
             on ? "true" : "false", on ? "on" : "off");
    esp_mqtt_client_publish(s_mqtt, topic, payload, 0, 1, 1);

    snprintf(topic, sizeof(topic), "office/light/node/%s/status", device_id);
    snprintf(payload, sizeof(payload),
             "{\"id\":\"%s\",\"online\":%s,\"state\":\"%s\"}",
             device_id, online ? "true" : "false", on ? "on" : "off");
    esp_mqtt_client_publish(s_mqtt, topic, payload, 0, 1, 1);
}

void farmely_gateway_bridge_publish_ack(const char *device_id,
                                        const char *incoming_message_id)
{
    if (!s_mqtt_connected || !device_id || !incoming_message_id ||
        !incoming_message_id[0]) {
        return;
    }

    char id[64];
    char topic[96];
    char payload[384];
    message_id(device_id, id, sizeof(id));
    snprintf(topic, sizeof(topic), "farmely/light/%s/up/event", device_id);
    snprintf(payload, sizeof(payload),
             "{\"v\":1,\"msg_id\":\"%s\",\"ts\":%" PRId64 ","
             "\"type\":\"ack\",\"data\":{\"ack_msg_id\":\"%s\"}}",
             id, unix_timestamp(), incoming_message_id);
    esp_mqtt_client_publish(s_mqtt, topic, payload, 0, 1, 0);
}

void farmely_gateway_bridge_publish_gateway_status(size_t node_count)
{
    if (!s_mqtt_connected) {
        return;
    }

    char id[64];
    char payload[512];
    message_id(s_gateway_id, id, sizeof(id));
    snprintf(payload, sizeof(payload),
             "{\"v\":1,\"msg_id\":\"%s\",\"ts\":%" PRId64 ","
             "\"type\":\"status\",\"data\":{\"online\":true,"
             "\"type\":\"gateway\",\"nodes\":%u,"
             "\"capabilities\":[\"ble_mesh_bridge\",\"on_off\"]}}",
             id, unix_timestamp(), (unsigned)node_count);
    esp_mqtt_client_publish(s_mqtt, s_gateway_topic, payload, 0, 1, 1);
}
