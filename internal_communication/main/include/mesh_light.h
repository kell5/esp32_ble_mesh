/* Mesh Internal Communication Example — LED driver (dual board support)

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.

   Board support (auto-selected by IDF target):
   - ESP32-S3 (N16R8):  WS2812 addressable RGB LED on GPIO48 via RMT
   - ESP32 (WROOM):     Simple on/off LED on GPIO2 via GPIO output
*/

#ifndef __MESH_LIGHT_H__
#define __MESH_LIGHT_H__

#include "esp_err.h"

/*******************************************************
 *          GPIO Pin Definitions (per board)
 *******************************************************/
#if CONFIG_IDF_TARGET_ESP32S3
  /* ESP32-S3-DevKitC / N16R8: WS2812 RGB LED on GPIO48 */
  #define MESH_LIGHT_RMT_GPIO      48
#else
  /* ESP32-WROOM / DevKitC: onboard blue LED on GPIO2 */
  #define MESH_LIGHT_GPIO          2
#endif

/*******************************************************
 *                Protocol Constants
 *******************************************************/
#define MESH_TOKEN_ID           (0x0)
#define MESH_TOKEN_VALUE        (0xbeef)
#define MESH_CONTROL_CMD        (0x2)
#define MESH_STATUS_CMD         (0x3)   /* node -> root: per-node status report */
#define MESH_OTA_CMD            (0x4)   /* root -> node: OTA request */
#define MESH_OTA_STATUS_CMD     (0x5)   /* node -> root: OTA progress/result */
#define MESH_DEVICE_TYPE_MAX_LEN (24)
#define MESH_PRODUCT_ID_MAX_LEN  (32)
#define MESH_HW_VERSION_MAX_LEN   (32)
#define MESH_FW_VERSION_MAX_LEN   (64)
#define MESH_OTA_URL_MAX_LEN      (256)
#define MESH_OTA_MSG_ID_MAX_LEN   (128)
#define MESH_OTA_DETAIL_MAX_LEN   (96)

/*******************************************************
 *                Structures
 *******************************************************/
typedef struct {
    uint8_t cmd;
    bool on;
    uint8_t token_id;
    uint16_t token_value;
} mesh_light_ctl_t;

/* Status packet a node sends upstream to the root with MESH_DATA_P2P.
   device_type is appended so roots remain compatible with legacy packets that
   contain only the fields before it. */
typedef struct {
    uint8_t cmd;        /* MESH_STATUS_CMD */
    uint8_t on;         /* current on/off state */
    uint8_t mac[6];     /* node STA MAC – identity */
    uint8_t layer;      /* mesh layer */
    uint8_t is_root;    /* 1 if this node is the root */
    char device_type[MESH_DEVICE_TYPE_MAX_LEN];
    char product_id[MESH_PRODUCT_ID_MAX_LEN];
    char hw_version[MESH_HW_VERSION_MAX_LEN];
    char fw_version[MESH_FW_VERSION_MAX_LEN];
} mesh_light_status_t;

typedef struct {
    uint8_t cmd;        /* MESH_OTA_CMD */
    uint8_t reserved;
    uint8_t mac[6];     /* sender identity */
    char url[MESH_OTA_URL_MAX_LEN];
    char sha256[65];
    char fw_version[MESH_FW_VERSION_MAX_LEN];
    char ota_msg_id[MESH_OTA_MSG_ID_MAX_LEN];
    char product_id[MESH_PRODUCT_ID_MAX_LEN];
    char hw_version[MESH_HW_VERSION_MAX_LEN];
} mesh_ota_cmd_t;

typedef struct {
    uint8_t cmd;        /* MESH_OTA_STATUS_CMD */
    uint8_t reserved;
    uint8_t mac[6];     /* sender identity */
    char status[16];
    char fw_version[MESH_FW_VERSION_MAX_LEN];
    char ota_msg_id[MESH_OTA_MSG_ID_MAX_LEN];
    char detail[MESH_OTA_DETAIL_MAX_LEN];
} mesh_ota_status_t;

/*******************************************************
 *                Function Declarations
 *******************************************************/
esp_err_t mesh_light_init(void);
esp_err_t mesh_light_set(int state);   /* state: 0=off, non-zero=on (ESP32); ignored on S3 */
esp_err_t mesh_light_process(mesh_addr_t *from, uint8_t *buf, uint16_t len);
void mesh_connected_indicator(int layer);
void mesh_disconnected_indicator(void);

#endif /* __MESH_LIGHT_H__ */
