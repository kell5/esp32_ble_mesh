import 'package:flutter/cupertino.dart';

import '../services/cloud_client.dart';
import '../services/cloud_session.dart';

/// Email account gate. The user enters the service base URL plus an email +
/// password, and either registers a new account or logs in. On success a
/// per-user bearer token is persisted in [CloudSession] and [onAuthenticated]
/// is invoked so the shell can swap in the device list. When used as a pushed
/// route (no callback) it pops `true` instead.
class CloudLoginPage extends StatefulWidget {
  const CloudLoginPage({
    super.key,
    required this.session,
    this.onAuthenticated,
  });

  final CloudSession session;
  final VoidCallback? onAuthenticated;

  @override
  State<CloudLoginPage> createState() => _CloudLoginPageState();
}

class _CloudLoginPageState extends State<CloudLoginPage> {
  late final TextEditingController _baseUrl;
  late final TextEditingController _email;
  late final TextEditingController _password;

  bool _register = false;
  bool _busy = false;
  String? _error;

  @override
  void initState() {
    super.initState();
    _baseUrl = TextEditingController(
      text: widget.session.baseUrl.isEmpty
          ? 'https://lk-mcu.online/cloud'
          : widget.session.baseUrl,
    );
    _email = TextEditingController(text: widget.session.email);
    _password = TextEditingController();
  }

  @override
  void dispose() {
    _baseUrl.dispose();
    _email.dispose();
    _password.dispose();
    super.dispose();
  }

  Future<void> _submit() async {
    final baseUrl = _baseUrl.text.trim();
    final email = _email.text.trim();
    final password = _password.text;
    if (baseUrl.isEmpty || email.isEmpty || password.isEmpty) {
      setState(() => _error = '请填写云端地址、邮箱和密码');
      return;
    }
    if (!baseUrl.startsWith('http://') && !baseUrl.startsWith('https://')) {
      setState(() => _error = '云端地址需以 http:// 或 https:// 开头');
      return;
    }
    if (password.length < 6) {
      setState(() => _error = '密码至少 6 位');
      return;
    }

    setState(() {
      _busy = true;
      _error = null;
    });

    final client = CloudClient(baseUrl: baseUrl);
    try {
      final auth = _register
          ? await client.register(email, password)
          : await client.login(email, password);
      widget.session
        ..baseUrl = baseUrl
        ..token = auth.token
        ..userId = auth.userId
        ..email = auth.email;
      await widget.session.save();
      if (!mounted) return;
      final onAuthenticated = widget.onAuthenticated;
      if (onAuthenticated != null) {
        onAuthenticated();
      } else {
        Navigator.of(context).pop(true);
      }
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
      navigationBar: CupertinoNavigationBar(
        middle: Text(_register ? '注册账号' : '登录云端'),
      ),
      child: SafeArea(
        child: ListView(
          padding: const EdgeInsets.all(16),
          children: [
            CupertinoSlidingSegmentedControl<bool>(
              groupValue: _register,
              onValueChanged: (value) {
                if (_busy || value == null) return;
                setState(() {
                  _register = value;
                  _error = null;
                });
              },
              children: const {
                false: Padding(
                  padding: EdgeInsets.symmetric(vertical: 6),
                  child: Text('登录'),
                ),
                true: Padding(
                  padding: EdgeInsets.symmetric(vertical: 6),
                  child: Text('注册'),
                ),
              },
            ),
            const SizedBox(height: 18),
            _field(
              controller: _baseUrl,
              placeholder: 'https://lk-mcu.online/cloud',
              label: '云端地址',
              keyboardType: TextInputType.url,
            ),
            _field(
              controller: _email,
              placeholder: 'you@example.com',
              label: '邮箱',
              keyboardType: TextInputType.emailAddress,
            ),
            _field(
              controller: _password,
              placeholder: '至少 6 位',
              label: '密码',
              obscure: true,
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
                  : Text(_register ? '注册并登录' : '登录'),
            ),
            const Padding(
              padding: EdgeInsets.fromLTRB(4, 18, 4, 0),
              child: Text(
                '每个账号是独立的设备空间：登录后添加（认领）属于你的设备，'
                '之后无需连接硬件即可远程查看与控制。登录凭据仅保存在本机。',
                style: TextStyle(color: CupertinoColors.systemGrey, fontSize: 12),
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
