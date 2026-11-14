#include "IDotMatrixRtc.h"

namespace {
constexpr uint8_t DS3231_REG_SECONDS = 0x00;
constexpr uint8_t DS3231_REG_STATUS = 0x0F;
constexpr uint8_t DS3231_STATUS_OSF = 0x80;
}

bool IDotMatrixRtc::begin(TwoWire &wire, uint8_t address) {
  _wire = &wire;
  _address = address;
  _ready = false;
  _timeValid = false;
  _status = 0xFF;
  _error = IDotMatrixRtcError::NONE;

  if (!probe()) {
    _error = IDotMatrixRtcError::NO_RESPONSE;
    return false;
  }
  _ready = true;

  if (!refreshStatus()) {
    _error = IDotMatrixRtcError::STATUS_READ_FAILED;
    return true;
  }

  IDotMatrixRtcDateTime now;
  if (!read(now)) {
    if (_error == IDotMatrixRtcError::NONE) _error = IDotMatrixRtcError::TIME_READ_FAILED;
    _timeValid = false;
    return true;
  }

  _timeValid = (_status & DS3231_STATUS_OSF) == 0;
  if (_timeValid) _error = IDotMatrixRtcError::NONE;
  return true;
}

bool IDotMatrixRtc::probe() {
  if (!_wire || !_address) return false;
  _wire->beginTransmission(_address);
  return _wire->endTransmission() == 0;
}

bool IDotMatrixRtc::read(IDotMatrixRtcDateTime &value) {
  if (!_ready || !_wire || !_address) return false;

  uint8_t data[7];
  if (!readRegisters(DS3231_REG_SECONDS, data, sizeof(data))) {
    _error = IDotMatrixRtcError::TIME_READ_FAILED;
    return false;
  }

  value.second = bcdToBin(data[0] & 0x7F);
  value.minute = bcdToBin(data[1] & 0x7F);

  if (data[2] & 0x40) {
    // 12-hour mode. Bit 5 is PM, bits 4..0 hold 1..12.
    uint8_t hour12 = bcdToBin(data[2] & 0x1F);
    bool pm = (data[2] & 0x20) != 0;
    if (hour12 == 12) hour12 = 0;
    value.hour = hour12 + (pm ? 12 : 0);
  } else {
    value.hour = bcdToBin(data[2] & 0x3F);
  }

  value.day = bcdToBin(data[4] & 0x3F);
  const bool century = (data[5] & 0x80) != 0;
  value.month = bcdToBin(data[5] & 0x1F);
  value.year = uint16_t(2000U + bcdToBin(data[6]) + (century ? 100U : 0U));

  if (!validDateTime(value)) {
    _error = IDotMatrixRtcError::INVALID_TIME;
    _timeValid = false;
    return false;
  }

  _error = IDotMatrixRtcError::NONE;
  return true;
}

bool IDotMatrixRtc::adjust(const IDotMatrixRtcDateTime &value) {
  if (!_ready || !_wire || !_address || !validDateTime(value)) {
    _error = IDotMatrixRtcError::INVALID_TIME;
    return false;
  }

  uint16_t yearOffset = value.year - 2000U;
  uint8_t month = binToBcd(value.month);
  if (yearOffset >= 100U) {
    month |= 0x80;
    yearOffset -= 100U;
  }

  const uint8_t data[7] = {
    binToBcd(value.second),
    binToBcd(value.minute),
    binToBcd(value.hour),       // force 24-hour mode
    binToBcd(dayOfWeek(value)), // DS3231 accepts 1..7; semantics are user-defined
    binToBcd(value.day),
    month,
    binToBcd(uint8_t(yearOffset)),
  };

  if (!writeRegisters(DS3231_REG_SECONDS, data, sizeof(data))) {
    _error = IDotMatrixRtcError::WRITE_FAILED;
    return false;
  }

  uint8_t status = 0;
  if (!readRegister(DS3231_REG_STATUS, status) ||
      !writeRegister(DS3231_REG_STATUS, uint8_t(status & ~DS3231_STATUS_OSF))) {
    _error = IDotMatrixRtcError::WRITE_FAILED;
    return false;
  }

  _status = uint8_t(status & ~DS3231_STATUS_OSF);
  _timeValid = true;
  _error = IDotMatrixRtcError::NONE;
  return true;
}

const char *IDotMatrixRtc::errorText() const {
  switch (_error) {
    case IDotMatrixRtcError::NO_RESPONSE: return "no I2C response";
    case IDotMatrixRtcError::STATUS_READ_FAILED: return "status read failed";
    case IDotMatrixRtcError::TIME_READ_FAILED: return "time read failed";
    case IDotMatrixRtcError::INVALID_TIME: return "invalid RTC date/time";
    case IDotMatrixRtcError::WRITE_FAILED: return "RTC write failed";
    case IDotMatrixRtcError::NONE:
    default: return "OK";
  }
}

bool IDotMatrixRtc::readRegister(uint8_t reg, uint8_t &value) {
  return readRegisters(reg, &value, 1);
}

bool IDotMatrixRtc::readRegisters(uint8_t reg, uint8_t *data, size_t len) {
  if (!_wire || !_address || !data || !len) return false;
  _wire->beginTransmission(_address);
  _wire->write(reg);
  if (_wire->endTransmission(false) != 0) return false;

  const size_t received = _wire->requestFrom(_address, static_cast<uint8_t>(len), true);
  if (received != len) {
    while (_wire->available()) (void)_wire->read();
    return false;
  }
  for (size_t i = 0; i < len; ++i) data[i] = static_cast<uint8_t>(_wire->read());
  return true;
}

bool IDotMatrixRtc::writeRegister(uint8_t reg, uint8_t value) {
  return writeRegisters(reg, &value, 1);
}

bool IDotMatrixRtc::writeRegisters(uint8_t reg, const uint8_t *data, size_t len) {
  if (!_wire || !_address || !data || !len) return false;
  _wire->beginTransmission(_address);
  _wire->write(reg);
  for (size_t i = 0; i < len; ++i) _wire->write(data[i]);
  return _wire->endTransmission() == 0;
}

bool IDotMatrixRtc::refreshStatus() {
  uint8_t status = 0;
  if (!readRegister(DS3231_REG_STATUS, status)) return false;
  _status = status;
  return true;
}

uint8_t IDotMatrixRtc::bcdToBin(uint8_t value) {
  return uint8_t(value - 6U * (value >> 4));
}

uint8_t IDotMatrixRtc::binToBcd(uint8_t value) {
  return uint8_t(value + 6U * (value / 10U));
}

bool IDotMatrixRtc::validDateTime(const IDotMatrixRtcDateTime &value) {
  if (value.year < 2000 || value.year > 2199 || value.month < 1 || value.month > 12 ||
      value.hour > 23 || value.minute > 59 || value.second > 59) return false;

  static const uint8_t days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
  uint8_t dim = days[value.month - 1];
  const bool leap = (value.year % 4U == 0U && value.year % 100U != 0U) || (value.year % 400U == 0U);
  if (value.month == 2 && leap) dim = 29;
  return value.day >= 1 && value.day <= dim;
}

uint8_t IDotMatrixRtc::dayOfWeek(const IDotMatrixRtcDateTime &value) {
  // Sakamoto: 0=Sunday. DS3231 accepts any 1..7 convention; store Sunday=1.
  static const uint8_t t[] = {0,3,2,5,0,3,5,1,4,6,2,4};
  uint16_t y = value.year;
  if (value.month < 3) --y;
  uint8_t dow = uint8_t((y + y/4U - y/100U + y/400U + t[value.month-1] + value.day) % 7U);
  return uint8_t(dow + 1U);
}
