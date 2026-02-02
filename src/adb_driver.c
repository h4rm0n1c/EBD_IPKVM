#include "adb_driver.h"

#include <stddef.h>

#include "adb_bus.h"

#define ADB_KBD_QUEUE_DEPTH 16u
#define ADB_MOUSE_QUEUE_DEPTH 16u

static bool adb_key_queue_push(adb_key_queue_t *queue, uint8_t code) {
    if (queue->count >= ADB_KBD_QUEUE_DEPTH) {
        return false;
    }
    queue->data[queue->head] = code;
    queue->head = (uint8_t)((queue->head + 1u) % ADB_KBD_QUEUE_DEPTH);
    queue->count++;
    return true;
}

static bool adb_key_queue_pop(adb_key_queue_t *queue, uint8_t *code) {
    if (!queue->count) {
        return false;
    }
    *code = queue->data[queue->tail];
    queue->tail = (uint8_t)((queue->tail + 1u) % ADB_KBD_QUEUE_DEPTH);
    queue->count--;
    return true;
}

static bool adb_kbd_queue_push(adb_kbd_queue_t *queue, uint8_t first, uint8_t second) {
    if (queue->count >= ADB_KBD_QUEUE_DEPTH) {
        return false;
    }
    queue->data[queue->head][0] = first;
    queue->data[queue->head][1] = second;
    queue->head = (uint8_t)((queue->head + 1u) % ADB_KBD_QUEUE_DEPTH);
    queue->count++;
    return true;
}

static bool adb_kbd_queue_pop(adb_kbd_queue_t *queue, uint8_t *first, uint8_t *second) {
    if (!queue->count) {
        return false;
    }
    *first = queue->data[queue->tail][0];
    *second = queue->data[queue->tail][1];
    queue->tail = (uint8_t)((queue->tail + 1u) % ADB_KBD_QUEUE_DEPTH);
    queue->count--;
    return true;
}

static bool adb_mouse_queue_push(adb_mouse_queue_t *queue, uint8_t first, uint8_t second) {
    if (queue->count >= ADB_MOUSE_QUEUE_DEPTH) {
        return false;
    }
    queue->data[queue->head][0] = first;
    queue->data[queue->head][1] = second;
    queue->head = (uint8_t)((queue->head + 1u) % ADB_MOUSE_QUEUE_DEPTH);
    queue->count++;
    return true;
}

static bool adb_mouse_queue_pop(adb_mouse_queue_t *queue, uint8_t *first, uint8_t *second) {
    if (!queue->count) {
        return false;
    }
    *first = queue->data[queue->tail][0];
    *second = queue->data[queue->tail][1];
    queue->tail = (uint8_t)((queue->tail + 1u) % ADB_MOUSE_QUEUE_DEPTH);
    queue->count--;
    return true;
}

void adb_driver_init(adb_driver_state_t *state) {
    if (!state) {
        return;
    }
    adb_driver_reset(state);
    adb_driver_bind_callbacks(state);
}

void adb_driver_reset(adb_driver_state_t *state) {
    if (!state) {
        return;
    }
    state->key_queue.head = 0;
    state->key_queue.tail = 0;
    state->key_queue.count = 0;
    state->kbd_queue.head = 0;
    state->kbd_queue.tail = 0;
    state->kbd_queue.count = 0;
    state->mouse_queue.head = 0;
    state->mouse_queue.tail = 0;
    state->mouse_queue.count = 0;
    state->mouse_dx = 0;
    state->mouse_dy = 0;
    state->mouse_buttons = 0;
}

void adb_driver_handle_event(adb_driver_state_t *state, const adb_event_t *event, uint32_t *lock_fail_counter) {
    if (!state || !event) {
        return;
    }
    switch (event->type) {
    case ADB_EVENT_KEY: {
        uint8_t code = event->data.key.code;
        if (!event->data.key.down) {
            code |= 0x80u;
        }
        if (!adb_key_queue_push(&state->key_queue, code) && lock_fail_counter) {
            (*lock_fail_counter)++;
        }
        break;
    }
    case ADB_EVENT_MOUSE:
        state->mouse_dx += event->data.mouse.dx;
        state->mouse_dy += event->data.mouse.dy;
        state->mouse_buttons = event->data.mouse.buttons;
        break;
    default:
        break;
    }
}

void adb_driver_flush(adb_driver_state_t *state, uint32_t *lock_fail_counter) {
    if (!state) {
        return;
    }
    if (state->key_queue.count != 0) {
        uint8_t first = 0xFFu;
        uint8_t second = 0xFFu;
        if (adb_key_queue_pop(&state->key_queue, &first)) {
            (void)adb_key_queue_pop(&state->key_queue, &second);
            if (!adb_kbd_queue_push(&state->kbd_queue, first, second) && lock_fail_counter) {
                (*lock_fail_counter)++;
            }
        }
    }

    if (state->mouse_dx != 0 || state->mouse_dy != 0) {
        int16_t dx = state->mouse_dx;
        int16_t dy = state->mouse_dy;
        if (dx > 127) {
            dx = 127;
        } else if (dx < -127) {
            dx = -127;
        }
        if (dy > 127) {
            dy = 127;
        } else if (dy < -127) {
            dy = -127;
        }
        uint8_t buttons = (state->mouse_buttons & 0x01u) ? 0x80u : 0x00u;
        uint8_t first = (uint8_t)buttons | ((uint8_t)dx & 0x7Fu);
        uint8_t second = (uint8_t)dy;
        if (adb_mouse_queue_push(&state->mouse_queue, first, second)) {
            state->mouse_dx = 0;
            state->mouse_dy = 0;
        } else if (lock_fail_counter) {
            (*lock_fail_counter)++;
        }
    }
}

void adb_driver_bind_callbacks(adb_driver_state_t *state) {
    if (!state) {
        return;
    }
    (void)adb_bus_set_handler_id_fn(2u, adb_driver_handler_id);
    (void)adb_bus_set_handler_id_fn(3u, adb_driver_handler_id);
    (void)adb_bus_set_reg0_pop(2u, adb_driver_kbd_reg0_pop, &state->kbd_queue);
    (void)adb_bus_set_reg0_pop(3u, adb_driver_mouse_reg0_pop, &state->mouse_queue);
}

bool adb_driver_kbd_reg0_pop(struct adb_device *dev, uint8_t *first, uint8_t *second) {
    adb_kbd_queue_t *queue = (adb_kbd_queue_t *)adb_bus_get_reg0_ctx(dev);
    if (!queue) {
        return false;
    }
    return adb_kbd_queue_pop(queue, first, second);
}

bool adb_driver_mouse_reg0_pop(struct adb_device *dev, uint8_t *first, uint8_t *second) {
    adb_mouse_queue_t *queue = (adb_mouse_queue_t *)adb_bus_get_reg0_ctx(dev);
    if (!queue) {
        return false;
    }
    return adb_mouse_queue_pop(queue, first, second);
}

uint8_t adb_driver_handler_id(uint8_t address, uint8_t stored_id) {
    (void)address;
    return stored_id;
}
