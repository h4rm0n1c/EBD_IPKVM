#pragma once

#include "hardware/pio.h"

uint adb_pio_add_program(PIO pio);
void adb_pio_init(PIO pio, uint sm, uint offset, uint pin_adb);
