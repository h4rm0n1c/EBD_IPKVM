# Decisions (running)

- 2026-01-25: Repository initialised with docs-first workflow and agent documentation scaffold.
- 2026-01-26: Use GPIO9 (via ULN2803) as an active-high ATX PS_ON output; add CDC commands for PS_ON control, BOOTSEL, and watchdog reset.
- 2026-01-26: Add a CDC GPIO diagnostic command that samples pin states and edge counts to debug signal presence without a scope.
