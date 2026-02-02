#pragma once

#include <stdint.h>

#include "pico/stdlib.h"

typedef struct app_core_config {
    uint pin_pixclk;
    uint pin_vsync;
    uint pin_hsync;
    uint pin_video;
    uint pin_ps_on;
} app_core_config_t;

void app_core_init(const app_core_config_t *cfg);
void app_core_poll(void);
