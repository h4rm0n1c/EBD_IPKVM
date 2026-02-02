#include "adb_bus.h"

#include <stddef.h>

#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "pico/stdlib.h"

#include "adb_bus.pio.h"
#include "adb_core.h"
#include "adb_queue.h"

#define ADB_PIN_RECV 6
#define ADB_PIN_XMIT 12

#define ADB_MAX_REG_BYTES 8u
#define ADB_KBD_QUEUE_DEPTH 16u

#define PIO_ATN_MIN 386u
#define PIO_CMD_OFFSET 2u
#define TIME_RESET_THRESH_US 400u

typedef enum {
    ADB_PHASE_IDLE = 0,
    ADB_PHASE_ATTENTION,
    ADB_PHASE_COMMAND,
    ADB_PHASE_SRQ,
    ADB_PHASE_LISTEN,
    ADB_PHASE_TALK,
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

    uint8_t listen_buf[ADB_MAX_REG_BYTES];

    adb_key_queue_t key_queue;
    int16_t mouse_dx;
    int16_t mouse_dy;
    uint8_t mouse_buttons;
    uint16_t srq_flags;
} adb_bus_state_t;

static PIO adb_pio = pio1;
static uint adb_sm = 0;
static uint adb_offset_atn = 0;
static uint adb_offset_rx = 0;
static uint adb_offset_tx = 0;
static int adb_dma_chan = -1;

static volatile uint32_t adb_rx_activity = 0;
static volatile bool adb_gpio_rise = false;

static adb_device_t adb_devices[2];
static adb_bus_state_t adb_state;

static void adb_gpio_irq_handler(uint gpio, uint32_t events) {
    if (gpio != ADB_PIN_RECV) {
        return;
    }
    if (events & GPIO_IRQ_EDGE_RISE) {
        adb_gpio_rise = true;
    }
}

static void adb_gpio_rise_arm(void) {
    adb_gpio_rise = false;
    gpio_set_irq_enabled(ADB_PIN_RECV, GPIO_IRQ_EDGE_RISE, true);
}

static void adb_gpio_rise_disarm(void) {
    gpio_set_irq_enabled(ADB_PIN_RECV, GPIO_IRQ_EDGE_RISE, false);
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
    adb_state.srq_flags = 0;
}

static adb_device_t *adb_bus_find_device(uint8_t address) {
    for (size_t i = 0; i < 2; i++) {
        if (adb_devices[i].address == address) {
            return &adb_devices[i];
        }
    }
    return NULL;
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
    adb_state.srq_flags |= (uint16_t)(1u << kbd->address);
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
    adb_state.srq_flags |= (uint16_t)(1u << mouse->address);
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

static void adb_pio_stop(void) {
    pio_sm_set_enabled(adb_pio, adb_sm, false);
    pio_sm_clear_fifos(adb_pio, adb_sm);
    pio_interrupt_clear(adb_pio, adb_sm);
    pio_interrupt_clear(adb_pio, adb_sm + 4);
}

static void adb_pio_atn_start(void) {
    pio_sm_config pc;
    bus_atn_dev_pio_config(&pc, adb_offset_atn, ADB_PIN_RECV);
    pio_sm_init(adb_pio, adb_sm, adb_offset_atn, &pc);
    pio_sm_put(adb_pio, adb_sm, PIO_ATN_MIN);
    pio_sm_set_enabled(adb_pio, adb_sm, true);
}

static void adb_pio_command_start(void) {
    pio_sm_config pc;
    bus_rx_dev_pio_config(&pc, adb_offset_rx, ADB_PIN_XMIT, ADB_PIN_RECV);
    pio_sm_init(adb_pio, adb_sm, adb_offset_rx + PIO_CMD_OFFSET, &pc);
    pio_sm_put(adb_pio, adb_sm, 7);
    pio_sm_set_enabled(adb_pio, adb_sm, true);
}

static void adb_pio_tx_start(void) {
    pio_sm_config pc;
    bus_tx_dev_pio_config(&pc, adb_offset_tx, ADB_PIN_XMIT, ADB_PIN_RECV);
    pio_sm_init(adb_pio, adb_sm, adb_offset_tx, &pc);
    pio_sm_set_enabled(adb_pio, adb_sm, true);
}

static void adb_pio_rx_start(void) {
    pio_sm_config pc;
    bus_rx_dev_pio_config(&pc, adb_offset_rx, ADB_PIN_XMIT, ADB_PIN_RECV);
    pio_sm_init(adb_pio, adb_sm, adb_offset_rx, &pc);
    pio_sm_put(adb_pio, adb_sm, 63);

    dma_channel_config dc = dma_channel_get_default_config((uint)adb_dma_chan);
    channel_config_set_transfer_data_size(&dc, DMA_SIZE_8);
    channel_config_set_dreq(&dc, pio_get_dreq(adb_pio, adb_sm, false));
    channel_config_set_read_increment(&dc, false);
    channel_config_set_write_increment(&dc, true);
    dma_channel_configure((uint)adb_dma_chan, &dc,
                          adb_state.listen_buf,
                          &(adb_pio->rxf[adb_sm]),
                          ADB_MAX_REG_BYTES,
                          true);

    pio_sm_set_enabled(adb_pio, adb_sm, true);
}

static void adb_bus_execute_command(void) {
    adb_cmd_t type = adb_state.current_type;
    uint8_t reg = adb_state.current_reg;
    adb_device_t *dev = adb_state.current_dev;

    adb_pio_stop();

    if (type == ADB_CMD_RESET) {
        adb_bus_reset_devices();
        adb_pio_atn_start();
        adb_state.phase = ADB_PHASE_IDLE;
        return;
    }

    if (!dev) {
        adb_pio_atn_start();
        adb_state.phase = ADB_PHASE_IDLE;
        return;
    }

    switch (type) {
    case ADB_CMD_TALK:
        if (reg == 3) {
            adb_bus_prepare_reg3(dev);
        }
        if (dev->regs[reg].len == 0) {
            if (reg == 0) {
                dev->srq_pending = false;
                adb_state.srq_flags &= (uint16_t)~(1u << dev->address);
            }
            adb_pio_atn_start();
            adb_state.phase = ADB_PHASE_IDLE;
            return;
        }
        adb_pio_tx_start();
        bus_tx_dev_putm(adb_pio, adb_sm, dev->regs[reg].data, dev->regs[reg].len);
        adb_state.phase = ADB_PHASE_TALK;
        if (reg == 0) {
            dev->srq_pending = false;
            adb_state.srq_flags &= (uint16_t)~(1u << dev->address);
        }
        break;
    case ADB_CMD_LISTEN:
        adb_state.phase = ADB_PHASE_LISTEN;
        adb_pio_rx_start();
        break;
    case ADB_CMD_FLUSH:
        dev->regs[0].len = 0;
        dev->srq_pending = false;
        adb_state.srq_flags &= (uint16_t)~(1u << dev->address);
        adb_pio_atn_start();
        adb_state.phase = ADB_PHASE_IDLE;
        break;
    default:
        adb_pio_atn_start();
        adb_state.phase = ADB_PHASE_IDLE;
        break;
    }
}

static void adb_bus_apply_listen(uint8_t bytes) {
    adb_device_t *dev = adb_state.current_dev;
    if (!dev || bytes == 0) {
        return;
    }
    uint8_t reg = adb_state.current_reg;
    if (reg == 3 && bytes >= 2) {
        uint8_t up = adb_state.listen_buf[0];
        uint8_t low = adb_state.listen_buf[1];
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

    if (bytes > ADB_MAX_REG_BYTES) {
        bytes = ADB_MAX_REG_BYTES;
    }
    dev->regs[reg].len = bytes;
    dev->regs[reg].keep = true;
    for (uint8_t i = 0; i < bytes; i++) {
        dev->regs[reg].data[i] = adb_state.listen_buf[i];
    }
}

void adb_bus_init(void) {
    gpio_init(ADB_PIN_RECV);
    gpio_set_dir(ADB_PIN_RECV, GPIO_IN);
    gpio_pull_up(ADB_PIN_RECV);
    gpio_set_irq_enabled_with_callback(ADB_PIN_RECV, GPIO_IRQ_EDGE_RISE, false, adb_gpio_irq_handler);

    gpio_init(ADB_PIN_XMIT);
    gpio_set_dir(ADB_PIN_XMIT, GPIO_OUT);
    gpio_put(ADB_PIN_XMIT, 0);

    pio_gpio_init(adb_pio, ADB_PIN_XMIT);
    pio_gpio_init(adb_pio, ADB_PIN_RECV);

    adb_offset_atn = pio_add_program(adb_pio, &bus_atn_dev_program);
    adb_offset_rx = pio_add_program(adb_pio, &bus_rx_dev_program);
    adb_offset_tx = pio_add_program(adb_pio, &bus_tx_dev_program);

    adb_sm = pio_claim_unused_sm(adb_pio, true);
    adb_dma_chan = dma_claim_unused_channel(true);

    adb_bus_reset_devices();
    adb_state.phase = ADB_PHASE_IDLE;
    adb_pio_atn_start();
}

static void adb_bus_handle_command_irq(void) {
    uint8_t cmd = bus_rx_dev_get(adb_pio, adb_sm);
    adb_state.current_cmd = cmd;
    adb_state.current_type = (adb_cmd_t)((cmd >> 2) & 0x03u);
    adb_state.current_reg = cmd & 0x03u;
    adb_state.current_dev = adb_bus_find_device((cmd >> 4) & 0x0Fu);

    bool srq_needed = false;
    if (adb_state.current_dev) {
        uint16_t flags = adb_state.srq_flags;
        if (adb_state.current_type == ADB_CMD_TALK) {
            flags &= (uint16_t)~(1u << adb_state.current_dev->address);
        }
        srq_needed = flags != 0;
    }

    if (srq_needed) {
        pio_interrupt_clear(adb_pio, adb_sm);
        adb_state.phase = ADB_PHASE_SRQ;
        adb_gpio_rise_arm();
        return;
    }

    adb_pio_stop();
    adb_state.phase = ADB_PHASE_SRQ;
    adb_gpio_rise_arm();
}

bool adb_bus_service(void) {
    bool did_work = false;
    adb_bus_drain_events();

    if (adb_state.phase == ADB_PHASE_ATTENTION) {
        if (adb_gpio_rise) {
            adb_gpio_rise = false;
            adb_gpio_rise_disarm();
            int64_t delta = absolute_time_diff_us(get_absolute_time(), adb_state.attention_start);
            if (delta >= (int64_t)TIME_RESET_THRESH_US) {
                adb_bus_reset_devices();
            }
            pio_interrupt_clear(adb_pio, adb_sm);
            adb_state.phase = ADB_PHASE_COMMAND;
            adb_pio_command_start();
            did_work = true;
        }
        return did_work;
    }

    if (adb_state.phase == ADB_PHASE_SRQ && adb_gpio_rise) {
        adb_gpio_rise = false;
        adb_gpio_rise_disarm();
        adb_bus_execute_command();
        return true;
    }

    if (pio_interrupt_get(adb_pio, adb_sm)) {
        switch (adb_state.phase) {
        case ADB_PHASE_IDLE:
            adb_state.phase = ADB_PHASE_ATTENTION;
            adb_state.attention_start = get_absolute_time();
            adb_gpio_rise_arm();
            did_work = true;
            break;
        case ADB_PHASE_COMMAND:
            adb_bus_handle_command_irq();
            did_work = true;
            break;
        case ADB_PHASE_TALK: {
            adb_device_t *dev = adb_state.current_dev;
            if (adb_pio->irq & (1u << (adb_sm + 4))) {
                if (dev) {
                    dev->collision = true;
                }
                pio_interrupt_clear(adb_pio, adb_sm + 4);
            } else if (dev) {
                dev->collision = false;
                if (adb_state.current_reg == 0) {
                    dev->srq_pending = false;
                    adb_state.srq_flags &= (uint16_t)~(1u << dev->address);
                }
                if (adb_state.current_reg < 4 && !dev->regs[adb_state.current_reg].keep) {
                    dev->regs[adb_state.current_reg].len = 0;
                }
            }
            __atomic_store_n(&adb_rx_activity, 1u, __ATOMIC_RELEASE);
            adb_pio_atn_start();
            adb_state.phase = ADB_PHASE_IDLE;
            did_work = true;
            break;
        }
        case ADB_PHASE_LISTEN: {
            uint32_t remaining = dma_channel_hw_addr((uint)adb_dma_chan)->transfer_count;
            dma_channel_abort((uint)adb_dma_chan);
            uint8_t bytes = (uint8_t)(ADB_MAX_REG_BYTES - remaining);
            adb_bus_apply_listen(bytes);
            if (bytes > 0) {
                __atomic_store_n(&adb_rx_activity, 1u, __ATOMIC_RELEASE);
            }
            adb_pio_atn_start();
            adb_state.phase = ADB_PHASE_IDLE;
            did_work = true;
            break;
        }
        default:
            break;
        }
    }

    return did_work;
}

bool adb_bus_take_activity(void) {
    return __atomic_exchange_n(&adb_rx_activity, 0u, __ATOMIC_ACQ_REL) != 0u;
}
