#include "audio.h"
#include "hal.h"

extern const uint8_t bubbles_start[] asm("_binary_src_bubbles_raw_start");
extern const uint8_t bubbles_end[] asm("_binary_src_bubbles_raw_end");
extern const uint8_t feed_start[] asm("_binary_src_feed_raw_start");
extern const uint8_t feed_end[] asm("_binary_src_feed_raw_end");

static volatile size_t g_fxPos = (size_t)-1;      // feed sound position (-1 = idle)

void audioDing() { g_fxPos = 0; }
// Sine by table lookup (x in cycles): sinf per sample was eating the CYD's core and the DAC output crackled.
static int16_t g_sinT[512];
static float sinC(float x) {
  if (!g_sinT[1]) for (int i = 0; i < 512; i++) g_sinT[i] = (int16_t)(sinf(6.2831853f * i / 512.0f) * 32000.0f);
  x -= (int)x; if (x < 0) x += 1.0f;
  return g_sinT[(int)(x * 512.0f) & 511] * (1.0f / 32000.0f);
}
// Cash register "cha-ching", synthesised: a short noise burst, then a bell with inharmonic partials.
static volatile uint32_t g_cashPos = 0xFFFFFFFF;
void audioCash() { g_cashPos = 0; }
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
bool audioAlarmOn() { return g_alLevel > 0; }
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

void audioMix(int16_t *out, int n) {
  static uint32_t alPos = 0;
  const int alLvl = g_alLevel;
  if (alLvl == 0) alPos = 0;
  static size_t idx = 0;
  const int16_t *pcm = (const int16_t *)bubbles_start;
  const size_t len = (bubbles_end - bubbles_start) / 2;
  const int16_t *fx = (const int16_t *)feed_start;
  const size_t fxN = (feed_end - feed_start) / 2;
  const bool on = g_snd && !g_asleep && !g_isNight;
  for (int i = 0; i < n; i++) {
#ifdef BOARD_CYD28
    int32_t s = 0;                                // 8-bit DAC: the ambient bubbles just chirp, so the CYD plays effects only
#else
    int32_t s = pcm[idx];
#endif
    uint32_t cp = g_cashPos;
    if (cp < 17600) { s += cashSample(cp); g_cashPos = cp + 1; }
    size_t fp = g_fxPos;
    if (fp < fxN) { s += ((int32_t)fx[fp] * 7) / 10; g_fxPos = fp + 1; }
    s = on ? (s * g_vol) / 60 : 0;
    if (alLvl > 0) s += (alarmSample(alPos++) * alLvl) / 100;      // the alarm ignores mute, night mode and sleep
    if (s > 32767) s = 32767; else if (s < -32768) s = -32768;
    out[i] = (int16_t)s;
    if (++idx >= len) idx = 0;
  }
}
