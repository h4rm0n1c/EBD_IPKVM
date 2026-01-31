#include "adb_bus.h"

#include "pico/stdlib.h"
#include "hardware/gpio.h"

#include "adb_events.h"
#include "adb_pio.h"

#define ADB_PULSE_MIN_US 50u
#define ADB_PULSE_MAX_US 1000u

static uint adb_pin_recv = 0;
static uint adb_pin_xmit = 0;
static bool adb_last_state = true;
static uint32_t adb_low_start_us = 0;
static adb_pio_t adb_pio = {0};

static volatile uint32_t adb_rx_pulses = 0;
static volatile uint32_t adb_rx_seen = 0;
static volatile uint32_t adb_events_consumed = 0;
static volatile bool adb_rx_latched = false;

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

    adb_last_state = gpio_get(adb_pin_recv);
    adb_low_start_us = time_us_32();
    adb_rx_pulses = 0;
    adb_rx_seen = 0;
    adb_events_consumed = 0;
    adb_rx_latched = false;
}

static inline void adb_note_rx_pulse(uint32_t pulse_us) {
    if (pulse_us < ADB_PULSE_MIN_US || pulse_us > ADB_PULSE_MAX_US) {
        return;
    }
    __atomic_fetch_add(&adb_rx_pulses, 1u, __ATOMIC_RELAXED);
    __atomic_fetch_add(&adb_rx_seen, 1u, __ATOMIC_RELAXED);
    __atomic_store_n(&adb_rx_latched, true, __ATOMIC_RELEASE);
}

bool adb_bus_poll(void) {
    bool did_work = false;
    uint32_t now_us = time_us_32();
    bool state = gpio_get(adb_pin_recv);

    if (state != adb_last_state) {
        did_work = true;
        adb_last_state = state;
        if (!state) {
            adb_low_start_us = now_us;
        } else {
            uint32_t pulse_us = (uint32_t)(now_us - adb_low_start_us);
            adb_note_rx_pulse(pulse_us);
        }
    }

    uint16_t pulse_count = 0;
    while (adb_pio_rx_pop(&adb_pio, &pulse_count)) {
        adb_note_rx_pulse((uint32_t)pulse_count);
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
    out_stats->rx_pulses = __atomic_load_n(&adb_rx_pulses, __ATOMIC_ACQUIRE);
    out_stats->rx_seen = __atomic_load_n(&adb_rx_seen, __ATOMIC_ACQUIRE);
    out_stats->events_consumed = __atomic_load_n(&adb_events_consumed, __ATOMIC_ACQUIRE);
}
