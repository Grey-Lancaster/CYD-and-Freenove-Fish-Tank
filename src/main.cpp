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
#include <Update.h>
#include "board.h"
#include "canvas.h"
#include "hal.h"
#include "audio.h"
#include "motion.h"
#include "wifi_setup.h"

#define FW_VERSION "1.2.0"

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
int g_rot = DEFAULT_ROT;       // 1 or 3 (180 degrees apart)
float g_fps = 0;
bool g_motion = false;         // Wi-Fi motion sensing: screen sleeps when nobody is around
int g_mmin = 10;               // minutes without motion before sleeping
int g_msens = 5;               // 1..10
volatile bool g_asleep = false;
static uint32_t lastActivity = 0;
bool g_invert = false;         // flip the panel's colour inversion
bool g_swaprb = DEFAULT_SWAPRB;  // swap red/blue
bool g_h24 = false;            // 24-hour clock
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
  prefs.putInt("rot", g_rot); prefs.putBool("snd", g_snd); prefs.putInt("vol", g_vol);
  prefs.putBool("afeed", g_auto); prefs.putInt("aint", g_aint); prefs.putBool("h24", g_h24);
  prefs.putBool("invert", g_invert); prefs.putBool("swaprb", g_swaprb);
  prefs.putBool("motion", g_motion); prefs.putInt("mmin", g_mmin); prefs.putInt("msens", g_msens);
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

static void drawClock() {
  int h, mi;
  char date[24] = "";
  struct tm tmv;
  if (getLocalTime(&tmv, 0)) {
    h = tmv.tm_hour; mi = tmv.tm_min;
    static const char *dn[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    snprintf(date, sizeof(date), "%s %02d/%02d/%02d", dn[tmv.tm_wday], tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_year % 100);
  } else {
    uint32_t s = (baseSecs + millis() / 1000) % 86400;
    h = s / 3600; mi = (s / 60) % 60;
  }
  char buf[16];
  if (g_h24) snprintf(buf, sizeof(buf), "%02d:%02d", h, mi);
  else {
    int h12 = h % 12; if (h12 == 0) h12 = 12;
    snprintf(buf, sizeof(buf), "%d:%02d %s", h12, mi, h >= 12 ? "PM" : "AM");
  }
  cv.setTextDatum(TR_DATUM);
  cv.setTextColor(RGB(10, 40, 80));
  cv.drawString(buf, VW - 9, TIME_Y + 1, 4);
  if (date[0]) cv.drawString(date, VW - 9, DATE_Y + 1, 2);
  cv.setTextColor(RGB(235, 245, 255));
  cv.drawString(buf, VW - 10, TIME_Y, 4);
  if (date[0]) cv.drawString(date, VW - 10, DATE_Y, 2);
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
    case CHEST_OPENING: if (chestT >= 1.0f) { chestState = CHEST_OPEN; chestT = 0; chestBubbleT = 0; } break;
    case CHEST_OPEN:
      chestBubbleT += dt;
      while (chestBubbleT > 0.11f) {
        chestBubbleT -= 0.11f;
        for (auto &b : cbub) if (!b.on) { b = {CHEST_X + (float)random(-14, 15), (float)(CHEST_BASE - 24), 45.0f + random(40), 2.0f + random(4), (float)random(628) / 100.0f, true}; break; }
      }
      if (chestT >= 4.5f) { chestState = CHEST_CLOSING; chestT = 0; }
      break;
    case CHEST_CLOSING: if (chestT >= 1.0f) { chestState = CHEST_CLOSED; chestT = 0; chestNextOpen = 12 + random(10); } break;
  }
  for (auto &b : cbub) if (b.on) { b.y -= b.v * dt; if (b.y < -6) b.on = false; }
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
    } else if (now > f.nextPick || (fabsf(tx - f.x) < 8 && fabsf(ty - f.y) < 6)) {
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
static void wakeScreen(uint8_t cause) {
  if (!g_asleep) return;
  g_asleep = false;
  motionLogEvent(false, cause);
  halBacklight(true);
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
  if (!motionReady() || motionDetected()) {
    lastActivity = now;
    wakeScreen(motionDetected() ? 1 : 5);
    return;
  }
  if (!g_asleep && now - lastActivity >= (uint32_t)g_mmin * 60000UL) {
    g_asleep = true;
    motionLogEvent(true, 0);
    halBacklight(false);
  }
}

static const char PAGE[] PROGMEM = R"HTML(<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>Fish Tank</title>
<style>body{margin:0;font:16px system-ui,sans-serif;background:#0b2a45;color:#e8f4ff}
main{max-width:460px;margin:0 auto;padding:16px}h1{margin:.2em 0}
.c{background:#12466f;border-radius:12px;padding:14px;margin:12px 0}
label{display:flex;justify-content:space-between;align-items:center;margin:10px 0}
input[type=range]{width:55%}select,button{font:inherit;padding:8px 12px;border-radius:8px;border:0}
button,.b{background:#2fb36b;color:#fff;text-decoration:none;border-radius:8px;padding:8px 12px;margin-top:10px}small{color:#9cc4e4}</style></head><body><main>
<h1>&#128032; Fish Tank</h1><small id="st">loading&hellip;</small>
<div class="c"><label>Sound <input type="checkbox" id="snd"></label>
<label>Volume <input type="range" id="vol" min="0" max="100"><span id="vv"></span></label>
<label>24-hour time <input type="checkbox" id="h24"></label>
<label>Screen rotation <select id="rot"><option value="@L@">USB on left</option><option value="@R@">USB on right</option></select></label>
<label>Invert colours <input type="checkbox" id="inv"></label>
<label>Swap red and blue <input type="checkbox" id="swaprb"></label>
<small>Use these if the colours look wrong (like a photo negative, or red and blue swapped).</small></div>
<div class="c"><button id="feed">Feed the fish</button> <button id="chest">Open the treasure chest</button>
<label style="margin-top:14px">Auto feed <input type="checkbox" id="auto"></label>
<label>Every (seconds) <input type="number" id="aint" min="5" max="3600" style="width:90px"></label></div>
<div class="c"><button id="shot">Screenshot</button> <label style="display:inline"><input type="checkbox" id="live"> live view</label>
<small id="shm"> (the tank pauses for a second while it copies)</small>
<canvas id="cv" width="480" height="320" style="display:none;width:100%;margin-top:10px;border-radius:8px"></canvas>
<a id="dl" style="display:none" class="b">Download PNG</a></div>
<div class="c"><b>Motion sleep</b> <small>(experimental)</small>
<label>Sleep the screen when nobody is around <input type="checkbox" id="motion"></label>
<label>Sleep after (minutes) <input type="number" id="mmin" min="1" max="240" style="width:90px"></label>
<label>Sensitivity <input type="range" id="msens" min="1" max="10"></label>
<small id="mlive"></small>
<div id="mlog" style="margin-top:8px;font-size:14px"></div>
<button id="mclr">Clear log</button></div>
<div class="c"><button id="apply">Apply</button> <small id="amsg"></small><br><small>Applies sound, volume, rotation and auto feed.</small></div>
<div class="c"><b>Firmware update</b><br><small>Upload the <b>update</b> file for this board (not the factory file): it keeps your WiFi and settings.</small><br>
<input type="file" id="fw" accept=".bin"><br><button id="fwb">Upload &amp; restart</button> <small id="fwmsg"></small></div>
<div class="c"><b>System</b><br>
<button id="wifi">Forget WiFi &amp; restart</button> <button id="recal" style="display:none">Recalibrate touch</button> <small id="sysmsg"></small></div>
<div class="c"><label>Hostname <input id="host" maxlength="31" style="width:55%"></label>
<small>letters, digits and dashes &mdash; reachable as <b id="hl"></b>.local</small><br><br>
<button id="hsave">Save &amp; reboot</button> <small id="hmsg"></small></div>
<script>
const $=i=>document.getElementById(i);let busy=0,dirty=0;
async function load(){const s=await (await fetch('/api/status')).json();
 $('mlive').textContent=s.motion?(s.mready?'motion score '+s.mscore+(s.mon?' \u25cf moving':' (quiet)')+(s.asleep?' \u00b7 screen asleep':''):'sensor warming up\u2026'):'';
window.envId=s.env;$('recal').style.display=s.board=='cyd'?'inline-block':'none';
if(!busy&&!dirty){$('inv').checked=s.inv;$('swaprb').checked=s.swaprb;$('motion').checked=s.motion;$('mmin').value=s.mmin;$('msens').value=s.msens;$('h24').checked=s.h24;$('auto').checked=s.auto;$('aint').value=s.aint;if(document.activeElement!==$('host'))$('host').value=s.host;$('hl').textContent=s.host;$('snd').checked=s.snd;$('vol').value=s.vol;$('vv').textContent=s.vol;$('rot').value=s.rot}
 $('st').textContent='v'+s.ver+' · WiFi '+(s.ssid||'?')+' · '+s.ip+' · '+s.rssi+' dBm · up '+Math.floor(s.up/60)+' min · '+s.fps.toFixed(1)+' fps'}
async function send(){busy=1;await fetch('/api/set?snd='+($('snd').checked?1:0)+'&vol='+$('vol').value+'&rot='+$('rot').value+'&motion='+($('motion').checked?1:0)+'&mmin='+$('mmin').value+'&msens='+$('msens').value+'&inv='+($('inv').checked?1:0)+'&swaprb='+($('swaprb').checked?1:0)+'&h24='+($('h24').checked?1:0)+'&auto='+($('auto').checked?1:0)+'&aint='+$('aint').value,{method:'POST'});busy=0;dirty=0;$('amsg').textContent='Applied \u2713'}
const mark=()=>{dirty=1;$('amsg').textContent='not applied yet'};
['snd','h24','auto','aint','rot','motion','mmin','msens','inv','swaprb'].forEach(i=>$(i).onchange=mark);$('vol').oninput=()=>{$('vv').textContent=$('vol').value;mark()};$('apply').onclick=send;
const CAUSE=['no motion','motion','touch','BOOT button','setting turned off','sensor offline'];
async function mlog(){try{const a=await (await fetch('/api/motionlog')).json();
 $('mlog').innerHTML=a.length?a.map(e=>(e.e?'\u2600 woke':'\u263e slept')+' \u00b7 '+(e.w||'up '+Math.floor(e.up/60)+' min')+' \u00b7 '+CAUSE[e.c]).join('<br>'):'<small>No sleep/wake events yet.</small>'}catch(e){}}
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
$('fwb').onclick=()=>{const f=$('fw').files[0];if(!f){$('fwmsg').textContent='choose a file first';return}
 if(!f.name.includes(window.envId)&&!confirm('This file name does not contain "'+window.envId+'", so it may be for a different board. Continue anyway?'))return;
 const x=new XMLHttpRequest(),fd=new FormData();fd.append('fw',f,f.name);x.open('POST','/update');
 x.upload.onprogress=e=>$('fwmsg').textContent=Math.round(e.loaded*100/e.total)+'%';
 x.onload=()=>{$('fwmsg').textContent=x.status==200?'Done. Restarting\u2026 reload this page in a few seconds.':'Update failed ('+x.responseText+')'};
 x.onerror=()=>$('fwmsg').textContent='connection lost (it may be restarting)';x.send(fd)};
$('chest').onclick=()=>fetch('/api/chest',{method:'POST'});
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


static String statusJson() {
  String ssid = WiFi.status() == WL_CONNECTED ? WiFi.SSID() : String("");
  ssid.replace("\\", ""); ssid.replace("\"", "");
  char b[700];
  snprintf(b, sizeof(b), "{\"env\":\"" BOARD_ID "\",\"board\":\"%s\",\"inv\":%s,\"swaprb\":%s,\"motion\":%s,\"mmin\":%d,\"msens\":%d,\"mon\":%s,\"mready\":%s,\"mscore\":%.2f,\"asleep\":%s,\"h24\":%s,\"ssid\":\"%s\",\"snd\":%s,\"vol\":%d,\"rot\":%d,\"auto\":%s,\"aint\":%d,\"host\":\"%s\",\"ver\":\"" FW_VERSION "\",\"ip\":\"%s\",\"rssi\":%d,\"up\":%lu,\"fps\":%.1f}",
#ifdef BOARD_CYD28
           "cyd",
#else
           "freenove",
#endif
           g_invert ? "true" : "false", g_swaprb ? "true" : "false",
           g_motion ? "true" : "false", g_mmin, g_msens, motionDetected() ? "true" : "false", motionReady() ? "true" : "false", motionScore(), g_asleep ? "true" : "false",
           g_h24 ? "true" : "false", ssid.c_str(), g_snd ? "true" : "false", (int)g_vol, g_rot, g_auto ? "true" : "false", g_aint, g_host.c_str(), WiFi.localIP().toString().c_str(), WiFi.RSSI(), millis() / 1000UL, g_fps);
  return String(b);
}

static void drawScene(float t, uint32_t now);
#ifdef BOARD_CYD28
static void renderStrip(int i) {
  cv.s = &strip; cv.oy = i * STRIP_H;
  renderBackground(cv);
  drawScene(curT, curNow);
}
#endif

static void setupWeb() {
  server.on("/", HTTP_GET, []() {
    String p = FPSTR(PAGE);
    p.replace("@L@", String(ROT_LEFT)); p.replace("@R@", String(ROT_RIGHT));
    server.send(200, "text/html", p);
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
    if (server.hasArg("vol")) g_vol = constrain(server.arg("vol").toInt(), 0, 100);
    if (server.hasArg("snd")) g_snd = server.arg("snd").toInt() != 0;
    if (server.hasArg("motion")) g_motion = server.arg("motion").toInt() != 0;
    if (server.hasArg("mmin")) g_mmin = constrain(server.arg("mmin").toInt(), 1, 240);
    if (server.hasArg("msens")) g_msens = constrain(server.arg("msens").toInt(), 1, 10);
    if (server.hasArg("h24")) g_h24 = server.arg("h24").toInt() != 0;
    if (server.hasArg("auto")) g_auto = server.arg("auto").toInt() != 0;
    if (server.hasArg("aint")) g_aint = constrain(server.arg("aint").toInt(), 5, 3600);
    bool colorsChanged = false;
    if (server.hasArg("inv")) { bool v = server.arg("inv").toInt() != 0; colorsChanged |= v != g_invert; g_invert = v; }
    if (server.hasArg("swaprb")) { bool v = server.arg("swaprb").toInt() != 0; colorsChanged |= v != g_swaprb; g_swaprb = v; }
    if (server.hasArg("rot")) {
      int r = server.arg("rot").toInt();
      if ((r == 1 || r == 3) && r != g_rot) { g_rot = r; halApplyRotation(); colorsChanged = false; }
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
  server.on("/update", HTTP_POST, []() {
    bool ok = !Update.hasError();
    server.send(ok ? 200 : 500, "text/plain", ok ? "OK" : Update.errorString());
    if (ok) restartAt = millis() + 1000;
  }, []() {
    HTTPUpload &up = server.upload();
    if (up.status == UPLOAD_FILE_START) Update.begin(UPDATE_SIZE_UNKNOWN);
    else if (up.status == UPLOAD_FILE_WRITE) Update.write(up.buf, up.currentSize);
    else if (up.status == UPLOAD_FILE_END) Update.end(true);
    else if (up.status == UPLOAD_FILE_ABORTED) Update.abort();
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
  server.begin();
}

static void startServices() {
  g_online = true;
  WiFi.setAutoReconnect(true);
  configTzTime("EST5EDT,M3.2.0,M11.1.0", "pool.ntp.org", "time.nist.gov");
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
  snprintf(b, sizeof(b), "Up %lu min  Free %u KB  %.1f fps", millis() / 60000UL, (unsigned)(ESP.getFreeHeap() / 1024), g_fps);
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
    cv.fillEllipse(x, top, 17 * p, 4 * p + 1, RGB(255, 214, 95));
    cv.fillEllipse(x - 6, top - 1, 4 * p + 1, 2 * p + 1, goldL);
    cv.fillEllipse(x + 7, top, 4 * p + 1, 2 * p + 1, goldL);
  }
  // lock
  cv.fillRect(x - 4, top + 7, 8, 8, gold);
  cv.fillRect(x - 1, top + 10, 2, 3, woodD);
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
  for (int i = 0; i < NFISH; i++) {
    Fish &f = fishes[i];
    float wag = sinf(t * 9.0f + f.ph) * 4.0f;
    float fin = sinf(t * 6.0f + f.ph) * 4.0f;
    switch (f.type) {
      case TANG:  drawTang(f.x, f.y, f.dir, wag); break;
      case CLOWN: drawClown(f.x, f.y, f.dir, wag); break;
      case ANGEL: drawAngel(f.x, f.y, f.dir, wag, fin); break;
      case GOLD:  drawGold(f.x, f.y, f.dir, wag); break;
      case NEON:  drawNeon(f.x, f.y, f.dir, wag * .6f); break;
    }
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
  drawClock();
  if (infoUntil > now) drawInfo(now);
}

void setup() {
  Serial.begin(115200);
  prefs.begin("tank", false);
  g_rot = prefs.getInt("rot", DEFAULT_ROT); g_snd = prefs.getBool("snd", true); g_vol = prefs.getInt("vol", 60);
  g_auto = prefs.getBool("afeed", false); g_aint = prefs.getInt("aint", 60); g_h24 = prefs.getBool("h24", false);
  g_invert = prefs.getBool("invert", false); g_swaprb = prefs.getBool("swaprb", DEFAULT_SWAPRB);
  g_motion = prefs.getBool("motion", false); g_mmin = constrain(prefs.getInt("mmin", 10), 1, 240); g_msens = constrain(prefs.getInt("msens", 5), 1, 10);
  g_host = prefs.getString("host", DEFAULT_HOST);
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
  float dt = (now - last) / 1000.0f; last = now;
  if (dt > 0.1f) dt = 0.1f;
  float t = now / 1000.0f;

  if (g_online) {
    ArduinoOTA.handle();
    server.handleClient();
    static uint32_t lastWifi = 0;
    if (WiFi.status() != WL_CONNECTED && now - lastWifi > 30000) { lastWifi = now; WiFi.reconnect(); }
  }
  if (restartAt && millis() > restartAt) ESP.restart();

  static bool lastBtn = true;
  bool btn = digitalRead(0);
  if (!btn && lastBtn) {
    lastActivity = now;
    if (g_asleep) wakeScreen(3); else infoUntil = (infoUntil > now) ? 0 : now + 20000;
  }
  lastBtn = btn;

  int px, py;
  bool touched = halTouch(px, py);
  if (touched) lastActivity = now;
  if (touched && now - lastTouch > 120) {
    lastTouch = now;
    if (g_asleep) {
      wakeScreen(2);                  // the first touch only wakes the screen
    } else {
      feedAt(px / cv.sx, max(py / cv.sy, 14.0f), 3);
    }
  }

  updateSleep();
  if (g_asleep) { delay(40); return; }       // screen off: no rendering

  updateWorld(dt, t, now);
  curT = t; curNow = now;

#ifdef BOARD_FREENOVE_S3
  cv.s = &frame; cv.oy = 0;
  memcpy(frame.getPointer(), bg.getPointer(), PANEL_W * PANEL_H * 2);
  drawScene(t, now);
  frame.pushSprite(0, 0);
#else
  for (int i = 0; i < PANEL_H / STRIP_H; i++) {
    renderStrip(i);
    strip.pushSprite(0, i * STRIP_H);
  }
#endif

  frames++;
  if (now - fpsT >= 5000) {
    g_fps = frames * 1000.0f / (now - fpsT);
    Serial.printf("fps: %.1f  heap %u\n", g_fps, (unsigned)ESP.getFreeHeap());
    frames = 0; fpsT = now;
  }
}
