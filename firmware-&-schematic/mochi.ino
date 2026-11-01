#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <math.h>
// 0 = Bluetooth only (fits the default "Default 4MB with spiffs" partition).
// 1 = also build the old WiFi web page + OTA (then choose Tools > Partition Scheme > Huge APP).
#define MOCHI_WIFI 0
#if MOCHI_WIFI
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <Update.h>
#endif
#include <Preferences.h>
#include <LittleFS.h>
#include <esp_system.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <esp_gap_ble_api.h>

// =================================================================
//  PINS
// =================================================================
constexpr uint8_t PIN_SDA = 21;
constexpr uint8_t PIN_SCL = 22;
constexpr uint8_t PIN_TOUCH = 27;
constexpr uint8_t PIN_LDR = 34;
constexpr uint8_t PIN_DF_RX = 25;    // ESP32 RX  <- DFPlayer TX
constexpr uint8_t PIN_DF_TX = 26;    // ESP32 TX  -> (1k resistor optional) -> DFPlayer RX
constexpr uint8_t PIN_DF_BUSY = 33;  // DFPlayer BUSY (LOW = playing)
constexpr uint8_t PIN_VIBRATION = 18;

// =================================================================
//  USER SETTINGS
// =================================================================
constexpr uint8_t OLED_ADDRESS = 0x3C;  // try 0x3D if the OLED stays blank
// I2C speed for the whole bus. 100000 is the most stable; use 400000 for
// smoother animation. If the OLED picture gets corrupted, use 100000.
constexpr uint32_t I2C_CLOCK_HZ = 100000;

// LDR: most LM393 modules give a HIGHER AO reading in darkness.
// If day/night is reversed on your module, set this to false.
constexpr bool LDR_HIGH_WHEN_DARK = true;
constexpr uint8_t NIGHT_BELOW_PERCENT = 20;   // darker than this = night
constexpr uint8_t DAY_ABOVE_PERCENT = 35;     // brighter than this = day
constexpr uint8_t SUDDEN_DARK_DROP = 35;      // % drop in ~0.7 s = "who turned off the light?"

// MPU6050 mounting: Z axis pointing up when Mochi stands normally.
// If eyes move the wrong way when tilting, flip TILT_INVERT.
// If left/right tilt does nothing, set TILT_USE_X_AXIS to true.
constexpr bool TILT_USE_X_AXIS = false;
constexpr bool TILT_INVERT = false;
constexpr float SHAKE_JERK = 6.0f;            // m/s^2 change between samples
constexpr float MOVE_GYRO = 2.5f;             // rad/s for "movement" reaction

// Timing
constexpr uint32_t LONG_TOUCH_MS = 1200;
constexpr uint32_t TAP_GAP_MS = 350;          // max gap between taps in a multi-tap
constexpr uint32_t TOUCH_DEBOUNCE_MS = 40;    // a touch must stay steady this long to count
// The vibration motor makes electrical noise that the TTP223 can read as extra taps
// (one touch counted as 3 taps = surprise laugh). Touch is ignored while the motor
// runs and for this long after it switches on or off.
constexpr uint32_t TOUCH_QUIET_AFTER_VIB_MS = 150;
constexpr uint32_t NIGHT_SLEEPY_MS = 15000;   // dark + idle -> sleepy eyes
constexpr uint32_t DIALOGUE_ROTATE_MS = 8000;

// DFPlayer Mini
// Set to false to test without WiFi (isolates freezes / power problems).
// Bluetooth (BLE) is how the phone app talks to Mochi. WiFi is OFF by default.
// Set ENABLE_WIFI = true to bring back the old WiFi page + OTA (needs the "Huge APP" partition).
constexpr bool ENABLE_BLE = true;
constexpr bool ENABLE_WIFI = MOCHI_WIFI;
constexpr bool USE_BUSY_PIN = true;           // set false if BUSY is not wired
// Songs show the dancing stick man on the OLED, unless you chose "Video" for that song in the
// app (Songs -> On Mochi's screen): then the video saved on Mochi plays. false = always stick man.
constexpr bool PLAY_SONG_VIDEOS = true;

// Defaults for settings the app can change (saved in flash, survive power-off)
constexpr uint8_t DEFAULT_VOLUME = 30;        // 0..30 (30 = loudest), one volume for day and night
constexpr uint16_t DEFAULT_SLEEP_AFTER_S = 45;
const char* DEFAULT_PIN = "1234";             // app + OTA password

// WiFi setup hotspot (used until Mochi knows your home WiFi)
const char* SETUP_AP_NAME = "Mochi-Setup";
const char* SETUP_AP_PASS = "mochi1234";      // at least 8 characters
const char* FW_VERSION = "2.0";

constexpr uint8_t SONG_SLOTS = 3;            // songs the app can configure (title, lyrics, video)

// -------- What Mochi does for each touch / movement / light change --------
// Chosen in the phone page or Flutter app, saved in Mochi's memory.
enum Trig : uint8_t {
  TG_TAP1, TG_TAP2, TG_TAP3, TG_LONG,          // touch: 1 tap, 2 taps, 3+ taps, hold
  TG_SHAKE, TG_UPSIDE, TG_MOVE,                // motion
  TG_DARK, TG_NIGHT, TG_BRIGHT,                // light: suddenly off, night starts, light is back
  TG_LONELY,                                   // nobody touched Mochi for a long time
  TG_WAKE, TG_SLEEP, TG_START, TG_SONGEND,     // wakes up, falls asleep, turned on, a song ended
  TG_COUNT
};
enum ActType : uint8_t { ACT_DEFAULT, ACT_MOOD, ACT_SOUND, ACT_SONG, ACT_NONE };
// Vibration for each trigger (chosen in the app): Mochi decides / always vibrate / never vibrate.
enum TrigVib : uint8_t { VIB_NORMAL, VIB_ON, VIB_OFF };
struct TrigAction {
  uint8_t type;    // ActType
  uint8_t mood;    // face (Expr) when type = ACT_MOOD
  uint8_t folder;  // sound folder 1..13 when type = ACT_SOUND
  uint8_t file;    // sound file 1..4
  uint8_t slot;    // song 1..3 when type = ACT_SONG
};

// -------- Saved configuration --------
struct Config {
  char name[13];
  uint8_t volume;             // DFPlayer volume 0..30 (set in the app)
  uint16_t sleepAfterS;
  bool autoSleep;
  bool vibration;
  bool sound;
  char pin[17];
  TrigAction trig[TG_COUNT];
  uint8_t trigVib[TG_COUNT];  // TrigVib for each trigger
  uint32_t songMs[SONG_SLOTS]; // song length, measured by Mochi the first time each song plays to the end
  uint8_t lonelyMin;          // minutes without petting before Mochi feels lonely
  bool idleSounds;            // random "la la la" / "hmm?" sounds while idle
  uint8_t soundFace[14][4];   // [folder 1..13][file 1..4] -> Expr (face shown with that sound)
  char songTitle[SONG_SLOTS][17];
  char wifiSsid[33];
  char wifiPass[65];
};
Config cfg;
Preferences prefs;

// =================================================================
//  SOUND FILES ON THE microSD CARD  (folders 01..15 in the card root)
// =================================================================
// true : /FF/NNN_name.mp3     (FF = folder number, NNN = file number)
// false: /mp3/FFNN_name.mp3   (flat fallback, FF = folder, NN = file)
constexpr bool DF_USE_FOLDERS = true;

// LCD custom characters (defined in setup): music note and heart.
#define LCD_NOTE "\x01"
#define LCD_HEART "\x02"

enum Folder : uint8_t {
  F_HAPPY = 1, F_SAD = 2, F_ANGRY = 3, F_SLEEPY = 4, F_EXCITED = 5, F_SURPRISED = 6,
  F_LOVE = 7, F_GREETING = 8, F_TAP = 9, F_HUG = 10, F_SHAKE = 11, F_LONELY = 12,
  F_WAKE = 13, F_SONGS = 14, F_SYSTEM = 15, F_LAST = 15
};

// How many files each folder has. A random one is played. Edit when you add files.
//                                       0  1  2  3  4  5  6  7  8  9 10 11 12 13 14 15
constexpr uint8_t FILES_IN_FOLDER[F_LAST + 1] = {0, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 3, 2};

// Length (ms) of every sound on the microSD card (measured from the DASAI SOUNDS files).
// Used so the face lasts exactly as long as its sound. If you replace a file with a
// different length, update its number here (0 = unknown: the BUSY pin is used instead).
//                                         file 001  002    003
const uint16_t SOUND_MS[F_LAST + 1][3] = {
  {    0,     0,     0},  // 00
  { 5878, 10632,     0},  // 01 happy
  { 9012,  1881,     0},  // 02 sad
  {14550,  4911,     0},  // 03 angry
  { 3030, 22674,     0},  // 04 sleepy
  {10080, 12600,     0},  // 05 excited
  { 7752,  4944,     0},  // 06 surprised
  { 9288,  6661,     0},  // 07 love
  { 7776,  1071,     0},  // 08 greetings
  {10266,  5878,     0},  // 09 tap
  { 2299,  1411,     0},  // 10 hug
  { 2743,  1620,     0},  // 11 shake
  { 1672,  4754,     0},  // 12 lonely
  { 2456,  7445,     0},  // 13 wake up
  {11416, 13453, 13767},  // 14 songs
  { 2821,  1698,     0}   // 15 system
};


// What happened (used all over the code) -> which folder to play from.
enum Track : uint16_t {
  TRK_NONE = 0, TRK_HELLO, TRK_HEHE, TRK_LOVE, TRK_YAY, TRK_EEK, TRK_DIZZY, TRK_HEY,
  TRK_HUH, TRK_MISS, TRK_SLEEPY, TRK_GOODNIGHT, TRK_MORNING, TRK_STARTUP, TRK_LALA,
  TRK_HAPPY, TRK_SAD, TRK_LOVEFX, TRK_COUNT
};

struct SoundRef {
  uint8_t folder;
  uint8_t file;  // 0 = pick a random file in the folder
};

const SoundRef SOUND_FOR[TRK_COUNT] = {
  {0, 0},               // NONE
  {F_GREETING, 0},      // HELLO     - greeting after waking up
  {F_TAP, 0},           // HEHE      - one tap
  {F_HUG, 0},           // LOVE      - long touch
  {F_EXCITED, 0},       // YAY       - many taps, fun movement
  {F_SURPRISED, 0},     // EEK       - sudden darkness, surprise
  {F_SHAKE, 0},         // DIZZY     - shake
  {F_ANGRY, 0},         // HEY       - upside down (angry)
  {F_SHAKE, 0},         // HUH       - confused
  {F_LONELY, 0},        // MISS      - nobody petted Mochi for a long time
  {F_SLEEPY, 0},        // SLEEPY    - night begins
  {F_SLEEPY, 0},        // GOODNIGHT - going to sleep
  {F_WAKE, 0},          // MORNING   - waking up
  {F_SYSTEM, 1},        // STARTUP   - /15/001
  {F_HAPPY, 0},         // LALA      - idle humming
  {F_HAPPY, 0},         // HAPPY     - folder 01
  {F_SAD, 0},           // SAD       - folder 02 (feeling sad)
  {F_LOVE, 0},          // LOVEFX    - folder 07 (shy / sweet)
};

// =================================================================
//  SONG PLAYLIST  (2 taps = play Song 1, the default song)
// =================================================================
// Lyric cue: shown on the LCD atMs milliseconds after the song starts.
// Max 16 characters per line.
//
// HOW TO TIME YOUR LYRICS: start the song (2 taps), then tap the touch
// sensor ONCE at the start of each line. Serial Monitor prints
// "Lyric mark: 12345 ms" - copy those numbers into atMs below.
struct LyricCue {
  uint32_t atMs;
  const char* line1;
  const char* line2;
};

struct Song {
  const char* title;
  uint8_t file;          // plays /14/<file>_name.mp3  (1..3)
  uint32_t durationMs;   // 0 = detect the end with the BUSY pin
  const LyricCue* lyrics;
  uint8_t lyricCount;
};

// ---------- Song 1  (/14/001_*.mp3) ----------
const LyricCue SONG1_LYRICS[] = {
  {3000,  "Song 1 line 1", "goes here..."},
  {7000,  "Song 1 line 2", "goes here..."},
  {11000, "Song 1 line 3", "goes here..."},
  {15000, "Song 1 line 4", "goes here..."},
};

// ---------- Song 2  (/14/002_*.mp3) ----------
const LyricCue SONG2_LYRICS[] = {
  {3000,  "Song 2 line 1", "goes here..."},
  {7000,  "Song 2 line 2", "goes here..."},
  {11000, "Song 2 line 3", "goes here..."},
};

// ---------- Song 3  (/14/003_*.mp3) ----------
const LyricCue SONG3_LYRICS[] = {
  {3000,  "Song 3 line 1", "goes here..."},
  {7000,  "Song 3 line 2", "goes here..."},
  {11000, "Song 3 line 3", "goes here..."},
};

#define SONG(title, file, durationMs, lyrics) \
  {title, file, durationMs, lyrics, (uint8_t)(sizeof(lyrics) / sizeof(lyrics[0]))}

// The songs are the files in folder 14. Titles and lyrics can be changed from the
// Mochi page, so you normally never edit this part.
const Song PLAYLIST[] = {
  SONG("Song 1", 1, 0, SONG1_LYRICS),
  SONG("Song 2", 2, 0, SONG2_LYRICS),
  SONG("Song 3", 3, 0, SONG3_LYRICS),
};
constexpr uint8_t PLAYLIST_COUNT = sizeof(PLAYLIST) / sizeof(PLAYLIST[0]);
static_assert(PLAYLIST_COUNT == SONG_SLOTS, "PLAYLIST and SONG_SLOTS must match");

// =================================================================
//  MINIMAL I2C LCD DRIVER (PCF8574 backpack)
//  P0=RS P1=RW P2=EN P3=Backlight P4..P7=D4..D7
// =================================================================
class MiniLCD {
 public:
  bool ok = false;

  bool begin() {
    const uint8_t addresses[] = {0x27, 0x3F};
    for (uint8_t a : addresses) {
      Wire.beginTransmission(a);
      if (Wire.endTransmission() == 0) {
        address_ = a;
        ok = true;
        break;
      }
    }
    if (!ok) return false;
    delay(50);
    write4(0x03, 0); delay(5);
    write4(0x03, 0); delayMicroseconds(150);
    write4(0x03, 0);
    write4(0x02, 0);
    send(0x28, 0);  // 4-bit, 2 lines
    send(0x0C, 0);  // display on, cursor off
    send(0x06, 0);  // entry mode
    clear();
    return true;
  }

  void clear() {
    if (!ok) return;
    send(0x01, 0);
    delay(2);
  }

  void backlight(bool on) {
    if (!ok) return;
    backlight_ = on ? 0x08 : 0x00;
    expander(0);
  }

  void createChar(uint8_t location, const uint8_t bitmap[8]) {
    if (!ok) return;
    send(0x40 | ((location & 0x07) << 3), 0);
    for (uint8_t i = 0; i < 8; ++i) send(bitmap[i], 1);
  }

  char shown[2][17] = {{0}, {0}};  // what the LCD shows now (for the app's monitor)

  // Always writes 16 characters (pads with spaces) so there is no flicker.
  void printRow(uint8_t row, const char* text) {
    if (!ok) return;
    strncpy(shown[row ? 1 : 0], text, 16);
    shown[row ? 1 : 0][16] = 0;
    send(0x80 | (row ? 0x40 : 0x00), 0);
    bool ended = false;
    for (uint8_t i = 0; i < 16; ++i) {
      if (!ended && text[i] == '\0') ended = true;
      send(ended ? ' ' : text[i], 1);
    }
  }

  uint8_t address() const { return address_; }

 private:
  uint8_t address_ = 0x27;
  uint8_t backlight_ = 0x08;

  void expander(uint8_t v) {
    Wire.beginTransmission(address_);
    Wire.write(v | backlight_);
    Wire.endTransmission();
  }
  void pulse(uint8_t v) {
    expander(v | 0x04);
    delayMicroseconds(2);
    expander(v & 0xFB);
    delayMicroseconds(50);
  }
  void write4(uint8_t nibble, uint8_t rs) {
    const uint8_t v = (nibble << 4) | rs;
    expander(v);
    pulse(v);
  }
  void send(uint8_t value, uint8_t rs) {
    write4(value >> 4, rs);
    write4(value & 0x0F, rs);
  }
};

const uint8_t GLYPH_NOTE[8] = {0b00010, 0b00011, 0b00010, 0b00010, 0b01110, 0b11110, 0b01100, 0b00000};
const uint8_t GLYPH_HEART[8] = {0b00000, 0b01010, 0b11111, 0b11111, 0b11111, 0b01110, 0b00100, 0b00000};

// =================================================================
//  DEVICES + STATE
// =================================================================
Adafruit_SH1106G oled(128, 64, &Wire, -1);
Adafruit_MPU6050 mpu;
MiniLCD lcd;
bool oledOk = false;
bool mpuOk = false;

enum Mode : uint8_t { MODE_NORMAL, MODE_MUSIC, MODE_SLEEP };
enum Expr : uint8_t {
  EX_NORMAL, EX_HAPPY, EX_SAD, EX_ANGRY, EX_SLEEPY, EX_EXCITED,
  EX_SURPRISED, EX_DIZZY, EX_LOVE, EX_CONFUSED, EX_WINK, EX_LAUGH, EX_CRY, EX_SHY,
  EX_SLEEP, EX_WAKING
};
const char* EXPR_NAMES[] = {"normal", "happy", "sad", "angry", "sleepy", "excited",
                            "surprised", "dizzy", "love", "confused", "wink", "laugh", "cry", "shy",
                            "sleep", "waking"};

// =================================================================
//  FACE FOR EACH SOUND  (folders 01..13, up to 4 files per folder)
// =================================================================
// Every sound can have its own face. The defaults below can be changed from
// the phone page / Flutter app ("Face for each sound") and are saved in
// Mochi's memory, so after saving, Mochi shows them on its own.
constexpr uint8_t SOUND_FACE_FOLDERS = 13;
constexpr uint8_t SOUND_FACE_FILES = 4;
static_assert(sizeof(Config::soundFace) == (SOUND_FACE_FOLDERS + 1) * SOUND_FACE_FILES, "soundFace size");

// File names for the web page (match your SD card). Files 3 and 4 show as "sound".
const char* const SOUND_NAMES[SOUND_FACE_FOLDERS + 1][2] = {
  {"", ""},
  {"sparkle", "cheer"},        // 01 happy
  {"sigh", "drop"},            // 02 sad
  {"grumble", "growl"},        // 03 angry
  {"yawn", "good-night"},      // 04 sleepy
  {"zoom", "yay"},             // 05 excited
  {"gasp", "squeak"},          // 06 surprised
  {"heart-glow", "warm-chime"},// 07 love
  {"hello", "hi-there"},       // 08 greetings
  {"hehe", "that-tickles"},    // 09 tap
  {"i-love-you", "hug-me"},    // 10 hug
  {"whoaa", "dizzy"},          // 11 shake
  {"i-miss-you", "pet-me"},    // 12 lonely
  {"good-morning", "im-awake"} // 13 wake up
};

#define FACES(a, b) {a, b, a, b}
const uint8_t DEFAULT_SOUND_FACE[SOUND_FACE_FOLDERS + 1][SOUND_FACE_FILES] = {
  FACES(EX_NORMAL, EX_NORMAL),
  FACES(EX_HAPPY, EX_EXCITED),      // 01 sparkle, cheer
  FACES(EX_SAD, EX_CRY),            // 02 sigh, drop
  FACES(EX_ANGRY, EX_ANGRY),        // 03 grumble, growl
  FACES(EX_SLEEPY, EX_SLEEPY),      // 04 yawn, good-night
  FACES(EX_EXCITED, EX_LAUGH),      // 05 zoom, yay
  FACES(EX_SURPRISED, EX_SURPRISED),// 06 gasp, squeak
  FACES(EX_LOVE, EX_SHY),           // 07 heart-glow, warm-chime
  FACES(EX_HAPPY, EX_WINK),         // 08 hello, hi-there
  FACES(EX_WINK, EX_LAUGH),         // 09 hehe, that-tickles
  FACES(EX_LOVE, EX_SHY),           // 10 i-love-you, hug-me
  FACES(EX_CONFUSED, EX_DIZZY),     // 11 whoaa, dizzy
  FACES(EX_SAD, EX_CRY),            // 12 i-miss-you, pet-me
  FACES(EX_HAPPY, EX_EXCITED)       // 13 good-morning, im-awake
};

const char* const TRIG_NAMES[TG_COUNT] = {"tap1", "tap2", "tap3", "long", "shake", "upside",
                                           "move", "dark", "night", "bright", "lonely",
                                           "wake", "sleep", "start", "songend"};
const char* const ACT_NAMES[] = {"default", "mood", "sound", "song", "none"};

Mode mode = MODE_NORMAL;

// Reaction (temporary expression) state
Expr reactionExpr = EX_NORMAL;
uint32_t reactionStart = 0;
uint32_t reactionUntil = 0;
bool greetAfterWake = false;
bool greetIsStartup = false;
uint32_t wakeSoundUntil = 0;  // the wake-up sound ends then (the greeting face stays until it ends)  // the greeting after power-on (trigger "start") instead of waking up ("wake")

// Dialogue override message
char msg1[17] = "";
char msg2[17] = "";
uint32_t msgUntil = 0;

// Activity + affection
int affection = 60;                 // 0..100
uint32_t lastActivity = 0;          // touch or motion
uint32_t lastPet = 0;               // touch only
uint32_t lastAffectionDecay = 0;
uint32_t lastLonely = 0;
uint32_t nextIdleEvent = 0;
uint32_t lastIdleSound = 0;
constexpr uint32_t IDLE_SOUND_GAP_MS = 180000;  // at most one idle sound every 3 minutes

// Light
uint16_t ldrRaw = 0;
float lightSmooth = 50.0f;
uint8_t lightPercent = 50;
bool isNight = false;
uint32_t nightCandidateSince = 0;
uint32_t dayCandidateSince = 0;
uint32_t suddenDarkCooldown = 0;

// Motion
float fax = 0, fay = 0, faz = 9.8f;  // filtered acceleration
float accelMag = 9.8f;
float tiltX = 0;                     // -1 .. +1
bool upsideDown = false;
float gyroNow = 0;                   // rotation speed (rad/s), for the app's sensor monitor

// Eyes
float lookX = 0, lookY = 0;
float targetX = 0, targetY = 0;
uint32_t nextLookChange = 0;
uint32_t blinkStart = 0, nextBlink = 2000;

// Sleep
uint32_t sleepStarted = 0;
bool lcdLightOn = true;
bool oledDimmed = false;

// Touch
bool touchDown = false;
bool fsOk = false;            // LittleFS storage for lyrics + videos
bool lcdForceRedraw = false;  // set after writing to the LCD directly

// Note: keep functions below the type definitions above. The Arduino IDE
// adds function prototypes before the first function in the file.
void defaultSoundFaces() {
  for (uint8_t f = 0; f <= SOUND_FACE_FOLDERS; ++f)
    for (uint8_t n = 0; n < SOUND_FACE_FILES; ++n) cfg.soundFace[f][n] = DEFAULT_SOUND_FACE[f][n];
}

void defaultTriggers() {
  for (uint8_t i = 0; i < TG_COUNT; ++i) cfg.trig[i] = TrigAction{ACT_DEFAULT, EX_HAPPY, F_TAP, 1, 1};
  for (uint8_t i = 0; i < TG_COUNT; ++i) cfg.trigVib[i] = VIB_NORMAL;
}

bool validTrigger(const TrigAction& a) {
  return a.type <= ACT_NONE && a.mood < EX_SLEEP && a.folder >= 1 && a.folder <= SOUND_FACE_FOLDERS &&
         a.file >= 1 && a.file <= SOUND_FACE_FILES && a.slot >= 1 && a.slot <= SONG_SLOTS;
}

void loadConfig() {
  prefs.begin("mochi", false);
  prefs.getString("name", "Mochi").toCharArray(cfg.name, sizeof(cfg.name));
  // One volume ("vol"). Boards updated from the day/night version start loud (30) once.
  cfg.volume = constrain(prefs.getUChar("vol", DEFAULT_VOLUME), 0, 30);
  cfg.sleepAfterS = prefs.getUShort("sleepS", DEFAULT_SLEEP_AFTER_S);
  cfg.autoSleep = prefs.getBool("autoSleep", true);
  cfg.vibration = prefs.getBool("vibration", true);
  cfg.sound = prefs.getBool("sound", true);
  prefs.getString("pin", DEFAULT_PIN).toCharArray(cfg.pin, sizeof(cfg.pin));
  for (uint8_t i = 0; i < SONG_SLOTS; ++i) {
    char key[8];
    snprintf(key, sizeof(key), "title%u", i + 1);
    prefs.getString(key, "").toCharArray(cfg.songTitle[i], sizeof(cfg.songTitle[i]));
  }
  prefs.getString("ssid", "").toCharArray(cfg.wifiSsid, sizeof(cfg.wifiSsid));
  prefs.getString("pass", "").toCharArray(cfg.wifiPass, sizeof(cfg.wifiPass));
  defaultTriggers();
  // Older firmware saved fewer triggers: load what is there, new ones keep their defaults.
  TrigAction savedTrig[TG_COUNT];
  const size_t trigLen = prefs.getBytesLength("trig");
  if (trigLen && trigLen % sizeof(TrigAction) == 0 && trigLen <= sizeof(savedTrig) &&
      prefs.getBytes("trig", savedTrig, trigLen) == trigLen) {
    for (uint8_t i = 0; i < trigLen / sizeof(TrigAction); ++i)
      if (validTrigger(savedTrig[i])) cfg.trig[i] = savedTrig[i];
  }
  uint8_t savedVib[TG_COUNT];
  const size_t vibLen = prefs.getBytesLength("tvib");
  if (vibLen && vibLen <= sizeof(savedVib) && prefs.getBytes("tvib", savedVib, vibLen) == vibLen) {
    for (uint8_t i = 0; i < vibLen; ++i)
      if (savedVib[i] <= VIB_OFF) cfg.trigVib[i] = savedVib[i];
  }
  cfg.lonelyMin = constrain(prefs.getUChar("lonelyM", 3), 1, 60);
  for (uint8_t i = 0; i < SONG_SLOTS; ++i) {
    char key[8];
    snprintf(key, sizeof(key), "slen%u", i + 1);
    cfg.songMs[i] = prefs.getUInt(key, 0);  // 0 = not measured yet (SOUND_MS is used)
  }
  cfg.idleSounds = prefs.getBool("idleSnd", true);
  defaultSoundFaces();
  uint8_t saved[sizeof(cfg.soundFace)];
  if (prefs.getBytes("sfaces", saved, sizeof(saved)) == sizeof(saved)) {
    for (uint8_t i = 0; i < sizeof(saved); ++i) {
      if (saved[i] <= EX_WAKING) ((uint8_t*)cfg.soundFace)[i] = saved[i];
    }
  }
  prefs.end();
}

void saveConfig() {
  prefs.begin("mochi", false);
  prefs.putString("name", cfg.name);
  prefs.putUChar("vol", cfg.volume);
  prefs.putUShort("sleepS", cfg.sleepAfterS);
  prefs.putBool("autoSleep", cfg.autoSleep);
  prefs.putBool("vibration", cfg.vibration);
  prefs.putBool("sound", cfg.sound);
  prefs.putString("pin", cfg.pin);
  for (uint8_t i = 0; i < SONG_SLOTS; ++i) {
    char key[8];
    snprintf(key, sizeof(key), "title%u", i + 1);
    prefs.putString(key, cfg.songTitle[i]);
  }
  prefs.putString("ssid", cfg.wifiSsid);
  prefs.putString("pass", cfg.wifiPass);
  prefs.putBytes("sfaces", cfg.soundFace, sizeof(cfg.soundFace));
  prefs.putBytes("trig", cfg.trig, sizeof(cfg.trig));
  prefs.putBytes("tvib", cfg.trigVib, sizeof(cfg.trigVib));
  prefs.putUChar("lonelyM", cfg.lonelyMin);
  prefs.putBool("idleSnd", cfg.idleSounds);
  prefs.end();
}

bool pending(uint32_t until) { return (int32_t)(until - millis()) > 0; }
bool reactionActive() { return pending(reactionUntil); }

// =================================================================
//  EVENT LOG + HEALTH STATS  (evidence for testing; read from the app: Diagnostics)
// =================================================================
// /log.csv lines: boot,uptime_ms,unix_time,type,detail   (unix_time = 0 until the app syncs the clock)
// The file rotates to /log.old.csv at 24 KB, so the flash never fills up.
uint32_t bootCount = 0;
uint32_t timeOffset = 0;     // unix seconds at millis() = 0, sent by the app
const char* resetReasonText = "unknown";
bool lcdOk = false;
bool ldrAtLimit = false;
uint32_t statReqOk = 0, statReqErr = 0, statAuthFail = 0, statBleConnects = 0, statBleDrops = 0;
uint32_t statLoopMaxUs = 0, statLoopMaxShownUs = 0;
float statLoopAvgUs = 0;
// Event -> output response time (measured on the device).
uint32_t evtAt = 0;
bool evtWaitOled = false, evtWaitAudio = false;
uint16_t respOledMs = 0, respAudioMs = 0, respOledMaxMs = 0, respAudioMaxMs = 0;
uint32_t respCount = 0;

void logEvent(const char* type, const String& detail) {
  char line[160];
  const uint32_t ms = millis();
  const uint32_t unixTime = timeOffset ? timeOffset + ms / 1000 : 0;
  String d = detail;
  d.replace(",", ";");
  d.replace("\n", " ");
  snprintf(line, sizeof(line), "%lu,%lu,%lu,%s,%s\n", (unsigned long)bootCount, (unsigned long)ms,
           (unsigned long)unixTime, type, d.c_str());
  Serial.print("LOG ");
  Serial.print(line);
  if (!fsOk) return;
  File f = LittleFS.open("/log.csv", "a");
  if (!f) return;
  if (f.size() > 24576) {
    f.close();
    LittleFS.remove("/log.old.csv");
    LittleFS.rename("/log.csv", "/log.old.csv");
    f = LittleFS.open("/log.csv", "a");
    if (!f) return;
  }
  f.print(line);
  f.close();
}

const char* resetReasonName(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON: return "power-on";
    case ESP_RST_SW: return "software restart";
    case ESP_RST_PANIC: return "crash (panic)";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_BROWNOUT: return "brownout (low power)";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    case ESP_RST_EXT: return "reset button";
    default: return "other";
  }
}

void markEvent() {  // a trigger happened: measure how long until the OLED / sound react
  evtAt = millis();
  evtWaitOled = evtWaitAudio = true;
}

void noteResponse(bool oledDone) {
  const uint32_t dt = millis() - evtAt;
  if (dt > 5000) {  // nothing for this event (for example sound is off)
    if (oledDone) evtWaitOled = false; else evtWaitAudio = false;
    return;
  }
  if (oledDone) {
    respOledMs = dt;
    if (dt > respOledMaxMs) respOledMaxMs = dt;
    evtWaitOled = false;
    ++respCount;
  } else {
    respAudioMs = dt;
    if (dt > respAudioMaxMs) respAudioMaxMs = dt;
    evtWaitAudio = false;
  }
}

void copyText(char* dst, const char* src) {
  snprintf(dst, 17, "%s", src ? src : "");
}

// =================================================================
//  AUDIO  (DFPlayer Mini over UART2)
// =================================================================
// DFPlayer needs ~100 ms between commands, so requests are queued and sent
// from updateAudio() without blocking the animations.
HardwareSerial& dfSerial = Serial1;   // UART1 on the DFPlayer pins (same as the working test)
bool dfOk = false;                     // becomes true once the DFPlayer has answered
uint8_t pendingFolder = 0;   // what to play next (0 = nothing)
// Output monitor (sent to the app): last sound played, volume, vibration pulses.
uint8_t monFolder = 0, monFile = 0, monVolume = 0;
uint32_t monPlayAt = 0, monVibCount = 0;
uint8_t pendingFile = 0;
bool pendingStop = false;
int8_t pendingVolume = -1;
uint8_t volSendsLeft = 0;         // the volume command is sent twice (a lost one would keep the old volume)
uint32_t lastDfCommand = 0;
bool dfTrackFinished = false;
uint32_t songAudioAt = 0;         // when the song's audio really starts (video + lyrics follow it)
constexpr uint16_t SONG_START_MS = 120;  // DFPlayer delay between "play" and the first sound
// DFPlayer recovery: a short power dip (motor or speaker peak) can reset the DFPlayer or
// make it stop answering, and then every sound is silent. Mochi notices and sets it up again.
uint32_t dfAwaitReplySince = 0;   // a command was sent and no reply came yet
uint8_t dfMissedReplies = 0;
bool dfAckSeen = false;           // this DFPlayer sends "OK" replies (some clones never do)
bool dfNeedInit = false;
uint32_t dfLastInit = 0;
uint32_t dfHoldUntil = 0;         // wait after re-initializing before the next command
uint32_t dfLastInitLog = 0;

// Sends one 10-byte DFPlayer command frame (with "please acknowledge" set).
void dfSend(uint8_t cmd, uint16_t param) {
  uint8_t f[10] = {0x7E, 0xFF, 0x06, cmd, 0x01,
                   (uint8_t)(param >> 8), (uint8_t)(param & 0xFF), 0, 0, 0xEF};
  uint16_t sum = 0;
  for (int i = 1; i < 7; i++) sum += f[i];
  sum = 0 - sum;
  f[7] = sum >> 8;
  f[8] = sum & 0xFF;
  dfSerial.write(f, 10);
}

// Reads replies from the DFPlayer: marks it as alive, notices "track finished" and errors.
void dfPoll() {
  static uint8_t buf[10];
  static uint8_t n = 0;
  while (dfSerial.available()) {
    const uint8_t b = dfSerial.read();
    if (n == 0 && b != 0x7E) continue;
    buf[n++] = b;
    if (n == 10) {
      n = 0;
      if (buf[1] == 0xFF && buf[9] == 0xEF) {
        dfOk = true;
        dfAwaitReplySince = 0;
        dfMissedReplies = 0;
        if (buf[3] == 0x41) dfAckSeen = true;
        if (buf[3] == 0x3D) dfTrackFinished = true;
        else if (buf[3] == 0x3F || buf[3] == 0x3A) {
          // 0x3F = DFPlayer just (re)started, 0x3A = SD card back: select the card + volume again.
          dfNeedInit = true;
        } else if (buf[3] == 0x40) {
          Serial.printf("DFPlayer error %u (1 = busy/no SD card, 6 = file not found)\n", buf[6]);
          if (buf[6] >= 1 && buf[6] <= 4) dfNeedInit = true;  // busy, sleeping, serial or checksum error
        }
      }
    }
  }
}

// Picks a file in a folder: a fixed one, or a random variation (never the same twice in a row).
uint8_t pickFile(uint8_t folder, uint8_t fixed) {
  if (fixed) return fixed;
  const uint8_t n = folder <= F_LAST ? FILES_IN_FOLDER[folder] : 0;
  if (n <= 1) return 1;
  static uint8_t last[F_LAST + 1] = {0};
  uint8_t f;
  do {
    f = (uint8_t)random(1, n + 1);
  } while (f == last[folder]);
  last[folder] = f;
  return f;
}

bool queueSound(uint16_t track) {
  if (track == TRK_NONE || track >= TRK_COUNT) return false;
  const SoundRef r = SOUND_FOR[track];
  pendingFolder = r.folder;
  pendingFile = pickFile(r.folder, r.file);
  return true;
}

// The face configured for one sound (folders 01..13); `fallback` for other folders.
Expr faceForSound(uint8_t folder, uint8_t file, Expr fallback) {
  if (folder < 1 || folder > SOUND_FACE_FOLDERS || file < 1 || file > SOUND_FACE_FILES) return fallback;
  const uint8_t e = cfg.soundFace[folder][file - 1];
  return e <= EX_WAKING ? (Expr)e : fallback;
}

// Reaction sounds and voice lines (muted while a song is playing).
bool playTrack(uint16_t track) {
  if (mode == MODE_MUSIC || !cfg.sound) return false;
  return queueSound(track);
}

void playSongTrack(uint8_t file) {
  pendingStop = false;
  pendingFolder = F_SONGS;
  pendingFile = file;
}

void stopAudio() {
  pendingFolder = 0;
  pendingStop = true;
}

void setVolume(uint8_t volume) {
  pendingVolume = volume;
  volSendsLeft = 2;
}

void updateAudio() {
  dfPoll();
  const uint32_t now = millis();
  // No reply to the last command: after 2 misses in a row, set the DFPlayer up again.
  if (dfAckSeen && dfAwaitReplySince && now - dfAwaitReplySince > 600) {
    dfAwaitReplySince = 0;
    if (++dfMissedReplies >= 2) {
      dfMissedReplies = 0;
      dfNeedInit = true;
    }
  }
  if (pending(dfHoldUntil) || now - lastDfCommand < 120) return;
  if (dfNeedInit && now - dfLastInit > 3000) {
    dfNeedInit = false;
    dfLastInit = now;
    dfSend(0x09, 2);                       // select the microSD card
    setVolume(cfg.volume);                 // volume is sent next
    dfHoldUntil = now + 400;
    if (!dfLastInitLog || now - dfLastInitLog > 60000) {  // at most once a minute in the log
      dfLastInitLog = now;
      logEvent("RECOVER", "DFPlayer set up again (SD card + volume)");
    }
  } else if (pendingStop) {
    dfSend(0x16, 0);                       // stop
    pendingStop = false;
  } else if (pendingVolume >= 0) {
    dfSend(0x06, pendingVolume);           // volume 0..30
    monVolume = pendingVolume;
    if (volSendsLeft > 1) {
      --volSendsLeft;
    } else {
      volSendsLeft = 0;
      pendingVolume = -1;
    }
  } else if (pendingFolder) {
    if (DF_USE_FOLDERS) {
      dfSend(0x0F, ((uint16_t)pendingFolder << 8) | pendingFile);   // play /FF/NNN_*.mp3
      Serial.printf("DFPlayer: play /%02u/%03u\n", (unsigned)pendingFolder, (unsigned)pendingFile);
    } else {
      const uint16_t flat = pendingFolder * 100 + pendingFile;
      dfSend(0x12, flat);                                           // play /mp3/FFNN_*.mp3
      Serial.printf("DFPlayer: play /mp3/%04u\n", (unsigned)flat);
    }
    dfTrackFinished = false;
    if (pendingFolder == F_SONGS) songAudioAt = millis() + SONG_START_MS;
    monFolder = pendingFolder;
    monFile = pendingFile;
    monPlayAt = millis();
    pendingFolder = 0;
    if (evtWaitAudio) noteResponse(false);
  } else {
    return;
  }
  lastDfCommand = now;
  dfAwaitReplySince = now;  // every command asks for a reply (see dfSend)
}

// =================================================================
//  VIBRATION  (non-blocking pulse patterns)
// =================================================================
uint8_t vibPulsesLeft = 0;
uint32_t vibHoldUntil = 0;  // vibrate() calls are ignored until then (per-trigger vibration choice)
uint16_t vibOnMs = 0, vibOffMs = 0;
bool vibIsOn = false;
uint32_t vibNextChange = 0;
uint32_t vibLastEdge = 0;   // last time the motor switched on or off (touch ignores noise around it)

void vibrate(uint8_t pulses, uint16_t onMs, uint16_t offMs = 80) {
  if (!pulses || !cfg.vibration || pending(vibHoldUntil)) return;
  vibPulsesLeft = pulses;
  vibOnMs = onMs;
  vibOffMs = offMs;
  vibIsOn = true;
  monVibCount += pulses;
  digitalWrite(PIN_VIBRATION, HIGH);
  vibLastEdge = millis();
  vibNextChange = millis() + onMs;
}

void updateVibration() {
  if (!vibPulsesLeft || pending(vibNextChange)) return;
  vibLastEdge = millis();
  if (vibIsOn) {
    digitalWrite(PIN_VIBRATION, LOW);
    vibIsOn = false;
    if (--vibPulsesLeft == 0) return;
    vibNextChange = millis() + vibOffMs;
  } else {
    digitalWrite(PIN_VIBRATION, HIGH);
    vibIsOn = true;
    vibNextChange = millis() + vibOnMs;
  }
}

// =================================================================
//  REACTIONS + DIALOGUE
// =================================================================
void say(const char* line1, const char* line2, uint32_t ms) {
  copyText(msg1, line1);
  copyText(msg2, line2);
  msgUntil = millis() + ms;
}

// Which sound belongs to each face (used when a mood is picked from the web page / Flutter app).
//   happy->01  sad/cry->02  angry->03  sleepy->04  excited/laugh->05  surprised->06
//   love->10  shy->07  wink->09  dizzy->11  confused->11  (normal / sleep / waking stay silent)
uint16_t soundForExpr(Expr e) {
  switch (e) {
    case EX_HAPPY:     return TRK_HAPPY;
    case EX_SAD:       return TRK_SAD;
    case EX_CRY:       return TRK_SAD;
    case EX_ANGRY:     return TRK_HEY;
    case EX_SLEEPY:    return TRK_SLEEPY;
    case EX_EXCITED:   return TRK_YAY;
    case EX_LAUGH:     return TRK_YAY;
    case EX_SURPRISED: return TRK_EEK;
    case EX_LOVE:      return TRK_LOVE;
    case EX_SHY:       return TRK_LOVEFX;
    case EX_WINK:      return TRK_HEHE;
    case EX_DIZZY:     return TRK_DIZZY;
    case EX_CONFUSED:  return TRK_HUH;
    default:           return TRK_NONE;
  }
}

// Length of one SD card sound (0 = unknown).
uint16_t soundLengthMs(uint8_t folder, uint8_t file) {
  return (folder <= F_LAST && file >= 1 && file <= 3) ? SOUND_MS[folder][file - 1] : 0;
}

// ---- The face lasts exactly as long as its sound ----
// The length of every SD sound is known (SOUND_MS), so the face ends together with the sound.
// The BUSY pin (if wired) corrects it when a sound ends early or never starts. Sounds longer
// than REACTION_MAX_MS are stopped together with the face, so face and sound never drift apart.
bool faceFollowsSound = false;
bool faceSoundSeen = false;
bool faceCutSound = false;     // stop the sound when the face ends (it is longer than REACTION_MAX_MS)
bool busyEverLow = false;      // the BUSY pin works (it went LOW at least once)
uint32_t faceSoundStart = 0;
uint16_t faceFallbackMs = 0;
uint16_t faceKnownMs = 0;
constexpr uint16_t SOUND_START_MS = 250;     // time from "play" until the DFPlayer is heard
constexpr uint16_t FACE_MIN_MS = 900;        // even very short sounds show the face this long
constexpr uint16_t REACTION_MAX_MS = 8000;   // longest reaction (face + sound)
constexpr uint16_t FACE_AFTER_SOUND_MS = 200;

void startSoundFace(uint16_t fallbackMs) {
  const uint32_t now = millis();
  faceSoundStart = now;
  faceSoundSeen = false;
  faceFallbackMs = fallbackMs;
  faceKnownMs = soundLengthMs(pendingFolder, pendingFile);
  faceCutSound = false;
  uint32_t len;
  if (faceKnownMs) {
    len = SOUND_START_MS + faceKnownMs + FACE_AFTER_SOUND_MS;
  } else if (USE_BUSY_PIN) {
    len = REACTION_MAX_MS;  // unknown length: BUSY decides, this is only the limit
  } else {
    faceFollowsSound = false;
    return;                 // nothing to follow: keep the normal time
  }
  if (len > REACTION_MAX_MS) {
    len = REACTION_MAX_MS;
    faceCutSound = true;
  }
  if (len < FACE_MIN_MS) len = FACE_MIN_MS;
  reactionUntil = now + len;
  faceFollowsSound = true;
}

void updateSoundFace() {
  if (!faceFollowsSound) return;
  if (mode != MODE_NORMAL) {
    faceFollowsSound = false;
    return;
  }
  if (!reactionActive()) {                // the face is over: end the sound with it
    if (faceCutSound && !pendingFolder) stopAudio();
    faceFollowsSound = false;
    return;
  }
  if (!USE_BUSY_PIN) return;
  const uint32_t now = millis();
  const uint32_t age = now - faceSoundStart;
  const bool playing = digitalRead(PIN_DF_BUSY) == LOW;
  if (playing) busyEverLow = true;
  if (playing && age > 150) {             // (150 ms: BUSY may still show the previous sound)
    faceSoundSeen = true;
    return;
  }
  if (faceSoundSeen) {                    // the sound ended (earlier than its table length?)
    uint32_t end = now + FACE_AFTER_SOUND_MS;
    if (end < faceSoundStart + FACE_MIN_MS) end = faceSoundStart + FACE_MIN_MS;
    if ((int32_t)(end - reactionUntil) < 0) reactionUntil = end;
    faceCutSound = false;
    faceFollowsSound = false;
  } else if (age > 1500 && (busyEverLow || !faceKnownMs)) {
    // BUSY works but this sound never started (missing file, no SD card): normal time.
    reactionUntil = faceSoundStart + faceFallbackMs;
    faceCutSound = false;
    faceFollowsSound = false;
  }
}

// Temporary expression + message + voice/sound + vibration (Reaction Mode).
// When it ends, Mochi automatically returns to its idle face (function 24).
void react(Expr expr, uint16_t ms, const char* line1, const char* line2,
           uint16_t track = TRK_NONE,
           uint8_t pulses = 0, uint16_t vibOn = 0, uint16_t vibOff = 80) {
  if (mode != MODE_NORMAL) return;
  const uint32_t now = millis();
  reactionExpr = expr;
  reactionStart = now;
  reactionUntil = now + ms;
  if (line1) say(line1, line2, ms < 2500 ? 2500 : ms);
  // The face always stays the one asked for (it no longer switches to the random sound's face),
  // and it lasts as long as the sound.
  faceFollowsSound = false;
  if (playTrack(track)) startSoundFace(ms);
  vibrate(pulses, vibOn, vibOff);
}

void addAffection(int amount) {
  affection = constrain(affection + amount, 0, 100);
}

Expr baseExpression() {
  const uint32_t now = millis();
  if (mode == MODE_SLEEP) return EX_SLEEP;
  if (upsideDown) return EX_CONFUSED;
  if (isNight && now - lastActivity > NIGHT_SLEEPY_MS) return EX_SLEEPY;
  if (affection < 25) return EX_SAD;
  return EX_NORMAL;
}

Expr currentExpression() {
  if (reactionActive()) return reactionExpr;
  return baseExpression();
}

// =================================================================
//  MUSIC MODE  (functions 3, 4, 5, 6)
// =================================================================
enum MusicPhase : uint8_t { MP_INTRO, MP_PLAYING, MP_DONE };
constexpr uint16_t MUSIC_BEAT_MS = 400;  // dance animation speed
MusicPhase musicPhase = MP_INTRO;
uint32_t musicStart = 0;
uint32_t musicPhaseStart = 0;
uint32_t musicIdleSince = 0;
int musicCue = -1;
uint16_t musicBeat = 0;
constexpr int8_t DEFAULT_SONG = 0;  // Song 1: played by 2 taps and "music" unless another song is chosen
int8_t songIndex = -1;

const Song& song() { return PLAYLIST[songIndex < 0 ? 0 : songIndex]; }

// Time since the song's audio started (before that: since the song was started).
uint32_t songElapsed() {
  const uint32_t now = millis();
  const uint32_t base = songAudioAt ? songAudioAt : musicStart;
  return (int32_t)(now - base) > 0 ? now - base : 0;
}

// Length of a song: measured on Mochi, else the length of the SD card file.
uint32_t songLength(int8_t index) {
  if (index < 0 || index >= SONG_SLOTS) return 0;
  return cfg.songMs[index] ? cfg.songMs[index] : soundLengthMs(F_SONGS, PLAYLIST[index].file);
}

// The song played to its end: remember its real length (the video is stretched to it).
void learnSongLength(uint32_t ms) {
  if (songIndex < 0 || ms < 3000 || ms > 600000) return;
  const uint32_t old = songLength(songIndex);
  if (old && (ms > old ? ms - old : old - ms) < 250) return;
  cfg.songMs[songIndex] = ms;
  char key[8];
  snprintf(key, sizeof(key), "slen%u", songIndex + 1);
  prefs.begin("mochi", false);
  prefs.putUInt(key, ms);
  prefs.end();
  logEvent("SONG", "song " + String(songIndex + 1) + " length " + String(ms) + " ms");
}

bool openVideo(uint8_t index);
void closeVideo();
void beginTrigger(Trig t);  // defined below (CHOSEN ACTIONS)
bool runTrigger(Trig t);

const char* songTitle(int8_t index) {
  if (index < 0) index = 0;
  return index < SONG_SLOTS && cfg.songTitle[index][0] ? cfg.songTitle[index] : PLAYLIST[index].title;
}

// Lyrics can be replaced from the app: /lyrics<N>.txt, one "ms|line1|line2" per line.
struct CueText {
  uint32_t atMs;
  char line1[17];
  char line2[17];
};
constexpr uint8_t MAX_CUES = 48;
CueText cues[MAX_CUES];
uint8_t cueCount = 0;

void loadCues(uint8_t index) {
  cueCount = 0;
  char path[16];
  snprintf(path, sizeof(path), "/lyrics%u.txt", index + 1);
  if (fsOk && LittleFS.exists(path)) {
    File f = LittleFS.open(path, "r");
    while (f && f.available() && cueCount < MAX_CUES) {
      String line = f.readStringUntil('\n');
      line.trim();
      const int a = line.indexOf('|');
      if (a <= 0) continue;
      const int b = line.indexOf('|', a + 1);
      CueText& c = cues[cueCount++];
      c.atMs = line.substring(0, a).toInt();
      copyText(c.line1, line.substring(a + 1, b < 0 ? line.length() : b).c_str());
      copyText(c.line2, b < 0 ? "" : line.substring(b + 1).c_str());
    }
    if (f) f.close();
    if (cueCount) return;
  }
  const Song& s = PLAYLIST[index];
  for (uint8_t i = 0; i < s.lyricCount && i < MAX_CUES; ++i) {
    cues[i].atMs = s.lyrics[i].atMs;
    copyText(cues[i].line1, s.lyrics[i].line1);
    copyText(cues[i].line2, s.lyrics[i].line2);
    cueCount = i + 1;
  }
}

void startMusic(int8_t forceIndex = -1) {
  reactionUntil = 0;
  msgUntil = 0;
  songIndex = (forceIndex >= 0 && forceIndex < PLAYLIST_COUNT) ? forceIndex : DEFAULT_SONG;
  loadCues(songIndex);
  const bool hasVideo = openVideo(songIndex);
  mode = MODE_MUSIC;
  musicPhase = MP_INTRO;
  musicStart = musicPhaseStart = millis();
  musicIdleSince = 0;
  musicCue = -1;
  musicBeat = 0;
  songAudioAt = 0;
  playSongTrack(song().file);
  vibrate(1, 80);
  logEvent("SONG", "start " + String(songIndex + 1) + (hasVideo ? " video" : " no-video") + " cues=" + String(cueCount));
  Serial.printf("Music: start \"%s\" (/%02u/%03u)\n", songTitle(songIndex), (unsigned)F_SONGS, (unsigned)song().file);
}

void finishMusic(bool stoppedByUser) {
  closeVideo();
  logEvent("SONG", stoppedByUser ? "stopped by user" : "finished");
  mode = MODE_NORMAL;
  lastActivity = millis();
  if (stoppedByUser) {
    stopAudio();
    react(EX_NORMAL, 1200, "Music stopped", "", TRK_NONE, 1, 60);
  } else {
    addAffection(3);
    if (!runTrigger(TG_SONGEND))  // chosen in the app ("songend"), e.g. another song or a mood
      react(EX_HAPPY, 2500, "Did you like it?", "Hehe " LCD_HEART LCD_HEART, TRK_HEHE, 1, 80);
  }
  Serial.println("Music: end");
}

bool songEnded(uint32_t elapsed, uint32_t now) {
  if (elapsed < 2000) return false;  // give the DFPlayer time to start
  const Song& s = song();
  if (s.durationMs && elapsed >= s.durationMs) return true;
  if (dfTrackFinished) return true;
  if (USE_BUSY_PIN) {
    if (digitalRead(PIN_DF_BUSY) == HIGH) {  // HIGH = not playing
      if (!musicIdleSince) musicIdleSince = now;
      return now - musicIdleSince > 500;
    }
    musicIdleSince = 0;
    return false;
  }
  // No BUSY pin: end when the song's known length is over (or a few seconds after the last lyric).
  const uint32_t len = songLength(songIndex);
  if (len) return songElapsed() > len + 300;
  const uint32_t lastCue = cueCount ? cues[cueCount - 1].atMs : 0;
  return elapsed > lastCue + 6000;
}

void updateMusic() {
  if (mode != MODE_MUSIC) return;
  const uint32_t now = millis();
  const uint32_t elapsed = now - musicStart;

  if (musicPhase == MP_DONE) {
    if (now - musicPhaseStart >= 3000) finishMusic(false);
    return;
  }

  musicBeat = elapsed / MUSIC_BEAT_MS;
  if (musicPhase == MP_INTRO && elapsed >= 1500) musicPhase = MP_PLAYING;

  // Advance lyrics by time (function 4), counted from when the audio started.
  const uint32_t heard = songElapsed();
  while (musicCue + 1 < (int)cueCount && cues[musicCue + 1].atMs <= heard) {
    ++musicCue;
    vibrate(1, 35);
  }

  if (songEnded(elapsed, now)) {
    if (songAudioAt && (USE_BUSY_PIN || dfTrackFinished)) {
      const uint32_t endAt = musicIdleSince ? musicIdleSince : now;
      learnSongLength(endAt - songAudioAt);
    }
    musicPhase = MP_DONE;
    musicPhaseStart = now;
    vibrate(2, 90, 90);
  }
}

// =================================================================
//  SLEEP / WAKE  (functions 11, 12)
// =================================================================
void setScreensAwake(bool awake) {
  if (oledOk) oled.setContrast(awake ? 0xFF : 0x08);
  oledDimmed = !awake;
  lcd.backlight(true);
  lcdLightOn = true;
}

void enterSleep() {
  reactionUntil = 0;
  beginTrigger(TG_SLEEP);
  mode = MODE_SLEEP;
  sleepStarted = millis();
  // Sound when falling asleep, chosen in the app ("sleep"). The face is always the sleeping face,
  // and a song is not used here (it would wake Mochi up again).
  const TrigAction& a = cfg.trig[TG_SLEEP];
  if (cfg.sound) {
    if (a.type == ACT_MOOD) {
      queueSound(soundForExpr((Expr)a.mood));
    } else if (a.type == ACT_SOUND && a.folder <= F_LAST && a.file >= 1 && a.file <= FILES_IN_FOLDER[a.folder]) {
      pendingStop = false;
      pendingFolder = a.folder;
      pendingFile = a.file;
    } else if (a.type != ACT_NONE) {
      queueSound(TRK_GOODNIGHT);  // playTrack() is for NORMAL mode, so queue directly
    }
  }
  say("Yaaawn...", "Good night " LCD_HEART, 5000);
  Serial.println("Mode: SLEEP");
}

void updateSleepScreens() {
  if (mode != MODE_SLEEP) return;
  const uint32_t t = millis() - sleepStarted;
  if (!oledDimmed && t > 3000) {
    if (oledOk) oled.setContrast(0x08);
    oledDimmed = true;
  }
  if (lcdLightOn && t > 10000) {
    lcd.backlight(false);
    lcdLightOn = false;
  }
}

void wakeUp(bool byTouch) {
  mode = MODE_NORMAL;
  setScreensAwake(true);
  lastActivity = millis();
  if (byTouch) lastPet = lastActivity;
  greetAfterWake = true;
  greetIsStartup = false;
  react(EX_WAKING, 1100, "*yawn*", "Huh...?", TRK_NONE, 1, 120);
  // Normal wake-up: the wake-up sound (folder 13) plays right away, with the yawn.
  // Something else chosen in the app ("wake") runs when the yawn ends (see updateBehavior).
  if (cfg.trig[TG_WAKE].type == ACT_DEFAULT && playTrack(TRK_MORNING)) {
    uint32_t len = SOUND_START_MS + soundLengthMs(pendingFolder, pendingFile);
    if (len > REACTION_MAX_MS) len = REACTION_MAX_MS;
    wakeSoundUntil = millis() + len;
  }
  Serial.println("Mode: NORMAL (woke up)");
}

// =================================================================
//  CHOSEN ACTIONS  (what the app set for each touch / movement / light change)
// =================================================================
// Returns true when the app chose a custom action (the built-in reaction is skipped).
// Shared start of every trigger: response timing, the vibration chosen in the app, event log.
void beginTrigger(Trig t) {
  const TrigAction& a = cfg.trig[t];
  markEvent();
  // Vibration chosen in the app for this trigger. The hold blocks the reaction's own
  // vibration that follows in this same event, so "Vibrate" keeps its strong buzz and
  // "No vibration" stays silent.
  vibHoldUntil = 0;
  if (cfg.trigVib[t] == VIB_ON) {
    vibrate(2, 160, 90);
    vibHoldUntil = millis() + 60;
  } else if (cfg.trigVib[t] == VIB_OFF) {
    vibHoldUntil = millis() + 60;
  }
  {
    String what = String(TRIG_NAMES[t]) + " -> " + ACT_NAMES[a.type];
    if (a.type == ACT_MOOD) what += String(":") + EXPR_NAMES[a.mood];
    if (a.type == ACT_SONG) what += ":" + String(a.slot);
    if (a.type == ACT_SOUND) what += ":" + String(a.folder) + "/" + String(a.file);
    if (cfg.trigVib[t] == VIB_ON) what += " +vibrate";
    if (cfg.trigVib[t] == VIB_OFF) what += " no-vibration";
    logEvent("EVENT", what);
  }
}

bool runTrigger(Trig t) {
  const TrigAction& a = cfg.trig[t];
  beginTrigger(t);
  switch (a.type) {
    case ACT_MOOD: {
      const Expr face = a.mood < EX_SLEEP ? (Expr)a.mood : EX_HAPPY;
      react(face, 3000, nullptr, nullptr, soundForExpr(face), 1, 60);
      reactionExpr = face;  // keep the face that was chosen
      return true;
    }
    case ACT_SOUND:
      if (a.folder < 1 || a.folder > F_LAST || a.file < 1 || a.file > FILES_IN_FOLDER[a.folder]) return false;
      react(faceForSound(a.folder, a.file, EX_HAPPY), 3000, nullptr, nullptr, TRK_NONE, 1, 60);
      if (mode == MODE_NORMAL && cfg.sound) {
        pendingStop = false;
        pendingFolder = a.folder;
        pendingFile = a.file;
        startSoundFace(3000);
      }
      return true;
    case ACT_SONG:
      if (a.slot < 1 || a.slot > SONG_SLOTS) return false;
      if (mode == MODE_NORMAL) startMusic(a.slot - 1);  // audio + lyrics + video together
      return true;
    case ACT_NONE:
      return true;
    default:
      return false;
  }
}

// =================================================================
//  TOUCH  (functions 7, 8, 9, 12, 19)
// =================================================================
void onSingleTap() {
  if (mode == MODE_MUSIC) {
    // Lyric timing helper: tap at the start of each line while the song plays.
    Serial.printf("Lyric mark: %lu ms\n", (unsigned long)songElapsed());
    vibrate(1, 30);
    return;
  }
  if (runTrigger(TG_TAP1)) return;
  addAffection(4);
  const uint8_t roll = random(0, 10);
  if (affection >= 80 && roll < 3) {
    react(EX_SHY, 2000, "Hehe... shy~", "You're sweet", TRK_LOVEFX, 1, 60);
    return;
  }
  if (roll < 4) {
    react(EX_WINK, 1600, "Wink wink~", "Hehe!", TRK_HEHE, 1, 60);
    return;
  }
  const char* lines[][2] = {
    {"Hehe~ " LCD_HEART, "That tickles!"},
    {"Yay! Pat pat!", "More please~"},
    {"Hi hi! " LCD_HEART, "I'm happy!"},
  };
  const uint8_t i = random(0, 3);
  react(EX_HAPPY, 1800, lines[i][0], lines[i][1], TRK_HEHE, 1, 60);
}

void onLongTouch() {
  if (mode == MODE_MUSIC) return;
  if (runTrigger(TG_LONG)) return;
  addAffection(10);
  react(EX_LOVE, 3200, "I love you! " LCD_HEART, "Hug me more~", TRK_LOVE, 2, 120, 140);
}

void onDoubleTap() {
  if (mode == MODE_MUSIC) {
    finishMusic(true);
    return;
  }
  if (runTrigger(TG_TAP2)) return;
  startMusic();
}

void onMultiTap() {
  if (mode == MODE_MUSIC) return;
  if (runTrigger(TG_TAP3)) return;
  addAffection(6);
  react(EX_EXCITED, 2600, "Yay! Yay! Yay!", "So much fun!", TRK_YAY, 3, 70, 70);
}

void updateTouch() {
  static bool stable = false, lastRaw = false;
  static uint32_t rawChanged = 0, pressAt = 0, lastTapAt = 0;
  static bool longFired = false, consumed = false;
  static uint8_t taps = 0;

  const uint32_t now = millis();
  const bool raw = digitalRead(PIN_TOUCH) == HIGH;
  if (raw != lastRaw) {
    lastRaw = raw;
    rawChanged = now;
  }
  // Ignore motor noise: restart the debounce timer while the motor is (just) switching.
  const bool quiet = vibIsOn || now - vibLastEdge < TOUCH_QUIET_AFTER_VIB_MS;
  if (quiet) rawChanged = now;

  if (!quiet && raw != stable && now - rawChanged >= TOUCH_DEBOUNCE_MS) {
    stable = raw;
    touchDown = stable;
    if (stable) {  // pressed
      pressAt = now;
      longFired = false;
      lastActivity = now;
      if (mode == MODE_SLEEP) {
        wakeUp(true);
        consumed = true;
      }
    } else {  // released
      if (consumed) consumed = false;
      else if (!longFired) {
        ++taps;
        lastTapAt = now;
      }
    }
  }

  if (stable && !consumed && !longFired && now - pressAt >= LONG_TOUCH_MS) {
    longFired = true;
    taps = 0;
    lastPet = now;
    onLongTouch();
  }

  if (taps && !stable && now - lastTapAt > TAP_GAP_MS) {
    const uint8_t n = taps;
    taps = 0;
    lastPet = now;
    if (n == 1) onSingleTap();
    else if (n == 2) onDoubleTap();
    else if (mode == MODE_MUSIC) finishMusic(true);  // 3+ taps also stop a song
    else onMultiTap();
  }
}

// =================================================================
//  LIGHT  (functions 10, 11, 13)
// =================================================================
void updateLight() {
  static uint32_t lastRead = 0, lastHistory = 0;
  static uint8_t history[8] = {50, 50, 50, 50, 50, 50, 50, 50};
  static uint8_t histHead = 0;
  const uint32_t now = millis();
  if (now - lastRead < 50) return;
  lastRead = now;

  uint32_t sum = 0;
  for (uint8_t i = 0; i < 8; ++i) sum += analogRead(PIN_LDR);
  ldrRaw = sum / 8;
  const float pct = LDR_HIGH_WHEN_DARK ? (4095 - ldrRaw) * 100.0f / 4095.0f
                                       : ldrRaw * 100.0f / 4095.0f;
  lightSmooth += (pct - lightSmooth) * 0.3f;
  lightPercent = (uint8_t)constrain((int)lightSmooth, 0, 100);

  // Sudden darkness: compare with the reading from ~0.7 s ago.
  if (now - lastHistory >= 100) {
    lastHistory = now;
    const uint8_t old = history[histHead];
    history[histHead] = lightPercent;
    histHead = (histHead + 1) % 8;
    if (mode == MODE_NORMAL && !pending(suddenDarkCooldown) &&
        old >= lightPercent + SUDDEN_DARK_DROP && lightPercent < NIGHT_BELOW_PERCENT + 10) {
      suddenDarkCooldown = now + 10000;
      if (!runTrigger(TG_DARK)) react(EX_SURPRISED, 2400, "Eek! Who turned", "off the light?!", TRK_EEK, 2, 80, 60);
    }
  }

  // Day / night with hysteresis and a 3 s confirmation delay.
  if (!isNight) {
    if (lightPercent < NIGHT_BELOW_PERCENT) {
      if (!nightCandidateSince) nightCandidateSince = now;
      if (now - nightCandidateSince > 3000) {
        isNight = true;
        nightCandidateSince = 0;
        Serial.println("Light: NIGHT");
        if (!reactionActive()) {
          if (!runTrigger(TG_NIGHT)) react(EX_SLEEPY, 2500, "It's getting", "dark... yawn~", TRK_SLEEPY);
        }
      }
    } else {
      nightCandidateSince = 0;
    }
  } else {
    if (lightPercent > DAY_ABOVE_PERCENT) {
      if (!dayCandidateSince) dayCandidateSince = now;
      if (now - dayCandidateSince > 3000) {
        isNight = false;
        dayCandidateSince = 0;
        Serial.println("Light: DAY");
        if (mode == MODE_SLEEP) wakeUp(false);
        else {
          if (!runTrigger(TG_BRIGHT)) react(EX_EXCITED, 2000, "Yay! The light", "is back!", TRK_YAY);
        }
      }
    } else {
      dayCandidateSince = 0;
    }
  }
}

// =================================================================
//  MOTION  (functions 14, 15, 16, 17)
// =================================================================
void updateMotion() {
  static uint32_t lastRead = 0, hitWindow = 0, upsideSince = 0, lastUpsideReact = 0;
  static uint32_t shakeCooldown = 0, moveCooldown = 0, movePendingAt = 0;
  static float prevMag = 9.8f;
  static uint8_t hits = 0;
  static bool wasUpside = false;
  const uint32_t now = millis();
  if (!mpuOk || now - lastRead < 20) return;
  lastRead = now;

  sensors_event_t a, g, t;
  mpu.getEvent(&a, &g, &t);
  {
    // Data validation: all-zero or impossible readings for 2 s = the sensor stopped answering.
    static uint16_t badReads = 0;
    const float m2 = a.acceleration.x * a.acceleration.x + a.acceleration.y * a.acceleration.y +
                     a.acceleration.z * a.acceleration.z;
    const bool bad = !isfinite(m2) || m2 < 0.01f || m2 > 40000.0f;
    badReads = bad ? badReads + 1 : 0;
    if (bad) {
      if (badReads >= 100) {
        mpuOk = false;
        badReads = 0;
        tiltX = 0;
        logEvent("FAULT", "MPU6050 readings invalid - motion paused, retrying");
      }
      return;  // never react to an invalid reading
    }
  }
  fax += (a.acceleration.x - fax) * 0.2f;
  fay += (a.acceleration.y - fay) * 0.2f;
  faz += (a.acceleration.z - faz) * 0.2f;
  accelMag = sqrtf(a.acceleration.x * a.acceleration.x + a.acceleration.y * a.acceleration.y +
                   a.acceleration.z * a.acceleration.z);
  const float jerk = fabsf(accelMag - prevMag);
  prevMag = accelMag;
  const float gyroMag = sqrtf(g.gyro.x * g.gyro.x + g.gyro.y * g.gyro.y + g.gyro.z * g.gyro.z);
  gyroNow = gyroMag;

  // Tilt-responsive eyes (function 14)
  float side = (TILT_USE_X_AXIS ? fax : fay) / 7.0f;
  if (TILT_INVERT) side = -side;
  tiltX = constrain(side, -1.0f, 1.0f);

  if (mode != MODE_NORMAL) return;

  // Upside-down (function 16)
  const bool nowUpside = faz < -6.5f;
  if (nowUpside) {
    if (!upsideSince) upsideSince = now;
    if (now - upsideSince > 700) {
      upsideDown = true;
      if (now - lastUpsideReact > 6000) {
        lastUpsideReact = now;
        lastActivity = now;
        if (runTrigger(TG_UPSIDE)) {
          // custom action chosen in the app
        } else if (random(0, 2)) {
          react(EX_ANGRY, 2600, "Hey! Put me", "right side up!", TRK_HEY, 2, 100, 100);
        } else {
          react(EX_CONFUSED, 2600, "Huh? Why is the", "world upside?", TRK_HUH, 1, 120);
        }
      }
    }
  } else {
    upsideSince = 0;
    upsideDown = false;
  }
  if (wasUpside && !upsideDown && cfg.trig[TG_UPSIDE].type == ACT_DEFAULT) {
    react(EX_HAPPY, 1600, "Phew! Thanks~", "", TRK_HAPPY);
  }
  wasUpside = upsideDown;
  if (upsideDown) return;

  // Shake (function 15): 4 strong jerks inside 1 second
  if (!pending(shakeCooldown) && jerk > SHAKE_JERK) {
    if (now - hitWindow > 1000) {
      hitWindow = now;
      hits = 0;
    }
    if (++hits >= 4) {
      hits = 0;
      shakeCooldown = now + 3500;
      moveCooldown = now + 5000;
      movePendingAt = 0;
      lastActivity = now;
      if (!runTrigger(TG_SHAKE)) react(EX_DIZZY, 3000, "Whoaa~ I'm so", "dizzy @_@", TRK_DIZZY, 1, 260);
      return;
    }
  }

  // Movement (function 17): wait 600 ms so a shake can take priority.
  if (!pending(moveCooldown) && !movePendingAt && (gyroMag > MOVE_GYRO || jerk > 3.5f)) {
    movePendingAt = now;
  }
  if (movePendingAt && now - movePendingAt > 600) {
    movePendingAt = 0;
    moveCooldown = now + 5000;
    lastActivity = now;
    if (runTrigger(TG_MOVE)) {
      // custom action chosen in the app
    } else if (random(0, 2)) {
      react(EX_SURPRISED, 1800, "Whoa! Where are", "we going?", TRK_EEK, 1, 80);
    } else {
      react(EX_EXCITED, 1800, "Wheee~!", "Again! Again!", TRK_YAY, 1, 80);
    }
  }
}

// =================================================================
//  BEHAVIOR  (functions 11, 18, 19, 23, 24)
// =================================================================
void updateBehavior() {
  const uint32_t now = millis();

  // Wake-up animation finished -> greeting
  // ("start" after power-on, "wake" after sleeping: both can be changed in the app)
  if (greetAfterWake && !reactionActive()) {
    greetAfterWake = false;
    const bool startup = greetIsStartup;
    greetIsStartup = false;
    if (!runTrigger(startup ? TG_START : TG_WAKE)) {
      char hello[17];
      snprintf(hello, sizeof(hello), "I'm %s \x02", cfg.name);
      // After sleeping, the wake-up sound already played with the yawn.
      react(EX_HAPPY, 2400, isNight ? "Hello again!" : "Good morning!", hello,
            startup ? (isNight ? TRK_HELLO : TRK_MORNING) : TRK_NONE, 1, 60);
      // After sleeping: keep the happy face until the wake-up sound has ended.
      if (!startup && pending(wakeSoundUntil) && (int32_t)(wakeSoundUntil - reactionUntil) > 0) reactionUntil = wakeSoundUntil;
    }
  }

  if (mode != MODE_NORMAL) return;

  // Affection decays slowly when nobody pets Mochi (function 19)
  if (now - lastPet > 60000 && now - lastAffectionDecay > 60000) {
    lastAffectionDecay = now;
    addAffection(-1);
  }

  // Very low affection: Mochi cries (sad sound from folder 02)
  static bool sadNoticed = false;
  if (affection >= 40) sadNoticed = false;
  if (!sadNoticed && affection < 25 && !isNight && !reactionActive()) {
    sadNoticed = true;
    react(EX_CRY, 4000, "Nobody plays", "with me... T_T", TRK_SAD);
  }

  // Lonely reaction after long inactivity
  if (!isNight && !reactionActive() && now - lastPet > cfg.lonelyMin * 60000UL && now - lastLonely > 60000) {
    lastLonely = now;
    if (!runTrigger(TG_LONELY)) react(EX_SAD, 4000, "I miss you...", "Pet me please?", TRK_MISS);
  }

  // Automatic sleep when dark and inactive (function 11)
  if (cfg.autoSleep && isNight && !reactionActive() && now - lastActivity > cfg.sleepAfterS * 1000UL) {
    enterSleep();
    return;
  }

  // Idle like a Dasai Mochi (function 18): quiet little eye moves every few seconds
  // (glances, blinks, a smile, a wink). A short sound only now and then (Idle sounds setting).
  if (!reactionActive() && now >= nextIdleEvent) {
    nextIdleEvent = now + random(5000, 12000);
    const uint8_t pick = random(0, 10);
    if (isNight) {
      if (pick < 3) react(EX_SLEEPY, 2500, nullptr, nullptr);            // heavy eyelids
      else { blinkStart = now; nextBlink = now + 600; }                    // slow blink
    } else if (pick <= 2) {                                                // glance to a side
      targetX = random(0, 2) ? random(9, 15) : -random(9, 15);
      targetY = random(-4, 5);
      nextLookChange = now + random(700, 1500);
    } else if (pick == 3) {                                                // look up, thinking
      targetX = random(-4, 5);
      targetY = -6;
      nextLookChange = now + 1000;
    } else if (pick <= 5) {                                                // double blink
      blinkStart = now;
      nextBlink = now + 260;
    } else if (pick == 6) {
      react(EX_HAPPY, 1000, nullptr, nullptr);                             // small smile
    } else if (pick == 7) {
      react(EX_WINK, 800, nullptr, nullptr);
    } else if (pick == 8 && affection > 70) {
      react(EX_SHY, 1200, nullptr, nullptr);
    } else if (cfg.idleSounds && cfg.sound && affection > 40 && (!lastIdleSound || now - lastIdleSound > IDLE_SOUND_GAP_MS)) {
      // A short, soft sound (all under 2.5 s), with its face and a line on the LCD.
      static const struct { uint8_t folder, file; Expr face; const char* line1; const char* line2; } IDLE_SOUNDS[] = {
        {F_GREETING, 2, EX_HAPPY, "Hi there!", "Hehe~"},
        {F_HUG, 2, EX_SHY, "Hug me?", LCD_HEART},
        {F_HUG, 1, EX_LOVE, "I love you!", LCD_HEART " " LCD_HEART},
      };
      const auto& snd = IDLE_SOUNDS[random(0, 3)];
      lastIdleSound = now;
      react(snd.face, 1500, snd.line1, snd.line2);
      pendingStop = false;
      pendingFolder = snd.folder;
      pendingFile = snd.file;
      startSoundFace(1500);
    } else {
      react(EX_HAPPY, 1000, nullptr, nullptr);
    }
  }
}

// =================================================================
//  OLED DRAWING  (functions 1, 2, 5)
// =================================================================
constexpr int EYE_LX = 38, EYE_RX = 90, EYE_Y = 28;
#define WHITE SH110X_WHITE
#define BLACK SH110X_BLACK

// ---- OLED video (converted by the app, stored as /video<N>.mvid) ----
// 16-byte header: "MVID", version, fps, frames (uint16 LE), width, height (uint16 LE),
// then 1024 bytes per frame: 128x64, 1 bit per pixel, row-major, MSB = leftmost pixel.
File videoFile;
bool videoOpen = false;
uint8_t videoFps = 10;
uint16_t videoFrames = 0;
int32_t videoShownFrame = -1;
uint8_t videoBuf[1024];

void closeVideo() {
  if (videoOpen) videoFile.close();
  videoOpen = false;
}

bool openVideo(uint8_t index) {
  closeVideo();
  if (!PLAY_SONG_VIDEOS) return false;  // songs show the dancing stick man
  char path[16];
  snprintf(path, sizeof(path), "/video%u.mvid", index + 1);
  if (!fsOk || !LittleFS.exists(path)) return false;
  videoFile = LittleFS.open(path, "r");
  uint8_t h[16];
  if (!videoFile || videoFile.read(h, 16) != 16 || memcmp(h, "MVID", 4) != 0 || h[5] == 0) {
    if (videoFile) videoFile.close();
    return false;
  }
  videoFps = h[5];
  videoFrames = h[6] | (h[7] << 8);
  if (!videoFrames) {
    videoFile.close();
    return false;
  }
  videoOpen = true;
  videoShownFrame = -1;
  Serial.printf("Video: %u frames at %u fps\n", videoFrames, videoFps);
  return true;
}

// Loops the animation for as long as the song plays.
// The video is stretched to the song's length and plays once, so it starts and ends with
// the sound (the last frame stays if the song is a little longer).
bool drawVideoFrame(uint32_t elapsed) {
  const uint32_t len = songLength(songIndex);
  uint32_t frame;
  if (len) {
    frame = (uint32_t)((uint64_t)elapsed * videoFrames / len);
    if (frame >= videoFrames) frame = videoFrames - 1;
  } else {
    frame = (uint32_t)(((uint64_t)elapsed * videoFps / 1000) % videoFrames);
  }
  if ((int32_t)frame != videoShownFrame) {
    videoFile.seek(16 + frame * 1024UL);
    if (videoFile.read(videoBuf, sizeof(videoBuf)) != sizeof(videoBuf)) return false;
    videoShownFrame = frame;
  }
  oled.drawBitmap(0, 0, videoBuf, 128, 64, WHITE);
  return true;
}

void drawHeart(int cx, int cy, int r) {
  oled.fillCircle(cx - r + 1, cy - r / 2, r, WHITE);
  oled.fillCircle(cx + r - 1, cy - r / 2, r, WHITE);
  oled.fillTriangle(cx - 2 * r + 1, cy - r / 2 + 2, cx + 2 * r - 1, cy - r / 2 + 2, cx, cy + 2 * r, WHITE);
}

void drawNoteIcon(int x, int y) {
  oled.fillCircle(x, y, 2, WHITE);
  oled.drawLine(x + 2, y, x + 2, y - 8, WHITE);
  oled.drawLine(x + 2, y - 8, x + 5, y - 6, WHITE);
}

void drawSmile(int y, int w) {
  for (int x = -w; x <= w; ++x) {
    const int dy = (x * x) / (w + 2);
    oled.drawPixel(64 + x, y + w / 2 - dy / 2, WHITE);
    oled.drawPixel(64 + x, y + w / 2 - dy / 2 + 1, WHITE);
  }
}

void drawEye(Expr e, bool left, int cx, int cy, float open, uint32_t now) {
  switch (e) {
    case EX_HAPPY:
      oled.fillCircle(cx, cy + 4, 15, WHITE);
      oled.fillCircle(cx, cy + 12, 15, BLACK);
      break;

    case EX_EXCITED: {
      const int b = (int)(sinf(now / 90.0f) * 3);
      oled.fillRoundRect(cx - 16, cy - 19 + b, 32, 38, 12, WHITE);
      oled.fillCircle(cx + 6, cy - 8 + b, 4, BLACK);
      oled.fillCircle(cx - 5, cy + 6 + b, 2, BLACK);
      if ((now / 150) % 2) {
        const int sx = left ? cx - 22 : cx + 22;
        oled.drawLine(sx - 3, cy - 18, sx + 3, cy - 18, WHITE);
        oled.drawLine(sx, cy - 21, sx, cy - 15, WHITE);
      }
    } break;

    case EX_SAD: {
      oled.fillRoundRect(cx - 15, cy - 14, 30, 30, 9, WHITE);
      if (left) oled.fillTriangle(cx - 17, cy - 16, cx + 17, cy - 16, cx - 17, cy + 2, BLACK);
      else oled.fillTriangle(cx - 17, cy - 16, cx + 17, cy - 16, cx + 17, cy + 2, BLACK);
      if (left) {
        const int ty = cy + 16 + (now / 45) % 18;
        oled.fillCircle(cx - 9, ty, 2, WHITE);
        oled.fillTriangle(cx - 11, ty, cx - 7, ty, cx - 9, ty - 5, WHITE);
      }
    } break;

    case EX_ANGRY: {
      const int j = (now / 60) % 2;
      oled.fillRoundRect(cx - 15 + j, cy - 12, 30, 28, 8, WHITE);
      if (left) oled.fillTriangle(cx - 17, cy - 14, cx + 17, cy - 14, cx + 17, cy + 4, BLACK);
      else oled.fillTriangle(cx - 17, cy - 14, cx + 17, cy - 14, cx - 17, cy + 4, BLACK);
    } break;

    case EX_SLEEPY: {
      const int lid = 17 + (int)(sinf(now / 600.0f) * 3);
      oled.fillRoundRect(cx - 15, cy - 17, 30, 34, 9, WHITE);
      oled.fillRect(cx - 16, cy - 18, 32, lid + 1, BLACK);
    } break;

    case EX_WAKING: {
      float p = (millis() - reactionStart) / 1100.0f;
      if (p > 1) p = 1;
      oled.fillRoundRect(cx - 15, cy - 17, 30, 34, 9, WHITE);
      oled.fillRect(cx - 16, cy - 18, 32, (int)(36 * (1.0f - p)), BLACK);
    } break;

    case EX_SLEEP:
      oled.fillCircle(cx, cy + 2, 13, WHITE);
      oled.fillCircle(cx, cy - 3, 13, BLACK);
      break;

    case EX_SURPRISED:
      oled.drawCircle(cx, cy, 17, WHITE);
      oled.drawCircle(cx, cy, 16, WHITE);
      oled.fillCircle(cx + (int)(lookX * 0.4f), cy + (int)(lookY * 0.4f), 6, WHITE);
      break;

    case EX_DIZZY: {
      const float phase = now / 120.0f * (left ? 1 : -1);
      for (float s = 0; s < 14; s += 0.3f) {
        const float r = s * 1.1f;
        oled.fillCircle(cx + (int)(cosf(s + phase) * r), cy + (int)(sinf(s + phase) * r), 1, WHITE);
      }
    } break;

    case EX_LOVE:
      drawHeart(cx, cy - 2, ((now / 220) % 2) ? 9 : 8);
      break;

    case EX_WINK:
      if (left) {
        oled.fillRoundRect(cx - 15, cy - 15, 30, 30, 9, WHITE);
        oled.fillCircle(cx + 5, cy - 6, 4, BLACK);  // sparkle in the open eye
      } else {
        oled.fillCircle(cx, cy + 4, 15, WHITE);      // closed eye (^)
        oled.fillCircle(cx, cy + 12, 15, BLACK);
      }
      break;

    case EX_LAUGH: {
      const int b = (int)(sinf(now / 70.0f) * 2);
      oled.fillCircle(cx, cy + 4 + b, 15, WHITE);
      oled.fillCircle(cx, cy + 12 + b, 15, BLACK);
      oled.fillCircle(left ? cx - 20 : cx + 20, cy + 6 + b + (int)((now / 60) % 8), 2, WHITE);  // tears of joy
    } break;

    case EX_CRY: {
      oled.fillRoundRect(cx - 15, cy - 14, 30, 30, 9, WHITE);
      if (left) oled.fillTriangle(cx - 17, cy - 16, cx + 17, cy - 16, cx - 17, cy + 2, BLACK);
      else oled.fillTriangle(cx - 17, cy - 16, cx + 17, cy - 16, cx + 17, cy + 2, BLACK);
      for (uint8_t k = 0; k < 2; ++k) {  // two streams of tears under each eye
        const int ty = cy + 16 + (int)((now / 30 + k * 11) % 24);
        const int tx = cx + (left ? -8 : 8) + (k ? 4 : -4);
        oled.fillCircle(tx, ty, 2, WHITE);
        oled.fillTriangle(tx - 2, ty, tx + 2, ty, tx, ty - 5, WHITE);
      }
    } break;

    case EX_SHY:
      oled.fillCircle(cx, cy - 2, 14, WHITE);        // eyes looking down (u)
      oled.fillCircle(cx, cy - 10, 14, BLACK);
      break;

    case EX_CONFUSED:
      if (left) {
        oled.fillRoundRect(cx - 15, cy - 15, 30, 30, 9, WHITE);
      } else {
        oled.fillRoundRect(cx - 13, cy - 4, 26, 10, 4, WHITE);
      }
      break;

    default: {  // EX_NORMAL with blinking
      int h = (int)(34 * open);
      if (h < 3) h = 3;
      oled.fillRoundRect(cx - 15, cy - h / 2, 30, h, h / 2 < 9 ? h / 2 : 9, WHITE);
    } break;
  }
}

void drawMouthAndExtras(Expr e, uint32_t now) {
  switch (e) {
    case EX_HAPPY:
      drawSmile(52, 6);
      break;
    case EX_EXCITED:
      oled.fillCircle(64, 52, 6, WHITE);
      oled.fillRect(57, 45, 15, 7, BLACK);
      break;
    case EX_SAD:
      for (int x = -6; x <= 6; ++x) oled.drawPixel(64 + x, 56 - (36 - x * x) / 12, WHITE);
      break;
    case EX_ANGRY:
      oled.drawLine(57, 55, 61, 52, WHITE);
      oled.drawLine(61, 52, 65, 55, WHITE);
      oled.drawLine(65, 55, 69, 52, WHITE);
      break;
    case EX_SURPRISED:
      oled.drawCircle(64, 54, 4, WHITE);
      break;
    case EX_DIZZY:
      for (int x = -10; x <= 10; ++x)
        oled.drawPixel(64 + x, 54 + (int)(2 * sinf(x * 0.9f + now / 100.0f)), WHITE);
      break;
    case EX_LOVE:
      drawSmile(52, 5);
      oled.drawLine(22, 50, 26, 46, WHITE);
      oled.drawLine(28, 50, 32, 46, WHITE);
      oled.drawLine(96, 50, 100, 46, WHITE);
      oled.drawLine(102, 50, 106, 46, WHITE);
      break;
    case EX_WINK:
      drawSmile(52, 6);
      break;
    case EX_LAUGH:
      oled.fillCircle(64, 50, 9, WHITE);              // big open laughing mouth
      oled.fillRect(53, 38, 23, 12, BLACK);
      break;
    case EX_CRY: {
      const int w = 5 + (int)((now / 120) % 2);       // trembling mouth
      for (int x = -w; x <= w; ++x) oled.drawPixel(64 + x, 58 - (w * w - x * x) / 6, WHITE);
    } break;
    case EX_SHY:
      drawSmile(54, 3);
      for (uint8_t k = 0; k < 3; ++k) {               // blush lines on both cheeks
        oled.drawLine(20 + k * 5, 50, 24 + k * 5, 45, WHITE);
        oled.drawLine(98 + k * 5, 50, 102 + k * 5, 45, WHITE);
      }
      break;
    case EX_CONFUSED:
      oled.setTextSize(2);
      oled.setTextColor(WHITE);
      oled.setCursor(112, 0);
      oled.print("?");
      oled.drawLine(58, 54, 70, 52, WHITE);
      break;
    case EX_SLEEPY:
    case EX_SLEEP: {
      const int z = (now / 400) % 12;
      oled.setTextSize(1);
      oled.setTextColor(WHITE);
      oled.setCursor(106, 12 - z / 2);
      oled.print("z");
      if (e == EX_SLEEP) {
        oled.setCursor(114, 4 - z / 3 + 4);
        oled.print("Z");
      }
    } break;
    default:
      break;
  }
}

void thickLine(int x0, int y0, int x1, int y1) {
  oled.drawLine(x0, y0, x1, y1, WHITE);
  oled.drawLine(x0 + 1, y0, x1 + 1, y1, WHITE);
  oled.drawLine(x0, y0 + 1, x1, y1 + 1, WHITE);
}

void drawDancer(uint8_t frame) {
  const int b = (frame == 1 || frame == 3) ? 2 : 0;
  const int headY = 10 + b, shoulderY = 24 + b, hipY = 40 + b;
  oled.fillCircle(64, headY, 7, WHITE);
  oled.fillRect(61, headY + 7, 7, hipY - headY - 6, WHITE);
  switch (frame) {
    case 0:
      thickLine(63, shoulderY, 47, 13 + b); thickLine(47, 13 + b, 42, 20 + b);
      thickLine(67, shoulderY, 82, 34 + b);
      thickLine(63, hipY, 49, 57); thickLine(66, hipY, 82, 53);
      break;
    case 1:
      thickLine(63, shoulderY, 46, 34 + b);
      thickLine(67, shoulderY, 82, 13 + b); thickLine(82, 13 + b, 87, 20 + b);
      thickLine(63, hipY, 47, 52); thickLine(66, hipY, 80, 58);
      break;
    case 2:
      thickLine(63, shoulderY, 48, 10); thickLine(67, shoulderY, 82, 10);
      thickLine(63, hipY, 45, 56); thickLine(66, hipY, 84, 56);
      break;
    default:
      thickLine(63, shoulderY, 45, 27 + b); thickLine(67, shoulderY, 84, 27 + b);
      thickLine(63, hipY, 50, 50); thickLine(50, 50, 43, 48);
      thickLine(66, hipY, 79, 50); thickLine(79, 50, 86, 48);
      break;
  }
}

void drawMusicScreen(uint32_t now) {
  // Floating notes on both sides
  for (uint8_t i = 0; i < 3; ++i) {
    const int y = 60 - (int)((now / 30 + i * 22) % 64);
    drawNoteIcon(i % 2 ? 112 : 10, y);
    drawNoteIcon(i % 2 ? 20 : 104, (y + 30) % 64);
  }

  if (musicPhase == MP_DONE) {
    drawEye(EX_LOVE, true, EYE_LX, EYE_Y, 1, now);
    drawEye(EX_LOVE, false, EYE_RX, EYE_Y, 1, now);
    return;
  }
  if (musicPhase == MP_INTRO) {
    drawEye(EX_HAPPY, true, EYE_LX, EYE_Y + (int)(sinf(now / 150.0f) * 3), 1, now);
    drawEye(EX_HAPPY, false, EYE_RX, EYE_Y + (int)(sinf(now / 150.0f) * 3), 1, now);
    return;
  }
  // Alternate dancer and bouncing happy face every 16 notes
  if ((musicBeat / 16) % 2 == 0) {
    drawDancer(musicBeat % 4);
  } else {
    const int bounce = (musicBeat % 2) ? -3 : 2;
    drawEye(EX_HAPPY, true, EYE_LX, EYE_Y + bounce, 1, now);
    drawEye(EX_HAPPY, false, EYE_RX, EYE_Y + bounce, 1, now);
    drawSmile(52 + bounce, 6);
  }
}

void updateEyesMotion(uint32_t now) {
  // Tilt overrides idle looking (function 14); otherwise wander (function 18).
  if (mode == MODE_NORMAL && fabsf(tiltX) > 0.15f) {
    targetX = tiltX * 14.0f;
    targetY = 0;
  } else if (now >= nextLookChange) {
    nextLookChange = now + random(1500, 4000);
    if (random(0, 10) < 6) {
      targetX = 0;
      targetY = 0;
    } else {
      targetX = random(-12, 13);
      targetY = random(-5, 6);
    }
  }
  lookX += (targetX - lookX) * 0.25f;
  lookY += (targetY - lookY) * 0.25f;

  if (now >= nextBlink) {
    blinkStart = now;
    nextBlink = now + (random(0, 5) == 0 ? 260 : random(2500, isNight ? 3500 : 6000));
  }
}

void renderOled() {
  static uint32_t lastDraw = 0;
  const uint32_t now = millis();
  const uint32_t interval = mode == MODE_SLEEP ? 150 : 33;
  if (!oledOk || now - lastDraw < interval) return;
  lastDraw = now;

  updateEyesMotion(now);
  oled.clearDisplay();

  if (mode == MODE_MUSIC) {
    if (videoOpen && musicPhase != MP_DONE) {
      if (!drawVideoFrame(songElapsed())) {
        closeVideo();
        drawMusicScreen(now);
      }
    } else {
      drawMusicScreen(now);
    }
  } else {
    const Expr e = currentExpression();
    float open = 1.0f;
    const uint32_t bt = now - blinkStart;
    if (bt < 160) open = fabsf((float)bt - 80.0f) / 80.0f;

    const bool follow = e == EX_NORMAL || e == EX_SAD || e == EX_SLEEPY || e == EX_CONFUSED || e == EX_ANGRY ||
                        e == EX_WINK || e == EX_SHY || e == EX_CRY;
    const int ox = follow ? (int)lookX : 0;
    const int oy = follow ? (int)lookY : 0;
    const int breathe = mode == MODE_SLEEP ? (int)(sinf(now / 900.0f) * 2) : 0;
    drawEye(e, true, EYE_LX + ox, EYE_Y + oy + breathe, open, now);
    drawEye(e, false, EYE_RX + ox, EYE_Y + oy + breathe, open, now);
    drawMouthAndExtras(e, now);
  }

  // The I2C speed is set once in setup(). Changing it on every frame can
  // corrupt the OLED picture when other I2C devices share the bus.
  oled.display();
  if (evtWaitOled) noteResponse(true);
}

// =================================================================
//  LCD CONTENT  (functions 4, 6, 20)
// =================================================================
// What Mochi says on the LCD while idle. Every line fits the 16x2 LCD and changes every 8 s.
void idleDialogue(char* r0, char* r1) {
  const uint32_t slot = millis() / DIALOGUE_ROTATE_MS;
  auto show = [&](const char* const (*lines)[2], uint8_t count) {
    const uint8_t i = slot % count;
    copyText(r0, lines[i][0]);
    copyText(r1, lines[i][1]);
  };

  if (isNight) {
    static const char* const night[][2] = {
      {"It's getting", "late... yawn~"},
      {"Sleepy time", "soon... zzz"},
      {"One more pat", "before bed? " LCD_HEART},
      {"The stars are", "out tonight~"},
    };
    return show(night, 4);
  }

  if (affection < 25) {
    static const char* const lonely[][2] = {
      {"I miss you...", "Pet me please?"},
      {"Play with me?", "Just one tap~"},
      {"It's quiet...", "Are you there?"},
    };
    return show(lonely, 3);
  }

  switch (slot % 8) {
    case 0: snprintf(r0, 17, "Hi! I'm %s", cfg.name); copyText(r1, "Nice to see you!"); break;
    case 1: {
      copyText(r0, "Love meter:");
      char bar[17];
      const uint8_t hearts = affection / 20;
      uint8_t p = 0;
      for (uint8_t i = 0; i < 5; ++i) bar[p++] = i < hearts ? '\x02' : '-';
      snprintf(bar + p, sizeof(bar) - p, " %3d%%", affection);
      copyText(r1, bar);
    } break;
    case 2: copyText(r0, "Tap me twice"); copyText(r1, "for a song! " LCD_NOTE); break;
    case 3: copyText(r0, "Hold me for"); copyText(r1, "a big hug " LCD_HEART); break;
    case 4: copyText(r0, "Tilt me left"); copyText(r1, "or right~"); break;
    case 5: copyText(r0, "Let's play!"); copyText(r1, "Tap me~"); break;
    case 6: copyText(r0, affection > 70 ? "You're my best" : "Pet me a little"); copyText(r1, affection > 70 ? "friend! " LCD_HEART : "more? Hehe~"); break;
    default: copyText(r0, "I'm happy you're"); copyText(r1, "here! " LCD_HEART); break;
  }
}

void updateLcd() {
  static uint32_t lastUpdate = 0;
  static char prev0[17] = "#", prev1[17] = "#";
  if (lcdForceRedraw) {
    copyText(prev0, "#");
    copyText(prev1, "#");
    lcdForceRedraw = false;
  }
  const uint32_t now = millis();
  if (!lcd.ok || now - lastUpdate < 150) return;
  lastUpdate = now;

  char r0[17], r1[17];
  if (mode == MODE_MUSIC) {
    if (musicPhase == MP_DONE) {
      copyText(r0, "Playback done");
      copyText(r1, "Thank you! " LCD_HEART);
    } else if (musicPhase == MP_INTRO || musicCue < 0) {
      copyText(r0, LCD_NOTE " Playing...");
      copyText(r1, songTitle(songIndex));
    } else {
      copyText(r0, cues[musicCue].line1);
      copyText(r1, cues[musicCue].line2);
    }
  } else if (pending(msgUntil)) {
    copyText(r0, msg1);
    copyText(r1, msg2);
  } else if (mode == MODE_SLEEP) {
    copyText(r0, "Zzz...");
    copyText(r1, "Touch to wake");
  } else {
    idleDialogue(r0, r1);
  }

  if (strcmp(r0, prev0) != 0) {
    lcd.printRow(0, r0);
    copyText(prev0, r0);
  }
  if (strcmp(r1, prev1) != 0) {
    lcd.printRow(1, r1);
    copyText(prev1, r1);
  }
}

// =================================================================
//  WEB PAGES SERVED BY MOCHI  (setup hotspot + main app)
// =================================================================
#if MOCHI_WIFI
const char SETUP_HTML[] PROGMEM = R"MOCHI(<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>Mochi WiFi setup</title>
<style>body{font-family:system-ui,sans-serif;background:#f5f3ff;color:#1e1b4b;margin:0;padding:24px;max-width:420px;margin:auto}
h1{font-size:22px}input,button{width:100%;box-sizing:border-box;font-size:16px;padding:12px;border-radius:10px;border:1px solid #c4b5fd;margin:6px 0}
button{background:#7c3aed;color:#fff;border:0;font-weight:700}.card{background:#fff;border-radius:16px;padding:18px;box-shadow:0 1px 3px #0001}</style></head>
<body><h1>Hi! I'm Mochi</h1><div class="card"><p>Connect me to your home WiFi.</p>
<input id="s" placeholder="WiFi name (SSID)"><input id="p" type="password" placeholder="WiFi password">
<button onclick="save()">Save and connect</button><p id="m"></p></div>
<script>const save=async()=>{const b=new URLSearchParams({ssid:s.value,pass:p.value});
m.textContent='Saving...';try{await fetch('/api/wifi',{method:'POST',body:b})}catch(e){}
m.innerHTML='Saved! Mochi is restarting. Reconnect your phone to <b>'+s.value+'</b>, then open <b>http://mochi.local</b> (or the IP shown on Mochi\'s LCD).'}</script></body></html>)MOCHI";

const char APP_HTML[] PROGMEM = R"MOCHI(<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>Mochi</title>
<style>
body{font-family:system-ui,sans-serif;background:#f5f3ff;color:#1e1b4b;margin:0;padding:16px;max-width:760px;margin:auto}
h1{font-size:24px;margin:8px 0}h2{font-size:15px;text-transform:uppercase;letter-spacing:.08em;color:#6d28d9;margin:0 0 10px}
.card{background:#fff;border-radius:16px;padding:16px;margin:12px 0;box-shadow:0 1px 3px #0001}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(130px,1fr));gap:8px}
.stat b{display:block;font-size:18px}.stat span{font-size:12px;color:#64748b}
button{background:#7c3aed;color:#fff;border:0;border-radius:10px;padding:10px 12px;font-weight:700;font-size:14px;cursor:pointer}
button.g{background:#ede9fe;color:#5b21b6}input,select,textarea{font:inherit;padding:8px;border:1px solid #ddd6fe;border-radius:8px;box-sizing:border-box}
label{display:flex;justify-content:space-between;align-items:center;gap:8px;margin:6px 0;font-size:14px}
textarea{width:100%;min-height:150px;font-family:ui-monospace,monospace;font-size:13px}
canvas{image-rendering:pixelated;width:100%;max-width:384px;background:#000;border-radius:8px}
.row{display:flex;gap:8px;flex-wrap:wrap;align-items:center}.muted{color:#64748b;font-size:13px}
.pvcard{position:sticky;top:6px;z-index:5;display:flex;gap:12px;align-items:center;padding:10px 12px}
.pvcard canvas{width:176px;flex:none}.trow{padding:10px 0;border-top:1px solid #ede9fe}.trow select{max-width:100%}
</style></head><body>
<h1 id="title">Mochi</h1>
<div class="card row"><span>PIN</span><input id="pin" type="password" style="width:110px" placeholder="1234"><span class="muted">Needed for every change. Default 1234.</span></div>

<div class="card pvcard"><canvas id="pvFace" width="128" height="64"></canvas>
<div style="flex:1;min-width:0"><b style="font-size:13px;color:#6d28d9">PREVIEW ON THIS PHONE</b>
<div id="pvLyric" style="font-weight:700;min-height:38px;font-size:14px"></div><div id="pvNote" class="muted"></div>
<button class="g" style="padding:4px 10px;margin-top:4px" onclick="stopPreview()">Stop</button></div></div>

<div class="card"><h2>Status</h2><div class="grid">
<div class="stat"><b id="sMode">-</b><span>Mode</span></div><div class="stat"><b id="sExpr">-</b><span>Expression</span></div>
<div class="stat"><b id="sLove">-</b><span>Love meter</span></div><div class="stat"><b id="sLight">-</b><span>Light</span></div>
<div class="stat"><b id="sSong">-</b><span>Song</span></div><div class="stat"><b id="sDf">-</b><span>DFPlayer</span></div></div></div>

<div class="card"><h2>Play with Mochi</h2><div class="row">
<button onclick="act('pet')">Pet</button><button onclick="act('love')">Hug</button><button onclick="act('excited')">Play</button>
<button onclick="act('music')">Play Song 1</button><button class="g" onclick="act('stop')">Stop song</button>
<button class="g" onclick="act('wake')">Wake</button><button class="g" onclick="act('sleep')">Sleep</button></div>
<div class="row" style="margin-top:8px"><select id="mood"><option>happy</option><option>sad</option><option>angry</option><option>sleepy</option>
<option>excited</option><option>surprised</option><option>dizzy</option><option>love</option><option>confused</option>
<option>wink</option><option>laugh</option><option>cry</option><option>shy</option></select>
<button class="g" onclick="act('mood','&m='+mood.value)">Set mood</button></div></div>

<div class="card"><h2>What Mochi does</h2>
<p class="muted">Pick what Mochi does for each touch, movement, or light change. <b>Preview</b> shows the face, lyrics, and video and plays the sound here on the phone only. Press <b>Save to Mochi</b>, and then Mochi does it by itself when you touch or move it.</p>
<div id="trigList"></div>
<div class="row" style="margin-top:12px"><button onclick="saveTriggers()">Save to Mochi</button>
<button class="g" onclick="resetTriggers()">Back to defaults</button><span id="trMsg" class="muted"></span></div>
<p class="muted" style="margin-top:14px"><b>Sounds on this phone (optional).</b> To hear sounds and songs in the preview, pick your mp3 files once (for example the files from the SD card). They stay in this browser.</p>
<div class="row"><input id="sfiles" type="file" accept="audio/*" multiple><input id="sdir" type="file" webkitdirectory multiple>
</div><div id="sfCount" class="muted"></div></div>

<div class="card"><h2>Face for each sound</h2>
<p class="muted">Pick the face Mochi shows with each sound. "Show" previews it here on the phone. Press Save, and Mochi uses these faces by itself (no phone needed).</p>
<div id="soundList"></div>
<div class="row" style="margin-top:12px"><button onclick="saveSounds()">Save faces</button>
<button class="g" onclick="resetSounds()">Back to defaults</button><span id="sfMsg" class="muted"></span></div></div>

<div class="card"><h2>Settings</h2>
<label>Name<input id="name" maxlength="12"></label>
<label>Day volume <span id="dvv"></span><input id="dayVol" type="range" min="0" max="30" oninput="dvv.textContent=this.value"></label>
<label>Night volume <span id="nvv"></span><input id="nightVol" type="range" min="0" max="30" oninput="nvv.textContent=this.value"></label>
<label>Sleep after (seconds, when dark)<input id="sleepSec" type="number" min="10" max="3600" style="width:90px"></label>
<label>Auto sleep<input id="autoSleep" type="checkbox"></label><label>Vibration<input id="vibration" type="checkbox"></label>
<label>Voice and sounds<input id="sound" type="checkbox"></label>
<label>Song 1 title<input id="title1" maxlength="16"></label><label>Song 2 title<input id="title2" maxlength="16"></label>
<label>Song 3 title<input id="title3" maxlength="16"></label><label>New PIN (optional)<input id="newPin" type="password" style="width:110px"></label>
<button onclick="saveSettings()">Save settings</button> <span id="setMsg" class="muted"></span></div>

<div class="card"><h2>Lyrics</h2>
<div class="row"><select id="lslot" onchange="loadLyrics()"><option value="1">Song 1</option><option value="2">Song 2</option><option value="3">Song 3</option></select>
<button class="g" onclick="act('play','&slot='+lslot.value)">Play this song</button><button class="g" onclick="mark()">Insert current time</button></div>
<p class="muted">One line per lyric: <code>milliseconds|top line|bottom line</code> (16 characters per line). Play the song, then tap "Insert current time" at the start of each line.</p>
<textarea id="lyrics"></textarea><div class="row"><button onclick="saveLyrics()">Save lyrics</button>
<button class="g" onclick="resetLyrics()">Reset to built-in</button><span id="lyrMsg" class="muted"></span></div></div>

<div class="card"><h2>Video for the OLED</h2>
<p class="muted">Pick a video. It is converted here into a 128x64 black-and-white animation and saved on Mochi. It plays on the OLED together with the song in the same slot. Clear silhouettes (like dancers) look best.</p>
<div class="row"><select id="vslot"><option value="1">Song 1</option><option value="2">Song 2</option><option value="3">Song 3</option></select>
<input id="vfile" type="file" accept="video/*"></div>
<label>Frames per second<select id="vfps"><option>8</option><option selected>10</option><option>12</option><option>15</option></select></label>
<label>Max length (seconds)<input id="vmax" type="number" value="30" min="1" max="180" style="width:80px"></label>
<label>Fit<select id="vfit"><option value="contain">Whole video</option><option value="cover">Fill screen</option></select></label>
<label>Brightness threshold <span id="thv">128</span><input id="vthr" type="range" min="20" max="235" value="128" oninput="thv.textContent=this.value"></label>
<label>Dithering (shading)<input id="vdith" type="checkbox"></label><label>Invert colors<input id="vinv" type="checkbox"></label>
<div class="row"><input id="mfile" type="file"><button class="g" onclick="loadMvid()">Load .mvid file</button></div>
<div class="row"><button onclick="convert()">Convert</button><button onclick="upload()">Send to Mochi</button>
<button class="g" onclick="delVideo()">Delete video</button></div><p id="vmsg" class="muted"></p>
<canvas id="prev" width="128" height="64"></canvas></div>

<div class="card"><h2>WiFi and updates</h2>
<p class="muted">Firmware update: in Arduino IDE use Sketch &gt; Export Compiled Binary, then choose the .bin file here.</p>
<div class="row"><input id="fw" type="file" accept=".bin"><button onclick="updateFw()">Update firmware</button></div><p id="fwMsg" class="muted"></p>
<button class="g" onclick="forget()">Forget WiFi (back to setup hotspot)</button></div>

<script>
const $=id=>document.getElementById(id);let st={},video=null,timer=null;
pin.value=localStorage.mochiPin||'';pin.onchange=()=>localStorage.mochiPin=pin.value;
const k=()=>'key='+encodeURIComponent(pin.value||'1234');
const url=(p,q='')=>p+'?'+k()+q;
const post=async(p,q='',body)=>{const r=await fetch(url(p,q),{method:'POST',body});const t=await r.text();if(r.status==403)alert('Wrong PIN');return {ok:r.ok,t}}
const act=async(a,q='')=>{await post('/api/action','&do='+a+q);poll()}
const poll=async()=>{try{st=await (await fetch('/api/status')).json();title.textContent=st.name;
sMode.textContent=st.mode;sExpr.textContent=st.expr;sLove.textContent=st.love+'%';sLight.textContent=st.light+'% '+(st.night?'night':'day');
sSong.textContent=st.song?st.songTitle+' '+(st.songMs/1000).toFixed(1)+'s':'-';sDf.textContent=st.df?'OK':'not found'}catch(e){}}
setInterval(poll,1000);poll();
const loadSettings=async()=>{const r=await fetch(url('/api/settings'));if(!r.ok)return;const s=await r.json();S=s;
for(const id of ['name','dayVol','nightVol','sleepSec','title1','title2','title3'])$(id).value=s[id];
for(const id of ['autoSleep','vibration','sound'])$(id).checked=s[id];dvv.textContent=s.dayVol;nvv.textContent=s.nightVol}
const saveSettings=async()=>{const b=new URLSearchParams();for(const id of ['name','dayVol','nightVol','sleepSec','title1','title2','title3','newPin'])if($(id).value!=='')b.append(id,$(id).value);
for(const id of ['autoSleep','vibration','sound'])b.append(id,$(id).checked?1:0);const r=await post('/api/settings','',b);
if(r.ok&&newPin.value){pin.value=newPin.value;localStorage.mochiPin=pin.value;newPin.value=''}setMsg.textContent=r.ok?'Saved!':'Failed';poll()}
const loadLyrics=async()=>{const r=await fetch(url('/api/lyrics','&slot='+lslot.value));lyrics.value=r.ok?await r.text():''}
const saveLyrics=async()=>{const r=await post('/api/lyrics','&slot='+lslot.value,lyrics.value.trim()+'\n');lyrMsg.textContent=r.ok?'Saved!':'Failed'}
const resetLyrics=async()=>{await post('/api/lyrics/reset','&slot='+lslot.value);loadLyrics()}
const mark=()=>{if(!st.song){alert('Play the song first');return}const t=lyrics.value;lyrics.value=t+(t&&!t.endsWith('\n')?'\n':'')+st.songMs+'|';lyrics.focus()}
const convert=async()=>{const f=vfile.files[0];if(!f)return vmsg.textContent='Choose a video first.';
const fps=+vfps.value,thr=+vthr.value,dith=vdith.checked,inv=vinv.checked,cover=vfit.value=='cover';
const v=document.createElement('video');v.muted=true;v.playsInline=true;v.preload='auto';v.src=URL.createObjectURL(f);
try{await new Promise((ok,no)=>{v.onloadeddata=ok;v.onerror=no})}catch(e){return vmsg.textContent='This video format cannot be read here.'}
const dur=Math.min(v.duration,+vmax.value);let n=Math.max(1,Math.floor(dur*fps));
const maxFrames=Math.floor(((st.fsFree||0)-8192)/1024);if(maxFrames<1)return vmsg.textContent='Mochi storage is full. Delete a video first.';
let note='';if(n>maxFrames){n=maxFrames;note=' (shortened to fit storage)'}
const c=document.createElement('canvas');c.width=128;c.height=64;const g=c.getContext('2d',{willReadFrequently:true});
const out=new Uint8Array(16+n*1024);out.set([77,86,73,68,1,fps,n&255,n>>8,128,0,64,0]);
for(let i=0;i<n;i++){v.currentTime=Math.min(dur-0.01,i/fps+0.001);await new Promise(r=>v.onseeked=r);
g.fillStyle='#000';g.fillRect(0,0,128,64);const sc=(cover?Math.max:Math.min)(128/v.videoWidth,64/v.videoHeight);
const w=v.videoWidth*sc,h=v.videoHeight*sc;g.drawImage(v,(128-w)/2,(64-h)/2,w,h);const d=g.getImageData(0,0,128,64).data;
const L=new Float32Array(8192);for(let p=0;p<8192;p++){const y=.299*d[p*4]+.587*d[p*4+1]+.114*d[p*4+2];L[p]=inv?255-y:y}
const base=16+i*1024;for(let y=0;y<64;y++)for(let x=0;x<128;x++){const p=y*128+x,o=L[p],on=o>=thr;
if(on)out[base+y*16+(x>>3)]|=128>>(x&7);if(dith){const e=o-(on?255:0);if(x<127)L[p+1]+=e*7/16;
if(y<63){if(x>0)L[p+127]+=e*3/16;L[p+128]+=e*5/16;if(x<127)L[p+129]+=e/16}}}
if(i%5==0)vmsg.textContent='Converting '+Math.round((i+1)/n*100)+'%'}
URL.revokeObjectURL(v.src);video=out;vmsg.textContent=n+' frames, '+(n/fps).toFixed(1)+' s, '+Math.round(out.length/1024)+' KB'+note+'. Press "Send to Mochi".';preview(fps,n)}
const preview=(fps,n)=>{clearInterval(timer);const g=prev.getContext('2d'),img=g.createImageData(128,64);let i=0;
timer=setInterval(()=>{const b=16+i*1024;for(let p=0;p<8192;p++){const on=video[b+(p>>3)]&(128>>(p&7));const q=p*4;
img.data[q]=img.data[q+1]=img.data[q+2]=on?235:0;img.data[q+3]=255}g.putImageData(img,0,0);i=(i+1)%n},1000/fps)}
const loadMvid=async()=>{const f=mfile.files[0];if(!f)return vmsg.textContent='Choose the song .mvid file first.';
const b=new Uint8Array(await f.arrayBuffer());if(b.length<16||String.fromCharCode(b[0],b[1],b[2],b[3])!=='MVID')return vmsg.textContent='This is not a Mochi video file.';
const fps=b[5],n=b[6]|(b[7]<<8);if(!fps||!n||b.length!==16+n*1024)return vmsg.textContent='This video file is damaged.';
video=b;vmsg.textContent=n+' frames, '+(n/fps).toFixed(1)+' s loaded. Press "Send to Mochi".';preview(fps,n)}
const upload=()=>{if(!video)return vmsg.textContent='Convert a video first.';const fd=new FormData();fd.append('file',new Blob([video]),'video.mvid');
const x=new XMLHttpRequest();x.open('POST',url('/api/video','&slot='+vslot.value));
x.upload.onprogress=e=>vmsg.textContent='Sending '+Math.round(e.loaded/e.total*100)+'%';
x.onload=()=>vmsg.textContent=x.status==200?'Saved on Mochi! Play song '+vslot.value+' to watch it.':'Failed: '+x.responseText;
x.onerror=()=>vmsg.textContent='Send failed.';x.send(fd)}
const delVideo=async()=>{const r=await post('/api/video/delete','&slot='+vslot.value);vmsg.textContent=r.ok?'Deleted.':'Failed';poll()}
const updateFw=()=>{const f=fw.files[0];if(!f)return;const fd=new FormData();fd.append('firmware',f,f.name);const x=new XMLHttpRequest();
x.open('POST',url('/update'));x.upload.onprogress=e=>fwMsg.textContent='Uploading '+Math.round(e.loaded/e.total*100)+'%';
x.onload=()=>fwMsg.textContent=x.status==200?'Done! Mochi is restarting...':'Failed: '+x.responseText;x.send(fd)}
const forget=async()=>{if(confirm('Forget WiFi? Mochi will restart as the "Mochi-Setup" hotspot.'))await post('/api/wifi/forget')}

/* ---------- phone preview: Mochi's faces drawn here (same look as the OLED) ---------- */
let sounds=[],faces=[],S={},TR={},sg=null,curAudio=null,face='normal',faceT0=0,faceUntil=0;
const G=pvFace.getContext('2d'),WH='#f4f4f5';
const fill=c=>G.fillStyle=c?WH:'#000';
const circ=(x,y,r,c=1)=>{fill(c);G.beginPath();G.arc(x,y,r,0,6.3);G.fill()};
const rrect=(x,y,w,h,r,c=1)=>{fill(c);G.beginPath();G.roundRect(x,y,w,h,r);G.fill()};
const rect=(x,y,w,h,c=1)=>{fill(c);G.fillRect(x,y,w,h)};
const tri=(a,b,c,d,e,f,k=1)=>{fill(k);G.beginPath();G.moveTo(a,b);G.lineTo(c,d);G.lineTo(e,f);G.fill()};
const line=(a,b,c,d)=>{G.strokeStyle=WH;G.lineWidth=1.3;G.beginPath();G.moveTo(a,b);G.lineTo(c,d);G.stroke()};
const ring=(x,y,r)=>{G.strokeStyle=WH;G.lineWidth=2;G.beginPath();G.arc(x,y,r,0,6.3);G.stroke()};
const px=(x,y)=>rect(x,y,1,1);
const heart=(cx,cy,r)=>{circ(cx-r+1,cy-r/2,r);circ(cx+r-1,cy-r/2,r);tri(cx-2*r+1,cy-r/2+2,cx+2*r-1,cy-r/2+2,cx,cy+2*r)};
const smile=(y,w)=>{for(let x=-w;x<=w;x++){const dy=Math.floor(x*x/(w+2)),yy=y+Math.floor(w/2)-Math.floor(dy/2);px(64+x,yy);px(64+x,yy+1)}};
const sadEye=(l,cx,cy)=>{rrect(cx-15,cy-14,30,30,9);l?tri(cx-17,cy-16,cx+17,cy-16,cx-17,cy+2,0):tri(cx-17,cy-16,cx+17,cy-16,cx+17,cy+2,0)};
const happyEye=(cx,cy,b=0)=>{circ(cx,cy+4+b,15);circ(cx,cy+12+b,15,0)};
const txt=(t,x,y,sz)=>{fill(1);G.font='bold '+sz+'px monospace';G.textBaseline='top';G.fillText(t,x,y)};
const eye=(e,l,cx,cy,t)=>{const sn=Math.sin;switch(e){
case'happy':happyEye(cx,cy);break;
case'excited':{const b=sn(t/90)*3;rrect(cx-16,cy-19+b,32,38,12);circ(cx+6,cy-8+b,4,0);circ(cx-5,cy+6+b,2,0);
if(Math.floor(t/150)%2){const sx=l?cx-22:cx+22;line(sx-3,cy-18,sx+3,cy-18);line(sx,cy-21,sx,cy-15)}}break;
case'sad':sadEye(l,cx,cy);if(l){const ty=cy+16+Math.floor(t/45)%18;circ(cx-9,ty,2);tri(cx-11,ty,cx-7,ty,cx-9,ty-5)}break;
case'angry':{const j=Math.floor(t/60)%2;rrect(cx-15+j,cy-12,30,28,8);l?tri(cx-17,cy-14,cx+17,cy-14,cx+17,cy+4,0):tri(cx-17,cy-14,cx+17,cy-14,cx-17,cy+4,0)}break;
case'sleepy':rrect(cx-15,cy-17,30,34,9);rect(cx-16,cy-18,32,17+sn(t/600)*3+1,0);break;
case'waking':{const p=Math.min(1,(t%2000)/1100);rrect(cx-15,cy-17,30,34,9);rect(cx-16,cy-18,32,36*(1-p),0)}break;
case'sleep':circ(cx,cy+2,13);circ(cx,cy-3,13,0);break;
case'surprised':ring(cx,cy,16.5);circ(cx,cy,6);break;
case'dizzy':{const ph=t/120*(l?1:-1);for(let q=0;q<14;q+=.3){const r=q*1.1;circ(cx+Math.cos(q+ph)*r,cy+sn(q+ph)*r,1)}}break;
case'love':heart(cx,cy-2,Math.floor(t/220)%2?9:8);break;
case'wink':if(l){rrect(cx-15,cy-15,30,30,9);circ(cx+5,cy-6,4,0)}else happyEye(cx,cy);break;
case'laugh':{const b=sn(t/70)*2;happyEye(cx,cy,b);circ(l?cx-20:cx+20,cy+6+b+Math.floor(t/60)%8,2)}break;
case'cry':sadEye(l,cx,cy);for(let k=0;k<2;k++){const ty=cy+16+Math.floor(t/30+k*11)%24,tx=cx+(l?-8:8)+(k?4:-4);circ(tx,ty,2);tri(tx-2,ty,tx+2,ty,tx,ty-5)}break;
case'shy':circ(cx,cy-2,14);circ(cx,cy-10,14,0);break;
case'confused':l?rrect(cx-15,cy-15,30,30,9):rrect(cx-13,cy-4,26,10,4);break;
default:{const ph=t%3600,op=ph<160?Math.abs(ph-80)/80:1,h=Math.max(3,34*op);rrect(cx-15,cy-h/2,30,h,Math.min(h/2,9))}}};
const blush=()=>{for(let k=0;k<3;k++){line(20+k*5,50,24+k*5,45);line(98+k*5,50,102+k*5,45)}};
const mouth=(e,t)=>{switch(e){
case'happy':case'wink':smile(52,6);break;
case'excited':circ(64,52,6);rect(57,45,15,7,0);break;
case'sad':for(let x=-6;x<=6;x++)px(64+x,56-Math.floor((36-x*x)/12));break;
case'angry':line(57,55,61,52);line(61,52,65,55);line(65,55,69,52);break;
case'surprised':ring(64,54,4);break;
case'dizzy':for(let x=-10;x<=10;x++)px(64+x,54+Math.round(2*Math.sin(x*.9+t/100)));break;
case'love':smile(52,5);line(22,50,26,46);line(28,50,32,46);line(96,50,100,46);line(102,50,106,46);break;
case'laugh':circ(64,50,9);rect(53,38,23,12,0);break;
case'cry':{const w=5+Math.floor(t/120)%2;for(let x=-w;x<=w;x++)px(64+x,58-Math.floor((w*w-x*x)/6))}break;
case'shy':smile(54,3);blush();break;
case'confused':txt('?',112,0,16);line(58,54,70,52);break;
case'sleepy':case'sleep':txt('z',106,12-Math.floor(t/400)%12/2,9);if(e=='sleep')txt('Z',112,6,9);break}};
const drawFace=(e,t)=>{rect(0,0,128,64,0);eye(e,1,38,28,t);eye(e,0,90,28,t);mouth(e,t)};
const showFace=(n,ms=3000)=>{face=n;faceT0=performance.now();faceUntil=ms?faceT0+ms:0};
const stopPreview=()=>{sg=null;if(curAudio){curAudio.pause();curAudio=null}pvLyric.textContent='';showFace('normal',0)};
setInterval(()=>{const now=performance.now();
if(sg){const el=now-sg.t0;
if(sg.v){const i=Math.floor(el*sg.fps/1000)%sg.n,b=16+i*1024,im=G.createImageData(128,64);
for(let q=0;q<8192;q++){const on=sg.v[b+(q>>3)]&(128>>(q&7)),o=q*4;im.data[o]=im.data[o+1]=im.data[o+2]=on?235:0;im.data[o+3]=255}G.putImageData(im,0,0)}
else drawFace('excited',el);
let k=-1;for(let i=0;i<sg.cues.length;i++)if(sg.cues[i][0]<=el)k=i;
pvLyric.innerHTML=k>=0?sg.cues[k][1]+'<br>'+sg.cues[k][2]:'&#9835; ...';
if(el>sg.until)stopPreview();return}
if(faceUntil&&now>faceUntil){face='normal';faceUntil=0}
drawFace(face,now-faceT0)},33);

/* ---------- sounds stored on this phone (for previews only) ---------- */
const snd={};
const dbp=new Promise(r=>{try{const q=indexedDB.open('mochi',1);q.onupgradeneeded=()=>q.result.createObjectStore('s');q.onsuccess=()=>r(q.result);q.onerror=()=>r(null)}catch(e){r(null)}});
const idbPut=async(k,v)=>{const d=await dbp;if(d)d.transaction('s','readwrite').objectStore('s').put(v,k)};
const idbAll=async()=>{const d=await dbp;if(!d)return{};return new Promise(r=>{const m={},q=d.transaction('s').objectStore('s').openCursor();q.onsuccess=()=>{const c=q.result;if(c){m[c.key]=c.value;c.continue()}else r(m)};q.onerror=()=>r(m)})};
const keyFor=f=>{const p=(f.webkitRelativePath||'').split('/'),base=f.name.replace(/\.[^.]+$/,''),num=parseInt(base),nm=base.replace(/^\d+[_\- ]*/,'').toLowerCase(),dir=p.length>1?p[p.length-2]:'';
if(/^\d{1,2}$/.test(dir)&&num)return+dir+'_'+num;const m=sounds.find(x=>x.name==nm);if(m)return m.f+'_'+m.n;if(/song/.test(nm)&&num)return'14_'+num;return null};
const sfShow=()=>sfCount.textContent=Object.keys(snd).length+' sounds on this phone';
const addSounds=files=>{for(const f of files){const k=keyFor(f);if(!k)continue;snd[k]=URL.createObjectURL(f);idbPut(k,f)}sfShow()};
sfiles.onchange=()=>addSounds(sfiles.files);sdir.onchange=()=>addSounds(sdir.files);
idbAll().then(m=>{for(const k in m)snd[k]=URL.createObjectURL(m[k]);sfShow()});
const playUrl=u=>{if(curAudio)curAudio.pause();curAudio=new Audio(u);curAudio.play().catch(()=>{})};
const hear=(f,n)=>{const u=snd[f+'_'+n];if(u){playUrl(u);pvNote.textContent=''}else pvNote.textContent='Sound not loaded on this phone. Pick your mp3 files below.'};
const MOODF={happy:1,sad:2,cry:2,angry:3,sleepy:4,excited:5,laugh:5,surprised:6,love:10,shy:7,wink:9,dizzy:11,confused:11};
const hearMood=m=>{const f=MOODF[m];if(!f)return;const ks=[1,2,3,4].filter(n=>snd[f+'_'+n]);if(ks.length)hear(f,ks[Math.floor(Math.random()*ks.length)]);else pvNote.textContent=f?'Sound not loaded on this phone. Pick your mp3 files below.':''};
const faceOf=(f,n)=>{const e=$('sf'+f+'_'+n);if(e)return e.value;const x=sounds.find(y=>y.f==f&&y.n==n);return x?x.face:'happy'};

/* ---------- song preview: video + lyrics + sound, all on the phone ---------- */
const previewSong=async slot=>{stopPreview();pvLyric.textContent='Loading...';
const lr=await fetch(url('/api/lyrics','&slot='+slot));
const cues=lr.ok?(await lr.text()).split('\n').map(l=>l.split('|')).filter(c=>c.length>=2&&c[0]!==''&&!isNaN(+c[0])).map(c=>[+c[0],c[1]||'',c[2]||'']):[];
let v=null,fps=10,n=0;const vr=await fetch(url('/api/video','&slot='+slot));
if(vr.ok){const b=new Uint8Array(await vr.arrayBuffer());if(b.length>=16&&String.fromCharCode(b[0],b[1],b[2],b[3])==='MVID'){fps=b[5];n=b[6]|(b[7]<<8);if(fps&&n&&b.length===16+n*1024)v=b}}
let until=(cues.length?cues[cues.length-1][0]:0)+6000;const u=snd['14_'+slot];
if(u){playUrl(u);curAudio.onloadedmetadata=()=>{if(sg&&curAudio)sg.until=curAudio.duration*1000};curAudio.onended=stopPreview}
pvNote.textContent=(v?'Video: yes':'Video: none (Mochi dances)')+' | Lyrics: '+(cues.length?cues.length+' lines':'none')+' | Sound: '+(u?'yes':'not loaded');
sg={t0:performance.now(),cues,v,fps,n,until}};

/* ---------- what Mochi does: pick, preview, save ---------- */
const TRIGS={tap1:'1 touch',tap2:'2 touches',tap3:'3 or more touches',long:'Hold (long touch)',shake:'Shake',upside:'Upside down',move:'Carried / moving',dark:'Light suddenly off',night:'Room gets dark (night)',bright:'Light comes back',lonely:'Nobody touches for a long time'};
const TYPEL={default:'Normal (built-in)',mood:'Show a mood',sound:'Play a sound',song:'Play a song',none:'Do nothing'};
const initVal=t=>t.type=='mood'?t.mood:t.type=='sound'?t.f+'_'+t.n:t.slot;
const fillChoice=(id,type,val)=>{const c=$('tc_'+id);c.style.display=(type=='default'||type=='none')?'none':'';let o='';
if(type=='mood')o=faces.slice(0,14).map(m=>'<option'+(m==val?' selected':'')+'>'+m+'</option>').join('');
if(type=='sound')o=sounds.map(x=>{const k=x.f+'_'+x.n;return'<option value="'+k+'"'+(k==val?' selected':'')+'>'+String(x.f).padStart(2,'0')+' '+FOLDER[x.f]+' - '+x.name+'</option>'}).join('');
if(type=='song')o=[1,2,3].map(i=>'<option value="'+i+'"'+(i==val?' selected':'')+'>'+(S['title'+i]||'Song '+i)+'</option>').join('');
c.innerHTML=o};
const trType=id=>fillChoice(id,$('tt_'+id).value,initVal(TR[id]));
const trPreview=id=>{const t=$('tt_'+id).value,v=$('tc_'+id).value;stopPreview();
if(t=='mood'){showFace(v);hearMood(v)}else if(t=='sound'){const[f,n]=v.split('_');showFace(faceOf(f,n));hear(f,n)}
else if(t=='song')previewSong(v);else pvNote.textContent=t=='none'?'Mochi does nothing for this.':'Mochi uses its normal built-in reaction.'};
const loadTriggers=async()=>{const r=await fetch(url('/api/triggers'));if(!r.ok)return;const s=await r.json();let h='';
for(const t of s.triggers){TR[t.id]=t;h+='<div class="trow"><b>'+(TRIGS[t.id]||t.id)+'</b><div class="row"><select id="tt_'+t.id+'" data-id="'+t.id+'" onchange="trType(this.dataset.id)">'
+Object.keys(TYPEL).map(x=>'<option value="'+x+'"'+(x==t.type?' selected':'')+'>'+TYPEL[x]+'</option>').join('')
+'</select><select id="tc_'+t.id+'"></select><button class="g" data-id="'+t.id+'" onclick="trPreview(this.dataset.id)">Preview</button></div></div>'}
trigList.innerHTML=h;for(const id in TR)fillChoice(id,TR[id].type,initVal(TR[id]))};
const saveTriggers=async()=>{const b=new URLSearchParams();for(const id in TR){const t=$('tt_'+id).value,c=$('tc_'+id).value;
b.append(id,t=='mood'?'mood:'+c:t=='sound'?'sound:'+c.replace('_',':'):t=='song'?'song:'+c:t)}
const r=await post('/api/triggers','',b);trMsg.textContent=r.ok?'Saved on Mochi! Now touch or move Mochi.':'Failed'};
const resetTriggers=async()=>{if(!confirm('Back to the normal built-in reactions?'))return;await post('/api/triggers','&reset=1');await loadTriggers();trMsg.textContent='Defaults restored'};
const FOLDER=['','Happy','Sad','Angry','Sleepy / good night','Excited','Surprised','Love','Greetings','Tap','Hug / love','Shake / dizzy / confused','Lonely','Wake up'];
const loadSounds=async()=>{const r=await fetch(url('/api/sounds'));if(!r.ok)return;const s=await r.json();sounds=s.sounds;faces=s.faces;let h='',last=0;
for(const x of s.sounds){if(x.f!=last){h+='<p class="muted" style="margin:12px 0 4px"><b>'+String(x.f).padStart(2,'0')+' '+FOLDER[x.f]+'</b></p>';last=x.f}
h+='<div class="row" style="margin:4px 0"><span style="flex:1;min-width:120px">'+String(x.n).padStart(3,'0')+'_'+x.name+'</span><select id="sf'+x.f+'_'+x.n+'">'
+s.faces.map(n=>'<option'+(n==x.face?' selected':'')+'>'+n+'</option>').join('')+'</select><button class="g" onclick="showSound('+x.f+','+x.n+')">Show</button></div>'}
soundList.innerHTML=h}
const showSound=(f,n)=>{stopPreview();showFace($('sf'+f+'_'+n).value);hear(f,n)};
const saveSounds=async()=>{const b=new URLSearchParams();for(const e of document.querySelectorAll('#soundList select'))b.append('s'+e.id.slice(2),e.value);
const r=await post('/api/sounds','',b);sfMsg.textContent=r.ok?'Saved on Mochi!':'Failed'}
const resetSounds=async()=>{if(!confirm('Back to the default faces?'))return;await post('/api/sounds','&reset=1');loadSounds();sfMsg.textContent='Defaults restored'}
loadLyrics();Promise.all([loadSettings(),loadSounds()]).then(loadTriggers);
</script></body></html>)MOCHI";
#endif  // MOCHI_WIFI


// =================================================================
//  WIFI + APP API + OTA  (configure Mochi without a cable)
// =================================================================
// First boot (or when the saved WiFi is not found): Mochi creates the
// hotspot "Mochi-Setup" (password mochi1234). Connect your phone, open
// http://192.168.4.1 and enter your home WiFi. After that, open
// http://mochi.local (or the IP shown on the LCD) to control Mochi.
//
// The same HTTP API is used by the Flutter app. Every POST needs ?key=<PIN>.
//   GET  /api/status                    live status (JSON)
//   GET  /api/settings?key=             current settings (JSON)
//   POST /api/settings?key=             name, volume (0..30), sleepSec, autoSleep,
//                                       vibration, sound, lonelyMin (1..60), idleSounds, title1..3, newPin
//   POST /api/action?key=&do=           pet | love | excited | music | play(&slot=1..3)
//                                       | stop | wake | sleep | mood(&m=happy...)
//   POST /api/action?key=&do=sound      preview one sound + its face (&f=folder &n=file [&face=name])
//   GET  /api/triggers?key=             what Mochi does for tap1 tap2 tap3 long shake upside move
//                                       dark night bright lonely wake sleep start songend
//                                       (JSON: type default|mood|sound|song|none)
//   POST /api/triggers?key=             <id>=default | none | mood:<face> | sound:<folder>:<file> | song:<1..3>
//                                       <id>_vib=0|1|2  vibration: 0 Mochi decides, 1 always vibrate, 2 never
//                                       (e.g. tap3=song:1 , shake=mood:dizzy) ; reset=1
//   GET  /api/video?key=&slot=1..3      the saved .mvid video (for previewing on the phone)
//   GET  /api/sounds?key=               faces + the face chosen for every sound (JSON)
//   POST /api/sounds?key=               s<folder>_<file>=<face name> (e.g. s9_2=laugh), reset=1
//   GET  /api/lyrics?key=&slot=1..3     lyrics text: one "ms|line1|line2" per line
//   POST /api/lyrics?key=&slot=1..3     body = lyrics text   (/api/lyrics/reset to delete)
//   POST /api/video?key=&slot=1..3      multipart "file" = .mvid  (/api/video/delete)
//   POST /api/wifi                      ssid, pass  (then Mochi restarts)
//   POST /api/wifi/forget?key=          clear WiFi, restart into setup hotspot
//   POST /update?key=                   multipart firmware .bin (OTA update)
#if MOCHI_WIFI
WebServer server(80);
#endif
bool apMode = false;
String ipText = "";
File uploadFile;
bool uploadOk = false;
size_t uploadSize = 0;
bool firmwareOk = false;

void lcdBanner(const char* line1, const char* line2) {
  lcd.printRow(0, line1);
  lcd.printRow(1, line2);
  lcdForceRedraw = true;
}

String jsonEscape(const char* text) {
  String out;
  for (const char* c = text; *c; ++c) {
    if (*c == '"' || *c == '\\') out += '\\';
    if ((uint8_t)*c >= 32) out += *c;
  }
  return out;
}

// =================================================================
//  REQUEST HELPERS  (the same handlers answer WiFi *and* Bluetooth requests)
// =================================================================
bool bleReqActive = false;   // a request that came over Bluetooth is being answered
String bleArgs;              // its url-encoded arguments  (key=value&key2=value2)
String bleReqId;
String bleTxText;            // response being sent over Bluetooth: "id|status|payload\n"
size_t bleTxPos = 0;
File bleTxFile;              // optional raw bytes streamed after the header (video download)
size_t bleTxFileLeft = 0;

String urlDecode(const String& in) {
  String out;
  for (size_t i = 0; i < in.length(); ++i) {
    const char c = in[i];
    if (c == '+') {
      out += ' ';
    } else if (c == '%' && i + 2 < in.length() + 0 && isxdigit(in[i + 1]) && isxdigit(in[i + 2])) {
      const char hex[3] = {in[i + 1], in[i + 2], 0};
      out += (char)strtol(hex, nullptr, 16);
      i += 2;
    } else {
      out += c;
    }
  }
  return out;
}

String urlEncode(const String& in) {
  static const char* HEX_DIGITS = "0123456789ABCDEF";
  String out;
  for (size_t i = 0; i < in.length(); ++i) {
    const uint8_t c = (uint8_t)in[i];
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += (char)c;
    } else {
      out += '%';
      out += HEX_DIGITS[c >> 4];
      out += HEX_DIGITS[c & 15];
    }
  }
  return out;
}

String bleArg(const String& name, bool* found) {
  size_t pos = 0;
  while (pos <= bleArgs.length()) {
    int amp = bleArgs.indexOf('&', pos);
    if (amp < 0) amp = bleArgs.length();
    const String pair = bleArgs.substring(pos, amp);
    const int eq = pair.indexOf('=');
    if (eq > 0 && urlDecode(pair.substring(0, eq)) == name) {
      if (found) *found = true;
      return urlDecode(pair.substring(eq + 1));
    }
    pos = amp + 1;
  }
  if (found) *found = false;
  return "";
}

String argOf(const String& name) {
  if (bleReqActive) return bleArg(name, nullptr);
#if MOCHI_WIFI
  return server.arg(name);
#else
  return "";
#endif
}

bool hasArgOf(const String& name) {
  if (bleReqActive) {
    bool found = false;
    bleArg(name, &found);
    return found;
  }
#if MOCHI_WIFI
  return server.hasArg(name);
#else
  return false;
#endif
}

int bleLastCode = 0;
void bleRespond(int code, const String& payload) {
  bleLastCode = code;
  bleTxText = bleReqId + "|" + String(code) + "|" + payload + "\n";
  bleTxPos = 0;
}

void sendCors() {
#if MOCHI_WIFI
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
#endif
}

void sendJson(int code, const String& body) {
  if (bleReqActive) {
    bleRespond(code, body);
    return;
  }
  sendCors();
#if MOCHI_WIFI
  server.send(code, "application/json", body);
#endif
}

bool keyValid() { return apMode || argOf("key") == cfg.pin; }

// Security: 5 wrong PINs in a row lock every PIN-protected command for 60 s.
uint8_t badKeyCount = 0;
uint32_t keyLockUntil = 0;

bool requireKey() {
  if (pending(keyLockUntil)) {
    sendJson(429, "{\"ok\":false,\"error\":\"too many wrong PINs, wait 60 s\"}");
    return false;
  }
  if (keyValid()) {
    badKeyCount = 0;
    return true;
  }
  ++statAuthFail;
  if (++badKeyCount >= 5) {
    badKeyCount = 0;
    keyLockUntil = millis() + 60000;
    logEvent("SECURITY", "5 wrong PINs - locked for 60 s");
  } else {
    logEvent("SECURITY", "wrong PIN");
  }
  sendJson(403, "{\"ok\":false,\"error\":\"wrong PIN\"}");
  return false;
}

uint8_t slotArg() {
  const long slot = argOf("slot").toInt();
  return (slot >= 1 && slot <= SONG_SLOTS) ? (uint8_t)slot : 0;
}

bool videoExists(uint8_t slot) {
  char path[16];
  snprintf(path, sizeof(path), "/video%u.mvid", slot);
  return fsOk && LittleFS.exists(path);
}

const char* modeName() {
  return mode == MODE_MUSIC ? "music" : mode == MODE_SLEEP ? "sleep" : reactionActive() ? "reaction" : "normal";
}

void handleStatus() {
  String j = "{";
  j += "\"name\":\"" + jsonEscape(cfg.name) + "\"";
  j += ",\"mode\":\"" + String(modeName()) + "\"";
  j += ",\"expr\":\"" + String(EXPR_NAMES[currentExpression()]) + "\"";
  j += ",\"light\":" + String(lightPercent);
  j += ",\"night\":" + String(isNight ? "true" : "false");
  j += ",\"love\":" + String(affection);
  j += ",\"tilt\":" + String(tiltX, 2);
  j += ",\"touch\":" + String(touchDown ? "true" : "false");
  // Live sensor values for the app's sensor monitor
  j += ",\"lightRaw\":" + String(ldrRaw);
  j += ",\"ax\":" + String(fax, 2) + ",\"ay\":" + String(fay, 2) + ",\"az\":" + String(faz, 2);
  j += ",\"accel\":" + String(accelMag, 2) + ",\"gyro\":" + String(gyroNow, 2);
  j += ",\"upside\":" + String(upsideDown ? "true" : "false");
  j += ",\"mpuOk\":" + String(mpuOk ? "true" : "false") + ",\"lightOk\":" + String(ldrAtLimit ? "false" : "true");
  // Live outputs (actuators) for the app's monitor
  {
    auto lcdText = [](const char* t) {
      String out;
      for (const char* c = t; *c; ++c) {
        if (*c == '\x01') out += "\xE2\x99\xAA";       // music note
        else if (*c == '\x02') out += "\xE2\x99\xA5";  // heart
        else out += *c;
      }
      return out;
    };
    const bool playing = monFolder && (USE_BUSY_PIN ? digitalRead(PIN_DF_BUSY) == LOW : millis() - monPlayAt < 3000);
    char file[8];
    snprintf(file, sizeof(file), "%02u/%03u", (unsigned)monFolder, (unsigned)monFile);
    j += ",\"oled\":\"" + String(!oledOk ? "off" : mode == MODE_MUSIC ? (videoOpen ? "video" : "dance") : mode == MODE_SLEEP ? "sleep" : "face") + "\"";
    j += ",\"lcdOk\":" + String(lcdOk ? "true" : "false");
    j += ",\"lcd1\":\"" + jsonEscape(lcdText(lcd.shown[0]).c_str()) + "\"";
    j += ",\"lcd2\":\"" + jsonEscape(lcdText(lcd.shown[1]).c_str()) + "\"";
    j += ",\"audioPlaying\":" + String(playing ? "true" : "false");
    j += ",\"audioFile\":\"" + String(monFolder ? file : "") + "\"";
    j += ",\"volume\":" + String(monVolume) + ",\"soundOn\":" + String(cfg.sound ? "true" : "false");
    j += ",\"vib\":" + String(vibIsOn ? "true" : "false") + ",\"vibCount\":" + String(monVibCount);
    j += ",\"vibOn\":" + String(cfg.vibration ? "true" : "false");
  }
  j += ",\"df\":" + String(dfOk ? "true" : "false");
  j += ",\"song\":" + String(mode == MODE_MUSIC ? songIndex + 1 : 0);
  j += ",\"songTitle\":\"" + jsonEscape(mode == MODE_MUSIC ? songTitle(songIndex) : "") + "\"";
  j += ",\"songMs\":" + String(mode == MODE_MUSIC ? songElapsed() : 0);
  j += ",\"videos\":[";
  for (uint8_t s = 1; s <= SONG_SLOTS; ++s) j += String(s > 1 ? "," : "") + (videoExists(s) ? "true" : "false");
  j += "],\"fsFree\":" + String(fsOk ? (uint32_t)(LittleFS.totalBytes() - LittleFS.usedBytes()) : 0);
  j += ",\"ip\":\"" + ipText + "\"";
  j += ",\"ap\":" + String(apMode ? "true" : "false");
  j += ",\"version\":\"" + String(FW_VERSION) + "\"}";
  sendJson(200, j);
}

void handleGetSettings() {
  if (!requireKey()) return;
  String j = "{";
  j += "\"name\":\"" + jsonEscape(cfg.name) + "\"";
  j += ",\"volume\":" + String(cfg.volume);
  j += ",\"dayVol\":" + String(cfg.volume);  // older apps
  j += ",\"sleepSec\":" + String(cfg.sleepAfterS);
  j += ",\"autoSleep\":" + String(cfg.autoSleep ? "true" : "false");
  j += ",\"vibration\":" + String(cfg.vibration ? "true" : "false");
  j += ",\"sound\":" + String(cfg.sound ? "true" : "false");
  j += ",\"lonelyMin\":" + String(cfg.lonelyMin);
  j += ",\"idleSounds\":" + String(cfg.idleSounds ? "true" : "false");
  for (uint8_t i = 0; i < SONG_SLOTS; ++i) j += ",\"title" + String(i + 1) + "\":\"" + jsonEscape(songTitle(i)) + "\"";
  j += ",\"wifi\":\"" + jsonEscape(cfg.wifiSsid) + "\"}";
  sendJson(200, j);
}

bool argBool(const char* name, bool current) {
  if (!hasArgOf(name)) return current;
  const String v = argOf(name);
  return v == "1" || v == "true" || v == "on";
}

void handlePostSettings() {
  if (!requireKey()) return;
  const String oldName = cfg.name;
  if (hasArgOf("name")) {
    String n = argOf("name");
    n.trim();
    if (n.length()) n.toCharArray(cfg.name, sizeof(cfg.name));
  }
  const uint8_t oldVolume = cfg.volume;
  if (hasArgOf("volume")) cfg.volume = constrain(argOf("volume").toInt(), 0L, 30L);
  else if (hasArgOf("dayVol")) cfg.volume = constrain(argOf("dayVol").toInt(), 0L, 30L);  // older apps
  if (hasArgOf("sleepSec")) cfg.sleepAfterS = constrain(argOf("sleepSec").toInt(), 10L, 3600L);
  cfg.autoSleep = argBool("autoSleep", cfg.autoSleep);
  cfg.vibration = argBool("vibration", cfg.vibration);
  cfg.sound = argBool("sound", cfg.sound);
  if (hasArgOf("lonelyMin")) cfg.lonelyMin = constrain(argOf("lonelyMin").toInt(), 1L, 60L);
  cfg.idleSounds = argBool("idleSounds", cfg.idleSounds);
  for (uint8_t i = 0; i < SONG_SLOTS; ++i) {
    const String key = "title" + String(i + 1);
    if (hasArgOf(key)) argOf(key).toCharArray(cfg.songTitle[i], sizeof(cfg.songTitle[i]));
  }
  if (hasArgOf("newPin") && argOf("newPin").length() >= 4) {
    argOf("newPin").toCharArray(cfg.pin, sizeof(cfg.pin));
#if MOCHI_WIFI
    ArduinoOTA.setPassword(cfg.pin);
#endif
  }
  saveConfig();
  logEvent("CONFIG", "settings saved");
  setVolume(cfg.volume);
  if (cfg.volume != oldVolume && oldName == cfg.name) {
    // Let the new volume be heard right away.
    char line2[17];
    snprintf(line2, sizeof(line2), "%u / 30", (unsigned)cfg.volume);
    react(EX_HAPPY, 1500, "Volume", line2, TRK_HAPPY);
  }
  if (oldName != cfg.name) {
    // Show the new name on Mochi right away (LCD + happy face) and use it for Bluetooth.
    char line1[17];
    snprintf(line1, sizeof(line1), "Hi! I'm %s", cfg.name);
    react(EX_HAPPY, 3500, line1, "New name! " LCD_HEART, TRK_HAPPY, 1, 80);
    bleSetName();
    logEvent("CONFIG", "name changed to " + String(cfg.name));
  }
  sendJson(200, "{\"ok\":true}");
}

// ---- What Mochi does: GET lists, POST saves (<id>=default|none|mood:<face>|sound:<f>:<n>|song:<slot>) ----
void handleGetTriggers() {
  if (!requireKey()) return;
  String j = "{\"triggers\":[";
  for (uint8_t i = 0; i < TG_COUNT; ++i) {
    const TrigAction& a = cfg.trig[i];
    j += String(i ? "," : "") + "{\"id\":\"" + TRIG_NAMES[i] + "\",\"type\":\"" + ACT_NAMES[a.type] +
         "\",\"mood\":\"" + EXPR_NAMES[a.mood] + "\",\"f\":" + a.folder + ",\"n\":" + a.file +
         ",\"slot\":" + a.slot + ",\"vib\":" + cfg.trigVib[i] + "}";
  }
  j += "]}";
  sendJson(200, j);
}

void handlePostTriggers() {
  if (!requireKey()) return;
  if (argBool("reset", false)) defaultTriggers();
  for (uint8_t i = 0; i < TG_COUNT; ++i) {
    const String vibKey = String(TRIG_NAMES[i]) + "_vib";  // e.g. tap1_vib=1 (0 normal, 1 vibrate, 2 none)
    if (hasArgOf(vibKey)) {
      const long v = argOf(vibKey).toInt();
      if (v >= VIB_NORMAL && v <= VIB_OFF) cfg.trigVib[i] = (uint8_t)v;
    }
    if (!hasArgOf(TRIG_NAMES[i])) continue;
    const String v = argOf(TRIG_NAMES[i]);
    TrigAction a = cfg.trig[i];
    if (v == "default") {
      a.type = ACT_DEFAULT;
    } else if (v == "none") {
      a.type = ACT_NONE;
    } else if (v.startsWith("mood:")) {
      const String m = v.substring(5);
      for (uint8_t k = 0; k < EX_SLEEP; ++k)
        if (m == EXPR_NAMES[k]) { a.type = ACT_MOOD; a.mood = k; }
    } else if (v.startsWith("sound:")) {
      const int c = v.indexOf(':', 6);
      if (c > 0) {
        a.type = ACT_SOUND;
        a.folder = v.substring(6, c).toInt();
        a.file = v.substring(c + 1).toInt();
      }
    } else if (v.startsWith("song:")) {
      a.type = ACT_SONG;
      a.slot = v.substring(5).toInt();
    }
    if (validTrigger(a)) cfg.trig[i] = a;
  }
  saveConfig();
  sendJson(200, "{\"ok\":true}");
}

// Sends the saved video of a song to the phone, so the app can preview it before saving.
#if MOCHI_WIFI
void handleGetVideo() {
  if (!requireKey()) return;
  const uint8_t slot = slotArg();
  if (!slot || !videoExists(slot)) return sendJson(404, "{\"ok\":false,\"error\":\"no video\"}");
  char path[16];
  snprintf(path, sizeof(path), "/video%u.mvid", slot);
  File f = LittleFS.open(path, "r");
  sendCors();
  server.streamFile(f, "application/octet-stream");
  f.close();
}
#endif


// ---- Face for each sound: GET lists them, POST saves (s<folder>_<file>=<face name>) ----
void handleGetSounds() {
  if (!requireKey()) return;
  String j = "{\"faces\":[";
  for (uint8_t i = 0; i <= EX_WAKING; ++i) j += String(i ? ",\"" : "\"") + EXPR_NAMES[i] + "\"";
  j += "],\"sounds\":[";
  bool first = true;
  for (uint8_t f = 1; f <= SOUND_FACE_FOLDERS; ++f) {
    const uint8_t count = FILES_IN_FOLDER[f] < SOUND_FACE_FILES ? FILES_IN_FOLDER[f] : SOUND_FACE_FILES;
    for (uint8_t n = 1; n <= count; ++n) {
      const char* name = n <= 2 ? SOUND_NAMES[f][n - 1] : "sound";
      j += String(first ? "" : ",") + "{\"f\":" + f + ",\"n\":" + n + ",\"name\":\"" + jsonEscape(name) +
           "\",\"face\":\"" + EXPR_NAMES[cfg.soundFace[f][n - 1]] + "\"}";
      first = false;
    }
  }
  j += "]}";
  sendJson(200, j);
}

void handlePostSounds() {
  if (!requireKey()) return;
  if (argBool("reset", false)) defaultSoundFaces();
  for (uint8_t f = 1; f <= SOUND_FACE_FOLDERS; ++f) {
    for (uint8_t n = 1; n <= SOUND_FACE_FILES; ++n) {
      const String key = "s" + String(f) + "_" + String(n);
      if (!hasArgOf(key)) continue;
      const String v = argOf(key);
      for (uint8_t i = 0; i <= EX_WAKING; ++i) if (v == EXPR_NAMES[i]) cfg.soundFace[f][n - 1] = i;
    }
  }
  saveConfig();
  sendJson(200, "{\"ok\":true}");
}

void handleAction() {
  if (!requireKey()) return;
  const String what = argOf("do");
  lastActivity = millis();
  if (mode == MODE_SLEEP && what != "sleep") wakeUp(true);
  if (what == "pet") onSingleTap();
  else if (what == "love") onLongTouch();
  else if (what == "excited") onMultiTap();
  else if (what == "music") startMusic();
  else if (what == "play" && slotArg()) startMusic(slotArg() - 1);
  else if (what == "stop") { if (mode == MODE_MUSIC) finishMusic(true); }
  else if (what == "sleep") { if (mode == MODE_MUSIC) finishMusic(true); if (mode == MODE_NORMAL) enterSleep(); }
  else if (what == "mood") {
    const String m = argOf("m");
    for (uint8_t i = 0; i < EX_SLEEP; ++i) {
      if (m == EXPR_NAMES[i]) {
        react((Expr)i, 4000, nullptr, nullptr, soundForExpr((Expr)i));
        reactionExpr = (Expr)i;  // keep the face that was picked
      }
    }
  } else if (what == "sound") {
    // Preview one sound with its face (&f=folder &n=file, optional &face=name to try an unsaved face).
    const long f = argOf("f").toInt(), n = argOf("n").toInt();
    if (f < 1 || f > F_LAST || n < 1 || n > FILES_IN_FOLDER[f]) {
      sendJson(400, "{\"ok\":false,\"error\":\"bad sound\"}");
      return;
    }
    Expr face = faceForSound(f, n, EX_HAPPY);
    const String fn = argOf("face");
    for (uint8_t i = 0; i <= EX_WAKING; ++i) if (fn == EXPR_NAMES[i]) face = (Expr)i;
    if (mode == MODE_NORMAL) {
      react(face, 3000, nullptr, nullptr);
      pendingStop = false;
      pendingFolder = f;
      pendingFile = n;
      if (cfg.sound) startSoundFace(3000);
    }
  } else if (what != "wake") {
    sendJson(400, "{\"ok\":false,\"error\":\"unknown action\"}");
    return;
  }
  sendJson(200, "{\"ok\":true}");
}

void lyricsPath(uint8_t slot, char* path, size_t size) { snprintf(path, size, "/lyrics%u.txt", slot); }

void handleGetLyrics() {
  if (!requireKey()) return;
  const uint8_t slot = slotArg();
  if (!slot) return sendJson(400, "{\"ok\":false,\"error\":\"slot 1-3\"}");
  char path[16];
  lyricsPath(slot, path, sizeof(path));
  String text;
  if (fsOk && LittleFS.exists(path)) {
    File f = LittleFS.open(path, "r");
    text = f.readString();
    f.close();
  } else {
    const Song& s = PLAYLIST[slot - 1];
    for (uint8_t i = 0; i < s.lyricCount; ++i) {
      text += String(s.lyrics[i].atMs) + "|" + s.lyrics[i].line1 + "|" + s.lyrics[i].line2 + "\n";
    }
  }
  if (bleReqActive) {
    bleRespond(200, urlEncode(text));  // the app decodes it
    return;
  }
  sendCors();
#if MOCHI_WIFI
  server.send(200, "text/plain", text);
#endif
}

void handlePostLyrics() {
  if (!requireKey()) return;
  const uint8_t slot = slotArg();
  if (!slot || !fsOk) return sendJson(400, "{\"ok\":false,\"error\":\"slot 1-3 / no storage\"}");
  char path[16];
  lyricsPath(slot, path, sizeof(path));
  File f = LittleFS.open(path, "w");
  f.print(argOf("plain"));
  f.close();
  sendJson(200, "{\"ok\":true}");
}

void handleResetLyrics() {
  if (!requireKey()) return;
  char path[16];
  lyricsPath(slotArg(), path, sizeof(path));
  if (fsOk) LittleFS.remove(path);
  sendJson(200, "{\"ok\":true}");
}

#if MOCHI_WIFI
void handleVideoUpload() {
  HTTPUpload& up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    uploadSize = 0;
    uploadOk = keyValid() && fsOk && slotArg();
    if (uploadOk) {
      closeVideo();
      uploadFile = LittleFS.open("/upload.tmp", "w");
      uploadOk = (bool)uploadFile;
      lcdBanner("Receiving video", "Please wait...");
    }
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (uploadOk && uploadFile.write(up.buf, up.currentSize) != up.currentSize) uploadOk = false;
    uploadSize += up.currentSize;
  } else if (up.status == UPLOAD_FILE_END || up.status == UPLOAD_FILE_ABORTED) {
    if (uploadFile) uploadFile.close();
    if (up.status == UPLOAD_FILE_ABORTED) uploadOk = false;
  }
}

void handleVideoDone() {
  if (!keyValid()) return sendJson(403, "{\"ok\":false,\"error\":\"wrong PIN\"}");
  uint16_t frames = 0;
  if (uploadOk) {
    File f = LittleFS.open("/upload.tmp", "r");
    uint8_t h[16];
    uploadOk = f && f.read(h, 16) == 16 && memcmp(h, "MVID", 4) == 0 && h[5] > 0;
    if (uploadOk) {
      frames = h[6] | (h[7] << 8);
      uploadOk = frames > 0 && uploadSize == 16 + (size_t)frames * 1024;
    }
    if (f) f.close();
  }
  if (!uploadOk) {
    if (fsOk) LittleFS.remove("/upload.tmp");
    return sendJson(400, "{\"ok\":false,\"error\":\"invalid video file or not enough space\"}");
  }
  char path[16];
  snprintf(path, sizeof(path), "/video%u.mvid", slotArg());
  LittleFS.remove(path);
  LittleFS.rename("/upload.tmp", path);
  say("Video saved!", LCD_NOTE " Ready to play", 3000);
  sendJson(200, "{\"ok\":true,\"frames\":" + String(frames) + "}");
}
#endif


void handleVideoDelete() {
  if (!requireKey()) return;
  char path[16];
  snprintf(path, sizeof(path), "/video%u.mvid", slotArg());
  closeVideo();
  if (fsOk) LittleFS.remove(path);
  sendJson(200, "{\"ok\":true}");
}

#if MOCHI_WIFI
void handleWifiSave() {
  if (!requireKey()) return;
  if (!hasArgOf("ssid") || !argOf("ssid").length()) {
    return sendJson(400, "{\"ok\":false,\"error\":\"ssid required\"}");
  }
  argOf("ssid").toCharArray(cfg.wifiSsid, sizeof(cfg.wifiSsid));
  argOf("pass").toCharArray(cfg.wifiPass, sizeof(cfg.wifiPass));
  saveConfig();
  sendJson(200, "{\"ok\":true,\"restarting\":true}");
  lcdBanner("WiFi saved!", "Restarting...");
  delay(800);
  ESP.restart();
}

void handleWifiForget() {
  if (!requireKey()) return;
  cfg.wifiSsid[0] = cfg.wifiPass[0] = '\0';
  saveConfig();
  sendJson(200, "{\"ok\":true,\"restarting\":true}");
  delay(800);
  ESP.restart();
}

void handleFirmwareUpload() {
  HTTPUpload& up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    firmwareOk = keyValid();
    if (firmwareOk) {
      stopAudio();
      closeVideo();
      lcdBanner("Updating Mochi", "Do not unplug!");
      firmwareOk = Update.begin(UPDATE_SIZE_UNKNOWN);
    }
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (firmwareOk && Update.write(up.buf, up.currentSize) != up.currentSize) firmwareOk = false;
  } else if (up.status == UPLOAD_FILE_END) {
    if (firmwareOk) firmwareOk = Update.end(true);
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
    firmwareOk = false;
  }
}

void handleFirmwareDone() {
  if (!firmwareOk) {
    lcdForceRedraw = true;
    return sendJson(500, "{\"ok\":false,\"error\":\"update failed\"}");
  }
  sendJson(200, "{\"ok\":true,\"restarting\":true}");
  lcdBanner("Update done!", "Restarting...");
  delay(800);
  ESP.restart();
}

void handleRoot() {
  sendCors();
  server.send_P(200, "text/html", apMode ? SETUP_HTML : APP_HTML);
}

void handleNotFound() {
  if (server.method() == HTTP_OPTIONS) {
    sendCors();
    server.send(204);
    return;
  }
  // Captive-portal style: send phones to the setup page while in hotspot mode.
  if (apMode) return handleRoot();
  sendJson(404, "{\"ok\":false,\"error\":\"not found\"}");
}
#endif  // MOCHI_WIFI handlers


// =================================================================
//  BLUETOOTH (BLE)  - how the phone app configures Mochi
// =================================================================
// Nordic-UART style service. The app sends one request per line and Mochi answers with one line:
//   request :  <id>|<GET or POST>|<path>|<url-encoded args>\n      (same paths as the old WiFi API)
//   response:  <id>|<status>|<payload>\n   (payload = JSON; lyrics text is url-encoded)
//   GET /api/video answers "<id>|200|<size>\n" and then <size> raw bytes.
// Video upload: POST /api/video/begin (slot,size) -> write raw bytes to the VIDEO characteristic
//   -> POST /api/video/sync every 4096 bytes -> POST /api/video/end (slot,size).
// Every request except status/ping/stats needs key=<PIN>.
// Diagnostics: GET /api/stats (health, counters, response times), GET /api/log (event log, url-encoded),
//   POST /api/log/clear, POST /api/time (t=unix seconds from the phone).
#define BLE_SVC_UUID "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#define BLE_RX_UUID  "6e400002-b5a3-f393-e0a9-e50e24dcca9e"  // app -> Mochi  (requests)
#define BLE_TX_UUID  "6e400003-b5a3-f393-e0a9-e50e24dcca9e"  // Mochi -> app  (notify)
#define BLE_VID_UUID "6e400004-b5a3-f393-e0a9-e50e24dcca9e"  // app -> Mochi  (raw video bytes)

BLEServer* bleServer = nullptr;
BLECharacteristic* bleTx = nullptr;
bool bleConnected = false;
uint16_t bleChunk = 20;      // payload bytes per notification (MTU - 3)
uint32_t bleLastTx = 0;
String bleRxBuf;
String blePending;
volatile bool blePendingReady = false;
volatile bool bleJustConnected = false, bleJustDropped = false;  // logged from loop(), not the BLE task

// Video bytes arrive in the BLE task; loop() writes them to flash.
constexpr size_t VID_RING = 16384;
uint8_t vidRing[VID_RING];
volatile size_t vidHead = 0, vidTail = 0;
File bleUpFile;
bool bleUpOk = false;
size_t bleUpSize = 0, bleUpExpected = 0;
uint8_t bleUpSlot = 0;

void vidRingPush(const uint8_t* d, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    const size_t next = (vidHead + 1) % VID_RING;
    if (next == vidTail) {  // phone sent too fast
      bleUpOk = false;
      return;
    }
    vidRing[vidHead] = d[i];
    vidHead = next;
  }
}

void vidRingDrain() {
  uint8_t tmp[240];
  while (vidTail != vidHead) {
    size_t n = 0;
    while (vidTail != vidHead && n < sizeof(tmp)) {
      tmp[n++] = vidRing[vidTail];
      vidTail = (vidTail + 1) % VID_RING;
    }
    if (bleUpFile) {
      if (bleUpFile.write(tmp, n) != n) bleUpOk = false;
      bleUpSize += n;
    }
  }
}

void handleBleVideoBegin() {
  if (!requireKey()) return;
  const uint8_t slot = slotArg();
  const size_t size = (size_t)argOf("size").toInt();
  const size_t freeBytes = fsOk ? (LittleFS.totalBytes() - LittleFS.usedBytes()) : 0;
  if (!slot || !fsOk || size < 16 + 1024 || size + 8192 > freeBytes) {
    return sendJson(400, "{\"ok\":false,\"error\":\"not enough space\"}");
  }
  closeVideo();
  if (bleUpFile) bleUpFile.close();
  vidHead = 0;
  vidTail = 0;
  bleUpFile = LittleFS.open("/upload.tmp", "w");
  bleUpOk = (bool)bleUpFile;
  bleUpSize = 0;
  bleUpExpected = size;
  bleUpSlot = slot;
  lcdBanner("Receiving video", "Please wait...");
  sendJson(bleUpOk ? 200 : 500, bleUpOk ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"cannot write\"}");
}

void handleBleVideoSync() {
  if (!requireKey()) return;
  sendJson(bleUpOk ? 200 : 500, "{\"ok\":" + String(bleUpOk ? "true" : "false") + ",\"got\":" + String((unsigned)bleUpSize) + "}");
}

void handleBleVideoEnd() {
  if (!requireKey()) return;
  vidRingDrain();
  if (bleUpFile) bleUpFile.close();
  uint16_t frames = 0;
  bool ok = bleUpOk && bleUpSize == bleUpExpected;
  if (ok) {
    File f = LittleFS.open("/upload.tmp", "r");
    uint8_t h[16];
    ok = f && f.read(h, 16) == 16 && memcmp(h, "MVID", 4) == 0 && h[5] > 0;
    if (ok) {
      frames = h[6] | (h[7] << 8);
      ok = frames > 0 && bleUpSize == 16 + (size_t)frames * 1024;
    }
    if (f) f.close();
  }
  if (!ok) {
    if (fsOk) LittleFS.remove("/upload.tmp");
    logEvent("DATA", "video upload rejected (got " + String((unsigned)bleUpSize) + " of " + String((unsigned)bleUpExpected) + " bytes)");
    return sendJson(400, "{\"ok\":false,\"error\":\"invalid video file\"}");
  }
  logEvent("DATA", "video " + String(bleUpSlot) + " saved, " + String(frames) + " frames");
  char path[16];
  snprintf(path, sizeof(path), "/video%u.mvid", bleUpSlot);
  LittleFS.remove(path);
  LittleFS.rename("/upload.tmp", path);
  say("Video saved!", LCD_NOTE " Ready to play", 3000);
  sendJson(200, "{\"ok\":true,\"frames\":" + String(frames) + "}");
}

void handleBleGetVideo() {
  if (!requireKey()) return;
  const uint8_t slot = slotArg();
  if (!slot || !videoExists(slot)) return sendJson(404, "{\"ok\":false,\"error\":\"no video\"}");
  char path[16];
  snprintf(path, sizeof(path), "/video%u.mvid", slot);
  bleTxFile = LittleFS.open(path, "r");
  if (!bleTxFile) return sendJson(500, "{\"ok\":false,\"error\":\"cannot read\"}");
  bleTxFileLeft = bleTxFile.size();
  bleRespond(200, String((unsigned)bleTxFileLeft));  // header line, then the raw bytes follow
}

void handleStats() {
  String j = "{";
  j += "\"uptimeS\":" + String(millis() / 1000);
  j += ",\"boots\":" + String(bootCount);
  j += ",\"reset\":\"" + String(resetReasonText) + "\"";
  j += ",\"heap\":" + String(ESP.getFreeHeap());
  j += ",\"minHeap\":" + String(ESP.getMinFreeHeap());
  j += ",\"loopAvgUs\":" + String((uint32_t)statLoopAvgUs);
  j += ",\"loopMaxUs\":" + String(statLoopMaxShownUs);
  j += ",\"respOledMs\":" + String(respOledMs) + ",\"respOledMaxMs\":" + String(respOledMaxMs);
  j += ",\"respAudioMs\":" + String(respAudioMs) + ",\"respAudioMaxMs\":" + String(respAudioMaxMs);
  j += ",\"respCount\":" + String(respCount);
  j += ",\"reqOk\":" + String(statReqOk) + ",\"reqErr\":" + String(statReqErr);
  j += ",\"authFail\":" + String(statAuthFail);
  j += ",\"bleConnects\":" + String(statBleConnects) + ",\"bleDrops\":" + String(statBleDrops);
  j += ",\"locked\":" + String(pending(keyLockUntil) ? "true" : "false");
  j += ",\"timeSynced\":" + String(timeOffset ? "true" : "false");
  j += ",\"fsUsed\":" + String(fsOk ? (uint32_t)LittleFS.usedBytes() : 0);
  j += ",\"fsTotal\":" + String(fsOk ? (uint32_t)LittleFS.totalBytes() : 0);
  j += ",\"sensors\":{\"oled\":" + String(oledOk ? "true" : "false");
  j += ",\"lcd\":" + String(lcdOk ? "true" : "false");
  j += ",\"mpu\":" + String(mpuOk ? "true" : "false");
  j += ",\"dfplayer\":" + String(dfOk ? "true" : "false");
  j += ",\"storage\":" + String(fsOk ? "true" : "false");
  j += ",\"lightOk\":" + String(ldrAtLimit ? "false" : "true");
  j += ",\"lightRaw\":" + String(ldrRaw) + "}}";
  sendJson(200, j);
}

// The newest part of the event log (url-encoded text, like lyrics).
void handleGetLog() {
  if (!requireKey()) return;
  String text;
  if (fsOk && LittleFS.exists("/log.csv")) {
    File f = LittleFS.open("/log.csv", "r");
    const size_t size = f.size();
    const size_t want = 6000;
    if (size > want) {
      f.seek(size - want);
      f.readStringUntil('\n');  // start at a full line
    }
    text = f.readString();
    f.close();
  }
  sendJson(200, urlEncode(text));
}

void handleClearLog() {
  if (!requireKey()) return;
  if (fsOk) {
    LittleFS.remove("/log.csv");
    LittleFS.remove("/log.old.csv");
  }
  logEvent("CONFIG", "log cleared");
  sendJson(200, "{\"ok\":true}");
}

// The app sends the phone's clock so log lines get real dates (Mochi has no clock chip).
void handleSetTime() {
  if (!requireKey()) return;
  const uint32_t t = (uint32_t)argOf("t").toInt();
  if (t < 1700000000UL) return sendJson(400, "{\"ok\":false,\"error\":\"bad time\"}");
  timeOffset = t - millis() / 1000;
  logEvent("TIME", "clock synced from the app");
  sendJson(200, "{\"ok\":true}");
}

bool bleTxBusy() { return bleTxPos < bleTxText.length() || bleTxFileLeft > 0; }

void handleBleLine(const String& line) {
  const int a = line.indexOf('|');
  const int b = a < 0 ? -1 : line.indexOf('|', a + 1);
  if (a < 0 || b < 0) return;
  const int c = line.indexOf('|', b + 1);
  bleReqId = line.substring(0, a);
  const String method = line.substring(a + 1, b);
  const String path = c < 0 ? line.substring(b + 1) : line.substring(b + 1, c);
  bleArgs = c < 0 ? "" : line.substring(c + 1);
  bleTxText = "";
  bleTxPos = 0;
  bleTxFileLeft = 0;
  if (bleTxFile) bleTxFile.close();
  const bool isGet = method == "GET";

  bleReqActive = true;
  if (path == "/api/ping") sendJson(200, "{\"ok\":true,\"name\":\"" + jsonEscape(cfg.name) + "\",\"version\":\"" + String(FW_VERSION) + "\"}");
  else if (path == "/api/status") handleStatus();
  else if (path == "/api/settings") { if (isGet) handleGetSettings(); else handlePostSettings(); }
  else if (path == "/api/action") handleAction();
  else if (path == "/api/triggers") { if (isGet) handleGetTriggers(); else handlePostTriggers(); }
  else if (path == "/api/sounds") { if (isGet) handleGetSounds(); else handlePostSounds(); }
  else if (path == "/api/lyrics") { if (isGet) handleGetLyrics(); else handlePostLyrics(); }
  else if (path == "/api/lyrics/reset") handleResetLyrics();
  else if (path == "/api/video") handleBleGetVideo();
  else if (path == "/api/video/begin") handleBleVideoBegin();
  else if (path == "/api/video/sync") handleBleVideoSync();
  else if (path == "/api/video/end") handleBleVideoEnd();
  else if (path == "/api/video/delete") handleVideoDelete();
  else if (path == "/api/stats") handleStats();
  else if (path == "/api/log") handleGetLog();
  else if (path == "/api/log/clear") handleClearLog();
  else if (path == "/api/time") handleSetTime();
  else sendJson(404, "{\"ok\":false,\"error\":\"not found\"}");
  bleReqActive = false;
  if (bleLastCode >= 400) ++statReqErr; else ++statReqOk;
}

class MochiServerCb : public BLEServerCallbacks {
  void onConnect(BLEServer*) {
    bleConnected = true;
    bleJustConnected = true;
    bleChunk = 20;  // until the phone negotiates a bigger MTU
    Serial.println("BLE: phone connected");
  }
  void onDisconnect(BLEServer*) {
    bleConnected = false;
    bleJustDropped = true;
    bleRxBuf = "";
    blePendingReady = false;
    bleTxPos = bleTxText.length();
    bleTxFileLeft = 0;
    BLEDevice::startAdvertising();  // be visible again
    Serial.println("BLE: phone disconnected");
  }
  void onMtuChanged(BLEServer*, esp_ble_gatts_cb_param_t* param) {
    uint16_t mtu = param->mtu.mtu;
    if (mtu > 247) mtu = 247;
    bleChunk = mtu > 23 ? mtu - 3 : 20;
    Serial.printf("BLE: MTU %u\n", (unsigned)mtu);
  }
};

class MochiRxCb : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) {
    const uint8_t* d = c->getData();
    const size_t n = c->getLength();
    for (size_t i = 0; i < n; ++i) {
      const char ch = (char)d[i];
      if (ch == '\n') {
        if (!blePendingReady) {
          blePending = bleRxBuf;
          blePendingReady = true;
        }
        bleRxBuf = "";
      } else if (bleRxBuf.length() < 8000) {
        bleRxBuf += ch;
      }
    }
  }
};

class MochiVidCb : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) { vidRingPush(c->getData(), c->getLength()); }
};

// Bluetooth name = "Mochi-<name>". Phones see the new name after the next scan.
void bleSetName() {
  if (!ENABLE_BLE || !bleServer) return;
  char devName[24];
  snprintf(devName, sizeof(devName), "Mochi-%s", cfg.name);
  esp_ble_gap_set_device_name(devName);
  if (!bleConnected) {  // while connected it is applied when advertising starts again
    BLEDevice::stopAdvertising();
    BLEDevice::startAdvertising();
  }
}

void setupBle() {
  if (!ENABLE_BLE) return;
  char devName[24];
  snprintf(devName, sizeof(devName), "Mochi-%s", cfg.name);
  BLEDevice::init(devName);
  BLEDevice::setMTU(247);
  bleServer = BLEDevice::createServer();
  bleServer->setCallbacks(new MochiServerCb());
  BLEService* svc = bleServer->createService(BLE_SVC_UUID);

  bleTx = svc->createCharacteristic(BLE_TX_UUID, BLECharacteristic::PROPERTY_NOTIFY);
  bleTx->addDescriptor(new BLE2902());

  BLECharacteristic* rx = svc->createCharacteristic(BLE_RX_UUID, BLECharacteristic::PROPERTY_WRITE);
  rx->setCallbacks(new MochiRxCb());

  BLECharacteristic* vid = svc->createCharacteristic(BLE_VID_UUID, BLECharacteristic::PROPERTY_WRITE);
  vid->setCallbacks(new MochiVidCb());

  svc->start();
  BLEAdvertising* adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(BLE_SVC_UUID);
  adv->setScanResponse(true);
  BLEDevice::startAdvertising();
  Serial.printf("BLE: advertising as \"%s\"\n", devName);
  lcdBanner("Bluetooth ready", cfg.name);
}

void pumpBleTx() {
  if (!bleConnected || !bleTx || !bleTxBusy()) return;
  const uint32_t now = millis();
  if (now - bleLastTx < (bleTxFileLeft ? 15u : 10u)) return;  // pace it so the phone is not flooded
  bleLastTx = now;
  uint8_t buf[244];
  size_t n = 0;
  const size_t room = bleChunk < sizeof(buf) ? bleChunk : sizeof(buf);
  while (n < room && bleTxPos < bleTxText.length()) buf[n++] = (uint8_t)bleTxText[bleTxPos++];
  if (bleTxPos >= bleTxText.length() && n < room && bleTxFileLeft > 0) {
    size_t want = room - n;
    if (want > bleTxFileLeft) want = bleTxFileLeft;
    const size_t got = bleTxFile ? bleTxFile.read(buf + n, want) : 0;
    n += got;
    bleTxFileLeft = got == 0 ? 0 : bleTxFileLeft - got;
    if (bleTxFileLeft == 0 && bleTxFile) bleTxFile.close();
  }
  if (n) {
    bleTx->setValue(buf, n);
    bleTx->notify();
  }
}

void updateBle() {
  if (!ENABLE_BLE) return;
  if (bleJustConnected) {
    bleJustConnected = false;
    ++statBleConnects;
    logEvent("BLE", "phone connected");
  }
  if (bleJustDropped) {
    bleJustDropped = false;
    ++statBleDrops;
    logEvent("BLE", "phone disconnected - advertising again");
  }
  vidRingDrain();
  if (blePendingReady && !bleTxBusy()) {
    const String line = blePending;
    blePendingReady = false;
    handleBleLine(line);
  }
  pumpBleTx();
}

void setupNetwork() {
#if MOCHI_WIFI
  WiFi.persistent(false);
  WiFi.setHostname("mochi");
  if (cfg.wifiSsid[0]) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(cfg.wifiSsid, cfg.wifiPass);
    lcdBanner("Connecting WiFi", cfg.wifiSsid);
    const uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 12000) delay(200);
  }
  if (WiFi.status() == WL_CONNECTED) {
    apMode = false;
    WiFi.setAutoReconnect(true);
    ipText = WiFi.localIP().toString();
  } else {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_AP);
    WiFi.softAP(SETUP_AP_NAME, SETUP_AP_PASS);
    apMode = true;
    ipText = WiFi.softAPIP().toString();
  }
  if (MDNS.begin("mochi")) MDNS.addService("http", "tcp", 80);

  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/settings", HTTP_GET, handleGetSettings);
  server.on("/api/settings", HTTP_POST, handlePostSettings);
  server.on("/api/action", HTTP_POST, handleAction);
  server.on("/api/triggers", HTTP_GET, handleGetTriggers);
  server.on("/api/triggers", HTTP_POST, handlePostTriggers);
  server.on("/api/video", HTTP_GET, handleGetVideo);
  server.on("/api/sounds", HTTP_GET, handleGetSounds);
  server.on("/api/sounds", HTTP_POST, handlePostSounds);
  server.on("/api/lyrics", HTTP_GET, handleGetLyrics);
  server.on("/api/lyrics", HTTP_POST, handlePostLyrics);
  server.on("/api/lyrics/reset", HTTP_POST, handleResetLyrics);
  server.on("/api/video", HTTP_POST, handleVideoDone, handleVideoUpload);
  server.on("/api/video/delete", HTTP_POST, handleVideoDelete);
  server.on("/api/wifi", HTTP_POST, handleWifiSave);
  server.on("/api/wifi/forget", HTTP_POST, handleWifiForget);
  server.on("/update", HTTP_POST, handleFirmwareDone, handleFirmwareUpload);
  server.onNotFound(handleNotFound);
  server.begin();

  if (!apMode) {
    // Arduino IDE OTA: Tools > Port > "mochi at <ip>", password = PIN.
    ArduinoOTA.setHostname("mochi");
    ArduinoOTA.setPassword(cfg.pin);
    ArduinoOTA.onStart([]() {
      stopAudio();
      closeVideo();
      lcdBanner("Updating Mochi", "Do not unplug!");
    });
    ArduinoOTA.begin();
  }

  Serial.printf("WiFi: %s  ->  http://%s  (http://mochi.local)\n",
                apMode ? "SETUP HOTSPOT \"Mochi-Setup\" (pass mochi1234)" : cfg.wifiSsid, ipText.c_str());
  if (apMode) lcdBanner("WiFi: Mochi-Setup", "192.168.4.1");
  else lcdBanner("mochi.local", ipText.c_str());
  delay(2500);
#endif
}

void updateNetwork() {
#if MOCHI_WIFI
  if (!ENABLE_WIFI) return;
  server.handleClient();
  if (!apMode) ArduinoOTA.handle();
#endif
}

// =================================================================
//  SETUP + LOOP
// =================================================================
// Starts (or restarts) the OLED. Some modules need a little time after power-up,
// so setup() waits first and loop() keeps retrying until the OLED answers.
bool startOled() {
  if (!oled.begin(OLED_ADDRESS, true)) return false;
  oled.setContrast(0xFF);
  oled.clearDisplay();
  oled.setTextColor(WHITE);
  oled.setTextSize(2);
  const int nameW = (int)strlen(cfg.name) * 12;  // 12 px per letter at size 2
  oled.setCursor(nameW < 128 ? (128 - nameW) / 2 : 0, 18);
  oled.print(cfg.name);
  oled.setTextSize(1);
  oled.setCursor(28, 44);
  oled.print("waking up...");
  oled.display();
  return true;
}

void retryOled() {
  static uint32_t lastTry = 0;
  if (oledOk || millis() - lastTry < 3000) return;
  lastTry = millis();
  oledOk = startOled();
  Serial.println(oledOk ? "OLED SH1106 OK (after retry)" : "OLED still not answering, retrying...");
  if (oledOk) logEvent("RECOVER", "OLED answering again");
}

bool startMpu() {
  if (!mpu.begin(0x68, &Wire)) return false;
  mpu.setAccelerometerRange(MPU6050_RANGE_8_G);
  mpu.setGyroRange(MPU6050_RANGE_500_DEG);
  mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);
  return true;
}

// Recovery: if the MPU6050 is missing or failed, try again every 5 s.
void retryMpu() {
  static uint32_t lastTry = 0;
  if (mpuOk || millis() - lastTry < 5000) return;
  lastTry = millis();
  mpuOk = startMpu();
  if (mpuOk) logEvent("RECOVER", "MPU6050 answering again");
}

// Data validation for the light sensor: stuck at the very end of its range for 60 s.
void checkLdr() {
  static uint32_t limitSince = 0;
  const bool atLimit = ldrRaw <= 5 || ldrRaw >= 4090;
  if (!atLimit) {
    limitSince = 0;
    if (ldrAtLimit) {
      ldrAtLimit = false;
      logEvent("RECOVER", "light sensor back in normal range");
    }
    return;
  }
  if (!limitSince) limitSince = millis();
  if (!ldrAtLimit && millis() - limitSince > 60000) {
    ldrAtLimit = true;
    logEvent("WARN", "light sensor at its limit for 60 s (raw " + String(ldrRaw) + ") - check wiring if the room is not fully dark/bright");
  }
}

void scanI2c() {
  Serial.print("I2C devices:");
  uint8_t count = 0;
  for (uint8_t a = 1; a < 127; ++a) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      Serial.printf(" 0x%02X", a);
      ++count;
    }
  }
  if (!count) Serial.print(" none");
  Serial.println("\nExpected: OLED 0x3C, MPU6050 0x68, LCD 0x27 or 0x3F");
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== Mochi Virtual Pet ===");
  loadConfig();
  Serial.println("[boot] storage...");
  fsOk = LittleFS.begin(true);  // formats on first use
  Serial.printf("Storage: %s\n", fsOk ? "OK" : "not available (lyrics/videos from the app disabled)");
  prefs.begin("mochi", false);
  bootCount = prefs.getUInt("boots", 0) + 1;
  prefs.putUInt("boots", bootCount);
  prefs.end();
  resetReasonText = resetReasonName(esp_reset_reason());
  logEvent("BOOT", String("firmware ") + FW_VERSION + " reset=" + resetReasonText);

  pinMode(PIN_TOUCH, INPUT_PULLDOWN);  // keeps the pin LOW if the TTP223 is not connected yet
  pinMode(PIN_LDR, INPUT);
  pinMode(PIN_DF_BUSY, INPUT_PULLUP);
  pinMode(PIN_VIBRATION, OUTPUT);
  digitalWrite(PIN_VIBRATION, LOW);
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_LDR, ADC_11db);

  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(I2C_CLOCK_HZ);
  delay(100);
  Serial.println("[boot] i2c scan...");
  scanI2c();
  Serial.println("[boot] oled...");

  delay(300);  // give the OLED time to power up before talking to it
  oledOk = startOled();
  for (uint8_t attempt = 0; !oledOk && attempt < 3; ++attempt) {
    delay(300);
    oledOk = startOled();
  }
  Serial.println(oledOk ? "OLED SH1106 OK"
                        : "ERROR: SH1106 OLED not found at boot. Will keep retrying (try OLED_ADDRESS 0x3D)");

  lcdOk = lcd.begin();
  if (lcdOk) {
    lcd.createChar(1, GLYPH_NOTE);
    lcd.createChar(2, GLYPH_HEART);
    char hello[17];
    snprintf(hello, sizeof(hello), "Hello! I'm %s", cfg.name);
    lcd.printRow(0, hello);
    lcd.printRow(1, "Waking up...");
    Serial.printf("LCD OK at 0x%02X\n", lcd.address());
  } else {
    Serial.println("ERROR: I2C LCD not found at 0x27 / 0x3F");
  }

  Serial.println("[boot] mpu6050...");
  mpuOk = startMpu();
  if (mpuOk) {
    Serial.println("MPU6050 OK");
  } else {
    Serial.println("ERROR: MPU6050 not found at 0x68");
  }

  randomSeed(analogRead(PIN_LDR) ^ micros());

  Serial.println("[boot] light sensor...");
  // Prime the light reading so Mochi does not react to its own startup.
  for (uint8_t i = 0; i < 20; ++i) {
    updateLight();
    delay(55);
  }
  isNight = lightPercent < NIGHT_BELOW_PERCENT;

  Serial.println("[boot] dfplayer...");
  // The DFPlayer needs about 2 s after power-up. Same steps as the working test:
  // select the microSD card, then set the volume.
  dfSerial.begin(9600, SERIAL_8N1, PIN_DF_RX, PIN_DF_TX);
  delay(500);
  dfSend(0x09, 2);                         // select microSD card
  for (uint32_t t = millis(); millis() - t < 500;) dfPoll();
  dfSend(0x06, cfg.volume);
  monVolume = cfg.volume;
  for (uint32_t t = millis(); millis() - t < 400;) dfPoll();
  lastDfCommand = millis();
  Serial.println(dfOk ? "DFPlayer OK (answered)"
                      : "DFPlayer did not answer (it may still play: check the speaker)");
  logEvent(oledOk ? "CHECK" : "FAULT", oledOk ? "OLED ok" : "OLED not found");
  logEvent(lcdOk ? "CHECK" : "FAULT", lcdOk ? "LCD ok" : "LCD not found");
  logEvent(mpuOk ? "CHECK" : "FAULT", mpuOk ? "MPU6050 ok" : "MPU6050 not found");
  logEvent(dfOk ? "CHECK" : "WARN", dfOk ? "DFPlayer ok" : "DFPlayer did not answer");
  logEvent("CHECK", "light " + String(lightPercent) + "% raw " + String(ldrRaw));

  Serial.println("[boot] network...");
  if (ENABLE_WIFI) setupNetwork();
  else Serial.println("WiFi disabled (ENABLE_WIFI = false)");
  Serial.println("[boot] bluetooth...");
  setupBle();
  lcdForceRedraw = true;

  const uint32_t now = millis();
  lastActivity = lastPet = lastAffectionDecay = now;
  nextIdleEvent = now + 20000;
  delay(600);

  Serial.println("[boot] done, entering loop");
  playTrack(TRK_STARTUP);
  vibrate(1, 120);
  greetAfterWake = true;
  greetIsStartup = true;
  react(EX_WAKING, 1100, "*yawn*", "Huh...?");
  // The greeting waits until the startup sound has ended (it would cut it off).
  if (cfg.sound) {
    const uint32_t startupMs = SOUND_START_MS + soundLengthMs(F_SYSTEM, 1);
    if (startupMs > 1100) reactionUntil = millis() + startupMs;
  }
}

void loop() {
  const uint32_t loopStart = micros();
  retryOled();
  retryMpu();
  checkLdr();
  updateNetwork();
  updateBle();
  updateTouch();
  updateLight();
  updateMotion();
  updateMusic();
  updateBehavior();
  updateSleepScreens();
  updateAudio();
  updateSoundFace();
  updateVibration();
  renderOled();
  updateLcd();

  {
    const uint32_t us = micros() - loopStart;
    statLoopAvgUs += (us - statLoopAvgUs) * 0.01f;
    if (us > statLoopMaxUs) statLoopMaxUs = us;
    static uint32_t window = 0;
    if (millis() - window >= 10000) {  // max over the last 10 s
      window = millis();
      statLoopMaxShownUs = statLoopMaxUs;
      statLoopMaxUs = 0;
    }
  }

  static uint32_t lastLog = 0;
  if (millis() - lastLog >= 2000) {
    lastLog = millis();
    const char* modeName = mode == MODE_MUSIC ? "MUSIC" : mode == MODE_SLEEP ? "SLEEP"
                         : reactionActive() ? "REACTION" : "NORMAL";
    Serial.printf("mode=%s expr=%s light=%u%% (%s, raw %u) tilt=%.2f |a|=%.1f love=%d touch=%d\n",
                  modeName, EXPR_NAMES[currentExpression()], lightPercent, isNight ? "night" : "day",
                  ldrRaw, tiltX, accelMag, affection, touchDown);
  }
}