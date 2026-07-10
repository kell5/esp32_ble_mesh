import 'dart:async';
import 'dart:convert';

import 'package:http/http.dart' as http;

import 'mqtt_service.dart' show DeviceType;

/// Raised when the cloud service returns a non-success response or is
/// unreachable. [statusCode] is null for transport-level failures.
class CloudApiException implements Exception {
  CloudApiException(this.message, {this.statusCode});

  final String message;
  final int? statusCode;

  @override
  String toString() => 'CloudApiException($statusCode): $message';
}

/// A device as returned by `GET /api/v1/users/<user>/devices`
/// (matches `DeviceResponse` in cloud_service/models.py).
class CloudDevice {
  const CloudDevice({
    required this.deviceId,
    required this.type,
    this.ownerId,
    this.name,
    this.roomId,
  });

  final String deviceId;
  final String type;
  final String? ownerId;
  final String? name;
  final String? roomId;

  DeviceType get deviceType => DeviceType.fromString(type);

  String get displayName {
    if (name != null && name!.isNotEmpty) return name!;
    final dash = deviceId.lastIndexOf('-');
    final suffix = dash >= 0 ? deviceId.substring(dash + 1) : deviceId;
    return '${deviceType.label} $suffix';
  }

  static CloudDevice? fromJson(Map<String, dynamic> j) {
    final id = j['device_id'];
    if (id is! String || id.isEmpty) return null;
    return CloudDevice(
      deviceId: id,
      type: (j['type'] as String?) ?? 'unknown',
      ownerId: j['owner_id'] as String?,
      name: j['name'] as String?,
      roomId: j['room_id'] as String?,
    );
  }
}

/// The unified device shadow from `GET /api/v1/devices/<id>/shadow`
/// (matches `ShadowResponse`).
class CloudShadow {
  const CloudShadow({
    required this.deviceId,
    required this.desired,
    required this.reported,
    required this.version,
    this.offlineReason,
    this.lastSeenAt,
  });

  final String deviceId;
  final Map<String, dynamic> desired;
  final Map<String, dynamic> reported;
  final int version;
  final String? offlineReason;
  final DateTime? lastSeenAt;

  /// Firmware reports `online: true`; an offline reason means it dropped.
  bool get online => reported['online'] == true && offlineReason == null;

  /// Controllable devices report `on` (or legacy `state: "on"/"off"`).
  bool get on =>
      reported['on'] == true ||
      (reported['on'] == null && reported['state'] == 'on');

  /// The pending desired power state, if any (before firmware confirms it).
  bool? get desiredOn => desired['on'] is bool ? desired['on'] as bool : null;

  /// Optional sensor reading for read-only devices.
  String? get value => reported['value']?.toString();

  String? get lastEvent => reported['last_event']?.toString();

  static CloudShadow? fromJson(Map<String, dynamic> j) {
    final id = j['device_id'];
    if (id is! String || id.isEmpty) return null;
    DateTime? lastSeen;
    final rawSeen = j['last_seen_at'];
    if (rawSeen is String) lastSeen = DateTime.tryParse(rawSeen);
    return CloudShadow(
      deviceId: id,
      desired: _asMap(j['desired']),
      reported: _asMap(j['reported']),
      version: (j['version'] is num) ? (j['version'] as num).toInt() : 0,
      offlineReason: j['offline_reason'] as String?,
      lastSeenAt: lastSeen,
    );
  }

  static Map<String, dynamic> _asMap(Object? value) =>
      value is Map<String, dynamic> ? value : <String, dynamic>{};
}

/// A device plus its shadow, ready for the UI.
class CloudDeviceView {
  const CloudDeviceView({required this.device, this.shadow});

  final CloudDevice device;
  final CloudShadow? shadow;

  bool get online => shadow?.online ?? false;
  bool get on => shadow?.on ?? false;
  bool get isControllable => device.deviceType.isControllable;
}

/// Thin REST client for the Phase B cloud service. All mutating and listing
/// endpoints require the `X-Cloud-Token` header when the server is configured
/// with `CLOUD_API_TOKEN`.
class CloudClient {
  CloudClient({
    required this.baseUrl,
    required this.token,
    http.Client? httpClient,
  }) : _http = httpClient ?? http.Client();

  final String baseUrl;
  final String token;
  final http.Client _http;

  static const Duration _timeout = Duration(seconds: 10);

  Map<String, String> get _headers => {
    'Content-Type': 'application/json',
    if (token.isNotEmpty) 'X-Cloud-Token': token,
  };

  Uri _uri(String path) {
    final root = baseUrl.endsWith('/')
        ? baseUrl.substring(0, baseUrl.length - 1)
        : baseUrl;
    return Uri.parse('$root$path');
  }

  Future<bool> health() async {
    final response = await _send(() => _http.get(_uri('/health')));
    return response.statusCode == 200;
  }

  Future<List<CloudDevice>> listDevices(String userId) async {
    final response = await _send(
      () => _http.get(
        _uri('/api/v1/users/${Uri.encodeComponent(userId)}/devices'),
        headers: _headers,
      ),
    );
    _ensureOk(response);
    final decoded = jsonDecode(response.body);
    if (decoded is! List) return const [];
    return decoded
        .whereType<Map<String, dynamic>>()
        .map(CloudDevice.fromJson)
        .whereType<CloudDevice>()
        .toList();
  }

  Future<CloudShadow?> getShadow(String deviceId) async {
    final response = await _send(
      () => _http.get(
        _uri('/api/v1/devices/${Uri.encodeComponent(deviceId)}/shadow'),
        headers: _headers,
      ),
    );
    if (response.statusCode == 404) return null;
    _ensureOk(response);
    final decoded = jsonDecode(response.body);
    return decoded is Map<String, dynamic> ? CloudShadow.fromJson(decoded) : null;
  }

  /// Fetch every device for [userId] along with its shadow (concurrently).
  Future<List<CloudDeviceView>> listDeviceViews(String userId) async {
    final devices = await listDevices(userId);
    final shadows = await Future.wait(
      devices.map((d) async {
        try {
          return await getShadow(d.deviceId);
        } on CloudApiException {
          return null;
        }
      }),
    );
    return [
      for (var i = 0; i < devices.length; i++)
        CloudDeviceView(device: devices[i], shadow: shadows[i]),
    ];
  }

  /// Merge [state] into the device's desired shadow (used for on/off control).
  Future<void> setDesired(
    String deviceId,
    Map<String, Object?> state, {
    String? messageId,
  }) async {
    final body = jsonEncode({
      'state': state,
      'message_id': ?messageId,
    });
    final response = await _send(
      () => _http.patch(
        _uri('/api/v1/devices/${Uri.encodeComponent(deviceId)}/shadow/desired'),
        headers: _headers,
        body: body,
      ),
    );
    _ensureOk(response);
  }

  Future<http.Response> _send(Future<http.Response> Function() request) async {
    try {
      return await request().timeout(_timeout);
    } on TimeoutException {
      throw CloudApiException('请求超时，请检查云端地址与网络');
    } catch (error) {
      throw CloudApiException('无法连接云端：$error');
    }
  }

  void _ensureOk(http.Response response) {
    if (response.statusCode >= 200 && response.statusCode < 300) return;
    if (response.statusCode == 401) {
      throw CloudApiException('鉴权失败：API Token 不正确', statusCode: 401);
    }
    throw CloudApiException(
      'HTTP ${response.statusCode}: ${response.reasonPhrase ?? ''}',
      statusCode: response.statusCode,
    );
  }

  void close() => _http.close();
}
