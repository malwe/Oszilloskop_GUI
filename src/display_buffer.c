#include "display_buffer.h"

#include "app_config.h"
#include "display_column.h"

PixelColumn *pixel_column_for_screen_x(AppState *app, int x)
{
    return &app->pixel_columns[(app->pixel_columns_start + x) % N_VIEW];
}

const PixelColumn *pixel_column_for_screen_x_const(const AppState *app, int x)
{
    return &app->pixel_columns[(app->pixel_columns_start + x) % N_VIEW];
}

void push_pixel_column_right(AppState *app, const PixelColumn *col)
{
    app->pixel_columns_start = (app->pixel_columns_start + 1) % N_VIEW;
    *pixel_column_for_screen_x(app, N_VIEW - 1) = *col;
}

void fill_pixel_columns_from_window(AppState *app, const uint16_t *src, uint32_t src_count)
{
    app->pixel_columns_start = 0;

    for (uint32_t x = 0; x < N_VIEW; x++) {
        clear_pixel_column(&app->pixel_columns[x]);
    }

    if (src_count <= 1) return;

    for (uint32_t i = 0; i < src_count; i++) {
        uint32_t x = (i * (N_VIEW - 1)) / (src_count - 1);
        if (x >= N_VIEW) x = N_VIEW - 1;
        PixelColumn *pixel_col = pixel_column_for_screen_x(app, (int)x);

        append_sample_to_pixel_column(pixel_col, src[i]);
    }

    for (int x = 0; x < N_VIEW; x++) {
        finalize_pixel_column(&app->pixel_columns[x]);
    }
}
