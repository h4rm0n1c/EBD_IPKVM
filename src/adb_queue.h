#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    ADB_EVENT_KEY = 0,
    ADB_EVENT_MOUSE = 1,
} adb_event_type_t;

typedef struct {
    uint8_t code;
    bool down;
} adb_key_event_t;

typedef struct {
    int8_t dx;
    int8_t dy;
    uint8_t buttons;
} adb_mouse_event_t;

typedef struct {
    adb_event_type_t type;
    union {
        adb_key_event_t key;
        adb_mouse_event_t mouse;
    } data;
} adb_event_t;

#define ADB_MOUSE_BUTTON_PRIMARY 0x01u

void adb_queue_init(void);
bool adb_queue_push(const adb_event_t *event);
bool adb_queue_pop(adb_event_t *event);
uint16_t adb_queue_count(void);
uint32_t adb_queue_take_drops(void);
