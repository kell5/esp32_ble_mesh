/* main.c - Application main entry point */

/*
 * SPDX-FileCopyrightText: 2017 Intel Corporation
 * SPDX-FileContributor: 2018-2021 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_ble_mesh_defs.h"
#include "esp_ble_mesh_common_api.h"
#include "esp_ble_mesh_provisioning_api.h"
#include "esp_ble_mesh_networking_api.h"
#include "esp_ble_mesh_config_model_api.h"
#include "esp_ble_mesh_generic_model_api.h"

#include "ble_mesh_example_init.h"
#include "gateway_bridge.h"

#define TAG "EXAMPLE"

#define LED_OFF             0x0
#define LED_ON              0x1

#define CID_ESP             0x02E5

#define PROV_OWN_ADDR       0x0001

#define MSG_SEND_TTL        3
#define MSG_TIMEOUT         0
#define MSG_ROLE            ROLE_PROVISIONER

#define COMP_DATA_PAGE_0    0x00

#define APP_KEY_IDX         0x0000
#define APP_KEY_OCTET       0x12
#define FARMELY_GROUP_ADDR  0xC000
#define FARMELY_MAX_RETRIES 3

static uint8_t dev_uuid[16];
static uint8_t s_tid;
static uint16_t s_toggle_after_get_addr = ESP_BLE_MESH_ADDR_UNASSIGNED;

enum {
    FARMELY_STAGE_STARTED = 1,
    FARMELY_STAGE_NODE_FOUND,
    FARMELY_STAGE_PROVISIONED,
    FARMELY_STAGE_COMPOSITION_RECEIVED,
    FARMELY_STAGE_APP_KEY_ADDED,
    FARMELY_STAGE_MODEL_BOUND,
    FARMELY_STAGE_ONOFF_RECEIVED,
    FARMELY_STAGE_ONOFF_SET,
    FARMELY_STAGE_NODE_RECOVERED,
};

typedef struct {
    uint8_t  uuid[16];
    uint16_t unicast;
    uint8_t  elem_num;
    uint8_t  onoff;
} esp_ble_mesh_node_info_t;

static esp_ble_mesh_node_info_t nodes[CONFIG_BLE_MESH_MAX_PROV_NODES] = {0};
static uint8_t s_progress_stage;

typedef struct {
    bool in_flight;
    uint8_t desired_onoff;
    uint8_t retries;
    char message_id[96];
} farmely_pending_command_t;

static farmely_pending_command_t s_pending[CONFIG_BLE_MESH_MAX_PROV_NODES];
static uint8_t s_get_retries[CONFIG_BLE_MESH_MAX_PROV_NODES];
static uint8_t s_config_retries[CONFIG_BLE_MESH_MAX_PROV_NODES];
static uint32_t s_status_version[CONFIG_BLE_MESH_MAX_PROV_NODES];

static void farmely_store_progress(uint8_t stage, uint16_t addr, uint8_t onoff, int32_t error)
{
    nvs_handle_t handle;

    if (stage < s_progress_stage && error == 0) {
        return;
    }
    if (stage > s_progress_stage) {
        s_progress_stage = stage;
    }

    if (nvs_open("farmely_mesh", NVS_READWRITE, &handle) != ESP_OK) {
        return;
    }

    nvs_set_u8(handle, "stage", s_progress_stage);
    nvs_set_u16(handle, "addr", addr);
    nvs_set_u8(handle, "onoff", onoff);
    nvs_set_i32(handle, "last_err", error);
    nvs_commit(handle);
    nvs_close(handle);
}

static void farmely_store_nodes(void)
{
    nvs_handle_t handle;

    if (nvs_open("farmely_mesh", NVS_READWRITE, &handle) != ESP_OK) {
        return;
    }

    nvs_set_blob(handle, "nodes", nodes, sizeof(nodes));
    nvs_commit(handle);
    nvs_close(handle);
}

static void farmely_load_state(void)
{
    nvs_handle_t handle;
    size_t size = sizeof(nodes);

    if (nvs_open("farmely_mesh", NVS_READONLY, &handle) != ESP_OK) {
        return;
    }

    nvs_get_u8(handle, "stage", &s_progress_stage);
    if (nvs_get_blob(handle, "nodes", nodes, &size) != ESP_OK || size != sizeof(nodes)) {
        memset(nodes, 0, sizeof(nodes));
    }
    nvs_close(handle);
}

static struct esp_ble_mesh_key {
    uint16_t net_idx;
    uint16_t app_idx;
    uint8_t  app_key[16];
} prov_key;

static esp_ble_mesh_client_t config_client;
static esp_ble_mesh_client_t onoff_client;

static esp_ble_mesh_cfg_srv_t config_server = {
    /* 3 transmissions with 20ms interval */
    .net_transmit = ESP_BLE_MESH_TRANSMIT(2, 20),
    .relay = ESP_BLE_MESH_RELAY_DISABLED,
    .relay_retransmit = ESP_BLE_MESH_TRANSMIT(2, 20),
    .beacon = ESP_BLE_MESH_BEACON_ENABLED,
#if defined(CONFIG_BLE_MESH_GATT_PROXY_SERVER)
    .gatt_proxy = ESP_BLE_MESH_GATT_PROXY_ENABLED,
#else
    .gatt_proxy = ESP_BLE_MESH_GATT_PROXY_NOT_SUPPORTED,
#endif
#if defined(CONFIG_BLE_MESH_FRIEND)
    .friend_state = ESP_BLE_MESH_FRIEND_ENABLED,
#else
    .friend_state = ESP_BLE_MESH_FRIEND_NOT_SUPPORTED,
#endif
    .default_ttl = 7,
};

static esp_ble_mesh_model_t root_models[] = {
    ESP_BLE_MESH_MODEL_CFG_SRV(&config_server),
    ESP_BLE_MESH_MODEL_CFG_CLI(&config_client),
    ESP_BLE_MESH_MODEL_GEN_ONOFF_CLI(NULL, &onoff_client),
};

static esp_ble_mesh_elem_t elements[] = {
    ESP_BLE_MESH_ELEMENT(0, root_models, ESP_BLE_MESH_MODEL_NONE),
};

static esp_ble_mesh_comp_t composition = {
    .cid = CID_ESP,
    .element_count = ARRAY_SIZE(elements),
    .elements = elements,
};

static esp_ble_mesh_prov_t provision = {
    .prov_uuid           = dev_uuid,
    .prov_unicast_addr   = PROV_OWN_ADDR,
    .prov_start_address  = 0x0005,
    .prov_attention      = 0x00,
    .prov_algorithm      = 0x00,
    .prov_pub_key_oob    = 0x00,
    .prov_static_oob_val = NULL,
    .prov_static_oob_len = 0x00,
    .flags               = 0x00,
    .iv_index            = 0x00,
};

static esp_err_t example_ble_mesh_store_node_info(const uint8_t uuid[16], uint16_t unicast,
                                                  uint8_t elem_num, uint8_t onoff_state)
{
    int i;

    if (!uuid || !ESP_BLE_MESH_ADDR_IS_UNICAST(unicast)) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Judge if the device has been provisioned before */
    for (i = 0; i < ARRAY_SIZE(nodes); i++) {
        if (!memcmp(nodes[i].uuid, uuid, 16)) {
            ESP_LOGW(TAG, "%s: reprovisioned device 0x%04x", __func__, unicast);
            nodes[i].unicast = unicast;
            nodes[i].elem_num = elem_num;
            nodes[i].onoff = onoff_state;
            farmely_store_nodes();
            return ESP_OK;
        }
    }

    for (i = 0; i < ARRAY_SIZE(nodes); i++) {
        if (nodes[i].unicast == ESP_BLE_MESH_ADDR_UNASSIGNED) {
            memcpy(nodes[i].uuid, uuid, 16);
            nodes[i].unicast = unicast;
            nodes[i].elem_num = elem_num;
            nodes[i].onoff = onoff_state;
            farmely_store_nodes();
            return ESP_OK;
        }
    }

    return ESP_FAIL;
}

static esp_ble_mesh_node_info_t *example_ble_mesh_get_node_info(uint16_t unicast)
{
    int i;

    if (!ESP_BLE_MESH_ADDR_IS_UNICAST(unicast)) {
        return NULL;
    }

    for (i = 0; i < ARRAY_SIZE(nodes); i++) {
        if (nodes[i].unicast <= unicast &&
                nodes[i].unicast + nodes[i].elem_num > unicast) {
            return &nodes[i];
        }
    }

    return NULL;
}

static void farmely_node_device_id(const esp_ble_mesh_node_info_t *node,
                                   char *out, size_t size)
{
    snprintf(out, size, "node-%02X%02X%02X",
             node->uuid[5], node->uuid[6], node->uuid[7]);
}

static esp_err_t example_ble_mesh_set_msg_common(esp_ble_mesh_client_common_param_t *common,
                                                 esp_ble_mesh_node_info_t *node,
                                                 esp_ble_mesh_model_t *model, uint32_t opcode)
{
    if (!common || !node || !model) {
        return ESP_ERR_INVALID_ARG;
    }

    common->opcode = opcode;
    common->model = model;
    common->ctx.net_idx = prov_key.net_idx;
    common->ctx.app_idx = prov_key.app_idx;
    common->ctx.addr = node->unicast;
    common->ctx.send_ttl = MSG_SEND_TTL;
    common->msg_timeout = MSG_TIMEOUT;
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 2, 0)
    common->msg_role = MSG_ROLE;
#endif

    return ESP_OK;
}

static esp_err_t farmely_send_get(esp_ble_mesh_node_info_t *node)
{
    esp_ble_mesh_client_common_param_t common = {0};
    esp_ble_mesh_generic_client_get_state_t get_state = {0};

    example_ble_mesh_set_msg_common(&common, node, onoff_client.model,
                                    ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_GET);
    return esp_ble_mesh_generic_client_get_state(&common, &get_state);
}

static esp_err_t farmely_start_get(esp_ble_mesh_node_info_t *node)
{
    size_t index = node - nodes;

    if (index >= ARRAY_SIZE(nodes)) {
        return ESP_ERR_INVALID_ARG;
    }
    s_get_retries[index] = 0;
    return farmely_send_get(node);
}

static void farmely_reset_config_retries(esp_ble_mesh_node_info_t *node)
{
    size_t index = node - nodes;

    if (index < ARRAY_SIZE(s_config_retries)) {
        s_config_retries[index] = 0;
    }
}

static bool farmely_take_config_retry(esp_ble_mesh_node_info_t *node,
                                      const char *operation)
{
    size_t index = node - nodes;

    if (index >= ARRAY_SIZE(s_config_retries)) {
        return false;
    }
    if (s_config_retries[index] >= FARMELY_MAX_RETRIES) {
        ESP_LOGE(TAG, "%s timed out for 0x%04x", operation, node->unicast);
        s_config_retries[index] = 0;
        return false;
    }
    s_config_retries[index]++;
    return true;
}

static esp_err_t farmely_send_group_subscription(
    esp_ble_mesh_node_info_t *node)
{
    esp_ble_mesh_client_common_param_t common = {0};
    esp_ble_mesh_cfg_client_set_state_t set_state = {0};

    example_ble_mesh_set_msg_common(
        &common, node, config_client.model,
        ESP_BLE_MESH_MODEL_OP_MODEL_SUB_ADD);
    set_state.model_sub_add.element_addr = node->unicast;
    set_state.model_sub_add.sub_addr = FARMELY_GROUP_ADDR;
    set_state.model_sub_add.model_id =
        ESP_BLE_MESH_MODEL_ID_GEN_ONOFF_SRV;
    set_state.model_sub_add.company_id = ESP_BLE_MESH_CID_NVAL;
    return esp_ble_mesh_config_client_set_state(&common, &set_state);
}

static esp_err_t farmely_start_group_subscription(
    esp_ble_mesh_node_info_t *node)
{
    size_t index = node - nodes;

    if (index >= ARRAY_SIZE(nodes)) {
        return ESP_ERR_INVALID_ARG;
    }
    farmely_reset_config_retries(node);
    return farmely_send_group_subscription(node);
}

static void farmely_clear_pending(size_t index)
{
    if (index < ARRAY_SIZE(s_pending)) {
        memset(&s_pending[index], 0, sizeof(s_pending[index]));
    }
}

static void farmely_clear_all_pending(void)
{
    for (size_t i = 0; i < ARRAY_SIZE(s_pending); i++) {
        farmely_clear_pending(i);
    }
}

static esp_err_t farmely_send_onoff_request(esp_ble_mesh_node_info_t *node,
                                            bool on)
{
    esp_ble_mesh_client_common_param_t common = {0};
    esp_ble_mesh_generic_client_set_state_t set_state = {0};

    example_ble_mesh_set_msg_common(&common, node, onoff_client.model,
                                    ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_SET);
    set_state.onoff_set.op_en = false;
    set_state.onoff_set.onoff = on;
    set_state.onoff_set.tid = s_tid++;
    return esp_ble_mesh_generic_client_set_state(&common, &set_state);
}

static esp_err_t farmely_send_onoff(esp_ble_mesh_node_info_t *node, bool on,
                                    const char *message_id)
{
    size_t index = node - nodes;

    if (index >= ARRAY_SIZE(nodes)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_pending[index].in_flight) {
        return ESP_ERR_INVALID_STATE;
    }

    s_pending[index].in_flight = true;
    s_pending[index].desired_onoff = on;
    strlcpy(s_pending[index].message_id, message_id ? message_id : "",
            sizeof(s_pending[index].message_id));

    esp_err_t err = farmely_send_onoff_request(node, on);
    if (err != ESP_OK) {
        farmely_clear_pending(index);
    }
    return err;
}

static void farmely_group_sync_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(500));

    for (size_t i = 0; i < ARRAY_SIZE(nodes); i++) {
        if (!s_pending[i].in_flight ||
            !ESP_BLE_MESH_ADDR_IS_UNICAST(nodes[i].unicast)) {
            continue;
        }
        if (farmely_send_get(&nodes[i]) != ESP_OK) {
            farmely_clear_pending(i);
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    vTaskDelete(NULL);
}

static esp_err_t farmely_send_group_onoff(bool on, const char *message_id)
{
    esp_ble_mesh_client_common_param_t common = {0};
    esp_ble_mesh_generic_client_set_state_t set_state = {0};
    size_t node_count = 0;

    for (size_t i = 0; i < ARRAY_SIZE(nodes); i++) {
        if (!ESP_BLE_MESH_ADDR_IS_UNICAST(nodes[i].unicast)) {
            continue;
        }
        if (s_pending[i].in_flight) {
            return ESP_ERR_INVALID_STATE;
        }
        node_count++;
    }
    if (node_count == 0) {
        return ESP_ERR_NOT_FOUND;
    }

    for (size_t i = 0; i < ARRAY_SIZE(nodes); i++) {
        if (!ESP_BLE_MESH_ADDR_IS_UNICAST(nodes[i].unicast)) {
            continue;
        }
        s_pending[i].in_flight = true;
        s_pending[i].desired_onoff = on;
        strlcpy(s_pending[i].message_id, message_id ? message_id : "",
                sizeof(s_pending[i].message_id));
    }

    common.opcode = ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_SET_UNACK;
    common.model = onoff_client.model;
    common.ctx.net_idx = prov_key.net_idx;
    common.ctx.app_idx = prov_key.app_idx;
    common.ctx.addr = FARMELY_GROUP_ADDR;
    common.ctx.send_ttl = MSG_SEND_TTL;
    common.msg_timeout = MSG_TIMEOUT;
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 2, 0)
    common.msg_role = MSG_ROLE;
#endif
    set_state.onoff_set.op_en = false;
    set_state.onoff_set.onoff = on;
    set_state.onoff_set.tid = s_tid++;

    if (xTaskCreate(farmely_group_sync_task, "mesh_group_sync", 3072,
                    NULL, 5, NULL) != pdPASS) {
        farmely_clear_all_pending();
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_ble_mesh_generic_client_set_state(&common, &set_state);
    if (err != ESP_OK) {
        farmely_clear_all_pending();
    }
    return err;
}

static esp_err_t farmely_bridge_command(const char *device_id, bool on,
                                        const char *message_id, void *ctx)
{
    (void)ctx;

    if (!strcmp(device_id, "*")) {
        return farmely_send_group_onoff(on, message_id);
    }

    for (size_t i = 0; i < ARRAY_SIZE(nodes); i++) {
        if (!ESP_BLE_MESH_ADDR_IS_UNICAST(nodes[i].unicast)) {
            continue;
        }
        char current_id[32];
        farmely_node_device_id(&nodes[i], current_id, sizeof(current_id));
        if (strcmp(device_id, current_id)) {
            continue;
        }
        return farmely_send_onoff(&nodes[i], on, message_id);
    }

    return ESP_ERR_NOT_FOUND;
}

static void farmely_bridge_sync(void *ctx)
{
    (void)ctx;
    size_t node_count = 0;

    for (size_t i = 0; i < ARRAY_SIZE(nodes); i++) {
        if (!ESP_BLE_MESH_ADDR_IS_UNICAST(nodes[i].unicast)) {
            continue;
        }
        node_count++;
        farmely_start_get(&nodes[i]);
    }
    farmely_gateway_bridge_publish_gateway_status(node_count);
}

static void farmely_recover_nodes_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(3000));
    for (size_t i = 0; i < ARRAY_SIZE(nodes); i++) {
        if (ESP_BLE_MESH_ADDR_IS_UNICAST(nodes[i].unicast)) {
            if (farmely_start_group_subscription(&nodes[i]) != ESP_OK) {
                farmely_start_get(&nodes[i]);
            }
            vTaskDelay(pdMS_TO_TICKS(250));
        }
    }
    vTaskDelete(NULL);
}

/* Called from gateway_bridge.c around the SoftAP provisioning window.
 * Disabling both Provisioner bearers also disables the BLE Mesh scan
 * (see bt_mesh_provisioner_disable), which hands the shared 2.4 GHz radio
 * to the SoftAP while the phone associates and provisions. Mesh control is
 * unavailable during the window and recovers right after. */
void farmely_mesh_yield_radio(bool yield)
{
    esp_err_t err;

    if (yield) {
        err = esp_ble_mesh_provisioner_prov_disable(
            (esp_ble_mesh_prov_bearer_t)(ESP_BLE_MESH_PROV_ADV |
                                         ESP_BLE_MESH_PROV_GATT));
        ESP_LOGI(TAG, "BLE Mesh yielded radio to SoftAP provisioning (err %d)",
                 err);
    } else {
        err = esp_ble_mesh_provisioner_prov_enable(
            (esp_ble_mesh_prov_bearer_t)(ESP_BLE_MESH_PROV_ADV |
                                         ESP_BLE_MESH_PROV_GATT));
        ESP_LOGI(TAG, "BLE Mesh radio resumed after provisioning (err %d)",
                 err);
        if (err == ESP_OK &&
            xTaskCreate(farmely_recover_nodes_task, "mesh_recover", 3072,
                        NULL, 5, NULL) != pdPASS) {
            ESP_LOGW(TAG, "Unable to restart Mesh recovery task");
        }
    }
}

#if CONFIG_FARMELY_BENCH_SELF_TEST
static bool farmely_wait_status_update(size_t index, uint32_t version,
                                       TickType_t timeout)
{
    TickType_t start = xTaskGetTickCount();

    while (s_status_version[index] == version) {
        if (xTaskGetTickCount() - start >= timeout) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return true;
}

static bool farmely_wait_pending_clear(size_t index, TickType_t timeout)
{
    TickType_t start = xTaskGetTickCount();

    while (s_pending[index].in_flight) {
        if (xTaskGetTickCount() - start >= timeout) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return true;
}

static void farmely_bench_self_test_task(void *arg)
{
    (void)arg;
    size_t active[CONFIG_BLE_MESH_MAX_PROV_NODES];
    bool initial_onoff[CONFIG_BLE_MESH_MAX_PROV_NODES];
    size_t active_count = 0;
    unsigned group_passed = 0;
    unsigned directed_passed = 0;

    vTaskDelay(pdMS_TO_TICKS(7000));
    for (size_t i = 0; i < ARRAY_SIZE(nodes); i++) {
        if (!ESP_BLE_MESH_ADDR_IS_UNICAST(nodes[i].unicast)) {
            continue;
        }
        uint32_t version = s_status_version[i];
        if (farmely_start_get(&nodes[i]) == ESP_OK &&
            farmely_wait_status_update(i, version, pdMS_TO_TICKS(3000))) {
            initial_onoff[active_count] = nodes[i].onoff;
            active[active_count++] = i;
        }
    }
    if (active_count < 2) {
        ESP_LOGE(TAG, "Bench self-test requires at least two online nodes");
        vTaskDelete(NULL);
        return;
    }

    for (unsigned round = 0; round < 20; round++) {
        bool desired = (round & 1U) != 0;
        bool passed = farmely_send_group_onoff(desired, NULL) == ESP_OK;

        for (size_t i = 0; i < active_count; i++) {
            size_t index = active[i];
            passed = farmely_wait_pending_clear(
                         index, pdMS_TO_TICKS(5000)) &&
                     nodes[index].onoff == desired && passed;
        }
        if (passed) {
            group_passed++;
        } else {
            ESP_LOGE(TAG, "Bench group round %u failed", round + 1);
        }
        vTaskDelay(pdMS_TO_TICKS(150));
    }

    for (unsigned command = 0; command < 50; command++) {
        size_t target_pos = command % active_count;
        size_t target = active[target_pos];
        bool previous[CONFIG_BLE_MESH_MAX_PROV_NODES] = {0};
        bool desired = !nodes[target].onoff;
        bool passed;

        for (size_t i = 0; i < active_count; i++) {
            previous[i] = nodes[active[i]].onoff;
        }
        passed = farmely_send_onoff(&nodes[target], desired, NULL) == ESP_OK &&
                 farmely_wait_pending_clear(target, pdMS_TO_TICKS(3000)) &&
                 nodes[target].onoff == desired;
        for (size_t i = 0; i < active_count; i++) {
            if (i == target_pos) {
                continue;
            }
            size_t index = active[i];
            uint32_t version = s_status_version[index];
            if (farmely_start_get(&nodes[index]) != ESP_OK ||
                !farmely_wait_status_update(
                    index, version, pdMS_TO_TICKS(3000)) ||
                nodes[index].onoff != previous[i]) {
                passed = false;
            }
        }
        if (passed) {
            directed_passed++;
        } else {
            ESP_LOGE(TAG, "Bench directed command %u failed for 0x%04x",
                     command + 1, nodes[target].unicast);
        }
        vTaskDelay(pdMS_TO_TICKS(150));
    }

    ESP_LOGI(TAG,
             "Bench self-test complete: group=%u/20 directed=%u/50",
             group_passed, directed_passed);
    for (size_t i = 0; i < active_count; i++) {
        size_t index = active[i];
        if (nodes[index].onoff != initial_onoff[i] &&
            farmely_send_onoff(&nodes[index], initial_onoff[i], NULL) ==
                ESP_OK) {
            farmely_wait_pending_clear(index, pdMS_TO_TICKS(3000));
        }
    }
    vTaskDelete(NULL);
}
#endif

static esp_err_t prov_complete(int node_idx, const esp_ble_mesh_octet16_t uuid,
                               uint16_t unicast, uint8_t elem_num, uint16_t net_idx)
{
    esp_ble_mesh_client_common_param_t common = {0};
    esp_ble_mesh_cfg_client_get_state_t get_state = {0};
    esp_ble_mesh_node_info_t *node = NULL;
    char name[11] = {0};
    int err;

    ESP_LOGI(TAG, "node index: 0x%x, unicast address: 0x%02x, element num: %d, netkey index: 0x%02x",
             node_idx, unicast, elem_num, net_idx);
    ESP_LOGI(TAG, "device uuid: %s", bt_hex(uuid, 16));

    sprintf(name, "%s%d", "NODE-", node_idx);
    err = esp_ble_mesh_provisioner_set_node_name(node_idx, name);
    if (err) {
        ESP_LOGE(TAG, "%s: Set node name failed", __func__);
        return ESP_FAIL;
    }

    err = example_ble_mesh_store_node_info(uuid, unicast, elem_num, LED_OFF);
    if (err) {
        ESP_LOGE(TAG, "%s: Store node info failed", __func__);
        return ESP_FAIL;
    }
    farmely_store_progress(FARMELY_STAGE_PROVISIONED, unicast, LED_OFF, ESP_OK);

    node = example_ble_mesh_get_node_info(unicast);
    if (!node) {
        ESP_LOGE(TAG, "%s: Get node info failed", __func__);
        return ESP_FAIL;
    }

    example_ble_mesh_set_msg_common(&common, node, config_client.model, ESP_BLE_MESH_MODEL_OP_COMPOSITION_DATA_GET);
    get_state.comp_data_get.page = COMP_DATA_PAGE_0;
    err = esp_ble_mesh_config_client_get_state(&common, &get_state);
    if (err) {
        ESP_LOGE(TAG, "%s: Send config comp data get failed", __func__);
        return ESP_FAIL;
    }

    return ESP_OK;
}

static void prov_link_open(esp_ble_mesh_prov_bearer_t bearer)
{
    ESP_LOGI(TAG, "%s link open", bearer == ESP_BLE_MESH_PROV_ADV ? "PB-ADV" : "PB-GATT");
}

static void prov_link_close(esp_ble_mesh_prov_bearer_t bearer, uint8_t reason)
{
    ESP_LOGI(TAG, "%s link close, reason 0x%02x",
             bearer == ESP_BLE_MESH_PROV_ADV ? "PB-ADV" : "PB-GATT", reason);
}

static void recv_unprov_adv_pkt(uint8_t dev_uuid[16], uint8_t addr[BD_ADDR_LEN],
                                esp_ble_mesh_addr_type_t addr_type, uint16_t oob_info,
                                uint8_t adv_type, esp_ble_mesh_prov_bearer_t bearer)
{
    esp_ble_mesh_unprov_dev_add_t add_dev = {0};
    int err;

    /* Due to the API esp_ble_mesh_provisioner_set_dev_uuid_match, Provisioner will only
     * use this callback to report the devices, whose device UUID starts with 0xdd & 0xdd,
     * to the application layer.
     */

    ESP_LOGI(TAG, "address: %s, address type: %d, adv type: %d", bt_hex(addr, BD_ADDR_LEN), addr_type, adv_type);
    ESP_LOGI(TAG, "device uuid: %s", bt_hex(dev_uuid, 16));
    ESP_LOGI(TAG, "oob info: %d, bearer: %s", oob_info, (bearer & ESP_BLE_MESH_PROV_ADV) ? "PB-ADV" : "PB-GATT");
    farmely_store_progress(FARMELY_STAGE_NODE_FOUND,
                           ESP_BLE_MESH_ADDR_UNASSIGNED, LED_OFF, ESP_OK);

    memcpy(add_dev.addr, addr, BD_ADDR_LEN);
    add_dev.addr_type = (esp_ble_mesh_addr_type_t)addr_type;
    memcpy(add_dev.uuid, dev_uuid, 16);
    add_dev.oob_info = oob_info;
    add_dev.bearer = (esp_ble_mesh_prov_bearer_t)bearer;
    /* Note: If unprovisioned device adv packets have not been received, we should not add
             device with ADD_DEV_START_PROV_NOW_FLAG set. */
    err = esp_ble_mesh_provisioner_add_unprov_dev(&add_dev,
            (esp_ble_mesh_dev_add_flag_t)(ADD_DEV_RM_AFTER_PROV_FLAG | ADD_DEV_START_PROV_NOW_FLAG | ADD_DEV_FLUSHABLE_DEV_FLAG));
    if (err) {
        ESP_LOGE(TAG, "%s: Add unprovisioned device into queue failed", __func__);
    }

    return;
}

static void example_ble_mesh_provisioning_cb(esp_ble_mesh_prov_cb_event_t event,
                                             esp_ble_mesh_prov_cb_param_t *param)
{
    switch (event) {
    case ESP_BLE_MESH_PROVISIONER_PROV_ENABLE_COMP_EVT:
        ESP_LOGI(TAG, "ESP_BLE_MESH_PROVISIONER_PROV_ENABLE_COMP_EVT, err_code %d", param->provisioner_prov_enable_comp.err_code);
        break;
    case ESP_BLE_MESH_PROVISIONER_PROV_DISABLE_COMP_EVT:
        ESP_LOGI(TAG, "ESP_BLE_MESH_PROVISIONER_PROV_DISABLE_COMP_EVT, err_code %d", param->provisioner_prov_disable_comp.err_code);
        break;
    case ESP_BLE_MESH_PROVISIONER_RECV_UNPROV_ADV_PKT_EVT:
        ESP_LOGI(TAG, "ESP_BLE_MESH_PROVISIONER_RECV_UNPROV_ADV_PKT_EVT");
        recv_unprov_adv_pkt(param->provisioner_recv_unprov_adv_pkt.dev_uuid, param->provisioner_recv_unprov_adv_pkt.addr,
                            param->provisioner_recv_unprov_adv_pkt.addr_type, param->provisioner_recv_unprov_adv_pkt.oob_info,
                            param->provisioner_recv_unprov_adv_pkt.adv_type, param->provisioner_recv_unprov_adv_pkt.bearer);
        break;
    case ESP_BLE_MESH_PROVISIONER_PROV_LINK_OPEN_EVT:
        prov_link_open(param->provisioner_prov_link_open.bearer);
        break;
    case ESP_BLE_MESH_PROVISIONER_PROV_LINK_CLOSE_EVT:
        prov_link_close(param->provisioner_prov_link_close.bearer, param->provisioner_prov_link_close.reason);
        break;
    case ESP_BLE_MESH_PROVISIONER_PROV_COMPLETE_EVT:
        prov_complete(param->provisioner_prov_complete.node_idx, param->provisioner_prov_complete.device_uuid,
                      param->provisioner_prov_complete.unicast_addr, param->provisioner_prov_complete.element_num,
                      param->provisioner_prov_complete.netkey_idx);
        break;
    case ESP_BLE_MESH_PROVISIONER_ADD_UNPROV_DEV_COMP_EVT:
        ESP_LOGI(TAG, "ESP_BLE_MESH_PROVISIONER_ADD_UNPROV_DEV_COMP_EVT, err_code %d", param->provisioner_add_unprov_dev_comp.err_code);
        break;
    case ESP_BLE_MESH_PROVISIONER_SET_DEV_UUID_MATCH_COMP_EVT:
        ESP_LOGI(TAG, "ESP_BLE_MESH_PROVISIONER_SET_DEV_UUID_MATCH_COMP_EVT, err_code %d", param->provisioner_set_dev_uuid_match_comp.err_code);
        break;
    case ESP_BLE_MESH_PROVISIONER_SET_NODE_NAME_COMP_EVT: {
        ESP_LOGI(TAG, "ESP_BLE_MESH_PROVISIONER_SET_NODE_NAME_COMP_EVT, err_code %d", param->provisioner_set_node_name_comp.err_code);
        if (param->provisioner_set_node_name_comp.err_code == ESP_OK) {
            const char *name = NULL;
            name = esp_ble_mesh_provisioner_get_node_name(param->provisioner_set_node_name_comp.node_index);
            if (!name) {
                ESP_LOGE(TAG, "Get node name failed");
                return;
            }
            ESP_LOGI(TAG, "Node %d name is: %s", param->provisioner_set_node_name_comp.node_index, name);
        }
        break;
    }
    case ESP_BLE_MESH_PROVISIONER_ADD_LOCAL_APP_KEY_COMP_EVT: {
        ESP_LOGI(TAG, "ESP_BLE_MESH_PROVISIONER_ADD_LOCAL_APP_KEY_COMP_EVT, err_code %d", param->provisioner_add_app_key_comp.err_code);
        if (param->provisioner_add_app_key_comp.err_code == ESP_OK) {
            esp_err_t err = 0;
            prov_key.app_idx = param->provisioner_add_app_key_comp.app_idx;
            err = esp_ble_mesh_provisioner_bind_app_key_to_local_model(PROV_OWN_ADDR, prov_key.app_idx,
                    ESP_BLE_MESH_MODEL_ID_GEN_ONOFF_CLI, ESP_BLE_MESH_CID_NVAL);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Provisioner bind local model appkey failed");
                return;
            }
        }
        break;
    }
    case ESP_BLE_MESH_PROVISIONER_BIND_APP_KEY_TO_MODEL_COMP_EVT:
        ESP_LOGI(TAG, "ESP_BLE_MESH_PROVISIONER_BIND_APP_KEY_TO_MODEL_COMP_EVT, err_code %d", param->provisioner_bind_app_key_to_model_comp.err_code);
        break;
    default:
        break;
    }

    return;
}

static void example_ble_mesh_config_client_cb(esp_ble_mesh_cfg_client_cb_event_t event,
                                              esp_ble_mesh_cfg_client_cb_param_t *param)
{
    esp_ble_mesh_client_common_param_t common = {0};
    esp_ble_mesh_node_info_t *node = NULL;
    uint32_t opcode;
    uint16_t addr;
    int err;

    opcode = param->params->opcode;
    addr = param->params->ctx.addr;

    ESP_LOGI(TAG, "%s, error_code = 0x%02x, event = 0x%02x, addr: 0x%04x, opcode: 0x%04" PRIx32,
             __func__, param->error_code, event, param->params->ctx.addr, opcode);

    node = example_ble_mesh_get_node_info(addr);
    if (param->error_code) {
        ESP_LOGE(TAG, "Send config client message failed, opcode 0x%04" PRIx32, opcode);
        farmely_store_progress(s_progress_stage, addr, node ? node->onoff : LED_OFF,
                               param->error_code);
        return;
    }

    if (!node) {
        ESP_LOGE(TAG, "%s: Get node info failed", __func__);
        return;
    }

    switch (event) {
    case ESP_BLE_MESH_CFG_CLIENT_GET_STATE_EVT:
        switch (opcode) {
        case ESP_BLE_MESH_MODEL_OP_COMPOSITION_DATA_GET: {
            farmely_reset_config_retries(node);
            farmely_store_progress(FARMELY_STAGE_COMPOSITION_RECEIVED,
                                   addr, node->onoff, ESP_OK);
            ESP_LOGI(TAG, "composition data %s", bt_hex(param->status_cb.comp_data_status.composition_data->data,
                     param->status_cb.comp_data_status.composition_data->len));
            esp_ble_mesh_cfg_client_set_state_t set_state = {0};
            example_ble_mesh_set_msg_common(&common, node, config_client.model, ESP_BLE_MESH_MODEL_OP_APP_KEY_ADD);
            set_state.app_key_add.net_idx = prov_key.net_idx;
            set_state.app_key_add.app_idx = prov_key.app_idx;
            memcpy(set_state.app_key_add.app_key, prov_key.app_key, 16);
            err = esp_ble_mesh_config_client_set_state(&common, &set_state);
            if (err) {
                ESP_LOGE(TAG, "%s: Config AppKey Add failed", __func__);
                return;
            }
            break;
        }
        default:
            break;
        }
        break;
    case ESP_BLE_MESH_CFG_CLIENT_SET_STATE_EVT:
        switch (opcode) {
        case ESP_BLE_MESH_MODEL_OP_APP_KEY_ADD: {
            if (param->status_cb.appkey_status.status) {
                ESP_LOGE(TAG, "AppKey Add status 0x%02x",
                         param->status_cb.appkey_status.status);
                farmely_store_progress(
                    s_progress_stage, addr, node->onoff,
                    param->status_cb.appkey_status.status);
                break;
            }
            farmely_reset_config_retries(node);
            farmely_store_progress(FARMELY_STAGE_APP_KEY_ADDED,
                                   addr, node->onoff, ESP_OK);
            esp_ble_mesh_cfg_client_set_state_t set_state = {0};
            example_ble_mesh_set_msg_common(&common, node, config_client.model, ESP_BLE_MESH_MODEL_OP_MODEL_APP_BIND);
            set_state.model_app_bind.element_addr = node->unicast;
            set_state.model_app_bind.model_app_idx = prov_key.app_idx;
            set_state.model_app_bind.model_id = ESP_BLE_MESH_MODEL_ID_GEN_ONOFF_SRV;
            set_state.model_app_bind.company_id = ESP_BLE_MESH_CID_NVAL;
            err = esp_ble_mesh_config_client_set_state(&common, &set_state);
            if (err) {
                ESP_LOGE(TAG, "%s: Config Model App Bind failed", __func__);
                return;
            }
            break;
        }
        case ESP_BLE_MESH_MODEL_OP_MODEL_APP_BIND: {
            if (param->status_cb.model_app_status.status) {
                ESP_LOGE(TAG, "Model App Bind status 0x%02x",
                         param->status_cb.model_app_status.status);
                farmely_store_progress(
                    s_progress_stage, addr, node->onoff,
                    param->status_cb.model_app_status.status);
                break;
            }
            farmely_reset_config_retries(node);
            farmely_store_progress(FARMELY_STAGE_MODEL_BOUND,
                                   addr, node->onoff, ESP_OK);
            s_toggle_after_get_addr = node->unicast;
            err = farmely_start_group_subscription(node);
            if (err) {
                ESP_LOGE(TAG, "%s: Config Model Subscription Add failed",
                         __func__);
                return;
            }
            break;
        }
        case ESP_BLE_MESH_MODEL_OP_MODEL_SUB_ADD:
            if (param->status_cb.model_sub_status.status) {
                ESP_LOGE(TAG, "Model Subscription Add status 0x%02x",
                         param->status_cb.model_sub_status.status);
                farmely_store_progress(
                    s_progress_stage, addr, node->onoff,
                    param->status_cb.model_sub_status.status);
                break;
            }
            farmely_reset_config_retries(node);
            ESP_LOGI(TAG, "Node 0x%04x subscribed to group 0x%04x",
                     node->unicast, FARMELY_GROUP_ADDR);
            err = farmely_start_get(node);
            if (err) {
                ESP_LOGE(TAG, "%s: Generic OnOff Get failed", __func__);
                return;
            }
            break;
        default:
            break;
        }
        break;
    case ESP_BLE_MESH_CFG_CLIENT_PUBLISH_EVT:
        switch (opcode) {
        case ESP_BLE_MESH_MODEL_OP_COMPOSITION_DATA_STATUS:
            ESP_LOG_BUFFER_HEX("composition data %s", param->status_cb.comp_data_status.composition_data->data,
                               param->status_cb.comp_data_status.composition_data->len);
            break;
        case ESP_BLE_MESH_MODEL_OP_APP_KEY_STATUS:
            break;
        default:
            break;
        }
        break;
    case ESP_BLE_MESH_CFG_CLIENT_TIMEOUT_EVT:
        switch (opcode) {
        case ESP_BLE_MESH_MODEL_OP_COMPOSITION_DATA_GET: {
            if (!farmely_take_config_retry(node, "Composition Data Get")) {
                break;
            }
            esp_ble_mesh_cfg_client_get_state_t get_state = {0};
            example_ble_mesh_set_msg_common(&common, node, config_client.model, ESP_BLE_MESH_MODEL_OP_COMPOSITION_DATA_GET);
            get_state.comp_data_get.page = COMP_DATA_PAGE_0;
            err = esp_ble_mesh_config_client_get_state(&common, &get_state);
            if (err) {
                ESP_LOGE(TAG, "%s: Config Composition Data Get failed", __func__);
                return;
            }
            break;
        }
        case ESP_BLE_MESH_MODEL_OP_APP_KEY_ADD: {
            if (!farmely_take_config_retry(node, "AppKey Add")) {
                break;
            }
            esp_ble_mesh_cfg_client_set_state_t set_state = {0};
            example_ble_mesh_set_msg_common(&common, node, config_client.model, ESP_BLE_MESH_MODEL_OP_APP_KEY_ADD);
            set_state.app_key_add.net_idx = prov_key.net_idx;
            set_state.app_key_add.app_idx = prov_key.app_idx;
            memcpy(set_state.app_key_add.app_key, prov_key.app_key, 16);
            err = esp_ble_mesh_config_client_set_state(&common, &set_state);
            if (err) {
                ESP_LOGE(TAG, "%s: Config AppKey Add failed", __func__);
                return;
            }
            break;
        }
        case ESP_BLE_MESH_MODEL_OP_MODEL_APP_BIND: {
            if (!farmely_take_config_retry(node, "Model App Bind")) {
                break;
            }
            esp_ble_mesh_cfg_client_set_state_t set_state = {0};
            example_ble_mesh_set_msg_common(&common, node, config_client.model, ESP_BLE_MESH_MODEL_OP_MODEL_APP_BIND);
            set_state.model_app_bind.element_addr = node->unicast;
            set_state.model_app_bind.model_app_idx = prov_key.app_idx;
            set_state.model_app_bind.model_id = ESP_BLE_MESH_MODEL_ID_GEN_ONOFF_SRV;
            set_state.model_app_bind.company_id = ESP_BLE_MESH_CID_NVAL;
            err = esp_ble_mesh_config_client_set_state(&common, &set_state);
            if (err) {
                ESP_LOGE(TAG, "%s: Config Model App Bind failed", __func__);
                return;
            }
            break;
        }
        case ESP_BLE_MESH_MODEL_OP_MODEL_SUB_ADD: {
            if (!farmely_take_config_retry(
                    node, "Model Subscription Add")) {
                break;
            }
            err = farmely_send_group_subscription(node);
            if (err) {
                ESP_LOGE(TAG, "%s: Config Model Subscription Add failed",
                         __func__);
                return;
            }
            break;
        }
        default:
            break;
        }
        break;
    default:
        ESP_LOGE(TAG, "Not a config client status message event");
        break;
    }
}

static void example_ble_mesh_generic_client_cb(esp_ble_mesh_generic_client_cb_event_t event,
                                               esp_ble_mesh_generic_client_cb_param_t *param)
{
    esp_ble_mesh_node_info_t *node = NULL;
    uint32_t opcode;
    uint16_t addr;
    int err;

    opcode = param->params->opcode;
    addr = param->params->ctx.addr;

    ESP_LOGI(TAG, "%s, error_code = 0x%02x, event = 0x%02x, addr: 0x%04x, opcode: 0x%04" PRIx32,
             __func__, param->error_code, event, param->params->ctx.addr, opcode);

    if (opcode == ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_SET_UNACK &&
        addr == FARMELY_GROUP_ADDR) {
        if (param->error_code) {
            ESP_LOGE(TAG, "Group Generic OnOff Set failed, error 0x%02x",
                     param->error_code);
        }
        return;
    }

    node = example_ble_mesh_get_node_info(addr);
    if (param->error_code) {
        ESP_LOGE(TAG, "Send generic client message failed, opcode 0x%04" PRIx32, opcode);
        if (node) {
            size_t index = node - nodes;
            if (index < ARRAY_SIZE(s_pending)) {
                farmely_clear_pending(index);
            }
        }
        farmely_store_progress(s_progress_stage, addr, node ? node->onoff : LED_OFF,
                               param->error_code);
        return;
    }

    if (!node) {
        ESP_LOGE(TAG, "%s: Get node info failed", __func__);
        return;
    }

    switch (event) {
    case ESP_BLE_MESH_GENERIC_CLIENT_GET_STATE_EVT:
        switch (opcode) {
        case ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_GET: {
            node->onoff = param->status_cb.onoff_status.present_onoff;
            size_t index = node - nodes;
            if (index < ARRAY_SIZE(s_status_version)) {
                s_status_version[index]++;
                s_get_retries[index] = 0;
            }
            farmely_store_nodes();
            farmely_store_progress(
                s_toggle_after_get_addr == addr ? FARMELY_STAGE_ONOFF_RECEIVED
                                                : FARMELY_STAGE_NODE_RECOVERED,
                addr, node->onoff, ESP_OK);
            ESP_LOGI(TAG, "ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_GET onoff: 0x%02x", node->onoff);
            char device_id[32];
            farmely_node_device_id(node, device_id, sizeof(device_id));
            farmely_gateway_bridge_publish_node_status(
                device_id, node->onoff == LED_ON, true);
            if (index < ARRAY_SIZE(s_pending) && s_pending[index].in_flight) {
                if (s_pending[index].desired_onoff == node->onoff) {
                    farmely_gateway_bridge_publish_ack(
                        device_id, s_pending[index].message_id);
                    farmely_clear_pending(index);
                } else {
                    s_pending[index].retries = 0;
                    err = farmely_send_onoff_request(
                        node, s_pending[index].desired_onoff);
                    if (err) {
                        farmely_clear_pending(index);
                    }
                }
            }
            if (s_toggle_after_get_addr == addr) {
                s_toggle_after_get_addr = ESP_BLE_MESH_ADDR_UNASSIGNED;
                err = farmely_send_onoff(node, !node->onoff, NULL);
                if (err) {
                    ESP_LOGE(TAG, "%s: Generic OnOff Set failed", __func__);
                    return;
                }
            }
            break;
        }
        default:
            break;
        }
        break;
    case ESP_BLE_MESH_GENERIC_CLIENT_SET_STATE_EVT:
        switch (opcode) {
        case ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_SET: {
            node->onoff = param->status_cb.onoff_status.present_onoff;
            size_t index = node - nodes;
            if (index < ARRAY_SIZE(s_status_version)) {
                s_status_version[index]++;
            }
            farmely_store_nodes();
            farmely_store_progress(FARMELY_STAGE_ONOFF_SET,
                                   addr, node->onoff, ESP_OK);
            ESP_LOGI(TAG, "ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_SET onoff: 0x%02x", node->onoff);
            char device_id[32];
            farmely_node_device_id(node, device_id, sizeof(device_id));
            farmely_gateway_bridge_publish_node_status(
                device_id, node->onoff == LED_ON, true);
            if (index < ARRAY_SIZE(s_pending) &&
                s_pending[index].in_flight) {
                if (s_pending[index].desired_onoff == node->onoff) {
                    farmely_gateway_bridge_publish_ack(
                        device_id, s_pending[index].message_id);
                    farmely_clear_pending(index);
                } else if (s_pending[index].retries >=
                           FARMELY_MAX_RETRIES) {
                    ESP_LOGE(TAG,
                             "Generic OnOff Set status mismatch for 0x%04x",
                             node->unicast);
                    farmely_clear_pending(index);
                } else {
                    s_pending[index].retries++;
                    err = farmely_send_onoff_request(
                        node, s_pending[index].desired_onoff);
                    if (err) {
                        farmely_clear_pending(index);
                    }
                }
            }
            break;
        }
        default:
            break;
        }
        break;
    case ESP_BLE_MESH_GENERIC_CLIENT_PUBLISH_EVT:
        break;
    case ESP_BLE_MESH_GENERIC_CLIENT_TIMEOUT_EVT:
        /* If failed to receive the responses, these messages will be resend */
        switch (opcode) {
        case ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_GET: {
            size_t index = node - nodes;
            if (index < ARRAY_SIZE(s_pending) && s_pending[index].in_flight) {
                if (s_pending[index].retries >= FARMELY_MAX_RETRIES) {
                    ESP_LOGE(TAG, "Group status verification timed out for 0x%04x",
                             node->unicast);
                    farmely_clear_pending(index);
                    break;
                }
                s_pending[index].retries++;
            } else if (index < ARRAY_SIZE(s_get_retries)) {
                if (s_get_retries[index] >= FARMELY_MAX_RETRIES) {
                    ESP_LOGE(TAG, "Generic OnOff Get timed out for 0x%04x",
                             node->unicast);
                    s_get_retries[index] = 0;
                    break;
                }
                s_get_retries[index]++;
            }
            err = farmely_send_get(node);
            if (err) {
                if (index < ARRAY_SIZE(s_pending) &&
                    s_pending[index].in_flight) {
                    farmely_clear_pending(index);
                }
                ESP_LOGE(TAG, "%s: Generic OnOff Get failed", __func__);
                return;
            }
            break;
        }
        case ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_SET: {
            size_t index = node - nodes;
            if (index >= ARRAY_SIZE(s_pending) || !s_pending[index].in_flight) {
                break;
            }
            if (s_pending[index].retries >= FARMELY_MAX_RETRIES) {
                ESP_LOGE(TAG, "Generic OnOff Set timed out for 0x%04x",
                         node->unicast);
                farmely_clear_pending(index);
                break;
            }
            s_pending[index].retries++;
            err = farmely_send_onoff_request(
                node, s_pending[index].desired_onoff);
            if (err) {
                farmely_clear_pending(index);
                ESP_LOGE(TAG, "%s: Generic OnOff Set retry failed", __func__);
                return;
            }
            break;
        }
        default:
            break;
        }
        break;
    default:
        ESP_LOGE(TAG, "Not a generic client status message event");
        break;
    }
}

static esp_err_t ble_mesh_init(void)
{
    uint8_t match[2] = {0xdd, 0xdd};
    esp_err_t err = ESP_OK;

    prov_key.net_idx = ESP_BLE_MESH_KEY_PRIMARY;
    prov_key.app_idx = APP_KEY_IDX;
    memset(prov_key.app_key, APP_KEY_OCTET, sizeof(prov_key.app_key));

    esp_ble_mesh_register_prov_callback(example_ble_mesh_provisioning_cb);
    esp_ble_mesh_register_config_client_callback(example_ble_mesh_config_client_cb);
    esp_ble_mesh_register_generic_client_callback(example_ble_mesh_generic_client_cb);

    err = esp_ble_mesh_init(&provision, &composition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize mesh stack (err %d)", err);
        return err;
    }

    err = esp_ble_mesh_provisioner_set_dev_uuid_match(match, sizeof(match), 0x0, false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set matching device uuid (err %d)", err);
        return err;
    }

    err = esp_ble_mesh_provisioner_prov_enable((esp_ble_mesh_prov_bearer_t)(ESP_BLE_MESH_PROV_ADV | ESP_BLE_MESH_PROV_GATT));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable mesh provisioner (err %d)", err);
        return err;
    }

    err = esp_ble_mesh_provisioner_add_local_app_key(prov_key.app_key, prov_key.net_idx, prov_key.app_idx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add local AppKey (err %d)", err);
        return err;
    }

    ESP_LOGI(TAG, "BLE Mesh Provisioner initialized");

    return err;
}

void app_main(void)
{
    esp_err_t err;

    ESP_LOGI(TAG, "Initializing...");

    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    farmely_load_state();
    farmely_store_progress(FARMELY_STAGE_STARTED,
                           PROV_OWN_ADDR, LED_OFF, ESP_OK);

    err = bluetooth_init();
    if (err) {
        ESP_LOGE(TAG, "esp32_bluetooth_init failed (err %d)", err);
        return;
    }

    ble_mesh_get_dev_uuid(dev_uuid);

    /* Initialize the Bluetooth Mesh Subsystem */
    err = ble_mesh_init();
    if (err) {
        ESP_LOGE(TAG, "Bluetooth mesh init failed (err %d)", err);
        return;
    }

    char gateway_id[32];
    snprintf(gateway_id, sizeof(gateway_id), "node-%02X%02X%02X",
             dev_uuid[5], dev_uuid[6], dev_uuid[7]);
    farmely_gateway_bridge_start(gateway_id, farmely_bridge_command,
                                 farmely_bridge_sync, NULL);
    if (xTaskCreate(farmely_recover_nodes_task, "mesh_recover", 3072,
                    NULL, 5, NULL) != pdPASS) {
        ESP_LOGW(TAG, "Unable to start Mesh recovery task");
    }
#if CONFIG_FARMELY_BENCH_SELF_TEST
    if (xTaskCreate(farmely_bench_self_test_task, "mesh_bench_test", 4096,
                    NULL, 5, NULL) != pdPASS) {
        ESP_LOGW(TAG, "Unable to start Mesh bench self-test");
    }
#endif
}
