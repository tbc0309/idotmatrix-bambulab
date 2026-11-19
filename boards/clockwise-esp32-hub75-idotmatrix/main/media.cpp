#include "app_state.h"
#include "text_media.h"
#include <atomic>
#include <initializer_list>
#include <nvs.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <esp_littlefs.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_partition.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

int clockwise_media_format(const char *path);

namespace {
constexpr size_t frameSize = 64 * 64 * 3;
constexpr size_t maxMedia = 1024 * 1024;
constexpr const char *staging = "/media/.receive";
constexpr const char *directPaths[] = {"/media/direct", "/media/direct-a", "/media/direct-b"};
constexpr unsigned directCount = sizeof(directPaths) / sizeof(directPaths[0]);
constexpr const char *directIndex = "/media/.direct-index";
constexpr const char *directIndexTemp = "/media/.direct-next";
unsigned latestDirect = 0, playingDirect = directCount;
bool latestGraffitiPng;
constexpr unsigned slots = 12;
bool mounted;
StaticSemaphore_t mutexStorage;
SemaphoreHandle_t mutex;
struct MediaLock {
    MediaLock() { if (mutex) xSemaphoreTake(mutex, portMAX_DELAY); }
    ~MediaLock() { if (mutex) xSemaphoreGive(mutex); }
};
int receiving = -1;
size_t written, total;
uint32_t expectedCrc, crc;
uint64_t lastPacket;
uint8_t type, slot;
constexpr char manifest[] = "/media/bank";
constexpr char manifestTemp[] = "/media/.bank";
constexpr int64_t settleUs = 8000000;
enum Source : uint8_t { Empty, GenerationA, GenerationB, Previous, Legacy };
struct Slot { uint8_t source; uint8_t reserved; uint16_t dwell; };
struct Bank {
    uint32_t version;
    uint8_t order[slots];
    Slot entry[slots];
};
constexpr uint32_t bankVersion = 0x314b4e42;
Bank activeBank{};
Bank upload{};
uint16_t bankMask, completedMask;
bool bankOpen, bankFailed, autoplay;
int64_t lastComplete;
int64_t nextSlide;

void slotPath(char (&path)[40], unsigned slot, unsigned source) {
    const char suffix = source == GenerationA ? 'a' : 'b';
    snprintf(path, sizeof(path), "/media/bank%c%u", suffix, slot);
}

void initialBank() {
    activeBank.version = bankVersion;
    for (unsigned i = 0; i < slots; ++i) {
        activeBank.order[i] = i;
        char path[40];
        snprintf(path, sizeof(path), "/media/native%u", i);
        activeBank.entry[i] = {uint8_t(clockwise_media_format(path) ? Previous : Legacy), 0, 5};
    }
    nvs_handle_t handle;
    if (nvs_open("idot-car", NVS_READONLY, &handle) == ESP_OK) {
        uint8_t order[slots], count = 0;
        size_t bytes = sizeof(order);
        if (nvs_get_u8(handle, "count", &count) == ESP_OK && count <= slots &&
            nvs_get_blob(handle, "order", order, &bytes) == ESP_OK && bytes == slots) {
            uint16_t used = 0;
            unsigned at = 0;
            for (unsigned i = 0; i < count; ++i)
                if (order[i] < slots && !(used & (1u << order[i]))) {
                    used |= 1u << order[i];
                    activeBank.order[at++] = order[i];
                }
            for (unsigned i = 0; i < slots; ++i)
                if (!(used & (1u << i))) activeBank.order[at++] = i;
        }
        for (unsigned i = 0; i < slots; ++i) {
            if (activeBank.entry[i].source != Legacy) continue;
            char key[12];
            snprintf(key, sizeof(key), "c%u", i);
            uint8_t meta[12];
            size_t length = sizeof(meta);
            if (nvs_get_blob(handle, key, meta, &length) != ESP_OK || length != sizeof(meta)) continue;
            if (!meta[0]) activeBank.entry[i] = {};
            else activeBank.entry[i].dwell = uint16_t(meta[2]) | uint16_t(meta[3]) << 8;
        }
        nvs_close(handle);
    }
    FILE *f = fopen(manifest, "rb");
    if (!f) return;
    Bank candidate{};
    const bool complete = fread(&candidate, 1, sizeof(candidate), f) == sizeof(candidate) &&
                          fgetc(f) == EOF;
    fclose(f);
    if (!complete || candidate.version != bankVersion) return;
    uint16_t seen = 0;
    for (unsigned i = 0; i < slots; ++i) {
        const unsigned slot = candidate.order[i];
        if (slot >= slots || (seen & (1u << slot)) || candidate.entry[i].source > Legacy) return;
        seen |= 1u << slot;
    }
    activeBank = candidate;
}

void discardBank() {
    bankOpen = false;
    lastComplete = 0;
    bankMask = completedMask = 0;
}

bool publishBank() {
    FILE *f = fopen(manifestTemp, "wb");
    if (!f) return false;
    const bool wrote = fwrite(&upload, 1, sizeof(upload), f) == sizeof(upload);
    const bool closed = fclose(f) == 0;
    if (!wrote || !closed || rename(manifestTemp, manifest) != 0) {
        unlink(manifestTemp);
        return false;
    }
    activeBank = upload;
    ESP_LOGI("media", "Published bank, received mask=0x%03x", unsigned(completedMask));
    discardBank();
    return true;
}
uint16_t dwell;
constexpr uint8_t rasterType = 0xff;
constexpr unsigned presetBase = 14, presetCount = 6;
uint8_t presetGeneration[presetCount]{}, presetReady[presetCount]{};
uint16_t presetDelay[presetCount]{}, candidateDelay[presetCount]{};
uint8_t presetOrder[presetCount]{};
unsigned presetSize, presetAt;
bool presetOn, presetChanged;
int64_t presetNext;

void presetPath(char (&path)[40], unsigned slot, unsigned generation) {
    snprintf(path, sizeof(path), "/media/native-preset%u-%u", slot, generation);
}
std::atomic<int> pending{-2}; // -2: none, -1: direct, 0..11: saved, 12: select saved
void pruneDirect() {
    for (unsigned i = 0; i < directCount; ++i)
        if (i != latestDirect && i != playingDirect) unlink(directPaths[i]);
}

bool publishDirect(bool graffitiPng = false) {
    // The last handed-off file may still be open in LVGL. Keep it and the
    // latest committed file while the staged upload is promoted.
    for (unsigned i = 0; i < directCount; ++i) {
        if (i == latestDirect || i == playingDirect) continue;
        if (rename(staging, directPaths[i]) != 0) continue;
        const int index = open(directIndexTemp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        bool indexed = false;
        if (index >= 0) {
            const uint8_t generation = i;
            const bool wrote = write(index, &generation, sizeof(generation)) == sizeof(generation);
            indexed = close(index) == 0 && wrote && rename(directIndexTemp, directIndex) == 0;
        }
        if (!indexed) {
            unlink(directIndexTemp);
            unlink(directPaths[i]);
            return false;
        }
        latestDirect = i;
        latestGraffitiPng = graffitiPng;
        pending.store(-1);
        return true;
    }
    return false;
}

void selectDirect(char (&path)[40]) {
    playingDirect = latestDirect;
    snprintf(path, sizeof(path), "%s", directPaths[playingDirect]);
}

uint32_t read32(const uint8_t *d) {
    return uint32_t(d[0]) | uint32_t(d[1]) << 8 | uint32_t(d[2]) << 16 | uint32_t(d[3]) << 24;
}
bool blank(const esp_partition_t *p) {
    uint8_t buf[256];
    for (size_t offset = 0; offset < p->size; offset += sizeof(buf)) {
        if (esp_partition_read(p, offset, buf, sizeof(buf)) != ESP_OK) return false;
        for (uint8_t b : buf) if (b != 0xff) return false;
    }
    return true;
}
}
void clockwise_media_mount() {
    mutex = xSemaphoreCreateMutexStatic(&mutexStorage);
    const auto *p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "spiffs");
    if (!p) return;
    esp_vfs_littlefs_conf_t options{};
    options.base_path = "/media";
    options.partition_label = "spiffs";
    auto result = esp_vfs_littlefs_register(&options);
    if (result != ESP_OK && blank(p)) {
        options.format_if_mount_failed = true;
        result = esp_vfs_littlefs_register(&options);
    }
    mounted = result == ESP_OK;
    if (mounted) {
        unlink(staging);
        unlink(manifestTemp);
        unlink(directIndexTemp);
        if (FILE *index = fopen(directIndex, "rb")) {
            const int generation = fgetc(index);
            fclose(index);
            if (generation >= 0 && unsigned(generation) < directCount &&
                clockwise_media_format(directPaths[generation])) latestDirect = generation;
        }
        pruneDirect();
        initialBank();
        for (unsigned slot = 0; slot < presetCount; ++slot)
            for (unsigned generation = 1; generation <= 2; ++generation) {
                char path[40];
                presetPath(path, slot, generation);
                unlink(path);
            }
        ESP_LOGI("media", "LittleFS mounted");
    }
    else ESP_LOGW("media", "LittleFS unavailable (%s); existing partition left intact", esp_err_to_name(result));
}
static void abortTransfer() {
    if (receiving >= 0) {
        close(receiving);
        receiving = -1;
        unlink(staging);
    }
    written = 0;
}
static void failTransfer() {
    abortTransfer();
    if (bankOpen) {
        ESP_LOGW("media", "Bank discarded, previous bank retained");
        bankFailed = true;
        discardBank();
    }
}

void clockwise_media_shutdown() {
    MediaLock lock;
    mounted = false;
    abortTransfer();
}

extern "C" void clockwise_media_disconnect() {
    MediaLock lock;
    abortTransfer();
    discardBank();
    bankFailed = false;
    memset(presetReady, 0, sizeof(presetReady));
}
extern "C" bool clockwise_media_bank(const uint8_t *data, size_t size) {
    MediaLock lock;
    if (!mounted || size < 5 || data[4] > slots || size != 5u + data[4]) return false;
    uint16_t mask = 0;
    for (unsigned i = 5; i < size; ++i) {
        if (data[i] >= slots || (mask & (1u << data[i]))) return false;
        mask |= 1u << data[i];
    }
    abortTransfer();
    discardBank();
    upload = activeBank;
    bankMask = mask;
    bankFailed = false;
    bankOpen = true;
    unsigned at = 0;
    for (unsigned i = 5; i < size; ++i) {
        const unsigned slot = data[i];
        upload.order[at++] = slot;
        upload.entry[slot] = {};
        for (unsigned source : {unsigned(GenerationA), unsigned(GenerationB)}) {
            if (activeBank.entry[slot].source == source) continue;
            char path[40];
            slotPath(path, slot, source);
            unlink(path);
        }
    }
    for (unsigned slot : activeBank.order)
        if (!(mask & (1u << slot))) upload.order[at++] = slot;
    if (!mask) {
        for (auto &entry : upload.entry) entry = {};
        const bool committed = publishBank();
        discardBank();
        if (committed) pending.store(slots);
        return committed;
    }
    return true;
}
extern "C" bool clockwise_preset_activate(const uint8_t *data, size_t size) {
    MediaLock lock;
    if (!mounted || size < 5 || data[4] > presetCount || size != 5u + data[4]) return false;
    uint8_t seen = 0;
    for (unsigned i = 5; i < size; ++i) {
        if (data[i] < presetBase || data[i] >= presetBase + presetCount) return false;
        const unsigned slot = data[i] - presetBase;
        if ((seen & (1u << slot)) || (!presetReady[slot] && !presetGeneration[slot])) return false;
        seen |= 1u << slot;
    }
    for (unsigned i = 0; i < data[4]; ++i) {
        const unsigned slot = data[5 + i] - presetBase;
        presetOrder[i] = slot;
        if (presetReady[slot]) {
            presetGeneration[slot] = presetReady[slot];
            presetDelay[slot] = candidateDelay[slot];
        }
    }
    memset(presetReady, 0, sizeof(presetReady));
    presetSize = data[4];
    presetAt = 0;
    presetOn = presetSize != 0;
    presetChanged = true;
    ESP_LOGI("media", "Preset activated, entries=%u", presetSize);
    return true;
}

extern "C" void clockwise_media_replay() {
    pending.store(slots);
}
extern "C" uint8_t clockwise_accept_raster(const uint8_t *data, size_t size) {
    MediaLock lock;
    if (!mounted || size < 9 || (data[4] != 0 && data[4] != 2)) return 3;
    const auto now = uint64_t(esp_timer_get_time());
    if (receiving >= 0 && (now - lastPacket > 5000000 || data[4] == 0)) abortTransfer();
    const auto requested = read32(data + 5);
    if (data[4] == 0) {
        const bool png = size >= 17 && !memcmp(data + 9, "\x89PNG\r\n\x1a\n", 8);
        if (png ? requested > 48 * 1024 : requested != frameSize && requested != frameSize / 4) return 3;
        if (!requested) return 3;
        pruneDirect();
        receiving = open(staging, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (receiving < 0) return 3;
        total = requested;
        type = rasterType;
    }
    if (receiving < 0 || type != rasterType || total != requested) return 3;
    const auto n = size - 9;
    if (n > total - written || write(receiving, data + 9, n) != ssize_t(n)) {
        abortTransfer();
        return 3;
    }
    written += n;
    lastPacket = now;
    if (written != total) return 2;
    const bool closed = close(receiving) == 0;
    receiving = -1;
    written = 0;
    const auto format = closed ? clockwise_media_format(staging) : 0;
    if (format < 2 || format > 4 || !publishDirect(format == 2)) {
        unlink(staging);
        return 3;
    }
    return 1;
}
extern "C" uint8_t clockwise_accept_media(const uint8_t *data, size_t size) {
    MediaLock lock;
    if (bankFailed) {
        ESP_LOGW("media", "Upload discarded after bank failure");
        return 3;
    }
    const auto now = uint64_t(esp_timer_get_time());
    if (receiving >= 0 && now - lastPacket > 5000000) {
        failTransfer();
        if (bankFailed) {
        ESP_LOGW("media", "Upload discarded after bank failure");
        return 3;
    }
    }
    if (!mounted || size <= 16 || !read32(data + 5) || read32(data + 5) > maxMedia) {
        failTransfer();
        return 3;
    }
    // A fresh upload can follow a canceled one on the same BLE connection.
    if (receiving >= 0 && data[4] == 0) abortTransfer();
    if (receiving < 0) {
        if (data[4] != 0) return 3;
        pruneDirect();
        receiving = open(staging, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (receiving < 0) {
            ESP_LOGW("media", "Cannot open upload staging file (errno=%d)", errno);
            failTransfer();
            return 3;
        }
        total = read32(data + 5);
        expectedCrc = read32(data + 9);
        crc = UINT32_MAX;
        type = data[2];
        slot = data[15];
        dwell = uint16_t(data[13]) | uint16_t(data[14]) << 8;
        if (bankOpen && !(slot >= presetBase && slot < presetBase + presetCount) && (slot >= slots || !(bankMask & (1u << slot)))) {
            failTransfer();
            return 3;
        }
    }
    const size_t len = size - 16;
    if (read32(data + 5) != total || read32(data + 9) != expectedCrc ||
        data[2] != type || data[15] != slot || len > total - written) {
        ESP_LOGW("media", "Upload chunk does not match active asset (offset=%zu, chunk=%zu, total=%zu)",
                 written, len, total);
        failTransfer();
        return 3;
    }
    if (write(receiving, data + 16, len) != ssize_t(len)) {
        ESP_LOGW("media", "Upload write failed (offset=%zu, errno=%d)", written, errno);
        failTransfer();
        return 3;
    }
    for (size_t i = 16; i < size; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0u);
    }
    written += len;
    lastPacket = now;
    if (written < total) return 1;
    const bool closed = close(receiving) == 0;
    receiving = -1;
    written = 0;
    // Validate the container before replacing an existing asset.
    const auto format = closed ? clockwise_media_format(staging) : 0;
    const bool validType = type == 1 ? format == 1 : type == 3 ? format == 5 : format >= 1 && format <= 4;
    if ((crc ^ UINT32_MAX) != expectedCrc || !validType) {
        ESP_LOGW("media", "Upload validation failed: crc_ok=%d format=%d type=%u total=%zu", (crc ^ UINT32_MAX) == expectedCrc, format, unsigned(type), total);
        unlink(staging);
        failTransfer();
        return 3;
    }
    char name[40];
    if (slot >= presetBase && slot < presetBase + presetCount && (type == 1 || type == 3)) {
        const unsigned index = slot - presetBase;
        const unsigned generation = presetGeneration[index] == 1 ? 2 : 1;
        presetPath(name, index, generation);
        if (rename(staging, name) != 0) {
            unlink(staging);
            memset(presetReady, 0, sizeof(presetReady));
            return 3;
        }
        presetReady[index] = generation;
        candidateDelay[index] = dwell;
        return 3;
    }
    const unsigned generation = activeBank.entry[slot < slots ? slot : 0].source == GenerationA
        ? GenerationB : GenerationA;
    if (bankOpen) slotPath(name, slot, generation);
    if (bankOpen ? rename(staging, name) != 0 : !publishDirect()) {
        ESP_LOGW("media", "Upload publish failed: bank=%d slot=%u errno=%d", bankOpen, unsigned(slot), errno);
        unlink(staging);
        failTransfer();
        return 3;
    }
    if (bankOpen) {
        upload.entry[slot] = {uint8_t(generation), 0, dwell};
        completedMask |= 1u << slot;
        lastComplete = now;
        ESP_LOGI("media", "Staged bank slot %u", unsigned(slot));
        if (completedMask == bankMask) {
            if (publishBank()) pending.store(slots);
            else failTransfer();
        }
    }
    return 3;
}
int clockwise_media_format(const char *path) {
    const int fd = open(path, O_RDONLY);
    if (fd < 0) {
        ESP_LOGW("media", "Cannot open media header (errno=%d)", errno);
        return 0;
    }
    uint8_t h[24]{};
    const auto len = read(fd, h, sizeof(h));
    if (len < 0) {
        ESP_LOGW("media", "Cannot read media header (errno=%d)", errno);
        close(fd);
        return 0;
    }
    const auto size = lseek(fd, 0, SEEK_END);
    close(fd);
    if (len >= 10 && (!memcmp(h, "GIF87a", 6) || !memcmp(h, "GIF89a", 6))) {
        const auto w = h[6] | h[7] << 8, hh = h[8] | h[9] << 8;
        return w > 0 && w <= 64 && hh > 0 && hh <= 64 ? 1 : 0;
    }
    if (len == sizeof(h) && !memcmp(h, "\x89PNG\r\n\x1a\n", 8)) {
        if (size > 48 * 1024 || memcmp(h + 12, "IHDR", 4)) return 0;
        const auto dim = [](const uint8_t *d) { return (d[0] | d[1] | d[2]) == 0 && d[3] > 0 && d[3] <= 64; };
        return dim(h + 16) && dim(h + 20) ? 2 : 0;
    }
    if (size == frameSize) return 3;
    if (size == frameSize / 4) return 4;
    return size > 14 && size <= 16654 && size % 4 == 2 && clockwise_text_valid(path) ? 5 : 0;
}
void clockwise_media_stop_carousel() {
    MediaLock lock;
    autoplay = presetOn = presetChanged = false;
}

bool clockwise_media_next(bool cycle, int &current, char (&path)[40], bool &graffitiPng) {
    graffitiPng = false;
    MediaLock lock;
    const auto now = esp_timer_get_time();
    if (bankOpen && receiving >= 0 && now - lastPacket > 5000000) {
        failTransfer();
    }
    if (bankOpen && receiving < 0 && lastComplete && now - lastComplete >= settleUs) {
        if (publishBank()) pending.store(slots);
        else failTransfer();
    }
    if (bankOpen) return false;
    const int candidate = pending.exchange(-2);
    if (candidate != -2 || cycle) presetOn = presetChanged = false;
    if (presetChanged && !presetOn) {
        presetChanged = false;
        path[0] = 0;
        return true;
    }
    if (presetOn && (presetChanged || now >= presetNext)) {
        if (presetSize == 1 && !presetChanged) return false;
        const unsigned slot = presetOrder[presetAt++ % presetSize];
        presetPath(path, slot, presetGeneration[slot]);
        presetChanged = false;
        presetNext = now + int64_t(presetDelay[slot] ? presetDelay[slot] : 5) * 1000000;
        return true;
    }
    if (presetOn) return false;
    if (candidate == -1) {
        autoplay = false;
        graffitiPng = latestGraffitiPng;
        selectDirect(path);
        return true;
    }
    if (candidate == int(slots)) {
        current = -1;
        autoplay = true;
    }
    if (!cycle && candidate != int(slots) && (!autoplay || now < nextSlide)) return false;
    int start = -1;
    for (unsigned i = 0; i < slots; ++i)
        if (activeBank.order[i] == current) start = i;
    for (unsigned i = 1; i <= slots; ++i) {
        const unsigned slot = activeBank.order[(start + int(i)) % slots];
        const Slot entry = activeBank.entry[slot];
        switch (entry.source) {
        case GenerationA:
        case GenerationB: slotPath(path, slot, entry.source); break;
        case Previous: snprintf(path, sizeof(path), "/media/native%u", slot); break;
        case Legacy:
            snprintf(path, sizeof(path), "/media/car%u.gif", slot);
            if (!clockwise_media_format(path)) snprintf(path, sizeof(path), "/media/car%u.raw", slot);
            if (!clockwise_media_format(path)) snprintf(path, sizeof(path), "/media/car%u.txt", slot);
            break;
        default: continue;
        }
        if (clockwise_media_format(path)) {
            nextSlide = now + int64_t(entry.dwell ? entry.dwell : 5) * 1000000;
            if (current == int(slot) && !cycle) return false;
            current = slot;
            return true;
        }
    }
    autoplay = false;
    if (candidate == int(slots)) { path[0] = '\0'; return true; }
    if (clockwise_media_format(directPaths[latestDirect])) {
        selectDirect(path);
        return true;
    }
    return false;
}
