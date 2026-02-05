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
#include "usb_control.h"
#include "video_capture.h"
#include "video_core.h"

#define APP_PKT_MAX_PAYLOAD (CAP_BYTES_PER_LINE * 2)
#define APP_PKT_MAX_BYTES (STREAM_HEADER_BYTES + APP_PKT_MAX_PAYLOAD)
#define APP_PKT_RAW_BYTES (STREAM_HEADER_BYTES + CAP_BYTES_PER_LINE)

#define CDC_CTRL 0

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

static volatile uint32_t diag_pixclk_edges = 0;
static volatile uint32_t diag_hsync_edges = 0;
static volatile uint32_t diag_vsync_edges = 0;
static volatile uint32_t diag_video_edges = 0;

static absolute_time_t status_next;
static uint32_t status_last_lines = 0;

static inline void set_ps_on(bool on) {
    ps_on_state = on;
    gpio_put(app_cfg.pin_ps_on, on ? 1 : 0);
}

static inline bool can_emit_text(void) {
    return tud_cdc_n_connected(CDC_CTRL);
}

static inline void reset_diag_counts(void) {
    diag_pixclk_edges = 0;
    diag_hsync_edges = 0;
    diag_vsync_edges = 0;
    diag_video_edges = 0;
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

static void cdc_ctrl_write(const char *buf, size_t len) {
    if (!tud_cdc_n_connected(CDC_CTRL) || len == 0) {
        return;
    }

    int avail = tud_cdc_n_write_available(CDC_CTRL);
    if (avail < (int)len) {
        return;
    }

    uint32_t wrote = tud_cdc_n_write(CDC_CTRL, buf, len);
    if (wrote == len) {
        tud_cdc_n_write_flush(CDC_CTRL);
    }
}

static void cdc_ctrl_printf(const char *fmt, ...) {
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

static inline void diag_accumulate_edges(bool pixclk, bool hsync, bool vsync, bool video,
                                         bool *prev_pixclk, bool *prev_hsync,
                                         bool *prev_vsync, bool *prev_video) {
    if (pixclk != *prev_pixclk) diag_pixclk_edges++;
    if (hsync != *prev_hsync) diag_hsync_edges++;
    if (vsync != *prev_vsync) diag_vsync_edges++;
    if (video != *prev_video) diag_video_edges++;
    *prev_pixclk = pixclk;
    *prev_hsync = hsync;
    *prev_vsync = vsync;
    *prev_video = video;
}

static void run_gpio_diag(void) {
    const uint32_t diag_ms = 500;

    gpio_set_function(app_cfg.pin_pixclk, GPIO_FUNC_SIO);
    gpio_set_function(app_cfg.pin_hsync, GPIO_FUNC_SIO);
    gpio_set_function(app_cfg.pin_video, GPIO_FUNC_SIO);

    reset_diag_counts();

    bool prev_pixclk = gpio_get(app_cfg.pin_pixclk);
    bool prev_hsync = gpio_get(app_cfg.pin_hsync);
    bool prev_vsync = gpio_get(app_cfg.pin_vsync);
    bool prev_video = gpio_get(app_cfg.pin_video);

    absolute_time_t end = make_timeout_time_ms(diag_ms);
    while (absolute_time_diff_us(get_absolute_time(), end) > 0) {
        tud_task();
        bool pixclk = gpio_get(app_cfg.pin_pixclk);
        bool hsync = gpio_get(app_cfg.pin_hsync);
        bool vsync = gpio_get(app_cfg.pin_vsync);
        bool video = gpio_get(app_cfg.pin_video);
        diag_accumulate_edges(pixclk, hsync, vsync, video,
                              &prev_pixclk, &prev_hsync, &prev_vsync, &prev_video);
        sleep_us(2);
        tight_loop_contents();
    }

    bool pixclk = gpio_get(app_cfg.pin_pixclk);
    bool hsync = gpio_get(app_cfg.pin_hsync);
    bool vsync = gpio_get(app_cfg.pin_vsync);
    bool video = gpio_get(app_cfg.pin_video);

    gpio_set_function(app_cfg.pin_pixclk, GPIO_FUNC_PIO0);
    gpio_set_function(app_cfg.pin_hsync, GPIO_FUNC_PIO0);
    gpio_set_function(app_cfg.pin_video, GPIO_FUNC_PIO0);

    cdc_ctrl_printf("[EBD_IPKVM] gpio diag: pixclk=%d hsync=%d vsync=%d video=%d edges/%.2fs pixclk=%lu hsync=%lu vsync=%lu video=%lu\n",
                    pixclk ? 1 : 0,
                    hsync ? 1 : 0,
                    vsync ? 1 : 0,
                    video ? 1 : 0,
                    diag_ms / 1000.0,
                    (unsigned long)diag_pixclk_edges,
                    (unsigned long)diag_hsync_edges,
                    (unsigned long)diag_vsync_edges,
                    (unsigned long)diag_video_edges);
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

static void emit_debug_state(void) {
    if (!tud_cdc_n_connected(CDC_CTRL)) return;

    uint16_t txq_r = 0;
    uint16_t txq_w = 0;
    video_core_get_txq_indices(&txq_r, &txq_w);

    cdc_ctrl_printf("[EBD_IPKVM] dbg a=%d cap=%d test=%d probe=%d vs=%s\n",
                    video_core_is_armed() ? 1 : 0,
                    video_core_capture_enabled() ? 1 : 0,
                    video_core_test_frame_active() ? 1 : 0,
                    __atomic_load_n(&probe_pending, __ATOMIC_ACQUIRE) ? 1 : 0,
                    video_core_get_vsync_edge() ? "fall" : "rise");
    cdc_ctrl_printf("[EBD_IPKVM] dbg txq=%u/%u av=%d fr=%lu ln=%lu dr=%lu ov=%lu sh=%lu\n",
                    (unsigned)txq_r,
                    (unsigned)txq_w,
                    stream_write_available(),
                    (unsigned long)video_core_get_frames_done(),
                    (unsigned long)video_core_get_lines_ok(),
                    (unsigned long)video_core_get_lines_drop(),
                    (unsigned long)video_core_get_frame_overrun(),
                    (unsigned long)video_core_get_frame_short());
}

static void handle_capture_start(void) {
    video_core_set_armed(true);
}

static void handle_capture_stop(void) {
    video_core_set_armed(false);
    video_core_set_want_frame(false);
    txq_offset = 0;
    core_bridge_send(CORE_BRIDGE_CMD_STOP_CAPTURE, 0);
    if (can_emit_text()) {
        cdc_ctrl_printf("[EBD_IPKVM][cmd] armed=0 (stop)\n");
    }
}

static void handle_capture_park(void) {
    video_core_set_armed(false);
    video_core_set_want_frame(false);
    txq_offset = 0;
    core_bridge_send(CORE_BRIDGE_CMD_STOP_CAPTURE, 0);
    if (can_emit_text()) {
        cdc_ctrl_printf("[EBD_IPKVM][cmd] parked\n");
    }
    while (true) { tud_task(); sleep_ms(50); }
}

static void handle_reset_counters(void) {
    usb_drops = 0;
    video_core_set_take_toggle(false);
    video_core_set_want_frame(false);
    txq_offset = 0;
    core_bridge_send(CORE_BRIDGE_CMD_STOP_CAPTURE, 0);
    core_bridge_send(CORE_BRIDGE_CMD_RESET_COUNTERS, 0);
    if (can_emit_text()) {
        cdc_ctrl_printf("[EBD_IPKVM][cmd] reset counters\n");
    }
}

static void handle_probe_packet(void) {
    request_probe_packet();
}

static void handle_rle_on(void) {
    video_core_set_tx_rle_enabled(true);
    if (can_emit_text()) {
        cdc_ctrl_printf("[EBD_IPKVM][cmd] rle=on\n");
    }
}

static void handle_rle_off(void) {
    video_core_set_tx_rle_enabled(false);
    if (can_emit_text()) {
        cdc_ctrl_printf("[EBD_IPKVM][cmd] rle=off\n");
    }
}

static void handle_ep0_command(uint8_t cmd) {
    switch (cmd) {
    case USB_CTRL_REQ_CAPTURE_START:
        handle_capture_start();
        break;
    case USB_CTRL_REQ_CAPTURE_STOP:
        handle_capture_stop();
        break;
    case USB_CTRL_REQ_CAPTURE_PARK:
        handle_capture_park();
        break;
    case USB_CTRL_REQ_RESET_COUNTERS:
        handle_reset_counters();
        break;
    case USB_CTRL_REQ_PROBE_PACKET:
        handle_probe_packet();
        break;
    case USB_CTRL_REQ_RLE_ON:
        handle_rle_on();
        break;
    case USB_CTRL_REQ_RLE_OFF:
        handle_rle_off();
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

static bool poll_cdc_commands(void) {
    // Reads single-byte commands: R=reset, P/p=power, B/Z=boot/reset, I=debug
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
                cdc_ctrl_printf("[EBD_IPKVM] ps_on=1\n");
            }
        } else if (ch == 'p') {
            set_ps_on(false);
            if (can_emit_text()) {
                cdc_ctrl_printf("[EBD_IPKVM] ps_on=0\n");
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
        } else if (ch == 'F' || ch == 'f') {
            core_bridge_send(CORE_BRIDGE_CMD_SINGLE_FRAME, 0);
        } else if (ch == 'T' || ch == 't') {
            video_core_set_armed(false);
            video_core_set_want_frame(false);
            txq_offset = 0;
            core_bridge_send(CORE_BRIDGE_CMD_START_TEST, 0);
            request_probe_packet();
        } else if (ch == 'U' || ch == 'u') {
            request_probe_packet();
        } else if (ch == 'I' || ch == 'i') {
            debug_requested = true;
        } else if (ch == 'E') {
            video_core_set_tx_rle_enabled(true);
            if (can_emit_text()) {
                cdc_ctrl_printf("[EBD_IPKVM] rle=on\n");
            }
        } else if (ch == 'e') {
            video_core_set_tx_rle_enabled(false);
            if (can_emit_text()) {
                cdc_ctrl_printf("[EBD_IPKVM] rle=off\n");
            }
        } else if (ch == 'G' || ch == 'g') {
            if (can_emit_text()) {
                core_bridge_send(CORE_BRIDGE_CMD_DIAG_PREP, 0);
                run_gpio_diag();
                core_bridge_send(CORE_BRIDGE_CMD_DIAG_DONE, 0);
            }
        } else if (ch == 'V' || ch == 'v') {
            bool new_edge = !video_core_get_vsync_edge();
            video_core_set_vsync_edge(new_edge);
            video_core_set_armed(false);
            video_core_set_want_frame(false);
            txq_offset = 0;
            core_bridge_send(CORE_BRIDGE_CMD_STOP_CAPTURE, 0);
            core_bridge_send(CORE_BRIDGE_CMD_CONFIG_VSYNC, 0);
            if (can_emit_text()) {
                cdc_ctrl_printf("[EBD_IPKVM] vsync_edge=%s\n", new_edge ? "fall" : "rise");
            }
        } else if (ch == 'M' || ch == 'm') {
            capture_mode_t mode = video_core_get_capture_mode();
            capture_mode_t next_mode = (mode == CAPTURE_MODE_TEST_30FPS)
                                           ? CAPTURE_MODE_CONTINUOUS_60FPS
                                           : CAPTURE_MODE_TEST_30FPS;
            video_core_set_capture_mode(next_mode);
            video_core_set_want_frame(false);
            video_core_set_take_toggle(false);
            txq_offset = 0;
            core_bridge_send(CORE_BRIDGE_CMD_STOP_CAPTURE, 0);
            if (can_emit_text()) {
                cdc_ctrl_printf("[EBD_IPKVM] mode=%s\n",
                                next_mode == CAPTURE_MODE_CONTINUOUS_60FPS ? "60fps-continuous"
                                                                          : "30fps-test");
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

    cdc_ctrl_printf("\n[EBD_IPKVM] USB packet stream @ ~60fps (continuous mode)\n");
    cdc_ctrl_printf("[EBD_IPKVM] BULK0=video stream, CDC0=control/status\n");
    cdc_ctrl_printf("[EBD_IPKVM] Capture control via EP0 vendor requests (use host tool).\n");
    cdc_ctrl_printf("[EBD_IPKVM] Power/control: 'P' on, 'p' off, 'B' BOOTSEL, 'Z' reset, 'R' reset counters.\n");
    cdc_ctrl_printf("[EBD_IPKVM] Debug: send 'I' for internal state.\n");

    status_next = make_timeout_time_ms(1000);
    status_last_lines = 0;
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

    if (probe_pending) {
        active_start = time_us_32();
        if (try_send_probe_packet()) {
            probe_pending = 0;
            active_us += (uint32_t)(time_us_32() - active_start);
        }
    }
    if (debug_requested && can_emit_text()) {
        active_start = time_us_32();
        debug_requested = false;
        emit_debug_state();
        active_us += (uint32_t)(time_us_32() - active_start);
    }
    active_start = time_us_32();
    if (service_txq()) {
        active_us += (uint32_t)(time_us_32() - active_start);
    }

    if (can_emit_text()) {
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

            cdc_ctrl_printf("[EBD_IPKVM] a=%d c=%d ps=%d l/s=%lu tot=%lu fr=%lu\n",
                            video_core_is_armed() ? 1 : 0,
                            video_core_capture_enabled() ? 1 : 0,
                            ps_on_state ? 1 : 0,
                            (unsigned long)per_s,
                            (unsigned long)l,
                            (unsigned long)video_core_get_frames_done());
            cdc_ctrl_printf("[EBD_IPKVM] dr=%lu usb=%lu ov=%lu vs/s=%lu c0=%lu%% c1=%lu%%\n",
                            (unsigned long)video_core_get_lines_drop(),
                            (unsigned long)usb_drops,
                            (unsigned long)video_core_get_frame_overrun(),
                            (unsigned long)ve,
                            (unsigned long)core0_pct,
                            (unsigned long)core1_pct);
        }
    }

    tight_loop_contents();
    uint32_t loop_end = time_us_32();
    core0_busy_us += active_us;
    core0_total_us += (uint32_t)(loop_end - loop_start);
}
