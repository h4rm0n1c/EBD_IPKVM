#ifndef ADB_BUS_H
#define ADB_BUS_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t lock_fails;
    uint32_t collisions;
} adb_bus_stats_t;

void adb_bus_init(void);
bool adb_bus_service(void);
bool adb_bus_take_activity(void);
void adb_bus_get_stats(adb_bus_stats_t *out);

#endif
