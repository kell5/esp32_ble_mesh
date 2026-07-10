# 智能家居项目开发进度与会话交接

> **后续新对话首先读取本文件。**
> 本文件记录当前真实状态、阶段边界、硬件、构建结果、遗留问题和下一步，避免重复排查或误改已验证链路。

- 项目：`Mesh_esp32_farmely`
- GitHub：`https://github.com/kell5/esp32_ble_mesh`
- 主分支：`main`
- 最近更新：`2026-07-10`
- 当前开发主题：多设备类型、App 图标资产、Mesh 状态 `type` 协议

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

### Phase A-3：门铃系统通知 — 按当前需求已完成

- [x] App 在前台收到 `ringing` 时显示来电页面。
- [x] App 处于后台但进程仍存活时显示 Android/iOS 本地系统通知。
- [x] 点击通知进入门铃来电/实时画面页面。
- [x] Android 13+ 通知权限和高优先级门铃通知频道。
- [x] 通知插件所需 core library desugaring 配置。
- [x] 用户确认 App 被系统彻底杀死后不需要通知，不接入 FCM/HMS 云推送。

> 当前通知范围：App 前台显示来电页；App 退到后台但 MQTT 进程仍存活时显示系统通知。

### Phase A-4：多设备类型、图标资产与状态协议 — 代码、构建与手机布局验证完成

- [x] 实际读取 `app/image/`：找到 26 张单图，不是清单中的 36 张。
- [x] 识别 9 类设备：网关、灯泡、吸顶灯、灯带、墙壁开关、插座、窗帘电机、阀门、门锁。
- [x] 确认缺失：门铃、摄像头、继电器三态，以及阀门离线态。
- [x] 将可用素材中心裁剪并转换为 26 个 `512×512` WebP 测试 asset。
- [x] App `DeviceType` 扩展到具体产品类型，并兼容旧值 `light/switch`。
- [x] 首页、网关详情和设备详情接入真实设备图片；缺图时使用系统图标。
- [x] 使用新素材替换 Android、iOS、macOS、Web 和 Windows 应用启动图标。
- [x] 首页过滤 root 节点，网关只在网关模块显示，避免重复显示成灯。
- [x] 设备详情按灯具、开关/插座、窗帘、阀门、门锁显示不同扩展方向。
- [x] Mesh 固件增加 Kconfig 节点类型选择和 MQTT `type` 字段。
- [x] 状态包采用末尾追加字段，网关仍兼容没有 `type` 的旧节点。
- [x] Flutter analyze 通过（0 issues），debug APK 构建通过。
- [x] debug APK 已安装到已连接手机；设备图标与页面布局显示正常，已修复设备卡片底部 16px 溢出；未触发实际节点控制。
- [x] ESP32-S3 网关构建通过。
- [x] ESP32 节点构建通过；COM6 WROOM 当前断电，未做烧录或串口验证。
- [ ] 用户实机选择不同设备类型并烧录验证。

详细文件映射、协议示例和测试步骤见 [DEVICE_ASSET_STATUS.md](DEVICE_ASSET_STATUS.md)。

### Phase B：云端设备模型 — 首个纵向切片完成

- [x] 新增 `cloud_service/`：FastAPI + SQLite 本地优先服务。
- [x] 设备注册、未认领设备绑定用户、按用户查询设备。
- [x] 统一设备影子（`desired/reported`）、独立版本号和 `message_id` 幂等。
- [x] 兼容现有 Mesh 节点、网关和门铃 MQTT 状态主题，不修改既有主题语义。
- [x] `desired.on`/`desired.command` 转换为现有节点和门铃控制主题。
- [x] 云端超时离线与明确 `offline_reason`。
- [ ] 固件 MQTT LWT、消息版本/消息 ID 原生上报和 broker ACL。
- [ ] App 登录、云端设备列表和影子状态接入。
- [ ] 房间、分组、场景和自动化规则。
- [ ] 固件 OTA、版本管理、灰度与回滚。
- [ ] 门铃事件记录、快照索引和权限控制。

运行、API、环境变量和兼容主题见 [`cloud_service/README.md`](../cloud_service/README.md)。

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

> Phase A-4 已完成 Flutter、固件和手机布局验证；Phase B 首个云端设备模型切片已通过静态检查与本地 API 测试。

| 目标 | 结果 | 产物/备注 |
|---|---|---|
| Flutter analyze | 通过，0 issues | `app/` |
| Flutter debug APK | 通过 | `app/build/app/outputs/flutter-apk/app-debug.apk` |
| Cloud service | Ruff 通过；7 个 API/影子测试通过 | `cloud_service/` |
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
- `app/assets/device_icons/`
- `app/lib/widgets/device_icon.dart`
- `app/lib/widgets/smart_cards.dart`
- `app/lib/pages/home_page.dart`
- `app/lib/pages/gateway_detail_page.dart`
- `app/lib/pages/light_detail_page.dart`
- `app/lib/services/mqtt_service.dart`
- `internal_communication/main/include/mesh_light.h`
- `docs/DEVICE_ASSET_STATUS.md`

## 9. 已知风险和禁止回归

1. **不得破坏门铃视频链路**：MJPEG 本地和中继必须保留。
2. **不得改变现有 MQTT topic 语义**：新协议应兼容旧 `demo/cmd`。
3. **不得给普通 Mesh 节点强制开启网关 BLE 配网**。
4. ESP32 节点固件 app 分区余量约 1%，新增组件前先检查尺寸。
5. COM6 关闭 brownout 只是供电不足的兜底，存在掉电/闪存风险，最终应改善供电。
6. BLE 配网只支持 2.4 GHz WiFi；错误提示不要暗示 ESP32 可连接 5 GHz。
7. 通知仅保证 App 前台和后台进程存活场景；进程被杀后不通知是当前确认需求。
8. 不提交 `build/`、生成的 `sdkconfig.*.generated`、日志、截图和临时脚本。
9. 当前 26 张图片是 RGB 棋盘格背景测试素材；正式发布前应替换为无平台标识、透明背景、状态一致的素材。
10. 窗帘、阀门、门锁等目前只复用 on/off 协议和板载 LED 指示，尚未实现真实电机、限位、继电器与安全保护。
11. COM6 WROOM 当前已断电；本轮不连接、不烧录，仅做节点固件构建验证。

## 10. 下一步优先级

1. 完成 Phase A-4 的 Flutter analyze、debug APK、ESP32-S3 网关和 ESP32 节点构建。
2. 将新版 APK 安装到已连接手机，检查设备图标、离线态、类型文案和详情页。
3. WROOM 上电后由用户选择节点类型并烧录，验证 MQTT `type` 和 App 卡片同步变化。
4. 将当前功能分支合并到 `main`。
5. 可选：用测试手机验证 App 退到后台后按 COM8 IO0，系统通知出现且点击能进入门铃页。
6. 可选：擦除门铃或网关 WiFi，完成一次真实 BLE 配网闭环。
7. 设计 Phase B 的设备注册、影子、LWT、OTA、房间和自动化接口。
8. 准备 Matter over WiFi 灯/插座演示节点。

## 11. 新会话恢复步骤

新会话不要直接改代码，按顺序执行：

1. 阅读本文件、`docs/DEVICE_ASSET_STATUS.md`、`docs/APP_DEVICE_ICON_PROMPTS.md` 和 `docs/SINGLE_DEVICE_ICON_PROMPTS.md`。
2. 执行 `git status --short`、`git branch --show-current`、`git log -1 --oneline`。
3. 先检查 Phase A-4 的构建/手机安装是否完成，再确认继续 BLE 硬件验证、Phase B 或 Matter。
4. 修改前阅读对应模块 README 和当前实现。
5. 完成后更新本文件的阶段状态、构建结果、已知问题和下一步。

## 12. 文档索引

- `docs/DEVELOPMENT_PROGRESS.md`：总进度和会话交接（本文件）。
- `docs/DEVICE_ASSET_STATUS.md`：已识别图片、缺失素材、App 映射、固件 `type` 协议和测试步骤。
- `docs/APP_DEVICE_ICON_PROMPTS.md`：设备图标视觉规范与类型描述。
- `docs/SINGLE_DEVICE_ICON_PROMPTS.md`：逐张生成的完整可复制提示词。
- `camera_stream/README.md`：门铃摄像头与 MJPEG 链路。
- `server_relay/README.md`：公网 MJPEG 中继部署。
- `internal_communication/办公区灯控技术方案.md`：Mesh 灯控设计。
