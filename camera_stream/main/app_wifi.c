#include "app_wifi.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "driver/gpio.h"
#include "sdkconfig.h"

#include "network_provisioning/manager.h"
#include "network_provisioning/scheme_softap.h"

static const char *TAG = "app_wifi";

#define WIFI_CONNECTED_BIT BIT0

static EventGroupHandle_t s_wifi_event_group;

// Build the SoftAP service name as "<prefix><last 3 MAC bytes>", e.g. Doorbell-4C3408.
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
            ESP_LOGI(TAG, "provisioning started (join SoftAP, then send WiFi via app)");
            break;
        case NETWORK_PROV_WIFI_CRED_RECV: {
            wifi_sta_config_t *cfg = (wifi_sta_config_t *)event_data;
            ESP_LOGI(TAG, "received WiFi credentials: SSID=%s", (const char *)cfg->ssid);
            break;
        }
        case NETWORK_PROV_WIFI_CRED_FAIL:
            ESP_LOGW(TAG, "provisioning failed (wrong password or AP not found), retry from app");
            break;
        case NETWORK_PROV_WIFI_CRED_SUCCESS:
            ESP_LOGI(TAG, "provisioning successful");
            break;
        case NETWORK_PROV_END:
            ESP_LOGI(TAG, "provisioning finished, deinit manager");
            network_prov_mgr_deinit();
            break;
        default:
            break;
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        // Robust reconnect: keep retrying forever, never abort.
        ESP_LOGW(TAG, "disconnected, reconnecting...");
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "got ip: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

// Forget the stored WiFi credentials and reboot into SoftAP provisioning.
// Triggered at runtime (never at boot) by a long-press of the doorbell button
// or by the MQTT "reprovision" command. Note: GPIO0 is the ESP32-S3 boot
// strapping pin, so a boot-time hold would enter USB download mode instead of
// the app -- that is why re-provisioning is a runtime action, not a boot hold.
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
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(NETWORK_PROV_EVENT, ESP_EVENT_ANY_ID,
                                                        &event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &event_handler, NULL, NULL));

    network_prov_mgr_config_t prov_cfg = {
        .scheme = network_prov_scheme_softap,
        .scheme_event_handler = NETWORK_PROV_EVENT_HANDLER_NONE,
    };
    ESP_ERROR_CHECK(network_prov_mgr_init(prov_cfg));

    bool provisioned = false;
    ESP_ERROR_CHECK(network_prov_mgr_is_wifi_provisioned(&provisioned));

    if (!provisioned) {
        char service_name[32];
        build_service_name(service_name, sizeof(service_name));
        const char *pop = CONFIG_EXAMPLE_PROV_POP;
        ESP_LOGI(TAG, "not provisioned -> starting SoftAP provisioning, AP name: %s", service_name);
        // Open SoftAP (no password); data channel is encrypted via Security1 + PoP.
        ESP_ERROR_CHECK(network_prov_mgr_start_provisioning(
            NETWORK_PROV_SECURITY_1, (const void *)pop, service_name, NULL));
        // Provisioning drives the STA connection; wait for it to complete then release manager.
        network_prov_mgr_wait();
        network_prov_mgr_deinit();
    } else {
        ESP_LOGI(TAG, "already provisioned -> connecting with stored WiFi");
        network_prov_mgr_deinit();
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_start());
    }

    // Block until we obtain an IP (covers both fresh-provision and stored-creds paths).
    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT,
                        pdFALSE, pdFALSE, portMAX_DELAY);
    return ESP_OK;
}
