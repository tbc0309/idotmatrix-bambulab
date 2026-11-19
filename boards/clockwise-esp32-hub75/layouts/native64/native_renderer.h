#pragma once

#include "ported/features/pixel_screen/core.h"

namespace pixel_screen {
constexpr unsigned nativePanelSize = 64;
constexpr unsigned pixels64 = nativePanelSize * nativePanelSize;
void render64(const Snapshot &snapshot, unsigned phase, uint32_t *frame);
}
