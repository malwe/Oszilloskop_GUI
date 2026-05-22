#ifndef APP_LOGIC_H
#define APP_LOGIC_H

#include <stdbool.h>
#include <stdint.h>

#include "app_state.h"

bool trigger_active(const AppState *app);
int trigger_pos_x_from_step_index(int idx);
bool is_analog_channel(TriggerChannel ch);
void update_zoom_from_wheel(AppState *app);
void update_display_buffer(AppState *app);

#endif
