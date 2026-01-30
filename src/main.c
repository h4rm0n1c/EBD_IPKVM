#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/gpio.h"
#include "hardware/watchdog.h"
#include "pico/bootrom.h"

#include "tusb.h"
#include "classic_line.pio.h"
#include "video_capture.h"

#define PIN_PIXCLK 0
#define PIN_VSYNC  1   // active-low
#define PIN_HSYNC  2   // active-low
#define PIN_VIDEO  3
#define PIN_PS_ON  9   // via ULN2803, GPIO high asserts ATX PS_ON

#define BYTES_PER_LINE CAP_BYTES_PER_LINE
#define WORDS_PER_LINE CAP_WORDS_PER_LINE

#define PKT_HDR_BYTES 8
#define PKT_FLAG_RLE  0x8000u
#define PKT_LEN_MASK  0x7FFFu
#define PKT_MAX_PAYLOAD (BYTES_PER_LINE * 2)
#define PKT_MAX_BYTES (PKT_HDR_BYTES + PKT_MAX_PAYLOAD)
#define PKT_RAW_BYTES (PKT_HDR_BYTES + BYTES_PER_LINE)

/* TX queue: power-of-two depth so we can mask wrap. */
#define TXQ_DEPTH 512
#define TXQ_MASK  (TXQ_DEPTH - 1)

typedef enum {
    CAPTURE_MODE_TEST_30FPS = 0,
    CAPTURE_MODE_CONTINUOUS_60FPS = 1,
} capture_mode_t;

static volatile bool armed = false;            // host says start/stop
static volatile bool want_frame = false;       // transmit this frame or skip
static volatile bool take_toggle = false;      // every other frame => ~30fps
static volatile capture_mode_t capture_mode = CAPTURE_MODE_CONTINUOUS_60FPS;

static volatile uint16_t frame_id = 0;         // increments per transmitted frame
/* repurpose as: queue overflows (we couldn't enqueue a line packet fast enough) */
static volatile uint32_t lines_drop = 0;
/* repurpose as: usb send failures (no space / disconnected / short write) */
static volatile uint32_t usb_drops = 0;

static volatile uint32_t vsync_edges = 0;
static volatile uint32_t frames_done = 0;
static volatile bool ps_on_state = false;
static volatile uint32_t diag_pixclk_edges = 0;
static volatile uint32_t diag_hsync_edges = 0;
static volatile uint32_t diag_vsync_edges = 0;
static volatile uint32_t diag_video_edges = 0;
static volatile bool test_frame_active = false;
static volatile bool diag_active = false;
static volatile uint32_t last_vsync_us = 0;
static uint16_t test_line = 0;
static uint8_t test_line_buf[BYTES_PER_LINE];
static uint8_t probe_buf[PKT_MAX_BYTES];
static volatile uint8_t probe_pending = 0;
static uint16_t probe_offset = 0;
static volatile bool debug_requested = false;
static volatile bool tx_rle_enabled = true;

static uint32_t framebuf_a[CAP_MAX_LINES][CAP_WORDS_PER_LINE];
static uint32_t framebuf_b[CAP_MAX_LINES][CAP_WORDS_PER_LINE];
static video_capture_t capture = {0};
static uint32_t (*frame_tx_buf)[CAP_WORDS_PER_LINE] = NULL;
static uint16_t frame_tx_id = 0;
static uint16_t frame_tx_line = 0;
static uint16_t frame_tx_lines = 0;
static uint16_t frame_tx_start = 0;
static uint32_t frame_tx_line_buf[CAP_WORDS_PER_LINE];
static uint8_t rle_line_buf[PKT_MAX_PAYLOAD];

static int dma_chan;
static PIO pio = pio0;
static uint sm = 0;
static bool vsync_fall_edge = true;
static uint offset_fall_pixrise = 0;
static void gpio_irq(uint gpio, uint32_t events);

typedef struct {
    uint16_t len;
    uint8_t data[PKT_MAX_BYTES];
} txq_entry_t;

/* Ring buffer of complete line packets (variable length). */
static txq_entry_t txq[TXQ_DEPTH];
static volatile uint16_t txq_w = 0;
static volatile uint16_t txq_r = 0;
static uint16_t txq_offset = 0;

typedef enum {
    CORE1_CMD_STOP_CAPTURE = 1,
    CORE1_CMD_RESET_COUNTERS = 2,
    CORE1_CMD_SINGLE_FRAME = 3,
    CORE1_CMD_START_TEST = 4,
    CORE1_CMD_CONFIG_VSYNC = 5,
    CORE1_CMD_DIAG_PREP = 6,
    CORE1_CMD_DIAG_DONE = 7,
} core1_cmd_t;

#define CORE1_CMD(code, param) (((uint32_t)(code) << 16) | ((param) & 0xFFFFu))
#define CORE1_CMD_CODE(cmd) ((uint16_t)((cmd) >> 16))
#define CORE1_CMD_PARAM(cmd) ((uint16_t)((cmd) & 0xFFFFu))

static inline void core1_send_cmd(uint16_t code, uint16_t param) {
    multicore_fifo_push_blocking(CORE1_CMD(code, param));
}

static inline bool load_bool(const volatile bool *value) {
    return __atomic_load_n(value, __ATOMIC_ACQUIRE);
}

static inline uint16_t load_u16(const volatile uint16_t *value) {
    return __atomic_load_n(value, __ATOMIC_ACQUIRE);
}

static inline uint32_t load_u32(const volatile uint32_t *value) {
    return __atomic_load_n(value, __ATOMIC_ACQUIRE);
}

static inline void store_bool(volatile bool *value, bool data) {
    __atomic_store_n(value, data, __ATOMIC_RELEASE);
}

static inline void store_u16(volatile uint16_t *value, uint16_t data) {
    __atomic_store_n(value, data, __ATOMIC_RELEASE);
}

static inline void store_u32(volatile uint32_t *value, uint32_t data) {
    __atomic_store_n(value, data, __ATOMIC_RELEASE);
}

static inline void txq_store_w(uint16_t value) {
    store_u16(&txq_w, value);
}

static inline void txq_store_r(uint16_t value) {
    store_u16(&txq_r, value);
}

static inline uint16_t txq_load_w(void) {
    return load_u16(&txq_w);
}

static inline uint16_t txq_load_r(void) {
    return load_u16(&txq_r);
}

static inline void txq_reset(void) {
    txq_store_w(0);
    txq_store_r(0);
}

static inline void reset_frame_tx_state(void) {
    frame_tx_buf = NULL;
    frame_tx_line = 0;
    frame_tx_id = 0;
    frame_tx_lines = 0;
    frame_tx_start = 0;
    capture.frame_ready = false;
    capture.frame_ready_lines = 0;
    capture.ready_buf = NULL;
    video_capture_set_inflight(&capture, NULL);
}

static inline void set_ps_on(bool on) {
    ps_on_state = on;
    gpio_put(PIN_PS_ON, on ? 1 : 0);
}

static inline void reset_diag_counts(void) {
    diag_pixclk_edges = 0;
    diag_hsync_edges = 0;
    diag_vsync_edges = 0;
    diag_video_edges = 0;
}

static inline bool txq_is_empty(void) {
    uint16_t r = txq_load_r();
    uint16_t w = txq_load_w();
    return r == w;
}

static inline bool txq_has_space(void) {
    uint16_t r = txq_load_r();
    uint16_t w = txq_load_w();
    return ((uint16_t)((w + 1) & TXQ_MASK)) != r;
}

static inline bool can_emit_text(void) {
    return !load_bool(&capture.capture_enabled)
           && !load_bool(&test_frame_active)
           && txq_is_empty()
           && !load_bool(&armed);
}

static size_t rle_encode_line(const uint8_t *src, size_t src_len, uint8_t *dst, size_t dst_cap) {
    size_t out = 0;
    size_t i = 0;
    while (i < src_len) {
        uint8_t value = src[i];
        size_t run = 1;
        while ((i + run) < src_len && run < 255 && src[i + run] == value) {
            run++;
        }
        if ((out + 2) > dst_cap) {
            return 0;
        }
        dst[out++] = (uint8_t)run;
        dst[out++] = value;
        i += run;
    }
    return out;
}

static inline bool txq_enqueue_payload(uint16_t fid, uint16_t lid, const uint8_t *payload,
                                       uint16_t payload_len, bool rle) {
    if (payload_len > PKT_MAX_PAYLOAD) {
        return false;
    }
    uint16_t w = txq_load_w();
    uint16_t next = (uint16_t)((w + 1) & TXQ_MASK);
    if (next == txq_load_r()) {
        /* queue full */
        return false;
    }

    uint8_t *p = txq[w].data;
    p[0] = 0xEB;
    p[1] = 0xD1;

    p[2] = (uint8_t)(fid & 0xFF);
    p[3] = (uint8_t)((fid >> 8) & 0xFF);

    p[4] = (uint8_t)(lid & 0xFF);
    p[5] = (uint8_t)((lid >> 8) & 0xFF);

    uint16_t len_field = payload_len;
    if (rle) {
        len_field = (uint16_t)(len_field | PKT_FLAG_RLE);
    }
    p[6] = (uint8_t)(len_field & 0xFF);
    p[7] = (uint8_t)((len_field >> 8) & 0xFF);

    memcpy(&p[8], payload, payload_len);
    txq[w].len = (uint16_t)(PKT_HDR_BYTES + payload_len);

    /* publish write index last so reader never sees a half-filled packet */
    txq_store_w(next);
    return true;
}

static inline bool txq_enqueue_line(uint16_t fid, uint16_t lid, const uint8_t *data64) {
    if (!load_bool(&tx_rle_enabled)) {
        return txq_enqueue_payload(fid, lid, data64, BYTES_PER_LINE, false);
    }

    size_t rle_len = rle_encode_line(data64, BYTES_PER_LINE, rle_line_buf, sizeof(rle_line_buf));
    if (rle_len > 0 && rle_len < BYTES_PER_LINE) {
        return txq_enqueue_payload(fid, lid, rle_line_buf, (uint16_t)rle_len, true);
    }

    return txq_enqueue_payload(fid, lid, data64, BYTES_PER_LINE, false);
}

static bool try_send_probe_packet(void) {
    if (!tud_cdc_connected()) return false;

    if (probe_offset == 0) {
        probe_buf[0] = 0xEB;
        probe_buf[1] = 0xD1;
        probe_buf[2] = 0xAA;
        probe_buf[3] = 0x55;
        probe_buf[4] = 0x34;
        probe_buf[5] = 0x12;
        probe_buf[6] = (uint8_t)(BYTES_PER_LINE & 0xFF);
        probe_buf[7] = (uint8_t)((BYTES_PER_LINE >> 8) & 0xFF);
        memset(&probe_buf[8], 0xA5, BYTES_PER_LINE);
    }

    while (probe_offset < PKT_RAW_BYTES) {
        int avail = tud_cdc_write_available();
        if (avail <= 0) return false;
        uint32_t to_write = (uint32_t)avail;
        uint32_t remain = (uint32_t)(PKT_RAW_BYTES - probe_offset);
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
    printf("[EBD_IPKVM] dbg armed=%d cap=%d test=%d probe=%d vsync=%s txq_r=%u txq_w=%u write_avail=%d frames=%lu lines=%lu drops=%lu usb=%lu frame_overrun=%lu short=%lu\n",
           load_bool(&armed) ? 1 : 0,
           load_bool(&capture.capture_enabled) ? 1 : 0,
           load_bool(&test_frame_active) ? 1 : 0,
           __atomic_load_n(&probe_pending, __ATOMIC_ACQUIRE) ? 1 : 0,
           load_bool(&vsync_fall_edge) ? "fall" : "rise",
           (unsigned)txq_load_r(),
           (unsigned)txq_load_w(),
           tud_cdc_write_available(),
           (unsigned long)load_u32(&frames_done),
           (unsigned long)load_u32(&capture.lines_ok),
           (unsigned long)load_u32(&lines_drop),
           (unsigned long)load_u32(&usb_drops),
           (unsigned long)__atomic_load_n(&capture.frame_overrun, __ATOMIC_ACQUIRE),
           (unsigned long)__atomic_load_n(&capture.frame_short, __ATOMIC_ACQUIRE));
}

static inline void reorder_line_words(uint32_t *buf) {
    for (size_t i = 0; i < WORDS_PER_LINE; i++) {
        buf[i] = __builtin_bswap32(buf[i]);
    }
}

static void configure_pio_program(void) {
    pio_sm_set_enabled(pio, sm, false);
    pio_sm_clear_fifos(pio, sm);
    pio_sm_restart(pio, sm);
    classic_line_fall_pixrise_program_init(pio, sm, offset_fall_pixrise, PIN_VIDEO);
}

static void configure_vsync_irq(void) {
    uint32_t edge = load_bool(&vsync_fall_edge) ? GPIO_IRQ_EDGE_FALL : GPIO_IRQ_EDGE_RISE;
    gpio_acknowledge_irq(PIN_VSYNC, GPIO_IRQ_EDGE_FALL | GPIO_IRQ_EDGE_RISE);
    gpio_set_irq_enabled_with_callback(PIN_VSYNC, edge, true, &gpio_irq);
}

static void service_vsync(uint32_t now_us) {
    if ((uint32_t)(now_us - last_vsync_us) < 8000u) {
        return;
    }
    last_vsync_us = now_us;

    vsync_edges++;

    if (load_bool(&diag_active)) {
        return;
    }
    if (load_bool(&capture.capture_enabled)) {
        return;
    }
    if (!load_bool(&armed)) {
        return;
    }

    bool tx_busy = (frame_tx_buf != NULL) || capture.frame_ready || !txq_is_empty();
    capture_mode_t mode = __atomic_load_n(&capture_mode, __ATOMIC_ACQUIRE);
    if (mode == CAPTURE_MODE_TEST_30FPS) {
        bool toggle = !load_bool(&take_toggle);          // every other VSYNC => ~30fps
        store_bool(&take_toggle, toggle);
        store_bool(&want_frame, toggle && !tx_busy);
    } else {
        store_bool(&want_frame, !tx_busy);
    }

    if (load_bool(&want_frame)) {
        video_capture_start(&capture, true);
    }
}

static void gpio_irq(uint gpio, uint32_t events) {
    if (gpio != PIN_VSYNC) return;
    bool fall_edge = load_bool(&vsync_fall_edge);
    if (fall_edge) {
        if (!(events & GPIO_IRQ_EDGE_FALL)) return;
    } else {
        if (!(events & GPIO_IRQ_EDGE_RISE)) return;
    }

    service_vsync(time_us_32());
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

    gpio_set_function(PIN_PIXCLK, GPIO_FUNC_SIO);
    gpio_set_function(PIN_HSYNC, GPIO_FUNC_SIO);
    gpio_set_function(PIN_VIDEO, GPIO_FUNC_SIO);

    reset_diag_counts();

    bool prev_pixclk = gpio_get(PIN_PIXCLK);
    bool prev_hsync = gpio_get(PIN_HSYNC);
    bool prev_vsync = gpio_get(PIN_VSYNC);
    bool prev_video = gpio_get(PIN_VIDEO);

    absolute_time_t end = make_timeout_time_ms(diag_ms);
    while (absolute_time_diff_us(get_absolute_time(), end) > 0) {
        tud_task();
        bool pixclk = gpio_get(PIN_PIXCLK);
        bool hsync = gpio_get(PIN_HSYNC);
        bool vsync = gpio_get(PIN_VSYNC);
        bool video = gpio_get(PIN_VIDEO);
        diag_accumulate_edges(pixclk, hsync, vsync, video,
                              &prev_pixclk, &prev_hsync, &prev_vsync, &prev_video);
        sleep_us(2);
        tight_loop_contents();
    }

    bool pixclk = gpio_get(PIN_PIXCLK);
    bool hsync = gpio_get(PIN_HSYNC);
    bool vsync = gpio_get(PIN_VSYNC);
    bool video = gpio_get(PIN_VIDEO);

    gpio_set_function(PIN_PIXCLK, GPIO_FUNC_PIO0);
    gpio_set_function(PIN_HSYNC, GPIO_FUNC_PIO0);
    gpio_set_function(PIN_VIDEO, GPIO_FUNC_PIO0);

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

static void service_test_frame(void) {
    if (!load_bool(&test_frame_active) || load_bool(&capture.capture_enabled)) return;

    while (load_bool(&test_frame_active)) {
        uint8_t fill = (test_line & 1) ? 0xFF : 0x00;
        memset(test_line_buf, fill, BYTES_PER_LINE);
        if (!txq_enqueue_line(frame_id, test_line, test_line_buf)) {
            break;
        }

        test_line++;
        if (test_line >= CAP_ACTIVE_H) {
            store_bool(&test_frame_active, false);
            test_line = 0;
            frame_id++;
            frames_done++;
            break;
        }
    }
}

static void service_frame_tx(void) {
    if (!frame_tx_buf) {
        uint32_t (*buf)[CAP_WORDS_PER_LINE] = NULL;
        uint16_t fid = 0;
        uint16_t lines = 0;
        if (video_capture_take_ready(&capture, &buf, &fid, &lines)) {
            frame_tx_buf = buf;
            frame_tx_id = fid;
            frame_tx_line = 0;
            frame_tx_lines = lines;
            if (lines >= (CAP_YOFF_LINES + CAP_ACTIVE_H)) {
                frame_tx_start = CAP_YOFF_LINES;
            } else {
                frame_tx_start = (lines >= CAP_ACTIVE_H) ? (uint16_t)(lines - CAP_ACTIVE_H) : 0;
            }
            video_capture_set_inflight(&capture, buf);
        }
    }

    if (!frame_tx_buf) return;

    if (frame_tx_lines < CAP_ACTIVE_H) {
        capture.frame_short++;
        frame_tx_buf = NULL;
        video_capture_set_inflight(&capture, NULL);
        return;
    }

    while (frame_tx_line < CAP_ACTIVE_H) {
        if (!txq_has_space()) break;

        uint16_t src_line = (uint16_t)(frame_tx_line + frame_tx_start);
        if (src_line >= frame_tx_lines) {
            capture.frame_short++;
            frame_tx_buf = NULL;
            video_capture_set_inflight(&capture, NULL);
            return;
        }
        memcpy(frame_tx_line_buf, frame_tx_buf[src_line], sizeof(frame_tx_line_buf));
        reorder_line_words(frame_tx_line_buf);

        if (!txq_enqueue_line(frame_tx_id, frame_tx_line, (const uint8_t *)frame_tx_line_buf)) {
            lines_drop++;
            break;
        }

        frame_tx_line++;
    }

    if (frame_tx_line >= CAP_ACTIVE_H) {
        frame_tx_buf = NULL;
        video_capture_set_inflight(&capture, NULL);
    }
}

static void poll_cdc_commands(void) {
    // Reads single-byte commands: S=start, X=stop, R=reset counters, Q=park
    while (tud_cdc_available()) {
        uint8_t ch;
        if (tud_cdc_read(&ch, 1) != 1) break;

        if (ch == 'S' || ch == 's') {
            store_bool(&armed, true);
            /* IMPORTANT: do NOT printf here; keep binary stream clean once armed */
        } else if (ch == 'X' || ch == 'x') {
            store_bool(&armed, false);
            store_bool(&want_frame, false);
            txq_offset = 0;
            core1_send_cmd(CORE1_CMD_STOP_CAPTURE, 0);
            if (can_emit_text()) {
                printf("[EBD_IPKVM] armed=0 (host stop)\n");
            }
        } else if (ch == 'R' || ch == 'r') {
            store_u32(&usb_drops, 0);
            store_bool(&take_toggle, false);
            store_bool(&want_frame, false);
            txq_offset = 0;
            core1_send_cmd(CORE1_CMD_STOP_CAPTURE, 0);
            core1_send_cmd(CORE1_CMD_RESET_COUNTERS, 0);
            if (can_emit_text()) {
                printf("[EBD_IPKVM] reset counters\n");
            }
        } else if (ch == 'Q' || ch == 'q') {
            store_bool(&armed, false);
            store_bool(&want_frame, false);
            txq_offset = 0;
            core1_send_cmd(CORE1_CMD_STOP_CAPTURE, 0);
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
            store_bool(&armed, false);
            store_bool(&want_frame, false);
            txq_offset = 0;
            core1_send_cmd(CORE1_CMD_STOP_CAPTURE, 0);
            sleep_ms(10);
            reset_usb_boot(0, 0);
        } else if (ch == 'Z' || ch == 'z') {
            store_bool(&armed, false);
            store_bool(&want_frame, false);
            txq_offset = 0;
            core1_send_cmd(CORE1_CMD_STOP_CAPTURE, 0);
            sleep_ms(10);
            watchdog_reboot(0, 0, 0);
            while (true) { tight_loop_contents(); }
        } else if (ch == 'F' || ch == 'f') {
            core1_send_cmd(CORE1_CMD_SINGLE_FRAME, 0);
        } else if (ch == 'T' || ch == 't') {
            store_bool(&armed, false);
            store_bool(&want_frame, false);
            txq_offset = 0;
            core1_send_cmd(CORE1_CMD_START_TEST, 0);
            request_probe_packet();
        } else if (ch == 'U' || ch == 'u') {
            request_probe_packet();
        } else if (ch == 'I' || ch == 'i') {
            debug_requested = true;
        } else if (ch == 'E') {
            store_bool(&tx_rle_enabled, true);
            if (can_emit_text()) {
                printf("[EBD_IPKVM] rle=on\n");
            }
        } else if (ch == 'e') {
            store_bool(&tx_rle_enabled, false);
            if (can_emit_text()) {
                printf("[EBD_IPKVM] rle=off\n");
            }
        } else if (ch == 'G' || ch == 'g') {
            if (can_emit_text()) {
                core1_send_cmd(CORE1_CMD_DIAG_PREP, 0);
                run_gpio_diag();
                core1_send_cmd(CORE1_CMD_DIAG_DONE, 0);
            }
        } else if (ch == 'V' || ch == 'v') {
            bool new_edge = !load_bool(&vsync_fall_edge);
            store_bool(&vsync_fall_edge, new_edge);
            store_bool(&armed, false);
            store_bool(&want_frame, false);
            txq_offset = 0;
            core1_send_cmd(CORE1_CMD_STOP_CAPTURE, 0);
            core1_send_cmd(CORE1_CMD_CONFIG_VSYNC, 0);
            if (can_emit_text()) {
                printf("[EBD_IPKVM] vsync_edge=%s\n", new_edge ? "fall" : "rise");
            }
        } else if (ch == 'M' || ch == 'm') {
            capture_mode_t next_mode = (capture_mode == CAPTURE_MODE_TEST_30FPS)
                                           ? CAPTURE_MODE_CONTINUOUS_60FPS
                                           : CAPTURE_MODE_TEST_30FPS;
            __atomic_store_n(&capture_mode, next_mode, __ATOMIC_RELEASE);
            store_bool(&want_frame, false);
            store_bool(&take_toggle, false);
            txq_offset = 0;
            core1_send_cmd(CORE1_CMD_STOP_CAPTURE, 0);
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
        uint16_t r = txq_load_r();
        uint16_t w = txq_load_w();

        if (r == w) break; /* empty */

        int avail = tud_cdc_write_available();
        if (avail <= 0) break;

        uint16_t pkt_len = txq[r].len;
        if (pkt_len == 0 || pkt_len > PKT_MAX_BYTES) {
            txq_offset = 0;
            txq_store_r((uint16_t)((r + 1) & TXQ_MASK));
            continue;
        }

        uint32_t remain = (uint32_t)(pkt_len - txq_offset);
        uint32_t to_write = (uint32_t)avail;
        if (to_write > remain) {
            to_write = remain;
        }

        uint32_t n = tud_cdc_write(&txq[r].data[txq_offset], to_write);
        if (n == 0) {
            usb_drops++;
            break;
        }

        txq_offset = (uint16_t)(txq_offset + n);
        wrote_any = true;

        if (txq_offset >= pkt_len) {
            txq_offset = 0;
            txq_store_r((uint16_t)((r + 1) & TXQ_MASK));
        }
    }

    if (wrote_any) {
        tud_cdc_write_flush();
    }
}

static void core1_stop_capture_and_reset(void) {
    store_bool(&want_frame, false);
    store_bool(&take_toggle, false);
    store_bool(&test_frame_active, false);
    test_line = 0;
    video_capture_stop(&capture);
    txq_reset();
    reset_frame_tx_state();
}

static void core1_handle_command(uint32_t cmd) {
    switch (CORE1_CMD_CODE(cmd)) {
    case CORE1_CMD_STOP_CAPTURE:
        store_bool(&diag_active, false);
        core1_stop_capture_and_reset();
        break;
    case CORE1_CMD_RESET_COUNTERS:
        store_u16(&frame_id, 0);
        store_u32(&frames_done, 0);
        store_u32(&lines_drop, 0);
        store_u32(&vsync_edges, 0);
        store_u32(&capture.lines_ok, 0);
        __atomic_store_n(&capture.frame_overrun, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&capture.frame_short, 0, __ATOMIC_RELEASE);
        break;
    case CORE1_CMD_SINGLE_FRAME:
        if (!capture.capture_enabled) {
            store_bool(&want_frame, true);
            video_capture_start(&capture, true);
        }
        break;
    case CORE1_CMD_START_TEST:
        store_bool(&armed, false);
        store_bool(&diag_active, false);
        core1_stop_capture_and_reset();
        store_bool(&test_frame_active, true);
        test_line = 0;
        break;
    case CORE1_CMD_CONFIG_VSYNC:
        configure_vsync_irq();
        break;
    case CORE1_CMD_DIAG_PREP:
        store_bool(&armed, false);
        store_bool(&diag_active, true);
        core1_stop_capture_and_reset();
        break;
    case CORE1_CMD_DIAG_DONE:
        store_bool(&diag_active, false);
        break;
    default:
        break;
    }
}

static void core1_entry(void) {
    configure_vsync_irq();

    while (true) {
        while (multicore_fifo_rvalid()) {
            uint32_t cmd = multicore_fifo_pop_blocking();
            core1_handle_command(cmd);
        }

        if (capture.capture_enabled && !dma_channel_is_busy(capture.dma_chan)) {
            if (video_capture_finalize_frame(&capture, frame_id)) {
                frame_id++;
                frames_done++;
            } else {
                frame_id++;
            }
        }

        service_test_frame();
        service_frame_tx();
        tight_loop_contents();
    }
}

int main(void) {
    stdio_init_all();
    sleep_ms(1200);

    printf("\n[EBD_IPKVM] USB packet stream @ ~60fps (continuous mode)\n");
    printf("[EBD_IPKVM] WAITING for host. Send 'S' to start, 'X' stop, 'R' reset.\n");
    printf("[EBD_IPKVM] Power/control: 'P' on, 'p' off, 'B' BOOTSEL, 'Z' reset.\n");
    printf("[EBD_IPKVM] GPIO diag: send 'G' for pin states + edge counts.\n");
    printf("[EBD_IPKVM] Edge toggles: 'V' VSYNC edge. Mode toggle: 'M' 30fps↔60fps.\n");

    // SIO GPIO inputs + pulls (sane when Mac is off)
    gpio_init(PIN_PIXCLK); gpio_set_dir(PIN_PIXCLK, GPIO_IN); gpio_disable_pulls(PIN_PIXCLK);
    gpio_init(PIN_VIDEO);  gpio_set_dir(PIN_VIDEO,  GPIO_IN); gpio_disable_pulls(PIN_VIDEO);
    gpio_init(PIN_HSYNC);  gpio_set_dir(PIN_HSYNC,  GPIO_IN); gpio_disable_pulls(PIN_HSYNC);

    // VSYNC must remain SIO GPIO for IRQ to work
    gpio_init(PIN_VSYNC);  gpio_set_dir(PIN_VSYNC,  GPIO_IN); gpio_disable_pulls(PIN_VSYNC);
    gpio_init(PIN_PS_ON);  gpio_set_dir(PIN_PS_ON, GPIO_OUT); gpio_put(PIN_PS_ON, 0);

    // Clear any stale IRQ state, core1 will enable callback
    gpio_acknowledge_irq(PIN_VSYNC, GPIO_IRQ_EDGE_FALL | GPIO_IRQ_EDGE_RISE);

    // Hand ONLY the pins PIO needs to PIO0
    gpio_set_function(PIN_PIXCLK, GPIO_FUNC_PIO0);
    gpio_set_function(PIN_HSYNC,  GPIO_FUNC_PIO0);
    gpio_set_function(PIN_VIDEO,  GPIO_FUNC_PIO0);

    pio_sm_set_consecutive_pindirs(pio, sm, PIN_PIXCLK, 1, false);
    pio_sm_set_consecutive_pindirs(pio, sm, PIN_HSYNC,  1, false);
    pio_sm_set_consecutive_pindirs(pio, sm, PIN_VIDEO,  1, false);

    offset_fall_pixrise = pio_add_program(pio, &classic_line_fall_pixrise_program);
    configure_pio_program();

    dma_chan = dma_claim_unused_channel(true);
    irq_set_priority(USBCTRL_IRQ, 1);

    video_capture_init(&capture, pio, sm, dma_chan, framebuf_a, framebuf_b);
    video_capture_stop(&capture);
    txq_reset();
    reset_frame_tx_state();

    multicore_launch_core1(core1_entry);

    absolute_time_t next = make_timeout_time_ms(1000);
    uint32_t last_lines = 0;

    while (true) {
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
        bool can_report = !load_bool(&capture.capture_enabled)
                          && !load_bool(&test_frame_active)
                          && txq_is_empty()
                          && !load_bool(&armed);
        if (can_report) {
            if (absolute_time_diff_us(get_absolute_time(), next) <= 0) {
                next = delayed_by_ms(next, 1000);

                uint32_t l = load_u32(&capture.lines_ok);
                uint32_t per_s = l - last_lines;
                last_lines = l;

                uint32_t ve = load_u32(&vsync_edges);
                store_u32(&vsync_edges, 0);

                printf("[EBD_IPKVM] armed=%d cap=%d ps_on=%d lines/s=%lu total=%lu q_drops=%lu usb_drops=%lu frame_overrun=%lu vsync_edges/s=%lu frames=%lu\n",
                       load_bool(&armed) ? 1 : 0,
                       load_bool(&capture.capture_enabled) ? 1 : 0,
                       ps_on_state ? 1 : 0,
                       (unsigned long)per_s,
                       (unsigned long)l,
                       (unsigned long)load_u32(&lines_drop),
                       (unsigned long)load_u32(&usb_drops),
                       (unsigned long)__atomic_load_n(&capture.frame_overrun, __ATOMIC_ACQUIRE),
                       (unsigned long)ve,
                       (unsigned long)load_u32(&frames_done));
            }
        }

        tight_loop_contents();
    }
}
