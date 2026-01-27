#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "hardware/pio.h"

#define CAP_ACTIVE_H 342
#define CAP_YOFF_LINES 28
#define CAP_FRAME_LINES (CAP_YOFF_LINES + CAP_ACTIVE_H)
#define CAP_BYTES_PER_LINE 64
#define CAP_WORDS_PER_LINE (CAP_BYTES_PER_LINE / 4)
#define CAP_MAX_LINES CAP_FRAME_LINES

typedef struct video_capture {
    PIO pio;
    uint sm;
    int dma_chan;

    volatile bool capture_enabled;
    volatile bool capture_want_frame;
    volatile uint32_t lines_ok;

    uint32_t (*framebuf_a)[CAP_WORDS_PER_LINE];
    uint32_t (*framebuf_b)[CAP_WORDS_PER_LINE];
    uint32_t (*capture_buf)[CAP_WORDS_PER_LINE];
    uint32_t (*ready_buf)[CAP_WORDS_PER_LINE];
    uint32_t (*inflight_buf)[CAP_WORDS_PER_LINE];

    volatile bool frame_ready;
    uint16_t frame_ready_id;
    uint16_t frame_ready_lines;
    uint32_t frame_overrun;
    uint32_t frame_short;
} video_capture_t;

void video_capture_init(video_capture_t *cap,
                        PIO pio,
                        uint sm,
                        int dma_chan,
                        uint32_t framebuf_a[CAP_MAX_LINES][CAP_WORDS_PER_LINE],
                        uint32_t framebuf_b[CAP_MAX_LINES][CAP_WORDS_PER_LINE]);
void video_capture_start(video_capture_t *cap, bool want_frame);
void video_capture_stop(video_capture_t *cap);
bool video_capture_finalize_frame(video_capture_t *cap, uint16_t frame_id);
bool video_capture_take_ready(video_capture_t *cap,
                              uint32_t (**out_buf)[CAP_WORDS_PER_LINE],
                              uint16_t *out_frame_id,
                              uint16_t *out_lines);
void video_capture_set_inflight(video_capture_t *cap, uint32_t (*buf)[CAP_WORDS_PER_LINE]);
