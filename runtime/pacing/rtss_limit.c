#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gfx.h"
#include "host.h"
#include "platform/platform.h"
#include "rtss_decide.h"

#ifdef _WIN32
#include <windows.h>

typedef DWORD (*EnumProfilesFn)(LPSTR list, DWORD size);
typedef void (*ProfileFn)(LPCSTR name);
typedef BOOL (*PropertyFn)(LPCSTR property, LPBYTE data, DWORD size);
typedef void (*UpdateFn)(void);

#define POLL_MS 500

static PlatMutex lock = PLAT_MUTEX_INIT;
static _Atomic int64_t want = RTSS_UNSET;
static _Atomic int64_t active_for = RTSS_UNSET;
static _Atomic int64_t movie_mhz;
static _Atomic int failed;
static _Atomic int closed;
static _Atomic int two;
static _Atomic int started;
static _Atomic int ready;
static char dll_path[MAX_PATH], profile[MAX_PATH], rtss_dir[MAX_PATH];
static HMODULE dll;
static EnumProfilesFn enum_profiles;
static ProfileFn load_profile, save_profile, delete_profile;
static PropertyFn get_property, set_property;
static UpdateFn update_profiles;
static RtssState S = RTSS_STATE_INIT;
static int has_denominator;
static int had_profile;
static DWORD old_limit, old_denominator = 1;
static int owed;
static int creations, removals;
static char state_path[MAX_PATH + 32];
static RtssSaved rec;
static _Atomic int recorded;
static int state_off;
static _Atomic uint32_t rtss_tid;

static void rtss_map(RtssApp *a) {
    memset(a, 0, sizeof *a);
    HANDLE m = OpenFileMappingA(FILE_MAP_READ, FALSE, "RTSSSharedMemoryV2");
    if (!m) return;
    const uint8_t *p = MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0);
    MEMORY_BASIC_INFORMATION mi;
    if (p && VirtualQuery(p, &mi, sizeof mi)) rtss_parse(p, mi.RegionSize, GetCurrentProcessId(), a);
    if (p) UnmapViewOfFile(p);
    CloseHandle(m);
}

static int load_hooks(void) {
    if (dll) return 1;
    dll = LoadLibraryA(dll_path);
    if (!dll) {
        rt_log("frame rate: RivaTuner Statistics Server's %s did not load (%lu)", dll_path, GetLastError());
        return 0;
    }
    enum_profiles = (EnumProfilesFn)(void *)GetProcAddress(dll, "EnumProfiles");
    load_profile = (ProfileFn)(void *)GetProcAddress(dll, "LoadProfile");
    save_profile = (ProfileFn)(void *)GetProcAddress(dll, "SaveProfile");
    delete_profile = (ProfileFn)(void *)GetProcAddress(dll, "DeleteProfile");
    get_property = (PropertyFn)(void *)GetProcAddress(dll, "GetProfileProperty");
    set_property = (PropertyFn)(void *)GetProcAddress(dll, "SetProfileProperty");
    update_profiles = (UpdateFn)(void *)GetProcAddress(dll, "UpdateProfiles");
    if (enum_profiles && load_profile && save_profile && delete_profile && get_property && set_property && update_profiles)
        return 1;
    rt_log("frame rate: RivaTuner Statistics Server's hooks library lacks the profile calls");
    FreeLibrary(dll);
    dll = NULL;
    return 0;
}

static int profile_exists(void) {
    if (!rt_knob("RTSS_ENUM", "0")) {
        char path[MAX_PATH * 2];
        snprintf(path, sizeof path, "%s\\Profiles\\%s.cfg", rtss_dir, profile);
        return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
    }
    char list[8192];
    DWORD need = enum_profiles(list, sizeof list);
    list[sizeof list - 1] = 0;
    if (need > sizeof list)
        rt_log("frame rate: RivaTuner Statistics Server lists %lu bytes of profiles; reading the first %zu", need, sizeof list);
    size_t n = strlen(profile);
    for (char *s = list; *s;) {
        char *end = strchr(s, ',');
        size_t len = end ? (size_t)(end - s) : strlen(s);
        if (len == n && !_strnicmp(s, profile, n)) return 1;
        if (!end) break;
        s = end + 1;
    }
    return 0;
}

static int write_limit(DWORD limit, DWORD denominator) {
    load_profile(profile);
    DWORD none = 0;
    if (!set_property("FramerateLimit", (LPBYTE)&none, sizeof none)) return 0;
    if (has_denominator && !set_property("FramerateLimitDenominator", (LPBYTE)&denominator, sizeof denominator)) return 0;
    if (limit && !set_property("FramerateLimit", (LPBYTE)&limit, sizeof limit)) return 0;
    save_profile(profile);
    load_profile(profile);
    DWORD l = ~0u, d = 1;
    if (!get_property("FramerateLimit", (LPBYTE)&l, sizeof l)) return -1;
    if (has_denominator && !get_property("FramerateLimitDenominator", (LPBYTE)&d, sizeof d)) return -1;
    return l == limit && (!has_denominator || d == denominator) ? 1 : -1;
}

static int state_put(const RtssSaved *r) {
    if (state_off || !*state_path) return 0;
    uint64_t n = 0;
    char *text = (char *)plat_file_read(state_path, &n);
    if (!text && plat_file_size(state_path) > 0) return 0;
    size_t cap = (size_t)(text ? n : 0) + sizeof RTSS_SAVED_HEADER + 2 * (RTSS_NAME_MAX + 96);
    char *out = malloc(cap);
    int records = 0, ok = 0;
    size_t len = out ? rtss_saved_update(text ? text : "", profile, r, out, cap, &records) : 0;
    if (len && !records) {
        plat_file_delete(state_path);
        ok = !plat_file_exists(state_path);
    } else if (len) {
        ok = plat_file_write_atomic(state_path, out, len);
    }
    free(out);
    free(text);
    return ok;
}

static void unrecorded(void) {
    static int said;
    atomic_store(&recorded, 0);
    if (!state_off && !said++)
        rt_log("frame rate: %s could not be written; a crash would leave the game's limit in RivaTuner Statistics "
               "Server's profile %s", *state_path ? state_path : "rtss-profile.txt", profile);
}

static void record_before(DWORD limit, DWORD denominator) {
    if (!atomic_load(&recorded) || (rec.limit == limit && rec.den == denominator)) return;
    rec.prev_limit = rec.limit, rec.prev_den = rec.den;
    rec.limit = limit, rec.den = denominator;
    if (!state_put(&rec)) unrecorded();
}

static void record_after(int landed) {
    if (!atomic_load(&recorded) || (rec.limit == rec.prev_limit && rec.den == rec.prev_den)) return;
    if (landed) rec.prev_limit = rec.limit, rec.prev_den = rec.den;
    else rec.limit = rec.prev_limit, rec.den = rec.prev_den;
    if (!state_put(&rec)) unrecorded();
}

static int apply(void *ctx, int64_t mhz, int first, int clear) {
    (void)ctx;
    if (first && !owed) {
        had_profile = profile_exists();
        load_profile(profile);
        old_limit = 0, old_denominator = 1;
        get_property("FramerateLimit", (LPBYTE)&old_limit, sizeof old_limit);
        has_denominator = get_property("FramerateLimitDenominator", (LPBYTE)&old_denominator, sizeof old_denominator);
        if (!old_denominator) old_denominator = 1;
    }
    uint32_t l, d;
    rtss_limit_value(mhz, has_denominator, &l, &d);
    DWORD limit = l, denominator = d;
    if (first) {
        RtssSaved was = rec;
        int kept = atomic_load(&recorded);
        rec = (RtssSaved){had_profile, old_limit, old_denominator, limit, denominator, kept ? was.limit : limit,
                          kept ? was.den : denominator, ""};
        snprintf(rec.name, sizeof rec.name, "%s", profile);
        if (state_put(&rec)) atomic_store(&recorded, 1);
        else unrecorded();
    } else {
        record_before(limit, denominator);
    }
    int r = write_limit(limit, denominator);
    if (r < 0) {
        rt_log("frame rate: RivaTuner Statistics Server's profile %s did not keep the limit: its Profiles folder needs the "
               "game run as administrator", profile);
        if (first && !owed && atomic_exchange(&recorded, 0)) state_put(NULL);
        return RTSS_WROTE_READONLY;
    }
    owed = 1;
    if (first) {
        if (!had_profile)
            rt_log("frame rate: RivaTuner Statistics Server profile %s created; it goes away when the game closes "
                   "(creation %d this run)", profile, ++creations);
        else
            rt_log("frame rate: RivaTuner Statistics Server profile %s held a limit of %lu/%lu; it gets that back when the "
                   "game closes", profile, old_limit, old_denominator);
    }
    if (r == 0) {
        rt_log("frame rate: RivaTuner Statistics Server refused a limit of %.3f fps", mhz / 1000.0);
        record_after(0);
        if (!clear) return RTSS_WROTE_REFUSED;
        record_before(0, 1);
        int cleared = write_limit(0, 1) > 0;
        record_after(cleared);
        if (!cleared) return RTSS_WROTE_REFUSED;
        update_profiles();
        return RTSS_WROTE_REFUSED_CLEARED;
    }
    record_after(1);
    update_profiles();
    if (!mhz) rt_log("frame rate: RivaTuner Statistics Server's profile %s has no limit", profile);
    else if ((int64_t)limit * 1000 == mhz * denominator)
        rt_log("frame rate: RivaTuner Statistics Server's profile %s limits to %.3f fps", profile, mhz / 1000.0);
    else
        rt_log("frame rate: RivaTuner Statistics Server's profile %s limits to %lu/%lu (%.3f fps; the game asked %.3f)",
               profile, limit, denominator, (double)limit / denominator, mhz / 1000.0);
    return RTSS_WROTE_HELD;
}

static void put_back(void) {
    int back = 1;
    if (!had_profile) {
        delete_profile(profile);
        back = !profile_exists();
        if (back)
            rt_log("frame rate: RivaTuner Statistics Server profile %s removed, as before the game started "
                   "(removal %d this run)", profile, ++removals);
        else
            rt_log("frame rate: RivaTuner Statistics Server profile %s did not go away", profile);
    } else if (write_limit(old_limit, old_denominator) > 0) {
        rt_log("frame rate: RivaTuner Statistics Server profile %s has its limit of %lu/%lu back", profile, old_limit,
               old_denominator);
    } else {
        rt_log("frame rate: RivaTuner Statistics Server profile %s did not take its old limit back", profile);
        back = 0;
    }
    update_profiles();
    if (back) owed = 0;
    if (back && atomic_exchange(&recorded, 0)) state_put(NULL);
}

static int left_line(RtssSaved *r) {
    uint64_t n = 0;
    char *text = *state_path ? (char *)plat_file_read(state_path, &n) : NULL;
    int found = text && rtss_saved_find(text, profile, r);
    free(text);
    return found;
}

static void restore_left(const RtssSaved *r) {
    int exists = profile_exists();
    load_profile(profile);
    DWORD limit = 0, den = 1;
    get_property("FramerateLimit", (LPBYTE)&limit, sizeof limit);
    has_denominator = get_property("FramerateLimitDenominator", (LPBYTE)&den, sizeof den);
    if (!den) den = 1;
    int plan = rtss_restore_plan(r, exists, limit, den);
    if (plan == RTSS_PLAN_DELETE) delete_profile(profile);
    if ((plan == RTSS_PLAN_WRITE && write_limit(r->old_limit, r->old_den) <= 0) ||
        (plan == RTSS_PLAN_DELETE && profile_exists())) {
        rt_log("frame rate: RivaTuner Statistics Server profile %s still has the game's limit of %lu/%lu from a run "
               "that did not close, and did not go back as it was; %s keeps its line", profile, limit, den, state_path);
        state_off = 1;
        return;
    }
    if (plan == RTSS_PLAN_WRITE || plan == RTSS_PLAN_DELETE) update_profiles();
    if (plan == RTSS_PLAN_WRITE)
        rt_log("frame rate: RivaTuner Statistics Server profile %s had the game's limit of %lu/%lu from a run that did "
               "not close (%s); its limit of %u/%u is back", profile, limit, den, state_path, r->old_limit, r->old_den);
    else if (plan == RTSS_PLAN_DELETE)
        rt_log("frame rate: RivaTuner Statistics Server profile %s had the game's limit of %lu/%lu from a run that did "
               "not close (%s); removed, as before that run", profile, limit, den, state_path);
    else if (plan == RTSS_PLAN_LEAVE)
        rt_log("frame rate: RivaTuner Statistics Server profile %s changed since a run that did not close (%s); left as "
               "it is", profile, state_path);
    else
        rt_log("frame rate: RivaTuner Statistics Server profile %s is as it was before a run that did not close (%s)",
               profile, state_path);
    if (!state_put(NULL)) {
        rt_log("frame rate: %s could not be written; its line for %s stays", state_path, profile);
        state_off = 1;
    }
}

static int left_start(int64_t w, int hooks) {
    RtssSaved r;
    if (!left_line(&r)) return 1;
    if (hooks != 1 && w == RTSS_OUT_SETTING) {
        rt_log("frame rate: %s holds the game's limit of %u/%u for RivaTuner Statistics Server's profile %s from a run "
               "that did not close; NX2SARIA_RTSS=0 leaves the profile as it is, and a start with RTSS let in puts it "
               "back", state_path, r.limit, r.den, profile);
        return 1;
    }
    if (hooks != 1 && (w >= 0 || !load_hooks())) return 0;
    restore_left(&r);
    return 1;
}

static void put_back_op(void *ctx, const RtssOut *o) {
    (void)ctx;
    if (o->why == RTSS_WHY_FAILED && S.fail == RTSS_FAIL_NOT_LIMITING)
        rt_log("frame rate: RivaTuner Statistics Server measures %.1f fps with its limit at %.3f fps", o->fps,
               o->mhz / 1000.0);
    else if (o->why == RTSS_WHY_FAILED)
        rt_log("frame rate: RivaTuner Statistics Server listed the game with its limit at %.3f fps for %d s and never "
               "showed it holds it; the game's own limiter for the rest of the run", o->mhz / 1000.0,
               RTSS_UNPROVEN_POLLS * POLL_MS / 1000);
    put_back();
}

#define LIMITER_LINES 100
static void set_active(int64_t w, int on, const char *why) {
    static int logged = -1, lines;
    static char said[128];
    atomic_store(&active_for, on ? w : RTSS_UNSET);
    if ((on && atomic_load(&want) != w) || (logged == on && (on || !strcmp(why, said)))) return;
    logged = on;
    snprintf(said, sizeof said, "%s", on ? "" : why);
    if (++lines > LIMITER_LINES) return;
    if (on) rt_log("frame rate: RivaTuner Statistics Server does the limiting; the game's own limiter stands aside");
    else rt_log("frame rate: the game's own limiter (%s)", why);
    if (lines == LIMITER_LINES) rt_log("frame rate: later changes of the limiter in charge are not logged");
}

static RtssSettings settings;
static char settings_file[MAX_PATH + 16];

static uint64_t file_stamp(const char *path) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &a)) return 0;
    return ((uint64_t)a.ftLastWriteTime.dwHighDateTime << 32 | a.ftLastWriteTime.dwLowDateTime) | 1;
}

static int cfg_int(const char *own, const char *global, const char *key) {
    char v[32] = "";
    if (own) GetPrivateProfileStringA("Framerate", key, "", v, sizeof v, own);
    if (!*v) GetPrivateProfileStringA("Framerate", key, "", v, sizeof v, global);
    return (int)strtol(v, NULL, 10);
}

static void read_settings(void) {
    static uint64_t stamps[2] = {~0ull, ~0ull};
    char own[MAX_PATH * 2], global[MAX_PATH * 2];
    snprintf(own, sizeof own, "%s\\Profiles\\%s.cfg", rtss_dir, profile);
    snprintf(global, sizeof global, "%s\\Profiles\\Global", rtss_dir);
    uint64_t a = file_stamp(own), b = file_stamp(global);
    if (a == stamps[0] && b == stamps[1]) return;
    stamps[0] = a, stamps[1] = b;
    const char *p = a ? own : NULL;
    settings = (RtssSettings){cfg_int(p, global, "Limit"), cfg_int(p, global, "LimitDenominator"),
                              cfg_int(p, global, "LimitTime"), cfg_int(p, global, "LimitTimeDenominator"),
                              cfg_int(p, global, "SyncScanline0"), cfg_int(p, global, "SyncScanline1"),
                              cfg_int(p, global, "SyncDisplay"), cfg_int(p, global, "ReflexSleep"),
                              cfg_int(p, global, "PassiveWait")};
    if (a) snprintf(settings_file, sizeof settings_file, "Profiles\\%s.cfg", profile);
    else snprintf(settings_file, sizeof settings_file, "Profiles\\Global");
}

static void watch(int64_t w, int fail, int listed) {
    static int said, lines, settings_lines;
    static const char *said_by;
    static char said_text[128], said_settings[MAX_PATH + 256];
    int out = rtss_out_of_limit(w, fail), on = 0;
    const char *by = rtss_out_limiter(w);
    char text[128] = "";
    if (listed) {
        read_settings();
        char all[MAX_PATH + 256], modes[192];
        rtss_settings_text(modes, sizeof modes, &settings, 1);
        snprintf(all, sizeof all, "(%s): %s", settings_file, modes);
        if (strcmp(all, said_settings) && settings_lines++ < 40)
            rt_log("frame rate: RivaTuner Statistics Server's settings for the game %s", all);
        snprintf(said_settings, sizeof said_settings, "%s", all);
    }
    if (out && listed) {
        on = rtss_two(w, fail, listed, rtss_settings_limit(&settings));
        rtss_settings_text(text, sizeof text, &settings, 0);
    }
    if (!out) said = 0;
    else if (on != said || (on && (strcmp(text, said_text) || by != said_by))) {
        if (lines++ < 40) {
            if (on)
                rt_log("frame rate: two limiters: RivaTuner Statistics Server's own setting for the game (%s: %s) limits "
                       "it beside %s", settings_file, text, by);
            else
                rt_log("frame rate: RivaTuner Statistics Server no longer limits the game beside %s", by);
        }
        said = on, said_by = by;
        snprintf(said_text, sizeof said_text, "%s", text);
    }
    atomic_store(&two, on && w < 0);
}

static int find_rtss(void) {
    char dir[MAX_PATH];
    DWORD size = sizeof dir;
    if (RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\WOW6432Node\\Unwinder\\RTSS", "InstallDir", RRF_RT_REG_SZ, NULL, dir,
                     &size) != ERROR_SUCCESS) {
        rt_log("frame rate: RivaTuner Statistics Server is not installed; the game's own limiter");
        return 0;
    }
    snprintf(dll_path, sizeof dll_path, "%s\\RTSSHooks64.dll", dir);
    snprintf(rtss_dir, sizeof rtss_dir, "%s", dir);
    if (GetFileAttributesA(dll_path) == INVALID_FILE_ATTRIBUTES) {
        rt_log("frame rate: RivaTuner Statistics Server's folder %s has no RTSSHooks64.dll; the game's own limiter", dir);
        return 0;
    }
    char exe[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, exe, sizeof exe);
    if (!n || n >= sizeof exe) return 0;
    const char *base = exe;
    for (const char *s = exe; *s; s++)
        if (*s == '\\' || *s == '/') base = s + 1;
    snprintf(profile, sizeof profile, "%s", base);
    char path[MAX_PATH], *cut;
    if (plat_exe_path(path, sizeof path) && (cut = plat_last_sep(path)))
        snprintf(state_path, sizeof state_path, "%.*s%s", (int)(cut + 1 - path), path, "rtss-profile.txt");
    int64_t first = atomic_load(&want);
    if (first >= 0)
        rt_log("frame rate: RivaTuner Statistics Server found in %s; it takes the frame limit while it runs", dir);
    else
        rt_log("frame rate: RivaTuner Statistics Server found in %s; kept out (%s), and while it is kept out its "
               "thread only reads RTSS's own settings for the game, to tell when RTSS limits it a second time", dir,
               rtss_out_text(first));
    return 1;
}

static void worker(void *arg) {
    (void)arg;
    atomic_store(&rtss_tid, plat_thread_id());
    if (!find_rtss()) return;
    atomic_store_explicit(&ready, 1, memory_order_release);
    int was_running = -1, was_listed = 0, left_done = 0, left_tried = 0;
    const RtssOps ops = {NULL, apply, put_back_op};
    for (;; plat_sleep_ms(POLL_MS)) {
        int64_t w = atomic_load(&want);
        if (w == RTSS_UNSET) continue;
        plat_mutex_lock(&lock);
        if (atomic_load(&closed)) {
            plat_mutex_unlock(&lock);
            continue;
        }
        RtssIn in = {.want = w, .movie_mhz = atomic_load(&movie_mhz), .now_ns = kern_now_ns(),
                     .hosted_ns = gfx_window_hosted_ns()};
        rtss_map(&in.app);
        if (in.app.running != was_running || in.app.listed != was_listed) {
            if (!in.app.running) rt_log("frame rate: RivaTuner Statistics Server is installed but not running");
            else if (!in.app.listed) rt_log("frame rate: RivaTuner Statistics Server runs; it has not hooked the game yet");
            else rt_log("frame rate: RivaTuner Statistics Server has hooked the game");
            was_running = in.app.running, was_listed = in.app.listed;
        }
        in.hooks = dll ? 1 : rtss_wants_hooks(&S, &in) ? load_hooks() : -1;
        if (!left_done && in.app.running && (in.hooks == 1 || !left_tried)) {
            left_tried = 1;
            left_done = left_start(w, in.hooks);
        }
        in.current = atomic_load(&want) == w;
        RtssOut o = rtss_poll(&S, &in, &ops);
        atomic_store(&failed, S.fail);
        if (o.why != RTSS_WHY_NEWER) {
            char why[128];
            rtss_why_text(why, sizeof why, &o, S.fail);
            set_active(w, o.holds, why);
        }
        watch(w, S.fail, in.app.listed);
        plat_mutex_unlock(&lock);
    }
}

void rtss_limit_want(int64_t mhz) {
    if (atomic_load_explicit(&closed, memory_order_relaxed)) return;
    atomic_store(&want, mhz);
    if (!atomic_load_explicit(&started, memory_order_relaxed) && !atomic_exchange(&started, 1) &&
        !plat_thread_spawn(worker, NULL, "rtss", 0))
        rt_log("frame rate: the RivaTuner Statistics Server thread did not start");
}

void rtss_limit_movie(int hz) { atomic_store_explicit(&movie_mhz, (int64_t)hz * 1000, memory_order_relaxed); }

int rtss_limit_active(void) {
    if (atomic_load_explicit(&closed, memory_order_relaxed)) return 0;
    int64_t a = atomic_load_explicit(&active_for, memory_order_relaxed);
    return a >= 0 && a == atomic_load_explicit(&want, memory_order_relaxed);
}

int rtss_limit_state(void) {
    if (atomic_load_explicit(&closed, memory_order_relaxed)) return 0;
    int f = atomic_load_explicit(&failed, memory_order_relaxed);
    int64_t w = atomic_load(&want);
    if (w >= 0 && (f == RTSS_FAIL_READONLY || f == RTSS_FAIL_NOT_LIMITING || f == RTSS_FAIL_UNPROVEN))
        return f == RTSS_FAIL_READONLY ? 2 : 3;
    if (w < 0 && w != RTSS_UNSET && atomic_load_explicit(&two, memory_order_relaxed)) return 4;
    return rtss_limit_active() && w > 0;
}

void rtss_limit_restore(void) {
    atomic_store(&closed, 1);
    if (!atomic_load_explicit(&ready, memory_order_acquire)) return;
    for (int i = 0; i < 100; i++) {
        if (plat_mutex_trylock(&lock)) {
            if (dll && (S.changed || owed)) put_back(), rtss_put_back_done(&S);
            plat_mutex_unlock(&lock);
            return;
        }
        plat_sleep_ms(10);
    }
}

void rtss_limit_crash(void) {
    atomic_store(&closed, 1);
    atomic_store(&active_for, RTSS_UNSET);
    if (atomic_load(&recorded) || plat_thread_id() == atomic_load(&rtss_tid)) return;
    rtss_limit_restore();
}

#else
void rtss_limit_want(int64_t mhz) { (void)mhz; }
void rtss_limit_movie(int hz) { (void)hz; }
int rtss_limit_active(void) { return 0; }
int rtss_limit_state(void) { return 0; }
void rtss_limit_restore(void) {}
void rtss_limit_crash(void) {}
#endif
