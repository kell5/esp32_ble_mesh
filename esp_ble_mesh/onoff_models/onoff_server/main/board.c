/* board.c - Board-specific hooks */

/*
 * SPDX-FileCopyrightText: 2017 Intel Corporation
 * SPDX-FileContributor: 2018-2021 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "board.h"

#if CONFIG_IDF_TARGET_ESP32S3
#include "driver/rmt_tx.h"
#include "led_strip_encoder.h"
#else
#include "driver/gpio.h"
#endif

#define TAG "BOARD"
#define LED_STRIP_RESOLUTION_HZ (10 * 1000 * 1000)

struct _led_state led_state[BOARD_LED_COUNT] = {
    { LED_OFF, LED_OFF, LED_G, "onoff" },
};

#if CONFIG_IDF_TARGET_ESP32S3
static rmt_channel_handle_t s_led_channel;
static rmt_encoder_handle_t s_led_encoder;
static rmt_transmit_config_t s_led_tx_config = {
    .loop_count = 0,
};

static void board_led_write(uint8_t onoff)
{
    uint8_t pixel[3] = {
        onoff ? 32 : 0,
        onoff ? 32 : 0,
        onoff ? 32 : 0,
    };

    ESP_ERROR_CHECK(rmt_transmit(s_led_channel, s_led_encoder, pixel, sizeof(pixel),
                                 &s_led_tx_config));
    ESP_ERROR_CHECK(rmt_tx_wait_all_done(s_led_channel, portMAX_DELAY));
}
#else
static void board_led_write(uint8_t onoff)
{
    gpio_set_level(LED_G, onoff);
}
#endif

void board_led_operation(uint8_t pin, uint8_t onoff)
{
    for (int i = 0; i < BOARD_LED_COUNT; i++) {
        if (led_state[i].pin != pin) {
            continue;
        }
        if (onoff == led_state[i].previous) {
            ESP_LOGW(TAG, "led %s is already %s",
                     led_state[i].name, (onoff ? "on" : "off"));
            return;
        }
        board_led_write(onoff);
        led_state[i].previous = onoff;
        return;
    }

    ESP_LOGE(TAG, "LED is not found!");
}

static void board_led_init(void)
{
#if CONFIG_IDF_TARGET_ESP32S3
    rmt_tx_channel_config_t tx_channel_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .gpio_num = LED_G,
        .mem_block_symbols = 64,
        .resolution_hz = LED_STRIP_RESOLUTION_HZ,
        .trans_queue_depth = 4,
    };
    led_strip_encoder_config_t encoder_config = {
        .resolution = LED_STRIP_RESOLUTION_HZ,
    };

    ESP_ERROR_CHECK(rmt_new_tx_channel(&tx_channel_config, &s_led_channel));
    ESP_ERROR_CHECK(rmt_new_led_strip_encoder(&encoder_config, &s_led_encoder));
    ESP_ERROR_CHECK(rmt_enable(s_led_channel));
#else
    gpio_reset_pin(LED_G);
    gpio_set_direction(LED_G, GPIO_MODE_OUTPUT);
#endif

    board_led_write(LED_OFF);
    for (int i = 0; i < BOARD_LED_COUNT; i++) {
        led_state[i].previous = LED_OFF;
    }
}

void board_init(void)
{
    board_led_init();
}
