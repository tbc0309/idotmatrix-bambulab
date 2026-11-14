#pragma once

#include <Arduino.h>
#include <Wire.h>

struct IDotMatrixRtcDateTime {
  uint16_t year = 2000;
  uint8_t month = 1;
  uint8_t day = 1;
  uint8_t hour = 0;
  uint8_t minute = 0;
  uint8_t second = 0;
};

enum class IDotMatrixRtcError : uint8_t {
  NONE = 0,
  NO_RESPONSE,
  STATUS_READ_FAILED,
  TIME_READ_FAILED,
  INVALID_TIME,
  WRITE_FAILED,
};

class IDotMatrixRtc {
public:
  bool begin(TwoWire &wire, uint8_t address = 0x68);
  bool read(IDotMatrixRtcDateTime &value);
  bool adjust(const IDotMatrixRtcDateTime &value);

  bool ready() const { return _ready; }
  bool timeValid() const { return _timeValid; }
  uint8_t address() const { return _address; }
  uint8_t statusRegister() const { return _status; }
  IDotMatrixRtcError error() const { return _error; }
  const char *errorText() const;

private:
  TwoWire *_wire = nullptr;
  uint8_t _address = 0;
  bool _ready = false;
  bool _timeValid = false;
  uint8_t _status = 0xFF;
  IDotMatrixRtcError _error = IDotMatrixRtcError::NONE;

  bool probe();
  bool readRegister(uint8_t reg, uint8_t &value);
  bool readRegisters(uint8_t reg, uint8_t *data, size_t len);
  bool writeRegister(uint8_t reg, uint8_t value);
  bool writeRegisters(uint8_t reg, const uint8_t *data, size_t len);
  bool refreshStatus();

  static uint8_t bcdToBin(uint8_t value);
  static uint8_t binToBcd(uint8_t value);
  static bool validDateTime(const IDotMatrixRtcDateTime &value);
  static uint8_t dayOfWeek(const IDotMatrixRtcDateTime &value);
};
