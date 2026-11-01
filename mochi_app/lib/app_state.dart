import 'dart:async';
import 'dart:math' as math;
import 'package:audioplayers/audioplayers.dart';
import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart' show HapticFeedback, rootBundle;
import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import 'package:shared_preferences/shared_preferences.dart';
import 'ble_link.dart';
import 'demo_link.dart';
import 'mochi_api.dart';
import 'tones.dart';

/// Length (ms) of Mochi's SD card sounds, folders 01..13, files 001 and 002
/// (the same files are in assets/sounds/FF_NNN.mp3, so the phone preview matches Mochi).
const List<List<int>> soundMs = [
  [0, 0],
  [5878, 10632],
  [9012, 1881],
  [14550, 4911],
  [3030, 22674],
  [10080, 12600],
  [7752, 4944],
  [9288, 6661],
  [7776, 1071],
  [10266, 5878],
  [2299, 1411],
  [2743, 1620],
  [1672, 4754],
  [2456, 7445],
];

/// Length (ms) of the 3 songs (assets/audio/songN.mp3 = /14/00N on the SD card).
const List<int> songMs = [11416, 13453, 13767];

/// The sound folder Mochi uses for each mood (same as the firmware's soundForExpr).
const Map<String, int> moodFolder = {
  'happy': 1, 'sad': 2, 'cry': 2, 'angry': 3, 'sleepy': 4, 'excited': 5, 'laugh': 5, 'surprised': 6,
  'love': 10, 'shy': 7, 'wink': 9, 'dizzy': 11, 'confused': 11,
};

/// Moods you can pick (same names as the firmware faces).
const List<String> moods = [
  'happy', 'excited', 'laugh', 'wink', 'love', 'shy', 'sad', 'cry', 'angry', 'sleepy', 'surprised', 'dizzy', 'confused',
];

const Map<String, String> moodLabel = {
  'happy': 'Happy', 'excited': 'Excited', 'laugh': 'Laugh', 'wink': 'Wink', 'love': 'Love', 'shy': 'Shy',
  'sad': 'Sad', 'cry': 'Cry', 'angry': 'Angry', 'sleepy': 'Sleepy', 'surprised': 'Surprised', 'dizzy': 'Dizzy',
  'confused': 'Confused', 'normal': 'Normal', 'sleep': 'Sleeping', 'waking': 'Waking up',
};

/// Triggers, grouped the way the app shows them.
const List<(String, List<(String, String)>)> triggerGroups = [
  ('When you touch Mochi', [('tap1', 'Touch once'), ('tap2', 'Touch 2 times'), ('tap3', 'Touch 3 times'), ('long', 'Hold')]),
  ('When you move Mochi', [('shake', 'Shake'), ('upside', 'Turn upside down'), ('move', 'Carry around')]),
  ('When the light changes', [('dark', 'Light turns off'), ('night', 'Room gets dark'), ('bright', 'Light comes back')]),
  ('When Mochi is alone', [('lonely', 'Nobody plays for a while')]),
  ('Mochi\'s day', [('wake', 'Wakes up'), ('sleep', 'Falls asleep'), ('start', 'Is turned on'), ('songend', 'A song ends')]),
];

String triggerLabel(String id) {
  for (final g in triggerGroups) {
    for (final t in g.$2) {
      if (t.$1 == id) return t.$2;
    }
  }
  return id;
}

/// The face of Mochi's own built-in reaction (for the preview).
const Map<String, String> builtInFace = {
  'tap1': 'happy', 'tap2': 'excited', 'tap3': 'excited', 'long': 'love', 'shake': 'dizzy', 'upside': 'angry',
  'move': 'surprised', 'dark': 'surprised', 'night': 'sleepy', 'bright': 'excited', 'lonely': 'sad',
  'wake': 'waking', 'sleep': 'sleep', 'start': 'happy', 'songend': 'happy',
};

/// An OLED video bundled with the app (assets/videos/songN.mvid).
class MochiVideo {
  MochiVideo(this.bytes, this.fps, this.frames);
  final Uint8List bytes;
  final int fps;
  final int frames;
  int get durationMs => frames * 1000 ~/ fps;
}

// ---------------------------------------------------------------- lyrics helpers
/// Splits one lyric line into the two 16-character LCD rows.
(String, String) splitForLcd(String line) {
  final t = line.trim();
  if (t.length <= 16) return (t, '');
  var cut = t.lastIndexOf(' ', 16);
  if (cut <= 0) cut = 16;
  final second = t.substring(cut).trim();
  return (t.substring(0, cut).trim(), second.length > 16 ? second.substring(0, 16) : second);
}

/// Plain lyric lines (one per row) spread evenly over the song.
List<LyricCue> cuesFor(String text, int durationMs) {
  final lines = text.split('\n').map((l) => l.trim()).where((l) => l.isNotEmpty).toList();
  if (lines.isEmpty) return [];
  final step = ((durationMs - 1000) / lines.length).clamp(1500, 60000).toDouble();
  return [
    for (var i = 0; i < lines.length; i++)
      () {
        final r = splitForLcd(lines[i]);
        return LyricCue(500 + (i * step).round(), r.$1, r.$2);
      }(),
  ];
}

/// Mochi's lyric format: one "ms|top|bottom" per line.
String toMochiLyrics(String text, int durationMs) =>
    cuesFor(text, durationMs).map((c) => '${c.ms}|${c.line1}|${c.line2}').join('\n');

/// Turns Mochi's lyrics back into plain lines (and hides the built-in placeholders).
String fromMochiLyrics(String text) {
  final out = <String>[];
  for (final line in text.split('\n')) {
    final p = line.split('|');
    if (p.length < 2 || int.tryParse(p[0].trim()) == null) continue;
    final joined = [p[1], if (p.length > 2) p[2]].where((s) => s.trim().isNotEmpty).join(' ').trim();
    if (joined.isNotEmpty) out.add(joined);
  }
  if (out.isNotEmpty && out.every((l) => RegExp(r'^Song \d line \d goes here').hasMatch(l))) return '';
  return out.join('\n');
}

// ---------------------------------------------------------------- preview
/// What "Mochi's screen" at the top of the app shows (on the phone only).
class Preview {
  String face = 'normal';
  String label = '';
  DateTime? faceUntil;
  int? songSlot;
  List<LyricCue> cues = [];
  int untilMs = 0;
  final Stopwatch sw = Stopwatch();
  DateTime faceSince = DateTime.now();
}

/// One global app state (simple on purpose).
class AppState extends ChangeNotifier {
  String pin = '1234';
  BluetoothDevice? device;
  MochiLink? link;
  bool demo = false; // true = pretend Mochi (website / no hardware)
  MochiApi? api;
  bool connected = false;
  String? error;
  Map<String, dynamic> status = {};
  Map<String, dynamic> settings = {};
  List<TriggerCfg> triggers = [];
  Map<String, String> savedTriggers = {}; // id -> value saved on Mochi
  final List<String> lyrics = ['', '', ''];
  final Map<int, MochiVideo> videos = {};
  final Preview pv = Preview();
  final AudioPlayer player = AudioPlayer();
  final math.Random _rnd = math.Random();
  Timer? _poll;

  // Recovery: when Bluetooth drops, the app tries to reconnect by itself.
  bool reconnecting = false;
  int reconnectAttempt = 0;
  int? lastReconnectMs;
  int drops = 0;
  bool _userClosing = false;

  // Monitor history (one sample per second, last 60 s).
  final List<double> lightHist = [];
  final List<double> motionHist = [];

  Future<void> init() async {
    final sp = await SharedPreferences.getInstance();
    pin = sp.getString('pin') ?? pin;
    for (var i = 0; i < 3; i++) {
      lyrics[i] = sp.getString('lyrics${i + 1}') ?? '';
      try {
        final b = (await rootBundle.load('assets/videos/song${i + 1}.mvid')).buffer.asUint8List();
        final fps = b[5], frames = b[6] | (b[7] << 8);
        if (fps > 0 && frames > 0 && b.length == 16 + frames * 1024) videos[i + 1] = MochiVideo(b, fps, frames);
      } catch (_) {}
    }
    player.onPlayerComplete.listen((_) {
      if (pv.songSlot != null) stopPreview();
    });
    notifyListeners();
  }

  int songDurationMs(int slot) => slot >= 1 && slot <= songMs.length ? songMs[slot - 1] : (videos[slot]?.durationMs ?? 15000);

  void _record() {
    void add(List<double> h, double v) {
      h.add(v);
      if (h.length > 60) h.removeAt(0);
    }

    add(lightHist, ((status['light'] as num?) ?? 0).toDouble());
    final a = ((status['accel'] as num?) ?? 9.8).toDouble();
    final g = ((status['gyro'] as num?) ?? 0).toDouble();
    add(motionHist, (a - 9.8).abs() + g * 2); // 0 = still
  }

  // ---------------------------------------------------------------- connection
  /// Connects to Mochi over Bluetooth and loads everything saved on it.
  Future<bool> connect(BluetoothDevice d, String p) async {
    demo = false;
    pin = p.trim();
    _poll?.cancel();
    _userClosing = true; // closing the old link is not a "drop"
    await link?.close();
    _userClosing = false;
    final l = BleLink(d);
    try {
      await l.connect();
      l.onDisconnected = () {
        connected = false;
        _poll?.cancel();
        notifyListeners();
        if (!_userClosing) _autoReconnect();
      };
      final a = MochiApi(l, pin);
      status = await a.status();
      link = l;
      api = a;
      device = d;
      await refreshAll(); // needs the right PIN
      final sp = await SharedPreferences.getInstance();
      await sp.setString('pin', pin);
      connected = true;
      error = null;
      try {
        await a.syncTime(); // real dates in Mochi's log
      } catch (_) {}
      _poll = Timer.periodic(const Duration(seconds: 1), (_) async {
        if (l.paused || !l.connected) return;
        try {
          status = await a.status();
          _record();
          notifyListeners();
        } catch (_) {}
      });
    } catch (e) {
      connected = false;
      error = '$e';
      await l.close();
    }
    notifyListeners();
    return connected;
  }

  /// Opens the app with a pretend Mochi, so every screen can be checked (website version).
  Future<void> connectDemo() async {
    _poll?.cancel();
    await link?.close();
    final l = DemoLink()..pin = pin;
    final a = MochiApi(l, pin);
    demo = true;
    link = l;
    api = a;
    device = null;
    status = await a.status();
    await refreshAll();
    connected = true;
    error = null;
    _poll = Timer.periodic(const Duration(seconds: 1), (_) async {
      if (l.paused || !l.connected) return;
      try {
        status = await a.status();
        _record();
        notifyListeners();
      } catch (_) {}
    });
    notifyListeners();
  }

  /// After the PIN was changed on Mochi, use the new PIN from now on.
  Future<void> usePin(String newPin) async {
    pin = newPin;
    final sp = await SharedPreferences.getInstance();
    await sp.setString('pin', pin);
    if (link != null) api = MochiApi(link!, pin);
    if (link is DemoLink) (link as DemoLink).pin = pin;
    notifyListeners();
  }

  // ---------------------------------------------------------------- song videos on Mochi's OLED
  /// true = Mochi plays this song's video, false = the stick man (default).
  bool videoOnMochi(int slot) {
    final v = status['videos'];
    return v is List && slot - 1 < v.length && v[slot - 1] == true;
  }

  /// Progress (0..1) of the video being sent to Mochi, by song.
  final Map<int, double> videoSending = {};

  /// Sends the song's video to Mochi (Video) or removes it (Stick man). Saved on Mochi.
  Future<void> setVideoOnMochi(int slot, bool on) async {
    final a = api;
    if (a == null || videoSending.isNotEmpty) return;
    try {
      if (on) {
        final v = videos[slot];
        if (v == null) throw MochiException('The video for Song $slot is missing in the app');
        videoSending[slot] = 0;
        notifyListeners();
        await a.uploadVideo(slot, v.bytes, onProgress: (p) {
          videoSending[slot] = p;
          notifyListeners();
        });
      } else {
        await a.deleteVideo(slot);
      }
    } finally {
      videoSending.remove(slot);
      try {
        status = await a.status();
      } catch (_) {}
      notifyListeners();
    }
  }

  Future<void> refreshAll() async {
    final a = api!;
    settings = await a.settings();
    triggers = await a.triggers();
    savedTriggers = {for (final t in triggers) t.id: t.state};
    final sp = await SharedPreferences.getInstance();
    for (var i = 0; i < 3; i++) {
      try {
        final text = fromMochiLyrics(await a.lyricsText(i + 1));
        if (text.isNotEmpty || lyrics[i].isEmpty) lyrics[i] = text;
        await sp.setString('lyrics${i + 1}', lyrics[i]);
      } catch (_) {}
    }
    notifyListeners();
  }

  Future<void> disconnect() async {
    _poll?.cancel();
    reconnecting = false;
    connected = false;
    _userClosing = true;
    await link?.close();
    _userClosing = false;
    stopPreview();
    notifyListeners();
  }

  /// Tries to reconnect every 2 s, up to 10 times, and remembers how long it took.
  Future<void> _autoReconnect() async {
    if (reconnecting || device == null) return;
    reconnecting = true;
    drops++;
    reconnectAttempt = 0;
    final sw = Stopwatch()..start();
    notifyListeners();
    while (reconnecting && reconnectAttempt < 10) {
      reconnectAttempt++;
      notifyListeners();
      await Future<void>.delayed(const Duration(seconds: 2));
      if (!reconnecting) return;
      if (await connect(device!, pin)) {
        lastReconnectMs = sw.elapsedMilliseconds;
        reconnecting = false;
        notifyListeners();
        return;
      }
    }
    if (reconnecting) {
      reconnecting = false;
      error = 'Could not reconnect. Is Mochi on and close to the phone?';
      notifyListeners();
    }
  }

  // ---------------------------------------------------------------- reactions
  int get unsavedCount => triggers.where((t) => savedTriggers[t.id] != t.state).length;

  Future<void> saveTriggers() async {
    await api!.saveTriggers(triggers);
    savedTriggers = {for (final t in triggers) t.id: t.state};
    notifyListeners();
  }

  /// Pick the vibration for one trigger (0 Mochi decides, 1 vibrate, 2 none). Saved with "Save to Mochi".
  void chooseVib(TriggerCfg t, int vib) {
    t.vib = vib;
    if (vib == 1) HapticFeedback.heavyImpact(); // feel it on the phone right away
    notifyListeners();
  }

  /// Pick a new action for one trigger (shown and heard right away, saved later).
  void choose(TriggerCfg t, String type, {String? mood, int? slot}) {
    t.type = type;
    if (mood != null) t.mood = mood;
    if (slot != null) t.slot = slot;
    previewTrigger(t);
  }

  void previewTrigger(TriggerCfg t) {
    switch (t.type) {
      case 'mood':
        previewMood(t.mood);
      case 'song':
        previewSong(t.slot);
      case 'none':
        previewMood('normal', label: 'Nothing happens', sound: false);
      case 'sound':
        previewMood('happy', label: 'Sound ${t.f.toString().padLeft(2, '0')}/${t.n.toString().padLeft(3, '0')}');
      default:
        if (t.id == 'tap2') {
          previewSong(1); // Mochi's normal reaction to 2 taps is Song 1
        } else {
          final f = builtInFace[t.id] ?? 'happy';
          previewMood(f, label: 'Normal: ${moodLabel[f]}');
        }
    }
  }

  // ---------------------------------------------------------------- preview on this phone
  void previewMood(String mood, {String? label, bool sound = true}) {
    player.stop();
    pv
      ..songSlot = null
      ..sw.stop()
      ..face = mood
      ..label = label ?? moodLabel[mood] ?? mood
      ..faceSince = DateTime.now();
    // Play the same sound file Mochi plays; the face lasts as long as the sound (max 8 s, like Mochi).
    final folder = sound ? moodFolder[mood] : null;
    var faceMs = 3500;
    if (folder != null) {
      final file = 1 + _rnd.nextInt(2);
      faceMs = (250 + soundMs[folder][file - 1] + 200).clamp(900, 8000);
      player.play(AssetSource('sounds/${folder.toString().padLeft(2, '0')}_00$file.mp3'));
    } else {
      final tone = sound ? moodTone(mood) : null;
      if (tone != null) player.play(BytesSource(tone));
    }
    pv.faceUntil = DateTime.now().add(Duration(milliseconds: faceMs));
    notifyListeners();
  }

  void previewSong(int slot) {
    player.stop();
    pv
      ..songSlot = slot
      ..label = 'Song $slot'
      ..cues = cuesFor(lyrics[slot - 1], songDurationMs(slot))
      ..untilMs = songDurationMs(slot) + 300
      ..sw.reset()
      ..sw.start();
    player.play(AssetSource('audio/song$slot.mp3'));
    notifyListeners();
  }

  void stopPreview() {
    player.stop();
    pv
      ..songSlot = null
      ..sw.stop()
      ..face = 'normal'
      ..label = ''
      ..faceUntil = null
      ..faceSince = DateTime.now();
    notifyListeners();
  }

  /// Pet / Hug / Play / Sleep: sent to Mochi right away, and shown here too.
  Future<void> action(String what, String face, String label) async {
    previewMood(face, label: label);
    await api!.action(what);
  }

  Future<void> saveLyrics(int slot, String text) async {
    lyrics[slot - 1] = text.trim();
    final sp = await SharedPreferences.getInstance();
    await sp.setString('lyrics$slot', lyrics[slot - 1]);
    notifyListeners();
    if (api == null || !connected) return;
    if (lyrics[slot - 1].isEmpty) {
      await api!.resetLyrics(slot);
    } else {
      await api!.saveLyrics(slot, '${toMochiLyrics(lyrics[slot - 1], songDurationMs(slot))}\n');
    }
  }

  List<String> songTriggers(int slot) =>
      [
        for (final t in triggers)
          if ((t.type == 'song' && t.slot == slot) || (slot == 1 && t.type == 'default' && t.id == 'tap2')) triggerLabel(t.id)
      ];
}

final AppState app = AppState();
