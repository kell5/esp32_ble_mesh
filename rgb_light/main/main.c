#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_app_desc.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "nvs_flash.h"

#include "app_light.h"
#include "app_mqtt.h"
#include "app_wifi.h"

static const char *TAG = "rgb_light";

#define PROV_BUTTON_GPIO      GPIO_NUM_0
#define PROV_BUTTON_HOLD_MS   3000
#define PROV_BUTTON_POLL_MS   100

/* BOOT-button long press (3 s) clears WiFi and reboots into provisioning. */
static void prov_button_task(void *arg)
{
    (void)arg;
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << PROV_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&io) != ESP_OK) {
        vTaskDelete(NULL);
        return;
    }
    uint32_t held_ms = 0;
    for (;;) {
        if (gpio_get_level(PROV_BUTTON_GPIO) == 0) {
            held_ms += PROV_BUTTON_POLL_MS;
            if (held_ms == PROV_BUTTON_HOLD_MS) {
                app_wifi_reset_provisioning();
            }
        } else {
            held_ms = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(PROV_BUTTON_POLL_MS));
    }
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_LOGI(TAG, "firmware %s", esp_app_get_description()->version);

    ESP_ERROR_CHECK(app_light_init());
    app_light_boot_blink(10, 120, 120);

    if (xTaskCreate(prov_button_task, "prov_btn", 2560, NULL, 4, NULL) != pdPASS) {
        ESP_LOGW(TAG, "failed to start provisioning button task");
    }

    ESP_ERROR_CHECK(app_wifi_connect());
    ESP_ERROR_CHECK(app_mqtt_start());
}
