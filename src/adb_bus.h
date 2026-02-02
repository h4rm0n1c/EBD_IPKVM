#ifndef ADB_BUS_H
#define ADB_BUS_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t lock_fails;
    uint32_t collisions;
} adb_bus_stats_t;

struct adb_device;

void adb_bus_init(void);
bool adb_bus_service(void);
bool adb_bus_take_activity(void);
void adb_bus_get_stats(adb_bus_stats_t *out);
bool adb_bus_set_handler_id_fn(uint8_t address, uint8_t (*fn)(uint8_t address, uint8_t stored_id));
bool adb_bus_set_reg0_pop(uint8_t address, bool (*fn)(struct adb_device *dev, uint8_t *first, uint8_t *second));

#endif
