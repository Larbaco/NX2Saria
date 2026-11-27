#include <stdio.h>
#include <string.h>

#include "pause_tab_core.h"

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL: " __VA_ARGS__); putchar('\n'); fails++; } } while (0)

static void press(void) {
    PtPress s = {0, 0};
    int opened;
    CHECK(!pt_press(&s, 1, 0, &opened) && !opened, "R up takes nothing");
    CHECK(!pt_press(&s, 0, 1, &opened) && !opened, "R pressed while not armed goes to the game");
    CHECK(!pt_press(&s, 1, 1, &opened) && !opened, "R held from before the arming goes on to the game");
    CHECK(!pt_press(&s, 1, 0, &opened), "R let go");
    CHECK(pt_press(&s, 1, 1, &opened) && opened, "a new R while armed opens the tab and is taken");
    CHECK(pt_press(&s, 0, 1, &opened) && !opened, "the R that opened stays taken while held, armed or not");
    CHECK(!pt_press(&s, 0, 0, &opened) && !opened, "let go: nothing taken");
}

static void boxes(void) {
    PtBox items = {30, -112, 430, 112}, system = {490, -112, 890, 112}, tab;
    CHECK(pt_tab_box(&items, &system, &tab), "a tab box from the game's two last categories");
    CHECK(fabsf((tab.x0 + tab.x1) * 0.5f - (690 + 460 * PT_TAB_STEP)) < 0.01f, "the tab's centre past the R icon: %.1f",
          (double)((tab.x0 + tab.x1) * 0.5f));
    CHECK(fabsf(tab.x1 - tab.x0 - 260) < 0.01f && tab.y0 == -112 && tab.y1 == 112, "the tab 0.65 of a category wide");
    CHECK(tab.x0 > 1044, "the tab clear of the R icon (936..1044): %.1f", (double)tab.x0);
    CHECK(tab.x1 < 1324, "the tab clear of the rupee icon (1324..1468): %.1f", (double)tab.x1);
    PtBox wide_items = {30 + 640, -112, 430 + 640, 112}, wide_system = {490 + 640, -112, 890 + 640, 112};
    CHECK(pt_tab_box(&wide_items, &wide_system, &tab) && (tab.x0 + tab.x1) * 0.5f > 890 + 640, "spread categories");
    CHECK(!pt_tab_box(&system, &items, &tab), "categories in the wrong order give no box");
    PtBox before, last;
    pt_header_boxes(&before, &last);
    CHECK(pt_tab_box(&before, &last, &tab) && fabsf(tab.x0 - 1054.5f) < 0.01f && fabsf(tab.x1 - 1314.5f) < 0.01f &&
              tab.y0 == 888 && tab.y1 == 1112,
          "the tab from the layout's own categories: %.0f..%.0f", (double)tab.x0, (double)tab.x1);
    PtBox flat = {10, 0, 10, 0};
    CHECK(!pt_tab_box(&items, &flat, &tab), "an empty category gives no box");
}

static void settled(void) {
    CHECK(pt_settled(0.0f, 0.0f, 2.0f), "the System page in the middle and still");
    CHECK(!pt_settled(0.0f, 30.0f, 2.0f), "still moving");
    CHECK(!pt_settled(-1260.0f, -1260.0f, 2.0f), "another page in the middle");
}

static void trees(void) {
    uint16_t system[16] = {0xffff, 0, 1, 2, 2, 4, 4, 4, 2, 2, 2, 2, 2, 0, 0, 0};
    CHECK(pt_tree_ok(system, 16, PT_SYSTEM_TREE, (int)(sizeof PT_SYSTEM_TREE / sizeof *PT_SYSTEM_TREE)),
          "PaSystemMenu_00's panes as the game's file has them");
    system[2] = 0;
    CHECK(!pt_tree_ok(system, 16, PT_SYSTEM_TREE, (int)(sizeof PT_SYSTEM_TREE / sizeof *PT_SYSTEM_TREE)),
          "another shape is refused");
    uint16_t cat[16] = {0xffff, 0, 1, 2, 3, 3, 1, 6, 7, 7};
    CHECK(pt_tree_ok(cat, 16, PT_CATEGORY_TREE, (int)(sizeof PT_CATEGORY_TREE / sizeof *PT_CATEGORY_TREE)),
          "PaMenuCategory_00's panes");
    CHECK(!pt_tree_ok(cat, 4, PT_CATEGORY_TREE, (int)(sizeof PT_CATEGORY_TREE / sizeof *PT_CATEGORY_TREE)),
          "too few panes read");
}

static void five(void) {
    float game[PT_MOVED], five_x[PT_MOVED], odd[PT_MOVED];
    memcpy(game, PT_GAME_X, sizeof game);
    memcpy(five_x, PT_FIVE_X, sizeof five_x);
    memcpy(odd, PT_GAME_X, sizeof odd);
    odd[5] = 1040.0f;
    CHECK(pt_spacing(game) == PT_AS_GAME, "the game's places");
    CHECK(pt_spacing(five_x) == PT_AS_FIVE, "the five places");
    CHECK(pt_spacing(odd) == PT_AS_OTHER, "one pane elsewhere: neither");
    for (int i = 1; i < 4; i++)
        CHECK(PT_FIVE_X[i] - PT_FIVE_X[i - 1] == 420.0f, "the categories 420 apart");
    PtBox last = {230, 888, 630, 1112}, tab;
    pt_five_box(&last, &tab);
    CHECK(tab.x0 == 670 && tab.x1 == 1010 && tab.y0 == 888 && tab.y1 == 1112, "the fifth place: %.0f..%.0f",
          (double)tab.x0, (double)tab.x1);
    CHECK(tab.x0 > PT_FIVE_X[3] + 200 && tab.x1 < PT_FIVE_X[5] - 54, "the fifth place between System and the moved R");
    CHECK(pt_five_tab_at(-840, 1000) == 0 && pt_five_tab_at(-420, 1000) == 1 && pt_five_tab_at(0, 1000) == 2 &&
              pt_five_tab_at(420, 1000) == 3 && pt_five_tab_at(840, 1000) == PT_TAB_SETTINGS,
          "a click on each of the five");
    CHECK(pt_five_tab_at(-1100, 1000) == 4 && pt_five_tab_at(1100, 1000) == 5, "a click on L and R");
    CHECK(pt_five_tab_at(0, 700) == -1 && pt_five_tab_at(1300, 1000) == -1, "off the row");
}

static void trans(void) {
    float panes[PT_MOVED][44];
    const float *rows[PT_MOVED];
    for (int i = 0; i < PT_MOVED; i++) {
        for (int k = 0; k < 44; k++) panes[i][k] = 1.0f;
        panes[i][19] = PT_GAME_X[i], panes[i][20] = PT_GAME_Y[i] + 1000.0f;
        panes[i][26] = PT_GAME_X[i], panes[i][27] = PT_GAME_Y[i];
        rows[i] = panes[i];
    }
    CHECK(pt_find_trans(rows, 44, 10, 22) == 26, "the local position found, the matrix's own skipped");
    CHECK(pt_find_trans(rows, 44, 0, 0) == 26, "a matrix x without the local y is not taken");
    panes[5][27] = 3.0f;
    CHECK(pt_find_trans(rows, 44, 10, 22) == -1, "R's y off: not found");
    panes[5][27] = PT_GAME_Y[5];
    for (int i = 0; i < PT_MOVED; i++) panes[i][34] = PT_GAME_X[i], panes[i][35] = PT_GAME_Y[i];
    CHECK(pt_find_trans(rows, 44, 10, 22) == -2, "two places with the game's values: none taken");
}

static void alpha(void) {
    float a[4] = {1.0f, 1.0f, 0.5f, 0.4f};
    uint32_t f[4] = {1, 1 | 8, 1 | 8, 1};
    CHECK(fabsf(pt_chain_alpha(a, f, 4) - 0.5f) < 1e-6f, "the parents that pass their alpha on multiply it: %.3f",
          (double)pt_chain_alpha(a, f, 4));
    f[3] = 1 | 8;
    CHECK(fabsf(pt_chain_alpha(a, f, 4) - 0.2f) < 1e-6f, "the header fading in at 0.4 takes the name with it");
    f[2] = 8;
    CHECK(pt_chain_alpha(a, f, 4) == 0.0f, "a parent not shown hides it");
    f[2] = 1 | 8, a[0] = 3.0f, a[1] = -1.0f;
    CHECK(pt_chain_alpha(a, f, 2) == 0.0f, "out of range: clamped");
    CHECK(pt_chain_alpha(a, f, 0) == 0.0f, "no pane: nothing");
}

static void page(void) {
    PtBox b;
    pt_page_box(&b, 1920.0f, 1);
    CHECK(b.x0 == -1560 && b.x1 == 1560 && b.y0 == -880 && b.y1 == 410, "the page at 16:9");
    pt_page_box(&b, 2560.0f, 1);
    CHECK(b.x0 == -2200 && b.x1 == 2200, "the page at 21:9 with the wide menus spread as the game's pages are");
    pt_page_box(&b, 2560.0f, 0);
    CHECK(b.x0 == -1560 && b.x1 == 1560, "the page at 21:9 with the menus in the middle");
}

static void names(void) {
    int known;
    CHECK(pt_name_kind(NULL, &known) == PT_NAME_PRODUCT && known, "unset: the product's name");
    CHECK(pt_name_kind("product", &known) == PT_NAME_PRODUCT && known, "product");
    CHECK(pt_name_kind("settings", &known) == PT_NAME_SETTINGS && known, "settings");
    CHECK(pt_name_kind("Settings", &known) == PT_NAME_PRODUCT && !known, "an unknown value: the product's, said");
}

int main(void) {
    press();
    boxes();
    settled();
    trees();
    five();
    trans();
    alpha();
    page();
    names();
    printf("pause_tab_check: %s\n", fails ? "FAILED" : "passed");
    return fails != 0;
}
