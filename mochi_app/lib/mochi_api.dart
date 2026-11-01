import 'dart:convert';
import 'dart:typed_data';
import 'ble_link.dart';

/// What Mochi does for one touch / movement / light change.
class TriggerCfg {
  TriggerCfg({required this.id, required this.type, required this.mood, required this.f, required this.n, required this.slot, this.vib = 0});
  factory TriggerCfg.fromJson(Map<String, dynamic> j) => TriggerCfg(
        id: j['id'] as String,
        type: j['type'] as String,
        mood: j['mood'] as String,
        f: j['f'] as int,
        n: j['n'] as int,
        slot: j['slot'] as int,
        vib: (j['vib'] as int?) ?? 0, // older firmware has no vibration choice
      );
  final String id;
  String type; // default | mood | sound | song | none
  String mood;
  int f;
  int n;
  int slot;
  int vib; // 0 = Mochi decides, 1 = always vibrate, 2 = no vibration

  /// Everything that is saved for this trigger (used to show "not saved").
  String get state => '${toArg()}|$vib';

  /// Value sent to POST /api/triggers.
  String toArg() {
    switch (type) {
      case 'mood':
        return 'mood:$mood';
      case 'sound':
        return 'sound:$f:$n';
      case 'song':
        return 'song:$slot';
      default:
        return type;
    }
  }
}

/// One sound on the SD card (folders 01..13) and the face shown with it.
class SoundInfo {
  SoundInfo({required this.f, required this.n, required this.name, required this.face});
  factory SoundInfo.fromJson(Map<String, dynamic> j) =>
      SoundInfo(f: j['f'] as int, n: j['n'] as int, name: j['name'] as String, face: j['face'] as String);
  final int f;
  final int n;
  final String name;
  String face;
  String get key => '${f}_$n';
}

class LyricCue {
  LyricCue(this.ms, this.line1, this.line2);
  final int ms;
  final String line1;
  final String line2;
}

/// Talks to Mochi over Bluetooth. Every request carries key=<PIN>.
class MochiApi {
  MochiApi(this.link, this.pin);
  final MochiLink link;
  final String pin;

  String _errorText(BleReply r) {
    try {
      final j = jsonDecode(r.body) as Map<String, dynamic>;
      if (j['error'] != null) return '${j['error']}';
    } catch (_) {}
    return 'Mochi replied ${r.status}';
  }

  Future<BleReply> _req(String method, String path,
      [Map<String, String>? args, Duration timeout = const Duration(seconds: 10), bool wantBytes = false]) async {
    final r = await link.request(method, path, {'key': pin, ...?args}, wantBytes: wantBytes, timeout: timeout);
    if (r.status == 403) throw MochiException('Wrong PIN');
    if (r.status >= 400) throw MochiException(_errorText(r));
    return r;
  }

  Map<String, dynamic> _json(BleReply r) => jsonDecode(r.body) as Map<String, dynamic>;

  // ---- status + settings ----
  Future<Map<String, dynamic>> status() async => _json(await _req('GET', '/api/status'));
  Future<Map<String, dynamic>> settings() async => _json(await _req('GET', '/api/settings'));

  /// name, volume (older firmware: dayVol, nightVol), sleepSec, autoSleep(1/0), vibration, sound, title1..3, newPin
  Future<void> saveSettings(Map<String, String> body) async => _req('POST', '/api/settings', body);

  // ---- actions on Mochi (pet, love, excited, music, play, stop, wake, sleep, mood, sound) ----
  Future<void> action(String what, [Map<String, String>? extra]) async =>
      _req('POST', '/api/action', {'do': what, ...?extra});

  // ---- what Mochi does ----
  Future<List<TriggerCfg>> triggers() async {
    final j = _json(await _req('GET', '/api/triggers'));
    return (j['triggers'] as List).map((e) => TriggerCfg.fromJson(e as Map<String, dynamic>)).toList();
  }

  Future<void> saveTriggers(List<TriggerCfg> list) async =>
      _req('POST', '/api/triggers', {
        for (final t in list) ...{t.id: t.toArg(), '${t.id}_vib': '${t.vib}'},
      });
  Future<void> resetTriggers() async => _req('POST', '/api/triggers', {'reset': '1'});

  // ---- face for each sound ----
  Future<({List<String> faces, List<SoundInfo> sounds})> sounds() async {
    final j = _json(await _req('GET', '/api/sounds'));
    return (
      faces: (j['faces'] as List).cast<String>(),
      sounds: (j['sounds'] as List).map((e) => SoundInfo.fromJson(e as Map<String, dynamic>)).toList(),
    );
  }

  Future<void> saveSounds(List<SoundInfo> list) async =>
      _req('POST', '/api/sounds', {for (final s in list) 's${s.f}_${s.n}': s.face});
  Future<void> resetSounds() async => _req('POST', '/api/sounds', {'reset': '1'});

  // ---- lyrics: text, one "ms|line1|line2" per line ----
  Future<String> lyricsText(int slot) async {
    final r = await _req('GET', '/api/lyrics', {'slot': '$slot'}, const Duration(seconds: 20));
    return Uri.decodeQueryComponent(r.body);
  }

  Future<List<LyricCue>> lyrics(int slot) async {
    final out = <LyricCue>[];
    for (final line in (await lyricsText(slot)).split('\n')) {
      final p = line.split('|');
      final ms = p.length >= 2 ? int.tryParse(p[0].trim()) : null;
      if (ms != null) out.add(LyricCue(ms, p[1], p.length > 2 ? p[2] : ''));
    }
    return out;
  }

  Future<void> saveLyrics(int slot, String text) async =>
      _req('POST', '/api/lyrics', {'slot': '$slot', 'plain': text}, const Duration(seconds: 20));

  Future<void> resetLyrics(int slot) async => _req('POST', '/api/lyrics/reset', {'slot': '$slot'});

  // ---- OLED video (.mvid: 128x64, 1 bit per pixel) ----
  /// The video saved on Mochi (about 15 seconds over Bluetooth), or null when there is none.
  Future<Uint8List?> video(int slot) async {
    final r = await link.request('GET', '/api/video', {'key': pin, 'slot': '$slot'},
        wantBytes: true, timeout: const Duration(seconds: 120));
    if (r.status == 404) return null;
    if (r.status == 403) throw MochiException('Wrong PIN');
    if (r.status >= 400) throw MochiException(_errorText(r));
    return r.bytes;
  }

  /// Sends a .mvid file in small packets (about 10 to 40 seconds for a 15 second video).
  Future<void> uploadVideo(int slot, Uint8List bytes, {void Function(double)? onProgress}) async {
    link.paused = true;
    try {
      await _req('POST', '/api/video/begin', {'slot': '$slot', 'size': '${bytes.length}'});
      final chunk = link.chunkSize;
      var sent = 0, sinceSync = 0;
      while (sent < bytes.length) {
        final end = sent + chunk > bytes.length ? bytes.length : sent + chunk;
        await link.writeVideo(bytes.sublist(sent, end));
        sinceSync += end - sent;
        sent = end;
        onProgress?.call(sent / bytes.length);
        if (sinceSync >= 4096) {
          await _req('POST', '/api/video/sync');
          sinceSync = 0;
        }
      }
      await _req('POST', '/api/video/end', {'slot': '$slot', 'size': '${bytes.length}'}, const Duration(seconds: 30));
    } finally {
      link.paused = false;
    }
  }

  Future<void> deleteVideo(int slot) async => _req('POST', '/api/video/delete', {'slot': '$slot'});

  // ---- diagnostics (evidence for testing) ----
  /// Health, counters, and response times measured on Mochi.
  Future<Map<String, dynamic>> stats() async => _json(await _req('GET', '/api/stats'));

  /// The newest part of Mochi's event log (CSV: boot,uptime_ms,unix_time,type,detail).
  Future<String> logText() async {
    final r = await _req('GET', '/api/log', null, const Duration(seconds: 30));
    return Uri.decodeQueryComponent(r.body);
  }

  Future<void> clearLog() async => _req('POST', '/api/log/clear');

  /// Gives Mochi the phone's clock so its log lines get real dates.
  Future<void> syncTime() async =>
      _req('POST', '/api/time', {'t': '${DateTime.now().millisecondsSinceEpoch ~/ 1000}'});

  /// One round trip over Bluetooth, in milliseconds (throws when Mochi does not answer in 3 s).
  Future<int> pingMs() async {
    final sw = Stopwatch()..start();
    final r = await link.request('GET', '/api/ping', {}, timeout: const Duration(seconds: 3));
    sw.stop();
    if (r.status != 200) throw MochiException('ping failed');
    return sw.elapsedMilliseconds;
  }
}
