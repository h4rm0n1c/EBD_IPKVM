#include "adb_bus.h"

#include <string.h>

#include "pico/stdlib.h"

#include "adb_pio.h"
#include "adb_pio.pio.h"

#define ADB_RESET_MIN_US 2500u
#define ADB_ATTENTION_MIN_US 700u
#define ADB_ATTENTION_MAX_US 1100u
#define ADB_CELL_MIN_US 60u
#define ADB_CELL_MAX_US 140u

typedef enum {
    ADB_BUS_IDLE = 0,
    ADB_BUS_SYNC = 1,
    ADB_BUS_CMD = 2,
    ADB_BUS_STOP = 3,
} adb_bus_state_t;

static struct {
    PIO pio;
    uint sm;
    uint pin;
    adb_bus_state_t state;
    uint8_t bit_index;
    uint8_t cmd;
    adb_bus_stats_t stats;
} adb_bus;

static inline uint32_t adb_duration_us(uint32_t remaining) {
    return 0xFFFFFFFFu - remaining;
}

static inline bool adb_is_attention(uint32_t low_us) {
    return (low_us >= ADB_ATTENTION_MIN_US) && (low_us <= ADB_ATTENTION_MAX_US);
}

static inline bool adb_is_reset(uint32_t low_us) {
    return low_us >= ADB_RESET_MIN_US;
}

static inline bool adb_is_cell(uint32_t low_us, uint32_t high_us) {
    uint32_t total = low_us + high_us;
    return (total >= ADB_CELL_MIN_US) && (total <= ADB_CELL_MAX_US);
}

static inline bool adb_cell_bit(uint32_t low_us, uint32_t high_us) {
    return low_us < high_us;
}

void adb_bus_init(PIO pio, uint sm, uint pin_adb) {
    memset(&adb_bus, 0, sizeof(adb_bus));
    adb_bus.pio = pio;
    adb_bus.sm = sm;
    adb_bus.pin = pin_adb;

    uint offset = pio_add_program(pio, &adb_edge_sampler_program);
    adb_pio_init(pio, sm, offset, pin_adb);
    adb_bus.state = ADB_BUS_IDLE;
}

static void adb_bus_reset_state(void) {
    adb_bus.state = ADB_BUS_IDLE;
    adb_bus.bit_index = 0;
    adb_bus.cmd = 0;
}

bool adb_bus_task(void) {
    bool did_work = false;
    while (!pio_sm_is_rx_fifo_empty(adb_bus.pio, adb_bus.sm)) {
        uint32_t low_remaining = pio_sm_get(adb_bus.pio, adb_bus.sm);
        if (pio_sm_is_rx_fifo_empty(adb_bus.pio, adb_bus.sm)) {
            break;
        }
        uint32_t high_remaining = pio_sm_get(adb_bus.pio, adb_bus.sm);
        did_work = true;

        uint32_t low_us = adb_duration_us(low_remaining);
        uint32_t high_us = adb_duration_us(high_remaining);

        if (adb_is_reset(low_us)) {
            adb_bus.stats.reset_count++;
            adb_bus_reset_state();
            continue;
        }

        if (adb_is_attention(low_us)) {
            adb_bus.stats.attention_count++;
            adb_bus.state = ADB_BUS_SYNC;
            continue;
        }

        if (!adb_is_cell(low_us, high_us)) {
            adb_bus.stats.error_count++;
            adb_bus_reset_state();
            continue;
        }

        bool bit = adb_cell_bit(low_us, high_us);
        switch (adb_bus.state) {
        case ADB_BUS_IDLE:
            break;
        case ADB_BUS_SYNC:
            if (!bit) {
                adb_bus.stats.error_count++;
                adb_bus_reset_state();
                break;
            }
            adb_bus.state = ADB_BUS_CMD;
            adb_bus.bit_index = 0;
            adb_bus.cmd = 0;
            break;
        case ADB_BUS_CMD:
            adb_bus.cmd = (uint8_t)((adb_bus.cmd << 1) | (bit ? 1u : 0u));
            adb_bus.bit_index++;
            if (adb_bus.bit_index >= 8) {
                adb_bus.stats.command_count++;
                adb_bus.stats.last_command = adb_bus.cmd;
                adb_bus.state = ADB_BUS_STOP;
            }
            break;
        case ADB_BUS_STOP:
            if (bit) {
                adb_bus.stats.error_count++;
            }
            adb_bus_reset_state();
            break;
        default:
            adb_bus_reset_state();
            break;
        }
    }
    return did_work;
}

adb_bus_stats_t adb_bus_get_stats(void) {
    return adb_bus.stats;
}
