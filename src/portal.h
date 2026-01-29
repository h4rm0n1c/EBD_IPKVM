#ifndef EBD_IPKVM_PORTAL_H
#define EBD_IPKVM_PORTAL_H

#include <stdbool.h>
#include <stdint.h>

#include "wifi_config.h"

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

typedef void (*portal_ps_on_cb)(bool on, void *ctx);

void portal_init(portal_ps_on_cb ps_on_cb, void *ctx);
bool portal_has_config(void);
const wifi_config_t *portal_get_config(void);
bool portal_start_station(void);
bool portal_start_portal(void);
bool portal_is_active(void);
bool portal_config_saved(void);
void portal_clear_config_saved(void);
void portal_poll(void);

#endif
