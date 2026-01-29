#include "portal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "pico/cyw43_arch.h"

#include "cyw43.h"
#include "lwip/dhcp.h"
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"
#include "lwip/udp.h"

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
    char mac_str[18];
    char hostname[32];
    wifi_config_t config;
    portal_scan_result_t scan_results[PORTAL_MAX_SCAN];
    uint8_t scan_count;
    char http_page_buf[2048];
    char http_json_buf[1024];
    struct udp_pcb *dns_pcb;
    struct udp_pcb *dhcp_pcb;
    struct tcp_pcb *http_pcb;
    portal_ps_on_cb ps_on_cb;
    void *ps_on_ctx;
} portal_state_t;

static portal_state_t portal = {0};

__attribute__((weak)) int cyw43_arch_wifi_scan(cyw43_wifi_scan_options_t *opts,
                                               int (*result_cb)(void *, const cyw43_ev_scan_result_t *),
                                               void *env) {
    return cyw43_wifi_scan(&cyw43_state, opts, env, result_cb);
}

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

static void portal_set_identity(uint8_t itf) {
    uint8_t mac[6] = {0};
    if (cyw43_wifi_get_mac(&cyw43_state, itf, mac) == 0) {
        snprintf(portal.mac_str, sizeof(portal.mac_str),
                 "%02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        snprintf(portal.hostname, sizeof(portal.hostname),
                 "EBDIPKVM-%02X%02X%02X", mac[3], mac[4], mac[5]);
    } else {
        strncpy(portal.mac_str, "unknown", sizeof(portal.mac_str));
        portal.mac_str[sizeof(portal.mac_str) - 1] = '\0';
        portal.hostname[0] = '\0';
    }
}

static size_t portal_url_decode(char *dst, size_t dst_len, const char *src, size_t src_len) {
    size_t out = 0;
    for (size_t i = 0; i < src_len && out + 1 < dst_len; i++) {
        if (src[i] == '+') {
            dst[out++] = ' ';
        } else if (src[i] == '%' && (i + 2) < src_len) {
            char hex[3] = {src[i + 1], src[i + 2], '\0'};
            dst[out++] = (char)strtol(hex, NULL, 16);
            i += 2;
        } else {
            dst[out++] = src[i];
        }
    }
    dst[out] = '\0';
    return out;
}

static bool portal_form_value(const char *body, const char *key, char *out, size_t out_len) {
    if (!body || !key) return false;
    size_t key_len = strlen(key);
    const char *p = body;
    while (p && *p) {
        const char *eq = strchr(p, '=');
        if (!eq) return false;
        const char *amp = strchr(eq + 1, '&');
        size_t name_len = (size_t)(eq - p);
        if (name_len == key_len && strncmp(p, key, key_len) == 0) {
            size_t val_len = amp ? (size_t)(amp - (eq + 1)) : strlen(eq + 1);
            portal_url_decode(out, out_len, eq + 1, val_len);
            return true;
        }
        p = amp ? (amp + 1) : NULL;
    }
    return false;
}

static void portal_http_send(struct tcp_pcb *tpcb, const char *content_type, const char *body) {
    if (!tpcb || !body) return;
    char header[256];
    int len = snprintf(header, sizeof(header),
                       "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %u\r\nConnection: close\r\n\r\n",
                       content_type,
                       (unsigned)strlen(body));
    if (len <= 0 || (size_t)len >= sizeof(header)) return;
    tcp_write(tpcb, header, (uint16_t)len, TCP_WRITE_FLAG_COPY);
    tcp_write(tpcb, body, (uint16_t)strlen(body), TCP_WRITE_FLAG_COPY);
    tcp_output(tpcb);
}

static void portal_http_send_redirect(struct tcp_pcb *tpcb, const char *location) {
    if (!tpcb || !location) return;
    char header[256];
    int len = snprintf(header, sizeof(header),
                       "HTTP/1.1 302 Found\r\nLocation: %s\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
                       location);
    if (len <= 0 || (size_t)len >= sizeof(header)) return;
    tcp_write(tpcb, header, (uint16_t)len, TCP_WRITE_FLAG_COPY);
    tcp_output(tpcb);
}

typedef struct portal_http_state {
    bool is_post;
    char path[128];
    char *buf;
    size_t buf_len;
    size_t parse_pos;
    int content_length;
    bool request_line_parsed;
    bool headers_done;
} portal_http_state_t;

static void portal_http_state_cleanup(struct tcp_pcb *tpcb, portal_http_state_t *state) {
    if (state) {
        if (state->buf) {
            free(state->buf);
        }
        free(state);
    }
    if (tpcb) {
        tcp_arg(tpcb, NULL);
        tcp_recv(tpcb, NULL);
        tcp_err(tpcb, NULL);
        tcp_close(tpcb);
    }
}

static void portal_http_err(void *arg, err_t err) {
    (void)err;
    portal_http_state_t *state = (portal_http_state_t *)arg;
    if (state) {
        if (state->buf) {
            free(state->buf);
        }
        free(state);
    }
}

static bool portal_find_line_end(const char *buf, size_t len, size_t *line_len, size_t *eol_len) {
    for (size_t i = 0; i < len; i++) {
        if (buf[i] == '\n') {
            *line_len = (i > 0 && buf[i - 1] == '\r') ? (i - 1) : i;
            *eol_len = (i > 0 && buf[i - 1] == '\r') ? 2 : 1;
            return true;
        }
    }
    return false;
}

static int portal_parse_content_length_line(const char *line, size_t len) {
    const char *prefix = "Content-Length:";
    size_t prefix_len = strlen(prefix);
    if (len < prefix_len) return -1;
    if (strncasecmp(line, prefix, prefix_len) != 0) return -1;
    const char *num = line + prefix_len;
    while ((size_t)(num - line) < len && (*num == ' ' || *num == '\t')) {
        num++;
    }
    return atoi(num);
}

static bool portal_parse_request_line(portal_http_state_t *state, const char *line, size_t len) {
    const char *space = memchr(line, ' ', len);
    if (!space) return false;
    size_t method_len = (size_t)(space - line);
    state->is_post = (method_len == 4 && strncmp(line, "POST", 4) == 0);

    const char *path = space + 1;
    size_t remain = len - (size_t)(path - line);
    const char *path_end = memchr(path, ' ', remain);
    if (!path_end) return false;
    size_t path_len = (size_t)(path_end - path);
    if (path_len >= sizeof(state->path)) return false;

    if (strncmp(path, "http://", 7) == 0 || strncmp(path, "https://", 8) == 0) {
        const char *first = strstr(path, "://");
        if (!first) return false;
        const char *slash = memchr(first + 3, '/', len - (size_t)(first + 3 - line));
        if (!slash) return false;
        path = slash;
        path_len = (size_t)(path_end - path);
        if (path_len >= sizeof(state->path)) return false;
    }

    const char *query = memchr(path, '?', path_len);
    if (query) {
        path_len = (size_t)(query - path);
    }

    memcpy(state->path, path, path_len);
    state->path[path_len] = '\0';
    return true;
}

static void portal_send_index(struct tcp_pcb *tpcb) {
    int n = snprintf(portal.http_page_buf, sizeof(portal.http_page_buf),
                     "<html><head><title>EBD IP-KVM</title></head><body>"
                     "<h1>EBD IP-KVM</h1>"
                     "<p>Status: %s</p>"
                     "<p>MAC: %s</p>"
                     "<p>SSID: %s</p>"
                     "<p>UDP Port: %u</p>"
                     "<form method=\"POST\" action=\"/ps_on\" id=\"ps_on_form\">"
                     "<button type=\"submit\" id=\"ps_on_btn\">Power On</button>"
                     "</form>"
                     "<form method=\"POST\" action=\"/ps_off\" id=\"ps_off_form\">"
                     "<button type=\"submit\" id=\"ps_off_btn\">Power Off</button>"
                     "</form>"
                     "<p>Configure Wi-Fi and set the UDP listen port for video streaming.</p>"
                     "<form method=\"POST\" action=\"/save\">"
                     "<label>SSID</label><input name=\"ssid\" value=\"%s\" />"
                     "<label>Password</label><input name=\"pass\" value=\"%s\" />"
                     "<label>UDP Listen Port</label><input name=\"udp_port\" value=\"%u\" />"
                     "<button type=\"submit\">Save</button>"
                     "</form>"
                     "<form method=\"GET\" action=\"/scan\">"
                     "<button type=\"submit\">Scan Wi-Fi</button>"
                     "</form>"
                     "</body></html>",
                     portal.ap_mode ? "AP (setup)" : "Station",
                     portal.mac_str,
                     portal.config.ssid,
                     (unsigned)portal.config.udp_port,
                     portal.config.ssid,
                     portal.config.pass,
                     (unsigned)portal.config.udp_port);
    if (n <= 0 || (size_t)n >= sizeof(portal.http_page_buf)) {
        portal_http_send(tpcb, "text/plain", "render error");
        return;
    }
    portal_http_send(tpcb, "text/html", portal.http_page_buf);
}

static int portal_scan_callback(void *env, const cyw43_ev_scan_result_t *result) {
    (void)env;
    if (!result) {
        portal.scan_in_progress = false;
        return 0;
    }
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
    if (err != 0) {
        portal.scan_in_progress = false;
    }
}

static void portal_send_scan(struct tcp_pcb *tpcb) {
    if (!portal.scan_in_progress) {
        portal_start_scan();
    }
    int n = snprintf(portal.http_json_buf, sizeof(portal.http_json_buf),
                     "{\"scanning\":%s,\"results\":[",
                     portal.scan_in_progress ? "true" : "false");
    for (uint8_t i = 0; i < portal.scan_count && n > 0 && (size_t)n < sizeof(portal.http_json_buf); i++) {
        portal_scan_result_t *r = &portal.scan_results[i];
        n += snprintf(portal.http_json_buf + n, sizeof(portal.http_json_buf) - (size_t)n,
                      "%s{\"ssid\":\"%s\",\"rssi\":%ld,\"auth\":%u}",
                      (i == 0) ? "" : ",",
                      r->ssid,
                      (long)r->rssi,
                      r->auth);
    }
    if (n > 0 && (size_t)n < sizeof(portal.http_json_buf)) {
        snprintf(portal.http_json_buf + n, sizeof(portal.http_json_buf) - (size_t)n, "]}");
        portal_http_send(tpcb, "application/json", portal.http_json_buf);
        return;
    }
    portal_http_send(tpcb, "application/json", "{\"scanning\":false,\"results\":[]}");
}

static void portal_send_scan_page(struct tcp_pcb *tpcb) {
    if (!portal.scan_in_progress) {
        portal_start_scan();
    }
    int n = snprintf(portal.http_page_buf, sizeof(portal.http_page_buf),
                     "<html><head><title>Scan</title>%s</head><body>"
                     "<h1>Wi-Fi Scan</h1>"
                     "<p>Status: %s</p>"
                     "<ul>",
                     portal.scan_in_progress ? "<meta http-equiv=\"refresh\" content=\"2\">" : "",
                     portal.scan_in_progress ? "scanning" : "done");
    for (uint8_t i = 0; i < portal.scan_count && n > 0 && (size_t)n < sizeof(portal.http_page_buf); i++) {
        portal_scan_result_t *r = &portal.scan_results[i];
        n += snprintf(portal.http_page_buf + n, sizeof(portal.http_page_buf) - (size_t)n,
                      "<li>SSID: %s (RSSI %ld)"
                      "<form method=\"POST\" action=\"/select\">"
                      "<input type=\"hidden\" name=\"ssid\" value=\"%s\" />"
                      "<button type=\"submit\">Use</button>"
                      "</form>"
                      "</li>",
                      r->ssid,
                      (long)r->rssi,
                      r->ssid);
    }
    if (n > 0 && (size_t)n < sizeof(portal.http_page_buf)) {
        snprintf(portal.http_page_buf + n, sizeof(portal.http_page_buf) - (size_t)n,
                 "</ul><a href=\"/\">Back</a></body></html>");
        portal_http_send(tpcb, "text/html", portal.http_page_buf);
        return;
    }
    portal_http_send(tpcb, "text/plain", "render error");
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
        if (p > 0 && p < UINT16_MAX) {
            portal.config.udp_port = (uint16_t)p;
        }
    }

    wifi_config_save(&portal.config);
    portal.config_saved = true;
}

static void portal_handle_select_ssid(const char *body) {
    char ssid[WIFI_CFG_MAX_SSID + 1] = {0};
    if (portal_form_value(body, "ssid", ssid, sizeof(ssid))) {
        strncpy(portal.config.ssid, ssid, WIFI_CFG_MAX_SSID);
    }
}

static void portal_handle_ps_on(bool on) {
    if (portal.ps_on_cb) {
        portal.ps_on_cb(on, portal.ps_on_ctx);
    }
}

static void portal_handle_http_request_parsed(struct tcp_pcb *tpcb, bool is_post,
                                              const char *path, const char *body) {
    if (!path) {
        portal_http_send(tpcb, "text/plain", "bad request");
        return;
    }
    if (!is_post) {
        if (strcmp(path, "/") == 0) {
            portal_send_index(tpcb);
            return;
        } else if (strcmp(path, "/scan") == 0) {
            portal_send_scan_page(tpcb);
            return;
        } else if (strcmp(path, "/ps_on") == 0) {
            portal_handle_ps_on(true);
            portal_http_send_redirect(tpcb, "/");
            return;
        } else if (strcmp(path, "/ps_off") == 0) {
            portal_handle_ps_on(false);
            portal_http_send_redirect(tpcb, "/");
            return;
        }
        portal_http_send_redirect(tpcb, "/");
        return;
    }

    if (strcmp(path, "/scan") == 0) {
        portal_send_scan(tpcb);
        return;
    }
    if (!body) {
        portal_http_send(tpcb, "text/plain", "missing body");
        return;
    }
    if (strcmp(path, "/save") == 0) {
        portal_handle_save(body);
        portal_http_send_redirect(tpcb, "/");
        return;
    }
    if (strcmp(path, "/select") == 0) {
        portal_handle_select_ssid(body);
        portal_http_send_redirect(tpcb, "/");
        return;
    }

    if (strcmp(path, "/ps_on") == 0) {
        portal_handle_ps_on(true);
        portal_http_send_redirect(tpcb, "/");
        return;
    }
    if (strcmp(path, "/ps_off") == 0) {
        portal_handle_ps_on(false);
        portal_http_send_redirect(tpcb, "/");
        return;
    }

    portal_http_send_redirect(tpcb, "/");
}

static err_t portal_http_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {
    portal_http_state_t *state = (portal_http_state_t *)arg;
    if (!p || err != ERR_OK) {
        if (p) {
            pbuf_free(p);
        }
        portal_http_state_cleanup(tpcb, state);
        return ERR_OK;
    }

    if (!state) {
        pbuf_free(p);
        portal_http_state_cleanup(tpcb, state);
        return ERR_OK;
    }

    if (!state->buf) {
        state->buf = calloc(1, PORTAL_MAX_REQ);
        state->buf_len = 0;
        state->parse_pos = 0;
        state->content_length = -1;
        state->request_line_parsed = false;
        state->headers_done = false;
    }

    if (state->buf_len + p->tot_len >= PORTAL_MAX_REQ && state->parse_pos > 0) {
        size_t remaining = state->buf_len - state->parse_pos;
        memmove(state->buf, state->buf + state->parse_pos, remaining);
        state->buf_len = remaining;
        state->parse_pos = 0;
    }

    if (state->buf_len + p->tot_len >= PORTAL_MAX_REQ) {
        portal_http_send(tpcb, "text/plain", "request too large");
        pbuf_free(p);
        portal_http_state_cleanup(tpcb, state);
        return ERR_OK;
    }

    pbuf_copy_partial(p, state->buf + state->buf_len, p->tot_len, 0);
    state->buf_len += p->tot_len;
    tcp_recved(tpcb, p->tot_len);
    pbuf_free(p);

    while (state->parse_pos < state->buf_len) {
        if (!state->request_line_parsed) {
            size_t line_len = 0;
            size_t eol_len = 0;
            if (!portal_find_line_end(state->buf + state->parse_pos,
                                      state->buf_len - state->parse_pos,
                                      &line_len,
                                      &eol_len)) {
                break;
            }
            if (!portal_parse_request_line(state, state->buf + state->parse_pos, line_len)) {
                portal_http_send(tpcb, "text/plain", "bad request");
                portal_http_state_cleanup(tpcb, state);
                return ERR_OK;
            }
            state->request_line_parsed = true;
            state->parse_pos += line_len + eol_len;
            continue;
        }

        if (!state->headers_done) {
            size_t line_len = 0;
            size_t eol_len = 0;
            if (!portal_find_line_end(state->buf + state->parse_pos,
                                      state->buf_len - state->parse_pos,
                                      &line_len,
                                      &eol_len)) {
                break;
            }
            if (line_len == 0) {
                state->headers_done = true;
                state->parse_pos += eol_len;
                continue;
            }
            int content_len = portal_parse_content_length_line(state->buf + state->parse_pos, line_len);
            if (content_len >= 0) {
                state->content_length = content_len;
            }
            state->parse_pos += line_len + eol_len;
            continue;
        }

        if (state->headers_done) {
            const char *body = NULL;
            if (state->content_length >= 0) {
                if (state->buf_len - state->parse_pos < (size_t)state->content_length) {
                    break;
                }
                state->buf[state->parse_pos + (size_t)state->content_length] = '\0';
                body = state->buf + state->parse_pos;
            } else if (state->parse_pos < state->buf_len) {
                state->buf[state->buf_len] = '\0';
                body = state->buf + state->parse_pos;
            }
            portal_handle_http_request_parsed(tpcb, state->is_post, state->path, body);
            portal_http_state_cleanup(tpcb, state);
            return ERR_OK;
        }
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
    if (!pcb || !addr) {
        pbuf_free(p);
        return;
    }

    if (p->tot_len < 12) {
        pbuf_free(p);
        return;
    }

    size_t req_len = (size_t)p->tot_len;
    size_t resp_extra = 16;
    struct pbuf *out = pbuf_alloc(PBUF_TRANSPORT, (u16_t)(req_len + resp_extra), PBUF_RAM);
    if (!out) {
        pbuf_free(p);
        return;
    }

    pbuf_copy_partial(p, out->payload, p->tot_len, 0);
    uint8_t *resp = (uint8_t *)out->payload;
    resp[2] = 0x81; // response + recursion available
    resp[3] = 0x80; // no error
    resp[7] = 0x01; // answer count = 1

    size_t resp_len = req_len;
    resp[resp_len++] = 0xC0;
    resp[resp_len++] = 0x0C; // name pointer
    resp[resp_len++] = 0x00;
    resp[resp_len++] = 0x01; // type A
    resp[resp_len++] = 0x00;
    resp[resp_len++] = 0x01; // class IN
    resp[resp_len++] = 0x00;
    resp[resp_len++] = 0x00;
    resp[resp_len++] = 0x00;
    resp[resp_len++] = 0x3C; // TTL 60s
    resp[resp_len++] = 0x00;
    resp[resp_len++] = 0x04; // rdlen
    // Always answer with the portal IP so captive clients can reach the web UI
    // without requiring secondary-IP routing support.
    uint32_t ip = (PORTAL_IP_OCT1 << 24) | (PORTAL_IP_OCT2 << 16) | (PORTAL_IP_OCT3 << 8) | PORTAL_IP_OCT4;
    resp[resp_len++] = (uint8_t)((ip >> 24) & 0xFF);
    resp[resp_len++] = (uint8_t)((ip >> 16) & 0xFF);
    resp[resp_len++] = (uint8_t)((ip >> 8) & 0xFF);
    resp[resp_len++] = (uint8_t)(ip & 0xFF);

    out->len = (u16_t)resp_len;
    out->tot_len = (u16_t)resp_len;
    udp_sendto(pcb, out, addr, port);
    pbuf_free(out);
    pbuf_free(p);
}

typedef struct dhcp_msg {
    uint8_t op;
    uint8_t htype;
    uint8_t hlen;
    uint8_t hops;
    uint32_t xid;
    uint16_t secs;
    uint16_t flags;
    uint32_t ciaddr;
    uint32_t yiaddr;
    uint32_t siaddr;
    uint32_t giaddr;
    uint8_t chaddr[16];
    uint8_t sname[64];
    uint8_t file[128];
    uint8_t options[312];
} dhcp_msg_t;

static void dhcp_write_option(uint8_t **opt, uint8_t code, const void *data, uint8_t len) {
    **opt = code;
    (*opt)++;
    **opt = len;
    (*opt)++;
    memcpy(*opt, data, len);
    (*opt) += len;
}

static void dhcp_write_u32(uint8_t **opt, uint8_t code, uint32_t value) {
    uint8_t data[4];
    data[0] = (uint8_t)((value >> 24) & 0xFF);
    data[1] = (uint8_t)((value >> 16) & 0xFF);
    data[2] = (uint8_t)((value >> 8) & 0xFF);
    data[3] = (uint8_t)(value & 0xFF);
    dhcp_write_option(opt, code, data, sizeof(data));
}

static void dhcp_send_reply(struct udp_pcb *pcb, struct netif *nif, uint8_t msg_type, const dhcp_msg_t *req) {
    if (!pcb || !req) return;

    struct pbuf *out = pbuf_alloc(PBUF_TRANSPORT, sizeof(dhcp_msg_t), PBUF_RAM);
    if (!out) return;

    memset(out->payload, 0, sizeof(dhcp_msg_t));
    dhcp_msg_t *msg = (dhcp_msg_t *)out->payload;
    msg->op = 2; // reply
    msg->htype = req->htype;
    msg->hlen = req->hlen;
    msg->xid = req->xid;
    msg->flags = req->flags;
    msg->yiaddr = PP_HTONL((PORTAL_IP_OCT1 << 24) | (PORTAL_IP_OCT2 << 16) | (PORTAL_IP_OCT3 << 8) | PORTAL_LEASE_OCT4);
    memcpy(msg->chaddr, req->chaddr, sizeof(msg->chaddr));

    uint8_t *opt = msg->options;
    *opt++ = 0x63; *opt++ = 0x82; *opt++ = 0x53; *opt++ = 0x63; // magic cookie
    dhcp_write_option(&opt, 53, &msg_type, 1); // message type
    dhcp_write_u32(&opt, 54, PP_HTONL((PORTAL_IP_OCT1 << 24) | (PORTAL_IP_OCT2 << 16) | (PORTAL_IP_OCT3 << 8) | PORTAL_IP_OCT4));
    dhcp_write_u32(&opt, 1, PP_HTONL((255 << 24) | (255 << 16) | (255 << 8) | 0));
    dhcp_write_u32(&opt, 3, PP_HTONL((PORTAL_IP_OCT1 << 24) | (PORTAL_IP_OCT2 << 16) | (PORTAL_IP_OCT3 << 8) | PORTAL_IP_OCT4));
    dhcp_write_u32(&opt, 6, PP_HTONL((PORTAL_IP_OCT1 << 24) | (PORTAL_IP_OCT2 << 16) | (PORTAL_IP_OCT3 << 8) | PORTAL_IP_OCT4));
    *opt++ = 255; // end

    ip_addr_t dst;
    ip4_addr_set_u32(&dst, PP_HTONL(0xFFFFFFFF));
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
    if (p->tot_len < sizeof(dhcp_msg_t)) {
        pbuf_free(p);
        return;
    }

    dhcp_msg_t req_buf;
    pbuf_copy_partial(p, &req_buf, sizeof(req_buf), 0);
    struct netif *nif = portal.ap_mode ? &cyw43_state.netif[CYW43_ITF_AP] : NULL;
    dhcp_msg_t *req = &req_buf;
    uint8_t msg_type = 0;
    uint8_t *opt = req->options;
    if (opt[0] == 0x63 && opt[1] == 0x82 && opt[2] == 0x53 && opt[3] == 0x63) {
        opt += 4;
        while ((size_t)(opt - req->options) < sizeof(req->options)) {
            size_t remaining = sizeof(req->options) - (size_t)(opt - req->options);
            if (remaining < 1) break;
            uint8_t code = *opt++;
            if (code == 255) break;
            if (code == 0) continue;
            remaining = sizeof(req->options) - (size_t)(opt - req->options);
            if (remaining < 1) break;
            uint8_t len = *opt++;
            if (len > remaining - 1) break;
            if (code == 53 && len == 1) {
                msg_type = *opt;
                break;
            }
            opt += len;
        }
    }

    if (msg_type == 1) { // DISCOVER
        dhcp_send_reply(pcb, nif, 2, req); // OFFER
    } else if (msg_type == 3) { // REQUEST
        dhcp_send_reply(pcb, nif, 5, req); // ACK
    }

    pbuf_free(p);
}

static void portal_start_servers(bool enable_ap_services) {
    cyw43_arch_lwip_begin();
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
    cyw43_arch_lwip_end();
}

static bool wifi_start_station_internal(const wifi_config_t *cfg) {
    if (!cfg) return false;

    portal_set_identity(CYW43_ITF_STA);
    cyw43_arch_lwip_begin();
    if (portal.hostname[0] != '\0') {
        netif_set_hostname(&cyw43_state.netif[CYW43_ITF_STA], portal.hostname);
    }
    cyw43_arch_lwip_end();

    int err = cyw43_arch_wifi_connect_timeout_ms(cfg->ssid, cfg->pass, CYW43_AUTH_WPA2_AES_PSK, 20000);
    if (err != 0) {
        printf("[EBD_IPKVM] wifi connect failed: %d\n", err);
        return false;
    }

    portal.active = true;
    portal.ap_mode = false;
    portal_start_servers(false);
    return true;
}

static bool wifi_start_portal_internal(void) {
    portal_set_identity(CYW43_ITF_AP);

    cyw43_arch_disable_sta_mode();
    cyw43_arch_enable_ap_mode(PORTAL_AP_SSID, PORTAL_AP_PASS, CYW43_AUTH_OPEN);

    ip4_addr_t gw;
    ip4addr_aton(PORTAL_AP_ADDR, &gw);
    ip4_addr_t mask;
    ip4addr_aton(PORTAL_AP_NETMASK, &mask);
    cyw43_arch_lwip_begin();
    netif_set_addr(&cyw43_state.netif[CYW43_ITF_AP], &gw, &mask, &gw);
    netif_set_default(&cyw43_state.netif[CYW43_ITF_AP]);
    netif_set_up(&cyw43_state.netif[CYW43_ITF_AP]);
    cyw43_arch_lwip_end();

    portal_start_servers(true);
    portal.active = true;
    portal.ap_mode = true;
    printf("[EBD_IPKVM] portal active: %s\n", PORTAL_AP_SSID);
    return true;
}

void portal_init(portal_ps_on_cb ps_on_cb, void *ctx) {
    memset(&portal, 0, sizeof(portal));
    portal.ps_on_cb = ps_on_cb;
    portal.ps_on_ctx = ctx;
    portal.has_config = wifi_config_load(&portal.config);
    if (!portal.has_config) {
        portal_defaults();
    }
}

bool portal_has_config(void) {
    return portal.has_config;
}

const wifi_config_t *portal_get_config(void) {
    return &portal.config;
}

bool portal_start_station(void) {
    return wifi_start_station_internal(&portal.config);
}

bool portal_start_portal(void) {
    return wifi_start_portal_internal();
}

bool portal_is_active(void) {
    return portal.active;
}

bool portal_config_saved(void) {
    return portal.config_saved;
}

void portal_clear_config_saved(void) {
    portal.config_saved = false;
}

void portal_poll(void) {
    if (portal.active) {
        cyw43_arch_poll();
    }
}
