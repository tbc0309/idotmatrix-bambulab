#pragma once

#include <stdint.h>

// Pure buzzer policy helpers. Keeping the electrical idle policy independent
// from Arduino/LEDC makes the active-low module behavior host-testable.
static inline uint32_t idotPassiveBuzzerIdleDuty(bool triggerLow, uint32_t maxDuty) {
  return triggerLow ? maxDuty : 0U;
}

static inline int idotPassiveBuzzerIdleLevel(bool triggerLow) {
  return triggerLow ? 1 : 0;
}
