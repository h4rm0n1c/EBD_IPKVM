#pragma once

#include <stdbool.h>
#include <stdint.h>

#define ADB_KBD_QUEUE_DEPTH 32

typedef struct {
    uint8_t queue[ADB_KBD_QUEUE_DEPTH];
    uint8_t w;
    uint8_t r;
    uint8_t reg2;
    bool srq;
} adb_kbd_t;

void adb_kbd_init(adb_kbd_t *kbd);
bool adb_kbd_enqueue(adb_kbd_t *kbd, uint8_t keycode, bool key_down);
bool adb_kbd_take_report(adb_kbd_t *kbd, uint8_t *out0, uint8_t *out1);
void adb_kbd_set_leds(adb_kbd_t *kbd, uint8_t leds);
bool adb_kbd_has_srq(const adb_kbd_t *kbd);
