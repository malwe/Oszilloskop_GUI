#include <stdio.h>

#include "control_panel.h"

#include "raylib.h"

#include "app_layout.h"
#include "app_logic.h"
#include "app_view.h"
#include "controls.h"

static float adc_to_volts(int adc)
{
    return ((float)adc * 5.0f) / 1023.0f;
}

static bool button(Rectangle rect, const char *text, bool active, bool enabled)
{
    Vector2 mouse = GetMousePosition();
    bool hover = enabled && CheckCollisionPointRec(mouse, rect);
    Color bg = active ? (Color){58, 34, 18, 255} : (Color){28, 35, 45, 255};
    Color border = (Color){70, 85, 110, 255};
    Color text_color = active ? ORANGE : RAYWHITE;

    if (!enabled) {
        bg = (Color){22, 28, 35, 255};
        border = (Color){55, 60, 70, 255};
        text_color = GRAY;
    } else if (hover) {
        border = (Color){130, 160, 200, 255};
    }

    DrawRectangleRec(rect, bg);
    DrawRectangleLinesEx(rect, 1.0f, border);
    DrawText(text,
             (int)(rect.x + (rect.width - MeasureText(text, 20)) / 2),
             (int)(rect.y + (rect.height - 20) / 2),
             20, text_color);

    return enabled && hover && IsMouseButtonPressed(MOUSE_LEFT_BUTTON);
}

static const char *mode_button_text(const AppState *app)
{
    switch (app->control.mode) {
    case ACQ_MODE_RUN:
        return "RUN";
    case ACQ_MODE_SINGLE:
        return "SINGLE";
    case ACQ_MODE_CONT:
        return "CONT";
    }

    return "RUN";
}

static const char *analog_plot_mode_text(const AppState *app)
{
    return app->control.analog_plot_mode == ANALOG_PLOT_MODE_RAW ? "RAW" : "MEDIAN";
}

void update_control_panel(AppState *app)
{
    const float y = MENU_BUTTON_GAP;

    float x = PLOT_X;
    float x_dist_small = 2.0f;
    float x_dist_medium = 12.0f;

    Rectangle mode_rect = { x, y, 105.0f, MENU_BUTTON_H };
    x += x_dist_medium + mode_rect.width;
    Rectangle action_rect = { x, y, 85.0f, MENU_BUTTON_H };
    x += x_dist_medium + action_rect.width;
    Rectangle analog_plot_rect = { x, y, 105.0f, MENU_BUTTON_H };
    x += x_dist_medium + analog_plot_rect.width;
    Rectangle ch_left_rect = { x, y, MENU_BUTTON_H, MENU_BUTTON_H };
    x += x_dist_small + ch_left_rect.width;
    Rectangle ch_rect = { x, y, 52.0f, MENU_BUTTON_H };
    x += x_dist_small + ch_rect.width;
    Rectangle ch_right_rect = { x, y, MENU_BUTTON_H, MENU_BUTTON_H };
    x += x_dist_medium + ch_right_rect.width;
    Rectangle pos_left_rect = { x, y, MENU_BUTTON_H, MENU_BUTTON_H };
    x += x_dist_small + pos_left_rect.width;
    Rectangle pos_rect = { x, y, 52.0f, MENU_BUTTON_H };
    x += x_dist_small + pos_rect.width;
    Rectangle pos_right_rect = { x, y, MENU_BUTTON_H, MENU_BUTTON_H };
    x += x_dist_medium + pos_right_rect.width;
    Rectangle edge_rect = { x, y, 105.0f, MENU_BUTTON_H };
    x += x_dist_medium + edge_rect.width;

    app->control.trigger_pos_x = trigger_pos_x_from_step_index(app->control.trigger_pos_idx);

    if (button(mode_rect, mode_button_text(app), false, true)) {
        apply_control_action(app, (ControlAction){
            .type = CONTROL_ACTION_CYCLE_ACQUISITION_MODE,
        });
    }

    if (app->control.mode == ACQ_MODE_SINGLE) {
        bool next_enabled = !app->capture.waiting_for_trigger;
        if (button(action_rect, "NEXT", next_enabled, next_enabled)) {
            apply_control_action(app, (ControlAction){
                .type = CONTROL_ACTION_TOGGLE_HOLD,
            });
        }
    } else if (button(action_rect, "HOLD", app->capture.hold, true)) {
        apply_control_action(app, (ControlAction){
            .type = CONTROL_ACTION_TOGGLE_HOLD,
        });
    }

    if (button(analog_plot_rect,
               analog_plot_mode_text(app),
               app->control.analog_plot_mode == ANALOG_PLOT_MODE_RAW,
               true)) {
        apply_control_action(app, (ControlAction){
            .type = CONTROL_ACTION_TOGGLE_ANALOG_PLOT_MODE,
        });
    }

    if (!trigger_active(app)) {
        return;
    }

    if (button(ch_left_rect, "<", false, true)) {
        apply_control_action(app, (ControlAction){
            .type = CONTROL_ACTION_SHIFT_TRIGGER_CHANNEL,
            .value = -1,
        });
    }

    DrawRectangleRec(ch_rect, (Color){28, 35, 45, 255});
    DrawRectangleLinesEx(ch_rect, 1.0f, (Color){70, 85, 110, 255});
    DrawText(CH_LABELS[app->control.trigger_ch],
             (int)(ch_rect.x + (ch_rect.width - MeasureText(CH_LABELS[app->control.trigger_ch], 20)) / 2),
             (int)(ch_rect.y + (ch_rect.height - 20) / 2),
             20, RAYWHITE);

    if (button(ch_right_rect, ">", false, true)) {
        apply_control_action(app, (ControlAction){
            .type = CONTROL_ACTION_SHIFT_TRIGGER_CHANNEL,
            .value = 1,
        });
    }

    if (button(pos_left_rect, "<", false, true)) {
        apply_control_action(app, (ControlAction){
            .type = CONTROL_ACTION_SHIFT_TRIGGER_POSITION,
            .value = -1,
        });
    }

    {
        char pos_text[16];
        snprintf(pos_text, sizeof(pos_text), "%d%%", (app->control.trigger_pos_idx * 100) / TRIG_POS_STEPS);
        DrawRectangleRec(pos_rect, (Color){28, 35, 45, 255});
        DrawRectangleLinesEx(pos_rect, 1.0f, (Color){70, 85, 110, 255});
        DrawText(pos_text,
                 (int)(pos_rect.x + (pos_rect.width - MeasureText(pos_text, 20)) / 2),
                 (int)(pos_rect.y + (pos_rect.height - 20) / 2),
                 20, RAYWHITE);
    }

    if (button(pos_right_rect, ">", false, true)) {
        apply_control_action(app, (ControlAction){
            .type = CONTROL_ACTION_SHIFT_TRIGGER_POSITION,
            .value = 1,
        });
    }

    if (button(edge_rect,
               app->control.trigger_edge == TRIG_EDGE_RISING ? "RISING" : "FALLING",
               false, true)) {
        apply_control_action(app, (ControlAction){
            .type = CONTROL_ACTION_TOGGLE_TRIGGER_EDGE,
        });
    }

    if (!is_analog_channel(app->control.trigger_ch)) {
        return;
    }

    Rectangle slider_rect = { x, y, 180.0f, MENU_BUTTON_H };
    DrawRectangleRec(slider_rect, (Color){35, 44, 52, 255});
    DrawRectangleLinesEx(slider_rect, 1.0f, (Color){78, 98, 118, 255});

    float threshold_fraction = (float)app->control.trigger_threshold / 1023.0f;
    float knob_x = slider_rect.x + threshold_fraction * slider_rect.width;
    DrawLine((int)knob_x, (int)slider_rect.y - 2,
             (int)knob_x, (int)(slider_rect.y + slider_rect.height + 2), YELLOW);

    Vector2 mouse = GetMousePosition();
    if (CheckCollisionPointRec(mouse, slider_rect) && IsMouseButtonDown(MOUSE_LEFT_BUTTON)) {
        float relative = (mouse.x - slider_rect.x) / slider_rect.width;
        if (relative < 0.0f) relative = 0.0f;
        if (relative > 1.0f) relative = 1.0f;
        apply_control_action(app, (ControlAction){
            .type = CONTROL_ACTION_SET_TRIGGER_THRESHOLD,
            .value = (int)(relative * 1023.0f + 0.5f),
        });
    }

    {
        char threshold_text[48];
        snprintf(threshold_text, sizeof(threshold_text), "%.2fV", (double)adc_to_volts(app->control.trigger_threshold));
        DrawText(threshold_text,
                 (int)(slider_rect.x + slider_rect.width + 10.0f),
                 MENU_TEXT_H,
                 20,
                 LIGHTGRAY);
    }
}