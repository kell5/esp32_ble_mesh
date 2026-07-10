import 'dart:typed_data';

import 'package:flutter_blue_plus/flutter_blue_plus.dart';

import 'transport.dart';

class TransportBLE implements Transport {
  TransportBLE(
    this.device, {
    this.serviceUuid = defaultServiceUuid,
    this.endpointTable = defaultEndpoints,
  });

  static const String defaultServiceUuid =
      '021a9004-0382-4aea-bff4-6b3f1c5adfb4';
  static const Map<String, String> defaultEndpoints = {
    'prov-scan': 'ff50',
    'prov-session': 'ff51',
    'prov-config': 'ff52',
    'proto-ver': 'ff53',
    'custom-data': 'ff54',
  };

  final BluetoothDevice device;
  final String serviceUuid;
  final Map<String, String> endpointTable;
  final Map<String, BluetoothCharacteristic> _characteristics = {};

  String _characteristicUuid(String endpointHex) =>
      '${serviceUuid.substring(0, 4)}$endpointHex${serviceUuid.substring(8)}';

  @override
  Future<bool> connect() async {
    if (device.isConnected) {
      await device.disconnect();
    }
    await device.connect(autoConnect: false);
    try {
      await device.requestMtu(256);
    } catch (_) {}

    final services = await device.discoverServices();
    final wanted = <String, String>{
      for (final endpoint in endpointTable.entries)
        _characteristicUuid(endpoint.value).toLowerCase(): endpoint.key,
    };
    _characteristics.clear();
    for (final service in services) {
      for (final characteristic in service.characteristics) {
        final endpoint = wanted[characteristic.uuid.toString().toLowerCase()];
        if (endpoint != null) {
          _characteristics[endpoint] = characteristic;
        }
      }
    }
    return _characteristics.keys.toSet().containsAll(const {
      'prov-session',
      'prov-scan',
      'prov-config',
    });
  }

  @override
  Future<void> disconnect() async {
    if (device.isConnected) {
      await device.disconnect();
    }
    _characteristics.clear();
  }

  @override
  Future<Uint8List?> sendReceive(String epName, Uint8List data) async {
    final characteristic = _characteristics[epName];
    if (characteristic == null) {
      throw StateError('未找到 BLE 配网端点：$epName');
    }
    if (data.isNotEmpty) {
      await characteristic.write(data, withoutResponse: false);
    }
    return Uint8List.fromList(await characteristic.read());
  }
}
