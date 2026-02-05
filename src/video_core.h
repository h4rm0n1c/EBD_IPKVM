#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "hardware/pio.h"

#include "stream_protocol.h"
#include "video_capture.h"

typedef enum {
    CAPTURE_MODE_TEST_30FPS = 0,
    CAPTURE_MODE_CONTINUOUS_60FPS = 1,
} capture_mode_t;

typedef struct video_core_config {
    PIO pio;
    uint sm;
    int dma_chan;
    int post_dma_chan;
    uint offset_fall_pixrise;
    uint pin_video;
    uint pin_vsync;
} video_core_config_t;

#define VIDEO_CORE_MAX_PAYLOAD (CAP_BYTES_PER_LINE * 2)
#define VIDEO_CORE_MAX_PACKET_BYTES (STREAM_HEADER_BYTES + VIDEO_CORE_MAX_PAYLOAD)

void video_core_init(const video_core_config_t *cfg);
void video_core_launch(void);

bool video_core_can_emit_text(void);

void video_core_set_armed(bool armed);
bool video_core_is_armed(void);
void video_core_set_want_frame(bool want_frame);
void video_core_set_take_toggle(bool take_toggle);
void video_core_set_capture_mode(capture_mode_t mode);
capture_mode_t video_core_get_capture_mode(void);
void video_core_set_vsync_edge(bool fall_edge);
bool video_core_get_vsync_edge(void);
void video_core_set_tx_rle_enabled(bool enabled);
bool video_core_get_tx_rle_enabled(void);

bool video_core_capture_enabled(void);
bool video_core_test_frame_active(void);

uint32_t video_core_get_lines_drop(void);
uint32_t video_core_get_frames_done(void);
uint32_t video_core_get_lines_ok(void);
uint32_t video_core_get_frame_overrun(void);
uint32_t video_core_get_frame_short(void);
uint32_t video_core_take_vsync_edges(void);
void video_core_take_core1_utilization(uint32_t *busy_us, uint32_t *total_us);

bool video_core_txq_is_empty(void);
bool video_core_txq_peek(const uint8_t **out_data, uint16_t *out_len);
void video_core_txq_consume(void);
void video_core_get_txq_indices(uint16_t *out_r, uint16_t *out_w);
