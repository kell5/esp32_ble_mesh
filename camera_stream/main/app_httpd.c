#include "app_httpd.h"

#include <stdio.h>
#include <string.h>
#include "esp_http_server.h"
#include "esp_camera.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "app_httpd";

#define PART_BOUNDARY "123456789000000000000987654321"
static const char *STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char *STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char *STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

static httpd_handle_t s_stream_httpd;

static esp_err_t index_handler(httpd_req_t *req)
{
    static const char page[] =
        "<!doctype html><html><head><meta charset=\"utf-8\">"
        "<title>ESP32-S3 Doorbell</title></head><body>"
        "<h3>ESP32-S3 Doorbell Debug</h3>"
        "<img src=\"/stream\" style=\"max-width:100%\"/>"
        "</body></html>";
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, page, sizeof(page) - 1);
}

static esp_err_t stream_handler(httpd_req_t *req)
{
    esp_err_t res = httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
    if (res != ESP_OK) {
        return res;
    }
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    char part_buf[64];
    while (true) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) {
            ESP_LOGW(TAG, "fb_get failed");
            res = ESP_FAIL;
            break;
        }

        res = httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));
        if (res == ESP_OK) {
            int hlen = snprintf(part_buf, sizeof(part_buf), STREAM_PART, fb->len);
            res = httpd_resp_send_chunk(req, part_buf, hlen);
        }
        if (res == ESP_OK) {
            res = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
        }
        esp_camera_fb_return(fb);

        if (res != ESP_OK) {
            break;
        }
    }
    return res;
}

esp_err_t app_httpd_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.ctrl_port = 32768;
    config.max_uri_handlers = 4;

    httpd_uri_t index_uri = { .uri = "/", .method = HTTP_GET, .handler = index_handler };
    httpd_uri_t stream_uri = { .uri = "/stream", .method = HTTP_GET, .handler = stream_handler };

    // Serve the stream on a dedicated port so the browser page and the MJPEG
    // long-poll do not share a single worker socket.
    config.server_port = 81;
    config.ctrl_port = 32769;
    if (httpd_start(&s_stream_httpd, &config) == ESP_OK) {
        httpd_register_uri_handler(s_stream_httpd, &index_uri);
        httpd_register_uri_handler(s_stream_httpd, &stream_uri);
        ESP_LOGI(TAG, "HTTP debug server started on port %d", config.server_port);
        return ESP_OK;
    }
    ESP_LOGE(TAG, "failed to start HTTP server");
    return ESP_FAIL;
}
