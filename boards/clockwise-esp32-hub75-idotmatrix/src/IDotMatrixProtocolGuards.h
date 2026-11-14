#pragma once

#include <stddef.h>
#include <stdint.h>

// Shared length guards kept independent from Arduino so malformed protocol
// boundaries can be regression-tested on the host toolchain.
static inline bool idotTextPayloadHasMarker(const uint8_t *data, size_t len, size_t globalHeaderBytes) {
  return data != nullptr && len >= globalHeaderBytes + 1U;
}
