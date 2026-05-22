#ifndef DISPLAY_COLUMN_H
#define DISPLAY_COLUMN_H

#include <stdint.h>

#include "app_config.h"
#include "sample_ring.h"

#define MAX_SAMPLES_PER_COL ((RING_CAPACITY + N_VIEW - 1) / N_VIEW + 1)

typedef struct {
    uint16_t analog[MAX_SAMPLES_PER_COL];
    uint16_t digital[N_DIGITAL];
    uint16_t analog_median;
    uint16_t n_samples;
} PixelColumn;

typedef enum {
    DIGITAL_STATE_EMPTY = 0,
    DIGITAL_STATE_ZERO,
    DIGITAL_STATE_ONE,
    DIGITAL_STATE_MIXED,
} DigitalColumnState;

void clear_pixel_column(PixelColumn *col);
void append_sample_to_pixel_column(PixelColumn *col, uint16_t sample);
void finalize_pixel_column(PixelColumn *col);
DigitalColumnState digital_column_state(const PixelColumn *pixel_col, int ch);

#endif
