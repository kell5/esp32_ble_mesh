import 'dart:async';
import 'dart:io';

import 'package:flutter/cupertino.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import 'package:permission_handler/permission_handler.dart';

import '../config.dart';
import '../prov/connection_models.dart';
import '../prov/provisioning.dart';
import '../prov/security1.dart';
import '../prov/transport.dart';
import '../prov/transport_ble.dart';
import '../prov/transport_http.dart';

enum _ProvisioningTransport { ble, softAp }

class ProvisioningPage extends StatefulWidget {
  const ProvisioningPage({super.key});

  @override
  State<ProvisioningPage> createState() => _ProvisioningPageState();
}

class _ProvisioningPageState extends State<ProvisioningPage> {
  final _ssid = TextEditingController();
  final _password = TextEditingController();
  final _pop = TextEditingController(text: AppConfig.doorbellProvPop);
  final _host = TextEditingController(text: AppConfig.provHost);

  _ProvisioningTransport _transport = _ProvisioningTransport.ble;
  StreamSubscription<List<ScanResult>>? _scanSubscription;
  List<ScanResult> _devices = const [];
  BluetoothDevice? _device;
  bool _busy = false;
  bool _bleScanning = false;
  bool _obscure = true;
  String _status = '';

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addPostFrameCallback((_) => _scanBleDevices());
  }

  @override
  void dispose() {
    _scanSubscription?.cancel();
    FlutterBluePlus.stopScan();
    _ssid.dispose();
    _password.dispose();
    _pop.dispose();
    _host.dispose();
    super.dispose();
  }

  void _log(String message) {
    if (mounted) setState(() => _status = message);
  }

  String _deviceName(BluetoothDevice device) {
    final name = device.advName.isNotEmpty
        ? device.advName
        : device.platformName;
    return name.isEmpty ? device.remoteId.str : name;
  }

  Future<bool> _requestBlePermissions() async {
    if (Platform.isAndroid) {
      final statuses = await [
        Permission.bluetoothScan,
        Permission.bluetoothConnect,
        Permission.locationWhenInUse,
      ].request();
      return statuses[Permission.bluetoothScan]!.isGranted &&
          statuses[Permission.bluetoothConnect]!.isGranted;
    }
    return (await Permission.bluetooth.request()).isGranted;
  }

  Future<void> _scanBleDevices() async {
    if (_bleScanning || _busy) return;
    setState(() {
      _bleScanning = true;
      _devices = const [];
      _status = '正在搜索附近处于配网模式的设备…';
    });
    try {
      if (!await _requestBlePermissions()) {
        _log('需要蓝牙权限才能搜索设备。请在系统设置中允许后重试。');
        return;
      }
      if (!await FlutterBluePlus.isSupported) {
        _log('此手机不支持蓝牙低功耗。');
        return;
      }
      if (FlutterBluePlus.adapterStateNow != BluetoothAdapterState.on) {
        if (Platform.isAndroid) {
          await FlutterBluePlus.turnOn();
        }
        await FlutterBluePlus.adapterState
            .where((state) => state == BluetoothAdapterState.on)
            .first
            .timeout(const Duration(seconds: 10));
      }

      await _scanSubscription?.cancel();
      _scanSubscription = FlutterBluePlus.scanResults.listen((results) {
        final found = [...results]..sort((a, b) => b.rssi.compareTo(a.rssi));
        if (mounted) setState(() => _devices = found);
      });
      await FlutterBluePlus.startScan(
        withServices: [Guid(TransportBLE.defaultServiceUuid)],
        timeout: const Duration(seconds: 8),
      );
      await Future<void>.delayed(const Duration(seconds: 8));
      if (mounted && _devices.isEmpty) {
        _log('未发现设备。请让设备进入配网模式后重新搜索。');
      } else if (mounted) {
        _log('请选择要添加的设备。');
      }
    } on TimeoutException {
      _log('蓝牙未开启，请开启后重试。');
    } catch (error) {
      _log('搜索设备失败：$error');
    } finally {
      await FlutterBluePlus.stopScan();
      if (mounted) setState(() => _bleScanning = false);
    }
  }

  void _selectDevice(BluetoothDevice device) {
    final name = _deviceName(device);
    setState(() {
      _device = device;
      _pop.text = name.startsWith(AppConfig.gatewayProvPrefix)
          ? AppConfig.gatewayProvPop
          : AppConfig.doorbellProvPop;
      _status = '已选择 $name，可扫描该设备附近的 WiFi。';
    });
  }

  Provisioning _newSession() {
    final Transport transport;
    if (_transport == _ProvisioningTransport.ble) {
      final device = _device;
      if (device == null) throw StateError('请先选择设备。');
      transport = TransportBLE(device);
    } else {
      transport = TransportHTTP(hostname: _host.text.trim());
    }
    return Provisioning(
      transport: transport,
      security: Security1(pop: _pop.text),
    );
  }

  Future<void> _scanWifi() async {
    if (_busy) return;
    setState(() => _busy = true);
    Provisioning? prov;
    try {
      prov = _newSession();
      _log('正在连接设备并建立安全会话…');
      if (!await prov.establishSession()) {
        _log('会话建立失败，请确认设备仍处于配网模式且配网口令正确。');
        return;
      }
      _log('正在扫描设备附近的 WiFi…');
      final list = await prov.startScanWiFi();
      if (!mounted) return;
      if (list == null || list.isEmpty) {
        _log('未扫描到 WiFi，可手动输入名称。');
        return;
      }
      _log('');
      await _pickSsid(list);
    } catch (error) {
      _log('扫描 WiFi 失败：$error');
    } finally {
      await prov?.dispose();
      if (mounted) setState(() => _busy = false);
    }
  }

  Future<void> _pickSsid(List<Map<String, dynamic>> list) async {
    final seen = <String>{};
    final items = <Map<String, dynamic>>[];
    for (final entry in list) {
      final ssid = (entry['ssid'] ?? '').toString();
      if (ssid.isEmpty || !seen.add(ssid)) continue;
      items.add(entry);
    }
    final chosen = await showCupertinoModalPopup<String>(
      context: context,
      builder: (context) => CupertinoActionSheet(
        title: const Text('选择 WiFi'),
        actions: [
          for (final entry in items)
            CupertinoActionSheetAction(
              onPressed: () =>
                  Navigator.of(context).pop(entry['ssid'].toString()),
              child: Text('${entry['ssid']}   (${entry['rssi']}dBm)'),
            ),
        ],
        cancelButton: CupertinoActionSheetAction(
          onPressed: () => Navigator.of(context).pop(),
          child: const Text('取消'),
        ),
      ),
    );
    if (chosen != null) _ssid.text = chosen;
  }

  Future<void> _provision() async {
    if (_busy) return;
    if (_ssid.text.trim().isEmpty) {
      _log('请填写设备要连接的 WiFi 名称。');
      return;
    }
    setState(() => _busy = true);
    Provisioning? prov;
    try {
      prov = _newSession();
      _log('正在建立安全会话…');
      if (!await prov.establishSession()) {
        _log('会话建立失败，请确认设备仍处于配网模式且配网口令正确。');
        return;
      }
      _log('正在下发 WiFi 配置…');
      if (!await prov.sendWifiConfig(
        ssid: _ssid.text.trim(),
        password: _password.text,
      )) {
        _log('下发配置失败。');
        return;
      }
      if (!await prov.applyWifiConfig()) {
        _log('应用配置失败。');
        return;
      }
      _log('设备正在连接 WiFi，请稍候…');
      for (var second = 1; second <= 20; second++) {
        await Future<void>.delayed(const Duration(seconds: 1));
        try {
          final status = await prov.getStatus();
          if (status.state == WifiConnectionState.Connected) {
            _log('配网成功！设备已连上「${_ssid.text.trim()}」。');
            if (mounted) await _showDone();
            return;
          }
          if (status.state == WifiConnectionState.ConnectionFailed) {
            final reason =
                status.failedReason == WifiConnectFailedReason.AuthError
                ? 'WiFi 密码错误'
                : '找不到该 WiFi（可能是 5G 或超出范围）';
            _log('配网失败：$reason。');
            return;
          }
          _log('设备正在连接 WiFi…（${second}s）');
        } catch (_) {
          // Both BLE and the gateway SoftAP tear down the provisioning
          // transport once credentials are saved, so a failing status read
          // after a few seconds means the device accepted the config.
          if (second >= 3) {
            _log('设备已保存配置并退出配网模式，请稍后在首页确认上线。');
            if (mounted) await _showDone();
            return;
          }
        }
      }
      _log('等待超时，设备可能仍在连接，请稍后在首页确认是否上线。');
    } catch (error) {
      _log('配网出错：$error');
    } finally {
      await prov?.dispose();
      if (mounted) setState(() => _busy = false);
    }
  }

  Future<void> _showDone() async {
    await showCupertinoDialog<void>(
      context: context,
      builder: (context) => CupertinoAlertDialog(
        title: const Text('配网完成'),
        content: const Text('设备已收到 WiFi 配置，回到首页后会自动发现并显示在线状态。'),
        actions: [
          CupertinoDialogAction(
            isDefaultAction: true,
            onPressed: () {
              Navigator.of(context).pop();
              Navigator.of(this.context).pop();
            },
            child: const Text('完成'),
          ),
        ],
      ),
    );
  }

  @override
  Widget build(BuildContext context) {
    return CupertinoPageScaffold(
      navigationBar: const CupertinoNavigationBar(middle: Text('添加设备')),
      child: SafeArea(
        child: ListView(
          padding: const EdgeInsets.all(16),
          children: [
            CupertinoSlidingSegmentedControl<_ProvisioningTransport>(
              groupValue: _transport,
              children: const {
                _ProvisioningTransport.ble: Padding(
                  padding: EdgeInsets.symmetric(horizontal: 18),
                  child: Text('蓝牙搜索'),
                ),
                _ProvisioningTransport.softAp: Padding(
                  padding: EdgeInsets.symmetric(horizontal: 18),
                  child: Text('热点兼容'),
                ),
              },
              onValueChanged: (value) {
                if (_busy || value == null) return;
                setState(() {
                  _transport = value;
                  _status = '';
                  if (value == _ProvisioningTransport.softAp) {
                    // SoftAP is the gateway provisioning flow.
                    _pop.text = AppConfig.gatewayProvPop;
                    _host.text = AppConfig.provHost;
                  } else {
                    final device = _device;
                    _pop.text =
                        (device != null &&
                            _deviceName(
                              device,
                            ).startsWith(AppConfig.gatewayProvPrefix))
                        ? AppConfig.gatewayProvPop
                        : AppConfig.doorbellProvPop;
                  }
                });
              },
            ),
            const SizedBox(height: 16),
            if (_transport == _ProvisioningTransport.ble)
              _bleDeviceCard()
            else
              _softApCard(),
            const SizedBox(height: 16),
            _field(
              '要连接的 WiFi',
              _ssid,
              placeholder: '2.4G WiFi 名称',
              trailing: CupertinoButton(
                padding: EdgeInsets.zero,
                onPressed: _busy ? null : _scanWifi,
                child: const Text('扫描'),
              ),
            ),
            _field(
              'WiFi 密码',
              _password,
              placeholder: '留空表示开放网络',
              obscure: _obscure,
              trailing: CupertinoButton(
                padding: EdgeInsets.zero,
                onPressed: () => setState(() => _obscure = !_obscure),
                child: Icon(
                  _obscure ? CupertinoIcons.eye : CupertinoIcons.eye_slash,
                ),
              ),
            ),
            _field('配网口令 (PoP)', _pop),
            if (_transport == _ProvisioningTransport.softAp)
              _field('设备热点地址', _host, placeholder: '192.168.4.1'),
            const SizedBox(height: 8),
            CupertinoButton.filled(
              onPressed: _busy ? null : _provision,
              child: _busy
                  ? const CupertinoActivityIndicator()
                  : const Text('连接设备'),
            ),
            if (_status.isNotEmpty) ...[
              const SizedBox(height: 16),
              Text(
                _status,
                style: const TextStyle(color: CupertinoColors.systemGrey),
              ),
            ],
          ],
        ),
      ),
    );
  }

  Widget _bleDeviceCard() {
    return Container(
      padding: const EdgeInsets.all(14),
      decoration: BoxDecoration(
        color: CupertinoColors.systemGrey6,
        borderRadius: BorderRadius.circular(14),
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(
            children: [
              const Expanded(
                child: Text(
                  '附近设备',
                  style: TextStyle(fontWeight: FontWeight.w700, fontSize: 16),
                ),
              ),
              CupertinoButton(
                padding: EdgeInsets.zero,
                onPressed: _bleScanning ? null : _scanBleDevices,
                child: _bleScanning
                    ? const CupertinoActivityIndicator()
                    : const Text('重新搜索'),
              ),
            ],
          ),
          if (_devices.isEmpty && !_bleScanning)
            const Text(
              '让门铃或网关进入配网模式后搜索。',
              style: TextStyle(color: CupertinoColors.systemGrey),
            ),
          for (final result in _devices)
            GestureDetector(
              onTap: () => _selectDevice(result.device),
              child: Container(
                margin: const EdgeInsets.only(top: 8),
                padding: const EdgeInsets.all(12),
                decoration: BoxDecoration(
                  color: _device?.remoteId == result.device.remoteId
                      ? CupertinoColors.activeBlue.withValues(alpha: 0.12)
                      : CupertinoColors.systemBackground,
                  borderRadius: BorderRadius.circular(10),
                  border: Border.all(
                    color: _device?.remoteId == result.device.remoteId
                        ? CupertinoColors.activeBlue
                        : CupertinoColors.separator,
                  ),
                ),
                child: Row(
                  children: [
                    const Icon(CupertinoIcons.bluetooth),
                    const SizedBox(width: 10),
                    Expanded(child: Text(_deviceName(result.device))),
                    Text('${result.rssi} dBm'),
                  ],
                ),
              ),
            ),
        ],
      ),
    );
  }

  Widget _softApCard() {
    return Container(
      padding: const EdgeInsets.all(14),
      decoration: BoxDecoration(
        color: CupertinoColors.systemGrey6,
        borderRadius: BorderRadius.circular(14),
      ),
      child: const Text(
        '网关配网：先在手机「WiFi 设置」里连接网关热点 Gateway-XXXXXX（无需密码），'
        '若提示无法上网请选择「保持连接」，然后回到这里下发 WiFi。'
        '配网口令已自动填为网关口令，无需修改。',
      ),
    );
  }

  Widget _field(
    String label,
    TextEditingController controller, {
    String? placeholder,
    bool obscure = false,
    Widget? trailing,
  }) {
    return Padding(
      padding: const EdgeInsets.only(bottom: 12),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Text(
            label,
            style: const TextStyle(
              fontSize: 13,
              color: CupertinoColors.systemGrey,
            ),
          ),
          const SizedBox(height: 4),
          Row(
            children: [
              Expanded(
                child: CupertinoTextField(
                  controller: controller,
                  placeholder: placeholder,
                  obscureText: obscure,
                  autocorrect: false,
                  enableSuggestions: false,
                ),
              ),
              ?trailing,
            ],
          ),
        ],
      ),
    );
  }
}
