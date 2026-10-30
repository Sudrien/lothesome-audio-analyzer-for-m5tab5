/*
 * menutest.c -- main/menu.h: the gear, the menu and the licences page,
 * as presses land on them (0036).
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>

#include "menu.h"

static int checks, failures;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; \
    printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* src: analyzer.c LAND_W x LAND_H */
#define LW 1280
#define LH 720

int main(void)
{
    printf("the gear\n");
    {
        const int gx = MENU_GEAR_X(LW);
        CHECK(gx + MENU_GEAR_W == LW - 10, "ends 10 px from the edge, at %d", gx + MENU_GEAR_W);
        CHECK(menu_gear_hit(gx + MENU_GEAR_W / 2, MENU_GEAR_Y + MENU_GEAR_W / 2, LW), "its middle");
        CHECK(menu_gear_hit(gx, 60, LW), "a finger landing under it");
        CHECK(menu_gear_hit(gx - 29, 10, LW), "29 px left");
        CHECK(!menu_gear_hit(gx - 31, 10, LW), "not 31 px left");
        CHECK(!menu_gear_hit(gx, 66, LW), "not the graph under it");
        CHECK(!menu_gear_hit(640, 300, LW), "not the graph");
    }

    printf("the menu\n");
    {
        const int x0 = MENU_X(LW);
        CHECK(MENU_W == 360 && x0 + MENU_W == LW - 10, "360 wide, at %d", x0);
        CHECK(MENU_Y > MENU_GEAR_Y + MENU_GEAR_W, "under the gear");
        CHECK(MENU_Y + MENU_H < LH, "on the screen");
        static const menu_hit_t views[3] = { MENU_HIT_BARS, MENU_HIT_OCT, MENU_HIT_FALL };
        for (int s = 0; s < 3; s++) {
            const int x = menu_seg_x(s, LW) + MENU_SEG_W / 2;
            CHECK(menu_at(x, menu_row_y(0) + 30, LW) == views[s], "view %d", s);
            CHECK(menu_at(menu_seg_x(s, LW), menu_row_y(0) + 1, LW) == views[s], "view %d's corner", s);
        }
        CHECK(menu_at(x0 + 2, menu_row_y(0) + 30, LW) == MENU_HIT_BARS, "the left pad is BARS");
        CHECK(menu_at(x0 + MENU_W - 2, menu_row_y(0) + 30, LW) == MENU_HIT_FALL, "the right pad is FALL");
        CHECK(menu_at(menu_seg_x(0, LW) + 50, menu_row_y(1) + 30, LW) == MENU_HIT_FAST, "fast");
        CHECK(menu_at(menu_seg_x(1, LW) + 50, menu_row_y(1) + 30, LW) == MENU_HIT_SLOW, "slow");
        CHECK(menu_at(menu_seg_x(2, LW) + 50, menu_row_y(1) + 30, LW) == MENU_HIT_NOTHING, "the weighting's third");
        CHECK(menu_at(menu_seg_x(0, LW) + 5, menu_row_y(2) + 30, LW) == MENU_HIT_MON, "mon, left");
        CHECK(menu_at(menu_seg_x(2, LW) + 100, menu_row_y(2) + 30, LW) == MENU_HIT_MON, "mon, right");
        CHECK(menu_at(x0 + MENU_W / 2, menu_row_y(3) + 30, LW) == MENU_HIT_LICENCES, "licences");
        CHECK(menu_at(x0 + MENU_W / 2, menu_row_y(3) + MENU_ROW_H - 1, LW) == MENU_HIT_LICENCES, "licences' foot");
        CHECK(menu_at(x0 + MENU_W / 2, MENU_Y + 2, LW) == MENU_HIT_NOTHING, "the top pad");
        CHECK(menu_at(x0 + MENU_W / 2, MENU_Y + MENU_H - 2, LW) == MENU_HIT_NOTHING, "the bottom pad");
        CHECK(menu_at(x0 - 1, menu_row_y(0) + 30, LW) == MENU_HIT_OUTSIDE, "left of it");
        CHECK(menu_at(x0 + 10, MENU_Y + MENU_H, LW) == MENU_HIT_OUTSIDE, "under it");
        CHECK(menu_at(MENU_GEAR_X(LW) + 10, MENU_GEAR_Y + 10, LW) == MENU_HIT_OUTSIDE, "the gear: closes");
    }

    printf("the licences page\n");
    {
        /* 34: LABEL_SCALE's 24 px glyphs and a 10 px gap */
        CHECK(lic_rows(LH, 34) == (LH - 70 - 70) / 34, "rows %d", lic_rows(LH, 34));
        CHECK(lic_rows(100, 34) == 1 && lic_rows(LH, 0) == 1, "at least one");
        const int by = lic_btn_y(LH);
        CHECK(by > LH - LIC_FOOT_H && by + LIC_BTN_H < LH, "buttons in the footer, at %d", by);
        CHECK(lic_down_x(LW) + LIC_BTN_W == LW - 10, "down ends 10 px in");
        CHECK(lic_up_x(LW) + LIC_BTN_W < lic_down_x(LW), "up left of down");
        CHECK(lic_at(lic_down_x(LW) + 50, by + 20, LW, LH) == LIC_HIT_DOWN, "down");
        CHECK(lic_at(lic_up_x(LW) + 50, by + 20, LW, LH) == LIC_HIT_UP, "up");
        CHECK(lic_at(lic_up_x(LW) + 50, LH - 1, LW, LH) == LIC_HIT_UP, "up, at the bottom edge");
        CHECK(lic_at(lic_up_x(LW) + LIC_BTN_W + 2, by, LW, LH) == LIC_HIT_UP, "the gap is up's");
        CHECK(lic_at(lic_close_x() + 20, by + 20, LW, LH) == LIC_HIT_CLOSE, "close");
        CHECK(lic_at(LW / 2, by + 20, LW, LH) == LIC_HIT_CLOSE, "the page count closes");
        CHECK(lic_at(lic_down_x(LW) + 50, LH / 2, LW, LH) == LIC_HIT_CLOSE, "a line closes");
    }

    printf("\nmenutest: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
