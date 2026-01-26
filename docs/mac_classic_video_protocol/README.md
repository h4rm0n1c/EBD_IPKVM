# Macintosh Classic video protocol references

This folder collects PDFs and snapshots of web resources about classic Macintosh video output.

## How to use these sources

- Use `pdftotext` (already available in the container) for quick text extraction when you need timing, pinout, or sync details.
- The consolidated, KVM-focused summary lives in `docs/mac_classic_video_capture_notes.md`.

## Inventory

- *Classic Macintosh Video Signals Demystified, Designing a Mac-to-VGA Adapter with LM1881* (Big Mess o' Wires) — monitor ID pins, composite sync behavior, sync-on-green details.
- *Control a Macintosh Classic CRT with a BeagleBone Black (Part 1)* — Classic CRT HSYNC/VSYNC/DATA timing.
- *Mac SE/30 video interface* — SE/30 timing notes and capture window timing.
- *Macintosh Classic II Developer Note* — 512x342 timing chart and dot clock.
- *Mac Plus Analog Board* — sync polarity + line rate notes for classic compact Macs.

> Tip: When you learn a new constraint (timing, polarity, or gating behavior), record it in `docs/agent/notes.md` and update `docs/agent/log.md`.
