# 智能家居项目开发进度与会话交接

> **后续新对话首先读取本文件。**
> 本文件记录当前真实状态、阶段边界、硬件、构建结果、遗留问题和下一步，避免重复排查或误改已验证链路。

- 项目：`Mesh_esp32_farmely`
- GitHub：`https://github.com/kell5/esp32_ble_mesh`
- 主分支：`main`
- 最近更新：`2026-07-13`
- 当前开发主题：BLE Mesh + Wi-Fi 网关硬件联调
- 下一阶段规划：见 `docs/PRODUCT_ARCHITECTURE_AND_ROADMAP.md`（账号优先 UX、配网即绑定、WebRTC 媒体、OTA、协议规约；已按模块解耦成可并行任务）

## 0. 2026-07-14 当前联调状态

- 当前先完成固件和硬件 Mesh 联调，Flutter App 暂不联调；App 仍为 PR #4 合并后的版本，尚未接入 BLE Mesh 新网关的发现、设备映射与控制界面。
- 当前串口映射：COM7 门铃；COM4、COM8 为 ESP32-S3 N16R8；COM6 为 ESP32-WROOM。COM14、COM15 存在串口/启动异常，不再作为活动端口。
- 串口号不固定代表产品角色；COM4/COM8 可在读取芯片信息确认后分别刷成网关或普通节点，当前保持 COM4 网关、COM8 节点。
- COM4 已确认是 ESP32-S3 N16R8，作为 Provisioner、Config Client、Generic OnOff Client 和未来 Wi-Fi/MQTT 网关。
- COM8 已确认是 ESP32-S3 N16R8，替代无法启动应用的 COM14 完成双板联调。
- COM4 + COM8 已完成未配网发现、Provisioning、Composition Data Get、AppKey Add、Generic OnOff Server Model Bind、Generic OnOff Get/Set/Status：
  - COM4：`farmely_mesh:stage=8, addr=5, onoff=1`；
  - COM8：`farmely_node:stage=7, addr=5, onoff=1`。
- 保留 NVS 重刷网关后，COM4 自动查询已配网节点并达到 `stage=9, addr=5`；双方 NetKey/AppKey/节点信息仍存在，未重复 Provisioning。
- COM14 固件写入和校验成功，但应用级 NVS 始终为空；同一节点固件在 COM4 可运行，故记录为 COM14 启动/复位路径阻塞，不记录为 BLE Mesh 射频失败。
- 网关已加入可配置 Wi-Fi STA、MQTT 自动重连、normalized/legacy topic 订阅、MQTT→BLE Generic OnOff、状态与 ACK 回传代码；固件构建成功并刷入 COM4。由于未配置 Wi-Fi/MQTT 凭据，联网、云端闭环和 30 分钟共存压力尚未验收。
- 节点已写入“恢复已保存 OnOff 与配网阶段”的本地修改；S3 固件 836224 bytes、WROOM 固件 811424 bytes，app 分区分别剩余 46% 和 47%。
- COM8 已刷入最新 S3 节点固件，重启后保持地址 `0x0005`、已配网状态和 `onoff=0`。
- COM6 已由 esptool 确认为 ESP32-D0WD-V3 rev 3.1、4 MB Flash，并刷成 GPIO2/D2 的第二个 BLE Mesh Generic OnOff Server；已配网为地址 `0x0006`，重启后保持 `stage=7/onoff=1`。
- COM4 重启恢复后 NVS 同时保存节点 `0x0005`、`0x0006`，并查询到 `addr=6/onoff=1/stage=9`；分组地址和 50 次定向无误控仍未测试。

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

COM4 BLE Mesh + Wi-Fi 网关 ESP32-S3 N16R8
  ├─ BLE Mesh Provisioner / Config Client / Generic OnOff Client
  ├─ Wi-Fi STA + MQTT 上下行桥接
  ├─ normalized/legacy topic 兼容
  └─ 节点地址、状态和重启恢复

COM8 BLE Mesh 灯节点 ESP32-S3 N16R8
  ├─ Generic OnOff Server
  ├─ BLE Mesh settings
  └─ GPIO48 WS2812

ESP-WIFI-MESH
  └─ 保留为历史回归基线；代码完成、历史部分验证、正式硬件验收未完成

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
| `esp_ble_mesh/` | 乐鑫官方 BLE Mesh 示例及本轮 Provisioner/OnOff Server 主线 |
| `server_relay/` | Python MJPEG 公网中继 |
| `docs/` | 总进度、交接说明、设计资源提示词 |

## 4. 硬件与端口

| 角色 | 硬件 | 串口 | 当前说明 |
|---|---|---|---|
| 门铃 | ESP32-S3 N8R8 + OV3660 | COM7 | 实测 Flash 8 MB、PSRAM 8 MB；Wi-Fi/MQTT/MJPEG，不加入 BLE Mesh |
| BLE Mesh + Wi-Fi 网关 | ESP32-S3 N16R8 | COM4 | 16 MB Flash、8 MB PSRAM；Provisioner/Config/OnOff Client + Wi-Fi/MQTT bridge |
| BLE Mesh 灯节点 | ESP32-S3 N16R8 | COM8 | 16 MB Flash、8 MB PSRAM；Generic OnOff Server；GPIO48 WS2812；已完成双板主链路 |
| 第二 BLE Mesh 灯节点 | ESP32-WROOM | COM6 | ESP32-D0WD-V3、4 MB Flash；GPIO2/D2；已配网地址 `0x0006` |
| 非活动端口 | 历史板卡 | COM14、COM15 | 存在启动或下载异常，不再阻塞当前联调 |
| Android 测试机 | MEP AN00 | ADB `AQRVUT5B11011314` | Flutter 调试安装目标 |

> 当前只使用 COM7、COM4、COM8、COM6；每次刷写前仍需读取芯片信息，串口号可随插拔变化，也可通过重新刷写改变板卡角色。

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

### Phase A-1：ESP-WIFI-MESH 灯控 — 代码完成，正式硬件验收未完成

- [x] 节点上电自动加入 Mesh。
- [x] node → root 使用 `MESH_DATA_P2P` 上报状态。
- [x] root 节点表、30 秒离线判定、retained MQTT 状态。
- [x] 广播、单节点寻址、旧 `demo/cmd` 兼容。
- [x] 修复 RMT/WS2812 多任务并发死锁。
- [x] App 两列设备卡片、网关详情、灯详情。
- [x] 设备模型预留 `type/name/value`。
- [x] ESP32-S3 gateway 与 ESP32 node 固件构建通过。
- [x] 历史记录显示 COM14 曾烧录节点并能自动入网/上报。
- [ ] 缺少可复现的 root + 至少两个节点完整 UART 日志和逐项结果，不能标记为“Wi-Fi Mesh 已完整验证成功”。
- [ ] 若作为回退方案正式验收，需补自动组网、单播、广播、断电重连、离线判定和持续运行测试。

### Phase A-1B：BLE Mesh 灯控 + Wi-Fi 网关 — 联调准备中

- [x] 架构确认：普通灯节点使用 BLE Mesh Generic OnOff；N16R8 网关同时运行 BLE Mesh Provisioner/Client 与 Wi-Fi/MQTT。
- [x] 门铃保持独立 Wi-Fi/MQTT/视频链路，不加入 BLE Mesh。
- [x] 乐鑫官方示例已放入工作区作为参考，覆盖 Provisioner、Generic OnOff Server 和 Wi-Fi coexist。
- [x] 当前硬件约束确认：COM6 WROOM 使用 GPIO2/D2，COM4/COM8 N16R8 使用 GPIO48 WS2812。
- [x] 提交联调计划和结果判定标准。
- [x] COM7 N8R8 门铃固件构建和刷写成功；OV3660、Wi-Fi、MQTT 启动验证通过。
- [ ] COM7 尚需实体按键、60 秒视频观看和云端命令 ACK 验收。
- [x] WROOM Generic OnOff Server 已完成 GPIO2、brownout-off、settings 和单元素适配。
- [x] COM14、COM15 记录为非活动异常端口，不再继续消耗主线联调时间。
- [x] COM6 WROOM 已完成构建、刷写、配网和 NVS 重启恢复。
- [ ] COM6 仍需人工确认 GPIO2/D2 实灯恢复，并与 COM8 完成分组和 50 次定向无误控测试。
- [ ] N16R8 BLE Mesh + Wi-Fi/MQTT 网关实现与共存压力验证。

详细步骤、判定阈值和结果记录见 [MESH_HARDWARE_INTEGRATION_PLAN.md](MESH_HARDWARE_INTEGRATION_PLAN.md)。

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
- [x] Mesh 网关和门铃 MQTT LWT、消息版本/消息 ID 原生上报。
- [x] 房间、分组、场景和自动化规则（纯 cloud_service 后端切片：REST CRUD、整组下发、场景激活、`reported` 触发的自动化引擎）。
- [x] 邮箱账号体系（demo 级）：`accounts`/`account_tokens` 表、注册/登录/me/logout、per-user Bearer token、密码 PBKDF2 哈希（标准库、无新依赖）。
- [x] 账号即空间的设备隔离：`/me/devices`、`/me/devices/<id>/claim`，非管理员只能访问自己认领的设备（跨账号 403）；`X-Cloud-Token` 保留为管理员/设备置备。
- [x] 门铃事件记录：MQTT `event` 落库（`message_id` 幂等），`/me/events` 与 `/devices/<id>/events` 分页查询。
- [x] App 注册/登录页（邮箱+密码，登录/注册切换）+ 只存 服务器地址/Bearer token/user_id/email；「云端」页支持「添加设备」（输入设备 ID 认领）。
- [x] App 修复 Android 系统返回键直接退出：先弹内层页面 → 切回首个标签 → 再弹确认框退出（`PopScope` + 每标签独立 Navigator）。
- [x] 部署到自有服务器：`Dockerfile` + `docker-compose.yml`（容器仅监听 `127.0.0.1:8000`），Nginx 反代 `https://lk-mcu.online/cloud/` 复用现有 Let's Encrypt 证书，已上线连真实 broker。
- [x] Ruff 通过；28 个测试通过（含账号鉴权与隔离、门铃事件）。
- [ ] 生产 broker ACL、凭据轮换；账号体系生产化（token 过期/刷新、邮箱验证、限流）。
- [ ] 固件 OTA、版本管理、灰度与回滚。
- [ ] 门铃快照/媒体存储与索引。
- [ ] 自动化增强：时间/多条件触发、延时与冷却。

运行、API、环境变量、账号鉴权和部署见 [`cloud_service/README.md`](../cloud_service/README.md)。

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

> Phase A-4 已完成 Flutter、固件和手机布局验证；Phase B 云端设备模型与固件 LWT/消息版本切片已通过静态检查、本地 API 测试和三套固件构建。

| 目标 | 结果 | 产物/备注 |
|---|---|---|
| Flutter analyze | 通过，0 issues | `app/` |
| Flutter debug APK | 通过 | `app/build/app/outputs/flutter-apk/app-debug.apk` |
| Cloud service | Ruff 通过；28 个测试通过（含账号鉴权/隔离、门铃事件、房间/分组/场景/自动化） | `cloud_service/` |
| 门铃 ESP32-S3 | 通过 | `camera_stream/build/`；app 分区剩余 13% |
| Mesh 网关 ESP32-S3 | 通过 | `internal_communication/build-gateway/`；app 分区剩余 13% |
| Mesh 节点 ESP32 | 通过 | `internal_communication/build-node/`；app 分区只剩 1%，需关注 |

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
- `cloud_service/src/cloud_service/models.py`（房间/分组/场景/自动化模型）
- `cloud_service/src/cloud_service/storage.py`（对应 SQLite 表与方法）
- `cloud_service/src/cloud_service/automation.py`（分组下发/场景激活/自动化引擎）
- `cloud_service/src/cloud_service/main.py`（新增 REST 端点、引擎接线）
- `cloud_service/src/cloud_service/mqtt_bridge.py`（reported 变更回调触发自动化）
- `cloud_service/tests/test_organization.py`（新增功能测试）
- `cloud_service/README.md`
- `app/lib/services/cloud_client.dart`（云端 REST 客户端；Bearer 鉴权、register/login/claim、错误文案）
- `app/lib/services/cloud_session.dart`（登录会话本地持久化：地址/token/user_id/email）
- `app/lib/pages/cloud_login_page.dart`（邮箱注册/登录页）、`cloud_devices_page.dart`（`/me/devices` + 添加设备认领）、`cloud_device_detail_page.dart`
- `app/lib/pages/root_page.dart`（底部标签 + `PopScope` 修复系统返回退出）
- `app/pubspec.yaml`（新增 `shared_preferences`）
- `cloud_service/src/cloud_service/security.py`（PBKDF2 密码哈希、token 生成、`Principal`）
- `cloud_service/src/cloud_service/models.py`（账号/事件模型）、`storage.py`（`accounts`/`account_tokens`/事件表与方法）、`main.py`（auth/me/events 端点、Bearer 鉴权与归属校验）、`mqtt_bridge.py`（门铃事件落库）
- `cloud_service/tests/test_auth.py`（账号鉴权与隔离）、`test_api.py`（事件）
- `cloud_service/Dockerfile`、`docker-compose.yml`、`.dockerignore`（自有服务器部署，容器仅监听 127.0.0.1）

## 9. 已知风险和禁止回归

1. **不得破坏门铃视频链路**：MJPEG 本地和中继必须保留。
2. **不得改变现有 MQTT topic 语义**：新协议应兼容旧 `demo/cmd`。
3. **不得给普通 Mesh 节点强制开启网关 BLE 配网**。
4. ESP32 节点固件 app 分区余量约 1%，新增组件前先检查尺寸。
5. WROOM brownout-off 仅用于 COM6 台架联调，不能复制到 S3 主线或量产配置。
6. BLE 配网只支持 2.4 GHz WiFi；错误提示不要暗示 ESP32 可连接 5 GHz。
7. 通知仅保证 App 前台和后台进程存活场景；进程被杀后不通知是当前确认需求。
8. 不提交 `build/`、生成的 `sdkconfig.*.generated`、日志、截图和临时脚本。
9. 当前 26 张图片是 RGB 棋盘格背景测试素材；正式发布前应替换为无平台标识、透明背景、状态一致的素材。
10. 窗帘、阀门、门锁等目前只复用 on/off 协议和板载 LED 指示，尚未实现真实电机、限位、继电器与安全保护。
11. BLE Mesh 与 Wi-Fi 可在同一 ESP32-S3 网关共存，但共用 2.4 GHz 射频；未经 30 分钟并行和命令压力测试不得标记为稳定。
12. COM14、COM15 仅保留历史异常记录，不再作为活动端口；不得把串口问题写成 BLE Mesh 失败。
13. 当前双板 BLE Mesh 证据来自 COM4 + COM8；只有一个灯节点，分组控制仍未测试。

## 10. 下一步优先级

1. 人工确认 COM6 重启后 GPIO2/D2 仍亮；随后为 COM8/COM6 增加并验证分组地址和 50 次单设备命令不误控。
2. 为 COM4 的本地 `sdkconfig` 配置 Wi-Fi/MQTT（不提交凭据），验证 MQTT→BLE→OnOff Status→ACK/reported state 闭环。
3. 在 MQTT/BLE 命令链路中复验 COM8 的 On 状态重启恢复，确保 GPIO48、Model state 和 NVS 状态一致。
4. 完成 Wi-Fi 重连、节点重启、网关重启和 BLE/Wi-Fi 30 分钟共存测试。
5. COM14、COM15 暂不排障，不阻塞 COM4 + COM8 + COM6 主线。
6. 完成 COM7 实体按键、60 秒外部视频、云端 ACK、heartbeat/LWT 和 10 分钟稳定性。
7. 检查 diff、显式暂存所需源码与文档、commit、push 并创建 PR。

### 下一阶段（已规划，见 `docs/PRODUCT_ARCHITECTURE_AND_ROADMAP.md`）

已确认四项决策：账号优先单一范式、配网即绑定、媒体走 WebRTC、对标 ESP RainMaker。任务已解耦（每个带依赖/边界/契约/验收自检），可多 AI 并行：
- P0：`T-APP-BACK`（返回键修复，本轮执行）、`T-APP-UX`（账号优先 UX 重构）。
- P1：`T-CLOUD-1` capability 模型、`T-CLOUD-2` MQTT 规范化+兼容双订阅、`T-CLOUD-3` OTA 服务端。
- P2：`T-FW-CONTRACT` 固件对齐协议、`T-FW-PROVISION-BIND` 配网回传 device_id、`T-FW-OTA-HTTPS`、`T-FW-OTA-MESH`。
- P3：`T-MEDIA-WEBRTC`、`T-APP-ADDDEV`。

## 11. 新会话恢复步骤

新会话不要直接改代码，按顺序执行：

1. 阅读本文件、`docs/DEVICE_ASSET_STATUS.md`、`docs/APP_DEVICE_ICON_PROMPTS.md` 和 `docs/SINGLE_DEVICE_ICON_PROMPTS.md`。
2. 执行 `git status --short`、`git branch --show-current`、`git log -1 --oneline`。
3. 先检查本文件“2026-07-14 当前联调状态”、本地 Git 状态和 COM4/COM8/COM6 当前固件，再继续节点持久化、双节点与 Wi-Fi/MQTT 验证。
4. 修改前阅读对应模块 README 和当前实现。
5. 完成后更新本文件的阶段状态、构建结果、已知问题和下一步。

## 12. 文档索引

- `docs/DEVELOPMENT_PROGRESS.md`：总进度和会话交接（本文件）。
- `docs/PRODUCT_ARCHITECTURE_AND_ROADMAP.md`：产品架构与协议规约 / 路线图（账号优先、配网绑定、WebRTC、OTA、解耦任务分点+验收要点，供多 AI 并行）。
- `docs/DEVICE_ASSET_STATUS.md`：已识别图片、缺失素材、App 映射、固件 `type` 协议和测试步骤。
- `docs/APP_DEVICE_ICON_PROMPTS.md`：设备图标视觉规范与类型描述。
- `docs/SINGLE_DEVICE_ICON_PROMPTS.md`：逐张生成的完整可复制提示词。
- `camera_stream/README.md`：门铃摄像头与 MJPEG 链路。
- `server_relay/README.md`：公网 MJPEG 中继部署。
- `internal_communication/办公区灯控技术方案.md`：Mesh 灯控设计。
