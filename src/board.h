// Board selection. Build envs: freenove4 (default) and cyd28.
#pragma once

#if defined(BOARD_CYD28)
  #define BOARD_NAME "CYD 2.8"
  #define PANEL_W 320
  #define PANEL_H 240
  #define STRIP_H 30            // no PSRAM: the tank is drawn and pushed in horizontal strips
  #define DEFAULT_ROT 1
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
  #define DEFAULT_ROT 3         // USB port on the left
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
