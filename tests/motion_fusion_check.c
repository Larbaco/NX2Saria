// runtime/input/motion_fusion.c on made-up sensor samples, 4 ms apart, with a stand-in for the log. Built and run by
// tests/motion_fusion_check.py; prints a PASS or FAIL line per case and each failed check, and exits 1 on any.
#include "motion_fusion.c"

#include <stdarg.h>
#include <stdio.h>

// ---- the log: printed, and the lines kept for the checks
enum { MAX_LINES = 512 };
static char lines[MAX_LINES][LOG_LINE + 32];
static int nlines;
void rt_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char buf[sizeof lines[0]];
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    printf("  log: %s\n", buf);
    if (nlines < MAX_LINES) memcpy(lines[nlines++], buf, sizeof buf);
}
void rt_fatal(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    exit(2);
}
const char *cfg_env(const char *name);   // the platform layer's settings: none here
const char *cfg_env(const char *name) { (void)name; return NULL; }
// the lines since index `from` that contain `what`
static int count_lines(int from, const char *what) {
    int n = 0;
    for (int i = from; i < nlines; i++) n += strstr(lines[i], what) != NULL;
    return n;
}
static const char *find_line(int from, const char *what) {
    for (int i = from; i < nlines; i++)
        if (strstr(lines[i], what)) return lines[i];
    return NULL;
}

static int fails, case_fails;
#define CHECK(c, ...) do { if (!(c)) { case_fails++; printf("  FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
static void begin(const char *what) { case_fails = 0; printf("case: %s\n", what); }
static void end(const char *what) {
    printf("%s: %s\n", case_fails ? "FAIL" : "PASS", what);
    fails += case_fails != 0;
}

// ---- the simulated controller: its true orientation T (controller -> world, z up), rotated by the true rate
#define DEG (M_PI / 180)
#define STEP_NS 4000000ull
typedef struct {
    MotionFusion *m;
    uint64_t t;
    double T[3][3];
    double bias[3], gyro_noise, acc_noise;   // rad/s, rad/s RMS per axis, G RMS per axis
} Sim;
static uint64_t rng = 0x9e3779b97f4a7c15ull;
static double uniform(void) {
    rng ^= rng << 13, rng ^= rng >> 7, rng ^= rng << 17;
    return ((rng >> 11) + 0.5) / 9007199254740992.0;
}
static double gauss(void) { return sqrt(-2 * log(uniform())) * cos(2 * M_PI * uniform()); }
static void sim_new(Sim *s, const MotionOptions *o) {
    *s = (Sim){.m = motion_new(), .t = 1000000000ull};
    for (int i = 0; i < 3; i++) s->T[i][i] = 1;
    if (o) motion_set_options(s->m, o);
}
static void sim_pitch(Sim *s, double deg) {   // the true pose: turned about the controller's x by deg from flat
    double w[3] = {deg * DEG, 0, 0};
    rotate(s->T, w, 1);
}
// n samples turning at the true rate w (rad/s, controller axes); the sensors add the bias and the noise
static void sim_run(Sim *s, const double w[3], int n) {
    for (int i = 0; i < n; i++) {
        rotate(s->T, w, STEP_NS / 1e9);
        double acc[3], gyro[3];
        for (int j = 0; j < 3; j++) {
            acc[j] = -s->T[2][j] + s->acc_noise * gauss();   // gravity in controller axes, as an accelerometer reads it
            gyro[j] = w[j] + s->bias[j] + s->gyro_noise * gauss();
        }
        motion_feed(s->m, acc, gyro, s->t += STEP_NS);
    }
}
static const double zero3[3] = {0, 0, 0};
static MotionState newest(Sim *s) {
    MotionState st = {0};
    motion_read(s->m, &st, 1, s->t + 1000000);
    return st;
}
static int near3(const float *v, double x, double y, double z, double tol) {
    return fabs(v[0] - x) < tol && fabs(v[1] - y) < tol && fabs(v[2] - z) < tol;
}
// the angle between the reported up (the reference z in controller axes) and the true one, degrees
static double tilt_error_deg(Sim *s, const MotionState *st) {
    double est[3] = {st->dir[2], st->dir[5], st->dir[8]}, d = dot3(est, s->T[2]) / norm3(est);
    return acos(d > 1 ? 1 : d) / DEG;
}
static const MotionOptions RAW = {0, 0, 1, 1};   // no calibration, no dead zone: the sensor's own rate

// 1. lying still with a bias and a little noise: learned, then the reported rate is none and the angle stops
static void case_still_bias(void) {
    begin("1 still with a bias");
    int from = nlines;
    Sim s;
    sim_new(&s, NULL);
    const double b[3] = {0.6 * DEG, -0.3 * DEG, 0.2 * DEG};
    memcpy(s.bias, b, sizeof b);
    s.gyro_noise = 0.05 * DEG, s.acc_noise = 0.002;
    int first_still = -1, learned_at = -1;
    for (int i = 0; i < 1250; i++) {   // 5 s
        sim_run(&s, zero3, 1);
        if (first_still < 0 && s.m->still_ms > 0) first_still = i;
        int ok = 1;
        for (int j = 0; j < 3; j++) ok &= fabs(s.m->bias[j] - b[j]) < 0.05 * DEG;
        if (learned_at < 0 && ok) learned_at = i;
        if (!ok) learned_at = -1;
    }
    printf("  still after %.2f s, bias within 0.05 deg/s %.2f s later: %+.3f/%+.3f/%+.3f deg/s\n", first_still * 0.004,
           (learned_at - first_still) * 0.004, s.m->bias[0] / DEG, s.m->bias[1] / DEG, s.m->bias[2] / DEG);
    CHECK(first_still >= 0 && first_still * 0.004 < 1.0, "still counted after %d samples", first_still);
    CHECK(learned_at >= 0 && (learned_at - first_still) * 0.004 <= 1.5, "bias not within 0.05 deg/s 1.5 s into the still period");
    MotionState a = newest(&s);
    int zero = 1;
    for (int i = 0; i < 500; i++) {
        sim_run(&s, zero3, 1);
        MotionState st = newest(&s);
        zero &= st.gyro[0] == 0 && st.gyro[1] == 0 && st.gyro[2] == 0;
    }
    MotionState b2 = newest(&s);
    CHECK(zero, "a reported rate that is not 0 after the bias was learned");
    CHECK(memcmp(a.angle, b2.angle, sizeof a.angle) == 0, "the angle moved from %.5f to %.5f rev", a.angle[2], b2.angle[2]);
    CHECK(count_lines(from, "motion: bias learned") == 1, "%d bias learned lines, want 1", count_lines(from, "motion: bias learned"));
    end("1 still with a bias: learned, reported rate 0, angle stops, one log line");
}

// 2. no calibration: the bias goes through without a dead zone; each dead zone level zeroes a rate just under it and
// passes one just over it unchanged
static void case_deadzone(void) {
    begin("2 calibration off and the dead zone levels");
    Sim s;
    sim_new(&s, &RAW);
    s.bias[2] = 0.6 * DEG;
    sim_run(&s, zero3, 250);
    MotionState st = newest(&s);
    CHECK(fabs(st.gyro[2] - 0.6 / 360) < 1e-7, "reported %.7f rev/s, want the bias %.7f", st.gyro[2], 0.6 / 360);
    MotionOptions low = {0, MOTION_DEADZONE_LOW, 1, 1};
    motion_set_options(s.m, &low);
    sim_run(&s, zero3, 10);
    st = newest(&s);
    CHECK(st.gyro[2] == 0, "a 0.6 deg/s bias is not held by the low dead zone (%.7f rev/s)", st.gyro[2]);
    const double levels[3] = {MOTION_DEADZONE_LOW, MOTION_DEADZONE_STANDARD, MOTION_DEADZONE_HIGH};
    for (int l = 0; l < 3; l++) {
        MotionOptions o = {0, levels[l], 1, 1};
        for (int side = 0; side < 2; side++) {
            double f = side ? 1.02 : 0.98, rate = f * levels[l] * 2 * M_PI;
            Sim t;
            sim_new(&t, &o);
            double w[3] = {0, 0, rate};
            sim_run(&t, w, 10);
            st = newest(&t);
            float want = side ? (float)(rate / (2 * M_PI)) : 0;
            CHECK(st.gyro[2] == want && st.gyro[0] == 0 && st.gyro[1] == 0, "level %.3f rev/s, rate x%.2f: reported %.7f, want %.7f",
                  levels[l], f, st.gyro[2], want);
            printf("  dead zone %.3f rev/s: %.5f rev/s in -> %.5f out\n", levels[l], rate / (2 * M_PI), st.gyro[2]);
        }
    }
    end("2 the bias passes with calibration off and no dead zone; each level holds just under and passes just over");
}

// 3. known rotations from lying flat: a quarter turn about up, a quarter pitch and back
static void case_rotations(void) {
    begin("3 known rotations");
    Sim s;
    sim_new(&s, &RAW);
    sim_run(&s, zero3, 1);
    double wz[3] = {0, 0, 90 * DEG};
    sim_run(&s, wz, 250);
    MotionState st = newest(&s);
    CHECK(fabs(st.angle[2] - 0.25) < 0.002 && near3(st.dir, 0, 1, 0, 0.01) && near3(st.dir + 3, -1, 0, 0, 0.01) &&
          near3(st.dir + 6, 0, 0, 1, 0.01), "quarter turn about up: angle %.4f, x axis %.3f %.3f %.3f", st.angle[2],
          st.dir[0], st.dir[1], st.dir[2]);
    Sim p;
    sim_new(&p, &RAW);
    sim_run(&p, zero3, 1);
    double wx[3] = {90 * DEG, 0, 0};
    sim_run(&p, wx, 250);
    st = newest(&p);
    CHECK(fabs(st.angle[0] - 0.25) < 0.002 && near3(st.dir, 1, 0, 0, 0.01) && near3(st.dir + 3, 0, 0, 1, 0.01) &&
          near3(st.dir + 6, 0, -1, 0, 0.01), "quarter pitch: angle %.4f, y axis %.3f %.3f %.3f", st.angle[0], st.dir[3],
          st.dir[4], st.dir[5]);
    printf("  quarter pitch: angle %.4f rev, y axis %.3f %.3f %.3f\n", st.angle[0], st.dir[3], st.dir[4], st.dir[5]);
    wx[0] = -wx[0];
    sim_run(&p, wx, 250);
    st = newest(&p);
    CHECK(fabs(st.angle[0]) < 0.002 && near3(st.dir, 1, 0, 0, 0.01) && near3(st.dir + 3, 0, 1, 0, 0.01) &&
          near3(st.dir + 6, 0, 0, 1, 0.01), "back flat: angle %.4f, y axis %.3f %.3f %.3f", st.angle[0], st.dir[3],
          st.dir[4], st.dir[5]);
    end("3 quarter turn about up, quarter pitch and back");
}

// 4. held in the hand: tremor and a slow pan are never still
static void case_hand_held(void) {
    begin("4 hand tremor");
    int from = nlines;
    Sim s;
    sim_new(&s, NULL);
    s.gyro_noise = 2 * DEG / sqrt(3), s.acc_noise = 0.01;   // 2 deg/s RMS in all
    double pan[3] = {0, 0, 3 * DEG};
    int ever_still = 0;
    for (int i = 0; i < 8750; i++) {   // 35 s
        sim_run(&s, pan, 1);
        ever_still |= s.m->still_ms > 0;
    }
    CHECK(!ever_still, "counted still while held");
    CHECK(s.m->bias[0] == 0 && s.m->bias[1] == 0 && s.m->bias[2] == 0 && s.m->learned_ms == 0, "a bias learned while held");
    CHECK(count_lines(from, "motion: bias learned") == 0, "a bias learned line while held");
    CHECK(count_lines(from, "motion: no still moment yet") == 1, "%d no-still lines after 35 s held, want 1",
          count_lines(from, "motion: no still moment yet"));
    end("4 hand tremor (2 deg/s RMS, with a slow pan) is never still: nothing learned, one hint to set it down");
}

// 5. re-learning: still with a bias, held, then still with a bias 0.3 deg/s off it
static void case_relearn(void) {
    begin("5 re-learning");
    int from = nlines;
    Sim s;
    sim_new(&s, NULL);
    const double A[3] = {0.5 * DEG, -0.4 * DEG, 0.3 * DEG};
    memcpy(s.bias, A, sizeof A);
    s.gyro_noise = 0.05 * DEG, s.acc_noise = 0.002;
    sim_run(&s, zero3, 750);   // 3 s: still from about 0.8 s, so the period learns for the 1 s that logs it
    double after_a[3];
    memcpy(after_a, s.m->bias, sizeof after_a);
    s.gyro_noise = 2 * DEG / sqrt(3), s.acc_noise = 0.01;
    sim_run(&s, zero3, 500);
    s.gyro_noise = 0.05 * DEG, s.acc_noise = 0.002;
    s.bias[0] += 0.3 * DEG;
    sim_run(&s, zero3, 1500);   // 6 s: past 2 s learned in all the bias follows with a 3 s time constant
    printf("  bias after A %+.3f/%+.3f/%+.3f, after B %+.3f/%+.3f/%+.3f deg/s (learned %.0f ms)\n", after_a[0] / DEG,
           after_a[1] / DEG, after_a[2] / DEG, s.m->bias[0] / DEG, s.m->bias[1] / DEG, s.m->bias[2] / DEG,
           s.m->learned_ms);
    int ok = 1;
    for (int j = 0; j < 3; j++) ok &= fabs(after_a[j] - A[j]) < 0.05 * DEG && fabs(s.m->bias[j] - s.bias[j]) < 0.05 * DEG;
    CHECK(ok, "the bias did not follow A then B");
    CHECK(count_lines(from, "motion: bias learned") == 2, "%d bias learned lines, want 2",
          count_lines(from, "motion: bias learned"));
    CHECK(find_line(from, "calibration 2"), "no second calibration counted");
    end("5 re-learning: the bias moves to B, a second learned line");
}

// 6. the bias of each controller comes back with it
static void case_sources(void) {
    begin("6 source cache");
    int from = nlines;
    Sim s;
    sim_new(&s, NULL);
    motion_source(s.m, 1, "Pad one");
    CHECK(find_line(from, "motion: sensors of \"Pad one\": no bias learned yet"), "no line for the first source");
    s.bias[1] = 0.8 * DEG;
    s.gyro_noise = 0.05 * DEG, s.acc_noise = 0.002;
    sim_run(&s, zero3, 750);
    double one[3];
    memcpy(one, s.m->bias, sizeof one);
    double one_ms = s.m->learned_ms;
    CHECK(fabs(one[1] - 0.8 * DEG) < 0.05 * DEG, "source 1 learned %.3f deg/s", one[1] / DEG);
    int mark = nlines;
    motion_source(s.m, 1, "Pad one");
    CHECK(nlines == mark, "a line for the same source again");
    motion_source(s.m, 2, "Pad two");
    CHECK(s.m->bias[1] == 0 && s.m->learned_ms == 0, "source 2 starts with a bias");
    CHECK(find_line(mark, "motion: sensors of \"Pad two\": no bias learned yet"), "no line for source 2");
    CHECK(newest(&s).sampling != 0, "the samples were dropped at the source change");
    s.bias[1] = 0;
    sim_pitch(&s, 30);
    sim_run(&s, zero3, 1);
    MotionState st = newest(&s);
    CHECK(fabs(st.angle[0]) < 0.001 && tilt_error_deg(&s, &st) < 0.5, "the first sample of source 2 did not align: angle %.4f, tilt error %.2f deg",
          st.angle[0], tilt_error_deg(&s, &st));
    sim_run(&s, zero3, 250);
    mark = nlines;
    motion_source(s.m, 1, "Pad one");
    CHECK(memcmp(one, s.m->bias, sizeof one) == 0 && s.m->learned_ms == one_ms, "source 1's bias not restored");
    char want[160];
    snprintf(want, sizeof want, "motion: sensors of \"Pad one\": bias %+.2f/%+.2f/%+.2f deg/s restored (learned %u ms)",
             one[0] / DEG, one[1] / DEG, one[2] / DEG, (unsigned)one_ms);
    CHECK(find_line(mark, want), "no line '%s'", want);
    // five more sources push the oldest out of the four kept
    for (uint64_t id = 3; id <= 7; id++) motion_source(s.m, id, "Pad more");
    motion_source(s.m, 1, "Pad one");
    CHECK(s.m->learned_ms == 0, "source 1 still cached after five others");
    (void)from;
    end("6 source cache: restored, new ones start empty, lines as specified");
}

// 7. the turn and tilt factors
static void case_factors(void) {
    begin("7 turn and tilt factors");
    MotionOptions o = {0, 0, 2.0, 1.0};
    Sim s;
    sim_new(&s, &o);
    sim_run(&s, zero3, 1);
    double wz[3] = {0, 0, 90 * DEG}, wx[3] = {90 * DEG, 0, 0};
    sim_run(&s, wz, 250);
    MotionState st = newest(&s);
    CHECK(fabs(st.gyro[2] - 0.5) < 1e-6 && fabs(st.angle[2] - 0.5) < 0.004 && near3(st.dir, -1, 0, 0, 0.01) &&
          near3(st.dir + 6, 0, 0, 1, 0.01), "turn 2: gyro %.4f, angle %.4f, x axis %.3f %.3f %.3f", st.gyro[2],
          st.angle[2], st.dir[0], st.dir[1], st.dir[2]);
    Sim p;
    sim_new(&p, &o);
    sim_run(&p, zero3, 1);
    sim_run(&p, wx, 250);
    st = newest(&p);
    CHECK(fabs(st.gyro[0] - 0.25) < 1e-6 && fabs(st.angle[0] - 0.25) < 0.002 && near3(st.dir + 3, 0, 0, 1, 0.01),
          "turn 2, pitch: gyro %.4f, angle %.4f", st.gyro[0], st.angle[0]);
    MotionOptions half = {0, 0, 1.0, 0.5};
    Sim h;
    sim_new(&h, &half);
    sim_run(&h, zero3, 1);
    sim_run(&h, wx, 250);
    st = newest(&h);
    CHECK(fabs(st.gyro[0] - 0.125) < 1e-6 && fabs(st.angle[0] - 0.125) < 0.002 &&
          near3(st.dir + 3, 0, sqrt(0.5), sqrt(0.5), 0.02), "tilt 0.5: gyro %.4f, angle %.4f, y axis %.3f %.3f %.3f",
          st.gyro[0], st.angle[0], st.dir[3], st.dir[4], st.dir[5]);
    MotionOptions inv = {0, 0, -1.0, 1.0};
    Sim v;
    sim_new(&v, &inv);
    sim_run(&v, zero3, 1);
    sim_run(&v, wz, 250);
    st = newest(&v);
    CHECK(fabs(st.gyro[2] + 0.25) < 1e-6 && fabs(st.angle[2] + 0.25) < 0.002 && near3(st.dir, 0, -1, 0, 0.01),
          "turn -1: gyro %.4f, angle %.4f, x axis %.3f %.3f %.3f", st.gyro[2], st.angle[2], st.dir[0], st.dir[1], st.dir[2]);
    // 1 and 1: the reported rate is the input's and the direction the filter's, bit for bit; back to 1/1 snaps to it
    MotionOptions one = {0, 0, 1.0, 1.0};
    motion_set_options(v.m, &one);
    double wmix[3] = {0.3, -0.7, 1.1};
    int same = 1;
    for (int i = 0; i < 100; i++) {
        sim_run(&v, wmix, 1);
        st = newest(&v);
        double g = wmix[1] / (2 * M_PI);
        same &= st.gyro[1] == (float)g;
        for (int a = 0; a < 3; a++)
            for (int b = 0; b < 3; b++) same &= st.dir[a * 3 + b] == (float)v.m->R[b][a];
    }
    CHECK(same && memcmp(v.m->Rout, v.m->R, sizeof v.m->R) == 0, "1/1 is not the unscaled path");
    end("7 turn 2 doubles the turn and leaves the tilt, tilt 0.5 halves the pitch, turn -1 inverts, 1/1 is exact");
}

// 8. the integral term: a constant bias about a horizontal axis that is never learned (calibration off), tilted 30 deg
static double run_tilted(int with_ki) {
    Sim s;
    sim_new(&s, &RAW);
    sim_pitch(&s, 30);
    s.bias[0] = 1 * DEG;
    sim_run(&s, zero3, 1);
    for (int i = 0; i < 15000; i++) {   // 60 s
        sim_run(&s, zero3, 1);
        if (!with_ki) memset(s.m->ei, 0, sizeof s.m->ei);   // the proportional term alone
    }
    MotionState st = newest(&s);
    return tilt_error_deg(&s, &st);
}
static void case_integral(void) {
    begin("8 integral term");
    double with = run_tilted(1), without = run_tilted(0), analytic = asin(1 * DEG / MOTION_KP) / DEG;
    printf("  tilt error after 60 s: %.4f deg with MOTION_KI, %.4f deg without (analytic %.4f)\n", with, without, analytic);
    CHECK(fabs(without - analytic) < 0.1 * analytic, "the proportional term alone holds %.4f deg, want %.4f", without, analytic);
    CHECK(with * 5 < without, "the integral term does not cut the error 5 times");
    end("8 the integral term takes up a bias the gravity can see");
}

// 9. the aggregate line after 60 s
static void case_aggregate(void) {
    begin("9 aggregate line");
    int from = nlines;
    Sim s;
    sim_new(&s, NULL);
    const double b[3] = {0.6 * DEG, -0.3 * DEG, 0.2 * DEG};
    memcpy(s.bias, b, sizeof b);
    s.gyro_noise = 0.05 * DEG, s.acc_noise = 0.002;
    sim_run(&s, zero3, 14999);
    CHECK(count_lines(from, "motion: last") == 0, "an aggregate line before 60 s");
    sim_run(&s, zero3, 250);
    const char *l = find_line(from, "motion: last");
    CHECK(count_lines(from, "motion: last") == 1 && l, "%d aggregate lines after 61 s, want 1", count_lines(from, "motion: last"));
    if (l) {
        double secs, still, raw, rep, traw, trep, held;
        unsigned periods, learned;
        int n = sscanf(l, "motion: last %lf s: still %lf s in %u periods, drift while still %lf deg/s raw -> %lf deg/s "
                          "reported, turn drift %lf deg/min raw -> %lf reported, bias learned %u times, dead zone held %lf",
                       &secs, &still, &periods, &raw, &rep, &traw, &trep, &learned, &held);
        double mag = norm3(b) / DEG;
        CHECK(n == 9, "the line does not parse (%d fields)", n);
        CHECK(fabs(secs - 60) < 0.5 && still > 55 && periods == 1 && learned == 1, "window %.0f s, still %.1f s in %u, learned %u",
              secs, still, periods, learned);
        CHECK(fabs(raw - mag) < 0.05 && rep < 0.01 && fabs(traw - 0.2 * 60) < 1 && fabs(trep) < 0.05,
              "raw %.2f (want %.2f), reported %.2f, turn %.1f (want 12) -> %.1f", raw, mag, rep, traw, trep);
    }
    end("9 one aggregate line after 60 s: raw drift the bias, reported none");
}

// 10. the quarter turn and restart runtime/hle/ov_probe.c probe_feedback checks
static void case_probe(void) {
    begin("10 probe quarter turn and restart");
    MotionFusion *m = motion_new();
    uint64_t t = 1000000000ull;
    const double down[3] = {0, 0, -1}, still[3] = {0, 0, 0}, turn[3] = {0, 0, M_PI / 2};
    motion_feed(m, down, still, t);
    for (int i = 0; i < 250; i++) motion_feed(m, down, turn, t += 4000000);
    MotionState ms[16];
    int n = motion_read(m, ms, 16, t + 1000000);
    CHECK(n == 16 && ms[0].sampling == 251 && ms[15].sampling == 236 && ms[0].attributes == 1 &&
          fabs(ms[0].angle[2] - 0.25) < 0.002 && fabs(ms[0].gyro[2] - 0.25) < 1e-6 && near3(ms[0].dir, 0, 1, 0, 0.01) &&
          near3(ms[0].dir + 3, -1, 0, 0, 0.01) && near3(ms[0].dir + 6, 0, 0, 1, 0.01),
          "quarter turn: %d states, sampling %lld, angle %.4f rev", n, (long long)ms[0].sampling, ms[0].angle[2]);
    CHECK(motion_read(m, ms, 16, t + 300000000ull) == 0, "states 300 ms after the last sample");
    for (int i = 0; i < 250; i++) motion_feed(m, down, turn, t += 4000000);
    motion_restart(m);
    int same = motion_read(m, ms, 16, t + 1000000);
    motion_feed(m, down, turn, t += 4000000);
    int next = motion_read(m, ms, 16, t + 1000000);
    CHECK(same == 0 && next == 1 && ms[0].sampling == 502 && fabs(ms[0].angle[2]) < 0.002 && near3(ms[0].dir, 1, 0, 0, 0.01),
          "restart: %d states, then %d (sampling %lld, angle %.4f rev)", same, next, (long long)ms[0].sampling,
          ms[0].angle[2]);
    end("10 the probe's quarter turn and restart");
}

// a. larger biases lying still: learned too (the step rule waits until the bias has been learned for 2 s)
static void case_large_bias(void) {
    begin("a large biases");
    const double sizes[2] = {3, 6};
    for (int k = 0; k < 2; k++) {
        Sim s;
        sim_new(&s, NULL);
        const double b[3] = {sizes[k] * DEG, -sizes[k] / 2 * DEG, sizes[k] / 3 * DEG};
        memcpy(s.bias, b, sizeof b);
        s.gyro_noise = 0.05 * DEG, s.acc_noise = 0.002;
        sim_run(&s, zero3, 1250);   // 5 s
        int ok = 1;
        for (int j = 0; j < 3; j++) ok &= fabs(s.m->bias[j] - b[j]) < 0.05 * DEG;
        printf("  bias %.1f/%.1f/%.1f deg/s: learned %+.3f/%+.3f/%+.3f\n", b[0] / DEG, b[1] / DEG, b[2] / DEG,
               s.m->bias[0] / DEG, s.m->bias[1] / DEG, s.m->bias[2] / DEG);
        CHECK(ok, "a %.0f deg/s bias not learned within 0.05 deg/s in 5 s", sizes[k]);
        MotionState st = newest(&s);
        CHECK(st.gyro[0] == 0 && st.gyro[1] == 0 && st.gyro[2] == 0, "a reported rate after a %.0f deg/s bias was learned",
              sizes[k]);
    }
    end("a 3 and 6 deg/s biases lying still are learned within 0.05 deg/s");
}

// b. held pitched with a bias about the controller's z long enough for the integral to take part of it up, then set
// down flat: once the bias is learned, the integral no longer turns the heading of the still controller
static double heading_deg(const MotionState *st) { return atan2(st->dir[1], st->dir[0]) / DEG; }   // the x axis'
static void case_held_then_flat(void) {
    begin("b held pitched, then flat");
    Sim s;
    sim_new(&s, NULL);
    s.bias[2] = 1 * DEG;
    s.gyro_noise = 2 * DEG / sqrt(3), s.acc_noise = 0.01;
    sim_pitch(&s, 60);
    sim_run(&s, zero3, 30000);   // 120 s held
    double ei_held[3];
    memcpy(ei_held, s.m->ei, sizeof ei_held);
    double back[3] = {-30 * DEG, 0, 0};
    sim_run(&s, back, 500);   // 2 s to flat
    s.gyro_noise = 0.05 * DEG, s.acc_noise = 0.002;
    sim_run(&s, zero3, 2500);   // 10 s still
    MotionState a = newest(&s);
    sim_run(&s, zero3, 12500);   // 50 s more
    MotionState b = newest(&s);
    double drift = remainder(heading_deg(&b) - heading_deg(&a), 360);
    printf("  integral after holding %+.3f/%+.3f/%+.3f deg/s, bias learned flat %+.3f/%+.3f/%+.3f deg/s, heading drift "
           "%.3f deg in the last 50 s\n", ei_held[0] / DEG, ei_held[1] / DEG, ei_held[2] / DEG, s.m->bias[0] / DEG,
           s.m->bias[1] / DEG, s.m->bias[2] / DEG, drift);
    CHECK(fabs(s.m->bias[2] - 1 * DEG) < 0.05 * DEG, "the bias was not learned flat");
    CHECK(fabs(drift) < 2, "the heading drifted %.2f deg in 50 s still", drift);
    end("b after holding pitched with a z bias, the still flat controller's heading holds");
}

// c. turn and tilt factors over closed and non-commuting paths: the reported orientation stays tied to the filter's
static double reported_tilt_deg(const MotionState *st) { return acos(fmin(1, fmax(-1, st->dir[8]))) / DEG; }
static double true_tilt_deg(const Sim *s) { return acos(fmin(1, fmax(-1, s->T[2][2]))) / DEG; }
static void loops(Sim *s, int reverse) {   // five loops of 30 deg turns about the controller's x and y
    double xp[3] = {30 * DEG, 0, 0}, xm[3] = {-30 * DEG, 0, 0}, yp[3] = {0, 30 * DEG, 0}, ym[3] = {0, -30 * DEG, 0};
    for (int k = 0; k < 5; k++) {
        sim_run(s, xp, 250);
        sim_run(s, yp, 250);
        sim_run(s, reverse ? ym : xm, 250);
        sim_run(s, reverse ? xm : ym, 250);
    }
}
static void case_factor_loops(void) {
    begin("c factors on closed paths");
    const MotionOptions opts[2] = {{0, 0, 1.0, 2.0}, {0, 0, 2.0, 2.0}};
    for (int k = 0; k < 2; k++)
        for (int reverse = 0; reverse < 2; reverse++) {
            Sim s;
            sim_new(&s, &opts[k]);
            sim_run(&s, zero3, 1);
            loops(&s, reverse);
            MotionState st = newest(&s);
            double rep = reported_tilt_deg(&st), tru = true_tilt_deg(&s);
            printf("  turn %.0f tilt %.0f, %s loops: true tilt %.3f deg, reported %.3f deg\n", opts[k].turn, opts[k].tilt,
                   reverse ? "closed" : "x y -x -y", tru, rep);
            CHECK(fabs(rep - 2 * tru) < 1, "reported tilt %.2f deg, want twice %.2f", rep, tru);
            if (reverse) CHECK(rep < 1, "the closed loops leave the reported up %.2f deg off flat", rep);
        }
    Sim p;
    MotionOptions tilt2 = {0, 0, 1.0, 2.0};
    sim_new(&p, &tilt2);
    sim_run(&p, zero3, 1);
    double pitch[3] = {10 * DEG, 0, 0};
    sim_run(&p, pitch, 250);
    MotionState st = newest(&p);
    printf("  tilt 2, pitch 10 deg: reported %.3f deg\n", reported_tilt_deg(&st));
    CHECK(fabs(reported_tilt_deg(&st) - 20) < 0.2, "tilt 2 with a 10 deg pitch reports %.2f deg", reported_tilt_deg(&st));
    Sim y;
    MotionOptions turn2 = {0, 0, 2.0, 1.0};
    sim_new(&y, &turn2);
    sim_run(&y, zero3, 1);
    double p30[3] = {30 * DEG, 0, 0};
    sim_run(&y, p30, 250);
    MotionState a = newest(&y);
    double up_rate[3] = {45 * DEG * y.T[2][0], 45 * DEG * y.T[2][1], 45 * DEG * y.T[2][2]};   // about the world's up
    sim_run(&y, up_rate, 250);
    MotionState b = newest(&y);
    double turned = remainder(atan2(b.dir[4], b.dir[3]) / DEG - atan2(a.dir[4], a.dir[3]) / DEG, 360);
    printf("  turn 2, 45 deg about up while pitched 30: heading %.3f deg, reported pitch %.3f deg\n", turned,
           reported_tilt_deg(&b));
    CHECK(fabs(turned - 90) < 0.5 && fabs(reported_tilt_deg(&b) - 30) < 0.2, "heading %.2f (want 90), pitch %.2f (want 30)",
          turned, reported_tilt_deg(&b));
    end("c tilt 2 and turn 2 stay tied to the filter over loops; 10 deg pitch -> 20, 45 deg turn -> 90 at 30 deg pitch");
}

// d. a known limit: a smooth slow turn with little noise looks like a still controller with a bias
static void case_smooth_pan(void) {
    begin("d smooth slow pan");
    Sim s;
    sim_new(&s, NULL);
    s.gyro_noise = 0.05 * DEG, s.acc_noise = 0.002;
    double pan[3] = {0, 0, 3 * DEG};
    int ever_still = 0;
    for (int i = 0; i < 1250; i++) {
        sim_run(&s, pan, 1);
        ever_still |= s.m->still_ms > 0;
    }
    printf("  still %d, bias learned %+.3f deg/s about z\n", ever_still, s.m->bias[2] / DEG);
    CHECK(ever_still && fabs(s.m->bias[2] - 3 * DEG) < 0.1 * DEG, "the smooth pan was not taken as still");
    end("d expected limit: a smooth 3 deg/s pan with sensor-level noise IS taken as still, its rate learned as bias");
}

// e. samples with values that are not finite move nothing and poison nothing
static void case_not_finite(void) {
    begin("e samples that are not finite");
    Sim s;
    sim_new(&s, NULL);
    s.bias[0] = 0.5 * DEG;
    s.gyro_noise = 0.05 * DEG, s.acc_noise = 0.002;
    sim_run(&s, zero3, 500);
    const double nan3[3] = {NAN, 0, -1}, inf3[3] = {0, INFINITY, 0}, down[3] = {0, 0, -1};
    motion_feed(s.m, nan3, zero3, s.t += STEP_NS);
    motion_feed(s.m, down, inf3, s.t += STEP_NS);
    sim_run(&s, zero3, 250);
    MotionState st = newest(&s);
    int ok = isfinite(s.m->var_g) && isfinite(s.m->var_a) && isfinite(s.m->mean_an) && isfinite(s.m->ei[0]);
    for (int i = 0; i < 3; i++) {
        ok &= isfinite(s.m->mean_g[i]) && isfinite(s.m->bias[i]) && isfinite(st.angle[i]) && isfinite(st.gyro[i]);
        for (int j = 0; j < 3; j++) ok &= isfinite(s.m->R[i][j]) && isfinite(s.m->Rout[i][j]);
    }
    CHECK(ok, "a value that is not finite reached the state");
    CHECK(near3(st.dir + 6, 0, 0, 1, 0.01) && s.m->still_ms > 0, "the orientation or the stillness was lost");
    end("e NaN and infinite samples leave the orientation and the statistics finite");
}

// f. a wrong bias (a smooth pan learned as one) is learned again once the controller lies truly still
static void case_wrong_bias(void) {
    begin("f wrong bias relearned");
    int from = nlines;
    Sim s;
    sim_new(&s, NULL);
    s.gyro_noise = 0.05 * DEG, s.acc_noise = 0.002;
    double pan[3] = {0, 0, 3 * DEG};
    sim_run(&s, pan, 875);   // 3.5 s
    double wrong = s.m->bias[2];
    sim_run(&s, zero3, 15000);   // 60 s still, no bias
    MotionState a = newest(&s);
    sim_run(&s, zero3, 250);
    MotionState b = newest(&s);
    printf("  after the pan %+.3f deg/s, after 60 s still %+.3f/%+.3f/%+.3f deg/s\n", wrong / DEG, s.m->bias[0] / DEG,
           s.m->bias[1] / DEG, s.m->bias[2] / DEG);
    CHECK(wrong > 2 * DEG, "the pan was not learned as a bias (%.3f deg/s): the case does not test the relearning", wrong / DEG);
    int ok = 1;
    for (int j = 0; j < 3; j++) ok &= fabs(s.m->bias[j]) < 0.05 * DEG;
    CHECK(ok, "the bias did not come back to 0");
    CHECK(b.gyro[0] == 0 && b.gyro[1] == 0 && b.gyro[2] == 0 && memcmp(a.angle, b.angle, sizeof a.angle) == 0,
          "a reported rate %.6f rev/s after relearning", b.gyro[2]);
    CHECK(count_lines(from, "motion: bias relearned from scratch") == 1, "%d relearn lines, want 1",
          count_lines(from, "motion: bias relearned from scratch"));
    end("f a pan learned as a bias is relearned after 3 s truly still: bias back to 0, reported rate 0");
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    case_still_bias();
    case_deadzone();
    case_rotations();
    case_hand_held();
    case_relearn();
    case_sources();
    case_factors();
    case_integral();
    case_aggregate();
    case_probe();
    case_large_bias();
    case_held_then_flat();
    case_factor_loops();
    case_smooth_pan();
    case_not_finite();
    case_wrong_bias();
    printf("motion_fusion_check: %s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
