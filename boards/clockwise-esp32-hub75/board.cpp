#include "board.h"
#include "app.h"
#include "layouts/native64/native_renderer.h"
#include "ESP32-HUB75-MatrixPanel-I2S-DMA.h"
#include "vendor/qrcode/qrcodegen.h"
#include "ported/features/pixel_screen/display_policy.h"
#include "ported/features/pixel_screen/layout_data.h"
#include <algorithm>
#include <atomic>
#include <driver/gpio.h>
#include <esp_log.h>
#include <esp_rom_gpio.h>
#include <esp_wifi.h>
#include <soc/gpio_sig_map.h>

namespace bridge::board {
namespace {
constexpr gpio_num_t bootPin = GPIO_NUM_0;
constexpr gpio_num_t outputEnable = GPIO_NUM_15;
constexpr unsigned maxDuty = 110;
constexpr unsigned setupDuty = 20;
std::atomic<bool> initialized{false};
std::atomic<unsigned> applied{0};
bool screenOn = true;
unsigned rotation = 0;
unsigned brightness = 80;
MatrixPanel_I2S_DMA matrix;
#ifdef CLOCKWISE_SCALE_LAYOUT
uint8_t frame[pixel_screen::pixels];
#else
uint32_t frame[pixel_screen::pixels64];
#endif
}

void prepareDisplay() {
    gpio_set_level(outputEnable, 1);
    gpio_set_pull_mode(outputEnable, GPIO_PULLUP_ONLY);
    gpio_set_direction(outputEnable, GPIO_MODE_OUTPUT);
}

static void beginMatrix() {
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

void begin() {
    gpio_set_direction(bootPin, GPIO_MODE_INPUT);
    gpio_set_pull_mode(bootPin, GPIO_PULLUP_ONLY);
    beginMatrix();
}

static void drawSetupLabel(const std::string &label, int y, uint8_t red, uint8_t green, uint8_t blue) {
    int x = (64 - (int(label.size()) * 4 - 1)) / 2;
    for (char ch : label) {
        for (const auto &glyph : pixel_screen::print_layout::font) {
            if (glyph.ch != ch) continue;
            for (int row = 0; row < 5; ++row)
                for (int column = 0; column < glyph.width; ++column)
                    if (glyph.rows[row] & (1u << (glyph.width - 1 - column)))
                        matrix.drawPixelRGB888(x + column, y + row, red, green, blue);
            break;
        }
        x += 4;
    }
}

static void drawSetupCode() {
    constexpr int version = 3;
    constexpr int outer = 52, left = 6, top = 6, quiet = 2;
    constexpr int inner = outer - quiet * 2;
    uint8_t temporary[qrcodegen_BUFFER_LEN_FOR_VERSION(version)];
    uint8_t code[qrcodegen_BUFFER_LEN_FOR_VERSION(version)];
    const auto payload = "WIFI:T:WPA;S:" + setupSsid() + ";P:" + setupPassword + ";;";
    if (!qrcodegen_encodeText(payload.c_str(), temporary, code, qrcodegen_Ecc_LOW,
                             version, version, qrcodegen_Mask_AUTO, true)) {
        ESP_LOGE("hub75", "Could not encode setup QR");
        return;
    }
    const int size = qrcodegen_getSize(code);
    matrix.clearScreen();
    matrix.setBrightness8(setupDuty);
    for (int y = 0; y < outer; ++y)
        for (int x = 0; x < outer; ++x) {
            const bool inCode = x >= quiet && x < outer - quiet && y >= quiet && y < outer - quiet;
            const bool dark = inCode && qrcodegen_getModule(code, (x - quiet) * size / inner,
                                                           (y - quiet) * size / inner);
            const uint8_t light = dark ? 0 : 255;
            matrix.drawPixelRGB888(left + x, top + y, light, light, light);
        }
    drawSetupLabel("WI-FI SETUP", 0, 0x00, 0xd6, 0xee);
    drawSetupLabel("SCAN TO JOIN", 59, 0xff, 0xc4, 0x00);
    ESP_LOGI("hub75", "Setup QR drawn");
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

void displaySnapshot(const pixel_screen::Snapshot &snapshot, bool online, const char *, uint32_t now) {
    if (!initialized) return;
    static bool paused = false;
    static bool showingSetup = false;
    if (!online) {
        wifi_sta_list_t clients{};
        const bool clientAttached = esp_wifi_ap_get_sta_list(&clients) == ESP_OK && clients.num > 0;
        if (clientAttached && !paused) {
            gpio_set_level(outputEnable, 1);
            esp_rom_gpio_connect_out_signal(outputEnable, SIG_GPIO_OUT_IDX, false, false);
            matrix.stopDMAoutput();
            paused = true;
            showingSetup = false;
            ESP_LOGI("hub75", "Panel output paused for setup client");
        }
        if (!clientAttached && !showingSetup) {
            drawSetupCode();
            if (paused) matrix.resumeDMAoutput();
            paused = false;
            showingSetup = true;
        }
        return;
    }
    if (paused || showingSetup) {
        matrix.clearScreen();
        if (paused) matrix.resumeDMAoutput();
        paused = false;
        showingSetup = false;
        applied.store(0);
        ESP_LOGI("hub75", "Printer display restored after Wi-Fi setup");
    }
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
#ifdef CLOCKWISE_SCALE_LAYOUT
    pixel_screen::render(picture, now / 125, frame);
#else
    pixel_screen::render64(picture, now / 125, frame);
#endif
    constexpr unsigned size = pixel_screen::nativePanelSize;
    for (unsigned y = 0; y < size; ++y) {
        for (unsigned x = 0; x < size; ++x) {
#ifdef CLOCKWISE_SCALE_LAYOUT
            const auto c = pixel_screen::rgb(frame[(y / 2) * pixel_screen::width + x / 2],
                                             picture.filamentColor, picture.feedColor);
#else
            const auto c = frame[y * size + x];
#endif
            unsigned column = x, row = y;
            switch (rotation) {
                case 1: column = size - 1 - y; row = x; break;
                case 2: column = size - 1 - x; row = size - 1 - y; break;
                case 3: column = y; row = size - 1 - x; break;
            }
            matrix.drawPixelRGB888(column, row, c >> 16, c >> 8, c);
        }
    }
}
}
