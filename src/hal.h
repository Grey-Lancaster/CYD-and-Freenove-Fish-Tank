// Board-specific hardware (hal_freenove.cpp / hal_cyd28.cpp).
#pragma once
#include <Arduino.h>
#include <TFT_eSPI.h>

extern TFT_eSPI tft;
extern int g_rot;               // 1 or 3 (landscape, 3 = rotated 180 deg from 1)
extern bool g_invert, g_swaprb;
extern volatile bool g_snd, g_asleep, g_isNight;
extern volatile bool g_sBub, g_sDiv, g_sFeed;   // per-sound switches: bubbles, diver k-ching, feed ding
extern volatile int g_vol;

void halDisplayBegin();         // tft.init, rotation, colour order, inversion, black screen
void halApplyRotation();        // after g_rot changed
void halApplyColors();          // after g_invert / g_swaprb changed
void halTouchBegin();           // may run an on-screen calibration the first time (CYD)
bool halTouch(int &x, int &y);  // panel pixel coordinates of the current touch, in the current rotation
void halRecalibrateTouch();     // forget calibration and restart (CYD); no-op elsewhere
void halAudioStart();           // sets up the speaker path and starts the looping audio task
void halApplySound();           // after g_snd changed
void halBacklight(bool on);
