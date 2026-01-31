#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "pico/stdlib.h"

typedef struct {
    uint32_t rx_raw_pulses;
    uint32_t rx_pulses;
    uint32_t rx_seen;
    uint32_t events_consumed;
    uint32_t last_pulse_us;
} adb_bus_stats_t;

void adb_bus_init(uint pin_recv, uint pin_xmit);
bool adb_bus_poll(void);
bool adb_bus_take_rx_seen(void);
void adb_bus_get_stats(adb_bus_stats_t *out_stats);
