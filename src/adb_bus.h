#ifndef ADB_BUS_H
#define ADB_BUS_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t lock_fails;
    uint32_t collisions;
    uint32_t attentions;
    uint32_t attention_short;
    uint32_t resets;
    uint32_t aborts;
    uint32_t abort_time;
    uint32_t errors;
} adb_bus_stats_t;

struct adb_device;

void adb_bus_init(void);
bool adb_bus_service(void);
bool adb_bus_take_activity(void);
void adb_bus_get_stats(adb_bus_stats_t *out);
bool adb_bus_set_handler_id_fn(uint8_t address, uint8_t (*fn)(uint8_t address, uint8_t stored_id));
bool adb_bus_set_reg0_pop(uint8_t address, bool (*fn)(struct adb_device *dev, uint8_t *first, uint8_t *second), void *queue_ctx);
bool adb_bus_set_listen_fn(uint8_t address, void (*fn)(uint8_t address, uint8_t reg, const uint8_t *data, uint8_t len, void *ctx), void *ctx);
bool adb_bus_set_flush_fn(uint8_t address, void (*fn)(uint8_t address, uint8_t reg, void *ctx), void *ctx);
void *adb_bus_get_reg0_ctx(struct adb_device *dev);

#endif
