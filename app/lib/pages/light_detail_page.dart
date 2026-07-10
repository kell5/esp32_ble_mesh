import 'dart:async';

import 'package:flutter/cupertino.dart';

import '../services/mqtt_service.dart';
import '../widgets/device_icon.dart';
import '../widgets/smart_cards.dart';

/// Detail and control page for a single mesh device.
class LightDetailPage extends StatefulWidget {
  const LightDetailPage({
    super.key,
    required this.mqtt,
    required this.deviceId,
  });

  final MqttService mqtt;
  final String deviceId;

  @override
  State<LightDetailPage> createState() => _LightDetailPageState();
}

class _LightDetailPageState extends State<LightDetailPage> {
  MeshDevice? _device;
  StreamSubscription<List<MeshDevice>>? _sub;

  @override
  void initState() {
    super.initState();
    _device = _find(widget.mqtt.devicesSnapshot);
    _sub = widget.mqtt.devices.listen((list) {
      if (!mounted) return;
      setState(() => _device = _find(list));
    });
  }

  MeshDevice? _find(List<MeshDevice> list) {
    for (final d in list) {
      if (d.id == widget.deviceId) return d;
    }
    return _device; // keep last known if it dropped out momentarily
  }

  @override
  void dispose() {
    _sub?.cancel();
    super.dispose();
  }

  void _toggle() {
    final d = _device;
    if (d == null) return;
    final value = !d.on;
    widget.mqtt.setNodeLight(d.id, value);
    setState(
      () => _device = MeshDevice(
        id: d.id,
        on: value,
        online: d.online,
        layer: d.layer,
        role: d.role,
        type: d.type,
        name: d.name,
        value: d.value,
        updatedAt: d.updatedAt,
      ),
    );
  }

  @override
  Widget build(BuildContext context) {
    final d = _device;
    return CupertinoPageScaffold(
      backgroundColor: CupertinoColors.systemGroupedBackground,
      navigationBar: CupertinoNavigationBar(
        middle: Text(d?.displayName ?? widget.deviceId),
      ),
      child: SafeArea(
        child: d == null
            ? const Center(child: Text('设备不存在'))
            : ListView(
                padding: const EdgeInsets.all(16),
                children: [
                  _heroToggle(d),
                  const SizedBox(height: 16),
                  _infoCard(d),
                  const SizedBox(height: 16),
                  _futureCard(d),
                ],
              ),
      ),
    );
  }

  Widget _heroToggle(MeshDevice d) {
    final active = d.online && d.on;
    return Container(
      padding: const EdgeInsets.symmetric(vertical: 28),
      decoration: BoxDecoration(
        color: CupertinoColors.systemBackground,
        borderRadius: BorderRadius.circular(16),
      ),
      child: Column(
        children: [
          DeviceIcon(
            type: d.type,
            online: d.online,
            on: d.on,
            size: 148,
            borderRadius: 24,
          ),
          const SizedBox(height: 14),
          Text(
            d.type.stateLabel(online: d.online, on: d.on),
            style: const TextStyle(fontSize: 16, fontWeight: FontWeight.w600),
          ),
          if (d.type.isControllable) ...[
            const SizedBox(height: 18),
            PowerButton(
              on: active,
              enabled: d.online,
              onPressed: _toggle,
              size: 64,
            ),
          ],
        ],
      ),
    );
  }

  Widget _infoCard(MeshDevice d) {
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 16),
      decoration: BoxDecoration(
        color: CupertinoColors.systemBackground,
        borderRadius: BorderRadius.circular(16),
      ),
      child: Column(
        children: [
          _row('设备 ID', d.id),
          _divider(),
          _row('设备类型', d.type.label),
          _divider(),
          _row('角色', d.isRoot ? '网关 (root)' : '节点'),
          _divider(),
          _row('Mesh 层级', 'L${d.layer}'),
          _divider(),
          _row('在线状态', d.online ? '在线' : '离线'),
          _divider(),
          _row('更新于', agoLabel(d.updatedAt)),
        ],
      ),
    );
  }

  Widget _futureCard(MeshDevice d) {
    final description = switch (d.type) {
      DeviceType.lightBulb ||
      DeviceType.ceilingLight ||
      DeviceType.lightStrip => '亮度、色温和场景将在固件上报对应能力后启用。',
      DeviceType.wallSwitch || DeviceType.relay => '可继续扩展按键模式、定时、联动规则和上电状态。',
      DeviceType.socket => '可继续扩展定时、倒计时、电量统计和过载保护。',
      DeviceType.curtainMotor => '可继续扩展开合百分比、行程校准和反向设置。',
      DeviceType.valve => '可继续扩展开度、自动关闭和安全告警。',
      DeviceType.doorLock => '可继续扩展临时密码、开锁记录和异常告警。',
      _ => '设备能力将在固件上报后显示在此页面。',
    };
    return Container(
      padding: const EdgeInsets.all(16),
      decoration: BoxDecoration(
        color: CupertinoColors.systemBackground,
        borderRadius: BorderRadius.circular(16),
      ),
      child: Row(
        children: [
          const Icon(
            CupertinoIcons.slider_horizontal_3,
            color: CupertinoColors.systemGrey,
          ),
          const SizedBox(width: 10),
          Expanded(
            child: Text(
              description,
              style: const TextStyle(
                color: CupertinoColors.systemGrey,
                fontSize: 12,
              ),
            ),
          ),
        ],
      ),
    );
  }

  Widget _row(String k, String v) => Padding(
    padding: const EdgeInsets.symmetric(vertical: 14),
    child: Row(
      children: [
        Text(k, style: const TextStyle(fontSize: 15)),
        const Spacer(),
        Text(
          v,
          style: const TextStyle(
            fontSize: 15,
            color: CupertinoColors.systemGrey,
          ),
        ),
      ],
    ),
  );

  Widget _divider() =>
      Container(height: 0.5, color: CupertinoColors.systemGrey5);
}
