#include "adb_bus.h"

#include "pico/stdlib.h"
#include "hardware/gpio.h"

#include "adb_events.h"
#include "adb_pio.h"

#define ADB_PULSE_MIN_US 30u
#define ADB_PULSE_MAX_US 1000u
#define ADB_MAX_PULSES_PER_POLL 64u
#define ADB_ATTENTION_MIN_US 700u
#define ADB_ATTENTION_MAX_US 900u
#define ADB_SYNC_MIN_US 60u
#define ADB_SYNC_MAX_US 90u
#define ADB_BIT_ZERO_MAX_US 60u
#define ADB_BIT_ONE_MIN_US 60u
#define ADB_BIT_ONE_MAX_US 90u
#define ADB_KBD_ADDR 2u
#define ADB_KBD_HANDLER_ID 0x02u
#define ADB_MOUSE_ADDR 3u
#define ADB_MOUSE_HANDLER_ID 0x01u
#define ADB_TX_BIT_CELL_US 100u
#define ADB_TX_BIT_ONE_LOW_US 35u
#define ADB_TX_BIT_ZERO_LOW_US 65u
#define ADB_TX_TALK_DELAY_US 190u
#define ADB_KBD_QUEUE_DEPTH 16u
#define ADB_SRQ_PULSE_US 300u
#define ADB_SRQ_COOLDOWN_US 2000u
#define ADB_TX_IDLE_GUARD_US 260u
#define ADB_SRQ_IDLE_GUARD_US 260u
#define ADB_CMD_FLUSH 0u
#define ADB_CMD_LISTEN 2u
#define ADB_CMD_TALK 3u

static uint adb_pin_recv = 0;
static uint adb_pin_xmit = 0;
static adb_pio_t adb_pio = {0};

typedef enum {
    ADB_RX_IDLE = 0,
    ADB_RX_GOT_ATTENTION,
    ADB_RX_BITS,
} adb_rx_state_t;

static volatile uint32_t adb_rx_pulses = 0;
static volatile uint32_t adb_rx_seen = 0;
static volatile uint32_t adb_rx_overruns = 0;
static volatile uint32_t adb_attention_pulses = 0;
static volatile uint32_t adb_sync_pulses = 0;
static volatile uint32_t adb_events_consumed = 0;
static volatile uint32_t adb_cmd_bytes = 0;
static volatile uint32_t adb_last_cmd = 0;
static volatile uint32_t adb_cmd_addr_miss = 0;
static volatile uint32_t adb_srq_pulses = 0;
static volatile uint32_t adb_tx_attempts = 0;
static volatile uint32_t adb_tx_success = 0;
static volatile uint32_t adb_tx_busy = 0;
static volatile uint32_t adb_tx_late_busy = 0;
static volatile uint8_t adb_kbd_addr = ADB_KBD_ADDR;
static volatile uint8_t adb_kbd_handler_id = ADB_KBD_HANDLER_ID;
static volatile uint8_t adb_mouse_addr = ADB_MOUSE_ADDR;
static volatile uint8_t adb_mouse_handler_id = ADB_MOUSE_HANDLER_ID;
static volatile bool adb_rx_latched = false;
static volatile uint32_t adb_rx_raw_pulses = 0;
static volatile uint32_t adb_last_pulse_us = 0;
static volatile uint64_t adb_last_rx_time_us = 0;
static volatile uint32_t adb_min_pulse_us = UINT32_MAX;
static volatile uint32_t adb_max_pulse_us = 0;
static volatile uint32_t adb_pulse_zero_us = 0;
static volatile uint32_t adb_pulse_lt_30_us = 0;
static volatile uint32_t adb_pulse_30_60_us = 0;
static volatile uint32_t adb_pulse_60_90_us = 0;
static volatile uint32_t adb_pulse_90_200_us = 0;
static volatile uint32_t adb_pulse_200_600_us = 0;
static volatile uint32_t adb_pulse_600_700_us = 0;
static volatile uint32_t adb_pulse_700_900_us = 0;
static volatile uint32_t adb_pulse_900_1100_us = 0;
static volatile uint32_t adb_pulse_gt_1100_us = 0;
static adb_rx_state_t adb_rx_state = ADB_RX_IDLE;
static uint8_t adb_rx_bit_count = 0;
static uint8_t adb_rx_shift = 0;
static uint32_t adb_cmd_bytes_handled = 0;
static uint8_t adb_rx_data_expected = 0;
static uint8_t adb_rx_data_count = 0;
static uint8_t adb_rx_data[2] = {0};
static uint8_t adb_listen_target = 0;
static uint8_t adb_kbd_queue[ADB_KBD_QUEUE_DEPTH] = {0};
static uint8_t adb_kbd_q_read = 0;
static uint8_t adb_kbd_q_write = 0;
static uint8_t adb_mouse_buttons = 0;
static uint8_t adb_mouse_reported = 0;
static int16_t adb_mouse_dx = 0;
static int16_t adb_mouse_dy = 0;
static int8_t adb_mouse_last_dx = 0;
static int8_t adb_mouse_last_dy = 0;
static uint64_t adb_srq_next_at = 0;
static uint8_t adb_pending_talk = 0;

enum {
    ADB_LISTEN_NONE = 0,
    ADB_LISTEN_KBD = 1,
    ADB_LISTEN_MOUSE = 2,
};

enum {
    ADB_PENDING_NONE = 0,
    ADB_PENDING_KBD_REG0,
    ADB_PENDING_KBD_REG3,
    ADB_PENDING_MOUSE_REG0,
    ADB_PENDING_MOUSE_REG3,
};

void adb_bus_init(uint pin_recv, uint pin_xmit) {
    adb_pin_recv = pin_recv;
    adb_pin_xmit = pin_xmit;

    gpio_init(adb_pin_recv);
    gpio_set_dir(adb_pin_recv, GPIO_IN);
    gpio_disable_pulls(adb_pin_recv);

    gpio_init(adb_pin_xmit);
    gpio_set_dir(adb_pin_xmit, GPIO_IN);
    gpio_disable_pulls(adb_pin_xmit);

    adb_pio_init(&adb_pio, pio1, 0, 1, pin_recv, pin_xmit);

    adb_rx_pulses = 0;
    adb_rx_seen = 0;
    adb_rx_overruns = 0;
    adb_attention_pulses = 0;
    adb_sync_pulses = 0;
    adb_events_consumed = 0;
    adb_cmd_bytes = 0;
    adb_last_cmd = 0;
    adb_cmd_addr_miss = 0;
    adb_srq_pulses = 0;
    adb_tx_attempts = 0;
    adb_tx_success = 0;
    adb_tx_busy = 0;
    adb_tx_late_busy = 0;
    adb_kbd_addr = ADB_KBD_ADDR;
    adb_kbd_handler_id = ADB_KBD_HANDLER_ID;
    adb_mouse_addr = ADB_MOUSE_ADDR;
    adb_mouse_handler_id = ADB_MOUSE_HANDLER_ID;
    adb_rx_latched = false;
    adb_rx_raw_pulses = 0;
    adb_last_pulse_us = 0;
    adb_last_rx_time_us = 0;
    adb_min_pulse_us = UINT32_MAX;
    adb_max_pulse_us = 0;
    adb_pulse_zero_us = 0;
    adb_pulse_lt_30_us = 0;
    adb_pulse_30_60_us = 0;
    adb_pulse_60_90_us = 0;
    adb_pulse_90_200_us = 0;
    adb_pulse_200_600_us = 0;
    adb_pulse_600_700_us = 0;
    adb_pulse_700_900_us = 0;
    adb_pulse_900_1100_us = 0;
    adb_pulse_gt_1100_us = 0;
    adb_rx_state = ADB_RX_IDLE;
    adb_rx_bit_count = 0;
    adb_rx_shift = 0;
    adb_cmd_bytes_handled = 0;
    adb_rx_data_expected = 0;
    adb_rx_data_count = 0;
    adb_rx_data[0] = 0;
    adb_rx_data[1] = 0;
    adb_listen_target = ADB_LISTEN_NONE;
    adb_kbd_q_read = 0;
    adb_kbd_q_write = 0;
    for (uint8_t i = 0; i < ADB_KBD_QUEUE_DEPTH; i++) {
        adb_kbd_queue[i] = 0;
    }
    adb_mouse_buttons = 0;
    adb_mouse_reported = 0;
    adb_mouse_dx = 0;
    adb_mouse_dy = 0;
    adb_mouse_last_dx = 0;
    adb_mouse_last_dy = 0;
    adb_srq_next_at = 0;
    adb_pending_talk = ADB_PENDING_NONE;
}

static inline bool adb_line_idle(void) {
    return gpio_get(adb_pin_recv);
}

static bool adb_kbd_queue_push(uint8_t code) {
    uint8_t next = (uint8_t)(adb_kbd_q_write + 1u);
    if (next >= ADB_KBD_QUEUE_DEPTH) {
        next = 0;
    }
    if (next == adb_kbd_q_read) {
        return false;
    }
    adb_kbd_queue[adb_kbd_q_write] = code;
    adb_kbd_q_write = next;
    return true;
}

static bool adb_kbd_queue_pop(uint8_t *out_code) {
    if (!out_code || adb_kbd_q_read == adb_kbd_q_write) {
        return false;
    }
    *out_code = adb_kbd_queue[adb_kbd_q_read];
    adb_kbd_q_read++;
    if (adb_kbd_q_read >= ADB_KBD_QUEUE_DEPTH) {
        adb_kbd_q_read = 0;
    }
    return true;
}

static uint8_t adb_kbd_queue_count(void) {
    if (adb_kbd_q_write >= adb_kbd_q_read) {
        return (uint8_t)(adb_kbd_q_write - adb_kbd_q_read);
    }
    return (uint8_t)(ADB_KBD_QUEUE_DEPTH - adb_kbd_q_read + adb_kbd_q_write);
}

static void adb_flush_keyboard(void) {
    adb_kbd_q_read = 0;
    adb_kbd_q_write = 0;
    for (uint8_t i = 0; i < ADB_KBD_QUEUE_DEPTH; i++) {
        adb_kbd_queue[i] = 0;
    }
}

static void adb_flush_mouse(void) {
    adb_mouse_dx = 0;
    adb_mouse_dy = 0;
    adb_mouse_last_dx = 0;
    adb_mouse_last_dy = 0;
    adb_mouse_reported = adb_mouse_buttons;
}

static void adb_tx_bit(bool one) {
    uint32_t low_us = one ? ADB_TX_BIT_ONE_LOW_US : ADB_TX_BIT_ZERO_LOW_US;
    uint32_t high_us = ADB_TX_BIT_CELL_US - low_us;
    uint16_t cycles = adb_pio_us_to_cycles(&adb_pio, low_us);
    adb_pio_tx_pulse(&adb_pio, cycles);
    sleep_us(high_us);
}

static bool adb_can_tx_now(uint64_t now_us) {
    if (!adb_line_idle()) {
        return false;
    }
    uint64_t last_rx = __atomic_load_n(&adb_last_rx_time_us, __ATOMIC_ACQUIRE);
    if (now_us - last_rx < ADB_TX_IDLE_GUARD_US) {
        return false;
    }
    return true;
}

static bool adb_tx_bytes(const uint8_t *data, size_t len) {
    if (!data || len == 0u) {
        return false;
    }
    __atomic_fetch_add(&adb_tx_attempts, 1u, __ATOMIC_RELAXED);
    uint64_t now_us = time_us_64();
    if (!adb_can_tx_now(now_us)) {
        __atomic_fetch_add(&adb_tx_busy, 1u, __ATOMIC_RELAXED);
        return false;
    }
    sleep_us(ADB_TX_TALK_DELAY_US);
    if (!adb_line_idle()) {
        __atomic_fetch_add(&adb_tx_late_busy, 1u, __ATOMIC_RELAXED);
        return false;
    }
    adb_tx_bit(true);
    for (size_t i = 0; i < len; i++) {
        uint8_t byte = data[i];
        for (int bit = 7; bit >= 0; bit--) {
            bool one = (byte & (1u << bit)) != 0u;
            adb_tx_bit(one);
        }
    }
    adb_tx_bit(false);
    __atomic_fetch_add(&adb_tx_success, 1u, __ATOMIC_RELAXED);
    return true;
}

static bool adb_should_srq(void) {
    if (adb_kbd_queue_count() > 0u) {
        return true;
    }
    if (adb_mouse_buttons != adb_mouse_reported) {
        return true;
    }
    if (adb_mouse_dx != 0 || adb_mouse_dy != 0) {
        return true;
    }
    return false;
}

static bool adb_try_srq_pulse(uint64_t now_us) {
    if (!adb_should_srq()) {
        return false;
    }
    if (now_us < adb_srq_next_at) {
        return false;
    }
    uint64_t last_rx = __atomic_load_n(&adb_last_rx_time_us, __ATOMIC_ACQUIRE);
    if (!adb_line_idle() || (now_us - last_rx < ADB_SRQ_IDLE_GUARD_US)) {
        return false;
    }
    uint16_t cycles = adb_pio_us_to_cycles(&adb_pio, ADB_SRQ_PULSE_US);
    adb_pio_tx_pulse(&adb_pio, cycles);
    adb_srq_next_at = now_us + (uint64_t)ADB_SRQ_COOLDOWN_US;
    __atomic_fetch_add(&adb_srq_pulses, 1u, __ATOMIC_RELAXED);
    return true;
}

bool adb_bus_tx_test_pulse_us(uint32_t low_us) {
    if (low_us == 0u) {
        return false;
    }
    uint16_t cycles = adb_pio_us_to_cycles(&adb_pio, low_us);
    adb_pio_tx_pulse(&adb_pio, cycles);
    return true;
}

static bool adb_try_keyboard_reg0(void) {
    uint8_t bytes[2] = {0xFFu, 0xFFu};
    uint32_t consumed = 0;
    uint8_t code = 0;
    while (consumed < 2u && adb_kbd_queue_pop(&code)) {
        bytes[consumed] = code;
        consumed++;
    }
    if (!adb_tx_bytes(bytes, 2u)) {
        return false;
    }
    __atomic_fetch_add(&adb_events_consumed, consumed, __ATOMIC_RELAXED);
    return true;
}

static bool adb_try_keyboard_reg3(void) {
    uint8_t address = __atomic_load_n(&adb_kbd_addr, __ATOMIC_ACQUIRE);
    uint8_t handler = __atomic_load_n(&adb_kbd_handler_id, __ATOMIC_ACQUIRE);
    uint8_t high = (uint8_t)(0x60u | (address & 0x0Fu));
    uint8_t bytes[2] = {high, handler};
    return adb_tx_bytes(bytes, 2u);
}

static void adb_apply_kbd_listen_reg3(uint8_t high, uint8_t low) {
    uint8_t handler = low;
    if (handler == 0x00u || handler == 0xFEu) {
        uint8_t new_addr = (uint8_t)(high & 0x0Fu);
        __atomic_store_n(&adb_kbd_addr, new_addr, __ATOMIC_RELEASE);
    }
    if (handler == 0x02u || handler == 0x03u) {
        __atomic_store_n(&adb_kbd_handler_id, handler, __ATOMIC_RELEASE);
    }
}

static bool adb_try_mouse_reg0(void) {
    int16_t dx = adb_mouse_dx;
    int16_t dy = adb_mouse_dy;
    if (dx < -64) {
        dx = -64;
    } else if (dx > 63) {
        dx = 63;
    }
    if (dy < -64) {
        dy = -64;
    } else if (dy > 63) {
        dy = 63;
    }
    adb_mouse_last_dx = (int8_t)dx;
    adb_mouse_last_dy = (int8_t)dy;
    uint8_t buttons = adb_mouse_buttons;
    uint8_t byte0 = (uint8_t)adb_mouse_last_dy & 0x7Fu;
    uint8_t byte1 = (uint8_t)adb_mouse_last_dx & 0x7Fu;
    byte0 |= (uint8_t)(((~buttons) & 0x01u) << 7);
    byte1 |= (uint8_t)(((~buttons) & 0x02u) << 6);
    uint8_t bytes[2] = {byte0, byte1};
    if (!adb_tx_bytes(bytes, 2u)) {
        return false;
    }
    adb_mouse_reported = buttons;
    adb_mouse_dx -= adb_mouse_last_dx;
    adb_mouse_dy -= adb_mouse_last_dy;
    return true;
}

static bool adb_try_mouse_reg3(void) {
    uint8_t address = __atomic_load_n(&adb_mouse_addr, __ATOMIC_ACQUIRE);
    uint8_t handler = __atomic_load_n(&adb_mouse_handler_id, __ATOMIC_ACQUIRE);
    uint8_t high = (uint8_t)(0x60u | (address & 0x0Fu));
    uint8_t bytes[2] = {high, handler};
    return adb_tx_bytes(bytes, 2u);
}

static void adb_apply_mouse_listen_reg3(uint8_t high, uint8_t low) {
    uint8_t handler = low;
    if (handler == 0x00u || handler == 0xFEu) {
        uint8_t new_addr = (uint8_t)(high & 0x0Fu);
        __atomic_store_n(&adb_mouse_addr, new_addr, __ATOMIC_RELEASE);
    }
    if (handler == 0x01u || handler == 0x03u) {
        __atomic_store_n(&adb_mouse_handler_id, handler, __ATOMIC_RELEASE);
    }
}

static void adb_handle_command(uint8_t cmd) {
    uint8_t address = (uint8_t)(cmd >> 4);
    uint8_t cmd_type = (uint8_t)((cmd >> 2) & 0x03u);
    uint8_t reg = (uint8_t)(cmd & 0x03u);
    uint8_t low_nibble = (uint8_t)(cmd & 0x0Fu);
    uint8_t active_addr = __atomic_load_n(&adb_kbd_addr, __ATOMIC_ACQUIRE);
    uint8_t mouse_addr = __atomic_load_n(&adb_mouse_addr, __ATOMIC_ACQUIRE);
    bool is_kbd = address == active_addr;
    bool is_mouse = address == mouse_addr;
    if (!is_kbd && !is_mouse) {
        __atomic_fetch_add(&adb_cmd_addr_miss, 1u, __ATOMIC_RELAXED);
        return;
    }
    if (low_nibble == 0x01u) {
        if (is_kbd) {
            adb_flush_keyboard();
        } else {
            adb_flush_mouse();
        }
        return;
    }
    if (cmd_type == ADB_CMD_TALK && reg == 0u) {
        adb_pending_talk = is_kbd ? ADB_PENDING_KBD_REG0 : ADB_PENDING_MOUSE_REG0;
        return;
    }
    if (cmd_type == ADB_CMD_TALK && reg == 3u) {
        adb_pending_talk = is_kbd ? ADB_PENDING_KBD_REG3 : ADB_PENDING_MOUSE_REG3;
    }
}

static inline void adb_note_rx_state(uint32_t pulse_us) {
    if (pulse_us >= ADB_ATTENTION_MIN_US && pulse_us <= ADB_ATTENTION_MAX_US) {
        adb_rx_state = ADB_RX_GOT_ATTENTION;
        adb_rx_bit_count = 0;
        adb_rx_shift = 0;
        return;
    }

    if (adb_rx_state == ADB_RX_GOT_ATTENTION) {
        if (pulse_us >= ADB_SYNC_MIN_US && pulse_us <= ADB_SYNC_MAX_US) {
            adb_rx_state = ADB_RX_BITS;
            adb_rx_bit_count = 0;
            adb_rx_shift = 0;
        } else {
            adb_rx_state = ADB_RX_IDLE;
        }
        return;
    }

    if (adb_rx_state != ADB_RX_BITS) {
        return;
    }

    uint8_t bit = 0;
    if (pulse_us < ADB_BIT_ZERO_MAX_US && pulse_us >= ADB_PULSE_MIN_US) {
        bit = 0;
    } else if (pulse_us >= ADB_BIT_ONE_MIN_US && pulse_us <= ADB_BIT_ONE_MAX_US) {
        bit = 1;
    } else {
        adb_rx_state = ADB_RX_IDLE;
        adb_rx_bit_count = 0;
        adb_rx_shift = 0;
        return;
    }

    adb_rx_shift = (uint8_t)((adb_rx_shift << 1u) | bit);
    adb_rx_bit_count++;
    if (adb_rx_bit_count >= 8u) {
        if (adb_rx_data_expected > 0u) {
            if (adb_rx_data_count < (uint8_t)sizeof(adb_rx_data)) {
                adb_rx_data[adb_rx_data_count] = adb_rx_shift;
            }
            adb_rx_data_count++;
            if (adb_rx_data_count >= adb_rx_data_expected) {
                if (adb_rx_data_expected == 2u) {
                    if (adb_listen_target == ADB_LISTEN_KBD) {
                        adb_apply_kbd_listen_reg3(adb_rx_data[0], adb_rx_data[1]);
                    } else if (adb_listen_target == ADB_LISTEN_MOUSE) {
                        adb_apply_mouse_listen_reg3(adb_rx_data[0], adb_rx_data[1]);
                    }
                }
                adb_rx_data_expected = 0;
                adb_rx_data_count = 0;
                adb_listen_target = ADB_LISTEN_NONE;
                adb_rx_state = ADB_RX_IDLE;
            }
        } else {
            __atomic_store_n(&adb_last_cmd, adb_rx_shift, __ATOMIC_RELEASE);
            __atomic_fetch_add(&adb_cmd_bytes, 1u, __ATOMIC_RELAXED);
            uint8_t address = (uint8_t)(adb_rx_shift >> 4);
            uint8_t cmd_type = (uint8_t)((adb_rx_shift >> 2) & 0x03u);
            uint8_t reg = (uint8_t)(adb_rx_shift & 0x03u);
            uint8_t active_addr = __atomic_load_n(&adb_kbd_addr, __ATOMIC_ACQUIRE);
            uint8_t mouse_addr = __atomic_load_n(&adb_mouse_addr, __ATOMIC_ACQUIRE);
            if (cmd_type == ADB_CMD_LISTEN && reg == 3u) {
                if (address == active_addr) {
                    adb_rx_data_expected = 2u;
                    adb_rx_data_count = 0;
                    adb_listen_target = ADB_LISTEN_KBD;
                } else if (address == mouse_addr) {
                    adb_rx_data_expected = 2u;
                    adb_rx_data_count = 0;
                    adb_listen_target = ADB_LISTEN_MOUSE;
                } else {
                    adb_rx_state = ADB_RX_IDLE;
                }
            } else {
                adb_rx_state = ADB_RX_IDLE;
            }
        }
        adb_rx_bit_count = 0;
        adb_rx_shift = 0;
    }
}

static inline void adb_note_rx_pulse(uint32_t pulse_us) {
    __atomic_fetch_add(&adb_rx_raw_pulses, 1u, __ATOMIC_RELAXED);
    if (pulse_us == 0u) {
        __atomic_fetch_add(&adb_pulse_zero_us, 1u, __ATOMIC_RELAXED);
        return;
    }
    __atomic_store_n(&adb_last_rx_time_us, time_us_64(), __ATOMIC_RELEASE);
    __atomic_store_n(&adb_last_pulse_us, pulse_us, __ATOMIC_RELEASE);
    uint32_t min_pulse = __atomic_load_n(&adb_min_pulse_us, __ATOMIC_ACQUIRE);
    if (pulse_us < min_pulse) {
        __atomic_store_n(&adb_min_pulse_us, pulse_us, __ATOMIC_RELEASE);
    }
    uint32_t max_pulse = __atomic_load_n(&adb_max_pulse_us, __ATOMIC_ACQUIRE);
    if (pulse_us > max_pulse) {
        __atomic_store_n(&adb_max_pulse_us, pulse_us, __ATOMIC_RELEASE);
    }
    if (pulse_us < 30u) {
        __atomic_fetch_add(&adb_pulse_lt_30_us, 1u, __ATOMIC_RELAXED);
    } else if (pulse_us < 60u) {
        __atomic_fetch_add(&adb_pulse_30_60_us, 1u, __ATOMIC_RELAXED);
    } else if (pulse_us <= 90u) {
        __atomic_fetch_add(&adb_pulse_60_90_us, 1u, __ATOMIC_RELAXED);
    } else if (pulse_us < 200u) {
        __atomic_fetch_add(&adb_pulse_90_200_us, 1u, __ATOMIC_RELAXED);
    } else if (pulse_us < 600u) {
        __atomic_fetch_add(&adb_pulse_200_600_us, 1u, __ATOMIC_RELAXED);
    } else if (pulse_us < 700u) {
        __atomic_fetch_add(&adb_pulse_600_700_us, 1u, __ATOMIC_RELAXED);
    } else if (pulse_us <= 900u) {
        __atomic_fetch_add(&adb_pulse_700_900_us, 1u, __ATOMIC_RELAXED);
    } else if (pulse_us <= 1100u) {
        __atomic_fetch_add(&adb_pulse_900_1100_us, 1u, __ATOMIC_RELAXED);
    } else {
        __atomic_fetch_add(&adb_pulse_gt_1100_us, 1u, __ATOMIC_RELAXED);
    }
    if (pulse_us >= ADB_ATTENTION_MIN_US && pulse_us <= ADB_ATTENTION_MAX_US) {
        __atomic_fetch_add(&adb_attention_pulses, 1u, __ATOMIC_RELAXED);
    } else if (pulse_us >= ADB_SYNC_MIN_US && pulse_us <= ADB_SYNC_MAX_US) {
        __atomic_fetch_add(&adb_sync_pulses, 1u, __ATOMIC_RELAXED);
    }
    adb_note_rx_state(pulse_us);
    if (pulse_us < ADB_PULSE_MIN_US || pulse_us > ADB_PULSE_MAX_US) {
        return;
    }
    __atomic_fetch_add(&adb_rx_pulses, 1u, __ATOMIC_RELAXED);
    __atomic_fetch_add(&adb_rx_seen, 1u, __ATOMIC_RELAXED);
    __atomic_store_n(&adb_rx_latched, true, __ATOMIC_RELEASE);
}

static bool adb_try_pending_talk(void) {
    uint8_t pending = adb_pending_talk;
    if (pending == ADB_PENDING_NONE) {
        return false;
    }
    bool ok = false;
    switch (pending) {
    case ADB_PENDING_KBD_REG0:
        ok = adb_try_keyboard_reg0();
        break;
    case ADB_PENDING_KBD_REG3:
        ok = adb_try_keyboard_reg3();
        break;
    case ADB_PENDING_MOUSE_REG0:
        ok = adb_try_mouse_reg0();
        break;
    case ADB_PENDING_MOUSE_REG3:
        ok = adb_try_mouse_reg3();
        break;
    default:
        ok = false;
        break;
    }
    if (ok) {
        adb_pending_talk = ADB_PENDING_NONE;
    }
    return ok;
}

bool adb_bus_poll(void) {
    bool did_work = false;
    uint32_t pulse_count = 0;
    uint32_t pulses_handled = 0;
    while (pulses_handled < ADB_MAX_PULSES_PER_POLL
           && adb_pio_rx_pop(&adb_pio, &pulse_count)) {
        uint32_t pulse_us = adb_pio_ticks_to_us(&adb_pio, pulse_count);
        adb_note_rx_pulse(pulse_us);
        did_work = true;
        pulses_handled++;
    }
    if (pulses_handled >= ADB_MAX_PULSES_PER_POLL && adb_pio_rx_has_data(&adb_pio)) {
        __atomic_fetch_add(&adb_rx_overruns, 1u, __ATOMIC_RELAXED);
        adb_pio_rx_flush(&adb_pio);
        did_work = true;
    }

    adb_event_t ev = {0};
    uint8_t event_budget = 16u;
    while (event_budget-- && adb_events_pop(&ev)) {
        if (ev.type == ADB_EVENT_KEY) {
            uint8_t code = ev.a;
            if (!ev.b) {
                code |= 0x80u;
            }
            adb_kbd_queue_push(code);
        } else if (ev.type == ADB_EVENT_MOUSE) {
            adb_mouse_buttons = ev.a;
            adb_mouse_dx += ev.b;
            adb_mouse_dy += ev.c;
        }
        did_work = true;
    }

    uint32_t cmd_bytes = __atomic_load_n(&adb_cmd_bytes, __ATOMIC_ACQUIRE);
    if (cmd_bytes != adb_cmd_bytes_handled) {
        adb_cmd_bytes_handled = cmd_bytes;
        uint8_t cmd = (uint8_t)__atomic_load_n(&adb_last_cmd, __ATOMIC_ACQUIRE);
        adb_handle_command(cmd);
        did_work = true;
    }

    if (adb_try_pending_talk()) {
        did_work = true;
    }

    adb_try_srq_pulse(time_us_64());

    return did_work;
}

bool adb_bus_take_rx_seen(void) {
    return __atomic_exchange_n(&adb_rx_latched, false, __ATOMIC_ACQ_REL);
}

void adb_bus_get_stats(adb_bus_stats_t *out_stats) {
    if (!out_stats) {
        return;
    }
    out_stats->rx_raw_pulses = __atomic_load_n(&adb_rx_raw_pulses, __ATOMIC_ACQUIRE);
    out_stats->rx_pulses = __atomic_load_n(&adb_rx_pulses, __ATOMIC_ACQUIRE);
    out_stats->rx_seen = __atomic_load_n(&adb_rx_seen, __ATOMIC_ACQUIRE);
    out_stats->rx_overruns = __atomic_load_n(&adb_rx_overruns, __ATOMIC_ACQUIRE);
    out_stats->attention_pulses = __atomic_load_n(&adb_attention_pulses, __ATOMIC_ACQUIRE);
    out_stats->sync_pulses = __atomic_load_n(&adb_sync_pulses, __ATOMIC_ACQUIRE);
    out_stats->events_consumed = __atomic_load_n(&adb_events_consumed, __ATOMIC_ACQUIRE);
    uint32_t pending = adb_events_pending();
    pending += adb_kbd_queue_count();
    if (adb_mouse_buttons != adb_mouse_reported || adb_mouse_dx != 0 || adb_mouse_dy != 0) {
        pending += 1u;
    }
    out_stats->events_pending = pending;
    out_stats->cmd_bytes = __atomic_load_n(&adb_cmd_bytes, __ATOMIC_ACQUIRE);
    out_stats->last_cmd = __atomic_load_n(&adb_last_cmd, __ATOMIC_ACQUIRE);
    out_stats->cmd_addr_miss = __atomic_load_n(&adb_cmd_addr_miss, __ATOMIC_ACQUIRE);
    out_stats->srq_pulses = __atomic_load_n(&adb_srq_pulses, __ATOMIC_ACQUIRE);
    out_stats->tx_attempts = __atomic_load_n(&adb_tx_attempts, __ATOMIC_ACQUIRE);
    out_stats->tx_success = __atomic_load_n(&adb_tx_success, __ATOMIC_ACQUIRE);
    out_stats->tx_busy = __atomic_load_n(&adb_tx_busy, __ATOMIC_ACQUIRE);
    out_stats->tx_late_busy = __atomic_load_n(&adb_tx_late_busy, __ATOMIC_ACQUIRE);
    out_stats->last_pulse_us = __atomic_load_n(&adb_last_pulse_us, __ATOMIC_ACQUIRE);
    uint32_t min_pulse = __atomic_load_n(&adb_min_pulse_us, __ATOMIC_ACQUIRE);
    if (min_pulse == UINT32_MAX) {
        min_pulse = 0;
    }
    out_stats->min_pulse_us = min_pulse;
    out_stats->max_pulse_us = __atomic_load_n(&adb_max_pulse_us, __ATOMIC_ACQUIRE);
    out_stats->pulse_zero_us = __atomic_load_n(&adb_pulse_zero_us, __ATOMIC_ACQUIRE);
    out_stats->pulse_lt_30_us = __atomic_load_n(&adb_pulse_lt_30_us, __ATOMIC_ACQUIRE);
    out_stats->pulse_30_60_us = __atomic_load_n(&adb_pulse_30_60_us, __ATOMIC_ACQUIRE);
    out_stats->pulse_60_90_us = __atomic_load_n(&adb_pulse_60_90_us, __ATOMIC_ACQUIRE);
    out_stats->pulse_90_200_us = __atomic_load_n(&adb_pulse_90_200_us, __ATOMIC_ACQUIRE);
    out_stats->pulse_200_600_us = __atomic_load_n(&adb_pulse_200_600_us, __ATOMIC_ACQUIRE);
    out_stats->pulse_600_700_us = __atomic_load_n(&adb_pulse_600_700_us, __ATOMIC_ACQUIRE);
    out_stats->pulse_700_900_us = __atomic_load_n(&adb_pulse_700_900_us, __ATOMIC_ACQUIRE);
    out_stats->pulse_900_1100_us = __atomic_load_n(&adb_pulse_900_1100_us, __ATOMIC_ACQUIRE);
    out_stats->pulse_gt_1100_us = __atomic_load_n(&adb_pulse_gt_1100_us, __ATOMIC_ACQUIRE);
}
