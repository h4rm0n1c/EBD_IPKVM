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
- ADB implementation references: hootswitch (`/opt/adb/hootswitch`, PIO + DMA bus engine), trabular (https://github.com/saybur/trabular), tashtrio (https://github.com/lampmerchant/tashtrio), adb-test-device (https://github.com/lampmerchant/adb-test-device), QuokkADB firmware (https://github.com/rabbitholecomputing/QuokkADB-firmware), HIDHopper_ADB (https://github.com/TechByAndroda/HIDHopper_ADB), adbuino (https://github.com/akuker/adbuino), adb-usb (https://github.com/gblargg/adb-usb), Apple ADB Manager PDF (https://developer.apple.com/library/archive/documentation/mac/pdf/Devices/ADB_Manager.pdf), Apple HW technote hw_01 (https://developer.apple.com/legacy/library/technotes/hw/hw_01.html#Extended), Microchip AN591B (`/opt/adb/miscdocs/an591b.pdf`).
  - Hootswitch `bus.pio` includes host+device TX/RX/attention programs; device-side logic lives in `computer.c` and shows collision detection + SRQ gating suitable for us to adapt.

- ADB timing reference: trabular notes that the serial handler must run roughly every 50–70 µs and defines timing windows for attention/sync/bit pulses in its ADB bus implementation.
- ATtiny85/trabular SPI framing defaults to 16-bit transfers with the command byte in the high byte and the response returned in the low byte of the next transfer; CS high resets the USI counter between bytes.
- SPI command byte placement can be flipped at build time via `ADB_SPI_CMD_IN_HIGH_BYTE` to accommodate alternate USI framing expectations.
- ADB default device addresses in trabular: keyboard=2, mouse=3; handler IDs reset alongside addresses on bus reset.
- ADB RX/TX are tied to the same shared bus; plan to filter out local TX from RX processing except when explicitly testing loopback timing.
- Current board wiring: GPIO6 is ADB RECV (non-inverting) and GPIO12 is ADB XMIT (inverted open-collector), so PIO output polarity must account for the inversion.
- ADB CDC test channel should emit a rate-limited RX-activity line when valid ADB traffic is observed, to confirm host queries are being received.
- Validate ADB behavior against the reference implementations stored in `/opt/adb` during bring-up.
