
#include "app_state.h"
#include "events.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <esp_log.h>
#include <esp_timer.h>
#include <nvs.h>
#include <unistd.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

int clockwise_media_format(const char *path);

namespace {
constexpr unsigned alarms = 10, schedules = 32;
constexpr uint32_t maxPayload = 1024 * 1024;
constexpr int64_t uploadTimeout = 10000000, scheduleSettle = 900000;
constexpr char temporary[] = "/media/.event";
struct Item {
    uint32_t size{}, crc{};
    uint16_t start{}, end{};
    uint8_t flags{}, duration{}, generation{}, type{};
    uint8_t buzzer{}, reserved[3]{};
};
struct Schedule {
    uint8_t flags{}, reserved[3]{};
    Item items[schedules];
};
static_assert(sizeof(Item) == 20);
Item alarmItems[alarms];
Schedule active, staging;
FILE *upload;
Item candidate;
int receiving = -1;
size_t received;
uint32_t crc;
int64_t lastPacket, lastSchedule;
uint32_t stagedMask, previousMinute[alarms];
int alarmActive = -1, displayed = -1;
int64_t alarmUntil;
bool addingSchedule, scheduleFailed;
StaticSemaphore_t mutexStorage;
SemaphoreHandle_t mutex;
struct Lock {
    Lock() { xSemaphoreTake(mutex, portMAX_DELAY); }
    ~Lock() { xSemaphoreGive(mutex); }
};

uint32_t read32(const uint8_t *data) {
    return uint32_t(data[0]) | uint32_t(data[1]) << 8 | uint32_t(data[2]) << 16 | uint32_t(data[3]) << 24;
}
void pathFor(char (&path)[40], bool alarm, unsigned slot, unsigned generation) {
    if (!generation) snprintf(path, sizeof(path), "/media/%s%u.bin", alarm ? "alarm" : "sch", slot);
    else snprintf(path, sizeof(path), "/media/event-%c%u-%u", alarm ? 'a' : 's', slot, generation);
}
int64_t ticks() { return esp_timer_get_time(); }
bool persist(const char *space, const char *key, const void *value, size_t len) {
    nvs_handle_t handle;
    if (nvs_open(space, NVS_READWRITE, &handle) != ESP_OK) return false;
    const bool ok = nvs_set_blob(handle, key, value, len) == ESP_OK && nvs_commit(handle) == ESP_OK;
    nvs_close(handle);
    return ok;
}
bool saveAlarm(unsigned slot, const Item &value) {
    char key[8];
    snprintf(key, sizeof(key), "n%u", slot);
    return persist("idot-alarm", key, &value, sizeof(value));
}
bool saveSchedule(const Schedule &value) {
    return persist("idot-sched", "native1", &value, sizeof(value));
}
void stopUpload() {
    if (upload) fclose(upload);
    upload = nullptr;
    receiving = -1;
    received = 0;
    unlink(temporary);
}
void abortSchedule() {
    if (receiving >= int(alarms)) stopUpload();
    addingSchedule = false;
    scheduleFailed = true;
    stagedMask = 0;
}
bool startUpload(int index, const Item &item, const Item &old) {
    stopUpload();
    candidate = item;
    candidate.generation = old.generation == 1 ? 2 : 1;
    char path[40];
    const bool alarm = index < int(alarms);
    pathFor(path, alarm, alarm ? index : index - alarms, candidate.generation);
    unlink(path);
    upload = fopen(temporary, "wb");
    if (!upload) return false;
    receiving = index;
    crc = UINT32_MAX;
    return true;
}
uint8_t chunk(int index, const Item &item, const Item &old, const uint8_t *bytes, size_t size,
              bool first) {
    if (!size || !item.size || item.size > maxPayload) return 2;
    if (upload && ticks() - lastPacket > uploadTimeout) stopUpload();
    if (!upload || (first && received)) {
        if (!first || !startUpload(index, item, old)) return 2;
    }
    if (receiving != index || candidate.crc != item.crc || candidate.size != item.size ||
        item.type != candidate.type || size > candidate.size - received || fwrite(bytes, 1, size, upload) != size) {
        stopUpload();
        return 2;
    }
    candidate.flags = item.flags;
    candidate.start = item.start;
    candidate.end = item.end;
    candidate.duration = item.duration;
    candidate.buzzer = item.buzzer;
    for (size_t i = 0; i < size; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0u);
    }
    received += size;
    lastPacket = ticks();
    if (received != candidate.size) return 1;
    const bool closed = fclose(upload) == 0;
    upload = nullptr;
    receiving = -1;
    const int format = closed ? clockwise_media_format(temporary) : 0;
    const bool formatMatches = candidate.type == 1 ? format == 1 :
        candidate.type == 2 ? format >= 2 && format <= 4 : candidate.type == 3 && format == 5;
    if (!formatMatches || (crc ^ UINT32_MAX) != candidate.crc) {
        stopUpload();
        return 2;
    }
    const bool isAlarm = index < int(alarms);
    const unsigned slot = isAlarm ? index : index - alarms;
    char path[40];
    pathFor(path, isAlarm, slot, candidate.generation);
    if (rename(temporary, path) != 0) { stopUpload(); return 2; }
    if (isAlarm) {
        if (!saveAlarm(slot, candidate)) { unlink(path); return 2; }
        alarmItems[slot] = candidate;
    } else {
        staging.items[slot] = candidate;
        stagedMask |= 1u << slot;
        lastSchedule = ticks();
    }
    return 3;
}

bool legacyMetadata(nvs_handle_t handle, const char *key, void *value, size_t len) {
    size_t read = len;
    return nvs_get_blob(handle, key, value, &read) == ESP_OK && read == len;
}
}

void initEvents() {
    mutex = xSemaphoreCreateMutexStatic(&mutexStorage);
    unlink(temporary);
    nvs_handle_t handle;
    if (nvs_open("idot-alarm", NVS_READONLY, &handle) == ESP_OK) {
        for (unsigned i = 0; i < alarms; ++i) {
            char key[8];
            snprintf(key, sizeof(key), "n%u", i);
            if (legacyMetadata(handle, key, &alarmItems[i], sizeof(Item))) continue;
            snprintf(key, sizeof(key), "a%u", i);
            uint8_t raw[28];
            if (legacyMetadata(handle, key, raw, sizeof(raw)) && raw[0] && raw[2] < 24 && raw[3] < 60)
                alarmItems[i] = {read32(raw + 12), read32(raw + 16),
                                 uint16_t(raw[2] * 60 + raw[3]), 0, raw[1], raw[4], 0, raw[6], raw[7]};
        }
        nvs_close(handle);
    }
    if (nvs_open("idot-sched", NVS_READONLY, &handle) == ESP_OK) {
        if (!legacyMetadata(handle, "native1", &active, sizeof(active))) {
            nvs_get_u8(handle, "flags", &active.flags);
            for (unsigned i = 0; i < schedules; ++i) {
                char key[8];
                uint8_t raw[20];
                snprintf(key, sizeof(key), "s%u", i);
                if (legacyMetadata(handle, key, raw, sizeof(raw)) && raw[0] &&
                    raw[2] < 24 && raw[3] < 60 && raw[4] < 24 && raw[5] < 60) {
                    active.items[i] = {read32(raw + 8), read32(raw + 12),
                        uint16_t(raw[2] * 60 + raw[3]), uint16_t(raw[4] * 60 + raw[5]),
                        raw[1], 0, 0, raw[6]};
                }
            }
        }
        nvs_close(handle);
    }
}

extern "C" uint8_t clockwise_alarm(const uint8_t *data, size_t len) {
    Lock lock;
    if (len < 12 || data[4] >= alarms || data[6] >= 24 || data[7] >= 60) return 2;
    const unsigned slot = data[4];
    Item next = alarmItems[slot];
    next.flags = data[5];
    next.start = data[6] * 60 + data[7];
    next.duration = data[8];
    next.type = data[10];
    next.buzzer = data[11];
    if (len < 24) {
        if (receiving == int(slot)) stopUpload();
        if (!saveAlarm(slot, next)) return 2;
        alarmItems[slot] = next;
        return 1;
    }
    next.size = read32(data + 13);
    next.crc = read32(data + 17);
    return chunk(slot, next, alarmItems[slot], data + 24, len - 24, data[12] == 0);
}

extern "C" uint8_t clockwise_schedule(const uint8_t *data, size_t len) {
    Lock lock;
    if (data[2] == 7 && len == 5) {
        abortSchedule();
        scheduleFailed = false;
        if (!(data[4] & 1)) {
            staging = active;
            staging.flags = data[4];
            if (!saveSchedule(staging)) return 2;
            active = staging;
            return 1;
        }
        staging.flags = 0;
        for (auto &item : staging.items) item = {};
        staging.flags = data[4];
        addingSchedule = true;
        lastSchedule = 0;
        return 1;
    }
    if (len < 24 || !addingSchedule || scheduleFailed || data[4] >= schedules ||
        data[6] >= 24 || data[7] >= 60 || data[8] >= 24 || data[9] >= 60) {
        abortSchedule();
        return 2;
    }
    const unsigned slot = data[4];
    const Item next{read32(data + 12), read32(data + 16), uint16_t(data[6] * 60 + data[7]),
                    uint16_t(data[8] * 60 + data[9]), data[5], 0, 0, data[10]};
    const auto status = chunk(slot + alarms, next, active.items[slot], data + 23, len - 23, data[11] == 0);
    if (status == 2) abortSchedule();
    return status;
}

extern "C" void clockwise_events_disconnect() {
    Lock lock;
    if (receiving >= int(alarms)) abortSchedule();
    else if (receiving >= 0) stopUpload();
}

EventChange pollEvents(uint32_t epoch, char (&path)[40]) {
    Lock lock;
    const auto now = ticks();
    if (upload && now - lastPacket >= uploadTimeout) {
        if (receiving >= int(alarms)) abortSchedule();
        else stopUpload();
    }
    if (addingSchedule && !upload && stagedMask && now - lastSchedule >= scheduleSettle) {
        if (saveSchedule(staging)) {
            active = staging;
            if (displayed >= int(alarms)) displayed = -2;
            ESP_LOGI("events", "Schedule saved, entries=%u", unsigned(__builtin_popcount(stagedMask)));
        }
        addingSchedule = false;
    }
    if (!epoch) return EventChange::None;
    time_t timestamp = epoch;
    tm date{};
    gmtime_r(&timestamp, &date);
    const unsigned day = 1u << (date.tm_wday == 0 ? 7 : date.tm_wday);
    const unsigned minute = date.tm_hour * 60 + date.tm_min;
    if (alarmActive >= 0 && now >= alarmUntil) alarmActive = -1;
    if (alarmActive < 0) {
        for (unsigned i = 0; i < alarms; ++i) {
            auto &alarm = alarmItems[i];
            if (!(alarm.flags & 1) || alarm.start != minute || previousMinute[i] == epoch / 60) continue;
            if ((alarm.flags & 0xfe) && !(alarm.flags & day)) continue;
            previousMinute[i] = epoch / 60;
            alarmActive = i;
            alarmUntil = now + int64_t(alarm.duration ? alarm.duration : 10) * 1000000;
            if (!(alarm.flags & 0xfe)) {
                alarm.flags &= ~1;
                saveAlarm(i, alarm);
            }
            ESP_LOGI("events", "Alarm %u triggered", i);
            break;
        }
    }
    int wanted = alarmActive;
    if (wanted < 0 && (active.flags & 1)) {
        for (unsigned i = 0; i < schedules; ++i) {
            const auto &schedule = active.items[i];
            if (!(schedule.flags & 1) || !(schedule.flags & day) || !schedule.size) continue;
            if (schedule.start < schedule.end ? minute >= schedule.start && minute < schedule.end :
                schedule.start > schedule.end && (minute >= schedule.start || minute < schedule.end)) {
                wanted = i + alarms;
                break;
            }
        }
    }
    if (wanted == displayed) return EventChange::None;
    displayed = wanted;
    if (wanted < 0) return EventChange::End;
    const bool alarm = wanted < int(alarms);
    const unsigned slot = alarm ? wanted : wanted - alarms;
    auto &item = alarm ? alarmItems[slot] : active.items[slot];
    pathFor(path, alarm, slot, item.generation);
    if (!clockwise_media_format(path)) path[0] = 0;
    ESP_LOGI("events", "Displaying %s %u", alarm ? "alarm" : "schedule", slot);
    return EventChange::Start;
}
