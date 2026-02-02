#include "adb_bus.h"

#include <stddef.h>

#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "pico/stdlib.h"

#include "adb_bus.pio.h"
#include "adb_core.h"
#include "adb_queue.h"

#define ADB_PIN_RECV 6
#define ADB_PIN_XMIT 12
#define ADB_RX_DRAIN_MAX 64u
#define ADB_RX_TIMEOUT_IRQ 0u

#define ADB_RX_COUNTDOWN 110u
#define ADB_BIT_THRESH_US 50u
#define ADB_RESET_THRESH_US 400u
#define ADB_SRQ_PULSE_US 300u
#define ADB_SRQ_HOLDOFF_US 250u
#define ADB_TX_BYTE_US 1100u

#define ADB_MAX_REG_BYTES 8u
#define ADB_KBD_QUEUE_DEPTH 16u

typedef enum {
    ADB_PHASE_IDLE = 0,
    ADB_PHASE_ATTENTION,
    ADB_PHASE_COMMAND,
    ADB_PHASE_LISTEN,
    ADB_PHASE_TALK,
    ADB_PHASE_SRQ,
} adb_phase_t;

typedef enum {
    ADB_CMD_TALK = 0,
    ADB_CMD_LISTEN = 1,
    ADB_CMD_FLUSH = 2,
    ADB_CMD_RESET = 3,
} adb_cmd_t;

typedef struct {
    uint8_t data[ADB_MAX_REG_BYTES];
    uint8_t len;
    bool keep;
} adb_register_t;

typedef struct {
    uint8_t address;
    uint8_t handler_id;
    bool srq_enabled;
    bool collision;
    bool srq_pending;
    adb_register_t regs[4];
} adb_device_t;

typedef struct {
    uint8_t data[ADB_KBD_QUEUE_DEPTH];
    uint8_t head;
    uint8_t tail;
    uint8_t count;
} adb_key_queue_t;

typedef struct {
    adb_phase_t phase;
    uint8_t current_cmd;
    adb_cmd_t current_type;
    uint8_t current_reg;
    adb_device_t *current_dev;
    absolute_time_t attention_start;
    bool attention_active;

    uint8_t rx_bytes[ADB_MAX_REG_BYTES];
    uint8_t rx_count;
    uint8_t rx_bits;
    uint8_t rx_byte;
    bool rx_expect_start;
    bool rx_have_low;
    uint8_t rx_low_count;

    bool tx_active;
    absolute_time_t tx_end;

    bool srq_active;
    absolute_time_t srq_end;
    absolute_time_t srq_next_allowed;

    adb_key_queue_t key_queue;
    int16_t mouse_dx;
    int16_t mouse_dy;
    uint8_t mouse_buttons;
} adb_bus_state_t;

static PIO adb_pio = pio1;
static uint adb_sm_rx = 0;
static uint adb_sm_tx = 0;
static uint adb_offset_rx = 0;
static uint adb_offset_tx = 0;
static bool adb_rx_enabled = false;

static volatile uint32_t adb_rx_activity = 0;

static adb_device_t adb_devices[2];
static adb_bus_state_t adb_state;

static inline uint32_t adb_low_duration_us(uint8_t low_count) {
    uint32_t ticks = (uint32_t)(ADB_RX_COUNTDOWN - low_count);
    return (ticks * 4u + 5u) / 10u;
}

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

static void adb_device_reset(adb_device_t *dev, uint8_t address, uint8_t handler_id) {
    dev->address = address;
    dev->handler_id = handler_id;
    dev->srq_enabled = true;
    dev->collision = false;
    dev->srq_pending = false;
    for (size_t i = 0; i < 4; i++) {
        dev->regs[i].len = 0;
        dev->regs[i].keep = false;
    }
}

static void adb_bus_reset_devices(void) {
    adb_device_reset(&adb_devices[0], 2, 2);
    adb_device_reset(&adb_devices[1], 3, 1);
    adb_state.key_queue.head = 0;
    adb_state.key_queue.tail = 0;
    adb_state.key_queue.count = 0;
    adb_state.mouse_dx = 0;
    adb_state.mouse_dy = 0;
    adb_state.mouse_buttons = 0;
}

static adb_device_t *adb_bus_find_device(uint8_t address) {
    for (size_t i = 0; i < 2; i++) {
        if (adb_devices[i].address == address) {
            return &adb_devices[i];
        }
    }
    return NULL;
}

static void adb_bus_rx_reset(void) {
    adb_state.rx_count = 0;
    adb_state.rx_bits = 0;
    adb_state.rx_byte = 0;
    adb_state.rx_expect_start = true;
    adb_state.rx_have_low = false;
}

static void adb_bus_queue_keyboard_data(void) {
    adb_device_t *kbd = &adb_devices[0];
    if (kbd->regs[0].len != 0) {
        return;
    }
    uint8_t first = 0xFF;
    uint8_t second = 0xFF;
    if (!adb_key_queue_pop(&adb_state.key_queue, &first)) {
        return;
    }
    (void)adb_key_queue_pop(&adb_state.key_queue, &second);
    kbd->regs[0].data[0] = first;
    kbd->regs[0].data[1] = second;
    kbd->regs[0].len = 2;
    kbd->regs[0].keep = false;
    kbd->srq_pending = true;
}

static void adb_bus_queue_mouse_data(void) {
    adb_device_t *mouse = &adb_devices[1];
    if (mouse->regs[0].len != 0) {
        return;
    }
    int16_t dx = adb_state.mouse_dx;
    int16_t dy = adb_state.mouse_dy;
    if (dx == 0 && dy == 0) {
        return;
    }
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
    uint8_t buttons = (adb_state.mouse_buttons & 0x01u) ? 0x80u : 0x00u;
    mouse->regs[0].data[0] = (uint8_t)buttons | ((uint8_t)dx & 0x7Fu);
    mouse->regs[0].data[1] = (uint8_t)dy;
    mouse->regs[0].len = 2;
    mouse->regs[0].keep = false;
    mouse->srq_pending = true;
    adb_state.mouse_dx = 0;
    adb_state.mouse_dy = 0;
}

static void adb_bus_drain_events(void) {
    adb_event_t event;

    while (adb_queue_pop(&event)) {
        adb_core_record_event(event.type);
        switch (event.type) {
        case ADB_EVENT_KEY: {
            uint8_t code = event.data.key.code;
            if (!event.data.key.down) {
                code |= 0x80u;
            }
            (void)adb_key_queue_push(&adb_state.key_queue, code);
            break;
        }
        case ADB_EVENT_MOUSE:
            adb_state.mouse_dx += event.data.mouse.dx;
            adb_state.mouse_dy += event.data.mouse.dy;
            adb_state.mouse_buttons = event.data.mouse.buttons;
            break;
        default:
            break;
        }
    }

    adb_bus_queue_keyboard_data();
    adb_bus_queue_mouse_data();
}

static void adb_bus_prepare_reg3(adb_device_t *dev) {
    dev->regs[3].data[0] = (uint8_t)((dev->srq_enabled ? 0x20u : 0x00u) | 0x40u | (dev->address & 0x0Fu));
    dev->regs[3].data[1] = dev->handler_id;
    dev->regs[3].len = 2;
    dev->regs[3].keep = false;
}

static void adb_bus_start_tx(const uint8_t *data, uint8_t len) {
    if (adb_rx_enabled) {
        pio_sm_set_enabled(adb_pio, adb_sm_rx, false);
        adb_rx_enabled = false;
    }
    pio_sm_set_enabled(adb_pio, adb_sm_tx, false);
    pio_sm_clear_fifos(adb_pio, adb_sm_tx);
    pio_sm_restart(adb_pio, adb_sm_tx);

    for (uint8_t i = 0; i < len; i++) {
        adb_bus_tx_put(adb_pio, adb_sm_tx, data[i]);
    }

    pio_sm_set_enabled(adb_pio, adb_sm_tx, true);
    adb_state.tx_active = true;
    adb_state.tx_end = delayed_by_us(get_absolute_time(), (uint64_t)len * ADB_TX_BYTE_US);
}

static void adb_bus_stop_tx(void) {
    pio_sm_set_enabled(adb_pio, adb_sm_tx, false);
    adb_state.tx_active = false;
}

static void adb_bus_start_srq(void) {
    if (adb_rx_enabled) {
        pio_sm_set_enabled(adb_pio, adb_sm_rx, false);
        adb_rx_enabled = false;
    }
    adb_state.srq_active = true;
    adb_state.srq_end = delayed_by_us(get_absolute_time(), ADB_SRQ_PULSE_US);
    pio_sm_set_pins(adb_pio, adb_sm_tx, 1u);
}

static void adb_bus_stop_srq(void) {
    adb_state.srq_active = false;
    pio_sm_set_pins(adb_pio, adb_sm_tx, 0u);
    adb_state.srq_next_allowed = delayed_by_us(get_absolute_time(), ADB_SRQ_HOLDOFF_US);
}

static void adb_bus_process_command(uint8_t command) {
    adb_state.current_cmd = command;
    adb_state.current_type = (adb_cmd_t)((command >> 2) & 0x03u);
    adb_state.current_reg = command & 0x03u;
    adb_state.current_dev = adb_bus_find_device((command >> 4) & 0x0Fu);

    if (adb_state.current_type == ADB_CMD_RESET) {
        adb_bus_reset_devices();
        adb_state.phase = ADB_PHASE_IDLE;
        return;
    }

    if (!adb_state.current_dev) {
        adb_state.phase = ADB_PHASE_IDLE;
        return;
    }

    switch (adb_state.current_type) {
    case ADB_CMD_TALK:
        if (adb_state.current_reg == 3) {
            adb_bus_prepare_reg3(adb_state.current_dev);
        }
        if (adb_state.current_dev->regs[adb_state.current_reg].len == 0) {
            adb_state.phase = ADB_PHASE_IDLE;
            return;
        }
        adb_bus_start_tx(adb_state.current_dev->regs[adb_state.current_reg].data,
                         adb_state.current_dev->regs[adb_state.current_reg].len);
        adb_state.phase = ADB_PHASE_TALK;
        if (adb_state.current_reg == 0) {
            adb_state.current_dev->srq_pending = false;
        }
        break;
    case ADB_CMD_LISTEN:
        adb_state.phase = ADB_PHASE_LISTEN;
        adb_bus_rx_reset();
        break;
    case ADB_CMD_FLUSH:
        adb_state.current_dev->regs[0].len = 0;
        adb_state.current_dev->srq_pending = false;
        adb_state.phase = ADB_PHASE_IDLE;
        break;
    default:
        adb_state.phase = ADB_PHASE_IDLE;
        break;
    }
}

static void adb_bus_apply_listen(void) {
    adb_device_t *dev = adb_state.current_dev;
    if (!dev) {
        return;
    }
    uint8_t reg = adb_state.current_reg;
    if (adb_state.rx_count == 0) {
        return;
    }

    if (reg == 3 && adb_state.rx_count >= 2) {
        uint8_t up = adb_state.rx_bytes[0];
        uint8_t low = adb_state.rx_bytes[1];
        if (dev->collision && low == 0xFE) {
            return;
        }
        if (low == 0xFD || low == 0xFF) {
            return;
        }
        if (low == 0x00 || low == 0xFE) {
            dev->address = up & 0x0Fu;
            if (low == 0x00) {
                dev->srq_enabled = (up & 0x20u) != 0u;
                if (!dev->srq_enabled) {
                    dev->srq_pending = false;
                }
            }
            dev->collision = false;
        } else {
            dev->handler_id = low;
        }
        return;
    }

    if (adb_state.rx_count > ADB_MAX_REG_BYTES) {
        adb_state.rx_count = ADB_MAX_REG_BYTES;
    }
    dev->regs[reg].len = adb_state.rx_count;
    dev->regs[reg].keep = true;
    for (uint8_t i = 0; i < adb_state.rx_count; i++) {
        dev->regs[reg].data[i] = adb_state.rx_bytes[i];
    }
}

static void adb_bus_rx_push_bit(uint8_t bit) {
    if (adb_state.rx_expect_start) {
        adb_state.rx_expect_start = false;
        adb_state.rx_bits = 0;
        adb_state.rx_byte = 0;
        return;
    }

    adb_state.rx_byte = (uint8_t)((adb_state.rx_byte << 1) | (bit & 0x01u));
    adb_state.rx_bits++;
    if (adb_state.rx_bits >= 8) {
        if (adb_state.rx_count < sizeof(adb_state.rx_bytes)) {
            adb_state.rx_bytes[adb_state.rx_count++] = adb_state.rx_byte;
        }
        adb_state.rx_bits = 0;
        adb_state.rx_byte = 0;
    }
}

static void adb_bus_decode_rx_fifo(void) {
    uint32_t drained = 0;

    while (!pio_sm_is_rx_fifo_empty(adb_pio, adb_sm_rx) && drained < ADB_RX_DRAIN_MAX) {
        uint8_t value = (uint8_t)pio_sm_get(adb_pio, adb_sm_rx);
        drained++;

        if (!adb_state.rx_have_low) {
            adb_state.rx_low_count = value;
            adb_state.rx_have_low = true;
            continue;
        }

        uint32_t low_us = adb_low_duration_us(adb_state.rx_low_count);
        uint32_t high_us = adb_low_duration_us(value);
        adb_state.rx_have_low = false;

        uint8_t bit = (low_us < ADB_BIT_THRESH_US && high_us > low_us) ? 1u : 0u;
        adb_bus_rx_push_bit(bit);
        __atomic_store_n(&adb_rx_activity, 1u, __ATOMIC_RELEASE);
    }

    if (!pio_sm_is_rx_fifo_empty(adb_pio, adb_sm_rx)) {
        pio_sm_clear_fifos(adb_pio, adb_sm_rx);
        pio_sm_restart(adb_pio, adb_sm_rx);
        pio_sm_put(adb_pio, adb_sm_rx, ADB_RX_COUNTDOWN);
    }
}

void adb_bus_init(void) {
    gpio_init(ADB_PIN_RECV);
    gpio_set_dir(ADB_PIN_RECV, GPIO_IN);
    gpio_pull_up(ADB_PIN_RECV);

    gpio_init(ADB_PIN_XMIT);
    gpio_set_dir(ADB_PIN_XMIT, GPIO_OUT);
    gpio_put(ADB_PIN_XMIT, 0); // release bus (inverted open-collector)

    pio_gpio_init(adb_pio, ADB_PIN_XMIT);
    pio_gpio_init(adb_pio, ADB_PIN_RECV);

    adb_offset_rx = pio_add_program(adb_pio, &adb_bus_rx_program);
    adb_offset_tx = pio_add_program(adb_pio, &adb_bus_tx_program);

    adb_sm_rx = pio_claim_unused_sm(adb_pio, true);
    adb_sm_tx = pio_claim_unused_sm(adb_pio, true);

    pio_sm_config rx_cfg;
    adb_bus_rx_pio_config(&rx_cfg, adb_offset_rx, ADB_PIN_RECV);
    pio_sm_init(adb_pio, adb_sm_rx, adb_offset_rx, &rx_cfg);
    pio_sm_put(adb_pio, adb_sm_rx, ADB_RX_COUNTDOWN);
    pio_sm_set_enabled(adb_pio, adb_sm_rx, true);
    adb_rx_enabled = true;

    pio_sm_config tx_cfg;
    adb_bus_tx_pio_config(&tx_cfg, adb_offset_tx, ADB_PIN_XMIT, ADB_PIN_RECV);
    pio_sm_init(adb_pio, adb_sm_tx, adb_offset_tx, &tx_cfg);
    pio_sm_set_consecutive_pindirs(adb_pio, adb_sm_tx, ADB_PIN_XMIT, 1, true);
    pio_sm_set_enabled(adb_pio, adb_sm_tx, false);
    pio_sm_set_pins(adb_pio, adb_sm_tx, 0u);

    adb_state.phase = ADB_PHASE_IDLE;
    adb_state.attention_active = false;
    adb_state.tx_active = false;
    adb_state.srq_active = false;
    adb_state.srq_next_allowed = get_absolute_time();
    adb_bus_reset_devices();
    adb_bus_rx_reset();
}

bool adb_bus_service(void) {
    bool did_work = false;
    bool line_high = gpio_get(ADB_PIN_RECV);

    if (!adb_rx_enabled && line_high && !adb_state.tx_active && !adb_state.srq_active) {
        pio_sm_restart(adb_pio, adb_sm_rx);
        pio_sm_put(adb_pio, adb_sm_rx, ADB_RX_COUNTDOWN);
        pio_sm_set_enabled(adb_pio, adb_sm_rx, true);
        adb_rx_enabled = true;
    }

    if (!line_high && !adb_state.attention_active && adb_state.phase == ADB_PHASE_IDLE) {
        adb_state.attention_active = true;
        adb_state.attention_start = get_absolute_time();
        adb_state.phase = ADB_PHASE_ATTENTION;
    }

    if (adb_state.attention_active && line_high && adb_state.phase == ADB_PHASE_ATTENTION) {
        adb_state.attention_active = false;
        if (absolute_time_diff_us(get_absolute_time(), adb_state.attention_start) >= ADB_RESET_THRESH_US) {
            adb_bus_reset_devices();
        }
        adb_state.phase = ADB_PHASE_COMMAND;
        adb_bus_rx_reset();
        did_work = true;
    }

    if (adb_state.srq_active) {
        if (absolute_time_diff_us(get_absolute_time(), adb_state.srq_end) <= 0) {
            adb_bus_stop_srq();
            did_work = true;
        }
    }

    if (adb_state.tx_active) {
        if (absolute_time_diff_us(get_absolute_time(), adb_state.tx_end) <= 0) {
            adb_bus_stop_tx();
            if (adb_state.current_type == ADB_CMD_TALK && adb_state.current_dev) {
                if (adb_state.current_reg < 4 && !adb_state.current_dev->regs[adb_state.current_reg].keep) {
                    adb_state.current_dev->regs[adb_state.current_reg].len = 0;
                }
                if (adb_state.current_reg == 0) {
                    adb_bus_queue_keyboard_data();
                    adb_bus_queue_mouse_data();
                }
            }
            adb_state.phase = ADB_PHASE_IDLE;
            did_work = true;
        }
        return did_work;
    }

    adb_bus_drain_events();

    if (!adb_state.srq_active && absolute_time_diff_us(get_absolute_time(), adb_state.srq_next_allowed) <= 0) {
        bool srq_needed = false;
        for (size_t i = 0; i < 2; i++) {
            if (adb_devices[i].srq_enabled && adb_devices[i].srq_pending) {
                srq_needed = true;
                break;
            }
        }
        if (srq_needed && line_high && adb_state.phase == ADB_PHASE_IDLE) {
            adb_bus_start_srq();
            adb_state.phase = ADB_PHASE_SRQ;
            did_work = true;
        }
    }

    if (adb_rx_enabled) {
        adb_bus_decode_rx_fifo();
    }

    if (pio_interrupt_get(adb_pio, ADB_RX_TIMEOUT_IRQ)) {
        pio_interrupt_clear(adb_pio, ADB_RX_TIMEOUT_IRQ);

        if (adb_state.phase == ADB_PHASE_COMMAND && adb_state.rx_count > 0) {
            adb_bus_process_command(adb_state.rx_bytes[0]);
            adb_bus_rx_reset();
            did_work = true;
        } else if (adb_state.phase == ADB_PHASE_LISTEN) {
            adb_bus_apply_listen();
            adb_state.phase = ADB_PHASE_IDLE;
            adb_bus_rx_reset();
            did_work = true;
        } else {
            adb_state.phase = ADB_PHASE_IDLE;
            adb_bus_rx_reset();
        }
    }

    return did_work;
}

bool adb_bus_take_activity(void) {
    return __atomic_exchange_n(&adb_rx_activity, 0u, __ATOMIC_ACQ_REL) != 0u;
}
