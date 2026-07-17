# 产品架构与协议规约 / 路线图（规划文档）

> **本文件是"从用户视角出发"的产品化规划与协议规约，供多 AI / 多人并行开发使用。**
> 每个任务都做了解耦（明确边界、依赖、接口契约、交付物、验收自检要点），可独立领取、独立验收。
> 本轮只做规划与文档；除"App 系统返回键修复"外，其余为待开发任务。

- 项目：`Mesh_esp32_farmely` / GitHub：`https://github.com/kell5/esp32_ble_mesh`
- 关联文档：`docs/DEVELOPMENT_PROGRESS.md`（真实进度）、`docs/MESH_HARDWARE_INTEGRATION_PLAN.md`（BLE Mesh/Wi-Fi 联调）、`cloud_service/README.md`（云端 API）
- 最近更新：`2026-07-13`
- 对标参考：**ESP RainMaker**（乐鑫官方 IoT 云：配网 + 用户-设备绑定 + 设备影子 + 本地控制 + OTA + 回滚，一整套，与本规划高度对应）、Matter（设备能力模型）、涂鸦/米家（账号即空间的 UX）、WebRTC（媒体流）。

---

## 0. 已确认的五项设计决策（本次讨论结论）

1. **账号优先单一范式**：账号是唯一入口，设备属于账号（以云端影子为准）；取消"本地/云端"双标签；本地直连 MQTT 降级为**透明的局域网加速通道**，用户无感知。
2. **配网即绑定（provision + bind）**：门铃等非 Mesh 设备在 BLE/SoftAP 配网时，App 顺带拿到 `device_id`，配网成功后**自动认领**到当前登录账号，闭环 onboarding。
3. **媒体流走 WebRTC**：信令走 MQTT、媒体走 WebRTC（产品化目标）；MJPEG 仅作 demo 保留。
4. **以 ESP RainMaker 模型为主要对标**：借鉴其配网/绑定/影子/OTA/本地控制的成熟模型，少走弯路。
5. **灯控采用 BLE Mesh + Wi-Fi 网关**：普通灯节点只运行 BLE Mesh；一块 ESP32-S3 N16R8 网关承担 Provisioner/Generic OnOff Client 和 Wi-Fi/MQTT bridge。门铃保持独立 Wi-Fi 链路。

---

## 1. 目标架构总览

```text
                     ┌──────────────────────────────────────┐
   App (Flutter)     │  账号(邮箱) = 空间；设备属于账号        │
   ── 登录/注册 ─────▶│  设备列表(云端影子为准)                │
   ── 配网+绑定 ─────▶│  控制：优先局域网直连，否则走云端       │
                     └──────────────────────────────────────┘
                                    │ HTTPS (Bearer token)
                                    ▼
                        cloud_service（自有服务器，Docker）
                        ├─ 账号/鉴权（已完成）
                        ├─ 设备注册 + 影子 desired/reported（已完成）
                        ├─ 账号-设备归属隔离（已完成）
                        ├─ 门铃事件（已完成）
                        ├─ [规划] OTA 固件仓库 + 灰度/回滚
                        └─ [规划] 媒体信令中转/TURN 协调
                                    │ MQTT (bridge)
                                    ▼
        ┌───────────────── MQTT Broker（第三方维护, 121.40.131.194）─────────────────┐
        │                               │                              │
   BLE Mesh + Wi-Fi 网关           门铃/摄像头(直连WiFi)          未来直连传感器/执行器
   ├ BLE Mesh Provisioner/Client  ├ 信令: MQTT                    └ 信令: MQTT
   ├ Wi-Fi + MQTT 上下行桥接       ├ 媒体: MJPEG(demo)→WebRTC(产品)
   └ 子节点状态聚合/离线缓存        └ OTA: esp_https_ota
        │
   BLE Mesh 灯控节点（无家庭 Wi-Fi/MQTT，经网关接云）
```

### 设备接入三分类（贯穿协议/配网/OTA）
| 类别 | 例子 | 信令传输 | 媒体传输 | 配网方式 | OTA 方式 |
|---|---|---|---|---|---|
| A. BLE Mesh 子设备 | 灯、开关、传感器 | BLE Mesh，经网关 MQTT 桥接 | 无 | 网关 Provisioner 配网（认领网关即可） | BLE Mesh OTA/网关分发（规划） |
| B. 直连 WiFi 低带宽 | 门铃信令、直连传感器 | MQTT 直连 | 无 | BLE/SoftAP 配网 + 自动绑定 | `esp_https_ota` |
| C. 媒体流设备 | 门铃视频、摄像头 | MQTT（信令） | WebRTC(产品)/MJPEG(demo) | BLE/SoftAP 配网 + 自动绑定 | `esp_https_ota` |

---

## 2. 统一设备能力模型（capability）

所有设备在云端都是 `device + shadow`，差异用 **capability** 描述（对齐 Matter cluster / 涂鸦 DP / HomeKit characteristic）。

建议 capability 命名（可扩展）：
- 执行器：`onoff`、`brightness`(0-100)、`color`(hsv/rgb)、`position`(0-100, 窗帘/阀门)
- 传感器：`sensor.temperature`、`sensor.humidity`、`sensor.motion`、`sensor.contact`、`sensor.smoke`
- 门铃/媒体：`doorbell.button`、`doorbell.ring`、`camera.stream`（含 `protocol: mjpeg|webrtc`、`url`/`signaling`）
- 系统：`system.fw_version`、`system.online`、`system.rssi`、`system.ota`

设备注册时上报 `type` + `capabilities[]`；App 按 capability 渲染控件（而非按写死的设备类型），新设备类型无需改 App。

---

## 3. 协议规约（MQTT）

> 目的：把现有 Mesh/门铃的 topic/payload 抽象成**统一、带版本、可幂等**的规范。非媒体设备一律 MQTT。

### 3.1 Topic 命名空间（建议标准化）
```
{root}/{class}/{device_id}/{direction}/{channel}
  root      固定前缀，如 farmely
  class     light | doorbell | gateway | sensor | camera
  device_id 全局唯一（MAC 或产品序列号）
  direction up（设备→云）| down（云→设备）
  channel   status | event | cmd | ota | shadow
```
示例：`farmely/light/node-0B55C0/up/status`、`farmely/light/node-0B55C0/down/cmd`。
> ⚠ 迁移约束：现有 Mesh/门铃 topic 已在固件与 broker 上线运行，**规范化需提供兼容期**（云端桥接同时订阅新旧 topic），不可一次性切断。详见任务 T-CLOUD-2。

### 3.2 Payload 规范（JSON）
统一信封：
```json
{
  "v": 1,                       // 协议版本
  "msg_id": "uuid",             // 幂等去重
  "ts": 1730000000,             // 设备/云时间戳(秒)
  "type": "status|event|cmd|ack",
  "data": { }                   // 与 capability 对应的键值
}
```
- QoS：状态/命令建议 QoS1；`retained` 仅用于"最后状态"类 topic。
- LWT：每设备注册遗嘱消息标记离线（Mesh 网关已实现，推广到直连设备）。
- 幂等：`msg_id` 去重（云端已实现命令幂等，设备侧上报也应带）。

### 3.3 媒体流（类别 C）
- **信令走 MQTT**：`.../up/event`（门铃按下/来电）、`.../down/cmd`（接听/挂断）、WebRTC 的 offer/answer/candidate 走专用信令 topic 或独立 signaling 服务。
- **媒体走 WebRTC**（产品目标）：SRTP 媒体、STUN 发现、**TURN 中转**（需自建 coturn 或用云服务）；实现 P2P + NAT 穿透，延迟低、带宽自适应、含音频。
- **MJPEG（demo 保留）**：局域网直连 + 公网 relay，简单但无音频、带宽大、难多路，仅演示用。
- **回看录像**：分段 MP4 + 对象存储 + HLS 点播（产品阶段）。

---

## 4. 用户体验重构（账号优先）

### 4.1 目标流程
```
安装打开 App
  → 有本地会话? ──否──▶ 登录/注册页（作为根页面）
  │                         └ 注册成功→自动登录
  └是─▶ 拉取账号设备
          ├ 有设备 → 设备列表（统一卡片，capability 渲染）
          └ 无设备 → 空白引导页（大按钮"添加设备"）
添加设备
  ├ 类别A(Mesh): 认领网关 → 子节点自动出现
  └ 类别B/C(WiFi): BLE/SoftAP 配网 → 拿 device_id → 自动 claim
控制
  └ 优先局域网直连(低延迟)，不可达时走云端；对用户是同一份设备、同一套操作
```

### 4.2 与现状的差异
- 删除"我的设备 / 云端"双标签；只保留一份"我的设备"（数据源=账号）。
- 直连 MQTT 由"独立标签"变为"控制传输层"，由 App 内部按可达性择优，不暴露给用户。
- 登录页从"某标签里的按钮"提升为"未登录时的应用根页面"。

---

## 5. 设备侧开发规范（固件）

> 让固件与云端/App 契约一致，避免"设备各写各的"。新设备开发必须遵守。

1. **身份**：每台设备有全局唯一 `device_id`（建议 MAC 或 `产品前缀-序列号`），出厂即固定；上电首次连接必须"自报"一次注册消息（`type=status` 全量），云端据此自动注册。
2. **能力声明**：注册消息里带 `type` + `capabilities[]`；App 按能力渲染，固件新增功能只需加 capability。
3. **上报**：状态变化即上报（QoS1 + `msg_id`）；周期心跳维持 `online`；断线由 LWT 置 `offline`。
4. **命令**：订阅 `.../down/cmd`，执行后回 `ack`（带原 `msg_id`），保证幂等（重复命令不重复执行副作用）。
5. **配网（类别B/C）**：BLE/SoftAP 配网必须在配网通道里**回传 `device_id`**（供 App 自动绑定），并可选返回一次性 `claim_code`。
6. **版本**：上报 `system.fw_version`、`hw_version`、`product_id`；OTA 依赖这三元组做灰度匹配。
7. **OTA**：
   - 类别B/C：实现 `esp_https_ota`，从云端下发的 HTTPS URL 拉取；**双分区 + 回滚**（`esp_ota_mark_app_valid_cancel_rollback`）；升级前后上报状态。
   - 类别A(Mesh)：见任务 T-FW-OTA-MESH（root 下载→Mesh 分发→子节点写入回滚）。
   - **安全**：固件镜像签名校验，条件具备时开启 secure boot / flash 加密。
8. **协议版本**：payload 带 `v` 字段；固件与云端按版本协商，旧版本兼容期内不破坏。

---

## 6. OTA 方案（行业主流 + 本项目落地）

### 6.1 直连设备（类别 B/C）
- `esp_https_ota` 拉取；双 OTA 分区；启动自检通过再 `mark_app_valid`，否则回滚；断点续传可选。

### 6.2 Mesh 设备（类别 A）
- 采用 ESP 官方 mesh OTA 思路：**root 节点从服务器下载固件 → 经 Mesh 网络分块分发给子节点 → 子节点校验写入 → 失败回滚**。
- 参考 `esp-mesh-lite` / `esp_mesh` OTA example。要点：分块传输可靠性、子节点异构（不同 `product_id` 用不同镜像）、灰度分批、断电续传。

### 6.3 云端 OTA 管理（服务端）
- `firmware` 表：`product_id + hw_version + fw_version + url + sha256 + sign`。
- `rollout` 规则：按 `(product_id, hw_version, 当前fw_version)` 决定推送目标 + 灰度比例。
- 设备上报当前版本 → 云端比对 → 通过 `.../down/ota` 下发升级指令（含 URL + 校验）。
- 记录升级进度/结果，失败告警。

### 6.4 多设备不同功能适配
- 用 `(product_id, hw_version)` 区分硬件；固件内用 Kconfig/capability 编译差异功能。
- OTA 服务端按三元组精确匹配镜像，杜绝"错刷"。

---

## 7. 任务分解（解耦，可多 AI 并行）

> 约定：每个任务标注 **依赖**（前置任务）、**边界**（只动哪些文件/模块）、**接口契约**、**交付物**、**验收自检要点**。
> 无依赖关系的任务可并行；共享文件的任务需串行或明确分区避免冲突。

### 分组 P0（立即，本轮部分执行）

#### T-APP-BACK — 修复 Android 系统返回键直接退出
- 依赖：无。边界：`app/lib/pages/root_page.dart`（仅此文件）。
- 根因：`PopScope` 被放在内层 `Navigator(_navKey)` 的路由里，而 Android 系统返回由 `CupertinoApp` 的**根导航器**分发；根导航器只有一个路由、没有 `PopScope`，`maybePop` 返回 false → 系统直接退出应用。
- 方案：把 `PopScope(canPop:false)` 提升到 **`RootPage.build` 根级**（注册到根导航器），在回调里按序处理：① `_navKey` 可 pop（关来电页/根级页）→ pop；② 当前 tab 内层导航器可 pop → pop；③ 不在首个 tab → 切回首个 tab；④ 已在首页 → 弹确认框，确认才 `SystemNavigator.pop()`。
- 契约：不改变来电覆盖逻辑（`_showIncomingCall` 仍走 `_navKey`）。
- 验收自检：`flutter analyze` 0 问题；真机（gesture 导航 + 三键导航）：详情页返回→回列表；子 tab 返回→回首个 tab；首页返回→弹确认，取消不退出、确认才退出；左滑手势与硬件键行为一致。

#### T-APP-UX — 账号优先 UX 重构（删双标签、登录为根、空态引导）
- 依赖：无（可与 T-APP-BACK 串行，同改 `root_page.dart`）。边界：`app/lib/pages/root_page.dart`、`cloud_login_page.dart`、`cloud_devices_page.dart`、新增 `home shell`；`home_page.dart`(原直连 MQTT)降级为传输层或并入统一列表。
- 契约：设备列表数据源=账号（`GET /me/devices`）；本地直连作为控制传输，UI 只有一份设备。
- 验收自检：未登录启动直达登录页；登录后有设备→列表、无设备→空态"添加设备"；返回键行为符合 T-APP-BACK；`flutter analyze` 0 问题。

### 分组 P1（协议与后端，自包含）

#### T-CLOUD-1 — capability 设备模型落地
- 依赖：无。边界：`cloud_service/src/cloud_service/{models,storage,main}.py` + 测试。
- 契约：设备注册/影子支持 `capabilities[]`；`GET /me/devices` 返回 capability；向后兼容旧无 capability 设备。
- 验收自检：ruff 干净；新增测试覆盖 capability 读写；旧设备读取不报错。

#### T-CLOUD-2 — MQTT topic/payload 规范化 + 兼容期双订阅
- 依赖：T-CLOUD-1（capability）。边界：`mqtt_bridge.py` + 文档 §3 + 测试。
- 契约：云端**同时**订阅新命名空间与现有 topic；payload 统一信封（`v/msg_id/ts/type/data`）；幂等去重。
- 验收自检：给出新旧两种 topic 的单测均能入影子；现有 Mesh/门铃链路回归不破坏（`test_api`/`test_organization` 全过）。

#### T-CLOUD-3 — OTA 服务端（固件仓库 + 灰度 + 下发）✅ 已完成
- 依赖：T-CLOUD-1。边界：新增 `firmware`/`rollout` 存储 + 端点 + MQTT `down/ota` 下发 + 测试。
- 契约：`(product_id, hw_version, fw_version)` 匹配；灰度比例；下发含 `url+sha256+sign`。
- 验收自检：ruff + 单测（匹配/灰度/幂等下发）；无固件时不误下发。
- 落地：`storage.py` 新增 `firmware`/`rollouts`/`ota_updates` 表与方法；`models.py` 新增 OTA 请求/响应；`ota.py` 新增 `OtaEngine`（匹配→确定性灰度→幂等下发）；`mqtt_bridge.py` 新增 `publish_ota` 与版本上报触发；`main.py` 新增 `/api/v1/firmware`、`/api/v1/rollouts`、`/devices/{id}/ota/{check,progress,updates}`。
- 结果：`ruff check` 干净；`unittest` 全绿（50 项，新增 `tests/test_ota.py` 15 项，覆盖匹配/灰度/幂等/无固件不误下发/版本上报触发/进度回报）。设备侧 `esp_https_ota`+双分区回滚由 T-FW-OTA-HTTPS/T-FW-OTA-MESH 承接。

### 分组 P2（设备侧固件）

#### T-FW-CONTRACT — 固件对齐统一协议（信封/能力/心跳/LWT/ack）
- 依赖：T-CLOUD-2。边界：门铃 `camera_stream`、网关 `internal_communication`。
- 验收自检：设备上电自报注册被云端自动注册；命令 `ack` 幂等；断线 LWT 置离线。

#### T-FW-PROVISION-BIND — 配网回传 device_id（支撑自动绑定）
- 依赖：无（固件侧）。边界：门铃 BLE 配网特征。
- 验收自检：配网流程中 App 能读到 `device_id`；配网成功后设备上线并被自动 claim（联调 T-APP-ADDDEV）。

#### T-FW-OTA-HTTPS — 直连设备 esp_https_ota + 双分区回滚
- 依赖：T-CLOUD-3。状态：`rgb_light` 已实现直连样例（云端下发 url/sha256/fw_version/msg_id；设备流式校验 sha256；MQTT 上报 downloading/success/failed；新固件 MQTT 上线后再确认 rollback）。验收自检仍需在可用 ESP-IDF 环境下重新 build/flash，并做真实板 OTA 成功/失败回滚。

#### T-FW-OTA-MESH — Mesh OTA（root 下载 + 分发 + 子节点回滚）
- 依赖：T-CLOUD-3。验收自检：网关下所有子节点可分批升级、失败回滚、异构镜像不错刷。

### 分组 P3（媒体流产品化）

#### T-MEDIA-WEBRTC — 门铃/摄像头 WebRTC（信令 MQTT + TURN）
- 依赖：T-FW-CONTRACT。边界：设备侧 WebRTC + 云端信令/TURN 协调 + App 播放器。
- 验收自检：公网跨 NAT 可拉流、含音频、延迟明显低于 MJPEG；MJPEG 作为回退保留。

#### T-APP-ADDDEV — App 配网 + 自动绑定 + capability 渲染
- 依赖：T-FW-PROVISION-BIND、T-CLOUD-1。边界：App 添加设备流程。
- 验收自检：BLE 配网→自动 claim→设备出现在列表；控件按 capability 渲染。

---

## 8. 全局验收 / 检测要点（合并阶段用）

- 后端：`ruff check` 干净；`python -m pytest`（或既有 unittest）全过；`/health` `mqtt_connected=true`。
- App：`flutter analyze` 0 问题；账号优先流程走通；返回键不误退。
- 协议：新旧 topic 兼容期内并存；payload 带 `v/msg_id`；幂等生效。
- 设备：上电自报注册；LWT 离线；命令 ack；OTA 可回滚。
- 安全（demo→产品演进）：Bearer token 传输经 HTTPS；产品阶段补 token 过期/刷新、固件签名、broker ACL/TLS。
- 隔离：非管理员只能见/控自己账号设备（已实现，回归勿破坏）。

---

## 9. 备注：demo 与产品的边界
- 当前为 demo 级：无邮箱验证/找回密码/token 过期；MJPEG 媒体；无固件签名。
- 产品化增量（不改变以上架构）：账号安全强化、WebRTC 媒体、OTA 灰度/签名、broker ACL/TLS/凭据轮换、Matter/Thread 演示节点。
