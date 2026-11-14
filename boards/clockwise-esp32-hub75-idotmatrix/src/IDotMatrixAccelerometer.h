#pragma once

#include "IDotMatrixHardwareConfig.h"

#if IDOTMATRIX_ORIENTATION_SENSOR

struct IDotMatrixAccelSample {
  float xG = 0.0f;
  float yG = 0.0f;
  float zG = 0.0f;
};

bool idotAccelBegin();
bool idotAccelRead(IDotMatrixAccelSample &sample);
const char *idotAccelDriverName();
void idotAccelPrintDiagnostics();

#endif
