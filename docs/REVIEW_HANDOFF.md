# 终检记录与交接（多 AI 协作）

本文件记录总指挥（Devin）对各 AI 提交的终检结果与**待修/待办交接**，供其他 AI 接手。分支：`devin/1783656952-cloud-device-shadow`。

## 已合并
- **T-CLOUD-1 + T-CLOUD-2（后端 capability + MQTT 规范化）** — commit `eeb179d`。
  独立复跑：`ruff` 0 报错、`unittest` 32/32 通过（含 capability 读写、旧设备兼容、新旧 topic 入影子、门铃事件去重）。旧库经 `ALTER TABLE ... DEFAULT '[]'` 自动迁移。

## 已审通过、待提交（改动在工作树，未 commit）
- **T-FW-PROVISION-BIND + 门铃 T-FW-CONTRACT（`camera_stream/`）** — 审阅通过，可提交。
  - ff54 custom-data 端点返回 `{device_id, claim_code}`；统一信封 status/event、capabilities、60s 心跳、LWT、命令 ack、新旧双 topic、订阅新旧两条 cmd。构建 PASS（EXIT 0）。
  - 非阻塞小项（可后续再改）：
    1. `claim_code` 用 `esp_random()` 每次新生成、云端暂不校验（demo 可接受）。
    2. `device_id` 实际取 `CONFIG_EXAMPLE_DOORBELL_ID`（Kconfig），量产需每板唯一；`app_wifi.h` 注释写“MAC 派生”与实现不符，需统一。
    3. ack 发到 `farmely/doorbell/<id>/up/event`（channel=event）；云端 `_ingest_farmely` 对 event 通道优先按事件处理，会把 ack 记进事件表。建议 ack 改走 `up/status` 或新增 `up/ack` 通道，或云端在按 channel 之前先看 `type`。

- **T-APP-UX（`app/`）** — `flutter analyze` 0 问题。**尚未完成人工审阅**：需确认新 `home_shell.dart`、删除 `home_page.dart`、`root_page.dart` 改动**没有破坏已修好的根级 PopScope 返回键逻辑**（commit `851e288` 引入，禁止回退）。确认无回归后再提交。

## 待修（阻塞，必须修复后重新构建再提交）
**任务号：T-FW-CONTRACT-FIX（`internal_communication/main/mesh_main.c`，网关/根节点）**

AI-C 已实现 capability/统一信封/新 topic/ack，但 `mqtt_dispatch()` 有两个功能性 bug，会导致新格式命令行为错误（当前被“云端同时下发旧 topic”掩盖，一旦网关刷新固件即暴露）：

1. **单设备新格式命令被误当广播**：
   ```c
   bool on_new_topic = (strstr(topic, "farmely/") && strstr(topic, "/down/cmd"));
   ...
   if (is_all || is_demo || on_new_topic) { mqtt_handle_broadcast(on); return; }
   ```
   `farmely/light/<id>/down/cmd`（单节点命令）也满足 `on_new_topic`，会被广播到所有节点。
   **修复要点**：`on_new_topic` 只应匹配“广播/网关级”topic（如 `farmely/gateway/+/down/cmd` 或约定的 all 主题），**不得**匹配 `farmely/light/<id>/down/cmd`；单节点命令必须落到下面的按 id 路由分支。

2. **提取节点 id 偏移量 off-by-one**：
   ```c
   const char *p = strstr(topic, "farmely/light/");
   if (p) { p += 13; ... }   // "farmely/light/" 长度是 14，应为 p += 14
   ```
   现在 `p` 指向结尾的 `/`，取出的 `target_id` 带前导 `/`，节点查找失配、命令丢失。
   **修复要点**：改为 `p += strlen("farmely/light/")`（=14），确保 `target_id` 不含前导 `/`。

**验收自检**：`idf.py build`（网关 + 节点两套）EXIT 0；`farmely/light/<id>/down/cmd` 只控制目标节点、不广播；`target_id` 无前导 `/`；旧 `office/light/node/<id>/cmd` 链路不回归。

## 其它
- 根目录出现的 `reasonix.toml` 为 AI 工具生成物，**不要提交进仓库**（保持工作区干净）。
- 后端已合并的 capability/规范化 topic **尚未重新部署**到 `114.55.208.72`；如需 App 实测新协议，重建容器即可（DB 自动迁移，`https://lk-mcu.online/cloud` 地址不变）。
