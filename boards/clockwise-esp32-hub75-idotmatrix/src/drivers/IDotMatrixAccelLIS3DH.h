#pragma once

#include "../IDotMatrixAccelerometer.h"

#if defined(IDOTMATRIX_ACCEL_DRIVER_LIS3DH)

bool idotAccelLIS3DHBegin();
bool idotAccelLIS3DHRead(IDotMatrixAccelSample &sample);
void idotAccelLIS3DHPrintDiagnostics();

#endif
