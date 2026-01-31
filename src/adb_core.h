#pragma once

#include <stdint.h>

void adb_core_init(void);
void adb_core_task(void);
uint32_t adb_core_take_kbd_events(void);
uint32_t adb_core_take_mouse_events(void);
