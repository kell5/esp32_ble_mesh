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

- **T-APP-UX（`app/`）** — WIP，未完成，本次先提交进度供接手。
  - **已发现并修复的回归（关键）**：账号优先重构第一版在 `_MainShell` 里多套了一层 `Navigator`（双层嵌套导航器），导致顶层「我的设备」列表按系统返回键**直接回桌面**，而不是弹「退出应用」确认框（真机 MEP-AN00 activity dump 实测：首次返回后 launcher 变 topResumedActivity）。详情页返回列表正常，仅顶层退出确认被破坏。这违反 851e288 的返回键契约。
    - **根因**：嵌套 `Navigator` 会自行吞掉系统返回键——栈里有页可弹时弹（所以详情返回列表正常），栈空时直接结束 Activity 回桌面（所以顶层返回被破坏），根级 `PopScope(canPop:false)` 根本没机会触发。这也是账号优先第一版把内容包在 `_MainShell`/嵌套导航器里才出的问题。
    - **修复**：`root_page.dart` **彻底去掉内容层的嵌套导航器**。`AppShell` 直接作为 `CupertinoApp` 根导航器的 home 路由；设备详情页、来电覆盖页都 `push` 到**同一个根导航器**（`Navigator.of(context, rootNavigator: true)`）。这样系统返回键会自己弹掉这些被 push 的页；只有回到 home 路由时，挂在 home 上的 `PopScope(canPop:false)` 才触发 `_handleBack()`→`_confirmExit()`。删除了 `CupertinoTabController _tab`、`_tabNavKeys`、`_navKey`、`_MainShell`。保留 851e288「首次返回不退出」的契约。
    - **已真机验证通过（MEP-AN00 / adb keyevent 4）**：①顶层「我的设备」返回→弹「退出应用」确认框（取消→停留列表，Activity 仍为 mesh_app）；②点开设备详情→返回→回到列表；三次 dumpsys 均确认未回桌面。截图见会话。
  - **新增功能（本次实现，`cloud_devices_page.dart`）**：
    1. **添加设备自动发现 + 保留手动输入**：新增 `_AddDevicePage`，「自动发现（局域网）」列出 `MqttService.devicesSnapshot` 中未认领的 mesh 节点，一键 `claimDevice`；「手动添加」保留设备 ID 输入框；「全新设备」提供**一个**「蓝牙配网」按钮进入既有 `ProvisioningPage`。复用现有接口，无新增依赖。
    2. 空态引导文案同步更新为「自动发现 / 手动输入 / 蓝牙配网」。
  - **手动删除设备（用户要求）——已端到端实现代码（规则放开后允许改 `cloud_service`）**：
    - 后端新增 `DELETE /api/v1/me/devices/{device_id}`（`main.py`），账号鉴权后调用 `store.unclaim_device(device_id, user_id)`（`storage.py`：校验 owner 一致，否则 `DeviceNotOwnedError`→403；不存在→404；成功把 `owner_id` 置 NULL，设备仍注册在云端，可再次认领）。新增异常类 `DeviceNotOwnedError`。
    - App 侧：`cloud_client.dart` 新增 `unclaimDevice(id)`（HTTP DELETE）；`smart_cards.dart` 的 `SmartCard` 新增 `onLongPress`；`cloud_devices_page.dart` 长按设备卡 → 操作表「移除设备（红色）/ 取消」→ 调 `unclaimDevice` 后刷新列表。
    - 新增后端测试 2 条（`tests/test_auth.py`）：`test_unclaim_removes_device_and_allows_reclaim`、`test_unclaim_foreign_device_is_forbidden`。
    - **⚠ 待部署**：线上 `lk-mcu.online/cloud` 尚未重新部署（与后端新协议一起），当前线上没有此端点，App 长按「移除」会收到 404/错误横幅，直到后端重新部署。用户日后「要回 door-001」也依赖此端点上线。
  - 自检：`flutter analyze` = 0 问题；`flutter build apk --debug` PASS；返回键已真机验证通过；`cloud_service` `ruff check` 通过、`unittest` 34/34 通过（含新增 2 条 unclaim 测试）。

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
