#pragma once
#include "board_identity.h"
#ifdef MATRIX_BOARD_OLED
#include "ported/features/pixel_screen/core.h"
#endif
namespace bridge::board {
void begin();
bool bootPressed();
void statusLed(bool enabled);
#ifdef MATRIX_BOARD_OLED
void displaySnapshot(const pixel_screen::Snapshot &snapshot, bool wifi, const char *ip, uint32_t now);
#endif
}
