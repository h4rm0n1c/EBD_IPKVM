#include "adb_bus.h"

#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "pico/stdlib.h"

#include "adb_bus.pio.h"

#define ADB_PIN_RECV 6
#define ADB_PIN_XMIT 12
#define ADB_RX_DRAIN_MAX 32u

static PIO adb_pio = pio1;
static uint adb_sm_rx = 0;
static uint adb_sm_tx = 0;
static uint adb_offset_rx = 0;
static uint adb_offset_tx = 0;

static volatile uint32_t adb_rx_activity = 0;

void adb_bus_init(void) {
    gpio_init(ADB_PIN_RECV);
    gpio_set_dir(ADB_PIN_RECV, GPIO_IN);
    gpio_pull_up(ADB_PIN_RECV);

    gpio_init(ADB_PIN_XMIT);
    gpio_set_dir(ADB_PIN_XMIT, GPIO_OUT);
    gpio_put(ADB_PIN_XMIT, 0); // release bus (inverted open-collector)

    pio_gpio_init(adb_pio, ADB_PIN_XMIT);
    pio_gpio_init(adb_pio, ADB_PIN_RECV);

    adb_offset_rx = pio_add_program(adb_pio, &adb_bus_rx_program);
    adb_offset_tx = pio_add_program(adb_pio, &adb_bus_tx_program);

    adb_sm_rx = pio_claim_unused_sm(adb_pio, true);
    adb_sm_tx = pio_claim_unused_sm(adb_pio, true);

    pio_sm_config rx_cfg;
    adb_bus_rx_pio_config(&rx_cfg, adb_offset_rx, ADB_PIN_RECV);
    pio_sm_init(adb_pio, adb_sm_rx, adb_offset_rx, &rx_cfg);
    pio_sm_put(adb_pio, adb_sm_rx, 110); // Tlt/bit timeout countdown
    pio_sm_set_enabled(adb_pio, adb_sm_rx, true);

    pio_sm_config tx_cfg;
    adb_bus_tx_pio_config(&tx_cfg, adb_offset_tx, ADB_PIN_XMIT, ADB_PIN_RECV);
    pio_sm_init(adb_pio, adb_sm_tx, adb_offset_tx, &tx_cfg);
    pio_sm_set_consecutive_pindirs(adb_pio, adb_sm_tx, ADB_PIN_XMIT, 1, true);
    pio_sm_set_enabled(adb_pio, adb_sm_tx, false);
}

bool adb_bus_service(void) {
    bool did_work = false;
    uint32_t drained = 0;

    while (!pio_sm_is_rx_fifo_empty(adb_pio, adb_sm_rx) && drained < ADB_RX_DRAIN_MAX) {
        (void)pio_sm_get(adb_pio, adb_sm_rx);
        did_work = true;
        drained++;
    }

    if (!pio_sm_is_rx_fifo_empty(adb_pio, adb_sm_rx)) {
        pio_sm_clear_fifos(adb_pio, adb_sm_rx);
        pio_sm_restart(adb_pio, adb_sm_rx);
        pio_sm_put(adb_pio, adb_sm_rx, 110);
    }

    if (did_work) {
        __atomic_store_n(&adb_rx_activity, 1u, __ATOMIC_RELEASE);
    }

    pio_interrupt_clear(adb_pio, adb_sm_rx);
    return did_work;
}

bool adb_bus_take_activity(void) {
    return __atomic_exchange_n(&adb_rx_activity, 0u, __ATOMIC_ACQ_REL) != 0u;
}
