import 'dart:async';

import 'package:flutter/cupertino.dart';

import '../services/cloud_client.dart';
import '../services/mqtt_service.dart' show DeviceType;
import '../widgets/device_icon.dart';
import '../widgets/smart_cards.dart';

/// Detail page for one cloud device: shows its unified shadow (reported vs
/// desired) and, for controllable devices, an on/off toggle that patches the
/// desired state.
class CloudDeviceDetailPage extends StatefulWidget {
  const CloudDeviceDetailPage({
    super.key,
    required this.client,
    required this.device,
    required this.initialShadow,
  });

  final CloudClient client;
  final CloudDevice device;
  final CloudShadow? initialShadow;

  @override
  State<CloudDeviceDetailPage> createState() => _CloudDeviceDetailPageState();
}

class _CloudDeviceDetailPageState extends State<CloudDeviceDetailPage> {
  CloudShadow? _shadow;
  List<CloudOtaUpdate> _otaUpdates = const [];
  bool _busy = false;
  bool _otaBusy = false;
  String? _error;
  String? _otaMessage;

  @override
  void initState() {
    super.initState();
    _shadow = widget.initialShadow;
    unawaited(_refreshOtaUpdates());
  }

  Future<void> _refresh() async {
    try {
      final shadow = await widget.client.getShadow(widget.device.deviceId);
      final otaUpdates = await widget.client.listOtaUpdates(
        widget.device.deviceId,
      );
      if (!mounted) return;
      setState(() {
        _shadow = shadow;
        _otaUpdates = otaUpdates;
        _error = null;
      });
    } on CloudApiException catch (e) {
      if (!mounted) return;
      setState(() => _error = e.message);
    }
  }

  Future<void> _refreshOtaUpdates() async {
    try {
      final otaUpdates = await widget.client.listOtaUpdates(
        widget.device.deviceId,
      );
      if (!mounted) return;
      setState(() => _otaUpdates = otaUpdates);
    } on CloudApiException catch (_) {
      // OTA history is optional on the detail page; shadow/control should not
      // be hidden just because there is no OTA record yet.
    }
  }

  Future<void> _checkOta() async {
    var shadow = _shadow;
    if (shadow == null) {
      shadow = await widget.client.getShadow(widget.device.deviceId);
      if (!mounted) return;
      setState(() => _shadow = shadow);
    }
    final productId = shadow?.productId;
    final hwVersion = shadow?.hwVersion;
    final fwVersion = shadow?.fwVersion;
    if (productId == null || hwVersion == null || fwVersion == null) {
      setState(
        () => _otaMessage =
            '设备还没有上报 product_id / hw_version / fw_version，暂不能检查 OTA。',
      );
      return;
    }

    setState(() {
      _otaBusy = true;
      _otaMessage = null;
      _error = null;
    });
    try {
      final result = await widget.client.checkOta(
        widget.device.deviceId,
        productId: productId,
        hwVersion: hwVersion,
        fwVersion: fwVersion,
      );
      final otaUpdates = await widget.client.listOtaUpdates(
        widget.device.deviceId,
      );
      if (!mounted) return;
      setState(() {
        _otaUpdates = otaUpdates;
        _otaMessage = _otaResultMessage(result);
      });
    } on CloudApiException catch (e) {
      if (!mounted) return;
      setState(() => _otaMessage = e.message);
    } finally {
      if (mounted) setState(() => _otaBusy = false);
    }
  }

  String _otaResultMessage(CloudOtaCheckResult result) {
    final target = result.target?.fwVersion ?? result.update?.targetFwVersion;
    if (target == null) return result.reasonLabel;
    return '${result.reasonLabel}：$target';
  }

  Future<void> _toggle() async {
    final shadow = _shadow;
    final target = !(shadow?.on ?? false);
    setState(() {
      _busy = true;
      _error = null;
    });
    try {
      await widget.client.setDesired(
        widget.device.deviceId,
        {'on': target},
        messageId: 'app-${DateTime.now().millisecondsSinceEpoch}',
      );
      await _refresh();
    } on CloudApiException catch (e) {
      if (!mounted) return;
      setState(() => _error = e.message);
    } finally {
      if (mounted) setState(() => _busy = false);
    }
  }

  @override
  Widget build(BuildContext context) {
    final device = widget.device;
    final type = device.deviceType;
    final shadow = _shadow;
    final online = shadow?.online ?? false;
    final on = shadow?.on ?? false;

    return CupertinoPageScaffold(
      backgroundColor: CupertinoColors.systemGroupedBackground,
      navigationBar: CupertinoNavigationBar(middle: Text(device.displayName)),
      child: SafeArea(
        child: CustomScrollView(
          slivers: [
            CupertinoSliverRefreshControl(onRefresh: _refresh),
            SliverPadding(
              padding: const EdgeInsets.all(16),
              sliver: SliverList(
                delegate: SliverChildListDelegate([
                  _hero(type, online: online, on: on),
                  const SizedBox(height: 16),
                  _infoCard(shadow, online: online, on: on),
                  const SizedBox(height: 16),
                  _otaCard(shadow),
                  if (_error != null) ...[
                    const SizedBox(height: 12),
                    Text(
                      _error!,
                      style: const TextStyle(
                        color: CupertinoColors.systemRed,
                        fontSize: 13,
                      ),
                    ),
                  ],
                ]),
              ),
            ),
          ],
        ),
      ),
    );
  }

  Widget _hero(DeviceType type, {required bool online, required bool on}) {
    return Container(
      padding: const EdgeInsets.symmetric(vertical: 28),
      decoration: BoxDecoration(
        color: CupertinoColors.systemBackground,
        borderRadius: BorderRadius.circular(16),
      ),
      child: Column(
        children: [
          DeviceIcon(
            type: type,
            online: online,
            on: on,
            size: 148,
            borderRadius: 24,
          ),
          const SizedBox(height: 14),
          Text(
            type.stateLabel(online: online, on: on),
            style: const TextStyle(fontSize: 16, fontWeight: FontWeight.w600),
          ),
          if (type.isControllable) ...[
            const SizedBox(height: 18),
            _busy
                ? const CupertinoActivityIndicator()
                : PowerButton(
                    on: online && on,
                    enabled: online,
                    onPressed: _toggle,
                    size: 64,
                  ),
          ],
        ],
      ),
    );
  }

  Widget _infoCard(
    CloudShadow? shadow, {
    required bool online,
    required bool on,
  }) {
    final device = widget.device;
    final desiredOn = shadow?.desiredOn;
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 16),
      decoration: BoxDecoration(
        color: CupertinoColors.systemBackground,
        borderRadius: BorderRadius.circular(16),
      ),
      child: Column(
        children: [
          _row('设备 ID', device.deviceId),
          _divider(),
          _row('设备类型', device.deviceType.label),
          _divider(),
          _row('在线状态', online ? '在线' : '离线'),
          if (shadow?.offlineReason != null) ...[
            _divider(),
            _row('离线原因', shadow!.offlineReason!),
          ],
          if (device.deviceType.isControllable) ...[
            _divider(),
            _row('上报状态', on ? '开' : '关'),
            _divider(),
            _row('期望状态', desiredOn == null ? '—' : (desiredOn ? '开' : '关')),
          ],
          if (shadow?.value != null) ...[
            _divider(),
            _row('读数', shadow!.value!),
          ],
          if (shadow?.lastEvent != null) ...[
            _divider(),
            _row('最近事件', shadow!.lastEvent!),
          ],
          if (shadow?.lastSeenAt != null) ...[
            _divider(),
            _row('最近上报', agoLabel(shadow!.lastSeenAt!)),
          ],
        ],
      ),
    );
  }

  Widget _otaCard(CloudShadow? shadow) {
    final latest = _otaUpdates.isEmpty ? null : _otaUpdates.first;
    final currentFw = shadow?.fwVersion ?? '未上报';
    final productId = shadow?.productId;
    final hwVersion = shadow?.hwVersion;
    final canCheck = shadow?.supportsOta == true && !_otaBusy;
    return Container(
      padding: const EdgeInsets.all(16),
      decoration: BoxDecoration(
        color: CupertinoColors.systemBackground,
        borderRadius: BorderRadius.circular(16),
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          const Text(
            '固件升级',
            style: TextStyle(fontSize: 16, fontWeight: FontWeight.w700),
          ),
          const SizedBox(height: 12),
          _row('当前版本', currentFw),
          if (productId != null) ...[_divider(), _row('产品型号', productId)],
          if (hwVersion != null) ...[_divider(), _row('硬件版本', hwVersion)],
          if (latest != null) ...[
            _divider(),
            _row('最近任务', '${latest.targetFwVersion} · ${latest.statusLabel}'),
            if (latest.updatedAt != null) ...[
              _divider(),
              _row('任务更新', agoLabel(latest.updatedAt!)),
            ],
          ],
          const SizedBox(height: 12),
          SizedBox(
            width: double.infinity,
            child: CupertinoButton.filled(
              padding: const EdgeInsets.symmetric(vertical: 12),
              onPressed: canCheck ? _checkOta : null,
              child: _otaBusy
                  ? const CupertinoActivityIndicator(
                      color: CupertinoColors.white,
                    )
                  : const Text('检查更新'),
            ),
          ),
          if (_otaMessage != null) ...[
            const SizedBox(height: 10),
            Text(
              _otaMessage!,
              style: const TextStyle(
                color: CupertinoColors.systemGrey,
                fontSize: 13,
              ),
            ),
          ] else if (shadow?.supportsOta != true) ...[
            const SizedBox(height: 10),
            const Text(
              '设备上报 OTA 能力后，这里会显示检查更新入口。',
              style: TextStyle(color: CupertinoColors.systemGrey, fontSize: 13),
            ),
          ],
        ],
      ),
    );
  }

  Widget _row(String k, String v) => Padding(
    padding: const EdgeInsets.symmetric(vertical: 14),
    child: Row(
      children: [
        Text(k, style: const TextStyle(fontSize: 15)),
        const Spacer(),
        Flexible(
          child: Text(
            v,
            textAlign: TextAlign.right,
            style: const TextStyle(
              fontSize: 15,
              color: CupertinoColors.systemGrey,
            ),
          ),
        ),
      ],
    ),
  );

  Widget _divider() =>
      Container(height: 0.5, color: CupertinoColors.systemGrey5);
}
