#include "app_mqtt.h"
#include "app_light.h"
#include "app_ota.h"
#include "app_wifi.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "mqtt_client.h"
#include "esp_app_desc.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "sdkconfig.h"

static const char *TAG = "app_mqtt";

static esp_mqtt_client_handle_t s_client;
static char s_topic_status[96];    /* office/light/node/<id>/status (retained, legacy) */
static char s_topic_cmd[96];       /* office/light/node/<id>/cmd (legacy) */
static char s_topic_up_status[96]; /* farmely/light/<id>/up/status */
static char s_topic_up_ota[96];    /* farmely/light/<id>/up/ota */
static char s_topic_down_cmd[96];  /* farmely/light/<id>/down/cmd */
static char s_topic_down_ota[96];  /* farmely/light/<id>/down/ota */
static char s_lwt_payload[256];
static uint32_t s_boot_nonce;
static uint32_t s_sequence;
static esp_timer_handle_t s_heartbeat_timer;

static const char *device_id(void)
{
    return app_wifi_get_device_id();
}

static void message_id(char *out, size_t max)
{
    uint32_t sequence = __atomic_add_fetch(&s_sequence, 1, __ATOMIC_RELAXED);
    snprintf(out, max, "%s-%08" PRIx32 "-%" PRIu32,
             device_id(), s_boot_nonce, sequence);
}

static void unix_ts_s(int64_t *ts)
{
    time_t now;
    time(&now);
    *ts = (now > 1730000000) ? (int64_t)now : 0;
}

static const char *caps_json(void)
{
#ifdef CONFIG_EXAMPLE_ENABLE_COLOR
    return "[\"on_off\",\"color\",\"system.fw_version\",\"system.online\",\"system.ota\"]";
#else
    return "[\"on_off\",\"system.fw_version\",\"system.online\",\"system.ota\"]";
#endif
}

static void publish_status(void)
{
    char msg_id[64];
    message_id(msg_id, sizeof(msg_id));
    int64_t ts = 0;
    unix_ts_s(&ts);
    bool on = app_light_is_on();
    char color[10];
    app_light_get_color(color, sizeof(color));
    const char *fw = esp_app_get_description()->version;

    /* Legacy retained status used by the App's local discovery */
    char legacy[256];
    snprintf(legacy, sizeof(legacy),
             "{\"id\":\"%s\",\"online\":true,\"state\":\"%s\",\"color\":\"%s\",\"type\":\"light_bulb\"}",
             device_id(), on ? "on" : "off", color);
    esp_mqtt_client_publish(s_client, s_topic_status, legacy, 0, 1, 1);

    /* Unified envelope consumed by the cloud (includes OTA identity) */
    char payload[512];
    snprintf(payload, sizeof(payload),
             "{\"v\":1,\"msg_id\":\"%s\",\"ts\":%" PRId64 ",\"type\":\"status\","
             "\"data\":{\"online\":true,\"on\":%s,\"state\":\"%s\",\"color\":\"%s\","
             "\"type\":\"light_bulb\",\"product_id\":\"%s\",\"hw_version\":\"%s\","
             "\"fw_version\":\"%s\",\"capabilities\":%s}}",
             msg_id, ts, on ? "true" : "false", on ? "on" : "off", color,
             CONFIG_EXAMPLE_PRODUCT_ID, CONFIG_EXAMPLE_HW_VERSION, fw, caps_json());
    esp_mqtt_client_publish(s_client, s_topic_up_status, payload, 0, 1, 1);
}

static void publish_ota_status(const char *status,
                               const char *fw_version,
                               const char *ota_msg_id,
                               const char *detail)
{
    if (s_client == NULL || status == NULL) {
        return;
    }
    char msg_id[64];
    message_id(msg_id, sizeof(msg_id));
    int64_t ts = 0;
    unix_ts_s(&ts);
    const char *fw = fw_version ? fw_version : esp_app_get_description()->version;
    const char *ota_msg = ota_msg_id ? ota_msg_id : "";
    const char *detail_text = detail ? detail : "";
    char payload[512];
    snprintf(payload, sizeof(payload),
             "{\"v\":1,\"msg_id\":\"%s\",\"ts\":%" PRId64 ",\"type\":\"ota\","
             "\"data\":{\"status\":\"%s\",\"fw_version\":\"%s\","
             "\"ota_msg_id\":\"%s\",\"detail\":\"%s\"}}",
             msg_id, ts, status, fw, ota_msg, detail_text);
    esp_mqtt_client_publish(s_client, s_topic_up_ota, payload, 0, 1, 0);
}

static bool parse_hex_color(const char *text, uint8_t *r, uint8_t *g, uint8_t *b)
{
    if (text == NULL) return false;
    if (*text == '#') text++;
    unsigned int rv, gv, bv;
    if (sscanf(text, "%2x%2x%2x", &rv, &gv, &bv) != 3) return false;
    *r = (uint8_t)rv;
    *g = (uint8_t)gv;
    *b = (uint8_t)bv;
    return true;
}

/* Extract a JSON string value ("key":"value") into out. */
static bool json_string(const char *json, const char *key, char *out, size_t max)
{
    char pattern[32];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return false;
    p = strchr(p + strlen(pattern), ':');
    if (!p) return false;
    p++;
    while (*p == ' ') p++;
    if (*p != '"') return false;
    p++;
    const char *e = strchr(p, '"');
    if (!e || (size_t)(e - p) >= max) return false;
    memcpy(out, p, e - p);
    out[e - p] = '\0';
    return true;
}

static void handle_command(const char *payload)
{
    bool changed = false;
    if (strcmp(payload, "on") == 0) {
        app_light_set_on(true);
        changed = true;
    } else if (strcmp(payload, "off") == 0) {
        app_light_set_on(false);
        changed = true;
    } else if (payload[0] == '{') {
        if (strstr(payload, "\"on\":true")) {
            app_light_set_on(true);
            changed = true;
        } else if (strstr(payload, "\"on\":false")) {
            app_light_set_on(false);
            changed = true;
        }
#ifdef CONFIG_EXAMPLE_ENABLE_COLOR
        char color[16];
        uint8_t r, g, b;
        if (json_string(payload, "color", color, sizeof(color)) &&
            parse_hex_color(color, &r, &g, &b)) {
            app_light_set_color(r, g, b);
            changed = true;
        }
#endif
    }
#ifdef CONFIG_EXAMPLE_ENABLE_COLOR
    else if (strncmp(payload, "color:", 6) == 0) {
        uint8_t r, g, b;
        if (parse_hex_color(payload + 6, &r, &g, &b)) {
            app_light_set_color(r, g, b);
            app_light_set_on(true);
            changed = true;
        }
    }
#endif
    if (changed) {
        publish_status();
    } else {
        ESP_LOGW(TAG, "unhandled command: %s", payload);
    }
}

static void handle_ota(const char *payload)
{
    char url[256];
    if (!json_string(payload, "url", url, sizeof(url))) {
        ESP_LOGW(TAG, "OTA command without url");
        return;
    }
    char sha256[65] = {0};
    char fw_version[64] = {0};
    char ota_msg_id[128] = {0};
    json_string(payload, "sha256", sha256, sizeof(sha256));
    json_string(payload, "fw_version", fw_version, sizeof(fw_version));
    json_string(payload, "msg_id", ota_msg_id, sizeof(ota_msg_id));
    esp_err_t err = app_ota_start(url, sha256, fw_version, ota_msg_id);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "failed to start OTA: %s", esp_err_to_name(err));
        publish_ota_status("failed", fw_version, ota_msg_id, esp_err_to_name(err));
    }
}

static void heartbeat_cb(void *arg)
{
    (void)arg;
    if (s_client) {
        publish_status();
    }
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "connected, subscribing command/OTA topics");
        esp_mqtt_client_subscribe(s_client, s_topic_cmd, 1);
        esp_mqtt_client_subscribe(s_client, s_topic_down_cmd, 1);
        esp_mqtt_client_subscribe(s_client, s_topic_down_ota, 1);
        publish_status();
        app_ota_confirm_running();
        if (s_heartbeat_timer) {
            esp_timer_start_periodic(s_heartbeat_timer, 60 * 1000000ULL);
        }
        break;
    case MQTT_EVENT_DATA: {
        int plen = event->data_len;
        char payload[512];
        if (plen <= 0 || plen >= (int)sizeof(payload)) {
            break;
        }
        memcpy(payload, event->data, plen);
        payload[plen] = '\0';
        bool is_ota = (event->topic_len == (int)strlen(s_topic_down_ota) &&
                       strncmp(event->topic, s_topic_down_ota, event->topic_len) == 0);
        if (is_ota) {
            handle_ota(payload);
        } else {
            handle_command(payload);
        }
        break;
    }
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "disconnected");
        if (s_heartbeat_timer) {
            esp_timer_stop(s_heartbeat_timer);
        }
        break;
    default:
        break;
    }
}

esp_err_t app_mqtt_start(void)
{
    const char *devid = device_id();
    snprintf(s_topic_status, sizeof(s_topic_status), "office/light/node/%s/status", devid);
    snprintf(s_topic_cmd, sizeof(s_topic_cmd), "office/light/node/%s/cmd", devid);
    snprintf(s_topic_up_status, sizeof(s_topic_up_status), "farmely/light/%s/up/status", devid);
    snprintf(s_topic_up_ota, sizeof(s_topic_up_ota), "farmely/light/%s/up/ota", devid);
    snprintf(s_topic_down_cmd, sizeof(s_topic_down_cmd), "farmely/light/%s/down/cmd", devid);
    snprintf(s_topic_down_ota, sizeof(s_topic_down_ota), "farmely/light/%s/down/ota", devid);
    if (s_boot_nonce == 0) {
        s_boot_nonce = esp_random();
    }
    snprintf(s_lwt_payload, sizeof(s_lwt_payload),
             "{\"id\":\"%s\",\"online\":false,\"state\":\"off\",\"type\":\"light_bulb\","
             "\"offline_reason\":\"mqtt_lwt\"}",
             devid);
    if (!s_heartbeat_timer) {
        const esp_timer_create_args_t targs = {
            .callback = heartbeat_cb,
            .name = "mqtt_heartbeat"
        };
        esp_err_t timer_err = esp_timer_create(&targs, &s_heartbeat_timer);
        if (timer_err != ESP_OK) {
            return timer_err;
        }
    }
    app_ota_set_status_callback(publish_ota_status);

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = CONFIG_EXAMPLE_MQTT_BROKER_URI,
        .credentials.client_id = device_id(),
        .credentials.username = CONFIG_EXAMPLE_MQTT_USERNAME,
        .credentials.authentication.password = CONFIG_EXAMPLE_MQTT_PASSWORD,
        .session.last_will.topic = s_topic_status,
        .session.last_will.msg = s_lwt_payload,
        .session.last_will.msg_len = 0,
        .session.last_will.qos = 1,
        .session.last_will.retain = 1,
    };

    s_client = esp_mqtt_client_init(&cfg);
    if (!s_client) {
        ESP_LOGE(TAG, "mqtt init failed");
        return ESP_FAIL;
    }
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    return esp_mqtt_client_start(s_client);
}

esp_err_t app_mqtt_publish_status(void)
{
    if (!s_client) return ESP_ERR_INVALID_STATE;
    publish_status();
    return ESP_OK;
}
