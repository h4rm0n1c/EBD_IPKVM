# Agent instructions (Codex / contributors)

You are working on **EBD_IPKVM** (RP2040/Pico W firmware + docs). Keep changes tight, testable, and documented.

## Hard rules
1) Do not commit build artifacts (`/build/`, `/frames/` are ignored).
2) Preserve existing wire/protocol behavior unless you also update the docs and any host expectations.
3) If you change:
   - pin assignments,
   - packet format,
   - timing/capture geometry,
   - capture gating/stopping behavior,
   you must update `docs/protocol/` and add a note in `docs/agent/decisions.md`.

## Documentation discipline
After any non-trivial change:
- Add a short entry to `docs/agent/log.md` (what changed + why).
- If it’s a design choice (not just refactor), add it to `docs/agent/decisions.md`.
- If you learned a constraint/quirk, write it down in `docs/agent/notes.md`.

## Reference sources
- Use `/opt/MacDevDocs` (Apple legacy Mac documentation) as a local reference when relevant to this project.
- The `docs/mac_classic_video_protocol/` folder contains PDFs/snapshots for classic Mac video timing and sync behavior; use `pdftotext` to mine details before changing capture logic.
- ADB references are available under `/opt/adb` (project snapshots) and `/opt/adb/miscdocs` (ADB Manager PDF, hardware technotes).

## Preferred reporting style
- Cite file paths.
- Cite function names / symbols.
- Provide minimal code context so changes are easy to apply.

## Safety
- Assume upstream signals may be 5V TTL.
- Never advise connecting 5V signals directly to Pico GPIO.

## Project-Specific Constraints & Gotchas (recall before proposing changes)
- Macintosh Classic video: 512×342, 1bpp, ~60 Hz interlaced-ish, negative syncs typical, pixel clock ~15.67 MHz.
- PIO (`src/classic_line.pio`): must be cycle-precise on pixel clock sampling; any added instruction can shift alignment → re-verify with scope or test pattern.
- DMA line queue: overflows drop frames silently → monitor queue depth in code.
- USB: vendor bulk stream + CDC control/ADB — preserve separation or host_recv_frames.py breaks.
- Signals: Mac video/ADB = 5V open-collector/bidirectional → **mandatory level shifters** (e.g. 74LVC245 or similar); never suggest direct GPIO connect.
- Host verification flow: `python scripts/host_recv_frames.py --pgm` for PGM output; or `--raw | ffplay -f rawvideo -pixel_format monob -video_size 512x342 -framerate 60 -`
- Use `./bootstrap_repo.sh` for fresh clones (SDK path, submodules if any).

## Mandatory Memory Check Before Any Significant Task
Start your response with:
“I have reviewed agents.md, PROJECT_STATE.md, docs/protocol/, and any existing docs/agent/* files. Relevant points / conflicts: …”
Then quote specifics if they exist (even if just “docs/agent/ is empty / not yet populated”).
Also include a quick scan of the codebase and any relevant `/opt/` references (for example `/opt/MacDevDocs` or `/opt/adb`) as part of the mandatory memory check.

## PIO, DMA & Timing Discipline
- PIO changes: always note cycle count impact (use pioasm output).
- DMA: prefer non-blocking enqueue; log potential stalls.
- Sync/geometry/timing changes: **must** update `docs/protocol/` and note in `docs/agent/decisions.md`.
- Test: propose verification with solid fill, checkerboard, or real Mac boot screen capture.

## When Adding/Planning Features (e.g. ADB)
- Check `docs/` for ADB plans (e.g. hootswitch integration).
- ADB: bidirectional open-collector at ~100 kHz; requires careful pull-up + inversion logic + level shift.
- Document new protocol extensions in `docs/protocol/` immediately.

## ADB-Specific Constraints & Gotchas (must recall for PIO RX/TX changes)
- ADB is **open-collector, bidirectional, 5V** — signals require **level shifting** AND **inversion** on TX path.
  - RECV (input): GPIO6 via 74LVC245 (non-inverting buffer, 5V → 3.3V)
  - XMIT (output): GPIO12 via ULN2803 (Darlington open-collector driver, **inverted** logic)
- **Never** connect Mac ADB directly to Pico GPIO — always document/enforce level shifters + drivers.
- PIO for ADB: ~100–125 kHz bit rate, precise timing for Start bit, Sync bit, data bits, Stop bit, Tlt (line turnaround).
  - Use PIO side-set / autopull carefully; measure cycles against real Mac timing.
  - Inversion on TX must be handled either in PIO logic or external hardware — document which.
- Reference implementation: study `/opt/adb/hootswitch` for PIO + DMA patterns on RP2040 ADB.
- Protocol changes (packet format, command handling): **must** update `docs/protocol/` and bump version.
- Testing: use CDC control interface for ASCII commands (e.g. key down/up) → verify with real Mac or ADB sniffer.

## Mandatory Memory Check – Updated for ADB Work
Before any ADB-related proposal:
- Review: agents.md, PROJECT_STATE.md, README.md (pin assignments), docs/protocol/ (if ADB sections added), the relevant code paths, and /opt/adb references.
- If repo docs conflict with /opt/adb (hootswitch or other ADB sources), treat /opt/adb as the overriding source and call out the discrepancy.
- Quote relevant pin / level-shift / inversion notes.
- Start response with:
  “Reviewed agents.md + README pinout for ADB (GPIO6 RECV via 74LVC245 non-inverting, GPIO12 XMIT via ULN2803 inverted). Relevant constraints: …”

## PIO Discipline for ADB RX/TX Skeletons
- New PIO programs (e.g. adb_rx.pio, adb_tx.pio): always note cycle counts and edge sensitivity (rising/falling for sync/start bits).
- Handle line turnaround (Tlt ~200–400 µs): use PIO IRQ + timer or state machine delay.
- DMA for ADB: prefer small ring buffer for RX packets; avoid blocking on TX.
- Any pin remap, polarity flip, or timing tweak → **mandatory** update to:
  - `docs/agent/decisions.md` (why chosen)
  - `docs/agent/notes.md` (measured quirk)
  - `docs/protocol/` (if protocol-visible)
- Propose tests: build/flash → send test key via minicom/picocom on CDC1 → observe Mac response (or scope ADB line).

## When Starting docs/agent/ Population
- Create/populate on first non-trivial ADB change:
  - `docs/agent/log.md`: chronological actions (use template with [YYYY-MM-DD])
  - `docs/agent/notes.md`: quirks (e.g. "ADB TX requires inversion due to ULN2803")
  - `docs/agent/decisions.md`: choices (e.g. "PIO handles inversion vs external")
- After each skeleton → self-reflect paragraph in log.md:
  - What ADB timing/PIO interaction learned?
  - What nearly failed (e.g. missing pull-up, wrong edge)?
  - Prevention for next iteration?
