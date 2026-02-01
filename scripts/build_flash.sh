#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/.." && pwd)"

pushd "${repo_root}"
git pull
popd

rm -rf "${repo_root}/build"/*

pushd "${repo_root}/build"
cmake -G Ninja -DPICO_SDK_PATH="$HOME/pico/pico-sdk" ..
ninja
popd

# just flash
python3 "${repo_root}/scripts/cdc_cmd.py" --cmd B --no-read --no-setup

sleep 6

sudo mkdir -p /mnt/RPI-RP2
sudo mount /dev/sda1 /mnt/RPI-RP2
sudo cp -v "${repo_root}/build/EBD_IPKVM.uf2" /mnt/RPI-RP2/
sync
sudo umount /mnt/RPI-RP2
