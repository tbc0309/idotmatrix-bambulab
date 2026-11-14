#pragma once

#include <stdint.h>
#include "../IDotMatrixAccelerometer.h"

#if defined(IDOTMATRIX_ACCEL_DRIVER_ICM20689) || defined(IDOTMATRIX_ACCEL_DRIVER_MPU6050)

bool idotAccelMPUFamilyBegin();
bool idotAccelMPUFamilyRead(IDotMatrixAccelSample &sample);
const char *idotAccelMPUFamilyName();
uint8_t idotAccelMPUFamilyWhoAmI();
void idotAccelMPUFamilyPrintDiagnostics();

#endif
