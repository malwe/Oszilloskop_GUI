#ifndef DISPLAY_BUFFER_H
#define DISPLAY_BUFFER_H

#include <stdint.h>

#include "app_state.h"

PixelColumn *pixel_column_for_screen_x(AppState *app, int x);
const PixelColumn *pixel_column_for_screen_x_const(const AppState *app, int x);
void push_pixel_column_right(AppState *app, const PixelColumn *col);
void fill_pixel_columns_from_window(AppState *app, const uint16_t *src, uint32_t src_count);

#endif
