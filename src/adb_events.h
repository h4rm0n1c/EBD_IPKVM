#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    ADB_EVENT_KEY = 1,
    ADB_EVENT_MOUSE = 2,
} adb_event_type_t;

typedef struct {
    uint8_t type;
    uint8_t a;
    int8_t b;
    int8_t c;
} adb_event_t;

void adb_events_init(void);
bool adb_events_push(const adb_event_t *event);
bool adb_events_pop(adb_event_t *out_event);
uint16_t adb_events_pending(void);
uint32_t adb_events_get_drop_count(void);
