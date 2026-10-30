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
#include <stddef.h>
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

/* ---- 0013, 0032: the stages ----
 * A 1024-point FFT at 48 kHz has 46.9 Hz bins: 20 to 80 Hz is about one
 * bin. So the capture is also halved in rate, again and again, and the
 * same FFT runs on each: stage k at rate / 2^k. 0013 had two stages, 48
 * and 3 kHz, a factor of 16 apart, and the seam between them showed: the
 * noise floor stepped 12 dB (a bin 16 times narrower holds a sixteenth of
 * the noise) and the time smear 16 times. 0032 has five, a factor of 2
 * apart:
 *
 *   stage   rate     bin       window   used for (at 48 kHz)
 *   0       48 kHz   46.9 Hz    21 ms   9.6 kHz up
 *   1       24       23.4       43      4.8 - 9.6 kHz
 *   2       12       11.7       85      2.4 - 4.8
 *   3        6        5.9      171      1.2 - 2.4
 *   4        3        2.9      341      below 1.2 kHz, as 0013's bass
 *
 * so each seam is 3 dB of noise floor and a doubling of smear. A band
 * reads the slowest stage whose usable range -- SPECTRUM_STAGE_SPLIT of
 * its Nyquist -- holds it whole. The slower stages move slower, as they
 * must: telling 30 Hz from 33 Hz takes about a third of a second.
 */
#define SPECTRUM_STAGES        (5)
#define SPECTRUM_STAGE_SPLIT   (0.8f)           /* of a stage's Nyquist */

/* The top of stage k's usable range: 1200 Hz for stage 4 at 48 kHz. */
static inline float spectrum_stage_split_hz(float rate, int k)
{
    return SPECTRUM_STAGE_SPLIT * rate / (2.0f * (float)(1 << k));
}

/* The slowest stage that holds a band reaching up to hi_hz. */
static inline int spectrum_stage_for(float hi_hz, float rate)
{
    for (int k = SPECTRUM_STAGES - 1; k > 0; k--)
        if (hi_hz <= spectrum_stage_split_hz(rate, k)) return k;
    return 0;
}

/* Hz a bin, at stage k. */
static inline float spectrum_stage_hz_per_bin(float rate, int k)
{
    return rate / (float)(1 << k) / (float)SPECTRUM_FFT_SIZE;
}

/*
 * Each halving is a half-band low-pass then every other sample: a
 * Blackman-windowed sinc cut at the new Nyquist (a quarter of the old
 * rate). It must be flat to the split -- 0.8 of the new Nyquist, 0.4 of
 * the old -- and deep from where anything would fold back onto it, 1.2
 * of the new Nyquist, 0.6 of the old. test/spectrumtest.c measures both,
 * one stage and four in cascade.
 */
#define SPECTRUM_HB_TAPS       (95)             /* odd: symmetric about the middle tap */
#define SPECTRUM_HB_HIST       (128)            /* a power of two >= TAPS */

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

/* 0032: the half-band filter's taps into h[SPECTRUM_HB_TAPS], unity at DC. */
static inline void spectrum_hb_filter(float *h)
{
    const int   m  = SPECTRUM_HB_TAPS - 1;
    const float fc = 0.25f;                     /* cycles per input sample: the new Nyquist */
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

/* One halving's state. One writer, so no lock. */
typedef struct {
    float    hist[SPECTRUM_HB_HIST];
    uint32_t pos;
    int      phase;
} spectrum_hb_t;

/* Push n samples through; every other one makes an output in out[].
 * Returns how many: at most n / 2 + 1. Only the outputs are filtered. */
static inline int spectrum_hb(spectrum_hb_t *d, const float *h, const float *in, int n, float *out)
{
    int made = 0;
    for (int i = 0; i < n; i++) {
        d->hist[d->pos++ & (SPECTRUM_HB_HIST - 1)] = in[i];
        if (++d->phase < 2) continue;
        d->phase = 0;
        float y = 0.0f;
        for (int k = 0; k < SPECTRUM_HB_TAPS; k++)
            y += h[k] * d->hist[(d->pos - 1u - (uint32_t)k) & (SPECTRUM_HB_HIST - 1)];
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
    int   stage;            /* 0032: whose FFT's bins (0013's bass is stage 4) */
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
    /* 0013, 0032: the slowest stage that holds the bar whole. */
    const int   stage       = spectrum_stage_for(hi_f, rate);
    const float hz_per_bin  = spectrum_stage_hz_per_bin(rate, stage);
    const float lo_b = lo_f / hz_per_bin;
    const float hi_b = hi_f / hz_per_bin;

    spectrum_band_t b = { 0 };
    b.stage = stage;
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

/* ---- 0014: the sound level meter ----
 *
 * LAeq, LAFmax and LCpeak, on the sample stream rather than the FFT:
 * peak needs every sample, and Fast and Leq are defined on the weighted
 * signal itself (IEC 61672-1). Levels are dB re a full-scale sine
 * (AES17), so a full-scale sine reads 0 on all three, as it does on the
 * bars. They are NOT dB SPL; see SPECTRUM_MIN_DBFS's comment.
 *
 * A and C weighting from their analog poles (IEC 61672-1 Annex E):
 *   C(s) = k s^2 / ((s+w1)^2 (s+w4)^2)
 *   A(s) = k s^4 / ((s+w1)^2 (s+w2)(s+w3)(s+w4)^2)
 * as biquads by the bilinear transform, each pole prewarped so it lands
 * at its own frequency, and the cascade set to 0 dB at 1 kHz. The
 * transform still bends the response near Nyquist; test/spectrumtest.c
 * holds it to class 1 at 44.1 and 48 kHz. A USB source below 32 kHz has
 * w4 past its Nyquist and gets an approximate C and A at the top.
 */
#define SPECTRUM_W_F1          (20.598997)
#define SPECTRUM_W_F2          (107.65265)
#define SPECTRUM_W_F3          (737.86223)
#define SPECTRUM_W_F4          (12194.217)
#define SPECTRUM_FAST_TAU      (0.125)          /* s, IEC 61672 F */
#define SPECTRUM_CLIP_FRAC     (0.999f)         /* of full scale: a sample there clipped */

typedef struct {
    float b0, b1, b2, a1, a2;
    float z1, z2;                               /* transposed direct form II */
} spectrum_biquad_t;

static inline float spectrum_biquad(spectrum_biquad_t *q, float x)
{
    const float y = q->b0 * x + q->z1;
    q->z1 = q->b1 * x - q->a1 * y + q->z2;
    q->z2 = q->b2 * x - q->a2 * y;
    return y;
}

/* An analog pole's rad/s, prewarped for the bilinear transform. */
static inline double spectrum_prewarp(double hz, double rate)
{
    const double pi = 3.14159265358979323846;
    if (hz >= 0.45 * rate) return 2.0 * pi * hz;     /* past Nyquist: as it is */
    return 2.0 * rate * tan(pi * hz / rate);
}

/* (B2 s^2 + B1 s + B0) / (A2 s^2 + A1 s + A0) to a digital biquad. */
static inline spectrum_biquad_t spectrum_bilinear(double B2, double B1, double B0,
                                                  double A2, double A1, double A0, double rate)
{
    const double k = 2.0 * rate, k2 = k * k;
    const double a0 = A2 * k2 + A1 * k + A0;
    spectrum_biquad_t q = { 0 };
    q.b0 = (float)((B2 * k2 + B1 * k + B0) / a0);
    q.b1 = (float)((2.0 * B0 - 2.0 * B2 * k2) / a0);
    q.b2 = (float)((B2 * k2 - B1 * k + B0) / a0);
    q.a1 = (float)((2.0 * A0 - 2.0 * A2 * k2) / a0);
    q.a2 = (float)((A2 * k2 - A1 * k + A0) / a0);
    return q;
}

/* |H| of n biquads at hz. */
static inline double spectrum_biquads_mag(const spectrum_biquad_t *q, int n, double hz, double rate)
{
    const double w = 2.0 * 3.14159265358979323846 * hz / rate;
    double g = 1.0;
    for (int i = 0; i < n; i++) {
        const double cr = cos(w), ci = -sin(w), c2r = cos(2 * w), c2i = -sin(2 * w);
        const double nr = q[i].b0 + q[i].b1 * cr + q[i].b2 * c2r, ni = q[i].b1 * ci + q[i].b2 * c2i;
        const double dr = 1.0 + q[i].a1 * cr + q[i].a2 * c2r,     di = q[i].a1 * ci + q[i].a2 * c2i;
        g *= sqrt((nr * nr + ni * ni) / (dr * dr + di * di));
    }
    return g;
}

/* C weighting into c[2], A into a[3], for `rate`. */
static inline void spectrum_weighting(spectrum_biquad_t c[2], spectrum_biquad_t a[3], double rate)
{
    const double w1 = spectrum_prewarp(SPECTRUM_W_F1, rate), w2 = spectrum_prewarp(SPECTRUM_W_F2, rate);
    const double w3 = spectrum_prewarp(SPECTRUM_W_F3, rate), w4 = spectrum_prewarp(SPECTRUM_W_F4, rate);
    const spectrum_biquad_t hp1 = spectrum_bilinear(1, 0, 0, 1, 2 * w1, w1 * w1, rate);   /* s^2/(s+w1)^2 */
    const spectrum_biquad_t lp4 = spectrum_bilinear(0, 0, 1, 1, 2 * w4, w4 * w4, rate);   /* 1/(s+w4)^2 */
    const spectrum_biquad_t hp23 = spectrum_bilinear(1, 0, 0, 1, w2 + w3, w2 * w3, rate); /* s^2/((s+w2)(s+w3)) */
    c[0] = hp1; c[1] = lp4;
    a[0] = hp1; a[1] = lp4; a[2] = hp23;
    const float gc = (float)(1.0 / spectrum_biquads_mag(c, 2, 1000.0, rate));
    const float ga = (float)(1.0 / spectrum_biquads_mag(a, 3, 1000.0, rate));
    c[0].b0 *= gc; c[0].b1 *= gc; c[0].b2 *= gc;
    a[0].b0 *= ga; a[0].b1 *= ga; a[0].b2 *= ga;
}

/* The analog curves, in dB, for checking the digital ones. */
static inline double spectrum_a_db(double f)
{
    const double f2 = f * f, p1 = SPECTRUM_W_F1 * SPECTRUM_W_F1, p4 = SPECTRUM_W_F4 * SPECTRUM_W_F4;
    const double r = p4 * f2 * f2 / ((f2 + p1) * sqrt((f2 + SPECTRUM_W_F2 * SPECTRUM_W_F2) *
                                                      (f2 + SPECTRUM_W_F3 * SPECTRUM_W_F3)) * (f2 + p4));
    return 20.0 * log10(r) + 2.0;
}

static inline double spectrum_c_db(double f)
{
    const double f2 = f * f, p1 = SPECTRUM_W_F1 * SPECTRUM_W_F1, p4 = SPECTRUM_W_F4 * SPECTRUM_W_F4;
    return 20.0 * log10(p4 * f2 / ((f2 + p1) * (f2 + p4))) + 0.06;
}

typedef struct {
    spectrum_biquad_t c[2], a[3];
    float    fast_k;        /* per-sample step of the Fast average */
    float    fast_e;        /* A-weighted mean square, Fast */
    float    max_e;         /* the largest fast_e */
    float    peak_c;        /* the largest |C-weighted sample| */
    double   sum_a2;        /* sum of A-weighted squares, for Leq */
    uint64_t n;
    bool     clipped;
    double   rate;
} spectrum_meter_t;

/* Start over: the readings, not the filters, which carry on. */
static inline void spectrum_meter_reset(spectrum_meter_t *m)
{
    m->max_e = m->peak_c = 0.0f;
    m->fast_e = 0.0f;
    m->sum_a2 = 0.0;
    m->n = 0;
    m->clipped = false;
}

/* For a source at `rate`: filters and readings both from nothing. */
static inline void spectrum_meter_init(spectrum_meter_t *m, double rate)
{
    *m = (spectrum_meter_t){ 0 };
    m->rate = rate;
    spectrum_weighting(m->c, m->a, rate);
    m->fast_k = (float)(1.0 - exp(-1.0 / (SPECTRUM_FAST_TAU * rate)));
}

static inline void spectrum_meter_push(spectrum_meter_t *m, const float *x, int n)
{
    for (int i = 0; i < n; i++) {
        float ya = x[i], yc = x[i];
        for (int k = 0; k < 3; k++) ya = spectrum_biquad(&m->a[k], ya);
        for (int k = 0; k < 2; k++) yc = spectrum_biquad(&m->c[k], yc);
        const float e = ya * ya;
        m->sum_a2 += e;
        m->fast_e += (e - m->fast_e) * m->fast_k;
        if (m->fast_e > m->max_e) m->max_e = m->fast_e;
        if (fabsf(yc) > m->peak_c) m->peak_c = fabsf(yc);
        if (fabsf(x[i]) >= SPECTRUM_CLIP_FRAC * SPECTRUM_SAMPLE_FULL_SCALE) m->clipped = true;
    }
    m->n += (uint64_t)n;
}

/* Mean square to dB re a full-scale sine, whose mean square is FS^2/2. */
static inline float spectrum_ms_db(double ms)
{
    const double ref = (double)SPECTRUM_SAMPLE_FULL_SCALE * SPECTRUM_SAMPLE_FULL_SCALE / 2.0;
    return (float)(10.0 * log10(ms / ref + 1e-18));
}

static inline float spectrum_laeq(const spectrum_meter_t *m)
{
    return m->n ? spectrum_ms_db(m->sum_a2 / (double)m->n) : -180.0f;
}

static inline float spectrum_laf(const spectrum_meter_t *m)    { return spectrum_ms_db(m->fast_e); }
static inline float spectrum_lafmax(const spectrum_meter_t *m) { return spectrum_ms_db(m->max_e); }

/* A sine's peak is FS at full scale, so peak is 20 log10 of |x| / FS. */
static inline float spectrum_lcpeak(const spectrum_meter_t *m)
{
    return (float)(20.0 * log10((double)m->peak_c / SPECTRUM_SAMPLE_FULL_SCALE + 1e-9));
}

/* ---- 0015: the waterfall ----
 *
 * Colour for a level, 0..1 of the graph's dB range: black through purple
 * and orange to pale yellow, after matplotlib's "inferno". Lightness
 * rises all the way, so the level reads in greyscale and without telling
 * red from green, which the bars' green-to-red does not.
 *
 * src: inferno at ninths of its range, matplotlib _cm_listed.py, rounded.
 */
static inline void spectrum_heat_rgb(float frac, int *r, int *g, int *b)
{
    static const unsigned char k[9][3] = {
        {   0,   0,   4 }, {  31,  12,  72 }, {  85,  15, 109 },
        { 136,  34, 106 }, { 186,  54,  85 }, { 227,  89,  51 },
        { 249, 140,  10 }, { 249, 201,  50 }, { 252, 255, 164 },
    };
    frac = spectrum_clampf(frac, 0.0f, 1.0f) * 8.0f;
    int i = (int)frac;
    if (i > 7) i = 7;
    const float t = frac - (float)i;
    *r = (int)(k[i][0] + (k[i + 1][0] - k[i][0]) * t + 0.5f);
    *g = (int)(k[i][1] + (k[i + 1][1] - k[i][1]) * t + 0.5f);
    *b = (int)(k[i][2] + (k[i + 1][2] - k[i][2]) * t + 0.5f);
}

/*
 * How many waterfall rows a frame of `dt` seconds owes at `rows_per_sec`,
 * carrying the fraction in *acc. The rows are time, so they advance by
 * the clock and not by the framerate: a slow frame owes several rows, a
 * fast one sometimes none, and the time labels stay true either way.
 */
static inline int spectrum_wf_rows(float *acc, float dt, float rows_per_sec)
{
    *acc += dt * rows_per_sec;
    const int n = (int)*acc;
    *acc -= (float)n;
    return n;
}

/* ---- 0016: one-third octaves ----
 *
 * The 31 bands of IEC 61260-1 from 20 Hz to 20 kHz, base ten: band k
 * (0..30) is centred on 1000 * 10^((k - 17) / 10) Hz, its edges a
 * twentieth of a decade either side. Nominal labels round those
 * (19.95 Hz is "20"); the arithmetic uses the exact ones.
 *
 * A band's level is the POWER in it, from the FFT bins it covers --
 * each bin weighted by how much of its width lies inside the band, so a
 * band narrower than a few bins still gets its share and no more. That
 * is what makes pink noise read flat, as a real-time analyzer must; the
 * bars' largest-bin rule (0012) would make it rise 3 dB an octave.
 * A band reads the slowest stage that holds it whole, like the bars.
 *
 * Power in dB re a full-scale sine, as the meter: by Parseval, a real
 * sine of amplitude A puts N * A^2 * sum(w^2) / 4 into the positive
 * bins, so mean square = 2 * sum|X|^2 / (N * sum(w^2)).
 */
#define SPECTRUM_OCT_BANDS     (31)
#define SPECTRUM_OCT_FIRST_K   (-17)            /* 1000 * 10^(-1.7) = 19.95 Hz */
#define SPECTRUM_SLOW_TAU      (1.0)            /* s, IEC 61672 S */

static inline float spectrum_oct_centre(int band)
{
    return 1000.0f * powf(10.0f, (float)(band + SPECTRUM_OCT_FIRST_K) / 10.0f);
}

typedef struct {
    int   stage;            /* 0032: whose FFT */
    float lo_b, hi_b;       /* the band's edges, in bins of that FFT */
    bool  partial;          /* the top edge is past Nyquist: the band is short */
} spectrum_oct_t;

static inline spectrum_oct_t spectrum_oct_band(int band, float rate)
{
    const float fc = spectrum_oct_centre(band);
    const float lo = fc * powf(10.0f, -0.05f), hi = fc * powf(10.0f, 0.05f);
    spectrum_oct_t o = { 0 };
    o.stage = spectrum_stage_for(hi, rate);
    const float df = spectrum_stage_hz_per_bin(rate, o.stage);
    o.lo_b = lo / df;
    o.hi_b = hi / df;
    o.partial = hi > rate / 2.0f;
    if (o.hi_b > (float)(SPECTRUM_FFT_SIZE / 2) - 0.5f) o.hi_b = (float)(SPECTRUM_FFT_SIZE / 2) - 0.5f;
    return o;
}

/* The Hamming window's sum of squares over N, for the power scale. */
static inline float spectrum_hamming_ss(void)
{
    static float ss;
    if (ss == 0.0f) {
        double a = 0.0;
        for (int n = 0; n < SPECTRUM_FFT_SIZE; n++) {
            const double w = spectrum_hamming(n, SPECTRUM_FFT_SIZE);
            a += w * w;
        }
        ss = (float)a;
    }
    return ss;
}

/* The band's mean square, at 24-bit scale, from that FFT's magnitudes. */
static inline float spectrum_oct_ms(const spectrum_oct_t *o, const float *mag)
{
    if (o->hi_b <= o->lo_b) return 0.0f;
    double p = 0.0;
    int k0 = (int)floorf(o->lo_b + 0.5f), k1 = (int)floorf(o->hi_b + 0.5f);
    if (k0 < 1) k0 = 1;
    if (k1 > SPECTRUM_FFT_SIZE / 2 - 1) k1 = SPECTRUM_FFT_SIZE / 2 - 1;
    for (int k = k0; k <= k1; k++) {
        const float a = fmaxf(o->lo_b, (float)k - 0.5f), b = fminf(o->hi_b, (float)k + 0.5f);
        if (b > a) p += (double)(b - a) * (double)mag[k] * (double)mag[k];
    }
    return (float)(2.0 * p / ((double)SPECTRUM_FFT_SIZE * spectrum_hamming_ss()));
}

/* Mean square to dB re a full-scale sine, on the bars' 0..1 scale. */
static inline float spectrum_oct_frac(float ms)
{
    const double ref = (double)SPECTRUM_SAMPLE_FULL_SCALE * SPECTRUM_SAMPLE_FULL_SCALE / 2.0;
    return spectrum_frac((float)(10.0 * log10((double)ms / ref + 1e-18)));
}

/* Fast or Slow: the exponential time weighting of a mean square, over a
 * frame of dt seconds, framerate-independent like the ballistics. */
static inline void spectrum_oct_weight(float *e, float ms, float dt, double tau)
{
    *e += (ms - *e) * (float)(1.0 - exp(-(double)dt / tau));
}

/*
 * 0034: one USB microphone sample, of the width uac_mic_read() handed
 * back (16, or 24 sign-extended in int32), as the 16-bit PCM a USB
 * output takes. A 16-bit value passes unchanged; a 24-bit one is
 * rounded to the nearest 16-bit step (half away from zero) and held to
 * the int16 range, so the top of a 24-bit full scale does not wrap.
 */
static inline int16_t spectrum_pcm16(int32_t v, unsigned bits)
{
    if (bits == 24) {
        v = v >= 0 ? (v + 128) / 256 : -((-v + 128) / 256);
    }
    if (v > 32767) v = 32767;
    if (v < -32768) v = -32768;
    return (int16_t)v;
}

/*
 * 0034: n frames of a USB microphone's own channels -- one or two, at
 * its own width -- to interleaved 16-bit stereo for a USB output. A
 * stereo microphone keeps its left and right; a mono one is the same
 * sample on both sides. Nothing is mixed, folded or re-levelled: what
 * the microphone sent, at the output's width.
 */
static inline void spectrum_passthrough(const int32_t *frames, size_t n,
                                        unsigned ch, unsigned bits, int16_t *stereo)
{
    for (size_t i = 0; i < n; i++) {
        const int16_t l = spectrum_pcm16(frames[ch == 2 ? 2 * i : i], bits);
        const int16_t r = ch == 2 ? spectrum_pcm16(frames[2 * i + 1], bits) : l;
        stereo[2 * i] = l;
        stereo[2 * i + 1] = r;
    }
}

#ifdef __cplusplus
}
#endif
