#include "IDotMatrixAccelLIS3DH.h"

#if defined(IDOTMATRIX_ACCEL_DRIVER_LIS3DH)

#include <Adafruit_LIS3DH.h>
#include <Adafruit_Sensor.h>

#ifndef IDOTMATRIX_ACCEL_I2C_ADDRESS
  #define IDOTMATRIX_ACCEL_I2C_ADDRESS 0x19
#endif

static Adafruit_LIS3DH idotLis3dh;

bool idotAccelLIS3DHBegin() {
  if (!idotLis3dh.begin(IDOTMATRIX_ACCEL_I2C_ADDRESS)) return false;
  idotLis3dh.setRange(LIS3DH_RANGE_2_G);
  idotLis3dh.setDataRate(LIS3DH_DATARATE_50_HZ);
  return true;
}

bool idotAccelLIS3DHRead(IDotMatrixAccelSample &sample) {
  sensors_event_t event;
  idotLis3dh.getEvent(&event);
  constexpr float kGravity = SENSORS_GRAVITY_STANDARD;
  if (kGravity <= 0.0f) return false;
  sample.xG = event.acceleration.x / kGravity;
  sample.yG = event.acceleration.y / kGravity;
  sample.zG = event.acceleration.z / kGravity;
  return true;
}


void idotAccelLIS3DHPrintDiagnostics() {
#if IDOTMATRIX_ORIENTATION_DIAGNOSTICS
  Serial.print("ACCEL CONFIG: backend=LIS3DH address=0x");
  Serial.println((uint8_t)IDOTMATRIX_ACCEL_I2C_ADDRESS, HEX);
#endif
}

#endif
