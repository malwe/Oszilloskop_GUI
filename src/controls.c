#include "controls.h"
#include "app_config.h"

static bool control_trigger_channel_is_analog(TriggerChannel ch)
{
    return ch == TRIG_CH_A;
}

void apply_control_action(AppState *app, ControlAction action)
{
    switch (action.type) {
    case CONTROL_ACTION_TOGGLE_HOLD: {
        bool was_hold = app->capture.hold;
        app->capture.hold = !app->capture.hold;
        if (!was_hold && app->capture.hold) {
            app->frozen.capture_on_hold = true;
            app->frozen.valid = false;
            app->capture.waiting_for_trigger = false;
        } else if (was_hold && !app->capture.hold) {
            app->frozen.capture_on_hold = false;
            if (app->control.mode == ACQ_MODE_SINGLE && app->trigger_runtime.last_trigger_ring_idx_valid) {
                app->trigger_runtime.single_require_new_trigger = true;
                app->trigger_runtime.single_min_trigger_ring_idx = app->trigger_runtime.last_trigger_ring_idx + 1u;
                app->capture.waiting_for_trigger = true;
            } else {
                app->capture.waiting_for_trigger = false;
            }
        } else {
            app->capture.waiting_for_trigger = false;
        }
        break;
    }

    case CONTROL_ACTION_CYCLE_ACQUISITION_MODE:
        if (app->control.mode == ACQ_MODE_RUN) {
            app->control.mode = ACQ_MODE_SINGLE;
            app->capture.hold = false;
            app->capture.waiting_for_trigger = true;
        } else if (app->control.mode == ACQ_MODE_SINGLE) {
            app->control.mode = ACQ_MODE_CONT;
            app->capture.hold = false;
            app->capture.waiting_for_trigger = false;
        } else {
            app->control.mode = ACQ_MODE_RUN;
            app->capture.hold = false;
            app->capture.waiting_for_trigger = false;
        }
        app->trigger_runtime.single_require_new_trigger = false;
        app->frozen.capture_on_hold = false;
        break;

    case CONTROL_ACTION_TOGGLE_ANALOG_PLOT_MODE:
        app->control.analog_plot_mode =
            (app->control.analog_plot_mode == ANALOG_PLOT_MODE_MEDIAN)
                ? ANALOG_PLOT_MODE_RAW
                : ANALOG_PLOT_MODE_MEDIAN;
        break;

    case CONTROL_ACTION_SHIFT_TRIGGER_CHANNEL:
        app->control.trigger_ch = (TriggerChannel)((app->control.trigger_ch + TRIG_CH_COUNT + action.value) % TRIG_CH_COUNT);
        app->control.trigger_threshold = control_trigger_channel_is_analog(app->control.trigger_ch) ? 512 : 1;
        app->capture.waiting_for_trigger = false;
        break;

    case CONTROL_ACTION_SHIFT_TRIGGER_POSITION:
        app->control.trigger_pos_idx += action.value;
        if (app->control.trigger_pos_idx < 0) app->control.trigger_pos_idx = 0;
        if (app->control.trigger_pos_idx > TRIG_POS_STEPS) app->control.trigger_pos_idx = TRIG_POS_STEPS;
        app->capture.waiting_for_trigger = false;
        break;

    case CONTROL_ACTION_TOGGLE_TRIGGER_EDGE:
        app->control.trigger_edge = (app->control.trigger_edge == TRIG_EDGE_RISING)
            ? TRIG_EDGE_FALLING
            : TRIG_EDGE_RISING;
        app->capture.waiting_for_trigger = false;
        break;

    case CONTROL_ACTION_SET_TRIGGER_THRESHOLD:
        app->control.trigger_threshold = action.value;
        app->capture.waiting_for_trigger = false;
        break;
    }
}
