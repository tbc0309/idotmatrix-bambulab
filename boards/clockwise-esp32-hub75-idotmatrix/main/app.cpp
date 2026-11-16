#include "app_state.h"
#if CLOCKWISE_WITH_WIFI
#include "portal.h"
#endif
#include "ESP32-HUB75-MatrixPanel-I2S-DMA.h"
#include "lvgl.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <driver/gpio.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <nvs.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs_flash.h>

bool clockwise_effect_active(uint32_t now, bool &redraw);
lv_color_t clockwise_effect_pixel(unsigned x, unsigned y, uint32_t now);
void clockwise_effect_stop();
uint8_t *clockwise_take_media();

static_assert(LVGL_VERSION_MAJOR == 9 && LVGL_VERSION_MINOR == 6 && LVGL_VERSION_PATCH == 0,
              "Clockwise requires LVGL 9.6.0");

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
std::atomic<uint32_t> resetAt{0};
std::atomic<uint32_t> requestedClockColor{UINT32_MAX};
constexpr gpio_num_t boot = GPIO_NUM_0;

void eraseSettings(const char *name) {
    nvs_handle_t handle;
    if (nvs_open(name, NVS_READWRITE, &handle) == ESP_OK) {
        if (nvs_erase_all(handle) == ESP_OK) nvs_commit(handle);
        nvs_close(handle);
    }
}

void reset(bool factoryReset) {
    for (const char *name : {"idotmatrix", "idm-auth", "idot-alarm", "idot-car", "idot-sched"}) eraseSettings(name);
    if (factoryReset) {
        eraseSettings("cw-native-wifi");
        eraseSettings("cw-wifi");
    }
    const auto *media = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "spiffs");
    if (media && esp_partition_erase_range(media, 0, media->size) != ESP_OK)
        ESP_LOGE("clockwise", "Failed to clear media partition");
    esp_restart();
}
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

extern "C" void clockwise_request_clock(uint32_t rgb) { requestedClockColor = rgb; }
extern "C" void clockwise_request_reset() { resetAt = lvTicks() + 150; }
extern "C" void clockwise_set_power(bool on) { powered = on; }
extern "C" void clockwise_set_brightness(uint8_t level) { brightness = std::min<unsigned>(level, 100); }
extern "C" void clockwise_set_clock(uint32_t seconds) {
    clockOffset = seconds - clockSeconds();
    clockSet = true;
}

extern "C" void app_main() {
    ESP_ERROR_CHECK(nvs_flash_init());
    gpio_set_direction(boot, GPIO_MODE_INPUT);
    gpio_set_pull_mode(boot, GPIO_PULLUP_ONLY);
    nvs_handle_t handle;
    if (nvs_open("idotmatrix", NVS_READONLY, &handle) == ESP_OK) {
        uint8_t savedBrightness;
        if (nvs_get_u8(handle, "brightness", &savedBrightness) == ESP_OK) clockwise_set_brightness(savedBrightness);
        nvs_close(handle);
    }
    gpio_set_level(outputEnable, 1);
    gpio_set_pull_mode(outputEnable, GPIO_PULLUP_ONLY);
    gpio_set_direction(outputEnable, GPIO_MODE_OUTPUT);
    HUB75_I2S_CFG config(side, side, 1);
    config.gpio = {25, 26, 27, 14, 12, 13, 23, 19, 5, 17, 18, 4, outputEnable, 16};
    config.clkphase = false;
    config.setPixelColorDepthBits(8);
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
#if CLOCKWISE_WITH_WIFI
    clockwise_wifi_start();
    bool setupPaused = false;
#endif
    ESP_LOGI("clockwise", "HUB75/LVGL ready, heap=%zu largest=%zu",
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

    unsigned lastDuty = 256;
    uint32_t lastMinute = UINT32_MAX;
    bool bootPressed = false;
    bool bootReleased = false;
    uint32_t bootAt = 0;
    uint8_t *currentMedia = nullptr;
    bool showingContent = false;
    for (;;) {
        const uint32_t now = lvTicks();
        if (gpio_get_level(boot) == 0) {
            if (!bootPressed) bootAt = now;
            bootPressed = true;
        } else {
            if (bootPressed && bootReleased && now - bootAt >= 10000) reset(true);
            bootPressed = false;
            bootReleased = true;
        }
        const uint32_t requestedReset = resetAt.load();
        if (requestedReset && int32_t(now - requestedReset) >= 0) reset(false);
#if CLOCKWISE_WITH_WIFI
        clockwise_wifi_poll();
        if (clockwise_wifi_has_setup_client()) {
            if (!setupPaused) {
                panel.clearScreen();
                ESP_LOGI("clockwise", "HUB75 drawing paused for provisioning");
                setupPaused = true;
            }
            vTaskDelay(pdMS_TO_TICKS(40));
            continue;
        }
        if (setupPaused) {
            lv_obj_invalidate(screen);
            showingContent = false;
            setupPaused = false;
            ESP_LOGI("clockwise", "HUB75 drawing resumed");
        }
#endif
        const unsigned duty = powered ? (brightness.load() * maxDuty + 50) / 100 : 0;
        if (lastDuty != duty) {
            panel.setBrightness8(duty);
            lv_obj_invalidate(screen);
            lastDuty = duty;
            showingContent = false;
        }
        const auto color = requestedClockColor.exchange(UINT32_MAX);
        if (color != UINT32_MAX) {
            free(currentMedia);
            currentMedia = nullptr;
            free(clockwise_take_media());
            clockwise_effect_stop();
            lv_obj_set_style_text_color(clock, lv_color_hex(color), 0);
            lv_obj_invalidate(screen);
        }
        bool redrawEffect;
        const bool effect = clockwise_effect_active(now, redrawEffect);
        uint8_t *nextMedia = clockwise_take_media();
        if (nextMedia) {
            free(currentMedia);
            currentMedia = nextMedia;
            clockwise_effect_stop();
        } else if (effect && currentMedia) {
            free(currentMedia);
            currentMedia = nullptr;
        }
        if ((effect && !nextMedia) || currentMedia) {
            if (redrawEffect || nextMedia || !showingContent) {
                for (unsigned y = 0; y < side; ++y) {
                    for (unsigned x = 0; x < side; ++x) {
                        if (currentMedia) {
                            const uint8_t *p = currentMedia + 3 * (y * side + x);
                            panel.drawPixelRGB888(x, y, p[0], p[1], p[2]);
                        } else {
                            const auto color = clockwise_effect_pixel(x, y, now);
                            panel.drawPixelRGB888(x, y, color.red, color.green, color.blue);
                        }
                    }
                }
            }
            showingContent = true;
            vTaskDelay(pdMS_TO_TICKS(15));
            continue;
        }
        if (showingContent) {
            lv_obj_invalidate(screen);
            showingContent = false;
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
