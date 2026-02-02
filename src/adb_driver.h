#ifndef ADB_DRIVER_H
#define ADB_DRIVER_H

#include <stdbool.h>
#include <stdint.h>

#include "adb_queue.h"

struct adb_device;

typedef struct {
    uint8_t data[16];
    uint8_t head;
    uint8_t tail;
    uint8_t count;
} adb_key_queue_t;

typedef struct {
    uint8_t data[16][2];
    uint8_t head;
    uint8_t tail;
    uint8_t count;
} adb_kbd_queue_t;

typedef struct {
    uint8_t data[16][2];
    uint8_t head;
    uint8_t tail;
    uint8_t count;
} adb_mouse_queue_t;

typedef struct {
    adb_key_queue_t key_queue;
    adb_kbd_queue_t kbd_queue;
    adb_mouse_queue_t mouse_queue;
    int16_t mouse_dx;
    int16_t mouse_dy;
    uint8_t mouse_buttons;
} adb_driver_state_t;

void adb_driver_init(adb_driver_state_t *state);
void adb_driver_reset(adb_driver_state_t *state);
void adb_driver_handle_event(adb_driver_state_t *state, const adb_event_t *event, uint32_t *lock_fail_counter);
void adb_driver_flush(adb_driver_state_t *state, uint32_t *lock_fail_counter);
void adb_driver_bind_callbacks(adb_driver_state_t *state);
bool adb_driver_kbd_reg0_pop(struct adb_device *dev, uint8_t *first, uint8_t *second);
bool adb_driver_mouse_reg0_pop(struct adb_device *dev, uint8_t *first, uint8_t *second);
uint8_t adb_driver_handler_id(uint8_t address, uint8_t stored_id);

#endif
