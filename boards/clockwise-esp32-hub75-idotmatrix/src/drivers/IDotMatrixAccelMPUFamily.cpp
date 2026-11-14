#include "IDotMatrixAccelMPUFamily.h"

#if defined(IDOTMATRIX_ACCEL_DRIVER_ICM20689) || defined(IDOTMATRIX_ACCEL_DRIVER_MPU6050)

#include <Arduino.h>
#include <Wire.h>

namespace {

constexpr uint8_t kAddressPrimary = 0x68;
constexpr uint8_t kAddressSecondary = 0x69;

constexpr uint8_t kRegSampleRateDivider = 0x19;
constexpr uint8_t kRegConfig = 0x1A;
constexpr uint8_t kRegAccelConfig = 0x1C;
constexpr uint8_t kRegAccelConfig2 = 0x1D;
constexpr uint8_t kRegAccelXoutH = 0x3B;
constexpr uint8_t kRegPowerMgmt1 = 0x6B;
constexpr uint8_t kRegWhoAmI = 0x75;

constexpr uint8_t kWhoAmIICM20689 = 0x98;
constexpr uint8_t kWhoAmIMPU6050Primary = 0x68;
constexpr uint8_t kWhoAmIMPU6050Secondary = 0x69;

#ifndef IDOTMATRIX_ACCEL_I2C_ADDRESS
  #define IDOTMATRIX_ACCEL_I2C_ADDRESS 0
#endif

struct ProbeTrace {
  bool attempted = false;
  bool ack = false;
  bool whoRead = false;
  uint8_t who = 0;
  bool matched = false;
};

uint8_t sensorAddress = 0;
uint8_t sensorWhoAmI = 0;
uint8_t lastProbeAddress = 0;
bool sensorReady = false;
bool configAttempted = false;
bool configOk = false;
ProbeTrace probe68;
ProbeTrace probe69;

ProbeTrace &traceFor(uint8_t address) {
  return address == kAddressSecondary ? probe69 : probe68;
}

bool readRegisters(uint8_t reg, uint8_t *data, size_t len) {
  if (!sensorAddress || !data || !len) return false;

  Wire.beginTransmission(sensorAddress);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;

  const size_t received = Wire.requestFrom(sensorAddress, static_cast<uint8_t>(len), true);
  if (received != len) {
    while (Wire.available()) (void)Wire.read();
    return false;
  }

  for (size_t i = 0; i < len; ++i) data[i] = static_cast<uint8_t>(Wire.read());
  return true;
}

bool readRegister(uint8_t reg, uint8_t &value) {
  return readRegisters(reg, &value, 1);
}

bool writeRegister(uint8_t reg, uint8_t value) {
  if (!sensorAddress) return false;
  Wire.beginTransmission(sensorAddress);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

bool verifyRegisterMasked(uint8_t reg, uint8_t mask, uint8_t expected) {
  uint8_t value = 0;
  return readRegister(reg, value) && ((value & mask) == (expected & mask));
}

bool expectedWhoAmI(uint8_t who) {
#if defined(IDOTMATRIX_ACCEL_DRIVER_ICM20689)
  return who == kWhoAmIICM20689;
#elif defined(IDOTMATRIX_ACCEL_DRIVER_MPU6050)
  return who == kWhoAmIMPU6050Primary || who == kWhoAmIMPU6050Secondary;
#else
  (void)who;
  return false;
#endif
}

bool probeAddress(uint8_t address) {
  ProbeTrace &trace = traceFor(address);
  trace = ProbeTrace{};
  trace.attempted = true;

  Wire.beginTransmission(address);
  trace.ack = (Wire.endTransmission() == 0);
  if (!trace.ack) return false;

  lastProbeAddress = address;
  const uint8_t previousAddress = sensorAddress;
  sensorAddress = address;
  uint8_t who = 0;
  trace.whoRead = readRegister(kRegWhoAmI, who);
  if (trace.whoRead) {
    trace.who = who;
    sensorWhoAmI = who;
  }
  trace.matched = trace.whoRead && expectedWhoAmI(who);
  if (trace.matched) return true;

  sensorAddress = previousAddress;
  return false;
}

bool selectAddress() {
#if IDOTMATRIX_ACCEL_I2C_ADDRESS != 0
  return probeAddress(static_cast<uint8_t>(IDOTMATRIX_ACCEL_I2C_ADDRESS));
#else
  // DS3231 is fixed at 0x68. When that RTC backend is enabled, try the
  // MPU-family secondary address first so a shared bus does not needlessly
  // read an unrelated DS3231 register during normal auto-probe.
#if IDOTMATRIX_RTC_TYPE == IDOTMATRIX_RTC_DS3231
  if (probeAddress(kAddressSecondary)) return true;
  sensorAddress = 0;
  if (probeAddress(kAddressPrimary)) return true;
#else
  if (probeAddress(kAddressPrimary)) return true;
  sensorAddress = 0;
  if (probeAddress(kAddressSecondary)) return true;
#endif
  sensorAddress = 0;
  return false;
#endif
}

bool configureSensor() {
  // Wake the device and select the X-axis gyro PLL clock source.
  if (!writeRegister(kRegPowerMgmt1, 0x01)) return false;
  delay(10);

  // 50 Hz output rate and +/-2 g accelerometer range. ICM-20689 has a
  // dedicated accelerometer DLPF register at 0x1D; MPU-6050 does not.
  if (!writeRegister(kRegConfig, 0x03) ||
      !writeRegister(kRegSampleRateDivider, 19) ||
      !writeRegister(kRegAccelConfig, 0x00) ||
      (sensorWhoAmI == kWhoAmIICM20689 && !writeRegister(kRegAccelConfig2, 0x03))) {
    return false;
  }
  delay(5);

  // Read back the configuration. This rejects devices that merely ACK the I2C
  // address but do not behave like the selected sensor family.
  if (!verifyRegisterMasked(kRegPowerMgmt1, 0x47, 0x01) ||
      !verifyRegisterMasked(kRegConfig, 0x07, 0x03) ||
      !verifyRegisterMasked(kRegSampleRateDivider, 0xFF, 19) ||
      !verifyRegisterMasked(kRegAccelConfig, 0x18, 0x00) ||
      (sensorWhoAmI == kWhoAmIICM20689 && !verifyRegisterMasked(kRegAccelConfig2, 0x0F, 0x03))) {
    return false;
  }

  return true;
}

} // namespace

bool idotAccelMPUFamilyBegin() {
  sensorAddress = 0;
  sensorWhoAmI = 0;
  lastProbeAddress = 0;
  sensorReady = false;
  configAttempted = false;
  configOk = false;
  probe68 = ProbeTrace{};
  probe69 = ProbeTrace{};

  if (!selectAddress()) {
#if IDOTMATRIX_ORIENTATION_DIAGNOSTICS
    Serial.print("ORIENTATION SENSOR PROBE: FAILED");
    if (lastProbeAddress) {
      Serial.print(" addr=0x"); Serial.print(lastProbeAddress, HEX);
      Serial.print(" who=0x"); Serial.print(sensorWhoAmI, HEX);
    }
    Serial.println();
#endif
    return false;
  }

#if IDOTMATRIX_ORIENTATION_DIAGNOSTICS
  Serial.print("ORIENTATION SENSOR PROBE: addr=0x"); Serial.print(sensorAddress, HEX);
  Serial.print(" who=0x"); Serial.println(sensorWhoAmI, HEX);
#endif

  configAttempted = true;
  sensorReady = configureSensor();
  configOk = sensorReady;
#if IDOTMATRIX_ORIENTATION_DIAGNOSTICS
  if (!sensorReady) Serial.println("ORIENTATION SENSOR CONFIG: FAILED");
#endif
  return sensorReady;
}

bool idotAccelMPUFamilyRead(IDotMatrixAccelSample &sample) {
  if (!sensorReady) return false;

  uint8_t data[6];
  if (!readRegisters(kRegAccelXoutH, data, sizeof(data))) return false;

  const int16_t rawX = static_cast<int16_t>((uint16_t(data[0]) << 8) | data[1]);
  const int16_t rawY = static_cast<int16_t>((uint16_t(data[2]) << 8) | data[3]);
  const int16_t rawZ = static_cast<int16_t>((uint16_t(data[4]) << 8) | data[5]);
  constexpr float kScale = 1.0f / 16384.0f; // +/-2 g
  sample.xG = rawX * kScale;
  sample.yG = rawY * kScale;
  sample.zG = rawZ * kScale;
  return true;
}

const char *idotAccelMPUFamilyName() {
  if (sensorWhoAmI == kWhoAmIICM20689) return "ICM-20689";
  if (sensorWhoAmI == kWhoAmIMPU6050Primary || sensorWhoAmI == kWhoAmIMPU6050Secondary) return "MPU-6050";
#if defined(IDOTMATRIX_ACCEL_DRIVER_ICM20689)
  return "ICM-20689";
#else
  return "MPU-6050";
#endif
}

uint8_t idotAccelMPUFamilyWhoAmI() {
  return sensorWhoAmI;
}


void idotAccelMPUFamilyPrintDiagnostics() {
#if IDOTMATRIX_ORIENTATION_DIAGNOSTICS
  Serial.print("ACCEL CONFIG: backend="); Serial.print(idotAccelMPUFamilyName());
  Serial.print(" requestedAddress=");
#if IDOTMATRIX_ACCEL_I2C_ADDRESS == 0
  #if IDOTMATRIX_RTC_TYPE == IDOTMATRIX_RTC_DS3231
    Serial.println("auto(0x69,0x68; DS3231 reserves 0x68)");
  #else
    Serial.println("auto(0x68,0x69)");
  #endif
#else
  Serial.print("0x"); Serial.println((uint8_t)IDOTMATRIX_ACCEL_I2C_ADDRESS, HEX);
#endif

  auto printProbe = [](uint8_t address, const ProbeTrace &trace) {
    Serial.print("ACCEL PROBE 0x"); Serial.print(address, HEX); Serial.print(": ");
    if (!trace.attempted) { Serial.println("not attempted"); return; }
    Serial.print("ACK="); Serial.print(trace.ack ? "YES" : "NO");
    if (trace.ack) {
      Serial.print(" WHO_AM_I=");
      if (trace.whoRead) { Serial.print("0x"); Serial.print(trace.who, HEX); }
      else Serial.print("READ_FAILED");
      Serial.print(" MATCH="); Serial.print(trace.matched ? "YES" : "NO");
    }
    Serial.println();
  };

  printProbe(kAddressPrimary, probe68);
  printProbe(kAddressSecondary, probe69);
  Serial.print("ACCEL RESULT: ready="); Serial.print(sensorReady ? "YES" : "NO");
  Serial.print(" activeAddress=");
  if (sensorAddress) { Serial.print("0x"); Serial.print(sensorAddress, HEX); }
  else Serial.print("none");
  Serial.print(" who=");
  if (sensorWhoAmI) { Serial.print("0x"); Serial.print(sensorWhoAmI, HEX); }
  else Serial.print("unknown");
  Serial.print(" config=");
  if (!configAttempted) Serial.println("not attempted");
  else Serial.println(configOk ? "OK" : "FAILED");
#endif
}

#endif
