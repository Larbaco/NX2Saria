// Personal folders written short in what players share: the game log (main.c rt_log_raw, so the log file, stderr, the
// queue the exit note reads and the crash report alike), and the values the bug capture writes (bugcap.c). Players
// attach logs/nx2saria.log and the F8 capture's log-tail.txt to public issues, and the log named files under the
// account's folder: C:\Users\<name>\AppData\Local\Programs\NX2Saria\game\SDL3.dll, /home/<name>/Games/...
//
// The folders come from the environment once (log_privacy_init, before the first line), and the matching is
// log_privacy_core.h: a table read in one pass, with no allocation and no lock, so the crash report goes through it too.
//   Windows: LOCALAPPDATA, APPDATA, USERPROFILE, HOMEDRIVE + HOMEPATH, HOMESHARE (a profile on a network share),
//            OneDrive, TEMP and TMP (often written in the 8.3 form, C:\Users\ANASOU~1\...), the 8.3 forms of
//            USERPROFILE and LOCALAPPDATA, and under Wine the Unix home (HOME, Z:\home\<name>, WINEHOMEDIR); the
//            name is USERNAME, the computer COMPUTERNAME and USERDOMAIN (the computer's own name on a PC outside a
//            domain), and under Wine also the Linux host name (HOSTNAME).
//   Linux:   HOME and the folder it resolves to (realpath: /var/home/<name> on systems where /home is a link); the
//            name is USER or LOGNAME, the computer gethostname (and its first label) and HOSTNAME.
// Computer names often hold the owner's name ("maria-pc"); they are written %COMPUTERNAME% (%USERDOMAIN% for a
// domain's name) as a word of their own anywhere in a line.
// The markers are the ones a player can type back (%LOCALAPPDATA%, ~). A line with no personal folder costs one table
// read per byte (log_privacy_find) and is passed on as it is.
//
// log_short_paths = 0 (NX2SARIA_LOG_SHORT_PATHS=0) writes everything in full, for debugging on one's own machine. The
// lines before the settings are read are written short either way.
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "log_privacy.h"
#include "log_privacy_core.h"
#include "platform/platform.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

// host.h's, declared here alone: host.h brings the CPU emulation's headers, which tests/log_privacy_env_check.c leaves out
void rt_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static PrivTable T;
static volatile int g_on = 1;
static volatile unsigned long long g_count;
static int g_ready;

static void add_env(const char *var, int kind, const char *mark) {
    char v[PRIV_PAT_MAX];
    if (plat_getenv(var, v, sizeof v)) priv_add(&T, v, kind, mark);
}

// A host name, and its first label when it has a domain (maria-pc.lan: maria-pc too).
static void add_host(const char *h) {
    char v[256];
    snprintf(v, sizeof v, "%s", h);
    priv_add(&T, v, PRIV_HOST, "%COMPUTERNAME%");
    char *dot = strchr(v, '.');
    if (dot) *dot = 0, priv_add(&T, v, PRIV_HOST, "%COMPUTERNAME%");
}

static void add_host_env(const char *var) {
    char v[256];
    if (plat_getenv(var, v, sizeof v)) add_host(v);
}

#ifdef _WIN32
// The 8.3 form of an existing folder (C:\Users\ANASOU~1), which TEMP and paths from older programs use.
static void add_short(const char *var, const char *mark) {
    char v[PRIV_PAT_MAX], s[PRIV_PAT_MAX];
    wchar_t w[PRIV_PAT_MAX], ws[PRIV_PAT_MAX];
    if (!plat_getenv(var, v, sizeof v) || !MultiByteToWideChar(CP_UTF8, 0, v, -1, w, PRIV_PAT_MAX)) return;
    DWORD n = GetShortPathNameW(w, ws, PRIV_PAT_MAX);
    if (!n || n >= PRIV_PAT_MAX || !WideCharToMultiByte(CP_UTF8, 0, ws, -1, s, sizeof s, NULL, NULL)) return;
    priv_add(&T, s, PRIV_PREFIX, mark);
}
#endif

void log_privacy_init(void) {
    if (g_ready) return;
#ifdef _WIN32
    priv_clear(&T, 1);
    add_env("LOCALAPPDATA", PRIV_PREFIX, "%LOCALAPPDATA%");
    add_env("APPDATA", PRIV_PREFIX, "%APPDATA%");
    add_env("USERPROFILE", PRIV_PREFIX, "%USERPROFILE%");
    add_env("HOMESHARE", PRIV_PREFIX, "%HOMESHARE%");
    add_env("OneDrive", PRIV_PREFIX, "%OneDrive%");
    add_env("TEMP", PRIV_PREFIX, "%TEMP%");
    add_env("TMP", PRIV_PREFIX, "%TEMP%");
    char d[8], p[PRIV_PAT_MAX], both[PRIV_PAT_MAX + 8];
    if (plat_getenv("HOMEDRIVE", d, sizeof d) && plat_getenv("HOMEPATH", p, sizeof p)) {
        snprintf(both, sizeof both, "%s%s", d, p);
        priv_add(&T, both, PRIV_PREFIX, "%USERPROFILE%");
    }
    // Wine hands the Unix environment on: the Unix home, also as the Windows path Z:\home\<name>
    char h[PRIV_PAT_MAX], z[PRIV_PAT_MAX + 4];
    if (plat_getenv("HOME", h, sizeof h) && h[0] == '/') {
        priv_add(&T, h, PRIV_PREFIX, "~");
        snprintf(z, sizeof z, "Z:%s", h);
        for (char *c = z; *c; c++)
            if (*c == '/') *c = '\\';
        priv_add(&T, z, PRIV_PREFIX, "~");
    }
    if (plat_getenv("WINEHOMEDIR", h, sizeof h)) priv_add(&T, !strncmp(h, "\\??\\", 4) ? h + 4 : h, PRIV_PREFIX, "~");
    add_env("USERNAME", PRIV_NAME, "%USERNAME%");
    add_host_env("COMPUTERNAME");
    char cn[256], dom[256];   // USERDOMAIN is the computer's own name on a PC outside a domain, else the domain's
    if (plat_getenv("USERDOMAIN", dom, sizeof dom))
        priv_add(&T, dom, PRIV_HOST, plat_getenv("COMPUTERNAME", cn, sizeof cn) && !_stricmp(cn, dom) ? "%COMPUTERNAME%"
                                                                                                   : "%USERDOMAIN%");
    add_host_env("HOSTNAME");   // Wine hands the Linux environment on
#else
    priv_clear(&T, 0);
    add_env("HOME", PRIV_PREFIX, "~");
    char u[PRIV_PAT_MAX];
    if (plat_getenv("USER", u, sizeof u) || plat_getenv("LOGNAME", u, sizeof u)) priv_add(&T, u, PRIV_NAME, "$USER");
    char h[256];
    if (!gethostname(h, sizeof h)) h[sizeof h - 1] = 0, add_host(h);   // uname: a system call, no allocation
    add_host_env("HOSTNAME");
#endif
    g_ready = 1;
}

void log_privacy_start(void) {
    log_privacy_init();
#ifdef _WIN32
    add_short("USERPROFILE", "%USERPROFILE%");
    add_short("LOCALAPPDATA", "%LOCALAPPDATA%");
#else
    char h[PRIV_PAT_MAX], r[PATH_MAX];
    if (plat_getenv("HOME", h, sizeof h) && realpath(h, r)) priv_add(&T, r, PRIV_PREFIX, "~");
#endif
    g_on = cfg_bool("NX2SARIA_LOG_SHORT_PATHS");
    int pre = 0, name = 0, host = 0;
    for (int i = 0; i < T.n; i++) T.p[i].kind == PRIV_NAME ? name++ : T.p[i].kind == PRIV_HOST ? host++ : pre++;
    if (g_on)
        rt_log("log: personal folders are written short (log_short_paths = 1): %s, %d folder%s, %s and %s; the rest of "
               "each path is kept", T.fold ? "%LOCALAPPDATA%, %APPDATA%, %USERPROFILE% and the like" : "~ for the home",
               pre, pre == 1 ? "" : "s", name ? (T.fold ? "the account name as %USERNAME%" : "the account name as $USER")
                                               : "no account name to look for",
               host ? "the computer's name as %COMPUTERNAME%" : "no computer name to look for");
    else
        rt_log("log: paths are written in full, the account and computer names included (log_short_paths = 0)");
}

int log_privacy_on(void) { return g_on; }

size_t log_privacy_find(const char *p, size_t n) {
    if (!g_on || !g_ready) return n;
    size_t at = priv_find(&T, p, n);
    if (at < n) __atomic_add_fetch(&g_count, 1, __ATOMIC_RELAXED);   // every caller rewrites what it finds
    return at;
}

size_t log_privacy_rewrite(const char *p, size_t n, size_t *at, char *out, size_t cap) {
    return priv_rewrite(&T, p, n, at, out, cap, NULL);
}

const char *log_privacy_str(const char *s, char *out, size_t cap) {
    size_t n = s ? strlen(s) : 0;
    if (!n || !cap || log_privacy_find(s, n) == n) return s;
    size_t at = 0, o = log_privacy_rewrite(s, n, &at, out, cap - 1);
    out[o] = 0;
    return out;
}

const char *log_privacy_str_always(const char *s, char *out, size_t cap) {
    size_t n = s ? strlen(s) : 0;
    if (!n || !cap || !g_ready || priv_find(&T, s, n) == n) return s;
    size_t at = 0, o = log_privacy_rewrite(s, n, &at, out, cap - 1);
    out[o] = 0;
    return out;
}

char *log_privacy_dup(const char *p, size_t n, size_t *out_n) {
    if (!n || log_privacy_find(p, n) == n) return NULL;
    size_t cap = n + n / 4 + 64, o = 0;
    char *out = malloc(cap);
    for (size_t at = 0; out && at < n;) {
        o += log_privacy_rewrite(p, n, &at, out + o, cap - o);
        if (at == n) break;
        char *more = realloc(out, cap * 2);   // the rest did not fit
        if (!more) free(out);
        out = more, cap *= 2;
    }
    if (out) *out_n = o;
    return out;
}

unsigned long long log_privacy_count(void) { return __atomic_load_n(&g_count, __ATOMIC_RELAXED); }
