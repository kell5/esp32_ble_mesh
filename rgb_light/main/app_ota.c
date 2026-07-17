#include "app_ota.h"

#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_https_ota.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_system.h"

static const char *TAG = "app_ota";

static bool s_running;

static void ota_task(void *arg)
{
    char *url = (char *)arg;
    ESP_LOGI(TAG, "starting OTA from %s", url);

    esp_http_client_config_t http_cfg = {
        .url = url,
        .timeout_ms = 30000,
        .keep_alive_enable = true,
    };
    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
    };
    esp_err_t err = esp_https_ota(&ota_cfg);
    free(url);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "OTA succeeded, rebooting into new firmware");
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    }
    ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(err));
    s_running = false;
    vTaskDelete(NULL);
}

esp_err_t app_ota_start(const char *url)
{
    if (url == NULL || url[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_running) {
        ESP_LOGW(TAG, "OTA already in progress");
        return ESP_ERR_INVALID_STATE;
    }
    char *copy = strdup(url);
    if (copy == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_running = true;
    if (xTaskCreate(ota_task, "ota", 8192, copy, 5, NULL) != pdPASS) {
        free(copy);
        s_running = false;
        return ESP_FAIL;
    }
    return ESP_OK;
}
