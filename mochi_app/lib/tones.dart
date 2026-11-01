import 'dart:math' as math;
import 'dart:typed_data';

/// Short sample sounds for the phone preview (Mochi itself plays the mp3s on its SD card).
class _Note {
  const _Note(this.f, this.at, this.len, {this.to, this.square = false, this.vol = 0.35});
  final double f;
  final double at;
  final double len;
  final double? to;
  final bool square;
  final double vol;
}

const Map<String, List<_Note>> _patterns = {
  'happy': [_Note(660, 0, .12), _Note(880, .13, .18)],
  'excited': [_Note(600, 0, .08), _Note(750, .09, .08), _Note(900, .18, .08), _Note(1200, .27, .16)],
  'laugh': [_Note(820, 0, .11, to: 620), _Note(780, .16, .11, to: 580), _Note(740, .32, .11, to: 540), _Note(700, .48, .11, to: 500)],
  'wink': [_Note(500, 0, .14, to: 1100), _Note(1300, .17, .06, vol: .25)],
  'love': [_Note(523, 0, .28, vol: .3), _Note(784, .3, .42, vol: .3)],
  'shy': [_Note(980, 0, .16, to: 880, vol: .22), _Note(1080, .2, .2, to: 980, vol: .2)],
  'sad': [_Note(520, 0, .45, to: 390, vol: .3), _Note(390, .5, .55, to: 300, vol: .28)],
  'cry': [_Note(560, 0, .2, to: 440, vol: .28), _Note(560, .22, .2, to: 440, vol: .28), _Note(560, .44, .2, to: 440, vol: .28)],
  'angry': [_Note(160, 0, .18, square: true, vol: .2), _Note(140, .22, .28, square: true, vol: .2)],
  'sleepy': [_Note(440, 0, .9, to: 260, vol: .25)],
  'sleep': [_Note(440, 0, .9, to: 260, vol: .25)],
  'surprised': [_Note(400, 0, .22, to: 1400)],
  'dizzy': [_Note(700, 0, .09), _Note(520, .1, .09), _Note(700, .2, .09), _Note(520, .3, .09), _Note(700, .4, .09), _Note(520, .5, .09)],
  'confused': [_Note(600, 0, .15, to: 450), _Note(450, .18, .22, to: 750)],
};

final Map<String, Uint8List> _cache = {};

/// A small WAV file for the mood, or null when the mood has no sound.
Uint8List? moodTone(String mood) {
  final notes = _patterns[mood];
  if (notes == null) return null;
  return _cache.putIfAbsent(mood, () => _render(notes));
}

Uint8List _render(List<_Note> notes) {
  const rate = 22050;
  final end = notes.map((n) => n.at + n.len).reduce(math.max) + 0.05;
  final samples = Float64List((end * rate).ceil());
  for (final n in notes) {
    final start = (n.at * rate).round();
    final count = (n.len * rate).round();
    var phase = 0.0;
    for (var i = 0; i < count && start + i < samples.length; i++) {
      final p = i / count;
      final f = n.to == null ? n.f : n.f * math.pow(n.to! / n.f, p);
      phase += 2 * math.pi * f / rate;
      final wave = n.square ? (math.sin(phase) >= 0 ? 1.0 : -1.0) : math.sin(phase);
      final env = math.min(1.0, i / (0.015 * rate)) * math.pow(1 - p, 2);
      samples[start + i] += wave * env * n.vol;
    }
  }
  final data = ByteData(44 + samples.length * 2);
  // Play the preview loud: scale the loudest point to 95% of full volume (no distortion).
  final peak = samples.fold<double>(0, (m, s) => math.max(m, s.abs()));
  if (peak > 0) {
    for (var i = 0; i < samples.length; i++) {
      samples[i] *= 0.95 / peak;
    }
  }
  void str(int o, String s) {
    for (var i = 0; i < s.length; i++) {
      data.setUint8(o + i, s.codeUnitAt(i));
    }
  }

  str(0, 'RIFF');
  data.setUint32(4, 36 + samples.length * 2, Endian.little);
  str(8, 'WAVE');
  str(12, 'fmt ');
  data.setUint32(16, 16, Endian.little);
  data.setUint16(20, 1, Endian.little);
  data.setUint16(22, 1, Endian.little);
  data.setUint32(24, rate, Endian.little);
  data.setUint32(28, rate * 2, Endian.little);
  data.setUint16(32, 2, Endian.little);
  data.setUint16(34, 16, Endian.little);
  str(36, 'data');
  data.setUint32(40, samples.length * 2, Endian.little);
  for (var i = 0; i < samples.length; i++) {
    data.setInt16(44 + i * 2, (samples[i].clamp(-1.0, 1.0) * 32767).round(), Endian.little);
  }
  return data.buffer.asUint8List();
}
