#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cfg.h"
#include "cheat_stats.h"
#include "cheats.h"
#include "host.h"
#include "input.h"
#include "mods.h"
#include "platform/platform.h"

#ifdef _WIN32
#define SEP "\\"
#else
#define SEP "/"
#endif

enum { STATUS_MAX = 160, OPS_PER_RUN = 1 << 20 };

typedef struct {
    CheatInfo info;
    uint32_t *words;
    int n;
    int dup;
    uint32_t content;
    int set;
    volatile int32_t on;
    CheatStats stats;
    char note[STATUS_MAX];
} Cheat;

static Cheat *g_cheats;
static int g_n, g_cap, g_vm;
static char g_file[64];
static int g_files;
static uint64_t g_main, g_main_size, g_code_end;

static uint32_t fnv(const char *s, size_t n) {
    uint32_t h = 0x811C9DC5u;
    for (size_t i = 0; i < n; i++) h = (h ^ (uint8_t)s[i]) * 0x01000193u;
    return h;
}

static int exe_dir(char *out, size_t n) {
    if (!plat_exe_path(out, n)) return 0;
    char *cut = plat_last_sep(out);
    if (!cut) return 0;
    cut[1] = 0;
    return 1;
}

static int build_id(char out[17]) {
    char path[PLAT_PATH_MAX];
    snprintf(path, sizeof path, "%s/main", rt_config()->exefs);
    PlatFile *f = plat_file_open(path);
    if (!f) return 0;
    uint8_t head[0x48];
    int ok = plat_file_pread(f, head, sizeof head, 0) == sizeof head && !memcmp(head, "NSO0", 4);
    plat_file_close(f);
    for (int i = 0; ok && i < 8; i++) snprintf(out + 2 * i, 3, "%02X", head[0x40 + i]);
    return ok;
}

static int op_len(const uint32_t *w, int left) {
    uint32_t f = w[0];
    int t = f >> 28, width = (f >> 24) & 0xF, n;
    switch (t) {
    case 0: case 1: n = width == 8 ? 4 : 3; break;
    case 2: case 8: n = 1; break;
    case 3: n = width == 0 ? 2 : 1; break;
    case 4: case 6: n = 3; break;
    case 5: case 7: n = 2; break;
    case 9: n = 1 + (((f >> 8) & 0xF) ? (width == 8 ? 2 : 1) : 0); break;
    case 0xA: n = ((f >> 8) & 0xF) == 2 || ((f >> 8) & 0xF) == 4 || ((f >> 8) & 0xF) == 5 ? 2 : 1; break;
    default: n = 1; break;
    }
    return n <= left ? n : 0;
}

static const char *vm_unsupported(const uint32_t *w, char *buf, int n) {
    uint32_t f = w[0];
    int t = f >> 28, width = (f >> 24) & 0xF;
    if (t > 0xA || (t == 5 && ((f >> 12) & 0xF) > 1) || (t == 0xA && ((f >> 8) & 0xF) > 5)) {
        snprintf(buf, n, "opcode %08X is not supported", f);
        return buf;
    }
    if ((t <= 1 || t == 5 || t == 6 || t == 7 || t == 9 || t == 0xA) && width != 1 && width != 2 && width != 4 &&
        width != 8) {
        snprintf(buf, n, "opcode %08X has a bad width", f);
        return buf;
    }
    return NULL;
}

static CheatKind classify(Cheat *c) {
    int ops = 0, ends = 0;
    for (int i = 0; i < c->n;) {
        int len = op_len(c->words + i, c->n - i);
        if (!len) {
            snprintf(c->note, sizeof c->note, "opcode %08X is cut short", c->words[i]);
            return CHEAT_UNSUPPORTED;
        }
        uint32_t f = c->words[i];
        ops++;
        ends += f >> 28 == 2;
        if (f >> 28 == 0 && ((f >> 20) & 0xF) == 0) {
            uint64_t off = ((uint64_t)(f & 0xFF) << 32) | c->words[i + 1];
            if (off < g_code_end - g_main) return CHEAT_REBUILD;
        }
        if (vm_unsupported(c->words + i, c->note, sizeof c->note)) return CHEAT_UNSUPPORTED;
        i += len;
    }
    return ops == ends && ops <= 1 ? CHEAT_HEADING : CHEAT_DATA;
}

static uint64_t region(int m) {
    const RtGuestLayout *l = mem_layout();
    switch (m) {
    case 0: return g_main;
    case 1: return l->heap.base;
    case 2: return l->alias.base;
    case 3: return l->aslr.base;
    default: return 0;
    }
}

static int in_main(uint64_t a, unsigned n) { return a >= g_main && n <= g_main_size && a - g_main <= g_main_size - n; }

static int guest_ok(uint64_t a, unsigned n, uint32_t perm) {
    if (in_main(a, n)) return 1;
    RtMemoryInfo mi;
    if (mem_query(a, &mi) || !mi.state || (mi.perm & perm) != perm) return 0;
    return a + n <= mi.base + mi.size;
}

static int rd(uint64_t a, int width, uint64_t *v) {
    if (!a || !guest_ok(a, (unsigned)width, 1)) return 0;
    uint64_t x = 0;
    memcpy(&x, (const void *)(uintptr_t)a, (size_t)width);
    *v = x;
    return 1;
}

static int wr_main(uint64_t a, const uint8_t *from, int n) {
    while (n > 0) {
        uint64_t page = a & ~0xFFFull, end;
        int k = (int)(page + 0x1000 - a);
        if (k > n) k = n;
        int prot = plat_mem_state(page, page + 0x1000, &end);
        if (prot != PLAT_PROT_READ && prot != PLAT_PROT_RW) return 0;
        if (prot == PLAT_PROT_READ && !plat_mem_protect(page, 0x1000, PLAT_PROT_RW)) return 0;
        memcpy((void *)(uintptr_t)a, from, (size_t)k);
        if (prot == PLAT_PROT_READ) plat_mem_protect(page, 0x1000, PLAT_PROT_READ);
        a += (uint64_t)k;
        from += k;
        n -= k;
    }
    return 1;
}

static uint32_t g_wr[CHEAT_WR_KINDS];

static void wr(uint64_t a, int width, uint64_t v) {
    uint64_t now;
    if (!rd(a, width, &now)) {
        g_wr[CHEAT_WR_REFUSED]++;
        return;
    }
    if (!memcmp(&now, &v, (size_t)width)) {
        g_wr[CHEAT_WR_UNCHANGED]++;
        return;
    }
    int stored = 0;
    if (in_main(a, (unsigned)width)) {
        stored = wr_main(a, (const uint8_t *)&v, width);
    } else if (guest_ok(a, (unsigned)width, 2)) {
        memcpy((void *)(uintptr_t)a, &v, (size_t)width);
        stored = 1;
    }
    g_wr[stored ? CHEAT_WR_WRITTEN : CHEAT_WR_REFUSED]++;
}

static uint64_t masked(int width, uint64_t v) { return width >= 8 ? v : v & ((1ull << (8 * width)) - 1); }

static uint64_t arith(int op, uint64_t a, uint64_t b) {
    switch (op) {
    case 0: return a + b;
    case 1: return a - b;
    case 2: return a * b;
    case 3: return b < 64 ? a << b : 0;
    case 4: return b < 64 ? a >> b : 0;
    case 5: return a & b;
    case 6: return a | b;
    case 7: return ~a;
    case 8: return a ^ b;
    default: return a;
    }
}

static int compare(int cond, uint64_t a, uint64_t b) {
    switch (cond) {
    case 1: return a > b;
    case 2: return a >= b;
    case 3: return a < b;
    case 4: return a <= b;
    case 5: return a == b;
    case 6: return a != b;
    default: return 0;
    }
}

static int skip(const uint32_t *w, int n, int i, int else_ok) {
    int depth = 0;
    while (i < n) {
        int len = op_len(w + i, n - i);
        if (!len) return n;
        uint32_t f = w[i];
        int t = f >> 28;
        i += len;
        if (t == 1 || t == 8) depth++;
        else if (t == 2 && ((f >> 24) & 0xF) == 0 && depth-- == 0) return i;
        else if (t == 2 && depth == 0 && else_ok) return i;
    }
    return n;
}

static uint64_t imm64(const uint32_t *w) { return (uint64_t)w[0] << 32 | w[1]; }

static void run(const Cheat *c, uint64_t keys) {
    uint64_t r[16] = {0};
    int loop[16] = {0};
    const uint32_t *w = c->words;
    int n = c->n, budget = OPS_PER_RUN;
    for (int i = 0; i < n && budget-- > 0;) {
        int len = op_len(w + i, n - i);
        if (!len) return;
        uint32_t f = w[i];
        int t = f >> 28, width = (f >> 24) & 0xF, x = (f >> 20) & 0xF, y = (f >> 16) & 0xF, z = (f >> 12) & 0xF;
        uint64_t rel = ((uint64_t)(f & 0xFF) << 32) | (len > 1 ? w[i + 1] : 0), v;
        switch (t) {
        case 0:
            wr(region(x) + rel + r[y], width, width == 8 ? imm64(w + i + 2) : w[i + 2]);
            break;
        case 1:
            if (!rd(region(x) + rel, width, &v) || !compare(y, v, width == 8 ? imm64(w + i + 2) : w[i + 2])) {
                i = skip(w, n, i + len, 1);
                continue;
            }
            break;
        case 2:
            if (width == 1) {
                i = skip(w, n, i + len, 0);
                continue;
            }
            break;
        case 3:
            if (width == 0) {
                r[y] = w[i + 1];
                loop[y] = i + len;
            } else if (r[y] && --r[y]) {
                i = loop[y];
                continue;
            }
            break;
        case 4:
            r[y] = imm64(w + i + 1);
            break;
        case 5:
            if (!rd(z ? r[y] + rel : region(x) + rel, width, &v)) return;
            r[y] = v;
            break;
        case 6: {
            uint64_t a = r[y] + (((f >> 8) & 0xF) ? r[(f >> 4) & 0xF] : 0);
            wr(a, width, imm64(w + i + 1));
            if (z) r[y] += (uint64_t)width;
            break;
        }
        case 7:
            r[y] = masked(width, arith(z, r[y], w[i + 1]));
            break;
        case 8:
            if ((keys & (f & 0x0FFFFFFF)) != (f & 0x0FFFFFFF)) {
                i = skip(w, n, i + len, 1);
                continue;
            }
            break;
        case 9: {
            uint64_t b = (f >> 8) & 0xF ? (width == 8 ? imm64(w + i + 1) : w[i + 1]) : r[(f >> 4) & 0xF];
            r[y] = masked(width, arith(x, r[z], b));
            break;
        }
        case 0xA: {
            int o = (f >> 8) & 0xF, m = (f >> 4) & 0xF;
            uint64_t imm = ((uint64_t)(f & 0xF) << 32) | (len > 1 ? w[i + 1] : 0), a;
            switch (o) {
            case 0: a = r[y]; break;
            case 1: a = r[y] + r[m]; break;
            case 2: a = r[y] + imm; break;
            case 3: a = region(m) + r[y]; break;
            case 4: a = region(m) + imm; break;
            default: a = region(m) + r[y] + imm; break;
            }
            wr(a, width, r[x]);
            if (z) r[y] += (uint64_t)width;
            break;
        }
        default:
            return;
        }
        i += len;
    }
}

static char *read_text(const char *path) {
    uint64_t n = 0;
    char *s = (char *)plat_file_read(path, &n);
    if (s && n >= 3 && !memcmp(s, "\xEF\xBB\xBF", 3)) memmove(s, s + 3, (size_t)n - 2);
    return s;
}

static void add_word(Cheat *c, uint32_t v, int *cap) {
    if (c->n == *cap) {
        *cap = *cap ? *cap * 2 : 16;
        c->words = (uint32_t *)realloc(c->words, sizeof *c->words * (size_t)*cap);
        if (!c->words) rt_fatal("cheats: out of memory");
    }
    c->words[c->n++] = v;
}

static void parse(char *s, const char *mod) {
    int first = g_n, wcap = 0;
    for (char *line = s; line && *line;) {
        char *end = strchr(line, '\n'), *next = end ? end + 1 : NULL;
        if (end) *end = 0;
        while (*line == ' ' || *line == '\t') line++;
        size_t len = strlen(line);
        while (len && (line[len - 1] == '\r' || line[len - 1] == ' ' || line[len - 1] == '\t')) line[--len] = 0;
        if (len >= 2 && (line[0] == '[' || line[0] == '{') && (line[len - 1] == ']' || line[len - 1] == '}')) {
            if (g_n == g_cap) {
                g_cap = g_cap ? g_cap * 2 : 32;
                g_cheats = (Cheat *)realloc(g_cheats, sizeof *g_cheats * (size_t)g_cap);
                if (!g_cheats) rt_fatal("cheats: out of memory");
            }
            Cheat *c = &g_cheats[g_n++];
            memset(c, 0, sizeof *c);
            line[len - 1] = 0;
            char *name = line + 1;
            while (*name == ' ' || *name == '\t') name++;
            size_t k = strlen(name);
            while (k && (name[k - 1] == ' ' || name[k - 1] == '\t')) name[--k] = 0;
            c->info.name = strdup(name);
            c->info.mod = mod;
            c->info.kind = line[0] == '{' ? CHEAT_MASTER : CHEAT_HEADING;
            c->set = -1;
            c->dup = 1;
            for (int j = first; j < g_n - 1; j++) c->dup += !strcmp(g_cheats[j].info.name, c->info.name);
            wcap = 0;
        } else if (g_n > first) {
            for (char *tok = strtok(line, " \t"); tok; tok = strtok(NULL, " \t")) {
                char *e;
                unsigned long v = strtoul(tok, &e, 16);
                if (e - tok == 8 && !*e) add_word(&g_cheats[g_n - 1], (uint32_t)v, &wcap);
            }
        }
        line = next;
    }
}

typedef struct {
    const char *dir;
    char *out;
    size_t cap;
    int found;
} FindFile;
static int find_cb(const char *name, int kind, uint64_t size, void *ctx) {
    (void)size;
    FindFile *f = (FindFile *)ctx;
    if (kind == PLAT_FS_FILE && !strcasecmp(name, g_file) &&
        snprintf(f->out, f->cap, "%s/%s", f->dir, name) < (int)f->cap)
        f->found = 1;
    return !f->found;
}
static int find_file(const char *folder, char *out, size_t cap) {
    FindFile f = {folder, out, cap, 0};
    plat_fs_list(folder, find_cb, &f);
    return f.found;
}

static int status_of(char *status, const char *key, char **fields) {
    for (char *line = status; line && *line;) {
        char *end = strchr(line, '\n'), *next = end ? end + 1 : NULL;
        if (!strncmp(line, key, 8) && line[8] == '\t') {
            static char copy[512];
            size_t n = end ? (size_t)(end - line) : strlen(line);
            if (n >= sizeof copy) n = sizeof copy - 1;
            memcpy(copy, line, n);
            copy[n] = 0;
            if (n && copy[n - 1] == '\r') copy[n - 1] = 0;
            int k = 0;
            fields[k++] = copy;
            for (char *p = copy; *p && k < 5; p++)
                if (*p == '\t') *p = 0, fields[k++] = p + 1;
            while (k < 5) fields[k++] = (char *)"";
            return 1;
        }
        line = next;
    }
    return 0;
}

static void plain_name(char *s) {
    for (char *p; (p = strstr(s, "\xE2\x84\x96")) != NULL;) {
        p[0] = 'N', p[1] = 'o';
        memmove(p + 2, p + 3, strlen(p + 3) + 1);
    }
}

static int g_unbuilt_code, g_unbuilt_patches;
static unsigned g_cfg_seen;

static void report_unbuilt(void) {
    uint32_t key = 0;
    g_unbuilt_code = 0;
    for (int i = 0; i < g_n; i++) g_unbuilt_code += g_cheats[i].info.kind == CHEAT_REBUILD;
    g_unbuilt_patches = mods_exefs_pending(&key);
    if (g_unbuilt_code || g_unbuilt_patches)
        rt_log("cheats: %d code cheat%s and %d ExeFS patch file%s are not built into the game; Play from the launcher "
               "builds them in", g_unbuilt_code, cheat_s(g_unbuilt_code), g_unbuilt_patches, cheat_s(g_unbuilt_patches));
}

void cheats_init(void) {
    const RtLoaded *m = ldr_count() ? ldr_module(0) : NULL;
    char dir[PLAT_PATH_MAX], bid[17], path[2400];
    if (!m || !exe_dir(dir, sizeof dir) || !build_id(bid)) return;
    g_main = m->base;
    g_main_size = m->size;
    g_code_end = m->base + ((m->text_end + 0xFFF) & ~0xFFFull);
    snprintf(g_file, sizeof g_file, "%s.txt", bid);
    if (cfg_is_set("NX2SARIA_MODS_REBUILD"))
        rt_log("cheats: mods_rebuild = %s does nothing now: Play from the launcher builds code cheats and ExeFS patches "
               "in", cfg_value("NX2SARIA_MODS_REBUILD"));
    mods_scan();
    for (int k = 0; k < mods_count(); k++) {
        const ModInfo *mod = mods_at(k);
        if (!mod->enabled || mod->problem != MOD_OK || !mod->cheats[0] || !find_file(mod->cheats, path, sizeof path))
            continue;
        char *text = read_text(path);
        if (!text) {
            rt_log("cheats: %s: cannot read %s", mod->name, path);
            continue;
        }
        parse(text, strdup(mod->name));
        free(text);
        g_files++;
    }
    if (!g_files) {
        rt_log("cheats: no enabled mod holds cheats/%s; the menu has no Cheats tab", g_file);
        report_unbuilt();
        return;
    }
    snprintf(path, sizeof path, "%sbuild-cache" SEP "cheats.status", dir);
    char *status = read_text(path);
    int counts[CHEAT_UNSUPPORTED + 1] = {0}, seen_dups = 0;
    for (int i = 0; i < g_n; i++) {
        Cheat *c = &g_cheats[i];
        char named[900];
        snprintf(named, sizeof named, c->dup > 1 ? "%s/%s#%d" : "%s/%s", c->info.mod, c->info.name, c->dup);
        seen_dups += c->dup > 1;
        char key[9], content[9];
        snprintf(key, sizeof key, "%08X", fnv(named, strlen(named)));
        size_t tn = (size_t)c->n * 9 + 1, tl = 0;
        char *t = (char *)malloc(tn);
        if (!t) rt_fatal("cheats: out of memory");
        t[0] = 0;
        for (int k = 0; k < c->n; k++) tl += (size_t)snprintf(t + tl, tn - tl, k ? " %08X" : "%08X", c->words[k]);
        c->content = fnv(t, tl);
        snprintf(content, sizeof content, "%08X", c->content);
        free(t);
        snprintf(c->info.key, sizeof c->info.key, "NX2SARIA_CHEAT_%s", key);
        int master = c->info.kind == CHEAT_MASTER;
        char *f[5];
        CheatKind kind;
        if (status && status_of(status, key, f) && !strcmp(f[1], content)) {
            const char *k = f[2];
            kind = !strcmp(k, "code") ? CHEAT_CODE : !strcmp(k, "key") ? CHEAT_KEY : !strcmp(k, "master") ? CHEAT_MASTER
                 : !strcmp(k, "restore") ? CHEAT_RESTORE : !strcmp(k, "heading") ? CHEAT_HEADING
                 : !strcmp(k, "data") ? CHEAT_DATA : CHEAT_UNSUPPORTED;
            c->info.buttons = (uint32_t)strtoul(f[3], NULL, 16);
            snprintf(c->note, sizeof c->note, "%s", f[4]);
            if (kind == CHEAT_CODE || kind == CHEAT_KEY || kind == CHEAT_MASTER) {
                char set[32];
                snprintf(set, sizeof set, "cheat_%08x", fnv(named, strlen(named)));
                c->set = rt_patch_set(set);
                if (c->set < 0 && kind != CHEAT_MASTER) kind = CHEAT_REBUILD;
            }
        } else {
            kind = classify(c);
        }
        if (master && kind == CHEAT_DATA) kind = CHEAT_MASTER;
        c->info.kind = kind;
        c->info.note = c->note;
        const char *v = cfg_get(c->info.key);
        c->on = kind == CHEAT_MASTER || (v && *v && strcmp(v, "0") != 0);
        if (kind == CHEAT_KEY && c->set >= 0) rt_patch_switch(c->set, 0);
        if (kind == CHEAT_KEY || kind == CHEAT_DATA || (kind == CHEAT_MASTER && c->set < 0)) g_vm = 1;
        counts[kind]++;
    }
    free(status);
    report_unbuilt();
    for (int i = 0; i < g_n; i++) plain_name((char *)g_cheats[i].info.name);
    int on = 0;
    for (int i = 0; i < g_n; i++) on += g_cheats[i].on && g_cheats[i].info.kind != CHEAT_HEADING;
    rt_log("cheats: %s in %d mod%s: %d cheat%s, %d on; %d code, %d while buttons are held, %d data, %d master, "
           "%d restore, %d heading%s, %d wait%s for a rebuild, %d not supported%s; %s",
           g_file, g_files, cheat_s(g_files), g_n, cheat_s(g_n), on, counts[CHEAT_CODE], counts[CHEAT_KEY],
           counts[CHEAT_DATA], counts[CHEAT_MASTER], counts[CHEAT_RESTORE], counts[CHEAT_HEADING],
           cheat_s(counts[CHEAT_HEADING]), counts[CHEAT_REBUILD], counts[CHEAT_REBUILD] == 1 ? "s" : "",
           counts[CHEAT_UNSUPPORTED],
           seen_dups ? " (repeated names numbered)" : "",
           g_n ? "the menu's Cheats tab lists them" : "the menu has no Cheats tab");
    g_cfg_seen = cfg_version();
}

void cheats_unbuilt(int *code, int *patches) {
    *code = g_unbuilt_code;
    *patches = g_unbuilt_patches;
}

int cheats_count(void) { return g_n; }

const CheatInfo *cheats_info(int i) { return i >= 0 && i < g_n ? &g_cheats[i].info : NULL; }

int cheats_on(int i) { return i >= 0 && i < g_n && g_cheats[i].on; }

const char *cheats_file(void) { return g_file; }

static int switchable(const Cheat *c) {
    return c->info.kind == CHEAT_CODE || c->info.kind == CHEAT_KEY || c->info.kind == CHEAT_DATA;
}

static void follow_settings(void) {
    unsigned ver = cfg_version();
    if (ver == g_cfg_seen) return;
    g_cfg_seen = ver;
    for (int i = 0; i < g_n; i++) {
        Cheat *c = &g_cheats[i];
        if (!switchable(c)) continue;
        const char *v = cfg_get(c->info.key);
        int on = v && *v && strcmp(v, "0") != 0;
        if (on == (c->on != 0)) continue;
        plat_xchg32(&c->on, on);
        if (c->info.kind == CHEAT_CODE) rt_patch_switch(c->set, on);
        rt_log("cheats: %s/%s %s", c->info.mod, c->info.name, on ? "on" : "off");
    }
}

static void say_stats(const Cheat *c, const char *when) {
    char line[1100];
    rt_log("cheats: %s", cheat_stats_line(&c->stats, c->info.mod, c->info.name, when, line, sizeof line));
}

static void run_counted(Cheat *c, uint64_t keys, uint64_t now_ns) {
    memset(g_wr, 0, sizeof g_wr);
    run(c, keys);
    cheat_stats_frame(&c->stats, g_wr);
    if (cheat_stats_due(&c->stats, now_ns)) say_stats(c, "so far");
}

void cheats_exit_log(void) {
    for (int i = 0; i < g_n; i++) {
        const Cheat *c = &g_cheats[i];
        int vm = c->info.kind == CHEAT_DATA || (c->info.kind == CHEAT_MASTER && c->set < 0);
        if (vm && (c->on || cheat_stats_get(&c->stats.frames))) say_stats(c, "this session");
    }
}

void cheats_frame(uint64_t frame) {
    follow_settings();
    if (!g_vm || frame < 60) return;
    uint64_t keys = 0;
    int32_t lx, ly, rx, ry;
    input_poll(&keys, &lx, &ly, &rx, &ry);
    uint64_t now_ns = plat_now_ns();
    for (int i = 0; i < g_n; i++) {
        Cheat *c = &g_cheats[i];
        switch (c->info.kind) {
        case CHEAT_KEY:
            if (c->set >= 0) {
                int want = c->on && (keys & c->info.buttons) == c->info.buttons;
                if (rt_patch_enabled(c->set) != want) rt_patch_switch(c->set, want);
            }
            break;
        case CHEAT_DATA:
            if (c->on) run_counted(c, keys, now_ns);
            break;
        case CHEAT_MASTER:
            if (c->set < 0) run_counted(c, keys, now_ns);
            break;
        default:
            break;
        }
    }
}

const char *cheats_buttons(uint32_t mask, char *buf, int n) {
    static const char *const names[] = {"A", "B", "X", "Y", "Left stick press", "Right stick press", "L", "R", "ZL",
                                        "ZR", "+", "-", "D-pad left", "D-pad up", "D-pad right", "D-pad down",
                                        "Left stick left", "Left stick up", "Left stick right", "Left stick down",
                                        "Right stick left", "Right stick up", "Right stick right", "Right stick down"};
    int k = 0;
    buf[0] = 0;
    for (int b = 0; b < 24; b++)
        if (mask & (1u << b)) k += snprintf(buf + k, (size_t)(n > k ? n - k : 0), "%s%s", k ? " + " : "", names[b]);
    return buf;
}
