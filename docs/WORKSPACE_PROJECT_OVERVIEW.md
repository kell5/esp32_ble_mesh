# 工作区项目总览（2026-07-18）

本文件用于快速接手 `G:\Mesh_esp32_farmely`。详细历史仍以 `docs/DEVELOPMENT_PROGRESS.md`、`docs/MESH_HARDWARE_INTEGRATION_PLAN.md` 和各子项目 README 为准。

## 当前主线

目标是把 ESP32 门铃、BLE Mesh 灯控、账号云端、设备影子、App 统一首页、配网绑定和 OTA 做成一个可演示的智能家居闭环。

当前已实机打通的关键链路：

- Flutter App 登录/设备列表/添加设备/设备详情。
- 门铃 MQTT 信令和 MJPEG 本地/中继播放链路。
- BLE Mesh 网关和灯节点控制链路。
- 云端 FastAPI + SQLite + MQTT bridge：设备注册、账号认领、设备影子、房间/分组/场景/自动化、OTA。
- `rgb_light` ESP32-S3 直连 OTA：`rgb-F53324` 已从 v1.0.0 OTA 到 v1.1.0，云端记录为 success。

## 目录职责

| 目录 | 角色 | 当前状态 |
| --- | --- | --- |
| `app/` | Flutter App。账号优先入口、设备列表、云端设备详情、门铃视频、BLE/SoftAP 配网、局域网 MQTT 加速控制。 | 可构建、可安装；设备页已加 OTA 检查入口。 |
| `cloud_service/` | FastAPI 云端。设备注册/认领、shadow、自动化、OTA 固件仓库与 rollout、MQTT bridge。 | 已部署到阿里云容器；`/health` 正常，MQTT connected。 |
| `rgb_light/` | ESP32-S3 RGB 灯直连样例。BLE 配网、MQTT 上报/控制、HTTP OTA、sha256 校验、回滚确认。 | COM14 实机 OTA 通过。 |
| `camera_stream/` | ESP32-S3 门铃/摄像头固件。OV3660、IO0 门铃、MQTT、MJPEG、本地/中继视频。 | 视频闭环已完成，后续偏产品化优化。 |
| `server_relay/` | Python MJPEG 公网中继。 | 与门铃远程视频链路配套。 |
| `internal_communication/` | ESP-WIFI-MESH/BLE Mesh 网关与节点主线，MQTT 桥接、节点状态。 | Mesh 控制链路已验证，后续仍需整理固件分支与产物。 |
| `docs/` | 架构、进度、硬件记录、交接、素材和验收文档。 | 继续作为新会话入口。 |
| `esp_ble_mesh/` | 乐鑫 BLE Mesh 示例/参考代码。 | 当前大量文件仍是未跟踪状态，提交前必须谨慎筛选。 |

## 配网口令规则

不同设备类型可以使用不同 PoP。App 依据 BLE 广播名/设备名前缀自动选择：

- `Doorbell-...` 或未知设备：`doorbell1234`
- `Gateway-...`：`gateway1234`
- `Light-...`：`light1234`

这样不会影响下次门铃或网关配网。新增设备类型时优先复用这个前缀映射，不要把所有设备硬编码成同一个 PoP。

## OTA 现状

### 云端

- 固件记录：`product_id + hw_version + fw_version -> url + sha256`
- rollout：按 `product_id + hw_version` 命中，可配置百分比灰度。
- 下发：设备上报版本后云端发布 `farmely/<class>/<device_id>/down/ota`。
- 回报：设备通过 `farmely/<class>/<device_id>/up/ota` 回报 `downloading / success / failed`，云端写入 `ota_updates`。
- App：设备详情页可查询 OTA 任务历史，并对自己名下设备触发“检查更新”。

### RGB 灯实测

- 设备：ESP32-S3 N16R8，`rgb-F53324`，COM14，MAC `e0:72:a1:f5:33:24`。
- 基线：v1.0.0，无 `color` capability。
- 目标：v1.1.0，支持 RGB color capability。
- 线上固件：`http://114.55.208.72/firmware/rgb_light_v11.bin`
- 当前 SHA256：`861487fb6d7822d1ba5fd90edd63d83025a4df9520bf6e20c959894d154f31c8`
- 验收结果：设备下载、校验、写入 `ota_1`、重启、上报 v1.1.0 和 OTA success；云端记录 success。

## 常用验证命令

### App

```powershell
cd app
flutter analyze
flutter build apk --debug
```

安装到当前测试手机：

```powershell
Copy-Item .\build\app\outputs\flutter-apk\app-debug.apk $env:TEMP\app-debug.apk -Force
cd ..
.\app_install.ps1
```

### 云端

```powershell
cd cloud_service
.\.venv\Scripts\python.exe -m unittest discover -s tests
.\.venv\Scripts\python.exe -m unittest tests.test_ota
```

### RGB 灯固件

```powershell
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass
& 'D:\esp-idf\v6.0.1\esp-idf\export.ps1'
cd rgb_light
idf.py -B build_ota_v1_uart -DSDKCONFIG=build_ota_v1_uart\sdkconfig -DSDKCONFIG_DEFAULTS='sdkconfig.defaults;sdkcfg-nocolor' -D PROJECT_VER=1.0.0 build
idf.py -B build_ota_v11_uart -DSDKCONFIG=build_ota_v11_uart\sdkconfig -DSDKCONFIG_DEFAULTS='sdkconfig.defaults' -D PROJECT_VER=1.1.0 build
idf.py -B build_ota_v1_uart -p COM14 flash
```

## 提交纪律

提交前重点看：

```powershell
git status --short
git diff --stat
git diff --cached --stat
```

当前工作区存在大量历史/临时未跟踪文件和目录，例如 `.devin*`、`_shots/`、`对话记录/`、`esp_ble_mesh/` 示例大批文件、`rgb_light/build_*`、日志和截图。除非明确需要，不能混进功能提交。

如果误暂存，先用：

```powershell
git restore --staged .
```

这只清暂存区，不删除工作区文件。

## 近期建议

1. App 设备页 OTA 已能“检查更新”，下一步可增加更清晰的升级进度轮询/倒计时提示。
2. 为门铃、网关固件补齐与 `rgb_light` 同样的 `product_id / hw_version / fw_version / capabilities` 上报，再接入 OTA。
3. 清理历史构建产物、日志、截图和误引入示例目录，完善 `.gitignore`，降低后续误提交风险。
4. 产品化前补固件签名、HTTPS 固件下载、broker ACL/TLS、账号 token 过期/刷新。
