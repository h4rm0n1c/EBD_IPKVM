#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t key_events;
    uint32_t mouse_events;
    uint32_t drops;
    uint32_t pending;
} adb_core_stats_t;

void adb_core_init(void);
bool adb_core_service(void);
void adb_core_get_stats(adb_core_stats_t *out);
