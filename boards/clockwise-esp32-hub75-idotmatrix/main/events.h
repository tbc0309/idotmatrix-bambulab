#pragma once
#include <stdint.h>

enum class EventChange { None, Start, End };
void initEvents();
EventChange pollEvents(uint32_t epoch, char (&path)[40]);
