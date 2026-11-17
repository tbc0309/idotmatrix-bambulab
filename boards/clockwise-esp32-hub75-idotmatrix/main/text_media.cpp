
#include "text_media.h"
#include <algorithm>
#include <iterator>
#include <cmath>
#include <cstdlib>
#include <cstdio>

namespace {
constexpr size_t headerSize = 14;
constexpr size_t maxSize = 16654;
struct Dimensions { uint8_t width, height; };
constexpr Dimensions formats[] = {{}, {}, {8,16}, {16,16}, {}, {16,32}, {32,32}, {32,64}, {64,64}};

bool parse(TextMedia &m, Dimensions fallback) {
    size_t offset = headerSize;
    m.count = m.bytes[0] | unsigned(m.bytes[1]) << 8;
    if (!m.count || m.count > 64) return false;
    m.height = m.advance = 0;
    for (unsigned i = 0; i < m.count; ++i) {
        if (m.length - offset < 4) return false;
        unsigned marker = m.bytes[offset];
        if (!fallback.width && marker >= std::size(formats)) return false;
        const auto d = fallback.width ? fallback : formats[marker];
        if (!d.width || (m.height && m.height != d.height)) return false;
        m.height = d.height;
        m.advance = std::max<unsigned>(m.advance, d.width);
        const auto bytes = unsigned(d.width) / 8 * d.height;
        if (bytes > m.length - offset - 4) return false;
        m.glyph[i] = {uint16_t(offset + 4), d.width};
        offset += 4 + bytes;
    }
    return offset == m.length;
}
}

bool TextMedia::load(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    fseek(file, 0, SEEK_END);
    const long size = ftell(file);
    rewind(file);
    if (size <= long(headerSize) || size > long(maxSize)) {
        fclose(file);
        return false;
    }
    length = size;
    bytes = static_cast<uint8_t *>(malloc(length));
    const bool read = bytes && fread(bytes, 1, length, file) == length;
    fclose(file);
    if (read && (parse(*this, {}) || parse(*this, {8,16}) ||
                 parse(*this, {16,32}) || parse(*this, {32,64}))) return true;
    free(bytes);
    bytes = nullptr;
    return false;
}

bool clockwise_text_valid(const char *path) {
    TextMedia candidate{};
    return candidate.load(path);
}

lv_color_t TextMedia::pixel(unsigned x, unsigned y, uint32_t elapsed) const {
    const auto background = bytes[10] ? lv_color_make(bytes[11], bytes[12], bytes[13]) : lv_color_black();
    const unsigned motion = bytes[4];
    const unsigned interval = 140 - 120 * std::min<unsigned>(bytes[5], 100) / 100;
    const unsigned perLine = 64 / advance;
    const unsigned perPage = perLine * (64 / height);
    int gx = x, gy = y;
    unsigned index;
    if (motion == 1 || motion == 2) {
        const int span = int(count * advance);
        const int step = (elapsed / interval) % (64 + span);
        gx -= motion == 1 ? 64 - step : step - span;
        gy -= (64 - height) / 2;
        if (gx < 0 || gx >= span) return background;
        index = gx / advance;
    } else {
        const unsigned pages = (count + perPage - 1) / perPage;
        unsigned page;
        if (motion == 3 || motion == 4) {
            const unsigned step = elapsed / interval;
            const int shift = step % (height + 1);
            gy += motion == 3 ? shift : -shift;
            page = step / (height + 1) % pages;
        } else {
            const unsigned duration = 1600 - 1350 * std::min<unsigned>(bytes[5], 100) / 100;
            page = elapsed / duration % pages;
        }
        const unsigned visible = std::min(perPage, count - page * perPage);
        const unsigned rows = (visible + perLine - 1) / perLine;
        gy -= (64 - rows * height) / 2;
        if (gy < 0 || gy >= int(rows * height)) return background;
        index = page * perPage + (gy / height) * perLine + x / advance;
        gy %= height;
    }
    if (motion == 7) {
        const uint32_t seed = (x * 2654435761u) ^ (x << 12);
        if (x % 3 && ((seed >> 10) + elapsed / (85 + x % 25)) % 64 == y)
            return lv_color_white();
    }
    if (motion == 8 && y == elapsed / 70 % 64) return lv_color_make(130, 0, 0);
    gx %= advance;
    if (index >= count || gy < 0 || gy >= int(height) ||
        gx >= glyph[index].width || (motion == 5 && (elapsed / 350) % 2)) return background;
    const unsigned offset = glyph[index].offset + unsigned(gy) * (glyph[index].width / 8) + unsigned(gx) / 8;
    if (!(bytes[offset] & (1u << (gx % 8)))) return background;
    lv_color_t color = lv_color_make(bytes[7], bytes[8], bytes[9]);
    switch (bytes[6]) {
    case 2: color = lv_color_hsv_to_rgb((x * 25 + elapsed / 11) % 360, 100, 100); break;
    case 3: color = lv_color_hsv_to_rgb((y * 27 + 360 - elapsed / 13 % 360) % 360, 69, 100); break;
    case 4:
    case 5: {
        const unsigned center = bytes[6] == 4 ? 28 : 230;
        const auto variation = int(27 * std::sin((x * 5 + y * 3 + elapsed / 8) * .08));
        color = lv_color_hsv_to_rgb(center + variation, 86, 100);
        break;
    }
    }
    if (motion == 6 || motion == 8) {
        const unsigned scale = motion == 8 ? 110 : unsigned(148 + 107 * std::sin(elapsed * .003));
        color = lv_color_make(color.red * scale / 255, color.green * scale / 255, color.blue * scale / 255);
    }
    return color;
}
