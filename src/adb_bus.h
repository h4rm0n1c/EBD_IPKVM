#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "hardware/pio.h"

typedef struct {
    uint32_t attention_count;
    uint32_t reset_count;
    uint32_t command_count;
    uint32_t error_count;
    uint8_t last_command;
} adb_bus_stats_t;

void adb_bus_init(PIO pio, uint sm, uint pin_adb);
bool adb_bus_task(void);
adb_bus_stats_t adb_bus_get_stats(void);
