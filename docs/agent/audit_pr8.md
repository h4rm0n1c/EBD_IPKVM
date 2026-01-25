# Audit notes: missing scripts and CDC helper

## Findings
- The upstream GitHub repo main branch does not contain the `scripts/` directory or the `cdc_cmd.py` helper.
- The `scripts/` directory (with `ab_capture.py` and `cdc_cmd.py`) exists on non-main branches, including:
  - `origin/codex/troubleshoot-no-output-from-serial-communication` (commit `7038af9`: adds `scripts/cdc_cmd.py`).
  - `origin/codex/troubleshoot-no-output-from-serial-communication` (commit `55af0f5`: adds `scripts/ab_capture.py`).
- No delete commit for `scripts/` or `cdc_cmd.py` exists on `main`; instead, the helpers appear to have been added on a branch that was never merged.
- `scripts/ab_capture.py` relies on the `O` CDC command to toggle VIDEO inversion, which is not part of the current `main` firmware command list.

## Suggested follow-ups
- Decide whether to merge the firmware changes that introduced the `O` command before advertising `ab_capture.py` as a standard helper.
- If PR #8 was intended to merge the troubleshooting branch, rebase or cherry-pick commits `55af0f5` and `7038af9` onto `main` and resolve any conflicts.
