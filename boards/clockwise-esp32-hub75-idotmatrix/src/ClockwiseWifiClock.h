#pragma once

#include <ctime>

namespace clockwiseWifi {
void begin(const char *deviceName, bool (*get24Hour)(), void (*set24Hour)(bool));
void loop();
bool localTime(std::tm &result);
bool clearSettings();
}
