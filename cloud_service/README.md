# Phase B 云端设备模型（首个纵向切片 + 房间/分组/场景/自动化）

该服务把门铃与 ESP-WIFI-MESH MQTT 消息收敛为统一设备注册表和设备影子。首版采用 FastAPI + SQLite，本地即可运行；新增状态字段和门铃状态主题均向后兼容现有 App。

## 已实现

- 设备注册、认领和按用户查询。
- `desired/reported` 双向设备影子及独立版本号。
- `message_id` 幂等处理。
- MQTT 状态接入、既有单节点控制主题下发。
- 设备超时离线和 `offline_reason=cloud_timeout`。
- Mesh 网关和门铃 LWT 离线状态，固件状态/事件原生携带 `version` 与 `message_id`。
- 房间：按用户分组设备，设备最多归属一个房间；删除房间自动清空设备归属。
- 设备分组：多对多集合，支持整组下发一次 `desired` 命令。
- 场景：一组设备的 `desired` 快照，激活时批量下发。
- 自动化：`触发 → 动作` 规则，`reported` 字段命中条件时执行动作（下发设备/整组/激活场景）。
- 邮箱账号体系（demo 级）：注册/登录换取 per-user Bearer token；每个账号是独立设备空间，只能访问自己认领的设备。
- 门铃事件记录：门铃 `event` 主题落库，支持按设备/按账号分页查询（`message_id` 幂等去重）。
- OTA 服务端：固件仓库（`product_id + hw_version + fw_version → url + sha256 + sign`）、灰度 rollout、`down/ota` 下发、升级进度回报；无匹配固件不误下发，重复下发幂等。
- 可选 `X-Cloud-Token` 共享密钥（管理/设备置备用）；默认只监听 `127.0.0.1`。

## MQTT 兼容映射

| 现有主题 | 云端行为 |
|---|---|
| `office/light/node/+/status` | 自动注册节点并更新 `reported` |
| `office/light/gateway/status` | 自动注册网关并更新 `reported` |
| `doorbell/+/status` | 自动注册门铃并接收在线状态或 `mqtt_lwt` 离线原因 |
| `doorbell/+/event` | 自动注册门铃并记录 `last_event` |
| `office/light/node/<id>/cmd` | `desired.on` 转换为原有 `on/off` 指令 |
| `doorbell/<id>/cmd` | `desired.command` 透传原有门铃命令 |

`reported` 每次变化后会评估以该设备为触发源的启用自动化，命中则通过既有下发通道执行动作，不新增或改变任何 MQTT 主题语义。

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

两种鉴权方式并存：

- **共享密钥 `X-Cloud-Token`**（配置 `CLOUD_API_TOKEN` 后）：用于设备置备与管理类接口（注册设备、`/api/v1/users/<user>/...`、房间/分组/场景/自动化），视为管理员。
- **per-user Bearer token**：注册/登录得到，请求头 `Authorization: Bearer <token>`。用于终端用户访问自己的设备（`/api/v1/me/...`、设备影子/事件）。带 Bearer 访问设备接口时强制校验设备归属，跨账号返回 403。

`/health` 与 `/api/v1/auth/register`、`/api/v1/auth/login` 为匿名接口。

### 账号与鉴权（demo 级）

| 方法 | 路径 | 用途 |
|---|---|---|
| `POST` | `/api/v1/auth/register` | 邮箱+密码注册，返回 `{user_id,email,token}` |
| `POST` | `/api/v1/auth/login` | 邮箱+密码登录，返回新 token |
| `GET` | `/api/v1/auth/me` | 读取当前账号（需 Bearer） |
| `POST` | `/api/v1/auth/logout` | 使当前 token 失效 |

密码用标准库 PBKDF2-HMAC-SHA256（加随机盐）哈希，token 为不透明随机串。仅用于 demo，不含邮箱验证、找回密码、过期与限流。

### 设备与影子

| 方法 | 路径 | 用途 |
|---|---|---|
| `GET` | `/health` | 服务与 MQTT 连接状态 |
| `POST` | `/api/v1/devices` | 注册或刷新设备元数据（管理员） |
| `POST` | `/api/v1/devices/<id>/claim` | 将未认领设备绑定到指定用户（管理员） |
| `GET` | `/api/v1/users/<user>/devices` | 查询指定用户设备（管理员） |
| `GET` | `/api/v1/me/devices` | 查询当前账号设备（Bearer） |
| `POST` | `/api/v1/me/devices/<id>/claim` | 将设备认领到当前账号（Bearer） |
| `GET` | `/api/v1/devices/<id>/shadow` | 读取统一设备影子（归属校验） |
| `PATCH` | `/api/v1/devices/<id>/shadow/desired` | 合并期望状态并尝试下发 MQTT（归属校验） |
| `POST` | `/api/v1/devices/<id>/offline` | 记录明确离线原因 |

### 门铃事件

| 方法 | 路径 | 用途 |
|---|---|---|
| `GET` | `/api/v1/me/events` | 当前账号所有设备的事件（分页，Bearer） |
| `GET` | `/api/v1/devices/<id>/events` | 指定设备的事件（分页，归属校验） |

分页参数：`limit`（1–200，默认 50）、`before_id`（游标，取更早事件）。

### 房间

| 方法 | 路径 | 用途 |
|---|---|---|
| `POST` | `/api/v1/users/<user>/rooms` | 新建房间 |
| `GET` | `/api/v1/users/<user>/rooms` | 列出用户房间 |
| `GET` `PATCH` `DELETE` | `/api/v1/rooms/<id>` | 读取、重命名、删除房间 |
| `PUT` `DELETE` | `/api/v1/rooms/<id>/devices/<device>` | 将设备加入/移出房间 |

### 分组

| 方法 | 路径 | 用途 |
|---|---|---|
| `POST` | `/api/v1/users/<user>/groups` | 新建分组 |
| `GET` | `/api/v1/users/<user>/groups` | 列出用户分组 |
| `GET` `PATCH` `DELETE` | `/api/v1/groups/<id>` | 读取、重命名、删除分组 |
| `PUT` `DELETE` | `/api/v1/groups/<id>/devices/<device>` | 增删分组成员 |
| `POST` | `/api/v1/groups/<id>/command` | 对整组成员下发一次 `desired` |

### 场景

| 方法 | 路径 | 用途 |
|---|---|---|
| `POST` | `/api/v1/users/<user>/scenes` | 新建场景（含设备动作快照） |
| `GET` | `/api/v1/users/<user>/scenes` | 列出用户场景 |
| `GET` `PATCH` `DELETE` | `/api/v1/scenes/<id>` | 读取、更新、删除场景 |
| `POST` | `/api/v1/scenes/<id>/activate` | 激活场景，批量下发 `desired` |

### 自动化

| 方法 | 路径 | 用途 |
|---|---|---|
| `POST` | `/api/v1/users/<user>/automations` | 新建规则（触发 + 动作） |
| `GET` | `/api/v1/users/<user>/automations` | 列出用户规则 |
| `GET` `PATCH` `DELETE` | `/api/v1/automations/<id>` | 读取、更新（含启用/停用）、删除规则 |

示例：

```bash
# 账号：注册并拿到 per-user token（App 走这条路径）
TOKEN=$(curl -s -X POST http://127.0.0.1:8000/api/v1/auth/register \
  -H "Content-Type: application/json" \
  -d '{"email":"me@example.com","password":"secret123"}' | python -c "import sys,json;print(json.load(sys.stdin)['token'])")

# 用账号 token 认领设备并查询自己的设备
curl -X POST http://127.0.0.1:8000/api/v1/me/devices/node-001/claim \
  -H "Authorization: Bearer $TOKEN"
curl http://127.0.0.1:8000/api/v1/me/devices -H "Authorization: Bearer $TOKEN"

# 设备置备仍用共享密钥（管理员）
curl -X POST http://127.0.0.1:8000/api/v1/devices \
  -H "X-Cloud-Token: replace-with-a-random-token" \
  -H "Content-Type: application/json" \
  -d '{"device_id":"node-001","type":"light_bulb","name":"书房灯"}'

curl -X PATCH http://127.0.0.1:8000/api/v1/devices/node-001/shadow/desired \
  -H "X-Cloud-Token: replace-with-a-random-token" \
  -H "Content-Type: application/json" \
  -d '{"state":{"on":true},"message_id":"command-001"}'

# 场景：一组设备的 desired 快照
curl -X POST http://127.0.0.1:8000/api/v1/users/user-001/scenes \
  -H "X-Cloud-Token: replace-with-a-random-token" \
  -H "Content-Type: application/json" \
  -d '{"name":"回家","actions":[{"device_id":"node-001","state":{"on":true}}]}'

# 自动化：门铃响铃时激活场景
curl -X POST http://127.0.0.1:8000/api/v1/users/user-001/automations \
  -H "X-Cloud-Token: replace-with-a-random-token" \
  -H "Content-Type: application/json" \
  -d '{"name":"按门铃开灯","trigger":{"device_id":"door-001","field":"last_event","equals":"ringing"},"action":{"type":"scene","scene_id":"<scene_id>"}}'
```

动作类型：`device`（需 `device_id`+`state`）、`group`（需 `group_id`+`state`）、`scene`（需 `scene_id`）。触发条件按 `reported[field] == equals` 精确匹配。

### OTA（T-CLOUD-3）

固件仓库、灰度 rollout 与升级下发均为管理类接口（`X-Cloud-Token`），设备侧仅通过 `GET .../ota/updates` 读取自己的升级记录（归属校验）。

| 方法 | 路径 | 用途 |
|---|---|---|
| `POST` | `/api/v1/firmware` | 注册/刷新固件（同 `product_id+hw_version+fw_version` 为幂等更新） |
| `GET` | `/api/v1/firmware?product_id=` | 列出固件（可按产品过滤） |
| `GET` `DELETE` | `/api/v1/firmware/<id>` | 读取、删除固件 |
| `POST` | `/api/v1/rollouts` | 新建灰度规则 |
| `GET` | `/api/v1/rollouts` | 列出 rollout |
| `GET` `PATCH` `DELETE` | `/api/v1/rollouts/<id>` | 读取、更新、删除 rollout |
| `POST` | `/api/v1/devices/<id>/ota/check` | 按上报版本匹配并下发 OTA（管理员/置备触发） |
| `POST` | `/api/v1/devices/<id>/ota/progress` | 设备回报升级进度/结果 |
| `GET` | `/api/v1/devices/<id>/ota/updates` | 查询该设备升级记录（归属校验） |

匹配三元组为 `(product_id, hw_version, fw_version)`。rollout 按 `(product_id, hw_version)` 命中，可选 `from_fw_version` 限定当前版本，`percent`（0–100）按 `sha256(rollout_id:device_id)` 做**确定性**灰度（同一设备结果稳定）。命中后校验固件是否存在，存在才下发；`down/ota` 信封 `data` 携带 `url`、`sha256`、`sign`。返回 `reason` 覆盖 `dispatched | already_dispatched | no_rollout | up_to_date | not_selected | firmware_missing`：

- 无匹配 rollout 或目标固件缺失 → 不下发（`no_rollout` / `firmware_missing`）。
- 目标版本等于当前版本 → 不下发（`up_to_date`）。
- 已对该设备下发过同一固件且未失败 → 幂等，不重复下发（`already_dispatched`）。

设备通过规范化 `up` 上报同时带 `product_id + hw_version + fw_version`（或旧字段 `version`）时，云端会自动触发一次匹配下发。

```bash
# 注册固件
curl -X POST http://127.0.0.1:8000/api/v1/firmware \
  -H "X-Cloud-Token: replace-with-a-random-token" \
  -H "Content-Type: application/json" \
  -d '{"product_id":"bulb","hw_version":"rev-a","fw_version":"1.1.0","url":"https://ota.example.com/1.1.0.bin","sha256":"<64位十六进制>","sign":"<签名>"}'

# 建立 10% 灰度
curl -X POST http://127.0.0.1:8000/api/v1/rollouts \
  -H "X-Cloud-Token: replace-with-a-random-token" \
  -H "Content-Type: application/json" \
  -d '{"product_id":"bulb","hw_version":"rev-a","target_fw_version":"1.1.0","percent":10}'
```

## 验证

```powershell
.\.venv\Scripts\python -m unittest discover -s tests -v
```

## 部署到自有服务器（Docker）

在装有 Docker 的服务器上，进入 `cloud_service/`，创建 `.env`（compose 会自动读取）：

```bash
cat > .env <<'EOF'
CLOUD_API_TOKEN=用一段强随机串
CLOUD_MQTT_ENABLED=true
CLOUD_MQTT_HOST=121.40.131.194
CLOUD_MQTT_PORT=1883
CLOUD_MQTT_USERNAME=broker-user
CLOUD_MQTT_PASSWORD=broker-pass
# 可选：对外发布端口（默认 8000），以及数据保留天数等
CLOUD_PUBLISH_PORT=8000
EOF

docker compose up -d --build
docker compose logs -f            # 观察 MQTT 连接与设备上报
curl http://127.0.0.1:8000/health
```

SQLite 数据保存在命名卷 `cloud_data`（容器内 `/data`），升级重建容器不丢数据。`CLOUD_API_HOST` 在容器内固定为 `0.0.0.0`，由 compose 的端口映射对外暴露。

公网暴露务必加 TLS：在 `.env` 里设 `CLOUD_BIND_ADDR=127.0.0.1`，让容器只监听本机 8000，再用 Nginx/Caddy 反代到它并只放行 443。App 登录时"云端地址"填反代后的 `https://你的域名`（可带子路径，如 `https://你的域名/cloud`）。

Nginx 反代示例（子路径 `/cloud/` → 本机 8000，`proxy_pass` 末尾斜杠会剥掉前缀）：

```nginx
location ^~ /cloud/ {
    proxy_pass http://127.0.0.1:8000/;
    proxy_http_version 1.1;
    proxy_set_header Host $host;
    proxy_set_header X-Real-IP $remote_addr;
    proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
    proxy_set_header X-Forwarded-Proto $scheme;
    proxy_read_timeout 60s;
}
```

不要把真实 `.env` 提交到仓库（已在忽略之列）；broker 凭据、`CLOUD_API_TOKEN` 都应作为服务器上的密钥管理。

## 后续边界

当前邮箱账号体系为 **demo 级**：token 不过期、无邮箱验证/找回密码/登录限流，密码哈希用标准库 PBKDF2。生产化前还需正式认证（如 JWT+刷新、过期与限流）、broker ACL 和凭据轮换。TLS 由自有服务器的 Nginx 反代提供（见上）。门铃事件仅记录事件元数据，尚无快照/媒体存储与索引。OTA 已实现服务端（固件仓库、灰度、`down/ota` 下发与进度回报），设备侧 `esp_https_ota` + 双分区 A/B 与失败回滚仍待固件实现。自动化目前为单条件等值触发与即时动作，尚不含时间/多条件、延时与冷却。

## MQTT 协议规范（T-CLOUD-2）

设备注册支持 `capabilities: string[]`，例如 `["onoff", "doorbell.ring"]`。未上报能力的旧设备会返回空数组；`GET /api/v1/me/devices`、`GET /api/v1/devices/{id}` 和 shadow 响应都会带 `capabilities`，便于 App 按能力渲染控件。

规范化 topic 使用：

```text
farmely/{class}/{device_id}/{direction}/{channel}
```

- `class`: `light | doorbell | gateway | sensor | camera`
- `direction`: `up` 表示设备到云，`down` 表示云到设备
- `channel`: `status | event | cmd | ota | shadow`

规范化 payload 使用统一信封：

```json
{
  "v": 1,
  "msg_id": "uuid-or-device-message-id",
  "ts": 1730000000,
  "type": "status",
  "data": {
    "online": true
  }
}
```

云端桥接当前订阅 `farmely/+/+/up/+`，同时继续订阅兼容期旧 topic：`office/light/node/+/status`、`office/light/gateway/status`、`doorbell/+/status`、`doorbell/+/event`。旧 topic 不会被切断；新旧 topic 的上报都会进入同一份设备影子。`msg_id`（兼容旧字段 `message_id`）用于上报幂等去重，重复上报不会重复推进 reported shadow 版本或重复记录门铃事件。

云端下发 desired 命令时会同时发布新规范 topic `farmely/{class}/{device_id}/down/cmd`（信封 payload）和现有旧 topic（light: `office/light/node/{id}/cmd`，doorbell: `doorbell/{id}/cmd`），兼容期内新旧固件都可接收。
