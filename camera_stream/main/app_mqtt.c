#include "app_mqtt.h"
#include "app_doorbell.h"

#include <stdio.h>
#include <string.h>
#include "mqtt_client.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "app_mqtt";

static esp_mqtt_client_handle_t s_client;
static char s_topic_event[96];
static char s_topic_cmd[96];

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "connected, subscribing %s", s_topic_cmd);
        esp_mqtt_client_subscribe(s_client, s_topic_cmd, 1);
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

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = CONFIG_EXAMPLE_MQTT_BROKER_URI,
        .credentials.client_id = CONFIG_EXAMPLE_MQTT_CLIENT_ID,
        .credentials.username = CONFIG_EXAMPLE_MQTT_USERNAME,
        .credentials.authentication.password = CONFIG_EXAMPLE_MQTT_PASSWORD,
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
    char payload[128];
    int n = snprintf(payload, sizeof(payload),
                     "{\"id\":\"%s\",\"event\":\"%s\"}",
                     CONFIG_EXAMPLE_DOORBELL_ID, event);
    int msg_id = esp_mqtt_client_publish(s_client, s_topic_event, payload, n, 1, 0);
    if (msg_id < 0) {
        ESP_LOGE(TAG, "publish failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "published event=%s", event);
    return ESP_OK;
}
