// Shared audio mixing: looping bubbles + the feed "ding ding", with volume / mute applied.
#pragma once
#include <Arduino.h>
void audioMix(int16_t *out, int n);     // n mono samples at 16 kHz
void audioDing();                       // start the feed sound
void audioAlarm(int level);               // alarm-clock tone, 0 = off .. 100 (independent of the tank volume and mute)
bool audioAlarmOn();
void audioCash();                       // cash register sound (the diver appears)
void audioLamb();                       // speaker test: plays "Mary Had a Little Lamb" (ignores mute and volume)
void audioBubMode(int m, int g);              // CYD only: bubbles on/off and gain (m: 0 off, else on; g: gain 1..16, 1 = as designed at volume 60)
#ifdef BOARD_CYD28
void audioBubbles44(int16_t *out, int n);   // CYD: the synthesised ambience at 44.1 kHz, volume/mute applied
#endif
