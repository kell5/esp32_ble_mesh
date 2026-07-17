#include "app_wifi.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_random.h"
#include "sdkconfig.h"

#include "network_provisioning/manager.h"
#ifdef CONFIG_EXAMPLE_PROV_TRANSPORT_BLE
#include "network_provisioning/scheme_ble.h"
#else
#include "network_provisioning/scheme_softap.h"
#endif

static const char *TAG = "app_wifi";

#define WIFI_CONNECTED_BIT BIT0

static EventGroupHandle_t s_wifi_event_group;
static char s_device_id[32] = {0};

static void build_device_id(void)
{
    if (s_device_id[0] != 0) return;
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_device_id, sizeof(s_device_id), "rgb-%02X%02X%02X",
             mac[3], mac[4], mac[5]);
}

const char *app_wifi_get_device_id(void)
{
    if (s_device_id[0] == 0) {
        build_device_id();
    }
    return s_device_id;
}

/* Provisioning custom-data endpoint handler — returns device_id + claim_code */
static esp_err_t custom_data_handler(uint32_t session_id, const uint8_t *inbuf,
                                     ssize_t inlen, uint8_t **outbuf,
                                     ssize_t *outlen, void *priv_data)
{
    (void)session_id; (void)inbuf; (void)inlen; (void)priv_data;
    const char *devid = app_wifi_get_device_id();
    uint32_t claim_code = esp_random();
    char resp[128];
    int n = snprintf(resp, sizeof(resp),
                     "{\"device_id\":\"%s\",\"claim_code\":\"%08" PRIx32 "\"}",
                     devid, claim_code);
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

static void build_service_name(char *out, size_t max)
{
    uint8_t mac[6] = {0};
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    snprintf(out, max, "%s%02X%02X%02X",
             CONFIG_EXAMPLE_PROV_SOFTAP_PREFIX, mac[3], mac[4], mac[5]);
}

static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    if (event_base == NETWORK_PROV_EVENT) {
        switch (event_id) {
        case NETWORK_PROV_START:
            ESP_LOGI(TAG, "provisioning started");
            break;
        case NETWORK_PROV_WIFI_CRED_RECV: {
            wifi_sta_config_t *cfg = (wifi_sta_config_t *)event_data;
            ESP_LOGI(TAG, "received WiFi credentials: SSID=%s", (const char *)cfg->ssid);
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
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "disconnected, reconnecting...");
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "got ip: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void app_wifi_reset_provisioning(void)
{
    ESP_LOGW(TAG, "reset provisioning -> erasing stored WiFi and rebooting");
    esp_wifi_restore();
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
}

esp_err_t app_wifi_connect(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    esp_netif_create_default_wifi_sta();
#ifdef CONFIG_EXAMPLE_PROV_TRANSPORT_SOFTAP
    esp_netif_create_default_wifi_ap();
#endif

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    build_device_id();

    ESP_ERROR_CHECK(esp_event_handler_instance_register(NETWORK_PROV_EVENT, ESP_EVENT_ANY_ID,
                                                        &event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &event_handler, NULL, NULL));

    network_prov_mgr_config_t prov_cfg = {
#ifdef CONFIG_EXAMPLE_PROV_TRANSPORT_BLE
        .scheme = network_prov_scheme_ble,
        .scheme_event_handler = NETWORK_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM,
#else
        .scheme = network_prov_scheme_softap,
        .scheme_event_handler = NETWORK_PROV_EVENT_HANDLER_NONE,
#endif
    };
    ESP_ERROR_CHECK(network_prov_mgr_init(prov_cfg));

    bool provisioned = false;
    ESP_ERROR_CHECK(network_prov_mgr_is_wifi_provisioned(&provisioned));

    if (!provisioned) {
        char service_name[32];
        build_service_name(service_name, sizeof(service_name));
        const char *pop = CONFIG_EXAMPLE_PROV_POP;
#ifdef CONFIG_EXAMPLE_PROV_TRANSPORT_BLE
        uint8_t service_uuid[] = {
            0xb4, 0xdf, 0x5a, 0x1c, 0x3f, 0x6b, 0xf4, 0xbf,
            0xea, 0x4a, 0x82, 0x03, 0x04, 0x90, 0x1a, 0x02,
        };
        ESP_ERROR_CHECK(network_prov_scheme_ble_set_service_uuid(service_uuid));
        ESP_LOGI(TAG, "not provisioned -> BLE advertising as %s", service_name);
#else
        ESP_LOGI(TAG, "not provisioned -> SoftAP name: %s", service_name);
#endif
        ESP_ERROR_CHECK(network_prov_mgr_endpoint_create("custom-data"));
        ESP_ERROR_CHECK(network_prov_mgr_start_provisioning(
            NETWORK_PROV_SECURITY_1, (const void *)pop, service_name, NULL));
        ESP_ERROR_CHECK(network_prov_mgr_endpoint_register(
            "custom-data", custom_data_handler, NULL));
        network_prov_mgr_wait();
        ESP_ERROR_CHECK(network_prov_mgr_deinit());
    } else {
        ESP_LOGI(TAG, "already provisioned -> connecting with stored WiFi");
        ESP_ERROR_CHECK(network_prov_mgr_deinit());
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_start());
    }

    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT,
                        pdFALSE, pdFALSE, portMAX_DELAY);
    return ESP_OK;
}
