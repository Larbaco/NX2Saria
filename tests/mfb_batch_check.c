// runtime/mfb_batch.h: the rule of the mesh fallback's batch (vk_exec_draw.c mfb_draw): which draws are batched and
// which go one by one (the way back), the parts of the buffers they take, and a program's history. Built and run by
// tests/mfb_batch_check.py; prints each failed check and exits 1 on any.
#include <stdio.h>
#include <string.h>

#include "mfb_batch.h"

#define FS 100u   // the parts' floors in these checks (mfb_floor)
#define FT 20u

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// The draw path's order as vk_exec_draw.c mfb_draw has it, without Vulkan: mfb_batch_need, then mfb_batch_steps (the
// same function mfb_draw calls) with the cut, the pre-pass command buffer and the part as calls on this simulation, and
// mfb_count. A draw one by one after parts were taken closes them (vk_mesh_fb.c closed): the next batched draw cuts.
// The end of a pre-pass command buffer (a cut, or pre_end) starts the parts over.
typedef struct {
    MfbSpan used, cap;
    uint64_t align, tb, lb, mb;
    int closed, pass_ended, cut_fails, pre_fails, cuts;
    MfbPart part;
    MfbCount n;
} Sim;
static void sim_pre_end(Sim *s) { s->used = (MfbSpan){0, 0, 0}, s->closed = 0; }
static int sim_cut(void *u) {
    Sim *s = u;
    if (s->cut_fails) return 0;
    s->cuts++, s->pass_ended = 0;
    sim_pre_end(s);
    return 1;
}
static int sim_pre(void *u) { return !((Sim *)u)->pre_fails; }
static int sim_take(void *u) {
    Sim *s = u;
    return mfb_part_take(&s->used, &s->cap, s->align, s->tb, s->lb, s->mb, &s->part);
}
static int sim_draw_ex(Sim *s, int writes, int images, int direct, uint32_t count, int has_task, const MfbHist *h,
                       uint32_t slot_words, uint32_t task_words, MfbPart *part, int *cut) {
    uint32_t slots, tasks;
    int why = mfb_batch_need(1, writes, direct, count, has_task, h, FS, FT, &slots, &tasks);
    mfb_part_bytes(slots, tasks, has_task, task_words, slot_words, &s->tb, &s->lb, &s->mb);
    const MfbSteps st = {sim_cut, sim_pre, sim_take, s};
    why = mfb_batch_steps(why, images, s->pass_ended || s->closed, &st, cut);
    if (why != MFB_BATCHED) {
        if (mfb_parts_taken(&s->used)) s->closed = 1;   // vk_meshfb_one_by_one
        s->pass_ended = 1;                              // a draw one by one ends its pass (vk_exec_barrier)
    } else if (part) {
        *part = s->part;
    }
    mfb_count(&s->n, why, *cut, 1);
    return why;
}
static int sim_draw(Sim *s, int writes, int direct, uint32_t count, int has_task, const MfbHist *h, uint32_t slot_words,
                    uint32_t task_words, MfbPart *part) {
    int cut;
    return sim_draw_ex(s, writes, 0, direct, count, has_task, h, slot_words, task_words, part, &cut);
}

int main(void) {
    uint32_t slots, tasks;
    MfbHist none = {0}, seen = {.slots = 40, .tasks = 3, .seen = MFB_MIN_SEEN}, big = {.slots = 900, .tasks = 70, .seen = 20},
            out = {.slots = 10, .tasks = 1, .seen = 9, .excluded = 1};
    // ---- the reasons, in their order
    CHECK(mfb_batch_need(0, 0, 1, 8, 0, &seen, FS, FT, &slots, &tasks) == MFB_ONE_OFF, "off is not one by one");
    CHECK(!slots && !tasks, "a draw one by one has a part");
    CHECK(mfb_batch_need(1, 1, 1, 8, 0, &seen, FS, FT, &slots, &tasks) == MFB_ONE_WRITES, "a stage that writes is batched");
    CHECK(mfb_batch_need(1, 0, 1, 8, 0, &out, FS, FT, &slots, &tasks) == MFB_ONE_EXCLUDED, "an excluded program is batched");
    CHECK(mfb_batch_need(1, 0, 0, 0, 0, &none, FS, FT, &slots, &tasks) == MFB_ONE_HISTORY,
          "a count the GPU writes, no history: batched");
    CHECK(mfb_batch_need(1, 0, 0, 0, 0, &(MfbHist){.slots = 40, .seen = MFB_MIN_SEEN - 1}, FS, FT, &slots, &tasks) ==
              MFB_ONE_HISTORY, "a GPU count with fewer records read back than MFB_MIN_SEEN: batched");
    CHECK(mfb_batch_need(1, 0, 1, 8, 1, &none, FS, FT, &slots, &tasks) == MFB_ONE_HISTORY,
          "a task stage without history: batched (its mesh workgroups are known only on the GPU)");
    // ---- the parts' sizes
    CHECK(mfb_batch_need(1, 0, 1, 300, 0, &none, FS, FT, &slots, &tasks) == MFB_BATCHED && slots == 300 && !tasks,
          "a direct draw without a task stage: %u slots, %u tasks (300, 0)", slots, tasks);
    CHECK(mfb_batch_need(1, 0, 0, 0, 0, &seen, FS, FT, &slots, &tasks) == MFB_BATCHED && slots == 160 && !tasks,
          "a GPU count with history 40: %u slots (four times, 160)", slots);
    CHECK(mfb_batch_need(1, 0, 0, 0, 0, &(MfbHist){.slots = 3, .seen = 9}, FS, FT, &slots, &tasks) == MFB_BATCHED &&
              slots == FS, "a small history: %u slots (the floor, %u)", slots, FS);
    CHECK(mfb_batch_need(1, 0, 1, 12, 1, &seen, FS, FT, &slots, &tasks) == MFB_BATCHED && tasks == 12 && slots == 160,
          "a direct draw with a task stage: %u tasks (the count, 12), %u slots (160)", tasks, slots);
    CHECK(mfb_batch_need(1, 0, 0, 0, 1, &big, FS, FT, &slots, &tasks) == MFB_BATCHED && tasks == 280 && slots == 3600,
          "a GPU count with a task stage: %u tasks (280), %u slots (3600)", tasks, slots);
    CHECK(mfb_batch_need(1, 0, 0, 0, 1, &seen, FS, FT, &slots, &tasks) == MFB_BATCHED && tasks == FT,
          "a small task history: %u tasks (the floor, %u)", tasks, FT);
    // the floor: 1/64 of the buffer, in the program's workgroups, never under the least
    CHECK(mfb_floor(112ull << 20, 540 * 4, MFB_MIN_SLOTS) == (112ull << 20) / 64 / 2160, "the floor of 112 MiB at 2160 "
          "bytes: %u", mfb_floor(112ull << 20, 540 * 4, MFB_MIN_SLOTS));
    CHECK(mfb_floor(1 << 20, 1 << 20, MFB_MIN_SLOTS) == MFB_MIN_SLOTS, "a floor under the least");
    CHECK(mfb_floor(1 << 20, 0, 7) == 7, "a floor of 0-byte workgroups");
    // the case seen in Kokiri Forest: a program that had asked for 32 at most asked for 454 (mfb_batch.h)
    CHECK(mfb_batch_need(1, 0, 0, 0, 0, &(MfbHist){.slots = 32, .seen = 9},
                         mfb_floor(112ull << 20, 540 * 4, MFB_MIN_SLOTS), FT, &slots, &tasks) == MFB_BATCHED &&
              slots >= 454, "a history of 32 at 2160 bytes a workgroup: %u slots, short of 454", slots);
    CHECK(mfb_twice(0xf0000000u, 1) == 0x7fffffffu, "a huge history does not saturate");
    // ---- the cut: at a pass's first batched draw, and again whenever the pass it served ended
    CHECK(mfb_need_cut(1, 7, 8), "a new pass does not cut");
    CHECK(!mfb_need_cut(1, 8, 8), "the pass of the pre-pass command buffer, still open, cuts again");
    CHECK(mfb_need_cut(0, 8, 8), "the pass ended (a uniform update between two draws) and no cut: the second draw's "
                                 "stages would run before the update");
    CHECK(mfb_need_cut(0, 7, 8), "no pass open, another serial: no cut");
    uint64_t tb, lb, mb;
    mfb_part_bytes(80, 12, 1, 9, 100, &tb, &lb, &mb);
    CHECK(tb == 12 * 9 * 4 && lb == 80 * 4 && mb == 80 * 100 * 4, "part bytes with a task stage: %llu %llu %llu",
          (unsigned long long)tb, (unsigned long long)lb, (unsigned long long)mb);
    mfb_part_bytes(80, 0, 0, 9, 100, &tb, &lb, &mb);
    CHECK(!tb && !lb && mb == 32000, "part bytes without a task stage: %llu %llu %llu", (unsigned long long)tb,
          (unsigned long long)lb, (unsigned long long)mb);
    // ---- the parts: in turn, aligned, never past the buffers, and nothing taken when one does not fit
    MfbSpan used = {0}, cap = {1024, 1024, 4096};
    MfbPart p, q;
    CHECK(mfb_part_take(&used, &cap, 256, 100, 0, 1000, &p), "a first part does not fit");
    CHECK(p.task_off == 0 && p.task_len == 256 && p.list_off == 0 && p.list_len == 256 && p.mesh_off == 0 &&
              p.mesh_len == 1024, "the first part: %llu+%llu %llu+%llu %llu+%llu", (unsigned long long)p.task_off,
          (unsigned long long)p.task_len, (unsigned long long)p.list_off, (unsigned long long)p.list_len,
          (unsigned long long)p.mesh_off, (unsigned long long)p.mesh_len);
    CHECK(mfb_part_take(&used, &cap, 256, 10, 10, 10, &q) && q.task_off == 256 && q.list_off == 256 && q.mesh_off == 1024,
          "the second part does not follow the first");
    CHECK(q.task_off + q.task_len <= cap.task && q.mesh_off + q.mesh_len <= cap.mesh, "a part past the buffers");
    MfbSpan before = used;
    CHECK(!mfb_part_take(&used, &cap, 256, 10, 10, 4096, &q), "a part larger than the room left fits");
    CHECK(!memcmp(&before, &used, sizeof used), "a part that did not fit took room");
    CHECK(mfb_part_take(&used, &cap, 256, 10, 10, 2048, &q) && q.mesh_off == 1280 && q.mesh_off + q.mesh_len <= 4096,
          "what is left is not taken");
    CHECK(mfb_part_take(&(MfbSpan){0}, &cap, 1, 0, 0, 0, &q) && q.task_len == 4 && q.mesh_len == 4,
          "an empty part has no size for its binding");
    // ---- the history: grows from the records read back; a batched draw short of its part excludes the program
    MfbHist h = {0};
    CHECK(!mfb_hist_note(&h, 50, 2, 1000, 100, 0) && h.slots == 50 && h.tasks == 2 && h.seen == 1, "the first record");
    CHECK(!mfb_hist_note(&h, 30, 7, 1000, 100, 1) && h.slots == 50 && h.tasks == 7 && h.seen == 2, "the most so far");
    CHECK(!mfb_hist_note(&h, 2000, 0, 1000, 100, 0) && !h.excluded, "a draw one by one short of the buffers excludes");
    CHECK(mfb_hist_note(&h, 120, 0, 100, 100, 1) && h.excluded, "a batched draw short of its part does not exclude");
    CHECK(mfb_batch_need(1, 0, 1, 8, 0, &h, FS, FT, &slots, &tasks) == MFB_ONE_EXCLUDED, "excluded, then batched again");
    MfbHist ht = {0};
    CHECK(mfb_hist_note(&ht, 10, 40, 100, 32, 1) && ht.excluded, "batched short of its task part does not exclude");
    // ---- a pass as mfb_draw sees it: the way back to one by one, its reasons and the cuts
    Sim s = {.cap = {1 << 20, 1 << 20, 1 << 24}, .align = 64};
    MfbHist grass = {.slots = 200, .tasks = 8, .seen = 30}, fresh = {0};
    for (int i = 0; i < 5; i++)
        CHECK(sim_draw(&s, 0, 0, 0, 1, &grass, 120, 16, &p) == MFB_BATCHED, "draw %d of a run is not batched", i);
    CHECK(s.cuts == 0, "batched draws of one pass cut %d times", s.cuts);
    CHECK(sim_draw(&s, 0, 0, 0, 1, &fresh, 120, 16, &p) == MFB_ONE_HISTORY, "a program without history is batched");
    CHECK(s.closed, "a draw one by one after the parts did not close them");
    int cut;
    CHECK(sim_draw_ex(&s, 0, 0, 0, 0, 1, &grass, 120, 16, &p, &cut) == MFB_BATCHED && cut == MFB_CUT_PASS && !p.mesh_off,
          "a draw after one by one: not batched, or not cut (cut %d), or not from the buffers' start (it would read what "
          "the draw one by one wrote over its part)", cut);
    CHECK(!s.closed && s.cuts == 1, "the cut left the parts closed, or cut %d times", s.cuts);
    CHECK(sim_draw_ex(&s, 0, 0, 0, 0, 1, &grass, 120, 16, &p, &cut) == MFB_BATCHED && cut == MFB_CUT_NONE && p.mesh_off,
          "the next draw of the same pass cut again (%d) or took the same part", cut);
    // closed with the pass still open (a draw one by one that did not end it): the next batched draw cuts too
    s.closed = 1, s.pass_ended = 0;
    CHECK(sim_draw_ex(&s, 0, 0, 0, 0, 1, &grass, 120, 16, &p, &cut) == MFB_BATCHED && cut == MFB_CUT_PASS,
          "closed parts in an open pass: no cut (%d)", cut);
    // a stage that reads a texture or image: one by one, without a cut
    CHECK(sim_draw_ex(&s, 0, 1, 0, 0, 1, &grass, 120, 16, &p, &cut) == MFB_ONE_TEXTURES && cut == MFB_CUT_NONE,
          "a stage that reads images is batched, or cut (%d)", cut);
    // the cut fails: one by one (no pre-pass), and the draws after it are tried again
    Sim e = {.cap = {1 << 20, 1 << 20, 1 << 24}, .align = 64, .pass_ended = 1, .cut_fails = 1};
    CHECK(sim_draw_ex(&e, 0, 0, 0, 0, 1, &grass, 120, 16, &p, &cut) == MFB_ONE_PRE && cut == MFB_CUT_NONE,
          "a failed cut: %d", cut);
    e.cut_fails = 0, e.pre_fails = 1;
    CHECK(sim_draw_ex(&e, 0, 0, 0, 0, 1, &grass, 120, 16, &p, &cut) == MFB_ONE_PRE && cut == MFB_CUT_PASS,
          "no pre-pass command buffer after a cut: batched, or the cut not said (%d)", cut);
    CHECK(e.n.cuts_lost == 1 && e.n.why[MFB_ONE_PRE] == 2 && !e.n.why[MFB_BATCHED],
          "the counts of the failed draws: %llu cuts for nothing, %llu no pre-pass, %llu batched",
          (unsigned long long)e.n.cuts_lost, (unsigned long long)e.n.why[MFB_ONE_PRE], (unsigned long long)e.n.why[MFB_BATCHED]);
    // the buffers fill: the draw that does not fit cuts and starts the parts over; a part larger than the buffers goes
    // one by one
    Sim f = {.cap = {1 << 16, 1 << 16, 120 * 4 * 1000}, .align = 64};
    int n = 0, why;
    while ((why = sim_draw_ex(&f, 0, 0, 1, 300, 0, &fresh, 120, 16, &p, &cut)) == MFB_BATCHED && cut == MFB_CUT_NONE) n++;
    CHECK(n == 3 && why == MFB_BATCHED && cut == MFB_CUT_ROOM && !p.mesh_off,
          "a full buffer: %d batched (3), then %d cut %d (batched, cut for room, from the start)", n, why, cut);
    CHECK(sim_draw_ex(&f, 0, 0, 1, 1200, 0, &fresh, 120, 16, &p, &cut) == MFB_ONE_ROOM && cut == MFB_CUT_ROOM,
          "a part larger than the buffers: %d, cut %d", why, cut);
    f.pass_ended = 1;
    CHECK(sim_draw_ex(&f, 0, 0, 1, 1200, 0, &fresh, 120, 16, &p, &cut) == MFB_ONE_ROOM && cut == MFB_CUT_PASS,
          "a part larger than the buffers after a pass cut is cut again for room");
    CHECK(f.n.why[MFB_BATCHED] == 4 && f.n.why[MFB_ONE_ROOM] == 2 && f.n.cuts_room == 1 && f.n.cuts_lost == 2,
          "the counts: %llu batched (4), %llu no room (2), %llu cuts for room (1), %llu for nothing (2)",
          (unsigned long long)f.n.why[MFB_BATCHED], (unsigned long long)f.n.why[MFB_ONE_ROOM],
          (unsigned long long)f.n.cuts_room, (unsigned long long)f.n.cuts_lost);
    // a batched draw whose stages failed to record: not batched, and its cut was for nothing
    MfbCount c0 = {0};
    mfb_count(&c0, MFB_BATCHED, MFB_CUT_PASS, 0);
    CHECK(!c0.why[MFB_BATCHED] && c0.cuts_lost == 1 && !c0.cuts_pass, "a failed batched draw counted as batched");
    // the task overflow (meshfb_clamp.comp keeps the count asked for): a batched draw short of its task part excludes
    MfbHist tk = {.slots = 10, .tasks = 4, .seen = 9};
    CHECK(mfb_hist_note(&tk, 10, 5000, 100, 32, 1) && tk.excluded && tk.tasks == 5000,
          "a task count past the part, kept by the clamp: not excluded");
    if (fails) return 1;
    printf("mfb_batch_check: ok\n");
    return 0;
}
