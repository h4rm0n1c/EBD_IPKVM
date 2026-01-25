# Decisions (running)

- 2026-01-25: Repository initialised with docs-first workflow and agent documentation scaffold.
- 2026-01-26: Use GPIO9 (via ULN2803) as an active-high ATX PS_ON output; add CDC commands for PS_ON control, BOOTSEL, and watchdog reset.
- 2026-01-26: Use polling-based edge sampling for GPIO diagnostics to avoid interrupt overload on PIXCLK while still confirming activity.
- 2026-01-26: Add a force-capture command to bypass VSYNC gating for troubleshooting capture stalls.
- 2026-01-26: Add a synthetic test-frame command to validate USB streaming when capture remains stalled.
- 2026-01-26: Skip force-capture fallback when explicitly running synthetic test frames.
- 2026-01-26: Add a probe packet command to verify CDC byte delivery independently of framing.
- 2026-01-26: Emit a probe packet alongside synthetic test frames for immediate CDC verification.
- 2026-01-26: Add an on-demand debug command to expose CDC/queue state without a scope.
- 2026-01-26: Default PIXCLK sampling to the falling edge and invert VIDEO bits in firmware to compensate for 74HC14 inversion.
- 2026-01-26: Buffer PIXCLK and VIDEO through a 74HC14 Schmitt-trigger inverter before the Pico inputs.
- 2026-01-25: Switch line-capture trigger to HSYNC falling edge to validate sync polarity.
- 2026-01-25: Set horizontal skip to 178 PIXCLK cycles after HSYNC falling edge based on VCD sweep results.
- 2026-01-25: Add runtime CDC toggles for HSYNC/VSYNC edge selection to speed capture validation.
- 2026-01-25: Add runtime CDC toggle for PIXCLK edge selection to diagnose sampling phase.
