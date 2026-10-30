// Fish Tank - animated aquarium for the Freenove FNK0104S (480x320, PSRAM) and the CYD 2.8" (320x240, no PSRAM).
// The world lives in a 480x320 virtual space; a Canvas maps it to the panel. The Freenove renders a full frame in
// PSRAM, the CYD renders and pushes the tank in horizontal strips. Touch drops fish food; WiFi is set up on the
// screen with TouchWifiProvisioner.
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <time.h>
#include <ElegantOTA.h>
#include <sys/time.h>
#include "esp_sntp.h"
#include "board.h"
#include "canvas.h"
#include "hal.h"
#include "audio.h"
#include "motion.h"
#include "wifi_setup.h"

#define FW_VERSION "1.7.0"

TFT_eSPI tft = TFT_eSPI();
TFT_eSprite frame = TFT_eSprite(&tft);    // Freenove: the whole frame
TFT_eSprite bg = TFT_eSprite(&tft);       // Freenove: static background
TFT_eSprite strip = TFT_eSprite(&tft);    // CYD: one horizontal strip
Canvas cv;
Preferences prefs;
WebServer server(80);

// Settings (persisted in NVS, editable from the web page)
volatile int g_vol = 60;       // 0..100 (60 = the original loop level)
volatile bool g_snd = true;
volatile bool g_sBub = true, g_sDiv = true, g_sFeed = true;
int g_rot = DEFAULT_ROT;       // 1 or 3 (180 degrees apart)
float g_fps = 0;
bool g_motion = false;         // Wi-Fi motion sensing: screen sleeps when nobody is around
int g_mmin = 10;               // minutes without motion before sleeping
int g_msens = 5;               // 1..10
volatile bool g_asleep = false;
volatile bool g_isNight = false;   // inside the night window: screen and sound off
bool g_night = false;          // night mode enabled
int g_nstart = 22 * 60;        // minutes after midnight the screen turns off
int g_nend = 7 * 60;           // minutes after midnight it turns back on

// ---- alarm clock
bool g_alOn = false;
int g_alMin = 7 * 60;          // minutes after midnight
int g_alDays = 0x7F;           // bit 0 = Sunday .. bit 6 = Saturday
int g_alSnz = 9;               // snooze, minutes
int g_alVol = 70;              // alarm loudness 0..100, separate from the tank volume
enum AlState { AL_IDLE, AL_RAMP, AL_RING, AL_SNOOZE };
AlState alState = AL_IDLE;
uint32_t alT0 = 0, alRampLen = 60000, alSnoozeUntil = 0;
int alFiredDay = -1;           // day of the year it last started, so it rings once a day
bool alMissed = false, alPanel = false;
uint32_t alPanelAt = 0;
static inline bool alActive() { return alState == AL_RAMP || alState == AL_RING; }
static uint32_t nightWakeUntil = 0;   // a touch / BOOT lights the screen for a minute at night
static uint32_t lastActivity = 0;
bool g_invert = false;         // flip the panel's colour inversion
bool g_swaprb = DEFAULT_SWAPRB;  // swap red/blue
bool g_h24 = false;            // 24-hour clock
String g_tz = "EST5EDT,M3.2.0,M11.1.0";   // POSIX TZ string
int g_datefmt = 0;             // 0 Tue 10/20/26, 1 Tue 20/10/26, 2 Tue 2026-10-20, 3 Tue, Oct 20, 4 hidden
bool g_auto = false;           // automatic feeding
int g_aint = 60;               // seconds between automatic feeds
String g_host = DEFAULT_HOST;  // mDNS / OTA / DHCP name (editable on the web page)
uint32_t restartAt = 0;
static bool g_online = false;  // WiFi connected (web page, OTA, NTP available)
static float curT = 0;         // time of the frame being drawn (for the screenshot endpoint)
static uint32_t curNow = 0;

static inline uint16_t RGB(int r, int g, int b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
static inline uint16_t bswap16(uint16_t v) { return (v >> 8) | (v << 8); }

static void saveSettings() {
  prefs.putInt("rot", g_rot); prefs.putBool("snd", g_snd); prefs.putBool("sbub", g_sBub); prefs.putBool("sdiv", g_sDiv); prefs.putBool("sfeed", g_sFeed); prefs.putInt("vol", g_vol);
  prefs.putBool("afeed", g_auto); prefs.putInt("aint", g_aint); prefs.putBool("h24", g_h24); prefs.putString("tz", g_tz); prefs.putInt("dfmt", g_datefmt);
  prefs.putBool("invert", g_invert); prefs.putBool("swaprb", g_swaprb);
  prefs.putBool("motion", g_motion); prefs.putInt("mmin", g_mmin); prefs.putInt("msens", g_msens);
  prefs.putBool("night", g_night); prefs.putInt("nstart", g_nstart); prefs.putInt("nend", g_nend);
  prefs.putBool("alon", g_alOn); prefs.putInt("almin", g_alMin); prefs.putInt("aldays", g_alDays); prefs.putInt("alsnz", g_alSnz); prefs.putInt("alvol", g_alVol);
}

// ---------------------------------------------------------------- fish
enum FishType { TANG, CLOWN, ANGEL, GOLD, NEON };

struct Fish {
  FishType type;
  float x, y, vx, tx, ty, spd, yMin, yMax, ph;
  int dir;
  uint32_t nextPick;
};

Fish fishes[] = {
  {TANG,  300, 105, 0, 0, 0, 38, 60, 150, 0.0f, 1, 0},
  {CLOWN, 150, 205, 0, 0, 0, 34, 160, 250, 1.3f, 1, 0},
  {ANGEL, 200, 150, 0, 0, 0, 26, 90, 200, 2.1f, 1, 0},
  {GOLD,  330, 250, 0, 0, 0, 24, 205, 262, 0.7f, 1, 0},
  {NEON,  100, 70,  0, 0, 0, 55, 40, 110, 0.2f, 1, 0},
  {NEON,  130, 85,  0, 0, 0, 52, 40, 110, 1.1f, 1, 0},
  {NEON,  160, 98,  0, 0, 0, 58, 40, 110, 2.4f, 1, 0},
};
const int NFISH = sizeof(fishes) / sizeof(fishes[0]);

struct Food { float x, y; bool on; uint32_t landed; };
Food food[16];

struct Bubble { float x, y, v, r, ph; };
Bubble bubbles[22];

// Treasure chest on the sand: opens now and then and bubbles pour out.
#define CHEST_X 250
#define CHEST_BASE 303
enum ChestState { CHEST_CLOSED, CHEST_OPENING, CHEST_OPEN, CHEST_CLOSING };
ChestState chestState = CHEST_CLOSED;
float chestT = 0, chestNextOpen = 8, chestBubbleT = 0;
struct ChestBubble { float x, y, v, r, ph; bool on; };
ChestBubble cbub[14];
// Every sixth time the chest opens, a scuba diver swims in from one side, takes a bag of gold from it and leaves by the other side.
struct Diver { float t; bool on; bool right; };
Diver diver = {0, false, false};
int chestOpens = 0;
bool chestLooted = false;      // the diver has taken the gold; refilled once the chest closes
#define DIVER_IN 5.0f      // swim in to the chest
#define DIVER_GRAB 1.2f    // hover and take the bag (bag appears at DIVER_BAG)
#define DIVER_BAG (DIVER_IN + 0.7f)
#define DIVER_OUT 7.0f     // swim off the far side
#define DIVER_DUR (DIVER_IN + DIVER_GRAB + DIVER_OUT)
static void diverPos(float t, float &x, float &y) {
  const float dir = diver.right ? 1.0f : -1.0f;            // direction he travels: enters one side, leaves the other
  const float cx = CHEST_X, cy = CHEST_BASE - 34;
  const float ex = diver.right ? -40.0f : VW + 40.0f, xe = diver.right ? VW + 50.0f : -50.0f;
  if (t < DIVER_IN) {
    float s = t / DIVER_IN;
    x = ex + (cx - ex) * s;
    y = cy - 80 * (1 - s) * (1 - s) + 5 * sinf(s * 8.0f) * (1 - s);
  } else if (t < DIVER_IN + DIVER_GRAB) {
    float u = (t - DIVER_IN) / DIVER_GRAB;
    x = cx + dir * 8 * u;
    y = cy + 2 * sinf(u * 6.2832f);
  } else {
    float s = (t - DIVER_IN - DIVER_GRAB) / DIVER_OUT, x0 = cx + dir * 8;
    x = x0 + (xe - x0) * s;
    y = cy - (cy + 50) * s * s + 12 * sinf(s * 8.0f) * min(1.0f, s * 5.0f);
  }
}
static float chestOpenness() {      // 0 closed .. 1 fully open
  switch (chestState) {
    case CHEST_OPENING: return min(1.0f, chestT / 1.0f);
    case CHEST_OPEN: return 1.0f;
    case CHEST_CLOSING: return max(0.0f, 1.0f - chestT / 1.0f);
    default: return 0.0f;
  }
}

struct Kelp { int x, h; float ph; };
Kelp kelp[] = {{40,150,0},{52,110,1.5f},{430,170,2},{445,120,.5f},{300,90,3},{170,100,4}};

static float halfH(float b, float a, float dx) {
  float q = 1.0f - (dx * dx) / (a * a);
  return q <= 0 ? 0 : b * sqrtf(q);
}

// Vertical stripe/band clipped to the body ellipse.
static void band(float cx, float cy, float a, float b, float dx, int w, uint16_t col, uint16_t edge) {
  float h = halfH(b, a, dx);
  if (h < 2) return;
  int x0 = (int)(cx + dx - w / 2);
  if (edge) cv.fillRect(x0 - 1, (int)(cy - h + 1), w + 2, (int)(2 * h - 1), edge);
  cv.fillRect(x0, (int)(cy - h + 1), w, (int)(2 * h - 1), col);
}

static void eye(float x, float y, int r, uint16_t iris = TFT_WHITE) {
  cv.fillCircle((int)x, (int)y, r, iris);
  cv.fillCircle((int)x + 1, (int)y, r > 3 ? 2 : 1, TFT_BLACK);
}

#define FX(dx) ((int)(x + f * (dx)))

static void drawTang(float x, float y, int f, float wag) {
  const float a = 24, b = 16;
  uint16_t blue = RGB(25, 75, 225), lite = RGB(70, 135, 250), dk = RGB(10, 30, 110), yel = RGB(255, 215, 30);
  cv.fillTriangle(FX(-a * .9f), (int)y, FX(-a - 14), (int)(y - 11 + wag), FX(-a - 14), (int)(y + 11 + wag), yel);
  cv.fillTriangle(FX(-10), (int)(y - b * .9f), FX(6), (int)(y - b - 7), FX(14), (int)(y - b * .8f), blue);
  cv.fillTriangle(FX(-8), (int)(y + b * .9f), FX(4), (int)(y + b + 5), FX(12), (int)(y + b * .85f), blue);
  cv.fillEllipse((int)x, (int)y, a, b, blue);
  cv.fillEllipse(FX(1), (int)y + 3, a - 4, b - 8, lite);
  cv.drawEllipse((int)x, (int)y, a, b, dk);
  cv.drawWideLine(FX(-(a - 8)), y + 1, FX(a - 13), y - 3, 3, dk, dk);
  cv.fillTriangle(FX(4), (int)y + 3, FX(-4), (int)y + 9, FX(1), (int)y + 10, yel);
  eye(FX(a - 9), y - 4, 3);
}

static void drawClown(float x, float y, int f, float wag) {
  const float a = 22, b = 14;
  uint16_t org = RGB(255, 110, 15), lite = RGB(255, 170, 60), dk = RGB(110, 40, 5);
  cv.fillTriangle(FX(-a * .9f), (int)y, FX(-a - 11), (int)(y - 9 + wag), FX(-a - 11), (int)(y + 9 + wag), org);
  cv.fillTriangle(FX(-8), (int)(y - b * .9f), FX(2), (int)(y - b - 6), FX(12), (int)(y - b * .8f), org);
  cv.fillEllipse((int)x, (int)y, a, b, org);
  cv.fillEllipse(FX(1), (int)y + 3, a - 4, b - 7, lite);
  float cx = x;
  // bands are drawn in screen space around the (flipped) offsets
  band(x, y, a, b, f * 10, 5, TFT_WHITE, TFT_BLACK);
  band(x, y, a, b, f * -2, 6, TFT_WHITE, TFT_BLACK);
  band(x, y, a, b, f * -14, 4, TFT_WHITE, TFT_BLACK);
  (void)cx;
  cv.drawEllipse((int)x, (int)y, a, b, dk);
  eye(FX(a - 6), y - 3, 3);
}

static void drawAngel(float x, float y, int f, float wag, float fin) {
  const float a = 18, b = 15;
  uint16_t gray = RGB(120, 126, 138), grayd = RGB(70, 75, 88), cream = RGB(238, 228, 185), white = RGB(245, 245, 245);
  // tail fan
  cv.fillTriangle(FX(-16), (int)y, FX(-36), (int)(y - 15 + wag), FX(-36), (int)(y + 15 + wag), gray);
  cv.drawLine(FX(-16), (int)y, FX(-36), (int)(y + wag), grayd);
  // dorsal, anal, pelvic fins
  cv.fillTriangle(FX(8), (int)(y - b + 3), FX(-10), (int)(y - b - 26), FX(-20), (int)(y - b + 5), gray);
  cv.fillTriangle(FX(6), (int)(y + b - 3), FX(-8), (int)(y + b + 24), FX(-17), (int)(y + b - 4), gray);
  cv.drawWideLine(FX(4), y + b - 3, FX(-2 + fin), y + b + 30, 2, gray, gray);
  // body: cream top, white belly
  cv.fillEllipse((int)x, (int)y, a, b, white);
  cv.fillEllipse((int)x, (int)y - 4, a - 1, b - 5, cream);
  cv.drawEllipse((int)x, (int)y, a, b, grayd);
  band(x, y, a, b, f * 6, 2, TFT_BLACK, 0);
  band(x, y, a, b, f * -1, 2, TFT_BLACK, 0);
  band(x, y, a, b, f * -8, 2, TFT_BLACK, 0);
  cv.fillTriangle(FX(3), (int)y + 2, FX(-3), (int)y + 8, FX(0), (int)y + 9, RGB(215, 222, 232));
  eye(FX(a - 6), y - 3, 3, RGB(230, 45, 40));
}

static void drawGold(float x, float y, int f, float wag) {
  const float a = 20, b = 13;
  uint16_t org = RGB(240, 95, 15), lite = RGB(255, 170, 50), tailc = RGB(255, 125, 30), dk = RGB(120, 45, 5);
  cv.fillTriangle(FX(-16), (int)y, FX(-38), (int)(y - 17 + wag), FX(-30), (int)(y + wag), tailc);
  cv.fillTriangle(FX(-16), (int)y, FX(-38), (int)(y + 17 + wag), FX(-30), (int)(y + wag), tailc);
  cv.fillTriangle(FX(-6), (int)(y - b * .9f), FX(2), (int)(y - b - 7), FX(10), (int)(y - b * .8f), tailc);
  cv.fillEllipse((int)x, (int)y, a, b, org);
  cv.fillEllipse(FX(1), (int)y + 3, a - 4, b - 7, lite);
  cv.drawEllipse((int)x, (int)y, a, b, dk);
  eye(FX(a - 6), y - 3, 3);
}

static void drawNeon(float x, float y, int f, float wag) {
  const float a = 12, b = 5;
  cv.fillTriangle(FX(-a + 2), (int)y, FX(-a - 6), (int)(y - 4 + wag), FX(-a - 6), (int)(y + 4 + wag), RGB(200, 210, 230));
  cv.fillEllipse((int)x, (int)y, a, b, RGB(235, 35, 45));
  cv.fillEllipse(FX(4), (int)y, a - 5, b - 1, RGB(200, 212, 228));
  cv.drawWideLine(FX(a - 2), y - 1, FX(-1), y - 1, 2, RGB(40, 210, 255), RGB(40, 210, 255));
  eye(FX(a - 3), y - 1, 2);
}

// ----------------------------------------------------------- background
// Gradient water, sand, speckles and rocks for whatever rows the current canvas covers.
static void renderBackground(Canvas &c) {
  TFT_eSprite *s = c.s;
  for (int r = 0; r < s->height(); r++) {
    float vy = (r + c.oy) / c.sy;
    float t = vy / VH;
    uint16_t col = RGB((int)(20 + 10 * (1 - t)), (int)(110 - 60 * t), (int)(190 - 90 * t));
    if (vy >= SAND_Y) {
      float u = (vy - SAND_Y) / (VH - SAND_Y);
      col = RGB((int)(214 - 40 * u), (int)(190 - 40 * u), (int)(140 - 40 * u));
    }
    s->drawFastHLine(0, r, s->width(), col);
  }
  uint32_t seed = 7;
  for (int i = 0; i < 500; i++) {
    seed = seed * 1664525u + 1013904223u; int vx = (seed >> 8) % VW;
    seed = seed * 1664525u + 1013904223u; int vy = SAND_Y + 2 + (seed >> 8) % (VH - SAND_Y - 2);
    c.drawPixel(vx, vy, RGB(150, 125, 90));
  }
  struct R { int cx, cy, rx, ry; uint16_t c; } rocks[] = {
    {90, 296, 32, 18, RGB(95, 95, 105)}, {120, 302, 22, 12, RGB(120, 118, 125)}, {380, 300, 38, 20, RGB(90, 90, 100)}};
  for (auto &r : rocks) {
    c.fillEllipse(r.cx, r.cy, r.rx, r.ry, r.c);
    c.fillEllipse(r.cx - r.rx / 4, r.cy - r.ry / 2, r.rx / 2, r.ry / 3, RGB(140, 140, 150));
  }
}

// Lighten pixels (byte-swapped RGB565 in the sprite buffer) for the light rays.
static void drawRays(float t) {
  TFT_eSprite *s = cv.s;
  uint16_t *buf = (uint16_t *)s->getPointer();
  const int W = s->width();
  const struct { float x0, w, ph; } rays[] = {{80, 40, 0}, {200, 60, 1.5f}, {330, 35, 3}, {420, 50, 4.5f}};
  for (auto &r : rays) {
    float xs = r.x0 + 25 * sinf(t * 0.4f + r.ph);
    for (int row = 0; row < s->height(); row++) {
      float vy = (row + cv.oy) / cv.sy;
      if (vy >= SAND_Y) break;
      int xa = (int)((xs - vy * 0.28f) * cv.sx), xb = (int)((xs - vy * 0.28f + r.w) * cv.sx);
      if (xa < 0) xa = 0;
      if (xb > W) xb = W;
      uint16_t *p = buf + row * W + xa;
      for (int x = xa; x < xb; x++, p++) {
        uint16_t c = bswap16(*p);
        int rr = c >> 11, g = (c >> 5) & 63, b = c & 31;
        rr += (31 - rr) >> 4; g += (63 - g) >> 4; b += (31 - b) >> 4;
        *p = bswap16((rr << 11) | (g << 5) | b);
      }
    }
  }
}

// ---------------------------------------------------------------- clock + network
static uint32_t baseSecs = 0;      // seconds since midnight at millis()==0 (fallback clock)

static void startServices();

static void initClockFallback() {
  int h = 0, m = 0, s = 0;
  sscanf(__TIME__, "%d:%d:%d", &h, &m, &s);
  baseSecs = h * 3600 + m * 60 + s;
}

// WiFi comes from the on-screen picker (TouchWifiProvisioner). BOOT skips it: the tank then runs offline.
static void connectWifi() {
  static volatile bool up = false;
  wifiSetupBegin(g_host.c_str(), [](const String &) { up = true; });
  uint32_t armed = millis();
  while (!up) {
    wifiSetupLoop();
    if (millis() - armed > 1500 && digitalRead(0) == LOW) break;     // BOOT pressed: skip
    delay(5);
  }
  tft.fillScreen(TFT_BLACK);
  if (up) startServices();
}

// Time and date text in the chosen formats (date is empty until the clock is set, or when hidden).
static void clockStrings(char *buf, size_t bn, char *date, size_t dn) {
  int h, mi;
  date[0] = 0;
  struct tm tmv;
  if (getLocalTime(&tmv, 0)) {
    h = tmv.tm_hour; mi = tmv.tm_min;
    static const char *dnm[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char *mnm[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    switch (g_datefmt) {
      case 0: snprintf(date, dn, "%s %02d/%02d/%02d", dnm[tmv.tm_wday], tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_year % 100); break;
      case 1: snprintf(date, dn, "%s %02d/%02d/%02d", dnm[tmv.tm_wday], tmv.tm_mday, tmv.tm_mon + 1, tmv.tm_year % 100); break;
      case 2: snprintf(date, dn, "%s %04d-%02d-%02d", dnm[tmv.tm_wday], tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday); break;
      case 3: snprintf(date, dn, "%s, %s %d", dnm[tmv.tm_wday], mnm[tmv.tm_mon], tmv.tm_mday); break;
      default: break;
    }
  } else {
    uint32_t s = (baseSecs + millis() / 1000) % 86400;
    h = s / 3600; mi = (s / 60) % 60;
  }
  if (g_h24) snprintf(buf, bn, "%02d:%02d", h, mi);
  else {
    int h12 = h % 12; if (h12 == 0) h12 = 12;
    snprintf(buf, bn, "%d:%02d %s", h12, mi, h >= 12 ? "PM" : "AM");
  }
}

static void drawClock() {
  char buf[16], date[24];
  clockStrings(buf, sizeof(buf), date, sizeof(date));
  cv.setTextDatum(TR_DATUM);
  cv.setTextColor(RGB(10, 40, 80));
  cv.drawString(buf, VW - 9, TIME_Y + 1, 4);
  if (date[0]) cv.drawString(date, VW - 9, DATE_Y + 1, 2);
  cv.setTextColor(RGB(235, 245, 255));
  cv.drawString(buf, VW - 10, TIME_Y, 4);
  if (date[0]) cv.drawString(date, VW - 10, DATE_Y, 2);
  if (g_alOn) {                                   // little bell: an alarm is set
    const float bx = VW - 10 - cv.s->textWidth(buf, 4) / cv.sx - 16, by = TIME_Y + 13;
    const uint16_t gold = RGB(240, 200, 60);
    cv.fillEllipse(bx, by, 6, 7, gold);
    cv.fillRect(bx - 8, by + 4, 16, 3, gold);
    cv.fillCircle(bx, by + 10, 2, gold);
  }
}

// ------------------------------------------------------------- behaviour
static void pickTarget(Fish &f, uint32_t now) {
  f.tx = 30 + random(VW - 60);
  f.ty = f.yMin + random((int)(f.yMax - f.yMin));
  f.nextPick = now + 2500 + random(5000);
}

static void spawnFood(float x, float y) {
  for (auto &fd : food)
    if (!fd.on) { fd = {x, y, true, 0}; return; }
}

struct Ring { float x, y, age; bool on; };
Ring rings[6];
static void feedAt(float x, float y, int n) {
  for (int i = 0; i < n; i++) spawnFood(x + random(-8, 9), y + random(-4, 5));
  for (auto &r : rings) if (!r.on) { r = {x, y, 0, true}; break; }      // sonar ring
  audioDing();
}

static void updateChest(float dt) {
  chestT += dt;
  switch (chestState) {
    case CHEST_CLOSED:  if (chestT >= chestNextOpen) { chestState = CHEST_OPENING; chestT = 0; } break;
    case CHEST_OPENING:
      if (chestT >= 1.0f) {
        chestState = CHEST_OPEN; chestT = 0; chestBubbleT = 0;
        if (++chestOpens % 6 == 0 && !diver.on) {
          static bool nextRight = false;
          diver = {0, true, nextRight}; nextRight = !nextRight; audioCash();
        }
      }
      break;
    case CHEST_OPEN:
      chestBubbleT += dt;
      while (chestBubbleT > 0.11f) {
        chestBubbleT -= 0.11f;
        for (auto &b : cbub) if (!b.on) { b = {CHEST_X + (float)random(-14, 15), (float)(CHEST_BASE - 24), 45.0f + random(40), 2.0f + random(4), (float)random(628) / 100.0f, true}; break; }
      }
      if (chestT >= 4.5f && !(diver.on && diver.t < DIVER_IN + DIVER_GRAB + 1.5f)) { chestState = CHEST_CLOSING; chestT = 0; }
      break;
    case CHEST_CLOSING: if (chestT >= 1.0f) { chestState = CHEST_CLOSED; chestT = 0; chestLooted = false; chestNextOpen = 12 + random(10); } break;
  }
  for (auto &b : cbub) if (b.on) { b.y -= b.v * dt; if (b.y < -6) b.on = false; }
  if (diver.on) {
    float dx, dy;
    float before = fmodf(diver.t, 0.35f);
    float tPrev = diver.t;
    diver.t += dt;
    if (tPrev < DIVER_BAG && diver.t >= DIVER_BAG) chestLooted = true;
    if (floorf(diver.t / 2.5f) > floorf(tPrev / 2.5f))
      audioCash();                               // rings as he starts swimming in, and keeps ringing all the way out
    if (diver.t >= DIVER_DUR) diver.on = false;
    else if (before + dt >= 0.35f) {             // breath bubbles from the regulator
      diverPos(diver.t, dx, dy);
      for (auto &b : cbub) if (!b.on) { b = {dx - 6, dy - 6, 40.0f + random(30), 1.5f + random(3), (float)random(628) / 100.0f, true}; break; }
    }
  }
}

static void updateWorld(float dt, float t, uint32_t now) {
  updateChest(dt);
  static uint32_t nextFeed = 0;
  if (g_auto) {
    if (nextFeed == 0 || now >= nextFeed) {
      if (nextFeed != 0) feedAt(60 + random(VW - 120), 16, 4);
      nextFeed = now + (uint32_t)g_aint * 1000UL;
    }
  } else nextFeed = 0;
  for (auto &r : rings) if (r.on) { r.age += dt; if (r.age > 1.5f) r.on = false; }
  for (int i = 0; i < NFISH; i++) {
    Fish &f = fishes[i];
    if (alActive()) {                             // alarm: swim up as the sun rises, then circle the clock
      const float a = t * 0.5f + i * 6.2832f / NFISH;
      if (alState == AL_RING) { f.tx = VW / 2 + 165 * cosf(a); f.ty = 150 + 85 * sinf(a); }
      else f.ty = f.yMin;
      f.nextPick = now + 1000;
    }
    float tx = f.tx, ty = f.ty;
    int target = -1; float best = 1e9;
    for (int k = 0; k < 16; k++) {
      if (!food[k].on) continue;
      float d = fabsf(food[k].x - f.x) + fabsf(food[k].y - f.y);
      if (d < best) { best = d; target = k; }
    }
    float spd = f.spd;
    if (target >= 0) {
      tx = food[target].x; ty = food[target].y; spd *= 2.2f;
      if (fabsf(tx - f.x) < 14 && fabsf(ty - f.y) < 12) food[target].on = false;   // eaten
    } else if (!alActive() && (now > f.nextPick || (fabsf(tx - f.x) < 8 && fabsf(ty - f.y) < 6))) {
      pickTarget(f, now); tx = f.tx; ty = f.ty;
    }
    float dx = tx - f.x, dy = ty - f.y;
    float want = fabsf(dx) < 4 ? 0 : (dx > 0 ? spd : -spd);
    f.vx += (want - f.vx) * min(1.0f, dt * 2.5f);
    float vy = fabsf(dy) < 3 ? 0 : (dy > 0 ? spd * .45f : -spd * .45f);
    f.x += f.vx * dt;
    f.y += vy * dt + sinf(t * 2.0f + f.ph) * 6.0f * dt;
    f.x = constrain(f.x, 30.0f, (float)(VW - 30));
    if (f.vx > 6) f.dir = 1; else if (f.vx < -6) f.dir = -1;
  }
  for (auto &fd : food) {
    if (!fd.on) continue;
    if (fd.y < SAND_Y + 6) fd.y += 28 * dt; else if (!fd.landed) fd.landed = now;
    if (fd.landed && now - fd.landed > 4000) fd.on = false;
  }
  for (auto &b : bubbles) {
    b.y -= b.v * dt;
    if (b.y < -6) { b.y = SAND_Y - 6; b.x = 40 + random(VW - 80); }
  }
}




// ---------------------------------------------------------------- motion sleep
static void applyBacklight() { halBacklight(!(g_asleep || g_isNight)); }

static void wakeScreen(uint8_t cause) {
  if (!g_asleep) return;
  g_asleep = false;
  motionLogEvent(false, cause | (g_isNight ? 0x80 : 0));      // 0x80: night mode, the screen stays dark regardless
  applyBacklight();
  lastActivity = millis();
}

// With motion sensing on, the screen sleeps after g_mmin minutes without motion or touch and
// wakes on either. If sensing is not running (offline, warming up) that counts as activity,
// so the tank never sleeps blind.
static void updateSleep() {
  static uint32_t last = 0;
  uint32_t now = millis();
  if (now - last < 250) return;
  last = now;
  if (!g_motion) { wakeScreen(4); return; }
  // Right after the screen goes dark the radio environment changes (backlight current), which used to look like
  // motion and woke the screen again within seconds: ignore motion for a while after sleeping and relearn the quiet level.
  static uint32_t sleepAt = 0;
  bool motionNow = motionDetected() && !(g_asleep && now - sleepAt < 20000);
  if (!motionReady() || motionNow) {
    lastActivity = now;
    if (g_asleep) motionRebaseline();
    wakeScreen(motionNow ? 1 : 5);
    return;
  }
  if (!g_asleep && now - lastActivity >= (uint32_t)g_mmin * 60000UL) {
    g_asleep = true;
    sleepAt = now;
    motionRebaseline();
    motionLogEvent(true, g_isNight ? 0x80 : 0);
    applyBacklight();
  }
}

// ---------------------------------------------------------------- alarm clock
// A minute before the set time the tank brightens like a sunrise (silently); at the time the fish
// circle a big clock and the chime plays, getting louder until Stop or Snooze (touch, or the BOOT button). It lights the
// screen even in night mode / motion sleep, and sounds even with the tank muted.
static void alSetSound(int lvl) {
  const bool was = audioAlarmOn();
  audioAlarm(lvl);
  if (was != (lvl > 0)) halApplySound();
}
static void alStop() { alState = AL_IDLE; alSetSound(0); }
static void alSnooze(uint32_t now) { alState = AL_SNOOZE; alSnoozeUntil = now + (uint32_t)g_alSnz * 60000UL; alSetSound(0); }
static void alTest(uint32_t now) { alRampLen = 8000; alT0 = now; alState = AL_RAMP; alMissed = false; }
static float alProgress(uint32_t now) { return alState == AL_RAMP ? min(1.0f, (now - alT0) / (float)alRampLen) : 1.0f; }

static void updateAlarm(uint32_t now) {
  static uint32_t last = 0;
  if (now - last < 250) return;
  last = now;
  if (alState == AL_IDLE && g_alOn && !alPanel) {                 // not while the set panel is open: wait for Done
    struct tm tm;
    if (getLocalTime(&tm, 0) && ((g_alDays >> tm.tm_wday) & 1) && alFiredDay != tm.tm_yday) {
      const int d = g_alMin * 60 - (tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec);   // seconds until the alarm
      if (d <= 60 && d > -120) {
        alFiredDay = tm.tm_yday; alRampLen = 60000; alMissed = false;
        alT0 = now - (d > 0 ? (uint32_t)(60 - d) * 1000UL : 60000UL);
        alState = AL_RAMP;
      }
    }
  }
  if (alState == AL_RAMP && now - alT0 >= alRampLen) { alState = AL_RING; alT0 = now; }
  else if (alState == AL_SNOOZE && now >= alSnoozeUntil) { alState = AL_RING; alT0 = now; }
  else if (alState == AL_RING && now - alT0 > 600000UL) { alStop(); alMissed = true; }   // nobody came: give up after 10 min
  if (alActive()) {
    const float e = min(1.0f, (now - alT0) / 30000.0f);
    alSetSound(alState == AL_RAMP ? 0 : (int)(g_alVol * (0.6f + 0.4f * e)));      // silent sunrise lead-in; the chime starts at the set time
    nightWakeUntil = now + 5000; lastActivity = now;
    if (g_isNight) { g_isNight = false; applyBacklight(); }
    if (g_asleep) wakeScreen(6);
  }
  if (alPanel && now - alPanelAt > 30000) { alPanel = false; saveSettings(); }
}

// ---- night mode: screen and sound off between two times of day (local time; needs the clock to be set)
static bool inNightWindow() {
  if (!g_night) return false;
  struct tm t;
  if (!getLocalTime(&t, 0)) return false;
  int now = t.tm_hour * 60 + t.tm_min;
  if (g_nstart == g_nend) return false;
  return g_nstart < g_nend ? (now >= g_nstart && now < g_nend) : (now >= g_nstart || now < g_nend);
}

static void updateNight(uint32_t now) {
  static uint32_t last = 0;
  if (now - last < 1000) return;
  last = now;
  bool night = inNightWindow() && now >= nightWakeUntil;
  if (night != g_isNight) { g_isNight = night; applyBacklight(); if (!night) lastActivity = now; }
}

// A touch or BOOT while it is dark lights the screen for a minute.
static bool nightWake(uint32_t now) {
  if (!g_isNight) return false;
  nightWakeUntil = now + 60000;
  g_isNight = false;
  applyBacklight();
  lastActivity = now;
  return true;
}

static const char PAGE[] PROGMEM = R"HTML(<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>Fish Tank</title>
<link rel="icon" href="/favicon.ico">
<style>body{margin:0;font:16px system-ui,sans-serif;background:#0b2a45;color:#e8f4ff}
main{max-width:460px;margin:0 auto;padding:16px}h1{margin:.2em 0}
.c{background:#12466f;border-radius:12px;padding:14px;margin:12px 0}
label{display:flex;justify-content:space-between;align-items:center;margin:10px 0}
input[type=range]{width:55%}select,button{font:inherit;padding:8px 12px;border-radius:8px;border:0}
button,.b{background:#2fb36b;color:#fff;text-decoration:none;border-radius:8px;padding:8px 12px;margin-top:10px}small{color:#9cc4e4}</style></head><body><main>
<h1>&#128032; Fish Tank</h1><small id="st">loading&hellip;</small>
<div class="c"><label>Sound <input type="checkbox" id="snd"></label>
<label>&nbsp;&nbsp;Bubbles <input type="checkbox" id="sbub"></label>
<label>&nbsp;&nbsp;Diver k-ching <input type="checkbox" id="sdiv"></label>
<label>&nbsp;&nbsp;Feeding ding <input type="checkbox" id="sfeed"></label>
<label>Volume <input type="range" id="vol" min="0" max="100"><span id="vv"></span></label>
<label>Screen rotation <select id="rot"><option value="@L@">USB on left</option><option value="@R@">USB on right</option></select></label>
<label>Invert colours <input type="checkbox" id="inv"></label>
<label>Swap red and blue <input type="checkbox" id="swaprb"></label>
<small>Use these if the colours look wrong (like a photo negative, or red and blue swapped).</small></div>
<div class="c"><button id="feed">Feed the fish</button> <button id="chest">Open the treasure chest</button> <button id="diver">Send the diver</button>
<label style="margin-top:14px">Auto feed <input type="checkbox" id="auto"></label>
<label>Every (seconds) <input type="number" id="aint" min="5" max="3600" style="width:90px"></label></div>
<div class="c"><b>Date &amp; time</b> <small id="now"></small>
<label>Time zone <select id="tz">
<option value="EST5EDT,M3.2.0,M11.1.0">US Eastern</option><option value="CST6CDT,M3.2.0,M11.1.0">US Central</option>
<option value="MST7MDT,M3.2.0,M11.1.0">US Mountain</option><option value="MST7">US Arizona</option>
<option value="PST8PDT,M3.2.0,M11.1.0">US Pacific</option><option value="AKST9AKDT,M3.2.0,M11.1.0">US Alaska</option>
<option value="HST10">US Hawaii</option><option value="GMT0BST,M3.5.0/1,M10.5.0">UK / Ireland</option>
<option value="CET-1CEST,M3.5.0,M10.5.0">Central Europe</option><option value="EET-2EEST,M3.5.0/3,M10.5.0/4">Eastern Europe</option>
<option value="IST-5:30">India</option><option value="CST-8">China</option><option value="JST-9">Japan</option>
<option value="AEST-10AEDT,M10.1.0,M4.1.0/3">Australia East</option><option value="UTC0">UTC</option></select></label>
<label>Date format <select id="dfmt"><option value="0">Tue 10/20/26 (month/day)</option><option value="1">Tue 20/10/26 (day/month)</option>
<option value="2">Tue 2026-10-20</option><option value="3">Tue, Oct 20</option><option value="4">Hide the date</option></select></label>
<label>24-hour time <input type="checkbox" id="h24"></label>
<button id="settime">Set time from this device</button> <small id="tmsg"></small></div>
<div class="c"><button id="shot">Screenshot</button> <label style="display:inline"><input type="checkbox" id="live"> live view</label>
<small id="shm"> (the tank pauses for a second while it copies)</small>
<canvas id="cv" width="480" height="320" style="display:none;width:100%;margin-top:10px;border-radius:8px"></canvas>
<a id="dl" style="display:none" class="b">Download PNG</a></div>
<div class="c"><b>Alarm clock</b> <small id="alstat"></small>
<label>Alarm on <input type="checkbox" id="alon"></label>
<label>Wake at <span><select id="al_h"></select> : <select id="al_m"></select></span></label>
<div id="aldays" style="margin:6px 0"></div>
<label>Snooze (minutes) <input type="number" id="alsnz" min="1" max="30" style="width:90px"></label>
<label>Alarm loudness <input type="range" id="alvol" min="1" max="100"><span id="alvv"></span></label>
<button id="altest">Test alarm</button> <button id="alstop">Stop alarm</button>
<small>A minute before, the tank silently brightens like a sunrise; at the set time the chime starts and the fish circle a big clock. You can also tap the time on the tank to set it. The alarm sounds even if the tank is muted.</small></div>
<div class="c"><b>Night mode</b> <small id="nstat"></small>
<label>Screen and sound off at night <input type="checkbox" id="night"></label>
<label>Off from <span><select id="nstart_h"></select> : <select id="nstart_m"></select></span></label>
<label>Back on at <span><select id="nend_h"></select> : <select id="nend_m"></select></span></label>
<small>Uses the time zone above (the clock must be set). Touch the screen or press BOOT to light it for a minute.</small></div>
<div class="c"><b>Motion sleep</b> <small>(experimental)</small>
<label>Sleep the screen when nobody is around <input type="checkbox" id="motion"></label>
<label>Sleep after (minutes) <input type="number" id="mmin" min="1" max="240" style="width:90px"></label>
<label>Sensitivity <input type="range" id="msens" min="1" max="10"><span id="msv"></span></label>
<small id="mlive"></small>
<div id="mlog" style="margin-top:8px;font-size:14px"></div>
<button id="mclr">Clear log</button></div>
<div class="c"><button id="apply">Apply</button> <small id="amsg"></small><br><small>Applies sound, volume, rotation, colours, time zone, date format and the other settings above.</small></div>
<div class="c"><b>Firmware update</b><br><small>Open the update page and choose the <b>update</b> file for this board (<b id="envname"></b>; not the factory file): it keeps your WiFi and settings.</small><br>
<a href="/update"><button type="button">Open update page</button></a></div>
<div class="c"><b>Restart log</b> <small>(crashes and watchdog restarts, newest first)</small><div id="rlog" style="margin-top:6px;font-size:13px;white-space:pre-wrap;word-break:break-word"></div><button id="rlogclr">Clear</button></div>
<div class="c"><b>System</b><br>
<button id="wifi">Forget WiFi &amp; restart</button> <button id="recal" style="display:none">Recalibrate touch</button> <small id="sysmsg"></small></div>
<div class="c"><label>Hostname <input id="host" maxlength="31" style="width:55%"></label>
<small>letters, digits and dashes &mdash; reachable as <b id="hl"></b>.local</small><br><br>
<button id="hsave">Save &amp; reboot</button> <small id="hmsg"></small></div>
<script>
const $=i=>document.getElementById(i);let busy=0,dirty=0;
function fillTime(id){const h=$(id+'_h'),mi=$(id+'_m');if(h.options.length)return;
 for(let i=0;i<24;i++){const t=i==0?'12 AM (midnight)':i<12?i+' AM':i==12?'12 PM (noon)':(i-12)+' PM';h.add(new Option(t,i))}
 for(let i=0;i<60;i+=5)mi.add(new Option(String(i).padStart(2,'0'),i))}
function setTime(id,v){fillTime(id);const p=(v||'0:0').split(':'),h=+p[0],mm=+p[1],mi=$(id+'_m');
 if(![...mi.options].some(o=>+o.value==mm)){const vals=[...mi.options].map(o=>+o.value);vals.push(mm);vals.sort((a,b)=>a-b);mi.innerHTML='';vals.forEach(x=>mi.add(new Option(String(x).padStart(2,'0'),x)))}
 $(id+'_h').value=h;mi.value=mm}
function getTime(id){return $(id+'_h').value+':'+String($(id+'_m').value).padStart(2,'0')}
fillTime('nstart');fillTime('nend');fillTime('al');
$('aldays').innerHTML=['Sun','Mon','Tue','Wed','Thu','Fri','Sat'].map((n,i)=>'<label style="display:inline;margin-right:8px"><input type=checkbox id=ad'+i+'> '+n+'</label>').join('');
async function load(){const s=await (await fetch('/api/status')).json();
 $('mlive').textContent=s.motion?(s.mready?'motion score '+s.mscore+(s.mon?' \u25cf moving':' (quiet)')+(s.asleep?' \u00b7 screen asleep':''):'sensor warming up\u2026'):'';
window.envId=s.env;$('envname').textContent=s.env;$('recal').style.display=s.board=='cyd'?'inline-block':'none';
$('now').textContent=s.now+(s.ntp?'  (internet time)':'');
if(!s.ntp&&!window.timeSent){window.timeSent=1;fetch('/api/settime?epoch='+Math.floor(Date.now()/1000),{method:'POST'})}
$('nstat').textContent=s.isnight?'(it is night now: screen off)':'';
if(!busy&&!dirty){$('alon').checked=s.alon;setTime('al',s.atime);for(let i=0;i<7;i++)$('ad'+i).checked=!!((s.adays>>i)&1);$('alsnz').value=s.asnz;$('alvol').value=s.avol;$('alvv').textContent=s.avol;$('night').checked=s.night;setTime('nstart',s.nstart);setTime('nend',s.nend);$('tz').value=s.tz;$('dfmt').value=s.dfmt;$('inv').checked=s.inv;$('swaprb').checked=s.swaprb;$('motion').checked=s.motion;$('mmin').value=s.mmin;$('msens').value=s.msens;$('msv').textContent=s.msens;$('h24').checked=s.h24;$('auto').checked=s.auto;$('aint').value=s.aint;if(document.activeElement!==$('host'))$('host').value=s.host;$('hl').textContent=s.host;$('snd').checked=s.snd;$('sbub').checked=s.sbub;$('sdiv').checked=s.sdiv;$('sfeed').checked=s.sfeed;$('vol').value=s.vol;$('vv').textContent=s.vol;$('rot').value=s.rot}
 $('alstat').textContent=['','sunrise starting\u2026','RINGING','snoozed'][s.alstate]||(s.almiss?'last alarm was not stopped':'');
 $('st').textContent='v'+s.ver+' · WiFi '+(s.ssid||'?')+' · '+s.ip+' · '+s.rssi+' dBm · up '+Math.floor(s.up/60)+' min (last restart: '+s.rst+') · '+s.fps.toFixed(1)+' fps'}
async function send(){if(!$('mmin').value||!$('aint').value){$('amsg').textContent='enter a number in the sleep / auto feed boxes';return}
busy=1;await fetch('/api/set?snd='+($('snd').checked?1:0)+'&sbub='+($('sbub').checked?1:0)+'&sdiv='+($('sdiv').checked?1:0)+'&sfeed='+($('sfeed').checked?1:0)+'&vol='+$('vol').value+'&rot='+$('rot').value+'&motion='+($('motion').checked?1:0)+'&mmin='+$('mmin').value+'&msens='+$('msens').value+'&inv='+($('inv').checked?1:0)+'&swaprb='+($('swaprb').checked?1:0)+'&night='+($('night').checked?1:0)+'&nstart='+getTime('nstart')+'&nend='+getTime('nend')+'&tz='+encodeURIComponent($('tz').value)+'&dfmt='+$('dfmt').value+'&h24='+($('h24').checked?1:0)+'&auto='+($('auto').checked?1:0)+'&aint='+$('aint').value+'&alon='+($('alon').checked?1:0)+'&atime='+getTime('al')+'&adays='+[0,1,2,3,4,5,6].reduce((a,i)=>a+($('ad'+i).checked?1<<i:0),0)+'&asnz='+$('alsnz').value+'&avol='+$('alvol').value,{method:'POST'});busy=0;dirty=0;$('amsg').textContent='Applied \u2713'}
const mark=()=>{dirty=1;$('amsg').textContent='not applied yet'};
['alon','al_h','al_m','ad0','ad1','ad2','ad3','ad4','ad5','ad6','alsnz','alvol','snd','sbub','sdiv','sfeed','h24','auto','aint','rot','motion','mmin','msens','inv','swaprb','tz','dfmt','night','nstart_h','nstart_m','nend_h','nend_m'].forEach(i=>{$(i).onchange=mark;$(i).oninput=mark});$('vol').oninput=()=>{$('vv').textContent=$('vol').value;mark()};$('msens').oninput=()=>{$('msv').textContent=$('msens').value;mark()};$('apply').onclick=send;$('alvol').oninput=()=>{$('alvv').textContent=$('alvol').value;mark()};$('altest').onclick=()=>fetch('/api/alarm?cmd=test',{method:'POST'});$('alstop').onclick=()=>fetch('/api/alarm?cmd=stop',{method:'POST'});
const CAUSE=['no motion','motion','touch','BOOT button','setting turned off','sensor offline','alarm'];
async function mlog(){try{const a=await (await fetch('/api/motionlog')).json();
 $('mlog').innerHTML=a.length?a.map(e=>((e.c&128)?(e.e?'\u25cb motion seen':'\u25cb quiet'):(e.e?'\u2600 woke':'\u263e slept'))+' \u00b7 '+(e.w||'up '+Math.floor(e.up/60)+' min')+' \u00b7 '+CAUSE[e.c&127]+((e.c&128)?' \u00b7 night mode, screen stays dark':'')).join('<br>'):'<small>No sleep/wake events yet.</small>'}catch(e){}}
async function rlog(){try{const t=await (await fetch('/api/resetlog')).text();$('rlog').textContent=t.trim()||'None recorded.'}catch(e){}}
$('rlogclr').onclick=async()=>{await fetch('/api/resetlog?clear=1');rlog()};
rlog();
$('mclr').onclick=async()=>{await fetch('/api/motionlog/clear',{method:'POST'});mlog()};
mlog();setInterval(mlog,5000);
async function shot(){const r=await fetch('/api/frame',{cache:'no-store'});const W=+r.headers.get('X-W')||480,H=+r.headers.get('X-H')||320;const b=new Uint8Array(await r.arrayBuffer());
 if(b.length!=W*H*2)return;const cv=$('cv');cv.width=W;cv.height=H;const x=cv.getContext('2d'),im=x.createImageData(W,H);
 for(let i=0,o=0;i<b.length;i+=2,o+=4){const v=(b[i]<<8)|b[i+1];im.data[o]=((v>>11)&31)*255/31;im.data[o+1]=((v>>5)&63)*255/63;im.data[o+2]=(v&31)*255/31;im.data[o+3]=255}
 x.putImageData(im,0,0);cv.style.display='block';
 cv.toBlob(bl=>{const a=$('dl');a.href=URL.createObjectURL(bl);a.download='fishtank-'+new Date().toISOString().replace(/[:.]/g,'-').slice(0,19)+'.png';a.textContent='Download PNG';a.style.display='inline-block'})}
$('shot').onclick=shot;
$('live').onchange=()=>{clearInterval(window.lv);if($('live').checked){shot();window.lv=setInterval(shot,2500)}};
$('wifi').onclick=async()=>{if(!confirm('Forget all saved WiFi networks and restart? You will set up WiFi again on the screen.'))return;await fetch('/api/wifi/forget',{method:'POST'});$('sysmsg').textContent='Restarting\u2026'};
$('recal').onclick=async()=>{if(!confirm('Restart and calibrate the touch screen again?'))return;await fetch('/api/recal',{method:'POST'});$('sysmsg').textContent='Restarting\u2026'};
$('settime').onclick=async()=>{await fetch('/api/settime?epoch='+Math.floor(Date.now()/1000),{method:'POST'});$('tmsg').textContent='time set \u2713';load()};
$('chest').onclick=()=>fetch('/api/chest',{method:'POST'});
$('diver').onclick=()=>fetch('/api/diver',{method:'POST'});
$('feed').onclick=()=>fetch('/api/feed',{method:'POST'});
$('hsave').onclick=async()=>{const h=$('host').value.trim().toLowerCase();
 const r=await fetch('/api/host?host='+encodeURIComponent(h),{method:'POST'});
 if(!r.ok){$('hmsg').textContent=(await r.json()).error;return}
 $('hmsg').textContent='Rebooting\u2026';clearInterval(window.poll);
 const cands=[location.origin,'http://'+h+'.local'];
 setTimeout(()=>{window.poll=setInterval(async()=>{for(const o of cands){try{const s=await (await fetch(o+'/api/status',{cache:'no-store'})).json();
  if(s.host===h){clearInterval(window.poll);location.href=o+'/';return}}catch(e){}}},2000)},4000)};
load();setInterval(load,5000);
</script></main></body></html>)HTML";


// ---- restart diagnostics: a breadcrumb in RTC memory survives watchdog / crash resets; at boot it is turned into a
// line of the (persistent) restart log, shown on the web page and at /api/resetlog.
#define CRUMB_MAGIC 0xF15C7A11u
enum Stage : uint8_t { ST_START, ST_NET, ST_BUTTON, ST_TOUCH, ST_SLEEP, ST_WORLD, ST_RENDER, ST_PUSH, ST_IDLE };
static const char *stageName[] = {"start", "network", "button", "touch", "sleep/night", "world", "render", "display push", "idle"};
struct Crumb { uint32_t magic, up, epoch, heap, minHeap, maxLoop; uint8_t stage, wifi; int8_t rssi; };
RTC_NOINIT_ATTR static Crumb crumb;
#define STAGE(x) (crumb.stage = (x))

static const char *resetKind(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_PANIC: return "CRASH (panic)";
    case ESP_RST_INT_WDT: return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_BROWNOUT: return "brownout";
    default: return nullptr;
  }
}

static void logPreviousCrash() {
  esp_reset_reason_t r = esp_reset_reason();
  const char *kind = resetKind(r);
  if (kind && crumb.magic == CRUMB_MAGIC) {
    char when[32] = "time unknown";
    if (crumb.epoch > 1700000000UL) {
      time_t e = crumb.epoch; struct tm t; localtime_r(&e, &t);
      strftime(when, sizeof(when), "%a %m/%d %I:%M %p", &t);
    }
    char line[200];
    snprintf(line, sizeof(line), "%s | %s | in %s | up %lu s | heap %lu KB (min %lu) | wifi %s rssi %d | slowest loop %lu ms",
             when, kind, crumb.stage < 9 ? stageName[crumb.stage] : "?", (unsigned long)crumb.up, (unsigned long)crumb.heap / 1024,
             (unsigned long)crumb.minHeap / 1024, crumb.wifi == WL_CONNECTED ? "up" : "DOWN", crumb.rssi, (unsigned long)crumb.maxLoop);
    String log = prefs.getString("rlog", "");
    log = String(line) + "\n" + log;
    while (log.length() > 2400) { int k = log.lastIndexOf('\n', log.length() - 2); if (k < 0) break; log = log.substring(0, k + 1); }
    prefs.putString("rlog", log);
    prefs.putUInt("ncrash", prefs.getUInt("ncrash", 0) + 1);
  }
  memset(&crumb, 0, sizeof(crumb));
  crumb.magic = CRUMB_MAGIC;
}

static void updateCrumb(uint32_t loopMs) {
  static uint32_t last = 0, worst = 0;
  if (loopMs > worst) worst = loopMs;
  uint32_t now = millis();
  if (now - last < 1000) return;
  last = now;
  crumb.up = now / 1000;
  time_t e = time(nullptr); crumb.epoch = e > 1700000000 ? (uint32_t)e : 0;
  crumb.heap = ESP.getFreeHeap(); crumb.minHeap = ESP.getMinFreeHeap();
  crumb.wifi = (uint8_t)WiFi.status(); crumb.rssi = (int8_t)WiFi.RSSI();
  if (worst > crumb.maxLoop) crumb.maxLoop = worst;
  worst = 0;
}

static const char *resetReasonShort() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "power-on";
    case ESP_RST_EXT: return "reset button";
    case ESP_RST_SW: return "restart";
    case ESP_RST_PANIC: return "CRASH";
    case ESP_RST_INT_WDT: return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_BROWNOUT: return "brownout";
    default: return "other";
  }
}

// The boards have no battery clock. Time comes from NTP, from a device's browser (web page) or, after a restart,
// from the last time we saved (so the clock is roughly right immediately).
static bool timeIsSet() { struct tm t; return getLocalTime(&t, 0); }
static void saveTimeNow() { if (timeIsSet()) prefs.putUInt("lastt", (uint32_t)time(nullptr)); }
static void setClockEpoch(uint32_t epoch) {
  struct timeval tv = {(time_t)epoch, 0};
  settimeofday(&tv, nullptr);
  saveTimeNow();
}
static bool ntpSynced() { return sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED; }

static String statusJson() {
  String ssid = WiFi.status() == WL_CONNECTED ? WiFi.SSID() : String("");
  ssid.replace("\\", ""); ssid.replace("\"", "");
  char b[1200];
  char tbuf[16], dbuf[24];
  clockStrings(tbuf, sizeof(tbuf), dbuf, sizeof(dbuf));
  snprintf(b, sizeof(b), "{\"crashes\":%u,\"night\":%s,\"nstart\":\"%02d:%02d\",\"nend\":\"%02d:%02d\",\"isnight\":%s,\"rst\":\"%s\",\"timeset\":%s,\"ntp\":%s,\"tz\":\"%s\",\"dfmt\":%d,\"now\":\"%s  %s\",\"env\":\"" BOARD_ID "\",\"board\":\"%s\",\"inv\":%s,\"swaprb\":%s,\"motion\":%s,\"mmin\":%d,\"msens\":%d,\"mon\":%s,\"mready\":%s,\"mscore\":%.2f,\"asleep\":%s,\"h24\":%s,\"ssid\":\"%s\",\"snd\":%s,\"vol\":%d,\"rot\":%d,\"auto\":%s,\"aint\":%d,\"host\":\"%s\",\"ver\":\"" FW_VERSION "\",\"ip\":\"%s\",\"rssi\":%d,\"up\":%lu,\"fps\":%.1f}",
           (unsigned)prefs.getUInt("ncrash", 0), g_night ? "true" : "false", g_nstart / 60, g_nstart % 60, g_nend / 60, g_nend % 60, g_isNight ? "true" : "false",
           resetReasonShort(), timeIsSet() ? "true" : "false", ntpSynced() ? "true" : "false",
           g_tz.c_str(), g_datefmt, tbuf, dbuf,
#ifdef BOARD_CYD28
           "cyd",
#else
           "freenove",
#endif
           g_invert ? "true" : "false", g_swaprb ? "true" : "false",
           g_motion ? "true" : "false", g_mmin, g_msens, motionDetected() ? "true" : "false", motionReady() ? "true" : "false", motionScore(), g_asleep ? "true" : "false",
           g_h24 ? "true" : "false", ssid.c_str(), g_snd ? "true" : "false", (int)g_vol, g_rot, g_auto ? "true" : "false", g_aint, g_host.c_str(), WiFi.localIP().toString().c_str(), WiFi.RSSI(), millis() / 1000UL, g_fps);
  String o(b);
  o.remove(o.length() - 1);
  char a[200];
  snprintf(a, sizeof(a), ",\"sbub\":%s,\"sdiv\":%s,\"sfeed\":%s,\"alon\":%s,\"atime\":\"%02d:%02d\",\"adays\":%d,\"asnz\":%d,\"avol\":%d,\"alstate\":%d,\"almiss\":%s}",
           g_sBub ? "true" : "false", g_sDiv ? "true" : "false", g_sFeed ? "true" : "false",
           g_alOn ? "true" : "false", g_alMin / 60, g_alMin % 60, g_alDays, g_alSnz, g_alVol, (int)alState, alMissed ? "true" : "false");
  return o + a;
}

static void drawScene(float t, uint32_t now);
#ifdef BOARD_CYD28
static void renderStrip(int i) {
  cv.s = &strip; cv.oy = i * STRIP_H;
  renderBackground(cv);
  drawScene(curT, curNow);
}
#endif

// A numeric request argument, only if it is present and not empty (an empty value must not reset a setting to its minimum).
static bool numArg(const char *name, int lo, int hi, int &out) {
  if (!server.hasArg(name)) return false;
  String v = server.arg(name); v.trim();
  if (!v.length()) return false;
  out = constrain((int)v.toInt(), lo, hi);
  return true;
}

extern const uint8_t favicon_start[] asm("_binary_src_favicon_ico_start");
extern const uint8_t favicon_end[] asm("_binary_src_favicon_ico_end");

static void setupWeb() {
  server.on("/", HTTP_GET, []() {
    String p = FPSTR(PAGE);
    p.replace("@L@", String(ROT_LEFT)); p.replace("@R@", String(ROT_RIGHT));
    server.send(200, "text/html", p);
  });
  server.on("/favicon.ico", HTTP_GET, []() {
    server.sendHeader("Cache-Control", "max-age=86400");
    server.send_P(200, "image/x-icon", (PGM_P)favicon_start, favicon_end - favicon_start);
  });
  server.on("/api/status", HTTP_GET, []() {
    server.sendHeader("Access-Control-Allow-Origin", "*");      // lets the page find the board under a new hostname
    server.send(200, "application/json", statusJson());
  });
  server.on("/api/host", HTTP_ANY, []() {
    String h = server.arg("host"); h.trim(); h.toLowerCase();
    bool ok = h.length() >= 1 && h.length() <= 31 && h[0] != '-' && h[h.length() - 1] != '-';
    for (size_t i = 0; ok && i < h.length(); i++) ok = isalnum((unsigned char)h[i]) || h[i] == '-';
    if (!ok) { server.send(400, "application/json", "{\"error\":\"letters, digits and - only (1-31 chars)\"}"); return; }
    prefs.putString("host", h);
    server.send(200, "application/json", "{\"ok\":true,\"host\":\"" + h + "\"}");
    restartAt = millis() + 800;
  });
  server.on("/api/set", HTTP_ANY, []() {
    int v;
    if (numArg("vol", 0, 100, v)) g_vol = v;
    if (numArg("snd", 0, 1, v)) g_snd = v != 0;
    if (numArg("sbub", 0, 1, v)) g_sBub = v != 0;
    if (numArg("sdiv", 0, 1, v)) g_sDiv = v != 0;
    if (numArg("sfeed", 0, 1, v)) g_sFeed = v != 0;
    if (numArg("motion", 0, 1, v)) g_motion = v != 0;
    if (numArg("mmin", 1, 240, v)) g_mmin = v;
    if (numArg("msens", 1, 10, v)) g_msens = v;
    if (numArg("h24", 0, 1, v)) g_h24 = v != 0;
    if (numArg("auto", 0, 1, v)) g_auto = v != 0;
    if (numArg("aint", 5, 3600, v)) g_aint = v;
    if (numArg("dfmt", 0, 4, v)) g_datefmt = v;
    if (numArg("night", 0, 1, v)) g_night = v != 0;
    if (numArg("alon", 0, 1, v)) { g_alOn = v != 0; alFiredDay = -1; }
    if (numArg("adays", 0, 127, v)) g_alDays = v;
    if (numArg("asnz", 1, 30, v)) g_alSnz = v;
    if (numArg("avol", 1, 100, v)) g_alVol = v;
    { String s = server.arg("atime"); s.trim(); int c = s.indexOf(':');
      if (c > 0) { int h = s.substring(0, c).toInt(), mi = s.substring(c + 1).toInt(); if (h >= 0 && h < 24 && mi >= 0 && mi < 60) { g_alMin = h * 60 + mi; alFiredDay = -1; } } }
    for (int k = 0; k < 2; k++) {
      String s = server.arg(k ? "nend" : "nstart"); s.trim();
      int c = s.indexOf(':');
      if (c > 0) { int h = s.substring(0, c).toInt(), mi = s.substring(c + 1).toInt(); if (h >= 0 && h < 24 && mi >= 0 && mi < 60) (k ? g_nend : g_nstart) = h * 60 + mi; }
    }
    if (server.hasArg("tz")) {
      String z = server.arg("tz"); z.trim();
      bool ok = z.length() >= 3 && z.length() <= 47;
      for (size_t i = 0; ok && i < z.length(); i++) { char c = z[i]; ok = isalnum((unsigned char)c) || strchr(",.:/+-", c); }
      if (ok && z != g_tz) { g_tz = z; setenv("TZ", g_tz.c_str(), 1); tzset(); }
    }
    bool colorsChanged = false;
    if (numArg("inv", 0, 1, v)) { bool b = v != 0; colorsChanged |= b != g_invert; g_invert = b; }
    if (numArg("swaprb", 0, 1, v)) { bool b = v != 0; colorsChanged |= b != g_swaprb; g_swaprb = b; }
    if (numArg("rot", 0, 3, v)) {
      if ((v == 1 || v == 3) && v != g_rot) { g_rot = v; halApplyRotation(); colorsChanged = false; }
    }
    if (colorsChanged) halApplyColors();
    saveSettings(); halApplySound();
    server.send(200, "application/json", statusJson());
  });
  // Raw copy of the screen (RGB565, big-endian) for the page's screenshot button.
  server.on("/api/frame", HTTP_GET, []() {
    const size_t total = PANEL_W * PANEL_H * 2;
    server.setContentLength(total);
    server.sendHeader("Cache-Control", "no-store");
    server.sendHeader("X-W", String(PANEL_W));
    server.sendHeader("X-H", String(PANEL_H));
    server.send(200, "application/octet-stream", "");
#ifdef BOARD_CYD28
    for (int i = 0; i < PANEL_H / STRIP_H; i++) {
      renderStrip(i);
      server.sendContent((const char *)strip.getPointer(), PANEL_W * STRIP_H * 2);
    }
#else
    const uint8_t *p = (const uint8_t *)frame.getPointer();
    for (size_t off = 0; off < total; off += 4096) server.sendContent((const char *)p + off, min((size_t)4096, total - off));
#endif
  });
  server.on("/api/motionlog", HTTP_GET, []() { server.send(200, "application/json", motionLogJson()); });
  server.on("/api/motionlog/clear", HTTP_ANY, []() { motionLogClear(); server.send(200, "application/json", "{\"ok\":true}"); });
  server.on("/api/feed", HTTP_ANY, []() {
    feedAt(50 + random(VW - 100), 16, 5);
    server.send(200, "application/json", "{\"ok\":true}");
  });
  server.on("/api/settime", HTTP_ANY, []() {
    uint32_t e = (uint32_t)server.arg("epoch").toInt();
    if (e < 1700000000UL || e > 4000000000UL) { server.send(400, "application/json", "{\"error\":\"bad time\"}"); return; }
    setClockEpoch(e);
    server.send(200, "application/json", statusJson());
  });
#ifdef CRASH_TEST
  server.on("/api/crashtest", HTTP_ANY, []() {      // debug builds only: proves the restart log works
    server.send(200, "text/plain", "crashing");
    delay(200);
    STAGE(ST_RENDER);
    portDISABLE_INTERRUPTS();
    while (true) {}
  });
#endif
  server.on("/api/reboot", HTTP_ANY, []() { server.send(200, "text/plain", "rebooting"); restartAt = millis() + 500; });
  server.on("/api/mdbg", HTTP_ANY, []() {      // motion debug mode (see motion.cpp); toggle motion off/on to apply
    int v; if (numArg("mode", 0, 4, v)) g_motionDbg = v;
    server.send(200, "text/plain", String("motion debug mode ") + (int)g_motionDbg);
  });
  server.on("/api/resetlog", HTTP_ANY, []() {
    if (server.hasArg("clear")) prefs.putString("rlog", "");
    server.send(200, "text/plain", prefs.getString("rlog", ""));
  });
  server.on("/api/alarm", HTTP_ANY, []() {         // cmd=test (8 s sunrise, then ringing) or cmd=stop
    String c = server.arg("cmd");
    if (c == "test") alTest(millis()); else if (c == "stop") alStop();
    server.send(200, "text/plain", "ok");
  });
  server.on("/api/bub", HTTP_ANY, []() { audioBubMode(server.arg("m").length() ? server.arg("m").toInt() : 1, server.arg("g").toInt()); server.send(200, "text/plain", "ok"); });   // CYD bubbles on/off and gain
  server.on("/api/lamb", HTTP_ANY, []() { audioLamb(); server.send(200, "text/plain", "ok"); });   // speaker test tune
  server.on("/api/diver", HTTP_ANY, []() {         // send the diver out right now (opens the chest too)
    static bool nextRight = true;
    if (!diver.on) {
      diver = {0, true, nextRight}; nextRight = !nextRight; audioCash();
      if (chestState == CHEST_CLOSED) { chestState = CHEST_OPENING; chestT = 0; }
    }
    server.send(200, "text/plain", "ok");
  });
  server.on("/api/chest", HTTP_ANY, []() {
    if (chestState == CHEST_CLOSED) { chestState = CHEST_OPENING; chestT = 0; }
    server.send(200, "application/json", "{\"ok\":true}");
  });
  server.on("/api/wifi/forget", HTTP_ANY, []() {
    wifiForgetAll();
    server.send(200, "application/json", "{\"ok\":true}");
    restartAt = millis() + 800;
  });
  server.on("/api/recal", HTTP_ANY, []() {
    server.send(200, "application/json", "{\"ok\":true}");
    delay(300);
    halRecalibrateTouch();
  });
  // Firmware update page at /update (ElegantOTA, the same one CYD-PhotoFrame uses). The upload is handled inside
  // server.handleClient(), so the tank is paused while it runs; progress is drawn on the screen as well.
#ifdef OTA_PASS
  ElegantOTA.begin(&server, "admin", OTA_PASS);   // same password as ArduinoOTA; user name is "admin"
#else
  ElegantOTA.begin(&server);
#endif
  ElegantOTA.onStart([]() { tft.fillScreen(TFT_BLACK); tft.setTextColor(TFT_WHITE); tft.drawString("Firmware update...", 10, 10, 4); });
  ElegantOTA.onProgress([](size_t cur, size_t total) {
    static uint32_t lastDraw = 0;
    if (!total || (millis() - lastDraw < 150 && cur < total)) return;   // each repaint costs SPI time, so throttle
    lastDraw = millis();
    tft.fillRect(10, 60, PANEL_W - 20, 20, TFT_DARKGREY);
    tft.fillRect(10, 60, (int)((PANEL_W - 20) * (float)cur / total), 20, TFT_GREEN);
  });
  server.begin();
}

static void startServices() {
  g_online = true;
  WiFi.setAutoReconnect(true);
  configTzTime(g_tz.c_str(), "pool.ntp.org", "time.nist.gov");
  ArduinoOTA.setHostname(g_host.c_str());
#ifdef OTA_PASS
  ArduinoOTA.setPassword(OTA_PASS);
#endif
  ArduinoOTA.onStart([]() { tft.fillScreen(TFT_BLACK); tft.setTextColor(TFT_WHITE); tft.drawString("OTA update...", 10, 10, 4); });
  ArduinoOTA.onProgress([](unsigned int p, unsigned int total) {
    tft.fillRect(10, 60, PANEL_W - 20, 20, TFT_DARKGREY);
    tft.fillRect(10, 60, (int)((PANEL_W - 20) * (float)p / total), 20, TFT_GREEN);
  });
  ArduinoOTA.begin();
  MDNS.begin(g_host.c_str());
  MDNS.addService("http", "tcp", 80);
  setupWeb();
  Serial.printf("WiFi connected, IP %s, host %s\n", WiFi.localIP().toString().c_str(), g_host.c_str());
}

// BOOT button shows connection info for a while
static uint32_t infoUntil = 0;
static void drawInfo(uint32_t now) {
  cv.fillRoundRect(30, 50, 420, 210, 12, RGB(8, 28, 52));
  cv.drawRoundRect(30, 50, 420, 210, 12, RGB(120, 190, 240));
  cv.setTextDatum(TL_DATUM);
  cv.setTextColor(RGB(255, 255, 255));
  cv.drawString("Fish Tank  v" FW_VERSION, 46, 60, FONT_BIG);
  char b[96];
  cv.setTextColor(RGB(170, 220, 255));
  snprintf(b, sizeof(b), "http://%s.local", g_host.c_str());
  cv.drawString(b, 46, 100, FONT_BIG);
  snprintf(b, sizeof(b), "http://%s", WiFi.localIP().toString().c_str());
  cv.drawString(b, 46, 132, FONT_BIG);
  cv.setTextColor(RGB(220, 235, 250));
  snprintf(b, sizeof(b), "WiFi: %s  (%d dBm)", WiFi.status() == WL_CONNECTED ? WiFi.SSID().c_str() : "not connected", WiFi.RSSI());
  cv.drawString(b, 46, 172, FONT_SMALL);
  snprintf(b, sizeof(b), "Up %lu min  Reset: %s  %.1f fps", millis() / 60000UL, resetReasonShort(), g_fps);
  cv.drawString(b, 46, 194, FONT_SMALL);
  snprintf(b, sizeof(b), "Sound %s, vol %d   %s", g_snd ? "on" : "off", (int)g_vol, BOARD_NAME);
  cv.drawString(b, 46, 216, FONT_SMALL);
  cv.setTextColor(RGB(140, 170, 200));
  snprintf(b, sizeof(b), "Press BOOT to close (%lu s)", (unsigned long)((infoUntil - now) / 1000));
  cv.drawString(b, 46, 238, FONT_SMALL);
}

// Everything that moves, drawn over the background already in the canvas.
static void drawChest() {
  const float p = chestOpenness();
  const float x = CHEST_X, top = CHEST_BASE - 20;
  const uint16_t wood = RGB(135, 82, 36), woodD = RGB(88, 50, 22), gold = RGB(232, 188, 62), goldL = RGB(252, 220, 100);
  // lid: a dome when closed, lifted and flattened when open
  const float lidY = top - 12 * p - 1, lidRy = 12 * (1 - p) + 4, lidRx = 27 - 3 * p;
  cv.fillEllipse(x, lidY, lidRx, lidRy, wood);
  cv.drawEllipse(x, lidY, lidRx, lidRy, woodD);
  cv.fillRect(x - 19, lidY - lidRy + 1, 4, lidRy * 2 - 3, gold);
  cv.fillRect(x + 15, lidY - lidRy + 1, 4, lidRy * 2 - 3, gold);
  // body
  cv.fillRect(x - 27, top, 54, 20, wood);
  cv.fillRect(x - 27, top + 6, 54, 1, woodD);
  cv.fillRect(x - 27, top + 13, 54, 1, woodD);
  cv.fillRect(x - 27, top, 54, 2, woodD);
  cv.fillRect(x - 27, top + 19, 54, 1, woodD);
  cv.fillRect(x - 19, top, 4, 20, gold);
  cv.fillRect(x + 15, top, 4, 20, gold);
  // dark opening with a gold glow while open
  if (p > 0.05f) {
    cv.fillEllipse(x, top + 1, 25, 7 * p + 1, RGB(25, 12, 5));
    if (!chestLooted) {
      cv.fillEllipse(x, top, 17 * p, 4 * p + 1, RGB(255, 214, 95));
      cv.fillEllipse(x - 6, top - 1, 4 * p + 1, 2 * p + 1, goldL);
      cv.fillEllipse(x + 7, top, 4 * p + 1, 2 * p + 1, goldL);
    }
  }
  // lock
  cv.fillRect(x - 4, top + 7, 8, 8, gold);
  cv.fillRect(x - 1, top + 10, 2, 3, woodD);
}

static void drawDiver(float t) {
  if (!diver.on) return;
  float x, y, x2, y2;
  diverPos(diver.t, x, y);
  diverPos(diver.t + 0.1f, x2, y2);
  float dx = x2 - x, dy = y2 - y, l = sqrtf(dx * dx + dy * dy);
  if (l < 0.001f) return;
  dx /= l; dy /= l;
  const float nx = diver.right ? dy : -dy, ny = diver.right ? -dx : dx;   // toward the diver's back (up, whichever way he swims)
  auto P = [&](float u, float v, float &ox, float &oy) { ox = x + dx * u + nx * v; oy = y + dy * u + ny * v; };
  const uint16_t suit = RGB(25, 40, 70), suitL = RGB(50, 80, 125), tank = RGB(225, 190, 60), skin = RGB(240, 200, 160);
  const uint16_t fin = RGB(235, 120, 30), mask = RGB(150, 215, 245), bagC = RGB(150, 100, 45);
  float ax, ay, bx, by, kick = sinf(t * 7.0f) * 5.0f;
  // flippers (behind the legs)
  P(-16, 0, ax, ay);
  P(-27, 3 + kick, bx, by);  cv.drawWideLine(ax, ay, bx, by, 5, fin, fin);
  P(-27, -3 - kick, bx, by); cv.drawWideLine(ax, ay, bx, by, 5, fin, fin);
  // air tank on the back
  P(-2, 6, ax, ay); P(7, 6, bx, by);
  cv.drawWideLine(ax, ay, bx, by, 5, tank, tank);
  // torso + legs
  P(-17, 0, ax, ay); P(6, 0, bx, by);
  cv.drawWideLine(ax, ay, bx, by, 7, suit, suit);
  P(-2, 3, ax, ay); P(5, 3, bx, by);
  cv.drawWideLine(ax, ay, bx, by, 2, suitL, suitL);
  // head with mask
  float hx, hy;
  P(10, 0, hx, hy);
  cv.fillCircle(hx, hy, 5, skin);
  P(12, -1, ax, ay);
  cv.fillCircle(ax, ay, 3, mask);
  P(10, -4, ax, ay);
  cv.drawLine(hx, hy, ax, ay, suit);
  // arm holding the gold bag out front and below
  float ax2, ay2;
  P(4, -3, ax, ay); P(11, -9, ax2, ay2);
  cv.drawWideLine(ax, ay, ax2, ay2, 3, suit, suit);
  if (diver.t < DIVER_BAG) return;                // empty-handed until he takes the bag
  float gx = ax2 + 2 * sinf(t * 4.0f), gy = ay2 + 7 + 2 * cosf(t * 4.0f);
  cv.fillCircle(gx, gy, 9, bagC);
  cv.fillTriangle(gx - 5, gy - 6, gx + 5, gy - 6, gx, gy - 13, bagC);
  cv.fillCircle(gx, gy, 7.5f, RGB(240, 196, 70));
  cv.fillCircle(gx - 2, gy - 2, 3, RGB(255, 228, 120));
  cv.fillRect(gx - 5, gy - 7, 10, 2, RGB(232, 188, 62));
}

// ---------------------------------------------------------------- alarm clock drawing
static void seg7(float x, float y, float h, int d, uint16_t c) {       // one 7-segment digit, top-left x,y
  static const uint8_t M[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};
  const float w = h * 0.55f, t = h * 0.13f, hh = h / 2, v = hh - 1.5f * t;
  const uint8_t m = M[d];
  if (m & 1)  cv.fillRect(x + t, y, w - 2 * t, t, c);
  if (m & 2)  cv.fillRect(x + w - t, y + t, t, v, c);
  if (m & 4)  cv.fillRect(x + w - t, y + hh + t / 2, t, v, c);
  if (m & 8)  cv.fillRect(x + t, y + h - t, w - 2 * t, t, c);
  if (m & 16) cv.fillRect(x, y + hh + t / 2, t, v, c);
  if (m & 32) cv.fillRect(x, y + t, t, v, c);
  if (m & 64) cv.fillRect(x + t, y + hh - t / 2, w - 2 * t, t, c);
}
static void digitsGeom(float cx, float h, float &x0, float &hC, float &mC) {
  const float w = h * 0.55f, gap = h * 0.18f, cw = h * 0.22f, W = 4 * w + cw + 4 * gap;
  x0 = cx - W / 2;
  hC = x0 + (2 * w + gap) / 2;
  mC = x0 + 2 * w + gap + gap + cw + gap + (2 * w + gap) / 2;
}
static void drawDigits(float cx, float y, float h, int hh, int mm, bool lead, bool colon, uint16_t c) {
  float x0, hC, mC;
  digitsGeom(cx, h, x0, hC, mC);
  const float w = h * 0.55f, gap = h * 0.18f, cw = h * 0.22f;
  float x = x0;
  if (lead || hh >= 10) seg7(x, y, h, hh / 10, c);
  x += w + gap; seg7(x, y, h, hh % 10, c);
  x += w + gap;
  if (colon) { const float s = h * 0.12f; cv.fillRect(x + (cw - s) / 2, y + h * 0.3f, s, s, c); cv.fillRect(x + (cw - s) / 2, y + h * 0.65f, s, s, c); }
  x += cw + gap; seg7(x, y, h, mm / 10, c);
  x += w + gap; seg7(x, y, h, mm % 10, c);
}

// Warm sunrise wash over the whole scene, stronger at the top.
static void alarmTint(float amt) {
  uint16_t *px = (uint16_t *)cv.s->getPointer();
  const int w = cv.s->width(), hgt = cv.s->height();
  for (int y = 0; y < hgt; y++) {
    const float wy = (y + cv.oy) / cv.sy;
    const int k = (int)(amt * 200.0f * (1.0f - 0.5f * wy / VH));
    if (k <= 0) continue;
    uint16_t *row = px + (size_t)y * w;
    for (int x = 0; x < w; x++) {
      const uint16_t v = bswap16(row[x]);
      int r = v >> 11, g = (v >> 5) & 63, b = v & 31;
      r += ((31 - r) * k) >> 8; g += ((50 - g) * k) >> 8; b += ((12 - b) * k) >> 8;
      row[x] = bswap16((r << 11) | (g << 5) | b);
    }
  }
}

#define BTN_SNZ_X 60
#define BTN_STOP_X 250
#define BTN_Y 232
#define BTN_W 170
#define BTN_H 58

static void drawAlarmScreen(float t, uint32_t now) {
  const float p = alProgress(now);
  const float pulse = alState == AL_RING ? 0.5f + 0.5f * sinf(t * 4.0f) : 0;
  alarmTint(min(1.0f, (0.3f + 0.7f * p) * (1.0f + 0.12f * pulse)));
  if (alState != AL_RING) { drawClock(); return; }       // the sunrise lead-in is just the glow; the big clock and buttons come with the sound
  char buf[16], date[24];
  clockStrings(buf, sizeof(buf), date, sizeof(date));
  const int h12 = atoi(buf);
  const char *col = strchr(buf, ':');
  const int mm = col ? atoi(col + 1) : 0;
  const char *ap = col ? strchr(col, ' ') : nullptr;
  const bool colon = (now / 500) % 2 == 0;
  drawDigits(VW / 2 + 2, 82, 78, h12, mm, g_h24, true, RGB(120, 60, 10));
  drawDigits(VW / 2, 80, 78, h12, mm, g_h24, colon, RGB(255, 244, 205));
  cv.setTextDatum(TC_DATUM);
  if (ap) { cv.setTextColor(RGB(255, 244, 205)); cv.drawString(ap + 1, VW / 2, 166, 4); }
  if (alState == AL_RING) { cv.setTextColor(RGB(255, 255, 255)); cv.drawString("Time to get up!", VW / 2, 28, 4); }
  cv.fillRoundRect(BTN_SNZ_X, BTN_Y, BTN_W, BTN_H, 12, RGB(40, 90, 150));
  cv.fillRoundRect(BTN_STOP_X, BTN_Y, BTN_W, BTN_H, 12, RGB(190, 40, 40));
  cv.drawRoundRect(BTN_SNZ_X, BTN_Y, BTN_W, BTN_H, 12, RGB(190, 220, 255));
  cv.drawRoundRect(BTN_STOP_X, BTN_Y, BTN_W, BTN_H, 12, RGB(255, 190, 190));
  cv.setTextColor(RGB(255, 255, 255));
  cv.setTextDatum(MC_DATUM);
  char sb[16];
  snprintf(sb, sizeof(sb), "%d min", g_alSnz);
  cv.drawString("Snooze", BTN_SNZ_X + BTN_W / 2, BTN_Y + 20, 4);
  cv.drawString(sb, BTN_SNZ_X + BTN_W / 2, BTN_Y + 43, 2);
  cv.drawString("Stop", BTN_STOP_X + BTN_W / 2, BTN_Y + BTN_H / 2, 4);
}

// On-screen alarm setter: tap the time in the corner.
#define PANEL_DIG_H 56
static void drawAlarmPanel() {
  const uint16_t bg = RGB(10, 30, 60), fg = RGB(235, 245, 255), acc = RGB(120, 190, 240);
  cv.fillRoundRect(70, 22, 340, 276, 14, bg);
  cv.drawRoundRect(70, 22, 340, 276, 14, acc);
  cv.setTextDatum(TC_DATUM);
  cv.setTextColor(fg);
  cv.drawString("Alarm clock", 240, 32, 4);
  float x0, hC, mC;
  digitsGeom(240, PANEL_DIG_H, x0, hC, mC);
  const int h24 = g_alMin / 60, mm = g_alMin % 60;
  const int hs = g_h24 ? h24 : (h24 % 12 == 0 ? 12 : h24 % 12);
  drawDigits(240, 110, PANEL_DIG_H, hs, mm, g_h24, true, g_alOn ? RGB(255, 214, 95) : RGB(130, 150, 175));
  const float cx[2] = {hC, mC};
  for (int i = 0; i < 2; i++) {
    cv.fillTriangle(cx[i] - 16, 100, cx[i] + 16, 100, cx[i], 76, acc);
    cv.fillTriangle(cx[i] - 16, 178, cx[i] + 16, 178, cx[i], 202, acc);
  }
  if (!g_h24) {
    cv.fillRoundRect(345, 126, 56, 36, 8, RGB(40, 90, 150));
    cv.setTextDatum(MC_DATUM);
    cv.drawString(h24 < 12 ? "AM" : "PM", 373, 144, 4);
  }
  cv.setTextDatum(MC_DATUM);
  cv.fillRoundRect(90, 244, 130, 42, 10, g_alOn ? RGB(30, 130, 70) : RGB(90, 100, 115));
  cv.drawString(g_alOn ? "Alarm ON" : "Alarm OFF", 155, 265, 4);
  cv.fillRoundRect(260, 244, 130, 42, 10, RGB(40, 90, 150));
  cv.drawString("Done", 325, 265, 4);
  cv.setTextColor(RGB(150, 180, 210));
  cv.drawString("days and loudness: web page", 240, 224, 2);
}

// Touch while the panel is open. Arrows repeat while held; the buttons act once.
static void alPanelTouch(float wx, float wy, bool press, bool rep, uint32_t now) {
  alPanelAt = now;
  auto in = [&](float x, float y, float w, float h) { return wx >= x && wx < x + w && wy >= y && wy < y + h; };
  float x0, hC, mC;
  digitsGeom(240, PANEL_DIG_H, x0, hC, mC);
  const int hh = g_alMin / 60, mi = g_alMin % 60;
  bool changed = false;
  if (press || rep) {
    if (in(hC - 38, 66, 76, 52))       { g_alMin = ((hh + 1) % 24) * 60 + mi; changed = true; }
    else if (in(hC - 38, 160, 76, 52)) { g_alMin = ((hh + 23) % 24) * 60 + mi; changed = true; }
    else if (in(mC - 38, 66, 76, 52))  { g_alMin = hh * 60 + (mi + 1) % 60; changed = true; }
    else if (in(mC - 38, 160, 76, 52)) { g_alMin = hh * 60 + (mi + 59) % 60; changed = true; }
  }
  if (press) {
    if (!g_h24 && in(340, 120, 66, 48)) { g_alMin = (g_alMin + 720) % 1440; changed = true; }
    else if (in(90, 244, 130, 42)) { g_alOn = !g_alOn; alFiredDay = -1; }
    else if (in(260, 244, 130, 42)) { alPanel = false; saveSettings(); }
  }
  if (changed) { g_alOn = true; alFiredDay = -1; }
}

static void drawScene(float t, uint32_t now) {
  drawRays(t);
  for (auto &k : kelp) {
    float px = k.x, py = SAND_Y + 28;
    for (int i = 3; i <= k.h; i += 12) {
      float nx = k.x + 9 * sinf(i / 22.0f + k.ph - t * 1.2f), ny = SAND_Y + 28 - i;
      cv.drawWideLine(px, py, nx, ny, 4, RGB(30, 150, 80), RGB(30, 150, 80));
      px = nx; py = ny;
    }
  }
  drawChest();
  drawDiver(t);
  for (int i = 0; i < NFISH; i++) {
    Fish &f = fishes[i];
    float wag = sinf(t * 9.0f + f.ph) * 4.0f;
    float fin = sinf(t * 6.0f + f.ph) * 4.0f;
    cv.fl = alState == AL_RAMP; cv.fc = f.y;          // the minute before the alarm the fish swim upside down
    switch (f.type) {
      case TANG:  drawTang(f.x, f.y, f.dir, wag); break;
      case CLOWN: drawClown(f.x, f.y, f.dir, wag); break;
      case ANGEL: drawAngel(f.x, f.y, f.dir, wag, fin); break;
      case GOLD:  drawGold(f.x, f.y, f.dir, wag); break;
      case NEON:  drawNeon(f.x, f.y, f.dir, wag * .6f); break;
    }
    cv.fl = false;
  }
  for (auto &fd : food) if (fd.on) cv.fillCircle(fd.x, fd.y, 2, RGB(235, 200, 120));
  for (auto &r : rings) {
    if (!r.on) continue;
    for (int k = 0; k < 2; k++) {
      float a = r.age - k * 0.25f;
      if (a <= 0 || a > 1.4f) continue;
      float b = 1.0f - a / 1.4f;
      uint16_t c = RGB((int)(60 + 170 * b), (int)(120 + 120 * b), (int)(170 + 85 * b));
      float rad = a * 55;
      cv.drawCircle(r.x, r.y, rad, c);
      cv.drawCircle(r.x, r.y, rad + 1, c);
    }
  }
  for (auto &b : bubbles) {
    float bx = b.x + 5 * sinf(t * b.v * 0.08f + b.ph), by = b.y, r = b.r;
    cv.drawCircle(bx, by, r, RGB(205, 232, 255));
    cv.drawPixel(bx - r / 3, by - r / 3, TFT_WHITE);
  }
  for (auto &b : cbub) {
    if (!b.on) continue;
    float bx = b.x + 6 * sinf(t * 3.0f + b.ph + b.y * 0.05f), by = b.y;
    cv.drawCircle(bx, by, b.r, RGB(205, 232, 255));
    cv.drawPixel(bx - b.r / 3, by - b.r / 3, TFT_WHITE);
  }
  if (alActive()) drawAlarmScreen(t, now); else drawClock();
  if (alPanel) drawAlarmPanel();
  if (infoUntil > now) drawInfo(now);
}

void setup() {
  Serial.begin(115200);
  prefs.begin("tank", false);
  g_rot = prefs.getInt("rot", DEFAULT_ROT); g_snd = prefs.getBool("snd", true); g_sBub = prefs.getBool("sbub", true); g_sDiv = prefs.getBool("sdiv", true); g_sFeed = prefs.getBool("sfeed", true); g_vol = prefs.getInt("vol", 60);
  g_auto = prefs.getBool("afeed", false); g_aint = prefs.getInt("aint", 60); g_h24 = prefs.getBool("h24", false); g_tz = prefs.getString("tz", g_tz); g_datefmt = constrain(prefs.getInt("dfmt", 0), 0, 4);
  g_invert = prefs.getBool("invert", false); g_swaprb = prefs.getBool("swaprb", DEFAULT_SWAPRB);
  g_motion = prefs.getBool("motion", false); g_mmin = constrain(prefs.getInt("mmin", 10), 1, 240); g_msens = constrain(prefs.getInt("msens", 5), 1, 10);
  g_host = prefs.getString("host", DEFAULT_HOST);
  g_alOn = prefs.getBool("alon", false); g_alMin = constrain(prefs.getInt("almin", 7 * 60), 0, 1439); g_alDays = prefs.getInt("aldays", 0x7F) & 0x7F;
  g_alSnz = constrain(prefs.getInt("alsnz", 9), 1, 30); g_alVol = constrain(prefs.getInt("alvol", 70), 1, 100);
  g_night = prefs.getBool("night", false); g_nstart = constrain(prefs.getInt("nstart", 22 * 60), 0, 1439); g_nend = constrain(prefs.getInt("nend", 7 * 60), 0, 1439);
  setenv("TZ", g_tz.c_str(), 1); tzset();
  { uint32_t lt = prefs.getUInt("lastt", 0);                    // restore the last saved time (no battery clock)
    if (lt > 1700000000UL) { struct timeval tv = {(time_t)lt + 5, 0}; settimeofday(&tv, nullptr); } }
  logPreviousCrash();
  if (g_rot != 1 && g_rot != 3) g_rot = DEFAULT_ROT;
  pinMode(0, INPUT_PULLUP);        // BOOT button
  halAudioStart();                 // before the touch controller: the Freenove shares I2C with its codec
  halDisplayBegin();
  halTouchBegin();

  cv.sx = PANEL_W / (float)VW;
  cv.sy = PANEL_H / (float)VH;
#ifdef BOARD_FREENOVE_S3
  frame.setColorDepth(16);
  bg.setColorDepth(16);
  void *fp = frame.createSprite(PANEL_W, PANEL_H);
  void *bp = bg.createSprite(PANEL_W, PANEL_H);
  if (!fp || !bp) {
    tft.setTextColor(TFT_RED);
    tft.drawString("Sprite alloc failed (PSRAM?)", 10, 10, 4);
    while (true) delay(1000);
  }
  Canvas bgc; bgc.s = &bg; bgc.sx = cv.sx; bgc.sy = cv.sy;
  renderBackground(bgc);
#else
  strip.setColorDepth(16);
  if (!strip.createSprite(PANEL_W, STRIP_H)) {
    tft.setTextColor(TFT_RED);
    tft.drawString("Out of memory", 10, 10, 4);
    while (true) delay(1000);
  }
#endif
  randomSeed(esp_random());
  for (auto &b : bubbles) { b = {(float)(40 + random(VW - 80)), (float)random(20, SAND_Y), 14.0f + random(30), (float)(2 + random(5)), (float)random(628) / 100.0f}; }
  for (auto &f : fishes) pickTarget(f, 0);
  for (auto &fd : food) fd.on = false;
  initClockFallback();
  connectWifi();
  motionBegin();
  lastActivity = millis();
}

void loop() {
  static uint32_t last = millis(), lastTouch = 0, fpsT = millis();
  static int frames = 0;
  uint32_t now = millis();
  STAGE(ST_NET);
  float dt = (now - last) / 1000.0f; last = now;
  if (dt > 0.1f) dt = 0.1f;
  float t = now / 1000.0f;

  if (g_online) {
    ArduinoOTA.handle();
    server.handleClient();
    ElegantOTA.loop();
    static uint32_t lastWifi = 0;
    if (WiFi.status() != WL_CONNECTED && now - lastWifi > 30000) { lastWifi = now; WiFi.reconnect(); }
  }
  if (restartAt && millis() > restartAt) ESP.restart();
  static uint32_t lastSave = 0;
  static bool savedAfterNtp = false;
  if (!savedAfterNtp && ntpSynced()) { savedAfterNtp = true; saveTimeNow(); }
  if (now - lastSave > 6UL * 3600000UL) { lastSave = now; saveTimeNow(); }

  STAGE(ST_BUTTON);
  static bool lastBtn = true;
  bool btn = digitalRead(0);
  if (!btn && lastBtn) {
    lastActivity = now;
    if (alActive()) alStop(); else if (nightWake(now)) {} else if (g_asleep) wakeScreen(3); else infoUntil = (infoUntil > now) ? 0 : now + 20000;
  }
  lastBtn = btn;

  STAGE(ST_TOUCH);
  int px, py;
  bool touched = halTouch(px, py);
  if (touched) lastActivity = now;
  static bool wasTouched = false;
  static uint32_t pressAt = 0;
  const bool press = touched && !wasTouched;
  if (press) pressAt = now;
  wasTouched = touched;
  const bool rep = touched && !press && now - pressAt > 450;
  if (touched && (press || now - lastTouch > 120)) {
    lastTouch = now;
    if (nightWake(now)) {
      // touched in the dark: just light the screen
    } else if (g_asleep) {
      wakeScreen(2);                  // the first touch only wakes the screen
    } else {
      float wx = px / cv.sx, wy = max(py / cv.sy, 14.0f);
      if (alState == AL_RING) {
        if (press && wy >= BTN_Y && wy < BTN_Y + BTN_H) {
          if (wx >= BTN_SNZ_X && wx < BTN_SNZ_X + BTN_W) alSnooze(now);
          else if (wx >= BTN_STOP_X && wx < BTN_STOP_X + BTN_W) alStop();
        }
      } else if (alPanel) {
        alPanelTouch(wx, wy, press, rep, now);
      } else if (wx > VW - 170 && wy < 64) {
        if (press) { alPanel = true; alPanelAt = now; }          // tap the time to set the alarm
      } else if (fabsf(wx - CHEST_X) < 36 && wy > CHEST_BASE - 48 && wy < CHEST_BASE + 10) {
        if (chestState == CHEST_CLOSED) { chestState = CHEST_OPENING; chestT = 0; }   // tap the chest to open it
      } else feedAt(wx, wy, 3);
    }
  }

  STAGE(ST_SLEEP);
  updateAlarm(now);
  updateNight(now);
  updateSleep();
  if (g_asleep || g_isNight) { STAGE(ST_IDLE); updateCrumb(millis() - now); delay(40); return; }
  STAGE(ST_WORLD);       // screen off: no rendering

  updateWorld(dt, t, now);
  curT = t; curNow = now;

#ifdef BOARD_FREENOVE_S3
  STAGE(ST_RENDER);
  cv.s = &frame; cv.oy = 0;
  memcpy(frame.getPointer(), bg.getPointer(), PANEL_W * PANEL_H * 2);
  drawScene(t, now);
  STAGE(ST_PUSH);
  frame.pushSprite(0, 0);
#else
  for (int i = 0; i < PANEL_H / STRIP_H; i++) {
    STAGE(ST_RENDER);
    renderStrip(i);
    STAGE(ST_PUSH);
    strip.pushSprite(0, i * STRIP_H);
  }
#endif

  updateCrumb(millis() - now);
  frames++;
  if (now - fpsT >= 5000) {
    g_fps = frames * 1000.0f / (now - fpsT);
    Serial.printf("fps: %.1f  heap %u\n", g_fps, (unsigned)ESP.getFreeHeap());
    frames = 0; fpsT = now;
  }
}
