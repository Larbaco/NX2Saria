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
    names();
    printf("pause_tab_check: %s\n", fails ? "FAILED" : "passed");
    return fails != 0;
}
