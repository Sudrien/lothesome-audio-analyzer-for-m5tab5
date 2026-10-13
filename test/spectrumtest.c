/*
 * spectrumtest.c -- main/spectrum.h on the host.
 *
 * The FFT here is a direct DFT of the bins under test, not esp-dsp: the
 * point is to check the scaling and the band arithmetic against an
 * answer computed a different way, and a DFT is the definition the FFT
 * has to agree with.
 *
 * SPDX-License-Identifier: MIT
 */
#define _GNU_SOURCE
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "landmap.h"
#include "monoring.h"
#include "spectrum.h"

static int checks, failures;

/* 0013: a band's top edge in Hz, whichever FFT it reads, so bands on
 * either side of a stage seam can be put in order. */
static float band_top_hz(const spectrum_band_t *b, float rate)
{
    const float hz_per_bin = spectrum_stage_hz_per_bin(rate, b->stage);   /* 0032 */
    return (b->narrow ? b->centre_bin : (float)b->hi_bin) * hz_per_bin;
}

#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define N SPECTRUM_FFT_SIZE

/* |X[k]| of x[0..N), windowed, by the definition. */
static double dft_mag(const float *x, int k)
{
    double re = 0, im = 0;
    for (int n = 0; n < N; n++) {
        const double w = spectrum_hamming(n, N);
        const double a = -2.0 * M_PI * k * n / N;
        re += x[n] * w * cos(a);
        im += x[n] * w * sin(a);
    }
    return sqrt(re * re + im * im);
}

static void tone(float *x, int bin, double dbfs)
{
    const double amp = SPECTRUM_SAMPLE_FULL_SCALE * pow(10.0, dbfs / 20.0);
    for (int n = 0; n < N; n++) x[n] = (float)(amp * sin(2.0 * M_PI * bin * n / N));
}

static void test_scale(void)
{
    static float x[N];
    const int bins[] = { 3, 21, 100, 400 };
    const double levels[] = { 0.0, -20.0, -40.0, -80.0 };
    for (size_t i = 0; i < sizeof(bins) / sizeof(bins[0]); i++) {
        for (size_t j = 0; j < sizeof(levels) / sizeof(levels[0]); j++) {
            tone(x, bins[i], levels[j]);
            const float db = spectrum_dbfs((float)dft_mag(x, bins[i]));
            CHECK(fabs(db - levels[j]) < 0.1, "bin %d at %.0f dBFS read %.3f",
                  bins[i], levels[j], db);
        }
    }
    CHECK(spectrum_dbfs(0.0f) < -170.0f, "silence read %.1f", spectrum_dbfs(0.0f));
    CHECK(spectrum_frac(spectrum_dbfs(0.0f)) == 0.0f, "silence not at the floor");
    CHECK(spectrum_frac(0.0f) == 1.0f, "0 dBFS not at the top");
    CHECK(fabsf(spectrum_frac(-45.0f) - 0.5f) < 1e-6f, "-45 dBFS not halfway");
    CHECK(spectrum_frac(6.0f) == 1.0f, "over full scale not clamped");
}

static void test_bands(void)
{
    int narrow = 0;
    float prev_hz = 0;
    int prev_stage = SPECTRUM_STAGES - 1;
    for (int b = 0; b < SPECTRUM_BARS; b++) {
        const spectrum_band_t s = spectrum_band(b);
        if (s.narrow) {
            narrow++;
            CHECK(s.centre_bin >= 1.0f && s.centre_bin <= N / 2 - 2, "bar %d centre %f", b, s.centre_bin);
        } else {
            CHECK(s.lo_bin >= 1 && s.hi_bin <= N / 2 - 1, "bar %d bins %d..%d", b, s.lo_bin, s.hi_bin);
            CHECK(s.lo_bin <= s.hi_bin, "bar %d empty", b);
        }
        const float hz = band_top_hz(&s, 48000.0f);
        CHECK(hz >= prev_hz, "bar %d goes backwards", b);
        prev_hz = hz;
        CHECK(s.stage <= prev_stage, "bar %d back on a slower stage", b);   /* 0032 */
        prev_stage = s.stage;
    }
    /* 0013: 20 Hz to 24 kHz in 64 log steps is ~11.7% a bar; the bass
     * FFT's bin is 2.9 Hz, so only the first bar or two are narrower
     * than a bin. */
    CHECK(narrow > 0 && narrow < SPECTRUM_BARS / 2, "%d narrow bars", narrow);
    const spectrum_band_t last = spectrum_band(SPECTRUM_BARS - 1);
    CHECK(!last.narrow && last.hi_bin == N / 2 - 1, "top bar does not reach Nyquist");

    /* A narrow bar between two bins reads between their values. */
    static float mag[N / 2];
    for (int i = 0; i < N / 2; i++) mag[i] = (float)i;
    const spectrum_band_t s0 = spectrum_band(0);
    CHECK(fabsf(spectrum_band_mag(&s0, mag) - s0.centre_bin) < 1e-4f, "interpolation off");
}

static void test_ballistics(void)
{
    float level = 0, peak = 0;
    spectrum_ballistics(1.0f, SPECTRUM_ATTACK_TAU, &level, &peak);
    CHECK(fabsf(level - (1.0f - expf(-1.0f))) < 1e-5f, "one attack tau reached %f", level);
    CHECK(peak == level, "peak did not follow the rise");

    /* The same 40 ms as one frame or as forty: framerate-independent. */
    float a = 0.5f, pa = 0.5f, b = 0.5f, pb = 0.5f;
    spectrum_ballistics(0.0f, 0.040f, &a, &pa);
    for (int i = 0; i < 40; i++) spectrum_ballistics(0.0f, 0.001f, &b, &pb);
    CHECK(fabsf(a - b) < 1e-4f, "release depends on frame rate: %f vs %f", a, b);
    CHECK(fabsf(pa - pb) < 1e-4f, "peak fall depends on frame rate: %f vs %f", pa, pb);
    CHECK(fabsf(pa - (0.5f - SPECTRUM_PEAK_FALL_PER_SEC * 0.040f)) < 1e-4f, "peak fell %f", pa);

    float l = 0.1f, p = 0.0f;
    for (int i = 0; i < 1000; i++) spectrum_ballistics(0.0f, 0.01f, &l, &p);
    CHECK(p == 0.0f && l < 1e-6f, "did not settle at zero");
}

static void test_axes(void)
{
    CHECK(spectrum_freq_x(SPECTRUM_LOG_MIN_HZ, 1000) == 0, "50 Hz not at the left");
    CHECK(spectrum_freq_x(SPECTRUM_LOG_MAX_HZ, 1000) == 1000, "Nyquist not at the right");
    CHECK(spectrum_freq_x(10.0f, 1000) == 0, "below range not clamped");
    const int mid = spectrum_freq_x(sqrtf(SPECTRUM_LOG_MIN_HZ * SPECTRUM_LOG_MAX_HZ), 1000);
    CHECK(mid >= 499 && mid <= 500, "geometric mean at %d", mid);

    int r, g;
    spectrum_row_rg(0.0f, &r, &g);  CHECK(r == 0 && g == 255, "bottom not green");
    spectrum_row_rg(0.5f, &r, &g);  CHECK(r == 255 && g == 255, "middle not yellow");
    spectrum_row_rg(1.0f, &r, &g);  CHECK(r == 255 && g == 0, "top not red");
}

/* 0003: the newest window comes out contiguous and in order, across the
 * wrap and across the 2^32 wrap of the count. */
static void test_ring(void)
{
    static monoring_t r;
    static float out[N], chunk[120];
    float next = 0.0f;
    for (int round = 0; round < 200; round++) {
        for (int i = 0; i < 120; i++) chunk[i] = next++;
        monoring_write(&r, chunk, 120);
        const uint32_t end = monoring_count(&r);
        if (end < N) continue;
        monoring_newest(&r, end, out, N);
        int ok = 1;
        for (int i = 0; i < N; i++) ok &= out[i] == next - N + i;
        CHECK(ok, "window wrong after %u samples", (unsigned)end);
    }
    r.count = UINT32_MAX - 50;          /* about to wrap the count */
    next = 0.0f;
    for (int round = 0; round < 20; round++) {
        for (int i = 0; i < 120; i++) chunk[i] = next++;
        monoring_write(&r, chunk, 120);
    }
    monoring_newest(&r, monoring_count(&r), out, N);
    int ok = 1;
    for (int i = 0; i < N; i++) ok &= out[i] == next - N + i;
    CHECK(ok, "window wrong across the count's wrap");
}

/* 0006: landmap.h's forward map against the inverse gfx's 270 gather
 * uses (feckless-graphics-handler gather_rotated(): ly = px,
 * lx = s_w - 1 - py), for every pixel of the landscape. */
static void test_landmap(void)
{
    enum { LW = 1280, LH = 720 };
    int ok = 1;
    for (int ly = 0; ly < LH && ok; ly++)
        for (int lx = 0; lx < LW && ok; lx++) {
            const int px = landmap_px(lx, ly, LW), py = landmap_py(lx, ly, LW);
            ok = px >= 0 && px < LH && py >= 0 && py < LW &&
                 py == (LW - 1) - lx && px == ly;        /* inverse: ly=px, lx=LW-1-py */
            ok = ok && (LW - 1 - py) == lx && px == ly;
        }
    CHECK(ok, "landmap_px/py is not gfx's 270");

    /* A rectangle maps to the rectangle its corners map to. */
    const landmap_rect_t r = landmap_rect(100, 70, 16, 604, LW);
    CHECK(r.x == 70 && r.w == 604, "rect x/w %d/%d", r.x, r.w);
    CHECK(r.y == landmap_py(115, 70, LW) && r.y + r.h - 1 == landmap_py(100, 70, LW),
          "rect rows %d..%d", r.y, r.y + r.h - 1);

    int y0, y1;
    landmap_rows(100, 116, LW, &y0, &y1);
    CHECK(y0 == r.y && y1 == r.y + r.h, "rows %d..%d for the same columns", y0, y1);
    landmap_rows(0, LW, LW, &y0, &y1);
    CHECK(y0 == 0 && y1 == LW, "whole width is not every panel row");
}

/* 0008: other sample rates. The bars still cover 80 Hz to Nyquist, in
 * order, and a tone still reads its level wherever it lands. */
static void test_rates(void)
{
    const float rates[] = { 16000.0f, 22050.0f, 32000.0f, 44100.0f, 48000.0f };
    for (size_t r = 0; r < sizeof(rates) / sizeof(rates[0]); r++) {
        int ok = 1;
        float prev_hz = 0;
        for (int b = 0; b < SPECTRUM_BARS; b++) {
            const spectrum_band_t s = spectrum_band_at(b, rates[r]);
            if (!s.narrow) ok &= s.lo_bin >= 1 && s.hi_bin >= s.lo_bin && s.hi_bin <= N / 2 - 1;
            const float hz = band_top_hz(&s, rates[r]);
            ok &= hz >= prev_hz;
            prev_hz = hz;
        }
        CHECK(ok, "bands out of order at %.0f Hz", rates[r]);
        const spectrum_band_t top = spectrum_band_at(SPECTRUM_BARS - 1, rates[r]);
        CHECK(!top.narrow && top.hi_bin == N / 2 - 1, "top bar short of Nyquist at %.0f", rates[r]);
        CHECK(spectrum_freq_x_at(rates[r] / 2, 1000, rates[r] / 2) == 1000,
              "Nyquist not at the right at %.0f", rates[r]);
    }
    const spectrum_band_t a = spectrum_band(10), b = spectrum_band_at(10, 48000.0f);
    CHECK(a.narrow == b.narrow && a.lo_bin == b.lo_bin && a.hi_bin == b.hi_bin &&
          a.centre_bin == b.centre_bin, "spectrum_band() is not the 48 kHz case");
}

/* 0012: a bar per pixel column. The graph is 1170 columns wide; the
 * columns still run 80 Hz to Nyquist in order, and a tone reads its
 * level in a wide column, not its level shared among the column's bins. */
static void test_columns(void)
{
    const int cols = 1170;
    int ok = 1;
    float prev_hz = 0;
    for (int b = 0; b < cols; b++) {
        const spectrum_band_t s = spectrum_band_n(b, cols, 48000.0f);
        if (!s.narrow) ok &= s.lo_bin >= 1 && s.hi_bin >= s.lo_bin && s.hi_bin <= N / 2 - 1;
        const float hz = band_top_hz(&s, 48000.0f);
        ok &= hz >= prev_hz;
        prev_hz = hz;
    }
    CHECK(ok, "columns out of order");
    const spectrum_band_t top = spectrum_band_n(cols - 1, cols, 48000.0f);
    CHECK(!top.narrow && top.hi_bin == N / 2 - 1, "top column short of Nyquist");

    static float mag[N / 2];
    const spectrum_band_t w = spectrum_band(SPECTRUM_BARS - 1);
    mag[w.lo_bin] = 123.0f;
    CHECK(spectrum_band_mag(&w, mag) == 123.0f, "a tone in a wide bar read %f",
          spectrum_band_mag(&w, mag));
    const spectrum_band_t a = spectrum_band_at(10, 44100.0f),
                          b = spectrum_band_n(10, SPECTRUM_BARS, 44100.0f);
    CHECK(a.narrow == b.narrow && a.lo_bin == b.lo_bin && a.hi_bin == b.hi_bin &&
          a.centre_bin == b.centre_bin, "spectrum_band_at() is not SPECTRUM_BARS bars");
}

/* 0032: n half-band stages' response at f Hz of 48 kHz, by the
 * definition: stage j runs at 48 kHz / 2^j and sees f at that rate. */
static double hb_db(const float *h, int stages, double f)
{
    double db = 0.0;
    for (int j = 0; j < stages; j++) {
        const double fs = 48000.0 / (1 << j);
        double re = 0, im = 0;
        for (int n = 0; n < SPECTRUM_HB_TAPS; n++) {
            re += h[n] * cos(2.0 * M_PI * f / fs * n);
            im -= h[n] * sin(2.0 * M_PI * f / fs * n);
        }
        db += 20.0 * log10(sqrt(re * re + im * im) + 1e-12);
    }
    return db;
}

/* 0032: the stages. Each halving flat to its split and deep where
 * anything would fold onto it; a tone through k halvings reads the same
 * dBFS on stage k as on stage 0; bands go to the stage that holds them. */
static void test_stages(void)
{
    static float h[SPECTRUM_HB_TAPS];
    spectrum_hb_filter(h);

    const double splits[] = { 0, 9600, 4800, 2400, 1200 };
    for (int k = 1; k < SPECTRUM_STAGES; k++) {
        const double split = spectrum_stage_split_hz(48000.0f, k);
        CHECK(fabs(split - splits[k]) < 1e-3, "stage %d split at %.1f Hz", k, split);
        /* Flat to the split, through all k halvings. */
        double worst_pass = 0;
        for (double f = 0; f <= split; f += split / 400)
            if (fabs(hb_db(h, k, f)) > worst_pass) worst_pass = fabs(hb_db(h, k, f));
        CHECK(worst_pass < 0.05, "stage %d: passband ripple %.3f dB", k, worst_pass);
        /* What would fold onto [0, split] at stage k's rate comes from
         * [rate_k - split, ...); the last halving is what removes it. */
        const double fs_k = 48000.0 / (1 << k);
        double worst_stop = -999;
        for (double f = fs_k - split; f <= 24000.0; f += 5) {
            const double d = hb_db(h, k, f);
            if (d > worst_stop) worst_stop = d;
        }
        CHECK(worst_stop < -70.0, "stage %d: alias only %.1f dB down", k, worst_stop);
    }

    /* A bin-centred tone at -20 dBFS through k halvings, in capture-sized
     * chunks: bin 100 of stage k's FFT reads -20 there. */
    for (int k = 1; k < SPECTRUM_STAGES; k++) {
        static spectrum_hb_t d[SPECTRUM_STAGES];
        memset(d, 0, sizeof(d));
        static float a[128], b[128], st[N * 2];
        const double fs_k = 48000.0 / (1 << k);
        const double f = 100.0 * fs_k / N;
        const double amp = SPECTRUM_SAMPLE_FULL_SCALE * pow(10.0, -20.0 / 20.0);
        int have = 0;
        long t = 0;
        while (have < N * 2) {
            for (int i = 0; i < 120; i++, t++) a[i] = (float)(amp * sin(2.0 * M_PI * f * t / 48000.0));
            int m = 120;
            float *src = a, *dst = b;
            for (int j = 1; j <= k; j++) {
                const int in_n = m;
                m = spectrum_hb(&d[j], h, src, m, dst);
                CHECK(m <= in_n / 2 + 1, "stage %d: %d outputs from %d", j, m, in_n);
                float *x = src; src = dst; dst = x;
            }
            for (int i = 0; i < m && have < N * 2; i++) st[have++] = src[i];
        }
        const float db = spectrum_dbfs((float)dft_mag(st + N, 100));
        CHECK(fabsf(db + 20.0f) < 0.1f, "stage %d tone read %.3f dBFS", k, db);
    }

    /* Bands to stages, at the table's edges. */
    CHECK(spectrum_stage_for(20.0f, 48000.0f) == 4, "20 Hz not on stage 4");
    CHECK(spectrum_stage_for(1199.0f, 48000.0f) == 4 && spectrum_stage_for(1201.0f, 48000.0f) == 3,
          "the 1.2 kHz seam");
    CHECK(spectrum_stage_for(2401.0f, 48000.0f) == 2 && spectrum_stage_for(4801.0f, 48000.0f) == 1,
          "the 2.4 and 4.8 kHz seams");
    CHECK(spectrum_stage_for(9601.0f, 48000.0f) == 0 && spectrum_stage_for(24000.0f, 48000.0f) == 0,
          "the 9.6 kHz seam");
    const spectrum_band_t lo = spectrum_band_n(0, 1170, 48000.0f);
    const spectrum_band_t hi = spectrum_band_n(1169, 1170, 48000.0f);
    CHECK(lo.stage == 4 && hi.stage == 0, "columns at the ends on stages %d and %d", lo.stage, hi.stage);
}

/* 0014: the weighting curves against the analog ones, held to IEC
 * 61672-1:2013's class 1 limits, at the two rates the sources use. */
static void test_weighting(void)
{
    static const struct { double hz, up, down; } k[] = {
        { 10, 3.0, 99 }, { 20, 2.0, 2.0 }, { 31.5, 1.5, 1.5 }, { 63, 1.0, 1.0 },
        { 125, 1.0, 1.0 }, { 250, 1.0, 1.0 }, { 500, 1.0, 1.0 }, { 1000, 0.7, 0.7 },
        { 2000, 1.0, 1.0 }, { 4000, 1.0, 1.0 }, { 6300, 1.5, 2.0 }, { 8000, 1.5, 2.5 },
        { 10000, 2.0, 3.0 }, { 12500, 2.0, 5.0 }, { 16000, 2.5, 16.0 },
    };
    const double rates[] = { 44100.0, 48000.0 };
    for (int r = 0; r < 2; r++) {
        spectrum_biquad_t c[2], a[3];
        spectrum_weighting(c, a, rates[r]);
        for (size_t i = 0; i < sizeof(k) / sizeof(k[0]); i++) {
            const double ea = 20 * log10(spectrum_biquads_mag(a, 3, k[i].hz, rates[r])) - spectrum_a_db(k[i].hz);
            const double ec = 20 * log10(spectrum_biquads_mag(c, 2, k[i].hz, rates[r])) - spectrum_c_db(k[i].hz);
            CHECK(ea <= k[i].up && ea >= -k[i].down, "A off by %+.2f dB at %.0f Hz, %.0f", ea, k[i].hz, rates[r]);
            CHECK(ec <= k[i].up && ec >= -k[i].down, "C off by %+.2f dB at %.0f Hz, %.0f", ec, k[i].hz, rates[r]);
            if (k[i].hz <= 1000)
                CHECK(fabs(ea) < 0.1 && fabs(ec) < 0.1, "%.0f Hz off by %+.2f / %+.2f", k[i].hz, ea, ec);
        }
    }
    CHECK(fabs(spectrum_a_db(1000)) < 0.01 && fabs(spectrum_c_db(1000)) < 0.01, "analog not 0 at 1 kHz");
    CHECK(fabs(spectrum_a_db(100) + 19.1) < 0.05, "analog A at 100 Hz %.2f", spectrum_a_db(100));
}

/* 0014: the meter on sines, through the float filters, in chunks. */
static void meter_sine(spectrum_meter_t *m, double hz, double dbfs, double secs)
{
    static float x[120];
    const double amp = SPECTRUM_SAMPLE_FULL_SCALE * pow(10.0, dbfs / 20.0);
    static long t;
    for (long done = 0; done < (long)(secs * 48000.0); done += 120) {
        for (int i = 0; i < 120; i++, t++) x[i] = (float)(amp * sin(2.0 * M_PI * hz * t / 48000.0));
        spectrum_meter_push(m, x, 120);
    }
}

static void test_meter(void)
{
    static spectrum_meter_t m;
    spectrum_meter_init(&m, 48000.0);
    meter_sine(&m, 1000.0, -20.0, 0.5);
    spectrum_meter_reset(&m);                     /* past the filters' start */
    meter_sine(&m, 1000.0, -20.0, 2.0);
    CHECK(fabsf(spectrum_laeq(&m) + 20.0f) < 0.05f, "1 kHz LAeq %.3f", spectrum_laeq(&m));
    CHECK(fabsf(spectrum_lafmax(&m) + 20.0f) < 0.1f, "1 kHz LAFmax %.3f", spectrum_lafmax(&m));
    CHECK(fabsf(spectrum_lcpeak(&m) + 20.0f) < 0.05f, "1 kHz LCpeak %.3f", spectrum_lcpeak(&m));
    CHECK(!m.clipped, "clipped at -20");

    spectrum_meter_init(&m, 48000.0);
    meter_sine(&m, 100.0, -20.0, 0.5);
    spectrum_meter_reset(&m);
    meter_sine(&m, 100.0, -20.0, 2.0);
    CHECK(fabsf(spectrum_laeq(&m) - (float)(-20.0 + spectrum_a_db(100))) < 0.1f,
          "100 Hz LAeq %.3f", spectrum_laeq(&m));

    /* Fast falls 34.7 dB a second (IEC 61672-1: 8.69 / 0.125 s). */
    const float before = spectrum_laf(&m);
    meter_sine(&m, 100.0, -200.0, 1.0);
    CHECK(fabsf(before - spectrum_laf(&m) - 34.7f) < 0.5f, "Fast fell %.2f dB in 1 s",
          before - spectrum_laf(&m));
    CHECK(spectrum_lafmax(&m) >= before - 0.01f, "LAFmax did not hold");

    /* Leq is energy over time: a second at -20 and a second of silence
     * is 3 dB under -20. */
    spectrum_meter_init(&m, 48000.0);
    meter_sine(&m, 1000.0, -20.0, 0.5);
    spectrum_meter_reset(&m);
    meter_sine(&m, 1000.0, -20.0, 1.0);
    meter_sine(&m, 1000.0, -300.0, 1.0);
    CHECK(fabsf(spectrum_laeq(&m) + 23.01f) < 0.1f, "half-silent LAeq %.3f", spectrum_laeq(&m));

    meter_sine(&m, 1000.0, 0.0, 0.1);
    CHECK(m.clipped, "a full-scale sine did not flag");
    spectrum_meter_reset(&m);
    CHECK(!m.clipped && m.n == 0 && spectrum_laeq(&m) < -170.0f, "reset left readings");
}

/* 0015: the waterfall's colours get lighter all the way up, from black
 * to pale yellow, and its rows keep time at any framerate. */
static void test_waterfall(void)
{
    int r, g, b, ok = 1;
    double prev = -1;
    for (int i = 0; i <= 256; i++) {
        spectrum_heat_rgb(i / 256.0f, &r, &g, &b);
        ok &= r >= 0 && r <= 255 && g >= 0 && g <= 255 && b >= 0 && b <= 255;
        const double luma = 0.2126 * r + 0.7152 * g + 0.0722 * b;
        ok &= luma >= prev;
        prev = luma;
    }
    CHECK(ok, "heat map not rising in lightness");
    spectrum_heat_rgb(0.0f, &r, &g, &b);
    CHECK(r + g + b < 10, "floor not black: %d %d %d", r, g, b);
    spectrum_heat_rgb(2.0f, &r, &g, &b);
    CHECK(r > 240 && g > 240, "top not pale yellow: %d %d %d", r, g, b);

    const float rates[] = { 7.0f, 23.0f, 45.0f, 120.0f };   /* frames a second */
    for (int i = 0; i < 4; i++) {
        float acc = 0.0f;
        int rows = 0;
        for (int f = 0; f < (int)(rates[i] * 10.0f); f++)
            rows += spectrum_wf_rows(&acc, 1.0f / rates[i], 40.0f);
        CHECK(rows >= 399 && rows <= 400, "%d rows in 10 s at %.0f fps", rows, rates[i]);
    }
}

/* 0016: one-third octaves. The centres are the standard's; a tone at
 * a band's centre reads its level there; flat power per bin -- white
 * noise -- rises 1 dB a band, so pink reads flat; Fast and Slow settle. */
static void test_octaves(void)
{
    CHECK(fabsf(spectrum_oct_centre(0) - 19.953f) < 0.01f, "first centre %f", spectrum_oct_centre(0));
    CHECK(fabsf(spectrum_oct_centre(17) - 1000.0f) < 0.01f, "1 kHz not band 17");
    CHECK(fabsf(spectrum_oct_centre(30) - 19953.0f) < 1.0f, "last centre %f", spectrum_oct_centre(30));

    /* A bin-centred tone in a band of stage 0: 10 kHz band, bin 213
     * (9984 Hz), and 16 kHz band (bin 341, 15984 Hz). 0032: the stages
     * below are each checked for level in test_stages(). */
    static float x[N];
    static float mag[N / 2];
    const int tests[][2] = { { 27, 213 }, { 29, 341 } };
    for (int t = 0; t < 2; t++) {
        tone(x, tests[t][1], -20.0);
        for (int k = 1; k < N / 2; k++) mag[k] = (float)dft_mag(x, k);
        const spectrum_oct_t o = spectrum_oct_band(tests[t][0], 48000.0f);
        CHECK(o.stage == 0 && !o.partial, "band %d on stage %d", tests[t][0], o.stage);
        const float db = (float)(10.0 * log10(spectrum_oct_ms(&o, mag) /
                         ((double)SPECTRUM_SAMPLE_FULL_SCALE * SPECTRUM_SAMPLE_FULL_SCALE / 2.0)));
        CHECK(fabsf(db + 20.0f) < 0.3f, "band %d tone read %.2f", tests[t][0], db);
    }

    /* White: every bin the same power. A band's power is its width. */
    for (int k = 0; k < N / 2; k++) mag[k] = 1000.0f;
    double prev = 0;
    int ok = 1;
    for (int b = 0; b < SPECTRUM_OCT_BANDS - 1; b++) {
        const spectrum_oct_t o = spectrum_oct_band(b, 48000.0f);
        const double db = 10.0 * log10(spectrum_oct_ms(&o, mag));
        if (b > 0) {
            const spectrum_oct_t p = spectrum_oct_band(b - 1, 48000.0f);
            /* 0032: across a seam the bin width doubles per stage, -3.01
             * dB of power a bin a stage; within a stage each band is 1 dB up. */
            const double want = 1.0 - 10.0 * log10((double)(1 << (p.stage - o.stage)));
            if (fabs(db - prev - want) > 0.05) {
                ok = 0;
                printf("  band %d step %.3f, want %.3f\n", b, db - prev, want);
            }
        }
        prev = db;
    }
    CHECK(ok, "white noise not 1 dB a band");
    CHECK(spectrum_oct_band(17, 48000.0f).stage == 4 && spectrum_oct_band(18, 48000.0f).stage == 3,
          "the 1.2 kHz seam not between the 1 and 1.25 kHz bands");
    CHECK(spectrum_oct_band(30, 44100.0f).partial && !spectrum_oct_band(30, 48000.0f).partial,
          "20 kHz band partial at the wrong rates");

    float e = 0.0f;
    for (int i = 0; i < 100; i++) spectrum_oct_weight(&e, 1.0f, 0.01f, SPECTRUM_FAST_TAU);
    CHECK(e > 0.9996f, "Fast not settled in 1 s: %f", e);
    float a = 0.0f, b = 0.0f;
    spectrum_oct_weight(&a, 1.0f, 0.5f, SPECTRUM_SLOW_TAU);
    for (int i = 0; i < 50; i++) spectrum_oct_weight(&b, 1.0f, 0.01f, SPECTRUM_SLOW_TAU);
    CHECK(fabsf(a - b) < 1e-4f, "Slow depends on frame rate");
}

int main(void)
{
    test_rates();
    test_landmap();
    test_ring();
    test_scale();
    test_bands();
    test_ballistics();
    test_columns();
    test_stages();
    test_weighting();
    test_meter();
    test_waterfall();
    test_octaves();
    test_axes();
    printf("spectrumtest: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
