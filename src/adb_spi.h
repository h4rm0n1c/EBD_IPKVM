#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * SPI master driver for talking to an ATtiny85 running saybur/trabular firmware.
 *
 * Pico SPI0 pins:
 *   GP19 = MOSI (TX) → ATtiny85 PB1 (DI)
 *   GP16 = MISO (RX) ← ATtiny85 PB0 (DO)
 *   GP18 = SCK        → ATtiny85 PB2 (USCK)
 *
 * Trabular protocol: single-byte SPI transactions.
 *   Upper nibble = command, lower nibble = 4-bit payload.
 *
 * Keyboard:  0x4N = key low nibble, 0x5N = key high nibble (pushes ADB keycode)
 * Mouse btn: 0x6N = btn low nibble,  0x7N = btn high nibble
 * Mouse X:   0x8N = X+=N, 0x9N = X-=N, 0xAN = X+=N<<4, 0xBN = X-=N<<4
 * Mouse Y:   0xCN = Y+=N, 0xDN = Y-=N, 0xEN = Y+=N<<4, 0xFN = Y-=N<<4
 * Status:    0x01 = query status
 *
 * IMPORTANT: init/deinit are lazy — SPI is only active while diag mode
 * is running.  Pins are returned to safe hi-Z inputs on deinit so the
 * ATtiny85 USI isn't fed garbage at boot.
 */

/* Bring up SPI0, claim pins, and flush trabular's buffers.
 * Safe to call multiple times (idempotent). */
void adb_spi_init(void);

/* Release SPI0 and return pins to hi-Z inputs.
 * Safe to call when already deinited. */
void adb_spi_deinit(void);

/* True if SPI0 is currently initialised. */
bool adb_spi_is_active(void);

/* Send a raw trabular command byte. Returns the response byte.
 * No-op (returns 0) if SPI is not active. */
uint8_t adb_spi_xfer(uint8_t cmd);

/* Send an ADB keycode (bit 7 = release flag, bits 6:0 = ADB scancode).
 * key_down=true: sends code with bit7=0.  key_down=false: sends code|0x80. */
void adb_spi_send_key(uint8_t adb_code, bool key_down);

/* Set mouse button state.  btn1=primary click, btn2=secondary. */
void adb_spi_set_buttons(bool btn1, bool btn2);

/* Accumulate mouse motion.  dx/dy are signed: +X=right, +Y=down. */
void adb_spi_move_mouse(int8_t dx, int8_t dy);

/* Query trabular status byte. */
uint8_t adb_spi_status(void);
