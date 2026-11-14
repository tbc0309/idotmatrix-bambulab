#pragma once

// Local hardware overrides for iDotMatrix ESP32 Emulator.
//
// Copy this file to:
//   src/IDotMatrixUserConfig.h
//
// The copied file is intentionally ignored by Git and preserved by the
// repository update helper. Uncomment only the settings you need. Explicit
// values here take precedence over the corresponding PlatformIO profile
// defaults. PlatformIO supplies those profile defaults at build time; Arduino
// IDE does not, so copy/uncomment the complete hardware settings needed by the
// target when compiling directly with Arduino IDE.

// -----------------------------------------------------------------------------
// Accelerometer backend -- select at most one.
// Use NONE to suppress a backend supplied by the selected PlatformIO profile.
// -----------------------------------------------------------------------------
// #define IDOTMATRIX_ACCEL_DRIVER_NONE
// #define IDOTMATRIX_ACCEL_DRIVER_LIS3DH
// #define IDOTMATRIX_ACCEL_DRIVER_ICM20689
// #define IDOTMATRIX_ACCEL_DRIVER_MPU6050

// -----------------------------------------------------------------------------
// External/shared I2C bus pins. Define both or neither.
// -----------------------------------------------------------------------------
// #define IDOTMATRIX_I2C_SDA_PIN 8
// #define IDOTMATRIX_I2C_SCL_PIN 9

// -----------------------------------------------------------------------------
// Accelerometer I2C address.
//
// LIS3DH normally uses 0x19 (0x18 is also possible).
// ICM-20689 / MPU-6050 may use 0x68 or 0x69. Value 0 enables auto-probe for
// the MPU-family backend.
// -----------------------------------------------------------------------------
// #define IDOTMATRIX_ACCEL_I2C_ADDRESS 0


// -----------------------------------------------------------------------------
// Optional RTC configuration.
//
// The current backend is DS3231. On the qualified ESP32-C3 profile it shares
// the same I2C bus on SDA=GPIO1 / SCL=GPIO2. DS3231 uses address 0x68.
// App time synchronization updates the RTC by default, so an invalid/lost-power
// clock is repaired automatically when the app sends its normal time packet.
// -----------------------------------------------------------------------------
// #define IDOTMATRIX_RTC_TYPE IDOTMATRIX_RTC_DS3231
// #define IDOTMATRIX_RTC_I2C_ADDRESS 0x68
// #define IDOTMATRIX_RTC_SYNC_FROM_BLE 1
// #define IDOTMATRIX_RTC_RETRY_INTERVAL_MS 60000UL
// #define IDOTMATRIX_RTC_DIAGNOSTICS 1

// -----------------------------------------------------------------------------
// Orientation diagnostics.
//
// Verbose probe/configuration diagnostics and continuous XYZ samples are
// disabled by default after ICM-20689 qualification. Enable them only for
// targeted bring-up or troubleshooting. A failed sensor initialization still
// emits one concise error line in a normal build.
// -----------------------------------------------------------------------------
// #define IDOTMATRIX_ORIENTATION_DIAGNOSTICS 1
// #define IDOTMATRIX_ORIENTATION_SAMPLE_DIAGNOSTICS 1

// -----------------------------------------------------------------------------
// Clockwise planar mounting compensation: 0, 90, 180 or 270 degrees.
// -----------------------------------------------------------------------------
// #define IDOTMATRIX_ACCEL_MOUNT_ROTATION 0


// -----------------------------------------------------------------------------
// Optional buzzer configuration.
//
// ACTIVE is for self-oscillating modules; PASSIVE generates a PWM tone through
// the ESP32 LEDC peripheral. The ESP32-C3 16x16 profile defaults to a passive
// low-level-trigger buzzer module on GPIO3, but every value can be overridden here.
// -----------------------------------------------------------------------------
// #define IDOTMATRIX_BUZZER_TYPE IDOTMATRIX_BUZZER_PASSIVE
// #define IDOTMATRIX_BUZZER_PIN 3
// #define IDOTMATRIX_BUZZER_FREQUENCY_HZ 2000
// #define IDOTMATRIX_BUZZER_PASSIVE_TRIGGER_LOW 1  // three-wire transistor module marked "low level trigger"
// #define IDOTMATRIX_BUZZER_ACTIVE_HIGH 1  // active buzzer only
//
// #define IDOTMATRIX_ALARM_BUZZER_ENABLED 1
// #define IDOTMATRIX_COUNTDOWN_BUZZER_ENABLED 1
// #define IDOTMATRIX_SCHEDULE_BUZZER_ENABLED 1
// #define IDOTMATRIX_CONNECTION_BUZZER_ENABLED 1

// -----------------------------------------------------------------------------

// Optional synthesized codec-audio output (Waveshare ES8311 profile).
// No WAV/PCM assets are used; notification tones are generated at runtime.
// #define IDOTMATRIX_AUDIO_CODEC_ENABLED 1
// #define IDOTMATRIX_AUDIO_CODEC_I2C_ADDRESS 0x18
// #define IDOTMATRIX_AUDIO_I2C_SDA_PIN 47
// #define IDOTMATRIX_AUDIO_I2C_SCL_PIN 48
// #define IDOTMATRIX_AUDIO_I2S_BCLK_PIN 43
// #define IDOTMATRIX_AUDIO_I2S_WS_PIN 38
// #define IDOTMATRIX_AUDIO_I2S_DOUT_PIN 21
// #define IDOTMATRIX_AUDIO_I2S_MCLK_PIN 12
// #define IDOTMATRIX_AUDIO_PA_ENABLE_PIN 11
// #define IDOTMATRIX_AUDIO_CODEC_VOLUME 100

// Optional OTA maintenance override.
//
// The Waveshare 0.6.0 profile enables OTA by default and uses the
// on-board BOOT button (GPIO0). Hold the button for about two seconds while the
// firmware is already running to start a temporary Wi-Fi AP and HTTP upload
// page. A short press/release before the hold threshold performs a software
// reboot, which is useful when a serial monitor must stay attached. The AP is never started during normal operation unless this physical
// trigger is used.
// -----------------------------------------------------------------------------
// #define IDOTMATRIX_OTA_ENABLED 1
// #define IDOTMATRIX_OTA_TRIGGER_PIN 0
// #define IDOTMATRIX_OTA_TRIGGER_ACTIVE_LOW 1
// #define IDOTMATRIX_OTA_TRIGGER_HOLD_MS 2000UL
// #define IDOTMATRIX_OTA_TRIGGER_DEBOUNCE_MS 40UL
// #define IDOTMATRIX_OTA_AP_PASSWORD "idotmatrix"  // minimum 8 characters
