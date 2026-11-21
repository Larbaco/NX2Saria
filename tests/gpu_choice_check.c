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
    CHECK(gpuc_has("a", "a") && gpuc_has("ab", "a") && gpuc_has("ab", "B") && !gpuc_has("ab", "c") && !gpuc_has("", "a"),
          "a one-character part");

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

    // A position equal to the count lists no card, and the card after the list (a sentinel that has the name) is never read.
    {
        static const GpucCard past[] = {{NVIDIA, GPUC_DISCRETE, 1}, {INTEL, GPUC_INTEGRATED, 1}, {INTEL, GPUC_INTEGRATED, 1},
                                        {NVIDIA, GPUC_DISCRETE, 1}};
        CHECK(idx(past, 3, 3, NVIDIA, &dropped) == -1 && dropped, "a position == n with a name: dropped, not read");
        CHECK(gpuc_shown(past, 3, 3, NULL, 0) == -1, "a position == n alone shows Auto");
        CHECK(gpuc_shown(past, 3, 3, "NVIDIA", 0) == 0, "a position == n over a name: the name's card");
        char l[512];
        gpuc_dropped_line(past, 3, 3, NVIDIA, l, sizeof l);
        CHECK(!strcmp(l, "vk: gpu_index = 3 lists no card (3 are listed): gpu_name = " NVIDIA " decides"), "dropped, == n: %s", l);
    }

    {   // a name that only the card after the list has: no card listed has it
        static const GpucCard after[] = {{INTEL, GPUC_INTEGRATED, 1}, {INTEL, GPUC_INTEGRATED, 1}, {INTEL, GPUC_INTEGRATED, 1},
                                         {NVIDIA, GPUC_DISCRETE, 1}};
        char l[512];
        CHECK(gpuc_shown(after, 3, -1, "NVIDIA", 0) == -1, "a name only the card after the list has: none");
        gpuc_setting_line(after, 3, -1, "NVIDIA", 0, l, sizeof l);
        CHECK(!strcmp(l, "vk: card choice: gpu_name = \"NVIDIA\" alone (gpu_index unset), which no card listed has"), "name after the list: %s", l);
        // the first card with the name is the one named, also when it is gpu 0 and another follows
        gpuc_setting_line(day1, 3, -1, "Intel", 0, l, sizeof l);
        CHECK(!strcmp(l, "vk: card choice: gpu_name = \"Intel\" alone (gpu_index unset), which gpu 0 \"" INTEL "\" has"), "first match: %s", l);
        CHECK(gpuc_shown(day1, 3, -1, "Intel", 0) == 0, "the first card with the name is shown");
    }

    // gpuc_shown: the menu's row shows the card the settings pick, which the game picks too.
    CHECK(gpuc_shown(day1, 3, 1, NULL, 0) == 1, "an older file: the position");
    CHECK(gpuc_shown(day2, 3, 1, NULL, 0) == -1, "an older file the next day: the position lists the Intel graphics, which gives way to Auto");
    CHECK(gpuc_shown(day2, 3, 1, NULL, 1) == 1, "the same position from the environment: it stands, the Intel graphics");
    CHECK(gpuc_shown(day2, 3, 1, NVIDIA, 0) == 0, "the menu's two settings the next day: the name's card");
    CHECK(gpuc_shown(day2, 3, -1, "NVIDIA", 0) == 0, "a name alone");
    CHECK(gpuc_shown(day2, 3, -1, NULL, 0) == -1, "Auto");
    CHECK(gpuc_shown(day2, 3, -1, "Radeon", 0) == -1, "a name no card has: the game takes the best card, as for Auto");
    CHECK(gpuc_shown(day2, 3, 7, NULL, 0) == -1, "a position past the list");
    CHECK(gpuc_shown(day2, 3, 1, "Radeon", 0) == -1, "a position over a name no card has: the name drops it, none left");
    // The menu reads an unset gpu_name as an empty string, not NULL
    CHECK(gpuc_shown(day2, 3, 1, "", 0) == -1, "an empty name is no name: the Intel position gives way");
    CHECK(gpuc_shown(day1, 3, 1, "", 0) == 1 && gpuc_shown(day2, 3, -1, "", 0) == -1, "an empty name: a discrete position, and Auto");
    // Position 0 is a position
    CHECK(gpuc_shown(day2, 3, 0, NULL, 0) == 0, "gpu_index = 0 alone on the NVIDIA card is shown");
    CHECK(gpuc_shown(day1, 3, 0, NULL, 0) == -1, "gpu_index = 0 alone on the Intel graphics gives way");
    CHECK(gpuc_shown(day1, 3, 0, NULL, 1) == 0, "and from the environment it stands");

    // gpuc_legacy_alt: a gpu_index alone that lists a card that is not a discrete one gives way to the discrete card.
    // A position written while the NVIDIA card was gpu 1 (day1's order) or gpu 0 (day2's) names the Intel graphics in
    // the other order.
    CHECK(gpuc_legacy_alt(day2, 3, 1, NULL, 0) == 0, "gpu_index = 1 alone, which lists the Intel graphics: the NVIDIA card, gpu 0");
    CHECK(gpuc_legacy_alt(day1, 3, 0, NULL, 0) == 1, "gpu_index = 0 alone in the other order: the NVIDIA card, gpu 1");
    CHECK(gpuc_legacy_alt(day1, 3, 2, NULL, 0) == 1, "the Intel graphics' other listing: the same card");
    CHECK(gpuc_legacy_alt(day2, 3, 0, NULL, 0) == -1, "gpu_index = 0 alone, which lists the NVIDIA card: the position stands");
    CHECK(gpuc_legacy_alt(day1, 3, 1, NULL, 0) == -1, "gpu_index = 1 alone, which lists the NVIDIA card: the position stands");
    CHECK(gpuc_legacy_alt(day2, 3, 1, "", 0) == 0, "an empty name is no name");
    CHECK(gpuc_legacy_alt(day2, 3, 1, "Intel", 0) == -1, "a gpu_name is the player's own choice of the Intel graphics");
    CHECK(gpuc_legacy_alt(day2, 3, 1, NVIDIA, 0) == -1, "and of any card: nothing to give way for");
    CHECK(gpuc_legacy_alt(day2, 3, 1, NULL, 1) == -1, "an index from the environment is set for the run on purpose");
    CHECK(gpuc_legacy_alt(day2, 3, -1, NULL, 0) == -1, "Auto");
    CHECK(gpuc_legacy_alt(day2, 3, 7, NULL, 0) == -1, "a position past the list is already Auto");
    CHECK(gpuc_legacy_alt(day2, 3, -2, NULL, 0) == -1, "a negative position is no position");
    CHECK(gpuc_legacy_applies(day2, 3, 1, NULL, 0) && !gpuc_legacy_applies(day2, 3, 0, NULL, 0) &&
              !gpuc_legacy_applies(day2, 3, 1, NULL, 1) && !gpuc_legacy_applies(day2, 3, 1, "x", 0) &&
              !gpuc_legacy_applies(day2, 3, -1, NULL, 0) && !gpuc_legacy_applies(day2, 3, 3, NULL, 0),
          "gpuc_legacy_applies: the rule's part that needs no card's usable");
    {   // the discrete card the game cannot use is no card to give way to
        GpucCard broken[] = {{INTEL, GPUC_INTEGRATED, 1}, {NVIDIA, GPUC_DISCRETE, 0}};
        CHECK(gpuc_legacy_alt(broken, N(broken), 0, NULL, 0) == -1, "a discrete card the game cannot use: the position stands");
        // the first usable one is the card to give way to
        GpucCard two[] = {{INTEL, GPUC_INTEGRATED, 1}, {NVIDIA, GPUC_DISCRETE, 0}, {"AMD Radeon RX 6700 (RADV NAVI22)", GPUC_DISCRETE, 1}};
        CHECK(gpuc_legacy_alt(two, N(two), 0, NULL, 0) == 2, "the first discrete card the game can use");
        GpucCard both[] = {{INTEL, GPUC_INTEGRATED, 1}, {NVIDIA, GPUC_DISCRETE, 1}, {"AMD Radeon RX 6700 (RADV NAVI22)", GPUC_DISCRETE, 1}};
        CHECK(gpuc_legacy_alt(both, N(both), 0, NULL, 0) == 1, "of two the game can use, the first in the list");
        // no discrete card at all
        GpucCard none[] = {{INTEL, GPUC_INTEGRATED, 1}, {INTEL, GPUC_INTEGRATED, 1}};
        CHECK(gpuc_legacy_alt(none, N(none), 1, NULL, 0) == -1, "two integrated listings and nothing else: the position stands");
        GpucCard one[] = {{INTEL, GPUC_INTEGRATED, 1}};
        CHECK(gpuc_legacy_alt(one, 1, 0, NULL, 0) == -1, "one card: nothing to give way to");
        // a software renderer chosen by position beside a card
        GpucCard bench[] = {{"AMD Radeon RX 6700 (RADV NAVI22)", GPUC_DISCRETE, 1}, {"llvmpipe (LLVM 20.1.2, 256 bits)", GPUC_CPU, 0}};
        CHECK(gpuc_legacy_alt(bench, N(bench), 1, NULL, 0) == 0, "a software renderer by position: the discrete card");
        // a discrete card chosen over another discrete card
        GpucCard dd[] = {{NVIDIA, GPUC_DISCRETE, 1}, {"AMD Radeon RX 6700 (RADV NAVI22)", GPUC_DISCRETE, 1}};
        CHECK(gpuc_legacy_alt(dd, N(dd), 1, NULL, 0) == -1, "a discrete card at the position: it stands, whatever else is listed");
        // a virtual GPU, and a GPU of no known kind, are no discrete cards either
        GpucCard virt[] = {{"A virtual GPU", GPUC_VIRTUAL, 1}, {NVIDIA, GPUC_DISCRETE, 1}};
        CHECK(gpuc_legacy_alt(virt, N(virt), 0, NULL, 0) == 1, "a virtual GPU at the position gives way");
        GpucCard other[] = {{"A GPU of no kind", GPUC_OTHER, 1}, {NVIDIA, GPUC_DISCRETE, 1}};
        CHECK(gpuc_legacy_alt(other, N(other), 0, NULL, 0) == 1, "and a GPU of no known kind");
    }

    // gpuc_resolve: gpuc_index's position, or Auto when the position gives way; the device choice and the preset's probe both
    // call it, so a preset is chosen for the card the game takes.
    {
        int alt;
        CHECK(gpuc_resolve(day2, 3, 1, NULL, 0, &dropped, &alt) == -1 && alt == 0 && !dropped,
              "the position alone on the Intel graphics goes as Auto, to gpu 0");
        CHECK(gpuc_resolve(day2, 3, 1, NULL, 1, &dropped, &alt) == 1 && alt == -1 && !dropped, "from the environment: gpu 1");
        CHECK(gpuc_resolve(day2, 3, 0, NULL, 0, &dropped, &alt) == 0 && alt == -1 && !dropped, "a discrete card at it: gpu 0");
        CHECK(gpuc_resolve(day2, 3, 1, NVIDIA, 0, &dropped, &alt) == -1 && alt == -1 && dropped,
              "the menu's two settings in the other order: the name decides, the position is dropped, no legacy");
        CHECK(gpuc_resolve(day2, 3, 1, "Intel", 0, &dropped, &alt) == 1 && alt == -1 && !dropped,
              "the Intel graphics chosen in the menu: gpu 1 stands");
        CHECK(gpuc_resolve(day2, 3, -1, NULL, 0, &dropped, &alt) == -1 && alt == -1 && !dropped, "Auto stays Auto");
    }

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

    // Position 0 is a position too: the environment's gpu_index = 0 on the Intel graphics, listed first.
    CHECK(gpuc_choice_line(day1, 3, 0, 0, NULL, line, sizeof line, note, sizeof note) == 1 && strstr(line, "gpu 0 \"" INTEL "\"") &&
              strstr(line, "gpu_index = 0, while gpu 1 \"" NVIDIA "\""),
          "an index 0 on the Intel graphics warns: %s", line);
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
    gpuc_setting_line(day2, 3, -1, NULL, 0, line, sizeof line);
    CHECK(!strcmp(line, "vk: card choice: auto (gpu_index and gpu_name unset): a discrete card first"), "auto: %s", line);
    gpuc_setting_line(day2, 3, -1, "", 0, line, sizeof line);
    CHECK(!strncmp(line, "vk: card choice: auto", 21), "an empty name is auto: %s", line);
    gpuc_setting_line(day1, 3, 1, NULL, 0, line, sizeof line);
    CHECK(!strcmp(line, "vk: card choice: gpu_index = 1 alone, which lists \"" NVIDIA "\" (gpu_name unset: the position "
                        "decides)"), "index alone on a discrete card: %s", line);
    gpuc_setting_line(day2, 3, 1, NULL, 0, line, sizeof line);
    CHECK(!strcmp(line, "vk: card choice: gpu_index = 1 alone, which lists \"" INTEL "\" (gpu_name unset, and it is not a "
                        "discrete card: it counts as auto)"), "index alone on the Intel graphics: %s", line);
    gpuc_setting_line(day2, 3, 1, NULL, 1, line, sizeof line);
    CHECK(!strcmp(line, "vk: card choice: gpu_index = 1 alone, which lists \"" INTEL "\" (gpu_name unset: the position "
                        "decides)"), "index alone from the environment: the position decides: %s", line);
    gpuc_setting_line(day2, 3, 0, NULL, 0, line, sizeof line);
    CHECK(!strcmp(line, "vk: card choice: gpu_index = 0 alone, which lists \"" NVIDIA "\" (gpu_name unset: the position "
                        "decides)"), "index 0 alone: %s", line);
    gpuc_setting_line(day2, 3, 0, NVIDIA, 0, line, sizeof line);
    CHECK(!strcmp(line, "vk: card choice: gpu_index = 0, gpu_name = \"" NVIDIA "\" (both agree)"), "index 0 and a name: %s", line);
    gpuc_setting_line(day2, 3, 7, NULL, 0, line, sizeof line);
    CHECK(!strcmp(line, "vk: card choice: gpu_index = 7 alone, which lists no card (3 are listed)"), "index past: %s", line);
    gpuc_setting_line(day2, 3, 3, NULL, 0, line, sizeof line);   // the first position past the list: no card to name
    CHECK(!strcmp(line, "vk: card choice: gpu_index = 3 alone, which lists no card (3 are listed)"), "index == n: %s", line);
    {
        GpucCard one[] = {{INTEL, GPUC_INTEGRATED, 1}};
        gpuc_setting_line(one, 1, 1, NULL, 0, line, sizeof line);
        CHECK(!strcmp(line, "vk: card choice: gpu_index = 1 alone, which lists no card (1 are listed)"), "one card, index 1: %s", line);
    }
    gpuc_setting_line(day2, 3, -1, "nvidia", 0, line, sizeof line);
    CHECK(!strcmp(line, "vk: card choice: gpu_name = \"nvidia\" alone (gpu_index unset), which gpu 0 \"" NVIDIA "\" has"),
          "name alone: %s", line);
    gpuc_setting_line(day2, 3, -1, "Radeon", 0, line, sizeof line);
    CHECK(!strcmp(line, "vk: card choice: gpu_name = \"Radeon\" alone (gpu_index unset), which no card listed has"),
          "name no card has: %s", line);
    gpuc_setting_line(day1, 3, 1, NVIDIA, 0, line, sizeof line);
    CHECK(!strcmp(line, "vk: card choice: gpu_index = 1, gpu_name = \"" NVIDIA "\" (both agree)"), "both agree: %s", line);
    gpuc_setting_line(day2, 3, 1, NVIDIA, 0, line, sizeof line);
    CHECK(!strcmp(line, "vk: card choice: gpu_index = 1, gpu_name = \"" NVIDIA "\" (they disagree: the name decides)"),
          "both disagree: %s", line);
    gpuc_setting_line(day2, 3, 7, NVIDIA, 0, line, sizeof line);
    CHECK(strstr(line, "(they disagree: the name decides)") != NULL, "a position past the list disagrees: %s", line);

    // The line for a position alone that gives way to Auto: the position and what it listed, the card taken instead, and how
    // to run on the Intel graphics on purpose.
    alt = gpuc_legacy_alt(day2, 3, 1, NULL, 0);
    gpuc_legacy_line(day2, 1, alt, line, sizeof line);
    CHECK(!strcmp(line, "vk: gpu_index = 1 alone lists \"" INTEL "\" (integrated GPU) while gpu 0 \"" NVIDIA "\" is a discrete "
                        "card, so it counts as Auto and the game takes the discrete card. The order the cards are listed in "
                        "can change from one start to the next (the system's graphics settings for the game, a display on "
                        "one card, the power source), and a position alone, which is all a settings file from before the menu "
                        "kept the card's name has, then names another card. To run on \"" INTEL "\" instead, set gpu_name = " INTEL
                        " in the settings file, or choose it in the menu's Graphics card row, which writes gpu_name beside the "
                        "position"),
          "legacy line: %s", line);
    CHECK(strlen(line) < 800, "the legacy line fits vk_device.c's 800 byte buffer, with real names: %zu", strlen(line));

    // The shader preparation's line: which card its driver cache is built for, and by what. The cases are the ones the
    // laptop's log shows in the update (an index alone that lists the NVIDIA card), and those that would not be.
    gpuc_prepare_line(day2, 3, 0, GPUC_BY_POSITION, 0, NULL, line, sizeof line);
    CHECK(!strcmp(line, "building for gpu 0 \"" NVIDIA "\" (discrete card), chosen by gpu_index = 0 alone"),
          "prepare, a position alone on the NVIDIA card: %s", line);
    gpuc_prepare_line(day2, 3, 0, GPUC_BY_AUTO, -1, NULL, line, sizeof line);
    CHECK(!strcmp(line, "building for gpu 0 \"" NVIDIA "\" (discrete card), chosen by auto"), "prepare, auto: %s", line);
    gpuc_prepare_line(day1, 3, 1, GPUC_BY_LEGACY, 0, NULL, line, sizeof line);
    CHECK(!strcmp(line, "building for gpu 1 \"" NVIDIA "\" (discrete card), chosen by auto, as gpu_index = 0 alone listed a "
                        "card that is not a discrete one"), "prepare, a position that gave way: %s", line);
    gpuc_prepare_line(day1, 3, 1, GPUC_BY_NAME, 2, NVIDIA, line, sizeof line);
    CHECK(!strcmp(line, "building for gpu 1 \"" NVIDIA "\" (discrete card), chosen by gpu_name = \"" NVIDIA "\""),
          "prepare, by the name: %s", line);
    gpuc_prepare_line(day2, 3, 0, GPUC_BY_FALLBACK, 5, NULL, line, sizeof line);
    CHECK(!strcmp(line, "building for gpu 0 \"" NVIDIA "\" (discrete card), chosen by auto, as the card the settings choose "
                        "cannot be used"), "prepare, the choice cannot be used: %s", line);
    // The card is not a discrete one while a discrete one the game can use is listed: the player's own choice of the Intel
    // graphics by its name (kept on purpose), and an index from the environment.
    gpuc_prepare_line(day2, 3, 1, GPUC_BY_NAME, 1, "Intel", line, sizeof line);
    CHECK(!strcmp(line, "building for gpu 1 \"" INTEL "\" (integrated GPU), chosen by gpu_name = \"Intel\"; gpu 0 \"" NVIDIA
                        "\" is a discrete card, and its driver cache is not the one built here"),
          "prepare, the Intel graphics by its name: %s", line);
    gpuc_prepare_line(day2, 3, 1, GPUC_BY_POSITION, 1, NULL, line, sizeof line);
    CHECK(strstr(line, "(integrated GPU), chosen by gpu_index = 1 alone; gpu 0 \"" NVIDIA "\" is a discrete card") != NULL,
          "prepare, the Intel graphics by an index from the environment: %s", line);
    {   // a software renderer by its name while two discrete cards the game can use are listed: the note names the first
        GpucCard cpu[] = {{"llvmpipe (LLVM 20.1.2, 256 bits)", GPUC_CPU, 0}, {NVIDIA, GPUC_DISCRETE, 1},
                          {"AMD Radeon RX 6700 (RADV NAVI22)", GPUC_DISCRETE, 1}};
        gpuc_prepare_line(cpu, N(cpu), 0, GPUC_BY_NAME, -1, "llvmpipe", line, sizeof line);
        CHECK(!strcmp(line, "building for gpu 0 \"llvmpipe (LLVM 20.1.2, 256 bits)\" (software renderer), chosen by gpu_name = "
                            "\"llvmpipe\"; gpu 1 \"" NVIDIA "\" is a discrete card, and its driver cache is not the one built here"),
              "prepare, a software renderer by its name: %s", line);
    }
    {   // two discrete cards the game can use, the first at gpu 0: the note names that one, not the last
        GpucCard two_d[] = {{NVIDIA, GPUC_DISCRETE, 1}, {INTEL, GPUC_INTEGRATED, 1}, {"AMD Radeon RX 6700 (RADV NAVI22)", GPUC_DISCRETE, 1}};
        gpuc_prepare_line(two_d, N(two_d), 1, GPUC_BY_NAME, 1, "Intel", line, sizeof line);
        CHECK(strstr(line, "; gpu 0 \"" NVIDIA "\" is a discrete card") != NULL, "prepare, the first of two discrete cards: %s", line);
        alt = gpuc_choice_line(two_d, N(two_d), 1, 1, NULL, line, sizeof line, note, sizeof note);
        CHECK(alt == 0 && strstr(line, "while gpu 0 \"" NVIDIA "\" is a discrete card") != NULL, "choice line, the first of two: %s", line);
        // a card picked past the list, or before it, gives no line (the cards around the list would make one)
        static const GpucCard around[] = {{INTEL, GPUC_INTEGRATED, 1}, {NVIDIA, GPUC_DISCRETE, 1}, {INTEL, GPUC_INTEGRATED, 1},
                                          {INTEL, GPUC_INTEGRATED, 1}};
        gpuc_prepare_line(around + 1, 2, 2, GPUC_BY_AUTO, -1, NULL, line, sizeof line);
        CHECK(!line[0], "prepare, picked == n: no line");
        gpuc_prepare_line(around + 1, 2, -1, GPUC_BY_AUTO, -1, NULL, line, sizeof line);
        CHECK(!line[0], "prepare, picked == -1: no line");
        CHECK(gpuc_choice_line(around + 1, 2, 2, 0, NULL, line, sizeof line, note, sizeof note) == -1 && !line[0],
              "choice line, picked == n: nothing");
        CHECK(gpuc_choice_line(around + 1, 2, -1, 0, NULL, line, sizeof line, note, sizeof note) == -1 && !line[0],
              "choice line, picked == -1: nothing");
    }
    {   // no discrete card the game can use, one the game cannot use, and two discrete cards: nothing more to say
        GpucCard broken[] = {{INTEL, GPUC_INTEGRATED, 1}, {NVIDIA, GPUC_DISCRETE, 0}};
        gpuc_prepare_line(broken, N(broken), 0, GPUC_BY_POSITION, 0, NULL, line, sizeof line);
        CHECK(!strcmp(line, "building for gpu 0 \"" INTEL "\" (integrated GPU), chosen by gpu_index = 0 alone"),
              "prepare, a discrete card the game cannot use: %s", line);
        GpucCard two[] = {{NVIDIA, GPUC_DISCRETE, 1}, {"AMD Radeon RX 6700 (RADV NAVI22)", GPUC_DISCRETE, 1}};
        gpuc_prepare_line(two, N(two), 1, GPUC_BY_POSITION, 1, NULL, line, sizeof line);
        CHECK(!strstr(line, "is a discrete card"), "prepare, a discrete card over another discrete card: %s", line);
        gpuc_prepare_line(two, N(two), 2, GPUC_BY_AUTO, -1, NULL, line, sizeof line);
        CHECK(!line[0], "prepare, a card past the list: no line");
        gpuc_prepare_line(two, N(two), -1, GPUC_BY_AUTO, -1, NULL, line, sizeof line);
        CHECK(!line[0], "prepare, no card: no line");
    }

    // The laptop's runs, replayed through gpuc_resolve, which pick_device and probe_gpu both call, and then through a
    // model of the score the pick gives (vk_device.c pick_device: the chosen position 100, discrete 3, integrated 2,
    // other 1; the first of the best), which the test does not share with them: a gpu_index = 1 alone while the NVIDIA
    // card is gpu 0, the NVIDIA card's gpu_index = 0 alone in the same order, that gpu_index = 0 once the order has the
    // Intel graphics first, an empty settings file, and the Intel graphics chosen in the menu.
    {
        struct { const GpucCard *c; int want; const char *name; int pick; int alt; } runs[] = {
            {day2, 1, NULL, 0, 0},      // gpu_index = 1 alone, the Intel graphics: Auto, the NVIDIA card
            {day2, 0, NULL, 0, -1},     // gpu_index = 0 alone, the NVIDIA card: as written
            {day1, 0, NULL, 1, 1},      // the same file when the Intel graphics is first: Auto, the NVIDIA card
            {day1, -1, NULL, 1, -1},    // an empty settings file: Auto
            {day2, 0, "Intel", 1, -1},  // the player chose the Intel graphics in the menu (the name): kept
        };
        for (size_t k = 0; k < sizeof runs / sizeof *runs; k++) {
            int alt2, got = gpuc_resolve(runs[k].c, 3, runs[k].want, runs[k].name, 0, &dropped, &alt2);
            int best = -1, best_score = -1;
            for (int i = 0; i < 3; i++) {
                if (got >= 0 && i != got) continue;
                if (runs[k].name && *runs[k].name && !gpuc_has(runs[k].c[i].name, runs[k].name)) continue;
                int score = i == got ? 100 : runs[k].c[i].type == GPUC_DISCRETE ? 3 : runs[k].c[i].type == GPUC_INTEGRATED ? 2 : 1;
                if (score > best_score) best_score = score, best = i;
            }
            CHECK(best == runs[k].pick && alt2 == runs[k].alt, "replayed run %zu: card %d (want %d), alt %d (want %d)", k, best,
                  runs[k].pick, alt2, runs[k].alt);
        }
    }

    // A line longer than the buffer is cut, not overrun.
    {
        char tiny[40];
        alt = gpuc_choice_line(day2, 3, 1, 1, NULL, tiny, sizeof tiny, NULL, 0);
        CHECK(alt == 0 && strlen(tiny) == sizeof tiny - 1, "a short buffer takes what fits");
        gpuc_legacy_line(day2, 1, 0, tiny, sizeof tiny);
        CHECK(strlen(tiny) == sizeof tiny - 1, "the legacy line too");
        gpuc_prepare_line(day2, 3, 1, GPUC_BY_NAME, 1, "Intel", tiny, sizeof tiny);
        CHECK(strlen(tiny) == sizeof tiny - 1, "and the preparation's");
    }

    if (fails) printf("%d checks failed\n", fails);
    else printf("gpu_choice_check: all checks passed\n");
    return fails != 0;
}
