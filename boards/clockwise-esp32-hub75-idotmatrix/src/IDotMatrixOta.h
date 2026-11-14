#pragma once

#include <Arduino.h>
#include "IDotMatrixHardwareConfig.h"

#if IDOTMATRIX_OTA_AVAILABLE

void idotOtaBegin(const char *release, uint32_t build);
void idotOtaLoop(uint32_t nowMs);
bool idotOtaIsActive();
const char *idotOtaStateText();

#else

inline void idotOtaBegin(const char *, uint32_t) {}
inline void idotOtaLoop(uint32_t) {}
inline bool idotOtaIsActive() { return false; }
inline const char *idotOtaStateText() { return "disabled"; }

#endif
