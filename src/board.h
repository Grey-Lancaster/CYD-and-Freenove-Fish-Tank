// Board selection. Build envs: freenove4 (default) and cyd28.
#pragma once

// 2-USB CYDs (ST7789) show red and blue swapped with the stock colour order, so that build starts with the swap on.
#if defined(ST7789_DRIVER)
  #define DEFAULT_SWAPRB true
  #define BOARD_ID "cyd28usb2"
#elif defined(BOARD_CYD28)
  #define DEFAULT_SWAPRB false
  #define BOARD_ID "cyd28"
#else
  #define DEFAULT_SWAPRB false
  #define BOARD_ID "freenove4"
#endif

#if defined(BOARD_CYD28)
  #define BOARD_NAME "CYD 2.8"
  #define PANEL_W 320
  #define PANEL_H 240
  #define STRIP_H 30            // no PSRAM: the tank is drawn and pushed in horizontal strips
  #define ROT_LEFT 3            // USB port on the left (assumes the USB ports are on the bottom edge in portrait)
  #define ROT_RIGHT 1
  #define DEFAULT_ROT ROT_LEFT
  #define DEFAULT_HOST "cydfishtank"
  #define TIME_Y 6
  #define DATE_Y 46
  #define FONT_BIG 2
  #define FONT_SMALL 1
#else
  #define BOARD_FREENOVE4 1
  #define BOARD_NAME "Freenove 4.0"
  #define PANEL_W 480
  #define PANEL_H 320
  #define ROT_LEFT 3            // USB port on the left
  #define ROT_RIGHT 1
  #define DEFAULT_ROT ROT_LEFT
  #define DEFAULT_HOST "fishtank"
  #define TIME_Y 8
  #define DATE_Y 37
  #define FONT_BIG 4
  #define FONT_SMALL 2
#endif

// The world (fish, food, bubbles...) always lives in a 480x320 "virtual" space; the Canvas maps it to the panel.
#define VW 480
#define VH 320
#define SAND_Y 270
