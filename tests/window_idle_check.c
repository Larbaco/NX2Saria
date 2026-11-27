#include <stdint.h>
#include <stdio.h>
#include "window_idle.h"

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL: " __VA_ARGS__); putchar('\n'); fails++; } } while (0)

#define MS 1000000ull

static void cursor_hides_after_three_still_seconds(void) {
    CursorIdle s = {0};
    uint64_t t = 5000 * MS;
    CHECK(!cursor_idle_step(&s, 1, 1, 10, 20, t), "the first look shows the cursor");
    CHECK(!cursor_idle_step(&s, 1, 1, 10, 20, t + 2999 * MS), "2.999 s still: shown");
    CHECK(cursor_idle_step(&s, 1, 1, 10, 20, t + 3000 * MS), "3 s still: hidden");
    CHECK(cursor_idle_step(&s, 1, 1, 10, 20, t + 9000 * MS) && s.hides == 1, "still hidden, one hide (%u)", s.hides);
    CHECK(!cursor_idle_step(&s, 1, 1, 11, 20, t + 9001 * MS), "a move shows it at once");
    CHECK(!cursor_idle_step(&s, 1, 1, 11, 20, t + 11000 * MS), "the 3 s count from the move");
    CHECK(cursor_idle_step(&s, 1, 1, 11, 20, t + 12001 * MS) && s.hides == 2, "hidden again 3 s after the move (%u)",
          s.hides);
}

static void cursor_off_the_window_or_off(void) {
    CursorIdle s = {0};
    uint64_t t = 0;
    for (int i = 0; i <= 10; i++) CHECK(!cursor_idle_step(&s, 1, 0, 0, 0, t + i * 1000 * MS), "not over the window, %d s", i);
    CHECK(!cursor_idle_step(&s, 1, 1, 0, 0, t + 10500 * MS), "back over the window: the 3 s start again");
    CHECK(!cursor_idle_step(&s, 1, 1, 0, 0, t + 13000 * MS), "2.5 s over the window: shown");
    CHECK(cursor_idle_step(&s, 1, 1, 0, 0, t + 13500 * MS), "3 s over the window: hidden");
    CHECK(!cursor_idle_step(&s, 1, 0, 0, 0, t + 13600 * MS), "captured or gone: shown, the capture hides it itself");
    CursorIdle off = {0};
    for (int i = 0; i <= 10; i++) CHECK(!cursor_idle_step(&off, 0, 1, 0, 0, i * 1000 * MS), "cursor_hide = 0, %d s", i);
    CHECK(off.hides == 0, "cursor_hide = 0 never hides (%u)", off.hides);
    CHECK(!cursor_idle_step(&s, 1, 1, 0, 0, t + 13700 * MS), "over the window again");
    CHECK(cursor_idle_step(&s, 1, 1, 0, 0, t + 16700 * MS), "hidden 3 s later");
    CHECK(!cursor_idle_step(&s, 0, 1, 0, 0, t + 16800 * MS), "switched off while hidden: shown at once");
}

static void mute_follows_the_focus(void) {
    FocusMute m = {0};
    CHECK(!focus_mute_step(&m, 1, 1), "focused: sound");
    CHECK(focus_mute_step(&m, 1, 0) && m.mutes == 1, "focus lost: muted, once (%u)", m.mutes);
    CHECK(focus_mute_step(&m, 1, 0) && m.mutes == 1, "still without focus: one mute (%u)", m.mutes);
    CHECK(!focus_mute_step(&m, 1, 1), "focus back: sound");
    CHECK(focus_mute_step(&m, 1, 0) && m.mutes == 2, "lost again: the second mute (%u)", m.mutes);
    CHECK(!focus_mute_step(&m, 0, 0), "background_mute = 0 while unfocused: sound at once");
    FocusMute off = {0};
    CHECK(!focus_mute_step(&off, 0, 0) && !focus_mute_step(&off, 0, 1) && off.mutes == 0, "background_mute = 0 never mutes");
}

int main(void) {
    cursor_hides_after_three_still_seconds();
    cursor_off_the_window_or_off();
    mute_follows_the_focus();
    if (!fails) puts("window idle: the cursor hides after 3 s still over the window and shows on a move; the sound is muted "
                     "without the focus while background_mute is on");
    return fails != 0;
}
