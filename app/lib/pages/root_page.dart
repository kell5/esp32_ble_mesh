import 'dart:async';

import 'package:flutter/cupertino.dart';

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

class _RootPageState extends State<RootPage> {
  final MqttService _mqtt = MqttService();
  final GlobalKey<NavigatorState> _navKey = GlobalKey<NavigatorState>();
  StreamSubscription<DoorbellEvent>? _eventSub;
  bool _callVisible = false;

  @override
  void initState() {
    super.initState();
    _eventSub = _mqtt.doorbellEvents.listen(_onEvent);
    _connect();
  }

  Future<void> _connect() async {
    try {
      await _mqtt.connect();
    } catch (_) {
      // autoReconnect will keep retrying; UI shows the disconnected state.
    }
  }

  void _onEvent(DoorbellEvent event) {
    if (event == DoorbellEvent.ringing && !_callVisible) {
      _callVisible = true;
      _navKey.currentState
          ?.push(
            CupertinoPageRoute<void>(
              fullscreenDialog: true,
              builder: (_) => IncomingCallPage(mqtt: _mqtt),
            ),
          )
          .then((_) => _callVisible = false);
    }
  }

  @override
  void dispose() {
    _eventSub?.cancel();
    _mqtt.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    // Nested navigator lets the incoming-call screen overlay the tab bar.
    return Navigator(
      key: _navKey,
      onGenerateRoute: (_) => CupertinoPageRoute<void>(
        builder: (_) => HomePage(mqtt: _mqtt),
      ),
    );
  }
}
