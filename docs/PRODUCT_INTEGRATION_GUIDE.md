# 新产品/旧产品升级接入指南

本文档是实际开发清单；协议细节以 [`COMMUNICATION_PROTOCOL.md`](COMMUNICATION_PROTOCOL.md) 为准。

## 先定产品身份

每个产品先固定三件事：

| 项 | 示例 | 说明 |
| --- | --- | --- |
| `class` | `light` | MQTT topic 大类。 |
| `device_type` | `light_bulb` | App 展示和默认控件类型。 |
| `product_id` | `rgb_light` | OTA 产品型号，不随固件版本变化。 |
| `hw_version` | `esp32s3-n16r8-rev-a` | 硬件兼容边界，决定固件能否通刷。 |
| `fw_version` | `1.0.0` | 固件版本，建议 SemVer。 |

不要把 `product_id` 写成某一台设备 ID；不要把不同 Flash/PSRAM/摄像头/引脚方案混成同一个 `hw_version`。

## 固件接入清单

- [ ] 设备 ID 稳定：建议 `<短类型>-<MAC后6位>`。
- [ ] BLE/SoftAP 配网广播名前缀稳定，例如 `Doorbell-`、`Gateway-`、`Light-`。
- [ ] `up/status` 使用 `farmely.v1` 信封。
- [ ] `up/status.data` 上报：
  - [ ] `online`
  - [ ] `type`
  - [ ] `product_id`
  - [ ] `hw_version`
  - [ ] `fw_version`
  - [ ] `capabilities`
- [ ] 每条上报都有稳定递增的 `msg_id`。
- [ ] 心跳周期建议 60 秒。
- [ ] MQTT LWT 上报 `online=false` 和 `offline_reason=mqtt_lwt`。
- [ ] 可控设备订阅 `down/cmd`。
- [ ] 可升级设备订阅 `down/ota`，实现 sha256 校验和回滚确认。
- [ ] 执行命令后重新上报 `up/status`，形成闭环。

## App 接入清单

- [ ] 如果是新配网前缀，在 `app/lib/config.dart` 增加 PoP 映射。
- [ ] 如果是新 `device_type`，在 App 设备图标/卡片映射中加图标。
- [ ] 根据 `capabilities` 渲染控件，不只靠 `device_type` 猜能力。
- [ ] OTA 卡片依赖 shadow 中的 `product_id/hw_version/fw_version` 和 `system.ota`。
- [ ] 控制命令优先写云端 desired shadow；局域网 MQTT 只做加速/兜底，不做唯一状态源。

## 云端接入清单

- [ ] 确认 `class` 已能被 `cloud_service` 自动注册为合适 `device_type`。
- [ ] 如需新事件类型，确认事件名稳定并能落库/展示。
- [ ] 如需新控制字段，扩展 desired → MQTT `down/cmd` 的映射。
- [ ] 为新字段补测试，至少覆盖：
  - [ ] 设备上报进入 shadow。
  - [ ] 用户只能访问自己认领的设备。
  - [ ] 命令下发 topic/payload 正确。
  - [ ] OTA check/updates 权限正确。

## OTA 升级发布清单

1. 固件构建时写入正确 `PROJECT_VER`、`product_id`、`hw_version`。
2. 生成固件包并计算 SHA256。
3. 上传固件到可访问 URL。
4. 调云端注册固件：

```bash
POST /api/v1/firmware
{
  "product_id": "rgb_light",
  "hw_version": "esp32s3-n16r8-rev-a",
  "fw_version": "1.2.0",
  "url": "https://example.com/firmware/rgb_light_v120.bin",
  "sha256": "<64位十六进制>",
  "sign": "<可选签名>"
}
```

5. 创建 rollout：

```bash
POST /api/v1/rollouts
{
  "product_id": "rgb_light",
  "hw_version": "esp32s3-n16r8-rev-a",
  "target_fw_version": "1.2.0",
  "from_fw_version": "1.1.0",
  "percent": 10
}
```

6. 在 App 设备详情页点“检查更新”，或等设备下一次 status 上报自动触发。
7. 验证：
   - [ ] 云端 `ota_updates` 生成任务。
   - [ ] 设备串口显示下载/校验/写入/重启。
   - [ ] 新固件上报新 `fw_version`。
   - [ ] `up/ota status=success` 落库。
   - [ ] App 显示最新任务状态。

## 旧产品升级策略

旧产品不要求一次性推倒重来，按下面顺序升级最稳：

1. 保留旧 topic，新增 `farmely.v1` 规范 topic 双发。
2. status 先补 `capabilities`。
3. 再补 `product_id/hw_version/fw_version`。
4. 可控设备再订阅 `down/cmd`。
5. 最后接 `down/ota` 和 `up/ota`。
6. 云端和 App 均确认无回归后，再考虑停止旧 topic。

## 当前产品建议

| 产品 | 下一步 |
| --- | --- |
| RGB 灯 | 已完成 OTA 主链路；后续可加签名校验和 HTTPS 固件下载。 |
| 门铃 | 补 `product_id/hw_version/fw_version`，再接 `system.ota` 和门铃固件 OTA。 |
| Mesh 网关 | 补网关自身三元组；明确网关固件与子节点固件是否分开 OTA。 |
| Mesh 灯节点 | 补节点产品三元组；如通过网关升级，需新增网关到节点的 OTA 转发协议。 |
