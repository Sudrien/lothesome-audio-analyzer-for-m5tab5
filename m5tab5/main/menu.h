/*
 * menu.h -- 0036: the gear, its menu, and the licences page, as geometry
 * with no drawing in it, so the hit tests run on the host
 * (test/menutest.c). Landscape coordinates, LAND_W x LAND_H, as the rest
 * of analyzer.c's layout.
 *
 * Before 0036 the view buttons (BARS, 1/3 OCT, FALL), FAST/SLOW and MON
 * sat along the top right all the time. They are behind the gear now,
 * with LICENCES under them: a list of what the firmware is built from,
 * which there was nowhere to put while every control was on the graph.
 *
 * Every press zone reaches further down than what is drawn, because on
 * the board fingers landed 1 to 80 px BELOW the buttons they were aimed
 * at (0021).
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>

/* ---- the gear: top right, where the view buttons began ---- */

/* src: 0015's MODE_Y and MODE_H, 6 and 36, with the gear square at the
 * old buttons' height plus 6: 42 px is a fingertip, 36 was not quite. */
#define MENU_GEAR_W         (42)
#define MENU_GEAR_Y         (6)
#define MENU_GEAR_X(lw)     ((lw) - 10 - MENU_GEAR_W)
/* src: 0021's MODE_HIT_Y1, 66, and 30 px left of the gear: the empty
 * band above the graph takes the misses. */
#define MENU_GEAR_HIT_Y1    (66)
#define MENU_GEAR_HIT_PAD   (30)

static inline bool menu_gear_hit(int x, int y, int lw)
{
    return y >= 0 && y < MENU_GEAR_HIT_Y1 && x >= MENU_GEAR_X(lw) - MENU_GEAR_HIT_PAD && x < lw;
}

/* ---- the menu: a panel hung under the gear ---- */

/* src: 0016's MODE_W, 112, three to a row: "1/3 OCT" is 98 px of Ark12
 * at LABEL_SCALE, with room either side. */
#define MENU_SEG_W          (112)
#define MENU_PAD            (12)
#define MENU_W              (3 * MENU_SEG_W + 2 * MENU_PAD)        /* 360 */
/* src: 0021: a row as tall as a fingertip lands, not as tall as text. */
#define MENU_ROW_H          (64)
#define MENU_ROWS           (4)     /* view, weighting, MON, LICENCES */
#define MENU_H              (MENU_ROWS * MENU_ROW_H + 2 * MENU_PAD)
#define MENU_X(lw)          ((lw) - 10 - MENU_W)
#define MENU_Y              (MENU_GEAR_Y + MENU_GEAR_W + 8)      /* under the gear */

typedef enum {
    MENU_HIT_OUTSIDE = 0,   /* closes it */
    MENU_HIT_NOTHING,       /* the panel's edge, between rows */
    MENU_HIT_BARS,          /* row 0, in view_mode_t's order */
    MENU_HIT_OCT,
    MENU_HIT_FALL,
    MENU_HIT_FAST,          /* row 1: the 1/3-octave weighting */
    MENU_HIT_SLOW,
    MENU_HIT_MON,           /* row 2 */
    MENU_HIT_LICENCES,      /* row 3 */
} menu_hit_t;

/* Row r's top, and segment s's left edge (rows 0 and 1 are segmented:
 * three for the views, the first two of three for FAST and SLOW). */
static inline int menu_row_y(int r) { return MENU_Y + MENU_PAD + r * MENU_ROW_H; }
static inline int menu_seg_x(int s, int lw) { return MENU_X(lw) + MENU_PAD + s * MENU_SEG_W; }

/* What a press at (x, y) hits with the menu open. The gear itself is
 * OUTSIDE, so a second press on it closes the menu. */
static inline menu_hit_t menu_at(int x, int y, int lw)
{
    const int x0 = MENU_X(lw), y0 = MENU_Y;
    if (x < x0 || x >= x0 + MENU_W || y < y0 || y >= y0 + MENU_H) return MENU_HIT_OUTSIDE;
    const int ix = x - (x0 + MENU_PAD), iy = y - (y0 + MENU_PAD);
    if (iy < 0 || iy >= MENU_ROWS * MENU_ROW_H) return MENU_HIT_NOTHING;
    const int row = iy / MENU_ROW_H;
    int seg = ix < 0 ? 0 : ix / MENU_SEG_W;          /* the pads count as the nearest */
    if (seg > 2) seg = 2;
    switch (row) {
    case 0:  return (menu_hit_t)(MENU_HIT_BARS + seg);
    case 1:  return seg == 0 ? MENU_HIT_FAST : seg == 1 ? MENU_HIT_SLOW : MENU_HIT_NOTHING;
    case 2:  return MENU_HIT_MON;
    default: return MENU_HIT_LICENCES;
    }
}

/* ---- the licences page: the whole screen ---- */

#define LIC_HEAD_H          (70)     /* the title and the repository */
#define LIC_FOOT_H          (70)     /* close, the page count, up, down */
#define LIC_BTN_W           (112)    /* MENU_SEG_W, so they read as the same buttons */
#define LIC_BTN_H           (44)
#define LIC_MARGIN          (10)     /* draw_static()'s title inset */

/* Lines on one page at line_h px a line; at least one. */
static inline int lic_rows(int lh, int line_h)
{
    const int rows = line_h > 0 ? (lh - LIC_HEAD_H - LIC_FOOT_H) / line_h : 0;
    return rows < 1 ? 1 : rows;
}

/* The footer's buttons: close at the left, up and down at the right. */
static inline int lic_btn_y(int lh)   { return lh - LIC_FOOT_H + (LIC_FOOT_H - LIC_BTN_H) / 2; }
static inline int lic_close_x(void)   { return LIC_MARGIN; }
static inline int lic_up_x(int lw)    { return lw - LIC_MARGIN - 2 * LIC_BTN_W - MENU_PAD; }
static inline int lic_down_x(int lw)  { return lw - LIC_MARGIN - LIC_BTN_W; }

typedef enum {
    LIC_HIT_CLOSE = 0,      /* anywhere that is not up or down */
    LIC_HIT_UP,
    LIC_HIT_DOWN,
} lic_hit_t;

/* A press on the page. Up and down take the footer's whole height and
 * the gap between them; anything else closes, since the page is the
 * whole screen and has no outside to tap. */
static inline lic_hit_t lic_at(int x, int y, int lw, int lh)
{
    if (y >= lh - LIC_FOOT_H) {
        if (x >= lic_down_x(lw) - MENU_PAD / 2) return LIC_HIT_DOWN;
        if (x >= lic_up_x(lw) - MENU_PAD) return LIC_HIT_UP;
    }
    return LIC_HIT_CLOSE;
}
