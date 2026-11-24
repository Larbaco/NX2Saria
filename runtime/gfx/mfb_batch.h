#pragma once

#include <stdint.h>

#define MFB_MIN_SLOTS 64u
#define MFB_MIN_TASKS 16u
#define MFB_MIN_SEEN 8u
#define MFB_MIN_CALM 32u
#define MFB_READMIT 256u
#define MFB_MAX_SHORTS 3u
#define MFB_HEADROOM 4u
#define MFB_FLOOR_DIV 32u

enum {
    MFB_BATCHED,
    MFB_ONE_OFF,
    MFB_ONE_WRITES,
    MFB_ONE_HISTORY,
    MFB_ONE_EXCLUDED,
    MFB_ONE_ROOM,
    MFB_ONE_PRE,
    MFB_ONE_TEXTURES,
    MFB_ONE_LOADING,
    MFB_REASONS
};

typedef struct {
    uint32_t slots, tasks;
    uint32_t seen;
    uint32_t excluded;
    uint32_t calm;
    uint32_t shorts;
} MfbHist;

static inline uint32_t mfb_floor(uint64_t cap, uint64_t unit, uint32_t least) {
    uint64_t n = unit ? cap / MFB_FLOOR_DIV / unit : 0;
    if (n < least) n = least;
    return n > 0x7fffffffu ? 0x7fffffffu : (uint32_t)n;
}

static inline uint32_t mfb_twice(uint32_t v, uint32_t least) {
    uint64_t t = (uint64_t)MFB_HEADROOM * v;
    if (t < least) t = least;
    return t > 0x7fffffffu ? 0x7fffffffu : (uint32_t)t;
}

static inline int mfb_batch_need(int on, int writes, int direct, uint32_t count, int has_task, const MfbHist *h,
                                 uint32_t floor_slots, uint32_t floor_tasks, uint32_t *slots, uint32_t *tasks) {
    *slots = *tasks = 0;
    if (!on) return MFB_ONE_OFF;
    if (writes) return MFB_ONE_WRITES;
    if (h->excluded) return MFB_ONE_EXCLUDED;
    if (!has_task) {
        if (direct) {
            *slots = count;
            return MFB_BATCHED;
        }
        if (h->seen < MFB_MIN_SEEN || h->calm < MFB_MIN_CALM) return MFB_ONE_HISTORY;
        *slots = mfb_twice(h->slots, floor_slots);
        return MFB_BATCHED;
    }
    if (h->seen < MFB_MIN_SEEN || h->calm < MFB_MIN_CALM) return MFB_ONE_HISTORY;
    *tasks = direct ? count : mfb_twice(h->tasks, floor_tasks);
    *slots = mfb_twice(h->slots, floor_slots);
    return MFB_BATCHED;
}

static inline void mfb_part_bytes(uint32_t slots, uint32_t tasks, int has_task, uint32_t task_words, uint32_t slot_words,
                                  uint64_t *task_b, uint64_t *list_b, uint64_t *mesh_b) {
    *task_b = has_task ? (uint64_t)tasks * task_words * 4 : 0;
    *list_b = has_task ? (uint64_t)slots * 4 : 0;
    *mesh_b = (uint64_t)slots * slot_words * 4;
}

typedef struct { uint64_t task, list, mesh; } MfbSpan;
typedef struct { uint64_t task_off, task_len, list_off, list_len, mesh_off, mesh_len; } MfbPart;

static inline int mfb_need_cut(int in_pass, uint32_t pre_pass, uint32_t pass_serial) {
    return !in_pass || pre_pass != pass_serial;
}

static inline int mfb_parts_taken(const MfbSpan *u) { return u->task || u->list || u->mesh; }

static inline uint64_t mfb_align_up(uint64_t v, uint64_t a) { return a > 1 ? (v + a - 1) / a * a : v; }

static inline int mfb_part_take(MfbSpan *used, const MfbSpan *cap, uint64_t align, uint64_t task_b, uint64_t list_b,
                                uint64_t mesh_b, MfbPart *out) {
    uint64_t t = mfb_align_up(task_b ? task_b : 4, align), l = mfb_align_up(list_b ? list_b : 4, align),
             m = mfb_align_up(mesh_b ? mesh_b : 4, align);
    uint64_t to = mfb_align_up(used->task, align), lo = mfb_align_up(used->list, align),
             mo = mfb_align_up(used->mesh, align);
    if (to + t > cap->task || lo + l > cap->list || mo + m > cap->mesh) return 0;
    *out = (MfbPart){to, t, lo, l, mo, m};
    used->task = to + t, used->list = lo + l, used->mesh = mo + m;
    return 1;
}

enum { MFB_NOTE_SHORT = 1, MFB_NOTE_READMIT = 2 };

static inline int mfb_hist_note(MfbHist *h, uint32_t want_slots, uint32_t want_tasks, uint32_t cap_slots,
                                uint32_t cap_tasks, int batched) {
    uint32_t hs = batched || want_slots <= cap_slots ? want_slots : cap_slots;
    uint32_t ht = batched || want_tasks <= cap_tasks ? want_tasks : cap_tasks;
    int jump = !h->seen || (uint64_t)hs > 2ull * h->slots || (uint64_t)ht > 2ull * h->tasks;
    if (hs > h->slots) h->slots = hs;
    if (ht > h->tasks) h->tasks = ht;
    h->seen++;
    h->calm = jump ? 0 : h->calm + 1;
    if (batched && (want_slots > cap_slots || want_tasks > cap_tasks)) {
        if (!h->excluded) h->shorts++;
        h->excluded = 1, h->calm = 0;
        return MFB_NOTE_SHORT;
    }
    if (h->excluded && h->shorts < MFB_MAX_SHORTS && h->calm >= MFB_READMIT) {
        h->excluded = 0;
        return MFB_NOTE_READMIT;
    }
    return 0;
}

typedef struct {
    int (*cut)(void *u);
    int (*pre)(void *u);
    int (*take)(void *u);
    void *u;
} MfbSteps;
enum { MFB_CUT_NONE, MFB_CUT_PASS, MFB_CUT_ROOM };

static inline int mfb_batch_steps(int why, int images, int need_cut, const MfbSteps *s, int *cut) {
    *cut = MFB_CUT_NONE;
    if (why != MFB_BATCHED) return why;
    if (images) return MFB_ONE_TEXTURES;
    if (need_cut) {
        if (!s->cut(s->u)) return MFB_ONE_PRE;
        *cut = MFB_CUT_PASS;
    }
    if (!s->pre(s->u)) return MFB_ONE_PRE;
    if (s->take(s->u)) return MFB_BATCHED;
    if (*cut) return MFB_ONE_ROOM;
    if (!s->cut(s->u)) return MFB_ONE_ROOM;
    *cut = MFB_CUT_ROOM;
    if (!s->pre(s->u)) return MFB_ONE_PRE;
    return s->take(s->u) ? MFB_BATCHED : MFB_ONE_ROOM;
}

typedef struct { uint64_t why[MFB_REASONS], cuts_pass, cuts_room, cuts_lost, failed; } MfbCount;
static inline void mfb_count(MfbCount *n, int why, int cut, int ok) {
    if (why == MFB_BATCHED && ok) {
        n->why[MFB_BATCHED]++;
        n->cuts_pass += cut == MFB_CUT_PASS, n->cuts_room += cut == MFB_CUT_ROOM;
        return;
    }
    if (why != MFB_BATCHED) n->why[why]++;
    else n->failed++;
    n->cuts_lost += cut != MFB_CUT_NONE;
}
