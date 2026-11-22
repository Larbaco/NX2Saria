// runtime/nvidia_cache.h: what the game puts into the environment for NVIDIA's shader disk cache (runtime/vk_device.c
// nvidia_cache_beside_exe), how it counts the folder (vk_pipeline.c lw_say_cache_folder) and the lines the logs say about
// both. Built and run by tests/nvidia_cache_check.py; prints each failed check and exits 1 on any.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "nvidia_cache.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define N(a) ((int)(sizeof (a) / sizeof *(a)))

static NvcIn in_of(int windows, int enabled, int driver, int path_set, int size_set, int skip_set, int folder_ok) {
    NvcIn in = {windows, enabled, driver, path_set, size_set, skip_set, folder_ok};
    return in;
}

// ---- a tree the lister answers from

typedef struct { const char *dir, *name; int kind; uint64_t size; } Ent;
static const Ent *tree;
static int n_tree;
static const char *const *empties;   // folders that are there and hold nothing
static const char *const *denied;    // folders the system refuses
static int listed;                    // lister calls

static int in_list(const char *const *l, const char *s) {
    for (; l && *l; l++)
        if (!strcmp(*l, s)) return 1;
    return 0;
}

static int fake_list(const char *dir, NvcCallback cb, void *ctx) {
    listed++;
    if (in_list(denied, dir)) return 5;   // platform.h PLAT_FS_ERROR
    int there = in_list(empties, dir);
    for (int i = 0; i < n_tree; i++) {
        if (!strcmp(tree[i].dir, dir)) there = 1;
        if (strcmp(tree[i].dir, dir)) continue;
        if (!cb(tree[i].name, tree[i].kind, tree[i].size, ctx)) break;
    }
    for (int i = 0; i < n_tree && !there; i++)   // a folder some entry sits in below it exists too
        there = !strncmp(tree[i].dir, dir, strlen(dir)) && (tree[i].dir[strlen(dir)] == '/' || tree[i].dir[strlen(dir)] == '\\');
    return there ? NVC_LISTED : NVC_MISSING;
}

static uint64_t callbacks;
static int many_list(const char *dir, NvcCallback cb, void *ctx) {
    (void)dir;
    for (uint64_t i = 0; i < NVC_MAX_ENTRIES + 50; i++) {
        callbacks++;
        if (!cb("f", NVC_KIND_FILE, 1, ctx)) break;
    }
    return NVC_LISTED;
}

static int deaf_list(const char *dir, NvcCallback cb, void *ctx) {   // goes on after the callback says stop
    (void)dir;
    for (uint64_t i = 0; i < NVC_MAX_ENTRIES + 50; i++) {
        callbacks++;
        cb("f", NVC_KIND_FILE, 1, ctx);
    }
    return NVC_LISTED;
}

static int deep_deaf_list(const char *dir, NvcCallback cb, void *ctx) {   // a folder and a file in each, whatever cb says
    (void)dir;
    listed++;
    cb("d", NVC_KIND_DIR, 0, ctx);
    cb("f", NVC_KIND_FILE, 1, ctx);
    return NVC_LISTED;
}

static int deep_list(const char *dir, NvcCallback cb, void *ctx) {   // one folder inside each one, for ever
    (void)dir;
    listed++;
    cb("d", NVC_KIND_DIR, 0, ctx);
    return NVC_LISTED;
}

static NvcSize measure(NvcLister l, const char *root, size_t cap) {
    char work[512];
    NvcSize s;
    char sentinel = 0x5a;
    work[cap] = sentinel;   // the walk never writes past cap
    nvc_measure(l, root, work, cap, &s);
    CHECK(work[cap] == sentinel, "the walk wrote past its buffer");
    CHECK(cap < strlen(root) + 1 || !strcmp(work, root), "the buffer holds the root again: %s", work);
    return s;
}

int main(void) {
    // ---- the plan: scenarios written out by hand. in_of(windows, enabled, driver, path_set, size_set, skip_set, folder_ok)
    {
        static const struct { const char *what; NvcIn in; int verdict, set; } rows[] = {
            {"Linux, nothing set: the folder and the limit", {0, 1, 1, 0, 0, 0, 1}, NVC_USE, NVC_SET_PATH | NVC_SET_SIZE},
            {"Linux, no NVIDIA driver: nothing, silent", {0, 1, 0, 0, 0, 0, 1}, NVC_NO_DRIVER, 0},
            {"Linux, the player's limit stands", {0, 1, 1, 0, 1, 0, 1}, NVC_USE, NVC_SET_PATH},
            {"Linux, the player's folder stands, the limit is the game's", {0, 1, 1, 1, 0, 0, 1}, NVC_USE, NVC_SET_SIZE},
            {"Linux, both the player's", {0, 1, 1, 1, 1, 0, 1}, NVC_USE, 0},
            {"Linux, the folder cannot be used", {0, 1, 1, 0, 0, 0, 0}, NVC_NO_FOLDER, 0},
            {"Linux, off", {0, 0, 1, 0, 0, 0, 1}, NVC_OFF, 0},
            {"Windows, nothing set: folder, limit and no cleanup", {1, 1, 1, 0, 0, 0, 1}, NVC_USE,
             NVC_SET_PATH | NVC_SET_SIZE | NVC_SET_SKIP},
            {"Windows, no NVIDIA driver", {1, 1, 0, 0, 0, 0, 1}, NVC_NO_DRIVER, 0},
            {"Windows, the player's cleanup setting stands", {1, 1, 1, 0, 0, 1, 1}, NVC_USE, NVC_SET_PATH | NVC_SET_SIZE},
            {"Windows, the player's folder stands", {1, 1, 1, 1, 0, 0, 1}, NVC_USE, NVC_SET_SIZE | NVC_SET_SKIP},
            {"Windows, all three the player's", {1, 1, 1, 1, 1, 1, 1}, NVC_USE, 0},
            {"Windows, the folder cannot be used", {1, 1, 1, 0, 0, 0, 0}, NVC_NO_FOLDER, 0},
            {"Windows, the folder cannot be used but the player's stands: never asked", {1, 1, 1, 1, 0, 0, 0}, NVC_USE,
             NVC_SET_SIZE | NVC_SET_SKIP},
            {"Windows, off", {1, 0, 1, 0, 0, 0, 1}, NVC_OFF, 0},
            {"Windows, off and no driver: the silence wins", {1, 0, 0, 0, 0, 0, 1}, NVC_NO_DRIVER, 0},
        };
        for (int k = 0; k < N(rows); k++) {
            NvcPlan p = nvc_plan(&rows[k].in);
            CHECK(p.verdict == rows[k].verdict && p.set == rows[k].set, "%s: verdict %d set %d, wanted %d and %d", rows[k].what,
                  p.verdict, p.set, rows[k].verdict, rows[k].set);
        }
    }

    // ---- the plan: what holds for every combination of the seven inputs
    for (int bits = 0; bits < 128; bits++) {
        NvcIn in = in_of(bits & 1, bits >> 1 & 1, bits >> 2 & 1, bits >> 3 & 1, bits >> 4 & 1, bits >> 5 & 1, bits >> 6 & 1);
        NvcPlan p = nvc_plan(&in);
        CHECK(nvc_need_folder(&in) == (in.driver && in.enabled && !in.path_set), "need_folder for %d", bits);
        CHECK(!(p.set & NVC_SET_PATH) || (!in.path_set && in.folder_ok), "the game's folder only when it is not the player's and usable: %d", bits);
        CHECK(!in.path_set || !(p.set & NVC_SET_PATH), "never over the player's folder: %d", bits);
        CHECK(!in.size_set || !(p.set & NVC_SET_SIZE), "never over the player's limit: %d", bits);
        CHECK(!in.skip_set || !(p.set & NVC_SET_SKIP), "never over the player's cleanup setting: %d", bits);
        CHECK(in.windows || !(p.set & NVC_SET_SKIP), "the cleanup is the Windows rule alone: %d", bits);
        CHECK(p.verdict == NVC_USE || p.set == 0, "nothing set without a use: %d", bits);
        CHECK(p.verdict != NVC_USE || (in.driver && in.enabled), "a use needs the driver and the setting: %d", bits);
        CHECK((p.verdict == NVC_USE) == (in.driver && in.enabled && (in.path_set || in.folder_ok)), "the verdict: %d", bits);
        if (!nvc_need_folder(&in)) {   // the folder's state is not asked, so it changes nothing
            NvcIn other = in;
            other.folder_ok = !in.folder_ok;
            NvcPlan q = nvc_plan(&other);
            CHECK(q.verdict == p.verdict && q.set == p.set, "folder_ok is read only when the folder is needed: %d", bits);
        }
        if (in.windows == 0) {   // Linux never differs from what it did before: the folder, then the limit
            int want = (in.path_set ? 0 : NVC_SET_PATH) | (in.size_set ? 0 : NVC_SET_SIZE);
            CHECK(p.verdict != NVC_USE || p.set == want, "Linux sets the folder and the limit as before: %d", bits);
        }
    }

    // ---- the variables a restart inherits: the game names what it set, and a start takes those for the game's
    {
        static const struct { const char *text; int bits; } marks[] = {
            {"1", 1}, {"2", 2}, {"3", 3}, {"4", 4}, {"5", 5}, {"6", 6}, {"7", 7}, {"0", 0}, {"8", 0}, {"9", 0}, {"", 0},
            {"12", 0}, {"x", 0}, {" 3", 0}, {"3 ", 0}, {"-1", 0}, {"07", 0},
        };
        for (int k = 0; k < N(marks); k++)
            CHECK(nvc_mark_parse(marks[k].text) == marks[k].bits, "mark \"%s\": %d, wanted %d", marks[k].text,
                  nvc_mark_parse(marks[k].text), marks[k].bits);
        CHECK(nvc_mark_parse(NULL) == 0, "no mark");
        for (int bits = 0; bits < 8; bits++) {
            char t[8];
            nvc_mark_text(bits, t, sizeof t);
            CHECK(nvc_mark_parse(t) == bits || bits == 0, "the mark of %d reads back as %d (%s)", bits, nvc_mark_parse(t), t);
        }
        char t[8];
        nvc_mark_text(9, t, sizeof t);
        CHECK(!strcmp(t, "1"), "bits past the three are cut: %s", t);

        // nvc_stale: what the game set before and does not set now is taken out of the environment
        NvcPlan use_all = {NVC_USE, NVC_SET_PATH | NVC_SET_SIZE | NVC_SET_SKIP}, use_two = {NVC_USE, NVC_SET_PATH | NVC_SET_SIZE};
        NvcPlan off = {NVC_OFF, 0}, nofolder = {NVC_NO_FOLDER, 0}, nodriver = {NVC_NO_DRIVER, 0};
        CHECK(nvc_stale(&use_all, 7) == 0, "all set again: nothing stale");
        CHECK(nvc_stale(&use_two, 7) == NVC_SET_SKIP, "the cleanup is not set again: it goes");
        CHECK(nvc_stale(&use_two, 0) == 0, "nothing was the game's before: nothing goes");
        CHECK(nvc_stale(&off, 7) == 7, "off: all that the game set goes");
        CHECK(nvc_stale(&off, 0) == 0, "off with nothing of the game's: the player's stay");
        CHECK(nvc_stale(&off, 3) == 3 && nvc_stale(&nofolder, 5) == 5 && nvc_stale(&nodriver, 6) == 6, "any other verdict too");
        CHECK(nvc_stale(&off, 15) == 7, "only the three variables");
    }

    // ---- the lines
    {
        char got[512], want[512];
        const char *dir = "/home/ana/nx2saria/nvidia-cache";
        // Linux: the words the log has always had, built here with the old format
        nvc_use_line(got, sizeof got, 0, NVC_SET_PATH | NVC_SET_SIZE, dir, NVC_SIZE_VALUE, "");
        snprintf(want, sizeof want, "vk: NVIDIA's shader disk cache in %s (%s), up to %s bytes (%s)", dir, "beside the exe",
                 "8589934592", "the game's limit");
        CHECK(!strcmp(got, want), "Linux, the game's folder and limit: %s", got);
        nvc_use_line(got, sizeof got, 0, NVC_SET_SIZE, "/data/gl", NVC_SIZE_VALUE, "");
        snprintf(want, sizeof want, "vk: NVIDIA's shader disk cache in %s (%s), up to %s bytes (%s)", "/data/gl",
                 "__GL_SHADER_DISK_CACHE_PATH", "8589934592", "the game's limit");
        CHECK(!strcmp(got, want), "Linux, the player's folder: %s", got);
        nvc_use_line(got, sizeof got, 0, NVC_SET_PATH, dir, "2147483648", "");
        snprintf(want, sizeof want, "vk: NVIDIA's shader disk cache in %s (%s), up to %s bytes (%s)", dir, "beside the exe",
                 "2147483648", "__GL_SHADER_DISK_CACHE_SIZE");
        CHECK(!strcmp(got, want), "Linux, the player's limit: %s", got);
        nvc_use_line(got, sizeof got, 0, 0, "/data/gl", "2147483648", "");
        snprintf(want, sizeof want, "vk: NVIDIA's shader disk cache in %s (%s), up to %s bytes (%s)", "/data/gl",
                 "__GL_SHADER_DISK_CACHE_PATH", "2147483648", "__GL_SHADER_DISK_CACHE_SIZE");
        CHECK(!strcmp(got, want), "Linux, both the player's: %s", got);
        nvc_use_line(got, sizeof got, 0, NVC_SET_PATH | NVC_SET_SIZE | NVC_SET_SKIP, dir, NVC_SIZE_VALUE, "1");
        nvc_use_line(want, sizeof want, 0, NVC_SET_PATH | NVC_SET_SIZE, dir, NVC_SIZE_VALUE, "9");
        CHECK(!strcmp(got, want), "Linux says nothing of the cleanup: %s", got);

        // Windows: the three variables, with their values and whose they are
        const char *wdir = "C:\\Users\\ana\\AppData\\Local\\Programs\\NX2Saria\\game\\nvidia-cache";
        nvc_use_line(got, sizeof got, 1, NVC_SET_PATH | NVC_SET_SIZE | NVC_SET_SKIP, wdir, NVC_SIZE_VALUE, NVC_SKIP_VALUE);
        snprintf(want, sizeof want,
                 "vk: NVIDIA's shader disk cache: __GL_SHADER_DISK_CACHE_PATH=%s (the game's, beside the exe), "
                 "__GL_SHADER_DISK_CACHE_SIZE=8589934592 (the game's), __GL_SHADER_DISK_CACHE_SKIP_CLEANUP=1 (the game's)",
                 wdir);
        CHECK(!strcmp(got, want), "Windows, all the game's: %s", got);
        nvc_use_line(got, sizeof got, 1, NVC_SET_SIZE | NVC_SET_SKIP, "D:\\gl", NVC_SIZE_VALUE, NVC_SKIP_VALUE);
        CHECK(strstr(got, "__GL_SHADER_DISK_CACHE_PATH=D:\\gl (the player's, from the environment), ") &&
                  strstr(got, "__GL_SHADER_DISK_CACHE_SIZE=8589934592 (the game's), "),
              "Windows, the player's folder: %s", got);
        nvc_use_line(got, sizeof got, 1, NVC_SET_PATH | NVC_SET_SIZE, wdir, NVC_SIZE_VALUE, "0");
        CHECK(strstr(got, "__GL_SHADER_DISK_CACHE_SKIP_CLEANUP=0 (the player's, from the environment)"), "Windows, the player's cleanup: %s", got);
        nvc_use_line(got, sizeof got, 1, 0, "D:\\gl", "1073741824", "0");
        CHECK(!strstr(got, "the game's"), "Windows, all the player's: %s", got);

        // the lines when nothing is set
        nvc_stays_line(got, sizeof got, dir, "is not writable", "Permission denied");
        snprintf(want, sizeof want,
                 "vk: NVIDIA's shader disk cache stays in the driver's own folder with its own limit: %s %s (%s)", dir,
                 "is not writable", "Permission denied");
        CHECK(!strcmp(got, want), "the stays line is the old one: %s", got);
        nvc_off_line(got, sizeof got);
        CHECK(!strcmp(got, "vk: NVIDIA's shader disk cache stays in the driver's own folder with its own limit: "
                           "nvidia_cache_dir = 0"),
              "the off line: %s", got);

        // the line when the environment refuses a variable
        static const struct { int bits; const char *names; } refused[] = {
            {NVC_SET_PATH, "__GL_SHADER_DISK_CACHE_PATH"}, {NVC_SET_SIZE, "__GL_SHADER_DISK_CACHE_SIZE"},
            {NVC_SET_SKIP, "__GL_SHADER_DISK_CACHE_SKIP_CLEANUP"},
            {NVC_SET_PATH | NVC_SET_SIZE, "__GL_SHADER_DISK_CACHE_PATH, __GL_SHADER_DISK_CACHE_SIZE"},
            {NVC_SET_PATH | NVC_SET_SKIP, "__GL_SHADER_DISK_CACHE_PATH, __GL_SHADER_DISK_CACHE_SKIP_CLEANUP"},
            {NVC_SET_SIZE | NVC_SET_SKIP, "__GL_SHADER_DISK_CACHE_SIZE, __GL_SHADER_DISK_CACHE_SKIP_CLEANUP"},
            {7, "__GL_SHADER_DISK_CACHE_PATH, __GL_SHADER_DISK_CACHE_SIZE, __GL_SHADER_DISK_CACHE_SKIP_CLEANUP"},
        };
        for (int k = 0; k < N(refused); k++) {
            nvc_refused_line(got, sizeof got, refused[k].bits);
            snprintf(want, sizeof want, "vk: NVIDIA's shader disk cache: the environment refused %s, so the driver keeps its own value",
                     refused[k].names);
            CHECK(!strcmp(got, want), "refused %d: %s", refused[k].bits, got);
        }

        // a buffer too short takes what fits, ends with a NUL and is not passed
        for (size_t cap = 1; cap < 60; cap += 7) {
            char b[80];
            memset(b, 0x5a, sizeof b);
            nvc_use_line(b, cap, 1, NVC_SET_PATH, wdir, NVC_SIZE_VALUE, NVC_SKIP_VALUE);
            CHECK(strlen(b) < cap && b[cap] == 0x5a, "use line, cap %zu", cap);
            memset(b, 0x5a, sizeof b);
            nvc_use_line(b, cap, 0, NVC_SET_PATH, wdir, NVC_SIZE_VALUE, NVC_SKIP_VALUE);
            CHECK(strlen(b) < cap && b[cap] == 0x5a, "Linux use line, cap %zu", cap);
            memset(b, 0x5a, sizeof b);
            nvc_stays_line(b, cap, dir, "is not writable", "x");
            CHECK(strlen(b) < cap && b[cap] == 0x5a, "stays line, cap %zu", cap);
            memset(b, 0x5a, sizeof b);
            nvc_off_line(b, cap);
            CHECK(strlen(b) < cap && b[cap] == 0x5a, "off line, cap %zu", cap);
            memset(b, 0x5a, sizeof b);
            nvc_refused_line(b, cap, 7);
            CHECK(strlen(b) < cap && b[cap] == 0x5a, "refused line, cap %zu", cap);
        }
    }

    // ---- nvc_commas
    {
        char b[32];
        static const struct { uint64_t n; const char *s; } rows[] = {
            {0, "0"}, {7, "7"}, {999, "999"}, {1000, "1,000"}, {12345, "12,345"}, {41203, "41,203"}, {100000, "100,000"},
            {1234567, "1,234,567"}, {18446744073709551615ull, "18,446,744,073,709,551,615"},
        };
        for (int k = 0; k < N(rows); k++) {
            nvc_commas(rows[k].n, b, sizeof b);
            CHECK(!strcmp(b, rows[k].s), "%llu as %s, got %s", (unsigned long long)rows[k].n, rows[k].s, b);
        }
        char t[5];
        nvc_commas(1234567, t, sizeof t);   // cut, never overrun
        CHECK(strlen(t) < sizeof t, "a short buffer: %s", t);
        for (size_t cap = 1; cap < 30; cap++) {
            char z[40];
            memset(z, 0x5a, sizeof z);
            nvc_commas(18446744073709551615ull, z, cap);
            CHECK(strlen(z) < cap && z[cap] == 0x5a, "commas, cap %zu", cap);
        }
    }

    // ---- nvc_size_text
    {
        char got[400];
        NvcSize s = {1739461181ull, 2, 0, 0};   // 1.62 GiB
        nvc_size_text(got, sizeof got, "/x/nvidia-cache", &s);
        CHECK(!strcmp(got, "NVIDIA's shader disk cache folder /x/nvidia-cache holds 1.62 GB in 2 files"), "%s", got);
        NvcSize e = {0, 0, 0, 0};
        nvc_size_text(got, sizeof got, "/x", &e);
        CHECK(!strcmp(got, "NVIDIA's shader disk cache folder /x holds 0.00 GB in 0 files"), "an empty folder: %s", got);
        NvcSize big = {1470000000ull, 41203, 0, 0};
        nvc_size_text(got, sizeof got, "/x", &big);
        CHECK(strstr(got, "holds 1.37 GB in 41,203 files") && !strstr(got, "("), "a count with a thousands separator: %s", got);
        NvcSize st = {5, 1, 1, 1};
        nvc_size_text(got, sizeof got, "/x", &st);
        CHECK(strstr(got, "(the count stopped)") && !strstr(got, "could not be read"), "a stopped count says so, once: %s", got);
        NvcSize un = {5, 1, 1, 0};
        nvc_size_text(got, sizeof got, "/x", &un);
        CHECK(strstr(got, "(some of it could not be read)"), "%s", got);
    }

    // ---- nvc_measure
    {
        static const Ent files[] = {
            {"/c", "a.toc", NVC_KIND_FILE, 100},   {"/c", "a.bin", NVC_KIND_FILE, 5000}, {"/c", "sub", NVC_KIND_DIR, 4096},
            {"/c/sub", "b.bin", NVC_KIND_FILE, 700}, {"/c/sub", "deeper", NVC_KIND_DIR, 0},
            {"/c/sub/deeper", "c.bin", NVC_KIND_FILE, 30}, {"/c", "link", NVC_KIND_LINK, 11},
        };
        tree = files, n_tree = N(files), empties = NULL, denied = NULL;
        NvcSize s = measure(fake_list, "/c", 200);
        CHECK(s.bytes == 100 + 5000 + 700 + 30 + 11 && s.files == 5 && !s.unreadable && !s.stopped,
              "a tree: %llu bytes in %llu files (a link is counted as the file it is, a folder's own size not at all)",
              (unsigned long long)s.bytes, (unsigned long long)s.files);

        s = measure(fake_list, "/missing", 200);
        CHECK(s.bytes == 0 && s.files == 0 && !s.unreadable && !s.stopped, "a folder that is not there is 0 and 0, said of nothing");

        static const char *const none[] = {"/c/empty", NULL};
        static const Ent one[] = {{"/c", "empty", NVC_KIND_DIR, 0}, {"/c", "f", NVC_KIND_FILE, 9}};
        tree = one, n_tree = N(one), empties = none;
        s = measure(fake_list, "/c", 200);
        CHECK(s.bytes == 9 && s.files == 1 && !s.unreadable, "an empty folder inside: %llu in %llu", (unsigned long long)s.bytes,
              (unsigned long long)s.files);
        empties = NULL;

        static const char *const refused[] = {"/c/sub", NULL};
        tree = files, n_tree = N(files), denied = refused;
        s = measure(fake_list, "/c", 200);
        CHECK(s.unreadable && !s.stopped && s.bytes == 100 + 5000 + 11 && s.files == 3,
              "a folder the system refuses is said, the rest is counted: %llu in %llu", (unsigned long long)s.bytes,
              (unsigned long long)s.files);
        static const char *const root_refused[] = {"/c", NULL};
        denied = root_refused;
        s = measure(fake_list, "/c", 200);
        CHECK(s.unreadable && s.bytes == 0 && s.files == 0, "a root the system refuses");
        denied = NULL;

        // Windows paths join with a backslash, the root's own kind
        static const Ent win[] = {{"C:\\gl", "GLCache", NVC_KIND_DIR, 0}, {"C:\\gl\\GLCache", "x.bin", NVC_KIND_FILE, 77}};
        tree = win, n_tree = N(win);
        s = measure(fake_list, "C:\\gl", 200);
        CHECK(s.bytes == 77 && s.files == 1 && !s.unreadable, "a backslash path: %llu in %llu", (unsigned long long)s.bytes,
              (unsigned long long)s.files);

        // the path buffer: a folder whose path does not fit is said, the rest counted
        static const Ent tight[] = {{"/a", "bb", NVC_KIND_DIR, 0}, {"/a", "f", NVC_KIND_FILE, 3}, {"/a/bb", "g", NVC_KIND_FILE, 4}};
        tree = tight, n_tree = N(tight);
        s = measure(fake_list, "/a", 5);   // "/a/bb" needs 6 bytes with its NUL
        CHECK(s.unreadable && s.files == 1 && s.bytes == 3, "a path past the buffer: %llu in %llu, unreadable %d",
              (unsigned long long)s.bytes, (unsigned long long)s.files, s.unreadable);
        s = measure(fake_list, "/a", 6);
        CHECK(!s.unreadable && s.files == 2 && s.bytes == 7, "a path that just fits");
        s = measure(fake_list, "/a", 2);   // the root alone does not fit
        CHECK(s.unreadable && s.files == 0, "a root past the buffer");
        s = measure(fake_list, "/a", 3);   // the root fits to the last byte, "/a/bb" does not
        CHECK(s.unreadable && s.files == 1 && s.bytes == 3, "a root that just fits: %llu in %llu, unreadable %d",
              (unsigned long long)s.bytes, (unsigned long long)s.files, s.unreadable);

        // the entry limit
        callbacks = 0;
        s = measure(many_list, "/m", 200);
        CHECK(s.stopped && s.files == NVC_MAX_ENTRIES && callbacks == NVC_MAX_ENTRIES + 1,
              "the entry limit: %llu files after %llu entries", (unsigned long long)s.files, (unsigned long long)callbacks);

        // a lister that goes on after the callback said stop is not counted past the limit
        callbacks = 0;
        s = measure(deaf_list, "/m", 200);
        CHECK(s.stopped && s.files == NVC_MAX_ENTRIES, "a lister that does not stop: %llu files", (unsigned long long)s.files);

        // the depth limit: a folder inside every folder, for ever
        listed = 0;
        s = measure(deep_list, "/d", 500);
        CHECK(s.stopped && listed == NVC_MAX_DEPTH + 1, "the depth limit: %d folders listed", listed);
        // and nothing is counted after it, whatever the lister does with the callback's answer
        listed = 0;
        s = measure(deep_deaf_list, "/d", 500);
        CHECK(s.stopped && s.files == 0, "nothing counted once the count stopped: %llu files", (unsigned long long)s.files);
    }

    if (fails) printf("%d checks failed\n", fails);
    else printf("nvidia_cache_check: all checks passed\n");
    return fails != 0;
}
