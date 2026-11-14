#include "board.h"
#include "ESP32-HUB75-MatrixPanel-I2S-DMA.h"
#include "ported/features/pixel_screen/display_policy.h"
#include "ported/features/pixel_screen/palette.h"
#include <algorithm>
#include <atomic>
#include <driver/gpio.h>
#include <esp_log.h>

namespace bridge::board {
namespace {
constexpr gpio_num_t bootPin = GPIO_NUM_0;
constexpr gpio_num_t outputEnable = GPIO_NUM_15;
// Preserve the existing 16/255 duty at the normal 80% setting.
constexpr unsigned maxDuty = 20;
std::atomic<bool> initialized{false};
std::atomic<unsigned> applied{0};
bool screenOn = true;
unsigned rotation = 0;
unsigned brightness = 80;
MatrixPanel_I2S_DMA matrix;
uint8_t frame[pixel_screen::pixels];
}

void prepareDisplay() {
    gpio_set_level(outputEnable, 1);
    gpio_set_pull_mode(outputEnable, GPIO_PULLUP_ONLY);
    gpio_set_direction(outputEnable, GPIO_MODE_OUTPUT);
}

void begin() {
    gpio_set_direction(bootPin, GPIO_MODE_INPUT);
    gpio_set_pull_mode(bootPin, GPIO_PULLUP_ONLY);
    HUB75_I2S_CFG cfg(64, 64, 1);
    cfg.gpio = {25, 26, 27, 14, 12, 13, 23, 19, 5, 17, 18, 4, outputEnable, 16};
    cfg.clkphase = false;
    cfg.setPixelColorDepthBits(6);
    matrix.setCfg(cfg);
    initialized = matrix.begin();
    if (!initialized) {
        ESP_LOGE("hub75", "Panel DMA initialization failed");
        return;
    }
    matrix.clearScreen();
    ESP_LOGI("hub75", "64x64 panel ready");
}

void configureDisplay(bool power, unsigned level, unsigned degrees) {
    screenOn = power;
    brightness = std::clamp(level, 5u, 100u);
    rotation = degrees / 90 % 4;
}

void displayStatus(JsonObject out) {
    const unsigned setting = applied.load();
    out["ready"] = initialized.load();
    out["screenOn"] = bool(setting & 1);
    out["rotation"] = ((setting >> 1) & 3) * 90;
    out["brightness"] = setting >> 3;
}

bool bootPressed() { return gpio_get_level(bootPin) == 0; }
void statusLed(bool) {}

void displaySnapshot(const pixel_screen::Snapshot &snapshot, bool, const char *, uint32_t now) {
    if (!initialized) return;
    const unsigned setting = (brightness << 3) | (rotation << 1) | screenOn;
    if (applied.load() != setting) {
        matrix.setBrightness8(screenOn ? (brightness * maxDuty + 50) / 100 : 0);
        matrix.clearScreen();
        applied.store(setting);
    }
    if (!screenOn) return;
    static uint32_t lastFrame = 0;
    if (now - lastFrame < 125) return;
    lastFrame = now;
    const auto plan = pixel_screen::displayPlan(snapshot, pixel_screen::IdleDisplay::Clock,
                                                snapshot.stateAgeMs);
    const auto &picture = plan.picture;
    pixel_screen::render(picture, now / 125, frame);
    for (unsigned y = 0; y < 32; ++y) {
        for (unsigned x = 0; x < 32; ++x) {
            const auto c = pixel_screen::rgb(frame[y * 32 + x], picture.filamentColor, picture.feedColor);
            unsigned column = x, row = y;
            switch (rotation) {
                case 1: column = 31 - y; row = x; break;
                case 2: column = 31 - x; row = 31 - y; break;
                case 3: column = y; row = 31 - x; break;
            }
            const unsigned left = 2 * column;
            const unsigned top = 2 * row;
            for (unsigned dy = 0; dy < 2; ++dy)
                for (unsigned dx = 0; dx < 2; ++dx)
                    matrix.drawPixelRGB888(left + dx, top + dy, c >> 16, c >> 8, c);
        }
    }
}
}
