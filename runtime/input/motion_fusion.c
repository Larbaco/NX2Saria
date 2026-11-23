// Contains code Copyright (c) 2026 MacNativeGames contributors (MIT; see THIRD_PARTY_NOTICES.md).
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "input.h"
#include "platform/platform.h"

enum { MOTION_RING = 64, MOTION_CACHE = 4, LOG_LINE = 320 };
#define STALE_NS 200000000ull
#define REST_TAU 0.2
#define STILL_GYRO_VAR (0.015 * 0.015)
#define STILL_ACC_VAR (0.0173 * 0.0173)
#define STILL_ACC_G 0.1
#define STILL_MAX_RATE 0.15
#define STILL_BIAS_STEP 0.035
#define RELEARN_MS 3000.0
#define SEED_VAR 0.01
#define LEARN_AFTER_MS 500.0
#define LEARN_FAST_MS 2000.0
#define LEARN_TAU_FAST 0.25
#define LEARN_TAU_SLOW 3.0
#define LEARN_LOG_MS 1500.0
#define EI_MAX 0.1
#define NO_STILL_S 30.0
#define AGG_FIRST_S 60.0
#define AGG_EVERY_S 300.0
#define RAD2DEG (180.0 / M_PI)

enum { CAP_BIAS = 24, CAP_SOURCE = 16, CAP_NO_STILL = 16, CAP_AGG = 48 };
static unsigned log_bias_n, log_source_n, log_no_still_n, log_agg_n;
static int log_take(unsigned *n, unsigned cap) { return __atomic_fetch_add(n, 1, __ATOMIC_RELAXED) < cap; }

typedef struct {
    uint64_t id, used;
    double bias[3], learned_ms;
    unsigned periods;
} MotionCached;

struct MotionFusion {
    PlatMutex lock;
    MotionState ring[MOTION_RING];
    uint64_t produced, last_ns;
    uint64_t first;
    double R[3][3];
    double Rout[3][3];
    double psi_prev, psi_out;
    double swing_axis[3];
    double angle[3];
    int restart;
    MotionOptions opt;
    double bias[3], learned_ms;
    unsigned periods;
    double ei[3];
    int rest_valid;
    double mean_g[3], mean_a[3], var_g, var_a, mean_an, still_ms;
    int period_logged;
    int near_still;
    double off_bias_ms;
    uint64_t source_id, switches;
    char name[64];
    double source_fed_s;
    int no_still_logged;
    MotionCached cache[MOTION_CACHE];
    double fed_s, agg_next_s, w_fed_s, w_still_s;
    unsigned w_periods, w_learned;
    uint64_t w_still_n, w_moving_n, w_zeroed_n;
    double w_raw_mag, w_out_mag, w_raw_turn, w_out_turn;
};

MotionFusion *motion_new(void) {
    MotionFusion *m = calloc(1, sizeof *m);
    if (!m) rt_fatal("motion: out of memory");
    plat_mutex_init(&m->lock);
    m->restart = 1;
    m->opt = MOTION_OPTIONS_DEFAULT;
    m->swing_axis[0] = 1;
    for (int i = 0; i < 3; i++) m->R[i][i] = m->Rout[i][i] = 1;
    snprintf(m->name, sizeof m->name, "controller");
    m->agg_next_s = AGG_FIRST_S;
    return m;
}

static MotionFusion *global;

static void global_init(void *arg) {
    (void)arg;
    global = motion_new();
}

MotionFusion *motion_global(void) {
    static PlatOnce once = PLAT_ONCE_INIT;
    plat_once(&once, global_init, NULL);
    return global;
}

static double dot3(const double a[3], const double b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static double norm3(const double v[3]) { return sqrt(dot3(v, v)); }
static void cross3(const double a[3], const double b[3], double out[3]) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}
static void orthonormalize(double R[3][3]) {
    double n = norm3(R[2]);
    for (int j = 0; j < 3; j++) R[2][j] /= n;
    double d = R[0][0] * R[2][0] + R[0][1] * R[2][1] + R[0][2] * R[2][2];
    for (int j = 0; j < 3; j++) R[0][j] -= d * R[2][j];
    n = norm3(R[0]);
    for (int j = 0; j < 3; j++) R[0][j] /= n;
    cross3(R[2], R[0], R[1]);
}
static void align_to(double R[3][3], const double acc[3]) {
    double n = norm3(acc), up[3] = {0, 0, 1}, x[3] = {1, 0, 0};
    if (n > 0.5)
        for (int j = 0; j < 3; j++) up[j] = -acc[j] / n;
    if (fabs(up[0]) > 0.9) { x[0] = 0; x[1] = 1; }
    memcpy(R[2], up, sizeof up);
    memcpy(R[0], x, sizeof x);
    orthonormalize(R);
}
static void rotate(double R[3][3], const double w[3], double dt) {
    double rate = norm3(w), theta = rate * dt;
    if (theta < 1e-12) return;
    double k[3] = {w[0] / rate, w[1] / rate, w[2] / rate}, s = sin(theta), c = 1 - cos(theta);
    double K[3][3] = {{0, -k[2], k[1]}, {k[2], 0, -k[0]}, {-k[1], k[0], 0}}, E[3][3], out[3][3];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            double kk = 0;
            for (int q = 0; q < 3; q++) kk += K[i][q] * K[q][j];
            E[i][j] = (i == j) + s * K[i][j] + c * kk;
        }
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            double v = 0;
            for (int q = 0; q < 3; q++) v += R[i][q] * E[q][j];
            out[i][j] = v;
        }
    memcpy(R, out, sizeof out);
    orthonormalize(R);
}
static void correction(MotionFusion *m, const double acc[3], double dt, double c[3]) {
    double n = norm3(acc), e[3] = {0, 0, 0};
    int gate = fabs(n - 1) < 0.15;
    if (gate) {
        double measured[3] = {acc[0] / n, acc[1] / n, acc[2] / n};
        double estimated[3] = {-m->R[2][0], -m->R[2][1], -m->R[2][2]};
        cross3(measured, estimated, e);
        for (int j = 0; j < 3; j++) {
            m->ei[j] += MOTION_KI * e[j] * dt;
            m->ei[j] = m->ei[j] > EI_MAX ? EI_MAX : m->ei[j] < -EI_MAX ? -EI_MAX : m->ei[j];
        }
    }
    for (int j = 0; j < 3; j++) c[j] = (gate ? MOTION_KP * e[j] : 0) + m->ei[j];
}

static void axis_angle(const double k[3], double a, double M[3][3]) {
    double s = sin(a), c = 1 - cos(a), K[3][3] = {{0, -k[2], k[1]}, {k[2], 0, -k[0]}, {-k[1], k[0], 0}};
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            double kk = 0;
            for (int q = 0; q < 3; q++) kk += K[i][q] * K[q][j];
            M[i][j] = (i == j) + s * K[i][j] + c * kk;
        }
}
static void report_orientation(MotionFusion *m, const MotionOptions *o, int fresh) {
    const double *u = m->R[2];
    double h = hypot(u[0], u[1]), theta = atan2(h, u[2]), S[3][3];
    if (h > 1e-6) m->swing_axis[0] = u[1] / h, m->swing_axis[1] = -u[0] / h, m->swing_axis[2] = 0;
    axis_angle(m->swing_axis, theta, S);
    double psi = atan2(dot3(m->R[1], S[0]), dot3(m->R[0], S[0]));
    int unscaled = o->turn == 1 && o->tilt == 1;
    if (fresh || unscaled) m->psi_out = psi;
    else {
        double d = psi - m->psi_prev;
        d -= 2 * M_PI * floor((d + M_PI) / (2 * M_PI));
        m->psi_out = remainder(m->psi_out + o->turn * d, 2 * M_PI);
    }
    m->psi_prev = psi;
    if (unscaled) {
        memcpy(m->Rout, m->R, sizeof m->R);
        return;
    }
    double So[3][3], c = cos(m->psi_out), s = sin(m->psi_out);
    axis_angle(m->swing_axis, theta * o->tilt, So);
    for (int j = 0; j < 3; j++) {
        m->Rout[0][j] = c * So[0][j] - s * So[1][j];
        m->Rout[1][j] = s * So[0][j] + c * So[1][j];
        m->Rout[2][j] = So[2][j];
    }
}

static void rest_reset(MotionFusion *m) {
    m->rest_valid = 0;
    m->still_ms = 0;
    m->period_logged = 0;
    m->off_bias_ms = 0;
}
static int rest_update(MotionFusion *m, const double acc[3], const double gyro[3], double dt) {
    double an = norm3(acc);
    if (!m->rest_valid) {
        memcpy(m->mean_g, gyro, sizeof m->mean_g);
        memcpy(m->mean_a, acc, sizeof m->mean_a);
        m->mean_an = an;
        m->var_g = m->var_a = SEED_VAR;
        m->rest_valid = 1;
    } else {
        double k = dt / (REST_TAU + dt), dg = 0, da = 0;
        for (int j = 0; j < 3; j++) {
            m->mean_g[j] += k * (gyro[j] - m->mean_g[j]);
            m->mean_a[j] += k * (acc[j] - m->mean_a[j]);
            dg += (gyro[j] - m->mean_g[j]) * (gyro[j] - m->mean_g[j]);
            da += (acc[j] - m->mean_a[j]) * (acc[j] - m->mean_a[j]);
        }
        m->mean_an += k * (an - m->mean_an);
        m->var_g += k * (dg - m->var_g);
        m->var_a += k * (da - m->var_a);
    }
    double off[3] = {m->mean_g[0] - m->bias[0], m->mean_g[1] - m->bias[1], m->mean_g[2] - m->bias[2]};
    m->near_still = m->var_g < STILL_GYRO_VAR && m->var_a < STILL_ACC_VAR && fabs(m->mean_an - 1) < STILL_ACC_G &&
                    norm3(m->mean_g) < STILL_MAX_RATE;
    return m->near_still && (m->learned_ms < LEARN_FAST_MS || norm3(off) < STILL_BIAS_STEP);
}

typedef struct { int n; char line[3][LOG_LINE]; } Lines;
#define ADD_LINE(L, ...) snprintf((L)->line[(L)->n++], LOG_LINE, __VA_ARGS__)

static void aggregate(MotionFusion *m, Lines *L) {
    double n = m->w_still_n ? (double)m->w_still_n : 1;
    if (log_take(&log_agg_n, CAP_AGG))
        ADD_LINE(L,
                 "motion: last %.0f s: still %.1f s in %u periods, drift while still %.2f deg/s raw -> %.2f deg/s "
                 "reported, turn drift %.1f deg/min raw -> %.1f reported, bias learned %u times, dead zone held %.1f %% "
                 "of moving samples",
                 m->w_fed_s, m->w_still_s, m->w_periods, m->w_raw_mag / n, m->w_out_mag / n, m->w_raw_turn / n * 60,
                 m->w_out_turn / n * 60, m->w_learned,
                 m->w_moving_n ? 100.0 * (double)m->w_zeroed_n / (double)m->w_moving_n : 0.0);
    m->w_fed_s = m->w_still_s = m->w_raw_mag = m->w_out_mag = m->w_raw_turn = m->w_out_turn = 0;
    m->w_periods = m->w_learned = 0;
    m->w_still_n = m->w_moving_n = m->w_zeroed_n = 0;
}

void motion_feed(MotionFusion *m, const double acc[3], const double gyro[3], uint64_t now) {
    Lines L = {0};
    plat_mutex_lock(&m->lock);
    double dt = m->last_ns && now > m->last_ns ? (double)(now - m->last_ns) / 1e9 : 0.004;
    if (dt > 0.05) {
        dt = 0.004;
        rest_reset(m);
    }
    m->last_ns = now;
    const MotionOptions o = m->opt;
    int finite = 1;
    for (int j = 0; j < 3; j++) finite &= isfinite(acc[j]) && isfinite(gyro[j]);

    int still = finite ? rest_update(m, acc, gyro, dt) : m->still_ms > 0;
    if (finite && m->near_still && !still) {
        m->off_bias_ms += dt * 1000;
        if (m->off_bias_ms >= RELEARN_MS) {
            m->off_bias_ms = 0;
            m->learned_ms = 0;
            if (log_take(&log_bias_n, CAP_BIAS))
                ADD_LINE(&L, "motion: bias relearned from scratch: still for %u ms away from the learned bias (\"%s\")",
                         (unsigned)RELEARN_MS, m->name);
        }
    } else if (finite) m->off_bias_ms = 0;
    if (finite && still) {
        if (m->still_ms <= 0) m->w_periods++;
        m->still_ms += dt * 1000;
    } else if (finite) {
        m->still_ms = 0;
        m->period_logged = 0;
    }
    int learning = finite && o.calibrate && still && m->still_ms >= LEARN_AFTER_MS;
    if (learning) {
        memset(m->ei, 0, sizeof m->ei);
        double tau = m->learned_ms < LEARN_FAST_MS ? LEARN_TAU_FAST : LEARN_TAU_SLOW, k2 = dt / (tau + dt);
        for (int j = 0; j < 3; j++) m->bias[j] += k2 * (m->mean_g[j] - m->bias[j]);
        m->learned_ms += dt * 1000;
        if (m->still_ms >= LEARN_LOG_MS && !m->period_logged) {
            m->period_logged = 1;
            m->periods++;
            m->w_learned++;
            if (log_take(&log_bias_n, CAP_BIAS))
                ADD_LINE(&L, "motion: bias learned %+.2f/%+.2f/%+.2f deg/s after %u ms still (\"%s\", calibration %u)",
                         m->bias[0] * RAD2DEG, m->bias[1] * RAD2DEG, m->bias[2] * RAD2DEG, (unsigned)m->still_ms,
                         m->name, m->periods);
        }
    }

    double w[3] = {0, 0, 0}, a[3] = {0, 0, 0};
    if (finite)
        for (int j = 0; j < 3; j++) w[j] = gyro[j] - (o.calibrate ? m->bias[j] : 0), a[j] = acc[j];
    if (finite && norm3(w) < o.deadzone_rps * 2 * M_PI) {
        if (!still && norm3(w) > 0) m->w_zeroed_n++;
        w[0] = w[1] = w[2] = 0;
    }

    int unscaled = o.turn == 1 && o.tilt == 1;
    if (finite && m->restart) {
        align_to(m->R, acc);
        report_orientation(m, &o, 1);
        memset(m->angle, 0, sizeof m->angle);
        m->restart = 0;
    } else if (finite) {
        double c[3], wc[3];
        correction(m, acc, dt, c);
        for (int j = 0; j < 3; j++) wc[j] = w[j] + c[j];
        rotate(m->R, wc, dt);
        report_orientation(m, &o, 0);
    }
    const double *up = m->R[2];
    double w_out[3], wu = dot3(w, up);
    if (unscaled) memcpy(w_out, w, sizeof w);
    else
        for (int j = 0; j < 3; j++) w_out[j] = o.turn * wu * up[j] + o.tilt * (w[j] - wu * up[j]);

    MotionState *s = &m->ring[m->produced % MOTION_RING];
    *s = (MotionState){.delta_ns = (int64_t)(dt * 1e9), .sampling = (int64_t)m->produced + 1, .attributes = 1};
    for (int i = 0; i < 3; i++) {
        double rps = w_out[i] / (2 * M_PI);
        m->angle[i] += rps * dt;
        s->acc[i] = (float)a[i];
        s->gyro[i] = (float)rps;
        s->angle[i] = (float)m->angle[i];
        for (int j = 0; j < 3; j++) s->dir[i * 3 + j] = (float)m->Rout[j][i];
    }
    m->produced++;

    m->fed_s += dt;
    m->w_fed_s += dt;
    m->source_fed_s += dt;
    if (finite && still) {
        m->w_still_n++;
        m->w_still_s += dt;
        m->w_raw_mag += norm3(gyro) * RAD2DEG;
        m->w_out_mag += norm3(w_out) * RAD2DEG;
        m->w_raw_turn += dot3(gyro, up) * RAD2DEG;
        m->w_out_turn += dot3(w_out, up) * RAD2DEG;
    } else if (finite) m->w_moving_n++;
    if (o.calibrate && m->learned_ms <= 0 && m->source_fed_s >= NO_STILL_S && !m->no_still_logged) {
        m->no_still_logged = 1;
        if (log_take(&log_no_still_n, CAP_NO_STILL))
            ADD_LINE(&L, "motion: no still moment yet on \"%s\": set the controller down for a second so its gyro bias is learned",
                     m->name);
    }
    if (m->fed_s >= m->agg_next_s) {
        m->agg_next_s += AGG_EVERY_S;
        aggregate(m, &L);
    }
    plat_mutex_unlock(&m->lock);
    for (int i = 0; i < L.n; i++) rt_log("%s", L.line[i]);
}

int motion_read(MotionFusion *m, MotionState *out, int max, uint64_t now) {
    if (max <= 0) return 0;
    int n = 0;
    plat_mutex_lock_shared(&m->lock);
    if (m->produced > m->first && (now < m->last_ns || now - m->last_ns < STALE_NS)) {
        uint64_t have = m->produced - m->first < MOTION_RING ? m->produced - m->first : MOTION_RING;
        n = (int)(have < (uint64_t)max ? have : (uint64_t)max);
        for (int i = 0; i < n; i++) out[i] = m->ring[(m->produced - 1 - (uint64_t)i) % MOTION_RING];
    }
    plat_mutex_unlock_shared(&m->lock);
    return n;
}

void motion_restart(MotionFusion *m) {
    plat_mutex_lock(&m->lock);
    m->restart = 1;
    m->first = m->produced;
    m->last_ns = 0;
    memset(m->angle, 0, sizeof m->angle);
    memset(m->ei, 0, sizeof m->ei);
    rest_reset(m);
    plat_mutex_unlock(&m->lock);
}

void motion_set_options(MotionFusion *m, const MotionOptions *o) {
    plat_mutex_lock(&m->lock);
    m->opt = *o;
    plat_mutex_unlock(&m->lock);
}

void motion_source(MotionFusion *m, uint64_t id, const char *name) {
    char line[LOG_LINE] = "";
    plat_mutex_lock(&m->lock);
    if (id == m->source_id) {
        plat_mutex_unlock(&m->lock);
        return;
    }
    m->switches++;
    if (m->source_id) {
        MotionCached *slot = NULL;
        for (int i = 0; i < MOTION_CACHE && !slot; i++)
            if (m->cache[i].id == m->source_id) slot = &m->cache[i];
        for (int i = 0; i < MOTION_CACHE && !slot; i++)
            if (!m->cache[i].id) slot = &m->cache[i];
        if (!slot) {
            slot = &m->cache[0];
            for (int i = 1; i < MOTION_CACHE; i++)
                if (m->cache[i].used < slot->used) slot = &m->cache[i];
        }
        *slot = (MotionCached){.id = m->source_id, .used = m->switches, .learned_ms = m->learned_ms, .periods = m->periods};
        memcpy(slot->bias, m->bias, sizeof slot->bias);
    }
    memset(m->bias, 0, sizeof m->bias);
    m->learned_ms = 0;
    m->periods = 0;
    for (int i = 0; i < MOTION_CACHE; i++)
        if (id && m->cache[i].id == id) {
            memcpy(m->bias, m->cache[i].bias, sizeof m->bias);
            m->learned_ms = m->cache[i].learned_ms;
            m->periods = m->cache[i].periods;
        }
    m->source_id = id;
    snprintf(m->name, sizeof m->name, "%s", name && *name ? name : "controller");
    m->source_fed_s = 0;
    m->no_still_logged = 0;
    rest_reset(m);
    memset(m->ei, 0, sizeof m->ei);
    m->restart = 1;
    unsigned k = __atomic_fetch_add(&log_source_n, 1, __ATOMIC_RELAXED);
    if (k < CAP_SOURCE) {
        if (m->learned_ms > 0)
            snprintf(line, sizeof line, "motion: sensors of \"%s\": bias %+.2f/%+.2f/%+.2f deg/s restored (learned %u ms)",
                     m->name, m->bias[0] * RAD2DEG, m->bias[1] * RAD2DEG, m->bias[2] * RAD2DEG, (unsigned)m->learned_ms);
        else snprintf(line, sizeof line, "motion: sensors of \"%s\": no bias learned yet", m->name);
    } else if (k == CAP_SOURCE)
        snprintf(line, sizeof line, "motion: further changes of the sensors' controller are not logged");
    plat_mutex_unlock(&m->lock);
    if (*line) rt_log("%s", line);
}
