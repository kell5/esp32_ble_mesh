# ESP32 智能门铃与 Mesh 智能家居

本仓库包含 Flutter 智能家居 App、ESP32-S3 可视门铃、ESP-WIFI-MESH 网关/节点和 MJPEG 公网中继。

## 新会话/任务交接

**继续开发前先读：[`docs/DEVELOPMENT_PROGRESS.md`](docs/DEVELOPMENT_PROGRESS.md)**。

该文档记录当前阶段、硬件串口、协议、构建结果、遗留问题和下一步。完成任何阶段后同步更新它。

## 目录

- `app/`：Flutter App，统一设备首页、设备子页面、MQTT、MJPEG、BLE/SoftAP 配网和门铃通知。
- `camera_stream/`：ESP32-S3 门铃、OV3660、IO0、MQTT、MJPEG 和中继推流。
- `internal_communication/`：ESP-WIFI-MESH 网关与节点、MQTT 桥接、节点状态。
- `server_relay/`：Python MJPEG 公网中继。
- `docs/APP_DEVICE_ICON_PROMPTS.md`：App 设备图标视觉规范。
- `docs/SINGLE_DEVICE_ICON_PROMPTS.md`：逐张生成时可直接复制的完整提示词。

## 当前阶段

- 门铃本地/远程视频闭环：完成。
- Mesh 自动入网、自动发现、单灯/全体控制：完成。
- 统一设备首页与设备子页面：完成。
- 门铃/网关统一 BLE WiFi 配网：代码与构建完成，硬件回归可选。
- App 前台来电页与后台进程存活时的系统通知：完成；进程被杀后不通知是当前确认需求。
- Matter over WiFi：待开始。
- Thread：等待 ESP32-C6/H2 硬件。

详细状态以进度文档为准。
