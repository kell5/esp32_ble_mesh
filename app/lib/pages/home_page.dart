import 'dart:async';

import 'package:flutter/cupertino.dart';

import '../services/mqtt_service.dart';
import '../widgets/smart_cards.dart';
import 'doorbell_page.dart';
import 'gateway_detail_page.dart';
import 'light_detail_page.dart';
import 'provisioning_page.dart';

/// Unified device home (à la Huawei "智慧生活"): every device is a card in a
/// 2-column grid. Tapping a card opens its type-specific sub-page. A single
/// "+ 添加设备" entry drives provisioning for any Wi-Fi device. New device
/// classes slot in by extending the tile builder — the grid never changes.
class HomePage extends StatefulWidget {
  const HomePage({super.key, required this.mqtt});

  final MqttService mqtt;

  @override
  State<HomePage> createState() => _HomePageState();
}

class _HomePageState extends State<HomePage> {
  List<LightDevice> _devices = const [];
  GatewayStatus? _gateway;
  bool _connected = false;

  StreamSubscription<List<LightDevice>>? _devSub;
  StreamSubscription<GatewayStatus?>? _gwSub;
  StreamSubscription<bool>? _connSub;

  @override
  void initState() {
    super.initState();
    _devices = widget.mqtt.devicesSnapshot;
    _gateway = widget.mqtt.gatewaySnapshot;
    _connected = widget.mqtt.isConnected;

    _devSub = widget.mqtt.devices.listen((list) {
      if (!mounted) return;
      setState(() => _devices = list);
    });
    _gwSub = widget.mqtt.gateway.listen((gw) {
      if (!mounted) return;
      setState(() => _gateway = gw);
    });
    _connSub = widget.mqtt.connection.listen((c) {
      if (!mounted) return;
      setState(() => _connected = c);
    });
  }

  @override
  void dispose() {
    _devSub?.cancel();
    _gwSub?.cancel();
    _connSub?.cancel();
    super.dispose();
  }

  bool get _gatewayLive {
    final gw = _gateway;
    if (gw == null || !gw.online) return false;
    return DateTime.now().difference(gw.updatedAt).inSeconds < 35;
  }

  int get _onlineCount => _devices.where((d) => d.online).length;

  void _toggle(LightDevice d) {
    final value = !d.on;
    widget.mqtt.setNodeLight(d.id, value);
    setState(() {
      _devices = _devices
          .map((x) => x.id == d.id
              ? LightDevice(
                  id: x.id,
                  on: value,
                  online: x.online,
                  layer: x.layer,
                  role: x.role,
                  type: x.type,
                  name: x.name,
                  value: x.value,
                  updatedAt: x.updatedAt)
              : x)
          .toList();
    });
  }

  void _openDoorbell() => Navigator.of(context).push(
      CupertinoPageRoute<void>(builder: (_) => DoorbellPage(mqtt: widget.mqtt)));

  void _openGateway() => Navigator.of(context).push(CupertinoPageRoute<void>(
      builder: (_) => GatewayDetailPage(mqtt: widget.mqtt)));

  void _openLight(LightDevice d) => Navigator.of(context).push(
      CupertinoPageRoute<void>(
          builder: (_) => LightDetailPage(mqtt: widget.mqtt, deviceId: d.id)));

  void _addDevice() {
    showCupertinoModalPopup<void>(
      context: context,
      builder: (ctx) => CupertinoActionSheet(
        title: const Text('添加设备'),
        message: const Text('让待配网设备进入配网模式，选择一种方式下发 WiFi。'),
        actions: [
          CupertinoActionSheetAction(
            onPressed: () {
              Navigator.of(ctx).pop();
              Navigator.of(context).push(CupertinoPageRoute<void>(
                  builder: (_) => const ProvisioningPage()));
            },
            child: const Text('手机配网（连设备热点下发 WiFi）'),
          ),
          CupertinoActionSheetAction(
            onPressed: () {
              Navigator.of(ctx).pop();
              _bleComingSoon();
            },
            child: const Text('BLE 配网（即将支持）'),
          ),
        ],
        cancelButton: CupertinoActionSheetAction(
          isDefaultAction: true,
          onPressed: () => Navigator.of(ctx).pop(),
          child: const Text('取消'),
        ),
      ),
    );
  }

  void _bleComingSoon() {
    showCupertinoDialog<void>(
      context: context,
      builder: (ctx) => CupertinoAlertDialog(
        title: const Text('BLE 统一配网'),
        content: const Text(
            '门铃 / 监控 / 网关 / 灯将共用同一套 BLE 加密配网流程（扫描→选设备→填 WiFi→自动连接）。\n设备侧固件接入后即可启用。'),
        actions: [
          CupertinoDialogAction(
            isDefaultAction: true,
            onPressed: () => Navigator.of(ctx).pop(),
            child: const Text('好'),
          ),
        ],
      ),
    );
  }

  @override
  Widget build(BuildContext context) {
    return CupertinoPageScaffold(
      backgroundColor: CupertinoColors.systemGroupedBackground,
      navigationBar: CupertinoNavigationBar(
        middle: const Text('我的设备'),
        trailing: CupertinoButton(
          padding: EdgeInsets.zero,
          onPressed: _addDevice,
          child: const Icon(CupertinoIcons.add_circled),
        ),
      ),
      child: SafeArea(
        child: CustomScrollView(
          slivers: [
            SliverToBoxAdapter(child: _gatewayBanner()),
            SliverPadding(
              padding: const EdgeInsets.fromLTRB(12, 4, 12, 12),
              sliver: SliverGrid(
                gridDelegate:
                    const SliverGridDelegateWithFixedCrossAxisCount(
                  crossAxisCount: 2,
                  mainAxisSpacing: 12,
                  crossAxisSpacing: 12,
                  childAspectRatio: 1.25,
                ),
                delegate: SliverChildListDelegate(_tiles()),
              ),
            ),
            const SliverToBoxAdapter(
              child: Padding(
                padding: EdgeInsets.fromLTRB(20, 4, 20, 24),
                child: Text(
                  '设备上电自动入网、自动发现；点任意设备进入其功能子页。以后新增控制机构/检测设备会自动作为新卡片出现。',
                  style: TextStyle(
                      color: CupertinoColors.systemGrey, fontSize: 12),
                ),
              ),
            ),
          ],
        ),
      ),
    );
  }

  List<Widget> _tiles() {
    final tiles = <Widget>[
      // Doorbell: fixed device from config, navigation-only card.
      SmartCard(
        icon: CupertinoIcons.bell_fill,
        iconColor: _connected
            ? CupertinoColors.activeBlue
            : CupertinoColors.systemGrey3,
        title: '门铃',
        subtitle: _connected ? '已连接' : '未连接',
        online: _connected,
        trailing: const Icon(CupertinoIcons.chevron_forward,
            size: 16, color: CupertinoColors.systemGrey3),
        onTap: _openDoorbell,
      ),
    ];

    for (final d in _devices) {
      final active = d.online && d.on && d.type.isControllable;
      tiles.add(SmartCard(
        icon: _iconFor(d),
        iconColor: !d.online
            ? CupertinoColors.systemGrey3
            : (active
                ? CupertinoColors.systemYellow
                : CupertinoColors.systemGrey),
        title: d.displayName,
        subtitle: _subtitleFor(d),
        online: d.online,
        trailing: d.type.isControllable
            ? PowerButton(
                on: active, enabled: d.online, onPressed: () => _toggle(d))
            : (d.value != null
                ? Text(d.value!,
                    style: const TextStyle(
                        fontSize: 16,
                        fontWeight: FontWeight.bold,
                        color: CupertinoColors.activeBlue))
                : null),
        onTap: () => _openLight(d),
      ));
    }
    return tiles;
  }

  IconData _iconFor(LightDevice d) {
    switch (d.type) {
      case DeviceType.sensor:
        return CupertinoIcons.thermometer;
      case DeviceType.switch_:
        return CupertinoIcons.power;
      case DeviceType.light:
      case DeviceType.unknown:
        return d.on ? CupertinoIcons.lightbulb_fill : CupertinoIcons.lightbulb;
    }
  }

  String _subtitleFor(LightDevice d) {
    final where = d.isRoot ? '网关' : '节点 · L${d.layer}';
    final state = !d.online
        ? '离线'
        : (d.type.isControllable ? (d.on ? '已开启' : '已关闭') : '在线');
    return '$where · $state';
  }

  Widget _gatewayBanner() {
    final gw = _gateway;
    final live = _gatewayLive;
    return GestureDetector(
      onTap: _openGateway,
      behavior: HitTestBehavior.opaque,
      child: Container(
        margin: const EdgeInsets.fromLTRB(12, 12, 12, 6),
        padding: const EdgeInsets.all(16),
        decoration: BoxDecoration(
          color: CupertinoColors.systemBackground,
          borderRadius: BorderRadius.circular(16),
        ),
        child: Row(
          children: [
            Icon(
              live
                  ? CupertinoIcons.antenna_radiowaves_left_right
                  : CupertinoIcons.wifi_slash,
              color: live
                  ? CupertinoColors.activeGreen
                  : CupertinoColors.systemGrey,
              size: 28,
            ),
            const SizedBox(width: 12),
            Expanded(
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Text(
                    live
                        ? 'Mesh 网关在线'
                        : (_connected ? '等待网关上报…' : '未连接服务器'),
                    style: const TextStyle(
                        fontSize: 16, fontWeight: FontWeight.w600),
                  ),
                  const SizedBox(height: 2),
                  Text(
                    gw == null
                        ? '点此查看网络拓扑'
                        : 'root ${gw.root} · L${gw.layer} · 更新于 ${agoLabel(gw.updatedAt)}',
                    style: const TextStyle(
                        color: CupertinoColors.systemGrey, fontSize: 12),
                  ),
                ],
              ),
            ),
            Column(
              crossAxisAlignment: CrossAxisAlignment.end,
              children: [
                Text('$_onlineCount',
                    style: const TextStyle(
                        fontSize: 22,
                        fontWeight: FontWeight.bold,
                        color: CupertinoColors.activeBlue)),
                Text('在线/${_devices.length}',
                    style: const TextStyle(
                        color: CupertinoColors.systemGrey, fontSize: 11)),
              ],
            ),
            const SizedBox(width: 4),
            const Icon(CupertinoIcons.chevron_forward,
                size: 16, color: CupertinoColors.systemGrey3),
          ],
        ),
      ),
    );
  }
}
