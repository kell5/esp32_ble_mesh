# MJPEG 中继（A2 远程观看方案）

ESP32-S3 / OV3660 只能输出 JPEG，没有 H.264 硬编码，MJPEG-in-RTMP 又无法被
SRS/ffmpeg 解析。所以远程观看不走 RTMP/SRS，而是：

```
ESP32-S3 ──(HTTP POST, 一条长连接, multipart JPEG)──> 中继(公网ECS) ──(multipart MJPEG)──> App / 浏览器
   /pub/door-001                                        :8090                 /stream/door-001
```

板子在按门铃时把每一帧 JPEG 通过一条长连接 POST 到中继的 `/pub/<门铃id>`；
中继缓存最新帧，并以标准 `multipart/x-mixed-replace` 分发给任意数量的观看端。
App 端的纯 Dart MJPEG 解析控件、浏览器、ffmpeg 都能直接播放。

## 一、部署（在 ECS 114.55.208.72 上）

只用 Python3 标准库，无需 pip 安装。

```bash
# 上传脚本
scp server_relay/mjpeg_relay.py root@114.55.208.72:/opt/mjpeg_relay.py

# 前台快速验证
python3 /opt/mjpeg_relay.py 8090

# 或用 systemd 常驻
sudo tee /etc/systemd/system/mjpeg-relay.service >/dev/null <<'EOF'
[Unit]
Description=ESP32 doorbell MJPEG relay
After=network.target

[Service]
ExecStart=/usr/bin/python3 /opt/mjpeg_relay.py 8090
Restart=always
User=root

[Install]
WantedBy=multi-user.target
EOF
sudo systemctl daemon-reload
sudo systemctl enable --now mjpeg-relay
```

## 二、安全组 / 防火墙

放行 **TCP 8090**（0.0.0.0/0 或按需收窄）。中继只用这一个端口，
既收板子推流也发观看流。RTMP(1935)/SRS(8088)/WebRTC(8000) 这条链已弃用。

## 三、接口

| 方法 | 路径 | 用途 |
|------|------|------|
| POST | `/pub/<cam>`     | 板子推流（multipart/x-mixed-replace，每 part 一帧 JPEG） |
| GET  | `/stream/<cam>`  | App/浏览器观看（multipart/x-mixed-replace MJPEG） |
| GET  | `/<cam>.mjpg`    | 同上（浏览器友好） |
| GET  | `/status`        | JSON：各 cam 的 seq 与最近一帧的秒龄 |

`<cam>` 即门铃 id，默认 `door-001`（与固件 `EXAMPLE_DOORBELL_ID`、
App 观看地址一致）。

## 四、验证

```bash
# 浏览器直接开（板子推流期间）：
http://114.55.208.72:8090/stream/door-001
# 或命令行探测编码：
ffprobe http://114.55.208.72:8090/stream/door-001    # 应识别为 mpjpeg / mjpeg 640x480
curl -s http://114.55.208.72:8090/status              # {"door-001": {"seq": N, "age_s": x.x}}
```

## 五、固件 / App 对应配置

- 固件 `menuconfig` → Camera Stream Configuration：
  `MJPEG relay host = 114.55.208.72`、`MJPEG relay port = 8090`
  （已写入 `camera_stream/sdkconfig.defaults`，改服务器只需改这里重编译）。
- App 门铃页右上角齿轮里把观看地址填成
  `http://114.55.208.72:8090/stream/door-001` 即可远程观看；
  局域网(A1)仍可填 `http://<板子IP>:81/stream`。
