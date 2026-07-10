import 'package:flutter/cupertino.dart';

import '../services/cloud_client.dart';
import '../services/cloud_session.dart';

/// Lightweight cloud login: the user enters the service base URL, the optional
/// API token and their user id. On success the session is persisted and
/// returned to the caller. This intentionally has no password step — the cloud
/// service has no account system yet (see [CloudSession]).
class CloudLoginPage extends StatefulWidget {
  const CloudLoginPage({super.key, required this.session});

  final CloudSession session;

  @override
  State<CloudLoginPage> createState() => _CloudLoginPageState();
}

class _CloudLoginPageState extends State<CloudLoginPage> {
  late final TextEditingController _baseUrl;
  late final TextEditingController _token;
  late final TextEditingController _userId;

  bool _busy = false;
  String? _error;

  @override
  void initState() {
    super.initState();
    _baseUrl = TextEditingController(text: widget.session.baseUrl);
    _token = TextEditingController(text: widget.session.token);
    _userId = TextEditingController(text: widget.session.userId);
  }

  @override
  void dispose() {
    _baseUrl.dispose();
    _token.dispose();
    _userId.dispose();
    super.dispose();
  }

  Future<void> _submit() async {
    final baseUrl = _baseUrl.text.trim();
    final userId = _userId.text.trim();
    final token = _token.text.trim();
    if (baseUrl.isEmpty || userId.isEmpty) {
      setState(() => _error = '请填写云端地址和用户 ID');
      return;
    }
    if (!baseUrl.startsWith('http://') && !baseUrl.startsWith('https://')) {
      setState(() => _error = '云端地址需以 http:// 或 https:// 开头');
      return;
    }

    setState(() {
      _busy = true;
      _error = null;
    });

    final client = CloudClient(baseUrl: baseUrl, token: token);
    try {
      // Verify reachability and that the token lists devices without 401.
      await client.listDevices(userId);
      widget.session
        ..baseUrl = baseUrl
        ..token = token
        ..userId = userId;
      await widget.session.save();
      if (!mounted) return;
      Navigator.of(context).pop(true);
    } on CloudApiException catch (e) {
      if (!mounted) return;
      setState(() => _error = e.message);
    } finally {
      client.close();
      if (mounted) setState(() => _busy = false);
    }
  }

  @override
  Widget build(BuildContext context) {
    return CupertinoPageScaffold(
      backgroundColor: CupertinoColors.systemGroupedBackground,
      navigationBar: const CupertinoNavigationBar(middle: Text('登录云端')),
      child: SafeArea(
        child: ListView(
          padding: const EdgeInsets.all(16),
          children: [
            _field(
              controller: _baseUrl,
              placeholder: 'https://your-cloud-host',
              label: '云端地址',
              keyboardType: TextInputType.url,
            ),
            _field(
              controller: _token,
              placeholder: '未配置可留空',
              label: 'API Token',
              obscure: true,
            ),
            _field(
              controller: _userId,
              placeholder: 'user-001',
              label: '用户 ID',
            ),
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
            const SizedBox(height: 20),
            CupertinoButton.filled(
              onPressed: _busy ? null : _submit,
              child: _busy
                  ? const CupertinoActivityIndicator()
                  : const Text('登录并同步设备'),
            ),
            const Padding(
              padding: EdgeInsets.fromLTRB(4, 18, 4, 0),
              child: Text(
                '登录信息仅保存在本机，用于访问该用户名下已认领的云端设备与影子状态。'
                '云端暂无账号密码体系，用户 ID 即设备归属标识。',
                style: TextStyle(
                  color: CupertinoColors.systemGrey,
                  fontSize: 12,
                ),
              ),
            ),
          ],
        ),
      ),
    );
  }

  Widget _field({
    required TextEditingController controller,
    required String placeholder,
    required String label,
    bool obscure = false,
    TextInputType? keyboardType,
  }) {
    return Padding(
      padding: const EdgeInsets.only(bottom: 12),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Padding(
            padding: const EdgeInsets.only(left: 4, bottom: 6),
            child: Text(
              label,
              style: const TextStyle(
                fontSize: 13,
                color: CupertinoColors.systemGrey,
              ),
            ),
          ),
          CupertinoTextField(
            controller: controller,
            placeholder: placeholder,
            obscureText: obscure,
            keyboardType: keyboardType,
            autocorrect: false,
            enableSuggestions: false,
            padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 12),
          ),
        ],
      ),
    );
  }
}
