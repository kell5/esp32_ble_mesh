# ESP32 智能门铃与 Mesh 智能家居

本仓库是 **萤火智联** 的 ESP32 智能家居开源工程，包含 Flutter 智能家居 App、ESP32-S3 可视门铃、ESP-WIFI-MESH 网关/节点、RGB 灯直连样例、云端设备模型、OTA 服务和 MJPEG 公网中继。

## APK 与源码下载

- Android APK：[`https://www.lk-mcu.online/downloads/yinghuo-zhilian-mesh-app-release.apk`](https://www.lk-mcu.online/downloads/yinghuo-zhilian-mesh-app-release.apk)
- 项目展示页：[`https://www.lk-mcu.online/product-yinghuo-zhilian.html`](https://www.lk-mcu.online/product-yinghuo-zhilian.html)
- 固定通信协议：[`docs/COMMUNICATION_PROTOCOL.md`](docs/COMMUNICATION_PROTOCOL.md)
- 新产品/旧产品升级接入清单：[`docs/PRODUCT_INTEGRATION_GUIDE.md`](docs/PRODUCT_INTEGRATION_GUIDE.md)

当前 APK 是硬件联调/演示用 release 包；生产分发前仍建议配置正式签名、版本号、渠道和隐私合规说明。

## 新会话/任务交接

继续开发前建议按这个顺序读：

1. [`docs/WORKSPACE_PROJECT_OVERVIEW.md`](docs/WORKSPACE_PROJECT_OVERVIEW.md)：当前工作区总览、目录职责、验证命令和提交纪律。
2. [`docs/COMMUNICATION_PROTOCOL.md`](docs/COMMUNICATION_PROTOCOL.md)：固定通信协议，新增产品和旧产品升级必须优先遵守。
3. [`docs/PRODUCT_INTEGRATION_GUIDE.md`](docs/PRODUCT_INTEGRATION_GUIDE.md)：新产品/旧产品 OTA 接入清单。
4. [`docs/DEVELOPMENT_PROGRESS.md`](docs/DEVELOPMENT_PROGRESS.md)：详细进度、硬件串口、构建结果、遗留问题和下一步。

完成任何阶段后同步更新总览、协议相关说明和进度文档。

## 目录

- `app/`：Flutter App，统一设备首页、设备子页面、MQTT、MJPEG、BLE/SoftAP 配网和门铃通知。
- `camera_stream/`：ESP32-S3 门铃、OV3660、IO0、MQTT、MJPEG 和中继推流。
- `internal_communication/`：ESP-WIFI-MESH 网关与节点、MQTT 桥接、节点状态。
- `server_relay/`：Python MJPEG 公网中继。
- `cloud_service/`：设备注册、用户绑定、设备影子和 MQTT 兼容桥接。
- `docs/DEVICE_ASSET_STATUS.md`：已识别图片、缺失素材、App 映射、固件设备类型协议和测试步骤。
- `docs/COMMUNICATION_PROTOCOL.md`：Farmely MQTT/Shadow/OTA 统一协议。
- `docs/PRODUCT_INTEGRATION_GUIDE.md`：后续新增门铃、网关、灯、传感器等产品的接入 checklist。
- `docs/APP_DEVICE_ICON_PROMPTS.md`：App 设备图标视觉规范。
- `docs/SINGLE_DEVICE_ICON_PROMPTS.md`：逐张生成时可直接复制的完整提示词。

## 当前阶段

- 门铃本地/远程视频闭环：完成。
- Mesh 自动入网、自动发现、单灯/全体控制：完成。
- 统一设备首页与设备子页面：完成。
- 门铃/网关统一 BLE WiFi 配网：代码与构建完成，硬件回归可选。
- App 前台来电页与后台进程存活时的系统通知：完成；进程被杀后不通知是当前确认需求。
- 多设备类型与图标资产：App、APK、网关和节点固件构建已通过，手机布局与启动图标已验证。
- 云端设备模型：首个 FastAPI + SQLite + MQTT 兼容切片完成，待接入 App 登录和固件原生 LWT/消息版本。
- Matter over WiFi：待开始。
- Thread：等待 ESP32-C6/H2 硬件。

详细状态以进度文档为准。
