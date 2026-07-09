import 'package:flutter/foundation.dart';

import 'config.dart';

/// Live-view source mode: local direct (A1, `http://<板子IP>:81/stream`) or
/// server relay (A2, `http://<relay>:8090/stream/<id>`).
enum CameraMode { local, relay }

/// Runtime-editable live-view settings. Holds the current [streamUrl] plus the
/// selected [mode] and the remembered LAN host, so the view can be switched
/// between local and relay without recompiling or editing the full URL.
class CameraSettings {
  /// Last-used board LAN IP for local mode (user-editable).
  static final ValueNotifier<String> lanHost =
      ValueNotifier<String>('192.168.1.100');

  /// Currently selected mode (drives the segmented control on the live view).
  static final ValueNotifier<CameraMode> mode = ValueNotifier<CameraMode>(
    AppConfig.defaultStreamUrl == AppConfig.relayStreamUrl
        ? CameraMode.relay
        : CameraMode.local,
  );

  /// Full MJPEG URL the player consumes.
  static final ValueNotifier<String> streamUrl =
      ValueNotifier<String>(AppConfig.defaultStreamUrl);

  static void useRelay() {
    mode.value = CameraMode.relay;
    streamUrl.value = AppConfig.relayStreamUrl;
  }

  static void useLan(String host) {
    lanHost.value = host;
    mode.value = CameraMode.local;
    streamUrl.value = AppConfig.cameraStreamUrl(host);
  }

  /// True when [lanHost] is still the placeholder and needs the real board IP.
  static bool get lanHostIsPlaceholder => lanHost.value == '192.168.1.100';
}
