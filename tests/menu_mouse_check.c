// The arithmetic of the mouse in Ocarina of Time 1.1.1's menus (runtime/menu_mouse_geom.h): a button's box from its
// pane's fields as the game's focus frame takes them, the pointer in the layout's units on a 16:9 picture and on
// ultrawide ones (the UI squeezed into the middle 16:9, with panes NX2SARIA_WIDE_MENUS = full moved out by u units), the
// way back to the picture, the box a point falls in, which presses are clicks and the presses queued for the game. No
// game files. tests/menu_mouse_check.py builds and runs this.
#include <math.h>
#include <stdio.h>
#include "menu_mouse_geom.h"

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL: " __VA_ARGS__); putchar('\n'); fails++; } } while (0)
#define NEAR(a, b) (fabsf((a) - (b)) < 0.01f)

// the flags word with base position h (0 left, 1 centre, 2 right) and v (0 top, 1 centre, 2 bottom)
static unsigned base(unsigned h, unsigned v) { return (v * 3 + h) << 11 | 1u; }

int main(void) {
    MmBox b;
    // centred pane 200 x 100 at (300, -50), scale 1
    CHECK(mm_pane_box(200, 100, 1, 1, 300, -50, base(1, 1), 0, &b) && NEAR(b.x0, 200) && NEAR(b.x1, 400) &&
          NEAR(b.y0, -100) && NEAR(b.y1, 0), "centred box (%g %g %g %g)", b.x0, b.y0, b.x1, b.y1);
    // left-top base: the translation is the left edge and the top edge (y up: the box hangs below it)
    CHECK(mm_pane_box(200, 100, 1, 1, 300, -50, base(0, 0), 0, &b) && NEAR(b.x0, 300) && NEAR(b.x1, 500) &&
          NEAR(b.y0, -150) && NEAR(b.y1, -50), "left-top box (%g %g %g %g)", b.x0, b.y0, b.x1, b.y1);
    // right-bottom base, scale 2 and a mirrored scale (the size counts, not the sign)
    CHECK(mm_pane_box(100, 50, -2, 2, 0, 0, base(2, 2), 0, &b) && NEAR(b.x0, -200) && NEAR(b.x1, 0) && NEAR(b.y0, 0) &&
          NEAR(b.y1, 100), "right-bottom box (%g %g %g %g)", b.x0, b.y0, b.x1, b.y1);
    // the extra size (main+0x769930): bit 10 with bit 9 clear adds to the height, with bit 9 set to the width
    CHECK(mm_pane_box(100, 50, 1, 1, 0, 0, base(1, 1) | 1u << 10, 60, &b) && NEAR(b.x1 - b.x0, 100) &&
          NEAR(b.y1 - b.y0, 110), "extra height");
    CHECK(mm_pane_box(100, 50, 1, 1, 0, 0, base(1, 1) | 1u << 10 | 1u << 9, 60, &b) && NEAR(b.x1 - b.x0, 160) &&
          NEAR(b.y1 - b.y0, 50), "extra width");
    // no area, or not a number: no box
    CHECK(!mm_pane_box(0, 50, 1, 1, 0, 0, base(1, 1), 0, &b), "zero width");
    CHECK(!mm_pane_box(100, 50, 0, 1, 0, 0, base(1, 1), 0, &b), "zero scale (a page not shown)");
    CHECK(!mm_pane_box(100, 50, 1, 1, NAN, 0, base(1, 1), 0, &b), "NaN translation");

    // the layout units across the picture from the projection: 16:9 3840 x 2160; 21:9 (64:27) squeezed by 0.75, so
    // ±2560 = 1920 + u with u = 640, where wide_menus = full puts the menus' edge panes; 32:9 ±3840; a 1280 x 720
    // layout ±640 x ±360; a matrix that is no projection: the 16:9 values and 0
    float ux, uy, x, y, px, py;
    CHECK(mm_units(2.0f / 3840, 2.0f / 2160, &ux, &uy) && NEAR(ux, 1920) && NEAR(uy, 1080), "16:9 units (%g %g)", ux, uy);
    CHECK(mm_units(2.0f / 3840 * 0.75f, -2.0f / 2160, &ux, &uy) && NEAR(ux, 2560) && NEAR(uy, 1080), "21:9 units (%g)", ux);
    CHECK(mm_units(2.0f / 3840 * 0.5f, 2.0f / 2160, &ux, &uy) && NEAR(ux, 3840), "32:9 units (%g)", ux);
    CHECK(mm_units(2.0f / 1280, 2.0f / 720, &ux, &uy) && NEAR(ux, 640) && NEAR(uy, 360), "1280 x 720 layout");
    CHECK(!mm_units(1, 1, &ux, &uy) && NEAR(ux, 1920) && !mm_units(0, 0, &ux, &uy) && !mm_units(NAN, 1e-3f, &ux, &uy),
          "not a projection");

    // the pointer in layout units. 16:9: the picture's corners are the layout's
    mm_to_layout(0, 0, 1920, 1080, &x, &y);
    CHECK(NEAR(x, -1920) && NEAR(y, 1080), "16:9 top left (%g, %g)", x, y);
    mm_to_layout(0.5f, 0.5f, 1920, 1080, &x, &y);
    CHECK(NEAR(x, 0) && NEAR(y, 0), "centre");
    // 21:9: the layout's 3840 sit in the middle 16:9 (from 1/8 to 7/8 of the width)
    mm_to_layout(0.125f, 0.5f, 2560, 1080, &x, &y);
    CHECK(NEAR(x, -1920), "21:9: the middle 16:9's left edge is layout -1920 (%g)", x);
    mm_to_layout(1, 0, 2560, 1080, &x, &y);
    CHECK(NEAR(x, 2560) && NEAR(y, 1080), "21:9: the picture's right edge is layout 1920 + 640 (%g)", x);
    mm_to_layout(0, 1, 3840, 1080, &x, &y);
    CHECK(NEAR(x, -3840) && NEAR(y, -1080), "32:9: the left edge is layout -3840 (%g, %g)", x, y);
    // and back
    for (float u = 1920; u <= 3840; u += 640)
        for (float fx = 0; fx <= 1; fx += 0.125f) {
            mm_to_layout(fx, 1 - fx, u, 1080, &x, &y);
            mm_to_picture(x, y, u, 1080, &px, &py);
            CHECK(fabsf(px - fx) < 1e-4f && fabsf(py - (1 - fx)) < 1e-4f, "round trip at %g, %g units", fx, u);
        }

    // the box under a point: the smallest of those it is in (a slot inside its panel), -1 off them all
    MmBox boxes[] = {{-1000, -500, 1000, 500}, {-100, -50, 100, 50}, {2000, -100, 2500, 100}};
    CHECK(mm_pick(boxes, 3, 0, 0) == 1, "the slot inside the panel");
    CHECK(mm_pick(boxes, 3, 500, 0) == 0, "the panel");
    CHECK(mm_pick(boxes, 3, 1500, 0) == -1, "between them");
    // a wide_menus = full button moved out to x 2250 on a 21:9 picture: the pointer at 95% of the width reaches it
    mm_to_layout(0.95f, 0.5f, 2560, 1080, &x, &y);
    CHECK(mm_pick(boxes, 3, x, y) == 2, "a pane moved out to the 21:9 edge (pointer at layout %g)", x);
    // edges count as in
    CHECK(mm_pick(boxes, 3, 100, 50) == 1, "the box's corner");
    CHECK(mm_pick(boxes, 0, 0, 0) == -1, "no boxes");

    // movement: a pixel of a 4K picture is movement, less is not
    CHECK(!mm_moved(0.5f, 0.5f, 0.5f, 0.5f) && !mm_moved(0.5f + 0.0001f, 0.5f, 0.5f, 0.5f), "still");
    CHECK(mm_moved(0.5f + 0.0003f, 0.5f, 0.5f, 0.5f) && mm_moved(0.5f, 0.5f, -1, -1), "moved; the first look moves");

    // clicks: a press counts once; a press given while no menu takes the mouse (synced there) is no click of the next
    MmClicks c = {{0, 0}};
    int cl, cr;
    mm_clicks_take(&c, 1, 0, &cl, &cr);
    CHECK(cl && !cr, "a left press is a click");
    mm_clicks_take(&c, 1, 0, &cl, &cr);
    CHECK(!cl && !cr, "the same press is not a click again");
    mm_clicks_take(&c, 3, 1, &cl, &cr);
    CHECK(cl && cr, "two quick left presses between looks are one click, a right press another");
    mm_clicks_sync(&c, 4, 1);   // between menus: a quick second click lands on the fade, where no menu takes the mouse
    mm_clicks_take(&c, 4, 1, &cl, &cr);
    CHECK(!cl && !cr, "a click given between two menus is not a click of the next menu");

    // the queue: A and B wait one at most, the wheel three; a turn the other way drops the rest; a drop empties it
    volatile int32_t q[MM_PRESSES] = {0};
    for (int i = 0; i < 4; i++) mm_queue(q, MM_B, 1);   // four quick right clicks on the pause menu
    CHECK(mm_queue_take(q) == MM_B && mm_queue_take(q) == -1, "four quick right clicks are one B, none left for the play");
    mm_queue(q, MM_A, 1), mm_queue(q, MM_A, 1);
    CHECK(mm_queue_take(q) == MM_A && mm_queue_take(q) == -1, "two quick left clicks are one A");
    mm_queue(q, MM_DOWN, 5);
    CHECK(q[MM_DOWN] == 3, "the wheel queues three at most (%d)", (int)q[MM_DOWN]);
    mm_queue(q, MM_UP, 1);
    CHECK(q[MM_DOWN] == 0 && q[MM_UP] == 1, "a turn the other way drops the rest");
    mm_queue(q, MM_B, 1), mm_queue(q, MM_A, 1), mm_queue(q, MM_UP, 2);
    mm_queue_drop(q);   // the menu goes (a B closed it)
    CHECK(mm_queue_take(q) == -1, "nothing queued reaches the play after the menu goes");
    mm_queue(q, MM_UP, 1), mm_queue(q, MM_A, 1);
    CHECK(mm_queue_take(q) == MM_A && mm_queue_take(q) == MM_UP && mm_queue_take(q) == -1, "A goes before the wheel");

    if (fails) { printf("menu mouse: %d failed\n", fails); return 1; }
    printf("menu mouse: ok\n");
    return 0;
}
