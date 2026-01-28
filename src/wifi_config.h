#pragma once

#include <stdbool.h>
#include <stdint.h>

#define WIFI_CFG_MAX_SSID 32
#define WIFI_CFG_MAX_PASS 64
#define WIFI_CFG_MAX_ADDR 15

typedef struct wifi_config {
    uint32_t magic;
    char ssid[WIFI_CFG_MAX_SSID + 1];
    char pass[WIFI_CFG_MAX_PASS + 1];
    char udp_addr[WIFI_CFG_MAX_ADDR + 1];
    uint16_t udp_port;
    uint32_t checksum;
} wifi_config_t;

bool wifi_config_load(wifi_config_t *cfg);
bool wifi_config_save(wifi_config_t *cfg);
void wifi_config_clear(void);
