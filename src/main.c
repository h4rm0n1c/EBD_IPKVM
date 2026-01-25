#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/gpio.h"
#include "hardware/watchdog.h"
#include "hardware/sync.h"
#include "pico/bootrom.h"

#include "tusb.h"
#include "classic_line.pio.h"

#define PIN_PIXCLK 0
#define PIN_VSYNC  1   // active-low
#define PIN_HSYNC  2   // active-low
#define PIN_VIDEO  3
#define PIN_PS_ON  9   // via ULN2803, GPIO high asserts ATX PS_ON

#define ACTIVE_H    342
#define YOFF_LINES  28
#define CAP_LINES   (YOFF_LINES + ACTIVE_H)   // 370 HSYNCs after VSYNC fall

#define BYTES_PER_LINE 64
#define WORDS_PER_LINE (BYTES_PER_LINE / 4)

#define PKT_HDR_BYTES 8
#define PKT_BYTES     (PKT_HDR_BYTES + BYTES_PER_LINE)

/* TX queue: power-of-two depth so we can mask wrap. */
#define TXQ_DEPTH 512
#define TXQ_MASK  (TXQ_DEPTH - 1)

static uint32_t line_a[WORDS_PER_LINE];
static uint32_t line_b[WORDS_PER_LINE];

static volatile bool using_a = true;

static volatile bool armed = false;            // host says start/stop
static volatile bool capture_enabled = false;  // SM+DMA running right now
static volatile bool want_frame = false;       // transmit this frame or skip
static volatile bool take_toggle = false;      // every other frame => ~30fps

static volatile uint16_t frame_id = 0;         // increments per transmitted frame
static volatile uint16_t raw_line = 0;         // 0..CAP_LINES-1

static volatile uint32_t lines_ok = 0;
/* repurpose as: queue overflows (we couldn't enqueue a line packet fast enough) */
static volatile uint32_t lines_drop = 0;
/* repurpose as: usb send failures (no space / disconnected / short write) */
static volatile uint32_t usb_drops = 0;

static volatile uint32_t vsync_edges = 0;
static volatile uint32_t frames_done = 0;
static volatile bool done_latched = false;
static volatile bool ps_on_state = false;
static volatile uint32_t diag_pixclk_edges = 0;
static volatile uint32_t diag_hsync_edges = 0;
static volatile uint32_t diag_vsync_edges = 0;
static volatile uint32_t diag_video_edges = 0;
static volatile bool test_frame_active = false;
static uint16_t test_line = 0;
static uint8_t test_line_buf[BYTES_PER_LINE];
static uint8_t probe_buf[PKT_BYTES];
static volatile uint8_t probe_pending = 0;
static uint16_t probe_offset = 0;
static volatile bool debug_requested = false;

static int dma_chan;
static PIO pio = pio0;
static uint sm = 0;
static uint offset_fall_pixrise = 0;
static uint offset_fall_pixfall = 0;
static uint offset_rise_pixrise = 0;
static uint offset_rise_pixfall = 0;
static bool hsync_fall_edge = false;
static bool vsync_fall_edge = true;
static bool pixclk_rise_edge = true;
static void gpio_irq(uint gpio, uint32_t events);

/* Ring buffer of complete line packets (72 bytes each). */
static uint8_t txq[TXQ_DEPTH][PKT_BYTES];
static volatile uint16_t txq_w = 0;
static volatile uint16_t txq_r = 0;

static inline void txq_reset(void) {
    uint32_t s = save_and_disable_interrupts();
    txq_w = 0;
    txq_r = 0;
    restore_interrupts(s);
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
    uint16_t r = txq_r;
    uint16_t w = txq_w;
    return r == w;
}

static inline bool can_emit_text(void) {
    return !capture_enabled && !test_frame_active && txq_is_empty() && (!armed || frames_done >= 100);
}

static inline bool txq_enqueue(uint16_t fid, uint16_t lid, const void *data64) {
    uint16_t w = txq_w;
    uint16_t next = (uint16_t)((w + 1) & TXQ_MASK);
    if (next == txq_r) {
        /* queue full */
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

    /* publish write index last so reader never sees a half-filled packet */
    txq_w = next;
    return true;
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

static inline void request_probe_packet(void) {
    probe_offset = 0;
    probe_pending = 1;
}

static void emit_debug_state(void) {
    if (!tud_cdc_connected()) return;
    printf("[EBD_IPKVM] dbg armed=%d cap=%d test=%d probe=%d hsync=%s vsync=%s pixclk=%s txq_r=%u txq_w=%u write_avail=%d frames=%lu lines=%lu drops=%lu usb=%lu\n",
           armed ? 1 : 0,
           capture_enabled ? 1 : 0,
           test_frame_active ? 1 : 0,
           probe_pending ? 1 : 0,
           hsync_fall_edge ? "fall" : "rise",
           vsync_fall_edge ? "fall" : "rise",
           pixclk_rise_edge ? "rise" : "fall",
           (unsigned)txq_r,
           (unsigned)txq_w,
           tud_cdc_write_available(),
           (unsigned long)frames_done,
           (unsigned long)lines_ok,
           (unsigned long)lines_drop,
           (unsigned long)usb_drops);
}

static inline void arm_dma(uint32_t *dst) {
    dma_channel_config c = dma_channel_get_default_config(dma_chan);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, true);
    channel_config_set_dreq(&c, pio_get_dreq(pio, sm, false)); // RX

    dma_channel_configure(
        dma_chan,
        &c,
        dst,
        &pio->rxf[sm],
        WORDS_PER_LINE,
        true
    );
}

static inline void stop_capture(void) {
    capture_enabled = false;
    pio_sm_set_enabled(pio, sm, false);
    dma_channel_abort(dma_chan);
    dma_hw->ints0 = 1u << dma_chan;
    pio_sm_clear_fifos(pio, sm);
    pio_sm_restart(pio, sm);
    using_a = true;
}

static void configure_pio_program(void) {
    pio_sm_set_enabled(pio, sm, false);
    pio_sm_clear_fifos(pio, sm);
    pio_sm_restart(pio, sm);
    if (hsync_fall_edge) {
        if (pixclk_rise_edge) {
            classic_line_fall_pixrise_program_init(pio, sm, offset_fall_pixrise, PIN_VIDEO);
        } else {
            classic_line_fall_pixfall_program_init(pio, sm, offset_fall_pixfall, PIN_VIDEO);
        }
    } else {
        if (pixclk_rise_edge) {
            classic_line_rise_pixrise_program_init(pio, sm, offset_rise_pixrise, PIN_VIDEO);
        } else {
            classic_line_rise_pixfall_program_init(pio, sm, offset_rise_pixfall, PIN_VIDEO);
        }
    }
    using_a = true;
}

static void configure_vsync_irq(void) {
    uint32_t edge = vsync_fall_edge ? GPIO_IRQ_EDGE_FALL : GPIO_IRQ_EDGE_RISE;
    gpio_acknowledge_irq(PIN_VSYNC, GPIO_IRQ_EDGE_FALL | GPIO_IRQ_EDGE_RISE);
    gpio_set_irq_enabled_with_callback(PIN_VSYNC, edge, true, &gpio_irq);
}

static inline void start_capture_window(void) {
    capture_enabled = true;
    pio_sm_clear_fifos(pio, sm);
    pio_sm_restart(pio, sm);
    using_a = true;
    arm_dma(line_a);
    pio_sm_set_enabled(pio, sm, true);
}

static void __isr dma_irq0_handler(void) {
    dma_hw->ints0 = 1u << dma_chan;
    if (!capture_enabled) return;

    uint32_t *buf = using_a ? line_a : line_b;

    lines_ok++;

    uint16_t this_raw = raw_line;
    raw_line++;

    if (want_frame && this_raw >= YOFF_LINES && this_raw < (YOFF_LINES + ACTIVE_H)) {
        uint16_t line_id = (uint16_t)(this_raw - YOFF_LINES);

        if (!txq_enqueue(frame_id, line_id, buf)) {
            lines_drop++; /* queue overflow */
        }
    }

    if (raw_line >= CAP_LINES) {
        stop_capture();
        if (want_frame) {
            frames_done++;
            want_frame = false;
            frame_id++;
        }
        return;
    }

    using_a = !using_a;
    arm_dma(using_a ? line_a : line_b);
}

static void gpio_irq(uint gpio, uint32_t events) {
    if (gpio != PIN_VSYNC) return;
    if (vsync_fall_edge) {
        if (!(events & GPIO_IRQ_EDGE_FALL)) return;
    } else {
        if (!(events & GPIO_IRQ_EDGE_RISE)) return;
    }

    vsync_edges++;

    if (!armed) return;
    if (capture_enabled) return; // ignore if we’re mid-frame
    if (frames_done >= 100) return;

    take_toggle = !take_toggle;          // every other VSYNC => ~30fps
    want_frame = take_toggle;

    raw_line = 0;
    start_capture_window();
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

    armed = false;
    want_frame = false;
    stop_capture();
    txq_reset();

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
    if (!test_frame_active || capture_enabled) return;

    while (test_frame_active) {
        uint8_t fill = (test_line & 1) ? 0xFF : 0x00;
        memset(test_line_buf, fill, BYTES_PER_LINE);
        if (!txq_enqueue(frame_id, test_line, test_line_buf)) {
            break;
        }

        test_line++;
        if (test_line >= ACTIVE_H) {
            test_frame_active = false;
            test_line = 0;
            frame_id++;
            frames_done++;
            break;
        }
    }
}

static void poll_cdc_commands(void) {
    // Reads single-byte commands: S=start, X=stop, R=reset counters, Q=park
    while (tud_cdc_available()) {
        uint8_t ch;
        if (tud_cdc_read(&ch, 1) != 1) break;

        if (ch == 'S' || ch == 's') {
            armed = true;
            /* IMPORTANT: do NOT printf here; keep binary stream clean once armed */
        } else if (ch == 'X' || ch == 'x') {
            armed = false;
            want_frame = false;
            stop_capture();
            txq_reset();
            test_frame_active = false;
            test_line = 0;
            if (can_emit_text()) {
                printf("[EBD_IPKVM] armed=0 (host stop)\n");
            }
        } else if (ch == 'R' || ch == 'r') {
            frames_done = 0;
            frame_id = 0;
            lines_ok = 0;
            lines_drop = 0;
            usb_drops = 0;
            vsync_edges = 0;
            take_toggle = false;
            want_frame = false;
            done_latched = false;
            stop_capture();
            txq_reset();
            test_frame_active = false;
            test_line = 0;
            if (can_emit_text()) {
                printf("[EBD_IPKVM] reset counters\n");
            }
        } else if (ch == 'Q' || ch == 'q') {
            armed = false;
            want_frame = false;
            stop_capture();
            txq_reset();
            test_frame_active = false;
            test_line = 0;
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
            armed = false;
            want_frame = false;
            stop_capture();
            txq_reset();
            sleep_ms(10);
            reset_usb_boot(0, 0);
        } else if (ch == 'Z' || ch == 'z') {
            armed = false;
            want_frame = false;
            stop_capture();
            txq_reset();
            sleep_ms(10);
            watchdog_reboot(0, 0, 0);
            while (true) { tight_loop_contents(); }
        } else if (ch == 'F' || ch == 'f') {
            if (!capture_enabled) {
                want_frame = true;
                raw_line = 0;
                start_capture_window();
            }
        } else if (ch == 'T' || ch == 't') {
            armed = false;
            want_frame = false;
            stop_capture();
            txq_reset();
            test_frame_active = true;
            test_line = 0;
            request_probe_packet();
        } else if (ch == 'U' || ch == 'u') {
            request_probe_packet();
        } else if (ch == 'I' || ch == 'i') {
            debug_requested = true;
        } else if (ch == 'G' || ch == 'g') {
            if (can_emit_text()) {
                run_gpio_diag();
            }
        } else if (ch == 'H' || ch == 'h') {
            hsync_fall_edge = !hsync_fall_edge;
            armed = false;
            want_frame = false;
            stop_capture();
            txq_reset();
            configure_pio_program();
            if (can_emit_text()) {
                printf("[EBD_IPKVM] hsync_edge=%s\n", hsync_fall_edge ? "fall" : "rise");
            }
        } else if (ch == 'K' || ch == 'k') {
            pixclk_rise_edge = !pixclk_rise_edge;
            armed = false;
            want_frame = false;
            stop_capture();
            txq_reset();
            configure_pio_program();
            if (can_emit_text()) {
                printf("[EBD_IPKVM] pixclk_edge=%s\n", pixclk_rise_edge ? "rise" : "fall");
            }
        } else if (ch == 'V' || ch == 'v') {
            vsync_fall_edge = !vsync_fall_edge;
            armed = false;
            want_frame = false;
            stop_capture();
            txq_reset();
            configure_vsync_irq();
            if (can_emit_text()) {
                printf("[EBD_IPKVM] vsync_edge=%s\n", vsync_fall_edge ? "fall" : "rise");
            }
        }
    }
}

static inline void service_txq(void) {
    if (!tud_cdc_connected()) return;

    bool wrote_any = false;
    static uint16_t txq_offset = 0;

    while (true) {
        uint16_t r, w;
        uint32_t s = save_and_disable_interrupts();
        r = txq_r;
        w = txq_w;
        restore_interrupts(s);

        if (r == w) break; /* empty */

        int avail = tud_cdc_write_available();
        if (avail <= 0) break;

        uint32_t remain = (uint32_t)(PKT_BYTES - txq_offset);
        uint32_t to_write = (uint32_t)avail;
        if (to_write > remain) {
            to_write = remain;
        }

        uint32_t n = tud_cdc_write(&txq[r][txq_offset], to_write);
        if (n == 0) {
            usb_drops++;
            break;
        }

        txq_offset = (uint16_t)(txq_offset + n);
        wrote_any = true;

        if (txq_offset >= PKT_BYTES) {
            txq_offset = 0;
            s = save_and_disable_interrupts();
            txq_r = (uint16_t)((r + 1) & TXQ_MASK);
            restore_interrupts(s);
        }
    }

    if (wrote_any) {
        tud_cdc_write_flush();
    }
}

int main(void) {
    stdio_init_all();
    sleep_ms(1200);

    printf("\n[EBD_IPKVM] USB packet stream @ ~30fps, capture 100 frames\n");
    printf("[EBD_IPKVM] WAITING for host. Send 'S' to start, 'X' stop, 'R' reset.\n");
    printf("[EBD_IPKVM] Power/control: 'P' on, 'p' off, 'B' BOOTSEL, 'Z' reset.\n");
    printf("[EBD_IPKVM] GPIO diag: send 'G' for pin states + edge counts.\n");
    printf("[EBD_IPKVM] Edge toggles: 'H' HSYNC edge, 'K' PIXCLK edge, 'V' VSYNC edge.\n");

    // SIO GPIO inputs + pulls (sane when Mac is off)
    gpio_init(PIN_PIXCLK); gpio_set_dir(PIN_PIXCLK, GPIO_IN); gpio_pull_down(PIN_PIXCLK);
    gpio_init(PIN_VIDEO);  gpio_set_dir(PIN_VIDEO,  GPIO_IN); gpio_pull_down(PIN_VIDEO);
    gpio_init(PIN_HSYNC);  gpio_set_dir(PIN_HSYNC,  GPIO_IN); gpio_pull_up(PIN_HSYNC);

    // VSYNC must remain SIO GPIO for IRQ to work
    gpio_init(PIN_VSYNC);  gpio_set_dir(PIN_VSYNC,  GPIO_IN); gpio_pull_up(PIN_VSYNC);
    gpio_init(PIN_PS_ON);  gpio_set_dir(PIN_PS_ON, GPIO_OUT); gpio_put(PIN_PS_ON, 0);

    // Clear any stale IRQ state, then enable callback
    gpio_acknowledge_irq(PIN_VSYNC, GPIO_IRQ_EDGE_FALL);
    configure_vsync_irq();

    // Hand ONLY the pins PIO needs to PIO0
    gpio_set_function(PIN_PIXCLK, GPIO_FUNC_PIO0);
    gpio_set_function(PIN_HSYNC,  GPIO_FUNC_PIO0);
    gpio_set_function(PIN_VIDEO,  GPIO_FUNC_PIO0);

    pio_sm_set_consecutive_pindirs(pio, sm, PIN_PIXCLK, 1, false);
    pio_sm_set_consecutive_pindirs(pio, sm, PIN_HSYNC,  1, false);
    pio_sm_set_consecutive_pindirs(pio, sm, PIN_VIDEO,  1, false);

    offset_fall_pixrise = pio_add_program(pio, &classic_line_fall_pixrise_program);
    offset_fall_pixfall = pio_add_program(pio, &classic_line_fall_pixfall_program);
    offset_rise_pixrise = pio_add_program(pio, &classic_line_rise_pixrise_program);
    offset_rise_pixfall = pio_add_program(pio, &classic_line_rise_pixfall_program);
    configure_pio_program();

    dma_chan = dma_claim_unused_channel(true);
    dma_channel_set_irq0_enabled(dma_chan, true);
    irq_set_exclusive_handler(DMA_IRQ_0, dma_irq0_handler);
    irq_set_enabled(DMA_IRQ_0, true);

    stop_capture();
    txq_reset();

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
        service_test_frame();
        service_txq();

        /* Keep status text off the wire while armed/capturing or while TX queue not empty. */
        bool can_report = !capture_enabled && !test_frame_active && txq_is_empty() && (!armed || frames_done >= 100);
        if (can_report) {
            if (absolute_time_diff_us(get_absolute_time(), next) <= 0) {
                next = delayed_by_ms(next, 1000);

                uint32_t l = lines_ok;
                uint32_t per_s = l - last_lines;
                last_lines = l;

                uint32_t ve = vsync_edges;
                vsync_edges = 0;

                printf("[EBD_IPKVM] armed=%d cap=%d ps_on=%d lines/s=%lu total=%lu q_drops=%lu usb_drops=%lu vsync_edges/s=%lu frames=%lu/100\n",
                       armed ? 1 : 0,
                       capture_enabled ? 1 : 0,
                       ps_on_state ? 1 : 0,
                       (unsigned long)per_s,
                       (unsigned long)l,
                       (unsigned long)lines_drop,
                       (unsigned long)usb_drops,
                       (unsigned long)ve,
                       (unsigned long)frames_done);
            }

            if (frames_done >= 100 && !done_latched) {
                printf("[EBD_IPKVM] done (100 frames). Send 'R' then 'S' to run again.\n");
                done_latched = true;
            }
        }

        tight_loop_contents();
    }
}
