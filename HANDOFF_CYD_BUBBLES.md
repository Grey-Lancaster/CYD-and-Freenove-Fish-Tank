# Handoff: make the Fish Tank bubbles sound right on the 1-USB CYD

Written 2026-10-30 for a fresh Claude (Opus) session. Grey is the owner and the only listener. Nobody but Grey can
judge the sound, so every change has to be flashed and heard by ear. Do not say a sound "works" until Grey says so.

## The goal
The CYD (ESP32-2432S028, ILI9341, one USB port, `pio` env `cyd28`) should play a continuous, pleasant bubbling
ambience, like the Freenove boards do. Today it does not. Grey hears hiss, "squishy", pops that repeat, or a
"coffee commercial" drip. Everything else on that board sounds fine (feed ding, diver k-ching, alarm, a test tune).

## What is already proven (do not re-investigate)
- **The playback path is good.** The sibling repo Grey-Lancaster/CYD-and-Freenove-Deer-and-Turkey-display uses the
  same code (44.1 kHz I2S, noise-shaped 8-bit rounding, 16 x 256 DMA buffers, task on core 1). Its woods ambience
  sounds "perfect, same as the Freenove boards" on this same CYD. Its recipe is `design/make_ambience.py` there.
- **It is not the speaker.** Grey tried a different speaker: no change.
- **It is the content.** Spectrum of each sound (share of energy):

  | Sound | below 300 Hz | 300 Hz - 2 kHz | above 2 kHz | result on CYD |
  |---|---|---|---|---|
  | Deer-and-Turkey woods ambience | 0% | 3% | 97% | sounds right |
  | `src/bubbles.raw` (Freenove bubbles) | 2% | 97% | 0% | fine on Freenove (real speaker) |
  | Grey's `bubbles.wav` as is | 76% | 24% | 0% | chop, hiss |
  | processed bubbles (denoise, 80% peak) | 10% | 90% | 0% | "squishy" |

  The CYD speaker plays almost nothing below about 1 kHz. Every bubble recording is below 1 kHz. Normalising, filtering,
  cutting the bass and boosting do not help: the speaker cannot reproduce that range. Chart: `design/compare_spectrograms.png`.
- Things that did NOT fix it: gain 1 vs 4, a bass cut at 350 Hz, spectral-gate denoise, sample-rate 16 k vs 44.1 k
  (44.1 k helped a lot, keep it), core 0 vs 1, deeper DMA, touch not polled (IRQ-gated), loop-point crossfade (that
  did fix a click every 6 s), a 3x speed-up of the recording (sounded "kinda like bubbles" but the pop repeats; a
  10 s loop of the 31 s source did not change that).
- Synthesised bubbles in `src/audio.cpp` (`bubblesSynth`, mode 2, current default): first version was "too chirpy"
  then "a coffee commercial"; a messier, size-varied version (random radius sets pitch and ring time, noise splash at
  the onset, bursts then pauses) is on the board and Grey has **not reported on it**. Treat it as untested.
- The hiss exists in PhotoFrame on the same board too, so some of it is board hardware (8-bit DAC + amp).
- Grey does NOT hear the pops in PhotoFrame playing the same WAV, but a freeze-the-screen test (no redraws) showed no
  difference, so the pops are probably the content, not the display.

## Ideas worth trying (none done yet)
1. **Find or make a bubble sound that lives above ~1.5 kHz.** Real small bubbles ring at 1-4 kHz. A recording of
   fizz/small bubbles (carbonated water, fish tank aerator close up) may be bright enough. Check the spectrogram
   first (script in the history: share of energy per band), the target is the Deer-and-Turkey profile, mostly above 2 kHz.
2. **Resynthesise the existing recording brighter**: pitch-shift up 1-2 octaves while keeping the duration (phase
   vocoder / PSOLA), so it stays long (no repeating pop) and lands in the speaker's range. A plain 3x speed-up got the
   pitch but cut the loop to 1.8 s (10 s from the long source), and Grey could hear it repeat.
3. **A longer loop at low cost**: Grey's "if I hit the lottery we might try opus" means compressed audio so a long,
   bright loop fits the flash. The CYD build is 75% full (`cyd28`), 1.97 MB app partition; WAVs are stored 16-bit at
   11.025 kHz and upsampled x4. A 25 s loop at 11.025 kHz is 551 KB (what Deer-and-Turkey ships). Opus/ADPCM decoding
   needs a decoder library on an ESP32 with no PSRAM; ESP8266Audio is already used by PhotoFrame (WAV and MP3).
4. **Better synthesis** if recordings fail: add a soft filtered-noise "water" bed above 2 kHz, vary burst structure.
   Tune by Grey's words: chirpy = too much pitch rise/too high; coffee commercial = too regular, too tonal.

## Where everything is
- Repo: github.com/Grey-Lancaster/CYD-and-Freenove-Fish-Tank, local `/home/grey/Documents/Feenove-Aquarium` on host
  **greyhound** (builds happen there). `ci` and `main` are at commit `aeb421c`. Release `v1.7.0` (tag, built) has the
  earlier as-is `bubbles.wav` build, not the later work.
- `src/audio.cpp`: CYD ambience is the separate stream `audioBubbles44()` (44.1 kHz). Mode `g_bubMode`: 2 = synth
  (default), 1 = recorded loop `src/bubbles_cyd.wav`, 0 = off. `g_bubGain` playback gain. Effects (feed, diver, alarm,
  `/api/lamb` test tune) still go through the 16 kHz `audioMix()` and are upsampled in `src/hal_cyd28.cpp` `audioTask`.
- `design/make_bubbles.py`: builds `src/bubbles_cyd.wav` from `src/bubbles_cyd_source.wav` (Grey's 31 s recording;
  denoise, `SPEED = 3`, crossfaded loop, peak 0.66). Needs numpy; there is a venv at the session scratch path with
  `av numpy scipy matplotlib` (recreate with `python3 -m venv v && v/bin/pip install av numpy scipy matplotlib`).
- Other files from Grey: `/home/grey/Documents/freesound_community-indoor-nice-fish-tank-ambiance-75601.mp3` (the
  same 31 s recording), `design/compare_spectrograms.png`. Grey's uploads are under `~/.claude/uploads/`.
- Web test routes on the CYD (`http://cydfishtank.local/`): `/api/bub?m=0|1|2&g=1..16`, `/api/lamb` (test tune),
  `/api/set?vol=0..100&snd=1`, `/api/status`, per-sound switches `sbub`, `sdiv`, `sfeed` on the settings page.
- A sound is judged at volume 60, gain 1.

## How to build and flash (rules from Grey)
- Build on greyhound: `cd /home/grey/Documents/Feenove-Aquarium && pio run -e cyd28` (building one env cleans the
  others' `.pio`; rebuild the one you flash). Flash fish tank over the air:
  `python3 ~/.platformio/packages/framework-arduinoespressif32/tools/espota.py -i cydfishtank.local -p 3232 -f .pio/build/cyd28/firmware.bin -r`
  then confirm `curl http://cydfishtank.local/api/status` shows a small `up` (uptime resets). The bare name
  `fishtank` fails; use `.local`. OTA sometimes fails: retry once and check uptime before retrying again.
- If the board is running PhotoFrame instead: `photoframe.local`, push with
  `curl "http://photoframe.local/ota/start?mode=fr"` then `curl -F "file=@firmware.bin" http://photoframe.local/ota/upload`.
  Put PhotoFrame back with espota to `cydfishtank.local` using
  `/home/grey/Documents/CYD-PhotoFrame/.pio/build/cyd/firmware.bin`. The board's IP changes between 192.168.1.223 and .232.
- **Flashing replaces whatever is on the board: ask Grey first.** Ask before tagging a release (tags publish).
- Commit trailer: `Co-Authored-By: Claude Opus <noreply@anthropic.com>` (use your own model name). Pushes go to `ci`
  then `git push origin ci:main` (fast-forward). Grey said commits are fine when he asks.
- Do not claim hardware behaviour was verified unless observed. Grey is tired of repeated flashes: batch ideas and
  say plainly what changed and what to listen for.
