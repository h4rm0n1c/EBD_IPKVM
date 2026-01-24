# Log (running)

- 2026-01-25: Initial repo scaffolding (README, agents.md, docs/*, .gitignore).
- 2026-01-25: Documented availability of `/opt/MacDevDocs` as a local reference source.
- 2026-01-24: Expanded documentation with current capture behavior, pin map, and USB CDC packet format for host tooling.
- 2026-01-24: Clarified that `src/host_recv_frames.py` is the host-side test program.
- 2026-01-26: Reset counters before arming capture in `host_recv_frames.py` to avoid stale 100-frame runs.
- 2026-01-26: Added a short boot wait after opening CDC in `host_recv_frames.py`, configurable via `--boot-wait`.
