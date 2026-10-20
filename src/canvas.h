// Draws in the 480x320 virtual world onto a TFT_eSprite that may be smaller (CYD) and/or a horizontal strip of the panel.
#pragma once
#include <TFT_eSPI.h>
#include <math.h>

struct Canvas {
  TFT_eSprite *s = nullptr;
  float sx = 1, sy = 1;        // virtual -> panel scale
  int oy = 0;                  // panel row where this sprite's row 0 sits

  inline int X(float x) const { return (int)lroundf(x * sx); }
  inline int Y(float y) const { return (int)lroundf(y * sy) - oy; }
  inline int Rx(float r) const { int v = (int)lroundf(r * sx); return v < 1 ? 1 : v; }
  inline int Ry(float r) const { int v = (int)lroundf(r * sy); return v < 1 ? 1 : v; }
  inline int R(float r) const { int v = (int)lroundf(r * (sx + sy) * 0.5f); return v < 1 ? 1 : v; }

  void fillRect(float x, float y, float w, float h, uint16_t c) { s->fillRect(X(x), Y(y), Rx(w), Ry(h), c); }
  void fillRoundRect(float x, float y, float w, float h, float r, uint16_t c) { s->fillRoundRect(X(x), Y(y), Rx(w), Ry(h), R(r), c); }
  void drawRoundRect(float x, float y, float w, float h, float r, uint16_t c) { s->drawRoundRect(X(x), Y(y), Rx(w), Ry(h), R(r), c); }
  void fillEllipse(float x, float y, float rx, float ry, uint16_t c) { s->fillEllipse(X(x), Y(y), Rx(rx), Ry(ry), c); }
  void drawEllipse(float x, float y, float rx, float ry, uint16_t c) { s->drawEllipse(X(x), Y(y), Rx(rx), Ry(ry), c); }
  void fillCircle(float x, float y, float r, uint16_t c) { s->fillCircle(X(x), Y(y), R(r), c); }
  void drawCircle(float x, float y, float r, uint16_t c) { s->drawCircle(X(x), Y(y), R(r), c); }
  void fillTriangle(float x0, float y0, float x1, float y1, float x2, float y2, uint16_t c) {
    s->fillTriangle(X(x0), Y(y0), X(x1), Y(y1), X(x2), Y(y2), c);
  }
  void drawLine(float x0, float y0, float x1, float y1, uint16_t c) { s->drawLine(X(x0), Y(y0), X(x1), Y(y1), c); }
  void drawWideLine(float x0, float y0, float x1, float y1, float w, uint16_t c, uint16_t c2) {
    float wd = w * (sx + sy) * 0.5f; if (wd < 1) wd = 1;
    s->drawWideLine(x0 * sx, y0 * sy - oy, x1 * sx, y1 * sy - oy, wd, c, c2);
  }
  void drawPixel(float x, float y, uint16_t c) { s->drawPixel(X(x), Y(y), c); }
  void setTextDatum(uint8_t d) { s->setTextDatum(d); }
  void setTextColor(uint16_t c) { s->setTextColor(c); }
  void drawString(const char *str, float x, float y, int font) { s->drawString(str, X(x), Y(y), font); }
};
