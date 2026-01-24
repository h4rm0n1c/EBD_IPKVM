#!/usr/bin/env bash
set -euo pipefail

# EBD_IPKVM repo bootstrap
# Usage:
#   cd ~/pico/EBD_IPKVM
#   chmod +x bootstrap_repo.sh
#   ./bootstrap_repo.sh
#
# Optional env vars:
#   REPO_NAME=EBD_IPKVM
#   OWNER=h4rm0n1c
#   VISIBILITY=public|private|internal   (default: public)
#   DESCRIPTION="..."                    (default set below)
#
# Note: requires GitHub CLI `gh` installed + already logged in.

REPO_NAME="${REPO_NAME:-EBD_IPKVM}"
OWNER="${OWNER:-h4rm0n1c}"
VISIBILITY="${VISIBILITY:-public}"
DESCRIPTION="${DESCRIPTION:-RP2040/Pico W PIO+DMA capture for Macintosh Classic 1bpp video (future ADB/ATX KVM)}"

# Basic sanity: run from project root
if [[ ! -f "CMakeLists.txt" ]]; then
  echo "ERROR: Run this from the project root (where CMakeLists.txt lives)."
  exit 1
fi

# ---- .gitignore
cat > .gitignore <<'EOF'
/build/
/frames/

# Pico/RP2040 build artifacts (belt + suspenders)
*.uf2
*.elf
*.bin
*.hex
*.map
*.dis
*.o
*.a
*.d
*.log

# Editor/OS noise
*~
.*.swp
.*.swo
.DS_Store
.vscode/
.idea/
EOF

# ---- docs skeleton (agent-friendly)
mkdir -p docs/agent docs/hardware docs/protocol

# ---- README.md (no fenced code blocks inside; keep it simple)
cat > README.md <<'EOF'
# EBD_IPKVM

Work-in-progress “IP KVM” for a Macintosh Classic using an RP2040 (Pico W):
tap raw 1-bpp video + sync, capture with PIO+DMA, and stream to a host UI.

## Repo layout
- `src/` firmware sources (Pico SDK)
- `docs/` documentation
  - `docs/agent/` running notes + decisions for Codex/agents

## Build (typical Pico SDK)
Set your Pico SDK path, then build out-of-tree in `build/`.

EOF

# ---- agents.md (Codex/agent workflow rules)
cat > agents.md <<'EOF'
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

## Preferred reporting style
- Cite file paths.
- Cite function names / symbols.
- Provide minimal code context so changes are easy to apply.

## Safety
- Assume upstream signals may be 5V TTL.
- Never advise connecting 5V signals directly to Pico GPIO.
EOF

# ---- Living docs
cat > docs/PROJECT_STATE.md <<'EOF'
# Project state (living)

## Goal
Macintosh Classic KVM:
- Capture raw TTL video signals: PIXCLK + HSYNC + VSYNC + 1bpp VIDEO
- RP2040 PIO+DMA capture → stream to a host/web UI
- Future: ADB keyboard+mouse emulation, ATX soft power, reset/NMI

## Current behavior
Document current firmware behavior here as it stabilises.
EOF

cat > docs/agent/README.md <<'EOF'
# Agent docs

This folder exists so automated assistants (and humans) don’t lose context.

## What goes here
- `decisions.md`: durable design choices (pin maps, protocol, timing, architecture)
- `log.md`: running changelog (date + summary + why)
- `notes.md`: quirks, gotchas, measurements, scope screenshots references, etc.

Rule of thumb: if it would save you 30 minutes next week, write it down now.
EOF

cat > docs/agent/decisions.md <<'EOF'
# Decisions (running)

- 2026-01-25: Repository initialised with docs-first workflow and agent documentation scaffold.
EOF

cat > docs/agent/log.md <<'EOF'
# Log (running)

- 2026-01-25: Initial repo scaffolding (README, agents.md, docs/*, .gitignore).
EOF

touch docs/agent/notes.md

# ---- git init/commit
if ! git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  git init
fi

# Ensure main branch
git branch -M main >/dev/null 2>&1 || true

git add .
if git diff --cached --quiet; then
  echo "No staged changes (nothing to commit)."
else
  git commit -m "Initial scaffolding: docs + agent workflow + ignores"
fi

# ---- create/push GitHub repo
if command -v gh >/dev/null 2>&1; then
  gh auth status >/dev/null 2>&1 || {
    echo "ERROR: gh is installed but not authenticated. Run: gh auth login"
    exit 1
  }

  # Create remote if missing
  if ! git remote get-url origin >/dev/null 2>&1; then
    case "$VISIBILITY" in
      public|private|internal) ;;
      *) echo "ERROR: VISIBILITY must be public|private|internal"; exit 1 ;;
    esac

    gh repo create "${OWNER}/${REPO_NAME}" --"${VISIBILITY}" --source=. --remote=origin --push -d "${DESCRIPTION}"
  else
    git push -u origin main
  fi

  echo
  echo "Done. Repo:"
  gh repo view "${OWNER}/${REPO_NAME}" --web
else
  echo "NOTE: gh not found. Git repo initialised locally, but no GitHub remote created."
fi