#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vk_tex_compact.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define MB(n) ((uint64_t)(n) << 20)

static TcpBlock B(uint64_t id, uint64_t size) { return (TcpBlock){.id = id, .size = size}; }
static TcpAlloc A(uint32_t block, int what, uint64_t bytes) { return (TcpAlloc){block, (uint8_t)what, bytes}; }

static TcpParams P(uint64_t free_bytes, uint64_t min_gain) {
    return (TcpParams){.free_bytes = free_bytes, .sparse_pct = 50, .room_pct = 50, .min_gain = min_gain,
                       .max_move = MB(4096), .max_blocks = 64};
}

static uint64_t rng_state = 0x243f6a8885a308d3ull;
static uint32_t rnd(uint32_t n) {
    rng_state ^= rng_state << 13, rng_state ^= rng_state >> 7, rng_state ^= rng_state << 17;
    return (uint32_t)(rng_state >> 33) % n;
}

static void random_tables(void) {
    enum { NB = 48, NA = 600 };
    int with_plan = 0, with_moves = 0;
    for (int round = 0; round < 400; round++) {
        TcpBlock b[NB];
        TcpAlloc a[NA];
        uint64_t used[NB] = {0}, total_free = 0;
        int nb = 4 + (int)rnd(NB - 4), na = 0;
        for (int i = 0; i < nb; i++) b[i] = B(0x1000 + (uint64_t)i, MB(8 << rnd(4))), b[i].held = rnd(16) == 0;
        for (int k = 0; k < NA; k++) {
            uint32_t i = rnd((uint32_t)nb);
            uint64_t bytes = (uint64_t)(1 + rnd(4096)) << 10;
            if (used[i] + bytes > b[i].size || rnd(8) < (i & 7)) continue;
            int what = rnd(10) == 0 ? TCP_STAY : rnd(6) == 0 ? TCP_DROP : TCP_MOVE;
            a[na++] = A(i, what, bytes), used[i] += bytes;
        }
        for (int i = 0; i < nb; i++) total_free += b[i].size - used[i];
        uint64_t unlisted = rnd(3) ? 0 : total_free / (2 + rnd(6));
        TcpParams p = P(total_free - unlisted, MB(rnd(3) * 16));
        p.max_blocks = 1 + rnd(32), p.max_move = MB(1 + rnd(512));
        if (rnd(4) == 0) p.room_pct = 10 + rnd(90);
        uint32_t order[NB];
        int max_order = 1 + (int)rnd(NB);
        TcpPlan plan;
        tcp_tally(b, nb, a, na);
        int n = tcp_plan(b, nb, &p, order, max_order, &plan);
        CHECK(n == (int)plan.n && n <= max_order && (uint32_t)n <= p.max_blocks, "round %d: %d blocks chosen", round, n);
        with_plan += n > 0, with_moves += plan.move > 0;
        uint64_t gain = 0, move = 0, drop = 0, inside = 0, last = 0;
        int marked = 0;
        for (int i = 0; i < nb; i++) {
            marked += b[i].victim == 1;
            CHECK(b[i].victim <= 1, "round %d: mark %d", round, b[i].victim);
        }
        CHECK(marked == n, "round %d: %d blocks marked, %d listed", round, marked, n);
        for (int j = 0; j < n; j++) {
            const TcpBlock *k = &b[order[j]];
            CHECK(k->victim == 1, "round %d: block %u listed and not marked", round, order[j]);
            CHECK(!k->held && !k->n_stay && (k->n_move || k->n_drop), "round %d: block %u may not be emptied", round, order[j]);
            CHECK(tcp_listed(k) * 100 <= k->size * 50, "round %d: block %u is not sparse", round, order[j]);
            CHECK(k->move >= last, "round %d: block %u takes fewer moved bytes than the one before", round, order[j]);
            last = k->move;
            gain += k->size, move += k->move, drop += k->drop, inside += k->size - tcp_listed(k);
            uint64_t outside = p.free_bytes > inside ? p.free_bytes - inside : 0;
            CHECK(move * 100 <= outside * p.room_pct, "round %d: %llu bytes to move, %llu free outside", round,
                  (unsigned long long)move, (unsigned long long)outside);
        }
        CHECK(gain == plan.gain && move == plan.move && drop == plan.drop, "round %d: the plan's sums", round);
        CHECK(move <= p.max_move, "round %d: more moved than max_move", round);
        CHECK(!n || gain >= p.min_gain, "round %d: a plan under min_gain", round);
        if (n && (uint32_t)n < p.max_blocks && n < max_order)
            for (int i = 0; i < nb; i++)
                CHECK(b[i].victim || b[i].move || !tcp_emptiable(&b[i], 50), "round %d: block %d needs no move and was left", round, i);
    }
    CHECK(with_plan >= 100 && with_moves >= 100, "only %d of the random tables gave a plan, %d one with moves", with_plan,
          with_moves);
}

int main(void) {
    {
        TcpBlock b[2] = {B(1, MB(64)), B(2, MB(64))};
        TcpAlloc a[] = {A(0, TCP_MOVE, MB(3)), A(0, TCP_MOVE, MB(1)), A(0, TCP_DROP, MB(2)), A(1, TCP_STAY, MB(40)),
                        A(1, TCP_MOVE, MB(5)), A(7, TCP_MOVE, MB(9))};
        b[0].victim = 1;
        tcp_tally(b, 2, a, 6);
        CHECK(b[0].move == MB(4) && b[0].n_move == 2 && b[0].drop == MB(2) && b[0].n_drop == 1 && !b[0].stay, "block 0's sums");
        CHECK(b[1].stay == MB(40) && b[1].n_stay == 1 && b[1].move == MB(5), "block 1's sums");
        CHECK(!b[0].victim, "the tally clears an old plan's mark");
        CHECK(tcp_listed(&b[0]) == MB(6) && tcp_listed(&b[1]) == MB(45), "listed bytes");
        CHECK(tcp_sparse(&b[0], 50) && !tcp_sparse(&b[1], 50), "6 of 64 MiB is sparse, 45 of 64 is not");
        CHECK(tcp_emptiable(&b[0], 50) && !tcp_emptiable(&b[1], 50), "only the block without a staying allocation");
        TcpStats s;
        tcp_stats(b, 2, 50, &s);
        CHECK(s.blocks == 2 && s.block_bytes == MB(128) && s.listed_bytes == MB(51), "stats: all blocks");
        CHECK(s.move_bytes == MB(9) && s.drop_bytes == MB(2) && s.stay_bytes == MB(40), "stats: by kind");
        CHECK(s.sparse == 1 && s.sparse_bytes == MB(64) && s.sparse_listed == MB(6), "stats: the sparse block");
        CHECK(s.emptiable == 1 && s.emptiable_bytes == MB(64) && s.emptiable_move == MB(4) && s.emptiable_drop == MB(2),
              "stats: the block a plan could choose");
    }
    {
        TcpBlock b[6] = {B(1, MB(64)), B(2, MB(64)), B(3, MB(64)), B(4, MB(64)), B(5, MB(64)), B(6, 0)};
        TcpAlloc a[] = {A(0, TCP_MOVE, MB(32)),
                        A(1, TCP_MOVE, MB(32)), A(1, TCP_MOVE, 1),
                        A(2, TCP_MOVE, MB(4)), A(2, TCP_STAY, 4096),
                        A(4, TCP_MOVE, MB(80)),
                        A(5, TCP_MOVE, 1)};
        tcp_tally(b, 6, a, 7);
        CHECK(tcp_emptiable(&b[0], 50), "a block exactly half full is sparse");
        CHECK(!tcp_emptiable(&b[1], 50), "a block a byte over half is not");
        CHECK(tcp_sparse(&b[2], 50) && !tcp_emptiable(&b[2], 50), "one allocation that stays keeps its block");
        CHECK(tcp_sparse(&b[3], 50) && !tcp_emptiable(&b[3], 50), "a block with nothing listed is not a plan's to empty");
        CHECK(!tcp_sparse(&b[4], 50) && !tcp_sparse(&b[5], 50), "a block listed over its size, or without one, is not sparse");
        b[0].held = 1;
        CHECK(!tcp_emptiable(&b[0], 50), "a held block is not chosen");
    }
    {
        TcpBlock b[4] = {B(0xa, MB(64)), B(0xb, MB(64)), B(0xc, MB(64)), B(0xd, MB(64))};
        TcpAlloc a[] = {A(0, TCP_STAY, MB(30)), A(0, TCP_MOVE, MB(30)),
                        A(1, TCP_MOVE, MB(8)),
                        A(2, TCP_MOVE, MB(4)), A(2, TCP_DROP, MB(2)),
                        A(3, TCP_MOVE, MB(6)), A(3, TCP_STAY, MB(1))};
        uint64_t free_bytes = MB(4 + 56 + 58 + 57);
        TcpParams p = P(free_bytes, MB(64));
        uint32_t order[8];
        TcpPlan plan;
        tcp_tally(b, 4, a, 7);
        int n = tcp_plan(b, 4, &p, order, 8, &plan);
        CHECK(n == 2 && order[0] == 2 && order[1] == 1, "blocks c and b, c first (4 MiB to move against 8)");
        CHECK(plan.gain == MB(128) && plan.move == MB(12) && plan.drop == MB(2) && plan.n_move == 2 && plan.n_drop == 1,
              "the plan's sums");
        CHECK(!b[0].victim && b[1].victim && b[2].victim && !b[3].victim, "the marks");
        p.min_gain = MB(192);
        n = tcp_plan(b, 4, &p, order, 8, &plan);
        CHECK(n == 0 && !plan.gain && !b[1].victim && !b[2].victim, "under min_gain: no plan and no mark");
        p.min_gain = 0, p.max_blocks = 1;
        n = tcp_plan(b, 4, &p, order, 8, &plan);
        CHECK(n == 1 && order[0] == 2 && plan.gain == MB(64), "max_blocks 1: the cheapest alone");
        p.max_blocks = 64;
        n = tcp_plan(b, 4, &p, order, 1, &plan);
        CHECK(n == 1 && order[0] == 2, "an order array of one: the cheapest alone");
        p.max_move = MB(5);
        n = tcp_plan(b, 4, &p, order, 8, &plan);
        CHECK(n == 1 && order[0] == 2 && !b[1].victim, "max_move 5 MiB: block b's 8 MiB do not fit after c's 4");
        p.max_move = MB(4096), b[2].held = 1;
        n = tcp_plan(b, 4, &p, order, 8, &plan);
        CHECK(n == 1 && order[0] == 1, "block c held: only b");
    }
    {
        TcpBlock b[3] = {B(1, MB(64)), B(2, MB(64)), B(3, MB(64))};
        TcpAlloc a[] = {A(0, TCP_STAY, MB(64)), A(1, TCP_MOVE, MB(30)), A(2, TCP_MOVE, MB(30))};
        TcpParams p = P(MB(34 + 34), 0);
        uint32_t order[4];
        TcpPlan plan;
        tcp_tally(b, 3, a, 3);
        CHECK(tcp_plan(b, 3, &p, order, 4, &plan) == 0, "every free byte lies in the two candidates: 30 MiB do not fit 17");
        p.free_bytes += MB(40);
        int n = tcp_plan(b, 3, &p, order, 4, &plan);
        CHECK(n == 1 && plan.gain == MB(64) && plan.move == MB(30), "74 MiB outside the first: it goes; 40 outside both: the second stays");
        CHECK(b[1].victim + b[2].victim == 1, "one of the two alike marked");
        p.room_pct = 100, p.free_bytes += MB(20);
        n = tcp_plan(b, 3, &p, order, 4, &plan);
        CHECK(n == 2 && plan.move == MB(60), "60 MiB outside both at 100 percent: both go");
    }
    {
        TcpBlock b[2] = {B(1, MB(64)), B(2, MB(64))};
        TcpAlloc a[] = {A(0, TCP_DROP, MB(20)), A(1, TCP_MOVE, MB(1))};
        TcpParams p = P(0, 0);
        uint32_t order[4];
        TcpPlan plan;
        tcp_tally(b, 2, a, 2);
        int n = tcp_plan(b, 2, &p, order, 4, &plan);
        CHECK(n == 1 && order[0] == 0 && plan.drop == MB(20) && !plan.move, "the dropped block goes, the moved one has no room");
    }
    {
        TcpBlock b[2] = {B(1, MB(16)), B(2, MB(64))};
        TcpAlloc a[] = {A(0, TCP_MOVE, MB(2)), A(1, TCP_MOVE, MB(2))};
        TcpParams p = P(MB(14 + 62 + 100), 0);
        uint32_t order[4];
        TcpPlan plan;
        tcp_tally(b, 2, a, 2);
        CHECK(tcp_plan(b, 2, &p, order, 4, &plan) == 2 && order[0] == 1, "the 64 MiB block before the 16 MiB one");
    }
    {
        enum { NB = 110 };
        TcpBlock b[NB];
        TcpAlloc a[NB * 3];
        int na = 0;
        uint64_t free_bytes = 0;
        for (int i = 0; i < NB; i++) {
            b[i] = B(0x7000 + (uint64_t)i, MB(64));
            if (i % 4 == 0) {
                a[na++] = A((uint32_t)i, TCP_MOVE, MB(6));
                if (i % 16 == 0) a[na++] = A((uint32_t)i, TCP_STAY, MB(1));
                free_bytes += MB(i % 16 == 0 ? 57 : 58);
            } else {
                a[na++] = A((uint32_t)i, TCP_MOVE, MB(30)), a[na++] = A((uint32_t)i, TCP_STAY, MB(22));
                free_bytes += MB(12);
            }
        }
        TcpParams p = P(free_bytes, MB(256));
        uint32_t order[64];
        TcpPlan plan;
        TcpStats s;
        tcp_tally(b, NB, a, na);
        tcp_stats(b, NB, 50, &s);
        CHECK(s.sparse == 28 && s.emptiable == 21, "28 sparse blocks, 21 without a mirror (%u, %u)", s.sparse, s.emptiable);
        int n = tcp_plan(b, NB, &p, order, 64, &plan);
        CHECK(n == 21 && plan.gain == MB(21 * 64) && plan.move == MB(21 * 6), "all 21 go: 126 MiB moved for 1344 MiB (%d)", n);
    }
    random_tables();
    if (fails) return printf("%d checks failed\n", fails), 1;
    printf("tex_compact_check: all checks passed\n");
    return 0;
}
