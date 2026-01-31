#include "adb_core.h"

#include "adb_kbd.h"
#include "adb_mouse.h"
#include "core_bridge.h"

static adb_kbd_t adb_kbd;
static adb_mouse_t adb_mouse;
static uint32_t adb_kbd_events = 0;
static uint32_t adb_mouse_events = 0;

void adb_core_init(void) {
    adb_kbd_init(&adb_kbd);
    adb_mouse_init(&adb_mouse);
    adb_kbd_events = 0;
    adb_mouse_events = 0;
}

void adb_core_task(void) {
    adb_event_t event;
    while (core_bridge_adb_pop(&event)) {
        if (event.type == ADB_EVENT_KEY) {
            bool down = event.b != 0;
            adb_kbd_enqueue(&adb_kbd, event.a, down);
            adb_kbd_events++;
        } else if (event.type == ADB_EVENT_MOUSE) {
            adb_mouse_enqueue(&adb_mouse, event.b, event.c, event.a);
            adb_mouse_events++;
        }
    }
}

uint32_t adb_core_take_kbd_events(void) {
    uint32_t value = adb_kbd_events;
    adb_kbd_events = 0;
    return value;
}

uint32_t adb_core_take_mouse_events(void) {
    uint32_t value = adb_mouse_events;
    adb_mouse_events = 0;
    return value;
}
