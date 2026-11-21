// runtime/gpu_choice.h: which card gpu_index and gpu_name pick when the cards are listed in another order than when the
// settings were written, and the log lines about a choice (runtime/vk_device.c pick_device, hw_detect.c probe_gpu,
// ui/menu.cpp row_gpu). Built and run by tests/gpu_choice_check.py; prints each failed check and exits 1 on any.
#include <stdio.h>
#include <string.h>

#include "gpu_choice.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define N(a) ((int)(sizeof (a) / sizeof *(a)))

#define NVIDIA "NVIDIA GeForce RTX 3050 6GB Laptop GPU"
#define INTEL "Intel(R) UHD Graphics"

// A laptop with an RTX 3050 and Intel UHD Graphics, which the loader lists twice: the order of one day, in which the
// NVIDIA card is gpu 1 (where a menu's gpu_index = 1 was written), and the order of the next, with it first.
static const GpucCard day1[] = {{INTEL, GPUC_INTEGRATED, 1}, {NVIDIA, GPUC_DISCRETE, 1}, {INTEL, GPUC_INTEGRATED, 1}};
static const GpucCard day2[] = {{NVIDIA, GPUC_DISCRETE, 1}, {INTEL, GPUC_INTEGRATED, 1}, {INTEL, GPUC_INTEGRATED, 1}};

static int idx(const GpucCard *c, int n, int want, const char *name, int *dropped) {
    return gpuc_index(c, n, want, name, dropped);
}

int main(void) {
    int dropped;

    // gpuc_has: gpu_name's match is a part of the name, upper and lower case alike.
    CHECK(gpuc_has(NVIDIA, "NVIDIA"), "the start of the name");
    CHECK(gpuc_has(NVIDIA, "rtx 3050"), "the middle, in lower case");
    CHECK(gpuc_has("AMD Radeon RX 6700 (RADV NAVI22)", "(radv"), "a parenthesis is plain text");
    CHECK(!gpuc_has(NVIDIA, "Radeon"), "another vendor");
    CHECK(!gpuc_has(NVIDIA, "NVIDIA GeForce RTX 3050 6GB Laptop GPU and more"), "a part longer than the name");
    CHECK(!gpuc_has(NULL, "x") && !gpuc_has("x", NULL), "NULL is no match");

    // gpuc_index: the position goes unchanged while one of the two settings is unset (an older version's file).
    CHECK(idx(day1, 3, 1, NULL, &dropped) == 1 && !dropped, "gpu_index alone, the day it was written");
    CHECK(idx(day2, 3, 1, NULL, &dropped) == 1 && !dropped, "gpu_index alone goes by its position the next day: the Intel graphics");
    CHECK(idx(day2, 3, 1, "", &dropped) == 1 && !dropped, "an empty name is no name");
    CHECK(idx(day2, 3, -1, NVIDIA, &dropped) == -1 && !dropped, "a name alone: nothing to drop");
    CHECK(idx(day2, 3, -1, NULL, &dropped) == -1 && !dropped, "Auto");

    // The settings the menu writes: the position and the card's name. The position holds while it lists that card, and
    // the name decides when it does not (the loader's other order).
    CHECK(idx(day1, 3, 1, NVIDIA, &dropped) == 1 && !dropped, "both, the day they were written");
    CHECK(idx(day2, 3, 1, NVIDIA, &dropped) == -1 && dropped, "both, the next day: gpu 1 is the Intel graphics, so the name decides");
    CHECK(idx(day2, 3, 0, NVIDIA, &dropped) == 0 && !dropped, "both, the next day, written again: gpu 0 has the name");
    CHECK(idx(day2, 3, 7, NVIDIA, &dropped) == -1 && dropped, "a position past the list: the name decides");
    CHECK(idx(day2, 3, 1, "nvidia", &dropped) == -1 && dropped, "a part of the name, in lower case, as written by hand");
    CHECK(idx(day1, 3, 2, INTEL, &dropped) == 2 && !dropped, "the Intel graphics listed twice: the position tells which listing");
    CHECK(idx(day1, 3, 0, INTEL, &dropped) == 0 && !dropped, "and the other listing");

    // gpuc_shown: the menu's row shows the card the settings pick, which the game picks too.
    CHECK(gpuc_shown(day1, 3, 1, NULL) == 1, "an older file: the position");
    CHECK(gpuc_shown(day2, 3, 1, NULL) == 1, "an older file the next day: still the position, the Intel graphics");
    CHECK(gpuc_shown(day2, 3, 1, NVIDIA) == 0, "the menu's two settings the next day: the name's card");
    CHECK(gpuc_shown(day2, 3, -1, "NVIDIA") == 0, "a name alone");
    CHECK(gpuc_shown(day2, 3, -1, NULL) == -1, "Auto");
    CHECK(gpuc_shown(day2, 3, -1, "Radeon") == -1, "a name no card has: the game takes the best card, as for Auto");
    CHECK(gpuc_shown(day2, 3, 7, NULL) == -1, "a position past the list");
    CHECK(gpuc_shown(day2, 3, 1, "Radeon") == -1, "a position over a name no card has: the name drops it, none left");

    char line[1024], note[256];

    // The dropped position's line.
    gpuc_dropped_line(day2, 3, 1, NVIDIA, line, sizeof line);
    CHECK(!strcmp(line, "vk: gpu_index = 1 lists \"" INTEL "\", which does not have gpu_name = " NVIDIA " in its name: the "
                        "cards are listed in another order than when gpu_index was written, so the name decides"),
          "dropped line: %s", line);
    gpuc_dropped_line(day2, 3, 7, NVIDIA, line, sizeof line);
    CHECK(!strcmp(line, "vk: gpu_index = 7 lists no card (3 are listed): gpu_name = " NVIDIA " decides"), "past the list: %s", line);

    // The warning: gpu_index = 1 on the Intel graphics while the NVIDIA card is listed first. The log of the day it was
    // found had no such line, and said nothing of why the game ran on the Intel graphics.
    int alt = gpuc_choice_line(day2, 3, 1, 1, NULL, line, sizeof line, note, sizeof note);
    CHECK(alt == 0, "the discrete card to name is gpu 0, got %d", alt);
    CHECK(!strcmp(line, "vk: warning: gpu 1 \"" INTEL "\" (integrated GPU) was chosen by gpu_index = 1, while gpu 0 \""
                        NVIDIA "\" is a discrete card. The order the cards are listed in can change from one start to the "
                        "next (the system's graphics settings for the game, a display on one card, the power source), and "
                        "gpu_index then names another card. Auto, in the menu's Graphics card row, takes the discrete "
                        "card; gpu_name = " NVIDIA " keeps it by its name"),
          "position warning: %s", line);
    CHECK(!strcmp(note, "card chosen by gpu_index = 1 (integrated GPU) while gpu 0 is a discrete card"), "note: %s", note);

    // The day the position lists the NVIDIA card: the choice is a discrete card, nothing to say.
    CHECK(gpuc_choice_line(day1, 3, 1, 1, NULL, line, sizeof line, note, sizeof note) == -1 && !line[0] && !note[0],
          "a discrete card chosen is no warning");
    // Auto picks the best card: no choice, no warning, whatever it picked.
    CHECK(gpuc_choice_line(day2, 3, 1, -1, NULL, line, sizeof line, note, sizeof note) == -1 && !line[0], "Auto: no warning");
    // A gpu_name, alone or beside the position the menu wrote with it, is the player's own choice of the card: no
    // warning at each start, nor the advice to set the gpu_name that is already set.
    CHECK(gpuc_choice_line(day2, 3, 1, -1, "Intel", line, sizeof line, note, sizeof note) == -1 && !line[0] && !note[0],
          "a name alone: no warning, got %s", line);
    CHECK(gpuc_choice_line(day1, 3, 2, 2, INTEL, line, sizeof line, note, sizeof note) == -1 && !line[0] && !note[0],
          "the Intel graphics chosen in the menu (both settings): no warning, got %s", line);
    CHECK(gpuc_choice_line(day2, 3, 1, 1, "", line, sizeof line, note, sizeof note) == 0 && line[0],
          "an empty name is no name: the position alone warns");
    // No usable discrete card: the player's choice is the only card that runs the game.
    {
        GpucCard broken[] = {{INTEL, GPUC_INTEGRATED, 1}, {NVIDIA, GPUC_DISCRETE, 0}};
        CHECK(gpuc_choice_line(broken, N(broken), 0, 0, NULL, line, sizeof line, note, sizeof note) == -1 && !line[0],
              "a discrete card the game cannot use is no warning");
    }
    // A software renderer picked by position beside a card.
    {
        GpucCard bench[] = {{"AMD Radeon RX 6700 (RADV NAVI22)", GPUC_DISCRETE, 1}, {"llvmpipe (LLVM 20.1.2, 256 bits)", GPUC_CPU, 0}};
        alt = gpuc_choice_line(bench, N(bench), 1, 1, NULL, line, sizeof line, note, sizeof note);
        CHECK(alt == 0 && strstr(line, "gpu 1 \"llvmpipe (LLVM 20.1.2, 256 bits)\" (software renderer) was chosen by gpu_index = 1"),
              "a software renderer: %s", line);
    }
    // Two discrete cards and a choice of one of them: any discrete card is a fine choice, nothing to say about the other.
    {
        GpucCard two[] = {{NVIDIA, GPUC_DISCRETE, 1}, {"AMD Radeon RX 6700 (RADV NAVI22)", GPUC_DISCRETE, 1}};
        CHECK(gpuc_choice_line(two, N(two), 1, 1, NULL, line, sizeof line, note, sizeof note) == -1 && !line[0] && !note[0],
              "a discrete card chosen over another discrete card is no warning");
    }
    // One card: nothing to compare with.
    {
        GpucCard one[] = {{INTEL, GPUC_INTEGRATED, 1}};
        CHECK(gpuc_choice_line(one, 1, 0, 0, NULL, line, sizeof line, note, sizeof note) == -1 && !line[0], "one card: no warning");
    }
    // The card choice line at each start: what the settings hold, agreeing or not.
    gpuc_setting_line(day2, 3, -1, NULL, line, sizeof line);
    CHECK(!strcmp(line, "vk: card choice: auto (gpu_index and gpu_name unset): a discrete card first"), "auto: %s", line);
    gpuc_setting_line(day2, 3, -1, "", line, sizeof line);
    CHECK(!strncmp(line, "vk: card choice: auto", 21), "an empty name is auto: %s", line);
    gpuc_setting_line(day2, 3, 1, NULL, line, sizeof line);
    CHECK(!strcmp(line, "vk: card choice: gpu_index = 1 alone, which lists \"" INTEL "\" (gpu_name unset: the position "
                        "decides)"), "index alone: %s", line);
    gpuc_setting_line(day2, 3, 7, NULL, line, sizeof line);
    CHECK(!strcmp(line, "vk: card choice: gpu_index = 7 alone, which lists no card (3 are listed)"), "index past: %s", line);
    gpuc_setting_line(day2, 3, -1, "nvidia", line, sizeof line);
    CHECK(!strcmp(line, "vk: card choice: gpu_name = \"nvidia\" alone (gpu_index unset), which gpu 0 \"" NVIDIA "\" has"),
          "name alone: %s", line);
    gpuc_setting_line(day2, 3, -1, "Radeon", line, sizeof line);
    CHECK(!strcmp(line, "vk: card choice: gpu_name = \"Radeon\" alone (gpu_index unset), which no card listed has"),
          "name no card has: %s", line);
    gpuc_setting_line(day1, 3, 1, NVIDIA, line, sizeof line);
    CHECK(!strcmp(line, "vk: card choice: gpu_index = 1, gpu_name = \"" NVIDIA "\" (both agree)"), "both agree: %s", line);
    gpuc_setting_line(day2, 3, 1, NVIDIA, line, sizeof line);
    CHECK(!strcmp(line, "vk: card choice: gpu_index = 1, gpu_name = \"" NVIDIA "\" (they disagree: the name decides)"),
          "both disagree: %s", line);
    gpuc_setting_line(day2, 3, 7, NVIDIA, line, sizeof line);
    CHECK(strstr(line, "(they disagree: the name decides)") != NULL, "a position past the list disagrees: %s", line);

    // A line longer than the buffer is cut, not overrun.
    {
        char tiny[40];
        alt = gpuc_choice_line(day2, 3, 1, 1, NULL, tiny, sizeof tiny, NULL, 0);
        CHECK(alt == 0 && strlen(tiny) == sizeof tiny - 1, "a short buffer takes what fits");
    }

    if (fails) printf("%d checks failed\n", fails);
    else printf("gpu_choice_check: all checks passed\n");
    return fails != 0;
}
