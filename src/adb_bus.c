#include "adb_bus.h"

#include <stddef.h>

#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "pico/sem.h"
#include "pico/rand.h"
#include "pico/stdlib.h"

#include "adb_bus.pio.h"
#include "adb_core.h"
#include "adb_driver.h"
#include "adb_queue.h"
#include "gpio_irq_dispatch.h"

#define ADB_PIN_RECV 6
#define ADB_PIN_XMIT 12

#define ADB_MAX_REG_BYTES 8u
#define PIO_ATN_MIN 386u
#define PIO_CMD_OFFSET 2u
#define TIME_RESET_THRESH_US 400u

typedef void (*adb_handle_get_fn)(uint8_t address, uint8_t stored_id, uint8_t *out, void *ctx);
typedef void (*adb_handle_set_fn)(uint8_t address, uint8_t proposed, uint8_t *stored_id, void *ctx);
typedef void (*adb_listen_fn)(uint8_t address, uint8_t reg, const uint8_t *data, uint8_t len, void *ctx);
typedef void (*adb_flush_fn)(uint8_t address, uint8_t reg, void *ctx);

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

typedef struct adb_device {
    uint8_t address;
    uint8_t handler_id;
    adb_handle_get_fn get_handle_fn;
    adb_handle_set_fn set_handle_fn;
    void *handle_ctx;
    bool (*reg0_pop)(struct adb_device *dev, uint8_t *first, uint8_t *second);
    void *reg0_queue_ctx;
    adb_listen_fn listen_fn;
    adb_flush_fn flush_fn;
    void *listen_ctx;
    void *flush_ctx;
    bool srq_enabled;
    bool collision;
    bool srq_pending;
    semaphore_t talk_sem;
    adb_register_t regs[4];
} adb_device_t;

typedef struct {
    adb_phase_t phase;
    uint8_t current_cmd;
    adb_cmd_t current_type;
    uint8_t current_reg;
    adb_device_t *current_dev;
    uint32_t time;

    uint8_t listen_buf[ADB_MAX_REG_BYTES];

    adb_driver_state_t driver;
    uint16_t srq_flags;
    bool talk_lock_held;

    uint32_t dbg_lock_fail;
    uint32_t dbg_collision;
    uint32_t dbg_atn;
    uint32_t dbg_atn_short;
    uint32_t dbg_rst;
    uint32_t dbg_abrt;
    uint32_t dbg_abrt_time;
    uint32_t dbg_err;
    uint32_t dbg_talk_empty;
    uint32_t dbg_talk_bytes;
    uint32_t dbg_reg0_fills;
    uint32_t dbg_srq_set;
    uint32_t dbg_srq_clear;
    uint32_t dbg_srq_suppress;
    uint32_t dbg_rx_gate_armed;
    uint32_t dbg_rx_gate_immediate;
    uint32_t dbg_gpio_rise;
} adb_bus_state_t;

static PIO adb_pio = pio1;
static uint adb_sm = 0;
static uint adb_offset_atn = 0;
static uint adb_offset_rx = 0;
static uint adb_offset_tx = 0;
static int adb_dma_chan = -1;
static volatile uint32_t adb_rx_activity = 0;
static uint32_t adb_gpio_last_rise = 0;

static const uint8_t adb_rand_table[] = {
    0x25, 0x5D, 0x05, 0x17, 0x58, 0xE9, 0x5E, 0xD4,
    0xAB, 0xB2, 0xCD, 0xC6, 0x9B, 0xB4, 0x54, 0x11,
    0x0E, 0x82, 0x74, 0x41, 0x21, 0x3D, 0xDC, 0x87
};
static uint8_t adb_rand_idx = 0;

static adb_device_t adb_devices[2];
static adb_bus_state_t adb_state;

static void adb_device_reset(adb_device_t *dev, uint8_t address, uint8_t handler_id);
static void adb_default_get_handle(uint8_t address, uint8_t stored_id, uint8_t *out, void *ctx);
static uint8_t adb_bus_get_handler_id(const adb_device_t *dev);

static void adb_default_get_handle(uint8_t address, uint8_t stored_id, uint8_t *out, void *ctx) {
    (void)address;
    (void)ctx;
    if (out) {
        *out = stored_id;
    }
}

static uint8_t adb_bus_get_handler_id(const adb_device_t *dev) {
    if (dev->get_handle_fn) {
        uint8_t handler = dev->handler_id;
        dev->get_handle_fn(dev->address, dev->handler_id, &handler, dev->handle_ctx);
        return handler;
    }
    return dev->handler_id;
}

static void adb_device_reset(adb_device_t *dev, uint8_t address, uint8_t handler_id) {
    dev->address = address;
    dev->handler_id = handler_id;
    dev->srq_enabled = true;
    dev->collision = false;
    dev->srq_pending = false;
    sem_init(&dev->talk_sem, 1, 1);
    for (size_t i = 0; i < 4; i++) {
        dev->regs[i].len = 0;
        dev->regs[i].keep = false;
    }
}

static void adb_bus_reset_devices(void) {
    adb_device_reset(&adb_devices[0], 2, 2);
    adb_device_reset(&adb_devices[1], 3, 1);
    adb_driver_reset(&adb_state.driver);
    adb_state.srq_flags = 0;
    adb_state.talk_lock_held = false;
    adb_state.dbg_lock_fail = 0;
    adb_state.dbg_collision = 0;
    adb_state.dbg_atn = 0;
    adb_state.dbg_atn_short = 0;
    adb_state.dbg_rst = 0;
    adb_state.dbg_abrt = 0;
    adb_state.dbg_abrt_time = 0;
    adb_state.dbg_err = 0;
    adb_state.dbg_talk_empty = 0;
    adb_state.dbg_talk_bytes = 0;
    adb_state.dbg_reg0_fills = 0;
    adb_state.dbg_srq_set = 0;
    adb_state.dbg_srq_clear = 0;
    adb_state.dbg_srq_suppress = 0;
    adb_state.dbg_rx_gate_armed = 0;
    adb_state.dbg_rx_gate_immediate = 0;
    adb_state.dbg_gpio_rise = 0;
    adb_gpio_last_rise = 0;
}

static adb_device_t *adb_bus_find_device(uint8_t address) {
    for (size_t i = 0; i < 2; i++) {
        if (adb_devices[i].address == address) {
            return &adb_devices[i];
        }
    }
    return NULL;
}

static bool adb_bus_try_drain_reg0(adb_device_t *dev) {
    bool did_pop = false;
    if (!dev->reg0_pop) {
        return false;
    }
    if (!sem_try_acquire(&dev->talk_sem)) {
        adb_state.dbg_lock_fail++;
        return false;
    }
    if (dev->regs[0].len != 0) {
        sem_release(&dev->talk_sem);
        return false;
    }
    uint8_t first = 0xFF;
    uint8_t second = 0xFF;
    if (dev->reg0_pop(dev, &first, &second)) {
        dev->regs[0].data[0] = first;
        dev->regs[0].data[1] = second;
        dev->regs[0].len = 2;
        dev->regs[0].keep = false;
        did_pop = true;
        adb_state.dbg_reg0_fills++;
    } else {
        dev->regs[0].len = 0;
        dev->regs[0].keep = false;
    }
    if (dev->regs[0].len > 0) {
        if (dev->srq_enabled) {
            dev->srq_pending = true;
            adb_state.srq_flags |= (uint16_t)(1u << dev->address);
            adb_state.dbg_srq_set++;
        } else {
            adb_state.dbg_srq_suppress++;
        }
    }
    sem_release(&dev->talk_sem);
    return did_pop;
}

static bool adb_bus_drain_events(void) {
    bool did_work = false;
    adb_event_t event;
    while (adb_queue_pop(&event)) {
        adb_core_record_event(event.type);
        adb_driver_handle_event(&adb_state.driver, &event, &adb_state.dbg_lock_fail);
        did_work = true;
    }
    adb_driver_flush(&adb_state.driver, &adb_state.dbg_lock_fail);
    if (adb_bus_try_drain_reg0(&adb_devices[0])) {
        did_work = true;
    }
    if (adb_bus_try_drain_reg0(&adb_devices[1])) {
        did_work = true;
    }
    return did_work;
}

static void adb_bus_prepare_reg3(adb_device_t *dev, uint8_t handler_id) {
    uint8_t rand_addr = adb_rand_table[adb_rand_idx++] & 0x0Fu;
    if (adb_rand_idx >= sizeof(adb_rand_table)) {
        adb_rand_idx = 0;
    }
    dev->regs[3].data[0] = (uint8_t)((dev->srq_enabled ? 0x20u : 0x00u) | 0x40u | rand_addr);
    dev->regs[3].data[1] = handler_id;
    dev->regs[3].len = 2;
    dev->regs[3].keep = false;
}

static void dev_pio_stop(void) {
    pio_sm_set_enabled(adb_pio, adb_sm, false);
    pio_interrupt_clear(adb_pio, adb_sm);
}

static void dev_pio_atn_start(void) {
    pio_sm_config pc;
    bus_atn_dev_pio_config(&pc, adb_offset_atn, ADB_PIN_RECV);
    pio_sm_init(adb_pio, adb_sm, adb_offset_atn, &pc);
    pio_sm_put(adb_pio, adb_sm, PIO_ATN_MIN);
    adb_state.time = time_us_32();
    pio_sm_set_enabled(adb_pio, adb_sm, true);
}

static void dev_pio_command_start(void) {
    pio_sm_config pc;
    bus_rx_dev_pio_config(&pc, adb_offset_rx, ADB_PIN_XMIT, ADB_PIN_RECV);
    pio_sm_init(adb_pio, adb_sm, adb_offset_rx + PIO_CMD_OFFSET, &pc);
    pio_sm_put(adb_pio, adb_sm, 7);
    adb_state.time = time_us_32();
    pio_sm_set_enabled(adb_pio, adb_sm, true);
}

static void dev_pio_tx_start(void) {
    pio_sm_config pc;
    bus_tx_dev_pio_config(&pc, adb_offset_tx, ADB_PIN_XMIT, ADB_PIN_RECV);
    pio_sm_init(adb_pio, adb_sm, adb_offset_tx, &pc);
    adb_state.time = time_us_32();
    pio_sm_set_enabled(adb_pio, adb_sm, true);
}

static void dev_pio_rx_start(void) {
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

    adb_state.time = time_us_32();
    pio_sm_set_enabled(adb_pio, adb_sm, true);
}

static bool setup_gpio_isr(void) {
    adb_state.time = time_us_32();
    if (gpio_get(ADB_PIN_RECV)) {
        adb_state.dbg_rx_gate_immediate++;
        return false;
    }
    gpio_set_irq_enabled(ADB_PIN_RECV, GPIO_IRQ_EDGE_RISE, true);
    if (gpio_get(ADB_PIN_RECV)) {
        gpio_set_irq_enabled(ADB_PIN_RECV, GPIO_IRQ_EDGE_RISE, false);
        gpio_acknowledge_irq(ADB_PIN_RECV, GPIO_IRQ_EDGE_RISE);
        adb_state.dbg_rx_gate_immediate++;
        return false;
    }
    adb_state.dbg_rx_gate_armed++;
    return true;
}

static bool isr_command_process(bool *srq) {
    if (pio_sm_is_rx_fifo_empty(adb_pio, adb_sm)) {
        return false;
    }
    uint8_t cmd = bus_rx_dev_get(adb_pio, adb_sm);
    adb_state.current_cmd = cmd;
    adb_state.current_type = (adb_cmd_t)((cmd >> 2) & 0x03u);
    adb_state.current_reg = cmd & 0x03u;
    adb_state.current_dev = adb_bus_find_device((cmd >> 4) & 0x0Fu);

    *srq = false;
    if (adb_state.current_type == ADB_CMD_TALK && adb_state.current_reg == 0) {
        if (adb_state.current_dev) {
            *srq = (adb_state.srq_flags & (uint16_t)~(1u << adb_state.current_dev->address)) != 0;
        } else {
            *srq = adb_state.srq_flags != 0;
        }
    }

    return true;
}

static adb_phase_t isr_command_execute(void) {
    adb_cmd_t type = adb_state.current_type;
    uint8_t reg = adb_state.current_reg;
    adb_device_t *dev = adb_state.current_dev;

    if (type == ADB_CMD_RESET) {
        adb_bus_reset_devices();
        dev_pio_atn_start();
        return ADB_PHASE_IDLE;
    }

    if (!dev) {
        dev_pio_atn_start();
        return ADB_PHASE_IDLE;
    }

    switch (type) {
    case ADB_CMD_TALK: {
        dev_pio_tx_start();
        if (reg == 3) {
            uint8_t handler_id = adb_bus_get_handler_id(dev);
            if (handler_id == 0xFFu) {
                dev_pio_stop();
                dev_pio_atn_start();
                return ADB_PHASE_IDLE;
            }
            adb_bus_prepare_reg3(dev, handler_id);
            adb_state.dbg_talk_bytes += dev->regs[3].len;
            bus_tx_dev_putm(adb_pio, adb_sm, dev->regs[3].data, dev->regs[3].len);
            return ADB_PHASE_TALK;
        }
        if (sem_try_acquire(&dev->talk_sem)) {
            if (dev->regs[reg].len == 0) {
                adb_state.dbg_talk_empty++;
                if (reg == 0) {
                    dev->srq_pending = false;
                    adb_state.srq_flags &= (uint16_t)~(1u << dev->address);
                    adb_state.dbg_srq_clear++;
                }
                sem_release(&dev->talk_sem);
                dev_pio_stop();
                dev_pio_atn_start();
                return ADB_PHASE_IDLE;
            }
            adb_state.dbg_talk_bytes += dev->regs[reg].len;
            adb_state.talk_lock_held = true;
            bus_tx_dev_putm(adb_pio, adb_sm, dev->regs[reg].data, dev->regs[reg].len);
            return ADB_PHASE_TALK;
        }
        adb_state.dbg_lock_fail++;
        dev_pio_stop();
        dev_pio_atn_start();
        return ADB_PHASE_IDLE;
    }
    case ADB_CMD_LISTEN:
        dev_pio_rx_start();
        return ADB_PHASE_LISTEN;
    case ADB_CMD_FLUSH:
        if (dev->flush_fn) {
            dev->flush_fn(dev->address, reg, dev->flush_ctx);
        }
        dev_pio_atn_start();
        return ADB_PHASE_IDLE;
    default:
        adb_state.dbg_err++;
        dev_pio_atn_start();
        return ADB_PHASE_IDLE;
    }
}

static void isr_talk_complete(void) {
    adb_device_t *dev = adb_state.current_dev;
    if (!dev) {
        return;
    }
    if (adb_pio->irq & (1u << (adb_sm + 4))) {
        dev->collision = true;
        adb_state.dbg_collision++;
        pio_interrupt_clear(adb_pio, adb_sm + 4);
    } else {
        dev->collision = false;
        if (adb_state.current_reg == 0) {
            dev->srq_pending = false;
            adb_state.srq_flags &= (uint16_t)~(1u << dev->address);
            adb_state.dbg_srq_clear++;
        }
        if (adb_state.current_reg < 4 && !dev->regs[adb_state.current_reg].keep) {
            dev->regs[adb_state.current_reg].len = 0;
        }
    }
    if (adb_state.talk_lock_held) {
        sem_release(&dev->talk_sem);
        adb_state.talk_lock_held = false;
    }
    __atomic_store_n(&adb_rx_activity, 1u, __ATOMIC_RELEASE);
}

static void isr_listen_complete(void) {
    adb_device_t *dev = adb_state.current_dev;
    if (!dev) {
        return;
    }
    uint8_t reg = adb_state.current_reg;

    uint32_t remaining = dma_channel_hw_addr((uint)adb_dma_chan)->transfer_count;
    dma_channel_abort((uint)adb_dma_chan);
    uint8_t bytes = (uint8_t)(ADB_MAX_REG_BYTES - remaining);
    if (bytes == 0) {
        return;
    }

    if (reg == 3) {
        if (bytes != 2) {
            return;
        }
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
                    adb_state.srq_flags &= (uint16_t)~(1u << dev->address);
                    adb_state.dbg_srq_clear++;
                }
            }
        } else if (dev->set_handle_fn) {
            dev->set_handle_fn(dev->address, low, &dev->handler_id, dev->handle_ctx);
        } else {
            dev->handler_id = low;
        }
        if (dev->listen_fn) {
            dev->listen_fn(dev->address, reg, adb_state.listen_buf, bytes, dev->listen_ctx);
        }
    } else {
        if (!sem_try_acquire(&dev->talk_sem)) {
            adb_state.dbg_lock_fail++;
            return;
        }
        if (bytes > ADB_MAX_REG_BYTES) {
            bytes = ADB_MAX_REG_BYTES;
        }
        if (bytes < 2) {
            dev->regs[reg].len = 0;
            dev->regs[reg].keep = false;
            if (reg == 0) {
                dev->srq_pending = false;
                adb_state.srq_flags &= (uint16_t)~(1u << dev->address);
                adb_state.dbg_srq_clear++;
            }
            sem_release(&dev->talk_sem);
            return;
        }
        dev->regs[reg].len = bytes;
        dev->regs[reg].keep = true;
        for (uint8_t i = 0; i < bytes; i++) {
            dev->regs[reg].data[i] = adb_state.listen_buf[i];
        }
        if (dev->listen_fn) {
            dev->listen_fn(dev->address, reg, dev->regs[reg].data, dev->regs[reg].len, dev->listen_ctx);
        }
        sem_release(&dev->talk_sem);
    }

    __atomic_store_n(&adb_rx_activity, 1u, __ATOMIC_RELEASE);
}

static void adb_gpio_isr(uint gpio, uint32_t events, void *ctx) {
    (void)gpio;
    (void)ctx;
    if (!(events & GPIO_IRQ_EDGE_RISE)) {
        return;
    }

    gpio_set_irq_enabled(ADB_PIN_RECV, GPIO_IRQ_EDGE_RISE, false);
    adb_state.dbg_gpio_rise++;
    uint32_t now = time_us_32();
    if ((uint32_t)(now - adb_gpio_last_rise) < 50u) {
        adb_gpio_last_rise = now;
        return;
    }
    adb_gpio_last_rise = now;

    switch (adb_state.phase) {
    case ADB_PHASE_ATTENTION: {
        uint32_t td = time_us_32() - adb_state.time;
        if (td > TIME_RESET_THRESH_US) {
            adb_bus_reset_devices();
            dev_pio_atn_start();
            adb_state.dbg_rst++;
            adb_state.phase = ADB_PHASE_IDLE;
        } else {
            dev_pio_command_start();
            adb_state.dbg_atn++;
            adb_state.phase = ADB_PHASE_COMMAND;
        }
        break;
    }
    case ADB_PHASE_SRQ:
        adb_state.phase = isr_command_execute();
        break;
    default:
        dev_pio_atn_start();
        adb_state.dbg_err++;
        adb_state.phase = ADB_PHASE_IDLE;
        break;
    }
}

static void adb_pio_isr(void) {
    uint32_t irq = adb_pio->irq;
    if (!(irq & (1u << adb_sm))) {
        return;
    }

    switch (adb_state.phase) {
    case ADB_PHASE_IDLE:
        dev_pio_stop();
        if (setup_gpio_isr()) {
            adb_state.phase = ADB_PHASE_ATTENTION;
        } else {
            dev_pio_command_start();
            adb_state.dbg_atn_short++;
            adb_state.phase = ADB_PHASE_COMMAND;
        }
        break;
    case ADB_PHASE_COMMAND: {
        bool srq = false;
        if (!isr_command_process(&srq)) {
            dev_pio_stop();
            dev_pio_atn_start();
            adb_state.dbg_abrt++;
            adb_state.dbg_abrt_time = time_us_32();
            adb_state.phase = ADB_PHASE_IDLE;
            break;
        }
        if (srq) {
            pio_interrupt_clear(adb_pio, adb_sm);
            adb_state.phase = ADB_PHASE_SRQ;
        } else {
            dev_pio_stop();
            if (!setup_gpio_isr()) {
                adb_state.phase = isr_command_execute();
            } else {
                adb_state.phase = ADB_PHASE_SRQ;
            }
        }
        break;
    }
    case ADB_PHASE_SRQ:
        dev_pio_stop();
        if (!setup_gpio_isr()) {
            adb_state.phase = isr_command_execute();
        }
        break;
    case ADB_PHASE_TALK:
        dev_pio_stop();
        isr_talk_complete();
        dev_pio_atn_start();
        adb_state.phase = ADB_PHASE_IDLE;
        break;
    case ADB_PHASE_LISTEN:
        dev_pio_stop();
        isr_listen_complete();
        dev_pio_atn_start();
        adb_state.phase = ADB_PHASE_IDLE;
        break;
    default:
        dev_pio_stop();
        dev_pio_atn_start();
        adb_state.dbg_err++;
        adb_state.phase = ADB_PHASE_IDLE;
        break;
    }
}

void adb_bus_init(void) {
    gpio_init(ADB_PIN_RECV);
    gpio_set_dir(ADB_PIN_RECV, GPIO_IN);
    gpio_pull_up(ADB_PIN_RECV);
    (void)gpio_irq_dispatch_register(ADB_PIN_RECV, GPIO_IRQ_EDGE_RISE, adb_gpio_isr, NULL);
    gpio_set_irq_enabled(ADB_PIN_RECV, GPIO_IRQ_EDGE_RISE, false);

    gpio_set_slew_rate(ADB_PIN_XMIT, GPIO_SLEW_RATE_SLOW);
    gpio_init(ADB_PIN_XMIT);
    gpio_set_dir(ADB_PIN_XMIT, GPIO_OUT);
    gpio_put(ADB_PIN_XMIT, 0);

    pio_gpio_init(adb_pio, ADB_PIN_XMIT);

    adb_offset_atn = pio_add_program(adb_pio, &bus_atn_dev_program);
    adb_offset_rx = pio_add_program(adb_pio, &bus_rx_dev_program);
    adb_offset_tx = pio_add_program(adb_pio, &bus_tx_dev_program);

    adb_sm = pio_claim_unused_sm(adb_pio, true);
    adb_dma_chan = dma_claim_unused_channel(true);

    pio_sm_set_pindirs_with_mask(adb_pio, adb_sm, 1u << ADB_PIN_XMIT, 1u << ADB_PIN_XMIT);
    pio_sm_set_pins_with_mask(adb_pio, adb_sm, 0u, 1u << ADB_PIN_XMIT);

    pio_set_irq0_source_mask_enabled(adb_pio,
                                     1u << (PIO_INTR_SM0_LSB + adb_sm),
                                     true);
    irq_set_exclusive_handler(PIO1_IRQ_0, adb_pio_isr);
    irq_set_enabled(PIO1_IRQ_0, true);

    adb_rand_idx = (uint8_t)(get_rand_32() % sizeof(adb_rand_table));
    adb_bus_reset_devices();
    adb_driver_init(&adb_state.driver);
    adb_state.phase = ADB_PHASE_IDLE;
    adb_state.talk_lock_held = false;
    dev_pio_atn_start();
}

bool adb_bus_service(void) {
    return adb_bus_drain_events();
}

bool adb_bus_take_activity(void) {
    return __atomic_exchange_n(&adb_rx_activity, 0u, __ATOMIC_ACQ_REL) != 0u;
}

void adb_bus_get_stats(adb_bus_stats_t *out) {
    out->lock_fails = __atomic_load_n(&adb_state.dbg_lock_fail, __ATOMIC_ACQUIRE);
    out->collisions = __atomic_load_n(&adb_state.dbg_collision, __ATOMIC_ACQUIRE);
    out->attentions = __atomic_load_n(&adb_state.dbg_atn, __ATOMIC_ACQUIRE);
    out->attention_short = __atomic_load_n(&adb_state.dbg_atn_short, __ATOMIC_ACQUIRE);
    out->resets = __atomic_load_n(&adb_state.dbg_rst, __ATOMIC_ACQUIRE);
    out->aborts = __atomic_load_n(&adb_state.dbg_abrt, __ATOMIC_ACQUIRE);
    out->abort_time = __atomic_load_n(&adb_state.dbg_abrt_time, __ATOMIC_ACQUIRE);
    out->errors = __atomic_load_n(&adb_state.dbg_err, __ATOMIC_ACQUIRE);
    out->talk_empty = __atomic_load_n(&adb_state.dbg_talk_empty, __ATOMIC_ACQUIRE);
    out->talk_bytes = __atomic_load_n(&adb_state.dbg_talk_bytes, __ATOMIC_ACQUIRE);
    out->reg0_fills = __atomic_load_n(&adb_state.dbg_reg0_fills, __ATOMIC_ACQUIRE);
    out->srq_sets = __atomic_load_n(&adb_state.dbg_srq_set, __ATOMIC_ACQUIRE);
    out->srq_clears = __atomic_load_n(&adb_state.dbg_srq_clear, __ATOMIC_ACQUIRE);
    out->srq_suppressed = __atomic_load_n(&adb_state.dbg_srq_suppress, __ATOMIC_ACQUIRE);
    out->rx_gate_armed = __atomic_load_n(&adb_state.dbg_rx_gate_armed, __ATOMIC_ACQUIRE);
    out->rx_gate_immediate = __atomic_load_n(&adb_state.dbg_rx_gate_immediate, __ATOMIC_ACQUIRE);
    out->gpio_rise_events = __atomic_load_n(&adb_state.dbg_gpio_rise, __ATOMIC_ACQUIRE);
}

bool adb_bus_set_handle_fns(uint8_t address,
                            void (*get_fn)(uint8_t address, uint8_t stored_id, uint8_t *out, void *ctx),
                            void (*set_fn)(uint8_t address, uint8_t proposed, uint8_t *stored_id, void *ctx),
                            void *ctx) {
    adb_device_t *dev = adb_bus_find_device(address);
    if (!dev) {
        return false;
    }
    dev->get_handle_fn = get_fn ? get_fn : adb_default_get_handle;
    dev->set_handle_fn = set_fn;
    dev->handle_ctx = ctx;
    return true;
}

bool adb_bus_set_reg0_pop(uint8_t address, bool (*fn)(struct adb_device *dev, uint8_t *first, uint8_t *second), void *queue_ctx) {
    adb_device_t *dev = adb_bus_find_device(address);
    if (!dev) {
        return false;
    }
    dev->reg0_pop = fn;
    dev->reg0_queue_ctx = queue_ctx;
    return true;
}

bool adb_bus_set_listen_fn(uint8_t address, void (*fn)(uint8_t address, uint8_t reg, const uint8_t *data, uint8_t len, void *ctx), void *ctx) {
    adb_device_t *dev = adb_bus_find_device(address);
    if (!dev) {
        return false;
    }
    dev->listen_fn = fn;
    dev->listen_ctx = ctx;
    return true;
}

bool adb_bus_set_flush_fn(uint8_t address, void (*fn)(uint8_t address, uint8_t reg, void *ctx), void *ctx) {
    adb_device_t *dev = adb_bus_find_device(address);
    if (!dev) {
        return false;
    }
    dev->flush_fn = fn;
    dev->flush_ctx = ctx;
    return true;
}

void *adb_bus_get_reg0_ctx(struct adb_device *dev) {
    return dev->reg0_queue_ctx;
}
