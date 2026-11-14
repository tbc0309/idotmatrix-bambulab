#include "IDotMatrixMemoryTelemetry.h"

#if IDOTMATRIX_MEMORY_TELEMETRY

#include <esp_heap_caps.h>

namespace {
size_t freeFor(uint32_t caps) {
  return heap_caps_get_free_size(caps);
}

size_t minFor(uint32_t caps) {
  return heap_caps_get_minimum_free_size(caps);
}

size_t largestFor(uint32_t caps) {
  return heap_caps_get_largest_free_block(caps);
}
}  // namespace

void idotMemoryTelemetrySnapshot(const char *tag) {
  const uint32_t internalCaps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
  const uint32_t dmaCaps = MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA;

  const size_t intFree = freeFor(internalCaps);
  const size_t intMin = minFor(internalCaps);
  const size_t intLargest = largestFor(internalCaps);
  const size_t dmaFree = freeFor(dmaCaps);
  const size_t dmaMin = minFor(dmaCaps);
  const size_t dmaLargest = largestFor(dmaCaps);

  size_t psramTotal = 0;
  size_t psramFree = 0;
  size_t psramMin = 0;
  size_t psramLargest = 0;
#if defined(BOARD_HAS_PSRAM)
  const uint32_t psramCaps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
  psramTotal = ESP.getPsramSize();
  psramFree = freeFor(psramCaps);
  psramMin = minFor(psramCaps);
  psramLargest = largestFor(psramCaps);
#endif

  Serial.print("[MEM] t_ms="); Serial.print(millis());
  Serial.print(" tag="); Serial.print(tag ? tag : "unknown");
  Serial.print(" int_free="); Serial.print(intFree);
  Serial.print(" int_min="); Serial.print(intMin);
  Serial.print(" int_largest="); Serial.print(intLargest);
  Serial.print(" dma_free="); Serial.print(dmaFree);
  Serial.print(" dma_min="); Serial.print(dmaMin);
  Serial.print(" dma_largest="); Serial.print(dmaLargest);
  Serial.print(" psram_total="); Serial.print(psramTotal);
  Serial.print(" psram_free="); Serial.print(psramFree);
  Serial.print(" psram_min="); Serial.print(psramMin);
  Serial.print(" psram_largest="); Serial.println(psramLargest);
}

void idotMemoryTelemetryLatency(const char *tag, uint32_t elapsedUs) {
  Serial.print("[LAT] t_ms="); Serial.print(millis());
  Serial.print(" tag="); Serial.print(tag ? tag : "unknown");
  Serial.print(" us="); Serial.println(elapsedUs);
}

#endif
