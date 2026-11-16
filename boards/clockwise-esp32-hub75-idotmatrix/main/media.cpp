#include "app_state.h"
#include <cstdlib>
#include <cstring>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>

namespace {
constexpr size_t frameSize = 64 * 64 * 3;
portMUX_TYPE frameLock = portMUX_INITIALIZER_UNLOCKED;
uint8_t *ready;
uint8_t *receiving;
size_t written;
uint32_t expectedCrc;
uint32_t crc;
uint64_t lastPacket;

uint32_t read32(const uint8_t *data) {
    return uint32_t(data[0]) | (uint32_t(data[1]) << 8) |
           (uint32_t(data[2]) << 16) | (uint32_t(data[3]) << 24);
}
}

extern "C" void clockwise_media_abort() {
    free(receiving);
    receiving = nullptr;
    written = 0;
}

extern "C" uint8_t clockwise_accept_media(const uint8_t *data, size_t size) {
    const auto now = uint64_t(esp_timer_get_time());
    if (receiving && now - lastPacket > 5000000) clockwise_media_abort();
    if (size < 16 || read32(data + 5) != frameSize) {
        clockwise_media_abort();
        return 3;
    }
    const uint32_t requestedCrc = read32(data + 9);
    if (!receiving) {
        receiving = static_cast<uint8_t *>(malloc(frameSize));
        if (!receiving) return 3;
        expectedCrc = requestedCrc;
        crc = UINT32_MAX;
    }
    const size_t length = size - 16;
    if (requestedCrc != expectedCrc || length > frameSize - written) {
        clockwise_media_abort();
        return 3;
    }
    const auto *pixels = data + 16;
    memcpy(receiving + written, pixels, length);
    for (size_t i = 0; i < length; ++i) {
        crc ^= pixels[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0u);
    }
    written += length;
    lastPacket = now;
    if (written < frameSize) return 1;
    if ((crc ^ UINT32_MAX) != expectedCrc) {
        clockwise_media_abort();
        return 3;
    }
    portENTER_CRITICAL(&frameLock);
    uint8_t *superseded = ready;
    ready = receiving;
    portEXIT_CRITICAL(&frameLock);
    free(superseded);
    receiving = nullptr;
    written = 0;
    return 3;
}

uint8_t *clockwise_take_media() {
    portENTER_CRITICAL(&frameLock);
    uint8_t *result = ready;
    ready = nullptr;
    portEXIT_CRITICAL(&frameLock);
    return result;
}
