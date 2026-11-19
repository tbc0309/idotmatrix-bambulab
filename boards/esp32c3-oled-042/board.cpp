#include "board.h"
#include "ported/features/pixel_screen/core.h"
#include "ported/features/pixel_screen/display_policy.h"
#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <esp_log.h>
#include <cstdio>
#include <cstring>
#include <climits>

namespace bridge::board {
namespace {
constexpr int width = 72, height = 40, columnOffset = 28;
constexpr uint8_t address = 0x3c;
constexpr gpio_num_t bootPin = GPIO_NUM_9;
i2c_master_dev_handle_t oled = nullptr;
uint8_t packet[1 + width * height / 8] = {0x40};
uint8_t previous[sizeof(packet)];
uint32_t lastUpdate = 0;

bool send(const uint8_t *data, size_t size) {
    return i2c_master_transmit(oled, data, size, 25) == ESP_OK;
}

void dot(int x, int y) {
    if (x < 0 || x >= width || y < 0 || y >= height) return;
    packet[1 + (y / 8) * width + x] |= uint8_t(1u << (y % 8));
}

void text(int y, const char *value, int scale, int origin = INT_MIN, bool thin = false) {
    int columns = -1;
    for (const char *c = value; *c; ++c) columns += *c == ':' ? 2 : 4;
    int x = origin == INT_MIN ? (width - columns * scale) / 2 : origin;
    static constexpr char symbols[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ%-:.";
    static constexpr uint16_t glyphs[] = {
        0x7b6f,0x2492,0x73e7,0x73cf,0x5bc9,0x79cf,0x79ef,0x7249,0x7bef,0x7bcf,
        0x2bed,0x6bae,0x3923,0x6b6e,0x79e7,0x79e4,0x396b,0x5bed,0x7497,0x124a,
        0x5bad,0x4927,0x5fed,0x5ffd,0x2b6a,0x6ba4,0x2b7b,0x6bad,0x39ce,0x7492,
        0x5b6f,0x5b6a,0x5bfd,0x5aad,0x5a92,0x72a7,0x52a5,0x01c0,0x0410,0x0002
    };
    for (; *value; ++value) {
        const int left = x;
        x += (*value == ':' ? 2 : 4) * scale;
        if (*value == ' ') continue;
        const char *symbol = strchr(symbols, *value);
        if (!symbol) continue;
        const uint16_t glyph = glyphs[symbol - symbols];
        for (int dy = 0; dy < 5; ++dy)
            for (int dx = 0; dx < 3; ++dx)
                if ((*value != ':' || dx == 1) && (glyph & (1u << (14 - 3 * dy - dx))))
                    for (int py = 0; py < scale; ++py)
                        for (int px = 0; px < scale; ++px)
                            {
                                if (thin) {
                                    auto solid = [&](int gx, int gy) {
                                        return gx >= 0 && gx < 3 * scale && gy >= 0 && gy < 5 * scale &&
                                            (glyph & (1u << (14 - 3 * (gy / scale) - gx / scale)));
                                    };
                                    const int gx = dx * scale + px, gy = dy * scale + py;
                                    if (!solid(gx - 1, gy) || !solid(gx + 1, gy) ||
                                        !solid(gx, gy - 1) || !solid(gx, gy + 1)) continue;
                                }
                                dot(left + (*value == ':' ? 0 : dx) * scale + px, y + dy * scale + py);
                            }
    }
}
}

void begin() {
    gpio_config_t button{};
    button.pin_bit_mask = 1ULL << bootPin;
    button.mode = GPIO_MODE_INPUT;
    button.pull_up_en = GPIO_PULLUP_ENABLE;
    ESP_ERROR_CHECK(gpio_config(&button));

    i2c_master_bus_config_t busConfig{};
    busConfig.i2c_port = I2C_NUM_0;
    busConfig.sda_io_num = GPIO_NUM_5;
    busConfig.scl_io_num = GPIO_NUM_6;
    busConfig.clk_source = I2C_CLK_SRC_DEFAULT;
    busConfig.glitch_ignore_cnt = 7;
    busConfig.flags.enable_internal_pullup = true;
    i2c_master_bus_handle_t bus = nullptr;
    if (i2c_new_master_bus(&busConfig, &bus) != ESP_OK) {
        ESP_LOGW("oled", "I2C initialization failed");
        return;
    }

    i2c_device_config_t device{};
    device.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    device.device_address = address;
    device.scl_speed_hz = 100000;
    const uint8_t init[] = {0x00, 0xae, 0xd5, 0x80, 0xa8, 39, 0xd3, 0, 0x40,
                            0x8d, 0x14, 0x20, 0, 0xa1, 0xc8, 0xda, 0x12,
                            0x81, 0x6f, 0xd9, 0x22, 0xdb, 0x20, 0xa4, 0xa6, 0xaf};
    if (i2c_master_bus_add_device(bus, &device, &oled) != ESP_OK || !send(init, sizeof(init))) {
        ESP_LOGW("oled", "No SSD1306 response at 0x3c");
        if (oled) i2c_master_bus_rm_device(oled);
        i2c_del_master_bus(bus);
        oled = nullptr;
        return;
    }
    ESP_LOGI("oled", "SSD1306 72x40 ready (SDA 5, SCL 6)");
}

bool bootPressed() { return gpio_get_level(bootPin) == 0; }

// The GPIO2 indicator on this board is an addressable LED.
void statusLed(bool) {}

void displaySnapshot(const pixel_screen::Snapshot &snapshot, bool wifi, const char *ip, uint32_t now) {
    if (!oled || uint32_t(now - lastUpdate) < 100) return;
    lastUpdate = now;
    memset(packet + 1, 0, sizeof(packet) - 1);

    using pixel_screen::State;
    const char *status = "OFFLINE";
    switch (snapshot.state) {
    case State::Idle: status = "READY"; break;
    case State::Preparing: status = "PREP"; break;
    case State::Printing: status = "PRINT"; break;
    case State::Paused: status = "PAUSE"; break;
    case State::Complete: status = "DONE"; break;
    case State::Cancelled: status = "STOP"; break;
    case State::Error: status = "ERROR"; break;
    default: break;
    }
    const auto plan = pixel_screen::displayPlan(snapshot, pixel_screen::IdleDisplay::Clock,
                                                snapshot.stateAgeMs);
    const bool idle = plan.picture.state == State::Idle;
    const bool showIp = idle && wifi && ip && *ip && snapshot.stateAgeMs % 15000 >= 10000;
    const char *title = !wifi ? "SETUP" : idle ? "READY" : status;
    text(1, title, strlen(title) <= 6 ? 3 : 2);
    char detail[10]{};
    if (snapshot.state == State::Preparing || snapshot.state == State::Printing ||
        snapshot.state == State::Paused)
        snprintf(detail, sizeof(detail), "%u%%", unsigned(snapshot.percent));
    else
        snprintf(detail, sizeof(detail), "%s", snapshot.clock);
    if (showIp) {
        // Keep the clock's size but use thinner strokes. Each 2.5-second pass
        // exposes the complete address within 1.8 seconds, twice per IP page.
        constexpr unsigned holdMs = 200, motionMs = 1600;
        const int contentWidth = (int(strlen(ip)) * 4 - 1) * 4;
        const int travel = contentWidth > width ? contentWidth - width : 0;
        unsigned elapsed = (snapshot.stateAgeMs % 15000 - 10000) % 2500;
        unsigned motion = elapsed > holdMs ? elapsed - holdMs : 0;
        if (motion > motionMs) motion = motionMs;
        const int x = travel ? -int(uint64_t(travel) * motion / motionMs)
                             : (width - contentWidth) / 2;
        text(18, ip, 4, x, true);
    } else text(18, detail, 4);

    if (!memcmp(previous, packet, sizeof(packet))) return;
    const uint8_t window[] = {0x00, 0x21, columnOffset, columnOffset + width - 1,
                              0x22, 0, height / 8 - 1};
    if (send(window, sizeof(window)) && send(packet, sizeof(packet)))
        memcpy(previous, packet, sizeof(packet));
}
}
