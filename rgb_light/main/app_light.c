#include "app_light.h"

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "led_strip.h"
#include "sdkconfig.h"

static const char *TAG = "app_light";

static led_strip_handle_t s_strip;
static bool s_on;
static uint8_t s_r = 255, s_g = 255, s_b = 255;

static void refresh(void)
{
    if (!s_strip) return;
    for (int i = 0; i < CONFIG_EXAMPLE_LIGHT_LED_COUNT; i++) {
        led_strip_set_pixel(s_strip, i,
                            s_on ? s_r : 0,
                            s_on ? s_g : 0,
                            s_on ? s_b : 0);
    }
    led_strip_refresh(s_strip);
}

esp_err_t app_light_init(void)
{
    led_strip_config_t strip_cfg = {
        .strip_gpio_num = CONFIG_EXAMPLE_LIGHT_GPIO,
        .max_leds = CONFIG_EXAMPLE_LIGHT_LED_COUNT,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    led_strip_rmt_config_t rmt_cfg = {
        .resolution_hz = 10 * 1000 * 1000,
    };
    esp_err_t err = led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "led_strip init failed on GPIO%d: %s",
                 CONFIG_EXAMPLE_LIGHT_GPIO, esp_err_to_name(err));
        return err;
    }
    refresh();
    return ESP_OK;
}

void app_light_set_on(bool on)
{
    s_on = on;
    refresh();
    ESP_LOGI(TAG, "light %s", on ? "on" : "off");
}

void app_light_boot_blink(uint8_t times, uint32_t on_ms, uint32_t off_ms)
{
    if (times == 0 || s_strip == NULL) {
        return;
    }
    for (uint8_t i = 0; i < times; ++i) {
        app_light_set_on(true);
        vTaskDelay(pdMS_TO_TICKS(on_ms));
        app_light_set_on(false);
        if (i + 1 < times) {
            vTaskDelay(pdMS_TO_TICKS(off_ms));
        }
    }
}

bool app_light_is_on(void)
{
    return s_on;
}

void app_light_set_color(uint8_t r, uint8_t g, uint8_t b)
{
    s_r = r;
    s_g = g;
    s_b = b;
    if (s_on) refresh();
    ESP_LOGI(TAG, "color -> #%02X%02X%02X", r, g, b);
}

void app_light_get_color(char *out, size_t max)
{
    snprintf(out, max, "#%02X%02X%02X", s_r, s_g, s_b);
}
