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

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Audio / FFT ---- */
#define SPECTRUM_SAMPLE_RATE   (48000)  /* Nyquist 24 kHz: the full audible range */
#define SPECTRUM_FFT_SIZE      (1024)   /* power of 2; ~46.9 Hz a bin */
#define SPECTRUM_BARS          (64)

/* ---- Frequency axis ---- */
/*
 * 0004: 80 Hz, not the sketch's 50. The array microphones only start
 * telling frequencies apart at about 80 Hz, so bars below it showed
 * nothing the hardware can resolve; the axis starts where it does.
 *
 * src: observed on the board, not a datasheet figure.
 */
#define SPECTRUM_LOG_MIN_HZ    (80.0f)
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

/*
 * Which bins make one bar. Bars are log-spaced from LOG_MIN to LOG_MAX.
 * Below about 50 Hz a bar is narrower than one bin, and two neighbours
 * would read the same bin and move as one block, so a narrow bar reads
 * between two bins, interpolated, instead of averaging a range.
 */
typedef struct {
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
static inline spectrum_band_t spectrum_band_at(int bar, float rate)
{
    const float hz_per_bin  = rate / (float)SPECTRUM_FFT_SIZE;
    const int   usable_bins = SPECTRUM_FFT_SIZE / 2;
    const float ratio       = (rate / 2.0f) / SPECTRUM_LOG_MIN_HZ;
    const float lo_f = SPECTRUM_LOG_MIN_HZ * powf(ratio, (float)bar / SPECTRUM_BARS);
    const float hi_f = SPECTRUM_LOG_MIN_HZ * powf(ratio, (float)(bar + 1) / SPECTRUM_BARS);
    const float lo_b = lo_f / hz_per_bin;
    const float hi_b = hi_f / hz_per_bin;

    spectrum_band_t b = { 0 };
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

static inline spectrum_band_t spectrum_band(int bar)
{
    return spectrum_band_at(bar, (float)SPECTRUM_SAMPLE_RATE);
}

/* One bar's magnitude from the FFT's magnitudes (`mag`, at least
 * FFT_SIZE/2 of them). */
static inline float spectrum_band_mag(const spectrum_band_t *b, const float *mag)
{
    if (b->narrow) {
        const int   lo = (int)b->centre_bin;
        const float t  = b->centre_bin - (float)lo;
        return mag[lo] * (1.0f - t) + mag[lo + 1] * t;
    }
    float sum = 0.0f;
    for (int i = b->lo_bin; i <= b->hi_bin; i++) sum += mag[i];
    return sum / (float)(b->hi_bin - b->lo_bin + 1);
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
