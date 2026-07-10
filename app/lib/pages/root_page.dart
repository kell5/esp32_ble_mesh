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
  final CupertinoTabController _tab = CupertinoTabController();
  final List<GlobalKey<NavigatorState>> _tabNavKeys = [
    GlobalKey<NavigatorState>(),
    GlobalKey<NavigatorState>(),
  ];
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
    _tab.dispose();
    _mqtt.dispose();
    super.dispose();
  }

  /// Handles the Android system back button / left-swipe. This [PopScope] lives
  /// at the [CupertinoApp] root navigator level (the only place that actually
  /// receives the system pop), so it never quits the app on the first press:
  /// it closes an overlay page, then a page pushed inside the active tab, then
  /// falls back to the first tab, and only then asks to exit.
  Future<void> _handleBack() async {
    final overlayNavigator = _navKey.currentState;
    if (overlayNavigator != null && overlayNavigator.canPop()) {
      overlayNavigator.pop();
      return;
    }
    final tabNavigator = _tabNavKeys[_tab.index].currentState;
    if (tabNavigator != null && tabNavigator.canPop()) {
      tabNavigator.pop();
      return;
    }
    if (_tab.index != 0) {
      _tab.index = 0;
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
    // The PopScope must sit above the nested navigator so it registers with the
    // CupertinoApp root navigator, which is what the OS back button targets.
    return PopScope(
      canPop: false,
      onPopInvokedWithResult: (didPop, _) {
        if (didPop) return;
        _handleBack();
      },
      // Nested navigator lets the incoming-call screen overlay the tab bar.
      child: Navigator(
        key: _navKey,
        onGenerateRoute: (_) => CupertinoPageRoute<void>(
          builder: (_) =>
              _MainTabs(mqtt: _mqtt, tab: _tab, navKeys: _tabNavKeys),
        ),
      ),
    );
  }
}

/// Bottom tab bar: the local MQTT device home and the cloud device registry.
///
/// The tab controller and per-tab navigator keys are owned by [_RootPageState]
/// so the root-level [PopScope] can drive the back-button behaviour.
class _MainTabs extends StatelessWidget {
  const _MainTabs({
    required this.mqtt,
    required this.tab,
    required this.navKeys,
  });

  final MqttService mqtt;
  final CupertinoTabController tab;
  final List<GlobalKey<NavigatorState>> navKeys;

  @override
  Widget build(BuildContext context) {
    return CupertinoTabScaffold(
      controller: tab,
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
          navigatorKey: navKeys[index],
          builder: (_) =>
              index == 0 ? HomePage(mqtt: mqtt) : const CloudDevicesPage(),
        );
      },
    );
  }
}
