# Notes

- Local reference: `/opt/MacDevDocs` contains Apple legacy Mac documentation to consult when needed for this project.
- ATX `PS_ON` is driven through a ULN2803, so GPIO9 high asserts the PSU on (active-low at the ATX header).
- PIXCLK phase-lock after HSYNC (and a pre-roll high before falling-edge capture) prevents occasional 1-pixel capture phase slips.
- VSYNC IRQs are deglitched with a ~12–23 ms interval window, resync after long gaps or mid-frame VSYNC, captures are gated to ~30 fps by time, and capture auto-aborts after ~50 ms if HSYNC stalls.
- `scripts/cdc_cmd.py` is a quick helper for sending CDC command bytes and reading ASCII responses.
- `scripts/ab_capture.py` expects firmware support for the `O` command to toggle VIDEO inversion between runs.
