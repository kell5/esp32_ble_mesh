/* Mesh Internal Communication Example — LED driver (dual board support)

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.

   ============================
   ESP32-S3 (N16R8) — WS2812 RGB LED on GPIO48
   ============================
   The onboard WS2812 is driven via the RMT peripheral. Each call to
   mesh_light_set() writes a single GRB pixel. Colors map to layers:

     Layer 1 (Root):  Pink    → 一目了然, 根节点
     Layer 2:         Yellow  → 中间节点
     Layer 3:         Red     → 更深的子节点
     Layer 4:         Blue
     Layer 5:         Green
     Layer 6+:        White   → 深层 / 断开连接

   Root 每隔一次发送 On/Off 交替命令, 所有节点同步闪烁。

   ============================
   ESP32 (WROOM) — Simple LED on GPIO2
   ============================
   No RGB available. Layer is indicated by blink count on the blue LED:
     1 blink  → Root
     2 blinks → Layer 2
     ...
     6 blinks → Layer 6+
   Disconnected → fast continuous blink.
*/

#include <string.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_mesh.h"
#include "mesh_light.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#if CONFIG_IDF_TARGET_ESP32S3
#include "driver/rmt_tx.h"
#include "led_strip_encoder.h"
#else
#include "driver/gpio.h"
#endif

static const char *TAG = "mesh_light";

/* Serializes all LED hardware access. mesh_light_process()/mesh_light_set()/
   the connect/disconnect indicators can be invoked from several tasks at once
   (mesh event task, RX task, and the MQTT command path). The RMT driver's
   rmt_transmit()+rmt_tx_wait_all_done() is not re-entrant and blocks forever
   if two transmits race, so every hardware write must hold this mutex. */
static SemaphoreHandle_t s_led_mtx = NULL;

static inline void led_lock(void)   { if (s_led_mtx) xSemaphoreTake(s_led_mtx, portMAX_DELAY); }
static inline void led_unlock(void) { if (s_led_mtx) xSemaphoreGive(s_led_mtx); }

/*******************************************************
 *                Variable Definitions
 *******************************************************/
static bool s_light_inited = false;

#if CONFIG_IDF_TARGET_ESP32S3
/* --- S3: WS2812 RMT handles --- */
static rmt_channel_handle_t   s_led_chan   = NULL;
static rmt_encoder_handle_t   s_led_encoder = NULL;
static rmt_transmit_config_t  s_tx_config   = { .loop_count = 0 };

/* One WS2812 pixel = 3 bytes: [G, R, B] (matches encoder's msb_first GRB order) */
static uint8_t s_led_pixel[3] = { 0, 0, 0 };
#endif

/*******************************************************
 *                S3: WS2812 RGB Helpers
 *******************************************************/
#if CONFIG_IDF_TARGET_ESP32S3

/* Write a GRB pixel to the LED strip and actually transmit it */
static void ws2812_set_pixel(uint8_t g, uint8_t r, uint8_t b)
{
    led_lock();
    s_led_pixel[0] = g;
    s_led_pixel[1] = r;
    s_led_pixel[2] = b;
    esp_err_t err = rmt_transmit(s_led_chan, s_led_encoder,
                                 s_led_pixel, sizeof(s_led_pixel),
                                 &s_tx_config);
    if (err == ESP_OK) {
        err = rmt_tx_wait_all_done(s_led_chan, portMAX_DELAY);
    }
    led_unlock();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ws2812 transmit err:0x%x", err);
    }
}

/* Map layer number → RGB color (same semantics as original WROVER-KIT code)
   ws2812_set_pixel(G, R, B) — GRB byte order for WS2812 */
static void ws2812_set_layer_color(int layer)
{
    /* ws2812_set_pixel(G, R, B) */
    switch (layer) {
    case 1:  ws2812_set_pixel(0,   255, 100); break;  // Pink
    case 2:  ws2812_set_pixel(200, 255,   0); break;  // Yellow
    case 3:  ws2812_set_pixel(0,   255,   0); break;  // Red
    case 4:  ws2812_set_pixel(0,     0, 255); break;  // Blue
    case 5:  ws2812_set_pixel(255,   0,   0); break;  // Green
    default: ws2812_set_pixel(255, 255, 255); break;  // White (L6+/unknown)
    }
}

#endif /* CONFIG_IDF_TARGET_ESP32S3 */

/*******************************************************
 *                Function Definitions
 *******************************************************/

esp_err_t mesh_light_init(void)
{
    if (s_light_inited) {
        return ESP_OK;
    }
    s_light_inited = true;

    if (!s_led_mtx) {
        s_led_mtx = xSemaphoreCreateMutex();
    }

#if CONFIG_IDF_TARGET_ESP32S3
    /* ---- ESP32-S3: set up RMT TX channel for WS2812 ---- */
    ESP_LOGI(TAG, "Init WS2812 RGB LED on GPIO%d", MESH_LIGHT_RMT_GPIO);

    rmt_tx_channel_config_t tx_chan_config = {
        .clk_src            = RMT_CLK_SRC_DEFAULT,
        .gpio_num           = MESH_LIGHT_RMT_GPIO,
        .mem_block_symbols  = 64,
        .resolution_hz      = 10 * 1000 * 1000,  // 10 MHz for WS2812 timing
        .trans_queue_depth  = 4,
    };
    ESP_ERROR_CHECK(rmt_new_tx_channel(&tx_chan_config, &s_led_chan));

    led_strip_encoder_config_t encoder_config = {
        .resolution = 10 * 1000 * 1000,  // 10 MHz
    };
    ESP_ERROR_CHECK(rmt_new_led_strip_encoder(&encoder_config, &s_led_encoder));

    ESP_ERROR_CHECK(rmt_enable(s_led_chan));

    /* Start with LED off */
    ws2812_set_pixel(0, 0, 0);

#else
    /* ---- ESP32: simple GPIO output ---- */
    ESP_LOGI(TAG, "Init simple LED on GPIO%d", MESH_LIGHT_GPIO);

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << MESH_LIGHT_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = 0,
        .pull_down_en = 0,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    gpio_set_level(MESH_LIGHT_GPIO, 0);
#endif

    return ESP_OK;
}

/* ---- Simple on/off (used by ESP32; on S3 this is mainly for "off") ---- */
esp_err_t mesh_light_set(int state)
{
#if CONFIG_IDF_TARGET_ESP32S3
    if (!state) {
        ws2812_set_pixel(0, 0, 0);  /* off */
    }
    /* "on" without layer context → just leave as-is; mesh_connected_indicator
       will be called when a command with on=1 arrives */
#else
    gpio_set_level(MESH_LIGHT_GPIO, state ? 1 : 0);
#endif
    return ESP_OK;
}

/* ---- Called when node successfully connects or changes layer ---- */
void mesh_connected_indicator(int layer)
{
#if CONFIG_IDF_TARGET_ESP32S3
    /* S3: just set the layer color. The MPRX task will blink it via On/Off. */
    ws2812_set_layer_color(layer);
#else
    /* ESP32: blink N times = layer number */
    int blinks = (layer >= 1 && layer <= 6) ? layer : 6;
    for (int i = 0; i < blinks; i++) {
        gpio_set_level(MESH_LIGHT_GPIO, 1);
        vTaskDelay(pdMS_TO_TICKS(150));
        gpio_set_level(MESH_LIGHT_GPIO, 0);
        vTaskDelay(pdMS_TO_TICKS(150));
    }
    vTaskDelay(pdMS_TO_TICKS(800));  /* pause between sequences */
#endif
}

/* ---- Called when node disconnects ---- */
void mesh_disconnected_indicator(void)
{
#if CONFIG_IDF_TARGET_ESP32S3
    /* S3: flash red 3 times as warning */
    for (int i = 0; i < 3; i++) {
        ws2812_set_pixel(0, 255, 0);        /* Red */
        vTaskDelay(pdMS_TO_TICKS(200));
        ws2812_set_pixel(0, 0, 0);          /* Off */
        vTaskDelay(pdMS_TO_TICKS(200));
    }
#else
    /* ESP32: fast continuous blink */
    for (int i = 0; i < 5; i++) {
        gpio_set_level(MESH_LIGHT_GPIO, 1);
        vTaskDelay(pdMS_TO_TICKS(80));
        gpio_set_level(MESH_LIGHT_GPIO, 0);
        vTaskDelay(pdMS_TO_TICKS(80));
    }
#endif
}

/* ---- Process incoming light-control message (called from MPRX task) ---- */
esp_err_t mesh_light_process(mesh_addr_t *from, uint8_t *buf, uint16_t len)
{
    mesh_light_ctl_t *in = (mesh_light_ctl_t *) buf;
    if (!from || !buf || len < sizeof(mesh_light_ctl_t)) {
        return ESP_FAIL;
    }
    if (in->token_id != MESH_TOKEN_ID || in->token_value != MESH_TOKEN_VALUE) {
        return ESP_FAIL;
    }
    if (in->cmd == MESH_CONTROL_CMD) {
        if (in->on) {
            mesh_connected_indicator(esp_mesh_get_layer());
        } else {
            mesh_light_set(0);   /* turn LED off */
        }
    }
    return ESP_OK;
}
