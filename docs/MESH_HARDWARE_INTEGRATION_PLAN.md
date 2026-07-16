# BLE Mesh + Wi-Fi 硬件联调计划与验收标准

> 最近更新：2026-07-14
> 目的：在刷写设备前固定硬件映射、目标架构、执行顺序、结果判定和记录格式。  
> 本文中的“通过”必须有构建输出、串口日志和可观察结果支撑；仅编译成功不等于无线 Mesh 联调成功。

## 1. 当前硬件矩阵

| 角色 | 硬件 | 串口 | 灯/外设 | 本轮约束 |
|---|---|---|---|---|
| 门铃 | ESP32-S3 N8R8 + 摄像头 | 当前拔下（历史 COM7） | 摄像头、门铃按键 | 实测 Flash 8 MB、PSRAM 8 MB；保持 Wi-Fi/MQTT/MJPEG，不加入 BLE Mesh |
| BLE Mesh 网关 | ESP32-S3 N16R8 | COM4 | GPIO48 板载 WS2812 RGB | 已实测 16 MB Flash、8 MB PSRAM；Provisioner/Config Client/Generic OnOff Client；Wi-Fi/MQTT bridge 已实现但尚未联网验收 |
| BLE Mesh 灯节点 A | ESP32-S3 N16R8 | COM8 | GPIO48 板载 WS2812 RGB | 已完成 Provisioning、Config 和 Generic OnOff 实物链路 |
| BLE Mesh 灯节点 B | ESP32-WROOM | COM7（历史 COM6） | D2/GPIO2 | 2026-07-14 以芯片、MAC、项目名和 Mesh 地址确认；地址 `0x0006` |
| 非活动端口 | 历史板卡 | COM14、COM15 | 不纳入当前映射 | 两个串口/板卡存在启动或下载异常，不再阻塞主线 |

本轮固定映射为 COM4 网关、COM8 S3 节点 `0x0005`、COM7 WROOM
节点 `0x0006`；摄像头门铃已拔下。串口号不等于永久产品角色，每次
刷写前必须重新读取芯片型号和 MAC，并结合固件项目名、Mesh 地址确认。

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

每轮联调结束后更新本表，并在 `docs/DEVELOPMENT_PROGRESS.md` 中只写已经有证据的结论。
