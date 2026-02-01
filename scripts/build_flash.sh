#!/usr/bin/env bash
set -euo pipefail

git pull

rm -rf ~/pico/EBD_IPKVM/build/*

pushd ~/pico/EBD_IPKVM/build
cmake -G Ninja -DPICO_SDK_PATH="$HOME/pico/pico-sdk" ..
ninja
popd

# just flash
python3 scripts/cdc_cmd.py --cmd B --no-read --no-setup

sleep 6

sudo mkdir -p /mnt/RPI-RP2
sudo mount /dev/sda1 /mnt/RPI-RP2
sudo cp -v ~/pico/EBD_IPKVM/build/EBD_IPKVM.uf2 /mnt/RPI-RP2/
sync
sudo umount /mnt/RPI-RP2
