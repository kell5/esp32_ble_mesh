import 'dart:async';

import 'package:flutter/cupertino.dart';

import '../services/mqtt_service.dart';
import '../widgets/smart_cards.dart';

/// Detail / control sub-page for a single mesh device (light today). Stays
/// subscribed so the state tracks the gateway's retained status. Extra
/// capabilities (brightness, colour temperature, schedules) can be added here
/// without touching the home grid.
class LightDetailPage extends StatefulWidget {
  const LightDetailPage({super.key, required this.mqtt, required this.deviceId});

  final MqttService mqtt;
  final String deviceId;

  @override
  State<LightDetailPage> createState() => _LightDetailPageState();
}

class _LightDetailPageState extends State<LightDetailPage> {
  LightDevice? _device;
  StreamSubscription<List<LightDevice>>? _sub;

  @override
  void initState() {
    super.initState();
    _device = _find(widget.mqtt.devicesSnapshot);
    _sub = widget.mqtt.devices.listen((list) {
      if (!mounted) return;
      setState(() => _device = _find(list));
    });
  }

  LightDevice? _find(List<LightDevice> list) {
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
    setState(() => _device = LightDevice(
          id: d.id,
          on: value,
          online: d.online,
          layer: d.layer,
          role: d.role,
          type: d.type,
          name: d.name,
          value: d.value,
          updatedAt: d.updatedAt,
        ));
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
                  _futureCard(),
                ],
              ),
      ),
    );
  }

  Widget _heroToggle(LightDevice d) {
    final active = d.online && d.on;
    return Container(
      padding: const EdgeInsets.symmetric(vertical: 28),
      decoration: BoxDecoration(
        color: CupertinoColors.systemBackground,
        borderRadius: BorderRadius.circular(16),
      ),
      child: Column(
        children: [
          Icon(
            active ? CupertinoIcons.lightbulb_fill : CupertinoIcons.lightbulb,
            size: 72,
            color: !d.online
                ? CupertinoColors.systemGrey3
                : (active
                    ? CupertinoColors.systemYellow
                    : CupertinoColors.systemGrey),
          ),
          const SizedBox(height: 14),
          Text(
            !d.online ? '离线' : (d.on ? '已开启' : '已关闭'),
            style: const TextStyle(fontSize: 16, fontWeight: FontWeight.w600),
          ),
          const SizedBox(height: 18),
          PowerButton(
            on: active,
            enabled: d.online,
            onPressed: _toggle,
            size: 64,
          ),
        ],
      ),
    );
  }

  Widget _infoCard(LightDevice d) {
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

  Widget _futureCard() {
    return Container(
      padding: const EdgeInsets.all(16),
      decoration: BoxDecoration(
        color: CupertinoColors.systemBackground,
        borderRadius: BorderRadius.circular(16),
      ),
      child: Row(
        children: const [
          Icon(CupertinoIcons.slider_horizontal_3,
              color: CupertinoColors.systemGrey),
          SizedBox(width: 10),
          Expanded(
            child: Text(
              '亮度 / 色温 / 定时等功能：待设备固件上报对应能力后在此扩展。',
              style:
                  TextStyle(color: CupertinoColors.systemGrey, fontSize: 12),
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
            Text(v,
                style: const TextStyle(
                    fontSize: 15, color: CupertinoColors.systemGrey)),
          ],
        ),
      );

  Widget _divider() => Container(
      height: 0.5, color: CupertinoColors.systemGrey5);
}
