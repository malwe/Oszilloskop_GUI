#pragma once

#include "sample_ring.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <pthread.h>

/* ── Device identifiers ────────────────────────────────────────────── */
#define USB_VID            0x03EBu   /* Atmel / LUFA                    */
#define USB_PID            0x2044u

/* ── Endpoint / interface ──────────────────────────────────────────── */
#define USB_RAW_EP         0x81u     /* Bulk IN, endpoint 1             */
#define USB_RAW_IFACE      2         /* Vendor-class interface index    */
#define USB_RAW_PKTSIZE    512       /* >= Bytes per bulk transfer      */

typedef struct {
    SampleRing       *ring;

    /* Shared state – written by USB thread, read by render thread.
     * Using _Atomic avoids data-race UB without needing a mutex. */
    _Atomic bool      stop;
    _Atomic bool      connected;
    _Atomic uint32_t  sample_count;
    _Atomic uint32_t  sample_rate;
    _Atomic uint32_t  error_count;
    _Atomic uint32_t  data_inconsistency_count;

    pthread_t         thread;
} UsbReader;

void usb_reader_init(UsbReader *ur, SampleRing *ring);
bool usb_reader_start(UsbReader *ur);
void usb_reader_stop(UsbReader *ur);   /* blocks until the thread exits */
