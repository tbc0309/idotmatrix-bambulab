#pragma once

#include <Arduino.h>
#include "IDotMatrixHardwareConfig.h"

#if IDOTMATRIX_AUDIO_CODEC_AVAILABLE

// Minimal synthesized notification-audio backend for the Waveshare
// ESP32-S3 RGB Matrix. No PCM/WAV assets are stored: the producer generates
// the same fixed-frequency square wave used by the passive buzzer backend.
bool idotAudioOutputBegin();
void idotAudioOutputSetTone(bool on);
bool idotAudioOutputReady();

#else

static inline bool idotAudioOutputBegin() { return false; }
static inline void idotAudioOutputSetTone(bool) {}
static inline bool idotAudioOutputReady() { return false; }

#endif
