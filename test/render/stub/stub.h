/*
 * stub.h -- just enough ESP-IDF, FreeRTOS, esp-dsp and the feckless
 * libraries for main/analyzer.c to compile and draw on a PC. Every
 * header analyzer.c includes is a one-line file here that includes this.
 *
 * The display is real: a 720 x 1280 RGB565 shadow buffer, as gfx's, that
 * render.c reads back through landmap.h. The FFT is real: a plain
 * radix-2, so the picture is what esp-dsp would give. Everything that
 * talks to hardware is a no-op or a constant.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ---- esp ---- */
typedef int esp_err_t;
#define ESP_OK              0
#define ESP_FAIL            (-1)
#define ESP_ERR_NOT_FOUND   0x105
#define ESP_ERROR_CHECK(x)  do { (void)(x); } while (0)
#define ESP_LOGI(tag, ...)  do { (void)(tag); } while (0)
#define ESP_LOGW(tag, ...)  do { (void)(tag); } while (0)
#define ESP_LOGE(tag, ...)  do { (void)(tag); } while (0)
static inline const char *esp_err_to_name(esp_err_t e) { (void)e; return "stub"; }

typedef struct { const char *version, *idf_ver, *date, *time; } esp_app_desc_t;
static inline const esp_app_desc_t *esp_app_get_description(void) { return NULL; }

extern int64_t stub_now_us;          /* render.c advances it */
static inline int64_t esp_timer_get_time(void) { return stub_now_us; }

/* ---- FreeRTOS ---- */
#define pdMS_TO_TICKS(ms)   (ms)
#define pdPASS              1
static inline void vTaskDelay(int ticks) { (void)ticks; }
static inline int xTaskCreatePinnedToCore(void (*fn)(void *), const char *name, int stack,
                                          void *arg, int prio, void *handle, int core)
{
    (void)fn; (void)name; (void)stack; (void)arg; (void)prio; (void)handle; (void)core;
    return pdPASS;
}

/* ---- esp-dsp: an in-order FFT, so bit reversal is a no-op ---- */
static inline esp_err_t dsps_fft2r_init_fc32(float *t, int n) { (void)t; (void)n; return ESP_OK; }
static inline void dsps_bit_rev_fc32(float *x, int n) { (void)x; (void)n; }
static inline void dsps_fft2r_fc32(float *x, int n)
{
    for (int i = 1, j = 0; i < n; i++) {             /* bit-reverse first */
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            float t = x[2 * i]; x[2 * i] = x[2 * j]; x[2 * j] = t;
            t = x[2 * i + 1]; x[2 * i + 1] = x[2 * j + 1]; x[2 * j + 1] = t;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        const double a = -2.0 * 3.14159265358979323846 / len;
        for (int i = 0; i < n; i += len)
            for (int k = 0; k < len / 2; k++) {
                const float wr = (float)cos(a * k), wi = (float)sin(a * k);
                float *u = &x[2 * (i + k)], *v = &x[2 * (i + k + len / 2)];
                const float vr = v[0] * wr - v[1] * wi, vi = v[0] * wi + v[1] * wr;
                v[0] = u[0] - vr; v[1] = u[1] - vi;
                u[0] += vr;       u[1] += vi;
            }
    }
}

/* ---- the panel and gfx: a real shadow buffer ---- */
#define LCD_H_RES   720
#define LCD_V_RES   1280
#define RGB(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
#define GFX_ROT_0   0
#define GFX_ROT_270 3
typedef void *esp_lcd_panel_handle_t;
extern uint16_t stub_fb[LCD_H_RES * LCD_V_RES];
static inline uint16_t *gfx_fb(void) { return stub_fb; }
static inline int gfx_w(void) { return LCD_H_RES; }
static inline int gfx_h(void) { return LCD_V_RES; }
static inline void gfx_blit(int y0, int y1) { (void)y0; (void)y1; }
static inline void gfx_fill_rect(int x, int y, int w, int h, uint16_t c)
{
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++)
            if (xx >= 0 && xx < LCD_H_RES && yy >= 0 && yy < LCD_V_RES)
                stub_fb[yy * LCD_H_RES + xx] = c;
}
static inline esp_err_t gfx_init(esp_lcd_panel_handle_t p, int w, int h) { (void)p; (void)w; (void)h; return ESP_OK; }
static inline esp_err_t lcd_init(esp_lcd_panel_handle_t *p) { (void)p; return ESP_OK; }
static inline esp_err_t lcd_backlight_set(int pct) { (void)pct; return ESP_OK; }

/* ---- the board ---- */
typedef void *i2c_master_bus_handle_t;
static inline esp_err_t tab5io_init(void) { return ESP_OK; }
static inline i2c_master_bus_handle_t tab5io_bus(void) { return NULL; }
static inline void *tab5io_exp1(void) { return NULL; }
static inline void *tab5io_exp2(void) { return NULL; }

typedef enum { AUDIO_CAPTURE_BUILTIN, AUDIO_CAPTURE_HEADSET } audio_capture_src_t;
static inline esp_err_t audio_out_init(void *b, void *e, int r) { (void)b; (void)e; (void)r; return ESP_OK; }
static inline esp_err_t audio_out_capture_begin(audio_capture_src_t s) { (void)s; return ESP_OK; }
static inline void audio_out_capture_end(void) {}
static inline bool audio_out_headphones(void) { return false; }
static inline size_t audio_out_capture_read(int32_t *f, size_t n, int ms) { (void)f; (void)n; (void)ms; return 0; }

static inline esp_err_t usbhost_init(void *e) { (void)e; return ESP_FAIL; }
static inline void usbhost_start(void) {}
static inline esp_err_t uac_init(void) { return ESP_FAIL; }
static inline bool uac_mic_gone(void) { return false; }
static inline void uac_mic_close(void) {}
static inline bool uac_mic_announced(void) { return false; }
static inline uint32_t uac_generation(void) { return 0; }
static inline esp_err_t uac_mic_open(uint32_t *r, uint8_t *c) { (void)r; (void)c; return ESP_FAIL; }
static inline const char *uac_mic_product(void) { return ""; }
static inline size_t uac_mic_read(int32_t *f, size_t n, int ms) { (void)f; (void)n; (void)ms; return 0; }

static inline esp_err_t touch_init(i2c_master_bus_handle_t b, int w, int h) { (void)b; (void)w; (void)h; return ESP_ERR_NOT_FOUND; }
static inline bool touch_get(int *x, int *y) { (void)x; (void)y; return false; }
static inline bool touch_present(void) { return false; }
static inline void touch_set_rotation(int q) { (void)q; }

/* ---- 0026: the heap ---- */
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM   (1u << 10)
static inline void *heap_caps_malloc(size_t sz, uint32_t caps) { (void)caps; return malloc(sz); }
