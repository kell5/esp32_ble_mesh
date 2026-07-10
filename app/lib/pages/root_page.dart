import 'dart:async';

import 'package:flutter/cupertino.dart';
import 'package:flutter/services.dart';

import '../services/doorbell_notification_service.dart';
import '../services/mqtt_service.dart';
import 'cloud_devices_page.dart';
import 'home_page.dart';
import 'incoming_call_page.dart';

/// Owns the shared [MqttService], connects on startup, and presents the
/// incoming-call screen when a "ringing" event arrives.
class RootPage extends StatefulWidget {
  const RootPage({super.key});

  @override
  State<RootPage> createState() => _RootPageState();
}

class _RootPageState extends State<RootPage> with WidgetsBindingObserver {
  final MqttService _mqtt = MqttService();
  final GlobalKey<NavigatorState> _navKey = GlobalKey<NavigatorState>();
  StreamSubscription<DoorbellEvent>? _eventSub;
  StreamSubscription<String>? _notificationSub;
  AppLifecycleState _lifecycleState = AppLifecycleState.resumed;
  bool _callVisible = false;

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addObserver(this);
    _eventSub = _mqtt.doorbellEvents.listen(_onEvent);
    _initializeNotifications();
    _connect();
  }

  Future<void> _connect() async {
    try {
      await _mqtt.connect();
    } catch (_) {
      // autoReconnect will keep retrying; UI shows the disconnected state.
    }
  }

  Future<void> _initializeNotifications() async {
    final notifications = DoorbellNotificationService.instance;
    final launchPayload = await notifications.initialize();
    _notificationSub = notifications.taps.listen(_onNotificationTap);
    if (launchPayload != null) _onNotificationTap(launchPayload);
  }

  void _onNotificationTap(String payload) {
    if (payload != DoorbellNotificationService.ringingPayload) return;
    WidgetsBinding.instance.addPostFrameCallback((_) => _showIncomingCall());
  }

  @override
  void didChangeAppLifecycleState(AppLifecycleState state) {
    _lifecycleState = state;
  }

  Future<void> _onEvent(DoorbellEvent event) async {
    if (event != DoorbellEvent.ringing || _callVisible) return;
    if (_lifecycleState == AppLifecycleState.resumed) {
      _showIncomingCall();
    } else {
      await DoorbellNotificationService.instance.showRinging();
    }
  }

  void _showIncomingCall() {
    if (_callVisible || !mounted) return;
    final navigator = _navKey.currentState;
    if (navigator == null) return;
    _callVisible = true;
    navigator
        .push(
          CupertinoPageRoute<void>(
            fullscreenDialog: true,
            builder: (_) => IncomingCallPage(mqtt: _mqtt),
          ),
        )
        .then((_) => _callVisible = false);
  }

  @override
  void dispose() {
    WidgetsBinding.instance.removeObserver(this);
    _eventSub?.cancel();
    _notificationSub?.cancel();
    _mqtt.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    // Nested navigator lets the incoming-call screen overlay the tab bar.
    return Navigator(
      key: _navKey,
      onGenerateRoute: (_) =>
          CupertinoPageRoute<void>(builder: (_) => _MainTabs(mqtt: _mqtt)),
    );
  }
}

/// Bottom tab bar: the local MQTT device home and the cloud device registry.
///
/// The Android system back button is handled explicitly so it never quits the
/// app on the first press: it first pops any pushed page inside the active
/// tab, then falls back to the first tab, and only then asks to exit.
class _MainTabs extends StatefulWidget {
  const _MainTabs({required this.mqtt});

  final MqttService mqtt;

  @override
  State<_MainTabs> createState() => _MainTabsState();
}

class _MainTabsState extends State<_MainTabs> {
  final CupertinoTabController _tab = CupertinoTabController();
  final List<GlobalKey<NavigatorState>> _navKeys = [
    GlobalKey<NavigatorState>(),
    GlobalKey<NavigatorState>(),
  ];

  @override
  void dispose() {
    _tab.dispose();
    super.dispose();
  }

  Future<void> _handleBack() async {
    final navigator = _navKeys[_tab.index].currentState;
    if (navigator != null && navigator.canPop()) {
      navigator.pop();
      return;
    }
    if (_tab.index != 0) {
      setState(() => _tab.index = 0);
      return;
    }
    final shouldExit = await _confirmExit();
    if (shouldExit) await SystemNavigator.pop();
  }

  Future<bool> _confirmExit() async {
    final result = await showCupertinoDialog<bool>(
      context: context,
      builder: (dialogContext) => CupertinoAlertDialog(
        title: const Text('退出应用'),
        content: const Text('确定要退出吗？'),
        actions: [
          CupertinoDialogAction(
            onPressed: () => Navigator.of(dialogContext).pop(false),
            child: const Text('取消'),
          ),
          CupertinoDialogAction(
            isDestructiveAction: true,
            onPressed: () => Navigator.of(dialogContext).pop(true),
            child: const Text('退出'),
          ),
        ],
      ),
    );
    return result ?? false;
  }

  @override
  Widget build(BuildContext context) {
    return PopScope(
      canPop: false,
      onPopInvokedWithResult: (didPop, _) {
        if (didPop) return;
        _handleBack();
      },
      child: CupertinoTabScaffold(
        controller: _tab,
        tabBar: CupertinoTabBar(
          items: const [
            BottomNavigationBarItem(
              icon: Icon(CupertinoIcons.house_fill),
              label: '我的设备',
            ),
            BottomNavigationBarItem(
              icon: Icon(CupertinoIcons.cloud_fill),
              label: '云端',
            ),
          ],
        ),
        tabBuilder: (context, index) {
          return CupertinoTabView(
            navigatorKey: _navKeys[index],
            builder: (_) => index == 0
                ? HomePage(mqtt: widget.mqtt)
                : const CloudDevicesPage(),
          );
        },
      ),
    );
  }
}
