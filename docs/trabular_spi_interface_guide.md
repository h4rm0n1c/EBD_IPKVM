# Trabular SPI Interface Guide

This document describes the SPI/USI command protocol implemented by trabular’s firmware, based on the current codebase. It covers framing, response timing, and the command byte layout for keyboard, mouse, and arbitrary device features.

## Overview

Trabular is an ADB transceiver for AVR MCUs that can be controlled over SPI or UART in the standard configuration, allowing an external host MCU to drive ADB devices without handling ADB timing directly.【F:README.md†L1-L19】

When built **without** `USE_USART`, the firmware uses the AVR **USI three‑wire mode** as an SPI‑like slave transport.【F:serial.c†L44-L51】

## Electrical/pin notes (USI mode)

* **MISO/DO**: PB1 is configured as output for USI three‑wire mode.【F:serial.c†L44-L50】
* **CS**: PB4 is treated as active‑low chip‑select. A pull‑up is enabled and the firmware checks this pin to reset the USI counter when CS is high.【F:serial.c†L48-L76】
* **Clock/data direction**: The firmware assumes the external master clocks the USI; the code only polls the USI overflow flag and does not drive the clock.【F:serial.c†L52-L65】

> **Important:** CS is used purely as a framing/reset signal for the USI counter. When CS is **high**, the counter is reset so the next byte starts at bit 0.【F:serial.c†L66-L76】

## Transaction framing and response timing

### 8‑bit byte framing

Each **8‑bit SPI byte** is processed independently:

1. When the USI overflow flag asserts, the firmware reads the received byte from `USIBR`.
2. It computes a one‑byte response via `handle_serial_data()`.
3. It writes that response to `USIDR` for the **next** SPI transfer.【F:serial.c†L52-L65】【F:serial.c†L78-L203】

### Response latency (one‑byte delay)

Because the response byte is written *after* receiving a byte, the response to byte **N** is shifted out during byte **N+1**. To read a response, the master must clock an additional “dummy” byte (often `0x00`) after sending the command.【F:serial.c†L52-L65】【F:serial.c†L78-L203】

### CS framing expectations

The firmware resets the USI counter when **CS is high**, so for clean framing you should **toggle CS high** between logical bytes or between command sequences. Holding CS low across multiple bytes is allowed, but if you do that you must ensure byte alignment yourself (i.e., an exact multiple of 8 clocks per byte).【F:serial.c†L66-L76】

## Command byte format

Each SPI byte uses:

* **Upper nibble** (bits 7‑4): command `cmd`
* **Lower nibble** (bits 3‑0): `payload`

```text
cmd = byte >> 4
payload = byte & 0x0F
```
【F:serial.c†L78-L84】

## Command reference

> Sections below are gated by compile‑time flags (`USE_KEYBOARD`, `USE_MOUSE`, `USE_ARBITRARY`).

### Command group 0x0_ (special/control)

| Payload | Meaning | Response |
| --- | --- | --- |
| `0x01` | Talk status | `0x80` + flag bits (see below) |
| `0x02` | ARB reg0 ready | none |
| `0x03` | ARB reg0 clear | none |
| `0x04` | ARB reg2 clear | none |
| `0x05` | KBD reg0 clear | none |
| `0x06` | MSE clear buttons | none |
| `0x07` | MSE clear X motion | none |
| `0x08` | MSE clear Y motion | none |
| `0x0C` | Talk ARB reg2 byte0 low nibble | `0x40 + (arb_buf2_low & 0x0F)` |
| `0x0D` | Talk ARB reg2 byte0 high nibble | `0x50 + ((arb_buf2_low >> 4) & 0x0F)` |
| `0x0E` | Talk ARB reg2 byte1 low nibble | `0x60 + (arb_buf2_high & 0x0F)` |
| `0x0F` | Talk ARB reg2 byte1 high nibble | `0x70 + ((arb_buf2_high >> 4) & 0x0F)` |

【F:serial.c†L94-L160】

#### Status response (`0x01`)

`handle_serial_data()` returns `0x80` with additional bits:

* **Bit 3**: ARB reg2 set
* **Bit 2**: ARB reg0 set
* **Bit 0**: Keyboard buffer > half full

【F:serial.c†L98-L127】

### Command group 0x2_ / 0x3_ (arbitrary device register 0 data)

* `0x2_`: lower nibble of next ARB reg0 data byte (latched temporarily)
* `0x3_`: upper nibble; completes the byte and appends to ARB reg0 buffer (if space)

【F:serial.c†L162-L187】

### Command group 0x4_ / 0x5_ (keyboard keycodes)

* `0x4_`: lower nibble of keycode (latched temporarily)
* `0x5_`: upper nibble; completes the 8‑bit keycode and pushes into the keyboard buffer

Keycodes are 7‑bit with bit 7 indicating up/down transitions (consistent with ADB keycodes).【F:serial.c†L131-L146】【F:serial.c†L214-L347】

### Command group 0x6_ / 0x7_ (mouse buttons)

* `0x6_`: lower nibble of mouse button data
* `0x7_`: upper nibble of mouse button data

Mouse button bits are later inverted when reported on ADB (active‑low).【F:serial.c†L226-L247】【F:registers.c†L220-L227】

### Command group 0x8_–0xB_ (mouse X motion)

Let:

* `high_bits = (cmd & 0x2) >> 1`
* `positive = !(cmd & 0x1)`

Then:

* `0x8_`: X positive, low nibble (`+payload`)
* `0x9_`: X negative, low nibble (`-payload`)
* `0xA_`: X positive, high nibble (`+(payload << 4)`)
* `0xB_`: X negative, high nibble (`-(payload << 4)`)

All changes accumulate in `mse_x` (signed 16‑bit).【F:serial.c†L256-L304】

### Command group 0xC_–0xF_ (mouse Y motion)

Using the same `high_bits`/`positive` interpretation:

* `0xC_`: Y positive, low nibble (`+payload`)
* `0xD_`: Y negative, low nibble (`-payload`)
* `0xE_`: Y positive, high nibble (`+(payload << 4)`)
* `0xF_`: Y negative, high nibble (`-(payload << 4)`)

All changes accumulate in `mse_y` (signed 16‑bit).【F:serial.c†L256-L332】

## How motion and buttons are reported on ADB

When the ADB host talks to the mouse device (register 0), the firmware returns two bytes:

* **Byte 0**: Y delta (7‑bit two’s‑complement) with button 0 in bit 7
* **Byte 1**: X delta (7‑bit two’s‑complement) with button 1 in bit 6

Button bits are active‑low (inverted). The deltas are clamped to the ADB range of −64..+63 per report, and remaining motion is kept for subsequent reports.【F:registers.c†L187-L249】【F:registers.c†L251-L266】

## Example exchange

**Goal:** Move mouse down by 5.

1. Send `0xC5` (Y positive low‑nibble, +5).
2. Send a dummy byte (e.g., `0x00`) to clock the response for `0xC5` (usually `0x00`).
3. ADB host will later read register 0 and receive Y=+5, X=0 (with button bits set high when no buttons are pressed).【F:serial.c†L52-L65】【F:serial.c†L256-L332】【F:registers.c†L187-L249】
