// The parts of boot_map.c that only read and change bytes, apart so that tests/boot_map_check.c runs them on made-up
// files: the two sequence graphs (AINB) the title runs, the save file, the file menu's card, the setting's values.
// Ocarina of Time 1.1.1. docs/boot-to-map.md says what each change is for.
#pragma once
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// One change of bytes: at off, n bytes (at most 64) become repl; orig is what the file held there.
typedef struct {
    uint64_t off;
    uint32_t n;
    uint8_t orig[64], repl[64];
} BmEdit;

static inline uint32_t bm_u32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline uint16_t bm_u16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }

// The hash the game knows a game data value by: MurmurHash3 (x86, 32 bits, seed 0) of its name.
static inline uint32_t bm_hash(const char *s) {
    const uint8_t *p = (const uint8_t *)s;
    size_t len = strlen(s), n = len & ~(size_t)3;
    uint32_t h = 0, k;
    for (size_t i = 0; i < n; i += 4) {
        k = bm_u32(p + i) * 0xcc9e2d51u, k = (k << 15) | (k >> 17), k *= 0x1b873593u;
        h ^= k, h = (h << 13) | (h >> 19), h = h * 5 + 0xe6546b64u;
    }
    k = 0;
    switch (len & 3) {
    case 3: k ^= (uint32_t)p[n + 2] << 16;   // fall through
    case 2: k ^= (uint32_t)p[n + 1] << 8;    // fall through
    case 1: k ^= p[n], k *= 0xcc9e2d51u, k = (k << 15) | (k >> 17), k *= 0x1b873593u, h ^= k;
    }
    h ^= (uint32_t)len;
    h ^= h >> 16, h *= 0x85ebca6bu, h ^= h >> 13, h *= 0xc2b2ae35u, h ^= h >> 16;
    return h;
}

static inline void bm_why(char *why, size_t cap, const char *text) {
    if (why && cap) snprintf(why, cap, "%s", text);
}

// ---- AINB, version 0x408. Header: +0xc the commands' count, +0x10 the nodes', +0x24 the string pool (names are
// offsets into it), +0x34 the input and output parameters (twelve offsets: inputs, outputs of int, bool, float,
// string, vec3f, ptr). The commands follow at +0x74, 0x18 bytes each: the name, a GUID, the root node (u16 at +0x14).
// The nodes follow, 0x3c bytes each: type (u16; 2 Sequential, 3 Simultaneous), name at +8, body at +0x14. A body has at
// +0x30 per parameter type {first input, inputs, first output, outputs} and at +0x90 ten {count, first} byte pairs of
// links, kind 2 the children, whose offsets follow at +0xa4 (each link starts with the node's index). An int input is
// 0x10 bytes: name, the node it comes from (s16, -1 none), that node's output (u16), flags, the value; every input
// without a link in the game's graphs has flag 0x800000.
typedef struct {
    const uint8_t *d;
    size_t n;
    uint32_t ncmd, nnode, pool, io;
} BmAinb;

static inline int bm_ainb_open(BmAinb *a, const uint8_t *d, size_t n) {
    if (n < 0x74 || memcmp(d, "AIB ", 4) || bm_u32(d + 4) != 0x408) return 0;
    a->d = d, a->n = n, a->ncmd = bm_u32(d + 0xc), a->nnode = bm_u32(d + 0x10), a->pool = bm_u32(d + 0x24), a->io = bm_u32(d + 0x34);
    return a->ncmd < 64 && a->nnode < 4096 && a->pool < n && (uint64_t)a->io + 48 <= n && 0x74 + 0x18ull * a->ncmd + 0x3cull * a->nnode <= n;
}

// The pool's text at off, NULL when it runs past the file.
static inline const char *bm_ainb_str(const BmAinb *a, uint32_t off) {
    if ((uint64_t)a->pool + off >= a->n || !memchr(a->d + a->pool + off, 0, a->n - a->pool - off)) return NULL;
    return (const char *)a->d + a->pool + off;
}

static inline uint64_t bm_ainb_node(const BmAinb *a, uint32_t i) { return 0x74 + 0x18ull * a->ncmd + 0x3cull * i; }
static inline int bm_ainb_type(const BmAinb *a, uint32_t i) { return bm_u16(a->d + bm_ainb_node(a, i)); }
static inline int bm_ainb_named(const BmAinb *a, uint32_t i, const char *name) {
    const char *s = bm_ainb_str(a, bm_u32(a->d + bm_ainb_node(a, i) + 8));
    return s && !strcmp(s, name);
}

// The only node named name, or -1 (none, or more than one).
static inline int bm_ainb_find(const BmAinb *a, const char *name) {
    int at = -1;
    for (uint32_t i = 0; i < a->nnode; i++)
        if (bm_ainb_named(a, i, name)) {
            if (at >= 0) return -1;
            at = (int)i;
        }
    return at;
}

// The children of node i (its links of kind 2), at most max; their number, -1 when the node's body runs past the file.
static inline int bm_ainb_children(const BmAinb *a, uint32_t i, int *out, int max) {
    uint32_t body = bm_u32(a->d + bm_ainb_node(a, i) + 0x14);
    if ((uint64_t)body + 0xa4 > a->n) return -1;
    int total = 0, first = 0, count = 0;
    for (int k = 0; k < 10; k++) {
        if (k == 2) count = a->d[body + 0x90 + 2 * k], first = a->d[body + 0x90 + 2 * k + 1];
        total += a->d[body + 0x90 + 2 * k];
    }
    if ((uint64_t)body + 0xa4 + 4ull * (unsigned)total > a->n || first + count > total) return -1;
    for (int j = 0; j < count && j < max; j++) {
        uint32_t link = bm_u32(a->d + body + 0xa4 + 4 * (first + j));
        if ((uint64_t)link + 4 > a->n) return -1;
        out[j] = (int)bm_u32(a->d + link);
    }
    return count;
}

// NormalTitle.module: its command Root starts the title (node 35 in 1.1.1, a Simultaneous that shows the title and
// waits for a button). The edit makes it start the Sequential whose children are the Simultaneous of the scene
// transition's pause and TitleToMainWithLoadSave.module: what Continue and then Yes in the file menu start.
static inline int bm_ainb_title_edit(const uint8_t *d, size_t n, BmEdit *e, char *why, size_t cap) {
    BmAinb a;
    if (!bm_ainb_open(&a, d, n)) return bm_why(why, cap, "not an AINB file of version 0x408"), 0;
    int root_cmd = -1;
    for (uint32_t k = 0; k < a.ncmd; k++) {
        const char *s = bm_ainb_str(&a, bm_u32(d + 0x74 + 0x18 * k));
        if (s && !strcmp(s, "Root")) root_cmd = (int)k;
    }
    int load = bm_ainb_find(&a, "TitleToMainWithLoadSave.module"), target = -1;
    if (root_cmd < 0 || load < 0) return bm_why(why, cap, "no command Root, or not one node TitleToMainWithLoadSave.module"), 0;
    uint32_t root = bm_u16(d + 0x74 + 0x18 * root_cmd + 0x14);
    if (root >= a.nnode || bm_ainb_type(&a, root) != 3) return bm_why(why, cap, "Root does not start a Simultaneous node"), 0;
    for (uint32_t i = 0; i < a.nnode; i++) {
        int ch[4];
        if (bm_ainb_type(&a, i) != 2 || bm_ainb_children(&a, i, ch, 4) != 2 || ch[1] != load) continue;
        if (ch[0] < 0 || (uint32_t)ch[0] >= a.nnode || bm_ainb_type(&a, (uint32_t)ch[0]) != 3) continue;
        if (target >= 0) return bm_why(why, cap, "more than one Sequential runs TitleToMainWithLoadSave.module"), 0;
        target = (int)i;
    }
    if (target < 0) return bm_why(why, cap, "no Sequential of a Simultaneous and TitleToMainWithLoadSave.module"), 0;
    uint16_t to = (uint16_t)target, from = (uint16_t)root;
    e->off = 0x74 + 0x18ull * (unsigned)root_cmd + 0x14, e->n = 2;
    memcpy(e->orig, &from, 2), memcpy(e->repl, &to, 2);
    return 1;
}

// OnLoadGameSaveData.module: SeqExecuteLoadSaveData and SeqOneShotSetCaptionMgrLatestIndex take their Index from the
// query of CaptionData.SelectedSavePathIndex, which the file menu sets. The two edits make each Index a constant, the
// slot: no link (-1), flag 0x800000, the value.
static inline int bm_ainb_slot_edits(const uint8_t *d, size_t n, int slot, BmEdit e[2], char *why, size_t cap) {
    static const char *const nodes[2] = {"SeqExecuteLoadSaveData", "SeqOneShotSetCaptionMgrLatestIndex"};
    BmAinb a;
    if (!bm_ainb_open(&a, d, n)) return bm_why(why, cap, "not an AINB file of version 0x408"), 0;
    uint32_t inputs = bm_u32(d + a.io), inputs_end = bm_u32(d + a.io + 4);
    for (int k = 0; k < 2; k++) {
        int i = bm_ainb_find(&a, nodes[k]), found = 0;
        if (i < 0) return bm_why(why, cap, "the node SeqExecuteLoadSaveData or SeqOneShotSetCaptionMgrLatestIndex is missing"), 0;
        uint32_t body = bm_u32(d + bm_ainb_node(&a, (uint32_t)i) + 0x14);
        if ((uint64_t)body + 0x40 > n) return bm_why(why, cap, "a node's body runs past the file"), 0;
        uint32_t first = bm_u32(d + body + 0x30), count = bm_u32(d + body + 0x34);
        for (uint32_t j = 0; j < count && !found; j++) {
            uint64_t p = inputs + 0x10ull * ((uint64_t)first + j);
            if (p + 0x10 > inputs_end || p + 0x10 > n) return bm_why(why, cap, "an int input runs past its table"), 0;
            const char *name = bm_ainb_str(&a, bm_u32(d + p));
            if (!name || strcmp(name, "Index")) continue;
            int16_t from;
            memcpy(&from, d + p + 4, 2);
            if (from < 0) return bm_why(why, cap, "an Index input is already a constant"), 0;
            int16_t none = -1;
            uint16_t out = 0;
            uint32_t flags = 0x800000;
            int32_t value = slot;
            e[k].off = p + 4, e[k].n = 12;
            memcpy(e[k].orig, d + p + 4, 12);
            memcpy(e[k].repl, &none, 2), memcpy(e[k].repl + 2, &out, 2), memcpy(e[k].repl + 4, &flags, 4), memcpy(e[k].repl + 8, &value, 4);
            found = 1;
        }
        if (!found) return bm_why(why, cap, "a node has no int input Index"), 0;
    }
    return 1;
}

// ---- save files (progress.sav, caption.sav, account.sav): u32 0x01020304, the version, the data's offset; from 0x28
// up to that offset {u32 name hash, u32 value} pairs, where {0, k} starts the values of type k (0 Bool, 2 Int,
// 4 Float, 10 Vector3, 16 String64, 24 UInt64, ...). Bool, Int and Float values are the pair's own; Vector3, String64
// and UInt64 values are the offset of 12, 64 and 8 bytes in the file.
enum { BM_BOOL = 0, BM_INT = 2, BM_FLOAT = 4, BM_VEC3 = 10, BM_STR64 = 16, BM_UINT64 = 24 };

// The place of a value: the pair's offset; *value its second word. 0 when the file has no such value.
static inline uint64_t bm_save_find(const uint8_t *d, size_t n, int type, uint32_t hash, uint32_t *value) {
    if (n < 0x28 || bm_u32(d) != 0x01020304u) return 0;
    uint64_t end = bm_u32(d + 8);
    if (end > n) return 0;
    int t = 0;
    for (uint64_t o = 0x28; o + 8 <= end; o += 8) {
        uint32_t h = bm_u32(d + o), v = bm_u32(d + o + 4);
        if (!h) t = (int)v;
        else if (t == type && h == hash) {
            if (value) *value = v;
            return o;
        }
    }
    return 0;
}

// The file menu's date of a card (caption.sav) as one number that grows with it, 0 without one.
static inline uint64_t bm_caption_date(const uint8_t *d, size_t n) {
    static const char *const parts[6] = {"CaptionData.Date.Year", "CaptionData.Date.Month", "CaptionData.Date.Day",
                                         "CaptionData.Date.Hour", "CaptionData.Date.Minute", "CaptionData.Date.Second"};
    static const uint32_t room[6] = {10000, 13, 32, 24, 60, 60};
    uint64_t date = 0;
    for (int k = 0; k < 6; k++) {
        uint32_t v = 0;
        if (!bm_save_find(d, n, BM_INT, bm_hash(parts[k]), &v) || v >= room[k]) return 0;
        date = date * room[k] + v;
    }
    return date;
}

// What a boot map changes in the save it loads.
typedef struct {
    const char *area;        // Sequence_CurrentBanc
    float pos[3], yaw;       // PlayerStatus.SavePos, PlayerStatus.SavePosRadY
    int child;               // Sequence_IsChildhood: 1 child, 0 adult, -1 the save's
    int day_seconds;         // the clock's seconds of the day (UInt64 ef10b80f, low word), -1 the save's
} BmPlace;

// The edits that make the save s (n bytes) start at place p, at most 8. Their number; 0 with why when the save lacks
// one of the values. DungeonRoom, the save's room of a dungeon, becomes -1 (none) whatever the area: Link starts at
// another place than the save's, in another room or another area.
static inline int bm_save_edits(const uint8_t *s, size_t n, const BmPlace *p, BmEdit *e, char *why, size_t cap) {
    uint32_t v = 0;
    int k = 0;
    uint64_t at = bm_save_find(s, n, BM_STR64, bm_hash("Sequence_CurrentBanc"), &v);
    if (!at || (uint64_t)v + 64 > n || strlen(p->area) > 63) return bm_why(why, cap, "no Sequence_CurrentBanc"), 0;
    e[k].off = v, e[k].n = 64, memcpy(e[k].orig, s + v, 64), memset(e[k].repl, 0, 64), memcpy(e[k].repl, p->area, strlen(p->area));
    k++;
    at = bm_save_find(s, n, BM_VEC3, bm_hash("PlayerStatus.SavePos"), &v);
    if (!at || (uint64_t)v + 12 > n) return bm_why(why, cap, "no PlayerStatus.SavePos"), 0;
    e[k].off = v, e[k].n = 12, memcpy(e[k].orig, s + v, 12), memcpy(e[k].repl, p->pos, 12);
    k++;
    at = bm_save_find(s, n, BM_FLOAT, bm_hash("PlayerStatus.SavePosRadY"), &v);
    if (!at) return bm_why(why, cap, "no PlayerStatus.SavePosRadY"), 0;
    e[k].off = at + 4, e[k].n = 4, memcpy(e[k].orig, s + at + 4, 4), memcpy(e[k].repl, &p->yaw, 4);
    k++;
    if ((at = bm_save_find(s, n, BM_INT, bm_hash("DungeonRoom"), &v))) {
        int32_t none = -1;
        e[k].off = at + 4, e[k].n = 4, memcpy(e[k].orig, s + at + 4, 4), memcpy(e[k].repl, &none, 4);
        k++;
    }
    if (p->child >= 0) {
        at = bm_save_find(s, n, BM_BOOL, bm_hash("Sequence_IsChildhood"), &v);
        if (!at) return bm_why(why, cap, "no Sequence_IsChildhood"), 0;
        uint32_t b = p->child != 0;
        e[k].off = at + 4, e[k].n = 4, memcpy(e[k].orig, s + at + 4, 4), memcpy(e[k].repl, &b, 4);
        k++;
    }
    if (p->day_seconds >= 0) {
        at = bm_save_find(s, n, BM_UINT64, 0xef10b80fu, &v);   // the clock: {u32 seconds of the day, s32 day}; its name is not known
        if (!at || (uint64_t)v + 8 > n) return bm_why(why, cap, "no clock (UInt64 ef10b80f)"), 0;
        uint32_t secs = (uint32_t)p->day_seconds;
        e[k].off = v, e[k].n = 4, memcpy(e[k].orig, s + v, 4), memcpy(e[k].repl, &secs, 4);
        k++;
    }
    return k;
}

// Whether two lists of edits change the same bytes into the same values (only the bytes in use count: the rest of
// orig and repl holds nothing).
static inline int bm_edits_same(const BmEdit *a, int na, const BmEdit *b, int nb) {
    if (na != nb) return 0;
    for (int k = 0; k < na; k++)
        if (a[k].off != b[k].off || a[k].n != b[k].n || memcmp(a[k].repl, b[k].repl, a[k].n)) return 0;
    return 1;
}

// Applies edits to the n bytes read at offset off.
static inline void bm_edits_apply(const BmEdit *e, int count, uint64_t off, uint8_t *buf, uint64_t n) {
    for (int k = 0; k < count; k++) {
        uint64_t a = e[k].off, b = a + e[k].n;
        if (b <= off || a >= off + n) continue;
        uint64_t from = a > off ? a : off, to = b < off + n ? b : off + n;
        memcpy(buf + (from - off), e[k].repl + (from - a), (size_t)(to - from));
    }
}

// ---- the setting's values
// The heading a start position's Rot (radians about x, y, z; R = Rz Ry Rx) gives Link: atan2 of its forward vector's
// x and z, as SeqCalcSavePlayerForward turns a heading back into a forward vector. Rot (pi, t, pi) faces pi - t.
static inline float bm_yaw(const float r[3]) {
    double cx = cos(r[0]), sx = sin(r[0]), cy = cos(r[1]), sy = sin(r[1]), cz = cos(r[2]), sz = sin(r[2]);
    double fx = cz * sy * cx + sz * sx, fz = cy * cx;   // R * (0, 0, 1)
    return (float)atan2(fx, fz);
}

// "HH:MM" (24 hours) as seconds of the day; -1 when it is not one.
static inline int bm_day_seconds(const char *s) {
    int h = -1, m = -1, used = 0;
    if (!s || sscanf(s, "%2d:%2d%n", &h, &m, &used) != 2 || s[used] || h < 0 || h > 23 || m < 0 || m > 59) return -1;
    return h * 3600 + m * 60;
}

// The age an area's name gives Link (1 child, 0 adult), -1 for an area both ages use: the game's overworld areas end in
// _Child or _Adult (_Adult_Alternative too), and its castle grounds, HyruleCastle, are the child's.
static inline int bm_area_age(const char *area) {
    if (strstr(area, "_Child")) return 1;
    if (strstr(area, "_Adult")) return 0;
    return !strcmp(area, "HyruleCastle") ? 1 : -1;
}
