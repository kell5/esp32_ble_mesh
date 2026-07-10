# 智能家居项目开发进度与会话交接

> **后续新对话首先读取本文件。**
> 本文件记录当前真实状态、阶段边界、硬件、构建结果、遗留问题和下一步，避免重复排查或误改已验证链路。

- 项目：`Mesh_esp32_farmely`
- GitHub：`https://github.com/kell5/esp32_ble_mesh`
- 主分支：`main`
- 最近更新：`2026-07-10`
- 当前开发主题：统一 BLE 配网、统一设备 App、门铃后台通知

## 1. 产品目标

构建一个适合作品集和智能家居开发简历展示的完整系统：

1. 门铃/监控/网关统一由 App 搜索和配网。
2. Mesh 子设备上电自动入网、自动发现、自动上报状态。
3. App 首页只展示统一设备卡片；点击后进入各类型独立子页面。
4. 支持执行器（灯、开关、插座、继电器、窗帘、阀门）和检测设备（温湿度、人体、门磁、烟雾等）的能力扩展。
5. 保留门铃本地/公网中继视频、MQTT 信令和 IO0 按键链路。
6. 后续补充设备影子、OTA、自动化、Matter 演示节点和 Thread 方案。

## 2. 当前系统架构

```text
门铃 ESP32-S3
  ├─ BLE/SoftAP WiFi 配网
  ├─ MQTT：门铃事件与控制
  ├─ MJPEG：局域网直连
  └─ MJPEG Relay：公网远程观看

Mesh 网关 ESP32-S3
  ├─ BLE WiFi 配网（仅网关构建启用）
  ├─ ESP-WIFI-MESH root
  ├─ MQTT 上下行桥接
  └─ per-node 状态聚合

Mesh 节点 ESP32/WROOM/S3
  ├─ 上电自动加入 Mesh
  ├─ 接收广播或寻址控制
  └─ P2P 上报在线、状态、类型、层级

Flutter App
  ├─ 统一设备首页
  ├─ 门铃/灯/网关独立子页面
  ├─ BLE 与 SoftAP 统一配网
  ├─ MQTT 设备发现和实时状态
  └─ 门铃前台来电页与后台系统通知
```

## 3. 目录职责

| 目录 | 职责 |
|---|---|
| `app/` | Flutter App、MQTT、MJPEG 播放、BLE/SoftAP 配网、设备页面 |
| `camera_stream/` | ESP32-S3 门铃固件、摄像头、IO0、MQTT、MJPEG、中继推流 |
| `internal_communication/` | ESP-WIFI-MESH 网关和节点、MQTT 桥接、per-node 状态 |
| `server_relay/` | Python MJPEG 公网中继 |
| `docs/` | 总进度、交接说明、设计资源提示词 |

## 4. 硬件与端口

| 角色 | 硬件 | 串口 | 当前说明 |
|---|---|---|---|
| 门铃 | ESP32-S3 N16R8 + OV3660 | COM8 | IO0 短按门铃；长按约 3 秒重置配网 |
| Mesh 网关/root | ESP32-S3 N16R8 | COM4 | MQTT 桥接；网关专用 BLE 配网配置 |
| Mesh 节点 1 | ESP32/WROOM | COM6 | 板载 D2 灯；节点构建关闭 brownout 作为供电兜底 |
| Mesh 节点 2 | ESP32-S3 N16R8 | COM14 | 已烧节点固件，可自动入网和上报状态 |
| Android 测试机 | MEP AN00 | ADB `AQRVUT5B11011314` | Flutter 调试安装目标 |

> 不要把 COM8 当作 Mesh 网关烧录；COM8 是门铃产品板。

## 5. 协议与固定接口

### 5.1 门铃 MQTT

| Topic | 方向 | 说明 |
|---|---|---|
| `doorbell/<id>/event` | 设备 → App | `ringing`、`stream_start`、`stream_stop` |
| `doorbell/<id>/cmd` | App → 设备 | `hangup`、`snapshot` |

### 5.2 Mesh MQTT

| Topic | 方向 | 说明 |
|---|---|---|
| `office/light/all/cmd` | App → 网关 | 全体控制 |
| `office/light/node/<id>/cmd` | App → 网关 | 单节点寻址控制 |
| `office/light/demo/cmd` | App → 网关 | 旧协议兼容 |
| `office/light/node/<id>/status` | 网关 → App | retained 节点状态 |
| `office/light/gateway/status` | 网关 → App | retained 网关聚合状态 |

### 5.3 统一 BLE 配网

| 项目 | 值 |
|---|---|
| BLE Service UUID | `021a9004-0382-4aea-bff4-6b3f1c5adfb4` |
| 门铃广播名前缀 | `Doorbell-` |
| 网关广播名前缀 | `Gateway-` |
| 传输 | ESP-IDF network_provisioning + protocomm Security1 |
| App 兼容模式 | BLE 优先；SoftAP 保留 |

配网端点：`prov-scan=ff50`、`prov-session=ff51`、`prov-config=ff52`、`proto-ver=ff53`、`custom-data=ff54`。

## 6. 阶段状态

### Phase A-0：门铃基础闭环 — 已完成

- [x] SoftAP 动态 WiFi 配网。
- [x] IO0 短按发布 `ringing`，长按 3 秒清除 WiFi。
- [x] MQTT 信令。
- [x] 局域网 MJPEG 实时画面。
- [x] 公网 MJPEG 中继。
- [x] App 本地/中继切换、快照、呼叫页。

### Phase A-1：Mesh 灯控产品化 — 已完成

- [x] 节点上电自动加入 Mesh。
- [x] node → root 使用 `MESH_DATA_P2P` 上报状态。
- [x] root 节点表、30 秒离线判定、retained MQTT 状态。
- [x] 广播、单节点寻址、旧 `demo/cmd` 兼容。
- [x] 修复 RMT/WS2812 多任务并发死锁。
- [x] App 两列设备卡片、网关详情、灯详情。
- [x] 设备模型预留 `type/name/value`。

### Phase A-2：统一设备 App 与 BLE 配网 — 代码和构建已完成

- [x] App 统一设备首页；门铃、灯、网关进入独立子页面。
- [x] 实时画面拆成纯观看页，不再复用呼叫挂断页面。
- [x] `TransportBLE` 接入现有 protocomm/Security1 配网栈。
- [x] BLE 扫描、设备选择、WiFi 扫描和凭据下发。
- [x] Android/iOS 蓝牙权限。
- [x] 门铃固件可选择 BLE 或 SoftAP，默认 BLE。
- [x] 网关专用构建启用 BLE 配网；普通 Mesh 节点不启用。
- [x] 门铃、网关、ESP32 节点固件编译通过。
- [x] Flutter analyze 和 debug APK 构建通过。
- [ ] 可选硬件验证：实际清除门铃/网关 WiFi 后，用 App 完成一次 BLE 配网。

### Phase A-3：门铃系统通知 — 第一阶段已实现

- [x] App 在前台收到 `ringing` 时显示来电页面。
- [x] App 处于后台但进程仍存活时显示 Android/iOS 本地系统通知。
- [x] 点击通知进入门铃来电/实时画面页面。
- [x] Android 13+ 通知权限和高优先级门铃通知频道。
- [x] 通知插件所需 core library desugaring 配置。
- [ ] 真正“App 被系统彻底杀死后仍能通知”：需要服务端推送。
- [ ] 推送提供方待选：国际 Android/iOS 用 FCM + APNs；华为无 GMS 设备建议 HMS Push Kit；也可做统一 Push Provider 接口同时支持两者。

> 当前本地通知依赖 App 的 MQTT 进程仍在运行。不要把它描述成已实现的离线云推送。

### Phase B：云端设备模型 — 未开始

计划内容：

- [ ] 设备注册与用户绑定。
- [ ] 统一设备影子（desired/reported）。
- [ ] MQTT LWT、离线原因、消息版本和幂等。
- [ ] 房间、分组、场景和自动化规则。
- [ ] 固件 OTA、版本管理、灰度与回滚。
- [ ] 门铃事件记录、快照索引和权限控制。
- [ ] 服务端推送（FCM/APNs/HMS）和设备 token 管理。

### Phase C：Matter over WiFi 简历演示 — 未开始

- [ ] 使用现有 ESP32-S3 做 Matter over WiFi 灯或插座演示节点。
- [ ] BLE commissioning、OnOff Cluster、设备证明和二维码。
- [ ] 接入 Apple Home / Google Home / Alexa 中至少一个生态。
- [ ] 自研 MQTT 产品线与 Matter 演示线并行，不把视频门铃强行迁移到 Matter。

### Phase D：Thread / Border Router — 暂停

- 原因：当前 ESP32-S3/WROOM 没有 802.15.4 射频。
- 启动条件：准备 ESP32-C6 或 ESP32-H2；Border Router 通常还需要可承担 WiFi/Ethernet 回程的设备。
- 当前不写无法在现有硬件验证的 Thread 产品代码。

## 7. 最近构建结果（2026-07-10）

| 目标 | 结果 | 产物/备注 |
|---|---|---|
| Flutter analyze | 通过，0 issues | `app/` |
| Flutter debug APK | 通过 | `app/build/app/outputs/flutter-apk/app-debug.apk` |
| 门铃 ESP32-S3 | 通过 | `camera_stream/build/`；并行编译曾触发编译器异常，`ninja -j1` 可稳定通过 |
| Mesh 网关 ESP32-S3 | 通过 | `internal_communication/build-gateway/` |
| Mesh 节点 ESP32 | 通过 | `internal_communication/build-node/`；app 分区只剩约 1%，需关注 |

关键构建配置：

- ESP-IDF：`D:\esp-idf\v6.0.1\esp-idf`
- Flutter：`D:\flutter\flutter\bin\flutter.bat`
- 网关 defaults：`sdkconfig.defaults;sdkconfig.defaults.esp32s3;sdkconfig.gateway.defaults`
- 节点 defaults：`sdkconfig.defaults;sdkconfig.node.defaults`

## 8. 当前代码变更范围

主要文件：

- `app/lib/prov/transport_ble.dart`
- `app/lib/pages/provisioning_page.dart`
- `app/lib/pages/home_page.dart`
- `app/lib/pages/root_page.dart`
- `app/lib/services/doorbell_notification_service.dart`
- `app/lib/config.dart`
- `app/pubspec.yaml`
- `app/android/app/src/main/AndroidManifest.xml`
- `app/android/app/build.gradle.kts`
- `app/ios/Runner/Info.plist`
- `camera_stream/main/app_wifi.c`
- `camera_stream/main/Kconfig.projbuild`
- `camera_stream/sdkconfig.defaults`
- `internal_communication/main/mesh_main.c`
- `internal_communication/main/Kconfig.projbuild`
- `internal_communication/main/CMakeLists.txt`
- `internal_communication/sdkconfig.gateway.defaults`

## 9. 已知风险和禁止回归

1. **不得破坏门铃视频链路**：MJPEG 本地和中继必须保留。
2. **不得改变现有 MQTT topic 语义**：新协议应兼容旧 `demo/cmd`。
3. **不得给普通 Mesh 节点强制开启网关 BLE 配网**。
4. ESP32 节点固件 app 分区余量约 1%，新增组件前先检查尺寸。
5. COM6 关闭 brownout 只是供电不足的兜底，存在掉电/闪存风险，最终应改善供电。
6. BLE 配网只支持 2.4 GHz WiFi；错误提示不要暗示 ESP32 可连接 5 GHz。
7. 本地通知不是云推送；进程被杀后的通知必须由服务器和系统推送通道完成。
8. 不提交 `build/`、生成的 `sdkconfig.*.generated`、日志、截图和临时脚本。

## 10. 下一步优先级

1. 提交并推送 Phase A-2/A-3 与文档。
2. 可选：用测试手机验证 App 退到后台后按 COM8 IO0，系统通知出现且点击能进入门铃页。
3. 可选：擦除门铃或网关 WiFi，完成一次真实 BLE 配网闭环。
4. 设计 Phase B 的设备注册、影子、LWT、OTA 和 Push Provider 接口。
5. 如需要“App 被杀仍提醒”，优先确定测试机是否具备 GMS；MEP AN00 若无 GMS，选择 HMS Push Kit 或 FCM/HMS 双实现。
6. 准备 Matter over WiFi 灯/插座演示节点。

## 11. 新会话恢复步骤

新会话不要直接改代码，按顺序执行：

1. 阅读本文件和 `docs/APP_DEVICE_ICON_PROMPTS.md`。
2. 执行 `git status --short`、`git branch --show-current`、`git log -1 --oneline`。
3. 确认用户要继续的是 BLE 硬件验证、系统推送、Phase B、Matter，还是 UI 图标资源。
4. 修改前阅读对应模块 README 和当前实现。
5. 完成后更新本文件的阶段状态、构建结果、已知问题和下一步。

## 12. 文档索引

- `docs/DEVELOPMENT_PROGRESS.md`：总进度和会话交接（本文件）。
- `docs/APP_DEVICE_ICON_PROMPTS.md`：设备类型图标统一生成提示词。
- `camera_stream/README.md`：门铃摄像头与 MJPEG 链路。
- `server_relay/README.md`：公网 MJPEG 中继部署。
- `internal_communication/办公区灯控技术方案.md`：Mesh 灯控设计。
