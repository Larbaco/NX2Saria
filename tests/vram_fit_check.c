#include <stdio.h>

#include "vram_fit.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static PresetHw card(uint32_t vram_mib, int dlss) {
    PresetHw hw = {0};
    hw.have_gpu = 1, hw.gpu_type = PRESET_GPU_DISCRETE, hw.mesh = 1, hw.vram_mib = vram_mib, hw.dlss = dlss;
    return hw;
}

int main(void) {
    unsigned what, res;
    int mode;

    CHECK(vram_fit_mode(0, "dlaa") == VRAM_MODE_OFF && vram_fit_mode(1, "dlaa") == VRAM_MODE_FULL &&
          vram_fit_mode(1, "quality") == VRAM_MODE_QUALITY && vram_fit_mode(1, "ultra_performance") == VRAM_MODE_ULTRA_PERFORMANCE,
          "the modes in order of the scene they render");

    VramLoad now = {1080, VRAM_MODE_QUALITY, 0}, same = now;
    VramLoad dlaa = {1080, VRAM_MODE_FULL, 0}, lighter = {1080, VRAM_MODE_PERFORMANCE, 0};
    VramLoad all = {1440, VRAM_MODE_FULL, 1}, trade = {1440, VRAM_MODE_PERFORMANCE, 0};
    CHECK(vram_fit_heavier(&now, &same) == 0 && vram_fit_heavier(&now, &lighter) == 0, "the same and a lighter draft: nothing heavier");
    CHECK(vram_fit_heavier(&now, &all) == (VRAM_SUGGEST_RES | VRAM_SUGGEST_MODE | VRAM_SUGGEST_FRAMEGEN), "all three heavier");
    CHECK(vram_fit_heavier(&now, &trade) == VRAM_SUGGEST_RES, "a larger picture with a lighter mode: the picture is named");
    CHECK(vram_fit_heavier(&all, &now) == 0, "frame generation turned off, a smaller picture: nothing");

    PresetHw six = card(6017, 1), four = card(4096, 1), eight = card(8100, 1), six_plain = card(6017, 0), none = {0};
    CHECK(vram_fit_tier(6017) == PRESET_MEDIUM && vram_fit_tier(4096) == PRESET_LOW && vram_fit_tier(8100) == -1 &&
          vram_fit_tier(7700) == -1 && vram_fit_tier(0) == -1, "the tier the video memory alone gives; none from 8 GiB and for an unknown card");

    CHECK(vram_fit_note(MEM_TIGHT, 0, &now, &dlaa, &eight, &what, &res, &mode) == VRAM_FIT_MEASURED && what == VRAM_SUGGEST_MODE,
          "tight now and a heavier mode: the measured note, on any card");
    CHECK(vram_fit_note(MEM_CRITICAL, 0, &now, &all, &six, &what, &res, &mode) == VRAM_FIT_MEASURED &&
          what == (VRAM_SUGGEST_RES | VRAM_SUGGEST_MODE | VRAM_SUGGEST_FRAMEGEN), "critical on a 6 GiB card: the measured note wins over the preset's");
    CHECK(vram_fit_note(MEM_NORMAL, 1, &now, &dlaa, &eight, &what, &res, &mode) == VRAM_FIT_MEASURED,
          "room now but a notice was shown this session: the measured note");
    CHECK(vram_fit_note(MEM_CRITICAL, 1, &now, &lighter, &six, &what, &res, &mode) == VRAM_FIT_NONE && !what,
          "critical and a lighter draft: no note");
    CHECK(vram_fit_note(MEM_CRITICAL, 1, &now, &same, &six, &what, &res, &mode) == VRAM_FIT_NONE, "nothing changed: no note");

    CHECK(vram_fit_note(MEM_ROOMY, 0, &now, &dlaa, &six, &what, &res, &mode) == VRAM_FIT_PRESET && what == VRAM_SUGGEST_MODE &&
          res == 1080 && mode == VRAM_MODE_QUALITY, "room now, 6 GiB, DLAA over the preset's quality: the preset's note (%u, %d)", res, mode);
    CHECK(vram_fit_note(MEM_ROOMY, 0, &now, &all, &six, &what, &res, &mode) == VRAM_FIT_PRESET &&
          what == (VRAM_SUGGEST_RES | VRAM_SUGGEST_MODE), "frame generation is no part of the preset's note");
    VramLoad fg = {1080, VRAM_MODE_QUALITY, 1};
    CHECK(vram_fit_note(MEM_ROOMY, 0, &now, &fg, &six, &what, &res, &mode) == VRAM_FIT_NONE, "frame generation alone with room: no note");
    VramLoad low_now = {1080, VRAM_MODE_PERFORMANCE, 0}, to_quality = {1080, VRAM_MODE_QUALITY, 0};
    CHECK(vram_fit_note(MEM_ROOMY, 0, &low_now, &to_quality, &four, &what, &res, &mode) == VRAM_FIT_PRESET && mode == VRAM_MODE_PERFORMANCE,
          "4 GiB: quality is over the low tier's performance");
    CHECK(vram_fit_note(MEM_ROOMY, 0, &low_now, &to_quality, &six, &what, &res, &mode) == VRAM_FIT_NONE,
          "6 GiB: quality is what the tier gives");
    VramLoad over_now = {1440, VRAM_MODE_FULL, 0}, over_res = {1800, VRAM_MODE_FULL, 0};
    CHECK(vram_fit_note(MEM_ROOMY, 0, &over_now, &over_res, &six, &what, &res, &mode) == VRAM_FIT_PRESET && what == VRAM_SUGGEST_RES,
          "already over the preset in the mode: only what the draft makes heavier is named");
    CHECK(vram_fit_note(MEM_ROOMY, 0, &now, &dlaa, &six_plain, &what, &res, &mode) == VRAM_FIT_NONE,
          "a 6 GiB card the preset gives no upscaler: no mode to compare with");
    CHECK(vram_fit_note(MEM_ROOMY, 0, &now, &all, &eight, &what, &res, &mode) == VRAM_FIT_NONE, "8 GiB with room: no note");
    CHECK(vram_fit_note(MEM_ROOMY, 0, &now, &all, &none, &what, &res, &mode) == VRAM_FIT_NONE, "nothing known of the card: no note");

    if (fails) return printf("vram_fit_check: %d checks failed\n", fails), 1;
    printf("vram_fit_check: ok\n");
    return 0;
}
