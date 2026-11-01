import 'dart:async';
import 'package:flutter/foundation.dart' show kIsWeb;
import 'package:flutter/material.dart';
import 'package:flutter/scheduler.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import 'app_state.dart';
import 'ble_link.dart';
import 'face_painter.dart';
import 'pages.dart';

Future<void> main() async {
  WidgetsFlutterBinding.ensureInitialized();
  await app.init();
  runApp(const MochiApp());
}

class MochiApp extends StatelessWidget {
  const MochiApp({super.key});
  @override
  Widget build(BuildContext context) => MaterialApp(
        title: 'Mochi',
        debugShowCheckedModeBanner: false,
        theme: ThemeData(
          colorSchemeSeed: const Color(0xFF7C3AED),
          useMaterial3: true,
          scaffoldBackgroundColor: const Color(0xFFF7F5FB),
          cardTheme: const CardThemeData(color: Colors.white, elevation: 0.5, margin: EdgeInsets.zero),
        ),
        home: ListenableBuilder(
          listenable: app,
          builder: (context, _) => app.connected || app.reconnecting ? const Shell() : const ConnectPage(),
        ),
      );
}

// =====================================================================
//  CONNECT: find Mochi over Bluetooth
// =====================================================================
class ConnectPage extends StatefulWidget {
  const ConnectPage({super.key});
  @override
  State<ConnectPage> createState() => _ConnectPageState();
}

class _ConnectPageState extends State<ConnectPage> {
  late final TextEditingController pin = TextEditingController(text: app.pin);
  List<ScanResult> results = [];
  StreamSubscription<List<ScanResult>>? sub;
  bool scanning = false;
  bool busy = false;
  String? scanError;

  @override
  void initState() {
    super.initState();
    sub = FlutterBluePlus.scanResults.listen((r) {
      if (mounted) setState(() => results = r);
    });
    if (!kIsWeb) _scan(); // web browsers only allow scanning after a tap
  }

  @override
  void dispose() {
    sub?.cancel();
    FlutterBluePlus.stopScan();
    super.dispose();
  }

  Future<void> _scan() async {
    setState(() {
      scanning = true;
      scanError = null;
    });
    try {
      await FlutterBluePlus.startScan(withServices: [MochiBle.service], timeout: const Duration(seconds: 8));
      await FlutterBluePlus.isScanning.where((v) => v == false).first;
    } catch (e) {
      scanError = 'Could not search: $e\nTurn on Bluetooth and allow the "Nearby devices" permission.';
    }
    if (mounted) setState(() => scanning = false);
  }

  Future<void> _connect(ScanResult r) async {
    setState(() => busy = true);
    await FlutterBluePlus.stopScan();
    await app.connect(r.device, pin.text);
    if (mounted) setState(() => busy = false);
  }

  String _name(ScanResult r) {
    final n = r.advertisementData.advName.isNotEmpty ? r.advertisementData.advName : r.device.platformName;
    return n.isEmpty ? 'Mochi' : n;
  }

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    return Scaffold(
      body: SafeArea(
        child: Center(
          child: ConstrainedBox(
            constraints: const BoxConstraints(maxWidth: 420),
            child: ListView(padding: const EdgeInsets.all(24), shrinkWrap: true, children: [
              Center(
                child: Container(
                  width: 248,
                  padding: const EdgeInsets.all(10),
                  decoration: BoxDecoration(color: const Color(0xFF0F172A), borderRadius: BorderRadius.circular(24)),
                  child: const FaceThumb(face: 'happy', radius: 12),
                ),
              ),
              const SizedBox(height: 28),
              Text('Find your Mochi', textAlign: TextAlign.center, style: theme.textTheme.headlineSmall?.copyWith(fontWeight: FontWeight.bold)),
              const SizedBox(height: 6),
              const Text('Turn on Mochi and Bluetooth, then tap your Mochi below.', textAlign: TextAlign.center),
              const SizedBox(height: 20),
              TextField(
                controller: pin,
                obscureText: true,
                decoration: const InputDecoration(labelText: 'PIN (default 1234)', border: OutlineInputBorder()),
              ),
              const SizedBox(height: 16),
              if (scanning)
                const Card(
                  child: Padding(
                    padding: EdgeInsets.all(20),
                    child: Row(mainAxisAlignment: MainAxisAlignment.center, children: [
                      SizedBox(width: 20, height: 20, child: CircularProgressIndicator(strokeWidth: 2)),
                      SizedBox(width: 12),
                      Text('Looking for Mochi...'),
                    ]),
                  ),
                ),
              for (final r in results)
                Padding(
                  padding: const EdgeInsets.only(top: 8),
                  child: Card(
                    child: ListTile(
                      leading: const CircleAvatar(child: Icon(Icons.bluetooth)),
                      title: Text(_name(r), style: const TextStyle(fontWeight: FontWeight.w600)),
                      subtitle: Text('Signal ${r.rssi} dBm'),
                      trailing: busy
                          ? const SizedBox(width: 20, height: 20, child: CircularProgressIndicator(strokeWidth: 2))
                          : const Text('Connect', style: TextStyle(fontWeight: FontWeight.w600)),
                      onTap: busy ? null : () => _connect(r),
                    ),
                  ),
                ),
              if (!scanning && results.isEmpty && scanError == null)
                const Padding(padding: EdgeInsets.all(8), child: Text('No Mochi found yet. Is it on?', textAlign: TextAlign.center)),
              if (!scanning) TextButton(onPressed: busy ? null : _scan, child: const Text('Search again')),
              // The demo (pretend Mochi) is only for the website version, to check the screens.
              if (kIsWeb) ...[
                const Divider(height: 32),
                const Padding(
                  padding: EdgeInsets.only(bottom: 8),
                  child: Text('Website version: Bluetooth works in the phone app. Use the demo here to check every screen.',
                      textAlign: TextAlign.center),
                ),
                OutlinedButton.icon(
                  onPressed: busy ? null : () => app.connectDemo(),
                  icon: const Icon(Icons.visibility_outlined),
                  label: const Text('Try the demo (no Mochi needed)'),
                ),
              ],
              if (scanError != null) Text(scanError!, style: TextStyle(color: theme.colorScheme.error)),
              if (app.error != null) Text(app.error!, style: TextStyle(color: theme.colorScheme.error)),
            ]),
          ),
        ),
      ),
    );
  }
}

// =====================================================================
//  SHELL: header, Mochi's screen, tabs
// =====================================================================
class Shell extends StatefulWidget {
  const Shell({super.key});
  @override
  State<Shell> createState() => _ShellState();
}

class _ShellState extends State<Shell> {
  int tab = 0;
  static const pages = <Widget>[HomePage(), ReactionsPage(), SongsPage(), SettingsPage()];

  @override
  Widget build(BuildContext context) => Scaffold(
        body: SafeArea(
          bottom: false,
          child: Column(children: [
            ListenableBuilder(
              listenable: app,
              builder: (context, _) => Column(children: [
                if (app.reconnecting)
                  MaterialBanner(
                    leading: const SizedBox(width: 20, height: 20, child: CircularProgressIndicator(strokeWidth: 2)),
                    content: Text('Bluetooth lost. Reconnecting... (try ${app.reconnectAttempt} of 10)'),
                    actions: [TextButton(onPressed: app.disconnect, child: const Text('Stop'))],
                  ),
                Padding(
                  padding: const EdgeInsets.fromLTRB(16, 8, 16, 8),
                  child: Row(children: [
                    Text('${app.settings['name'] ?? 'Mochi'}', style: Theme.of(context).textTheme.titleLarge?.copyWith(fontWeight: FontWeight.bold)),
                    const Spacer(),
                    Container(
                      padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 4),
                      decoration: BoxDecoration(
                        color: app.connected ? const Color(0xFFECFDF5) : const Color(0xFFFFFBEB),
                        borderRadius: BorderRadius.circular(20),
                      ),
                      child: Row(children: [
                        Icon(Icons.circle, size: 8, color: app.connected ? Colors.green : Colors.orange),
                        const SizedBox(width: 6),
                        Text(app.demo ? 'Demo' : app.connected ? 'Connected' : 'Reconnecting',
                            style: TextStyle(fontSize: 13, color: app.connected ? Colors.green.shade800 : Colors.orange.shade900)),
                      ]),
                    ),
                  ]),
                ),
              ]),
            ),
            const Padding(padding: EdgeInsets.fromLTRB(16, 0, 16, 12), child: MochiScreen()),
            Expanded(child: pages[tab]),
            ListenableBuilder(
              listenable: app,
              builder: (context, _) => tab == 1 && app.unsavedCount > 0
                  ? Container(
                      color: Colors.white,
                      padding: const EdgeInsets.fromLTRB(16, 10, 16, 10),
                      child: Row(children: [
                        Expanded(child: Text('${app.unsavedCount} ${app.unsavedCount == 1 ? 'change' : 'changes'} not saved')),
                        FilledButton(
                          onPressed: () => guard(context, app.saveTriggers, 'Saved to Mochi! Try it on Mochi now.'),
                          child: const Text('Save to Mochi'),
                        ),
                      ]),
                    )
                  : const SizedBox.shrink(),
            ),
          ]),
        ),
        bottomNavigationBar: NavigationBar(
          selectedIndex: tab,
          onDestinationSelected: (i) => setState(() => tab = i),
          destinations: const [
            NavigationDestination(icon: Icon(Icons.home_outlined), selectedIcon: Icon(Icons.home), label: 'Home'),
            NavigationDestination(icon: Icon(Icons.auto_awesome_outlined), selectedIcon: Icon(Icons.auto_awesome), label: 'Reactions'),
            NavigationDestination(icon: Icon(Icons.music_note_outlined), selectedIcon: Icon(Icons.music_note), label: 'Songs'),
            NavigationDestination(icon: Icon(Icons.settings_outlined), selectedIcon: Icon(Icons.settings), label: 'Settings'),
          ],
        ),
      );
}

/// Mochi's screen at the top: face, song video, lyrics (preview on the phone only).
class MochiScreen extends StatefulWidget {
  const MochiScreen({super.key});
  @override
  State<MochiScreen> createState() => _MochiScreenState();
}

class _MochiScreenState extends State<MochiScreen> with SingleTickerProviderStateMixin {
  late final Ticker _ticker;

  @override
  void initState() {
    super.initState();
    _ticker = createTicker((_) {
      final pv = app.pv;
      if (pv.songSlot != null && pv.sw.elapsedMilliseconds > pv.untilMs) {
        app.stopPreview();
      } else if (pv.songSlot == null && pv.faceUntil != null && DateTime.now().isAfter(pv.faceUntil!)) {
        app.player.stop(); // the sound ends with the face (like Mochi)
        pv
          ..face = 'normal'
          ..label = ''
          ..faceUntil = null
          ..faceSince = DateTime.now();
      }
      if (mounted) setState(() {});
    })
      ..start();
  }

  @override
  void dispose() {
    _ticker.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final pv = app.pv;
    final slot = pv.songSlot;
    final el = pv.sw.elapsedMilliseconds;
    CustomPainter painter;
    final video = slot == null ? null : app.videos[slot];
    if (slot != null && video != null) {
      // Stretched to the song's length, so the video starts and ends with the sound (like on Mochi).
      final frame = el * video.frames ~/ app.songDurationMs(slot);
      painter = VideoPainter(video.bytes, frame < video.frames ? frame : video.frames - 1);
    } else if (slot != null) {
      painter = DancerPainter(el.toDouble()); // no video file: the stick man, like Mochi
    } else {
      painter = FacePainter(slot != null ? 'excited' : pv.face, DateTime.now().difference(pv.faceSince).inMilliseconds.toDouble());
    }
    String top = pv.label.isEmpty ? 'Pick something to see it here' : pv.label, bottom = '';
    if (slot != null) {
      final cur = pv.cues.where((c) => c.ms <= el).toList();
      if (cur.isNotEmpty) {
        top = cur.last.line1;
        bottom = cur.last.line2;
      } else {
        top = 'Playing Song $slot';
      }
    }
    return Container(
      padding: const EdgeInsets.all(10),
      decoration: BoxDecoration(color: const Color(0xFF0F172A), borderRadius: BorderRadius.circular(22)),
      child: Column(children: [
        AspectRatio(aspectRatio: 2, child: ClipRRect(borderRadius: BorderRadius.circular(12), child: CustomPaint(painter: painter))),
        SizedBox(
          height: 46,
          child: Row(children: [
            const SizedBox(width: 6),
            Expanded(
              child: Column(mainAxisAlignment: MainAxisAlignment.center, crossAxisAlignment: CrossAxisAlignment.start, children: [
                Text(top, maxLines: 1, overflow: TextOverflow.ellipsis,
                    style: TextStyle(color: Colors.white, fontWeight: slot != null ? FontWeight.w700 : FontWeight.w400)),
                if (bottom.isNotEmpty) Text(bottom, maxLines: 1, style: const TextStyle(color: Color(0xFFCBD5E1))),
              ]),
            ),
            if (slot != null || pv.label.isNotEmpty)
              TextButton(
                onPressed: app.stopPreview,
                style: TextButton.styleFrom(foregroundColor: Colors.white, backgroundColor: Colors.white12),
                child: const Text('Stop'),
              ),
          ]),
        ),
      ]),
    );
  }
}
