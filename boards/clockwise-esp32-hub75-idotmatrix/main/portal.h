#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif
void clockwise_wifi_start(void);
void clockwise_wifi_poll(void);
bool clockwise_wifi_has_setup_client(void);
#ifdef __cplusplus
}
#endif
