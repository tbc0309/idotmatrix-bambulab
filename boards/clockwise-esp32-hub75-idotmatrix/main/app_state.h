#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
void clockwise_ble_start(void);
void clockwise_set_power(bool on);
void clockwise_set_brightness(uint8_t level);
void clockwise_set_clock(uint32_t seconds);
#ifdef __cplusplus
}
#endif
