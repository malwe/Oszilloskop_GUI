#include "sample_ring.h"
#include <string.h>

static bool ring_read_window_matches(SampleRingReadWindow window,
                                     size_t start_idx, size_t count)
{
    return !window.start_clamped &&
           window.start_abs_idx == start_idx &&
           ring_read_window_count(window) == count;
}

static SampleRingReadWindow read_window_locked(SampleRing *r, size_t start_idx,
                                               uint16_t *out, size_t max_count)
{
    SampleRingReadWindow window;
    size_t start = start_idx;

    window.start_clamped = false;

    if (start < r->read_idx) {
        start = r->read_idx;
        window.start_clamped = true;
    }
    if (start > r->write_idx) {
        start = r->write_idx;
        window.start_clamped = true;
    }

    size_t available = r->write_idx - start;
    size_t copy = (available < max_count) ? available : max_count;

    for (size_t i = 0; i < copy; i++) {
        out[i] = r->buf[(start + i) & (RING_CAPACITY - 1u)];
    }

    window.start_abs_idx = start;
    window.next_abs_idx = start + copy;
    return window;
}

void ring_init(SampleRing *r)
{
    memset(r->buf, 0, sizeof(r->buf));
    r->write_idx = 0;
    r->read_idx  = 0;
    pthread_mutex_init(&r->mtx, NULL);
}

void ring_destroy(SampleRing *r)
{
    pthread_mutex_destroy(&r->mtx);
}

void ring_push(SampleRing *r, uint16_t sample)
{
    pthread_mutex_lock(&r->mtx);

    r->buf[r->write_idx & (RING_CAPACITY - 1u)] = sample;
    r->write_idx++;

    /* Evict the oldest entry when the buffer is full */
    if (r->write_idx - r->read_idx > RING_CAPACITY) {
        r->read_idx = r->write_idx - RING_CAPACITY;
    }

    pthread_mutex_unlock(&r->mtx);
}

void ring_push_batch(SampleRing *r, const uint16_t *samples, size_t count)
{
    if (count == 0) return;

    pthread_mutex_lock(&r->mtx);

    for (size_t i = 0; i < count; i++) {
        r->buf[r->write_idx & (RING_CAPACITY - 1u)] = samples[i];
        r->write_idx++;
    }

    /* Evict in bulk: if we wrote more than the capacity, keep only the last
     * RING_CAPACITY samples */
    if (r->write_idx - r->read_idx > RING_CAPACITY) {
        r->read_idx = r->write_idx - RING_CAPACITY;
    }

    pthread_mutex_unlock(&r->mtx);
}

SampleRingReadWindow ring_read_tail_window(SampleRing *r, uint16_t *out, size_t max_count)
{
    SampleRingReadWindow window;

    pthread_mutex_lock(&r->mtx);

    size_t avail = r->write_idx - r->read_idx;
    size_t copy  = (avail < max_count) ? avail : max_count;
    size_t start = r->write_idx - copy;   /* index of oldest copied sample */

    window = read_window_locked(r, start, out, max_count);

    pthread_mutex_unlock(&r->mtx);
    return window;
}

SampleRingReadWindow ring_read_window(SampleRing *r, size_t start_idx,
                                      uint16_t *out, size_t max_count)
{
    SampleRingReadWindow window;

    pthread_mutex_lock(&r->mtx);
    window = read_window_locked(r, start_idx, out, max_count);
    pthread_mutex_unlock(&r->mtx);

    return window;
}

bool ring_try_read_window(SampleRing *r, size_t start_idx,
                          uint16_t *out, size_t count)
{
    return ring_read_window_matches(ring_read_window(r, start_idx, out, count),
                                    start_idx, count);
}

SampleRingReadWindow ring_read_window_from_cursor(SampleRing *r, size_t *cursor,
                                                  uint16_t *out, size_t max_count)
{
    SampleRingReadWindow window;

    pthread_mutex_lock(&r->mtx);

    window = read_window_locked(r, *cursor, out, max_count);

    *cursor = window.next_abs_idx;
    pthread_mutex_unlock(&r->mtx);

    return window;
}

SampleRingSnapshot ring_snapshot(SampleRing *r)
{
    SampleRingSnapshot snapshot;

    pthread_mutex_lock(&r->mtx);
    snapshot.oldest_abs_idx = r->read_idx;
    snapshot.next_abs_idx = r->write_idx;
    pthread_mutex_unlock(&r->mtx);

    return snapshot;
}
