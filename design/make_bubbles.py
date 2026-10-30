"""Make the CYD bubbles loop (played SPEED times faster) from a recording (default: src/bubbles_cyd_source.wav, mono 16-bit).
  python design/make_bubbles.py [input.wav]
Writes src/bubbles_cyd.wav (11.025 kHz mono 16-bit; the CYD firmware upsamples x4 to 44.1 kHz, its 8-bit DAC chirps at
16 kHz) and design/bubbles_preview.wav (the same thing, to audition on a PC).
Same treatment as the Deer-and-Turkey ambience, which sounds right on the CYD speaker: the steady background is
gated out, everything above 2.3 kHz goes (the speaker mangles it), the loop is blended at its seam and normalised to
80% of full scale, because quiet sound through the 8-bit output is mostly rounding noise.
"""
import os, sys, wave
import numpy as np

XFADE_S, PEAK = 0.25, 0.66
SPEED = 3.0   # play the recording this much faster: the CYD speaker only does well above ~1 kHz, bubbles are mostly below it
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")


def read_mono(path, seconds):
    w = wave.open(path)
    ch, sw, rate = w.getnchannels(), w.getsampwidth(), w.getframerate()
    raw = w.readframes(int(seconds * rate))
    if sw == 3:
        b = np.frombuffer(raw, np.uint8).reshape(-1, 3).astype(np.int32)
        x = (b[:, 0] | (b[:, 1] << 8) | (b[:, 2] << 16))
        x = np.where(x >= 1 << 23, x - (1 << 24), x) / float(1 << 23)
    elif sw == 2:
        x = np.frombuffer(raw, np.int16).astype(np.float64) / 32768.0
    else:
        raise SystemExit("unsupported sample width %d" % sw)
    x = x.reshape(-1, ch).mean(axis=1)
    return x, rate


def resample(x, r0, r1):
    if r0 == r1:
        return x
    n = int(round(len(x) * r1 / r0))
    t = np.arange(n) * (r0 / r1)
    # band-limit first when going down (simple FFT low-pass), then linear interpolation
    if r1 < r0:
        X = np.fft.rfft(x)
        f = np.fft.rfftfreq(len(x), 1.0 / r0)
        X[f > 0.45 * r1] = 0
        x = np.fft.irfft(X, len(x))
    return np.interp(t, np.arange(len(x)), x)


def denoise(x, rate, n=2048, pct=50, over=1.6, floor_db=24, hp=90):
    """Spectral gate: learn the steady background (hum, wind, cricket drone) as a per-frequency level and
    subtract it, so only sounds that stand out of it (crows, gobbles, bird chirps) get through."""
    hop = n // 4
    w = np.hanning(n)
    pad = np.concatenate([np.zeros(n), x, np.zeros(n)])
    starts = range(0, len(pad) - n, hop)
    S = np.array([np.fft.rfft(pad[i:i + n] * w) for i in starts])
    mag = np.abs(S)
    noise = np.percentile(mag, pct, axis=0)
    noise = np.convolve(noise, np.ones(5) / 5, mode="same")               # smooth the profile across frequency
    g = np.clip(1.0 - over * noise / (mag + 1e-12), 10 ** (-floor_db / 20.0), 1.0)
    g = np.maximum.reduce([g, np.roll(g, 1, axis=0) * 0.6, np.roll(g, -1, axis=0) * 0.6])      # let gates linger a frame either side
    box = np.ones(5) / 5                                                                         # smooth the gains over time and frequency: no watery "musical noise"
    g = np.apply_along_axis(np.convolve, 0, g, np.ones(3) / 3, "same")
    g = np.apply_along_axis(np.convolve, 1, g, box, "same")
    f = np.fft.rfftfreq(n, 1.0 / rate)
    g *= np.clip((f - hp * 0.6) / (hp * 0.4), 0, 1)                      # high-pass: nothing below hp/2 .. hp Hz
    out = np.zeros(len(pad))
    for k, i in enumerate(starts):
        out[i:i + n] += np.fft.irfft(S[k] * g[k], n)
    return out[n:n + len(x)] / 2.0                                        # Hann at 75% overlap sums to 2


def loop(x, rate):
    f = int(XFADE_S * rate)
    out = x[:len(x) - f].copy()
    a = np.linspace(0, 1, f)
    out[:f] = x[:f] * a + x[len(x) - f:] * (1 - a)      # blend the end into the start: no click at the seam
    return out


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "src", "bubbles_cyd_source.wav")
    x, rate = read_mono(src, 60)
    x = denoise(x, rate)
    y = resample(x, rate * SPEED, 11025)                  # faster = higher pitch and shorter
    y = loop(y, 11025)
    y = y / max(1e-9, np.abs(y).max()) * PEAK
    pcm = (y * 32767).astype("<i2")
    for path in (os.path.join(ROOT, "src", "bubbles_cyd.wav"), os.path.join(ROOT, "design", "bubbles_preview.wav")):
        with wave.open(path, "wb") as w:
            w.setnchannels(1); w.setsampwidth(2); w.setframerate(11025); w.writeframes(pcm.tobytes())
        print(os.path.basename(path), "%.1f s" % (len(pcm) / 11025), os.path.getsize(path) // 1024, "KB")


if __name__ == "__main__":
    main()
