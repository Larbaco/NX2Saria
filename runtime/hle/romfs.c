// Contains code Copyright (c) 2026 MacNativeGames contributors (MIT; see THIRD_PARTY_NOTICES.md).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "action_prompt.h"
#include "wide_hud.h"
#include "cfg.h"
#include "dispatch.h"
#include "fs.h"
#include "host.h"
#include "input_map.h"
#include "mods.h"
#include "romfs_config.h"
#include "romfs_path.h"
#include "platform/platform.h"
#include "restbl.h"
#include "sha256.h"
#include "stutter_map.h"

#include <zstd.h>

#define NH 65536
#define READ_CHUNK (64u << 20)
#define HASH_CHUNK (8u << 20)
#define ADDED UINT64_MAX

typedef struct { uint64_t offset, size; uint32_t name, len; } Entry;
_Static_assert(sizeof(Entry) == 24, "index entry layout");

typedef struct { uint32_t volume, pad; uint64_t file_id, size, write_time, create_time; } Identity;
typedef struct { uint64_t index, size; char *path, *name; Identity id; int prio, mod; } Overlay;

static uint8_t *index_bytes;
static size_t index_size;
static uint64_t file_count, strings_offset;
static PlatFile *image;
static const uint8_t *image_map;
static char image_file[2048];
static char index_dir[2048];
#define NROM_NAMES 4
static char mount_names[NROM_NAMES][32];

static const char *rom_relative(const char *path) {
    static __thread char folded[2048];
    if (path && strstr(path, "//") && romfs_path_collapse(path, folded, sizeof folded)) path = folded;
    for (int i = 0; i < NROM_NAMES && mount_names[i][0]; i++) {
        const char *p = romfs_path_relative(mount_names[i], path);
        if (p) return p;
    }
    return NULL;
}
static PlatMutex lock = PLAT_MUTEX_INIT;
static uint64_t handles[NH];
static uint32_t handle_ovl[NH];
static PlatFile *handle_file[NH];
static Overlay *overlays;
static uint32_t *open_ms;
static uint16_t *open_n;
static size_t noverlay, overlay_cap;
static char **added;
static size_t nadded;
static int patches_loaded;

static void need(int c, const char *m) {
    if (!c) rt_fatal("RomFS: %s", m);
}

static Entry entry(uint64_t i) {
    Entry e;
    memcpy(&e, index_bytes + 24 + i * 24, 24);
    return e;
}
static const char *entry_name(Entry e) { return (const char *)index_bytes + strings_offset + e.name; }

static double now_s(void) { return (double)plat_now_ns() / 1e9; }

static int64_t read_at(PlatFile *f, void *buf, uint32_t n, uint64_t off) {
    mem_thaw((uintptr_t)buf, n);
    int64_t got = plat_file_pread(f, buf, n, off);
    if (got == -2)
        rt_log("RomFS: read of %u bytes into %p refused: the buffer is not writable", n, buf);
    return got < 0 ? -1 : got;
}

static PlatFile *open_read(const char *path) { return plat_file_open(path); }

static Identity identity_of(PlatFile *f) {
    PlatFileId i;
    need(plat_file_id(f, &i), "file information");
    return (Identity){(uint32_t)i.volume, 0, i.file_id, i.size, i.write_time, i.create_time};
}

static void hex32(const uint8_t d[32], char out[65]) {
    for (int i = 0; i < 32; i++) snprintf(out + 2 * i, 3, "%02x", d[i]);
}

static void sha_range(PlatFile *f, uint64_t offset, uint64_t size, char hex[65], int progress) {
    Sha256 h;
    sha256_init(&h);
    uint8_t *buf = malloc(HASH_CHUNK);
    need(buf != NULL, "hash buffer");
    double t0 = now_s();
    for (uint64_t done = 0; done < size;) {
        uint32_t want = size - done > HASH_CHUNK ? HASH_CHUNK : (uint32_t)(size - done);
        int64_t n = read_at(f, buf, want, offset + done);
        need(n > 0, "read of hash input");
        sha256_update(&h, buf, (size_t)n);
        done += (uint64_t)n;
        if (progress && (done % (2ull << 30) < (uint64_t)n || done == size))
            rt_log("RomFS: hashed %.1f of %.1f GiB (%.0f MiB/s)", done / 1073741824.0, size / 1073741824.0,
                   done / 1048576.0 / (now_s() - t0 + 1e-9));
    }
    uint8_t d[32];
    sha256_final(&h, d);
    free(buf);
    hex32(d, hex);
}

typedef struct { char magic[16], sha256[65]; uint8_t pad[7]; Identity id; } RomfsProof;

static void verify_image(const char *dir) {
    const char *mode = cfg_get("NX2SARIA_ROMFS_VERIFY");
    if (!mode || (strcmp(mode, "full") && strcmp(mode, "force"))) return;
    const char *pdir = cfg_get("NX2SARIA_ROMFS_PROOF_DIR");
    if (pdir && *pdir) dir = pdir;
    RomfsProof want = {0}, saved;
    memcpy(want.magic, "NX2NAVIRFS-PROOF-1", 16);
    memcpy(want.sha256, ROMFS_IMAGE_SHA256, sizeof ROMFS_IMAGE_SHA256 < 65 ? sizeof ROMFS_IMAGE_SHA256 : 65);
    want.id = identity_of(image);
    char path[2100];
    need(snprintf(path, sizeof path, "%s/.romfs-verified-v1", dir) < (int)sizeof path, "proof path");
    if (strcmp(mode, "force")) {
        PlatFile *p = open_read(path);
        if (p) {
            int ok = plat_file_pread(p, &saved, sizeof saved, 0) == sizeof saved;
            plat_file_close(p);
            if (ok && !memcmp(&saved, &want, sizeof want)) {
                rt_log("RomFS unchanged since its full SHA-256 check (%s)", path);
                return;
            }
        }
    }
    rt_log("RomFS: verifying the SHA-256 of %s (%llu bytes)", image_file, (unsigned long long)want.id.size);
    char hex[65];
    sha_range(image, 0, want.id.size, hex, 1);
    if (strcmp(hex, ROMFS_IMAGE_SHA256))
        rt_fatal("RomFS image %s has SHA-256 %s, the extraction recorded %s (extract it again)", image_file, hex,
                 ROMFS_IMAGE_SHA256);
    Identity after = identity_of(image);
    need(!memcmp(&after, &want.id, sizeof after), "image changed during verification");
    plat_file_write_atomic(path, &want, sizeof want);
    rt_log("RomFS: image SHA-256 ok");
}

static uint64_t lower(const char *name) {
    uint64_t lo = 0, hi = file_count;
    while (lo < hi) {
        uint64_t m = lo + (hi - lo) / 2;
        if (strcmp(entry_name(entry(m)), name) < 0) lo = m + 1;
        else hi = m;
    }
    return lo;
}

static size_t added_lower(const char *p) {
    size_t lo = 0, hi = nadded;
    while (lo < hi) {
        size_t m = lo + (hi - lo) / 2;
        if (strcmp(added[m], p) < 0) lo = m + 1;
        else hi = m;
    }
    return lo;
}

static uint64_t file_index(const char *p) {
    uint64_t i = lower(p);
    if (i < file_count && !strcmp(p, entry_name(entry(i)))) return i;
    size_t k = added_lower(p);
    return k < nadded && !strcmp(added[k], p) ? file_count + k : UINT64_MAX;
}

static int folder_exists(const char *folder) {
    size_t n = strlen(folder);
    uint64_t i = lower(folder);
    if (i < file_count && !strncmp(entry_name(entry(i)), folder, n)) return 1;
    size_t k = added_lower(folder);
    return k < nadded && !strncmp(added[k], folder, n);
}

static uint32_t overlay_slot(uint64_t i) {
    size_t lo = 0, hi = noverlay;
    while (lo < hi) {
        size_t m = lo + (hi - lo) / 2;
        if (overlays[m].index < i) lo = m + 1;
        else hi = m;
    }
    return lo < noverlay && overlays[lo].index == i ? (uint32_t)(lo + 1) : 0;
}

static int overlay_load(const char *dir, int optional, int prio) {
#define BAD(...) do { if (!optional) rt_fatal(__VA_ARGS__); rt_log(__VA_ARGS__); goto bad; } while (0)
    char path[2100];
    need(snprintf(path, sizeof path, "%s/manifest.tsv", dir) < (int)sizeof path, "overlay path");
    FILE *m = fopen(path, "rb");
    if (!m) {
        rt_log("RomFS overlay: none (%s)", path);
        return 1;
    }
    char line[8192];
    size_t first = noverlay;
    uint64_t hashed = 0;
    double t0 = now_s();
    while (fgets(line, sizeof line, m)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        char name[4096], sha[65], orig[65], hex[65];
        unsigned long long size = 0;
        need(sscanf(line, "%4095[^\t]\t%llu\t%64[0-9a-f]\t%64[0-9a-f-]", name, &size, sha, orig) == 4 &&
             strlen(sha) == 64 && (strlen(orig) == 64 || !strcmp(orig, "-")), "overlay manifest line");
        need(romfs_path_valid_relative(name), "overlay path");
        uint64_t i = lower(name);
        int known = i < file_count && !strcmp(entry_name(entry(i)), name), add = !strcmp(orig, "-");
        if (!known && !add)
            BAD("RomFS overlay: %s/manifest.tsv names %s, which the RomFS lacks; a new file has \"-\" as its "
                "original's SHA-256", dir, name);
        if (known && add)
            BAD("RomFS overlay: %s/manifest.tsv adds %s, which the RomFS already has; give its original's SHA-256",
                dir, name);
        Entry e = {0};
        if (known) {
            e = entry(i);
            sha_range(image, e.offset, e.size, hex, 0);
            if (strcmp(hex, orig)) BAD("RomFS overlay: the original of %s has SHA-256 %s, %s/manifest.tsv says %s",
                                       name, hex, dir, orig);
        } else {
            i = ADDED;
        }
        char file[8200];
        need(snprintf(file, sizeof file, "%s/%s", dir, name) < (int)sizeof file, "overlay file path");
        PlatFile *f = open_read(file);
        if (!f) BAD("RomFS overlay: %s missing", file);
        Identity id = identity_of(f);
        if (id.size != size) {
            plat_file_close(f);
            BAD("RomFS overlay: %s has %llu bytes, its manifest says %llu", file, (unsigned long long)id.size, size);
        }
        sha_range(f, 0, size, hex, 0);
        plat_file_close(f);
        if (strcmp(hex, sha)) BAD("RomFS overlay: %s has SHA-256 %s, its manifest says %s", file, hex, sha);
        hashed += e.size + size;
        if (noverlay == overlay_cap) {
            overlay_cap = overlay_cap ? overlay_cap * 2 : 16;
            overlays = realloc(overlays, overlay_cap * sizeof *overlays);
            need(overlays != NULL, "overlay table");
        }
        char *copy = strdup(file), *rel = strdup(name);
        need(copy && rel, "overlay path copy");
        overlays[noverlay++] = (Overlay){i, size, copy, rel, id, prio, 0};
        if (!prio) patches_loaded = 1;
        if (noverlay - first <= 16)
            rt_log("RomFS overlay: %s (%llu bytes) %s", name, size, add ? "is added" : "replaces the original");
    }
    fclose(m);
    if (noverlay - first > 16) rt_log("RomFS overlay: %zu files from %s (%zu not listed)", noverlay - first, dir,
                                      noverlay - first - 16);
    if (noverlay > first) rt_log("RomFS overlay: %s checked, %zu files, %.1f MiB hashed in %.2f s", dir,
                                 noverlay - first, hashed / 1048576.0, now_s() - t0);
    return 1;
bad:
    fclose(m);
    for (size_t k = first; k < noverlay; k++) {
        free((void *)overlays[k].path);
        free((void *)overlays[k].name);
    }
    noverlay = first;
    return 0;
#undef BAD
}

static uint32_t *folded;

static int folded_order(const void *a, const void *b) {
    return strcasecmp(entry_name(entry(*(const uint32_t *)a)), entry_name(entry(*(const uint32_t *)b)));
}

static uint64_t index_any_case(const char *name) {
    uint64_t i = lower(name);
    if (i < file_count && !strcmp(entry_name(entry(i)), name)) return i;
    if (!folded) {
        folded = malloc(file_count * sizeof *folded);
        need(folded != NULL, "case-folded index");
        for (uint64_t k = 0; k < file_count; k++) folded[k] = (uint32_t)k;
        qsort(folded, file_count, sizeof *folded, folded_order);
    }
    uint64_t lo = 0, hi = file_count;
    while (lo < hi) {
        uint64_t m = lo + (hi - lo) / 2;
        if (strcasecmp(entry_name(entry(folded[m])), name) < 0) lo = m + 1;
        else hi = m;
    }
    return lo < file_count && !strcasecmp(entry_name(entry(folded[lo])), name) ? folded[lo] : UINT64_MAX;
}

static void overlay_push(Overlay o) {
    if (noverlay == overlay_cap) {
        overlay_cap = overlay_cap ? overlay_cap * 2 : 16;
        overlays = realloc(overlays, overlay_cap * sizeof *overlays);
        need(overlays != NULL, "overlay table");
    }
    overlays[noverlay++] = o;
}

static int other_version(const char *path) {
    for (const char *p = strstr(path, ".Product."); p; p = strstr(p + 1, ".Product.")) {
        const char *d = p + strlen(".Product.");
        if (*d >= '0' && *d <= '9') return 1;
    }
    return 0;
}

static char restbl_rel[256];
typedef struct {
    char *file;
    int mod, other;
} Shipped;
static Shipped *shipped;
static size_t nshipped;

static int ends_with(const char *s, const char *tail) {
    size_t n = strlen(s), k = strlen(tail);
    return n >= k && !strcmp(s + n - k, tail);
}

static int is_restbl(const char *rel) {
    return !strncmp(rel, "System/Resource/ResourceSizeTable.", 34) && ends_with(rel, ".rsizetable.zs");
}

static void shipped_add(const char *file, int mod, int other) {
    Shipped *grown = realloc(shipped, (nshipped + 1) * sizeof *shipped);
    char *copy = strdup(file);
    need(grown && copy, "resource size table list");
    shipped = grown;
    shipped[nshipped++] = (Shipped){copy, mod, other};
}

static void restbl_find(void) {
    for (uint64_t i = lower("System/Resource/ResourceSizeTable."); i < file_count; i++) {
        const char *n = entry_name(entry(i));
        if (strncmp(n, "System/Resource/ResourceSizeTable.", 34)) break;
        if (is_restbl(n) && strlen(n) < sizeof restbl_rel) {
            snprintf(restbl_rel, sizeof restbl_rel, "%s", n);
            return;
        }
    }
}

static void restbl_collect(void) {
    for (size_t k = 0; k < noverlay; k++)
        if (restbl_rel[0] && !strcmp(overlays[k].name, restbl_rel)) shipped_add(overlays[k].path, overlays[k].mod, 0);
}

typedef struct { int k, prio, depth; const char *dir, *rel; } ModWalk;

#define MOD_WALK_DEPTH 32

static void mod_walk(int k, int prio, int depth, const char *dir, const char *rel);

static int mod_walk_cb(const char *leaf, int kind, uint64_t size, void *ctx) {
    (void)size;
    const ModWalk *w = ctx;
    ModInfo *m = mods_at(w->k);
    char file[4200], path[4200];
    if (snprintf(file, sizeof file, "%s/%s", w->dir, leaf) >= (int)sizeof file ||
        snprintf(path, sizeof path, "%s%s%s", w->rel, *w->rel ? "/" : "", leaf) >= (int)sizeof path) {
        rt_log("mods: %s: path too long under %s; left out", m->name, w->dir);
        return 1;
    }
    if (kind == PLAT_FS_DIR) {
        if (w->depth >= MOD_WALK_DEPTH) rt_log("mods: %s: %s nests too deep; left out", m->name, file);
        else mod_walk(w->k, w->prio, w->depth + 1, file, path);
        return 1;
    }
    if (kind != PLAT_FS_FILE) return 1;
    if (!romfs_path_valid_relative(path)) {
        rt_log("mods: %s: %s is not a RomFS path; left out", m->name, path);
        return 1;
    }
    uint64_t i = index_any_case(path);
    if (i == UINT64_MAX && other_version(path)) {
        if (is_restbl(path)) shipped_add(file, w->k + 1, 1);
        m->other_version++;
        return 1;
    }
    PlatFile *f = open_read(file);
    if (!f) {
        rt_log("mods: %s: cannot open %s; left out", m->name, file);
        return 1;
    }
    Identity id = identity_of(f);
    plat_file_close(f);
    char *copy = strdup(file), *rp = strdup(i == UINT64_MAX ? path : entry_name(entry(i)));
    need(copy && rp, "overlay path copy");
    overlay_push((Overlay){i == UINT64_MAX ? ADDED : i, id.size, copy, rp, id, w->prio, w->k + 1});
    return 1;
}

static void mod_walk(int k, int prio, int depth, const char *dir, const char *rel) {
    ModWalk w = {k, prio, depth, dir, rel};
    if (plat_fs_list(dir, mod_walk_cb, &w) != PLAT_FS_OK)
        rt_log("mods: %s: cannot list %s (error %d); its files are left out", mods_at(k)->name, dir,
               plat_fs_os_error());
}

static int contest_order(const void *a, const void *b) {
    const Overlay *x = a, *y = b;
    if (x->index != y->index) return (x->index > y->index) - (x->index < y->index);
    int c = x->index == ADDED ? strcmp(x->name, y->name) : 0;
    return c ? c : y->prio - x->prio;
}

static const char *owner_name(const Overlay *o) {
    return o->mod ? mods_at(o->mod - 1)->name : o->prio ? "romfs_mods" : "the port's patches";
}

static void overlay_contest(void) {
    if (!noverlay) return;
    qsort(overlays, noverlay, sizeof *overlays, contest_order);
    size_t kept = 0;
    for (size_t k = 0; k < noverlay; k++) {
        Overlay *o = &overlays[k], *w = kept ? &overlays[kept - 1] : NULL;
        if (w && w->index == o->index && (o->index != ADDED || !strcmp(w->name, o->name))) {
            if (o->mod) {
                ModInfo *m = mods_at(o->mod - 1);
                m->overridden++;
                if (!m->overridden_by[0]) snprintf(m->overridden_by, sizeof m->overridden_by, "%s", owner_name(w));
            } else if (w->mod) {
                rt_log("RomFS overlay: %s from mod %s replaces the one of %s", o->name, owner_name(w), owner_name(o));
            }
            free(o->path);
            free(o->name);
            continue;
        }
        if (o->mod) {
            if (o->index == ADDED) mods_at(o->mod - 1)->added++;
            else mods_at(o->mod - 1)->replaced++;
        }
        overlays[kept++] = *o;
    }
    noverlay = kept;
}

static void mods_overlay_init(void) {
    mods_scan();
    int n = mods_count();
    for (int k = 0; k < n; k++) {
        const ModInfo *m = mods_at(k);
        if (m->enabled && (m->kind & MOD_ROMFS) && m->problem == MOD_OK) mod_walk(k, 2 + (n - k), 0, m->romfs, "");
    }
}

static void mods_overlay_log(void) {
    for (int k = 0; k < mods_count(); k++) {
        const ModInfo *m = mods_at(k);
        if (!m->enabled || !(m->kind & MOD_ROMFS) || m->problem != MOD_OK) continue;
        rt_log("mods: %s: %u files served (%u replaced, %u added), %u overridden%s%s, %u for another game version",
               m->name, m->replaced + m->added, m->replaced, m->added, m->overridden, m->overridden ? " by " : "",
               m->overridden_by, m->other_version);
    }
}

static int overlay_order(const void *a, const void *b) {
    uint64_t x = ((const Overlay *)a)->index, y = ((const Overlay *)b)->index;
    return (x > y) - (x < y);
}

static int added_order(const void *a, const void *b) {
    return strcmp((*(Overlay *const *)a)->name, (*(Overlay *const *)b)->name);
}

static void added_init(void) {
    Overlay **list = NULL;
    for (size_t k = 0; k < noverlay; k++)
        if (overlays[k].index == ADDED) {
            list = realloc(list, (nadded + 1) * sizeof *list);
            need(list != NULL, "added files");
            list[nadded++] = &overlays[k];
        }
    if (!nadded) return;
    qsort(list, nadded, sizeof *list, added_order);
    added = malloc(nadded * sizeof *added);
    need(added != NULL, "added files");
    for (size_t k = 0; k < nadded; k++) {
        list[k]->index = file_count + k;
        added[k] = list[k]->name;
    }
    free(list);
    rt_log("RomFS overlay: %zu files added to the RomFS", nadded);
}

static void *read_served(const char *rel, Overlay **base, size_t *n, const char *keeps) {
    uint64_t i = lower(rel);
    *base = NULL;
    if (i >= file_count || strcmp(entry_name(entry(i)), rel)) {
        rt_log("RomFS: %s missing; %s", rel, keeps);
        return NULL;
    }
    for (size_t k = 0; k < noverlay; k++)
        if (overlays[k].index == i) *base = &overlays[k];
    PlatFile *f = *base ? open_read((*base)->path) : image;
    uint64_t off = *base ? 0 : entry(i).offset, size = *base ? (*base)->size : entry(i).size;
    void *in = size && size < (16u << 20) && f ? malloc((size_t)size) : NULL;
    int ok = in && read_at(f, in, (uint32_t)size, off) == (int64_t)size;
    if (*base && f) plat_file_close(f);
    if (!ok) {
        free(in);
        rt_log("RomFS: cannot read %s; %s", rel, keeps);
        return NULL;
    }
    *n = (size_t)size;
    return in;
}

static Overlay *served_overlay(const char *rel) {
    uint64_t i = lower(rel);
    Overlay *o = NULL;
    for (size_t k = 0; k < noverlay; k++)
        if (overlays[k].index == i) o = &overlays[k];
    return o;
}

static int serve_generated(const char *rel, Overlay *base, void *out, size_t out_n, const char *sub, const char *what,
                           const char *keeps) {
    char dir[2100], file[2200];
    const char *leaf = strrchr(rel, '/') ? strrchr(rel, '/') + 1 : rel;
    need(snprintf(dir, sizeof dir, "%s/%s", rt_config()->profile, sub) < (int)sizeof dir, "generated overlay path");
    need(snprintf(file, sizeof file, "%s/%s", dir, leaf) < (int)sizeof file, "generated overlay path");
    plat_file_mkdir_p(dir);
    int ok = plat_file_write(file, out, out_n);
    free(out);
    PlatFile *r = ok ? open_read(file) : NULL;
    if (!r) {
        rt_log("RomFS: cannot write %s; %s", file, keeps);
        return 0;
    }
    Identity id = identity_of(r);
    plat_file_close(r);
    size_t pl = strlen(rt_config()->profile);
    int generated = base && !strncmp(base->path, rt_config()->profile, pl) &&
                    (base->path[pl] == '/' || base->path[pl] == '\\');
    char *copy = strdup(file);
    need(copy != NULL, "overlay path copy");
    if (base) {
        free(base->path);
        base->path = copy;
        base->size = out_n;
        base->id = id;
    } else {
        if (noverlay == overlay_cap) {
            overlay_cap = overlay_cap ? overlay_cap * 2 : 16;
            overlays = realloc(overlays, overlay_cap * sizeof *overlays);
            need(overlays != NULL, "overlay table");
        }
        char *name = strdup(rel);
        need(name != NULL, "overlay path copy");
        overlays[noverlay++] = (Overlay){lower(rel), out_n, copy, name, id};
    }
    rt_log("RomFS overlay: %s (%zu bytes, %s) replaces %s", file, out_n, what,
           !base ? "the original" : generated ? "the file another change made" : "the mod's file");
    return 1;
}

static uint64_t content_size(PlatFile *f, uint64_t off, uint64_t size, const char *rel) {
    if (!ends_with(rel, ".zs") || !f) return size;
    uint8_t head[18];
    uint32_t n = size < sizeof head ? (uint32_t)size : (uint32_t)sizeof head;
    if (read_at(f, head, n, off) != n) return size;
    unsigned long long c = ZSTD_getFrameContentSize(head, n);
    return c == ZSTD_CONTENTSIZE_ERROR ? size : c == ZSTD_CONTENTSIZE_UNKNOWN ? size * 4 : c;
}

static Restbl *restbl_unpack(const void *z, size_t n) {
    unsigned long long raw = ZSTD_getFrameContentSize(z, n);
    if (raw == ZSTD_CONTENTSIZE_UNKNOWN || raw == ZSTD_CONTENTSIZE_ERROR || raw > (64u << 20)) return NULL;
    uint8_t *d = malloc((size_t)raw ? (size_t)raw : 1);
    size_t got = d ? ZSTD_decompress(d, (size_t)raw, z, n) : 0;
    Restbl *t = d && !ZSTD_isError(got) && got == raw ? restbl_parse(d, (size_t)raw) : NULL;
    free(d);
    return t;
}

static void restbl_overlay(void) {
    int player = 0;
    for (size_t k = 0; k < noverlay; k++) player |= overlays[k].mod || overlays[k].prio == 1;
    if (!player || !restbl_rel[0]) return;
    static const char keeps[] = "mod files that grew may fail to load";
    double t0 = now_s();
    uint64_t gi = lower(restbl_rel);
    Entry ge = entry(gi);
    void *gz = ge.size < (64u << 20) ? malloc((size_t)ge.size) : NULL;
    Restbl *base = NULL, *out = NULL;
    if (gz && read_at(image, gz, (uint32_t)ge.size, ge.offset) == (int64_t)ge.size) {
        base = restbl_unpack(gz, (size_t)ge.size);
        out = restbl_unpack(gz, (size_t)ge.size);
    }
    free(gz);
    if (!base || !out) {
        rt_log("RomFS: cannot read the resource size table %s; %s", restbl_rel, keeps);
        restbl_free(base);
        restbl_free(out);
        return;
    }
    int tables = 0, merged = 0, raised = 0, estimated = 0;
    for (size_t s = 0; s < nshipped; s++) {
        uint64_t n = 0;
        void *z = plat_file_read(shipped[s].file, &n);
        Restbl *t = z ? restbl_unpack(z, (size_t)n) : NULL;
        free(z);
        if (!t) {
            rt_log("RomFS: %s is not a resource size table; left out", shipped[s].file);
            continue;
        }
        tables++;
        if (!shipped[s].other) {
            merged += restbl_merge(out, base, t);
        } else {
            for (size_t k = 0; k < noverlay; k++) {
                char key[4200];
                const char *kp = restbl_key(overlays[k].name, key, sizeof key);
                if (overlays[k].mod == shipped[s].mod) merged += restbl_take(out, t, &kp, 1);
            }
        }
        restbl_free(t);
    }
    Overlay *served = NULL;
    for (size_t k = 0; k < noverlay; k++) {
        Overlay *o = &overlays[k];
        if (o->index == gi) served = o;
        if ((!o->mod && o->prio != 1) || o->index == gi) continue;
        char key[4200];
        restbl_key(o->name, key, sizeof key);
        int mc = ends_with(o->name, ".mc");
        PlatFile *f = open_read(o->path);
        uint64_t now = mc ? o->size : content_size(f, 0, o->size, o->name);
        if (f) plat_file_close(f);
        uint32_t entry_size, want;
        if (o->index != ADDED) {
            if (!restbl_get(base, key, &entry_size)) continue;
            Entry e = entry(o->index);
            uint64_t was = mc ? e.size : content_size(image, e.offset, e.size, o->name);
            want = restbl_grow(key, entry_size, was, now, mc);
        } else {
            want = restbl_estimate(key, mc ? now * 8 : now);
            estimated += want != 0;
        }
        raised += want && restbl_raise(out, key, want);
    }
    restbl_free(base);
    if (!merged && !raised && !served) {
        restbl_free(out);
        rt_log("RomFS: resource size table: the mods' files fit the game's entries (%.0f ms)", (now_s() - t0) * 1000);
        return;
    }
    size_t rn = 0, zn = 0;
    uint8_t *raw = restbl_write(out, &rn);
    restbl_free(out);
    size_t cap = raw ? ZSTD_compressBound(rn) : 0;
    void *z = raw ? malloc(cap) : NULL;
    if (z) zn = ZSTD_compress(z, cap, raw, rn, 3);
    free(raw);
    if (!z || ZSTD_isError(zn)) {
        free(z);
        rt_log("RomFS: cannot pack the merged resource size table; %s", keeps);
        return;
    }
    if (serve_generated(restbl_rel, served, z, zn, "mods", "merged resource sizes", keeps))
        rt_log("RomFS: resource size table: %d tables merged (%d entries), %d entries raised for mod files (%d "
               "added files estimated), %.0f ms", tables, merged, raised, estimated, (now_s() - t0) * 1000);
}

static int restbl_room(const char *rel, uint64_t was, uint64_t now, const char *sub, char *why, size_t why_n) {
    static const char keeps[] = "the file it would size keeps its own size";
    char key[300];
    uint32_t from = 0, to = 0;
    Overlay *base;
    size_t zn = 0, rn = 0;
    if (!restbl_rel[0]) {
        snprintf(why, why_n, "the RomFS has no resource size table");
        return 0;
    }
    void *z = read_served(restbl_rel, &base, &zn, keeps);
    Restbl *t = z ? restbl_unpack(z, zn) : NULL;
    free(z);
    restbl_key(rel, key, sizeof key);
    int ok = t && restbl_fit(t, key, was, now, &from, &to) && to >= now;
    uint8_t *raw = ok ? restbl_write(t, &rn) : NULL;
    restbl_free(t);
    size_t cap = raw ? ZSTD_compressBound(rn) : 0;
    void *out = raw ? malloc(cap) : NULL;
    zn = out ? ZSTD_compress(out, cap, raw, rn, 3) : 0;
    free(raw);
    if (!out || ZSTD_isError(zn)) {
        free(out);
        snprintf(why, why_n, "%s cannot be read, raised or packed again", restbl_rel);
        return 0;
    }
    if (!serve_generated(restbl_rel, base, out, zn, sub, "a larger file's resource size", keeps)) {
        snprintf(why, why_n, "%s cannot be written", restbl_rel);
        return 0;
    }
    rt_log("RomFS: resource size table: %s raised %u -> %u for the file's %llu -> %llu bytes", key, from, to,
           (unsigned long long)was, (unsigned long long)now);
    return 1;
}

static const char *const heapdef_names[] = {"System/HeapSize/Product.Nin_NX_NVN.HeapDef.byml.zs",
                                            "System/HeapSize/Product.Nin_Ounce_NVN2.HeapDef.byml.zs"};
static void heapdef_overlay(void) {
    if (!res_heap_extra_kib()) return;
    static const char keeps[] = "the game keeps its own resolution";
    const char *rel = heapdef_names[0];
    for (size_t k = 0; k < sizeof heapdef_names / sizeof *heapdef_names; k++) {
        uint64_t i = lower(heapdef_names[k]);
        if (i < file_count && !strcmp(entry_name(entry(i)), heapdef_names[k])) {
            rel = heapdef_names[k];
            break;
        }
    }
    Overlay *base;
    size_t n;
    void *in = read_served(rel, &base, &n, keeps), *out = NULL;
    size_t out_n = 0;
    int ok = in && res_heapdef_build(in, n, &out, &out_n);
    free(in);
    if (ok && serve_generated(rel, base, out, out_n, "resolution", "grown heaps", keeps)) res_heapdef_served();
}

#define CAMPACK "Pack/Actor/PlayerCamera.pack.zs"
static void camera_overlay(void) {
    static const char keeps[] = "the camera keeps its own speed";
    Overlay *base, *dic_base;
    size_t n, dn;
    void *in = read_served(CAMPACK, &base, &n, keeps), *out = NULL;
    void *dic = in ? read_served("Pack/ZsDic.pack.zs", &dic_base, &dn, keeps) : NULL;
    size_t out_n = 0;
    int ok = in && dic && cam_pack_build(in, n, dic, dn, &out, &out_n);
    free(in);
    free(dic);
    if (ok && serve_generated(CAMPACK, base, out, out_n, "camera", "camera speed", keeps)) cam_pack_served();
}

static int action_prompt_for = -1;
static void action_prompt_overlay(void) {
    int layout = input_pad_layout_pick(cfg_value("NX2SARIA_PAD_LAYOUT"), 0);
    if (layout == PAD_LAYOUT_NINTENDO) {
        action_prompt_for = layout;
        return;
    }
    static const char keeps[] = "the action prompt keeps the Switch places";
    Overlay *base, *dic_base;
    size_t n, dn, out_n = 0;
    void *in = read_served(ACTION_PROMPT_ARCHIVE, &base, &n, keeps), *out = NULL;
    void *dic = in ? read_served("Pack/ZsDic.pack.zs", &dic_base, &dn, keeps) : NULL;
    int made = in && dic ? action_prompt_build(in, n, dic, dn, 1, &out, &out_n) : -1;
    free(in);
    free(dic);
    if (made == 0 || (made > 0 && serve_generated(ACTION_PROMPT_ARCHIVE, base, out, out_n, "action-prompt",
                                                   "face buttons by label", keeps)))
        action_prompt_for = layout;
}

int romfs_action_prompt(void) { return action_prompt_for; }

static void wide_hud_overlay(void) {
    if (!wide_available()) return;
    unsigned w, h;
    int edges = !strcmp(cfg_value("NX2SARIA_WIDE_HUD"), "edges");
    int center = !strcmp(cfg_value("NX2SARIA_WIDE_MENUS"), "center");
    int wide = wide_start_picture(&w, &h);
    float wider = wide ? wide_hud_out(w, h) : 0;
    WideHudShift shift = {edges ? wider : 0, center && wider > 0 ? (float)(16.0 / 9.0) / ((float)w / h) : 1,
                          !center ? wider : 0, 0};
    char what[240] = "", menus_what[1024] = "";
    for (int m = 0; m < 2; m++) {
        float by = m ? shift.menus : shift.out;
        char *out = m ? menus_what : what;
        size_t cap = m ? sizeof menus_what : sizeof what;
        if (!(by > 0)) continue;
        WideHudCount c[64];
        int n = wide_hud_counts_of(m, c, 64), at = snprintf(out, cap, "%g layout units %s: ", (double)by,
                                                             m ? "wider on each side" : "out");
        if (m && !n) snprintf(out, cap, "no layout has a redesign yet, the menus stay in the middle 16:9");
        for (int i = 0; i < n && at > 0 && at < (int)cap; i++) {
            at += snprintf(out + at, cap - at, "%s%s %d panes", i ? ", " : "", c[i].layout, c[i].panes);
            if (c[i].keys && at < (int)cap) at += snprintf(out + at, cap - at, " and %d animation keys", c[i].keys);
        }
    }
    rt_log("wide hud: %s, picture %ux%u (%.3f:1, 16:9 is 1.778): %s", edges ? "edges" : "center", w, h,
           (double)w / h, shift.out > 0 ? what : "no pane moves, the HUD stays in the middle 16:9");
    if (shift.menus > 0) rt_log("wide menus: full, picture %ux%u: %s", w, h, menus_what);
    if (!(shift.out > 0) && !(shift.squeeze < 1) && !(shift.menus > 0)) return;
    for (int archive = WIDE_HUD_COMMON; archive <= WIDE_HUD_TITLE; archive++) {
        const char *rel = wide_hud_archive_path(archive);
        static const char keeps[] = "the HUD and the loading icon stay where the game lays them out";
        static const char keeps_title[] = "the title menu's backdrop stays in the middle 16:9";
        const char *keep = archive == WIDE_HUD_TITLE ? keeps_title : keeps;
        if (archive == WIDE_HUD_TITLE && !(shift.menus > 0)) continue;
        Overlay *base, *dic_base;
        size_t n, dn, out_n = 0;
        void *in = read_served(rel, &base, &n, keep), *z = NULL;
        void *dic = in ? read_served("Pack/ZsDic.pack.zs", &dic_base, &dn, keep) : NULL;
        int made = in && dic ? wide_hud_build_in(archive, in, n, dic, dn, &shift, &z, &out_n) : -1;
        unsigned long long was = made > 0 ? ZSTD_getFrameContentSize(in, n) : 0,
                           now = made > 0 ? ZSTD_getFrameContentSize(z, out_n) : 0;
        char why[300] = "the archive's decompressed size cannot be read";
        int sized = was < ZSTD_CONTENTSIZE_ERROR && now < ZSTD_CONTENTSIZE_ERROR;
        if (made > 0 && (!sized || (now > was && !restbl_room(rel, was, now, "wide-hud", why, sizeof why)))) {
            rt_log("wide hud: %s; %s is made again without the edits that make it larger", why, rel);
            free(z);
            z = NULL;
            WideHudShift same = shift;
            same.no_growth = 1;
            made = wide_hud_build_in(archive, in, n, dic, dn, &same, &z, &out_n);
        }
        free(in);
        free(dic);
        char what_made[160] = "";
        if (archive == WIDE_HUD_TITLE) {
            snprintf(what_made, sizeof what_made, "the title menu's backdrop over the whole picture");
        } else {
            if (shift.out > 0) snprintf(what_made, sizeof what_made, "the HUD at the picture's edges");
            if (shift.menus > 0 || shift.squeeze < 1)
                snprintf(what_made + strlen(what_made), sizeof what_made - strlen(what_made), "%s%s",
                         what_made[0] ? ", " : "",
                         shift.menus > 0 ? "the menus over the whole picture" : "the loading icon in the middle 16:9");
        }
        if (made > 0) serve_generated(rel, served_overlay(rel), z, out_n, "wide-hud", what_made, keep);
    }
}

#define PROMPTS_PADS 3
#define PROMPTS_FOLDERS (PROMPTS_PADS * 2 + 1)
static const char *prompts_style(int pick) {
    return pick == PROMPTS_PC ? "keyboard and mouse" : pick == PROMPTS_PLAYSTATION ? "PlayStation"
           : pick == PROMPTS_STEAMDECK ? "Steam Deck" : "Xbox";
}
static const char *prompts_value(int pick) {
    return pick == PROMPTS_PC ? "pc" : pick == PROMPTS_PLAYSTATION ? "playstation"
           : pick == PROMPTS_STEAMDECK ? "steamdeck" : "xbox";
}
static struct { const char *name; int ready; } prompts_folders[PROMPTS_FOLDERS];
static const char *prompts_wanted, *prompts_loaded;
static int prompts_off;
static char pc_dir[64];

const char *romfs_prompts(void) { return prompts_loaded; }

int romfs_prompts_ready(const char *folder) {
    for (int i = 0; i < PROMPTS_FOLDERS; i++)
        if (folder && prompts_folders[i].name && !strcmp(prompts_folders[i].name, folder)) return prompts_folders[i].ready;
    return 0;
}

int romfs_prompts_off(const char *folder) {
    return folder && prompts_wanted && !strcmp(folder, prompts_wanted) ? prompts_off : ROMFS_PROMPTS_OK;
}

static const char *overlay_clash(size_t a, size_t b, size_t c, size_t d) {
    for (size_t i = a; i < b; i++)
        for (size_t j = c; j < d; j++)
            if (overlays[i].index != ADDED && overlays[i].index == overlays[j].index &&
                strcmp(overlays[i].name, restbl_rel))
                return overlays[i].name;
    return NULL;
}

static void overlay_drop(size_t a, size_t b) {
    for (size_t k = a; k < b; k++) {
        free((void *)overlays[k].path);
        free((void *)overlays[k].name);
    }
    memmove(overlays + a, overlays + b, (noverlay - b) * sizeof *overlays);
    noverlay -= b - a;
}

static void overlay_init(void) {
    char def[2100], path[2100];
    const char *patches = cfg_get("NX2SARIA_ROMFS_PATCHES"), *mods = cfg_get("NX2SARIA_ROMFS_MODS");
    int wide = 0;
    unsigned out[2];
    res_wanted(out);
    if (!patches && res_wants_wide_overlay(out)) {
        if (snprintf(def, sizeof def, "%s/romfs-patches-%ux%u/manifest.tsv", index_dir, out[0], out[1]) < (int)sizeof def &&
            plat_file_exists(def)) {
            def[strlen(def) - strlen("/manifest.tsv")] = 0;
            patches = def, wide = 1;
        } else {
            rt_log("RomFS overlay: no romfs-patches-%ux%u for this game resolution, the UI keeps its 16:9 layout "
                   "(tools/make_romfs_patches.py --aspect %ux%u)", out[0], out[1], out[0], out[1]);
        }
    }
    static const char *const layouts[2] = {"nintendo", "xbox"};
    static const int pads[PROMPTS_PADS] = {PROMPTS_XBOX, PROMPTS_PLAYSTATION, PROMPTS_STEAMDECK};
    for (int i = 0; i < PROMPTS_FOLDERS; i++) {
        int pc = i == PROMPTS_FOLDERS - 1;
        const char *name = pc ? input_prompts_folder(PROMPTS_PC, 0) : input_prompts_folder(pads[i / 2], layouts[i % 2]);
        const char *dir = name;
        if (pc) {
            if (wide) snprintf(pc_dir, sizeof pc_dir, "%s-%ux%u", name, out[0], out[1]);
            else snprintf(pc_dir, sizeof pc_dir, "%s", name);
            dir = pc_dir;
        }
        prompts_folders[i].name = name;
        prompts_folders[i].ready =
            snprintf(path, sizeof path, "%s/%s/manifest.tsv", index_dir, dir) < (int)sizeof path && plat_file_exists(path);
    }
    const char *layout = cfg_value("NX2SARIA_PAD_LAYOUT");
    int pick = input_prompts_pick(cfg_get("NX2SARIA_PROMPTS"), layout), shaped = wide && pick == PROMPTS_PC;
    const char *style = prompts_style(pick);
    prompts_wanted = input_prompts_folder(pick, layout);
    prompts_off = !prompts_wanted                         ? ROMFS_PROMPTS_OK
                  : wide && !shaped                       ? ROMFS_PROMPTS_SHAPE
                  : patches && !wide                      ? ROMFS_PROMPTS_PATCHES
                  : !romfs_prompts_ready(prompts_wanted)  ? (shaped ? ROMFS_PROMPTS_SHAPE : ROMFS_PROMPTS_MISSING)
                                                          : ROMFS_PROMPTS_OK;
    char record[512];
    if (input_prompts_record(index_dir, record, sizeof record))
        rt_log("button prompts: %s (button-prompts.txt)", record);
    else
        rt_log("button prompts: the game folder has no button-prompts.txt (a build before NX2Saria recorded where "
               "its prompts come from, or a game folder made by hand)");
    restbl_find();
    size_t first = noverlay;
    if (prompts_wanted && prompts_off == ROMFS_PROMPTS_OK) {
        snprintf(path, sizeof path, "%s/%s", index_dir, shaped ? pc_dir : prompts_wanted);
        if (overlay_load(path, 1, 0)) {
            prompts_loaded = prompts_wanted;
            rt_log("RomFS overlay: %s button prompts (%s)", style, path);
            if (shaped) patches = NULL;
        } else {
            prompts_off = ROMFS_PROMPTS_FAILED;
            rt_log("RomFS overlay: %s failed its check (run the setup again); romfs-patches loads instead", path);
        }
    } else if (prompts_wanted) {
        rt_log("RomFS overlay: prompts = %s, but %s; Switch prompts stay",
               prompts_value(pick),
               prompts_off == ROMFS_PROMPTS_SHAPE     ? (shaped ? "the game resolution has no romfs-patches-pc-<w>x<h> "
                                                                  "(tools/make_romfs_patches.py --prompts pc --aspect)"
                                                             : "the game resolution takes its own UI layout")
               : prompts_off == ROMFS_PROMPTS_PATCHES ? "NX2SARIA_ROMFS_PATCHES is set"
                                                      : "their folder has no manifest.tsv (the build draws it when its "
                                                        "version carries button art: see the button prompts line)");
    }
    size_t prompts_end = noverlay;
    char plain[2100];
    int plain_ok = snprintf(plain, sizeof plain, "%s/romfs-patches/manifest.tsv", index_dir) < (int)sizeof plain &&
                   plat_file_exists(plain);
    if (plain_ok) plain[strlen(plain) - strlen("/manifest.tsv")] = 0;
    if (!patches && !prompts_loaded && plain_ok) patches = plain;
    if (patches && *patches && strcmp(patches, "0")) overlay_load(patches, 0, 0);
    size_t mods_first = noverlay;
    if (mods && *mods && strcmp(mods, "0")) overlay_load(mods, 0, 1);
    mods_overlay_init();
    const char *clash = prompts_loaded ? overlay_clash(first, prompts_end, mods_first, noverlay) : NULL;
    if (clash) {
        rt_log("RomFS overlay: a RomFS mod replaces %s, which the %s prompts change too; Switch prompts stay", clash,
               style);
        overlay_drop(first, prompts_end);
        prompts_loaded = NULL;
        prompts_off = ROMFS_PROMPTS_MODS;
        if (shaped) overlay_load(def, 0, 0);
        else if (plain_ok) overlay_load(plain, 0, 0);
    }
    restbl_collect();
    overlay_contest();
    mods_overlay_log();
    restbl_overlay();
    heapdef_overlay();
    camera_overlay();
    action_prompt_overlay();
    wide_hud_overlay();
    added_init();
    if (noverlay) qsort(overlays, noverlay, sizeof *overlays, overlay_order);
    for (size_t k = 1; k < noverlay; k++)
        if (overlays[k].index == overlays[k - 1].index)
            rt_fatal("RomFS overlay: %s and %s replace the same file %s", overlays[k - 1].path, overlays[k].path,
                     overlays[k].name);
}

static void index_path(char *out, size_t cap) {
    const char *e = cfg_get("NX2SARIA_ROMFS_INDEX");
    if (e && *e) {
        need(snprintf(out, cap, "%s", e) < (int)cap, "index path");
        return;
    }
    need(plat_exe_path(out, cap), "executable path");
    char *sep = plat_last_sep(out);
    size_t k = sep ? (size_t)(sep - out) + 1 : 0;
    need(k + 16 < cap, "executable path");
    strcpy(out + k, "romfs-index.bin");
}

static void load_index(void) {
    char path[2100];
    index_path(path, sizeof path);
    snprintf(index_dir, sizeof index_dir, "%s", path);
    char *sep = plat_last_sep(index_dir);
    if (sep) *sep = 0;
    else strcpy(index_dir, ".");
    uint64_t sz = 0;
    index_bytes = plat_file_read(path, &sz);
    if (!index_bytes) rt_fatal("RomFS index %s missing (tools/build_game.py writes it next to nx2saria.exe)", path);
    need(sz >= 24 && sz < (128 << 20), "index size");
    index_size = (size_t)sz;
    uint8_t d[32];
    char hex[65];
    rt_sha256(index_bytes, index_size, d);
    hex32(d, hex);
    if (strcmp(hex, ROMFS_INDEX_SHA256))
        rt_fatal("RomFS index %s has SHA-256 %s, this build expects %s (rebuild with tools/build_game.py)", path, hex,
                 ROMFS_INDEX_SHA256);
    need(!memcmp(index_bytes, "NX2NRFS1", 8), "index format");
    memcpy(&file_count, index_bytes + 8, 8);
    memcpy(&strings_offset, index_bytes + 16, 8);
    need(file_count == ROMFS_FILE_COUNT && file_count < 1000000 && strings_offset == 24 + file_count * 24 &&
         strings_offset < index_size, "index layout");
    const char *previous = NULL;
    for (uint64_t i = 0; i < file_count; i++) {
        Entry e = entry(i);
        need(e.offset <= ROMFS_IMAGE_SIZE && e.size <= ROMFS_IMAGE_SIZE - e.offset, "file bounds");
        need(e.name < index_size - strings_offset && e.len > 1 && e.len <= index_size - strings_offset - e.name,
             "name bounds");
        const char *name = entry_name(e);
        need(name[e.len - 1] == 0 && strlen(name) == e.len - 1 && (!previous || strcmp(previous, name) < 0),
             "sorted paths");
        previous = name;
    }
    rt_log("RomFS index %s: SHA-256 ok, %llu files, %zu bytes", path, (unsigned long long)file_count, index_size);
}

static void init_once(void *param) {
    (void)param;
#ifdef ROMFS_MISSING
    rt_fatal("this build has no RomFS: tools/build_game.py found no index.json in its --romfs directory");
#else
    double t0 = now_s();
    load_index();
    const char *dir = rt_config()->romfs;
    need(snprintf(image_file, sizeof image_file, "%s/romfs.bin", dir) < (int)sizeof image_file, "image path");
    image = open_read(image_file);
    if (!image) rt_fatal("RomFS image %s missing; put romfs.bin into the romfs folder next to the game executable or set NX2SARIA_ROMFS",
                         image_file);
    PlatFileId sz;
    need(plat_file_id(image, &sz), "image size");
    if (sz.size != ROMFS_IMAGE_SIZE)
        rt_fatal("RomFS image %s has %llu bytes, the index was built for %llu", image_file,
                 (unsigned long long)sz.size, (unsigned long long)ROMFS_IMAGE_SIZE);
    rt_log("RomFS image %s: size ok (%llu bytes)", image_file, (unsigned long long)ROMFS_IMAGE_SIZE);
    verify_image(index_dir);
    const char *mm = cfg_get("NX2SARIA_ROMFS_MMAP");
    if (mm && strcmp(mm, "0")) {
        const void *v = plat_file_map(image);
        if (v) {
            image_map = v;
            rt_log("RomFS: mapped read path enabled");
        } else {
            rt_log("RomFS: mapping failed; using positional reads");
        }
    }
    overlay_init();
    open_ms = calloc(file_count, sizeof *open_ms), open_n = calloc(file_count, sizeof *open_n);
    rt_log("RomFS ready: %llu files, %zu overlays, %zu added, %.2f s", (unsigned long long)file_count, noverlay,
           nadded, now_s() - t0);
#endif
    return;
}

void fs_init(void) {
    static PlatOnce once = PLAT_ONCE_INIT;
    plat_once(&once, init_once, NULL);
}

void fs_trace(const char *what, const char *path) {
    static double t0;
    if (!(rt_trace & RT_TRACE_FS)) return;
    if (!t0) t0 = now_s();
    rt_log("fs %8.3f %s %s", now_s() - t0, what, path ? path : "(null)");
}

uint64_t romfs_file_count(void) { return file_count; }

const char *romfs_entry(uint64_t i, uint64_t *offset, uint64_t *size) {
    if (i >= file_count) return NULL;
    Entry e = entry(i);
    if (offset) *offset = e.offset;
    if (size) *size = e.size;
    return entry_name(e);
}

const char *romfs_image_path(void) { return image_file; }
uint8_t *romfs_load(const char *rel, uint64_t *size_out) {
    fs_init();
    if (!rel || !image) return NULL;
    uint64_t i = file_index(rel);
    if (i >= file_count) return NULL;
    Entry e = entry(i);
    uint8_t *b = malloc(e.size ? (size_t)e.size : 1);
    if (!b) return NULL;
    if (image_map) {
        memcpy(b, image_map + e.offset, (size_t)e.size);
    } else {
        for (uint64_t done = 0; done < e.size;) {
            uint32_t chunk = e.size - done > READ_CHUNK ? READ_CHUNK : (uint32_t)(e.size - done);
            int64_t r = read_at(image, b + done, chunk, e.offset + done);
            if (r <= 0) { free(b); return NULL; }
            done += (uint64_t)r;
        }
    }
    if (size_out) *size_out = e.size;
    return b;
}


size_t romfs_overlay_count(void) { return noverlay; }

int romfs_patches_loaded(void) { return patches_loaded; }

const char *romfs_overlay(size_t k, const char **file, uint64_t *size, int *is_added) {
    if (k >= noverlay) return NULL;
    *file = overlays[k].path;
    *size = overlays[k].size;
    *is_added = overlays[k].index >= file_count;
    return overlays[k].name;
}

int romfs_owns(const char *path) {
    if (!path) return 0;
    for (int i = 0; i < NROM_NAMES && mount_names[i][0]; i++) {
        size_t n = strlen(mount_names[i]);
        if (!strncmp(path, mount_names[i], n) && path[n] == ':') return 1;
    }
    return 0;
}

int romfs_handle(uint64_t h) { return h > ROMFS_HANDLE_BASE && h < ROMFS_HANDLE_BASE + NH; }

uint32_t romfs_mount(const char *name) {
    fs_init();
    if (!name || !romfs_path_valid_relative(name) || strlen(name) >= sizeof mount_names[0] || strchr(name, '/') ||
        strchr(name, ':'))
        return FS_R_INVALID;
    plat_mutex_lock(&lock);
    int i = 0;
    while (i < NROM_NAMES && mount_names[i][0] && strcmp(mount_names[i], name)) i++;
    if (i == NROM_NAMES) rt_fatal("RomFS mounted under more than %d names (%s)", NROM_NAMES, name);
    strcpy(mount_names[i], name);
    plat_mutex_unlock(&lock);
    rt_log("fs mount %s: -> RomFS", name);
    return 0;
}

static int movie_skipped(const char *p) {
    size_t n = strlen(p);
    if (n < 5 || strcmp(p + n - 5, ".webm")) return 0;
    const char *v = cfg_get("NX2SARIA_MOVIES");
    if (!v || strcmp(v, "off")) return 0;
    rt_log("fs: movie %s skipped (NX2SARIA_MOVIES=off)", p);
    return 1;
}

#define RETRY_OPENS 24
#define RETRY_GAP_MS 3000

static void open_note(uint64_t i, const char *path) {
    if (!open_n || i >= file_count) return;
    uint32_t now = (uint32_t)(plat_now_ns() / 1000000);
    if (now - open_ms[i] > RETRY_GAP_MS) open_n[i] = 0;
    open_ms[i] = now;
    if (open_n[i] < RETRY_OPENS && ++open_n[i] == RETRY_OPENS)
        rt_log("RomFS: the game keeps opening %s and cannot load it; what needs it stays out of the world "
               "(romfs-check.json lists the files found damaged)", path);
}

static int area_name(const char *p, char *name, size_t cap) {
    static const char tail[] = "_Static.bcett.byml";
    if (strncmp(p, "Banc/", 5)) return 0;
    const char *leaf = strrchr(p, '/'), *t = leaf ? strstr(leaf, tail) : NULL;
    if (!t || (t[sizeof tail - 1] && strcmp(t + sizeof tail - 1, ".zs"))) return 0;
    snprintf(name, cap, "%.*s", (int)(t - leaf - 1), leaf + 1);
    return 1;
}

static void scene_note(const char *p) {
    char name[96];
    if (area_name(p, name, sizeof name)) perf_scene(name);
}

static char area_missing_name[96];

static void area_missing_fatal_note(void) {
    if (area_missing_name[0])
        rt_log("fatal: the game asked for the area %s, which this game version does not have; a save that names it "
               "cannot be loaded", area_missing_name);
}

static void area_missing(const char *p) {
    char name[96];
    if (!area_name(p, name, sizeof name)) return;
    plat_mutex_lock(&lock);
    int first = !area_missing_name[0];
    if (first) snprintf(area_missing_name, sizeof area_missing_name, "%s", name);
    plat_mutex_unlock(&lock);
    if (!first) return;
    rt_log("RomFS: the game asked for the area %s (%s), which this game version does not have; the save being loaded "
           "names it and cannot be loaded here", name, p);
    rt_fatal_note_add(area_missing_fatal_note);
}

uint32_t romfs_open(uint64_t *out, const char *path, int mode) {
    fs_trace("rom", path);
    if (!out || mode != FS_OPEN_READ) return FS_R_UNSUPPORTED_OPEN;
    const char *p = rom_relative(path);
    if (!p) return FS_R_PATH_NOT_FOUND;
    if (movie_skipped(p)) return FS_R_PATH_NOT_FOUND;
    if (!strcmp(p, "npln_config.json") && cfg_get("NX2SARIA_NPLN") && !strcmp(cfg_get("NX2SARIA_NPLN"), "off")) {
        rt_log("fs: %s hidden (NX2SARIA_NPLN=off)", p);
        return FS_R_PATH_NOT_FOUND;
    }
    uint64_t i = file_index(p);
    if (i == UINT64_MAX) {
        area_missing(p);
        return FS_R_PATH_NOT_FOUND;
    }
    open_note(i, p);
    if ((rt_trace & RT_TRACE_PERF) || stutter_map_on) scene_note(p);
    if (!strcasecmp(p, CAMPACK)) cam_pack_opened();
    uint32_t ov = overlay_slot(i);
    PlatFile *f = NULL;
    if (ov) {
        const Overlay *o = &overlays[ov - 1];
        f = open_read(o->path);
        Identity id;
        if (f) id = identity_of(f);
        if (!f || memcmp(&id, &o->id, sizeof id))
            rt_fatal("RomFS overlay %s is missing or changed since the start check (restart the game)", o->path);
    }
    plat_mutex_lock(&lock);
    for (unsigned j = 1; j < NH; j++)
        if (!handles[j]) {
            handles[j] = i + 1;
            handle_ovl[j] = ov;
            handle_file[j] = f;
            *out = ROMFS_HANDLE_BASE + j;
            plat_mutex_unlock(&lock);
            return 0;
        }
    plat_mutex_unlock(&lock);
    rt_fatal("RomFS file handle table full (%d)", NH);
}

static int handle_source(uint64_t h, PlatFile **f, uint64_t *base, uint64_t *size) {
    if (!romfs_handle(h)) return 0;
    uint64_t j = h - ROMFS_HANDLE_BASE;
    plat_mutex_lock_shared(&lock);
    uint64_t i = handles[j];
    uint32_t ov = handle_ovl[j];
    PlatFile *of = handle_file[j];
    plat_mutex_unlock_shared(&lock);
    if (!i) return 0;
    if (ov) {
        *f = of;
        *base = 0;
        *size = overlays[ov - 1].size;
    } else {
        Entry e = entry(i - 1);
        *f = image;
        *base = e.offset;
        *size = e.size;
    }
    return 1;
}

#define READ_PATCH_MAX 8
static struct { uint64_t base, offset; uint32_t n; uint8_t repl[16]; } read_patch[READ_PATCH_MAX];
static int read_patch_n;
static int32_t read_patch_on;

int romfs_read_patch(const char *rel, uint64_t offset, const void *orig, const void *repl, uint32_t n, char *why, size_t cap) {
    fs_init();
    uint64_t i = rel && image ? file_index(rel) : UINT64_MAX;
    uint8_t have[16];
    const char *no = i >= file_count                               ? "not in the RomFS"
                     : overlay_slot(i)                             ? "served by a mod or a RomFS overlay"
                     : !n || n > sizeof have                       ? "too many bytes to change"
                     : read_patch_n == READ_PATCH_MAX              ? "one change too many (the table is full)"
                                                                   : NULL;
    if (!no) {
        Entry e = entry(i);
        if (offset + n > e.size) no = "shorter than the change";
        else if (read_at(image, have, n, e.offset + offset) != (int64_t)n || memcmp(have, orig, n)) no = "not the file the change is for";
        else {
            read_patch[read_patch_n].base = e.offset, read_patch[read_patch_n].offset = offset, read_patch[read_patch_n].n = n;
            memcpy(read_patch[read_patch_n].repl, repl, n);
            read_patch_n++;
            __atomic_store_n(&read_patch_on, 1, __ATOMIC_RELEASE);
        }
    }
    if (no && why) snprintf(why, cap, "%s %s", rel ? rel : "(none)", no);
    return !no;
}

void romfs_read_patches_off(void) { __atomic_store_n(&read_patch_on, 0, __ATOMIC_RELEASE); }

static void read_patch_apply(uint64_t base, uint64_t offset, uint8_t *buf, uint64_t n) {
    for (int k = 0; k < read_patch_n; k++) {
        uint64_t a = read_patch[k].offset, b = a + read_patch[k].n;
        if (read_patch[k].base != base || b <= offset || a >= offset + n) continue;
        uint64_t from = a > offset ? a : offset, to = b < offset + n ? b : offset + n;
        memcpy(buf + (from - offset), read_patch[k].repl + (from - a), (size_t)(to - from));
    }
}

uint32_t romfs_read(uint64_t *out, uint64_t h, int64_t offset, void *buffer, uint64_t size) {
    PlatFile *f;
    uint64_t base, fsize;
    if (!out || offset < 0 || !handle_source(h, &f, &base, &fsize)) return FS_R_INVALID;
    if ((uint64_t)offset > fsize || (!buffer && size)) return FS_R_INVALID;
    uint64_t n = size < fsize - (uint64_t)offset ? size : fsize - (uint64_t)offset, done = 0, t0 = perf_mark_begin();
    if (f == image && image_map) {
        memcpy(buffer, image_map + base + (uint64_t)offset, (size_t)n);
        done = n;
    }
    while (done < n) {
        uint32_t chunk = n - done > READ_CHUNK ? READ_CHUNK : (uint32_t)(n - done);
        int64_t r = read_at(f, (uint8_t *)buffer + done, chunk, base + (uint64_t)offset + done);
        if (r <= 0) rt_fatal("RomFS read failure at %#llx", (unsigned long long)(base + offset + done));
        done += (uint64_t)r;
    }
    if (f == image && __atomic_load_n(&read_patch_on, __ATOMIC_ACQUIRE)) read_patch_apply(base, (uint64_t)offset, buffer, done);
    perf_mark_end(PERF_M_FILE, t0, done);
    *out = done;
    return 0;
}

uint32_t romfs_size(int64_t *out, uint64_t h) {
    PlatFile *f;
    uint64_t base, size;
    if (!out || !handle_source(h, &f, &base, &size)) return FS_R_INVALID;
    *out = (int64_t)size;
    return 0;
}

void romfs_close(uint64_t h) {
    PlatFile *f;
    uint64_t base, size;
    if (!handle_source(h, &f, &base, &size))
        rt_fatal("RomFS: close of invalid file handle %#llx", (unsigned long long)h);
    uint64_t j = h - ROMFS_HANDLE_BASE;
    plat_mutex_lock(&lock);
    PlatFile *of = handle_ovl[j] ? handle_file[j] : NULL;
    handles[j] = 0;
    handle_ovl[j] = 0;
    handle_file[j] = NULL;
    plat_mutex_unlock(&lock);
    if (of) plat_file_close(of);
}

uint32_t romfs_entry_type(int *out, const char *path) {
    if (!out) return FS_R_INVALID;
    const char *p = rom_relative(path);
    if (!p) return FS_R_PATH_NOT_FOUND;
    if (file_index(p) != UINT64_MAX) {
        *out = FS_ENTRY_FILE;
        return 0;
    }
    char dir[4096];
    if (snprintf(dir, sizeof dir, "%s/", p) >= (int)sizeof dir) return FS_R_INVALID;
    if (folder_exists(dir)) {
        *out = FS_ENTRY_DIRECTORY;
        return 0;
    }
    return FS_R_PATH_NOT_FOUND;
}

uint32_t romfs_list(const char *path, int (*fn)(const char *name, int is_dir, uint64_t size, void *ctx), void *ctx) {
    const char *p = rom_relative(path);
    if (!p) return FS_R_PATH_NOT_FOUND;
    char dir[4096];
    if (snprintf(dir, sizeof dir, *p ? "%s/" : "%s", p) >= (int)sizeof dir) return FS_R_INVALID;
    size_t n = strlen(dir);
    if (n && !folder_exists(dir)) return FS_R_PATH_NOT_FOUND;
    char last[4096] = "";
    for (uint64_t i = lower(dir); i < file_count; i++) {
        Entry e = entry(i);
        const char *name = entry_name(e);
        if (strncmp(name, dir, n)) break;
        const char *rest = name + n, *slash = strchr(rest, '/');
        if (!slash) {
            if (!fn(rest, 0, e.size, ctx)) break;
            continue;
        }
        size_t k = (size_t)(slash - rest);
        if (k >= sizeof last) continue;
        if (!strncmp(last, rest, k) && last[k] == 0) continue;
        memcpy(last, rest, k);
        last[k] = 0;
        if (!fn(last, 1, 0, ctx)) break;
    }
    return 0;
}

static void ov_mount_rom(Cpu *c) { ret_x(c, romfs_mount(arg_p(c, 0))); }
static void ov_query_cache(Cpu *c) {
    uint64_t *out = arg_p(c, 0);
    if (out) *out = 0;
    ret_x(c, out ? 0 : FS_R_INVALID);
}
static void ov_initialize(Cpu *c) {
    (void)c;
    fs_init();
}

HOST_OVERRIDE("_ZN2nn2fs8MountRomEPKcPvm", ov_mount_rom);
HOST_OVERRIDE("_ZN2nn2fs8MountRomEPKc", ov_mount_rom);
HOST_OVERRIDE("_ZN2nn2fs22QueryMountRomCacheSizeEPm", ov_query_cache);
HOST_OVERRIDE("_ZN2nn2fs24InitializeForApplicationEv", ov_initialize);

void romfs_sizes(PerfSizes *s) {
    uint64_t n = 0;
    plat_mutex_lock_shared(&lock);
    for (unsigned j = 1; j < NH; j++) n += handles[j] != 0;
    plat_mutex_unlock_shared(&lock);
    perf_size(s, "RomFS handles open", n, NH - 1, PERF_SIZE_NONE);
}
