# Build report - v0.1.0
- `pio run`: SUCCESS (flash ~397 KB). Merged image `docs/firmware/aquarium.bin` (462,224 bytes), header byte 2 = 0x02 (DIO).
- sha256: d16dec299da8591581adee662c626e9d7a41b783ba5e3e5043f365f9b53efe33
- NOT verified: anything on real hardware - frame rate, colors/orientation, touch mapping, clock. Max SPI throughput suggests ~12-16 fps for a full-frame push at 80 MHz.
- The 10:24 PM unattended systemd run failed immediately (invalid API key in its environment); this build was done interactively instead.
