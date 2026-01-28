#include "wifi_config.h"

#include <stddef.h>
#include <string.h>

#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/stdlib.h"

#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES (2 * 1024 * 1024)
#endif

#define WIFI_CFG_MAGIC 0xEBD1F001u
#define WIFI_CFG_SECTOR_SIZE FLASH_SECTOR_SIZE
#define WIFI_CFG_OFFSET (PICO_FLASH_SIZE_BYTES - WIFI_CFG_SECTOR_SIZE)

static uint32_t wifi_cfg_checksum(const wifi_config_t *cfg) {
    const uint8_t *p = (const uint8_t *)cfg;
    size_t len = offsetof(wifi_config_t, checksum);
    uint32_t hash = 5381u;
    for (size_t i = 0; i < len; i++) {
        hash = ((hash << 5) + hash) ^ p[i];
    }
    return hash;
}

static void wifi_cfg_sanitize(wifi_config_t *cfg) {
    cfg->ssid[WIFI_CFG_MAX_SSID] = '\0';
    cfg->pass[WIFI_CFG_MAX_PASS] = '\0';
    cfg->udp_addr[WIFI_CFG_MAX_ADDR] = '\0';
    if (cfg->udp_port == 0) {
        cfg->udp_port = 5004;
    }
}

bool wifi_config_load(wifi_config_t *cfg) {
    if (!cfg) return false;
    const uint8_t *flash = (const uint8_t *)(XIP_BASE + WIFI_CFG_OFFSET);
    memcpy(cfg, flash, sizeof(*cfg));
    if (cfg->magic != WIFI_CFG_MAGIC) {
        return false;
    }
    if (wifi_cfg_checksum(cfg) != cfg->checksum) {
        return false;
    }
    wifi_cfg_sanitize(cfg);
    if (cfg->ssid[0] == '\0') {
        return false;
    }
    return true;
}

bool wifi_config_save(wifi_config_t *cfg) {
    if (!cfg) return false;
    wifi_cfg_sanitize(cfg);
    cfg->magic = WIFI_CFG_MAGIC;
    cfg->checksum = wifi_cfg_checksum(cfg);

    uint8_t sector[WIFI_CFG_SECTOR_SIZE];
    memset(sector, 0xFF, sizeof(sector));
    memcpy(sector, cfg, sizeof(*cfg));

    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(WIFI_CFG_OFFSET, WIFI_CFG_SECTOR_SIZE);
    flash_range_program(WIFI_CFG_OFFSET, sector, WIFI_CFG_SECTOR_SIZE);
    restore_interrupts(ints);
    return true;
}

void wifi_config_clear(void) {
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(WIFI_CFG_OFFSET, WIFI_CFG_SECTOR_SIZE);
    restore_interrupts(ints);
}
