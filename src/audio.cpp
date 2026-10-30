#include "audio.h"
#include "hal.h"

#ifndef BOARD_CYD28     // the CYD synthesises its own 44.1 kHz ambience (see audioBubbles44)
extern const uint8_t bubbles_start[] asm("_binary_src_bubbles_raw_start");
extern const uint8_t bubbles_end[] asm("_binary_src_bubbles_raw_end");
#endif
extern const uint8_t feed_start[] asm("_binary_src_feed_raw_start");
extern const uint8_t feed_end[] asm("_binary_src_feed_raw_end");

static volatile size_t g_fxPos = (size_t)-1;      // feed sound position (-1 = idle)

void audioDing() { if (g_sFeed) g_fxPos = 0; }
// Sine by table lookup (x in cycles): sinf per sample was eating the CYD's core and the DAC output crackled.
static int16_t g_sinT[512];
static float sinC(float x) {
  if (!g_sinT[1]) for (int i = 0; i < 512; i++) g_sinT[i] = (int16_t)(sinf(6.2831853f * i / 512.0f) * 32000.0f);
  x -= (int)x; if (x < 0) x += 1.0f;
  return g_sinT[(int)(x * 512.0f) & 511] * (1.0f / 32000.0f);
}
// Cash register "cha-ching", synthesised: a short noise burst, then a bell with inharmonic partials.
static volatile uint32_t g_cashPos = 0xFFFFFFFF;
void audioCash() { if (g_sDiv) g_cashPos = 0; }
static int32_t cashSample(uint32_t n) {
  const float t = n / 16000.0f;
  if (t > 1.1f) return 0;
  static uint32_t rnd = 12345;
  float v = 0;
  if (t < 0.09f) {                                   // "k": drawer clunk, a low thump plus a short click
    rnd = rnd * 1664525u + 1013904223u;
    const float e = 1.0f - t / 0.09f;
    v += e * e * (0.45f * sinC(140.0f * t) + (((int32_t)(rnd >> 16) - 32768) / 32768.0f) * 0.25f);
  }
  for (int k = 0; k < 2; k++) {                      // "CHING": the bell struck, then again just after for the shimmer
    const float u = t - 0.10f - 0.07f * k;
    if (u <= 0) continue;
    const float e = expf(-u * 4.5f) * (k ? 0.6f : 1.0f);
    v += e * (0.45f * sinC(2349.0f * u) + 0.25f * sinC(3520.0f * u));
  }
  return (int32_t)(v * 18000.0f);
}

static volatile int g_alLevel = 0;
void audioAlarm(int level) { g_alLevel = level < 0 ? 0 : level > 100 ? 100 : level; }
// Speaker test tune: plain sine notes, 0.4 s each.
static volatile uint32_t g_lambPos = 0xFFFFFFFF;
void audioLamb() { g_lambPos = 0; }
static const uint8_t LAMB[] = {4,2,0,2,4,4,4,255,2,2,2,255,4,7,7,255,4,2,0,2,4,4,4,4,2,2,4,2,0};   // scale steps C D E . G
static bool lambDone(uint32_t n) { return n / 6400 >= sizeof(LAMB); }
static int32_t lambSample(uint32_t n) {
  const uint32_t k = n / 6400;                       // note index
  const uint32_t m = n % 6400;
  if (LAMB[k] == 255 || m > 5600) return 0;           // rest / gap between notes
  const float f = LAMB[k] == 4 ? 659 : LAMB[k] == 2 ? 587 : LAMB[k] == 0 ? 523 : 784;
  const float e = min(1.0f, m / 160.0f) * min(1.0f, (5600 - m) / 400.0f);
  return (int32_t)(sinC(f * m / 16000.0f) * e * 24000.0f);
}
bool audioAlarmOn() { return g_alLevel > 0 || g_lambPos != 0xFFFFFFFF; }
// Two rising bell arpeggios every 1.6 s.
static int32_t alarmSample(uint32_t n) {
  const float t = (n % 25600) / 16000.0f;
  static const float F[4] = {1047, 1319, 1568, 2093};
  float v = 0;
  for (int rep = 0; rep < 2; rep++)
    for (int k = 0; k < 4; k++) {
      const float u = t - rep * 0.8f - k * 0.13f;
      if (u <= 0 || u > 0.45f) continue;
      v += expf(-u * 7.0f) * min(1.0f, u / 0.004f) * sinC(F[k] * u);
    }
  return (int32_t)(v * 9000.0f);
}


#ifdef BOARD_CYD28
// The CYD's 8-bit DAC needs the ambience at a high sample rate (like PhotoFrame's 44.1 kHz WAV), so it is a
// separate stream mixed in by the HAL at 44.1 kHz, not part of the 16 kHz mixer. It is synthesised: the CYD speaker
// plays almost nothing below ~1 kHz, and everything tried that lives there sounded bad on it (Grey's aquarium recording
// as is, denoised, sped up, or pitched up 2-3 octaves; the Freenove bubbles file; gliding "bloops" like it at 1-2x its
// pitch). Mode: 0 = off, anything else = on.
static volatile int g_bubMode = 2, g_bubGain = 1;
void audioBubMode(int m, int g) { g_bubMode = m; if (g >= 1 && g <= 16) g_bubGain = g; }
// Synthesised bubbles. Each bubble has a random size: small ones ring high and short, big ones
// lower and longer (a damped sine with a slight upward glide), with a tiny noise splash at the start. They come in
// fast bursts of mostly small bubbles with the occasional big blob, then a pause. No loop, so nothing repeats.
static void bubblesSynth(int16_t *out, int n, bool on) {
  struct Voice { float ph, f, rise, amp, dec, splash, sdec; bool live; };
  static Voice vs[12];
  static uint32_t rnd = 2463534242u, wait = 22050, burst = 0;
  static const float SR = 44100.0f;
  auto rf = [&]() { rnd ^= rnd << 13; rnd ^= rnd >> 17; rnd ^= rnd << 5; return (rnd & 0xFFFF) / 65535.0f; };
  auto spawn = [&]() {
    for (Voice &v : vs) if (!v.live) {
      const float r = 0.8f + 4.2f * powf(rf(), 2.2f);                       // radius in mm: mostly small, some big
      v.f = constrain(3100.0f / r, 750.0f, 3800.0f);                       // small = high, big = low
      v.ph = 0;
      v.rise = 1.0f + (0.03f + 0.12f * rf()) / (0.015f * SR);
      v.amp = (0.08f + 0.30f * rf()) * (0.6f + 0.1f * r);                  // very uneven loudness
      v.dec = expf(-1.0f / ((0.0025f + 0.0022f * r) * SR));               // 3-12 ms, longer for big ones
      v.splash = 0.18f * v.amp * (0.5f + rf());                            // the "pop" noise at the start
      v.sdec = expf(-1.0f / (0.0008f * SR));
      v.live = true;
      return;
    }
  };
  for (int i = 0; i < n; i++) {
    if (wait == 0) {
      spawn();
      if (burst == 0) burst = 2 + (uint32_t)(10.0f * rf() * rf());       // a new burst of 2-12
      if (--burst > 0) wait = (uint32_t)((0.006f + 0.05f * rf() * rf()) * SR);       // 6-56 ms, mostly quick
      else wait = (uint32_t)((0.25f + 1.3f * rf()) * SR);                             // then a pause
    } else wait--;
    float sum = 0;
    for (Voice &v : vs) if (v.live) {
      rnd ^= rnd << 13; rnd ^= rnd >> 17; rnd ^= rnd << 5;
      const float nz = ((int32_t)(rnd & 0xFFFF) - 32768) / 32768.0f;
      sum += v.amp * sinC(v.ph) + v.splash * nz;
      v.ph += v.f / SR; if (v.ph >= 1.0f) v.ph -= 1.0f;
      v.f *= v.rise; v.amp *= v.dec; v.splash *= v.sdec;
      if (v.amp < 0.004f) v.live = false;
    }
    out[i] = on ? (int16_t)constrain((int32_t)(sum * 32767.0f * 0.8f), -32768, 32767) : 0;
  }
}

void audioBubbles44(int16_t *out, int n) {
  bubblesSynth(out, n, g_bubMode != 0 && g_snd && g_sBub && !g_asleep && !g_isNight);
  for (int i = 0; i < n; i++) out[i] = (int16_t)constrain(((int32_t)out[i] * g_vol * g_bubGain) / 60, -32768, 32767);
}
#else
void audioBubMode(int, int) {}
#endif

void audioMix(int16_t *out, int n) {
  static uint32_t alPos = 0;
  const int alLvl = g_alLevel;
  if (alLvl == 0) alPos = 0;
#ifndef BOARD_CYD28
  static size_t idx = 0;
#endif
#ifndef BOARD_CYD28
  const int16_t *pcm = (const int16_t *)bubbles_start;
  const size_t len = (bubbles_end - bubbles_start) / 2;
#endif
  const int16_t *fx = (const int16_t *)feed_start;
  const size_t fxN = (feed_end - feed_start) / 2;
  const bool on = g_snd && !g_asleep && !g_isNight;
  for (int i = 0; i < n; i++) {
#ifdef BOARD_CYD28
    int32_t s = 0;                                // the ambience is a separate 44.1 kHz stream (audioBubbles44)
#else
    int32_t s = g_sBub ? pcm[idx] : 0;
#endif
    uint32_t cp = g_cashPos;
    if (cp < 17600) { s += cashSample(cp); g_cashPos = cp + 1; }
    size_t fp = g_fxPos;
    if (fp < fxN) { s += ((int32_t)fx[fp] * 7) / 10; g_fxPos = fp + 1; }
    s = on ? (s * g_vol) / 60 : 0;
    { uint32_t lp = g_lambPos; if (lp != 0xFFFFFFFF) { if (lambDone(lp)) g_lambPos = 0xFFFFFFFF; else { s += lambSample(lp); g_lambPos = lp + 1; } } }
    if (alLvl > 0) s += (alarmSample(alPos++) * alLvl) / 100;      // the alarm ignores mute, night mode and sleep
    if (s > 32767) s = 32767; else if (s < -32768) s = -32768;
    out[i] = (int16_t)s;
#ifndef BOARD_CYD28
    if (++idx >= len) idx = 0;
#endif
  }
}
