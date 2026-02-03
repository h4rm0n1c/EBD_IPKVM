#pragma once

#include <stdbool.h>
#include <stdint.h>

// Commands sent from core0 to core1 over the multicore FIFO.
typedef enum {
    CORE_BRIDGE_CMD_STOP_CAPTURE = 1,
    CORE_BRIDGE_CMD_RESET_COUNTERS = 2,
    CORE_BRIDGE_CMD_UNUSED = 3,
} core_bridge_cmd_t;

uint32_t core_bridge_pack(core_bridge_cmd_t code, uint16_t param);
core_bridge_cmd_t core_bridge_unpack_code(uint32_t packed);
uint16_t core_bridge_unpack_param(uint32_t packed);

// Push a command to core1 (blocking).
void core_bridge_send(core_bridge_cmd_t code, uint16_t param);

// Non-blocking pop on core1. Returns true when a command was read.
bool core_bridge_try_pop(uint32_t *out_cmd);
