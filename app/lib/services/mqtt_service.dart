import 'dart:async';
import 'dart:convert';

import 'package:mqtt_client/mqtt_client.dart';
import 'package:mqtt_client/mqtt_server_client.dart';

import '../config.dart';

/// Doorbell events published by the ESP32-S3 to `doorbell/<id>/event`.
enum DoorbellEvent { ringing, streamStart, streamStop, unknown }

/// Product type reported by firmware and used for device-specific UI.
enum DeviceType {
  doorbell,
  camera,
  gateway,
  lightBulb,
  ceilingLight,
  lightStrip,
  wallSwitch,
  socket,
  relay,
  curtainMotor,
  valve,
  doorLock,
  sensor,
  unknown;

  static DeviceType fromString(String? value) {
    switch (value) {
      case 'doorbell':
        return DeviceType.doorbell;
      case 'camera':
        return DeviceType.camera;
      case 'gateway':
        return DeviceType.gateway;
      case 'light':
      case 'light_bulb':
        return DeviceType.lightBulb;
      case 'ceiling_light':
        return DeviceType.ceilingLight;
      case 'light_strip':
        return DeviceType.lightStrip;
      case 'switch':
      case 'wall_switch':
        return DeviceType.wallSwitch;
      case 'socket':
        return DeviceType.socket;
      case 'relay':
        return DeviceType.relay;
      case 'curtain_motor':
        return DeviceType.curtainMotor;
      case 'valve':
        return DeviceType.valve;
      case 'door_lock':
        return DeviceType.doorLock;
      case 'sensor':
        return DeviceType.sensor;
      case null:
        return DeviceType.lightBulb;
      default:
        return DeviceType.unknown;
    }
  }

  bool get isControllable => switch (this) {
    DeviceType.lightBulb ||
    DeviceType.ceilingLight ||
    DeviceType.lightStrip ||
    DeviceType.wallSwitch ||
    DeviceType.socket ||
    DeviceType.relay ||
    DeviceType.curtainMotor ||
    DeviceType.valve ||
    DeviceType.doorLock => true,
    _ => false,
  };

  bool get isLight =>
      this == DeviceType.lightBulb ||
      this == DeviceType.ceilingLight ||
      this == DeviceType.lightStrip;

  String get label => switch (this) {
    DeviceType.doorbell => '智能门铃',
    DeviceType.camera => '监控摄像头',
    DeviceType.gateway => 'Mesh 网关',
    DeviceType.lightBulb => '智能灯泡',
    DeviceType.ceilingLight => '吸顶灯',
    DeviceType.lightStrip => '智能灯带',
    DeviceType.wallSwitch => '墙壁开关',
    DeviceType.socket => '智能插座',
    DeviceType.relay => '继电器',
    DeviceType.curtainMotor => '窗帘电机',
    DeviceType.valve => '智能阀门',
    DeviceType.doorLock => '智能门锁',
    DeviceType.sensor => '检测设备',
    DeviceType.unknown => '智能设备',
  };

  String stateLabel({required bool online, required bool on}) {
    if (!online) return '离线';
    return switch (this) {
      DeviceType.curtainMotor || DeviceType.valve => on ? '已打开' : '已关闭',
      DeviceType.doorLock => on ? '已上锁' : '已解锁',
      DeviceType.gateway || DeviceType.sensor => '在线',
      _ => on ? '已开启' : '已关闭',
    };
  }
}

/// A mesh device reported by the gateway on
/// `office/light/node/<id>/status`.
class MeshDevice {
  const MeshDevice({
    required this.id,
    required this.on,
    required this.online,
    required this.layer,
    required this.role,
    required this.updatedAt,
    this.type = DeviceType.lightBulb,
    this.name,
    this.value,
    this.colorHex,
  });

  final String id;
  final bool on;
  final bool online;
  final int layer;
  final String role; // "root" or "node"
  final DateTime updatedAt;
  final DeviceType type;

  /// Optional friendly name reported by the device; falls back to the id.
  final String? name;

  /// Optional sensor reading (e.g. "26.5°C"), for read-only devices.
  final String? value;

  /// Current RGB color ("#RRGGBB") for lights that support color.
  final String? colorHex;

  bool get isRoot => role == 'root';

  /// Short display name: reported name, else product type plus MAC suffix.
  String get displayName {
    if (name != null && name!.isNotEmpty) return name!;
    final dash = id.lastIndexOf('-');
    final suffix = dash >= 0 ? id.substring(dash + 1) : id;
    return '${type.label} $suffix';
  }

  static MeshDevice? fromJson(Map<String, dynamic> j) {
    final id = j['id'];
    if (id is! String || id.isEmpty) return null;
    return MeshDevice(
      id: id,
      on: (j['state'] ?? 'off') == 'on' || j['on'] == true,
      online: j['online'] == true,
      layer: (j['layer'] is num) ? (j['layer'] as num).toInt() : 0,
      role: (j['role'] as String?) ?? 'node',
      type: DeviceType.fromString(j['type'] as String?),
      name: j['name'] as String?,
      value: j['value']?.toString(),
      colorHex: j['color'] as String?,
      updatedAt: DateTime.now(),
    );
  }
}

/// Aggregate gateway (mesh root) status from `office/light/gateway/status`.
class GatewayStatus {
  const GatewayStatus({
    required this.online,
    required this.root,
    required this.layer,
    required this.nodes,
    required this.onlineNodes,
    required this.updatedAt,
  });

  final bool online;
  final String root;
  final int layer;
  final int nodes;
  final int onlineNodes;
  final DateTime updatedAt;

  static GatewayStatus? fromJson(Map<String, dynamic> j) {
    return GatewayStatus(
      online: j['online'] == true,
      root: (j['root'] as String?) ?? '—',
      layer: (j['layer'] is num) ? (j['layer'] as num).toInt() : 0,
      nodes: (j['nodes'] is num) ? (j['nodes'] as num).toInt() : 0,
      onlineNodes: (j['online_nodes'] is num)
          ? (j['online_nodes'] as num).toInt()
          : 0,
      updatedAt: DateTime.now(),
    );
  }
}

/// Thin wrapper around [MqttServerClient]. Exposes broadcast streams for the
/// doorbell signalling channel and the BLE-Mesh light network (per-node device
/// list, gateway status, and the legacy demo status text).
class MqttService {
  MqttServerClient? _client;

  final StreamController<bool> _connection = StreamController<bool>.broadcast();
  final StreamController<DoorbellEvent> _events =
      StreamController<DoorbellEvent>.broadcast();
  final StreamController<String> _lightStatus =
      StreamController<String>.broadcast();
  final StreamController<List<MeshDevice>> _devices =
      StreamController<List<MeshDevice>>.broadcast();
  final StreamController<GatewayStatus?> _gateway =
      StreamController<GatewayStatus?>.broadcast();

  final Map<String, MeshDevice> _deviceMap = {};
  GatewayStatus? _lastGateway;

  Stream<bool> get connection => _connection.stream;
  Stream<DoorbellEvent> get doorbellEvents => _events.stream;
  Stream<String> get lightStatus => _lightStatus.stream;

  /// Emits the full device list whenever any node's status changes.
  Stream<List<MeshDevice>> get devices => _devices.stream;
  Stream<GatewayStatus?> get gateway => _gateway.stream;

  List<MeshDevice> get devicesSnapshot => _sortedDevices();
  GatewayStatus? get gatewaySnapshot => _lastGateway;

  bool get isConnected =>
      _client?.connectionStatus?.state == MqttConnectionState.connected;

  Future<void> connect() async {
    final clientId =
        'flutter_doorbell_${DateTime.now().millisecondsSinceEpoch}';
    final client = MqttServerClient.withPort(
      AppConfig.mqttHost,
      clientId,
      AppConfig.mqttPort,
    );
    client.logging(on: false);
    client.keepAlivePeriod = 30;
    client.autoReconnect = true;
    client.onConnected = () => _connection.add(true);
    client.onDisconnected = () => _connection.add(false);
    client.onSubscribed = (_) {};
    client.connectionMessage = MqttConnectMessage()
        .withClientIdentifier(clientId)
        .startClean();

    _client = client;
    try {
      await client.connect(AppConfig.mqttUsername, AppConfig.mqttPassword);
    } catch (_) {
      client.disconnect();
      rethrow;
    }

    client.subscribe(AppConfig.doorbellEventTopic, MqttQos.atLeastOnce);
    client.subscribe(AppConfig.lightStatusTopic, MqttQos.atLeastOnce);
    client.subscribe(AppConfig.lightGatewayStatusTopic, MqttQos.atLeastOnce);
    client.subscribe(AppConfig.lightNodeStatusFilter, MqttQos.atLeastOnce);
    client.subscribe(AppConfig.doorbellStatusFilter, MqttQos.atLeastOnce);
    client.subscribe(AppConfig.gatewayStatusFilter, MqttQos.atLeastOnce);
    client.updates?.listen(_onMessage);
  }

  void _onMessage(List<MqttReceivedMessage<MqttMessage>> batch) {
    for (final received in batch) {
      final message = received.payload as MqttPublishMessage;
      final payload = MqttPublishPayload.bytesToStringAsString(
        message.payload.message,
      );
      final topic = received.topic;

      if (topic == AppConfig.doorbellEventTopic) {
        _events.add(_parseEvent(payload));
      } else if (topic == AppConfig.lightStatusTopic) {
        _lightStatus.add(payload);
      } else if (topic == AppConfig.lightGatewayStatusTopic) {
        _handleGateway(payload);
      } else if (AppConfig.lightNodeIdFromStatusTopic(topic) != null) {
        _handleNodeStatus(payload);
      } else if (topic.startsWith('doorbell/') && topic.endsWith('/status')) {
        _handleNodeStatus(payload);
      } else if (AppConfig.gatewayIdFromStatusTopic(topic) != null) {
        _handleFarmelyGateway(AppConfig.gatewayIdFromStatusTopic(topic)!, payload);
      }
    }
  }

  void _handleGateway(String payload) {
    final map = _tryDecode(payload);
    if (map == null) return;
    final status = GatewayStatus.fromJson(map);
    if (status == null) return;
    _lastGateway = status;
    _gateway.add(status);
    if (status.root != '\u2014') {
      _deviceMap[status.root] = MeshDevice(
        id: status.root,
        on: false,
        online: status.online,
        layer: status.layer,
        role: 'root',
        type: DeviceType.gateway,
        name: 'Mesh \u7f51\u5173',
        updatedAt: DateTime.now(),
      );
      _devices.add(_sortedDevices());
    }
  }

  void _handleFarmelyGateway(String gatewayId, String payload) {
    final map = _tryDecode(payload);
    if (map == null) return;
    final data = map['data'];
    if (data is! Map<String, dynamic>) return;
    _deviceMap[gatewayId] = MeshDevice(
      id: gatewayId,
      on: false,
      online: data['online'] == true,
      layer: 0,
      role: 'root',
      type: DeviceType.gateway,
      name: 'Mesh \u7f51\u5173',
      updatedAt: DateTime.now(),
    );
    _devices.add(_sortedDevices());
  }

  void _handleNodeStatus(String payload) {
    final map = _tryDecode(payload);
    if (map == null) return;
    final device = MeshDevice.fromJson(map);
    if (device == null) return;
    _deviceMap[device.id] = device;
    _devices.add(_sortedDevices());
  }

  List<MeshDevice> _sortedDevices() {
    final list = _deviceMap.values.toList();
    // Root first, then online before offline, then by id.
    list.sort((a, b) {
      if (a.isRoot != b.isRoot) return a.isRoot ? -1 : 1;
      if (a.online != b.online) return a.online ? -1 : 1;
      return a.id.compareTo(b.id);
    });
    return list;
  }

  Map<String, dynamic>? _tryDecode(String payload) {
    try {
      final decoded = jsonDecode(payload);
      return decoded is Map<String, dynamic> ? decoded : null;
    } catch (_) {
      return null;
    }
  }

  DoorbellEvent _parseEvent(String payload) {
    if (payload.contains('ringing')) return DoorbellEvent.ringing;
    if (payload.contains('stream_start')) return DoorbellEvent.streamStart;
    if (payload.contains('stream_stop')) return DoorbellEvent.streamStop;
    return DoorbellEvent.unknown;
  }

  void _publish(String topic, String payload) {
    final builder = MqttClientPayloadBuilder()..addString(payload);
    _client?.publishMessage(topic, MqttQos.atLeastOnce, builder.payload!);
  }

  /// Doorbell commands: "hangup" ends the call, "snapshot" asks for a still.
  void sendDoorbellCommand(String command) =>
      _publish(AppConfig.doorbellCmdTopic, command);

  /// Legacy global on/off (office/light/demo/cmd).
  void setLight(bool on) =>
      _publish(AppConfig.lightCmdTopic, on ? 'on' : 'off');

  /// Turn the whole mesh on/off (office/light/all/cmd).
  void setAllLights(bool on) =>
      _publish(AppConfig.lightAllCmdTopic, on ? 'on' : 'off');

  /// Address a single node (`office/light/node/<id>/cmd`).
  void setNodeLight(String id, bool on) =>
      _publish(AppConfig.lightNodeCmdTopic(id), on ? 'on' : 'off');

  /// Set the RGB color of a color-capable light ("#RRGGBB").
  void setNodeColor(String id, String hex) =>
      _publish(AppConfig.lightNodeCmdTopic(id), 'color:$hex');

  void dispose() {
    _client?.disconnect();
    _connection.close();
    _events.close();
    _lightStatus.close();
    _devices.close();
    _gateway.close();
  }
}
