#pragma once
#include <stdint.h>

using ClockPixel = void (*)(unsigned, unsigned, uint8_t, uint8_t, uint8_t);
void renderClockView(ClockPixel pixel, uint32_t now, uint8_t hour, uint8_t minute,
                     uint8_t day, uint8_t month, uint32_t settings, bool datePage);
void renderUtilityView(ClockPixel pixel, uint32_t now, unsigned mode, uint32_t value);
