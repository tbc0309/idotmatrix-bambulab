#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "IDotMatrixHardwareConfig.h"
#include "IDotMatrixProtocolGuards.h"
#include "IDotMatrixBuzzerPolicy.h"
#include "IDotMatrixAudioOutput.h"
#include "IDotMatrixOta.h"
#include "IDotMatrixMemoryTelemetry.h"
#if IDOTMATRIX_BUZZER_TYPE == IDOTMATRIX_BUZZER_PASSIVE
  #include <esp32-hal-ledc.h>
#endif
#if IDOTMATRIX_ORIENTATION_SENSOR
  #include "IDotMatrixOrientation.h"
#endif
#if IDOTMATRIX_RTC_AVAILABLE
  #include "IDotMatrixRtc.h"
#endif

// ======================================================
// FIRMWARE RELEASE / BUILD ID
// FW_RELEASE identifies the public project release.
// FW_BUILD is an internal diagnostic identifier and is not part of the public release identity.
// ======================================================
#define FW_RELEASE "0.6.0"
#define FW_RELEASE_MAJOR 0
#define FW_RELEASE_MINOR 6
#define FW_BUILD 223

#define IDOT_STRINGIFY_INNER(x) #x
#define IDOT_STRINGIFY(x) IDOT_STRINGIFY_INNER(x)
static const char FW_SIGNATURE[] __attribute__((used)) = "IDOTMATRIX_FW=" FW_RELEASE;
#define PNG_DIAG_SERIAL 0
#define TEXT_PROTOCOL_DEBUG 0
#define BULK_PROTOCOL_DEBUG 0
#define GRAFFITI_PROTOCOL_DEBUG 0  // Release default: protocol tracing disabled; enable only for targeted Graffiti diagnostics.
#define PRESET_PROTOCOL_DEBUG 0  // Set to 1 only for Preset/Default protocol tracing (volatile slots 14..19).
#define CAROUSEL_PROTOCOL_DEBUG 0  // Set to 1 only for Device Assets protocol tracing.
#define DEVICE_INFO_PROTOCOL_DEBUG 0  // Set to 1 while studying MCU-version encoding in the 9-byte Device Info response.
#define CAROUSEL_SLOT_COUNT 12
#define CAROUSEL_DEFAULT_DWELL_SEC 5U
// Device Assets has no observed explicit end-of-bank frame. Real official-app
// uploads can pause for several seconds between items, so keep the replacement
// transaction open long enough to bridge the longest qualified inter-asset gap.
// This timeout is emulator policy, not a claimed wire-protocol field.
#define CAROUSEL_UPLOAD_SETTLE_MS 8000UL
#define PRESET_SLOT_BASE 14U
#define PRESET_SLOT_COUNT 6U
#define PRESET_DWELL_MS 3000UL

// Waveshare media policy: bounded PSRAM staging/cache/prefetch is best-effort;
// LittleFS remains authoritative. Device Assets type 2 is a static-image
// container: exact logical RGB24 is accepted directly, while app-observed PNG
// payloads are decoded on loopTask. Carousel and Preset publication remain
// transactional and filesystem-heavy work stays out of the BLE host callback.
#ifndef IDOTMATRIX_GIF_PSRAM_STAGE_MAX_BYTES
  #define IDOTMATRIX_GIF_PSRAM_STAGE_MAX_BYTES 0UL
#endif
#ifndef IDOTMATRIX_GIF_PSRAM_RESERVE_BYTES
  #define IDOTMATRIX_GIF_PSRAM_RESERVE_BYTES 0UL
#endif
#ifndef IDOTMATRIX_GIF_PSRAM_CACHE_MAX_BYTES
  #define IDOTMATRIX_GIF_PSRAM_CACHE_MAX_BYTES 0UL
#endif
#ifndef IDOTMATRIX_GIF_PSRAM_CACHE_ENTRY_MAX_BYTES
  #define IDOTMATRIX_GIF_PSRAM_CACHE_ENTRY_MAX_BYTES 0UL
#endif
#ifndef IDOTMATRIX_GIF_PSRAM_CACHE_MAX_ENTRIES
  #define IDOTMATRIX_GIF_PSRAM_CACHE_MAX_ENTRIES 0
#endif
#ifndef IDOTMATRIX_CAROUSEL_GIF_PREFETCH
  #define IDOTMATRIX_CAROUSEL_GIF_PREFETCH 0
#endif
#ifndef IDOTMATRIX_CAROUSEL_GIF_PREFETCH_DELAY_MS
  #define IDOTMATRIX_CAROUSEL_GIF_PREFETCH_DELAY_MS 250UL
#endif
#ifndef IDOTMATRIX_CAROUSEL_GIF_PREFETCH_CHUNK_BYTES
  #define IDOTMATRIX_CAROUSEL_GIF_PREFETCH_CHUNK_BYTES 4096U
#endif

// Optional hardware backends are resolved by IDotMatrixHardwareConfig.h.
struct ScheduleActivity;
struct AlarmSlot;

// ======================================================
// GENERIC REPOSITORY HARDWARE DEFAULTS
// ======================================================
#define DEBUG_SERIAL        1
#ifndef DEBUG_SERIAL_VERBOSE
#define DEBUG_SERIAL_VERBOSE 0
#endif

// Try a normal LittleFS mount first; format and retry only after mount failure.
#define LITTLEFS_FORMAT_ON_MOUNT_FAIL 1

// WS2812/FastLED backend pin. Ignored by HUB75 builds.
#ifndef MATRIX_PIN
#define MATRIX_PIN           4
#endif

// Optional external status LED. -1 disables it.
#define STATUS_LED_PIN      -1

// Optional SSD1306 diagnostic display. Disabled by default.
#define OLED_STATUS_ENABLED 0
#define OLED_SDA            4
#define OLED_SCL            15
#define OLED_RST            16
#define OLED_ROTATION       U8G2_R0
#define OLED_UNKNOWN_ALERT_MS 8000UL
#define OLED_UNKNOWN_BYTES    12

// Last unhandled FA02 command: kept independently from Serial so the
// on-board OLED can act as the protocol diagnostic console.
bool unknownCommandActive = false;
uint32_t unknownCommandAt = 0;
uint32_t unknownCommandCount = 0;
uint16_t unknownCommandLen = 0;
uint8_t unknownCommandData[OLED_UNKNOWN_BYTES] = {0};
uint8_t unknownCommandStored = 0;

#if IDOTMATRIX_USE_NIMBLE
  #include <NimBLEDevice.h>
  #include <string>
#else
  #include <BLEDevice.h>
  #include <BLEServer.h>
  #include <BLEUtils.h>
#endif
#include <FastLED.h>
#include <AnimatedGIF.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <esp_partition.h>
#if OLED_STATUS_ENABLED
  #include <U8g2lib.h>
  // Configuration identical to the DollaTek sketch already verified on hardware.
  U8G2_SSD1306_128X64_NONAME_F_SW_I2C statusOLED(OLED_ROTATION, OLED_SCL, OLED_SDA, OLED_RST);
  bool statusOLEDReady=false;
#endif

// Schedule PNG: use the miniz inflater already provided by the ESP32 ROM/SDK,
// avoiding an additional external PNG library.
#if __has_include(<rom/miniz.h>)
  #include <rom/miniz.h>
#elif __has_include(<miniz.h>)
  #include <miniz.h>
#else
  #error "miniz header not found: required for Schedule PNG decoding"
#endif
#if IDOTMATRIX_ORIENTATION_SENSOR || IDOTMATRIX_RTC_AVAILABLE || IDOTMATRIX_AUDIO_CODEC_AVAILABLE
  #include <Wire.h>
#endif
#if IDOTMATRIX_RTC_AVAILABLE
  IDotMatrixRtc rtc;
  bool rtcReady = false;
  bool rtcTimeValid = false;
  uint32_t rtcRetryNextAt = 0;
#endif


bool littleFsReady = false;

#if DEBUG_SERIAL
  #define DBG_BEGIN(x)       Serial.begin(x)
  #define DBG_PRINT(x)       Serial.print(x)
  #define DBG_PRINTLN(x)     Serial.println(x)
#else
  #define DBG_BEGIN(x)       do {} while (0)
  #define DBG_PRINT(x)       do {} while (0)
  #define DBG_PRINTLN(x)     do {} while (0)
#endif

#if PNG_DIAG_SERIAL
  #define PDBG_BEGIN() Serial.begin(115200)
  #define PDBG(x) Serial.print(x)
  #define PDBGLN(x) Serial.println(x)
#else
  #define PDBG_BEGIN() do {} while (0)
  #define PDBG(x) do {} while (0)
  #define PDBGLN(x) do {} while (0)
#endif

#if defined(IDOTMATRIX_BOARD_CLOCKWISE)
#include <esp_mac.h>
static char DEVICE_NAME[] = "IDM_64*64_4C42";
#else
#define DEVICE_NAME "IDM-858931"
#endif

// -----------------------------------------------------------------------------
// Display architecture
//
// LOGICAL resolution is the iDotMatrix model exposed to the official app.
// PHYSICAL resolution is the actually connected display. They are independent.
// The final output stage automatically performs 1:1 copy, nearest-neighbour
// upscaling or box-average downscaling for every 16/32/64 combination.
// -----------------------------------------------------------------------------
#define DISPLAY_BACKEND_WS2812  0
#define DISPLAY_BACKEND_HUB75   1

// These defaults remain Arduino-IDE friendly, while PlatformIO environments
// can override them with build_flags without editing this file.
#ifndef DISPLAY_BACKEND
#define DISPLAY_BACKEND DISPLAY_BACKEND_HUB75
#endif

// iDotMatrix logical profile: 1=16x16, 3=32x32, 4=64x64.
#ifndef IDOTMATRIX_SCREEN_TYPE
#define IDOTMATRIX_SCREEN_TYPE  4
#endif

#if IDOTMATRIX_SCREEN_TYPE == 1
  #define MATRIX_WIDTH   16
  #define MATRIX_HEIGHT  16
#elif IDOTMATRIX_SCREEN_TYPE == 3
  #define MATRIX_WIDTH   32
  #define MATRIX_HEIGHT  32
#elif IDOTMATRIX_SCREEN_TYPE == 4
  #define MATRIX_WIDTH   64
  #define MATRIX_HEIGHT  64
#else
  #error "Unsupported IDOTMATRIX_SCREEN_TYPE (supported: 1=16x16, 3=32x32, 4=64x64)"
#endif

#define NUM_LEDS       ((uint16_t)MATRIX_WIDTH * (uint16_t)MATRIX_HEIGHT)
#define LOGICAL_FRAME_BYTES ((size_t)NUM_LEDS * sizeof(CRGB))

// Physical display dimensions. These do not change what the app sees.
#ifndef PHYSICAL_MATRIX_WIDTH
#define PHYSICAL_MATRIX_WIDTH   64
#endif
#ifndef PHYSICAL_MATRIX_HEIGHT
#define PHYSICAL_MATRIX_HEIGHT  64
#endif
#define PHYSICAL_NUM_LEDS       ((uint16_t)PHYSICAL_MATRIX_WIDTH * (uint16_t)PHYSICAL_MATRIX_HEIGHT)

// WS2812 backend options.
#define LED_TYPE       WS2812B
#define COLOR_ORDER    GRB
#define MATRIX_MIRROR_X 1

// MatrixPortal S3 / HUB75 backend. This mirrors the validated WLED-family
// MatrixPortal path: ESP32-HUB75-MatrixPanel-I2S-DMA, 8-bit
// color depth, single buffering and the exact MatrixPortal S3 HUB75 pinout.
// Keeping brightness in the HUB75 OE/PWM path preserves the full RGB values
// instead of pre-scaling the framebuffer to the local brightness ceiling.
#if DISPLAY_BACKEND == DISPLAY_BACKEND_HUB75
  #include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
  #if defined(IDOTMATRIX_BOARD_CLOCKWISE)
  HUB75_I2S_CFG::i2s_pins hub75Pins = {
    25, 26, 27, 14, 12, 13, 23, 19, 5, 17, 18, 4, 15, 16
  };
  #elif defined(IDOTMATRIX_BOARD_WAVESHARE_S3_RGB_MATRIX)
  // Waveshare ESP32-S3-RGB-Matrix official/WLED-qualified HUB75 wiring.
  // Order: R1,G1,B1,R2,G2,B2,A,B,C,D,E,LAT,OE,CLK.
  HUB75_I2S_CFG::i2s_pins hub75Pins = {
    4, 5, 6, 7, 15, 16,
    18, 8, 3, 42, 9,
    40, 2, 41
  };
  #else
  // Adafruit MatrixPortal S3 HUB75 wiring.
  HUB75_I2S_CFG::i2s_pins hub75Pins = {
    42, 41, 40, 38, 39, 37, // R1, G1, B1, R2, G2, B2
    45, 36, 48, 35, 21,     // A, B, C, D, E
    47, 14, 2                // LAT, OE, CLK
  };
  #endif
  #define HUB75_COLOR_DEPTH 8
  MatrixPanel_I2S_DMA *hub75Matrix = nullptr;
#endif

// Local hardware/test ceiling expressed on the common 0..255 brightness scale.
// WS2812 applies it through FastLED global brightness. HUB75 applies it through
// MatrixPanel_I2S_DMA output-enable timing so framebuffer RGB values stay 8-bit.
#define MAX_LED_BRIGHTNESS 50

// Brightness persistence
#define BRIGHTNESS_NVS_NAMESPACE "idotmatrix"
#define BRIGHTNESS_NVS_KEY       "brightness"
#define BRIGHTNESS_SAVE_DELAY_MS 1000UL

// Clock presentation persistence. The RTC stores date/time only; the app's
// Clock command also carries renderer state that must survive MCU reboot.
// Keep this in the same namespace as brightness so the existing device-reset
// path clears all local presentation preferences atomically.
#define CLOCK_NVS_NAMESPACE      "idotmatrix"
#define CLOCK_NVS_VALID_KEY      "cvalid"
#define CLOCK_NVS_STYLE_KEY      "cstyle"
#define CLOCK_NVS_FLAGS_KEY      "cflags"
#define CLOCK_NVS_R_KEY          "cr"
#define CLOCK_NVS_G_KEY          "cg"
#define CLOCK_NVS_B_KEY          "cb"
#define CLOCK_SAVE_DELAY_MS      1000UL

extern uint8_t brightnessPercent;
bool brightnessDirty = false;
uint32_t brightnessDirtySince = 0;

uint8_t brightnessPercentToHw(uint8_t percent) {
  if (percent > 100) percent = 100;
  return (uint8_t)((uint16_t)percent * MAX_LED_BRIGHTNESS / 100U);
}

void applyCurrentBrightness() {
  uint8_t hw = brightnessPercentToHw(brightnessPercent);
#if DISPLAY_BACKEND == DISPLAY_BACKEND_WS2812
  FastLED.setBrightness(hw);
#endif
#if DEBUG_SERIAL
  Serial.print("BRIGHTNESS HW: ");
  Serial.print(brightnessPercent);
  Serial.print("% -> ");
  Serial.print(hw);
  Serial.print("/");
  Serial.println(MAX_LED_BRIGHTNESS);
#endif
}

void scheduleBrightnessSave() {
  brightnessDirty = true;
  brightnessDirtySince = millis();
}

void flushBrightnessSaveIfNeeded() {
  if (!brightnessDirty) return;
  if ((uint32_t)(millis() - brightnessDirtySince) < BRIGHTNESS_SAVE_DELAY_MS) return;

  Preferences prefs;
  if (prefs.begin(BRIGHTNESS_NVS_NAMESPACE, false)) {
    prefs.putUChar(BRIGHTNESS_NVS_KEY, brightnessPercent);
    prefs.end();
#if DEBUG_SERIAL
    Serial.print("BRIGHTNESS SAVE: ");
    Serial.print(brightnessPercent);
    Serial.println("%");
#endif
  }
  brightnessDirty = false;
}

void loadBrightnessFromNVS() {
  Preferences prefs;
  if (prefs.begin(BRIGHTNESS_NVS_NAMESPACE, true)) {
    if (prefs.isKey(BRIGHTNESS_NVS_KEY)) {
      brightnessPercent = prefs.getUChar(BRIGHTNESS_NVS_KEY, 100);
    }
    prefs.end();
  }
  if (brightnessPercent > 100) brightnessPercent = 100;
#if DEBUG_SERIAL
  Serial.print("BRIGHTNESS LOAD: ");
  Serial.print(brightnessPercent);
  Serial.println("%");
#endif
  applyCurrentBrightness();
}


#define MAX_PACKET_SIZE     8192
#define MAX_TEXT_PAYLOAD   16654  // 14-byte header + up to 64 x (4 meta + 256 bitmap)
#define MAX_TEXT_GLYPHS        64
#define TEXT_GLOBAL_HEADER     14
#define TEXT_GLYPH_META        4   // marker + RGB; glyph bitmap follows
#define TEXT_MAX_BITMAP_BYTES 256  // 32x64 glyph = 256 bytes
#define MAX_EFFECT_COLORS   16
#define PACKET_REASSEMBLY_TIMEOUT_MS  5000UL
#define BULK_TRANSFER_TIMEOUT_MS      30000UL

// ======================================================
// ALARMS
// ======================================================
#define ALARM_SLOT_COUNT       10

// Buzzer timing is shared by active and passive outputs. The electrical/output
// backend and event policy are resolved by IDotMatrixHardwareConfig.h so local
// hardware can be changed without editing this source file.
#define BUZZER_PULSE_ON_MS       90UL
#define BUZZER_PULSE_GAP_MS      70UL
#define BUZZER_TRILL_PAUSE_MS   550UL
#define BUZZER_TRILL_PULSES       3
#if IDOTMATRIX_BUZZER_TYPE == IDOTMATRIX_BUZZER_PASSIVE
static constexpr uint8_t BUZZER_LEDC_RESOLUTION_BITS = 10;
static constexpr uint32_t BUZZER_LEDC_MAX_DUTY = (1u << BUZZER_LEDC_RESOLUTION_BITS) - 1u;
#endif
#define ALARM_MEDIA_BASE_ID    0x14
#define ALARM_CONTENT_GIF      0x01
#define ALARM_CONTENT_RAW      0x02
#define ALARM_HEADER_SIZE      24
#define ALARM_UPLOAD_TIMEOUT_MS 10000UL

// ======================================================
// PROGRAM / SCHEDULE
// ======================================================
#define SCHEDULE_MAX_ACTIVITIES 32
#define SCHEDULE_COMMIT_DELAY_MS 900UL
#define SCHEDULE_MEDIA_UPLOAD_TIMEOUT_MS 10000UL
#define SCHEDULE_CONTENT_GIF    0x01
#define SCHEDULE_CONTENT_IMAGE  0x02
#define SCHEDULE_CONTENT_TEXT   0x03
#define SCHEDULE_MEDIA_BASE_ID  0x1E



#define FA_SERVICE_UUID "000000fa-0000-1000-8000-00805f9b34fb"
#define FA02_UUID       "0000fa02-0000-1000-8000-00805f9b34fb"
#define FA03_UUID       "0000fa03-0000-1000-8000-00805f9b34fb"
#define AE_SERVICE_UUID "0000ae00-0000-1000-8000-00805f9b34fb"
#define AE01_UUID       "0000ae01-0000-1000-8000-00805f9b34fb"
#define AE02_UUID       "0000ae02-0000-1000-8000-00805f9b34fb"

#if IDOTMATRIX_USE_NIMBLE
NimBLEServer *server = nullptr;
NimBLECharacteristic *fa02 = nullptr;
NimBLECharacteristic *fa03 = nullptr;
NimBLECharacteristic *ae01 = nullptr;
NimBLECharacteristic *ae02 = nullptr;
#else
BLEServer *server = nullptr;
BLECharacteristic *fa02 = nullptr;
BLECharacteristic *fa03 = nullptr;
BLECharacteristic *ae01 = nullptr;
BLECharacteristic *ae02 = nullptr;
#endif
bool deviceConnected = false;

// BLE callbacks and Arduino loop() run on different FreeRTOS tasks
// (and typically different ESP32 cores). Protect shared protocol/runtime state
// with a task mutex instead of relying on volatile flags. This is a scheduler
// mutex, not a spinlock: filesystem and renderer work must never run inside an
// interrupt/critical section.
SemaphoreHandle_t runtimeStateMutex = nullptr;

bool lockRuntimeState() {
  return runtimeStateMutex && xSemaphoreTake(runtimeStateMutex, portMAX_DELAY) == pdTRUE;
}

void unlockRuntimeState() {
  if (runtimeStateMutex) xSemaphoreGive(runtimeStateMutex);
}

#if DISPLAY_BACKEND == DISPLAY_BACKEND_WS2812
CRGB leds[PHYSICAL_NUM_LEDS];
#endif
// Logical buffers are allocated from the heap at runtime. Keeping 64x64
// framebuffers in .bss overflows ESP32 DRAM before the sketch can link.
CRGB *framebuffer = nullptr;

enum DisplayMode {
  DISPLAY_NONE,
  DISPLAY_SOLID,
  DISPLAY_RAW,
  DISPLAY_GRAFFITI,
  DISPLAY_GIF,
  DISPLAY_TEXT,
  DISPLAY_EFFECT,
  DISPLAY_AUDIO,
  DISPLAY_CLOCK,
  DISPLAY_COUNTDOWN,
  DISPLAY_STOPWATCH,
  DISPLAY_SCOREBOARD
};

DisplayMode displayMode = DISPLAY_NONE;
bool screenOn = false;
bool diyMode = false;
bool flipped180 = false;
uint8_t brightnessPercent = 100;

// ======================================================
// CLOCK / DATE
// ======================================================
bool clockSynced = false;
uint16_t syncYear = 2026;
uint8_t syncMonth = 1, syncDay = 1;
uint8_t syncHour = 0, syncMinute = 0, syncSecond = 0;
uint64_t syncMillis = 0;

static inline uint64_t monotonicMillis64() {
  return (uint64_t)(esp_timer_get_time() / 1000LL);
}
uint8_t clockStyle = 0;
bool clock24h = false;
bool clockShowDate = false;
CRGB defaultClockColor(uint8_t style) {
#if defined(IDOTMATRIX_BOARD_CLOCKWISE)
  // Android ClockFragment presets for square displays, including 64x64.
  static const CRGB colors[] = {
    {97,24,214}, {229,12,23}, {255,152,43}, {19,67,248},
    {106,39,248}, {255,146,42}, {255,15,77}, {67,250,85},
  };
  return colors[style & 7];
#else
  (void)style;
  return CRGB::White;
#endif
}

#if defined(IDOTMATRIX_BOARD_CLOCKWISE)
CRGB clockwiseClockDisplayColor(uint8_t style, const CRGB& selected) {
  static const CRGB previewColors[] = {
    {110,16,252}, {218,1,1}, {252,161,1}, {34,76,252},
    {111,42,252}, {252,155,1}, {252,1,84}, {116,248,76},
  };
  const CRGB preset = defaultClockColor(style);
  if (selected.r == preset.r && selected.g == preset.g && selected.b == preset.b)
    return previewColors[style & 7];
  // The Android 64x64 clock tab also sends this red for the Christmas clock.
  if ((style & 7) == 1 && selected.r == 220 && selected.g == 1 && selected.b == 1)
    return previewColors[1];
  return selected;
}
#endif

CRGB clockColor = defaultClockColor(0);
uint32_t clockCycleStartedAt = 0;
uint32_t clockEntryProtectUntil = 0;
bool clockDatePreference = false;
bool clockPrefsDirty = false;
uint32_t clockPrefsDirtySince = 0;

void scheduleClockPreferencesSave() {
  clockPrefsDirty = true;
  clockPrefsDirtySince = millis();
}

void flushClockPreferencesSaveIfNeeded() {
  if (!clockPrefsDirty) return;
  if ((uint32_t)(millis() - clockPrefsDirtySince) < CLOCK_SAVE_DELAY_MS) return;

  Preferences prefs;
  if (prefs.begin(CLOCK_NVS_NAMESPACE, false)) {
    const uint8_t flags = (clock24h ? 0x01 : 0x00) | (clockShowDate ? 0x02 : 0x00);
    prefs.putBool(CLOCK_NVS_VALID_KEY, true);
    prefs.putUChar(CLOCK_NVS_STYLE_KEY, clockStyle);
    prefs.putUChar(CLOCK_NVS_FLAGS_KEY, flags);
    prefs.putUChar(CLOCK_NVS_R_KEY, clockColor.r);
    prefs.putUChar(CLOCK_NVS_G_KEY, clockColor.g);
    prefs.putUChar(CLOCK_NVS_B_KEY, clockColor.b);
    prefs.end();
    clockPrefsDirty = false;
#if DEBUG_SERIAL
    Serial.print("CLOCK PREF SAVE: style="); Serial.print(clockStyle);
    Serial.print(" 24H="); Serial.print(clock24h ? 1 : 0);
    Serial.print(" DATE="); Serial.print(clockShowDate ? 1 : 0);
    Serial.print(" RGB="); Serial.print(clockColor.r); Serial.print(',');
    Serial.print(clockColor.g); Serial.print(','); Serial.println(clockColor.b);
#endif
  }
}

void loadClockPreferencesFromNVS() {
  Preferences prefs;
  if (!prefs.begin(CLOCK_NVS_NAMESPACE, true)) return;
  if (prefs.getBool(CLOCK_NVS_VALID_KEY, false)) {
    clockStyle = (uint8_t)(prefs.getUChar(CLOCK_NVS_STYLE_KEY, 0) & 0x3F);
    const uint8_t flags = prefs.getUChar(CLOCK_NVS_FLAGS_KEY, 0);
    clock24h = (flags & 0x01) != 0;
    clockShowDate = (flags & 0x02) != 0;
    clockDatePreference = clockShowDate;
    const CRGB fallback = defaultClockColor(clockStyle);
    clockColor = CRGB(
      prefs.getUChar(CLOCK_NVS_R_KEY, fallback.r),
      prefs.getUChar(CLOCK_NVS_G_KEY, fallback.g),
      prefs.getUChar(CLOCK_NVS_B_KEY, fallback.b));
#if DEBUG_SERIAL
    Serial.print("CLOCK PREF LOAD: style="); Serial.print(clockStyle);
    Serial.print(" 24H="); Serial.print(clock24h ? 1 : 0);
    Serial.print(" DATE="); Serial.print(clockShowDate ? 1 : 0);
    Serial.print(" RGB="); Serial.print(clockColor.r); Serial.print(',');
    Serial.print(clockColor.g); Serial.print(','); Serial.println(clockColor.b);
#endif
  }
  prefs.end();
}


// ======================================================
// ECO
// ======================================================
struct EnergySavingState {
  bool enabled = false;
  uint8_t startHour = 0, startMinute = 0;
  uint8_t endHour = 0, endMinute = 0;
  uint8_t reductionPercent = 0;
} energySaving;

// Last brightness actually pushed to FastLED. This lets static display modes
// react when an ECO interval begins/ends even if no renderer requests refresh.
uint8_t lastAppliedOutputBrightness = 0xFF;
uint32_t lastEnergySavingCheckMs = 0;

// ======================================================
// COUNTDOWN / STOPWATCH / SCOREBOARD
// ======================================================
bool countdownRunning = false;
bool countdownPaused = false;
uint32_t countdownRemainingMs = 0;
uint32_t countdownStartMillis = 0;
bool countdownFinishSent = false;

bool stopwatchRunning = false;
uint32_t stopwatchElapsedMs = 0;
uint32_t stopwatchStartMillis = 0;

uint16_t scoreA = 0, scoreB = 0;

// ======================================================
// TEXT
// ======================================================
struct TextState {
  bool valid = false;
  uint8_t glyphCount = 0;
  uint8_t motionEffect = 0;
  uint8_t speed = 5;
  uint8_t colorMode = 1;
  uint8_t colorR = 255, colorG = 255, colorB = 255;
  uint8_t backgroundMode = 0;
  uint8_t backgroundR = 0, backgroundG = 0, backgroundB = 0;
  uint8_t meta[MAX_TEXT_GLYPHS][TEXT_GLYPH_META];
  uint8_t bitmap[MAX_TEXT_GLYPHS * TEXT_MAX_BITMAP_BYTES];
  uint16_t bitmapOffset[MAX_TEXT_GLYPHS];
  uint8_t bitmapWidth[MAX_TEXT_GLYPHS];
  uint8_t glyphWidth = 8;
  uint8_t glyphHeight = 16;
  uint8_t glyphAdvance = 8;
  int16_t offsetX = 0, offsetY = 1;
  uint32_t animationStart = 0;
  uint32_t lastFrame = 0;        // motion/page timing
  uint32_t lastRenderFrame = 0;  // visual refresh timing for dynamic colors/effects
} textState;

// ======================================================
// EFFECT LIGHT: 03 02 EFFECT SPEED COUNT [R G B]...
// RGB protocol values use the observed 0..127 scale.
// ======================================================
struct EffectState {
  bool valid = false;
  uint8_t effect = 0;
  uint8_t speed = 90;
  uint8_t colorCount = 0;
  CRGB colors[MAX_EFFECT_COLORS];
  uint32_t startMillis = 0;
  uint32_t lastFrameMillis = 0;
} effectState;

// ======================================================
// AUDIO / RHYTHM
// Five LEVEL modes and five FFT modes observed from app.
// LEVEL: 06 00 00 02 LEVEL MODE, MODE=1..5
// FFT  : 21 00 01 02 MODE + 16 mirrored values, MODE=0..4
// ======================================================
struct AudioState {
  bool valid = false;
  bool fft = false;
  uint8_t mode = 0;
  uint8_t level = 0;
  uint8_t bands[8] = {0};
  uint32_t lastPacketMs = 0;
  uint32_t packetCounter = 0;
} audioState;


// ======================================================
// FORWARD DECLARATIONS USED BY ALARM SUPPORT
//
// Alarm routines are defined before the GIF section.
// Keep the required declarations here instead of relying on Arduino IDE
// automatic prototype generation.
// ======================================================
void freeGIF();
bool startGIF();
bool startStoredGIFPlayback(const String &sourcePath, uint32_t expectedSize, uint32_t expectedCRC);
void stopGIFPlayback();
void switchDisplayMode(DisplayMode m);
void renderClock();
void stopScheduleActivity();
extern uint8_t scheduleGlobalFlags;
extern int8_t scheduleActiveIndex;
extern int8_t scheduleFailedIndex;
extern CRGB *scheduleSavedFrame;
uint32_t crc32Update(uint32_t crc, const uint8_t *data, size_t len);
bool fileMatchesMedia(const String &path, uint32_t expectedSize, uint32_t expectedCRC);
void clearFramebuffer(const CRGB &color = CRGB::Black);
bool handleCarouselCommand(const uint8_t *data, size_t len);
void loadCarousel();
void updateCarousel(uint32_t now);
void stopCarouselPlayback();
void updatePreset(uint32_t now);
void stopPresetPlayback();
void clearPresetBank(bool removeFiles);
int8_t nextPresetSlot(int8_t currentLocal);
bool startPresetSlot(uint8_t localSlot);
void clearPersistentDeviceState();
void applyBootDisplayPolicy();
int8_t nextCarouselSlot(int8_t current);
bool startCarouselSlot(uint8_t slot);
String carouselFileName(uint8_t slot, uint8_t dataType);
bool decodeSchedulePNG(File &f, uint32_t fileSize);
extern bool carouselActive;
extern bool presetActive;
extern int8_t carouselActiveSlot;
extern bool gifCarouselPlaybackFileActive;
extern bool carouselEnterRequested;


// ======================================================
// ALARM STATE / PERSISTENCE
// ======================================================
struct AlarmSlot {
  bool configured = false;
  uint8_t flags = 0;          // bit0 enable, bit1..7 lun..dom
  uint8_t hour = 0;
  uint8_t minute = 0;
  uint8_t durationSec = 10;
  uint8_t reserved1 = 0;
  uint8_t contentType = 0;
  uint8_t buzzer = 0;
  uint8_t reserved2 = 0;
  uint32_t mediaSize = 0;
  uint32_t mediaCRC = 0;
  uint16_t reserved3 = 0;
  uint8_t mediaId = 0;
  uint32_t lastTriggerMinuteKey = 0xFFFFFFFFUL;
};

AlarmSlot alarms[ALARM_SLOT_COUNT];
Preferences alarmPrefs;
bool alarmActive = false;
uint8_t activeAlarmSlot = 0xFF;
uint32_t alarmEndsAt = 0;
DisplayMode alarmPreviousMode = DISPLAY_CLOCK;
bool alarmPreviousCarousel = false;
int8_t alarmPreviousCarouselSlot = -1;

String alarmFileName(uint8_t slot) { return String("/alarm") + slot + ".bin"; }
String alarmTempFileName(uint8_t slot) { return String("/alarm") + slot + ".tmp"; }
String alarmBackupFileName(uint8_t slot) { return String("/alarm") + slot + ".bak"; }

// 64x64 Alarm media can span multiple complete FA02 logical
// packets. Each packet repeats the same 24-byte Alarm header; mediaSize and
// mediaCRC describe the complete media object, not the payload bytes carried
// by the current packet. Stage chunks in LittleFS and publish atomically only
// after the declared total size and CRC have been verified.
struct AlarmUploadState {
  bool active = false;
  uint8_t slot = 0xFF;
  AlarmSlot candidate;
  AlarmSlot previous;
  uint32_t received = 0;
  uint32_t runningCRC = 0xFFFFFFFFUL;
  uint32_t lastChunkAt = 0;
};

AlarmUploadState alarmUpload;

bool alarmUploadMetadataMatches(const AlarmSlot &a, const AlarmSlot &b) {
  return a.flags == b.flags &&
         a.hour == b.hour &&
         a.minute == b.minute &&
         a.durationSec == b.durationSec &&
         a.reserved1 == b.reserved1 &&
         a.contentType == b.contentType &&
         a.buzzer == b.buzzer &&
         a.mediaSize == b.mediaSize &&
         a.mediaCRC == b.mediaCRC &&
         a.reserved3 == b.reserved3 &&
         a.mediaId == b.mediaId;
}

void abortAlarmUpload(const char *reason) {
  if (!alarmUpload.active) return;
#if DEBUG_SERIAL
  if (reason) {
    Serial.print("ALARM UPLOAD ABORT slot="); Serial.print(alarmUpload.slot);
    Serial.print(" received="); Serial.print(alarmUpload.received);
    Serial.print("/"); Serial.print(alarmUpload.candidate.mediaSize);
    Serial.print(" reason="); Serial.println(reason);
  }
#endif
  if (littleFsReady && alarmUpload.slot < ALARM_SLOT_COUNT)
    LittleFS.remove(alarmTempFileName(alarmUpload.slot));
  alarmUpload = AlarmUploadState();
}

bool beginAlarmUpload(uint8_t slot, const AlarmSlot &candidate) {
  abortAlarmUpload(nullptr);
  if (!littleFsReady || slot >= ALARM_SLOT_COUNT) return false;

  String tmp = alarmTempFileName(slot);
  LittleFS.remove(tmp);
  File f = LittleFS.open(tmp, "w");
  if (!f) return false;
  f.close();

  alarmUpload.active = true;
  alarmUpload.slot = slot;
  alarmUpload.candidate = candidate;
  alarmUpload.previous = alarms[slot];
  alarmUpload.received = 0;
  alarmUpload.runningCRC = 0xFFFFFFFFUL;
  alarmUpload.lastChunkAt = millis();
  return true;
}

void updateAlarmUploadTimeout() {
  if (alarmUpload.active &&
      (uint32_t)(millis() - alarmUpload.lastChunkAt) > ALARM_UPLOAD_TIMEOUT_MS)
    abortAlarmUpload("timeout");
}

bool saveAlarmMetaValue(uint8_t slot, const AlarmSlot &value) {
  if (slot >= ALARM_SLOT_COUNT) return false;
  char key[12];
  snprintf(key,sizeof(key),"a%u",slot);
  return alarmPrefs.putBytes(key,&value,sizeof(AlarmSlot)) == sizeof(AlarmSlot);
}

bool saveAlarmMeta(uint8_t slot) {
  return slot < ALARM_SLOT_COUNT && saveAlarmMetaValue(slot, alarms[slot]);
}

void recoverAlarmFile(uint8_t slot) {
  if (!littleFsReady || slot >= ALARM_SLOT_COUNT) return;
  const AlarmSlot &a = alarms[slot];
  String dst = alarmFileName(slot), tmp = alarmTempFileName(slot), bak = alarmBackupFileName(slot);
  LittleFS.remove(tmp);

  if (!a.configured || !a.mediaSize) {
    LittleFS.remove(dst);
    LittleFS.remove(bak);
    return;
  }

  bool dstOK = fileMatchesMedia(dst, a.mediaSize, a.mediaCRC);
  bool bakOK = fileMatchesMedia(bak, a.mediaSize, a.mediaCRC);
  if (dstOK) {
    LittleFS.remove(bak);
  } else if (bakOK) {
    LittleFS.remove(dst);
    if (!LittleFS.rename(bak, dst)) {
#if DEBUG_SERIAL
      Serial.print("ALARM RECOVERY rename failed slot="); Serial.println(slot);
#endif
    }
  }
}

void loadAlarms() {
  alarmPrefs.begin("idot-alarm",false);
  for(uint8_t i=0;i<ALARM_SLOT_COUNT;i++){
    char key[12]; snprintf(key,sizeof(key),"a%u",i);
    size_t n=alarmPrefs.getBytesLength(key);
    if(n==sizeof(AlarmSlot)) alarmPrefs.getBytes(key,&alarms[i],sizeof(AlarmSlot));
    alarms[i].lastTriggerMinuteKey=0xFFFFFFFFUL;
    recoverAlarmFile(i);
#if DEBUG_SERIAL
    if(alarms[i].configured){
      Serial.print("ALARM LOAD slot="); Serial.print(i);
      Serial.print(" flags=0x"); Serial.print(alarms[i].flags,HEX);
      Serial.print(" enabled="); Serial.print((alarms[i].flags&0x01)?1:0);
      Serial.print(" time="); if(alarms[i].hour<10)Serial.print('0'); Serial.print(alarms[i].hour);
      Serial.print(':'); if(alarms[i].minute<10)Serial.print('0'); Serial.print(alarms[i].minute);
      Serial.print(" dur="); Serial.print(alarms[i].durationSec);
      Serial.print(" type="); Serial.print(alarms[i].contentType);
      Serial.print(" buzzer="); Serial.print(alarms[i].buzzer);
      Serial.print(" bytes="); Serial.print(alarms[i].mediaSize);
      Serial.print(" mediaId=0x"); Serial.print(alarms[i].mediaId,HEX);
      Serial.print(" file=");
      Serial.println(!littleFsReady ? "FS-OFFLINE" : (LittleFS.exists(alarmFileName(i)) ? "YES" : "NO"));
    }
#endif
  }
}

bool isLeapYear(uint16_t y){
  return (y%4==0 && y%100!=0) || (y%400==0);
}

bool isValidDateTime(uint16_t y,uint8_t mo,uint8_t d,uint8_t h,uint8_t mi,uint8_t se){
  if(mo<1 || mo>12 || h>23 || mi>59 || se>59) return false;
  static const uint8_t mdays[]={31,28,31,30,31,30,31,31,30,31,30,31};
  uint8_t dim=mdays[mo-1];
  if(mo==2 && isLeapYear(y)) dim=29;
  return d>=1 && d<=dim;
}

uint8_t currentWeekdayBit(uint16_t y,uint8_t m,uint8_t d){
  // Sakamoto: 0=Sunday. Protocol: bit1=Monday ... bit7=Sunday.
  if(!isValidDateTime(y,m,d,0,0,0)) return 0;
  static const uint8_t t[]={0,3,2,5,0,3,5,1,4,6,2,4};
  uint16_t yy=y; if(m<3) yy--;
  uint8_t dow=(yy+yy/4-yy/100+yy/400+t[m-1]+d)%7;
  return dow==0 ? 0x80 : (uint8_t)(1U<<dow);
}

bool hasValidTimeSource(){
#if IDOTMATRIX_RTC_AVAILABLE
  if(rtcReady && rtcTimeValid) return true;
#endif
  return clockSynced;
}

void getAlarmDateTime(uint16_t &y,uint8_t &mo,uint8_t &d,uint8_t &h,uint8_t &mi,uint8_t &se){
#if IDOTMATRIX_RTC_AVAILABLE
  if(rtcReady && rtcTimeValid){
    IDotMatrixRtcDateTime now;
    if (rtc.read(now)) {
      y=now.year; mo=now.month; d=now.day; h=now.hour; mi=now.minute; se=now.second;
      return;
    }
    rtcTimeValid=false;
    rtcReady=false;
    rtcRetryNextAt=millis() + IDOTMATRIX_RTC_RETRY_INTERVAL_MS;
  }
#endif
  getCurrentTime(h,mi,se);
  y=syncYear; mo=syncMonth; d=syncDay;
  if(!clockSynced) return;
  uint64_t elapsed=(monotonicMillis64()-syncMillis)/1000ULL;
  uint64_t dayCarry=((uint64_t)syncHour*3600ULL+(uint64_t)syncMinute*60ULL+syncSecond+elapsed)/86400ULL;
  static const uint8_t mdays[]={31,28,31,30,31,30,31,31,30,31,30,31};
  while(dayCarry--){
    uint8_t dim=mdays[mo-1];
    if(mo==2&&isLeapYear(y)) dim=29;
    if(++d>dim){ d=1; if(++mo>12){mo=1;y++;} }
  }
}

#if IDOTMATRIX_RTC_AVAILABLE
void updateRtcRecovery(uint32_t nowMs){
  if(rtcReady) return;
  if((int32_t)(nowMs-rtcRetryNextAt)<0) return;
  rtcRetryNextAt=nowMs + IDOTMATRIX_RTC_RETRY_INTERVAL_MS;

  if(!rtc.begin(Wire,IDOTMATRIX_RTC_I2C_ADDRESS)){
#if DEBUG_SERIAL && IDOTMATRIX_RTC_DIAGNOSTICS
    Serial.print("RTC: retry failed: "); Serial.println(rtc.errorText());
#endif
    return;
  }

  rtcReady=true;
  rtcTimeValid=rtc.timeValid();
#if DEBUG_SERIAL
  Serial.print("RTC: recovered on I2C address 0x"); Serial.print(IDOTMATRIX_RTC_I2C_ADDRESS,HEX);
  Serial.print(" time="); Serial.println(rtcTimeValid?"VALID":"INVALID");
#endif

  if(rtcTimeValid && !clockSynced){
    IDotMatrixRtcDateTime recovered;
    if(rtc.read(recovered)){
      syncYear=recovered.year; syncMonth=recovered.month; syncDay=recovered.day;
      syncHour=recovered.hour; syncMinute=recovered.minute; syncSecond=recovered.second;
      syncMillis=monotonicMillis64(); clockSynced=true;
#if DEBUG_SERIAL
      Serial.println("RTC: software clock initialized from recovered RTC");
#endif
    } else {
      rtcReady=false; rtcTimeValid=false;
      return;
    }
  }

#if IDOTMATRIX_RTC_SYNC_FROM_BLE
  if(!rtcTimeValid && clockSynced){
    uint16_t y=0; uint8_t mo=0,d=0,h=0,mi=0,se=0;
    getAlarmDateTime(y,mo,d,h,mi,se);
    IDotMatrixRtcDateTime value;
    value.year=y; value.month=mo; value.day=d; value.hour=h; value.minute=mi; value.second=se;
    if(rtc.adjust(value)){
      rtcTimeValid=true;
#if DEBUG_SERIAL
      Serial.println("RTC: recovered and synchronized from software time");
#endif
    } else {
#if DEBUG_SERIAL
      Serial.print("RTC: recovery synchronization failed: "); Serial.println(rtc.errorText());
#endif
    }
  }
#endif
}
#endif

bool loadAlarmMedia(uint8_t slot){
  if(!littleFsReady || slot>=ALARM_SLOT_COUNT) return false;
  AlarmSlot &a=alarms[slot];
  File f=LittleFS.open(alarmFileName(slot),"r");
  if(!f || (uint32_t)f.size()!=a.mediaSize){ if(f)f.close(); return false; }
  if(a.contentType==ALARM_CONTENT_RAW && a.mediaSize==(uint32_t)NUM_LEDS*3UL){
    uint8_t rgb[3];
    switchDisplayMode(DISPLAY_RAW);
    for(uint16_t i=0;i<NUM_LEDS;i++){ if(f.read(rgb,3)!=3){f.close();return false;} framebuffer[i]=CRGB(rgb[0],rgb[1],rgb[2]); }
    f.close(); refreshMatrix(); return true;
  }
  if(a.contentType==ALARM_CONTENT_GIF){
    f.close();
    return startStoredGIFPlayback(alarmFileName(slot), a.mediaSize, a.mediaCRC);
  }
  f.close(); return false;
}

// ======================================================
// BUZZER - NON-BLOCKING TRILL (ACTIVE OR PASSIVE)
// ======================================================
#if IDOTMATRIX_BUZZER_AVAILABLE || IDOTMATRIX_AUDIO_CODEC_AVAILABLE
static bool buzzerHardwareReady = false;
static bool buzzerOutputOn = false;
static bool buzzerPatternRunning = false;
static bool countdownBuzzerOneShot = false;
static bool scheduleBuzzerOneShot = false;
static bool connectionBuzzerOneShot = false;
static uint8_t buzzerPulseIndex = 0;
static uint32_t buzzerNextChangeAt = 0;

static inline void setBuzzerOutput(bool on) {
  if (!buzzerHardwareReady) {
    buzzerOutputOn = false;
    return;
  }
  buzzerOutputOn = on;
#if IDOTMATRIX_AUDIO_CODEC_AVAILABLE
  idotAudioOutputSetTone(on);
#elif IDOTMATRIX_BUZZER_TYPE == IDOTMATRIX_BUZZER_PASSIVE
  // Passive outputs use a 50% LEDC tone while active. At rest, keep the GPIO
  // at the module's inactive logic level. This is critical for three-wire
  // transistor modules marked "low level trigger": leaving the pin LOW would
  // continuously bias the buzzer even though no audible tone is generated.
  if (on) {
    ledcWriteTone(IDOTMATRIX_BUZZER_PIN, IDOTMATRIX_BUZZER_FREQUENCY_HZ);
  } else {
    ledcWrite(IDOTMATRIX_BUZZER_PIN, idotPassiveBuzzerIdleDuty(IDOTMATRIX_BUZZER_PASSIVE_TRIGGER_LOW != 0, BUZZER_LEDC_MAX_DUTY));
  }
#else
  digitalWrite(IDOTMATRIX_BUZZER_PIN,
               on ? (IDOTMATRIX_BUZZER_ACTIVE_HIGH ? HIGH : LOW)
                  : (IDOTMATRIX_BUZZER_ACTIVE_HIGH ? LOW : HIGH));
#endif
}

static bool continuousBuzzerRequested() {
  bool wanted = false;

#if IDOTMATRIX_ALARM_BUZZER_ENABLED
  if (alarmActive && activeAlarmSlot < ALARM_SLOT_COUNT)
    wanted |= alarms[activeAlarmSlot].buzzer != 0;
#endif

  return wanted;
}

void triggerCountdownFinishBuzzer() {
#if IDOTMATRIX_COUNTDOWN_BUZZER_ENABLED
  countdownBuzzerOneShot = true;
#endif
}

void cancelCountdownFinishBuzzer() {
  countdownBuzzerOneShot = false;
}

void triggerScheduleStartBuzzer() {
#if IDOTMATRIX_SCHEDULE_BUZZER_ENABLED
  scheduleBuzzerOneShot = true;
#endif
}

void cancelScheduleStartBuzzer() {
  scheduleBuzzerOneShot = false;
}

void triggerConnectionBuzzer() {
#if IDOTMATRIX_CONNECTION_BUZZER_ENABLED
  // Connection feedback is intentionally low priority. Never interrupt an
  // alarm or another one-shot notification already using the buzzer.
  if (!continuousBuzzerRequested() && !buzzerPatternRunning &&
      !countdownBuzzerOneShot && !scheduleBuzzerOneShot) {
    connectionBuzzerOneShot = true;
  }
#endif
}

void cancelConnectionBuzzer() {
  connectionBuzzerOneShot = false;
}

void updateBuzzer() {
  if (!buzzerHardwareReady) {
    countdownBuzzerOneShot = false;
    scheduleBuzzerOneShot = false;
    connectionBuzzerOneShot = false;
    buzzerPatternRunning = false;
    buzzerPulseIndex = 0;
    return;
  }

  const bool continuousWanted = continuousBuzzerRequested();
  const bool trillOneShotWanted = countdownBuzzerOneShot || scheduleBuzzerOneShot;
  const bool wanted = continuousWanted || trillOneShotWanted || connectionBuzzerOneShot;
  const uint32_t now = millis();

  if (!wanted) {
    if (buzzerPatternRunning || buzzerOutputOn) setBuzzerOutput(false);
    buzzerPatternRunning = false;
    buzzerPulseIndex = 0;
    return;
  }

  if (!buzzerPatternRunning) {
    buzzerPatternRunning = true;
    buzzerPulseIndex = 0;
    setBuzzerOutput(true);
    buzzerNextChangeAt = now + BUZZER_PULSE_ON_MS;
    return;
  }

  if ((int32_t)(now - buzzerNextChangeAt) < 0) return;

  if (buzzerOutputOn) {
    setBuzzerOutput(false);

    // BLE connection feedback is exactly one short pulse, not a trill.
    if (connectionBuzzerOneShot) {
      connectionBuzzerOneShot = false;
      buzzerPatternRunning = false;
      buzzerPulseIndex = 0;
      return;
    }

    ++buzzerPulseIndex;

    if (buzzerPulseIndex >= BUZZER_TRILL_PULSES) {
      // Countdown completion and Schedule start are one-shot trills. Alarm
      // remains the only repeating buzzer request.
      countdownBuzzerOneShot = false;
      scheduleBuzzerOneShot = false;
      if (!continuousWanted) {
        buzzerPatternRunning = false;
        buzzerPulseIndex = 0;
        return;
      }
      buzzerNextChangeAt = now + BUZZER_TRILL_PAUSE_MS;
    } else {
      buzzerNextChangeAt = now + BUZZER_PULSE_GAP_MS;
    }
  } else {
    if (buzzerPulseIndex >= BUZZER_TRILL_PULSES) buzzerPulseIndex = 0;
    setBuzzerOutput(true);
    buzzerNextChangeAt = now + BUZZER_PULSE_ON_MS;
  }
}
#else
void triggerCountdownFinishBuzzer() {}
void cancelCountdownFinishBuzzer() {}
void triggerScheduleStartBuzzer() {}
void cancelScheduleStartBuzzer() {}
void triggerConnectionBuzzer() {}
void cancelConnectionBuzzer() {}
void updateBuzzer() {}
#endif

void startAlarm(uint8_t slot){
  if(slot>=ALARM_SLOT_COUNT || alarmActive) return;
  AlarmSlot &a=alarms[slot];

  // Alarm has priority over Schedule. Stop the Schedule first so its restore
  // path cannot tear down media that the Alarm has just started.
  if(scheduleActiveIndex>=0) stopScheduleActivity();
  if(presetActive) stopPresetPlayback();

  alarmPreviousMode=displayMode;
  alarmPreviousCarousel=carouselActive || gifCarouselPlaybackFileActive;
  alarmPreviousCarouselSlot=carouselActiveSlot;
  if(scheduleSavedFrame) ::memcpy(scheduleSavedFrame, framebuffer, LOGICAL_FRAME_BYTES);
  alarmActive=true; activeAlarmSlot=slot; alarmEndsAt=millis()+(uint32_t)a.durationSec*1000UL;
  loadAlarmMedia(slot);
#if DEBUG_SERIAL
  Serial.print("ALARM TRIGGER slot=");Serial.print(slot);Serial.print(" duration=");Serial.print(a.durationSec);Serial.print(" buzzer=");Serial.println(a.buzzer);
#endif
}

void stopAlarm(){
  if(!alarmActive) return;
  alarmActive=false; activeAlarmSlot=0xFF;
  stopGIFPlayback();

  if(alarmPreviousCarousel && nextCarouselSlot(-1)>=0){
    carouselEnterRequested=true;
    int8_t slot=alarmPreviousCarouselSlot;
    if(slot<0 || slot>=CAROUSEL_SLOT_COUNT) slot=nextCarouselSlot(-1);
    alarmPreviousCarousel=false; alarmPreviousCarouselSlot=-1;
    if(slot>=0){ startCarouselSlot((uint8_t)slot); return; }
  }
  alarmPreviousCarousel=false; alarmPreviousCarouselSlot=-1;

  // Restore framebuffer-backed modes exactly. Other dynamic modes cannot be
  // reconstructed generically, so fall back to the synchronized clock/blank.
  if(scheduleSavedFrame && (alarmPreviousMode==DISPLAY_SOLID || alarmPreviousMode==DISPLAY_RAW ||
      alarmPreviousMode==DISPLAY_GRAFFITI)) {
    ::memcpy(framebuffer, scheduleSavedFrame, LOGICAL_FRAME_BYTES);
    displayMode=alarmPreviousMode;
    refreshMatrix();
  } else if(hasValidTimeSource()) {
    switchDisplayMode(DISPLAY_CLOCK); renderClock();
  } else {
    switchDisplayMode(DISPLAY_NONE); clearFramebuffer(); refreshMatrix();
  }
}

void updateAlarms(){
  updateAlarmUploadTimeout();
  if(alarmActive){ if((int32_t)(millis()-alarmEndsAt)>=0) stopAlarm(); return; }
  if(!hasValidTimeSource()) return;
  static uint32_t lastCheck=0; if(millis()-lastCheck<500) return; lastCheck=millis();
  uint16_t y; uint8_t mo,d,h,mi,se; getAlarmDateTime(y,mo,d,h,mi,se);
  uint32_t minuteKey=((uint32_t)y<<20)|((uint32_t)mo<<16)|((uint32_t)d<<11)|((uint32_t)h<<6)|mi;
  uint8_t dayBit=currentWeekdayBit(y,mo,d);
  for(uint8_t i=0;i<ALARM_SLOT_COUNT;i++){
    AlarmSlot &a=alarms[i];
    if(!a.configured || !(a.flags&0x01) || a.hour!=h || a.minute!=mi || a.lastTriggerMinuteKey==minuteKey) continue;
    uint8_t days=a.flags&0xFE;
    if(days && !(days&dayBit)) continue;
    a.lastTriggerMinuteKey=minuteKey;
    if(days==0){ a.flags&=~0x01; saveAlarmMeta(i); } // one-shot
    startAlarm(i); break;
  }
}

bool processAlarmCommand(const uint8_t *data,size_t len){
  if(len<12 || data[2]!=0x00 || data[3]!=0x80) return false;
  uint8_t slot=data[4]; if(slot>=ALARM_SLOT_COUNT){sendCommandAck(0x00,0x80);return true;}

  AlarmSlot previous=alarms[slot];
  AlarmSlot candidate=previous;
  candidate.configured=true; candidate.flags=data[5]; candidate.hour=data[6]; candidate.minute=data[7]; candidate.durationSec=data[8];
  if(candidate.hour>23 || candidate.minute>59){
#if DEBUG_SERIAL
    Serial.println("ALARM ERROR: invalid time");
#endif
    sendCommandAck(0x00,0x80); return true;
  }

  // Short packet: metadata-only update. If the same slot currently has an
  // incomplete media transaction, discard that staging file first.
  if(len<ALARM_HEADER_SIZE){
    if(alarmUpload.active && alarmUpload.slot==slot) abortAlarmUpload("metadata update");
    if(len>9)candidate.reserved1=data[9]; if(len>10)candidate.contentType=data[10]; if(len>11)candidate.buzzer=data[11];
    bool stored=saveAlarmMetaValue(slot,candidate);
    if(stored) alarms[slot]=candidate;
    else saveAlarmMetaValue(slot,previous);
#if DEBUG_SERIAL
    Serial.print("ALARM META slot=");Serial.print(slot);Serial.print(" flags=0x");Serial.print(candidate.flags,HEX);Serial.print(" ");Serial.print(candidate.hour);Serial.print(":");Serial.print(candidate.minute);Serial.print(" stored=");Serial.println(stored?"YES":"NO");
#endif
    sendCommandAck(0x00,0x80); return true;
  }

  candidate.reserved1=data[9]; candidate.contentType=data[10]; candidate.buzzer=data[11]; candidate.reserved2=data[12];
  if(!littleFsReady){
#if DEBUG_SERIAL
    Serial.println("ALARM ERROR: LittleFS unavailable for media update");
#endif
    sendCommandAck(0x00,0x80); return true;
  }
  candidate.mediaSize=(uint32_t)data[13]|((uint32_t)data[14]<<8)|((uint32_t)data[15]<<16)|((uint32_t)data[16]<<24);
  candidate.mediaCRC=(uint32_t)data[17]|((uint32_t)data[18]<<8)|((uint32_t)data[19]<<16)|((uint32_t)data[20]<<24);
  candidate.reserved3=(uint16_t)data[21]|((uint16_t)data[22]<<8); candidate.mediaId=data[23];

  const uint8_t *media=data+ALARM_HEADER_SIZE;
  const size_t chunkSize=len-ALARM_HEADER_SIZE;

  // The observed 64x64 app uses reserved2=0x00 on the first logical packet
  // and 0x02 on the following packet. Treat reserved2 as diagnostic only:
  // completeness is determined from accumulated bytes, declared mediaSize and
  // the CRC of the complete media object.
  bool needBegin=!alarmUpload.active;
  if(alarmUpload.active){
    const bool sameTransaction = alarmUpload.slot==slot &&
      alarmUploadMetadataMatches(alarmUpload.candidate,candidate);

    // A repeated first chunk is most safely interpreted as a restarted app
    // transfer rather than appended as duplicate media.
    if(candidate.reserved2==0x00 && alarmUpload.received>0){
      abortAlarmUpload("transfer restarted");
      needBegin=true;
    } else if(!sameTransaction){
      if(candidate.reserved2==0x00){
        abortAlarmUpload("new transaction");
        needBegin=true;
      } else {
#if DEBUG_SERIAL
        Serial.print("ALARM ERROR: unexpected continuation slot="); Serial.print(slot);
        Serial.print(" part=0x"); Serial.print(candidate.reserved2,HEX);
        Serial.print(" chunk="); Serial.print(chunkSize);
        Serial.print(" media="); Serial.println(candidate.mediaSize);
#endif
        sendCommandAck(0x00,0x80); return true;
      }
    }
  }

  if(needBegin){
    // A partial packet marked as a continuation cannot be reconstructed if its
    // first chunk was not received. A one-packet media update is accepted
    // regardless of the reserved2 value because no continuation is required.
    if(candidate.reserved2!=0x00 && chunkSize<candidate.mediaSize){
#if DEBUG_SERIAL
      Serial.print("ALARM ERROR: continuation without start slot="); Serial.print(slot);
      Serial.print(" part=0x"); Serial.print(candidate.reserved2,HEX);
      Serial.print(" chunk="); Serial.print(chunkSize);
      Serial.print(" media="); Serial.println(candidate.mediaSize);
#endif
      sendCommandAck(0x00,0x80); return true;
    }
    if(!beginAlarmUpload(slot,candidate)){
#if DEBUG_SERIAL
      Serial.print("ALARM ERROR: could not create staging file slot="); Serial.println(slot);
#endif
      sendCommandAck(0x00,0x80); return true;
    }
  }

  if((uint64_t)alarmUpload.received+(uint64_t)chunkSize > (uint64_t)alarmUpload.candidate.mediaSize){
#if DEBUG_SERIAL
    Serial.print("ALARM ERROR: media overflow slot="); Serial.print(slot);
    Serial.print(" received="); Serial.print(alarmUpload.received);
    Serial.print(" chunk="); Serial.print(chunkSize);
    Serial.print(" expected="); Serial.println(alarmUpload.candidate.mediaSize);
#endif
    abortAlarmUpload("media overflow");
    sendCommandAck(0x00,0x80); return true;
  }

  String tmp=alarmTempFileName(slot);
  File f=LittleFS.open(tmp,"a");
  bool writeOK=(bool)f;
  if(writeOK && (uint32_t)f.size()!=alarmUpload.received) writeOK=false;
  if(writeOK && chunkSize && f.write(media,chunkSize)!=chunkSize) writeOK=false;
  if(f)f.close();
  if(!writeOK){
#if DEBUG_SERIAL
    Serial.print("ALARM ERROR: staging write failed slot="); Serial.println(slot);
#endif
    abortAlarmUpload("staging write");
    sendCommandAck(0x00,0x80); return true;
  }

  alarmUpload.runningCRC=crc32Update(alarmUpload.runningCRC,media,chunkSize);
  alarmUpload.received+=(uint32_t)chunkSize;
  alarmUpload.lastChunkAt=millis();
  alarmUpload.candidate.reserved2=candidate.reserved2;

#if DEBUG_SERIAL
  Serial.print("ALARM RX slot="); Serial.print(slot);
  Serial.print(" part=0x"); Serial.print(candidate.reserved2,HEX);
  Serial.print(" chunk="); Serial.print(chunkSize);
  Serial.print(" total="); Serial.print(alarmUpload.received);
  Serial.print("/"); Serial.println(alarmUpload.candidate.mediaSize);
#endif

  if(alarmUpload.received<alarmUpload.candidate.mediaSize){
    sendCommandAck(0x00,0x80); return true;
  }

  const uint32_t calc=alarmUpload.runningCRC^0xFFFFFFFFUL;
  if(calc!=alarmUpload.candidate.mediaCRC){
#if DEBUG_SERIAL
    Serial.print("ALARM CRC ERROR calc=0x");Serial.print(calc,HEX);Serial.print(" expected=0x");Serial.println(alarmUpload.candidate.mediaCRC,HEX);
#endif
    abortAlarmUpload("CRC mismatch");
    sendCommandAck(0x00,0x80); return true;
  }

  AlarmSlot committed=alarmUpload.candidate;
  AlarmSlot rollback=alarmUpload.previous;
  String dst=alarmFileName(slot), bak=alarmBackupFileName(slot);
  LittleFS.remove(bak);
  bool hadOld=LittleFS.exists(dst);
  bool stored=true;
  if(hadOld) stored=LittleFS.rename(dst,bak);
  if(stored) stored=LittleFS.rename(tmp,dst);
  if(!stored){
    LittleFS.remove(tmp);
    if(hadOld && LittleFS.exists(bak) && !LittleFS.exists(dst)) LittleFS.rename(bak,dst);
  } else if(!saveAlarmMetaValue(slot,committed)){
    // Metadata did not commit: roll the filesystem and NVS back to the
    // previous slot so a failed update cannot destroy a working Alarm.
    LittleFS.remove(dst);
    if(hadOld && LittleFS.exists(bak)) LittleFS.rename(bak,dst);
    saveAlarmMetaValue(slot,rollback);
    stored=false;
  } else {
    alarms[slot]=committed;
    LittleFS.remove(bak);
  }

  // The staging path was either renamed or rolled back. Reset the in-memory
  // transaction without deleting the committed file.
  alarmUpload=AlarmUploadState();

#if DEBUG_SERIAL
  Serial.print("ALARM SAVE slot=");Serial.print(slot);Serial.print(" flags=0x");Serial.print(committed.flags,HEX);
  Serial.print(" time=");Serial.print(committed.hour);Serial.print(":");Serial.print(committed.minute);Serial.print(" dur=");Serial.print(committed.durationSec);
  Serial.print(" type=");Serial.print(committed.contentType);Serial.print(" buzzer=");Serial.print(committed.buzzer);Serial.print(" bytes=");Serial.print(committed.mediaSize);
  Serial.print(" mediaId=0x");Serial.print(committed.mediaId,HEX);Serial.print(" stored=");Serial.println(stored?"YES":"NO");
#endif
  // Preserve the compatibility ACK while the original device's error status
  // for Alarm storage failures remains unknown.
  sendCommandAck(0x00,0x80); return true;
}

// ======================================================
// GIF
// ======================================================
AnimatedGIF *gif = nullptr;
bool gifDecoderTeardownPending = false;
bool gifDecoderOpenPending = false;
uint32_t gifDecoderOpenAt = 0;
size_t gifSize = 0;
bool gifStoredOnFS = false;
bool gifEventPlaybackFileActive = false;
#if IDOTMATRIX_MEMORY_TELEMETRY
uint32_t gifTelemetryStartedAtUs = 0;
uint8_t gifTelemetryContext = 0; // 0=live, 1=event, 2=carousel
bool gifTelemetryFirstFramePending = false;

const char *gifTelemetryTag(const char *phase) {
  if (gifTelemetryContext == 2) {
    if (!strcmp(phase, "open")) return "gif.carousel.open_us";
    if (!strcmp(phase, "first")) return "gif.carousel.first_frame_us";
  } else if (gifTelemetryContext == 1) {
    if (!strcmp(phase, "open")) return "gif.event.open_us";
    if (!strcmp(phase, "first")) return "gif.event.first_frame_us";
  }
  return !strcmp(phase, "open") ? "gif.live.open_us" : "gif.live.first_frame_us";
}
#endif
// B73: RX and playback files are deliberately different.  BLE writes happen
// on Core 0 while AnimatedGIF playback runs from loop() on Core 1; touching
// the file/decoder currently in use from the BLE callback can corrupt the heap.
const char *GIF_PLAY_FILE = "/gif_play.gif";
const char *GIF_PLAY_BACKUP_FILE = "/gif_play.bak";
const char *GIF_RX_FILES[2] = { "/gif_rx0.tmp", "/gif_rx1.tmp" };
// Alarm/Schedule GIFs use a disposable PLAY copy. Their persistent source
// files remain free to participate in staging/backup/rollback transactions.
const char *EVENT_GIF_PLAY_FILE = "/event_play.gif";

uint32_t storedFileSize(const char *path) {
  if(!littleFsReady) return 0;
  File f=LittleFS.open(path,"r");
  if(!f) return 0;
  uint32_t size=(uint32_t)f.size();
  f.close();
  return size;
}

void recoverGifPlayFile() {
  if(!littleFsReady) return;
  bool havePlay=LittleFS.exists(GIF_PLAY_FILE);
  bool haveBackup=LittleFS.exists(GIF_PLAY_BACKUP_FILE);
  if(havePlay) LittleFS.remove(GIF_PLAY_BACKUP_FILE);
  else if(haveBackup) LittleFS.rename(GIF_PLAY_BACKUP_FILE,GIF_PLAY_FILE);
  // RX files belong to an interrupted BLE session and are never resumable.
  LittleFS.remove(GIF_RX_FILES[0]);
  LittleFS.remove(GIF_RX_FILES[1]);
  // Event PLAY is an ephemeral copy and is never authoritative after reboot.
  LittleFS.remove(EVENT_GIF_PLAY_FILE);
}

bool promoteGifRxToPlay(uint8_t slot) {
  if(!littleFsReady || slot>1) return false;
  const char *rxPath=GIF_RX_FILES[slot];
  if(!LittleFS.exists(rxPath)) return false;

  // Reconcile any leftover backup from an earlier interrupted promotion before
  // starting a new replacement; never discard the only known-good PLAY file.
  if(!LittleFS.exists(GIF_PLAY_FILE) && LittleFS.exists(GIF_PLAY_BACKUP_FILE)) {
    if(!LittleFS.rename(GIF_PLAY_BACKUP_FILE,GIF_PLAY_FILE)) return false;
  } else if(LittleFS.exists(GIF_PLAY_FILE) && LittleFS.exists(GIF_PLAY_BACKUP_FILE)) {
    LittleFS.remove(GIF_PLAY_BACKUP_FILE);
  }

  bool hadOld=LittleFS.exists(GIF_PLAY_FILE);
  if(hadOld && !LittleFS.rename(GIF_PLAY_FILE,GIF_PLAY_BACKUP_FILE)) return false;

  if(!LittleFS.rename(rxPath,GIF_PLAY_FILE)){
    if(hadOld && LittleFS.exists(GIF_PLAY_BACKUP_FILE))
      LittleFS.rename(GIF_PLAY_BACKUP_FILE,GIF_PLAY_FILE);
    return false;
  }

  LittleFS.remove(GIF_PLAY_BACKUP_FILE);
  return true;
}

File gifBulkFile;
size_t gifRxWriteOffset = 0;
uint8_t nextGifRxSlot = 0;
bool gifLoaded = false;
bool gifPlaying = false;
bool gifRestartPending = false;
bool gifFrameWasDrawn = false;
// A completed BLE GIF transfer is acknowledged immediately and opened later
// from loop().  AnimatedGIF/LittleFS initialization must not run inside the
// BLE write callback: complex 64x64 GIFs can otherwise corrupt/crash Core 0.
bool pendingGifStart = false;
int8_t pendingGifSlot = -1;
uint32_t pendingGifSize = 0;
uint32_t pendingGifCRC = 0;
uint32_t gifPlayCRC = 0;
uint32_t pendingGifStartAt = 0;
uint32_t gifNextFrameAt = 0;
CRGB *gifFrame = nullptr;

// ======================================================
// DEVICE-ASSET CAROUSEL
// Hardware-tested 12-slot Device Assets model. The official
// app may keep multiple 12-slot pages in its UI, but a pushed page addresses
// device slots 0..11. Command 02/01 carries a slot-setup descriptor; current
// official-app captures use count=12 followed by 0..11 even when only a subset
// of those slots receives a GIF. Bulk bytes 13-14 are the per-slot dwell
// (timeSign, LE seconds) and byte 15 is the device asset slot/imageIndex.
// Command 0A/01 enters/starts the asset view, but current official-app captures
// show it may arrive before a later page push and is not necessarily repeated
// afterwards. Command 02/01 therefore establishes persistent Assets-view intent,
// stores GIF, static IMAGE (PNG/RGB24) and observed TEXT slots without rendering them during the push,
// then starts the completed bank after a short upload-idle settle interval.
// Empty positions are skipped.
// ======================================================
struct __attribute__((packed)) CarouselSlotMeta {
  uint8_t configured = 0;
  uint8_t dataType = 0;       // 1=GIF, 2=static IMAGE (PNG or RGB24), 3=TEXT
  uint16_t dwellSeconds = 0;
  uint32_t mediaSize = 0;
  uint32_t mediaCRC = 0;
};

// Carousel bank transaction journal. The manifest is persisted before
// any declared slot is destructively cleared. Old media are rename-backed up
// (no duplicate media copy), so an interrupted Device Assets push can restore
// the complete pre-push bank on the next boot. The manifest itself is the
// commit marker: successful publication removes it before backup cleanup.
constexpr uint32_t CAROUSEL_BANK_TXN_MAGIC = 0x43425458UL; // 'CBTX'
constexpr uint16_t CAROUSEL_BANK_TXN_VERSION = 1U;
const char *CAROUSEL_BANK_TXN_FILE = "/carbank.txn";
const char *CAROUSEL_BANK_TXN_TMP_FILE = "/carbank.tmp";
struct __attribute__((packed)) CarouselBankTxnManifest {
  uint32_t magic = CAROUSEL_BANK_TXN_MAGIC;
  uint16_t version = CAROUSEL_BANK_TXN_VERSION;
  uint16_t bytes = 0;
  uint16_t touchedMask = 0;
  uint16_t mediaPresentMask = 0;
  uint8_t orderCount = 0;
  uint8_t order[CAROUSEL_SLOT_COUNT] = {};
  CarouselSlotMeta slots[CAROUSEL_SLOT_COUNT];
  uint32_t crc = 0;
};

CarouselBankTxnManifest carouselBankTxnManifest;
bool carouselBankTxnActive = false;

// Heavy 02/01 bank setup (journal write + old-media renames + slot wipe) must
// never run on NimBLE's host task. The callback captures only the tiny command
// descriptor; loopTask performs the filesystem work and emits the ACK after the
// bank is ready to receive media.
struct DeferredCarouselBankSetupState {
  bool active = false;
  bool viewIntent = false;
  uint8_t count = 0;
  uint8_t order[CAROUSEL_SLOT_COUNT] = {};
};
DeferredCarouselBankSetupState deferredCarouselBankSetup;

CarouselSlotMeta carouselSlots[CAROUSEL_SLOT_COUNT];
Preferences carouselPrefs;
bool carouselPrefsReady = false;
uint8_t carouselOrder[CAROUSEL_SLOT_COUNT] = {};
uint8_t carouselOrderCount = 0;
bool carouselUploadOpen = false;
// Emulator UX policy: while a Device Assets bank is being replaced, force the
// physical matrix black without changing the protocol-controlled screenOn state.
bool carouselUploadBlackout = false;

// Procedural Carousel transfer indicator. The app does not announce the byte
// sizes of future assets, so whole-Carousel progress is asset-weighted while
// the active asset uses its real byte fraction.
bool carouselTransferIndicatorActive = false;
bool carouselTransferIndicatorVisible = false;
uint32_t carouselTransferIndicatorStartedAt = 0;
uint32_t carouselTransferIndicatorLastFrameAt = 0;
uint32_t carouselTransferExpectedBytes = 0;
uint32_t carouselTransferReceivedBytes = 0;
uint16_t carouselUploadCompletedMask = 0;
constexpr uint32_t CAROUSEL_TRANSFER_INDICATOR_DELAY_MS = 250UL;
bool carouselEnterRequested = false;
bool carouselStartPending = false;
bool carouselActive = false;
int8_t carouselActiveSlot = -1;
uint32_t carouselSlotStartedAt = 0;
uint32_t carouselLastAssetCommitAt = 0;
bool gifCarouselPlaybackFileActive = false;

// ======================================================
// PRESET / DEFAULT (volatile mini-playlist)
// Hardware captures from the official app use device slots 14..19 and command
// 06/02 <count> <slot...>. The app re-sends all media when a preset is invoked.
// Captured assets use timeSign=5 while the original display visibly advances
// at about 3 seconds per item, so timeSign is retained as opaque metadata and
// is NOT interpreted as seconds here. Preset media is intentionally volatile.
// ======================================================
struct PresetSlotMeta {
  uint8_t configured = 0;
  uint8_t dataType = 0;       // 1=GIF/image, 3=TEXT
  uint16_t timeSign = 0;      // observed 5; semantics not established
  uint32_t mediaSize = 0;
  uint32_t mediaCRC = 0;
};

PresetSlotMeta presetSlots[PRESET_SLOT_COUNT];
uint8_t presetOrder[PRESET_SLOT_COUNT] = {}; // local slots 0..5
uint8_t presetOrderCount = 0;
bool presetActive = false;
int8_t presetActiveSlot = -1;
uint32_t presetSlotStartedAt = 0;
uint32_t presetSlotHoldMs = PRESET_DWELL_MS;
bool gifPresetPlaybackFileActive = false;
bool presetTransferIndicatorActive = false;
uint32_t presetTransferLastActivityAt = 0;
constexpr uint32_t PRESET_TRANSFER_TIMEOUT_MS = 5000UL;

// Preset/Default bank transaction. Preset media intentionally remain
// volatile across reboot, so recovery does not need a persistent journal.
// Instead, every completed object is published to a candidate namespace and
// only the 06/02 activation command makes the selected bank authoritative.
// This prevents an interrupted multi-object invocation from leaving a mixed
// old/new Preset bank in the live namespace.
PresetSlotMeta presetStaging[PRESET_SLOT_COUNT];
uint16_t presetStagedMask = 0;
bool presetBankTxnActive = false;
bool presetBankCleanupPending = false;
uint8_t presetBankCleanupReason = 0;
bool presetBankPreviousActive = false;
int8_t presetBankPreviousActiveSlot = -1;

struct DeferredPresetActivationState {
  bool active = false;
  uint8_t count = 0;
  uint8_t order[PRESET_SLOT_COUNT] = {};
};
DeferredPresetActivationState deferredPresetActivation;

// ======================================================
// PACKET / BULK
// ======================================================
uint8_t packetBuffer[MAX_PACKET_SIZE];
size_t packetReceived = 0;
size_t packetExpected = 0;
uint32_t packetLastRxMs = 0;
uint32_t bulkLastRxMs = 0;

// Audio/Rhythm FA02 is a byte stream, not a normal LE16-length packet stream.
// LEVEL frames are exactly 6 bytes. FFT frames are exactly 21 bytes and carry
// 16 wire bands. ATT writes may end in the middle of the next audio frame.
constexpr size_t AUDIO_LEVEL_FRAME_SIZE = 6U;
constexpr size_t AUDIO_FFT_FRAME_SIZE = 21U;
constexpr uint32_t AUDIO_REASSEMBLY_TIMEOUT_MS = 1000UL;
uint8_t audioRxBuffer[AUDIO_FFT_FRAME_SIZE];
size_t audioRxReceived = 0;
size_t audioRxExpected = 0;
uint32_t audioRxLastMs = 0;

static void resetAudioReassembly() {
  audioRxReceived = 0;
  audioRxExpected = 0;
  audioRxLastMs = 0;
}

struct BulkTransferState {
  bool active = false;
  uint8_t dataType = 0;
  uint32_t expectedSize = 0;
  uint32_t receivedSize = 0;
  uint32_t expectedCRC = 0;
  uint32_t runningCRC = 0xFFFFFFFF;
  uint32_t chunkCount = 0;
  bool gifToFS = false;
  bool carouselToFS = false;
  bool carouselBuffered = false;
  bool presetToFS = false;
  int8_t gifRxSlot = -1;
  int8_t carouselLocalSlot = -1;
  int8_t presetLocalSlot = -1;
  uint16_t timeSign = 0;
  uint8_t imageIndex = 0xFF;
  String format = "UNKNOWN";
} bulk;

// Final publication of Preset/Carousel files can traverse deep LittleFS and
// flash call chains (remove/rename/NVS). Never execute that work on the
// NimBLE host task: field testing hit the nimble_host stack canary
// while commitPresetSlot() was inside LittleFS.remove(). The BLE callback only
// receives/stages bytes and queues this descriptor. Arduino loop() performs
// flush/close/commit and sends the final transfer ACK afterwards.
enum DeferredAssetCommitKind : uint8_t {
  DEFERRED_ASSET_NONE = 0,
  DEFERRED_ASSET_CAROUSEL = 1,
  DEFERRED_ASSET_PRESET = 2
};

struct DeferredAssetCommitState {
  bool active = false;
  DeferredAssetCommitKind kind = DEFERRED_ASSET_NONE;
  uint8_t dataType = 0;
  uint8_t localSlot = 0xFF;
  uint16_t timeSign = 0;
  uint32_t size = 0;
  uint32_t crc = 0;
  bool integrityOk = false;
  uint32_t queuedAt = 0;
} deferredAssetCommit;

uint8_t textPayload[MAX_TEXT_PAYLOAD];
size_t textPayloadReceived = 0;

// Local TEXT viewport state. The app already sends the complete glyph stream;
// PIN/UP/DOWN page through it when the full line does not fit the matrix.
uint8_t textFirstVisibleGlyph = 0;
uint32_t textPageChangedAt = 0;

uint8_t *rawRgbData = nullptr;
size_t rawRgbWriteOffset = 0;

// Waveshare persistent Carousel assets are buffered in PSRAM on the BLE host
// task and written to LittleFS only by loopTask. Other targets retain their
// qualified streaming policy; type 2 remains buffered because it may be PNG.
uint8_t *carouselImageRxData = nullptr;
size_t carouselImageRxWriteOffset = 0;

__attribute__((noinline)) uint8_t *allocateCarouselImageRxBuffer(size_t bytes){
#if defined(IDOTMATRIX_BOARD_WAVESHARE_S3_RGB_MATRIX)
  // Persistent Carousel RX must stay off the NimBLE host task filesystem path.
  // Keep the same PSRAM reserve used by GIF staging/cache and fail cleanly rather
  // than falling back to scarce internal RAM for a potentially multi-megabyte asset.
  const size_t freePsram=heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  const size_t largestPsram=heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
  const size_t reserve=(size_t)IDOTMATRIX_GIF_PSRAM_RESERVE_BYTES;
  if(bytes>largestPsram || freePsram<=reserve || bytes>freePsram-reserve) return nullptr;
  return (uint8_t*)heap_caps_malloc(bytes,MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
  return (uint8_t*)malloc(bytes);
#endif
}

__attribute__((noinline)) bool beginCarouselBufferedImageTransfer(uint32_t total, uint8_t imageIndex, uint16_t timeSign){
  carouselImageRxData=allocateCarouselImageRxBuffer(total);
  if(!carouselImageRxData){
#if DEBUG_SERIAL
    Serial.println("BULK CAROUSEL asset rejected: RX buffer allocation failed");
#endif
    return false;
  }
  bulk.carouselLocalSlot=(int8_t)imageIndex;
  bulk.carouselBuffered=true;
  carouselImageRxWriteOffset=0;
  beginCarouselTransferIndicator(total,imageIndex);
#if DEBUG_SERIAL && BULK_PROTOCOL_DEBUG
  Serial.print("BULK BUFFERED: CAROUSEL slot="); Serial.print(imageIndex);
  Serial.print(" format="); Serial.print(bulk.format);
  Serial.print(" dwell="); Serial.println(timeSign);
#endif
  return true;
}


// Graffiti full-raster transfer used by the official 64x64 Android app.
// This is NOT the normal 16-byte Bulk protocol. Captured original hardware
// uses a 9-byte header followed by up to 4096 RGB bytes per logical packet.
struct GraffitiRasterState {
  bool active = false;
  uint32_t expectedSize = 0;
  uint32_t receivedSize = 0;
  uint32_t chunkCount = 0;
  uint32_t lastRxMs = 0;
  uint8_t *data = nullptr;
} graffitiRaster;

static void resetGraffitiRaster(){
  if(graffitiRaster.data){ free(graffitiRaster.data); graffitiRaster.data=nullptr; }
  graffitiRaster=GraffitiRasterState();
}

bool pendingDeviceInfoPush = false;
uint32_t deviceInfoPushAt = 0;
bool pendingSoftReset = false;
uint32_t softResetAt = 0;
bool pendingAdvertisingRestart = false;
uint32_t advertisingRestartAt = 0;

// ======================================================
// UTILITY
// ======================================================
void dumpHex(const uint8_t *data, size_t len) {
#if DEBUG_SERIAL
  for (size_t i = 0; i < len; i++) {
    if (data[i] < 0x10) Serial.print('0');
    Serial.print(data[i], HEX);
    if (i + 1 < len) Serial.print(' ');
  }
  Serial.println();
#endif
}

void reportHeap(const char *label) {
#if DEBUG_SERIAL
  Serial.print("[HEAP] "); Serial.print(label);
  Serial.print(": free="); Serial.print(ESP.getFreeHeap());
  Serial.print(" min="); Serial.print(ESP.getMinFreeHeap());
  Serial.print(" largest=");
  Serial.println(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
#endif
  idotMemoryTelemetrySnapshot(label);
}

uint32_t crc32Update(uint32_t crc, const uint8_t *data, size_t len) {
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (uint8_t j = 0; j < 8; j++) {
      crc = (crc & 1) ? ((crc >> 1) ^ 0xEDB88320UL) : (crc >> 1);
    }
  }
  return crc;
}

bool fileMatchesMedia(const String &path, uint32_t expectedSize, uint32_t expectedCRC) {
  if(!littleFsReady) return false;
  File f=LittleFS.open(path,"r");
  if(!f || (uint32_t)f.size()!=expectedSize){ if(f)f.close(); return false; }
  uint8_t buf[256];
  uint32_t crc=0xFFFFFFFF;
  uint32_t remaining=expectedSize;
  while(remaining){
    size_t want=remaining>sizeof(buf)?sizeof(buf):(size_t)remaining;
    size_t got=f.read(buf,want);
    if(got!=want){f.close();return false;}
    crc=crc32Update(crc,buf,got);
    remaining-=(uint32_t)got;
  }
  f.close();
  return (crc^0xFFFFFFFF)==expectedCRC;
}

uint16_t logicalIndex(uint8_t x, uint8_t y) {
  return (uint16_t)y * MATRIX_WIDTH + x;
}

uint16_t physicalXY(uint8_t x, uint8_t y) {
  if ((y & 1) == 0) return (uint16_t)y * PHYSICAL_MATRIX_WIDTH + x;
  return (uint16_t)y * PHYSICAL_MATRIX_WIDTH + PHYSICAL_MATRIX_WIDTH - 1 - x;
}

// Scale the final logical framebuffer to one physical pixel. Upscaling uses
// nearest-neighbour replication; downscaling uses a box average, matching the
// independently hardware-tested WLED Usermod policy.
CRGB logicalToPhysicalPixel(uint16_t px, uint16_t py) {
#if IDOTMATRIX_ORIENTATION_SENSOR
  // Orientation is applied after logical rendering and before physical output,
  // so every display mode rotates consistently without renderer-specific code.
  uint16_t rotatedX=px, rotatedY=py;
  idotOrientationMapOutputToSource(
    px, py, PHYSICAL_MATRIX_WIDTH, PHYSICAL_MATRIX_HEIGHT, rotatedX, rotatedY);
  px=rotatedX;
  py=rotatedY;
#endif

  const uint16_t sourceWidth = MATRIX_WIDTH;
  const uint16_t sourceHeight = MATRIX_HEIGHT;
  const uint16_t targetWidth = PHYSICAL_MATRIX_WIDTH;
  const uint16_t targetHeight = PHYSICAL_MATRIX_HEIGHT;

  if (sourceWidth == targetWidth && sourceHeight == targetHeight)
    return framebuffer[logicalIndex((uint8_t)px, (uint8_t)py)];

  const bool downscale = targetWidth < sourceWidth || targetHeight < sourceHeight;
  if (!downscale) {
    const uint16_t sx = (uint32_t)px * sourceWidth / targetWidth;
    const uint16_t sy = (uint32_t)py * sourceHeight / targetHeight;
    return framebuffer[logicalIndex((uint8_t)sx, (uint8_t)sy)];
  }

  const uint16_t sy0 = (uint32_t)py * sourceHeight / targetHeight;
  uint16_t sy1 = (uint32_t)(py + 1u) * sourceHeight / targetHeight;
  if (sy1 <= sy0) sy1 = sy0 + 1u;
  if (sy1 > sourceHeight) sy1 = sourceHeight;

  const uint16_t sx0 = (uint32_t)px * sourceWidth / targetWidth;
  uint16_t sx1 = (uint32_t)(px + 1u) * sourceWidth / targetWidth;
  if (sx1 <= sx0) sx1 = sx0 + 1u;
  if (sx1 > sourceWidth) sx1 = sourceWidth;

  uint32_t r=0, g=0, b=0, samples=0;
  for (uint16_t sy=sy0; sy<sy1; ++sy) {
    for (uint16_t sx=sx0; sx<sx1; ++sx) {
      const CRGB &c=framebuffer[logicalIndex((uint8_t)sx,(uint8_t)sy)];
      r += c.r; g += c.g; b += c.b; ++samples;
    }
  }
  if (!samples) samples=1;
  return CRGB((uint8_t)(r/samples),(uint8_t)(g/samples),(uint8_t)(b/samples));
}

void clearFramebuffer(const CRGB &color) {
  fill_solid(framebuffer, NUM_LEDS, color);
}

void putPixel(int16_t x, int16_t y, const CRGB &c) {
  if (x < 0 || x >= MATRIX_WIDTH || y < 0 || y >= MATRIX_HEIGHT) return;
  framebuffer[logicalIndex((uint8_t)x, (uint8_t)y)] = c;
}

// Several hand-tuned clock/audio/scoreboard renderers were reconstructed on a
// 16x16 reference canvas. Preserve their appearance on larger logical panels by
// scaling that 16x16 artwork after it has been rendered. App-supplied media and
// graffiti bypass this helper and therefore remain truly native-resolution.
void scaleLegacy16CanvasToLogical() {
  if (MATRIX_WIDTH == 16 && MATRIX_HEIGHT == 16) return;
  CRGB base[16 * 16];
  for (uint8_t y=0; y<16; y++)
    for (uint8_t x=0; x<16; x++)
      base[(uint16_t)y*16+x] = (x<MATRIX_WIDTH && y<MATRIX_HEIGHT) ? framebuffer[logicalIndex(x,y)] : CRGB::Black;

  for (uint8_t y=0; y<MATRIX_HEIGHT; y++) {
    const uint8_t sy=(uint16_t)y*16U/MATRIX_HEIGHT;
    for (uint8_t x=0; x<MATRIX_WIDTH; x++) {
      const uint8_t sx=(uint16_t)x*16U/MATRIX_WIDTH;
      framebuffer[logicalIndex(x,y)] = base[(uint16_t)sy*16+sx];
    }
  }
}

void setStatusLed(bool on) {
#if STATUS_LED_PIN >= 0
  digitalWrite(STATUS_LED_PIN, on ? HIGH : LOW);
#else
  (void)on;
#endif
}

void getCurrentTime(uint8_t &h, uint8_t &m, uint8_t &s) {
#if IDOTMATRIX_RTC_AVAILABLE
  if (rtcReady && rtcTimeValid) {
    IDotMatrixRtcDateTime now;
    if (rtc.read(now)) {
      h = now.hour; m = now.minute; s = now.second;
      return;
    }
    rtcTimeValid=false;
    rtcReady=false;
    rtcRetryNextAt=millis() + IDOTMATRIX_RTC_RETRY_INTERVAL_MS;
  }
#endif
  if (!clockSynced) { h = m = s = 0; return; }
  uint64_t elapsed = (monotonicMillis64() - syncMillis) / 1000ULL;
  uint64_t total = ((uint64_t)syncHour * 3600ULL + (uint64_t)syncMinute * 60ULL + syncSecond + elapsed) % 86400ULL;
  h = total / 3600UL;
  total %= 3600UL;
  m = total / 60UL;
  s = total % 60UL;
}

bool isEnergySavingActive() {
  if (!energySaving.enabled) return false;
  uint8_t h=0, m=0, s=0;
#if IDOTMATRIX_RTC_AVAILABLE
  if (rtcReady && rtcTimeValid) {
    IDotMatrixRtcDateTime now;
    if (!rtc.read(now)) {
      rtcTimeValid=false;
      rtcReady=false;
      rtcRetryNextAt=millis() + IDOTMATRIX_RTC_RETRY_INTERVAL_MS;
      return false;
    }
    h=now.hour; m=now.minute; s=now.second;
  } else if (clockSynced) {
    getCurrentTime(h, m, s);
  } else return false;
#else
  if (!clockSynced) return false;
  getCurrentTime(h, m, s);
#endif
  uint16_t now = (uint16_t)h * 60 + m;
  uint16_t start = (uint16_t)energySaving.startHour * 60 + energySaving.startMinute;
  uint16_t end = (uint16_t)energySaving.endHour * 60 + energySaving.endMinute;
  if (start == end) return false;
  return (start < end) ? (now >= start && now < end) : (now >= start || now < end);
}

uint8_t effectiveBrightnessPercent() {
  uint8_t v = brightnessPercent;
  if (isEnergySavingActive()) {
    uint8_t r = min((uint8_t)100, energySaving.reductionPercent);
#if defined(IDOTMATRIX_BOARD_CLOCKWISE)
    v = min(v, r);
#else
    v = ((uint16_t)v * (100 - r)) / 100;
#endif
  }
  return v;
}

uint8_t brightnessToOutput(uint8_t percent) {
  percent = min((uint8_t)100, percent);
  return ((uint16_t)percent * MAX_LED_BRIGHTNESS) / 100;
}

CRGB scaleOutputBrightness(const CRGB &input, uint8_t level) {
  return CRGB(
    (uint8_t)(((uint16_t)input.r * level) / 255U),
    (uint8_t)(((uint16_t)input.g * level) / 255U),
    (uint8_t)(((uint16_t)input.b * level) / 255U)
  );
}

void clearPhysicalDisplay() {
#if DISPLAY_BACKEND == DISPLAY_BACKEND_WS2812
  FastLED.clear();
  FastLED.show();
#elif DISPLAY_BACKEND == DISPLAY_BACKEND_HUB75
  if (hub75Matrix) {
    hub75Matrix->clearScreen();
  }
#endif
}

void refreshMatrix() {
  if (!screenOn || (carouselUploadBlackout && !carouselTransferIndicatorVisible)) {
    clearPhysicalDisplay();
    lastAppliedOutputBrightness = 0xFF;
    return;
  }

  const uint8_t outputBrightness=brightnessToOutput(effectiveBrightnessPercent());

#if DISPLAY_BACKEND == DISPLAY_BACKEND_WS2812
  FastLED.clear();
  for (uint16_t py=0; py<PHYSICAL_MATRIX_HEIGHT; ++py) {
    for (uint16_t px=0; px<PHYSICAL_MATRIX_WIDTH; ++px) {
      uint16_t outX=px, outY=py;
#if MATRIX_MIRROR_X
      outX=PHYSICAL_MATRIX_WIDTH-1-outX;
#endif
      if (flipped180) {
        outX=PHYSICAL_MATRIX_WIDTH-1-outX;
        outY=PHYSICAL_MATRIX_HEIGHT-1-outY;
      }
      leds[physicalXY((uint8_t)outX,(uint8_t)outY)] = logicalToPhysicalPixel(px,py);
    }
  }
  FastLED.setBrightness(outputBrightness);
  FastLED.show();
#elif DISPLAY_BACKEND == DISPLAY_BACKEND_HUB75
  if (!hub75Matrix) return;
  // MatrixPanel_I2S_DMA controls panel brightness through the output-enable
  // timing. Do not scale RGB channels here: doing so destroys gradient
  // resolution before the driver's 8-bit bitplanes are generated.
  if (outputBrightness != lastAppliedOutputBrightness)
    hub75Matrix->setBrightness8(outputBrightness);
  for (uint16_t py=0; py<PHYSICAL_MATRIX_HEIGHT; ++py) {
    for (uint16_t px=0; px<PHYSICAL_MATRIX_WIDTH; ++px) {
      uint16_t outX=px, outY=py;
      if (flipped180) {
        outX=PHYSICAL_MATRIX_WIDTH-1-outX;
        outY=PHYSICAL_MATRIX_HEIGHT-1-outY;
      }
      const CRGB c=logicalToPhysicalPixel(px,py);
      hub75Matrix->drawPixelRGB888(outX,outY,c.r,c.g,c.b);
    }
  }
#else
  #error "Unsupported DISPLAY_BACKEND"
#endif

  lastAppliedOutputBrightness=outputBrightness;
}

void updateEnergySavingOutput(uint32_t now) {
  // ECO is a local emulator policy inferred from the app command. Check once
  // per second so static images also change brightness at interval boundaries.
  if ((uint32_t)(now-lastEnergySavingCheckMs) < 1000UL) return;
  lastEnergySavingCheckMs=now;
  if (!screenOn) return;
  uint8_t wanted=brightnessToOutput(effectiveBrightnessPercent());
  if (wanted != lastAppliedOutputBrightness) refreshMatrix();
}

// ======================================================
// CAROUSEL TRANSFER INDICATOR
// ======================================================
int8_t inferCarouselRawUploadSlot() {
  if(!carouselUploadOpen || !carouselBankTxnActive || !carouselOrderCount) return -1;
  int8_t lastCommittedOrdinal=-1;
  for(uint8_t i=0;i<carouselOrderCount;i++){
    const uint8_t slot=carouselOrder[i];
    if(slot<CAROUSEL_SLOT_COUNT && (carouselUploadCompletedMask&(uint16_t(1U)<<slot)))
      lastCommittedOrdinal=(int8_t)i;
  }
  for(uint8_t i=(uint8_t)(lastCommittedOrdinal+1); i<carouselOrderCount; i++){
    const uint8_t slot=carouselOrder[i];
    if(slot>=CAROUSEL_SLOT_COUNT) continue;
    if(!(carouselUploadCompletedMask&(uint16_t(1U)<<slot))) return (int8_t)slot;
  }
  return -1;
}

uint8_t carouselCompletedUploadCountExcluding(uint8_t activeSlot) {
  uint16_t mask=carouselUploadCompletedMask;
  if(activeSlot<CAROUSEL_SLOT_COUNT) mask &= (uint16_t)~(uint16_t(1U)<<activeSlot);
  uint8_t count=0;
  for(uint8_t i=0;i<CAROUSEL_SLOT_COUNT;i++) if(mask&(uint16_t(1U)<<i)) count++;
  return count;
}

void beginCarouselTransferIndicator(uint32_t totalBytes, uint8_t activeSlot) {
  const bool first=!carouselTransferIndicatorActive;
  carouselTransferIndicatorActive=true;
  if(first){
    carouselTransferIndicatorStartedAt=millis();
    carouselTransferIndicatorLastFrameAt=0;
    carouselTransferIndicatorVisible=false;
    clearFramebuffer();
    clearPhysicalDisplay();
  }
  carouselTransferExpectedBytes=totalBytes;
  carouselTransferReceivedBytes=0;
  (void)activeSlot;
}

void updateCarouselTransferIndicatorProgress(uint32_t receivedBytes, uint32_t totalBytes) {
  if(!carouselTransferIndicatorActive) return;
  carouselTransferReceivedBytes=receivedBytes;
  if(totalBytes) carouselTransferExpectedBytes=totalBytes;
}

void endCarouselTransferIndicator() {
  carouselTransferIndicatorActive=false;
  carouselTransferIndicatorVisible=false;
  carouselTransferExpectedBytes=0;
  carouselTransferReceivedBytes=0;
  carouselTransferIndicatorLastFrameAt=0;
}

void renderCarouselTransferIndicator(uint32_t now) {
  if(!carouselTransferIndicatorActive) return;
  if((uint32_t)(now-carouselTransferIndicatorStartedAt)<CAROUSEL_TRANSFER_INDICATOR_DELAY_MS){
    carouselTransferIndicatorVisible=false;
    return;
  }
  if(carouselTransferIndicatorLastFrameAt &&
     (uint32_t)(now-carouselTransferIndicatorLastFrameAt)<50UL) return;
  carouselTransferIndicatorLastFrameAt=now;
  carouselTransferIndicatorVisible=true;
  clearFramebuffer();

  const uint8_t w=MATRIX_WIDTH, h=MATRIX_HEIGHT;
  if(w==0 || h==0) return;

  auto px=[&](int16_t x,int16_t y,uint8_t r,uint8_t g,uint8_t b){
    if(x>=0 && y>=0 && x<w && y<h) putPixel((uint8_t)x,(uint8_t)y,CRGB(r,g,b));
  };

  // Current WLED-aligned Carousel transfer artwork.
  // 16x16/32x32 retain the validated compact icon; 64x64 uses a dedicated
  // native drawing rather than a blocky 4x enlargement.  The activity bar is
  // intentionally indeterminate: the app does not announce the size/count of
  // future assets, so a determinate whole-session percentage would be false.
  if(w>=64 && h>=64){
    auto fillRect=[&](int16_t x0,int16_t y0,int16_t x1,int16_t y1,
                      uint8_t r,uint8_t g,uint8_t b){
      for(int16_t y=y0;y<=y1;y++) for(int16_t x=x0;x<=x1;x++) px(x,y,r,g,b);
    };
    auto hLine=[&](int16_t x0,int16_t x1,int16_t y,
                   uint8_t r,uint8_t g,uint8_t b){
      for(int16_t x=x0;x<=x1;x++) px(x,y,r,g,b);
    };

    constexpr uint8_t blueDkR=10, blueDkG=62, blueDkB=150;
    constexpr uint8_t blueR=25, blueG=112, blueB=235;
    constexpr uint8_t blueHiR=64, blueHiG=176, blueHiB=255;
    fillRect(11,42,14,49,blueDkR,blueDkG,blueDkB);
    fillRect(49,42,52,49,blueDkR,blueDkG,blueDkB);
    fillRect(14,48,49,51,blueDkR,blueDkG,blueDkB);
    fillRect(12,40,15,47,blueR,blueG,blueB);
    fillRect(48,40,51,47,blueR,blueG,blueB);
    fillRect(15,47,48,50,blueR,blueG,blueB);
    fillRect(13,39,15,41,blueR,blueG,blueB);
    fillRect(48,39,50,41,blueR,blueG,blueB);
    hLine(16,47,47,blueHiR,blueHiG,blueHiB);
    px(12,41,blueHiR,blueHiG,blueHiB);
    px(51,41,blueHiR,blueHiG,blueHiB);

    constexpr uint8_t redDkR=150, redDkG=8, redDkB=18;
    constexpr uint8_t redR=238, redG=30, redB=42;
    constexpr uint8_t redHiR=255, redHiG=92, redHiB=88;
    const uint8_t phase=(uint8_t)((now/80U)%12U);
    const int16_t top=(int16_t)(4+phase);
    fillRect(29,top,34,top+13,redDkR,redDkG,redDkB);
    fillRect(30,top,33,top+13,redR,redG,redB);
    hLine(31,32,top,redHiR,redHiG,redHiB);
    hLine(23,40,top+13,redDkR,redDkG,redDkB);
    hLine(24,39,top+14,redR,redG,redB);
    hLine(25,38,top+15,redR,redG,redB);
    hLine(26,37,top+16,redR,redG,redB);
    hLine(27,36,top+17,redR,redG,redB);
    hLine(28,35,top+18,redR,redG,redB);
    hLine(29,34,top+19,redR,redG,redB);
    hLine(30,33,top+20,redR,redG,redB);
    hLine(31,32,top+21,redHiR,redHiG,redHiB);

    constexpr uint8_t barR=60, barG=220, barB=150;
    constexpr uint8_t dimR=12, dimG=36, dimB=28;
    constexpr int16_t barX0=8, barX1=55, sweepW=12;
    for(int16_t x=barX0;x<=barX1;x++){
      px(x,59,dimR,dimG,dimB);
      px(x,60,dimR,dimG,dimB);
    }
    constexpr int16_t travel=(barX1-barX0+1)-sweepW;
    constexpr uint16_t stepMs=45U;
    const uint16_t cycle=(uint16_t)(travel*2);
    const uint16_t raw=cycle ? (uint16_t)((now/stepMs)%cycle) : 0U;
    const int16_t pos=raw<=travel ? (int16_t)raw : (int16_t)(cycle-raw);
    for(int16_t x=0;x<sweepW;x++){
      px((int16_t)(barX0+pos+x),59,barR,barG,barB);
      px((int16_t)(barX0+pos+x),60,barR,barG,barB);
    }
  }else{
    const uint8_t scale=(w>=32 && h>=32)?2U:1U;
    const int16_t artW=(int16_t)(16U*scale);
    const int16_t artH=(int16_t)(16U*scale);
    const int16_t ox=((int16_t)w-artW)/2;
    const int16_t oy=((int16_t)h-artH)/2;

    auto logicalPixel=[&](uint8_t x,uint8_t y,uint8_t r,uint8_t g,uint8_t b){
      for(uint8_t yy=0;yy<scale;yy++) for(uint8_t xx=0;xx<scale;xx++)
        px(ox+(int16_t)(x*scale+xx),oy+(int16_t)(y*scale+yy),r,g,b);
    };

    constexpr uint8_t trayR=35, trayG=125, trayB=235;
    for(uint8_t y=9;y<=11;y++){
      logicalPixel(2,y,trayR,trayG,trayB);
      logicalPixel(3,y,trayR,trayG,trayB);
      logicalPixel(12,y,trayR,trayG,trayB);
      logicalPixel(13,y,trayR,trayG,trayB);
    }
    for(uint8_t x=2;x<=13;x++) logicalPixel(x,12,trayR,trayG,trayB);
    for(uint8_t x=3;x<=12;x++) logicalPixel(x,13,trayR,trayG,trayB);

    constexpr uint8_t arrowR=236, arrowG=28, arrowB=36;
    const uint8_t phase=(uint8_t)((now/100U)%8U);
    const int8_t top=(int8_t)(-4+phase);
    auto arrowRow=[&](int8_t localY,uint8_t x0,uint8_t x1){
      const int8_t y=(int8_t)(top+localY);
      if(y<0 || y>=15) return;
      for(uint8_t x=x0;x<=x1;x++) logicalPixel(x,(uint8_t)y,arrowR,arrowG,arrowB);
    };
    arrowRow(0,7,9);
    arrowRow(1,7,9);
    arrowRow(2,7,9);
    arrowRow(3,7,9);
    arrowRow(4,5,11);
    arrowRow(5,6,10);
    arrowRow(6,7,9);
    arrowRow(7,8,8);

    constexpr uint8_t barY=15, barX=1, barW=14, sweepW=4;
    constexpr uint8_t barR=60, barG=220, barB=150;
    constexpr uint8_t dimR=12, dimG=36, dimB=28;
    for(uint8_t x=0;x<barW;x++) logicalPixel((uint8_t)(barX+x),barY,dimR,dimG,dimB);
    constexpr uint8_t travel=barW-sweepW;
    constexpr uint16_t stepMs=90U;
    const uint8_t cycle=(uint8_t)(travel*2U);
    const uint8_t raw=cycle ? (uint8_t)((now/stepMs)%cycle) : 0U;
    const uint8_t pos=raw<=travel ? raw : (uint8_t)(cycle-raw);
    for(uint8_t x=0;x<sweepW;x++)
      logicalPixel((uint8_t)(barX+pos+x),barY,barR,barG,barB);
  }

  refreshMatrix();
}

// ======================================================
// BLE TX
// ======================================================
void sendFA03(const uint8_t *data, size_t len) {
  if (!deviceConnected || !fa03) return;
  fa03->setValue(data, len);
#if defined(IDOTMATRIX_BOARD_CLOCKWISE)
  const bool sent = fa03->notify();
  if (len == 9 && data[2] == 1 && data[3] == 0x80)
    Serial.printf("APP FA03 device info notify=%u mcu=%u.%02u type=%u\n", unsigned(sent), data[4], data[5], data[7]);
#else
  fa03->notify();
#endif
#if DEBUG_SERIAL && DEBUG_SERIAL_VERBOSE
  Serial.print("TX FA03 ["); Serial.print(len); Serial.print("]: "); dumpHex(data, len);
#endif
}

void sendCommandStatus(uint8_t cmd, uint8_t sub, uint8_t status) {
  uint8_t r[] = {0x05,0x00,cmd,sub,status};
  sendFA03(r, sizeof(r));
}
void sendCommandAck(uint8_t cmd, uint8_t sub) { sendCommandStatus(cmd, sub, 0x01); }
void sendTransferAck(uint8_t type, uint8_t status) { sendCommandStatus(type, 0x00, status); }

#if defined(IDOTMATRIX_BOARD_CLOCKWISE)
// Report the 64x64 protocol compatibility version expected by the app.
constexpr uint8_t deviceInfo[] = {0x09,0x00,0x01,0x80,5,11,0x01,IDOTMATRIX_SCREEN_TYPE,0x00};
#else
constexpr uint8_t deviceInfo[] = {0x09,0x00,0x01,0x80,FW_RELEASE_MAJOR,FW_RELEASE_MINOR,0x01,IDOTMATRIX_SCREEN_TYPE,0x00};
#endif
static constexpr uint32_t noDisplayPin = UINT32_MAX;
static uint32_t displayPin = noDisplayPin;
static bool displayPinVerified = false;

static bool saveDisplayPin(uint32_t value) {
  Preferences prefs;
  if (!prefs.begin("idm-auth", false)) return false;
  const bool saved = prefs.putUInt("pin", value) == sizeof(value);
  prefs.end();
  if (saved) displayPin = value;
  return saved;
}

static void loadDisplayPin() {
  Preferences prefs;
  if (!prefs.begin("idm-auth", true)) return;
  const uint32_t saved = prefs.getUInt("pin", noDisplayPin);
  prefs.end();
  if (saved <= 999999) displayPin = saved;
}

static uint32_t decodeDisplayPin(const uint8_t* pairs) {
  if (pairs[0] > 99 || pairs[1] > 99 || pairs[2] > 99) return noDisplayPin;
  return uint32_t(pairs[0]) * 10000 + uint32_t(pairs[1]) * 100 + pairs[2];
}

void sendDeviceInfo() {
  uint8_t r[sizeof(deviceInfo)];
  memcpy(r, deviceInfo, sizeof(r));
  r[8] = displayPin != noDisplayPin;
#if DEVICE_INFO_PROTOCOL_DEBUG
  Serial.print("DEVICE INFO TX: release="); Serial.print(FW_RELEASE);
  Serial.print(" build="); Serial.print(FW_BUILD);
  Serial.print(" screenType="); Serial.print(IDOTMATRIX_SCREEN_TYPE);
  Serial.print(" raw="); dumpHex(r, sizeof(r));
  Serial.println("DEVICE INFO NOTE: app MCU field is encoded by the 9-byte FA03 Device Info response");
#endif
  sendFA03(r, sizeof(r));
}

// ======================================================
// DISPLAY MODE
// ======================================================
void stopGIFPlayback();
void switchDisplayMode(DisplayMode m) {
  if (presetActive) stopPresetPlayback();
  if (displayMode == DISPLAY_GIF && m != DISPLAY_GIF) stopGIFPlayback();
  if (m != DISPLAY_GIF) {
    carouselActive = false;
    carouselActiveSlot = -1;
    carouselStartPending = false;
    carouselEnterRequested = false;
    carouselUploadOpen = false;
    carouselUploadBlackout = false;
    endCarouselTransferIndicator();
    gifCarouselPlaybackFileActive = false;
  }
  if (m == DISPLAY_CLOCK && displayMode != DISPLAY_CLOCK) clockCycleStartedAt = millis();
  displayMode = m;
}

// ======================================================
// FONT 3x5
// ======================================================
const uint8_t digits3x5[10][5] = {
  {0b111,0b101,0b101,0b101,0b111},
  {0b010,0b110,0b010,0b010,0b111},
  {0b111,0b001,0b111,0b100,0b111},
  {0b111,0b001,0b111,0b001,0b111},
  {0b101,0b101,0b111,0b001,0b001},
  {0b111,0b100,0b111,0b001,0b111},
  {0b111,0b100,0b111,0b101,0b111},
  {0b111,0b001,0b010,0b010,0b010},
  {0b111,0b101,0b111,0b101,0b111},
  {0b111,0b101,0b111,0b001,0b111}
};

void drawDigit3x5(uint8_t d, int16_t x, int16_t y, const CRGB &c) {
  if (d > 9) return;
  for (uint8_t row = 0; row < 5; row++)
    for (uint8_t col = 0; col < 3; col++)
      if (digits3x5[d][row] & (1 << (2-col))) putPixel(x+col, y+row, c);
}

bool clockRenderingDate = false;
bool clockColonVisible = true;

void drawColon(int16_t x, int16_t y, const CRGB &c) {
  // Preserve the selected clock renderer/effect exactly. During the 5-second
  // date phase only the separator changes from ':' to '/'.
  if (clockRenderingDate) {
    putPixel(x+1,y,c);
    putPixel(x+1,y+1,c);
    putPixel(x,y+2,c);
    putPixel(x,y+3,c);
    return;
  }
  if (!clockColonVisible) return;
  putPixel(x,y+1,c);
  putPixel(x,y+3,c);
}

void drawTimeTwoRows(uint8_t h, uint8_t m, const CRGB &hc, const CRGB &mc, bool leftColon=false, int8_t xShift=0, int8_t colonShift=0) {
  drawDigit3x5(h/10, 4 + xShift, 2, hc);
  drawDigit3x5(h%10, 8 + xShift, 2, hc);
  drawDigit3x5(m/10, 4 + xShift, 9, mc);
  drawDigit3x5(m%10, 8 + xShift, 9, mc);
  // For the date keep '/' exactly where it was; for time move ':' with the digits.
  drawColon((leftColon ? 2 : 12) + (clockRenderingDate ? 0 : xShift + colonShift), 9, mc);
}

// ======================================================
// CLOCK STYLES - reconstructed from official-app visuals and hardware comparison.
// 0 rainbow frame + digits in the selected color
// 1 Christmas: red digits + green tree
// 2 racing/checker: cyan/magenta frame, orange digits
// 3 selected-color background, black digits
// 4 hourglass: selected-color digits + orange/white hourglass
// 5 cyan/blue frame + orange digits
// 6 cyan/blue frame + digits in the selected color
// 7 RGBY quadrant frame + digits in the selected color
// ======================================================
void drawRainbowBorder() {
  uint8_t hue = (millis()/20) & 0xFF;
  for (uint8_t x=0; x<16; x++) {
    putPixel(x,0,CHSV(hue+x*10,255,255));
    putPixel(15-x,15,CHSV(hue+160+x*10,255,255));
  }
  for (uint8_t y=1; y<15; y++) {
    putPixel(0,y,CHSV(hue+40+y*10,255,255));
    putPixel(15,15-y,CHSV(hue+200+y*10,255,255));
  }
}

void drawChristmasTree() {
  // 6x7 tree in the lower-left area, matching the observed layout.
  CRGB green(0,180,20), darkGreen(0,100,0), yellow(255,210,0), red(255,0,0), magenta(255,0,150);
  putPixel(2,7,yellow);
  putPixel(1,8,green); putPixel(2,8,green); putPixel(3,8,green);
  for (int x=1;x<=3;x++) putPixel(x,9,green);
  for (int x=0;x<=4;x++) putPixel(x,10,green);
  for (int x=0;x<=4;x++) putPixel(x,11,green);
  for (int x=0;x<=5;x++) putPixel(x,12,green);
  for (int x=0;x<=5;x++) putPixel(x,13,darkGreen);
  putPixel(2,14,CRGB(90,45,0)); putPixel(3,14,CRGB(90,45,0));
  putPixel(1,10,red); putPixel(3,11,magenta); putPixel(2,12,yellow); putPixel(4,12,red);
}

void drawCheckerRows() {
  // Observed style: three solid horizontal bands at the top and bottom.
  // Outer = cyan, middle = violet, inner = fuchsia.
  // The borders are not segmented/checkered: each row is continuous.
  const CRGB cyan(0,255,255);
  const CRGB violet(145,0,255);
  const CRGB fuchsia(255,0,170);

  for (uint8_t x=0; x<16; x++) {
    // top
    putPixel(x,0,cyan);
    putPixel(x,1,violet);
    putPixel(x,2,fuchsia);

    // mirrored bottom
    putPixel(x,13,fuchsia);
    putPixel(x,14,violet);
    putPixel(x,15,cyan);
  }
}

void drawFrameStyle5(bool cornerBlocks) {
  CRGB cyan(0,255,255), blue(0,70,255);

  if (cornerBlocks) {
    // Previous style: cyan frame with blue blocks at the corners.
    for (uint8_t x=2;x<=13;x++) { putPixel(x,1,cyan); putPixel(x,14,cyan); }
    for (uint8_t y=2;y<=13;y++) { putPixel(1,y,cyan); putPixel(14,y,cyan); }
    for (uint8_t y=0;y<3;y++) for (uint8_t x=0;x<3;x++) {
      putPixel(x,y,blue); putPixel(15-x,y,blue); putPixel(x,15-y,blue); putPixel(15-x,15-y,blue);
    }
  } else {
    // TWO continuous frames, each exactly one pixel wide.
    // Pure-blue outer frame and pure-cyan inner frame.
    const CRGB outerBlue(0,0,255);
    const CRGB innerCyan(0,255,255);

    for (uint8_t x=0;x<16;x++) {
      putPixel(x,0,outerBlue);
      putPixel(x,15,outerBlue);
    }
    for (uint8_t y=0;y<16;y++) {
      putPixel(0,y,outerBlue);
      putPixel(15,y,outerBlue);
    }

    for (uint8_t x=1;x<15;x++) {
      putPixel(x,1,innerCyan);
      putPixel(x,14,innerCyan);
    }
    for (uint8_t y=1;y<15;y++) {
      putPixel(1,y,innerCyan);
      putPixel(14,y,innerCyan);
    }
  }
}

void drawQuadrantBorder() {
  CRGB red(255,30,20), yellow(255,255,40), green(70,255,50), blue(30,80,255);
  for (uint8_t x=0;x<8;x++) putPixel(x,0,red);
  for (uint8_t x=8;x<16;x++) putPixel(x,0,yellow);
  for (uint8_t y=0;y<8;y++) putPixel(0,y,red);
  for (uint8_t y=8;y<16;y++) putPixel(0,y,blue);
  for (uint8_t y=0;y<8;y++) putPixel(15,y,yellow);
  for (uint8_t y=8;y<16;y++) putPixel(15,y,green);
  for (uint8_t x=0;x<8;x++) putPixel(x,15,blue);
  for (uint8_t x=8;x<16;x++) putPixel(x,15,green);
}

void drawHourglassIcon() {
  CRGB orange(255,155,0), sand(255,220,80), white(255,255,255);
  // 5x7 icon on the left, matching the observed position.
  for (uint8_t x=0;x<=4;x++) { putPixel(x,8,orange); putPixel(x,14,orange); }
  putPixel(0,9,orange); putPixel(4,9,orange);
  putPixel(1,10,orange); putPixel(3,10,orange);
  putPixel(2,11,white);
  putPixel(1,12,white); putPixel(2,12,white); putPixel(3,12,white);
  putPixel(0,13,sand); putPixel(1,13,sand); putPixel(2,13,sand); putPixel(3,13,sand); putPixel(4,13,sand);
}


void drawDigit3x5NativeScaled(uint8_t d, int16_t x, int16_t y, uint8_t scale, const CRGB &c) {
  if (d > 9) return;
  for (uint8_t row=0; row<5; row++) {
    for (uint8_t col=0; col<3; col++) {
      if (!(digits3x5[d][row] & (1 << (2-col)))) continue;
      for (uint8_t yy=0; yy<scale; yy++)
        for (uint8_t xx=0; xx<scale; xx++)
          putPixel(x + col*scale + xx, y + row*scale + yy, c);
    }
  }
}

void drawNative64ClockRow(uint8_t a, uint8_t b, uint8_t c, uint8_t d,
                          int16_t y, const CRGB &color, bool slash) {
  constexpr uint8_t scale=3;
  constexpr int16_t width=51;
  const int16_t x0=(64-width)/2;
  drawDigit3x5NativeScaled(a,x0,y,scale,color);
  drawDigit3x5NativeScaled(b,x0+12,y,scale,color);
  const int16_t sx=x0+24;
  if(slash){
    for(uint8_t i=0;i<3;i++){
      putPixel(sx+2-i,y+3+i,color);
      putPixel(sx+2-i,y+6+i,color);
      putPixel(sx+1-i,y+9+i,color);
    }
  } else if(clockColonVisible) {
    for(uint8_t yy=0;yy<3;yy++){
      for(uint8_t xx=0;xx<3;xx++){
        putPixel(sx+xx,y+3+yy,color);
        putPixel(sx+xx,y+9+yy,color);
      }
    }
  }
  drawDigit3x5NativeScaled(c,x0+30,y,scale,color);
  drawDigit3x5NativeScaled(d,x0+42,y,scale,color);
}

#if defined(IDOTMATRIX_BOARD_CLOCKWISE)
#include "ClockwiseClockGlyphs.h"

void drawClockwiseClockDigit(uint8_t digit, int x, int y, uint8_t style,
                             const CRGB& color) {
  if (digit > 9) return;
  const auto& glyph = clockwiseClockGlyphs[style][digit];
  for (uint8_t row = 0; row < 14; ++row)
    for (uint8_t col = 0; col < 10; ++col)
      if (glyph[row] & (1U << col))
        for (uint8_t dy = 0; dy < 2; ++dy)
          for (uint8_t dx = 0; dx < 2; ++dx)
            putPixel(x + 2 * col + dx, y + 2 * row + dy, color);
}

void drawClockwiseRainbowBorder(uint32_t now) {
  constexpr uint8_t blockSize = 4;
  constexpr uint8_t last = 64 / blockSize - 1;
  constexpr uint32_t lapMs = 3000;
  // RGB samples from the Android 64x64 clock preview, in clockwise order.
  static const CRGB palette[last] = {
    {248,32,0}, {248,0,209}, {203,0,245}, {144,0,252}, {110,17,252},
    {83,49,252}, {43,108,252}, {4,186,252}, {1,241,235}, {40,248,158},
    {113,251,80}, {219,251,15}, {252,213,1}, {252,150,1}, {252,91,1},
  };
  const uint32_t cycle = sizeof(palette) / sizeof(palette[0]) * 256U;
  const uint32_t phase = (now % lapMs) * cycle / lapMs;
  constexpr int edge = MATRIX_WIDTH - 1;
  constexpr int perimeter = edge * 8;
  for (int y = 0; y <= edge; ++y) {
    for (int x = 0; x <= edge; ++x) {
      const int dx = 2 * x - edge, dy = 2 * y - edge;
      const int radius = max(abs(dx), abs(dy));
      if (radius <= edge - 2 * blockSize) continue;
      int distance;
      if (dy == -radius) distance = edge + dx * edge / radius;
      else if (dx == radius) distance = 3 * edge + dy * edge / radius;
      else if (dy == radius) distance = 5 * edge - dx * edge / radius;
      else distance = 7 * edge - dy * edge / radius;
      const uint32_t position = (uint32_t(distance) * cycle / perimeter + cycle - phase) % cycle;
      const uint8_t index = position / 256U;
      putPixel(x, y, blend(palette[index], palette[(index + 1) % last], position % 256U));
    }
  }
}
#else
void drawNative64RainbowBorder() {
  const uint8_t hue=(uint8_t)((millis()/20U)&0xFFU);
  for(uint8_t x=0;x<64;x++){
    putPixel(x,0,CHSV((uint8_t)(hue+x*3),255,255));
    putPixel(63-x,63,CHSV((uint8_t)(hue+128+x*3),255,255));
  }
  for(uint8_t y=1;y<63;y++){
    putPixel(0,y,CHSV((uint8_t)(hue+32+y*3),255,255));
    putPixel(63,63-y,CHSV((uint8_t)(hue+160+y*3),255,255));
  }
}

#endif

void renderClock64Combined(uint8_t h, uint8_t m, uint8_t d, uint8_t mo, uint8_t style) {
  CRGB displayColor = clockColor;
#if defined(IDOTMATRIX_BOARD_CLOCKWISE)
  displayColor = clockwiseClockDisplayColor(style, clockColor);
#endif
  clearFramebuffer(style == 3 ? displayColor : CRGB::Black);
#if defined(IDOTMATRIX_BOARD_CLOCKWISE)
  if (style == 0) drawClockwiseRainbowBorder(millis());
#else
  if(style==0) drawNative64RainbowBorder();
#endif
  const CRGB c = style == 3 ? CRGB::Black : displayColor;
  // WLED reference parity: the time row received the final +4 physical-pixel adjustment.
  drawNative64ClockRow(h/10,h%10,m/10,m%10,12,c,false);
  drawNative64ClockRow(d/10,d%10,mo/10,mo%10,38,c,true);
  refreshMatrix();
}

void adjustClockStyle2ColonAfterScale() {
  const uint8_t style=(uint8_t)(clockStyle & 0x07);
  if(style!=2 || clockRenderingDate || !clockColonVisible || MATRIX_WIDTH<=16) return;
  const uint8_t scale=(uint8_t)(MATRIX_WIDTH/16U);
  const int16_t shift=(MATRIX_WIDTH>=64) ? 2 : 1;
  const int16_t oldX=8*scale;
  const int16_t newX=oldX-shift;
  for(uint8_t dot=0; dot<2; dot++){
    const int16_t y=(dot==0 ? 6 : 8)*scale;
    for(uint8_t yy=0; yy<scale; yy++)
      for(uint8_t xx=0; xx<scale; xx++) putPixel(oldX+xx,y+yy,CRGB::Black);
    for(uint8_t yy=0; yy<scale; yy++)
      for(uint8_t xx=0; xx<scale; xx++) putPixel(newX+xx,y+yy,CRGB::White);
  }
}

#if defined(IDOTMATRIX_BOARD_CLOCKWISE)

void drawClockwiseCornerFrame() {
  static constexpr char top[][17] = {
    "0222000220022200",
    "2221111111111222",
    "2210000000000122",
    "2100000000000012",
    "0100000000000010",
  };
  static constexpr char bottom[][17] = {
    "2201111111111022",
    "2210000000000122",
    "0100000000000010",
  };
  const CRGB colors[] = {CRGB::Black, CRGB(1,249,252), CRGB(34,76,252)};
  for (uint8_t y=0; y<16; ++y) {
    for (uint8_t x=0; x<16; ++x) {
      const uint8_t index = y<5 ? top[y][x]-'0' :
                            y>=13 ? bottom[15-y][x]-'0' :
                            (x==0 || x==15);
      if (index) putPixel(x,y,colors[index]);
    }
  }
}
void drawClockwiseChristmasTree() {
  static const CRGB colors[] = {{0,0,0}, {14,91,1}, {106,73,1}, {252,252,3},
                                {1,252,1}, {1,168,252}, {209,72,1}, {252,2,127}};
  static constexpr char rows[][15] = {
    "00000033000000",
    "00000033000000",
    "00001111110000",
    "00001111110000",
    "00006611110000",
    "00006611110000",
    "00111111441100",
    "00111111441100",
    "00116611111100",
    "00116611111100",
    "11111111117711",
    "11111111117711",
    "11551122111111",
    "11551122111111",
    "00000022000000",
    "00000022000000"
  };
  for (uint8_t y = 0; y < 16; ++y) {
    for (uint8_t x = 0; x < 14; ++x) {
      const char sample = rows[y][x];
      if (sample == '0') continue;
      const CRGB& color = colors[sample <= '9' ? sample - '0' : sample - 'a' + 10];
      for (uint8_t dy = 0; dy < 2; ++dy)
        for (uint8_t dx = 0; dx < 2; ++dx)
          putPixel(x * 2 + dx, 32 + y * 2 + dy, color);
    }
  }
}

void drawClockwiseHourglass() {
  static const CRGB colors[] = {{0,0,0}, {252,252,252}, {252,155,5}, {253,155,2}, {252,155,1}, {252,153,2}, {252,153,1}, {5,5,5}, {5,3,2}, {5,3,1}, {2,2,2}, {2,1,1}, {1,1,1}};
  static constexpr char rows[][13] = {
    "344444444444",
    "344444444444",
    "006400112400",
    "006400112400",
    "006411006400",
    "006411006400",
    "000053440000",
    "000064440000",
    "006411006400",
    "006411006400",
    "006400112400",
    "006400112400",
    "344444334444",
    "344444444444"
  };
  for (uint8_t y = 0; y < 14; ++y) {
    for (uint8_t x = 0; x < 12; ++x) {
      const char sample = rows[y][x];
      if (sample == '0') continue;
      const CRGB& color = colors[sample <= '9' ? sample - '0' : sample - 'a' + 10];
      for (uint8_t dy = 0; dy < 2; ++dy)
        for (uint8_t dx = 0; dx < 2; ++dx)
          putPixel(x * 2 + dx, 34 + y * 2 + dy, color);
    }
  }
}

void renderClockwiseClock(uint8_t first, uint8_t second) {
  struct ClockLayout {
    uint8_t hourX, hourY, hourUnitsX, minuteX, minuteY, minuteUnitsX, separatorX, separatorY;
  };
  constexpr ClockLayout layouts[] = {
    {16,  8, 36, 16, 36, 36,  8, 40},
    {10,  2, 28, 30, 34, 48, 50,  8},
    { 2, 24, 16, 36, 24, 50, 30, 28},
    {14,  2, 38, 14, 34, 38,  6, 42},
    {10,  2, 28, 28, 34, 46, 50, 10},
    {20, 12, 36, 20, 36, 36, 12, 40},
    {20, 10, 36, 20, 34, 36, 12, 38},
    {16, 10, 36, 16, 34, 36,  8, 38},
  };
  const uint8_t style = clockStyle & 7;
  const auto& layout = layouts[style];
  const CRGB displayColor = clockwiseClockDisplayColor(style, clockColor);
  const CRGB color = style == 3 ? CRGB::Black : displayColor;
  clearFramebuffer(style == 3 ? displayColor : CRGB::Black);
  if (style == 2) {
    const CRGB colors[] = {{118,68,252}, {249,54,205}, {1,242,252}};
    for (uint8_t y = 0; y < 12; ++y) {
      for (uint8_t x = 0; x < 64; ++x) {
        const uint8_t col = (x < 32 ? x : 63 - x) / 4;
        if ((col + y / 4) % 2 != 0) continue;
        putPixel(x, y, colors[y / 4]);
        putPixel(x, 63 - y, colors[y / 4]);
      }
    }
  }
  if (style >= 5) {
    if (style == 5) drawClockwiseCornerFrame();
    if (style == 6) drawFrameStyle5(false);
    if (style == 7) drawQuadrantBorder();
    scaleLegacy16CanvasToLogical();
  }
  if (style == 1) drawClockwiseChristmasTree();
  if (style == 4) drawClockwiseHourglass();
  if (style == 0) drawClockwiseRainbowBorder(millis());
  drawClockwiseClockDigit(first / 10, layout.hourX, layout.hourY, style, color);
  drawClockwiseClockDigit(first % 10, layout.hourUnitsX, layout.hourY, style, color);
  drawClockwiseClockDigit(second / 10, layout.minuteX, layout.minuteY, style, color);
  drawClockwiseClockDigit(second % 10, layout.minuteUnitsX, layout.minuteY, style, color);
  const CRGB separator = style == 2 ? CRGB::White : color;
  if (clockRenderingDate) {
    for (uint8_t row = 0; row < 12; ++row)
      for (uint8_t dx = 0; dx < 2; ++dx)
        putPixel(layout.separatorX + 3 - row / 4 + dx, layout.separatorY + row, separator);
  } else if (clockColonVisible) {
    for (uint8_t dy = 0; dy < 4; ++dy) {
      for (uint8_t dx = 0; dx < 4; ++dx) {
        putPixel(layout.separatorX + dx, layout.separatorY + dy, separator);
        putPixel(layout.separatorX + dx, layout.separatorY + 8 + dy, separator);
      }
    }
  }
  refreshMatrix();
}
#endif

void renderClockValues(uint8_t h, uint8_t m) {
#if defined(IDOTMATRIX_BOARD_CLOCKWISE)
  renderClockwiseClock(h, m);
  return;
#endif
  clearFramebuffer();

  switch (clockStyle & 0x07) {
    case 0: { // rainbow frame, selected digits
#if !defined(IDOTMATRIX_BOARD_CLOCKWISE)
      drawRainbowBorder();
#endif
      drawTimeTwoRows(h,m,clockColor,clockColor,true,2,-1);
      break;
    }
    case 1: { // Christmas
      CRGB red(255,0,0);
      drawDigit3x5(h/10,2,1,red); drawDigit3x5(h%10,6,1,red); drawColon(clockRenderingDate ? 10 : 11,1,red);
      drawChristmasTree();
      drawDigit3x5(m/10,7,9,red); drawDigit3x5(m%10,11,9,red);
      break;
    }
    case 2: { // racing/checker
      drawCheckerRows();
      CRGB orange(255,170,0);
      drawDigit3x5(h/10,0,5,orange); drawDigit3x5(h%10,4,5,orange);
      if (clockRenderingDate) {
        putPixel(9,5,CRGB::White); putPixel(9,6,CRGB::White);
        putPixel(8,7,CRGB::White); putPixel(8,8,CRGB::White);
      } else {
        if (clockColonVisible) { putPixel(8,6,CRGB::White); putPixel(8,8,CRGB::White); }
      }
      drawDigit3x5(m/10,9,5,orange); drawDigit3x5(m%10,13,5,orange);
      break;
    }
    case 3: { // inverted blue/selected background
      clearFramebuffer(clockColor);
      CRGB black(0,0,0);
      drawTimeTwoRows(h,m,black,black,true,2,-1);
      break;
    }
    case 4: { // hourglass
      drawDigit3x5(h/10,2,1,clockColor); drawDigit3x5(h%10,6,1,clockColor); drawColon(clockRenderingDate ? 11 : 10,1,clockColor);
      drawHourglassIcon();
      drawDigit3x5(m/10,6,9,clockColor); drawDigit3x5(m%10,10,9,clockColor);
      break;
    }
    case 5: { // cyan rounded-ish frame + blue corners + orange digits
      drawFrameStyle5(true);
      CRGB orange(255,165,0);
      drawTimeTwoRows(h,m,orange,orange,true,2,0);
      break;
    }
    case 6: { // double frame: blue outer + cyan inner
      // Draw digits first and frames afterward so both borders remain
      // complete and cannot be overwritten.
      drawTimeTwoRows(h,m,clockColor,clockColor,true,2,-1);
      drawFrameStyle5(false);
      break;
    }
    case 7: { // RGBY quadrant frame + selected digits
      drawQuadrantBorder();
      drawTimeTwoRows(h,m,clockColor,clockColor,true,2,-1);
      break;
    }
  }

  scaleLegacy16CanvasToLogical();
  adjustClockStyle2ColonAfterScale();
#if defined(IDOTMATRIX_BOARD_CLOCKWISE)
  if ((clockStyle & 0x07) == 0) drawClockwiseRainbowBorder(millis());
#endif
  refreshMatrix();
}


void renderDateDDMM() {
  uint16_t y; uint8_t mo,d,h,mi,se;
  getAlarmDateTime(y,mo,d,h,mi,se);
  // Reuse the *unchanged* selected clock renderer/effect. The only
  // difference is that drawColon() renders '/' while this flag is true.
  clockRenderingDate = true;
  renderClockValues(d, mo);
  clockRenderingDate = false;
}

void renderClock() {
  uint8_t h,m,s;
  getCurrentTime(h,m,s);
  clockColonVisible = ((millis()/500UL)&1U)==0U;
  if (!clock24h) { if (h==0) h=12; else if (h>12) h-=12; }

  const uint8_t style=(uint8_t)(clockStyle & 0x07);
  if (MATRIX_WIDTH==64 && clockShowDate && (style==0 || style==3)) {
    uint16_t y; uint8_t mo,d,ah,ami,ase;
    getAlarmDateTime(y,mo,d,ah,ami,ase);
    renderClock64Combined(h,m,d,mo,style);
    return;
  }

  // 16x16 and 32x32 retain the validated alternating date behavior.
  if (clockShowDate) {
    const uint32_t phase = (millis() - clockCycleStartedAt) % 35000UL;
    if (phase >= 30000UL) { renderDateDDMM(); return; }
  }
  renderClockValues(h,m);
}

// ======================================================
// MM:SS / SCOREBOARD
// ======================================================
void renderMMSS(uint32_t sec, const CRGB &c) {
  uint8_t mm=(sec/60)%100, ss=sec%60;
  clearFramebuffer();
  drawDigit3x5(mm/10,0,5,c); drawDigit3x5(mm%10,3,5,c); drawColon(7,5,c);
  drawDigit3x5(ss/10,9,5,c); drawDigit3x5(ss%10,12,5,c);
  scaleLegacy16CanvasToLogical();
  refreshMatrix();
}

void drawCountdownTimerIcon(uint8_t phase) {
  const CRGB rim(255,145,0);
  const CRGB hand(255,45,20);
  const CRGB center(255,220,120);
  const int8_t cx=7, cy=4;

  // Legacy stopwatch timer icon retained for Stopwatch only. Countdown now
  // reproduces the original-device hourglass artwork below.
  putPixel(6,0,rim); putPixel(7,0,rim); putPixel(8,0,rim);
  putPixel(7,1,rim);
  putPixel(4,1,rim); putPixel(10,1,rim);
  putPixel(3,2,rim); putPixel(11,2,rim);
  putPixel(2,3,rim); putPixel(12,3,rim);
  putPixel(2,4,rim); putPixel(12,4,rim);
  putPixel(2,5,rim); putPixel(12,5,rim);
  putPixel(3,6,rim); putPixel(11,6,rim);
  putPixel(4,7,rim); putPixel(10,7,rim);
  putPixel(5,8,rim); putPixel(6,8,rim); putPixel(7,8,rim); putPixel(8,8,rim); putPixel(9,8,rim);

  static const int8_t hx[8]={ 7,10,11,10,7,4,3,4 };
  static const int8_t hy[8]={ 1, 2, 4, 6,7,6,4,2 };
  int8_t ex=hx[phase&7], ey=hy[phase&7];
  putPixel(cx,cy,center);
  putPixel((cx+ex)/2,(cy+ey)/2,hand);
  putPixel(ex,ey,hand);
}

// Original 16x16 countdown artwork captured from the manufacturer device.
// Palette indexes: 0=black, 1=white glass/frame, 2=orange sand, 3=brown bases.
// Only the 7x10 hourglass area is stored; MM/SS are rendered dynamically.
static const uint8_t countdownHourglassFrames[10][70] PROGMEM = {
  {3,3,3,3,3,3,3, 1,2,2,2,2,2,1, 1,2,2,2,2,2,1, 0,1,2,2,2,1,0, 0,0,1,2,1,0,0, 0,0,1,0,1,0,0, 0,1,0,0,0,1,0, 1,0,0,0,0,0,1, 1,0,0,0,0,0,1, 3,3,3,3,3,3,3},
  {3,3,3,3,3,3,3, 1,2,2,2,2,2,1, 1,2,2,2,2,2,1, 0,1,2,2,2,1,0, 0,0,1,2,1,0,0, 0,0,1,2,1,0,0, 0,1,0,0,0,1,0, 1,0,0,0,0,0,1, 1,0,0,0,0,0,1, 3,3,3,3,3,3,3},
  {3,3,3,3,3,3,3, 1,2,2,2,2,2,1, 1,2,2,2,2,2,1, 0,1,2,2,2,1,0, 0,0,1,2,1,0,0, 0,0,1,2,1,0,0, 0,1,0,2,0,1,0, 1,0,0,0,0,0,1, 1,0,0,0,0,0,1, 3,3,3,3,3,3,3},
  {3,3,3,3,3,3,3, 1,2,2,2,2,2,1, 1,2,2,2,2,2,1, 0,1,2,2,2,1,0, 0,0,1,2,1,0,0, 0,0,1,2,1,0,0, 0,1,0,2,0,1,0, 1,0,0,2,0,0,1, 1,0,0,2,0,0,1, 3,3,3,3,3,3,3},
  {3,3,3,3,3,3,3, 1,2,2,2,2,2,1, 1,2,2,2,2,2,1, 0,1,2,2,2,1,0, 0,0,1,2,1,0,0, 0,0,1,2,1,0,0, 0,1,0,2,0,1,0, 1,0,0,2,0,0,1, 1,0,2,2,2,0,1, 3,3,3,3,3,3,3},
  {3,3,3,3,3,3,3, 1,0,0,0,0,0,1, 1,2,2,2,2,2,1, 0,1,2,2,2,1,0, 0,0,1,2,1,0,0, 0,0,1,2,1,0,0, 0,1,0,2,0,1,0, 1,0,0,2,0,0,1, 1,2,2,2,2,2,1, 3,3,3,3,3,3,3},
  {3,3,3,3,3,3,3, 1,0,0,0,0,0,1, 1,2,2,2,2,2,1, 0,1,2,2,2,1,0, 0,0,1,2,1,0,0, 0,0,1,2,1,0,0, 0,1,0,2,0,1,0, 1,0,2,2,2,0,1, 1,2,2,2,2,2,1, 3,3,3,3,3,3,3},
  {3,3,3,3,3,3,3, 1,0,0,0,0,0,1, 1,0,0,0,0,0,1, 0,1,2,2,2,1,0, 0,0,1,2,1,0,0, 0,0,1,2,1,0,0, 0,1,0,2,0,1,0, 1,2,2,2,2,2,1, 1,2,2,2,2,2,1, 3,3,3,3,3,3,3},
  {3,3,3,3,3,3,3, 1,0,0,0,0,0,1, 1,0,0,0,0,0,1, 0,1,0,0,0,1,0, 0,0,1,2,1,0,0, 0,0,1,2,1,0,0, 0,1,2,2,2,1,0, 1,2,2,2,2,2,1, 1,2,2,2,2,2,1, 3,3,3,3,3,3,3},
  {3,3,3,3,3,3,3, 1,0,0,0,0,0,1, 1,0,0,0,0,0,1, 0,1,0,0,0,1,0, 0,0,1,0,1,0,0, 0,0,1,2,1,0,0, 0,1,2,2,2,1,0, 1,2,2,2,2,2,1, 1,2,2,2,2,2,1, 3,3,3,3,3,3,3}
};

void fillCountdownBlock(int16_t x, int16_t y, uint8_t scale, const CRGB &c) {
  for (uint8_t yy=0; yy<scale; yy++)
    for (uint8_t xx=0; xx<scale; xx++)
      putPixel(x+xx,y+yy,c);
}

void drawDigit3x5Scaled(uint8_t d, int16_t x, int16_t y, uint8_t scale, const CRGB &c) {
  if (d>9) return;
  for (uint8_t row=0; row<5; row++)
    for (uint8_t col=0; col<3; col++)
      if (digits3x5[d][row] & (1 << (2-col)))
        fillCountdownBlock(x + col*scale, y + row*scale, scale, c);
}

void drawOriginalCountdownHourglass(uint8_t frame, uint8_t scale) {
  const CRGB colors[4] = { CRGB::Black, CRGB::White, CRGB(242,119,6), CRGB(146,86,61) };
  frame %= 10;
  for (uint8_t sy=0; sy<10; sy++) {
    for (uint8_t sx=0; sx<7; sx++) {
      const uint8_t pi = pgm_read_byte(&countdownHourglassFrames[frame][sy*7+sx]);
      if (!pi) continue;
      fillCountdownBlock(sx*scale, (4+sy)*scale, scale, colors[pi]);
    }
  }
}

void renderCountdown(uint32_t remainMs) {
  const uint32_t remainSec=(remainMs+999UL)/1000UL;
  const uint8_t mm=(remainSec/60UL)%100UL, ss=remainSec%60UL;
  const uint8_t scale=(uint8_t)(MATRIX_WIDTH/16U);
  const CRGB minuteColor=CRGB::White;
  const CRGB normalSecondColor(242,119,6);
  const CRGB secondColor = remainSec<=10UL ? CRGB::Red : normalSecondColor;
  const CRGB colonColor(242,119,6);

  clearFramebuffer();

  // Manufacturer 16x16 animation: ten frames at 200 ms each.  On larger
  // logical matrices the same pixel artwork is enlarged cleanly rather than
  // being produced on a 16x16 framebuffer and post-scaled.
  const uint8_t frame = (remainSec == 0UL) ? 9U : (uint8_t)((millis()/200UL)%10UL);
  drawOriginalCountdownHourglass(frame,scale);

  // Original device layout: minutes above seconds on the right side.
  drawDigit3x5Scaled(mm/10,9*scale,3*scale,scale,minuteColor);
  drawDigit3x5Scaled(mm%10,13*scale,3*scale,scale,minuteColor);
  drawDigit3x5Scaled(ss/10,9*scale,9*scale,scale,secondColor);
  drawDigit3x5Scaled(ss%10,13*scale,9*scale,scale,secondColor);

  // The original 16x16 separator is squeezed against the hourglass at x=7.
  // Preserve it exactly at 16x16, but center each dot in the wider 2*scale
  // gap available on 32x32 and 64x64.  The separator itself does not turn red;
  // only the seconds do during the final ten seconds.
  const int16_t colonShift = (MATRIX_WIDTH>=64) ? 2 : ((MATRIX_WIDTH>=32) ? 1 : 0);
  const int16_t colonX = 7*scale + (scale>1 ? scale/2 : 0) + colonShift;
  if (((millis()/500UL)&1U)==0U) {
    fillCountdownBlock(colonX,10*scale,scale,colonColor);
    fillCountdownBlock(colonX,12*scale,scale,colonColor);
  }

  refreshMatrix();
}

// Original 16x16 stopwatch artwork captured from the manufacturer device.
// Revalidated frame-by-frame against the supplied native 16x16 reference video.
// Palette indexes: 0=black, 1=white face, 2=orange button, 3=gray/lilac case,
// 4=red hand. Only the 7x9 stopwatch icon area is stored; MM/SS and the
// blinking orange separator are rendered dynamically.
static const uint8_t stopwatchFrames[8][63] PROGMEM = {
  {0,2,2,2,2,2,0, 0,0,2,2,2,0,0, 0,3,3,3,3,3,0, 3,1,1,1,1,1,3, 3,1,1,4,1,1,3, 3,1,1,4,1,1,3, 3,1,1,1,1,1,3, 3,1,1,1,1,1,3, 0,3,3,3,3,3,0},
  {0,2,2,2,2,2,0, 0,0,2,2,2,0,0, 0,3,3,3,3,3,0, 3,1,1,1,1,1,3, 3,1,1,1,4,1,3, 3,1,1,4,1,1,3, 3,1,1,1,1,1,3, 3,1,1,1,1,1,3, 0,3,3,3,3,3,0},
  {0,2,2,2,2,2,0, 0,0,2,2,2,0,0, 0,3,3,3,3,3,0, 3,1,1,1,1,1,3, 3,1,1,1,1,1,3, 3,1,1,4,4,1,3, 3,1,1,1,1,1,3, 3,1,1,1,1,1,3, 0,3,3,3,3,3,0},
  {0,2,2,2,2,2,0, 0,0,2,2,2,0,0, 0,3,3,3,3,3,0, 3,1,1,1,1,1,3, 3,1,1,1,1,1,3, 3,1,1,4,1,1,3, 3,1,1,1,4,1,3, 3,1,1,1,1,1,3, 0,3,3,3,3,3,0},
  {0,2,2,2,2,2,0, 0,0,2,2,2,0,0, 0,3,3,3,3,3,0, 3,1,1,1,1,1,3, 3,1,1,1,1,1,3, 3,1,1,4,1,1,3, 3,1,1,4,1,1,3, 3,1,1,1,1,1,3, 0,3,3,3,3,3,0},
  {0,2,2,2,2,2,0, 0,0,2,2,2,0,0, 0,3,3,3,3,3,0, 3,1,1,1,1,1,3, 3,1,1,1,1,1,3, 3,1,1,4,1,1,3, 3,1,4,1,1,1,3, 3,1,1,1,1,1,3, 0,3,3,3,3,3,0},
  {0,2,2,2,2,2,0, 0,0,2,2,2,0,0, 0,3,3,3,3,3,0, 3,1,1,1,1,1,3, 3,1,1,1,1,1,3, 3,1,4,4,1,1,3, 3,1,1,1,1,1,3, 3,1,1,1,1,1,3, 0,3,3,3,3,3,0},
  {0,2,2,2,2,2,0, 0,0,2,2,2,0,0, 0,3,3,3,3,3,0, 3,1,1,1,1,1,3, 3,1,4,1,1,1,3, 3,1,1,4,1,1,3, 3,1,1,1,1,1,3, 3,1,1,1,1,1,3, 0,3,3,3,3,3,0}
};

void drawOriginalStopwatch(uint8_t frame, uint8_t scale) {
  const CRGB colors[5] = {
    CRGB::Black,
    CRGB::White,
    CRGB(242,119,6),
    CRGB(164,158,165),
    CRGB(220,5,39)
  };
  frame &= 7U;
  for (uint8_t sy=0; sy<9; sy++) {
    for (uint8_t sx=0; sx<7; sx++) {
      const uint8_t pi=pgm_read_byte(&stopwatchFrames[frame][sy*7+sx]);
      if (!pi) continue;
      fillCountdownBlock(sx*scale,(4+sy)*scale,scale,colors[pi]);
    }
  }
}

void renderStopwatch(uint32_t elapsedMs) {
  const uint32_t elapsedSec=elapsedMs/1000UL;
  const uint8_t mm=(elapsedSec/60UL)%100UL, ss=elapsedSec%60UL;
  const uint8_t scale=(uint8_t)(MATRIX_WIDTH/16U);
  const CRGB minuteColor=CRGB::White;
  const CRGB secondColor(242,119,6);
  const CRGB colonColor(242,119,6);

  clearFramebuffer();

  // Manufacturer artwork: eight hand positions at 100 ms per frame.  Using
  // elapsed time rather than wall-clock time naturally freezes the hand when
  // the stopwatch is paused and resumes from the same phase afterwards.
  const uint8_t frame=(uint8_t)((elapsedMs/100UL)&7U);
  drawOriginalStopwatch(frame,scale);

  // Original-device layout: minutes above seconds on the right.  On 32x32
  // and 64x64 the separator is centered in the enlarged gap rather than
  // inheriting the cramped 16x16 placement.
  drawDigit3x5Scaled(mm/10,9*scale,3*scale,scale,minuteColor);
  drawDigit3x5Scaled(mm%10,13*scale,3*scale,scale,minuteColor);
  drawDigit3x5Scaled(ss/10,9*scale,9*scale,scale,secondColor);
  drawDigit3x5Scaled(ss%10,13*scale,9*scale,scale,secondColor);
  const int16_t colonShift=(MATRIX_WIDTH>=64) ? 2 : ((MATRIX_WIDTH>=32) ? 1 : 0);
  const int16_t colonX=7*scale+(scale>1 ? scale/2 : 0)+colonShift;
  if (((millis()/500UL)&1U)==0U) {
    fillCountdownBlock(colonX,10*scale,scale,colonColor);
    fillCountdownBlock(colonX,12*scale,scale,colonColor);
  }

  refreshMatrix();
}

// Original-device Scoreboard artwork (16x16 reference supplied from hardware).
// The real display uses two full-width 3-digit rows: blue player A above red
// player B.  Each glyph is a 4x7 pixel-outline digit, with one blank column
// between digits and one blank pixel at each horizontal edge.
static const uint8_t SCORE_DIGITS_4X7[10][7] = {
  {0xF,0x9,0x9,0x9,0x9,0x9,0xF}, // 0
  {0x2,0x6,0x2,0x2,0x2,0x2,0x7}, // 1
  {0xF,0x1,0x1,0xF,0x8,0x8,0xF}, // 2
  {0xF,0x1,0x1,0xF,0x1,0x1,0xF}, // 3
  {0x9,0x9,0x9,0xF,0x1,0x1,0x1}, // 4
  {0xF,0x8,0x8,0xF,0x1,0x1,0xF}, // 5
  {0xF,0x8,0x8,0xF,0x9,0x9,0xF}, // 6
  {0xF,0x1,0x1,0x2,0x2,0x4,0x4}, // 7
  {0xF,0x9,0x9,0xF,0x9,0x9,0xF}, // 8
  {0xF,0x9,0x9,0xF,0x1,0x1,0xF}  // 9
};

void drawScoreDigit4x7(uint8_t digit, int16_t x, int16_t y, const CRGB &c) {
  if(digit>9) return;
  for(uint8_t row=0; row<7; ++row){
    const uint8_t bits=SCORE_DIGITS_4X7[digit][row];
    for(uint8_t col=0; col<4; ++col){
      if(bits & (1U << (3-col))) putPixel(x+col,y+row,c);
    }
  }
}

void drawScore3Digit(uint16_t score, int16_t y, const CRGB &c) {
  score%=1000;
  drawScoreDigit4x7((score/100)%10,1,y,c);
  drawScoreDigit4x7((score/10)%10,6,y,c);
  drawScoreDigit4x7(score%10,11,y,c);
}

void renderScoreboard() {
  clearFramebuffer();
  // Captured original-device palette.
  const CRGB playerAColor(120,88,248);   // #7858F8, sampled from the official app UI
  const CRGB playerBColor(248,32,120);    // #F82078, sampled from the official app UI

  // Exact 16x16 manufacturer composition: rows 0..6 and 9..15, leaving
  // two blank scanlines between players.  Larger logical profiles preserve
  // this pixel-art composition by scaling the legacy 16x16 canvas.
  drawScore3Digit(scoreA,0,playerAColor);
  drawScore3Digit(scoreB,9,playerBColor);
  scaleLegacy16CanvasToLogical();
  refreshMatrix();
}

// ======================================================
// TEXT
// ======================================================
CRGB getTextPixelColor(int16_t x, int16_t y) {
  uint32_t t=millis();
  if (textState.colorMode<=1) return CRGB(textState.colorR,textState.colorG,textState.colorB);
  if (textState.colorMode==2) return CHSV(x*18+t/18,255,255);
  if (textState.colorMode==3) return CHSV(y*19-t/22,175,255);
  if (textState.colorMode==4) return CHSV(map(sin8(x*20+y*12+t/8),0,255,0,42),220,255);
  return CHSV(map(sin8(x*16-y*10+t/10),0,255,125,205),220,255);
}

bool isTextPixel(uint8_t glyph,uint8_t row,uint8_t col) {
  if (glyph>=textState.glyphCount || row>=textState.glyphHeight ||
      col>=textState.bitmapWidth[glyph]) return false;
  const uint8_t bytesPerRow=(textState.bitmapWidth[glyph]+7)/8;
  const uint16_t off=textState.bitmapOffset[glyph]+uint16_t(row)*bytesPerRow+(col>>3);
  return textState.bitmap[off] & (1U << (col & 7));
}

uint8_t textGlyphsPerLine() {
  if (textState.glyphAdvance == 0) return 1;
  uint16_t capacity = MATRIX_WIDTH / textState.glyphAdvance;
  if (capacity == 0) capacity = 1;
  if (capacity > textState.glyphCount) capacity = textState.glyphCount;
  return (uint8_t)capacity;
}

uint8_t textMaximumRows() {
  if (textState.glyphHeight == 0) return 1;
  uint16_t rows = MATRIX_HEIGHT / textState.glyphHeight;
  if (rows == 0) rows = 1;
  // The current protocol profiles top out at 64 px and therefore at four
  // 16-pixel rows. Keep the calculation generic for future logical sizes.
  if (rows > MAX_TEXT_GLYPHS) rows = MAX_TEXT_GLYPHS;
  return (uint8_t)rows;
}

bool textUsesMultilineLayout() {
  // Original hardware uses all available vertical rows only for effects that
  // do not move the text through the display. LEFT/RIGHT and UP/DOWN remain
  // single-line scrolling presentations.
  return textState.motionEffect == 0 || textState.motionEffect >= 5;
}

uint8_t textVisibleGlyphCapacity() {
  uint16_t capacity = textGlyphsPerLine();
  if (textUsesMultilineLayout()) capacity *= textMaximumRows();
  if (capacity == 0) capacity = 1;
  if (capacity > textState.glyphCount) capacity = textState.glyphCount;
  return (uint8_t)capacity;
}

bool textUsesPagedViewport() {
  // LEFT/RIGHT (1/2) already traverse the complete line continuously.
  // Every other text effect uses a viewport when the full glyph stream does
  // not fit the current page. Non-scrolling effects may use several rows in
  // that page; UP/DOWN keep the historical one-row vertical tape.
  return textState.motionEffect != 1 && textState.motionEffect != 2;
}

void advanceTextPage() {
  const uint8_t capacity = textVisibleGlyphCapacity();
  if (capacity == 0 || textState.glyphCount <= capacity) {
    textFirstVisibleGlyph = 0;
    return;
  }
  uint16_t next = (uint16_t)textFirstVisibleGlyph + capacity;
  textFirstVisibleGlyph = (next >= textState.glyphCount) ? 0 : (uint8_t)next;
  textPageChangedAt = millis();
}

uint8_t nextTextPageFirstGlyph(uint8_t firstGlyph) {
  const uint8_t capacity = textVisibleGlyphCapacity();
  if (capacity == 0 || textState.glyphCount <= capacity) return 0;
  uint16_t next = (uint16_t)firstGlyph + capacity;
  return (next >= textState.glyphCount) ? 0 : (uint8_t)next;
}

void drawTextPage(uint8_t firstGlyph, int16_t pageX, int16_t pageY,
                  uint8_t scale=255, int16_t laserRow=-1) {
  const uint8_t capacity = textVisibleGlyphCapacity();
  const uint8_t glyphsToDraw =
      min(capacity, (uint8_t)(textState.glyphCount - firstGlyph));

  if (textUsesMultilineLayout()) {
    const uint8_t perLine=textGlyphsPerLine();
    const uint8_t lineCount=(uint8_t)((glyphsToDraw + perLine - 1U) / perLine);
    const int16_t blockHeight=(int16_t)lineCount * textState.glyphHeight;
    const int16_t startY=(MATRIX_HEIGHT>blockHeight)
        ? (int16_t)((MATRIX_HEIGHT-blockHeight)/2) + pageY
        : pageY;

    // Keep the historical horizontal origin. The original-device behavior
    // observed here changes vertical packing only: glyphs wrap sequentially
    // into the available rows and the used row block is centered vertically.
    for (uint8_t local=0; local<glyphsToDraw; local++) {
      const uint8_t g=firstGlyph+local;
      const uint8_t line=(uint8_t)(local/perLine);
      const uint8_t column=(uint8_t)(local%perLine);
      const int16_t gx=pageX+(int16_t)column*textState.glyphAdvance;
      const int16_t gy=startY+(int16_t)line*textState.glyphHeight;
      for (uint8_t row=0;row<textState.glyphHeight;row++) for (uint8_t col=0;col<textState.glyphWidth;col++) {
        if (!isTextPixel(g,row,col)) continue;
        const int16_t px=gx+col, py=gy+row;
        CRGB c=getTextPixelColor(px,py);
        if (scale<255) c.nscale8_video(scale);
        if (laserRow>=0 && py==laserRow) c=CRGB::White;
        putPixel(px,py,c);
      }
    }
    return;
  }

  for (uint8_t local=0; local<glyphsToDraw; local++) {
    const uint8_t g = firstGlyph + local;
    int16_t gx=pageX+(int16_t)local*textState.glyphAdvance;
    int16_t gy=pageY;
    for (uint8_t row=0;row<textState.glyphHeight;row++) for (uint8_t col=0;col<textState.glyphWidth;col++) {
      if (!isTextPixel(g,row,col)) continue;
      int16_t px=gx+col, py=gy+row;
      CRGB c=getTextPixelColor(px,py);
      if (scale<255) c.nscale8_video(scale);
      if (laserRow>=0 && py==laserRow) c=CRGB::White;
      putPixel(px,py,c);
    }
  }
}

void drawTextGlyphs(uint8_t scale=255,int16_t laserRow=-1) {
  if (!textUsesPagedViewport()) {
    // LEFT/RIGHT retain their historical full-line continuous renderer.
    for (uint8_t g=0; g<textState.glyphCount; g++) {
      int16_t gx=textState.offsetX+(int16_t)g*textState.glyphAdvance;
      int16_t gy=textState.offsetY;
      for (uint8_t row=0;row<textState.glyphHeight;row++) for (uint8_t col=0;col<textState.glyphWidth;col++) {
        if (!isTextPixel(g,row,col)) continue;
        int16_t px=gx+col, py=gy+row;
        CRGB c=getTextPixelColor(px,py);
        if (scale<255) c.nscale8_video(scale);
        if (laserRow>=0 && py==laserRow) c=CRGB::White;
        putPixel(px,py,c);
      }
    }
    return;
  }

  drawTextPage(textFirstVisibleGlyph, textState.offsetX, textState.offsetY, scale, laserRow);

  // UP/DOWN are a continuous vertical tape. Draw the next page immediately
  // behind the current one with a one-pixel separator so the matrix never
  // enters a fully blank interval between pages.
  if ((textState.motionEffect == 3 || textState.motionEffect == 4) &&
      textState.glyphCount > textVisibleGlyphCapacity()) {
    const uint8_t nextFirst = nextTextPageFirstGlyph(textFirstVisibleGlyph);
    const int16_t step = (int16_t)textState.glyphHeight + 1;
    const int16_t nextY = (textState.motionEffect == 3)
                        ? textState.offsetY + step
                        : textState.offsetY - step;
    drawTextPage(nextFirst, textState.offsetX, nextY, scale, laserRow);
  }
}

void renderTextFrame() {
  if (!textState.valid) return;
  CRGB bg = textState.backgroundMode ? CRGB(textState.backgroundR,textState.backgroundG,textState.backgroundB) : CRGB::Black;
  clearFramebuffer(bg);
  uint32_t e=millis()-textState.animationStart;
  switch (textState.motionEffect) {
    default: case 0: case 1: case 2: case 3: case 4: drawTextGlyphs(); break;
    case 5: if (((e/350)&1)==0) drawTextGlyphs(); break;
    case 6: drawTextGlyphs(40+scale8(sin8(e/8),215)); break;
    case 7: {
      drawTextGlyphs();
      const uint16_t ph=e/100U;
      // A regular index*3 Y phase creates visible travelling snow bands,
      // especially on a 16x16 logical canvas upscaled to a larger panel.
      // Use deterministic irregular phases and two fall rates instead, with
      // particle density proportional to the logical display width.
      const uint8_t flakeCount=MATRIX_WIDTH>=64 ? 48U : (MATRIX_WIDTH>=32 ? 24U : 16U);
      for(uint8_t i=0;i<flakeCount;i++){
        const uint32_t seed=effectHash((uint32_t)i*0x9E3779B9UL+0x51ED270BUL);
        const uint8_t x=(uint8_t)(seed%MATRIX_WIDTH);
        const uint16_t startY=(uint16_t)((seed>>8)%MATRIX_HEIGHT);
        const uint8_t rate=(uint8_t)(1U+((seed>>20)&0x01U));
        const uint8_t y=(uint8_t)((startY+(uint32_t)ph*rate)%MATRIX_HEIGHT);
        putPixel(x,y,CRGB::White);
      }
      break;
    }
    case 8: {
      int16_t lr=(e/70)%MATRIX_HEIGHT;
      drawTextGlyphs(110,lr);
      for(uint8_t x=0;x<MATRIX_WIDTH;x++){ CRGB c=CRGB::Red; c.nscale8_video(120); putPixel(x,lr,c); }
      break;
    }
  }
  refreshMatrix();
}

uint16_t textFrameInterval() {
  uint8_t s=min((uint8_t)100,textState.speed);
  return map(s,0,100,140,20);
}

uint16_t textPageInterval() {
  // PIN uses the same app speed setting as the motion effects, but as a
  // readable page dwell rather than as a per-pixel frame interval.
  uint8_t s=min((uint8_t)100,textState.speed);
  return map(s,0,100,1600,250);
}

void resetTextPosition() {
  int tw=textState.glyphCount*textState.glyphAdvance;
  const int16_t centeredY =
      (MATRIX_HEIGHT > textState.glyphHeight)
          ? (int16_t)((MATRIX_HEIGHT - textState.glyphHeight) / 2)
          : (int16_t)0;
  textFirstVisibleGlyph=0;
  textPageChangedAt=millis();
  switch(textState.motionEffect){
    case 1:textState.offsetX=MATRIX_WIDTH;textState.offsetY=centeredY;break;
    case 2:textState.offsetX=-tw;textState.offsetY=centeredY;break;
    case 3:textState.offsetX=0;textState.offsetY=MATRIX_HEIGHT;break;
    case 4:textState.offsetX=0;textState.offsetY=-textState.glyphHeight;break;
    default:
      textState.offsetX=0;
      textState.offsetY=textUsesMultilineLayout() ? 0 : centeredY;
      break;
  }
  textState.animationStart=millis();
  textState.lastFrame=0;
  textState.lastRenderFrame=0;
}

void updateTextAnimation() {
  if(!textState.valid) return;
  uint32_t now=millis();
  const uint8_t capacity = textVisibleGlyphCapacity();
  const bool hasMorePages = textState.glyphCount > capacity;

  // Static/page-based effects change only the visible glyph window. Do not
  // reset animationStart here: Blink/Breathe/Snow/Laser must keep a continuous
  // phase while the displayed word fragment changes.
  if (textState.motionEffect == 0) {
    if (hasMorePages && (uint32_t)(now-textPageChangedAt) >= textPageInterval()) {
      advanceTextPage();
      renderTextFrame();
      textState.lastRenderFrame=now;
    } else if (textState.colorMode>=2 &&
               (uint32_t)(now-textState.lastRenderFrame) >= 45U) {
      // Dynamic color refresh must not advance text motion.
      renderTextFrame();
      textState.lastRenderFrame=now;
    }
    return;
  }

  if (textState.motionEffect >= 5 && hasMorePages &&
      (uint32_t)(now-textPageChangedAt) >= textPageInterval()) {
    advanceTextPage();
  }

  const uint16_t motionInterval=textFrameInterval();
  bool motionAdvanced=false;

  if ((uint32_t)(now-textState.lastFrame) >= motionInterval) {
    textState.lastFrame=now;
    int tw=textState.glyphCount*textState.glyphAdvance;
    switch(textState.motionEffect){
      case 1:
        if(--textState.offsetX < -tw) textState.offsetX=MATRIX_WIDTH;
        motionAdvanced=true;
        break;
      case 2:
        if(++textState.offsetX > MATRIX_WIDTH) textState.offsetX=-tw;
        motionAdvanced=true;
        break;
      case 3: {
        --textState.offsetY;
        const int16_t step=(int16_t)textState.glyphHeight+1;
        if(textState.offsetY <= -step) {
          textState.offsetY += step;
          advanceTextPage();
        }
        motionAdvanced=true;
        break;
      }
      case 4: {
        ++textState.offsetY;
        const int16_t step=(int16_t)textState.glyphHeight+1;
        if(textState.offsetY >= step) {
          textState.offsetY -= step;
          advanceTextPage();
        }
        motionAdvanced=true;
        break;
      }
    }
  }

  // Dynamic text visuals may need a faster redraw rate, but redraws alone
  // never change X/Y offsets or page position.
  const bool fastVisualRefresh=(textState.motionEffect>=5 || textState.colorMode>=2);
  const bool visualDue=fastVisualRefresh &&
                       (uint32_t)(now-textState.lastRenderFrame) >= 45U;

  if (motionAdvanced || visualDue) {
    renderTextFrame();
    textState.lastRenderFrame=now;
  }
}

// TEXT glyph records observed from the official app:
// marker 0x02/0x03:  8x16 bitmap,  4-byte meta +  16 bitmap bytes =  20 bytes/glyph
// marker 0x05/0x06: 16x32 bitmap,  4-byte meta +  64 bitmap bytes =  68 bytes/glyph
// marker 0x08/0x09: 32x64 bitmap,  4-byte meta + 256 bitmap bytes = 260 bytes/glyph
// SimSun/SimHei are rasterized by the app: no device-side font selection is required.
// The exact-record-size fallback mirrors the hardware-validated WLED Usermod path
// and tolerates equivalent app/firmware marker variants (for example 0x0A).
bool parseTextPayloadInternal(const uint8_t *data,size_t len,bool carouselContext) {
  if(!idotTextPayloadHasMarker(data, len, TEXT_GLOBAL_HEADER)) return false;
#if IDOTMATRIX_MEMORY_TELEMETRY
  idotMemoryTelemetrySnapshot(carouselContext ? "text.carousel.before_parse" : "text.live.before_parse");
  const uint32_t textTelemetryStartedAtUs = micros();
#endif
  const uint16_t requested=uint16_t(data[0]) | uint16_t(data[1])<<8;
  if (!requested || requested>MAX_TEXT_GLYPHS) return false;
  struct GlyphRecord { uint8_t width, height; };
  static constexpr GlyphRecord formats[] = {
    {0,0}, {0,0}, {8,16}, {16,16}, {0,0}, {16,32},
    {32,32}, {32,64}, {64,64},
  };
  auto decodeRecords = [&](GlyphRecord uniform, bool store) {
    size_t input=TEXT_GLOBAL_HEADER, output=0;
    uint8_t height=0, width=0;
    for (uint16_t g=0; g<requested; ++g) {
      if (len-input<TEXT_GLYPH_META) return false;
      const uint8_t marker=data[input];
      const auto format=uniform.width ? uniform :
          (marker<sizeof(formats)/sizeof(formats[0]) ? formats[marker] : GlyphRecord{0,0});
      if (!format.width || (height && height!=format.height)) return false;
      height=format.height;
      width=max(width,format.width);
      const size_t bytes=size_t(format.width/8)*height;
      if (bytes>len-input-TEXT_GLYPH_META || bytes>sizeof(textState.bitmap)-output) return false;
      if (store) {
        memcpy(textState.meta[g],data+input,TEXT_GLYPH_META);
        textState.bitmapOffset[g]=output;
        textState.bitmapWidth[g]=format.width;
        memcpy(textState.bitmap+output,data+input+TEXT_GLYPH_META,bytes);
      }
      input+=TEXT_GLYPH_META+bytes;
      output+=bytes;
    }
    if (input!=len) return false;
    if (store) {
      textState.glyphWidth=width;
      textState.glyphHeight=height;
      textState.glyphAdvance=width;
    }
    return true;
  };
  GlyphRecord uniform{0,0};
  if (!decodeRecords(uniform,false)) {
    const size_t bytes=(len-TEXT_GLOBAL_HEADER)/requested;
    if (bytes==TEXT_GLYPH_META+16) uniform={8,16};
    else if (bytes==TEXT_GLYPH_META+64) uniform={16,32};
    else if (bytes==TEXT_GLYPH_META+256) uniform={32,64};
    else return false;
    if (!decodeRecords(uniform,false)) return false;
  }
  if (!decodeRecords(uniform,true)) return false;
  textState.valid=true;
  textState.glyphCount=requested;
  textState.motionEffect=data[4]; textState.speed=data[5]; textState.colorMode=data[6];
  textState.colorR=data[7]; textState.colorG=data[8]; textState.colorB=data[9];
  textState.backgroundMode=data[10]; textState.backgroundR=data[11]; textState.backgroundG=data[12]; textState.backgroundB=data[13];
#if DEBUG_SERIAL
  Serial.print("TEXT glyphs="); Serial.print(textState.glyphCount);
  Serial.print(" height="); Serial.println(textState.glyphHeight);
#endif
  resetTextPosition();
  if(carouselContext){
    // Carousel TEXT is still a carousel slot. Do not call switchDisplayMode(),
    // because that intentionally tears down carousel/upload state for ordinary
    // live TEXT commands. The caller has already stopped any GIF decoder.
    displayMode=DISPLAY_TEXT;
  } else {
    switchDisplayMode(DISPLAY_TEXT);
  }
  renderTextFrame();
#if IDOTMATRIX_MEMORY_TELEMETRY
  idotMemoryTelemetryLatency(carouselContext ? "text.carousel.parse_render_us" : "text.live.parse_render_us",
                             (uint32_t)(micros() - textTelemetryStartedAtUs));
  idotMemoryTelemetrySnapshot(carouselContext ? "text.carousel.after_render" : "text.live.after_render");
#endif
  return true;
}

void parseTextPayload(const uint8_t *data,size_t len) {
  (void)parseTextPayloadInternal(data,len,false);
}

// ======================================================
// LIGHT EFFECTS
// ======================================================
// Band-based effects were originally tuned on the 16x16 profile. Preserve
// their apparent geometry on larger logical displays by scaling band widths
// with the logical iDotMatrix resolution: 16x16=1x, 32x32=2x, 64x64=4x.
static inline uint8_t effectBandScale(){
  return (MATRIX_WIDTH >= 64) ? 4 : ((MATRIX_WIDTH >= 32) ? 2 : 1);
}

uint8_t effectChannelTo8(uint8_t v){ return v>=127 ? 255 : v*2; }
CRGB getEffectColor(uint8_t i){ if(!effectState.colorCount) return CRGB::White; return effectState.colors[i%effectState.colorCount]; }

CRGB effectPaletteGradient(uint8_t p){
  if(!effectState.colorCount) return CRGB::Black;
  if(effectState.colorCount==1) return effectState.colors[0];
  uint16_t scaled=(uint16_t)p*effectState.colorCount;
  uint8_t idx=scaled>>8, frac=scaled&0xFF;
  return blend(effectState.colors[idx%effectState.colorCount],effectState.colors[(idx+1)%effectState.colorCount],frac);
}

// Slower speed mapping than the previous build.
uint16_t effectFrameInterval(){
  uint8_t s=min((uint8_t)100,effectState.speed);

  // Effect 6 is a true per-pixel cross-fade, so smooth motion requires
  // frequent refreshes even when the app-selected speed is very low
  // (for example speed=5).
  if(effectState.effect==6) return 40;
#if defined(IDOTMATRIX_BOARD_CLOCKWISE)
  if (effectState.effect == 0) return 25;
#endif

  // The remaining effects intentionally stay slower.
  return map(s,0,100,360,70);
}

uint32_t effectPhase(){
  uint16_t multiplier=4+(effectState.speed/3);
  return ((millis()-effectState.startMillis)*multiplier)/100UL;
}

uint32_t effectHash(uint32_t v){ v^=v>>16; v*=0x7FEB352DUL; v^=v>>15; v*=0x846CA68BUL; v^=v>>16; return v; }

void renderEffect0(){
  uint32_t ph=effectPhase()/4;
  for(uint8_t y=0;y<MATRIX_HEIGHT;y++) for(uint8_t x=0;x<MATRIX_WIDTH;x++) {
    // Long gradient: roughly 10+ pixels per color transition.
    uint8_t pos=(uint8_t)((y*5 + x + ph)/3);
    framebuffer[logicalIndex(x,y)]=effectPaletteGradient(pos);
  }
}

void renderEffect1(){
  clearFramebuffer(); uint32_t frame=effectPhase()/8;
  for(uint8_t i=0;i<22;i++){
    uint32_t h=effectHash((uint32_t)i*173UL+frame*31UL);
    uint8_t x=(uint8_t)(h%MATRIX_WIDTH),y=(uint8_t)((h>>8)%MATRIX_HEIGHT);
    CRGB c=getEffectColor((h>>8)%max((uint8_t)1,effectState.colorCount));
    c.nscale8_video(100+((h>>16)&0x9F)); putPixel(x,y,c);
  }
}

void renderEffect2(){
  uint32_t ph=effectPhase()/3;
  // Background: continuous fading between colors.
  for(uint8_t y=0;y<MATRIX_HEIGHT;y++) for(uint8_t x=0;x<MATRIX_WIDTH;x++){
    uint8_t pos=(uint8_t)(ph+x*2+y*2);
    CRGB c=effectPaletteGradient(pos); c.nscale8_video(190);
    framebuffer[logicalIndex(x,y)]=c;
  }
  // Spikes are always white.
  uint32_t frame=ph/4;
  for(uint8_t i=0;i<18;i++){
    uint32_t h=effectHash((uint32_t)i*223UL+frame*19UL);
    putPixel((uint8_t)(h%MATRIX_WIDTH),(uint8_t)((h>>8)%MATRIX_HEIGHT),CRGB::White);
  }
}

void renderEffect3(){
  // The observed style advances the bands in two-pixel steps.
  uint32_t raw=effectPhase()/10;
  uint32_t ph=(raw/2)*2;
  uint8_t count=max((uint8_t)1,effectState.colorCount);
  const uint8_t stripeWidth=(uint8_t)(4U*effectBandScale());
  for(uint8_t y=0;y<MATRIX_HEIGHT;y++) for(uint8_t x=0;x<MATRIX_WIDTH;x++)
    framebuffer[logicalIndex(x,y)]=getEffectColor(((x+ph)/stripeWidth)%count);
}

void renderEffect4(){
  // Diagonals also advance in two-pixel steps.
  uint32_t raw=effectPhase()/10;
  uint32_t ph=(raw/2)*2;
  uint8_t count=max((uint8_t)1,effectState.colorCount);
  const uint8_t stripeWidth=(uint8_t)(4U*effectBandScale());
  for(uint8_t y=0;y<MATRIX_HEIGHT;y++) for(uint8_t x=0;x<MATRIX_WIDTH;x++)
    framebuffer[logicalIndex(x,y)]=getEffectColor(((x+y+ph)/stripeWidth)%count);
}

void renderEffect5(){
  clearFramebuffer();
  uint32_t raw=effectPhase()/11;
  uint32_t ph=(raw/2)*2;
  uint8_t count=max((uint8_t)1,effectState.colorCount);
  const uint8_t scale=effectBandScale();
  const uint8_t colorWidth=(uint8_t)(5U*scale);
  const uint8_t blackWidth=(uint8_t)(4U*scale);
  const uint8_t block=(uint8_t)(colorWidth+blackWidth);
  for(uint8_t y=0;y<MATRIX_HEIGHT;y++) for(uint8_t x=0;x<MATRIX_WIDTH;x++){
    uint16_t d=x+y+ph; uint8_t within=d%block;
    if(within<colorWidth) putPixel(x,y,getEffectColor((d/block)%count));
  }
}

void renderEffect6(){
  // UI effect 7: each pixel keeps its own phase and genuinely fades
  // from one color to the next instead of switching instantly.
  const uint8_t count=max((uint8_t)1,effectState.colorCount);

  if(count==1){
    clearFramebuffer(getEffectColor(0));
    return;
  }

  // In the app speed=5 is slow: one transition lasts about 5.7 seconds,
  // while speed=100 drops to about 700 ms. A 40 ms update interval gives
  // the fade enough steps to remain visibly continuous at minimum speed.
  const uint8_t sp=constrain(effectState.speed,0,100);
  const uint32_t fadeMs=map(sp,0,100,6000,700);
  const uint32_t elapsed=millis()-effectState.startMillis;
  const uint32_t cycleMs=fadeMs*(uint32_t)count;

  for(uint8_t y=0;y<MATRIX_HEIGHT;y++){
    for(uint8_t x=0;x<MATRIX_WIDTH;x++){
      const uint16_t i=(uint16_t)y*MATRIX_WIDTH+x;
      const uint32_t seed=effectHash((uint32_t)i*977UL+0x51EDUL);

      // Each pixel starts at a different point in the same continuous
      // palette cycle.
      const uint32_t local=(elapsed+(seed%cycleMs))%cycleMs;
      const uint8_t a=(uint8_t)(local/fadeMs);
      const uint8_t b=(uint8_t)((a+1)%count);
      const uint32_t within=local%fadeMs;

      // 0..255 with high temporal resolution.
      const uint8_t linear=(uint8_t)((within*255UL)/(fadeMs-1UL));
      const uint8_t frac=ease8InOutCubic(linear);

      framebuffer[logicalIndex(x,y)] = blend(
        getEffectColor(a),
        getEffectColor(b),
        frac
      );
    }
  }
}

#if defined(IDOTMATRIX_BOARD_CLOCKWISE)
void clockEffectRgbToHsv(const CRGB& color, float (&hsv)[3]) {
  const int high = max(color.r, max(color.g, color.b));
  const int low = min(color.r, min(color.g, color.b));
  const float delta = high - low;
  hsv[2] = high / 255.0f;
  hsv[1] = high ? delta / high : 0;
  if (!delta) hsv[0] = 0;
  else if (high == color.r) hsv[0] = 60 * ((color.g - color.b) / delta);
  else if (high == color.g) hsv[0] = 60 * (2 + (color.b - color.r) / delta);
  else hsv[0] = 60 * (4 + (color.r - color.g) / delta);
  if (hsv[0] < 0) hsv[0] += 360;
}

CRGB clockEffectHueBlend(const CRGB& first, const CRGB& second, float fraction) {
  float a[3], b[3];
  clockEffectRgbToHsv(first, a);
  clockEffectRgbToHsv(second, b);
  float delta = b[0] - a[0];
  if (delta > 180) delta -= 360;
  if (delta < -180) delta += 360;
  const float hue = fmodf(a[0] + delta * fraction + 360, 360) / 60;
  const float sat = a[1] + (b[1] - a[1]) * fraction;
  const float val = a[2] + (b[2] - a[2]) * fraction;
  const float chroma = val * sat;
  const float middle = chroma * (1 - fabsf(fmodf(hue, 2) - 1));
  const float floor = val - chroma;
  const float channels[6][3] = {
    {chroma, middle, 0}, {middle, chroma, 0}, {0, chroma, middle},
    {0, middle, chroma}, {middle, 0, chroma}, {chroma, 0, middle},
  };
  const auto& rgb = channels[uint8_t(hue)];
  return CRGB(lroundf((rgb[0] + floor) * 255), lroundf((rgb[1] + floor) * 255),
              lroundf((rgb[2] + floor) * 255));
}

void renderClockwiseLightEffect() {
  constexpr uint8_t previewSize = 32;
  const uint8_t count = max(uint8_t(1), effectState.colorCount);
  const uint32_t elapsed = millis() - effectState.startMillis;
  const uint8_t effect = effectState.effect;
  const uint32_t interval = map(min(uint8_t(100), effectState.speed), 0, 100, 370, 70);
  const uint32_t step = elapsed / interval;
  if (effect == 0) {
    constexpr uint8_t rowsPerColor = MATRIX_WIDTH / 2;
    constexpr uint32_t segmentSpan = rowsPerColor * 256U;
    const uint32_t cycle = segmentSpan * count;
    const uint32_t rowInterval = map(min(uint8_t(100), effectState.speed), 0, 100, 210, 38);
    const uint32_t phase = ((elapsed % (rowInterval * rowsPerColor * count)) * 256U) / rowInterval;
    for (uint8_t y = 0; y < MATRIX_WIDTH; ++y) {
      const uint32_t position = (uint32_t(y) * 256U + cycle - phase) % cycle;
      const uint8_t segment = position / segmentSpan;
      const CRGB color = clockEffectHueBlend(getEffectColor(segment), getEffectColor(segment + 1),
                                            float(position % segmentSpan) / segmentSpan);
      for (uint8_t x = 0; x < MATRIX_WIDTH; ++x) {
        putPixel(x, y, color);
      }
    }
    return;
  }
  CRGB wash;
  if (effect == 2) {
    constexpr uint8_t stepsPerColor = 12;
    const uint32_t length = count * stepsPerColor;
    const uint32_t index = (length - step % length) % length;
    const uint8_t segment = index / stepsPerColor;
    wash = clockEffectHueBlend(getEffectColor(segment), getEffectColor(segment + 1),
                               float(index % stepsPerColor) / stepsPerColor);
  }
  for (uint8_t y = 0; y < previewSize; ++y) {
    for (uint8_t x = 0; x < previewSize; ++x) {
      const uint32_t seed = effectHash(uint32_t(y) * previewSize + x);
      const uint32_t noise = effectHash(seed + step * 31U);
      CRGB color;
      switch (effect) {
        case 1: {
          const uint8_t pick = noise % (count * (count < 7 ? 10 - count : 3));
          color = (pick < count && pick < effectState.colorCount) ? getEffectColor(pick) : CRGB(CRGB::Black);
          break;
        }
        case 2: color = noise % 10 == 0 ? CRGB(CRGB::White) : wash; break;
        case 3: color = getEffectColor(((x + count * 4U - step % (count * 4U)) / 4U) % count); break;
        case 4: color = getEffectColor(((x + previewSize - y + count * 4U - step % (count * 4U)) / 4U) % count); break;
        case 5: {
          const uint32_t band = (previewSize + x + y + step) / 5U;
          color = band % 2 ? CRGB(CRGB::Black) : getEffectColor((band / 2) % count);
          break;
        }
        case 6: {
          const uint32_t fadeMs = map(min(uint8_t(100), effectState.speed), 0, 100, 6000, 700);
          const uint32_t local = elapsed + seed % (fadeMs * count);
          color = effectPaletteGradient(uint8_t((local % (fadeMs * count)) * 256U / (fadeMs * count)));
          break;
        }
        default: color = CRGB::Black; break;
      }
      putPixel(x * 2, y * 2, color);
      putPixel(x * 2 + 1, y * 2, color);
      putPixel(x * 2, y * 2 + 1, color);
      putPixel(x * 2 + 1, y * 2 + 1, color);
    }
  }
}
#endif

void renderEffect(){
  if(!effectState.valid) return;
#if defined(IDOTMATRIX_BOARD_CLOCKWISE)
  renderClockwiseLightEffect();
#else
  switch(effectState.effect){
    case 0:renderEffect0();break; case 1:renderEffect1();break; case 2:renderEffect2();break;
    case 3:renderEffect3();break; case 4:renderEffect4();break; case 5:renderEffect5();break; case 6:renderEffect6();break;
    default:clearFramebuffer();break;
  }
#endif
  refreshMatrix();
}

void updateEffect(){
  if(displayMode!=DISPLAY_EFFECT || !effectState.valid) return;
  uint32_t now=millis();
  if(now-effectState.lastFrameMillis<effectFrameInterval()) return;
  effectState.lastFrameMillis=now; renderEffect();
}

bool processEffectCommand(const uint8_t *data,size_t len){
  if(len<7 || data[2]!=0x03 || data[3]!=0x02) return false;
  uint8_t effect=data[4], speed=data[5], count=data[6];
  size_t expected=7+(size_t)count*3;
  if(len<expected){ sendCommandAck(0x03,0x02); return true; }
  count=min((uint8_t)MAX_EFFECT_COLORS,count);
  effectState.valid=true; effectState.effect=effect; effectState.speed=speed; effectState.colorCount=count;
  for(uint8_t i=0;i<count;i++){
    size_t p=7+(size_t)i*3;
    effectState.colors[i]=CRGB(effectChannelTo8(data[p]),effectChannelTo8(data[p+1]),effectChannelTo8(data[p+2]));
  }
  effectState.startMillis=millis(); effectState.lastFrameMillis=0;
#if DEBUG_SERIAL
  Serial.print("EFFECT id=");Serial.print(effect);Serial.print(" speed=");Serial.print(speed);Serial.print(" colors=");Serial.println(count);
#endif
  switchDisplayMode(DISPLAY_EFFECT); renderEffect(); sendCommandAck(0x03,0x02); return true;
}

// ======================================================
// GIF
// ======================================================
// Complete compressed GIF sources use a two-level PSRAM policy.
// A bounded persistent LRU cache serves warm media directly from PSRAM. Cache
// misses retain one-shot staging, with transparent LittleFS
// fallback whenever cap/reserve/allocation/copy policy cannot be satisfied.
uint8_t *gifStageData = nullptr;
size_t gifStageSize = 0;
String gifStagePath;
size_t gifStagePeakBytes = 0;
size_t gifStageRequestedBytes = 0;
uint32_t gifStageAttempts = 0;
uint32_t gifStageOk = 0;
uint32_t gifStageFallback = 0;

#if IDOTMATRIX_GIF_PSRAM_CACHE_MAX_ENTRIES > 0
  #define IDOTMATRIX_GIF_CACHE_STORAGE_ENTRIES IDOTMATRIX_GIF_PSRAM_CACHE_MAX_ENTRIES
#else
  #define IDOTMATRIX_GIF_CACHE_STORAGE_ENTRIES 1
#endif

constexpr size_t GIF_CACHE_PATH_MAX = 48U;
struct GifCacheEntry {
  bool valid = false;
  uint8_t *data = nullptr;
  size_t size = 0;
  uint32_t crc = 0;
  uint32_t lastUse = 0;
  char path[GIF_CACHE_PATH_MAX] = {0};
};

GifCacheEntry gifCache[IDOTMATRIX_GIF_CACHE_STORAGE_ENTRIES];
size_t gifCacheBytes = 0;
size_t gifCachePeakBytes = 0;
uint32_t gifCacheClock = 0;
uint32_t gifCacheHits = 0;
uint32_t gifCacheMisses = 0;
uint32_t gifCacheInserts = 0;
uint32_t gifCacheEvictions = 0;
uint32_t gifCacheInvalidations = 0;
uint32_t gifCacheBypasses = 0;
int8_t gifCacheActiveIndex = -1;

struct GifSourceHandle {
  uint8_t source = 0; // 0=LittleFS, 1=transient stage, 2=persistent source cache
  int8_t cacheIndex = -1;
  File file;
};

struct CarouselGifPrefetchState {
  bool scheduled = false;
  bool active = false;
  uint8_t slot = 0;
  uint32_t readyAt = 0;
  uint8_t *data = nullptr;
  size_t size = 0;
  size_t copied = 0;
  uint32_t expectedCRC = 0;
  uint32_t runningCRC = 0xFFFFFFFFUL;
  uint32_t startedAtUs = 0;
  File file;
  char path[GIF_CACHE_PATH_MAX] = {0};
};

CarouselGifPrefetchState carouselGifPrefetch;
uint32_t carouselGifPrefetchAttempts = 0;
uint32_t carouselGifPrefetchOk = 0;
uint32_t carouselGifPrefetchSkips = 0;
uint32_t carouselGifPrefetchFailures = 0;
uint32_t carouselGifPrefetchCancels = 0;

bool gifCacheConfigured(){
#if IDOTMATRIX_GIF_PSRAM_CACHE_MAX_BYTES > 0 && IDOTMATRIX_GIF_PSRAM_CACHE_ENTRY_MAX_BYTES > 0 && IDOTMATRIX_GIF_PSRAM_CACHE_MAX_ENTRIES > 0
  return true;
#else
  return false;
#endif
}

bool gifCachePathEquals(uint8_t index, const char *path){
  if(index>=IDOTMATRIX_GIF_CACHE_STORAGE_ENTRIES || !path) return false;
  const GifCacheEntry &e=gifCache[index];
  return e.valid && strncmp(e.path,path,GIF_CACHE_PATH_MAX)==0;
}

void reportGifCache(const char *state, const char *reason, int index, const char *path, size_t size, uint32_t crc){
#if DEBUG_SERIAL
  Serial.print("[GIFCACHE] state="); Serial.print(state ? state : "unknown");
  Serial.print(" reason="); Serial.print(reason ? reason : "none");
  Serial.print(" index="); Serial.print(index);
  Serial.print(" bytes="); Serial.print((uint32_t)size);
  Serial.print(" crc="); Serial.print(crc,HEX);
  Serial.print(" used="); Serial.print((uint32_t)gifCacheBytes);
  Serial.print(" peak="); Serial.print((uint32_t)gifCachePeakBytes);
  Serial.print(" hits="); Serial.print(gifCacheHits);
  Serial.print(" misses="); Serial.print(gifCacheMisses);
  Serial.print(" inserts="); Serial.print(gifCacheInserts);
  Serial.print(" evict="); Serial.print(gifCacheEvictions);
  Serial.print(" invalidate="); Serial.print(gifCacheInvalidations);
  Serial.print(" bypass="); Serial.print(gifCacheBypasses);
  Serial.print(" budget="); Serial.print((uint32_t)IDOTMATRIX_GIF_PSRAM_CACHE_MAX_BYTES);
  Serial.print(" entryMax="); Serial.print((uint32_t)IDOTMATRIX_GIF_PSRAM_CACHE_ENTRY_MAX_BYTES);
  Serial.print(" entries="); Serial.print((uint32_t)IDOTMATRIX_GIF_PSRAM_CACHE_MAX_ENTRIES);
  Serial.print(" path="); Serial.println(path ? path : "-");
#else
  (void)state; (void)reason; (void)index; (void)path; (void)size; (void)crc;
#endif
}

int8_t gifCacheFind(const char *path, size_t size, uint32_t crc, bool touch){
  if(!gifCacheConfigured() || !path || !size || !crc) return -1;
  for(uint8_t i=0;i<IDOTMATRIX_GIF_CACHE_STORAGE_ENTRIES;i++){
    GifCacheEntry &e=gifCache[i];
    if(!e.valid || !e.data || e.size!=size || e.crc!=crc || !gifCachePathEquals(i,path)) continue;
    if(touch) e.lastUse=++gifCacheClock;
    return (int8_t)i;
  }
  return -1;
}

bool gifCacheMatches(const char *path, uint32_t size, uint32_t crc){
  return gifCacheFind(path,(size_t)size,crc,false)>=0;
}

bool gifCacheDropIndex(uint8_t index, const char *reason, bool countEviction){
  if(index>=IDOTMATRIX_GIF_CACHE_STORAGE_ENTRIES) return false;
  GifCacheEntry &e=gifCache[index];
  if(!e.valid) return true;
  if((int8_t)index==gifCacheActiveIndex) return false;
  const size_t oldSize=e.size;
  char oldPath[GIF_CACHE_PATH_MAX];
  strncpy(oldPath,e.path,sizeof(oldPath)-1); oldPath[sizeof(oldPath)-1]='\0';
  const uint32_t oldCRC=e.crc;
  if(e.data) heap_caps_free(e.data);
  if(gifCacheBytes>=oldSize) gifCacheBytes-=oldSize; else gifCacheBytes=0;
  e=GifCacheEntry();
  if(countEviction) gifCacheEvictions++; else gifCacheInvalidations++;
  reportGifCache(countEviction ? "evict" : "invalidate",reason,(int)index,oldPath,oldSize,oldCRC);
#if IDOTMATRIX_MEMORY_TELEMETRY
  idotMemoryTelemetrySnapshot(countEviction ? "gif.cache.evict" : "gif.cache.invalidate");
#endif
  return true;
}

void gifCacheInvalidatePath(const char *path){
  if(!gifCacheConfigured() || !path) return;
  for(uint8_t i=0;i<IDOTMATRIX_GIF_CACHE_STORAGE_ENTRIES;i++){
    if(gifCachePathEquals(i,path)) gifCacheDropIndex(i,"path",false);
  }
}

int8_t gifCacheLruVictim(){
  int8_t victim=-1;
  uint32_t oldest=0;
  for(uint8_t i=0;i<IDOTMATRIX_GIF_CACHE_STORAGE_ENTRIES;i++){
    const GifCacheEntry &e=gifCache[i];
    if(!e.valid || (int8_t)i==gifCacheActiveIndex) continue;
    if(victim<0 || e.lastUse<oldest){ victim=(int8_t)i; oldest=e.lastUse; }
  }
  return victim;
}

int8_t gifCacheFreeSlot(){
  for(uint8_t i=0;i<IDOTMATRIX_GIF_CACHE_STORAGE_ENTRIES;i++) if(!gifCache[i].valid) return (int8_t)i;
  return -1;
}

bool gifCacheMakeRoom(size_t incoming){
  if(!gifCacheConfigured() || !incoming) return false;
  const size_t budget=(size_t)IDOTMATRIX_GIF_PSRAM_CACHE_MAX_BYTES;
  const size_t reserve=(size_t)IDOTMATRIX_GIF_PSRAM_RESERVE_BYTES;
  const uint32_t caps=MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
  while(true){
    const size_t freePsram=heap_caps_get_free_size(caps);
    const bool budgetOK=incoming<=budget && gifCacheBytes<=budget-incoming;
    const bool reserveOK=freePsram>=reserve;
    const bool slotOK=gifCacheFreeSlot()>=0;
    if(budgetOK && reserveOK && slotOK) return true;
    int8_t victim=gifCacheLruVictim();
    if(victim<0) return false;
    if(!gifCacheDropIndex((uint8_t)victim,"lru",true)) return false;
  }
}

void reportGifStage(const char *state, const char *reason){
#if DEBUG_SERIAL
  Serial.print("[GIFSTAGE] state="); Serial.print(state ? state : "unknown");
  Serial.print(" reason="); Serial.print(reason ? reason : "none");
  Serial.print(" bytes="); Serial.print((uint32_t)gifStageSize);
  Serial.print(" requested="); Serial.print((uint32_t)gifStageRequestedBytes);
  Serial.print(" peak="); Serial.print((uint32_t)gifStagePeakBytes);
  Serial.print(" attempts="); Serial.print(gifStageAttempts);
  Serial.print(" ok="); Serial.print(gifStageOk);
  Serial.print(" fallback="); Serial.print(gifStageFallback);
  Serial.print(" max="); Serial.print((uint32_t)IDOTMATRIX_GIF_PSRAM_STAGE_MAX_BYTES);
  Serial.print(" reserve="); Serial.println((uint32_t)IDOTMATRIX_GIF_PSRAM_RESERVE_BYTES);
#else
  (void)state; (void)reason;
#endif
}

void releaseGifStage(){
  const bool hadStage=gifStageData!=nullptr;
  if(gifStageData){
    heap_caps_free(gifStageData);
    gifStageData=nullptr;
  }
  gifStageSize=0;
  gifStagePath="";
#if IDOTMATRIX_MEMORY_TELEMETRY
  if(hadStage) idotMemoryTelemetrySnapshot("gif.stage.released");
#endif
}

bool failGifStage(const char *reason){
  gifStageFallback++;
#if IDOTMATRIX_MEMORY_TELEMETRY
  idotMemoryTelemetrySnapshot("gif.stage.fallback");
#endif
  reportGifStage("fs",reason);
  return false;
}

bool gifCacheInsertOwnedBuffer(const char *path, size_t size, uint32_t crc, uint8_t *data, const char *reason, int8_t *slotOut){
  if(slotOut) *slotOut=-1;
  if(!gifCacheConfigured() || !data || !size || !path || !crc) return false;
  if(size>(size_t)IDOTMATRIX_GIF_PSRAM_CACHE_ENTRY_MAX_BYTES){
    gifCacheBypasses++; reportGifCache("bypass","entry_cap",-1,path,size,crc); return false;
  }
  if(size>(size_t)IDOTMATRIX_GIF_PSRAM_CACHE_MAX_BYTES){
    gifCacheBypasses++; reportGifCache("bypass","budget",-1,path,size,crc); return false;
  }
  if(!gifCacheMakeRoom(size)){
    gifCacheBypasses++; reportGifCache("bypass","room",-1,path,size,crc); return false;
  }
  int8_t slot=gifCacheFreeSlot();
  if(slot<0){ gifCacheBypasses++; reportGifCache("bypass","slot",-1,path,size,crc); return false; }
  GifCacheEntry &e=gifCache[(uint8_t)slot];
  e.valid=true;
  e.data=data;
  e.size=size;
  e.crc=crc;
  e.lastUse=++gifCacheClock;
  strncpy(e.path,path,GIF_CACHE_PATH_MAX-1); e.path[GIF_CACHE_PATH_MAX-1]='\0';
  gifCacheBytes+=e.size;
  if(gifCacheBytes>gifCachePeakBytes) gifCachePeakBytes=gifCacheBytes;
  gifCacheInserts++;
  if(slotOut) *slotOut=slot;
  reportGifCache("insert",reason ? reason : "owned",slot,e.path,e.size,e.crc);
#if IDOTMATRIX_MEMORY_TELEMETRY
  idotMemoryTelemetrySnapshot("gif.cache.insert");
#endif
  return true;
}

bool gifCacheAdoptStage(const char *path, uint32_t crc){
  if(!gifStageData || !gifStageSize || !path || !crc) return false;
  int8_t slot=-1;
  if(!gifCacheInsertOwnedBuffer(path,gifStageSize,crc,gifStageData,"cold",&slot)) return false;
  gifCacheActiveIndex=slot;
  gifStageData=nullptr;
  gifStageSize=0;
  gifStagePath="";
  return true;
}

bool prepareGifStage(const char *path, size_t expectedSize, uint32_t expectedCRC){
  releaseGifStage();
  gifCacheActiveIndex=-1;
  gifStageRequestedBytes=expectedSize;

  if(gifCacheConfigured() && path && expectedSize && expectedCRC &&
     expectedSize<=(size_t)IDOTMATRIX_GIF_PSRAM_CACHE_ENTRY_MAX_BYTES){
#if IDOTMATRIX_MEMORY_TELEMETRY
    const uint32_t cacheLookupStartedAtUs=micros();
#endif
    int8_t hit=gifCacheFind(path,expectedSize,expectedCRC,true);
    if(hit>=0){
      gifCacheHits++;
      gifCacheActiveIndex=hit;
      GifCacheEntry &e=gifCache[(uint8_t)hit];
      reportGifCache("hit","identity",hit,e.path,e.size,e.crc);
#if IDOTMATRIX_MEMORY_TELEMETRY
      idotMemoryTelemetryLatency("gif.cache.hit_us",(uint32_t)(micros()-cacheLookupStartedAtUs));
      idotMemoryTelemetrySnapshot("gif.cache.hit");
#endif
      return true;
    }
    gifCacheMisses++;
    reportGifCache("miss","identity",-1,path,expectedSize,expectedCRC);
  } else if(gifCacheConfigured()){
    gifCacheBypasses++;
    reportGifCache("bypass",expectedCRC ? "entry_cap" : "no_crc",-1,path,expectedSize,expectedCRC);
  }

#if IDOTMATRIX_GIF_PSRAM_STAGE_MAX_BYTES > 0
  gifStageAttempts++;
  if(!littleFsReady || !path || !expectedSize){
    return failGifStage("invalid");
  }
  if(expectedSize > (size_t)IDOTMATRIX_GIF_PSRAM_STAGE_MAX_BYTES){
    return failGifStage("cap");
  }

  const uint32_t caps=MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
  const size_t freePsram=heap_caps_get_free_size(caps);
  const size_t largestPsram=heap_caps_get_largest_free_block(caps);
  const size_t reserve=(size_t)IDOTMATRIX_GIF_PSRAM_RESERVE_BYTES;
  if(expectedSize > largestPsram){
    return failGifStage("largest");
  }
  if(expectedSize > freePsram || freePsram-expectedSize < reserve){
    return failGifStage("reserve");
  }

  File src=LittleFS.open(path,"r");
  if(!src || (size_t)src.size()!=expectedSize){
    if(src) src.close();
    return failGifStage("source");
  }

#if IDOTMATRIX_MEMORY_TELEMETRY
  idotMemoryTelemetrySnapshot("gif.stage.before");
  const uint32_t stageStartedAtUs=micros();
#endif
  uint8_t *buf=(uint8_t *)heap_caps_malloc(expectedSize,caps);
  if(!buf){
    src.close();
    return failGifStage("alloc");
  }

  size_t copied=0;
  bool copyOk=true;
  while(copied<expectedSize){
    size_t want=min((size_t)4096,expectedSize-copied);
    int n=src.read(buf+copied,want);
    if(n<=0){ copyOk=false; break; }
    copied+=(size_t)n;
    yield();
  }
  src.close();
  if(!copyOk || copied!=expectedSize){
    heap_caps_free(buf);
    return failGifStage("copy");
  }

  gifStageData=buf;
  gifStageSize=expectedSize;
  gifStagePath=path;
  if(gifStageSize>gifStagePeakBytes) gifStagePeakBytes=gifStageSize;
  gifStageOk++;
#if IDOTMATRIX_MEMORY_TELEMETRY
  idotMemoryTelemetryLatency("gif.stage.copy_us",(uint32_t)(micros()-stageStartedAtUs));
  idotMemoryTelemetrySnapshot("gif.stage.psram");
#endif
  reportGifStage("psram","ok");
  gifCacheAdoptStage(path,expectedCRC);
  return true;
#else
  (void)path; (void)expectedSize; (void)expectedCRC;
  return false;
#endif
}

void reportCarouselGifPrefetch(const char *state, const char *reason){
#if DEBUG_SERIAL
  Serial.print("[GIFPREFETCH] state="); Serial.print(state ? state : "unknown");
  Serial.print(" reason="); Serial.print(reason ? reason : "none");
  Serial.print(" slot="); Serial.print((int)carouselGifPrefetch.slot);
  Serial.print(" bytes="); Serial.print((uint32_t)carouselGifPrefetch.size);
  Serial.print(" copied="); Serial.print((uint32_t)carouselGifPrefetch.copied);
  Serial.print(" attempts="); Serial.print(carouselGifPrefetchAttempts);
  Serial.print(" ok="); Serial.print(carouselGifPrefetchOk);
  Serial.print(" skip="); Serial.print(carouselGifPrefetchSkips);
  Serial.print(" fail="); Serial.print(carouselGifPrefetchFailures);
  Serial.print(" cancel="); Serial.print(carouselGifPrefetchCancels);
  Serial.print(" path="); Serial.println(carouselGifPrefetch.path[0] ? carouselGifPrefetch.path : "-");
#else
  (void)state; (void)reason;
#endif
}

void resetCarouselGifPrefetch(bool freeBuffer){
  if(carouselGifPrefetch.file) carouselGifPrefetch.file.close();
  if(freeBuffer && carouselGifPrefetch.data) heap_caps_free(carouselGifPrefetch.data);
  carouselGifPrefetch.scheduled=false;
  carouselGifPrefetch.active=false;
  carouselGifPrefetch.slot=0;
  carouselGifPrefetch.readyAt=0;
  carouselGifPrefetch.data=nullptr;
  carouselGifPrefetch.size=0;
  carouselGifPrefetch.copied=0;
  carouselGifPrefetch.expectedCRC=0;
  carouselGifPrefetch.runningCRC=0xFFFFFFFFUL;
  carouselGifPrefetch.startedAtUs=0;
  carouselGifPrefetch.path[0]='\0';
}

void cancelCarouselGifPrefetch(const char *reason){
#if IDOTMATRIX_CAROUSEL_GIF_PREFETCH
  if(!carouselGifPrefetch.scheduled && !carouselGifPrefetch.active) return;
  carouselGifPrefetchCancels++;
  reportCarouselGifPrefetch("cancel",reason ? reason : "cancel");
#if IDOTMATRIX_MEMORY_TELEMETRY
  idotMemoryTelemetrySnapshot("gif.prefetch.cancel");
#endif
  resetCarouselGifPrefetch(true);
#else
  (void)reason;
#endif
}

void skipCarouselGifPrefetch(const char *reason){
#if IDOTMATRIX_CAROUSEL_GIF_PREFETCH
  carouselGifPrefetchSkips++;
  reportCarouselGifPrefetch("skip",reason ? reason : "skip");
  resetCarouselGifPrefetch(true);
#else
  (void)reason;
#endif
}

void failCarouselGifPrefetch(const char *reason){
#if IDOTMATRIX_CAROUSEL_GIF_PREFETCH
  carouselGifPrefetchFailures++;
  reportCarouselGifPrefetch("fail",reason ? reason : "fail");
#if IDOTMATRIX_MEMORY_TELEMETRY
  idotMemoryTelemetrySnapshot("gif.prefetch.fail");
#endif
  resetCarouselGifPrefetch(true);
#else
  (void)reason;
#endif
}

void scheduleCarouselGifPrefetch(uint8_t currentSlot){
#if IDOTMATRIX_CAROUSEL_GIF_PREFETCH
  cancelCarouselGifPrefetch("reschedule");
  if(!gifCacheConfigured() || !littleFsReady || currentSlot>=CAROUSEL_SLOT_COUNT) return;
  int8_t next=nextCarouselSlot((int8_t)currentSlot);
  if(next<0 || next>=CAROUSEL_SLOT_COUNT) return;
  const CarouselSlotMeta &m=carouselSlots[(uint8_t)next];
  if(!m.configured || m.dataType!=1 || !m.mediaSize || !m.mediaCRC){
    carouselGifPrefetch.slot=(uint8_t)next;
    skipCarouselGifPrefetch(m.dataType==3 ? "next_text" : (m.dataType==2 ? "next_image" : "metadata"));
    return;
  }
  String path=carouselFileName((uint8_t)next,1);
  carouselGifPrefetch.slot=(uint8_t)next;
  carouselGifPrefetch.size=m.mediaSize;
  carouselGifPrefetch.expectedCRC=m.mediaCRC;
  strncpy(carouselGifPrefetch.path,path.c_str(),GIF_CACHE_PATH_MAX-1);
  carouselGifPrefetch.path[GIF_CACHE_PATH_MAX-1]='\0';
  if(m.mediaSize>(uint32_t)IDOTMATRIX_GIF_PSRAM_CACHE_ENTRY_MAX_BYTES){
    skipCarouselGifPrefetch("entry_cap");
    return;
  }
  if(gifCacheMatches(carouselGifPrefetch.path,m.mediaSize,m.mediaCRC)){
    skipCarouselGifPrefetch("cached");
    return;
  }
  carouselGifPrefetch.scheduled=true;
  carouselGifPrefetch.readyAt=millis()+(uint32_t)IDOTMATRIX_CAROUSEL_GIF_PREFETCH_DELAY_MS;
  reportCarouselGifPrefetch("scheduled","next");
#else
  (void)currentSlot;
#endif
}

bool beginCarouselGifPrefetch(){
#if IDOTMATRIX_CAROUSEL_GIF_PREFETCH
  if(!carouselGifPrefetch.scheduled || carouselGifPrefetch.active) return false;
  if(!carouselActive || carouselActiveSlot<0){ cancelCarouselGifPrefetch("inactive"); return false; }
  if(gifCarouselPlaybackFileActive && gifCacheActiveIndex<0 && !gifStageData){
    skipCarouselGifPrefetch("current_fs");
    return false;
  }
  if(carouselGifPrefetch.slot>=CAROUSEL_SLOT_COUNT){ failCarouselGifPrefetch("slot"); return false; }
  const CarouselSlotMeta &m=carouselSlots[carouselGifPrefetch.slot];
  if(!m.configured || m.dataType!=1 || m.mediaSize!=carouselGifPrefetch.size || m.mediaCRC!=carouselGifPrefetch.expectedCRC){
    failCarouselGifPrefetch("stale"); return false;
  }
  if(gifCacheMatches(carouselGifPrefetch.path,(uint32_t)carouselGifPrefetch.size,carouselGifPrefetch.expectedCRC)){
    skipCarouselGifPrefetch("cached"); return false;
  }
  if(!LittleFS.exists(carouselGifPrefetch.path)){ failCarouselGifPrefetch("missing"); return false; }
  const uint32_t caps=MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
  const size_t freePsram=heap_caps_get_free_size(caps);
  const size_t largestPsram=heap_caps_get_largest_free_block(caps);
  const size_t reserve=(size_t)IDOTMATRIX_GIF_PSRAM_RESERVE_BYTES;
  if(carouselGifPrefetch.size>largestPsram){ skipCarouselGifPrefetch("largest"); return false; }
  if(carouselGifPrefetch.size>freePsram || freePsram-carouselGifPrefetch.size<reserve){ skipCarouselGifPrefetch("reserve"); return false; }
  carouselGifPrefetch.file=LittleFS.open(carouselGifPrefetch.path,"r");
  if(!carouselGifPrefetch.file || (size_t)carouselGifPrefetch.file.size()!=carouselGifPrefetch.size){
    if(carouselGifPrefetch.file) carouselGifPrefetch.file.close();
    failCarouselGifPrefetch("source"); return false;
  }
#if IDOTMATRIX_MEMORY_TELEMETRY
  idotMemoryTelemetrySnapshot("gif.prefetch.before");
#endif
  uint8_t *buf=(uint8_t *)heap_caps_malloc(carouselGifPrefetch.size,caps);
  if(!buf){ carouselGifPrefetch.file.close(); skipCarouselGifPrefetch("alloc"); return false; }
  carouselGifPrefetch.data=buf;
  carouselGifPrefetch.copied=0;
  carouselGifPrefetch.runningCRC=0xFFFFFFFFUL;
  carouselGifPrefetch.startedAtUs=micros();
  carouselGifPrefetch.active=true;
  carouselGifPrefetchAttempts++;
  reportCarouselGifPrefetch("begin","copy");
#if IDOTMATRIX_MEMORY_TELEMETRY
  idotMemoryTelemetrySnapshot("gif.prefetch.begin");
#endif
  return true;
#else
  return false;
#endif
}

void finishCarouselGifPrefetch(){
#if IDOTMATRIX_CAROUSEL_GIF_PREFETCH
  if(!carouselGifPrefetch.active) return;
  if(carouselGifPrefetch.file) carouselGifPrefetch.file.close();
  const uint32_t calc=carouselGifPrefetch.runningCRC^0xFFFFFFFFUL;
  if(carouselGifPrefetch.copied!=carouselGifPrefetch.size || calc!=carouselGifPrefetch.expectedCRC){
    failCarouselGifPrefetch(calc!=carouselGifPrefetch.expectedCRC ? "crc" : "short"); return;
  }
  const CarouselSlotMeta &m=carouselSlots[carouselGifPrefetch.slot];
  if(!m.configured || m.dataType!=1 || m.mediaSize!=carouselGifPrefetch.size || m.mediaCRC!=carouselGifPrefetch.expectedCRC){
    failCarouselGifPrefetch("stale"); return;
  }
  if(gifCacheMatches(carouselGifPrefetch.path,(uint32_t)carouselGifPrefetch.size,carouselGifPrefetch.expectedCRC)){
    skipCarouselGifPrefetch("cached"); return;
  }
  int8_t inserted=-1;
  if(!gifCacheInsertOwnedBuffer(carouselGifPrefetch.path,carouselGifPrefetch.size,carouselGifPrefetch.expectedCRC,
                                carouselGifPrefetch.data,"prefetch",&inserted)){
    failCarouselGifPrefetch("cache"); return;
  }
  carouselGifPrefetch.data=nullptr; // ownership transferred to cache
  carouselGifPrefetchOk++;
  reportCarouselGifPrefetch("cached","ok");
#if IDOTMATRIX_MEMORY_TELEMETRY
  idotMemoryTelemetryLatency("gif.prefetch.total_us",(uint32_t)(micros()-carouselGifPrefetch.startedAtUs));
  idotMemoryTelemetrySnapshot("gif.prefetch.cached");
#endif
  resetCarouselGifPrefetch(false);
#else
#endif
}

void serviceCarouselGifPrefetch(uint32_t now){
#if IDOTMATRIX_CAROUSEL_GIF_PREFETCH
  if(!carouselGifPrefetch.scheduled && !carouselGifPrefetch.active) return;
  if(!carouselActive || carouselActiveSlot<0){ cancelCarouselGifPrefetch("inactive"); return; }
  if(!carouselGifPrefetch.active){
    if((int32_t)(now-carouselGifPrefetch.readyAt)<0) return;
    if(!beginCarouselGifPrefetch()) return;
  }
  if(!carouselGifPrefetch.active || !carouselGifPrefetch.file || !carouselGifPrefetch.data) return;
  if(carouselGifPrefetch.slot>=CAROUSEL_SLOT_COUNT){ failCarouselGifPrefetch("slot"); return; }
  const CarouselSlotMeta &m=carouselSlots[carouselGifPrefetch.slot];
  if(!m.configured || m.dataType!=1 || m.mediaSize!=carouselGifPrefetch.size || m.mediaCRC!=carouselGifPrefetch.expectedCRC){
    cancelCarouselGifPrefetch("stale"); return;
  }
  const size_t remaining=carouselGifPrefetch.size-carouselGifPrefetch.copied;
  const size_t want=min((size_t)IDOTMATRIX_CAROUSEL_GIF_PREFETCH_CHUNK_BYTES,remaining);
  int n=carouselGifPrefetch.file.read(carouselGifPrefetch.data+carouselGifPrefetch.copied,want);
  if(n<=0){ failCarouselGifPrefetch("read"); return; }
  carouselGifPrefetch.runningCRC=crc32Update(carouselGifPrefetch.runningCRC,
                                             carouselGifPrefetch.data+carouselGifPrefetch.copied,(size_t)n);
  carouselGifPrefetch.copied+=(size_t)n;
  if(carouselGifPrefetch.copied>=carouselGifPrefetch.size) finishCarouselGifPrefetch();
#else
  (void)now;
#endif
}

void destroyGIFDecoder(){
  gifPlaying=false; gifLoaded=false; gifRestartPending=false; gifFrameWasDrawn=false;
  if(gif){
#if DEBUG_SERIAL && BULK_PROTOCOL_DEBUG
    Serial.println("GIF OLD close");
#endif
    gif->close();
#if DEBUG_SERIAL && BULK_PROTOCOL_DEBUG
    Serial.println("GIF OLD delete");
#endif
    delete gif;
    gif=nullptr;
  }
  // Source memory must outlive AnimatedGIF::close(). A transient stage can be
  // freed now; a cache entry is merely unpinned and remains available to LRU.
  gifCacheActiveIndex=-1;
  releaseGifStage();
}
void stopGIFPlayback(){
  bool removeEventFile=gifEventPlaybackFileActive;
  destroyGIFDecoder();
  gifEventPlaybackFileActive=false;
  gifCarouselPlaybackFileActive=false;
  gifPresetPlaybackFileActive=false;
  if(removeEventFile && littleFsReady){
    gifCacheInvalidatePath(EVENT_GIF_PLAY_FILE);
    LittleFS.remove(EVENT_GIF_PLAY_FILE);
  }
}
void freeGIF(){
  pendingGifStart=false;
  pendingGifSlot=-1;
  pendingGifSize=0;
  pendingGifCRC=0;
  pendingGifStartAt=0;
  gifDecoderTeardownPending=false;
  gifDecoderOpenPending=false;
  gifDecoderOpenAt=0;
  stopGIFPlayback();
  gifSize=0;
  gifPlayCRC=0;
  gifStoredOnFS=false;
}

// AnimatedGIF source callbacks.
// Select a pinned persistent cache entry, a transient stage, or the original
// LittleFS file behind one callback API.
void *GIFOpenFile(const char *fname, int32_t *pSize){
  if(!pSize || !fname) return nullptr;
  GifSourceHandle *h=new GifSourceHandle();
  if(!h) return nullptr;
  if(gifCacheActiveIndex>=0){
    GifCacheEntry &e=gifCache[(uint8_t)gifCacheActiveIndex];
    if(e.valid && e.data && gifCachePathEquals((uint8_t)gifCacheActiveIndex,fname)){
      h->source=2;
      h->cacheIndex=gifCacheActiveIndex;
      *pSize=(int32_t)e.size;
      return (void *)h;
    }
  }
  if(gifStageData && gifStageSize && gifStagePath==fname){
    h->source=1;
    *pSize=(int32_t)gifStageSize;
    return (void *)h;
  }
  if(!littleFsReady){ delete h; return nullptr; }
  h->file=LittleFS.open(fname,"r");
  if(!h->file){ delete h; return nullptr; }
  *pSize=(int32_t)h->file.size();
  return (void *)h;
}
void GIFCloseFile(void *pHandle){
  GifSourceHandle *h=(GifSourceHandle *)pHandle;
  if(!h) return;
  if(h->source==0 && h->file) h->file.close();
  delete h;
}
int32_t GIFReadFile(GIFFILE *pFile,uint8_t *pBuf,int32_t iLen){
  if(!pFile || !pBuf || iLen<=0) return 0;
  GifSourceHandle *h=(GifSourceHandle *)pFile->fHandle;
  if(!h) return 0;
  int32_t remain=pFile->iSize-pFile->iPos;
  if(remain<=0) return 0;
  if(iLen>remain) iLen=remain;
  if(h->source==2){
    if(h->cacheIndex<0 || h->cacheIndex>=IDOTMATRIX_GIF_CACHE_STORAGE_ENTRIES) return 0;
    GifCacheEntry &e=gifCache[(uint8_t)h->cacheIndex];
    if(!e.valid || !e.data || pFile->iPos<0 || (size_t)pFile->iPos+(size_t)iLen>e.size) return 0;
    memcpy(pBuf,e.data+(size_t)pFile->iPos,(size_t)iLen);
    pFile->iPos+=iLen;
    return iLen;
  }
  if(h->source==1){
    if(!gifStageData || pFile->iPos<0 || (size_t)pFile->iPos+(size_t)iLen>gifStageSize) return 0;
    memcpy(pBuf,gifStageData+(size_t)pFile->iPos,(size_t)iLen);
    pFile->iPos+=iLen;
    return iLen;
  }
  if(!h->file) return 0;
  int32_t n=(int32_t)h->file.read(pBuf,(size_t)iLen);
  pFile->iPos=(int32_t)h->file.position();
  return n;
}
int32_t GIFSeekFile(GIFFILE *pFile,int32_t iPosition){
  if(!pFile) return -1;
  GifSourceHandle *h=(GifSourceHandle *)pFile->fHandle;
  if(!h) return -1;
  if(iPosition<0) iPosition=0;
  if(iPosition>pFile->iSize) iPosition=pFile->iSize;
  if(h->source==2){
    if(h->cacheIndex<0 || h->cacheIndex>=IDOTMATRIX_GIF_CACHE_STORAGE_ENTRIES) return -1;
    GifCacheEntry &e=gifCache[(uint8_t)h->cacheIndex];
    if(!e.valid || !e.data || (size_t)iPosition>e.size) return -1;
    pFile->iPos=iPosition;
    return pFile->iPos;
  }
  if(h->source==1){
    if(!gifStageData || (size_t)iPosition>gifStageSize) return -1;
    pFile->iPos=iPosition;
    return pFile->iPos;
  }
  if(!h->file || !h->file.seek((uint32_t)iPosition,SeekSet)) return -1;
  pFile->iPos=(int32_t)h->file.position();
  return pFile->iPos;
}

void GIFDraw(GIFDRAW *pDraw){
  if(!pDraw) return; gifFrameWasDrawn=true;
  int y=pDraw->iY+pDraw->y; if(y<0||y>=MATRIX_HEIGHT) return;
  int width=pDraw->iWidth; if(pDraw->iX+width>MATRIX_WIDTH) width=MATRIX_WIDTH-pDraw->iX; if(width<=0) return;
  uint8_t *pixels=pDraw->pPixels; uint16_t *palette=pDraw->pPalette; if(!palette) return;
  if(pDraw->ucDisposalMethod==2){
    for(int x=0;x<width;x++) if(pixels[x]==pDraw->ucTransparent) pixels[x]=pDraw->ucBackground;
    pDraw->ucHasTransparency=0;
  }
  for(int x=0;x<width;x++){
    int sx=pDraw->iX+x; if(sx<0||sx>=MATRIX_WIDTH) continue;
    uint8_t pi=pixels[x]; if(pDraw->ucHasTransparency && pi==pDraw->ucTransparent) continue;
    uint16_t rgb=palette[pi];
    uint8_t r=((rgb>>11)&0x1F)*255/31, g=((rgb>>5)&0x3F)*255/63, b=(rgb&0x1F)*255/31;
    gifFrame[logicalIndex(sx,y)]=CRGB(r,g,b);
  }
}

bool startGIFFile(const char *path, bool eventPlayback, bool carouselPlayback, uint32_t expectedCRC){
  if(!littleFsReady || !path || !gifSize || !LittleFS.exists(path)) return false;
  if(gif) destroyGIFDecoder();
  else releaseGifStage();
#if IDOTMATRIX_MEMORY_TELEMETRY
  gifTelemetryContext = carouselPlayback ? 2 : (eventPlayback ? 1 : 0);
  gifTelemetryFirstFramePending = false;
  idotMemoryTelemetrySnapshot(carouselPlayback ? "gif.carousel.before_open" : (eventPlayback ? "gif.event.before_open" : "gif.live.before_open"));
  // First-frame latency includes the staging copy so it reflects the
  // complete user-visible cold-start cost. Decoder open latency below remains
  // separately comparable with the file-backed baseline.
  gifTelemetryStartedAtUs = micros();
#endif
  // Best-effort only: failure leaves the file-backed fallback path intact.
  prepareGifStage(path,gifSize,expectedCRC);
  fill_solid(gifFrame,NUM_LEDS,CRGB::Black);
#if DEBUG_SERIAL && BULK_PROTOCOL_DEBUG
  Serial.println("GIF NEW alloc");
#endif
  gif=new AnimatedGIF();
  if(!gif){
#if DEBUG_SERIAL
    Serial.println("GIF NEW alloc FAILED");
#endif
    releaseGifStage();
    return false;
  }
#if DEBUG_SERIAL && BULK_PROTOCOL_DEBUG
  Serial.println("GIF NEW begin");
#endif
  gif->begin(LITTLE_ENDIAN_PIXELS); gif->setDrawType(GIF_DRAW_RAW);
#if DEBUG_SERIAL && BULK_PROTOCOL_DEBUG
  Serial.println("GIF NEW open");
#endif
#if IDOTMATRIX_MEMORY_TELEMETRY
  const uint32_t gifDecoderOpenStartedAtUs=micros();
#endif
  bool opened=gif->open(path,GIFOpenFile,GIFCloseFile,GIFReadFile,GIFSeekFile,GIFDraw);
  if(!opened){
#if DEBUG_SERIAL
    Serial.println("GIF NEW open FAILED");
#endif
    destroyGIFDecoder();
    return false;
  }
#if DEBUG_SERIAL && BULK_PROTOCOL_DEBUG
  Serial.println("GIF NEW open OK");
  Serial.println("GIF NEW reset");
#endif
  gif->reset();
#if DEBUG_SERIAL && BULK_PROTOCOL_DEBUG
  Serial.println("GIF NEW reset OK");
#endif
  gifStoredOnFS=true;
  gifEventPlaybackFileActive=eventPlayback;
  gifCarouselPlaybackFileActive=carouselPlayback;
#if IDOTMATRIX_MEMORY_TELEMETRY
  idotMemoryTelemetryLatency(gifTelemetryTag("open"), (uint32_t)(micros() - gifDecoderOpenStartedAtUs));
  idotMemoryTelemetrySnapshot(carouselPlayback ? "gif.carousel.after_open" : (eventPlayback ? "gif.event.after_open" : "gif.live.after_open"));
  gifTelemetryFirstFramePending = true;
#endif
  gifLoaded=gifPlaying=true; gifNextFrameAt=millis(); displayMode=DISPLAY_GIF; return true;
}

bool startGIF(){
  if(presetActive) stopPresetPlayback();
  carouselActive=false;
  carouselActiveSlot=-1;
  carouselStartPending=false;
  carouselEnterRequested=false;
  carouselUploadOpen=false;
  carouselUploadBlackout=false;
  endCarouselTransferIndicator();
  return startGIFFile(GIF_PLAY_FILE,false,false,gifPlayCRC);
}

bool startStoredGIFPlayback(const String &sourcePath, uint32_t expectedSize, uint32_t expectedCRC){
  if(presetActive) stopPresetPlayback();
  carouselActive=false;
  carouselActiveSlot=-1;
  carouselStartPending=false;
  carouselEnterRequested=false;
  carouselUploadOpen=false;
  carouselUploadBlackout=false;
  endCarouselTransferIndicator();
  if(!littleFsReady || !expectedSize) return false;
  File src=LittleFS.open(sourcePath,"r");
  if(!src || (uint32_t)src.size()!=expectedSize){ if(src) src.close(); return false; }

  // Never decode an Alarm/Schedule source file directly: it can later be
  // renamed as part of a transactional update. Copy it to a disposable PLAY
  // file so AnimatedGIF owns a stable file for the whole playback lifetime.
  stopGIFPlayback();
  gifCacheInvalidatePath(EVENT_GIF_PLAY_FILE);
  LittleFS.remove(EVENT_GIF_PLAY_FILE);
  size_t fsTotal=LittleFS.totalBytes();
  size_t fsUsed=LittleFS.usedBytes();
  size_t fsFree=fsTotal>fsUsed ? fsTotal-fsUsed : 0;
  if((uint64_t)expectedSize>(uint64_t)fsFree){
#if DEBUG_SERIAL
    Serial.print("EVENT GIF rejected: bytes="); Serial.print(expectedSize);
    Serial.print(" LittleFS free="); Serial.println(fsFree);
#endif
    src.close();
    return false;
  }

  File dst=LittleFS.open(EVENT_GIF_PLAY_FILE,"w");
  if(!dst){ src.close(); return false; }
  uint8_t buf[512];
  uint32_t remaining=expectedSize;
  uint32_t runningCRC=0xFFFFFFFF;
  bool ok=true;
  while(remaining && ok){
    size_t want=min((size_t)remaining,sizeof(buf));
    int n=src.read(buf,want);
    if(n<=0){ ok=false; break; }
    runningCRC=crc32Update(runningCRC,buf,(size_t)n);
    if(dst.write(buf,(size_t)n)!=(size_t)n){ ok=false; break; }
    remaining-=(uint32_t)n;
  }
  dst.flush();
  dst.close();
  src.close();
  runningCRC^=0xFFFFFFFF;
  if(!ok || remaining || runningCRC!=expectedCRC || storedFileSize(EVENT_GIF_PLAY_FILE)!=expectedSize){
#if DEBUG_SERIAL
    Serial.println("EVENT GIF copy/CRC failed");
#endif
    LittleFS.remove(EVENT_GIF_PLAY_FILE);
    return false;
  }

  gifSize=expectedSize;
  gifStoredOnFS=true;
#if DEBUG_SERIAL && BULK_PROTOCOL_DEBUG
  Serial.print("EVENT GIF PLAY copy bytes="); Serial.print(expectedSize);
  Serial.print(" source="); Serial.println(sourcePath);
#endif
  if(!startGIFFile(EVENT_GIF_PLAY_FILE,true,false,expectedCRC)){
    gifStoredOnFS=false;
    gifSize=0;
    LittleFS.remove(EVENT_GIF_PLAY_FILE);
    return false;
  }
  return true;
}

void updateGIF(){
  if(!gif || !gifPlaying||!gifLoaded||displayMode!=DISPLAY_GIF) return;
  uint32_t now=millis(); if((long)(now-gifNextFrameAt)<0) return;
  if(gifRestartPending){ gif->reset(); gifRestartPending=false; fill_solid(gifFrame,NUM_LEDS,CRGB::Black); }
  gifFrameWasDrawn=false; int delayMs=0; int result=gif->playFrame(false,&delayMs);
  if(!gifFrameWasDrawn){ gif->reset(); gifNextFrameAt=now+10; return; }
  ::memcpy(framebuffer,gifFrame,LOGICAL_FRAME_BYTES); refreshMatrix();
#if IDOTMATRIX_MEMORY_TELEMETRY
  if(gifTelemetryFirstFramePending){
    gifTelemetryFirstFramePending=false;
    idotMemoryTelemetryLatency(gifTelemetryTag("first"), (uint32_t)(micros() - gifTelemetryStartedAtUs));
    idotMemoryTelemetrySnapshot(gifTelemetryContext==2 ? "gif.carousel.first_frame" : (gifTelemetryContext==1 ? "gif.event.first_frame" : "gif.live.first_frame"));
  }
#endif
  if(delayMs<10) delayMs=10; gifNextFrameAt=now+delayMs; if(result==0) gifRestartPending=true;
}

// ======================================================
// DEVICE-ASSET CAROUSEL
// ======================================================
String carouselFileName(uint8_t slot, uint8_t dataType) {
  // Keep the legacy .raw internal filename for dataType 2 so transaction
  // journals from earlier firmware revisions remain rollback-compatible. The stored bytes
  // may be either raw RGB24 or a PNG container; dataType 2 means static IMAGE.
  const char *ext=(dataType==3) ? ".txt" : ((dataType==2) ? ".raw" : ".gif");
  return String("/car") + slot + ext;
}
String carouselTempFileName(uint8_t slot) { return String("/car") + slot + ".tmp"; }
String carouselBackupFileName(uint8_t slot) { return String("/car") + slot + ".bak"; }
String carouselBankBackupFileName(uint8_t slot, uint8_t dataType) {
  const char *ext=(dataType==3) ? ".old.txt" : ((dataType==2) ? ".old.raw" : ".old.gif");
  return String("/car") + slot + ext;
}
bool carouselDataTypeSupported(uint8_t dataType) { return dataType==1 || dataType==2 || dataType==3; }
const char *carouselDataTypeLabel(uint8_t dataType) {
  return dataType==3 ? "TEXT" : (dataType==2 ? "IMAGE" : "GIF");
}
void removeCarouselMediaFiles(uint8_t slot) {
  if(!littleFsReady || slot>=CAROUSEL_SLOT_COUNT) return;
  LittleFS.remove(carouselFileName(slot,1));
  LittleFS.remove(carouselFileName(slot,2));
  LittleFS.remove(carouselFileName(slot,3));
}
void removeCarouselMediaFilesExcept(uint8_t slot, uint8_t keepType) {
  if(!littleFsReady || slot>=CAROUSEL_SLOT_COUNT) return;
  for(uint8_t type=1; type<=3; type++) if(type!=keepType) LittleFS.remove(carouselFileName(slot,type));
}

uint32_t carouselBankTxnManifestCRC() {
  uint32_t crc=0xFFFFFFFFUL;
  crc=crc32Update(crc,reinterpret_cast<const uint8_t*>(&carouselBankTxnManifest),
                  sizeof(CarouselBankTxnManifest)-sizeof(carouselBankTxnManifest.crc));
  return crc^0xFFFFFFFFUL;
}

bool carouselBankTxnManifestIsValid() {
  if(carouselBankTxnManifest.magic!=CAROUSEL_BANK_TXN_MAGIC ||
     carouselBankTxnManifest.version!=CAROUSEL_BANK_TXN_VERSION ||
     carouselBankTxnManifest.bytes!=sizeof(CarouselBankTxnManifest) ||
     carouselBankTxnManifest.orderCount>CAROUSEL_SLOT_COUNT ||
     (carouselBankTxnManifest.touchedMask & ~((1U<<CAROUSEL_SLOT_COUNT)-1U)) ||
     (carouselBankTxnManifest.mediaPresentMask & ~carouselBankTxnManifest.touchedMask)) return false;
  bool seen[CAROUSEL_SLOT_COUNT]={false};
  for(uint8_t i=0;i<carouselBankTxnManifest.orderCount;i++){
    const uint8_t slot=carouselBankTxnManifest.order[i];
    if(slot>=CAROUSEL_SLOT_COUNT || seen[slot]) return false;
    seen[slot]=true;
  }
  for(uint8_t i=0;i<CAROUSEL_SLOT_COUNT;i++){
    const CarouselSlotMeta &m=carouselBankTxnManifest.slots[i];
    if(m.configured && (m.dataType!=1 && m.dataType!=2 && m.dataType!=3)) return false;
  }
  return carouselBankTxnManifest.crc==carouselBankTxnManifestCRC();
}

void cleanupCarouselBankBackupFiles() {
  if(!littleFsReady) return;
  for(uint8_t i=0;i<CAROUSEL_SLOT_COUNT;i++){
    LittleFS.remove(carouselBankBackupFileName(i,1));
    LittleFS.remove(carouselBankBackupFileName(i,2));
    LittleFS.remove(carouselBankBackupFileName(i,3));
  }
  LittleFS.remove(CAROUSEL_BANK_TXN_TMP_FILE);
}

bool loadCarouselBankTxnManifest() {
  if(!littleFsReady || !LittleFS.exists(CAROUSEL_BANK_TXN_FILE)) return false;
  File f=LittleFS.open(CAROUSEL_BANK_TXN_FILE,"r");
  if(!f || (size_t)f.size()!=sizeof(CarouselBankTxnManifest)){ if(f) f.close(); return false; }
  const size_t got=f.read(reinterpret_cast<uint8_t*>(&carouselBankTxnManifest),sizeof(CarouselBankTxnManifest));
  f.close();
  return got==sizeof(CarouselBankTxnManifest) && carouselBankTxnManifestIsValid();
}

bool persistCarouselBankTxnManifest() {
  if(!littleFsReady) return false;
  carouselBankTxnManifest.crc=carouselBankTxnManifestCRC();
  LittleFS.remove(CAROUSEL_BANK_TXN_TMP_FILE);
  File f=LittleFS.open(CAROUSEL_BANK_TXN_TMP_FILE,"w");
  if(!f) return false;
  const size_t wrote=f.write(reinterpret_cast<const uint8_t*>(&carouselBankTxnManifest),sizeof(CarouselBankTxnManifest));
  f.flush(); f.close();
  if(wrote!=sizeof(CarouselBankTxnManifest)){ LittleFS.remove(CAROUSEL_BANK_TXN_TMP_FILE); return false; }
  LittleFS.remove(CAROUSEL_BANK_TXN_FILE);
  if(!LittleFS.rename(CAROUSEL_BANK_TXN_TMP_FILE,CAROUSEL_BANK_TXN_FILE)) return false;
  CarouselBankTxnManifest verify;
  File vf=LittleFS.open(CAROUSEL_BANK_TXN_FILE,"r");
  if(!vf || (size_t)vf.size()!=sizeof(verify)){ if(vf) vf.close(); return false; }
  const size_t got=vf.read(reinterpret_cast<uint8_t*>(&verify),sizeof(verify));
  vf.close();
  if(got!=sizeof(verify) || memcmp(&verify,&carouselBankTxnManifest,sizeof(verify))!=0) return false;
  return true;
}

void prepareCarouselBankTxnManifest(uint16_t touchedMask) {
  carouselBankTxnManifest=CarouselBankTxnManifest();
  carouselBankTxnManifest.bytes=sizeof(CarouselBankTxnManifest);
  carouselBankTxnManifest.touchedMask=touchedMask;
  carouselBankTxnManifest.orderCount=carouselOrderCount;
  memcpy(carouselBankTxnManifest.order,carouselOrder,CAROUSEL_SLOT_COUNT);
  for(uint8_t i=0;i<CAROUSEL_SLOT_COUNT;i++){
    carouselBankTxnManifest.slots[i]=carouselSlots[i];
    const CarouselSlotMeta &m=carouselSlots[i];
    if(!(touchedMask&(1U<<i)) || !m.configured || (m.dataType!=1 && m.dataType!=2 && m.dataType!=3)) continue;
    if(LittleFS.exists(carouselFileName(i,m.dataType))) carouselBankTxnManifest.mediaPresentMask|=(1U<<i);
  }
}

bool restoreCarouselBankPreferences() {
  if(!carouselPrefsReady) return false;
  bool ok=true;
  carouselOrderCount=carouselBankTxnManifest.orderCount;
  memcpy(carouselOrder,carouselBankTxnManifest.order,CAROUSEL_SLOT_COUNT);
  if(carouselPrefs.putUChar("count",carouselOrderCount)!=1) ok=false;
  if(carouselPrefs.putBytes("order",carouselOrder,CAROUSEL_SLOT_COUNT)!=CAROUSEL_SLOT_COUNT) ok=false;
  for(uint8_t i=0;i<CAROUSEL_SLOT_COUNT;i++){
    carouselSlots[i]=carouselBankTxnManifest.slots[i];
    char key[8]; snprintf(key,sizeof(key),"c%u",i);
    if(carouselSlots[i].configured){
      if(carouselPrefs.putBytes(key,&carouselSlots[i],sizeof(CarouselSlotMeta))!=sizeof(CarouselSlotMeta)) ok=false;
    } else if(carouselPrefs.isKey(key) && !carouselPrefs.remove(key)) ok=false;
  }
  return ok;
}

bool rollbackCarouselBankTransaction(const char *reason) {
  if(!littleFsReady || !carouselPrefsReady) return false;
  if(!carouselBankTxnActive){
    if(!loadCarouselBankTxnManifest()) return !LittleFS.exists(CAROUSEL_BANK_TXN_FILE);
    carouselBankTxnActive=true;
  }
  bool ok=true;
  for(uint8_t i=0;i<CAROUSEL_SLOT_COUNT;i++){
    const uint16_t bit=(uint16_t)(1U<<i);
    if(!(carouselBankTxnManifest.touchedMask&bit)) continue;
    const CarouselSlotMeta &old=carouselBankTxnManifest.slots[i];
    String gifPath=carouselFileName(i,1);
    gifCacheInvalidatePath(gifPath.c_str());
    LittleFS.remove(carouselTempFileName(i));
    LittleFS.remove(carouselBackupFileName(i));
    if(carouselBankTxnManifest.mediaPresentMask&bit){
      if(!old.configured || !carouselDataTypeSupported(old.dataType)){ ok=false; continue; }
      String oldPath=carouselFileName(i,old.dataType);
      String oldBak=carouselBankBackupFileName(i,old.dataType);
      if(LittleFS.exists(oldBak)){
        removeCarouselMediaFiles(i);
        if(!LittleFS.rename(oldBak,oldPath)) ok=false;
      } else if(!LittleFS.exists(oldPath)){
        ok=false;
      }
      removeCarouselMediaFilesExcept(i,old.dataType);
    } else {
      removeCarouselMediaFiles(i);
    }
    for(uint8_t type=1; type<=3; type++) if(type!=old.dataType) LittleFS.remove(carouselBankBackupFileName(i,type));
  }
  if(ok) ok=restoreCarouselBankPreferences();
  if(!ok){
#if DEBUG_SERIAL
    Serial.print("[CARBANK] state=rollback_failed reason="); Serial.println(reason ? reason : "unknown");
#endif
    return false;
  }
  // Old bank is authoritative again. Removing the manifest commits rollback;
  // any leftover .old file after this point is disposable cleanup only.
  if(!LittleFS.remove(CAROUSEL_BANK_TXN_FILE) && LittleFS.exists(CAROUSEL_BANK_TXN_FILE)) return false;
  carouselBankTxnActive=false;
  cleanupCarouselBankBackupFiles();
#if DEBUG_SERIAL
  Serial.print("[CARBANK] state=rollback reason="); Serial.print(reason ? reason : "unknown");
  Serial.print(" touched=0x"); Serial.println(carouselBankTxnManifest.touchedMask,HEX);
#endif
  return true;
}

bool beginCarouselBankTransaction(uint16_t touchedMask) {
  if(!littleFsReady || !carouselPrefsReady) return false;
  if(carouselBankTxnActive || LittleFS.exists(CAROUSEL_BANK_TXN_FILE)){
    if(!rollbackCarouselBankTransaction("restart")) return false;
  } else {
    cleanupCarouselBankBackupFiles();
  }
  prepareCarouselBankTxnManifest(touchedMask);
  if(!persistCarouselBankTxnManifest()){
    LittleFS.remove(CAROUSEL_BANK_TXN_FILE);
    LittleFS.remove(CAROUSEL_BANK_TXN_TMP_FILE);
    cleanupCarouselBankBackupFiles();
    return false;
  }
  carouselBankTxnActive=true;
  for(uint8_t i=0;i<CAROUSEL_SLOT_COUNT;i++){
    const uint16_t bit=(uint16_t)(1U<<i);
    if(!(carouselBankTxnManifest.mediaPresentMask&bit)) continue;
    const CarouselSlotMeta &old=carouselBankTxnManifest.slots[i];
    String src=carouselFileName(i,old.dataType);
    String bak=carouselBankBackupFileName(i,old.dataType);
    LittleFS.remove(bak);
    if(!LittleFS.exists(src) || !LittleFS.rename(src,bak)){
      rollbackCarouselBankTransaction("backup_failed");
      return false;
    }
  }
#if DEBUG_SERIAL
  Serial.print("[CARBANK] state=begin touched=0x"); Serial.print(touchedMask,HEX);
  Serial.print(" media=0x"); Serial.println(carouselBankTxnManifest.mediaPresentMask,HEX);
#endif
  return true;
}

bool finalizeCarouselBankTransaction() {
  if(!carouselBankTxnActive) return true;
  // Manifest removal is the publication commit point. If power disappears
  // before this succeeds, boot recovery restores the previous complete bank.
  // If power disappears after it succeeds, the new bank remains authoritative
  // and any leftover .old files are harmless stale cleanup.
  if(!LittleFS.remove(CAROUSEL_BANK_TXN_FILE) && LittleFS.exists(CAROUSEL_BANK_TXN_FILE)) return false;
  carouselBankTxnActive=false;
  cleanupCarouselBankBackupFiles();
#if DEBUG_SERIAL
  Serial.println("[CARBANK] state=commit");
#endif
  return true;
}

bool saveCarouselSlotMeta(uint8_t slot) {
  if(!carouselPrefsReady || slot>=CAROUSEL_SLOT_COUNT) return false;
  char key[8]; snprintf(key,sizeof(key),"c%u",slot);
  return carouselPrefs.putBytes(key,&carouselSlots[slot],sizeof(CarouselSlotMeta)) == sizeof(CarouselSlotMeta);
}

void removeCarouselSlotMeta(uint8_t slot) {
  if(slot>=CAROUSEL_SLOT_COUNT) return;
  if(carouselPrefsReady){
    char key[8]; snprintf(key,sizeof(key),"c%u",slot);
    if(carouselPrefs.isKey(key)) carouselPrefs.remove(key);
  }
  carouselSlots[slot]=CarouselSlotMeta();
}

void clearCarouselSlot(uint8_t slot) {
  if(slot>=CAROUSEL_SLOT_COUNT) return;
  String gifPath=carouselFileName(slot,1);
  gifCacheInvalidatePath(gifPath.c_str());
  if(littleFsReady){
    LittleFS.remove(carouselTempFileName(slot));
    LittleFS.remove(carouselBackupFileName(slot));
    removeCarouselMediaFiles(slot);
  }
  removeCarouselSlotMeta(slot);
}

void saveCarouselOrder() {
  if(!carouselPrefsReady) return;
  carouselPrefs.putUChar("count",carouselOrderCount);
  carouselPrefs.putBytes("order",carouselOrder,CAROUSEL_SLOT_COUNT);
}

void recoverCarouselSlot(uint8_t slot) {
  if(!littleFsReady || slot>=CAROUSEL_SLOT_COUNT) return;
  String tmp=carouselTempFileName(slot), bak=carouselBackupFileName(slot);
  LittleFS.remove(tmp);
  const CarouselSlotMeta &m=carouselSlots[slot];
  if(!m.configured || !m.mediaSize || !carouselDataTypeSupported(m.dataType)){
    removeCarouselMediaFiles(slot);
    LittleFS.remove(bak);
    return;
  }
  String dst=carouselFileName(slot,m.dataType);
  bool dstOK=fileMatchesMedia(dst,m.mediaSize,m.mediaCRC);
  bool bakOK=fileMatchesMedia(bak,m.mediaSize,m.mediaCRC);
  if(dstOK) LittleFS.remove(bak);
  else if(bakOK){
    LittleFS.remove(dst);
    if(!LittleFS.rename(bak,dst)){
#if DEBUG_SERIAL
      Serial.print("CAROUSEL recovery rename failed slot="); Serial.println(slot);
#endif
    }
  }
  // Never leave stale files of the other supported slot types around.
  removeCarouselMediaFilesExcept(slot,m.dataType);
}

void loadCarousel() {
  carouselPrefsReady=carouselPrefs.begin("idot-car",false);
  // An on-disk manifest means the previous whole-bank replacement never
  // reached its publication commit point. Recover the old bank before reading
  // active order/slot metadata into runtime state.
  if(littleFsReady && carouselPrefsReady && LittleFS.exists(CAROUSEL_BANK_TXN_FILE)){
    if(loadCarouselBankTxnManifest()){
      carouselBankTxnActive=true;
      rollbackCarouselBankTransaction("boot");
    } else {
#if DEBUG_SERIAL
      Serial.println("[CARBANK] state=manifest_invalid reason=boot");
#endif
    }
  } else if(littleFsReady) {
    // A successful commit removes the manifest before backup cleanup. Remove
    // any stale .old files left by a reset in that cleanup-only window.
    cleanupCarouselBankBackupFiles();
  }
  carouselOrderCount=0;
  for(uint8_t i=0;i<CAROUSEL_SLOT_COUNT;i++) carouselOrder[i]=0;
  if(carouselPrefsReady){
    uint8_t savedCount=carouselPrefs.getUChar("count",0);
    if(savedCount<=CAROUSEL_SLOT_COUNT && carouselPrefs.getBytesLength("order")==CAROUSEL_SLOT_COUNT){
      uint8_t savedOrder[CAROUSEL_SLOT_COUNT];
      carouselPrefs.getBytes("order",savedOrder,CAROUSEL_SLOT_COUNT);
      bool seen[CAROUSEL_SLOT_COUNT]={false};
      bool valid=true;
      for(uint8_t i=0;i<savedCount;i++){
        if(savedOrder[i]>=CAROUSEL_SLOT_COUNT || seen[savedOrder[i]]){ valid=false; break; }
        seen[savedOrder[i]]=true;
      }
      if(valid){
        carouselOrderCount=savedCount;
        memcpy(carouselOrder,savedOrder,CAROUSEL_SLOT_COUNT);
      }
    }
  }
  for(uint8_t i=0;i<CAROUSEL_SLOT_COUNT;i++){
    if(carouselPrefsReady){
      char key[8]; snprintf(key,sizeof(key),"c%u",i);
      if(carouselPrefs.getBytesLength(key)==sizeof(CarouselSlotMeta))
        carouselPrefs.getBytes(key,&carouselSlots[i],sizeof(CarouselSlotMeta));
      else if(carouselPrefs.isKey(key))
        carouselPrefs.remove(key); // discard metadata written by superseded pre-release layouts
    }
    recoverCarouselSlot(i);
#if DEBUG_SERIAL && CAROUSEL_PROTOCOL_DEBUG
    if(carouselSlots[i].configured){
      Serial.print("CAROUSEL LOAD slot="); Serial.print(i);
      Serial.print(" type="); Serial.print(carouselDataTypeLabel(carouselSlots[i].dataType));
      Serial.print(" dwell="); Serial.print(carouselSlots[i].dwellSeconds);
      Serial.print(" bytes="); Serial.print(carouselSlots[i].mediaSize);
      String path=carouselFileName(i,carouselSlots[i].dataType);
      Serial.print(" file="); Serial.println(littleFsReady && LittleFS.exists(path) ? "YES" : "NO");
    }
#endif
  }

  // An early pre-release format temporarily allowed slots 12..35. Remove
  // those leftovers so they cannot waste LittleFS/NVS space.
  if(littleFsReady){
    for(uint8_t i=12;i<36;i++){
      LittleFS.remove(String("/car")+i+".gif");
      LittleFS.remove(String("/car")+i+".raw");
      LittleFS.remove(String("/car")+i+".txt");
      LittleFS.remove(String("/car")+i+".tmp");
      LittleFS.remove(String("/car")+i+".bak");
    }
  }
  if(carouselPrefsReady){
    for(uint8_t i=12;i<36;i++){
      char key[8]; snprintf(key,sizeof(key),"c%u",i);
      if(carouselPrefs.isKey(key)) carouselPrefs.remove(key);
    }
  }
}

void stopCarouselPlayback() {
  cancelCarouselGifPrefetch("stop");
  carouselStartPending=false;
  carouselActive=false;
  carouselActiveSlot=-1;
  if(gifCarouselPlaybackFileActive){
    stopGIFPlayback();
    gifStoredOnFS=false;
    gifSize=0;
  }
  // Preserve the last logical carousel frame while a replacement bank is uploaded.
  // Preserve the physical-output blackout used during replacement.
  // Direct assignment is intentional: switchDisplayMode() would clear the
  // still-open Device Assets upload context.
  if(displayMode==DISPLAY_GIF || displayMode==DISPLAY_TEXT) displayMode=DISPLAY_RAW;
}

int8_t nextCarouselSlot(int8_t current) {
  if(!littleFsReady || !carouselOrderCount) return -1;
  int start=-1;
  for(uint8_t i=0;i<carouselOrderCount;i++) if((int8_t)carouselOrder[i]==current){ start=i; break; }
  for(uint8_t step=1; step<=carouselOrderCount; step++){
    uint8_t pos=(uint8_t)((start<0 ? step-1 : start+step)%carouselOrderCount);
    uint8_t slot=carouselOrder[pos];
    if(slot>=CAROUSEL_SLOT_COUNT) continue;
    const CarouselSlotMeta &m=carouselSlots[slot];
    if(!m.configured || !m.mediaSize || !carouselDataTypeSupported(m.dataType)) continue;
    if(LittleFS.exists(carouselFileName(slot,m.dataType))) return (int8_t)slot;
  }
  return -1;
}

bool loadCarouselRawFrame(const char *path, uint32_t expectedSize) {
  const uint32_t required=(uint32_t)NUM_LEDS*3UL;
  if(!littleFsReady || !path || expectedSize!=required) return false;
  File f=LittleFS.open(path,"r");
  if(!f || (uint32_t)f.size()!=required){ if(f) f.close(); return false; }
  uint8_t rgb[192];
  uint16_t pixel=0;
  while(pixel<NUM_LEDS){
    const uint16_t pixelsThis=min((uint16_t)(sizeof(rgb)/3U),(uint16_t)(NUM_LEDS-pixel));
    const size_t want=(size_t)pixelsThis*3U;
    if(f.read(rgb,want)!=want){ f.close(); return false; }
    for(uint16_t i=0;i<pixelsThis;i++){
      const size_t o=(size_t)i*3U;
      framebuffer[pixel+i]=CRGB(rgb[o],rgb[o+1],rgb[o+2]);
    }
    pixel=(uint16_t)(pixel+pixelsThis);
  }
  f.close();
  refreshMatrix();
  return true;
}

bool loadCarouselImageFrame(const char *path, uint32_t expectedSize) {
  if(!littleFsReady || !path || !expectedSize) return false;
  const uint32_t rawBytes=(uint32_t)NUM_LEDS*3UL;
  if(expectedSize==rawBytes) return loadCarouselRawFrame(path,expectedSize);

  File f=LittleFS.open(path,"r");
  if(!f || (uint32_t)f.size()!=expectedSize){ if(f) f.close(); return false; }
#if IDOTMATRIX_MEMORY_TELEMETRY
  idotMemoryTelemetrySnapshot("image.carousel.png.before_decode");
  const uint32_t imageStartedAtUs=micros();
#endif
  const bool ok=decodeSchedulePNG(f,expectedSize);
  f.close();
#if IDOTMATRIX_MEMORY_TELEMETRY
  idotMemoryTelemetryLatency(ok ? "image.carousel.png.decode_us" : "image.carousel.png.decode_failed_us",
                             (uint32_t)(micros()-imageStartedAtUs));
  idotMemoryTelemetrySnapshot(ok ? "image.carousel.png.after_decode" : "image.carousel.png.decode_failed");
#endif
  return ok;
}

bool startCarouselSlot(uint8_t slot) {
  // Any unfinished look-ahead belongs to the previous active slot. Drop it
  // before validating/opening the new source so it cannot consume reserve or
  // race a slot replacement.
  cancelCarouselGifPrefetch("advance");
#if IDOTMATRIX_MEMORY_TELEMETRY
  idotMemoryTelemetrySnapshot("carousel.before_start");
  const uint32_t carouselTelemetryStartedAtUs = micros();
#endif
  if(presetActive) stopPresetPlayback();
  if(!littleFsReady || slot>=CAROUSEL_SLOT_COUNT) return false;
  carouselActive=false;
  carouselActiveSlot=-1;
  CarouselSlotMeta &m=carouselSlots[slot];
  if(!m.configured || !m.mediaSize || !carouselDataTypeSupported(m.dataType)) return false;
  String path=carouselFileName(slot,m.dataType);
  if(!LittleFS.exists(path)) return false;
  if(m.dataType==1){
    if(!gifCacheMatches(path.c_str(),m.mediaSize,m.mediaCRC) && !fileMatchesMedia(path,m.mediaSize,m.mediaCRC)) return false;
  } else if(!fileMatchesMedia(path,m.mediaSize,m.mediaCRC)) return false;

  bool started=false;
  stopGIFPlayback();
  if(m.dataType==1){
    gifSize=m.mediaSize;
    started=startGIFFile(path.c_str(),false,true,m.mediaCRC);
    if(!started){ gifSize=0; gifStoredOnFS=false; }
  } else if(m.dataType==2){
#if IDOTMATRIX_MEMORY_TELEMETRY
    idotMemoryTelemetrySnapshot("image.carousel.before_load");
    const uint32_t imageCarouselStartedAtUs=micros();
#endif
    started=loadCarouselImageFrame(path.c_str(),m.mediaSize);
    if(started) displayMode=DISPLAY_RAW;
#if IDOTMATRIX_MEMORY_TELEMETRY
    if(started){
      idotMemoryTelemetryLatency("image.carousel.load_render_us",(uint32_t)(micros()-imageCarouselStartedAtUs));
      idotMemoryTelemetrySnapshot("image.carousel.after_render");
    }
#endif
  } else if(m.dataType==3){
    if(m.mediaSize<=MAX_TEXT_PAYLOAD){
      File f=LittleFS.open(path,"r");
      if(f && (uint32_t)f.size()==m.mediaSize){
        size_t got=f.read(textPayload,m.mediaSize);
        f.close();
        if(got==m.mediaSize) started=parseTextPayloadInternal(textPayload,got,true);
      } else if(f) f.close();
    }
  }
  if(!started) return false;

  carouselActive=true;
  carouselActiveSlot=(int8_t)slot;
  carouselSlotStartedAt=millis();
  scheduleCarouselGifPrefetch(slot);
#if IDOTMATRIX_MEMORY_TELEMETRY
  const char *carouselStartTag=m.dataType==3 ? "carousel.text.start_us" :
                               (m.dataType==2 ? "carousel.image.start_us" : "carousel.gif.start_us");
  const char *carouselMemTag=m.dataType==3 ? "carousel.text.started" :
                             (m.dataType==2 ? "carousel.image.started" : "carousel.gif.opened");
  idotMemoryTelemetryLatency(carouselStartTag,(uint32_t)(micros() - carouselTelemetryStartedAtUs));
  idotMemoryTelemetrySnapshot(carouselMemTag);
#endif
#if DEBUG_SERIAL && CAROUSEL_PROTOCOL_DEBUG
  Serial.print("CAROUSEL PLAY slot="); Serial.print(slot);
  Serial.print(" type="); Serial.print(carouselDataTypeLabel(m.dataType));
  Serial.print(" dwell="); Serial.print(m.dwellSeconds ? m.dwellSeconds : CAROUSEL_DEFAULT_DWELL_SEC);
  Serial.print("s bytes="); Serial.println(m.mediaSize);
#endif
  return true;
}

void updateCarousel(uint32_t now) {
  // Official-app workflows may establish the Assets view before a page push,
  // and a completed Device Assets replacement is expected to become visible
  // even when no second 0A/01 follows the Bulk transfers.  Therefore 02/01
  // establishes persistent Carousel view intent.  Because no explicit end-of-push frame
  // has been observed, use a conservative idle-settle boundary only
  // after at least one carousel asset has committed.  This is emulator policy,
  // not a claimed original-device protocol timing.
  if(carouselUploadOpen && carouselLastAssetCommitAt && !bulk.active &&
     (uint32_t)(now-carouselLastAssetCommitAt)>=CAROUSEL_UPLOAD_SETTLE_MS){
    if(carouselBankTxnActive && !finalizeCarouselBankTransaction()){
#if DEBUG_SERIAL
      Serial.println("[CARBANK] state=commit_retry reason=manifest_remove");
#endif
      carouselLastAssetCommitAt=now;
      return;
    }
    carouselUploadOpen=false;
    carouselUploadBlackout=false;
    endCarouselTransferIndicator();
    bool canStart=carouselEnterRequested && nextCarouselSlot(-1)>=0;
    if(canStart) carouselStartPending=true;
    else refreshMatrix(); // restore the preserved framebuffer if Assets view is not active
#if DEBUG_SERIAL && CAROUSEL_PROTOCOL_DEBUG
    Serial.print("CAROUSEL UPLOAD SETTLED after "); Serial.print(CAROUSEL_UPLOAD_SETTLE_MS);
    Serial.println(canStart ? "ms; blackout OFF; starting stored bank" : "ms; blackout OFF; view not active");
#endif
  }

  if(carouselStartPending){
    carouselStartPending=false;
    int8_t first=nextCarouselSlot(-1);
    if(first>=0) startCarouselSlot((uint8_t)first);
    return;
  }
  if(!carouselActive || carouselActiveSlot<0) return;
  uint16_t dwell=carouselSlots[(uint8_t)carouselActiveSlot].dwellSeconds;
  if(!dwell) dwell=CAROUSEL_DEFAULT_DWELL_SEC;
  if((uint32_t)(now-carouselSlotStartedAt) < (uint32_t)dwell*1000UL) return;
  int8_t next=nextCarouselSlot(carouselActiveSlot);
  if(next>=0) startCarouselSlot((uint8_t)next);
}

bool commitCarouselSlot(uint8_t slot, uint8_t dataType, uint16_t dwellSeconds, uint32_t size, uint32_t crc) {
  if(!littleFsReady || slot>=CAROUSEL_SLOT_COUNT || !size || !carouselDataTypeSupported(dataType)) return false;
  String tmp=carouselTempFileName(slot), dst=carouselFileName(slot,dataType), bak=carouselBackupFileName(slot);
  if(!fileMatchesMedia(tmp,size,crc)) return false;

  CarouselSlotMeta oldMeta=carouselSlots[slot];
  String oldDst=(oldMeta.configured && carouselDataTypeSupported(oldMeta.dataType)) ? carouselFileName(slot,oldMeta.dataType) : String();
  if(oldDst.length()) gifCacheInvalidatePath(oldDst.c_str());
  gifCacheInvalidatePath(dst.c_str());
  bool hadOld=oldDst.length() && LittleFS.exists(oldDst);
  LittleFS.remove(bak);
  if(hadOld && !LittleFS.rename(oldDst,bak)) return false;
  if(oldDst!=dst) LittleFS.remove(dst);
  if(!LittleFS.rename(tmp,dst)){
    if(hadOld) LittleFS.rename(bak,oldDst);
    return false;
  }

  CarouselSlotMeta newMeta;
  newMeta.configured=1; newMeta.dataType=dataType; newMeta.dwellSeconds=dwellSeconds; newMeta.mediaSize=size; newMeta.mediaCRC=crc;
  carouselSlots[slot]=newMeta;
  if(!saveCarouselSlotMeta(slot)){
    LittleFS.remove(dst);
    if(hadOld) LittleFS.rename(bak,oldDst);
    carouselSlots[slot]=oldMeta;
    if(oldMeta.configured) saveCarouselSlotMeta(slot); else removeCarouselSlotMeta(slot);
    return false;
  }
  LittleFS.remove(bak);
  removeCarouselMediaFilesExcept(slot,dataType);
  carouselLastAssetCommitAt=millis();
#if DEBUG_SERIAL && CAROUSEL_PROTOCOL_DEBUG
  Serial.print("CAROUSEL STORE slot="); Serial.print(slot);
  Serial.print(" type="); Serial.print(carouselDataTypeLabel(dataType));
  Serial.print(" dwell="); Serial.print(dwellSeconds);
  Serial.print("s bytes="); Serial.println(size);
#endif
  return true;
}

bool queueDeferredCarouselBankSetup(const uint8_t *slots, uint8_t count, bool viewIntent) {
  if(!slots || count>CAROUSEL_SLOT_COUNT || deferredCarouselBankSetup.active) return false;
  deferredCarouselBankSetup=DeferredCarouselBankSetupState();
  deferredCarouselBankSetup.active=true;
  deferredCarouselBankSetup.viewIntent=viewIntent;
  deferredCarouselBankSetup.count=count;
  memcpy(deferredCarouselBankSetup.order,slots,count);
  return true;
}

void processDeferredCarouselBankSetup() {
  if(!deferredCarouselBankSetup.active) return;
  const uint8_t count=deferredCarouselBankSetup.count;
  const bool queuedViewIntent=deferredCarouselBankSetup.viewIntent;
  uint8_t order[CAROUSEL_SLOT_COUNT]={0};
  memcpy(order,deferredCarouselBankSetup.order,CAROUSEL_SLOT_COUNT);

  // Playback/temporary live-GIF cleanup is loop-side for the same reason as
  // final asset publication: avoid deep decoder/LittleFS activity on
  // nimble_host.
  if(carouselActive || gifCarouselPlaybackFileActive || displayMode==DISPLAY_TEXT) stopCarouselPlayback();
  if(pendingGifStart){
    if(littleFsReady && pendingGifSlot>=0 && pendingGifSlot<=1) LittleFS.remove(GIF_RX_FILES[(uint8_t)pendingGifSlot]);
    pendingGifStart=false; pendingGifSlot=-1; pendingGifSize=0; pendingGifCRC=0; pendingGifStartAt=0;
  }
  // Preserve any 0A/01 that may have arrived after 02/01 was queued, and
  // honor the bank-setup request itself as persistent Carousel view intent.
  carouselEnterRequested=queuedViewIntent || carouselEnterRequested;
  carouselStartPending=false;

  uint16_t touchedMask=0;
  for(uint8_t i=0;i<count;i++) touchedMask|=(uint16_t)(1U<<order[i]);
  const bool bankProtected=beginCarouselBankTransaction(touchedMask);
#if DEBUG_SERIAL
  if(!bankProtected) Serial.println("[CARBANK] state=fallback reason=journal_unavailable");
#endif

  carouselUploadOpen=true;
  carouselUploadBlackout=true;
  carouselUploadCompletedMask=0;
  endCarouselTransferIndicator();
  carouselLastAssetCommitAt=0;
  carouselOrderCount=count;
  memcpy(carouselOrder,order,CAROUSEL_SLOT_COUNT);

  for(uint8_t i=0;i<count;i++) clearCarouselSlot(order[i]);
  saveCarouselOrder();
  refreshMatrix();
#if DEBUG_SERIAL && CAROUSEL_PROTOCOL_DEBUG
  Serial.print("CAROUSEL SLOT SETUP count="); Serial.print(count); Serial.print(" slots=");
  for(uint8_t i=0;i<count;i++){ if(i) Serial.print(','); Serial.print(order[i]); }
  Serial.print(" playback=SUSPENDED matrix=BLACK viewIntent=");
  Serial.println(carouselEnterRequested ? "YES" : "NO");
#endif
  deferredCarouselBankSetup=DeferredCarouselBankSetupState();
  sendCommandAck(0x02,0x01);
}

bool handleCarouselCommand(const uint8_t *data, size_t len) {
  if(!data || len<4) return false;

  // Slot setup / material wipe. Public hardware-validated RE shows byte 4 as
  // a count followed by that many device-slot IDs. The current official app
  // sends all twelve IDs (0..11) even when only a subset receives media.
  if(data[2]==0x02 && data[3]==0x01){
    if(len<5 || ((uint16_t)data[0] | ((uint16_t)data[1]<<8))!=len) return false;
    uint8_t count=data[4];
    if(count>CAROUSEL_SLOT_COUNT || len!=(size_t)(5U+count)) return false;
    bool seen[CAROUSEL_SLOT_COUNT]={false};
    for(uint8_t i=0;i<count;i++){
      uint8_t slot=data[5+i];
      if(slot>=CAROUSEL_SLOT_COUNT || seen[slot]) return false;
      seen[slot]=true;
    }

    // Capture the command only. Filesystem journal/backup/wipe work is
    // deferred to loopTask; its ACK is emitted only after the bank is ready for
    // the first Bulk object, preserving app pacing without blocking nimble_host.
    // Starting a Device Assets bank replacement is itself a request to enter
    // the Carousel view once the new bank is safely committed.  This matters
    // when the app starts the upload while Preset/Default playback owns the
    // display and does not send a second 0A/01 after the transfer.
    const bool viewIntent=true;
    if(!queueDeferredCarouselBankSetup(data+5,count,viewIntent)){
#if DEBUG_SERIAL
      Serial.println("[CARBANK] state=setup_queue_busy");
#endif
      return true;
    }
    return true;
  }

  // Enter/start Device Assets view. Captures show this may arrive before a
  // subsequent page push, not necessarily after it. Therefore it establishes
  // persistent view intent; an open replacement upload is allowed to finish
  // before playback starts. Repeated 0A/01 while already playing is idempotent.
  if(len==4 && data[2]==0x0A && data[3]==0x01){
    carouselEnterRequested=true;
    if(!carouselUploadOpen && !carouselActive && nextCarouselSlot(-1)>=0) carouselStartPending=true;
#if DEBUG_SERIAL && CAROUSEL_PROTOCOL_DEBUG
    Serial.print("CAROUSEL ASSET VIEW (0A/01) uploadOpen=");
    Serial.print(carouselUploadOpen ? "YES" : "NO");
    Serial.print(" active="); Serial.println(carouselActive ? "YES" : "NO");
#endif
    sendCommandAck(0x0A,0x01);
    return true;
  }
  return false;
}

// ======================================================
// PRESET / DEFAULT volatile bank
// ======================================================
String presetFileName(uint8_t localSlot, uint8_t dataType) {
  return String("/pre") + localSlot + (dataType==3 ? ".txt" : ".gif");
}
String presetTempFileName(uint8_t localSlot) { return String("/pre") + localSlot + ".tmp"; }
String presetStagedFileName(uint8_t localSlot, uint8_t dataType) {
  return String("/pre") + localSlot + (dataType==3 ? ".new.txt" : ".new.gif");
}
String presetBackupFileName(uint8_t localSlot, uint8_t dataType) {
  return String("/pre") + localSlot + (dataType==3 ? ".old.txt" : ".old.gif");
}

const char *presetBankCleanupReasonText(uint8_t reason) {
  switch(reason){
    case 1: return "disconnect";
    case 2: return "bulk_abort";
    case 3: return "activation_reject";
    case 4: return "device_reset";
    default: return "cleanup";
  }
}

void resetPresetBankTxnState() {
  presetStagedMask=0;
  presetBankTxnActive=false;
  presetBankCleanupPending=false;
  presetBankCleanupReason=0;
  presetBankPreviousActive=false;
  presetBankPreviousActiveSlot=-1;
  for(uint8_t i=0;i<PRESET_SLOT_COUNT;i++) presetStaging[i]=PresetSlotMeta();
}

void cleanupPresetCandidateFiles() {
  if(!littleFsReady) return;
  for(uint8_t i=0;i<PRESET_SLOT_COUNT;i++){
    LittleFS.remove(presetTempFileName(i));
    LittleFS.remove(presetStagedFileName(i,1));
    LittleFS.remove(presetStagedFileName(i,3));
    LittleFS.remove(presetBackupFileName(i,1));
    LittleFS.remove(presetBackupFileName(i,3));
  }
}

void beginPresetBankTransaction(uint8_t previousActive, int8_t previousSlot) {
  // Starting a fresh invocation only changes RAM here. Any stale candidate
  // files from an interrupted older invocation are overwritten slot-by-slot
  // on loopTask and are never authoritative without presetStaging metadata.
  presetStagedMask=0;
  presetBankTxnActive=true;
  presetBankCleanupPending=false;
  presetBankCleanupReason=0;
  presetBankPreviousActive=previousActive!=0;
  presetBankPreviousActiveSlot=previousSlot;
  deferredPresetActivation=DeferredPresetActivationState();
  for(uint8_t i=0;i<PRESET_SLOT_COUNT;i++) presetStaging[i]=PresetSlotMeta();
#if DEBUG_SERIAL
  Serial.print("[PREBANK] state=begin previousActive="); Serial.println(presetBankPreviousActive ? 1 : 0);
#endif
}

void requestPresetBankCleanup(uint8_t reason) {
  if(!presetBankTxnActive && !presetStagedMask) return;
  presetBankCleanupPending=true;
  presetBankCleanupReason=reason;
}

void abortPresetBankTransaction(uint8_t reason) {
  const bool resumePrevious=presetBankPreviousActive;
  const int8_t previousSlot=presetBankPreviousActiveSlot;
  cleanupPresetCandidateFiles();
  resetPresetBankTxnState();
  bool resumed=false;
  if(resumePrevious){
    if(previousSlot>=0 && previousSlot<PRESET_SLOT_COUNT) resumed=startPresetSlot((uint8_t)previousSlot);
    if(!resumed){ int8_t first=nextPresetSlot(-1); if(first>=0) resumed=startPresetSlot((uint8_t)first); }
  }
#if DEBUG_SERIAL
  Serial.print("[PREBANK] state=abort reason="); Serial.print(presetBankCleanupReasonText(reason));
  Serial.print(" resumed="); Serial.println(resumed ? 1 : 0);
#endif
}

void clearPresetSlot(uint8_t localSlot) {
  if(localSlot>=PRESET_SLOT_COUNT) return;
  String gifPath=presetFileName(localSlot,1);
  gifCacheInvalidatePath(gifPath.c_str());
  if(littleFsReady){
    LittleFS.remove(presetTempFileName(localSlot));
    LittleFS.remove(presetFileName(localSlot,1));
    LittleFS.remove(presetFileName(localSlot,3));
    LittleFS.remove(presetStagedFileName(localSlot,1));
    LittleFS.remove(presetStagedFileName(localSlot,3));
    LittleFS.remove(presetBackupFileName(localSlot,1));
    LittleFS.remove(presetBackupFileName(localSlot,3));
  }
  presetSlots[localSlot]=PresetSlotMeta();
  presetStaging[localSlot]=PresetSlotMeta();
  presetStagedMask&=(uint16_t)~(1U<<localSlot);
}

void clearPresetBank(bool removeFiles) {
  presetActive=false;
  presetActiveSlot=-1;
  presetOrderCount=0;
  presetSlotHoldMs=PRESET_DWELL_MS;
  gifPresetPlaybackFileActive=false;
  deferredPresetActivation=DeferredPresetActivationState();
  for(uint8_t i=0;i<PRESET_SLOT_COUNT;i++){
    presetOrder[i]=0;
    if(removeFiles) clearPresetSlot(i);
    else { presetSlots[i]=PresetSlotMeta(); presetStaging[i]=PresetSlotMeta(); }
  }
  resetPresetBankTxnState();
}

void stopPresetPlayback() {
  const bool hadGif=gifPresetPlaybackFileActive;
  presetActive=false;
  presetActiveSlot=-1;
  presetSlotHoldMs=PRESET_DWELL_MS;
  gifPresetPlaybackFileActive=false;
  if(hadGif) stopGIFPlayback();
  if(displayMode==DISPLAY_GIF || displayMode==DISPLAY_TEXT) displayMode=DISPLAY_RAW;
}

bool commitPresetSlot(uint8_t localSlot, uint8_t dataType, uint16_t timeSign, uint32_t size, uint32_t crc) {
  if(!littleFsReady || localSlot>=PRESET_SLOT_COUNT || !size || (dataType!=1 && dataType!=3)) return false;
  String tmp=presetTempFileName(localSlot);
  if(!fileMatchesMedia(tmp,size,crc)) return false;

  // While an invocation transaction is open, a completed object becomes
  // a candidate only. The currently authoritative /preN.* file remains intact
  // until the later 06/02 command commits the complete selected bank.
  if(presetBankTxnActive){
    String staged=presetStagedFileName(localSlot,dataType);
    LittleFS.remove(presetStagedFileName(localSlot,1));
    LittleFS.remove(presetStagedFileName(localSlot,3));
    if(!LittleFS.rename(tmp,staged)) return false;
    PresetSlotMeta m;
    m.configured=1;
    m.dataType=dataType;
    m.timeSign=timeSign;
    m.mediaSize=size;
    m.mediaCRC=crc;
    presetStaging[localSlot]=m;
    presetStagedMask|=(uint16_t)(1U<<localSlot);
#if DEBUG_SERIAL
    Serial.print("[PREBANK] state=stage slot="); Serial.print((unsigned)(PRESET_SLOT_BASE+localSlot));
    Serial.print(" type="); Serial.print(dataType==3 ? "TEXT" : "GIF");
    Serial.print(" bytes="); Serial.println(size);
#endif
    return true;
  }

  // Compatibility fallback for a caller that reaches this helper without a
  // bank transaction. This is the legacy per-slot publication behavior.
  if(presetActive && presetActiveSlot==(int8_t)localSlot) stopPresetPlayback();
  String dst=presetFileName(localSlot,dataType);
  gifCacheInvalidatePath(dst.c_str());
  if(dataType!=1){ String oldGif=presetFileName(localSlot,1); gifCacheInvalidatePath(oldGif.c_str()); }
  LittleFS.remove(dst);
  if(!LittleFS.rename(tmp,dst)) return false;
  LittleFS.remove(presetFileName(localSlot,dataType==1 ? 3 : 1));

  PresetSlotMeta &m=presetSlots[localSlot];
  m.configured=1;
  m.dataType=dataType;
  m.timeSign=timeSign;
  m.mediaSize=size;
  m.mediaCRC=crc;
  return true;
}

bool applyPresetOrderAndStart(const uint8_t *order, uint8_t count) {
  if((count && !order) || count>PRESET_SLOT_COUNT) return false;
  stopPresetPlayback();
  presetOrderCount=count;
  for(uint8_t i=0;i<count;i++) presetOrder[i]=order[i];
  for(uint8_t i=count;i<PRESET_SLOT_COUNT;i++) presetOrder[i]=0;
  if(count){
    int8_t first=nextPresetSlot(-1);
    if(first>=0) startPresetSlot((uint8_t)first);
#if DEBUG_SERIAL && PRESET_PROTOCOL_DEBUG
    else Serial.println("PRESET ACT: no valid uploaded slots");
#endif
  }
  return true;
}

bool rollbackPresetBankPublication(const uint8_t *order, uint8_t count,
                                   uint16_t backedMask, uint16_t hadGifMask, uint16_t hadTxtMask) {
  if(!littleFsReady) return false;
  bool ok=true;
  for(uint8_t i=0;i<count;i++){
    const uint8_t slot=order[i];
    const uint16_t bit=(uint16_t)(1U<<slot);
    if(!(backedMask&bit)) continue;
    String gif=presetFileName(slot,1), txt=presetFileName(slot,3);
    String gifBak=presetBackupFileName(slot,1), txtBak=presetBackupFileName(slot,3);
    gifCacheInvalidatePath(gif.c_str());
    LittleFS.remove(gif);
    LittleFS.remove(txt);
    if(hadGifMask&bit){ if(!LittleFS.exists(gifBak) || !LittleFS.rename(gifBak,gif)) ok=false; }
    else LittleFS.remove(gifBak);
    if(hadTxtMask&bit){ if(!LittleFS.exists(txtBak) || !LittleFS.rename(txtBak,txt)) ok=false; }
    else LittleFS.remove(txtBak);
  }
  return ok;
}

bool commitPresetBankActivation(const uint8_t *order, uint8_t count) {
  if((count && !order) || count>PRESET_SLOT_COUNT || !littleFsReady) return false;
  if(!presetBankTxnActive) return applyPresetOrderAndStart(order,count);

  uint16_t declaredMask=0;
  for(uint8_t i=0;i<count;i++){
    const uint8_t slot=order[i];
    if(slot>=PRESET_SLOT_COUNT) return false;
    const uint16_t bit=(uint16_t)(1U<<slot);
    declaredMask|=bit;
    const PresetSlotMeta &m=presetStaging[slot];
    if(!(presetStagedMask&bit) || !m.configured || (m.dataType!=1 && m.dataType!=3) ||
       !fileMatchesMedia(presetStagedFileName(slot,m.dataType),m.mediaSize,m.mediaCRC)){
#if DEBUG_SERIAL
      Serial.print("[PREBANK] state=reject reason=incomplete slot="); Serial.println((unsigned)(PRESET_SLOT_BASE+slot));
#endif
      return false;
    }
  }

  // count=0 is a valid empty activation. No staged media becomes authoritative.
  if(!count){
    cleanupPresetCandidateFiles();
    resetPresetBankTxnState();
    applyPresetOrderAndStart(order,count);
#if DEBUG_SERIAL
    Serial.println("[PREBANK] state=commit count=0");
#endif
    return true;
  }

  uint16_t backedMask=0, hadGifMask=0, hadTxtMask=0;
  // Phase 1: move the currently authoritative selected files aside. Rename-only
  // backups keep the old bank recoverable without duplicating flash payloads.
  for(uint8_t i=0;i<count;i++){
    const uint8_t slot=order[i];
    const uint16_t bit=(uint16_t)(1U<<slot);
    String gif=presetFileName(slot,1), txt=presetFileName(slot,3);
    String gifBak=presetBackupFileName(slot,1), txtBak=presetBackupFileName(slot,3);
    LittleFS.remove(gifBak); LittleFS.remove(txtBak);
    const bool hadGif=LittleFS.exists(gif), hadTxt=LittleFS.exists(txt);
    bool gifMoved=true, txtMoved=true;
    if(hadGif) gifMoved=LittleFS.rename(gif,gifBak);
    if(hadTxt) txtMoved=LittleFS.rename(txt,txtBak);
    if(!gifMoved || !txtMoved){
      if(gifMoved && hadGif && LittleFS.exists(gifBak)) LittleFS.rename(gifBak,gif);
      if(txtMoved && hadTxt && LittleFS.exists(txtBak)) LittleFS.rename(txtBak,txt);
      rollbackPresetBankPublication(order,count,backedMask,hadGifMask,hadTxtMask);
      return false;
    }
    backedMask|=bit;
    if(hadGif) hadGifMask|=bit;
    if(hadTxt) hadTxtMask|=bit;
  }

  // Phase 2: publish every declared candidate. Metadata/order are still old at
  // this point, so a failure can restore the old files without mixed runtime state.
  uint16_t publishedMask=0;
  for(uint8_t i=0;i<count;i++){
    const uint8_t slot=order[i];
    const uint16_t bit=(uint16_t)(1U<<slot);
    const PresetSlotMeta &m=presetStaging[slot];
    String staged=presetStagedFileName(slot,m.dataType);
    String dst=presetFileName(slot,m.dataType);
    gifCacheInvalidatePath(presetFileName(slot,1).c_str());
    LittleFS.remove(dst);
    LittleFS.remove(presetFileName(slot,m.dataType==1 ? 3 : 1));
    if(!LittleFS.rename(staged,dst)){
      for(uint8_t j=0;j<count;j++){
        const uint8_t publishedSlot=order[j];
        const uint16_t publishedBit=(uint16_t)(1U<<publishedSlot);
        if(!(publishedMask&publishedBit)) continue;
        LittleFS.remove(presetFileName(publishedSlot,1));
        LittleFS.remove(presetFileName(publishedSlot,3));
      }
      rollbackPresetBankPublication(order,count,backedMask,hadGifMask,hadTxtMask);
      return false;
    }
    publishedMask|=bit;
  }

  // File publication succeeded for the full declared bank. Only now switch RAM
  // metadata/order, then remove backups/candidate leftovers. Presets remain
  // volatile and will still be cleared on reboot.
  for(uint8_t i=0;i<count;i++) presetSlots[order[i]]=presetStaging[order[i]];
  for(uint8_t i=0;i<count;i++){
    const uint8_t slot=order[i];
    LittleFS.remove(presetBackupFileName(slot,1));
    LittleFS.remove(presetBackupFileName(slot,3));
  }
  for(uint8_t slot=0;slot<PRESET_SLOT_COUNT;slot++){
    if(!(declaredMask&(1U<<slot))){
      LittleFS.remove(presetStagedFileName(slot,1));
      LittleFS.remove(presetStagedFileName(slot,3));
    }
  }
  resetPresetBankTxnState();
  applyPresetOrderAndStart(order,count);
#if DEBUG_SERIAL
  Serial.print("[PREBANK] state=commit count="); Serial.println(count);
#endif
  return true;
}

bool queueDeferredPresetActivation(const uint8_t *order, uint8_t count) {
  if((count && !order) || count>PRESET_SLOT_COUNT || deferredPresetActivation.active) return false;
  deferredPresetActivation=DeferredPresetActivationState();
  deferredPresetActivation.active=true;
  deferredPresetActivation.count=count;
  if(count) memcpy(deferredPresetActivation.order,order,count);
  return true;
}

void processDeferredPresetActivation() {
  if(!deferredPresetActivation.active) return;
  const uint8_t count=deferredPresetActivation.count;
  uint8_t order[PRESET_SLOT_COUNT]={0};
  memcpy(order,deferredPresetActivation.order,PRESET_SLOT_COUNT);

  if(presetTransferIndicatorActive){
    presetTransferIndicatorActive=false;
    endCarouselTransferIndicator();
  }
  const bool committed=commitPresetBankActivation(order,count);
  if(!committed && presetBankTxnActive){
    abortPresetBankTransaction(3);
  }
  deferredPresetActivation=DeferredPresetActivationState();
  sendCommandAck(0x06,0x02);
}

void processPresetBankCleanup() {
  if(!presetBankCleanupPending) return;
  // Let an already-complete media object and/or a received 06/02 command reach
  // their loopTask commit boundary first. A disconnect after the final object
  // therefore cannot strand an unflushed temp file in the cleanup path.
  if((deferredAssetCommit.active && deferredAssetCommit.kind==DEFERRED_ASSET_PRESET) ||
     deferredPresetActivation.active || bulk.presetToFS) return;
  const uint8_t reason=presetBankCleanupReason;
  abortPresetBankTransaction(reason ? reason : 1);
}

int8_t nextPresetSlot(int8_t currentLocal) {
  if(!littleFsReady || !presetOrderCount) return -1;
  int start=-1;
  for(uint8_t i=0;i<presetOrderCount;i++) if((int8_t)presetOrder[i]==currentLocal){ start=i; break; }
  for(uint8_t step=1;step<=presetOrderCount;step++){
    uint8_t pos=(uint8_t)((start<0 ? step-1 : start+step)%presetOrderCount);
    uint8_t local=presetOrder[pos];
    if(local>=PRESET_SLOT_COUNT) continue;
    const PresetSlotMeta &m=presetSlots[local];
    if(!m.configured || !m.mediaSize || (m.dataType!=1 && m.dataType!=3)) continue;
    String path=presetFileName(local,m.dataType);
    if(m.dataType==1){
      if(LittleFS.exists(path) && (gifCacheMatches(path.c_str(),m.mediaSize,m.mediaCRC) || fileMatchesMedia(path,m.mediaSize,m.mediaCRC))) return (int8_t)local;
    } else if(fileMatchesMedia(path,m.mediaSize,m.mediaCRC)) return (int8_t)local;
  }
  return -1;
}

uint32_t presetTextHoldDurationMs() {
  if(!textState.valid || !textState.glyphCount) return PRESET_DWELL_MS;

  const uint8_t motion=textState.motionEffect;
  const uint8_t capacity=textVisibleGlyphCapacity();
  const uint16_t pages=(capacity>0)
      ? (uint16_t)((textState.glyphCount + capacity - 1U) / capacity)
      : 1U;

  // LEFT/RIGHT are true continuous scroll modes. The original display advances
  // the Preset as soon as the final glyph has completely left the matrix.
  if(motion==1 || motion==2){
    const uint32_t textWidth=(uint32_t)textState.glyphCount * textState.glyphAdvance;
    const uint32_t travel=(uint32_t)MATRIX_WIDTH + textWidth + 1U;
    return max((uint32_t)textFrameInterval(), travel * (uint32_t)textFrameInterval());
  }

  // PIN and the page/viewport based modes must show every page once, then keep
  // the final page visible for the same ~3 s tail observed on original hardware.
  if(motion==3 || motion==4){
    const uint32_t pageTravel=(uint32_t)textState.glyphHeight + 1U;
    const uint32_t perPage=pageTravel * (uint32_t)textFrameInterval();
    return (pages>1U ? (uint32_t)(pages-1U)*perPage : 0U) + PRESET_DWELL_MS;
  }

  return (pages>1U ? (uint32_t)(pages-1U)*(uint32_t)textPageInterval() : 0U)
       + PRESET_DWELL_MS;
}

bool startPresetSlot(uint8_t localSlot) {
  if(!littleFsReady || localSlot>=PRESET_SLOT_COUNT) return false;
  PresetSlotMeta &m=presetSlots[localSlot];
  if(!m.configured || !m.mediaSize || (m.dataType!=1 && m.dataType!=3)) return false;
  String path=presetFileName(localSlot,m.dataType);
  if(m.dataType==1){
    if(!LittleFS.exists(path) || (!gifCacheMatches(path.c_str(),m.mediaSize,m.mediaCRC) && !fileMatchesMedia(path,m.mediaSize,m.mediaCRC))) return false;
  } else if(!fileMatchesMedia(path,m.mediaSize,m.mediaCRC)) return false;

  if(carouselActive || gifCarouselPlaybackFileActive || carouselStartPending) stopCarouselPlayback();
  stopGIFPlayback();
  gifPresetPlaybackFileActive=false;

  bool started=false;
  if(m.dataType==1){
    gifSize=m.mediaSize;
    started=startGIFFile(path.c_str(),false,false,m.mediaCRC);
    if(started) gifPresetPlaybackFileActive=true;
    else { gifSize=0; gifStoredOnFS=false; }
  } else {
    if(m.mediaSize<=MAX_TEXT_PAYLOAD){
      File f=LittleFS.open(path,"r");
      if(f && (uint32_t)f.size()==m.mediaSize){
        size_t got=f.read(textPayload,m.mediaSize);
        f.close();
        if(got==m.mediaSize) started=parseTextPayloadInternal(textPayload,got,true);
      } else if(f) f.close();
    }
  }
  if(!started) return false;

  presetActive=true;
  presetActiveSlot=(int8_t)localSlot;
  presetSlotStartedAt=millis();
  presetSlotHoldMs=(m.dataType==3) ? presetTextHoldDurationMs() : PRESET_DWELL_MS;
#if DEBUG_SERIAL && PRESET_PROTOCOL_DEBUG
  Serial.print("PRESET PLAY slot="); Serial.print((unsigned)(PRESET_SLOT_BASE+localSlot));
  Serial.print(" type="); Serial.print(m.dataType==3 ? "TEXT" : "GIF");
  Serial.print(" holdMs="); Serial.print(presetSlotHoldMs);
  if(m.dataType==3){
    Serial.print(" motion="); Serial.print(textState.motionEffect);
    Serial.print(" pages=");
    const uint8_t cap=textVisibleGlyphCapacity();
    Serial.print(cap ? (unsigned)((textState.glyphCount+cap-1U)/cap) : 1U);
  }
  Serial.println();
#endif
  return true;
}

void updatePreset(uint32_t now) {
  if(!presetActive || presetActiveSlot<0 || presetOrderCount<=1) return;
  if((uint32_t)(now-presetSlotStartedAt)<presetSlotHoldMs) return;
  int8_t next=nextPresetSlot(presetActiveSlot);
  if(next>=0 && next!=presetActiveSlot) startPresetSlot((uint8_t)next);
  else presetSlotStartedAt=now;
}

bool handlePresetCommand(const uint8_t *data, size_t len) {
  if(!data || len<5 || data[2]!=0x06 || data[3]!=0x02) return false;
  uint16_t declared=(uint16_t)data[0] | ((uint16_t)data[1]<<8);
  if(declared!=len) return false;
  uint8_t count=data[4];
  if(count>PRESET_SLOT_COUNT || len!=(size_t)(5U+count)) return false;

  bool seen[PRESET_SLOT_COUNT]={false};
  uint8_t localOrder[PRESET_SLOT_COUNT]={0};
  for(uint8_t i=0;i<count;i++){
    uint8_t deviceSlot=data[5+i];
    if(deviceSlot<PRESET_SLOT_BASE || deviceSlot>=PRESET_SLOT_BASE+PRESET_SLOT_COUNT) return false;
    uint8_t local=(uint8_t)(deviceSlot-PRESET_SLOT_BASE);
    if(seen[local]) return false;
    seen[local]=true;
    localOrder[i]=local;
  }

#if DEBUG_SERIAL && PRESET_PROTOCOL_DEBUG
  Serial.print("PRESET ACT count="); Serial.print(count); Serial.print(" slots=");
  for(uint8_t i=0;i<count;i++){ if(i) Serial.print(','); Serial.print(data[5+i]); }
  Serial.println();
#endif

  // Activation may publish multiple staged files and start a decoder.
  // Capture only the tiny order descriptor on nimble_host; loopTask performs
  // validation/publication/start and emits the historical ACK afterwards.
  if(!queueDeferredPresetActivation(localOrder,count)){
#if DEBUG_SERIAL
    Serial.println("[PREBANK] state=activation_queue_busy");
#endif
    return true;
  }
  return true;
}

// ======================================================
// BULK
// ======================================================
bool startsWithGIF(const uint8_t *data,size_t len){ return len>=6 && (!memcmp(data,"GIF87a",6)||!memcmp(data,"GIF89a",6)); }
bool startsWithPNG(const uint8_t *data,size_t len){
  static const uint8_t sig[8]={0x89,'P','N','G',0x0D,0x0A,0x1A,0x0A};
  return len>=sizeof(sig) && !memcmp(data,sig,sizeof(sig));
}
void resetBulkTransfer(bool removePartialGif=false){
  int8_t rxSlot=bulk.gifRxSlot;
  const bool abortedCarouselTransfer=removePartialGif && (bulk.carouselToFS || bulk.carouselBuffered);
  const bool abortedPresetTransfer=removePartialGif && bulk.presetToFS;
  if(gifBulkFile) gifBulkFile.close();
  if(littleFsReady && removePartialGif){
    if(bulk.carouselToFS && bulk.carouselLocalSlot>=0 && bulk.carouselLocalSlot<CAROUSEL_SLOT_COUNT){
      LittleFS.remove(carouselTempFileName((uint8_t)bulk.carouselLocalSlot));
    } else if(bulk.presetToFS && bulk.presetLocalSlot>=0 && bulk.presetLocalSlot<PRESET_SLOT_COUNT){
      LittleFS.remove(presetTempFileName((uint8_t)bulk.presetLocalSlot));
    } else if(rxSlot>=0 && rxSlot<=1){
      // Never remove a completed file already queued for deferred playback.
      if(!(pendingGifStart && pendingGifSlot==rxSlot)) LittleFS.remove(GIF_RX_FILES[(uint8_t)rxSlot]);
    }
  }
  bulk=BulkTransferState();
  bulkLastRxMs=0;
  textPayloadReceived=0;
  if(rawRgbData){ free(rawRgbData); rawRgbData=nullptr; }
  rawRgbWriteOffset=0;
  if(carouselImageRxData){ free(carouselImageRxData); carouselImageRxData=nullptr; }
  carouselImageRxWriteOffset=0;
  if(abortedCarouselTransfer) endCarouselTransferIndicator();
  if(abortedPresetTransfer) requestPresetBankCleanup(2);
}

bool queueDeferredAssetCommit(uint8_t kindValue, uint8_t dataType, uint8_t localSlot,
                              uint16_t timeSign, uint32_t size, uint32_t crc, bool integrityOk) {
  // Keep the helper signature Arduino-preprocessor-safe: .ino prototype generation
  // can place function prototypes before the custom enum declaration. Accept a
  // primitive here and validate/cast only inside the implementation.
  if(kindValue != DEFERRED_ASSET_CAROUSEL && kindValue != DEFERRED_ASSET_PRESET) return false;
  if(deferredAssetCommit.active) return false;
  const DeferredAssetCommitKind kind = static_cast<DeferredAssetCommitKind>(kindValue);
  deferredAssetCommit.active=true;
  deferredAssetCommit.kind=kind;
  deferredAssetCommit.dataType=dataType;
  deferredAssetCommit.localSlot=localSlot;
  deferredAssetCommit.timeSign=timeSign;
  deferredAssetCommit.size=size;
  deferredAssetCommit.crc=crc;
  deferredAssetCommit.integrityOk=integrityOk;
  deferredAssetCommit.queuedAt=millis();
#if DEBUG_SERIAL && BULK_PROTOCOL_DEBUG
  Serial.print("BULK COMMIT queued kind=");
  Serial.print(kind==DEFERRED_ASSET_CAROUSEL ? "CAROUSEL" : "PRESET");
  Serial.print(" slot="); Serial.print(localSlot);
  Serial.print(" type="); Serial.print(dataType);
  Serial.print(" integrity="); Serial.println(integrityOk ? "OK" : "BAD");
#endif
  return true;
}

void processDeferredAssetCommit(){
  if(!deferredAssetCommit.active) return;
  const DeferredAssetCommitState job=deferredAssetCommit;
#if IDOTMATRIX_MEMORY_TELEMETRY
  idotMemoryTelemetrySnapshot(job.kind==DEFERRED_ASSET_CAROUSEL ? "fs.commit.carousel.before" : "fs.commit.preset.before");
#endif

  // File finalization and every operation below intentionally run on loopTask,
  // never on nimble_host. This is the stack-safety boundary.
  if(gifBulkFile){
    gifBulkFile.flush();
    gifBulkFile.close();
  }

  bool stored=false;
  bool stagedForCommit=job.integrityOk;
  if(stagedForCommit && job.kind==DEFERRED_ASSET_CAROUSEL && carouselImageRxData){
    // Waveshare stack-safety boundary: persistent Carousel bytes are accumulated
    // in memory by nimble_host. Perform every LittleFS open/write here on loopTask.
    stagedForCommit=false;
    if(littleFsReady && carouselImageRxData && carouselImageRxWriteOffset==job.size){
      const String tmp=carouselTempFileName(job.localSlot);
      LittleFS.remove(tmp);
      File out=LittleFS.open(tmp,"w");
      if(out){
        const size_t written=out.write(carouselImageRxData,job.size);
        out.flush();
        out.close();
        stagedForCommit=(written==job.size);
      }
    }
  }
  if(stagedForCommit){
    if(job.kind==DEFERRED_ASSET_CAROUSEL && job.localSlot<CAROUSEL_SLOT_COUNT){
      stored=commitCarouselSlot(job.localSlot,job.dataType,job.timeSign,job.size,job.crc);
      if(stored) carouselUploadCompletedMask |= (uint16_t)(1U<<job.localSlot);
    } else if(job.kind==DEFERRED_ASSET_PRESET && job.localSlot<PRESET_SLOT_COUNT){
      stored=commitPresetSlot(job.localSlot,job.dataType,job.timeSign,job.size,job.crc);
    }
  }

  if(!stored && littleFsReady){
    if(job.kind==DEFERRED_ASSET_CAROUSEL && job.localSlot<CAROUSEL_SLOT_COUNT)
      LittleFS.remove(carouselTempFileName(job.localSlot));
    else if(job.kind==DEFERRED_ASSET_PRESET && job.localSlot<PRESET_SLOT_COUNT)
      LittleFS.remove(presetTempFileName(job.localSlot));
  }

#if DEBUG_SERIAL
  if(!stored){
    Serial.print("BULK DEFERRED COMMIT FAILED kind=");
    Serial.print(job.kind==DEFERRED_ASSET_CAROUSEL ? "CAROUSEL" : "PRESET");
    Serial.print(" slot="); Serial.print(job.localSlot);
    Serial.print(" integrity="); Serial.println(job.integrityOk ? "OK" : "BAD");
  }
#endif
#if DEBUG_SERIAL && BULK_PROTOCOL_DEBUG
  Serial.print("BULK COMMIT done kind=");
  Serial.print(job.kind==DEFERRED_ASSET_CAROUSEL ? "CAROUSEL" : "PRESET");
  Serial.print(" slot="); Serial.print(job.localSlot);
  Serial.print(" stored="); Serial.print(stored ? "YES" : "NO");
  Serial.print(" latencyMs="); Serial.println((uint32_t)(millis()-job.queuedAt));
#endif

  // Clear protocol state before the completion notification. The runtime mutex
  // remains held by loop(), so a new FA02 write cannot observe a half-reset
  // transfer even if the peer reacts immediately to the ACK.
  deferredAssetCommit=DeferredAssetCommitState();
  resetBulkTransfer(false);
#if IDOTMATRIX_MEMORY_TELEMETRY
  idotMemoryTelemetrySnapshot(job.kind==DEFERRED_ASSET_CAROUSEL ? "fs.commit.carousel.after" : "fs.commit.preset.after");
#endif
  sendTransferAck(job.dataType,0x03);
}

void expireStalledTransfers(uint32_t now){
  // These are emulator safety timeouts, not inferred protocol timings.
  if(packetReceived && packetLastRxMs && (uint32_t)(now-packetLastRxMs)>PACKET_REASSEMBLY_TIMEOUT_MS){
#if DEBUG_SERIAL
    Serial.println("PACKET RX timeout; dropping incomplete logical packet");
#endif
    packetReceived=packetExpected=0;
    packetLastRxMs=0;
    resetBulkTransfer(true);
  }
  if(audioRxReceived && audioRxLastMs && (uint32_t)(now-audioRxLastMs)>AUDIO_REASSEMBLY_TIMEOUT_MS){
#if DEBUG_SERIAL
    Serial.println("AUDIO RX timeout; dropping partial audio frame");
#endif
    resetAudioReassembly();
  }
  if(bulk.active && !deferredAssetCommit.active && bulkLastRxMs && (uint32_t)(now-bulkLastRxMs)>BULK_TRANSFER_TIMEOUT_MS){
#if DEBUG_SERIAL
    Serial.println("BULK timeout; aborting stalled transfer");
#endif
    resetBulkTransfer(true);
  }
  if(graffitiRaster.active && graffitiRaster.lastRxMs && (uint32_t)(now-graffitiRaster.lastRxMs)>BULK_TRANSFER_TIMEOUT_MS){
#if DEBUG_SERIAL && GRAFFITI_PROTOCOL_DEBUG
    Serial.println("GRAFFITI timeout; aborting stalled raster transfer");
#endif
    resetGraffitiRaster();
  }
}

bool processBulkPacket(const uint8_t *data,size_t len){
  // Protocol captured from an original 64x64 iDotMatrix and retained in 0.5.1.
  // Graffiti full-raster packets use their own 9-byte framing:
  //   0..1 LE logical packet length
  //   2    type 0x00
  //   3    0x00
  //   4    marker: 0x00 first, 0x02 continuation
  //   5..8 total raster size LE (12288 for 64x64 RGB)
  //   9..  RGB payload (4096 bytes in the captured 64x64 transfer)
  // Original hardware ACKs status 0x02 while more raster data is required,
  // then status 0x01 after the complete raster has been accepted. There is no
  // CRC field in this 9-byte Graffiti header. Keep this path completely
  // separate from the normal 16-byte GIF/RAW/TEXT Bulk state machine.
  if(len>=9 && data[2]==0x00 && data[3]==0x00 && (data[4]==0x00 || data[4]==0x02)){
    const uint8_t marker=data[4];
    const uint32_t total=(uint32_t)data[5]|((uint32_t)data[6]<<8)|((uint32_t)data[7]<<16)|((uint32_t)data[8]<<24);
    const uint32_t expected=(uint32_t)NUM_LEDS*3UL;
    const bool halfSize=MATRIX_WIDTH==64 && MATRIX_HEIGHT==64 && total==expected/4;
    if(total==expected || halfSize){
      const uint8_t *payload=data+9;
      const size_t payloadSize=len-9;
      if(marker==0x00){
        resetGraffitiRaster();
        idotMemoryTelemetrySnapshot("graffiti.before_alloc");
        graffitiRaster.data=(uint8_t*)malloc(total);
        idotMemoryTelemetrySnapshot(graffitiRaster.data ? "graffiti.after_alloc" : "graffiti.alloc_failed");
        if(!graffitiRaster.data){
#if DEBUG_SERIAL && GRAFFITI_PROTOCOL_DEBUG
          Serial.println("GRAFFITI BEGIN allocation failed");
#endif
          return true;
        }
        graffitiRaster.active=true;
        graffitiRaster.expectedSize=total;
#if DEBUG_SERIAL && GRAFFITI_PROTOCOL_DEBUG
        Serial.print("GRAFFITI BEGIN total="); Serial.print(total);
        Serial.print(" firstPayload="); Serial.println(payloadSize);
#endif
      } else if(!graffitiRaster.active || !graffitiRaster.data || graffitiRaster.expectedSize!=total){
#if DEBUG_SERIAL && GRAFFITI_PROTOCOL_DEBUG
        Serial.println("GRAFFITI CONT without matching active transfer; ignored");
#endif
        return true;
      }

      graffitiRaster.lastRxMs=millis();
      const uint32_t remain=graffitiRaster.expectedSize-graffitiRaster.receivedSize;
      const size_t useful=min((size_t)remain,payloadSize);
      if(useful){
        ::memcpy(graffitiRaster.data+graffitiRaster.receivedSize,payload,useful);
        graffitiRaster.receivedSize+=useful;
      }
      graffitiRaster.chunkCount++;

      const bool complete=graffitiRaster.receivedSize>=graffitiRaster.expectedSize;
#if DEBUG_SERIAL && GRAFFITI_PROTOCOL_DEBUG
      Serial.print("GRAFFITI RX marker=0x"); Serial.print(marker,HEX);
      Serial.print(" chunk="); Serial.print(graffitiRaster.chunkCount);
      Serial.print(" payload="); Serial.print(useful);
      Serial.print(" total="); Serial.print(graffitiRaster.receivedSize);
      Serial.print('/'); Serial.println(graffitiRaster.expectedSize);
#endif
      if(complete){
        switchDisplayMode(DISPLAY_GRAFFITI);
        for(uint8_t y=0;y<MATRIX_HEIGHT;y++) for(uint8_t x=0;x<MATRIX_WIDTH;x++){
          const uint16_t pix=halfSize ? uint16_t(y/2)*32+x/2 : uint16_t(y)*MATRIX_WIDTH+x;
          const uint32_t o=(uint32_t)pix*3UL;
          framebuffer[logicalIndex(x,y)]=CRGB(graffitiRaster.data[o],graffitiRaster.data[o+1],graffitiRaster.data[o+2]);
        }
        refreshMatrix();
        // Captured original hardware final ACK.
        sendTransferAck(0x00,0x01);
#if DEBUG_SERIAL && GRAFFITI_PROTOCOL_DEBUG
        Serial.print("GRAFFITI END chunks="); Serial.print(graffitiRaster.chunkCount);
        Serial.println(" ACK=01");
#endif
        resetGraffitiRaster();
        idotMemoryTelemetrySnapshot("graffiti.after_free");
      } else {
        // Captured original hardware continuation ACK.
        sendTransferAck(0x00,0x02);
#if DEBUG_SERIAL && GRAFFITI_PROTOCOL_DEBUG
        Serial.println("GRAFFITI ACK=02");
#endif
      }
      return true;
    }
  }
  if(len<16 || data[3]!=0x00 || data[2]<0x01 || data[2]>0x03) return false;
  uint8_t type=data[2];
  uint32_t total=(uint32_t)data[5]|((uint32_t)data[6]<<8)|((uint32_t)data[7]<<16)|((uint32_t)data[8]<<24);
  uint32_t crc=(uint32_t)data[9]|((uint32_t)data[10]<<8)|((uint32_t)data[11]<<16)|((uint32_t)data[12]<<24);
  uint16_t timeSign=(uint16_t)data[13]|((uint16_t)data[14]<<8);
  uint8_t imageIndex=data[15];
  const bool presetTraceSlot=(imageIndex>=0x0E && imageIndex<=0x13);
#if DEBUG_SERIAL && PRESET_PROTOCOL_DEBUG
  if((type==1 || type==3) && presetTraceSlot){
    Serial.print("PRESET BULK HDR type="); Serial.print(type);
    Serial.print(" slot="); Serial.print(imageIndex);
    Serial.print(" timeSign="); Serial.print(timeSign);
    Serial.print(" total="); Serial.print(total);
    Serial.print(" crc=0x"); Serial.println(crc,HEX);
    Serial.print("PRESET BULK RAW [16]: "); dumpHex(data,16);
  }
#endif
  if(!total || total>10UL*1024UL*1024UL) return false;
  if(type==3 && total>MAX_TEXT_PAYLOAD){
#if DEBUG_SERIAL
    Serial.print("TEXT RX rejected: declared payload "); Serial.print(total);
    Serial.print(" exceeds MAX_TEXT_PAYLOAD="); Serial.println(MAX_TEXT_PAYLOAD);
#endif
    resetBulkTransfer(true);
    sendTransferAck(type,0x03);
    return true;
  }
  const uint8_t *payload=data+16; size_t payloadSize=len-16;
  if(!bulk.active){
    resetBulkTransfer(); bulk.active=true; bulk.dataType=type; bulk.expectedSize=total; bulk.expectedCRC=crc; bulk.runningCRC=0xFFFFFFFF;
    bulk.timeSign=timeSign; bulk.imageIndex=imageIndex;
    if(startsWithGIF(payload,payloadSize)) bulk.format="GIF";
    else if(type==2 && startsWithPNG(payload,payloadSize)) bulk.format="PNG";
    else if(type==2 && total==(uint32_t)NUM_LEDS*3UL) bulk.format="RAW RGB";
    else if(type==3) bulk.format="TEXT";
#if DEBUG_SERIAL && BULK_PROTOCOL_DEBUG
    Serial.print("BULK BEGIN type="); Serial.print(type);
    Serial.print(" total="); Serial.print(total);
    Serial.print(" firstPayload="); Serial.print(payloadSize);
    if(type==1 || type==3){ Serial.print(" timeSign="); Serial.print(timeSign); Serial.print(" imageIndex="); Serial.print(imageIndex); }
    Serial.print(" format="); Serial.println(bulk.format.length() ? bulk.format : "UNKNOWN");
#endif
    if(carouselUploadOpen && carouselBankTxnActive && (type==1 || type==2 || type==3) &&
       imageIndex<CAROUSEL_SLOT_COUNT && !(carouselBankTxnManifest.touchedMask&(1U<<imageIndex))){
#if DEBUG_SERIAL
      Serial.print("[CARBANK] state=reject reason=undeclared_slot slot="); Serial.println(imageIndex);
#endif
      resetBulkTransfer(true); sendTransferAck(type,0x03); return true;
    }
#if defined(IDOTMATRIX_BOARD_WAVESHARE_S3_RGB_MATRIX)
    // Waveshare stack-safety boundary: every persistent Carousel
    // asset (GIF, static IMAGE/PNG/RAW, TEXT) is accumulated in PSRAM first.
    // No LittleFS open/write is allowed from nimble_host for Device Assets.
    if((type==1 || type==2 || type==3) && carouselUploadOpen &&
       imageIndex<CAROUSEL_SLOT_COUNT &&
       (!carouselBankTxnActive || (carouselBankTxnManifest.touchedMask&(1U<<imageIndex)))){
      if(carouselActive && carouselActiveSlot==(int8_t)imageIndex) stopCarouselPlayback();
      if(!beginCarouselBufferedImageTransfer(total,imageIndex,timeSign)){
        resetBulkTransfer(true); sendTransferAck(type,0x03); return true;
      }
    }
#else
    // Other targets retain the qualified streaming path. Type 2 has always
    // required buffering because it can carry a static PNG rather than RGB24.
    if(type==2 && carouselUploadOpen && imageIndex<CAROUSEL_SLOT_COUNT &&
       (!carouselBankTxnActive || (carouselBankTxnManifest.touchedMask&(1U<<imageIndex)))){
      if(!beginCarouselBufferedImageTransfer(total,imageIndex,timeSign)){
        resetBulkTransfer(true); sendTransferAck(type,0x03); return true;
      }
    }
#endif
    if((type==1 || type==3) && presetTraceSlot){
      // A new preset invocation re-sends the whole selected bank. Freeze the
      // previous preset on its last framebuffer while replacement assets arrive.
      // Remember an active old Preset so abort/disconnect can resume it.
      const bool previousPresetActive=presetActive;
      const int8_t previousPresetSlot=presetActiveSlot;
      if(presetActive) stopPresetPlayback();
      if(!littleFsReady){
#if DEBUG_SERIAL
        Serial.println("PRESET BULK rejected: LittleFS unavailable");
#endif
        resetBulkTransfer(true); sendTransferAck(type,0x03); return true;
      }
      size_t fsTotal=LittleFS.totalBytes();
      size_t fsUsed=LittleFS.usedBytes();
      size_t fsFree=fsTotal>fsUsed ? fsTotal-fsUsed : 0;
      if((uint64_t)total > (uint64_t)fsFree){
#if DEBUG_SERIAL
        Serial.print("PRESET BULK rejected: bytes="); Serial.print(total);
        Serial.print(" LittleFS free="); Serial.println(fsFree);
#endif
        resetBulkTransfer(true); sendTransferAck(type,0x03); return true;
      }
      if(!presetBankTxnActive) beginPresetBankTransaction(previousPresetActive ? 1U : 0U,previousPresetSlot);
      uint8_t localSlot=(uint8_t)(imageIndex-PRESET_SLOT_BASE);
      bulk.presetLocalSlot=(int8_t)localSlot;
      bulk.presetToFS=true;
      String tmp=presetTempFileName(localSlot);
      LittleFS.remove(tmp);
      gifBulkFile=LittleFS.open(tmp,"w");
      if(!gifBulkFile){ resetBulkTransfer(true); sendTransferAck(type,0x03); return true; }
      gifRxWriteOffset=0;
      presetTransferIndicatorActive=true;
      presetTransferLastActivityAt=millis();
      beginCarouselTransferIndicator(total,localSlot);
#if DEBUG_SERIAL && PRESET_PROTOCOL_DEBUG
      Serial.print("PRESET RX BEGIN slot="); Serial.print(imageIndex);
      Serial.print(" type="); Serial.print(type);
      Serial.print(" timeSign="); Serial.println(timeSign);
#endif
    }
#if !defined(IDOTMATRIX_BOARD_WAVESHARE_S3_RGB_MATRIX)
    if(type==3 && !bulk.presetToFS && !bulk.carouselBuffered && carouselUploadOpen && imageIndex<CAROUSEL_SLOT_COUNT &&
       (!carouselBankTxnActive || (carouselBankTxnManifest.touchedMask&(1U<<imageIndex)))){
      if(!littleFsReady){
#if DEBUG_SERIAL
        Serial.println("BULK TEXT carousel rejected: LittleFS unavailable");
#endif
        resetBulkTransfer(true); sendTransferAck(type,0x03); return true;
      }
      size_t fsTotal=LittleFS.totalBytes();
      size_t fsUsed=LittleFS.usedBytes();
      size_t fsFree=fsTotal>fsUsed ? fsTotal-fsUsed : 0;
      if((uint64_t)total > (uint64_t)fsFree){
#if DEBUG_SERIAL
        Serial.print("BULK TEXT carousel rejected: bytes="); Serial.print(total);
        Serial.print(" LittleFS free="); Serial.println(fsFree);
#endif
        resetBulkTransfer(true); sendTransferAck(type,0x03); return true;
      }
      uint8_t localSlot=imageIndex;
      bulk.carouselLocalSlot=(int8_t)localSlot;
      String tmp=carouselTempFileName(localSlot);
      LittleFS.remove(tmp);
      gifBulkFile=LittleFS.open(tmp,"w");
      if(!gifBulkFile){ resetBulkTransfer(true); sendTransferAck(type,0x03); return true; }
      bulk.carouselToFS=true;
      gifRxWriteOffset=0;
      beginCarouselTransferIndicator(total,localSlot);
#if DEBUG_SERIAL && BULK_PROTOCOL_DEBUG
      Serial.print("BULK TEXT STORAGE: CAROUSEL slot="); Serial.print(localSlot);
      Serial.print(" dwell="); Serial.println(timeSign);
#endif
    }
#endif
    if(type==1 && !bulk.presetToFS && !bulk.carouselBuffered){
      if(!littleFsReady){
#if DEBUG_SERIAL
        Serial.println("BULK GIF rejected: LittleFS unavailable");
#endif
        resetBulkTransfer(true); sendTransferAck(type,0x03); return true;
      }
      size_t fsTotal=LittleFS.totalBytes();
      size_t fsUsed=LittleFS.usedBytes();
      size_t fsFree=fsTotal>fsUsed ? fsTotal-fsUsed : 0;
      if((uint64_t)total > (uint64_t)fsFree){
#if DEBUG_SERIAL
        Serial.print("BULK GIF rejected: bytes="); Serial.print(total);
        Serial.print(" LittleFS free="); Serial.println(fsFree);
#endif
        resetBulkTransfer(true); sendTransferAck(type,0x03); return true;
      }

#if !defined(IDOTMATRIX_BOARD_WAVESHARE_S3_RGB_MATRIX)
      if(carouselUploadOpen && imageIndex<CAROUSEL_SLOT_COUNT &&
         (!carouselBankTxnActive || (carouselBankTxnManifest.touchedMask&(1U<<imageIndex)))){
        // Non-Waveshare targets retain the established persistent streaming path.
        uint8_t localSlot=imageIndex;
        if(carouselActive && carouselActiveSlot==(int8_t)localSlot) stopCarouselPlayback();
        bulk.carouselLocalSlot=(int8_t)localSlot;
        String tmp=carouselTempFileName(localSlot);
        LittleFS.remove(tmp);
        gifBulkFile=LittleFS.open(tmp,"w");
        if(!gifBulkFile){ resetBulkTransfer(true); sendTransferAck(type,0x03); return true; }
        bulk.carouselToFS=true;
        gifRxWriteOffset=0;
        beginCarouselTransferIndicator(total,localSlot);
#if DEBUG_SERIAL && BULK_PROTOCOL_DEBUG
        Serial.print("BULK GIF STORAGE: CAROUSEL slot="); Serial.print(localSlot);
        Serial.print(" dwell="); Serial.println(timeSign);
#endif
      } else
#endif
      {
        // Live/preview GIF path: preserve the stable alternating RX -> PLAY flow.
        int8_t slot=(int8_t)(nextGifRxSlot & 1U);
        nextGifRxSlot ^= 1U;
        if(pendingGifStart && slot==pendingGifSlot) slot ^= 1;
        bulk.gifRxSlot=slot;
        const char *rxPath=GIF_RX_FILES[(uint8_t)slot];
        LittleFS.remove(rxPath);
        gifBulkFile=LittleFS.open(rxPath,"w");
        if(!gifBulkFile){ resetBulkTransfer(true); sendTransferAck(type,0x03); return true; }
        bulk.gifToFS=true;
        gifRxWriteOffset=0;
#if DEBUG_SERIAL && BULK_PROTOCOL_DEBUG
        Serial.print("BULK GIF STORAGE: LittleFS RX slot="); Serial.print(slot);
        Serial.print(" imageIndex="); Serial.println(imageIndex);
#endif
      }
    }
    if(type==2 && bulk.format=="RAW RGB" && !bulk.carouselBuffered){
      idotMemoryTelemetrySnapshot("raw.before_alloc");
      rawRgbData=(uint8_t*)malloc(total);
      idotMemoryTelemetrySnapshot(rawRgbData ? "raw.after_alloc" : "raw.alloc_failed");
      if(!rawRgbData){ resetBulkTransfer(true); sendTransferAck(type,0x03); return true; }
      rawRgbWriteOffset=0;
    }
  }
  if(type==2 && bulk.format=="UNKNOWN" && startsWithPNG(payload,payloadSize)) bulk.format="PNG";
  bulkLastRxMs=millis();
  if(type!=bulk.dataType || total!=bulk.expectedSize || crc!=bulk.expectedCRC ||
     ((type==1 || bulk.carouselToFS || bulk.carouselBuffered || bulk.presetToFS) && (timeSign!=bulk.timeSign || imageIndex!=bulk.imageIndex))){
#if DEBUG_SERIAL
    Serial.println("BULK header mismatch; aborting transaction");
#endif
    resetBulkTransfer(true); return true;
  }
  uint32_t remain=bulk.expectedSize-bulk.receivedSize; size_t useful=min((size_t)remain,payloadSize);
  bulk.runningCRC=crc32Update(bulk.runningCRC,payload,useful);
  if(bulk.carouselToFS || bulk.presetToFS || (type==1 && bulk.gifToFS)){
    size_t n=gifBulkFile ? gifBulkFile.write(payload,useful) : 0;
    gifRxWriteOffset+=n;
    if(n!=useful){
#if DEBUG_SERIAL
      Serial.println("BULK asset LittleFS RX write error");
#endif
      resetBulkTransfer(true); sendTransferAck(type,0x03); return true;
    }
  }
  if(bulk.carouselBuffered && carouselImageRxData && carouselImageRxWriteOffset+useful<=bulk.expectedSize){
    ::memcpy(carouselImageRxData+carouselImageRxWriteOffset,payload,useful);
    carouselImageRxWriteOffset+=useful;
  }
  if(type==2 && !bulk.carouselBuffered && rawRgbData && rawRgbWriteOffset+useful<=bulk.expectedSize){
    ::memcpy(rawRgbData+rawRgbWriteOffset,payload,useful);
    rawRgbWriteOffset+=useful;
  }
  if(type==3 && !bulk.carouselToFS && !bulk.presetToFS){
    size_t off=bulk.receivedSize;
    if(off<MAX_TEXT_PAYLOAD){ size_t cp=min(useful,(size_t)MAX_TEXT_PAYLOAD-off); ::memcpy(textPayload+off,payload,cp); textPayloadReceived=off+cp; }
  }
  bulk.receivedSize+=useful; bulk.chunkCount++;
  if(bulk.carouselToFS || bulk.carouselBuffered || bulk.presetToFS) {
    updateCarouselTransferIndicatorProgress(bulk.receivedSize,bulk.expectedSize);
    if(bulk.presetToFS) presetTransferLastActivityAt=millis();
  }
  if(bulk.receivedSize>=bulk.expectedSize){
    bool ok=((bulk.runningCRC^0xFFFFFFFF)==bulk.expectedCRC);
#if DEBUG_SERIAL && TEXT_PROTOCOL_DEBUG
    if(type==3){
      Serial.print("TEXT RX COMPLETE len="); Serial.print((bulk.carouselToFS || bulk.carouselBuffered) ? bulk.receivedSize : textPayloadReceived);
      Serial.print(" carousel="); Serial.print((bulk.carouselToFS || bulk.carouselBuffered) ? "YES" : "NO");
      Serial.print(" crc="); Serial.println(ok ? "OK" : "BAD");
    }
#endif
    if(bulk.carouselToFS || bulk.carouselBuffered || bulk.presetToFS){
      const bool isCarousel=bulk.carouselToFS || bulk.carouselBuffered;
      uint8_t localSlot=isCarousel
          ? ((bulk.carouselLocalSlot>=0) ? (uint8_t)bulk.carouselLocalSlot : 0xFF)
          : ((bulk.presetLocalSlot>=0) ? (uint8_t)bulk.presetLocalSlot : 0xFF);
      const bool slotValid=isCarousel ? (localSlot<CAROUSEL_SLOT_COUNT) : (localSlot<PRESET_SLOT_COUNT);
      const bool payloadComplete=bulk.carouselBuffered
          ? (carouselImageRxData && carouselImageRxWriteOffset==bulk.expectedSize)
          : (gifRxWriteOffset==bulk.expectedSize);
      const bool integrityOk=ok && slotValid && payloadComplete;
      const DeferredAssetCommitKind kind=isCarousel ? DEFERRED_ASSET_CAROUSEL : DEFERRED_ASSET_PRESET;
      if(!queueDeferredAssetCommit(kind,type,localSlot,bulk.timeSign,bulk.expectedSize,bulk.expectedCRC,integrityOk)){
#if DEBUG_SERIAL
        Serial.println("BULK deferred commit queue busy; withholding final ACK");
#endif
      }
      // Do not flush/close/remove/rename/reset or send the final ACK from the
      // BLE host task. loop() owns the complete finalization transaction.
      return true;
    } else {
#if DEBUG_SERIAL && PRESET_PROTOCOL_DEBUG
      if(type==3 && ok && textPayloadReceived && presetTraceSlot){
        const size_t h=min(textPayloadReceived,(size_t)TEXT_GLOBAL_HEADER);
        Serial.print("PRESET TEXT GLOBAL ["); Serial.print(h); Serial.print("]: "); dumpHex(textPayload,h);
        if(textPayloadReceived>TEXT_GLOBAL_HEADER){
          const size_t m=min(textPayloadReceived-(size_t)TEXT_GLOBAL_HEADER,(size_t)TEXT_GLYPH_META);
          Serial.print("PRESET TEXT META0 ["); Serial.print(m); Serial.print("]: "); dumpHex(textPayload+TEXT_GLOBAL_HEADER,m);
        }
      }
#endif
      if(type==3 && ok && textPayloadReceived) parseTextPayload(textPayload,textPayloadReceived);
      if(type==1){
        if(gifBulkFile){ gifBulkFile.flush(); gifBulkFile.close(); }
        if(ok && bulk.gifToFS && gifRxWriteOffset==bulk.expectedSize && bulk.gifRxSlot>=0){
          // Publish only metadata here. No AnimatedGIF calls, no closing the
          // current decoder and no PLAY-file mutation are allowed from Core 0.
          pendingGifSlot=bulk.gifRxSlot;
          pendingGifSize=bulk.expectedSize;
          pendingGifCRC=bulk.expectedCRC;
          pendingGifStartAt=millis();
          pendingGifStart=true;
#if DEBUG_SERIAL && BULK_PROTOCOL_DEBUG
          Serial.print("GIF RX COMPLETE queued slot="); Serial.print((int)bulk.gifRxSlot);
          Serial.print(" bytes="); Serial.println(bulk.expectedSize);
#endif
        } else {
          if(bulk.gifRxSlot>=0) LittleFS.remove(GIF_RX_FILES[(uint8_t)bulk.gifRxSlot]);
        }
      }
    }
    if(type==2 && !bulk.carouselBuffered && ok && rawRgbData && rawRgbWriteOffset==bulk.expectedSize){
      switchDisplayMode(DISPLAY_RAW);
      for(uint8_t y=0;y<MATRIX_HEIGHT;y++) for(uint8_t x=0;x<MATRIX_WIDTH;x++){
        const uint16_t pix=(uint16_t)y*MATRIX_WIDTH+x;
        const uint32_t o=(uint32_t)pix*3UL;
        framebuffer[logicalIndex(x,y)]=CRGB(rawRgbData[o],rawRgbData[o+1],rawRgbData[o+2]);
      }
      refreshMatrix();
    }
#if DEBUG_SERIAL && BULK_PROTOCOL_DEBUG
    Serial.print("BULK END type="); Serial.print(type);
    Serial.print(" total="); Serial.print(bulk.receivedSize);
    Serial.print(" chunks="); Serial.print(bulk.chunkCount);
    if(bulk.carouselToFS || bulk.carouselBuffered){
      Serial.print(" storage=CAROUSEL slot="); Serial.print((int)bulk.carouselLocalSlot);
      Serial.print(" type="); Serial.print(carouselDataTypeLabel(type));
      Serial.print(bulk.carouselBuffered ? " mode=BUFFERED" : " mode=FS");
      Serial.print(" dwell="); Serial.print(bulk.timeSign);
    } else if(bulk.presetToFS){
      Serial.print(" storage=PRESET slot="); Serial.print((int)(PRESET_SLOT_BASE+bulk.presetLocalSlot));
      Serial.print(" type="); Serial.print(type==3 ? "TEXT" : "GIF");
      Serial.print(" timeSign="); Serial.print(bulk.timeSign);
    } else if(type==1){
      Serial.print(" storage=LittleFS-RX"); Serial.print(" slot="); Serial.print((int)bulk.gifRxSlot); Serial.print(" imageIndex="); Serial.print(bulk.imageIndex);
    }
    Serial.print(" crc="); Serial.println(ok ? "OK" : "BAD");
#endif
    sendTransferAck(type,0x03);
    const bool telemetryRawTransfer = (type==2);
    resetBulkTransfer();
    if(telemetryRawTransfer) idotMemoryTelemetrySnapshot("raw.after_free");
  } else sendTransferAck(type,0x01);
  return true;
}

// ======================================================
// AUDIO / RHYTHM RENDERER
// ======================================================
static uint32_t audioLastLogMs = 0;
static int audioLastUiEffect = -1;

static CRGB audioRainbow(uint8_t pos, uint8_t value = 255) {
  return CHSV(pos, 255, value);
}

static void audioPixel(int16_t x, int16_t y, const CRGB &c) {
  if (x >= 0 && x < MATRIX_WIDTH && y >= 0 && y < MATRIX_HEIGHT) putPixel(x, y, c);
}

static void drawLineAudio(int x0,int y0,int x1,int y1,const CRGB &c) {
  int dx=abs(x1-x0), sx=x0<x1?1:-1;
  int dy=-abs(y1-y0), sy=y0<y1?1:-1;
  int err=dx+dy;
  while(true){
    audioPixel(x0,y0,c);
    if(x0==x1 && y0==y1) break;
    int e2=2*err;
    if(e2>=dy){ err+=dy; x0+=sx; }
    if(e2<=dx){ err+=dx; y0+=sy; }
  }
}

static void drawAudioHeart(int16_t ox, int16_t oy, uint8_t scale, const CRGB &outline, const CRGB &fill) {
  static const uint16_t rows[8] = {
    0b001101100, 0b011111110, 0b111111111, 0b111111111,
    0b011111110, 0b001111100, 0b000111000, 0b000010000
  };
  for (uint8_t y=0; y<8; y++) {
    for (uint8_t x=0; x<9; x++) {
      if (!(rows[y] & (1 << (8-x)))) continue;
      bool edge = false;
      if (x==0 || x==8 || y==0 || y==7) edge=true;
      else {
        bool l = rows[y] & (1 << (8-(x-1)));
        bool r = rows[y] & (1 << (8-(x+1)));
        bool u = y ? (rows[y-1] & (1 << (8-x))) : false;
        bool d = y<7 ? (rows[y+1] & (1 << (8-x))) : false;
        edge = !(l && r && u && d);
      }
      audioPixel(ox+x, oy+y, edge ? outline : fill);
      if (scale && !edge && x>1 && x<7 && y>1 && y<6)
        audioPixel(ox+x, oy+y, fill + CRGB(40,40,40));
    }
  }
}

// LEVEL 5 - red/cyan robot face seen in the app.
static void renderAudioRobotFace(uint8_t level) {
  clearFramebuffer();
  const CRGB red(255,20,25), cyan(0,235,255), blue(15,55,255), darkBlue(4,15,90);

  // Two square red eyes with cyan pupils.
  for(int y=3;y<=7;y++) {
    for(int x=1;x<=5;x++) audioPixel(x,y,red);
    for(int x=10;x<=14;x++) audioPixel(x,y,red);
  }
  for(int y=4;y<=6;y++) {
    for(int x=2;x<=3;x++) audioPixel(x,y,cyan);
    for(int x=12;x<=13;x++) audioPixel(x,y,cyan);
  }

  // Pupils drift one pixel with the beat.
  int drift=(level>=5)?1:0;
  audioPixel(3+drift,5,CRGB::White);
  audioPixel(12+drift,5,CRGB::White);

  // Blue mouth: two horizontal rows, height reacts slightly to level.
  for(int x=3;x<=12;x++) audioPixel(x,11,blue);
  for(int x=4;x<=11;x++) audioPixel(x,12,(level>=5)?blue:darkBlue);
  if(level>=7) for(int x=5;x<=10;x++) audioPixel(x,13,blue);
}

// LEVEL 4 - purple face with moving eyes and two lips that open/expand.
static void renderAudioPurpleFace(uint8_t level) {
  clearFramebuffer();
  const CRGB purple(125,20,225), purple2(82,8,165);
  const CRGB skin(248,235,210), iris(30,25,65), lip(255,35,55), lipHi(255,115,125);

  // Hair/head silhouette, deliberately irregular like the original sprite.
  for(int y=1;y<=13;y++) for(int x=1;x<=14;x++) {
    if((y==1 && (x<4||x>11)) || (y==2 && (x<2||x>13))) continue;
    if((x==1||x==14) && (y<4||y>11)) continue;
    audioPixel(x,y, ((x+y)&1)?purple:purple2);
  }

  // Pale eye blocks.
  for(int y=5;y<=8;y++) {
    for(int x=3;x<=6;x++) audioPixel(x,y,skin);
    for(int x=9;x<=12;x++) audioPixel(x,y,skin);
  }
  int eyeShift = (int)((millis()/180 + level) % 3) - 1;
  audioPixel(5+eyeShift,7,iris);
  audioPixel(10+eyeShift,7,iris);

  // Two lips: the opening widens with level, rather than a sliding line.
  uint8_t open = constrain((int)level,1,7);
  int halfW = 2 + open/2;                 // 2..5 pixels each side
  int cx=8;
  for(int dx=-halfW; dx<=halfW; dx++) {
    int taper=abs(dx);
    if(taper<=halfW) audioPixel(cx+dx,10 + (taper>halfW-2), lipHi);
    if(taper<=halfW) audioPixel(cx+dx,12 - (taper>halfW-2), lip);
  }
  // Dark opening between the lips grows vertically on louder beats.
  for(int x=cx-halfW+1;x<=cx+halfW-1;x++) audioPixel(x,11,CRGB(35,0,25));
  if(open>=6) for(int x=cx-halfW+2;x<=cx+halfW-2;x++) audioPixel(x,12,CRGB(35,0,25));
}

// LEVEL 5 - manufacturer face reconstructed from the supplied hardware video.
// Eyes and mouth are independent 4-state components rather than four coupled
// animation frames.  The global LEVEL controls cadence; FFT bands are unused.
struct ObservedFaceState {
  uint8_t eyes=1, mouth=0;
  uint32_t rng=0xA341316Cu;
  uint32_t nextEyesMs=0, nextMouthMs=0;
  bool initialized=false;
};
static ObservedFaceState observedFaceState;

static uint32_t observedFaceRand() {
  uint32_t x=observedFaceState.rng;
  x ^= x << 13; x ^= x >> 17; x ^= x << 5;
  observedFaceState.rng=x ? x : 0xA341316Cu;
  return observedFaceState.rng;
}
static uint8_t observedFaceDifferent(uint8_t current) {
  uint8_t next=(uint8_t)(observedFaceRand()&3U);
  if(next==current) next=(uint8_t)((next+1U+(observedFaceRand()%3U))&3U);
  return next;
}
static void drawObservedFaceRows(const uint16_t rows[16], const CRGB &c) {
  for(uint8_t y=0;y<16;y++) for(uint8_t x=0;x<16;x++) if(rows[y]&(1U<<x)) audioPixel(x,y,c);
}
static void renderAudioObservedFace(uint8_t level) {
  static const uint16_t eyeRed[4][16] = {
    {0,0,0,0,0x381C,0x4422,0x381C,0,0,0,0,0,0,0,0,0},
    {0,0x381C,0x4422,0x4422,0x4422,0x4422,0x381C,0,0,0,0,0,0,0,0,0},
    {0,0x381C,0x4422,0x4422,0x4422,0x4422,0x381C,0,0,0,0,0,0,0,0,0},
    {0,0x381C,0x4422,0x4422,0x4422,0x4422,0x381C,0,0,0,0,0,0,0,0,0}
  };
  static const uint16_t eyeWhite[4][16] = {
    {0,0,0,0,0,0x381C,0,0,0,0,0,0,0,0,0,0},
    {0,0,0x381C,0x2814,0x2814,0x381C,0,0,0,0,0,0,0,0,0},
    {0,0,0x381C,0x381C,0x381C,0x2010,0,0,0,0,0,0,0,0,0},
    {0,0,0x381C,0x3018,0x3018,0x381C,0,0,0,0,0,0,0,0,0}
  };
  static const uint16_t mouthBlue[4][16] = {
    {0,0,0,0,0,0,0,0,0,0x3FFC,0x7FFE,0,0x7FFE,0x3FFC,0,0},
    {0,0,0,0,0,0,0,0,0x3FFC,0x7FFE,0,0,0,0x7FFE,0x3FFC,0},
    {0,0,0,0,0,0,0,0,0,0,0x3FFC,0x7FFE,0x7FFE,0x3FFC,0,0},
    {0,0,0,0,0,0,0,0,0x3FFC,0x7FFE,0,0,0,0x7FFE,0x3FFC,0}
  };
  static const uint16_t mouthRed[4][16] = {
    {0},{0},{0},{0,0,0,0,0,0,0,0,0,0,0x0180,0x0240,0,0,0,0}
  };
  const uint32_t now=millis();
  if(!observedFaceState.initialized){
    observedFaceState.initialized=true;
    observedFaceState.nextEyesMs=now+420;
    observedFaceState.nextMouthMs=now+520;
  }
  const uint16_t eyeBase=(uint16_t)max(180,520-(int)level*22);
  const uint16_t mouthBase=(uint16_t)max(220,640-(int)level*24);
  if((int32_t)(now-observedFaceState.nextEyesMs)>=0){
    observedFaceState.eyes=observedFaceDifferent(observedFaceState.eyes);
    observedFaceState.nextEyesMs=now+eyeBase+(observedFaceRand()%180U);
  }
  if((int32_t)(now-observedFaceState.nextMouthMs)>=0){
    observedFaceState.mouth=observedFaceDifferent(observedFaceState.mouth);
    observedFaceState.nextMouthMs=now+mouthBase+(observedFaceRand()%220U);
  }
  clearFramebuffer();
  drawObservedFaceRows(eyeRed[observedFaceState.eyes],CRGB(232,0,11));
  drawObservedFaceRows(eyeWhite[observedFaceState.eyes],CRGB(255,255,252));
  drawObservedFaceRows(mouthBlue[observedFaceState.mouth],CRGB(8,69,247));
  drawObservedFaceRows(mouthRed[observedFaceState.mouth],CRGB(232,0,11));
}

// LEVEL 2 - retained because it was visually accepted.
static void renderAudioHeartLevel(uint8_t level) {
  clearFramebuffer();
  uint8_t hot = (uint8_t)min(255, 80 + (int)level*24);
  drawAudioHeart(3,4,level>=5,CRGB::White,CRGB(hot,0,20));
  if(level>=6) { audioPixel(1,7,CRGB(255,0,30)); audioPixel(14,7,CRGB(255,0,30)); }
}

// LEVEL 1 - manufacturer-style breakdancer.  The captured reference frames
// are an atlas of independent body-part alternatives, not a frame sequence.
// State changes are considered ONLY when a new LEVEL packet arrives.  Stale
// audio and silence therefore freeze the exact current composition.
struct BreakdancerState {
  uint8_t head=0, leftArm=0, rightArm=0, leftLeg=0, rightLeg=0, bg=0;
  uint32_t rng=0x6D2B79F5u;
  uint32_t lastProcessedPacket=0;
  bool initialized=false;
};
static BreakdancerState breakdancerState;

static uint32_t breakdancerRand() {
  uint32_t x=breakdancerState.rng;
  x ^= x << 13; x ^= x >> 17; x ^= x << 5;
  breakdancerState.rng=x ? x : 0x6D2B79F5u;
  return breakdancerState.rng;
}

static void drawAudioPixels(const int8_t (*pts)[2], uint8_t count, const CRGB &c) {
  for(uint8_t i=0;i<count;i++) audioPixel(pts[i][0],pts[i][1],c);
}

static void renderAudioBreakdancer(uint8_t level) {
  clearFramebuffer();
  const CRGB white=CRGB::White;

  if(!breakdancerState.initialized) {
    breakdancerState.initialized=true;
    breakdancerState.rng ^= millis() ^ (audioState.packetCounter*0x9E3779B9u);
    breakdancerState.lastProcessedPacket=audioState.packetCounter;
  }

  const bool newPacket = audioState.packetCounter != breakdancerState.lastProcessedPacket;
  if(newPacket) {
    breakdancerState.lastProcessedPacket=audioState.packetCounter;
    // Silence freezes.  Louder packets make each independent component more
    // likely to choose a different valid alternative.
    if(level>1U) {
      const uint8_t chance=(uint8_t)constrain(18 + (int)level*6, 24, 92);
      if((breakdancerRand()%100)<chance) breakdancerState.head=(uint8_t)(breakdancerRand()%3);
      if((breakdancerRand()%100)<chance) breakdancerState.leftArm=(uint8_t)(breakdancerRand()%2);
      if((breakdancerRand()%100)<chance) breakdancerState.rightArm=(uint8_t)(breakdancerRand()%2);
      if((breakdancerRand()%100)<chance) breakdancerState.leftLeg=(uint8_t)(breakdancerRand()%2);
      if((breakdancerRand()%100)<chance) breakdancerState.rightLeg=(uint8_t)(breakdancerRand()%2);
      if((breakdancerRand()%100)<chance) breakdancerState.bg=(uint8_t)(breakdancerRand()%3);
    }
  }

  static const CRGB bgColours[3]={ CRGB(90,215,70), CRGB(247,255,100), CRGB(46,228,222) };
  const CRGB bg=bgColours[breakdancerState.bg%3];
  for(int y=0;y<3;y++) for(int x=0;x<16;x++) if(((x+y)&1)==1) audioPixel(x,y,bg);

  static const int8_t torso[][2]={{7,5},{8,5},{7,6},{8,6},{7,7},{8,7},{7,8},{8,8},{7,9},{8,9},{7,10},{8,10}};
  drawAudioPixels(torso,sizeof(torso)/sizeof(torso[0]),white);
  static const int8_t head0[][2]={{6,1},{7,1},{8,1},{9,1},{6,2},{7,2},{8,2},{9,2},{6,3},{7,3},{8,3},{9,3},{10,3},{6,4},{7,4},{8,4},{9,4}};
  static const int8_t head1[][2]={{7,1},{8,1},{9,1},{10,1},{7,2},{8,2},{9,2},{10,2},{7,3},{8,3},{9,3},{10,3},{7,4},{8,4},{9,4},{10,4}};
  static const int8_t head2[][2]={{5,1},{6,1},{7,1},{8,1},{5,2},{6,2},{7,2},{8,2},{5,3},{6,3},{7,3},{8,3},{5,4},{6,4},{7,4},{8,4}};
  if(breakdancerState.head==1) drawAudioPixels(head1,sizeof(head1)/sizeof(head1[0]),white);
  else if(breakdancerState.head==2) drawAudioPixels(head2,sizeof(head2)/sizeof(head2[0]),white);
  else drawAudioPixels(head0,sizeof(head0)/sizeof(head0[0]),white);

  static const int8_t leftArm0[][2]={{6,6},{5,7},{4,8},{3,9}};
  static const int8_t leftArm1[][2]={{6,7},{5,7},{4,7},{3,7},{2,8}};
  static const int8_t rightArm0[][2]={{9,6},{10,7},{11,8},{12,9}};
  static const int8_t rightArm1[][2]={{9,7},{10,7},{11,7},{12,7},{13,8}};
  drawAudioPixels(breakdancerState.leftArm?leftArm1:leftArm0, breakdancerState.leftArm?5:4, white);
  drawAudioPixels(breakdancerState.rightArm?rightArm1:rightArm0, breakdancerState.rightArm?5:4, white);

  static const int8_t leftLeg0[][2]={{7,11},{6,12},{5,12},{5,13},{5,14},{6,12}};
  static const int8_t leftLeg1[][2]={{6,11},{7,11},{6,12},{6,13},{6,14},{5,15},{6,15}};
  static const int8_t rightLeg0[][2]={{9,11},{9,12},{9,13},{9,14},{9,15},{10,15}};
  static const int8_t rightLeg1[][2]={{8,11},{9,11},{9,12},{9,13},{9,14},{9,15},{10,15}};
  drawAudioPixels(breakdancerState.leftLeg?leftLeg1:leftLeg0, breakdancerState.leftLeg?7:6, white);
  drawAudioPixels(breakdancerState.rightLeg?rightLeg1:rightLeg0, breakdancerState.rightLeg?7:6, white);
}

static uint8_t bandHeight(uint8_t v) { return constrain((int)v, 0, 12); }

// FFT 0 - already validated by the user.
static void renderAudioFFT0(const uint8_t *b) {
  clearFramebuffer();
  for(uint8_t x=0;x<16;x++) {
    uint8_t v=bandHeight(b[x<8?x:15-x]);
    uint8_t half=(v+1)/2;
    for(uint8_t n=0;n<half;n++) {
      int y1=7-n, y2=8+n;
      CRGB c=audioRainbow((uint8_t)(x*15+n*8));
      audioPixel(x,y1,c); audioPixel(x,y2,c);
    }
  }
}

// FFT 1 - same geometry as FFT0, but COLOR BELONGS TO THE ROW.
// Two red centre rows, then yellow, green, cyan, blue, violet outwards.
static CRGB fftRowColor(uint8_t distanceFromCenter) {
  static const CRGB rowPalette[8]={
    CRGB(255,20,20),   // centre pair
    CRGB(255,185,0),   // yellow/orange
    CRGB(235,255,0),   // yellow-green
    CRGB(30,255,40),   // green
    CRGB(0,245,220),   // cyan
    CRGB(0,120,255),   // blue
    CRGB(120,40,255),  // violet
    CRGB(255,30,210)   // fuchsia
  };
  return rowPalette[min((uint8_t)7,distanceFromCenter)];
}

static void renderAudioFFT1(const uint8_t *b) {
  clearFramebuffer();
  for(uint8_t x=0;x<16;x++) {
    uint8_t v=bandHeight(b[x<8?x:15-x]);
    uint8_t half=(uint8_t)max(1,(int)(v+1)/2);
    for(uint8_t n=0;n<half && n<8;n++) {
      CRGB c=fftRowColor(n);
      audioPixel(x,7-n,c);
      audioPixel(x,8+n,c);
    }
  }
}

// FFT 4 - validated: top/bottom bars moving toward centre.
static void renderAudioFFT3(const uint8_t *b) {
  clearFramebuffer();
  for(uint8_t x=0;x<16;x++) {
    uint8_t v=bandHeight(b[x<8?x:15-x]);
    uint8_t h=(uint8_t)min(7,(int)(v+1)/2);
    for(uint8_t n=0;n<h;n++) {
      CRGB c=audioRainbow((uint8_t)(150+x*11+n*7));
      audioPixel(x,n,c);
      audioPixel(x,15-n,c);
    }
  }
}

static uint8_t audioProtocolIndex(bool fft, uint8_t mode) {
  if (fft) return (uint8_t)(5 + mode);
  return mode;
}

// LEVEL 3 - existing internal visualization plus WLED-reference moving perimeter.
// Perimeter: one cyan pixel ON followed by two OFF, advancing one logical
// perimeter pixel counter-clockwise approximately every 95 ms.
static void renderAudioFakeSpectrum(uint8_t level) {
  clearFramebuffer();
  const CRGB frame(0,235,255);
  const uint8_t phase=(uint8_t)((millis()/95UL)%60UL);
  for(uint8_t i=0;i<60;i++) {
    if(((uint8_t)(i+phase)%3U)!=0U) continue;
    int16_t x=0,y=0;
    // Counter-clockwise perimeter order: top L->R, right T->B,
    // bottom R->L, left B->T.
    if(i<16){ x=i; y=0; }
    else if(i<31){ x=15; y=i-15; }
    else if(i<46){ x=45-i; y=15; }
    else { x=0; y=60-i; }
    audioPixel(x,y,frame);
  }

  uint8_t base=constrain((int)level,1,7);
  uint32_t t=millis()/95;
  for(int x=2;x<=13;x++) {
    int centreBoost=6-abs(x-7);
    uint8_t wobble=(uint8_t)((x*7+t*3+(x&1)*5)%5);
    uint8_t h=constrain((int)base+centreBoost/2+(int)wobble-1,2,13);
    for(uint8_t n=0;n<h;n++) {
      uint8_t hue=(uint8_t)(185 + x*10 + n*7 + t*2);
      audioPixel(x,14-n,audioRainbow(hue));
    }
  }
}

// FFT 2 - large rainbow heart at rest.  The heart is made of horizontal
// spectrum-like bars: music squeezes/expands each row instead of placing a
// separate equaliser underneath it.
static void renderAudioRainbowHeartFFT(const uint8_t *b) {
  clearFramebuffer();
  uint16_t sum=0;
  for(uint8_t i=0;i<8;i++) sum+=bandHeight(b[i]);
  uint8_t avg=(uint8_t)(sum/8);

  // Full-screen 14x13 heart silhouette expressed as left/right extents.
  static const int8_t left[13] ={3,1,0,0,0,1,1,2,3,4,5,6,7};
  static const int8_t right[13]={6,7,7,7,7,7,6,6,5,4,3,2,1};

  uint32_t phase=millis()/45;
  for(uint8_t y=0;y<13;y++) {
    int cx=7;
    int l=left[y], r=right[y];
    int baseHalf=max(l,r);

    // Each heart row follows one FFT band. Quiet -> full heart; louder values
    // produce visible in/out breathing while keeping the heart recognisable.
    uint8_t band=b[min((uint8_t)7,(uint8_t)(y/2))];
    int squeeze=(band>=8)?2:((band>=4)?1:0);
    if(avg<=2) squeeze=0;
    int half=max(1,baseHalf-squeeze);

    // Top lobes have two centres; lower rows one central bar.
    if(y<=4) {
      int lc=4, rc=11;
      int hw=max(1,3-squeeze/2);
      for(int x=lc-hw;x<=lc+hw;x++) audioPixel(x,y+1,audioRainbow((uint8_t)(x*13+y*12+phase)));
      for(int x=rc-hw;x<=rc+hw;x++) audioPixel(x,y+1,audioRainbow((uint8_t)(x*13+y*12+phase)));
      if(y>=2) for(int x=5+squeeze;x<=10-squeeze;x++) audioPixel(x,y+1,audioRainbow((uint8_t)(x*13+y*12+phase)));
    } else {
      for(int x=cx-half;x<=cx+half+1;x++) audioPixel(x,y+1,audioRainbow((uint8_t)(x*13+y*12+phase)));
    }
  }
}

// FFT 3 - validated horizontal spectrum growing from the vertical centre.
static void renderAudioHorizontalFromCenter(const uint8_t *b) {
  clearFramebuffer();
  for(int y=0;y<16;y++) { audioPixel(7,y,CRGB(40,140,255)); audioPixel(8,y,CRGB(40,140,255)); }
  for(uint8_t y=0;y<16;y++) {
    uint8_t src=(y<8)?y:(15-y);
    uint8_t v=bandHeight(b[src]);
    uint8_t w=(uint8_t)min(7,(int)((v+1)/2));
    for(uint8_t n=1;n<=w;n++) {
      CRGB c=audioRainbow((uint8_t)(y*14+n*9));
      audioPixel(7-n,y,c); audioPixel(8+n,y,c);
    }
  }
}

void renderAudio() {
  if(!audioState.valid) return;

  if (!audioState.fft) {
    switch(audioState.mode) {
      case 0: renderAudioBreakdancer(audioState.level); break;  // LEVEL 1
      case 1: renderAudioHeartLevel(audioState.level); break;   // LEVEL 2
      case 2: renderAudioFakeSpectrum(audioState.level); break; // LEVEL 3
      case 3: renderAudioPurpleFace(audioState.level); break; // LEVEL 4
      default: renderAudioObservedFace(audioState.level); break; // LEVEL 5
    }
  } else {
    switch(audioState.mode) {
      case 0: renderAudioFFT0(audioState.bands); break;              // FFT 1 validated
      case 1: renderAudioFFT1(audioState.bands); break;              // FFT 2 row-coloured
      case 2: renderAudioRainbowHeartFFT(audioState.bands); break;   // FFT 3 full-screen heart
      case 3: renderAudioHorizontalFromCenter(audioState.bands); break; // FFT 4 validated
      default: renderAudioFFT3(audioState.bands); break;             // FFT 5 validated
    }
  }
  scaleLegacy16CanvasToLogical();
  refreshMatrix();
}

bool processAudioPacket(const uint8_t *data, size_t len) {
  // LEVEL family: MODE is 1..5 (last byte), LEVEL is byte 4.
  if (len == 6 && data[0] == 0x06 && data[1] == 0x00 && data[2] == 0x00 && data[3] == 0x02) {
    uint8_t rawMode=data[5];
    if(rawMode<1 || rawMode>5) return false;
    audioState.valid=true;
    audioState.fft=false;
    audioState.mode=rawMode-1;
    audioState.level=(uint8_t)min(12,(int)data[4]);
    audioState.lastPacketMs=millis();
    audioState.packetCounter++;
    switchDisplayMode(DISPLAY_AUDIO);
    renderAudio();
#if DEBUG_SERIAL
    int uiEffect=audioProtocolIndex(false, audioState.mode);
    if(uiEffect!=audioLastUiEffect || millis()-audioLastLogMs>=1000) {
      Serial.print("AUDIO FX="); Serial.print(uiEffect);
      Serial.print(" LEVEL mode="); Serial.print(rawMode);
      Serial.print(" level="); Serial.println(audioState.level);
      audioLastUiEffect=uiEffect; audioLastLogMs=millis();
    }
#endif
    sendCommandAck(0x00,0x02);
    return true;
  }

  // FFT family. A complete wire frame is exactly 21 bytes:
  //   21 00 01 02 <mode> <16 GEQ bands>
  // The legacy iDotMatrix visualizers consume 8 logical bands, produced by
  // averaging adjacent wire-band pairs. The transport layer below guarantees
  // that only one complete 21-byte frame reaches this function.
  if (len == AUDIO_FFT_FRAME_SIZE && data[0] == 0x21 && data[1] == 0x00 && data[2] == 0x01 && data[3] == 0x02) {
    uint8_t mode=data[4];
    if(mode>4) return false;
    audioState.valid=true;
    audioState.fft=true;
    audioState.mode=mode;
    for(uint8_t i=0;i<8;i++) {
      const uint16_t a=data[5U+(uint16_t)i*2U];
      const uint16_t b=data[6U+(uint16_t)i*2U];
      audioState.bands[i]=(uint8_t)min(12U,(unsigned)((a+b+1U)/2U));
    }
    audioState.lastPacketMs=millis();
    audioState.packetCounter++;
    switchDisplayMode(DISPLAY_AUDIO);
    renderAudio();
#if DEBUG_SERIAL
    int uiEffect=audioProtocolIndex(true, mode);
    if(uiEffect!=audioLastUiEffect || millis()-audioLastLogMs>=1000) {
      Serial.print("AUDIO FX="); Serial.print(uiEffect);
      Serial.print(" FFT mode="); Serial.print(mode);
      Serial.print(" bands=");
      for(uint8_t i=0;i<8;i++){ if(i)Serial.print(','); Serial.print(audioState.bands[i]); }
      Serial.println();
      audioLastUiEffect=uiEffect; audioLastLogMs=millis();
    }
#endif
    sendCommandAck(0x01,0x02);
    return true;
  }
  return false;
}

bool handleScheduleCommand(const uint8_t *data, size_t len);

// ======================================================
// COMMAND PROCESSOR
// ======================================================
void processFA02Packet(const uint8_t *data,size_t len){

  if (len < 4) {
    if (displayPin == noDisplayPin || displayPinVerified) processAudioPacket(data, len);
    return;
  }
  const uint8_t cmd = data[2], sub = data[3];
  if (len == 4 && cmd == 0x01 && sub == 0x80) { sendDeviceInfo(); return; }
  if (len == 7 && cmd == 0x05 && sub == 0x02) {
    const uint32_t candidate = decodeDisplayPin(data + 4);
    displayPinVerified = candidate != noDisplayPin && candidate == displayPin;
    sendCommandStatus(cmd, sub, displayPinVerified ? 1 : 0);
    return;
  }
  if (len == 8 && cmd == 0x04 && sub == 0x02) {
    const uint32_t candidate = decodeDisplayPin(data + 5);
    const bool set = data[4] == 1 && (displayPin == noDisplayPin || displayPinVerified);
    const bool clear = data[4] == 0 && candidate == displayPin;
    const bool saved = candidate != noDisplayPin && (set || clear) &&
                       saveDisplayPin(set ? candidate : noDisplayPin);
    if (saved) displayPinVerified = set;
    sendCommandStatus(cmd, sub, saved ? 1 : 0);
    return;
  }
  if (displayPin != noDisplayPin && !displayPinVerified) return;
  if (processAudioPacket(data, len)) return;
  if (processAlarmCommand(data, len)) return;
  if (processBulkPacket(data, len)) return;
  if (processEffectCommand(data, len)) return;
  if(len==11 && cmd==0x01 && sub==0x80){
    uint16_t y=2000+data[4];
    uint8_t mo=data[5], d=data[6], h=data[8], mi=data[9], se=data[10];
    if(isValidDateTime(y,mo,d,h,mi,se)){
      syncYear=y; syncMonth=mo; syncDay=d; syncHour=h; syncMinute=mi; syncSecond=se; syncMillis=monotonicMillis64(); clockSynced=true;
#if IDOTMATRIX_RTC_AVAILABLE && IDOTMATRIX_RTC_SYNC_FROM_BLE
      if(!rtcReady) rtcRetryNextAt=millis();
      if(rtcReady){
        IDotMatrixRtcDateTime rtcValue;
        rtcValue.year=syncYear; rtcValue.month=syncMonth; rtcValue.day=syncDay;
        rtcValue.hour=syncHour; rtcValue.minute=syncMinute; rtcValue.second=syncSecond;
        if (rtc.adjust(rtcValue)) {
          rtcTimeValid=true;
#if DEBUG_SERIAL
          Serial.println("RTC: synchronized from BLE time");
#endif
        } else {
          rtcTimeValid=false;
#if DEBUG_SERIAL
          Serial.print("RTC: BLE synchronization failed: "); Serial.println(rtc.errorText());
#endif
        }
      }
#endif
    } else {
#if DEBUG_SERIAL
      Serial.print("TIME SYNC rejected: "); Serial.print(y); Serial.print('-'); Serial.print(mo); Serial.print('-'); Serial.print(d);
      Serial.print(' '); Serial.print(h); Serial.print(':'); Serial.print(mi); Serial.print(':'); Serial.println(se);
#endif
    }
    // ACK compatibility is preserved even when malformed fields are ignored;
    // negative/error ACK semantics are not yet experimentally established.
#if defined(IDOTMATRIX_BOARD_CLOCKWISE)
    // The Android time-sync callback uses this reply to initialize its canvas size.
    sendDeviceInfo();
#else
    sendCommandAck(0x01,0x80);
#endif
    return;
  }

  if(len==5 && cmd==0x07 && sub==0x01){
    screenOn=data[4]!=0;
    setStatusLed(screenOn); refreshMatrix(); sendCommandAck(cmd,sub); return;
  }
  if(len==5 && cmd==0x06 && sub==0x80){ flipped180=data[4]!=0; refreshMatrix(); sendCommandAck(cmd,sub); return; }

  if(len==10 && cmd==0x02 && sub==0x80){
    bool valid=data[5]<24 && data[6]<60 && data[7]<24 && data[8]<60 && data[9]<=100;
    if(valid){
      energySaving.enabled=data[4]!=0; energySaving.startHour=data[5]; energySaving.startMinute=data[6]; energySaving.endHour=data[7]; energySaving.endMinute=data[8]; energySaving.reductionPercent=data[9];
      refreshMatrix();
    } else {
#if DEBUG_SERIAL
      Serial.println("POWER SAVING rejected: invalid time/reduction fields");
#endif
    }
    // Preserve the compatibility ACK: an original-device error status has not
    // been established for malformed power-saving records.
    sendCommandAck(cmd,sub); return;
  }

  if(len==4 && cmd==0x03 && sub==0x80){ pendingSoftReset=true; softResetAt=millis()+100; return; }

  if(len==8 && cmd==0x0A && sub==0x80){
    scoreA=(uint16_t)data[4]|((uint16_t)data[5]<<8); scoreB=(uint16_t)data[6]|((uint16_t)data[7]<<8);
    switchDisplayMode(DISPLAY_SCOREBOARD); renderScoreboard(); sendCommandAck(cmd,sub); return;
  }

  if(len==5 && cmd==0x04 && sub==0x01){
    const uint8_t mode=data[4];
    if (mode==1) {
      resetGraffitiRaster();
      switchDisplayMode(DISPLAY_GRAFFITI);
      clearFramebuffer();
      refreshMatrix();
    } else if (mode==3) {
      switchDisplayMode(DISPLAY_GRAFFITI);
    }
    diyMode=mode==1 || mode==3;
    sendCommandAck(cmd,sub);
    return;
  }

  if(len==5 && cmd==0x04 && sub==0x80){
    brightnessPercent=min((uint8_t)100,data[4]);
    refreshMatrix();
    scheduleBrightnessSave();
#if DEBUG_SERIAL
    Serial.print("BRIGHTNESS RX: "); Serial.print(brightnessPercent); Serial.println("%");
#endif
    sendCommandAck(cmd,sub);
    return;
  }

  if(len==7 && cmd==0x02 && sub==0x02){ switchDisplayMode(DISPLAY_SOLID); clearFramebuffer(CRGB(data[4],data[5],data[6])); refreshMatrix(); sendCommandAck(cmd,sub); return; }

  if(len==8 && cmd==0x06 && sub==0x01){
    uint8_t flags=data[4];
    const bool incomingShowDate=(flags&0x80)!=0;
    const uint8_t incomingStyle=(uint8_t)(flags&0x3F);
    const uint32_t nowClock=millis();
    const bool enteringClock=(displayMode!=DISPLAY_CLOCK);
    const bool styleChanged=(incomingStyle!=clockStyle);
    if((enteringClock || styleChanged) && clockDatePreference) clockEntryProtectUntil=nowClock+1000UL;
    const bool protectDateOff=clockDatePreference &&
      (enteringClock || styleChanged || (int32_t)(clockEntryProtectUntil-nowClock)>0);
    const uint8_t previousStyle=clockStyle;
    const bool previous24h=clock24h;
    const bool previousShowDate=clockShowDate;
    const CRGB previousColor=clockColor;
    clockStyle=incomingStyle;
    clock24h=(flags&0x40)!=0;
    if(incomingShowDate || !protectDateOff){
      clockShowDate=incomingShowDate;
      clockDatePreference=incomingShowDate;
    } else {
      clockShowDate=true;
    }
    clockColor=CRGB(data[5],data[6],data[7]);
    if(previousStyle!=clockStyle || previous24h!=clock24h || previousShowDate!=clockShowDate ||
       previousColor.r!=clockColor.r || previousColor.g!=clockColor.g || previousColor.b!=clockColor.b){
      scheduleClockPreferencesSave();
    }
#if DEBUG_SERIAL
    Serial.print("CLOCK RAW ["); Serial.print(len); Serial.print("]: "); dumpHex(data,len);
    Serial.print("CLOCK style raw="); Serial.print(clockStyle);
    Serial.print(" case="); Serial.print(clockStyle & 0x07);
    Serial.print(" 24H="); Serial.print(clock24h ? 1 : 0);
    Serial.print(" DATE="); Serial.print(clockShowDate ? 1 : 0);
    Serial.print(" RGB="); Serial.print(data[5]); Serial.print(','); Serial.print(data[6]); Serial.print(','); Serial.println(data[7]);
#endif
    clockCycleStartedAt=millis();
    switchDisplayMode(DISPLAY_CLOCK); renderClock(); sendCommandAck(cmd,sub); return;
  }

  if(len==7 && cmd==0x08 && sub==0x80){
    uint8_t mode=data[4]; uint32_t req=((uint32_t)data[5]*60UL+data[6])*1000UL;
    switch(mode){
      case 0:cancelCountdownFinishBuzzer();countdownRunning=false;countdownPaused=false;countdownRemainingMs=0;countdownFinishSent=false;break;
      case 1:cancelCountdownFinishBuzzer();countdownRemainingMs=req;countdownStartMillis=millis();countdownRunning=true;countdownPaused=false;countdownFinishSent=false;break;
      case 2:if(countdownRunning){uint32_t e=millis()-countdownStartMillis;countdownRemainingMs=(e<countdownRemainingMs)?countdownRemainingMs-e:0;}countdownRunning=false;countdownPaused=true;break;
      case 3:if(countdownRemainingMs){countdownStartMillis=millis();countdownRunning=true;countdownPaused=false;}break;
    }
    switchDisplayMode(DISPLAY_COUNTDOWN); sendCommandAck(cmd,sub); return;
  }

  if(len==5 && cmd==0x09 && sub==0x80){
    uint8_t mode=data[4];
    switch(mode){
      case 0:stopwatchRunning=false;stopwatchElapsedMs=0;break;
      case 1:stopwatchElapsedMs=0;stopwatchStartMillis=millis();stopwatchRunning=true;break;
      case 2:if(stopwatchRunning)stopwatchElapsedMs+=millis()-stopwatchStartMillis;stopwatchRunning=false;break;
      case 3:if(!stopwatchRunning){stopwatchStartMillis=millis();stopwatchRunning=true;}break;
    }
    switchDisplayMode(DISPLAY_STOPWATCH); sendCommandAck(cmd,sub); return;
  }

  if(len>=10 && cmd==0x05 && sub==0x01){
    switchDisplayMode(DISPLAY_GRAFFITI);
    uint8_t r=data[5],g=data[6],b=data[7];
    for(size_t i=8;i+1<len;i+=2){ uint8_t x=data[i],y=data[i+1]; if(x<MATRIX_WIDTH&&y<MATRIX_HEIGHT) framebuffer[logicalIndex(x,y)]=CRGB(r,g,b); }
    refreshMatrix(); return;
  }

  if (handleCarouselCommand(data, len)) return;
  if (handleScheduleCommand(data, len)) return;
  if (handlePresetCommand(data, len)) return;

  // Unknown command: always latch it for the OLED, even when Serial debug is off.
  unknownCommandActive = true;
  unknownCommandAt = millis();
  unknownCommandCount++;
  unknownCommandLen = (uint16_t)min(len, (size_t)0xFFFF);
  unknownCommandStored = (uint8_t)min(len, (size_t)OLED_UNKNOWN_BYTES);
  for (uint8_t i=0; i<unknownCommandStored; i++) unknownCommandData[i]=data[i];
#if DEBUG_SERIAL
  Serial.print("UNHANDLED COMMAND [");Serial.print(len);Serial.print("]: ");dumpHex(data,len);
#endif
  sendCommandAck(cmd,sub);
}

// ======================================================
// BLE CALLBACKS
// ======================================================

static bool startsAudioFrame(const uint8_t *data, size_t len) {
  if(!data || len<4) return false;
  return (data[0]==0x06 && data[1]==0x00 && data[2]==0x00 && data[3]==0x02) ||
         (data[0]==0x21 && data[1]==0x00 && data[2]==0x01 && data[3]==0x02);
}

static size_t audioFrameSizeFromHeader(const uint8_t *data, size_t len) {
  if(!data || len<4) return 0;
  if(data[0]==0x06 && data[1]==0x00 && data[2]==0x00 && data[3]==0x02) return AUDIO_LEVEL_FRAME_SIZE;
  if(data[0]==0x21 && data[1]==0x00 && data[2]==0x01 && data[3]==0x02) return AUDIO_FFT_FRAME_SIZE;
  return 0;
}

// Mirrors the WLED framing guard: when an Audio/Rhythm frame is incomplete,
// a known normal FA02 command must be allowed to take ownership immediately
// instead of being swallowed as audio continuation bytes.
static bool startsKnownNonAudioFrame(const uint8_t *data, size_t len) {
  if(!data || len<4) return false;
  const uint16_t declared=(uint16_t)data[0]|((uint16_t)data[1]<<8);
  if(declared<4 || declared>MAX_PACKET_SIZE) return false;
  const uint8_t command=data[2], subcommand=data[3];

  // Bulk begin/chunk/end envelopes and compact inline media.
  if(subcommand==0x00 && (command==0x01 || command==0x02 || command==0x03)) return true;
  if(command==0x00 && subcommand==0x00) return true;

  // Captured normal FA02 families. Keep this list explicit so an arbitrary
  // FFT continuation is not misclassified just because its first two values
  // happen to form a plausible little-endian length.
  return (command==0x01 && subcommand==0x80) ||
         (command==0x03 && subcommand==0x80) ||
         (command==0x02 && subcommand==0x01) ||
         (command==0x0A && subcommand==0x01) ||
         (command==0x00 && subcommand==0x80) ||
         (command==0x07 && (subcommand==0x80 || subcommand==0x01)) ||
         (command==0x05 && (subcommand==0x80 || subcommand==0x01 || subcommand==0x02)) ||
         (command==0x04 && (subcommand==0x80 || subcommand==0x01 || subcommand==0x02)) ||
         (command==0x02 && (subcommand==0x80 || subcommand==0x02)) ||
         (command==0x03 && subcommand==0x02) ||
         (command==0x06 && (subcommand==0x80 || subcommand==0x01 || subcommand==0x02)) ||
         (command==0x08 && subcommand==0x80) ||
         (command==0x09 && subcommand==0x80) ||
         (command==0x0A && subcommand==0x80);
}

// Normal LE16-length FA02 assembler. Audio/Rhythm never enters this path.
static void processNormalFA02Bytes(const uint8_t *data, size_t len, uint32_t now) {
  size_t offset=0;
  while(offset<len){
    if(packetReceived<2){
      const size_t needHeader=2U-packetReceived;
      const size_t available=len-offset;
      const size_t take=needHeader<available?needHeader:available;
      ::memcpy(packetBuffer+packetReceived,data+offset,take);
      packetReceived+=take;
      offset+=take;
      packetLastRxMs=now;
      if(packetReceived<2) return;

      packetExpected=(uint16_t)packetBuffer[0]|((uint16_t)packetBuffer[1]<<8);
      if(packetExpected<2||packetExpected>MAX_PACKET_SIZE){
#if DEBUG_SERIAL
        Serial.print("PACKET RX invalid declared length="); Serial.println(packetExpected);
#endif
        packetExpected=packetReceived=0; packetLastRxMs=0; resetBulkTransfer(true); return;
      }
    }

    const size_t needed=(size_t)packetExpected-packetReceived;
    const size_t available=len-offset;
    const size_t take=needed<available?needed:available;
    if(packetReceived+take>MAX_PACKET_SIZE){
      packetExpected=packetReceived=0; packetLastRxMs=0; resetBulkTransfer(true); return;
    }
    if(take){
      ::memcpy(packetBuffer+packetReceived,data+offset,take);
      packetReceived+=take;
      offset+=take;
      packetLastRxMs=now;
    }

#if DEBUG_SERIAL && DEBUG_SERIAL_VERBOSE
    if(bulk.active || packetExpected>=512){
      Serial.print("[RXASM] write="); Serial.print(len);
      Serial.print(" consumed="); Serial.print(offset);
      Serial.print(" packet="); Serial.print(packetReceived);
      Serial.print('/'); Serial.println(packetExpected);
    }
#endif

    if(packetReceived<packetExpected) return;

    processFA02Packet(packetBuffer,packetExpected);
    packetReceived=packetExpected=0;
    packetLastRxMs=0;
  }
}

// Consume an Audio/Rhythm byte stream. ATT writes do not define audio-frame
// boundaries: for example, a 33-byte write is one complete 21-byte FFT frame
// plus the first 12 bytes of the next frame. Remainders are therefore retained
// until exactly 6 (LEVEL) or 21 (FFT) bytes are available.
static void processAudioStreamBytes(const uint8_t *data, size_t len, uint32_t now) {
  size_t offset=0;

  while(offset<len){
    // At a clean frame boundary the next bytes must identify a new audio frame.
    if(audioRxReceived==0){
      const size_t remaining=len-offset;
      if(remaining<4){
        ::memcpy(audioRxBuffer,data+offset,remaining);
        audioRxReceived=remaining;
        audioRxExpected=0;
        audioRxLastMs=now;
        return;
      }

      audioRxExpected=audioFrameSizeFromHeader(data+offset,remaining);
      if(!audioRxExpected){
        // A normal command may immediately follow an audio session. Route it
        // back to the normal assembler instead of creating sticky audio state.
        if(startsKnownNonAudioFrame(data+offset,remaining)){
          resetAudioReassembly();
          processNormalFA02Bytes(data+offset,remaining,now);
        } else {
#if DEBUG_SERIAL
          Serial.print("AUDIO RX lost sync at frame boundary, dropping ");
          Serial.print(remaining); Serial.println(" byte(s)");
#endif
          resetAudioReassembly();
        }
        return;
      }
    }

    // If a frame header was split across writes, finish the four-byte prefix
    // first and only then decide whether it is LEVEL or FFT.
    if(audioRxExpected==0){
      const size_t headerNeed=4U-audioRxReceived;
      const size_t available=len-offset;
      const size_t take=headerNeed<available?headerNeed:available;
      ::memcpy(audioRxBuffer+audioRxReceived,data+offset,take);
      audioRxReceived+=take;
      offset+=take;
      audioRxLastMs=now;
      if(audioRxReceived<4) return;
      audioRxExpected=audioFrameSizeFromHeader(audioRxBuffer,audioRxReceived);
      if(!audioRxExpected){
#if DEBUG_SERIAL
        Serial.println("AUDIO RX invalid split frame header; resync");
#endif
        resetAudioReassembly();
        return;
      }
    }

    const size_t needed=audioRxExpected-audioRxReceived;
    const size_t available=len-offset;
    const size_t take=needed<available?needed:available;
    if(take){
      ::memcpy(audioRxBuffer+audioRxReceived,data+offset,take);
      audioRxReceived+=take;
      offset+=take;
      audioRxLastMs=now;
    }
    if(audioRxReceived<audioRxExpected) return;

    const size_t completed=audioRxExpected;
    const bool accepted=processAudioPacket(audioRxBuffer,completed);
#if DEBUG_SERIAL && DEBUG_SERIAL_VERBOSE
    if(!accepted){
      Serial.print("AUDIO RX rejected complete frame size="); Serial.println(completed);
    }
#endif
    audioRxReceived=0;
    audioRxExpected=0;
    audioRxLastMs=0;
    // Continue: the same ATT write may already contain the next audio frame.
  }
}

// Caller must hold runtimeStateMutex: this function mutates reassembly, Bulk and renderer state.
void processFA02Write(const uint8_t *data, size_t len) {
  if(!data || !len) return;
  const uint32_t now=millis();
  expireStalledTransfers(now);
  if(bulk.active) bulkLastRxMs=now;

  // If a normal logical packet is already being reassembled, preserve its
  // ownership. This avoids changing multipart media semantics.
  if(packetReceived){
    processNormalFA02Bytes(data,len,now);
    return;
  }

  const bool audioStart=startsAudioFrame(data,len);
  const bool normalStart=startsKnownNonAudioFrame(data,len);

  // A complete normal command is allowed to break an incomplete audio frame.
  // This is the key recovery path for Audio -> Clock/GIF/Carousel/Reset/etc.
  if(audioRxReceived && normalStart && !audioStart){
#if DEBUG_SERIAL
    Serial.println("AUDIO RX -> normal FA02; dropping partial audio frame");
#endif
    resetAudioReassembly();
    processNormalFA02Bytes(data,len,now);
    return;
  }

  if(audioRxReceived || audioStart){
    if (displayPin != noDisplayPin && !displayPinVerified) return;
    processAudioStreamBytes(data,len,now);
    return;
  }

  // At an audio frame boundary any non-audio write belongs to normal FA02.
  processNormalFA02Bytes(data,len,now);
}

void handleBleConnected(){
#if PNG_DIAG_SERIAL
  Serial.println("BLE C");
#endif
  if(!lockRuntimeState()) return;
  deviceConnected=true;
  displayPinVerified=false;
  // BLE connection must not change display power/state. The boot policy or
  // the latest app command remains authoritative, avoiding ON+black state.
  triggerConnectionBuzzer();
  pendingAdvertisingRestart=false;
#if !defined(IDOTMATRIX_BOARD_CLOCKWISE)
  pendingDeviceInfoPush=true;
  deviceInfoPushAt=millis()+1200;
#endif
  unlockRuntimeState();
}

void handleBleDisconnected(){
#if PNG_DIAG_SERIAL
  Serial.println("BLE D");
#endif
  if(!lockRuntimeState()) return;
  deviceConnected=false;
  displayPinVerified=false;
  // A queued 02/01 has not touched storage yet; if BLE disappears before
  // loopTask publishes it, simply discard the descriptor. Active bank journals
  // are intentionally left on disk for reboot/restart recovery.
  if(deferredCarouselBankSetup.active) deferredCarouselBankSetup=DeferredCarouselBankSetupState();
  packetReceived=packetExpected=0;
  packetLastRxMs=0;
  resetAudioReassembly();
  // If a complete Preset/Carousel is awaiting loopTask publication, preserve
  // its staged file and descriptor across disconnect. The loop can safely
  // finish the already-received transaction; sendFA03() will simply no-op
  // while disconnected. Partial transfers keep the historical abort path.
  if(!deferredAssetCommit.active) resetBulkTransfer(true);
  // Preset candidates are not authoritative until 06/02. If the link
  // disappears before activation, let loopTask finish any already-queued final
  // asset commit and then discard the candidate bank, preserving /preN.*.
  if(presetBankTxnActive) requestPresetBankCleanup(1);
  resetGraffitiRaster();
  // Do not sleep inside the BLE callback. Preserve the historical 300 ms
  // restart delay by deferring advertising restart to the Arduino loop.
  pendingAdvertisingRestart=true;
  advertisingRestartAt=millis()+300;
  unlockRuntimeState();
}

#if IDOTMATRIX_USE_NIMBLE
#if defined(IDOTMATRIX_BOARD_CLOCKWISE)
class DeviceInfoCallbacks : public NimBLECharacteristicCallbacks {
  void onRead(NimBLECharacteristic *characteristic, NimBLEConnInfo&) override {
    characteristic->setValue(deviceInfo, sizeof(deviceInfo));
    Serial.println("APP FA03 device info read");
  }
  void onSubscribe(NimBLECharacteristic*, NimBLEConnInfo&, uint16_t mode) override {
    Serial.printf("APP FA03 subscription=%u\n", mode);
    if (!(mode & 1) || !lockRuntimeState()) return;
    pendingDeviceInfoPush = true;
    deviceInfoPushAt = millis() + 100;
    unlockRuntimeState();
  }
};
static DeviceInfoCallbacks deviceInfoCallbacks;
#endif
class FA02Callbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *c, NimBLEConnInfo&) override {
    const std::string value=c->getValue(); if(value.empty()) return;
#if defined(IDOTMATRIX_BOARD_CLOCKWISE)
    static uint32_t last = 0;
    const uint32_t now = millis();
    if (value.size() <= 24 || now - last >= 250) {
      last = now;
      Serial.printf("APP FA02 len=%u cmd=%02X sub=%02X\n", unsigned(value.size()),
                    value.size() > 2 ? uint8_t(value[2]) : 0,
                    value.size() > 3 ? uint8_t(value[3]) : 0);
    }
#endif
    if(!lockRuntimeState()) return;
    processFA02Write(reinterpret_cast<const uint8_t*>(value.data()),value.size());
    unlockRuntimeState();
  }
};

class AE01Callbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *c, NimBLEConnInfo&) override {
#if defined(IDOTMATRIX_BOARD_CLOCKWISE)
    Serial.printf("APP AE01 write len=%u\n", unsigned(c->getValue().size()));
#elif DEBUG_SERIAL && DEBUG_SERIAL_VERBOSE
    const std::string v=c->getValue(); if(v.empty()) return; Serial.print("RX AE01 [");Serial.print(v.size());Serial.print("]: ");dumpHex(reinterpret_cast<const uint8_t*>(v.data()),v.size());
#else
    (void)c;
#endif
  }
};

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer*, NimBLEConnInfo& info) override {
#if defined(IDOTMATRIX_BOARD_CLOCKWISE)
    Serial.printf("APP BLE connected mtu=%u\n", info.getMTU());
#endif
    handleBleConnected();
  }
  void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int reason) override {
#if defined(IDOTMATRIX_BOARD_CLOCKWISE)
    Serial.printf("APP BLE disconnected reason=%d\n", reason);
#endif
    handleBleDisconnected();
  }
};
#else
class FA02Callbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *c) override {
    String value=c->getValue(); if(!value.length()) return;
    if(!lockRuntimeState()) return;
    processFA02Write((const uint8_t*)value.c_str(),value.length());
    unlockRuntimeState();
  }
};

class AE01Callbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *c) override {
#if DEBUG_SERIAL && DEBUG_SERIAL_VERBOSE
    String v=c->getValue(); if(!v.length()) return; Serial.print("RX AE01 [");Serial.print(v.length());Serial.print("]: ");dumpHex((const uint8_t*)v.c_str(),v.length());
#else
    (void)c;
#endif
  }
};

class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer*) override { handleBleConnected(); }
  void onDisconnect(BLEServer*) override { handleBleDisconnected(); }
};
#endif

static FA02Callbacks fa02CallbacksInstance;
static AE01Callbacks ae01CallbacksInstance;
static ServerCallbacks serverCallbacksInstance;

// ======================================================
// OTA
// ======================================================

// ======================================================
// RESET RUNTIME
// ======================================================
void resetRuntimeState(){
  if (displayPin != noDisplayPin && !saveDisplayPin(noDisplayPin)) {
    sendCommandStatus(0x03, 0x80, 0);
    return;
  }
  displayPinVerified = false;
  // An app-issued reset clears emulator-managed persistent device
  // state but is not treated as an electrical power cycle. BLE remains up,
  // the already synchronized software clock is preserved, and the logical
  // screen remains ON with a black framebuffer ready for the next command.
  int8_t queuedGifSlot=pendingGifSlot;
  carouselEnterRequested=false; carouselUploadOpen=false; carouselUploadBlackout=false; carouselStartPending=false;
  carouselActive=false; carouselActiveSlot=-1;
  freeGIF();
  if(littleFsReady && queuedGifSlot>=0 && queuedGifSlot<=1) LittleFS.remove(GIF_RX_FILES[(uint8_t)queuedGifSlot]);
  alarmActive=false; activeAlarmSlot=0xFF; alarmEndsAt=0;
  scheduleActiveIndex=-1; scheduleFailedIndex=-1;
  diyMode=false; effectState.valid=false; textState.valid=false; audioState.valid=false;
  resetAudioReassembly();
  resetGraffitiRaster();
  cancelCountdownFinishBuzzer(); cancelScheduleStartBuzzer(); cancelConnectionBuzzer(); countdownRunning=false; countdownPaused=false; countdownRemainingMs=0; countdownFinishSent=false;
  stopwatchRunning=false; stopwatchElapsedMs=0; scoreA=scoreB=0;
  displayMode=DISPLAY_NONE;
  clearFramebuffer();
  clearPersistentDeviceState();
  screenOn=true;
  setStatusLed(true);
  clearFramebuffer();
  refreshMatrix();
#if DEBUG_SERIAL
  Serial.println("DEVICE RESET: runtime ready, screen ON/black, volatile time preserved");
#endif
  sendCommandAck(0x03, 0x80);
}

// ======================================================
// SETUP
// ======================================================


// -----------------------------------------------------------------------------
// PROGRAM / SCHEDULE
// The app stores the Programs; the device stores only the active Schedule.
// 07 80 -> global state (bit0 enabled, bit1 sound), ACK=01
// 05 80 -> activity media chunk: ACK=01 while incomplete, ACK=03 on final CRC-valid chunk
// A new list is received in staging and committed after a short timeout.
// -----------------------------------------------------------------------------
struct ScheduleActivity {
  bool configured = false;
  uint8_t flags = 0;            // bit0 enabled, bit1..7 lun..dom
  uint8_t startHour = 0;
  uint8_t startMinute = 0;
  uint8_t endHour = 0;
  uint8_t endMinute = 0;
  uint16_t contentType = 0;
  uint32_t mediaSize = 0;
  uint32_t mediaCRC = 0;
  uint16_t reserved = 0;
  uint8_t mediaId = 0;
};

ScheduleActivity scheduleActivities[SCHEDULE_MAX_ACTIVITIES];
ScheduleActivity scheduleStaging[SCHEDULE_MAX_ACTIVITIES];
Preferences schedulePrefs;
uint8_t scheduleGlobalFlags = 0;
uint8_t scheduleStagingGlobalFlags = 0;
bool scheduleUploadOpen = false;
bool scheduleUploadDirty = false;
uint32_t scheduleLastRxMs = 0;
uint32_t scheduleReceivedMask = 0;
int8_t scheduleActiveIndex = -1;
int8_t scheduleFailedIndex = -1;  // Prevent continuous retries when media cannot be decoded.
DisplayMode schedulePreviousMode = DISPLAY_CLOCK;
bool schedulePreviousCarousel = false;
int8_t schedulePreviousCarouselSlot = -1;
CRGB *scheduleSavedFrame = nullptr;

// Hardware logs decoded the byte that appeared to change the
// Schedule media type from 1 to 513. The field at offset 10 is the one-byte
// content type. Offset 11 is a per-chunk marker: observed 0x00 on the first
// chunk and 0x02 on continuation chunks. It is framing metadata, not part of
// contentType and therefore must never reset staging. The complete media object
// remains identified by contentType + mediaSize + mediaCRC.
struct ScheduleMediaUploadState {
  bool active = false;
  ScheduleActivity candidate;
  uint32_t received = 0;
  uint32_t runningCRC = 0xFFFFFFFFUL;
  uint32_t lastChunkAt = 0;
};

ScheduleMediaUploadState scheduleMediaUploads[SCHEDULE_MAX_ACTIVITIES];

bool scheduleMediaObjectMatches(const ScheduleActivity &a, const ScheduleActivity &b) {
  // For a multi-packet activity the media object itself is identified by type,
  // declared complete size and complete-object CRC. Hardware captures proved that one
  // or more ancillary header fields can change between chunks, so they must
  // not be used to decide whether the payload is a continuation.
  return a.contentType == b.contentType &&
         a.mediaSize == b.mediaSize &&
         a.mediaCRC == b.mediaCRC;
}

bool scheduleActivityHeaderMetadataMatches(const ScheduleActivity &a, const ScheduleActivity &b) {
  return a.flags == b.flags &&
         a.startHour == b.startHour && a.startMinute == b.startMinute &&
         a.endHour == b.endHour && a.endMinute == b.endMinute &&
         a.reserved == b.reserved &&
         a.mediaId == b.mediaId;
}

static inline uint16_t rd16le(const uint8_t *p) {
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
static inline uint32_t rd32le(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

String scheduleFileName(uint8_t idx) { return String("/sch") + idx + ".bin"; }
String scheduleTempFileName(uint8_t idx) { return String("/scht") + idx + ".bin"; }
String scheduleBackupFileName(uint8_t idx) { return String("/schb") + idx + ".bin"; }

bool saveScheduleGlobalValue(uint8_t flags) {
  return schedulePrefs.putUChar("flags", flags) == 1;
}

bool saveScheduleGlobal() {
  return saveScheduleGlobalValue(scheduleGlobalFlags);
}

bool saveScheduleMetaValue(uint8_t idx, const ScheduleActivity &value) {
  if (idx >= SCHEDULE_MAX_ACTIVITIES) return false;
  char key[10]; snprintf(key, sizeof(key), "s%u", idx);
  return schedulePrefs.putBytes(key, &value, sizeof(ScheduleActivity)) == sizeof(ScheduleActivity);
}

bool saveScheduleMeta(uint8_t idx) {
  return idx < SCHEDULE_MAX_ACTIVITIES && saveScheduleMetaValue(idx, scheduleActivities[idx]);
}

bool removeScheduleMetaValue(uint8_t idx) {
  if (idx >= SCHEDULE_MAX_ACTIVITIES) return false;
  char key[10]; snprintf(key, sizeof(key), "s%u", idx);
  if (!schedulePrefs.isKey(key)) return true;
  return schedulePrefs.remove(key);
}

void cleanupScheduleTempFiles() {
  if(!littleFsReady) return;
  for (uint8_t i=0;i<SCHEDULE_MAX_ACTIVITIES;i++) LittleFS.remove(scheduleTempFileName(i));
}

void cleanupScheduleBackupFiles() {
  if(!littleFsReady) return;
  for (uint8_t i=0;i<SCHEDULE_MAX_ACTIVITIES;i++) LittleFS.remove(scheduleBackupFileName(i));
}

void cleanupScheduleTransactionFiles() {
  cleanupScheduleTempFiles();
  cleanupScheduleBackupFiles();
}

void recoverScheduleFile(uint8_t idx) {
  if (!littleFsReady || idx >= SCHEDULE_MAX_ACTIVITIES) return;
  String dst=scheduleFileName(idx), tmp=scheduleTempFileName(idx), bak=scheduleBackupFileName(idx);
  LittleFS.remove(tmp);

  const ScheduleActivity &a=scheduleActivities[idx];
  if (!a.configured || !a.mediaSize) {
    LittleFS.remove(dst);
    LittleFS.remove(bak);
    return;
  }

  bool dstOK=fileMatchesMedia(dst,a.mediaSize,a.mediaCRC);
  bool bakOK=fileMatchesMedia(bak,a.mediaSize,a.mediaCRC);
  if(dstOK){
    LittleFS.remove(bak);
  } else if(bakOK){
    LittleFS.remove(dst);
    if(!LittleFS.rename(bak,dst)){
#if DEBUG_SERIAL
      Serial.print("SCH RECOVERY rename failed i="); Serial.println(idx);
#endif
    }
  }
}

void loadSchedule() {
  schedulePrefs.begin("idot-sched", false);
  scheduleGlobalFlags = schedulePrefs.getUChar("flags", 0);
  scheduleStagingGlobalFlags = scheduleGlobalFlags;
  uint8_t count = 0;
  for (uint8_t i = 0; i < SCHEDULE_MAX_ACTIVITIES; i++) {
    char key[10]; snprintf(key, sizeof(key), "s%u", i);
    size_t n = schedulePrefs.getBytesLength(key);
    if (n == sizeof(ScheduleActivity)) {
      schedulePrefs.getBytes(key, &scheduleActivities[i], sizeof(ScheduleActivity));
      if (scheduleActivities[i].configured) count++;
    }
    recoverScheduleFile(i);
  }

  Serial.print("SCH LOAD "); Serial.print(scheduleGlobalFlags, HEX); Serial.print("/"); Serial.println(count);
}

bool scheduleHasActiveMediaUpload() {
  for (uint8_t i=0; i<SCHEDULE_MAX_ACTIVITIES; ++i)
    if (scheduleMediaUploads[i].active) return true;
  return false;
}

void clearScheduleMediaUploadStates() {
  for (uint8_t i=0; i<SCHEDULE_MAX_ACTIVITIES; ++i)
    scheduleMediaUploads[i] = ScheduleMediaUploadState();
}

void resetScheduleMediaUpload(uint8_t idx, bool removeTemp) {
  if (idx >= SCHEDULE_MAX_ACTIVITIES) return;
  if (removeTemp && littleFsReady) LittleFS.remove(scheduleTempFileName(idx));
  scheduleMediaUploads[idx] = ScheduleMediaUploadState();
}

void abortScheduleUploadTransaction(const char *reason) {
#if DEBUG_SERIAL
  if (reason) {
    Serial.print("SCH UPLOAD ABORT reason="); Serial.println(reason);
  }
#endif
  if (littleFsReady) cleanupScheduleTempFiles();
  clearScheduleMediaUploadStates();
  for (uint8_t i=0; i<SCHEDULE_MAX_ACTIVITIES; ++i)
    scheduleStaging[i] = ScheduleActivity();
  scheduleReceivedMask = 0;
  scheduleUploadOpen = false;
  scheduleUploadDirty = false;
}

bool beginScheduleMediaUpload(uint8_t idx, const ScheduleActivity &candidate) {
  if (!littleFsReady || idx >= SCHEDULE_MAX_ACTIVITIES || !candidate.mediaSize) return false;
  resetScheduleMediaUpload(idx, true);
  File f = LittleFS.open(scheduleTempFileName(idx), "w");
  if (!f) return false;
  f.close();
  ScheduleMediaUploadState &u = scheduleMediaUploads[idx];
  u.active = true;
  u.candidate = candidate;
  u.received = 0;
  u.runningCRC = 0xFFFFFFFFUL;
  u.lastChunkAt = millis();
  return true;
}

void updateScheduleMediaUploadTimeout() {
  uint32_t now = millis();
  for (uint8_t i=0; i<SCHEDULE_MAX_ACTIVITIES; ++i) {
    const ScheduleMediaUploadState &u = scheduleMediaUploads[i];
    if (u.active && (uint32_t)(now-u.lastChunkAt) > SCHEDULE_MEDIA_UPLOAD_TIMEOUT_MS) {
      abortScheduleUploadTransaction("media timeout");
      return;
    }
  }
}

void beginScheduleUpload(uint8_t flags) {
  scheduleStagingGlobalFlags = flags;
  scheduleUploadOpen = true;
  scheduleUploadDirty = false;
  scheduleFailedIndex = -1;
  scheduleReceivedMask = 0;
  scheduleLastRxMs = millis();
  clearScheduleMediaUploadStates();
  for (uint8_t i=0;i<SCHEDULE_MAX_ACTIVITIES;i++) {
    recoverScheduleFile(i);
    scheduleStaging[i] = ScheduleActivity();
  }

  Serial.print("SCH OPEN "); Serial.println(flags, HEX);
}

bool rollbackScheduleFilesystem(uint32_t backedUpMask, uint32_t promotedMask) {
  bool ok=true;
  for(uint8_t i=0;i<SCHEDULE_MAX_ACTIVITIES;i++){
    uint32_t bit=(1UL<<i);
    if(promotedMask & bit) LittleFS.remove(scheduleFileName(i));
  }
  for(uint8_t i=0;i<SCHEDULE_MAX_ACTIVITIES;i++){
    uint32_t bit=(1UL<<i);
    if(!(backedUpMask & bit)) continue;
    String bak=scheduleBackupFileName(i), dst=scheduleFileName(i);
    LittleFS.remove(dst);
    if(LittleFS.exists(bak) && !LittleFS.rename(bak,dst)) ok=false;
  }
  return ok;
}

void restoreSchedulePreferences(uint8_t oldFlags) {
  saveScheduleGlobalValue(oldFlags);
  for(uint8_t i=0;i<SCHEDULE_MAX_ACTIVITIES;i++){
    if(scheduleActivities[i].configured) saveScheduleMetaValue(i,scheduleActivities[i]);
    else removeScheduleMetaValue(i);
  }
}

bool commitScheduleUpload() {
  if (!littleFsReady) {
    scheduleUploadOpen=false; scheduleUploadDirty=false;
    return false;
  }
  if (!scheduleUploadOpen || !scheduleUploadDirty) {
    scheduleUploadOpen = false;
    return false;
  }
  // Never publish a partial list while one activity media object is still
  // being assembled from multiple logical packets.
  if (scheduleHasActiveMediaUpload()) return false;

  // Preflight every staged media file before touching the active Schedule.
  for(uint8_t i=0;i<SCHEDULE_MAX_ACTIVITIES;i++){
    if(!(scheduleReceivedMask & (1UL<<i))) continue;
    const ScheduleActivity &a=scheduleStaging[i];
    if(!a.configured || !fileMatchesMedia(scheduleTempFileName(i),a.mediaSize,a.mediaCRC)){
#if DEBUG_SERIAL
      Serial.print("SCH COMMIT preflight failed i="); Serial.println(i);
#endif
      scheduleUploadOpen=false; scheduleUploadDirty=false;
      cleanupScheduleTransactionFiles();
      clearScheduleMediaUploadStates();
      return false;
    }
  }

  uint32_t backedUpMask=0, promotedMask=0;
  bool fsOK=true;

  // Preserve every old destination first. Entries omitted by the new upload
  // remain only in backup until the new metadata has committed successfully.
  for(uint8_t i=0;i<SCHEDULE_MAX_ACTIVITIES && fsOK;i++){
    String dst=scheduleFileName(i), bak=scheduleBackupFileName(i);
    LittleFS.remove(bak);
    if(LittleFS.exists(dst)){
      if(LittleFS.rename(dst,bak)) backedUpMask|=(1UL<<i);
      else fsOK=false;
    }
  }

  for(uint8_t i=0;i<SCHEDULE_MAX_ACTIVITIES && fsOK;i++){
    uint32_t bit=(1UL<<i);
    if(!(scheduleReceivedMask & bit)) continue;
    if(LittleFS.rename(scheduleTempFileName(i),scheduleFileName(i))) promotedMask|=bit;
    else fsOK=false;
  }

  if(!fsOK){
    bool rollbackOK=rollbackScheduleFilesystem(backedUpMask,promotedMask);
    scheduleUploadOpen=false; scheduleUploadDirty=false;
    cleanupScheduleTempFiles();
    clearScheduleMediaUploadStates();
    if(rollbackOK) cleanupScheduleBackupFiles();
#if DEBUG_SERIAL
    Serial.println("SCH COMMIT filesystem rollback");
#endif
    return false;
  }

  // NVS is updated only after all media files are in place. Runtime state is
  // published last; on a preference failure the previous files/metadata are
  // restored best-effort and the active Schedule remains unchanged.
  uint8_t oldFlags=scheduleGlobalFlags;
  bool prefsOK=saveScheduleGlobalValue(scheduleStagingGlobalFlags);
  for(uint8_t i=0;i<SCHEDULE_MAX_ACTIVITIES && prefsOK;i++){
    if(scheduleReceivedMask & (1UL<<i)) prefsOK=saveScheduleMetaValue(i,scheduleStaging[i]);
    else prefsOK=removeScheduleMetaValue(i);
  }

  if(!prefsOK){
    restoreSchedulePreferences(oldFlags);
    bool rollbackOK=rollbackScheduleFilesystem(backedUpMask,promotedMask);
    scheduleUploadOpen=false; scheduleUploadDirty=false;
    cleanupScheduleTempFiles();
    clearScheduleMediaUploadStates();
    if(rollbackOK) cleanupScheduleBackupFiles();
#if DEBUG_SERIAL
    Serial.println("SCH COMMIT preference rollback");
#endif
    return false;
  }

  if(scheduleActiveIndex>=0) stopScheduleActivity();
  scheduleGlobalFlags=scheduleStagingGlobalFlags;
  for(uint8_t i=0;i<SCHEDULE_MAX_ACTIVITIES;i++){
    if(scheduleReceivedMask & (1UL<<i)) scheduleActivities[i]=scheduleStaging[i];
    else scheduleActivities[i]=ScheduleActivity();
    LittleFS.remove(scheduleBackupFileName(i));
    LittleFS.remove(scheduleTempFileName(i));
  }
  scheduleUploadOpen=false;
  scheduleUploadDirty=false;
  scheduleFailedIndex=-1;
  clearScheduleMediaUploadStates();

  Serial.print("SCH COMMIT "); Serial.println(scheduleReceivedMask, HEX);
  return true;
}

bool scheduleTimeInside(const ScheduleActivity &a, uint16_t nowMin) {
  uint16_t s = (uint16_t)a.startHour * 60U + a.startMinute;
  uint16_t e = (uint16_t)a.endHour * 60U + a.endMinute;
  if (s == e) return true;                 // 24h
  if (s < e) return nowMin >= s && nowMin < e;
  return nowMin >= s || nowMin < e;        // crosses midnight
}

static inline uint32_t pngBE32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static inline uint8_t pngPaeth(uint8_t a, uint8_t b, uint8_t c) {
  int p = (int)a + (int)b - (int)c;
  int pa = abs(p - (int)a), pb = abs(p - (int)b), pc = abs(p - (int)c);
  if (pa <= pb && pa <= pc) return a;
  if (pb <= pc) return b;
  return c;
}

// Small PNG decoder for app-supplied Schedule images. The expected image
// dimensions follow the selected logical iDotMatrix profile. Supports 8-bit,
// non-interlaced RGB (type 2) and RGBA (type 6).
// DEFLATE decompression is provided by the ESP32 miniz implementation.
bool decodeSchedulePNG(File &f, uint32_t fileSize) {
#if PNG_DIAG_SERIAL
  Serial.print("P0 n="); Serial.print(fileSize);
  Serial.print(" h="); Serial.println(ESP.getFreeHeap());
#endif
  if (!f || fileSize < 33 || fileSize > 65536UL) { PDBGLN("PF0"); return false; }

  uint8_t *png = (uint8_t*)malloc(fileSize);
  if (!png) { PDBGLN("PF1"); return false; }
  size_t got = f.read(png, fileSize);
  if (got != fileSize) { free(png); PDBGLN("PF2"); return false; }

  static const uint8_t sig[8] = {0x89,'P','N','G',0x0D,0x0A,0x1A,0x0A};
  if (::memcmp(png, sig, 8) != 0) { free(png); PDBGLN("PF3"); return false; }

  uint32_t w=0, h=0, idatBytes=0;
  uint8_t depth=0, colorType=0, comp=0, filterMethod=0, interlace=0;
  size_t pos=8;
  while (pos + 12 <= fileSize) {
    uint32_t n = pngBE32(png + pos);
    if ((uint64_t)pos + 12ULL + n > fileSize) { free(png); PDBGLN("PF4"); return false; }
    const uint8_t *type = png + pos + 4;
    const uint8_t *data = png + pos + 8;
    if (!::memcmp(type, "IHDR", 4)) {
      if (n != 13) { free(png); PDBGLN("PF5"); return false; }
      w=pngBE32(data); h=pngBE32(data+4); depth=data[8]; colorType=data[9];
      comp=data[10]; filterMethod=data[11]; interlace=data[12];
    } else if (!::memcmp(type, "IDAT", 4)) idatBytes += n;
    else if (!::memcmp(type, "IEND", 4)) break;
    pos += 12 + n;
  }
#if PNG_DIAG_SERIAL
  Serial.print("P1 "); Serial.print(w); Serial.print('x'); Serial.print(h);
  Serial.print(" ct="); Serial.print(colorType); Serial.print(" z="); Serial.println(idatBytes);
#endif
  if (w != MATRIX_WIDTH || h != MATRIX_HEIGHT || depth != 8 || comp != 0 || filterMethod != 0 ||
      interlace != 0 || (colorType != 2 && colorType != 6) || !idatBytes) {
    free(png); PDBGLN("PF6"); return false;
  }

  uint8_t bpp = (colorType == 6) ? 4 : 3;
  const size_t rowBytes = (size_t)w * bpp;
  const size_t rawSize = (rowBytes + 1) * h;
  uint8_t *idat = (uint8_t*)malloc(idatBytes);
  if (!idat) { free(png); PDBGLN("PF7"); return false; }

  pos=8; size_t io=0;
  while (pos + 12 <= fileSize) {
    uint32_t n=pngBE32(png+pos); const uint8_t *type=png+pos+4;
    if ((uint64_t)pos + 12ULL + n > fileSize) break;
    if (!::memcmp(type,"IDAT",4)) { ::memcpy(idat+io,png+pos+8,n); io+=n; }
    if (!::memcmp(type,"IEND",4)) break;
    pos += 12 + n;
  }
  free(png);
  if (io != idatBytes) { free(idat); PDBGLN("PF8"); return false; }

  uint8_t *raw = (uint8_t*)malloc(rawSize);
  if (!raw) { free(idat); PDBGLN("PF9"); return false; }
#if PNG_DIAG_SERIAL
  Serial.print("P2 raw="); Serial.print(rawSize); Serial.print(" h="); Serial.println(ESP.getFreeHeap());
  Serial.println("P3 inflate");
  Serial.flush();
#endif
  // IMPORTANT: do not use tinfl_decompress_mem_to_mem() here. That wrapper
  // creates a large local tinfl_decompressor on the loopTask stack and can
  // trigger the ESP32 stack-canary failure seen during testing. Keep the
  // inflater state on the heap and call the low-level API instead.
  tinfl_decompressor *infl = (tinfl_decompressor*)malloc(sizeof(tinfl_decompressor));
  if (!infl) { free(idat); free(raw); PDBGLN("PF10A"); return false; }
  tinfl_init(infl);
  size_t inBytes = idatBytes;
  size_t outBytes = rawSize;
  tinfl_status inflateStatus = tinfl_decompress(
      infl,
      idat, &inBytes,
      raw, raw, &outBytes,
      TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
  free(infl);
  free(idat);
#if PNG_DIAG_SERIAL
  Serial.print("P4 st="); Serial.print((int)inflateStatus);
  Serial.print(" in="); Serial.print(inBytes);
  Serial.print(" out="); Serial.print(outBytes);
  Serial.print(" h="); Serial.print(ESP.getFreeHeap());
  Serial.print(" stk="); Serial.println(uxTaskGetStackHighWaterMark(nullptr));
  Serial.flush();
#endif
  if (inflateStatus != TINFL_STATUS_DONE || outBytes != rawSize) {
    free(raw); PDBGLN("PF10"); return false;
  }

  for (uint8_t y=0; y<h; y++) {
    uint8_t *row = raw + (size_t)y * (rowBytes + 1);
    uint8_t ft = row[0];
    uint8_t *cur = row + 1;
    uint8_t *prev = y ? (raw + (size_t)(y-1) * (rowBytes + 1) + 1) : nullptr;
    if (ft > 4) { free(raw); PDBGLN("PF11"); return false; }
    for (size_t x=0; x<rowBytes; x++) {
      uint8_t left = (x >= bpp) ? cur[x-bpp] : 0;
      uint8_t up = prev ? prev[x] : 0;
      uint8_t ul = (prev && x >= bpp) ? prev[x-bpp] : 0;
      switch (ft) {
        case 0: break;
        case 1: cur[x] = (uint8_t)(cur[x] + left); break;
        case 2: cur[x] = (uint8_t)(cur[x] + up); break;
        case 3: cur[x] = (uint8_t)(cur[x] + ((uint16_t)left + up)/2U); break;
        case 4: cur[x] = (uint8_t)(cur[x] + pngPaeth(left,up,ul)); break;
      }
    }
  }
  PDBGLN("P5 filter");

  clearFramebuffer();
  for (uint8_t y=0; y<MATRIX_HEIGHT; y++) {
    const uint8_t *row = raw + (size_t)y * (rowBytes + 1) + 1;
    for (uint8_t x=0; x<MATRIX_WIDTH; x++) {
      const uint8_t *p = row + (size_t)x * bpp;
      uint8_t r=p[0], g=p[1], b=p[2];
      if (bpp == 4) {
        uint8_t a=p[3];
        r=(uint8_t)(((uint16_t)r*a + 127U)/255U);
        g=(uint8_t)(((uint16_t)g*a + 127U)/255U);
        b=(uint8_t)(((uint16_t)b*a + 127U)/255U);
      }
      framebuffer[logicalIndex(x,y)] = CRGB(r,g,b);
    }
  }
  free(raw);
  PDBGLN("P6 show");
  displayMode = DISPLAY_RAW;
  refreshMatrix();
  PDBGLN("P7 done");
  return true;
}

bool loadScheduleMedia(uint8_t idx) {
  if (!littleFsReady || idx >= SCHEDULE_MAX_ACTIVITIES) return false;
  ScheduleActivity &a = scheduleActivities[idx];
  if (!a.configured || !a.mediaSize) return false;
  File f = LittleFS.open(scheduleFileName(idx), "r");
  if (!f || (uint32_t)f.size() != a.mediaSize) { if(f)f.close(); return false; }

  if (a.contentType == SCHEDULE_CONTENT_GIF) {
    f.close();
    return startStoredGIFPlayback(scheduleFileName(idx), a.mediaSize, a.mediaCRC);
  }

  if (a.contentType == SCHEDULE_CONTENT_TEXT) {
    if (a.mediaSize > MAX_TEXT_PAYLOAD) { f.close(); return false; }
    size_t got = f.read(textPayload, a.mediaSize); f.close();
    if (got != a.mediaSize) return false;
    parseTextPayload(textPayload, got);
    return true;
  }

  if (a.contentType == SCHEDULE_CONTENT_IMAGE) {
#if IDOTMATRIX_MEMORY_TELEMETRY
    idotMemoryTelemetrySnapshot("schedule.png.before_decode");
    const uint32_t pngTelemetryStartedAtUs = micros();
#endif
    bool ok = decodeSchedulePNG(f, a.mediaSize);
    f.close();
#if IDOTMATRIX_MEMORY_TELEMETRY
    idotMemoryTelemetryLatency(ok ? "schedule.png.decode_us" : "schedule.png.decode_failed_us",
                               (uint32_t)(micros() - pngTelemetryStartedAtUs));
    idotMemoryTelemetrySnapshot(ok ? "schedule.png.after_decode" : "schedule.png.decode_failed");
#endif
  #if PNG_DIAG_SERIAL
    Serial.print("PR i="); Serial.print(idx); Serial.print(" ok="); Serial.println(ok ? 1 : 0);
#endif
    return ok;
  }

  f.close();
  return false;
}

void stopScheduleActivity() {
  if (scheduleActiveIndex < 0) return;
  scheduleActiveIndex = -1;
  stopGIFPlayback();
  if (schedulePreviousCarousel && nextCarouselSlot(-1) >= 0) {
    carouselEnterRequested = true;
    int8_t slot = schedulePreviousCarouselSlot;
    if (slot < 0 || slot >= CAROUSEL_SLOT_COUNT || !carouselSlots[(uint8_t)slot].configured)
      slot = nextCarouselSlot(-1);
    if (slot >= 0) startCarouselSlot((uint8_t)slot);
    schedulePreviousCarousel = false;
    schedulePreviousCarouselSlot = -1;
    return;
  }
  schedulePreviousCarousel = false;
  schedulePreviousCarouselSlot = -1;
  if (schedulePreviousMode == DISPLAY_SOLID || schedulePreviousMode == DISPLAY_RAW ||
      schedulePreviousMode == DISPLAY_GRAFFITI) {
    ::memcpy(framebuffer, scheduleSavedFrame, LOGICAL_FRAME_BYTES);
    displayMode = schedulePreviousMode;
    refreshMatrix();
  } else if (hasValidTimeSource()) {
    displayMode = DISPLAY_CLOCK;
    renderClock();
  } else {
    displayMode = DISPLAY_NONE;
    clearFramebuffer(); refreshMatrix();
  }

#if PNG_DIAG_SERIAL
  Serial.println("S-");
#endif
}

void startScheduleActivity(uint8_t idx) {
  if (idx >= SCHEDULE_MAX_ACTIVITIES || alarmActive) return;
  if (scheduleActiveIndex == idx) return;
  if (scheduleActiveIndex >= 0) stopScheduleActivity();
  if(presetActive) stopPresetPlayback();
  schedulePreviousMode = displayMode;
  schedulePreviousCarousel = carouselActive || gifCarouselPlaybackFileActive;
  schedulePreviousCarouselSlot = carouselActiveSlot;
  ::memcpy(scheduleSavedFrame, framebuffer, LOGICAL_FRAME_BYTES);
  if (loadScheduleMedia(idx)) {
    scheduleActiveIndex = idx;
    scheduleFailedIndex = -1;
    if (scheduleGlobalFlags & 0x02) triggerScheduleStartBuzzer();
#if DEBUG_SERIAL
    Serial.print("SCH START i="); Serial.print(idx);
    Serial.print(" type="); Serial.print(scheduleActivities[idx].contentType);
    Serial.print(" window="); Serial.print(scheduleActivities[idx].startHour); Serial.print(':'); Serial.print(scheduleActivities[idx].startMinute);
    Serial.print('-'); Serial.print(scheduleActivities[idx].endHour); Serial.print(':'); Serial.println(scheduleActivities[idx].endMinute);
#endif
#if PNG_DIAG_SERIAL
    Serial.print("S+"); Serial.println(idx);
#endif
  } else {
    // Without this latch updateSchedule() would retry the decode on every loop
    // iteration, which can saturate CPU/heap and make the device appear locked.
    scheduleFailedIndex = (int8_t)idx;
#if DEBUG_SERIAL
    Serial.print("SCH START FAIL i="); Serial.print(idx);
    Serial.print(" type="); Serial.println(scheduleActivities[idx].contentType);
#endif
#if PNG_DIAG_SERIAL
    Serial.print("S!"); Serial.println(idx);
#endif
  }
}

void updateSchedule() {
  uint32_t nowMs = millis();
  updateScheduleMediaUploadTimeout();
  if (scheduleUploadOpen && scheduleUploadDirty && !scheduleHasActiveMediaUpload() &&
      (uint32_t)(nowMs - scheduleLastRxMs) >= SCHEDULE_COMMIT_DELAY_MS) {
    commitScheduleUpload();
  }
  if (!(scheduleGlobalFlags & 0x01) || alarmActive) {
    if (scheduleActiveIndex >= 0) stopScheduleActivity();
    return;
  }
  if (!hasValidTimeSource()) return;

  uint16_t y; uint8_t mo,d,h,mi,se;
  getAlarmDateTime(y,mo,d,h,mi,se);
  uint8_t dayBit = currentWeekdayBit(y,mo,d);
  uint16_t nowMin = (uint16_t)h * 60U + mi;
  int8_t wanted = -1;
  for (uint8_t i=0;i<SCHEDULE_MAX_ACTIVITIES;i++) {
    ScheduleActivity &a=scheduleActivities[i];
    if (!a.configured || !(a.flags & 0x01)) continue;
    if (!(a.flags & dayBit)) continue;
    if (scheduleTimeInside(a, nowMin)) { wanted=(int8_t)i; break; }
  }
  if (wanted < 0) {
    scheduleFailedIndex = -1;
    if (scheduleActiveIndex >= 0) stopScheduleActivity();
  } else if (scheduleActiveIndex != wanted && scheduleFailedIndex != wanted) {
    startScheduleActivity((uint8_t)wanted);
  }
}

bool handleScheduleCommand(const uint8_t *data, size_t len) {
  if (!data || len < 5) return false;

  // Global state: ACK 01.
  if (len == 5 && data[0] == 0x05 && data[1] == 0x00 &&
      data[2] == 0x07 && data[3] == 0x80) {
    uint8_t flags = data[4];
    if (!(flags & 0x01)) {
      scheduleUploadOpen = false;
      scheduleUploadDirty = false;
      scheduleReceivedMask = 0;
      cleanupScheduleTempFiles();
      clearScheduleMediaUploadStates();
      scheduleGlobalFlags = flags;
      scheduleStagingGlobalFlags = flags;
      bool persisted=saveScheduleGlobal();
      scheduleFailedIndex = -1;
      if (scheduleActiveIndex >= 0) stopScheduleActivity();

      Serial.print("SCH OFF f="); Serial.print(flags, HEX); Serial.print(" nvs="); Serial.println(persisted?1:0);
    } else {
      beginScheduleUpload(flags);
    }
    uint8_t ack[5] = {0x05,0x00,0x07,0x80,0x01};
    sendFA03(ack, sizeof(ack));
    return true;
  }

  // Activity media. Small/legacy activities still complete in one logical
  // packet. Large 64x64 activities repeat a 23-byte header for each logical
  // packet; mediaSize/mediaCRC describe the complete object and bytes from
  // offset 23 onward are the current chunk. Hardware validation
  // established ACK 01 as the continuation request for an incomplete object
  // and ACK 03 as the terminal ACK for the final CRC-valid chunk.
  if (len >= 23 && data[2] == 0x05 && data[3] == 0x80) {
    const uint16_t declared = rd16le(data);
    const uint8_t idx = data[4];
    const uint8_t chunkMarker = data[11];
    bool ok = declared == len && idx < SCHEDULE_MAX_ACTIVITIES && littleFsReady;
    ScheduleActivity candidate;
    size_t chunkSize = len - 23;

    if (ok) {
      candidate.configured = true;
      candidate.flags = data[5];
      candidate.startHour = data[6]; candidate.startMinute = data[7];
      candidate.endHour = data[8]; candidate.endMinute = data[9];
      candidate.contentType = data[10];
      candidate.mediaSize = rd32le(data + 12);
      candidate.mediaCRC = rd32le(data + 16);
      candidate.reserved = rd16le(data + 20);
      candidate.mediaId = data[22];
      ok = candidate.startHour < 24 && candidate.endHour < 24 &&
           candidate.startMinute < 60 && candidate.endMinute < 60 &&
           candidate.mediaSize > 0 && chunkSize > 0 &&
           (uint64_t)chunkSize <= (uint64_t)candidate.mediaSize;
    }

    if (ok) {
      const uint32_t bit = (1UL << idx);
      ScheduleMediaUploadState &u = scheduleMediaUploads[idx];

#if DEBUG_SERIAL
      Serial.print("SCH HDR i="); Serial.print(idx);
      Serial.print(" f=0x"); Serial.print(candidate.flags, HEX);
      Serial.print(" "); Serial.print(candidate.startHour); Serial.print(':'); Serial.print(candidate.startMinute);
      Serial.print("-"); Serial.print(candidate.endHour); Serial.print(':'); Serial.print(candidate.endMinute);
      Serial.print(" t="); Serial.print(candidate.contentType);
      Serial.print(" m=0x"); Serial.print(chunkMarker, HEX);
      Serial.print(" n="); Serial.print(candidate.mediaSize);
      Serial.print(" crc=0x"); Serial.print(candidate.mediaCRC, HEX);
      Serial.print(" r=0x"); Serial.print(candidate.reserved, HEX);
      Serial.print(" id=0x"); Serial.print(candidate.mediaId, HEX);
      Serial.print(" chunk="); Serial.println(chunkSize);
#endif

      // Reusing an already completed index means the app is replacing/retrying
      // that activity inside the same list upload. Start its staging file over.
      if (scheduleReceivedMask & bit) {
        scheduleReceivedMask &= ~bit;
        scheduleStaging[idx] = ScheduleActivity();
        resetScheduleMediaUpload(idx, true);
      } else if (u.active) {
        if (!scheduleMediaObjectMatches(u.candidate, candidate)) {
#if DEBUG_SERIAL
          Serial.print("SCH OBJECT CHANGE i="); Serial.print(idx);
          Serial.print(" old(t/n/crc)="); Serial.print(u.candidate.contentType); Serial.print('/');
          Serial.print(u.candidate.mediaSize); Serial.print("/0x"); Serial.print(u.candidate.mediaCRC, HEX);
          Serial.print(" new="); Serial.print(candidate.contentType); Serial.print('/');
          Serial.print(candidate.mediaSize); Serial.print("/0x"); Serial.println(candidate.mediaCRC, HEX);
#endif
          // A different media identity really is a new/restarted activity.
          resetScheduleMediaUpload(idx, true);
        } else if (!scheduleActivityHeaderMetadataMatches(u.candidate, candidate)) {
#if DEBUG_SERIAL
          // Ancillary timing/id fields may vary without changing the media
          // object. The per-chunk marker at offset 11 is intentionally excluded
          // from ScheduleActivity and from this comparison.
          Serial.print("SCH META VAR i="); Serial.print(idx);
          Serial.print(" f "); Serial.print(u.candidate.flags, HEX); Serial.print('>'); Serial.print(candidate.flags, HEX);
          Serial.print(" st "); Serial.print(u.candidate.startHour); Serial.print(':'); Serial.print(u.candidate.startMinute);
          Serial.print('>'); Serial.print(candidate.startHour); Serial.print(':'); Serial.print(candidate.startMinute);
          Serial.print(" en "); Serial.print(u.candidate.endHour); Serial.print(':'); Serial.print(u.candidate.endMinute);
          Serial.print('>'); Serial.print(candidate.endHour); Serial.print(':'); Serial.print(candidate.endMinute);
          Serial.print(" r "); Serial.print(u.candidate.reserved, HEX); Serial.print('>'); Serial.print(candidate.reserved, HEX);
          Serial.print(" id "); Serial.print(u.candidate.mediaId, HEX); Serial.print('>'); Serial.println(candidate.mediaId, HEX);
#endif
        }
      }

      if (!scheduleMediaUploads[idx].active)
        ok = beginScheduleMediaUpload(idx, candidate);
    }

    if (ok) {
      ScheduleMediaUploadState &u = scheduleMediaUploads[idx];
      if ((uint64_t)u.received + (uint64_t)chunkSize > (uint64_t)u.candidate.mediaSize) {
        ok = false;
      } else {
        File f = LittleFS.open(scheduleTempFileName(idx), "a");
        bool writeOK = (bool)f;
        if (writeOK && (uint32_t)f.size() != u.received) writeOK = false;
        if (writeOK && f.write(data + 23, chunkSize) != chunkSize) writeOK = false;
        if (f) f.close();
        if (!writeOK) {
          ok = false;
        } else {
          u.runningCRC = crc32Update(u.runningCRC, data + 23, chunkSize);
          u.received += (uint32_t)chunkSize;
          u.lastChunkAt = millis();
          // Keep the activity metadata from the first chunk. Later ancillary
          // header fields may be chunk markers; SCH META VAR records changes.
          scheduleUploadOpen = true;
          scheduleLastRxMs = u.lastChunkAt;

          if (u.received == u.candidate.mediaSize) {
            const uint32_t calc = u.runningCRC ^ 0xFFFFFFFFUL;
            if (calc != u.candidate.mediaCRC) {
#if DEBUG_SERIAL
              Serial.print("SCH CRC FAIL i="); Serial.print(idx);
              Serial.print(" calc=0x"); Serial.print(calc,HEX);
              Serial.print(" expected=0x"); Serial.println(u.candidate.mediaCRC,HEX);
#endif
              ok = false;
            } else {
              scheduleStaging[idx] = u.candidate;
              scheduleReceivedMask |= (1UL << idx);
              scheduleUploadDirty = true;
              u.active = false;
            }
          }
        }
      }
    }

    if (!ok) {
      // Schedule replacement is list-atomic by design. A malformed/failed media
      // chunk invalidates the new staging transaction but never touches the
      // currently active Schedule.
      abortScheduleUploadTransaction("activity media error");
    }

#if DEBUG_SERIAL
    if (ok) {
      const ScheduleMediaUploadState &u = scheduleMediaUploads[idx];
      const uint32_t total = (scheduleReceivedMask & (1UL << idx))
                               ? scheduleStaging[idx].mediaSize : u.received;
      const uint32_t expected = (scheduleReceivedMask & (1UL << idx))
                               ? scheduleStaging[idx].mediaSize : u.candidate.mediaSize;
      Serial.print("SCH RX i="); Serial.print(idx);
      Serial.print(" chunk="); Serial.print(chunkSize);
      Serial.print(" total="); Serial.print(total); Serial.print("/"); Serial.print(expected);
      Serial.print(" done="); Serial.println((scheduleReceivedMask & (1UL << idx)) ? 1 : 0);
    } else {
      Serial.print("SCH RX i="); Serial.print(idx);
      Serial.print(" chunk="); Serial.print(chunkSize);
      Serial.println(" ok=0");
    }
#endif

    // Hardware validation established the large-activity flow:
    // 0x01 requests the next chunk while the declared media object is incomplete,
    // and 0x03 terminates the CRC-valid final chunk. One-packet/small activities
    // therefore still receive the historically required 0x03 immediately.
    uint8_t scheduleStatus=0x02;
    if(ok){
      const bool complete=(scheduleReceivedMask & (1UL << idx)) != 0;
      scheduleStatus=complete ? (uint8_t)0x03 : (uint8_t)0x01;
#if DEBUG_SERIAL
      Serial.print("SCH ACK i="); Serial.print(idx);
      Serial.print(" status="); Serial.println(scheduleStatus,HEX);
#endif
    }
    uint8_t ack[5] = {0x05,0x00,0x05,0x80,scheduleStatus};
    sendFA03(ack, sizeof(ack));
    return true;
  }
  return false;
}

// ======================================================
// PERSISTENT RESET + BOOT POLICY
// ======================================================
void clearPersistentDeviceState() {
  abortAlarmUpload(nullptr);
  alarmPrefs.clear();
  for (uint8_t i=0;i<ALARM_SLOT_COUNT;i++) alarms[i]=AlarmSlot();

  schedulePrefs.clear();
  scheduleGlobalFlags=0; scheduleStagingGlobalFlags=0;
  scheduleUploadOpen=false; scheduleUploadDirty=false; scheduleReceivedMask=0;
  for (uint8_t i=0;i<SCHEDULE_MAX_ACTIVITIES;i++) {
    scheduleActivities[i]=ScheduleActivity();
    scheduleStaging[i]=ScheduleActivity();
  }

  stopPresetPlayback();
  clearPresetBank(true);

  if (carouselPrefsReady) carouselPrefs.clear();
  carouselOrderCount=0;
  for (uint8_t i=0;i<CAROUSEL_SLOT_COUNT;i++) {
    carouselOrder[i]=0;
    carouselSlots[i]=CarouselSlotMeta();
  }

  if (littleFsReady) {
    for (uint8_t i=0;i<ALARM_SLOT_COUNT;i++) {
      LittleFS.remove(alarmFileName(i));
      LittleFS.remove(String("/alarm")+i+".tmp");
      LittleFS.remove(String("/alarm")+i+".bak");
    }
    for (uint8_t i=0;i<SCHEDULE_MAX_ACTIVITIES;i++) {
      LittleFS.remove(scheduleFileName(i));
      LittleFS.remove(String("/sch")+i+".tmp");
      LittleFS.remove(String("/sch")+i+".bak");
    }
    for (uint8_t i=0;i<CAROUSEL_SLOT_COUNT;i++) {
      LittleFS.remove(carouselFileName(i,1));
      LittleFS.remove(carouselFileName(i,2));
      LittleFS.remove(carouselFileName(i,3));
      LittleFS.remove(carouselTempFileName(i));
      LittleFS.remove(carouselBackupFileName(i));
      LittleFS.remove(carouselBankBackupFileName(i,1));
      LittleFS.remove(carouselBankBackupFileName(i,2));
      LittleFS.remove(carouselBankBackupFileName(i,3));
    }
    LittleFS.remove(CAROUSEL_BANK_TXN_FILE);
    LittleFS.remove(CAROUSEL_BANK_TXN_TMP_FILE);
    carouselBankTxnActive=false;
    LittleFS.remove(GIF_PLAY_FILE); LittleFS.remove(GIF_PLAY_BACKUP_FILE);
    LittleFS.remove(GIF_RX_FILES[0]); LittleFS.remove(GIF_RX_FILES[1]);
    LittleFS.remove(EVENT_GIF_PLAY_FILE);
  }

  Preferences bp;
  if (bp.begin(CLOCK_NVS_NAMESPACE, false)) { bp.clear(); bp.end(); }
  brightnessPercent=80; brightnessDirty=false;
  if (bp.begin(BRIGHTNESS_NVS_NAMESPACE, false)) {
    bp.putUChar(BRIGHTNESS_NVS_KEY, brightnessPercent);
    bp.end();
  }
  clockStyle=0; clock24h=false; clockShowDate=false; clockDatePreference=false; clockColor=defaultClockColor(0);
  clockPrefsDirty=false;
  energySaving=EnergySavingState();
  flipped180=false;
  diyMode=false;
  // Deliberately preserve clockSynced/syncMillis here: 03/80 is a live
  // device-state reset, not an electrical power cycle.
  applyCurrentBrightness();
#if DEBUG_SERIAL
  Serial.println("DEVICE RESET: persistent emulator state cleared (volatile time preserved)");
#endif
}

void applyBootDisplayPolicy() {
  presetActive=false; presetActiveSlot=-1; presetOrderCount=0; gifPresetPlaybackFileActive=false;
  carouselUploadBlackout=false; carouselUploadOpen=false; carouselStartPending=false;
  carouselActive=false; carouselActiveSlot=-1; carouselEnterRequested=false;
#if defined(IDOTMATRIX_BOARD_CLOCKWISE)
  screenOn=true;
  setStatusLed(true);
  displayMode=DISPLAY_CLOCK;
  clockCycleStartedAt=millis();
  renderClock();
  Serial.println("BOOT POLICY: CLOCK (sync from app)");
#else
  int8_t slot=nextCarouselSlot(-1);
  if (slot>=0) {
    screenOn=true; setStatusLed(true); carouselEnterRequested=true;
    if (startCarouselSlot((uint8_t)slot)) {
#if DEBUG_SERIAL
      Serial.print("BOOT POLICY: stored carousel -> slot "); Serial.println(slot);
#endif
      return;
    }
  }
#if IDOTMATRIX_RTC_AVAILABLE
  if (rtcReady && rtcTimeValid) {
    screenOn=true; setStatusLed(true);
    displayMode=DISPLAY_CLOCK; clockCycleStartedAt=millis(); renderClock();
#if DEBUG_SERIAL
    Serial.println("BOOT POLICY: no stored carousel + valid RTC -> CLOCK");
#endif
    return;
  }
#endif
  screenOn=false; setStatusLed(false); displayMode=DISPLAY_NONE;
  clearFramebuffer(); refreshMatrix();
#if DEBUG_SERIAL
  Serial.println("BOOT POLICY: no stored carousel / valid RTC -> SCREEN OFF");
#endif
#endif
}

#if OLED_STATUS_ENABLED
const char* oledModeName(uint8_t m) {
  switch(m) {
    case DISPLAY_SOLID: return "SOLID"; case DISPLAY_RAW: return "IMAGE";
    case DISPLAY_GRAFFITI: return "DIY"; case DISPLAY_GIF: return presetActive ? "PRESET" : (carouselActive ? "CAROUSEL" : "GIF");
    case DISPLAY_TEXT: return presetActive ? "PRESET" : (carouselActive ? "CAROUSEL" : "TEXT"); case DISPLAY_EFFECT: return "EFFECT";
    case DISPLAY_AUDIO: return "AUDIO"; case DISPLAY_CLOCK: return "CLOCK";
    case DISPLAY_COUNTDOWN: return "COUNTDOWN"; case DISPLAY_STOPWATCH: return "STOPWATCH";
    case DISPLAY_SCOREBOARD: return "SCORE"; default: return "IDLE";
  }
}

void setupStatusOLED() {
  // Same initialization as the hardware-validated DollaTek sketch, using rotation R0.
  statusOLED.begin();
  statusOLED.enableUTF8Print();
  statusOLED.setPowerSave(0);
  statusOLED.setContrast(50);
  statusOLEDReady=true;

  statusOLED.clearBuffer();
  statusOLED.setFont(u8g2_font_7x14B_tf);
  statusOLED.setCursor(0,14);
  statusOLED.print("iDotMatrix B");
  statusOLED.print(FW_BUILD);
  statusOLED.setCursor(0,31);
  statusOLED.print("OLED LIVE STATUS");
  statusOLED.setCursor(0,48);
  statusOLED.print("DollaTek ESP32");
  statusOLED.sendBuffer();
#if DEBUG_SERIAL
  Serial.println("OLED: U8g2 OK - rotation R0, pure event-driven dashboard");
#endif
  delay(1000);
}

void updateStatusOLED() {
  if(!statusOLEDReady) return;

  static bool first = true;
  static uint32_t lastUnknownAtSeen = 0;
  static bool lastBle = false;
  static bool lastScreen = false;
  static uint8_t lastMode = 0xFF;
  static uint8_t lastBrightness = 0xFF;
  static int8_t lastScheduleIndex = -127;
  static uint8_t lastScheduleFlags = 0xFF;
  static bool lastAlarmActive = false;
  static uint8_t lastAlarmSlot = 0xFE;
  static bool lastClockSynced = false;
  static bool lastStopwatchRunning = false;
  static bool lastCountdownRunning = false;
  static bool lastCountdownPaused = false;

  uint32_t now = millis();

  // When the alert expires, force an immediate redraw of the normal dashboard.
  bool alertExpired = false;
  if (unknownCommandActive && (uint32_t)(now - unknownCommandAt) >= OLED_UNKNOWN_ALERT_MS) {
    unknownCommandActive = false;
    alertExpired = true;
  }

  // Events that require an immediate OLED refresh.
  bool unknownChanged = unknownCommandActive && (unknownCommandAt != lastUnknownAtSeen);
  bool stateChanged = first || alertExpired || unknownChanged ||
                      deviceConnected != lastBle ||
                      screenOn != lastScreen ||
                      (uint8_t)displayMode != lastMode ||
                      brightnessPercent != lastBrightness ||
                      scheduleActiveIndex != lastScheduleIndex ||
                      scheduleGlobalFlags != lastScheduleFlags ||
                      alarmActive != lastAlarmActive ||
                      activeAlarmSlot != lastAlarmSlot ||
                      clockSynced != lastClockSynced ||
                      stopwatchRunning != lastStopwatchRunning ||
                      countdownRunning != lastCountdownRunning ||
                      countdownPaused != lastCountdownPaused;

  // No periodic refresh: SW-I2C blocks the loop during
  // sendBuffer(), so the OLED is redrawn only after a real state change.
  if (!stateChanged) return;

  // Snapshot of displayed state: no sendBuffer() until the state changes.
  first = false;
  lastBle = deviceConnected;
  lastScreen = screenOn;
  lastMode = (uint8_t)displayMode;
  lastBrightness = brightnessPercent;
  lastScheduleIndex = scheduleActiveIndex;
  lastScheduleFlags = scheduleGlobalFlags;
  lastAlarmActive = alarmActive;
  lastAlarmSlot = activeAlarmSlot;
  lastClockSynced = clockSynced;
  lastStopwatchRunning = stopwatchRunning;
  lastCountdownRunning = countdownRunning;
  lastCountdownPaused = countdownPaused;
  if (unknownChanged) lastUnknownAtSeen = unknownCommandAt;

  statusOLED.clearBuffer();

  // Unhandled command: show an immediate alert for 8 seconds.
  if (unknownCommandActive) {
    char line[32];
    statusOLED.setFont(u8g2_font_7x14B_tf);
    statusOLED.drawStr(0,13,"UNKNOWN CMD!");
    statusOLED.setFont(u8g2_font_6x10_tf);
    snprintf(line,sizeof(line),"LEN:%u  N:%lu",unknownCommandLen,(unsigned long)unknownCommandCount);
    statusOLED.drawStr(0,25,line);

    for (uint8_t row=0; row<3; row++) {
      char hexline[24]; size_t pos=0;
      for (uint8_t j=0; j<4; j++) {
        uint8_t i=row*4+j;
        if (i>=unknownCommandStored) break;
        pos += snprintf(hexline+pos,sizeof(hexline)-pos,"%02X%s",unknownCommandData[i],j==3?"":" ");
      }
      statusOLED.drawStr(0,37+row*10,hexline);
    }
    statusOLED.sendBuffer();
    return;
  }

  statusOLED.setFont(u8g2_font_6x10_tf);
  char line[32];
  snprintf(line,sizeof(line),"B%d BLE:%s SCR:%s",FW_BUILD,deviceConnected?"ON":"OFF",screenOn?"ON":"OFF");
  statusOLED.drawStr(0,9,line);
  snprintf(line,sizeof(line),"MODE:%s",oledModeName(displayMode));
  statusOLED.drawStr(0,19,line);
  if(hasValidTimeSource()) {
    uint8_t h,m,se; getCurrentTime(h,m,se);
    snprintf(line,sizeof(line),"%02u:%02u:%02u BRI:%u%%",h,m,se,brightnessPercent);
  } else snprintf(line,sizeof(line),"--:--:-- BRI:%u%%",brightnessPercent);
  statusOLED.drawStr(0,29,line);
  if(scheduleActiveIndex>=0) snprintf(line,sizeof(line),"SCH:%s ACT:%d",(scheduleGlobalFlags&1)?"ON":"OFF",scheduleActiveIndex);
  else snprintf(line,sizeof(line),"SCH:%s ACT:-",(scheduleGlobalFlags&1)?"ON":"OFF");
  statusOLED.drawStr(0,39,line);

  if(displayMode==DISPLAY_STOPWATCH) {
    uint32_t e=stopwatchElapsedMs+(stopwatchRunning?(now-stopwatchStartMillis):0);
    snprintf(line,sizeof(line),"SW %s %lu.%03lus",stopwatchRunning?"RUN":"STOP",(unsigned long)(e/1000),(unsigned long)(e%1000));
  } else if(displayMode==DISPLAY_COUNTDOWN) {
    uint32_t r=countdownRemainingMs;
    if(countdownRunning){uint32_t e=now-countdownStartMillis;r=e<r?r-e:0;}
    snprintf(line,sizeof(line),"CD %s %lus",countdownRunning?"RUN":(countdownPaused?"PAUSE":"STOP"),(unsigned long)((r+999)/1000));
  } else if(alarmActive) {
    snprintf(line,sizeof(line),"ALARM SLOT:%u UNK:%lu",activeAlarmSlot,(unsigned long)unknownCommandCount);
  } else {
    snprintf(line,sizeof(line),"UNK:%lu",(unsigned long)unknownCommandCount);
  }
  statusOLED.drawStr(0,49,line);
  snprintf(line,sizeof(line),"HEAP:%luK STK:%lu",(unsigned long)(ESP.getFreeHeap()/1024UL),(unsigned long)uxTaskGetStackHighWaterMark(NULL));
  statusOLED.drawStr(0,60,line);
  statusOLED.sendBuffer();
}
#endif

void printStartupHardwareSummary() {
#if DEBUG_SERIAL
  Serial.println("=== IDOTMATRIX STARTUP SUMMARY ===");
  Serial.println(FW_SIGNATURE);
#if defined(CONFIG_IDF_TARGET_ESP32C3)
  Serial.println("MCU: ESP32-C3");
#elif defined(CONFIG_IDF_TARGET_ESP32S3)
  Serial.println("MCU: ESP32-S3");
#else
  Serial.println("MCU: ESP32");
#endif
  Serial.print("BOARD: ");
#if defined(IDOTMATRIX_BOARD_WAVESHARE_S3_RGB_MATRIX)
  Serial.println("Waveshare ESP32-S3 RGB Matrix");
#elif defined(ARDUINO_ADAFRUIT_MATRIXPORTAL_ESP32S3)
  Serial.println("Adafruit MatrixPortal ESP32-S3");
#else
  Serial.println("generic/custom");
#endif
  Serial.print("FLASH: "); Serial.print(ESP.getFlashChipSize()); Serial.println(" bytes");
#if defined(BOARD_HAS_PSRAM)
  Serial.print("PSRAM: total="); Serial.print(ESP.getPsramSize());
  Serial.print(" free="); Serial.println(ESP.getFreePsram());
#else
  Serial.println("PSRAM: not compiled");
#endif
  Serial.print("OTA: ");
#if IDOTMATRIX_OTA_AVAILABLE
  Serial.print("enabled / "); Serial.println(idotOtaStateText());
#else
  Serial.println("disabled");
#endif
  Serial.print("USER CONFIG: "); Serial.println(IDOTMATRIX_USER_CONFIG_PRESENT ? "present" : "not present");
#if IDOTMATRIX_ORIENTATION_SENSOR || IDOTMATRIX_RTC_AVAILABLE || IDOTMATRIX_AUDIO_CODEC_AVAILABLE
  Serial.print("I2C BUS: ");
  #if defined(IDOTMATRIX_I2C_SDA_PIN) && defined(IDOTMATRIX_I2C_SCL_PIN)
    Serial.print("SDA=GPIO"); Serial.print(IDOTMATRIX_I2C_SDA_PIN);
    Serial.print(" SCL=GPIO"); Serial.println(IDOTMATRIX_I2C_SCL_PIN);
  #else
    Serial.println("board default pins");
  #endif
#else
  Serial.println("I2C BUS: no orientation/RTC backend compiled");
#endif

  Serial.print("RTC BACKEND: ");
#if IDOTMATRIX_RTC_TYPE == IDOTMATRIX_RTC_DS3231
  Serial.println("DS3231");
  Serial.print("RTC ADDRESS: 0x"); Serial.println(IDOTMATRIX_RTC_I2C_ADDRESS, HEX);
  Serial.print("RTC READY: "); Serial.println(rtcReady ? "YES" : "NO");
  Serial.print("RTC TIME VALID: "); Serial.println(rtcTimeValid ? "YES" : "NO");
  Serial.print("RTC STATUS: 0x"); Serial.println(rtc.statusRegister(), HEX);
  if (rtcReady) {
    IDotMatrixRtcDateTime now;
    if (rtc.read(now)) {
      Serial.printf("RTC NOW: %04u-%02u-%02u %02u:%02u:%02u\n",
                    now.year, now.month, now.day, now.hour, now.minute, now.second);
    } else {
      Serial.print("RTC READ: FAILED ("); Serial.print(rtc.errorText()); Serial.println(")");
    }
  }
#else
  Serial.println("NONE");
#endif

  Serial.print("SOFTWARE TIME SYNCED: "); Serial.println(clockSynced ? "YES" : "NO");
  Serial.print("BOOT DISPLAY MODE: "); Serial.println(displayMode);
  Serial.print("SCREEN ON: "); Serial.println(screenOn ? "YES" : "NO");
  Serial.println("=== END STARTUP SUMMARY ===");
#endif
}

void setup(){
#if PNG_DIAG_SERIAL
  Serial.begin(115200);
#else
  DBG_BEGIN(115200);
#endif

  delay(300);
  // Keep the complete release/build signature as one compile-time literal.
  // update_idotmatrix_emulator.sh verifies this exact string inside firmware.bin
  // before allowing upload, preventing stale PlatformIO objects from being flashed.
  DBG_PRINTLN(FW_SIGNATURE);
  idotMemoryTelemetrySnapshot("setup.begin");
#if PNG_DIAG_SERIAL
  Serial.print("BOOT rr="); Serial.print((int)esp_reset_reason());
  Serial.print(" heap="); Serial.println(ESP.getFreeHeap());
#endif
#if STATUS_LED_PIN >= 0
  pinMode(STATUS_LED_PIN,OUTPUT);
#endif
  setStatusLed(false);
#if OLED_STATUS_ENABLED
  setupStatusOLED();
#endif
#if IDOTMATRIX_BUZZER_AVAILABLE
  #if IDOTMATRIX_BUZZER_TYPE == IDOTMATRIX_BUZZER_PASSIVE
    // Establish the inactive level before handing the pin to LEDC. The C3
    // reference module is a transistor-driven low-level-trigger board, so its
    // safe idle state is HIGH.
    pinMode(IDOTMATRIX_BUZZER_PIN, OUTPUT);
    digitalWrite(IDOTMATRIX_BUZZER_PIN, idotPassiveBuzzerIdleLevel(IDOTMATRIX_BUZZER_PASSIVE_TRIGGER_LOW != 0) ? HIGH : LOW);
    // Attach one LEDC channel to the passive buzzer. Tone generation is fully
    // hardware-driven and therefore does not block BLE or display updates.
    buzzerHardwareReady = ledcAttach(IDOTMATRIX_BUZZER_PIN, IDOTMATRIX_BUZZER_FREQUENCY_HZ, BUZZER_LEDC_RESOLUTION_BITS);
    if (!buzzerHardwareReady) {
      Serial.print("BUZZER: LEDC attach failed on GPIO");
      Serial.println(IDOTMATRIX_BUZZER_PIN);
    } else {
      setBuzzerOutput(false);
#if DEBUG_SERIAL
      Serial.print("BUZZER: passive GPIO"); Serial.print(IDOTMATRIX_BUZZER_PIN);
      Serial.print(" frequency="); Serial.print(IDOTMATRIX_BUZZER_FREQUENCY_HZ);
      Serial.print(" Hz trigger="); Serial.println(IDOTMATRIX_BUZZER_PASSIVE_TRIGGER_LOW ? "LOW" : "HIGH");
#endif
    }
  #else
    pinMode(IDOTMATRIX_BUZZER_PIN,OUTPUT);
    digitalWrite(IDOTMATRIX_BUZZER_PIN, IDOTMATRIX_BUZZER_ACTIVE_HIGH ? LOW : HIGH);
    buzzerHardwareReady = true;
#if DEBUG_SERIAL
    Serial.print("BUZZER: active GPIO"); Serial.println(IDOTMATRIX_BUZZER_PIN);
#endif
  #endif
#endif
  idotMemoryTelemetrySnapshot("setup.before_logical_buffers");
// Allocate the logical display buffers at runtime. This is essential for the
  // 64x64 profile: three static 4096-pixel CRGB buffers would consume ~36 KB
  // of .bss and overflow the ESP32 DRAM linker segment.
  framebuffer = (CRGB*)malloc(LOGICAL_FRAME_BYTES);
  gifFrame = (CRGB*)malloc(LOGICAL_FRAME_BYTES);
  scheduleSavedFrame = (CRGB*)malloc(LOGICAL_FRAME_BYTES);
  if (!framebuffer || !gifFrame || !scheduleSavedFrame) {
#if DEBUG_SERIAL
    Serial.print("FATAL: logical framebuffer allocation failed, bytes each=");
    Serial.print(LOGICAL_FRAME_BYTES);
    Serial.print(" freeHeap=");
    Serial.println(ESP.getFreeHeap());
#endif
#if OLED_STATUS_ENABLED
    statusOLED.clearBuffer();
    statusOLED.setFont(u8g2_font_6x10_tf);
    statusOLED.drawStr(0, 14, "MEMORY ERROR");
    statusOLED.drawStr(0, 28, "Logical buffers");
    statusOLED.drawStr(0, 42, "allocation failed");
    statusOLED.sendBuffer();
#endif
    while (true) delay(1000);
  }
  fill_solid(framebuffer, NUM_LEDS, CRGB::Black);
  fill_solid(gifFrame, NUM_LEDS, CRGB::Black);
  fill_solid(scheduleSavedFrame, NUM_LEDS, CRGB::Black);
  idotMemoryTelemetrySnapshot("setup.after_logical_buffers");
#if DEBUG_SERIAL
  Serial.print("LOGICAL BUFFERS: "); Serial.print(MATRIX_WIDTH); Serial.print('x'); Serial.print(MATRIX_HEIGHT);
  Serial.print(" x3, bytes="); Serial.print(LOGICAL_FRAME_BYTES * 3U);
  Serial.print(" freeHeap="); Serial.println(ESP.getFreeHeap());
  Serial.print("DISPLAY BACKEND: ");
#if DISPLAY_BACKEND == DISPLAY_BACKEND_HUB75
  Serial.println("HUB75 / MatrixPanel I2S DMA");
  Serial.print("HUB75 color depth: "); Serial.print(HUB75_COLOR_DEPTH); Serial.println(" bit/channel");
#else
  Serial.println("WS2812 / FastLED");
#endif
  Serial.print("DISPLAY SCALE: screenType="); Serial.print(IDOTMATRIX_SCREEN_TYPE);
  Serial.print(" logical="); Serial.print(MATRIX_WIDTH); Serial.print('x'); Serial.print(MATRIX_HEIGHT);
  Serial.print(" physical="); Serial.print(PHYSICAL_MATRIX_WIDTH); Serial.print('x'); Serial.println(PHYSICAL_MATRIX_HEIGHT);
#if defined(BOARD_HAS_PSRAM)
  Serial.print("PSRAM: total="); Serial.print(ESP.getPsramSize()); Serial.print(" free="); Serial.println(ESP.getFreePsram());
#endif
#endif

#if IDOTMATRIX_ORIENTATION_SENSOR || IDOTMATRIX_RTC_AVAILABLE || IDOTMATRIX_AUDIO_CODEC_AVAILABLE
  #if defined(IDOTMATRIX_I2C_SDA_PIN) && defined(IDOTMATRIX_I2C_SCL_PIN)
    Wire.begin(IDOTMATRIX_I2C_SDA_PIN, IDOTMATRIX_I2C_SCL_PIN);
  #else
    Wire.begin();
  #endif
#endif
#if IDOTMATRIX_AUDIO_CODEC_AVAILABLE
  buzzerHardwareReady = idotAudioOutputBegin();
  if (!buzzerHardwareReady) Serial.println("AUDIO: notification backend unavailable");
#endif
#if IDOTMATRIX_ORIENTATION_SENSOR
  idotOrientationBegin();
#endif
#if IDOTMATRIX_RTC_AVAILABLE
  rtcReady=rtc.begin(Wire, IDOTMATRIX_RTC_I2C_ADDRESS);
  rtcTimeValid=rtcReady && rtc.timeValid();
  if (!rtcReady) {
    rtcRetryNextAt = millis() + IDOTMATRIX_RTC_RETRY_INTERVAL_MS;
  } else if (rtcTimeValid) {
    IDotMatrixRtcDateTime bootRtc;
    if (rtc.read(bootRtc)) {
      syncYear=bootRtc.year; syncMonth=bootRtc.month; syncDay=bootRtc.day;
      syncHour=bootRtc.hour; syncMinute=bootRtc.minute; syncSecond=bootRtc.second;
      syncMillis=monotonicMillis64(); clockSynced=true;
    } else {
      rtcReady=false; rtcTimeValid=false;
      rtcRetryNextAt = millis() + IDOTMATRIX_RTC_RETRY_INTERVAL_MS;
    }
  }
#if DEBUG_SERIAL
  Serial.print("RTC DS3231: "); Serial.print(rtcReady?"OK":"NOT FOUND");
  Serial.print(" addr=0x"); Serial.print(IDOTMATRIX_RTC_I2C_ADDRESS,HEX);
  if (rtcReady) {
    Serial.print(" time="); Serial.println(rtcTimeValid?"VALID":"INVALID");
  } else {
    Serial.println();
  }
  if(rtcReady && !rtcTimeValid) Serial.println("RTC: time invalid/oscillator-stop flag set; waiting for BLE synchronization");
  if(!rtcReady) { Serial.print("RTC: "); Serial.println(rtc.errorText()); }
#if IDOTMATRIX_RTC_DIAGNOSTICS
  if(rtcReady){
    IDotMatrixRtcDateTime rtcDiag;
    if(rtc.read(rtcDiag)){
      Serial.printf("RTC NOW: %04u-%02u-%02u %02u:%02u:%02u status=0x%02X\n",
                    rtcDiag.year,rtcDiag.month,rtcDiag.day,rtcDiag.hour,rtcDiag.minute,rtcDiag.second,rtc.statusRegister());
    }
  }
#endif
#endif
#endif
  // Always try a non-destructive mount first. The repository default enables
  // format-on-fail for first use/recovery; set it to 0 for forensic preservation.
#if DEBUG_SERIAL
  {
    const esp_partition_t *fsPart=esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, nullptr);
    if(fsPart){
      Serial.print("LittleFS partition: label="); Serial.print(fsPart->label);
      Serial.print(" address=0x"); Serial.print(fsPart->address,HEX);
      Serial.print(" size="); Serial.println(fsPart->size);
    } else {
      Serial.println("LittleFS partition: NOT FOUND (expected DATA/SPIFFS subtype)");
    }
  }
#endif
  littleFsReady=LittleFS.begin(false);
#if LITTLEFS_FORMAT_ON_MOUNT_FAIL
  if(!littleFsReady){
#if DEBUG_SERIAL
    Serial.println("LittleFS: mount failed; explicit format-on-fail enabled");
#endif
    // Reuse the Arduino-ESP32 mount-and-format path that older builds used,
    // but only after the explicit opt-in above.
    littleFsReady=LittleFS.begin(true);
  }
#endif
#if DEBUG_SERIAL
  Serial.print("LittleFS: "); Serial.println(littleFsReady ? "OK" : "ERROR (unavailable)");
  if(!littleFsReady){
#if LITTLEFS_FORMAT_ON_MOUNT_FAIL
    Serial.println("LittleFS hint: verify the Arduino partition scheme includes a SPIFFS/LittleFS-compatible data partition; FAT-only schemes are unsupported.");
#else
    Serial.println("LittleFS: set LITTLEFS_FORMAT_ON_MOUNT_FAIL=1 only for intentional recovery/first use");
    Serial.println("LittleFS hint: verify the Arduino partition scheme includes a SPIFFS/LittleFS-compatible data partition; FAT-only schemes are unsupported.");
#endif
  }
  if(littleFsReady){
    Serial.print("LittleFS capacity: used="); Serial.print(LittleFS.usedBytes());
    Serial.print(" total="); Serial.println(LittleFS.totalBytes());
  }
  Serial.print("ALARM SLOTS: "); Serial.println(ALARM_SLOT_COUNT);
#endif
  if(littleFsReady){
    recoverGifPlayFile();
    clearPresetBank(true); // Preset/Default slots are intentionally volatile across reboot.
  }
  // Preferences metadata is still loaded when LittleFS is offline so the
  // device can report/retain configuration; media recovery simply no-ops.
  loadAlarms();
  loadSchedule();
  loadCarousel();
  idotMemoryTelemetrySnapshot("setup.after_storage");
#if DISPLAY_BACKEND == DISPLAY_BACKEND_WS2812
  FastLED.addLeds<LED_TYPE,MATRIX_PIN,COLOR_ORDER>(leds,PHYSICAL_NUM_LEDS);
#elif DISPLAY_BACKEND == DISPLAY_BACKEND_HUB75
  {
    idotMemoryTelemetrySnapshot("setup.before_hub75");
#if defined(IDOTMATRIX_BOARD_WAVESHARE_S3_RGB_MATRIX) && DEBUG_SERIAL
    Serial.println("BOOT CHECKPOINT: entering HUB75 initialization");
    Serial.flush();
#endif
    HUB75_I2S_CFG mxconfig(PHYSICAL_MATRIX_WIDTH, PHYSICAL_MATRIX_HEIGHT, 1, hub75Pins);
    mxconfig.double_buff=false;
    // WLED maps its normal (non-reversed) MatrixPortal configuration to the
    // negative clock phase used successfully on this exact board/panel pair.
    mxconfig.clkphase=false;
    mxconfig.setPixelColorDepthBits(HUB75_COLOR_DEPTH);
    hub75Matrix=new MatrixPanel_I2S_DMA(mxconfig);
    const bool ok=hub75Matrix && hub75Matrix->begin();
#if DEBUG_SERIAL
#if defined(IDOTMATRIX_BOARD_WAVESHARE_S3_RGB_MATRIX)
    Serial.println("BOOT CHECKPOINT: returned from HUB75 initialization");
#endif
    Serial.print("HUB75 MatrixPanel I2S DMA begin="); Serial.println(ok?"OK":"FAILED");
    Serial.print("HUB75 max brightness: "); Serial.println(MAX_LED_BRIGHTNESS);
#endif
    if(!ok){
#if DEBUG_SERIAL
      Serial.println("FATAL: HUB75 MatrixPanel I2S DMA initialization failed");
#endif
      while(true) delay(1000);
    }
    idotMemoryTelemetrySnapshot("setup.after_hub75");
  }
#endif
  loadDisplayPin();
  loadBrightnessFromNVS();
  loadClockPreferencesFromNVS();
  clearPhysicalDisplay(); clearFramebuffer(); fill_solid(gifFrame,NUM_LEDS,CRGB::Black);

  runtimeStateMutex=xSemaphoreCreateMutex();
  if(!runtimeStateMutex){
#if DEBUG_SERIAL
    Serial.println("FATAL: runtime state mutex allocation failed");
#endif
#if OLED_STATUS_ENABLED
    statusOLED.clearBuffer();
    statusOLED.setFont(u8g2_font_6x10_tf);
    statusOLED.drawStr(0,14,"RUNTIME ERROR");
    statusOLED.drawStr(0,28,"Mutex allocation");
    statusOLED.drawStr(0,42,"failed");
    statusOLED.sendBuffer();
#endif
    while(true) delay(1000);
  }

  applyBootDisplayPolicy();

#if IDOTMATRIX_USE_NIMBLE
  // The WLED-qualified Waveshare/Tasmota stack does not bundle the legacy
  // Arduino BLE compatibility headers. Use the same NimBLE-Arduino 2.x family
  // already qualified by the iDotMatrix WLED Usermod on this board.
#if defined(IDOTMATRIX_BOARD_CLOCKWISE)
  uint8_t mac[6]{};
  if (esp_read_mac(mac, ESP_MAC_BT) == ESP_OK)
    snprintf(DEVICE_NAME, sizeof(DEVICE_NAME), "IDM_64*64_%02X%02X", mac[4], mac[5]);
  Serial.printf("APP advertising %s screenType=%u\n", DEVICE_NAME, IDOTMATRIX_SCREEN_TYPE);
#endif
  if(!NimBLEDevice::init(DEVICE_NAME)){
#if DEBUG_SERIAL
    Serial.println("FATAL: NimBLE initialization failed");
#endif
    while(true) delay(1000);
  }
  NimBLEDevice::setMTU(517);
#if DEBUG_SERIAL
  Serial.println("BLE backend: NimBLE-Arduino 2.x");
  Serial.println("BLE local MTU configured: 517");
#endif
  server=NimBLEDevice::createServer();
  if(!server){
#if DEBUG_SERIAL
    Serial.println("FATAL: NimBLE server allocation failed");
#endif
    while(true) delay(1000);
  }
  server->setCallbacks(&serverCallbacksInstance, false);
  NimBLEService *fas=server->createService(NimBLEUUID(uint16_t(0x00FA)));
#if !defined(IDOTMATRIX_BOARD_CLOCKWISE)
  NimBLEService *aes=server->createService(NimBLEUUID(uint16_t(0xAE00)));
#endif
  if (!fas
#if !defined(IDOTMATRIX_BOARD_CLOCKWISE)
      || !aes
#endif
  ) {
#if DEBUG_SERIAL
    Serial.println("FATAL: NimBLE service allocation failed");
#endif
    while(true) delay(1000);
  }
  fa02=fas->createCharacteristic(NimBLEUUID(uint16_t(0xFA02)),NIMBLE_PROPERTY::WRITE|NIMBLE_PROPERTY::WRITE_NR); fa02->setCallbacks(&fa02CallbacksInstance);
#if defined(IDOTMATRIX_BOARD_CLOCKWISE)
  // Match the FA02 user-description descriptor exposed by the reference panel.
  auto* fa02Description = fa02->createDescriptor("2901", NIMBLE_PROPERTY::READ);
  if (fa02Description) {
    constexpr char description[] = "TR2306R007-15";
    fa02Description->setValue(reinterpret_cast<const uint8_t*>(description), sizeof(description));
  }
  fa03=fas->createCharacteristic(NimBLEUUID(uint16_t(0xFA03)),NIMBLE_PROPERTY::NOTIFY);
  if (fa03) fa03->setCallbacks(&deviceInfoCallbacks);
#else
  fa03=fas->createCharacteristic(NimBLEUUID(uint16_t(0xFA03)),NIMBLE_PROPERTY::READ|NIMBLE_PROPERTY::NOTIFY);
  ae01=aes->createCharacteristic(NimBLEUUID(uint16_t(0xAE01)),NIMBLE_PROPERTY::WRITE|NIMBLE_PROPERTY::WRITE_NR);
  ae02=aes->createCharacteristic(NimBLEUUID(uint16_t(0xAE02)),NIMBLE_PROPERTY::READ|NIMBLE_PROPERTY::NOTIFY);
#endif
  if (ae01) ae01->setCallbacks(&ae01CallbacksInstance);
  if (!fa02 || !fa03
#if !defined(IDOTMATRIX_BOARD_CLOCKWISE)
      || !ae01 || !ae02
#endif
      || !server->start()) {
#if DEBUG_SERIAL
    Serial.println("FATAL: NimBLE GATT database start failed");
#endif
    while(true) delay(1000);
  }

  NimBLEAdvertising *adv=NimBLEDevice::getAdvertising();
  NimBLEAdvertisementData ad;
  ad.setFlags(BLE_HS_ADV_F_DISC_GEN|BLE_HS_ADV_F_BREDR_UNSUP);
  ad.setName(DEVICE_NAME);
  const char mb[]={0x54,0x52,0x00,0x70,(char)IDOTMATRIX_SCREEN_TYPE,(char)FW_RELEASE_MAJOR,(char)FW_RELEASE_MINOR};
  NimBLEAdvertisementData scan;
#if defined(IDOTMATRIX_BOARD_CLOCKWISE)
  ad.setCompleteServices(NimBLEUUID(uint16_t(0x00FA)));
  constexpr char testManufacturerData[] = {0x54,0x52,0x00,0x70,char(IDOTMATRIX_SCREEN_TYPE),5,11,0x00,0x01,0x05};
  scan.setManufacturerData(std::string(testManufacturerData, sizeof(testManufacturerData)));
#else
  ad.setCompleteServices(NimBLEUUID(uint16_t(0x00FA)));
  ad.setManufacturerData(std::string(mb,sizeof(mb)));
  scan.setCompleteServices(NimBLEUUID(uint16_t(0xAE00)));
#endif
  adv->setAdvertisementData(ad);
  adv->setScanResponseData(scan);
  adv->enableScanResponse(true);
  if(!adv->start()){
#if DEBUG_SERIAL
    Serial.println("FATAL: NimBLE advertising start failed");
#endif
    while(true) delay(1000);
  }
#else
  BLEDevice::init(DEVICE_NAME);
  // Match the working WLED/NimBLE implementation and the official app's
  // large-transfer expectations. On Arduino-ESP32 3.3.x the compatibility
  // BLE layer otherwise negotiates an ATT payload of about 253 bytes. The app
  // appears to schedule long writes as if a 517-byte MTU were available; that
  // mismatch truncates 778-byte TEXT packets after 2 writes (506 bytes) and
  // 2890-byte media packets after 6 writes (1518 bytes).
  BLEDevice::setMTU(517);
#if DEBUG_SERIAL
  Serial.println("BLE backend: Arduino BLE compatibility layer");
  Serial.println("BLE local MTU configured: 517");
#endif
  server=BLEDevice::createServer(); server->setCallbacks(&serverCallbacksInstance);
  BLEService *fas=server->createService(FA_SERVICE_UUID);
  fa02=fas->createCharacteristic(FA02_UUID,BLECharacteristic::PROPERTY_WRITE|BLECharacteristic::PROPERTY_WRITE_NR); fa02->setCallbacks(&fa02CallbacksInstance);
  fa03=fas->createCharacteristic(FA03_UUID,BLECharacteristic::PROPERTY_READ|BLECharacteristic::PROPERTY_NOTIFY); fas->start();
  BLEService *aes=server->createService(AE_SERVICE_UUID);
  ae01=aes->createCharacteristic(AE01_UUID,BLECharacteristic::PROPERTY_WRITE|BLECharacteristic::PROPERTY_WRITE_NR); ae01->setCallbacks(&ae01CallbacksInstance);
  ae02=aes->createCharacteristic(AE02_UUID,BLECharacteristic::PROPERTY_READ|BLECharacteristic::PROPERTY_NOTIFY); aes->start();

  BLEAdvertising *adv=BLEDevice::getAdvertising(); BLEAdvertisementData ad;
  ad.setFlags(ESP_BLE_ADV_FLAG_GEN_DISC|ESP_BLE_ADV_FLAG_BREDR_NOT_SPT); ad.setName(DEVICE_NAME); ad.setCompleteServices(BLEUUID(FA_SERVICE_UUID));
  const char mb[]={0x54,0x52,0x00,0x70,(char)IDOTMATRIX_SCREEN_TYPE,(char)FW_RELEASE_MAJOR,(char)FW_RELEASE_MINOR}; ad.setManufacturerData(String(mb,sizeof(mb))); adv->setAdvertisementData(ad);
  BLEAdvertisementData scan; scan.setCompleteServices(BLEUUID(AE_SERVICE_UUID)); adv->setScanResponseData(scan); adv->start();
#endif

  idotMemoryTelemetrySnapshot("setup.after_ble");
#if IDOTMATRIX_OTA_AVAILABLE
  idotOtaBegin(FW_RELEASE, FW_BUILD);
#endif
  idotMemoryTelemetrySnapshot("setup.after_ota_arm");

#if IDOTMATRIX_ORIENTATION_SENSOR && IDOTMATRIX_ORIENTATION_DIAGNOSTICS
  // Repeat the orientation summary at the end of setup. On ESP32-C3 USB/CDC
  // serial can attach after the early sensor probe, so this late summary keeps
  // qualification diagnostics visible without changing sensor behaviour.
  idotOrientationPrintDiagnostics();
#endif

  printStartupHardwareSummary();
  reportHeap("setup complete");
}

// ======================================================
// LOOP
// ======================================================
void loop(){
#if IDOTMATRIX_OTA_AVAILABLE
  // OTA HTTP handling must remain outside the runtime-state mutex. Firmware
  // upload can take seconds and must never block BLE callbacks or leave shared
  // playback state locked.
  idotOtaLoop(millis());
#endif
  bool restartAdvertisingNow=false;
  if(!lockRuntimeState()){ delay(1); return; }
  // Take the time snapshot only after the runtime mutex is held.
  // If loop() captures millis() before blocking on the mutex, a BLE callback
  // can update packetLastRxMs/bulkLastRxMs to a newer value while loop() waits.
  // Unsigned subtraction would then wrap and falsely look like a huge timeout.
  uint32_t now=millis();
  // Publish completed Preset/Carousel assets on loopTask before any
  // timeout or playback work. This keeps LittleFS remove/rename/NVS activity
  // off nimble_host and preserves ACK pacing for the next bulk transfer.
  processDeferredAssetCommit();
  // 06/02 may publish a complete Preset candidate bank and start its
  // first decoder. Keep that work on loopTask as well, then perform any
  // disconnect/bulk-abort cleanup only after pending publication has settled.
  processDeferredPresetActivation();
  processPresetBankCleanup();
  // Perform 02/01 Carousel bank journaling/backup/wipe on loopTask, never
  // on the NimBLE host callback stack. ACK is sent by the finalizer.
  processDeferredCarouselBankSetup();
  now=millis();
#if IDOTMATRIX_RTC_AVAILABLE
  updateRtcRecovery(now);
#endif
#if IDOTMATRIX_ORIENTATION_SENSOR
  // A static framebuffer must be redrawn immediately when the panel orientation
  // changes; animated modes will continue to use the same final-output mapping.
  if (idotOrientationUpdate(now)) refreshMatrix();
#endif
  expireStalledTransfers(now);
  flushBrightnessSaveIfNeeded();
  flushClockPreferencesSaveIfNeeded();
#if OLED_STATUS_ENABLED
  updateStatusOLED();
#endif
  if(pendingAdvertisingRestart && (long)(now-advertisingRestartAt)>=0){
    pendingAdvertisingRestart=false;
    restartAdvertisingNow=true;
  }
  if(pendingDeviceInfoPush && (long)(now-deviceInfoPushAt)>=0){pendingDeviceInfoPush=false;if(deviceConnected)sendDeviceInfo();}
  if(pendingSoftReset && (long)(now-softResetAt)>=0){pendingSoftReset=false;resetRuntimeState();}

  updateAlarms();
  updateSchedule();
  updateBuzzer();
  updateEnergySavingOutput(now);

  // B73: a decoder instance is never reused.  Switching GIFs is split across
  // separate loop iterations: first destroy the old AnimatedGIF object and
  // promote RX -> PLAY, then allocate/open a fresh decoder on the next pass.
  if(pendingGifStart && (long)(now-pendingGifStartAt)>=0 && !gifDecoderTeardownPending && !gifDecoderOpenPending){
    int8_t slot=pendingGifSlot;
    uint32_t newSize=pendingGifSize;
    uint32_t newCRC=pendingGifCRC;
    pendingGifStart=false;
    pendingGifSlot=-1;
    pendingGifSize=0;
    pendingGifCRC=0;
    if(slot>=0 && slot<=1){
      const char *rxPath=GIF_RX_FILES[(uint8_t)slot];
#if DEBUG_SERIAL && BULK_PROTOCOL_DEBUG
      Serial.print("GIF RX -> PLAY SWITCH slot="); Serial.print((int)slot);
      Serial.print(" bytes="); Serial.println(newSize);
      reportHeap("before GIF teardown");
#endif
      bool restartPrevious=(littleFsReady && displayMode==DISPLAY_GIF && gifStoredOnFS &&
                            !gifEventPlaybackFileActive && !gifCarouselPlaybackFileActive &&
                            LittleFS.exists(GIF_PLAY_FILE));
      uint32_t previousPlaySize=restartPrevious ? storedFileSize(GIF_PLAY_FILE) : 0;
      uint32_t previousPlayCRC=restartPrevious ? gifPlayCRC : 0;
      carouselActive=false; carouselActiveSlot=-1; carouselEnterRequested=false; carouselUploadOpen=false; carouselUploadBlackout=false; carouselStartPending=false;
      stopGIFPlayback();
      gifCacheInvalidatePath(GIF_PLAY_FILE);
      gifStoredOnFS=false; gifSize=0; gifPlayCRC=0;

      bool promoted=promoteGifRxToPlay((uint8_t)slot);
      if(!promoted){
#if DEBUG_SERIAL
        Serial.println("GIF RX -> PLAY rename FAILED; previous PLAY preserved");
#endif
        if(restartPrevious && previousPlaySize && littleFsReady && LittleFS.exists(GIF_PLAY_FILE)){
          gifStoredOnFS=true;
          gifSize=previousPlaySize;
          gifPlayCRC=previousPlayCRC;
          gifDecoderOpenPending=true;
          gifDecoderOpenAt=millis()+1;
        }
      } else {
        gifStoredOnFS=true;
        gifSize=newSize;
        gifPlayCRC=newCRC;
#if DEBUG_SERIAL && BULK_PROTOCOL_DEBUG
        Serial.println("GIF RX -> PLAY rename OK");
        Serial.println("GIF OPEN deferred to next loop");
#endif
        gifDecoderOpenPending=true;
        gifDecoderOpenAt=millis()+1;
      }
    }
  }

  if(gifDecoderOpenPending && (long)(now-gifDecoderOpenAt)>=0){
    gifDecoderOpenPending=false;
#if DEBUG_SERIAL && BULK_PROTOCOL_DEBUG
    Serial.println("GIF FRESH DECODER START");
    reportHeap("before fresh GIF open");
#endif
    if(!startGIF()){
#if DEBUG_SERIAL
      Serial.println("GIF PLAY OPEN failed");
#endif
      destroyGIFDecoder();
      gifStoredOnFS=false;
      gifSize=0;
    } else {
#if DEBUG_SERIAL && BULK_PROTOCOL_DEBUG
      Serial.println("GIF PLAY START OK");
      reportHeap("after fresh GIF open");
#endif
    }
  }

  if(presetTransferIndicatorActive && (uint32_t)(now-presetTransferLastActivityAt)>=PRESET_TRANSFER_TIMEOUT_MS){
    presetTransferIndicatorActive=false;
    endCarouselTransferIndicator();
  }
  if(carouselTransferIndicatorActive) renderCarouselTransferIndicator(now);

  if(!carouselTransferIndicatorActive && displayMode==DISPLAY_EFFECT) updateEffect();
  if(!carouselTransferIndicatorActive && displayMode==DISPLAY_AUDIO){ static uint32_t lastAudioRender=0; if(now-lastAudioRender>=80){ lastAudioRender=now; renderAudio(); } }
  updateCarousel(now);
  updatePreset(now);
  if(!carouselTransferIndicatorActive && displayMode==DISPLAY_GIF) updateGIF();
  if(!carouselTransferIndicatorActive && displayMode==DISPLAY_TEXT) updateTextAnimation();
  // Copy at most one prefetch chunk after the active media has had its
  // loop-time rendering opportunity. The cached source remains authoritative
  // only after complete size/CRC verification.
  if(!carouselTransferIndicatorActive) serviceCarouselGifPrefetch(now);

  if (!carouselTransferIndicatorActive && displayMode == DISPLAY_CLOCK) {
    static uint32_t lastClockFrame = 0;
    uint32_t frameInterval = 100;
#if defined(IDOTMATRIX_BOARD_CLOCKWISE)
    if ((clockStyle & 0x07) == 0) frameInterval = 25;
#endif
    if (now - lastClockFrame >= frameInterval) {
      lastClockFrame = now;
      renderClock();
    }
  }

  if(!carouselTransferIndicatorActive && displayMode==DISPLAY_COUNTDOWN){
    static uint32_t last=0; uint32_t remain=countdownRemainingMs;
    if(countdownRunning){ uint32_t e=now-countdownStartMillis; if(e>=countdownRemainingMs){remain=0;countdownRemainingMs=0;countdownRunning=false;if(!countdownFinishSent){countdownFinishSent=true;sendCommandStatus(0x08,0x80,0x03);triggerCountdownFinishBuzzer();}} else remain=countdownRemainingMs-e; }
    if(now-last>=200){
      last=now;
      renderCountdown(remain);
    }
  }

  if(!carouselTransferIndicatorActive && displayMode==DISPLAY_STOPWATCH){
    static uint32_t last=0; if(now-last>=100){last=now;uint32_t e=stopwatchElapsedMs;if(stopwatchRunning)e+=now-stopwatchStartMillis;renderStopwatch(e);}
  }

#if IDOTMATRIX_MEMORY_TELEMETRY
  static uint32_t lastMemoryTelemetry=0;
  if(now-lastMemoryTelemetry>=30000UL){
    lastMemoryTelemetry=now;
    idotMemoryTelemetrySnapshot("runtime.periodic");
  }
#endif
#if DEBUG_SERIAL
  static uint32_t lastHeap=0;
#if DEBUG_SERIAL && DEBUG_SERIAL_VERBOSE
  if(now-lastHeap>=30000){lastHeap=now;reportHeap("periodic");}
#else
  (void)lastHeap;
#endif
#endif
  unlockRuntimeState();
  if(restartAdvertisingNow){
#if IDOTMATRIX_USE_NIMBLE
    NimBLEDevice::startAdvertising();
#else
    BLEDevice::startAdvertising();
#endif
  }
  delay(5);
}
