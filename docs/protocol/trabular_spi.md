# Trabular SPI interface (ATtiny85 ADB controller)

This project uses an external ATtiny85 running the upstream **trabular**
firmware as the ADB keyboard/mouse controller. The RP2040 talks to trabular
over a simple, byte-oriented SPI link; the ADB bus timing stays on the AVR.

Upstream reference: `/opt/adb/trabular` (`serial.c`, `serial.h`, `data.h`).

## Transport characteristics

- **Byte-oriented SPI**: Each transfer is one byte. The ATtiny85 polls the
  USI overflow flag in `handle_data()`, so the host must pace bytes so the
  polling loop can keep up (≈50 µs or slower between bytes).
- **One-byte-late responses**: Any response byte is written into `USIDR`
  after the current byte is processed. The host must send a *dummy* byte
  to clock the response out on the next transfer.
- **No command resync**: Trabular does not resynchronize nibble state if
  bytes are dropped or reordered; the host must enforce correct ordering.

## Command framing

Each byte is split into:

- **Upper nibble**: command (`cmd = byte >> 4`)
- **Lower nibble**: payload (`payload = byte & 0x0F`)

### Common command ranges

- **`cmd == 0`**: special commands (status, clears, read register 2 nibbles).
- **`cmd == 2` / `cmd == 3`**: arbitrary device register 0 (lower/upper nibble).
- **`cmd == 4` / `cmd == 5`**: keyboard keycode (lower/upper nibble).
- **`cmd == 6` / `cmd == 7`**: mouse button state (lower/upper nibble).
- **`cmd == 8..11`**: mouse X motion (sign + lower/upper nibble).
- **`cmd == 12..15`**: mouse Y motion (sign + lower/upper nibble).

## Response behavior

Responses are only generated for specific commands (e.g., status query, read
register 2). The response is returned on the **next** SPI transfer.

Example (status query):

1. Send `0x01` (TALK STATUS).
2. Wait at least one polling interval (≈50–150 µs).
3. Send dummy byte `0x00` to clock out the status response.

## Timing guidance

- Use a conservative inter-byte gap (≈150 µs) to ensure the ATtiny85’s polling
  loop can service the USI flag.
- Avoid bursty SPI traffic without gaps; missing the polling window can lose
  bytes and corrupt nibble ordering.
