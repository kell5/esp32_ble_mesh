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

/*******************************************************
 *                Structures
 *******************************************************/
typedef struct {
    uint8_t cmd;
    bool on;
    uint8_t token_id;
    uint16_t token_value;
} mesh_light_ctl_t;

/* Status packet a node sends upstream to the root (MESH_DATA_TODS).
   Fixed-width fields keep the wire layout unambiguous across nodes. */
typedef struct {
    uint8_t cmd;        /* MESH_STATUS_CMD */
    uint8_t on;         /* current light state: 0 = off, 1 = on */
    uint8_t mac[6];     /* node STA MAC — identity */
    uint8_t layer;      /* mesh layer */
    uint8_t is_root;    /* 1 if this node is the root */
} mesh_light_status_t;

/*******************************************************
 *                Function Declarations
 *******************************************************/
esp_err_t mesh_light_init(void);
esp_err_t mesh_light_set(int state);   /* state: 0=off, non-zero=on (ESP32); ignored on S3 */
esp_err_t mesh_light_process(mesh_addr_t *from, uint8_t *buf, uint16_t len);
void mesh_connected_indicator(int layer);
void mesh_disconnected_indicator(void);

#endif /* __MESH_LIGHT_H__ */
