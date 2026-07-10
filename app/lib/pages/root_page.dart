import 'dart:async';

import 'package:flutter/cupertino.dart';
import 'package:flutter/services.dart';

import '../services/doorbell_notification_service.dart';
import '../services/mqtt_service.dart';
import 'home_shell.dart';
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
    _callVisible = true;
    Navigator.of(context, rootNavigator: true)
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

  /// Handles the Android system back button / left-swipe while the account
  /// content is the front-most route. Device detail pages and the full-screen
  /// incoming-call overlay are pushed onto the [CupertinoApp] root navigator,
  /// so the system pops those first on its own; this [PopScope] (attached to
  /// the home route) only runs once nothing is left to pop, and then asks to
  /// exit instead of quitting on the first press.
  Future<void> _handleBack() async {
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
    // [AppShell] is the home route of the CupertinoApp root navigator; pushed
    // pages (device detail / incoming-call overlay) go on that same navigator,
    // so the OS back button pops them on its own. This [PopScope] guards only
    // the home route: with nothing left to pop it prompts to exit rather than
    // quitting immediately. Wrapping the content in a nested [Navigator] here
    // would let that inner navigator swallow the back button and quit the app
    // once its stack is empty, which is the regression this avoids.
    return PopScope(
      canPop: false,
      onPopInvokedWithResult: (didPop, _) {
        if (didPop) return;
        _handleBack();
      },
      child: AppShell(mqtt: _mqtt),
    );
  }
}
