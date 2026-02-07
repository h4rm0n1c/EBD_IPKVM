#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * Diagnostic HID mode — bridges CDC1 terminal input to ADB keyboard & mouse
 * via the ATtiny85/trabular SPI link.
 *
 * Toggle:    'A' in normal CDC command mode enters ADB-diag mode.
 *            ESC ESC (double-escape) exits back to normal mode.
 *
 * Keyboard:  Printable ASCII chars → ADB key-down then key-up.
 * Mouse:     Arrow keys → mouse movement (±8 pixels per press).
 *            PgUp → toggle primary click (press-and-hold / release).
 *            PgDn → momentary click (down then up after 100 ms).
 *
 * Boot macro: Ctrl-X in diag mode sends Cmd-Opt-X-O held for ~30 s
 *             (for booting into the ROM OS on supported Macs).
 */

/* Call once after adb_spi_init(). */
void diag_hid_init(void);

/* Returns true if diagnostic HID mode is currently active. */
bool diag_hid_active(void);

/* Enter diagnostic HID mode. */
void diag_hid_enter(void);

/* Exit diagnostic HID mode. */
void diag_hid_exit(void);

/* Feed one byte from CDC1 RX into the escape-sequence parser.
 * Only call when diag_hid_active() is true. */
void diag_hid_feed(uint8_t ch);

/* Call periodically (~every poll loop) to service timed actions
 * (momentary click release, boot macro hold timer). */
void diag_hid_poll(void);
