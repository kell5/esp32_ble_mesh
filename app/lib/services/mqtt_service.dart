import 'dart:async';
import 'dart:convert';

import 'package:mqtt_client/mqtt_client.dart';
import 'package:mqtt_client/mqtt_server_client.dart';

import '../config.dart';

/// Doorbell events published by the ESP32-S3 to `doorbell/<id>/event`.
enum DoorbellEvent { ringing, streamStart, streamStop, unknown }

/// Broad category of a mesh device, used to pick the card/control to render.
/// Only [light] is produced today; the others are placeholders so new device
/// classes can be added by having the firmware report `"type"` without any
/// App refactor.
enum DeviceType {
  light, // on/off luminaire (current)
  switch_, // generic on/off actuator (relay / socket)
  sensor, // read-only measurement (temp / humidity / door contact)
  unknown;

  static DeviceType fromString(String? s) {
    switch (s) {
      case 'light':
        return DeviceType.light;
      case 'switch':
      case 'relay':
      case 'socket':
        return DeviceType.switch_;
      case 'sensor':
        return DeviceType.sensor;
      case null:
        return DeviceType.light; // default: legacy nodes are lights
      default:
        return DeviceType.unknown;
    }
  }

  /// True for devices the user can toggle on/off.
  bool get isControllable =>
      this == DeviceType.light || this == DeviceType.switch_;
}

/// A single BLE-Mesh device as reported by the gateway (root) on
/// `office/light/node/<id>/status`. Today every node is a [DeviceType.light];
/// the model carries [type]/[value]/[name] so future actuators and sensors
/// display without changing this class.
class LightDevice {
  const LightDevice({
    required this.id,
    required this.on,
    required this.online,
    required this.layer,
    required this.role,
    required this.updatedAt,
    this.type = DeviceType.light,
    this.name,
    this.value,
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

  bool get isRoot => role == 'root';

  /// Short display name: reported name, else the hex suffix of the id.
  String get displayName {
    if (name != null && name!.isNotEmpty) return name!;
    final dash = id.lastIndexOf('-');
    return dash >= 0 ? '灯 ${id.substring(dash + 1)}' : id;
  }

  static LightDevice? fromJson(Map<String, dynamic> j) {
    final id = j['id'];
    if (id is! String || id.isEmpty) return null;
    return LightDevice(
      id: id,
      on: (j['state'] ?? 'off') == 'on' || j['on'] == true,
      online: j['online'] == true,
      layer: (j['layer'] is num) ? (j['layer'] as num).toInt() : 0,
      role: (j['role'] as String?) ?? 'node',
      type: DeviceType.fromString(j['type'] as String?),
      name: j['name'] as String?,
      value: j['value']?.toString(),
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
      onlineNodes:
          (j['online_nodes'] is num) ? (j['online_nodes'] as num).toInt() : 0,
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
  final StreamController<List<LightDevice>> _devices =
      StreamController<List<LightDevice>>.broadcast();
  final StreamController<GatewayStatus?> _gateway =
      StreamController<GatewayStatus?>.broadcast();

  final Map<String, LightDevice> _deviceMap = {};
  GatewayStatus? _lastGateway;

  Stream<bool> get connection => _connection.stream;
  Stream<DoorbellEvent> get doorbellEvents => _events.stream;
  Stream<String> get lightStatus => _lightStatus.stream;

  /// Emits the full device list whenever any node's status changes.
  Stream<List<LightDevice>> get devices => _devices.stream;
  Stream<GatewayStatus?> get gateway => _gateway.stream;

  List<LightDevice> get devicesSnapshot => _sortedDevices();
  GatewayStatus? get gatewaySnapshot => _lastGateway;

  bool get isConnected =>
      _client?.connectionStatus?.state == MqttConnectionState.connected;

  Future<void> connect() async {
    final clientId = 'flutter_doorbell_${DateTime.now().millisecondsSinceEpoch}';
    final client =
        MqttServerClient.withPort(AppConfig.mqttHost, clientId, AppConfig.mqttPort);
    client.logging(on: false);
    client.keepAlivePeriod = 30;
    client.autoReconnect = true;
    client.onConnected = () => _connection.add(true);
    client.onDisconnected = () => _connection.add(false);
    client.onSubscribed = (_) {};
    client.connectionMessage =
        MqttConnectMessage().withClientIdentifier(clientId).startClean();

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
    client.updates?.listen(_onMessage);
  }

  void _onMessage(List<MqttReceivedMessage<MqttMessage>> batch) {
    for (final received in batch) {
      final message = received.payload as MqttPublishMessage;
      final payload =
          MqttPublishPayload.bytesToStringAsString(message.payload.message);
      final topic = received.topic;

      if (topic == AppConfig.doorbellEventTopic) {
        _events.add(_parseEvent(payload));
      } else if (topic == AppConfig.lightStatusTopic) {
        _lightStatus.add(payload);
      } else if (topic == AppConfig.lightGatewayStatusTopic) {
        _handleGateway(payload);
      } else if (AppConfig.lightNodeIdFromStatusTopic(topic) != null) {
        _handleNodeStatus(payload);
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
  }

  void _handleNodeStatus(String payload) {
    final map = _tryDecode(payload);
    if (map == null) return;
    final device = LightDevice.fromJson(map);
    if (device == null) return;
    _deviceMap[device.id] = device;
    _devices.add(_sortedDevices());
  }

  List<LightDevice> _sortedDevices() {
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

  void dispose() {
    _client?.disconnect();
    _connection.close();
    _events.close();
    _lightStatus.close();
    _devices.close();
    _gateway.close();
  }
}
