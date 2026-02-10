# ADB controller wiring + control notes

ADB is handled by an external ATmega328p running the MacFriends Arduino core. The
RP2040 speaks to that controller over UART1; the ADB bus itself is terminated on
the Arduino shield/hardware, not on the Pico GPIO.

## UART1 assignment (RP2040 ↔ ATmega328p)

| Signal | GPIO | Direction | Level shifting | Notes |
| ------ | ---- | --------- | -------------- | ----- |
| UART1 TX | GPIO20 | Output | Direct | Pico TX → Arduino RX (D0). |
| UART1 RX | GPIO21 | Input | Resistor divider | Arduino TX (D1) → Pico RX (5V→3.3V divider). |

## Electrical constraints
- Do not connect 5V UART signals directly to RP2040 GPIO; use a divider on Arduino TX (D1) → Pico RX (GPIO21).
- Divider options: 1k+2k, 4.7k+10k, or 10k+20k (top resistor from Arduino TX to node, bottom from node to GND).
- Pico GND must tie to Arduino GND.
- ADB bus level shifting and open-collector behavior are handled on the Arduino side, not the Pico.

## Duemilanove wiring (Pico ↔ Arduino)
- Pico GP20 (TX) → Arduino RX (D0) direct.
- Arduino TX (D1) → resistor divider → Pico GP21 (RX).
- GND ↔ GND.

## Implementation notes
- Base the UART protocol on the MacFriends host client, which already issues keyboard and mouse commands over USB serial to the Arduino shield.
- The Pico should treat UART1 as a transport to the ATmega328p firmware, not as a direct ADB bus driver.


## Web client serial bridge packet format

The web backend now writes MacFriends-compatible 8-byte instructions directly to the Arduino serial port (`115200` baud) when the browser sends mouse input.

- Device selection defaults to `/dev/serial/by-id/usb-FTDI_FT232R_USB_UART_*-if00-port0`.
- Override with `ADB_SERIAL_PORT` to provide an explicit tty path or glob.
- Packet bytes match `Arduino/include/instruction.h` and MacFriends `ADBInstruction.toData()`.

| Byte | Field | Type | Value |
| ---- | ----- | ---- | ----- |
| 0 | `magic` | int8 | `123` |
| 1 | `updateType` | uint8 | `1` (`UPDATE_MOUSE`) |
| 2 | `mouseIsDown` | int8 | `0` or `1` |
| 3 | `dx` | int8 | relative X delta, clamped to `[-63, 63]` |
| 4 | `dy` | int8 | relative Y delta, clamped to `[-63, 63]` |
| 5 | `keyCode` | uint8 | `0` for mouse-only packets |
| 6 | `isKeyUp` | uint8 | `0` for mouse-only packets |
| 7 | `modifierKeys` | uint8 | `0` for mouse-only packets |

The browser captures pointer-locked movement on the video canvas and sends relative `dx`/`dy` plus left-button state over WebSocket as `mouse_input`; the backend converts those into these serial packets. During capture, the browser tracks a virtual pointer bounded to the 512×342 canvas and only emits deltas for in-bounds movement.


Keyboard packets use the same 8-byte layout with `updateType=2` (`UPDATE_KEYBOARD`), `dx=0`, `dy=0`, and key fields populated (`keyCode`, `isKeyUp`, `modifierKeys`). Browser events are translated to Mac-style scan codes before transport.

In the web client capture mode, right-click exits pointer lock (instead of relying on Escape) so Escape can be forwarded as keyboard input.

When `/api/session/start` is called with `boot_rom_disk=true`, the backend asserts `Command+Option+X+O` before power-on and holds it for ~10 seconds from startup (then releases it), using standard keyboard packets (`updateType=2`). During that hold window the backend reasserts the same chord periodically to survive early-boot keyboard init timing. Modifier sequencing follows key transitions (Cmd down, Opt down, X down, O down; release in reverse).
