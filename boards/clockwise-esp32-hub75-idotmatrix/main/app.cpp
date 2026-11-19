#include "canvas_gif.h"
#include "lvgl_private.h"
#include "app_state.h"
#include "text_media.h"
#include "clock_view.h"
#include "events.h"
#include <cstring>
#include <ctime>
#include <iterator>
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
#include <esp_rom_crc.h>
#include <miniz.h>
#include <esp_system.h>
#include <nvs.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs_flash.h>

bool renderRhythm(ClockPixel pixel, uint32_t now);
bool clockwise_effect_active(uint32_t now, bool &redraw);
lv_color_t clockwise_effect_pixel(unsigned x, unsigned y, uint32_t now);
void clockwise_effect_stop();
void clockwise_media_mount();
void clockwise_media_stop_carousel();
void clockwise_media_shutdown();
bool clockwise_media_next(bool cycle, int &current, char (&path)[40], bool &graffitiPng);
int clockwise_media_format(const char *path);

static_assert(LVGL_VERSION_MAJOR == 9 && LVGL_VERSION_MINOR == 6 && LVGL_VERSION_PATCH == 0,
              "Clockwise requires LVGL 9.6.0");

namespace {
constexpr unsigned side = 64;
constexpr unsigned maxDuty = 100;
constexpr unsigned fullWhiteDuty = 80;
constexpr unsigned solidChannelBudget = 3 * 255 * fullWhiteDuty;
constexpr gpio_num_t outputEnable = GPIO_NUM_15;
MatrixPanel_I2S_DMA panel;
alignas(4) uint8_t drawBuffer[side * 8 * 3];
std::atomic<bool> powered{true};
std::atomic<unsigned> brightness{80};
std::atomic<uint32_t> clockOffset{0};
std::atomic<bool> calendarKnown{false};
std::atomic<bool> ntpKnown{false};
std::atomic<bool> clockSet{false};
std::atomic<uint32_t> resetAt{0};
std::atomic<uint32_t> requestedClockColor{UINT32_MAX};
portMUX_TYPE controlLock = portMUX_INITIALIZER_UNLOCKED;
uint8_t requestedView;
uint32_t requestedValue;
std::atomic<uint32_t> savingSettings{0};
std::atomic<bool> requestedFlip{false};
bool flipped = false;
enum class View { Clock, Solid, Countdown, Stopwatch, Score, Rhythm, Graffiti };
struct TimerState {
    uint32_t value{}, started{};
    bool running{};
    uint32_t remaining(uint32_t now) const {
        return running ? value - std::min(value, now - started) : value;
    }
    void update(unsigned action, unsigned initial, uint32_t now, bool down) {
        switch (action) {
        case 0: *this = {}; break;
        case 1: value = initial; started = now; running = true; break;
        case 2:
            if (running) value = down ? remaining(now) : value + now - started;
            running = false;
            break;
        case 3:
            if (!running && (!down || value)) { started = now; running = true; }
            break;
        }
    }
};

struct Pixel { uint8_t r, g, b; };
alignas(LV_DRAW_BUF_ALIGN) Pixel frame[side * side];
static_assert(sizeof(Pixel) == 3);
void writeFramePixel(unsigned x, unsigned y, uint8_t r, uint8_t g, uint8_t b) {
    if (x < side && y < side) frame[y * side + x] = {r, g, b};
}

void drawPixel(unsigned x, unsigned y, uint8_t r, uint8_t g, uint8_t b) {
    panel.drawPixelRGB888(flipped ? side - 1 - x : x, flipped ? side - 1 - y : y, r, g, b);
}

uint32_t clockSeconds();

void presentFrame() {
    for (unsigned y = 0; y < side; ++y)
        for (unsigned x = 0; x < side; ++x) {
            const auto &pixel = frame[y * side + x];
            drawPixel(x, y, pixel.r, pixel.g, pixel.b);
        }
}

uint8_t paeth(uint8_t left, uint8_t above, uint8_t corner) {
    const int estimate = int(left) + above - corner;
    const int dl = abs(estimate - left), da = abs(estimate - above), dc = abs(estimate - corner);
    return dl <= da && dl <= dc ? left : da <= dc ? above : corner;
}

uint32_t pngRead32(const uint8_t *bytes) {
    return uint32_t(bytes[0]) << 24 | uint32_t(bytes[1]) << 16 | uint32_t(bytes[2]) << 8 | bytes[3];
}

bool inflateGraffitiRows(FILE *file, size_t remaining, tinfl_decompressor &decoder,
                        uint8_t *rows, size_t capacity) {
    tinfl_init(&decoder);
    tinfl_status status = TINFL_STATUS_NEEDS_MORE_INPUT;
    size_t written = 0;
    uint8_t chunk[8], block[512], checksum[4];
    while (remaining >= sizeof(chunk) + sizeof(checksum)) {
        if (fread(chunk, 1, sizeof(chunk), file) != sizeof(chunk)) return false;
        const size_t length = pngRead32(chunk);
        if (length > remaining - sizeof(chunk) - sizeof(checksum)) return false;
        remaining -= sizeof(chunk) + length + sizeof(checksum);
        const bool imageData = !memcmp(chunk + 4, "IDAT", 4);
        uint32_t crc = esp_rom_crc32_le(0, chunk + 4, 4);
        for (size_t left = length; left;) {
            const size_t size = std::min(left, sizeof(block));
            if (fread(block, 1, size, file) != size) return false;
            crc = esp_rom_crc32_le(crc, block, size);
            left -= size;
            if (!imageData) continue;
            for (size_t input = 0; input < size;) {
                if (status == TINFL_STATUS_DONE) return false;
                size_t consumed = size - input;
                size_t produced = capacity - written;
                status = tinfl_decompress(&decoder, block + input, &consumed, rows, rows + written,
                                          &produced, TINFL_FLAG_PARSE_ZLIB_HEADER |
                                          TINFL_FLAG_HAS_MORE_INPUT | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
                if (status < TINFL_STATUS_DONE || (!consumed && !produced)) return false;
                input += consumed;
                written += produced;
            }
        }
        if (fread(checksum, 1, sizeof(checksum), file) != sizeof(checksum) || pngRead32(checksum) != crc)
            return false;
        if (!memcmp(chunk + 4, "IEND", 4))
            return length == 0 && status == TINFL_STATUS_DONE && written == capacity;
    }
    return false;
}

bool restoreGraffitiPng(const char *path, bool reuseFrame = false) {
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    fseek(file, 0, SEEK_END);
    const long size = ftell(file);
    rewind(file);
    uint8_t header[33]{};
    const bool headerRead = size >= long(sizeof(header)) && size <= 48 * 1024 &&
                            fread(header, 1, sizeof(header), file) == sizeof(header);
    const auto width = pngRead32(header + 16), height = pngRead32(header + 20);
    const unsigned channels = header[25] == 6 ? 4 : header[25] == 2 ? 3 : 0;
    bool valid = headerRead && !memcmp(header, "\x89PNG\r\n\x1a\n", 8) &&
                 pngRead32(header + 8) == 13 && !memcmp(header + 12, "IHDR", 4) &&
                 esp_rom_crc32_le(0, header + 12, 17) == pngRead32(header + 29) &&
                 (width == side || width == side / 2) && height == width && channels &&
                 header[24] == 8 && header[26] == 0 && header[27] == 0 && header[28] == 0;
    uint8_t *rows = nullptr;
    const size_t stride = width * channels + 1;
    if (valid) {
        static_assert(sizeof(tinfl_decompressor) <= sizeof(frame));
        static_assert(alignof(tinfl_decompressor) <= LV_DRAW_BUF_ALIGN);
        auto *decoder = reuseFrame ? ::new (static_cast<void *>(frame)) tinfl_decompressor :
                                     static_cast<tinfl_decompressor *>(malloc(sizeof(tinfl_decompressor)));
        rows = static_cast<uint8_t *>(malloc(stride * height));
        if (!decoder || !rows)
            ESP_LOGW("media", "PNG inflate allocation failed, largest=%zu",
                     heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        valid = decoder && rows && inflateGraffitiRows(file, size - sizeof(header), *decoder,
                                                       rows, stride * height);
        if (reuseFrame) {
            decoder->~tinfl_decompressor();
            for (auto &pixel : frame) ::new (static_cast<void *>(&pixel)) Pixel{};
        } else free(decoder);
    }
    fclose(file);
    for (unsigned y = 0; valid && y < height; ++y) {
        auto *row = rows + stride * y;
        const unsigned filter = *row++;
        if (filter > 4) { valid = false; break; }
        for (unsigned x = 0; x + 1 < stride; ++x) {
            const uint8_t left = x >= channels ? row[x - channels] : 0;
            const uint8_t above = y ? *(row + x - stride) : 0;
            const uint8_t corner = y && x >= channels ? *(row + x - stride - channels) : 0;
            const uint8_t predictor[] = {0, left, above, uint8_t((left + above) / 2), paeth(left, above, corner)};
            row[x] += predictor[filter];
        }
    }
    if (valid) {
        const unsigned scale = side / width;
        for (unsigned y = 0; y < side; ++y)
            for (unsigned x = 0; x < side; ++x) {
                const auto *pixel = rows + stride * (y / scale) + 1 + channels * (x / scale);
                const unsigned alpha = channels == 4 ? pixel[3] : 255;
                writeFramePixel(x, y, pixel[0] * alpha / 255,
                                pixel[1] * alpha / 255, pixel[2] * alpha / 255);
            }
        ESP_LOGI("clockwise", "Graffiti canvas restored (%ux%u)", width, height);
    } else ESP_LOGW("clockwise", "Graffiti PNG rejected");
    free(rows);
    return valid;
}

struct Point { uint8_t x, y; Pixel color; };
portMUX_TYPE graffitiLock = portMUX_INITIALIZER_UNLOCKED;
Point stroke[512];
unsigned strokeCount;
bool clearGraffiti, selectGraffiti;

unsigned displayDuty(unsigned percent) {
    const uint32_t settings = savingSettings.load();
    unsigned duty = (percent * maxDuty + 50) / 100;
    if (!clockSet || !(settings >> 31)) return duty;
    const unsigned minute = (clockSeconds() + clockOffset.load()) % 86400 / 60;
    const unsigned start = (settings >> 18) & 0x7ff;
    const unsigned end = (settings >> 7) & 0x7ff;
    if (start != end && (start < end ? minute >= start && minute < end : minute >= start || minute < end))
        duty = duty * (100 - (settings & 127)) / 100;
    return duty;
}
constexpr gpio_num_t boot = GPIO_NUM_0;

void eraseSettings(const char *name) {
    nvs_handle_t handle;
    if (nvs_open(name, NVS_READWRITE, &handle) == ESP_OK) {
        if (nvs_erase_all(handle) == ESP_OK) nvs_commit(handle);
        nvs_close(handle);
    }
}

void reset(bool factoryReset) {
    clockwise_media_shutdown();
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
            drawPixel(x, y, colors->red, colors->green, colors->blue);
        }
    }
    lv_display_flush_ready(display);
}
}

extern "C" bool clockwise_graffiti_submit(const uint8_t *data, size_t size) {
    if (data[2] == 4) {
        if (size != 5) return false;
        if (data[4] != 1 && data[4] != 3) return true;
    }
    // iOS sends one coordinate pair followed by a sequence byte.
    // The other form carries one or more coordinate pairs without a trailer.
    const size_t pointsEnd = data[2] == 5 && size == 11 ? 10 : size;
    if (data[2] == 5 && (pointsEnd < 10 || pointsEnd % 2)) return false;
    portENTER_CRITICAL(&graffitiLock);
    selectGraffiti = true;
    if (data[2] == 4 && data[4] == 1) {
        clearGraffiti = true;
        strokeCount = 0;
    }
    if (data[2] == 5) {
        if ((pointsEnd - 8) / 2 > std::size(stroke) - strokeCount) {
            portEXIT_CRITICAL(&graffitiLock);
            return false;
        }
        for (size_t i = 8; i < pointsEnd; i += 2)
            if (data[i] < side && data[i + 1] < side)
                stroke[strokeCount++] = {data[i], data[i + 1], {data[5], data[6], data[7]}};
    }
    portEXIT_CRITICAL(&graffitiLock);
    return true;
}

extern "C" void clockwise_set_flip(bool value) { requestedFlip = value; }
extern "C" void clockwise_set_saving(bool on, unsigned start, unsigned end, unsigned reduction) {
    savingSettings = (uint32_t(on) << 31) | (start << 18) | (end << 7) | reduction;
}
extern "C" void clockwise_request_view(uint8_t mode, uint32_t value) {
    portENTER_CRITICAL(&controlLock);
    requestedValue = value;
    requestedView = mode;
    portEXIT_CRITICAL(&controlLock);
}
extern "C" void clockwise_request_clock(uint32_t rgb) { requestedClockColor = rgb; }
extern "C" void clockwise_request_reset() { resetAt = lvTicks() + 150; }
extern "C" void clockwise_set_power(bool on) { powered = on; }
extern "C" void clockwise_set_brightness(uint8_t level) { brightness = std::min<unsigned>(level, 100); }
extern "C" void clockwise_set_calendar(uint32_t seconds, bool fromNtp) {
    if (ntpKnown && !fromNtp) return;
    clockOffset = seconds - clockSeconds();
    calendarKnown = true;
    ntpKnown = fromNtp;
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
    clockwise_media_mount();
    initEvents();
    clockwise_ble_start();
#if CLOCKWISE_WITH_WIFI
    clockwise_wifi_start();
    bool setupPaused = false;
#endif
    ESP_LOGI("clockwise", "HUB75/LVGL ready, heap=%zu largest=%zu",
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

    unsigned lastDuty = 256;
    bool lastSolid = false;
    unsigned observedBrightness = brightness.load(), storedBrightness = observedBrightness;
    uint32_t brightnessChanged = 0;
    View view = View::Clock;
    uint32_t solidColor = 0, scores = 0;
    TimerState countdown, stopwatch;
    uint32_t lastValue = UINT32_MAX;
    uint32_t clockSettings = 0x406118d6;
    uint32_t nextClockFrame = 0, clockStarted = 0;
    uint32_t savedClockSettings = clockSettings, clockSettingsChanged = 0;
    bool drawNativeClock = false;
    if (nvs_open("idotmatrix", NVS_READONLY, &handle) == ESP_OK) {
        uint8_t valid = 0, flags = 0, style = 0, red = 97, green = 24, blue = 214;
        if (nvs_get_u8(handle, "cvalid", &valid) == ESP_OK && valid) {
            nvs_get_u8(handle, "cstyle", &style);
            nvs_get_u8(handle, "cflags", &flags);
            nvs_get_u8(handle, "cr", &red);
            nvs_get_u8(handle, "cg", &green);
            nvs_get_u8(handle, "cb", &blue);
            clockSettings = savedClockSettings = (uint32_t(((flags & 3) << 6) | (style & 7)) << 24) |
                            uint32_t(red) << 16 | uint32_t(green) << 8 | blue;
        }
        nvs_close(handle);
    }

    bool bootPressed = false;
    bool bootReleased = false;
    uint32_t bootAt = 0;
    const uint8_t *currentMedia = nullptr;
    bool showingContent = false;
    lv_obj_t *mediaWidget = nullptr;
    static CanvasGif gif;
    char source[42]{};
    int mediaSlot = -1;
    View eventPreviousView = View::Clock;
    char normalMedia[40]{};
    bool normalGraffitiPng = false;
    bool eventActive = false;
    volatile uint32_t *savedCanvas = nullptr;
    unsigned rawSide = side;
    TextMedia textMedia;
    uint32_t textStarted = 0, nextTextFrame = 0;
    auto stopMedia = [&]() {
        textMedia.clear();
        gif.close();
        if (mediaWidget) lv_obj_delete(mediaWidget);
        mediaWidget = nullptr;
        currentMedia = nullptr;
        lv_obj_set_hidden(clock, false);
    };
    for (;;) {
        const uint32_t now = lvTicks();
        bool cycle = false;
        if (gpio_get_level(boot) == 0) {
            if (!bootPressed) bootAt = now;
            bootPressed = true;
        } else {
            if (bootPressed && bootReleased && now - bootAt >= 10000) reset(true);
            cycle = bootPressed && bootReleased && now - bootAt >= 40;
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
        const unsigned percent = brightness.load();
        if (percent != observedBrightness) {
            brightnessChanged = now;
            observedBrightness = percent;
        }
        if (observedBrightness != storedBrightness && now - brightnessChanged >= 1000) {
            nvs_handle_t settings;
            if (nvs_open("idotmatrix", NVS_READWRITE, &settings) == ESP_OK) {
                if (nvs_set_u8(settings, "brightness", observedBrightness) == ESP_OK &&
                    nvs_commit(settings) == ESP_OK) storedBrightness = observedBrightness;
                nvs_close(settings);
            }
            brightnessChanged = now;
        }
        const bool flip = requestedFlip.load();
        if (flip != flipped) {
            flipped = flip;
            lv_obj_invalidate(screen);
            showingContent = false;
        }
        const auto color = requestedClockColor.exchange(UINT32_MAX);
        if (color != UINT32_MAX) {
            if (!eventActive) normalMedia[0] = 0;
            clockwise_media_stop_carousel();
            stopMedia();
            clockwise_effect_stop();
            view = View::Clock;
            nextClockFrame = 0;
            clockSettings = color;
            clockStarted = clockSettingsChanged = now;
            ESP_LOGI("clockwise", "Clock style=%u flags=0x%02x", unsigned(color >> 24 & 7), unsigned(color >> 24));
            lv_obj_invalidate(screen);
        }
        uint8_t control;
        uint32_t value;
        portENTER_CRITICAL(&controlLock);
        control = requestedView;
        value = requestedValue;
        requestedView = 0;
        portEXIT_CRITICAL(&controlLock);
        if (control) {
            if (!eventActive) normalMedia[0] = 0;
            clockwise_media_stop_carousel();
            clockwise_effect_stop();
            stopMedia();
            showingContent = false;
            lastValue = UINT32_MAX;
            view = static_cast<View>(control);
            switch (view) {
            case View::Solid: solidColor = value; break;
            case View::Countdown: countdown.update(value >> 24, value & 0xffffff, now, true); break;
            case View::Stopwatch: stopwatch.update(value, 0, now, false); break;
            case View::Score: scores = value; break;
            default: break;
            }
            ESP_LOGI("clockwise", "View %u", unsigned(control));
        }
        if (countdown.running && !countdown.remaining(now)) {
            countdown = {};
            clockwise_countdown_finished();
        }
        if (clockSettings != savedClockSettings && now - clockSettingsChanged >= 1000) {
            nvs_handle_t preferences;
            if (nvs_open("idotmatrix", NVS_READWRITE, &preferences) == ESP_OK) {
                const bool ok = nvs_set_u8(preferences, "cstyle", clockSettings >> 24 & 7) == ESP_OK &&
                    nvs_set_u8(preferences, "cflags", clockSettings >> 30) == ESP_OK &&
                    nvs_set_u8(preferences, "cr", clockSettings >> 16) == ESP_OK &&
                    nvs_set_u8(preferences, "cg", clockSettings >> 8) == ESP_OK &&
                    nvs_set_u8(preferences, "cb", clockSettings) == ESP_OK &&
                    nvs_set_u8(preferences, "cvalid", 1) == ESP_OK && nvs_commit(preferences) == ESP_OK;
                if (ok) savedClockSettings = clockSettings;
                nvs_close(preferences);
            }
            clockSettingsChanged = now;
        }
        bool redrawEffect;
        const bool effect = clockwise_effect_active(now, redrawEffect);
        if (effect && redrawEffect) {
            if (!eventActive) normalMedia[0] = 0;
            clockwise_media_stop_carousel();
            view = View::Clock;
            nextClockFrame = 0;
        }
        char path[40];
        const auto event = pollEvents(calendarKnown ? clockSeconds() + clockOffset.load() : 0, path);
        bool nextMedia = false;
        bool graffitiPng = false;
        const bool resuming = event == EventChange::End;
        if (event == EventChange::Start) {
            if (!eventActive) {
                eventPreviousView = view;
                if (view == View::Graffiti) {
                    savedCanvas = static_cast<uint32_t *>(heap_caps_malloc_prefer(sizeof(frame), 2, MALLOC_CAP_EXEC | MALLOC_CAP_32BIT, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
                    if (savedCanvas) for (size_t i = 0; i < sizeof(frame) / 4; ++i) {
                        uint32_t word;
                        memcpy(&word, reinterpret_cast<const uint8_t *>(frame) + 4 * i, 4);
                        savedCanvas[i] = word;
                    }
                }
            }
            eventActive = true;
            nextMedia = true;
        } else if (resuming) {
            eventActive = false;
            if (savedCanvas) {
                for (size_t i = 0; i < sizeof(frame) / 4; ++i) {
                    const uint32_t word = savedCanvas[i];
                    memcpy(reinterpret_cast<uint8_t *>(frame) + 4 * i, &word, 4);
                }
                heap_caps_free(const_cast<uint32_t *>(savedCanvas));
                savedCanvas = nullptr;
            }
            if (normalMedia[0]) {
                memcpy(path, normalMedia, sizeof(path));
                graffitiPng = normalGraffitiPng;
                nextMedia = true;
            } else {
                stopMedia();
                view = eventPreviousView;
                showingContent = false;
                lastValue = UINT32_MAX;
                nextClockFrame = 0;
            }
        } else if (!eventActive) {
            nextMedia = clockwise_media_next(cycle, mediaSlot, path, graffitiPng);
            if (nextMedia) {
                snprintf(normalMedia, sizeof(normalMedia), "%s", path);
                normalGraffitiPng = graffitiPng;
            }
        }
        if (nextMedia) {
            showingContent = false;
            view = View::Clock;
            nextClockFrame = 0;
            stopMedia();
            clockwise_effect_stop();
            const int format = clockwise_media_format(path);
            switch (format) {
            case 1:
                if (gif.open(path, reinterpret_cast<uint8_t *>(frame)) && gif.advance(now)) {
                    presentFrame();
                    ESP_LOGI("media", "GIF decoder loaded %s", path);
                } else ESP_LOGW("media", "GIF could not open %s", path);
                break;
            case 2:
                snprintf(source, sizeof(source), "M:%s", path);
                if (eventActive && restoreGraffitiPng(path, true)) {
                    rawSide = side;
                    currentMedia = reinterpret_cast<const uint8_t *>(frame);
                    break;
                }
                if (graffitiPng) {
                    if (restoreGraffitiPng(path)) view = View::Graffiti;
                    break;
                }
                mediaWidget = lv_image_create(screen);
                if (mediaWidget) lv_image_set_src(mediaWidget, source);
                break;
            case 5:
                if (textMedia.load(path)) textStarted = nextTextFrame = now;
                break;
            case 3:
            case 4:
                if (FILE *f = fopen(path, "rb")) {
                    rawSide = format == 3 ? side : side / 2;
                    const auto bytes = rawSide * rawSide * 3;
                    if (fread(frame, 1, bytes, f) == bytes)
                        currentMedia = reinterpret_cast<const uint8_t *>(frame);
                    fclose(f);
                }
                break;
            }
            if (mediaWidget) {
                lv_obj_set_hidden(clock, true);
                lv_obj_center(mediaWidget);
            }
            lv_obj_invalidate(screen);
        } else if (effect && (gif.active() || currentMedia || mediaWidget || textMedia.bytes)) {
            stopMedia();
        }
        if (!eventActive && renderRhythm(writeFramePixel, now)) {
            normalMedia[0] = 0;
            clockwise_media_stop_carousel();
            clockwise_effect_stop();
            stopMedia();
            view = View::Rhythm;
            showingContent = false;
        }
        portENTER_CRITICAL(&graffitiLock);
        const bool graffiti = selectGraffiti && !eventActive;
        if (graffiti) {
            if (clearGraffiti || view != View::Graffiti)
                std::fill_n(frame, side * side, Pixel{});
            for (unsigned i = 0; i < strokeCount; ++i) {
                const auto &point = stroke[i];
                frame[point.y * side + point.x] = point.color;
            }
            strokeCount = 0;
            clearGraffiti = selectGraffiti = false;
        }
        portEXIT_CRITICAL(&graffitiLock);
        if (graffiti) {
            normalMedia[0] = 0;
            clockwise_media_stop_carousel();
            clockwise_effect_stop();
            stopMedia();
            view = View::Graffiti;
            showingContent = false;
        }
        const bool solid = view == View::Solid;
        unsigned dutyLimit = maxDuty;
        if (solid) {
            const unsigned channels = (solidColor >> 16 & 255) + (solidColor >> 8 & 255) + (solidColor & 255);
            dutyLimit = std::min(maxDuty, solidChannelBudget / std::max(1u, channels));
        }
        const unsigned duty = powered ? std::min(displayDuty(percent), dutyLimit) : 0;
        if (lastDuty != duty) {
            // Discard the old full-screen fill before restoring higher duty.
            if (lastSolid && duty > lastDuty) panel.clearScreen();
            panel.setBrightness8(duty);
            ESP_LOGI("clockwise", "Brightness duty=%u/255 (limit=%u, App=%u%%)",
                     duty, dutyLimit, percent);
            lv_obj_invalidate(screen);
            lastDuty = duty;
            showingContent = false;
        }
        lastSolid = solid;
        if (view == View::Graffiti || view == View::Rhythm) {
            if (!showingContent) presentFrame();
            showingContent = true;
            vTaskDelay(pdMS_TO_TICKS(15));
            continue;
        }
        if ((effect && !nextMedia) || currentMedia || textMedia.bytes) {
            const bool redrawText = textMedia.bytes && int32_t(now - nextTextFrame) >= 0;
            if (redrawText) nextTextFrame = now + 35;
            if (redrawEffect || redrawText || nextMedia || !showingContent) {
                for (unsigned y = 0; y < side; ++y) {
                    for (unsigned x = 0; x < side; ++x) {
                        if (currentMedia) {
                            const auto scale = side / rawSide;
                            const uint8_t *p = currentMedia + 3 * ((y / scale) * rawSide + x / scale);
                            drawPixel(x, y, p[0], p[1], p[2]);
                        } else if (textMedia.bytes) {
                            const auto pixel = textMedia.pixel(x, y, now - textStarted);
                            drawPixel(x, y, pixel.red, pixel.green, pixel.blue);
                        } else {
                            const auto color = clockwise_effect_pixel(x, y, now);
                            drawPixel(x, y, color.red, color.green, color.blue);
                        }
                    }
                }
            }
            showingContent = true;
            vTaskDelay(pdMS_TO_TICKS(15));
            continue;
        }
        if (gif.active()) {
            if (gif.advance(now)) presentFrame();
            showingContent = true;
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (showingContent && (mediaWidget || (view == View::Clock && !clockSet))) {
            lv_obj_invalidate(screen);
            showingContent = false;
        }
        if (!mediaWidget && (view != View::Clock || clockSet)) {
            const bool refresh = !drawNativeClock || !showingContent || int32_t(now - nextClockFrame) >= 0;
            if (refresh) {
                nextClockFrame = now + (view == View::Clock && (clockSettings >> 24 & 7) == 0 ? 24 : 60);
                if (view == View::Solid) {
                    if (lastValue != solidColor || !showingContent)
                        for (unsigned y = 0; y < side; ++y)
                            for (unsigned x = 0; x < side; ++x)
                                drawPixel(x, y, solidColor >> 16, solidColor >> 8, solidColor);
                    lastValue = solidColor;
                } else if (view == View::Clock) {
                    const auto seconds = clockSeconds() + clockOffset.load();
                    const unsigned minute = seconds % 86400 / 60;
                    time_t epoch = seconds;
                    tm date{};
                    if (calendarKnown) gmtime_r(&epoch, &date);
                    renderClockView(writeFramePixel, now, minute / 60, minute % 60, date.tm_mday,
                                    date.tm_mon + 1, clockSettings,
                                    (clockSettings >> 31) && (now - clockStarted) % 35000 >= 30000);
                } else {
                    uint32_t reading = scores;
                    if (view == View::Countdown) reading = countdown.remaining(now);
                    if (view == View::Stopwatch) reading = stopwatch.value + (stopwatch.running ? now - stopwatch.started : 0);
                    renderUtilityView(writeFramePixel, now, unsigned(view), reading);
                }
                if (view != View::Solid) presentFrame();
            }
            showingContent = drawNativeClock = true;
            vTaskDelay(pdMS_TO_TICKS(15));
            continue;
        }
        drawNativeClock = false;
        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(15));
    }
}
