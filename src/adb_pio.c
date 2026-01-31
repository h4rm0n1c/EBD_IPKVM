#include "adb_pio.h"

#include "adb_pio.pio.h"

uint adb_pio_add_program(PIO pio) {
    return pio_add_program(pio, &adb_edge_sampler_program);
}

void adb_pio_init(PIO pio, uint sm, uint offset, uint pin_adb) {
    pio_sm_set_enabled(pio, sm, false);
    pio_sm_clear_fifos(pio, sm);
    pio_sm_restart(pio, sm);

    pio_sm_set_consecutive_pindirs(pio, sm, pin_adb, 1, false);
    pio_sm_config cfg = adb_edge_sampler_program_get_default_config(offset);
    sm_config_set_in_pins(&cfg, pin_adb);
    sm_config_set_jmp_pin(&cfg, pin_adb);
    sm_config_set_clkdiv(&cfg, 1.0f);
    pio_sm_init(pio, sm, offset, &cfg);
    pio_sm_set_enabled(pio, sm, true);
}
