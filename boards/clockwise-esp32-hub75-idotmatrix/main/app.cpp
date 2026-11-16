#include "app_state.h"
#include "ESP32-HUB75-MatrixPanel-I2S-DMA.h"
#include "lvgl.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <driver/gpio.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs_flash.h>

namespace {
constexpr unsigned side = 64;
constexpr unsigned maxDuty = 20;
constexpr gpio_num_t outputEnable = GPIO_NUM_15;
MatrixPanel_I2S_DMA panel;
alignas(4) uint8_t drawBuffer[side * 8 * 3];
std::atomic<bool> powered{true};
std::atomic<unsigned> brightness{80};
std::atomic<uint32_t> clockOffset{0};
std::atomic<bool> clockSet{false};
uint32_t clockSeconds() { return esp_timer_get_time() / 1000000; }
uint32_t lvTicks() { return esp_timer_get_time() / 1000; }

void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels) {
    const auto *colors = reinterpret_cast<const lv_color_t *>(pixels);
    for (int y = area->y1; y <= area->y2; ++y) {
        for (int x = area->x1; x <= area->x2; ++x, ++colors) {
            panel.drawPixelRGB888(x, y, colors->red, colors->green, colors->blue);
        }
    }
    lv_display_flush_ready(display);
}
}

extern "C" void clockwise_set_power(bool on) { powered = on; }
extern "C" void clockwise_set_brightness(uint8_t level) { brightness = std::min<unsigned>(level, 100); }
extern "C" void clockwise_set_clock(uint32_t seconds) {
    clockOffset = seconds - clockSeconds();
    clockSet = true;
}

extern "C" void app_main() {
    ESP_ERROR_CHECK(nvs_flash_init());
    gpio_set_level(outputEnable, 1);
    gpio_set_pull_mode(outputEnable, GPIO_PULLUP_ONLY);
    gpio_set_direction(outputEnable, GPIO_MODE_OUTPUT);
    HUB75_I2S_CFG config(side, side, 1);
    config.gpio = {25, 26, 27, 14, 12, 13, 23, 19, 5, 17, 18, 4, outputEnable, 16};
    config.clkphase = false;
    config.setPixelColorDepthBits(6);
    panel.setCfg(config);
    if (!panel.begin()) {
        ESP_LOGE("clockwise", "Unable to initialize HUB75 DMA");
        return;
    }
    panel.clearScreen();
    lv_init();
    lv_tick_set_cb(lvTicks);
    auto *display = lv_display_create(side, side);
    if (!display) return;
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB888);
    lv_display_set_buffers(display, drawBuffer, nullptr, sizeof(drawBuffer), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    auto *screen = lv_display_get_screen_active(display);
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    auto *clock = lv_label_create(screen);
    lv_obj_set_style_text_color(clock, lv_color_hex(0x23e5b3), 0);
    lv_obj_center(clock);
    lv_label_set_text(clock, "--:--");
    clockwise_ble_start();
    ESP_LOGI("clockwise", "HUB75/LVGL ready, heap=%zu largest=%zu",
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

    unsigned lastDuty = 256;
    uint32_t lastMinute = UINT32_MAX;
    for (;;) {
        const unsigned duty = powered ? (brightness.load() * maxDuty + 50) / 100 : 0;
        if (lastDuty != duty) {
            panel.setBrightness8(duty);
            lv_obj_invalidate(screen);
            lastDuty = duty;
        }
        if (clockSet) {
            const uint32_t minute = (clockSeconds() + clockOffset.load()) % 86400 / 60;
            if (minute != lastMinute) {
                char text[8];
                snprintf(text, sizeof(text), "%02u:%02u", unsigned(minute / 60), unsigned(minute % 60));
                lv_label_set_text(clock, text);
                lastMinute = minute;
            }
        }
        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(15));
    }
}
