# CYD-and-Freenove-Fish-Tank

Animated fake aquarium for the Freenove FNK0104S (ESP32-S3, 4.0" ST7796 480x320, FT6336U touch). A smaller CYD (320x240) version is planned.

**Flasher:** https://grey-lancaster.github.io/CYD-and-Freenove-Fish-Tank/ (Chrome/Edge; put the board in bootloader mode: hold BOOT, tap RESET, release BOOT; press RESET after flashing).

## What it does
Blue tang, clownfish, angelfish, goldfish and three neon tetras wander the tank. Bubbles rise, kelp sways, light rays drift. **Touch the screen to drop fish food** - fish swim over and eat it. A clock sits top-right (NTP if `src/secrets.h` exists, otherwise firmware build time + uptime).

## Layout
- `src/main.cpp` - firmware. Full-frame RGB565 sprite in PSRAM, static background sprite memcpy'd each frame, pushed to the display.
- `platformio.ini`, `boards/`, `lib_freenove/` - build config carried over from Feenove-104S.
- `docs/` - browser flasher (GitHub Pages) + `firmware/aquarium.bin` (merged image, flash at 0x0, DIO).
- `design/` - PC-side Python reference render (`fish2.py`, `angel.py`) and `tank4.mp4` preview.

## Build
```
pio run
python3 ~/.platformio/packages/tool-esptoolpy/esptool.py --chip esp32s3 merge_bin -o docs/firmware/aquarium.bin \
  --flash_mode dio --flash_freq keep --flash_size keep \
  0x0 .pio/build/esp32s3/bootloader.bin 0x8000 .pio/build/esp32s3/partitions.bin \
  0xe000 <framework>/tools/partitions/boot_app0.bin 0x10000 .pio/build/esp32s3/firmware.bin
```
Check byte 2 of the merged image is `0x02` (DIO; this board's octal PSRAM needs it).

## Features (v1.1.0)
Two boards, one codebase: **Freenove FNK0104S** (`pio run -e freenove4`) and **CYD 2.8"** (`pio run -e cyd28`).
- Tap-to-feed (sonar ring + ding ding), auto feed, looping bubble sound, clock + date (12/24 h), BOOT info screen.
- **WiFi is set up on the screen** with [TouchWifiProvisioner](https://github.com/Grey-Lancaster/TouchWifiProvisioner) (no credentials in the firmware). BOOT during setup = run offline.
- Web settings page at `http://fishtank.local` (CYD: `cydfishtank.local`): sound, volume, rotation, **invert colours**, **swap red/blue**, auto feed, 12/24 h, hostname, screenshots, forget WiFi, recalibrate touch (CYD), WiFi **motion sleep** (ported from CYD PhotoFrame).
- OTA updates: `espota.py -i <ip> -p 3232 -f .pio/build/<env>/firmware.bin -r` (hostname = the board's hostname).

## Rendering
The world is a 480x320 virtual space. The Freenove renders a full frame in PSRAM; the CYD has no PSRAM, so it draws and pushes the tank in 30-row strips. `src/hal_*.cpp` hold the per-board display, touch and audio code (Freenove: ES8311 codec; CYD: internal DAC on GPIO26 and bit-banged XPT2046 touch with a five-point calibration).

## Status
Freenove runs at about 10 fps. The CYD port compiles and is untested on hardware as of this commit.
