# ADB wiring + control notes

## Status

- The in-tree ADB implementation has been retired in favor of the RP2040-focused **hootswitch** project (`/opt/adb/hootswitch`).
- Firmware no longer exposes the CDC2 ADB test channel; ADB bring-up will be driven by hootswitch integration work.

## GPIO assignment

| Signal | GPIO | Direction | Level shifting | Notes |
| ------ | ---- | --------- | -------------- | ----- |
| ADB RECV | GPIO6 | Input | 74LVC245 | Receives ADB data from the Mac. |
| ADB XMIT | GPIO12 | Output | ULN2803 | Drives the shared ADB data line (open-collector; 10k pulldown). |

## Electrical constraints
- The ADB data line is shared between RECV and XMIT; transmit is open-collector via ULN2803, so GPIO high pulls the bus low and floating releases it.
- Do not connect 5V ADB signals directly to RP2040 GPIO; use level shifting (74LVC245) and an open-collector driver (ULN2803).
- Ground is common between the Mac ADB port and the RP2040.

## Implementation notes
- Electrical constraints and pin assignments remain unchanged, but firmware-level ADB handling will be driven by hootswitch.
- Reference ADB timing/behavior details should come from `/opt/adb/hootswitch` and the Microchip AN591B tech note (`/opt/adb/miscdocs/an591b.pdf`).
