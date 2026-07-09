import 'dart:async';

import 'package:flutter/cupertino.dart';

import '../camera_settings.dart';
import '../services/mqtt_service.dart';
import '../widgets/mjpeg_view.dart';

/// Passive live-view screen: full-screen video with a snapshot action and a
/// local/relay source switch. Unlike [CallPage] it has no call affordances
/// (no answer/hang-up), so opening "查看实时画面" no longer looks like a call.
class LiveViewPage extends StatefulWidget {
  const LiveViewPage({super.key, required this.mqtt});

  final MqttService mqtt;

  @override
  State<LiveViewPage> createState() => _LiveViewPageState();
}

class _LiveViewPageState extends State<LiveViewPage> {
  Timer? _keepAlive;

  @override
  void initState() {
    super.initState();
    // The board only pushes frames while a session is active, so request one
    // on open and re-ping so its inactivity timeout doesn't stop the stream.
    widget.mqtt.sendDoorbellCommand('stream');
    _keepAlive = Timer.periodic(const Duration(seconds: 30),
        (_) => widget.mqtt.sendDoorbellCommand('stream'));
  }

  @override
  void dispose() {
    _keepAlive?.cancel();
    // Tell the board to stop pushing when we leave (safe no-op if already idle).
    widget.mqtt.sendDoorbellCommand('hangup');
    super.dispose();
  }

  Future<void> _onModeChanged(BuildContext context, CameraMode m) async {
    if (m == CameraMode.relay) {
      CameraSettings.useRelay();
      widget.mqtt.sendDoorbellCommand('stream');
      return;
    }
    if (CameraSettings.lanHostIsPlaceholder) {
      final host = await _askLanHost(context);
      if (host == null || host.isEmpty) return;
      CameraSettings.useLan(host);
    } else {
      CameraSettings.useLan(CameraSettings.lanHost.value);
    }
  }

  Future<String?> _askLanHost(BuildContext context) {
    final controller =
        TextEditingController(text: CameraSettings.lanHost.value);
    return showCupertinoDialog<String>(
      context: context,
      builder: (ctx) => CupertinoAlertDialog(
        title: const Text('板子局域网 IP'),
        content: Padding(
          padding: const EdgeInsets.only(top: 12),
          child: CupertinoTextField(
            controller: controller,
            placeholder: '192.168.x.x',
            keyboardType: TextInputType.url,
            autocorrect: false,
            autofocus: true,
          ),
        ),
        actions: [
          CupertinoDialogAction(
            onPressed: () => Navigator.of(ctx).pop(),
            child: const Text('取消'),
          ),
          CupertinoDialogAction(
            isDefaultAction: true,
            onPressed: () => Navigator.of(ctx).pop(controller.text.trim()),
            child: const Text('确定'),
          ),
        ],
      ),
    );
  }

  @override
  Widget build(BuildContext context) {
    return CupertinoPageScaffold(
      backgroundColor: CupertinoColors.black,
      child: Stack(
        children: [
          Positioned.fill(
            child: ValueListenableBuilder<String>(
              valueListenable: CameraSettings.streamUrl,
              builder: (_, url, _) => MjpegView(url: url),
            ),
          ),
          SafeArea(
            child: Column(
              children: [
                Padding(
                  padding:
                      const EdgeInsets.symmetric(horizontal: 16, vertical: 8),
                  child: Row(
                    children: [
                      const Text(
                        '实时画面',
                        style: TextStyle(
                          color: CupertinoColors.white,
                          fontSize: 20,
                          fontWeight: FontWeight.w600,
                        ),
                      ),
                      const SizedBox(width: 10),
                      const _LiveBadge(),
                      const Spacer(),
                      CupertinoButton(
                        padding: EdgeInsets.zero,
                        onPressed: () => Navigator.of(context).pop(),
                        child: const Icon(
                          CupertinoIcons.xmark_circle_fill,
                          color: CupertinoColors.white,
                          size: 30,
                        ),
                      ),
                    ],
                  ),
                ),
                Padding(
                  padding: const EdgeInsets.symmetric(horizontal: 16),
                  child: ValueListenableBuilder<CameraMode>(
                    valueListenable: CameraSettings.mode,
                    builder: (context, m, _) => SizedBox(
                      width: double.infinity,
                      child: CupertinoSlidingSegmentedControl<CameraMode>(
                        backgroundColor:
                            CupertinoColors.white.withValues(alpha: 0.18),
                        thumbColor: CupertinoColors.activeBlue,
                        groupValue: m,
                        onValueChanged: (v) {
                          if (v != null) _onModeChanged(context, v);
                        },
                        children: const {
                          CameraMode.local: Padding(
                            padding: EdgeInsets.symmetric(vertical: 6),
                            child: Text('本地(局域网)',
                                style: TextStyle(color: CupertinoColors.white)),
                          ),
                          CameraMode.relay: Padding(
                            padding: EdgeInsets.symmetric(vertical: 6),
                            child: Text('中继(远程)',
                                style: TextStyle(color: CupertinoColors.white)),
                          ),
                        },
                      ),
                    ),
                  ),
                ),
                const Spacer(),
                Padding(
                  padding: const EdgeInsets.only(bottom: 40),
                  child: _CircleButton(
                    icon: CupertinoIcons.camera,
                    color: CupertinoColors.systemGrey,
                    label: '快照',
                    onPressed: () =>
                        widget.mqtt.sendDoorbellCommand('snapshot'),
                  ),
                ),
              ],
            ),
          ),
        ],
      ),
    );
  }
}

class _LiveBadge extends StatelessWidget {
  const _LiveBadge();

  @override
  Widget build(BuildContext context) {
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 4),
      decoration: BoxDecoration(
        color: CupertinoColors.destructiveRed,
        borderRadius: BorderRadius.circular(12),
      ),
      child: const Text(
        'LIVE',
        style: TextStyle(
          color: CupertinoColors.white,
          fontSize: 12,
          fontWeight: FontWeight.bold,
        ),
      ),
    );
  }
}

class _CircleButton extends StatelessWidget {
  const _CircleButton({
    required this.icon,
    required this.color,
    required this.label,
    required this.onPressed,
  });

  final IconData icon;
  final Color color;
  final String label;
  final VoidCallback onPressed;

  @override
  Widget build(BuildContext context) {
    return Column(
      mainAxisSize: MainAxisSize.min,
      children: [
        GestureDetector(
          onTap: onPressed,
          child: Container(
            width: 68,
            height: 68,
            decoration: BoxDecoration(color: color, shape: BoxShape.circle),
            child: Icon(icon, color: CupertinoColors.white, size: 30),
          ),
        ),
        const SizedBox(height: 8),
        Text(label,
            style:
                const TextStyle(color: CupertinoColors.white, fontSize: 13)),
      ],
    );
  }
}
