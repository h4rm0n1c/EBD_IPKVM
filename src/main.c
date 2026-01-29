#include <stdio.h>

#include "pico/stdlib.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "hardware/watchdog.h"
#include "pico/bootrom.h"
#include "pico/cyw43_arch.h"

#include "tusb.h"

#include "portal.h"
#include "video_stream.h"
#include "wifi_config.h"

static void handle_ps_on(bool on, void *ctx) {
    (void)ctx;
    video_stream_set_ps_on(on);
}

static void poll_cdc_commands(void) {
    while (tud_cdc_available()) {
        uint8_t ch;
        if (tud_cdc_read(&ch, 1) != 1) break;

        if (ch == 'S' || ch == 's') {
            video_stream_start_capture();
        } else if (ch == 'X' || ch == 'x') {
            video_stream_stop_capture();
            if (video_stream_can_emit_text()) {
                printf("[EBD_IPKVM] armed=0 (host stop)\n");
            }
        } else if (ch == 'R' || ch == 'r') {
            video_stream_reset_counters();
            if (video_stream_can_emit_text()) {
                printf("[EBD_IPKVM] reset counters\n");
            }
        } else if (ch == 'Q' || ch == 'q') {
            video_stream_stop_capture();
            if (video_stream_can_emit_text()) {
                printf("[EBD_IPKVM] parked\n");
            }
            video_stream_park_forever();
        } else if (ch == 'P') {
            video_stream_set_ps_on(true);
            if (video_stream_can_emit_text()) {
                printf("[EBD_IPKVM] ps_on=1\n");
            }
        } else if (ch == 'p') {
            video_stream_set_ps_on(false);
            if (video_stream_can_emit_text()) {
                printf("[EBD_IPKVM] ps_on=0\n");
            }
        } else if (ch == 'B' || ch == 'b') {
            video_stream_stop_capture();
            sleep_ms(10);
            reset_usb_boot(0, 0);
        } else if (ch == 'Z' || ch == 'z') {
            video_stream_stop_capture();
            sleep_ms(10);
            watchdog_reboot(0, 0, 0);
            while (true) { tight_loop_contents(); }
        } else if (ch == 'F' || ch == 'f') {
            video_stream_force_capture();
        } else if (ch == 'T' || ch == 't') {
            video_stream_start_test_frame();
            video_stream_request_probe();
        } else if (ch == 'U' || ch == 'u') {
            video_stream_request_probe();
        } else if (ch == 'I' || ch == 'i') {
            video_stream_request_debug();
        } else if (ch == 'G' || ch == 'g') {
            if (video_stream_can_emit_text()) {
                video_stream_request_gpio_diag();
            }
        } else if (ch == 'V' || ch == 'v') {
            video_stream_toggle_vsync_edge();
            if (video_stream_can_emit_text()) {
                printf("[EBD_IPKVM] toggled vsync edge\n");
            }
        } else if (ch == 'W' || ch == 'w') {
            wifi_config_clear();
            printf("[EBD_IPKVM] wifi config cleared, rebooting into portal\n");
            sleep_ms(50);
            watchdog_reboot(0, 0, 0);
        }
    }
}

int main(void) {
    stdio_init_all();
    sleep_ms(1200);

    printf("\n[EBD_IPKVM] UDP RLE video stream @ ~60fps, continuous capture\n");
    printf("[EBD_IPKVM] WAITING for host. Send 'S' to start, 'X' stop, 'R' reset.\n");
    printf("[EBD_IPKVM] Power/control: 'P' on, 'p' off, 'B' BOOTSEL, 'Z' reset.\n");
    printf("[EBD_IPKVM] GPIO diag: send 'G' for pin states + edge counts.\n");
    printf("[EBD_IPKVM] Edge toggles: 'V' VSYNC edge.\n");

    int dma_chan = dma_claim_unused_channel(true);
    irq_set_priority(USBCTRL_IRQ, 1);

    video_stream_init(pio0, 0, dma_chan);
    video_stream_start_core1();

#if VIDEO_STREAM_UDP
    if (cyw43_arch_init()) {
        printf("[EBD_IPKVM] wifi init failed\n");
    } else {
        portal_init(handle_ps_on, NULL);
        if (portal_has_config() && portal_start_station()) {
            video_stream_udp_init(portal_get_config());
        } else {
            portal_start_portal();
        }
    }
#endif

    absolute_time_t next = make_timeout_time_ms(1000);
    uint32_t last_lines = 0;
    uint32_t last_vsync = 0;

    while (true) {
        tud_task();
        poll_cdc_commands();

#if VIDEO_STREAM_UDP
        portal_poll();
        if (portal_config_saved()) {
            portal_clear_config_saved();
            printf("[EBD_IPKVM] portal config saved, rebooting\n");
            sleep_ms(200);
            watchdog_reboot(0, 0, 0);
        }
#endif

        video_stream_poll_network();
        video_stream_poll_usb();

        if (video_stream_can_emit_text()) {
            if (absolute_time_diff_us(get_absolute_time(), next) <= 0) {
                next = delayed_by_ms(next, 1000);

                video_status_t status;
                video_stream_get_status(&status);

                uint32_t per_s = status.lines_ok - last_lines;
                last_lines = status.lines_ok;

                uint32_t ve = status.vsync_edges - last_vsync;
                last_vsync = status.vsync_edges;

                printf("[EBD_IPKVM] armed=%d cap=%d ps_on=%d lines/s=%lu total=%lu q_drops=%lu stream_drops=%lu frame_overrun=%lu vsync_edges/s=%lu frames=%lu\n",
                       status.armed ? 1 : 0,
                       status.capture_enabled ? 1 : 0,
                       status.ps_on ? 1 : 0,
                       (unsigned long)per_s,
                       (unsigned long)status.lines_ok,
                       (unsigned long)status.lines_drop,
                       (unsigned long)status.stream_drops,
                       (unsigned long)status.frame_overrun,
                       (unsigned long)ve,
                       (unsigned long)status.frames_done);
            }
        }
    }
}
