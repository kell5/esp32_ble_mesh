import 'dart:io';

import 'package:flutter/services.dart';

/// Binds the app's network traffic to the current Wi-Fi network on Android.
///
/// The gateway SoftAP has no internet, so Android keeps mobile data as the
/// default route and provisioning requests to 192.168.4.1 never reach the
/// device. Binding the process to the Wi-Fi network fixes the routing.
class WifiBind {
  static const _channel = MethodChannel('farmely/wifi_bind');

  /// Returns true when the process is bound to a Wi-Fi network.
  static Future<bool> bind() async {
    if (!Platform.isAndroid) return true;
    try {
      return await _channel.invokeMethod<bool>('bindWifi') ?? false;
    } on PlatformException {
      return false;
    }
  }

  static Future<void> unbind() async {
    if (!Platform.isAndroid) return;
    try {
      await _channel.invokeMethod<void>('unbindWifi');
    } on PlatformException {
      // Best effort.
    }
  }
}
