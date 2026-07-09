#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"

#include "app_wifi.h"
#include "app_camera.h"
#include "app_httpd.h"
#include "app_mqtt.h"
#include "app_doorbell.h"

static const char *TAG = "camera_stream";

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_ERROR_CHECK(app_wifi_connect());
    ESP_ERROR_CHECK(app_camera_init());
    ESP_ERROR_CHECK(app_httpd_start());
    ESP_ERROR_CHECK(app_mqtt_start());
    ESP_ERROR_CHECK(app_doorbell_init());

    ESP_LOGI(TAG, "camera_stream ready: press doorbell (GPIO%d) to start streaming",
             CONFIG_EXAMPLE_DOORBELL_GPIO);
}
