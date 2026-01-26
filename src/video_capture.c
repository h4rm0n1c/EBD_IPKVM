#include "video_capture.h"

#include "hardware/dma.h"

static inline void arm_dma(video_capture_t *cap, uint32_t *dst) {
    dma_channel_config c = dma_channel_get_default_config(cap->dma_chan);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, true);
    channel_config_set_dreq(&c, pio_get_dreq(cap->pio, cap->sm, false)); // RX

    dma_channel_configure(
        cap->dma_chan,
        &c,
        dst,
        &cap->pio->rxf[cap->sm],
        CAP_WORDS_PER_LINE,
        true
    );
}

static inline uint32_t *line_dst_for(video_capture_t *cap, uint16_t line_idx) {
    if (line_idx >= CAP_YOFF_LINES && line_idx < (CAP_YOFF_LINES + CAP_ACTIVE_H)) {
        return cap->capture_buf[line_idx - CAP_YOFF_LINES];
    }
    return cap->line_sink;
}

static uint32_t (*select_capture_buffer(video_capture_t *cap))[CAP_WORDS_PER_LINE] {
    if (cap->inflight_buf == cap->framebuf_a) {
        if (cap->ready_buf == cap->framebuf_b) {
            cap->frame_overrun++;
            cap->frame_ready = false;
            cap->ready_buf = NULL;
        }
        return cap->framebuf_b;
    }

    if (cap->inflight_buf == cap->framebuf_b) {
        if (cap->ready_buf == cap->framebuf_a) {
            cap->frame_overrun++;
            cap->frame_ready = false;
            cap->ready_buf = NULL;
        }
        return cap->framebuf_a;
    }

    if (cap->ready_buf == cap->framebuf_a) {
        return cap->framebuf_b;
    }
    if (cap->ready_buf == cap->framebuf_b) {
        return cap->framebuf_a;
    }

    return cap->framebuf_a;
}

void video_capture_init(video_capture_t *cap,
                        PIO pio,
                        uint sm,
                        int dma_chan,
                        uint32_t framebuf_a[CAP_ACTIVE_H][CAP_WORDS_PER_LINE],
                        uint32_t framebuf_b[CAP_ACTIVE_H][CAP_WORDS_PER_LINE],
                        uint32_t line_sink[CAP_WORDS_PER_LINE]) {
    cap->pio = pio;
    cap->sm = sm;
    cap->dma_chan = dma_chan;
    cap->capture_enabled = false;
    cap->raw_line = 0;
    cap->capture_want_frame = false;
    cap->lines_ok = 0;
    cap->framebuf_a = framebuf_a;
    cap->framebuf_b = framebuf_b;
    cap->capture_buf = framebuf_a;
    cap->ready_buf = NULL;
    cap->inflight_buf = NULL;
    cap->line_sink = line_sink;
    cap->frame_ready = false;
    cap->frame_ready_id = 0;
    cap->frame_overrun = 0;
    cap->guard_trips = 0;
}

void video_capture_stop(video_capture_t *cap) {
    cap->capture_enabled = false;
    cap->capture_want_frame = false;
    pio_sm_set_enabled(cap->pio, cap->sm, false);
    dma_channel_abort(cap->dma_chan);
    dma_hw->ints0 = 1u << cap->dma_chan;
    pio_sm_clear_fifos(cap->pio, cap->sm);
    pio_sm_restart(cap->pio, cap->sm);
}

void video_capture_start(video_capture_t *cap, bool want_frame) {
    cap->capture_want_frame = want_frame;
    cap->raw_line = 0;
    cap->capture_buf = select_capture_buffer(cap);
    cap->capture_enabled = true;
    pio_sm_clear_fifos(cap->pio, cap->sm);
    pio_sm_restart(cap->pio, cap->sm);
    arm_dma(cap, line_dst_for(cap, 0));
    pio_sm_set_enabled(cap->pio, cap->sm, true);
}

bool video_capture_finalize_frame(video_capture_t *cap, uint16_t frame_id) {
    bool was_wanted = cap->capture_want_frame;

    if (!cap->capture_enabled) {
        return false;
    }

    video_capture_stop(cap);

    if (!was_wanted) {
        return false;
    }

    if (cap->frame_ready && cap->ready_buf != cap->capture_buf) {
        cap->frame_overrun++;
    }

    cap->ready_buf = cap->capture_buf;
    cap->frame_ready_id = frame_id;
    cap->frame_ready = true;
    return true;
}

void video_capture_dma_irq(video_capture_t *cap) {
    dma_hw->ints0 = 1u << cap->dma_chan;
    if (!cap->capture_enabled) return;

    cap->lines_ok++;

    cap->raw_line++;

    if (cap->raw_line >= CAP_LINES_GUARD) {
        cap->guard_trips++;
        cap->frame_overrun++;
        video_capture_stop(cap);
        return;
    }

    arm_dma(cap, line_dst_for(cap, cap->raw_line));
}

bool video_capture_take_ready(video_capture_t *cap,
                              uint32_t (**out_buf)[CAP_WORDS_PER_LINE],
                              uint16_t *out_frame_id) {
    if (!cap->frame_ready || cap->ready_buf == NULL) {
        return false;
    }

    *out_buf = cap->ready_buf;
    *out_frame_id = cap->frame_ready_id;
    cap->ready_buf = NULL;
    cap->frame_ready = false;
    return true;
}

void video_capture_set_inflight(video_capture_t *cap, uint32_t (*buf)[CAP_WORDS_PER_LINE]) {
    cap->inflight_buf = buf;
}
