/*
 * menu.h -- the gear and the settings screen, as geometry with no
 * drawing in it, so the hit tests run on the host (test/menutest.c).
 * Landscape coordinates, LAND_W x LAND_H, as the rest of analyzer.c's
 * layout.
 *
 * 0036 put the view buttons (BARS, 1/3 OCT, FALL), FAST/SLOW and MON in
 * a menu hung under a gear, with LICENCES opening a page of its own.
 * 0051: one settings screen in their place, the whole screen and
 * scrolled by a finger -- the controls first, then the libraries
 * underneath -- and every control sized for the panel. The Tab5's is 5"
 * at 720 x 1280, about 294 px an inch: 0036's 42 px gear and 56 px rows
 * were under 5 mm, smaller than a fingertip. Buttons here are 96 px
 * (8.3 mm) tall and 36 px text, as the other Tab5 apps' keys are.
 *
 * Every press zone reaches further down than what is drawn, because on
 * the board fingers landed 1 to 80 px BELOW the buttons they were aimed
 * at (0021).
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>

/* ---- the gear, on the graph's screen: top right ---- */

/* 0051: 80 px, 6.9 mm, in the 104 px band above the graph (set_geometry()'s
 * s_top). Its press zone is that whole band's height and reaches 60 px
 * further left. */
#define MENU_GEAR_W         (80)
#define MENU_GEAR_Y         (6)
#define MENU_GEAR_X(lw)     ((lw) - 10 - MENU_GEAR_W)
#define MENU_GEAR_HIT_Y1    (104)
#define MENU_GEAR_HIT_PAD   (60)

static inline bool menu_gear_hit(int x, int y, int lw)
{
    return y >= 0 && y < MENU_GEAR_HIT_Y1 && x >= MENU_GEAR_X(lw) - MENU_GEAR_HIT_PAD && x < lw;
}

/* ---- the settings screen ---- */

/* The header: BACK at the left, the title, the gear lit where it was on
 * the graph's screen. Either closes the settings. */
#define SET_HEAD_H          (112)
#define SET_BACK_X          (10)
#define SET_BACK_Y          (8)
#define SET_BACK_W          (220)
#define SET_BACK_H          (96)

/* The scrolled part, under the header. A section is a label, a row of
 * buttons, and a gap; the libraries follow the last. */
#define SET_LEFT            (40)
#define SET_LABEL_H         (52)        /* the section's name, LABEL_SCALE */
#define SET_BTN_W           (280)
#define SET_BTN_H           (96)
#define SET_BTN_GAP         (16)        /* between buttons in a row */
#define SET_GAP             (28)        /* under a row: the press zone's reach */
#define SET_SECTIONS        (3)         /* view, 1/3-octave weighting, monitor */
#define SET_SECTION_H       (SET_LABEL_H + SET_BTN_H + SET_GAP)
#define SET_LIBS_HEAD_H     (64)        /* a rule, "Libraries", the repository */
#define SET_SCROLL_W        (10)        /* where the screen is, at the right edge */
#define SET_SLOP            (16)        /* a finger moving this far drags, not taps */

typedef enum {
    SET_HIT_NOTHING = 0,
    SET_HIT_CLOSE,          /* BACK, or the gear */
    SET_HIT_BARS,           /* section 0, in view_mode_t's order */
    SET_HIT_OCT,
    SET_HIT_FALL,
    SET_HIT_FAST,           /* section 1: the 1/3-octave weighting */
    SET_HIT_SLOW,
    SET_HIT_MON,            /* section 2: one button, two wide */
} set_hit_t;

/* Section s's buttons' top, and button b's left edge, from the top of the
 * scrolled part. */
static inline int set_btn_y(int s) { return s * SET_SECTION_H + SET_LABEL_H; }
static inline int set_btn_x(int b) { return SET_LEFT + b * (SET_BTN_W + SET_BTN_GAP); }

/* Where the libraries' heading starts, and how tall the whole scrolled
 * part is with n of them at line_h a line. */
static inline int set_libs_y(void) { return SET_SECTIONS * SET_SECTION_H; }
static inline int set_content_h(int n, int line_h)
{
    return set_libs_y() + SET_LIBS_HEAD_H + n * line_h + SET_GAP;
}

/* The furthest the scrolled part can move up, for a screen lh tall. */
static inline int set_max_scroll(int n, int line_h, int lh)
{
    const int view = lh - SET_HEAD_H, m = set_content_h(n, line_h) - view;
    return m > 0 ? m : 0;
}

static inline int set_clamp_scroll(int scroll, int n, int line_h, int lh)
{
    const int m = set_max_scroll(n, line_h, lh);
    return scroll < 0 ? 0 : scroll > m ? m : scroll;
}

/* What a tap at (x, y) on the screen hits, scrolled by `scroll`. A button
 * takes its row's whole width to the next one's start and the gap under
 * it; the gaps between buttons go to the nearer. */
static inline set_hit_t set_at(int x, int y, int scroll, int lw)
{
    if (y < SET_HEAD_H) {
        if (x < SET_BACK_X + SET_BACK_W + 20) return SET_HIT_CLOSE;
        if (menu_gear_hit(x, y, lw)) return SET_HIT_CLOSE;
        return SET_HIT_NOTHING;
    }
    const int cy = y - SET_HEAD_H + scroll;
    if (cy >= set_libs_y()) return SET_HIT_NOTHING;
    const int s = cy / SET_SECTION_H, in = cy - s * SET_SECTION_H;
    if (in < SET_LABEL_H) return SET_HIT_NOTHING;
    int b = (x - SET_LEFT + SET_BTN_GAP / 2) / (SET_BTN_W + SET_BTN_GAP);
    if (x < SET_LEFT - SET_BTN_GAP) return SET_HIT_NOTHING;
    if (b < 0) b = 0;
    switch (s) {
    case 0:
        if (b > 2) return x < set_btn_x(2) + SET_BTN_W + SET_BTN_GAP ? SET_HIT_FALL : SET_HIT_NOTHING;
        return (set_hit_t)(SET_HIT_BARS + b);
    case 1:
        if (b > 1) return x < set_btn_x(1) + SET_BTN_W + SET_BTN_GAP ? SET_HIT_SLOW : SET_HIT_NOTHING;
        return b == 0 ? SET_HIT_FAST : SET_HIT_SLOW;
    default:
        return x < set_btn_x(1) + SET_BTN_W + SET_BTN_GAP ? SET_HIT_MON : SET_HIT_NOTHING;
    }
}
