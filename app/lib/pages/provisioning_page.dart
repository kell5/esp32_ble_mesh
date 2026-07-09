import 'package:flutter/cupertino.dart';

import '../config.dart';
import '../prov/connection_models.dart';
import '../prov/provisioning.dart';
import '../prov/security1.dart';
import '../prov/transport_http.dart';

/// SoftAP WiFi provisioning screen.
///
/// The phone must first join the board's open hotspot (e.g. `Doorbell-4C3408`)
/// from the system WiFi settings. This page then talks to the board over the
/// ESP protocomm SoftAP + Security1 channel (default 192.168.4.1) to hand it
/// the target WiFi SSID + password.
class ProvisioningPage extends StatefulWidget {
  const ProvisioningPage({super.key});

  @override
  State<ProvisioningPage> createState() => _ProvisioningPageState();
}

class _ProvisioningPageState extends State<ProvisioningPage> {
  final _ssid = TextEditingController();
  final _password = TextEditingController();
  final _pop = TextEditingController(text: AppConfig.provPop);
  final _host = TextEditingController(text: AppConfig.provHost);

  bool _busy = false;
  String _status = '';
  bool _obscure = true;

  @override
  void dispose() {
    _ssid.dispose();
    _password.dispose();
    _pop.dispose();
    _host.dispose();
    super.dispose();
  }

  void _log(String s) {
    if (mounted) setState(() => _status = s);
  }

  Provisioning _newSession() => Provisioning(
        transport: TransportHTTP(hostname: _host.text.trim()),
        security: Security1(pop: _pop.text),
      );

  Future<void> _scan() async {
    if (_busy) return;
    setState(() => _busy = true);
    final prov = _newSession();
    try {
      _log('正在连接门铃热点并建立安全会话…');
      if (!await prov.establishSession()) {
        _log('会话建立失败：请确认手机已连上门铃热点，且配网口令正确。');
        return;
      }
      _log('正在扫描周边 WiFi…');
      final list = await prov.startScanWiFi();
      if (!mounted) return;
      if (list == null || list.isEmpty) {
        _log('未扫描到 WiFi，可手动输入名称。');
        return;
      }
      _log('');
      await _pickSsid(list);
    } catch (e) {
      _log('扫描出错：$e');
    } finally {
      await prov.dispose();
      if (mounted) setState(() => _busy = false);
    }
  }

  Future<void> _pickSsid(List<Map<String, dynamic>> list) async {
    // De-duplicate by SSID, keep strongest signal.
    final seen = <String>{};
    final items = <Map<String, dynamic>>[];
    for (final e in list) {
      final ssid = (e['ssid'] ?? '').toString();
      if (ssid.isEmpty || seen.contains(ssid)) continue;
      seen.add(ssid);
      items.add(e);
    }
    final chosen = await showCupertinoModalPopup<String>(
      context: context,
      builder: (ctx) => CupertinoActionSheet(
        title: const Text('选择 WiFi'),
        actions: [
          for (final e in items)
            CupertinoActionSheetAction(
              onPressed: () => Navigator.of(ctx).pop(e['ssid'].toString()),
              child: Text('${e['ssid']}   (${e['rssi']}dBm)'),
            ),
        ],
        cancelButton: CupertinoActionSheetAction(
          isDestructiveAction: true,
          onPressed: () => Navigator.of(ctx).pop(),
          child: const Text('取消'),
        ),
      ),
    );
    if (chosen != null) _ssid.text = chosen;
  }

  Future<void> _provision() async {
    if (_busy) return;
    if (_ssid.text.trim().isEmpty) {
      _log('请先填写要连接的 WiFi 名称。');
      return;
    }
    setState(() => _busy = true);
    final prov = _newSession();
    try {
      _log('正在建立安全会话…');
      if (!await prov.establishSession()) {
        _log('会话建立失败：请确认手机已连上门铃热点，且配网口令正确。');
        return;
      }
      _log('正在下发 WiFi 配置…');
      if (!await prov.sendWifiConfig(
          ssid: _ssid.text.trim(), password: _password.text)) {
        _log('下发配置失败。');
        return;
      }
      if (!await prov.applyWifiConfig()) {
        _log('应用配置失败。');
        return;
      }
      _log('门铃正在连接 WiFi，请稍候…');
      for (var i = 0; i < 15; i++) {
        await Future<void>.delayed(const Duration(seconds: 1));
        ConnectionStatus st;
        try {
          st = await prov.getStatus();
        } catch (_) {
          continue; // board may briefly drop SoftAP while switching
        }
        switch (st.state) {
          case WifiConnectionState.Connected:
            _log('配网成功！门铃已连上「${_ssid.text.trim()}」，IP：${st.ip ?? '-'}');
            if (mounted) await _showDone();
            return;
          case WifiConnectionState.ConnectionFailed:
            final reason =
                st.failedReason == WifiConnectFailedReason.AuthError
                    ? 'WiFi 密码错误'
                    : '找不到该 WiFi（可能是 5G 或超出范围）';
            _log('配网失败：$reason。请检查后重试。');
            return;
          default:
            _log('门铃正在连接 WiFi…（${i + 1}s）');
        }
      }
      _log('等待超时。门铃可能仍在连接，请稍后在门铃页确认是否上线。');
    } catch (e) {
      _log('配网出错：$e');
    } finally {
      await prov.dispose();
      if (mounted) setState(() => _busy = false);
    }
  }

  Future<void> _showDone() async {
    await showCupertinoDialog<void>(
      context: context,
      builder: (ctx) => CupertinoAlertDialog(
        title: const Text('配网成功'),
        content: const Text(
            '门铃已连上目标 WiFi。请把手机切回你平时用的 WiFi 或移动网络，即可通过中继远程查看画面。'),
        actions: [
          CupertinoDialogAction(
            isDefaultAction: true,
            onPressed: () {
              Navigator.of(ctx).pop();
              Navigator.of(context).pop();
            },
            child: const Text('完成'),
          ),
        ],
      ),
    );
  }

  @override
  Widget build(BuildContext context) {
    return CupertinoPageScaffold(
      navigationBar: const CupertinoNavigationBar(middle: Text('设备配网')),
      child: SafeArea(
        child: ListView(
          padding: const EdgeInsets.all(16),
          children: [
            _stepCard(),
            const SizedBox(height: 16),
            _field('要连接的 WiFi', _ssid, placeholder: '2.4G WiFi 名称',
                trailing: CupertinoButton(
                  padding: EdgeInsets.zero,
                  onPressed: _busy ? null : _scan,
                  child: const Text('扫描'),
                )),
            _field('WiFi 密码', _password,
                placeholder: '留空表示开放网络',
                obscure: _obscure,
                trailing: CupertinoButton(
                  padding: EdgeInsets.zero,
                  onPressed: () => setState(() => _obscure = !_obscure),
                  child: Icon(_obscure
                      ? CupertinoIcons.eye
                      : CupertinoIcons.eye_slash),
                )),
            _field('配网口令 (PoP)', _pop, placeholder: 'doorbell1234'),
            _field('门铃热点地址', _host, placeholder: '192.168.4.1'),
            const SizedBox(height: 20),
            CupertinoButton.filled(
              onPressed: _busy ? null : _provision,
              child: _busy
                  ? const CupertinoActivityIndicator()
                  : const Text('开始配网'),
            ),
            if (_status.isNotEmpty) ...[
              const SizedBox(height: 16),
              Text(_status,
                  style: const TextStyle(color: CupertinoColors.systemGrey)),
            ],
          ],
        ),
      ),
    );
  }

  Widget _stepCard() {
    return Container(
      padding: const EdgeInsets.all(16),
      decoration: BoxDecoration(
        color: CupertinoColors.systemGrey6,
        borderRadius: BorderRadius.circular(14),
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: const [
          Text('配网步骤',
              style: TextStyle(fontWeight: FontWeight.w700, fontSize: 16)),
          SizedBox(height: 8),
          Text('1. 到手机「设置 › 无线局域网」，连接名为 ${AppConfig.provSoftApPrefix}xxxxxx 的门铃热点（开放，无需密码）。'),
          SizedBox(height: 4),
          Text('2. 回到本页，填写门铃要连接的 2.4G WiFi 和密码（ESP32 不支持 5G）。'),
          SizedBox(height: 4),
          Text('3. 点「开始配网」。成功后把手机切回常用网络即可。'),
        ],
      ),
    );
  }

  Widget _field(String label, TextEditingController c,
      {String? placeholder, bool obscure = false, Widget? trailing}) {
    return Padding(
      padding: const EdgeInsets.only(bottom: 12),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Text(label,
              style: const TextStyle(
                  fontSize: 13, color: CupertinoColors.systemGrey)),
          const SizedBox(height: 4),
          Row(
            children: [
              Expanded(
                child: CupertinoTextField(
                  controller: c,
                  placeholder: placeholder,
                  obscureText: obscure,
                  autocorrect: false,
                  enableSuggestions: false,
                ),
              ),
              ?trailing,
            ],
          ),
        ],
      ),
    );
  }
}
