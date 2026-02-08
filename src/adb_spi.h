#pragma once

#include <stdbool.h>
#include <stdint.h>

/* ── SPI trace buffer (diagnostic) ────────────────────────────────── */
#define ADB_SPI_TRACE_LEN 16

typedef struct {
    uint8_t tx;
    uint8_t rx;
} adb_spi_trace_entry_t;

/*
 * SPI master driver for talking to an ATtiny85 running saybur/trabular firmware.
 *
 * Pico SPI0 pins:
 *   GP19 = MOSI (TX) → ATtiny85 PB0 (DI, pin 5)
 *   GP16 = MISO (RX) ← ATtiny85 PB1 (DO, pin 6)
 *   GP18 = SCK        → ATtiny85 PB2 (USCK, pin 7)
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
 * Init is glitch-free (SCK/MOSI pre-conditioned to idle levels before
 * mux switch) and zero-blocking.  The buffer flush is deferred to the
 * first adb_spi_flush() call so boot-time init never does SPI traffic.
 */

/* Hold ATtiny85 in reset (GP17 LOW) and pre-drive SCK LOW.
 * Call this as early as possible in main(), before stdio/USB init,
 * so the ATtiny85 USI never sees spurious clock edges. */
void adb_spi_hold_reset(void);

/* Bring up SPI0, claim pins, then release ATtiny85 from reset.
 * SCK is stable LOW before the ATtiny85 USI starts counting.
 * Safe to call multiple times (idempotent). */
void adb_spi_init(void);

/* Flush all trabular buffers (keyboard, mouse, arb device).
 * Idempotent — only sends the clear commands once.
 * Call this before the first real SPI traffic (e.g. on diag enter). */
void adb_spi_flush(void);

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

/* Reset the trace buffer (call before the operation you want to trace). */
void adb_spi_trace_reset(void);

/* Number of entries currently in the trace buffer. */
uint8_t adb_spi_trace_count(void);

/* Get a trace entry by index (0..count-1).  Returns {0,0} if out of range. */
adb_spi_trace_entry_t adb_spi_trace_entry(uint8_t idx);
