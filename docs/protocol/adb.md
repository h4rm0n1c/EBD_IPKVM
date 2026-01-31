# ADB wiring + control notes

## GPIO assignment

| Signal | GPIO | Direction | Level shifting | Notes |
| ------ | ---- | --------- | -------------- | ----- |
| ADB RECV | GPIO7 | Input | 74LVC245 | Receives ADB data from the Mac. |
| ADB XMIT | GPIO8 | Output | ULN2803 | Drives the shared ADB data line (open-collector). |

## Electrical constraints
- The ADB data line is shared between RECV and XMIT; ensure open-collector behavior on the transmit path.
- Do not connect 5V ADB signals directly to RP2040 GPIO; use level shifting (74LVC245) and an open-collector driver (ULN2803).
- Ground is common between the Mac ADB port and the RP2040.

## Implementation notes
- ADB should follow the same core split as the video pipeline: PIO for timing and core1 for RX/TX state handling, with core0 only enqueueing host commands.
