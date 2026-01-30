#include "app_core.h"

#include <stdio.h>
#include <string.h>

#include "pico/bootrom.h"
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/watchdog.h"
#include "tusb.h"

#include "core_bridge.h"
#include "stream_protocol.h"
#include "video_capture.h"
#include "video_core.h"

#define APP_PKT_MAX_PAYLOAD (CAP_BYTES_PER_LINE * 2)
#define APP_PKT_MAX_BYTES (STREAM_HEADER_BYTES + APP_PKT_MAX_PAYLOAD)
#define APP_PKT_RAW_BYTES (STREAM_HEADER_BYTES + CAP_BYTES_PER_LINE)

static app_core_config_t app_cfg;

static volatile bool ps_on_state = false;
static uint32_t usb_drops = 0;
static uint16_t txq_offset = 0;

static uint8_t probe_buf[APP_PKT_MAX_BYTES];
static volatile uint8_t probe_pending = 0;
static uint16_t probe_offset = 0;
static volatile bool debug_requested = false;

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
    return video_core_can_emit_text();
}

static inline void reset_diag_counts(void) {
    diag_pixclk_edges = 0;
    diag_hsync_edges = 0;
    diag_vsync_edges = 0;
    diag_video_edges = 0;
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

    printf("[EBD_IPKVM] gpio diag: pixclk=%d hsync=%d vsync=%d video=%d edges/%.2fs pixclk=%lu hsync=%lu vsync=%lu video=%lu\n",
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
    if (!tud_cdc_connected()) return false;

    if (probe_offset == 0) {
        stream_write_header(probe_buf, 0x55AAu, 0x1234u, CAP_BYTES_PER_LINE);
        memset(&probe_buf[STREAM_HEADER_BYTES], 0xA5, CAP_BYTES_PER_LINE);
    }

    while (probe_offset < APP_PKT_RAW_BYTES) {
        int avail = tud_cdc_write_available();
        if (avail <= 0) return false;
        uint32_t to_write = (uint32_t)avail;
        uint32_t remain = (uint32_t)(APP_PKT_RAW_BYTES - probe_offset);
        if (to_write > remain) {
            to_write = remain;
        }
        uint32_t wrote = tud_cdc_write(&probe_buf[probe_offset], to_write);
        if (wrote == 0) return false;
        probe_offset = (uint16_t)(probe_offset + wrote);
    }

    tud_cdc_write_flush();
    return true;
}

static inline void request_probe_packet(void) {
    probe_offset = 0;
    probe_pending = 1;
}

static void emit_debug_state(void) {
    if (!tud_cdc_connected()) return;

    uint16_t txq_r = 0;
    uint16_t txq_w = 0;
    video_core_get_txq_indices(&txq_r, &txq_w);

    printf("[EBD_IPKVM] dbg armed=%d cap=%d test=%d probe=%d vsync=%s txq_r=%u txq_w=%u write_avail=%d frames=%lu lines=%lu drops=%lu usb=%lu frame_overrun=%lu short=%lu\n",
           video_core_is_armed() ? 1 : 0,
           video_core_capture_enabled() ? 1 : 0,
           video_core_test_frame_active() ? 1 : 0,
           __atomic_load_n(&probe_pending, __ATOMIC_ACQUIRE) ? 1 : 0,
           video_core_get_vsync_edge() ? "fall" : "rise",
           (unsigned)txq_r,
           (unsigned)txq_w,
           tud_cdc_write_available(),
           (unsigned long)video_core_get_frames_done(),
           (unsigned long)video_core_get_lines_ok(),
           (unsigned long)video_core_get_lines_drop(),
           (unsigned long)usb_drops,
           (unsigned long)video_core_get_frame_overrun(),
           (unsigned long)video_core_get_frame_short());
}

static void poll_cdc_commands(void) {
    // Reads single-byte commands: S=start, X=stop, R=reset counters, Q=park
    while (tud_cdc_available()) {
        uint8_t ch;
        if (tud_cdc_read(&ch, 1) != 1) break;

        if (ch == 'S' || ch == 's') {
            video_core_set_armed(true);
            /* IMPORTANT: do NOT printf here; keep binary stream clean once armed */
        } else if (ch == 'X' || ch == 'x') {
            video_core_set_armed(false);
            video_core_set_want_frame(false);
            txq_offset = 0;
            core_bridge_send(CORE_BRIDGE_CMD_STOP_CAPTURE, 0);
            if (can_emit_text()) {
                printf("[EBD_IPKVM] armed=0 (host stop)\n");
            }
        } else if (ch == 'R' || ch == 'r') {
            usb_drops = 0;
            video_core_set_take_toggle(false);
            video_core_set_want_frame(false);
            txq_offset = 0;
            core_bridge_send(CORE_BRIDGE_CMD_STOP_CAPTURE, 0);
            core_bridge_send(CORE_BRIDGE_CMD_RESET_COUNTERS, 0);
            if (can_emit_text()) {
                printf("[EBD_IPKVM] reset counters\n");
            }
        } else if (ch == 'Q' || ch == 'q') {
            video_core_set_armed(false);
            video_core_set_want_frame(false);
            txq_offset = 0;
            core_bridge_send(CORE_BRIDGE_CMD_STOP_CAPTURE, 0);
            if (can_emit_text()) {
                printf("[EBD_IPKVM] parked\n");
            }
            while (true) { tud_task(); sleep_ms(50); }
        } else if (ch == 'P') {
            set_ps_on(true);
            if (can_emit_text()) {
                printf("[EBD_IPKVM] ps_on=1\n");
            }
        } else if (ch == 'p') {
            set_ps_on(false);
            if (can_emit_text()) {
                printf("[EBD_IPKVM] ps_on=0\n");
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
                printf("[EBD_IPKVM] rle=on\n");
            }
        } else if (ch == 'e') {
            video_core_set_tx_rle_enabled(false);
            if (can_emit_text()) {
                printf("[EBD_IPKVM] rle=off\n");
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
                printf("[EBD_IPKVM] vsync_edge=%s\n", new_edge ? "fall" : "rise");
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
                printf("[EBD_IPKVM] mode=%s\n",
                       next_mode == CAPTURE_MODE_CONTINUOUS_60FPS ? "60fps-continuous"
                                                                 : "30fps-test");
            }
        }
    }
}

static inline void service_txq(void) {
    if (!tud_cdc_connected()) return;

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

        int avail = tud_cdc_write_available();
        if (avail <= 0) break;

        uint32_t remain = (uint32_t)(pkt_len - txq_offset);
        uint32_t to_write = (uint32_t)avail;
        if (to_write > remain) {
            to_write = remain;
        }

        uint32_t n = tud_cdc_write(&data[txq_offset], to_write);
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
        tud_cdc_write_flush();
    }
}

void app_core_init(const app_core_config_t *cfg) {
    app_cfg = *cfg;

    printf("\n[EBD_IPKVM] USB packet stream @ ~60fps (continuous mode)\n");
    printf("[EBD_IPKVM] WAITING for host. Send 'S' to start, 'X' stop, 'R' reset.\n");
    printf("[EBD_IPKVM] Power/control: 'P' on, 'p' off, 'B' BOOTSEL, 'Z' reset.\n");
    printf("[EBD_IPKVM] GPIO diag: send 'G' for pin states + edge counts.\n");
    printf("[EBD_IPKVM] Edge toggles: 'V' VSYNC edge. Mode toggle: 'M' 30fps↔60fps.\n");

    status_next = make_timeout_time_ms(1000);
    status_last_lines = 0;
}

void app_core_poll(void) {
    tud_task();
    poll_cdc_commands();

    /* Send queued binary packets from thread context (NOT IRQ). */
    if (probe_pending && try_send_probe_packet()) {
        probe_pending = 0;
    }
    if (debug_requested && can_emit_text()) {
        debug_requested = false;
        emit_debug_state();
    }
    service_txq();

    /* Keep status text off the wire while armed/capturing or while TX queue not empty. */
    if (can_emit_text()) {
        if (absolute_time_diff_us(get_absolute_time(), status_next) <= 0) {
            status_next = delayed_by_ms(status_next, 1000);

            uint32_t l = video_core_get_lines_ok();
            uint32_t per_s = l - status_last_lines;
            status_last_lines = l;

            uint32_t ve = video_core_take_vsync_edges();

            printf("[EBD_IPKVM] armed=%d cap=%d ps_on=%d lines/s=%lu total=%lu q_drops=%lu usb_drops=%lu frame_overrun=%lu vsync_edges/s=%lu frames=%lu\n",
                   video_core_is_armed() ? 1 : 0,
                   video_core_capture_enabled() ? 1 : 0,
                   ps_on_state ? 1 : 0,
                   (unsigned long)per_s,
                   (unsigned long)l,
                   (unsigned long)video_core_get_lines_drop(),
                   (unsigned long)usb_drops,
                   (unsigned long)video_core_get_frame_overrun(),
                   (unsigned long)ve,
                   (unsigned long)video_core_get_frames_done());
        }
    }

    tight_loop_contents();
}
