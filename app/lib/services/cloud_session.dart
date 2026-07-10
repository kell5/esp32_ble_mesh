import 'package:shared_preferences/shared_preferences.dart';

/// Locally persisted cloud login: the service base URL, the shared API token
/// and the owning user id. There is no password auth yet — the cloud service
/// uses a `user_id` ownership model plus an optional `X-Cloud-Token` secret —
/// so "login" here just remembers where to talk and as whom.
class CloudSession {
  CloudSession({this.baseUrl = '', this.token = '', this.userId = ''});

  String baseUrl;
  String token;
  String userId;

  static const _kBaseUrl = 'cloud_base_url';
  static const _kToken = 'cloud_api_token';
  static const _kUserId = 'cloud_user_id';

  bool get isLoggedIn => baseUrl.isNotEmpty && userId.isNotEmpty;

  static Future<CloudSession> load() async {
    final prefs = await SharedPreferences.getInstance();
    return CloudSession(
      baseUrl: prefs.getString(_kBaseUrl) ?? '',
      token: prefs.getString(_kToken) ?? '',
      userId: prefs.getString(_kUserId) ?? '',
    );
  }

  Future<void> save() async {
    final prefs = await SharedPreferences.getInstance();
    await prefs.setString(_kBaseUrl, baseUrl);
    await prefs.setString(_kToken, token);
    await prefs.setString(_kUserId, userId);
  }

  Future<void> clear() async {
    baseUrl = '';
    token = '';
    userId = '';
    final prefs = await SharedPreferences.getInstance();
    await prefs.remove(_kBaseUrl);
    await prefs.remove(_kToken);
    await prefs.remove(_kUserId);
  }
}
