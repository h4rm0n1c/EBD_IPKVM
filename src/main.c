#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/gpio.h"
#include "hardware/watchdog.h"
#include "hardware/sync.h"
#include "pico/bootrom.h"
#include "pico/cyw43_arch.h"

#include "tusb.h"
#include "classic_line.pio.h"
#include "video_capture.h"
#include "wifi_config.h"

#define PIN_PIXCLK 0
#define PIN_VSYNC  1   // active-low
#define PIN_HSYNC  2   // active-low
#define PIN_VIDEO  3
#define PIN_PS_ON  9   // via ULN2803, GPIO high asserts ATX PS_ON

#define BYTES_PER_LINE CAP_BYTES_PER_LINE
#define WORDS_PER_LINE CAP_WORDS_PER_LINE

#define PKT_HDR_BYTES 8
#define PKT_BYTES     (PKT_HDR_BYTES + BYTES_PER_LINE)

#ifndef VIDEO_STREAM_UDP
#define VIDEO_STREAM_UDP 1
#endif

#ifndef VIDEO_STREAM_USB
#define VIDEO_STREAM_USB 0
#endif

#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif

#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD ""
#endif

#ifndef VIDEO_UDP_ADDR
#define VIDEO_UDP_ADDR "192.168.1.100"
#endif

#ifndef VIDEO_UDP_PORT
#define VIDEO_UDP_PORT 5004
#endif

#if VIDEO_STREAM_UDP
#include "cyw43.h"
#include "lwip/pbuf.h"
#include "lwip/udp.h"
#include "lwip/ip_addr.h"
#include "lwip/ip.h"
#include "lwip/ip4_addr.h"
#include "lwip/tcp.h"
#include "lwip/netif.h"
#endif

#define UDP_MAGIC0 0xEB
#define UDP_MAGIC1 0xD1
#define UDP_VERSION 1
#define UDP_FORMAT_RLE8 1
#define UDP_HDR_BYTES 10
#define UDP_RLE_MAX_BYTES (CAP_BYTES_PER_LINE * 2)

#define PORTAL_HTTP_PORT 80
#define PORTAL_DNS_PORT 53
#define PORTAL_DHCP_PORT 67
#define PORTAL_CLIENT_PORT 68
#define PORTAL_AP_SSID "EBD-IPKVM-Setup"
#define PORTAL_AP_PASS ""
#define PORTAL_AP_ADDR "192.168.4.1"
#define PORTAL_AP_NETMASK "255.255.255.0"
#define PORTAL_AP_LEASE "192.168.4.2"
#define PORTAL_IP_OCT1 192
#define PORTAL_IP_OCT2 168
#define PORTAL_IP_OCT3 4
#define PORTAL_IP_OCT4 1
#define PORTAL_LEASE_OCT4 2
#define PORTAL_MAX_SCAN 12
#define PORTAL_MAX_REQ 4096

/* TX queue: power-of-two depth so we can mask wrap. */
#define TXQ_DEPTH 512
#define TXQ_MASK  (TXQ_DEPTH - 1)

static volatile bool armed = false;            // host says start/stop
static volatile bool want_frame = false;       // transmit this frame or skip

static volatile uint16_t frame_id = 0;         // increments per transmitted frame
/* repurpose as: queue overflows (we couldn't enqueue a line packet fast enough) */
static volatile uint32_t lines_drop = 0;
/* repurpose as: stream send failures (no space / disconnected / short write) */
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
static bool vsync_fall_edge = true;
static uint offset_fall_pixrise = 0;
static void gpio_irq(uint gpio, uint32_t events);

/* Ring buffer of complete line packets (72 bytes each). */
static uint8_t txq[TXQ_DEPTH][PKT_BYTES];
static volatile uint16_t txq_w = 0;
static volatile uint16_t txq_r = 0;

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

typedef struct portal_scan_result {
    char ssid[WIFI_CFG_MAX_SSID + 1];
    int32_t rssi;
    uint8_t auth;
} portal_scan_result_t;

typedef struct portal_state {
    bool active;
    bool scan_in_progress;
    bool config_saved;
    bool has_config;
    bool ap_mode;
    wifi_config_t config;
    portal_scan_result_t scan_results[PORTAL_MAX_SCAN];
    uint8_t scan_count;
    struct udp_pcb *dns_pcb;
    struct udp_pcb *dhcp_pcb;
    struct tcp_pcb *http_pcb;
} portal_state_t;

static portal_state_t portal = {0};

__attribute__((weak)) int cyw43_arch_wifi_scan(cyw43_wifi_scan_options_t *opts,
                                               int (*result_cb)(void *, const cyw43_ev_scan_result_t *),
                                               void *env) {
    return cyw43_wifi_scan(&cyw43_state, opts, env, result_cb);
}

static inline void txq_reset(void) {
    uint32_t s = save_and_disable_interrupts();
    txq_w = 0;
    txq_r = 0;
    restore_interrupts(s);
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
    if (on) {
        armed = true;
        if (!capture.capture_enabled) {
            want_frame = true;
            video_capture_start(&capture, true);
        }
    }
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

static inline bool txq_has_space(void) {
    uint32_t s = save_and_disable_interrupts();
    uint16_t r = txq_r;
    uint16_t w = txq_w;
    restore_interrupts(s);
    return ((uint16_t)((w + 1) & TXQ_MASK)) != r;
}

static inline bool stream_is_idle(void) {
    bool idle = !capture.capture_enabled && !test_frame_active && !capture.frame_ready && (frame_tx_buf == NULL);
#if VIDEO_STREAM_USB
    idle = idle && txq_is_empty();
#endif
    return idle;
}

static inline bool can_emit_text(void) {
    return stream_is_idle() && !armed;
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

#if VIDEO_STREAM_UDP
static void udp_stream_recv(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                            const ip_addr_t *addr, u16_t port) {
    (void)arg;
    if (!p) return;
    udp_stream.client_addr = *addr;
    udp_stream.client_port = port;
    udp_stream.client_set = true;
    udp_stream.ready = true;
    pbuf_free(p);
}

static bool udp_stream_init(const wifi_config_t *cfg) {
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

static bool stream_send_line(uint16_t fid, uint16_t lid, const uint8_t *data64) {
#if VIDEO_STREAM_USB
    if (!txq_enqueue(fid, lid, data64)) {
        return false;
    }
    return true;
#elif VIDEO_STREAM_UDP
    uint8_t rle_buf[UDP_RLE_MAX_BYTES];
    uint8_t packet[UDP_HDR_BYTES + UDP_RLE_MAX_BYTES];
    size_t rle_len = 0;
    if (!rle_encode_bytes(data64, BYTES_PER_LINE, rle_buf, sizeof(rle_buf), &rle_len)) {
        return false;
    }
    packet[0] = UDP_MAGIC0;
    packet[1] = UDP_MAGIC1;
    packet[2] = UDP_VERSION;
    packet[3] = UDP_FORMAT_RLE8;
    packet[4] = (uint8_t)(fid & 0xFF);
    packet[5] = (uint8_t)((fid >> 8) & 0xFF);
    packet[6] = (uint8_t)(lid & 0xFF);
    packet[7] = (uint8_t)((lid >> 8) & 0xFF);
    packet[8] = (uint8_t)(rle_len & 0xFF);
    packet[9] = (uint8_t)((rle_len >> 8) & 0xFF);
    memcpy(&packet[UDP_HDR_BYTES], rle_buf, rle_len);
    bool ok = udp_stream_send(packet, UDP_HDR_BYTES + rle_len);
    if (!ok) {
        stream_drops++;
    }
    return ok;
#else
    (void)fid;
    (void)lid;
    (void)data64;
    return false;
#endif
}

#if VIDEO_STREAM_UDP
static void portal_reset_scan(void) {
    portal.scan_count = 0;
    for (size_t i = 0; i < PORTAL_MAX_SCAN; i++) {
        portal.scan_results[i].ssid[0] = '\0';
        portal.scan_results[i].rssi = 0;
        portal.scan_results[i].auth = 0;
    }
}

static void portal_defaults(void) {
    memset(&portal.config, 0, sizeof(portal.config));
    strncpy(portal.config.ssid, WIFI_SSID, WIFI_CFG_MAX_SSID);
    strncpy(portal.config.pass, WIFI_PASSWORD, WIFI_CFG_MAX_PASS);
    strncpy(portal.config.udp_addr, VIDEO_UDP_ADDR, WIFI_CFG_MAX_ADDR);
    portal.config.udp_port = VIDEO_UDP_PORT;
}

static size_t portal_url_decode(char *dst, size_t dst_len, const char *src, size_t src_len) {
    size_t out = 0;
    for (size_t i = 0; i < src_len && out + 1 < dst_len; i++) {
        char c = src[i];
        if (c == '+') {
            dst[out++] = ' ';
        } else if (c == '%' && (i + 2) < src_len) {
            char hex[3] = {src[i + 1], src[i + 2], 0};
            char *end = NULL;
            long v = strtol(hex, &end, 16);
            if (end && *end == '\0') {
                dst[out++] = (char)v;
                i += 2;
            } else {
                dst[out++] = c;
            }
        } else {
            dst[out++] = c;
        }
    }
    dst[out] = '\0';
    return out;
}

static bool portal_form_value(const char *body, const char *key, char *out, size_t out_len) {
    if (!body || !key || !out || out_len == 0) return false;
    size_t key_len = strlen(key);
    const char *p = body;
    while (*p) {
        const char *eq = strchr(p, '=');
        if (!eq) break;
        size_t name_len = (size_t)(eq - p);
        const char *amp = strchr(eq + 1, '&');
        size_t val_len = amp ? (size_t)(amp - (eq + 1)) : strlen(eq + 1);
        if (name_len == key_len && strncmp(p, key, key_len) == 0) {
            portal_url_decode(out, out_len, eq + 1, val_len);
            return true;
        }
        if (!amp) break;
        p = amp + 1;
    }
    return false;
}

static void portal_http_send(struct tcp_pcb *tpcb, const char *content_type, const char *body) {
    char header[128];
    int body_len = body ? (int)strlen(body) : 0;
    snprintf(header, sizeof(header),
             "HTTP/1.1 200 OK\r\n"
             "Content-Type: %s\r\n"
             "Content-Length: %d\r\n"
             "Connection: close\r\n\r\n",
             content_type, body_len);
    tcp_write(tpcb, header, strlen(header), TCP_WRITE_FLAG_COPY);
    if (body_len > 0) {
        tcp_write(tpcb, body, body_len, TCP_WRITE_FLAG_COPY);
    }
    tcp_output(tpcb);
}

static void portal_http_send_redirect(struct tcp_pcb *tpcb, const char *location) {
    char header[192];
    snprintf(header, sizeof(header),
             "HTTP/1.1 302 Found\r\n"
             "Location: %s\r\n"
             "Content-Length: 0\r\n"
             "Connection: close\r\n\r\n",
             location);
    tcp_write(tpcb, header, strlen(header), TCP_WRITE_FLAG_COPY);
    tcp_output(tpcb);
}

typedef struct {
    char buf[PORTAL_MAX_REQ];
    size_t len;
    size_t parse_pos;
    size_t body_offset;
    int content_length;
    bool request_line_done;
    bool headers_done;
    bool is_post;
    char path[64];
} portal_http_state_t;

static void portal_http_state_cleanup(struct tcp_pcb *tpcb, portal_http_state_t *state) {
    if (state) {
        free(state);
    }
    if (tpcb) {
        tcp_arg(tpcb, NULL);
        tcp_recv(tpcb, NULL);
        tcp_err(tpcb, NULL);
    }
}

static void portal_http_err(void *arg, err_t err) {
    (void)err;
    portal_http_state_t *state = (portal_http_state_t *)arg;
    if (state) {
        free(state);
    }
}

static int portal_find_crlf(const char *buf, size_t len) {
    if (len < 2) return -1;
    for (size_t i = 0; i + 1 < len; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n') {
            return (int)i;
        }
    }
    return -1;
}

static int portal_parse_content_length_line(const char *line, size_t len) {
    const char header[] = "Content-Length:";
    size_t header_len = sizeof(header) - 1;
    if (len < header_len || strncasecmp(line, header, header_len) != 0) {
        return -1;
    }
    const char *val = line + header_len;
    while ((size_t)(val - line) < len && (*val == ' ' || *val == '\t')) {
        val++;
    }
    return atoi(val);
}

static bool portal_parse_request_line(portal_http_state_t *state, const char *line, size_t len) {
    const char *space1 = memchr(line, ' ', len);
    if (!space1) return false;
    const char *space2 = memchr(space1 + 1, ' ', len - (size_t)(space1 + 1 - line));
    if (!space2) return false;
    size_t method_len = (size_t)(space1 - line);
    state->is_post = (method_len == 4 && strncmp(line, "POST", 4) == 0);
    size_t path_len = (size_t)(space2 - (space1 + 1));
    if (path_len >= sizeof(state->path)) {
        path_len = sizeof(state->path) - 1;
    }
    memcpy(state->path, space1 + 1, path_len);
    state->path[path_len] = '\0';
    if (strncmp(state->path, "http://", 7) == 0 || strncmp(state->path, "https://", 8) == 0) {
        const char *p = strstr(state->path, "://");
        if (p) {
            p += 3;
            const char *slash = strchr(p, '/');
            if (slash) {
                size_t new_len = strlen(slash);
                memmove(state->path, slash, new_len + 1);
            } else {
                strcpy(state->path, "/");
            }
        }
    }
    char *q = strchr(state->path, '?');
    if (q) {
        *q = '\0';
    }
    return true;
}

static void portal_send_index(struct tcp_pcb *tpcb) {
    char page[2048];
    int n = snprintf(page, sizeof(page),
                     "<!doctype html><html><head><meta charset=\"utf-8\">"
                     "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
                     "<title>EBD IPKVM Setup</title>"
                     "<style>body{font-family:sans-serif;margin:20px;}label{display:block;margin-top:8px;}"
                     "input,select{width:100%%;padding:8px;margin-top:4px;}button{margin-top:12px;padding:8px 12px;}"
                     ".ssid{display:flex;gap:8px;align-items:center;}</style>"
                     "</head><body>"
                     "<h2>EBD IPKVM Wi-Fi Setup</h2>"
                     "<p>Configure Wi-Fi and set the UDP listen port for video streaming.</p>"
                     "<p>Mode: <strong>%s</strong></p>"
                     "<div class=\"row\">"
                     "<button type=\"button\" id=\"scan_btn\">Scan Wi-Fi</button>"
                     "<span id=\"scan_status\"></span>"
                     "</div>"
                     "<ul id=\"scan_list\"></ul>"
                     "<form method=\"POST\" action=\"/save\">"
                     "<label>SSID</label><input name=\"ssid\" value=\"%s\" />"
                     "<label>Password</label><input name=\"pass\" type=\"password\" value=\"%s\" />"
                     "<label>UDP Listen Port</label><input name=\"udp_port\" value=\"%u\" />"
                     "<button type=\"submit\">Save &amp; Reboot</button>"
                     "</form>"
                     "<div class=\"row\">"
                     "<button type=\"button\" id=\"ps_on_btn\">Power On</button>"
                     "<button type=\"button\" id=\"ps_off_btn\">Power Off</button>"
                     "<span id=\"power_status\"></span>"
                     "</div>"
                     "<script>"
                     "const scanBtn=document.getElementById('scan_btn');"
                     "const scanList=document.getElementById('scan_list');"
                     "const scanStatus=document.getElementById('scan_status');"
                     "const powerStatus=document.getElementById('power_status');"
                     "const ssidInput=document.querySelector('input[name=\"ssid\"]');"
                     "async function scan(){"
                     "scanStatus.textContent='scanning...';"
                     "scanBtn.disabled=true;"
                     "try{"
                     "const res=await fetch('/scan');"
                     "const data=await res.json();"
                     "scanList.innerHTML='';"
                     "if(Array.isArray(data.results)){"
                     "data.results.forEach((r)=>{"
                     "const li=document.createElement('li');"
                     "const btn=document.createElement('button');"
                     "btn.type='button';"
                     "btn.textContent=`${r.ssid} (rssi ${r.rssi})`;"
                     "btn.addEventListener('click',()=>{ssidInput.value=r.ssid;});"
                     "li.appendChild(btn);"
                     "scanList.appendChild(li);"
                     "});"
                     "}"
                     "if(data.scanning){"
                     "scanStatus.textContent='scanning...';"
                     "setTimeout(scan,1000);"
                     "}else{"
                     "scanStatus.textContent='done';"
                     "scanBtn.disabled=false;"
                     "}"
                     "}catch(e){"
                     "scanStatus.textContent=`error: ${e.message}`;"
                     "scanBtn.disabled=false;"
                     "}"
                     "}"
                     "scanBtn.addEventListener('click',scan);"
                     "async function power(path){"
                     "powerStatus.textContent='...';"
                     "try{"
                     "await fetch(path,{method:'POST'});"
                     "powerStatus.textContent=path==='\\/ps_on'?'on':'off';"
                     "}catch(e){"
                     "powerStatus.textContent=`error: ${e.message}`;"
                     "}"
                     "}"
                     "document.getElementById('ps_on_btn').addEventListener('click',()=>power('/ps_on'));"
                     "document.getElementById('ps_off_btn').addEventListener('click',()=>power('/ps_off'));"
                     "window.addEventListener('error',(e)=>{scanStatus.textContent=`error: ${e.message}`;});"
                     "</script>"
                     "</body></html>",
                     portal.ap_mode ? "AP (setup)" : "Station",
                     portal.config.ssid,
                     portal.config.pass,
                     (unsigned)portal.config.udp_port);
    if (n < 0) {
        portal_http_send(tpcb, "text/plain", "render error");
        return;
    }
    portal_http_send(tpcb, "text/html", page);
}

static int portal_scan_callback(void *env, const cyw43_ev_scan_result_t *result) {
    (void)env;
    if (!result) {
        portal.scan_in_progress = false;
        return 0;
    }
    if (result->ssid_len == 0) return 0;
    if (portal.scan_count >= PORTAL_MAX_SCAN) return 0;
    portal_scan_result_t *slot = &portal.scan_results[portal.scan_count++];
    size_t len = result->ssid_len;
    if (len > WIFI_CFG_MAX_SSID) len = WIFI_CFG_MAX_SSID;
    memcpy(slot->ssid, result->ssid, len);
    slot->ssid[len] = '\0';
    slot->rssi = result->rssi;
    slot->auth = result->auth_mode;
    return 0;
}

static void portal_start_scan(void) {
    if (portal.scan_in_progress) return;
    portal_reset_scan();
    portal.scan_in_progress = true;
    cyw43_wifi_scan_options_t opts = {0};
    int err = cyw43_arch_wifi_scan(&opts, portal_scan_callback, NULL);
    if (err) {
        portal.scan_in_progress = false;
    }
}

static void portal_send_scan(struct tcp_pcb *tpcb) {
    if (!portal.scan_in_progress) {
        portal_start_scan();
    }
    char json[768];
    int n = snprintf(json, sizeof(json), "{\"scanning\":%s,\"results\":[",
                     portal.scan_in_progress ? "true" : "false");
    for (uint8_t i = 0; i < portal.scan_count && n > 0 && (size_t)n < sizeof(json); i++) {
        portal_scan_result_t *r = &portal.scan_results[i];
        int wrote = snprintf(json + n, sizeof(json) - (size_t)n,
                             "%s{\"ssid\":\"%s\",\"rssi\":%ld,\"auth\":%u}",
                             (i == 0) ? "" : ",",
                             r->ssid,
                             (long)r->rssi,
                             (unsigned)r->auth);
        if (wrote < 0) break;
        n += wrote;
    }
    if (n < 0) n = 0;
    if ((size_t)n < sizeof(json)) {
        snprintf(json + n, sizeof(json) - (size_t)n, "]}");
    }
    portal_http_send(tpcb, "application/json", json);
}

static void portal_handle_save(const char *body) {
    char ssid[WIFI_CFG_MAX_SSID + 1] = {0};
    char pass[WIFI_CFG_MAX_PASS + 1] = {0};
    char addr[WIFI_CFG_MAX_ADDR + 1] = {0};
    char port[8] = {0};

    if (portal_form_value(body, "ssid", ssid, sizeof(ssid))) {
        strncpy(portal.config.ssid, ssid, WIFI_CFG_MAX_SSID);
    }
    if (portal_form_value(body, "pass", pass, sizeof(pass))) {
        strncpy(portal.config.pass, pass, WIFI_CFG_MAX_PASS);
    }
    if (portal_form_value(body, "udp_addr", addr, sizeof(addr))) {
        strncpy(portal.config.udp_addr, addr, WIFI_CFG_MAX_ADDR);
    }
    if (portal_form_value(body, "udp_port", port, sizeof(port))) {
        int p = atoi(port);
        if (p > 0 && p < 65536) {
            portal.config.udp_port = (uint16_t)p;
        }
    }

    wifi_config_save(&portal.config);
    portal.config_saved = true;
}

static void portal_handle_ps_on(bool on) {
    set_ps_on(on);
}

static void portal_handle_http_request_parsed(struct tcp_pcb *tpcb, bool is_post,
                                              const char *path, const char *body) {
    cyw43_arch_lwip_begin();
    if (!path || path[0] == '\0') {
        portal_http_send(tpcb, "text/plain", "bad request");
        goto out;
    }
    if (!is_post) {
        if (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0) {
            portal_send_index(tpcb);
        } else if (strcmp(path, "/scan") == 0) {
            portal_send_scan(tpcb);
        } else if (strcmp(path, "/ps_on") == 0) {
            portal_handle_ps_on(true);
            portal_http_send(tpcb, "text/plain", "ps_on");
        } else if (strcmp(path, "/ps_off") == 0) {
            portal_handle_ps_on(false);
            portal_http_send(tpcb, "text/plain", "ps_off");
        } else {
            portal_http_send_redirect(tpcb, "/");
        }
        goto out;
    }

    if (strcmp(path, "/save") == 0) {
        if (body) {
            portal_handle_save(body);
            portal_http_send(tpcb, "text/plain", "saved");
        } else {
            portal_http_send(tpcb, "text/plain", "missing body");
        }
        goto out;
    }
    if (strcmp(path, "/ps_on") == 0) {
        portal_handle_ps_on(true);
        portal_http_send(tpcb, "text/plain", "ps_on");
        goto out;
    }
    if (strcmp(path, "/ps_off") == 0) {
        portal_handle_ps_on(false);
        portal_http_send(tpcb, "text/plain", "ps_off");
        goto out;
    }
    if (strcmp(path, "/scan") == 0) {
        portal_send_scan(tpcb);
        goto out;
    }

    portal_http_send_redirect(tpcb, "/");
out:
    cyw43_arch_lwip_end();
}

static err_t portal_http_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {
    portal_http_state_t *state = (portal_http_state_t *)arg;
    if (err != ERR_OK) {
        if (p) pbuf_free(p);
        portal_http_state_cleanup(tpcb, state);
        tcp_close(tpcb);
        return err;
    }
    if (!p) {
        portal_http_state_cleanup(tpcb, state);
        tcp_close(tpcb);
        return ERR_OK;
    }

    if (!state) {
        pbuf_free(p);
        portal_http_state_cleanup(tpcb, state);
        tcp_close(tpcb);
        return ERR_VAL;
    }

    size_t space = sizeof(state->buf) - state->len - 1;
    size_t total = p->tot_len;
    if (total > space) {
        total = space;
    }
    if (total > 0) {
        pbuf_copy_partial(p, state->buf + state->len, total, 0);
        state->len += total;
        state->buf[state->len] = '\0';
    }
    tcp_recved(tpcb, p->tot_len);
    pbuf_free(p);

    for (;;) {
        if (!state->request_line_done) {
            int idx = portal_find_crlf(state->buf + state->parse_pos,
                                       state->len - state->parse_pos);
            if (idx < 0) break;
            size_t line_len = (size_t)idx;
            if (!portal_parse_request_line(state, state->buf + state->parse_pos, line_len)) {
                portal_http_send(tpcb, "text/plain", "bad request");
                portal_http_state_cleanup(tpcb, state);
                tcp_close(tpcb);
                return ERR_OK;
            }
            state->parse_pos += line_len + 2;
            state->request_line_done = true;
            continue;
        }
        if (!state->headers_done) {
            int idx = portal_find_crlf(state->buf + state->parse_pos,
                                       state->len - state->parse_pos);
            if (idx < 0) break;
            size_t line_len = (size_t)idx;
            if (line_len == 0) {
                state->headers_done = true;
                state->body_offset = state->parse_pos + 2;
                size_t max_body = sizeof(state->buf) - state->body_offset - 1;
                if (state->content_length < 0 || (size_t)state->content_length > max_body) {
                    portal_http_send(tpcb, "text/plain", "request too large");
                    portal_http_state_cleanup(tpcb, state);
                    tcp_close(tpcb);
                    return ERR_OK;
                }
                state->parse_pos += 2;
                continue;
            }
            int parsed = portal_parse_content_length_line(state->buf + state->parse_pos,
                                                          line_len);
            if (parsed >= 0) {
                state->content_length = parsed;
            }
            state->parse_pos += line_len + 2;
            continue;
        }
        size_t available = state->len - state->body_offset;
        if (available < (size_t)state->content_length) {
            break;
        }
        const char *body = NULL;
        if (state->content_length > 0) {
            body = state->buf + state->body_offset;
            state->buf[state->body_offset + (size_t)state->content_length] = '\0';
        }
        portal_handle_http_request_parsed(tpcb, state->is_post, state->path, body);
        portal_http_state_cleanup(tpcb, state);
        tcp_close(tpcb);
        return ERR_OK;
    }

    if (space == 0) {
        portal_http_send(tpcb, "text/plain", "request too large");
        portal_http_state_cleanup(tpcb, state);
        tcp_close(tpcb);
    }
    return ERR_OK;
}

static err_t portal_http_accept(void *arg, struct tcp_pcb *newpcb, err_t err) {
    (void)arg;
    if (err != ERR_OK || !newpcb) {
        return ERR_VAL;
    }
    portal_http_state_t *state = calloc(1, sizeof(*state));
    if (!state) {
        tcp_abort(newpcb);
        return ERR_MEM;
    }
    tcp_arg(newpcb, state);
    tcp_recv(newpcb, portal_http_recv);
    tcp_err(newpcb, portal_http_err);
    return ERR_OK;
}

static void portal_dns_recv(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                            const ip_addr_t *addr, u16_t port) {
    (void)arg;
    if (!p) return;
    if (p->len < 12) {
        pbuf_free(p);
        return;
    }

    uint8_t buf[256];
    size_t len = p->tot_len;
    if (len > sizeof(buf)) len = sizeof(buf);
    pbuf_copy_partial(p, buf, len, 0);
    pbuf_free(p);

    uint16_t id = (uint16_t)((buf[0] << 8) | buf[1]);
    uint16_t flags = 0x8180;
    uint16_t qdcount = (uint16_t)((buf[4] << 8) | buf[5]);
    if (qdcount == 0) return;

    uint8_t resp[256];
    size_t resp_len = 0;
    resp[resp_len++] = (uint8_t)(id >> 8);
    resp[resp_len++] = (uint8_t)(id & 0xFF);
    resp[resp_len++] = (uint8_t)(flags >> 8);
    resp[resp_len++] = (uint8_t)(flags & 0xFF);
    resp[resp_len++] = 0x00; resp[resp_len++] = 0x01;
    resp[resp_len++] = 0x00; resp[resp_len++] = 0x01;
    resp[resp_len++] = 0x00; resp[resp_len++] = 0x00;
    resp[resp_len++] = 0x00; resp[resp_len++] = 0x00;

    size_t qlen = 12;
    while (qlen < len && buf[qlen] != 0) {
        qlen += buf[qlen] + 1;
    }
    if (qlen + 5 >= len) return;
    qlen += 5;
    if (resp_len + qlen > sizeof(resp)) return;
    memcpy(resp + resp_len, buf + 12, qlen - 12);
    resp_len += qlen - 12;

    resp[resp_len++] = 0xC0;
    resp[resp_len++] = 0x0C;
    resp[resp_len++] = 0x00; resp[resp_len++] = 0x01;
    resp[resp_len++] = 0x00; resp[resp_len++] = 0x01;
    resp[resp_len++] = 0x00; resp[resp_len++] = 0x00;
    resp[resp_len++] = 0x00; resp[resp_len++] = 0x3C;
    resp[resp_len++] = 0x00; resp[resp_len++] = 0x04;

    resp[resp_len++] = PORTAL_IP_OCT1;
    resp[resp_len++] = PORTAL_IP_OCT2;
    resp[resp_len++] = PORTAL_IP_OCT3;
    resp[resp_len++] = PORTAL_IP_OCT4;

    struct pbuf *out = pbuf_alloc(PBUF_TRANSPORT, (u16_t)resp_len, PBUF_RAM);
    if (!out) return;
    memcpy(out->payload, resp, resp_len);
    udp_sendto(pcb, out, addr, port);
    pbuf_free(out);
}

typedef struct dhcp_msg {
    uint8_t op;
    uint8_t htype;
    uint8_t hlen;
    uint8_t hops;
    uint32_t xid;
    uint16_t secs;
    uint16_t flags;
    uint8_t ciaddr[4];
    uint8_t yiaddr[4];
    uint8_t siaddr[4];
    uint8_t giaddr[4];
    uint8_t chaddr[16];
    uint8_t sname[64];
    uint8_t file[128];
    uint8_t options[312];
} dhcp_msg_t;

#define DHCP_OPT_MSG_TYPE 53
#define DHCP_OPT_SERVER_ID 54
#define DHCP_OPT_IP_LEASE_TIME 51
#define DHCP_OPT_SUBNET_MASK 1
#define DHCP_OPT_ROUTER 3
#define DHCP_OPT_DNS 6
#define DHCP_OPT_END 255

static uint8_t *dhcp_opt_find(uint8_t *opt, size_t len, uint8_t code) {
    for (size_t i = 0; i + 1 < len;) {
        if (opt[i] == DHCP_OPT_END) break;
        if (opt[i] == 0) {
            i++;
            continue;
        }
        if (opt[i] == code) return &opt[i];
        i += 2 + opt[i + 1];
    }
    return NULL;
}

static void dhcp_opt_write_n(uint8_t **opt, uint8_t code, size_t n, const void *data) {
    uint8_t *o = *opt;
    *o++ = code;
    *o++ = (uint8_t)n;
    memcpy(o, data, n);
    *opt = o + n;
}

static void dhcp_opt_write_u8(uint8_t **opt, uint8_t code, uint8_t val) {
    uint8_t *o = *opt;
    *o++ = code;
    *o++ = 1;
    *o++ = val;
    *opt = o;
}

static void dhcp_opt_write_u32(uint8_t **opt, uint8_t code, uint32_t val) {
    uint8_t *o = *opt;
    *o++ = code;
    *o++ = 4;
    *o++ = (uint8_t)(val >> 24);
    *o++ = (uint8_t)(val >> 16);
    *o++ = (uint8_t)(val >> 8);
    *o++ = (uint8_t)val;
    *opt = o;
}

static void dhcp_send_reply(struct udp_pcb *pcb, struct netif *nif, uint8_t msg_type, const dhcp_msg_t *req) {
    dhcp_msg_t resp;
    memset(&resp, 0, sizeof(resp));
    resp.op = 2;
    resp.htype = req->htype ? req->htype : 1;
    resp.hlen = req->hlen ? req->hlen : 6;
    resp.xid = req->xid;
    resp.flags = req->flags;
    memcpy(resp.chaddr, req->chaddr, sizeof(resp.chaddr));

    ip4_addr_t yiaddr;
    IP4_ADDR(&yiaddr, PORTAL_IP_OCT1, PORTAL_IP_OCT2, PORTAL_IP_OCT3, PORTAL_LEASE_OCT4);
    memcpy(resp.yiaddr, &yiaddr.addr, 4);
    ip4_addr_t siaddr;
    IP4_ADDR(&siaddr, PORTAL_IP_OCT1, PORTAL_IP_OCT2, PORTAL_IP_OCT3, PORTAL_IP_OCT4);
    memcpy(resp.siaddr, &siaddr.addr, 4);

    uint8_t *opt = resp.options;
    opt[0] = 99;
    opt[1] = 130;
    opt[2] = 83;
    opt[3] = 99;
    opt += 4;
    dhcp_opt_write_u8(&opt, DHCP_OPT_MSG_TYPE, msg_type);
    dhcp_opt_write_n(&opt, DHCP_OPT_SERVER_ID, 4, &siaddr.addr);
    ip4_addr_t netmask;
    IP4_ADDR(&netmask, 255, 255, 255, 0);
    dhcp_opt_write_n(&opt, DHCP_OPT_SUBNET_MASK, 4, &netmask.addr);
    dhcp_opt_write_n(&opt, DHCP_OPT_ROUTER, 4, &siaddr.addr);
    dhcp_opt_write_n(&opt, DHCP_OPT_DNS, 4, &siaddr.addr);
    dhcp_opt_write_u32(&opt, DHCP_OPT_IP_LEASE_TIME, 24 * 60 * 60);
    *opt++ = DHCP_OPT_END;

    size_t resp_len = (size_t)((uint8_t *)opt - (uint8_t *)&resp);
    struct pbuf *out = pbuf_alloc(PBUF_TRANSPORT, (u16_t)resp_len, PBUF_RAM);
    if (!out) return;
    memcpy(out->payload, &resp, resp_len);

    ip_addr_t dst;
    ip_addr_set_ip4_u32(&dst, PP_HTONL(0xFFFFFFFFu));
    if (nif) {
        udp_sendto_if(pcb, out, &dst, PORTAL_CLIENT_PORT, nif);
    } else {
        udp_sendto(pcb, out, &dst, PORTAL_CLIENT_PORT);
    }
    pbuf_free(out);
}

static void portal_dhcp_recv(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                             const ip_addr_t *addr, u16_t port) {
    (void)arg;
    (void)addr;
    (void)port;
    if (!p) return;

    dhcp_msg_t req;
    const size_t min_size = 240 + 3;
    size_t len = p->tot_len;
    if (len < min_size) {
        pbuf_free(p);
        return;
    }
    struct netif *nif = ip_current_input_netif();
    size_t copied = pbuf_copy_partial(p, &req, sizeof(req), 0);
    pbuf_free(p);
    if (copied < min_size) {
        return;
    }

    if (req.options[0] != 99 || req.options[1] != 130 ||
        req.options[2] != 83 || req.options[3] != 99) {
        return;
    }
    uint8_t *opt = &req.options[4];
    size_t opt_len = sizeof(req.options) - 4;
    uint8_t *msg = dhcp_opt_find(opt, opt_len, DHCP_OPT_MSG_TYPE);
    if (!msg || msg[1] != 1) {
        return;
    }
    uint8_t msg_type = msg[2];
    if (msg_type == 1) {
        dhcp_send_reply(pcb, nif, 2, &req);
    } else if (msg_type == 3) {
        dhcp_send_reply(pcb, nif, 5, &req);
    }
}

static void portal_start_servers(bool enable_ap_services) {
    if (enable_ap_services) {
        portal.dns_pcb = udp_new_ip_type(IPADDR_TYPE_V4);
        if (portal.dns_pcb) {
            udp_bind(portal.dns_pcb, IP_ADDR_ANY, PORTAL_DNS_PORT);
            udp_recv(portal.dns_pcb, portal_dns_recv, NULL);
        }
        portal.dhcp_pcb = udp_new_ip_type(IPADDR_TYPE_V4);
        if (portal.dhcp_pcb) {
            udp_bind(portal.dhcp_pcb, IP_ADDR_ANY, PORTAL_DHCP_PORT);
            udp_recv(portal.dhcp_pcb, portal_dhcp_recv, NULL);
        }
    }
    portal.http_pcb = tcp_new_ip_type(IPADDR_TYPE_V4);
    if (portal.http_pcb) {
        tcp_bind(portal.http_pcb, IP_ADDR_ANY, PORTAL_HTTP_PORT);
        portal.http_pcb = tcp_listen_with_backlog(portal.http_pcb, 2);
        tcp_accept(portal.http_pcb, portal_http_accept);
    }
}

static bool wifi_start_station(const wifi_config_t *cfg) {
    if (!cfg || cfg->ssid[0] == '\0') return false;
    cyw43_arch_enable_sta_mode();
    int auth = (cfg->pass[0] == '\0') ? CYW43_AUTH_OPEN : CYW43_AUTH_WPA2_AES_PSK;
    int err = cyw43_arch_wifi_connect_timeout_ms(cfg->ssid, cfg->pass, auth, 30000);
    if (err) {
        printf("[EBD_IPKVM] wifi connect failed: %d\n", err);
        return false;
    }
    printf("[EBD_IPKVM] wifi connected: %s\n", cfg->ssid);
    portal.active = true;
    portal.ap_mode = false;
    portal_start_servers(false);
    return true;
}

static bool wifi_start_portal(void) {
    int auth = (PORTAL_AP_PASS[0] == '\0') ? CYW43_AUTH_OPEN : CYW43_AUTH_WPA2_AES_PSK;
    cyw43_arch_disable_sta_mode();
    cyw43_arch_enable_ap_mode(PORTAL_AP_SSID, PORTAL_AP_PASS, auth);
    struct netif *netif = &cyw43_state.netif[CYW43_ITF_AP];
    ip4_addr_t ip, mask, gw;
    IP4_ADDR(&ip, PORTAL_IP_OCT1, PORTAL_IP_OCT2, PORTAL_IP_OCT3, PORTAL_IP_OCT4);
    IP4_ADDR(&mask, 255, 255, 255, 0);
    IP4_ADDR(&gw, PORTAL_IP_OCT1, PORTAL_IP_OCT2, PORTAL_IP_OCT3, PORTAL_IP_OCT4);
    netif_set_addr(netif, &ip, &mask, &gw);
    netif_set_default(netif);
    netif_set_up(netif);
    netif_set_link_up(netif);
    portal_start_servers(true);
    portal.active = true;
    portal.ap_mode = true;
    printf("[EBD_IPKVM] portal active: %s\n", PORTAL_AP_SSID);
    return true;
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

static inline void request_probe_packet(void) {
    probe_offset = 0;
    probe_pending = 1;
}

static void emit_debug_state(void) {
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
    want_frame = !tx_busy;

    if (want_frame) {
        video_capture_start(&capture, true);
    }
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
    if (!test_frame_active || capture.capture_enabled) return;

    while (test_frame_active) {
        uint8_t fill = (test_line & 1) ? 0xFF : 0x00;
        memset(test_line_buf, fill, BYTES_PER_LINE);
        if (!stream_send_line(frame_id, test_line, test_line_buf)) {
            lines_drop++;
            break;
        }

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

        if (!stream_send_line(frame_tx_id, frame_tx_line, (uint8_t *)frame_tx_line_buf)) {
            lines_drop++;
            break;
        }

        frame_tx_line++;
        lines_sent++;
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
            armed = true;
            /* IMPORTANT: do NOT printf here; keep binary stream clean once armed */
        } else if (ch == 'X' || ch == 'x') {
            armed = false;
            want_frame = false;
            video_capture_stop(&capture);
            txq_reset();
            reset_frame_tx_state();
            test_frame_active = false;
            test_line = 0;
            if (can_emit_text()) {
                printf("[EBD_IPKVM] armed=0 (host stop)\n");
            }
        } else if (ch == 'R' || ch == 'r') {
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
            if (can_emit_text()) {
                printf("[EBD_IPKVM] reset counters\n");
            }
        } else if (ch == 'Q' || ch == 'q') {
            armed = false;
            want_frame = false;
            video_capture_stop(&capture);
            txq_reset();
            reset_frame_tx_state();
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
            video_capture_stop(&capture);
            txq_reset();
            reset_frame_tx_state();
            sleep_ms(10);
            reset_usb_boot(0, 0);
        } else if (ch == 'Z' || ch == 'z') {
            armed = false;
            want_frame = false;
            video_capture_stop(&capture);
            txq_reset();
            reset_frame_tx_state();
            sleep_ms(10);
            watchdog_reboot(0, 0, 0);
            while (true) { tight_loop_contents(); }
        } else if (ch == 'F' || ch == 'f') {
            if (!capture.capture_enabled) {
                want_frame = true;
                video_capture_start(&capture, true);
            }
        } else if (ch == 'T' || ch == 't') {
            armed = false;
            want_frame = false;
            video_capture_stop(&capture);
            txq_reset();
            reset_frame_tx_state();
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
        } else if (ch == 'V' || ch == 'v') {
            vsync_fall_edge = !vsync_fall_edge;
            armed = false;
            want_frame = false;
            video_capture_stop(&capture);
            txq_reset();
            reset_frame_tx_state();
            configure_vsync_irq();
            if (can_emit_text()) {
                printf("[EBD_IPKVM] vsync_edge=%s\n", vsync_fall_edge ? "fall" : "rise");
            }
        } else if (ch == 'W' || ch == 'w') {
            wifi_config_clear();
            printf("[EBD_IPKVM] wifi config cleared, rebooting into portal\n");
            sleep_ms(50);
            watchdog_reboot(0, 0, 0);
        }
    }
}

static inline void service_txq(void) {
#if VIDEO_STREAM_USB
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
            stream_drops++;
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
#else
    (void)stream_drops;
#endif
}

int main(void) {
    stdio_init_all();
    sleep_ms(1200);

    printf("\n[EBD_IPKVM] UDP RLE video stream @ ~60fps, continuous capture\n");
    printf("[EBD_IPKVM] WAITING for host. Send 'S' to start, 'X' stop, 'R' reset.\n");
    printf("[EBD_IPKVM] Power/control: 'P' on, 'p' off, 'B' BOOTSEL, 'Z' reset.\n");
    printf("[EBD_IPKVM] GPIO diag: send 'G' for pin states + edge counts.\n");
    printf("[EBD_IPKVM] Edge toggles: 'V' VSYNC edge.\n");

    // SIO GPIO inputs + pulls (sane when Mac is off)
    gpio_init(PIN_PIXCLK); gpio_set_dir(PIN_PIXCLK, GPIO_IN); gpio_disable_pulls(PIN_PIXCLK);
    gpio_init(PIN_VIDEO);  gpio_set_dir(PIN_VIDEO,  GPIO_IN); gpio_disable_pulls(PIN_VIDEO);
    gpio_init(PIN_HSYNC);  gpio_set_dir(PIN_HSYNC,  GPIO_IN); gpio_disable_pulls(PIN_HSYNC);

    // VSYNC must remain SIO GPIO for IRQ to work
    gpio_init(PIN_VSYNC);  gpio_set_dir(PIN_VSYNC,  GPIO_IN); gpio_disable_pulls(PIN_VSYNC);
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
    configure_pio_program();

    dma_chan = dma_claim_unused_channel(true);
    irq_set_priority(USBCTRL_IRQ, 1);

    video_capture_init(&capture, pio, sm, dma_chan, framebuf_a, framebuf_b);
    video_capture_stop(&capture);
    txq_reset();
    reset_frame_tx_state();

#if VIDEO_STREAM_UDP
    if (cyw43_arch_init()) {
        printf("[EBD_IPKVM] wifi init failed\n");
    } else {
        portal.has_config = wifi_config_load(&portal.config);
        if (!portal.has_config) {
            portal_defaults();
        }

        if (portal.has_config && wifi_start_station(&portal.config)) {
            udp_stream_init(&portal.config);
        } else {
            wifi_start_portal();
        }
    }
#endif

    absolute_time_t next = make_timeout_time_ms(1000);
    uint32_t last_lines = 0;

    while (true) {
        if (capture.capture_enabled && !dma_channel_is_busy(capture.dma_chan)) {
            if (video_capture_finalize_frame(&capture, frame_id)) {
                frame_id++;
                frames_done++;
            } else {
                frame_id++;
            }
        }

        tud_task();
        poll_cdc_commands();

#if VIDEO_STREAM_UDP
        if (udp_stream.ready || portal.active) {
            cyw43_arch_poll();
        }
        if (portal.active && portal.config_saved) {
            printf("[EBD_IPKVM] portal config saved, rebooting\n");
            sleep_ms(200);
            watchdog_reboot(0, 0, 0);
        }
#endif

        /* Send queued binary packets from thread context (NOT IRQ). */
        if (probe_pending && try_send_probe_packet()) {
            probe_pending = 0;
        }
        if (debug_requested && can_emit_text()) {
            debug_requested = false;
            emit_debug_state();
        }
        service_test_frame();
        service_frame_tx();
        service_txq();

        /* Keep status text off the wire while armed/capturing or while TX path active. */
        bool can_report = stream_is_idle() && !armed;
        if (can_report) {
            if (absolute_time_diff_us(get_absolute_time(), next) <= 0) {
                next = delayed_by_ms(next, 1000);

                uint32_t l = capture.lines_ok;
                uint32_t per_s = l - last_lines;
                last_lines = l;

                uint32_t ve = vsync_edges;
                vsync_edges = 0;

                printf("[EBD_IPKVM] armed=%d cap=%d ps_on=%d lines/s=%lu total=%lu q_drops=%lu stream_drops=%lu frame_overrun=%lu vsync_edges/s=%lu frames=%lu\n",
                       armed ? 1 : 0,
                       capture.capture_enabled ? 1 : 0,
                       ps_on_state ? 1 : 0,
                       (unsigned long)per_s,
                       (unsigned long)l,
                       (unsigned long)lines_drop,
                       (unsigned long)stream_drops,
                       (unsigned long)capture.frame_overrun,
                       (unsigned long)ve,
                       (unsigned long)frames_done);
            }
        }

        tight_loop_contents();
    }
}
