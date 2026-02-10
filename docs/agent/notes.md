# Notes

- In `lsusb -t`, CDC ACM shows two interfaces (`Communications` + `CDC Data`) for one serial function; this is expected and does not indicate two separate CDC control channels.
- Firmware now hard-fails build if `CFG_TUD_CDC` is not exactly 1, guarding against accidental reintroduction of a CDC video interface.
- Legacy CDC video transport is retired: video ingestion must use vendor bulk, and CDC should be treated as control/debug text only.
- Local reference: `/opt/MacDevDocs` contains Apple legacy Mac documentation to consult when needed for this project.
- Local reference: `/opt/Pico-SDK` and `/opt/PicoHTTPServer` are available for Pico SDK API details and Pico W captive-portal web UI patterns.
- Local reference: `/opt/SigrokPico` documents USB CDC RLE streaming patterns, and `/opt/picovga` shows a core1 video pipeline split for RP2040.
- MacDevDocs includes Macintosh Classic and Classic II developer notes; Classic lists /VSYNC and /HSYNC on the power/sweep connector and Classic II documents 512×342 internal video timing (EAGLE-generated).
- ATX `PS_ON` is driven through a ULN2803, so GPIO9 high asserts the PSU on (active-low at the ATX header).
- PIXCLK phase-lock after HSYNC (and a pre-roll high before falling-edge capture) prevents occasional 1-pixel capture phase slips.
- If HSYNC edge polarity is ever changed from the default, retune XOFF before enabling capture; otherwise the window can straddle horizontal blanking and produce a stable but incorrect black band.
- `scripts/cdc_cmd.py` is a quick helper for sending CDC command bytes and reading ASCII responses.
- Control/debug traffic uses CDC0 while binary video runs on vendor bulk; host tooling should open CDC only for logs/commands.
- `host_recv_frames.py` auto-detects the CDC control port via `/dev/serial/by-id/*if01*`; pass `--ctrl-device` if udev naming is unavailable or ambiguous.
- Web client dependency changes require upgrading the editable install (`pip install -e . --upgrade`); missing `serial` indicates `pyserial` is not installed yet.
- Web client video ingest uses the USB bulk interface via pyusb (same path as `host_recv_frames.py`), not the CDC tty stream.
- If `ModuleNotFoundError: No module named 'usb'` appears, install `python3-usb` (Debian/Devuan) and re-run `pip install -e . --upgrade`.
- Linux udev symlinks usually expose CDC control as `...-if01...`; use `/dev/serial/by-id` for stable naming.
- `scripts/ab_capture.py` expects firmware support for the `O` command to toggle VIDEO inversion between runs.
- Classic compact Mac video timing: dot clock ~15.6672 MHz, HSYNC ~22.25 kHz (≈45 µs line), VSYNC ~60.15 Hz with ~180 µs low pulse; HSYNC continues during VSYNC and DATA idles high between active pixels.
- Classic compact Mac HSYNC and VIDEO polarity are inverted compared to TTL PC monitor expectations (VSYNC polarity matches).
- ADB implementation references: macfriends (`/opt/adb/macfriends`, Arduino core + host client over USB serial), hootswitch (`/opt/adb/hootswitch`, PIO + DMA bus engine), trabular (https://github.com/saybur/trabular), tashtrio (https://github.com/lampmerchant/tashtrio), adb-test-device (https://github.com/lampmerchant/adb-test-device), QuokkADB firmware (https://github.com/rabbitholecomputing/QuokkADB-firmware), HIDHopper_ADB (https://github.com/TechByAndroda/HIDHopper_ADB), adbuino (https://github.com/akuker/adbuino), adb-usb (https://github.com/gblargg/adb-usb), Apple ADB Manager PDF (https://developer.apple.com/library/archive/documentation/mac/pdf/Devices/ADB_Manager.pdf), Apple HW technote hw_01 (https://developer.apple.com/legacy/library/technotes/hw/hw_01.html#Extended), Microchip AN591B (`/opt/adb/miscdocs/an591b.pdf`), Lopaciuk ADB protocol summary (https://www.lopaciuk.eu/2021/03/26/apple-adb-protocol.html), TMK ADB wiki (https://github.com/tmk/tmk_keyboard/wiki/Apple-Desktop-Bus), MiSTer plus_too ADB core (https://raw.githubusercontent.com/mist-devel/plus_too/refs/heads/master/adb.v).
  - Hootswitch `bus.pio` includes host+device TX/RX/attention programs; device-side logic lives in `computer.c` and shows collision detection + SRQ gating suitable for us to adapt.

- ADB timing reference: trabular notes that the serial handler must run roughly every 50–70 µs and defines timing windows for attention/sync/bit pulses in its ADB bus implementation.
- ADB default device addresses in trabular: keyboard=2, mouse=3; handler IDs reset alongside addresses on bus reset.
- ADB RX/TX are tied to the same shared bus; plan to filter out local TX from RX processing except when explicitly testing loopback timing.
- Current board wiring: UART1 on GPIO20/21 connects the Pico to the ATmega328p (MacFriends core); Arduino TX → Pico RX needs a resistor divider to avoid 5V on RP2040 GPIO.
- ADB CDC test channel should emit a rate-limited RX-activity line when valid ADB traffic is observed, to confirm host queries are being received.
- Validate ADB behavior against the reference implementations stored in `/opt/adb` during bring-up.

- Browser pointer lock for canvas-relative mouse capture requires a user gesture (canvas click) before movement events include `movementX/movementY`; unlocked state should send a mouse-up packet to avoid stuck button state.

- Browser keyboard capture now maps `KeyboardEvent.code` values to Mac-style scan codes for serial transport; unmapped keys are intentionally ignored to avoid sending incorrect scan codes.

- ROM-disk boot assist releases keys in a `finally` path and on session stop/disconnect to reduce risk of stuck modifier state if the hold task is interrupted.

- Field testing indicates ROM-disk boot chord timing may need a longer post-power-on hold; 45s currently improves reliability versus 30s.
