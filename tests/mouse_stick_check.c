#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "input_map.h"

const char *cfg_get(const char *key) { (void)key; return NULL; }
double cfg_double(const char *key) { (void)key; return 0; }
void rt_log(const char *fmt, ...) { (void)fmt; }

#define FULL 32767

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL: " __VA_ARGS__); putchar('\n'); fails++; } } while (0)

int main(void) {
    static const float cams[] = {0.1f, 0.5f, 1.0f, 1.5f, 2.0f, 3.0f};
    int32_t st[2], one[2];
    for (unsigned i = 0; i < sizeof cams / sizeof *cams; i++) {
        float cam = cams[i];
        for (int counts = 1; counts <= 40; counts += 3) {
            input_mouse_stick(counts, 0, 1.0 / 60, 1, 1, one);
            input_mouse_stick(counts, 0, 1.0 / 60, 1, cam, st);
            double at_one = FULL * (counts * 60.0) / 3000.0;
            double want = at_one / cam > FULL ? FULL : at_one / cam;
            CHECK(fabs(one[0] - at_one) <= 0.5, "camera speed 1, %d counts: %d, expected %.1f", counts, one[0], at_one);
            CHECK(fabs(st[0] - want) <= 0.5 && st[1] == 0, "camera speed %.1f, %d counts: %d, expected %.1f", cam, counts,
                  st[0], want);
        }
        input_mouse_stick(5000, 0, 1.0 / 60, 1, cam, st);
        CHECK(st[0] == FULL && st[1] == 0, "camera speed %.1f: a flick right gives %d %d", cam, st[0], st[1]);
        input_mouse_stick(0, 5000, 1.0 / 60, 1, cam, st);
        CHECK(st[0] == 0 && st[1] == -FULL, "camera speed %.1f: a flick down gives %d %d", cam, st[0], st[1]);
        input_mouse_stick(-5000, -5000, 1.0 / 60, 1, cam, st);
        double len = sqrt((double)st[0] * st[0] + (double)st[1] * st[1]);
        CHECK(st[0] < 0 && st[1] > 0 && st[0] == -st[1] && len <= FULL + 1.0 && len >= FULL - 2.0,
              "camera speed %.1f: a diagonal flick gives %d %d", cam, st[0], st[1]);
    }
    input_mouse_stick(80, 0, 1.0 / 60, 1, 1.5f, st);
    CHECK(st[0] == FULL, "camera speed 1.5, 80 counts in 1/60 s: %d, expected full tilt", st[0]);
    input_mouse_stick(30, 0, 1.0 / 60, 1, 1.5f, st);
    CHECK(st[0] == 13107, "camera speed 1.5, 30 counts in 1/60 s: %d, expected 13107", st[0]);
    input_mouse_stick(30, 0, 1.0 / 60, 1, 0, st);
    CHECK(st[0] == 0 && st[1] == 0, "camera speed 0 gives %d %d", st[0], st[1]);
    input_mouse_stick(30, 0, 1.0 / 60, 1, -1, st);
    CHECK(st[0] == 0 && st[1] == 0, "a negative camera speed gives %d %d", st[0], st[1]);
    if (!fails) puts("mouse stick: the camera speed divides the deflection before the limit to full tilt");
    return fails != 0;
}
