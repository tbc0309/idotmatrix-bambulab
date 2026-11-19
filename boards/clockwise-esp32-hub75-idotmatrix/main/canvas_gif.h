#pragma once
#include <cstdint>
#include <cstdio>

class CanvasGif {
public:
    bool open(const char *path, uint8_t *rgb);
    void close();
    bool active() const { return file != nullptr; }
    bool advance(uint32_t now);

private:
    struct Rect {
        unsigned x, y, w, h;
    };
    struct RGB {
        uint8_t r, g, b;
    };
    bool next(unsigned &delay);
    bool image(Rect rect, int flags, const RGB *palette, unsigned colors);
    bool skipBlocks();
    bool table(RGB *palette, unsigned count);
    uint8_t *pixel(unsigned x, unsigned y);
    FILE *file{};
    uint8_t *canvas{}, *indices{}, *saved{};
    volatile uint32_t *dictionary{};
    RGB global[256]{}, local[256]{};
    unsigned width{}, height{}, globalCount{}, background{}, disposal{}, lastDisposal{};
    int transparent{-1};
    unsigned duration{};
    long restart{};
    Rect previous{};
    uint32_t due{};
    bool first{};
};
