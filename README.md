# Mochi app (Flutter, Bluetooth)

Controls and configures Mochi over **Bluetooth (BLE)**. Needs the latest `mochi_virtual_pet.ino` on the ESP32
(WiFi is OFF in that firmware; Bluetooth is ON).

## What it does
- **Home**: love meter, Pet / Hug / Play / Sleep (sent to Mochi right away), live monitor of sensors (touch, light, motion) and outputs (OLED, LCD text, speaker, vibration).
- **Reactions**: tap a trigger (touch, shake, light...), pick a mood or a song. You see and hear it right away on the phone. Press **Save to Mochi** to store it on Mochi.
- **Songs**: one tap plays the video, lyrics and sound together (preview on the phone). Type the lyrics once (one line per row); they are timed by themselves and also shown on Mochi's LCD.
- **Settings**: name (shown on Mochi's LCD, OLED and Bluetooth name), volume, sleep, vibration, sounds, PIN, **Diagnostics** (health, communication test, event log).
- The phone preview plays the **same sound files as Mochi's SD card** (`assets/sounds/`, `assets/audio/`), and faces and videos last exactly as long as their sound.
- **Songs → On Mochi's screen**: *Stick man* (default) or *Video*. Choosing Video sends that song's video to Mochi (about 15-30 s, keep the phone close); choosing Stick man removes it.
- **Reactions → Mochi's day**: choose what happens when Mochi wakes up (normal: wake-up sound from folder 13 + greeting), falls asleep, is turned on, or a song ends.
- **Settings**: one volume (Mochi plays a sound at the new volume when you press Save), idle sounds on/off and "feels lonely after N minutes" (need the latest firmware).
- **Reactions → Vibration**: for each touch / movement / light change choose Normal (Mochi decides), Vibrate (strong buzz every time) or No vibration, then Save to Mochi.

## Checked
- `flutter analyze`: no issues (Flutter 3.47.9).
- `flutter build web`: builds, and the demo was clicked through (Home, Reactions + Save bar, Songs, Settings).
- Firmware: compiles for ESP32 (Arduino ESP32 core 3.3.12) and fits the default partition.

## Update your project (keep your android/ folder)
1. Replace `lib/` and `pubspec.yaml` with the ones in this zip.
2. Copy the whole `assets/` folder (videos, audio, sounds) into your project folder, and use the new `pubspec.yaml`.
3. Delete `test/widget_test.dart` if it shows red errors.
4. Run:
```bash
flutter clean
flutter pub get
flutter build apk --release       # build\app\outputs\flutter-apk\app-release.apk
```
Bluetooth does not work in the Windows/Chrome preview of the UI, so test it on the Android phone.

## Website version (UI check only)
The same code runs in a browser. Bluetooth to Mochi does not work there, so press
**Try the demo (no Mochi needed)** on the first screen: a pretend Mochi (`lib/demo_link.dart`)
answers like the firmware, so every screen, button, preview, song and the monitor can be checked.
```bash
flutter create .                 # once, adds the web/ folder
flutter run -d chrome            # or: flutter build web  (output in build/web)
```
The demo button is only in the website version. The Android app always connects to the real Mochi.

## Connect
1. Upload the firmware (Arduino IDE, board "ESP32 Dev Module", default partition is fine).
2. Turn on Mochi (LCD says "Bluetooth ready"). Turn on Bluetooth on the phone and allow the permission.
3. Open the app, type the PIN (default `1234`), tap **Mochi** in the list.
4. Done. Settings, reactions and vibration are saved on Mochi with **Save to Mochi**.

## Android: replace android/app/src/main/AndroidManifest.xml with
```xml
<manifest xmlns:android="http://schemas.android.com/apk/res/android">
    <uses-permission android:name="android.permission.BLUETOOTH" android:maxSdkVersion="30"/>
    <uses-permission android:name="android.permission.BLUETOOTH_ADMIN" android:maxSdkVersion="30"/>
    <uses-permission android:name="android.permission.BLUETOOTH_SCAN" android:usesPermissionFlags="neverForLocation"/>
    <uses-permission android:name="android.permission.BLUETOOTH_CONNECT"/>
    <uses-permission android:name="android.permission.ACCESS_FINE_LOCATION" android:maxSdkVersion="30"/>
    <uses-feature android:name="android.hardware.bluetooth_le" android:required="true"/>
    <application
        android:label="Mochi"
        android:name="${applicationName}"
        android:icon="@mipmap/ic_launcher">
        <activity
            android:name=".MainActivity"
            android:exported="true"
            android:launchMode="singleTop"
            android:taskAffinity=""
            android:theme="@style/LaunchTheme"
            android:configChanges="orientation|keyboardHidden|keyboard|screenSize|smallestScreenSize|locale|layoutDirection|fontScale|screenLayout|density|uiMode"
            android:hardwareAccelerated="true"
            android:windowSoftInputMode="adjustResize">
            <meta-data android:name="io.flutter.embedding.android.NormalTheme" android:resource="@style/NormalTheme"/>
            <intent-filter>
                <action android:name="android.intent.action.MAIN"/>
                <category android:name="android.intent.category.LAUNCHER"/>
            </intent-filter>
        </activity>
        <meta-data android:name="flutterEmbedding" android:value="2"/>
    </application>
    <queries>
        <intent>
            <action android:name="android.intent.action.PROCESS_TEXT"/>
            <data android:mimeType="text/plain"/>
        </intent>
    </queries>
</manifest>
```

## iOS: add to ios/Runner/Info.plist
```xml
<key>NSBluetoothAlwaysUsageDescription</key><string>Talk to Mochi over Bluetooth</string>
```

## If the Windows build fails
- `jni` / `global_jni_env.c.o`: the pubspec pins `path_provider_android` and `shared_preferences_android` to older versions. Run `flutter clean` then `flutter pub get`.
- `compileSdk 36` / `checkReleaseAarMetadata`: add the `subprojects { ... bumpCompileSdk ... }` block at the end of `android/build.gradle.kts`.

## Songs on the OLED
Mochi shows its dancing stick man while a song plays, unless you chose **Video** for that song in the app.
The song videos (`assets/videos/`) play in the app's preview and are sent to Mochi only when you choose Video.

## Files
- `lib/demo_link.dart` : pretend Mochi for the website / demo
- `lib/ble_link.dart` : the Bluetooth connection (one request line at a time, big answers in small packets)
- `lib/mochi_api.dart` : every Mochi call (`/api/status`, `/settings`, `/triggers`, `/sounds`, `/lyrics`, `/video`, `/action`)
- `lib/app_state.dart` : state, auto-reconnect, lyrics timing, phone previews
- `lib/tones.dart` : sample mood sounds for the phone preview
- `lib/sensor_monitor.dart` : live monitor on Home
- `lib/diagnostics_page.dart` : health, communication test, event log
- `lib/face_painter.dart` : Mochi's 16 faces, the song video painter and the stick man
- `lib/main.dart`, `lib/pages.dart` : screens
