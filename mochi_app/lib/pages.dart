import 'package:flutter/material.dart';
import 'app_state.dart';
import 'diagnostics_page.dart';
import 'face_painter.dart';
import 'mochi_api.dart';
import 'sensor_monitor.dart';

void toast(BuildContext c, String m) => ScaffoldMessenger.of(c).showSnackBar(SnackBar(content: Text(m)));

/// Runs a Mochi command and shows the result.
Future<void> guard(BuildContext c, Future<void> Function() job, String ok) async {
  try {
    await job();
    if (c.mounted) toast(c, ok);
  } catch (e) {
    if (c.mounted) toast(c, 'Failed: $e');
  }
}

/// A still picture of one of Mochi's faces.
class FaceThumb extends StatelessWidget {
  const FaceThumb({super.key, required this.face, this.width, this.radius = 6});
  final String face;
  final double? width;
  final double radius;
  @override
  Widget build(BuildContext context) => SizedBox(
        width: width,
        child: AspectRatio(
          aspectRatio: 2,
          child: ClipRRect(borderRadius: BorderRadius.circular(radius), child: CustomPaint(painter: FacePainter(face, 700))),
        ),
      );
}

Widget sectionTitle(BuildContext context, String text, {String? subtitle}) => Padding(
      padding: const EdgeInsets.only(top: 18, bottom: 8),
      child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
        Text(text, style: Theme.of(context).textTheme.titleSmall?.copyWith(fontWeight: FontWeight.w600)),
        if (subtitle != null) Text(subtitle, style: Theme.of(context).textTheme.bodySmall),
      ]),
    );

// =====================================================================
//  HOME: love meter, Pet / Hug / Play / Sleep, monitor
// =====================================================================
class HomePage extends StatelessWidget {
  const HomePage({super.key});

  static const actions = [('Pet', 'pet', 'wink'), ('Hug', 'love', 'love'), ('Play', 'excited', 'laugh'), ('Sleep', 'sleep', 'sleep')];

  @override
  Widget build(BuildContext context) => ListenableBuilder(
        listenable: app,
        builder: (context, _) {
          final love = ((app.status['love'] as num?) ?? 0).toInt();
          return ListView(padding: const EdgeInsets.fromLTRB(16, 0, 16, 24), children: [
            Card(
              child: Padding(
                padding: const EdgeInsets.all(16),
                child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
                  Text("Hi! I'm ${app.settings['name'] ?? 'Mochi'}",
                      style: Theme.of(context).textTheme.titleMedium?.copyWith(fontWeight: FontWeight.bold)),
                  const SizedBox(height: 8),
                  const Text('Love meter'),
                  Row(crossAxisAlignment: CrossAxisAlignment.end, children: [
                    Text('$love%', style: Theme.of(context).textTheme.headlineMedium?.copyWith(fontWeight: FontWeight.bold)),
                    const Spacer(),
                    Text('Mood: ${moodLabel['${app.status['expr']}'] ?? app.status['expr'] ?? '-'}'),
                  ]),
                  const SizedBox(height: 8),
                  ClipRRect(borderRadius: BorderRadius.circular(8), child: LinearProgressIndicator(value: love / 100, minHeight: 10)),
                ]),
              ),
            ),
            sectionTitle(context, 'Play with Mochi', subtitle: 'Sent to Mochi right away. No Save needed.'),
            GridView.count(
              crossAxisCount: 2,
              shrinkWrap: true,
              physics: const NeverScrollableScrollPhysics(),
              mainAxisSpacing: 10,
              crossAxisSpacing: 10,
              childAspectRatio: 1.55,
              children: [
                for (final a in actions)
                  Card(
                    clipBehavior: Clip.antiAlias,
                    child: InkWell(
                      onTap: () => guard(context, () => app.action(a.$2, a.$3, a.$1), '${a.$1}: sent to Mochi'),
                      child: Padding(
                        padding: const EdgeInsets.all(10),
                        child: Column(mainAxisAlignment: MainAxisAlignment.center, children: [
                          FaceThumb(face: a.$3),
                          const SizedBox(height: 6),
                          Text(a.$1, style: const TextStyle(fontWeight: FontWeight.w600)),
                        ]),
                      ),
                    ),
                  ),
              ],
            ),
            const SizedBox(height: 16),
            const SensorMonitor(),
          ]);
        },
      );
}

// =====================================================================
//  REACTIONS: tap a trigger, pick a mood or song, see and hear it right away
// =====================================================================
/// 2 taps with the "Normal" choice plays Song 1 on Mochi.
bool playsDefaultSong(TriggerCfg t) => t.type == 'default' && t.id == 'tap2';

String choiceText(TriggerCfg t) => switch (t.type) {
      'mood' => moodLabel[t.mood] ?? t.mood,
      'song' => 'Song ${t.slot}',
      'none' => 'Nothing',
      'sound' => 'Sound',
      _ => playsDefaultSong(t) ? 'Song 1' : 'Normal',
    };

class ReactionsPage extends StatelessWidget {
  const ReactionsPage({super.key});

  @override
  Widget build(BuildContext context) => ListenableBuilder(
        listenable: app,
        builder: (context, _) => ListView(padding: const EdgeInsets.fromLTRB(16, 0, 16, 24), children: [
          const Text('Tap one to choose what Mochi does. You will see and hear it right away.'),
          for (final g in triggerGroups) ...[
            sectionTitle(context, g.$1),
            Card(
              clipBehavior: Clip.antiAlias,
              child: Column(children: [
                for (final (i, item) in g.$2.indexed)
                  if (app.triggers.where((t) => t.id == item.$1).isNotEmpty)
                    _TriggerRow(t: app.triggers.firstWhere((t) => t.id == item.$1), label: item.$2, first: i == 0),
              ]),
            ),
          ],
        ]),
      );
}

class _TriggerRow extends StatelessWidget {
  const _TriggerRow({required this.t, required this.label, required this.first});
  final TriggerCfg t;
  final String label;
  final bool first;

  @override
  Widget build(BuildContext context) {
    final dirty = app.savedTriggers[t.id] != t.state;
    final face = switch (t.type) { 'mood' => t.mood, 'none' => 'normal', _ => builtInFace[t.id] ?? 'happy' };
    return InkWell(
      onTap: () {
        app.previewTrigger(t);
        showModalBottomSheet<void>(
          context: context,
          isScrollControlled: true,
          showDragHandle: true,
          backgroundColor: Colors.white,
          barrierColor: Colors.black.withValues(alpha: 0.08), // keep Mochi's screen visible
          builder: (_) => _ChoiceSheet(t: t, label: label),
        );
      },
      child: Container(
        decoration: BoxDecoration(border: first ? null : const Border(top: BorderSide(color: Color(0xFFF1F5F9)))),
        padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 12),
        child: Row(children: [
          Expanded(child: Text(label, style: const TextStyle(fontSize: 16))),
          if (dirty) const Padding(padding: EdgeInsets.only(right: 8), child: Icon(Icons.circle, size: 8, color: Colors.orange)),
          if (t.vib == 1)
            Padding(padding: const EdgeInsets.only(right: 6), child: Icon(Icons.vibration, size: 18, color: Theme.of(context).colorScheme.primary)),
          if (t.vib == 2) const Padding(padding: EdgeInsets.only(right: 6), child: Icon(Icons.mobile_off, size: 18, color: Color(0xFF94A3B8))),
          Text(choiceText(t), style: TextStyle(color: Theme.of(context).colorScheme.primary, fontWeight: FontWeight.w600)),
          const SizedBox(width: 10),
          t.type == 'song' || playsDefaultSong(t)
              ? Container(
                  width: 48,
                  height: 24,
                  decoration: BoxDecoration(color: Theme.of(context).colorScheme.primary, borderRadius: BorderRadius.circular(6)),
                  child: const Icon(Icons.music_note, size: 16, color: Colors.white),
                )
              : FaceThumb(face: face, width: 48),
        ]),
      ),
    );
  }
}

class _ChoiceSheet extends StatelessWidget {
  const _ChoiceSheet({required this.t, required this.label});
  final TriggerCfg t;
  final String label;

  Widget _option(BuildContext context, {required bool on, required VoidCallback onTap, required Widget child}) => InkWell(
        borderRadius: BorderRadius.circular(12),
        onTap: onTap,
        child: Container(
          padding: const EdgeInsets.all(6),
          decoration: BoxDecoration(
            color: on ? Theme.of(context).colorScheme.primaryContainer : null,
            borderRadius: BorderRadius.circular(12),
            border: Border.all(color: on ? Theme.of(context).colorScheme.primary : const Color(0xFFE2E8F0), width: on ? 2 : 1),
          ),
          child: child,
        ),
      );

  @override
  Widget build(BuildContext context) => ListenableBuilder(
        listenable: app,
        builder: (context, _) => ConstrainedBox(
          // Leave Mochi's screen (top of the app) visible so the picked face shows while choosing.
          constraints: BoxConstraints(maxHeight: (MediaQuery.of(context).size.height - 330).clamp(300.0, 900.0)),
          child: SingleChildScrollView(
            padding: const EdgeInsets.fromLTRB(16, 0, 16, 24),
            child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
              Row(children: [
                Expanded(child: Text(label, style: Theme.of(context).textTheme.titleLarge?.copyWith(fontWeight: FontWeight.bold))),
                FilledButton(onPressed: () => Navigator.pop(context), child: const Text('Done')),
              ]),
              sectionTitle(context, 'Mood'),
              GridView.count(
                crossAxisCount: 3,
                shrinkWrap: true,
                physics: const NeverScrollableScrollPhysics(),
                mainAxisSpacing: 8,
                crossAxisSpacing: 8,
                childAspectRatio: 1.3,
                children: [
                  for (final m in moods)
                    _option(
                      context,
                      on: t.type == 'mood' && t.mood == m,
                      onTap: () => app.choose(t, 'mood', mood: m),
                      child: Column(mainAxisAlignment: MainAxisAlignment.center, children: [
                        FaceThumb(face: m),
                        const SizedBox(height: 4),
                        Text(moodLabel[m] ?? m, style: const TextStyle(fontSize: 13)),
                      ]),
                    ),
                ],
              ),
              // Falling asleep cannot start a song (it would wake Mochi up again).
              if (t.id != 'sleep') sectionTitle(context, 'Song'),
              if (t.id != 'sleep') Row(children: [
                for (var s = 1; s <= 3; s++)
                  Expanded(
                    child: Padding(
                      padding: EdgeInsets.only(right: s < 3 ? 8 : 0),
                      child: _option(
                        context,
                        on: t.type == 'song' && t.slot == s,
                        onTap: () => app.choose(t, 'song', slot: s),
                        child: Padding(
                          padding: const EdgeInsets.symmetric(vertical: 8),
                          child: Row(mainAxisAlignment: MainAxisAlignment.center, children: [
                            const Icon(Icons.music_note, size: 18),
                            Text('Song $s'),
                          ]),
                        ),
                      ),
                    ),
                  ),
              ]),
              sectionTitle(context, 'Other'),
              Row(children: [
                Expanded(
                  child: _option(
                    context,
                    on: t.type == 'default',
                    onTap: () => app.choose(t, 'default'),
                    child: const Padding(padding: EdgeInsets.symmetric(vertical: 8), child: Center(child: Text('Normal (Mochi decides)'))),
                  ),
                ),
                const SizedBox(width: 8),
                Expanded(
                  child: _option(
                    context,
                    on: t.type == 'none',
                    onTap: () => app.choose(t, 'none'),
                    child: const Padding(padding: EdgeInsets.symmetric(vertical: 8), child: Center(child: Text('Do nothing'))),
                  ),
                ),
              ]),
              sectionTitle(context, 'Vibration'),
              Row(children: [
                for (final (v, icon, text) in const [
                  (0, Icons.auto_awesome_outlined, 'Normal'),
                  (1, Icons.vibration, 'Vibrate'),
                  (2, Icons.mobile_off, 'No vibration'),
                ])
                  Expanded(
                    child: Padding(
                      padding: EdgeInsets.only(right: v < 2 ? 8 : 0),
                      child: _option(
                        context,
                        on: t.vib == v,
                        onTap: () => app.chooseVib(t, v),
                        child: Padding(
                          padding: const EdgeInsets.symmetric(vertical: 8),
                          child: Column(children: [
                            Icon(icon, size: 20),
                            const SizedBox(height: 2),
                            Text(text, style: const TextStyle(fontSize: 13)),
                          ]),
                        ),
                      ),
                    ),
                  ),
              ]),
              Padding(
                padding: const EdgeInsets.only(top: 8),
                child: Text(
                  'Normal: Mochi vibrates only for some reactions. Vibrate: a strong buzz every time. '
                  'Press Save to Mochi to keep it.',
                  style: Theme.of(context).textTheme.bodySmall?.copyWith(color: const Color(0xFF64748B)),
                ),
              ),
            ]),
          ),
        ),
      );
}

// =====================================================================
//  SONGS: one tap = video + lyrics + sound; lyrics are timed by themselves
// =====================================================================
class SongsPage extends StatelessWidget {
  const SongsPage({super.key});

  @override
  Widget build(BuildContext context) => ListenableBuilder(
        listenable: app,
        builder: (context, _) => ListView(padding: const EdgeInsets.fromLTRB(16, 0, 16, 24), children: [
          const Text('Tap a song to preview it. The video, lyrics, and sound play together.'),
          const SizedBox(height: 12),
          for (var slot = 1; slot <= 3; slot++) ...[_SongCard(slot: slot), const SizedBox(height: 10)],
          Text('To make a touch play a song, go to Reactions and pick the song.', style: Theme.of(context).textTheme.bodySmall),
        ]),
      );
}

class _SongCard extends StatelessWidget {
  const _SongCard({required this.slot});
  final int slot;

  void _editLyrics(BuildContext context) {
    final ctrl = TextEditingController(text: app.lyrics[slot - 1]);
    showModalBottomSheet<void>(
      context: context,
      isScrollControlled: true,
      showDragHandle: true,
      backgroundColor: Colors.white,
      builder: (ctx) => Padding(
        padding: EdgeInsets.fromLTRB(16, 0, 16, 16 + MediaQuery.of(ctx).viewInsets.bottom),
        child: Column(mainAxisSize: MainAxisSize.min, crossAxisAlignment: CrossAxisAlignment.start, children: [
          Text('Song $slot lyrics', style: Theme.of(ctx).textTheme.titleLarge?.copyWith(fontWeight: FontWeight.bold)),
          const SizedBox(height: 6),
          const Text('Type or paste the lyrics, one line per row. They show in order while the song plays, on the phone and on Mochi\'s LCD.'),
          const SizedBox(height: 10),
          TextField(
            controller: ctrl,
            minLines: 6,
            maxLines: 10,
            decoration: const InputDecoration(border: OutlineInputBorder(), hintText: 'First line of the song\nSecond line\nThird line'),
          ),
          const SizedBox(height: 12),
          Row(children: [
            Expanded(
              child: OutlinedButton.icon(
                onPressed: () async {
                  Navigator.pop(ctx);
                  await guard(context, () => app.saveLyrics(slot, ctrl.text), 'Lyrics saved');
                  app.previewSong(slot);
                },
                icon: const Icon(Icons.play_arrow),
                label: const Text('Save and play'),
              ),
            ),
            const SizedBox(width: 10),
            Expanded(
              child: FilledButton(
                onPressed: () async {
                  Navigator.pop(ctx);
                  await guard(context, () => app.saveLyrics(slot, ctrl.text), 'Lyrics saved');
                },
                child: const Text('Save'),
              ),
            ),
          ]),
        ]),
      ),
    );
  }

  @override
  Widget build(BuildContext context) {
    final playing = app.pv.songSlot == slot;
    final used = app.songTriggers(slot);
    final lines = app.lyrics[slot - 1].split('\n').where((l) => l.trim().isNotEmpty).length;
    final primary = Theme.of(context).colorScheme.primary;
    return Card(
      clipBehavior: Clip.antiAlias,
      shape: RoundedRectangleBorder(
        borderRadius: BorderRadius.circular(16),
        side: BorderSide(color: playing ? primary : Colors.transparent, width: 2),
      ),
      child: Column(children: [
        InkWell(
          onTap: () => playing ? app.stopPreview() : app.previewSong(slot),
          child: Padding(
            padding: const EdgeInsets.all(14),
            child: Row(children: [
              CircleAvatar(
                radius: 24,
                backgroundColor: playing ? primary : Theme.of(context).colorScheme.primaryContainer,
                child: Icon(playing ? Icons.stop : Icons.play_arrow, color: playing ? Colors.white : primary),
              ),
              const SizedBox(width: 14),
              Expanded(
                child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
                  Text('Song $slot', style: const TextStyle(fontSize: 17, fontWeight: FontWeight.w600)),
                  Text(used.isEmpty ? 'Not set to a touch yet' : 'Plays when: ${used.join(', ')}', style: Theme.of(context).textTheme.bodySmall),
                ]),
              ),
              if (playing) Text('Playing', style: TextStyle(color: primary, fontWeight: FontWeight.w600)),
            ]),
          ),
        ),
        const Divider(height: 1),
        InkWell(
          onTap: () => _editLyrics(context),
          child: Padding(
            padding: const EdgeInsets.symmetric(horizontal: 14, vertical: 10),
            child: Row(children: [
              Expanded(child: Text(lines == 0 ? 'Lyrics: not added yet' : 'Lyrics: $lines lines')),
              Text(lines == 0 ? 'Add lyrics' : 'Edit', style: TextStyle(color: primary, fontWeight: FontWeight.w600)),
            ]),
          ),
        ),
        const Divider(height: 1),
        Padding(
          padding: const EdgeInsets.fromLTRB(14, 8, 10, 8),
          child: Row(children: [
            const Expanded(child: Text("On Mochi's screen")),
            if (app.videoSending.containsKey(slot))
              Padding(
                padding: const EdgeInsets.symmetric(vertical: 10),
                child: Text('Sending video ${(app.videoSending[slot]! * 100).round()}%',
                    style: TextStyle(color: primary, fontWeight: FontWeight.w600)),
              )
            else
              SegmentedButton<bool>(
                showSelectedIcon: false,
                style: const ButtonStyle(visualDensity: VisualDensity.compact),
                segments: const [
                  ButtonSegment(value: false, label: Text('Stick man')),
                  ButtonSegment(value: true, label: Text('Video')),
                ],
                selected: {app.videoOnMochi(slot)},
                onSelectionChanged: app.videoSending.isNotEmpty
                    ? null
                    : (s) => guard(context, () => app.setVideoOnMochi(slot, s.first),
                        s.first ? 'Video saved on Mochi. Play Song $slot on Mochi to see it.' : 'Mochi shows the stick man for Song $slot'),
              ),
          ]),
        ),
        if (app.videoSending.containsKey(slot))
          LinearProgressIndicator(value: app.videoSending[slot]),
      ]),
    );
  }
}

// =====================================================================
//  SETTINGS: name, volume, switches, PIN, diagnostics
// =====================================================================
class SettingsPage extends StatefulWidget {
  const SettingsPage({super.key});
  @override
  State<SettingsPage> createState() => _SettingsPageState();
}

class _SettingsPageState extends State<SettingsPage> {
  late final TextEditingController name = TextEditingController(text: '${app.settings['name'] ?? 'Mochi'}');
  final TextEditingController newPin = TextEditingController();
  late double volume = ((app.settings['volume'] ?? app.settings['dayVol']) as num? ?? 30).toDouble().clamp(0, 30);
  late bool autoSleep = app.settings['autoSleep'] as bool? ?? true;
  late bool vibration = app.settings['vibration'] as bool? ?? true;
  late bool sound = app.settings['sound'] as bool? ?? true;
  late double lonelyMin = ((app.settings['lonelyMin'] as num?) ?? 3).toDouble().clamp(1, 60);
  late bool idleSounds = app.settings['idleSounds'] as bool? ?? true;
  late final bool newSettings = app.settings.containsKey('lonelyMin'); // older firmware does not have them

  Future<void> _save() async {
    final pinText = newPin.text.trim();
    await guard(context, () async {
      await app.api!.saveSettings({
        'name': name.text,
        'volume': '${volume.round()}',
        'dayVol': '${volume.round()}', // older firmware (day/night volumes)
        'nightVol': '${volume.round()}',
        'autoSleep': autoSleep ? '1' : '0',
        'vibration': vibration ? '1' : '0',
        'sound': sound ? '1' : '0',
        if (newSettings) 'lonelyMin': '${lonelyMin.round()}',
        if (newSettings) 'idleSounds': idleSounds ? '1' : '0',
        if (pinText.length >= 4) 'newPin': pinText,
      });
      if (pinText.length >= 4) {
        newPin.clear();
        await app.usePin(pinText);
        await app.refreshAll();
      } else {
        await app.refreshAll();
      }
    }, 'Saved to Mochi!');
  }

  @override
  Widget build(BuildContext context) => ListView(padding: const EdgeInsets.fromLTRB(16, 0, 16, 24), children: [
        Card(
          child: Padding(
            padding: const EdgeInsets.all(16),
            child: TextField(
              controller: name,
              maxLength: 12,
              decoration: const InputDecoration(
                labelText: 'Name',
                helperText: "Shown on Mochi's LCD, OLED (at start) and Bluetooth name",
                counterText: '',
              ),
            ),
          ),
        ),
        const SizedBox(height: 12),
        Card(
          child: Padding(
            padding: const EdgeInsets.fromLTRB(16, 12, 16, 4),
            child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
              Text('Volume: ${volume.round()} / 30'),
              Slider(value: volume, max: 30, divisions: 30, onChanged: (v) => setState(() => volume = v)),
              Text('Press Save: Mochi plays a sound at the new volume.', style: Theme.of(context).textTheme.bodySmall),
              const SizedBox(height: 8),
            ]),
          ),
        ),
        const SizedBox(height: 12),
        Card(
          child: Column(children: [
            SwitchListTile(title: const Text('Sleep when dark'), value: autoSleep, onChanged: (v) => setState(() => autoSleep = v)),
            SwitchListTile(title: const Text('Vibration'), value: vibration, onChanged: (v) => setState(() => vibration = v)),
            SwitchListTile(title: const Text('Sounds'), value: sound, onChanged: (v) => setState(() => sound = v)),
            if (newSettings)
              SwitchListTile(
                title: const Text('Idle sounds'),
                subtitle: const Text('A short soft sound now and then (at most every 3 minutes)'),
                value: idleSounds,
                onChanged: (v) => setState(() => idleSounds = v),
              ),
          ]),
        ),
        if (newSettings) ...[
          const SizedBox(height: 12),
          Card(
            child: Padding(
              padding: const EdgeInsets.fromLTRB(16, 12, 16, 4),
              child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
                Text('Feels lonely after ${lonelyMin.round()} min without petting'),
                Slider(value: lonelyMin, min: 1, max: 60, divisions: 59, onChanged: (v) => setState(() => lonelyMin = v)),
              ]),
            ),
          ),
        ],
        const SizedBox(height: 12),
        Card(
          child: Padding(
            padding: const EdgeInsets.all(16),
            child: TextField(controller: newPin, obscureText: true, decoration: const InputDecoration(labelText: 'New PIN (optional, 4+ characters)')),
          ),
        ),
        const SizedBox(height: 16),
        FilledButton(onPressed: _save, style: FilledButton.styleFrom(minimumSize: const Size.fromHeight(48)), child: const Text('Save')),
        const SizedBox(height: 16),
        Card(
          child: Column(children: [
            ListTile(
              leading: const Icon(Icons.monitor_heart_outlined),
              title: const Text('Diagnostics'),
              subtitle: const Text('Health, communication test, event log'),
              trailing: const Icon(Icons.chevron_right),
              onTap: () => Navigator.of(context).push(MaterialPageRoute<void>(builder: (_) => const DiagnosticsPage())),
            ),
            ListTile(
              leading: const Icon(Icons.bluetooth_disabled),
              title: const Text('Disconnect'),
              onTap: app.disconnect,
            ),
          ]),
        ),
      ]);
}
