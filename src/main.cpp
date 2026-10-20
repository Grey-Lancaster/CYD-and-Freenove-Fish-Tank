// Feenove-Aquarium - animated fish tank for the Freenove FNK0104S.
// Renders into a full-frame RGB565 sprite in PSRAM (static background is a
// second PSRAM sprite that gets copied each frame), then pushes it to the
// ST7796. Touch drops fish food; a clock sits in the top-right corner.
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <time.h>
#include "FT6336U.h"
#if __has_include("secrets.h")
#include "secrets.h"
#endif

#define SW 480
#define SH 320
#define SAND_Y 270

#define I2C_SDA 16
#define I2C_SCL 15
#define RST_N_PIN 18
#define INT_N_PIN 17

TFT_eSPI tft = TFT_eSPI();
TFT_eSprite frame = TFT_eSprite(&tft);
TFT_eSprite bg = TFT_eSprite(&tft);
FT6336U ft6336u(I2C_SDA, I2C_SCL, RST_N_PIN, INT_N_PIN);

static inline uint16_t RGB(int r, int g, int b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
static inline uint16_t bswap16(uint16_t v) { return (v >> 8) | (v << 8); }

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
  if (edge) frame.fillRect(x0 - 1, (int)(cy - h + 1), w + 2, (int)(2 * h - 1), edge);
  frame.fillRect(x0, (int)(cy - h + 1), w, (int)(2 * h - 1), col);
}

static void eye(float x, float y, int r, uint16_t iris = TFT_WHITE) {
  frame.fillCircle((int)x, (int)y, r, iris);
  frame.fillCircle((int)x + 1, (int)y, r > 3 ? 2 : 1, TFT_BLACK);
}

#define FX(dx) ((int)(x + f * (dx)))

static void drawTang(float x, float y, int f, float wag) {
  const float a = 24, b = 16;
  uint16_t blue = RGB(25, 75, 225), lite = RGB(70, 135, 250), dk = RGB(10, 30, 110), yel = RGB(255, 215, 30);
  frame.fillTriangle(FX(-a * .9f), (int)y, FX(-a - 14), (int)(y - 11 + wag), FX(-a - 14), (int)(y + 11 + wag), yel);
  frame.fillTriangle(FX(-10), (int)(y - b * .9f), FX(6), (int)(y - b - 7), FX(14), (int)(y - b * .8f), blue);
  frame.fillTriangle(FX(-8), (int)(y + b * .9f), FX(4), (int)(y + b + 5), FX(12), (int)(y + b * .85f), blue);
  frame.fillEllipse((int)x, (int)y, a, b, blue);
  frame.fillEllipse(FX(1), (int)y + 3, a - 4, b - 8, lite);
  frame.drawEllipse((int)x, (int)y, a, b, dk);
  frame.drawWideLine(FX(-(a - 8)), y + 1, FX(a - 13), y - 3, 3, dk, dk);
  frame.fillTriangle(FX(4), (int)y + 3, FX(-4), (int)y + 9, FX(1), (int)y + 10, yel);
  eye(FX(a - 9), y - 4, 3);
}

static void drawClown(float x, float y, int f, float wag) {
  const float a = 22, b = 14;
  uint16_t org = RGB(255, 110, 15), lite = RGB(255, 170, 60), dk = RGB(110, 40, 5);
  frame.fillTriangle(FX(-a * .9f), (int)y, FX(-a - 11), (int)(y - 9 + wag), FX(-a - 11), (int)(y + 9 + wag), org);
  frame.fillTriangle(FX(-8), (int)(y - b * .9f), FX(2), (int)(y - b - 6), FX(12), (int)(y - b * .8f), org);
  frame.fillEllipse((int)x, (int)y, a, b, org);
  frame.fillEllipse(FX(1), (int)y + 3, a - 4, b - 7, lite);
  float cx = x;
  // bands are drawn in screen space around the (flipped) offsets
  band(x, y, a, b, f * 10, 5, TFT_WHITE, TFT_BLACK);
  band(x, y, a, b, f * -2, 6, TFT_WHITE, TFT_BLACK);
  band(x, y, a, b, f * -14, 4, TFT_WHITE, TFT_BLACK);
  (void)cx;
  frame.drawEllipse((int)x, (int)y, a, b, dk);
  eye(FX(a - 6), y - 3, 3);
}

static void drawAngel(float x, float y, int f, float wag, float fin) {
  const float a = 18, b = 15;
  uint16_t gray = RGB(120, 126, 138), grayd = RGB(70, 75, 88), cream = RGB(238, 228, 185), white = RGB(245, 245, 245);
  // tail fan
  frame.fillTriangle(FX(-16), (int)y, FX(-36), (int)(y - 15 + wag), FX(-36), (int)(y + 15 + wag), gray);
  frame.drawLine(FX(-16), (int)y, FX(-36), (int)(y + wag), grayd);
  // dorsal, anal, pelvic fins
  frame.fillTriangle(FX(8), (int)(y - b + 3), FX(-10), (int)(y - b - 26), FX(-20), (int)(y - b + 5), gray);
  frame.fillTriangle(FX(6), (int)(y + b - 3), FX(-8), (int)(y + b + 24), FX(-17), (int)(y + b - 4), gray);
  frame.drawWideLine(FX(4), y + b - 3, FX(-2 + fin), y + b + 30, 2, gray, gray);
  // body: cream top, white belly
  frame.fillEllipse((int)x, (int)y, a, b, white);
  frame.fillEllipse((int)x, (int)y - 4, a - 1, b - 5, cream);
  frame.drawEllipse((int)x, (int)y, a, b, grayd);
  band(x, y, a, b, f * 6, 2, TFT_BLACK, 0);
  band(x, y, a, b, f * -1, 2, TFT_BLACK, 0);
  band(x, y, a, b, f * -8, 2, TFT_BLACK, 0);
  frame.fillTriangle(FX(3), (int)y + 2, FX(-3), (int)y + 8, FX(0), (int)y + 9, RGB(215, 222, 232));
  eye(FX(a - 6), y - 3, 3, RGB(230, 45, 40));
}

static void drawGold(float x, float y, int f, float wag) {
  const float a = 20, b = 13;
  uint16_t org = RGB(240, 95, 15), lite = RGB(255, 170, 50), tailc = RGB(255, 125, 30), dk = RGB(120, 45, 5);
  frame.fillTriangle(FX(-16), (int)y, FX(-38), (int)(y - 17 + wag), FX(-30), (int)(y + wag), tailc);
  frame.fillTriangle(FX(-16), (int)y, FX(-38), (int)(y + 17 + wag), FX(-30), (int)(y + wag), tailc);
  frame.fillTriangle(FX(-6), (int)(y - b * .9f), FX(2), (int)(y - b - 7), FX(10), (int)(y - b * .8f), tailc);
  frame.fillEllipse((int)x, (int)y, a, b, org);
  frame.fillEllipse(FX(1), (int)y + 3, a - 4, b - 7, lite);
  frame.drawEllipse((int)x, (int)y, a, b, dk);
  eye(FX(a - 6), y - 3, 3);
}

static void drawNeon(float x, float y, int f, float wag) {
  const float a = 12, b = 5;
  frame.fillTriangle(FX(-a + 2), (int)y, FX(-a - 6), (int)(y - 4 + wag), FX(-a - 6), (int)(y + 4 + wag), RGB(200, 210, 230));
  frame.fillEllipse((int)x, (int)y, a, b, RGB(235, 35, 45));
  frame.fillEllipse(FX(4), (int)y, a - 5, b - 1, RGB(200, 212, 228));
  frame.drawWideLine(FX(a - 2), y - 1, FX(-1), y - 1, 2, RGB(40, 210, 255), RGB(40, 210, 255));
  eye(FX(a - 3), y - 1, 2);
}

// ----------------------------------------------------------- background
static void buildBackground() {
  for (int y = 0; y < SH; y++) {
    float t = (float)y / SH;
    bg.drawFastHLine(0, y, SW, RGB((int)(20 + 10 * (1 - t)), (int)(110 - 60 * t), (int)(190 - 90 * t)));
  }
  for (int y = SAND_Y; y < SH; y++) {
    float t = (float)(y - SAND_Y) / (SH - SAND_Y);
    bg.drawFastHLine(0, y, SW, RGB((int)(214 - 40 * t), (int)(190 - 40 * t), (int)(140 - 40 * t)));
  }
  randomSeed(7);
  for (int i = 0; i < 500; i++) bg.drawPixel(random(SW), random(SAND_Y + 2, SH), RGB(150, 125, 90));
  struct R { int cx, cy, rx, ry; uint16_t c; } rocks[] = {
    {90, 296, 32, 18, RGB(95, 95, 105)}, {120, 302, 22, 12, RGB(120, 118, 125)}, {380, 300, 38, 20, RGB(90, 90, 100)}};
  for (auto &r : rocks) {
    bg.fillEllipse(r.cx, r.cy, r.rx, r.ry, r.c);
    bg.fillEllipse(r.cx - r.rx / 4, r.cy - r.ry / 2, r.rx / 2, r.ry / 3, RGB(140, 140, 150));
  }
}

// Lighten pixels (byte-swapped RGB565 in the sprite buffer) for the light rays.
static void drawRays(float t) {
  uint16_t *buf = (uint16_t *)frame.getPointer();
  const struct { float x0, w, ph; } rays[] = {{80, 40, 0}, {200, 60, 1.5f}, {330, 35, 3}, {420, 50, 4.5f}};
  for (auto &r : rays) {
    float xs = r.x0 + 25 * sinf(t * 0.4f + r.ph);
    for (int y = 0; y < SAND_Y; y += 1) {
      int xa = (int)(xs - y * 0.28f), xb = (int)(xa + r.w);
      if (xa < 0) xa = 0;
      if (xb > SW) xb = SW;
      uint16_t *p = buf + y * SW + xa;
      for (int x = xa; x < xb; x++, p++) {
        uint16_t c = bswap16(*p);
        int rr = c >> 11, g = (c >> 5) & 63, b = c & 31;
        rr += (31 - rr) >> 4; g += (63 - g) >> 4; b += (31 - b) >> 4;
        *p = bswap16((rr << 11) | (g << 5) | b);
      }
    }
  }
}

// ---------------------------------------------------------------- clock
static uint32_t baseSecs = 0;      // seconds since midnight at millis()==0 (fallback clock)

static void initClock() {
  int h = 0, m = 0, s = 0;
  sscanf(__TIME__, "%d:%d:%d", &h, &m, &s);
  baseSecs = h * 3600 + m * 60 + s;
#ifdef WIFI_SSID
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.setHostname("fish-tank");
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 8000) delay(100);
  configTzTime("EST5EDT,M3.2.0,M11.1.0", "pool.ntp.org", "time.nist.gov");
  // WiFi stays connected: NTP keeps syncing and OTA stays reachable.
  ArduinoOTA.setHostname("fish-tank");
#ifdef OTA_PASS
  ArduinoOTA.setPassword(OTA_PASS);
#endif
  ArduinoOTA.onStart([]() { tft.fillScreen(TFT_BLACK); tft.setTextColor(TFT_WHITE); tft.drawString("OTA update...", 10, 10, 4); });
  ArduinoOTA.onProgress([](unsigned int p, unsigned int total) {
    tft.fillRect(10, 60, 460, 20, TFT_DARKGREY);
    tft.fillRect(10, 60, (int)(460.0f * p / total), 20, TFT_GREEN);
  });
  ArduinoOTA.begin();
  Serial.printf("WiFi %s, IP %s, OTA host fish-tank\n", WiFi.status() == WL_CONNECTED ? "connected" : "NOT connected", WiFi.localIP().toString().c_str());
#endif
}

static void drawClock() {
  int h, m;
  struct tm tmv;
  if (getLocalTime(&tmv, 0)) {
    h = tmv.tm_hour; m = tmv.tm_min;
  } else {
    uint32_t s = (baseSecs + millis() / 1000) % 86400;
    h = s / 3600; m = (s / 60) % 60;
  }
  const char *ap = h >= 12 ? "PM" : "AM";
  int h12 = h % 12; if (h12 == 0) h12 = 12;
  char buf[16];
  snprintf(buf, sizeof(buf), "%d:%02d %s", h12, m, ap);
  frame.setTextDatum(TR_DATUM);
  frame.setTextColor(RGB(10, 40, 80));
  frame.drawString(buf, SW - 9, 9, 4);
  frame.setTextColor(RGB(235, 245, 255));
  frame.drawString(buf, SW - 10, 8, 4);
}

// ------------------------------------------------------------- behaviour
static void pickTarget(Fish &f, uint32_t now) {
  f.tx = 30 + random(SW - 60);
  f.ty = f.yMin + random((int)(f.yMax - f.yMin));
  f.nextPick = now + 2500 + random(5000);
}

static void spawnFood(float x, float y) {
  for (auto &fd : food)
    if (!fd.on) { fd = {x, y, true, 0}; return; }
}

static void updateWorld(float dt, float t, uint32_t now) {
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
    f.x = constrain(f.x, 30.0f, (float)(SW - 30));
    if (f.vx > 6) f.dir = 1; else if (f.vx < -6) f.dir = -1;
  }
  for (auto &fd : food) {
    if (!fd.on) continue;
    if (fd.y < SAND_Y + 6) fd.y += 28 * dt; else if (!fd.landed) fd.landed = now;
    if (fd.landed && now - fd.landed > 4000) fd.on = false;
  }
  for (auto &b : bubbles) {
    b.y -= b.v * dt;
    if (b.y < -6) { b.y = SAND_Y - 6; b.x = 40 + random(SW - 80); }
  }
}


void setup() {
  Serial.begin(115200);
  ft6336u.begin();
  tft.init();
  tft.setRotation(3);   // 180 deg from rotation 1: USB port on the left
  tft.fillScreen(TFT_BLACK);
  tft.setSwapBytes(true);

  frame.setColorDepth(16);
  bg.setColorDepth(16);
  void *fp = frame.createSprite(SW, SH);
  void *bp = bg.createSprite(SW, SH);
  if (!fp || !bp) {
    tft.setTextColor(TFT_RED);
    tft.drawString("Sprite alloc failed (PSRAM?)", 10, 10, 4);
    while (true) delay(1000);
  }
  randomSeed(esp_random());
  buildBackground();
  for (auto &b : bubbles) { b = {(float)(40 + random(SW - 80)), (float)random(20, SAND_Y), 14.0f + random(30), (float)(2 + random(5)), (float)random(628) / 100.0f}; }
  for (auto &f : fishes) pickTarget(f, 0);
  for (auto &fd : food) fd.on = false;
  tft.setCursor(10, 10, 4); tft.setTextColor(TFT_WHITE); tft.print("Starting tank...");
  initClock();
}

void loop() {
  static uint32_t last = millis(), lastTouch = 0, fpsT = millis();
  static int frames = 0;
  uint32_t now = millis();
  float dt = (now - last) / 1000.0f; last = now;
  if (dt > 0.1f) dt = 0.1f;
  float t = now / 1000.0f;

#ifdef WIFI_SSID
  ArduinoOTA.handle();
  static uint32_t lastWifi = 0;
  if (WiFi.status() != WL_CONNECTED && now - lastWifi > 30000) { lastWifi = now; WiFi.reconnect(); }
#endif
  FT6336U_TouchPointType tp = ft6336u.scan();
  if (tp.touch_count != 0 && now - lastTouch > 120) {
    lastTouch = now;
    float x = SW - tp.tp[0].y, y = tp.tp[0].x;   // matches rotation 3
    for (int i = 0; i < 3; i++) spawnFood(x + random(-8, 9), max(y, 14.0f) + random(-4, 5));
  }

  updateWorld(dt, t, now);

  memcpy(frame.getPointer(), bg.getPointer(), SW * SH * 2);
  drawRays(t);
  for (auto &k : kelp) {
    float px = k.x, py = SAND_Y + 28;
    for (int i = 3; i <= k.h; i += 12) {
      float nx = k.x + 9 * sinf(i / 22.0f + k.ph - t * 1.2f), ny = SAND_Y + 28 - i;
      frame.drawWideLine(px, py, nx, ny, 4, RGB(30, 150, 80), RGB(30, 150, 80));
      px = nx; py = ny;
    }
  }
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
  for (auto &fd : food) if (fd.on) frame.fillCircle((int)fd.x, (int)fd.y, 2, RGB(235, 200, 120));
  for (auto &b : bubbles) {
    int bx = (int)(b.x + 5 * sinf(t * b.v * 0.08f + b.ph)), by = (int)b.y, r = (int)b.r;
    frame.drawCircle(bx, by, r, RGB(205, 232, 255));
    frame.drawPixel(bx - r / 3, by - r / 3, TFT_WHITE);
  }
  drawClock();

  frame.pushSprite(0, 0);

  frames++;
  if (now - fpsT >= 5000) {
    Serial.printf("fps: %.1f\n", frames * 1000.0f / (now - fpsT));
    frames = 0; fpsT = now;
  }
}
