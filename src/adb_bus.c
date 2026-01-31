#include "adb_bus.h"

#include "pico/stdlib.h"
#include "hardware/gpio.h"

#include "adb_events.h"
#include "adb_pio.h"

#define ADB_PULSE_MIN_US 30u
#define ADB_PULSE_MAX_US 1000u
#define ADB_MAX_PULSES_PER_POLL 64u
#define ADB_ATTENTION_MIN_US 600u
#define ADB_ATTENTION_MAX_US 1100u
#define ADB_SYNC_MIN_US 60u
#define ADB_SYNC_MAX_US 90u

static uint adb_pin_recv = 0;
static uint adb_pin_xmit = 0;
static adb_pio_t adb_pio = {0};

static volatile uint32_t adb_rx_pulses = 0;
static volatile uint32_t adb_rx_seen = 0;
static volatile uint32_t adb_rx_overruns = 0;
static volatile uint32_t adb_attention_pulses = 0;
static volatile uint32_t adb_sync_pulses = 0;
static volatile uint32_t adb_events_consumed = 0;
static volatile bool adb_rx_latched = false;
static volatile uint32_t adb_rx_raw_pulses = 0;
static volatile uint32_t adb_last_pulse_us = 0;

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
    adb_rx_latched = false;
    adb_rx_raw_pulses = 0;
    adb_last_pulse_us = 0;
}

static inline void adb_note_rx_pulse(uint32_t pulse_us) {
    __atomic_fetch_add(&adb_rx_raw_pulses, 1u, __ATOMIC_RELAXED);
    __atomic_store_n(&adb_last_pulse_us, pulse_us, __ATOMIC_RELEASE);
    if (pulse_us >= ADB_ATTENTION_MIN_US && pulse_us <= ADB_ATTENTION_MAX_US) {
        __atomic_fetch_add(&adb_attention_pulses, 1u, __ATOMIC_RELAXED);
    } else if (pulse_us >= ADB_SYNC_MIN_US && pulse_us <= ADB_SYNC_MAX_US) {
        __atomic_fetch_add(&adb_sync_pulses, 1u, __ATOMIC_RELAXED);
    }
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

    adb_event_t event;
    if (adb_events_pop(&event)) {
        __atomic_fetch_add(&adb_events_consumed, 1u, __ATOMIC_RELAXED);
        did_work = true;
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
    out_stats->last_pulse_us = __atomic_load_n(&adb_last_pulse_us, __ATOMIC_ACQUIRE);
}
