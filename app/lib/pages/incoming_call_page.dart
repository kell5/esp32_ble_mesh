import 'package:flutter/cupertino.dart';

import '../services/mqtt_service.dart';
import 'call_page.dart';

/// iOS-style full-screen incoming call presented when a "ringing" event fires.
class IncomingCallPage extends StatelessWidget {
  const IncomingCallPage({super.key, required this.mqtt});

  final MqttService mqtt;

  void _accept(BuildContext context) {
    Navigator.of(context).pushReplacement(
      CupertinoPageRoute<void>(
        builder: (_) => CallPage(mqtt: mqtt, title: '门铃通话'),
      ),
    );
  }

  void _decline(BuildContext context) {
    mqtt.sendDoorbellCommand('hangup');
    Navigator.of(context).pop();
  }

  @override
  Widget build(BuildContext context) {
    return CupertinoPageScaffold(
      backgroundColor: const Color(0xFF1C1C1E),
      child: SafeArea(
        child: Column(
          children: [
            const SizedBox(height: 80),
            Container(
              width: 120,
              height: 120,
              decoration: const BoxDecoration(
                color: CupertinoColors.systemGrey,
                shape: BoxShape.circle,
              ),
              child: const Icon(CupertinoIcons.bell_fill,
                  color: CupertinoColors.white, size: 56),
            ),
            const SizedBox(height: 24),
            const Text(
              '门铃呼叫',
              style: TextStyle(
                  color: CupertinoColors.white,
                  fontSize: 30,
                  fontWeight: FontWeight.w600),
            ),
            const SizedBox(height: 8),
            const Text(
              '有人按响门铃…',
              style: TextStyle(color: CupertinoColors.systemGrey2, fontSize: 16),
            ),
            const Spacer(),
            Padding(
              padding: const EdgeInsets.only(bottom: 48),
              child: Row(
                mainAxisAlignment: MainAxisAlignment.spaceEvenly,
                children: [
                  _CallAction(
                    icon: CupertinoIcons.phone_down_fill,
                    color: CupertinoColors.destructiveRed,
                    label: '拒绝',
                    onPressed: () => _decline(context),
                  ),
                  _CallAction(
                    icon: CupertinoIcons.videocam_fill,
                    color: CupertinoColors.activeGreen,
                    label: '接听',
                    onPressed: () => _accept(context),
                  ),
                ],
              ),
            ),
          ],
        ),
      ),
    );
  }
}

class _CallAction extends StatelessWidget {
  const _CallAction({
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
            width: 76,
            height: 76,
            decoration: BoxDecoration(color: color, shape: BoxShape.circle),
            child: Icon(icon, color: CupertinoColors.white, size: 34),
          ),
        ),
        const SizedBox(height: 10),
        Text(label,
            style: const TextStyle(color: CupertinoColors.white, fontSize: 15)),
      ],
    );
  }
}
