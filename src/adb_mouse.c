#include "adb_mouse.h"

void adb_mouse_init(adb_mouse_t *mouse) {
    mouse->dx = 0;
    mouse->dy = 0;
    mouse->buttons = 0;
    mouse->srq = false;
}

void adb_mouse_enqueue(adb_mouse_t *mouse, int8_t dx, int8_t dy, uint8_t buttons) {
    mouse->dx = dx;
    mouse->dy = dy;
    mouse->buttons = buttons;
    mouse->srq = true;
}

bool adb_mouse_take_report(adb_mouse_t *mouse, int8_t *dx, int8_t *dy, uint8_t *buttons) {
    if (!dx || !dy || !buttons) {
        return false;
    }
    *dx = mouse->dx;
    *dy = mouse->dy;
    *buttons = mouse->buttons;
    mouse->dx = 0;
    mouse->dy = 0;
    mouse->srq = false;
    return true;
}

bool adb_mouse_has_srq(const adb_mouse_t *mouse) {
    return mouse->srq;
}
