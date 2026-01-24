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

## Preferred reporting style
- Cite file paths.
- Cite function names / symbols.
- Provide minimal code context so changes are easy to apply.

## Safety
- Assume upstream signals may be 5V TTL.
- Never advise connecting 5V signals directly to Pico GPIO.
