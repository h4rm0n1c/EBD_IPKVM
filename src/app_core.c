#include "app_core.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "pico/bootrom.h"
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/watchdog.h"
#include "tusb.h"

#include "core_bridge.h"
#include "stream_protocol.h"
#include "adb_bus.h"
#include "adb_core.h"
#include "adb_queue.h"
#include "usb_control.h"
#include "video_capture.h"
#include "video_core.h"

#define APP_PKT_MAX_PAYLOAD (CAP_BYTES_PER_LINE * 2)
#define APP_PKT_MAX_BYTES (STREAM_HEADER_BYTES + APP_PKT_MAX_PAYLOAD)
#define APP_PKT_RAW_BYTES (STREAM_HEADER_BYTES + CAP_BYTES_PER_LINE)

#define CDC_CTRL 0
#define CDC_ADB 1

static app_core_config_t app_cfg;

static volatile bool ps_on_state = false;
static uint32_t usb_drops = 0;
static uint16_t txq_offset = 0;

static uint8_t probe_buf[APP_PKT_MAX_BYTES];
static volatile uint8_t probe_pending = 0;
static uint16_t probe_offset = 0;
static volatile bool debug_requested = false;
static uint32_t core0_busy_us = 0;
static uint32_t core0_total_us = 0;
static volatile uint8_t ep0_cmd_queue[8];
static volatile uint8_t ep0_cmd_r = 0;
static volatile uint8_t ep0_cmd_w = 0;

static absolute_time_t status_next;
static uint32_t status_last_lines = 0;
static absolute_time_t adb_rx_next;
static bool adb_rx_seen = false;
static uint8_t adb_esc_state = 0;
static uint8_t adb_mouse_buttons = 0;

static inline bool stream_ready(void) {
    return tud_ready();
}

static inline int stream_write_available(void) {
    return tud_vendor_write_available();
}

static inline uint32_t stream_write(const void *buf, uint32_t len) {
    return tud_vendor_write(buf, len);
}

static inline void stream_flush(void) {
    tud_vendor_flush();
}

static inline void set_ps_on(bool on) {
    ps_on_state = on;
    gpio_put(app_cfg.pin_ps_on, on ? 1 : 0);
}

static inline bool can_emit_text(void) {
    return tud_cdc_n_connected(CDC_CTRL);
}

static inline void take_core0_utilization(uint32_t *busy_us, uint32_t *total_us) {
    if (busy_us) {
        *busy_us = core0_busy_us;
        core0_busy_us = 0;
    }
    if (total_us) {
        *total_us = core0_total_us;
        core0_total_us = 0;
    }
}

bool app_core_enqueue_ep0_command(uint8_t cmd) {
    uint8_t w = __atomic_load_n(&ep0_cmd_w, __ATOMIC_ACQUIRE);
    uint8_t r = __atomic_load_n(&ep0_cmd_r, __ATOMIC_ACQUIRE);
    uint8_t next = (uint8_t)((w + 1u) % (uint8_t)sizeof(ep0_cmd_queue));
    if (next == r) {
        return false;
    }
    ep0_cmd_queue[w] = cmd;
    __atomic_store_n(&ep0_cmd_w, next, __ATOMIC_RELEASE);
    return true;
}

static bool cdc_ctrl_write(const char *buf, size_t len) {
    if (!tud_cdc_n_connected(CDC_CTRL) || len == 0) {
        return false;
    }

    absolute_time_t deadline = make_timeout_time_ms(20);
    size_t offset = 0;
    while (offset < len && tud_cdc_n_connected(CDC_CTRL)) {
        int avail = tud_cdc_n_write_available(CDC_CTRL);
        if (avail <= 0) {
            tud_task();
            if (absolute_time_diff_us(get_absolute_time(), deadline) <= 0) {
                break;
            }
            sleep_us(200);
            continue;
        }
        size_t to_write = (size_t)avail;
        size_t remain = len - offset;
        if (to_write > remain) {
            to_write = remain;
        }
        uint32_t wrote = tud_cdc_n_write(CDC_CTRL, buf + offset, to_write);
        if (wrote == 0) {
            tud_task();
            if (absolute_time_diff_us(get_absolute_time(), deadline) <= 0) {
                break;
            }
            sleep_us(200);
            continue;
        }
        offset += wrote;
        tud_cdc_n_write_flush(CDC_CTRL);
    }
    return offset == len;
}

static void cdc_ctrl_printf(const char *fmt, ...) {
    if (!tud_cdc_n_connected(CDC_CTRL)) {
        return;
    }
    char buf[320];
    char out[360];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (len <= 0) {
        return;
    }
    size_t raw_len = (size_t)len;
    if (raw_len >= sizeof(buf)) {
        raw_len = sizeof(buf) - 1;
    }
    size_t out_len = 0;
    for (size_t i = 0; i < raw_len && out_len + 1 < sizeof(out); i++) {
        if (buf[i] == '\n') {
            if (out_len + 2 >= sizeof(out)) {
                break;
            }
            out[out_len++] = '\r';
        }
        out[out_len++] = buf[i];
    }
    if (out_len == 0) {
        return;
    }
    cdc_ctrl_write(out, out_len);
}

static bool try_send_probe_packet(void) {
    if (!stream_ready()) return false;

    if (probe_offset == 0) {
        stream_write_header(probe_buf, 0x55AAu, 0x1234u, CAP_BYTES_PER_LINE);
        memset(&probe_buf[STREAM_HEADER_BYTES], 0xA5, CAP_BYTES_PER_LINE);
    }

    while (probe_offset < APP_PKT_RAW_BYTES) {
        int avail = stream_write_available();
        if (avail <= 0) return false;
        uint32_t to_write = (uint32_t)avail;
        uint32_t remain = (uint32_t)(APP_PKT_RAW_BYTES - probe_offset);
        if (to_write > remain) {
            to_write = remain;
        }
        uint32_t wrote = stream_write(&probe_buf[probe_offset], to_write);
        if (wrote == 0) return false;
        probe_offset = (uint16_t)(probe_offset + wrote);
    }

    stream_flush();
    return true;
}

static inline void request_probe_packet(void) {
    probe_offset = 0;
    probe_pending = 1;
}

static void handle_reset_counters(void) {
    usb_drops = 0;
    video_core_set_want_frame(false);
    txq_offset = 0;
    core_bridge_send(CORE_BRIDGE_CMD_STOP_CAPTURE, 0);
    core_bridge_send(CORE_BRIDGE_CMD_RESET_COUNTERS, 0);
    if (can_emit_text()) {
        cdc_ctrl_printf("[EBD_IPKVM][cmd] reset counters\n");
    }
}

static void handle_capture_command(uint8_t ch) {
    switch (ch) {
    case 'S':
        video_core_set_armed(true);
        break;
    case 'X':
        video_core_set_armed(false);
        video_core_set_want_frame(false);
        txq_offset = 0;
        core_bridge_send(CORE_BRIDGE_CMD_STOP_CAPTURE, 0);
        if (can_emit_text()) {
            cdc_ctrl_printf("[EBD_IPKVM][cmd] armed=0 (host stop)\n");
        }
        break;
    case 'Q':
        video_core_set_armed(false);
        video_core_set_want_frame(false);
        txq_offset = 0;
        core_bridge_send(CORE_BRIDGE_CMD_STOP_CAPTURE, 0);
        if (can_emit_text()) {
            cdc_ctrl_printf("[EBD_IPKVM][cmd] parked\n");
        }
        while (true) { tud_task(); sleep_ms(50); }
        break;
    case 'U':
        request_probe_packet();
        break;
    case 'E':
        video_core_set_tx_rle_enabled(true);
        if (can_emit_text()) {
            cdc_ctrl_printf("[EBD_IPKVM][cmd] rle=on\n");
        }
        break;
    case 'e':
        video_core_set_tx_rle_enabled(false);
        if (can_emit_text()) {
            cdc_ctrl_printf("[EBD_IPKVM][cmd] rle=off\n");
        }
        break;
    default:
        break;
    }
}

static void handle_ep0_command(uint8_t cmd) {
    switch (cmd) {
    case USB_CTRL_REQ_CAPTURE_START:
        handle_capture_command('S');
        break;
    case USB_CTRL_REQ_CAPTURE_STOP:
        handle_capture_command('X');
        break;
    case USB_CTRL_REQ_CAPTURE_PARK:
        handle_capture_command('Q');
        break;
    case USB_CTRL_REQ_RESET_COUNTERS:
        handle_reset_counters();
        break;
    case USB_CTRL_REQ_PROBE_PACKET:
        handle_capture_command('U');
        break;
    case USB_CTRL_REQ_RLE_ON:
        handle_capture_command('E');
        break;
    case USB_CTRL_REQ_RLE_OFF:
        handle_capture_command('e');
        break;
    default:
        break;
    }
}

static void service_ep0_commands(void) {
    while (true) {
        uint8_t r = __atomic_load_n(&ep0_cmd_r, __ATOMIC_ACQUIRE);
        uint8_t w = __atomic_load_n(&ep0_cmd_w, __ATOMIC_ACQUIRE);
        if (r == w) {
            break;
        }
        uint8_t cmd = ep0_cmd_queue[r];
        uint8_t next = (uint8_t)((r + 1u) % (uint8_t)sizeof(ep0_cmd_queue));
        __atomic_store_n(&ep0_cmd_r, next, __ATOMIC_RELEASE);
        handle_ep0_command(cmd);
    }
}

static void emit_debug_state(void) {
    uint16_t txq_r = 0;
    uint16_t txq_w = 0;
    adb_core_stats_t adb_stats = {0};
    adb_bus_stats_t adb_bus_stats = {0};
    video_core_get_txq_indices(&txq_r, &txq_w);
    adb_core_get_stats(&adb_stats);
    adb_bus_get_stats(&adb_bus_stats);

    cdc_ctrl_printf("[EBD_IPKVM][debug] armed=%d capture=%d probe=%d\n",
                    video_core_is_armed() ? 1 : 0,
                    video_core_capture_enabled() ? 1 : 0,
                    __atomic_load_n(&probe_pending, __ATOMIC_ACQUIRE) ? 1 : 0);
    cdc_ctrl_printf("[EBD_IPKVM][debug] txq=%u/%u stream_avail=%d frames=%lu lines=%lu drops=%lu overruns=%lu shorts=%lu\n",
                    (unsigned)txq_r,
                    (unsigned)txq_w,
                    stream_write_available(),
                    (unsigned long)video_core_get_frames_done(),
                    (unsigned long)video_core_get_lines_ok(),
                    (unsigned long)video_core_get_lines_drop(),
                    (unsigned long)video_core_get_frame_overrun(),
                    (unsigned long)video_core_get_frame_short());
    cdc_ctrl_printf("[EBD_IPKVM][debug] adb pending=%lu key=%lu mouse=%lu drop=%lu lock=%lu coll=%lu\n",
                    (unsigned long)adb_stats.pending,
                    (unsigned long)adb_stats.key_events,
                    (unsigned long)adb_stats.mouse_events,
                    (unsigned long)adb_stats.drops,
                    (unsigned long)adb_bus_stats.lock_fails,
                    (unsigned long)adb_bus_stats.collisions);
    cdc_ctrl_printf("[EBD_IPKVM][debug] adb attention=%lu short=%lu reset=%lu abort=%lu error=%lu abort_t=%lu\n",
                    (unsigned long)adb_bus_stats.attentions,
                    (unsigned long)adb_bus_stats.attention_short,
                    (unsigned long)adb_bus_stats.resets,
                    (unsigned long)adb_bus_stats.aborts,
                    (unsigned long)adb_bus_stats.errors,
                    (unsigned long)adb_bus_stats.abort_time);
    cdc_ctrl_printf("[EBD_IPKVM][debug] adb talk_empty=%lu talk_bytes=%lu\n",
                    (unsigned long)adb_bus_stats.talk_empty,
                    (unsigned long)adb_bus_stats.talk_bytes);
    cdc_ctrl_printf("[EBD_IPKVM][debug] adb reg0=%lu srq_set=%lu srq_clr=%lu srq_sup=%lu gate=%lu gate_hi=%lu rise=%lu\n",
                    (unsigned long)adb_bus_stats.reg0_fills,
                    (unsigned long)adb_bus_stats.srq_sets,
                    (unsigned long)adb_bus_stats.srq_clears,
                    (unsigned long)adb_bus_stats.srq_suppressed,
                    (unsigned long)adb_bus_stats.rx_gate_armed,
                    (unsigned long)adb_bus_stats.rx_gate_immediate,
                    (unsigned long)adb_bus_stats.gpio_rise_events);
}

static void emit_status_state(uint32_t per_s,
                              uint32_t total_lines,
                              uint32_t frames_done,
                              uint32_t drops,
                              uint32_t usb_drop_count,
                              uint32_t overruns,
                              uint32_t vsync_edges,
                              uint32_t core0_pct,
                              uint32_t core1_pct) {
    cdc_ctrl_printf("[EBD_IPKVM][status] armed=%d capture=%d ps_on=%d lines/s=%lu total_lines=%lu frames=%lu drops=%lu usb_drops=%lu overruns=%lu vsync/s=%lu core0=%lu%% core1=%lu%%\n",
                    video_core_is_armed() ? 1 : 0,
                    video_core_capture_enabled() ? 1 : 0,
                    ps_on_state ? 1 : 0,
                    (unsigned long)per_s,
                    (unsigned long)total_lines,
                    (unsigned long)frames_done,
                    (unsigned long)drops,
                    (unsigned long)usb_drop_count,
                    (unsigned long)overruns,
                    (unsigned long)vsync_edges,
                    (unsigned long)core0_pct,
                    (unsigned long)core1_pct);
}

static bool poll_cdc_commands(void) {
    // Reads single-byte commands: R reset, P/p power, B/Z reset, I debug
    bool did_work = false;
    while (tud_cdc_n_available(CDC_CTRL)) {
        uint8_t ch;
        if (tud_cdc_n_read(CDC_CTRL, &ch, 1) != 1) break;
        did_work = true;

        if (ch == 'R' || ch == 'r') {
            handle_reset_counters();
        } else if (ch == 'P') {
            set_ps_on(true);
            if (can_emit_text()) {
                cdc_ctrl_printf("[EBD_IPKVM][cmd] ps_on=1\n");
            }
        } else if (ch == 'p') {
            set_ps_on(false);
            if (can_emit_text()) {
                cdc_ctrl_printf("[EBD_IPKVM][cmd] ps_on=0\n");
            }
        } else if (ch == 'B' || ch == 'b') {
            video_core_set_armed(false);
            video_core_set_want_frame(false);
            txq_offset = 0;
            core_bridge_send(CORE_BRIDGE_CMD_STOP_CAPTURE, 0);
            sleep_ms(10);
            reset_usb_boot(0, 0);
        } else if (ch == 'Z' || ch == 'z') {
            video_core_set_armed(false);
            video_core_set_want_frame(false);
            txq_offset = 0;
            core_bridge_send(CORE_BRIDGE_CMD_STOP_CAPTURE, 0);
            sleep_ms(10);
            watchdog_reboot(0, 0, 0);
            while (true) { tight_loop_contents(); }
        } else if (ch == 'I' || ch == 'i') {
            debug_requested = true;
        }
    }
    return did_work;
}

static void adb_enqueue_key(uint8_t code, bool down) {
    adb_event_t event = {
        .type = ADB_EVENT_KEY,
        .data.key = {
            .code = code,
            .down = down,
        },
    };
    adb_queue_push(&event);
}

static bool adb_ascii_to_keycode(uint8_t ch, uint8_t *out_code) {
    uint8_t lower = ch;
    if (ch >= 'A' && ch <= 'Z') {
        lower = (uint8_t)(ch - 'A' + 'a');
    }

    switch (lower) {
    case 'a': *out_code = 0x00; return true;
    case 's': *out_code = 0x01; return true;
    case 'd': *out_code = 0x02; return true;
    case 'f': *out_code = 0x03; return true;
    case 'h': *out_code = 0x04; return true;
    case 'g': *out_code = 0x05; return true;
    case 'z': *out_code = 0x06; return true;
    case 'x': *out_code = 0x07; return true;
    case 'c': *out_code = 0x08; return true;
    case 'v': *out_code = 0x09; return true;
    case 'b': *out_code = 0x0B; return true;
    case 'q': *out_code = 0x0C; return true;
    case 'w': *out_code = 0x0D; return true;
    case 'e': *out_code = 0x0E; return true;
    case 'r': *out_code = 0x0F; return true;
    case 'y': *out_code = 0x10; return true;
    case 't': *out_code = 0x11; return true;
    case '1': *out_code = 0x12; return true;
    case '2': *out_code = 0x13; return true;
    case '3': *out_code = 0x14; return true;
    case '4': *out_code = 0x15; return true;
    case '6': *out_code = 0x16; return true;
    case '5': *out_code = 0x17; return true;
    case '=': *out_code = 0x18; return true;
    case '9': *out_code = 0x19; return true;
    case '7': *out_code = 0x1A; return true;
    case '-': *out_code = 0x1B; return true;
    case '8': *out_code = 0x1C; return true;
    case '0': *out_code = 0x1D; return true;
    case ']': *out_code = 0x1E; return true;
    case 'o': *out_code = 0x1F; return true;
    case 'u': *out_code = 0x20; return true;
    case '[': *out_code = 0x21; return true;
    case 'i': *out_code = 0x22; return true;
    case 'p': *out_code = 0x23; return true;
    case '\r': *out_code = 0x24; return true;
    case 'l': *out_code = 0x25; return true;
    case 'j': *out_code = 0x26; return true;
    case '\'': *out_code = 0x27; return true;
    case 'k': *out_code = 0x28; return true;
    case ';': *out_code = 0x29; return true;
    case '\\': *out_code = 0x2A; return true;
    case ',': *out_code = 0x2B; return true;
    case '/': *out_code = 0x2C; return true;
    case 'n': *out_code = 0x2D; return true;
    case 'm': *out_code = 0x2E; return true;
    case '.': *out_code = 0x2F; return true;
    case '\t': *out_code = 0x30; return true;
    case ' ': *out_code = 0x31; return true;
    case '`': *out_code = 0x32; return true;
    case 0x08:
    case 0x7F:
        *out_code = 0x33;
        return true;
    case 0x1B:
        *out_code = 0x35;
        return true;
    default:
        return false;
    }
}

static void adb_enqueue_mouse(int8_t dx, int8_t dy) {
    adb_event_t event = {
        .type = ADB_EVENT_MOUSE,
        .data.mouse = {
            .dx = dx,
            .dy = dy,
            .buttons = adb_mouse_buttons,
        },
    };
    adb_queue_push(&event);
}

static bool poll_cdc_adb(void) {
    bool did_work = false;
    const int8_t step = 4;

    while (tud_cdc_n_available(CDC_ADB)) {
        uint8_t ch;
        if (tud_cdc_n_read(CDC_ADB, &ch, 1) != 1) {
            break;
        }
        did_work = true;

        if (adb_esc_state == 1) {
            if (ch == '[') {
                adb_esc_state = 2;
                continue;
            }
            adb_esc_state = 0;
        } else if (adb_esc_state == 2) {
            switch (ch) {
            case 'A':
                adb_enqueue_mouse(0, -step);
                break;
            case 'B':
                adb_enqueue_mouse(0, step);
                break;
            case 'C':
                adb_enqueue_mouse(step, 0);
                break;
            case 'D':
                adb_enqueue_mouse(-step, 0);
                break;
            default:
                break;
            }
            adb_esc_state = 0;
            continue;
        }

        if (ch == 0x1B) {
            adb_esc_state = 1;
            continue;
        }

        if (ch == 0x12) {
            adb_mouse_buttons ^= ADB_MOUSE_BUTTON_PRIMARY;
            adb_enqueue_mouse(0, 0);
            continue;
        }

        if (ch == '\r' || ch == '\n') {
            uint8_t code = 0;
            if (adb_ascii_to_keycode('\r', &code)) {
                adb_enqueue_key(code, true);
                adb_enqueue_key(code, false);
            }
            continue;
        }

        if (ch == '\t' || ch == 0x08 || ch == 0x7F || (ch >= 0x20 && ch <= 0x7E)) {
            uint8_t code = 0;
            if (adb_ascii_to_keycode(ch, &code)) {
                adb_enqueue_key(code, true);
                adb_enqueue_key(code, false);
            }
        }
    }

    return did_work;
}

static inline bool service_txq(void) {
    if (!stream_ready()) return false;

    bool wrote_any = false;

    while (true) {
        const uint8_t *data = NULL;
        uint16_t pkt_len = 0;
        if (!video_core_txq_peek(&data, &pkt_len)) {
            break;
        }

        if (pkt_len == 0 || pkt_len > APP_PKT_MAX_BYTES) {
            txq_offset = 0;
            video_core_txq_consume();
            continue;
        }

        int avail = stream_write_available();
        if (avail <= 0) break;

        uint32_t remain = (uint32_t)(pkt_len - txq_offset);
        uint32_t to_write = (uint32_t)avail;
        if (to_write > remain) {
            to_write = remain;
        }

        uint32_t n = stream_write(&data[txq_offset], to_write);
        if (n == 0) {
            usb_drops++;
            break;
        }

        txq_offset = (uint16_t)(txq_offset + n);
        wrote_any = true;

        if (txq_offset >= pkt_len) {
            txq_offset = 0;
            video_core_txq_consume();
        }
    }

    if (wrote_any) {
        stream_flush();
    }
    return wrote_any;
}

void app_core_init(const app_core_config_t *cfg) {
    app_cfg = *cfg;

    cdc_ctrl_printf("\n[EBD_IPKVM][info] USB packet stream @ ~60fps (continuous mode)\n");
    cdc_ctrl_printf("[EBD_IPKVM][info] BULK0=video stream, CDC1=control/status\n");
    cdc_ctrl_printf("[EBD_IPKVM][info] Capture control now uses EP0 vendor requests (see docs).\n");
    cdc_ctrl_printf("[EBD_IPKVM][info] CDC1: 'R' reset counters, 'I' debug, 'P'/'p' power, 'B' BOOTSEL, 'Z' reset.\n");
    cdc_ctrl_printf("[EBD_IPKVM][info] CDC2: ADB test input (arrow keys, Ctrl+R click, ASCII text).\n");

    status_next = make_timeout_time_ms(1000);
    status_last_lines = 0;
    adb_rx_next = get_absolute_time();
    adb_rx_seen = false;
}

void app_core_poll(void) {
    uint32_t loop_start = time_us_32();
    uint32_t active_us = 0;
    tud_task();
    service_ep0_commands();
    uint32_t active_start = time_us_32();
    bool did_work = poll_cdc_commands();
    if (did_work) {
        active_us += (uint32_t)(time_us_32() - active_start);
    }
    active_start = time_us_32();
    bool did_adb = poll_cdc_adb();
    if (did_adb) {
        active_us += (uint32_t)(time_us_32() - active_start);
    }

    if (adb_bus_take_activity()) {
        adb_rx_seen = true;
    }

    /* Send queued binary packets from thread context (NOT IRQ). */
    if (probe_pending) {
        active_start = time_us_32();
        if (try_send_probe_packet()) {
            probe_pending = 0;
            active_us += (uint32_t)(time_us_32() - active_start);
        }
    }
    if (!tud_cdc_n_connected(CDC_CTRL)) {
        debug_requested = false;
    }
    if (debug_requested && can_emit_text()) {
        active_start = time_us_32();
        emit_debug_state();
        debug_requested = false;
        active_us += (uint32_t)(time_us_32() - active_start);
    }
    active_start = time_us_32();
    if (service_txq()) {
        active_us += (uint32_t)(time_us_32() - active_start);
    }

    if (can_emit_text() && !debug_requested) {
        if (adb_rx_seen && absolute_time_diff_us(get_absolute_time(), adb_rx_next) <= 0) {
            adb_rx_seen = false;
            adb_rx_next = delayed_by_ms(get_absolute_time(), 2000);
            cdc_ctrl_printf("[EBD_IPKVM][adb] rx seen\n");
        }

        if (absolute_time_diff_us(get_absolute_time(), status_next) <= 0) {
            status_next = delayed_by_ms(status_next, 1000);

            uint32_t l = video_core_get_lines_ok();
            uint32_t per_s = l - status_last_lines;
            status_last_lines = l;

            uint32_t ve = video_core_take_vsync_edges();
            uint32_t core1_busy = 0;
            uint32_t core1_total = 0;
            uint32_t core0_busy = 0;
            uint32_t core0_total = 0;
            video_core_take_core1_utilization(&core1_busy, &core1_total);
            take_core0_utilization(&core0_busy, &core0_total);
            uint32_t core1_pct = core1_total ? (uint32_t)((core1_busy * 100u) / core1_total) : 0;
            uint32_t core0_pct = core0_total ? (uint32_t)((core0_busy * 100u) / core0_total) : 0;

            emit_status_state(per_s,
                              l,
                              video_core_get_frames_done(),
                              video_core_get_lines_drop(),
                              usb_drops,
                              video_core_get_frame_overrun(),
                              ve,
                              core0_pct,
                              core1_pct);
        }
    }

    tight_loop_contents();
    uint32_t loop_end = time_us_32();
    core0_busy_us += active_us;
    core0_total_us += (uint32_t)(loop_end - loop_start);
}
