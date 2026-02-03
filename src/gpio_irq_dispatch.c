#include "gpio_irq_dispatch.h"

#include <stddef.h>

#include "pico/platform/compiler.h"

#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/structs/io_bank0.h"

#ifndef GPIO_IRQ_DISPATCH_MAX
#define GPIO_IRQ_DISPATCH_MAX 4u
#endif

typedef struct {
    uint gpio;
    uint32_t mask;
    gpio_irq_dispatch_fn fn;
    void *ctx;
    bool active;
} gpio_irq_dispatch_entry_t;

static gpio_irq_dispatch_entry_t gpio_irq_entries[GPIO_IRQ_DISPATCH_MAX];
static bool gpio_irq_installed = false;

static void gpio_irq_bank0_dispatch(void) {
    uint32_t pending[count_of(io_bank0_hw->intr)];
    bool any_pending = false;
    for (uint bank = 0; bank < count_of(io_bank0_hw->intr); bank++) {
        pending[bank] = io_bank0_hw->intr[bank];
        if (pending[bank]) {
            any_pending = true;
        }
    }
    if (!any_pending) {
        return;
    }
    for (uint bank = 0; bank < count_of(io_bank0_hw->intr); bank++) {
        if (pending[bank]) {
            io_bank0_hw->intr[bank] = pending[bank];
        }
    }
    for (size_t i = 0; i < GPIO_IRQ_DISPATCH_MAX; i++) {
        gpio_irq_dispatch_entry_t *entry = &gpio_irq_entries[i];
        if (!entry->active || !entry->fn) {
            continue;
        }
        uint pin = entry->gpio;
        if (pin >= NUM_BANK0_GPIOS) {
            continue;
        }
        uint bank = pin / 8u;
        uint shift = (pin % 8u) * 4u;
        uint32_t events = (pending[bank] >> shift) & 0xFu;
        if (!events) {
            continue;
        }
        uint32_t masked = events & entry->mask;
        if (masked) {
            entry->fn(pin, masked, entry->ctx);
        }
    }
}

void gpio_irq_dispatch_init(void) {
    if (gpio_irq_installed) {
        return;
    }
    irq_set_exclusive_handler(IO_IRQ_BANK0, gpio_irq_bank0_dispatch);
    irq_set_enabled(IO_IRQ_BANK0, true);
    gpio_irq_installed = true;
}

bool gpio_irq_dispatch_register(uint gpio, uint32_t event_mask, gpio_irq_dispatch_fn fn, void *ctx) {
    gpio_irq_dispatch_init();
    for (size_t i = 0; i < GPIO_IRQ_DISPATCH_MAX; i++) {
        gpio_irq_dispatch_entry_t *entry = &gpio_irq_entries[i];
        if (entry->active && entry->gpio == gpio) {
            entry->mask = event_mask;
            entry->fn = fn;
            entry->ctx = ctx;
            return true;
        }
    }
    for (size_t i = 0; i < GPIO_IRQ_DISPATCH_MAX; i++) {
        gpio_irq_dispatch_entry_t *entry = &gpio_irq_entries[i];
        if (!entry->active) {
            entry->gpio = gpio;
            entry->mask = event_mask;
            entry->fn = fn;
            entry->ctx = ctx;
            entry->active = true;
            return true;
        }
    }
    return false;
}
