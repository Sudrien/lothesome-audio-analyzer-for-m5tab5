/*
 * spectrum.h -- the analyzer's arithmetic: which FFT bins make each bar,
 * magnitude to dBFS to bar height, and the ballistics.
 *
 * Header-only and free of ESP-IDF, so test/spectrumtest.c checks all of
 * it on the host. analyzer.c is the hardware and the drawing; nothing in
 * here knows there is a screen.
 *
 * Every number is the original sketch's (original/Tab5_SpectrumAnalyzer.ino)
 * unless a comment says otherwise. The one that changed is the sample
 * full scale: the sketch read 16-bit samples through M5Unified, and this
 * reads feckless-drivers' capture, which hands back 24-bit scale in an
 * int32 (audio_out.h). See SPECTRUM_SAMPLE_FULL_SCALE.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Audio / FFT ---- */
#define SPECTRUM_SAMPLE_RATE   (48000)  /* Nyquist 24 kHz: the full audible range */
#define SPECTRUM_FFT_SIZE      (1024)   /* power of 2; ~46.9 Hz a bin */
#define SPECTRUM_BARS          (64)

/* ---- Frequency axis ---- */
/*
 * 0004 set this to 80 Hz, where the array microphones start telling
 * frequencies apart. 0013: 20 Hz, the bottom of hearing, now that the
 * bass path below resolves it; a USB measurement microphone hears down
 * there, and the part of the axis a source cannot hear is greyed
 * (analyzer.c's source floors) rather than cut off.
 */
#define SPECTRUM_LOG_MIN_HZ    (20.0f)
#define SPECTRUM_LOG_MAX_HZ    ((float)SPECTRUM_SAMPLE_RATE / 2.0f)

/* ---- Level, in dBFS ----
 * 0 dB is digital full scale, everything real below it. The scale comes
 * from the sample format alone and needs no calibration. It is NOT dB
 * SPL: the offset to acoustic dB depends on the microphones' sensitivity
 * and gain, and needs an acoustic reference this project does not have
 * (the original README's "Calibration" section). */
#define SPECTRUM_MIN_DBFS      (-90.0f)
#define SPECTRUM_MAX_DBFS      (0.0f)

/*
 * A full-scale sample.
 *
 * src: audio_out.h (feckless-drivers) -- capture returns 24-bit values
 *      sign-extended in int32, "16 bits shifted to 24-bit scale" for the
 *      built-in pair (5212). So full scale is 2^23, not the sketch's
 *      2^15. The low 8 bits are zero: the resolution is still 16-bit.
 */
#define SPECTRUM_SAMPLE_FULL_SCALE (8388608.0f)

/*
 * A Hamming window sums to 0.54 of a rectangular one, so a tone's bin
 * comes out 0.54 of its unwindowed height; dividing it back out makes a
 * full-scale sine read 0 dBFS.
 *
 * src: the Hamming window w(n) = 0.54 - 0.46 cos(2 pi n / (N-1)); its
 *      mean over n is 0.54 for large N (the cosine term averages out).
 */
#define SPECTRUM_HAMMING_GAIN  (0.54f)

/*
 * A real tone of amplitude A lands in an unnormalised N-point FFT at
 * magnitude A * N / 2. esp-dsp's dsps_fft2r_fc32() is unnormalised, as
 * arduinoFFT's was, so the sketch's formula carries over unchanged.
 */
#define SPECTRUM_FULL_SCALE_MAG \
    (SPECTRUM_SAMPLE_FULL_SCALE * (SPECTRUM_FFT_SIZE / 2.0f) * SPECTRUM_HAMMING_GAIN)

/* ---- 0013: the bass path ----
 * A 1024-point FFT at 48 kHz has 46.9 Hz bins: 20 to 80 Hz is about one
 * bin. So the capture is also low-passed and kept every 16th sample, and
 * a second FFT of the same size runs on that: 3 kHz, 2.9 Hz bins, a
 * 341 ms window. Everything below the split comes from it. The bass
 * moves slower than the treble, as it must -- telling 30 Hz from 33 Hz
 * takes about a third of a second.
 *
 * The filter is a Blackman-windowed sinc cut at the decimated Nyquist;
 * its transition is about 520 Hz wide at 48 kHz, so what folds back
 * below the split is from 1760 Hz up, at least 74 dB down.
 * test/spectrumtest.c measures both.
 */
#define SPECTRUM_BASS_DECIM    (16)
#define SPECTRUM_BASS_TAPS     (511)            /* odd: symmetric about the middle tap */
#define SPECTRUM_BASS_HIST     (512)            /* a power of two >= TAPS */
#define SPECTRUM_BASS_SPLIT    (0.8f)           /* of the decimated Nyquist: 1200 Hz at 48 kHz */

static inline float spectrum_bass_split_hz(float rate)
{
    return SPECTRUM_BASS_SPLIT * rate / (2.0f * SPECTRUM_BASS_DECIM);
}

/* ---- Ballistics ----
 * Time constants in seconds rather than per-frame steps, so the display
 * moves the same at any framerate. */
#define SPECTRUM_ATTACK_TAU        (0.020f)  /* ~20 ms rise */
#define SPECTRUM_RELEASE_TAU       (0.150f)  /* ~150 ms fall */
#define SPECTRUM_PEAK_FALL_PER_SEC (0.55f)   /* peak marker sink rate */

static inline float spectrum_clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* The Hamming window, for one sample. */
static inline float spectrum_hamming(int n, int size)
{
    return 0.54f - 0.46f * cosf(2.0f * 3.14159265358979f * (float)n / (float)(size - 1));
}

/* 0013: the bass filter's taps into h[SPECTRUM_BASS_TAPS], unity at DC. */
static inline void spectrum_bass_filter(float *h)
{
    const int   m  = SPECTRUM_BASS_TAPS - 1;
    const float fc = 0.5f / (float)SPECTRUM_BASS_DECIM;     /* cycles per sample */
    const float pi = 3.14159265358979f;
    float sum = 0.0f;
    for (int n = 0; n <= m; n++) {
        const float t = (float)n - (float)m / 2.0f;
        const float sinc = t == 0.0f ? 2.0f * fc : sinf(2.0f * pi * fc * t) / (pi * t);
        const float w = 0.42f - 0.5f * cosf(2.0f * pi * n / m) + 0.08f * cosf(4.0f * pi * n / m);
        h[n] = sinc * w;
        sum += h[n];
    }
    for (int n = 0; n <= m; n++) h[n] /= sum;
}

/* 0013: the decimator's state. One writer, so no lock. */
typedef struct {
    float    hist[SPECTRUM_BASS_HIST];
    uint32_t pos;
    int      phase;
} spectrum_decim_t;

/* Push n samples through; every SPECTRUM_BASS_DECIM-th makes one output
 * in out[]. Returns how many it made: at most n / DECIM + 1. Only the
 * outputs are filtered, which is what makes this cheap. */
static inline int spectrum_decim(spectrum_decim_t *d, const float *h,
                                 const float *in, int n, float *out)
{
    int made = 0;
    for (int i = 0; i < n; i++) {
        d->hist[d->pos++ & (SPECTRUM_BASS_HIST - 1)] = in[i];
        if (++d->phase < SPECTRUM_BASS_DECIM) continue;
        d->phase = 0;
        float y = 0.0f;
        for (int k = 0; k < SPECTRUM_BASS_TAPS; k++)
            y += h[k] * d->hist[(d->pos - 1u - (uint32_t)k) & (SPECTRUM_BASS_HIST - 1)];
        out[made++] = y;
    }
    return made;
}

/*
 * Which bins make one bar. Bars are log-spaced from LOG_MIN to LOG_MAX.
 * Below about 50 Hz a bar is narrower than one bin, and two neighbours
 * would read the same bin and move as one block, so a narrow bar reads
 * between two bins, interpolated, instead of averaging a range.
 */
typedef struct {
    bool  bass;             /* 0013: bins of the bass FFT, not the main one */
    bool  narrow;
    float centre_bin;       /* narrow: where to interpolate */
    int   lo_bin, hi_bin;   /* wide: the inclusive range averaged */
} spectrum_band_t;

/*
 * 0008: at a given sample rate. A USB microphone runs at its own rate --
 * 48 kHz if it offers it, else 44.1, else its highest (uac.h) -- and the
 * bars span 80 Hz to that rate's Nyquist, so each bin is rate/N wide and
 * the top of the axis moves with it. spectrum_band() is this at 48 kHz.
 */
/*
 * 0012: for any number of bars. The display has one bar per pixel column
 * of the graph, so the count is the graph's width, known only at run
 * time; spectrum_band_at() is this at SPECTRUM_BARS.
 */
static inline spectrum_band_t spectrum_band_n(int bar, int bars, float rate)
{
    const int   usable_bins = SPECTRUM_FFT_SIZE / 2;
    const float ratio       = (rate / 2.0f) / SPECTRUM_LOG_MIN_HZ;
    const float lo_f = SPECTRUM_LOG_MIN_HZ * powf(ratio, (float)bar / (float)bars);
    const float hi_f = SPECTRUM_LOG_MIN_HZ * powf(ratio, (float)(bar + 1) / (float)bars);
    /* 0013: a bar wholly under the split reads the bass FFT. */
    const bool  bass        = hi_f <= spectrum_bass_split_hz(rate);
    const float hz_per_bin  = rate / (bass ? (float)SPECTRUM_BASS_DECIM : 1.0f) /
                              (float)SPECTRUM_FFT_SIZE;
    const float lo_b = lo_f / hz_per_bin;
    const float hi_b = hi_f / hz_per_bin;

    spectrum_band_t b = { 0 };
    b.bass = bass;
    if (hi_b - lo_b < 1.0f) {
        b.narrow = true;
        b.centre_bin = spectrum_clampf((lo_b + hi_b) * 0.5f, 1.0f, (float)(usable_bins - 2));
    } else {
        b.lo_bin = (int)lo_b < 1 ? 1 : (int)lo_b;
        b.hi_bin = (int)hi_b > usable_bins - 1 ? usable_bins - 1 : (int)hi_b;
        if (b.hi_bin < b.lo_bin) b.hi_bin = b.lo_bin;
    }
    return b;
}

static inline spectrum_band_t spectrum_band_at(int bar, float rate)
{
    return spectrum_band_n(bar, SPECTRUM_BARS, rate);
}

static inline spectrum_band_t spectrum_band(int bar)
{
    return spectrum_band_at(bar, (float)SPECTRUM_SAMPLE_RATE);
}

/* One bar's magnitude from the FFT's magnitudes (`mag`, at least
 * FFT_SIZE/2 of them).
 *
 * 0012: a wide bar is the largest of its bins, not their mean. The mean
 * read a tone lower the more bins shared its bar, so the same tone sat at
 * different heights across the axis; the largest reads it at its level
 * anywhere, as a single bin would. */
static inline float spectrum_band_mag(const spectrum_band_t *b, const float *mag)
{
    if (b->narrow) {
        const int   lo = (int)b->centre_bin;
        const float t  = b->centre_bin - (float)lo;
        return mag[lo] * (1.0f - t) + mag[lo + 1] * t;
    }
    float m = 0.0f;
    for (int i = b->lo_bin; i <= b->hi_bin; i++) if (mag[i] > m) m = mag[i];
    return m;
}

/* Magnitude to dBFS. The 1e-9 keeps log10 off zero: silence reads -180. */
static inline float spectrum_dbfs(float mag)
{
    return 20.0f * log10f(mag / SPECTRUM_FULL_SCALE_MAG + 1e-9f);
}

/* dBFS to a fraction of the graph's height, 0..1. */
static inline float spectrum_frac(float dbfs)
{
    return spectrum_clampf((dbfs - SPECTRUM_MIN_DBFS) /
                           (SPECTRUM_MAX_DBFS - SPECTRUM_MIN_DBFS), 0.0f, 1.0f);
}

/* Where a log-axis frequency falls across `width` pixels. */
static inline int spectrum_freq_x_at(float hz, int width, float max_hz)
{
    hz = spectrum_clampf(hz, SPECTRUM_LOG_MIN_HZ, max_hz);
    const float t = logf(hz / SPECTRUM_LOG_MIN_HZ) /
                    logf(max_hz / SPECTRUM_LOG_MIN_HZ);
    return (int)(t * (float)width);
}

static inline int spectrum_freq_x(float hz, int width)
{
    return spectrum_freq_x_at(hz, width, SPECTRUM_LOG_MAX_HZ);
}

/*
 * One bar's ballistics for one frame of `dt` seconds: rise quickly, fall
 * gently, and a peak marker that holds the top and sinks at a fixed
 * rate. `level` and `peak` are 0..1 and updated in place.
 */
static inline void spectrum_ballistics(float target, float dt, float *level, float *peak)
{
    const float a_attack  = 1.0f - expf(-dt / SPECTRUM_ATTACK_TAU);
    const float a_release = 1.0f - expf(-dt / SPECTRUM_RELEASE_TAU);
    float s = *level;
    s += (target - s) * (target > s ? a_attack : a_release);
    *level = s;
    if (s > *peak) *peak = s;
    else           *peak = fmaxf(0.0f, *peak - SPECTRUM_PEAK_FALL_PER_SEC * dt);
}

/*
 * The bar colour for a row `frac` of the way up the graph: green at the
 * bottom, yellow halfway, red at the top. By ROW, never by a bar's
 * level -- that is what lets a growing bar paint only its new rows.
 * Returned as 8-bit red and green; blue is always 0.
 */
static inline void spectrum_row_rg(float frac, int *r, int *g)
{
    frac = spectrum_clampf(frac, 0.0f, 1.0f);
    if (frac < 0.5f) { *r = (int)(255.0f * (frac / 0.5f)); *g = 255; }
    else             { *r = 255; *g = (int)(255.0f * (1.0f - (frac - 0.5f) / 0.5f)); }
}

#ifdef __cplusplus
}
#endif
