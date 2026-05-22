#include <string.h>

#include "app_logic.h"

#include "app_config.h"
#include "display_buffer.h"
#include "display_column.h"

static int sample_value_for_trigger(uint16_t sample, TriggerChannel ch)
{
    if (ch == TRIG_CH_A) {
        return (int)((sample >> 6) & 0x3FFu);
    }
    return (int)((sample >> ((int)ch - 1)) & 0x1u);
}

bool trigger_active(const AppState *app)
{
    return app->control.mode != ACQ_MODE_RUN;
}

int trigger_pos_x_from_step_index(int idx)
{
    if (idx < 0) idx = 0;
    if (idx > TRIG_POS_STEPS) idx = TRIG_POS_STEPS;
    return (int)(((long)idx * (long)(N_VIEW - 1)) / (long)TRIG_POS_STEPS);
}

bool is_analog_channel(TriggerChannel ch)
{
    return ch == TRIG_CH_A;
}

static void reset_run_scroll_state(AppState *app)
{
    app->live_scroll.scroll_ready = false;
    app->live_scroll.scroll_view_len = 0;
    app->live_scroll.ring_cursor = 0;
    app->live_scroll.columns_per_sample = 1.0f;
    app->live_scroll.pending_columns = 0.0f;
    clear_pixel_column(&app->live_scroll.pending_column);
    app->signal_texture_cache.shift_columns = 0;
    app->signal_texture_cache.force_full = true;
}

static void reset_trigger_scan_state(AppState *app)
{
    app->trigger_runtime.scan_ring_idx = 0;
    app->trigger_runtime.scan_view_len = 0;
    app->trigger_runtime.scan_trigger_view_idx = 0;
    app->trigger_runtime.scan_prev_sample = 0;
    app->trigger_runtime.scan_have_prev = false;
    app->trigger_runtime.scan_valid = false;
}

static int trigger_scan_backlog(int view_len)
{
    int backlog = view_len * 8;
    if (backlog < 4096) backlog = 4096;
    if (backlog > (int)RING_CAPACITY) backlog = (int)RING_CAPACITY;
    return backlog;
}

/* Last scan cursor position that still leaves enough samples after the trigger. */
static size_t trigger_scan_limit(SampleRingSnapshot snapshot, bool unique_window_start,
                                 size_t post_needed)
{
    if (unique_window_start) {
        return snapshot.next_abs_idx;
    }

    if (post_needed >= snapshot.next_abs_idx) {
        return 0u;
    }

    return snapshot.next_abs_idx - post_needed;
}

static size_t clamp_trigger_scan_cursor(SampleRingSnapshot snapshot, size_t scan_ring_idx)
{
    size_t min_scan_ring_idx = snapshot.oldest_abs_idx + 1u;

    if (scan_ring_idx < min_scan_ring_idx) {
        return min_scan_ring_idx;
    }
    if (scan_ring_idx > snapshot.next_abs_idx) {
        return snapshot.next_abs_idx;
    }

    return scan_ring_idx;
}

static bool try_load_trigger_scan_prev_sample(AppState *app)
{
    if (app->trigger_runtime.scan_ring_idx == 0u) {
        return false;
    }

    if (!ring_try_read_window(&app->ring,
                              app->trigger_runtime.scan_ring_idx - 1u,
                              &app->trigger_runtime.scan_prev_sample,
                              1u)) {
        return false;
    }

    app->trigger_runtime.scan_have_prev = true;
    return true;
}

/* Clamp the scan position to the readable ring range and ensure the previous sample is available. */
static bool ensure_trigger_scan_ready(AppState *app, SampleRingSnapshot snapshot)
{
    size_t clamped_scan_ring_idx = clamp_trigger_scan_cursor(snapshot,
                                                             app->trigger_runtime.scan_ring_idx);
    if (clamped_scan_ring_idx != app->trigger_runtime.scan_ring_idx) {
        app->trigger_runtime.scan_ring_idx = clamped_scan_ring_idx;
        app->trigger_runtime.scan_have_prev = false;
        if (app->trigger_runtime.scan_ring_idx > snapshot.oldest_abs_idx) {
            try_load_trigger_scan_prev_sample(app);
        }
    }

    if (app->trigger_runtime.scan_have_prev) {
        return true;
    }

    if (app->trigger_runtime.scan_ring_idx == 0u ||
        app->trigger_runtime.scan_ring_idx > snapshot.next_abs_idx) {
        return false;
    }

    return try_load_trigger_scan_prev_sample(app);
}

static bool trigger_hit_between(uint16_t prev_sample, uint16_t curr_sample,
                                TriggerChannel ch, TriggerEdge edge, int threshold)
{
    int prev = sample_value_for_trigger(prev_sample, ch);
    int curr = sample_value_for_trigger(curr_sample, ch);

    if (!is_analog_channel(ch)) {
        return (edge == TRIG_EDGE_RISING)
            ? (prev == 0 && curr == 1)
            : (prev == 1 && curr == 0);
    }

    return (edge == TRIG_EDGE_RISING)
        ? (prev < threshold && curr >= threshold)
        : (prev > threshold && curr <= threshold);
}

/* Decide whether a trigger candidate is valid for the current scan context. */
static bool trigger_candidate_is_eligible(const AppState *app,
                                          uint16_t prev_sample, uint16_t curr_sample,
                                          size_t trigger_ring_idx,
                                          size_t min_trigger_ring_idx)
{
    if (app->trigger_runtime.single_require_new_trigger &&
        trigger_ring_idx < app->trigger_runtime.single_min_trigger_ring_idx) {
        return false;
    }

    return trigger_ring_idx >= min_trigger_ring_idx &&
           trigger_hit_between(prev_sample, curr_sample, app->control.trigger_ch,
                               app->control.trigger_edge, app->control.trigger_threshold);
}

static void init_trigger_scan_state(AppState *app, int view_len, int trigger_view_idx)
{
    SampleRingSnapshot snapshot = ring_snapshot(&app->ring);
    size_t scan_ring_idx = snapshot.next_abs_idx;

    size_t backlog = (size_t)trigger_scan_backlog(view_len);

    if (scan_ring_idx > backlog) {
        scan_ring_idx -= backlog;
    } else {
        scan_ring_idx = 0;
    }

    scan_ring_idx = clamp_trigger_scan_cursor(snapshot, scan_ring_idx);

    app->trigger_runtime.scan_ring_idx = scan_ring_idx;
    app->trigger_runtime.scan_view_len = view_len;
    app->trigger_runtime.scan_trigger_view_idx = trigger_view_idx;
    app->trigger_runtime.scan_have_prev = false;
    app->trigger_runtime.scan_valid = true;

    if (scan_ring_idx > snapshot.oldest_abs_idx) {
        try_load_trigger_scan_prev_sample(app);
    }
}

static void store_frozen_capture(AppState *app, const uint16_t *samples, int count,
                                 int trigger_view_idx)
{
    if (count <= 0) {
        app->frozen.n = 0;
        app->frozen.trigger_view_idx = -1;
        app->frozen.valid = false;
        app->frozen.display_valid = false;
        return;
    }

    memcpy(app->frozen.buf, samples, (size_t)count * sizeof(app->frozen.buf[0]));
    app->frozen.n = count;
    app->frozen.trigger_view_idx = trigger_view_idx;
    app->frozen.valid = true;
    app->frozen.display_valid = false;
}

/* Apply the common state updates after capturing a complete triggered window. */
static void apply_trigger_capture(AppState *app, const uint16_t *samples,
                                  int view_len, int trigger_view_idx,
                                  size_t trigger_ring_idx)
{
    fill_pixel_columns_from_window(app, samples, view_len);
    app->capture.has_display_data = true;
    app->capture.waiting_for_trigger = false;
    store_frozen_capture(app, samples, view_len, trigger_view_idx);
    app->frozen.display_valid = true;
    app->frozen.display_view_len = view_len;
    app->frozen.display_trig_pos_x = app->control.trigger_pos_x;
    app->trigger_runtime.last_trigger_ring_idx = trigger_ring_idx;
    app->trigger_runtime.last_trigger_ring_idx_valid = true;
    app->trigger_runtime.single_require_new_trigger = false;

    if (app->control.mode == ACQ_MODE_SINGLE) {
        app->capture.hold = true;
        app->frozen.capture_on_hold = false;
    }
}

/* Read and commit a full display window around a resolved trigger position. */
static bool try_capture_trigger_window(AppState *app, uint16_t *fetch_buf,
                                       size_t window_start_ring_idx, int view_len,
                                       int trigger_view_idx, size_t trigger_ring_idx)
{
    if (!ring_try_read_window(&app->ring, window_start_ring_idx, fetch_buf, (size_t)view_len)) {
        return false;
    }

    apply_trigger_capture(app, fetch_buf, view_len, trigger_view_idx, trigger_ring_idx);
    return true;
}

/* Advance trigger scan state after accepting a trigger hit. */
static void commit_trigger_scan_hit(AppState *app, size_t trigger_ring_idx,
                                    uint16_t trigger_sample)
{
    app->trigger_runtime.scan_ring_idx = trigger_ring_idx + 1u;
    app->trigger_runtime.scan_prev_sample = trigger_sample;
    app->trigger_runtime.scan_have_prev = true;
}

/* Advance trigger scan state after consuming a full scan chunk. */
static void commit_trigger_scan_chunk(AppState *app, size_t chunk_start_ring_idx,
                                      size_t chunk_count, uint16_t last_sample)
{
    app->trigger_runtime.scan_ring_idx = chunk_start_ring_idx + chunk_count;
    app->trigger_runtime.scan_prev_sample = last_sample;
    app->trigger_runtime.scan_have_prev = true;
}

/* Search one loaded scan chunk for a trigger and capture its display window. */
static bool try_capture_trigger_from_scan_chunk(AppState *app, uint16_t *fetch_buf,
                                                size_t chunk_start_ring_idx,
                                                size_t chunk_count, int view_len,
                                                int trigger_view_idx,
                                                bool unique_window_start,
                                                size_t min_trigger_ring_idx,
                                                uint16_t *out_last_sample)
{
    uint16_t prev_sample = app->trigger_runtime.scan_prev_sample;

    for (size_t i = 0; i < chunk_count; i++) {
        uint16_t curr_sample = fetch_buf[i];
        size_t trigger_ring_idx = chunk_start_ring_idx + i;

        size_t candidate_min_trigger_ring_idx = unique_window_start ? 0u : min_trigger_ring_idx;

        if (trigger_candidate_is_eligible(app,
                          prev_sample,
                          curr_sample,
                          trigger_ring_idx,
                          candidate_min_trigger_ring_idx)) {
            size_t window_start_ring_idx = trigger_ring_idx - (size_t)trigger_view_idx;

            if (try_capture_trigger_window(app,
                                           fetch_buf,
                                           window_start_ring_idx,
                                           view_len,
                                           trigger_view_idx,
                                           trigger_ring_idx)) {
                commit_trigger_scan_hit(app, trigger_ring_idx, curr_sample);
                *out_last_sample = curr_sample;
                return true;
            }
            break;
        }

        prev_sample = curr_sample;
    }

    *out_last_sample = prev_sample;
    return false;
}

/* Read the next trigger scan chunk and reset scan state if the ring moved under us. */
static bool try_read_trigger_scan_chunk(AppState *app, uint16_t *fetch_buf,
                                        size_t scan_limit, size_t max_chunk_count,
                                        int view_len, int trigger_view_idx,
                                        size_t *out_chunk_start_ring_idx,
                                        size_t *out_chunk_count)
{
    size_t remaining = scan_limit - app->trigger_runtime.scan_ring_idx;
    size_t chunk_count = remaining;
    if (chunk_count > max_chunk_count) {
        chunk_count = max_chunk_count;
    }

    if (!ring_try_read_window(&app->ring,
                              app->trigger_runtime.scan_ring_idx,
                              fetch_buf, chunk_count)) {
        init_trigger_scan_state(app, view_len, trigger_view_idx);
        return false;
    }

    *out_chunk_start_ring_idx = app->trigger_runtime.scan_ring_idx;
    *out_chunk_count = chunk_count;
    return true;
}

static void init_run_scroll_from_window(AppState *app, const uint16_t *window_src,
                                        int view_len, size_t next_ring_cursor)
{
    fill_pixel_columns_from_window(app, window_src, (uint32_t)view_len);
    app->capture.has_display_data = true;
    app->capture.waiting_for_trigger = false;
    app->live_scroll.scroll_ready = true;
    app->live_scroll.scroll_view_len = view_len;
    app->live_scroll.ring_cursor = next_ring_cursor;
    app->live_scroll.columns_per_sample =
        (view_len > 1) ? ((float)(N_VIEW - 1) / (float)(view_len - 1)) : 1.0f;
    app->live_scroll.pending_columns = 0.0f;
    clear_pixel_column(&app->live_scroll.pending_column);
    app->signal_texture_cache.shift_columns = 0;
    app->signal_texture_cache.force_full = true;
    app->frozen.display_valid = false;
}

/* Push one live-scroll column and record that the cached texture must shift. */
static void push_live_scroll_column(AppState *app, const PixelColumn *column)
{
    push_pixel_column_right(app, column);
    app->signal_texture_cache.shift_columns++;
}

/* Emit as many display columns as the current live-scroll phase allows. */
static void emit_live_scroll_columns(AppState *app)
{
    int emitted_columns = (int)app->live_scroll.pending_columns;

    if (emitted_columns <= 0) {
        return;
    }

    app->live_scroll.pending_columns -= (float)emitted_columns;
    finalize_pixel_column(&app->live_scroll.pending_column);

    if (emitted_columns > 1) {
        PixelColumn empty_column;

        clear_pixel_column(&empty_column);
        for (int i = 0; i < emitted_columns - 1; i++) {
            push_live_scroll_column(app, &empty_column);
        }
    }

    push_live_scroll_column(app, &app->live_scroll.pending_column);
    clear_pixel_column(&app->live_scroll.pending_column);
}

void update_zoom_from_wheel(AppState *app)
{
    const float zoom_factor = 1.2f;

    float wheel = GetMouseWheelMove();
    while (wheel > 0.0f) {
        app->view_samples /= zoom_factor;
        wheel -= 1.0f;
    }
    while (wheel < 0.0f) {
        app->view_samples *= zoom_factor;
        wheel += 1.0f;
    }

    if (app->view_samples < 100.0f) app->view_samples = 100.0f;
    if (app->view_samples > (float)RING_CAPACITY) app->view_samples = (float)RING_CAPACITY;
}

static int current_view_len(const AppState *app)
{
    int view_len = (int)app->view_samples;
    if (view_len < 2) view_len = 2;
    return view_len;
}

static int trigger_view_idx_in_window(const AppState *app, int view_len)
{
    int trigger_view_idx = (int)(((long)app->control.trigger_pos_x * (long)(view_len - 1)) / (long)(N_VIEW - 1));
    if (trigger_view_idx < 0) trigger_view_idx = 0;
    if (trigger_view_idx >= view_len) trigger_view_idx = view_len - 1;
    return trigger_view_idx;
}

static int samples_per_pixel_column_ceil(int sample_count)
{
    if (sample_count <= 0) {
        return 1;
    }
    return (sample_count + N_VIEW - 1) / N_VIEW;
}

/* Build the narrow scan band used to search a fixed display window for a trigger. */
static bool init_fixed_trigger_scan_range(size_t window_start_ring_idx, int view_len,
                                          int desired_trigger_view_idx, int trigger_tolerance,
                                          int *min_trigger_view_idx, int *max_trigger_view_idx,
                                          size_t *scan_start_ring_idx, size_t *scan_count)
{
    int min_view_idx = desired_trigger_view_idx - trigger_tolerance;
    int max_view_idx = desired_trigger_view_idx + trigger_tolerance;

    if (min_view_idx < 0) min_view_idx = 0;
    if (max_view_idx >= view_len) max_view_idx = view_len - 1;
    if (max_view_idx < 1) {
        return false;
    }

    size_t band_start_ring_idx = window_start_ring_idx + (size_t)min_view_idx;
    size_t scan_start_ring_idx_local = band_start_ring_idx;
    if (scan_start_ring_idx_local > window_start_ring_idx) {
        scan_start_ring_idx_local--;
    }

    size_t band_end_ring_idx = window_start_ring_idx + (size_t)max_view_idx;

    *min_trigger_view_idx = min_view_idx;
    *max_trigger_view_idx = max_view_idx;
    *scan_start_ring_idx = scan_start_ring_idx_local;
    *scan_count = band_end_ring_idx - scan_start_ring_idx_local + 1u;
    return true;
}

/* Search a preloaded fixed-window scan band for an acceptable trigger hit. */
static bool find_trigger_in_scan_band(const AppState *app, const uint16_t *scan_buf,
                                      size_t window_start_ring_idx,
                                      int min_trigger_view_idx, int max_trigger_view_idx,
                                      size_t scan_start_ring_idx, size_t scan_count,
                                      int *out_trigger_view_idx)
{
    for (size_t i = 1; i < scan_count; i++) {
        size_t trigger_ring_idx = scan_start_ring_idx + i;
        int trigger_view_idx = (int)(trigger_ring_idx - window_start_ring_idx);

        if (trigger_view_idx < min_trigger_view_idx || trigger_view_idx > max_trigger_view_idx) {
            continue;
        }
        if (!trigger_candidate_is_eligible(app,
                                           scan_buf[i - 1],
                                           scan_buf[i],
                                           trigger_ring_idx,
                                           0u)) {
            continue;
        }

        *out_trigger_view_idx = trigger_view_idx;
        return true;
    }

    return false;
}

static bool find_trigger_in_fixed_window(AppState *app, uint16_t *fetch_buf,
                                         size_t window_start_ring_idx, int view_len,
                                         int desired_trigger_view_idx, int trigger_tolerance,
                                         int *out_trigger_view_idx)
{
    int min_trigger_view_idx;
    int max_trigger_view_idx;
    size_t scan_start_ring_idx;
    size_t scan_count;

    if (!init_fixed_trigger_scan_range(window_start_ring_idx, view_len,
                                       desired_trigger_view_idx, trigger_tolerance,
                                       &min_trigger_view_idx, &max_trigger_view_idx,
                                       &scan_start_ring_idx, &scan_count)) {
        return false;
    }

    if (!ring_try_read_window(&app->ring, scan_start_ring_idx, fetch_buf, scan_count)) {
        return false;
    }

    return find_trigger_in_scan_band(app, fetch_buf, window_start_ring_idx,
                                     min_trigger_view_idx, max_trigger_view_idx,
                                     scan_start_ring_idx, scan_count,
                                     out_trigger_view_idx);
}

/* Find and capture a trigger within a fixed display window. */
static bool try_capture_fixed_trigger_window(AppState *app, uint16_t *fetch_buf,
                                             size_t window_start_ring_idx, int view_len,
                                             int desired_trigger_view_idx,
                                             int trigger_tolerance)
{
    int resolved_trigger_view_idx = -1;

    if (!find_trigger_in_fixed_window(app, fetch_buf, window_start_ring_idx, view_len,
                                      desired_trigger_view_idx, trigger_tolerance,
                                      &resolved_trigger_view_idx)) {
        return false;
    }

    return try_capture_trigger_window(app, fetch_buf, window_start_ring_idx, view_len,
                                      resolved_trigger_view_idx,
                                      window_start_ring_idx + (size_t)resolved_trigger_view_idx);
}

static bool recapture_frozen_trigger_view(AppState *app, uint16_t *fetch_buf, int desired_view_len)
{
    if (!app->trigger_runtime.last_trigger_ring_idx_valid || desired_view_len < 2) {
        return false;
    }

    SampleRingSnapshot snapshot = ring_snapshot(&app->ring);

    if (!ring_snapshot_contains_idx(snapshot, app->trigger_runtime.last_trigger_ring_idx)) {
        return false;
    }

    size_t available = ring_snapshot_count(snapshot);
    size_t capture_len = (size_t)desired_view_len;
    if (capture_len > available) {
        capture_len = available;
    }
    if (capture_len < 2u) {
        return false;
    }

    int trigger_view_idx = trigger_view_idx_in_window(app, (int)capture_len);
    size_t preferred_start_ring_idx = 0u;
    if (app->trigger_runtime.last_trigger_ring_idx >= (size_t)trigger_view_idx) {
        preferred_start_ring_idx = app->trigger_runtime.last_trigger_ring_idx - (size_t)trigger_view_idx;
    }

    size_t window_start_ring_idx = ring_snapshot_clamp_window_start(snapshot,
                                                                    preferred_start_ring_idx,
                                                                    capture_len);

    if (!ring_try_read_window(&app->ring, window_start_ring_idx, fetch_buf, capture_len)) {
        return false;
    }

    store_frozen_capture(app, fetch_buf, (int)capture_len,
                         (int)(app->trigger_runtime.last_trigger_ring_idx - window_start_ring_idx));
    return true;
}

static bool update_hold_display(AppState *app, uint16_t *fetch_buf, int view_len)
{
    if (app->capture.hold && app->frozen.capture_on_hold) {
        if (!(trigger_active(app) && !app->capture.waiting_for_trigger &&
              recapture_frozen_trigger_view(app, fetch_buf, view_len))) {
            SampleRingReadWindow window = ring_read_tail_window(&app->ring, fetch_buf, RING_CAPACITY);
            int count = (int)ring_read_window_count(window);

            store_frozen_capture(app, fetch_buf, count, count - 1);
        }
        app->frozen.capture_on_hold = false;
    }

    if (!app->capture.hold) {
        return false;
    }

    reset_run_scroll_state(app);

    if (!app->frozen.valid) {
        return true;
    }

    if (view_len > app->frozen.n && trigger_active(app)) {
        recapture_frozen_trigger_view(app, fetch_buf, view_len);
    }

    if (view_len > app->frozen.n) view_len = app->frozen.n;

    if (app->frozen.display_valid &&
        app->frozen.display_view_len == view_len &&
        app->frozen.display_trig_pos_x == app->control.trigger_pos_x) {
        app->capture.has_display_data = true;
        app->capture.waiting_for_trigger = false;
        return true;
    }

    int trigger_view_idx = trigger_view_idx_in_window(app, view_len);
    int start_view_idx = app->frozen.trigger_view_idx - trigger_view_idx;
    if (start_view_idx < 0) {
        start_view_idx = 0;
    }
    if (start_view_idx + view_len > app->frozen.n) {
        start_view_idx = app->frozen.n - view_len;
    }

    fill_pixel_columns_from_window(app, app->frozen.buf + start_view_idx, view_len);
    app->frozen.display_valid = true;
    app->frozen.display_view_len = view_len;
    app->frozen.display_trig_pos_x = app->control.trigger_pos_x;
    app->capture.has_display_data = true;
    app->capture.waiting_for_trigger = false;
    return true;
}

static void update_run_display(AppState *app, uint16_t *fetch_buf, int view_len)
{
    reset_trigger_scan_state(app);

    if (!app->live_scroll.scroll_ready || app->live_scroll.scroll_view_len != view_len) {
        SampleRingReadWindow window = ring_read_tail_window(&app->ring, fetch_buf, RING_CAPACITY);
        int n = (int)ring_read_window_count(window);

        if (n <= 0) {
            return;
        }

        if (view_len > n) view_len = n;

        init_run_scroll_from_window(app, fetch_buf + (n - view_len), view_len,
                                    window.next_abs_idx);
        return;
    }

    SampleRingReadWindow window = ring_read_window_from_cursor(&app->ring,
                                                               &app->live_scroll.ring_cursor,
                                                               fetch_buf, RING_CAPACITY);
    size_t got_new = ring_read_window_count(window);

    if (window.start_clamped) {
        SampleRingReadWindow tail_window = ring_read_tail_window(&app->ring, fetch_buf, RING_CAPACITY);
        int n = (int)ring_read_window_count(tail_window);

        if (n <= 0) {
            reset_run_scroll_state(app);
            return;
        }

        if (view_len > n) view_len = n;

        init_run_scroll_from_window(app, fetch_buf + (n - view_len), view_len,
                                    tail_window.next_abs_idx);
        return;
    }

    if (got_new == 0u) {
        app->capture.has_display_data = true;
        app->capture.waiting_for_trigger = false;
        return;
    }

    for (size_t i = 0; i < got_new; i++) {
        append_sample_to_pixel_column(&app->live_scroll.pending_column, fetch_buf[i]);
        app->live_scroll.pending_columns += app->live_scroll.columns_per_sample;
        emit_live_scroll_columns(app);
    }

    app->capture.has_display_data = true;
    app->capture.waiting_for_trigger = false;
}

static void update_trigger_display(AppState *app, uint16_t *fetch_buf, int view_len)
{
    enum { TRIGGER_SCAN_CHUNK = 8192 };

    SampleRingSnapshot snapshot = ring_snapshot(&app->ring);
    size_t read_ring_idx = snapshot.oldest_abs_idx;

    size_t available = ring_snapshot_count(snapshot);
    if (available < 2u) {
        app->capture.waiting_for_trigger = true;
        return;
    }
    if ((size_t)view_len > available) {
        view_len = (int)available;
    }

    bool unique_window_start = ((size_t)view_len == available);

    int trigger_view_idx = trigger_view_idx_in_window(app, view_len);
    int trigger_tolerance = samples_per_pixel_column_ceil(view_len);

    if (unique_window_start) {
        if (!try_capture_fixed_trigger_window(app,
                                              fetch_buf,
                                              read_ring_idx,
                                              view_len,
                                              trigger_view_idx,
                                              trigger_tolerance)) {
            app->capture.waiting_for_trigger = true;
            return;
        }
        return;
    }

    if (!app->trigger_runtime.scan_valid ||
        app->trigger_runtime.scan_view_len != view_len ||
        app->trigger_runtime.scan_trigger_view_idx != trigger_view_idx) {
        init_trigger_scan_state(app, view_len, trigger_view_idx);
    }

    size_t post_needed = (size_t)(view_len - trigger_view_idx - 1);
    bool found_trigger = false;

    while (!found_trigger) {
        snapshot = ring_snapshot(&app->ring);
        read_ring_idx = snapshot.oldest_abs_idx;

        if (!ensure_trigger_scan_ready(app, snapshot)) {
            break;
        }

        size_t scan_limit = trigger_scan_limit(snapshot, unique_window_start, post_needed);

        if (app->trigger_runtime.scan_ring_idx >= scan_limit) {
            break;
        }

        size_t chunk_start_ring_idx;
        size_t chunk_count;
        if (!try_read_trigger_scan_chunk(app,
                                         fetch_buf,
                                         scan_limit,
                                         (size_t)TRIGGER_SCAN_CHUNK,
                                         view_len, trigger_view_idx,
                                         &chunk_start_ring_idx,
                                         &chunk_count)) {
            continue;
        }
        size_t min_trigger_ring_idx = read_ring_idx + (size_t)trigger_view_idx;
        uint16_t last_sample = app->trigger_runtime.scan_prev_sample;

        found_trigger = try_capture_trigger_from_scan_chunk(app,
                                                            fetch_buf,
                                                            chunk_start_ring_idx,
                                                            chunk_count,
                                                            view_len,
                                                            trigger_view_idx,
                                                            unique_window_start,
                                                            min_trigger_ring_idx,
                                                            &last_sample);

        if (found_trigger) {
            break;
        }

        commit_trigger_scan_chunk(app,
                                  chunk_start_ring_idx,
                                  chunk_count,
                                  last_sample);
    }

    if (!found_trigger) {
        app->capture.waiting_for_trigger = true;
        return;
    }
}

void update_display_buffer(AppState *app)
{
    static uint16_t fetch_buf[RING_CAPACITY];

    app->control.trigger_pos_x = trigger_pos_x_from_step_index(app->control.trigger_pos_idx);

    int view_len = current_view_len(app);

    if (update_hold_display(app, fetch_buf, view_len)) {
        return;
    }

    app->frozen.display_valid = false;

    if (!trigger_active(app)) {
        update_run_display(app, fetch_buf, view_len);
        return;
    }

    reset_run_scroll_state(app);
    update_trigger_display(app, fetch_buf, view_len);
}