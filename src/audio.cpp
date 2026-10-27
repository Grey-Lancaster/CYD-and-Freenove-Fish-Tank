#include "audio.h"
#include "hal.h"

extern const uint8_t bubbles_start[] asm("_binary_src_bubbles_raw_start");
extern const uint8_t bubbles_end[] asm("_binary_src_bubbles_raw_end");
extern const uint8_t feed_start[] asm("_binary_src_feed_raw_start");
extern const uint8_t feed_end[] asm("_binary_src_feed_raw_end");

static volatile size_t g_fxPos = (size_t)-1;      // feed sound position (-1 = idle)

void audioDing() { g_fxPos = 0; }

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
    v += e * e * (0.55f * sinf(6.2832f * 140.0f * t) + (((int32_t)(rnd >> 16) - 32768) / 32768.0f) * 0.35f);
  }
  for (int k = 0; k < 2; k++) {                      // "CHING": the bell struck, then again just after for the shimmer
    const float u = t - 0.10f - 0.07f * k;
    if (u <= 0) continue;
    const float e = expf(-u * 4.5f) * (k ? 0.6f : 1.0f);
    v += e * (0.5f * sinf(6.2832f * 2349.0f * u) + 0.3f * sinf(6.2832f * 3520.0f * u) + 0.25f * sinf(6.2832f * 5587.0f * u));
  }
  return (int32_t)(v * 22000.0f);
}

void audioMix(int16_t *out, int n) {
  static size_t idx = 0;
  const int16_t *pcm = (const int16_t *)bubbles_start;
  const size_t len = (bubbles_end - bubbles_start) / 2;
  const int16_t *fx = (const int16_t *)feed_start;
  const size_t fxN = (feed_end - feed_start) / 2;
  const bool on = g_snd && !g_asleep && !g_isNight;
  for (int i = 0; i < n; i++) {
    int32_t s = pcm[idx];
    uint32_t cp = g_cashPos;
    if (cp < 17600) { s += cashSample(cp); g_cashPos = cp + 1; }
    size_t fp = g_fxPos;
    if (fp < fxN) { s += ((int32_t)fx[fp] * 7) / 10; g_fxPos = fp + 1; }
    s = on ? (s * g_vol) / 60 : 0;
    if (s > 32767) s = 32767; else if (s < -32768) s = -32768;
    out[i] = (int16_t)s;
    if (++idx >= len) idx = 0;
  }
}
