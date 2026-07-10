import 'dart:async';

import 'package:flutter/cupertino.dart';

import '../services/doorbell_notification_service.dart';
import '../services/mqtt_service.dart';
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
          CupertinoPageRoute<void>(builder: (_) => HomePage(mqtt: _mqtt)),
    );
  }
}
