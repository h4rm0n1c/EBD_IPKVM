#ifndef EBD_IPKVM_VIDEO_STREAM_H
#define EBD_IPKVM_VIDEO_STREAM_H

#include <stdbool.h>
#include <stdint.h>

#include "hardware/pio.h"
#include "wifi_config.h"

#ifndef VIDEO_STREAM_UDP
#define VIDEO_STREAM_UDP 1
#endif

#ifndef VIDEO_STREAM_USB
#define VIDEO_STREAM_USB 0
#endif

typedef struct video_status {
    bool armed;
    bool capture_enabled;
    bool ps_on;
    bool test_active;
    bool probe_pending;
    bool vsync_fall_edge;
    uint32_t frames_done;
    uint32_t lines_ok;
    uint32_t lines_drop;
    uint32_t stream_drops;
    uint32_t frame_overrun;
    uint32_t frame_short;
    uint32_t vsync_edges;
} video_status_t;

void video_stream_init(PIO pio, uint sm, int dma_chan);
void video_stream_start_core1(void);
void video_stream_poll_network(void);
void video_stream_poll_usb(void);

bool video_stream_udp_init(const wifi_config_t *cfg);
bool video_stream_udp_ready(void);

void video_stream_set_ps_on(bool on);
void video_stream_start_capture(void);
void video_stream_stop_capture(void);
void video_stream_reset_counters(void);
void video_stream_force_capture(void);
void video_stream_start_test_frame(void);
void video_stream_request_probe(void);
void video_stream_request_debug(void);
void video_stream_request_gpio_diag(void);
void video_stream_toggle_vsync_edge(void);
void video_stream_park_forever(void);

bool video_stream_can_emit_text(void);
void video_stream_emit_debug_state(void);
void video_stream_get_status(video_status_t *out);

#endif
