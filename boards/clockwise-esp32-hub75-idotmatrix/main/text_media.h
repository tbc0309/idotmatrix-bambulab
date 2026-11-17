
#pragma once
#include <cstdlib>
#include "lvgl.h"

struct TextMedia {
    struct Glyph { uint16_t offset; uint8_t width; };
    uint8_t *bytes{};
    size_t length{};
    Glyph glyph[64]{};
    unsigned count{}, height{}, advance{};

    TextMedia() = default;
    TextMedia(const TextMedia &) = delete;
    TextMedia &operator=(const TextMedia &) = delete;
    ~TextMedia() { clear(); }
    void clear() { free(bytes); bytes = nullptr; }
    bool load(const char *path);
    lv_color_t pixel(unsigned x, unsigned y, uint32_t elapsed) const;
};

bool clockwise_text_valid(const char *path);
