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

## Status
v0.1.0: compiles; **not yet run on hardware** (frame rate and exact look unverified). Serial prints `fps:` every 5 s.
