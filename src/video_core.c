#include "video_core.h"

#include <string.h>

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"

#include "classic_line.pio.h"
#include "adb_bus.h"
#include "adb_core.h"
#include "core_bridge.h"

#define TXQ_DEPTH 512
#define TXQ_MASK  (TXQ_DEPTH - 1)

#define PKT_MAX_PAYLOAD VIDEO_CORE_MAX_PAYLOAD
#define PKT_MAX_BYTES VIDEO_CORE_MAX_PACKET_BYTES

static volatile bool armed = false;
static volatile bool want_frame = false;
static volatile bool take_toggle = false;
static volatile capture_mode_t capture_mode = CAPTURE_MODE_CONTINUOUS_60FPS;

static volatile uint16_t frame_id = 0;
static volatile uint32_t lines_drop = 0;

static volatile uint32_t vsync_edges = 0;
static volatile uint32_t frames_done = 0;
static volatile bool test_frame_active = false;
static volatile bool diag_active = false;
static volatile uint32_t last_vsync_us = 0;
static volatile bool tx_rle_enabled = true;
static volatile uint32_t core1_busy_us = 0;
static volatile uint32_t core1_total_us = 0;

static uint16_t test_line = 0;
static uint8_t test_line_buf[CAP_BYTES_PER_LINE];

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

typedef struct {
    uint16_t len;
    uint8_t data[PKT_MAX_BYTES];
} txq_entry_t;

static txq_entry_t txq[TXQ_DEPTH];
static volatile uint16_t txq_w = 0;
static volatile uint16_t txq_r = 0;

static PIO pio = pio0;
static uint sm = 0;
static uint offset_fall_pixrise = 0;
static uint pin_video = 0;
static uint pin_vsync = 0;
static volatile bool vsync_fall_edge = true;

static void gpio_irq(uint gpio, uint32_t events);

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

    uint16_t len_field = payload_len;
    if (rle) {
        len_field = (uint16_t)(len_field | STREAM_FLAG_RLE);
    }

    uint8_t *p = txq[w].data;
    stream_write_header(p, fid, lid, len_field);
    memcpy(&p[STREAM_HEADER_BYTES], payload, payload_len);
    txq[w].len = (uint16_t)(STREAM_HEADER_BYTES + payload_len);

    /* publish write index last so reader never sees a half-filled packet */
    txq_store_w(next);
    return true;
}

static inline bool txq_enqueue_line(uint16_t fid, uint16_t lid, const uint8_t *data64) {
    if (!load_bool(&tx_rle_enabled)) {
        return txq_enqueue_payload(fid, lid, data64, CAP_BYTES_PER_LINE, false);
    }

    size_t rle_len = rle_encode_line(data64, CAP_BYTES_PER_LINE, rle_line_buf, sizeof(rle_line_buf));
    if (rle_len > 0 && rle_len < CAP_BYTES_PER_LINE) {
        return txq_enqueue_payload(fid, lid, rle_line_buf, (uint16_t)rle_len, true);
    }

    return txq_enqueue_payload(fid, lid, data64, CAP_BYTES_PER_LINE, false);
}

static inline void reorder_line_words(uint32_t *buf) {
    for (size_t i = 0; i < CAP_WORDS_PER_LINE; i++) {
        buf[i] = __builtin_bswap32(buf[i]);
    }
}

static void configure_pio_program(void) {
    pio_sm_set_enabled(pio, sm, false);
    pio_sm_clear_fifos(pio, sm);
    pio_sm_restart(pio, sm);
    classic_line_fall_pixrise_program_init(pio, sm, offset_fall_pixrise, pin_video);
}

static void configure_vsync_irq(void) {
    uint32_t edge = load_bool(&vsync_fall_edge) ? GPIO_IRQ_EDGE_FALL : GPIO_IRQ_EDGE_RISE;
    gpio_acknowledge_irq(pin_vsync, GPIO_IRQ_EDGE_FALL | GPIO_IRQ_EDGE_RISE);
    gpio_set_irq_enabled_with_callback(pin_vsync, edge, true, &gpio_irq);
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
    if (gpio != pin_vsync) return;
    bool fall_edge = load_bool(&vsync_fall_edge);
    if (fall_edge) {
        if (!(events & GPIO_IRQ_EDGE_FALL)) return;
    } else {
        if (!(events & GPIO_IRQ_EDGE_RISE)) return;
    }

    service_vsync(time_us_32());
}

static bool service_test_frame(void) {
    if (!load_bool(&test_frame_active) || load_bool(&capture.capture_enabled)) return false;

    bool did_work = false;
    while (load_bool(&test_frame_active)) {
        uint8_t fill = (test_line & 1) ? 0xFF : 0x00;
        memset(test_line_buf, fill, CAP_BYTES_PER_LINE);
        if (!txq_enqueue_line(frame_id, test_line, test_line_buf)) {
            break;
        }

        did_work = true;
        test_line++;
        if (test_line >= CAP_ACTIVE_H) {
            store_bool(&test_frame_active, false);
            test_line = 0;
            frame_id++;
            frames_done++;
            break;
        }
    }
    return did_work;
}

static bool service_frame_tx(void) {
    bool did_work = false;
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
            did_work = true;
        }
    }

    if (!frame_tx_buf) return did_work;

    if (frame_tx_lines < CAP_ACTIVE_H) {
        capture.frame_short++;
        frame_tx_buf = NULL;
        video_capture_set_inflight(&capture, NULL);
        return true;
    }

    while (frame_tx_line < CAP_ACTIVE_H) {
        if (!txq_has_space()) break;

        uint16_t src_line = (uint16_t)(frame_tx_line + frame_tx_start);
        if (src_line >= frame_tx_lines) {
            capture.frame_short++;
            frame_tx_buf = NULL;
            video_capture_set_inflight(&capture, NULL);
            return true;
        }
        memcpy(frame_tx_line_buf, frame_tx_buf[src_line], sizeof(frame_tx_line_buf));
        reorder_line_words(frame_tx_line_buf);

        if (!txq_enqueue_line(frame_tx_id, frame_tx_line, (const uint8_t *)frame_tx_line_buf)) {
            lines_drop++;
            break;
        }

        did_work = true;
        frame_tx_line++;
    }

    if (frame_tx_line >= CAP_ACTIVE_H) {
        frame_tx_buf = NULL;
        video_capture_set_inflight(&capture, NULL);
    }
    return did_work;
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
    switch (core_bridge_unpack_code(cmd)) {
    case CORE_BRIDGE_CMD_STOP_CAPTURE:
        store_bool(&diag_active, false);
        core1_stop_capture_and_reset();
        break;
    case CORE_BRIDGE_CMD_RESET_COUNTERS:
        store_u16(&frame_id, 0);
        store_u32(&frames_done, 0);
        store_u32(&lines_drop, 0);
        store_u32(&vsync_edges, 0);
        store_u32(&capture.lines_ok, 0);
        __atomic_store_n(&capture.frame_overrun, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&capture.frame_short, 0, __ATOMIC_RELEASE);
        break;
    case CORE_BRIDGE_CMD_SINGLE_FRAME:
        if (!capture.capture_enabled) {
            store_bool(&want_frame, true);
            video_capture_start(&capture, true);
        }
        break;
    case CORE_BRIDGE_CMD_START_TEST:
        store_bool(&armed, false);
        store_bool(&diag_active, false);
        core1_stop_capture_and_reset();
        store_bool(&test_frame_active, true);
        test_line = 0;
        break;
    case CORE_BRIDGE_CMD_CONFIG_VSYNC:
        configure_vsync_irq();
        break;
    case CORE_BRIDGE_CMD_DIAG_PREP:
        store_bool(&armed, false);
        store_bool(&diag_active, true);
        core1_stop_capture_and_reset();
        break;
    case CORE_BRIDGE_CMD_DIAG_DONE:
        store_bool(&diag_active, false);
        break;
    default:
        break;
    }
}

static void core1_entry(void) {
    adb_core_init();
    configure_vsync_irq();

    while (true) {
        uint32_t loop_start = time_us_32();
        uint32_t active_us = 0;
        uint32_t cmd = 0;
        while (core_bridge_try_pop(&cmd)) {
            core1_handle_command(cmd);
        }

        uint32_t active_start = time_us_32();
        if (adb_bus_task()) {
            active_us += (uint32_t)(time_us_32() - active_start);
        }

        active_start = time_us_32();
        adb_core_task();
        active_us += (uint32_t)(time_us_32() - active_start);

        if (capture.capture_enabled && !dma_channel_is_busy(capture.dma_chan)) {
            active_start = time_us_32();
            if (video_capture_finalize_frame(&capture, frame_id)) {
                frame_id++;
                frames_done++;
            } else {
                frame_id++;
            }
            active_us += (uint32_t)(time_us_32() - active_start);
        }

        active_start = time_us_32();
        if (service_test_frame()) {
            active_us += (uint32_t)(time_us_32() - active_start);
        }

        active_start = time_us_32();
        if (service_frame_tx()) {
            active_us += (uint32_t)(time_us_32() - active_start);
        }

        tight_loop_contents();
        uint32_t loop_end = time_us_32();

        uint32_t total_delta = (uint32_t)(loop_end - loop_start);
        __atomic_fetch_add(&core1_busy_us, active_us, __ATOMIC_RELAXED);
        __atomic_fetch_add(&core1_total_us, total_delta, __ATOMIC_RELAXED);
    }
}

void video_core_init(const video_core_config_t *cfg) {
    pio = cfg->pio;
    sm = cfg->sm;
    offset_fall_pixrise = cfg->offset_fall_pixrise;
    pin_video = cfg->pin_video;
    pin_vsync = cfg->pin_vsync;

    vsync_fall_edge = true;
    __atomic_store_n(&capture_mode, CAPTURE_MODE_CONTINUOUS_60FPS, __ATOMIC_RELEASE);
    store_bool(&armed, false);
    store_bool(&want_frame, false);
    store_bool(&take_toggle, false);
    store_bool(&test_frame_active, false);
    store_bool(&diag_active, false);
    store_bool(&tx_rle_enabled, true);
    store_u16(&frame_id, 0);
    store_u32(&lines_drop, 0);
    store_u32(&frames_done, 0);
    store_u32(&vsync_edges, 0);
    store_u32(&last_vsync_us, 0);
    store_u32(&core1_busy_us, 0);
    store_u32(&core1_total_us, 0);
    test_line = 0;

    configure_pio_program();
    video_capture_init(&capture, pio, sm, cfg->dma_chan, framebuf_a, framebuf_b);
    video_capture_stop(&capture);
    txq_reset();
    reset_frame_tx_state();
    core_bridge_adb_reset();
}

void video_core_launch(void) {
    multicore_launch_core1(core1_entry);
}

bool video_core_can_emit_text(void) {
    return !load_bool(&capture.capture_enabled)
           && !load_bool(&test_frame_active)
           && txq_is_empty()
           && !load_bool(&armed);
}

void video_core_set_armed(bool value) {
    store_bool(&armed, value);
}

bool video_core_is_armed(void) {
    return load_bool(&armed);
}

void video_core_set_want_frame(bool value) {
    store_bool(&want_frame, value);
}

void video_core_set_take_toggle(bool value) {
    store_bool(&take_toggle, value);
}

void video_core_set_capture_mode(capture_mode_t mode) {
    __atomic_store_n(&capture_mode, mode, __ATOMIC_RELEASE);
}

capture_mode_t video_core_get_capture_mode(void) {
    return __atomic_load_n(&capture_mode, __ATOMIC_ACQUIRE);
}

void video_core_set_vsync_edge(bool fall_edge) {
    store_bool(&vsync_fall_edge, fall_edge);
}

bool video_core_get_vsync_edge(void) {
    return load_bool(&vsync_fall_edge);
}

void video_core_set_tx_rle_enabled(bool enabled) {
    store_bool(&tx_rle_enabled, enabled);
}

bool video_core_get_tx_rle_enabled(void) {
    return load_bool(&tx_rle_enabled);
}

bool video_core_capture_enabled(void) {
    return load_bool(&capture.capture_enabled);
}

bool video_core_test_frame_active(void) {
    return load_bool(&test_frame_active);
}

uint32_t video_core_get_lines_drop(void) {
    return load_u32(&lines_drop);
}

uint32_t video_core_get_frames_done(void) {
    return load_u32(&frames_done);
}

uint32_t video_core_get_lines_ok(void) {
    return load_u32(&capture.lines_ok);
}

uint32_t video_core_get_frame_overrun(void) {
    return __atomic_load_n(&capture.frame_overrun, __ATOMIC_ACQUIRE);
}

uint32_t video_core_get_frame_short(void) {
    return __atomic_load_n(&capture.frame_short, __ATOMIC_ACQUIRE);
}

uint32_t video_core_take_vsync_edges(void) {
    return __atomic_exchange_n(&vsync_edges, 0, __ATOMIC_ACQ_REL);
}

void video_core_take_core1_utilization(uint32_t *busy_us, uint32_t *total_us) {
    if (busy_us) {
        *busy_us = __atomic_exchange_n(&core1_busy_us, 0, __ATOMIC_ACQ_REL);
    }
    if (total_us) {
        *total_us = __atomic_exchange_n(&core1_total_us, 0, __ATOMIC_ACQ_REL);
    }
}

bool video_core_txq_is_empty(void) {
    return txq_is_empty();
}

bool video_core_txq_peek(const uint8_t **out_data, uint16_t *out_len) {
    uint16_t r = txq_load_r();
    uint16_t w = txq_load_w();
    if (r == w) {
        return false;
    }

    *out_data = txq[r].data;
    *out_len = txq[r].len;
    return true;
}

void video_core_txq_consume(void) {
    uint16_t r = txq_load_r();
    txq_store_r((uint16_t)((r + 1) & TXQ_MASK));
}

void video_core_get_txq_indices(uint16_t *out_r, uint16_t *out_w) {
    if (out_r) {
        *out_r = txq_load_r();
    }
    if (out_w) {
        *out_w = txq_load_w();
    }
}
