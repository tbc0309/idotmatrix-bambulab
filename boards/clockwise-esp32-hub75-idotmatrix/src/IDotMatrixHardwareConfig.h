#pragma once

// -----------------------------------------------------------------------------
// Optional local hardware configuration
//
// Copy IDotMatrixUserConfig.example.h to IDotMatrixUserConfig.h and edit the
// copy to keep board-specific wiring outside the tracked source files. The
// local header is optional and, when present, its explicit settings take
// precedence over PlatformIO profile defaults.
// -----------------------------------------------------------------------------
#if __has_include("IDotMatrixUserConfig.h")
  #include "IDotMatrixUserConfig.h"
  #define IDOTMATRIX_USER_CONFIG_PRESENT 1
#else
  #define IDOTMATRIX_USER_CONFIG_PRESENT 0
#endif


// -----------------------------------------------------------------------------
// Optional OTA maintenance service
//
// OTA is disabled by default and enabled only by profiles that explicitly
// provide IDOTMATRIX_DEFAULT_OTA_ENABLED=1. The initial 0.6.0 development
// implementation uses a physical-button maintenance mode: hold the configured
// button for IDOTMATRIX_OTA_TRIGGER_HOLD_MS while the firmware is running to
// start a temporary Wi-Fi access point and local HTTP upload page. This keeps
// Wi-Fi off during normal operation and avoids coupling OTA to stored network
// credentials.
// -----------------------------------------------------------------------------
#ifndef IDOTMATRIX_OTA_ENABLED
  #ifdef IDOTMATRIX_DEFAULT_OTA_ENABLED
    #define IDOTMATRIX_OTA_ENABLED IDOTMATRIX_DEFAULT_OTA_ENABLED
  #else
    #define IDOTMATRIX_OTA_ENABLED 0
  #endif
#endif

#ifndef IDOTMATRIX_OTA_TRIGGER_PIN
  #ifdef IDOTMATRIX_DEFAULT_OTA_TRIGGER_PIN
    #define IDOTMATRIX_OTA_TRIGGER_PIN IDOTMATRIX_DEFAULT_OTA_TRIGGER_PIN
  #else
    #define IDOTMATRIX_OTA_TRIGGER_PIN -1
  #endif
#endif

#ifndef IDOTMATRIX_OTA_TRIGGER_ACTIVE_LOW
  #ifdef IDOTMATRIX_DEFAULT_OTA_TRIGGER_ACTIVE_LOW
    #define IDOTMATRIX_OTA_TRIGGER_ACTIVE_LOW IDOTMATRIX_DEFAULT_OTA_TRIGGER_ACTIVE_LOW
  #else
    #define IDOTMATRIX_OTA_TRIGGER_ACTIVE_LOW 1
  #endif
#endif

#ifndef IDOTMATRIX_OTA_TRIGGER_HOLD_MS
  #define IDOTMATRIX_OTA_TRIGGER_HOLD_MS 2000UL
#endif

#ifndef IDOTMATRIX_OTA_TRIGGER_DEBOUNCE_MS
  #define IDOTMATRIX_OTA_TRIGGER_DEBOUNCE_MS 40UL
#endif

#ifndef IDOTMATRIX_OTA_AP_PASSWORD
  #define IDOTMATRIX_OTA_AP_PASSWORD "idotmatrix"
#endif

#if IDOTMATRIX_OTA_ENABLED != 0 && IDOTMATRIX_OTA_ENABLED != 1
  #error "IDOTMATRIX_OTA_ENABLED must be 0 or 1"
#endif

#if IDOTMATRIX_OTA_ENABLED && IDOTMATRIX_OTA_TRIGGER_PIN < 0
  #error "OTA maintenance mode requires IDOTMATRIX_OTA_TRIGGER_PIN >= 0"
#endif

#if IDOTMATRIX_OTA_TRIGGER_ACTIVE_LOW != 0 && IDOTMATRIX_OTA_TRIGGER_ACTIVE_LOW != 1
  #error "IDOTMATRIX_OTA_TRIGGER_ACTIVE_LOW must be 0 or 1"
#endif

#if IDOTMATRIX_OTA_ENABLED
  #define IDOTMATRIX_OTA_AVAILABLE 1
#else
  #define IDOTMATRIX_OTA_AVAILABLE 0
#endif

// PlatformIO profiles provide DEFAULT_* values rather than hard overrides.
// This lets IDotMatrixUserConfig.h select a different backend/address/mount
// without editing platformio.ini or causing duplicate driver selections.
#if (defined(IDOTMATRIX_DEFAULT_ACCEL_DRIVER_LIS3DH) && defined(IDOTMATRIX_DEFAULT_ACCEL_DRIVER_ICM20689)) || \
    (defined(IDOTMATRIX_DEFAULT_ACCEL_DRIVER_LIS3DH) && defined(IDOTMATRIX_DEFAULT_ACCEL_DRIVER_MPU6050)) || \
    (defined(IDOTMATRIX_DEFAULT_ACCEL_DRIVER_ICM20689) && defined(IDOTMATRIX_DEFAULT_ACCEL_DRIVER_MPU6050))
  #error "Select only one IDOTMATRIX_DEFAULT_ACCEL_DRIVER_* fallback"
#endif

#if defined(IDOTMATRIX_ACCEL_DRIVER_NONE) && (defined(IDOTMATRIX_ACCEL_DRIVER_LIS3DH) || \
    defined(IDOTMATRIX_ACCEL_DRIVER_ICM20689) || defined(IDOTMATRIX_ACCEL_DRIVER_MPU6050))
  #error "IDOTMATRIX_ACCEL_DRIVER_NONE cannot be combined with an accelerometer backend"
#endif

#if defined(IDOTMATRIX_ACCEL_DRIVER_NONE) || \
    defined(IDOTMATRIX_ACCEL_DRIVER_LIS3DH) || \
    defined(IDOTMATRIX_ACCEL_DRIVER_ICM20689) || \
    defined(IDOTMATRIX_ACCEL_DRIVER_MPU6050)
  #define IDOTMATRIX_ACCEL_DRIVER_EXPLICIT 1
#else
  #define IDOTMATRIX_ACCEL_DRIVER_EXPLICIT 0
#endif

#if !IDOTMATRIX_ACCEL_DRIVER_EXPLICIT
  #if defined(IDOTMATRIX_DEFAULT_ACCEL_DRIVER_LIS3DH)
    #define IDOTMATRIX_ACCEL_DRIVER_LIS3DH
  #elif defined(IDOTMATRIX_DEFAULT_ACCEL_DRIVER_ICM20689)
    #define IDOTMATRIX_ACCEL_DRIVER_ICM20689
  #elif defined(IDOTMATRIX_DEFAULT_ACCEL_DRIVER_MPU6050)
    #define IDOTMATRIX_ACCEL_DRIVER_MPU6050
  #endif
#endif

// A profile address belongs to its profile-selected backend. If the user
// explicitly switches backend but does not specify an address, let that
// backend use its own safe default (LIS3DH=0x19, MPU family=auto-probe).
#ifndef IDOTMATRIX_ACCEL_I2C_ADDRESS
  #if !IDOTMATRIX_ACCEL_DRIVER_EXPLICIT && defined(IDOTMATRIX_DEFAULT_ACCEL_I2C_ADDRESS)
    #define IDOTMATRIX_ACCEL_I2C_ADDRESS IDOTMATRIX_DEFAULT_ACCEL_I2C_ADDRESS
  #endif
#endif

#ifndef IDOTMATRIX_ACCEL_MOUNT_ROTATION
  #ifdef IDOTMATRIX_DEFAULT_ACCEL_MOUNT_ROTATION
    #define IDOTMATRIX_ACCEL_MOUNT_ROTATION IDOTMATRIX_DEFAULT_ACCEL_MOUNT_ROTATION
  #else
    #define IDOTMATRIX_ACCEL_MOUNT_ROTATION 0
  #endif
#endif


// -----------------------------------------------------------------------------
// Optional synthesized codec audio backend
//
// This is deliberately separate from the buzzer electrical backend. On the
// Waveshare ESP32-S3 RGB Matrix it drives the onboard ES8311 through I2S1 and
// synthesizes notification tones at runtime; no PCM/WAV sample assets exist.
// -----------------------------------------------------------------------------
#ifndef IDOTMATRIX_AUDIO_CODEC_ENABLED
  #ifdef IDOTMATRIX_DEFAULT_AUDIO_CODEC_ENABLED
    #define IDOTMATRIX_AUDIO_CODEC_ENABLED IDOTMATRIX_DEFAULT_AUDIO_CODEC_ENABLED
  #else
    #define IDOTMATRIX_AUDIO_CODEC_ENABLED 0
  #endif
#endif

#ifndef IDOTMATRIX_AUDIO_CODEC_I2C_ADDRESS
  #define IDOTMATRIX_AUDIO_CODEC_I2C_ADDRESS 0x18
#endif
#ifndef IDOTMATRIX_AUDIO_I2C_SDA_PIN
  #define IDOTMATRIX_AUDIO_I2C_SDA_PIN 47
#endif
#ifndef IDOTMATRIX_AUDIO_I2C_SCL_PIN
  #define IDOTMATRIX_AUDIO_I2C_SCL_PIN 48
#endif
#ifndef IDOTMATRIX_AUDIO_I2S_BCLK_PIN
  #define IDOTMATRIX_AUDIO_I2S_BCLK_PIN 43
#endif
#ifndef IDOTMATRIX_AUDIO_I2S_WS_PIN
  #define IDOTMATRIX_AUDIO_I2S_WS_PIN 38
#endif
#ifndef IDOTMATRIX_AUDIO_I2S_DOUT_PIN
  #define IDOTMATRIX_AUDIO_I2S_DOUT_PIN 21
#endif
#ifndef IDOTMATRIX_AUDIO_I2S_MCLK_PIN
  #define IDOTMATRIX_AUDIO_I2S_MCLK_PIN 12
#endif
#ifndef IDOTMATRIX_AUDIO_PA_ENABLE_PIN
  #define IDOTMATRIX_AUDIO_PA_ENABLE_PIN 11
#endif
#ifndef IDOTMATRIX_AUDIO_CODEC_VOLUME
  #define IDOTMATRIX_AUDIO_CODEC_VOLUME 100
#endif

#if IDOTMATRIX_AUDIO_CODEC_ENABLED != 0 && IDOTMATRIX_AUDIO_CODEC_ENABLED != 1
  #error "IDOTMATRIX_AUDIO_CODEC_ENABLED must be 0 or 1"
#endif
#if IDOTMATRIX_AUDIO_CODEC_VOLUME < 0 || IDOTMATRIX_AUDIO_CODEC_VOLUME > 100
  #error "IDOTMATRIX_AUDIO_CODEC_VOLUME must be in range 0..100"
#endif
#if IDOTMATRIX_AUDIO_CODEC_ENABLED
  #define IDOTMATRIX_AUDIO_CODEC_AVAILABLE 1
#else
  #define IDOTMATRIX_AUDIO_CODEC_AVAILABLE 0
#endif

// -----------------------------------------------------------------------------
// Optional buzzer backend
//
// ACTIVE drives a self-oscillating buzzer with a static GPIO level.
// PASSIVE uses the ESP32 LEDC peripheral to generate a 50% square wave.
// Build profiles may provide DEFAULT_* values; IDotMatrixUserConfig.h has
// precedence when explicit IDOTMATRIX_BUZZER_* values are present.
// -----------------------------------------------------------------------------
#define IDOTMATRIX_BUZZER_NONE    0
#define IDOTMATRIX_BUZZER_ACTIVE  1
#define IDOTMATRIX_BUZZER_PASSIVE 2

#ifdef IDOTMATRIX_BUZZER_TYPE
  #define IDOTMATRIX_BUZZER_TYPE_EXPLICIT 1
#else
  #define IDOTMATRIX_BUZZER_TYPE_EXPLICIT 0
#endif

#ifndef IDOTMATRIX_BUZZER_TYPE
  #ifdef IDOTMATRIX_DEFAULT_BUZZER_TYPE
    #define IDOTMATRIX_BUZZER_TYPE IDOTMATRIX_DEFAULT_BUZZER_TYPE
  #else
    #define IDOTMATRIX_BUZZER_TYPE IDOTMATRIX_BUZZER_NONE
  #endif
#endif

#ifndef IDOTMATRIX_BUZZER_PIN
  #ifdef IDOTMATRIX_DEFAULT_BUZZER_PIN
    #define IDOTMATRIX_BUZZER_PIN IDOTMATRIX_DEFAULT_BUZZER_PIN
  #else
    #define IDOTMATRIX_BUZZER_PIN -1
  #endif
#endif

#ifndef IDOTMATRIX_BUZZER_FREQUENCY_HZ
  #ifdef IDOTMATRIX_DEFAULT_BUZZER_FREQUENCY_HZ
    #define IDOTMATRIX_BUZZER_FREQUENCY_HZ IDOTMATRIX_DEFAULT_BUZZER_FREQUENCY_HZ
  #else
    #define IDOTMATRIX_BUZZER_FREQUENCY_HZ 2000
  #endif
#endif

#ifndef IDOTMATRIX_BUZZER_ACTIVE_HIGH
  #ifdef IDOTMATRIX_DEFAULT_BUZZER_ACTIVE_HIGH
    #define IDOTMATRIX_BUZZER_ACTIVE_HIGH IDOTMATRIX_DEFAULT_BUZZER_ACTIVE_HIGH
  #else
    #define IDOTMATRIX_BUZZER_ACTIVE_HIGH 1
  #endif
#endif

#ifndef IDOTMATRIX_BUZZER_PASSIVE_TRIGGER_LOW
  #ifdef IDOTMATRIX_DEFAULT_BUZZER_PASSIVE_TRIGGER_LOW
    #define IDOTMATRIX_BUZZER_PASSIVE_TRIGGER_LOW IDOTMATRIX_DEFAULT_BUZZER_PASSIVE_TRIGGER_LOW
  #else
    // Direct passive buzzers are normally driven active-high. Three-wire
    // transistor modules marked "low level trigger" must override this to 1.
    #define IDOTMATRIX_BUZZER_PASSIVE_TRIGGER_LOW 0
  #endif
#endif

#ifndef IDOTMATRIX_ALARM_BUZZER_ENABLED
  #if IDOTMATRIX_BUZZER_TYPE_EXPLICIT && IDOTMATRIX_BUZZER_TYPE == IDOTMATRIX_BUZZER_NONE && !IDOTMATRIX_AUDIO_CODEC_AVAILABLE
    #define IDOTMATRIX_ALARM_BUZZER_ENABLED 0
  #elif defined(IDOTMATRIX_DEFAULT_ALARM_BUZZER_ENABLED)
    #define IDOTMATRIX_ALARM_BUZZER_ENABLED IDOTMATRIX_DEFAULT_ALARM_BUZZER_ENABLED
  #else
    #define IDOTMATRIX_ALARM_BUZZER_ENABLED 0
  #endif
#endif

#ifndef IDOTMATRIX_COUNTDOWN_BUZZER_ENABLED
  #if IDOTMATRIX_BUZZER_TYPE_EXPLICIT && IDOTMATRIX_BUZZER_TYPE == IDOTMATRIX_BUZZER_NONE && !IDOTMATRIX_AUDIO_CODEC_AVAILABLE
    #define IDOTMATRIX_COUNTDOWN_BUZZER_ENABLED 0
  #elif defined(IDOTMATRIX_DEFAULT_COUNTDOWN_BUZZER_ENABLED)
    #define IDOTMATRIX_COUNTDOWN_BUZZER_ENABLED IDOTMATRIX_DEFAULT_COUNTDOWN_BUZZER_ENABLED
  #else
    #define IDOTMATRIX_COUNTDOWN_BUZZER_ENABLED 0
  #endif
#endif

#ifndef IDOTMATRIX_SCHEDULE_BUZZER_ENABLED
  #if IDOTMATRIX_BUZZER_TYPE_EXPLICIT && IDOTMATRIX_BUZZER_TYPE == IDOTMATRIX_BUZZER_NONE && !IDOTMATRIX_AUDIO_CODEC_AVAILABLE
    #define IDOTMATRIX_SCHEDULE_BUZZER_ENABLED 0
  #elif defined(IDOTMATRIX_DEFAULT_SCHEDULE_BUZZER_ENABLED)
    #define IDOTMATRIX_SCHEDULE_BUZZER_ENABLED IDOTMATRIX_DEFAULT_SCHEDULE_BUZZER_ENABLED
  #else
    #define IDOTMATRIX_SCHEDULE_BUZZER_ENABLED 0
  #endif
#endif

#ifndef IDOTMATRIX_CONNECTION_BUZZER_ENABLED
  #if IDOTMATRIX_BUZZER_TYPE_EXPLICIT && IDOTMATRIX_BUZZER_TYPE == IDOTMATRIX_BUZZER_NONE && !IDOTMATRIX_AUDIO_CODEC_AVAILABLE
    #define IDOTMATRIX_CONNECTION_BUZZER_ENABLED 0
  #elif defined(IDOTMATRIX_DEFAULT_CONNECTION_BUZZER_ENABLED)
    #define IDOTMATRIX_CONNECTION_BUZZER_ENABLED IDOTMATRIX_DEFAULT_CONNECTION_BUZZER_ENABLED
  #else
    #define IDOTMATRIX_CONNECTION_BUZZER_ENABLED 0
  #endif
#endif

#if IDOTMATRIX_BUZZER_TYPE != IDOTMATRIX_BUZZER_NONE && \
    IDOTMATRIX_BUZZER_TYPE != IDOTMATRIX_BUZZER_ACTIVE && \
    IDOTMATRIX_BUZZER_TYPE != IDOTMATRIX_BUZZER_PASSIVE
  #error "IDOTMATRIX_BUZZER_TYPE must be NONE, ACTIVE or PASSIVE"
#endif

#if IDOTMATRIX_BUZZER_TYPE == IDOTMATRIX_BUZZER_NONE
  #define IDOTMATRIX_BUZZER_AVAILABLE 0
#elif IDOTMATRIX_BUZZER_PIN < 0
  #error "A buzzer backend requires IDOTMATRIX_BUZZER_PIN >= 0"
#else
  #define IDOTMATRIX_BUZZER_AVAILABLE 1
#endif

#if IDOTMATRIX_BUZZER_TYPE == IDOTMATRIX_BUZZER_PASSIVE && IDOTMATRIX_BUZZER_FREQUENCY_HZ <= 0
  #error "Passive buzzer frequency must be greater than zero"
#endif

#if IDOTMATRIX_BUZZER_PASSIVE_TRIGGER_LOW != 0 && IDOTMATRIX_BUZZER_PASSIVE_TRIGGER_LOW != 1
  #error "IDOTMATRIX_BUZZER_PASSIVE_TRIGGER_LOW must be 0 or 1"
#endif

#if !IDOTMATRIX_BUZZER_AVAILABLE && !IDOTMATRIX_AUDIO_CODEC_AVAILABLE && \
    (IDOTMATRIX_ALARM_BUZZER_ENABLED || IDOTMATRIX_COUNTDOWN_BUZZER_ENABLED || \
     IDOTMATRIX_SCHEDULE_BUZZER_ENABLED || IDOTMATRIX_CONNECTION_BUZZER_ENABLED)
  #error "Buzzer event policy enabled but no buzzer backend/pin is configured"
#endif

// -----------------------------------------------------------------------------
// Optional real-time clock backend
//
// The current hardware backend is DS3231. It uses the already initialized
// shared TwoWire bus directly; the RTC driver never calls Wire.begin(), which
// keeps custom/shared ESP32-C3 SDA/SCL routing stable for gesture and
// accelerometer devices on the same bus.
// -----------------------------------------------------------------------------
#define IDOTMATRIX_RTC_NONE    0
#define IDOTMATRIX_RTC_DS3231  1

#ifndef IDOTMATRIX_RTC_TYPE
  #ifdef IDOTMATRIX_DEFAULT_RTC_TYPE
    #define IDOTMATRIX_RTC_TYPE IDOTMATRIX_DEFAULT_RTC_TYPE
  #else
    #define IDOTMATRIX_RTC_TYPE IDOTMATRIX_RTC_NONE
  #endif
#endif

#ifndef IDOTMATRIX_RTC_I2C_ADDRESS
  #ifdef IDOTMATRIX_DEFAULT_RTC_I2C_ADDRESS
    #define IDOTMATRIX_RTC_I2C_ADDRESS IDOTMATRIX_DEFAULT_RTC_I2C_ADDRESS
  #else
    #define IDOTMATRIX_RTC_I2C_ADDRESS 0x68
  #endif
#endif

#ifndef IDOTMATRIX_RTC_SYNC_FROM_BLE
  #ifdef IDOTMATRIX_DEFAULT_RTC_SYNC_FROM_BLE
    #define IDOTMATRIX_RTC_SYNC_FROM_BLE IDOTMATRIX_DEFAULT_RTC_SYNC_FROM_BLE
  #else
    #define IDOTMATRIX_RTC_SYNC_FROM_BLE 1
  #endif
#endif

#ifndef IDOTMATRIX_RTC_RETRY_INTERVAL_MS
  #ifdef IDOTMATRIX_DEFAULT_RTC_RETRY_INTERVAL_MS
    #define IDOTMATRIX_RTC_RETRY_INTERVAL_MS IDOTMATRIX_DEFAULT_RTC_RETRY_INTERVAL_MS
  #else
    #define IDOTMATRIX_RTC_RETRY_INTERVAL_MS 60000UL
  #endif
#endif

#ifndef IDOTMATRIX_RTC_DIAGNOSTICS
  #define IDOTMATRIX_RTC_DIAGNOSTICS 0
#endif

#if IDOTMATRIX_RTC_TYPE != IDOTMATRIX_RTC_NONE && \
    IDOTMATRIX_RTC_TYPE != IDOTMATRIX_RTC_DS3231
  #error "IDOTMATRIX_RTC_TYPE must be NONE or DS3231"
#endif

#if IDOTMATRIX_RTC_TYPE == IDOTMATRIX_RTC_NONE
  #define IDOTMATRIX_RTC_AVAILABLE 0
#else
  #define IDOTMATRIX_RTC_AVAILABLE 1
#endif

#if IDOTMATRIX_RTC_AVAILABLE && IDOTMATRIX_RTC_RETRY_INTERVAL_MS < 1000UL
  #error "IDOTMATRIX_RTC_RETRY_INTERVAL_MS must be at least 1000 ms"
#endif

#if defined(IDOTMATRIX_ACCEL_DRIVER_NONE) && (defined(IDOTMATRIX_ACCEL_DRIVER_LIS3DH) || \
    defined(IDOTMATRIX_ACCEL_DRIVER_ICM20689) || defined(IDOTMATRIX_ACCEL_DRIVER_MPU6050))
  #error "IDOTMATRIX_ACCEL_DRIVER_NONE cannot be combined with an accelerometer backend"
#endif

// -----------------------------------------------------------------------------
// Optional orientation-sensor architecture
//
// Select exactly one accelerometer backend explicitly or let the selected
// build profile provide a fallback. Any enabled accelerometer backend
// automatically enables the common orientation subsystem. Boards without a
// selected backend do not compile any sensor or orientation code.
// -----------------------------------------------------------------------------

#if (defined(IDOTMATRIX_ACCEL_DRIVER_LIS3DH) && defined(IDOTMATRIX_ACCEL_DRIVER_ICM20689)) || \
    (defined(IDOTMATRIX_ACCEL_DRIVER_LIS3DH) && defined(IDOTMATRIX_ACCEL_DRIVER_MPU6050)) || \
    (defined(IDOTMATRIX_ACCEL_DRIVER_ICM20689) && defined(IDOTMATRIX_ACCEL_DRIVER_MPU6050))
  #error "Select only one IDOTMATRIX_ACCEL_DRIVER_* backend"
#endif

#if defined(IDOTMATRIX_ACCEL_DRIVER_LIS3DH) || \
    defined(IDOTMATRIX_ACCEL_DRIVER_ICM20689) || \
    defined(IDOTMATRIX_ACCEL_DRIVER_MPU6050)
  #define IDOTMATRIX_ORIENTATION_SENSOR 1
#else
  #define IDOTMATRIX_ORIENTATION_SENSOR 0
#endif

// Optional shared I2C pin override for external sensors/RTC. Explicit local
// values win; otherwise a build profile may provide DEFAULT_* pins. Define
// complete SDA/SCL pairs only.
#if defined(IDOTMATRIX_DEFAULT_I2C_SDA_PIN) != defined(IDOTMATRIX_DEFAULT_I2C_SCL_PIN)
  #error "Define both IDOTMATRIX_DEFAULT_I2C_SDA_PIN and IDOTMATRIX_DEFAULT_I2C_SCL_PIN, or neither"
#endif

#if defined(IDOTMATRIX_I2C_SDA_PIN) != defined(IDOTMATRIX_I2C_SCL_PIN)
  #error "Define both IDOTMATRIX_I2C_SDA_PIN and IDOTMATRIX_I2C_SCL_PIN, or neither"
#endif

#if !defined(IDOTMATRIX_I2C_SDA_PIN) && defined(IDOTMATRIX_DEFAULT_I2C_SDA_PIN)
  #define IDOTMATRIX_I2C_SDA_PIN IDOTMATRIX_DEFAULT_I2C_SDA_PIN
  #define IDOTMATRIX_I2C_SCL_PIN IDOTMATRIX_DEFAULT_I2C_SCL_PIN
#endif

// DS3231 has a fixed 0x68 address. An MPU-family accelerometer explicitly
// forced to the same address cannot coexist on the shared bus. Auto-probe (0)
// remains allowed because it can discover an ICM/MPU physically strapped to 0x69.
#if IDOTMATRIX_RTC_TYPE == IDOTMATRIX_RTC_DS3231 && \
    (defined(IDOTMATRIX_ACCEL_DRIVER_ICM20689) || defined(IDOTMATRIX_ACCEL_DRIVER_MPU6050)) && \
    defined(IDOTMATRIX_ACCEL_I2C_ADDRESS) && IDOTMATRIX_ACCEL_I2C_ADDRESS == 0x68
  #error "DS3231 and MPU-family accelerometer cannot both use I2C address 0x68; strap/configure the accelerometer at 0x69"
#endif

#ifndef IDOTMATRIX_ORIENTATION_DIAGNOSTICS
  // Normal builds keep verbose sensor diagnostics disabled. A failed sensor
  // initialization still emits one concise error line; enable this flag for
  // the full probe/configuration report used during hardware qualification.
  #define IDOTMATRIX_ORIENTATION_DIAGNOSTICS 0
#endif

// Detailed sample logging is intentionally separate from normal orientation
// diagnostics. Normal builds can keep transition logs without
// printing X/Y/Z values continuously.
#ifndef IDOTMATRIX_ORIENTATION_SAMPLE_DIAGNOSTICS
  #define IDOTMATRIX_ORIENTATION_SAMPLE_DIAGNOSTICS 0
#endif

// Selecting an accelerometer backend enables automatic orientation by default.
// Set this to 0 to keep sensor classification active without rotating output.
#ifndef IDOTMATRIX_ORIENTATION_AUTO_ROTATE
  #define IDOTMATRIX_ORIENTATION_AUTO_ROTATE 1
#endif

// Clockwise planar rotation of the accelerometer PCB relative to the display.
// This is deliberately sensor/board agnostic so the same accelerometer driver
// can be reused on custom hardware or as an external module. Supported values:
// 0, 90, 180, 270. The orientation engine compensates this mounting offset
// before applying the final display rotation.
#if IDOTMATRIX_ACCEL_MOUNT_ROTATION != 0 && \
    IDOTMATRIX_ACCEL_MOUNT_ROTATION != 90 && \
    IDOTMATRIX_ACCEL_MOUNT_ROTATION != 180 && \
    IDOTMATRIX_ACCEL_MOUNT_ROTATION != 270
  #error "IDOTMATRIX_ACCEL_MOUNT_ROTATION must be 0, 90, 180 or 270"
#endif

#ifndef IDOTMATRIX_ACCEL_SAMPLE_INTERVAL_MS
  #define IDOTMATRIX_ACCEL_SAMPLE_INTERVAL_MS 100UL
#endif

#ifndef IDOTMATRIX_ORIENTATION_STABLE_MS
  #define IDOTMATRIX_ORIENTATION_STABLE_MS 600UL
#endif

#ifndef IDOTMATRIX_ORIENTATION_DIAG_INTERVAL_MS
  #define IDOTMATRIX_ORIENTATION_DIAG_INTERVAL_MS 500UL
#endif

#ifndef IDOTMATRIX_ORIENTATION_AXIS_MIN_G
  #define IDOTMATRIX_ORIENTATION_AXIS_MIN_G 0.55f
#endif

#ifndef IDOTMATRIX_ORIENTATION_AXIS_HYSTERESIS_G
  #define IDOTMATRIX_ORIENTATION_AXIS_HYSTERESIS_G 0.12f
#endif
