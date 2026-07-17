#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "cJSON.h"
#include "mqtt_client.h"
#include "network_provisioning/manager.h"
#include "network_provisioning/scheme_softap.h"
#include "esp_http_server.h"

#include "gateway_bridge.h"
#include "captive_portal.h"

#define TAG "FARMELY_BRIDGE"

/* Fixed SoftAP channel during provisioning so the AP does not hop while the
 * phone associates. */
#define FARMELY_PROV_AP_CHANNEL 1

/* Long-press this button (BOOT, active low) to erase the stored Wi-Fi
 * credentials and reboot into SoftAP provisioning mode. */
#define FARMELY_PROV_BUTTON_GPIO      GPIO_NUM_0
#define FARMELY_PROV_BUTTON_HOLD_MS   3000
#define FARMELY_PROV_BUTTON_POLL_MS   100

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
static bool s_wifi_autoconnect;
static httpd_handle_t s_prov_httpd;

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

/* Polls the BOOT button; a >=3 s press wipes the stored Wi-Fi credentials
 * (esp_wifi_restore) and restarts, which drops the gateway back into the
 * SoftAP provisioning flow on boot. */
static void prov_button_task(void *arg)
{
    (void)arg;
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << FARMELY_PROV_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&io) != ESP_OK) {
        ESP_LOGE(TAG, "provisioning button GPIO%d config failed",
                 (int)FARMELY_PROV_BUTTON_GPIO);
        vTaskDelete(NULL);
        return;
    }

    uint32_t held_ms = 0;
    for (;;) {
        if (gpio_get_level(FARMELY_PROV_BUTTON_GPIO) == 0) {
            held_ms += FARMELY_PROV_BUTTON_POLL_MS;
            if (held_ms == FARMELY_PROV_BUTTON_HOLD_MS) {
                ESP_LOGW(TAG, "button held %d ms -> erasing Wi-Fi credentials,"
                              " rebooting into provisioning mode",
                         (int)FARMELY_PROV_BUTTON_HOLD_MS);
                esp_wifi_restore();
                vTaskDelay(pdMS_TO_TICKS(100));
                esp_restart();
            }
        } else {
            held_ms = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(FARMELY_PROV_BUTTON_POLL_MS));
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_START) {
        farmely_captive_portal_configure_dhcp_dns();
        wifi_config_t ap_config;
        if (esp_wifi_get_config(WIFI_IF_AP, &ap_config) == ESP_OK &&
            ap_config.ap.channel != FARMELY_PROV_AP_CHANNEL) {
            ap_config.ap.channel = FARMELY_PROV_AP_CHANNEL;
            esp_wifi_set_config(WIFI_IF_AP, &ap_config);
            ESP_LOGI(TAG, "SoftAP pinned to channel %d",
                     FARMELY_PROV_AP_CHANNEL);
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (s_wifi_autoconnect) {
            esp_wifi_connect();
        }
    } else if (event_base == WIFI_EVENT &&
               event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_mqtt_connected = false;
        if (s_wifi_autoconnect) {
            esp_wifi_connect();
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        s_wifi_autoconnect = true;
        mqtt_start();
    }
}

/* SoftAP provisioning name: "<prefix><last 3 STA MAC bytes>", e.g. Gateway-D31548. */
static void build_prov_service_name(char *out, size_t max)
{
    uint8_t mac[6] = {0};
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    snprintf(out, max, "%s%02X%02X%02X",
             CONFIG_FARMELY_PROV_SOFTAP_PREFIX, mac[3], mac[4], mac[5]);
}

/* Custom provisioning endpoint returning the gateway identity + a claim code,
 * mirroring the doorbell's "custom-data" endpoint so the app binds uniformly. */
static esp_err_t prov_custom_data_handler(uint32_t session_id, const uint8_t *inbuf,
                                          ssize_t inlen, uint8_t **outbuf,
                                          ssize_t *outlen, void *priv_data)
{
    (void)session_id; (void)inbuf; (void)inlen; (void)priv_data;
    uint32_t claim_code = esp_random();
    char resp[128];
    int n = snprintf(resp, sizeof(resp),
                     "{\"device_id\":\"%s\",\"claim_code\":\"%08" PRIx32 "\","
                     "\"type\":\"gateway\"}",
                     s_gateway_id, claim_code);
    if (n < 0 || n >= (int)sizeof(resp)) {
        return ESP_ERR_NO_MEM;
    }
    *outbuf = malloc(n + 1);
    if (*outbuf == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(*outbuf, resp, n + 1);
    *outlen = n + 1;
    return ESP_OK;
}

static void prov_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;
    if (event_base != NETWORK_PROV_EVENT) {
        return;
    }
    switch (event_id) {
    case NETWORK_PROV_START:
        ESP_LOGI(TAG, "SoftAP provisioning started");
        break;
    case NETWORK_PROV_WIFI_CRED_RECV: {
        wifi_sta_config_t *cfg = (wifi_sta_config_t *)event_data;
        ESP_LOGI(TAG, "received Wi-Fi credentials: SSID=%s",
                 (const char *)cfg->ssid);
        break;
    }
    case NETWORK_PROV_WIFI_CRED_FAIL:
        ESP_LOGW(TAG, "provisioning failed, retry from app");
        break;
    case NETWORK_PROV_WIFI_CRED_SUCCESS:
        ESP_LOGI(TAG, "provisioning successful");
        break;
    case NETWORK_PROV_END:
        ESP_LOGI(TAG, "provisioning finished");
        break;
    default:
        break;
    }
}

/* Runs the network_provisioning manager off the caller's stack so app_main and
 * BLE Mesh keep running. SoftAP keeps BLE fully available for BLE Mesh. */
static void gateway_wifi_task(void *arg)
{
    (void)arg;
    network_prov_mgr_config_t prov_cfg = {
        .scheme = network_prov_scheme_softap,
        .scheme_event_handler = NETWORK_PROV_EVENT_HANDLER_NONE,
    };
    if (network_prov_mgr_init(prov_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "network_prov_mgr_init failed");
        vTaskDelete(NULL);
        return;
    }

    bool provisioned = false;
    network_prov_mgr_is_wifi_provisioned(&provisioned);

    if (!provisioned) {
        /* Verbose provisioning diagnostics: shows protocomm session setup,
         * security1 handshake steps and per-request httpd activity so a
         * failed "establish session" can be located from the UART log. */
        esp_log_level_set("protocomm", ESP_LOG_DEBUG);
        esp_log_level_set("protocomm_httpd", ESP_LOG_DEBUG);
        esp_log_level_set("security1", ESP_LOG_DEBUG);
        esp_log_level_set("httpd_uri", ESP_LOG_DEBUG);
        esp_log_level_set("httpd_sess", ESP_LOG_DEBUG);
        esp_log_level_set("httpd_parse", ESP_LOG_DEBUG);

        char service_name[32];
        build_prov_service_name(service_name, sizeof(service_name));
        ESP_LOGI(TAG, "not provisioned -> SoftAP \"%s\" at 192.168.4.1 "
                      "(connect phone, then provision from app)", service_name);
        /* Share a captive-portal keepalive HTTP server with protocomm so the
         * phone keeps the (internet-less) SoftAP connected long enough to
         * provision. Must be set before start_provisioning(). */
        if (farmely_captive_portal_start(&s_prov_httpd) == ESP_OK) {
            network_prov_scheme_softap_set_httpd_handle(&s_prov_httpd);
        }

        /* Give the SoftAP the radio while the phone associates and
         * provisions: BLE Mesh scanning otherwise competes for the shared
         * 2.4 GHz radio and makes 802.11 association intermittently fail. */
        farmely_mesh_yield_radio(true);

        ESP_ERROR_CHECK(network_prov_mgr_endpoint_create("custom-data"));
        ESP_ERROR_CHECK(network_prov_mgr_start_provisioning(
            NETWORK_PROV_SECURITY_1, (const void *)CONFIG_FARMELY_PROV_POP,
            service_name, NULL));
        ESP_ERROR_CHECK(network_prov_mgr_endpoint_register(
            "custom-data", prov_custom_data_handler, NULL));
        network_prov_mgr_wait();
        farmely_mesh_yield_radio(false);
        farmely_captive_portal_stop(s_prov_httpd);
        s_prov_httpd = NULL;
        network_prov_mgr_deinit();
    } else {
        ESP_LOGI(TAG, "already provisioned -> connecting with stored Wi-Fi");
        network_prov_mgr_deinit();
        s_wifi_autoconnect = true;
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_start());
    }
    vTaskDelete(NULL);
}

esp_err_t farmely_gateway_bridge_start(const char *gateway_id,
                                       farmely_bridge_command_handler_t command_handler,
                                       farmely_bridge_sync_handler_t sync_handler,
                                       void *ctx)
{
    if (!gateway_id || !gateway_id[0] || !command_handler) {
        return ESP_ERR_INVALID_ARG;
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
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t wifi_init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(NETWORK_PROV_EVENT, ESP_EVENT_ANY_ID,
                                               prov_event_handler, NULL));

    /* Optional compile-time override: if a Wi-Fi SSID is baked in, connect
     * directly and skip runtime provisioning (kept for lab/bench builds). */
    if (CONFIG_FARMELY_WIFI_SSID[0] != '\0') {
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

    if (xTaskCreate(prov_button_task, "prov_btn", 2560, NULL, 4, NULL) != pdPASS) {
        ESP_LOGW(TAG, "failed to start provisioning button task");
    }

    /* Runtime SoftAP provisioning: the phone app provides Wi-Fi credentials.
     * BLE stays entirely with BLE Mesh; provisioning uses the Wi-Fi radio. */
    if (xTaskCreate(gateway_wifi_task, "gw_wifi", 4096, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to start gateway Wi-Fi provisioning task");
        return ESP_FAIL;
    }
    return ESP_OK;
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
