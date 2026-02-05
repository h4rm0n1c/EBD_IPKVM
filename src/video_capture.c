#include "video_capture.h"

#include "hardware/dma.h"

static inline void arm_dma(video_capture_t *cap, uint32_t *dst, uint32_t word_count) {
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
        word_count,
        true
    );
}

static inline void arm_postprocess_dma(video_capture_t *cap, uint32_t *dst, uint32_t word_count) {
    dma_channel_config c = dma_channel_get_default_config(cap->post_dma_chan);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, true);
    channel_config_set_bswap(&c, true);
    channel_config_set_dreq(&c, DREQ_FORCE);

    dma_channel_configure(
        cap->post_dma_chan,
        &c,
        dst,
        dst,
        word_count,
        true
    );
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
                        int post_dma_chan,
                        uint32_t framebuf_a[CAP_MAX_LINES][CAP_WORDS_PER_LINE],
                        uint32_t framebuf_b[CAP_MAX_LINES][CAP_WORDS_PER_LINE]) {
    cap->pio = pio;
    cap->sm = sm;
    cap->dma_chan = dma_chan;
    cap->post_dma_chan = post_dma_chan;
    cap->capture_enabled = false;
    cap->capture_want_frame = false;
    cap->lines_ok = 0;
    cap->framebuf_a = framebuf_a;
    cap->framebuf_b = framebuf_b;
    cap->capture_buf = framebuf_a;
    cap->ready_buf = NULL;
    cap->inflight_buf = NULL;
    cap->postprocess_pending = false;
    cap->postprocess_wanted = false;
    cap->postprocess_buf = NULL;
    cap->postprocess_frame_id = 0;
    cap->postprocess_lines = 0;
    cap->frame_ready = false;
    cap->frame_ready_id = 0;
    cap->frame_ready_lines = 0;
    cap->frame_overrun = 0;
    cap->frame_short = 0;
}

void video_capture_stop(video_capture_t *cap) {
    cap->capture_enabled = false;
    cap->capture_want_frame = false;
    pio_sm_set_enabled(cap->pio, cap->sm, false);
    dma_channel_abort(cap->dma_chan);
    dma_hw->ints0 = 1u << cap->dma_chan;
    dma_channel_abort(cap->post_dma_chan);
    dma_hw->ints0 = 1u << cap->post_dma_chan;
    pio_sm_clear_fifos(cap->pio, cap->sm);
    pio_sm_restart(cap->pio, cap->sm);
    cap->postprocess_pending = false;
    cap->postprocess_wanted = false;
    cap->postprocess_buf = NULL;
}

void video_capture_start(video_capture_t *cap, bool want_frame) {
    cap->capture_want_frame = want_frame;
    cap->capture_buf = select_capture_buffer(cap);
    cap->capture_enabled = true;
    pio_sm_clear_fifos(cap->pio, cap->sm);
    pio_sm_restart(cap->pio, cap->sm);
    arm_dma(cap, &cap->capture_buf[0][0], CAP_MAX_LINES * CAP_WORDS_PER_LINE);
    pio_sm_set_enabled(cap->pio, cap->sm, true);
}

bool video_capture_finalize_frame(video_capture_t *cap, uint16_t frame_id) {
    bool was_wanted = cap->capture_want_frame;
    uint32_t remaining_words = 0;
    uint32_t words_done = 0;
    uint16_t lines_captured = 0;

    if (!cap->capture_enabled) {
        return false;
    }

    pio_sm_set_enabled(cap->pio, cap->sm, false);
    remaining_words = dma_channel_hw_addr(cap->dma_chan)->transfer_count;
    dma_channel_abort(cap->dma_chan);
    dma_hw->ints0 = 1u << cap->dma_chan;
    pio_sm_clear_fifos(cap->pio, cap->sm);
    pio_sm_restart(cap->pio, cap->sm);

    cap->capture_enabled = false;
    cap->capture_want_frame = false;

    words_done = (CAP_MAX_LINES * CAP_WORDS_PER_LINE) - remaining_words;
    lines_captured = (uint16_t)(words_done / CAP_WORDS_PER_LINE);
    if (lines_captured > CAP_MAX_LINES) {
        lines_captured = CAP_MAX_LINES;
    }
    cap->lines_ok += lines_captured;

    if (!was_wanted) {
        return false;
    }

    if (lines_captured > 0) {
        arm_postprocess_dma(cap, &cap->capture_buf[0][0], (uint32_t)lines_captured * CAP_WORDS_PER_LINE);
        cap->postprocess_pending = true;
        cap->postprocess_wanted = true;
        cap->postprocess_buf = cap->capture_buf;
        cap->postprocess_frame_id = frame_id;
        cap->postprocess_lines = lines_captured;
    }

    if (!cap->postprocess_pending) {
        return false;
    }

    if (dma_channel_is_busy(cap->post_dma_chan)) {
        return false;
    }

    dma_hw->ints0 = 1u << cap->post_dma_chan;
    cap->postprocess_pending = false;
    cap->postprocess_wanted = false;
    cap->postprocess_buf = NULL;
    cap->postprocess_frame_id = 0;
    cap->postprocess_lines = 0;

    if (cap->frame_ready && cap->ready_buf != cap->capture_buf) {
        cap->frame_overrun++;
    }

    cap->ready_buf = cap->capture_buf;
    cap->frame_ready_id = frame_id;
    cap->frame_ready_lines = lines_captured;
    cap->frame_ready = true;
    return true;
}

bool video_capture_take_ready(video_capture_t *cap,
                              uint32_t (**out_buf)[CAP_WORDS_PER_LINE],
                              uint16_t *out_frame_id,
                              uint16_t *out_lines) {
    if (!cap->frame_ready || cap->ready_buf == NULL) {
        return false;
    }

    *out_buf = cap->ready_buf;
    *out_frame_id = cap->frame_ready_id;
    *out_lines = cap->frame_ready_lines;
    cap->ready_buf = NULL;
    cap->frame_ready = false;
    return true;
}

void video_capture_set_inflight(video_capture_t *cap, uint32_t (*buf)[CAP_WORDS_PER_LINE]) {
    cap->inflight_buf = buf;
}

bool video_capture_service_postprocess(video_capture_t *cap) {
    if (!cap->postprocess_pending) {
        return false;
    }
    if (dma_channel_is_busy(cap->post_dma_chan)) {
        return false;
    }

    dma_hw->ints0 = 1u << cap->post_dma_chan;
    cap->postprocess_pending = false;

    if (!cap->postprocess_wanted || cap->postprocess_buf == NULL) {
        cap->postprocess_wanted = false;
        cap->postprocess_buf = NULL;
        cap->postprocess_frame_id = 0;
        cap->postprocess_lines = 0;
        return false;
    }

    if (cap->frame_ready && cap->ready_buf != cap->postprocess_buf) {
        cap->frame_overrun++;
    }

    cap->ready_buf = cap->postprocess_buf;
    cap->frame_ready_id = cap->postprocess_frame_id;
    cap->frame_ready_lines = cap->postprocess_lines;
    cap->frame_ready = true;

    cap->postprocess_wanted = false;
    cap->postprocess_buf = NULL;
    cap->postprocess_frame_id = 0;
    cap->postprocess_lines = 0;
    return true;
}
