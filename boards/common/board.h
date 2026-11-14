#pragma once
#include "board_identity.h"
#if defined(MATRIX_BOARD_OLED) || defined(MATRIX_BOARD_RGB_RING) || defined(MATRIX_BOARD_HUB75)
#include "ported/features/pixel_screen/core.h"
#endif
namespace bridge::board {
void begin();
bool bootPressed();
void statusLed(bool enabled);
#if defined(MATRIX_BOARD_OLED) || defined(MATRIX_BOARD_RGB_RING) || defined(MATRIX_BOARD_HUB75)
void displaySnapshot(const pixel_screen::Snapshot &snapshot, bool wifi, const char *ip, uint32_t now);
#endif
}
