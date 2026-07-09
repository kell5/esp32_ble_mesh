import 'dart:async';
import 'dart:typed_data';

import 'package:flutter/cupertino.dart';
import 'package:http/http.dart' as http;

/// Displays an MJPEG (multipart/x-mixed-replace) HTTP stream by scanning the
/// byte stream for JPEG frames (SOI 0xFFD8 … EOI 0xFFD9) and rendering each
/// complete frame. Pure Dart — no native video plugins required.
class MjpegView extends StatefulWidget {
  const MjpegView({super.key, required this.url});

  final String url;

  @override
  State<MjpegView> createState() => _MjpegViewState();
}

class _MjpegViewState extends State<MjpegView> {
  http.Client? _client;
  StreamSubscription<List<int>>? _sub;
  Uint8List? _frame;
  String? _error;
  final List<int> _buffer = <int>[];

  @override
  void initState() {
    super.initState();
    _start();
  }

  @override
  void didUpdateWidget(covariant MjpegView oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (oldWidget.url != widget.url) {
      _stop();
      setState(() {
        _frame = null;
        _error = null;
        _buffer.clear();
      });
      _start();
    }
  }

  Future<void> _start() async {
    final client = http.Client();
    _client = client;
    try {
      final request = http.Request('GET', Uri.parse(widget.url));
      final response = await client.send(request);
      if (response.statusCode != 200) {
        if (mounted) setState(() => _error = 'HTTP ${response.statusCode}');
        return;
      }
      _sub = response.stream.listen(
        _onData,
        onError: (Object e) {
          if (mounted) setState(() => _error = e.toString());
        },
        cancelOnError: true,
      );
    } catch (e) {
      if (mounted) setState(() => _error = e.toString());
    }
  }

  void _onData(List<int> chunk) {
    _buffer.addAll(chunk);
    while (true) {
      final start = _indexOfMarker(0xD8, 0);
      if (start < 0) {
        // Keep only a trailing byte in case a 0xFF was split across chunks.
        if (_buffer.length > 1) {
          _buffer.removeRange(0, _buffer.length - 1);
        }
        break;
      }
      final end = _indexOfMarker(0xD9, start + 2);
      if (end < 0) {
        if (start > 0) _buffer.removeRange(0, start);
        break;
      }
      final frame = Uint8List.fromList(_buffer.sublist(start, end + 2));
      _buffer.removeRange(0, end + 2);
      if (mounted) setState(() => _frame = frame);
    }
  }

  /// Finds a JPEG marker (0xFF followed by [second]) starting at [from].
  int _indexOfMarker(int second, int from) {
    for (int i = from; i + 1 < _buffer.length; i++) {
      if (_buffer[i] == 0xFF && _buffer[i + 1] == second) return i;
    }
    return -1;
  }

  void _stop() {
    _sub?.cancel();
    _sub = null;
    _client?.close();
    _client = null;
  }

  @override
  void dispose() {
    _stop();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    if (_error != null) {
      return Center(
        child: Padding(
          padding: const EdgeInsets.all(24),
          child: Text(
            '无法连接摄像头\n${widget.url}\n$_error',
            textAlign: TextAlign.center,
            style: const TextStyle(color: CupertinoColors.systemGrey),
          ),
        ),
      );
    }
    if (_frame == null) {
      return const Center(child: CupertinoActivityIndicator(color: CupertinoColors.white));
    }
    return Image.memory(_frame!, gaplessPlayback: true, fit: BoxFit.contain);
  }
}
