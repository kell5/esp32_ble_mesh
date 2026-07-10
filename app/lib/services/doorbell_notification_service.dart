import 'dart:async';

import 'package:flutter_local_notifications/flutter_local_notifications.dart';

class DoorbellNotificationService {
  DoorbellNotificationService._();

  static final DoorbellNotificationService instance =
      DoorbellNotificationService._();

  static const String ringingPayload = 'doorbell:ringing';

  final FlutterLocalNotificationsPlugin _plugin =
      FlutterLocalNotificationsPlugin();
  final StreamController<String> _taps = StreamController<String>.broadcast();
  bool _initialized = false;

  Stream<String> get taps => _taps.stream;

  Future<String?> initialize() async {
    if (_initialized) return null;

    const settings = InitializationSettings(
      android: AndroidInitializationSettings('@mipmap/ic_launcher'),
      iOS: DarwinInitializationSettings(),
    );
    await _plugin.initialize(
      settings,
      onDidReceiveNotificationResponse: (response) {
        final payload = response.payload;
        if (payload != null) _taps.add(payload);
      },
    );

    await _plugin
        .resolvePlatformSpecificImplementation<
          AndroidFlutterLocalNotificationsPlugin
        >()
        ?.requestNotificationsPermission();
    await _plugin
        .resolvePlatformSpecificImplementation<
          IOSFlutterLocalNotificationsPlugin
        >()
        ?.requestPermissions(alert: true, badge: true, sound: true);

    _initialized = true;
    final launchDetails = await _plugin.getNotificationAppLaunchDetails();
    return launchDetails?.didNotificationLaunchApp == true
        ? launchDetails?.notificationResponse?.payload
        : null;
  }

  Future<void> showRinging() async {
    const details = NotificationDetails(
      android: AndroidNotificationDetails(
        'doorbell_calls',
        '门铃提醒',
        channelDescription: '有人按门铃时显示提醒',
        importance: Importance.max,
        priority: Priority.high,
        category: AndroidNotificationCategory.call,
        visibility: NotificationVisibility.public,
        playSound: true,
      ),
      iOS: DarwinNotificationDetails(
        presentAlert: true,
        presentBadge: true,
        presentSound: true,
      ),
    );
    await _plugin.show(
      1001,
      '有人按门铃',
      '点击查看门口实时画面',
      details,
      payload: ringingPayload,
    );
  }
}
