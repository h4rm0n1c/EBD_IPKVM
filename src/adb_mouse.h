#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    int8_t dx;
    int8_t dy;
    uint8_t buttons;
    bool srq;
} adb_mouse_t;

void adb_mouse_init(adb_mouse_t *mouse);
void adb_mouse_enqueue(adb_mouse_t *mouse, int8_t dx, int8_t dy, uint8_t buttons);
bool adb_mouse_take_report(adb_mouse_t *mouse, int8_t *dx, int8_t *dy, uint8_t *buttons);
bool adb_mouse_has_srq(const adb_mouse_t *mouse);
