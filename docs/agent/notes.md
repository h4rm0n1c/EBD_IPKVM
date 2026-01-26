# Notes

- Local reference: `/opt/MacDevDocs` contains Apple legacy Mac documentation to consult when needed for this project.
- ATX `PS_ON` is driven through a ULN2803, so GPIO9 high asserts the PSU on (active-low at the ATX header).
- PIXCLK phase-lock after HSYNC (and a pre-roll high before falling-edge capture) prevents occasional 1-pixel capture phase slips.
- `scripts/cdc_cmd.py` is a quick helper for sending CDC command bytes and reading ASCII responses.
- `scripts/ab_capture.py` expects firmware support for the `O` command to toggle VIDEO inversion between runs.
- Classic compact Mac video timing: dot clock ~15.6672 MHz, HSYNC ~22.25 kHz (≈45 µs line), VSYNC ~60.15 Hz with ~180 µs low pulse; HSYNC continues during VSYNC and DATA idles high between active pixels.
- Classic compact Mac HSYNC and VIDEO polarity are inverted compared to TTL PC monitor expectations (VSYNC polarity matches).
