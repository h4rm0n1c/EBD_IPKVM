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
