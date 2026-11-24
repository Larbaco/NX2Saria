#pragma once
#include "imgui.h"

extern "C" {
#include "ui_text.h"
}

void opt_begin(const char *id, float scale);
void opt_end(void);
void opt_reserve_bottom(float h);
float opt_bottom(void);
void opt_focus_first(void);
bool opt_focus_take(void);
void opt_focus_key(const char *key);
void opt_disable_next(UiTextId note);
void opt_disable_next_text(const char *note);

void opt_group(UiTextId title);
void opt_text(UiTextId text, bool warn = false);

bool opt_toggle(const char *key, UiTextId label, UiTextId desc, bool inverted = false, const char *subtitle = nullptr);
bool opt_toggle_value(const char *key, UiTextId label, UiTextId desc, const char *on_value, const char *off_value,
                      const char *note = nullptr);
bool opt_choice(const char *key, UiTextId label, const UiTextId *names, int n, UiTextId desc,
                const char *note = nullptr);
bool opt_choice_list(const char *key, UiTextId label, const char *const *values, const char *const *names, int n,
                     UiTextId desc, int cur = -1, const char *note = nullptr);
int opt_choice_index(const char *key, UiTextId label, const char *const *names, int n, int cur, int def, UiTextId desc,
                     const char *note = nullptr, bool again = false);
void opt_apply(const char *key, const char *value);
void opt_context(const char *key, UiTextId label, const char *from, const char *to);
bool opt_button(const char *id, UiTextId label, UiTextId desc, const char *value = "");
int opt_text_row(const char *id, const char *label, const char *value, const char *note, const char *desc,
                 bool movable);
bool opt_toggle_text(const char *id, const char *label, const char *desc, bool on, const char *value = nullptr);
void opt_group_text(const char *title);
void opt_text_str(const char *text, bool warn = false);
bool opt_slider(const char *key, UiTextId label, float lo, float hi, UiTextId desc, float step = 1,
                const char *fmt = "%g", const char *note = nullptr, const char *fallback = nullptr);
bool opt_slider_stops(const char *key, UiTextId label, int lo, int hi, UiTextId desc, const char *fmt,
                      const char *const *stop_values, const char *const *stop_names, int stops,
                      const char *cur = nullptr, const char *note = nullptr);

void opt_bind_header(UiTextId c0, UiTextId c1, UiTextId c2);
int opt_bind_row(const char *key, const char *label, const char *action, UiTextId desc, const char *const cells[3],
                 const bool enabled[3], const char *note = nullptr);

const char *opt_value(const char *key);
const char *opt_get(const char *key);
const char *opt_before(const char *key);
int opt_preset_tier(void);
const char *opt_default(const char *key);
bool opt_is_set(const char *key);
int opt_preset_handback(int tier, char (*names)[64], int max);
int opt_pending_count(void);
int opt_int(const char *key);
double opt_double(const char *key);
bool opt_bool(const char *key);
void opt_change(const char *key, const char *value, const char *label, const char *from, const char *to);
int opt_draft_any(void);
int opt_draft_count(void);
struct OptDraftItem {
    char label[96], from[96], to[96];
    bool restart;
    bool display;
};
bool opt_draft_item(int i, OptDraftItem *out);
void opt_draft_sync(void);
bool opt_draft_apply(bool timed = false);
int opt_display_pending(void);
int opt_display_external(void);
bool opt_display_keep(void);
bool opt_display_revert(void);
void opt_draft_discard(void);
void opt_save_now(void);
