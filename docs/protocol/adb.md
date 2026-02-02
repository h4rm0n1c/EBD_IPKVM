# ADB wiring + control notes

## GPIO assignment

| Signal | GPIO | Direction | Level shifting | Notes |
| ------ | ---- | --------- | -------------- | ----- |
| ADB RECV | GPIO6 | Input | 74LVC245 | Receives ADB data from the Mac (non-inverting). |
| ADB XMIT | GPIO12 | Output | ULN2803 | Drives the shared ADB data line (open-collector, inverted: GPIO high pulls bus low). |

## Electrical constraints
- The ADB data line is shared between RECV and XMIT; ensure open-collector behavior on the transmit path.
- GPIO12 is inverted on the transmit driver: driving the GPIO high pulls the ADB bus low, so PIO should invert its output polarity accordingly.
- Do not connect 5V ADB signals directly to RP2040 GPIO; use level shifting (74LVC245) and an open-collector driver (ULN2803).
- Ground is common between the Mac ADB port and the RP2040.

## Implementation notes
- ADB should follow the same core split as the video pipeline (AppleCore): PIO for timing and core1 for RX/TX state handling, with core0 only enqueueing host commands.
- Core1 maintains a simple ADB device model (keyboard + mouse) with four registers, Talk/Listen parsing, and SRQ gating.
- ADB timing on PIO uses the hootswitch device-side bus implementation (GPLv3), with the license captured under `licenses/hootswitch-GPLv3.txt`.

## Register model (current firmware)

### Common
- Registers are stored as up to 8 bytes each.
- Talk responses are produced when a register has a non-zero length; Talk with an empty register yields no response.
- SRQ is raised when register 0 has pending input data and SRQ is enabled in register 3.

### Keyboard (default address 2)
- **Register 0**: two keycodes per report. If only one key is queued, the second byte is `0xFF`.
  - Key releases are encoded with bit 7 set (`code | 0x80`).
- **Register 2**: reserved for modifiers/LED state (host Listen updates stored verbatim).
- **Register 3 (Talk)**: `0b01SRRRRR` (bit 5 = SRQ enable, bit 6 = exceptional event, bits 0-3 = randomized nibble), followed by handler ID. The randomized nibble mirrors hootswitch’s reg3 behavior for address-resolution flows. If the handler ID is `0xFF`, no reg3 response is produced (hootswitch parity).

#### CDC2 ASCII → ADB keycodes (current mapping)
- The CDC2 test channel maps printable ASCII to the US ADB keycode set.
- Uppercase letters map to their lowercase keycode without modifiers.
- Enter/Return maps to ADB `0x24`, Tab to `0x30`, Space to `0x31`, Escape to `0x35`, and Backspace/Delete (`0x08`/`0x7F`) to `0x33`.

### Mouse (default address 3)
- **Register 0**: two bytes.
  - Byte 0: bit 7 = button 0 state (1 = pressed), bits 0-6 = X delta (signed 7-bit).
  - Byte 1: Y delta (signed 8-bit).
- **Register 3 (Talk)**: same format as keyboard (randomized nibble + SRQ enable + handler ID).

### Listen register 3 behavior
- `low == 0x00`: set address to `up[3:0]`, update SRQ enable from `up[5]`.
- `low == 0xFE`: set address to `up[3:0]`, preserve SRQ enable.
- `low == 0xFD` or `0xFF`: ignored.
- other values: treated as handler ID proposal (stored).
