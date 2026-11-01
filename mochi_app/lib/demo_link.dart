import 'dart:async';
import 'dart:convert';
import 'dart:math' as math;
import 'ble_link.dart';

/// A pretend Mochi inside the app, so every screen can be checked without the hardware
/// (used by the website version and the "Try the demo" button). It answers like the firmware.
class DemoLink implements MochiLink {
  @override
  bool connected = true;
  @override
  bool paused = false;
  @override
  void Function()? onDisconnected;
  @override
  int get chunkSize => 240;

  final _rnd = math.Random();
  final _start = DateTime.now();
  String pin = '1234';
  final Map<String, dynamic> _settings = {
    'name': 'Mochi', 'volume': 30, 'sleepSec': 60, 'autoSleep': true, 'vibration': true, 'sound': true,
    'lonelyMin': 3, 'idleSounds': true,
    'title1': '', 'title2': '', 'title3': '', 'wifi': '',
  };
  final List<Map<String, dynamic>> _trig = [
    for (final id in ['tap1', 'tap2', 'tap3', 'long', 'shake', 'upside', 'move', 'dark', 'night', 'bright', 'lonely', 'wake', 'sleep', 'start', 'songend'])
      {'id': id, 'type': 'default', 'mood': 'happy', 'f': 9, 'n': 1, 'slot': 1, 'vib': 0},
  ];
  final List<String> _lyrics = ['', '', ''];
  final List<bool> _videos = [false, false, false];
  int _upSize = 0, _upGot = 0, _upSlot = 1;
  String _expr = 'normal', _lcd1 = "Hello! I'm Mochi", _lcd2 = 'Pet me!', _audio = '';
  DateTime _exprUntil = DateTime.now();
  int _vibCount = 1, _reqOk = 0, _love = 72;
  final List<String> _log = [];

  DemoLink() {
    _addLog('BOOT', 'firmware 2.0 reset=power-on (demo)');
    for (final c in ['OLED ok', 'LCD ok', 'MPU6050 ok', 'DFPlayer ok']) {
      _addLog('CHECK', c);
    }
  }

  int get _ms => DateTime.now().difference(_start).inMilliseconds;
  void _addLog(String type, String detail) =>
      _log.add('1,$_ms,${DateTime.now().millisecondsSinceEpoch ~/ 1000},$type,$detail');

  void _react(String expr, String l1, String l2, String audio) {
    _expr = expr;
    _lcd1 = l1;
    _lcd2 = l2;
    _audio = audio;
    _vibCount++;
    _exprUntil = DateTime.now().add(const Duration(seconds: 3));
  }

  BleReply _json(Object o, [int status = 200]) {
    ++_reqOk;
    return BleReply(status, jsonEncode(o));
  }

  Map<String, dynamic> _status() {
    final t = _ms / 1000;
    if (DateTime.now().isAfter(_exprUntil)) {
      _expr = 'normal';
      _audio = '';
    }
    final moving = _rnd.nextDouble() < 0.08;
    final light = (62 + math.sin(t / 9) * 6 + (_rnd.nextDouble() - .5) * 3).round();
    return {
      'name': _settings['name'], 'mode': 'normal', 'expr': _expr, 'light': light, 'night': false, 'love': _love,
      'tilt': moving ? (_rnd.nextDouble() - .5) * 1.6 : math.sin(t / 7) * .08, 'touch': _rnd.nextDouble() < .05,
      'df': true, 'song': 0, 'songTitle': '', 'songMs': 0, 'videos': _videos, 'fsFree': 900000, 'version': '2.0',
      'lightRaw': 4095 - (light * 40.95).round(), 'ax': .1, 'ay': .2, 'az': 9.7,
      'accel': moving ? 13.5 : 9.8 + (_rnd.nextDouble() - .5) * .2, 'gyro': moving ? 1.8 : .02, 'upside': false,
      'mpuOk': true, 'lightOk': true, 'oled': 'face', 'lcdOk': true, 'lcd1': _lcd1, 'lcd2': _lcd2,
      'audioPlaying': _audio.isNotEmpty, 'audioFile': _audio, 'volume': _settings['volume'], 'soundOn': _settings['sound'],
      'vib': false, 'vibCount': _vibCount, 'vibOn': _settings['vibration'],
    };
  }

  @override
  Future<BleReply> request(String method, String path, Map<String, String> args,
      {bool wantBytes = false, Duration timeout = const Duration(seconds: 10)}) async {
    await Future<void>.delayed(Duration(milliseconds: 15 + _rnd.nextInt(30))); // like a Bluetooth round trip
    final open = path == '/api/status' || path == '/api/ping' || path == '/api/stats';
    if (!open && args['key'] != pin) {
      _addLog('SECURITY', 'wrong PIN');
      return BleReply(403, '{"ok":false,"error":"wrong PIN"}');
    }
    final get = method == 'GET';
    switch (path) {
      case '/api/ping':
        return _json({'ok': true, 'name': _settings['name'], 'version': '2.0'});
      case '/api/status':
        return _json(_status());
      case '/api/settings':
        if (get) return _json(_settings);
        final oldName = _settings['name'];
        for (final k in ['name', 'title1', 'title2', 'title3']) {
          if ((args[k] ?? '').trim().isNotEmpty) _settings[k] = args[k]!.trim();
        }
        for (final k in ['volume', 'sleepSec', 'lonelyMin']) {
          if (args[k] != null) _settings[k] = int.tryParse(args[k]!) ?? _settings[k];
        }
        for (final k in ['autoSleep', 'vibration', 'sound', 'idleSounds']) {
          if (args[k] != null) _settings[k] = args[k] == '1';
        }
        if ((args['newPin'] ?? '').length >= 4) pin = args['newPin']!;
        if (oldName != _settings['name']) _react('happy', "Hi! I'm ${_settings['name']}", 'New name! ♥', '01/001');
        _addLog('CONFIG', 'settings saved');
        return _json({'ok': true});
      case '/api/action':
        switch (args['do']) {
          case 'pet':
            _react('wink', 'Wink wink~', 'Hehe!', '09/001');
          case 'love':
            _react('love', 'I love you! ♥', 'Hug me more~', '10/001');
          case 'excited':
            _react('laugh', 'Hahaha!', 'So much fun!', '05/002');
          case 'sleep':
            _react('sleep', 'Zzz...', 'Good night ♥', '04/002');
          default:
            _react('happy', 'Yay!', '', '01/001');
        }
        _love = math.min(100, _love + 2);
        _addLog('EVENT', '${args['do']} from the app');
        return _json({'ok': true});
      case '/api/triggers':
        if (get) return _json({'triggers': _trig});
        if (args['reset'] == '1') {
          for (final t in _trig) {
            t['type'] = 'default';
            t['vib'] = 0;
          }
        }
        for (final t in _trig) {
          final vib = int.tryParse(args['${t['id']}_vib'] ?? '');
          if (vib != null && vib >= 0 && vib <= 2) t['vib'] = vib;
          final v = args[t['id']];
          if (v == null) continue;
          if (v == 'default' || v == 'none') t['type'] = v;
          if (v.startsWith('mood:')) t..['type'] = 'mood'..['mood'] = v.substring(5);
          if (v.startsWith('song:')) t..['type'] = 'song'..['slot'] = int.tryParse(v.substring(5)) ?? 1;
        }
        _addLog('CONFIG', 'reactions saved');
        return _json({'ok': true});
      case '/api/lyrics':
        final slot = (int.tryParse(args['slot'] ?? '') ?? 1).clamp(1, 3);
        if (get) return BleReply(200, Uri.encodeQueryComponent(_lyrics[slot - 1]));
        _lyrics[slot - 1] = args['plain'] ?? '';
        return _json({'ok': true});
      case '/api/lyrics/reset':
        _lyrics[((int.tryParse(args['slot'] ?? '') ?? 1).clamp(1, 3)) - 1] = '';
        return _json({'ok': true});
      case '/api/video':
        return BleReply(404, '{"ok":false,"error":"no video"}');
      case '/api/video/begin':
        _upSlot = (int.tryParse(args['slot'] ?? '') ?? 1).clamp(1, 3);
        _upSize = int.tryParse(args['size'] ?? '') ?? 0;
        _upGot = 0;
        return _json({'ok': true});
      case '/api/video/sync':
        await Future<void>.delayed(const Duration(milliseconds: 40));
        return _json({'ok': true, 'got': _upGot});
      case '/api/video/end':
        final ok = _upGot == _upSize;
        if (ok) _videos[_upSlot - 1] = true;
        _addLog('DATA', ok ? 'video $_upSlot saved' : 'video upload rejected');
        return ok ? _json({'ok': true, 'frames': (_upSize - 16) ~/ 1024}) : BleReply(400, '{"ok":false,"error":"invalid video file"}');
      case '/api/video/delete':
        _videos[((int.tryParse(args['slot'] ?? '') ?? 1).clamp(1, 3)) - 1] = false;
        return _json({'ok': true});
      case '/api/stats':
        return _json({
          'uptimeS': _ms ~/ 1000, 'boots': 1, 'reset': 'power-on (demo)', 'heap': 182340, 'minHeap': 151200,
          'loopAvgUs': 2400, 'loopMaxUs': 21000, 'respOledMs': 28, 'respOledMaxMs': 41, 'respAudioMs': 3, 'respAudioMaxMs': 9,
          'respCount': _vibCount, 'reqOk': _reqOk, 'reqErr': 0, 'authFail': 0, 'bleConnects': 1, 'bleDrops': 0,
          'locked': false, 'timeSynced': true, 'fsUsed': 520000, 'fsTotal': 1441792,
          'sensors': {'oled': true, 'lcd': true, 'mpu': true, 'dfplayer': true, 'storage': true, 'lightOk': true, 'lightRaw': 1500},
        });
      case '/api/log':
        return BleReply(200, Uri.encodeQueryComponent(_log.join('\n')));
      case '/api/log/clear':
        _log.clear();
        _addLog('CONFIG', 'log cleared');
        return _json({'ok': true});
      case '/api/time':
        _addLog('TIME', 'clock synced from the app');
        return _json({'ok': true});
    }
    return BleReply(404, '{"ok":false,"error":"not found"}');
  }

  @override
  Future<void> writeVideo(List<int> bytes) async => _upGot += bytes.length;

  @override
  Future<void> close() async => connected = false;
}
