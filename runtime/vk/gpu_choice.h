#pragma once
#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

enum { GPUC_OTHER = 0, GPUC_INTEGRATED = 1, GPUC_DISCRETE = 2, GPUC_VIRTUAL = 3, GPUC_CPU = 4 };

typedef struct {
    const char *name;
    int type;
    int usable;
} GpucCard;

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

static inline int gpuc_index(const GpucCard *c, int n, int want, const char *name, int *dropped) {
    *dropped = 0;
    if (want < 0 || !name || !*name) return want;
    if (want < n && gpuc_has(c[want].name, name)) return want;
    *dropped = 1;
    return -1;
}

static inline int gpuc_legacy_applies(const GpucCard *c, int n, int want, const char *name, int from_env) {
    return !(want < 0 || want >= n || from_env || (name && *name) || c[want].type == GPUC_DISCRETE);
}

static inline int gpuc_legacy_alt(const GpucCard *c, int n, int want, const char *name, int from_env) {
    if (!gpuc_legacy_applies(c, n, want, name, from_env)) return -1;
    for (int i = 0; i < n; i++)
        if (c[i].type == GPUC_DISCRETE && c[i].usable) return i;
    return -1;
}

static inline int gpuc_resolve(const GpucCard *c, int n, int want, const char *name, int from_env, int *dropped, int *alt) {
    want = gpuc_index(c, n, want, name, dropped);
    *alt = gpuc_legacy_alt(c, n, want, name, from_env);
    return *alt >= 0 ? -1 : want;
}

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

static inline void gpuc_dropped_line(const GpucCard *c, int n, int want, const char *name, char *line, size_t cap) {
    if (want < n)
        snprintf(line, cap,
                 "vk: gpu_index = %d lists \"%s\", which does not have gpu_name = %s in its name: the cards are listed in "
                 "another order than when gpu_index was written, so the name decides", want, c[want].name, name);
    else
        snprintf(line, cap, "vk: gpu_index = %d lists no card (%d are listed): gpu_name = %s decides", want, n, name);
}

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

enum { GPUC_BY_AUTO = 0, GPUC_BY_POSITION, GPUC_BY_NAME, GPUC_BY_LEGACY, GPUC_BY_FALLBACK };

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
