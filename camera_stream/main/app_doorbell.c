#include "app_doorbell.h"
#include "app_mqtt.h"
#include "app_relay.h"
#include "app_wifi.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "app_doorbell";

#define DEBOUNCE_MS 300
// Hold the doorbell button this long (while running) to forget WiFi & re-provision.
#define REPROVISION_HOLD_MS 3000

static TaskHandle_t s_task;
static esp_timer_handle_t s_timeout_timer;
static volatile int64_t s_last_press_us;

static void IRAM_ATTR doorbell_isr(void *arg)
{
    BaseType_t hp = pdFALSE;
    vTaskNotifyGiveFromISR(s_task, &hp);
    if (hp) {
        portYIELD_FROM_ISR();
    }
}

static void timeout_cb(void *arg)
{
    ESP_LOGI(TAG, "stream timeout reached");
    app_doorbell_stop_stream("timeout");
}

// (Re)arm the inactivity timeout that stops the stream when nobody is watching.
static void arm_timeout(void)
{
    esp_timer_stop(s_timeout_timer);
    esp_timer_start_once(s_timeout_timer,
                         (uint64_t)CONFIG_EXAMPLE_STREAM_TIMEOUT_SEC * 1000000ULL);
}

void app_doorbell_start_stream(void)
{
    if (app_relay_is_streaming()) {
        // Already streaming: treat as a viewer keepalive and refresh the timeout
        // so on-demand viewing doesn't stop after the initial window.
        arm_timeout();
        return;
    }
    ESP_LOGI(TAG, "doorbell ring -> start stream");
    app_mqtt_publish_event("ringing");
    if (app_relay_start() == ESP_OK) {
        app_mqtt_publish_event("stream_start");
        arm_timeout();
    }
}

void app_doorbell_stop_stream(const char *reason)
{
    if (!app_relay_is_streaming()) {
        return;
    }
    esp_timer_stop(s_timeout_timer);
    app_relay_stop();
    app_mqtt_publish_event("stream_stop");
    ESP_LOGI(TAG, "stream stopped (%s)", reason ? reason : "");
}

void app_doorbell_on_command(const char *cmd, int len)
{
    if (!cmd || len <= 0) {
        return;
    }
    if (len == 6 && strncmp(cmd, "hangup", 6) == 0) {
        app_doorbell_stop_stream("hangup");
    } else if (len == 6 && strncmp(cmd, "stream", 6) == 0) {
        // On-demand view request from the app (not a physical ring): start the
        // stream if idle, or refresh the timeout if already running.
        app_doorbell_start_stream();
    } else if (len == 8 && strncmp(cmd, "snapshot", 8) == 0) {
        ESP_LOGI(TAG, "snapshot command received");
    } else if (len == 11 && strncmp(cmd, "reprovision", 11) == 0) {
        ESP_LOGW(TAG, "reprovision command received -> forgetting WiFi");
        app_wifi_reset_provisioning();
    } else {
        ESP_LOGW(TAG, "unknown cmd: %.*s", len, cmd);
    }
}

static void doorbell_task(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        int64_t now = esp_timer_get_time();
        if (now - s_last_press_us < DEBOUNCE_MS * 1000) {
            continue;
        }
        s_last_press_us = now;

        // Measure how long the button stays held. A short press rings the
        // doorbell; holding ~3s forgets the stored WiFi and re-provisions.
        int64_t start = now;
        bool long_press = false;
        while (gpio_get_level(CONFIG_EXAMPLE_DOORBELL_GPIO) == 0) {
            if (esp_timer_get_time() - start >= REPROVISION_HOLD_MS * 1000) {
                long_press = true;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        if (long_press) {
            ESP_LOGW(TAG, "doorbell long-press -> reprovision");
            app_wifi_reset_provisioning();
        } else {
            app_doorbell_start_stream();
        }
    }
}

esp_err_t app_doorbell_init(void)
{
    const esp_timer_create_args_t targs = {
        .callback = timeout_cb,
        .name = "doorbell_timeout",
    };
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_timeout_timer));

    if (xTaskCreate(doorbell_task, "doorbell", 4096, NULL, 6, &s_task) != pdPASS) {
        return ESP_FAIL;
    }

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << CONFIG_EXAMPLE_DOORBELL_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    ESP_ERROR_CHECK(gpio_isr_handler_add(CONFIG_EXAMPLE_DOORBELL_GPIO, doorbell_isr, NULL));

    ESP_LOGI(TAG, "doorbell button on GPIO%d, timeout=%ds",
             CONFIG_EXAMPLE_DOORBELL_GPIO, CONFIG_EXAMPLE_STREAM_TIMEOUT_SEC);
    return ESP_OK;
}
