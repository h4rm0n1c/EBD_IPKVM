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

## Documentation usage
- Use `docs/` as the source of truth for protocol/behavior decisions.
- If instructions conflict, update the documentation and note the change in `docs/agent/log.md`.
- Capture new workflow rules, tooling quirks, and debugging discoveries in `docs/agent/notes.md` as we go.
- When behavior changes, update the relevant `docs/protocol/` page and add a brief entry to `docs/agent/log.md`.

## Safety
- Assume upstream signals may be 5V TTL.
- Never advise connecting 5V signals directly to Pico GPIO.
