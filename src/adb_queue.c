#include "adb_queue.h"

#define ADB_QUEUE_DEPTH 64u
#define ADB_QUEUE_MASK (ADB_QUEUE_DEPTH - 1u)

static adb_event_t queue[ADB_QUEUE_DEPTH];
static volatile uint16_t queue_head = 0;
static volatile uint16_t queue_tail = 0;
static volatile uint32_t queue_drops = 0;

static inline uint16_t load_u16(const volatile uint16_t *value) {
    return __atomic_load_n(value, __ATOMIC_ACQUIRE);
}

static inline void store_u16(volatile uint16_t *value, uint16_t data) {
    __atomic_store_n(value, data, __ATOMIC_RELEASE);
}

void adb_queue_init(void) {
    store_u16(&queue_head, 0);
    store_u16(&queue_tail, 0);
    __atomic_store_n(&queue_drops, 0, __ATOMIC_RELEASE);
}

bool adb_queue_push(const adb_event_t *event) {
    uint16_t head = load_u16(&queue_head);
    uint16_t tail = load_u16(&queue_tail);
    uint16_t next = (uint16_t)((head + 1u) & ADB_QUEUE_MASK);
    if (next == tail) {
        __atomic_fetch_add(&queue_drops, 1u, __ATOMIC_RELAXED);
        return false;
    }
    queue[head] = *event;
    store_u16(&queue_head, next);
    return true;
}

bool adb_queue_pop(adb_event_t *event) {
    uint16_t head = load_u16(&queue_head);
    uint16_t tail = load_u16(&queue_tail);
    if (tail == head) {
        return false;
    }
    *event = queue[tail];
    store_u16(&queue_tail, (uint16_t)((tail + 1u) & ADB_QUEUE_MASK));
    return true;
}

uint16_t adb_queue_count(void) {
    uint16_t head = load_u16(&queue_head);
    uint16_t tail = load_u16(&queue_tail);
    return (uint16_t)((head - tail) & ADB_QUEUE_MASK);
}

uint32_t adb_queue_take_drops(void) {
    return __atomic_exchange_n(&queue_drops, 0, __ATOMIC_ACQ_REL);
}
