#include "usb_reader.h"

#include <libusb-1.0/libusb.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static libusb_context *g_ctx = NULL;

static void sleep_ms(long ms)
{
    struct timespec req;
    req.tv_sec = ms / 1000;
    req.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&req, NULL);
}

/* ── Async transfer implementation ─────────────────────────────────────
 *
 * Root cause of MC buffer overflow with synchronous transfers:
 *
 *   |─── Kernel receives 512B ───|── USERSPACE GAP ──|─── next transfer ───|
 *                                  ↑ no IN tokens sent to AVR here
 *   Linux can preempt the USB thread for 10–50 ms during the gap.
 *   AVR ring buffer overflows after a few ms.
 *
 * Fix: NUM_XFER concurrent async transfers always pending in the kernel.
 * The kernel schedules IN tokens continuously, independent of userspace.
 * Even a 50 ms preemption cannot cause an overflow.
 */
#define NUM_XFER 16    /* concurrent in-flight bulk transfers: bei 70kS: 1 -> 3.66ms => 16 -> 58ms USB-Buffer in Kernel*/

typedef struct {
    UsbReader              *ur;
    struct libusb_transfer *xfr;
    uint8_t                 buf[USB_RAW_PKTSIZE];
    _Atomic int            *n_active;
    _Atomic bool           *device_lost;
    _Atomic uint32_t       *samples_cb;
} XferCtx;

static void LIBUSB_CALL bulk_cb(struct libusb_transfer *xfr)
{
    XferCtx   *ctx = (XferCtx *)xfr->user_data;
    UsbReader  *ur = ctx->ur;

    switch (xfr->status) {
    case LIBUSB_TRANSFER_COMPLETED:
    case LIBUSB_TRANSFER_TIMED_OUT:
        if (xfr->actual_length > 0 && (xfr->actual_length % 2) == 0) {
            size_t ns = (size_t)xfr->actual_length / 2u;
            uint16_t sbuf[USB_RAW_PKTSIZE / 2u];
            for (size_t i = 0; i < ns; i++) {
                sbuf[i] = (uint16_t)ctx->buf[i * 2u]  // Explicitly Little Endian
                        | ((uint16_t)ctx->buf[i * 2u + 1u] << 8);
            }
            ring_push_batch(ur->ring, sbuf, ns);
            atomic_fetch_add((uint32_t *)&ur->sample_count,  (uint32_t)ns);
            atomic_fetch_add((uint32_t *)ctx->samples_cb,    (uint32_t)ns);
            } else if ((xfr->actual_length % 2) != 0) {
                atomic_fetch_add((uint32_t *)&ur->data_inconsistency_count, 1u);
                fprintf(stderr, "[USB] Data inconsistency: odd payload length %d\n",
                    xfr->actual_length);
        }
        break;

    case LIBUSB_TRANSFER_CANCELLED:
        atomic_fetch_sub((uint32_t *)ctx->n_active, 1);
        return;

    case LIBUSB_TRANSFER_NO_DEVICE:
    case LIBUSB_TRANSFER_ERROR:
    case LIBUSB_TRANSFER_STALL:
    case LIBUSB_TRANSFER_OVERFLOW:
    default:
        fprintf(stderr, "[USB] async transfer error: %d\n", xfr->status);
        atomic_store(ctx->device_lost, true);
        atomic_fetch_sub(ctx->n_active, 1);
        return;
    }

    if (atomic_load(&ur->stop)) {
        atomic_fetch_sub(ctx->n_active, 1);
        return;
    }

    int rc = libusb_submit_transfer(xfr);
    if (rc != LIBUSB_SUCCESS) {
        fprintf(stderr, "[USB] resubmit failed: %s\n", libusb_error_name(rc));
        atomic_fetch_add(&ur->error_count, 1u);
        atomic_store(ctx->device_lost, true);
        atomic_fetch_sub(ctx->n_active, 1);
    }
}

static void *usb_thread_func(void *arg)
{
    UsbReader *ur = (UsbReader *)arg;

    while (!atomic_load(&ur->stop)) {

        /* ── Open device ─────────────────────────────────────────── */
        libusb_device_handle *dev =
            libusb_open_device_with_vid_pid(g_ctx, USB_VID, USB_PID);
        if (dev == NULL) {
            atomic_store(&ur->connected, false);
            atomic_store(&ur->sample_rate, 0u);
            sleep_ms(1000);
            continue;
        }

        libusb_set_auto_detach_kernel_driver(dev, 1);

        int rc = libusb_claim_interface(dev, USB_RAW_IFACE);
        if (rc != LIBUSB_SUCCESS) {
            if (rc == LIBUSB_ERROR_ACCESS)
                fprintf(stderr,
                    "[USB] Permission denied.\n"
                    "      Install the udev rule and replug the device:\n"
                    "        sudo cp 99-oszilloskop.rules /etc/udev/rules.d/\n"
                    "        sudo udevadm control --reload-rules\n"
                    "        sudo udevadm trigger\n");
            else
                fprintf(stderr, "[USB] claim_interface(%d): %s\n",
                        USB_RAW_IFACE, libusb_error_name(rc));
            libusb_close(dev);
            atomic_store(&ur->connected, false);
            atomic_store(&ur->sample_rate, 0u);
            atomic_fetch_add(&ur->error_count, 1u);
            sleep_ms(2000);
            continue;
        }

        atomic_store(&ur->connected, true);
        fprintf(stderr, "[USB] Opened – VID=%04X PID=%04X  iface=%d  ep=%02X\n",
                USB_VID, USB_PID, USB_RAW_IFACE, USB_RAW_EP);

        /* ── Allocate NUM_XFER async transfer contexts ───────────── */
        _Atomic int      n_active    = NUM_XFER;
        _Atomic bool     device_lost = false;
        _Atomic uint32_t samples_cb  = 0;

        XferCtx ctxs[NUM_XFER] = {0};
        bool alloc_ok = true;
        for (int i = 0; i < NUM_XFER; i++) {
            ctxs[i].ur          = ur;
            ctxs[i].n_active    = &n_active;
            ctxs[i].device_lost = &device_lost;
            ctxs[i].samples_cb  = &samples_cb;
            ctxs[i].xfr         = libusb_alloc_transfer(0);
            if (!ctxs[i].xfr) { alloc_ok = false; break; }
            libusb_fill_bulk_transfer(ctxs[i].xfr, dev, USB_RAW_EP,
                                      ctxs[i].buf, USB_RAW_PKTSIZE,
                                      bulk_cb, &ctxs[i],
                                      2000 /* ms timeout */);
        }

        if (!alloc_ok) {
            fprintf(stderr, "[USB] libusb_alloc_transfer failed\n");
            atomic_fetch_add(&ur->error_count, 1u);
            for (int i = 0; i < NUM_XFER; i++)
                if (ctxs[i].xfr) libusb_free_transfer(ctxs[i].xfr);
            libusb_release_interface(dev, USB_RAW_IFACE);
            libusb_close(dev);
            atomic_store(&ur->connected, false);
            atomic_store(&ur->sample_rate, 0u);
            sleep_ms(1000);
            continue;
        }

        /* ── Submit all transfers simultaneously ─────────────────── */
        for (int i = 0; i < NUM_XFER; i++) {
            int submit_rc = libusb_submit_transfer(ctxs[i].xfr);
            if (submit_rc != LIBUSB_SUCCESS) {
                fprintf(stderr, "[USB] submit[%d] failed: %s\n",
                        i, libusb_error_name(submit_rc));
                atomic_fetch_add(&ur->error_count, 1u);
                atomic_store(&device_lost, true);
                atomic_fetch_sub(&n_active, 1);
            }
        }

        /* ── Event loop ──────────────────────────────────────────── */
        struct timespec t0;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        struct timeval tv = { 0, 50000 };  /* 50 ms poll interval */

        while (!atomic_load(&ur->stop) && !atomic_load(&device_lost)) {
            int ev_rc = libusb_handle_events_timeout(g_ctx, &tv);
            if (ev_rc != LIBUSB_SUCCESS && ev_rc != LIBUSB_ERROR_INTERRUPTED) {
                fprintf(stderr, "[USB] handle_events: %s\n",
                        libusb_error_name(ev_rc));
                atomic_fetch_add(&ur->error_count, 1u);
                atomic_store(&device_lost, true);
                continue;
            }

            /* Update sample rate once per second */
            struct timespec t1;
            clock_gettime(CLOCK_MONOTONIC, &t1);
            double dt = (double)(t1.tv_sec  - t0.tv_sec)
                      + (double)(t1.tv_nsec - t0.tv_nsec) * 1e-9;
            if (dt >= 1.0) {
                uint32_t s = atomic_exchange(&samples_cb, 0u);
                atomic_store(&ur->sample_rate, (uint32_t)((double)s / dt));
                t0 = t1;
            }
        }

        /* ── Cancel all pending transfers and wait for completion ── */
        for (int i = 0; i < NUM_XFER; i++)
            libusb_cancel_transfer(ctxs[i].xfr);

        struct timeval tv2 = { 0, 50000 };
        while (atomic_load(&n_active) > 0) {
            int ev_rc = libusb_handle_events_timeout(g_ctx, &tv2);
            if (ev_rc != LIBUSB_SUCCESS && ev_rc != LIBUSB_ERROR_INTERRUPTED) {
                fprintf(stderr, "[USB] handle_events(cancel): %s\n",
                        libusb_error_name(ev_rc));
                atomic_fetch_add(&ur->error_count, 1u);
            }
        }

        for (int i = 0; i < NUM_XFER; i++)
            libusb_free_transfer(ctxs[i].xfr);

        libusb_release_interface(dev, USB_RAW_IFACE);
        libusb_close(dev);
        atomic_store(&ur->connected, false);
        atomic_store(&ur->sample_rate, 0u);

        if (!atomic_load(&ur->stop)) {
            fprintf(stderr, "[USB] Device lost – reconnecting...\n");
            atomic_fetch_add(&ur->error_count, 1u);
            sleep_ms(1000);
        }
    }

    return NULL;
}

/* ── Public API ─────────────────────────────────────────────────────── */

void usb_reader_init(UsbReader *ur, SampleRing *ring)
{
    ur->ring = ring;
    atomic_store(&ur->stop,           false);
    atomic_store(&ur->connected,      false);
    atomic_store(&ur->sample_count,   0u);
    atomic_store(&ur->sample_rate, 0u);
    atomic_store(&ur->error_count,    0u);
    atomic_store(&ur->data_inconsistency_count, 0u);
}

bool usb_reader_start(UsbReader *ur)
{
    int rc = libusb_init(&g_ctx);
    if (rc != LIBUSB_SUCCESS) {
        fprintf(stderr, "[USB] libusb_init: %s\n", libusb_error_name(rc));
        return false;
    }
    rc = pthread_create(&ur->thread, NULL, usb_thread_func, ur);
    if (rc != 0) {
        fprintf(stderr, "[USB] pthread_create failed: %d\n", rc);
        libusb_exit(g_ctx);
        g_ctx = NULL;
        return false;
    }

    /* Elevate the USB thread to soft-realtime priority so the OS scheduler
     * does not delay it behind GUI / compositor work.  With 16 concurrent
     * transfers the kernel buffer is ~58 ms; even a 20 ms preemption by the
     * GPU driver can drain all pending URBs and starve the AVR.
     *
     * SCHED_FIFO prio=1 (lowest RT level) is sufficient: it preempts all
     * SCHED_OTHER threads (GUI, X11, compositor) but not kernel IRQ threads.
     *
     * Requires CAP_SYS_NICE.  Grant without running as root:
     *   sudo setcap cap_sys_nice+eip ./build/oszilloskop_gui
     * The program works without the capability – just with higher overflow
     * risk under GUI load. */
    struct sched_param sp = { .sched_priority = 1 };
    if (pthread_setschedparam(ur->thread, SCHED_FIFO, &sp) != 0) {
        fprintf(stderr,
            "[USB] Note: could not set SCHED_FIFO (no CAP_SYS_NICE).\n"
            "      USB thread runs at normal priority – overflows more\n"
            "      likely under heavy GUI load.  Fix:\n"
            "        sudo setcap cap_sys_nice+eip ./build/oszilloskop_gui\n");
    } else {
        fprintf(stderr, "[USB] SCHED_FIFO prio=1 set for USB thread.\n");
    }

    return true;
}

void usb_reader_stop(UsbReader *ur)
{
    atomic_store(&ur->stop, true);
    pthread_join(ur->thread, NULL);
    libusb_exit(g_ctx);
    g_ctx = NULL;
}
