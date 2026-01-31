#include "adb_events.h"

#include "hardware/sync.h"

#define ADB_EVENT_QUEUE_DEPTH 128
#define ADB_EVENT_QUEUE_MASK (ADB_EVENT_QUEUE_DEPTH - 1)

static adb_event_t adb_queue[ADB_EVENT_QUEUE_DEPTH];
static uint16_t adb_queue_w = 0;
static uint16_t adb_queue_r = 0;
static uint32_t adb_drop_count = 0;
static spin_lock_t *adb_lock = NULL;

void adb_events_init(void) {
    adb_queue_w = 0;
    adb_queue_r = 0;
    adb_drop_count = 0;
    if (!adb_lock) {
        adb_lock = spin_lock_init(spin_lock_claim_unused(true));
    }
}

bool adb_events_push(const adb_event_t *event) {
    if (!adb_lock || !event) {
        return false;
    }
    uint32_t status = spin_lock_blocking(adb_lock);
    uint16_t next = (uint16_t)((adb_queue_w + 1) & ADB_EVENT_QUEUE_MASK);
    if (next == adb_queue_r) {
        adb_drop_count++;
        spin_unlock(adb_lock, status);
        return false;
    }
    adb_queue[adb_queue_w] = *event;
    adb_queue_w = next;
    spin_unlock(adb_lock, status);
    return true;
}

bool adb_events_pop(adb_event_t *out_event) {
    if (!adb_lock || !out_event) {
        return false;
    }
    uint32_t status = spin_lock_blocking(adb_lock);
    if (adb_queue_r == adb_queue_w) {
        spin_unlock(adb_lock, status);
        return false;
    }
    *out_event = adb_queue[adb_queue_r];
    adb_queue_r = (uint16_t)((adb_queue_r + 1) & ADB_EVENT_QUEUE_MASK);
    spin_unlock(adb_lock, status);
    return true;
}

uint16_t adb_events_pending(void) {
    if (!adb_lock) {
        return 0;
    }
    uint32_t status = spin_lock_blocking(adb_lock);
    uint16_t pending = (uint16_t)((adb_queue_w - adb_queue_r) & ADB_EVENT_QUEUE_MASK);
    spin_unlock(adb_lock, status);
    return pending;
}

uint32_t adb_events_get_drop_count(void) {
    return __atomic_load_n(&adb_drop_count, __ATOMIC_ACQUIRE);
}
