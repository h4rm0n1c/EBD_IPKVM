# ADB wiring + control notes

## Status

- ADB keyboard/mouse emulation is **in progress**. The immediate focus is enabling a first on-bus test: observe host polling, respond to Talk, and inject basic keyboard/mouse events via CDC2 per the implementation plan.
- Current firmware exposes CDC2 for ADB test input and latches basic RX activity on the shared bus; full Talk/Listen response handling is still being built.
- Initial PIO RX/TX programs are in place to capture ADB low-pulse widths and drive low pulses on the bus (PIO1, separate RX/TX state machines).

## GPIO assignment

| Signal | GPIO | Direction | Level shifting | Notes |
| ------ | ---- | --------- | -------------- | ----- |
| ADB RECV | GPIO6 | Input | 74LVC245 | Receives ADB data from the Mac. |
| ADB XMIT | GPIO14 | Output | ULN2803 | Drives the shared ADB data line (open-collector). |

## Electrical constraints
- The ADB data line is shared between RECV and XMIT; ensure open-collector behavior on the transmit path.
- Do not connect 5V ADB signals directly to RP2040 GPIO; use level shifting (74LVC245) and an open-collector driver (ULN2803).
- Ground is common between the Mac ADB port and the RP2040.

## Implementation notes
- ADB should follow the same core split as the video pipeline: PIO for timing and core1 for RX/TX state handling, with core0 only enqueueing host commands.
- The current PIO RX program runs with a clkdiv of 8 and reports pulse widths in microseconds after firmware conversion; pulse filters are expressed in µs.
- RX pushes are non-blocking with a joined RX FIFO so bursts cannot stall the state machine.
