#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "platform/platform.h"
#include "vk.h"
#include "vk_exec.h"
#include "vk_dlaa.h"

#define CHECK 128u
#define TABLE (1u << 14)
#define SUMMARY 600

enum { BONES = 2, PLACE = 4, CLOCK = 11, BANKS = 14 };

typedef struct {
    uint64_t addr;
    int index;
    uint32_t have;
} Seen;

typedef struct { uint64_t blocks, self, side, nearer, none, checked, intact, still, small, big; } Stat;

static struct {
    int on;
    int knob;
    Seen *t[2];
    uint8_t (*bytes[2])[CHECK];
    int timed;
    uint32_t frames;
    Stat st[BANKS];
    uint64_t last[BANKS];
    uint32_t mask;
    uint64_t ns_draw, ns_frame;
} P = {.knob = -1, .mask = 1u << BONES | 1u << PLACE | 1u << CLOCK};

static uint32_t slot_of(uint64_t addr, int index) {
    uint64_t h = (addr ^ (uint64_t)index * 0x9E3779B97F4A7C15ull) * 0xff51afd7ed558ccdull;
    return (uint32_t)(h >> 40) & (TABLE - 1);
}

static Seen *find(Seen *t, uint64_t addr, int index, int add) {
    for (uint32_t i = slot_of(addr, index), k = 0; k < TABLE; i = (i + 1) & (TABLE - 1), k++) {
        if (t[i].addr == addr && t[i].index == index) return &t[i];
        if (!t[i].addr) {
            if (!add) return NULL;
            t[i].addr = addr, t[i].index = index;
            return &t[i];
        }
    }
    return NULL;
}

int vk_mvprev_on(void) { return P.on > 0; }

void vk_mvprev_want(uint32_t index_mask) {
    static int fixed = -1;
    if (fixed < 0) fixed = !rt_knob("MVPREV_GROW", "1");
    if (!fixed) __atomic_fetch_or(&P.mask, index_mask & ((1u << BANKS) - 1), __ATOMIC_RELAXED);
}

uint64_t vk_mvprev_step(int stage, int index, uint64_t size) {
    if (P.on <= 0 || stage != 0 || index < 0 || index >= BANKS || !(P.mask >> index & 1)) return 0;
    return (size + 255) & ~(uint64_t)255;
}

static float moved(const uint8_t *a, const uint8_t *b) {
    const float *x = (const float *)a, *y = (const float *)b;
    float dx = x[3] - y[3], dy = x[7] - y[7], dz = x[11] - y[11];
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

int vk_mvprev_find(int index, uint64_t addr, uint64_t size, uint64_t *prev, int *how) {
    *how = MVPREV_NONE;
    if (P.on <= 0 || !P.t[1]) return 0;
    if (find(P.t[1], addr, index, 0)) {
        *prev = addr, *how = MVPREV_SELF;
        return 1;
    }
    uint64_t step = (size + 255) & ~(uint64_t)255, cand[2];
    int n = 0;
    if (find(P.t[1], addr + step, index, 0)) cand[n++] = addr + step;
    if (addr >= step && find(P.t[1], addr - step, index, 0)) cand[n++] = addr - step;
    if (!n) return 0;
    if (n == 2) {
        const uint32_t *c = (const uint32_t *)vk_usnap_bytes(addr, 64), *a = (const uint32_t *)vk_usnap_bytes(cand[0], 64),
                       *b = (const uint32_t *)vk_usnap_bytes(cand[1], 64);
        if (!c || !a || !b) return 0;
        int na = 0, nb = 0;
        for (int w = 0; w < 16; w++) na += c[w] == a[w], nb += c[w] == b[w];
        if (nb > na) cand[0] = cand[1];
        *how = MVPREV_NEARER;
    } else {
        *how = MVPREV_SIDE;
    }
    *prev = cand[0];
    return 1;
}

static void mvprev_draw_(VkExec *ex);
void vk_mvprev_draw(VkExec *ex) {
    if (P.on <= 0) return;
    if (!P.timed) return mvprev_draw_(ex);
    uint64_t t0 = kern_now_ns();
    mvprev_draw_(ex);
    P.ns_draw += kern_now_ns() - t0;
}

static void mvprev_draw_locked(VkExec *ex);
static PlatMutex draw_lock = PLAT_MUTEX_INIT;

static void mvprev_draw_(VkExec *ex) {
    const VkExecTarget *z = &ex->depth;
    if (!z->image || z->width < DLAA_SCENE_MIN_W || z->format != VK_FORMAT_D32_SFLOAT) return;
    int lk = vk_exec_shared();
    if (lk) plat_mutex_lock(&draw_lock);
    mvprev_draw_locked(ex);
    if (lk) plat_mutex_unlock(&draw_lock);
}

static void mvprev_draw_locked(VkExec *ex) {
    int check = P.bytes[0] != NULL;
    for (int index = 0; index < BANKS; index++) {
        if (!(P.mask >> index & 1)) continue;
        const p_bind_buf *u = &ex->ubo[0][index];
        if (!u->addr || !u->size || u->addr == P.last[index]) continue;
        P.last[index] = u->addr;
        Seen *k = find(P.t[0], u->addr, index, 1);
        if (!k || k->have || !check) continue;
        int w = index;
        uint32_t want = u->size < CHECK ? (uint32_t)u->size : CHECK;
        const uint8_t *cur = vk_usnap_bytes(u->addr, want);
        if (!cur) continue;
        memcpy(P.bytes[0][k - P.t[0]], cur, want), k->have = want;
        Stat *s = &P.st[w];
        s->blocks++;
        uint64_t prev;
        int how;
        if (!vk_mvprev_find(index, u->addr, u->size, &prev, &how)) {
            s->none++;
            continue;
        }
        if (how == MVPREV_SELF) s->self++;
        else if (how == MVPREV_SIDE) s->side++;
        else s->nearer++;
        const Seen *last = find(P.t[1], prev, index, 0);
        const uint8_t *now = vk_usnap_bytes(prev, want);
        if (last && now && last->have >= want && want >= 48) {
            s->checked++;
            if (!memcmp(now, P.bytes[1][last - P.t[1]], want)) s->intact++;
            float d = moved(cur, now);
            if (d < 0.01f) s->still++;
            else if (d < 1.0f) s->small++;
            else s->big++;
        }
    }
}

void vk_mvprev_frame(void) {
    if (!vk_dlaa_available() && !vk_dlaa_fg_only()) return;
    if (P.knob < 0) {
        P.knob = rt_knob("DLAA_OBJMV", "1");
        if (P.knob) {
            P.t[0] = calloc(TABLE, sizeof(Seen)), P.t[1] = calloc(TABLE, sizeof(Seen));
            if (!P.t[0] || !P.t[1]) P.knob = 0;
            if (P.knob && rt_knob("MVPREV_CHECK", "0")) {
                P.bytes[0] = calloc(TABLE, CHECK), P.bytes[1] = calloc(TABLE, CHECK);
                if (!P.bytes[0] || !P.bytes[1]) free(P.bytes[0]), free(P.bytes[1]), P.bytes[0] = P.bytes[1] = NULL;
            }
            P.timed = rt_knob("MVOBJ_TIMES", "0");
        }
    }
    int on = P.knob && (cfg_bool("NX2SARIA_DLAA") || vk_dlaa_fg_only());
    if (on != (P.on > 0)) {
        P.on = on ? 1 : -1;
        if (P.t[0]) memset(P.t[0], 0, TABLE * sizeof(Seen)), memset(P.t[1], 0, TABLE * sizeof(Seen));
        return;
    }
    if (P.on <= 0) return;
    uint64_t t0 = P.timed ? kern_now_ns() : 0;
    Seen *t = P.t[1];
    P.t[1] = P.t[0], P.t[0] = t;
    uint8_t (*b)[CHECK] = P.bytes[1];
    P.bytes[1] = P.bytes[0], P.bytes[0] = b;
    memset(P.t[0], 0, TABLE * sizeof(Seen));
    memset(P.last, 0, sizeof P.last);
    if (P.timed) P.ns_frame += kern_now_ns() - t0;
    if (++P.frames % SUMMARY) return;
    if (P.timed) rt_log("mv_prev: %.3f ms a frame in the draws, %.3f ms at the present", P.ns_draw / 1e6 / SUMMARY,
           P.ns_frame / 1e6 / SUMMARY);
    P.ns_draw = P.ns_frame = 0;
    for (int w = 0; w < BANKS; w++) {
        Stat *s = &P.st[w];
        if (!s->blocks) continue;
        double b = (double)s->blocks, c = s->checked ? (double)s->checked : 1;
        rt_log("mv_prev: c%d: %llu blocks: self %.1f %%, side %.1f %%, nearer %.1f %%, none %.1f %%; intact %.1f %% of "
               "%llu; moved: still %.1f %%, under one unit %.1f %%, more %.1f %%",
               w + 3, (unsigned long long)s->blocks, 100 * s->self / b, 100 * s->side / b, 100 * s->nearer / b,
               100 * s->none / b, 100 * s->intact / c, (unsigned long long)s->checked, 100 * s->still / c,
               100 * s->small / c, 100 * s->big / c);
    }
    memset(P.st, 0, sizeof P.st);
}
