# ADB wiring + control notes

## Status

- ADB keyboard/mouse emulation is **in progress**. The immediate focus is enabling a first on-bus test: observe host polling, respond to Talk, and inject basic keyboard/mouse events via CDC2 per the implementation plan.
- Current firmware exposes CDC2 for ADB test input and latches basic RX activity on the shared bus; basic Talk/Listen handling is under active bring-up.
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
- PIO RX counts decrement once per loop iteration (2 PIO cycles), so pulse widths are scaled by 2 ticks when converting to microseconds.
- RX pushes are non-blocking with a joined RX FIFO so bursts cannot stall the state machine.
- The attention pulse detector is tightened to 700–900 µs now that capture skew is under control; sync remains 60–90 µs.
- Minimal Talk responses are emitted for the active keyboard address (default 2):
  - Reg 0: queued CDC2 key events (0xFF fill when idle).
  - Reg 3: handler ID + current address byte.
- Listen reg 3 is accepted to update the keyboard address (handler ID 0x00/0xFE) or handler ID (0x02/0x03); SRQ/collision flags remain unimplemented.
- TX low-pulse timing uses the PIO TX loop (1 cycle per decrement) with a one-cycle adjustment for the `set pindirs` assert.

### PIO timing takeaways (ADB RX/TX)
- Treat PIO loops as **multi-cycle timers**: count cycles per iteration and convert tick counts using that cadence, not just instruction count.
- If the loop includes extra work (e.g., `set pindirs`, `wait`, or a branch delay), fold that into the conversion so pulses match real-time microseconds.
- RX and TX loops can have different cycles-per-iteration, so document each program’s cadence separately instead of sharing a single “ticks-to-us” formula.
- When tuning pulse-width filters, always validate with a scope or diagnostic bins to catch off-by-one-cycle errors early.
