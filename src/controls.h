#ifndef CONTROLS_H
#define CONTROLS_H

#include "app_state.h"

typedef enum {
    CONTROL_ACTION_TOGGLE_HOLD = 0,
    CONTROL_ACTION_CYCLE_ACQUISITION_MODE,
    CONTROL_ACTION_TOGGLE_ANALOG_PLOT_MODE,
    CONTROL_ACTION_SHIFT_TRIGGER_CHANNEL,
    CONTROL_ACTION_SHIFT_TRIGGER_POSITION,
    CONTROL_ACTION_TOGGLE_TRIGGER_EDGE,
    CONTROL_ACTION_SET_TRIGGER_THRESHOLD,
} ControlActionType;

typedef struct {
    ControlActionType type;
    int value;
} ControlAction;

void apply_control_action(AppState *app, ControlAction action);

#endif
