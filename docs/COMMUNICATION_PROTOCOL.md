# Farmely 设备通信协议 v1

本文档是本仓库后续新增产品、升级旧固件和接入云端/App 的固定协议入口。原则是：新产品只扩展 `class`、`type`、`capabilities` 和 `data` 字段，不重新发明 topic、信封、OTA 流程。

## 版本与兼容策略

- 当前协议名：`farmely.v1`
- MQTT 根 topic：`farmely`
- 统一信封版本：`v: 1`
- 兼容期内云端仍接收旧 topic，例如 `office/light/node/+/status`、`office/light/gateway/status`、`doorbell/+/status`、`doorbell/+/event`。
- 新固件必须优先发布 `farmely/...` 规范 topic；旧 topic 只作为兼容兜底，不作为新产品设计依据。

## Topic 规范

统一 topic 格式：

```text
farmely/{class}/{device_id}/{direction}/{channel}
```

字段说明：

| 字段 | 必填 | 允许值/格式 | 说明 |
| --- | --- | --- | --- |
| `class` | 是 | `light`、`doorbell`、`gateway`、`camera`、`sensor`、`switch`、`plug`、`lock`、`curtain` 等 | 大类用于云端路由和 App 默认展示。新增大类前先确认 App 是否已有图标/控件。 |
| `device_id` | 是 | 稳定字符串，建议 `<短类型>-<MAC后6位>`，如 `rgb-F53324` | 设备生命周期内不变；用户认领、影子、OTA 记录都依赖它。 |
| `direction` | 是 | `up`、`down` | `up` 是设备到云端，`down` 是云端/App 到设备。 |
| `channel` | 是 | `status`、`event`、`cmd`、`ota`、`shadow` | 不同业务通道，payload 仍使用统一信封。 |

常用 topic：

```text
farmely/{class}/{device_id}/up/status
farmely/{class}/{device_id}/up/event
farmely/{class}/{device_id}/up/ota
farmely/{class}/{device_id}/down/cmd
farmely/{class}/{device_id}/down/ota
```

## Payload 统一信封

所有新 topic 的 payload 使用 JSON 信封：

```json
{
  "v": 1,
  "msg_id": "rgb-F53324-a1b2c3d4-1",
  "ts": 1784342400,
  "type": "status",
  "data": {
    "online": true
  }
}
```

字段说明：

| 字段 | 必填 | 说明 |
| --- | --- | --- |
| `v` | 是 | 协议信封版本，当前固定为 `1`。 |
| `msg_id` | 是 | 消息幂等 ID。建议包含 `device_id + boot_nonce + sequence`，同一条消息重发必须保持一致。 |
| `ts` | 建议 | Unix 秒。设备未校时时可填 `0`，云端不依赖它做安全判定。 |
| `type` | 是 | 推荐与 channel 一致：`status`、`event`、`cmd`、`ota`、`ack`。 |
| `data` | 是 | 业务载荷。只在这里新增产品字段。 |

幂等规则：

- 设备上报必须带 `msg_id`；云端用它避免重复推进 shadow 或重复记录事件。
- 云端下发命令也带 `msg_id`；设备执行后可上报 `ack`，`data.ack_msg_id` 指向原始命令。
- 兼容旧字段时，云端同时识别 `message_id`，但新固件只写 `msg_id`。

## 设备身份和产品三元组

所有可升级产品必须在 `up/status` 的 `data` 中上报：

```json
{
  "product_id": "rgb_light",
  "hw_version": "esp32s3-n16r8-rev-a",
  "fw_version": "1.1.0"
}
```

字段规则：

| 字段 | 规则 |
| --- | --- |
| `product_id` | 产品型号，不随固件版本变化，例如 `rgb_light`、`doorbell_s3`、`mesh_gateway_s3`。 |
| `hw_version` | 硬件/分区/外设兼容边界，例如 `esp32s3-n16r8-rev-a`。不同 Flash/PSRAM/摄像头/引脚方案应拆版本。 |
| `fw_version` | 固件版本，建议 SemVer：`1.0.0`、`1.1.0`。云端 OTA 用它判断是否需要升级。 |

没有这三个字段，App 可以显示设备状态，但不能稳定触发 OTA。

## Capabilities 能力声明

设备在 `up/status.data.capabilities` 中声明能力，App 和云端按能力渲染/下发，不应只靠设备类型猜功能。

推荐命名：

| 能力 | 含义 |
| --- | --- |
| `system.online` | 设备会上报在线/离线状态。 |
| `system.fw_version` | 设备会上报 `fw_version`。 |
| `system.ota` | 设备支持 `down/ota` 升级。 |
| `on_off` | 开关控制。 |
| `color` | RGB/颜色控制，颜色值统一用 `#RRGGBB`。 |
| `doorbell.button` | 门铃按键事件。 |
| `camera.stream` | 视频流/预览能力。 |
| `sensor.temperature` | 温度传感。 |
| `sensor.humidity` | 湿度传感。 |

能力只增不删；如果产品升级新增能力，例如 RGB 灯从无色到彩色，发布新固件后在 status 中新增 `color`。

## Status 上报

在线状态示例：

```json
{
  "v": 1,
  "msg_id": "rgb-F53324-00000001-1",
  "ts": 1784342400,
  "type": "status",
  "data": {
    "online": true,
    "type": "light_bulb",
    "product_id": "rgb_light",
    "hw_version": "esp32s3-n16r8-rev-a",
    "fw_version": "1.1.0",
    "capabilities": ["on_off", "color", "system.online", "system.fw_version", "system.ota"],
    "on": true,
    "state": "on",
    "color": "#FFAA00"
  }
}
```

离线/LWT 建议：

- MQTT LWT topic 使用旧 topic 或规范 `up/status` 均可；新产品优先规范 topic。
- payload 中设置 `online: false` 和 `offline_reason`，例如 `mqtt_lwt`、`gateway_reported_offline`。
- retained：在线状态/LWT 建议 retained；事件和 OTA 进度不 retained。

## Command 下发

云端/App 通过：

```text
farmely/{class}/{device_id}/down/cmd
```

示例：

```json
{
  "v": 1,
  "msg_id": "cloud-4b63f0d5",
  "ts": 1784342401,
  "type": "cmd",
  "data": {
    "on": true,
    "color": "#3366FF"
  }
}
```

设备执行后建议上报：

```json
{
  "v": 1,
  "msg_id": "rgb-F53324-00000001-2",
  "ts": 1784342402,
  "type": "ack",
  "data": {
    "ack_msg_id": "cloud-4b63f0d5",
    "ok": true
  }
}
```

命令约定：

- `on: true/false`：开关。
- `color: "#RRGGBB"`：颜色。
- `command: "stream" | "hangup" | "snapshot" | "reboot" | ...`：门铃/摄像头/系统类命令。
- 设备不认识的字段必须忽略，不应崩溃或重启。

## Event 上报

事件 topic：

```text
farmely/{class}/{device_id}/up/event
```

门铃示例：

```json
{
  "v": 1,
  "msg_id": "doorbell-A1B2C3-00000001-1",
  "ts": 1784342403,
  "type": "event",
  "data": {
    "event": "ringing"
  }
}
```

事件名建议稳定小写：

- 门铃：`ringing`、`stream_start`、`stream_stop`
- 系统：`boot`、`reboot`、`factory_reset`
- OTA：不要走 `event`，使用 `up/ota`

## OTA 协议

云端下发：

```text
farmely/{class}/{device_id}/down/ota
```

payload：

```json
{
  "v": 1,
  "msg_id": "ota-123",
  "ts": 1784342500,
  "type": "ota",
  "data": {
    "command": "update",
    "product_id": "rgb_light",
    "hw_version": "esp32s3-n16r8-rev-a",
    "fw_version": "1.2.0",
    "url": "https://example.com/firmware/rgb_light_v120.bin",
    "sha256": "<64位十六进制>",
    "sign": "<可选签名>"
  }
}
```

设备回报：

```text
farmely/{class}/{device_id}/up/ota
```

payload：

```json
{
  "v": 1,
  "msg_id": "rgb-F53324-00000002-1",
  "ts": 1784342510,
  "type": "ota",
  "data": {
    "status": "downloading",
    "fw_version": "1.2.0",
    "ota_msg_id": "ota-123",
    "detail": "start"
  }
}
```

状态机：

| 状态 | 说明 |
| --- | --- |
| `downloading` | 已收到 OTA 命令并开始下载/写入。 |
| `success` | 新固件启动并确认运行。 |
| `failed` | 下载、校验、写入或启动失败；`detail` 写原因。 |

固件侧要求：

- 必须校验 `sha256`。
- 支持 OTA 分区与回滚确认。
- 新固件首次启动后尽快上报 status，包含新的 `fw_version`。
- 成功确认后再上报 `up/ota status=success`。

云端匹配规则：

- 固件仓库键：`product_id + hw_version + fw_version`
- rollout 键：`product_id + hw_version`
- 可选 `from_fw_version` 限定当前版本。
- `percent` 灰度使用稳定哈希，同一设备命中结果稳定。

## Shadow 映射

云端把 `status/event/ota` 中的 `data` 合并进 reported shadow；App 对用户操作写 desired shadow，再由云端下发 `down/cmd`。

约定：

- reported：设备实际上报状态。
- desired：用户/自动化希望设备达到的状态。
- 设备收到命令并执行后，应通过 `up/status` 重新上报真实状态。
- 设备不要直接相信 desired 已生效，必须以上报闭环为准。

## 新产品接入最低要求

新增产品固件至少做到：

1. 生成稳定 `device_id`。
2. 发布 `farmely/{class}/{device_id}/up/status`。
3. status 带 `product_id`、`hw_version`、`fw_version`、`capabilities`。
4. 使用统一信封，带 `v=1`、`msg_id`、`type`、`data`。
5. 若可控制，订阅 `farmely/{class}/{device_id}/down/cmd`。
6. 若可升级，订阅 `farmely/{class}/{device_id}/down/ota` 并上报 `up/ota`。
7. 保留未知字段，忽略未知命令，不破坏已有能力。

## 当前实现状态

| 子项目 | 协议状态 |
| --- | --- |
| `rgb_light/` | 已按 `farmely.v1` 上报 status、cmd、OTA；已实机 OTA 成功。 |
| `camera_stream/` | 已按 `farmely.v1` 上报 status/event/cmd；尚需补齐 OTA 三元组和 `system.ota` 后才能接入 OTA。 |
| `internal_communication/` | 网关/灯节点已发布规范 status/cmd；后续应补齐网关和节点的产品三元组。 |
| `cloud_service/` | 已订阅 `farmely/+/+/up/+`，维护 shadow、事件和 OTA 记录；兼容旧 topic。 |
| `app/` | 设备详情按 shadow 和 capabilities 展示；OTA 卡片依赖三元组和 `system.ota`。 |
