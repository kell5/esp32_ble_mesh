#include "app_relay.h"
#include "app_camera.h"

#include <string.h>
#include <errno.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_camera.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "app_relay";

static volatile bool s_streaming;
static TaskHandle_t s_task;

static int relay_connect(void)
{
    char port[8];
    snprintf(port, sizeof(port), "%d", CONFIG_EXAMPLE_RELAY_PORT);

    struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *res = NULL;
    int err = getaddrinfo(CONFIG_EXAMPLE_RELAY_HOST, port, &hints, &res);
    if (err != 0 || res == NULL) {
        ESP_LOGE(TAG, "DNS lookup failed for %s (err=%d)", CONFIG_EXAMPLE_RELAY_HOST, err);
        return -1;
    }

    int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket() failed errno=%d", errno);
        freeaddrinfo(res);
        return -1;
    }
    if (connect(sock, res->ai_addr, res->ai_addrlen) != 0) {
        ESP_LOGE(TAG, "connect to %s:%s failed errno=%d",
                 CONFIG_EXAMPLE_RELAY_HOST, port, errno);
        close(sock);
        freeaddrinfo(res);
        return -1;
    }
    freeaddrinfo(res);
    return sock;
}

static bool send_all(int sock, const char *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        int w = send(sock, buf + off, len - off, 0);
        if (w <= 0) {
            return false;
        }
        off += (size_t)w;
    }
    return true;
}

static void relay_push_task(void *arg)
{
    int sock = relay_connect();
    if (sock < 0) {
        s_streaming = false;
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    char req[256];
    int n = snprintf(req, sizeof(req),
                     "POST /pub/%s HTTP/1.1\r\n"
                     "Host: %s\r\n"
                     "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n"
                     "Cache-Control: no-cache\r\n"
                     "Connection: close\r\n\r\n",
                     CONFIG_EXAMPLE_DOORBELL_ID, CONFIG_EXAMPLE_RELAY_HOST);
    if (!send_all(sock, req, n)) {
        ESP_LOGE(TAG, "send request header failed");
        goto cleanup;
    }
    ESP_LOGI(TAG, "relay connected: %s:%d /pub/%s",
             CONFIG_EXAMPLE_RELAY_HOST, CONFIG_EXAMPLE_RELAY_PORT, CONFIG_EXAMPLE_DOORBELL_ID);

    const TickType_t frame_delay = pdMS_TO_TICKS(1000 / APP_CAM_FPS);
    char hdr[128];
    while (s_streaming) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) {
            ESP_LOGW(TAG, "fb_get returned NULL");
            vTaskDelay(frame_delay);
            continue;
        }
        int hn = snprintf(hdr, sizeof(hdr),
                          "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
                          (unsigned)fb->len);
        bool ok = send_all(sock, hdr, hn) &&
                  send_all(sock, (const char *)fb->buf, fb->len) &&
                  send_all(sock, "\r\n", 2);
        esp_camera_fb_return(fb);
        if (!ok) {
            ESP_LOGW(TAG, "relay send failed, stopping");
            break;
        }
        vTaskDelay(frame_delay);
    }

cleanup:
    close(sock);
    ESP_LOGI(TAG, "relay push task exited");
    s_streaming = false;
    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t app_relay_start(void)
{
    if (s_streaming) {
        return ESP_OK;
    }
    s_streaming = true;
    if (xTaskCreatePinnedToCore(relay_push_task, "relay_push", 6144, NULL, 5, &s_task, 0) != pdPASS) {
        s_streaming = false;
        ESP_LOGE(TAG, "failed to create relay task");
        return ESP_FAIL;
    }
    return ESP_OK;
}

void app_relay_stop(void)
{
    s_streaming = false;
}

bool app_relay_is_streaming(void)
{
    return s_streaming;
}
