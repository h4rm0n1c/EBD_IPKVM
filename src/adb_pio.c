#include "adb_pio.h"

#include "hardware/clocks.h"
#include "hardware/gpio.h"

#include "adb_pio.pio.h"

static const float adb_pio_clkdiv = 8.0f;

void adb_pio_init(adb_pio_t *ctx, PIO pio, uint sm_rx, uint sm_tx, uint pin_recv, uint pin_xmit) {
    if (!ctx) {
        return;
    }

    ctx->pio = pio;
    ctx->sm_rx = sm_rx;
    ctx->sm_tx = sm_tx;
    ctx->pin_recv = pin_recv;
    ctx->pin_xmit = pin_xmit;
    ctx->offset_rx = pio_add_program(pio, &adb_rx_program);
    ctx->offset_tx = pio_add_program(pio, &adb_tx_program);

    pio_sm_config rx_cfg = adb_rx_program_get_default_config(ctx->offset_rx);
    sm_config_set_in_pins(&rx_cfg, pin_recv);
    sm_config_set_jmp_pin(&rx_cfg, pin_recv);
    sm_config_set_fifo_join(&rx_cfg, PIO_FIFO_JOIN_RX);
    sm_config_set_clkdiv(&rx_cfg, adb_pio_clkdiv);
    pio_sm_init(pio, sm_rx, ctx->offset_rx, &rx_cfg);
    pio_sm_set_enabled(pio, sm_rx, true);

    pio_sm_config tx_cfg = adb_tx_program_get_default_config(ctx->offset_tx);
    sm_config_set_set_pins(&tx_cfg, pin_xmit, 1);
    sm_config_set_clkdiv(&tx_cfg, adb_pio_clkdiv);
    pio_sm_init(pio, sm_tx, ctx->offset_tx, &tx_cfg);
    pio_sm_set_enabled(pio, sm_tx, true);

    pio_sm_set_consecutive_pindirs(pio, sm_rx, pin_recv, 1, false);
    pio_sm_set_consecutive_pindirs(pio, sm_tx, pin_xmit, 1, false);

    gpio_set_function(pin_recv, GPIO_FUNC_PIO1);
    gpio_set_function(pin_xmit, GPIO_FUNC_PIO1);

    uint32_t clk_hz = clock_get_hz(clk_sys);
    ctx->tick_hz = (uint32_t)((float)clk_hz / adb_pio_clkdiv);
}

bool adb_pio_rx_pop(adb_pio_t *ctx, uint32_t *out_count) {
    if (!ctx || !out_count) {
        return false;
    }
    if (pio_sm_is_rx_fifo_empty(ctx->pio, ctx->sm_rx)) {
        return false;
    }
    uint32_t raw = pio_sm_get(ctx->pio, ctx->sm_rx);
    *out_count = 0xFFFFFFFFu - raw;
    return true;
}

bool adb_pio_rx_has_data(const adb_pio_t *ctx) {
    if (!ctx) {
        return false;
    }
    return !pio_sm_is_rx_fifo_empty(ctx->pio, ctx->sm_rx);
}

void adb_pio_tx_pulse(adb_pio_t *ctx, uint16_t cycles) {
    if (!ctx) {
        return;
    }
    pio_sm_put_blocking(ctx->pio, ctx->sm_tx, cycles);
}

uint32_t adb_pio_ticks_to_us(const adb_pio_t *ctx, uint32_t ticks) {
    if (!ctx || ctx->tick_hz == 0u) {
        return 0u;
    }
    return (uint32_t)(((uint64_t)ticks * 1000000ull) / ctx->tick_hz);
}
