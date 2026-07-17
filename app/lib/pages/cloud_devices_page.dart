import 'dart:async';

import 'package:flutter/cupertino.dart';

import '../services/cloud_client.dart';
import '../services/cloud_session.dart';
import '../services/mqtt_service.dart';
import '../widgets/device_icon.dart';
import '../widgets/smart_cards.dart';
import 'cloud_device_detail_page.dart';
import 'doorbell_page.dart';
import 'gateway_detail_page.dart';
import 'provisioning_page.dart';

/// The single unified device list. Its data source is the logged-in account
/// (`GET /me/devices`); the local MQTT network is only a transparent transport
/// layer used to accelerate control when the device is reachable on the LAN.
/// Controllable devices toggle via that transport, doorbell/gateway devices
/// open their live MQTT sub-pages, and everything else opens the cloud shadow
/// detail page.
class CloudDevicesPage extends StatefulWidget {
  const CloudDevicesPage({
    super.key,
    required this.mqtt,
    required this.session,
    this.onLogout,
  });

  final MqttService mqtt;
  final CloudSession session;
  final VoidCallback? onLogout;

  @override
  State<CloudDevicesPage> createState() => _CloudDevicesPageState();
}

class _CloudDevicesPageState extends State<CloudDevicesPage> {
  CloudClient? _client;
  List<CloudDeviceView> _devices = const [];
  bool _loading = true;
  String? _error;

  /// Live per-node state from the local MQTT bridge; fresher than the cloud
  /// shadow, so cards reflect a toggle as soon as the gateway confirms it.
  Map<String, MeshDevice> _live = const {};
  StreamSubscription<List<MeshDevice>>? _liveSub;

  /// Optimistic on/off per device while a toggle is in flight.
  final Map<String, bool> _pendingOn = {};

  @override
  void initState() {
    super.initState();
    _client = CloudClient(
      baseUrl: widget.session.baseUrl,
      token: widget.session.token,
    );
    _live = {for (final d in widget.mqtt.devicesSnapshot) d.id: d};
    _liveSub = widget.mqtt.devices.listen((list) {
      if (!mounted) return;
      setState(() {
        _live = {for (final d in list) d.id: d};
        _pendingOn.removeWhere(
          (id, on) => _live[id] != null && _live[id]!.on == on,
        );
      });
    });
    _refresh();
  }

  @override
  void dispose() {
    _liveSub?.cancel();
    _client?.close();
    super.dispose();
  }

  bool _effectiveOn(CloudDeviceView view) {
    final id = view.device.deviceId;
    return _pendingOn[id] ?? _live[id]?.on ?? view.on;
  }

  bool _effectiveOnline(CloudDeviceView view) {
    final id = view.device.deviceId;
    final live = _live[id];
    return (live != null && live.online) || view.online;
  }

  Future<void> _refresh() async {
    final client = _client;
    if (client == null) return;
    setState(() {
      _loading = true;
      _error = null;
    });
    try {
      final views = await client.listDeviceViews();
      if (!mounted) return;
      setState(() {
        _devices = views;
        _loading = false;
      });
    } on CloudApiException catch (e) {
      if (!mounted) return;
      setState(() {
        _error = e.message;
        _loading = false;
      });
    }
  }

  Future<void> _logout() async {
    await widget.session.clear();
    _client?.close();
    _client = null;
    widget.onLogout?.call();
  }

  /// True when the device is reachable on the local MQTT network right now, so
  /// control can skip the cloud round-trip (transparent LAN acceleration).
  bool _reachableLocally(String deviceId) {
    if (!widget.mqtt.isConnected) return false;
    return widget.mqtt.devicesSnapshot.any(
      (d) => d.id == deviceId && d.online,
    );
  }

  Future<void> _toggle(CloudDeviceView view) async {
    final deviceId = view.device.deviceId;
    final target = !_effectiveOn(view);
    // Flip the card immediately; the MQTT listener clears the override as
    // soon as the gateway reports the confirmed node state.
    setState(() => _pendingOn[deviceId] = target);
    if (_reachableLocally(deviceId)) {
      widget.mqtt.setNodeLight(deviceId, target);
      // Fallback: if no confirmation arrived, drop the optimistic state and
      // re-read the cloud shadow.
      unawaited(
        Future<void>.delayed(const Duration(seconds: 4)).then((_) async {
          if (!mounted) return;
          if (_pendingOn.remove(deviceId) != null) await _refresh();
        }),
      );
      return;
    }
    final client = _client;
    if (client == null) return;
    try {
      await client.setDesired(
        deviceId,
        {'on': target},
        messageId: 'app-${DateTime.now().millisecondsSinceEpoch}',
      );
      await _refresh();
    } on CloudApiException catch (e) {
      if (!mounted) return;
      setState(() {
        _pendingOn.remove(deviceId);
        _error = e.message;
      });
    }
  }

  /// Opens the add-device screen (LAN auto-discovery + manual id + BLE
  /// provisioning), then refreshes if anything was claimed.
  Future<void> _addDevice() async {
    final client = _client;
    if (client == null) return;
    final claimedIds = {for (final v in _devices) v.device.deviceId};
    final changed = await Navigator.of(context).push<bool>(
      CupertinoPageRoute<bool>(
        builder: (_) => _AddDevicePage(
          client: client,
          mqtt: widget.mqtt,
          claimedIds: claimedIds,
        ),
      ),
    );
    if (changed == true) await _refresh();
  }

  void _openDetail(CloudDeviceView view) {
    final type = view.device.deviceType;
    if (type == DeviceType.doorbell || type == DeviceType.camera) {
      Navigator.of(context)
          .push(
            CupertinoPageRoute<void>(
              builder: (_) => DoorbellPage(mqtt: widget.mqtt),
            ),
          )
          .then((_) => _refresh());
      return;
    }
    if (type == DeviceType.gateway) {
      Navigator.of(context)
          .push(
            CupertinoPageRoute<void>(
              builder: (_) => GatewayDetailPage(mqtt: widget.mqtt),
            ),
          )
          .then((_) => _refresh());
      return;
    }
    final client = _client;
    if (client == null) return;
    Navigator.of(context)
        .push(
          CupertinoPageRoute<void>(
            builder: (_) => CloudDeviceDetailPage(
              client: client,
              device: view.device,
              initialShadow: view.shadow,
            ),
          ),
        )
        .then((_) => _refresh());
  }

  @override
  Widget build(BuildContext context) {
    return CupertinoPageScaffold(
      backgroundColor: CupertinoColors.systemGroupedBackground,
      navigationBar: CupertinoNavigationBar(
        middle: const Text('我的设备'),
        trailing: Row(
          mainAxisSize: MainAxisSize.min,
          children: [
            CupertinoButton(
              padding: EdgeInsets.zero,
              onPressed: _addDevice,
              child: const Icon(CupertinoIcons.add_circled),
            ),
            CupertinoButton(
              padding: EdgeInsets.zero,
              onPressed: _showAccountSheet,
              child: const Icon(CupertinoIcons.person_crop_circle),
            ),
          ],
        ),
      ),
      child: SafeArea(child: _body()),
    );
  }

  Widget _body() {
    if (_loading && _devices.isEmpty) {
      return const Center(child: CupertinoActivityIndicator());
    }
    return CustomScrollView(
      slivers: [
        CupertinoSliverRefreshControl(onRefresh: _refresh),
        if (_error != null)
          SliverToBoxAdapter(child: _errorBanner(_error!)),
        if (_devices.isEmpty && _error == null)
          SliverToBoxAdapter(child: _emptyState())
        else
          SliverPadding(
            padding: const EdgeInsets.fromLTRB(12, 8, 12, 16),
            sliver: SliverGrid(
              gridDelegate: const SliverGridDelegateWithFixedCrossAxisCount(
                crossAxisCount: 2,
                mainAxisSpacing: 12,
                crossAxisSpacing: 12,
                childAspectRatio: 1.05,
              ),
              delegate: SliverChildListDelegate(_tiles()),
            ),
          ),
      ],
    );
  }

  List<Widget> _tiles() {
    return [
      for (final view in _devices)
        SmartCard(
          leading: DeviceIcon(
            type: view.device.deviceType,
            online: _effectiveOnline(view),
            on: _effectiveOn(view),
          ),
          title: view.device.displayName,
          subtitle: _subtitle(view),
          online: _effectiveOnline(view),
          trailing: view.isControllable
              ? PowerButton(
                  on: _effectiveOnline(view) && _effectiveOn(view),
                  enabled: _effectiveOnline(view),
                  onPressed: () => _toggle(view),
                )
              : (view.shadow?.value != null
                    ? Text(
                        view.shadow!.value!,
                        style: const TextStyle(
                          fontSize: 16,
                          fontWeight: FontWeight.bold,
                          color: CupertinoColors.activeBlue,
                        ),
                      )
                    : null),
          onTap: () => _openDetail(view),
          onLongPress: () => _confirmRemove(view),
        ),
    ];
  }

  /// Long-press a device card to remove (unclaim) it from the account.
  void _confirmRemove(CloudDeviceView view) {
    showCupertinoModalPopup<void>(
      context: context,
      builder: (sheetContext) => CupertinoActionSheet(
        title: Text(view.device.displayName),
        message: const Text('从当前账号移除该设备？设备本身不受影响，'
            '之后可重新添加。'),
        actions: [
          CupertinoActionSheetAction(
            isDestructiveAction: true,
            onPressed: () {
              Navigator.of(sheetContext).pop();
              _removeDevice(view);
            },
            child: const Text('移除设备'),
          ),
        ],
        cancelButton: CupertinoActionSheetAction(
          onPressed: () => Navigator.of(sheetContext).pop(),
          child: const Text('取消'),
        ),
      ),
    );
  }

  Future<void> _removeDevice(CloudDeviceView view) async {
    final client = _client;
    if (client == null) return;
    try {
      await client.unclaimDevice(view.device.deviceId);
      await _refresh();
    } on CloudApiException catch (e) {
      if (!mounted) return;
      setState(() => _error = e.message);
    }
  }

  String _subtitle(CloudDeviceView view) {
    final type = view.device.deviceType;
    return type.stateLabel(
      online: _effectiveOnline(view),
      on: _effectiveOn(view),
    );
  }

  Widget _emptyState() {
    return Padding(
      padding: const EdgeInsets.all(32),
      child: Column(
        children: [
          const Icon(
            CupertinoIcons.square_stack_3d_up,
            size: 56,
            color: CupertinoColors.systemGrey2,
          ),
          const SizedBox(height: 12),
          const Text(
            '还没有设备',
            style: TextStyle(fontSize: 17, fontWeight: FontWeight.w600),
          ),
          const SizedBox(height: 6),
          const Text(
            '添加你的第一台设备：可从局域网自动发现、手动输入设备 ID，'
            '或对全新设备蓝牙配网。下拉可刷新。',
            textAlign: TextAlign.center,
            style: TextStyle(color: CupertinoColors.systemGrey, fontSize: 13),
          ),
          const SizedBox(height: 20),
          CupertinoButton.filled(
            onPressed: _addDevice,
            child: const Text('添加设备'),
          ),
        ],
      ),
    );
  }

  Widget _errorBanner(String message) {
    return Container(
      margin: const EdgeInsets.fromLTRB(12, 12, 12, 0),
      padding: const EdgeInsets.all(14),
      decoration: BoxDecoration(
        color: CupertinoColors.systemRed.withValues(alpha: 0.1),
        borderRadius: BorderRadius.circular(12),
      ),
      child: Row(
        children: [
          const Icon(
            CupertinoIcons.exclamationmark_triangle,
            color: CupertinoColors.systemRed,
            size: 20,
          ),
          const SizedBox(width: 10),
          Expanded(
            child: Text(
              message,
              style: const TextStyle(
                color: CupertinoColors.systemRed,
                fontSize: 13,
              ),
            ),
          ),
        ],
      ),
    );
  }

  void _showAccountSheet() {
    final session = widget.session;
    showCupertinoModalPopup<void>(
      context: context,
      builder: (sheetContext) => CupertinoActionSheet(
        title: const Text('账户'),
        message: Text(
          '${session.email.isEmpty ? session.userId : session.email}\n'
          '${session.baseUrl}',
        ),
        actions: [
          CupertinoActionSheetAction(
            onPressed: () {
              Navigator.of(sheetContext).pop();
              _addDevice();
            },
            child: const Text('添加设备'),
          ),
          CupertinoActionSheetAction(
            isDestructiveAction: true,
            onPressed: () {
              Navigator.of(sheetContext).pop();
              _logout();
            },
            child: const Text('退出登录'),
          ),
        ],
        cancelButton: CupertinoActionSheetAction(
          onPressed: () => Navigator.of(sheetContext).pop(),
          child: const Text('取消'),
        ),
      ),
    );
  }
}

/// Add-device screen: shows devices auto-discovered on the local MQTT network
/// (that are not yet claimed to this account) for one-tap claim, keeps a manual
/// device-id field, and offers a single BLE provisioning entry for brand-new
/// (unconfigured) hardware. Pops `true` when a device was claimed so the caller
/// refreshes the list.
class _AddDevicePage extends StatefulWidget {
  const _AddDevicePage({
    required this.client,
    required this.mqtt,
    required this.claimedIds,
  });

  final CloudClient client;
  final MqttService mqtt;
  final Set<String> claimedIds;

  @override
  State<_AddDevicePage> createState() => _AddDevicePageState();
}

class _AddDevicePageState extends State<_AddDevicePage> {
  final TextEditingController _manual = TextEditingController();
  StreamSubscription<List<MeshDevice>>? _sub;
  List<MeshDevice> _discovered = const [];
  bool _busy = false;
  String? _error;

  @override
  void initState() {
    super.initState();
    _discovered = _filter(widget.mqtt.devicesSnapshot);
    _sub = widget.mqtt.devices.listen((list) {
      if (!mounted) return;
      setState(() => _discovered = _filter(list));
    });
  }

  List<MeshDevice> _filter(List<MeshDevice> list) =>
      list.where((d) => !widget.claimedIds.contains(d.id)).toList();

  @override
  void dispose() {
    _sub?.cancel();
    _manual.dispose();
    super.dispose();
  }

  Future<void> _claim(String rawId) async {
    final deviceId = rawId.trim();
    if (deviceId.isEmpty || _busy) return;
    setState(() {
      _busy = true;
      _error = null;
    });
    try {
      await widget.client.claimDevice(deviceId);
      if (!mounted) return;
      Navigator.of(context).pop(true);
    } on CloudApiException catch (e) {
      if (!mounted) return;
      if (e.statusCode == 409) {
        setState(() => _busy = false);
        await _confirmForceClaim(deviceId);
        return;
      }
      setState(() {
        _busy = false;
        _error = e.message;
      });
    }
  }

  Future<void> _confirmForceClaim(String deviceId) async {
    final confirmed = await showCupertinoDialog<bool>(
      context: context,
      builder: (ctx) => CupertinoAlertDialog(
        title: const Text('设备已被其它账号认领'),
        content: const Text('如果这台设备在你手里（例如刚重新配网），可以强制转移到当前账号。原账号将失去该设备。'),
        actions: [
          CupertinoDialogAction(
            onPressed: () => Navigator.of(ctx).pop(false),
            child: const Text('取消'),
          ),
          CupertinoDialogAction(
            isDestructiveAction: true,
            onPressed: () => Navigator.of(ctx).pop(true),
            child: const Text('强制认领'),
          ),
        ],
      ),
    );
    if (confirmed != true || !mounted) return;
    setState(() {
      _busy = true;
      _error = null;
    });
    try {
      await widget.client.claimDevice(deviceId, force: true);
      if (!mounted) return;
      Navigator.of(context).pop(true);
    } on CloudApiException catch (e) {
      if (!mounted) return;
      setState(() {
        _busy = false;
        _error = e.message;
      });
    }
  }

  Future<void> _openProvisioning() async {
    await Navigator.of(context).push(
      CupertinoPageRoute<void>(builder: (_) => const ProvisioningPage()),
    );
  }

  @override
  Widget build(BuildContext context) {
    return CupertinoPageScaffold(
      backgroundColor: CupertinoColors.systemGroupedBackground,
      navigationBar: const CupertinoNavigationBar(middle: Text('添加设备')),
      child: SafeArea(
        child: ListView(
          padding: const EdgeInsets.symmetric(vertical: 8),
          children: [
            if (_error != null) _errorBanner(_error!),
            _sectionHeader(
              '自动发现（局域网）',
              trailing: _busy
                  ? const CupertinoActivityIndicator(radius: 8)
                  : null,
            ),
            _discoverySection(),
            _sectionHeader('手动添加'),
            _manualSection(),
            _sectionHeader('全新设备'),
            _provisioningSection(),
          ],
        ),
      ),
    );
  }

  Widget _sectionHeader(String text, {Widget? trailing}) {
    return Padding(
      padding: const EdgeInsets.fromLTRB(16, 18, 16, 6),
      child: Row(
        children: [
          Text(
            text,
            style: const TextStyle(
              fontSize: 13,
              fontWeight: FontWeight.w600,
              color: CupertinoColors.systemGrey,
            ),
          ),
          const Spacer(),
          ?trailing,
        ],
      ),
    );
  }

  Widget _card({required Widget child}) {
    return Container(
      margin: const EdgeInsets.symmetric(horizontal: 12),
      decoration: BoxDecoration(
        color: CupertinoColors.systemBackground,
        borderRadius: BorderRadius.circular(12),
      ),
      child: child,
    );
  }

  Widget _discoverySection() {
    if (_discovered.isEmpty) {
      return _card(
        child: const Padding(
          padding: EdgeInsets.all(16),
          child: Text(
            '未发现局域网内可添加的新设备。请确保设备已上电，并与手机处于同一网络。',
            style: TextStyle(fontSize: 13, color: CupertinoColors.systemGrey),
          ),
        ),
      );
    }
    return _card(
      child: Column(
        children: [
          for (var i = 0; i < _discovered.length; i++) ...[
            if (i > 0)
              Container(
                margin: const EdgeInsets.only(left: 60),
                height: 1,
                color: CupertinoColors.separator,
              ),
            _discoveredRow(_discovered[i]),
          ],
        ],
      ),
    );
  }

  Widget _discoveredRow(MeshDevice device) {
    return Padding(
      padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 8),
      child: Row(
        children: [
          SizedBox(
            width: 40,
            height: 40,
            child: DeviceIcon(
              type: device.type,
              online: device.online,
              on: device.on,
            ),
          ),
          const SizedBox(width: 12),
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Text(
                  device.displayName,
                  style: const TextStyle(
                    fontSize: 15,
                    fontWeight: FontWeight.w500,
                  ),
                ),
                const SizedBox(height: 2),
                Text(
                  '${device.id}  ·  ${device.online ? '在线' : '离线'}',
                  style: const TextStyle(
                    fontSize: 12,
                    color: CupertinoColors.systemGrey,
                  ),
                ),
              ],
            ),
          ),
          CupertinoButton(
            padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 6),
            color: CupertinoColors.activeBlue,
            foregroundColor: CupertinoColors.white,
            borderRadius: BorderRadius.circular(16),
            onPressed: _busy ? null : () => _claim(device.id),
            child: const Text('添加', style: TextStyle(fontSize: 14)),
          ),
        ],
      ),
    );
  }

  Widget _manualSection() {
    return _card(
      child: Padding(
        padding: const EdgeInsets.all(12),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: [
            CupertinoTextField(
              controller: _manual,
              placeholder: '输入设备 ID，如 node-0B55C0 / door-001',
              autocorrect: false,
              enableSuggestions: false,
              onSubmitted: (v) => _claim(v),
            ),
            const SizedBox(height: 6),
            const Text(
              '设备需已上电并上报云端后方可认领。',
              style: TextStyle(fontSize: 12, color: CupertinoColors.systemGrey),
            ),
            const SizedBox(height: 10),
            CupertinoButton.filled(
              onPressed: _busy ? null : () => _claim(_manual.text),
              child: const Text('添加'),
            ),
          ],
        ),
      ),
    );
  }

  Widget _provisioningSection() {
    return _card(
      child: Padding(
        padding: const EdgeInsets.all(12),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: [
            const Text(
              '设备尚未联网？通过蓝牙为全新设备配置 Wi‑Fi。',
              style: TextStyle(fontSize: 12, color: CupertinoColors.systemGrey),
            ),
            const SizedBox(height: 10),
            CupertinoButton(
              color: CupertinoColors.activeBlue,
              foregroundColor: CupertinoColors.white,
              onPressed: _busy ? null : _openProvisioning,
              child: const Text('蓝牙配网'),
            ),
          ],
        ),
      ),
    );
  }

  Widget _errorBanner(String message) {
    return Container(
      margin: const EdgeInsets.fromLTRB(12, 12, 12, 0),
      padding: const EdgeInsets.all(14),
      decoration: BoxDecoration(
        color: CupertinoColors.systemRed.withValues(alpha: 0.1),
        borderRadius: BorderRadius.circular(12),
      ),
      child: Row(
        children: [
          const Icon(
            CupertinoIcons.exclamationmark_triangle,
            color: CupertinoColors.systemRed,
            size: 20,
          ),
          const SizedBox(width: 10),
          Expanded(
            child: Text(
              message,
              style: const TextStyle(
                color: CupertinoColors.systemRed,
                fontSize: 13,
              ),
            ),
          ),
        ],
      ),
    );
  }
}
