#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif
void clockwise_ble_start(void);
void clockwise_set_power(bool on);
void clockwise_set_brightness(uint8_t level);
void clockwise_set_clock(uint32_t seconds);
bool clockwise_accept_effect(const uint8_t *data, size_t size);
uint8_t clockwise_accept_media(const uint8_t *data, size_t size);
void clockwise_media_abort(void);
void clockwise_request_reset(void);
void clockwise_request_clock(uint32_t rgb);
#ifdef __cplusplus
}
#endif
