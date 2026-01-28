# Notes

- Local reference: `/opt/MacDevDocs` contains Apple legacy Mac documentation to consult when needed for this project.
- ATX `PS_ON` is driven through a ULN2803, so GPIO9 high asserts the PSU on (active-low at the ATX header).
- PIXCLK phase-lock after HSYNC (and a pre-roll high before falling-edge capture) prevents occasional 1-pixel capture phase slips.
- If HSYNC edge polarity is ever changed from the default, retune XOFF before enabling capture; otherwise the window can straddle horizontal blanking and produce a stable but incorrect black band.
- `scripts/cdc_cmd.py` is a quick helper for sending CDC command bytes and reading ASCII responses.
- `scripts/ab_capture.py` expects firmware support for the `O` command to toggle VIDEO inversion between runs.
- Classic compact Mac video timing: dot clock ~15.6672 MHz, HSYNC ~22.25 kHz (≈45 µs line), VSYNC ~60.15 Hz with ~180 µs low pulse; HSYNC continues during VSYNC and DATA idles high between active pixels.
- Classic compact Mac HSYNC and VIDEO polarity are inverted compared to TTL PC monitor expectations (VSYNC polarity matches).
- UDP video streaming stores Wi-Fi credentials in flash; if missing, the device starts a captive portal AP (`EBD-IPKVM-Setup`) with DHCP/DNS/HTTP setup and saves SSID/password plus UDP target settings.
- The config HTTP server stays up in station mode; only the DHCP/DNS captive-portal services are AP-only.
- PS_ON now auto-arms capture and can be toggled from the portal UI (in both AP and station modes).
- If the SDK doesn't export `cyw43_arch_wifi_scan`, the firmware provides a weak shim that forwards to `cyw43_wifi_scan` so portal scans can always call the wrapper name.
