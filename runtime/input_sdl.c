// The SDL3 back end of the keyboard and the mouse (input_wsys.h; input.c merges them with the gamepad): SDL scancodes to
// the key ids of input_map.h, the mouse camera's relative motion and the cursor capture (SDL's relative mouse mode).
// Key and button state comes from the window's events (window thread) and is read by any thread. Keys and their map:
// docs/controls.md.
//
// The capture follows the Win32 back end's rules (input_win.c): captured while the window has the focus, the mouse
// camera is on and no text entry or menu is open; Esc releases it until a click in the window (and closing the menu
// opened by that Esc gives it back); losing the focus releases it until the window has the focus again.
#include <string.h>

#include <SDL3/SDL.h>

#include "host.h"
#include "input.h"
#include "input_map.h"
#include "input_sdl.h"
#include "input_wsys.h"
#include "platform/platform.h"

static SDL_Window *win;
static volatile uint8_t key_held[KEY_COUNT];
static volatile int32_t esc_held, f8_held, insert_held, focused;
static volatile int32_t mouse_dx, mouse_dy;   // relative motion since the last ikb_mouse_take, whole counts
static float rem_x, rem_y;                    // window thread: the fractions SDL reported beyond them
static volatile int32_t captured;             // relative mouse mode is on
enum { CUR_REL = 1, CUR_ESC = 2 };            // as input_win.c: released by Esc or a click elsewhere; by Esc
static volatile int32_t cur_rel, cur_suspend, cur_pending;
static uint32_t wake_event;                   // SDL_RegisterEvents: wakes the window thread for a cursor change
// The pointer outside relative mode (menu_mouse.c points at the game's menus with it): fractions of the window, 0..1 from
// its top left, as float bits so any thread reads them whole; inside: the pointer is over the window
static volatile uint32_t ptr_x, ptr_y;
static volatile int32_t ptr_inside, ptr_warp;   // ptr_warp: ikb_pointer_set asked the window thread to move it there
static volatile uint32_t warp_x, warp_y;
static volatile uint32_t mouse_travel;           // counts the mouse moved in total, captured or not (|dx| + |dy|)
static volatile int32_t wheel_steps;             // wheel notches not taken yet, up positive (ikb_wheel_take)
static float wheel_rem;                          // window thread: the part of a notch a smooth wheel left over
static int warp_motion;                          // window thread: the next motion event is a warp's
static uint32_t fbits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static float bitsf(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

// SDL scancode of a key id, 0 for an id without a key (the mouse buttons).
static SDL_Scancode key_sc(int key) {
    if (key >= KEY_A && key <= KEY_Z) return (SDL_Scancode)(SDL_SCANCODE_A + (key - KEY_A));
    if (key >= KEY_1 && key <= KEY_9) return (SDL_Scancode)(SDL_SCANCODE_1 + (key - KEY_1));
    if (key >= KEY_NUM1 && key <= KEY_NUM9) return (SDL_Scancode)(SDL_SCANCODE_KP_1 + (key - KEY_NUM1));
    switch (key) {
    case KEY_0: return SDL_SCANCODE_0;
    case KEY_NUM0: return SDL_SCANCODE_KP_0;
    case KEY_SPACE: return SDL_SCANCODE_SPACE;
    case KEY_TAB: return SDL_SCANCODE_TAB;
    case KEY_LSHIFT: return SDL_SCANCODE_LSHIFT;
    case KEY_LCTRL: return SDL_SCANCODE_LCTRL;
    case KEY_UP: return SDL_SCANCODE_UP;
    case KEY_DOWN: return SDL_SCANCODE_DOWN;
    case KEY_LEFT: return SDL_SCANCODE_LEFT;
    case KEY_RIGHT: return SDL_SCANCODE_RIGHT;
    case KEY_ENTER: return SDL_SCANCODE_RETURN;
    case KEY_BACKSPACE: return SDL_SCANCODE_BACKSPACE;
    case KEY_RSHIFT: return SDL_SCANCODE_RSHIFT;
    case KEY_RCTRL: return SDL_SCANCODE_RCTRL;
    case KEY_LALT: return SDL_SCANCODE_LALT;
    case KEY_CAPSLOCK: return SDL_SCANCODE_CAPSLOCK;
    case KEY_INSERT: return SDL_SCANCODE_INSERT;
    case KEY_DELETE: return SDL_SCANCODE_DELETE;
    case KEY_HOME: return SDL_SCANCODE_HOME;
    case KEY_END: return SDL_SCANCODE_END;
    case KEY_PAGEUP: return SDL_SCANCODE_PAGEUP;
    case KEY_PAGEDOWN: return SDL_SCANCODE_PAGEDOWN;
    case KEY_F2: return SDL_SCANCODE_F2;
    case KEY_F3: return SDL_SCANCODE_F3;
    case KEY_F4: return SDL_SCANCODE_F4;
    case KEY_F5: return SDL_SCANCODE_F5;
    case KEY_F6: return SDL_SCANCODE_F6;
    case KEY_F7: return SDL_SCANCODE_F7;
    case KEY_F9: return SDL_SCANCODE_F9;
    case KEY_F12: return SDL_SCANCODE_F12;
    case KEY_NUM_ADD: return SDL_SCANCODE_KP_PLUS;
    case KEY_NUM_SUB: return SDL_SCANCODE_KP_MINUS;
    case KEY_NUM_MUL: return SDL_SCANCODE_KP_MULTIPLY;
    case KEY_NUM_DIV: return SDL_SCANCODE_KP_DIVIDE;
    case KEY_NUM_DECIMAL: return SDL_SCANCODE_KP_PERIOD;
    case KEY_MINUS: return SDL_SCANCODE_MINUS;
    case KEY_EQUALS: return SDL_SCANCODE_EQUALS;
    case KEY_LBRACKET: return SDL_SCANCODE_LEFTBRACKET;
    case KEY_RBRACKET: return SDL_SCANCODE_RIGHTBRACKET;
    case KEY_SEMICOLON: return SDL_SCANCODE_SEMICOLON;
    case KEY_APOSTROPHE: return SDL_SCANCODE_APOSTROPHE;
    case KEY_COMMA: return SDL_SCANCODE_COMMA;
    case KEY_PERIOD: return SDL_SCANCODE_PERIOD;
    case KEY_SLASH: return SDL_SCANCODE_SLASH;
    case KEY_BACKSLASH: return SDL_SCANCODE_BACKSLASH;
    case KEY_GRAVE: return SDL_SCANCODE_GRAVE;
    }
    return SDL_SCANCODE_UNKNOWN;
}

// The key id of a scancode (the inverse of key_sc, built on first use), 0 for none.
static int key_of_sc(SDL_Scancode sc) {
    static uint8_t map[SDL_SCANCODE_COUNT];
    static int built;
    if (!built) {
        for (int k = 1; k < KEY_COUNT; k++) {
            SDL_Scancode s = key_sc(k);
            if (s != SDL_SCANCODE_UNKNOWN) map[s] = (uint8_t)k;
        }
        built = 1;
    }
    return sc > 0 && sc < SDL_SCANCODE_COUNT ? map[sc] : 0;
}

static int mouse_key(uint8_t button) {
    switch (button) {
    case SDL_BUTTON_LEFT: return KEY_MOUSE_L;
    case SDL_BUTTON_RIGHT: return KEY_MOUSE_R;
    case SDL_BUTTON_MIDDLE: return KEY_MOUSE_M;
    case SDL_BUTTON_X1: return KEY_MOUSE_X1;
    case SDL_BUTTON_X2: return KEY_MOUSE_X2;
    }
    return 0;
}

uint32_t input_key_char(int key) {
    if (key < KEY_MINUS || key > KEY_GRAVE) return 0;
    SDL_Keycode k = SDL_GetKeyFromScancode(key_sc(key), SDL_KMOD_NONE, false);
    return k & SDLK_SCANCODE_MASK ? 0 : (uint32_t)k;   // a keycode with the mask bit names no character
}

int input_flight_key(void) { return focused && f8_held; }

int input_dump_key(void) { return focused && insert_held; }

// ---- the cursor (window thread)
// The pointer at window coordinates (x, y), in the window's units (points), as fractions of its size: the same
// fractions of the drawable in pixels, whatever the pixel density, so they compare with gfx_picture_rect's.
static void pointer_at(float x, float y) {
    int w = 0, h = 0;
    if (!win || !SDL_GetWindowSize(win, &w, &h) || w < 1 || h < 1) return;
    __atomic_store_n(&ptr_x, fbits(x / (float)w), __ATOMIC_RELAXED);
    __atomic_store_n(&ptr_y, fbits(y / (float)h), __ATOMIC_RELAXED);
}

// The pointer as SDL has it now (after the capture ends SDL puts it back where it chooses)
static void pointer_read(void) {
    float x, y;
    SDL_GetMouseState(&x, &y);
    pointer_at(x, y);
    plat_xchg32(&ptr_inside, SDL_GetMouseFocus() == win);
}

static void cursor_apply(void) {
    __atomic_store_n(&cur_pending, 0, __ATOMIC_RELAXED);
    if (!win) return;
    int want = focused && !(cur_rel & CUR_REL) && !cur_suspend && input_cursor_allowed() &&
               !(SDL_GetWindowFlags(win) & SDL_WINDOW_MINIMIZED);
    if (want != captured) {
        if (!SDL_SetWindowRelativeMouseMode(win, want)) {
            rt_log("input: relative mouse mode %s failed (%s)", want ? "on" : "off", SDL_GetError());
        } else {
            plat_xchg32(&mouse_dx, 0);
            plat_xchg32(&mouse_dy, 0);
            rem_x = rem_y = 0;
            plat_xchg32(&captured, want);
            if (!want) pointer_read();
        }
    }
    // A warp asked for (ikb_pointer_set) once the capture is off; SDL ignores or limits one on some systems (Wayland)
    if (!captured && plat_xchg32(&ptr_warp, 0)) {
        int w = 0, h = 0;
        if (SDL_GetWindowSize(win, &w, &h) && w > 0 && h > 0) {
            float x = bitsf(warp_x) * (float)w, y = bitsf(warp_y) * (float)h;
            SDL_WarpMouseInWindow(win, x, y);
            pointer_read();   // where it is now: a compositor may leave it where it was (Wayland)
            warp_motion = 1;   // the motion event the warp makes is not the player's
        }
    }
}

// Any thread: the window thread applies the change at its next event.
void ikb_cursor_update(void) {
    plat_xchg32(&cur_pending, 1);
    if (wake_event) {
        SDL_Event e = {.user = {.type = wake_event}};
        SDL_PushEvent(&e);
    }
}

void input_cursor_suspend(int on) {
    plat_xchg32(&cur_suspend, on != 0);
    ikb_cursor_update();
}

int input_cursor_released(void) { return (cur_rel & CUR_REL) != 0; }

void ikb_menu_closed(void) {
    int32_t o = cur_rel;
    if (o & CUR_ESC) plat_cas32(&cur_rel, 0, o);
}

void input_sdl_window(SDL_Window *w) {
    win = w;
    wake_event = SDL_RegisterEvents(1);
    plat_xchg32(&focused, (SDL_GetWindowFlags(w) & SDL_WINDOW_INPUT_FOCUS) != 0);
    plat_xchg32(&ptr_inside, (SDL_GetWindowFlags(w) & SDL_WINDOW_MOUSE_FOCUS) != 0);
    cursor_apply();
}

void input_sdl_idle(void) {
    if (cur_pending) cursor_apply();
}

void input_sdl_event(const SDL_Event *e) {
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        int down = e->type == SDL_EVENT_KEY_DOWN;
        if (e->key.scancode == SDL_SCANCODE_ESCAPE) {
            plat_xchg32(&esc_held, down);
            // Esc is unbound in the game layout: it releases the cursor and opens the menu (ui/menu_sdl.cpp)
            if (down && !e->key.repeat && !(cur_rel & CUR_REL)) plat_xchg32(&cur_rel, CUR_REL | CUR_ESC);
        }
        if (e->key.scancode == SDL_SCANCODE_F8) plat_xchg32(&f8_held, down);
        if (e->key.scancode == SDL_SCANCODE_INSERT) plat_xchg32(&insert_held, down);
        int k = key_of_sc(e->key.scancode);
        if (k) key_held[k] = (uint8_t)down;
        break;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        int down = e->type == SDL_EVENT_MOUSE_BUTTON_DOWN;
        int k = mouse_key(e->button.button);
        if (k) key_held[k] = (uint8_t)down;
        if (down && (cur_rel & CUR_REL)) plat_xchg32(&cur_rel, 0);   // a click in the window captures again
        break;
    }
    case SDL_EVENT_MOUSE_MOTION: {
        // Travel counts the player's mouse only: not a warp's motion, not a touch screen's or a pen's
        int own = !warp_motion && e->motion.which != SDL_TOUCH_MOUSEID && e->motion.which != SDL_PEN_MOUSEID;
        warp_motion = 0;
        float ax = e->motion.xrel < 0 ? -e->motion.xrel : e->motion.xrel, ay = e->motion.yrel < 0 ? -e->motion.yrel : e->motion.yrel;
        if (own) __atomic_add_fetch(&mouse_travel, (uint32_t)(ax + ay + 0.5f), __ATOMIC_RELAXED);
        if (!captured) pointer_at(e->motion.x, e->motion.y), plat_xchg32(&ptr_inside, 1);
        if (captured) {
            rem_x += e->motion.xrel;
            rem_y += e->motion.yrel;
            int32_t dx = (int32_t)rem_x, dy = (int32_t)rem_y;
            rem_x -= (float)dx, rem_y -= (float)dy;
            if (dx) plat_add32(&mouse_dx, dx);
            if (dy) plat_add32(&mouse_dy, dy);
        }
        break;
    }
    case SDL_EVENT_MOUSE_WHEEL: {   // whole notches; a smooth wheel's fractions add up to them
        // In the direction the system scrolls (natural scrolling kept, as Windows' WM_MOUSEWHEEL has it), in front only
        if (!focused) break;
        wheel_rem += e->wheel.y;
        int32_t n = (int32_t)wheel_rem;
        wheel_rem -= (float)n;
        if (n) plat_add32(&wheel_steps, n);
        break;
    }
    case SDL_EVENT_WINDOW_MOUSE_ENTER:
        plat_xchg32(&ptr_inside, 1);
        break;
    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
        plat_xchg32(&ptr_inside, 0);
        break;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
        plat_xchg32(&focused, 1);
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        plat_xchg32(&focused, 0);
        plat_xchg32(&cur_rel, 0);   // coming back (Alt+Tab) captures again
        memset((void *)key_held, 0, sizeof key_held);   // the key-ups go elsewhere
        plat_xchg32(&esc_held, 0);
        plat_xchg32(&f8_held, 0);
        plat_xchg32(&insert_held, 0);
        break;
    }
    cursor_apply();
}

// ---- input_wsys.h
int ikb_foreground(void) { return focused != 0; }
int ikb_down(int key) { return key > 0 && key < KEY_COUNT && key_held[key]; }
int ikb_escape(void) { return esc_held != 0; }
int ikb_captured(void) { return captured != 0; }
int ikb_pointer(float *nx, float *ny) {
    if (!focused || captured) return 0;   // (in relative mode the system pointer is hidden and stays put)
    float x = bitsf(__atomic_load_n(&ptr_x, __ATOMIC_RELAXED)), y = bitsf(__atomic_load_n(&ptr_y, __ATOMIC_RELAXED));
    *nx = x, *ny = y;
    return ptr_inside && x >= 0 && y >= 0 && x < 1 && y < 1;
}
// Any thread: the window thread warps the pointer at its next event (SDL's video calls belong to it)
void ikb_pointer_set(float nx, float ny) {
    if (!focused) return;
    __atomic_store_n(&warp_x, fbits(nx), __ATOMIC_RELAXED);
    __atomic_store_n(&warp_y, fbits(ny), __ATOMIC_RELAXED);
    plat_xchg32(&ptr_warp, 1);
    ikb_cursor_update();
}
uint32_t ikb_mouse_travel(void) { return __atomic_load_n(&mouse_travel, __ATOMIC_RELAXED); }
int32_t ikb_wheel_take(void) { return plat_xchg32(&wheel_steps, 0); }

void ikb_mouse_take(int32_t *dx, int32_t *dy) {
    *dx = plat_xchg32(&mouse_dx, 0);
    *dy = plat_xchg32(&mouse_dy, 0);
}
