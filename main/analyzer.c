/*
 * analyzer.c -- the Tab5's two microphones as a 64-band spectrum.
 *
 * A port of original/Tab5_SpectrumAnalyzer.ino from Arduino and
 * M5Unified to ESP-IDF and the feckless libraries. Same program: capture
 * 1024 samples at 48 kHz, Hamming window, FFT, 64 log-spaced bars in
 * dBFS with attack/release ballistics and peak markers, and only the
 * rows that changed redrawn. The arithmetic is spectrum.h; this file is
 * the hardware and the drawing.
 *
 * What replaced what:
 *
 *   M5.begin()           tab5io_init(), lcd_init(), gfx_init()
 *                        (feckless-drivers, feckless-graphics-handler)
 *   M5.Mic               audio_out_init() + audio_out_capture_*()
 *                        (feckless-drivers): the same ES7210, read as
 *                        stereo int32 at 24-bit scale and averaged to
 *                        mono here
 *   arduinoFFT           esp-dsp's dsps_fft2r_fc32(), with the Hamming
 *                        window computed here -- esp-dsp has none
 *   M5.Display drawing   straight into gfx_fb(), then gfx_blit() of the
 *                        rows that changed; text through gfx_draw_text()
 *                        in Ark12 rather than M5GFX's 6x8 font
 *   Serial.printf        ESP_LOGI
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_dsp.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "audio_out.h"
#include "gfx.h"
#include "lcd.h"
#include "tab5io.h"

#include "spectrum.h"

static const char *TAG = "analyzer";

/*
 * The sketch used setRotation(3): landscape, the same way up as the
 * printing on the back of the case. gfx counts quarter turns clockwise
 * from the panel's native portrait, and which of its two landscapes that
 * is has not been seen on a board yet.
 *
 * src: unverified. GFX_ROT_270 is a guess at M5GFX's 3 (= 270 degrees).
 *      If the picture is upside down, this is GFX_ROT_90.
 */
#define ANALYZER_ROTATION   (GFX_ROT_270)

/* The backlight, 0-100. The player's default (its LCD_BRIGHTNESS_PERCENT). */
#define ANALYZER_BACKLIGHT  (80)

/* Optional frame cap; 0 = unlimited. Only for cutting power draw. */
#define FRAME_LIMIT_HZ      (0)

/* Text, in Ark12 cells (12 px tall at scale 1). M5GFX's setTextSize(n)
 * was 8n px; these are the nearest that read cleanly at 12-px steps. */
#define TITLE_SCALE         (3)     /* was setTextSize(4), 32 px; now 36 */
#define LABEL_SCALE         (2)     /* was 2 (16 px) and 3 (24 px); now 24 */

/* Colours. TFT_DARKGREY and TFT_LIGHTGREY are M5GFX's 0x7BEF and 0xD69A. */
#define COL_BLACK           RGB(0, 0, 0)
#define COL_WHITE           RGB(255, 255, 255)
#define COL_GRID            RGB(45, 45, 45)
#define COL_PEAK            COL_WHITE
#define COL_BASE            ((uint16_t)0x7BEF)
#define COL_LABEL           ((uint16_t)0xD69A)

#define MAX_GRAPH_H         (800)   /* bound for the row tables */

/* ---- capture and FFT buffers: static, never on the task's stack ---- */
static int32_t s_frames[SPECTRUM_FFT_SIZE * 2];             /* capture, stereo */
static float   s_window[SPECTRUM_FFT_SIZE];
static float   s_fft[SPECTRUM_FFT_SIZE * 2] __attribute__((aligned(16)));  /* re, im */
static float   s_mag[SPECTRUM_FFT_SIZE / 2];

/* ---- layout ---- */
static int s_w, s_h;
static int s_left, s_right, s_top, s_bottom, s_graph_w, s_graph_h, s_bar_w;

/* The colour of each row, by height above the baseline, and whether a
 * dB gridline sits there. By row, never by level: see spectrum_row_rg(). */
static uint16_t s_row_colour[MAX_GRAPH_H + 1];
static bool     s_row_grid[MAX_GRAPH_H + 1];

/* ---- per bar ---- */
static spectrum_band_t s_band[SPECTRUM_BARS];
static float s_level[SPECTRUM_BARS];    /* smoothed 0..1, what is drawn */
static float s_peak[SPECTRUM_BARS];     /* peak hold 0..1 */
static int   s_prev_h[SPECTRUM_BARS];   /* last bar height, px */
static int   s_prev_peak_y[SPECTRUM_BARS];

static void freq_label(float hz, char *out, size_t out_len)
{
    if (hz >= 1000.0f) {
        const float k = hz / 1000.0f;
        snprintf(out, out_len, k >= 10.0f ? "%.0fk" : "%.1fk", k);
    } else {
        snprintf(out, out_len, "%d", (int)hz);
    }
}

/* A horizontal run of one colour, straight into the shadow buffer. */
static inline void hline(int x, int y, int w, uint16_t c)
{
    uint16_t *p = gfx_fb() + (size_t)y * (size_t)s_w + (size_t)x;
    for (int i = 0; i < w; i++) p[i] = c;
}

static void draw_static(void)
{
    gfx_fill_rect(0, 0, s_w, s_h, COL_BLACK);
    gfx_draw_text(10, 4, "Tab5 Spectrum Analyzer", TITLE_SCALE, s_w - 200, COL_WHITE);

    for (int h = 0; h <= s_graph_h; h++) {
        int r, g;
        spectrum_row_rg((float)h / (float)s_graph_h, &r, &g);
        s_row_colour[h] = RGB(r, g, 0);
        s_row_grid[h] = false;
    }

    /* dBFS ticks every 10 dB from the floor up to 0, labelled on the left. */
    const int label_h = GFX_GLYPH_H(LABEL_SCALE);
    int first = ((int)SPECTRUM_MIN_DBFS / 10) * 10;
    if (first < SPECTRUM_MIN_DBFS) first += 10;
    for (int db = first; db <= (int)SPECTRUM_MAX_DBFS; db += 10) {
        const float frac = ((float)db - SPECTRUM_MIN_DBFS) /
                           (SPECTRUM_MAX_DBFS - SPECTRUM_MIN_DBFS);
        const int h = (int)(frac * (float)s_graph_h);
        if (h < 0 || h > s_graph_h) continue;
        s_row_grid[h] = true;
        const int y = s_bottom - h;
        hline(s_left, y, s_graph_w, COL_GRID);

        char buf[8];
        snprintf(buf, sizeof(buf), "%d", db);
        const int tw = gfx_text_w(buf, LABEL_SCALE);
        gfx_draw_text(s_left - tw - 8, y - label_h / 2, buf, LABEL_SCALE, tw, COL_LABEL);
    }
    gfx_draw_text(10, s_top - label_h - 4, "dBFS", LABEL_SCALE, 200, COL_LABEL);

    /* Frequency labels along the log axis. */
    static const float label_hz[] = { 50, 100, 500, 1000, 5000, 10000, 20000 };
    for (size_t i = 0; i < sizeof(label_hz) / sizeof(label_hz[0]); i++) {
        const float f = label_hz[i];
        if (f < SPECTRUM_LOG_MIN_HZ || f > SPECTRUM_LOG_MAX_HZ) continue;
        char buf[8];
        freq_label(f, buf, sizeof(buf));
        int x = spectrum_freq_x(f, s_graph_w);
        if (x > s_graph_w - 60) x = s_graph_w - 60;
        if (x < 0) x = 0;
        gfx_draw_text(s_left + x, s_bottom + 10, buf, LABEL_SCALE, 120, COL_LABEL);
    }

    hline(s_left, s_bottom, s_graph_w, COL_BASE);
    gfx_blit(0, s_h);
}

/* Repaint rows [y_top..y_bot] of one bar's column with what belongs
 * there now. Called only on rows that changed since the last frame. */
static void repaint_rows(int xs, int w, int y_top, int y_bot, int bar_top_y, int peak_y)
{
    if (y_top < s_top)        y_top = s_top;
    if (y_bot > s_bottom - 1) y_bot = s_bottom - 1;
    for (int y = y_top; y <= y_bot; y++) {
        uint16_t c;
        if (y == peak_y)                        c = COL_PEAK;
        else if (y >= bar_top_y)                c = s_row_colour[s_bottom - y];
        else if (s_row_grid[s_bottom - y])      c = COL_GRID;
        else                                    c = COL_BLACK;
        hline(xs, y, w, c);
    }
}

/* Fill s_fft with one window's worth of mono samples. Returns false if
 * the capture stopped delivering. */
static bool capture(void)
{
    size_t got = 0;
    while (got < SPECTRUM_FFT_SIZE) {
        const size_t n = audio_out_capture_read(s_frames + got * 2,
                                                SPECTRUM_FFT_SIZE - got, 100);
        if (n == 0) return false;
        got += n;
    }
    for (int i = 0; i < SPECTRUM_FFT_SIZE; i++) {
        /* MIC1 left, MIC2 right; the sketch asked M5Unified for mono. */
        const float mono = ((float)s_frames[2 * i] + (float)s_frames[2 * i + 1]) * 0.5f;
        s_fft[2 * i]     = mono * s_window[i];
        s_fft[2 * i + 1] = 0.0f;
    }
    return true;
}

static void fft(void)
{
    dsps_fft2r_fc32(s_fft, SPECTRUM_FFT_SIZE);
    dsps_bit_rev_fc32(s_fft, SPECTRUM_FFT_SIZE);
    for (int i = 0; i < SPECTRUM_FFT_SIZE / 2; i++) {
        const float re = s_fft[2 * i], im = s_fft[2 * i + 1];
        s_mag[i] = sqrtf(re * re + im * im);
    }
}

/* One frame of bars. Returns the dirty row range through *y0, *y1
 * (y1 exclusive; y0 == y1 when nothing changed). */
static void render(float dt, int *y0, int *y1)
{
    int dirty_top = s_bottom, dirty_bot = s_top - 1;

    for (int b = 0; b < SPECTRUM_BARS; b++) {
        const float mag = spectrum_band_mag(&s_band[b], s_mag);
        spectrum_ballistics(spectrum_frac(spectrum_dbfs(mag)), dt, &s_level[b], &s_peak[b]);

        const int new_h     = (int)(s_level[b] * (float)s_graph_h);
        const int bar_top_y = s_bottom - new_h;
        int peak_y = s_bottom - (int)(s_peak[b] * (float)s_graph_h);
        if (peak_y < s_top)        peak_y = s_top;
        if (peak_y > s_bottom - 1) peak_y = s_bottom - 1;

        const int old_top_y  = s_bottom - s_prev_h[b];
        const int old_peak_y = s_prev_peak_y[b];

        /* Only rows between the old state and the new can have changed. */
        int y_top = bar_top_y < old_top_y ? bar_top_y : old_top_y;
        if (peak_y < y_top)     y_top = peak_y;
        if (old_peak_y < y_top) y_top = old_peak_y;
        int y_bot = bar_top_y > old_top_y ? bar_top_y : old_top_y;
        if (peak_y > y_bot)     y_bot = peak_y;
        if (old_peak_y > y_bot) y_bot = old_peak_y;

        if (y_top <= y_bot) {
            repaint_rows(s_left + b * s_bar_w + 1, s_bar_w - 2, y_top, y_bot, bar_top_y, peak_y);
            if (y_top < dirty_top) dirty_top = y_top;
            if (y_bot > dirty_bot) dirty_bot = y_bot;
        }
        s_prev_h[b]      = new_h;
        s_prev_peak_y[b] = peak_y;
    }

    if (dirty_top < s_top)        dirty_top = s_top;
    if (dirty_bot > s_bottom - 1) dirty_bot = s_bottom - 1;
    *y0 = dirty_top;
    *y1 = dirty_bot >= dirty_top ? dirty_bot + 1 : dirty_top;
}

static void draw_fps(float fps)
{
    const int x = s_w - 170, y = 14, h = GFX_GLYPH_H(LABEL_SCALE);
    char buf[16];
    snprintf(buf, sizeof(buf), "%5.1f fps", fps);
    gfx_fill_rect(x, y, s_w - x, h, COL_BLACK);
    gfx_draw_text(x, y, buf, LABEL_SCALE, s_w - x, COL_WHITE);
    gfx_blit(y, y + h);
}

void app_main(void)
{
    /* The bus and the expanders first: LCD_RST and the codecs are on them. */
    ESP_ERROR_CHECK(tab5io_init());

    esp_lcd_panel_handle_t panel;
    ESP_ERROR_CHECK(lcd_init(&panel));
    ESP_ERROR_CHECK(gfx_init(panel, LCD_H_RES, LCD_V_RES));
    gfx_set_rotation(ANALYZER_ROTATION);

    /* Capture borrows playback's clocks (audio_out.h), so playback comes
     * up too, at the capture rate, and plays nothing. */
    ESP_ERROR_CHECK(audio_out_init(tab5io_bus(), tab5io_exp1(), SPECTRUM_SAMPLE_RATE));
    ESP_ERROR_CHECK(audio_out_capture_begin(AUDIO_CAPTURE_BUILTIN));

    ESP_ERROR_CHECK(dsps_fft2r_init_fc32(NULL, SPECTRUM_FFT_SIZE));
    for (int i = 0; i < SPECTRUM_FFT_SIZE; i++)
        s_window[i] = spectrum_hamming(i, SPECTRUM_FFT_SIZE);

    s_w = gfx_w();
    s_h = gfx_h();
    s_left   = 100;             /* room for the dB labels */
    s_right  = s_w - 10;
    s_top    = 70;              /* clear of the title */
    s_bottom = s_h - 46;        /* room for the frequency labels */
    s_graph_w = s_right - s_left;
    s_graph_h = s_bottom - s_top;
    if (s_graph_h > MAX_GRAPH_H) s_graph_h = MAX_GRAPH_H;
    s_bottom = s_top + s_graph_h;
    s_bar_w = s_graph_w / SPECTRUM_BARS;

    for (int b = 0; b < SPECTRUM_BARS; b++) {
        s_band[b] = spectrum_band(b);
        s_prev_peak_y[b] = s_bottom;
    }

    draw_static();
    ESP_ERROR_CHECK(lcd_backlight_set(ANALYZER_BACKLIGHT));
    ESP_LOGI(TAG, "%dx%d, graph %dx%d, %d bars of %d px", s_w, s_h,
             s_graph_w, s_graph_h, SPECTRUM_BARS, s_bar_w);

    int64_t last_us = esp_timer_get_time();
    int64_t report_us = last_us;
    uint32_t frames = 0;
    int64_t t_capture = 0, t_fft = 0, t_draw = 0;

    for (;;) {
        const int64_t t0 = esp_timer_get_time();
        if (!capture()) {
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }
        const int64_t t1 = esp_timer_get_time();
        fft();
        const int64_t t2 = esp_timer_get_time();

        /* Frame delta for the ballistics, bounded against stalls. */
        const int64_t now = esp_timer_get_time();
        const float dt = spectrum_clampf((float)(now - last_us) * 1e-6f, 0.0005f, 0.25f);
        last_us = now;

        int y0, y1;
        render(dt, &y0, &y1);
        if (y1 > y0) gfx_blit(y0, y1);
        const int64_t t3 = esp_timer_get_time();

        t_capture += t1 - t0;
        t_fft     += t2 - t1;
        t_draw    += t3 - t2;
        frames++;

        if (t3 - report_us >= 1000000) {
            const float fps = (float)frames * 1e6f / (float)(t3 - report_us);
            ESP_LOGI(TAG, "FPS: %.1f | capture %.1fms  fft %.1fms  draw %.1fms",
                     fps, t_capture / 1000.0f / frames, t_fft / 1000.0f / frames,
                     t_draw / 1000.0f / frames);
            draw_fps(fps);
            frames = 0;
            report_us = t3;
            t_capture = t_fft = t_draw = 0;
        }

        if (FRAME_LIMIT_HZ > 0) {
            const int64_t target = 1000000 / (FRAME_LIMIT_HZ > 0 ? FRAME_LIMIT_HZ : 1);
            const int64_t spent = esp_timer_get_time() - now;
            if (spent < target) vTaskDelay(pdMS_TO_TICKS((target - spent) / 1000));
        }
    }
}
