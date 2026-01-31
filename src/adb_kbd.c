#include "adb_kbd.h"

#include <string.h>

#define ADB_KEY_UP_MASK 0x80u
#define ADB_KEY_EMPTY 0xFFu

static inline uint8_t adb_kbd_next(uint8_t idx) {
    return (uint8_t)((idx + 1u) % ADB_KBD_QUEUE_DEPTH);
}

void adb_kbd_init(adb_kbd_t *kbd) {
    memset(kbd, 0, sizeof(*kbd));
    kbd->queue[0] = ADB_KEY_EMPTY;
    kbd->queue[1] = ADB_KEY_EMPTY;
    kbd->srq = false;
}

bool adb_kbd_enqueue(adb_kbd_t *kbd, uint8_t keycode, bool key_down) {
    uint8_t next = adb_kbd_next(kbd->w);
    if (next == kbd->r) {
        return false;
    }
    uint8_t packed = key_down ? (keycode & ~ADB_KEY_UP_MASK)
                              : (uint8_t)(keycode | ADB_KEY_UP_MASK);
    kbd->queue[kbd->w] = packed;
    kbd->w = next;
    kbd->srq = true;
    return true;
}

bool adb_kbd_take_report(adb_kbd_t *kbd, uint8_t *out0, uint8_t *out1) {
    if (!out0 || !out1) {
        return false;
    }
    uint8_t first = ADB_KEY_EMPTY;
    uint8_t second = ADB_KEY_EMPTY;
    if (kbd->r != kbd->w) {
        first = kbd->queue[kbd->r];
        kbd->r = adb_kbd_next(kbd->r);
        if (kbd->r != kbd->w) {
            second = kbd->queue[kbd->r];
            kbd->r = adb_kbd_next(kbd->r);
        }
    }
    *out0 = first;
    *out1 = second;
    kbd->srq = (kbd->r != kbd->w);
    return true;
}

void adb_kbd_set_leds(adb_kbd_t *kbd, uint8_t leds) {
    kbd->reg2 = leds;
}

bool adb_kbd_has_srq(const adb_kbd_t *kbd) {
    return kbd->srq;
}
