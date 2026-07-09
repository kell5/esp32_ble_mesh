# 阶段二：摄像头流媒体链路实施方案

> 项目：Mesh_esp32_farmely
> 目标芯片：ESP32-S3（创乐博 ESP32S3-CAM）
> 摄像头：OV3660
> 更新时间：2026-07-08

---

## 〇、方案变更说明（重要，2026-07-08）

原方案设计为 **OV3660 → RTMP 推流 → 云端 SRS → WebRTC/HTTP-FLV → App 播放**。
实测联调后发现该链路在本硬件上**不可行**，已改为 **MJPEG-over-HTTP** 方案。原因如下：

- **ESP32-S3 + OV3660 只能输出 JPEG**，板端没有 H.264 硬件编码器，无法产出 RTMP/WebRTC 生态所需的 H.264 码流。
- 尝试把 MJPEG 封进 RTMP 推给 SRS：SRS 能收到字节（`recv_bytes` 正常增长），但**解析出的 `video_frames=0`、`video=null`**；本地 `ffmpeg/ffprobe` 直接拉这路流报 `Input/output error`。即 **MJPEG-in-RTMP 是非标准封装，SRS 与 ffmpeg 都无法解析**，HTTP-FLV 输出为空，App 自然无画面。
- 因 ffmpeg 连读都读不了这路输入，**服务器侧转码成 H.264 的方案也不成立**。

**结论**：放弃 RTMP/SRS/WebRTC 这条视频链，改用 JPEG 帧原生的 **MJPEG-over-HTTP**：

| 阶段 | 方案 | 状态 |
|------|------|------|
| **A1｜局域网直连** | App 直接拉板子 `http://<板子IP>:81/stream`（multipart MJPEG） | ✅ 已联调通过 |
| **A2｜服务器中继** | 板子把 JPEG 帧推给 ECS 上的 MJPEG 中继，中继对外用 multipart 分发，App 播中继公网地址，实现远程可看 | ✅ 已实现（中继脚本 `server_relay/` + 固件 `app_relay.c`，待部署联调） |

> **MQTT 信令通道保持不变**（门铃事件/挂断控制仍走 MQTT），仅视频传输方式改变。

---

## 一、项目概述

在已有的蓝牙Mesh网关（`internal_communication/`）和 Flutter App（`app/`）基础上，新建 **`camera_stream/`** 项目，实现摄像头流媒体的完整链路：

1. **OV3660 摄像头** → 通过 DVP 并口接入 ESP32-S3
2. **MJPEG HTTP 流** → 浏览器 / App 可直接查看实时画面（`:81/stream`，当前主用视频通道）
3. **MQTT 信令** → 门铃按键触发通知、远程挂断控制
4. **A2 MJPEG 中继** → 板子（`app_relay.c`）推 JPEG 帧到云端中继（`server_relay/`），App 远程播放

最终完成 **"按键→MQTT通知App→App 播放 MJPEG 画面→超时/挂断停止"** 的最小闭环验证。（A1 已达成）

---

## 二、目录结构

```
camera_stream/
├── CMakeLists.txt
├── sdkconfig.defaults
├── main/
│   ├── CMakeLists.txt
│   ├── idf_component.yml
│   ├── Kconfig.projbuild
│   ├── camera_main.c
│   ├── camera_pins.h          ← 复用 camera_pins.h
│   ├── app_camera.c / .h
│   ├── app_wifi.c / .h
│   ├── app_httpd.c / .h       ← MJPEG /stream（当前主视频通道）
│   ├── app_relay.c / .h       ← A2 中继推流：长连接 HTTP POST 推 JPEG 帧（取代已删除的 app_rtmp）
│   ├── app_mqtt.c / .h
│   └── app_doorbell.c / .h
└── README.md
```

---

## 三、引脚定义

使用 `camera_pins.h` 中的 **`CAMERA_MODEL_ESP32S3_EYE`**。

| 引脚功能 | GPIO |
|---------|------|
| PWDN | -1 |
| RESET | -1 |
| XCLK | 15 |
| SIOD (SDA) | 4 |
| SIOC (SCL) | 5 |
| Y9 (D7) | 16 |
| Y8 (D6) | 17 |
| Y7 (D5) | 18 |
| Y6 (D4) | 12 |
| Y5 (D3) | 10 |
| Y4 (D2) | 8 |
| Y3 (D1) | 9 |
| Y2 (D0) | 11 |
| VSYNC | 6 |
| HREF | 7 |
| PCLK | 13 |
| **IO0 (门铃)** | **0** |

编译宏：`-D CAMERA_MODEL_ESP32S3_EYE`

---

## 四、依赖组件

```yaml
dependencies:
  idf:
    version: ">=5.3.0"
  espressif/esp32-camera:
    version: "^2.1.7"
  espressif/mqtt:
    version: "^1.0.0"
  # esp_media_protocols(RTMP) 依赖已移除；中继推流用 lwip socket 直接 HTTP POST
```

---

## 五、sdkconfig.defaults

```ini
CONFIG_IDF_TARGET="esp32s3"
CONFIG_ESP32S3_SPIRAM_SUPPORT=y
CONFIG_ESP32S3_SPIRAM_MODE_OCT=y
CONFIG_ESP32S3_SPIRAM_SPEED_80M=y
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096
CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y
CONFIG_ESP_WIFI_ENABLED=y
CONFIG_HTTPD_MAX_REQ_HDR_LEN=1024
CONFIG_HTTPD_MAX_URI_LEN=512
CONFIG_FREERTOS_HZ=1000
CONFIG_LOG_DEFAULT_LEVEL_INFO=y
CONFIG_CAMERA_JPEG=y
```

---

## 六、软件架构

```
app_main()
  1. nvs_flash_init()
  2. esp_netif_init() + esp_event_loop_create_default()
  3. app_wifi_sta_start()           -- WiFi 连接
  4. app_camera_init()              -- OV3660 初始化
  5. app_httpd_start()              -- MJPEG 流服务器 (端口 81)
  6. app_mqtt_start()               -- MQTT 信令
  7. app_doorbell_init()            -- IO0 中断注册

数据流（A1 当前方案）:
  OV3660(DVP) → esp_camera_fb_get()
    ├── app_httpd.c  → /stream (MJPEG, :81) → App / 浏览器 直接播放
    └── app_mqtt.c   → MQTT → EMQX → App 推送（门铃事件/挂断）

数据流（A2 中继）:
  OV3660 → JPEG 帧 → app_relay.c（长连接 HTTP POST /pub/<id>）→ ECS MJPEG 中继(:8090) → App 拉 /stream/<id> 远程播放
```

---

## 七、模块设计

### 7.1 摄像头 (app_camera.c)

```c
camera_config_t camera_config = {
    .pin_pwdn = -1, .pin_reset = -1,
    .pin_xclk = 15, .pin_sccb_sda = 4, .pin_sccb_scl = 5,
    .pin_d7 = 16, .pin_d6 = 17, .pin_d5 = 18, .pin_d4 = 12,
    .pin_d3 = 10, .pin_d2 = 8,  .pin_d1 = 9,  .pin_d0 = 11,
    .pin_vsync = 6, .pin_href = 7, .pin_pclk = 13,
    .xclk_freq_hz = 20000000,
    .pixel_format = PIXFORMAT_JPEG,
    .frame_size = FRAMESIZE_VGA,
    .jpeg_quality = 12,
    .fb_count = 2,
    .grab_mode = CAMERA_GRAB_LATEST,
};
```

### 7.2 HTTP 流 (app_httpd.c) —— 当前主视频通道

| URI | 方法 | 功能 |
|-----|------|------|
| `/` | GET | 摄像头控制页面 |
| `/stream` | GET | **MJPEG 实时流 (81端口)** ← App 播放的就是这个 |
| `/capture` | GET | JPEG 快照 |
| `/control` | GET | 参数调节 |
| `/status` | GET | 传感器状态 JSON |

App 端用纯 Dart 的 MJPEG 解析控件（扫描 JPEG SOI `FFD8` / EOI `FFD9` 分帧）逐帧显示，无需任何原生视频解码插件。

### 7.3 中继推流 (app_relay.c) —— A2

> 原 `app_rtmp.c` 已删除（MJPEG-in-RTMP 无法被 SRS/ffmpeg 解析，见「方案变更说明」）。

按门铃开始推流时，`app_relay_start()` 创建一个 FreeRTOS 任务，用 lwip socket 向中继
`CONFIG_EXAMPLE_RELAY_HOST:PORT` 建一条长连接 HTTP POST `/pub/<门铃id>`，请求体为
`multipart/x-mixed-replace; boundary=frame`，随后每取一帧就发一段
`--frame` + `Content-Type: image/jpeg` + `Content-Length` + JPEG 数据。
超时或收到 `hangup` 时 `app_relay_stop()` 关闭连接。相关配置见 menuconfig
「MJPEG relay host / port」（默认写入 `sdkconfig.defaults`）。

### 7.4 MQTT 信令 (app_mqtt.c)

| Topic | QoS | 方向 | 说明 |
|-------|-----|------|------|
| `doorbell/{id}/event` | 1 | 发 | ringing / stream_start / stream_stop |
| `doorbell/{id}/cmd` | 1 | 收 | hangup / snapshot |

默认 `{id}` = `door-001`。Broker 使用已有的 `121.40.131.194:1883`。

### 7.5 门铃 (app_doorbell.c)

IO0 下降沿中断 → 去抖 300ms → MQTT 发布 ring → 开启视频（A1 下即保证 `/stream` 可访问）→ 60s 超时自动停止 / 收到 `hangup` 停止

---

## 八、服务器部署

### 8.1 MQTT（EMQX）—— 已有，继续使用

```
Broker: mqtt://121.40.131.194:1883
```

门铃信令走此 broker，App 与固件按 `doorbell/door-001/#` 收发。

### 8.2 ~~SRS（RTMP/WebRTC）~~ —— 视频链已弃用

> A1 方案下视频不经过 SRS。原 SRS/安全组（1935/8080/8000/50000-60000 等）对本项目视频**不再需要**。
> 若你的服务器仍跑着 SRS，可保留但与门铃视频无关。

### 8.3 MJPEG 中继（A2，已实现）

中继脚本见 **`server_relay/mjpeg_relay.py`**（纯 Python 标准库，无需 pip），部署与
安全组说明见 `server_relay/README.md`。要点：
- 入口：`POST /pub/<cam>` 接收板子长连接推来的 multipart JPEG 帧
- 出口：`GET /stream/<cam>` 对 App 提供 `multipart/x-mixed-replace` MJPEG 流
- 状态：`GET /status` 返回各 cam 的帧序号与最近一帧秒龄
- 安全组：开放 `8090/TCP`（既收推流也发观看流）

App 门铃页齿轮里把「视频地址」切到「中继(远程)」（`http://<relay>:8090/stream/door-001`）即可远程观看。

---

## 九、实施步骤 / 进度

| # | 内容 | 状态 |
|---|------|------|
| 1 | 项目脚手架（7 模块） | ✅ |
| 2 | 点亮 OV3660 | ✅ |
| 3 | MJPEG HTTP 流 `:81/stream` | ✅ |
| 4 | WiFi + MQTT 信令 | ✅ |
| 5 | 门铃 IO0 触发闭环 | ✅ |
| 6 | Flutter App（iOS 风格 + MJPEG 播放 + mesh 灯控预留） | ✅ |
| 7 | **A1 局域网直连联调出画面** | ✅ |
| 8 | A2 服务器 MJPEG 中继（远程可看）：中继脚本 + 固件 app_relay + App 地址切换 | ✅ 已实现，待部署联调 |

---

## 十、风险 / 注意

- PSRAM 必须开启；JPEG 模式省内存，勿用 RGB。
- IO0 做按键需软件去抖。
- **A1 前提**：手机（App）与板子必须在同一局域网（同一 WiFi），否则拉不到 `:81/stream`。远程访问需 A2 中继。
- ESP32-S3 仅支持 2.4G WiFi。

---

## 十一、App 侧（app/）

- 框架：Flutter + Cupertino（iOS 风格）。
- 视频：纯 Dart `MjpegView`（`http` 流式请求 + JPEG 分帧），无 media_kit / 原生解码插件。
- 信令：`mqtt_client` 连 `121.40.131.194:1883`，订 `doorbell/door-001/event`（ringing 弹 iOS 来电页），按钮发 `doorbell/door-001/cmd`（hangup/snapshot）。
- **Mesh 灯控接口（预留）**：独立「灯光」Tab，发 `office/light/demo/cmd`（on/off）、订 `office/light/demo/status`，与 `internal_communication/` 一致。
- 视频地址：门铃页右上角齿轮可随时修改，内置「中继(远程)」「局域网」两个预设，默认走中继公网地址。

---

## 十二、参考

- https://github.com/espressif/esp32-camera
- https://components.espressif.com/components/espressif/esp32-camera
- https://www.emqx.io/
- `internal_communication/` — MQTT 实现参考
