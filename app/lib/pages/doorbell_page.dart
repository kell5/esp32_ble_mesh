import 'package:flutter/cupertino.dart';

import '../camera_settings.dart';
import '../config.dart';
import '../services/mqtt_service.dart';
import 'live_view_page.dart';
import 'provisioning_page.dart';

/// Doorbell home tab: connection status, last event, and a manual live-view.
class DoorbellPage extends StatefulWidget {
  const DoorbellPage({super.key, required this.mqtt});

  final MqttService mqtt;

  @override
  State<DoorbellPage> createState() => _DoorbellPageState();
}

class _DoorbellPageState extends State<DoorbellPage> {
  String _lastEvent = '—';

  @override
  void initState() {
    super.initState();
    widget.mqtt.doorbellEvents.listen((e) {
      if (!mounted) return;
      setState(() => _lastEvent = _eventLabel(e));
    });
  }

  String _eventLabel(DoorbellEvent e) {
    switch (e) {
      case DoorbellEvent.ringing:
        return '门铃响起';
      case DoorbellEvent.streamStart:
        return '推流开始';
      case DoorbellEvent.streamStop:
        return '推流结束';
      case DoorbellEvent.unknown:
        return '未知事件';
    }
  }

  void _openLiveView() {
    Navigator.of(context).push(
      CupertinoPageRoute<void>(
        builder: (_) => LiveViewPage(mqtt: widget.mqtt),
      ),
    );
  }

  void _openProvisioning() {
    Navigator.of(context).push(
      CupertinoPageRoute<void>(builder: (_) => const ProvisioningPage()),
    );
  }

  void _showProvisioningOptions() {
    showCupertinoModalPopup<void>(
      context: context,
      builder: (ctx) => CupertinoActionSheet(
        title: const Text('设备配网'),
        message: const Text('首次上电用“手机配网”；已联网设备换 WiFi 可用“让设备重新配网”。'),
        actions: [
          CupertinoActionSheetAction(
            onPressed: () {
              Navigator.of(ctx).pop();
              _openProvisioning();
            },
            child: const Text('手机配网（连热点下发 WiFi）'),
          ),
          CupertinoActionSheetAction(
            onPressed: () {
              Navigator.of(ctx).pop();
              _confirmReprovision();
            },
            child: const Text('让设备重新配网（忘记当前 WiFi）'),
          ),
        ],
        cancelButton: CupertinoActionSheetAction(
          isDefaultAction: true,
          onPressed: () => Navigator.of(ctx).pop(),
          child: const Text('取消'),
        ),
      ),
    );
  }

  Future<void> _confirmReprovision() async {
    final ok = await showCupertinoDialog<bool>(
      context: context,
      builder: (ctx) => CupertinoAlertDialog(
        title: const Text('重新配网'),
        content: const Text(
            '将通过 MQTT 告知在线设备忘记当前 WiFi 并重启进入热点配网。\n需设备当前在线（能收到指令）。'),
        actions: [
          CupertinoDialogAction(
            onPressed: () => Navigator.of(ctx).pop(false),
            child: const Text('取消'),
          ),
          CupertinoDialogAction(
            isDestructiveAction: true,
            onPressed: () => Navigator.of(ctx).pop(true),
            child: const Text('确定重新配网'),
          ),
        ],
      ),
    );
    if (ok == true) {
      widget.mqtt.sendDoorbellCommand('reprovision');
      if (!mounted) return;
      showCupertinoDialog<void>(
        context: context,
        builder: (ctx) => CupertinoAlertDialog(
          title: const Text('已发送'),
          content: const Text(
              '指令已下发。若设备在线，它会重启并开出热点 Doorbell-xxxx，随后用“手机配网”重新下发 WiFi。'),
          actions: [
            CupertinoDialogAction(
              isDefaultAction: true,
              onPressed: () => Navigator.of(ctx).pop(),
              child: const Text('好'),
            ),
          ],
        ),
      );
    }
  }

  Future<void> _editStreamUrl() async {
    final controller = TextEditingController(text: CameraSettings.streamUrl.value);
    final result = await showCupertinoDialog<String>(
      context: context,
      builder: (ctx) => CupertinoAlertDialog(
        title: const Text('视频地址'),
        content: Padding(
          padding: const EdgeInsets.only(top: 12),
          child: Column(
            mainAxisSize: MainAxisSize.min,
            children: [
              CupertinoTextField(
                controller: controller,
                placeholder: 'http://…/stream',
                keyboardType: TextInputType.url,
                autocorrect: false,
                autofocus: true,
                maxLines: 2,
              ),
              const SizedBox(height: 8),
              Row(
                mainAxisAlignment: MainAxisAlignment.spaceEvenly,
                children: [
                  CupertinoButton(
                    padding: const EdgeInsets.symmetric(horizontal: 8),
                    onPressed: () => controller.text = AppConfig.relayStreamUrl,
                    child: const Text('中继(远程)', style: TextStyle(fontSize: 13)),
                  ),
                  CupertinoButton(
                    padding: const EdgeInsets.symmetric(horizontal: 8),
                    onPressed: () => controller.text = AppConfig.lanStreamUrlExample,
                    child: const Text('局域网', style: TextStyle(fontSize: 13)),
                  ),
                ],
              ),
            ],
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
            child: const Text('保存'),
          ),
        ],
      ),
    );
    if (result != null && result.isNotEmpty) {
      if (result == AppConfig.relayStreamUrl) {
        CameraSettings.useRelay();
      } else {
        final uri = Uri.tryParse(result);
        if (uri != null && uri.host.isNotEmpty) {
          CameraSettings.lanHost.value = uri.host;
        }
        CameraSettings.mode.value = CameraMode.local;
        CameraSettings.streamUrl.value = result;
      }
    }
  }

  @override
  Widget build(BuildContext context) {
    return CupertinoPageScaffold(
      navigationBar: CupertinoNavigationBar(
        middle: const Text('门铃'),
        trailing: CupertinoButton(
          padding: EdgeInsets.zero,
          onPressed: _editStreamUrl,
          child: const Icon(CupertinoIcons.gear),
        ),
      ),
      child: SafeArea(
        child: ListView(
          padding: const EdgeInsets.all(16),
          children: [
            StreamBuilder<bool>(
              stream: widget.mqtt.connection,
              initialData: widget.mqtt.isConnected,
              builder: (context, snapshot) {
                final connected = snapshot.data ?? false;
                return _StatusCard(connected: connected, lastEvent: _lastEvent);
              },
            ),
            const SizedBox(height: 12),
            ValueListenableBuilder<String>(
              valueListenable: CameraSettings.streamUrl,
              builder: (_, url, _) => GestureDetector(
                onTap: _editStreamUrl,
                child: Container(
                  padding: const EdgeInsets.all(16),
                  decoration: BoxDecoration(
                    color: CupertinoColors.systemBackground,
                    borderRadius: BorderRadius.circular(14),
                    border: Border.all(color: CupertinoColors.systemGrey5),
                  ),
                  child: Row(
                    children: [
                      const Icon(CupertinoIcons.videocam_fill,
                          color: CupertinoColors.activeBlue),
                      const SizedBox(width: 8),
                      const Text('视频地址',
                          style: TextStyle(fontWeight: FontWeight.w600)),
                      const SizedBox(width: 12),
                      Expanded(
                        child: Text(url,
                            textAlign: TextAlign.right,
                            overflow: TextOverflow.ellipsis,
                            style: const TextStyle(
                                color: CupertinoColors.systemGrey)),
                      ),
                      const SizedBox(width: 4),
                      const Icon(CupertinoIcons.chevron_forward,
                          size: 16, color: CupertinoColors.systemGrey3),
                    ],
                  ),
                ),
              ),
            ),
            const SizedBox(height: 12),
            GestureDetector(
              onTap: _showProvisioningOptions,
              child: Container(
                padding: const EdgeInsets.all(16),
                decoration: BoxDecoration(
                  color: CupertinoColors.systemBackground,
                  borderRadius: BorderRadius.circular(14),
                  border: Border.all(color: CupertinoColors.systemGrey5),
                ),
                child: Row(
                  children: const [
                    Icon(CupertinoIcons.wifi, color: CupertinoColors.activeBlue),
                    SizedBox(width: 8),
                    Text('设备配网',
                        style: TextStyle(fontWeight: FontWeight.w600)),
                    SizedBox(width: 12),
                    Expanded(
                      child: Text('首次上电 / 换 WiFi',
                          textAlign: TextAlign.right,
                          overflow: TextOverflow.ellipsis,
                          style: TextStyle(color: CupertinoColors.systemGrey)),
                    ),
                    SizedBox(width: 4),
                    Icon(CupertinoIcons.chevron_forward,
                        size: 16, color: CupertinoColors.systemGrey3),
                  ],
                ),
              ),
            ),
            const SizedBox(height: 24),
            Center(
              child: Container(
                width: 140,
                height: 140,
                decoration: const BoxDecoration(
                  color: CupertinoColors.systemGrey6,
                  shape: BoxShape.circle,
                ),
                child: const Icon(CupertinoIcons.bell_fill,
                    size: 64, color: CupertinoColors.activeBlue),
              ),
            ),
            const SizedBox(height: 32),
            CupertinoButton.filled(
              onPressed: _openLiveView,
              child: const Text('查看实时画面'),
            ),
          ],
        ),
      ),
    );
  }
}

class _StatusCard extends StatelessWidget {
  const _StatusCard({required this.connected, required this.lastEvent});

  final bool connected;
  final String lastEvent;

  @override
  Widget build(BuildContext context) {
    return Container(
      padding: const EdgeInsets.all(16),
      decoration: BoxDecoration(
        color: CupertinoColors.systemBackground,
        borderRadius: BorderRadius.circular(14),
        border: Border.all(color: CupertinoColors.systemGrey5),
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(
            children: [
              Icon(
                connected
                    ? CupertinoIcons.checkmark_seal_fill
                    : CupertinoIcons.exclamationmark_triangle_fill,
                color: connected
                    ? CupertinoColors.activeGreen
                    : CupertinoColors.systemOrange,
              ),
              const SizedBox(width: 8),
              Text(
                connected ? '已连接信令服务器' : '未连接',
                style: const TextStyle(fontSize: 16, fontWeight: FontWeight.w600),
              ),
            ],
          ),
          const SizedBox(height: 12),
          Text('最近事件：$lastEvent',
              style: const TextStyle(color: CupertinoColors.systemGrey)),
        ],
      ),
    );
  }
}
