# BLE Mesh + Wi-Fi 硬件联调计划与验收标准

> 最近更新：2026-07-16
> 目的：在刷写设备前固定硬件映射、目标架构、执行顺序、结果判定和记录格式。  
> 本文中的“通过”必须有构建输出、串口日志和可观察结果支撑；仅编译成功不等于无线 Mesh 联调成功。

## 1. 当前硬件矩阵

| 角色 | 硬件 | 串口 | MAC | 灯/外设 | 2026-07-16 实测状态 |
|---|---|---|---|---|---|
| 门铃 | ESP32-S3 **N8R8** + 摄像头 | COM7（CH340） | `dc:da:0c:4c:34:08` | OV3660 摄像头、GPIO0 门铃按键 | 在线：`camera_stream`(253c8c5)；Wi-Fi 连 `Xiaomi`、IP `10.50.212.74`；MQTT 已连 `door-001`；快照命令下行 + ACK 上行往返通过 |
| BLE Mesh 网关 | ESP32-S3 N16R8 | COM4（原生 USB-JTAG） | `e0:72:a1:d3:15:48` | GPIO48 板载 WS2812 RGB | 在线：Provisioner 初始化；重启后自动查询并让 `0x0005`/`0x0006` 重新订阅 `0xC000`，fresh GET 成功；**Wi-Fi 未配置**（`CONFIG_FARMELY_WIFI_SSID=""`），MQTT bridge 未联网 |
| BLE Mesh 灯节点（`0x0005`） | ESP32-S3 N16R8 | COM9（CH340） | `14:c1:9f:cb:59:e8` | GPIO48 板载 WS2812 RGB | 在线：mesh 内响应网关 GET，onoff=`0x01`；应用控制台走原生 USB-JTAG，CH340 口仅见 ROM/bootloader |
| BLE Mesh 灯节点（`0x0006`） | ESP32-WROOM (D0WD-V3) | COM8（CH340） | `b4:bf:e9:0b:55:c0` | D2/GPIO2 | 在线：`onoff_server`(0e9520c)，已配网 `addr 0x0006`，onoff=`0x00` |

> ⚠ 串口号相较 2026-07-14 已整体变化（门铃回插、S3 灯节点移到 COM9、WROOM 移到
> COM8）。串口号不等于永久产品角色；每次刷写前必须用 esptool 重新读取芯片型号/Flash/PSRAM/MAC，
> 并结合固件项目名与 Mesh 地址确认。本轮映射由 esptool `flash-id` + 各口启动日志确认。
> broker 使用 `mqtt://121.40.131.194:1883`；开发 VM 可直连该 broker。

### 1.1 WROOM 供电告警处理边界

- ESP32-WROOM 专用构建设置 `CONFIG_ESP_BROWNOUT_DET=n`，本轮目标为 COM7（历史为 COM6）。
- 关闭 brownout 只是不再触发欠压复位，不会改善供电质量。
- 该配置不能作为量产默认值；欠压时仍可能发生随机重启、异常执行或 Flash 写入损坏。
- 验收记录必须保留“brownout 已关闭”的风险说明；量产前应更换稳定电源、USB 线或供电设计并恢复 brownout 保护。

## 2. Mesh 方案结论

### 2.1 目标架构

```text
                        HTTPS / MQTT
App / cloud_service  <---------------->  MQTT Broker
                                                |
                                                | Wi-Fi STA
                                                v
                                   COM4 ESP32-S3 N16R8 网关
                                   - BLE Mesh Provisioner
                                   - Config Client
                                   - Generic OnOff Client
                                   - Wi-Fi + MQTT bridge
                                                |
                                          BLE Mesh
                           +--------------------+--------------------+
                           |                                         |
                    COM8 ESP32-S3 N16R8 灯节点              后续 N16R8 灯节点
                    GPIO48 WS2812 RGB                        GPIO48 WS2812 RGB
                    Generic OnOff Server                     Generic OnOff Server

门铃 ESP32-S3 N8R8：独立 Wi-Fi/MQTT/视频链路，不加入 BLE Mesh。
COM6 WROOM 作为第二灯节点；COM14、COM15 不再作为活动串口。
```

普通灯节点只运行 BLE Mesh，不连接家庭 Wi-Fi，也不运行 MQTT。网关负责：

1. 发现、配网并配置 BLE Mesh 节点；
2. 将 MQTT 单设备/分组命令映射到 BLE Mesh Generic OnOff；
3. 将节点 OnOff Status、在线状态和标识映射回统一 MQTT 协议；
4. 在 Wi-Fi 短时中断时保留本地 BLE Mesh 控制能力，恢复后补发最新状态。

当前阶段暂不进行 Flutter App 联调；App 仍为 PR #4 合并后的版本，BLE Mesh 新网关的发现、设备映射和控制界面留到固件链路稳定后再对接。

### 2.2 BLE 与 Wi-Fi 能否结合

可以。工作区中的乐鑫官方示例明确支持 ESP32 和 ESP32-S3：

- `esp_ble_mesh/provisioner`：Provisioner + Generic OnOff Client；
- `esp_ble_mesh/onoff_models/onoff_server`：Generic OnOff Server；
- `esp_ble_mesh/wifi_coexist`：BLE Mesh 与 Wi-Fi 同芯片共存参考。

共存不代表无需测试。BLE 和 Wi-Fi 共用 2.4 GHz 射频，网关必须验证高频 MQTT 收发、Wi-Fi 重连和 BLE Mesh 控制同时发生时的延迟、丢包和稳定性。普通灯节点不同时运行 ESP-WIFI-MESH 与 BLE Mesh，以减少内存、Flash 和射频竞争。

### 2.3 ESP-WIFI-MESH 当前验证结论

当前不能判定为“已完整验证成功”，应记为**代码完成、历史硬件部分验证、正式验收未完成**：

| 项目 | 当前证据 | 结论 |
|---|---|---|
| 网关/节点代码 | root、节点表、广播、单节点寻址、状态上报和 MQTT bridge 已实现 | 代码完成 |
| 固件构建 | ESP32-S3 gateway 与 ESP32 node 构建通过 | 构建通过 |
| 历史硬件记录 | 旧文档记载 COM14 曾烧录节点并能自动入网/上报 | 历史部分验证 |
| 当前可复现证据 | 没有保留完整的 root + 多节点 UART 日志、测试命令和逐项验收结果 | 不足 |
| 云端 MQTT 冒烟 | 新旧 MQTT、影子、ACK、认领/移除已验证 | 仅证明云端和 MQTT，不证明 ESP-WIFI-MESH 射频链路 |

若要把 ESP-WIFI-MESH 标记为正式通过，仍需 root + 至少两个节点完成自动组网、单播、广播、断电重连、离线判定和持续运行测试。该实现保留为回归基线与回退方案，但新灯控主线转向 BLE Mesh。

## 3. 联调执行顺序

### 阶段 0：文档与基线

1. 提交本文和总进度/架构文档更新。
2. 枚举串口，读取 COM7、COM4、COM8、COM6 的芯片型号、Flash、PSRAM，不凭端口号猜测硬件。
3. 保存现有固件启动日志，确认当前故障与功能基线。

### 阶段 1：历史 COM7 门铃基线（当前已拔下）

1. 按 ESP32-S3 N8R8 配置构建门铃固件；`camera_stream/sdkconfig.defaults` 保持 8 MB Flash 配置。
2. 刷写 COM7 并监控启动。
3. 验证摄像头初始化、Wi-Fi/MQTT、heartbeat/LWT、门铃事件、命令 ACK、本地和中继视频。
4. 门铃验证通过后不得为 BLE Mesh 改动其无线架构。

### 阶段 2：N16R8 BLE Mesh 灯节点

1. 从乐鑫官方 Generic OnOff Server 复用模型、配网和持久化流程。
2. ESP32-S3 N16R8 使用 GPIO48 WS2812 RMT 驱动、单元素 Generic OnOff Server 和 BLE Mesh settings。
3. COM8 作为当前 N16R8 主节点。
4. COM6 WROOM 使用 GPIO2、brownout-off 和 BLE Mesh settings，作为第二节点补充分组控制。

### 阶段 3：N16R8 BLE Mesh + Wi-Fi 网关

1. 从官方 Provisioner 和 Wi-Fi coexist 示例复用初始化与回调。
2. 网关提供 Provisioner、Config Client、Generic OnOff Client。
3. 接入现有 MQTT 新旧 topic 映射，避免改变云端协议。
4. 配置 GPIO48 WS2812 作为网关状态指示。
5. 自动发现并配网 N16R8 节点，完成 NetKey/AppKey、Composition Data、Model App Bind 和 Generic OnOff Get/Set/Status。

### 阶段 4：多节点与共存压力

1. 至少再接入一块 N16R8 Generic OnOff Server。
2. 验证单节点寻址、分组控制、状态上报、重启恢复。
3. 同时运行 Wi-Fi/MQTT 与 BLE Mesh 控制压力测试。
4. 记录失败项、复现步骤、日志时间点和是否阻断迁移。

## 4. 结果判定标准

### 4.1 历史 COM7 门铃

| 验收项 | 通过标准 |
|---|---|
| 硬件识别 | 读取结果为 ESP32-S3，Flash 8 MB、PSRAM 8 MB；与 N8R8 相符 |
| 稳定启动 | 连续运行 10 分钟无 panic、watchdog、循环重启 |
| 摄像头 | OV3660 初始化成功；连续观看本地 MJPEG 60 秒无崩溃 |
| Wi-Fi/MQTT | 获取 IP，MQTT connected；heartbeat/status 可在 broker/cloud 观察 |
| 门铃事件 | 连续按键 3 次，3 次事件均到达云端且不重复 |
| 云端命令 | 至少执行 1 次支持的命令并收到对应 ACK |
| 公网链路 | 中继健康时可观看 60 秒；若外部服务不可用，明确记为环境阻塞，不算固件通过 |

### 4.2 BLE Mesh 灯节点

| 验收项 | 通过标准 |
|---|---|
| 硬件识别 | N16R8 读取结果为 ESP32-S3、Flash 16 MB、PSRAM 8 MB；WROOM 旧基线单独记录 |
| GPIO 驱动 | N16R8 通过 RMT 驱动 GPIO48 WS2812；不得用普通 GPIO 电平代替 WS2812 时序 |
| 基础稳定性 | 连续运行 10 分钟无 panic、watchdog、循环重启 |
| BLE Mesh 配网 | 网关 60 秒内发现，120 秒内完成 provisioning、AppKey 添加和 Model Bind |
| 灯控 | 20 次交替 On/Off 全部正确驱动目标灯，状态回复与实灯一致 |
| 持久化 | 节点重启后 60 秒内恢复网络，无需重新 provisioning |

### 4.3 BLE Mesh + Wi-Fi 网关

| 验收项 | 通过标准 |
|---|---|
| 双协议并行 | BLE Mesh 与 Wi-Fi/MQTT 同时在线 30 分钟，无 panic、watchdog、内存持续下降 |
| 单设备控制 | 50 条云端单设备命令目标正确率 100%，不得误控其他节点 |
| 分组控制 | 至少两个节点时执行 20 轮分组开关，所有目标一致；只有一个节点时标记“未测试”，不能写“通过” |
| 状态闭环 | 每条确认命令最终产生 ACK/OnOff Status 并同步到 cloud shadow |
| 延迟 | 本地 BLE Mesh 命令 P95 ≤ 1 秒；云端 MQTT 到灯状态闭环 P95 ≤ 3 秒 |
| Wi-Fi 重连 | 断开并恢复 AP 后 MQTT 自动重连；BLE Mesh 本地控制不因 Wi-Fi 中断永久失效 |
| 节点重启 | 任一节点重启后 60 秒内恢复；网关不重复创建节点或错配地址 |

### 4.4 结果等级

- **通过**：所有必测项满足标准，有日志和观察结果。
- **部分通过**：核心路径通过，但多节点、外部服务或压力项因条件不足未测；必须列出未测项。
- **失败**：出现误控、无法恢复、持续重启、状态不一致或关键链路不通。
- **阻塞**：缺少硬件、电源、网络、凭据或外部服务；不得把阻塞写成通过。

## 5. 联调结果记录

| 日期 | 固件提交 | 设备/串口 | 测试范围 | 结果 | 证据/备注 |
|---|---|---|---|---|---|
| 2026-07-13 | `253c8c5` | COM7 门铃 N8R8 | 识别、构建、刷写、启动、摄像头、Wi-Fi、MQTT | 部分通过 | 实测 Flash 8 MB/PSRAM 8 MB；OV3660 和 HTTP 服务初始化成功；Wi-Fi 获取 IP、MQTT connected。尚未完成实体按键 3 次、60 秒视频观看和云端命令 ACK |
| 2026-07-13 | 本轮 BLE Mesh 分支 | COM15 WROOM | BLE Mesh Generic OnOff Server 离线构建 | 已弃用 | GPIO2 单灯、settings、Generic Server、`CONFIG_ESP_BROWNOUT_DET=n` 构建通过；串口始终无下载数据，用户决定不再使用该板 |
| 2026-07-13 | 本轮 BLE Mesh 分支 | COM14 N16R8 | 节点构建、刷写、启动诊断 | 阻塞 | 16 MB Flash/8 MB PSRAM 识别和固件写入校验通过；`farmely_node` NVS 始终为空。同一节点固件在 COM4 可运行到 BLE Mesh 初始化，当前结论为 COM14 启动/复位路径阻塞，不是 BLE Mesh 射频失败 |
| 2026-07-13 | 本轮 BLE Mesh 分支 | COM4 网关 + COM8 节点 | Provisioning、Composition Data、AppKey、Model Bind、Generic OnOff Get/Set/Status | 部分通过 | COM4 NVS 达到 `farmely_mesh:stage=8, addr=5, onoff=1`；COM8 达到 `farmely_node:stage=7, addr=5, onoff=1`，证明完整双板 BLE Mesh 控制链路通过 |
| 2026-07-14 | 本轮 BLE Mesh 分支 | COM4 网关 + COM8 节点 | 网关重启、节点重启和 settings 恢复 | 部分通过 | COM8 已刷入最新节点固件，重启后保持 `stage=5, addr=5, onoff=0`；COM4 保留 NVS 重刷最新网关后自动查询节点并达到 `stage=9`，未重复 Provisioning。COM8 的 On 状态实灯恢复仍待命令链路复验 |
| 2026-07-14 | 本轮 BLE Mesh 分支 | COM4 N16R8 网关 | Wi-Fi STA + MQTT bridge、normalized/legacy topic、ACK/status 映射 | 代码完成/运行未验收 | 网关固件构建成功，`provisioner.bin` 824896 bytes、app 分区剩余 46%，并已刷入 COM4；当前配置未写入 Wi-Fi/MQTT 凭据，因此不能标记联网、云端闭环或共存压力通过 |
| 2026-07-14 | 本轮 BLE Mesh 分支 | COM6 WROOM | 串口识别、构建、刷写、Provisioning、配置和重启恢复 | 部分通过 | esptool 识别 ESP32-D0WD-V3 rev 3.1、4 MB Flash；WROOM 固件 811424 bytes、app 分区剩余 47%；已配网为地址 `0x0006`，重启后保持 `farmely_node:stage=7, addr=6, onoff=1`。尚待人工确认 GPIO2/D2 实灯状态 |
| 2026-07-14 | 本轮 BLE Mesh 分支 | COM4 网关 + COM8/COM6 双节点 | 双节点记录与网关重启恢复 | 部分通过 | 网关 NVS 同时包含 `pn/0005`、`pn/0006` 和持久化 `nodes`；重启查询后达到 `farmely_mesh:stage=9, addr=6, onoff=1`。已证明两个节点地址不重复，分组地址和 50 次定向无误控尚未测试 |
| 2026-07-14 | 本轮 BLE Mesh 分支 | COM4 网关 + 已保存双节点 | `0xC000` 组订阅、group multicast、20 轮分组/50 条定向自测 | 未通过 | 默认和自测固件均构建通过，app 分区剩余 46%；启动日志曾确认 `0x0005`、`0x0006` 订阅 `0xC000`。正式自测时 `0x0005` 离线，分组 `0/20`、定向 `25/50`，其中在线 `0x0006` 的 25 条定向命令均成功。需恢复 `0x0005` 在线后重测，不能记录为分组或 50 条无误控通过 |
| 2026-07-14 | 本轮 BLE Mesh 分支 | COM4 网关 + COM8/`0x0005` + COM6/`0x0006` | 双节点在线、`0xC000` group multicast、20 轮分组、50 条定向及非目标复查、状态恢复、生产固件恢复 | 通过（逻辑状态） | COM8 恢复在线后真实台架结果为 `group=20/20 directed=50/50`，没有分组、定向或在线预检失败日志；结束状态恢复为 `0x0005=0`、`0x0006=1`。COM4 随后刷回默认关闭自测的 827552-byte 生产固件，冷启动确认 Provisioner 初始化且两节点重新订阅 `0xC000`。COM4/COM6/COM8 均已释放，COM7 未打开。物理 WS2812/GPIO2 灯态、MQTT/cloud 与共存压力仍未验收 |
| 2026-07-14 | 本轮 BLE Mesh 分支 | COM4 网关 + COM8/`0x0005` + COM7/`0x0006` | 慢速组控、定向物理隔离、节点回调、状态恢复、生产固件恢复 | 通过（一次人工物理验收） | COM4 结果 `group=4/4 directed=4/4`；用户在完整序列结束后确认“灯光正常”。COM7 同步记录到交替 `onoff 0x00/0x01` 的 Generic Server state-change callback。结束恢复 `0x0005=1`、`0x0006=0`；COM4 刷回 827552-byte 生产镜像并校验 hash，冷启动重新订阅两节点、fresh Get `(1,0)`，无临时自测输出，COM4/COM7/COM8 均已释放。早一轮曾观察 `D2常亮，48长灭`，仍需多次断电/冷启动复测后才能声明长期稳定 |

| 2026-07-16 | 现场固件（未重刷） | COM4/7/8/9 全部在线 | 芯片重识别、四板现状、门铃 MQTT 往返、mesh 在线预检 | 部分通过 | esptool 重识别：COM4=S3 16MB(网关,原生USB)、COM7=S3 **8MB N8R8**(门铃)、COM8=ESP32 4MB WROOM、COM9=S3 16MB(灯节点)。门铃在线并连 broker，`snapshot` 命令经 `doorbell/door-001/cmd` 与 `farmely/doorbell/door-001/down/cmd` 均收到 `ack_msg_id` 回执（往返通过）。网关重启后 `0x0005`(onoff=1)/`0x0006`(onoff=0) 均在线、重订阅 `0xC000`、fresh GET 成功（此前 `0x0005` 离线的阻塞点已消除）。**未做**：网关 Wi-Fi 未配置故 MQTT→mesh 云端闭环、group/定向实体灯态、门铃实体按键 ringing/视频观看均未验收 |

| 2026-07-16 | 网关 SoftAP App 配网固件（COM4 已刷 3MB 分区版） | COM4 网关 + 荣耀手机 | SoftAP 运行时配网、手机关联/App 配网、掉线排查 | 未通过（待续） | 网关固件已实现 `network_provisioning`+`scheme_softap`，串口实证热点 `Gateway-D31548`@192.168.4.1 开启、DHCP 起、BLE Mesh 与配网共存（`0x0005/0x0006` 订阅 `0xC000`）。手机侧现象：能搜到 SSID，能成功关联（`station <randMAC> join, AID=1`），但每次约 4~18 秒后被手机主动断开（`leave ... reason = 3` = STA leaving；随后 `removing station after unsuccessful auth/assoc`）。有浏览器流量时链接可维持到 ~18s 并建立数据 BA（`tid:6`），说明关联与数据通路本身正常。全程未见任何 protocomm/HTTP 配网请求到达网关，也未连上 Xiaomi、未起 MQTT。根因判定：荣耀 MagicOS 对无互联网热点自动断开过快，手动多步操作来不及完成配网；非网关拒绝、非 PoP、非 App 逻辑错误 |

## SoftAP App 配网调查（2026-07-16，进行中）

### 已确认证据
- 网关侧：`network_prov_mgr: Provisioning started with service name : Gateway-D31548`，SoftAP@192.168.4.1、DHCP server started、BLE Mesh 与配网共存正常。
- 手机侧（荣耀 / MagicOS，随机 MAC `66:69:53:3a:29:b3`）：`station ... join, AID=1` 关联成功 → 约 4~18s 后 `station ... leave, AID = 1, reason = 3`（reason 3 = 802.11 STA is leaving，手机主动离开）→ `removing station after unsuccessful auth/assoc`。
- 有主动流量（浏览器访问 192.168.4.1）时连接维持更久（~18s）并建立数据 BA（`tid:6`），证明关联+数据通路正常。
- 全程串口未见 protocomm/HTTP 配网请求，也未 `NETWORK_PROV_WIFI_CRED_RECV`、未连 Xiaomi、未起 MQTT。
- 构建里共存已启用：`CONFIG_ESP_COEX_SW_COEXIST_ENABLE=y`、`CONFIG_SW_COEXIST_ENABLE=y`——排除“缺共存”这一项。

### 结论
问题在手机系统对无互联网 SoftAP 的自动断开（断得过快，手动 App 步骤来不及），不是网关拒绝、不是 PoP、不是 App 配网协议逻辑错误。

### App 侧本轮发现/修复
- 已修复：`app/lib/pages/cloud_devices_page.dart` 两处 `CupertinoButton(color: activeBlue)` 蓝底蓝字不可见——Flutter 3.44.1 中普通 `CupertinoButton` 前景色取 `primaryColor`(=activeBlue)，故与蓝底同色；改为显式 `foregroundColor: CupertinoColors.white`。（`.filled` 用 `primaryContrastingColor` 故本就正常。）
- 待处理：`app/lib/pages/provisioning_page.dart` 切到“热点兼容”时 PoP 仍是门铃默认 `doorbell1234`，网关需 `gateway1234`（现阶段需手动改）。
- 待处理：`app/lib/prov/transport_http.dart` 用普通 `http.Client()`，未把请求绑定到热点网络（Android 有移动数据时可能走默认网/被系统判为无用而断开）。

### 明天的修复方向（择一或组合）
1. 网关加 Captive-Portal 保活（推荐，手机/App 无关）：SoftAP 上加 DNS catch-all + HTTP `/generate_204`(Android)、`/hotspot-detect.html`(Apple) 应答，让系统认为“有网”从而不自动断开；需重编重刷 COM4（注意 network_provisioning 已占用 80 端口 httpd，需要额外 URI handler/并行 DNS server）。
2. App 内置连接热点 + 网络绑定：用插件/平台通道 `ConnectivityManager.requestNetwork`+`bindProcessToNetwork` 程序化连接 SoftAP 并保活，连上后立即驱动 protocomm；顺带修复上面 PoP 与 transport 绑定问题。
3. 兜底手动流程：关移动数据 + 保持连接 + 尽快在“热点兼容”页把 PoP 改 `gateway1234` 后立刻下发（不稳定，仅应急）。

每轮联调结束后更新本表，并在 `docs/DEVELOPMENT_PROGRESS.md` 中只写已经有证据的结论。


## SoftAP App 配网调查（2026-07-17，进行中：三处根因已修，待复测）

上一轮把"掉线"归因为"手机对无网热点断得太快"并选了方案1（Captive-Portal 保活）。本轮实现保活后抓串口，逐步定位到**三个真正的固件级根因**，均已修复：

### 根因1：与 protocomm 共享 httpd 崩溃（LoadProhibited bootloop）
- 现象：刷入保活固件后网关一起步就 panic 重启，SoftAP/门户随崩溃消失，手机连上几秒即断。
- addr2line：`httpd_find_uri_handler` ← `protocomm_httpd_add_endpoint` ← `network_prov_mgr_start_service` ← `gateway_wifi_task`。
- 根因：`network_prov_scheme_softap_set_httpd_handle()` 内部按 `*(httpd_handle_t*)` 再解引用一层，必须传"句柄变量的地址"`&s_prov_httpd`，误传句柄值 → 野指针崩溃。
- 修复：`gateway_bridge.c` 改为 `network_prov_scheme_softap_set_httpd_handle(&s_prov_httpd);`（commit a476d6b）。
- 验证：重刷后干净启动，SoftAP+captive+DNS(53)+BLE Mesh(0x0005/0x0006 订阅 0xC000) 共存，无 panic。

### 根因2：配网期 STA 重连风暴致 DHCP 卡死（手机"一直获取IP地址"）
- 现象：手机能关联（join AID=1）但拿不到 IP，卡在"正在获取 IP 地址"，~18s 后 leave reason=3；串口无 DHCP 分配。
- 根因：`wifi_event_handler` 在 STA_START 无条件 `esp_wifi_connect()`、STA_DISCONNECTED 又重连；配网阶段无凭据，STA 反复连断抢占射频把 SoftAP DHCP 挤死。
- 修复：新增 `s_wifi_autoconnect` 标志，仅在已配网/拿到 IP 后才自动（重）连。
- 验证：重刷后手机拿到 192.168.4.2，DNS catch-all 收到查询并应答，门户探测打到网关，连接稳定不再秒断。

### 根因3：HTTP socket 耗尽致"会话建立失败"
- 现象：手机已连上、门户在应答，App 点"连接设备"报"会话建立失败"；串口 `E httpd: httpd_accept_conn: error in accept (23)`（errno23 = 无空闲 socket）。
- 根因：荣耀 MagicOS 高频发探测（/generate204、/ 等），短连接大量进入 TIME_WAIT，占满 LWIP 仅有的 10 个 socket；App 的 /prov-session 进来时已无 socket 可 accept → 握手失败。
- 修复：`CONFIG_LWIP_MAX_SOCKETS` 10→16；captive httpd `max_open_sockets` 7→12；为 /generate204(无下划线) 与 / 注册直接 204 handler。
- 状态：socket 修复固件已刷入 COM4 并校验通过。

### 当前问题现状（2026-07-17 收尾，未解决）
手机与网关 SoftAP 的**关联本身不稳定、时好时坏**，这是当前主要阻塞：
- 好的时候：手机能关联（join, AID=1）、拿到 IP 192.168.4.2、DNS/门户探测正常应答（根因1/2 修复后确认过一次）。
- 坏的时候：手机反复 removing station after unsuccessful auth/assoc, AID = 0——连 802.11 关联都没建立，App 报连不上/会话建立失败。刷入 socket 修复固件后的这次复测即落在此情况，全程没到 join AID=1，因此 socket 修复是否解决会话建立失败尚未验证。
- 判断：根因1（崩溃）、根因2（DHCP 卡死）已确证修复；根因3（socket 耗尽）已改但待一次成功关联后才能验证。当前最上游不稳定点在 AP 关联层，疑似 ESP32-S3 上 BLE Mesh 常驻扫描/广播与 SoftAP 射频共存争用导致关联偶发失败（SW_COEXIST 已开）。
- 下一步方向（下轮再做）：(1) 配网阶段临时降低 BLE Mesh 扫描占空比或暂停 Provisioner 扫描，给 SoftAP 关联让出射频；(2) 固定 SoftAP 信道并观察关联成功率；(3) 必要时评估配网期先只跑 SoftAP、拿到凭据后再起 Mesh 的分时方案。

### 关联不稳定的修复（2026-07-17，已实现待刷写复测）
针对上面"AP 关联层不稳定"，按方向 (1)+(2) 已改固件（未编译未刷写）：
- **配网期让出射频**：新增 `farmely_mesh_yield_radio(bool)`（`main.c`，声明在 `gateway_bridge.h`）。`gateway_wifi_task` 在 `network_prov_mgr_start_provisioning()` 前调用 `yield(true)` → `esp_ble_mesh_provisioner_prov_disable(ADV|GATT)`；已核对 IDF v6.0.1 `bt_mesh_provisioner_disable`：两个 bearer 全关时会 `bt_mesh_scan_disable()`，即配网窗口内 BLE Mesh 扫描完全停止，2.4G 射频让给 SoftAP。`network_prov_mgr_wait()` 返回后 `yield(false)` 重新 enable 并重启 `mesh_recover` 任务恢复节点订阅/状态。代价：配网窗口内 mesh 控制不可用（此时无 Wi-Fi/MQTT，无实际影响）。
- **固定 SoftAP 信道**：`gateway_bridge.c` 在 `WIFI_EVENT_AP_START` 时把 AP 信道钉在 1（`FARMELY_PROV_AP_CHANNEL`，读回配置不同才改写，避免事件循环）。
- 复测步骤：重编重刷 COM4 → 观察手机连续 5 次关联成功率（应稳定 join AID=1 + 拿 IP）→ 再验证根因3 的 /prov-session 会话建立 → 走通 CRED_RECV→SUCCESS→END→STA GOT_IP→MQTT。

### 复测结果（2026-07-17 中午）
- 用户实测：**关联已稳定**（射频让出+固定信道生效），但 App 仍报"会话建立失败"。
- 失败时未拿到串口证据；已刷入诊断固件（COM4，干净启动确认）：`.sdkcfg-gws3` 开 `CONFIG_LOG_MAXIMUM_LEVEL_DEBUG`，配网期运行时开 protocomm/protocomm_httpd/security1/httpd_uri/httpd_sess/httpd_parse 的 DEBUG 日志；captive 探测应答全部加 `Connection: close`（防 Honor 探测长连接占满 httpd socket），catch-all 204 记录 URI。下次复测时抓串口即可定位 /prov-session 是否到达、卡在握手哪一步。

### 待复测（下轮，需先获得一次稳定关联）

### App 侧本轮进展
- 重新构建并 adb 安装 debug APK 到荣耀手机（MEP AN00）：SoftAP 页自动填 gateway1234、修复添加设备蓝底无字按钮。
- 登录为云端账号（cloud_service bearer token）：须先在有外网环境登录/注册（token 存本地），再连无网的 Gateway-D31548 配网；配网仅与 192.168.4.1 通信，不需外网。

1. App「热点兼容」gateway1234 下发 → NETWORK_PROV_WIFI_CRED_RECV/SUCCESS/END；
2. 网关 IP_EVENT_STA_GOT_IP + MQTT 连接；
3. MQTT → 网关 → BLE Mesh → 物理灯（COM8 D2/GPIO2、COM9 GPIO48 WS2812）；
4. 门铃物理按键 → ringing / 快照 / 视频。


### 会话建立失败根因确认与修复（2026-07-17 下午，已验证）
- 串口证据：手机点"连接设备"/"扫描"时网关只收到荣耀系统探测（/generate204 等），App 的 /prov-session、/prov-scan 完全没到设备。根因：SoftAP 无互联网，安卓把 App 的 HTTP 流量路由到移动数据，192.168.4.1 请求走错网络。
- 修复（App 侧）：新增 `app/lib/prov/wifi_bind.dart` + `MainActivity.kt` 平台通道 `farmely/wifi_bind`（`ConnectivityManager.requestNetwork(TRANSPORT_WIFI)` + `bindProcessToNetwork`），`provisioning_page.dart` 在 SoftAP 流程 establishSession 前 bind、finally unbind；Manifest 补 INTERNET/ACCESS_NETWORK_STATE/CHANGE_NETWORK_STATE。用户实测配网成功。

### 新增功能（2026-07-17，已刷写 COM4 / 已装 App）
- 长按重新配网：`gateway_bridge.c` 新增 prov_button_task，BOOT 键（GPIO0，低有效）长按 3 秒 → `esp_wifi_restore()` 清 WiFi 凭据（保留 mesh 节点数据）→ 重启进 SoftAP 配网。`main/CMakeLists.txt` REQUIRES 补 esp_driver_gpio。
- 控灯反馈延迟优化（App）：`cloud_devices_page.dart` 卡片状态改为三层：点击立即乐观翻转（_pendingOn）→ 网关 MQTT 节点状态到达即确认（订阅 mqtt.devices，_live 覆盖 shadow）→ 4 秒未确认回退云端 shadow。原实现是发命令后固定等 500ms 再拉云 shadow（云影子未及时更新导致卡片回跳/迟钝）。


### 设备列表清理与门铃可发现（2026-07-17 晚，已部署）
- 现象：设备列表出现 3 个"Mesh 网关"（2 在线 1 离线），门铃找不到。
- 根因：MQTT broker 上残留旧固件的 retained 消息（office/light/gateway/status root=node-0B55C0、office/light/node/{node-0B55C0,node-D31548,node-F53324}/status role=root），云端 mqtt_bridge 每次收到都注册成 type=gateway 的幽灵设备；App 本地自动发现只订阅 office/light/node/+/status，门铃的 doorbell/+/status 不在其中。
- 修复：① 已用 mosquitto_pub -r -n 清除上述 5 条 retained 幽灵主题（真实灯泡 node-0B55C2/node-CB59EA 保留）；② App MqttService 新增订阅 doorbell/+/status（AppConfig.doorbellStatusFilter），门铃(door-001)现在会出现在「自动发现（局域网）」里。真实网关 id 为 node-D3154A（farmely/gateway/node-D3154A/up/status）。云端 DB 里已被认领的幽灵网关条目需在 App 长按卡片"移除设备"。


### 配网即认领（强制转移归属，2026-07-17 晚，已部署）
- 云端：POST /api/v1/me/devices/{id}/claim 支持可选 body {"force": true}，强制把设备归属转移到当前账号（物理重新配网 = 占有证明）；storage.claim_device 加 force 参数；新增单测 test_force_claim_transfers_ownership，服务器全量 51 测试通过后已重建容器上线（本次部署同时把工作区较新的 OTA 代码同步到了服务器 /opt/mesh-cloud-service）。
- App：添加设备时若返回 409（已被其它账号认领），弹窗确认后用 force=true 重新认领；MqttService._handleGateway 现在把网关本身（root id，如 node-D3154A）也加入本地设备列表，「自动发现」能直接看到网关。
- 数据清理：door-001 已解除旧测试账号归属；云端 DB 中 3 条幽灵网关记录（node-0B55C0/node-D31548/node-F53324）已删除。


### 网关自动发现修复（2026-07-17 深夜，已部署）
- 现象：网关重新配网上线后「添加设备-自动发现」看不到网关；门铃可以。
- 根因：现网关固件只发 farmely/gateway/<id>/up/status（不再发旧 office/light/gateway/status 汇总），App 之前只订阅旧主题，收不到网关状态。
- 修复：AppConfig 新增 gatewayStatusFilter=farmely/gateway/+/up/status 与 gatewayIdFromStatusTopic()；MqttService 订阅并把网关（type=gateway, role=root）加入本地设备列表，自动发现即与门铃同一机制。已抓 MQTT 验证 node-D3154A 在线上报正常。


### OTA 端到端 + RGB 灯（2026-07-17）
- 新增 `rgb_light/` 固件工程（ESP32-S3 N16R8，板载 WS2812/GPIO48）：BLE 配网（前缀 Light-，PoP light1234）、BOOT 长按 3 秒重配、MQTT 上报 office/light/node/<id>/status（App 本地发现）与 farmely/light/<id>/up/status（云端，含 product_id=farmely-rgb-light / hw_version=s3-n16r8 / fw_version）、订阅 down/cmd 与 down/ota。
- OTA 客户端：收到 down/ota 里的 url/sha256/fw_version/msg_id 后 esp_https_ota 下载；下载流式计算 sha256，不匹配则 abort 并通过 `farmely/light/<id>/up/ota` 上报 failed；匹配后写入 OTA 分区并重启（分区表 ota_0/ota_1 各 6MB）。
- 回滚确认：已启用 bootloader app rollback；新固件启动后先连 WiFi/MQTT、发布状态与 OTA success，再调用 `esp_ota_mark_app_valid_cancel_rollback()`，避免 WiFi 刚连上就过早确认镜像。
- 云端闭环：MQTT bridge 识别 `up/ota` 的 downloading/success/failed 并写入 ota_updates；设备上报目标 fw_version 时也会把对应 pending 记录标记 success。新增单测覆盖 MQTT OTA 进度与版本上报成功确认。
- 版本：v1.0.0 仅开关（sdkcfg-nocolor 构建），v1.1.0 支持 RGB（cmd: "color:#RRGGBB" 或 JSON {"color":"#RRGGBB"}）。
- 固件托管：服务器 nginx `/www/wwwroot/114.55.208.72/firmware/`，HTTP 直链 http://114.55.208.72/firmware/rgb_light_v11.bin（站点 https 重定向对 /firmware/ 路径豁免）。
- 云端已注册 firmware fw-9103b97c…（1.1.0）+ rollout 100%：设备上报 1.0.0 后云自动下发 OTA。
- App：MeshDevice 新增 colorHex；灯详情页对上报 color 的设备显示九宫格色板，发布 color:#RRGGBB 到 office/light/node/<id>/cmd。
- 2026-07-18 实机验证：ESP-IDF v6.0.1 环境恢复后，`rgb-F53324`（ESP32-S3 N16R8 / COM14 / MAC e0:72:a1:f5:33:24）完成 BLE 配网、MQTT 上线和云端 OTA 闭环。v1.0.0 基线（无 color capability）收到 `down/ota` 后从 `http://114.55.208.72/firmware/rgb_light_v11.bin` 下载 v1.1.0，流式 sha256 校验通过，写入 `ota_1`，重启后串口确认 `App version: 1.1.0`，MQTT 上报 `status=success`，云端 `ota_updates` 记录落为 success。
- 2026-07-18 修复项：App 侧新增 `Light-` 前缀识别，选中 RGB 灯时自动使用 PoP `light1234`，不影响原有 `Gateway-`/Doorbell 配网口令；OTA 客户端修正 `esp_https_ota_perform()` 循环，`ESP_ERR_HTTPS_OTA_IN_PROGRESS` 作为继续下载状态而非失败；IDF 6 下 sha256 校验改用 PSA Crypto API；ESP32-S3 日志保留 UART0 主输出并开启 USB Serial/JTAG 副输出。
- 当前线上固件：`rgb_light_v11.bin` sha256 = `861487fb6d7822d1ba5fd90edd63d83025a4df9520bf6e20c959894d154f31c8`；云端服务 `/health` 正常且 MQTT connected。
