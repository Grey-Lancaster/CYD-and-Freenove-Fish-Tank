#include "audio.h"
#include "hal.h"

extern const uint8_t bubbles_start[] asm("_binary_src_bubbles_raw_start");
extern const uint8_t bubbles_end[] asm("_binary_src_bubbles_raw_end");
extern const uint8_t feed_start[] asm("_binary_src_feed_raw_start");
extern const uint8_t feed_end[] asm("_binary_src_feed_raw_end");

static volatile size_t g_fxPos = (size_t)-1;      // feed sound position (-1 = idle)

void audioDing() { g_fxPos = 0; }

void audioMix(int16_t *out, int n) {
  static size_t idx = 0;
  const int16_t *pcm = (const int16_t *)bubbles_start;
  const size_t len = (bubbles_end - bubbles_start) / 2;
  const int16_t *fx = (const int16_t *)feed_start;
  const size_t fxN = (feed_end - feed_start) / 2;
  const bool on = g_snd && !g_asleep;
  for (int i = 0; i < n; i++) {
    int32_t s = pcm[idx];
    size_t fp = g_fxPos;
    if (fp < fxN) { s += ((int32_t)fx[fp] * 7) / 10; g_fxPos = fp + 1; }
    s = on ? (s * g_vol) / 60 : 0;
    if (s > 32767) s = 32767; else if (s < -32768) s = -32768;
    out[i] = (int16_t)s;
    if (++idx >= len) idx = 0;
  }
}
