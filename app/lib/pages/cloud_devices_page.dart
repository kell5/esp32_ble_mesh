import 'package:flutter/cupertino.dart';

import '../services/cloud_client.dart';
import '../services/cloud_session.dart';
import '../widgets/device_icon.dart';
import '../widgets/smart_cards.dart';
import 'cloud_device_detail_page.dart';
import 'cloud_login_page.dart';

/// Cloud tab root. When not logged in it shows a login prompt; once a session
/// exists it lists the user's cloud devices with their shadow state and lets
/// controllable devices be toggled via the desired shadow.
class CloudDevicesPage extends StatefulWidget {
  const CloudDevicesPage({super.key});

  @override
  State<CloudDevicesPage> createState() => _CloudDevicesPageState();
}

class _CloudDevicesPageState extends State<CloudDevicesPage> {
  CloudSession? _session;
  CloudClient? _client;
  List<CloudDeviceView> _devices = const [];
  bool _loading = true;
  String? _error;

  @override
  void initState() {
    super.initState();
    _bootstrap();
  }

  @override
  void dispose() {
    _client?.close();
    super.dispose();
  }

  Future<void> _bootstrap() async {
    final session = await CloudSession.load();
    if (!mounted) return;
    setState(() => _session = session);
    if (session.isLoggedIn) {
      _rebuildClient(session);
      await _refresh();
    } else {
      setState(() => _loading = false);
    }
  }

  void _rebuildClient(CloudSession session) {
    _client?.close();
    _client = CloudClient(baseUrl: session.baseUrl, token: session.token);
  }

  Future<void> _refresh() async {
    final session = _session;
    final client = _client;
    if (session == null || client == null || !session.isLoggedIn) return;
    setState(() {
      _loading = true;
      _error = null;
    });
    try {
      final views = await client.listDeviceViews(session.userId);
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

  Future<void> _login() async {
    final session = _session ?? CloudSession();
    final ok = await Navigator.of(context).push<bool>(
      CupertinoPageRoute<bool>(
        builder: (_) => CloudLoginPage(session: session),
      ),
    );
    if (ok == true && mounted) {
      setState(() => _session = session);
      _rebuildClient(session);
      await _refresh();
    }
  }

  Future<void> _logout() async {
    await _session?.clear();
    _client?.close();
    if (!mounted) return;
    setState(() {
      _client = null;
      _devices = const [];
      _error = null;
    });
  }

  Future<void> _toggle(CloudDeviceView view) async {
    final client = _client;
    if (client == null) return;
    final target = !view.on;
    try {
      await client.setDesired(
        view.device.deviceId,
        {'on': target},
        messageId: 'app-${DateTime.now().millisecondsSinceEpoch}',
      );
      await _refresh();
    } on CloudApiException catch (e) {
      if (!mounted) return;
      setState(() => _error = e.message);
    }
  }

  void _openDetail(CloudDeviceView view) {
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
    final loggedIn = _session?.isLoggedIn ?? false;
    return CupertinoPageScaffold(
      backgroundColor: CupertinoColors.systemGroupedBackground,
      navigationBar: CupertinoNavigationBar(
        middle: const Text('云端设备'),
        trailing: loggedIn
            ? CupertinoButton(
                padding: EdgeInsets.zero,
                onPressed: _showAccountSheet,
                child: const Icon(CupertinoIcons.person_crop_circle),
              )
            : null,
      ),
      child: SafeArea(child: _body(loggedIn)),
    );
  }

  Widget _body(bool loggedIn) {
    if (!loggedIn) return _loginPrompt();
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
            online: view.online,
            on: view.on,
          ),
          title: view.device.displayName,
          subtitle: _subtitle(view),
          online: view.online,
          trailing: view.isControllable
              ? PowerButton(
                  on: view.online && view.on,
                  enabled: view.online,
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
        ),
    ];
  }

  String _subtitle(CloudDeviceView view) {
    final type = view.device.deviceType;
    return type.stateLabel(online: view.online, on: view.on);
  }

  Widget _loginPrompt() {
    return Center(
      child: Padding(
        padding: const EdgeInsets.all(32),
        child: Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            const Icon(
              CupertinoIcons.cloud,
              size: 64,
              color: CupertinoColors.systemGrey2,
            ),
            const SizedBox(height: 16),
            const Text(
              '登录云端以查看你名下的设备',
              style: TextStyle(fontSize: 16, fontWeight: FontWeight.w600),
            ),
            const SizedBox(height: 8),
            const Text(
              '云端设备注册表提供统一的在线状态与期望/上报影子，'
              '可远程查看并控制已认领的设备。',
              textAlign: TextAlign.center,
              style: TextStyle(color: CupertinoColors.systemGrey, fontSize: 13),
            ),
            const SizedBox(height: 20),
            CupertinoButton.filled(onPressed: _login, child: const Text('登录云端')),
          ],
        ),
      ),
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
            '该用户名下暂无云端设备',
            style: TextStyle(fontSize: 15, fontWeight: FontWeight.w600),
          ),
          const SizedBox(height: 6),
          const Text(
            '设备上报到云端并被认领后会出现在这里。下拉可刷新。',
            textAlign: TextAlign.center,
            style: TextStyle(color: CupertinoColors.systemGrey, fontSize: 12),
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
    final session = _session;
    showCupertinoModalPopup<void>(
      context: context,
      builder: (sheetContext) => CupertinoActionSheet(
        title: const Text('云端账户'),
        message: session == null
            ? null
            : Text('${session.userId} @ ${session.baseUrl}'),
        actions: [
          CupertinoActionSheetAction(
            onPressed: () {
              Navigator.of(sheetContext).pop();
              _login();
            },
            child: const Text('切换 / 修改连接'),
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
