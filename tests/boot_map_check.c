// runtime/patches/boot_map_core.h: the byte changes of a boot map, on made-up files. Built and run by tests/boot_map_check.py;
// prints each failed check and exits 1 on any failure.
//
// The title's graph gets its command Root moved to the Sequential of a Simultaneous and TitleToMainWithLoadSave.module,
// and refuses a graph without exactly that; the slot's graph gets both Index inputs made the slot; a save gets the
// area, position, heading, dungeon room, age and clock, whichever way its reads are cut; the file menu's dates order
// the slots; the heading, the time and the age of an area name read as the game has them.
#include "boot_map_core.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void put16(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); }

// ---- a made-up AINB 0x408: one command, nodes with names, children and int inputs
typedef struct {
    int type;
    const char *name;
    int children[4], nchild;
    struct { const char *name; int16_t from; uint32_t flags; int32_t value; } in[3];
    int nin;
} TNode;

static uint8_t g[8192];
static size_t g_n;

static uint32_t pool_add(char *pool, uint32_t *pn, const char *s) {
    for (uint32_t o = 0; o < *pn; o += (uint32_t)strlen(pool + o) + 1)
        if (!strcmp(pool + o, s)) return o;
    uint32_t at = *pn;
    strcpy(pool + at, s);
    *pn += (uint32_t)strlen(s) + 1;
    return at;
}

// Lays out the graph: header, the command, the nodes, their bodies and links, the int inputs, the pool.
static void build(const TNode *t, int n, int root, const char *cmd, uint32_t version) {
    char pool[2048];
    uint32_t pn = 0;
    memset(g, 0, sizeof g);
    pool_add(pool, &pn, "");
    memcpy(g, "AIB ", 4);
    put32(g + 4, version);
    put32(g + 0xc, 1), put32(g + 0x10, (uint32_t)n);
    put32(g + 0x74, pool_add(pool, &pn, cmd));
    put16(g + 0x74 + 0x14, (uint16_t)root);
    size_t nodes = 0x8c, at = nodes + 0x3c * (size_t)n, inputs_total = 0;
    for (int i = 0; i < n; i++) inputs_total += (size_t)t[i].nin;
    size_t bodies = at;
    for (int i = 0; i < n; i++) at += 0xa4 + 4 * (size_t)t[i].nchild + 4 * (size_t)t[i].nchild;   // body, offsets, links
    size_t io = at, in_at = io + 48;
    put32(g + 0x34, (uint32_t)io);
    put32(g + io, (uint32_t)in_at), put32(g + io + 4, (uint32_t)(in_at + 0x10 * inputs_total));
    size_t b = bodies;
    int in_first = 0;
    for (int i = 0; i < n; i++) {
        uint8_t *nd = g + nodes + 0x3c * (size_t)i;
        put16(nd, (uint16_t)t[i].type);
        put32(nd + 8, pool_add(pool, &pn, t[i].name));
        put32(nd + 0x14, (uint32_t)b);
        put32(g + b + 0x30, (uint32_t)in_first), put32(g + b + 0x34, (uint32_t)t[i].nin);
        g[b + 0x90 + 4] = (uint8_t)t[i].nchild, g[b + 0x90 + 5] = 0;   // kind 2: count, first
        for (int j = 0; j < t[i].nchild; j++) {
            size_t link = b + 0xa4 + 4 * (size_t)t[i].nchild + 4 * (size_t)j;
            put32(g + b + 0xa4 + 4 * j, (uint32_t)link);
            put32(g + link, (uint32_t)t[i].children[j]);
        }
        for (int j = 0; j < t[i].nin; j++) {
            uint8_t *e = g + in_at + 0x10 * (size_t)(in_first + j);
            put32(e, pool_add(pool, &pn, t[i].in[j].name));
            memcpy(e + 4, &t[i].in[j].from, 2);
            put32(e + 8, t[i].in[j].flags);
            memcpy(e + 12, &t[i].in[j].value, 4);
        }
        in_first += t[i].nin;
        b += 0xa4 + 8 * (size_t)t[i].nchild;
    }
    size_t sp = in_at + 0x10 * inputs_total;
    put32(g + 0x24, (uint32_t)sp);
    memcpy(g + sp, pool, pn);
    g_n = sp + pn;
}

static void title_checks(void) {
    TNode t[5] = {
        {0, "TitleToMainWithLoadSave.module", {0}, 0, {{0}}, 0},
        {3, "", {0}, 0, {{0}}, 0},           // what Root starts: the title
        {3, "", {0}, 0, {{0}}, 0},           // the scene transition's pause
        {2, "", {2, 0}, 2, {{0}}, 0},        // Continue, Yes
        {2, "", {1}, 1, {{0}}, 0},           // another Sequential
    };
    BmEdit e;
    char why[160] = "";
    build(t, 5, 1, "Root", 0x408);
    CHECK(bm_ainb_title_edit(g, g_n, &e, why, sizeof why), "the title's graph: %s", why);
    CHECK(e.off == 0x88 && e.n == 2 && bm_u16(e.orig) == 1 && bm_u16(e.repl) == 3, "Root 1 -> 3 at 0x88: %llx %u %u",
          (unsigned long long)e.off, bm_u16(e.orig), bm_u16(e.repl));
    build(t, 5, 1, "Root", 0x407);
    CHECK(!bm_ainb_title_edit(g, g_n, &e, why, sizeof why), "another AINB version is refused");
    build(t, 5, 3, "Root", 0x408);
    CHECK(!bm_ainb_title_edit(g, g_n, &e, why, sizeof why), "a Root that does not start a Simultaneous is refused");
    build(t, 5, 1, "Other", 0x408);
    CHECK(!bm_ainb_title_edit(g, g_n, &e, why, sizeof why), "no command Root is refused");
    TNode two[6];
    memcpy(two, t, sizeof t);
    two[5] = (TNode){2, "", {2, 0}, 2, {{0}}, 0};
    build(two, 6, 1, "Root", 0x408);
    CHECK(!bm_ainb_title_edit(g, g_n, &e, why, sizeof why), "two Sequentials of the load are refused");
    t[3].children[0] = 4;   // its first child no Simultaneous
    build(t, 5, 1, "Root", 0x408);
    CHECK(!bm_ainb_title_edit(g, g_n, &e, why, sizeof why), "a Sequential whose first child is no Simultaneous is not taken");
    build(t, 5, 1, "Root", 0x408);
    CHECK(!bm_ainb_title_edit(g, g_n - 200 > 0x74 ? 0x80 : g_n, &e, why, sizeof why), "a cut file is refused");
}

static void slot_checks(void) {
    TNode t[3] = {
        {0, "SeqExecuteLoadSaveData", {0}, 0, {{"Index", 1, 0, 0}, {"LoadType", -1, 0x800000, 0}}, 2},
        {0, "SeqQueryGetGameDataInt", {0}, 0, {{0}}, 0},
        {0, "SeqOneShotSetCaptionMgrLatestIndex", {0}, 0, {{"Index", 1, 0, 0}}, 1},
    };
    BmEdit e[2];
    char why[160] = "";
    build(t, 3, 1, "Root", 0x408);
    CHECK(bm_ainb_slot_edits(g, g_n, 4, e, why, sizeof why), "the slot's graph: %s", why);
    for (int k = 0; k < 2; k++) {
        uint8_t copy[8192];
        memcpy(copy, g, g_n);
        bm_edits_apply(&e[k], 1, 0, copy, g_n);
        int16_t from;
        uint32_t flags;
        int32_t value;
        memcpy(&from, copy + e[k].off, 2), memcpy(&flags, copy + e[k].off + 4, 4), memcpy(&value, copy + e[k].off + 8, 4);
        CHECK(e[k].n == 12 && from == -1 && flags == 0x800000 && value == 4, "Index %d: from %d flags %#x value %d", k, from, flags, value);
        int16_t was;
        memcpy(&was, e[k].orig, 2);
        CHECK(was == 1, "Index %d was linked to node 1: %d", k, was);
    }
    t[2].in[0].from = -1, t[2].in[0].flags = 0x800000;
    build(t, 3, 1, "Root", 0x408);
    CHECK(!bm_ainb_slot_edits(g, g_n, 4, e, why, sizeof why), "an Index that is already a constant is refused");
    t[2].in[0] = (typeof(t[2].in[0])){"Slot", 1, 0, 0};
    build(t, 3, 1, "Root", 0x408);
    CHECK(!bm_ainb_slot_edits(g, g_n, 4, e, why, sizeof why), "a node without Index is refused");
    t[2].name = "Other";
    build(t, 3, 1, "Root", 0x408);
    CHECK(!bm_ainb_slot_edits(g, g_n, 4, e, why, sizeof why), "a missing node is refused");
}

// ---- a made-up save: the header, the pairs by type, the data area
static size_t make_save(uint8_t *s, const char *area, int with_clock) {
    memset(s, 0, 1024);
    put32(s, 0x01020304), put32(s + 4, 0x2d80eb);
    uint32_t o = 0x28, data = 0x100;
    float yaw = 1.5f, pos[3] = {10, 20, 30};
#define PAIR(h, v) (put32(s + o, h), put32(s + o + 4, v), o += 8)
    PAIR(bm_hash("Sequence_IsChildhood"), 0);
    PAIR(0, BM_INT), PAIR(bm_hash("DungeonRoom"), 22);
    uint32_t yb;
    memcpy(&yb, &yaw, 4);
    PAIR(0, BM_FLOAT), PAIR(bm_hash("PlayerStatus.SavePosRadY"), yb);
    PAIR(0, BM_VEC3), PAIR(bm_hash("PlayerStatus.SavePos"), data), memcpy(s + data, pos, 12);
    PAIR(0, BM_STR64), PAIR(bm_hash("Sequence_CurrentBanc"), data + 16), strcpy((char *)s + data + 16, area);
    if (with_clock) PAIR(0, BM_UINT64), PAIR(0xef10b80fu, data + 80), put32(s + data + 80, 59062), put32(s + data + 84, 2);
#undef PAIR
    put32(s + 8, o);
    return data + 96;
}

static void save_checks(void) {
    CHECK(bm_hash("Sequence_CurrentBanc") == 0x1d6189da && bm_hash("PlayerStatus.SavePos") == 0xc884818d &&
              bm_hash("Sequence_IsChildhood") == 0xcea220d3 && bm_hash("PlayerStatus.SavePosRadY") == 0x1ea93bd8,
          "the game's name hashes");
    uint8_t s[1024], read[1024];
    size_t n = make_save(s, "WaterTemple", 1);
    BmPlace p = {"HyruleField_Child", {1.5f, 150.25f, -7}, -2.0f, 1, 6 * 3600 + 30 * 60};
    BmEdit e[8];
    char why[160] = "";
    int k = bm_save_edits(s, n, &p, e, why, sizeof why);
    CHECK(k == 6, "six edits: %d (%s)", k, why);
    // reads cut at every size from 1 to 64 bytes give the same file
    for (size_t step = 1; step <= 64; step++) {
        for (size_t at = 0; at < n; at += step) {
            size_t len = at + step < n ? step : n - at;
            memcpy(read + at, s + at, len);
            bm_edits_apply(e, k, at, read + at, len);
        }
        uint32_t v = 0, room = 0, child = 1, secs = 0;
        float pos[3], yaw;
        bm_save_find(read, n, BM_STR64, bm_hash("Sequence_CurrentBanc"), &v);
        int area_ok = !strcmp((const char *)read + v, "HyruleField_Child") && !read[v + 63];
        bm_save_find(read, n, BM_VEC3, bm_hash("PlayerStatus.SavePos"), &v);
        memcpy(pos, read + v, 12);
        uint64_t at = bm_save_find(read, n, BM_FLOAT, bm_hash("PlayerStatus.SavePosRadY"), NULL);
        memcpy(&yaw, read + at + 4, 4);
        bm_save_find(read, n, BM_INT, bm_hash("DungeonRoom"), &room);
        bm_save_find(read, n, BM_BOOL, bm_hash("Sequence_IsChildhood"), &child);
        bm_save_find(read, n, BM_UINT64, 0xef10b80fu, &v);
        secs = bm_u32(read + v);
        CHECK(area_ok && pos[0] == 1.5f && pos[1] == 150.25f && pos[2] == -7 && yaw == -2.0f && (int32_t)room == -1 &&
                  child == 1 && secs == 23400 && bm_u32(read + v + 4) == 2,
              "reads of %zu bytes: area %d pos %g %g %g yaw %g room %d child %u clock %u day %u", step, area_ok, pos[0],
              pos[1], pos[2], yaw, (int32_t)room, child, secs, bm_u32(read + v + 4));
    }
    // the same file gives the same edits, whatever the unused bytes of the lists hold; another place does not
    {
        BmEdit a[8], b[8];
        memset(a, 0x11, sizeof a), memset(b, 0x77, sizeof b);
        int na = bm_save_edits(s, n, &p, a, why, sizeof why), nb = bm_save_edits(s, n, &p, b, why, sizeof why);
        CHECK(na == 6 && bm_edits_same(a, na, b, nb), "a second open of the same save matches the first");
        BmPlace q = p;
        q.pos[0] += 1;
        nb = bm_save_edits(s, n, &q, b, why, sizeof why);
        CHECK(!bm_edits_same(a, na, b, nb), "another position does not match");
    }
    // the save's own area, no age and no time asked: the area, position, heading and dungeon room
    p.area = "WaterTemple", p.child = -1, p.day_seconds = -1;
    k = bm_save_edits(s, n, &p, e, why, sizeof why);
    CHECK(k == 4, "the save's area, nothing else: %d", k);
    // a save without the clock refuses a time, and is fine without one
    n = make_save(s, "WaterTemple", 0);
    p.day_seconds = 3600;
    CHECK(!bm_save_edits(s, n, &p, e, why, sizeof why), "a time without a clock in the save is refused");
    p.day_seconds = -1;
    CHECK(bm_save_edits(s, n, &p, e, why, sizeof why) == 4, "no time asked, no clock needed");
    put32(s, 0x04030201);
    CHECK(!bm_save_edits(s, n, &p, e, why, sizeof why), "another file kind is refused");
}

static size_t make_caption(uint8_t *s, int y, int mo, int d, int h, int mi, int se) {
    memset(s, 0, 256);
    put32(s, 0x01020304);
    uint32_t o = 0x28;
    put32(s + o, 0), put32(s + o + 4, BM_INT), o += 8;
    const char *names[6] = {"CaptionData.Date.Year", "CaptionData.Date.Month", "CaptionData.Date.Day",
                            "CaptionData.Date.Hour", "CaptionData.Date.Minute", "CaptionData.Date.Second"};
    int v[6] = {y, mo, d, h, mi, se};
    for (int k = 5; k >= 0; k--) put32(s + o, bm_hash(names[k])), put32(s + o + 4, (uint32_t)v[k]), o += 8;   // any order
    put32(s + 8, o);
    return o;
}

static void caption_checks(void) {
    uint8_t a[256], b[256];
    size_t na = make_caption(a, 2026, 11, 16, 15, 58, 59), nb = make_caption(b, 2026, 11, 16, 15, 59, 0);
    uint64_t da = bm_caption_date(a, na), db = bm_caption_date(b, nb);
    CHECK(da && db && db > da, "a later second is a later card: %llu %llu", (unsigned long long)da, (unsigned long long)db);
    na = make_caption(a, 2027, 1, 1, 0, 0, 0);
    CHECK(bm_caption_date(a, na) > db, "a later year wins over every later field");
    na = make_caption(a, 2026, 13, 1, 0, 0, 0);
    CHECK(!bm_caption_date(a, na), "a month 13 is no date");
}

static void value_checks(void) {
    const float pi = 3.14159265f;
    float r1[3] = {pi, 1.24674094f, pi}, r2[3] = {0, -1.117f, 0}, r3[3] = {0, 0, 0};
    CHECK(fabsf(bm_yaw(r1) - (pi - 1.24674094f)) < 1e-4f, "Rot (pi, t, pi) faces pi - t: %f", bm_yaw(r1));
    CHECK(fabsf(bm_yaw(r2) + 1.117f) < 1e-4f, "Rot (0, t, 0) faces t: %f", bm_yaw(r2));
    CHECK(fabsf(bm_yaw(r3)) < 1e-6f, "no rotation faces 0");
    CHECK(bm_day_seconds("06:30") == 23400 && bm_day_seconds("00:00") == 0 && bm_day_seconds("23:59") == 86340, "HH:MM");
    CHECK(bm_day_seconds("24:00") < 0 && bm_day_seconds("6:3x") < 0 && bm_day_seconds("12:60") < 0 &&
              bm_day_seconds("save") < 0 && bm_day_seconds("") < 0,
          "not times");
    CHECK(bm_area_age("HyruleField_Child") == 1 && bm_area_age("KakarikoVillage_Adult_Alternative") == 0 &&
              bm_area_age("HyruleCastle") == 1 && bm_area_age("InsideTheDekuTree") == -1 && bm_area_age("ForestTemple") == -1,
          "ages of area names");
}

int main(void) {
    title_checks();
    slot_checks();
    save_checks();
    caption_checks();
    value_checks();
    if (!fails) printf("boot_map_check: all checks passed\n");
    return fails != 0;
}
