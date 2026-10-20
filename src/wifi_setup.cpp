// On-device WiFi setup: TouchWifiProvisioner (LVGL picker + keyboard) bridged onto the TFT_eSPI display and the
// board's touch (halTouch). LVGL is only used here, before the tank is online; the tank itself draws through TFT_eSPI.
#include <WiFi.h>
#include <TouchWifiProvisioner.h>
#include <lvgl.h>
#include "board.h"
#include "hal.h"
#include "wifi_setup.h"

static lv_disp_draw_buf_t drawBuf;
static lv_color_t buf1[PANEL_W * 10];

static void dispFlush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *colorP) {
  const uint32_t w = area->x2 - area->x1 + 1;
  const uint32_t h = area->y2 - area->y1 + 1;
  tft.startWrite();
  tft.setAddrWindow(area->x1, area->y1, w, h);
  tft.pushColors((uint16_t *)&colorP->full, w * h, true);
  tft.endWrite();
  lv_disp_flush_ready(disp);
}

static void touchpadRead(lv_indev_drv_t *, lv_indev_data_t *data) {
  int x, y;
  if (halTouch(x, y)) {
    data->state = LV_INDEV_STATE_PR;
    data->point.x = x;
    data->point.y = y;
  } else {
    data->state = LV_INDEV_STATE_REL;
  }
}

static std::function<void(const String &)> connectedCb;
static bool firedConnected = false;

void wifiSetupBegin(const char *hostname, std::function<void(const String &ip)> onConnected) {
  connectedCb = onConnected;
  firedConnected = false;

  lv_init();
  lv_disp_draw_buf_init(&drawBuf, buf1, nullptr, PANEL_W * 10);
  static lv_disp_drv_t dispDrv;
  lv_disp_drv_init(&dispDrv);
  dispDrv.hor_res = tft.width();
  dispDrv.ver_res = tft.height();
  dispDrv.flush_cb = dispFlush;
  dispDrv.draw_buf = &drawBuf;
  lv_disp_drv_register(&dispDrv);

  static lv_indev_drv_t indevDrv;
  lv_indev_drv_init(&indevDrv);
  indevDrv.type = LV_INDEV_TYPE_POINTER;
  indevDrv.read_cb = touchpadRead;
  lv_indev_drv_register(&indevDrv);

  lv_obj_set_style_bg_color(lv_scr_act(), lv_color_black(), 0);

  TouchWifiProvisioner::begin(lv_scr_act(), hostname, [](const String &ip) {
    if (firedConnected) return;            // also called again after later reconnects
    firedConnected = true;
    if (connectedCb) connectedCb(ip);
  });

  // hint on the top layer: the tank can run without WiFi
  lv_obj_t *hint = lv_label_create(lv_layer_top());
  lv_label_set_text(hint, "BOOT button = skip WiFi");
  lv_obj_set_style_text_color(hint, lv_color_hex(0x8899aa), 0);
  lv_obj_align(hint, LV_ALIGN_BOTTOM_RIGHT, -4, -2);
}

void wifiSetupLoop() {
  lv_timer_handler();
  TouchWifiProvisioner::loop();
}

void wifiForgetAll() {
  for (const String &s : TouchWifiProvisioner::savedNetworks()) TouchWifiProvisioner::forget(s);
}
