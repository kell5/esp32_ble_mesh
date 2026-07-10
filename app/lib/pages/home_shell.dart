import 'package:flutter/cupertino.dart';

import '../services/cloud_session.dart';
import '../services/mqtt_service.dart';
import 'cloud_devices_page.dart';
import 'cloud_login_page.dart';

/// Account-first root of the in-app content. Until a cloud session exists it
/// shows the email login/register screen as the application root; once logged
/// in it shows the single unified device list (data source = the account).
///
/// [mqtt] is owned by [RootPage] and kept alive purely as a transport layer
/// (transparent local-network acceleration + doorbell signalling), not as a
/// separate tab.
class AppShell extends StatefulWidget {
  const AppShell({super.key, required this.mqtt});

  final MqttService mqtt;

  @override
  State<AppShell> createState() => _AppShellState();
}

class _AppShellState extends State<AppShell> {
  CloudSession? _session;

  @override
  void initState() {
    super.initState();
    _load();
  }

  Future<void> _load() async {
    final session = await CloudSession.load();
    if (!mounted) return;
    setState(() => _session = session);
  }

  @override
  Widget build(BuildContext context) {
    final session = _session;
    if (session == null) {
      return const CupertinoPageScaffold(
        child: Center(child: CupertinoActivityIndicator()),
      );
    }
    if (!session.isLoggedIn) {
      return CloudLoginPage(session: session, onAuthenticated: _load);
    }
    return CloudDevicesPage(
      mqtt: widget.mqtt,
      session: session,
      onLogout: _load,
    );
  }
}
