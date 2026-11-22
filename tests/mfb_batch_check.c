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

// The draw path's order as vk_exec_draw.c mfb_draw and vk_mesh_fb.c have it, without Vulkan: a draw is batched when
// mfb_batch_need says so and its part fits; one by one, it closes the batch when parts were taken; the end of the
// pre-pass command buffer (pre_end) starts the parts over and opens it again.
typedef struct { MfbSpan used, cap; uint64_t align; int closed; uint64_t why[MFB_REASONS]; } Sim;
static int sim_draw(Sim *s, int writes, int direct, uint32_t count, int has_task, const MfbHist *h, uint32_t slot_words,
                    uint32_t task_words, MfbPart *part) {
    uint32_t slots, tasks;
    int why = mfb_batch_need(1, writes, s->closed, direct, count, has_task, h, FS, FT, &slots, &tasks);
    if (why == MFB_BATCHED) {
        uint64_t tb, lb, mb;
        mfb_part_bytes(slots, tasks, has_task, task_words, slot_words, &tb, &lb, &mb);
        if (!mfb_part_take(&s->used, &s->cap, s->align, tb, lb, mb, part)) why = MFB_ONE_ROOM;
    }
    if (why != MFB_BATCHED && mfb_parts_taken(&s->used)) s->closed = 1;
    s->why[why]++;
    return why;
}
static void sim_pre_end(Sim *s) { s->used = (MfbSpan){0, 0, 0}, s->closed = 0; }

int main(void) {
    uint32_t slots, tasks;
    MfbHist none = {0}, seen = {.slots = 40, .tasks = 3, .seen = MFB_MIN_SEEN}, big = {.slots = 900, .tasks = 70, .seen = 20},
            out = {.slots = 10, .tasks = 1, .seen = 9, .excluded = 1};
    // ---- the reasons, in their order
    CHECK(mfb_batch_need(0, 0, 0, 1, 8, 0, &seen, FS, FT, &slots, &tasks) == MFB_ONE_OFF, "off is not one by one");
    CHECK(!slots && !tasks, "a draw one by one has a part");
    CHECK(mfb_batch_need(1, 1, 0, 1, 8, 0, &seen, FS, FT, &slots, &tasks) == MFB_ONE_WRITES, "a stage that writes is batched");
    CHECK(mfb_batch_need(1, 0, 1, 1, 8, 0, &seen, FS, FT, &slots, &tasks) == MFB_ONE_CLOSED, "a closed batch takes a draw");
    CHECK(mfb_batch_need(1, 0, 0, 1, 8, 0, &out, FS, FT, &slots, &tasks) == MFB_ONE_EXCLUDED, "an excluded program is batched");
    CHECK(mfb_batch_need(1, 0, 0, 0, 0, 0, &none, FS, FT, &slots, &tasks) == MFB_ONE_HISTORY,
          "a count the GPU writes, no history: batched");
    CHECK(mfb_batch_need(1, 0, 0, 0, 0, 0, &(MfbHist){.slots = 40, .seen = MFB_MIN_SEEN - 1}, FS, FT, &slots, &tasks) ==
              MFB_ONE_HISTORY, "a GPU count with fewer records read back than MFB_MIN_SEEN: batched");
    CHECK(mfb_batch_need(1, 0, 0, 1, 8, 1, &none, FS, FT, &slots, &tasks) == MFB_ONE_HISTORY,
          "a task stage without history: batched (its mesh workgroups are known only on the GPU)");
    // ---- the parts' sizes
    CHECK(mfb_batch_need(1, 0, 0, 1, 300, 0, &none, FS, FT, &slots, &tasks) == MFB_BATCHED && slots == 300 && !tasks,
          "a direct draw without a task stage: %u slots, %u tasks (300, 0)", slots, tasks);
    CHECK(mfb_batch_need(1, 0, 0, 0, 0, 0, &seen, FS, FT, &slots, &tasks) == MFB_BATCHED && slots == 160 && !tasks,
          "a GPU count with history 40: %u slots (four times, 160)", slots);
    CHECK(mfb_batch_need(1, 0, 0, 0, 0, 0, &(MfbHist){.slots = 3, .seen = 9}, FS, FT, &slots, &tasks) == MFB_BATCHED &&
              slots == FS, "a small history: %u slots (the floor, %u)", slots, FS);
    CHECK(mfb_batch_need(1, 0, 0, 1, 12, 1, &seen, FS, FT, &slots, &tasks) == MFB_BATCHED && tasks == 12 && slots == 160,
          "a direct draw with a task stage: %u tasks (the count, 12), %u slots (160)", tasks, slots);
    CHECK(mfb_batch_need(1, 0, 0, 0, 0, 1, &big, FS, FT, &slots, &tasks) == MFB_BATCHED && tasks == 280 && slots == 3600,
          "a GPU count with a task stage: %u tasks (280), %u slots (3600)", tasks, slots);
    CHECK(mfb_batch_need(1, 0, 0, 0, 0, 1, &seen, FS, FT, &slots, &tasks) == MFB_BATCHED && tasks == FT,
          "a small task history: %u tasks (the floor, %u)", tasks, FT);
    // the floor: 1/64 of the buffer, in the program's workgroups, never under the least
    CHECK(mfb_floor(112ull << 20, 540 * 4, MFB_MIN_SLOTS) == (112ull << 20) / 64 / 2160, "the floor of 112 MiB at 2160 "
          "bytes: %u", mfb_floor(112ull << 20, 540 * 4, MFB_MIN_SLOTS));
    CHECK(mfb_floor(1 << 20, 1 << 20, MFB_MIN_SLOTS) == MFB_MIN_SLOTS, "a floor under the least");
    CHECK(mfb_floor(1 << 20, 0, 7) == 7, "a floor of 0-byte workgroups");
    // the case seen in Kokiri Forest: a program that had asked for 32 at most asked for 454 (mfb_batch.h)
    CHECK(mfb_batch_need(1, 0, 0, 0, 0, 0, &(MfbHist){.slots = 32, .seen = 9},
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
    CHECK(mfb_batch_need(1, 0, 0, 1, 8, 0, &h, FS, FT, &slots, &tasks) == MFB_ONE_EXCLUDED, "excluded, then batched again");
    MfbHist ht = {0};
    CHECK(mfb_hist_note(&ht, 10, 40, 100, 32, 1) && ht.excluded, "batched short of its task part does not exclude");
    // ---- a pass as mfb_draw sees it: the way back to one by one and its reasons
    Sim s = {.cap = {1 << 20, 1 << 20, 1 << 24}, .align = 64};
    MfbHist grass = {.slots = 200, .tasks = 8, .seen = 30}, fresh = {0};
    for (int i = 0; i < 5; i++)
        CHECK(sim_draw(&s, 0, 0, 0, 1, &grass, 120, 16, &p) == MFB_BATCHED, "draw %d of a run is not batched", i);
    CHECK(sim_draw(&s, 0, 0, 0, 1, &fresh, 120, 16, &p) == MFB_ONE_HISTORY, "a program without history is batched");
    CHECK(s.closed, "a draw one by one after the parts did not close the batch");
    CHECK(sim_draw(&s, 0, 0, 0, 1, &grass, 120, 16, &p) == MFB_ONE_CLOSED,
          "a draw after one by one is batched (it would read what the draw one by one wrote over its part)");
    sim_pre_end(&s);
    CHECK(!s.closed && !mfb_parts_taken(&s.used), "the pre-pass command buffer's end leaves the batch closed or parts taken");
    CHECK(sim_draw(&s, 0, 0, 0, 1, &grass, 120, 16, &p) == MFB_BATCHED && !p.mesh_off,
          "the next pre-pass command buffer's first draw is not batched from the buffers' start");
    sim_pre_end(&s);
    CHECK(sim_draw(&s, 0, 0, 0, 1, &fresh, 120, 16, &p) == MFB_ONE_HISTORY && !s.closed,
          "a draw one by one before any part closes the batch");
    CHECK(sim_draw(&s, 0, 1, 50, 0, &fresh, 120, 16, &p) == MFB_BATCHED, "a direct draw without a task stage after it");
    // the buffers fill: the draw that does not fit goes one by one and closes the batch
    Sim f = {.cap = {1 << 16, 1 << 16, 120 * 4 * 1000}, .align = 64};
    int n = 0, why;
    while ((why = sim_draw(&f, 0, 1, 300, 0, &fresh, 120, 16, &p)) == MFB_BATCHED) n++;
    CHECK(n == 3 && why == MFB_ONE_ROOM && f.closed, "a full buffer: %d batched (3), then %d (no room) closed %d", n, why,
          f.closed);
    CHECK(f.why[MFB_BATCHED] == 3 && f.why[MFB_ONE_ROOM] == 1, "the counts: %llu batched, %llu no room",
          (unsigned long long)f.why[MFB_BATCHED], (unsigned long long)f.why[MFB_ONE_ROOM]);
    if (fails) return 1;
    printf("mfb_batch_check: ok\n");
    return 0;
}
