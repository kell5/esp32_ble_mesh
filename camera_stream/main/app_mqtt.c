#include "app_mqtt.h"
#include "app_doorbell.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "mqtt_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "sdkconfig.h"

static const char *TAG = "app_mqtt";

static esp_mqtt_client_handle_t s_client;
static char s_topic_event[96];
static char s_topic_cmd[96];
static char s_topic_status[96];
static char s_lwt_payload[192];
static uint32_t s_boot_nonce;
static uint32_t s_sequence;

static void message_id(char *out, size_t max)
{
    uint32_t sequence = __atomic_add_fetch(&s_sequence, 1, __ATOMIC_RELAXED);
    snprintf(out, max, "%s-%08" PRIx32 "-%" PRIu32,
             CONFIG_EXAMPLE_DOORBELL_ID, s_boot_nonce, sequence);
}

static void publish_online_status(void)
{
    char id[64];
    message_id(id, sizeof(id));
    char payload[192];
    snprintf(payload, sizeof(payload),
             "{\"version\":1,\"message_id\":\"%s\",\"id\":\"%s\",\"online\":true,\"type\":\"doorbell\"}",
             id, CONFIG_EXAMPLE_DOORBELL_ID);
    esp_mqtt_client_publish(s_client, s_topic_status, payload, 0, 1, 1);
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "connected, subscribing %s", s_topic_cmd);
        esp_mqtt_client_subscribe(s_client, s_topic_cmd, 1);
        publish_online_status();
        break;
    case MQTT_EVENT_DATA:
        if (event->topic_len == (int)strlen(s_topic_cmd) &&
            strncmp(event->topic, s_topic_cmd, event->topic_len) == 0) {
            app_doorbell_on_command(event->data, event->data_len);
        }
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "disconnected");
        break;
    default:
        break;
    }
}

esp_err_t app_mqtt_start(void)
{
    snprintf(s_topic_event, sizeof(s_topic_event), "doorbell/%s/event", CONFIG_EXAMPLE_DOORBELL_ID);
    snprintf(s_topic_cmd, sizeof(s_topic_cmd), "doorbell/%s/cmd", CONFIG_EXAMPLE_DOORBELL_ID);
    snprintf(s_topic_status, sizeof(s_topic_status), "doorbell/%s/status", CONFIG_EXAMPLE_DOORBELL_ID);
    if (s_boot_nonce == 0) {
        s_boot_nonce = esp_random();
    }
    char id[64];
    message_id(id, sizeof(id));
    snprintf(s_lwt_payload, sizeof(s_lwt_payload),
             "{\"version\":1,\"message_id\":\"%s\",\"id\":\"%s\",\"online\":false,\"type\":\"doorbell\",\"offline_reason\":\"mqtt_lwt\"}",
             id, CONFIG_EXAMPLE_DOORBELL_ID);

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
    char id[64];
    message_id(id, sizeof(id));
    char payload[192];
    int n = snprintf(payload, sizeof(payload),
                     "{\"version\":1,\"message_id\":\"%s\",\"id\":\"%s\",\"event\":\"%s\"}",
                     id, CONFIG_EXAMPLE_DOORBELL_ID, event);
    int msg_id = esp_mqtt_client_publish(s_client, s_topic_event, payload, n, 1, 0);
    if (msg_id < 0) {
        ESP_LOGE(TAG, "publish failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "published event=%s", event);
    return ESP_OK;
}
