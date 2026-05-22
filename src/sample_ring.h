#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>

/* Must be a power of 2.  At 70 kHz this holds ~0.9 s of data. */
#define RING_CAPACITY 1048576u  // 2^20

typedef struct {
    uint16_t        buf[RING_CAPACITY];
    size_t          write_idx;   /* written by USB thread  */
    size_t          read_idx;    /* written by USB thread (eviction) */
    pthread_mutex_t mtx;
} SampleRing;

typedef struct {
    size_t oldest_abs_idx;
    size_t next_abs_idx;
} SampleRingSnapshot;

typedef struct {
    size_t start_abs_idx;
    size_t next_abs_idx;
    bool start_clamped;
} SampleRingReadWindow;

static inline size_t ring_snapshot_count(SampleRingSnapshot snapshot)
{
    return snapshot.next_abs_idx - snapshot.oldest_abs_idx;
}

static inline bool ring_snapshot_contains_idx(SampleRingSnapshot snapshot, size_t idx)
{
    return idx >= snapshot.oldest_abs_idx && idx < snapshot.next_abs_idx;
}

/* Latest valid start for a full window of `count` samples. */
static inline size_t ring_snapshot_latest_start(SampleRingSnapshot snapshot, size_t count)
{
    if (count >= ring_snapshot_count(snapshot)) {
        return snapshot.oldest_abs_idx;
    }

    return snapshot.next_abs_idx - count;
}

/* Clamp a preferred window start into the readable range for `count` samples. */
static inline size_t ring_snapshot_clamp_window_start(SampleRingSnapshot snapshot,
                                                      size_t preferred_start, size_t count)
{
    size_t latest_start = ring_snapshot_latest_start(snapshot, count);

    if (preferred_start < snapshot.oldest_abs_idx) {
        return snapshot.oldest_abs_idx;
    }
    if (preferred_start > latest_start) {
        return latest_start;
    }

    return preferred_start;
}

static inline size_t ring_read_window_count(SampleRingReadWindow window)
{
    return window.next_abs_idx - window.start_abs_idx;
}

static inline bool ring_snapshot_contains(SampleRingSnapshot snapshot, size_t start_idx, size_t count)
{
    if (start_idx < snapshot.oldest_abs_idx || start_idx > snapshot.next_abs_idx) {
        return false;
    }

    return count <= snapshot.next_abs_idx - start_idx;
}

void   ring_init(SampleRing *r);
void   ring_destroy(SampleRing *r);

/* Push one sample.  Evicts the oldest entry when the buffer is full. Use
 * ring_push_batch() for better performance when pushing multiple samples. */
void   ring_push(SampleRing *r, uint16_t sample);

/* Push `count` samples at once under a single mutex lock.
 * Much more efficient than calling ring_push() in a loop. */
void   ring_push_batch(SampleRing *r, const uint16_t *samples, size_t count);

/* Copy up to `max_count` newest samples (oldest→newest) into `out`.
 * Returns the absolute range copied into `out`. */
SampleRingReadWindow ring_read_tail_window(SampleRing *r, uint16_t *out, size_t max_count);

/* Copy samples starting at absolute `start_idx` up to `max_count` samples.
 * If `start_idx` falls outside the readable range, it is clamped before
 * copying and the returned window reports that via `start_clamped`. */
SampleRingReadWindow ring_read_window(SampleRing *r, size_t start_idx,
                                      uint16_t *out, size_t max_count);

/* Copy exactly `count` samples starting at absolute `start_idx` into `out`.
 * Returns false if any part of the requested range is no longer readable. */
bool ring_try_read_window(SampleRing *r, size_t start_idx,
                          uint16_t *out, size_t count);

/* Copy samples starting at `*cursor` up to `max_count` samples.
 * Advances `*cursor` to the end of the copied absolute range. If the cursor
 * fell outside the readable range, it is clamped before copying and the
 * returned window reports that via `start_clamped`. */
SampleRingReadWindow ring_read_window_from_cursor(SampleRing *r, size_t *cursor,
                                                  uint16_t *out, size_t max_count);

/* Return a consistent snapshot of the readable absolute index range.
 * Samples are available in [oldest_abs_idx, next_abs_idx). */
SampleRingSnapshot ring_snapshot(SampleRing *r);
