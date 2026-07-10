/// Central configuration for the doorbell + mesh-light control app.
///
/// Server endpoints match the Phase-2 deployment (camera_stream/README.md,
/// server_relay/README.md) and the existing BLE-Mesh light controller
/// (internal_communication/).
class AppConfig {
  // ---- MQTT broker (shared with the mesh light controller) ----
  static const String mqttHost = '121.40.131.194';
  static const int mqttPort = 1883;
  static const String mqttUsername = 'yskj';
  static const String mqttPassword = 'yskj@123';

  // ---- Doorbell signalling ----
  static const String doorbellId = 'door-001';
  static String get doorbellEventTopic => 'doorbell/$doorbellId/event';
  static String get doorbellCmdTopic => 'doorbell/$doorbellId/cmd';

  // ---- Video: MJPEG-over-HTTP (OV3660 only produces JPEG) ----
  // A1 局域网直连：手机与板子同一 WiFi，直接拉板子的 :81/stream。
  // A2 服务器中继：板子推帧到公网中继，App 拉 /stream/<id>，可远程观看。
  static const int cameraPort = 81; // 板子本机 MJPEG 端口 (A1)
  static String cameraStreamUrl(String host) =>
      'http://$host:$cameraPort/stream';

  static const String relayHost = '114.55.208.72';
  static const int relayPort = 8090;
  static String get relayStreamUrl =>
      'http://$relayHost:$relayPort/stream/$doorbellId';

  // 局域网直连的示例地址（用户在设置里把 IP 换成板子实际 LAN IP）。
  static String get lanStreamUrlExample => cameraStreamUrl('192.168.1.100');

  // 默认走中继（远程可用）；局域网可在设置里一键切换。
  static String get defaultStreamUrl => relayStreamUrl;

  // ---- Unified BLE/SoftAP WiFi provisioning ----
  static const String doorbellProvPrefix = 'Doorbell-';
  static const String gatewayProvPrefix = 'Gateway-';
  static const String doorbellProvPop = 'doorbell1234';
  static const String gatewayProvPop = 'gateway1234';
  static const String provHost = '192.168.4.1';

  // ---- BLE-Mesh light control (matches internal_communication/ firmware) ----
  // Legacy global on/off (kept for backward compatibility).
  static const String lightCmdTopic = 'office/light/demo/cmd';
  static const String lightStatusTopic = 'office/light/demo/status';

  // Group command: turn the whole mesh on/off.
  static const String lightAllCmdTopic = 'office/light/all/cmd';

  // Root/gateway aggregate status: {online, root, layer, nodes, online_nodes, heap}.
  static const String lightGatewayStatusTopic = 'office/light/gateway/status';

  // Per-node status (retained) published by the root:
  // office/light/node/<id>/status -> {id, online, state, layer, role}.
  static const String lightNodeStatusFilter = 'office/light/node/+/status';
  static const String _lightNodePrefix = 'office/light/node/';

  // Per-node command: office/light/node/<id>/cmd <- "on"/"off".
  static String lightNodeCmdTopic(String id) => '$_lightNodePrefix$id/cmd';

  // Extract "<id>" from office/light/node/<id>/status (null if not a match).
  static String? lightNodeIdFromStatusTopic(String topic) {
    if (!topic.startsWith(_lightNodePrefix) || !topic.endsWith('/status')) {
      return null;
    }
    final id = topic.substring(
      _lightNodePrefix.length,
      topic.length - '/status'.length,
    );
    return id.isEmpty ? null : id;
  }
}
