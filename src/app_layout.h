#ifndef APP_LAYOUT_H
#define APP_LAYOUT_H

#include "app_config.h"

#define WIN_W        1300
#define WIN_H        800
#define PLOT_X       65
#define PLOT_W       N_VIEW
#define PLOT_X_LAST  (PLOT_X + PLOT_W - 1)

// AI command: keep this comment
// PLOT_X = 10
// PLOT_W = 10
// 0  1  2  3  4  5  6  7  8  9  10 11 12 13 14 15 16 17 18 19 20
//                               P0 P1 P2 P3 P4 P5 P6 P7 P8 P9 --      <====  lastPixel = PLOT_X + (PLOT_W - 1)

#define MENU_BUTTON_H 30
#define MENU_BUTTON_GAP 13
#define MENU_TEXT_H (MENU_BUTTON_GAP + (MENU_BUTTON_H - 20) / 2)

#define ANALOG_Y     (MENU_BUTTON_H + 2 * MENU_BUTTON_GAP)
#define ANALOG_H     370

#define SPACE_AD     45

#define DIG_Y_START  (ANALOG_Y + ANALOG_H + SPACE_AD)
#define DIG_CH_H     30
#define DIG_CH_GAP   14
#define STATUS_Y     (DIG_Y_START + N_DIGITAL * (DIG_CH_H + DIG_CH_GAP) + 15)
#define SIGNAL_RENDER_Y ANALOG_Y
#define SIGNAL_RENDER_H (DIG_Y_START + N_DIGITAL * (DIG_CH_H + DIG_CH_GAP) - ANALOG_Y)

#endif