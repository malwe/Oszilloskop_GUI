#include <assert.h>
#include <string.h>

#include "display_column.h"

static uint16_t analog_adc_from_sample(uint16_t sample)
{
    return (uint16_t)((sample >> 6) & 0x3FFu);
}

void clear_pixel_column(PixelColumn *col)
{
    memset(col, 0, sizeof(*col));
}

void append_sample_to_pixel_column(PixelColumn *col, uint16_t sample)
{
    assert(col->n_samples < MAX_SAMPLES_PER_COL);
    col->analog[col->n_samples] = analog_adc_from_sample(sample);

    for (int ch = 0; ch < N_DIGITAL; ch++) {
        if (((sample >> ch) & 1u) != 0u) {
            col->digital[ch]++;
        }
    }

    col->n_samples++;
}

void finalize_pixel_column(PixelColumn *col)
{
    if (col->n_samples == 0) {
        col->analog_median = 0;
        return;
    }

    uint16_t adc_counts[1024] = {0};
    for (uint16_t i = 0; i < col->n_samples; i++) {
        adc_counts[col->analog[i]]++;
    }

    uint32_t cumulative = 0;
    uint32_t target = (uint32_t)col->n_samples / 2u;

    for (uint16_t adc = 0; adc < 1024u; adc++) {
        cumulative += adc_counts[adc];
        if (cumulative > target) {
            col->analog_median = adc;
            return;
        }
    }

    col->analog_median = 1023;
}

DigitalColumnState digital_column_state(const PixelColumn *pixel_col, int ch)
{
    if (pixel_col->n_samples == 0) return DIGITAL_STATE_EMPTY;

    int ones = pixel_col->digital[ch];
    if (ones == 0) return DIGITAL_STATE_ZERO;
    if (ones == pixel_col->n_samples) return DIGITAL_STATE_ONE;
    return DIGITAL_STATE_MIXED;
}
