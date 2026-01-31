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
} adb_pio_t;

void adb_pio_init(adb_pio_t *ctx, PIO pio, uint sm_rx, uint sm_tx, uint pin_recv, uint pin_xmit);
bool adb_pio_rx_pop(adb_pio_t *ctx, uint16_t *out_count);
void adb_pio_tx_pulse(adb_pio_t *ctx, uint16_t cycles);
