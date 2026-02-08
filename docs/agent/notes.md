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
- ADB implementation references: trabular (`/opt/adb/trabular`), trabatar (`/opt/adb/trabatar`), tashtrio (https://github.com/lampmerchant/tashtrio), adb-test-device (https://github.com/lampmerchant/adb-test-device), QuokkADB firmware (https://github.com/rabbitholecomputing/QuokkADB-firmware), HIDHopper_ADB (https://github.com/TechByAndroda/HIDHopper_ADB), adbuino (https://github.com/akuker/adbuino), adb-usb (https://github.com/gblargg/adb-usb), Apple ADB Manager PDF (https://developer.apple.com/library/archive/documentation/mac/pdf/Devices/ADB_Manager.pdf), Apple HW technote hw_01 (https://developer.apple.com/legacy/library/technotes/hw/hw_01.html#Extended), Microchip AN591B (`/opt/adb/miscdocs/an591b.pdf`).

- ADB timing reference: trabular notes that the serial handler must run roughly every 50–70 µs and defines timing windows for attention/sync/bit pulses in its ADB bus implementation.
- ATtiny85/trabular SPI framing uses 8-bit command bytes with a dummy byte to clock out the one-byte-delayed response; CS high resets the USI counter between commands.
- Trabatar’s serial driver sends keyboard low nibble (0x4N) then high nibble (0x5N for press, 0x58+hi for release) and mouse movement sends low nibble first with optional high nibble (0x80/0x90/0xA0/0xB0 for X, 0xC0/0xD0/0xE0/0xF0 for Y) with a 0.5 deceleration factor; mouse button codes use 0x60–0x63 (SPI should mirror this ordering).
- ADB diag mouse-keys mode uses a 5 px step per keypress for fine-grained pointer testing.
- Trabular mouse Talk reg0 returns two bytes: first byte = Y delta (7-bit, clamped to ±64) with bit7 = ~button1; second byte = X delta (7-bit, clamped to ±64) with bit6 = ~button2. After a successful talk, trabular drains the reported deltas from its accumulators.
- Enabling `USE_ARBITRARY` in trabular adds extra serial command cases (0x02/0x03 for register 0, 0x0C–0x0F for register 2 reads) and additional status bits for the arbitrary device; it should not change mouse/keyboard motion encoding unless the host is issuing those arbitrary-device commands or polling its address.
- The ATtiny85 SPI link is brought up 500 ms after PS_ON is asserted so the ADB device appears after Mac power-on.
- ADB default device addresses in trabular: keyboard=2, mouse=3; handler IDs reset alongside addresses on bus reset.
- ADB RX/TX are tied to the same shared bus; plan to filter out local TX from RX processing except when explicitly testing loopback timing.
- Current board wiring: GPIO6 is ADB RECV (non-inverting) and GPIO12 is ADB XMIT (inverted open-collector), so PIO output polarity must account for the inversion.
- ADB CDC test channel should emit a rate-limited RX-activity line when valid ADB traffic is observed, to confirm host queries are being received.
- Validate ADB behavior against the reference implementations stored in `/opt/adb` during bring-up.
