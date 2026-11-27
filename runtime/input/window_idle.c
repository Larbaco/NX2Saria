#include "window_idle.h"

int cursor_idle_step(CursorIdle *s, int enabled, int over, float x, float y, uint64_t now_ns) {
    int moved = !s->known || x != s->x || y != s->y;
    s->known = enabled && over, s->x = x, s->y = y;
    if (!enabled || !over || moved) {
        s->still_since = now_ns;
        s->hidden = 0;
        return 0;
    }
    if (!s->hidden && now_ns - s->still_since >= CURSOR_IDLE_NS) {
        s->hidden = 1;
        s->hides++;
    }
    return s->hidden;
}

int focus_mute_step(FocusMute *s, int enabled, int focused) {
    int mute = enabled && !focused;
    if (mute && !s->muted) s->mutes++;
    s->muted = mute;
    return mute;
}
