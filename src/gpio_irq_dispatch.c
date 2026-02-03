#include "gpio_irq_dispatch.h"

#include <stddef.h>

#include "hardware/gpio.h"
#include "hardware/irq.h"

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
    for (size_t i = 0; i < GPIO_IRQ_DISPATCH_MAX; i++) {
        gpio_irq_dispatch_entry_t *entry = &gpio_irq_entries[i];
        if (!entry->active) {
            continue;
        }
        uint gpio = entry->gpio;
        uint32_t events = gpio_get_irq_event_mask(gpio);
        if (!events) {
            continue;
        }
        gpio_acknowledge_irq(gpio, events);
        if (!entry->fn) {
            continue;
        }
        uint32_t masked = events & entry->mask;
        if (masked) {
            entry->fn(gpio, masked, entry->ctx);
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
