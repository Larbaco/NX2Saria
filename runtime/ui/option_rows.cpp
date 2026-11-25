// Contains code Copyright (c) 2026 Revivedx and the Savage Wind Recomp contributors, Copyright (c) 2026 Peiikai (MIT; see
// THIRD_PARTY_NOTICES.md).
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <atomic>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_internal.h"

#include "ui/menu_skin.h"
#include "ui/option_rows.h"

extern "C" {
#include "cfg.h"
#include "platform/platform.h"
#include "ui_text.h"
#include "menu_sound.h"
#include "session_summary.h"
void rt_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
}

static const ImU32 kGrey = IM_COL32(110, 110, 102, 255);

static float g_scale = 1;
static bool g_focus_req, g_take_first;
static char g_focus_key[64];
static bool g_held;
static float g_bottom;
static bool g_disable_next;
static const char *g_disable_note;

static struct { bool on; ImVec2 a, b; float alpha; } g_cursor;
static float row_margin(void) { return ceilf(skin_cursor_reach(g_scale) * 0.85f); }

struct Desc { char text[UI_DESC_BYTES]; };
static Desc g_hover, g_focus, g_first, g_shown;
static ImGuiID g_page;

static void desc_set(Desc &d, const char *desc, const char *key, const CfgMeta *m, const char *disabled) {
    int n = snprintf(d.text, sizeof d.text, "%s", desc);
    if (n < 0 || n >= (int)sizeof d.text) return;
    if (disabled)
        snprintf(d.text + n, sizeof d.text - n, "\n\n%s", disabled);
    else if (cfg_from_env(key))
        snprintf(d.text + n, sizeof d.text - n, "\n\n%s %s", menu_text(UI_MENU_SET_BY_ENV), key);
    else if (m && !m->live)
        snprintf(d.text + n, sizeof d.text - n, "\n\n%s", menu_text(UI_MENU_AFTER_RESTART));
}

void opt_focus_first(void) { g_focus_req = true; }

bool opt_focus_take(void) {
    const bool req = g_focus_req;
    g_focus_req = false;
    return req;
}

void opt_focus_key(const char *key) { snprintf(g_focus_key, sizeof g_focus_key, "%s", key); }

void opt_reserve_bottom(float h) { g_bottom = h; }
float opt_bottom(void) { return g_bottom; }

void opt_disable_next(UiTextId note) { opt_disable_next_text(menu_text(note)); }

void opt_disable_next_text(const char *note) {
    g_disable_next = true;
    g_disable_note = note;
}

void opt_begin(const char *id, float scale) {
    g_scale = scale;
    g_take_first = g_focus_req;
    g_focus_req = false;
    if (g_take_first) ImGui::NavMoveRequestCancel();
    g_hover.text[0] = g_focus.text[0] = g_first.text[0] = 0;
    if (ImGuiID page = ImGui::GetID(id); page != g_page) g_page = page, g_shown.text[0] = 0;
    ImVec2 avail = ImGui::GetContentRegionAvail();
    avail.y -= g_bottom;
    ImGui::PushID(id);
    ImGui::BeginChild("##rows", ImVec2(avail.x * 0.58f, avail.y), ImGuiChildFlags_NavFlattened);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 9 * scale));
    ImGui::Dummy(ImVec2(0, row_margin()));
    g_cursor.on = false;
    ImGui::GetWindowDrawList()->ChannelsSplit(3);
    ImGui::GetWindowDrawList()->ChannelsSetCurrent(1);
}

void opt_end(void) {
    ImGui::Dummy(ImVec2(0, row_margin()));
    ImDrawList *dl = ImGui::GetWindowDrawList();
    if (g_cursor.on) {
        dl->ChannelsSetCurrent(0);
        skin_cursor_glow(dl, g_cursor.a, g_cursor.b, g_scale, g_cursor.alpha);
        dl->ChannelsSetCurrent(2);
        skin_cursor_frame(dl, g_cursor.a, g_cursor.b, g_scale, g_cursor.alpha);
    }
    dl->ChannelsMerge();
    ImGui::PopStyleVar();
    ImGui::EndChild();
    if (g_hover.text[0]) g_shown = g_hover;
    else if (g_focus.text[0]) g_shown = g_focus;
    else if (!g_shown.text[0]) g_shown = g_first;
    ImGui::SameLine(0, 0);
    ImVec2 p = ImGui::GetCursorScreenPos(), avail = ImGui::GetContentRegionAvail();
    avail.y -= g_bottom;
    float gap = 24 * g_scale;
    skin_panel(ImGui::GetWindowDrawList(), ImVec2(p.x + gap * 0.5f, p.y), ImVec2(p.x + avail.x, p.y + avail.y), g_scale);
    ImGui::SetCursorScreenPos(ImVec2(p.x + gap, p.y));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(22 * g_scale, 20 * g_scale));
    ImGui::BeginChild("##desc", ImVec2(0, avail.y), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoNav);
    ImGui::PopStyleVar();
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(g_shown.text);
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
    ImGui::PopID();
}

struct RowView {
    const char *value;
    bool is_default;
    bool arrows;
    const char *note;
    float col;
    bool end_l, end_r;
    const char *subtitle = nullptr;
};

static float value_width(const char *const *names, int n) {
    ImFont *font = ImGui::GetFont();
    float fs = ImGui::GetFontSize(), w = 0;
    for (int i = 0; i < n; i++)
        if (names[i]) w = fmaxf(w, font->CalcTextSizeA(fs, FLT_MAX, 0, names[i]).x);
    return w;
}

static void arrow_box(ImDrawList *dl, ImVec2 a, ImVec2 b, bool left, bool hot, bool down, bool dim, float s) {
    const int al = dim ? 70 : hot ? 255 : 215;
    const ImU32 white = IM_COL32(255, 255, 255, al);
    if (hot && !dim) dl->AddRectFilled(a, b, IM_COL32(255, 255, 255, down ? 96 : 44), 4 * s);
    dl->AddRect(a, b, white, 4 * s, 0, fmaxf(1.0f, (hot && !dim ? 2.2f : 1.6f) * s));
    float cx = (a.x + b.x) * 0.5f, cy = (a.y + b.y) * 0.5f, aw = (b.y - a.y) * 0.38f, ah = aw * 0.8f;
    float tip = left ? -aw * 0.55f : aw * 0.55f, back = left ? aw * 0.45f : -aw * 0.45f;
    dl->AddTriangleFilled(ImVec2(cx + tip, cy), ImVec2(cx + back, cy - ah), ImVec2(cx + back, cy + ah), white);
}

static struct ClickTest { int parsed, n, at, frame; struct { char key[56]; char part; } c[24]; } g_click;

static void click_test(const char *key, ImVec2 la, ImVec2 lb, ImVec2 ra, ImVec2 rb, ImVec2 label) {
    ClickTest &t = g_click;
    if (!t.parsed) {
        t.parsed = 1;
        const char *s = cfg_get("NX2SARIA_MENU_TEST_CLICK");
        while (s && *s && t.n < 24) {
            size_t n = strcspn(s, ",");
            const char *colon = (const char *)memchr(s, ':', n);
            if (colon) {
                snprintf(t.c[t.n].key, sizeof t.c[0].key, "NX2SARIA_%.*s", (int)(colon - s), s);
                t.c[t.n++].part = colon[1];
            }
            s += n;
            if (*s == ',') s++;
        }
    }
    if (t.at >= t.n || strcmp(key, t.c[t.at].key)) return;
    const char part = t.c[t.at].part;
    const ImVec2 to = part == 'l'   ? ImVec2((la.x + lb.x) * 0.5f, (la.y + lb.y) * 0.5f)
                      : part == 'r' ? ImVec2((ra.x + rb.x) * 0.5f, (ra.y + rb.y) * 0.5f)
                                    : label;
    ImGuiIO &io = ImGui::GetIO();
    const int f = t.frame++;
    if (f == 0) io.AddMousePosEvent(to.x, to.y);
    else if (f == 4) io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    else if (f == 7) io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    else if (f == 12) {
        rt_log("menu: test hook: click on %s of %s: now %s",
               part == 'l' ? "the left arrow" : part == 'r' ? "the right arrow" : "the body", key, opt_value(key));
        t.at++, t.frame = 0;
    }
}

static int row_text(const char *key, const char *label, const char *desc, const RowView &v, bool movable) {
    const CfgMeta *m = cfg_meta(key);
    const char *disabled = g_disable_next ? g_disable_note : NULL;
    g_disable_next = false;
    bool off = disabled || cfg_from_env(key) != 0;
    ImGui::PushID(key);
    const float margin = row_margin();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + margin);
    ImVec2 p = ImGui::GetCursorScreenPos();
    float fs = ImGui::GetFontSize(), w = fmaxf(ImGui::GetContentRegionAvail().x - margin, 1.0f), h = floorf(fs * 2.2f);
    ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
    bool clicked = ImGui::InvisibleButton("##row", ImVec2(w, h), ImGuiButtonFlags_EnableNav);
    ImGui::PopStyleColor();
    bool to_key = g_focus_key[0] && !strcmp(key, g_focus_key);
    if (g_take_first || to_key) {
        ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
        ImGui::SetNavCursorVisible(true);
        if (g_take_first) ImGui::SetScrollY(0);
        else ImGui::ScrollToItem(ImGuiScrollFlags_KeepVisibleEdgeY);
        g_take_first = false;
        if (to_key) g_focus_key[0] = 0;
    }
    bool hov = ImGui::IsItemHovered(), foc = ImGui::IsItemFocused() && ImGui::GetCurrentContext()->NavCursorVisible;
    int step = clicked && !off ? 2 : 0;
    const bool by_mouse = clicked && hov && ImGui::IsMouseReleased(ImGuiMouseButton_Left);
    g_held = false;
    if (foc && !off) {
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft) || ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) step = -1;
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight) || ImGui::IsKeyPressed(ImGuiKey_RightArrow)) step = 1;
        static const ImGuiKey steps[] = {ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadDpadRight, ImGuiKey_LeftArrow,
                                         ImGuiKey_RightArrow};
        for (ImGuiKey k : steps) {
            if (ImGui::GetKeyData(k)->DownDuration > 1.0f) g_held = true;
            ImGui::SetKeyOwner(k, ImGui::GetItemID());
        }
        if (movable) {
            bool ctrl = ImGui::GetIO().KeyCtrl;
            if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceUp, false) || (ctrl && ImGui::IsKeyPressed(ImGuiKey_UpArrow)))
                step = 3;
            if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceLeft, false) || (ctrl && ImGui::IsKeyPressed(ImGuiKey_DownArrow)))
                step = 4;
        }
    }
    if (foc) desc_set(g_focus, desc, key, m, disabled);
    if (hov) desc_set(g_hover, desc, key, m, disabled);
    if (!g_first.text[0]) desc_set(g_first, desc, key, m, disabled);

    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImFont *font = ImGui::GetFont();
    float pad = 20 * g_scale, small = fs * 0.72f;
    skin_button(dl, p, ImVec2(p.x + w, p.y + h), g_scale, foc ? 1.0f : hov ? 0.6f : 0.0f, off);
    if (foc || (hov && !g_cursor.on)) {
        g_cursor.on = true, g_cursor.a = p, g_cursor.b = ImVec2(p.x + w, p.y + h);
        g_cursor.alpha = (foc ? 1.0f : 0.55f) * (off ? 0.5f : 1.0f);
    }
    float ty = p.y + fs * 0.25f, ny = p.y + fs * 1.3f;
    float right = p.x + w - pad;
    float value_left = right;
    const float cap = fmaxf((w - pad * 2) * 0.6f, 0.0f);
    float vfs = fs, tv = v.value[0] ? font->CalcTextSizeA(fs, FLT_MAX, 0, v.value).x : 0;
    if (tv > cap) {
        vfs = fmaxf(fs * cap / tv, fs * 0.75f);
        tv = fminf(font->CalcTextSizeA(vfs, FLT_MAX, 0, v.value).x, cap);
    }
    float value_mid = -1;
    if (v.arrows) {
        const float bw = floorf(fs * 1.3f), bh = floorf(fs * 1.02f), gap = floorf(fs * 0.4f), cy = floorf(ty + fs * 0.52f);
        float col = fminf(fmaxf(v.col, tv), fmaxf(cap - 2 * (bw + gap), fs));
        if (tv > col) {
            vfs = fmaxf(fs * col / font->CalcTextSizeA(fs, FLT_MAX, 0, v.value).x, fs * 0.75f);
            tv = fminf(font->CalcTextSizeA(vfs, FLT_MAX, 0, v.value).x, col);
        }
        const ImVec2 ra(right - bw, cy - bh * 0.5f), rb(right, cy + bh * 0.5f);
        const ImVec2 la(ra.x - gap - col - gap - bw, ra.y), lb(ra.x - gap - col - gap, rb.y);
        if (!g_click.parsed || g_click.at < g_click.n) click_test(key, la, lb, ra, rb, ImVec2(p.x + w * 0.25f, p.y + h * 0.5f));
        const ImVec2 mp = ImGui::GetIO().MousePos;
        const float slop = 2 * g_scale;
        auto on = [&](ImVec2 a, ImVec2 b) {
            return hov && mp.x >= a.x - slop && mp.x < b.x + slop && mp.y >= a.y - slop && mp.y < b.y + slop;
        };
        const bool hl = on(la, lb), hr = on(ra, rb), down = ImGui::IsMouseDown(ImGuiMouseButton_Left);
        if (by_mouse) step = 0;
        if (!off && ((hl && !v.end_l) || (hr && !v.end_r)) && ImGui::IsMouseClicked(ImGuiMouseButton_Left, true)) {
            step = hl ? -1 : 1;
            if (ImGui::GetIO().MouseDownDuration[ImGuiMouseButton_Left] > 1.0f) g_held = true;
        }
        arrow_box(dl, la, lb, true, hl, hl && down, off || v.end_l, g_scale);
        arrow_box(dl, ra, rb, false, hr, hr && down, off || v.end_r, g_scale);
        value_mid = (lb.x + ra.x) * 0.5f;
        right = ra.x - gap;
        value_left = la.x;
    }
    if (movable) {
        float aw = fs * 0.42f, gap = fs * 0.55f;
        float cx = right - fmaxf(tv, fs * 2.8f) - gap * 2 - aw;
        float uy = ty + fs * 0.12f, dy = ty + fs * 0.92f;
        ImU32 ac = off ? kGrey : hov || foc ? SKIN_TEXT : SKIN_TEXT_DIM;
        dl->AddTriangleFilled(ImVec2(cx, uy + aw * 0.7f), ImVec2(cx + aw, uy + aw * 0.7f), ImVec2(cx + aw * 0.5f, uy), ac);
        dl->AddTriangleFilled(ImVec2(cx, dy - aw * 0.7f), ImVec2(cx + aw, dy - aw * 0.7f), ImVec2(cx + aw * 0.5f, dy), ac);
        if (by_mouse && !off) {
            ImVec2 mp = ImGui::GetIO().MousePos;
            if (mp.x >= cx - gap * 0.5f && mp.x <= cx + aw + gap * 0.5f) step = mp.y < ty + fs * 0.52f ? 3 : 4;
        }
        value_left = cx - gap;
    }
    if (v.value[0]) {
        float vx = value_mid >= 0 ? floorf(value_mid - tv * 0.5f) : right - tv;
        ImVec4 vclip(vx, p.y, vx + tv, p.y + h);
        dl->AddText(font, vfs, ImVec2(vx, ty + (fs - vfs) * 0.5f),
                    off ? kGrey : v.is_default ? IM_COL32(232, 222, 198, 255) : IM_COL32(255, 255, 255, 255), v.value, NULL,
                    0, &vclip);
        if (!v.arrows && !movable) value_left = vx;
    }
    float note_right = p.x + w - pad;
    if (v.note || v.is_default) {
        const char *t = v.note ? v.note : menu_text(UI_MENU_DEFAULT_MARK);
        float tw = fminf(font->CalcTextSizeA(small, FLT_MAX, 0, t).x, cap);
        float nx = value_mid >= 0 ? fminf(floorf(value_mid - tw * 0.5f), p.x + w - pad - tw) : right - tw;
        ImVec4 nclip(nx, p.y, nx + tw, p.y + h);
        dl->AddText(font, small, ImVec2(nx, ny), v.note ? IM_COL32(250, 206, 130, 255) : SKIN_TEXT_DIM, t, NULL, 0, &nclip);
        if (tw > 0) note_right = nx - fs * 0.6f;
    }
    ImVec4 clip(p.x, p.y, value_left - fs * 0.6f, p.y + h);
    dl->AddText(font, fs, ImVec2(p.x + pad, ty), off ? kGrey : SKIN_TEXT, label, NULL, 0, &clip);
    const char *note = disabled                                               ? disabled
                     : off                                                    ? menu_text(UI_MENU_SET_BY_ENV)
                     : m && !m->live && cfg_pending_for(key, opt_value(key)) ? menu_text(UI_MENU_AFTER_RESTART)
                                                                              : NULL;
    char both[UI_DESC_BYTES];
    if (v.subtitle) {
        if (note) {
            snprintf(both, sizeof both, "%s — %s", v.subtitle, note);
            note = both;
        } else {
            note = v.subtitle;
        }
    }
    ImVec4 note_clip(p.x, p.y, note_right, p.y + h);
    if (note)
        dl->AddText(font, small, ImVec2(p.x + pad, ny), off ? IM_COL32(200, 150, 90, 255) : SKIN_TEXT_DIM, note, NULL, 0,
                    &note_clip);
    ImGui::PopID();
    return off ? 0 : step;
}

static int row(const char *key, UiTextId label, UiTextId desc, const RowView &v) {
    if (!cfg_supported(key)) {
        g_disable_next = false;
        return 0;
    }
    return row_text(key, menu_text(label), menu_text(desc), v, false);
}

namespace {

struct Group {
    std::string id;
    bool has_id;
    UiTextId label;
    std::string label_str, from, to;
};

constexpr size_t KEY_PREFIX = sizeof "NX2SARIA_" - 1;

struct Change {
    std::string key;
    std::string gid;
    std::string new_val;
    std::string orig_eff;
    const char *orig_val;
    int orig_state;
    bool staged;
    bool orig_set;
};

std::vector<Group> g_groups;
std::vector<Change> g_changes;
std::atomic<int> g_draft_n;
unsigned g_sync_ver;
struct { bool valid; std::string gid, from, to; UiTextId label; } g_ctx;

std::string norm_key(const char *key) {
    std::string k = key;
    for (char &c : k) c = (char)toupper((unsigned char)c);
    if (!k.compare(0, KEY_PREFIX, "NX2SARIA_")) return k;
    if (!k.compare(0, sizeof "NX2NAVI_" - 1, "NX2NAVI_")) k.erase(0, sizeof "NX2NAVI_" - 1);
    return "NX2SARIA_" + k;
}

bool is_staged(const std::string &key) {
    static const char *const forced[] = {"FPS_LIMIT", "VK_PRESENT", "HDR", "PAD_LAYOUT", "WALK_KEY", "MOTION",
                                        "FULLSCREEN", "WINDOW_SIZE", "FOV", "PRESET"};
    const CfgMeta *m = cfg_meta(key.c_str());
    if (!m) return !key.compare(KEY_PREFIX, sizeof "CHEAT_" - 1, "CHEAT_");
    if (!m->live) return true;
    for (const char *f : forced)
        if (!strcmp(f, m->key)) return true;
    return false;
}

std::string eff_now(const std::string &key) {
    if (key == "NX2SARIA_PRESET" && cfg_preset_hw_changed()) return std::string(cfg_value(key.c_str())) + " (other hardware)";
    if (cfg_meta(key.c_str())) return cfg_value(key.c_str());
    const char *v = cfg_get(key.c_str());
    return v ? v : "";
}

Change *find_change(const std::string &key) {
    for (Change &c : g_changes)
        if (c.key == key) return &c;
    return nullptr;
}

int preset_tier_of(const std::string &v) {
    std::string low = v;
    for (char &ch : low) ch = (char)tolower((unsigned char)ch);
    for (int t = 0; t < 4; t++)
        if (low == cfg_preset_name(t)) return t;
    return cfg_preset_recommended(nullptr);
}

int staged_tier(void) {
    Change *c = find_change("NX2SARIA_PRESET");
    return c ? preset_tier_of(c->new_val) : -1;
}

bool is_preset_key(const std::string &key) {
    int n = 0;
    const char *const *keys = cfg_preset_keys(&n);
    for (int i = 0; i < n; i++)
        if (!strcmp(keys[i], key.c_str() + KEY_PREFIX)) return true;
    return false;
}

const char *default_of(const std::string &key) {
    const int t = staged_tier();
    return t >= 0 ? cfg_default_tier(key.c_str(), t) : cfg_default(key.c_str());
}

std::string eff_of(const std::string &key, const std::string &value) {
    if (!value.empty()) return value;
    const char *d = default_of(key);
    return d ? d : "";
}

std::string eff_written(const std::string &key, const std::string &value) {
    if (!value.empty()) return value;
    const char *d = cfg_default(key.c_str());
    return d ? d : "";
}

Group *find_group(const std::string &id) {
    for (Group &g : g_groups)
        if (g.id == id) return &g;
    return nullptr;
}

void write_key(const std::string &key, const std::string &value) {
    if (!value.empty()) {
        cfg_set(key.c_str(), value.c_str());
    } else {
        const char *k = key.c_str();
        cfg_reset(&k, 1);
    }
}

void erase_change(size_t i) {
    std::string gid = g_changes[i].gid;
    g_changes.erase(g_changes.begin() + (std::ptrdiff_t)i);
    bool used = false;
    for (const Change &c : g_changes) used |= c.gid == gid;
    if (!used)
        for (size_t g = 0; g < g_groups.size(); g++)
            if (g_groups[g].id == gid) {
                g_groups.erase(g_groups.begin() + (std::ptrdiff_t)g);
                break;
            }
    g_draft_n.store((int)g_changes.size());
}

bool no_effect(const Change &c) { return eff_of(c.key, c.new_val) == c.orig_eff; }

std::string shown(const std::string &v) { return v.empty() ? "(default)" : "'" + v + "'"; }

void record(const std::string &key, const char *value, const std::string &gid, bool has_id, UiTextId label,
            const char *label_str, const char *from, const char *to) {
    Change *c = find_change(key);
    if (!c) {
        Change n;
        n.key = key;
        n.gid = gid;
        n.staged = is_staged(key);
        n.orig_eff = eff_now(key);
        n.orig_state = cfg_state(key.c_str(), &n.orig_val);
        n.orig_set = cfg_is_set(key.c_str()) != 0;
        if (!n.staged) cfg_hold(key.c_str(), 1);
        g_changes.push_back(n);
        c = &g_changes.back();
        rt_log("menu: draft + %s: %s -> %s (%s)", key.c_str() + KEY_PREFIX, shown(n.orig_eff).c_str(), shown(eff_of(key, value)).c_str(),
               n.staged ? "staged: waits for Apply" : "preview: in effect now");
    }
    if (Group *g = find_group(gid)) {
        g->to = to;
    } else {
        g_groups.push_back({gid, has_id, label, label_str ? label_str : "", from, to});
    }
    c->new_val = value;
    if (!c->staged) write_key(key, c->new_val);
    const bool handed = c->new_val.empty() && c->orig_set && is_preset_key(key) && staged_tier() >= 0;
    if (!handed && eff_of(key, c->new_val) == c->orig_eff) {
        rt_log("menu: draft - %s: back to %s", key.c_str() + KEY_PREFIX, shown(c->orig_eff).c_str());
        if (!c->staged) cfg_revert(key.c_str(), c->orig_val, c->orig_state);
        erase_change((size_t)(c - g_changes.data()));
    }
    g_draft_n.store((int)g_changes.size());
}

}

const char *opt_value(const char *key) {
    const std::string k = norm_key(key);
    Change *c = find_change(k);
    if (c && c->staged) {
        if (!c->new_val.empty()) return c->new_val.c_str();
        const char *d = default_of(k);
        return d ? d : "";
    }
    if (staged_tier() >= 0 && !(c ? !c->new_val.empty() : cfg_is_set(k.c_str())))
        if (const char *d = default_of(k)) return d;
    return cfg_value(key);
}

const char *opt_get(const char *key) {
    const std::string k = norm_key(key);
    Change *c = find_change(k);
    if (c && c->staged) return c->new_val.empty() ? nullptr : c->new_val.c_str();
    if (const int t = staged_tier(); t >= 0 && !(c ? !c->new_val.empty() : cfg_is_set(k.c_str()))) {
        const char *now = cfg_default(k.c_str()), *want = cfg_default_tier(k.c_str(), t);
        if (want && (!now || strcmp(now, want))) return want;
    }
    return cfg_get(key);
}

const char *opt_before(const char *key) {
    Change *c = find_change(norm_key(key));
    return c ? c->orig_eff.c_str() : cfg_value(key);
}

const char *opt_default(const char *key) { return default_of(norm_key(key)); }

int opt_preset_tier(void) { return staged_tier(); }

int opt_preset_handback(int tier, char (*names)[64], int max) {
    int nk = 0, n = 0;
    const char *const *keys = cfg_preset_keys(&nk);
    for (int i = 0; i < nk && n < max; i++) {
        snprintf(names[n], sizeof names[n], "NX2SARIA_%s", keys[i]);
        if (opt_is_set(names[n]) && !cfg_from_env(names[n]) && cfg_preset_differs(tier, names[n], opt_get(names[n]))) n++;
    }
    return n;
}

bool opt_is_set(const char *key) {
    const std::string k = norm_key(key);
    if (Change *c = find_change(k)) return !c->new_val.empty();
    return cfg_is_set(k.c_str()) != 0;
}

int opt_int(const char *key) {
    const char *v = opt_value(key);
    char *end;
    long l = strtol(v, &end, 10);
    return end == v || *end ? cfg_int(key) : (int)l;
}

double opt_double(const char *key) {
    const char *v = opt_value(key);
    char *end;
    double d = strtod(v, &end);
    return end == v || *end || d != d ? cfg_double(key) : d;
}

bool opt_bool(const char *key) { return strcmp(opt_value(key), "0") != 0; }

void opt_change(const char *key, const char *value, const char *label, const char *from, const char *to) {
    std::string k = norm_key(key);
    g_ctx.valid = false;
    record(k, value, k, false, UI_MENU_CONTINUE, label, from, to);
}

int opt_draft_any(void) { return g_draft_n.load(std::memory_order_relaxed); }

int opt_draft_count(void) {
    int n = 0;
    for (const Group &g : g_groups) n += g.from != g.to;
    return n;
}

extern "C" int vk_dlaa_needs_restart(void);
extern "C" int vk_hdr_restart_for(const char *value);
extern "C" int vk_dlssg_wanted(void);
extern "C" int vk_ngxfg_wanted(void);
extern "C" int menu_lowlat_restart_for(const char *value);
static bool live_needs_restart(const Change &c) {
    if (c.key == "NX2SARIA_LOW_LATENCY") return menu_lowlat_restart_for(eff_of(c.key, c.new_val).c_str()) != 0;
    if (c.key == "NX2SARIA_FRAMEGEN") {
        const bool dlss = eff_of(c.key, c.new_val) == "dlss";
#ifdef _WIN32
        return dlss != (vk_dlssg_wanted() != 0);
#else
        return dlss && !vk_ngxfg_wanted();
#endif
    }
    if (c.key == "NX2SARIA_HDR") return vk_hdr_restart_for(eff_of(c.key, c.new_val).c_str()) != 0;
    if (c.key == "NX2SARIA_DLAA" || c.key == "NX2SARIA_DLSS" || c.key == "NX2SARIA_TAA" || c.key == "NX2SARIA_DLSS_PRESET") return vk_dlaa_needs_restart() != 0;
    return false;
}

static bool is_timed(const Change &c) {
    static const char *const timed[] = {"NX2SARIA_FULLSCREEN", "NX2SARIA_WINDOW_SIZE", "NX2SARIA_VK_PRESENT",
                                        "NX2SARIA_HDR"};
    if (!c.staged || live_needs_restart(c)) return false;
    for (const char *t : timed)
        if (c.key == t) return true;
    return false;
}

static bool group_timed(const std::string &gid) {
    for (const Change &c : g_changes)
        if (c.gid == gid && is_timed(c)) return true;
    return false;
}

int opt_pending_count(void) {
    int n = 0;
    char name[80];
    for (int i = 0; const CfgMeta *m = cfg_meta_at(i); i++) {
        snprintf(name, sizeof name, "NX2SARIA_%s", m->key);
        n += cfg_pending_for(name, opt_value(name));
    }
    return n;
}

bool opt_draft_item(int i, OptDraftItem *out) {
    for (const Group &g : g_groups) {
        if (g.from == g.to || i--) continue;
        snprintf(out->label, sizeof out->label, "%s", g.has_id ? menu_text(g.label) : g.label_str.c_str());
        snprintf(out->from, sizeof out->from, "%s", g.from.c_str());
        snprintf(out->to, sizeof out->to, "%s", g.to.c_str());
        out->restart = false;
        out->display = group_timed(g.id);
        for (const Change &c : g_changes) {
            if (c.gid != g.id || no_effect(c)) continue;
            const CfgMeta *m = cfg_meta(c.key.c_str());
            if (c.staged) out->restart |= m && !m->live && cfg_pending_for(c.key.c_str(), eff_of(c.key, c.new_val).c_str());
            out->restart |= live_needs_restart(c);
            if (c.key == "NX2SARIA_PRESET") {
                int nk = 0;
                const char *const *keys = cfg_preset_keys(&nk);
                for (int k = 0; k < nk && !out->restart; k++) {
                    const std::string name = std::string("NX2SARIA_") + keys[k];
                    const CfgMeta *m = cfg_meta(name.c_str());
                    out->restart |= m && !m->live && cfg_pending_for(name.c_str(), opt_value(name.c_str()));
                }
            }
        }
        return true;
    }
    return false;
}

void opt_draft_sync(void) {
    if (g_changes.empty() || cfg_version() == g_sync_ver) return;
    g_sync_ver = cfg_version();
    for (size_t i = g_changes.size(); i-- > 0;) {
        const Change &c = g_changes[i];
        if (eff_now(c.key) == (c.staged ? c.orig_eff : eff_written(c.key, c.new_val))) continue;
        rt_log("menu: draft - %s: changed outside the menu (F10, F11, a cheat) to %s, which stays", c.key.c_str() + KEY_PREFIX,
               shown(eff_now(c.key)).c_str());
        if (!c.staged) cfg_hold(c.key.c_str(), 0);
        erase_change(i);
    }
}

void opt_draft_discard(void) {
    if (!g_changes.empty()) rt_log("menu: draft discarded: %d change(s), nothing saved", (int)g_changes.size());
    for (const Change &c : g_changes) {
        rt_log("menu: draft   %s back to %s%s", c.key.c_str() + KEY_PREFIX, shown(c.orig_eff).c_str(),
               c.staged ? " (it never left the draft)" : " (the preview is undone)");
        if (!c.staged) cfg_revert(c.key.c_str(), c.orig_val, c.orig_state);
    }
    g_changes.clear();
    g_groups.clear();
    g_ctx.valid = false;
    g_draft_n.store(0);
}

static bool g_save_logged;

void opt_save_now(void) {
    if (!cfg_save() && !g_save_logged) {
        g_save_logged = true;
        rt_log("menu: could not write the settings file; the changed settings hold for this session");
    }
}

namespace {
struct Pending {
    std::string key, applied;
    const char *orig_val;
    int orig_state;
};
std::vector<Pending> g_display;
}

bool opt_draft_apply(bool timed) {
    g_display.clear();
    int same = 0;
    for (const Change &c : g_changes) same += no_effect(c);
    if (same)
        rt_log("menu: draft applied: %d change(s)%s; %d key(s) handed back to the preset at the value they had",
               (int)g_changes.size() - same, timed ? "" : " (for a restart)", same);
    else
        rt_log("menu: draft applied: %d change(s)%s", (int)g_changes.size(), timed ? "" : " (for a restart)");
    const int tier = staged_tier();
    int n_keys = 0;
    const char *const *keys = cfg_preset_keys(&n_keys);
    std::vector<std::string> before((size_t)n_keys);
    std::vector<const char *> before_p((size_t)n_keys);
    for (int k = 0; tier >= 0 && k < n_keys; k++) {
        const std::string name = std::string("NX2SARIA_") + keys[k];
        const Change *c = find_change(name);
        before[(size_t)k] = c ? c->orig_eff : eff_now(name);
        before_p[(size_t)k] = before[(size_t)k].c_str();
    }
    for (const Change &c : g_changes) {
        const bool held = c.staged && timed && group_timed(c.gid);
        if (no_effect(c)) {
            const std::string v = eff_of(c.key, c.new_val);
            rt_log("menu: draft   %s: stays %s, no change%s%s", c.key.c_str() + KEY_PREFIX, shown(c.orig_eff).c_str(),
                   c.orig_set && c.new_val.empty() ? " (its line leaves the settings file: the preset sets it)" : "",
                   cfg_pending_for(c.key.c_str(), v.c_str()) ? "; an earlier change still waits for a restart" : "");
            if (c.staged) write_key(c.key, c.new_val);
            else cfg_hold(c.key.c_str(), 0);
            continue;
        }
        rt_log("menu: draft   %s: %s -> %s%s%s", c.key.c_str() + KEY_PREFIX, shown(c.orig_eff).c_str(),
               shown(eff_of(c.key, c.new_val)).c_str(),
               !held ? ", saved" : is_timed(c) ? ", in effect now, saved only after Keep"
                                           : ", saved only after Keep, with the display settings of its row",
               live_needs_restart(c) || (c.staged && cfg_meta(c.key.c_str()) && !cfg_meta(c.key.c_str())->live)
                   ? ", takes effect after a restart" : "");
        if (held) {
            cfg_hold(c.key.c_str(), 1);
            write_key(c.key, c.new_val);
            g_display.push_back({c.key, eff_of(c.key, c.new_val), c.orig_val, c.orig_state});
        } else if (c.staged) {
            write_key(c.key, c.new_val);
        } else {
            cfg_hold(c.key.c_str(), 0);
        }
    }
    g_changes.clear();
    g_groups.clear();
    g_ctx.valid = false;
    g_draft_n.store(0);
    if (tier >= 0) {
        rt_log("menu: draft: quality preset %s in use", cfg_preset_name(tier));
        cfg_preset_use_from(tier, before_p.data());
        cfg_preset_chosen();
    }
    opt_save_now();
    rt_log("menu: draft: settings file %s%s", cfg_last_save_ok() ? "saved" : "NOT saved (write failed)",
           g_display.empty() ? "" : "; display settings wait for Keep or Revert");
    session_summary_header("settings applied");
    return cfg_last_save_ok() != 0;
}

int opt_display_pending(void) { return (int)g_display.size(); }

int opt_display_external(void) {
    for (const Pending &p : g_display)
        if (eff_now(p.key) != p.applied) return 1;
    return 0;
}

bool opt_display_keep(void) {
    for (const Pending &p : g_display) {
        rt_log("menu: display kept: %s = %s", p.key.c_str() + KEY_PREFIX, shown(eff_now(p.key)).c_str());
        cfg_hold(p.key.c_str(), 0);
    }
    g_display.clear();
    opt_save_now();
    rt_log("menu: display: settings file %s", cfg_last_save_ok() ? "saved" : "NOT saved (write failed)");
    return cfg_last_save_ok() != 0;
}

bool opt_display_revert(void) {
    if (opt_display_external()) {
        rt_log("menu: display: changed outside the menu (F11) during the countdown: kept instead of reverted");
        opt_display_keep();
        return false;
    }
    for (const Pending &p : g_display) {
        cfg_revert(p.key.c_str(), p.orig_val, p.orig_state);
        rt_log("menu: display reverted: %s back to %s, not saved", p.key.c_str() + KEY_PREFIX, shown(eff_now(p.key)).c_str());
    }
    g_display.clear();
    return true;
}

static void apply(const char *key, const char *value, UiTextId label, const char *from, const char *to) {
    std::string k = norm_key(key);
    g_ctx.valid = false;
    record(k, value, k, true, label, nullptr, from, to);
    menu_sound_play(MENU_SOUND_CURSOR);
}

static float on_off_width(void) {
    const char *const names[] = {menu_text(UI_MENU_ON), menu_text(UI_MENU_OFF)};
    return value_width(names, 2);
}

bool opt_toggle(const char *key, UiTextId label, UiTextId desc, bool inverted, const char *subtitle) {
    const char *d = opt_default(key);
    bool set = opt_bool(key), def = d && *d && strcmp(d, "0") != 0;
    bool on = set != inverted;
    RowView v = {menu_text(on ? UI_MENU_ON : UI_MENU_OFF), set == def, true, NULL, on_off_width()};
    v.subtitle = subtitle;
    if (!row(key, label, desc, v)) return false;
    apply(key, set ? "0" : "1", label, v.value, menu_text(on ? UI_MENU_OFF : UI_MENU_ON));
    return true;
}

bool opt_toggle_value(const char *key, UiTextId label, UiTextId desc, const char *on_value, const char *off_value, const char *note) {
    const char *d = opt_default(key);
    bool on = strcmp(opt_value(key), off_value) != 0, def = d && strcmp(d, off_value) != 0;
    RowView v = {menu_text(on ? UI_MENU_ON : UI_MENU_OFF), on == def, true, note, on_off_width()};
    if (!row(key, label, desc, v)) return false;
    apply(key, on ? off_value : def && *d ? d : on_value, label, v.value, menu_text(on ? UI_MENU_OFF : UI_MENU_ON));
    return true;
}

bool opt_button(const char *id, UiTextId label, UiTextId desc, const char *value) {
    RowView v = {value ? value : "", false, false, NULL};
    return row(id, label, desc, v) == 2;
}

bool opt_toggle_text(const char *id, const char *label, const char *desc, bool on, const char *value) {
    RowView v = {value ? value : menu_text(on ? UI_MENU_ON : UI_MENU_OFF), !on, true, on ? NULL : "",
                 value ? 0 : on_off_width()};
    return row_text(id, label, desc, v, false) != 0;
}

static void group_divider(void) {
    float fh = skin_filigree_height(g_scale);
    ImVec2 p = ImGui::GetCursorScreenPos();
    float m = row_margin();
    skin_filigree(ImGui::GetWindowDrawList(), p.x + m, p.x + ImGui::GetContentRegionAvail().x - m, p.y + fh * 0.5f, g_scale);
    ImGui::Dummy(ImVec2(0, fh));
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + m);
}

static void column_text(const char *text, const ImVec4 *col, bool indented) {
    const float m = row_margin();
    if (!indented) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + m);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + fmaxf(ImGui::GetContentRegionAvail().x - m, 1.0f));
    if (col) ImGui::TextColored(*col, "%s", text);
    else ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
}

static const ImVec4 kWarn(0.95f, 0.66f, 0.40f, 1);

void opt_group_text(const char *title) {
    group_divider();
    const ImVec4 gold = ImGui::ColorConvertU32ToFloat4(SKIN_GOLD);
    column_text(title, &gold, true);
}

void opt_text_str(const char *text, bool warn) { column_text(text, warn ? &kWarn : nullptr, false); }

void opt_group(UiTextId title) {
    group_divider();
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(SKIN_GOLD), "%s", menu_text(title));
}

void opt_text(UiTextId text, bool warn) { column_text(menu_text(text), warn ? &kWarn : nullptr, false); }

bool opt_choice_list(const char *key, UiTextId label, const char *const *values, const char *const *names, int n,
                     UiTextId desc, int cur, const char *note) {
    const char *d = opt_default(key);
    const char *now = opt_value(key);
    if (cur < 0)
        for (int i = 0; i < n; i++)
            if (!strcmp(values[i], now)) cur = i;
    int def = -1;
    for (int i = 0; d && i < n; i++)
        if (!strcmp(values[i], d)) def = i;
    RowView v = {cur >= 0 ? names[cur] : now, cur == def, true, note, value_width(names, n), cur == 0, cur == n - 1};
    int step = row(key, label, desc, v);
    if (!step || n <= 0) return false;
    int nv;
    if (cur < 0) nv = step < 0 ? n - 1 : 0;
    else if (step == 2) nv = (cur + 1) % n;
    else nv = cur + step < 0 ? 0 : cur + step >= n ? n - 1 : cur + step;
    if (nv == cur) return false;
    apply(key, values[nv], label, v.value, names[nv]);
    return true;
}

int opt_choice_index(const char *key, UiTextId label, const char *const *names, int n, int cur, int def, UiTextId desc,
                     const char *note, bool again) {
    RowView v = {cur >= 0 && cur < n ? names[cur] : "", cur == def, true, note, value_width(names, n),
                 cur == 0 && !again, cur == n - 1 && !again};
    int step = row(key, label, desc, v);
    if (!step || n <= 0) return -1;
    int nv = cur < 0 ? (step < 0 ? n - 1 : 0) : step == 2 ? (cur + 1) % n : cur + step < 0 ? 0 : cur + step >= n ? n - 1 : cur + step;
    if (nv == cur && !again) return -1;
    menu_sound_play(MENU_SOUND_CURSOR);
    g_ctx = {true, norm_key(key), v.value, names[nv], label};
    return nv;
}

void opt_context(const char *key, UiTextId label, const char *from, const char *to) {
    g_ctx = {true, norm_key(key), from, to, label};
}

void opt_apply(const char *key, const char *value) {
    std::string k = norm_key(key);
    if (g_ctx.valid) record(k, value, g_ctx.gid, true, g_ctx.label, nullptr, g_ctx.from.c_str(), g_ctx.to.c_str());
    else record(k, value, k, false, UI_MENU_CONTINUE, key, "", value);
}

bool opt_choice(const char *key, UiTextId label, const UiTextId *names, int n, UiTextId desc, const char *note) {
    enum { MAX = 32 };
    const CfgMeta *m = cfg_meta(key);
    const char *values[MAX], *shown[MAX];
    char buf[256];
    int k = 0;
    if (m && !m->def[0]) values[k++] = "";
    snprintf(buf, sizeof buf, "%s", m && m->values ? m->values : "");
    for (char *s = buf, *e; *s && k < MAX; s = e) {
        e = strchr(s, '|');
        if (e) *e++ = 0;
        else e = s + strlen(s);
        values[k++] = s;
    }
    if (n > k) n = k;
    for (int i = 0; i < n; i++) shown[i] = menu_text(names[i]);
    return opt_choice_list(key, label, values, shown, n, desc, -1, note);
}

bool opt_slider(const char *key, UiTextId label, float lo, float hi, UiTextId desc, float step, const char *fmt,
                const char *note, const char *fallback) {
    const char *set = opt_get(key);
    const char *src = fallback && !(set && *set) ? fallback : key;
    const char *d = opt_default(src);
    if (fallback && !(d && *d)) d = opt_default(fallback);
    double v = opt_double(src), def = d && *d ? strtod(d, NULL) : lo;
    char shown[48], ends[2][48];
    snprintf(shown, sizeof shown, fmt, v);
    snprintf(ends[0], sizeof ends[0], fmt, (double)lo);
    snprintf(ends[1], sizeof ends[1], fmt, (double)hi);
    const char *const widest[] = {shown, ends[0], ends[1]};
    RowView rv = {shown, fabs(v - def) < step * 0.01, true, note, value_width(widest, 3), v <= lo + step * 0.01,
                  v >= hi - step * 0.01};
    int s = row(key, label, desc, rv);
    if (!s) return false;
    int steps = g_held && (hi - lo) > 40 * step ? 10 : 1;
    double n = round(v / step) * step + (s == 2 ? step : s * step * steps);
    if (s == 2 && n > hi + step * 0.01) n = lo;
    n = n < lo ? lo : n > hi ? hi : n;
    if (fabs(n - v) < step * 0.01) return false;
    char out[32];
    if (step >= 1) snprintf(out, sizeof out, "%d", (int)lround(n));
    else snprintf(out, sizeof out, "%g", round(n / step) * step);
    const bool unset = fallback && fabs(n - opt_double(fallback)) < step * 0.01;
    char to[48];
    snprintf(to, sizeof to, fmt, unset ? opt_double(fallback) : n);
    apply(key, unset ? "" : out, label, shown, to);
    return true;
}

bool opt_slider_stops(const char *key, UiTextId label, int lo, int hi, UiTextId desc, const char *fmt,
                      const char *const *stop_values, const char *const *stop_names, int stops, const char *cur,
                      const char *note) {
    auto pos_of = [&](const char *v, int fallback) {
        if (!v || !*v) return fallback;
        for (int k = 0; k < stops; k++)
            if (!strcmp(v, stop_values[k])) return hi + 1 + k;
        char *end;
        long n = strtol(v, &end, 10);
        if (*end || end == v) return fallback;
        return n < lo ? lo : n > hi ? hi : (int)n;
    };
    const char *d = opt_default(key);
    int def = pos_of(d, lo), pos = pos_of(cur ? cur : opt_value(key), def);
    char shown[48], ends[48];
    if (pos > hi) snprintf(shown, sizeof shown, "%s", stop_names[pos - hi - 1]);
    else snprintf(shown, sizeof shown, fmt, pos);
    snprintf(ends, sizeof ends, fmt, hi);
    const char *widest[2 + 8] = {shown, ends};
    int nw = 2;
    for (int k = 0; k < stops && nw < 10; k++) widest[nw++] = stop_names[k];
    int top = hi + stops;
    RowView rv = {shown, pos == def, true, note, value_width(widest, nw), pos <= lo, pos >= top};
    int s = row(key, label, desc, rv);
    if (!s) return false;
    int n;
    if (s == 2) n = pos >= top ? lo : pos + 1;
    else {
        int steps = g_held && hi - lo > 40 && (s < 0 ? pos <= hi : pos < hi) ? 10 : 1;
        n = pos + s * steps;
        if (pos < hi && n > hi) n = hi;
        n = n < lo ? lo : n > top ? top : n;
    }
    if (n == pos) return false;
    char out[32], to[48];
    if (n > hi) {
        snprintf(out, sizeof out, "%s", stop_values[n - hi - 1]);
        snprintf(to, sizeof to, "%s", stop_names[n - hi - 1]);
    } else {
        snprintf(out, sizeof out, "%d", n);
        snprintf(to, sizeof to, fmt, n);
    }
    apply(key, out, label, shown, to);
    return true;
}

static void fit_text(ImDrawList *dl, float x0, float x1, float y, float h, ImU32 col, const char *t, bool centred,
                     bool ellipsis = false) {
    ImFont *font = ImGui::GetFont();
    float fs = ImGui::GetFontSize(), room = x1 - x0, tw = font->CalcTextSizeA(fs, FLT_MAX, 0, t).x;
    char shortened[256];
    if (ellipsis && tw > room && strlen(t) < sizeof shortened - 4) {
        size_t n = strlen(t);
        for (;;) {
            while (n && (static_cast<unsigned char>(t[n]) & 0xC0) == 0x80) n--;
            memcpy(shortened, t, n);
            memcpy(shortened + n, "…", sizeof "…");
            if (font->CalcTextSizeA(fs, FLT_MAX, 0, shortened).x <= room || !n) break;
            n--;
            while (n && (static_cast<unsigned char>(t[n]) & 0xC0) == 0x80) n--;
        }
        t = shortened;
        tw = font->CalcTextSizeA(fs, FLT_MAX, 0, t).x;
    } else if (tw > room) {
        fs = fmaxf(fs * room / tw, fs * 0.65f);
        tw = font->CalcTextSizeA(fs, FLT_MAX, 0, t).x;
    }
    float x = centred && tw < room ? x0 + (room - tw) * 0.5f : x0;
    ImVec4 clip(x0, y, x1, y + h);
    dl->AddText(font, fs, ImVec2(x, y + (h - fs) * 0.5f), col, t, NULL, 0, &clip);
}

static void bind_columns(float x, float w, float cx[3], float *cw) {
    float lw = floorf(w * 0.34f), gap = floorf(8 * g_scale);
    *cw = fmaxf(floorf((w - lw - gap * 3) / 3), 1.0f);
    for (int c = 0; c < 3; c++) cx[c] = x + lw + gap + c * (*cw + gap);
}

void opt_bind_header(UiTextId c0, UiTextId c1, UiTextId c2) {
    const float margin = row_margin();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = fmaxf(ImGui::GetContentRegionAvail().x - margin * 2, 1.0f), cx[3], cw, fs = ImGui::GetFontSize() * 0.8f;
    bind_columns(p.x + margin, w, cx, &cw);
    const UiTextId cols[3] = {c0, c1, c2};
    ImFont *font = ImGui::GetFont();
    for (int c = 0; c < 3; c++) {
        const char *t = menu_text(cols[c]);
        float tw = font->CalcTextSizeA(fs, FLT_MAX, 0, t).x;
        ImGui::GetWindowDrawList()->AddText(font, fs, ImVec2(cx[c] + (cw - tw) * 0.5f, p.y), SKIN_TEXT_DIM, t);
    }
    ImGui::Dummy(ImVec2(w, fs * 1.2f));
}

int opt_bind_row(const char *key, const char *label, const char *action, UiTextId desc, const char *const cells[3],
                 const bool enabled[3], const char *note) {
    ImGui::PushID(key);
    const float margin = row_margin();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + margin);
    ImVec2 p = ImGui::GetCursorScreenPos();
    float fs = ImGui::GetFontSize(), w = ImGui::GetContentRegionAvail().x - margin, h = floorf(fs * 1.9f), cx[3], cw;
    bind_columns(p.x, w, cx, &cw);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float pad = 6 * g_scale;
    const float label_x = p.x + 16 * g_scale, label_right = cx[0] - fs * 0.3f;
    const bool has_action = action && action[0] && strcmp(action, "—");
    const float name_h = fs * 0.98f;
    fit_text(dl, label_x, label_right, p.y + fs * 0.08f, name_h, IM_COL32(240, 236, 224, 255), label, false);
    if (has_action)
        fit_text(dl, label_x, label_right, p.y + fs * 1.02f, h - fs * 1.05f, kGrey, action, false, true);
    int result = -1;
    bool row_foc = false, row_hov = false;
    for (int c = 0; c < 3; c++) {
        ImVec2 a(cx[c], p.y), b(cx[c] + cw, p.y + h);
        const char *t = cells[c] && cells[c][0] ? cells[c] : "-";
        if (!enabled[c]) {
            skin_button(dl, a, b, g_scale, 0, true);
            fit_text(dl, a.x + pad, b.x - pad, a.y, h, kGrey, t, true);
            continue;
        }
        ImGui::SetCursorScreenPos(a);
        ImGui::PushID(c);
        ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
        bool clicked = ImGui::InvisibleButton("##cell", ImVec2(cw, h), ImGuiButtonFlags_EnableNav);
        ImGui::PopStyleColor();
        char id[80];
        snprintf(id, sizeof id, "%s#%d", key, c);
        bool to_key = g_focus_key[0] && !strcmp(id, g_focus_key);
        if (g_take_first || to_key) {
            ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
            ImGui::SetNavCursorVisible(true);
            if (g_take_first) ImGui::SetScrollY(0);
            else ImGui::ScrollToItem(ImGuiScrollFlags_KeepVisibleEdgeY);
            g_take_first = false;
            if (to_key) g_focus_key[0] = 0;
        }
        bool hov = ImGui::IsItemHovered(), foc = ImGui::IsItemFocused() && ImGui::GetCurrentContext()->NavCursorVisible;
        row_foc |= foc;
        row_hov |= hov;
        if (clicked) result = c;
        if (foc && (ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false) ||
                    ImGui::IsKeyPressed(ImGuiKey_GamepadFaceUp, false)))
            result = 10 + c;
        skin_button(dl, a, b, g_scale, foc ? 1.0f : hov ? 0.6f : 0.0f, false);
        if (foc || (hov && !g_cursor.on)) g_cursor.on = true, g_cursor.a = a, g_cursor.b = b, g_cursor.alpha = foc ? 1.0f : 0.55f;
        fit_text(dl, a.x + pad, b.x - pad, a.y, h, SKIN_TEXT, t, true);
        ImGui::PopID();
    }
    char k[64];
    snprintf(k, sizeof k, "NX2SARIA_%s", key);
    const char *description = menu_text(desc);
    if (row_foc) desc_set(g_focus, description, k, NULL, NULL);
    if (row_hov) desc_set(g_hover, description, k, NULL, NULL);
    if (!g_first.text[0]) desc_set(g_first, description, k, NULL, NULL);
    if (note && (row_foc || row_hov)) {
        Desc &d = row_hov ? g_hover : g_focus;
        size_t n = strlen(d.text);
        snprintf(d.text + n, sizeof d.text - n, "\n\n%s", note);
    }
    ImGui::SetCursorScreenPos(p);
    ImGui::Dummy(ImVec2(w, h));
    ImGui::PopID();
    return result;
}
