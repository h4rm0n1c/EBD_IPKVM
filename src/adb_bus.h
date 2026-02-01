#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "pico/stdlib.h"

typedef struct {
    uint32_t rx_raw_pulses;
    uint32_t rx_pulses;
    uint32_t rx_seen;
    uint32_t rx_overruns;
    uint32_t attention_pulses;
    uint32_t sync_pulses;
    uint32_t events_consumed;
    uint32_t events_pending;
    uint32_t cmd_bytes;
    uint32_t last_cmd;
    uint32_t cmd_addr_miss;
    uint32_t srq_pulses;
    uint32_t tx_attempts;
    uint32_t tx_success;
    uint32_t tx_busy;
    uint32_t tx_late_busy;
    uint32_t last_pulse_us;
    uint32_t min_pulse_us;
    uint32_t max_pulse_us;
    uint32_t pulse_zero_us;
    uint32_t pulse_lt_30_us;
    uint32_t pulse_30_60_us;
    uint32_t pulse_60_90_us;
    uint32_t pulse_90_200_us;
    uint32_t pulse_200_600_us;
    uint32_t pulse_600_700_us;
    uint32_t pulse_700_900_us;
    uint32_t pulse_900_1100_us;
    uint32_t pulse_gt_1100_us;
} adb_bus_stats_t;

void adb_bus_init(uint pin_recv, uint pin_xmit);
bool adb_bus_poll(void);
bool adb_bus_take_rx_seen(void);
void adb_bus_get_stats(adb_bus_stats_t *out_stats);
bool adb_bus_tx_test_pulse_us(uint32_t low_us);
