// NVIDIA's shader disk cache folder: what the game puts into the environment before a driver loads
// (vk_device.c nvidia_cache_beside_exe), how it measures the folder (vk_pipeline.c lw_check) and the lines the logs say
// about both. Plain functions with no driver, environment or file system behind them, so tests/nvidia_cache_check.c
// checks every rule.
//
// NVIDIA's driver keeps what it compiles in a disk cache of its own. By default that is one folder that every program
// shares, and the limit belongs to the cache: a program that opens it later with a smaller limit brings it down to
// that. The shader preparation (ocarina --prepare-shaders) builds about 42,000 variants into it in one process, some
// 1.4 to 1.6 GB, so the game keeps the cache in a folder of its own beside the exe, which no other program opens, with
// a limit of its own and without the cleanup. It sets only what the environment does not already set: a player's own
// folder or limit stands.
//
// Linux: NVIDIA's kernel driver is loaded (/proc/driver/nvidia/version), the folder and the 8 GiB limit, as the log
// line has always said. Windows: NVIDIA's user-mode files are installed (System32\nvapi64.dll), the same folder and
// limit, and the cleanup off as well. Whether the Windows driver reads the variables at all shows in the launcher's log
// (the folder beside the game fills, or the driver's own folder does).
//
// A game that restarts itself (a setting that needs it) starts the new process with the environment of the old one,
// variables the game set included, which the new process would take for the player's: its log would say so, and an
// nvidia_cache_dir = 0 written meanwhile would find the driver already pointed at the folder. So the game names the
// variables it set in one of its own (NVC_MARK_VAR), and a start takes those for the game's, sets them again or takes
// them out of the environment.
#pragma once

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define NVC_PATH_VAR "__GL_SHADER_DISK_CACHE_PATH"
#define NVC_SIZE_VAR "__GL_SHADER_DISK_CACHE_SIZE"
#define NVC_SKIP_VAR "__GL_SHADER_DISK_CACHE_SKIP_CLEANUP"
#define NVC_SIZE_VALUE "8589934592"   // 8 GiB
#define NVC_SKIP_VALUE "1"
#define NVC_FOLDER "nvidia-cache"     // beside the exe
#define NVC_MARK_VAR "__NX2SARIA_GL_CACHE_SET"   // not NX2SARIA_...: platform.h plat_getenv reads those from the settings

enum { NVC_SET_PATH = 1, NVC_SET_SIZE = 2, NVC_SET_SKIP = 4 };   // what the game puts into the environment
enum {
    NVC_USE,         // set what NVC_SET_* says
    NVC_NO_DRIVER,   // no NVIDIA driver: nothing is done and nothing logged
    NVC_OFF,         // nvidia_cache_dir = 0: the driver keeps its own folder and limit
    NVC_NO_FOLDER,   // the folder beside the exe cannot be used: the driver keeps its own folder and limit
};

typedef struct {
    int windows;     // the Windows rules (the cleanup off too)
    int enabled;     // nvidia_cache_dir
    int driver;      // NVIDIA's driver is installed
    int path_set;    // the variables are in the environment and the player's (not the game's own from before a restart)
    int size_set;
    int skip_set;
    int folder_ok;   // the folder beside the exe is there and takes a new file; read only while nvc_need_folder
} NvcIn;

typedef struct {
    int verdict;     // NVC_*
    int set;         // NVC_SET_* bits, with NVC_USE
} NvcPlan;

// 1 when the folder beside the exe has to be made and tested before nvc_plan: the driver is there, the setting is on
// and the player has not named a folder.
static inline int nvc_need_folder(const NvcIn *in) {
    return in->driver && in->enabled && !in->path_set;
}

static inline NvcPlan nvc_plan(const NvcIn *in) {
    NvcPlan p = {NVC_USE, 0};
    if (!in->driver) {
        p.verdict = NVC_NO_DRIVER;
        return p;
    }
    if (!in->enabled) {
        p.verdict = NVC_OFF;
        return p;
    }
    if (!in->path_set) {
        if (!in->folder_ok) {
            p.verdict = NVC_NO_FOLDER;
            return p;
        }
        p.set |= NVC_SET_PATH;
    }
    if (!in->size_set) p.set |= NVC_SET_SIZE;
    if (in->windows && !in->skip_set) p.set |= NVC_SET_SKIP;
    return p;
}

// The variables a start inherits that the game set before (NVC_MARK_VAR's text: one digit, the NVC_SET_* bits); 0 for
// a text that is anything else.
static inline int nvc_mark_parse(const char *text) {
    return text && text[0] >= '1' && text[0] <= '7' && !text[1] ? text[0] - '0' : 0;
}

static inline void nvc_mark_text(int bits, char *out, size_t cap) {
    snprintf(out, cap, "%d", bits & 7);
}

// The variables to take out of the environment: the ones the game set before that this start does not set again (the
// setting is off, the driver is gone, the folder cannot be used).
static inline int nvc_stale(const NvcPlan *p, int ours) {
    return ours & ~(p->verdict == NVC_USE ? p->set : 0) & 7;
}

// The start's line when the game sets variables (NVC_USE). path, size and skip are what the environment holds after
// the start; the three sets say which of them are the game's. Linux keeps the words it has always logged.
static inline void nvc_use_line(char *out, size_t cap, int windows, int set, const char *path, const char *size,
                                const char *skip) {
    if (!windows) {
        snprintf(out, cap, "vk: NVIDIA's shader disk cache in %s (%s), up to %s bytes (%s)", path,
                 set & NVC_SET_PATH ? "beside the exe" : NVC_PATH_VAR, size,
                 set & NVC_SET_SIZE ? "the game's limit" : NVC_SIZE_VAR);
        return;
    }
    snprintf(out, cap,
             "vk: NVIDIA's shader disk cache: " NVC_PATH_VAR "=%s (%s), " NVC_SIZE_VAR "=%s (%s), " NVC_SKIP_VAR
             "=%s (%s)",
             path, set & NVC_SET_PATH ? "the game's, beside the exe" : "the player's, from the environment", size,
             set & NVC_SET_SIZE ? "the game's" : "the player's, from the environment", skip,
             set & NVC_SET_SKIP ? "the game's" : "the player's, from the environment");
}

// The start's line when the folder beside the exe cannot be used: dir, what is wrong with it ("is not writable",
// "cannot be made", ...) and the system's words for it.
static inline void nvc_stays_line(char *out, size_t cap, const char *dir, const char *what, const char *why) {
    snprintf(out, cap, "vk: NVIDIA's shader disk cache stays in the driver's own folder with its own limit: %s %s (%s)",
             dir, what, why);
}

// The start's line when the environment did not take a variable (bits of NVC_SET_*): what the log would otherwise say
// the game set is not in force.
static inline void nvc_refused_line(char *out, size_t cap, int bits) {
    snprintf(out, cap, "vk: NVIDIA's shader disk cache: the environment refused %s%s%s%s%s, so the driver keeps its own value",
             bits & NVC_SET_PATH ? NVC_PATH_VAR : "", (bits & NVC_SET_PATH) && (bits & (NVC_SET_SIZE | NVC_SET_SKIP)) ? ", " : "",
             bits & NVC_SET_SIZE ? NVC_SIZE_VAR : "", (bits & NVC_SET_SIZE) && (bits & NVC_SET_SKIP) ? ", " : "",
             bits & NVC_SET_SKIP ? NVC_SKIP_VAR : "");
}

// The start's line when nvidia_cache_dir is 0.
static inline void nvc_off_line(char *out, size_t cap) {
    snprintf(out, cap, "vk: NVIDIA's shader disk cache stays in the driver's own folder with its own limit: "
                       "nvidia_cache_dir = 0");
}

// ---- what a folder holds

enum { NVC_KIND_FILE = 0, NVC_KIND_DIR = 1, NVC_KIND_LINK = 2 };   // platform.h PLAT_FS_FILE, PLAT_FS_DIR, PLAT_FS_LINK
enum { NVC_LISTED = 0, NVC_MISSING = 1 };                         // platform.h PLAT_FS_OK, PLAT_FS_NOT_FOUND
#define NVC_MAX_ENTRIES 20000u    // entries looked at before the count stops: the folder holds a few files, but a player may name any
#define NVC_MAX_DEPTH 8

typedef int (*NvcCallback)(const char *name, int kind, uint64_t size, void *ctx);
// The shape of platform.h plat_fs_list: cb for each entry of dir; the listing's error (NVC_LISTED, NVC_MISSING, other).
typedef int (*NvcLister)(const char *dir, NvcCallback cb, void *ctx);

typedef struct {
    uint64_t bytes, files;
    int unreadable;   // the system refused a folder
    int stopped;      // NVC_MAX_ENTRIES or NVC_MAX_DEPTH came first
} NvcSize;

typedef struct {
    NvcLister list;
    NvcSize *out;
    char *path;
    size_t cap;
    uint64_t seen;
    int depth;
    char sep;
} NvcWalk;

static inline void nvc_dir(NvcWalk *w);

static inline int nvc_entry(const char *name, int kind, uint64_t size, void *ctx) {
    NvcWalk *w = (NvcWalk *)ctx;
    if (w->out->stopped) return 0;
    if (++w->seen > NVC_MAX_ENTRIES) {
        w->out->stopped = 1;
        return 0;
    }
    if (kind != NVC_KIND_DIR) {   // a link is counted as the file it is, not followed
        w->out->bytes += size;
        w->out->files++;
        return 1;
    }
    size_t len = strlen(w->path), n = strlen(name);
    if (w->depth >= NVC_MAX_DEPTH) {
        w->out->stopped = 1;
        return 0;
    }
    if (len + 1 + n + 1 > w->cap) {
        w->out->unreadable = 1;
        return 1;
    }
    w->path[len] = w->sep;
    memcpy(w->path + len + 1, name, n + 1);
    w->depth++;
    nvc_dir(w);
    w->depth--;
    w->path[len] = 0;
    return !w->out->stopped;
}

static inline void nvc_dir(NvcWalk *w) {
    int r = w->list(w->path, nvc_entry, w);
    if (r != NVC_LISTED && r != NVC_MISSING) w->out->unreadable = 1;
}

// What lies under root: bytes and files (a folder that is not there is 0 and 0). work: a buffer of cap bytes the walk
// extends and gives back, which holds at least root and its longest path.
static inline void nvc_measure(NvcLister list, const char *root, char *work, size_t cap, NvcSize *out) {
    memset(out, 0, sizeof *out);
    size_t n = strlen(root);
    if (n + 1 > cap) {
        out->unreadable = 1;
        return;
    }
    memcpy(work, root, n + 1);
    NvcWalk w = {list, out, work, cap, 0, 0, strchr(root, '\\') ? '\\' : '/'};
    nvc_dir(&w);
}

// 12345 as "12,345".
static inline void nvc_commas(uint64_t n, char *out, size_t cap) {
    char digits[24];
    int k = snprintf(digits, sizeof digits, "%llu", (unsigned long long)n), at = 0;
    for (int i = 0; i < k && (size_t)at + 2 < cap; i++) {
        if (i && (k - i) % 3 == 0) out[at++] = ',';
        out[at++] = digits[i];
    }
    out[at] = 0;
}

// The words after "library warm: " in the game's log: the folder and what it holds, in GB as the launcher's log counts
// (a gibibyte), and what the count missed.
static inline void nvc_size_text(char *out, size_t cap, const char *dir, const NvcSize *s) {
    char files[32];
    nvc_commas(s->files, files, sizeof files);
    snprintf(out, cap, "NVIDIA's shader disk cache folder %s holds %.2f GB in %s files%s", dir,
             (double)s->bytes / 1073741824.0, files,
             s->stopped ? " (the count stopped)" : s->unreadable ? " (some of it could not be read)" : "");
}
