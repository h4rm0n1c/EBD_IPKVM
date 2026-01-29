#include "video_stream.h"

#include <stdio.h>
#include <string.h>

#include "pico/bootrom.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/sync.h"

#include "tusb.h"

#include "board_pins.h"
#include "classic_line.pio.h"
#include "video_capture.h"

#if VIDEO_STREAM_UDP
#include "cyw43.h"
#include "lwip/pbuf.h"
#include "lwip/udp.h"
#endif

#define BYTES_PER_LINE CAP_BYTES_PER_LINE
#define WORDS_PER_LINE CAP_WORDS_PER_LINE

#define PKT_HDR_BYTES 8
#define PKT_BYTES     (PKT_HDR_BYTES + BYTES_PER_LINE)

#define UDP_MAGIC0 0xEB
#define UDP_MAGIC1 0xD1
#define UDP_VERSION 1
#define UDP_FORMAT_RLE8 1
#define UDP_HDR_BYTES 10
#define UDP_RLE_MAX_BYTES (CAP_BYTES_PER_LINE * 2)

#define TXQ_DEPTH 512
#define TXQ_MASK  (TXQ_DEPTH - 1)

#define VTXQ_DEPTH 256
#define VTXQ_MASK  (VTXQ_DEPTH - 1)

typedef enum {
    VIDEO_CMD_NONE = 0,
    VIDEO_CMD_START,
    VIDEO_CMD_STOP,
    VIDEO_CMD_RESET,
    VIDEO_CMD_FORCE_CAPTURE,
    VIDEO_CMD_TEST_FRAME,
    VIDEO_CMD_TOGGLE_VSYNC,
    VIDEO_CMD_GPIO_DIAG,
    VIDEO_CMD_PS_ON,
    VIDEO_CMD_PS_OFF,
} video_command_t;

static volatile bool armed = false;
static volatile bool want_frame = false;
static volatile uint16_t frame_id = 0;
static volatile uint32_t lines_drop = 0;
static volatile uint32_t stream_drops = 0;
static volatile uint32_t vsync_edges = 0;
static volatile uint32_t frames_done = 0;
static volatile bool ps_on_state = false;
static volatile uint32_t diag_pixclk_edges = 0;
static volatile uint32_t diag_hsync_edges = 0;
static volatile uint32_t diag_vsync_edges = 0;
static volatile uint32_t diag_video_edges = 0;
static volatile bool test_frame_active = false;
static volatile uint32_t last_vsync_us = 0;
static volatile bool stream_idle = true;
static volatile bool vsync_fall_edge = true;

static uint16_t test_line = 0;
static uint8_t test_line_buf[BYTES_PER_LINE];

static uint8_t probe_buf[PKT_BYTES];
static volatile uint8_t probe_pending = 0;
static uint16_t probe_offset = 0;
static volatile bool debug_requested = false;

static uint32_t framebuf_a[CAP_MAX_LINES][CAP_WORDS_PER_LINE];
static uint32_t framebuf_b[CAP_MAX_LINES][CAP_WORDS_PER_LINE];
static video_capture_t capture = {0};
static uint32_t (*frame_tx_buf)[CAP_WORDS_PER_LINE] = NULL;
static uint16_t frame_tx_id = 0;
static uint16_t frame_tx_line = 0;
static uint16_t frame_tx_lines = 0;
static uint16_t frame_tx_start = 0;
static uint32_t frame_tx_line_buf[CAP_WORDS_PER_LINE];

static int dma_chan;
static PIO pio = pio0;
static uint sm = 0;
static uint offset_fall_pixrise = 0;

static void gpio_irq(uint gpio, uint32_t events);

static uint8_t txq[TXQ_DEPTH][PKT_BYTES];
static volatile uint16_t txq_w = 0;
static volatile uint16_t txq_r = 0;
static spin_lock_t *txq_lock;

typedef struct video_packet {
    uint16_t len;
    uint8_t data[UDP_HDR_BYTES + UDP_RLE_MAX_BYTES];
} video_packet_t;

static video_packet_t vtxq[VTXQ_DEPTH];
static volatile uint16_t vtxq_w = 0;
static volatile uint16_t vtxq_r = 0;
static spin_lock_t *vtxq_lock;

#if VIDEO_STREAM_UDP
typedef struct udp_stream_state {
    struct udp_pcb *pcb;
    ip_addr_t client_addr;
    uint16_t client_port;
    bool client_set;
    bool ready;
} udp_stream_state_t;

static udp_stream_state_t udp_stream = {0};
#endif

static inline void txq_reset(void) {
    uint32_t s = spin_lock_blocking(txq_lock);
    txq_w = 0;
    txq_r = 0;
    spin_unlock(txq_lock, s);
}

static inline bool txq_is_empty(void) {
    uint32_t s = spin_lock_blocking(txq_lock);
    bool empty = (txq_w == txq_r);
    spin_unlock(txq_lock, s);
    return empty;
}

static inline bool txq_has_space(void) {
    uint32_t s = spin_lock_blocking(txq_lock);
    uint16_t w = txq_w;
    uint16_t next = (uint16_t)((w + 1) & TXQ_MASK);
    bool has_space = (next != txq_r);
    spin_unlock(txq_lock, s);
    return has_space;
}

static inline bool vtxq_is_empty(void) {
    uint32_t s = spin_lock_blocking(vtxq_lock);
    bool empty = (vtxq_w == vtxq_r);
    spin_unlock(vtxq_lock, s);
    return empty;
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

static inline void reset_diag_counts(void) {
    diag_pixclk_edges = 0;
    diag_hsync_edges = 0;
    diag_vsync_edges = 0;
    diag_video_edges = 0;
}

static inline bool txq_enqueue(uint16_t fid, uint16_t lid, const void *data64) {
    uint32_t s = spin_lock_blocking(txq_lock);
    uint16_t w = txq_w;
    uint16_t next = (uint16_t)((w + 1) & TXQ_MASK);
    if (next == txq_r) {
        spin_unlock(txq_lock, s);
        return false;
    }

    uint8_t *p = txq[w];
    p[0] = 0xEB;
    p[1] = 0xD1;
    p[2] = (uint8_t)(fid & 0xFF);
    p[3] = (uint8_t)((fid >> 8) & 0xFF);
    p[4] = (uint8_t)(lid & 0xFF);
    p[5] = (uint8_t)((lid >> 8) & 0xFF);
    p[6] = (uint8_t)(BYTES_PER_LINE & 0xFF);
    p[7] = (uint8_t)((BYTES_PER_LINE >> 8) & 0xFF);
    memcpy(&p[8], data64, BYTES_PER_LINE);

    txq_w = next;
    spin_unlock(txq_lock, s);
    return true;
}

static inline bool vtxq_enqueue(const uint8_t *payload, uint16_t len) {
    uint32_t s = spin_lock_blocking(vtxq_lock);
    uint16_t w = vtxq_w;
    uint16_t next = (uint16_t)((w + 1) & VTXQ_MASK);
    if (next == vtxq_r) {
        spin_unlock(vtxq_lock, s);
        return false;
    }
    vtxq[w].len = len;
    memcpy(vtxq[w].data, payload, len);
    vtxq_w = next;
    spin_unlock(vtxq_lock, s);
    return true;
}

static bool rle_encode_bytes(const uint8_t *src, size_t src_len, uint8_t *dst, size_t dst_cap, size_t *out_len) {
    size_t out = 0;
    size_t i = 0;
    while (i < src_len) {
        uint8_t value = src[i];
        size_t run = 1;
        while ((i + run) < src_len && src[i + run] == value && run < 255) {
            run++;
        }
        if ((out + 2) > dst_cap) {
            return false;
        }
        dst[out++] = (uint8_t)run;
        dst[out++] = value;
        i += run;
    }
    *out_len = out;
    return true;
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
    uint32_t edge = vsync_fall_edge ? GPIO_IRQ_EDGE_FALL : GPIO_IRQ_EDGE_RISE;
    gpio_acknowledge_irq(PIN_VSYNC, GPIO_IRQ_EDGE_FALL | GPIO_IRQ_EDGE_RISE);
    gpio_set_irq_enabled_with_callback(PIN_VSYNC, edge, true, &gpio_irq);
}

static void gpio_irq(uint gpio, uint32_t events) {
    if (gpio != PIN_VSYNC) return;
    if (vsync_fall_edge) {
        if (!(events & GPIO_IRQ_EDGE_FALL)) return;
    } else {
        if (!(events & GPIO_IRQ_EDGE_RISE)) return;
    }

    uint32_t now_us = time_us_32();
    if ((uint32_t)(now_us - last_vsync_us) < 8000u) {
        return;
    }
    last_vsync_us = now_us;

    vsync_edges++;

    if (capture.capture_enabled) {
        return;
    }
    if (!armed) {
        return;
    }
    bool tx_busy = (frame_tx_buf != NULL) || capture.frame_ready;
#if VIDEO_STREAM_USB
    tx_busy = tx_busy || !txq_is_empty();
#endif
#if VIDEO_STREAM_UDP
    tx_busy = tx_busy || !vtxq_is_empty();
#endif
    want_frame = !tx_busy;

    if (want_frame) {
        video_capture_start(&capture, true);
    }
}

static inline void update_stream_idle(void) {
    bool idle = !capture.capture_enabled && !test_frame_active && !capture.frame_ready && (frame_tx_buf == NULL);
#if VIDEO_STREAM_USB
    idle = idle && txq_is_empty();
#endif
#if VIDEO_STREAM_UDP
    idle = idle && vtxq_is_empty();
#endif
    stream_idle = idle;
}

static void run_gpio_diag(void) {
    const uint32_t diag_ms = 500;

    armed = false;
    want_frame = false;
    video_capture_stop(&capture);
    txq_reset();
    reset_frame_tx_state();

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
        bool pixclk = gpio_get(PIN_PIXCLK);
        bool hsync = gpio_get(PIN_HSYNC);
        bool vsync = gpio_get(PIN_VSYNC);
        bool video = gpio_get(PIN_VIDEO);
        if (pixclk != prev_pixclk) diag_pixclk_edges++;
        if (hsync != prev_hsync) diag_hsync_edges++;
        if (vsync != prev_vsync) diag_vsync_edges++;
        if (video != prev_video) diag_video_edges++;
        prev_pixclk = pixclk;
        prev_hsync = hsync;
        prev_vsync = vsync;
        prev_video = video;
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
    if (!test_frame_active || capture.capture_enabled) return;

    while (test_frame_active) {
        uint8_t fill = (test_line & 1) ? 0xFF : 0x00;
        memset(test_line_buf, fill, BYTES_PER_LINE);
#if VIDEO_STREAM_USB
        if (!txq_enqueue(frame_id, test_line, test_line_buf)) {
            lines_drop++;
            break;
        }
#elif VIDEO_STREAM_UDP
        uint8_t packet[UDP_HDR_BYTES + UDP_RLE_MAX_BYTES];
        size_t rle_len = 0;
        if (!rle_encode_bytes(test_line_buf, BYTES_PER_LINE, &packet[UDP_HDR_BYTES], sizeof(packet) - UDP_HDR_BYTES, &rle_len)) {
            break;
        }
        packet[0] = UDP_MAGIC0;
        packet[1] = UDP_MAGIC1;
        packet[2] = UDP_VERSION;
        packet[3] = UDP_FORMAT_RLE8;
        packet[4] = (uint8_t)(frame_id & 0xFF);
        packet[5] = (uint8_t)((frame_id >> 8) & 0xFF);
        packet[6] = (uint8_t)(test_line & 0xFF);
        packet[7] = (uint8_t)((test_line >> 8) & 0xFF);
        packet[8] = (uint8_t)(rle_len & 0xFF);
        packet[9] = (uint8_t)((rle_len >> 8) & 0xFF);
        if (!vtxq_enqueue(packet, (uint16_t)(UDP_HDR_BYTES + rle_len))) {
            lines_drop++;
            break;
        }
#else
        (void)fill;
#endif

        test_line++;
        if (test_line >= CAP_ACTIVE_H) {
            test_frame_active = false;
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

    uint16_t lines_sent = 0;
    uint16_t line_budget = CAP_ACTIVE_H;
#if VIDEO_STREAM_UDP && !VIDEO_STREAM_USB
    line_budget = 16;
#endif

    while (frame_tx_line < CAP_ACTIVE_H) {
        if (lines_sent >= line_budget) break;
#if VIDEO_STREAM_USB
        if (!txq_has_space()) break;
#endif

        uint16_t src_line = (uint16_t)(frame_tx_line + frame_tx_start);
        if (src_line >= frame_tx_lines) {
            capture.frame_short++;
            frame_tx_buf = NULL;
            video_capture_set_inflight(&capture, NULL);
            return;
        }
        memcpy(frame_tx_line_buf, frame_tx_buf[src_line], sizeof(frame_tx_line_buf));
        reorder_line_words(frame_tx_line_buf);

#if VIDEO_STREAM_USB
        if (!txq_enqueue(frame_tx_id, frame_tx_line, (uint8_t *)frame_tx_line_buf)) {
            lines_drop++;
            break;
        }
#elif VIDEO_STREAM_UDP
        uint8_t packet[UDP_HDR_BYTES + UDP_RLE_MAX_BYTES];
        size_t rle_len = 0;
        if (!rle_encode_bytes((uint8_t *)frame_tx_line_buf, BYTES_PER_LINE, &packet[UDP_HDR_BYTES], sizeof(packet) - UDP_HDR_BYTES, &rle_len)) {
            break;
        }
        packet[0] = UDP_MAGIC0;
        packet[1] = UDP_MAGIC1;
        packet[2] = UDP_VERSION;
        packet[3] = UDP_FORMAT_RLE8;
        packet[4] = (uint8_t)(frame_tx_id & 0xFF);
        packet[5] = (uint8_t)((frame_tx_id >> 8) & 0xFF);
        packet[6] = (uint8_t)(frame_tx_line & 0xFF);
        packet[7] = (uint8_t)((frame_tx_line >> 8) & 0xFF);
        packet[8] = (uint8_t)(rle_len & 0xFF);
        packet[9] = (uint8_t)((rle_len >> 8) & 0xFF);
        if (!vtxq_enqueue(packet, (uint16_t)(UDP_HDR_BYTES + rle_len))) {
            lines_drop++;
            break;
        }
#else
        (void)lines_sent;
#endif

        frame_tx_line++;
        lines_sent++;
    }

    if (frame_tx_line >= CAP_ACTIVE_H) {
        frame_tx_buf = NULL;
        video_capture_set_inflight(&capture, NULL);
    }
}

static void try_process_command(uint32_t cmd) {
    switch ((video_command_t)cmd) {
        case VIDEO_CMD_START:
            armed = true;
            break;
        case VIDEO_CMD_STOP:
            armed = false;
            want_frame = false;
            video_capture_stop(&capture);
            txq_reset();
            reset_frame_tx_state();
            test_frame_active = false;
            test_line = 0;
            break;
        case VIDEO_CMD_RESET:
            frames_done = 0;
            frame_id = 0;
            capture.lines_ok = 0;
            capture.frame_overrun = 0;
            capture.frame_short = 0;
            lines_drop = 0;
            stream_drops = 0;
            vsync_edges = 0;
            want_frame = false;
            video_capture_stop(&capture);
            txq_reset();
            reset_frame_tx_state();
            test_frame_active = false;
            test_line = 0;
            break;
        case VIDEO_CMD_FORCE_CAPTURE:
            if (!capture.capture_enabled) {
                want_frame = true;
                video_capture_start(&capture, true);
            }
            break;
        case VIDEO_CMD_TEST_FRAME:
            armed = false;
            want_frame = false;
            video_capture_stop(&capture);
            txq_reset();
            reset_frame_tx_state();
            test_frame_active = true;
            test_line = 0;
            break;
        case VIDEO_CMD_TOGGLE_VSYNC:
            vsync_fall_edge = !vsync_fall_edge;
            armed = false;
            want_frame = false;
            video_capture_stop(&capture);
            txq_reset();
            reset_frame_tx_state();
            configure_vsync_irq();
            break;
        case VIDEO_CMD_GPIO_DIAG:
            run_gpio_diag();
            break;
        case VIDEO_CMD_PS_ON:
            armed = true;
            if (!capture.capture_enabled) {
                want_frame = true;
                video_capture_start(&capture, true);
            }
            break;
        case VIDEO_CMD_PS_OFF:
            armed = false;
            want_frame = false;
            video_capture_stop(&capture);
            txq_reset();
            reset_frame_tx_state();
            break;
        case VIDEO_CMD_NONE:
        default:
            break;
    }
}

static void video_core1_loop(void) {
    configure_vsync_irq();

    while (true) {
        if (multicore_fifo_rvalid()) {
            uint32_t cmd = multicore_fifo_pop_blocking();
            try_process_command(cmd);
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
        update_stream_idle();
        tight_loop_contents();
    }
}

#if VIDEO_STREAM_UDP
static void udp_stream_recv(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                            const ip_addr_t *addr, u16_t port) {
    (void)arg;
    if (!p) return;
    udp_stream.client_addr = *addr;
    udp_stream.client_port = port;
    udp_stream.client_set = true;
    udp_stream.ready = true;
    if (!armed) {
        multicore_fifo_push_blocking(VIDEO_CMD_START);
    }
    pbuf_free(p);
}

static bool udp_stream_send(const uint8_t *payload, size_t len) {
    if (!udp_stream.ready || !udp_stream.pcb || !udp_stream.client_set) return false;

    bool ok = false;
    cyw43_arch_lwip_begin();
    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, (uint16_t)len, PBUF_RAM);
    if (p) {
        memcpy(p->payload, payload, len);
        err_t err = udp_sendto(udp_stream.pcb, p, &udp_stream.client_addr, udp_stream.client_port);
        ok = (err == ERR_OK);
        pbuf_free(p);
    }
    cyw43_arch_lwip_end();
    return ok;
}
#endif

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

    while (probe_offset < PKT_BYTES) {
        int avail = tud_cdc_write_available();
        if (avail <= 0) return false;
        uint32_t to_write = (uint32_t)avail;
        uint32_t remain = (uint32_t)(PKT_BYTES - probe_offset);
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

void video_stream_init(PIO init_pio, uint init_sm, int init_dma_chan) {
    pio = init_pio;
    sm = init_sm;
    dma_chan = init_dma_chan;

    txq_lock = spin_lock_instance(spin_lock_claim_unused(true));
    vtxq_lock = spin_lock_instance(spin_lock_claim_unused(true));

    gpio_init(PIN_PIXCLK); gpio_set_dir(PIN_PIXCLK, GPIO_IN); gpio_disable_pulls(PIN_PIXCLK);
    gpio_init(PIN_VIDEO);  gpio_set_dir(PIN_VIDEO,  GPIO_IN); gpio_disable_pulls(PIN_VIDEO);
    gpio_init(PIN_HSYNC);  gpio_set_dir(PIN_HSYNC,  GPIO_IN); gpio_disable_pulls(PIN_HSYNC);
    gpio_init(PIN_VSYNC);  gpio_set_dir(PIN_VSYNC,  GPIO_IN); gpio_disable_pulls(PIN_VSYNC);
    gpio_init(PIN_PS_ON);  gpio_set_dir(PIN_PS_ON, GPIO_OUT); gpio_put(PIN_PS_ON, 0);

    gpio_acknowledge_irq(PIN_VSYNC, GPIO_IRQ_EDGE_FALL);

    gpio_set_function(PIN_PIXCLK, GPIO_FUNC_PIO0);
    gpio_set_function(PIN_HSYNC,  GPIO_FUNC_PIO0);
    gpio_set_function(PIN_VIDEO,  GPIO_FUNC_PIO0);

    pio_sm_set_consecutive_pindirs(pio, sm, PIN_PIXCLK, 1, false);
    pio_sm_set_consecutive_pindirs(pio, sm, PIN_HSYNC,  1, false);
    pio_sm_set_consecutive_pindirs(pio, sm, PIN_VIDEO,  1, false);

    offset_fall_pixrise = pio_add_program(pio, &classic_line_fall_pixrise_program);
    configure_pio_program();

    video_capture_init(&capture, pio, sm, dma_chan, framebuf_a, framebuf_b);
    video_capture_stop(&capture);
    txq_reset();
    reset_frame_tx_state();
}

void video_stream_start_core1(void) {
    multicore_launch_core1(video_core1_loop);
}

void video_stream_poll_network(void) {
#if VIDEO_STREAM_UDP
    while (true) {
        uint16_t r;
        uint16_t w;
        uint32_t s = spin_lock_blocking(vtxq_lock);
        r = vtxq_r;
        w = vtxq_w;
        if (r == w) {
            spin_unlock(vtxq_lock, s);
            break;
        }
        uint16_t len = vtxq[r].len;
        uint8_t packet[UDP_HDR_BYTES + UDP_RLE_MAX_BYTES];
        memcpy(packet, vtxq[r].data, len);
        vtxq_r = (uint16_t)((r + 1) & VTXQ_MASK);
        spin_unlock(vtxq_lock, s);

        if (!udp_stream_send(packet, len)) {
            stream_drops++;
            break;
        }
    }
#endif
}

static void video_stream_service_usb_queue(void) {
#if VIDEO_STREAM_USB
    if (!tud_cdc_connected()) return;

    bool wrote_any = false;
    static uint16_t txq_offset = 0;

    while (true) {
        uint16_t r, w;
        uint32_t s = spin_lock_blocking(txq_lock);
        r = txq_r;
        w = txq_w;
        spin_unlock(txq_lock, s);

        if (r == w) break;

        int avail = tud_cdc_write_available();
        if (avail <= 0) break;

        uint32_t remain = (uint32_t)(PKT_BYTES - txq_offset);
        uint32_t to_write = (uint32_t)avail;
        if (to_write > remain) {
            to_write = remain;
        }

        uint32_t n = tud_cdc_write(&txq[r][txq_offset], to_write);
        if (n == 0) {
            stream_drops++;
            break;
        }

        txq_offset = (uint16_t)(txq_offset + n);
        wrote_any = true;

        if (txq_offset >= PKT_BYTES) {
            txq_offset = 0;
            s = spin_lock_blocking(txq_lock);
            txq_r = (uint16_t)((r + 1) & TXQ_MASK);
            spin_unlock(txq_lock, s);
        }
    }

    if (wrote_any) {
        tud_cdc_write_flush();
    }
#endif
}

bool video_stream_udp_init(const wifi_config_t *cfg) {
#if VIDEO_STREAM_UDP
    if (!cfg || cfg->udp_port == 0) {
        printf("[EBD_IPKVM] udp disabled: missing listen port\n");
        return false;
    }

    udp_stream.pcb = udp_new_ip_type(IPADDR_TYPE_V4);
    if (!udp_stream.pcb) {
        printf("[EBD_IPKVM] udp pcb alloc failed\n");
        return false;
    }

    err_t err = udp_bind(udp_stream.pcb, IP_ADDR_ANY, cfg->udp_port);
    if (err != ERR_OK) {
        printf("[EBD_IPKVM] udp bind failed: %d\n", err);
        udp_remove(udp_stream.pcb);
        udp_stream.pcb = NULL;
        return false;
    }
    udp_recv(udp_stream.pcb, udp_stream_recv, NULL);

    udp_stream.ready = false;
    udp_stream.client_set = false;
    printf("[EBD_IPKVM] udp video listening on port %u\n", (unsigned)cfg->udp_port);
    return true;
#else
    (void)cfg;
    return false;
#endif
}

bool video_stream_udp_ready(void) {
#if VIDEO_STREAM_UDP
    return udp_stream.ready;
#else
    return false;
#endif
}

void video_stream_set_ps_on(bool on) {
    ps_on_state = on;
    gpio_put(PIN_PS_ON, on ? 1 : 0);
    multicore_fifo_push_blocking(on ? VIDEO_CMD_PS_ON : VIDEO_CMD_PS_OFF);
}

void video_stream_start_capture(void) {
    multicore_fifo_push_blocking(VIDEO_CMD_START);
}

void video_stream_stop_capture(void) {
    multicore_fifo_push_blocking(VIDEO_CMD_STOP);
}

void video_stream_reset_counters(void) {
    multicore_fifo_push_blocking(VIDEO_CMD_RESET);
}

void video_stream_force_capture(void) {
    multicore_fifo_push_blocking(VIDEO_CMD_FORCE_CAPTURE);
}

void video_stream_start_test_frame(void) {
    multicore_fifo_push_blocking(VIDEO_CMD_TEST_FRAME);
}

void video_stream_request_probe(void) {
    probe_offset = 0;
    probe_pending = 1;
}

void video_stream_request_debug(void) {
    debug_requested = true;
}

void video_stream_request_gpio_diag(void) {
    multicore_fifo_push_blocking(VIDEO_CMD_GPIO_DIAG);
}

void video_stream_toggle_vsync_edge(void) {
    multicore_fifo_push_blocking(VIDEO_CMD_TOGGLE_VSYNC);
}

void video_stream_park_forever(void) {
    while (true) {
        tud_task();
        sleep_ms(50);
    }
}

bool video_stream_can_emit_text(void) {
    return stream_idle && !armed;
}

void video_stream_emit_debug_state(void) {
    if (!tud_cdc_connected()) return;
    printf("[EBD_IPKVM] dbg armed=%d cap=%d test=%d probe=%d vsync=%s txq_r=%u txq_w=%u write_avail=%d frames=%lu lines=%lu drops=%lu stream=%lu frame_overrun=%lu short=%lu\n",
           armed ? 1 : 0,
           capture.capture_enabled ? 1 : 0,
           test_frame_active ? 1 : 0,
           probe_pending ? 1 : 0,
           vsync_fall_edge ? "fall" : "rise",
           (unsigned)txq_r,
           (unsigned)txq_w,
           tud_cdc_write_available(),
           (unsigned long)frames_done,
           (unsigned long)capture.lines_ok,
           (unsigned long)lines_drop,
           (unsigned long)stream_drops,
           (unsigned long)capture.frame_overrun,
           (unsigned long)capture.frame_short);
}

void video_stream_get_status(video_status_t *out) {
    if (!out) return;
    out->armed = armed;
    out->capture_enabled = capture.capture_enabled;
    out->ps_on = ps_on_state;
    out->test_active = test_frame_active;
    out->probe_pending = probe_pending;
    out->vsync_fall_edge = vsync_fall_edge;
    out->frames_done = frames_done;
    out->lines_ok = capture.lines_ok;
    out->lines_drop = lines_drop;
    out->stream_drops = stream_drops;
    out->frame_overrun = capture.frame_overrun;
    out->frame_short = capture.frame_short;
    out->vsync_edges = vsync_edges;
}

void video_stream_poll_usb(void) {
#if VIDEO_STREAM_USB
    video_stream_service_usb_queue();
#endif
    if (probe_pending && try_send_probe_packet()) {
        probe_pending = 0;
    }
    if (debug_requested && video_stream_can_emit_text()) {
        debug_requested = false;
        video_stream_emit_debug_state();
    }
}
