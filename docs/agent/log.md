# Log (running)

- 2026-01-26: Added ATX soft-power, BOOTSEL, and watchdog reset CDC commands; documented GPIO9 PS_ON output.
- 2026-01-26: Defaulted host test helper to send ATX power-on before capture, wait with diagnostics, and request ATX shutdown on exit.
- 2026-01-25: Initial repo scaffolding (README, agents.md, docs/*, .gitignore).
- 2026-01-25: Documented availability of `/opt/MacDevDocs` as a local reference source.
- 2026-01-24: Expanded documentation with current capture behavior, pin map, and USB CDC packet format for host tooling.
- 2026-01-24: Clarified that `src/host_recv_frames.py` is the host-side test program.
- 2026-01-26: Reset counters before arming capture in `host_recv_frames.py` to avoid stale 100-frame runs.
- 2026-01-26: Added a short boot wait after opening CDC in `host_recv_frames.py`, configurable via `--boot-wait`.
- 2026-01-26: Added `--diag-secs` to `host_recv_frames.py` for printing ASCII status before arming capture.
- 2026-01-26: Allow status output to continue after 100-frame completion and avoid parking the main loop.
- 2026-01-26: Stop capture on host exit and pre-stop before arming in `host_recv_frames.py`.
- 2026-01-26: Stop re-arming DMA after a capture window completes to avoid stale DMA activity between runs.
