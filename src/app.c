#include <stdio.h>

#include "app.h"

#include "raylib.h"

#include "app_config.h"
#include "app_layout.h"
#include "app_logic.h"
#include "app_state.h"
#include "app_view.h"
#include "control_panel.h"

#define BG_COLOR ((Color){ 8, 12, 8, 255 })

static AppState g_app = {
    .control = {
        .mode = ACQ_MODE_RUN,
        .analog_plot_mode = ANALOG_PLOT_MODE_MEDIAN,
        .trigger_ch = TRIG_CH_A,
        .trigger_edge = TRIG_EDGE_RISING,
        .trigger_threshold = 512,
        .trigger_pos_idx = TRIG_POS_STEPS / 2,
    },
    .view_samples = N_VIEW,
    .live_scroll = {
        .columns_per_sample = 1.0f,
    },
    .signal_texture_cache = {
        .force_full = true,
    },
};

int app_run(void)
{
    ring_init(&g_app.ring);
    usb_reader_init(&g_app.usb, &g_app.ring);

    if (!usb_reader_start(&g_app.usb)) {
        fprintf(stderr, "Failed to start USB reader\n");
        ring_destroy(&g_app.ring);
        return 1;
    }

    SetTraceLogLevel(LOG_WARNING);

    // SetConfigFlags(FLAG_MSAA_4X_HINT);  // antialiasing (not effective in incremental rendering mode)

    InitWindow(WIN_W, WIN_H, "Oszilloskop GUI");
    SetTargetFPS(60);

    app_view_init(&g_app);

    while (!WindowShouldClose()) {
        update_zoom_from_wheel(&g_app);
        update_display_buffer(&g_app);

        BeginDrawing();
        ClearBackground(BG_COLOR);

        app_view_draw(&g_app);
        update_control_panel(&g_app);

        EndDrawing();
    }

    app_view_shutdown(&g_app);
    CloseWindow();
    usb_reader_stop(&g_app.usb);
    ring_destroy(&g_app.ring);
    return 0;
}