#include "app_state.h"
#include "rhythm_art.h"
#include "clock_view.h"
#include <algorithm>
#include <cstring>
#include <freertos/FreeRTOS.h>

namespace {
struct RGB { uint8_t r, g, b; };
struct Sample {
    bool fft{};
    uint8_t mode{}, level{}, wireBands[16]{};
    uint32_t packetCounter{};
};
struct Playback { uint32_t frameAt{}; uint8_t mode{255}, frame{}; };
portMUX_TYPE rhythmLock = portMUX_INITIALIZER_UNLOCKED;
Sample pending;
Playback clockwiseRhythmPlayback;
uint32_t consumed, counter;
using std::min;
using std::max;
uint8_t bandHeight(uint8_t value) { return min<uint8_t>(value, 12); }

struct Renderer {
    ClockPixel pixel;
    uint32_t now;
    Sample audioState;
    void putPixel(unsigned x, unsigned y, RGB color) { pixel(x, y, color.r, color.g, color.b); }
static RGB rhythmColor(uint16_t rgb) {
  const uint8_t red = rgb >> 11;
  const uint8_t green = (rgb >> 5) & 63U;
  const uint8_t blue = rgb & 31U;
  return RGB((red << 3) | (red >> 2), (green << 2) | (green >> 4), (blue << 3) | (blue >> 2));
}

void renderClockwiseRhythm() {
  using namespace clockwiseRhythm;

  const auto& effect = levelEffects[audioState.mode];
  auto& playback = clockwiseRhythmPlayback;

  if (playback.mode != audioState.mode) {
    playback = {now, audioState.mode, 0};
  }
  const uint16_t interval = effect.frameMs * (16 - audioState.level) / 12;
  if (audioState.level == 0) {
    playback.frameAt = now;
  } else if (now - playback.frameAt >= interval) {
    playback.frame = (playback.frame + 1) % effect.frameCount;
    playback.frameAt = now;
  }
  const auto& packed = effect.frames[playback.frame];
  for (uint16_t i = 0; i < 256; ++i) {
    const uint8_t colorIndex = (packed[i / 2] >> ((i % 2) * 4)) & 0x0fU;
    if (colorIndex)
      for (unsigned y = 0; y < 4; ++y)
        for (unsigned x = 0; x < 4; ++x)
          putPixel(i % 16 * 4 + x, i / 16 * 4 + y, rhythmColor(effect.palette[colorIndex]));
  }

}
// The app sends a display-ordered pair of mirrored eight-band halves.
void renderClockwiseSpectrum() {
  static constexpr uint32_t palettes[5][16] = {
    {0x04c9ff,0x2b7aff,0x5034ff,0x7e00ff,0xab00ff,0xdb00ff,0xfd00fd,0xff00b5,
     0xff0051,0xff2100,0xff2000,0xff4d00,0xff7d00,0xffa900,0xffd200,0xfff100},
    {0x7aff0e,0xfff300,0xff1d10,0xcb46df,0x2a96ff,0x61f52f,0xfffe01,0xff1d10,
     0xff1d10,0xfffe01,0x61f52f,0x2a96ff,0xcb46df,0xff1d10,0xfff300,0x7aff0e},
    {0x74f101,0x92fb00,0xc3ff00,0xf0ff00,0xffea14,0xffac4a,0xff5f8d,0xff1fca,
     0xfa00f6,0x6883ff,0x00e5ff,0x15aeff,0x3967ff,0x5a49ff,0x7b5dff,0x9781ff},
    {0x02dcff,0x1ac7ff,0x39aaff,0x5e89ff,0x8862ff,0xb13eff,0xd421ff,0xf209ff,
     0xff00f0,0xff00d6,0xff05b3,0xff118f,0xff2167,0xff3241,0xff4120,0xff4c07},
    {0x0001ff,0x0001ff,0x00faff,0x00faff,0xff00ad,0xff00ad,0x9c00ff,0x9c00ff,
     0x0001ff,0x9c00ff,0xff00ad,0xff00ad,0x00faff,0x00faff,0x0001ff,0x0001ff},
  };
  static constexpr uint8_t heartTop[] = {2,5,3,2,2,2,3,4,4,3,2,2,2,3,5,2};
  static constexpr uint8_t heartBottom[] = {9,6,7,9,10,11,12,13,13,12,11,10,9,7,6,9};
  const uint8_t mode = audioState.mode;
  uint8_t heights[16];
  // The wire range is 0..12. Match the preview's eight rows per half.
  for (uint8_t i = 0; i < 16; ++i) {
    heights[i] = (2 * bandHeight(audioState.wireBands[i]) + 2) / 3;
  }
  if (mode != 4) {
    // Recreate the Android preview's arrangement from the available wire values.
    // This only affects spatial placement; it does not change sampling cadence.
    static constexpr uint8_t rankForColumn[] = {1,3,5,7,9,11,13,15,14,12,10,8,6,4,2,0};
    uint8_t ranked[16];
    memcpy(ranked, heights, sizeof(ranked));
    std::sort(ranked, ranked + 16);
    for (uint8_t i = 0; i < 16; ++i) {
      const uint8_t height = ranked[rankForColumn[i]];
      heights[i] = i == 7 || i == 8 || height <= 1 ? height : height * 2 / 3;
    }
  }

  for (uint8_t column = 0; column < 16; ++column) {
    uint8_t height = heights[column];
    if (mode >= 2 && column >= 1 && column <= 2) height = heights[16 - column];
    if (mode == 4) {
      // The hourglass has its own per-column caps, rather than the rank layout.
      static constexpr uint8_t caps[] = {3,4,3,2,7,4,5,8,8,5,4,7,2,3,4,3};
      if (height >= caps[column]) {
        if (column <= 3 || column >= 12) {
          const uint32_t phase = audioState.packetCounter + column * 13;
          height = column == 0 || column == 15 ? phase % 4 : 2 + phase % 3;
        } else {
          height = min(height, caps[column]);
        }
      }
    }
    int first = 8 - height;
    int end = 8 + height;
    if (mode == 1 && height == 0) {
      first = 7;
      end = 9;
    } else if (mode == 2) {
      first = min((int)heartTop[column] + 2, max(0, heartTop[column] + 2 - height));
      end = min(16, max(first + 1, heartBottom[column] + height - 1));
    } else if (mode == 3) {
      first = min(first, 7);
      end = max(end, 9);
    } else if (mode == 4) {
      first = 0;
      end = 16;
    }
    for (int row = first; row < end; ++row) {
      if (mode == 4 && row >= height && row < 16 - height) continue;
      const uint8_t colorIndex = mode == 1 ? row : column;
      const uint32_t rgb = palettes[mode][colorIndex];
      const RGB color(rgb >> 16, (rgb >> 8) & 255U, rgb & 255U);
      int x = 5 + (52 * column + 7) / 15;
      int y = 10 + (42 * row + 7) / 15;
      if (mode == 3 || mode == 4) {
        x = 10 + (42 * (mode == 3 ? row : column) + 7) / 15;
        y = 5 + (52 * (mode == 3 ? column : row) + 7) / 15;
      }
      putPixel(x, y, color);
      putPixel(x + 1, y, color);
      putPixel(x, y + 1, color);
      putPixel(x + 1, y + 1, color);
    }
  }
}

};
}

extern "C" bool clockwise_rhythm_submit(const uint8_t *data, size_t length) {
    Sample sample;
    if (length == 6 && data[2] == 0 && data[5] >= 1 && data[5] <= 5) {
        sample.level = std::min<uint8_t>(data[4], 12);
        sample.mode = data[5] - 1;
    } else if (length == 21 && data[2] == 1 && data[4] < 5) {
        sample.fft = true;
        sample.mode = data[4];
        memcpy(sample.wireBands, data + 5, 16);
    } else return false;
    portENTER_CRITICAL(&rhythmLock);
    sample.packetCounter = ++counter;
    pending = sample;
    portEXIT_CRITICAL(&rhythmLock);
    return true;
}

bool renderRhythm(ClockPixel pixel, uint32_t now) {
    Sample sample;
    portENTER_CRITICAL(&rhythmLock);
    sample = pending;
    portEXIT_CRITICAL(&rhythmLock);
    if (sample.packetCounter == consumed) return false;
    consumed = sample.packetCounter;
    for (unsigned y = 0; y < 64; ++y)
        for (unsigned x = 0; x < 64; ++x) pixel(x, y, 0, 0, 0);
    Renderer renderer{pixel, now, sample};
    if (sample.fft) {
        clockwiseRhythmPlayback.mode = 255;
        renderer.renderClockwiseSpectrum();
    } else renderer.renderClockwiseRhythm();
    return true;
}
