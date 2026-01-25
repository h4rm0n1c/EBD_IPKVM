# Notes

- Local reference: `/opt/MacDevDocs` contains Apple legacy Mac documentation to consult when needed for this project.
- ATX `PS_ON` is driven through a ULN2803, so GPIO9 high asserts the PSU on (active-low at the ATX header).
- PIXCLK and VIDEO are buffered through a 74HC14 Schmitt-trigger inverter before reaching the Pico inputs.
- PIXCLK/VIDEO path is Mac → 74HC14 → 74LVC245 → Pico.
- Firmware defaults to falling-edge PIXCLK sampling and VIDEO bit inversion to undo the 74HC14 inversion.
- Use CDC command `O` to toggle VIDEO inversion if the capture looks unstable through the 74HC14 → 74LVC245 path.
