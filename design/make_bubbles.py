# Generates a seamless ~10 s aquarium bubbling loop: design/bubbles.wav (16 kHz mono)
# and src/bubbles.raw (same samples, int16 LE) which the firmware embeds and loops.
import math, random, struct, wave
SR = 16000; N = SR * 10
random.seed(11)
buf = [0.0] * N
# quiet water rumble: low-passed noise, made loop-safe with a crossfade
raw = [random.uniform(-1, 1) for _ in range(N + 4000)]
lp = 0.0; sm = []
for v in raw:
    lp += (v - lp) * 0.012; sm.append(lp)
for i in range(N):
    v = sm[i]
    if i < 4000:
        w = i / 4000.0
        v = sm[i] * w + sm[N + i] * (1 - w)
    buf[i] = v * 0.25
def bloop(t0, f0, dur, amp):
    n = int(dur * SR); ph = 0.0
    for k in range(n):
        t = k / SR
        f = f0 * (1 + 3.5 * t)
        ph += 2 * math.pi * f / SR
        env = min(1.0, k / (0.004 * SR)) * math.exp(-t / (dur / 3.2))
        buf[(t0 + k) % N] += amp * env * math.sin(ph)
for _ in range(34):
    t0 = random.randrange(N)
    f0 = random.uniform(320, 1100); dur = random.uniform(0.05, 0.16); amp = random.uniform(0.25, 0.6)
    bloop(t0, f0, dur, amp)
    if random.random() < 0.35:           # small cluster of follow-up bubbles
        for _ in range(random.randint(1, 3)):
            bloop(t0 + int(random.uniform(0.03, 0.2) * SR), f0 * random.uniform(0.8, 1.5), dur * 0.7, amp * 0.5)
peak = max(abs(v) for v in buf)
pcm = [int(v / peak * 8400) for v in buf]
with wave.open("design/bubbles.wav", "wb") as w:
    w.setnchannels(1); w.setsampwidth(2); w.setframerate(SR)
    w.writeframes(struct.pack("<%dh" % N, *pcm))
open("src/bubbles.raw", "wb").write(struct.pack("<%dh" % N, *pcm))
print("ok", N, "samples")
