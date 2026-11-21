// The graphics card the settings choose (gpu_index, gpu_name) and what the log says about the choice: vk_device.c
// pick_device and fatal_note, hw_detect.c probe_gpu, ui/menu.cpp row_gpu. Header only, and plain enough for C and C++,
// so that tests/gpu_choice_check.c checks the rule and every line without a device.
//
// The order the cards are listed in is not fixed. Vulkan says nothing of it, and on Windows the loader follows the order
// of the system's graphics adapters, which the system's graphics settings for the program, a display on one card and
// the power source change. A laptop with an RTX 3050 and Intel UHD Graphics listed the Intel graphics first at one start
// and the NVIDIA card first at the next, with the game unchanged: gpu_index = 1, chosen in the menu while 1 was the
// NVIDIA card, then ran the game on the Intel graphics. Those share the system's memory, which on 16 GB leaves them
// about 7 GB; the video memory budget was used up at the first save load and the device was lost (exit code 72), at
// every start that loaded a save.
//
// So a position is no lasting choice. The menu writes the card's name beside it (gpu_name), and when gpu_index lists
// a card that does not have that name in its own, the name decides (gpuc_index).
//
// A gpu_index written alone, as every settings file from before the menu kept the name has it, is the weak case: it
// names whatever card is listed at that position now, and the game and the shader preparation both went to it, the
// preparation filling the Intel graphics' driver cache of the laptop above while the NVIDIA card's stayed cold. When
// that card is not a discrete one while a discrete one the game can use is listed, the position now counts as Auto
// (gpuc_legacy_alt), with a line in the log. Only the menu's choice of a card writes a name, so a card that is not a
// discrete one is kept on purpose by choosing it there. A gpu_index that the environment sets (NX2SARIA_GPU_INDEX) is
// set for that run on purpose and goes by its position; the log then warns when it puts the game on a card that is not
// a discrete one while a discrete one the game can use is listed (gpuc_choice_line). A card picked through gpu_name is
// the player's own choice and gets no such line.
// At each start the log also says what the two settings hold, agreeing or not (gpuc_setting_line), and the shader
// preparation says which card it builds the driver's cache for (gpuc_prepare_line).
#pragma once
#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

// VkPhysicalDeviceType's values, so that this header needs no Vulkan header.
enum { GPUC_OTHER = 0, GPUC_INTEGRATED = 1, GPUC_DISCRETE = 2, GPUC_VIRTUAL = 3, GPUC_CPU = 4 };

typedef struct {
    const char *name;   // the name the driver gives the card
    int type;           // GPUC_*
    int usable;         // the game can run on it: Vulkan 1.3, its extensions, a queue that presents. Read for discrete cards
                        // only: gpuc_legacy_alt, gpuc_choice_line and gpuc_prepare_line look for one the game can use
} GpucCard;

// 1 when part is in s, upper and lower case alike: gpu_name's match.
static inline int gpuc_has(const char *s, const char *part) {
    if (!s || !part) return 0;
    size_t n = strlen(part);
    for (; *s; s++) {
        size_t i = 0;
        while (i < n && s[i] && tolower((unsigned char)s[i]) == tolower((unsigned char)part[i])) i++;
        if (i == n) return 1;
    }
    return 0;
}

// The position to go by, for gpu_index want (-1: unset) and gpu_name name (NULL or empty: unset) over the n cards listed
// now: want, unless a name is set and the card at that position does not have it (the cards are listed in another order
// than when gpu_index was written, or one of them is gone). Then -1, *dropped is 1 and the name finds the card. Nothing
// is dropped while one of the two is unset.
static inline int gpuc_index(const GpucCard *c, int n, int want, const char *name, int *dropped) {
    *dropped = 0;
    if (want < 0 || !name || !*name) return want;
    if (want < n && gpuc_has(c[want].name, name)) return want;
    *dropped = 1;
    return -1;
}

// 1 when a gpu_index alone lists a card that is not a discrete one, the part of gpuc_legacy_alt that needs no card's
// usable: want (-1: unset) over the n cards listed now lists a card that is not a discrete one, no gpu_name is set (a
// name is the player's own choice of that card), and the index is not the environment's (from_env: set for this run, on
// purpose). A caller that has to find out which cards the game can use does so only when this is 1.
static inline int gpuc_legacy_applies(const GpucCard *c, int n, int want, const char *name, int from_env) {
    return !(want < 0 || want >= n || from_env || (name && *name) || c[want].type == GPUC_DISCRETE);
}

// The discrete card a gpu_index alone gives way to, or -1 when the position stands: the first discrete card the game can
// use, when gpuc_legacy_applies. Nothing to give way for with Auto, a position past the list, a discrete card at it, a
// name, an environment setting, or with no discrete card to use (the card at the position is then the only one that
// runs the game).
static inline int gpuc_legacy_alt(const GpucCard *c, int n, int want, const char *name, int from_env) {
    if (!gpuc_legacy_applies(c, n, want, name, from_env)) return -1;
    for (int i = 0; i < n; i++)
        if (c[i].type == GPUC_DISCRETE && c[i].usable) return i;
    return -1;
}

// The position the pick goes by for the settings want and name (as gpuc_index and gpuc_legacy_alt take them), the one
// call for the device choice (vk_device.c pick_device) and the preset's probe (hw_detect.c probe_gpu), so that the preset
// is chosen for the card the game takes: gpuc_index's result, or -1 when a gpu_index alone gives way to a discrete card.
// *dropped is gpuc_index's, *alt gpuc_legacy_alt's (-1: the position stands); the caller says either in the log.
static inline int gpuc_resolve(const GpucCard *c, int n, int want, const char *name, int from_env, int *dropped, int *alt) {
    want = gpuc_index(c, n, want, name, dropped);
    *alt = gpuc_legacy_alt(c, n, want, name, from_env);
    return *alt >= 0 ? -1 : want;
}

// The card the settings pick as far as the list tells, for the menu's Graphics card row: the one gpu_index lists when it
// has the name (or no name is set), else the first with the name; -1 for Auto, for a position past the list, for a name
// that no card has (the game then takes the best card, as for Auto) and for a gpu_index alone that gives way to a discrete
// card (gpuc_legacy_alt; from_env: the index is the environment's).
static inline int gpuc_shown(const GpucCard *c, int n, int want, const char *name, int from_env) {
    int dropped;
    want = gpuc_index(c, n, want, name, &dropped);
    if (name && *name) {
        if (want >= 0) return want;
        for (int i = 0; i < n; i++)
            if (gpuc_has(c[i].name, name)) return i;
        return -1;
    }
    if (gpuc_legacy_alt(c, n, want, name, from_env) >= 0) return -1;
    return want >= 0 && want < n ? want : -1;
}

static inline const char *gpuc_kind(int type) {
    switch (type) {
    case GPUC_DISCRETE: return "discrete card";
    case GPUC_INTEGRATED: return "integrated GPU";
    case GPUC_CPU: return "software renderer";
    case GPUC_VIRTUAL: return "virtual GPU";
    default: return "GPU";
    }
}

// The line at each start that says what the two settings hold over the cards listed now, agreeing or not, with want and
// name as the settings file gives them (-1 and NULL or empty: unset), and from_env as for gpuc_legacy_alt.
static inline void gpuc_setting_line(const GpucCard *c, int n, int want, const char *name, int from_env, char *line,
                                     size_t cap) {
    int named = name && *name;
    if (want < 0 && !named) {
        snprintf(line, cap, "vk: card choice: auto (gpu_index and gpu_name unset): a discrete card first");
        return;
    }
    if (!named) {
        if (want >= n)
            snprintf(line, cap, "vk: card choice: gpu_index = %d alone, which lists no card (%d are listed)", want, n);
        else if (gpuc_legacy_alt(c, n, want, name, from_env) >= 0)
            snprintf(line, cap, "vk: card choice: gpu_index = %d alone, which lists \"%s\" (gpu_name unset, and it is not a "
                     "discrete card: it counts as auto)", want, c[want].name);
        else
            snprintf(line, cap, "vk: card choice: gpu_index = %d alone, which lists \"%s\" (gpu_name unset: the position "
                     "decides)", want, c[want].name);
        return;
    }
    int has = -1;
    for (int i = 0; i < n && has < 0; i++)
        if (gpuc_has(c[i].name, name)) has = i;
    if (want < 0) {
        if (has >= 0)
            snprintf(line, cap, "vk: card choice: gpu_name = \"%s\" alone (gpu_index unset), which gpu %d \"%s\" has",
                     name, has, c[has].name);
        else
            snprintf(line, cap, "vk: card choice: gpu_name = \"%s\" alone (gpu_index unset), which no card listed has",
                     name);
        return;
    }
    int dropped;
    gpuc_index(c, n, want, name, &dropped);
    snprintf(line, cap, "vk: card choice: gpu_index = %d, gpu_name = \"%s\" (%s)", want, name,
             dropped ? "they disagree: the name decides" : "both agree");
}

// The line for a gpu_index that gpuc_index dropped, with the want it was given: what it listed, and that the name decides.
static inline void gpuc_dropped_line(const GpucCard *c, int n, int want, const char *name, char *line, size_t cap) {
    if (want < n)
        snprintf(line, cap,
                 "vk: gpu_index = %d lists \"%s\", which does not have gpu_name = %s in its name: the cards are listed in "
                 "another order than when gpu_index was written, so the name decides", want, c[want].name, name);
    else
        snprintf(line, cap, "vk: gpu_index = %d lists no card (%d are listed): gpu_name = %s decides", want, n, name);
}

// The line for a gpu_index alone that gives way to Auto (gpuc_legacy_alt), with the want it was given and the card alt it
// returned: what the position listed, and what to do to run on that card on purpose (the settings file's gpu_name too,
// for a game that cannot open its menu).
static inline void gpuc_legacy_line(const GpucCard *c, int want, int alt, char *line, size_t cap) {
    snprintf(line, cap,
             "vk: gpu_index = %d alone lists \"%s\" (%s) while gpu %d \"%s\" is a discrete card, so it counts as Auto and the "
             "game takes the discrete card. The order the cards are listed in can change from one start to the next (the "
             "system's graphics settings for the game, a display on one card, the power source), and a position alone, "
             "which is all a settings file from before the menu kept the card's name has, then names another card. To run "
             "on \"%s\" instead, set gpu_name = %s in the settings file, or choose it in the menu's Graphics card row, "
             "which writes gpu_name beside the position",
             want, c[want].name, gpuc_kind(c[want].type), alt, c[alt].name, c[want].name, c[want].name);
}

// How the card the game runs on was chosen, for gpuc_prepare_line.
enum { GPUC_BY_AUTO = 0, GPUC_BY_POSITION, GPUC_BY_NAME, GPUC_BY_LEGACY, GPUC_BY_FALLBACK };

// The text the shader preparation modes print after "prepare-shaders: card: " ("prepare-pipelines: card: " in that mode;
// vk_pipeline.c): the card whose driver cache they build, picked (an index into c) by how, which is a GPUC_BY_* value, with
// want and name as the settings give them. Said in the launcher's log, where a player's report of a slow update is
// read: the preparation's own device lines are among sixty others. Where the card is not a discrete one while a discrete
// one the game can use is listed (the player's own choice of the card by its name, or an index in the environment), it
// also says that one's driver cache is not the one built.
static inline void gpuc_prepare_line(const GpucCard *c, int n, int picked, int how, int want, const char *name, char *line,
                                     size_t cap) {
    line[0] = 0;
    if (picked < 0 || picked >= n) return;
    char by[256];
    switch (how) {
    case GPUC_BY_POSITION: snprintf(by, sizeof by, "gpu_index = %d alone", want); break;
    case GPUC_BY_NAME: snprintf(by, sizeof by, "gpu_name = \"%s\"", name ? name : ""); break;
    case GPUC_BY_LEGACY:
        snprintf(by, sizeof by, "auto, as gpu_index = %d alone listed a card that is not a discrete one", want);
        break;
    case GPUC_BY_FALLBACK:
        snprintf(by, sizeof by, "auto, as the card the settings choose cannot be used");
        break;
    default: snprintf(by, sizeof by, "auto"); break;
    }
    int alt = -1;
    if (c[picked].type != GPUC_DISCRETE)
        for (int i = 0; i < n && alt < 0; i++)
            if (i != picked && c[i].type == GPUC_DISCRETE && c[i].usable) alt = i;
    if (alt < 0)
        snprintf(line, cap, "building for gpu %d \"%s\" (%s), chosen by %s", picked, c[picked].name, gpuc_kind(c[picked].type),
                 by);
    else
        snprintf(line, cap,
                 "building for gpu %d \"%s\" (%s), chosen by %s; gpu %d \"%s\" is a discrete card, and its driver cache is "
                 "not the one built here", picked, c[picked].name, gpuc_kind(c[picked].type), by, alt, c[alt].name);
}

// The card the settings picked (want, name: what the pick went by after gpuc_index, -1 and NULL for Auto) is not a
// discrete card while one the game can use is listed, and it was chosen by a gpu_index alone: the first such card is
// returned, line gets the warning for the log and note (NULL: none wanted) the short form for a fatal error's report. A
// position says little over the cards' order changing from one start to the next. A gpu_name, which the menu writes
// beside the position, is the player's own choice of that card, so it gets no warning at each start. Returns -1 and
// empties both when there is nothing to say: Auto, a gpu_name set, a discrete card picked, or no discrete card to use.
static inline int gpuc_choice_line(const GpucCard *c, int n, int picked, int want, const char *name, char *line, size_t cap,
                                   char *note, size_t note_cap) {
    line[0] = 0;
    if (note) note[0] = 0;
    if (want < 0 || (name && *name) || picked < 0 || picked >= n || c[picked].type == GPUC_DISCRETE) return -1;
    int alt = -1;
    for (int i = 0; i < n && alt < 0; i++)
        if (i != picked && c[i].type == GPUC_DISCRETE && c[i].usable) alt = i;
    if (alt < 0) return -1;
    snprintf(line, cap,
             "vk: warning: gpu %d \"%s\" (%s) was chosen by gpu_index = %d, while gpu %d \"%s\" is a discrete card. The "
             "order the cards are listed in can change from one start to the next (the system's graphics settings for the "
             "game, a display on one card, the power source), and gpu_index then names another card. Auto, in the menu's "
             "Graphics card row, takes the discrete card; gpu_name = %s keeps it by its name",
             picked, c[picked].name, gpuc_kind(c[picked].type), want, alt, c[alt].name, c[alt].name);
    if (note)
        snprintf(note, note_cap, "card chosen by gpu_index = %d (%s) while gpu %d is a discrete card", want,
                 gpuc_kind(c[picked].type), alt);
    return alt;
}
