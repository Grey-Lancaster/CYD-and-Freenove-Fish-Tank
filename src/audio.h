// Shared audio mixing: looping bubbles + the feed "ding ding", with volume / mute applied.
#pragma once
#include <Arduino.h>
void audioMix(int16_t *out, int n);     // n mono samples at 16 kHz
void audioDing();                       // start the feed sound
void audioAlarm(int level);               // alarm-clock tone, 0 = off .. 100 (independent of the tank volume and mute)
bool audioAlarmOn();
void audioCash();                       // cash register sound (the diver appears)
