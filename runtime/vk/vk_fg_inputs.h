#pragma once
#include <stdint.h>
#include <string.h>

#include "platform/platform.h"
#include "vk_fg_set.h"

#define FGI_MAX_AGE 4

typedef struct {
    PlatMutex m;
    VkDlaaFgSet set;
    VkDlaaFgHudless hl;
    uint64_t set_seq, hl_seq, set_taken, hl_taken;
    int has_set, has_hl;
} FgiBox;

static inline uint64_t fgi_age_(uint64_t now, uint64_t made) { return now > made ? now - made - 1 : 0; }

static inline void fgi_put(FgiBox *b, const VkDlaaFgSet *v) {
    plat_mutex_lock(&b->m);
    b->set = *v, b->set.age = 0, b->set_seq++, b->has_set = 1;
    plat_mutex_unlock(&b->m);
}

static inline void fgi_put_hudless(FgiBox *b, const VkDlaaFgHudless *v) {
    plat_mutex_lock(&b->m);
    b->hl = *v, b->hl.age = 0, b->hl_seq++, b->has_hl = 1;
    plat_mutex_unlock(&b->m);
}

static inline void fgi_withdraw(FgiBox *b, int set, int hudless) {
    plat_mutex_lock(&b->m);
    if (set) b->has_set = 0;
    if (hudless) b->has_hl = 0;
    plat_mutex_unlock(&b->m);
}

static inline int fgi_take(FgiBox *b, uint64_t now, VkDlaaFgSet *out) {
    plat_mutex_lock(&b->m);
    int ok = b->has_set && b->set_seq != b->set_taken;
    if (ok) {
        b->set_taken = b->set_seq;
        ok = now - b->set.made <= FGI_MAX_AGE;
    }
    if (ok) *out = b->set, out->age = fgi_age_(now, b->set.made);
    plat_mutex_unlock(&b->m);
    return ok;
}

static inline int fgi_take_hudless(FgiBox *b, uint64_t now, VkDlaaFgHudless *out) {
    plat_mutex_lock(&b->m);
    int ok = b->has_hl && b->hl_seq != b->hl_taken;
    if (ok) {
        b->hl_taken = b->hl_seq;
        ok = now - b->hl.made <= FGI_MAX_AGE;
    }
    if (ok) *out = b->hl, out->age = fgi_age_(now, b->hl.made);
    plat_mutex_unlock(&b->m);
    return ok;
}

static inline int fgi_camera(FgiBox *b, VkDlaaCamera *out) {
    plat_mutex_lock(&b->m);
    const int ok = b->has_set && b->set.cam_ok;
    if (ok) *out = b->set.cam;
    plat_mutex_unlock(&b->m);
    return ok;
}
