#include "clock_view.h"
#include "clock_glyphs.h"
#include <algorithm>
#include <cstdlib>

namespace {
struct RGB {
    uint8_t r, g, b;
    static const RGB Black, White, Red;
};
constexpr RGB RGB::Black{0,0,0};
constexpr RGB RGB::White{255,255,255};
constexpr RGB RGB::Red{255,0,0};
constexpr unsigned MATRIX_WIDTH = 64;

RGB blend(RGB a, RGB b, unsigned fraction) {
    const auto channel = [fraction](unsigned a, unsigned b) {
        return uint8_t((a * (256 - fraction) + b * fraction) >> 8);
    };
    return {channel(a.r, b.r), channel(a.g, b.g), channel(a.b, b.b)};
}

RGB defaultClockColor(uint8_t style) {
    static constexpr RGB colors[] = {
        {97,24,214}, {229,12,23}, {255,152,43}, {19,67,248},
        {106,39,248}, {255,146,42}, {255,15,77}, {67,250,85},
    };
    return colors[style & 7];
}
RGB clockwiseClockDisplayColor(uint8_t style, const RGB& selected) {
  static const RGB previewColors[] = {
    {110,16,252}, {218,1,1}, {252,161,1}, {34,76,252},
    {111,42,252}, {252,155,1}, {252,1,84}, {116,248,76},
  };
  const RGB preset = defaultClockColor(style);
  if (selected.r == preset.r && selected.g == preset.g && selected.b == preset.b)
    return previewColors[style & 7];
  // The Android 64x64 clock tab also sends this red for the Christmas clock.
  if ((style & 7) == 1 && selected.r == 220 && selected.g == 1 && selected.b == 1)
    return previewColors[1];
  return selected;
}

struct Renderer {
    ClockPixel pixel;
    uint32_t now;
    RGB clockColor;
    uint8_t clockStyle;
    bool clockRenderingDate, clockColonVisible, scaleLegacy;

    void putPixel(int x, int y, RGB value) {
        if (scaleLegacy) {
            for (unsigned dy = 0; dy < 4; ++dy)
                for (unsigned dx = 0; dx < 4; ++dx)
                    pixel(x * 4 + dx, y * 4 + dy, value.r, value.g, value.b);
        } else if (x >= 0 && y >= 0 && x < 64 && y < 64) {
            pixel(x, y, value.r, value.g, value.b);
        }
    }

    void clearFramebuffer(RGB color) {
        for (unsigned y = 0; y < 64; ++y)
            for (unsigned x = 0; x < 64; ++x)
                pixel(x, y, color.r, color.g, color.b);
    }

    static constexpr uint8_t digits3x5[10][5] = {
        {7,5,5,5,7}, {2,6,2,2,7}, {7,1,7,4,7}, {7,1,7,1,7}, {5,5,7,1,1},
        {7,4,7,1,7}, {7,4,7,5,7}, {7,1,2,2,2}, {7,5,7,5,7}, {7,5,7,1,7}
    };

void drawDigit3x5NativeScaled(uint8_t d, int16_t x, int16_t y, uint8_t scale, const RGB &c) {
  if (d > 9) return;
  for (uint8_t row=0; row<5; row++) {
    for (uint8_t col=0; col<3; col++) {
      if (!(digits3x5[d][row] & (1 << (2-col)))) continue;
      for (uint8_t yy=0; yy<scale; yy++)
        for (uint8_t xx=0; xx<scale; xx++)
          putPixel(x + col*scale + xx, y + row*scale + yy, c);
    }
  }
}

void drawNative64ClockRow(uint8_t a, uint8_t b, uint8_t c, uint8_t d,
                          int16_t y, const RGB &color, bool slash) {
  constexpr uint8_t scale=3;
  constexpr int16_t width=51;
  const int16_t x0=(64-width)/2;
  drawDigit3x5NativeScaled(a,x0,y,scale,color);
  drawDigit3x5NativeScaled(b,x0+12,y,scale,color);
  const int16_t sx=x0+24;
  if(slash){
    for(uint8_t i=0;i<3;i++){
      putPixel(sx+2-i,y+3+i,color);
      putPixel(sx+2-i,y+6+i,color);
      putPixel(sx+1-i,y+9+i,color);
    }
  } else if(clockColonVisible) {
    for(uint8_t yy=0;yy<3;yy++){
      for(uint8_t xx=0;xx<3;xx++){
        putPixel(sx+xx,y+3+yy,color);
        putPixel(sx+xx,y+9+yy,color);
      }
    }
  }
  drawDigit3x5NativeScaled(c,x0+30,y,scale,color);
  drawDigit3x5NativeScaled(d,x0+42,y,scale,color);
}

void drawClockwiseClockDigit(uint8_t digit, int x, int y, uint8_t style,
                             const RGB& color) {
  if (digit > 9) return;
  const auto& glyph = clockwiseClockGlyphs[style][digit];
  for (uint8_t row = 0; row < 14; ++row)
    for (uint8_t col = 0; col < 10; ++col)
      if (glyph[row] & (1U << col))
        for (uint8_t dy = 0; dy < 2; ++dy)
          for (uint8_t dx = 0; dx < 2; ++dx)
            putPixel(x + 2 * col + dx, y + 2 * row + dy, color);
}

void drawClockwiseRainbowBorder(uint32_t now) {
    // One path around the four sides. Pixels across the thickness share the
    // same position on a straight side; only the corners use a local angle.
    constexpr int thickness = 4;
    // Position units are 1/256 pixel. The four corner fractions are sampled
    // along a quarter circle around the nearby corner, independent of screen centre.
    constexpr unsigned span = 56 * 256;
    constexpr unsigned arc = 804;
    constexpr unsigned side = span + arc;
    constexpr uint32_t lapMs = 5200;
    static constexpr uint16_t cornerFraction[] = {93, 129, 210, 512, 264, 352, 512, 814, 404, 512, 672, 895, 512, 620, 760, 931};
    // The Android 64x64 preview repeats the full spectrum on each side.
    // Samples follow the top edge from magenta to the red corner.
    static constexpr RGB palette[] = {
        {251,0,232}, {208,0,239}, {143,0,250}, {109,10,251},
        {90,51,251}, {51,117,251}, {2,197,251}, {2,245,237},
        {59,251,176}, {134,251,106}, {228,249,16}, {251,212,0},
        {251,149,0}, {250,90,0}, {245,36,0}, {243,17,0},
    };
    constexpr unsigned count = sizeof(palette) / sizeof(palette[0]);
    constexpr unsigned cycle = count * 256;
    const unsigned phase = (now % lapMs) * (cycle * 4) / lapMs % cycle;
    const auto colorAt = [&](unsigned position) {
        const unsigned hue = ((position % side) * cycle / side + cycle - phase) % cycle;
        return blend(palette[hue / 256], palette[(hue / 256 + 1) % count], hue % 256);
    };

    // Reuse one clockwise mapping for each side and the quarter-turn after it.
    for (unsigned edge = 0; edge < 4; ++edge) {
        const auto plot = [&](int extent, int depth, RGB color) {
            constexpr int last = MATRIX_WIDTH - 1;
            switch (edge) {
            case 0: putPixel(extent, depth, color); break;
            case 1: putPixel(last - depth, extent, color); break;
            case 2: putPixel(last - extent, last - depth, color); break;
            case 3: putPixel(depth, last - extent, color); break;
            }
        };
        for (int extent = thickness; extent < int(MATRIX_WIDTH) - thickness; ++extent) {
            const RGB color = colorAt(edge * side + (extent * 2 - 7) * 128);
            for (int depth = 0; depth < thickness; ++depth) plot(extent, depth, color);
        }
        for (int extent = MATRIX_WIDTH - thickness; extent < int(MATRIX_WIDTH); ++extent)
            for (int depth = 0; depth < thickness; ++depth) {
                const unsigned cornerPixel = (extent - (MATRIX_WIDTH - thickness)) * thickness + depth;
                plot(extent, depth, colorAt(edge * side + span + arc * cornerFraction[cornerPixel] / 1024));
            }
    }
}

void drawFrameStyle5(bool cornerBlocks) {
  RGB cyan(0,255,255), blue(0,70,255);

  if (cornerBlocks) {
    // Previous style: cyan frame with blue blocks at the corners.
    for (uint8_t x=2;x<=13;x++) { putPixel(x,1,cyan); putPixel(x,14,cyan); }
    for (uint8_t y=2;y<=13;y++) { putPixel(1,y,cyan); putPixel(14,y,cyan); }
    for (uint8_t y=0;y<3;y++) for (uint8_t x=0;x<3;x++) {
      putPixel(x,y,blue); putPixel(15-x,y,blue); putPixel(x,15-y,blue); putPixel(15-x,15-y,blue);
    }
  } else {
    // TWO continuous frames, each exactly one pixel wide.
    // Pure-blue outer frame and pure-cyan inner frame.
    const RGB outerBlue(0,0,255);
    const RGB innerCyan(0,255,255);

    for (uint8_t x=0;x<16;x++) {
      putPixel(x,0,outerBlue);
      putPixel(x,15,outerBlue);
    }
    for (uint8_t y=0;y<16;y++) {
      putPixel(0,y,outerBlue);
      putPixel(15,y,outerBlue);
    }

    for (uint8_t x=1;x<15;x++) {
      putPixel(x,1,innerCyan);
      putPixel(x,14,innerCyan);
    }
    for (uint8_t y=1;y<15;y++) {
      putPixel(1,y,innerCyan);
      putPixel(14,y,innerCyan);
    }
  }
}

void drawQuadrantBorder() {
  RGB red(255,30,20), yellow(255,255,40), green(70,255,50), blue(30,80,255);
  for (uint8_t x=0;x<8;x++) putPixel(x,0,red);
  for (uint8_t x=8;x<16;x++) putPixel(x,0,yellow);
  for (uint8_t y=0;y<8;y++) putPixel(0,y,red);
  for (uint8_t y=8;y<16;y++) putPixel(0,y,blue);
  for (uint8_t y=0;y<8;y++) putPixel(15,y,yellow);
  for (uint8_t y=8;y<16;y++) putPixel(15,y,green);
  for (uint8_t x=0;x<8;x++) putPixel(x,15,blue);
  for (uint8_t x=8;x<16;x++) putPixel(x,15,green);
}


void drawClockwiseCornerFrame() {
  static constexpr char top[][17] = {
    "0222000220022200",
    "2221111111111222",
    "2210000000000122",
    "2100000000000012",
    "0100000000000010",
  };
  static constexpr char bottom[][17] = {
    "2201111111111022",
    "2210000000000122",
    "0100000000000010",
  };
  const RGB colors[] = {RGB::Black, RGB{1,249,252}, RGB{34,76,252}};
  for (uint8_t y=0; y<16; ++y) {
    for (uint8_t x=0; x<16; ++x) {
      const uint8_t index = y<5 ? top[y][x]-'0' :
                            y>=13 ? bottom[15-y][x]-'0' :
                            (x==0 || x==15);
      if (index) putPixel(x,y,colors[index]);
    }
  }
}
void drawClockwiseChristmasTree() {
  static const RGB colors[] = {{0,0,0}, {14,91,1}, {106,73,1}, {252,252,3},
                                {1,252,1}, {1,168,252}, {209,72,1}, {252,2,127}};
  static constexpr char rows[][15] = {
    "00000033000000",
    "00000033000000",
    "00001111110000",
    "00001111110000",
    "00006611110000",
    "00006611110000",
    "00111111441100",
    "00111111441100",
    "00116611111100",
    "00116611111100",
    "11111111117711",
    "11111111117711",
    "11551122111111",
    "11551122111111",
    "00000022000000",
    "00000022000000"
  };
  for (uint8_t y = 0; y < 16; ++y) {
    for (uint8_t x = 0; x < 14; ++x) {
      const char sample = rows[y][x];
      if (sample == '0') continue;
      const RGB& color = colors[sample <= '9' ? sample - '0' : sample - 'a' + 10];
      for (uint8_t dy = 0; dy < 2; ++dy)
        for (uint8_t dx = 0; dx < 2; ++dx)
          putPixel(x * 2 + dx, 32 + y * 2 + dy, color);
    }
  }
}

void drawClockwiseHourglass() {
  static const RGB colors[] = {{0,0,0}, {252,252,252}, {252,155,5}, {253,155,2}, {252,155,1}, {252,153,2}, {252,153,1}, {5,5,5}, {5,3,2}, {5,3,1}, {2,2,2}, {2,1,1}, {1,1,1}};
  static constexpr char rows[][13] = {
    "344444444444",
    "344444444444",
    "006400112400",
    "006400112400",
    "006411006400",
    "006411006400",
    "000053440000",
    "000064440000",
    "006411006400",
    "006411006400",
    "006400112400",
    "006400112400",
    "344444334444",
    "344444444444"
  };
  for (uint8_t y = 0; y < 14; ++y) {
    for (uint8_t x = 0; x < 12; ++x) {
      const char sample = rows[y][x];
      if (sample == '0') continue;
      const RGB& color = colors[sample <= '9' ? sample - '0' : sample - 'a' + 10];
      for (uint8_t dy = 0; dy < 2; ++dy)
        for (uint8_t dx = 0; dx < 2; ++dx)
          putPixel(x * 2 + dx, 34 + y * 2 + dy, color);
    }
  }
}

void renderClockwiseClock(uint8_t first, uint8_t second) {
  struct ClockLayout {
    uint8_t hourX, hourY, hourUnitsX, minuteX, minuteY, minuteUnitsX, separatorX, separatorY;
  };
  constexpr ClockLayout layouts[] = {
    {16,  8, 36, 16, 36, 36,  8, 40},
    {10,  2, 28, 30, 34, 48, 50,  8},
    { 2, 24, 16, 36, 24, 50, 30, 28},
    {14,  2, 38, 14, 34, 38,  6, 42},
    {10,  2, 28, 28, 34, 46, 50, 10},
    {20, 12, 36, 20, 36, 36, 12, 40},
    {20, 10, 36, 20, 34, 36, 12, 38},
    {16, 10, 36, 16, 34, 36,  8, 38},
  };
  const uint8_t style = clockStyle & 7;
  const auto& layout = layouts[style];
  const RGB displayColor = clockwiseClockDisplayColor(style, clockColor);
  const RGB color = style == 3 ? RGB::Black : displayColor;
  clearFramebuffer(style == 3 ? displayColor : RGB::Black);
  if (style == 2) {
    const RGB colors[] = {{118,68,252}, {249,54,205}, {1,242,252}};
    for (uint8_t y = 0; y < 12; ++y) {
      for (uint8_t x = 0; x < 64; ++x) {
        const uint8_t col = (x < 32 ? x : 63 - x) / 4;
        if ((col + y / 4) % 2 != 0) continue;
        putPixel(x, y, colors[y / 4]);
        putPixel(x, 63 - y, colors[y / 4]);
      }
    }
  }
  if (style >= 5) {
    scaleLegacy = true;
    if (style == 5) drawClockwiseCornerFrame();
    if (style == 6) drawFrameStyle5(false);
    if (style == 7) drawQuadrantBorder();
    scaleLegacy = false;
  }
  if (style == 1) drawClockwiseChristmasTree();
  if (style == 4) drawClockwiseHourglass();
  if (style == 0) drawClockwiseRainbowBorder(now);
  drawClockwiseClockDigit(first / 10, layout.hourX, layout.hourY, style, color);
  drawClockwiseClockDigit(first % 10, layout.hourUnitsX, layout.hourY, style, color);
  drawClockwiseClockDigit(second / 10, layout.minuteX, layout.minuteY, style, color);
  drawClockwiseClockDigit(second % 10, layout.minuteUnitsX, layout.minuteY, style, color);
  const RGB separator = style == 2 ? RGB::White : color;
  if (clockRenderingDate) {
    for (uint8_t row = 0; row < 12; ++row)
      for (uint8_t dx = 0; dx < 2; ++dx)
        putPixel(layout.separatorX + 3 - row / 4 + dx, layout.separatorY + row, separator);
  } else if (clockColonVisible) {
    for (uint8_t dy = 0; dy < 4; ++dy) {
      for (uint8_t dx = 0; dx < 4; ++dx) {
        putPixel(layout.separatorX + dx, layout.separatorY + dy, separator);
        putPixel(layout.separatorX + dx, layout.separatorY + 8 + dy, separator);
      }
    }
  }

}
static constexpr uint8_t countdownHourglassFrames[10][70] = {
  {3,3,3,3,3,3,3, 1,2,2,2,2,2,1, 1,2,2,2,2,2,1, 0,1,2,2,2,1,0, 0,0,1,2,1,0,0, 0,0,1,0,1,0,0, 0,1,0,0,0,1,0, 1,0,0,0,0,0,1, 1,0,0,0,0,0,1, 3,3,3,3,3,3,3},
  {3,3,3,3,3,3,3, 1,2,2,2,2,2,1, 1,2,2,2,2,2,1, 0,1,2,2,2,1,0, 0,0,1,2,1,0,0, 0,0,1,2,1,0,0, 0,1,0,0,0,1,0, 1,0,0,0,0,0,1, 1,0,0,0,0,0,1, 3,3,3,3,3,3,3},
  {3,3,3,3,3,3,3, 1,2,2,2,2,2,1, 1,2,2,2,2,2,1, 0,1,2,2,2,1,0, 0,0,1,2,1,0,0, 0,0,1,2,1,0,0, 0,1,0,2,0,1,0, 1,0,0,0,0,0,1, 1,0,0,0,0,0,1, 3,3,3,3,3,3,3},
  {3,3,3,3,3,3,3, 1,2,2,2,2,2,1, 1,2,2,2,2,2,1, 0,1,2,2,2,1,0, 0,0,1,2,1,0,0, 0,0,1,2,1,0,0, 0,1,0,2,0,1,0, 1,0,0,2,0,0,1, 1,0,0,2,0,0,1, 3,3,3,3,3,3,3},
  {3,3,3,3,3,3,3, 1,2,2,2,2,2,1, 1,2,2,2,2,2,1, 0,1,2,2,2,1,0, 0,0,1,2,1,0,0, 0,0,1,2,1,0,0, 0,1,0,2,0,1,0, 1,0,0,2,0,0,1, 1,0,2,2,2,0,1, 3,3,3,3,3,3,3},
  {3,3,3,3,3,3,3, 1,0,0,0,0,0,1, 1,2,2,2,2,2,1, 0,1,2,2,2,1,0, 0,0,1,2,1,0,0, 0,0,1,2,1,0,0, 0,1,0,2,0,1,0, 1,0,0,2,0,0,1, 1,2,2,2,2,2,1, 3,3,3,3,3,3,3},
  {3,3,3,3,3,3,3, 1,0,0,0,0,0,1, 1,2,2,2,2,2,1, 0,1,2,2,2,1,0, 0,0,1,2,1,0,0, 0,0,1,2,1,0,0, 0,1,0,2,0,1,0, 1,0,2,2,2,0,1, 1,2,2,2,2,2,1, 3,3,3,3,3,3,3},
  {3,3,3,3,3,3,3, 1,0,0,0,0,0,1, 1,0,0,0,0,0,1, 0,1,2,2,2,1,0, 0,0,1,2,1,0,0, 0,0,1,2,1,0,0, 0,1,0,2,0,1,0, 1,2,2,2,2,2,1, 1,2,2,2,2,2,1, 3,3,3,3,3,3,3},
  {3,3,3,3,3,3,3, 1,0,0,0,0,0,1, 1,0,0,0,0,0,1, 0,1,0,0,0,1,0, 0,0,1,2,1,0,0, 0,0,1,2,1,0,0, 0,1,2,2,2,1,0, 1,2,2,2,2,2,1, 1,2,2,2,2,2,1, 3,3,3,3,3,3,3},
  {3,3,3,3,3,3,3, 1,0,0,0,0,0,1, 1,0,0,0,0,0,1, 0,1,0,0,0,1,0, 0,0,1,0,1,0,0, 0,0,1,2,1,0,0, 0,1,2,2,2,1,0, 1,2,2,2,2,2,1, 1,2,2,2,2,2,1, 3,3,3,3,3,3,3}
};

void fillCountdownBlock(int16_t x, int16_t y, uint8_t scale, const RGB &c) {
  for (uint8_t yy=0; yy<scale; yy++)
    for (uint8_t xx=0; xx<scale; xx++)
      putPixel(x+xx,y+yy,c);
}

void drawDigit3x5Scaled(uint8_t d, int16_t x, int16_t y, uint8_t scale, const RGB &c) {
  if (d>9) return;
  for (uint8_t row=0; row<5; row++)
    for (uint8_t col=0; col<3; col++)
      if (digits3x5[d][row] & (1 << (2-col)))
        fillCountdownBlock(x + col*scale, y + row*scale, scale, c);
}

void drawOriginalCountdownHourglass(uint8_t frame, uint8_t scale) {
  const RGB colors[4] = { RGB::Black, RGB::White, RGB(242,119,6), RGB(146,86,61) };
  frame %= 10;
  for (uint8_t sy=0; sy<10; sy++) {
    for (uint8_t sx=0; sx<7; sx++) {
      const uint8_t pi = countdownHourglassFrames[frame][sy*7+sx];
      if (!pi) continue;
      fillCountdownBlock(sx*scale, (4+sy)*scale, scale, colors[pi]);
    }
  }
}

void renderCountdown(uint32_t remainMs) {
  const uint32_t remainSec=(remainMs+999UL)/1000UL;
  const uint8_t mm=(remainSec/60UL)%100UL, ss=remainSec%60UL;
  const uint8_t scale=(uint8_t)(MATRIX_WIDTH/16U);
  const RGB minuteColor=RGB::White;
  const RGB normalSecondColor(242,119,6);
  const RGB secondColor = remainSec<=10UL ? RGB::Red : normalSecondColor;
  const RGB colonColor(242,119,6);

  clearFramebuffer(RGB::Black);

  // Manufacturer 16x16 animation: ten frames at 200 ms each.  On larger
  // logical matrices the same pixel artwork is enlarged cleanly rather than
  // being produced on a 16x16 framebuffer and post-scaled.
  const uint8_t frame = (remainSec == 0UL) ? 9U : (uint8_t)((now/200UL)%10UL);
  drawOriginalCountdownHourglass(frame,scale);

  // Original device layout: minutes above seconds on the right side.
  drawDigit3x5Scaled(mm/10,9*scale,3*scale,scale,minuteColor);
  drawDigit3x5Scaled(mm%10,13*scale,3*scale,scale,minuteColor);
  drawDigit3x5Scaled(ss/10,9*scale,9*scale,scale,secondColor);
  drawDigit3x5Scaled(ss%10,13*scale,9*scale,scale,secondColor);

  // The original 16x16 separator is squeezed against the hourglass at x=7.
  // Preserve it exactly at 16x16, but center each dot in the wider 2*scale
  // gap available on 32x32 and 64x64.  The separator itself does not turn red;
  // only the seconds do during the final ten seconds.
  const int16_t colonShift = (MATRIX_WIDTH>=64) ? 2 : ((MATRIX_WIDTH>=32) ? 1 : 0);
  const int16_t colonX = 7*scale + (scale>1 ? scale/2 : 0) + colonShift;
  if (((now/500UL)&1U)==0U) {
    fillCountdownBlock(colonX,10*scale,scale,colonColor);
    fillCountdownBlock(colonX,12*scale,scale,colonColor);
  }


}

// Original 16x16 stopwatch artwork captured from the manufacturer device.
// Revalidated frame-by-frame against the supplied native 16x16 reference video.
// Palette indexes: 0=black, 1=white face, 2=orange button, 3=gray/lilac case,
// 4=red hand. Only the 7x9 stopwatch icon area is stored; MM/SS and the
// blinking orange separator are rendered dynamically.
static constexpr uint8_t stopwatchFrames[8][63] = {
  {0,2,2,2,2,2,0, 0,0,2,2,2,0,0, 0,3,3,3,3,3,0, 3,1,1,1,1,1,3, 3,1,1,4,1,1,3, 3,1,1,4,1,1,3, 3,1,1,1,1,1,3, 3,1,1,1,1,1,3, 0,3,3,3,3,3,0},
  {0,2,2,2,2,2,0, 0,0,2,2,2,0,0, 0,3,3,3,3,3,0, 3,1,1,1,1,1,3, 3,1,1,1,4,1,3, 3,1,1,4,1,1,3, 3,1,1,1,1,1,3, 3,1,1,1,1,1,3, 0,3,3,3,3,3,0},
  {0,2,2,2,2,2,0, 0,0,2,2,2,0,0, 0,3,3,3,3,3,0, 3,1,1,1,1,1,3, 3,1,1,1,1,1,3, 3,1,1,4,4,1,3, 3,1,1,1,1,1,3, 3,1,1,1,1,1,3, 0,3,3,3,3,3,0},
  {0,2,2,2,2,2,0, 0,0,2,2,2,0,0, 0,3,3,3,3,3,0, 3,1,1,1,1,1,3, 3,1,1,1,1,1,3, 3,1,1,4,1,1,3, 3,1,1,1,4,1,3, 3,1,1,1,1,1,3, 0,3,3,3,3,3,0},
  {0,2,2,2,2,2,0, 0,0,2,2,2,0,0, 0,3,3,3,3,3,0, 3,1,1,1,1,1,3, 3,1,1,1,1,1,3, 3,1,1,4,1,1,3, 3,1,1,4,1,1,3, 3,1,1,1,1,1,3, 0,3,3,3,3,3,0},
  {0,2,2,2,2,2,0, 0,0,2,2,2,0,0, 0,3,3,3,3,3,0, 3,1,1,1,1,1,3, 3,1,1,1,1,1,3, 3,1,1,4,1,1,3, 3,1,4,1,1,1,3, 3,1,1,1,1,1,3, 0,3,3,3,3,3,0},
  {0,2,2,2,2,2,0, 0,0,2,2,2,0,0, 0,3,3,3,3,3,0, 3,1,1,1,1,1,3, 3,1,1,1,1,1,3, 3,1,4,4,1,1,3, 3,1,1,1,1,1,3, 3,1,1,1,1,1,3, 0,3,3,3,3,3,0},
  {0,2,2,2,2,2,0, 0,0,2,2,2,0,0, 0,3,3,3,3,3,0, 3,1,1,1,1,1,3, 3,1,4,1,1,1,3, 3,1,1,4,1,1,3, 3,1,1,1,1,1,3, 3,1,1,1,1,1,3, 0,3,3,3,3,3,0}
};

void drawOriginalStopwatch(uint8_t frame, uint8_t scale) {
  const RGB colors[5] = {
    RGB::Black,
    RGB::White,
    RGB(242,119,6),
    RGB(164,158,165),
    RGB(220,5,39)
  };
  frame &= 7U;
  for (uint8_t sy=0; sy<9; sy++) {
    for (uint8_t sx=0; sx<7; sx++) {
      const uint8_t pi=stopwatchFrames[frame][sy*7+sx];
      if (!pi) continue;
      fillCountdownBlock(sx*scale,(4+sy)*scale,scale,colors[pi]);
    }
  }
}

void renderStopwatch(uint32_t elapsedMs) {
  const uint32_t elapsedSec=elapsedMs/1000UL;
  const uint8_t mm=(elapsedSec/60UL)%100UL, ss=elapsedSec%60UL;
  const uint8_t scale=(uint8_t)(MATRIX_WIDTH/16U);
  const RGB minuteColor=RGB::White;
  const RGB secondColor(242,119,6);
  const RGB colonColor(242,119,6);

  clearFramebuffer(RGB::Black);

  // Manufacturer artwork: eight hand positions at 100 ms per frame.  Using
  // elapsed time rather than wall-clock time naturally freezes the hand when
  // the stopwatch is paused and resumes from the same phase afterwards.
  const uint8_t frame=(uint8_t)((elapsedMs/100UL)&7U);
  drawOriginalStopwatch(frame,scale);

  // Original-device layout: minutes above seconds on the right.  On 32x32
  // and 64x64 the separator is centered in the enlarged gap rather than
  // inheriting the cramped 16x16 placement.
  drawDigit3x5Scaled(mm/10,9*scale,3*scale,scale,minuteColor);
  drawDigit3x5Scaled(mm%10,13*scale,3*scale,scale,minuteColor);
  drawDigit3x5Scaled(ss/10,9*scale,9*scale,scale,secondColor);
  drawDigit3x5Scaled(ss%10,13*scale,9*scale,scale,secondColor);
  const int16_t colonShift=(MATRIX_WIDTH>=64) ? 2 : ((MATRIX_WIDTH>=32) ? 1 : 0);
  const int16_t colonX=7*scale+(scale>1 ? scale/2 : 0)+colonShift;
  if (((now/500UL)&1U)==0U) {
    fillCountdownBlock(colonX,10*scale,scale,colonColor);
    fillCountdownBlock(colonX,12*scale,scale,colonColor);
  }


}

// Original-device Scoreboard artwork (16x16 reference supplied from hardware).
// The real display uses two full-width 3-digit rows: blue player A above red
// player B.  Each glyph is a 4x7 pixel-outline digit, with one blank column
// between digits and one blank pixel at each horizontal edge.
static constexpr uint8_t SCORE_DIGITS_4X7[10][7] = {
  {0xF,0x9,0x9,0x9,0x9,0x9,0xF}, // 0
  {0x2,0x6,0x2,0x2,0x2,0x2,0x7}, // 1
  {0xF,0x1,0x1,0xF,0x8,0x8,0xF}, // 2
  {0xF,0x1,0x1,0xF,0x1,0x1,0xF}, // 3
  {0x9,0x9,0x9,0xF,0x1,0x1,0x1}, // 4
  {0xF,0x8,0x8,0xF,0x1,0x1,0xF}, // 5
  {0xF,0x8,0x8,0xF,0x9,0x9,0xF}, // 6
  {0xF,0x1,0x1,0x2,0x2,0x4,0x4}, // 7
  {0xF,0x9,0x9,0xF,0x9,0x9,0xF}, // 8
  {0xF,0x9,0x9,0xF,0x1,0x1,0xF}  // 9
};

void drawScoreDigit4x7(uint8_t digit, int16_t x, int16_t y, const RGB &c) {
  if(digit>9) return;
  for(uint8_t row=0; row<7; ++row){
    const uint8_t bits=SCORE_DIGITS_4X7[digit][row];
    for(uint8_t col=0; col<4; ++col){
      if(bits & (1U << (3-col))) putPixel(x+col,y+row,c);
    }
  }
}

void drawScore3Digit(uint16_t score, int16_t y, const RGB &c) {
  score%=1000;
  drawScoreDigit4x7((score/100)%10,1,y,c);
  drawScoreDigit4x7((score/10)%10,6,y,c);
  drawScoreDigit4x7(score%10,11,y,c);
}

void renderScoreboard(uint16_t scoreA, uint16_t scoreB) {
  clearFramebuffer(RGB::Black);
  // Captured original-device palette.
  const RGB playerAColor(120,88,248);   // #7858F8, sampled from the official app UI
  const RGB playerBColor(248,32,120);    // #F82078, sampled from the official app UI

  // Exact 16x16 manufacturer composition: rows 0..6 and 9..15, leaving
  // two blank scanlines between players.  Larger logical profiles preserve
  // this pixel-art composition by scaling the legacy 16x16 canvas.
  scaleLegacy = true;
  drawScore3Digit(scoreA,0,playerAColor);
  drawScore3Digit(scoreB,9,playerBColor);
  scaleLegacy = false;

}

    void combined(unsigned hour, unsigned minute, unsigned day, unsigned month) {
        const RGB displayColor = clockwiseClockDisplayColor(clockStyle, clockColor);
        const RGB fg = clockStyle == 3 ? RGB::Black : displayColor;
        clearFramebuffer(clockStyle == 3 ? displayColor : RGB::Black);
        if (clockStyle == 0) drawClockwiseRainbowBorder(now);
        drawNative64ClockRow(hour/10,hour%10,minute/10,minute%10,12,fg,false);
        drawNative64ClockRow(day/10,day%10,month/10,month%10,38,fg,true);
    }
};
}

void renderClockView(ClockPixel pixel, uint32_t now, uint8_t hour, uint8_t minute,
                     uint8_t day, uint8_t month, uint32_t settings, bool datePage) {
    const uint8_t flags = settings >> 24;
    const uint8_t style = flags & 7;
    if (!(flags & 0x40)) hour = (hour + 11) % 12 + 1;
    Renderer renderer{pixel, now, {uint8_t(settings >> 16), uint8_t(settings >> 8), uint8_t(settings)},
                      style, bool(datePage), (now / 500u) % 2 == 0, false};
    if (day && (flags & 0x80) && (style == 0 || style == 3))
        renderer.combined(hour, minute, day, month);
    else if (day && datePage)
        renderer.renderClockwiseClock(day, month);
    else {
        renderer.clockRenderingDate = false;
        renderer.renderClockwiseClock(hour, minute);
    }
}

void renderUtilityView(ClockPixel pixel, uint32_t now, unsigned mode, uint32_t value) {
    Renderer renderer{pixel, now, {}, 0, false, false, false};
    switch (mode) {
    case 2: renderer.renderCountdown(value); break;
    case 3: renderer.renderStopwatch(value); break;
    case 4: renderer.renderScoreboard(value & 0xffff, value >> 16); break;
    }
}
