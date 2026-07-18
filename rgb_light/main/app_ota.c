#include "app_ota.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_app_desc.h"
#include "esp_https_ota.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "nvs.h"
#include "psa/crypto.h"

static const char *TAG = "app_ota";
static const char *NVS_NAMESPACE = "ota";
static const char *NVS_KEY_FW = "fw";
static const char *NVS_KEY_MSG = "msg";

static bool s_running;
static app_ota_status_cb_t s_status_cb;

typedef struct {
    char url[256];
    char sha256[65];
    char fw_version[64];
    char ota_msg_id[128];
} ota_request_t;

typedef struct {
    psa_hash_operation_t sha_op;
    bool sha_started;
    bool sha_failed;
} ota_hash_t;

void app_ota_set_status_callback(app_ota_status_cb_t cb)
{
    s_status_cb = cb;
}

static void publish_status(const ota_request_t *request, const char *status, const char *detail)
{
    if (s_status_cb == NULL || request == NULL) {
        return;
    }
    s_status_cb(status,
                request->fw_version[0] ? request->fw_version : NULL,
                request->ota_msg_id[0] ? request->ota_msg_id : NULL,
                detail);
}

static bool is_hex_sha256(const char *text)
{
    if (text == NULL || strlen(text) != 64) {
        return false;
    }
    for (size_t i = 0; i < 64; ++i) {
        if (!isxdigit((unsigned char)text[i])) {
            return false;
        }
    }
    return true;
}

static int hex_nibble(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    ch = (char)tolower((unsigned char)ch);
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    return -1;
}

static bool sha256_hex_to_bytes(const char *hex, uint8_t out[32])
{
    if (!is_hex_sha256(hex)) {
        return false;
    }
    for (size_t i = 0; i < 32; ++i) {
        int hi = hex_nibble(hex[i * 2]);
        int lo = hex_nibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

static esp_err_t http_event_handler(esp_http_client_event_t *event)
{
    ota_hash_t *hash = (ota_hash_t *)event->user_data;
    if (hash == NULL || !hash->sha_started) {
        return ESP_OK;
    }
    if (event->event_id == HTTP_EVENT_ON_DATA && event->data != NULL && event->data_len > 0) {
        psa_status_t status = psa_hash_update(&hash->sha_op,
                                              (const uint8_t *)event->data,
                                              (size_t)event->data_len);
        if (status != PSA_SUCCESS) {
            hash->sha_failed = true;
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

static esp_err_t save_pending_success(const ota_request_t *request)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    if (request->fw_version[0]) {
        err = nvs_set_str(nvs, NVS_KEY_FW, request->fw_version);
        if (err != ESP_OK) {
            nvs_close(nvs);
            return err;
        }
    }
    if (request->ota_msg_id[0]) {
        err = nvs_set_str(nvs, NVS_KEY_MSG, request->ota_msg_id);
        if (err != ESP_OK) {
            nvs_close(nvs);
            return err;
        }
    }
    err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

static bool load_pending_success(char *fw_version, size_t fw_len,
                                 char *ota_msg_id, size_t msg_len)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return false;
    }
    size_t fw_size = fw_len;
    size_t msg_size = msg_len;
    esp_err_t fw_err = nvs_get_str(nvs, NVS_KEY_FW, fw_version, &fw_size);
    esp_err_t msg_err = nvs_get_str(nvs, NVS_KEY_MSG, ota_msg_id, &msg_size);
    nvs_close(nvs);
    if (fw_err != ESP_OK) {
        fw_version[0] = '\0';
    }
    if (msg_err != ESP_OK) {
        ota_msg_id[0] = '\0';
    }
    return fw_err == ESP_OK || msg_err == ESP_OK;
}

static void clear_pending_success(void)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        return;
    }
    nvs_erase_key(nvs, NVS_KEY_FW);
    nvs_erase_key(nvs, NVS_KEY_MSG);
    nvs_commit(nvs);
    nvs_close(nvs);
}

static void ota_task(void *arg)
{
    ota_request_t *request = (ota_request_t *)arg;
    esp_https_ota_handle_t handle = NULL;
    ota_hash_t hash = {0};
    uint8_t expected_sha[32];
    bool have_expected_sha = sha256_hex_to_bytes(request->sha256, expected_sha);

    ESP_LOGI(TAG, "starting OTA from %s", request->url);
    publish_status(request, "downloading", "started");

    if (!have_expected_sha) {
        ESP_LOGE(TAG, "OTA command missing valid sha256");
        publish_status(request, "failed", "invalid_sha256");
        free(request);
        s_running = false;
        vTaskDelete(NULL);
    }

    if (have_expected_sha) {
        psa_status_t status = psa_crypto_init();
        if (status == PSA_SUCCESS) {
            hash.sha_op = psa_hash_operation_init();
            status = psa_hash_setup(&hash.sha_op, PSA_ALG_SHA_256);
        }
        if (status != PSA_SUCCESS) {
            ESP_LOGE(TAG, "failed to start sha256: %d", (int)status);
            publish_status(request, "failed", "sha256_start_failed");
            free(request);
            s_running = false;
            vTaskDelete(NULL);
        }
        hash.sha_started = true;
    }

    esp_http_client_config_t http_cfg = {
        .url = request->url,
        .timeout_ms = 30000,
        .keep_alive_enable = true,
        .event_handler = http_event_handler,
        .user_data = &hash,
    };
    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
    };

    esp_err_t err = esp_https_ota_begin(&ota_cfg, &handle);
    while (err == ESP_OK || err == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        err = esp_https_ota_perform(handle);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            break;
        }
    }
    if (err == ESP_OK && !esp_https_ota_is_complete_data_received(handle)) {
        err = ESP_ERR_INVALID_SIZE;
    }
    if (err == ESP_OK && hash.sha_failed) {
        err = ESP_FAIL;
    }
    if (err == ESP_OK && have_expected_sha) {
        uint8_t actual_sha[32];
        size_t actual_len = 0;
        psa_status_t status = psa_hash_finish(&hash.sha_op,
                                              actual_sha,
                                              sizeof(actual_sha),
                                              &actual_len);
        hash.sha_started = false;
        if (status != PSA_SUCCESS || actual_len != sizeof(actual_sha)) {
            ESP_LOGE(TAG, "failed to finish sha256: %d", (int)status);
            err = ESP_FAIL;
        } else if (memcmp(actual_sha, expected_sha, sizeof(actual_sha)) != 0) {
            ESP_LOGE(TAG, "OTA sha256 mismatch");
            err = ESP_ERR_INVALID_CRC;
        }
    }
    if (hash.sha_started) {
        psa_hash_abort(&hash.sha_op);
        hash.sha_started = false;
    }
    if (err == ESP_OK) {
        err = esp_https_ota_finish(handle);
        handle = NULL;
    }
    if (err == ESP_OK) {
        save_pending_success(request);
        ESP_LOGI(TAG, "OTA succeeded, rebooting into new firmware");
        publish_status(request, "downloading", "rebooting");
        free(request);
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    }
    if (handle != NULL) {
        esp_https_ota_abort(handle);
    }
    ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(err));
    publish_status(request, "failed", esp_err_to_name(err));
    free(request);
    s_running = false;
    vTaskDelete(NULL);
}

esp_err_t app_ota_start(const char *url,
                        const char *sha256,
                        const char *fw_version,
                        const char *ota_msg_id)
{
    if (url == NULL || url[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_running) {
        ESP_LOGW(TAG, "OTA already in progress");
        return ESP_ERR_INVALID_STATE;
    }
    ota_request_t *request = calloc(1, sizeof(*request));
    if (request == NULL) {
        return ESP_ERR_NO_MEM;
    }
    strlcpy(request->url, url, sizeof(request->url));
    if (sha256 != NULL) {
        strlcpy(request->sha256, sha256, sizeof(request->sha256));
    }
    if (fw_version != NULL) {
        strlcpy(request->fw_version, fw_version, sizeof(request->fw_version));
    }
    if (ota_msg_id != NULL) {
        strlcpy(request->ota_msg_id, ota_msg_id, sizeof(request->ota_msg_id));
    }
    s_running = true;
    if (xTaskCreate(ota_task, "ota", 12288, request, 5, NULL) != pdPASS) {
        free(request);
        s_running = false;
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t app_ota_confirm_running(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    bool pending_verify = false;
    if (esp_ota_get_state_partition(running, &state) == ESP_OK) {
        pending_verify = (state == ESP_OTA_IMG_PENDING_VERIFY);
    }

    char fw_version[64] = {0};
    char ota_msg_id[128] = {0};
    bool have_pending_success = load_pending_success(
        fw_version, sizeof(fw_version), ota_msg_id, sizeof(ota_msg_id));

    if (!pending_verify && !have_pending_success) {
        return ESP_OK;
    }
    if (fw_version[0] == '\0') {
        strlcpy(fw_version, esp_app_get_description()->version, sizeof(fw_version));
    }

    ota_request_t report = {0};
    strlcpy(report.fw_version, fw_version, sizeof(report.fw_version));
    strlcpy(report.ota_msg_id, ota_msg_id, sizeof(report.ota_msg_id));
    publish_status(&report, "success", "running");

    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "running OTA image marked valid");
    } else {
        ESP_LOGW(TAG, "mark valid returned %s", esp_err_to_name(err));
    }
    clear_pending_success();
    return err;
}
