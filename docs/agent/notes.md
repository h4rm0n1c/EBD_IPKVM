# Notes

- Local reference: `/opt/MacDevDocs` contains Apple legacy Mac documentation to consult when needed for this project.
- Local reference: `/opt/Pico-SDK` and `/opt/PicoHTTPServer` are available for Pico SDK API details and Pico W captive-portal web UI patterns.
- Local reference: `/opt/SigrokPico` documents USB CDC RLE streaming patterns, and `/opt/picovga` shows a core1 video pipeline split for RP2040.
- MacDevDocs includes Macintosh Classic and Classic II developer notes; Classic lists /VSYNC and /HSYNC on the power/sweep connector and Classic II documents 512×342 internal video timing (EAGLE-generated).
- ATX `PS_ON` is driven through a ULN2803, so GPIO9 high asserts the PSU on (active-low at the ATX header).
- PIXCLK phase-lock after HSYNC (and a pre-roll high before falling-edge capture) prevents occasional 1-pixel capture phase slips.
- If HSYNC edge polarity is ever changed from the default, retune XOFF before enabling capture; otherwise the window can straddle horizontal blanking and produce a stable but incorrect black band.
- `scripts/cdc_cmd.py` is a quick helper for sending CDC command bytes and reading ASCII responses.
- Control/status traffic now uses CDC1 while CDC0 is reserved for the binary video stream; host tooling must open the second CDC interface for commands.
- Host scripts default to `/dev/ttyACM0` for the CDC0 stream and `/dev/ttyACM1` for CDC1 control unless overridden.
- Linux udev symlinks include `if00` (CDC0 stream) and `if02` (CDC1 control); use `/dev/serial/by-id` for stable naming.
- `scripts/ab_capture.py` expects firmware support for the `O` command to toggle VIDEO inversion between runs.
- Classic compact Mac video timing: dot clock ~15.6672 MHz, HSYNC ~22.25 kHz (≈45 µs line), VSYNC ~60.15 Hz with ~180 µs low pulse; HSYNC continues during VSYNC and DATA idles high between active pixels.
- Classic compact Mac HSYNC and VIDEO polarity are inverted compared to TTL PC monitor expectations (VSYNC polarity matches).
- ADB implementation references: trabular (`/opt/adb/trabular`, https://github.com/saybur/trabular) plus the Apple ADB Manager PDF and AN591B in `/opt/adb/miscdocs`.
- Trabular SPI cadence: the serial handler must run roughly every 50–70 µs, so SPI bytes should be paced to give the ATtiny85 polling loop time to service USIOIF.
- Trabular SPI responses are returned on the next transfer; hosts must clock a dummy byte after command bytes that expect replies (e.g., status).
