// Wi-Fi motion sensing (ported from the CYD/Freenove PhotoFrame project).
#pragma once
#include <Arduino.h>

void motionBegin();          // starts the sensing task (idle until enabled and online)
bool motionDetected();       // someone is moving nearby
void motionRebaseline();     // relearn the quiet level
bool motionReady();          // sensing is running and warmed up
float motionScore();         // >= 1 means motion; for tuning
// causes: 0 no motion, 1 motion, 2 touch, 3 BOOT button, 4 setting turned off, 5 sensor offline
void motionLogEvent(bool sleep, uint8_t cause);
void motionLogClear();
String motionLogJson();

extern bool g_motion;        // sensing enabled (screen sleeps when nobody is around)
extern int g_mmin;           // minutes without motion before the screen sleeps
extern int g_msens;          // 1 (least) .. 10 (most) sensitive
extern bool g_h24;
