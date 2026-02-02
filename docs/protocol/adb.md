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
