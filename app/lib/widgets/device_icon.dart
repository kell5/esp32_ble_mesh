import 'package:flutter/cupertino.dart';

import '../services/mqtt_service.dart';

class DeviceIcon extends StatelessWidget {
  const DeviceIcon({
    super.key,
    required this.type,
    required this.online,
    required this.on,
    this.size = 56,
    this.borderRadius = 14,
  });

  final DeviceType type;
  final bool online;
  final bool on;
  final double size;
  final double borderRadius;

  @override
  Widget build(BuildContext context) {
    final asset = _assetPath;
    if (asset == null) return _fallback;

    return ClipRRect(
      borderRadius: BorderRadius.circular(borderRadius),
      child: Image.asset(
        asset,
        width: size,
        height: size,
        fit: BoxFit.cover,
        filterQuality: FilterQuality.medium,
        errorBuilder: (_, _, _) => _fallback,
      ),
    );
  }

  String? get _assetPath {
    final key = switch (type) {
      DeviceType.gateway => 'gateway',
      DeviceType.lightBulb => 'light_bulb',
      DeviceType.ceilingLight => 'ceiling_light',
      DeviceType.lightStrip => 'light_strip',
      DeviceType.wallSwitch => 'wall_switch',
      DeviceType.socket => 'socket',
      DeviceType.curtainMotor => 'curtain_motor',
      DeviceType.valve => 'valve',
      DeviceType.doorLock => 'door_lock',
      _ => null,
    };
    if (key == null) return null;

    final state = !online
        ? (type == DeviceType.valve ? 'off' : 'offline')
        : (on ? 'on' : 'off');
    return 'assets/device_icons/${key}_$state.webp';
  }

  Widget get _fallback {
    final color = !online
        ? CupertinoColors.systemGrey3
        : (on ? CupertinoColors.activeBlue : CupertinoColors.systemGrey);
    return Container(
      width: size,
      height: size,
      decoration: BoxDecoration(
        color: CupertinoColors.systemGrey6,
        borderRadius: BorderRadius.circular(borderRadius),
      ),
      alignment: Alignment.center,
      child: Icon(_fallbackIcon, size: size * 0.5, color: color),
    );
  }

  IconData get _fallbackIcon => switch (type) {
    DeviceType.doorbell => CupertinoIcons.bell_fill,
    DeviceType.camera => CupertinoIcons.video_camera_solid,
    DeviceType.gateway => CupertinoIcons.antenna_radiowaves_left_right,
    DeviceType.lightBulb || DeviceType.ceilingLight =>
      on ? CupertinoIcons.lightbulb_fill : CupertinoIcons.lightbulb,
    DeviceType.lightStrip => CupertinoIcons.light_max,
    DeviceType.wallSwitch ||
    DeviceType.socket ||
    DeviceType.relay => CupertinoIcons.power,
    DeviceType.curtainMotor => CupertinoIcons.rectangle_split_3x1,
    DeviceType.valve => CupertinoIcons.drop_fill,
    DeviceType.doorLock =>
      on ? CupertinoIcons.lock_fill : CupertinoIcons.lock_open_fill,
    DeviceType.sensor => CupertinoIcons.thermometer,
    DeviceType.unknown => CupertinoIcons.cube_box,
  };
}
