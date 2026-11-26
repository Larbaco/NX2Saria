#include <windows.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "input.h"
#include "input_win.h"
#include "input_map.h"
#include "input_wsys.h"
#include "nvn_api.h"
#include "ui/menu.h"
#include "vk.h"
#include "platform/platform.h"
#include "window_idle.h"

static int key_vk(int key) {
    if (key >= KEY_A && key <= KEY_Z) return 'A' + (key - KEY_A);
    if (key >= KEY_0 && key <= KEY_9) return '0' + (key - KEY_0);
    if (key >= KEY_NUM0 && key <= KEY_NUM9) return VK_NUMPAD0 + (key - KEY_NUM0);
    switch (key) {
    case KEY_SPACE: return VK_SPACE;
    case KEY_TAB: return VK_TAB;
    case KEY_LSHIFT: return VK_LSHIFT;
    case KEY_LCTRL: return VK_LCONTROL;
    case KEY_UP: return VK_UP;
    case KEY_DOWN: return VK_DOWN;
    case KEY_LEFT: return VK_LEFT;
    case KEY_RIGHT: return VK_RIGHT;
    case KEY_MOUSE_L: return GetSystemMetrics(SM_SWAPBUTTON) ? VK_RBUTTON : VK_LBUTTON;
    case KEY_MOUSE_R: return GetSystemMetrics(SM_SWAPBUTTON) ? VK_LBUTTON : VK_RBUTTON;
    case KEY_MOUSE_M: return VK_MBUTTON;
    case KEY_MOUSE_X1: return VK_XBUTTON1;
    case KEY_MOUSE_X2: return VK_XBUTTON2;
    case KEY_ENTER: return VK_RETURN;
    case KEY_BACKSPACE: return VK_BACK;
    case KEY_RSHIFT: return VK_RSHIFT;
    case KEY_RCTRL: return VK_RCONTROL;
    case KEY_LALT: return VK_LMENU;
    case KEY_CAPSLOCK: return VK_CAPITAL;
    case KEY_INSERT: return VK_INSERT;
    case KEY_DELETE: return VK_DELETE;
    case KEY_HOME: return VK_HOME;
    case KEY_END: return VK_END;
    case KEY_PAGEUP: return VK_PRIOR;
    case KEY_PAGEDOWN: return VK_NEXT;
    case KEY_F2: return VK_F2;
    case KEY_F3: return VK_F3;
    case KEY_F4: return VK_F4;
    case KEY_F5: return VK_F5;
    case KEY_F6: return VK_F6;
    case KEY_F7: return VK_F7;
    case KEY_F9: return VK_F9;
    case KEY_F12: return VK_F12;
    case KEY_NUM_ADD: return VK_ADD;
    case KEY_NUM_SUB: return VK_SUBTRACT;
    case KEY_NUM_MUL: return VK_MULTIPLY;
    case KEY_NUM_DIV: return VK_DIVIDE;
    case KEY_NUM_DECIMAL: return VK_DECIMAL;
    case KEY_MINUS: return VK_OEM_MINUS;
    case KEY_EQUALS: return VK_OEM_PLUS;
    case KEY_LBRACKET: return VK_OEM_4;
    case KEY_RBRACKET: return VK_OEM_6;
    case KEY_SEMICOLON: return VK_OEM_1;
    case KEY_APOSTROPHE: return VK_OEM_7;
    case KEY_COMMA: return VK_OEM_COMMA;
    case KEY_PERIOD: return VK_OEM_PERIOD;
    case KEY_SLASH: return VK_OEM_2;
    case KEY_BACKSLASH: return VK_OEM_5;
    case KEY_GRAVE: return VK_OEM_3;
    }
    return 0;
}

uint32_t input_key_char(int key) {
    if (key < KEY_MINUS || key > KEY_GRAVE) return 0;
    return MapVirtualKeyW((UINT)key_vk(key), MAPVK_VK_TO_CHAR) & 0x7fffffffu;
}

static volatile LONG mouse_dx, mouse_dy;
static volatile LONG mouse_travel;
static volatile LONG presses[2];
static volatile LONG wheel_delta;
static volatile LONG cur_captured;
static volatile LONG cur_active;
enum { CUR_REL = 1, CUR_ESC = 2 };
static volatile LONG cur_rel;
static void cur_rel_update(LONG (*f)(LONG)) {
    LONG o, n;
    do { o = cur_rel; n = f(o); } while (n != o && InterlockedCompareExchange(&cur_rel, n, o) != o);
}
static LONG rel_esc(LONG o) { return o & CUR_REL ? o : CUR_REL | CUR_ESC; }
static LONG rel_title(LONG o) { (void)o; return CUR_REL; }
static LONG rel_clear(LONG o) { (void)o; return 0; }
static LONG rel_menu_closed(LONG o) { return o & CUR_ESC ? 0 : o; }
static volatile LONG cur_suspend;
static SRWLOCK cur_lock = SRWLOCK_INIT;
static void cursor_update(HWND h) {
    AcquireSRWLockExclusive(&cur_lock);
    int want = h && cur_active && !(cur_rel & CUR_REL) && !cur_suspend && input_cursor_allowed() &&
               GetForegroundWindow() == h && !IsIconic(h);
    RECT r;
    POINT tl = {0, 0};
    if (want && GetClientRect(h, &r) && ClientToScreen(h, &tl)) {
        RECT clip = {tl.x, tl.y, tl.x + r.right, tl.y + r.bottom};
        ClipCursor(&clip);
        if (!cur_captured) { InterlockedExchange(&mouse_dx, 0); InterlockedExchange(&mouse_dy, 0); }
        if (!InterlockedExchange(&cur_captured, 1)) {
            static volatile LONG lines;
            if (InterlockedIncrement(&lines) <= 40) rt_log("input: the mouse captured for the camera (confined and hidden)");
        }
    } else {
        if (cur_captured) ClipCursor(NULL);
        if (InterlockedExchange(&cur_captured, 0)) {
            static volatile LONG lines;
            if (InterlockedIncrement(&lines) <= 40) rt_log("input: the mouse released (the cursor shows)");
        }
    }
    ReleaseSRWLockExclusive(&cur_lock);
}

int input_cursor_released(void) { return (cur_rel & CUR_REL) != 0; }

enum { CURSOR_IDLE_TIMER = 0x4e32, CURSOR_IDLE_TICK_MS = 200 };
static CursorIdle cursor_idle;
static int cursor_idle_hidden, cursor_idle_on;
static unsigned cursor_idle_seen = ~0u;

static void cursor_idle_check(HWND h) {
    unsigned ver = cfg_version();
    if (ver != cursor_idle_seen) cursor_idle_seen = ver, cursor_idle_on = cfg_bool("NX2SARIA_CURSOR_HIDE");
    POINT p = {0, 0};
    RECT r;
    int over = 0;
    if (!cur_captured && GetCursorPos(&p) && WindowFromPoint(p) == h && ScreenToClient(h, &p) && GetClientRect(h, &r))
        over = p.x >= r.left && p.y >= r.top && p.x < r.right && p.y < r.bottom;
    int hide = cursor_idle_step(&cursor_idle, cursor_idle_on, over, (float)p.x, (float)p.y, plat_now_ns());
    if (hide == cursor_idle_hidden) return;
    cursor_idle_hidden = hide;
    if (!hide) return;
    SetCursor(NULL);
    if (cursor_idle.hides == 1)
        rt_log("input: cursor_hide: the cursor hid after %.0f s still over the window (the first time this session; a "
               "move shows it again)", CURSOR_IDLE_NS / 1e9);
}

void input_cursor_suspend(int on) {
    InterlockedExchange(&cur_suspend, on != 0);
    cursor_update(gfx_window_handle());
}

int input_window_msg(HWND h, UINT m, WPARAM w, LPARAM l, LRESULT *res) {
    switch (m) {
    case WM_CREATE: {
        RAWINPUTDEVICE rid = {.usUsagePage = 1, .usUsage = 2, .dwFlags = 0, .hwndTarget = h};
        if (!RegisterRawInputDevices(&rid, 1, sizeof rid)) rt_log("input: raw mouse registration failed (%lu)", GetLastError());
        if (!SetTimer(h, CURSOR_IDLE_TIMER, CURSOR_IDLE_TICK_MS, NULL))
            rt_log("input: cursor_hide: no timer (%lu), the cursor stays shown", GetLastError());
        break;
    }
    case WM_INPUT: {
        RAWINPUT ri;
        UINT sz = sizeof ri;
        if (GetRawInputData((HRAWINPUT)l, RID_INPUT, &ri, &sz, sizeof(RAWINPUTHEADER)) != (UINT)-1 &&
            ri.header.dwType == RIM_TYPEMOUSE && !(ri.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) {
            LONG x = ri.data.mouse.lLastX, y = ri.data.mouse.lLastY;
            InterlockedAdd(&mouse_travel, (x < 0 ? -x : x) + (y < 0 ? -y : y));
            if (cur_captured) {
                InterlockedAdd(&mouse_dx, x);
                InterlockedAdd(&mouse_dy, y);
            }
        }
        break;
    }
    case WM_SETCURSOR:
        if (HIWORD(l) == WM_MOUSEMOVE) cursor_idle_check(h);
        if ((cur_captured || cursor_idle_hidden) && LOWORD(l) == HTCLIENT) { SetCursor(NULL); *res = TRUE; return 1; }
        break;
    case WM_TIMER:
        if (w != CURSOR_IDLE_TIMER) break;
        cursor_idle_check(h);
        *res = 0;
        return 1;
    case WM_MOUSEWHEEL:
        InterlockedAdd(&wheel_delta, GET_WHEEL_DELTA_WPARAM(w));
        break;
    case WM_SYSCOMMAND:
        if ((w & 0xFFF0) == SC_KEYMENU && l == 0) { *res = 0; return 1; }
        break;
    case WM_KEYDOWN:
        if (w == VK_ESCAPE && !(cur_rel & CUR_REL)) { cur_rel_update(rel_esc); cursor_update(h); }
        break;
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN:
        if (m != WM_MBUTTONDOWN) InterlockedIncrement(&presses[m == WM_RBUTTONDOWN]);
        if (cur_rel & CUR_REL) { cur_rel_update(rel_clear); cursor_update(h); }
        break;
    case WM_MOUSEACTIVATE:
        if (LOWORD(l) != HTCLIENT) cur_rel_update(rel_title);
        break;
    case WM_ACTIVATEAPP:
        if (!w) cur_rel_update(rel_clear);
        InterlockedExchange(&cur_active, w != 0);
        cursor_update(h);
        break;
    case WM_SETFOCUS: case WM_KILLFOCUS: case WM_SIZE: case WM_MOVE:
        cursor_update(h);
        break;
    case WM_ENTERSIZEMOVE:
        input_cursor_suspend(1);
        break;
    case WM_EXITSIZEMOVE:
        input_cursor_suspend(0);
        break;
    case WM_CLOSE:
        InterlockedExchange(&cur_active, 0);
        cursor_update(h);
        break;
    }
    return 0;
}

static int key_down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

int input_flight_key(void) {
    HWND w = gfx_window_handle();
    return w && w == GetForegroundWindow() && key_down(VK_F8);
}

int input_dump_key(void) { return key_down(VK_INSERT); }

int ikb_foreground(void) {
    HWND w = gfx_window_handle();
    return w && w == GetForegroundWindow();
}

int ikb_down(int key) {
    int vk = key_vk(key);
    return vk && key_down(vk);
}

int ikb_escape(void) { return key_down(VK_ESCAPE); }

int ikb_captured(void) { return cur_captured != 0; }

int ikb_pointer(float *nx, float *ny) {
    HWND h = gfx_window_handle();
    RECT r;
    POINT p;
    if (!h || GetForegroundWindow() != h || !GetClientRect(h, &r) || r.right < 1 || r.bottom < 1 || !GetCursorPos(&p) ||
        !ScreenToClient(h, &p))
        return 0;
    *nx = (float)p.x / (float)r.right, *ny = (float)p.y / (float)r.bottom;
    return p.x >= 0 && p.y >= 0 && p.x < r.right && p.y < r.bottom;
}

void ikb_pointer_set(float nx, float ny) {
    HWND h = gfx_window_handle();
    RECT r;
    if (!h || GetForegroundWindow() != h || !GetClientRect(h, &r)) return;
    POINT p = {(LONG)(nx * r.right), (LONG)(ny * r.bottom)};
    if (ClientToScreen(h, &p)) SetCursorPos(p.x, p.y);
}

uint32_t ikb_mouse_travel(void) { return (uint32_t)mouse_travel; }
uint32_t ikb_mouse_presses(int right) { return (uint32_t)presses[right != 0]; }

int32_t ikb_wheel_take(void) {
    LONG o, n;
    do { o = wheel_delta; n = o % WHEEL_DELTA; } while (InterlockedCompareExchange(&wheel_delta, n, o) != o);
    return (int32_t)((o - n) / WHEEL_DELTA);
}

void ikb_mouse_take(int32_t *dx, int32_t *dy) {
    *dx = InterlockedExchange(&mouse_dx, 0);
    *dy = InterlockedExchange(&mouse_dy, 0);
}

void ikb_cursor_update(void) { cursor_update(gfx_window_handle()); }

void ikb_menu_closed(void) { cur_rel_update(rel_menu_closed); }
