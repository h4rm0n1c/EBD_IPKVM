# MacFriends Arduino core

This directory is a **verbatim snapshot** of the `Arduino/` folder from the local `/opt/adb/macfriends` repository (MacFriends: Universal Control for your old Macintosh Classic). It is **the primary firmware we use** for the external ATmega328p that handles ADB on this project, not just a reference.

## Source
- Local source: `/opt/adb/macfriends/Arduino`
- Project name: **MacFriends** (see `/opt/adb/macfriends/Readme.md`)

## Build + flash (PlatformIO with pipx / PEP 668 friendly)

The simplest Devuan/Debian-friendly way to install PlatformIO is with `pipx`, which
keeps it isolated from the system Python environment.

### Install PlatformIO with pipx

```bash
sudo apt-get update
sudo apt-get install -y pipx python3-full
pipx install platformio
~/.local/bin/platformio --version
```

If `~/.local/bin` is not on your `PATH`, add it:

```bash
export PATH="$HOME/.local/bin:$PATH"
```

### Build the firmware

```bash
cd Arduino
~/.local/bin/platformio run -e uno
# or
~/.local/bin/platformio run -e nano
```

### Flash to the board

```bash
cd Arduino
~/.local/bin/platformio run -e uno -t upload
# or
~/.local/bin/platformio run -e nano -t upload
```

### Serial monitor

```bash
~/.local/bin/platformio device monitor -b 115200
```

### Notes

- The `uno` environment targets an ATmega328p @ 16 MHz and is compatible with a
  Duemilanove for building; some Duemilanove boards use a 57600 baud bootloader,
  so adjust `upload_speed` in `platformio.ini` if uploads fail.
- PlatformIO will fetch the AVR toolchain and the `TimerOne` dependency automatically.

## Credits / upstream references
The upstream MacFriends Arduino code credits or derives from the following projects:
- **ADBuino**: https://github.com/akuker/adbuino
- **tmk_keyboard** (ADB code reference): https://github.com/tmk/tmk_keyboard

## License
No explicit license was found in the provided snapshot. If an upstream license is identified later, update this file accordingly.
