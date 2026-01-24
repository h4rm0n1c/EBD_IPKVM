# EBD_IPKVM - Everything But Disks, IP KVM

Work-in-progress “IP KVM” for a Macintosh Classic using an RP2040 (Pico W):
tap raw 1-bpp video + sync, capture with PIO+DMA, and stream to a host UI.

## Repo layout
- `src/` firmware sources (Pico SDK)
- `docs/` documentation
  - `docs/agent/` running notes + decisions for Codex/agents

## Build (typical Pico SDK)
Set your Pico SDK path, then build out-of-tree in `build/`.

