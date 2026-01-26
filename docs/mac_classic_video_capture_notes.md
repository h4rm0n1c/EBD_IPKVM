# Macintosh Classic video output notes (datamined)

This document summarizes timing and sync behaviors relevant to capturing the Macintosh Classic-family video output for the KVM project. It consolidates details from the reference PDFs in `docs/mac_classic_video_protocol/` so engineering decisions can be made without re-reading the entire corpus.

## Sources in this repo

- Big Mess o' Wires: *Classic Macintosh Video Signals Demystified* (Mac RGB/monitor ID/sync behavior).【F:docs/mac_classic_video_protocol/Classic Macintosh Video Signals Demystified, Designing a Mac-to-VGA Adapter with LM1881 _ Big Mess o' Wires.pdf†L310-L369】
- Nerdhut: *Control a Macintosh Classic CRT with a BeagleBone Black (Part 1)* (Classic CRT signal timing).【F:docs/mac_classic_video_protocol/Control a Macintosh Classic CRT with a BeagleBone Black - Part 1 _ Nerdhut.pdf†L41-L121】
- Trammell Hudson: *Mac SE/30 video interface* (SE/30 timing + back-porch notes).【F:docs/mac_classic_video_protocol/Mac-SE_30 video interface - Trammell Hudson's Projects.pdf†L199-L233】
- *Macintosh Classic II Developer Note* (512x342 timing + dot clock/line rate).【F:docs/mac_classic_video_protocol/mac_classic_ii.pdf†L822-L869】
- *Mac Plus Analog Board* (sync polarity + higher-than-PC line rate).【F:docs/mac_classic_video_protocol/plus_analog.pdf†L1048-L1053】

## Practical capture takeaways

### Signal set + polarity

- Classic CRT logic uses three essential signals: HSYNC, VSYNC, and a single DATA line for monochrome pixels.【F:docs/mac_classic_video_protocol/Control a Macintosh Classic CRT with a BeagleBone Black - Part 1 _ Nerdhut.pdf†L41-L77】
- In the Classic/Plus analog context, HSYNC and video are logical inverses of what TTL PC monitors expect (VSYNC polarity is fine). This inversion matters if you front-end the signal into a capture chain or sync detector that assumes PC/VGA polarity.【F:docs/mac_classic_video_protocol/plus_analog.pdf†L1048-L1051】

### Timing / rates (Classic-family internal CRT)

| Parameter | Value | Notes |
| --- | --- | --- |
| Dot clock | 15.6672 MHz | 63.83 ns per dot; 512 pixels per line take ~32.8 µs.【F:docs/mac_classic_video_protocol/Control a Macintosh Classic CRT with a BeagleBone Black - Part 1 _ Nerdhut.pdf†L75-L121】 |
| HSYNC line rate | ~22.25 kHz | ~45 µs period; ~18.45 µs low (41% duty).【F:docs/mac_classic_video_protocol/Control a Macintosh Classic CRT with a BeagleBone Black - Part 1 _ Nerdhut.pdf†L46-L107】 |
| VSYNC frame rate | ~60.15 Hz | ~16.7 ms period; ~180 µs low; occurs after 342 HSYNC edges; HSYNC continues during VSYNC.【F:docs/mac_classic_video_protocol/Control a Macintosh Classic CRT with a BeagleBone Black - Part 1 _ Nerdhut.pdf†L54-L107】 |
| Active region | 512 x 342 | Classic II timing reference lists the 512x342 active window and a 22.25 kHz line rate for this mode.【F:docs/mac_classic_video_protocol/mac_classic_ii.pdf†L822-L869】 |

Additional detail from the SE/30 interface work (useful for validating capture windows):

- HSYNC falling-edge to falling-edge is ~45 µs, with the rising edge ~18.4 µs after the falling edge; video data begins ~11.2 µs after HSYNC falling edge (implying a short post-sync delay before pixels).【F:docs/mac_classic_video_protocol/Mac-SE_30 video interface - Trammell Hudson's Projects.pdf†L206-L233】
- VSYNC low time is ~180 µs; HSYNC continues during vertical blanking, with video data starting ~1.26 ms after VSYNC in the SE/30 measurements.【F:docs/mac_classic_video_protocol/Mac-SE_30 video interface - Trammell Hudson's Projects.pdf†L206-L221】

### Monitor ID + sync encoding (external RGB Macs)

While the Classic’s internal CRT doesn’t use external RGB, the same family of video hardware uses monitor ID pins to choose sync encoding on external connectors:

- The Mac senses three monitor ID pins to decide output mode; unsupported IDs disable video output entirely.【F:docs/mac_classic_video_protocol/Classic Macintosh Video Signals Demystified, Designing a Mac-to-VGA Adapter with LM1881 _ Big Mess o' Wires.pdf†L324-L340】
- When composite sync is used, HSYNC/VSYNC may be held high while CSYNC carries the timing; the same composite sync is also embedded onto the RGB channels (sync-on-green/red/blue).【F:docs/mac_classic_video_protocol/Classic Macintosh Video Signals Demystified, Designing a Mac-to-VGA Adapter with LM1881 _ Big Mess o' Wires.pdf†L343-L369】

## Implications for the KVM capture path

- Capture logic should allow for HSYNC pulses to continue during VSYNC, and for VSYNC being a short low pulse (~180 µs) rather than a long blanking interval.【F:docs/mac_classic_video_protocol/Control a Macintosh Classic CRT with a BeagleBone Black - Part 1 _ Nerdhut.pdf†L54-L107】
- For Classic-family CRT capture, the dot clock and 512-pixel active line width strongly suggest a 32–33 µs pixel window after a ~10–11 µs HSYNC back-porch delay (matching the SE/30 timing).【F:docs/mac_classic_video_protocol/Control a Macintosh Classic CRT with a BeagleBone Black - Part 1 _ Nerdhut.pdf†L75-L121】【F:docs/mac_classic_video_protocol/Mac-SE_30 video interface - Trammell Hudson's Projects.pdf†L231-L233】
- If any future work targets external RGB connectors, the monitor ID/sync encoding rules must be respected (CSYNC vs separate syncs, sync-on-green), or the Mac will disable output.【F:docs/mac_classic_video_protocol/Classic Macintosh Video Signals Demystified, Designing a Mac-to-VGA Adapter with LM1881 _ Big Mess o' Wires.pdf†L324-L369】
