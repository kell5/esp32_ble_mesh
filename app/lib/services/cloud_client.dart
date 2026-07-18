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

  String? get productId => reported['product_id']?.toString();

  String? get hwVersion => reported['hw_version']?.toString();

  String? get fwVersion => reported['fw_version']?.toString();

  List<String> get capabilities {
    final raw = reported['capabilities'];
    if (raw is! List) return const [];
    return raw.map((item) => item.toString()).toList(growable: false);
  }

  bool get supportsOta =>
      capabilities.contains('system.ota') ||
      (productId != null && hwVersion != null && fwVersion != null);

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

class CloudFirmware {
  const CloudFirmware({
    required this.firmwareId,
    required this.productId,
    required this.hwVersion,
    required this.fwVersion,
    required this.url,
    required this.sha256,
  });

  final String firmwareId;
  final String productId;
  final String hwVersion;
  final String fwVersion;
  final String url;
  final String sha256;

  static CloudFirmware? fromJson(Map<String, dynamic> j) {
    final firmwareId = j['firmware_id'];
    final productId = j['product_id'];
    final hwVersion = j['hw_version'];
    final fwVersion = j['fw_version'];
    final url = j['url'];
    final sha256 = j['sha256'];
    if (firmwareId is! String ||
        productId is! String ||
        hwVersion is! String ||
        fwVersion is! String ||
        url is! String ||
        sha256 is! String) {
      return null;
    }
    return CloudFirmware(
      firmwareId: firmwareId,
      productId: productId,
      hwVersion: hwVersion,
      fwVersion: fwVersion,
      url: url,
      sha256: sha256,
    );
  }
}

class CloudOtaUpdate {
  const CloudOtaUpdate({
    required this.updateId,
    required this.deviceId,
    required this.productId,
    required this.hwVersion,
    required this.targetFwVersion,
    required this.status,
    this.firmwareId,
    this.messageId,
    this.createdAt,
    this.updatedAt,
  });

  final int updateId;
  final String deviceId;
  final String? firmwareId;
  final String productId;
  final String hwVersion;
  final String targetFwVersion;
  final String status;
  final String? messageId;
  final DateTime? createdAt;
  final DateTime? updatedAt;

  String get statusLabel => switch (status) {
    'pending' => '等待设备执行',
    'downloading' => '正在升级',
    'success' => '升级成功',
    'failed' => '升级失败',
    _ => status,
  };

  static CloudOtaUpdate? fromJson(Map<String, dynamic> j) {
    final updateId = j['update_id'];
    final deviceId = j['device_id'];
    final productId = j['product_id'];
    final hwVersion = j['hw_version'];
    final targetFwVersion = j['target_fw_version'];
    final status = j['status'];
    if (updateId is! num ||
        deviceId is! String ||
        productId is! String ||
        hwVersion is! String ||
        targetFwVersion is! String ||
        status is! String) {
      return null;
    }
    return CloudOtaUpdate(
      updateId: updateId.toInt(),
      deviceId: deviceId,
      firmwareId: j['firmware_id'] as String?,
      productId: productId,
      hwVersion: hwVersion,
      targetFwVersion: targetFwVersion,
      status: status,
      messageId: j['message_id'] as String?,
      createdAt: _parseDate(j['created_at']),
      updatedAt: _parseDate(j['updated_at']),
    );
  }
}

class CloudOtaCheckResult {
  const CloudOtaCheckResult({
    required this.dispatched,
    required this.published,
    required this.reason,
    this.target,
    this.update,
  });

  final bool dispatched;
  final bool published;
  final String reason;
  final CloudFirmware? target;
  final CloudOtaUpdate? update;

  String get reasonLabel => switch (reason) {
    'dispatched' => '已下发升级命令',
    'already_dispatched' => '已有升级任务',
    'no_rollout' => '暂无适用升级',
    'up_to_date' => '已是最新版本',
    'not_selected' => '灰度暂未命中',
    'firmware_missing' => '目标固件缺失',
    _ => reason,
  };

  static CloudOtaCheckResult? fromJson(Map<String, dynamic> j) {
    final reason = j['reason'];
    if (reason is! String) return null;
    final targetRaw = j['target'];
    final updateRaw = j['update'];
    return CloudOtaCheckResult(
      dispatched: j['dispatched'] == true,
      published: j['published'] == true,
      reason: reason,
      target: targetRaw is Map<String, dynamic>
          ? CloudFirmware.fromJson(targetRaw)
          : null,
      update: updateRaw is Map<String, dynamic>
          ? CloudOtaUpdate.fromJson(updateRaw)
          : null,
    );
  }
}

/// The identity returned by `POST /auth/register` and `POST /auth/login`.
class CloudAuth {
  const CloudAuth({
    required this.userId,
    required this.email,
    required this.token,
  });

  final String userId;
  final String email;
  final String token;

  static CloudAuth? fromJson(Map<String, dynamic> j) {
    final userId = j['user_id'];
    final token = j['token'];
    if (userId is! String || token is! String) return null;
    return CloudAuth(
      userId: userId,
      email: (j['email'] as String?) ?? '',
      token: token,
    );
  }
}

/// Thin REST client for the Phase B cloud service. Auth endpoints
/// (`/auth/*`) are anonymous; every other call is authorized with the
/// per-user bearer [token] returned by register/login.
class CloudClient {
  CloudClient({required this.baseUrl, this.token = '', http.Client? httpClient})
    : _http = httpClient ?? http.Client();

  final String baseUrl;
  final String token;
  final http.Client _http;

  static const Duration _timeout = Duration(seconds: 10);

  Map<String, String> get _headers => {
    'Content-Type': 'application/json',
    if (token.isNotEmpty) 'Authorization': 'Bearer $token',
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

  Future<CloudAuth> register(String email, String password) =>
      _authCall('/api/v1/auth/register', email, password);

  Future<CloudAuth> login(String email, String password) =>
      _authCall('/api/v1/auth/login', email, password);

  Future<CloudAuth> _authCall(
    String path,
    String email,
    String password,
  ) async {
    final response = await _send(
      () => _http.post(
        _uri(path),
        headers: const {'Content-Type': 'application/json'},
        body: jsonEncode({'email': email, 'password': password}),
      ),
    );
    _ensureOk(response);
    final decoded = jsonDecode(response.body);
    final auth = decoded is Map<String, dynamic>
        ? CloudAuth.fromJson(decoded)
        : null;
    if (auth == null) throw CloudApiException('云端返回了异常的响应');
    return auth;
  }

  Future<List<CloudDevice>> listDevices() async {
    final response = await _send(
      () => _http.get(_uri('/api/v1/me/devices'), headers: _headers),
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

  /// Claim an existing (auto-registered) device to the logged-in account.
  Future<CloudDevice> claimDevice(String deviceId, {bool force = false}) async {
    final response = await _send(
      () => _http.post(
        _uri('/api/v1/me/devices/${Uri.encodeComponent(deviceId)}/claim'),
        headers: _headers,
        body: jsonEncode({'force': force}),
      ),
    );
    _ensureOk(response);
    final decoded = jsonDecode(response.body);
    final device = decoded is Map<String, dynamic>
        ? CloudDevice.fromJson(decoded)
        : null;
    if (device == null) throw CloudApiException('云端返回了异常的响应');
    return device;
  }

  /// Remove (unclaim) a device from the logged-in account. The device stays
  /// registered on the cloud but no longer belongs to this account, so it
  /// disappears from the list and can be re-claimed later.
  Future<void> unclaimDevice(String deviceId) async {
    final response = await _send(
      () => _http.delete(
        _uri('/api/v1/me/devices/${Uri.encodeComponent(deviceId)}'),
        headers: _headers,
      ),
    );
    _ensureOk(response);
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
    return decoded is Map<String, dynamic>
        ? CloudShadow.fromJson(decoded)
        : null;
  }

  Future<List<CloudOtaUpdate>> listOtaUpdates(String deviceId) async {
    final response = await _send(
      () => _http.get(
        _uri('/api/v1/devices/${Uri.encodeComponent(deviceId)}/ota/updates'),
        headers: _headers,
      ),
    );
    _ensureOk(response);
    final decoded = jsonDecode(response.body);
    if (decoded is! List) return const [];
    return decoded
        .whereType<Map<String, dynamic>>()
        .map(CloudOtaUpdate.fromJson)
        .whereType<CloudOtaUpdate>()
        .toList();
  }

  Future<CloudOtaCheckResult> checkOta(
    String deviceId, {
    required String productId,
    required String hwVersion,
    required String fwVersion,
  }) async {
    final response = await _send(
      () => _http.post(
        _uri('/api/v1/devices/${Uri.encodeComponent(deviceId)}/ota/check'),
        headers: _headers,
        body: jsonEncode({
          'product_id': productId,
          'hw_version': hwVersion,
          'fw_version': fwVersion,
        }),
      ),
    );
    _ensureOk(response);
    final decoded = jsonDecode(response.body);
    final result = decoded is Map<String, dynamic>
        ? CloudOtaCheckResult.fromJson(decoded)
        : null;
    if (result == null) throw CloudApiException('云端返回了异常的 OTA 响应');
    return result;
  }

  /// Fetch every device owned by the logged-in account with its shadow.
  Future<List<CloudDeviceView>> listDeviceViews() async {
    final devices = await listDevices();
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
    final body = jsonEncode({'state': state, 'message_id': ?messageId});
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
    final code = response.statusCode;
    if (code >= 200 && code < 300) return;
    final detail = _detail(response);
    final message = switch (code) {
      401 => detail ?? '登录已失效，请重新登录',
      403 => detail ?? '无权访问该设备（可能属于其它账号）',
      404 => detail ?? '设备不存在（请确认设备已上线并上报云端）',
      409 => detail ?? '设备已被其它账号认领',
      _ => detail ?? 'HTTP $code: ${response.reasonPhrase ?? ''}',
    };
    throw CloudApiException(message, statusCode: code);
  }

  String? _detail(http.Response response) {
    try {
      final decoded = jsonDecode(response.body);
      final detail = decoded is Map<String, dynamic> ? decoded['detail'] : null;
      return detail is String && detail.isNotEmpty ? detail : null;
    } catch (_) {
      return null;
    }
  }

  void close() => _http.close();
}

DateTime? _parseDate(Object? value) =>
    value is String ? DateTime.tryParse(value) : null;
