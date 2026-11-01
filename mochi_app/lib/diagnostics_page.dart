import 'dart:math' as math;
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'app_state.dart';

/// Health, communication test, and event log. Use it to collect test evidence for the report.
class DiagnosticsPage extends StatefulWidget {
  const DiagnosticsPage({super.key});
  @override
  State<DiagnosticsPage> createState() => _DiagnosticsPageState();
}

class _DiagnosticsPageState extends State<DiagnosticsPage> {
  Map<String, dynamic>? stats;
  int? rssi;
  String? statsError;
  int testCount = 50;
  bool testing = false;
  int sent = 0;
  final List<int?> trials = []; // null = no answer
  String log = '';
  bool loadingLog = false;

  @override
  void initState() {
    super.initState();
    _loadStats();
  }

  void _toast(String m) => ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text(m)));

  Future<void> _loadStats() async {
    try {
      final s = await app.api!.stats();
      int? r;
      try {
        r = await app.device?.readRssi();
      } catch (_) {}
      if (mounted) {
        setState(() {
          stats = s;
          rssi = r;
          statsError = null;
        });
      }
    } catch (e) {
      if (mounted) setState(() => statsError = '$e');
    }
  }

  Future<void> _runTest() async {
    setState(() {
      testing = true;
      sent = 0;
      trials.clear();
    });
    app.link?.paused = true; // stop status polling so it does not disturb the timing
    for (var i = 0; i < testCount && mounted; i++) {
      sent++;
      try {
        trials.add(await app.api!.pingMs());
      } catch (_) {
        trials.add(null);
      }
      if (mounted) setState(() {});
      await Future<void>.delayed(const Duration(milliseconds: 100));
    }
    app.link?.paused = false;
    if (mounted) setState(() => testing = false);
  }

  List<int> get _ok => trials.whereType<int>().toList();

  String _csv() {
    final b = StringBuffer('trial,latency_ms,result\n');
    for (var i = 0; i < trials.length; i++) {
      b.writeln('${i + 1},${trials[i] ?? ''},${trials[i] == null ? 'lost' : 'ok'}');
    }
    final ok = _ok;
    if (ok.isNotEmpty) {
      b.writeln('summary,sent=${trials.length},received=${ok.length},success=${(ok.length * 100 / trials.length).toStringAsFixed(1)}%,'
          'avg=${(ok.reduce((a, c) => a + c) / ok.length).toStringAsFixed(1)}ms,min=${ok.reduce(math.min)}ms,max=${ok.reduce(math.max)}ms');
    }
    return b.toString();
  }

  Future<void> _loadLog() async {
    setState(() => loadingLog = true);
    try {
      final t = await app.api!.logText();
      if (mounted) setState(() => log = t.trim().isEmpty ? '(log is empty)' : t);
    } catch (e) {
      if (mounted) _toast('Failed: $e');
    }
    if (mounted) setState(() => loadingLog = false);
  }

  Widget _row(String label, Object? value) => Padding(
        padding: const EdgeInsets.symmetric(vertical: 3),
        child: Row(children: [
          Expanded(child: Text(label, style: Theme.of(context).textTheme.bodyMedium)),
          Text('${value ?? '-'}', style: const TextStyle(fontWeight: FontWeight.w600)),
        ]),
      );

  Widget _chip(String label, bool? ok) => Chip(
        avatar: Icon(ok == true ? Icons.check_circle : Icons.error, color: ok == true ? Colors.green : Colors.red, size: 18),
        label: Text(label),
      );

  String _uptime(num s) {
    final d = Duration(seconds: s.toInt());
    return '${d.inHours}h ${d.inMinutes % 60}m ${d.inSeconds % 60}s';
  }

  @override
  Widget build(BuildContext context) {
    final s = stats;
    final sensors = (s?['sensors'] as Map<String, dynamic>?) ?? const {};
    final ok = _ok;
    return Scaffold(
      appBar: AppBar(title: const Text('Diagnostics')),
      body: ListView(padding: const EdgeInsets.all(12), children: [
        Card(
          child: Padding(
            padding: const EdgeInsets.all(14),
            child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
              Row(children: [
                Text('Mochi health', style: Theme.of(context).textTheme.titleMedium),
                const Spacer(),
                IconButton(onPressed: _loadStats, icon: const Icon(Icons.refresh)),
              ]),
              if (statsError != null) Text(statsError!, style: TextStyle(color: Theme.of(context).colorScheme.error)),
              if (s != null) ...[
                Wrap(spacing: 6, runSpacing: 6, children: [
                  _chip('OLED', sensors['oled'] as bool?),
                  _chip('LCD', sensors['lcd'] as bool?),
                  _chip('MPU6050', sensors['mpu'] as bool?),
                  _chip('DFPlayer', sensors['dfplayer'] as bool?),
                  _chip('Storage', sensors['storage'] as bool?),
                  _chip('Light sensor', sensors['lightOk'] as bool?),
                ]),
                const Divider(height: 24),
                _row('Uptime', _uptime(s['uptimeS'] as num)),
                _row('Boot count', s['boots']),
                _row('Last reset', s['reset']),
                _row('Free memory (lowest)', '${s['heap']} B (${s['minHeap']} B)'),
                _row('Main loop time avg / max (10 s)', '${s['loopAvgUs']} us / ${s['loopMaxUs']} us'),
                _row('Event -> OLED last / max', '${s['respOledMs']} ms / ${s['respOledMaxMs']} ms'),
                _row('Event -> sound command last / max', '${s['respAudioMs']} ms / ${s['respAudioMaxMs']} ms'),
                _row('Events measured', s['respCount']),
                _row('Requests ok / failed', '${s['reqOk']} / ${s['reqErr']}'),
                _row('Wrong PIN attempts', s['authFail']),
                _row('Bluetooth connects / drops', '${s['bleConnects']} / ${s['bleDrops']}'),
                _row('Signal (RSSI)', rssi == null ? '-' : '$rssi dBm'),
                _row('App reconnects / last time', '${app.drops} / ${app.lastReconnectMs == null ? '-' : '${app.lastReconnectMs} ms'}'),
                _row('Storage used', '${s['fsUsed']} / ${s['fsTotal']} B'),
                _row('Clock synced from phone', s['timeSynced'] == true ? 'yes' : 'no'),
              ],
            ]),
          ),
        ),
        Card(
          child: Padding(
            padding: const EdgeInsets.all(14),
            child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
              Text('Communication test', style: Theme.of(context).textTheme.titleMedium),
              const SizedBox(height: 4),
              const Text('Sends small messages to Mochi and measures how many come back and how long each takes.'),
              const SizedBox(height: 8),
              Row(children: [
                SegmentedButton<int>(
                  segments: const [
                    ButtonSegment(value: 20, label: Text('20')),
                    ButtonSegment(value: 50, label: Text('50')),
                    ButtonSegment(value: 100, label: Text('100')),
                  ],
                  selected: {testCount},
                  onSelectionChanged: testing ? null : (v) => setState(() => testCount = v.first),
                ),
                const Spacer(),
                FilledButton(onPressed: testing ? null : _runTest, child: Text(testing ? '$sent / $testCount' : 'Run')),
              ]),
              if (testing) Padding(padding: const EdgeInsets.only(top: 8), child: LinearProgressIndicator(value: sent / testCount)),
              if (trials.isNotEmpty) ...[
                const Divider(height: 24),
                _row('Sent / received', '${trials.length} / ${ok.length}'),
                _row('Success rate', '${(ok.length * 100 / trials.length).toStringAsFixed(1)} %'),
                if (ok.isNotEmpty) ...[
                  _row('Average latency', '${(ok.reduce((a, c) => a + c) / ok.length).toStringAsFixed(1)} ms'),
                  _row('Min / max latency', '${ok.reduce(math.min)} ms / ${ok.reduce(math.max)} ms'),
                ],
                Align(
                  alignment: Alignment.centerRight,
                  child: TextButton.icon(
                    onPressed: testing
                        ? null
                        : () {
                            Clipboard.setData(ClipboardData(text: _csv()));
                            _toast('Results copied (CSV). Paste them into your report.');
                          },
                    icon: const Icon(Icons.copy),
                    label: const Text('Copy results'),
                  ),
                ),
              ],
            ]),
          ),
        ),
        Card(
          child: Padding(
            padding: const EdgeInsets.all(14),
            child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
              Row(children: [
                Text('Event log', style: Theme.of(context).textTheme.titleMedium),
                const Spacer(),
                TextButton(onPressed: loadingLog ? null : _loadLog, child: Text(loadingLog ? 'Loading...' : 'Load')),
              ]),
              const Text('Saved on Mochi: boots, sensor checks, faults, recovery, every touch / motion / light event, Bluetooth, and wrong PINs.'),
              if (log.isNotEmpty) ...[
                const SizedBox(height: 8),
                Container(
                  constraints: const BoxConstraints(maxHeight: 280),
                  padding: const EdgeInsets.all(8),
                  decoration: BoxDecoration(color: Colors.black.withValues(alpha: 0.04), borderRadius: BorderRadius.circular(8)),
                  child: SingleChildScrollView(child: SelectableText(log, style: const TextStyle(fontFamily: 'monospace', fontSize: 11))),
                ),
                Row(mainAxisAlignment: MainAxisAlignment.end, children: [
                  TextButton.icon(
                    onPressed: () {
                      Clipboard.setData(ClipboardData(text: 'boot,uptime_ms,unix_time,type,detail\n$log'));
                      _toast('Log copied (CSV)');
                    },
                    icon: const Icon(Icons.copy),
                    label: const Text('Copy'),
                  ),
                  TextButton(
                    onPressed: () async {
                      try {
                        await app.api!.clearLog();
                        setState(() => log = '');
                        _toast('Log cleared');
                      } catch (e) {
                        _toast('Failed: $e');
                      }
                    },
                    child: const Text('Clear'),
                  ),
                ]),
              ],
            ]),
          ),
        ),
      ]),
    );
  }
}
