#include "app_state.h"
#include "lvgl.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <freertos/FreeRTOS.h>

namespace {
struct Lighting {
    uint8_t effect, speed, count;
    lv_color_t colors[16];
};
portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
Lighting pending{};
bool changed = false;
Lighting active{};
uint32_t started;
uint32_t nextFrame;
bool visible;

uint32_t hash(uint32_t value) {
    value ^= value >> 16;
    value *= 0x7feb352d;
    value ^= value >> 15;
    value *= 0x846ca68b;
    return value ^ (value >> 16);
}
lv_color_t color(unsigned index) {
    return active.count ? active.colors[index % active.count] : lv_color_black();
}
lv_color_t gradient(unsigned segment, unsigned part, unsigned span) {
    const auto a = color(segment), b = color(segment + 1);
    const auto from = lv_color_to_hsv(a), to = lv_color_to_hsv(b);
    const int hueDelta = ((int(to.h) - from.h + 540) % 360) - 180;
    const auto interpolate = [part, span](int x, int y) { return x + (y - x) * int(part) / int(span); };
    return lv_color_hsv_to_rgb((interpolate(from.h, from.h + hueDelta) + 360) % 360,
                               interpolate(from.s, to.s), interpolate(from.v, to.v));
}
}

extern "C" bool clockwise_accept_effect(const uint8_t *data, size_t size) {
    if (size < 7 || data[4] > 6 || data[6] > 16 || size != 7u + 3u * data[6]) return false;
    Lighting effect{};
    effect.effect = data[4];
    effect.speed = std::min<uint8_t>(data[5], 100);
    effect.count = data[6];
    for (unsigned i = 0; i < effect.count; ++i) {
        const auto channel = [](uint8_t v) { return uint8_t(v >= 127 ? 255 : v * 2); };
        effect.colors[i] = lv_color_make(channel(data[7 + 3 * i]), channel(data[8 + 3 * i]), channel(data[9 + 3 * i]));
    }
    portENTER_CRITICAL(&lock);
    pending = effect;
    changed = true;
    portEXIT_CRITICAL(&lock);
    return true;
}

void clockwise_effect_stop() {
    portENTER_CRITICAL(&lock);
    changed = false;
    portEXIT_CRITICAL(&lock);
    visible = false;
}

bool clockwise_effect_active(uint32_t now, bool &redraw) {
    portENTER_CRITICAL(&lock);
    if (changed) {
        active = pending;
        changed = false;
        started = nextFrame = now;
        visible = true;
    }
    portEXIT_CRITICAL(&lock);
    redraw = visible && int32_t(now - nextFrame) >= 0;
    if (redraw) nextFrame = now + 33;
    return visible;
}

lv_color_t clockwise_effect_pixel(unsigned x, unsigned y, uint32_t now) {
    const uint32_t elapsed = now - started;
    const unsigned count = std::max<unsigned>(active.count, 1);
    const unsigned step = elapsed / (370u - 3u * active.speed);
    if (active.effect == 0) {
        constexpr unsigned span = 32 * 256;
        const unsigned cycle = span * count;
        const unsigned rowMs = 210 - 172 * active.speed / 100;
        const unsigned phase = (elapsed % (rowMs * 32 * count)) * 256 / rowMs;
        const unsigned position = (y * 256 + cycle - phase) % cycle;
        return gradient(position / span, position % span, span);
    }
    x /= 2;
    y /= 2;
    const unsigned seed = hash(y * 32 + x);
    const unsigned noise = hash(seed + step * 31);
    switch (active.effect) {
    case 1: {
        const unsigned entry = noise % (count * (count < 7 ? 10 - count : 3));
        return entry < count ? color(entry) : lv_color_black();
    }
    case 2: {
        const unsigned length = count * 12;
        const unsigned entry = (length - step % length) % length;
        return noise % 10 == 0 ? lv_color_white() : gradient(entry / 12, entry % 12, 12);
    }
    case 3: return color((x + count * 4 - step % (count * 4)) / 4);
    case 4: return color((x + 32 - y + count * 4 - step % (count * 4)) / 4);
    case 5: {
        const unsigned band = (32 + x + y + step) / 5;
        return band % 2 ? lv_color_black() : color(band / 2);
    }
    case 6: {
        const unsigned fadeMs = 6000 - 53 * active.speed;
        const unsigned local = (elapsed + seed % (fadeMs * count)) % (fadeMs * count);
        return gradient(local / fadeMs, local % fadeMs, fadeMs);
    }
    default: return lv_color_black();
    }
}
