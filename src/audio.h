// Shared audio mixing: looping bubbles + the feed "ding ding", with volume / mute applied.
#pragma once
#include <Arduino.h>
void audioMix(int16_t *out, int n);     // n mono samples at 16 kHz
void audioDing();                       // start the feed sound
