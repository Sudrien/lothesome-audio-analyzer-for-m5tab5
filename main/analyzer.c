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
 *                        rows that changed; text in Ark12 rather than
 *                        M5GFX's 6x8 font. 0006: in landscape drawn by
 *                        this file onto gfx's portrait rows (landmap.h)
 *   Serial.printf        ESP_LOGI
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_dsp.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "audio_out.h"
#include "gfx.h"
#include "lcd.h"
#include "tab5io.h"
#include "ark12.h"        /* 0006: glyphs, drawn turned */

#include "landmap.h"      /* 0006 */
#include "monoring.h"     /* 0003 */
#include "spectrum.h"

static const char *TAG = "analyzer";

/*
 * The sketch used setRotation(3): landscape, the same way up as the
 * printing on the back of the case. 0002 got that from GFX_ROT_270,
 * confirmed acceptable on the board; 0006 keeps gfx at GFX_ROT_0 and
 * draws the same landscape picture itself, through landmap.h, so every
 * blit is a straight copy of panel rows. LAND_W x LAND_H is the
 * landscape the layout below is written in.
 */
#define LAND_W              (LCD_V_RES)     /* 1280 */
#define LAND_H              (LCD_H_RES)     /* 720 */

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

/*
 * 0003: the capture task's side. It reads CAPTURE_CHUNK stereo frames at
 * a time -- one DMA buffer's worth (feckless-drivers' CAPTURE_DMA_FRAMES)
 * -- averages them to mono and appends them to s_ring. See monoring.h
 * for why this is a task of its own.
 */
#define CAPTURE_CHUNK       (120)
#define CAPTURE_STACK       (4096)
#define CAPTURE_PRIO        (5)     /* above app_main's 1: it must never wait on a draw */
#define CAPTURE_CORE        (1)     /* app_main runs on 0 */

/* Fresh samples a frame waits for before redrawing, so a fast draw does
 * not repaint the same audio. 256 is 5.3 ms at 48 kHz. */
#define FRAME_HOP           (256)

static int32_t    s_frames[CAPTURE_CHUNK * 2];              /* capture, stereo */
static float      s_chunk[CAPTURE_CHUNK];                   /* the same, mono */
static monoring_t s_ring;
static float      s_mono[SPECTRUM_FFT_SIZE];                /* the window, oldest first */
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

/* ---- 0006: drawing in landscape on a portrait buffer ----
 * Everything below takes landscape coordinates and lands it where
 * landmap.h says. gfx's own text calls draw upright on the panel, so
 * text is drawn here from the Ark12 glyphs, turned. */

static void land_fill(int x, int y, int w, int h, uint16_t c)
{
    const landmap_rect_t r = landmap_rect(x, y, w, h, LAND_W);
    gfx_fill_rect(r.x, r.y, r.w, r.h, c);
}

/* A horizontal landscape run is a panel column; one pixel wide. */
static inline void hline(int x, int y, int w, uint16_t c)
{
    land_fill(x, y, w, 1, c);
}

/* ASCII only, which is all this program writes. A glyph the font does
 * not have is skipped, advancing by a narrow cell. */
static int land_text_w(const char *s, int scale)
{
    int w = 0;
    for (; *s; s++) {
        int gw = ARK12_HALF_W;
        uint16_t rows[ARK12_H];
        if (!ark12_glyph((unsigned char)*s, &gw, rows)) gw = ARK12_HALF_W;
        w += (gw + 1) * scale;
    }
    return w;
}

static void land_text(int x, int y, const char *s, int scale, uint16_t c)
{
    for (; *s; s++) {
        int gw = ARK12_HALF_W;
        uint16_t rows[ARK12_H];
        if (ark12_glyph((unsigned char)*s, &gw, rows)) {
            for (int row = 0; row < ARK12_H; row++)
                for (int col = 0; col < gw; col++)
                    if (rows[row] & (1u << col))
                        land_fill(x + col * scale, y + row * scale, scale, scale, c);
        } else {
            gw = ARK12_HALF_W;
        }
        x += (gw + 1) * scale;
    }
}

/* Send landscape columns [x0, x1) to the glass: panel rows, contiguous. */
static void land_blit(int x0, int x1)
{
    int y0, y1;
    landmap_rows(x0, x1, LAND_W, &y0, &y1);
    gfx_blit(y0, y1);
}

/* One landscape row's colours for one bar, top to bottom: the panel
 * columns of that bar's rows. MAX_GRAPH_H bounds it. */
static uint16_t s_line[LAND_H];

static void draw_static(void)
{
    land_fill(0, 0, s_w, s_h, COL_BLACK);
    land_text(10, 4, "Tab5 Spectrum Analyzer", TITLE_SCALE, COL_WHITE);

    for (int h = 0; h <= s_graph_h; h++) {
        int r, g;
        spectrum_row_rg((float)h / (float)s_graph_h, &r, &g);
        s_row_colour[h] = RGB(r, g, 0);
        s_row_grid[h] = false;
    }

    /* dBFS ticks every 10 dB from the floor up to 0, labelled on the left. */
    const int label_h = ARK12_H * LABEL_SCALE;
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
        const int tw = land_text_w(buf, LABEL_SCALE);
        land_text(s_left - tw - 8, y - label_h / 2, buf, LABEL_SCALE, COL_LABEL);
    }
    land_text(10, s_top - label_h - 4, "dBFS", LABEL_SCALE, COL_LABEL);

    /* Frequency labels along the log axis. */
    static const float label_hz[] = { 80, 100, 500, 1000, 5000, 10000, 20000 };
    for (size_t i = 0; i < sizeof(label_hz) / sizeof(label_hz[0]); i++) {
        const float f = label_hz[i];
        if (f < SPECTRUM_LOG_MIN_HZ || f > SPECTRUM_LOG_MAX_HZ) continue;
        char buf[8];
        freq_label(f, buf, sizeof(buf));
        int x = spectrum_freq_x(f, s_graph_w);
        if (x > s_graph_w - 60) x = s_graph_w - 60;
        if (x < 0) x = 0;
        land_text(s_left + x, s_bottom + 10, buf, LABEL_SCALE, COL_LABEL);
    }

    hline(s_left, s_bottom, s_graph_w, COL_BASE);
    gfx_blit(0, gfx_h());
}

/* Repaint rows [y_top..y_bot] of one bar's column with what belongs
 * there now. Called only on rows that changed since the last frame.
 *
 * 0006: in panel terms the bar is w panel rows, and its landscape rows
 * y_top..y_bot are one contiguous span of each of them -- so the colours
 * are worked out once into s_line and copied w times. */
static void repaint_rows(int xs, int w, int y_top, int y_bot, int bar_top_y, int peak_y)
{
    if (y_top < s_top)        y_top = s_top;
    if (y_bot > s_bottom - 1) y_bot = s_bottom - 1;
    if (y_top > y_bot) return;
    for (int y = y_top; y <= y_bot; y++) {
        uint16_t c;
        if (y == peak_y)                        c = COL_PEAK;
        else if (y >= bar_top_y)                c = s_row_colour[s_bottom - y];
        else if (s_row_grid[s_bottom - y])      c = COL_GRID;
        else                                    c = COL_BLACK;
        s_line[y] = c;
    }
    const landmap_rect_t r = landmap_rect(xs, y_top, w, y_bot - y_top + 1, LAND_W);
    uint16_t *fb = gfx_fb();
    const int stride = gfx_w();
    for (int py = r.y; py < r.y + r.h; py++)
        memcpy(fb + (size_t)py * (size_t)stride + (size_t)r.x, &s_line[y_top],
               (size_t)r.w * sizeof(uint16_t));
}

/* 0003: reads the microphones for as long as the program runs. */
static void capture_task(void *arg)
{
    (void)arg;
    for (;;) {
        const size_t n = audio_out_capture_read(s_frames, CAPTURE_CHUNK, 100);
        for (size_t i = 0; i < n; i++) {
            /* MIC1 left, MIC2 right; the sketch asked M5Unified for mono. */
            s_chunk[i] = ((float)s_frames[2 * i] + (float)s_frames[2 * i + 1]) * 0.5f;
        }
        if (n) monoring_write(&s_ring, s_chunk, n);
        else   vTaskDelay(pdMS_TO_TICKS(2));
    }
}

/* Fill s_fft with the newest window of mono samples, once at least
 * FRAME_HOP have arrived since the last one. Returns false if none have
 * for a while, which is a capture that stopped delivering. */
static bool capture(void)
{
    static uint32_t s_last;
    uint32_t end = monoring_count(&s_ring);
    for (int waited = 0; end < SPECTRUM_FFT_SIZE || end - s_last < FRAME_HOP; waited++) {
        if (waited > 100) return false;     /* ~100 ms with nothing new */
        vTaskDelay(pdMS_TO_TICKS(1));
        end = monoring_count(&s_ring);
    }
    s_last = end;
    monoring_newest(&s_ring, end, s_mono, SPECTRUM_FFT_SIZE);
    for (int i = 0; i < SPECTRUM_FFT_SIZE; i++) {
        s_fft[2 * i]     = s_mono[i] * s_window[i];
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

/* One frame of bars. Returns the dirty landscape COLUMN range through
 * *x0, *x1 (x1 exclusive; x0 == x1 when nothing changed) -- 0006: the
 * columns, because columns are what map to contiguous panel rows. */
static void render(float dt, int *x0, int *x1)
{
    int dirty_l = s_w, dirty_r = 0;

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
            const int xs = s_left + b * s_bar_w + 1, w = s_bar_w - 2;
            repaint_rows(xs, w, y_top, y_bot, bar_top_y, peak_y);
            if (xs < dirty_l)     dirty_l = xs;
            if (xs + w > dirty_r) dirty_r = xs + w;
        }
        s_prev_h[b]      = new_h;
        s_prev_peak_y[b] = peak_y;
    }

    *x0 = dirty_l;
    *x1 = dirty_r > dirty_l ? dirty_r : dirty_l;
}

static void draw_fps(float fps)
{
    const int x = s_w - 170, y = 14, h = ARK12_H * LABEL_SCALE;
    char buf[16];
    snprintf(buf, sizeof(buf), "%5.1f fps", fps);
    land_fill(x, y, s_w - x, h, COL_BLACK);
    land_text(x, y, buf, LABEL_SCALE, COL_WHITE);
    land_blit(x, s_w);
}

void app_main(void)
{
    /*
     * 0005: the line to start copying from, as the player has it.
     * Everything above is the ROM, the bootloader and IDF's startup;
     * everything below is this program. The version is here because a
     * log that cannot be matched to a tree cannot be decoded against it.
     * ESP_LOGW so it survives a build with the info level turned down,
     * and first, so a failure in the very first init is still below it.
     */
    {
        const esp_app_desc_t *d = esp_app_get_description();
        ESP_LOGW(TAG, "=== Lothesome Audio Analyzer === %s, IDF %s, built %s %s",
                 d ? d->version : "?", d ? d->idf_ver : "?",
                 d ? d->date : "?", d ? d->time : "?");
    }

    /* The bus and the expanders first: LCD_RST and the codecs are on them. */
    ESP_ERROR_CHECK(tab5io_init());

    esp_lcd_panel_handle_t panel;
    ESP_ERROR_CHECK(lcd_init(&panel));
    ESP_ERROR_CHECK(gfx_init(panel, LCD_H_RES, LCD_V_RES));     /* GFX_ROT_0: see LAND_W */

    /* Capture borrows playback's clocks (audio_out.h), so playback comes
     * up too, at the capture rate, and plays nothing. */
    ESP_ERROR_CHECK(audio_out_init(tab5io_bus(), tab5io_exp1(), SPECTRUM_SAMPLE_RATE));
    ESP_ERROR_CHECK(audio_out_capture_begin(AUDIO_CAPTURE_BUILTIN));
    if (xTaskCreatePinnedToCore(capture_task, "capture", CAPTURE_STACK, NULL,
                                CAPTURE_PRIO, NULL, CAPTURE_CORE) != pdPASS) {
        ESP_LOGE(TAG, "no capture task");
        return;
    }

    ESP_ERROR_CHECK(dsps_fft2r_init_fc32(NULL, SPECTRUM_FFT_SIZE));
    for (int i = 0; i < SPECTRUM_FFT_SIZE; i++)
        s_window[i] = spectrum_hamming(i, SPECTRUM_FFT_SIZE);

    s_w = LAND_W;
    s_h = LAND_H;
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

        int x0, x1;
        render(dt, &x0, &x1);
        if (x1 > x0) land_blit(x0, x1);
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
