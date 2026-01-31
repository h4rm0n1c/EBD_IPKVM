#pragma once

#include <stdbool.h>
#include <stdint.h>

void adb_test_cdc_init(void);
bool adb_test_cdc_poll(void);
bool adb_test_cdc_take_diag_request(void);
