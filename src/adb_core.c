#include "adb_core.h"

#include "adb_queue.h"

static volatile uint32_t adb_key_events = 0;
static volatile uint32_t adb_mouse_events = 0;
static volatile uint32_t adb_drops = 0;
static volatile uint32_t adb_pending = 0;

void adb_core_init(void) {
    __atomic_store_n(&adb_key_events, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&adb_mouse_events, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&adb_drops, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&adb_pending, 0, __ATOMIC_RELEASE);
}

bool adb_core_service(void) {
    bool did_work = false;
    adb_event_t event;

    while (adb_queue_pop(&event)) {
        did_work = true;
        switch (event.type) {
        case ADB_EVENT_KEY:
            __atomic_fetch_add(&adb_key_events, 1u, __ATOMIC_RELAXED);
            break;
        case ADB_EVENT_MOUSE:
            __atomic_fetch_add(&adb_mouse_events, 1u, __ATOMIC_RELAXED);
            break;
        default:
            break;
        }
    }

    uint32_t drops = adb_queue_take_drops();
    if (drops) {
        __atomic_fetch_add(&adb_drops, drops, __ATOMIC_RELAXED);
        did_work = true;
    }

    __atomic_store_n(&adb_pending, adb_queue_count(), __ATOMIC_RELEASE);
    return did_work;
}

void adb_core_get_stats(adb_core_stats_t *out) {
    if (!out) {
        return;
    }
    out->key_events = __atomic_load_n(&adb_key_events, __ATOMIC_ACQUIRE);
    out->mouse_events = __atomic_load_n(&adb_mouse_events, __ATOMIC_ACQUIRE);
    out->drops = __atomic_load_n(&adb_drops, __ATOMIC_ACQUIRE);
    out->pending = __atomic_load_n(&adb_pending, __ATOMIC_ACQUIRE);
}
