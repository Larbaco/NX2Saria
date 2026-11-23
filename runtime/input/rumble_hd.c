#include <math.h>
#include <string.h>

#include "rumble_hd.h"

const uint8_t rumble_hd_neutral[RUMBLE_HD_DATA] = {0x00, 0x01, 0x40, 0x40, 0x00, 0x01, 0x40, 0x40};

#define AMP_STEP1 0.007843f
#define AMP_STEP2 0.011823f

float rumble_hd_step_amp(int step) {
    if (step <= 0) return 0;
    if (step == 1) return AMP_STEP1;
    if (step < 16) return AMP_STEP2 * powf(2, (float)(step - 2) / 4);
    if (step < 32) return powf(2, (float)step / 16) / 17;
    if (step > RUMBLE_HD_AMP_STEPS) step = RUMBLE_HD_AMP_STEPS;
    return powf(2, (float)step / 32) / 8.7f;
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

int rumble_hd_amp_step(float a) {
    if (!(a >= AMP_STEP1 / 2)) return 0;
    if (a >= 1) return RUMBLE_HD_AMP_STEPS;
    if (a < 0.00963f) return 1;
    if (a < 0.1150f) return clampi(2 + (int)lroundf(4 * log2f(a / AMP_STEP2)), 2, 15);
    if (a < 0.2276f) return clampi((int)lroundf(16 * log2f(a * 17)), 16, 31);
    return clampi((int)lroundf(32 * log2f(a * 8.7f)), 32, RUMBLE_HD_AMP_STEPS);
}

static int freq_step(float f, int lo, int hi) {
    if (!(f > 0)) return lo;
    float n = 32 * log2f(f / 10);
    return n <= (float)lo ? lo : n >= (float)hi ? hi : clampi((int)lroundf(n), lo, hi);
}

void rumble_hd_encode(const RumbleHdValue *v, float strength, uint8_t out[RUMBLE_HD_SIDE]) {
    float s = strength > 0 ? strength : 0;
    int la = rumble_hd_amp_step(v->amp_low * s), ha = rumble_hd_amp_step(v->amp_high * s);
    if (!la && !ha) { memcpy(out, rumble_hd_neutral, RUMBLE_HD_SIDE); return; }
    int lf = freq_step(v->freq_low, 0x41, 0xBF) - 0x40, hf = (freq_step(v->freq_high, 0x61, 0xDF) - 0x60) * 4;
    out[0] = (uint8_t)(hf & 0xff);
    out[1] = (uint8_t)((hf >> 8) | ha << 1);
    out[2] = (uint8_t)(lf | (la & 1) << 7);
    out[3] = (uint8_t)(0x40 + (la >> 1));
}

void rumble_hd_data(const RumbleHdValue v[2], const int live[2], float strength, int one, uint8_t out[RUMBLE_HD_DATA]) {
    memcpy(out, rumble_hd_neutral, RUMBLE_HD_DATA);
    float power[2] = {0, 0};
    for (int s = 0; s < 2; s++) {
        if (!live[s]) continue;
        rumble_hd_encode(&v[s], strength, out + s * RUMBLE_HD_SIDE);
        float al = v[s].amp_low > 0 ? v[s].amp_low : 0, ah = v[s].amp_high > 0 ? v[s].amp_high : 0;
        power[s] = al * al + ah * ah;
    }
    if (!one) return;
    int pick = power[1] > power[0];
    memcpy(out + (1 - pick) * RUMBLE_HD_SIDE, out + pick * RUMBLE_HD_SIDE, RUMBLE_HD_SIDE);
}

void rumble_hd_report(const uint8_t data[RUMBLE_HD_DATA], unsigned number, uint8_t *out, unsigned n) {
    memset(out, 0, n);
    out[0] = RUMBLE_HD_REPORT_ID;
    out[1] = (uint8_t)(number & 0xf);
    memcpy(out + 2, data, RUMBLE_HD_DATA);
}

static int is_neutral(const uint8_t d[RUMBLE_HD_DATA]) { return !memcmp(d, rumble_hd_neutral, RUMBLE_HD_DATA); }

static const uint8_t *target(const RumbleHd *h, int on, uint64_t now) {
    return on && h->have && now - h->want_at <= RUMBLE_HD_STALE_NS ? h->want : rumble_hd_neutral;
}

void rumble_hd_post(RumbleHd *h, const uint8_t data[RUMBLE_HD_DATA], uint64_t now) {
    memcpy(h->want, data, RUMBLE_HD_DATA);
    h->want_at = now;
    h->have = 1;
}

int rumble_hd_due(const RumbleHd *h, int on, uint64_t now, uint64_t gap_ns, uint8_t out[RUMBLE_HD_DATA]) {
    const uint8_t *t = target(h, on, now);
    int due;
    if (!h->any) due = !is_neutral(t);
    else if (memcmp(t, h->sent, RUMBLE_HD_DATA)) due = now - h->sent_at >= gap_ns;
    else if (is_neutral(t)) due = h->neutral_left > 0 && now - h->sent_at >= gap_ns;
    else due = now - h->sent_at >= RUMBLE_HD_REFRESH_NS;
    if (due) memcpy(out, t, RUMBLE_HD_DATA);
    return due;
}

void rumble_hd_sent(RumbleHd *h, const uint8_t data[RUMBLE_HD_DATA], uint64_t now) {
    if (!is_neutral(data)) h->neutral_left = 0;
    else if (!h->any || !is_neutral(h->sent)) h->neutral_left = RUMBLE_HD_NEUTRAL_REPORTS - 1;
    else if (h->neutral_left > 0) h->neutral_left--;
    memcpy(h->sent, data, RUMBLE_HD_DATA);
    h->sent_at = now;
    h->any = 1;
}

int rumble_hd_idle(const RumbleHd *h, int on, uint64_t now) {
    if (!is_neutral(target(h, on, now))) return 0;
    return !h->any || (is_neutral(h->sent) && h->neutral_left == 0);
}

int rumble_hd_buzzing(const RumbleHd *h) { return h->any && !is_neutral(h->sent); }

void rumble_hd_forget(RumbleHd *h) {
    h->any = 0;
    h->neutral_left = 0;
    h->sent_at = 0;
    memset(h->sent, 0, sizeof h->sent);
}
