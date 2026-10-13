/*
 * render.c -- main/analyzer.c's own drawing, run on a PC, written out as
 * pictures of the screen. What README's docs/mockup.png was made with.
 *
 *   make -C test/render          (after an idf.py build, for the font)
 *
 * analyzer.c is included whole, against stub/ for the hardware, and
 * driven the way app_main() drives it -- feed(), capture(), fft(),
 * render(), waterfall_push(), the meter line -- on ten seconds of a
 * made-up signal: room noise, 60 Hz hum, plucked chords, a sine sweep and
 * a clap. Then the shadow buffer is read back through landmap.h, so the
 * picture is the landscape the panel shows, and saved as a PPM per view.
 *
 * Nothing here is a measurement. It is for seeing the layout, the text,
 * the colours and the views before flashing.
 *
 * SPDX-License-Identifier: MIT
 */
#define app_main analyzer_app_main
#include "../../main/analyzer.c"

#include <stdlib.h>

int64_t  stub_now_us;
uint16_t stub_fb[LCD_H_RES * LCD_V_RES];

#define FPS         (45)
#define SECONDS     (10.0)

/* ---- the signal ---- */
static double s_t;               /* seconds */
static double s_pink[3];
static uint32_t s_rng = 12345;

static double white(void)
{
    s_rng = s_rng * 1664525u + 1013904223u;
    return ((double)(s_rng >> 8) / 8388608.0) - 1.0;
}

static double db(double d) { return pow(10.0, d / 20.0); }

static float sample(double rate)
{
    const double t = s_t, pi2 = 2.0 * M_PI;
    double x = 0.0;
    /* pink-ish room noise (Paul Kellet's economy filter) */
    const double w = white();
    s_pink[0] = 0.99765 * s_pink[0] + w * 0.0990460;
    s_pink[1] = 0.96300 * s_pink[1] + w * 0.2965164;
    s_pink[2] = 0.57000 * s_pink[2] + w * 1.0526913;
    x += (s_pink[0] + s_pink[1] + s_pink[2] + w * 0.1848) * db(-72);
    /* hum */
    x += db(-46) * sin(pi2 * 60 * t) + db(-58) * sin(pi2 * 120 * t) + db(-54) * sin(pi2 * 180 * t);
    /* plucked chords */
    static const struct { double t0, f[3]; } ch[] = {
        { 0.3, { 110, 165, 220 } }, { 1.5, { 98, 147, 196 } }, { 2.7, { 131, 196, 262 } },
        { 8.0, { 110, 165, 220 } }, { 9.1, { 147, 220, 294 } },
    };
    for (size_t c = 0; c < sizeof(ch) / sizeof(ch[0]); c++) {
        const double tt = t - ch[c].t0;
        if (tt < 0) continue;
        for (int n = 0; n < 3; n++)
            for (int h = 1; h <= 16 && ch[c].f[n] * h < rate / 2; h++)
                x += exp(-tt * (1.2 + 0.5 * h)) * db(-14 - 3.2 * h) * sin(pi2 * ch[c].f[n] * h * tt);
    }
    /* a log sweep, 40 Hz to 16 kHz over 3.6..6.8 s */
    if (t >= 3.6 && t < 6.8) {
        const double K = 3.2 / log(16000.0 / 40.0), ts = t - 3.6;
        x += db(-12) * sin(pi2 * 40.0 * K * (exp(ts / K) - 1.0));
    }
    /* a clap at 7.4 s */
    if (t >= 7.4 && t < 8.0) x += white() * db(-10) * exp(-(t - 7.4) * 11.0);
    s_t += 1.0 / rate;
    return (float)(x * SPECTRUM_SAMPLE_FULL_SCALE);
}

/* ---- the screen ---- */
static void save(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    fprintf(f, "P6\n%d %d\n255\n", LAND_W, LAND_H);
    for (int ly = 0; ly < LAND_H; ly++)
        for (int lx = 0; lx < LAND_W; lx++) {
            const uint16_t c = stub_fb[landmap_py(lx, ly, LAND_W) * LCD_H_RES +
                                       landmap_px(lx, ly, LAND_W)];
            const unsigned char rgb[3] = {
                (unsigned char)(((c >> 11) & 0x1F) * 255 / 31),
                (unsigned char)(((c >> 5) & 0x3F) * 255 / 63),
                (unsigned char)((c & 0x1F) * 255 / 31),
            };
            fwrite(rgb, 1, 3, f);
        }
    fclose(f);
    printf("wrote %s\n", path);
}

/* app_main()'s setup, without the hardware or the loop. */
static void setup(void)
{
    for (int i = 0; i < SPECTRUM_FFT_SIZE; i++) s_window[i] = spectrum_hamming(i, SPECTRUM_FFT_SIZE);
    stages_init();              /* 0032 */
    s_w = LAND_W;
    s_h = LAND_H;
    for (int i = 0; i < 256; i++) {
        int r, g, b;
        spectrum_heat_rgb((float)i / 255.0f, &r, &g, &b);
        s_heat[i] = RGB(r, g, b);
    }
    set_geometry();
}

/* Ten seconds of the signal through one view, then its picture. */
static void run(view_mode_t mode, uint32_t rate, int src, const char *path)
{
    memset(&s_ring, 0, sizeof(s_ring));
    stages_reset();             /* 0032 */
    memset(s_pink, 0, sizeof(s_pink));
    s_t = 0.0;
    s_rng = 12345;
    s_rate = rate;
    spectrum_meter_init(&s_meter, (double)rate);
    snprintf(s_usb_label, sizeof(s_usb_label), rate == SPECTRUM_SAMPLE_RATE
             ? "USB Blue Snowball" : "USB Blue Snowball, %u kHz", (unsigned)(rate / 1000));

    s_mode = mode;
    layout_for(rate, src);
    draw_source(src);

    static float chunk[CAPTURE_CHUNK];
    const int per_frame = (int)(rate / FPS);
    const float dt = 1.0f / FPS;
    for (int frame = 0; frame < (int)(SECONDS * FPS); frame++) {
        for (int done = 0; done < per_frame; ) {
            const int n = per_frame - done < CAPTURE_CHUNK ? per_frame - done : CAPTURE_CHUNK;
            for (int i = 0; i < n; i++) chunk[i] = sample((double)rate);
            feed(chunk, (size_t)n);
            done += n;
        }
        stub_now_us += 1000000 / FPS;
        if (!capture()) continue;
        fft();
        int x0, x1;
        render(dt, &x0, &x1);
        waterfall_push(spectrum_wf_rows(&s_wf_acc, dt, WF_ROWS_PER_SEC), &x0, &x1);
    }
    draw_meters();
    draw_fps((float)FPS);
    save(path);
}

int main(void)
{
    setup();
    run(MODE_SPECTRUM,  48000, SRC_USB,     "bars.ppm");
    run(MODE_OCT,       48000, SRC_USB,     "oct.ppm");
    run(MODE_OCT,       44100, SRC_USB,     "oct-44k.ppm");
    run(MODE_WATERFALL, 48000, SRC_USB,     "fall.ppm");
    /* the built-in microphones: everything under 80 Hz greyed */
    run(MODE_SPECTRUM,  48000, SRC_BUILTIN, "bars-builtin.ppm");
    run(MODE_OCT,       48000, SRC_BUILTIN, "oct-builtin.ppm");
    run(MODE_WATERFALL, 48000, SRC_BUILTIN, "fall-builtin.ppm");
    return 0;
}
