#!/usr/bin/env bash
set -euo pipefail
set +H

# Codex reference corpus installer
#
# Goals:
# - Fetch/unpack reference corpora into /opt/*
# - For git resources: pull files ONLY (no history) by doing a shallow clone then deleting .git
# - Idempotent via .installed.ok stamps
# - For integrity-sensitive artifacts: verify .sha256 if provided

# ---------- packages ----------
sudo apt-get update
sudo apt-get install -y --no-install-recommends \
  ca-certificates curl git unzip tar zstd poppler-utils

# ---------- helpers ----------

install_tar_zst_dir() {
  local dest_dir="$1"
  local url="$2"

  local arc_name arc_path stamp tmpdir top_count top_entry
  arc_name="$(basename "$url")"
  arc_path="/tmp/${arc_name}"
  stamp="$dest_dir/.installed.ok"
  tmpdir="/tmp/tarzst.$$"

  mkdir -p "$dest_dir"

  if [ -e "$stamp" ]; then
    echo "[setup] Already installed: $dest_dir"
    return 0
  fi

  echo "[setup] Downloading $(basename "$dest_dir")"
  curl -fL --retry 3 --retry-delay 2 -o "$arc_path" "$url"

  rm -rf "$tmpdir"
  mkdir -p "$tmpdir"

  echo "[setup] Extracting .tar.zst into staging: $tmpdir"
  tar --use-compress-program=zstd -xf "$arc_path" -C "$tmpdir"

  top_count="$(find "$tmpdir" -mindepth 1 -maxdepth 1 -print | wc -l | tr -d ' ')"
  if [ "$top_count" = "1" ]; then
    top_entry="$(find "$tmpdir" -mindepth 1 -maxdepth 1 -print)"
    if [ -d "$top_entry" ]; then
      echo "[setup] Installing contents of $(basename "$top_entry") -> $dest_dir"
      cp -a "$top_entry"/. "$dest_dir"/
    else
      echo "[setup] Installing single file -> $dest_dir"
      cp -a "$top_entry" "$dest_dir"/
    fi
  else
    echo "[setup] Installing multiple top-level entries -> $dest_dir"
    cp -a "$tmpdir"/. "$dest_dir"/
  fi

  rm -rf "$tmpdir" || true
  rm -f "$arc_path" || true

  date -u +"%Y-%m-%dT%H:%M:%SZ" > "$stamp"
  echo "[setup] Installed: $dest_dir"
}

install_git_snapshot_dir() {
  # Fetch a repo as FILES ONLY (no version history):
  # - shallow clone depth=1, single branch, no tags
  # - then REMOVE .git and install into dest_dir
  #
  # Args:
  #   dest_dir  repo_url  [ref]
  # Where ref may be a branch name, tag, or commit-ish (default: HEAD of default branch)
  local dest_dir="$1"
  local repo_url="$2"
  local ref="${3:-}"

  local stamp tmpdir
  stamp="$dest_dir/.installed.ok"
  tmpdir="/tmp/gitsnap.$$"

  mkdir -p "$dest_dir"

  if [ -e "$stamp" ]; then
    echo "[setup] Already installed: $dest_dir"
    return 0
  fi

  rm -rf "$tmpdir"
  mkdir -p "$tmpdir"

  echo "[setup] Snapshotting repo (no history): $(basename "$dest_dir")"
  # Clone into tmpdir/repo
  if [[ -n "$ref" ]]; then
    git clone --depth 1 --single-branch --no-tags --branch "$ref" "$repo_url" "$tmpdir/repo"
  else
    git clone --depth 1 --single-branch --no-tags "$repo_url" "$tmpdir/repo"
  fi

  # Strip git metadata (this is the key “no version data” requirement)
  rm -rf "$tmpdir/repo/.git"

  echo "[setup] Installing snapshot -> $dest_dir"
  cp -a "$tmpdir/repo"/. "$dest_dir"/

  rm -rf "$tmpdir" || true

  date -u +"%Y-%m-%dT%H:%M:%SZ" > "$stamp"
  echo "[setup] Installed: $dest_dir"
}

install_tgz_dir() {
  local dest_dir="$1"
  local url="$2"

  local tgz_name tgz_path stamp tmpdir top_count top_entry
  tgz_name="$(basename "$url")"
  tgz_path="/tmp/${tgz_name}"
  stamp="$dest_dir/.installed.ok"
  tmpdir="/tmp/tar.$$"

  mkdir -p "$dest_dir"

  if [ -e "$stamp" ]; then
    echo "[setup] Already installed: $dest_dir"
    return 0
  fi

  echo "[setup] Downloading $(basename "$dest_dir")"
  curl -fL --retry 3 --retry-delay 2 -o "$tgz_path" "$url"

  rm -rf "$tmpdir"
  mkdir -p "$tmpdir"

  echo "[setup] Extracting into staging: $tmpdir"
  tar -xzf "$tgz_path" -C "$tmpdir"

  top_count="$(find "$tmpdir" -mindepth 1 -maxdepth 1 -print | wc -l | tr -d ' ')"
  if [ "$top_count" = "1" ]; then
    top_entry="$(find "$tmpdir" -mindepth 1 -maxdepth 1 -print)"
    if [ -d "$top_entry" ]; then
      echo "[setup] Installing contents of $(basename "$top_entry") -> $dest_dir"
      cp -a "$top_entry"/. "$dest_dir"/
    else
      echo "[setup] Installing single file -> $dest_dir"
      cp -a "$top_entry" "$dest_dir"/
    fi
  else
    echo "[setup] Installing multiple top-level entries -> $dest_dir"
    cp -a "$tmpdir"/. "$dest_dir"/
  fi

  rm -rf "$tmpdir" || true
  rm -f "$tgz_path" || true

  date -u +"%Y-%m-%dT%H:%M:%SZ" > "$stamp"
  echo "[setup] Installed: $dest_dir"
}

install_curl_files_dir() {
  # Download one or more URLs into a directory (files only).
  # - Skips files that already exist
  # - Strips URL fragments (#...) and queries (?...) for filename selection
  # - Writes .installed.ok when all requested files exist
  #
  # Args:
  #   dest_dir  url1  [url2 ...]
  local dest_dir="$1"
  shift

  local stamp
  stamp="$dest_dir/.installed.ok"

  mkdir -p "$dest_dir"

  local missing=0
  local url url_no_frag url_no_q fname out tmpout

  for url in "$@"; do
    url_no_frag="${url%%#*}"
    url_no_q="${url_no_frag%%\?*}"
    fname="$(basename "$url_no_q")"

    if [[ -z "$fname" || "$fname" == "/" || "$fname" == "." ]]; then
      echo "[setup] ERROR: Could not derive filename from URL: $url"
      exit 1
    fi

    out="$dest_dir/$fname"

    if [ -e "$out" ]; then
      echo "[setup] Already present: $out"
      continue
    fi

    echo "[setup] Downloading -> $out"
    tmpout="${out}.part.$$"
    rm -f "$tmpout" || true
    curl -fL --retry 3 --retry-delay 2 -o "$tmpout" "$url"
    mv -f "$tmpout" "$out"
    missing=1
  done

  # If everything exists now, stamp it.
  local all_ok=1
  for url in "$@"; do
    url_no_frag="${url%%#*}"
    url_no_q="${url_no_frag%%\?*}"
    fname="$(basename "$url_no_q")"
    if [ ! -e "$dest_dir/$fname" ]; then
      all_ok=0
      break
    fi
  done

  if [ "$all_ok" -eq 1 ]; then
    date -u +"%Y-%m-%dT%H:%M:%SZ" > "$stamp"
    echo "[setup] Installed: $dest_dir"
  else
    echo "[setup] WARNING: Not all files were downloaded for: $dest_dir"
    return 1
  fi
}

# ---------- GitHub release blob ----------
REL_BASE="https://github.com/h4rm0n1c/macdevdocandexampleblob/releases/download/thedocblob"

# Classic Mac Dev Docs corpus (ReadableOverlay; reference only)
install_tar_zst_dir "/opt/MacDevDocs" "https://github.com/h4rm0n1c/macdevdocandexampleblob/releases/download/thedocblobupdate1/ReadableOverlay-20251229.tar.zst"

install_git_snapshot_dir "/opt/PicoHTTPServer" "https://github.com/sysprogs/PicoHTTPServer"

install_git_snapshot_dir "/opt/SigrokPico/" "https://github.com/pico-coder/sigrok-pico"

install_git_snapshot_dir "/opt/picovga" "https://github.com/codaris/picovga-cmake"

# ADB stuff
install_git_snapshot_dir "/opt/adb/trabular" "https://github.com/saybur/trabular"

install_curl_files_dir "/opt/adb/miscdocs" \
  "https://developer.apple.com/library/archive/documentation/mac/pdf/Devices/ADB_Manager.pdf" \
  "https://developer.apple.com/legacy/library/technotes/hw/hw_01.html#Extended" \
  "http://www.t-es-t.hu/download/microchip/an591b.pdf"

install_tgz_dir "/opt/Pico-SDK" "https://github.com/raspberrypi/pico-sdk/archive/refs/tags/2.2.0.tar.gz"

echo "[setup] Done."
