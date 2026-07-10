import 'package:flutter/cupertino.dart';

/// Reusable smart-home UI pieces shared by the device home grid and the
/// per-device detail pages (Huawei "智慧生活" style).

/// A single device card: icon + name + status, with a trailing widget that is
/// either a round power button (controllable devices), a reading (sensors) or a
/// chevron (navigation-only devices such as the doorbell). Tapping the card
/// body invokes [onTap]; the trailing power button has its own handler so a
/// quick toggle doesn't open the detail page.
class SmartCard extends StatelessWidget {
  const SmartCard({
    super.key,
    required this.leading,
    required this.title,
    required this.subtitle,
    this.online = true,
    this.trailing,
    this.onTap,
    this.onLongPress,
  });

  final Widget leading;
  final String title;
  final String subtitle;
  final bool online;
  final Widget? trailing;
  final VoidCallback? onTap;
  final VoidCallback? onLongPress;

  @override
  Widget build(BuildContext context) {
    return Opacity(
      opacity: online ? 1 : 0.55,
      child: GestureDetector(
        onTap: onTap,
        onLongPress: onLongPress,
        behavior: HitTestBehavior.opaque,
        child: Container(
          padding: const EdgeInsets.all(14),
          decoration: BoxDecoration(
            color: CupertinoColors.systemBackground,
            borderRadius: BorderRadius.circular(16),
          ),
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              leading,
              const Spacer(),
              Text(
                title,
                maxLines: 1,
                overflow: TextOverflow.ellipsis,
                style: const TextStyle(
                  fontSize: 15,
                  fontWeight: FontWeight.w600,
                ),
              ),
              const SizedBox(height: 2),
              Row(
                children: [
                  Expanded(
                    child: Text(
                      subtitle,
                      maxLines: 1,
                      overflow: TextOverflow.ellipsis,
                      style: const TextStyle(
                        color: CupertinoColors.systemGrey,
                        fontSize: 12,
                      ),
                    ),
                  ),
                  ?trailing,
                ],
              ),
            ],
          ),
        ),
      ),
    );
  }
}

/// Round on/off button shown on controllable device cards.
class PowerButton extends StatelessWidget {
  const PowerButton({
    super.key,
    required this.on,
    required this.enabled,
    required this.onPressed,
    this.size = 40,
  });

  final bool on;
  final bool enabled;
  final VoidCallback onPressed;
  final double size;

  @override
  Widget build(BuildContext context) {
    return GestureDetector(
      onTap: enabled ? onPressed : null,
      child: Container(
        width: size,
        height: size,
        decoration: BoxDecoration(
          shape: BoxShape.circle,
          color: on ? CupertinoColors.activeBlue : CupertinoColors.systemGrey5,
        ),
        child: Icon(
          CupertinoIcons.power,
          size: size * 0.5,
          color: on ? CupertinoColors.white : CupertinoColors.systemGrey,
        ),
      ),
    );
  }
}

/// Small pill button used by group controls.
class PillButton extends StatelessWidget {
  const PillButton({
    super.key,
    required this.label,
    required this.filled,
    required this.onPressed,
  });

  final String label;
  final bool filled;
  final VoidCallback onPressed;

  @override
  Widget build(BuildContext context) {
    return GestureDetector(
      onTap: onPressed,
      child: Container(
        padding: const EdgeInsets.symmetric(horizontal: 18, vertical: 8),
        decoration: BoxDecoration(
          color: filled
              ? CupertinoColors.activeBlue
              : CupertinoColors.systemGrey5,
          borderRadius: BorderRadius.circular(20),
        ),
        child: Text(
          label,
          style: TextStyle(
            fontSize: 14,
            fontWeight: FontWeight.w600,
            color: filled ? CupertinoColors.white : CupertinoColors.label,
          ),
        ),
      ),
    );
  }
}

/// "X 分钟前" style relative timestamp.
String agoLabel(DateTime t) {
  final s = DateTime.now().difference(t).inSeconds;
  if (s < 5) return '刚刚';
  if (s < 60) return '$s 秒前';
  final m = s ~/ 60;
  if (m < 60) return '$m 分钟前';
  final h = m ~/ 60;
  return '$h 小时前';
}
