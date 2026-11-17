#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif
void clockwise_ble_start(void);
void clockwise_set_power(bool on);
uint8_t clockwise_alarm(const uint8_t *data, size_t length);
uint8_t clockwise_schedule(const uint8_t *data, size_t length);
void clockwise_events_disconnect(void);
bool clockwise_preset_activate(const uint8_t *data, size_t length);
bool clockwise_rhythm_submit(const uint8_t *data, size_t length);
bool clockwise_graffiti_submit(const uint8_t *data, size_t length);
void clockwise_set_flip(bool flipped);
void clockwise_set_saving(bool on, unsigned start, unsigned end, unsigned reduction);
void clockwise_request_view(uint8_t mode, uint32_t value);
void clockwise_countdown_finished(void);
void clockwise_set_brightness(uint8_t level);
void clockwise_set_calendar(uint32_t seconds, bool from_ntp);
bool clockwise_accept_effect(const uint8_t *data, size_t size);
uint8_t clockwise_accept_media(const uint8_t *data, size_t size);
void clockwise_media_disconnect(void);
bool clockwise_media_bank(const uint8_t *data, size_t size);
void clockwise_media_replay(void);
uint8_t clockwise_accept_raster(const uint8_t *data, size_t size);
void clockwise_request_reset(void);
void clockwise_request_clock(uint32_t rgb);
#ifdef __cplusplus
}
#endif
