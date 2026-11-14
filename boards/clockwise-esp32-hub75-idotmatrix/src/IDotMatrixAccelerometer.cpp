#include "IDotMatrixAccelerometer.h"

#if IDOTMATRIX_ORIENTATION_SENSOR

#if defined(IDOTMATRIX_ACCEL_DRIVER_LIS3DH)
  #include "drivers/IDotMatrixAccelLIS3DH.h"
#elif defined(IDOTMATRIX_ACCEL_DRIVER_ICM20689) || defined(IDOTMATRIX_ACCEL_DRIVER_MPU6050)
  #include "drivers/IDotMatrixAccelMPUFamily.h"
#else
  #error "IDOTMATRIX_ORIENTATION_SENSOR enabled without a supported accelerometer backend"
#endif

bool idotAccelBegin() {
#if defined(IDOTMATRIX_ACCEL_DRIVER_LIS3DH)
  return idotAccelLIS3DHBegin();
#elif defined(IDOTMATRIX_ACCEL_DRIVER_ICM20689) || defined(IDOTMATRIX_ACCEL_DRIVER_MPU6050)
  return idotAccelMPUFamilyBegin();
#endif
}

bool idotAccelRead(IDotMatrixAccelSample &sample) {
#if defined(IDOTMATRIX_ACCEL_DRIVER_LIS3DH)
  return idotAccelLIS3DHRead(sample);
#elif defined(IDOTMATRIX_ACCEL_DRIVER_ICM20689) || defined(IDOTMATRIX_ACCEL_DRIVER_MPU6050)
  return idotAccelMPUFamilyRead(sample);
#endif
}

const char *idotAccelDriverName() {
#if defined(IDOTMATRIX_ACCEL_DRIVER_LIS3DH)
  return "LIS3DH";
#elif defined(IDOTMATRIX_ACCEL_DRIVER_ICM20689) || defined(IDOTMATRIX_ACCEL_DRIVER_MPU6050)
  return idotAccelMPUFamilyName();
#else
  return "unknown";
#endif
}

void idotAccelPrintDiagnostics() {
#if defined(IDOTMATRIX_ACCEL_DRIVER_LIS3DH)
  idotAccelLIS3DHPrintDiagnostics();
#elif defined(IDOTMATRIX_ACCEL_DRIVER_ICM20689) || defined(IDOTMATRIX_ACCEL_DRIVER_MPU6050)
  idotAccelMPUFamilyPrintDiagnostics();
#endif
}

#endif
