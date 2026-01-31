#include "core_bridge.h"

#include "pico/multicore.h"

#define ADB_EVENT_QUEUE_DEPTH 64

static adb_event_t adb_events[ADB_EVENT_QUEUE_DEPTH];
static volatile uint16_t adb_w = 0;
static volatile uint16_t adb_r = 0;

static inline uint16_t adb_next(uint16_t idx) {
    return (uint16_t)((idx + 1u) % ADB_EVENT_QUEUE_DEPTH);
}

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

void core_bridge_adb_reset(void) {
    __atomic_store_n(&adb_w, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&adb_r, 0, __ATOMIC_RELEASE);
}

bool core_bridge_adb_push(const adb_event_t *event) {
    uint16_t w = __atomic_load_n(&adb_w, __ATOMIC_RELAXED);
    uint16_t next = adb_next(w);
    if (next == __atomic_load_n(&adb_r, __ATOMIC_ACQUIRE)) {
        return false;
    }
    adb_events[w] = *event;
    __atomic_store_n(&adb_w, next, __ATOMIC_RELEASE);
    return true;
}

bool core_bridge_adb_pop(adb_event_t *event) {
    uint16_t r = __atomic_load_n(&adb_r, __ATOMIC_RELAXED);
    if (r == __atomic_load_n(&adb_w, __ATOMIC_ACQUIRE)) {
        return false;
    }
    *event = adb_events[r];
    __atomic_store_n(&adb_r, adb_next(r), __ATOMIC_RELEASE);
    return true;
}
