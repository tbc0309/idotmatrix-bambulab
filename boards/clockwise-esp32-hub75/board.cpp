#include "board.h"
#include "ESP32-HUB75-MatrixPanel-I2S-DMA.h"
#include "ported/features/pixel_screen/display_policy.h"
#include "ported/features/pixel_screen/palette.h"
#include <driver/gpio.h>
#include <esp_log.h>

namespace bridge::board {
namespace {
constexpr gpio_num_t bootPin = GPIO_NUM_0;
constexpr uint8_t startupBrightness = 16;
MatrixPanel_I2S_DMA matrix;
bool initialized = false;
uint8_t frame[pixel_screen::pixels];
}

void begin() {
    gpio_set_direction(bootPin, GPIO_MODE_INPUT);
    gpio_set_pull_mode(bootPin, GPIO_PULLUP_ONLY);
    HUB75_I2S_CFG cfg(64, 64, 1);
    cfg.gpio = {25, 26, 27, 14, 12, 13, 23, 19, 5, 17, 18, 4, 15, 16};
    cfg.clkphase = false;
    cfg.setPixelColorDepthBits(6);
    matrix.setCfg(cfg);
    initialized = matrix.begin();
    if (!initialized) {
        ESP_LOGE("hub75", "Panel DMA initialization failed");
        return;
    }
    matrix.setBrightness8(startupBrightness);
    matrix.clearScreen();
    ESP_LOGI("hub75", "64x64 panel ready, brightness %u/255", startupBrightness);
}

bool bootPressed() { return gpio_get_level(bootPin) == 0; }
void statusLed(bool) {}

void displaySnapshot(const pixel_screen::Snapshot &snapshot, bool, const char *, uint32_t now) {
    if (!initialized) return;
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
            for (unsigned dy = 0; dy < 2; ++dy)
                for (unsigned dx = 0; dx < 2; ++dx)
                    matrix.drawPixelRGB888(x * 2 + dx, y * 2 + dy, c >> 16, c >> 8, c);
        }
    }
}
}
