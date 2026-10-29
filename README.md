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

## Features (v1.5.2)
Four builds, one codebase: **Freenove FNK0104S 4.0"** (`pio run -e freenove4`), **Freenove FNK0104B 2.8"** (`pio run -e freenove28`), **CYD 2.8"** with ILI9341 (`pio run -e cyd28`) and the **2-USB CYD 2.8"** with ST7789 (`pio run -e cyd28usb2`). Wrong colours on a CYD usually means the other CYD build.
- Tap-to-feed (sonar ring + ding ding), auto feed, looping bubble sound, clock + date (12/24 h), BOOT info screen.
- **WiFi is set up on the screen** with [TouchWifiProvisioner](https://github.com/Grey-Lancaster/TouchWifiProvisioner) (no credentials in the firmware). BOOT during setup = run offline.
- Web settings page at `http://fishtank.local` (CYD: `cydfishtank.local`): sound, volume, rotation, **invert colours**, **swap red/blue**, auto feed, 12/24 h, hostname, screenshots, forget WiFi, recalibrate touch (CYD), WiFi **motion sleep** (ported from CYD PhotoFrame).
- **Firmware update page** at `/update` ([ElegantOTA](https://github.com/ayushsharma82/ElegantOTA), linked from the tank's web page; choose `Fish-Tank-<build>-update.bin`; keeps WiFi and settings). The factory `.bin` (and the web installer) wipe the board including saved WiFi.
- **Night mode**: screen and sound off between two times (hour/minute dropdowns); touch or BOOT lights it for a minute. Works with motion sleep (night wins).
- **Date & time card**: time zone, date format (5 styles), 12/24 h, and "Set time from this device" for boards without internet; the last time is saved and restored after a restart.
- **Treasure chest** that opens every so often and fills the tank with bubbles (button on the web page).
- **Scuba diver**: every sixth time the chest opens he swims in from one side, takes a bag of gold (the chest goes empty), and swims out the other, with a cash-register "k-ching" as he goes. Tap the chest on the screen to open it; "Send the diver" on the web page calls him right away.
- **Alarm clock**: tap the time on the screen to set it (or use the web page: days, snooze, loudness). A minute before it rings the fish turn upside down and a sunrise glow builds; then the fish circle a big clock until you tap Stop or Snooze.
- **Sound switches**: bubbles, the diver's k-ching and the feeding ding can each be turned off on the web page. On the CYD the bubbles are a separate 44.1 kHz recording (`src/bubbles_cyd.wav`) because its 8-bit speaker output chirps at 16 kHz.
- OTA updates: `espota.py -i <ip> -p 3232 -f .pio/build/<env>/firmware.bin -r` (hostname = the board's hostname).

## Rendering
The world is a 480x320 virtual space. The Freenove renders a full frame in PSRAM; the CYD has no PSRAM, so it draws and pushes the tank in 30-row strips. `src/hal_*.cpp` hold the per-board display, touch and audio code (Freenove: ES8311 codec; CYD: internal DAC on GPIO26 and bit-banged XPT2046 touch with a five-point calibration).

## Status
Freenove and the 2-USB CYD (ST7789) run at about 10-11 fps. The 1-USB ILI9341 CYD build compiles but has not been tried on hardware.
