# Phase B 云端设备模型（首个纵向切片）

该服务把门铃与 ESP-WIFI-MESH MQTT 消息收敛为统一设备注册表和设备影子。首版采用 FastAPI + SQLite，本地即可运行；新增状态字段和门铃状态主题均向后兼容现有 App。

## 已实现

- 设备注册、认领和按用户查询。
- `desired/reported` 双向设备影子及独立版本号。
- `message_id` 幂等处理。
- MQTT 状态接入、既有单节点控制主题下发。
- 设备超时离线和 `offline_reason=cloud_timeout`。
- Mesh 网关和门铃 LWT 离线状态，固件状态/事件原生携带 `version` 与 `message_id`。
- 可选 `X-Cloud-Token` API 保护；默认只监听 `127.0.0.1`。

## MQTT 兼容映射

| 现有主题 | 云端行为 |
|---|---|
| `office/light/node/+/status` | 自动注册节点并更新 `reported` |
| `office/light/gateway/status` | 自动注册网关并更新 `reported` |
| `doorbell/+/status` | 自动注册门铃并接收在线状态或 `mqtt_lwt` 离线原因 |
| `doorbell/+/event` | 自动注册门铃并记录 `last_event` |
| `office/light/node/<id>/cmd` | `desired.on` 转换为原有 `on/off` 指令 |
| `doorbell/<id>/cmd` | `desired.command` 透传原有门铃命令 |

## 本地运行

要求 Python 3.12。

```powershell
cd cloud_service
python -m venv .venv
.\.venv\Scripts\python -m pip install -e ".[test]"
$env:CLOUD_API_TOKEN = "replace-with-a-random-token"
.\.venv\Scripts\python -m cloud_service
```

Linux/macOS 将解释器路径换成 `.venv/bin/python`。Swagger UI：`http://127.0.0.1:8000/docs`。

默认不开启 MQTT，启用时通过环境变量注入 broker 配置，不要把凭据提交到仓库：

```powershell
$env:CLOUD_MQTT_ENABLED = "true"
$env:CLOUD_MQTT_HOST = "broker.example.com"
$env:CLOUD_MQTT_PORT = "1883"
$env:CLOUD_MQTT_USERNAME = "mesh-cloud-service"
$env:CLOUD_MQTT_PASSWORD = "set-in-secret-manager"
```

完整变量参考 [`.env.example`](.env.example)。服务不会自动读取 `.env`，生产环境应由 systemd、容器平台或密钥管理服务注入。

## API

配置 `CLOUD_API_TOKEN` 后，除 `/health` 外的 API 均需要请求头 `X-Cloud-Token`。

| 方法 | 路径 | 用途 |
|---|---|---|
| `GET` | `/health` | 服务与 MQTT 连接状态 |
| `POST` | `/api/v1/devices` | 注册或刷新设备元数据 |
| `POST` | `/api/v1/devices/<id>/claim` | 将未认领设备绑定到用户 |
| `GET` | `/api/v1/users/<user>/devices` | 查询用户设备 |
| `GET` | `/api/v1/devices/<id>/shadow` | 读取统一设备影子 |
| `PATCH` | `/api/v1/devices/<id>/shadow/desired` | 合并期望状态并尝试下发 MQTT |
| `POST` | `/api/v1/devices/<id>/offline` | 记录明确离线原因 |

示例：

```bash
curl -X POST http://127.0.0.1:8000/api/v1/devices \
  -H "X-Cloud-Token: replace-with-a-random-token" \
  -H "Content-Type: application/json" \
  -d '{"device_id":"node-001","type":"light_bulb","name":"书房灯"}'

curl -X PATCH http://127.0.0.1:8000/api/v1/devices/node-001/shadow/desired \
  -H "X-Cloud-Token: replace-with-a-random-token" \
  -H "Content-Type: application/json" \
  -d '{"state":{"on":true},"message_id":"command-001"}'
```

## 验证

```powershell
.\.venv\Scripts\python -m unittest discover -s tests -v
```

## 后续边界

当前 `user_id` 是设备归属模型，不等同于完整登录系统。公网部署前还需接入正式用户认证、TLS、broker ACL 和凭据轮换；房间、场景、自动化、OTA 与门铃事件索引在后续切片实现。
