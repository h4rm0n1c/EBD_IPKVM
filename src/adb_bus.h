#ifndef ADB_BUS_H
#define ADB_BUS_H

#include <stdbool.h>

void adb_bus_init(void);
bool adb_bus_service(void);
bool adb_bus_take_activity(void);

#endif
