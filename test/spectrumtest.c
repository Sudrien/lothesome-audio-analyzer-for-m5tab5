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

#include "landmap.h"
#include "monoring.h"
#include "spectrum.h"

static int checks, failures;

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
    int narrow = 0, prev_hi = 0;
    float prev_centre = 0;
    for (int b = 0; b < SPECTRUM_BARS; b++) {
        const spectrum_band_t s = spectrum_band(b);
        if (s.narrow) {
            narrow++;
            CHECK(s.centre_bin >= 1.0f && s.centre_bin <= N / 2 - 2, "bar %d centre %f", b, s.centre_bin);
            CHECK(s.centre_bin > prev_centre, "bar %d centre not rising", b);
            prev_centre = s.centre_bin;
            CHECK(narrow == b + 1, "narrow bar %d after a wide one", b);
        } else {
            CHECK(s.lo_bin >= 1 && s.hi_bin <= N / 2 - 1, "bar %d bins %d..%d", b, s.lo_bin, s.hi_bin);
            CHECK(s.lo_bin <= s.hi_bin, "bar %d empty", b);
            CHECK(s.hi_bin >= prev_hi, "bar %d goes backwards", b);
            prev_hi = s.hi_bin;
        }
    }
    /* 80 Hz to 24 kHz in 64 log steps is ~9.3% a bar; a bin is 46.9 Hz,
     * so bars stay narrower than a bin up to ~500 Hz. */
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
        int prev_hi = 0, ok = 1;
        for (int b = 0; b < SPECTRUM_BARS; b++) {
            const spectrum_band_t s = spectrum_band_at(b, rates[r]);
            if (!s.narrow) { ok &= s.lo_bin >= 1 && s.hi_bin >= s.lo_bin &&
                                   s.hi_bin <= N / 2 - 1 && s.hi_bin >= prev_hi;
                             prev_hi = s.hi_bin; }
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

int main(void)
{
    test_rates();
    test_landmap();
    test_ring();
    test_scale();
    test_bands();
    test_ballistics();
    test_axes();
    printf("spectrumtest: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
