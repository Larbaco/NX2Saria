#include <stdio.h>
#include <string.h>

#include "vk_fg_stats.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define MS 1000000ull

static void session(FgStats *s, char lines[][1024], int *nlines, uint64_t frames) {
    uint64_t presents = 0, t = 0;
    for (uint64_t f = 0; f < frames; f++, t += 16667000ull) {
        FgsOutcome o = f < 4600 ? fgs_classify(0, 0, 0, 1, 0, 0)
                       : f >= 6000 && f < 6300 ? fgs_classify(1, f % 2, 0, 1, 1, 0)
                       : fgs_classify(0, 1, 1, 1, 1, f == 4600 || f == 6300);
        presents += o == FGS_GENERATED ? 2 : 1;
        fgs_count(s, o, presents, t);
        if (o == FGS_GENERATED) fgs_gpu(s, 2.0);
        char buf[1024];
        int r = fgs_line(s, "fg", presents, t, buf, sizeof buf);
        if (r == 1 && *nlines < 400) strcpy(lines[(*nlines)++], buf);
    }
}

static char lines[400][1024];

int main(void) {
    CHECK(fgs_classify(1, 1, 1, 1, 1, 0) == FGS_MENU, "the menu comes first, whatever the frame had");
    CHECK(fgs_classify(1, 0, 0, 0, 0, 0) == FGS_MENU, "the menu over no scene is the menu");
    CHECK(fgs_classify(0, 0, 0, 1, 1, 0) == FGS_NO_SCENE, "no scene and no inputs: no scene");
    CHECK(fgs_classify(0, 1, 0, 1, 1, 0) == FGS_NO_INPUTS, "a scene without inputs");
    CHECK(fgs_classify(0, 0, 1, 1, 1, 0) == FGS_GENERATED, "inputs mean a scene was drawn");
    CHECK(fgs_classify(0, 1, 1, 0, 1, 0) == FGS_TOO_LARGE, "too large before the HUD-less picture");
    CHECK(fgs_classify(0, 1, 1, 1, 0, 0) == FGS_NO_HUDLESS, "no HUD-less picture");
    CHECK(fgs_classify(0, 1, 1, 1, 1, 1) == FGS_CUT, "a history start");
    CHECK(fgs_classify(0, 1, 1, 1, 1, 0) == FGS_GENERATED, "generated");

    FgStats s;
    memset(&s, 0, sizeof s);
    char buf[1024];
    CHECK(fgs_line(&s, "fg", 0, 0, buf, sizeof buf) == -1, "no line before any frame");
    for (int i = 0; i < FGS_PERIOD - 1; i++) fgs_count(&s, FGS_GENERATED, 2 * (uint64_t)i, (uint64_t)i * 16 * MS);
    CHECK(fgs_line(&s, "fg", 0, 0, buf, sizeof buf) == -1, "no line before %d gameplay frames", FGS_PERIOD);
    fgs_count(&s, FGS_GENERATED, 2 * FGS_PERIOD, 10000 * MS);
    CHECK(fgs_line(&s, "fg", 2 * FGS_PERIOD, 10000 * MS, buf, sizeof buf) == 1, "a line at %d gameplay frames", FGS_PERIOD);
    CHECK(strstr(buf, "fg: 600 gameplay frames: 600 generated (100.0%)") != NULL, "the period's counts: %s", buf);
    CHECK(strstr(buf, "1200 presents in 10.0 s (120.0 a second on screen, 60.0 rendered)") != NULL,
          "presents over the period's time: %s", buf);
    CHECK(fgs_line(&s, "fg", 0, 0, buf, sizeof buf) == -1, "the next period starts empty");

    memset(&s, 0, sizeof s);
    for (int i = 0; i < FGS_IDLE; i++) fgs_count(&s, FGS_NO_SCENE, (uint64_t)i, (uint64_t)i * 16 * MS);
    CHECK(fgs_line(&s, "fsr3fg", FGS_IDLE, 60000 * MS, buf, sizeof buf) == 1, "a line after %d frames of any kind", FGS_IDLE);
    CHECK(strstr(buf, "fsr3fg: 0 gameplay frames: 0 generated (0.0%)") && strstr(buf, "3600 without a scene"), "%s", buf);

    memset(&s, 0, sizeof s);
    int n = 0;
    session(&s, lines, &n, 21600);
    CHECK(n > 0, "lines were written");
    const char *last = n ? lines[n - 1] : "";
    CHECK(s.total[FGS_NO_SCENE] == 4600 && s.total[FGS_MENU] == 300, "no scene %llu, menu %llu",
          (unsigned long long)s.total[FGS_NO_SCENE], (unsigned long long)s.total[FGS_MENU]);
    CHECK(s.total[FGS_NO_INPUTS] == 0, "the menu's frames are not frames without inputs (%llu)",
          (unsigned long long)s.total[FGS_NO_INPUTS]);
    uint64_t play = fgs_play(s.total);
    CHECK(play == 21600 - 4600 - 300, "gameplay frames %llu", (unsigned long long)play);
    CHECK(fgs_pct(s.total[FGS_GENERATED], play) > 99.9, "gameplay generated %.2f%%", fgs_pct(s.total[FGS_GENERATED], play));
    CHECK(strstr(last, "GPU 2.00 ms a generated frame") != NULL, "GPU time: %s", last);
    CHECK(strstr(last, "the run: ") != NULL, "the run's totals: %s", last);

    memset(&s, 0, sizeof s);
    int written = 0;
    uint64_t t = 0;
    for (uint64_t f = 0; f < (uint64_t)FGS_PERIOD * (FGS_LINES + 10 * FGS_EVERY); f++, t += 16 * MS) {
        fgs_count(&s, FGS_GENERATED, 2 * f, t);
        written += fgs_line(&s, "fg", 2 * f, t, buf, sizeof buf) == 1;
    }
    CHECK(s.periods == FGS_LINES + 10 * FGS_EVERY, "periods %llu", (unsigned long long)s.periods);
    CHECK(written == FGS_LINES + 10,
          "%d lines written for %llu periods", written, (unsigned long long)s.periods);
    CHECK(s.total[FGS_GENERATED] == (uint64_t)FGS_PERIOD * (FGS_LINES + 10 * FGS_EVERY), "the totals keep every frame");

    memset(&s, 0, sizeof s);
    for (int i = 0; i < 300; i++) fgs_count(&s, FGS_GENERATED, 5000 + 2 * (uint64_t)i, (uint64_t)i * 16 * MS);
    for (int i = 0; i < 300; i++) fgs_count(&s, FGS_GENERATED, 2 * (uint64_t)i, 60000 * MS + (uint64_t)i * 16 * MS);
    CHECK(fgs_line(&s, "fg", 600, 60000 * MS + 300 * 16 * MS, buf, sizeof buf) == 1, "a line at 600");
    CHECK(strstr(buf, "600 presents in 4.8 s") != NULL, "the presents since the new swapchain: %s", buf);

    memset(&s, 0, sizeof s);
    fgs_gpu(&s, 0), fgs_gpu(&s, -1), fgs_gpu(&s, 5000), fgs_gpu(&s, 1.5);
    CHECK(s.gpu_n == 1 && s.gpu_ms == 1.5, "GPU samples %llu, %.2f ms", (unsigned long long)s.gpu_n, s.gpu_ms);

    printf(fails ? "fg_stats_check: %d failed\n" : "fg_stats_check: ok\n", fails);
    return fails != 0;
}
