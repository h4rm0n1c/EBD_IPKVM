#include "adb_bus.h"

#include "pico/stdlib.h"
#include "hardware/gpio.h"

#include "adb_events.h"
#include "adb_pio.h"

#define ADB_PULSE_MIN_US 30u
#define ADB_PULSE_MAX_US 1000u
#define ADB_MAX_PULSES_PER_POLL 64u
#define ADB_ATTENTION_MIN_US 700u
#define ADB_ATTENTION_MAX_US 900u
#define ADB_SYNC_MIN_US 60u
#define ADB_SYNC_MAX_US 90u
#define ADB_BIT_ZERO_MAX_US 60u
#define ADB_BIT_ONE_MIN_US 60u
#define ADB_BIT_ONE_MAX_US 90u
#define ADB_KBD_ADDR 2u
#define ADB_TX_BIT_CELL_US 100u
#define ADB_TX_BIT_ONE_LOW_US 35u
#define ADB_TX_BIT_ZERO_LOW_US 65u
#define ADB_TX_TALK_DELAY_US 190u

static uint adb_pin_recv = 0;
static uint adb_pin_xmit = 0;
static adb_pio_t adb_pio = {0};

typedef enum {
    ADB_RX_IDLE = 0,
    ADB_RX_GOT_ATTENTION,
    ADB_RX_BITS,
} adb_rx_state_t;

static volatile uint32_t adb_rx_pulses = 0;
static volatile uint32_t adb_rx_seen = 0;
static volatile uint32_t adb_rx_overruns = 0;
static volatile uint32_t adb_attention_pulses = 0;
static volatile uint32_t adb_sync_pulses = 0;
static volatile uint32_t adb_events_consumed = 0;
static volatile uint32_t adb_cmd_bytes = 0;
static volatile uint32_t adb_last_cmd = 0;
static volatile bool adb_rx_latched = false;
static volatile uint32_t adb_rx_raw_pulses = 0;
static volatile uint32_t adb_last_pulse_us = 0;
static volatile uint32_t adb_min_pulse_us = UINT32_MAX;
static volatile uint32_t adb_max_pulse_us = 0;
static volatile uint32_t adb_pulse_zero_us = 0;
static volatile uint32_t adb_pulse_lt_30_us = 0;
static volatile uint32_t adb_pulse_30_60_us = 0;
static volatile uint32_t adb_pulse_60_90_us = 0;
static volatile uint32_t adb_pulse_90_200_us = 0;
static volatile uint32_t adb_pulse_200_600_us = 0;
static volatile uint32_t adb_pulse_600_700_us = 0;
static volatile uint32_t adb_pulse_700_900_us = 0;
static volatile uint32_t adb_pulse_900_1100_us = 0;
static volatile uint32_t adb_pulse_gt_1100_us = 0;
static adb_rx_state_t adb_rx_state = ADB_RX_IDLE;
static uint8_t adb_rx_bit_count = 0;
static uint8_t adb_rx_shift = 0;
static uint32_t adb_cmd_bytes_handled = 0;

void adb_bus_init(uint pin_recv, uint pin_xmit) {
    adb_pin_recv = pin_recv;
    adb_pin_xmit = pin_xmit;

    adb_pio_init(&adb_pio, pio1, 0, 1, pin_recv, pin_xmit);

    gpio_init(adb_pin_recv);
    gpio_set_dir(adb_pin_recv, GPIO_IN);
    gpio_disable_pulls(adb_pin_recv);

    gpio_init(adb_pin_xmit);
    gpio_set_dir(adb_pin_xmit, GPIO_IN);
    gpio_disable_pulls(adb_pin_xmit);

    adb_rx_pulses = 0;
    adb_rx_seen = 0;
    adb_rx_overruns = 0;
    adb_attention_pulses = 0;
    adb_sync_pulses = 0;
    adb_events_consumed = 0;
    adb_cmd_bytes = 0;
    adb_last_cmd = 0;
    adb_rx_latched = false;
    adb_rx_raw_pulses = 0;
    adb_last_pulse_us = 0;
    adb_min_pulse_us = UINT32_MAX;
    adb_max_pulse_us = 0;
    adb_pulse_zero_us = 0;
    adb_pulse_lt_30_us = 0;
    adb_pulse_30_60_us = 0;
    adb_pulse_60_90_us = 0;
    adb_pulse_90_200_us = 0;
    adb_pulse_200_600_us = 0;
    adb_pulse_600_700_us = 0;
    adb_pulse_700_900_us = 0;
    adb_pulse_900_1100_us = 0;
    adb_pulse_gt_1100_us = 0;
    adb_rx_state = ADB_RX_IDLE;
    adb_rx_bit_count = 0;
    adb_rx_shift = 0;
    adb_cmd_bytes_handled = 0;
}

static inline bool adb_line_idle(void) {
    return gpio_get(adb_pin_recv);
}

static void adb_tx_bit(bool one) {
    uint32_t low_us = one ? ADB_TX_BIT_ONE_LOW_US : ADB_TX_BIT_ZERO_LOW_US;
    uint32_t high_us = ADB_TX_BIT_CELL_US - low_us;
    uint16_t cycles = adb_pio_us_to_cycles(&adb_pio, low_us);
    adb_pio_tx_pulse(&adb_pio, cycles);
    sleep_us(high_us);
}

static bool adb_tx_bytes(const uint8_t *data, size_t len) {
    if (!data || len == 0u) {
        return false;
    }
    if (!adb_line_idle()) {
        return false;
    }
    sleep_us(ADB_TX_TALK_DELAY_US);
    if (!adb_line_idle()) {
        return false;
    }
    adb_tx_bit(true);
    for (size_t i = 0; i < len; i++) {
        uint8_t byte = data[i];
        for (int bit = 7; bit >= 0; bit--) {
            bool one = (byte & (1u << bit)) != 0u;
            adb_tx_bit(one);
        }
    }
    adb_tx_bit(false);
    return true;
}

static bool adb_try_keyboard_reg0(void) {
    uint8_t bytes[2] = {0xFFu, 0xFFu};
    uint32_t consumed = 0;
    adb_event_t ev;
    while (consumed < 2u && adb_events_pop(&ev)) {
        if (ev.type != ADB_EVENT_KEY) {
            continue;
        }
        uint8_t code = ev.a;
        if (!ev.b) {
            code |= 0x80u;
        }
        bytes[consumed] = code;
        consumed++;
    }
    if (consumed == 0u) {
        return false;
    }
    if (!adb_tx_bytes(bytes, 2u)) {
        return false;
    }
    __atomic_fetch_add(&adb_events_consumed, consumed, __ATOMIC_RELAXED);
    return true;
}

static void adb_handle_command(uint8_t cmd) {
    uint8_t address = (uint8_t)(cmd >> 4);
    uint8_t lcmd = (uint8_t)(cmd & 0x0Fu);
    if (lcmd == 1u) {
        return;
    }
    lcmd >>= 2;
    if (lcmd != 3u) {
        return;
    }
    uint8_t reg = (uint8_t)(cmd & 0x03u);
    if (address == ADB_KBD_ADDR && reg == 0u) {
        adb_try_keyboard_reg0();
    }
}

static inline void adb_note_rx_state(uint32_t pulse_us) {
    if (pulse_us >= ADB_ATTENTION_MIN_US && pulse_us <= ADB_ATTENTION_MAX_US) {
        adb_rx_state = ADB_RX_GOT_ATTENTION;
        adb_rx_bit_count = 0;
        adb_rx_shift = 0;
        return;
    }

    if (adb_rx_state == ADB_RX_GOT_ATTENTION) {
        if (pulse_us >= ADB_SYNC_MIN_US && pulse_us <= ADB_SYNC_MAX_US) {
            adb_rx_state = ADB_RX_BITS;
            adb_rx_bit_count = 0;
            adb_rx_shift = 0;
        } else {
            adb_rx_state = ADB_RX_IDLE;
        }
        return;
    }

    if (adb_rx_state != ADB_RX_BITS) {
        return;
    }

    uint8_t bit = 0;
    if (pulse_us < ADB_BIT_ZERO_MAX_US && pulse_us >= ADB_PULSE_MIN_US) {
        bit = 0;
    } else if (pulse_us >= ADB_BIT_ONE_MIN_US && pulse_us <= ADB_BIT_ONE_MAX_US) {
        bit = 1;
    } else {
        adb_rx_state = ADB_RX_IDLE;
        adb_rx_bit_count = 0;
        adb_rx_shift = 0;
        return;
    }

    adb_rx_shift = (uint8_t)((adb_rx_shift << 1u) | bit);
    adb_rx_bit_count++;
    if (adb_rx_bit_count >= 8u) {
        __atomic_store_n(&adb_last_cmd, adb_rx_shift, __ATOMIC_RELEASE);
        __atomic_fetch_add(&adb_cmd_bytes, 1u, __ATOMIC_RELAXED);
        adb_rx_state = ADB_RX_IDLE;
        adb_rx_bit_count = 0;
        adb_rx_shift = 0;
    }
}

static inline void adb_note_rx_pulse(uint32_t pulse_us) {
    __atomic_fetch_add(&adb_rx_raw_pulses, 1u, __ATOMIC_RELAXED);
    if (pulse_us == 0u) {
        __atomic_fetch_add(&adb_pulse_zero_us, 1u, __ATOMIC_RELAXED);
        return;
    }
    __atomic_store_n(&adb_last_pulse_us, pulse_us, __ATOMIC_RELEASE);
    uint32_t min_pulse = __atomic_load_n(&adb_min_pulse_us, __ATOMIC_ACQUIRE);
    if (pulse_us < min_pulse) {
        __atomic_store_n(&adb_min_pulse_us, pulse_us, __ATOMIC_RELEASE);
    }
    uint32_t max_pulse = __atomic_load_n(&adb_max_pulse_us, __ATOMIC_ACQUIRE);
    if (pulse_us > max_pulse) {
        __atomic_store_n(&adb_max_pulse_us, pulse_us, __ATOMIC_RELEASE);
    }
    if (pulse_us < 30u) {
        __atomic_fetch_add(&adb_pulse_lt_30_us, 1u, __ATOMIC_RELAXED);
    } else if (pulse_us < 60u) {
        __atomic_fetch_add(&adb_pulse_30_60_us, 1u, __ATOMIC_RELAXED);
    } else if (pulse_us <= 90u) {
        __atomic_fetch_add(&adb_pulse_60_90_us, 1u, __ATOMIC_RELAXED);
    } else if (pulse_us < 200u) {
        __atomic_fetch_add(&adb_pulse_90_200_us, 1u, __ATOMIC_RELAXED);
    } else if (pulse_us < 600u) {
        __atomic_fetch_add(&adb_pulse_200_600_us, 1u, __ATOMIC_RELAXED);
    } else if (pulse_us < 700u) {
        __atomic_fetch_add(&adb_pulse_600_700_us, 1u, __ATOMIC_RELAXED);
    } else if (pulse_us <= 900u) {
        __atomic_fetch_add(&adb_pulse_700_900_us, 1u, __ATOMIC_RELAXED);
    } else if (pulse_us <= 1100u) {
        __atomic_fetch_add(&adb_pulse_900_1100_us, 1u, __ATOMIC_RELAXED);
    } else {
        __atomic_fetch_add(&adb_pulse_gt_1100_us, 1u, __ATOMIC_RELAXED);
    }
    if (pulse_us >= ADB_ATTENTION_MIN_US && pulse_us <= ADB_ATTENTION_MAX_US) {
        __atomic_fetch_add(&adb_attention_pulses, 1u, __ATOMIC_RELAXED);
    } else if (pulse_us >= ADB_SYNC_MIN_US && pulse_us <= ADB_SYNC_MAX_US) {
        __atomic_fetch_add(&adb_sync_pulses, 1u, __ATOMIC_RELAXED);
    }
    adb_note_rx_state(pulse_us);
    if (pulse_us < ADB_PULSE_MIN_US || pulse_us > ADB_PULSE_MAX_US) {
        return;
    }
    __atomic_fetch_add(&adb_rx_pulses, 1u, __ATOMIC_RELAXED);
    __atomic_fetch_add(&adb_rx_seen, 1u, __ATOMIC_RELAXED);
    __atomic_store_n(&adb_rx_latched, true, __ATOMIC_RELEASE);
}

bool adb_bus_poll(void) {
    bool did_work = false;
    uint32_t pulse_count = 0;
    uint32_t pulses_handled = 0;
    while (pulses_handled < ADB_MAX_PULSES_PER_POLL
           && adb_pio_rx_pop(&adb_pio, &pulse_count)) {
        uint32_t pulse_us = adb_pio_ticks_to_us(&adb_pio, pulse_count);
        adb_note_rx_pulse(pulse_us);
        did_work = true;
        pulses_handled++;
    }
    if (pulses_handled >= ADB_MAX_PULSES_PER_POLL && adb_pio_rx_has_data(&adb_pio)) {
        __atomic_fetch_add(&adb_rx_overruns, 1u, __ATOMIC_RELAXED);
        adb_pio_rx_flush(&adb_pio);
        did_work = true;
    }

    uint32_t cmd_bytes = __atomic_load_n(&adb_cmd_bytes, __ATOMIC_ACQUIRE);
    if (cmd_bytes != adb_cmd_bytes_handled) {
        adb_cmd_bytes_handled = cmd_bytes;
        uint8_t cmd = (uint8_t)__atomic_load_n(&adb_last_cmd, __ATOMIC_ACQUIRE);
        adb_handle_command(cmd);
    }

    return did_work;
}

bool adb_bus_take_rx_seen(void) {
    return __atomic_exchange_n(&adb_rx_latched, false, __ATOMIC_ACQ_REL);
}

void adb_bus_get_stats(adb_bus_stats_t *out_stats) {
    if (!out_stats) {
        return;
    }
    out_stats->rx_raw_pulses = __atomic_load_n(&adb_rx_raw_pulses, __ATOMIC_ACQUIRE);
    out_stats->rx_pulses = __atomic_load_n(&adb_rx_pulses, __ATOMIC_ACQUIRE);
    out_stats->rx_seen = __atomic_load_n(&adb_rx_seen, __ATOMIC_ACQUIRE);
    out_stats->rx_overruns = __atomic_load_n(&adb_rx_overruns, __ATOMIC_ACQUIRE);
    out_stats->attention_pulses = __atomic_load_n(&adb_attention_pulses, __ATOMIC_ACQUIRE);
    out_stats->sync_pulses = __atomic_load_n(&adb_sync_pulses, __ATOMIC_ACQUIRE);
    out_stats->events_consumed = __atomic_load_n(&adb_events_consumed, __ATOMIC_ACQUIRE);
    out_stats->events_pending = adb_events_pending();
    out_stats->cmd_bytes = __atomic_load_n(&adb_cmd_bytes, __ATOMIC_ACQUIRE);
    out_stats->last_cmd = __atomic_load_n(&adb_last_cmd, __ATOMIC_ACQUIRE);
    out_stats->last_pulse_us = __atomic_load_n(&adb_last_pulse_us, __ATOMIC_ACQUIRE);
    uint32_t min_pulse = __atomic_load_n(&adb_min_pulse_us, __ATOMIC_ACQUIRE);
    if (min_pulse == UINT32_MAX) {
        min_pulse = 0;
    }
    out_stats->min_pulse_us = min_pulse;
    out_stats->max_pulse_us = __atomic_load_n(&adb_max_pulse_us, __ATOMIC_ACQUIRE);
    out_stats->pulse_zero_us = __atomic_load_n(&adb_pulse_zero_us, __ATOMIC_ACQUIRE);
    out_stats->pulse_lt_30_us = __atomic_load_n(&adb_pulse_lt_30_us, __ATOMIC_ACQUIRE);
    out_stats->pulse_30_60_us = __atomic_load_n(&adb_pulse_30_60_us, __ATOMIC_ACQUIRE);
    out_stats->pulse_60_90_us = __atomic_load_n(&adb_pulse_60_90_us, __ATOMIC_ACQUIRE);
    out_stats->pulse_90_200_us = __atomic_load_n(&adb_pulse_90_200_us, __ATOMIC_ACQUIRE);
    out_stats->pulse_200_600_us = __atomic_load_n(&adb_pulse_200_600_us, __ATOMIC_ACQUIRE);
    out_stats->pulse_600_700_us = __atomic_load_n(&adb_pulse_600_700_us, __ATOMIC_ACQUIRE);
    out_stats->pulse_700_900_us = __atomic_load_n(&adb_pulse_700_900_us, __ATOMIC_ACQUIRE);
    out_stats->pulse_900_1100_us = __atomic_load_n(&adb_pulse_900_1100_us, __ATOMIC_ACQUIRE);
    out_stats->pulse_gt_1100_us = __atomic_load_n(&adb_pulse_gt_1100_us, __ATOMIC_ACQUIRE);
}
