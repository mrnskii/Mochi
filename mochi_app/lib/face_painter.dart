import 'dart:math' as math;
import 'dart:typed_data';
import 'package:flutter/material.dart';

/// Mochi's faces, drawn the same way as on the 128x64 OLED.
/// face: normal happy sad angry sleepy excited surprised dizzy love confused
///       wink laugh cry shy sleep waking     t: milliseconds since the face started
class FacePainter extends CustomPainter {
  FacePainter(this.face, this.t);
  final String face;
  final double t;

  late Canvas _c;
  static final Paint _on = Paint()..color = const Color(0xFFF4F4F5);
  static final Paint _off = Paint()..color = Colors.black;
  static final Paint _stroke = Paint()
    ..color = const Color(0xFFF4F4F5)
    ..style = PaintingStyle.stroke
    ..strokeWidth = 1.3;
  static final Paint _ringPaint = Paint()
    ..color = const Color(0xFFF4F4F5)
    ..style = PaintingStyle.stroke
    ..strokeWidth = 2;
  Paint _p(bool on) => on ? _on : _off;

  void _circ(double x, double y, double r, [bool on = true]) => _c.drawCircle(Offset(x, y), r, _p(on));
  void _rrect(double x, double y, double w, double h, double r, [bool on = true]) =>
      _c.drawRRect(RRect.fromLTRBR(x, y, x + w, y + h, Radius.circular(r)), _p(on));
  void _rect(double x, double y, double w, double h, [bool on = true]) => _c.drawRect(Rect.fromLTWH(x, y, w, h), _p(on));
  void _tri(double a, double b, double c, double d, double e, double f, [bool on = true]) =>
      _c.drawPath(Path()..moveTo(a, b)..lineTo(c, d)..lineTo(e, f)..close(), _p(on));
  void _line(double a, double b, double c, double d) => _c.drawLine(Offset(a, b), Offset(c, d), _stroke);
  void _ring(double x, double y, double r) => _c.drawCircle(Offset(x, y), r, _ringPaint);
  void _px(double x, double y) => _rect(x, y, 1, 1);
  void _txt(String s, double x, double y, double size) {
    final tp = TextPainter(
      text: TextSpan(text: s, style: TextStyle(color: const Color(0xFFF4F4F5), fontSize: size, fontWeight: FontWeight.bold, fontFamily: 'monospace')),
      textDirection: TextDirection.ltr,
    )..layout();
    tp.paint(_c, Offset(x, y));
  }

  void _heart(double cx, double cy, double r) {
    _circ(cx - r + 1, cy - r / 2, r);
    _circ(cx + r - 1, cy - r / 2, r);
    _tri(cx - 2 * r + 1, cy - r / 2 + 2, cx + 2 * r - 1, cy - r / 2 + 2, cx, cy + 2 * r);
  }

  void _smile(double y, double w) {
    for (double x = -w; x <= w; x++) {
      final dy = (x * x / (w + 2)).floorToDouble();
      final yy = y + (w / 2).floorToDouble() - (dy / 2).floorToDouble();
      _px(64 + x, yy);
      _px(64 + x, yy + 1);
    }
  }

  void _sadEye(bool l, double cx, double cy) {
    _rrect(cx - 15, cy - 14, 30, 30, 9);
    if (l) {
      _tri(cx - 17, cy - 16, cx + 17, cy - 16, cx - 17, cy + 2, false);
    } else {
      _tri(cx - 17, cy - 16, cx + 17, cy - 16, cx + 17, cy + 2, false);
    }
  }

  void _happyEye(double cx, double cy, [double b = 0]) {
    _circ(cx, cy + 4 + b, 15);
    _circ(cx, cy + 12 + b, 15, false);
  }

  void _eye(bool l, double cx, double cy) {
    switch (face) {
      case 'happy':
        _happyEye(cx, cy);
      case 'excited':
        final b = math.sin(t / 90) * 3;
        _rrect(cx - 16, cy - 19 + b, 32, 38, 12);
        _circ(cx + 6, cy - 8 + b, 4, false);
        _circ(cx - 5, cy + 6 + b, 2, false);
        if ((t / 150).floor() % 2 == 1) {
          final sx = l ? cx - 22 : cx + 22;
          _line(sx - 3, cy - 18, sx + 3, cy - 18);
          _line(sx, cy - 21, sx, cy - 15);
        }
      case 'sad':
        _sadEye(l, cx, cy);
        if (l) {
          final ty = cy + 16 + ((t / 45).floor() % 18);
          _circ(cx - 9, ty, 2);
          _tri(cx - 11, ty, cx - 7, ty, cx - 9, ty - 5);
        }
      case 'angry':
        final j = ((t / 60).floor() % 2).toDouble();
        _rrect(cx - 15 + j, cy - 12, 30, 28, 8);
        if (l) {
          _tri(cx - 17, cy - 14, cx + 17, cy - 14, cx + 17, cy + 4, false);
        } else {
          _tri(cx - 17, cy - 14, cx + 17, cy - 14, cx - 17, cy + 4, false);
        }
      case 'sleepy':
        _rrect(cx - 15, cy - 17, 30, 34, 9);
        _rect(cx - 16, cy - 18, 32, 18 + math.sin(t / 600) * 3, false);
      case 'waking':
        final p = math.min(1.0, (t % 2000) / 1100);
        _rrect(cx - 15, cy - 17, 30, 34, 9);
        _rect(cx - 16, cy - 18, 32, 36 * (1 - p), false);
      case 'sleep':
        _circ(cx, cy + 2, 13);
        _circ(cx, cy - 3, 13, false);
      case 'surprised':
        _ring(cx, cy, 16.5);
        _circ(cx, cy, 6);
      case 'dizzy':
        final ph = t / 120 * (l ? 1 : -1);
        for (double q = 0; q < 14; q += 0.3) {
          final r = q * 1.1;
          _circ(cx + math.cos(q + ph) * r, cy + math.sin(q + ph) * r, 1);
        }
      case 'love':
        _heart(cx, cy - 2, (t / 220).floor() % 2 == 1 ? 9 : 8);
      case 'wink':
        if (l) {
          _rrect(cx - 15, cy - 15, 30, 30, 9);
          _circ(cx + 5, cy - 6, 4, false);
        } else {
          _happyEye(cx, cy);
        }
      case 'laugh':
        final b = math.sin(t / 70) * 2;
        _happyEye(cx, cy, b);
        _circ(l ? cx - 20 : cx + 20, cy + 6 + b + ((t / 60).floor() % 8), 2);
      case 'cry':
        _sadEye(l, cx, cy);
        for (int k = 0; k < 2; k++) {
          final ty = cy + 16 + (((t / 30) + k * 11).floor() % 24);
          final tx = cx + (l ? -8 : 8) + (k == 1 ? 4 : -4);
          _circ(tx, ty, 2);
          _tri(tx - 2, ty, tx + 2, ty, tx, ty - 5);
        }
      case 'shy':
        _circ(cx, cy - 2, 14);
        _circ(cx, cy - 10, 14, false);
      case 'confused':
        if (l) {
          _rrect(cx - 15, cy - 15, 30, 30, 9);
        } else {
          _rrect(cx - 13, cy - 4, 26, 10, 4);
        }
      default: // normal, with blinking
        final ph = t % 3600;
        final open = ph < 160 ? (ph - 80).abs() / 80 : 1.0;
        final h = math.max(3.0, 34 * open);
        _rrect(cx - 15, cy - h / 2, 30, h, math.min(h / 2, 9));
    }
  }

  void _blush() {
    for (int k = 0; k < 3; k++) {
      _line(20.0 + k * 5, 50, 24.0 + k * 5, 45);
      _line(98.0 + k * 5, 50, 102.0 + k * 5, 45);
    }
  }

  void _mouth() {
    switch (face) {
      case 'happy':
      case 'wink':
        _smile(52, 6);
      case 'excited':
        _circ(64, 52, 6);
        _rect(57, 45, 15, 7, false);
      case 'sad':
        for (double x = -6; x <= 6; x++) {
          _px(64 + x, 56 - ((36 - x * x) / 12).floorToDouble());
        }
      case 'angry':
        _line(57, 55, 61, 52);
        _line(61, 52, 65, 55);
        _line(65, 55, 69, 52);
      case 'surprised':
        _ring(64, 54, 4);
      case 'dizzy':
        for (double x = -10; x <= 10; x++) {
          _px(64 + x, 54 + (2 * math.sin(x * 0.9 + t / 100)).roundToDouble());
        }
      case 'love':
        _smile(52, 5);
        _line(22, 50, 26, 46);
        _line(28, 50, 32, 46);
        _line(96, 50, 100, 46);
        _line(102, 50, 106, 46);
      case 'laugh':
        _circ(64, 50, 9);
        _rect(53, 38, 23, 12, false);
      case 'cry':
        final w = 5.0 + ((t / 120).floor() % 2);
        for (double x = -w; x <= w; x++) {
          _px(64 + x, 58 - ((w * w - x * x) / 6).floorToDouble());
        }
      case 'shy':
        _smile(54, 3);
        _blush();
      case 'confused':
        _txt('?', 112, 0, 16);
        _line(58, 54, 70, 52);
      case 'sleepy':
      case 'sleep':
        _txt('z', 106, 12 - ((t / 400).floor() % 12) / 2, 9);
        if (face == 'sleep') _txt('Z', 112, 6, 9);
    }
  }

  @override
  void paint(Canvas canvas, Size size) {
    _c = canvas;
    canvas.save();
    canvas.scale(size.width / 128, size.height / 64);
    canvas.clipRect(const Rect.fromLTWH(0, 0, 128, 64));
    _rect(0, 0, 128, 64, false);
    _eye(true, 38, 28);
    _eye(false, 90, 28);
    _mouth();
    canvas.restore();
  }

  @override
  bool shouldRepaint(covariant FacePainter old) => true;
}

/// One frame of a Mochi video (.mvid): 16-byte header, then 1024 bytes per 128x64 frame.
class VideoPainter extends CustomPainter {
  VideoPainter(this.data, this.frame);
  final Uint8List data;
  final int frame;

  @override
  void paint(Canvas canvas, Size size) {
    canvas.drawRect(Offset.zero & size, Paint()..color = Colors.black);
    final base = 16 + frame * 1024;
    if (base + 1024 > data.length) return;
    final sx = size.width / 128, sy = size.height / 64;
    final p = Paint()..color = const Color(0xFFEBEBEB);
    for (int q = 0; q < 8192; q++) {
      if ((data[base + (q >> 3)] & (128 >> (q & 7))) != 0) {
        canvas.drawRect(Rect.fromLTWH((q % 128) * sx, (q ~/ 128) * sy, sx + 0.5, sy + 0.5), p);
      }
    }
  }

  @override
  bool shouldRepaint(covariant VideoPainter old) => old.frame != frame || old.data != data;
}

/// Mochi's song screen, drawn like the firmware's drawMusicScreen(): floating notes,
/// happy eyes for the intro, then the dancing stick man and a bouncing happy face.
/// el: milliseconds since the song started.
class DancerPainter extends FacePainter {
  DancerPainter(double el) : super('happy', el);

  static final Paint _thick = Paint()
    ..color = const Color(0xFFF4F4F5)
    ..strokeWidth = 2.2
    ..strokeCap = StrokeCap.round;

  void _note(double x, double y) {
    _circ(x, y, 2);
    _line(x + 2, y, x + 2, y - 8);
    _line(x + 2, y - 8, x + 5, y - 6);
  }

  void _limb(double a, double b, double c, double d) => _c.drawLine(Offset(a, b), Offset(c, d), _thick);

  void _dancer(int frame) {
    final b = (frame == 1 || frame == 3) ? 2.0 : 0.0;
    final headY = 10 + b, sh = 24 + b, hip = 40 + b;
    _circ(64, headY, 7);
    _rect(61, headY + 7, 7, hip - headY - 6);
    switch (frame) {
      case 0:
        _limb(63, sh, 47, 13 + b); _limb(47, 13 + b, 42, 20 + b);
        _limb(67, sh, 82, 34 + b);
        _limb(63, hip, 49, 57); _limb(66, hip, 82, 53);
      case 1:
        _limb(63, sh, 46, 34 + b);
        _limb(67, sh, 82, 13 + b); _limb(82, 13 + b, 87, 20 + b);
        _limb(63, hip, 47, 52); _limb(66, hip, 80, 58);
      case 2:
        _limb(63, sh, 48, 10); _limb(67, sh, 82, 10);
        _limb(63, hip, 45, 56); _limb(66, hip, 84, 56);
      default:
        _limb(63, sh, 45, 27 + b); _limb(67, sh, 84, 27 + b);
        _limb(63, hip, 50, 50); _limb(50, 50, 43, 48);
        _limb(66, hip, 79, 50); _limb(79, 50, 86, 48);
    }
  }

  @override
  void paint(Canvas canvas, Size size) {
    _c = canvas;
    canvas.save();
    canvas.scale(size.width / 128, size.height / 64);
    canvas.clipRect(const Rect.fromLTWH(0, 0, 128, 64));
    _rect(0, 0, 128, 64, false);
    for (var i = 0; i < 3; i++) {
      final y = 60 - ((t / 30 + i * 22) % 64);
      _note(i.isOdd ? 112 : 10, y);
      _note(i.isOdd ? 20 : 104, (y + 30) % 64);
    }
    if (t < 1500) {
      final bob = math.sin(t / 150) * 3;
      _eye(true, 38, 28 + bob);
      _eye(false, 90, 28 + bob);
    } else {
      final beat = t ~/ 400;
      if ((beat ~/ 16).isEven) {
        _dancer(beat % 4);
      } else {
        final bounce = beat.isOdd ? -3.0 : 2.0;
        _eye(true, 38, 28 + bounce);
        _eye(false, 90, 28 + bounce);
        _smile(52 + bounce, 6);
      }
    }
    canvas.restore();
  }
}
