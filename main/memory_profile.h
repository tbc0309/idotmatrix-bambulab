#pragma once
#include "board_identity.h"
#include <cstddef>

namespace bridge::memory {
// Board capabilities, rather than board names, select shared memory budgets.
constexpr bool externalRam = board::psramBytes != 0;
#ifdef CONFIG_IDF_TARGET_ESP32C3
constexpr bool compact = true;
#else
constexpr bool compact = false;
#endif
constexpr size_t animationBytes = externalRam ? 60000 : compact ? 4000 : 18000;
constexpr size_t configBytes = externalRam ? 262144 : compact ? 6144 : 12288;
constexpr size_t telemetryBytes = externalRam ? 65536 : 16384;
constexpr unsigned mqttStack = externalRam ? 16384 : 12288;
constexpr unsigned httpStack = compact ? 8192 : 12288;
}
