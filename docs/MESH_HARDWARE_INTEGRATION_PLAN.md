# BLE Mesh + Wi-Fi 硬件联调计划与验收标准

> 最近更新：2026-07-13  
> 目的：在刷写设备前固定硬件映射、目标架构、执行顺序、结果判定和记录格式。  
> 本文中的“通过”必须有构建输出、串口日志和可观察结果支撑；仅编译成功不等于无线 Mesh 联调成功。

## 1. 当前硬件矩阵

| 角色 | 硬件 | 串口 | 灯/外设 | 本轮约束 |
|---|---|---|---|---|
| 门铃 | ESP32-S3 N8R8 + 摄像头 | COM7 | 摄像头、门铃按键 | 实测 Flash 8 MB、PSRAM 8 MB；保持 Wi-Fi/MQTT/MJPEG，不加入 BLE Mesh |
| 灯控节点 A | ESP32-WROOM | COM15 | D2 板载 LED，GPIO2 | 关闭 brownout detector 仅用于当前台架联调；作为 BLE Mesh Generic OnOff Server |
| BLE Mesh 网关 | ESP32-S3 N16R8 | 待串口枚举确认 | GPIO48 板载 WS2812 RGB | 同时运行 BLE Mesh Provisioner/Client 与 Wi-Fi/MQTT |
| 其他灯控节点 | ESP32-S3 N16R8 | 待串口枚举确认 | GPIO48 板载 WS2812 RGB | BLE Mesh Generic OnOff Server；按需开启 Relay |

旧文档中的 COM8、COM4、COM6、COM14 是历史连接记录，不再作为本轮串口映射依据。刷写前必须读取芯片信息再次确认目标，禁止仅凭旧端口号烧录。

### 1.1 COM15 供电告警处理边界

- 本轮按要求在 ESP32-WROOM 专用构建中设置 `CONFIG_ESP_BROWNOUT_DET=n`。
- 关闭 brownout 只是不再触发欠压复位，不会改善供电质量。
- 该配置仅允许用于台架联调，不能作为量产默认值；欠压时仍可能发生随机重启、异常执行或 Flash 写入损坏。
- 验收记录必须保留“brownout 已关闭”的风险说明；量产前应更换稳定电源、USB 线或供电设计并恢复 brownout 保护。

## 2. Mesh 方案结论

### 2.1 目标架构

```text
                        HTTPS / MQTT
App / cloud_service  <---------------->  MQTT Broker
                                                |
                                                | Wi-Fi STA
                                                v
                                   ESP32-S3 N16R8 网关
                                   - BLE Mesh Provisioner
                                   - Config Client
                                   - Generic OnOff Client
                                   - Wi-Fi + MQTT bridge
                                                |
                                          BLE Mesh
                           +--------------------+--------------------+
                           |                                         |
                    ESP32-WROOM 灯节点                       ESP32-S3 N16R8 灯节点
                    GPIO2 D2 LED                             GPIO48 WS2812 RGB
                    Generic OnOff Server                     Generic OnOff Server

门铃 ESP32-S3 N8R8：独立 Wi-Fi/MQTT/视频链路，不加入 BLE Mesh。
```

普通灯节点只运行 BLE Mesh，不连接家庭 Wi-Fi，也不运行 MQTT。网关负责：

1. 发现、配网并配置 BLE Mesh 节点；
2. 将 MQTT 单设备/分组命令映射到 BLE Mesh Generic OnOff；
3. 将节点 OnOff Status、在线状态和标识映射回统一 MQTT 协议；
4. 在 Wi-Fi 短时中断时保留本地 BLE Mesh 控制能力，恢复后补发最新状态。

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
2. 枚举串口，读取 COM7、COM15 及其他串口的芯片型号、Flash、PSRAM，不写 Flash。
3. 保存现有固件启动日志，确认当前故障与功能基线。

### 阶段 1：COM7 门铃基线

1. 按 ESP32-S3 N8R8 配置构建门铃固件；`camera_stream/sdkconfig.defaults` 保持 8 MB Flash 配置。
2. 刷写 COM7 并监控启动。
3. 验证摄像头初始化、Wi-Fi/MQTT、heartbeat/LWT、门铃事件、命令 ACK、本地和中继视频。
4. 门铃验证通过后不得为 BLE Mesh 改动其无线架构。

### 阶段 2：COM15 BLE Mesh 灯节点

1. 从乐鑫官方 Generic OnOff Server 复用模型、配网和持久化流程。
2. ESP32-WROOM 专用配置：
   - `CONFIG_ESP_BROWNOUT_DET=n`；
   - 单灯输出 GPIO2；
   - 不初始化不存在的三路 RGB GPIO；
   - 开启 BLE Mesh settings，重启后保留网络密钥和配置。
3. 构建、刷写 COM15，确认进入未配网广播状态。

### 阶段 3：N16R8 BLE Mesh + Wi-Fi 网关

1. 从官方 Provisioner 和 Wi-Fi coexist 示例复用初始化与回调。
2. 网关提供 Provisioner、Config Client、Generic OnOff Client。
3. 接入现有 MQTT 新旧 topic 映射，避免改变云端协议。
4. 配置 GPIO48 WS2812 作为网关状态指示。
5. 自动发现并配网 COM15，完成 NetKey/AppKey、Model App Bind 和订阅配置。

### 阶段 4：多节点与共存压力

1. 至少再接入一块 N16R8 Generic OnOff Server。
2. 验证单节点寻址、分组控制、状态上报、重启恢复。
3. 同时运行 Wi-Fi/MQTT 与 BLE Mesh 控制压力测试。
4. 记录失败项、复现步骤、日志时间点和是否阻断迁移。

## 4. 结果判定标准

### 4.1 COM7 门铃

| 验收项 | 通过标准 |
|---|---|
| 硬件识别 | 读取结果为 ESP32-S3，Flash 8 MB、PSRAM 8 MB；与 N8R8 相符 |
| 稳定启动 | 连续运行 10 分钟无 panic、watchdog、循环重启 |
| 摄像头 | OV3660 初始化成功；连续观看本地 MJPEG 60 秒无崩溃 |
| Wi-Fi/MQTT | 获取 IP，MQTT connected；heartbeat/status 可在 broker/cloud 观察 |
| 门铃事件 | 连续按键 3 次，3 次事件均到达云端且不重复 |
| 云端命令 | 至少执行 1 次支持的命令并收到对应 ACK |
| 公网链路 | 中继健康时可观看 60 秒；若外部服务不可用，明确记为环境阻塞，不算固件通过 |

### 4.2 COM15 WROOM 灯节点

| 验收项 | 通过标准 |
|---|---|
| 硬件识别 | 读取结果为 ESP32；刷写目标和分区与 WROOM 配置一致 |
| Brownout 配置 | 构建配置确认 `CONFIG_ESP_BROWNOUT_DET=n`，启动日志不再出现 brownout reset |
| 基础稳定性 | 连续运行 10 分钟无 panic、watchdog、循环重启；欠压风险仍单独记录 |
| BLE Mesh 配网 | 网关 60 秒内发现，120 秒内完成 provisioning、AppKey 添加和 Model Bind |
| 灯控 | 20 次交替 On/Off 全部正确驱动 GPIO2 D2 LED，状态回复与实灯一致 |
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
| 2026-07-13 | 待提交 | COM15 WROOM | BLE Mesh Generic OnOff Server 离线构建 | 部分通过/刷写阻塞 | GPIO2 单灯、单元素、settings、Generic Server、`CONFIG_ESP_BROWNOUT_DET=n` 构建通过；COM15 自动/软件复位均无法进入下载模式，esptool 报 `No serial data received`，需人工按住 BOOT 后重试 |
| 2026-07-13 | 待填写 | N16R8 网关/节点 | 待枚举 | 待执行 | 需要至少一块网关；分组验收需要至少两个灯节点 |

每轮联调结束后更新本表，并在 `docs/DEVELOPMENT_PROGRESS.md` 中只写已经有证据的结论。
