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
// A draw goes one by one, as before (the way back), when (mfb_batch_need): batching is off; one of its stages writes
// memory (a later draw of the pass could read it); a draw went one by one since the pre-pass command buffer's parts
// began (it wrote the buffers from their start, over the parts of the batched draws drawn after it: until the pre-pass
// command buffer ends, every draw goes one by one); its program once asked for more than its part held in a batch
// (excluded for the session); its program has no history yet where the part would come from it; the part's buffers
// have no room left for it (mfb_part_take); there is no pre-pass command buffer, or the part could not be cut.
#pragma once

#include <stdint.h>

#define MFB_MIN_SLOTS 64u      // the floor's own floor, for programs whose workgroups are large
#define MFB_MIN_TASKS 16u
#define MFB_MIN_SEEN 8u        // records read back before a part comes from the history
#define MFB_HEADROOM 4u
#define MFB_FLOOR_DIV 64u      // a part holds at least 1/64 of a buffer: a pass's 55 draws at most fit (Kokiri, Field)

enum {
    MFB_BATCHED,
    MFB_ONE_OFF,         // NX2SARIA_MESH_FALLBACK_BATCH=0
    MFB_ONE_WRITES,      // a stage writes memory
    MFB_ONE_CLOSED,      // a draw went one by one since the parts began
    MFB_ONE_HISTORY,     // too little history for a part the GPU sizes
    MFB_ONE_EXCLUDED,    // the program overflowed its part once
    MFB_ONE_ROOM,        // no room left in the part's buffers
    MFB_ONE_PRE,         // no pre-pass command buffer, or the part could not be cut
    MFB_REASONS
};

// A program's history (MfbCs.hist): updated where vk_mesh_fb.c reads a record back (record_check)
typedef struct {
    uint32_t slots, tasks;   // the most mesh and task workgroups one of its draws asked for
    uint32_t seen;           // its records read back
    uint32_t excluded;       // a batched draw of it asked for more than its part held
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
static inline int mfb_batch_need(int on, int writes, int closed, int direct, uint32_t count, int has_task,
                                 const MfbHist *h, uint32_t floor_slots, uint32_t floor_tasks, uint32_t *slots,
                                 uint32_t *tasks) {
    *slots = *tasks = 0;
    if (!on) return MFB_ONE_OFF;
    if (writes) return MFB_ONE_WRITES;
    if (closed) return MFB_ONE_CLOSED;
    if (h->excluded) return MFB_ONE_EXCLUDED;
    if (!has_task) {
        if (direct) {
            *slots = count;
            return MFB_BATCHED;
        }
        if (h->seen < MFB_MIN_SEEN) return MFB_ONE_HISTORY;
        *slots = mfb_twice(h->slots, floor_slots);
        return MFB_BATCHED;
    }
    if (h->seen < MFB_MIN_SEEN) return MFB_ONE_HISTORY;   // the mesh workgroups the tasks start are known only on the GPU
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

// Parts taken since the buffers' use started over: a draw one by one then closes the batch (MFB_ONE_CLOSED)
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

// What a record read back says (record_check): the program's history grows, and a batched draw that asked for more
// than its part held excludes the program from batches. Returns 1 when that draw was short.
static inline int mfb_hist_note(MfbHist *h, uint32_t want_slots, uint32_t want_tasks, uint32_t cap_slots,
                                uint32_t cap_tasks, int batched) {
    if (want_slots > h->slots) h->slots = want_slots;
    if (want_tasks > h->tasks) h->tasks = want_tasks;
    h->seen++;
    int shortfall = want_slots > cap_slots || want_tasks > cap_tasks;
    if (batched && shortfall) h->excluded = 1;
    return batched && shortfall;
}
