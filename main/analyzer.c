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
#include "touch.h"        /* 0014 */
#include "uac.h"          /* 0008 */
#include "usbhost.h"      /* 0008 */
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
#define COL_DEAF            RGB(28, 28, 28)  /* 0013: below the source's floor */
#define DEAF_DBFS           (-80.0f)         /* 0020: the outline's top */
#define COL_CLIP            RGB(255, 40, 40) /* 0014 */

/*
 * 0015: the waterfall mode. The bars shrink to a strip and the last ten
 * seconds scroll down under them, newest at the top, on the same
 * frequency axis, so a line in one is a line in the other.
 *
 * Scrolling is a memmove per column. A landscape column is a panel row
 * (landmap.h), and the waterfall's rows are a contiguous run of it, so
 * moving the picture down one row is one short move along each panel
 * row: 1090 of 844 bytes. A ring in the shadow would save nothing,
 * because gfx_blit() sends whole panel rows from a buffer laid out as
 * the screen is: the ring would have to be put back in order every
 * frame, which is the same copy. And the bars already dirty nearly
 * every column a frame, so the waterfall adds no blit.
 */
#define WF_ROWS_PER_SEC     (40.0f)  /* time down the waterfall */
#define STRIP_H             (140)    /* the bars, in waterfall mode */
#define WF_GAP              (8)
#define KEY_W               (16)     /* the colour key, right of the graph */
#define MODE_COUNT          (3)      /* 0016: BARS, 1/3 OCT, FALL */
#define MODE_W              (112)    /* each button: "1/3 OCT" is 98 px of Ark12 */
#define MODE_X              (LAND_W - 10 - MODE_COUNT * MODE_W)
#define MODE_Y              (6)
#define MODE_H              (36)

/* 0016: the 1/3-octave view's Fast/Slow switch, under the mode buttons. */
#define WEIGHT_X            (MODE_X)
#define WEIGHT_Y            (74)
#define WEIGHT_W            (MODE_W)
#define WEIGHT_H            (26)

/*
 * 0021: where a press counts, which is more than what is drawn. On the
 * board fingers landed 1 to 80 px BELOW the buttons they were aimed at,
 * and a 36 px strip caught few of them. Nothing else on the screen takes
 * a tap, so the zones reach down into the empty band above the graph
 * and, for FAST/SLOW, into the top of the graph itself, where a press
 * otherwise only counts toward the meter's hold-to-reset.
 */
#define MODE_HIT_X0         (MODE_X - 10)
#define MODE_HIT_Y1         (66)     /* MODE_Y + MODE_H is 42 */
#define WEIGHT_HIT_Y0       (MODE_HIT_Y1)
#define WEIGHT_HIT_Y1       (150)    /* WEIGHT_Y + WEIGHT_H is 100 */
#define OCT_GAP             (6)      /* between bands */
#define COL_PARTIAL         ((uint16_t)0x7BEF)   /* a band cut by Nyquist */

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

/* 0013: the bass path (spectrum.h). The capture task decimates into
 * s_bass_ring; each frame takes its newest window into a second FFT. */
static float            s_bass_taps[SPECTRUM_BASS_TAPS];
static spectrum_decim_t s_decim;
static float            s_bass_chunk[CAPTURE_CHUNK / SPECTRUM_BASS_DECIM + 1];
static monoring_t       s_bass_ring;
static float            s_bass_fft[SPECTRUM_FFT_SIZE * 2] __attribute__((aligned(16)));
static float            s_bass_mag[SPECTRUM_FFT_SIZE / 2];
static bool             s_bass_ready;    /* a whole bass window has arrived */

/*
 * 0014: the sound level meter (spectrum.h), run by the capture task on
 * every sample. It publishes its readings after each chunk; each is one
 * aligned 32-bit store, and a reader that mixes two chunks' worth sees
 * readings 2.5 ms apart, which no display can show. The drawing loop
 * asks for a reset through s_meter_reset_req and the capture task, the
 * meter's only writer, does it.
 */
static spectrum_meter_t s_meter;
static volatile float   s_pub_laeq = -180.0f, s_pub_lafmax = -180.0f, s_pub_lcpeak = -180.0f;
static volatile float   s_pub_secs;
static volatile bool    s_pub_clip;
static volatile bool    s_meter_reset_req;

#define METER_DRAW_US       (250000)    /* the readout's refresh */
#define RESET_HOLD_US       (500000)    /* a press this long resets: not a tap,
                                         * which a stray contact can make
                                         * (feckless-drivers' touch.c, 0503) */
#define TOUCH_POLL_US       (50000)

/* ---- layout ---- */
static int s_w, s_h;
static int s_left, s_right, s_top, s_bottom, s_graph_w, s_graph_h;
static int s_axis_y;                 /* 0015: where the frequency labels hang */

/* 0016: in the buttons' order. */
typedef enum { MODE_SPECTRUM, MODE_OCT, MODE_WATERFALL } view_mode_t;
static view_mode_t s_mode = MODE_SPECTRUM;      /* 0015 */
static int    s_wf_top, s_wf_h;
static float  s_wf_frac[LAND_W];           /* this frame's level per column, unsmoothed */
static float  s_wf_acc;
static uint16_t s_heat[256];               /* spectrum_heat_rgb(), as 565 */

/* 0016: the 1/3-octave view. Its bands reuse s_level/s_peak/s_prev_*
 * by index; s_oct_e is each band's time-weighted mean square. */
static spectrum_oct_t s_oct[SPECTRUM_OCT_BANDS];
static float s_oct_e[SPECTRUM_OCT_BANDS];
static bool  s_oct_deaf[SPECTRUM_OCT_BANDS];
static bool  s_oct_slow;                   /* Fast by default */
static int   s_oct_slot;                   /* px a band, gap included */
static float s_max_hz = SPECTRUM_LOG_MAX_HZ;    /* 0008: the source's Nyquist */
static int   s_first_col;    /* 0013: the first column the source can hear */

/* The colour of each row, by height above the baseline, and whether a
 * dB gridline sits there. By row, never by level: see spectrum_row_rg(). */
static uint16_t s_row_colour[MAX_GRAPH_H + 1];
static bool     s_row_grid[MAX_GRAPH_H + 1];

/* ---- per bar ----
 * 0012: a bar is one pixel column of the graph, s_graph_w of them, so
 * the axis shows every bin the FFT has rather than 64 averages of them.
 * LAND_W bounds the count. */
static spectrum_band_t s_band[LAND_W];
static float s_level[LAND_W];    /* smoothed 0..1, what is drawn */
static float s_peak[LAND_W];     /* peak hold 0..1 */
static int   s_prev_h[LAND_W];   /* last bar height, px */
static int   s_prev_peak_y[LAND_W];

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

/* 0020: a one-pixel rectangle's edges, landscape coordinates. */
static void land_outline(int x, int y, int w, int h, uint16_t c)
{
    land_fill(x, y, w, 1, c);
    land_fill(x, y + h - 1, w, 1, c);
    land_fill(x, y, 1, h, c);
    land_fill(x + w - 1, y, 1, h, c);
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

/* 0015: the graph's frame for the mode. In waterfall mode the bars are
 * STRIP_H tall and the graph stops short of the right edge, for the key. */
static void set_geometry(void)
{
    s_left = 100;               /* room for the dB labels */
    s_top  = 104;               /* clear of the title and, 0014, the meter */
    s_axis_y = s_h - 46;        /* room for the frequency labels */
    if (s_mode == MODE_WATERFALL) {
        s_right  = s_w - 90;
        s_bottom = s_top + STRIP_H;
        s_wf_top = s_bottom + WF_GAP;
        s_wf_h   = s_axis_y - s_wf_top;
    } else {
        s_right  = s_w - 10;
        s_bottom = s_axis_y;
        s_wf_top = s_wf_h = 0;
    }
    s_graph_w = s_right - s_left;
    s_graph_h = s_bottom - s_top;
    if (s_graph_h > MAX_GRAPH_H) s_graph_h = MAX_GRAPH_H;
    s_bottom = s_top + s_graph_h;
    s_wf_acc = 0.0f;
}

/* 0015: the two mode buttons, top right; the chosen one lit. */
static void draw_mode_buttons(void)
{
    static const char *const names[MODE_COUNT] = { "BARS", "1/3 OCT", "FALL" };   /* 0016 */
    const int label_h = ARK12_H * LABEL_SCALE;
    land_fill(MODE_X - 1, MODE_Y - 1, MODE_COUNT * MODE_W + 2, MODE_H + 2, COL_BASE);
    for (int i = 0; i < MODE_COUNT; i++) {
        const bool on = (int)s_mode == i;
        const int x = MODE_X + i * MODE_W;
        land_fill(x, MODE_Y, MODE_W, MODE_H, on ? COL_LABEL : COL_BLACK);
        const int tw = land_text_w(names[i], LABEL_SCALE);
        land_text(x + (MODE_W - tw) / 2, MODE_Y + (MODE_H - label_h) / 2, names[i],
                  LABEL_SCALE, on ? COL_BLACK : COL_LABEL);
    }
}

/* 0016: the Fast/Slow switch, shown only in the 1/3-octave view. */
static void draw_weight_button(void)
{
    const char *t = s_oct_slow ? "SLOW" : "FAST";
    const int label_h = ARK12_H * LABEL_SCALE, tw = land_text_w(t, LABEL_SCALE);
    land_fill(WEIGHT_X - 1, WEIGHT_Y - 1, WEIGHT_W + 2, WEIGHT_H + 2, COL_BASE);
    land_fill(WEIGHT_X, WEIGHT_Y, WEIGHT_W, WEIGHT_H, COL_BLACK);
    land_text(WEIGHT_X + (WEIGHT_W - tw) / 2, WEIGHT_Y + (WEIGHT_H - label_h) / 2, t,
              LABEL_SCALE, COL_LABEL);
}

/* 0015: the waterfall's time labels and its colour key. */
static void draw_waterfall_static(void)
{
    const int label_h = ARK12_H * LABEL_SCALE;
    for (int sec = 0; ; sec += 2) {
        const int y = s_wf_top + (int)((float)sec * WF_ROWS_PER_SEC);
        if (y > s_wf_top + s_wf_h - label_h) break;
        char buf[16];    /* 0017: room for any int, as -Wformat-truncation wants */
        if (sec == 0) snprintf(buf, sizeof(buf), "now");
        else          snprintf(buf, sizeof(buf), "-%d s", sec);
        const int tw = land_text_w(buf, LABEL_SCALE);
        land_text(s_left - tw - 8, sec == 0 ? y : y - label_h / 2, buf, LABEL_SCALE, COL_LABEL);
    }
    const int kx = s_right + 14;
    for (int y = 0; y < s_wf_h; y++) {
        const int i = 255 - (y * 255) / (s_wf_h - 1);
        land_fill(kx, s_wf_top + y, KEY_W, 1, s_heat[i]);
    }
    for (int db = 0; db >= (int)SPECTRUM_MIN_DBFS; db -= 30) {
        const float frac = ((float)db - SPECTRUM_MIN_DBFS) / (SPECTRUM_MAX_DBFS - SPECTRUM_MIN_DBFS);
        int y = s_wf_top + (int)((1.0f - frac) * (float)(s_wf_h - 1)) - label_h / 2;
        if (y < s_wf_top) y = s_wf_top;
        if (y > s_wf_top + s_wf_h - label_h) y = s_wf_top + s_wf_h - label_h;
        char buf[8];
        snprintf(buf, sizeof(buf), "%d", db);
        land_text(kx + KEY_W + 6, y, buf, LABEL_SCALE, COL_LABEL);
    }
    hline(s_left, s_wf_top + s_wf_h, s_graph_w, COL_BASE);
}

static void draw_static(void)
{
    land_fill(0, 0, s_w, s_h, COL_BLACK);
    land_text(10, 4, "Lothesome Audio Analyzer", TITLE_SCALE, COL_WHITE);

    for (int h = 0; h <= s_graph_h; h++) {
        int r, g;
        spectrum_row_rg((float)h / (float)s_graph_h, &r, &g);
        s_row_colour[h] = RGB(r, g, 0);
        s_row_grid[h] = false;
    }

    /* dBFS ticks every 10 dB from the floor up to 0, labelled on the left.
     * 0015: every 30 on the waterfall's short strip, where 10 would crowd. */
    const int label_h = ARK12_H * LABEL_SCALE;
    const int step = s_graph_h < 300 ? 30 : 10;
    int first = ((int)SPECTRUM_MIN_DBFS / step) * step;
    if (first < SPECTRUM_MIN_DBFS) first += step;
    for (int db = first; db <= (int)SPECTRUM_MAX_DBFS; db += step) {
        const float frac = ((float)db - SPECTRUM_MIN_DBFS) /
                           (SPECTRUM_MAX_DBFS - SPECTRUM_MIN_DBFS);
        const int h = (int)(frac * (float)s_graph_h);
        if (h < 0 || h > s_graph_h) continue;
        s_row_grid[h] = true;
        const int y = s_bottom - h;
        hline(s_left, y, s_graph_w, COL_GRID);

        char buf[16];    /* 0017: the step is no longer a constant (0016) */
        snprintf(buf, sizeof(buf), "%d", db);
        const int tw = land_text_w(buf, LABEL_SCALE);
        land_text(s_left - tw - 8, y - label_h / 2, buf, LABEL_SCALE, COL_LABEL);
    }
    land_text(10, s_top - label_h - 4, "dBFS", LABEL_SCALE, COL_LABEL);

    /* 0016: the 1/3-octave view labels its octave centres, under the
     * bands, which are evenly spaced slots rather than the log axis. */
    if (s_mode == MODE_OCT) {
        static const char *const oct_label[SPECTRUM_OCT_BANDS] = {
            [2] = "31.5", [5] = "63", [8] = "125", [11] = "250", [14] = "500",
            [17] = "1k", [20] = "2k", [23] = "4k", [26] = "8k", [29] = "16k",
        };
        for (int b = 0; b < SPECTRUM_OCT_BANDS; b++) {
            if (!oct_label[b]) continue;
            const int tw = land_text_w(oct_label[b], LABEL_SCALE);
            land_text(s_left + b * s_oct_slot + s_oct_slot / 2 - tw / 2, s_axis_y + 10,
                      oct_label[b], LABEL_SCALE, COL_LABEL);
        }
        hline(s_left, s_bottom, s_graph_w, COL_BASE);
        draw_weight_button();
        draw_mode_buttons();
        gfx_blit(0, gfx_h());
        return;
    }

    /* Frequency labels along the log axis. */
    static const float label_hz[] = { 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000 };
    for (size_t i = 0; i < sizeof(label_hz) / sizeof(label_hz[0]); i++) {
        const float f = label_hz[i];
        if (f < SPECTRUM_LOG_MIN_HZ || f > s_max_hz) continue;
        char buf[8];
        freq_label(f, buf, sizeof(buf));
        int x = spectrum_freq_x_at(f, s_graph_w, s_max_hz);
        if (x > s_graph_w - 60) x = s_graph_w - 60;
        if (x < 0) x = 0;
        land_text(s_left + x, s_axis_y + 10, buf, LABEL_SCALE, COL_LABEL);
    }

    hline(s_left, s_bottom, s_graph_w, COL_BASE);
    if (s_mode == MODE_WATERFALL) draw_waterfall_static();
    draw_mode_buttons();
    gfx_blit(0, gfx_h());
}

/* Repaint rows [y_top..y_bot] of one bar's column with what belongs
 * there now. Called only on rows that changed since the last frame.
 *
 * 0006: in panel terms the bar is w panel rows, and its landscape rows
 * y_top..y_bot are one contiguous span of each of them -- so the colours
 * are worked out once into s_line and copied w times. */
static void repaint_rows(int xs, int w, int y_top, int y_bot, int bar_top_y, int peak_y,
                         bool partial)
{
    if (y_top < s_top)        y_top = s_top;
    if (y_bot > s_bottom - 1) y_bot = s_bottom - 1;
    if (y_top > y_bot) return;
    for (int y = y_top; y <= y_bot; y++) {
        uint16_t c;
        if (y == peak_y)                        c = COL_PEAK;
        else if (y >= bar_top_y)                c = partial ? COL_PARTIAL       /* 0016 */
                                                            : s_row_colour[s_bottom - y];
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

/*
 * 0007: where the sound comes from. The capture task owns the choice and
 * publishes it for the drawing loop, which names it on screen.
 *
 *   SRC_BUILTIN  the two array microphones, stereo, 24-bit scale
 *   SRC_JACK     a headset's microphone on the 3.5 mm jack: the ES7210's
 *                fourth channel, mono, 16-bit values (audio_out.h)
 *
 * The jack is chosen whenever audio_out_headphones() says something is
 * plugged in. That cannot tell a headset from plain headphones -- the
 * jack detects a plug, not a microphone -- so plain headphones give an
 * empty spectrum labelled "headset jack", which is the honest answer.
 */
typedef enum { SRC_BUILTIN, SRC_JACK, SRC_USB, SRC_COUNT } source_t;

static const char *const k_source_name[SRC_COUNT] = {
    [SRC_BUILTIN] = "built-in mics",
    [SRC_JACK]    = "headset jack",
    [SRC_USB]     = "USB",
};

/*
 * 0008: a USB microphone -- a UAC headset or a USB mic on the USB-A port
 * -- is preferred over both of the above whenever one is announced. It
 * runs at its own rate (uac.h: 48 kHz if offered, else 44.1, else its
 * highest), which the drawing loop rebuilds the bars for. s_rate and
 * s_usb_label are written by the capture task BEFORE s_source, so a
 * reader that sees the new source sees them too.
 */
static volatile uint32_t s_rate = SPECTRUM_SAMPLE_RATE;
static char s_usb_label[48];
static uint8_t s_usb_channels;
static uint32_t s_usb_refused_gen = UINT32_MAX;   /* uac_generation() at a refusal */

#define SOURCE_POLL_US      (250000)    /* how often a plug is looked for */

static volatile int s_source = SRC_BUILTIN;

/* Switch the ES7210's capture between the array and the jack. On a
 * failure, back to the array: a capture that is running beats one that
 * is not. 0008: also the way back from USB, when nothing is capturing
 * on the ES7210 and the end is a no-op. */
static source_t switch_source(source_t from, source_t to)
{
    audio_out_capture_end();
    const audio_capture_src_t want = (to == SRC_JACK) ? AUDIO_CAPTURE_HEADSET
                                                      : AUDIO_CAPTURE_BUILTIN;
    esp_err_t err = audio_out_capture_begin(want);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "source: %s", k_source_name[to]);
        return to;
    }
    ESP_LOGW(TAG, "source %s refused (%s); built-in mics", k_source_name[to],
             esp_err_to_name(err));
    if (audio_out_capture_begin(AUDIO_CAPTURE_BUILTIN) != ESP_OK)
        ESP_LOGE(TAG, "built-in mics refused too; nothing to show");
    (void)from;
    return SRC_BUILTIN;
}

/* 0013: one chunk of mono to the main ring and, decimated, the bass one. */
static void feed(const float *mono, size_t n)
{
    monoring_write(&s_ring, mono, n);
    const int m = spectrum_decim(&s_decim, s_bass_taps, mono, (int)n, s_bass_chunk);
    if (m) monoring_write(&s_bass_ring, s_bass_chunk, (size_t)m);

    if (s_meter_reset_req) {                    /* 0014 */
        spectrum_meter_reset(&s_meter);
        s_meter_reset_req = false;
    }
    spectrum_meter_push(&s_meter, mono, (int)n);
    s_pub_laeq   = spectrum_laeq(&s_meter);
    s_pub_lafmax = spectrum_lafmax(&s_meter);
    s_pub_lcpeak = spectrum_lcpeak(&s_meter);
    s_pub_secs   = (float)((double)s_meter.n / s_meter.rate);
    s_pub_clip   = s_meter.clipped;
}

/* 0003: reads the microphones for as long as the program runs. */
static void capture_task(void *arg)
{
    (void)arg;
    source_t cur = SRC_BUILTIN;         /* app_main() began the array */
    int64_t next_poll = 0;
    for (;;) {
        const int64_t now = esp_timer_get_time();
        if (now >= next_poll) {
            next_poll = now + SOURCE_POLL_US;
            const source_t es7210 = audio_out_headphones() ? SRC_JACK : SRC_BUILTIN;
            source_t next = cur;

            if (cur == SRC_USB) {
                if (uac_mic_gone()) {
                    uac_mic_close();
                    ESP_LOGI(TAG, "USB microphone unplugged");
                    s_rate = SPECTRUM_SAMPLE_RATE;
                    next = switch_source(cur, es7210);
                }
            } else if (uac_mic_announced() && uac_generation() != s_usb_refused_gen) {
                uint32_t rate = 0;
                uint8_t ch = 0;
                const esp_err_t err = uac_mic_open(&rate, &ch);
                if (err == ESP_OK) {
                    audio_out_capture_end();        /* the ES7210 is not wanted */
                    s_usb_channels = ch;
                    /* 0010: product strings arrive space-padded ("C-Media
                     * USB Headphone Set  "), which the label does not want. */
                    char product[40];
                    snprintf(product, sizeof(product), "%s", uac_mic_product());
                    for (size_t len = strlen(product); len && product[len - 1] == ' '; )
                        product[--len] = '\0';
                    if (rate == SPECTRUM_SAMPLE_RATE)
                        snprintf(s_usb_label, sizeof(s_usb_label), "USB %.30s", product);
                    else
                        snprintf(s_usb_label, sizeof(s_usb_label), "USB %.24s, %u kHz",
                                 product, (unsigned)(rate / 1000));
                    s_rate = rate;
                    next = SRC_USB;
                    ESP_LOGI(TAG, "source: USB microphone \"%s\", %u Hz, %u ch",
                             product, (unsigned)rate, (unsigned)ch);
                } else {
                    s_usb_refused_gen = uac_generation();
                    ESP_LOGW(TAG, "USB microphone refused: %s", esp_err_to_name(err));
                }
            } else if (es7210 != cur) {
                next = switch_source(cur, es7210);
            }

            if (next != cur) {
                cur = next;
                /* 0014: a new source is a new gain and maybe a new
                 * rate; a reading that spanned two would mean neither. */
                spectrum_meter_init(&s_meter, (double)s_rate);
                s_source = cur;
            }
        }

        if (cur == SRC_USB) {
            const size_t n = uac_mic_read(s_frames, CAPTURE_CHUNK, 100);
            const int ch = s_usb_channels == 2 ? 2 : 1;
            for (size_t i = 0; i < n; i++) {
                /* 16-bit values, one or two channels: to mono at 24-bit scale. */
                const float v = ch == 2 ? ((float)s_frames[2 * i] + (float)s_frames[2 * i + 1]) * 0.5f
                                        : (float)s_frames[i];
                s_chunk[i] = v * 256.0f;
            }
            if (n) feed(s_chunk, n);
            else   vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }

        const size_t n = audio_out_capture_read(s_frames, CAPTURE_CHUNK, 100);
        for (size_t i = 0; i < n; i++) {
            if (cur == SRC_JACK) {
                /* One frame, one int32, holding a 16-bit value: scaled up
                 * to the 24-bit full scale spectrum.h measures against. */
                s_chunk[i] = (float)s_frames[i] * 256.0f;
            } else {
                /* MIC1 left, MIC2 right; the sketch asked M5Unified for mono. */
                s_chunk[i] = ((float)s_frames[2 * i] + (float)s_frames[2 * i + 1]) * 0.5f;
            }
        }
        if (n) feed(s_chunk, n);
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
    /* 0013: the bass window, the newest one too. Until a whole one has
     * arrived (341 ms after boot) the bass bars stay at the floor. */
    const uint32_t bend = monoring_count(&s_bass_ring);
    s_bass_ready = bend >= SPECTRUM_FFT_SIZE;
    if (s_bass_ready) {
        monoring_newest(&s_bass_ring, bend, s_mono, SPECTRUM_FFT_SIZE);
        for (int i = 0; i < SPECTRUM_FFT_SIZE; i++) {
            s_bass_fft[2 * i]     = s_mono[i] * s_window[i];
            s_bass_fft[2 * i + 1] = 0.0f;
        }
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
    if (!s_bass_ready) {
        memset(s_bass_mag, 0, sizeof(s_bass_mag));
        return;
    }
    dsps_fft2r_fc32(s_bass_fft, SPECTRUM_FFT_SIZE);       /* 0013 */
    dsps_bit_rev_fc32(s_bass_fft, SPECTRUM_FFT_SIZE);
    for (int i = 0; i < SPECTRUM_FFT_SIZE / 2; i++) {
        const float re = s_bass_fft[2 * i], im = s_bass_fft[2 * i + 1];
        s_bass_mag[i] = sqrtf(re * re + im * im);
    }
}

/* One frame of bars. Returns the dirty landscape COLUMN range through
 * *x0, *x1 (x1 exclusive; x0 == x1 when nothing changed) -- 0006: the
 * columns, because columns are what map to contiguous panel rows. */
/* 0016: bar b at s_level[b] and s_peak[b], `w` px wide from xs: only
 * the rows that changed, widening [*dl, *dr) by what it touched. Was
 * the body of render()'s loop. */
static void paint_bar(int b, int xs, int w, bool partial, int *dl, int *dr)
{
    {
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
            repaint_rows(xs, w, y_top, y_bot, bar_top_y, peak_y, partial);
            if (xs < *dl)     *dl = xs;
            if (xs + w > *dr) *dr = xs + w;
        }
        s_prev_h[b]      = new_h;
        s_prev_peak_y[b] = peak_y;
    }
}

/* 0016: the 1/3-octave bands: power, Fast or Slow, and a peak that
 * holds and sinks as the bars' does. No attack/release -- the time
 * weighting is the standard's, not a look. */
static void render_oct(float dt, int *dl, int *dr)
{
    const double tau = s_oct_slow ? SPECTRUM_SLOW_TAU : SPECTRUM_FAST_TAU;
    for (int b = 0; b < SPECTRUM_OCT_BANDS; b++) {
        if (s_oct_deaf[b]) continue;
        const float ms = spectrum_oct_ms(&s_oct[b], s_oct[b].bass ? s_bass_mag : s_mag);
        spectrum_oct_weight(&s_oct_e[b], ms, dt, tau);
        s_level[b] = spectrum_oct_frac(s_oct_e[b]);
        s_peak[b]  = fmaxf(s_level[b], s_peak[b] - SPECTRUM_PEAK_FALL_PER_SEC * dt);
        paint_bar(b, s_left + b * s_oct_slot + OCT_GAP / 2, s_oct_slot - OCT_GAP,
                  s_oct[b].partial, dl, dr);
    }
}

static void render(float dt, int *x0, int *x1)
{
    int dirty_l = s_w, dirty_r = 0;

    if (s_mode == MODE_OCT) {
        render_oct(dt, &dirty_l, &dirty_r);
    } else {
        for (int b = s_first_col; b < s_graph_w; b++) {
            const float mag = spectrum_band_mag(&s_band[b], s_band[b].bass ? s_bass_mag : s_mag);
            const float target = spectrum_frac(spectrum_dbfs(mag));
            s_wf_frac[b] = target;              /* 0015: the waterfall takes it raw */
            spectrum_ballistics(target, dt, &s_level[b], &s_peak[b]);
            paint_bar(b, s_left + b, 1, false, &dirty_l, &dirty_r);   /* 0012: a column, no gap */
        }
    }

    *x0 = dirty_l;
    *x1 = dirty_r > dirty_l ? dirty_r : dirty_l;
}

/* 0015: move the waterfall down `n` rows and paint the newest at the
 * top, straight in the shadow's panel rows. Returns the dirty columns. */
static void waterfall_push(int n, int *x0, int *x1)
{
    if (n <= 0 || s_mode != MODE_WATERFALL) return;
    if (n > s_wf_h) n = s_wf_h;
    uint16_t *fb = gfx_fb();
    const int stride = gfx_w();
    for (int b = s_first_col; b < s_graph_w; b++) {
        const int lx = s_left + b;
        uint16_t *row = fb + (size_t)landmap_py(lx, 0, LAND_W) * (size_t)stride +
                        landmap_px(lx, s_wf_top, LAND_W);
        if (n < s_wf_h) memmove(row + n, row, (size_t)(s_wf_h - n) * sizeof(uint16_t));
        const uint16_t c = s_heat[(int)(s_wf_frac[b] * 255.0f + 0.5f)];
        for (int i = 0; i < n; i++) row[i] = c;
    }
    const int l = s_left + s_first_col, r = s_left + s_graph_w;
    if (*x1 <= *x0) { *x0 = l; *x1 = r; return; }
    if (l < *x0) *x0 = l;
    if (r > *x1) *x1 = r;
}

/* 0007: which source the bars are, between the title and the fps. */
static void draw_source(int src)
{
    const int x = 560, y = 14, h = ARK12_H * LABEL_SCALE, x_end = MODE_X - 14;   /* 0015 */
    land_fill(x, y, x_end - x, h, COL_BLACK);
    land_text(x, y, src == SRC_USB ? s_usb_label : k_source_name[src], LABEL_SCALE, COL_LABEL);
    land_blit(x, x_end);
}

/*
 * 0013: the lowest frequency each source hears. The array microphones
 * stop telling frequencies apart at about 80 Hz (0004, observed on the
 * board); a headset's capsule is no better; a USB microphone may be a
 * measurement one, flat to 20 Hz. Below the floor the graph is greyed
 * and not drawn, so a deaf region is not mistaken for a quiet one.
 */
static const float k_source_floor_hz[SRC_COUNT] = {
    [SRC_BUILTIN] = 80.0f,
    [SRC_JACK]    = 80.0f,
    [SRC_USB]     = SPECTRUM_LOG_MIN_HZ,
};

/* 0008: the bars for a sample rate -- bands, the axis's top, a cleared
 * graph with its labels -- and every bar back at the floor, since the
 * graph under them has just been repainted. 0013: and for a source,
 * whose floor greys the left of the graph. */
static void layout_for(uint32_t rate, int src)
{
    set_geometry();                         /* 0015: by the mode */
    s_max_hz = (float)rate / 2.0f;
    s_first_col = spectrum_freq_x_at(k_source_floor_hz[src], s_graph_w, s_max_hz);
    for (int b = 0; b < s_graph_w; b++) {
        s_band[b] = spectrum_band_n(b, s_graph_w, (float)rate);
        s_level[b] = s_peak[b] = 0.0f;
        s_prev_h[b] = 0;
        s_prev_peak_y[b] = s_bottom;
    }
    /* 0016: the 1/3-octave bands, for this rate and this source. A band
     * whose centre the source cannot hear is greyed, like the columns.
     * 0018: whose TOP EDGE -- the 20 Hz band is centred on 19.95 Hz, so a
     * centre test greyed it for a source that hears 20 Hz. */
    s_oct_slot = s_graph_w / SPECTRUM_OCT_BANDS;
    for (int b = 0; b < SPECTRUM_OCT_BANDS; b++) {
        s_oct[b] = spectrum_oct_band(b, (float)rate);
        s_oct_e[b] = 0.0f;
        s_oct_deaf[b] = spectrum_oct_centre(b) * powf(10.0f, 0.05f) <= k_source_floor_hz[src];
    }
    draw_static();

    /* 0020: what a source cannot hear is an empty outline from the floor
     * to -80 dBFS -- a bar with nothing in it, not a grey block that
     * reads as a fault. A band each in the 1/3-octave view; one box over
     * the span of columns in the others. The waterfall under it keeps
     * its dark fill: it has no level for a box to stand on. */
    const int deaf_y = s_bottom - (int)(spectrum_frac(DEAF_DBFS) * (float)s_graph_h);
    if (s_mode == MODE_OCT) {
        int last_deaf = -1;
        for (int b = 0; b < SPECTRUM_OCT_BANDS; b++) {
            if (!s_oct_deaf[b]) continue;
            land_outline(s_left + b * s_oct_slot + OCT_GAP / 2, deaf_y,
                         s_oct_slot - OCT_GAP, s_bottom - deaf_y, COL_BASE);
            last_deaf = b;
        }
        if (last_deaf >= 0) land_blit(s_left, s_left + (last_deaf + 1) * s_oct_slot);
        return;
    }
    if (s_first_col > 0) {
        land_outline(s_left, deaf_y, s_first_col, s_bottom - deaf_y, COL_BASE);
        if (s_mode == MODE_WATERFALL)       /* 0015 */
            land_fill(s_left, s_wf_top, s_first_col, s_wf_h, COL_DEAF);
        land_blit(s_left, s_left + s_first_col);
    }
}

/* 0014: the meter's line, under the title. dB re a full-scale sine,
 * which is not dB SPL, and the label says which. */
static void draw_meters(void)
{
    const int x = 10, y = 48, h = ARK12_H * LABEL_SCALE;
    const float secs = s_pub_secs;
    const unsigned t = (unsigned)secs;
    char buf[96];
    snprintf(buf, sizeof(buf), "LAeq %6.1f   LAFmax %6.1f   LCpeak %6.1f   dB re FS sine   %u:%02u:%02u",
             (double)s_pub_laeq, (double)s_pub_lafmax, (double)s_pub_lcpeak,
             t / 3600, t / 60 % 60, t % 60);
    land_fill(x, y, s_w - x, h, COL_BLACK);
    land_text(x, y, buf, LABEL_SCALE, COL_LABEL);
    if (s_pub_clip) land_text(s_w - 90, y, "CLIP", LABEL_SCALE, COL_CLIP);
    land_blit(x, s_w);
}

static void draw_fps(float fps)
{
    const int x = s_w - 170, y = 78, h = ARK12_H * LABEL_SCALE;   /* 0015: under the buttons */
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

    /* 0014: touch, for resetting the meter. Not fatal: without it the
     * meter resets on a source change only. */
    if (touch_init(tab5io_bus(), LCD_H_RES, LCD_V_RES) != ESP_OK)
        ESP_LOGW(TAG, "no touch; the meter resets only when the source changes");
    /* 0015: points in the landscape this file draws in. gfx stays at
     * GFX_ROT_0 and landmap.h is its 270, so touch is told 270. */
    touch_set_rotation(GFX_ROT_270);

    /* Capture borrows playback's clocks (audio_out.h), so playback comes
     * up too, at the capture rate, and plays nothing. */
    ESP_ERROR_CHECK(audio_out_init(tab5io_bus(), tab5io_exp1(), SPECTRUM_SAMPLE_RATE));
    ESP_ERROR_CHECK(audio_out_capture_begin(AUDIO_CAPTURE_BUILTIN));

    /*
     * 0008: the USB-A port, for a USB microphone. Not fatal: without it
     * the built-in mics and the jack still work. The UAC class driver is
     * registered before the port comes up, as usbhost.h requires, and
     * usbhost_start() powers the port on its own bus task.
     */
    if (usbhost_init(tab5io_exp2()) == ESP_OK && uac_init() == ESP_OK) {
        usbhost_start();
    } else {
        ESP_LOGW(TAG, "no USB host; USB microphones will not be seen");
    }

    spectrum_bass_filter(s_bass_taps);      /* 0013: before capture's first chunk */
    spectrum_meter_init(&s_meter, SPECTRUM_SAMPLE_RATE);   /* 0014: likewise */
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
    for (int i = 0; i < 256; i++) {         /* 0015 */
        int r, g, b;
        spectrum_heat_rgb((float)i / 255.0f, &r, &g, &b);
        s_heat[i] = RGB(r, g, b);
    }
    set_geometry();                         /* 0015: was the layout here */

    layout_for(SPECTRUM_SAMPLE_RATE, SRC_BUILTIN);     /* 0008, 0013 */
    ESP_ERROR_CHECK(lcd_backlight_set(ANALYZER_BACKLIGHT));
    ESP_LOGI(TAG, "%dx%d, graph %dx%d, a bar per column", s_w, s_h,
             s_graph_w, s_graph_h);

    int64_t last_us = esp_timer_get_time();
    int64_t report_us = last_us;
    int shown_source = -1;          /* 0007: drawn on the first frame */
    uint32_t shown_rate = SPECTRUM_SAMPLE_RATE;    /* 0008 */
    uint32_t frames = 0;
    int64_t t_capture = 0, t_fft = 0, t_draw = 0;
    int64_t t_bars = 0, t_fall = 0, t_blit = 0;    /* 0021: draw, in parts */
    int64_t meter_us = 0, touch_us = 0, press_us = -1;     /* 0014 */
    bool reset_done = false;
    bool press_on_mode = false;             /* 0015: a press that chose a mode resets nothing */

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
        const int64_t tb = esp_timer_get_time();
        waterfall_push(spectrum_wf_rows(&s_wf_acc, dt, WF_ROWS_PER_SEC), &x0, &x1);  /* 0015 */
        const int64_t tf = esp_timer_get_time();
        if (x1 > x0) land_blit(x0, x1);
        const int64_t tl = esp_timer_get_time();
        t_bars += tb - now;
        t_fall += tf - tb;
        t_blit += tl - tf;
        if (s_source != shown_source) {
            shown_source = s_source;
            /* 0008: a USB microphone's rate, published before its source.
             * 0013: every source change, since each has its own floor. */
            shown_rate = s_rate;
            layout_for(shown_rate, shown_source);
            draw_source(shown_source);
        }
        const int64_t t3 = esp_timer_get_time();

        t_capture += t1 - t0;
        t_fft     += t2 - t1;
        t_draw    += t3 - t2;
        frames++;

        if (t3 - report_us >= 1000000) {
            const float fps = (float)frames * 1e6f / (float)(t3 - report_us);
            /* 0021: draw in parts -- the bars, the waterfall's scroll and
             * the blit -- to say which of them FALL's extra 17 ms is. */
            ESP_LOGI(TAG, "FPS: %.1f | capture %.1fms  fft %.1fms  draw %.1fms "
                     "(bars %.1f  fall %.1f  blit %.1f)",
                     fps, t_capture / 1000.0f / frames, t_fft / 1000.0f / frames,
                     t_draw / 1000.0f / frames, t_bars / 1000.0f / frames,
                     t_fall / 1000.0f / frames, t_blit / 1000.0f / frames);
            draw_fps(fps);
            frames = 0;
            report_us = t3;
            t_capture = t_fft = t_draw = 0;
            t_bars = t_fall = t_blit = 0;
        }

        /* 0014: the meter's line, and a held press to reset it. */
        if (t3 - meter_us >= METER_DRAW_US) {
            meter_us = t3;
            draw_meters();
        }
        if (touch_present() && t3 - touch_us >= TOUCH_POLL_US) {
            touch_us = t3;
            int tx, ty;
            if (!touch_get(&tx, &ty)) {
                press_us = -1;
                reset_done = false;
                press_on_mode = false;
            } else if (press_us < 0) {
                press_us = t3;
                /* 0015: a press on a mode button picks that mode. */
                if (ty >= 0 && ty < MODE_HIT_Y1 && tx >= MODE_HIT_X0 && tx < s_w) {   /* 0021 */
                    press_on_mode = true;
                    int i = (tx - MODE_X) / MODE_W;                 /* 0016 */
                    if (tx < MODE_X) i = 0;
                    if (i > MODE_COUNT - 1) i = MODE_COUNT - 1;
                    const view_mode_t want = (view_mode_t)i;
                    if (want != s_mode && shown_source >= 0) {
                        s_mode = want;
                        layout_for(shown_rate, shown_source);
                        draw_source(shown_source);
                        meter_us = 0;       /* the repaint wiped the meter line */
                        ESP_LOGI(TAG, "mode: %s", want == MODE_WATERFALL ? "waterfall" :
                                                  want == MODE_OCT ? "1/3 octave" : "bars");
                    }
                } else if (s_mode == MODE_OCT && ty >= WEIGHT_HIT_Y0 && ty < WEIGHT_HIT_Y1 &&
                           tx >= WEIGHT_X - 10 && tx < WEIGHT_X + WEIGHT_W + 10) {     /* 0021 */
                    /* 0016: Fast <-> Slow. The averages carry on, so the
                     * bars glide to the new weighting rather than jump. */
                    press_on_mode = true;
                    s_oct_slow = !s_oct_slow;
                    ESP_LOGI(TAG, "1/3 octave: %s", s_oct_slow ? "slow" : "fast");   /* 0021 */
                    draw_weight_button();
                    land_blit(WEIGHT_X - 1, WEIGHT_X + WEIGHT_W + 1);
                }
            } else if (!press_on_mode && !reset_done && t3 - press_us >= RESET_HOLD_US) {
                s_meter_reset_req = true;
                reset_done = true;
                ESP_LOGI(TAG, "meter reset");
            }
        }

        if (FRAME_LIMIT_HZ > 0) {
            const int64_t target = 1000000 / (FRAME_LIMIT_HZ > 0 ? FRAME_LIMIT_HZ : 1);
            const int64_t spent = esp_timer_get_time() - now;
            if (spent < target) vTaskDelay(pdMS_TO_TICKS((target - spent) / 1000));
        }
    }
}
