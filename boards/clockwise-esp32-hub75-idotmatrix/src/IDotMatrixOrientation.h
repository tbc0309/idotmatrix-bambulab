#pragma once

#include <Arduino.h>
#include "IDotMatrixHardwareConfig.h"

#if IDOTMATRIX_ORIENTATION_SENSOR

enum class IDotMatrixGravityDirection : uint8_t {
  Unknown = 0,
  PositiveX,
  NegativeX,
  PositiveY,
  NegativeY
};

enum class IDotMatrixDisplayRotation : uint8_t {
  Deg0 = 0,
  Deg90 = 1,
  Deg180 = 2,
  Deg270 = 3
};

bool idotOrientationBegin();
// Returns true only when the effective automatic display rotation changes.
bool idotOrientationUpdate(uint32_t nowMs);
bool idotOrientationReady();
void idotOrientationPrintDiagnostics();
IDotMatrixGravityDirection idotOrientationStableDirection();
IDotMatrixDisplayRotation idotOrientationDisplayRotation();
const char *idotOrientationDirectionName(IDotMatrixGravityDirection direction);
const char *idotOrientationRotationName(IDotMatrixDisplayRotation rotation);

// Map one physical output pixel to the source pixel that keeps the rendered
// image upright for the detected panel orientation. 90/270 degree rotation is
// defined for square output surfaces; rectangular outputs fall back to 0/180.
void idotOrientationMapOutputToSource(
  uint16_t outputX,
  uint16_t outputY,
  uint16_t width,
  uint16_t height,
  uint16_t &sourceX,
  uint16_t &sourceY
);

#endif
