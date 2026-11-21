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
// a card that does not have that name in its own, the name decides (gpuc_index). A gpu_index written alone, by an older
// version or by hand, still goes by its position; the log then says so when it puts the game on a card that is not a
// discrete one while a discrete one the game can use is listed (gpuc_choice_line).
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
    int usable;         // the game can run on it: Vulkan 1.3, its extensions, a queue that presents. Read for discrete cards only
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

// The card the settings pick as far as the list tells, for the menu's Graphics card row: the one gpu_index lists when it
// has the name (or no name is set), else the first with the name; -1 for Auto, for a position past the list and for a name
// that no card has (the game then takes the best card, as for Auto).
static inline int gpuc_shown(const GpucCard *c, int n, int want, const char *name) {
    int dropped;
    want = gpuc_index(c, n, want, name, &dropped);
    if (name && *name) {
        if (want >= 0) return want;
        for (int i = 0; i < n; i++)
            if (gpuc_has(c[i].name, name)) return i;
        return -1;
    }
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

// The settings that chose, as the log names them: "gpu_index = 1", "gpu_name = NVIDIA" or both.
static inline void gpuc_by(char *out, size_t cap, int want, const char *name) {
    int has_name = name && *name;
    if (want >= 0 && has_name) snprintf(out, cap, "gpu_index = %d and gpu_name = %s", want, name);
    else if (want >= 0) snprintf(out, cap, "gpu_index = %d", want);
    else snprintf(out, cap, "gpu_name = %s", has_name ? name : "");
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

// The settings chose card picked (want, name: what the pick went by, -1 and NULL for Auto), and it is not a discrete card
// while one the game can use is listed: the first such card is returned, line gets the warning for the log and note (NULL:
// none wanted) the short form for a fatal error's report. Returns -1 and empties both when there is nothing to say: Auto,
// a discrete card picked, or no discrete card to use. A position says little over the cards' order changing from one start
// to the next, so a gpu_index is told so; a name alone is the player's own and is only named.
static inline int gpuc_choice_line(const GpucCard *c, int n, int picked, int want, const char *name, char *line, size_t cap,
                                   char *note, size_t note_cap) {
    line[0] = 0;
    if (note) note[0] = 0;
    int named = name && *name;
    if ((want < 0 && !named) || picked < 0 || picked >= n || c[picked].type == GPUC_DISCRETE) return -1;
    int alt = -1;
    for (int i = 0; i < n && alt < 0; i++)
        if (i != picked && c[i].type == GPUC_DISCRETE && c[i].usable) alt = i;
    if (alt < 0) return -1;
    char by[320];
    gpuc_by(by, sizeof by, want, name);
    if (want >= 0)
        snprintf(line, cap,
                 "vk: warning: gpu %d \"%s\" (%s) was chosen by %s, while gpu %d \"%s\" is a discrete card. The order the "
                 "cards are listed in can change from one start to the next (the system's graphics settings for the game, "
                 "a display on one card, the power source), and gpu_index then names another card. Auto, in the menu's "
                 "Graphics card row, takes the discrete card; gpu_name = %s keeps it by its name",
                 picked, c[picked].name, gpuc_kind(c[picked].type), by, alt, c[alt].name, c[alt].name);
    else
        snprintf(line, cap,
                 "vk: warning: gpu %d \"%s\" (%s) was chosen by %s, while gpu %d \"%s\" is a discrete card; Auto, in the "
                 "menu's Graphics card row, takes the discrete card",
                 picked, c[picked].name, gpuc_kind(c[picked].type), by, alt, c[alt].name);
    if (note)
        snprintf(note, note_cap, "card chosen by %s (%s) while gpu %d is a discrete card", by, gpuc_kind(c[picked].type),
                 alt);
    return alt;
}
