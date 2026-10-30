/*
 * monoring.h -- the newest mono samples, written by the capture task and
 * read by the drawing loop, so a slow frame never leaves a gap in the
 * window the FFT sees.
 *
 * WHY THIS EXISTS (0003)
 *
 * The port read 1024 frames, drew, and read again. The only buffering
 * between those reads is the I2S DMA: 8 x 120 frames, 20 ms (feckless-
 * drivers' CAPTURE_DMA_DESC/FRAMES). Drawing took 25-44 ms on the board,
 * so by the next read the DMA had wrapped and overwritten what arrived
 * during the draw. Each window was the newest ~20 ms that survived plus
 * whatever came after, spliced: a discontinuity in the middle of every
 * FFT, which smears energy across bins. The log's 4 ms "capture" was the
 * tell -- reading 1024 frames that had mostly already arrived.
 *
 * Now a capture task reads continuously into this ring, and the drawing
 * loop copies out the newest SPECTRUM_FFT_SIZE samples whenever it is
 * ready. The window is always contiguous and always the most recent
 * audio, whatever the draw costs, and consecutive windows overlap when
 * drawing is fast instead of each costing 21 ms of new samples.
 *
 * ONE WRITER, ONE READER, NO LOCK
 *
 * The writer stores samples and then publishes the running count with a
 * release store; the reader loads the count with acquire and copies the
 * MONORING_SIZE - window samples behind it. The writer could overwrite
 * the start of that copy only by getting MONORING_SIZE - window samples
 * ahead during the copy itself -- 64 ms at 48 kHz for a 4096 ring and a
 * 1024 window, against a copy of a few microseconds.
 *
 * Header-only and free of ESP-IDF; test/spectrumtest.c checks the copy
 * across the wrap.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MONORING_SIZE   (4096)      /* a power of two */

typedef struct {
    float    buf[MONORING_SIZE];
    uint32_t count;                 /* samples ever written; wraps at 2^32 */
} monoring_t;

/* Writer: append n samples, then publish. */
static inline void monoring_write(monoring_t *r, const float *s, size_t n)
{
    uint32_t c = __atomic_load_n(&r->count, __ATOMIC_RELAXED);
    for (size_t i = 0; i < n; i++) r->buf[(c + (uint32_t)i) & (MONORING_SIZE - 1)] = s[i];
    __atomic_store_n(&r->count, c + (uint32_t)n, __ATOMIC_RELEASE);
}

/* Reader: how many samples have ever been written. */
static inline uint32_t monoring_count(const monoring_t *r)
{
    return __atomic_load_n(&r->count, __ATOMIC_ACQUIRE);
}

/*
 * Reader: the `n` samples ending at `end` (a value monoring_count()
 * returned), oldest first, into `out`. n must be at most MONORING_SIZE
 * and at most `end`; the caller checks both.
 */
static inline void monoring_newest(const monoring_t *r, uint32_t end, float *out, size_t n)
{
    const uint32_t start = end - (uint32_t)n;
    for (size_t i = 0; i < n; i++) out[i] = r->buf[(start + (uint32_t)i) & (MONORING_SIZE - 1)];
}

#ifdef __cplusplus
}
#endif
