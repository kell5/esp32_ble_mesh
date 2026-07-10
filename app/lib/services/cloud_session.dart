import 'package:shared_preferences/shared_preferences.dart';

/// Locally persisted cloud login. After the move to email accounts, "login"
/// means: the service base URL plus a per-user bearer [token] issued by
/// `POST /auth/login`. [userId] and [email] are cached for display only — the
/// token is what actually authorizes requests.
class CloudSession {
  CloudSession({
    this.baseUrl = '',
    this.token = '',
    this.userId = '',
    this.email = '',
  });

  String baseUrl;
  String token;
  String userId;
  String email;

  static const _kBaseUrl = 'cloud_base_url';
  static const _kToken = 'cloud_auth_token';
  static const _kUserId = 'cloud_user_id';
  static const _kEmail = 'cloud_email';

  bool get isLoggedIn => baseUrl.isNotEmpty && token.isNotEmpty;

  static Future<CloudSession> load() async {
    final prefs = await SharedPreferences.getInstance();
    return CloudSession(
      baseUrl: prefs.getString(_kBaseUrl) ?? '',
      token: prefs.getString(_kToken) ?? '',
      userId: prefs.getString(_kUserId) ?? '',
      email: prefs.getString(_kEmail) ?? '',
    );
  }

  Future<void> save() async {
    final prefs = await SharedPreferences.getInstance();
    await prefs.setString(_kBaseUrl, baseUrl);
    await prefs.setString(_kToken, token);
    await prefs.setString(_kUserId, userId);
    await prefs.setString(_kEmail, email);
  }

  Future<void> clear() async {
    baseUrl = '';
    token = '';
    userId = '';
    email = '';
    final prefs = await SharedPreferences.getInstance();
    await prefs.remove(_kBaseUrl);
    await prefs.remove(_kToken);
    await prefs.remove(_kUserId);
    await prefs.remove(_kEmail);
  }
}
