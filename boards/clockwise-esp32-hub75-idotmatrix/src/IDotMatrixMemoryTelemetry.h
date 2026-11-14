#pragma once

#include <Arduino.h>

#ifndef IDOTMATRIX_MEMORY_TELEMETRY
#define IDOTMATRIX_MEMORY_TELEMETRY 0
#endif

#if IDOTMATRIX_MEMORY_TELEMETRY

void idotMemoryTelemetrySnapshot(const char *tag);
void idotMemoryTelemetryLatency(const char *tag, uint32_t elapsedUs);

#else

inline void idotMemoryTelemetrySnapshot(const char *) {}
inline void idotMemoryTelemetryLatency(const char *, uint32_t) {}

#endif
