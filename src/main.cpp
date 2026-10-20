// Feenove-Aquarium - animated fish tank for the Freenove FNK0104S.
// Renders into a full-frame RGB565 sprite in PSRAM (static background is a
// second PSRAM sprite that gets copied each frame), then pushes it to the
// ST7796. Touch drops fish food; a clock sits in the top-right corner.
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <time.h>
#include "FT6336U.h"
#include <Wire.h>
#include "driver/i2s.h"
#include "es8311.h"
#if __has_include("secrets.h")
#include "secrets.h"
#endif

#define FW_VERSION "0.2.0"
#define HOSTNAME "fishtank"

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
Preferences prefs;
WebServer server(80);
extern es8311_handle_t g_es_handle;

// Settings (persisted in NVS, editable from the web page)
volatile int g_vol = 60;       // 0..100 (60 = the original loop level)
volatile bool g_snd = true;
int g_rot = 3;                 // 3 = USB on the left, 1 = USB on the right
float g_fps = 0;
bool g_h24 = false;           // 24-hour clock
bool g_auto = false;           // automatic feeding
int g_aint = 60;               // seconds between automatic feeds
String g_host = HOSTNAME;      // mDNS / OTA / DHCP name (editable on the web page)
uint32_t restartAt = 0;

static inline uint16_t RGB(int r, int g, int b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
static inline uint16_t bswap16(uint16_t v) { return (v >> 8) | (v << 8); }


// ---------------------------------------------------------------- audio
// ES8311 codec + speaker: loops the embedded bubbles.raw (16 kHz mono int16)
// from a core-0 task via the legacy I2S driver. Codec is fixed at 16 kHz.
extern const uint8_t bubbles_start[] asm("_binary_src_bubbles_raw_start");
extern const uint8_t bubbles_end[] asm("_binary_src_bubbles_raw_end");

#define AP_ENABLE 1
#define I2S_MCK 4
#define I2S_BCK 5
#define I2S_DINT 6
#define I2S_DOUT 8
#define I2S_WS 7

static void audioTask(void *) {
  const int16_t *pcm = (const int16_t *)bubbles_start;
  const size_t n = (bubbles_end - bubbles_start) / 2;
  static int16_t out[512];           // 256 stereo frames
  size_t idx = 0;
  for (;;) {
    for (int i = 0; i < 256; i++) {
      int32_t s = g_snd ? ((int32_t)pcm[idx] * g_vol) / 60 : 0;
      if (s > 32767) s = 32767; else if (s < -32768) s = -32768;
      int16_t v = (int16_t)s;
      out[2 * i] = v; out[2 * i + 1] = v;
      if (++idx >= n) idx = 0;
    }
    size_t written;
    i2s_write(I2S_NUM_0, out, sizeof(out), &written, portMAX_DELAY);
  }
}

static void applySound() {
  digitalWrite(AP_ENABLE, g_snd ? LOW : HIGH);          // LOW = amp on (as in the Freenove demos)
  if (g_es_handle) es8311_voice_mute(g_es_handle, !g_snd);
}

static void saveSettings() {
  prefs.putInt("rot", g_rot); prefs.putBool("snd", g_snd); prefs.putInt("vol", g_vol);
  prefs.putBool("afeed", g_auto); prefs.putInt("aint", g_aint); prefs.putBool("h24", g_h24);
}

static void audioStart() {
  pinMode(AP_ENABLE, OUTPUT);
  digitalWrite(AP_ENABLE, LOW);
  Wire.begin(16, 15, 400000);
  i2s_config_t cfg = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = EXAMPLE_SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 6,
    .dma_buf_len = 256,
    .use_apll = false,
    .tx_desc_auto_clear = true,
    .fixed_mclk = EXAMPLE_MCLK_FREQ_HZ
  };
  if (i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL) != ESP_OK) { Serial.println("I2S install failed"); return; }
  i2s_pin_config_t pins = {.mck_io_num = I2S_MCK, .bck_io_num = I2S_BCK, .ws_io_num = I2S_WS,
                           .data_out_num = I2S_DOUT, .data_in_num = I2S_DINT};
  i2s_set_pin(I2S_NUM_0, &pins);
  if (es8311_codec_init() != ESP_OK) { Serial.println("ES8311 init failed"); return; }
  applySound();
  xTaskCreatePinnedToCore(audioTask, "bubbles", 4096, NULL, 2, NULL, 0);
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
  WiFi.setHostname(g_host.c_str());      // must come before WiFi.mode() or DHCP still sends the default name
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 8000) delay(100);
  configTzTime("EST5EDT,M3.2.0,M11.1.0", "pool.ntp.org", "time.nist.gov");
  // WiFi stays connected: NTP keeps syncing and OTA stays reachable.
  ArduinoOTA.setHostname(g_host.c_str());
#ifdef OTA_PASS
  ArduinoOTA.setPassword(OTA_PASS);
#endif
  ArduinoOTA.onStart([]() { tft.fillScreen(TFT_BLACK); tft.setTextColor(TFT_WHITE); tft.drawString("OTA update...", 10, 10, 4); });
  ArduinoOTA.onProgress([](unsigned int p, unsigned int total) {
    tft.fillRect(10, 60, 460, 20, TFT_DARKGREY);
    tft.fillRect(10, 60, (int)(460.0f * p / total), 20, TFT_GREEN);
  });
  ArduinoOTA.begin();
  MDNS.begin(g_host.c_str());
  MDNS.addService("http", "tcp", 80);
  Serial.printf("WiFi %s, IP %s, host %s\n", WiFi.status() == WL_CONNECTED ? "connected" : "NOT connected", WiFi.localIP().toString().c_str(), g_host.c_str());
#endif
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
  frame.setTextDatum(TR_DATUM);
  frame.setTextColor(RGB(10, 40, 80));
  frame.drawString(buf, SW - 9, 9, 4);
  if (date[0]) frame.drawString(date, SW - 9, 37, 2);
  frame.setTextColor(RGB(235, 245, 255));
  frame.drawString(buf, SW - 10, 8, 4);
  if (date[0]) frame.drawString(date, SW - 10, 36, 2);
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

struct Ring { float x, y, age; bool on; };
Ring rings[6];
static void feedAt(float x, float y, int n) {
  for (int i = 0; i < n; i++) spawnFood(x + random(-8, 9), y + random(-4, 5));
  for (auto &r : rings) if (!r.on) { r = {x, y, 0, true}; break; }      // sonar ring
}

static void updateWorld(float dt, float t, uint32_t now) {
  static uint32_t nextFeed = 0;
  if (g_auto) {
    if (nextFeed == 0 || now >= nextFeed) {
      if (nextFeed != 0) feedAt(60 + random(SW - 120), 16, 4);
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



// ------------------------------------------------------------- web page
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
<label>Screen rotation <select id="rot"><option value="3">USB on left</option><option value="1">USB on right</option></select></label></div>
<div class="c"><button id="feed">Feed the fish</button>
<label style="margin-top:14px">Auto feed <input type="checkbox" id="auto"></label>
<label>Every (seconds) <input type="number" id="aint" min="5" max="3600" style="width:90px"></label></div>
<div class="c"><button id="shot">Screenshot</button> <label style="display:inline"><input type="checkbox" id="live"> live view</label>
<small id="shm"> (the tank pauses for a second while it copies)</small>
<canvas id="cv" width="480" height="320" style="display:none;width:100%;margin-top:10px;border-radius:8px"></canvas>
<a id="dl" style="display:none" class="b">Download PNG</a></div>
<div class="c"><button id="apply">Apply</button> <small id="amsg"></small><br><small>Applies sound, volume, rotation and auto feed.</small></div>
<div class="c"><label>Hostname <input id="host" maxlength="31" style="width:55%"></label>
<small>letters, digits and dashes &mdash; reachable as <b id="hl"></b>.local</small><br><br>
<button id="hsave">Save &amp; reboot</button> <small id="hmsg"></small></div>
<script>
const $=i=>document.getElementById(i);let busy=0,dirty=0;
async function load(){const s=await (await fetch('/api/status')).json();
 if(!busy&&!dirty){$('h24').checked=s.h24;$('auto').checked=s.auto;$('aint').value=s.aint;if(document.activeElement!==$('host'))$('host').value=s.host;$('hl').textContent=s.host;$('snd').checked=s.snd;$('vol').value=s.vol;$('vv').textContent=s.vol;$('rot').value=s.rot}
 $('st').textContent='v'+s.ver+' · WiFi '+(s.ssid||'?')+' · '+s.ip+' · '+s.rssi+' dBm · up '+Math.floor(s.up/60)+' min · '+s.fps.toFixed(1)+' fps'}
async function send(){busy=1;await fetch('/api/set?snd='+($('snd').checked?1:0)+'&vol='+$('vol').value+'&rot='+$('rot').value+'&h24='+($('h24').checked?1:0)+'&auto='+($('auto').checked?1:0)+'&aint='+$('aint').value,{method:'POST'});busy=0;dirty=0;$('amsg').textContent='Applied \u2713'}
const mark=()=>{dirty=1;$('amsg').textContent='not applied yet'};
['snd','h24','auto','aint','rot'].forEach(i=>$(i).onchange=mark);$('vol').oninput=()=>{$('vv').textContent=$('vol').value;mark()};$('apply').onclick=send;
async function shot(){const r=await fetch('/api/frame',{cache:'no-store'});const b=new Uint8Array(await r.arrayBuffer());
 if(b.length!=480*320*2)return;const cv=$('cv'),x=cv.getContext('2d'),im=x.createImageData(480,320);
 for(let i=0,o=0;i<b.length;i+=2,o+=4){const v=(b[i]<<8)|b[i+1];im.data[o]=((v>>11)&31)*255/31;im.data[o+1]=((v>>5)&63)*255/63;im.data[o+2]=(v&31)*255/31;im.data[o+3]=255}
 x.putImageData(im,0,0);cv.style.display='block';
 cv.toBlob(bl=>{const a=$('dl');a.href=URL.createObjectURL(bl);a.download='fishtank-'+new Date().toISOString().replace(/[:.]/g,'-').slice(0,19)+'.png';a.textContent='Download PNG';a.style.display='inline-block'})}
$('shot').onclick=shot;
$('live').onchange=()=>{clearInterval(window.lv);if($('live').checked){shot();window.lv=setInterval(shot,2500)}};
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
  char b[420];
  snprintf(b, sizeof(b), "{\"h24\":%s,\"ssid\":\"%s\",\"snd\":%s,\"vol\":%d,\"rot\":%d,\"auto\":%s,\"aint\":%d,\"host\":\"%s\",\"ver\":\"" FW_VERSION "\",\"ip\":\"%s\",\"rssi\":%d,\"up\":%lu,\"fps\":%.1f}",
           g_h24 ? "true" : "false", ssid.c_str(), g_snd ? "true" : "false", (int)g_vol, g_rot, g_auto ? "true" : "false", g_aint, g_host.c_str(), WiFi.localIP().toString().c_str(), WiFi.RSSI(), millis() / 1000UL, g_fps);
  return String(b);
}

static void setupWeb() {
  server.on("/", HTTP_GET, []() { server.send_P(200, "text/html", PAGE); });
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
    if (server.hasArg("h24")) g_h24 = server.arg("h24").toInt() != 0;
    if (server.hasArg("auto")) g_auto = server.arg("auto").toInt() != 0;
    if (server.hasArg("aint")) g_aint = constrain(server.arg("aint").toInt(), 5, 3600);
    if (server.hasArg("rot")) {
      int r = server.arg("rot").toInt();
      if (r == 1 || r == 3) { g_rot = r; tft.setRotation(r); }
    }
    saveSettings(); applySound();
    server.send(200, "application/json", statusJson());
  });
  // Raw copy of the last finished frame (480x320 RGB565, big-endian) for the page's screenshot button.
  server.on("/api/frame", HTTP_GET, []() {
    const size_t total = SW * SH * 2;
    const uint8_t *p = (const uint8_t *)frame.getPointer();
    server.setContentLength(total);
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/octet-stream", "");
    for (size_t off = 0; off < total; off += 4096) server.sendContent((const char *)p + off, min((size_t)4096, total - off));
  });
  server.on("/api/feed", HTTP_ANY, []() {
    feedAt(50 + random(SW - 100), 16, 5);
    server.send(200, "application/json", "{\"ok\":true}");
  });
  server.begin();
}

// BOOT button shows connection info for a while
static uint32_t infoUntil = 0;
static void drawInfo(uint32_t now) {
  frame.fillRoundRect(30, 50, 420, 210, 12, RGB(8, 28, 52));
  frame.drawRoundRect(30, 50, 420, 210, 12, RGB(120, 190, 240));
  frame.setTextDatum(TL_DATUM);
  frame.setTextColor(RGB(255, 255, 255));
  frame.drawString("Fish Tank  v" FW_VERSION, 46, 60, 4);
  char b[80];
  frame.setTextColor(RGB(170, 220, 255));
  snprintf(b, sizeof(b), "http://%s.local", g_host.c_str());
  frame.drawString(b, 46, 100, 4);
  snprintf(b, sizeof(b), "http://%s", WiFi.localIP().toString().c_str());
  frame.drawString(b, 46, 132, 4);
  frame.setTextColor(RGB(220, 235, 250));
  snprintf(b, sizeof(b), "WiFi: %s  (%d dBm)", WiFi.status() == WL_CONNECTED ? WiFi.SSID().c_str() : "not connected", WiFi.RSSI());
  frame.drawString(b, 46, 172, 2);
  snprintf(b, sizeof(b), "Uptime %lu min   Free heap %u KB   %.1f fps", millis() / 60000UL, (unsigned)(ESP.getFreeHeap() / 1024), g_fps);
  frame.drawString(b, 46, 194, 2);
  snprintf(b, sizeof(b), "Sound %s, volume %d   Rotation: USB %s", g_snd ? "on" : "off", (int)g_vol, g_rot == 3 ? "left" : "right");
  frame.drawString(b, 46, 216, 2);
  frame.setTextColor(RGB(140, 170, 200));
  snprintf(b, sizeof(b), "Press BOOT to close (closes in %lu s)", (unsigned long)((infoUntil - now) / 1000));
  frame.drawString(b, 46, 238, 2);
}

void setup() {
  Serial.begin(115200);
  prefs.begin("tank", false);
  g_rot = prefs.getInt("rot", 3); g_snd = prefs.getBool("snd", true); g_vol = prefs.getInt("vol", 60);
  g_auto = prefs.getBool("afeed", false); g_aint = prefs.getInt("aint", 60); g_h24 = prefs.getBool("h24", false); g_host = prefs.getString("host", HOSTNAME);
  pinMode(0, INPUT_PULLUP);        // BOOT button
  audioStart();          // must run before ft6336u.begin(): both share I2C on GPIO 15/16
  ft6336u.begin();
  tft.init();
  tft.setRotation(g_rot);   // 3 = USB port on the left
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
#ifdef WIFI_SSID
  setupWeb();
#endif
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
  server.handleClient();
  if (restartAt && millis() > restartAt) ESP.restart();
  static uint32_t lastWifi = 0;
  if (WiFi.status() != WL_CONNECTED && now - lastWifi > 30000) { lastWifi = now; WiFi.reconnect(); }
#endif
  static bool lastBtn = true;
  bool btn = digitalRead(0);
  if (!btn && lastBtn) infoUntil = (infoUntil > now) ? 0 : now + 20000;
  lastBtn = btn;
  FT6336U_TouchPointType tp = ft6336u.scan();
  if (tp.touch_count != 0 && now - lastTouch > 120) {
    lastTouch = now;
    float x = g_rot == 3 ? SW - tp.tp[0].y : tp.tp[0].y;
    float y = g_rot == 3 ? tp.tp[0].x : SH - tp.tp[0].x;
    feedAt(x, max(y, 14.0f), 3);
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
  for (auto &r : rings) {
    if (!r.on) continue;
    for (int k = 0; k < 2; k++) {
      float a = r.age - k * 0.25f;
      if (a <= 0 || a > 1.4f) continue;
      float b = 1.0f - a / 1.4f;
      uint16_t c = RGB((int)(60 + 170 * b), (int)(120 + 120 * b), (int)(170 + 85 * b));
      int rad = (int)(a * 55);
      frame.drawCircle((int)r.x, (int)r.y, rad, c);
      frame.drawCircle((int)r.x, (int)r.y, rad + 1, c);
    }
  }
  for (auto &b : bubbles) {
    int bx = (int)(b.x + 5 * sinf(t * b.v * 0.08f + b.ph)), by = (int)b.y, r = (int)b.r;
    frame.drawCircle(bx, by, r, RGB(205, 232, 255));
    frame.drawPixel(bx - r / 3, by - r / 3, TFT_WHITE);
  }
  drawClock();
  if (infoUntil > now) drawInfo(now);

  frame.pushSprite(0, 0);

  frames++;
  if (now - fpsT >= 5000) {
    g_fps = frames * 1000.0f / (now - fpsT);
    Serial.printf("fps: %.1f\n", g_fps);
    frames = 0; fpsT = now;
  }
}
