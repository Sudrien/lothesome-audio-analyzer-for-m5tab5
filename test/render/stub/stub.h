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
extern uint16_t stub_fb[LCD_H_RES * LCD_V_RES];   /* 128-byte aligned, in render.c */
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

/* ---- 0022: heap, cache and the PPA ----
 * The PPA here is software: a block copy with the hardware's picture
 * and offset rules, so the ring's unrolling is checked on a PC. The
 * cache calls are no-ops; there is no cache. */
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM   (1u << 10)
#define MALLOC_CAP_8BIT     (1u << 2)
static inline void *heap_caps_calloc(size_t n, size_t sz, uint32_t caps) { (void)caps; return calloc(n, sz); }

#define ESP_CACHE_MSYNC_FLAG_DIR_C2M    (1u << 0)
#define ESP_CACHE_MSYNC_FLAG_DIR_M2C    (1u << 1)
#define ESP_CACHE_MSYNC_FLAG_UNALIGNED  (1u << 2)
static inline esp_err_t esp_cache_msync(void *p, size_t n, int flags) { (void)p; (void)n; (void)flags; return ESP_OK; }

typedef enum { PPA_OPERATION_SRM } ppa_operation_t;
typedef enum { PPA_SRM_COLOR_MODE_RGB565 } ppa_srm_color_mode_t;
typedef enum { PPA_SRM_ROTATION_ANGLE_0 } ppa_srm_rotation_angle_t;
typedef enum { PPA_TRANS_MODE_BLOCKING } ppa_trans_mode_t;
typedef struct { ppa_operation_t oper_type; uint32_t max_pending_trans_num; } ppa_client_config_t;
typedef struct ppa_client *ppa_client_handle_t;
typedef struct {
    const void *buffer; uint32_t pic_w, pic_h, block_w, block_h, block_offset_x, block_offset_y;
    ppa_srm_color_mode_t srm_cm;
} ppa_in_pic_blk_config_t;
typedef struct {
    void *buffer; uint32_t buffer_size, pic_w, pic_h, block_offset_x, block_offset_y;
    ppa_srm_color_mode_t srm_cm;
} ppa_out_pic_blk_config_t;
typedef struct {
    ppa_in_pic_blk_config_t in; ppa_out_pic_blk_config_t out;
    ppa_srm_rotation_angle_t rotation_angle; float scale_x, scale_y; ppa_trans_mode_t mode;
} ppa_srm_oper_config_t;

static inline esp_err_t ppa_register_client(const ppa_client_config_t *c, ppa_client_handle_t *h)
{
    (void)c; *h = (ppa_client_handle_t)(uintptr_t)1; return ESP_OK;
}
static inline esp_err_t ppa_do_scale_rotate_mirror(ppa_client_handle_t h, const ppa_srm_oper_config_t *o)
{
    (void)h;
    const ppa_in_pic_blk_config_t *i = &o->in;
    const ppa_out_pic_blk_config_t *u = &o->out;
    /* The driver's own checks: block in the input picture, block in the
     * output picture, output buffer and size cache-aligned, size fits. */
    if (i->block_w == 0 || i->block_h == 0) return ESP_FAIL;
    if (i->block_offset_x + i->block_w > i->pic_w || i->block_offset_y + i->block_h > i->pic_h) return ESP_FAIL;
    if (u->block_offset_x + i->block_w > u->pic_w || u->block_offset_y + i->block_h > u->pic_h) return ESP_FAIL;
    if ((uintptr_t)u->buffer % 128 || u->buffer_size % 128) return ESP_FAIL;
    if ((size_t)u->pic_w * u->pic_h * 2 > u->buffer_size) return ESP_FAIL;
    const uint16_t *src = i->buffer;
    uint16_t *dst = u->buffer;
    for (uint32_t y = 0; y < i->block_h; y++)
        memcpy(&dst[(size_t)(u->block_offset_y + y) * u->pic_w + u->block_offset_x],
               &src[(size_t)(i->block_offset_y + y) * i->pic_w + i->block_offset_x],
               i->block_w * sizeof(uint16_t));
    return ESP_OK;
}
