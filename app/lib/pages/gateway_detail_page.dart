import 'dart:async';

import 'package:flutter/cupertino.dart';

import '../services/mqtt_service.dart';
import '../widgets/smart_cards.dart';
import 'light_detail_page.dart';

/// Mesh gateway sub-page: aggregate gateway status, group all-on/all-off, and
/// the list of nodes currently attached to the mesh (topology). This is the
/// "网关子页" of the unified device home.
class GatewayDetailPage extends StatefulWidget {
  const GatewayDetailPage({super.key, required this.mqtt});

  final MqttService mqtt;

  @override
  State<GatewayDetailPage> createState() => _GatewayDetailPageState();
}

class _GatewayDetailPageState extends State<GatewayDetailPage> {
  List<LightDevice> _devices = const [];
  GatewayStatus? _gateway;

  StreamSubscription<List<LightDevice>>? _devSub;
  StreamSubscription<GatewayStatus?>? _gwSub;

  @override
  void initState() {
    super.initState();
    _devices = widget.mqtt.devicesSnapshot;
    _gateway = widget.mqtt.gatewaySnapshot;
    _devSub = widget.mqtt.devices.listen((list) {
      if (!mounted) return;
      setState(() => _devices = list);
    });
    _gwSub = widget.mqtt.gateway.listen((gw) {
      if (!mounted) return;
      setState(() => _gateway = gw);
    });
  }

  @override
  void dispose() {
    _devSub?.cancel();
    _gwSub?.cancel();
    super.dispose();
  }

  int get _onlineCount => _devices.where((d) => d.online).length;

  @override
  Widget build(BuildContext context) {
    return CupertinoPageScaffold(
      backgroundColor: CupertinoColors.systemGroupedBackground,
      navigationBar: const CupertinoNavigationBar(middle: Text('Mesh 网关')),
      child: SafeArea(
        child: ListView(
          padding: const EdgeInsets.all(16),
          children: [
            _statusCard(),
            const SizedBox(height: 16),
            _groupCard(),
            const SizedBox(height: 16),
            const Padding(
              padding: EdgeInsets.only(left: 4, bottom: 8),
              child: Text('网络节点',
                  style: TextStyle(fontWeight: FontWeight.w600)),
            ),
            ..._devices.map(_nodeRow),
            if (_devices.isEmpty)
              const Padding(
                padding: EdgeInsets.all(16),
                child: Text('暂无节点',
                    style: TextStyle(color: CupertinoColors.systemGrey)),
              ),
          ],
        ),
      ),
    );
  }

  Widget _statusCard() {
    final gw = _gateway;
    return Container(
      padding: const EdgeInsets.all(16),
      decoration: BoxDecoration(
        color: CupertinoColors.systemBackground,
        borderRadius: BorderRadius.circular(16),
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(
            children: [
              Icon(
                gw != null && gw.online
                    ? CupertinoIcons.antenna_radiowaves_left_right
                    : CupertinoIcons.wifi_slash,
                color: gw != null && gw.online
                    ? CupertinoColors.activeGreen
                    : CupertinoColors.systemGrey,
                size: 28,
              ),
              const SizedBox(width: 12),
              Text(gw != null && gw.online ? '网关在线' : '网关离线',
                  style: const TextStyle(
                      fontSize: 16, fontWeight: FontWeight.w600)),
              const Spacer(),
              Text('$_onlineCount 在线/${_devices.length}',
                  style: const TextStyle(
                      color: CupertinoColors.systemGrey, fontSize: 12)),
            ],
          ),
          const SizedBox(height: 12),
          if (gw != null) ...[
            _kv('Root', gw.root),
            _kv('Mesh 层级', 'L${gw.layer}'),
            _kv('节点数', '${gw.nodes}'),
            _kv('更新于', agoLabel(gw.updatedAt)),
          ] else
            const Text('等待网关上报…',
                style: TextStyle(color: CupertinoColors.systemGrey)),
        ],
      ),
    );
  }

  Widget _kv(String k, String v) => Padding(
        padding: const EdgeInsets.symmetric(vertical: 3),
        child: Row(
          children: [
            Text(k,
                style: const TextStyle(
                    color: CupertinoColors.systemGrey, fontSize: 13)),
            const Spacer(),
            Text(v, style: const TextStyle(fontSize: 13)),
          ],
        ),
      );

  Widget _groupCard() {
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 12),
      decoration: BoxDecoration(
        color: CupertinoColors.systemBackground,
        borderRadius: BorderRadius.circular(16),
      ),
      child: Row(
        children: [
          const Icon(CupertinoIcons.square_grid_2x2,
              color: CupertinoColors.activeBlue),
          const SizedBox(width: 10),
          const Text('全部设备',
              style: TextStyle(fontSize: 16, fontWeight: FontWeight.w600)),
          const Spacer(),
          PillButton(
              label: '全开',
              filled: true,
              onPressed: () => widget.mqtt.setAllLights(true)),
          const SizedBox(width: 8),
          PillButton(
              label: '全关',
              filled: false,
              onPressed: () => widget.mqtt.setAllLights(false)),
        ],
      ),
    );
  }

  Widget _nodeRow(LightDevice d) {
    return GestureDetector(
      onTap: () => Navigator.of(context).push(
        CupertinoPageRoute<void>(
          builder: (_) => LightDetailPage(mqtt: widget.mqtt, deviceId: d.id),
        ),
      ),
      behavior: HitTestBehavior.opaque,
      child: Container(
        margin: const EdgeInsets.only(bottom: 8),
        padding: const EdgeInsets.all(14),
        decoration: BoxDecoration(
          color: CupertinoColors.systemBackground,
          borderRadius: BorderRadius.circular(14),
        ),
        child: Row(
          children: [
            Icon(
              d.isRoot
                  ? CupertinoIcons.antenna_radiowaves_left_right
                  : CupertinoIcons.lightbulb,
              color: d.online
                  ? CupertinoColors.activeBlue
                  : CupertinoColors.systemGrey3,
            ),
            const SizedBox(width: 10),
            Expanded(
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Text(d.displayName,
                      style: const TextStyle(fontWeight: FontWeight.w600)),
                  const SizedBox(height: 2),
                  Text(
                    '${d.isRoot ? '网关' : '节点'} · L${d.layer} · ${d.online ? (d.on ? '已开启' : '已关闭') : '离线'}',
                    style: const TextStyle(
                        color: CupertinoColors.systemGrey, fontSize: 12),
                  ),
                ],
              ),
            ),
            const Icon(CupertinoIcons.chevron_forward,
                size: 16, color: CupertinoColors.systemGrey3),
          ],
        ),
      ),
    );
  }
}
