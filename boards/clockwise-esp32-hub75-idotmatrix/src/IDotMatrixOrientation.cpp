#include "IDotMatrixOrientation.h"

#if IDOTMATRIX_ORIENTATION_SENSOR

#include <math.h>
#include "IDotMatrixAccelerometer.h"

namespace {

bool sensorReady = false;
uint32_t lastSampleAt = 0;
#if IDOTMATRIX_ORIENTATION_SAMPLE_DIAGNOSTICS
uint32_t lastDiagAt = 0;
#endif
IDotMatrixGravityDirection candidateDirection = IDotMatrixGravityDirection::Unknown;
IDotMatrixGravityDirection stableDirection = IDotMatrixGravityDirection::Unknown;
IDotMatrixDisplayRotation displayRotation = IDotMatrixDisplayRotation::Deg0;
uint32_t candidateSince = 0;
IDotMatrixAccelSample lastSample;

float absf(float value) {
  return value < 0.0f ? -value : value;
}

IDotMatrixGravityDirection classifyGravity(const IDotMatrixAccelSample &sample) {
  const float ax = absf(sample.xG);
  const float ay = absf(sample.yG);
  const float strongest = ax > ay ? ax : ay;
  if (strongest < IDOTMATRIX_ORIENTATION_AXIS_MIN_G) {
    return IDotMatrixGravityDirection::Unknown;
  }

  // Near the diagonal, preserve the current candidate/stable axis while it is
  // still plausible. This prevents repeated orientation changes around 45 deg.
  if (absf(ax - ay) < IDOTMATRIX_ORIENTATION_AXIS_HYSTERESIS_G) {
    if (candidateDirection != IDotMatrixGravityDirection::Unknown) return candidateDirection;
    if (stableDirection != IDotMatrixGravityDirection::Unknown) return stableDirection;
  }

  if (ax >= ay) {
    return sample.xG >= 0.0f ? IDotMatrixGravityDirection::PositiveX
                             : IDotMatrixGravityDirection::NegativeX;
  }
  return sample.yG >= 0.0f ? IDotMatrixGravityDirection::PositiveY
                            : IDotMatrixGravityDirection::NegativeY;
}

IDotMatrixDisplayRotation rotationForDirection(IDotMatrixGravityDirection direction) {
  // Common normalized sensor convention validated with the MatrixPortal S3:
  // upright=+Y, 90 CW=+X, 180=-Y, 270 CW=-X. Drivers expose normalized
  // X/Y/Z samples; mounting compensation is applied separately below.
  switch (direction) {
    case IDotMatrixGravityDirection::PositiveX: return IDotMatrixDisplayRotation::Deg90;
    case IDotMatrixGravityDirection::NegativeY: return IDotMatrixDisplayRotation::Deg180;
    case IDotMatrixGravityDirection::NegativeX: return IDotMatrixDisplayRotation::Deg270;
    case IDotMatrixGravityDirection::PositiveY:
    default: return IDotMatrixDisplayRotation::Deg0;
  }
}

IDotMatrixDisplayRotation applyMountRotation(IDotMatrixDisplayRotation detected) {
  // IDOTMATRIX_ACCEL_MOUNT_ROTATION describes how far the sensor PCB itself is
  // mounted clockwise relative to the display. A clockwise-mounted sensor makes
  // the raw orientation appear clockwise by the same amount, therefore subtract
  // the mounting offset from the detected rotation.
  constexpr uint8_t mountSteps = (IDOTMATRIX_ACCEL_MOUNT_ROTATION / 90) & 0x03;
  const uint8_t detectedSteps = static_cast<uint8_t>(detected) & 0x03;
  return static_cast<IDotMatrixDisplayRotation>((detectedSteps + 4u - mountSteps) & 0x03);
}

void printSampleDiagnostics(uint32_t nowMs) {
#if IDOTMATRIX_ORIENTATION_SAMPLE_DIAGNOSTICS
  if ((uint32_t)(nowMs - lastDiagAt) < IDOTMATRIX_ORIENTATION_DIAG_INTERVAL_MS) return;
  lastDiagAt = nowMs;
  Serial.print("ORIENT sample x="); Serial.print(lastSample.xG, 3);
  Serial.print("g y="); Serial.print(lastSample.yG, 3);
  Serial.print("g z="); Serial.print(lastSample.zG, 3);
  Serial.print("g candidate="); Serial.print(idotOrientationDirectionName(candidateDirection));
  Serial.print(" stable="); Serial.println(idotOrientationDirectionName(stableDirection));
#else
  (void)nowMs;
#endif
}

} // namespace

const char *idotOrientationDirectionName(IDotMatrixGravityDirection direction) {
  switch (direction) {
    case IDotMatrixGravityDirection::PositiveX: return "+X";
    case IDotMatrixGravityDirection::NegativeX: return "-X";
    case IDotMatrixGravityDirection::PositiveY: return "+Y";
    case IDotMatrixGravityDirection::NegativeY: return "-Y";
    default: return "UNKNOWN";
  }
}

const char *idotOrientationRotationName(IDotMatrixDisplayRotation rotation) {
  switch (rotation) {
    case IDotMatrixDisplayRotation::Deg90: return "90";
    case IDotMatrixDisplayRotation::Deg180: return "180";
    case IDotMatrixDisplayRotation::Deg270: return "270";
    default: return "0";
  }
}

bool idotOrientationBegin() {
  sensorReady = idotAccelBegin();
#if IDOTMATRIX_ORIENTATION_DIAGNOSTICS
  Serial.print("ORIENTATION SENSOR: driver="); Serial.print(idotAccelDriverName());
  Serial.print(" init="); Serial.println(sensorReady ? "OK" : "FAILED");
  if (sensorReady) {
    Serial.print("ORIENTATION SENSOR MOUNT: ");
    Serial.print(IDOTMATRIX_ACCEL_MOUNT_ROTATION);
    Serial.println(" deg CW");
#if IDOTMATRIX_ORIENTATION_AUTO_ROTATE
    Serial.println("ORIENTATION AUTO-ROTATE: enabled");
#else
    Serial.println("ORIENTATION AUTO-ROTATE: disabled");
#endif
  }
#else
  if (!sensorReady) {
    Serial.print("ORIENTATION SENSOR ERROR: driver=");
    Serial.print(idotAccelDriverName());
    Serial.println(" init=FAILED; enable IDOTMATRIX_ORIENTATION_DIAGNOSTICS for details");
  }
#endif
  return sensorReady;
}

bool idotOrientationUpdate(uint32_t nowMs) {
  if (!sensorReady) return false;
  if ((uint32_t)(nowMs - lastSampleAt) < IDOTMATRIX_ACCEL_SAMPLE_INTERVAL_MS) return false;
  lastSampleAt = nowMs;

  IDotMatrixAccelSample sample;
  if (!idotAccelRead(sample)) return false;
  lastSample = sample;

  const IDotMatrixGravityDirection next = classifyGravity(sample);
  if (next != candidateDirection) {
    candidateDirection = next;
    candidateSince = nowMs;
  }

  bool rotationChanged = false;
  if (candidateDirection != IDotMatrixGravityDirection::Unknown &&
      candidateDirection != stableDirection &&
      (uint32_t)(nowMs - candidateSince) >= IDOTMATRIX_ORIENTATION_STABLE_MS) {
    stableDirection = candidateDirection;
    const IDotMatrixDisplayRotation nextRotation = applyMountRotation(rotationForDirection(stableDirection));
#if IDOTMATRIX_ORIENTATION_AUTO_ROTATE
    if (nextRotation != displayRotation) {
      displayRotation = nextRotation;
      rotationChanged = true;
#if IDOTMATRIX_ORIENTATION_DIAGNOSTICS
      Serial.print("ORIENT ROTATION -> "); Serial.println(idotOrientationRotationName(displayRotation));
#endif
    }
#else
    (void)nextRotation;
#endif
  }

  printSampleDiagnostics(nowMs);
  return rotationChanged;
}


void idotOrientationPrintDiagnostics() {
#if IDOTMATRIX_ORIENTATION_DIAGNOSTICS
  Serial.println("--- ORIENTATION DIAGNOSTIC SUMMARY ---");
  Serial.print("ORIENTATION SENSOR: driver="); Serial.print(idotAccelDriverName());
  Serial.print(" init="); Serial.println(sensorReady ? "OK" : "FAILED");
#if defined(IDOTMATRIX_I2C_SDA_PIN) && defined(IDOTMATRIX_I2C_SCL_PIN)
  Serial.print("I2C BUS: explicit pins SDA="); Serial.print(IDOTMATRIX_I2C_SDA_PIN);
  Serial.print(" SCL="); Serial.println(IDOTMATRIX_I2C_SCL_PIN);
#else
  Serial.println("I2C BUS: board-default pins");
#endif
  Serial.print("ORIENTATION SENSOR MOUNT: ");
  Serial.print(IDOTMATRIX_ACCEL_MOUNT_ROTATION); Serial.println(" deg CW");
  idotAccelPrintDiagnostics();
  Serial.println("--- END ORIENTATION DIAGNOSTIC SUMMARY ---");
#endif
}

bool idotOrientationReady() {
  return sensorReady;
}

IDotMatrixGravityDirection idotOrientationStableDirection() {
  return stableDirection;
}

IDotMatrixDisplayRotation idotOrientationDisplayRotation() {
  return displayRotation;
}

void idotOrientationMapOutputToSource(
  uint16_t outputX,
  uint16_t outputY,
  uint16_t width,
  uint16_t height,
  uint16_t &sourceX,
  uint16_t &sourceY
) {
  sourceX = outputX;
  sourceY = outputY;

#if !IDOTMATRIX_ORIENTATION_AUTO_ROTATE
  return;
#endif

  switch (displayRotation) {
    case IDotMatrixDisplayRotation::Deg90:
      // Panel rotated 90 deg clockwise: rotate the rendered image 90 deg CCW.
      if (width == height) {
        sourceX = width - 1u - outputY;
        sourceY = outputX;
      }
      break;
    case IDotMatrixDisplayRotation::Deg180:
      sourceX = width - 1u - outputX;
      sourceY = height - 1u - outputY;
      break;
    case IDotMatrixDisplayRotation::Deg270:
      // Panel rotated 270 deg clockwise: rotate the rendered image 90 deg CW.
      if (width == height) {
        sourceX = outputY;
        sourceY = height - 1u - outputX;
      }
      break;
    default:
      break;
  }
}

#endif
