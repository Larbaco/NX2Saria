#include "ui/menu.h"

static int g_text_on;

void menu_text_input_follow(int want) {
    want = want != 0;
    if (want == g_text_on) return;
    g_text_on = want;
    gfx_window_text_input(want);
}
