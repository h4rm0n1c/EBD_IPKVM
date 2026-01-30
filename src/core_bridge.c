#include "core_bridge.h"

#include "pico/multicore.h"

uint32_t core_bridge_pack(core_bridge_cmd_t code, uint16_t param) {
    return (((uint32_t)code) << 16) | ((uint32_t)param & 0xFFFFu);
}

core_bridge_cmd_t core_bridge_unpack_code(uint32_t packed) {
    return (core_bridge_cmd_t)((packed >> 16) & 0xFFFFu);
}

uint16_t core_bridge_unpack_param(uint32_t packed) {
    return (uint16_t)(packed & 0xFFFFu);
}

void core_bridge_send(core_bridge_cmd_t code, uint16_t param) {
    multicore_fifo_push_blocking(core_bridge_pack(code, param));
}

bool core_bridge_try_pop(uint32_t *out_cmd) {
    if (!multicore_fifo_rvalid()) {
        return false;
    }
    *out_cmd = multicore_fifo_pop_blocking();
    return true;
}
