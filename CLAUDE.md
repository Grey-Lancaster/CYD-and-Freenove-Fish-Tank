# CLAUDE.md

Animated fake aquarium firmware (PlatformIO / Arduino / ESP32) for four touchscreen boards, one codebase. Public repo: Grey-Lancaster/CYD-and-Freenove-Fish-Tank. Read `README.md` first; it is current and has the feature list.

## Boards = PlatformIO envs
| Env | Board |
|---|---|
| `freenove4` (default) | Freenove FNK0104S 4.0", ST7796 480x320, ESP32-S3 N16R8 |
| `freenove28` | Freenove FNK0104B/AB 2.8", ILI9341 |
| `cyd28` | CYD 2.8", ILI9341, one USB port |
| `cyd28usb2` | CYD 2.8", ST7789, two USB ports |

Wrong colours on a CYD almost always means the wrong env (driver), not the invert/swap-RGB toggles.

## Code map
- `src/main.cpp` - scene, fish, bubbles, feeding, web settings page. Draws into a 480x320 virtual world.
- `src/hal_freenove.cpp`, `src/hal_cyd28.cpp` - per-board display, touch, audio. Freenove renders a full-frame sprite in PSRAM; CYD has no PSRAM and renders in 30-row strips.
- `src/wifi_setup.*` - on-screen WiFi setup via TouchWifiProvisioner (no credentials in firmware).
- `src/motion.*` - WiFi motion sleep. `src/audio.*`, `src/es8311.*` - sound.
- `docs/` - GitHub Pages web flasher; `docs/firmware/*.bin` are merged images served to it.
- `dist/` - release binaries: `Fish-Tank-<env>-factory.bin` (wipes board incl. WiFi) and `-update.bin` (OTA, keeps settings).
- `design/` - PC-side Python art/sound generators and preview. Not part of the firmware build.

## Gotchas
- Merged flash images must be DIO mode (byte 2 of the file is `0x02`); the N16R8 octal PSRAM needs it. Use `esptool merge_bin` with `--flash_mode dio` (command is in the README).
- Building one env cleans the other's `.pio` dir. Rebuild the env you intend to flash/OTA right before doing it.
- OTA: `espota.py -i <ip-or-hostname> -p 3232 -f .pio/build/<env>/firmware.bin -r`. Hostnames: `fishtank`, `fishtank28`, `cydfishtank`. WiFi OTA can fail intermittently; retry.
- Releases: tag `vX.Y.Z`, title `Fish Tank vX.Y.Z`, attach factory + update bins for every env. Ask before publishing.

## Owner's workflow
Grey builds on the "greyhound" server (cached toolchains), not on the laptop, and flashes over USB from a browser on the PC the board is plugged into. A session that cannot reach those machines should say so rather than guess. Don't claim hardware behaviour was verified unless it was observed on a board.
