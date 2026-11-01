/*
 * menutest.c -- main/menu.h: the gear, the menu and the licences page,
 * as presses land on them (0036). 0051: the gear and the settings screen
 * that replaced the other two -- its buttons, its header, its scroll.
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
        CHECK(MENU_GEAR_W * 254 / 294 >= 69, "at 294 PPI, %d.%d mm", MENU_GEAR_W * 254 / 2940,
              MENU_GEAR_W * 254 / 294 % 10);
        CHECK(MENU_GEAR_Y + MENU_GEAR_W <= 104, "in the band above the graph");
        CHECK(menu_gear_hit(gx + MENU_GEAR_W / 2, MENU_GEAR_Y + MENU_GEAR_W / 2, LW), "its middle");
        CHECK(menu_gear_hit(gx, 100, LW), "a finger landing under it");
        CHECK(menu_gear_hit(gx - 59, 10, LW), "59 px left");
        CHECK(!menu_gear_hit(gx - 61, 10, LW), "not 61 px left");
        CHECK(!menu_gear_hit(gx, 104, LW), "not the graph under it");
        CHECK(!menu_gear_hit(640, 300, LW), "not the graph");
    }

    printf("the settings' header\n");
    {
        CHECK(set_at(SET_BACK_X + SET_BACK_W / 2, SET_BACK_Y + SET_BACK_H / 2, 0, LW) == SET_HIT_CLOSE, "BACK");
        CHECK(set_at(SET_BACK_X + SET_BACK_W + 10, 100, 0, LW) == SET_HIT_CLOSE, "just right of BACK");
        CHECK(set_at(MENU_GEAR_X(LW) + 40, 40, 0, LW) == SET_HIT_CLOSE, "the gear");
        CHECK(set_at(500, 50, 0, LW) == SET_HIT_NOTHING, "the title");
        CHECK(set_at(SET_BACK_X + SET_BACK_W / 2, 50, 500, LW) == SET_HIT_CLOSE, "BACK, scrolled");
    }

    printf("the settings' buttons\n");
    {
        CHECK(SET_BTN_H * 254 / 294 >= 82, "a button at 294 PPI, %d mm tall", SET_BTN_H * 254 / 2940);
        static const set_hit_t views[3] = { SET_HIT_BARS, SET_HIT_OCT, SET_HIT_FALL };
        for (int sc = 0; sc <= 150; sc += 150) {
            for (int b = 0; b < 3; b++) {
                const int x = set_btn_x(b) + SET_BTN_W / 2, y = SET_HEAD_H + set_btn_y(0) - sc;
                if (y < SET_HEAD_H) continue;
                CHECK(set_at(x, y + 48, sc, LW) == views[b], "view %d, scrolled %d", b, sc);
                CHECK(set_at(set_btn_x(b) + 1, y + 1, sc, LW) == views[b], "view %d's corner", b);
                CHECK(set_at(x, y + SET_BTN_H + SET_GAP - 1, sc, LW) == views[b], "under view %d", b);
                CHECK(set_at(x, y - 5, sc, LW) == SET_HIT_NOTHING, "the label over view %d", b);
            }
        }
        const int y1 = SET_HEAD_H + set_btn_y(1) + 48, y2 = SET_HEAD_H + set_btn_y(2) + 48;
        CHECK(set_at(set_btn_x(0) + 10, y1, 0, LW) == SET_HIT_FAST, "fast");
        CHECK(set_at(set_btn_x(1) + 10, y1, 0, LW) == SET_HIT_SLOW, "slow");
        CHECK(set_at(set_btn_x(1) - SET_BTN_GAP / 2 + 1, y1, 0, LW) == SET_HIT_SLOW, "the gap's right half");
        CHECK(set_at(set_btn_x(1) - SET_BTN_GAP / 2 - 1, y1, 0, LW) == SET_HIT_FAST, "the gap's left half");
        CHECK(set_at(set_btn_x(2) + 50, y1, 0, LW) == SET_HIT_NOTHING, "nothing where a third would be");
        CHECK(set_at(set_btn_x(0) + 5, y2, 0, LW) == SET_HIT_MON, "mon, left");
        CHECK(set_at(set_btn_x(1) + SET_BTN_W - 5, y2, 0, LW) == SET_HIT_MON, "mon, right");
        CHECK(set_at(set_btn_x(2) + 50, y2, 0, LW) == SET_HIT_NOTHING, "right of mon");
        CHECK(set_at(set_btn_x(3) + 50, SET_HEAD_H + set_btn_y(0) + 48, 0, LW) == SET_HIT_NOTHING,
              "right of FALL");
        CHECK(set_at(10, SET_HEAD_H + set_btn_y(0) + 48, 0, LW) == SET_HIT_NOTHING, "left of BARS");
        CHECK(set_btn_x(2) + SET_BTN_W < LW - SET_SCROLL_W, "the row fits");
    }

    printf("the libraries and the scroll\n");
    {
        /* 21 libraries at 34 px, as a board build has them */
        const int n = 21, line = 34, view = LH - SET_HEAD_H;
        const int total = set_content_h(n, line);
        CHECK(total == 3 * (52 + 96 + 28) + 64 + n * line + 28, "content %d px", total);
        CHECK(set_max_scroll(n, line, LH) == total - view, "max %d", set_max_scroll(n, line, LH));
        CHECK(set_max_scroll(1, line, 2000) == 0, "no scroll when it fits");
        CHECK(set_clamp_scroll(-5, n, line, LH) == 0, "not above the top");
        CHECK(set_clamp_scroll(99999, n, line, LH) == total - view, "not past the end");
        CHECK(set_clamp_scroll(300, n, line, LH) == 300, "in between");
        const int ly = SET_HEAD_H + set_libs_y() + SET_LIBS_HEAD_H + 10 - 400;
        CHECK(set_at(LW / 2, ly, 400, LW) == SET_HIT_NOTHING, "a library is nothing to tap");
        CHECK(set_at(set_btn_x(0) + 10, SET_HEAD_H + set_btn_y(2) + 48 - 400, 400, LW) == SET_HIT_MON,
              "mon, scrolled 400");
    }

    printf("\nmenutest: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
