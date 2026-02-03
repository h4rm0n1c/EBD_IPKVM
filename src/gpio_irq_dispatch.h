#ifndef GPIO_IRQ_DISPATCH_H
#define GPIO_IRQ_DISPATCH_H

#include <stdbool.h>
#include <stdint.h>

typedef void (*gpio_irq_dispatch_fn)(uint gpio, uint32_t events, void *ctx);

void gpio_irq_dispatch_init(void);
bool gpio_irq_dispatch_register(uint gpio, uint32_t event_mask, gpio_irq_dispatch_fn fn, void *ctx);

#endif
