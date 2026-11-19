#include "canvas_gif.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <esp_heap_caps.h>
#include <esp_log.h>

namespace {
unsigned le16(const uint8_t *p) { return p[0] | unsigned(p[1]) << 8; }

// GIF packs LZW codes low-bit first across arbitrary 255-byte sub-blocks.
struct Codes {
    FILE *file;
    unsigned left{}, bits{}, count{};
    int get(unsigned width) {
        while (count < width) {
            if (!left) {
                int n = fgetc(file);
                if (n <= 0)
                    return -1;
                left = n;
            }
            int b = fgetc(file);
            if (b < 0)
                return -1;
            --left;
            bits |= unsigned(b) << count;
            count += 8;
        }
        int code = bits & ((1u << width) - 1);
        bits >>= width;
        count -= width;
        return code;
    }
};
} // namespace

bool CanvasGif::table(RGB *p, unsigned n) { return fread(p, sizeof(RGB), n, file) == n; }
uint8_t *CanvasGif::pixel(unsigned x, unsigned y) {
    return canvas + (64 * ((64 - height) / 2 + y) + (64 - width) / 2 + x) * 3;
}
void CanvasGif::close() {
    if (file)
        fclose(file);
    file = nullptr;
    free(indices);
    indices = nullptr;
    free(saved);
    saved = nullptr;
    heap_caps_free(const_cast<uint32_t *>(dictionary));
    dictionary = nullptr;
}
bool CanvasGif::open(const char *path, uint8_t *rgb) {
    close();
    uint8_t h[13];
    file = fopen(path, "rb");
    if (!file)
        return false;
    if (fread(h, 1, sizeof(h), file) != sizeof(h) || (memcmp(h, "GIF87a", 6) && memcmp(h, "GIF89a", 6))) {
        close();
        return false;
    }
    width = le16(h + 6);
    height = le16(h + 8);
    if (!width || !height || width > 64 || height > 64) {
        close();
        return false;
    }
    globalCount = (h[10] & 128) ? 2u << (h[10] & 7) : 0;
    if (globalCount && !table(global, globalCount)) {
        close();
        return false;
    }
    background = h[11];
    restart = ftell(file);
    canvas = rgb;
    // Each dictionary node is accessed exclusively as an aligned 32-bit word.
    dictionary = static_cast<uint32_t *>(heap_caps_malloc_prefer(4096 * sizeof(uint32_t), 2,
                                                                 MALLOC_CAP_EXEC | MALLOC_CAP_32BIT,
                                                                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    indices = static_cast<uint8_t *>(malloc(width * height));
    if (!indices || !dictionary) {
        close();
        return false;
    }
    memset(canvas, 0, 64 * 64 * 3);
    first = true;
    due = 0;
    lastDisposal = disposal = duration = 0;
    transparent = -1;
    return true;
}
bool CanvasGif::skipBlocks() {
    int n;
    while ((n = fgetc(file)) > 0)
        if (fseek(file, n, SEEK_CUR))
            return false;
    return n == 0;
}
bool CanvasGif::image(Rect rect, int flags, const RGB *pal, unsigned colors) {
    const int minBits = fgetc(file);
    if (minBits < 2 || minBits > 8 || !colors)
        return false;
    const unsigned clear = 1u << minBits, end = clear + 1, total = rect.w * rect.h;
    unsigned widthBits = minBits + 1, nextCode = end + 1, written = 0, firstIndex = 0;
    int prev = -1;
    Codes codes{file};
    for (;;) {
        int code = codes.get(widthBits);
        if (code < 0)
            return false;
        if (unsigned(code) == end)
            break;
        if (unsigned(code) == clear) {
            widthBits = minBits + 1;
            nextCode = end + 1;
            prev = -1;
            continue;
        }
        if (unsigned(code) > nextCode || (prev < 0 && unsigned(code) >= clear))
            return false;
        unsigned root = code, extra = 0;
        if (unsigned(code) == nextCode) {
            root = prev;
            extra = 1;
        }
        unsigned length = root < clear ? 1 : (dictionary[root] >> 20) + 1;
        if (length + extra > total - written)
            return false;
        unsigned pos = written + length;
        if (extra)
            indices[pos] = firstIndex;
        while (root >= clear) {
            uint32_t node = dictionary[root];
            indices[--pos] = (node >> 12) & 255;
            root = node & 4095;
        }
        firstIndex = root;
        indices[written] = firstIndex;
        written += length + extra;
        if (prev >= 0 && nextCode < 4096) {
            unsigned prior = unsigned(prev) < clear ? 1 : (dictionary[prev] >> 20) + 1;
            dictionary[nextCode++] = (prior << 20) | (firstIndex << 12) | unsigned(prev);
            if (nextCode == (1u << widthBits) && widthBits < 12)
                ++widthBits;
        }
        prev = code;
    }
    if (written != total || fseek(file, codes.left, SEEK_CUR) || !skipBlocks())
        return false;
    if (lastDisposal == 2 || (lastDisposal == 3 && saved)) {
        RGB bg = background < globalCount ? global[background] : RGB{};
        for (unsigned y = 0; y < previous.h; ++y)
            for (unsigned x = 0; x < previous.w; ++x) {
                auto p = pixel(previous.x + x, previous.y + y);
                memcpy(p,
                       lastDisposal == 3 ? saved + 3 * (y * previous.w + x)
                                         : reinterpret_cast<const uint8_t *>(&bg),
                       3);
            }
    }
    free(saved);
    saved = nullptr;
    if (disposal == 3) {
        saved = static_cast<uint8_t *>(malloc(total * 3));
        if (!saved)
            return false;
        for (unsigned y = 0; y < rect.h; ++y)
            memcpy(saved + y * rect.w * 3, pixel(rect.x, rect.y + y), rect.w * 3);
    }
    constexpr unsigned starts[] = {0, 4, 2, 1}, steps[] = {8, 8, 4, 2};
    unsigned at = 0;
    const bool interlaced = flags & 64;
    for (unsigned pass = 0; pass < (interlaced ? 4u : 1u); ++pass)
        for (unsigned y = starts[pass]; y < rect.h; y += interlaced ? steps[pass] : 1)
            for (unsigned x = 0; x < rect.w; ++x) {
                unsigned index = indices[at++];
                if (int(index) == transparent)
                    continue;
                if (index >= colors)
                    return false;
                memcpy(pixel(rect.x + x, rect.y + y), &pal[index], 3);
            }
    previous = rect;
    lastDisposal = disposal;
    return true;
}
bool CanvasGif::next(unsigned &delay) {
    bool rewound = false;
    for (;;) {
        int tag = fgetc(file);
        if (tag == 0x3b && !rewound) {
            if (fseek(file, restart, SEEK_SET))
                return false;
            rewound = true;
            lastDisposal = 0;
            memset(canvas, 0, 64 * 64 * 3);
            continue;
        }
        if (tag == 0x21) {
            int kind = fgetc(file);
            if (kind < 0)
                return false;
            if (kind != 0xf9) {
                if (!skipBlocks())
                    return false;
                continue;
            }
            uint8_t g[6];
            if (fread(g, 1, 6, file) != 6 || g[0] != 4 || g[5])
                return false;
            disposal = (g[1] >> 2) & 7;
            transparent = (g[1] & 1) ? g[4] : -1;
            duration = le16(g + 2) * 10;
            continue;
        }
        uint8_t d[9];
        if (tag != 0x2c || fread(d, 1, 9, file) != 9)
            return false;
        Rect rect{le16(d), le16(d + 2), le16(d + 4), le16(d + 6)};
        if (!rect.w || !rect.h || rect.x + rect.w > width || rect.y + rect.h > height)
            return false;
        bool hasLocal = d[8] & 128;
        unsigned n = hasLocal ? 2u << (d[8] & 7) : globalCount;
        if (hasLocal && !table(local, n))
            return false;
        if (!image(rect, d[8], hasLocal ? local : global, n))
            return false;
        delay = std::max(10u, duration);
        duration = disposal = 0;
        transparent = -1;
        return true;
    }
}
bool CanvasGif::advance(uint32_t now) {
    if (!first && int32_t(now - due) < 0)
        return false;
    unsigned delay;
    if (!next(delay)) {
        ESP_LOGW("media", "GIF frame decode failed");
        close();
        return false;
    }
    first = false;
    due = now + delay;
    return true;
}
