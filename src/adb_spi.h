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
 *   GP16 = MISO (RX) ← ATtiny85 PB1 (DO, pin 6)
 *   GP17 = RESET      → ATtiny85 /RESET (pin 1, active low)
 *   GP18 = SCK        → ATtiny85 PB2 (USCK, pin 7)
 *   GP19 = MOSI (TX)  → ATtiny85 PB0 (DI, pin 5)
 *   GP20 = CS         → ATtiny85 PB4 (pin 3, active low)
 *
 * Per-byte protocol:
 *   1. Assert CS low
 *   2. Clock 8 bits (MOSI out, MISO in)
 *   3. Wait >=150 µs with CS still low (ATtiny handle_data() gates on CS)
 *   4. Deassert CS high (resets USI 4-bit counter for clean next byte)
 *   5. Next MISO byte carries the response to the previous command
 *
 * Trabular command encoding: upper nibble = command, lower = 4-bit payload.
 *   Keyboard:  0x4N low, 0x5N high → queues ADB keycode
 *   Mouse btn: 0x6N low, 0x7N high
 *   Mouse X:   0x8N += N, 0x9N -= N, 0xAN += N<<4, 0xBN -= N<<4
 *   Mouse Y:   0xCN += N, 0xDN -= N, 0xEN += N<<4, 0xFN -= N<<4
 *   Status:    0x01 query, 0x00 NOP (clock out previous response)
 *
 * Init is glitch-free (CS/SCK/MOSI pre-conditioned before mux switch)
 * and zero-blocking.  Flush is deferred to adb_spi_flush().
 */

/* Hold ATtiny85 in reset (GP17 LOW), drive CS high (inactive), SCK LOW.
 * Call this as early as possible in main(), before stdio/USB init,
 * so the ATtiny85 USI never sees spurious clock edges. */
void adb_spi_hold_reset(void);

/* Bring up SPI0, claim pins, then release ATtiny85 from reset.
 * SCK is stable LOW before the ATtiny85 USI starts counting.
 * Safe to call multiple times (idempotent). */
void adb_spi_init(void);

/* Flush all trabular buffers (keyboard, mouse, arb device).
 * Sends ONE clear command per call so the caller can yield to
 * tud_task() between bytes.  Returns true when flush is complete.
 * Idempotent — subsequent calls after completion return true immediately. */
bool adb_spi_flush(void);

/* Release SPI0 and return pins to hi-Z inputs.
 * Safe to call when already deinited. */
void adb_spi_deinit(void);

/* True if SPI0 is currently initialised. */
bool adb_spi_is_active(void);

/* Send a raw trabular command byte with CS assert/deassert.
 * Returns the MISO byte clocked in (response to PREVIOUS command).
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
