#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <zstd.h>

#include "cfg.h"
#include "host.h"
#include "platform/platform.h"
#include "sarc.h"

#define MEMBER "Component/PlayerCameraParam/PlayerCamera.game__component__PlayerCameraParam.bgyml"
#define MAX_RAW (64u << 20)

static float applied = 1.0f;
static float base[8];
static int base_n;
static float served_k = 1.0f;

float cam_speed_wanted(void) {
    double v = cfg_double("NX2SARIA_STICK_SENS");
    return (float)(v < 0.1 ? 0.1 : v > 3 ? 3 : v);
}

float cam_speed_applied(void) { return applied; }

static int u32_at(const uint8_t *p, size_t n, size_t off, uint32_t *v) {
    if (off > n || n - off < 4) return 0;
    memcpy(v, p + off, 4);
    return 1;
}

static int scale_sensitivity(uint8_t *p, size_t n, float k, float was[8]) {
    uint32_t keys, root, head;
    if (n < 16 || memcmp(p, "YB", 2) || !u32_at(p, n, 4, &keys) || !u32_at(p, n, 12, &root) ||
        !u32_at(p, n, root, &head) || (head & 0xff) != 0xc1 || !u32_at(p, n, keys, &head) || (head & 0xff) != 0xc2)
        return 0;
    uint32_t nkeys = head >> 8;
    if (!u32_at(p, n, root, &head)) return 0;
    for (uint32_t i = 0; i < head >> 8; i++) {
        uint32_t e, at, arr, ah;
        if (!u32_at(p, n, root + 4 + 8 * (size_t)i, &e) || (e & 0xffffff) >= nkeys ||
            !u32_at(p, n, keys + 4 + 4 * (size_t)(e & 0xffffff), &at))
            return 0;
        if ((size_t)keys + at + sizeof "StickSensitivity" > n) continue;
        const char *name = (const char *)p + (size_t)keys + at;
        if (strcmp(name, "StickSensitivity")) continue;
        if (e >> 24 != 0xc0 || !u32_at(p, n, root + 8 + 8 * (size_t)i, &arr) || !u32_at(p, n, arr, &ah) ||
            (ah & 0xff) != 0xc0)
            return 0;
        uint32_t count = ah >> 8;
        size_t vals = arr + 4 + ((count + 3) & ~3u);
        if (!count || count > 8 || vals + 4 * (size_t)count > n) return 0;
        for (uint32_t j = 0; j < count; j++)
            if (p[arr + 4 + j] != 0xd2) return 0;
        for (uint32_t j = 0; j < count; j++) {
            float v;
            memcpy(&v, p + vals + 4 * j, 4);
            was[j] = v;
            v *= k;
            memcpy(p + vals + 4 * j, &v, 4);
        }
        return (int)count;
    }
    return 0;
}

int cam_pack_build(const void *in, size_t n, const void *zsdic, size_t zsdic_n, void **out, size_t *out_n) {
    float k = cam_speed_wanted();
    *out = NULL;
    int ok = 0;
    uint8_t *dics = NULL, *raw = NULL;
    ZSTD_DDict *dd = NULL;
    ZSTD_CDict *cd = NULL;
    void *z = NULL;
    unsigned id = ZSTD_getDictID_fromFrame(in, n);
    size_t dn, dic_at, dic_n;
    if (!id || !(dics = zsdic_unpack(zsdic, zsdic_n, MAX_RAW, &dn)) || !zsdic_find(dics, dn, id, &dic_at, &dic_n)) {
        rt_log("camera speed: Pack/ZsDic.pack.zs has no dictionary %u for Pack/Actor/PlayerCamera.pack.zs; the camera "
               "keeps its own speed", id);
        goto done;
    }
    unsigned long long rn = ZSTD_getFrameContentSize(in, n);
    dd = ZSTD_createDDict(dics + dic_at, dic_n);
    if (!dd || rn == ZSTD_CONTENTSIZE_UNKNOWN || rn == ZSTD_CONTENTSIZE_ERROR || rn > MAX_RAW ||
        !(raw = malloc((size_t)rn))) {
        rt_log("camera speed: Pack/Actor/PlayerCamera.pack.zs is not a zstd frame with a size; the camera keeps its "
               "own speed");
        goto done;
    }
    ZSTD_DCtx *dctx = ZSTD_createDCtx();
    size_t got = dctx ? ZSTD_decompress_usingDDict(dctx, raw, (size_t)rn, in, n, dd) : 0;
    ZSTD_freeDCtx(dctx);
    size_t at, size;
    float was[8];
    int count = !ZSTD_isError(got) && got == rn && sarc_member(raw, (size_t)rn, MEMBER, &at, &size)
                    ? scale_sensitivity(raw + at, size, k, was) : 0;
    if (!count) {
        rt_log("camera speed: no StickSensitivity array in %s; the camera keeps its own speed", MEMBER);
        goto done;
    }
    memcpy(base, was, sizeof(float) * (size_t)count);
    base_n = count;
    if (k == 1.0f) goto done;
    cd = ZSTD_createCDict(dics + dic_at, dic_n, 9);
    size_t cap = ZSTD_compressBound((size_t)rn), zn = 0;
    ZSTD_CCtx *cctx = cd ? ZSTD_createCCtx() : NULL;
    if (cctx && (z = malloc(cap))) zn = ZSTD_compress_usingCDict(cctx, z, cap, raw, (size_t)rn, cd);
    ZSTD_freeCCtx(cctx);
    if (!z || !zn || ZSTD_isError(zn)) goto done;
    *out = z;
    *out_n = zn;
    z = NULL;
    ok = 1;
    rt_log("camera speed: x%.2f, StickSensitivity %.2f %.2f %.2f %.2f %.2f -> %.2f %.2f %.2f %.2f %.2f", k, was[0],
           count > 1 ? was[1] : 0, count > 2 ? was[2] : 0, count > 3 ? was[3] : 0, count > 4 ? was[4] : 0, was[0] * k,
           count > 1 ? was[1] * k : 0, count > 2 ? was[2] * k : 0, count > 3 ? was[3] * k : 0,
           count > 4 ? was[4] * k : 0);
done:
    free(z);
    free(raw);
    free(dics);
    ZSTD_freeDDict(dd);
    ZSTD_freeCDict(cd);
    return ok;
}

void cam_pack_served(void) { applied = served_k = cam_speed_wanted(); }

#define COPIES 16
static uint64_t copies[COPIES];
static int ncopies;

static void factor(float out[8], float k) {
    for (int i = 0; i < base_n; i++) out[i] = base[i], out[i] *= k;
}

static int rewrite(float k, float to, int fresh) {
    enum { PIECE = 16 << 20, STEP = 64 << 10 };
    static int told;
    uint64_t left_out = 0;
    float from[8], into[8];
    factor(from, k);
    factor(into, to);
    const size_t len = sizeof(float) * (size_t)base_n;
    const uint8_t first = *(const uint8_t *)from;
    const RtGuestLayout *l = mem_layout();
    const RtRange ranges[2] = {l->heap, l->alias};
    int found = 0;
    if (fresh) ncopies = 0;
    for (int r = 0; r < 2; r++) {
        uint64_t a = ranges[r].base, end = ranges[r].base + ranges[r].size;
        while (a < end) {
            RtMemoryInfo mi;
            if (mem_query(a, &mi) || !mi.size) break;
            uint64_t stop = mi.base + mi.size < end ? mi.base + mi.size : end;
            if (!mi.state || (mi.perm & 3) != 3) {
                a = stop;
                continue;
            }
            while (a < stop) {
                uint64_t skip = mem_untouched_run(a, stop);
                if (skip) {
                    a = skip;
                    continue;
                }
                uint64_t n = stop - a < PIECE ? stop - a : PIECE;
                if (mem_lazy_touches(a, n)) {
                    if (n > STEP) n = STEP;
                    if (mem_lazy_touches(a, n)) {
                        a += n;
                        left_out += n;
                        continue;
                    }
                }
                if (n >= len && mem_read_hold(a, n)) {
                    const uint8_t *p = (const uint8_t *)(uintptr_t)a, *e = p + n;
                    for (const uint8_t *q = p; q + len <= e && (q = memchr(q, first, (size_t)(e - q) - len + 1)); q++) {
                        if (memcmp(q, from, len)) continue;
                        mem_thaw((uint64_t)(uintptr_t)q, len);
                        memcpy((void *)(uintptr_t)q, into, len);
                        if (ncopies < COPIES) copies[ncopies++] = (uint64_t)(uintptr_t)q;
                        found++;
                    }
                    mem_read_release();
                }
                a += a + n >= stop ? n : n - (len - 1);
            }
        }
    }
    if (left_out && !told) {
        told = 1;
        rt_log("camera speed: the search left out %.1f MiB the GPU wrote and the CPU has not read since (lazy "
               "write-back; with NX2SARIA_WB_PROBE all of it)", left_out / 1048576.0);
    }
    return found;
}

static int rewrite_known(float k, float to) {
    float from[8], into[8];
    factor(from, k);
    factor(into, to);
    const size_t len = sizeof(float) * (size_t)base_n;
    if (!ncopies) return 0;
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < ncopies; i++) {
            if (!mem_read_hold(copies[i], len)) return 0;
            int same = !memcmp((const void *)(uintptr_t)copies[i], pass ? into : from, len) ||
                       (pass && !memcmp((const void *)(uintptr_t)copies[i], from, len));
            if (same && pass) {
                mem_thaw(copies[i], len);
                memcpy((void *)(uintptr_t)copies[i], into, len);
            }
            mem_read_release();
            if (!same) return 0;
        }
    return 1;
}

static volatile int32_t searching;
static volatile int32_t reopened;
static uint64_t due_ns;
static int tries;
static float asked = 1.0f;

static void search_main(void *arg) {
    (void)arg;
    float to = cam_speed_wanted(), was = applied;
    uint64_t t0 = plat_now_ns();
    int n = to != was ? rewrite(was, to, 1) : 0;
    if (served_k != was && served_k != to) n += rewrite(served_k, to, 0);
    if (n) {
        applied = to;
        rt_log("camera speed: x%.2f now, written over %d cop%s of the game's StickSensitivity in memory (%.0f ms)", to, n,
               n == 1 ? "y" : "ies", (plat_now_ns() - t0) / 1e6);
    } else if (to != was) {
        rt_log("camera speed: x%.2f asked for, but the game's StickSensitivity (x%.2f) is not in memory%s", to, was,
               tries ? "; looking again in a moment" : ": the camera keeps its speed until the game loads it");
    }
    plat_xchg32(&searching, 0);
}

void cam_pack_opened(void) { plat_xchg32(&reopened, 1); }

void cam_speed_tick(void) {
    if (!base_n) return;
    float want = cam_speed_wanted();
    if (plat_xchg32(&reopened, 0) && want != served_k) due_ns = plat_now_ns() + 1500000000ull, tries = 3;
    if (want == applied && !due_ns) return;
    if (plat_cas32(&searching, 0, 0)) return;
    uint64_t now = plat_now_ns();
    if (want != applied) {
        if (rewrite_known(applied, want)) {
            applied = asked = want;
            return;
        }
        if (want != asked) asked = want, due_ns = now, tries = 3;
    }
    if (!due_ns || now < due_ns) return;
    due_ns = 0;
    if (tries > 0 && want != applied) tries--, due_ns = now + 4000000000ull;
    plat_xchg32(&searching, 1);
    if (!plat_thread_spawn(search_main, NULL, "camera speed", 0)) plat_xchg32(&searching, 0);
}
