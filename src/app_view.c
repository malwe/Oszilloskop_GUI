#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <assert.h>

#include "app_layout.h"
#include "app_view.h"

#include "app_logic.h"
#include "display_buffer.h"

#define GRID_COLOR        ((Color){ 32,  52,  32, 255})
#define TRACE_COLOR       ((Color){  0, 230,  55, 255})
#define RAW_TRACE_COLOR   ((Color){  0, 230,  55,  72})
#define BORDER_COLOR      ((Color){ 52,  80,  52, 255})
#define AXIS_LABEL_COLOR  ((Color){110, 155, 110, 255})
#define VIEW_BG_COLOR     ((Color){  8,  12,   8, 255})

static const Color DIG_COLORS[N_DIGITAL] = {
    YELLOW,
    ORANGE,
    RED,
    MAGENTA,
    { 0, 140, 255, 255 },
    BEIGE,
};

static Texture2D g_view_texture = {0};
static Color *g_view_pixels = NULL;
static bool g_view_texture_ready = false;

const char * const CH_LABELS[TRIG_CH_COUNT] = {
    "A", "D1", "D2", "D3", "D4", "D5", "D6"
};

static float ms_per_div(const AppState *app, uint32_t sample_rate)
{
    if (sample_rate == 0u) return 0.0f;
    return (app->view_samples * 1000.0f) / (10.0f * (float)sample_rate);
}

static int analog_y_int_from_adc_local(int adc)
{
    return ANALOG_H - 1 - (int)((long)adc * (ANALOG_H - 1) / 1023);
}

static float analog_y_float_from_adc_local(int adc)
{
    return ANALOG_H - 1 - ((float)adc * (ANALOG_H - 1) / 1023.0f);
}

static int digital_ybase_local(int ch)
{
    return (DIG_Y_START - SIGNAL_RENDER_Y) + ch * (DIG_CH_H + DIG_CH_GAP);
}

/* Clear the full software framebuffer to a solid background color. */
static void clear_framebuffer(Color color)
{
    if (g_view_pixels == NULL) {
        return;
    }

    for (size_t i = 0; i < (size_t)WIN_W * (size_t)WIN_H; i++) {
        g_view_pixels[i] = color;
    }
}

/* Upload the CPU framebuffer and draw it as one screen-sized texture. */
static void present_framebuffer(void)
{
    if (!g_view_texture_ready || g_view_pixels == NULL) {
        return;
    }

    UpdateTexture(g_view_texture, g_view_pixels);
    DrawTexture(g_view_texture, 0, 0, WHITE);
}

/* Blend one RGBA pixel into the software framebuffer if it lies on screen. */
static void put_pixel(int x, int y, Color color)
{
    assert(g_view_pixels != NULL &&
           x >= 0 && x < WIN_W &&
           y >= 0 && y < WIN_H);

    Color *dst = &g_view_pixels[(size_t)y * (size_t)WIN_W + (size_t)x];
    if (color.a == 255) {
        *dst = color;
        return;
    }

    unsigned alpha = color.a;
    unsigned inv_alpha = 255u - alpha;

    dst->r = (unsigned char)((color.r * alpha + dst->r * inv_alpha + 127u) / 255u);
    dst->g = (unsigned char)((color.g * alpha + dst->g * inv_alpha + 127u) / 255u);
    dst->b = (unsigned char)((color.b * alpha + dst->b * inv_alpha + 127u) / 255u);
    dst->a = 255u;
}

/* Blend one RGBA pixel into th software framebuffer if it lies on screen. */
static void put_pixel_y_antialias(int x, float y, Color color)
{
    assert(g_view_pixels != NULL &&
           x >= 0 && x < WIN_W &&
           y >= 0.0f && y < (float)WIN_H);

    Color *dst_top;
    if (y < WIN_H - 1.0f) {
        dst_top = &g_view_pixels[(size_t)(y + 1) * (size_t)WIN_W + (size_t)x];
    } else {
        dst_top = &g_view_pixels[(size_t)y * (size_t)WIN_W + (size_t)x];
    }
    Color *dst_bottom = &g_view_pixels[(size_t)y * (size_t)WIN_W + (size_t)x];

    unsigned alpha = color.a;

    float dy_top = y - (float)(int)y;
    float dy_bottom = 1.0f - dy_top;

    unsigned top_alpha = (unsigned)(dy_top * (float)alpha);
    unsigned bottom_alpha = (unsigned)(dy_bottom * (float)alpha);

    unsigned inv_top_alpha = 255u - top_alpha;
    unsigned inv_bottom_alpha = 255u - bottom_alpha;

    dst_top->r = (unsigned char)((color.r * top_alpha + dst_top->r * inv_top_alpha + 127u) / 255u);
    dst_top->g = (unsigned char)((color.g * top_alpha + dst_top->g * inv_top_alpha + 127u) / 255u);
    dst_top->b = (unsigned char)((color.b * top_alpha + dst_top->b * inv_top_alpha + 127u) / 255u);
    dst_top->a = 255u;

    dst_bottom->r = (unsigned char)((color.r * bottom_alpha + dst_bottom->r * inv_bottom_alpha + 127u) / 255u);
    dst_bottom->g = (unsigned char)((color.g * bottom_alpha + dst_bottom->g * inv_bottom_alpha + 127u) / 255u);
    dst_bottom->b = (unsigned char)((color.b * bottom_alpha + dst_bottom->b * inv_bottom_alpha + 127u) / 255u);
    dst_bottom->a = 255u;
}

static void draw_line_horizontal(int x0, int y, int x1, Color color)
{
    assert(g_view_pixels != NULL &&
           y >= 0 && y < WIN_H &&
           x0 >= 0 && x0 < WIN_W &&
           x1 >= 0 && x1 < WIN_W);

    if (x0 > x1) {
        int tmp = x0;
        x0 = x1;
        x1 = tmp;
    }

    if (color.a == 255) {
        Color *pixel = &g_view_pixels[(size_t)y * (size_t)WIN_W + (size_t)x0];
        for (int x = x0; x <= x1; x++) {
            *pixel++ = color;
        }
        return;
    }

    for (int x = x0; x <= x1; x++) {
        put_pixel(x, y, color);
    }
}

static void draw_line_vertical(int x, int y0, int y1, Color color)
{
    assert(g_view_pixels != NULL &&
           x >= 0 && x < WIN_W &&
           y0 >= 0 && y0 < WIN_H &&
           y1 >= 0 && y1 < WIN_H);

    if (y0 > y1) {
        int tmp = y0;
        y0 = y1;
        y1 = tmp;
    }

    if (color.a == 255) {
        Color *pixel = &g_view_pixels[(size_t)y0 * (size_t)WIN_W + (size_t)x];
        for (int y = y0; y <= y1; y++) {
            *pixel = color;
            pixel += WIN_W;
        }
        return;
    }

    for (int y = y0; y <= y1; y++) {
        put_pixel(x, y, color);
    }
}

static void draw_line_bresenham(int x0, int y0, int x1, int y1, Color color)
{
    assert(g_view_pixels != NULL &&
           x0 >= 0 && x0 < WIN_W &&
           y0 >= 0 && y0 < WIN_H &&
           x1 >= 0 && x1 < WIN_W &&
           y1 >= 0 && y1 < WIN_H);

    int dx = abs(x1 - x0);
    int dy = abs(y1 - y0);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx - dy;

    while (true) {
        if (color.a == 255) {
            Color *pixel = &g_view_pixels[(size_t)y0 * (size_t)WIN_W + (size_t)x0];
            *pixel = color;
        } else {
            put_pixel(x0, y0, color);
        }

        if (x0 == x1 && y0 == y1) break;

        int e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            x0 += sx;
        }
        if (e2 < dx) {
            err += dx;
            y0 += sy;
        }
    }
}

/* Fill one axis-aligned rectangle in the software framebuffer. */
static void fill_rect(int x, int y, int width, int height, Color color)
{
    assert(g_view_pixels != NULL &&
           x >= 0 && x < WIN_W &&
           y >= 0 && y < WIN_H);

    if (width <= 0 || height <= 0) {
        return;
    }

    int x_end = x + width - 1;
    int y_end = y + height - 1;
    for (int yy = y; yy <= y_end; yy++) {
        draw_line_horizontal(x, yy, x_end, color);
    }
}

/* Draw a 1-pixel outline rectangle in the software framebuffer. */
static void draw_rect_outline(int x, int y, int width, int height, Color color)
{
    assert(g_view_pixels != NULL &&
           x >= 0 && x < WIN_W &&
           y >= 0 && y < WIN_H);

    if (width <= 0 || height <= 0) {
        return;
    }

    int x_end = x + width - 1;
    int y_end = y + height - 1;
    draw_line_horizontal(x, y, x_end, color);
    draw_line_horizontal(x, y_end, x_end, color);
    draw_line_vertical(x, y, y_end, color);
    draw_line_vertical(x_end, y, y_end, color);
}

/* Fill a small triangle marker directly into the software framebuffer. */
static void fill_triangle(int x0, int y0,
                                   int x1, int y1,
                                   int x2, int y2,
                                   Color color)
{
    assert(g_view_pixels != NULL &&
           x0 >= 0 && x0 < WIN_W &&
           y0 >= 0 && y0 < WIN_H &&
           x1 >= 0 && x1 < WIN_W &&
           y1 >= 0 && y1 < WIN_H &&
           x2 >= 0 && x2 < WIN_W &&
           y2 >= 0 && y2 < WIN_H);

    int min_x = x0;
    int max_x = x0;
    int min_y = y0;
    int max_y = y0;

    if (x1 < min_x) min_x = x1;
    if (x2 < min_x) min_x = x2;
    if (x1 > max_x) max_x = x1;
    if (x2 > max_x) max_x = x2;
    if (y1 < min_y) min_y = y1;
    if (y2 < min_y) min_y = y2;
    if (y1 > max_y) max_y = y1;
    if (y2 > max_y) max_y = y2;

    if (min_x < 0) min_x = 0;
    if (min_y < 0) min_y = 0;
    if (max_x >= WIN_W) max_x = WIN_W - 1;
    if (max_y >= WIN_H) max_y = WIN_H - 1;

    long area = (long)(x1 - x0) * (long)(y2 - y0) - (long)(y1 - y0) * (long)(x2 - x0);
    if (area == 0) {
        draw_line_bresenham(x0, y0, x1, y1, color);
        draw_line_bresenham(x1, y1, x2, y2, color);
        draw_line_bresenham(x2, y2, x0, y0, color);
        return;
    }

    for (int y = min_y; y <= max_y; y++) {
        for (int x = min_x; x <= max_x; x++) {
            long w0 = (long)(x1 - x0) * (long)(y - y0) - (long)(y1 - y0) * (long)(x - x0);
            long w1 = (long)(x2 - x1) * (long)(y - y1) - (long)(y2 - y1) * (long)(x - x1);
            long w2 = (long)(x0 - x2) * (long)(y - y2) - (long)(y0 - y2) * (long)(x - x2);

            if ((area > 0 && w0 >= 0 && w1 >= 0 && w2 >= 0) ||
                (area < 0 && w0 <= 0 && w1 <= 0 && w2 <= 0)) {
                put_pixel(x, y, color);
            }
        }
    }
}

static void draw_grid(void)
{
    for (int i = 0; i <= 10; i++) {
        int y = ANALOG_Y + (i * ANALOG_H) / 10;
        draw_line_horizontal(PLOT_X, y, PLOT_X_LAST, GRID_COLOR);
    }
    for (int i = 0; i <= 10; i++) {
        int x = PLOT_X + (i * (PLOT_W - 1)) / 10;
        draw_line_vertical(x, ANALOG_Y, ANALOG_Y + ANALOG_H, GRID_COLOR);
    }
    draw_rect_outline(PLOT_X, ANALOG_Y, PLOT_W, ANALOG_H, BORDER_COLOR);
}

/* Draw only the y-axis tick marks into the software framebuffer. */
static void draw_yaxis_ticks(void)
{
    static const struct { int adc; const char *label; } ticks[] = {
        {1023, "5V"}, {818, "4V"}, {614, "3V"}, {409, "2V"}, {205, "1V"}, {0, "0V"},
    };

    (void)ticks[0].label;
    for (int i = 0; i < 6; i++) {
        int y = ANALOG_Y + ANALOG_H - 1
              - (int)((long)ticks[i].adc * (ANALOG_H - 1) / 1023);
        draw_line_horizontal(PLOT_X - 4, y, PLOT_X, AXIS_LABEL_COLOR);
    }
}

/* Draw only the y-axis labels using raylib text rendering. */
static void draw_yaxis_labels(void)
{
    static const struct { int adc; const char *label; } ticks[] = {
        {1023, "5V"}, {818, "4V"}, {614, "3V"}, {409, "2V"}, {205, "1V"}, {0, "0V"},
    };

    for (int i = 0; i < 6; i++) {
        int y = ANALOG_Y + ANALOG_H - 1
              - (int)((long)ticks[i].adc * (ANALOG_H - 1) / 1023);
        DrawText(ticks[i].label, 4, y - 10, 20, AXIS_LABEL_COLOR);
    }
}

static void draw_analog_median_range(const AppState *app, int start_x, int x_offset, int y_offset)
{
    assert(start_x >= 0 && start_x < PLOT_W);

    int prev_x = -1;
    int prev_y = 0;

    for (int x = start_x; x < PLOT_W; x++) {
        const PixelColumn *pixel_col = pixel_column_for_screen_x_const(app, x);
        if (pixel_col->n_samples < 1) continue;

        int current_y = y_offset + analog_y_int_from_adc_local(pixel_col->analog_median);
        if (prev_x >= 0) {
            draw_line_bresenham(x_offset + prev_x,
                                prev_y,
                                x_offset + x,
                                current_y,
                                TRACE_COLOR);
        }

        prev_x = x;
        prev_y = current_y;
    }
}

static void draw_analog_raw_range(const AppState *app, int start_x, int x_offset, int y_offset)
{
    assert(start_x >= 0 && start_x < PLOT_W);

    for (int x = start_x; x < PLOT_W; x++) {
        const PixelColumn *pixel_col = pixel_column_for_screen_x_const(app, x);

        for (uint16_t sample_idx = 0; sample_idx < pixel_col->n_samples; sample_idx++) {
            float sample_y = y_offset + analog_y_float_from_adc_local(pixel_col->analog[sample_idx]);
            put_pixel_y_antialias(x_offset + x, sample_y, RAW_TRACE_COLOR);
        }
    }
}

static void draw_digital_range(const AppState *app, int start_x, int x_offset, int y_offset)
{
    assert(start_x >= 0 && start_x < PLOT_W);

    for (int ch = 0; ch < N_DIGITAL; ch++) {
        int ybase = y_offset + digital_ybase_local(ch);
        int y_high = ybase + 3;
        int y_low = ybase + DIG_CH_H - 4;
        int prev_x = -1;
        DigitalColumnState prev_state = DIGITAL_STATE_EMPTY;

        for (int x = start_x; x < PLOT_W; x++) {
            const PixelColumn *pixel_col = pixel_column_for_screen_x_const(app, x);
            DigitalColumnState state = digital_column_state(pixel_col, ch);
            if (state == DIGITAL_STATE_EMPTY) continue;

            if (state == DIGITAL_STATE_MIXED) {
                draw_line_vertical(x_offset + x, y_high, y_low, DIG_COLORS[ch]);
                prev_x = x;
                prev_state = state;
                continue;
            }

            if (state == DIGITAL_STATE_ZERO) {
                if (prev_state == DIGITAL_STATE_ONE) {
                    draw_line_bresenham(x_offset + prev_x, y_high,
                                         x_offset + x, y_low,
                                         DIG_COLORS[ch]);
                } else if (prev_x >= 0) {
                    draw_line_horizontal(x_offset + prev_x, y_low,
                                         x_offset + x,
                                         DIG_COLORS[ch]);
                } else {
                    put_pixel(x_offset + x, y_low, DIG_COLORS[ch]);
                }
            } else {
                if (prev_state == DIGITAL_STATE_ZERO) {
                    draw_line_bresenham(x_offset + prev_x, y_low,
                                         x_offset + x, y_high,
                                         DIG_COLORS[ch]);
                } else if (prev_x >= 0) {
                    draw_line_horizontal(x_offset + prev_x, y_high,
                                         x_offset + x,
                                         DIG_COLORS[ch]);
                } else {
                    put_pixel(x_offset + x, y_high, DIG_COLORS[ch]);
                }
            }

            prev_x = x;
            prev_state = state;
        }
    }
}

/* Draw the digital channel background panels into the software framebuffer. */
static void draw_digital_backgrounds(const AppState *app)
{
    for (int ch = 0; ch < N_DIGITAL; ch++) {
        int ybase = DIG_Y_START + ch * (DIG_CH_H + DIG_CH_GAP);
        bool is_trig_ch = trigger_active(app) &&
                          (app->control.trigger_ch == (TriggerChannel)(TRIG_CH_D1 + ch));

        fill_rect(PLOT_X, ybase, PLOT_W, DIG_CH_H, (Color){13, 13, 20, 255});
        draw_rect_outline(PLOT_X, ybase, PLOT_W, DIG_CH_H, (Color){38, 38, 55, 255});
        if (is_trig_ch) {
            fill_rect(2, ybase + 2, 58, DIG_CH_H - 4, (Color){70, 70, 70, 200});
        }
    }
}

/* Draw the digital channel labels using raylib text rendering. */
static void draw_digital_labels(const AppState *app)
{
    for (int ch = 0; ch < N_DIGITAL; ch++) {
        int ybase = DIG_Y_START + ch * (DIG_CH_H + DIG_CH_GAP);
        bool is_trig_ch = trigger_active(app) &&
                          (app->control.trigger_ch == (TriggerChannel)(TRIG_CH_D1 + ch));

        DrawText(CH_LABELS[ch + 1], 5, ybase + (DIG_CH_H - 20) / 2, 20,
                 is_trig_ch ? WHITE : DIG_COLORS[ch]);
    }
}

static void draw_analog(const AppState *app)
{
    if (app->control.analog_plot_mode == ANALOG_PLOT_MODE_RAW) {
        draw_analog_raw_range(app, 0, PLOT_X, SIGNAL_RENDER_Y);
    } else {
        draw_analog_median_range(app, 0, PLOT_X, SIGNAL_RENDER_Y);
    }

    if (trigger_active(app) && is_analog_channel(app->control.trigger_ch)) {
        int ty = ANALOG_Y + ANALOG_H - 1
               - (int)((long)app->control.trigger_threshold * (ANALOG_H - 1) / 1023);
        draw_line_horizontal(PLOT_X, ty, PLOT_X_LAST, (Color){255, 220, 0, 160});
        fill_triangle(PLOT_X - 8, ty,
                               PLOT_X, ty - 5,
                               PLOT_X, ty + 5,
                               (Color){255, 220, 0, 200});
    }
}

static void draw_digital(const AppState *app)
{
    draw_digital_range(app, 0, PLOT_X, SIGNAL_RENDER_Y);
}

static void draw_status(const AppState *app)
{
    bool conn = atomic_load(&app->usb.connected);
    uint32_t rate = atomic_load(&app->usb.sample_rate);
    uint32_t errs = atomic_load(&app->usb.error_count);
    uint32_t incons = atomic_load(&app->usb.data_inconsistency_count);

    DrawText(conn ? "USB connected" : "USB searching...",
             PLOT_X, STATUS_Y, 20,
             conn ? LIGHTGRAY : RED);

    const uint32_t spacing = 40;
    uint32_t x = PLOT_X + MeasureText("USB searching...", 20) + spacing;

    char buf[64];
    snprintf(buf, sizeof(buf), "%.2f kS/s", (double)rate / 1000);
    DrawText(buf, x, STATUS_Y, 20, LIGHTGRAY);

    x += MeasureText(buf, 20) + spacing;

    snprintf(buf, sizeof(buf), "%.2f ms/div", (double)ms_per_div(app, rate));
    DrawText(buf, x, STATUS_Y, 20, LIGHTGRAY);

    x += MeasureText(buf, 20) + spacing;

    if (errs > 0) {
        snprintf(buf, sizeof(buf), "Errors: %u", errs);
        DrawText(buf, x, STATUS_Y, 20, ORANGE);
        x += MeasureText(buf, 20) + spacing;
    }

    if (incons > 0) {
        snprintf(buf, sizeof(buf), "Data Inconsistency: %u", incons);
        DrawText(buf, x, STATUS_Y, 20, ORANGE);
    }
}

static void draw_time_axis(const AppState *app)
{
    uint32_t rate = atomic_load(&app->usb.sample_rate);
    if (rate == 0u) return;

    int ref_x = trigger_active(app) ? app->control.trigger_pos_x : (N_VIEW - 1);

    for (int i = 0; i <= 10; i++) {
        int plot_x = (i * (PLOT_W - 1)) / 10;
        int x = PLOT_X + plot_x;
        float dx_pixels = (float)plot_x - (float)ref_x;
        float dx_samples = (dx_pixels * (app->view_samples - 1.0f)) / (float)(N_VIEW - 1);
        float t_ms = (dx_samples * 1000.0f) / (float)rate;

        char lbl[24];
        snprintf(lbl, sizeof(lbl), "%.2f", (double)t_ms);
        int tw = MeasureText(lbl, 20);
        DrawText(lbl, x - tw / 2, ANALOG_Y + ANALOG_H + 5, 20,
                 (Color){95, 120, 95, 255});
    }
}

/* Rasterize all non-text view geometry into the software framebuffer. */
static void render_view_geometry(const AppState *app)
{
    clear_framebuffer(VIEW_BG_COLOR);
    draw_grid();
    draw_yaxis_ticks();
    draw_digital_backgrounds(app);
    draw_analog(app);
    draw_digital(app);

    if (trigger_active(app)) {
        int tx = PLOT_X + app->control.trigger_pos_x;
        draw_line_vertical(tx,
                           ANALOG_Y,
                           DIG_Y_START + N_DIGITAL * (DIG_CH_H + DIG_CH_GAP),
                           (Color){255, 220, 0, 120});
    }
}

void app_view_init(AppState *app)
{
    Image image = GenImageColor(WIN_W, WIN_H, VIEW_BG_COLOR);

    g_view_texture = LoadTextureFromImage(image);
    UnloadImage(image);

    g_view_pixels = malloc((size_t)WIN_W * (size_t)WIN_H * sizeof(*g_view_pixels));
    if (g_view_pixels == NULL) {
        UnloadTexture(g_view_texture);
        g_view_texture = (Texture2D){0};
        g_view_texture_ready = false;
        return;
    }

    clear_framebuffer(VIEW_BG_COLOR);
    SetTextureFilter(g_view_texture, TEXTURE_FILTER_POINT);
    UpdateTexture(g_view_texture, g_view_pixels);
    g_view_texture_ready = true;

    app->signal_texture_cache.signal_textures_ready = false;
    app->signal_texture_cache.signal_texture_valid = false;
    app->signal_texture_cache.shift_columns = 0;
    app->signal_texture_cache.force_full = true;
}

void app_view_shutdown(AppState *app)
{
    (void)app;

    if (g_view_texture_ready) {
        UnloadTexture(g_view_texture);
    }
    free(g_view_pixels);
    g_view_pixels = NULL;
    g_view_texture = (Texture2D){0};
    g_view_texture_ready = false;
}

void app_view_draw(AppState *app)
{
    render_view_geometry(app);
    present_framebuffer();

    draw_yaxis_labels();
    draw_time_axis(app);
    draw_digital_labels(app);
    draw_status(app);

    if (app->capture.hold && app->control.mode != ACQ_MODE_SINGLE) {
        DrawText("HOLD", PLOT_X + PLOT_W - 270, MENU_TEXT_H, 20, ORANGE);
    }
    if (trigger_active(app)) {
        DrawText(app->capture.waiting_for_trigger ? "TRIG: WAIT" : "TRIG: LOCK",
                 PLOT_X + PLOT_W - 200, MENU_TEXT_H, 20,
                 app->capture.waiting_for_trigger ? YELLOW : GREEN);
    }
    if (!app->capture.has_display_data) {
        DrawText("No samples yet", PLOT_X + 520, 210, 20, GRAY);
    }

    DrawFPS(WIN_W - 75, 4);
}
