import 'dart:async';
import 'dart:convert';
import 'dart:math' as math;
import 'dart:typed_data';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';

class MochiException implements Exception {
  MochiException(this.message);
  final String message;
  @override
  String toString() => message;
}

/// UUIDs of Mochi's Bluetooth service (must match the firmware).
class MochiBle {
  static final Guid service = Guid('6e400001-b5a3-f393-e0a9-e50e24dcca9e');
  static final Guid rx = Guid('6e400002-b5a3-f393-e0a9-e50e24dcca9e'); // app -> Mochi (requests)
  static final Guid tx = Guid('6e400003-b5a3-f393-e0a9-e50e24dcca9e'); // Mochi -> app (notify)
  static final Guid vid = Guid('6e400004-b5a3-f393-e0a9-e50e24dcca9e'); // app -> Mochi (raw video bytes)
}

class BleReply {
  BleReply(this.status, this.body, [this.bytes]);
  final int status;
  final String body;
  final Uint8List? bytes;
}

/// How the app talks to Mochi: Bluetooth (BleLink) or the built-in demo (DemoLink, for the website).
abstract class MochiLink {
  bool get connected;
  bool get paused;
  set paused(bool value);
  int get chunkSize;
  set onDisconnected(void Function()? callback);
  Future<BleReply> request(String method, String path, Map<String, String> args,
      {bool wantBytes = false, Duration timeout = const Duration(seconds: 10)});
  Future<void> writeVideo(List<int> bytes);
  Future<void> close();
}

/// One Bluetooth connection to Mochi. Requests are sent one line at a time:
///   <id>|<GET or POST>|<path>|<url-encoded args>\n      and Mochi answers   <id>|<status>|<payload>\n
class BleLink implements MochiLink {
  BleLink(this.device);
  final BluetoothDevice device;

  BluetoothCharacteristic? _rx;
  BluetoothCharacteristic? _tx;
  BluetoothCharacteristic? _vid;
  StreamSubscription<List<int>>? _dataSub;
  StreamSubscription<BluetoothConnectionState>? _stateSub;
  final List<int> _in = [];
  Completer<BleReply>? _waiting;
  int _waitingId = 0;
  bool _wantBytes = false;
  int _nextId = 1;
  Future<void> _queue = Future.value(); // one request at a time

  @override
  bool connected = false;
  @override
  bool paused = false; // true while a video upload runs (the app stops polling)
  @override
  void Function()? onDisconnected;

  @override
  int get chunkSize => math.max(20, math.min(device.mtuNow - 3, 240));

  Future<void> connect() async {
    await device.connect(timeout: const Duration(seconds: 15)); // asks Android for a big MTU by itself
    final services = await device.discoverServices();
    final matches = services.where((s) => s.uuid == MochiBle.service);
    if (matches.isEmpty) throw MochiException('This is not a Mochi (service not found)');
    for (final c in matches.first.characteristics) {
      if (c.uuid == MochiBle.rx) _rx = c;
      if (c.uuid == MochiBle.tx) _tx = c;
      if (c.uuid == MochiBle.vid) _vid = c;
    }
    if (_rx == null || _tx == null || _vid == null) throw MochiException('Mochi firmware is too old for Bluetooth');
    await _tx!.setNotifyValue(true);
    _dataSub = _tx!.onValueReceived.listen(_onData);
    _stateSub = device.connectionState.listen((s) {
      if (s == BluetoothConnectionState.disconnected && connected) {
        connected = false;
        _fail(MochiException('Bluetooth disconnected'));
        onDisconnected?.call();
      }
    });
    connected = true;
  }

  @override
  Future<void> close() async {
    connected = false;
    await _dataSub?.cancel();
    await _stateSub?.cancel();
    try {
      await device.disconnect();
    } catch (_) {}
  }

  static String _encode(Map<String, String> args) =>
      args.entries.map((e) => '${Uri.encodeQueryComponent(e.key)}=${Uri.encodeQueryComponent(e.value)}').join('&');

  @override
  Future<BleReply> request(String method, String path, Map<String, String> args,
      {bool wantBytes = false, Duration timeout = const Duration(seconds: 10)}) {
    final done = Completer<BleReply>();
    _queue = _queue.then((_) async {
      try {
        done.complete(await _send(method, path, args, wantBytes, timeout));
      } catch (e) {
        done.completeError(e);
      }
    });
    return done.future;
  }

  Future<BleReply> _send(String method, String path, Map<String, String> args, bool wantBytes, Duration timeout) async {
    if (!connected || _rx == null) throw MochiException('Not connected to Mochi');
    final id = _nextId++;
    _in.clear();
    final reply = Completer<BleReply>();
    _waiting = reply;
    _waitingId = id;
    _wantBytes = wantBytes;
    final data = utf8.encode('$id|$method|$path|${_encode(args)}\n');
    final chunk = chunkSize;
    for (var i = 0; i < data.length; i += chunk) {
      await _rx!.write(data.sublist(i, math.min(i + chunk, data.length)), timeout: 15);
    }
    return reply.future.timeout(timeout, onTimeout: () {
      _waiting = null;
      throw MochiException('Mochi did not answer');
    });
  }

  /// Raw video bytes (written with response, so the phone waits for each packet).
  @override
  Future<void> writeVideo(List<int> bytes) => _vid!.write(bytes, timeout: 15);

  void _onData(List<int> d) {
    final w = _waiting;
    if (w == null) return;
    _in.addAll(d);
    final nl = _in.indexOf(10);
    if (nl < 0) return;
    final header = utf8.decode(_in.sublist(0, nl), allowMalformed: true);
    final p1 = header.indexOf('|');
    final p2 = p1 < 0 ? -1 : header.indexOf('|', p1 + 1);
    if (p2 < 0) {
      _in.removeRange(0, nl + 1);
      return;
    }
    if (int.tryParse(header.substring(0, p1)) != _waitingId) {
      _in.removeRange(0, nl + 1); // answer to an old request
      return;
    }
    final status = int.tryParse(header.substring(p1 + 1, p2)) ?? 500;
    final payload = header.substring(p2 + 1);
    if (_wantBytes && status == 200) {
      final size = int.tryParse(payload) ?? 0;
      if (_in.length >= nl + 1 + size) {
        _finish(w, BleReply(status, '', Uint8List.fromList(_in.sublist(nl + 1, nl + 1 + size))));
      }
      return;
    }
    _finish(w, BleReply(status, payload));
  }

  void _finish(Completer<BleReply> w, BleReply r) {
    _waiting = null;
    _in.clear();
    if (!w.isCompleted) w.complete(r);
  }

  void _fail(Object e) {
    final w = _waiting;
    _waiting = null;
    if (w != null && !w.isCompleted) w.completeError(e);
  }
}
