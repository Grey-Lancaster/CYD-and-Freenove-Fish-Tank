#include "board.h"
#ifdef BOARD_CYD28
// CYD 2.8" (ESP32-2432S028): ILI9341 320x240, bit-banged XPT2046 resistive touch, speaker on the
// internal DAC (GPIO26). No PSRAM. Touch code and the five-point calibration come from CYD PhotoFrame.
#include <Preferences.h>
#include "driver/i2s.h"
#include "hal.h"
#include "audio.h"

#define XPT2046_IRQ 36
#define XPT2046_MOSI 32
#define XPT2046_MISO 39
#define XPT2046_CLK 25
#define XPT2046_CS 33

void halBacklight(bool on) { digitalWrite(TFT_BL, on ? HIGH : LOW); }

// ---------------------------------------------------------------- display
// The rotation's MADCTL as TFT_eSPI sets it for the ILI9341, with red/blue optionally flipped.
void halApplyColors() {
  static const uint8_t baseFor[4] = {TFT_MAD_MX, TFT_MAD_MV, TFT_MAD_MY, TFT_MAD_MX | TFT_MAD_MY | TFT_MAD_MV};
  const uint8_t base = baseFor[tft.getRotation() & 3];
  const uint8_t order = g_swaprb ? (TFT_MAD_COLOR_ORDER ^ TFT_MAD_BGR) : TFT_MAD_COLOR_ORDER;
  tft.writecommand(TFT_MADCTL);
  tft.writedata(base | order);
  tft.invertDisplay(g_invert);
}

void halApplyRotation() {
  tft.setRotation(g_rot);
  halApplyColors();
}

void halDisplayBegin() {
  tft.init();
  halApplyRotation();
  tft.fillScreen(TFT_BLACK);
  tft.setSwapBytes(true);
}

// ---------------------------------------------------------------- touch
// The resistive panels differ between revisions (axes swapped, skewed, different ranges), so the
// first start asks for five touches and fits a transform from raw readings to screen pixels.
static float touchAff[6];
static bool haveAff = false;

static bool solve3(const double A[3][3], const double b[3], double out[3]) {
  auto det = [](const double m[3][3]) {
    return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
           m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
  };
  double d = det(A);
  if (fabs(d) < 1e-6) return false;
  for (int c = 0; c < 3; c++) {
    double M[3][3];
    memcpy(M, A, sizeof(M));
    for (int r = 0; r < 3; r++) M[r][c] = b[r];
    out[c] = det(M) / d;
  }
  return true;
}

// Least-squares fit of sx = a*rx + b*ry + c and sy = d*rx + e*ry + f; returns the RMS error in pixels.
static bool fitAffine(const double *rx, const double *ry, const double *sx, const double *sy, int n, float out[6], double &rms) {
  double A[3][3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, (double)n}};
  double bx[3] = {0, 0, 0}, by[3] = {0, 0, 0};
  for (int i = 0; i < n; i++) {
    A[0][0] += rx[i] * rx[i];
    A[0][1] += rx[i] * ry[i];
    A[0][2] += rx[i];
    A[1][1] += ry[i] * ry[i];
    A[1][2] += ry[i];
    bx[0] += rx[i] * sx[i]; bx[1] += ry[i] * sx[i]; bx[2] += sx[i];
    by[0] += rx[i] * sy[i]; by[1] += ry[i] * sy[i]; by[2] += sy[i];
  }
  A[1][0] = A[0][1];
  A[2][0] = A[0][2];
  A[2][1] = A[1][2];
  double px[3], py[3];
  if (!solve3(A, bx, px) || !solve3(A, by, py)) return false;
  double sum = 0;
  for (int i = 0; i < n; i++) {
    double ex = px[0] * rx[i] + px[1] * ry[i] + px[2] - sx[i];
    double ey = py[0] * rx[i] + py[1] * ry[i] + py[2] - sy[i];
    sum += ex * ex + ey * ey;
  }
  rms = sqrt(sum / n);
  for (int k = 0; k < 3; k++) {
    out[k] = (float)px[k];
    out[3 + k] = (float)py[k];
  }
  return true;
}

// XPT2046 read over bit-banged GPIO (its own pins, so it cannot disturb the display or SD
// card buses). The library this used to call read Y straight after X with no settling
// conversion, which made Y wrong by hundreds of counts on some panels; this version does a
// throw-away conversion first and takes the median of three samples per axis.
struct RawTouch {
  uint16_t xRaw, yRaw, zRaw;
};

static void xptWrite(uint8_t cmd) {
  for (int i = 7; i >= 0; i--) {
    digitalWrite(XPT2046_MOSI, cmd & (1 << i));
    digitalWrite(XPT2046_CLK, LOW);
    delayMicroseconds(5);
    digitalWrite(XPT2046_CLK, HIGH);
    delayMicroseconds(5);
  }
  digitalWrite(XPT2046_MOSI, LOW);
  digitalWrite(XPT2046_CLK, LOW);
}

static uint16_t xptConvert(uint8_t cmd) {
  xptWrite(cmd);
  uint16_t v = 0;
  for (int i = 15; i >= 0; i--) {
    digitalWrite(XPT2046_CLK, HIGH);
    delayMicroseconds(5);
    digitalWrite(XPT2046_CLK, LOW);
    delayMicroseconds(5);
    v |= digitalRead(XPT2046_MISO) << i;
  }
  return v >> 4;
}

static uint16_t median3(uint16_t a, uint16_t b, uint16_t c) {
  if (a > b) { uint16_t t = a; a = b; b = t; }
  if (b > c) { b = c; }
  return a > b ? a : b;
}

static void xptBegin() {
  pinMode(XPT2046_MOSI, OUTPUT);
  pinMode(XPT2046_MISO, INPUT);
  pinMode(XPT2046_CLK, OUTPUT);
  pinMode(XPT2046_CS, OUTPUT);
  digitalWrite(XPT2046_CS, HIGH);
  digitalWrite(XPT2046_CLK, LOW);
}

// True while the panel is pressed (pressure reads ~1000-2000 for a real touch).
static bool rawTouched(RawTouch &p) {
  digitalWrite(XPT2046_CS, LOW);
  int z = xptConvert(0xB1) + 4095 - xptConvert(0xC1);
  if (z < 400) {
    digitalWrite(XPT2046_CS, HIGH);
    return false;
  }
  xptConvert(0x91);  // first conversion after a channel change is unreliable
  xptConvert(0xD1);
  uint16_t x[3], y[3];
  for (int k = 0; k < 3; k++) {
    y[k] = xptConvert(0xD1);
    x[k] = xptConvert(0x91);
  }
  xptConvert(0xD0);  // back to power-down
  digitalWrite(XPT2046_CS, HIGH);
  p.xRaw = median3(x[0], x[1], x[2]);
  p.yRaw = median3(y[0], y[1], y[2]);
  p.zRaw = z;
  return true;
}


static bool loadAff() {
  Preferences p;
  p.begin("tank-touch", true);
  size_t n = p.getBytes("aff", touchAff, sizeof(touchAff));
  p.end();
  return n == sizeof(touchAff);
}

static void saveAff() {
  Preferences p;
  p.begin("tank-touch", false);
  p.putBytes("aff", touchAff, sizeof(touchAff));
  p.end();
}

static void calMessage(const char *l1, const char *l2, uint16_t col) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(col, TFT_BLACK);
  tft.drawString(l1, tft.width() / 2, tft.height() / 2 - 12, 4);
  tft.drawString(l2, tft.width() / 2, tft.height() / 2 + 16, 2);
}

// Calibrate in rotation 1; 180-degree rotation just mirrors the result.
static void calibrate() {
  uint8_t oldRot = tft.getRotation();
  tft.setRotation(1);
  halApplyColors();
  const int W = tft.width(), H = tft.height(), inset = 30;
  const int tx[5] = {inset, W - inset, W - inset, inset, W / 2};
  const int ty[5] = {inset, inset, H - inset, H - inset, H / 2};
  double rx[5], ry[5], sx[5], sy[5];
  float fit[6];
  double rms = 0;
  bool ok = false;
  calMessage("Touch calibration", "Touch each crosshair as shown", TFT_CYAN);
  delay(2000);
  RawTouch p;
  for (int attempt = 0; attempt < 3 && !ok; attempt++) {
    for (int i = 0; i < 5; i++) {
      tft.fillScreen(TFT_BLACK);
      tft.drawFastHLine(tx[i] - 14, ty[i], 29, TFT_WHITE);
      tft.drawFastVLine(tx[i], ty[i] - 14, 29, TFT_WHITE);
      tft.drawCircle(tx[i], ty[i], 8, TFT_GREEN);
      tft.setTextDatum(MC_DATUM);
      tft.setTextColor(TFT_YELLOW, TFT_BLACK);
      char msg[40];
      snprintf(msg, sizeof(msg), "Touch the crosshair %d/5", i + 1);
      tft.drawString(msg, W / 2, i == 4 ? H / 2 + 36 : H / 2, 2);
      while (rawTouched(p)) delay(10);       // let go of the previous target first
      delay(250);
      while (!rawTouched(p)) delay(10);
      double sumX = 0, sumY = 0;
      int n = 0;
      delay(60);                              // skip the contact bounce
      for (int k = 0; k < 24 && rawTouched(p); k++) { sumX += p.xRaw; sumY += p.yRaw; n++; delay(8); }
      if (n == 0) { i--; continue; }          // lifted too fast: ask again
      rx[i] = sumX / n; ry[i] = sumY / n;
      sx[i] = tx[i]; sy[i] = ty[i];
    }
    ok = fitAffine(rx, ry, sx, sy, 5, fit, rms) && rms <= 12.0;
    Serial.printf("Touch cal fit: %s, rms %.1f px\n", ok ? "ok" : "inconsistent", rms);
    if (!ok) { calMessage("Taps were not consistent", "Let's try again", TFT_ORANGE); delay(1800); }
  }
  tft.fillScreen(TFT_BLACK);
  if (ok) { memcpy(touchAff, fit, sizeof(touchAff)); haveAff = true; saveAff(); }
  tft.setRotation(oldRot);
  halApplyColors();
}

void halTouchBegin() {
  xptBegin();
  haveAff = loadAff();
  if (!haveAff) calibrate();
}

void halRecalibrateTouch() {
  Preferences p;
  p.begin("tank-touch", false);
  p.remove("aff");
  p.end();
  ESP.restart();
}

bool halTouch(int &x, int &y) {
  xptBegin();                    // sound can disturb these pins; cheap to reassert
  RawTouch p;
  if (!rawTouched(p)) return false;
  if (haveAff) {
    x = lroundf(touchAff[0] * p.xRaw + touchAff[1] * p.yRaw + touchAff[2]);
    y = lroundf(touchAff[3] * p.xRaw + touchAff[4] * p.yRaw + touchAff[5]);
  } else {                       // calibration failed: rough linear guess
    x = map(p.xRaw, 0, 4095, 0, 319);
    y = map(p.yRaw, 0, 4095, 0, 239);
  }
  x = constrain(x, 0, PANEL_W - 1);
  y = constrain(y, 0, PANEL_H - 1);
  if (g_rot == 3) { x = PANEL_W - 1 - x; y = PANEL_H - 1 - y; }
  return true;
}

// ---------------------------------------------------------------- audio (internal DAC, speaker on GPIO26)
void halApplySound() {}          // muting is done in audioMix

static void audioTask(void *) {
  static int16_t mono[256];
  static uint16_t out[512];
  for (;;) {
    audioMix(mono, 256);
    for (int i = 0; i < 256; i++) {
      uint16_t v = (uint16_t)(mono[i] + 32768);   // DAC wants offset-binary in the high byte
      out[2 * i] = v; out[2 * i + 1] = v;
    }
    size_t written;
    i2s_write(I2S_NUM_0, out, sizeof(out), &written, portMAX_DELAY);
  }
}

void halAudioStart() {
  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_DAC_BUILT_IN);
  cfg.sample_rate = 16000;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  cfg.communication_format = (i2s_comm_format_t)I2S_COMM_FORMAT_STAND_MSB;
  cfg.intr_alloc_flags = 0;
  cfg.dma_buf_count = 8;
  cfg.dma_buf_len = 256;
  cfg.use_apll = false;
  cfg.tx_desc_auto_clear = true;
  if (i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL) != ESP_OK) { Serial.println("I2S install failed"); return; }
  i2s_set_dac_mode(I2S_DAC_CHANNEL_LEFT_EN);   // GPIO26 only; GPIO25 is the touch controller's clock
  xTaskCreatePinnedToCore(audioTask, "bubbles", 4096, NULL, 2, NULL, 0);
}
#endif
