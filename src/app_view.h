#ifndef APP_VIEW_H
#define APP_VIEW_H

#include "app_state.h"

extern const char * const CH_LABELS[TRIG_CH_COUNT];

void app_view_init(AppState *app);
void app_view_shutdown(AppState *app);
void app_view_draw(AppState *app);

#endif
