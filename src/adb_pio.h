#pragma once

#include "hardware/pio.h"

void adb_pio_init(PIO pio, uint sm, uint offset, uint pin_adb);
