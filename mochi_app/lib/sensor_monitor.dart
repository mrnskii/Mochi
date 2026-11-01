import 'dart:math' as math;
import 'package:flutter/material.dart';
import 'app_state.dart';
import 'face_painter.dart';

/// Live readings from Mochi's sensors (updated every second over Bluetooth).
class SensorMonitor extends StatelessWidget {
  const SensorMonitor({super.key});

  // Rebuild by itself every time new readings arrive (this widget is const in the Home page).
  @override
  Widget build(BuildContext context) => ListenableBuilder(listenable: app, builder: (context, _) => _build(context));

  Widget _build(BuildContext context) {
    final s = app.status;
    final theme = Theme.of(context);
    final touch = s['touch'] == true;
    final light = ((s['light'] as num?) ?? 0).toDouble();
    final tilt = ((s['tilt'] as num?) ?? 0).toDouble();
    final accel = ((s['accel'] as num?) ?? 0).toDouble();
    final gyro = ((s['gyro'] as num?) ?? 0).toDouble();
    final mpuOk = s['mpuOk'] != false;
    final lightOk = s['lightOk'] != false;
    final moving = app.motionHist.isNotEmpty && app.motionHist.last > 1.5;

    Widget tile({required IconData icon, required String title, required Widget value, bool ok = true, Widget? extra}) => Container(
          padding: const EdgeInsets.all(12),
          decoration: BoxDecoration(color: theme.colorScheme.surfaceContainerHighest.withValues(alpha: 0.5), borderRadius: BorderRadius.circular(14)),
          child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
            Row(children: [
              Icon(icon, size: 18, color: theme.colorScheme.primary),
              const SizedBox(width: 6),
              Text(title, style: theme.textTheme.labelLarge),
              const Spacer(),
              Icon(ok ? Icons.circle : Icons.error, size: 10, color: ok ? Colors.green : Colors.red),
            ]),
            const SizedBox(height: 6),
            value,
            if (extra != null) ...[const SizedBox(height: 8), extra],
          ]),
        );

    return Card(
      child: Padding(
        padding: const EdgeInsets.all(14),
        child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
          Row(children: [
            Text('Monitor', style: theme.textTheme.titleMedium),
            const Spacer(),
            Text('live · every 1 s', style: theme.textTheme.bodySmall),
          ]),
          const SizedBox(height: 10),
          Text('Sensors', style: theme.textTheme.titleSmall),
          const SizedBox(height: 8),
          tile(
            icon: Icons.touch_app,
            title: 'Touch',
            value: Text(touch ? 'Touched' : 'Not touched',
                style: theme.textTheme.titleLarge?.copyWith(color: touch ? theme.colorScheme.primary : null)),
          ),
          const SizedBox(height: 8),
          tile(
            icon: Icons.light_mode,
            title: 'Light',
            ok: lightOk,
            value: Row(crossAxisAlignment: CrossAxisAlignment.end, children: [
              Text('${light.round()}%', style: theme.textTheme.titleLarge),
              const SizedBox(width: 8),
              Text('${s['night'] == true ? 'Night' : 'Day'} · raw ${s['lightRaw'] ?? '-'}', style: theme.textTheme.bodySmall),
            ]),
            extra: _Spark(values: app.lightHist, max: 100, color: Colors.amber.shade700),
          ),
          const SizedBox(height: 8),
          tile(
            icon: Icons.screen_rotation,
            title: 'Motion',
            ok: mpuOk,
            value: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
              Text(
                !mpuOk ? 'Sensor not answering' : s['upside'] == true ? 'Upside down' : moving ? 'Moving' : 'Still',
                style: theme.textTheme.titleLarge,
              ),
              Text('accel ${accel.toStringAsFixed(1)} m/s² · turn ${(gyro * 57.3).round()}°/s', style: theme.textTheme.bodySmall),
              const SizedBox(height: 8),
              Row(children: [
                const Text('Tilt  L', style: TextStyle(fontSize: 12)),
                const SizedBox(width: 6),
                Expanded(child: _TiltBar(value: tilt)),
                const SizedBox(width: 6),
                const Text('R', style: TextStyle(fontSize: 12)),
              ]),
            ]),
            extra: _Spark(values: app.motionHist, max: 20, color: theme.colorScheme.primary),
          ),
          const SizedBox(height: 16),
          Text('Outputs', style: theme.textTheme.titleSmall),
          const SizedBox(height: 8),
          tile(
            icon: Icons.tv,
            title: 'OLED screen',
            ok: s['oled'] != 'off',
            value: Row(children: [
              SizedBox(
                width: 96,
                child: AspectRatio(
                  aspectRatio: 2,
                  child: ClipRRect(
                    borderRadius: BorderRadius.circular(6),
                    child: CustomPaint(painter: FacePainter('${s['expr'] ?? 'normal'}', 700)),
                  ),
                ),
              ),
              const SizedBox(width: 12),
              Expanded(
                child: Text(
                  switch (s['oled']) {
                    'video' => 'Song video (song ${s['song']})',
                    'dance' => 'Dancing (song ${s['song']})',
                    'sleep' => 'Sleeping',
                    'off' => 'Not answering',
                    _ => 'Face: ${s['expr'] ?? '-'}',
                  },
                  style: theme.textTheme.titleMedium,
                ),
              ),
            ]),
          ),
          const SizedBox(height: 8),
          tile(
            icon: Icons.notes,
            title: 'LCD screen',
            ok: s['lcdOk'] != false,
            value: Container(
              width: double.infinity,
              padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 8),
              decoration: BoxDecoration(color: const Color(0xFF9BC33B), borderRadius: BorderRadius.circular(6)),
              child: Text(
                '${(s['lcd1'] ?? '').toString().padRight(16)}\n${(s['lcd2'] ?? '').toString().padRight(16)}',
                style: const TextStyle(fontFamily: 'monospace', fontSize: 15, height: 1.3, color: Color(0xFF1F2A0B), fontWeight: FontWeight.w600),
              ),
            ),
          ),
          const SizedBox(height: 8),
          tile(
            icon: Icons.volume_up,
            title: 'Speaker (DFPlayer)',
            ok: s['df'] != false,
            value: Text(
              s['soundOn'] == false
                  ? 'Sounds turned off'
                  : s['audioPlaying'] == true
                      ? 'Playing ${s['audioFile']}'
                      : 'Idle${(s['audioFile'] ?? '') != '' ? ' · last ${s['audioFile']}' : ''}',
              style: theme.textTheme.titleMedium,
            ),
            extra: Text('Volume ${s['volume'] ?? '-'} / 30', style: theme.textTheme.bodySmall),
          ),
          const SizedBox(height: 8),
          tile(
            icon: Icons.vibration,
            title: 'Vibration motor',
            value: Text(
              s['vibOn'] == false ? 'Turned off' : s['vib'] == true ? 'Vibrating' : 'Off',
              style: theme.textTheme.titleMedium,
            ),
            extra: Text('Pulses since start: ${s['vibCount'] ?? 0}', style: theme.textTheme.bodySmall),
          ),
        ]),
      ),
    );
  }
}

class _TiltBar extends StatelessWidget {
  const _TiltBar({required this.value});
  final double value; // -1 .. 1
  @override
  Widget build(BuildContext context) => LayoutBuilder(builder: (context, c) {
        final w = c.maxWidth;
        final x = (value.clamp(-1.0, 1.0) + 1) / 2 * (w - 14);
        return SizedBox(
          height: 14,
          child: Stack(children: [
            Positioned.fill(top: 5, bottom: 5, child: DecoratedBox(decoration: BoxDecoration(color: Colors.black12, borderRadius: BorderRadius.circular(4)))),
            Positioned(left: w / 2 - 1, top: 2, bottom: 2, child: Container(width: 2, color: Colors.black26)),
            Positioned(left: x, child: Container(width: 14, height: 14, decoration: BoxDecoration(color: Theme.of(context).colorScheme.primary, shape: BoxShape.circle))),
          ]),
        );
      });
}

/// Small line chart of the last 60 seconds.
class _Spark extends StatelessWidget {
  const _Spark({required this.values, required this.max, required this.color});
  final List<double> values;
  final double max;
  final Color color;
  @override
  Widget build(BuildContext context) => SizedBox(height: 40, width: double.infinity, child: CustomPaint(painter: _SparkPainter(values, max, color)));
}

class _SparkPainter extends CustomPainter {
  _SparkPainter(this.values, this.max, this.color);
  final List<double> values;
  final double max;
  final Color color;

  @override
  void paint(Canvas canvas, Size size) {
    canvas.drawLine(Offset(0, size.height), Offset(size.width, size.height), Paint()..color = Colors.black12);
    if (values.length < 2) return;
    final path = Path();
    for (var i = 0; i < values.length; i++) {
      final x = size.width * i / (values.length - 1);
      final y = size.height - (math.min(values[i], max) / max) * size.height;
      if (i == 0) {
        path.moveTo(x, y);
      } else {
        path.lineTo(x, y);
      }
    }
    canvas.drawPath(path, Paint()
      ..color = color
      ..style = PaintingStyle.stroke
      ..strokeWidth = 2);
  }

  @override
  bool shouldRepaint(covariant _SparkPainter old) => true;
}
