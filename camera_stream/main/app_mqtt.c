#include "app_mqtt.h"
#include "app_doorbell.h"
#include "app_ota.h"
#include "app_wifi.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "mqtt_client.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "sdkconfig.h"

static const char *TAG = "app_mqtt";

static esp_mqtt_client_handle_t s_client;
static char s_topic_event[96];
static char s_topic_cmd[96];
static char s_topic_status[96];
static char s_topic_up_status[96];
static char s_topic_up_ota[96];
static char s_topic_up_event[96];
static char s_topic_down_cmd[96];
static char s_topic_down_ota[96];
static char s_lwt_payload[512];
static uint32_t s_boot_nonce;
static uint32_t s_sequence;
static esp_timer_handle_t s_heartbeat_timer;

static const char *s_capabilities[] = {
    "doorbell.button",
    "camera.stream",
    "system.fw_version",
    "system.online",
    "system.ota",
    NULL
};

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

static void caps_json(char *out, size_t max)
{
    size_t pos = 0;
    pos += snprintf(out + pos, max - pos, "[");
    for (int i = 0; s_capabilities[i] != NULL; i++) {
        if (i > 0) pos += snprintf(out + pos, max - pos, ",");
        pos += snprintf(out + pos, max - pos, "\"%s\"", s_capabilities[i]);
    }
    snprintf(out + pos, max - pos, "]");
}

static void publish_online_status(void)
{
    char msg_id[64];
    message_id(msg_id, sizeof(msg_id));
    int64_t ts = 0;
    unix_ts_s(&ts);
    char caps[128];
    caps_json(caps, sizeof(caps));
    const char *fw = esp_app_get_description()->version;
    /* Old format (backward compatible) */
    char old_payload[384];
    snprintf(old_payload, sizeof(old_payload),
             "{\"version\":1,\"message_id\":\"%s\",\"id\":\"%s\",\"online\":true,\"type\":\"doorbell\",\"product_id\":\"%s\",\"hw_version\":\"%s\",\"fw_version\":\"%s\",\"capabilities\":%s}",
             msg_id, device_id(), CONFIG_EXAMPLE_PRODUCT_ID, CONFIG_EXAMPLE_HW_VERSION, fw, caps);
    esp_mqtt_client_publish(s_client, s_topic_status, old_payload, 0, 1, 1);
    /* New format: unified envelope */
    char new_payload[512];
    snprintf(new_payload, sizeof(new_payload),
             "{\"v\":1,\"msg_id\":\"%s\",\"ts\":%" PRId64 ",\"type\":\"status\",\"data\":{\"online\":true,\"type\":\"doorbell\",\"product_id\":\"%s\",\"hw_version\":\"%s\",\"fw_version\":\"%s\",\"capabilities\":%s}}",
             msg_id, ts, CONFIG_EXAMPLE_PRODUCT_ID, CONFIG_EXAMPLE_HW_VERSION, fw, caps);
    esp_mqtt_client_publish(s_client, s_topic_up_status, new_payload, 0, 1, 1);
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

static void publish_ack(const char *original_msg_id)
{
    char msg_id[64];
    message_id(msg_id, sizeof(msg_id));
    int64_t ts = 0;
    unix_ts_s(&ts);
    char payload[384];
    snprintf(payload, sizeof(payload),
             "{\"v\":1,\"msg_id\":\"%s\",\"ts\":%" PRId64 ",\"type\":\"ack\",\"data\":{\"ack_msg_id\":\"%s\"}}",
             msg_id, ts, original_msg_id ? original_msg_id : "");
    esp_mqtt_client_publish(s_client, s_topic_up_event, payload, 0, 1, 0);
    char old_payload[256];
    snprintf(old_payload, sizeof(old_payload),
             "{\"version\":1,\"message_id\":\"%s\",\"id\":\"%s\",\"event\":\"ack\",\"ack_msg_id\":\"%s\"}",
             msg_id, device_id(), original_msg_id ? original_msg_id : "");
    esp_mqtt_client_publish(s_client, s_topic_event, old_payload, 0, 1, 0);
}

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
        publish_online_status();
        ESP_LOGD(TAG, "heartbeat");
    }
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "connected, subscribing %s", s_topic_cmd);
        esp_mqtt_client_subscribe(s_client, s_topic_cmd, 1);
        esp_mqtt_client_subscribe(s_client, s_topic_down_cmd, 1);
        esp_mqtt_client_subscribe(s_client, s_topic_down_ota, 1);
        publish_online_status();
        app_ota_confirm_running();
        if (s_heartbeat_timer) {
            esp_timer_start_periodic(s_heartbeat_timer, 60 * 1000000ULL);
        }
        break;
    case MQTT_EVENT_DATA: {
        bool on_cmd = (event->topic_len == (int)strlen(s_topic_cmd) &&
                       strncmp(event->topic, s_topic_cmd, event->topic_len) == 0);
        bool on_new = (event->topic_len == (int)strlen(s_topic_down_cmd) &&
                       strncmp(event->topic, s_topic_down_cmd, event->topic_len) == 0);
        bool is_ota = (event->topic_len == (int)strlen(s_topic_down_ota) &&
                       strncmp(event->topic, s_topic_down_ota, event->topic_len) == 0);
        if (is_ota) {
            int plen = event->data_len;
            char payload[512];
            if (plen <= 0 || plen >= (int)sizeof(payload)) {
                ESP_LOGW(TAG, "ignoring invalid OTA payload length=%d", plen);
                break;
            }
            memcpy(payload, event->data, plen);
            payload[plen] = '\0';
            handle_ota(payload);
        } else if (on_cmd || on_new) {
            int plen = event->data_len;
            char payload[512];
            if (plen <= 0 || plen >= (int)sizeof(payload)) {
                ESP_LOGW(TAG, "ignoring invalid command payload length=%d", plen);
                break;
            }
            memcpy(payload, event->data, plen);
            payload[plen] = '\0';
            const char *pl = payload;
            char mid_buf[128] = {0};
            const char *cmd_str = NULL;
            int cmd_len = 0;
            if (plen > 0 && pl[0] == '{') {
                const char *p = strstr(pl, "\"msg_id\"");
                if (p) {
                    p = strchr(p + 8, '"');
                    if (p) {
                        p++;
                        const char *e = strchr(p, '"');
                        if (e && (size_t)(e - p) < sizeof(mid_buf) - 1) {
                            memcpy(mid_buf, p, e - p);
                        }
                    }
                }
                p = strstr(pl, "\"command\"");
                if (p) {
                    p = strchr(p + 9, '"');
                    if (p) {
                        p++;
                        while (p[cmd_len] && p[cmd_len] != '"') cmd_len++;
                        cmd_str = p;
                    }
                }
            }
            if (!cmd_str) {
                cmd_str = pl;
                cmd_len = plen;
            }
            app_doorbell_on_command(cmd_str, cmd_len);
            if (mid_buf[0]) {
                publish_ack(mid_buf);
            }
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
    snprintf(s_topic_event, sizeof(s_topic_event), "doorbell/%s/event", devid);
    snprintf(s_topic_cmd, sizeof(s_topic_cmd), "doorbell/%s/cmd", devid);
    snprintf(s_topic_status, sizeof(s_topic_status), "doorbell/%s/status", devid);
    snprintf(s_topic_up_status, sizeof(s_topic_up_status), "farmely/doorbell/%s/up/status", devid);
    snprintf(s_topic_up_ota, sizeof(s_topic_up_ota), "farmely/doorbell/%s/up/ota", devid);
    snprintf(s_topic_up_event, sizeof(s_topic_up_event), "farmely/doorbell/%s/up/event", devid);
    snprintf(s_topic_down_cmd, sizeof(s_topic_down_cmd), "farmely/doorbell/%s/down/cmd", devid);
    snprintf(s_topic_down_ota, sizeof(s_topic_down_ota), "farmely/doorbell/%s/down/ota", devid);
    if (s_boot_nonce == 0) {
        s_boot_nonce = esp_random();
    }
    char msg_id[64];
    message_id(msg_id, sizeof(msg_id));
    char caps[128];
    caps_json(caps, sizeof(caps));
    snprintf(s_lwt_payload, sizeof(s_lwt_payload),
             "{\"version\":1,\"message_id\":\"%s\",\"id\":\"%s\",\"online\":false,\"type\":\"doorbell\",\"product_id\":\"%s\",\"hw_version\":\"%s\",\"fw_version\":\"%s\",\"offline_reason\":\"mqtt_lwt\",\"capabilities\":%s}",
             msg_id, devid, CONFIG_EXAMPLE_PRODUCT_ID, CONFIG_EXAMPLE_HW_VERSION, esp_app_get_description()->version, caps);
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
        .credentials.client_id = CONFIG_EXAMPLE_MQTT_CLIENT_ID,
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

esp_err_t app_mqtt_publish_event(const char *event)
{
    if (!s_client || !event) {
        return ESP_ERR_INVALID_STATE;
    }
    char msg_id[64];
    message_id(msg_id, sizeof(msg_id));
    int64_t ts = 0;
    unix_ts_s(&ts);
    char old_payload[256];
    int n = snprintf(old_payload, sizeof(old_payload),
                     "{\"version\":1,\"message_id\":\"%s\",\"id\":\"%s\",\"event\":\"%s\"}",
                     msg_id, device_id(), event);
    int legacy_msg_id =
        esp_mqtt_client_publish(s_client, s_topic_event, old_payload, n, 1, 0);
    char new_payload[384];
    snprintf(new_payload, sizeof(new_payload),
             "{\"v\":1,\"msg_id\":\"%s\",\"ts\":%" PRId64 ",\"type\":\"event\",\"data\":{\"event\":\"%s\"}}",
             msg_id, ts, event);
    int normalized_msg_id =
        esp_mqtt_client_publish(s_client, s_topic_up_event, new_payload, 0, 1, 0);
    if (legacy_msg_id < 0 && normalized_msg_id < 0) {
        ESP_LOGE(TAG, "publish failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "published event=%s", event);
    return ESP_OK;
}


esp_err_t app_mqtt_publish_status(void)
{
    if (!s_client) return ESP_ERR_INVALID_STATE;
    publish_online_status();
    return ESP_OK;
}
