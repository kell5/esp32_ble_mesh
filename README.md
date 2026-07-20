# 萤火智联 ESP32 BLE Mesh 智能家居

本仓库是「萤火智联」的 ESP32 智能家居开源工程，包含 Flutter App、ESP32-S3 可视门铃、**BLE Mesh 网关/灯节点**（主线）、ESP-WIFI-MESH 网关/节点（历史基线）、RGB 灯直连样例、云端设备模型、OTA 服务和 MJPEG 公网中继。

## 入口

- 项目展示页：<https://www.lk-mcu.online/product-yinghuo-zhilian.html>
- Android APK：<https://www.lk-mcu.online/downloads/yinghuo-zhilian-mesh-app-release.apk>
- 固定通信协议：[`docs/COMMUNICATION_PROTOCOL.md`](docs/COMMUNICATION_PROTOCOL.md)
- 新旧产品接入指南：[`docs/PRODUCT_INTEGRATION_GUIDE.md`](docs/PRODUCT_INTEGRATION_GUIDE.md)
- 工作区总览：[`docs/WORKSPACE_PROJECT_OVERVIEW.md`](docs/WORKSPACE_PROJECT_OVERVIEW.md)

## 系统架构

```
手机 App (Flutter)
    │ MQTT
    ▼
MQTT Broker (公网 mosquitto)
    │ WiFi
    ▼
BLE Mesh 网关 (ESP32-S3)  ← Provisioner / Config Client / OnOff Client
    │ BLE Mesh (蓝牙 SIG 标准)
    ├── 灯节点 1 (ESP32-S3, WS2812, addr=0x0005)
    ├── 灯节点 2 (ESP32-WROOM, GPIO2,  addr=0x0006)
    └── ...

门铃 (ESP32-S3 + OV3660) — 独立 WiFi 链路，不走 Mesh
    ├── MQTT：门铃事件与控制
    ├── MJPEG：局域网直连视频流
    └── MJPEG Relay：公网远程观看
```

## 当前状态

| 功能 | 状态 |
|---|---|
| 门铃本地/远程视频闭环 | ✅ 已完成 |
| **BLE Mesh 网关 + 灯节点**（当前主线） | ✅ **硬件联调已通过**（组控 20/20，定向 50/50） |
| ESP-WIFI-MESH（历史基线） | ✅ 代码完成，已归档 |
| 统一设备首页与子页面 | ✅ 已完成 |
| 门铃/网关统一 BLE/SoftAP Wi-Fi 配网 | ✅ 已完成 |
| App 前后台通知 | ✅ 已完成 |
| 云端设备模型 (FastAPI + SQLite) | ✅ 已完成基础骨架 |
| OTA 升级（云端 + 设备端） | ✅ 主流程落地，持续实机验证 |

## 仓库结构

| 目录 | 职责 |
|---|---|
| `app/` | Flutter App — 设备首页、子页面、MQTT、MJPEG、BLE/SoftAP 配网 |
| `camera_stream/` | ESP32-S3 门铃固件 (OV3660) — MQTT、MJPEG、中继推流、OTA |
| `esp_ble_mesh/` | **蓝牙 SIG Mesh 主线** — Provisioner 网关 + OnOff Server 灯节点 |
| `internal_communication/` | ESP-WIFI-MESH 网关/节点（历史基线，已归档） |
| `rgb_light/` | RGB 灯直连样例（独立 WiFi + MQTT，不加入 Mesh） |
| `cloud_service/` | FastAPI 云端服务 — 设备注册、绑定、影子、OTA 调度 |
| `server_relay/` | Python MJPEG 公网中继 |
| `camera_dsi/` | 摄像头 DSI 接口备选固件 |
| `docs/` | 协议、接入指南、进度、架构文档 |

## BLE Mesh 硬件拓扑

| 角色 | 硬件 | 串口 | 固件 | 说明 |
|---|---|---|---|---|
| 网关 | ESP32-S3 N16R8 | COM4 | `esp_ble_mesh/provisioner/` | Provisioner + Config Client + OnOff Client + Wi-Fi/MQTT 桥接 |
| 灯节点 1 | ESP32-S3 N16R8 | COM8 | `esp_ble_mesh/onoff_models/onoff_server/` | OnOff Server，GPIO48 WS2812，地址 `0x0005` |
| 灯节点 2 | ESP32-WROOM | COM7 | `esp_ble_mesh/onoff_models/onoff_server/` | OnOff Server，GPIO2，地址 `0x0006` |
| 门铃 | ESP32-S3 N8R8 + OV3660 | — | `camera_stream/` | 独立 WiFi，不加入 BLE Mesh |

## 建议接手顺序

1. 先看 [`docs/WORKSPACE_PROJECT_OVERVIEW.md`](docs/WORKSPACE_PROJECT_OVERVIEW.md)
2. 再看 [`docs/COMMUNICATION_PROTOCOL.md`](docs/COMMUNICATION_PROTOCOL.md)
3. 新设备接入看 [`docs/PRODUCT_INTEGRATION_GUIDE.md`](docs/PRODUCT_INTEGRATION_GUIDE.md)
4. 需要追历史看 [`docs/DEVELOPMENT_PROGRESS.md`](docs/DEVELOPMENT_PROGRESS.md)

## 说明

- APK 不再放在 GitHub 源码仓库内，统一由个人网站提供下载。
- 后续新增门铃、网关、灯具、传感器时，优先复用同一套通信协议和设备模型。
- `internal_communication/`（ESP-WIFI-MESH）保留为回归基线，不再做主线开发。
