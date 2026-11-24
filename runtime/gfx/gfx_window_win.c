#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <imm.h>

#include "cfg.h"
#include "flight.h"
#include "fs.h"
#include "gfx.h"
#include "gfx_wsys.h"
#include "input.h"
#include "input_win.h"
#include "nvn_api.h"
#include "ui/menu.h"
#include "vk.h"
#include "vk_dlssg.h"
#include "platform/platform.h"
#include <vulkan/vulkan_win32.h>

static HWND g_hwnd;
static PlatEvent g_wnd_ready;
static int g_wnd_failed;
static int g_background;
static int g_fs_pending;
static volatile int32_t g_dpi = USER_DEFAULT_SCREEN_DPI;

HWND gfx_window_handle(void) { return g_hwnd; }

int gfx_window_vk_extensions(const char **out, int max) {
    if (max < 2) return 0;
    out[0] = VK_KHR_SURFACE_EXTENSION_NAME;
    out[1] = VK_KHR_WIN32_SURFACE_EXTENSION_NAME;
    return 2;
}

int gfx_window_vk_presentable(VkInstance inst, VkPhysicalDevice pd, uint32_t family) {
    (void)inst;
    return vkGetPhysicalDeviceWin32PresentationSupportKHR(pd, family) != VK_FALSE;
}

#define WND_STYLE WS_OVERLAPPEDWINDOW
#define WM_APP_SETTINGS (WM_APP + 1)
#define WM_APP_FULLSCREEN (WM_APP + 2)
#define WM_APP_TEXT_INPUT (WM_APP + 3)
#define WM_APP_PASTE (WM_APP + 4)

static WINDOWPLACEMENT g_saved_place = {sizeof g_saved_place};

static int is_fullscreen(HWND h) { return !(GetWindowLongW(h, GWL_STYLE) & WS_OVERLAPPEDWINDOW); }

#define cfg_fullscreen gfx_cfg_fullscreen
#define cfg_window_size gfx_cfg_window_size

unsigned gfx_window_dpi(void) { return (unsigned)plat_cas32(&g_dpi, 0, 0); }

static void outer_size_at(int cw, int ch, UINT dpi, int *ow, int *oh) {
    RECT r = {0, 0, cw, ch};
    AdjustWindowRectExForDpi(&r, WND_STYLE, FALSE, 0, dpi);
    *ow = r.right - r.left;
    *oh = r.bottom - r.top;
}

static void outer_size(HWND h, int cw, int ch, int *ow, int *oh) {
    outer_size_at(cw, ch, h ? GetDpiForWindow(h) : GetDpiForSystem(), ow, oh);
}

static void fit_work_area(int *cw, int *ch, const RECT *work, UINT dpi) {
    RECT prim;
    int ow, oh;
    if (!work) {
        if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &prim, 0)) return;
        work = &prim;
    }
    outer_size_at(*cw, *ch, dpi, &ow, &oh);
    int aw = (work->right - work->left) - (ow - *cw), ah = (work->bottom - work->top) - (oh - *ch);
    if (aw <= 0 || ah <= 0 || (*cw <= aw && *ch <= ah)) return;
    double k = (double)aw / *cw < (double)ah / *ch ? (double)aw / *cw : (double)ah / *ch;
    int w = (int)(*cw * k) & ~1, hh = (int)(*ch * k) & ~1;
    if (w < 640 || hh < 360) return;
    rt_log("gfx: window %dx%d does not fit the screen's work area; opening at %dx%d", *cw, *ch, w, hh);
    *cw = w;
    *ch = hh;
}

static void keep_awake(int on) {
    static int cur = -1;
    if (on == cur) return;
    cur = on;
    SetThreadExecutionState(on ? ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED : ES_CONTINUOUS);
}

static void toggle_fullscreen(HWND h) {
    LONG style = GetWindowLongW(h, GWL_STYLE);
    if (style & WS_OVERLAPPEDWINDOW) {
        MONITORINFO mi = {sizeof mi};
        if (!GetWindowPlacement(h, &g_saved_place) || !GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTOPRIMARY), &mi))
            return;
        SetWindowLongW(h, GWL_STYLE, (style & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
        SetWindowPos(h, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED | (g_background ? SWP_NOZORDER | SWP_NOACTIVATE : 0));
    } else {
        SetWindowLongW(h, GWL_STYLE, (style & ~WS_POPUP) | WND_STYLE);
        if (g_background) g_saved_place.showCmd = SW_SHOWNOACTIVATE;
        SetWindowPlacement(h, &g_saved_place);
        SetWindowPos(h, NULL, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED |
                         (g_background ? SWP_NOACTIVATE : 0));
    }
}

static int fullscreen_before_show(HWND h, int ow, int oh) {
    SetWindowPos(h, NULL, 0, 0, ow, oh, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    toggle_fullscreen(h);
    g_saved_place.showCmd = SW_SHOWNORMAL;
    return is_fullscreen(h);
}

static char g_applied_size[32];
static int g_applied_fs;

static void note_applied(void) {
    snprintf(g_applied_size, sizeof g_applied_size, "%s", cfg_value("NX2SARIA_WINDOW_SIZE"));
    g_applied_fs = cfg_fullscreen();
}

static void apply_settings(HWND h) {
    int size_changed = strcmp(cfg_value("NX2SARIA_WINDOW_SIZE"), g_applied_size) != 0, fs = cfg_fullscreen(), fs_changed = fs != g_applied_fs;
    note_applied();
    if (size_changed) {
        int cw, ch, ow, oh;
        cfg_window_size(&cw, &ch);
        outer_size(h, cw, ch, &ow, &oh);
        if (is_fullscreen(h)) {
            RECT *n = &g_saved_place.rcNormalPosition;
            n->right = n->left + ow;
            n->bottom = n->top + oh;
        } else {
            RECT have;
            if (GetClientRect(h, &have) && (have.right != cw || have.bottom != ch)) {
                if (IsZoomed(h)) ShowWindow(h, SW_RESTORE);
                SetWindowPos(h, NULL, 0, 0, ow, oh, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
        }
    }
    if (fs_changed && fs != is_fullscreen(h)) {
        input_cursor_suspend(1);
        toggle_fullscreen(h);
        input_cursor_suspend(0);
    }
}

void gfx_window_apply_settings(void) {
    if (g_hwnd) PostMessageW(g_hwnd, WM_APP_SETTINGS, 0, 0);
}


void gfx_window_request_close(void) {
    if (g_hwnd) PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
}

void gfx_window_text_input(int on) {
    if (g_hwnd) PostMessageW(g_hwnd, WM_APP_TEXT_INPUT, (WPARAM)(on != 0), 0);
}

void gfx_window_paste(void) {
    if (g_hwnd) PostMessageW(g_hwnd, WM_APP_PASTE, 0, 0);
}

static HMONITOR g_rate_mon;

static double monitor_rate_hz(HMONITOR mon) {
    MONITORINFOEXW mi = {.cbSize = sizeof mi};
    if (!GetMonitorInfoW(mon, (MONITORINFO *)&mi)) return 0;
    double hz = 0;
    UINT32 np = 0, nm = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) == ERROR_SUCCESS && np && nm) {
        DISPLAYCONFIG_PATH_INFO *paths = calloc(np, sizeof *paths);
        DISPLAYCONFIG_MODE_INFO *modes = calloc(nm, sizeof *modes);
        if (paths && modes && QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths, &nm, modes, NULL) == ERROR_SUCCESS)
            for (UINT32 i = 0; i < np && !hz; i++) {
                DISPLAYCONFIG_SOURCE_DEVICE_NAME src = {.header = {.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME,
                                                                   .size = sizeof src,
                                                                   .adapterId = paths[i].sourceInfo.adapterId,
                                                                   .id = paths[i].sourceInfo.id}};
                DISPLAYCONFIG_RATIONAL r = paths[i].targetInfo.refreshRate;
                if (DisplayConfigGetDeviceInfo(&src.header) == ERROR_SUCCESS && !wcscmp(src.viewGdiDeviceName, mi.szDevice) &&
                    r.Denominator)
                    hz = (double)r.Numerator / r.Denominator;
            }
        free(paths);
        free(modes);
    }
    DEVMODEW dm = {.dmSize = sizeof dm};
    if (!hz && EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
        hz = dm.dmDisplayFrequency;
    return hz;
}

static void refresh_rate_update(HWND h, int force) {
    HMONITOR mon = MonitorFromWindow(h, MONITOR_DEFAULTTOPRIMARY);
    if (!force && mon == g_rate_mon) return;
    g_rate_mon = mon;
    gfx_window_set_refresh(monitor_rate_hz(mon));
}

#define MON_MAX 32

typedef struct {
    HMONITOR h;
    RECT rc, work;
    int primary;
    UINT dpi;
    wchar_t device[CCHDEVICENAME];
    wchar_t name[64];
} MonInfo;

typedef struct { MonInfo m[MON_MAX]; int n; } MonList;

static void to_utf8(const wchar_t *w, char *out, size_t n) {
    if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)n, NULL, NULL)) out[0] = 0;
}

static UINT monitor_dpi(HMONITOR mon) {
    typedef HRESULT(WINAPI * GetDpiForMonitorFn)(HMONITOR, int, UINT *, UINT *);
    static GetDpiForMonitorFn fn;
    static int looked;
    if (!looked) {
        HMODULE lib = LoadLibraryExW(L"shcore.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
        fn = lib ? (GetDpiForMonitorFn)(void *)GetProcAddress(lib, "GetDpiForMonitor") : NULL;
        looked = 1;
    }
    UINT x = 0, y = 0;
    if (!fn || fn(mon, 0, &x, &y) != S_OK) x = 0;
    return x;
}

static BOOL CALLBACK enum_monitor(HMONITOR h, HDC dc, LPRECT rc, LPARAM lp) {
    (void)dc, (void)rc;
    MonList *l = (MonList *)lp;
    MONITORINFOEXW mi = {.cbSize = sizeof mi};
    if (l->n >= MON_MAX) return FALSE;
    if (!GetMonitorInfoW(h, (MONITORINFO *)&mi)) return TRUE;
    MonInfo *m = &l->m[l->n++];
    *m = (MonInfo){.h = h, .rc = mi.rcMonitor, .work = mi.rcWork, .primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0,
                   .dpi = monitor_dpi(h)};
    wcsncpy(m->device, mi.szDevice, CCHDEVICENAME - 1);
    return TRUE;
}

static void fill_monitor_names(MonList *l) {
    DISPLAYCONFIG_PATH_INFO *paths = NULL;
    DISPLAYCONFIG_MODE_INFO *modes = NULL;
    for (int attempt = 0; attempt < 3; attempt++) {
        UINT32 np = 0, nm = 0;
        if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) != ERROR_SUCCESS || !np || !nm) break;
        free(paths);
        free(modes);
        paths = calloc(np, sizeof *paths);
        modes = calloc(nm, sizeof *modes);
        if (!paths || !modes) break;
        LONG rc = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths, &nm, modes, NULL);
        if (rc == ERROR_INSUFFICIENT_BUFFER) continue;
        if (rc != ERROR_SUCCESS) break;
        for (UINT32 i = 0; i < np; i++) {
            DISPLAYCONFIG_SOURCE_DEVICE_NAME src = {.header = {.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME,
                                                               .size = sizeof src,
                                                               .adapterId = paths[i].sourceInfo.adapterId,
                                                               .id = paths[i].sourceInfo.id}};
            DISPLAYCONFIG_TARGET_DEVICE_NAME tgt = {.header = {.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME,
                                                               .size = sizeof tgt,
                                                               .adapterId = paths[i].targetInfo.adapterId,
                                                               .id = paths[i].targetInfo.id}};
            if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS ||
                DisplayConfigGetDeviceInfo(&tgt.header) != ERROR_SUCCESS)
                continue;
            for (int k = 0; k < l->n; k++)
                if (!l->m[k].name[0] && !_wcsicmp(src.viewGdiDeviceName, l->m[k].device))
                    wcsncpy(l->m[k].name, tgt.monitorFriendlyDeviceName, (sizeof l->m[k].name / sizeof(wchar_t)) - 1);
        }
        break;
    }
    free(paths);
    free(modes);
}

static int enum_monitors(MonList *l) {
    *l = (MonList){0};
    EnumDisplayMonitors(NULL, NULL, enum_monitor, (LPARAM)l);
    fill_monitor_names(l);
    return l->n;
}

static void monitor_line(const MonList *l, int i, char *out, size_t cap) {
    const MonInfo *m = &l->m[i];
    char dev[CCHDEVICENAME * 4], name[64 * 4];
    to_utf8(m->device, dev, sizeof dev);
    to_utf8(m->name, name, sizeof name);
    snprintf(out, cap, "%d  %s  \"%s\"  %ldx%ld at %ld,%ld%s  dpi %u (%u%%)", i + 1, dev, name, m->rc.right - m->rc.left,
             m->rc.bottom - m->rc.top, m->rc.left, m->rc.top, m->primary ? "  primary" : "", m->dpi,
             m->dpi * 100 / USER_DEFAULT_SCREEN_DPI);
}

static int monitor_unset(const char *v) { return !v || !*v || !_stricmp(v, "auto"); }

static int contains_nocase(const wchar_t *text, const wchar_t *needle_lower) {
    wchar_t buf[CCHDEVICENAME + 64];
    if (!*needle_lower) return 0;
    wcsncpy(buf, text, (sizeof buf / sizeof *buf) - 1);
    buf[(sizeof buf / sizeof *buf) - 1] = 0;
    CharLowerBuffW(buf, (DWORD)wcslen(buf));
    return wcsstr(buf, needle_lower) != NULL;
}

static int resolve_monitor(const MonList *l, const char *want, char *why, size_t cap) {
    if (!_stricmp(want, "primary")) {
        for (int i = 0; i < l->n; i++)
            if (l->m[i].primary) {
                snprintf(why, cap, "the primary monitor");
                return i;
            }
        snprintf(why, cap, "no monitor is marked primary");
        return -1;
    }
    if (want[strspn(want, "0123456789")] == 0) {
        long idx = strtol(want, NULL, 10);
        if (idx >= 1 && idx <= l->n) {
            snprintf(why, cap, "index %ld of %d", idx, l->n);
            return (int)idx - 1;
        }
        snprintf(why, cap, "index %s is outside 1..%d", want, l->n);
        return -1;
    }
    wchar_t needle[CCHDEVICENAME + 64];
    if (!MultiByteToWideChar(CP_UTF8, 0, want, -1, needle, (int)(sizeof needle / sizeof *needle))) {
        snprintf(why, cap, "the text is not valid UTF-8 or is too long");
        return -1;
    }
    CharLowerBuffW(needle, (DWORD)wcslen(needle));
    int first = -1, hits = 0;
    for (int i = 0; i < l->n; i++)
        if (contains_nocase(l->m[i].device, needle) || contains_nocase(l->m[i].name, needle)) {
            if (first < 0) first = i;
            hits++;
        }
    if (first < 0) {
        snprintf(why, cap, "no device name or monitor name contains it");
        return -1;
    }
    if (hits > 1) snprintf(why, cap, "device or monitor name contains it; the first of %d matches", hits);
    else snprintf(why, cap, "device or monitor name contains it");
    return first;
}

static int chosen_monitor(MonInfo *out, int log) {
    const char *want = cfg_get("NX2SARIA_MONITOR");
    if (monitor_unset(want)) return 0;
    MonList l;
    char why[256], line[512];
    int n = enum_monitors(&l), pick = -1;
    if (n) pick = resolve_monitor(&l, want, why, sizeof why);
    else snprintf(why, sizeof why, "no monitor was found");
    if (pick < 0) {
        if (log) {
            rt_log("gfx: NX2SARIA_MONITOR=%s picks no monitor (%s); the window gets the default placement. Monitors present:",
                   want, why);
            for (int i = 0; i < n; i++) {
                monitor_line(&l, i, line, sizeof line);
                rt_log("gfx:   %s", line);
            }
        }
        return 0;
    }
    if (log) {
        monitor_line(&l, pick, line, sizeof line);
        rt_log("gfx: NX2SARIA_MONITOR=%s picks monitor %s (%s)", want, line, why);
    }
    *out = l.m[pick];
    return 1;
}

int list_monitors(void) {
    MonList l;
    char line[512], why[256];
    int n = enum_monitors(&l);
    if (!n) {
        fprintf(stderr, "list-monitors: no monitor found\n");
        return 1;
    }
    printf("index  device  name  desktop rectangle  primary  dpi\n");
    for (int i = 0; i < n; i++) {
        monitor_line(&l, i, line, sizeof line);
        printf("%s\n", line);
    }
    const char *want = cfg_get("NX2SARIA_MONITOR");
    if (monitor_unset(want)) {
        printf("NX2SARIA_MONITOR is not set (auto): the system places the window\n");
    } else {
        int pick = resolve_monitor(&l, want, why, sizeof why);
        if (pick >= 0) printf("NX2SARIA_MONITOR=%s picks monitor %d (%s)\n", want, pick + 1, why);
        else printf("NX2SARIA_MONITOR=%s picks no monitor (%s)\n", want, why);
    }
    return 0;
}

static void center_in(const RECT *work, int ow, int oh, int *x, int *y) {
    int ww = work->right - work->left, wh = work->bottom - work->top;
    *x = work->left + (ww > ow ? (ww - ow) / 2 : 0);
    *y = work->top + (wh > oh ? (wh - oh) / 2 : 0);
}

int gfx_window_monitor_size(int *w, int *h) {
    MONITORINFO mi = {sizeof mi};
    MonInfo chosen;
    HMONITOR mon = g_hwnd                      ? MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTOPRIMARY)
                   : chosen_monitor(&chosen, 0) ? chosen.h
                                                : MonitorFromPoint((POINT){0, 0}, MONITOR_DEFAULTTOPRIMARY);
    if (!GetMonitorInfoW(mon, &mi)) return 0;
    *w = mi.rcMonitor.right - mi.rcMonitor.left;
    *h = mi.rcMonitor.bottom - mi.rcMonitor.top;
    return 1;
}

static volatile int32_t g_exit_started;

static void exit_main(void *arg) {
    (void)arg;
    gfx_window_closed();
}

static PlatThread *exit_start(HWND h) {
    if (plat_xchg32(&g_exit_started, 1)) return NULL;
    ShowWindow(h, SW_HIDE);
    PlatThread *t = plat_thread_new(exit_main, NULL, "exit");
    if (!t) gfx_window_closed();
    return t;
}

static LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    LRESULT input_res;
    if (input_window_msg(h, m, w, l, &input_res)) return input_res;
    if (menu_window_msg(h, m, w, l, &input_res)) return input_res;
    switch (m) {
    case WM_KEYDOWN:
        if (w == VK_F11 && !(l & (1 << 30)) && GetKeyState(VK_CONTROL) >= 0 && GetKeyState(VK_SHIFT) >= 0) {
            input_cursor_suspend(1);
            toggle_fullscreen(h);
            input_cursor_suspend(0);
            cfg_set("NX2SARIA_FULLSCREEN", is_fullscreen(h) ? "1" : "0");
            cfg_save();
            note_applied();
        }
        break;
    case WM_APP_SETTINGS:
        apply_settings(h);
        return 0;
    case WM_APP_FULLSCREEN:
        if (cfg_fullscreen() && !is_fullscreen(h)) {
            input_cursor_suspend(1);
            toggle_fullscreen(h);
            input_cursor_suspend(0);
        }
        return 0;
    case WM_APP_TEXT_INPUT:
        if (w) ImmAssociateContextEx(h, NULL, IACE_DEFAULT);
        else ImmAssociateContext(h, NULL);
        return 0;
    case WM_APP_PASTE: {
        char text[256];
        plat_clipboard_get(text, sizeof text);
        menu_event_paste(text);
        return 0;
    }
    case WM_SIZE:
        keep_awake(w != SIZE_MINIMIZED);
        if (w != SIZE_MINIMIZED) gfx_window_mark_dirty();
        refresh_rate_update(h, 0);
        return 0;
    case WM_EXITSIZEMOVE:
        refresh_rate_update(h, 0);
        break;
    case WM_DISPLAYCHANGE:
        refresh_rate_update(h, 1);
        if (is_fullscreen(h) && !IsIconic(h)) {
            MONITORINFO mi = {sizeof mi};
            if (GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi))
                SetWindowPos(h, NULL, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                             mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        gfx_window_mark_dirty();
        break;
    case WM_GETDPISCALEDSIZE:
        if (!is_fullscreen(h) && !IsZoomed(h)) {
            RECT c, r;
            if (GetClientRect(h, &c)) {
                r = (RECT){0, 0, c.right, c.bottom};
                if (AdjustWindowRectExForDpi(&r, WND_STYLE, FALSE, 0, LOWORD(w))) {
                    *(SIZE *)l = (SIZE){r.right - r.left, r.bottom - r.top};
                    return TRUE;
                }
            }
        }
        break;
    case WM_DPICHANGED: {
        const RECT *s = (const RECT *)l;
        plat_xchg32(&g_dpi, LOWORD(w));
        rt_log("gfx: window DPI %u (%u%%)", LOWORD(w), LOWORD(w) * 100u / USER_DEFAULT_SCREEN_DPI);
        if (is_fullscreen(h)) {
            MONITORINFO mi = {sizeof mi};
            if (GetMonitorInfoW(MonitorFromRect(s, MONITOR_DEFAULTTONEAREST), &mi))
                SetWindowPos(h, NULL, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                             mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOZORDER | SWP_NOACTIVATE);
        } else {
            SetWindowPos(h, NULL, s->left, s->top, s->right - s->left, s->bottom - s->top, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return 0;
    }
    case WM_ACTIVATEAPP: {
        int in = w != 0;
        if (!in && __atomic_load_n(&g_exit_started, __ATOMIC_ACQUIRE)) return 0;
        gfx_window_focus_changed(in);
        if (in && g_fs_pending) {
            g_fs_pending = 0;
            PostMessageW(h, WM_APP_FULLSCREEN, 0, 0);
        }
        return 0;
    }
    case WM_ENDSESSION:
        if (w) {
            PlatThread *t = exit_start(h);
            if (t) plat_thread_join(t);
            for (;;) plat_sleep_ms(1000);
        }
        return 0;
    case WM_CLOSE:
        exit_start(h);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    }
    return DefWindowProcW(h, m, w, l);
}

static void raise_window(HWND h) {
    HWND fg = GetForegroundWindow();
    DWORD me = GetCurrentThreadId(), other = fg ? GetWindowThreadProcessId(fg, NULL) : 0;
    int attached = other && other != me && AttachThreadInput(me, other, TRUE);
    ShowWindow(h, IsIconic(h) ? SW_RESTORE : SW_SHOW);
    BringWindowToTop(h);
    SetForegroundWindow(h);
    SetActiveWindow(h);
    if (attached) AttachThreadInput(me, other, FALSE);
    if (GetForegroundWindow() != h) {
        SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        SetWindowPos(h, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    }
}

static int in_tri(float px, float py, float ax, float ay, float bx, float by, float cx, float cy) {
    float d1 = (px - bx) * (ay - by) - (ax - bx) * (py - by), d2 = (px - cx) * (by - cy) - (bx - cx) * (py - cy),
          d3 = (px - ax) * (cy - ay) - (cx - ax) * (py - ay);
    int neg = d1 < 0 || d2 < 0 || d3 < 0, pos = d1 > 0 || d2 > 0 || d3 > 0;
    return !(neg && pos);
}

static HCURSOR pointer_cursor(void) {
    UINT dpi = GetDpiForSystem();
    int n = (int)(32 * (dpi ? dpi : 96) / 96);
    if (n < 24) n = 24;
    if (n > 96) n = 96;
    BITMAPV5HEADER bh = {.bV5Size = sizeof bh, .bV5Width = n, .bV5Height = -n, .bV5Planes = 1, .bV5BitCount = 32,
                         .bV5Compression = BI_BITFIELDS, .bV5RedMask = 0x00ff0000, .bV5GreenMask = 0x0000ff00,
                         .bV5BlueMask = 0x000000ff, .bV5AlphaMask = 0xff000000};
    HDC dc = GetDC(NULL);
    uint32_t *px = NULL;
    HBITMAP color = CreateDIBSection(dc, (BITMAPINFO *)&bh, DIB_RGB_COLORS, (void **)&px, NULL, 0);
    ReleaseDC(NULL, dc);
    HBITMAP mask = CreateBitmap(n, n, 1, 1, NULL);
    if (!color || !mask || !px) {
        if (color) DeleteObject(color);
        if (mask) DeleteObject(mask);
        return LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    }
    float o = n * 0.07f, ax = o, ay = o, bx = o + n * 0.70f, by = o + n * 0.46f, cx = o + n * 0.30f, cy = o + n * 0.80f;
    float mx = o + n * 0.36f, my = o + n * 0.50f, e = n * 0.055f;
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) {
            int light = 0, deep = 0, edge = 0;
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++) {
                    float fx = x + (sx + 0.5f) / 4, fy = y + (sy + 0.5f) / 4;
                    int l = in_tri(fx, fy, ax, ay, bx, by, mx, my), d = !l && in_tri(fx, fy, ax, ay, mx, my, cx, cy);
                    int out = in_tri(fx, fy, ax - e, ay - e * 1.3f, bx + e * 1.4f, by + e * 0.2f, mx, my + e * 1.2f) ||
                              in_tri(fx, fy, ax - e * 1.3f, ay - e, mx + e * 1.2f, my, cx + e * 0.2f, cy + e * 1.4f);
                    light += l, deep += d, edge += out && !l && !d;
                }
            int cov = light + deep + edge;
            uint32_t a = (uint32_t)(cov * 255 / 16), r = 0, g = 0, b = 0;
            if (cov) {
                r = (uint32_t)((255 * light + 214 * deep + 44 * edge) / cov);
                g = (uint32_t)((226 * light + 158 * deep + 28 * edge) / cov);
                b = (uint32_t)((138 * light + 58 * deep + 10 * edge) / cov);
            }
            px[y * n + x] = a << 24 | (r * a / 255) << 16 | (g * a / 255) << 8 | (b * a / 255);
        }
    ICONINFO ii = {.fIcon = FALSE, .xHotspot = (DWORD)o, .yHotspot = (DWORD)o, .hbmMask = mask, .hbmColor = color};
    HCURSOR cur = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    return cur ? cur : LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
}

static void window_main(void *arg) {
    (void)arg;
    HINSTANCE inst = GetModuleHandleW(NULL);
    WNDCLASSEXW wc = {.cbSize = sizeof wc, .lpfnWndProc = wnd_proc, .hInstance = inst,
                      .hIcon = LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON),
                                          GetSystemMetrics(SM_CYICON), LR_SHARED),
                      .hIconSm = LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                            GetSystemMetrics(SM_CYSMICON), LR_SHARED),
                      .hCursor = pointer_cursor(), .lpszClassName = L"NX2SariaRecompWindow"};
    RegisterClassExW(&wc);
    int cw, ch, ow, oh, x = CW_USEDEFAULT, y = CW_USEDEFAULT;
    cfg_window_size(&cw, &ch);
    MonInfo mon;
    int on_mon = chosen_monitor(&mon, 1);
    fit_work_area(&cw, &ch, on_mon ? &mon.work : NULL, on_mon && mon.dpi ? mon.dpi : GetDpiForSystem());
    outer_size(NULL, cw, ch, &ow, &oh);
    if (on_mon) {
        outer_size_at(cw, ch, mon.dpi ? mon.dpi : GetDpiForSystem(), &ow, &oh);
        center_in(&mon.work, ow, oh, &x, &y);
    }
    int background = g_background = cfg_get("NX2SARIA_BACKGROUND_WINDOW") && atoi(cfg_get("NX2SARIA_BACKGROUND_WINDOW"));
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"NX2Saria", WND_STYLE, x, y, ow, oh, NULL, NULL,
                             inst, NULL);
    if (!g_hwnd) {
        rt_log("gfx: CreateWindowEx failed (%lu)", GetLastError());
        g_wnd_failed = 1;
    } else {
        ImmAssociateContext(g_hwnd, NULL);
        int nw, nh, nx = 0, ny = 0;
        UINT flags = SWP_NOACTIVATE | SWP_SHOWWINDOW;
        plat_xchg32(&g_dpi, (int32_t)GetDpiForWindow(g_hwnd));
        outer_size(g_hwnd, cw, ch, &nw, &nh);
        if (on_mon && (nw != ow || nh != oh)) center_in(&mon.work, nw, nh, &nx, &ny);
        else flags |= SWP_NOMOVE;
        int fs = !background && cfg_fullscreen() && fullscreen_before_show(g_hwnd, nw, nh);
        SetWindowPos(g_hwnd, HWND_BOTTOM, nx, ny, nw, nh, flags | (fs ? SWP_NOSIZE | SWP_NOMOVE : 0));
        if (on_mon && MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST) != mon.h)
            rt_log("gfx: the window is not on the chosen monitor; F11 fullscreen covers the monitor it is on");
        if (!background) {
            g_fs_pending = cfg_fullscreen() && !fs;
            raise_window(g_hwnd);
            FLASHWINFO fw = {.cbSize = sizeof fw, .hwnd = g_hwnd, .dwFlags = FLASHW_TRAY | FLASHW_TIMERNOFG};
            FlashWindowEx(&fw);
        }
        note_applied();
        refresh_rate_update(g_hwnd, 1);
        keep_awake(1);
    }
    plat_event_set(&g_wnd_ready);
    if (g_wnd_failed) return;
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return;
}

int wsys_open(void) {
    g_wnd_failed = 0;
    plat_event_init(&g_wnd_ready, 1, 0);
    if (!plat_thread_spawn(window_main, NULL, "window", 0)) rt_fatal("gfx: cannot start the window thread");
    plat_event_wait(&g_wnd_ready);
    return !g_wnd_failed;
}

VkSurfaceKHR wsys_surface(VkInstance inst) {
    VkWin32SurfaceCreateInfoKHR si = {.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR,
                                      .hinstance = GetModuleHandleW(NULL), .hwnd = g_hwnd};
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    PFN_vkCreateWin32SurfaceKHR create = (PFN_vkCreateWin32SurfaceKHR)vk_dlssg_instance_proc(inst, "vkCreateWin32SurfaceKHR");
    VkResult r = (create ? create : vkCreateWin32SurfaceKHR)(inst, &si, NULL, &surface);
    if (r != VK_SUCCESS) rt_log("gfx: vkCreateWin32SurfaceKHR failed (%d)", (int)r);
    return r == VK_SUCCESS ? surface : VK_NULL_HANDLE;
}

void wsys_client_size(int *w, int *h) {
    RECT rc = {0};
    GetClientRect(g_hwnd, &rc);
    *w = rc.right, *h = rc.bottom;
}

int wsys_minimized(void) { return IsIconic(g_hwnd) != 0; }

void wsys_resize(int cw, int ch) {
    int ow, oh;
    outer_size(g_hwnd, cw, ch, &ow, &oh);
    SetWindowPos(g_hwnd, NULL, 0, 0, ow, oh, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void wsys_probe_focus(int in) { SendMessageW(g_hwnd, WM_ACTIVATEAPP, in ? TRUE : FALSE, 0); }

void wsys_probe_escape(void) { SendMessageW(g_hwnd, WM_KEYDOWN, VK_ESCAPE, 0); }

int wsys_probe_click(int where) {
    if (where == 0) SendMessageW(g_hwnd, WM_LBUTTONDOWN, MK_LBUTTON, 0);
    else SendMessageW(g_hwnd, WM_MOUSEACTIVATE, (WPARAM)g_hwnd, MAKELPARAM(where == 1 ? HTCLIENT : HTCAPTION, WM_LBUTTONDOWN));
    return 1;
}
