/*
 * landmap.h -- the analyzer's landscape picture on the panel's portrait
 * rows, without asking gfx to rotate.
 *
 * WHY (0006)
 *
 * The panel scans portrait, 720 wide and 1280 tall. 0002 drew landscape
 * by setting gfx to GFX_ROT_270, which keeps the shadow buffer in
 * landscape rows and turns each blit through a gather-transpose:
 * a band of landscape rows is a band of panel COLUMNS, read a pixel at a
 * time down a stride. On the board that blit was most of every frame.
 *
 * Now gfx stays at GFX_ROT_0, the shadow buffer is in panel order, and
 * this file says where each landscape pixel goes. A bar -- a run of
 * landscape columns -- is a run of panel ROWS, full width and
 * contiguous, so blitting it is a straight copy. The picture on the
 * glass is the same one GFX_ROT_270 produced: the same way up.
 *
 * THE MAPPING
 *
 * gfx's 270 gather (feckless-graphics-handler, gather_rotated()) reads
 * landscape pixel (lx, ly) for panel pixel (px, py) where
 *
 *   ly = px,  lx = land_w - 1 - py
 *
 * so the forward map is px = ly, py = land_w - 1 - lx. test/spectrumtest.c
 * checks these against each other.
 *
 * Header-only and free of ESP-IDF.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Where landscape pixel (lx, ly) is on the panel. land_w is the
 * landscape width, which is the panel's height. */
static inline int landmap_px(int lx, int ly, int land_w) { (void)lx; (void)land_w; return ly; }
static inline int landmap_py(int lx, int ly, int land_w) { (void)ly; return land_w - 1 - lx; }

/* A landscape rectangle as a panel rectangle. */
typedef struct { int x, y, w, h; } landmap_rect_t;

static inline landmap_rect_t landmap_rect(int lx, int ly, int w, int h, int land_w)
{
    landmap_rect_t r = { ly, land_w - lx - w, h, w };
    return r;
}

/* The panel rows [*y0, *y1) that landscape columns [x0, x1) occupy:
 * what to blit after drawing in them. */
static inline void landmap_rows(int x0, int x1, int land_w, int *y0, int *y1)
{
    *y0 = land_w - x1;
    *y1 = land_w - x0;
}

#ifdef __cplusplus
}
#endif
