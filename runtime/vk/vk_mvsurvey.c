#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "vk.h"
#include "vk_exec.h"
#include "vk_dlaa.h"

#define BANKS 18
#define SNAP 256u
#define SURVEY_FRAMES 300
#define TABLE (1u << 15)

typedef struct {
    uint64_t addr;
    uint32_t size, n;
    int bank;
    uint8_t bytes[SNAP];
    uint32_t have;
    uint32_t mats;
} Block;

typedef struct {
    uint64_t draws, blocks, rep1, rep2, same, near, far, unread;
    uint32_t size[4];
    uint64_t size_n[4];
    uint64_t pairs, psame, pnear, pfar, unequal, mats[4];
    uint32_t mats_max;
    int64_t delta[4];
    uint64_t delta_n[4];
} Stat;

static struct {
    int on;
    Block *t[3];
    const Block **x, **y;
    uint32_t frames;
    Stat st[BANKS];
} S;

static uint32_t slot_of(uint64_t addr, int bank) {
    uint64_t h = (addr ^ (uint64_t)bank * 0x9E3779B97F4A7C15ull) * 0xff51afd7ed558ccdull;
    return (uint32_t)(h >> 40) & (TABLE - 1);
}

static Block *find(Block *t, uint64_t addr, int bank, int add) {
    for (uint32_t i = slot_of(addr, bank), k = 0; k < TABLE; i = (i + 1) & (TABLE - 1), k++) {
        if (t[i].addr == addr && t[i].bank == bank) return &t[i];
        if (!t[i].addr) {
            if (!add) return NULL;
            t[i].addr = addr, t[i].bank = bank;
            return &t[i];
        }
    }
    return NULL;
}

static void note_size(Stat *s, uint32_t size) {
    for (int i = 0; i < 4; i++)
        if (s->size[i] == size || !s->size_n[i]) {
            s->size[i] = size, s->size_n[i]++;
            return;
        }
    s->size_n[3]++;
}

static void note_delta(Stat *s, int64_t d) {
    for (int i = 0; i < 4; i++)
        if (s->delta[i] == d || !s->delta_n[i]) {
            s->delta[i] = d, s->delta_n[i]++;
            return;
        }
    s->delta_n[3]++;
}

static uint32_t lead_mats(const float *f, uint32_t nf) {
    uint32_t k = 0;
    for (; (k + 1) * 12 <= nf && k < 4096; k++) {
        const float *m = f + k * 12;
        float l[3];
        for (int r = 0; r < 3; r++) {
            const float *v = m + r * 4;
            if (!isfinite(v[0]) || !isfinite(v[1]) || !isfinite(v[2]) || !isfinite(v[3])) return k;
            l[r] = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            if (l[r] < 1e-3f || l[r] > 1e3f) return k;
        }
        if (fabsf(l[1] - l[0]) > 0.02f * l[0] || fabsf(l[2] - l[0]) > 0.02f * l[0]) return k;
        for (int a = 0; a < 3; a++)
            for (int b = a + 1; b < 3; b++) {
                const float *u = m + a * 4, *v = m + b * 4;
                if (fabsf(u[0] * v[0] + u[1] * v[1] + u[2] * v[2]) > 0.02f * l[a] * l[b]) return k;
            }
    }
    return k;
}

static int by_bank_addr(const void *a, const void *b) {
    const Block *x = *(const Block *const *)a, *y = *(const Block *const *)b;
    if (x->bank != y->bank) return x->bank < y->bank ? -1 : 1;
    return x->addr < y->addr ? -1 : x->addr > y->addr;
}

static void survey_init(void) {
    S.on = rt_knob("MV_SURVEY", "0") ? 1 : -1;
    if (S.on < 0) return;
    for (int i = 0; i < 3; i++) S.t[i] = calloc(TABLE, sizeof(Block));
    S.x = malloc(TABLE * sizeof *S.x), S.y = malloc(TABLE * sizeof *S.y);
    if (!S.t[0] || !S.t[1] || !S.t[2] || !S.x || !S.y) {
        S.on = -1;
        return;
    }
    rt_log("mv_survey: on (scene draws' uniform blocks, a summary every %d frames)", SURVEY_FRAMES);
}

void vk_mvsurvey_draw(VkExec *ex) {
    if (!S.on) survey_init();
    if (S.on < 0) return;
    const VkExecTarget *z = &ex->depth;
    if (!z->image || z->width < DLAA_SCENE_MIN_W || z->format != VK_FORMAT_D32_SFLOAT) return;
    for (int b = 0; b < BANKS; b++) {
        const p_bind_buf *u = &ex->ubo[0][b];
        if (!u->addr || !u->size) continue;
        S.st[b].draws++;
        Block *k = find(S.t[0], u->addr, b, 1);
        if (!k) continue;
        if (k->n++) continue;
        k->size = (uint32_t)u->size;
        uint32_t want = u->size < SNAP ? (uint32_t)u->size : SNAP;
        VkDescriptorBufferInfo bi;
        const uint8_t *src = vk_usnap_find(u->addr, want, 1, &bi) ? vk_usnap_map() + bi.offset : NULL;
        k->have = src ? want : 0;
        if (src) memcpy(k->bytes, src, want);
        const uint8_t *all = vk_usnap_find(u->addr, u->size, 1, &bi) ? vk_usnap_map() + bi.offset : NULL;
        k->mats = all ? lead_mats((const float *)all, (uint32_t)(u->size / 4)) : 0;
        if (all) {
            Stat *s = &S.st[b];
            s->mats[k->mats == 0 ? 0 : k->mats == 1 ? 1 : k->mats < 8 ? 2 : 3]++;
            if (k->mats > s->mats_max) s->mats_max = k->mats;
        }
    }
}

static int compare(const Block *a, const Block *b) {
    uint32_t n = a->have < b->have ? a->have : b->have;
    if (!n) return -1;
    if (!memcmp(a->bytes, b->bytes, n)) return 0;
    const float *x = (const float *)a->bytes, *y = (const float *)b->bytes;
    for (uint32_t i = 0; i < n / 4; i++) {
        float d = fabsf(x[i] - y[i]);
        if (isnan(d) || (d > 1.0f && d > 0.05f * fabsf(y[i]))) return 2;
    }
    return 1;
}

void vk_mvsurvey_frame(void) {
    if (S.on <= 0) return;
    for (uint32_t i = 0; i < TABLE; i++) {
        const Block *k = &S.t[0][i];
        if (!k->addr) continue;
        Stat *s = &S.st[k->bank];
        s->blocks++;
        note_size(s, k->size);
        const Block *p = find(S.t[1], k->addr, k->bank, 0);
        if (p) {
            s->rep1++;
            int c = compare(k, p);
            if (c == 0) s->same++;
            else if (c == 1) s->near++;
            else if (c == 2) s->far++;
            else s->unread++;
        }
        if (find(S.t[2], k->addr, k->bank, 0)) s->rep2++;
    }
    uint32_t nx = 0, ny = 0;
    for (uint32_t i = 0; i < TABLE; i++) {
        const Block *k = &S.t[0][i];
        if (k->addr && !find(S.t[1], k->addr, k->bank, 0) && find(S.t[2], k->addr, k->bank, 0)) S.x[nx++] = k;
        k = &S.t[1][i];
        if (k->addr && !find(S.t[0], k->addr, k->bank, 0)) S.y[ny++] = k;
    }
    qsort(S.x, nx, sizeof *S.x, by_bank_addr);
    qsort(S.y, ny, sizeof *S.y, by_bank_addr);
    for (uint32_t i = 0, j = 0; i < nx;) {
        int b = S.x[i]->bank;
        uint32_t i1 = i, j0, j1;
        while (i1 < nx && S.x[i1]->bank == b) i1++;
        while (j < ny && S.y[j]->bank < b) j++;
        for (j0 = j1 = j; j1 < ny && S.y[j1]->bank == b;) j1++;
        Stat *s = &S.st[b];
        if (i1 - i != j1 - j0) s->unequal++;
        else
            for (uint32_t a = i, c = j0; a < i1; a++, c++) {
                s->pairs++;
                note_delta(s, (int64_t)(S.x[a]->addr - S.y[c]->addr));
                int r = compare(S.x[a], S.y[c]);
                if (r == 0) s->psame++;
                else if (r == 1) s->pnear++;
                else if (r == 2) s->pfar++;
            }
        i = i1, j = j1;
    }
    Block *old = S.t[2];
    S.t[2] = S.t[1], S.t[1] = S.t[0], S.t[0] = old;
    memset(S.t[0], 0, TABLE * sizeof(Block));
    if (++S.frames % SURVEY_FRAMES) return;
    for (int b = 0; b < BANKS; b++) {
        Stat *s = &S.st[b];
        if (!s->blocks) continue;
        double bl = (double)s->blocks, r1 = s->rep1 ? (double)s->rep1 : 1;
        rt_log("mv_survey: c%d: %llu draws, %llu blocks (%.0f a frame), sizes %#x x%llu %#x x%llu %#x x%llu; repeat1 %.1f %%, "
               "repeat2 %.1f %%; same %.1f %%, near %.1f %%, far %.1f %%, unread %.1f %%",
               b + 3, (unsigned long long)s->draws, (unsigned long long)s->blocks, bl / SURVEY_FRAMES, s->size[0],
               (unsigned long long)s->size_n[0], s->size[1], (unsigned long long)s->size_n[1], s->size[2],
               (unsigned long long)s->size_n[2], 100 * s->rep1 / bl, 100 * s->rep2 / bl, 100 * s->same / r1,
               100 * s->near / r1, 100 * s->far / r1, 100 * s->unread / r1);
        double pr = s->pairs ? (double)s->pairs : 1;
        rt_log("mv_survey: c%d: pairs %llu (%llu frames unequal), delta %#llx x%llu %#llx x%llu %#llx x%llu; psame %.1f %%, "
               "pnear %.1f %%, pfar %.1f %%; mats 0 x%llu 1 x%llu 2-7 x%llu 8+ x%llu, at most %u",
               b + 3, (unsigned long long)s->pairs, (unsigned long long)s->unequal, (unsigned long long)s->delta[0],
               (unsigned long long)s->delta_n[0], (unsigned long long)s->delta[1], (unsigned long long)s->delta_n[1],
               (unsigned long long)s->delta[2], (unsigned long long)s->delta_n[2], 100 * s->psame / pr,
               100 * s->pnear / pr, 100 * s->pfar / pr, (unsigned long long)s->mats[0], (unsigned long long)s->mats[1],
               (unsigned long long)s->mats[2], (unsigned long long)s->mats[3], s->mats_max);
    }
    memset(S.st, 0, sizeof S.st);
}
