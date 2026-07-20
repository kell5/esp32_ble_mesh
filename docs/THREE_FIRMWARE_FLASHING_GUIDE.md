# 三套固件烧录与 OTA 测试说明

当前仓库可直接测试的三条线：

- Mesh 网关：`internal_communication` + `sdkconfig.gateway.defaults`
- Mesh 灯节点：`internal_communication` + `sdkconfig.node.defaults`
- 门铃：`camera_stream`

已实测确认：

- 灯节点开发板为 `ESP32-S3`
- 灯节点烧录串口为 `COM14`

## 1. 烧录对象

- 网关：负责 Mesh 入网、MQTT 桥接、OTA 分发
- 灯节点：接收 Mesh 控制，也支持 OTA
- 门铃：保持原有 App 发现/配网/视频/门铃流程，不改

## 2. 固件身份

- Mesh 网关
  - `product_id = mesh_gateway`
  - `hw_version = rev-a`
- Mesh 灯节点
  - `product_id = mesh_light`
  - `hw_version = rev-a`
- 门铃
  - `product_id = doorbell`
  - `hw_version = rev-a`

`fw_version` 由各工程根目录的 `version.txt` 提供。

## 3. 常用构建命令

在 ESP-IDF 环境里分别构建：

```powershell
idf.py -C internal_communication -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.gateway.defaults" build
idf.py -C internal_communication -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32s3;sdkconfig.node.defaults" build
idf.py -C camera_stream build
```

如果你已经在对应工程目录里，也可以直接：

```powershell
idf.py build
```

前提是当前工程选中了正确的 `sdkconfig.defaults` 组合。

## 4. OTA 流程

- 网关：
  - 云端下发 `farmely/gateway/<id>/down/ota`
  - 网关本机直接执行 OTA
- 灯节点：
  - 云端下发 `farmely/light/<id>/down/ota`
  - 网关收到后转发给对应 Mesh 节点
  - 节点自己执行 OTA，并把结果回传给网关
- 门铃：
  - 云端下发 `farmely/doorbell/<id>/down/ota`
  - 门铃本机直接执行 OTA

## 5. App 里怎么测

- 打开设备详情页
- 先确认设备已上报 `product_id / hw_version / fw_version`
- 点击“检查更新”
- 若云端固件匹配，会自动下发 OTA

## 6. 注意

- 网关和灯节点不要混刷。
- 门铃不要改成 Mesh 协议，它仍然走原来的网络链路。
- 测试前不要先提交 Git，等你确认后再提交。

## 7. 已验证烧录命令

灯控节点（ESP32-S3 / COM14）：

```powershell
cd internal_communication
cmd /c "call D:\esp-idf\v6.0.1\esp-idf\export.bat && set SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.esp32s3;sdkconfig.node.defaults && idf.py -B build-node set-target esp32s3 && idf.py -B build-node -p COM14 flash"
```
