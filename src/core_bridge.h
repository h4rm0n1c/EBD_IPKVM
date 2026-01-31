#pragma once

#include <stdbool.h>
#include <stdint.h>

// Commands sent from core0 to core1 over the multicore FIFO.
typedef enum {
    CORE_BRIDGE_CMD_STOP_CAPTURE = 1,
    CORE_BRIDGE_CMD_RESET_COUNTERS = 2,
    CORE_BRIDGE_CMD_SINGLE_FRAME = 3,
    CORE_BRIDGE_CMD_START_TEST = 4,
    CORE_BRIDGE_CMD_CONFIG_VSYNC = 5,
    CORE_BRIDGE_CMD_DIAG_PREP = 6,
    CORE_BRIDGE_CMD_DIAG_DONE = 7,
} core_bridge_cmd_t;

typedef enum {
    ADB_EVENT_KEY = 0,
    ADB_EVENT_MOUSE = 1,
} adb_event_type_t;

typedef struct {
    adb_event_type_t type;
    uint8_t a;
    int8_t b;
    int8_t c;
} adb_event_t;

uint32_t core_bridge_pack(core_bridge_cmd_t code, uint16_t param);
core_bridge_cmd_t core_bridge_unpack_code(uint32_t packed);
uint16_t core_bridge_unpack_param(uint32_t packed);

// Push a command to core1 (blocking).
void core_bridge_send(core_bridge_cmd_t code, uint16_t param);

// Non-blocking pop on core1. Returns true when a command was read.
bool core_bridge_try_pop(uint32_t *out_cmd);

void core_bridge_adb_reset(void);
bool core_bridge_adb_push(const adb_event_t *event);
bool core_bridge_adb_pop(adb_event_t *event);
