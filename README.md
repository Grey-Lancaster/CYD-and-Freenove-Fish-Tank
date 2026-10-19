# Feenove-Aquarium

Animated fake aquarium for the Freenove FNK0104S (ESP32-S3, 4.0" ST7796 480x320, touch).

- `design/` - PC-side Python (Pillow) reference render: `fish2.py` (tank + fish), `angel.py` (angelfish), `tank4.mp4` preview.
- Firmware: PlatformIO project (TFT_eSPI, PSRAM full-frame sprite), built on greyhound, flashed from boron via the browser flasher (same workflow as Feenove-104S).
- Features: swimming fish, bubbles, swaying kelp, light rays, touch to drop food, clock overlay.
