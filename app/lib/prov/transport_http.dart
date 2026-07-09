import 'dart:async';
import 'dart:typed_data';

import 'package:http/http.dart' as http;

import 'transport.dart';

/// HTTP transport to the board's SoftAP protocomm endpoints (default 192.168.4.1).
class TransportHTTP implements Transport {
  TransportHTTP({
    required this.hostname,
    this.timeout = const Duration(seconds: 10),
  }) {
    if (hostname.trim().isEmpty) {
      throw FormatException('hostname must not be empty');
    }
    headers['Content-type'] = 'application/x-www-form-urlencoded';
    headers['Accept'] = 'text/plain';
  }

  final String hostname;
  final Duration timeout;
  final Map<String, String> headers = {};
  final http.Client client = http.Client();

  @override
  Future<bool> connect() async => true;

  @override
  Future<void> disconnect() async => client.close();

  void _updateCookie(http.Response response) {
    final String? rawCookie = response.headers['set-cookie'];
    if (rawCookie != null) {
      final int index = rawCookie.indexOf(';');
      headers['cookie'] =
          (index == -1) ? rawCookie : rawCookie.substring(0, index);
    }
  }

  @override
  Future<Uint8List?> sendReceive(String epName, Uint8List data) async {
    final response = await client
        .post(Uri.http(hostname, '/$epName'), headers: headers, body: data)
        .timeout(timeout);
    _updateCookie(response);
    if (response.statusCode == 200) {
      return response.bodyBytes;
    }
    throw StateError(
        "ESP device did not respond (HTTP ${response.statusCode})");
  }
}
