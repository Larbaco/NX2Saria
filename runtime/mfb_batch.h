// The mesh fallback's batch (vk_exec_draw.c mfb_draw, NX2SARIA_MESH_FALLBACK_BATCH): the rule, apart from Vulkan so
// that tests/mfb_batch_check.c runs it.
//
// A mesh draw through the fallback runs its program's stages as compute dispatches that write the fallback's buffers,
// and an ordinary draw reads them. One by one, each draw ends its pass for its dispatches (a full barrier between
// every two steps) and the draw after it begins the pass again. Batched, the draws of one pass have their dispatches
// in the part's pre-pass command buffer (vk_sync_pre), which the queue runs before the part, and the pass stays whole:
// every draw of the batch needs a part of the buffers of its own, since all the dispatches run before all the draws.
//
// A draw's part (mfb_batch_need):
//   - its mesh workgroups: the count itself for a draw the CPU knows the count of and that has no task stage; else
//     MFB_HEADROOM times the most the program asked for in a draw so far (its history, read back from the records,
//     from MFB_MIN_SEEN of them), and at least a floor: what 1/MFB_FLOOR_DIV of the buffer holds (mfb_floor). A history
//     of a few records undersizes: in Kokiri Forest programs that had asked for 32 workgroups at most asked for 72,
//     81 and 454 a moment later (a batch at twice the history, 2026-11-22);
//   - its task workgroups, with a task stage: the count itself for a direct draw, else the same rule.
// A draw goes one by one, as before (the way back), when (mfb_batch_need, mfb_batch_steps): batching is off; one of its
// stages writes memory (a later draw of the pass could read it); its program once asked for more than its part held in a
// batch (excluded for the session); its program has no history yet where the part would come from it; its task or mesh
// stage reads a texture or image (its bind may record an image's first fill, which needs the part's pass); its part
// does not fit even a pre-pass command buffer of its own; there is no pre-pass command buffer, or the part could not be
// cut.
//
// The cut (mfb_batch_steps): a batched draw cuts the part, so that its stages go to a new pre-pass command buffer that
// runs after everything the part recorded before, when
//   - the pass the pre-pass command buffer serves ended since (mfb_need_cut);
//   - a draw went one by one since the parts began (vk_mesh_fb.c closed): it wrote the buffers from their start, over
//     the parts. A draw one by one ends its pass, so this is the case above; the flag keeps it so if that changes.
//     Until 2026-11-22 every later draw went one by one instead (MFB_ONE_CLOSED), which lost about half the batching
//     in Kokiri Forest;
//   - its part does not fit what the parts left: they start over in the new pre-pass command buffer.
//
// A task overflow: meshfb_clamp.comp keeps the task workgroups asked for in the record's report word (13) before it
// cuts the dispatch to the part's room, so a batched draw short of its task part excludes its program like one short of
// its mesh part (until 2026-11-22 the scan read the count already cut, and such a draw was never seen).
#pragma once

#include <stdint.h>

#define MFB_MIN_SLOTS 64u      // the floor's own floor, for programs whose workgroups are large
#define MFB_MIN_TASKS 16u
#define MFB_MIN_SEEN 8u        // records read back before a part comes from the history
#define MFB_MIN_CALM 32u       // and records in a row since its last jump (mfb_hist_note): the history has settled
#define MFB_READMIT 256u       // records in a row without a jump that take an excluded program back into batches
#define MFB_MAX_SHORTS 3u      // the batched draws short of their part after which a program stays one by one
#define MFB_HEADROOM 4u
#define MFB_FLOOR_DIV 32u      // a part holds at least 1/32 of a buffer: 32 such parts fit a pre-pass command buffer
                               // (a longer run of draws cuts for room). At 1/64 (483 workgroups of 3.8 KB) one draw of a
                               // program with a settled history of 120 asked for 577 in every Kokiri Forest start.

enum {
    MFB_BATCHED,
    MFB_ONE_OFF,         // NX2SARIA_MESH_FALLBACK_BATCH=0
    MFB_ONE_WRITES,      // a stage writes memory
    MFB_ONE_HISTORY,     // too little history for a part the GPU sizes
    MFB_ONE_EXCLUDED,    // the program overflowed its part once
    MFB_ONE_ROOM,        // the part does not fit a pre-pass command buffer of its own
    MFB_ONE_PRE,         // no pre-pass command buffer, or the part could not be cut
    MFB_ONE_TEXTURES,    // a task or mesh stage reads a texture or image (vk_exec_draw.c mfb_draw: its bind may record
                         // an image's first fill or an alias copy, vk_exec_inline_begin, which needs the part's pass)
    MFB_ONE_LOADING,     // an area's textures are loading (vk_exec_pool_loading): its frames are not shown, and the
                         // programs' histories have not settled yet (every short part seen on the bench was there)
    MFB_REASONS
};

// A program's history (MfbCs.hist): updated where vk_mesh_fb.c reads a record back (record_check)
// The parts of a program whose draws GPU-written counts size come from its history. In the bench's sessions every batched
// draw short of its part came in the first seconds (the loading after the start, before Link arrived), from a history
// of a few records still growing: parts of 141 to 483 workgroups for draws of 498 to 1581 (2026-11-22). A history is
// taken only once it has settled: MFB_MIN_CALM records in a row without a jump (a record more than twice the most so
// far, or the first). A batched draw short of its part excludes the program (its draws one by one, which draw whole);
// MFB_READMIT calm records later, with the history now holding what it asked for, it is batched again, and after
// MFB_MAX_SHORTS such draws it stays one by one for the session.
typedef struct {
    uint32_t slots, tasks;   // the most mesh and task workgroups one of its draws asked for
    uint32_t seen;           // its records read back
    uint32_t excluded;       // a batched draw of it asked for more than its part held (until it is taken back)
    uint32_t calm;           // records in a row since the last jump
    uint32_t shorts;         // its batched draws short of their part, this session
} MfbHist;

// The workgroups of 1/MFB_FLOOR_DIV of a buffer of cap bytes, each of unit bytes; at least least.
static inline uint32_t mfb_floor(uint64_t cap, uint64_t unit, uint32_t least) {
    uint64_t n = unit ? cap / MFB_FLOOR_DIV / unit : 0;
    if (n < least) n = least;
    return n > 0x7fffffffu ? 0x7fffffffu : (uint32_t)n;
}

static inline uint32_t mfb_twice(uint32_t v, uint32_t least) {   // MFB_HEADROOM times v, at least least
    uint64_t t = (uint64_t)MFB_HEADROOM * v;
    if (t < least) t = least;
    return t > 0x7fffffffu ? 0x7fffffffu : (uint32_t)t;
}

// The workgroups a batched draw's part holds, or a reason it goes one by one. direct: the CPU knows count. floor_slots,
// floor_tasks: the parts' floors (mfb_floor).
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
    // the mesh workgroups the tasks start are known only on the GPU
    if (h->seen < MFB_MIN_SEEN || h->calm < MFB_MIN_CALM) return MFB_ONE_HISTORY;
    *tasks = direct ? count : mfb_twice(h->tasks, floor_tasks);
    *slots = mfb_twice(h->slots, floor_slots);
    return MFB_BATCHED;
}

// The bytes of a part of slots mesh and tasks task workgroups: the task payloads, the list (a word per mesh workgroup,
// with a task stage only) and the mesh outputs (slot_words per mesh workgroup).
static inline void mfb_part_bytes(uint32_t slots, uint32_t tasks, int has_task, uint32_t task_words, uint32_t slot_words,
                                  uint64_t *task_b, uint64_t *list_b, uint64_t *mesh_b) {
    *task_b = has_task ? (uint64_t)tasks * task_words * 4 : 0;
    *list_b = has_task ? (uint64_t)slots * 4 : 0;
    *mesh_b = (uint64_t)slots * slot_words * 4;
}

// The part's buffers, filled from the start for each pre-pass command buffer (mfb_part_reset at its end)
typedef struct { uint64_t task, list, mesh; } MfbSpan;
typedef struct { uint64_t task_off, task_len, list_off, list_len, mesh_off, mesh_len; } MfbPart;

// Whether a batched draw cuts the part first (vk_exec_split_now): unless the pass the pre-pass command buffer serves
// (pre_pass, set after the cut's draw began it) is still the open one. pass_serial goes up only where a pass begins, so
// a pass ended since (a uniform update's copy, a game dispatch, a draw one by one) leaves it equal: what the part
// recorded after that end would run after the pre-pass command buffer that the draw's stages go to.
static inline int mfb_need_cut(int in_pass, uint32_t pre_pass, uint32_t pass_serial) {
    return !in_pass || pre_pass != pass_serial;
}

// Parts taken since the buffers' use started over: a draw one by one then closes them (a cut before the next batched)
static inline int mfb_parts_taken(const MfbSpan *u) { return u->task || u->list || u->mesh; }

static inline uint64_t mfb_align_up(uint64_t v, uint64_t a) { return a > 1 ? (v + a - 1) / a * a : v; }

// The next part of task_b, list_b and mesh_b bytes (each rounded up to align, the storage offset alignment) from used
// within cap; 0 when one does not fit (used is left as it was). A part of 0 bytes still takes align, so its binding
// has a size.
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

enum { MFB_NOTE_SHORT = 1, MFB_NOTE_READMIT = 2 };   // mfb_hist_note

// What a record read back says (record_check): the program's history grows, and a batched draw that asked for more
// than its part held excludes the program from batches. Returns MFB_NOTE_SHORT when that draw was short,
// MFB_NOTE_READMIT when the program is taken back into batches, else 0.
// A draw one by one that asked for more than the whole buffers hold (a garbage count the GPU wrote, meshfb_clamp.comp)
// raises the history only to what they held: a part from it fits them still (else every later draw of the program would
// be one by one, or cut for nothing).
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
        // the draws of the program already in flight come back short together: one exclusion counts once
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

// ---- mfb_draw's steps after mfb_batch_need, with the cut, the pre-pass command buffer and the part as calls
// (vk_exec_split_now, vk_sync_pre, vk_meshfb_part_take), so that tests/mfb_batch_check.c runs the same order.
typedef struct {
    int (*cut)(void *u);    // the part ends here; 0: it could not be cut
    int (*pre)(void *u);    // the pre-pass command buffer; 0: none
    int (*take)(void *u);   // the draw's part of the buffers; 0: it does not fit what is left
    void *u;
} MfbSteps;
enum { MFB_CUT_NONE, MFB_CUT_PASS, MFB_CUT_ROOM };   // mfb_batch_steps *cut: none, at the pass or closed, for room

// why: mfb_batch_need's answer. images: a task or mesh stage reads a texture or image. need_cut: mfb_need_cut, or a
// draw went one by one since the parts began. Returns MFB_BATCHED (the part taken) or why the draw goes one by one;
// *cut says whether and why the part was cut on the way (a cut for a draw then one by one was for nothing).
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
    if (*cut) return MFB_ONE_ROOM;   // the buffers' use started over with this cut: the part is too large for them
    if (!s->cut(s->u)) return MFB_ONE_ROOM;
    *cut = MFB_CUT_ROOM;
    if (!s->pre(s->u)) return MFB_ONE_PRE;
    return s->take(s->u) ? MFB_BATCHED : MFB_ONE_ROOM;
}

// The counts of the log line (vk_exec_draw.c mfb_batch_line). ok: a batched draw's stages were recorded.
typedef struct { uint64_t why[MFB_REASONS], cuts_pass, cuts_room, cuts_lost, failed; } MfbCount;
static inline void mfb_count(MfbCount *n, int why, int cut, int ok) {
    if (why == MFB_BATCHED && ok) {
        n->why[MFB_BATCHED]++;
        n->cuts_pass += cut == MFB_CUT_PASS, n->cuts_room += cut == MFB_CUT_ROOM;
        return;
    }
    if (why != MFB_BATCHED) n->why[why]++;
    else n->failed++;   // batched, its stages' pipeline failed: the draw was skipped
    n->cuts_lost += cut != MFB_CUT_NONE;
}
