# Notes

- Local reference: `/opt/MacDevDocs` contains Apple legacy Mac documentation to consult when needed for this project.
- ATX `PS_ON` is driven through a ULN2803, so GPIO9 high asserts the PSU on (active-low at the ATX header).
- HSYNC and VSYNC are buffered through a 74HC14 Schmitt-trigger inverter before reaching the Pico inputs.
- HSYNC/VSYNC path is Mac → 74HC14 → 74LVC245 → Pico.
- Firmware defaults are HSYNC fall, VSYNC fall, PIXCLK rise, VIDEO inversion off (stable capture set).
- Use CDC command `O` to toggle VIDEO inversion if the capture looks unstable through the 74HC14 → 74LVC245 path.
- Use CDC command `0` to clear VIDEO inversion and `1` to set it explicitly.
- `scripts/ab_capture.py` runs back-to-back capture runs with VIDEO inversion toggled for A/B comparison.
- `scripts/cdc_cmd.py` can be used to send `I`/`G` and read ASCII output from the Pico.
- Use CDC command `E` to print GPIO edges/sec for PIXCLK/HSYNC/VSYNC/VIDEO over a 1s window.
