#pragma once

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define NVC_PATH_VAR "__GL_SHADER_DISK_CACHE_PATH"
#define NVC_SIZE_VAR "__GL_SHADER_DISK_CACHE_SIZE"
#define NVC_SKIP_VAR "__GL_SHADER_DISK_CACHE_SKIP_CLEANUP"
#define NVC_SIZE_VALUE "8589934592"
#define NVC_SKIP_VALUE "1"
#define NVC_FOLDER "nvidia-cache"
#define NVC_MARK_VAR "__NX2SARIA_GL_CACHE_SET"

enum { NVC_SET_PATH = 1, NVC_SET_SIZE = 2, NVC_SET_SKIP = 4 };
enum {
    NVC_USE,
    NVC_NO_DRIVER,
    NVC_OFF,
    NVC_NO_FOLDER,
};

typedef struct {
    int windows;
    int enabled;
    int driver;
    int path_set;
    int size_set;
    int skip_set;
    int folder_ok;
} NvcIn;

typedef struct {
    int verdict;
    int set;
} NvcPlan;

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

static inline int nvc_mark_parse(const char *text) {
    return text && text[0] >= '1' && text[0] <= '7' && !text[1] ? text[0] - '0' : 0;
}

static inline void nvc_mark_text(int bits, char *out, size_t cap) {
    snprintf(out, cap, "%d", bits & 7);
}

static inline int nvc_stale(const NvcPlan *p, int ours) {
    return ours & ~(p->verdict == NVC_USE ? p->set : 0) & 7;
}

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

static inline void nvc_stays_line(char *out, size_t cap, const char *dir, const char *what, const char *why) {
    snprintf(out, cap, "vk: NVIDIA's shader disk cache stays in the driver's own folder with its own limit: %s %s (%s)",
             dir, what, why);
}

static inline void nvc_refused_line(char *out, size_t cap, int bits) {
    snprintf(out, cap, "vk: NVIDIA's shader disk cache: the environment refused %s%s%s%s%s, so the driver keeps its own value",
             bits & NVC_SET_PATH ? NVC_PATH_VAR : "", (bits & NVC_SET_PATH) && (bits & (NVC_SET_SIZE | NVC_SET_SKIP)) ? ", " : "",
             bits & NVC_SET_SIZE ? NVC_SIZE_VAR : "", (bits & NVC_SET_SIZE) && (bits & NVC_SET_SKIP) ? ", " : "",
             bits & NVC_SET_SKIP ? NVC_SKIP_VAR : "");
}

static inline void nvc_off_line(char *out, size_t cap) {
    snprintf(out, cap, "vk: NVIDIA's shader disk cache stays in the driver's own folder with its own limit: "
                       "nvidia_cache_dir = 0");
}

enum { NVC_KIND_FILE = 0, NVC_KIND_DIR = 1, NVC_KIND_LINK = 2 };
enum { NVC_LISTED = 0, NVC_MISSING = 1 };
#define NVC_MAX_ENTRIES 20000u
#define NVC_MAX_DEPTH 8

typedef int (*NvcCallback)(const char *name, int kind, uint64_t size, void *ctx);
typedef int (*NvcLister)(const char *dir, NvcCallback cb, void *ctx);

typedef struct {
    uint64_t bytes, files;
    int unreadable;
    int stopped;
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
    if (kind != NVC_KIND_DIR) {
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

static inline void nvc_commas(uint64_t n, char *out, size_t cap) {
    char digits[24];
    int k = snprintf(digits, sizeof digits, "%llu", (unsigned long long)n), at = 0;
    for (int i = 0; i < k && (size_t)at + 2 < cap; i++) {
        if (i && (k - i) % 3 == 0) out[at++] = ',';
        out[at++] = digits[i];
    }
    out[at] = 0;
}

static inline void nvc_size_text(char *out, size_t cap, const char *dir, const NvcSize *s) {
    char files[32];
    nvc_commas(s->files, files, sizeof files);
    snprintf(out, cap, "NVIDIA's shader disk cache folder %s holds %.2f GB in %s files%s", dir,
             (double)s->bytes / 1073741824.0, files,
             s->stopped ? " (the count stopped)" : s->unreadable ? " (some of it could not be read)" : "");
}
