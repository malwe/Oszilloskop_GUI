#ifndef APP_STATE_H
#define APP_STATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "raylib.h"
#include "display_column.h"
#include "sample_ring.h"
#include "usb_reader.h"

typedef enum {
    TRIG_CH_A = 0,
    TRIG_CH_D1,
    TRIG_CH_D2,
    TRIG_CH_D3,
    TRIG_CH_D4,
    TRIG_CH_D5,
    TRIG_CH_D6,
    TRIG_CH_COUNT
} TriggerChannel;

typedef enum {
    TRIG_EDGE_RISING = 0,
    TRIG_EDGE_FALLING,
} TriggerEdge;

typedef enum {
    ACQ_MODE_RUN = 0,
    ACQ_MODE_SINGLE,
    ACQ_MODE_CONT,
} AcquisitionMode;

typedef enum {
    ANALOG_PLOT_MODE_MEDIAN = 0,
    ANALOG_PLOT_MODE_RAW,
} AnalogPlotMode;

typedef struct {
    AcquisitionMode mode;
    AnalogPlotMode analog_plot_mode;
    TriggerChannel trigger_ch;
    TriggerEdge trigger_edge;
    int trigger_threshold;
    int trigger_pos_idx;
    int trigger_pos_x;
} AcquisitionControlState;

typedef struct {
    bool hold;
    bool waiting_for_trigger;
    bool has_display_data;
} CaptureStatusState;

typedef struct {
    uint16_t buf[RING_CAPACITY];
    int n;
    int trigger_view_idx;
    bool valid;
    bool display_valid;
    int display_view_len;
    int display_trig_pos_x;
    bool capture_on_hold;
} FrozenCaptureState;

typedef struct {
    bool last_trigger_ring_idx_valid;
    size_t last_trigger_ring_idx;
    bool single_require_new_trigger;
    size_t single_min_trigger_ring_idx;

    size_t scan_ring_idx;
    int scan_view_len;
    int scan_trigger_view_idx;
    uint16_t scan_prev_sample;
    bool scan_have_prev;
    bool scan_valid;
} TriggerRuntimeState;

typedef struct {
    bool scroll_ready;
    int scroll_view_len;
    size_t ring_cursor;
    float columns_per_sample;
    float pending_columns;
    PixelColumn pending_column;
} LiveScrollState;

typedef struct {
    RenderTexture2D signal_textures[2];
    bool signal_textures_ready;
    bool signal_texture_valid;
    int signal_texture_front;
    int shift_columns;
    bool force_full;
} SignalTextureCacheState;

typedef struct {
    SampleRing ring;
    UsbReader usb;

    AcquisitionControlState control;
    CaptureStatusState capture;
    FrozenCaptureState frozen;
    TriggerRuntimeState trigger_runtime;
    float view_samples;

    PixelColumn pixel_columns[N_VIEW];
    int pixel_columns_start;

    LiveScrollState live_scroll;
    SignalTextureCacheState signal_texture_cache;
} AppState;

#endif
