#include "pico/stdlib.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/pio.h"

#include "app_core.h"
#include "classic_line.pio.h"
#include "video_core.h"

#define PIN_PIXCLK 0
#define PIN_VSYNC  1   // active-low
#define PIN_HSYNC  2   // active-low
#define PIN_VIDEO  3
#define PIN_PS_ON  9   // via ULN2803, GPIO high asserts ATX PS_ON

int main(void) {
    stdio_init_all();
    sleep_ms(1200);

    // SIO GPIO inputs + pulls (sane when Mac is off)
    gpio_init(PIN_PIXCLK); gpio_set_dir(PIN_PIXCLK, GPIO_IN); gpio_disable_pulls(PIN_PIXCLK);
    gpio_init(PIN_VIDEO);  gpio_set_dir(PIN_VIDEO,  GPIO_IN); gpio_disable_pulls(PIN_VIDEO);
    gpio_init(PIN_HSYNC);  gpio_set_dir(PIN_HSYNC,  GPIO_IN); gpio_disable_pulls(PIN_HSYNC);

    // VSYNC must remain SIO GPIO for IRQ to work
    gpio_init(PIN_VSYNC);  gpio_set_dir(PIN_VSYNC,  GPIO_IN); gpio_disable_pulls(PIN_VSYNC);
    gpio_init(PIN_PS_ON);  gpio_set_dir(PIN_PS_ON, GPIO_OUT); gpio_put(PIN_PS_ON, 0);

    // Clear any stale IRQ state, core1 will enable callback
    gpio_acknowledge_irq(PIN_VSYNC, GPIO_IRQ_EDGE_FALL | GPIO_IRQ_EDGE_RISE);

    // Hand ONLY the pins PIO needs to PIO0
    gpio_set_function(PIN_PIXCLK, GPIO_FUNC_PIO0);
    gpio_set_function(PIN_HSYNC,  GPIO_FUNC_PIO0);
    gpio_set_function(PIN_VIDEO,  GPIO_FUNC_PIO0);

    PIO pio = pio0;
    uint sm = 0;

    pio_sm_set_consecutive_pindirs(pio, sm, PIN_PIXCLK, 1, false);
    pio_sm_set_consecutive_pindirs(pio, sm, PIN_HSYNC,  1, false);
    pio_sm_set_consecutive_pindirs(pio, sm, PIN_VIDEO,  1, false);

    uint offset_fall_pixrise = pio_add_program(pio, &classic_line_fall_pixrise_program);

    int dma_chan = dma_claim_unused_channel(true);
    irq_set_priority(USBCTRL_IRQ, 1);

    video_core_config_t video_cfg = {
        .pio = pio,
        .sm = sm,
        .dma_chan = dma_chan,
        .offset_fall_pixrise = offset_fall_pixrise,
        .pin_video = PIN_VIDEO,
        .pin_vsync = PIN_VSYNC,
    };
    video_core_init(&video_cfg);
    video_core_launch();

    app_core_config_t app_cfg = {
        .pin_pixclk = PIN_PIXCLK,
        .pin_vsync = PIN_VSYNC,
        .pin_hsync = PIN_HSYNC,
        .pin_video = PIN_VIDEO,
        .pin_ps_on = PIN_PS_ON,
    };
    app_core_init(&app_cfg);

    while (true) {
        app_core_poll();
    }
}
