#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "hardware/pio.h"

typedef struct {
    PIO pio;
    uint sm_rx;
    uint sm_tx;
    uint offset_rx;
    uint offset_tx;
    uint pin_recv;
    uint pin_xmit;
    uint32_t tick_hz;
} adb_pio_t;

void adb_pio_init(adb_pio_t *ctx, PIO pio, uint sm_rx, uint sm_tx, uint pin_recv, uint pin_xmit);
bool adb_pio_rx_pop(adb_pio_t *ctx, uint32_t *out_count);
bool adb_pio_rx_has_data(const adb_pio_t *ctx);
void adb_pio_rx_flush(adb_pio_t *ctx);
void adb_pio_tx_pulse(adb_pio_t *ctx, uint16_t cycles);
uint32_t adb_pio_ticks_to_us(const adb_pio_t *ctx, uint32_t ticks);
uint16_t adb_pio_us_to_cycles(const adb_pio_t *ctx, uint32_t us);
