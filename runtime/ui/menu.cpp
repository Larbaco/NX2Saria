// Contains code Copyright (c) 2026 Peiikai, Copyright (c) 2026 Revivedx and the Savage Wind Recomp contributors (MIT; see
// THIRD_PARTY_NOTICES.md).
#include <atomic>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_internal.h"
#include "backends/imgui_impl_vulkan.h"
static int g_font_bad;
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_assert(x) ((x) ? (void)0 : (void)(g_font_bad = 1))
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#include "imstb_truetype.h"
#include "menu_sound.h"
#include "menu_assets.h"
#include "ui/menu_art.h"
#include "ui/menu_skin.h"
#pragma clang diagnostic pop
#include "ui/amiibo_page.h"
#include "ui/binds_page.h"
#include "ui/debug_menu.h"
#include "ui/option_rows.h"
#include "ui/pause_tab.h"
#include "ui/perf_panel.h"
#include "gpu_choice.h"

extern "C" {
#include "amiibo.h"
#include "bugcap.h"
#include "cheats.h"
#include "debug_tools.h"
#include "cfg.h"
#include "folder_pick.h"
#include "fs.h"
#include "input.h"
#include "input_map.h"
#include "mods.h"
#include "perfmon.h"
#include "platform/platform.h"
#include "save_share.h"
#include "sprint.h"
#include "sysinfo.h"
#include "text_speed.h"
#include "amiibo_daily.h"
#include "ui/menu.h"
#include "ui_text.h"
#include "vk_dlssg.h"
#include "vk_lowlat.h"
#include "hw_detect.h"
#include "vram_fit.h"
#include "vram_warn.h"
void rt_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void vk_queue_lock(void);
void vk_queue_unlock(void);
void res_sizes(unsigned out[2], unsigned scene[2]);
int res_adjustable(void);
int wide_available(void);
int wide_scene_settable(void);
void wide_picture(unsigned *w, unsigned *h);
int ov_movie_live(void);
int wide_menu_open(void);
int gfx_picture_rect(float out[4]);
int gfx_surface_hdr10(void);
int wide_loading_open(void);
int64_t frame_step_world_us(void);
double wide_scene_share(void);
double wide_scene_wanted(void);
int wide_scene_restart(void);
const char *const *vk_color_preset(const char *name);
int vk_hdr_wanted(void);
int vk_hdr_active(void);
int vk_hdr_restart_for(const char *value);
int vk_dlaa_needs_restart(void);
int vk_ngxfg_wanted(void);
int vk_ngxfg_max(void);
int vk_ffx_available(void);
int vk_ffx_framegen_available(void);
int vk_ffx4_available(void);
int vk_ffx_fsr4(void);
void vk_ffx4_retry(void);
const char *vk_ffx4_name(void);
int vk_fg4_available(void);
int vk_fg4_failed(void);
int vk_fg4_active(void);
const char *vk_fg4_name(void);
int vk_texture_detail_auto(void);
int vk_card_usable(VkPhysicalDevice pd);
int vk_exec_replay_workers_auto(const char **why);
double frame_step_limit_hz(void);
int frame_limit_adjustable(void);
int rtss_limit_state(void);
int frame_free_running(void);
extern const int rt_game_version;
extern const char rt_port_version[];
void cheats_frame(uint64_t frame);
void tab_cheats(float scale);
}

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc23-extensions"
static const unsigned char roboto_ttf[] = {
#ifdef __has_embed
#embed "fonts/Roboto-Medium.ttf"
#else
#include "roboto_ttf.h"
#endif
};
#pragma clang diagnostic pop

static std::atomic<int> g_open;
static std::atomic<int> g_reset;

static PlatMutex open_lock = PLAT_MUTEX_INIT;

static PlatMutex ev_lock = PLAT_MUTEX_INIT;
static int ev_n;

static std::atomic<int> g_pause_tab{0};
static const char *g_pause_why = "closed";
static uint64_t g_more_open;

static int change_open(int want) {
    plat_mutex_lock(&open_lock);
    if (want == 2) {
        if (g_open.load()) {
            plat_mutex_unlock(&open_lock);
            return 0;
        }
        g_pause_tab.store(1);
        want = 1;
    }
    int open = want < 0 ? !g_open.load() : want;
    if (g_open.load() != open) {
        g_open.store(open);
        g_reset.store(1);
        plat_mutex_lock(&ev_lock);
        ev_n = 0;
        plat_mutex_unlock(&ev_lock);
        input_menu(open);
        const bool tab = g_pause_tab.load() != 0;
        menu_sound_play(open ? (tab ? MENU_SOUND_TAB_RIGHT : MENU_SOUND_OPEN) : (tab ? MENU_SOUND_TAB_LEFT : MENU_SOUND_CLOSE));
        rt_log("menu: %s%s", open ? "open" : "closed", g_pause_tab.load() ? " (the pause menu's tab)" : "");
        if (g_pause_tab.load()) pause_tab_set_open(open, g_pause_why);
        if (!open) g_pause_tab.store(0);
        if (!open && g_more_open) pause_tab_more(0, (plat_now_ns() - g_more_open) / 1000000ull), g_more_open = 0;
    }
    plat_mutex_unlock(&open_lock);
    return open;
}

static void set_open(int open) { change_open(open); }

static std::atomic<int> g_debug{0};
static std::atomic<int> g_picker{0};
extern "C" int menu_debug_enabled(void) { return cfg_bool("NX2SARIA_DEBUG_MENU"); }

static std::atomic<int> g_close_req;

static int g_failed_flag();
static std::atomic<int> g_booting{1};
extern "C" int menu_is_open(void) { return g_open.load(std::memory_order_relaxed); }
extern "C" void menu_toggle(void) {
    if (g_failed_flag() || g_booting.load(std::memory_order_acquire)) return;
    if (!menu_is_open()) g_debug.store(0), g_picker.store(0);
    if (menu_is_open() && !g_debug.load() && !g_picker.load() && opt_draft_any()) g_close_req.store(1);
    else change_open(-1);
}
extern "C" void menu_debug_toggle(void) {
    if (!menu_debug_enabled() || g_failed_flag() || g_booting.load(std::memory_order_acquire)) return;
    if (menu_is_open()) {
        if (g_debug.load()) change_open(0);
        return;
    }
    g_debug.store(1);
    g_picker.store(0);
    change_open(1);
}

enum { EV_KEY, EV_POS, EV_BUTTON, EV_WHEEL, EV_FOCUS, EV_CHAR, EV_MAX = 256 };
struct Ev { int type, a, b; float x, y; };
static Ev ev_q[EV_MAX];

static void push(Ev e) {
    if (!menu_is_open()) return;
    plat_mutex_lock(&ev_lock);
    if (!menu_is_open()) {
        plat_mutex_unlock(&ev_lock);
        return;
    }
    if (e.type == EV_POS && ev_n && ev_q[ev_n - 1].type == EV_POS) ev_q[ev_n - 1] = e;
    else if (ev_n < EV_MAX) ev_q[ev_n++] = e;
    plat_mutex_unlock(&ev_lock);
}

extern "C" void menu_event_key(int key, int down) { push({EV_KEY, key, down, 0, 0}); }
extern "C" void menu_event_mouse_pos(float x, float y) { push({EV_POS, 0, 0, x, y}); }
extern "C" void menu_event_mouse_button(int button, int down) { push({EV_BUTTON, button, down, 0, 0}); }
extern "C" void menu_event_wheel(float dx, float dy) { push({EV_WHEEL, 0, 0, dx, dy}); }
extern "C" void menu_event_focus(int focused) { push({EV_FOCUS, focused, 0, 0, 0}); }
extern "C" void menu_event_char(unsigned c) { push({EV_CHAR, (int)c, 0, 0, 0}); }

static PlatMutex paste_lock = PLAT_MUTEX_INIT;
static char paste_text[256];
static int paste_ready;

extern "C" void menu_event_paste(const char *utf8) {
    plat_mutex_lock(&paste_lock);
    snprintf(paste_text, sizeof paste_text, "%s", utf8 ? utf8 : "");
    paste_ready = 1;
    plat_mutex_unlock(&paste_lock);
}

extern "C" int menu_paste_take(char *out, size_t cap) {
    plat_mutex_lock(&paste_lock);
    int ready = paste_ready;
    if (ready && cap) snprintf(out, cap, "%s", paste_text);
    paste_ready = 0;
    plat_mutex_unlock(&paste_lock);
    return ready;
}

static void drain_events(ImGuiIO &io) {
    Ev q[EV_MAX];
    plat_mutex_lock(&ev_lock);
    int n = ev_n;
    for (int i = 0; i < n; i++) q[i] = ev_q[i];
    ev_n = 0;
    plat_mutex_unlock(&ev_lock);
    for (int i = 0; i < n; i++) {
        const Ev &e = q[i];
        switch (e.type) {
        case EV_KEY:
            io.AddKeyEvent((ImGuiKey)e.a, e.b != 0);
            if (e.a == ImGuiKey_LeftShift) io.AddKeyEvent(ImGuiMod_Shift, e.b != 0);
            if (e.a == ImGuiKey_LeftCtrl) io.AddKeyEvent(ImGuiMod_Ctrl, e.b != 0);
            break;
        case EV_POS: io.AddMousePosEvent(e.x, e.y); break;
        case EV_BUTTON: io.AddMouseButtonEvent(e.a, e.b != 0); break;
        case EV_WHEEL: io.AddMouseWheelEvent(e.x, e.y); break;
        case EV_FOCUS: io.AddFocusEvent(e.a != 0); break;
        case EV_CHAR: io.AddInputCharacter((unsigned)e.a); break;
        }
    }
}

static const struct { int bit; ImGuiKey key; } pad_keys[] = {
    {B_A, ImGuiKey_GamepadFaceDown}, {B_B, ImGuiKey_GamepadFaceRight}, {B_X, ImGuiKey_GamepadFaceUp},
    {B_Y, ImGuiKey_GamepadFaceLeft}, {B_L, ImGuiKey_GamepadL1}, {B_R, ImGuiKey_GamepadR1},
    {B_ZL, ImGuiKey_GamepadL2}, {B_ZR, ImGuiKey_GamepadR2}, {B_LEFT, ImGuiKey_GamepadDpadLeft},
    {B_UP, ImGuiKey_GamepadDpadUp}, {B_RIGHT, ImGuiKey_GamepadDpadRight}, {B_DOWN, ImGuiKey_GamepadDpadDown},
};
static uint64_t pad_prev;
static int stick_dir = -1;
static int g_pad_plus;

static int stick_to_dpad(int32_t lx, int32_t ly) {
    float x = lx / 32767.0f, y = ly / 32767.0f;
    float along = stick_dir == B_LEFT ? -x : stick_dir == B_RIGHT ? x : stick_dir == B_UP ? y : stick_dir == B_DOWN ? -y : 0;
    if (stick_dir >= 0 && along > 0.35f) return stick_dir;
    if (fmaxf(fabsf(x), fabsf(y)) < 0.5f) return -1;
    return fabsf(x) > fabsf(y) ? (x < 0 ? B_LEFT : B_RIGHT) : (y > 0 ? B_UP : B_DOWN);
}

static void pad_events(ImGuiIO &io, int reset) {
    uint64_t b;
    int32_t lx, ly;
    int have = input_pad_read(&b, &lx, &ly);
    if (g_pause_tab.load(std::memory_order_relaxed) || g_more_open) input_script_buttons(&b), have = 1;
    if (have) io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    else io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;
    stick_dir = stick_to_dpad(lx, ly);
    if (stick_dir >= 0) b |= 1ull << stick_dir;
    if (reset) {
        pad_prev = b;
        g_pad_plus = 0;
        return;
    }
    for (const auto &k : pad_keys) {
        uint64_t m = 1ull << k.bit;
        if ((b ^ pad_prev) & m) io.AddKeyEvent(k.key, (b & m) != 0);
    }
    if (g_pause_tab.load(std::memory_order_relaxed) && (b & ~pad_prev & (1ull << B_PLUS))) g_pad_plus = 1;
    pad_prev = b;
}

static int g_ctx_ok;
static std::atomic<int> g_failed;
static int g_failed_flag() { return g_failed.load(std::memory_order_relaxed); }
static int g_vk_ok;
static VkFormat g_vk_fmt;
static float g_font_scale;
static float g_style_scale;
static ImGuiStyle g_base_style;
static uint64_t g_last_ns;
static uint64_t g_cost_ns, g_cost_frames;

static float menu_scale(uint32_t w, uint32_t h) {
    float least = 720.0f * (float)gfx_window_dpi() / 96.0f, lines = fminf((float)h, (float)w * 0.68f);
    return (lines >= least ? lines : least) / 1080.0f;
}

static const ImVec4 kGold(0.93f, 0.76f, 0.38f, 1);

static void triforce(ImDrawList *dl, float x, float y, float h, ImU32 col) {
    float w = h * 1.1547f;
    ImVec2 t(x, y), l(x - w / 2, y + h), r(x + w / 2, y + h), ml(x - w / 4, y + h / 2), mr(x + w / 4, y + h / 2), mb(x, y + h);
    dl->AddTriangleFilled(t, mr, ml, col);
    dl->AddTriangleFilled(ml, mb, l, col);
    dl->AddTriangleFilled(mr, r, mb, col);
}

static void make_context(void) {
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendPlatformName = "nx2navi_menu";
    GImGui->ConfigNavWindowingKeyNext = GImGui->ConfigNavWindowingKeyPrev = 0;
    skin_style(g_base_style);
    ImGui::GetStyle() = g_base_style;
    g_ctx_ok = 1;
}

static std::atomic<int> g_vk_errors;
static void menu_vk_check(VkResult r) {
    if (r == VK_SUCCESS) return;
    if (g_vk_errors.fetch_add(1) < 8) rt_log("menu: Dear ImGui's Vulkan back end got VkResult %d", (int)r);
}
extern "C" int menu_vk_errors(void) { return g_vk_errors.load(); }

static void vk_down(void) {
    if (!g_vk_ok) return;
    menu_art_release();
    ImGui_ImplVulkan_Shutdown();
    g_vk_ok = 0;
    g_font_scale = 0;
}

static int vk_up(const MenuTarget *t) {
    if (g_vk_ok && g_vk_fmt == t->format) return 1;
    if (g_vk_ok) {
        vk_queue_lock();
        vkQueueWaitIdle(t->queue);
        vk_queue_unlock();
        vk_down();
    }
    ImGui_ImplVulkan_InitInfo ii = {};
    ii.ApiVersion = VK_API_VERSION_1_3;
    ii.Instance = t->instance;
    ii.PhysicalDevice = t->phys;
    ii.Device = t->device;
    ii.QueueFamily = t->queue_family;
    ii.Queue = t->queue;
    ii.DescriptorPoolSize = 32;
    ii.CheckVkResultFn = menu_vk_check;
    ii.MinImageCount = 2;
    ii.ImageCount = 4;
    ii.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    ii.UseDynamicRendering = true;
    g_vk_fmt = t->format;
    ii.PipelineRenderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    ii.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    ii.PipelineRenderingCreateInfo.pColorAttachmentFormats = &g_vk_fmt;
    if (!ImGui_ImplVulkan_Init(&ii)) return 0;
    g_vk_ok = 1;
    return 1;
}

static const ImWchar kRanges[] = {0x0020, 0x00FF, 0x0400, 0x052F, 0x2010, 0x2027, 0};
static const struct { const char *lang; UiTextId name; } kCjk[] = {
    {"ja", UI_MENU_LANGNAME_JA}, {"ko", UI_MENU_LANGNAME_KO}, {"zh-Hans", UI_MENU_LANGNAME_ZH_HANS},
    {"zh-Hant", UI_MENU_LANGNAME_ZH_HANT},
};

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static int font_usable(const uint8_t *d, uint64_t n) {
    if (!d || n <= 100 || n > INT_MAX) return 0;
    int off = stbtt_GetFontOffsetForIndex(d, 0);
    if (off < 0 || (uint64_t)off + 12 > n) return 0;
    uint32_t tables = (uint32_t)d[off + 4] << 8 | d[off + 5];
    if ((uint64_t)off + 12 + 16ull * tables > n) return 0;
    for (uint32_t i = 0; i < tables; i++) {
        const uint8_t *r = d + off + 12 + 16 * i;
        if ((uint64_t)be32(r + 8) + be32(r + 12) > n) return 0;
    }
    stbtt_fontinfo fi;
    g_font_bad = 0;
    return stbtt_InitFont(&fi, d, off) && !g_font_bad;
}

extern "C" int menu_font_check(void) {
    uint64_t n = sizeof roboto_ttf;
    uint8_t *bad = (uint8_t *)malloc(n);
    if (!bad) return 0;
    memcpy(bad, roboto_ttf, n);
    uint32_t tables = (uint32_t)bad[4] << 8 | bad[5];
    for (uint32_t i = 0; i < tables; i++)
        if (!memcmp(bad + 12 + 16 * i, "cmap", 4)) memset(bad + 12 + 16 * i, 0, 4);
    int ok = font_usable(roboto_ttf, n) && !font_usable(roboto_ttf, 4096) && !font_usable(roboto_ttf, 100) &&
             !font_usable(bad, n);
    memset(bad, 0, 4);
    ok = ok && !font_usable(bad, n);
    free(bad);
    return ok;
}

static void add_row_glyphs(void *ctx, const char *text) {
    static_cast<ImFontGlyphRangesBuilder *>(ctx)->AddText(text);
}

static uint8_t *g_game_font, *g_title_font_data;
static size_t g_game_font_n, g_title_font_n;
static ImFont *g_title_font;

static void game_fonts_read(void) {
    static int done;
    if (done) return;
    done = 1;
    g_game_font = menu_asset_font("Font2/Scalable/ClarimoUDPE-Regular.Nin_Ounce_NVN2.bfotf.zs", &g_game_font_n);
    g_title_font_data = menu_asset_font("Font2/Scalable/CetraDisplayPro-Medium.Nin_Ounce_NVN2.bfotf.zs", &g_title_font_n);
    if (g_game_font && !font_usable(g_game_font, g_game_font_n)) free(g_game_font), g_game_font = NULL;
    if (g_title_font_data && !font_usable(g_title_font_data, g_title_font_n)) free(g_title_font_data), g_title_font_data = NULL;
    rt_log("menu: the game's interface fonts: text %s, titles %s", g_game_font ? "ClarimoUDPE" : "not found (Roboto)",
           g_title_font_data ? "CetraDisplayPro" : "not found");
}

static int fonts_up(float scale) {
    enum { N = sizeof kCjk / sizeof *kCjk };
    ImFontAtlas *fa = ImGui::GetIO().Fonts;
    fa->Clear();
    game_fonts_read();
    g_title_font = nullptr;
    ImFontConfig fc;
    fc.FontDataOwnedByAtlas = false;
    if (g_game_font) {
        fa->AddFontFromMemoryTTF(g_game_font, (int)g_game_font_n, 25.5f * scale, &fc, kRanges);
        ImFontConfig rc;
        rc.MergeMode = true;
        rc.FontDataOwnedByAtlas = false;
        fa->AddFontFromMemoryTTF((void *)roboto_ttf, (int)sizeof roboto_ttf, 26.0f * scale, &rc, kRanges);
    } else {
        fa->AddFontFromMemoryTTF((void *)roboto_ttf, (int)sizeof roboto_ttf, 26.0f * scale, &fc, kRanges);
    }
    uint8_t *data[N] = {};
    uint64_t sizes[N] = {};
    ImVector<ImWchar> ranges[N], title_ranges[N];
    int merged = 0;
    for (int i = 0; i < N; i++) {
        char path[512];
        uint64_t &size = sizes[i];
        if (!plat_font_path(kCjk[i].lang, path, sizeof path) || !(data[i] = plat_file_read(path, &size))) continue;
        if (!font_usable(data[i], size)) {
            rt_log("menu: system font %s left out: the font parser cannot read it (%llu bytes)", path,
                   (unsigned long long)size);
            free(data[i]);
            data[i] = NULL;
            continue;
        }
        merged++;
        ImFontGlyphRangesBuilder b;
        b.AddText(ui_text_in("en", kCjk[i].name));
        ui_text_visit(kCjk[i].lang, add_row_glyphs, &b);
        for (int k = 0; k < cheats_count(); k++)
            if (const CheatInfo *ci = cheats_info(k); ci && ci->name) b.AddText(ci->name);
        for (int k = 0; k < mods_count(); k++)
            if (const ModInfo *mi = mods_at(k); mi) b.AddText(mi->name);
        b.BuildRanges(&ranges[i]);
        ImFontConfig mc;
        mc.MergeMode = true;
        mc.FontDataOwnedByAtlas = false;
        fa->AddFontFromMemoryTTF(data[i], (int)size, 26.0f * scale, &mc, ranges[i].Data);
    }
    if (g_title_font_data) {
        ImFontConfig tc;
        tc.FontDataOwnedByAtlas = false;
        g_title_font = fa->AddFontFromMemoryTTF(g_title_font_data, (int)g_title_font_n, 31.0f * scale, &tc, kRanges);
        ImFontConfig rc;
        rc.MergeMode = true;
        rc.FontDataOwnedByAtlas = false;
        fa->AddFontFromMemoryTTF((void *)roboto_ttf, (int)sizeof roboto_ttf, 30.0f * scale, &rc, kRanges);
        static const UiTextId titles[] = {UI_MENU_TITLE, UI_MENU_TAB_SCREEN, UI_MENU_TAB_GAME, UI_MENU_TAB_SOUND,
                                          UI_MENU_TAB_CONTROLS, UI_MENU_TAB_AMIIBO, UI_MENU_TAB_CHEATS, UI_MENU_PAUSE_TAB,
                                          UI_BOOT_TITLE};
        for (int i = 0; i < N; i++) {
            if (!data[i]) continue;
            ImFontGlyphRangesBuilder b;
            for (UiTextId id : titles) b.AddText(ui_text_in(kCjk[i].lang, id));
            b.BuildRanges(&title_ranges[i]);
            ImFontConfig mc;
            mc.MergeMode = true;
            mc.FontDataOwnedByAtlas = false;
            fa->AddFontFromMemoryTTF(data[i], (int)sizes[i], 31.0f * scale, &mc, title_ranges[i].Data);
        }
    }
    skin_atlas_add(fa);
    skin_atlas_fill(fa);
    unsigned char *px = nullptr;
    int aw = 0, ah = 0;
    fa->GetTexDataAsRGBA32(&px, &aw, &ah);
    ImTextureID tex = 0;
    int ok = menu_font_upload(px, aw, ah, &tex);
    if (ok) fa->SetTexID(tex);
    if (ok && g_game_font && fa->Fonts.Size) fa->Fonts[0]->FontSize *= 1.16f;
    fa->ClearInputData();
    fa->ClearTexData();
    for (uint8_t *d : data) free(d);
    rt_log("menu: font atlas at %.1f px, language names from %d of %d system fonts", 26.0f * scale, merged, (int)N);
    g_font_scale = ok ? scale : 0;
    return ok;
}

static float g_scale_seen;
static uint64_t g_scale_seen_ns;

static int fonts_for(float scale, uint64_t now) {
    ImGuiIO &io = ImGui::GetIO();
    if (g_font_scale == scale) {
        io.FontGlobalScale = 1;
        return 1;
    }
    if (scale != g_scale_seen) g_scale_seen = scale, g_scale_seen_ns = now;
    if (g_font_scale > 0 && now - g_scale_seen_ns < 250000000ull) {
        io.FontGlobalScale = scale / g_font_scale;
        return 1;
    }
    io.FontGlobalScale = 1;
    return fonts_up(scale);
}

extern "C" void menu_vk_release(void) {
    if (g_ctx_ok) vk_down();
}

enum { TAB_SCREEN, TAB_GAME, TAB_SOUND, TAB_CONTROLS, TAB_AMIIBO, TAB_CHEATS, TAB_COUNT };
static int tabs_shown(void) { return cheats_count() > 0 ? TAB_COUNT : TAB_CHEATS; }
enum { TEST_PAGE_FRAMES = 30, TEST_SETS = 16, TEST_OPENS = 8 };
struct TestSet { uint64_t frame; char key[64], value[64]; };
static struct {
    int parsed;
    uint64_t at[TEST_OPENS];
    int n_at, next_at;
    uint64_t per_page;
    uint64_t opened;
    int page;
    TestSet sets[TEST_SETS];
    int n_sets, next_set;
} g_test = {0, {}, 0, 0, 0, 0, -1, {}, 0, 0};

static void test_parse(void) {
    g_test.parsed = 1;
    if (const char *e = cfg_get("NX2SARIA_MENU_TEST")) {
        long long per = 0;
        if (const char *c = strchr(e, ':')) per = atoll(c + 1);
        for (const char *s = e; *s && *s != ':' && g_test.n_at < TEST_OPENS;) {
            char *end;
            unsigned long long at = strtoull(s, &end, 10);
            if (end == s) break;
            if (at > 0 && (!g_test.n_at || at > g_test.at[g_test.n_at - 1])) g_test.at[g_test.n_at++] = at;
            s = *end == ',' ? end + 1 : end;
        }
        g_test.per_page = per > 0 ? (uint64_t)per : TEST_PAGE_FRAMES;
    }
    const char *s = cfg_get("NX2SARIA_MENU_TEST_SET");
    while (s && *s) {
        char *end;
        unsigned long long f = strtoull(s, &end, 10);
        if (*end != ':') break;
        s = end + 1;
        while (*s && *s != ';' && g_test.n_sets < TEST_SETS) {
            size_t n = strcspn(s, ",;");
            const char *eq = (const char *)memchr(s, '=', n);
            if (eq) {
                TestSet &t = g_test.sets[g_test.n_sets++];
                t.frame = f;
                int kn = (int)(eq - s), vn = (int)(s + n - eq - 1);
                snprintf(t.key, sizeof t.key, "%s%.*s", strncmp(s, "NX2SARIA_", strlen("NX2SARIA_")) ? "NX2SARIA_" : "", kn, s);
                snprintf(t.value, sizeof t.value, "%.*s", vn, eq + 1);
            }
            s += n;
            if (*s == ',') s++;
        }
        while (*s && *s != ';') s++;
        if (*s == ';') s++;
    }
    if (g_test.n_at || g_test.n_sets)
        rt_log("menu: test hook: open at frame %llu (%d times), %llu frames per page, %d settings changes",
               (unsigned long long)(g_test.n_at ? g_test.at[0] : 0), g_test.n_at, (unsigned long long)g_test.per_page,
               g_test.n_sets);
}

static void test_apply_sets(uint64_t frame) {
    for (; g_test.next_set < g_test.n_sets && g_test.sets[g_test.next_set].frame <= frame; g_test.next_set++) {
        const TestSet &t = g_test.sets[g_test.next_set];
        rt_log("menu: test hook: frame %llu: %s=%s", (unsigned long long)frame, t.key, t.value);
        if (!strcmp(t.key, "NX2SARIA_SAVE")) {
            const int kept = cfg_test_keep();
            rt_log("menu: test hook: cfg_save of %d test value(s) %s", kept, cfg_save() ? "wrote" : "failed");
        } else if (!strcmp(t.key, "NX2SARIA_LEAVE")) {
            if (opt_draft_count() > 0) g_close_req.store(1);
            else rt_log("menu: test hook: LEAVE with nothing in the draft: no box to open");
        } else if (!strcmp(t.key, "NX2SARIA_APPLY")) {
            const int n = opt_draft_any();
            rt_log("menu: test hook: Apply of the draft's %d change(s) %s", n, opt_draft_apply() ? "saved" : "failed");
        } else if (!strcmp(t.key, "NX2SARIA_FOCUS")) {
            char row[80];
            snprintf(row, sizeof row, "%s%s", !strncmp(t.value, "BIND_", 5) || !strncmp(t.value, "NX2SARIA_", 9)
                                                    ? ""
                                                    : "NX2SARIA_",
                     t.value);
            opt_focus_key(row);
        } else {
            cfg_set_test(t.key, t.value);
        }
    }
}

enum { MAX_GPUS = 8 };
static char g_gpu_names[MAX_GPUS][VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];
static VkPhysicalDevice g_gpu_pd[MAX_GPUS];
static int g_gpu_type[MAX_GPUS];
static int g_gpu_usable[MAX_GPUS];
static int g_gpu_n = -1;

static void list_gpus(VkInstance inst) {
    if (g_gpu_n >= 0) return;
    VkPhysicalDevice pd[MAX_GPUS];
    uint32_t n = MAX_GPUS;
    if (vkEnumeratePhysicalDevices(inst, &n, pd) < 0) n = 0;
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(pd[i], &p);
        snprintf(g_gpu_names[i], sizeof g_gpu_names[i], "%s", p.deviceName);
        g_gpu_pd[i] = pd[i];
        g_gpu_type[i] = (int)p.deviceType;
        g_gpu_usable[i] = -1;
    }
    g_gpu_n = (int)n;
}

static const int kSizes[][2] = {{1280, 720}, {1366, 768}, {1600, 900}, {1920, 1080}, {2560, 1440}, {3200, 1800},
                                {3840, 2160}};
enum { MAX_SIZES = sizeof kSizes / sizeof *kSizes + 1 };

static void row_window_size(void) {
    int sz[MAX_SIZES][2], n = 0, mw = 0, mh = 0, cw = 0, ch = 0;
    int have = gfx_window_monitor_size(&mw, &mh);
    int cur_ok = sscanf(opt_value("NX2SARIA_WINDOW_SIZE"), "%dx%d", &cw, &ch) == 2;
    bool listed = false;
    for (size_t i = 0; i < sizeof kSizes / sizeof *kSizes; i++) {
        int w = kSizes[i][0], h = kSizes[i][1];
        bool cur = cur_ok && w == cw && h == ch;
        if (have && (w > mw || h > mh) && !cur && !(w == 1920 && h == 1080)) continue;
        listed |= cur;
        sz[n][0] = w, sz[n][1] = h, n++;
    }
    if (cur_ok && !listed) {
        int at = n;
        while (at > 0 && (long)sz[at - 1][0] * sz[at - 1][1] > (long)cw * ch) at--;
        memmove(sz[at + 1], sz[at], sizeof sz[0] * (n - at));
        sz[at][0] = cw, sz[at][1] = ch, n++;
    }
    char vals[MAX_SIZES][24], shown[MAX_SIZES][32];
    const char *pv[MAX_SIZES], *pn[MAX_SIZES];
    for (int i = 0; i < n; i++) {
        snprintf(vals[i], sizeof vals[i], "%dx%d", sz[i][0], sz[i][1]);
        snprintf(shown[i], sizeof shown[i], "%d × %d", sz[i][0], sz[i][1]);
        pv[i] = vals[i];
        pn[i] = shown[i];
    }
    opt_choice_list("NX2SARIA_WINDOW_SIZE", UI_MENU_WINDOW_SIZE, pv, pn, n, UI_MENU_WINDOW_SIZE_DESC);
}

static void row_output_res(void) {
    static const char *const vals[] = {"1280x720", "1600x900", "1920x1080", "2560x1440", "3200x1800", "3840x2160",
                                       "2560x1080", "3440x1440", "3840x1600", "5120x2160", "3840x1080", "5120x1440",
                                       "1280x800", "1680x1050", "1920x1200", "2560x1600",
                                       "1920x1280", "2160x1440", "2256x1504", "2880x1920", "3000x2000"};
    enum { N = sizeof vals / sizeof *vals };
    char shown[N][32];
    const char *names[N];
    for (int i = 0; i < N; i++) {
        int w = 0, h = 0;
        sscanf(vals[i], "%dx%d", &w, &h);
        snprintf(shown[i], sizeof shown[i], "%d × %d", w, h);
        names[i] = shown[i];
    }
    opt_choice_list("NX2SARIA_OUTPUT_RES", UI_MENU_OUTPUT_RES, vals, names, N, UI_MENU_OUTPUT_RES_DESC);
}

static void row_texture_detail(void) {
    static const char *const vals[] = {"auto", "full", "reduced", "low"};
    static const UiTextId level[] = {UI_MENU_TEX_DETAIL_FULL, UI_MENU_TEX_DETAIL_REDUCED, UI_MENU_TEX_DETAIL_LOW};
    static char picked[96];
    int a = vk_texture_detail_auto();
    snprintf(picked, sizeof picked, menu_text(UI_MENU_TEX_DETAIL_AUTO), menu_text(level[a < 0 || a > 2 ? 0 : a]));
    const char *names[] = {picked, menu_text(level[0]), menu_text(level[1]), menu_text(level[2])};
    opt_choice_list("NX2SARIA_TEXTURE_DETAIL", UI_MENU_TEX_DETAIL, vals, names, 4, UI_MENU_TEX_DETAIL_DESC);
}

static void row_gpu(void) {
    const char *names[MAX_GPUS + 1];
    GpucCard cards[MAX_GPUS];
    names[0] = menu_text(UI_MENU_GPU_AUTO);
    for (int i = 0; i < g_gpu_n; i++) {
        names[i + 1] = g_gpu_names[i];
        cards[i].name = g_gpu_names[i], cards[i].type = g_gpu_type[i], cards[i].usable = 0;
    }
    const char *at = opt_value("NX2SARIA_GPU_INDEX");
    const int want = *at ? atoi(at) : -1, from_env = cfg_from_env("NX2SARIA_GPU_INDEX") != 0;
    const char *name = opt_value("NX2SARIA_GPU_NAME");
    if (gpuc_legacy_applies(cards, g_gpu_n, want, name, from_env))
        for (int i = 0; i < g_gpu_n; i++) {
            if (cards[i].type != GPUC_DISCRETE) continue;
            if (g_gpu_usable[i] < 0) g_gpu_usable[i] = vk_card_usable(g_gpu_pd[i]);
            cards[i].usable = g_gpu_usable[i];
        }
    const int cur = gpuc_shown(cards, g_gpu_n, want, name, from_env) + 1;
    const int nv = opt_choice_index("NX2SARIA_GPU_INDEX", UI_MENU_GPU, names, g_gpu_n + 1, cur, 0, UI_MENU_GPU_DESC);
    if (nv < 0 || nv > g_gpu_n) return;
    char pos[8];
    snprintf(pos, sizeof pos, "%d", nv - 1);
    opt_apply("NX2SARIA_GPU_INDEX", nv ? pos : "");
    opt_apply("NX2SARIA_GPU_NAME", nv ? g_gpu_names[nv - 1] : "");
}

static int mals_regions(const char *out[24]) {
    static char reg[24][5];
    static int n = -1;
    if (n < 0 && romfs_file_count()) {
        n = 0;
        for (uint64_t i = 0, c = romfs_file_count(); i < c && n < 24; i++) {
            uint64_t off, size;
            const char *p = romfs_entry(i, &off, &size);
            if (!p || strncmp(p, "Mals/", 5) || strlen(p) < 10 || p[9] != '.') continue;
            memcpy(reg[n], p + 5, 4);
            reg[n][4] = 0;
            bool seen = false;
            for (int j = 0; j < n; j++) seen |= !strcmp(reg[j], reg[n]);
            if (!seen) n++;
        }
    }
    for (int i = 0; i < n; i++) out[i] = reg[i];
    return n > 0 ? n : 0;
}

static void row_game_lang(void) {
    UiGameLang langs[16];
    const char *regions[24];
    int nr = mals_regions(regions);
    int n = nr ? ui_text_game_langs_in(regions, nr, langs, 16) : ui_text_game_langs(rt_game_version, langs, 16);
    const char *vals[17], *names[17];
    vals[0] = "";
    names[0] = menu_text(UI_MENU_GAME_LANG_AUTO);
    const char *now = opt_value("NX2SARIA_LANG");
    int cur = *now ? -1 : 0;
    for (int i = 1; i <= n; i++) {
        vals[i] = langs[i - 1].code;
        names[i] = menu_text(langs[i - 1].name);
        if (*now && !strcmp(now, vals[i])) cur = i;
    }
    for (int i = 1; i <= n && *now && cur < 0; i++)
        if (!strcmp(ui_text_lang(now), ui_text_lang(vals[i]))) cur = i;
    opt_choice_list("NX2SARIA_LANG", UI_MENU_GAME_LANG, vals, names, n + 1, UI_MENU_GAME_LANG_DESC, cur);
}

static void row_fps_limit() {
    static const char *const stop_values[] = {"display", "unlimited"};
    const char *stop_names[2] = {menu_text(UI_MENU_FPS_DISPLAY), menu_text(UI_MENU_FPS_UNLIMITED)};
    int stops = frame_free_running() ? 2 : 1;
    static char display[64];
    double hz = gfx_window_refresh_hz();
    if (hz > 0) {
        snprintf(display, sizeof display, menu_text(UI_MENU_FPS_DISPLAY_HZ), hz);
        stop_names[0] = display;
    }
    const char *set = opt_get("NX2SARIA_FPS_LIMIT"), *old = cfg_get("NX2SARIA_FPS60");
    const char *cur = (!set || !*set) && old && !strcmp(old, "0") ? "30" : nullptr;
    int rtss = rtss_limit_state();
    const char *note = rtss == 1   ? menu_text(UI_MENU_FPS_RTSS)
                       : rtss == 2 ? menu_text(UI_MENU_FPS_RTSS_READONLY)
                       : rtss == 3 ? menu_text(UI_MENU_FPS_RTSS_IGNORED)
                       : rtss == 4 ? menu_text(UI_MENU_FPS_RTSS_TWO)
                                   : nullptr;
    opt_slider_stops("NX2SARIA_FPS_LIMIT", UI_MENU_FPS_LIMIT, 30, 240, UI_MENU_FPS_LIMIT_DESC, "%d FPS", stop_values,
                     stop_names, stops, cur, note);
}

static const char *vsync_note(void) {
    const char *got = gfx_window_present_mode();
    if (!*got) return nullptr;
    bool off = !strcmp(cfg_value("NX2SARIA_VK_PRESENT"), "immediate"), tears = !strcmp(got, "immediate");
    if (off == tears) return nullptr;
    static char note[96];
    snprintf(note, sizeof note, menu_text(UI_MENU_PRESENT_IN_USE),
             menu_text(tears ? UI_MENU_PRESENT_IMMEDIATE : UI_MENU_PRESENT_VSYNC));
    return note;
}

static void row_low_latency(void) {
    static const char *const values[] = {"0", "1", "2"};
    const char *names[] = {menu_text(UI_MENU_OFF), menu_text(UI_MENU_ON), menu_text(UI_MENU_LOW_LATENCY_BOOST)};
    const int st = vk_lowlat_status(), n = st == VK_LOWLAT_AMD ? 2 : 3;
    int cur = cfg_int("NX2SARIA_LOW_LATENCY");
    cur = cur < 0 ? 0 : cur >= n ? n - 1 : cur;
    if (st == VK_LOWLAT_NO_DRIVER) opt_disable_next(UI_MENU_LOW_LATENCY_NO_DRIVER);
    else if (st == VK_LOWLAT_DLSSG) opt_disable_next(UI_MENU_LOW_LATENCY_DLSSG);
    else if (st == VK_LOWLAT_FG) opt_disable_next(UI_MENU_LOW_LATENCY_FG);
    const char *note = st == VK_LOWLAT_FAILED && cur ? menu_text(UI_MENU_LOW_LATENCY_FAILED)
                     : st == VK_LOWLAT_RESTART && cur ? menu_text(UI_MENU_AFTER_RESTART) : nullptr;
    int nv = opt_choice_index("NX2SARIA_LOW_LATENCY", UI_MENU_LOW_LATENCY, names, n, cur, 0, UI_MENU_LOW_LATENCY_DESC, note);
    if (nv >= 0 && nv < n) opt_apply("NX2SARIA_LOW_LATENCY", values[nv]);
}

extern "C" int menu_lowlat_restart_for(const char *value) {
    return vk_lowlat_status() == VK_LOWLAT_RESTART && atoi(value) > 0;
}

static std::atomic<uint32_t> g_gpu_vendor;
static VkPhysicalDevice g_gpu_vendor_of;
static bool dlss_offered(void) {
    uint32_t v = g_gpu_vendor.load(std::memory_order_relaxed);
    return v == 0 || v == 0x10de;
}

static const char *fsr4_label(void) {
    static char s[32];
    const char *v = strstr(vk_ffx4_name(), "FSR ");
    int major = 0, minor = 0;
    if (v && sscanf(v + 4, "%d.%d", &major, &minor) == 2 && major >= 4) snprintf(s, sizeof s, "AMD FSR %d.%d", major, minor);
    else snprintf(s, sizeof s, "AMD FSR 4");
    return s;
}

static bool share_native(bool draft = true) {
    const char *s = draft ? opt_get("NX2SARIA_SCENE_SHARE") : cfg_get("NX2SARIA_SCENE_SHARE");
    return s && *s && atof(s) >= 1;
}

static const char *share_off(void) {
    const char *d = opt_default("NX2SARIA_SCENE_SHARE");
    const CfgMeta *m = cfg_meta("NX2SARIA_SCENE_SHARE");
    return d && atof(d) >= 1 && m ? m->def : "";
}

static const char *scene_note(void) {
    static char in_use[64];
    const double share = wide_scene_settable() ? wide_scene_share() : 0;
    if (share > 0 && wide_scene_restart()) return menu_text(UI_MENU_AFTER_RESTART);
    if (share > 0 && wide_scene_wanted() > share + 0.001) {
        char pct[16];
        snprintf(pct, sizeof pct, "%.0f %%", share * 100);
        snprintf(in_use, sizeof in_use, menu_text(UI_MENU_PRESENT_IN_USE), pct);
        return in_use;
    }
    return nullptr;
}

static void upscaling_name(char *out, size_t size) {
    static const char *const modes[] = {"dlaa", "quality", "balanced", "performance", "ultra_performance"};
    static const UiTextId dlss[] = {UI_MENU_DLSS_DLAA, UI_MENU_DLSS_QUALITY, UI_MENU_DLSS_BALANCED, UI_MENU_DLSS_PERFORMANCE, UI_MENU_DLSS_ULTRA};
    static const UiTextId fsr3[] = {UI_MENU_FSR3_NATIVE, UI_MENU_DLSS_QUALITY, UI_MENU_DLSS_BALANCED, UI_MENU_DLSS_PERFORMANCE, UI_MENU_DLSS_ULTRA};
    if (!cfg_bool("NX2SARIA_DLAA") || (!strcmp(cfg_value("NX2SARIA_TAA"), "fsr3") && !vk_ffx_available())) {
        if (share_native(false) && wide_scene_settable()) snprintf(out, size, "%s", menu_text(UI_MENU_UPSCALING_NATIVE));
        else if (strcmp(cfg_value("NX2SARIA_UPSCALER"), "fsr")) snprintf(out, size, "%s", menu_text(UI_MENU_UPSCALER_BILINEAR));
        else snprintf(out, size, menu_text(UI_MENU_OV_FMT_FSR), cfg_int("NX2SARIA_SHARPNESS"));
        return;
    }
    int mode = 0;
    for (int i = 0; i < 5; i++)
        if (!strcmp(cfg_value("NX2SARIA_DLSS"), modes[i])) mode = i;
    if (!strcmp(cfg_value("NX2SARIA_TAA"), "fsr3"))
        snprintf(out, size, "%s %s", vk_ffx_fsr4() ? fsr4_label() : "AMD FSR 3.1", menu_text(fsr3[mode]));
    else snprintf(out, size, mode ? "DLSS %s" : "NVIDIA %s", menu_text(dlss[mode]));
}

static void aa_name(char *out, size_t size) {
    static const char *const values[] = {"game", "off", "fxaa", "smaa"};
    static const UiTextId names[] = {UI_MENU_AA_GAME, UI_MENU_FRAMEGEN_OFF, UI_MENU_AA_FXAA, UI_MENU_AA_SMAA};
    int i = 0;
    for (int k = 0; k < 4; k++)
        if (!strcmp(cfg_value("NX2SARIA_AA"), values[k])) i = k;
    const bool taa = cfg_bool("NX2SARIA_DLAA") && (strcmp(cfg_value("NX2SARIA_TAA"), "fsr3") || vk_ffx_available());
    if (taa && i == 1) snprintf(out, size, "TAA");
    else if (taa) snprintf(out, size, "TAA + %s", menu_text(names[i]));
    else snprintf(out, size, "%s", menu_text(names[i]));
}

static bool upscaling_restart(void) { return dlss_offered() && vk_dlaa_needs_restart(); }

static void row_upscaling(void) {
    static const char *const modes[] = {"dlaa", "quality", "balanced", "performance", "ultra_performance"};
    static const UiTextId mode_names[] = {UI_MENU_DLSS_DLAA, UI_MENU_DLSS_QUALITY, UI_MENU_DLSS_BALANCED,
                                          UI_MENU_DLSS_PERFORMANCE, UI_MENU_DLSS_ULTRA};
    static const UiTextId fsr_names[] = {UI_MENU_FSR3_NATIVE, UI_MENU_DLSS_QUALITY, UI_MENU_DLSS_BALANCED,
                                         UI_MENU_DLSS_PERFORMANCE, UI_MENU_DLSS_ULTRA};
    enum { BILINEAR, FSR1, NATIVE, DLSS, FSR3, FSR4 };
    struct Entry { int kind, mode; };
    static char text[15][96];
    Entry list[18];
    const char *names[18];
    int n = 0;
    auto add = [&](int kind, int mode) {
        list[n] = {kind, mode};
        if (kind == BILINEAR) names[n] = menu_text(UI_MENU_UPSCALER_BILINEAR);
        else if (kind == FSR1) names[n] = menu_text(UI_MENU_UPSCALER_FSR);
        else if (kind == NATIVE) names[n] = menu_text(UI_MENU_UPSCALING_NATIVE);
        else {
            char *t = text[(kind == FSR4 ? 10 : kind == FSR3 ? 5 : 0) + mode];
            if (kind == DLSS) snprintf(t, sizeof text[0], mode ? "DLSS %s" : "NVIDIA %s", menu_text(mode_names[mode]));
            else snprintf(t, sizeof text[0], "%s %s", kind == FSR4 ? fsr4_label() : "AMD FSR 3.1", menu_text(fsr_names[mode]));
            names[n] = t;
        }
        n++;
    };
    Entry now = {share_native() ? NATIVE : strcmp(opt_value("NX2SARIA_UPSCALER"), "fsr") ? BILINEAR : FSR1, 0};
    if (opt_bool("NX2SARIA_DLAA")) {
        now = {strcmp(opt_value("NX2SARIA_TAA"), "fsr3") ? DLSS : vk_ffx_fsr4() ? FSR4 : FSR3, 0};
        for (int i = 0; i < 5; i++)
            if (!strcmp(opt_value("NX2SARIA_DLSS"), modes[i])) now.mode = i;
    }
    const bool dlss = dlss_offered(), fsr = vk_ffx_available(), fsr4 = fsr && vk_ffx4_available();
    add(BILINEAR, 0);
    add(FSR1, 0);
    if (wide_scene_settable() || now.kind == NATIVE) add(NATIVE, 0);
    for (int i = 0; dlss && i < 5; i++) add(DLSS, i);
    for (int i = 0; fsr && i < 5; i++) add(FSR3, i);
    for (int i = 0; fsr4 && i < 5; i++) add(FSR4, i);
    if (!dlss && now.kind == DLSS) add(DLSS, now.mode);
    if (!fsr && now.kind == FSR3) add(FSR3, now.mode);
    int cur = 0;
    for (int i = 0; i < n; i++)
        if (list[i].kind == now.kind && list[i].mode == now.mode) cur = i;
    const char *note = nullptr;
    if (!fsr && now.kind == FSR3) note = menu_text(UI_MENU_FSR3_OFF);
    else if (upscaling_restart()) note = menu_text(UI_MENU_AFTER_RESTART);
    else note = scene_note();
    int nv = opt_choice_index("NX2SARIA_UPSCALER", UI_MENU_UPSCALING, names, n, cur, 1,
                              !fsr ? UI_MENU_UPSCALING_DESC_NO_FSR : dlss ? UI_MENU_UPSCALING_DESC : UI_MENU_UPSCALING_DESC_FSR,
                              note);
    if (nv < 0 || nv >= n) return;
    const Entry e = list[nv];
    opt_apply("NX2SARIA_UPSCALER", e.kind == FSR1 ? "fsr" : "bilinear");
    opt_apply("NX2SARIA_DLAA", e.kind >= DLSS ? "1" : "0");
    if (e.kind == NATIVE) opt_apply("NX2SARIA_SCENE_SHARE", "1");
    else if (share_native() && e.kind < DLSS) opt_apply("NX2SARIA_SCENE_SHARE", share_off());
    else if (share_native() && now.kind == NATIVE) opt_apply("NX2SARIA_SCENE_SHARE", "");
    if (e.kind >= DLSS) opt_apply("NX2SARIA_DLSS", modes[e.mode]);
    if (e.kind >= DLSS) opt_apply("NX2SARIA_TAA", e.kind >= FSR3 ? "fsr3" : "dlss");
    if (e.kind >= FSR3) opt_apply("NX2SARIA_FSR4", e.kind == FSR4 ? "1" : "0");
    if (e.kind == FSR4) vk_ffx4_retry();
    if (e.kind < DLSS && !strcmp(opt_value("NX2SARIA_AA"), "taa")) opt_apply("NX2SARIA_AA", "game");
}

static const char *const scene_shares[] = {"0.33", "0.4", "0.5", "0.58", "0.67", "0.75", "1"};

static bool upscaler_sizes_scene(void) {
    return opt_bool("NX2SARIA_DLAA") && (strcmp(opt_value("NX2SARIA_TAA"), "fsr3") || vk_ffx_available());
}

static void row_scene_res(void) {
    if (!wide_scene_settable() || upscaler_sizes_scene()) return;
    const char *const *fixed = scene_shares;
    enum { N = sizeof scene_shares / sizeof *scene_shares };
    const CfgMeta *m = cfg_meta("NX2SARIA_SCENE_SHARE");
    const char *d = opt_default("NX2SARIA_SCENE_SHARE");
    const double dv = d && *d ? atof(d) : 0.75, game = m ? atof(m->def) : 0.75;
    static char text[N + 2][48], other[32];
    const char *values[N + 2], *names[N + 2];
    int n = 0, def = -1;
    auto pct = [](double v, char *out, size_t size) { snprintf(out, size, "%.0f %%", v * 100); };
    for (int i = 0; i <= N; i++) {
        const double v = i < N ? atof(fixed[i]) : 2;
        if (def < 0 && dv <= v + 0.001) {
            char p[16];
            pct(dv, p, sizeof p);
            snprintf(text[n], sizeof text[n], menu_text(fabs(dv - game) < 0.001 ? UI_MENU_SCENE_RES_GAME : UI_MENU_SCENE_RES_PRESET), p);
            values[n] = "", names[n] = text[n], def = n++;
            if (i < N && fabs(dv - v) < 0.001) continue;
        }
        if (i == N) break;
        pct(v, text[n], sizeof text[n]);
        if (v >= 1) snprintf(text[n], sizeof text[n], menu_text(UI_MENU_SCENE_RES_NATIVE), "100 %");
        else if (fabs(v - game) < 0.001) {
            char p[16];
            pct(v, p, sizeof p);
            snprintf(text[n], sizeof text[n], menu_text(UI_MENU_SCENE_RES_GAME), p);
        }
        values[n] = fixed[i], names[n] = text[n], n++;
    }
    int cur = def;
    if (opt_is_set("NX2SARIA_SCENE_SHARE")) {
        const double v = atof(opt_value("NX2SARIA_SCENE_SHARE"));
        cur = -1;
        for (int i = 0; i < n; i++)
            if (fabs((values[i][0] ? atof(values[i]) : dv) - v) < 0.001) cur = i;
        if (cur < 0) {
            snprintf(other, sizeof other, "%s", opt_value("NX2SARIA_SCENE_SHARE"));
            pct(v, text[n], sizeof text[n]);
            values[n] = other, names[n] = text[n], cur = n++;
        }
    }
    int nv = opt_choice_index("NX2SARIA_SCENE_SHARE", UI_MENU_SCENE_RES, names, n, cur, def, UI_MENU_SCENE_RES_DESC,
                              scene_note());
    if (nv >= 0 && nv < n) opt_apply("NX2SARIA_SCENE_SHARE", values[nv]);
}

static void row_dyn_res(void) {
    if (!wide_scene_settable()) return;
    const bool upscaled = upscaler_sizes_scene();
    const char *note = nullptr;
    if (opt_bool("NX2SARIA_DYN_RES")) {
        static const char *const modes[] = {"quality", "balanced", "performance", "ultra_performance"};
        const char *mode = opt_value("NX2SARIA_DLSS");
        bool steps = false;
        for (const char *m : modes) steps |= mode && !strcmp(mode, m);
        const char *d = opt_default("NX2SARIA_SCENE_SHARE");
        const double share = opt_is_set("NX2SARIA_SCENE_SHARE") ? atof(opt_value("NX2SARIA_SCENE_SHARE"))
                           : d && *d ? atof(d) : 0.75;
        if (upscaled && !steps) note = menu_text(UI_MENU_DYN_RES_NATIVE);
        else if (!upscaled && share > 0 && share <= atof(opt_value("NX2SARIA_DYN_RES_MIN")) + 0.001)
            note = menu_text(UI_MENU_DYN_RES_NO_ROOM);
    }
    opt_toggle("NX2SARIA_DYN_RES", UI_MENU_DYN_RES, UI_MENU_DYN_RES_DESC, false, note);
    if (upscaled) return;
    enum { N = sizeof scene_shares / sizeof *scene_shares };
    static char text[N][16];
    const char *names[N];
    const double now = atof(opt_value("NX2SARIA_DYN_RES_MIN"));
    int cur = -1;
    for (int i = 0; i < N; i++) {
        snprintf(text[i], sizeof text[i], "%.0f %%", atof(scene_shares[i]) * 100);
        names[i] = text[i];
        if (fabs(atof(scene_shares[i]) - now) < 0.001) cur = i;
    }
    if (!opt_bool("NX2SARIA_DYN_RES")) opt_disable_next(UI_MENU_DYN_RES_MIN_OFF);
    opt_choice_list("NX2SARIA_DYN_RES_MIN", UI_MENU_DYN_RES_MIN, scene_shares, names, N, UI_MENU_DYN_RES_MIN_DESC, cur);
}

static const char *preset_tier_text(int tier) {
    return menu_text((UiTextId)(UI_MENU_PRESET_LOW + (tier < 0 ? 2 : tier > 3 ? 3 : tier)));
}

static void preset_key_label(const char *name, char *out, size_t size) {
    static const struct { const char *key; UiTextId label; } rows[] = {
        {"SCENE_SHARE", UI_MENU_SCENE_RES}, {"GAME_RES", UI_MENU_GAME_RES},
        {"ANISO", UI_MENU_ANISO}, {"FPS_LIMIT", UI_MENU_FPS_LIMIT},
        {"DLAA", UI_MENU_UPSCALING}, {"TAA", UI_MENU_UPSCALING}, {"DLSS", UI_MENU_UPSCALING}, {"FSR4", UI_MENU_UPSCALING}};
    const char *key = name + sizeof "NX2SARIA_" - 1;
    char low[48];
    snprintf(low, sizeof low, "%s", key);
    for (char *c = low; *c; c++) *c = (char)tolower((unsigned char)*c);
    for (const auto &r : rows)
        if (!strcmp(r.key, key)) {
            if (r.label == UI_MENU_UPSCALING) snprintf(out, size, "%s (%s)", menu_text(r.label), low);
            else snprintf(out, size, "%s", menu_text(r.label));
            return;
        }
    snprintf(out, size, "%s", low);
}

static struct {
    bool ask;
    int choice;
    std::string from, to;
    std::vector<std::string> lines;
} g_preset;

static void preset_stage(int choice) {
    opt_apply("NX2SARIA_PRESET", choice ? cfg_preset_name(choice - 1) : "");
    char names[32][64];
    const int n = opt_preset_handback(choice ? choice - 1 : cfg_preset_recommended(nullptr), names, 32);
    for (int i = 0; i < n; i++) opt_apply(names[i], "");
    rt_log("menu: preset: %s chosen in the draft; %d key(s) set by hand handed back to it",
           choice ? cfg_preset_name(choice - 1) : "recommended", n);
}

static void row_preset(void) {
    const int rec = cfg_preset_recommended(nullptr);
    static char rec_name[96], custom[96];
    snprintf(rec_name, sizeof rec_name, menu_text(UI_MENU_PRESET_RECOMMENDED), preset_tier_text(rec));
    const char *names[5] = {rec_name, preset_tier_text(0), preset_tier_text(1), preset_tier_text(2), preset_tier_text(3)};
    const bool staged = opt_preset_tier() >= 0, moved = !staged && cfg_preset_hw_changed();
    const char *v = opt_get("NX2SARIA_PRESET");
    int cur = 0, file = 0;
    for (int t = 0; v && t < 4; t++)
        if (!strcasecmp(v, cfg_preset_name(t))) file = t + 1;
    if (!moved) cur = file;
    const int tier = cur ? cur - 1 : rec;
    int nk = 0, differ = 0;
    const char *const *keys = cfg_preset_keys(&nk);
    for (int i = 0; i < nk; i++) {
        char name[64];
        snprintf(name, sizeof name, "NX2SARIA_%s", keys[i]);
        if (opt_is_set(name)) differ += cfg_preset_differs(tier, name, opt_get(name)) != 0;
    }
    static char note[128];
    const char *shown_note = nullptr;
    if (moved) shown_note = menu_text(UI_MENU_PRESET_HW_NOTE);
    else if (differ) snprintf(note, sizeof note, menu_text(UI_MENU_PRESET_CUSTOM_NOTE), differ), shown_note = note;
    else if (cur && tier != rec) snprintf(note, sizeof note, menu_text(UI_MENU_PRESET_REC_NOTE), preset_tier_text(rec)), shown_note = note;
    if (differ) {
        snprintf(custom, sizeof custom, menu_text(UI_MENU_PRESET_CUSTOM), names[cur]);
        names[cur] = custom;
    }
    const int nv = opt_choice_index("NX2SARIA_PRESET", UI_MENU_PRESET, names, 5, cur, 0, UI_MENU_PRESET_DESC, shown_note,
                                    moved);
    if (nv < 0 || nv > 4) return;
    const char *from = moved && nv == 0 && file ? names[file] : names[cur];
    if (from != names[cur]) opt_context("NX2SARIA_PRESET", UI_MENU_PRESET, from, names[nv]);
    const int want = nv ? nv - 1 : rec;
    char handed[32][64];
    const int n = opt_preset_handback(want, handed, 32);
    if (!n) return preset_stage(nv);
    g_preset.lines.clear();
    for (int i = 0; i < n; i++) {
        char label[96], line[256];
        preset_key_label(handed[i], label, sizeof label);
        const char *to = cfg_default_tier(handed[i], want);
        snprintf(line, sizeof line, "%s: %s -> %s", label, opt_value(handed[i]), to && *to ? to : "-");
        g_preset.lines.push_back(line);
    }
    g_preset.ask = true, g_preset.choice = nv, g_preset.from = from, g_preset.to = names[nv];
}

static void row_aa(void) {
    enum { GAME, OFF, FXAA, SMAA, TAA };
    static const char *const values[] = {"game", "off", "fxaa", "smaa", "taa"};
    const bool dlss = dlss_offered(), fsr = vk_ffx_available(), fsr4 = fsr && vk_ffx4_available();
    const bool temporal = opt_bool("NX2SARIA_DLAA"), fsr_set = !strcmp(opt_value("NX2SARIA_TAA"), "fsr3");
    const char *names[5] = {menu_text(UI_MENU_AA_GAME), menu_text(UI_MENU_FRAMEGEN_OFF), menu_text(UI_MENU_AA_FXAA),
                            menu_text(UI_MENU_AA_SMAA), nullptr};
    int n = 4;
    enum { NONE, K_DLSS, K_FSR3, K_FSR4 };
    const int kind = dlss && (!fsr_set || !fsr) ? K_DLSS : fsr4 ? K_FSR4 : fsr ? K_FSR3 : NONE;
    static char taa[96];
    if (!temporal && kind) {
        if (kind == K_FSR4) snprintf(taa, sizeof taa, menu_text(UI_MENU_AA_TAA_FSR4), fsr4_label());
        else snprintf(taa, sizeof taa, "%s", menu_text(kind == K_DLSS ? UI_MENU_AA_TAA_DLSS : UI_MENU_AA_TAA_FSR));
        names[n++] = taa;
    }
    int cur = GAME;
    for (int i = 0; i < 4; i++)
        if (!strcmp(opt_value("NX2SARIA_AA"), values[i])) cur = i;
    const char *note = temporal && (!fsr_set || fsr) ? menu_text(UI_MENU_AA_WITH_TAA) : nullptr;
    int nv = opt_choice_index("NX2SARIA_AA", UI_MENU_AA, names, n, cur, GAME, UI_MENU_AA_DESC, note);
    if (nv < 0 || nv >= n || nv == cur) return;
    if (nv == TAA) {
        opt_apply("NX2SARIA_UPSCALER", "bilinear");
        opt_apply("NX2SARIA_DLAA", "1");
        opt_apply("NX2SARIA_DLSS", "dlaa");
        opt_apply("NX2SARIA_TAA", kind == K_DLSS ? "dlss" : "fsr3");
        if (kind != K_DLSS) opt_apply("NX2SARIA_FSR4", kind == K_FSR4 ? "1" : "0");
        if (kind == K_FSR4) vk_ffx4_retry();
    }
    opt_apply("NX2SARIA_AA", values[nv]);
}

static const char *hdr_note(void) {
    if (vk_hdr_restart_for(opt_value("NX2SARIA_HDR"))) return menu_text(UI_MENU_AFTER_RESTART);
    if (!vk_hdr_wanted() || vk_hdr_active() || !*gfx_window_present_mode() || !cfg_bool("NX2SARIA_HDR")) return nullptr;
    static char note[96];
    snprintf(note, sizeof note, menu_text(UI_MENU_PRESENT_IN_USE), menu_text(UI_MENU_FRAMEGEN_OFF));
    return note;
}

static void row_hdr(void) {
    static const char *const values[] = {"0", "1", "native"};
    const char *names[] = {menu_text(UI_MENU_FRAMEGEN_OFF), menu_text(UI_MENU_HDR_TONEMAPPED), menu_text(UI_MENU_HDR_NATIVE)};
    opt_choice_list("NX2SARIA_HDR", UI_MENU_HDR, values, names, 3, UI_MENU_HDR_DESC, -1, hdr_note());
    if (!opt_bool("NX2SARIA_HDR")) return;
    opt_slider("NX2SARIA_HDR_PEAK_NITS", UI_MENU_HDR_PEAK, 400, 10000, UI_MENU_HDR_PEAK_DESC, 50, "%.0f nits");
    opt_slider("NX2SARIA_HDR_GAME_NITS", UI_MENU_HDR_GAME, 48, 500, UI_MENU_HDR_GAME_DESC, 1, "%.0f nits");
    opt_slider("NX2SARIA_HDR_UI_NITS", UI_MENU_HDR_UI, 48, 500, UI_MENU_HDR_UI_DESC, 1, "%.0f nits");
    if (!strcmp(opt_value("NX2SARIA_HDR"), "native")) return;
    static const char *const curves[] = {"vanilla", "psychov35", "renodrt"};
    static const char *const curve_names[] = {"Vanilla", "PsychoV35", "RenoDRT"};
    opt_choice_list("NX2SARIA_HDR_TONEMAP", UI_MENU_HDR_TONEMAP, curves, curve_names, 3, UI_MENU_HDR_TONEMAP_DESC);
    static const char *const gammas[] = {"off", "2.2", "2.4"};
    const char *gamma_names[] = {menu_text(UI_MENU_FRAMEGEN_OFF), "2.2", "BT.1886 (2.4)"};
    opt_choice_list("NX2SARIA_HDR_GAMMA", UI_MENU_HDR_GAMMA, gammas, gamma_names, 3, UI_MENU_HDR_GAMMA_DESC);
    const char *curve = cfg_value("NX2SARIA_HDR_TONEMAP");
    if (!strcmp(curve, "vanilla")) return;
    const bool luminance = !strcmp(curve, "renodrt");
    if (!luminance) {
        static const char *const greys[] = {"render", "vanilla"};
        static const char *const grey_names[] = {"Render", "Vanilla"};
        opt_choice_list("NX2SARIA_HDR_MIDGRAY", UI_MENU_HDR_MIDGRAY, greys, grey_names, 2, UI_MENU_HDR_MIDGRAY_DESC);
        opt_toggle("NX2SARIA_HDR_MATCH_SLOPE", UI_MENU_HDR_MATCH_SLOPE, UI_MENU_HDR_MATCH_SLOPE_DESC);
        opt_slider("NX2SARIA_HDR_TONE_SLOPE", UI_MENU_HDR_TONE_SLOPE, 0, 100, UI_MENU_HDR_TONE_SLOPE_DESC, 1, "%.0f");
        opt_slider("NX2SARIA_HDR_HUE_CLIP", UI_MENU_HDR_HUE_CLIP, 0, 100, UI_MENU_HDR_HUE_CLIP_DESC, 5, "%.0f %%");
    }
    opt_slider("NX2SARIA_HDR_EXPOSURE", UI_MENU_HDR_EXPOSURE, 0, 200, UI_MENU_HDR_EXPOSURE_DESC, 5, "%.0f %%");
    opt_slider("NX2SARIA_HDR_HIGHLIGHTS", UI_MENU_HDR_HIGHLIGHTS, 0, 100, UI_MENU_HDR_HIGHLIGHTS_DESC, 1, "%.0f");
    opt_slider("NX2SARIA_HDR_SHADOWS", UI_MENU_HDR_SHADOWS, 0, 100, UI_MENU_HDR_SHADOWS_DESC, 1, "%.0f");
    opt_slider("NX2SARIA_HDR_CONTRAST", UI_MENU_HDR_CONTRAST, 0, 100, UI_MENU_HDR_CONTRAST_DESC, 1, "%.0f");
    opt_slider("NX2SARIA_HDR_SATURATION", UI_MENU_HDR_SATURATION, 0, 100, UI_MENU_HDR_SATURATION_DESC, 1, "%.0f");
    if (!luminance) return;
    opt_slider("NX2SARIA_HDR_HIGHLIGHT_SAT", UI_MENU_HDR_HIGHLIGHT_SAT, 0, 100, UI_MENU_HDR_HIGHLIGHT_SAT_DESC, 1, "%.0f");
    opt_slider("NX2SARIA_HDR_BLOWOUT", UI_MENU_HDR_BLOWOUT, 0, 100, UI_MENU_HDR_BLOWOUT_DESC, 1, "%.0f");
    opt_slider("NX2SARIA_HDR_FLARE", UI_MENU_HDR_FLARE, 0, 100, UI_MENU_HDR_FLARE_DESC, 1, "%.0f");
    opt_slider("NX2SARIA_HDR_HUE", UI_MENU_HDR_HUE, 0, 100, UI_MENU_HDR_HUE_DESC, 5, "%.0f %%");
    opt_slider("NX2SARIA_HDR_GRADE", UI_MENU_HDR_GRADE, 0, 100, UI_MENU_HDR_GRADE_DESC, 5, "%.0f %%");
}

static void row_framegen_mult(bool restart) {
    if (!restart && !vk_dlssg_active()) return;
    int old_dll = 0, max = restart ? 0 : vk_dlssg_multiplier_max(&old_dll);
    const char *note = nullptr;
    static char upto[64];
    if (max == 2) note = menu_text(old_dll ? UI_MENU_FRAMEGEN_MULT_OLD_DLL : UI_MENU_FRAMEGEN_MULT_RTX50);
    else if (max == 3) snprintf(upto, sizeof upto, menu_text(UI_MENU_FRAMEGEN_MULT_UPTO), max), note = upto;
    if (!max) max = 4;
    static const char *const values[] = {"2", "3", "4"};
    static const char *const names[] = {"2x", "3x", "4x"};
    int n = max - 1, cur = vk_dlssg_multiplier_of(opt_value("NX2SARIA_FRAMEGEN_MULTIPLIER"), max, nullptr) - 2;
    int nv = opt_choice_index("NX2SARIA_FRAMEGEN_MULTIPLIER", UI_MENU_FRAMEGEN_MULT, names, n, cur, 0,
                              UI_MENU_FRAMEGEN_MULT_DESC, note);
    if (nv >= 0 && nv < n) opt_apply("NX2SARIA_FRAMEGEN_MULTIPLIER", values[nv]);
}

static bool framegen_ngx(void) {
#ifdef _WIN32
    return false;
#else
    return true;
#endif
}

static void row_framegen_mult_ngx(void) {
    static const char *const mult[] = {"2", "3", "4", "5", "6"};
    static const char *const mult_names[] = {"2x", "3x", "4x", "5x", "6x"};
    const int max = vk_ngxfg_max();
    if (max < 1) return;
    const char *values[6], *names[6];
    int n = 0;
    for (; n < max && n < 5; n++) values[n] = mult[n], names[n] = mult_names[n];
    const int top = n - 1;
    values[n] = "dynamic", names[n++] = menu_text(UI_MENU_FG_MULT_DYNAMIC);
    const char *v = opt_value("NX2SARIA_FG_MULT");
    int cur = strcmp(v, "dynamic") ? top : n - 1;
    for (int i = 0; i < n; i++)
        if (!strcmp(v, values[i])) cur = i;
    int nv = opt_choice_index("NX2SARIA_FG_MULT", UI_MENU_FRAMEGEN_MULT, names, n, cur, 0, UI_MENU_FG_MULT_DESC, nullptr);
    if (nv >= 0 && nv < n) opt_apply("NX2SARIA_FG_MULT", values[nv]);
}

static bool framegen_restart(void) {
    const bool set = !strcmp(cfg_value("NX2SARIA_FRAMEGEN"), "dlss");
    if (!dlss_offered()) return false;
    return framegen_ngx() ? set && !vk_ngxfg_wanted() : set != (vk_dlssg_wanted() != 0);
}

static const char *fg4_label(void) {
    static char s[48];
    const char *v = vk_fg4_name();
    snprintf(s, sizeof s, *v ? "AMD FSR 4 library (%s)" : "AMD FSR 4 library", v);
    return s;
}
static bool fg4_chosen(void) {
    return cfg_bool("NX2SARIA_FSR4_FG") && cfg_bool("NX2SARIA_DLAA") && vk_ffx_fsr4() && vk_fg4_available() &&
           !vk_fg4_failed();
}

static void row_framegen(void) {
    const bool dlss = dlss_offered(), fsr = vk_ffx_framegen_available();
    const bool via_ngx = framegen_ngx() && dlss;
    const bool dlss_set = opt_bool("NX2SARIA_DLAA") && strcmp(opt_value("NX2SARIA_TAA"), "fsr3");
    const char *v = opt_value("NX2SARIA_FRAMEGEN");
    int cur = !strcmp(v, "fsr3") ? 1 : !strcmp(v, "dlss") ? 2 : 0;
#ifdef _WIN32
    const bool fsr_win_only = false;
#else
    const bool fsr_win_only = !dlss && !fsr && vk_ffx_available();
#endif
    const UiTextId desc = fsr_win_only ? UI_MENU_FRAMEGEN_DESC_FSR_WIN
                        : !dlss && !fsr ? UI_MENU_FRAMEGEN_DESC_NONE : via_ngx ? UI_MENU_FRAMEGEN_DESC_NGX
                                                                                : UI_MENU_FRAMEGEN_DESC_ANY;
    UiTextId note = fsr_win_only ? UI_TEXT_COUNT
                  : !dlss && !fsr ? UI_MENU_FRAMEGEN_NOTE : via_ngx ? UI_MENU_FRAMEGEN_NOTE_NGX : UI_TEXT_COUNT;
    if (!dlss && !fsr && cur == 0)
        opt_disable_next(fsr_win_only ? UI_MENU_FRAMEGEN_NEEDS_FSR_WIN : UI_MENU_FRAMEGEN_NEEDS_NONE);
    static bool told_fsr_win;
    if (fsr_win_only && !told_fsr_win) {
        told_fsr_win = true;
        rt_log("menu: frame generation: FSR 3.1 upscales here through the runtime's own passes, and its frame generation "
               "runs on Windows only (AMD's library): the row says so");
    }
    else if (via_ngx && !fsr && !vk_ngxfg_wanted() && !dlss_set) opt_disable_next(UI_MENU_FRAMEGEN_NEEDS_DLSS);
    enum { OFF, FSR3, FSR4LIB, DLSS };
    const bool four = cur == 1 && fg4_chosen();
    const bool four_offered = fsr && cfg_bool("NX2SARIA_DLAA") && vk_ffx4_available() && vk_fg4_available() &&
                              !vk_fg4_failed();
    const int now = cur == 2 ? DLSS : cur == 1 ? (four ? FSR4LIB : FSR3) : OFF;
    const char *all_names[] = {menu_text(UI_MENU_FRAMEGEN_OFF), "AMD FSR 3.1", fg4_label(), "NVIDIA DLSS"};
    int kinds[4];
    const char *names[4];
    int n = 0, at = 0;
    for (int i = 0; i < 4; i++) {
        if ((i == FSR3 && !fsr && cur != 1) || (i == FSR4LIB && !four_offered && !four) || (i == DLSS && !dlss && cur != 2)) continue;
        if (i == now) at = n;
        kinds[n] = i, names[n++] = all_names[i];
    }
    const bool ngx = framegen_ngx();
    const bool restart = framegen_restart();
    if (restart) note = UI_MENU_AFTER_RESTART;
    else if (cur == 1 && !fsr) note = fsr_win_only ? UI_MENU_FRAMEGEN_OFF_FSR_WIN
                                                   : UI_MENU_FSR3_OFF;
    const int st = cur != 2 || restart ? DLSSG_RUNS : ngx ? (vk_ngxfg_max() ? DLSSG_RUNS : DLSSG_NO_FILES) : vk_dlssg_status();
    if (st == DLSSG_UNAVAILABLE) note = UI_MENU_FRAMEGEN_UNAVAILABLE;
    else if (st == DLSSG_NO_FILES) note = UI_MENU_FRAMEGEN_OFF_FILES;
#ifndef _WIN32
    if (cur == 1 && fsr && !restart && note == UI_TEXT_COUNT) {
        const char *hdr = opt_value("NX2SARIA_HDR");
        const bool hdr_on = !strcmp(hdr, "native") || opt_bool("NX2SARIA_HDR");
        note = hdr_on ? UI_MENU_FRAMEGEN_OFF_HDR : UI_MENU_FRAMEGEN_FSR3_NATIVE;
        static int told = -1;
        if (told != (int)hdr_on) {
            told = hdr_on;
            rt_log("menu: frame generation: AMD FSR 3.1 through the runtime's passes, %s: the row says so",
                   hdr_on ? "off while HDR is on (SDR only)" : "2x");
        }
    }
#endif
    int nv = opt_choice_index("NX2SARIA_FRAMEGEN", UI_MENU_FRAMEGEN, names, n, at, 0, desc,
                              (four && !restart) || note == UI_TEXT_COUNT ? NULL : menu_text(note));
    if (nv >= 0 && nv < n) {
        const int k = kinds[nv];
        opt_apply("NX2SARIA_FRAMEGEN", k == OFF ? "off" : k == DLSS ? "dlss" : "fsr3");
        if (k == FSR4LIB) {
            opt_apply("NX2SARIA_FSR4_FG", "1");
            if (!vk_ffx_fsr4()) opt_apply("NX2SARIA_TAA", "fsr3"), opt_apply("NX2SARIA_FSR4", "1"), vk_ffx4_retry();
        } else if (k == FSR3 && cfg_bool("NX2SARIA_FSR4_FG")) {
            opt_apply("NX2SARIA_FSR4_FG", "0");
        }
    }
    else if (cur == 2 && dlss && ngx) row_framegen_mult_ngx();
    else if (cur == 2 && dlss) row_framegen_mult(restart);
}

static void row_resolution(void) {
    static const int heights[] = {1080, 1440, 1800, 2160, 2880};
    static const int wide169[] = {1920, 2560, 3200, 3840, 5120}, wide1610[] = {1728, 2304, 2880, 3456, 4608},
                     wide32[] = {1620, 2160, 2700, 3240, 4320}, wide219[] = {2560, 3440, 4300, 5120, 6880},
                     wide329[] = {3840, 5120, 6400, 7680, 10240};
    enum { N = sizeof heights / sizeof *heights };
    const char *aspect = opt_value("NX2SARIA_GAME_ASPECT");
    int mw = 0, mh = 0, ww = 0, wh = 0;
    int have_mon = gfx_window_monitor_size(&mw, &mh) && mw > 0 && mh > 0;
    int have_win = sscanf(opt_value("NX2SARIA_WINDOW_SIZE"), "%dx%d", &ww, &wh) == 2 && ww > 0 && wh > 0;
    double shape = opt_bool("NX2SARIA_FULLSCREEN") && have_mon ? (double)mw / mh : have_win ? (double)ww / wh : 16.0 / 9.0;
    static char sizes[N][24], names[N + 1][48];
    const char *pn[N + 1];
    for (int i = 0; i < N; i++) {
        int w = !strcmp(aspect, "16:10") ? wide1610[i] : !strcmp(aspect, "3:2") ? wide32[i] :
                !strcmp(aspect, "21:9") ? wide219[i] : !strcmp(aspect, "32:9") ? wide329[i] :
                !strcmp(aspect, "window") ? ((int)(heights[i] * shape + 0.5) & ~1) : wide169[i];
        int h = heights[i], fw = w, fh = h;
        if (have_mon && (fw > mw || fh > mh)) {
            double k = (double)mw / fw < (double)mh / fh ? (double)mw / fw : (double)mh / fh;
            fw = (int)(fw * k) & ~1, fh = (int)(fh * k) & ~1;
        }
        snprintf(sizes[i], sizeof sizes[i], "%dx%d", fw, fh);
        snprintf(names[i], sizeof names[i], "%d × %d", w, h);
        pn[i] = names[i];
    }
    pn[N] = menu_text(UI_MENU_GAME_RES_NATIVE);
    const char *res = opt_value("NX2SARIA_GAME_RES");
    int cur = -1;
    for (int i = 0; i < N; i++)
        if (atoi(res) == heights[i]) cur = i;
    if (!strcmp(res, "window")) cur = N;
    const char *dres = opt_default("NX2SARIA_GAME_RES");
    int def = -1;
    for (int i = 0; dres && i < N; i++)
        if (atoi(dres) == heights[i]) def = i;
    if (dres && !strcmp(dres, "window")) def = N;
    int nv = opt_choice_index("NX2SARIA_GAME_RES", UI_MENU_GAME_RES, pn, N + 1, cur, def, UI_MENU_GAME_RES_DESC);
    if (nv < 0 || nv > N) return;
    if (nv == N) {
        opt_apply("NX2SARIA_GAME_RES", "window");
        return;
    }
    char h[16];
    snprintf(h, sizeof h, "%d", heights[nv]);
    opt_apply("NX2SARIA_GAME_RES", h);
    opt_apply("NX2SARIA_WINDOW_SIZE", sizes[nv]);
}

static void row_wide_placement(void) {
    const char *aspect = opt_value("NX2SARIA_GAME_ASPECT");
    if (!strcmp(aspect, "16:9") || !strcmp(aspect, "16:10") || !strcmp(aspect, "3:2")) return;
    static const UiTextId hud[] = {UI_MENU_WIDE_HUD_CENTER, UI_MENU_WIDE_HUD_EDGES};
    static const UiTextId menus[] = {UI_MENU_WIDE_MENUS_FULL, UI_MENU_WIDE_MENUS_CENTER};
    opt_choice("NX2SARIA_WIDE_HUD", UI_MENU_WIDE_HUD, hud, 2, UI_MENU_WIDE_HUD_DESC);
    opt_choice("NX2SARIA_WIDE_MENUS", UI_MENU_WIDE_MENUS, menus, 2, UI_MENU_WIDE_MENUS_DESC);
}

static void tab_screen(float scale) {
    opt_begin("##screen", scale);
    opt_toggle("NX2SARIA_FULLSCREEN", UI_MENU_FULLSCREEN, UI_MENU_FULLSCREEN_DESC);
    if (!wide_available()) row_window_size();
    opt_toggle_value("NX2SARIA_VK_PRESENT", UI_MENU_PRESENT_VSYNC, UI_MENU_PRESENT_DESC, "mailbox", "immediate", vsync_note());
    row_low_latency();
    row_gpu();
    static const UiTextId perf[] = {UI_MENU_PERF_OFF, UI_MENU_PERF_FPS, UI_MENU_PERF_FULL};
    opt_choice("NX2SARIA_PERF_OVERLAY", UI_MENU_PERF_OVERLAY, perf, 3, UI_MENU_PERF_OVERLAY_DESC);
    opt_group(UI_MENU_GRAPHICS);
    row_preset();
    opt_toggle("NX2SARIA_TEX_CLOBBER", UI_MENU_TEX_CLOBBER, UI_MENU_TEX_CLOBBER_DESC, false,
               menu_text(UI_MENU_TEX_CLOBBER_NOTE));
    if (res_adjustable()) row_output_res();
    static const UiTextId wide[] = {UI_MENU_GAME_ASPECT_16_9, UI_MENU_GAME_ASPECT_WINDOW, UI_MENU_GAME_ASPECT_16_10,
                                    UI_MENU_GAME_ASPECT_3_2, UI_MENU_GAME_ASPECT_21_9, UI_MENU_GAME_ASPECT_32_9};
    if (wide_available()) opt_choice("NX2SARIA_GAME_ASPECT", UI_MENU_GAME_ASPECT, wide, 6, UI_MENU_GAME_ASPECT_DESC);
    if (wide_available()) row_wide_placement();
    if (wide_available()) row_resolution();
    opt_slider("NX2SARIA_FOV", UI_MENU_FOV, 70, 140, UI_MENU_FOV_DESC, 5, "%.0f %%");
    row_upscaling();
    row_scene_res();
    row_dyn_res();
    row_aa();
    row_framegen();
    row_hdr();
    int dl = opt_bool("NX2SARIA_DLAA"), fsr3 = dl && !strcmp(opt_value("NX2SARIA_TAA"), "fsr3") && vk_ffx_available();
    if (!fsr3 && (strcmp(opt_value("NX2SARIA_UPSCALER"), "fsr") || dl)) opt_disable_next(UI_MENU_SHARPNESS_NEEDS_FSR);
    opt_slider("NX2SARIA_SHARPNESS", UI_MENU_SHARPNESS, 0, 100, UI_MENU_SHARPNESS_DESC, 5, "%.0f %%");
    static const char *const aniso[] = {"game", "2", "4", "8", "16"};
    const char *aniso_names[] = {menu_text(UI_MENU_ANISO_GAME), "2x", "4x", "8x", "16x"};
    opt_choice_list("NX2SARIA_ANISO", UI_MENU_ANISO, aniso, aniso_names, 5, UI_MENU_ANISO_DESC);
    row_texture_detail();
    opt_end();
}

static PlatMutex g_share_lock = PLAT_MUTEX_INIT;
static std::atomic<int> g_share_busy{0};
static int g_share_text = -1;
static char g_share_arg[PLAT_PATH_MAX + 64];
static bool g_share_warn;

static void share_done(UiTextId text, const char *arg, bool warn) {
    plat_mutex_lock(&g_share_lock);
    g_share_text = text;
    snprintf(g_share_arg, sizeof g_share_arg, "%s", arg ? arg : "");
    g_share_warn = warn;
    plat_mutex_unlock(&g_share_lock);
    g_share_busy.store(0);
}

static void share_export_main(void *arg) {
    const int bug_report = (int)(intptr_t)arg;
    char dir[PLAT_PATH_MAX], path[PLAT_PATH_MAX + 64];
    SaveShareResult r = save_share_default_dir(dir, sizeof dir) ? save_share_export(dir, bug_report, path, sizeof path)
                                                                : SAVE_SHARE_WRITE_FAILED;
    if (r == SAVE_SHARE_OK) share_done(UI_MENU_SAVE_EXPORTED, path, false);
    else share_done(r == SAVE_SHARE_NO_SAVE ? UI_MENU_SAVE_E_NO_SAVE : r == SAVE_SHARE_READ_FAILED ? UI_MENU_SAVE_E_READ_SAVE
                                                                                                   : UI_MENU_SAVE_E_WRITE,
                    dir, true);
}

static void share_zip_picked(const char *path, void *ctx) {
    (void)ctx;
    if (!path) {
        g_share_busy.store(0);
        return;
    }
    char detail[160];
    SaveShareResult r = save_share_import(path, detail, sizeof detail);
    UiTextId text;
    switch (r) {
    case SAVE_SHARE_OK: text = UI_MENU_SAVE_IMPORT_READY; break;
    case SAVE_SHARE_READ_FAILED: text = UI_MENU_SAVE_E_READ; break;
    case SAVE_SHARE_WRITE_FAILED: text = UI_MENU_SAVE_E_STAGE; break;
    case SAVE_SHARE_OTHER_GAME: text = UI_MENU_SAVE_E_OTHER_GAME; break;
    case SAVE_SHARE_NEWER: text = UI_MENU_SAVE_E_NEWER; break;
    case SAVE_SHARE_DAMAGED: text = UI_MENU_SAVE_E_DAMAGED; break;
    case SAVE_SHARE_UNSAFE: text = UI_MENU_SAVE_E_UNSAFE; break;
    default: text = UI_MENU_SAVE_E_NOT_SAVE; break;
    }
    share_done(text, r == SAVE_SHARE_OK ? detail : path, r != SAVE_SHARE_OK);
}

static void rows_saves(void) {
    opt_group(UI_MENU_SAVES);
    const bool busy = g_share_busy.load() != 0;
    int start = 0;
    if (busy) opt_disable_next(UI_MENU_SAVE_BUSY);
    if (opt_button("##save_export", UI_MENU_SAVE_EXPORT, UI_MENU_SAVE_EXPORT_DESC)) start = 1;
    if (busy) opt_disable_next(UI_MENU_SAVE_BUSY);
    if (opt_button("##save_export_bug", UI_MENU_SAVE_EXPORT_BUG, UI_MENU_SAVE_EXPORT_BUG_DESC)) start = 2;
    const bool waiting = save_share_pending() != 0;
    if (busy) opt_disable_next(UI_MENU_SAVE_BUSY);
    if (opt_button("##save_import", UI_MENU_SAVE_IMPORT, UI_MENU_SAVE_IMPORT_DESC,
                   waiting ? menu_text(UI_MENU_SAVE_IMPORT_WAITING) : ""))
        start = 3;
    if (waiting) {
        if (busy) opt_disable_next(UI_MENU_SAVE_BUSY);
        if (opt_button("##save_import_cancel", UI_MENU_SAVE_IMPORT_CANCEL, UI_MENU_SAVE_IMPORT_CANCEL_DESC)) {
            if (save_share_cancel()) share_done(UI_MENU_SAVE_E_STAGE, "", true);
            else share_done(UI_MENU_SAVE_IMPORT_CANCELLED, "", false);
        }
    }
    if (start && !busy) {
        g_share_busy.store(1);
        if (start == 3) {
            char dir[PLAT_PATH_MAX];
            file_pick(save_share_default_dir(dir, sizeof dir) ? dir : "", "zip", share_zip_picked, NULL);
        } else if (!plat_thread_spawn(share_export_main, (void *)(intptr_t)(start == 2), "save export", 0)) {
            share_done(UI_MENU_SAVE_E_READ_SAVE, "", true);
        }
    }
    char line[PLAT_PATH_MAX * 2] = "";
    plat_mutex_lock(&g_share_lock);
    bool warn = g_share_warn;
    if (g_share_text >= 0) snprintf(line, sizeof line, menu_text((UiTextId)g_share_text), g_share_arg);
    plat_mutex_unlock(&g_share_lock);
    const char *done = fs_save_import_done();
    if (!line[0] && *done) {
        if (!strcmp(done, "-")) snprintf(line, sizeof line, "%s", menu_text(UI_MENU_SAVE_IMPORTED_NEW));
        else snprintf(line, sizeof line, menu_text(UI_MENU_SAVE_IMPORTED), done);
        warn = false;
    }
    if (line[0]) opt_text_str(line, warn);
}

static void rows_game(void) {
    row_game_lang();
    static const UiTextId mode[] = {UI_MENU_MODE_DOCKED, UI_MENU_MODE_HANDHELD};
    opt_choice("NX2SARIA_OPERATION_MODE", UI_MENU_OPERATION_MODE, mode, 2, UI_MENU_OPERATION_MODE_DESC);
    opt_toggle("NX2SARIA_ASSUME_FOCUS", UI_MENU_ASSUME_FOCUS, UI_MENU_ASSUME_FOCUS_DESC);
    if (text_speed_available()) {
        static const UiTextId speed[] = {UI_MENU_TEXT_SPEED_NORMAL, UI_MENU_TEXT_SPEED_FAST, UI_MENU_TEXT_SPEED_INSTANT};
        opt_choice("NX2SARIA_TEXT_SPEED", UI_MENU_TEXT_SPEED, speed, 3, UI_MENU_TEXT_SPEED_DESC);
    }
    if (amiibo_daily_available())
        opt_toggle("NX2SARIA_AMIIBO_DAILY_LIMIT", UI_MENU_AMIIBO_DAILY_LIMIT, UI_MENU_AMIIBO_DAILY_LIMIT_DESC);
    opt_toggle("NX2SARIA_DISCORD_RPC", UI_MENU_DISCORD_RPC, UI_MENU_DISCORD_RPC_DESC);
}

static void tab_game(float scale) {
    opt_begin("##game", scale);
    row_fps_limit();
    rows_game();
    rows_saves();
    opt_group_text("Testing");
    {
        static const char *const vals[] = {"auto", "0", "1", "2", "3", "4", "5", "6", "7", "8"};
        static char auto_name[64];
        snprintf(auto_name, sizeof auto_name, "%s (%d)", menu_text(UI_MENU_AUTO), vk_exec_replay_workers_auto(nullptr));
        const char *names[] = {auto_name, "0", "1", "2", "3", "4", "5", "6", "7", "8"};
        opt_choice_list("NX2SARIA_REPLAY_WORKERS", UI_MENU_REPLAY_WORKERS, vals, names, 10, UI_MENU_REPLAY_WORKERS_DESC);
    }
    const char *wset = opt_value("NX2SARIA_REPLAY_WORKERS");
    const bool workers = (!strcmp(wset, "auto") ? vk_exec_replay_workers_auto(nullptr) : atoi(wset)) > 0,
               pipe = workers && opt_int("NX2SARIA_REPLAY_PIPE") > 0;
    if (!workers) opt_disable_next(UI_MENU_NEEDS_WORKERS);
    opt_slider("NX2SARIA_REPLAY_PIPE", UI_MENU_REPLAY_PIPE, 0, 4, UI_MENU_REPLAY_PIPE_DESC, 1, "%.0f");
    if (!pipe) opt_disable_next(workers ? UI_MENU_NEEDS_PIPE : UI_MENU_NEEDS_WORKERS);
    opt_slider("NX2SARIA_REPLAY_SMALL", UI_MENU_REPLAY_SMALL, 0, 60, UI_MENU_REPLAY_SMALL_DESC, 4, "%.0f");
    opt_toggle("NX2SARIA_TAIL_EARLY", UI_MENU_TAIL_EARLY, UI_MENU_TAIL_EARLY_DESC);
    if (dbg_supported()) {
        const bool on = cfg_bool("NX2SARIA_DEBUG_MENU");
        if (opt_toggle_text("debug_menu", "Debug menu (F9)",
                            "F9 opens a window for testing: the game's flags by name, a teleport to every area's start "
                            "positions, its cutscenes to start, a free camera, the game's speed, and a search of its "
                            "memory with values to set and hold. All of these can break a save, and the game saves by itself after a change "
                            "of area: keep a copy of the save first.", on))
            opt_apply("NX2SARIA_DEBUG_MENU", on ? "0" : "1");
    }
    opt_end();
}

static void rows_sound(void) {
    opt_slider("NX2SARIA_VOLUME", UI_MENU_VOLUME, 0, 100, UI_MENU_VOLUME_DESC, 5, "%.0f %%");
    opt_slider("NX2SARIA_MENU_SOUND_VOLUME", UI_MENU_MENU_SOUNDS, 0, 100, UI_MENU_MENU_SOUNDS_DESC, 5, "%.0f %%");
    static const UiTextId channels[] = {UI_MENU_CHANNELS_AUTO, UI_MENU_CHANNELS_STEREO, UI_MENU_CHANNELS_51};
    opt_choice("NX2SARIA_AUDIO_CHANNELS", UI_MENU_AUDIO_CHANNELS, channels, 3, UI_MENU_AUDIO_CHANNELS_DESC);
}

static void tab_sound(float scale) {
    opt_begin("##sound", scale);
    rows_sound();
    opt_end();
}

static bool g_modal;

static const char *pad_layout_note(void) {
    int got = input_pad_layout();
    if (got < 0 || strcmp(opt_value("NX2SARIA_PAD_LAYOUT"), "auto")) return nullptr;
    static char note[96];
    UiTextId name = got != PAD_LAYOUT_XBOX                     ? UI_MENU_PAD_LAYOUT_NINTENDO
                    : input_pad_label(PAD_SOUTH) == PAD_LABEL_CROSS ? UI_MENU_PAD_LAYOUT_PLAYSTATION
                                                                  : UI_MENU_PAD_LAYOUT_XBOX;
    snprintf(note, sizeof note, menu_text(UI_MENU_PAD_LAYOUT_IN_USE), menu_text(name));
    return note;
}

static int key_action(const InputMap *m, int key) {
    for (int a = 0; a < ACTION_COUNT; a++) {
        uint8_t keys[INPUT_BINDING_MAX];
        int n = input_map_keys_of(m, a, keys, INPUT_BINDING_MAX);
        for (int i = 0; i < n; i++)
            if (keys[i] == key) return a;
    }
    return -1;
}

static void row_walk_key(void) {
    static const char *const keys[] = {"none", "lshift", "lalt", "lctrl", "tab", "capslock"};
    const char *names[sizeof keys / sizeof *keys] = {menu_text(UI_MENU_OFF), "Shift", "Alt", "Ctrl", "Tab", "Caps Lock"};
    const char *cur = opt_value("NX2SARIA_WALK_KEY");
    static char note[64];
    const char *n = nullptr;
    InputMap m;
    input_map_snapshot(&m);
    for (int k = KEY_A; k < KEY_COUNT && !n; k++) {
        if (!input_key_name(k) || strcasecmp(input_key_name(k), cur)) continue;
        int a = key_action(&m, k);
        if (a >= 0 && input_action_name(a)) snprintf(note, sizeof note, menu_text(UI_MENU_WALK_KEY_TAKEN), input_action_name(a)), n = note;
    }
    opt_choice_list("NX2SARIA_WALK_KEY", UI_MENU_WALK_KEY, keys, names, sizeof keys / sizeof *keys, UI_MENU_WALK_KEY_DESC,
                    -1, n);
}

static const char *prompts_note(bool *restart) {
    const char *layout = opt_value("NX2SARIA_PAD_LAYOUT"), *used = romfs_prompts();
    const char *want = input_prompts_folder(input_prompts_pick(opt_get("NX2SARIA_PROMPTS"), layout), layout);
    int diamond = romfs_action_prompt();
    *restart = diamond >= 0 && diamond != input_pad_layout_pick(layout, 0);
    if (want == used || (want && used && !strcmp(want, used))) return *restart ? menu_text(UI_MENU_PROMPTS_RESTART) : nullptr;
    int off = romfs_prompts_off(want);
    if (want && !romfs_prompts_ready(want) && off != ROMFS_PROMPTS_SHAPE) return menu_text(UI_MENU_PROMPTS_MISSING);
    switch (off) {
    case ROMFS_PROMPTS_FAILED: return menu_text(UI_MENU_PROMPTS_FAILED);
    case ROMFS_PROMPTS_SHAPE: return menu_text(UI_MENU_PROMPTS_OFF_SHAPE);
    case ROMFS_PROMPTS_MODS: return menu_text(UI_MENU_PROMPTS_OFF_MODS);
    case ROMFS_PROMPTS_PATCHES: return menu_text(UI_MENU_PROMPTS_OFF_PATCHES);
    default: *restart = true; return menu_text(UI_MENU_PROMPTS_RESTART);
    }
}

static int restart_pending(void) {
    bool prompts;
    prompts_note(&prompts);
    return opt_pending_count() + upscaling_restart() + framegen_restart()
           + (vk_hdr_restart_for(opt_value("NX2SARIA_HDR")) != 0)
           + (wide_scene_settable() && wide_scene_share() > 0 && wide_scene_restart() && !upscaling_restart())
           + (prompts && !cfg_pending_for("NX2SARIA_PROMPTS", opt_value("NX2SARIA_PROMPTS")));
}

static bool g_confirm_quit;
static void restart_confirm(float scale) {
    g_modal = false;
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 0);
    const bool open = ImGui::BeginPopupModal("##confirm", nullptr,
                                             ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::PopStyleVar();
    if (!open) return;
    g_modal = true;
    const ImVec2 box = ImGui::GetWindowPos(), box_size = ImGui::GetWindowSize();
    skin_panel(ImGui::GetWindowDrawList(), box, ImVec2(box.x + box_size.x, box.y + box_size.y), scale);
    ImGui::TextUnformatted(menu_text(g_confirm_quit ? UI_MENU_QUIT_CONFIRM : UI_MENU_RESTART_CONFIRM));
    if (opt_draft_count() > 0)
        ImGui::TextColored(ImVec4(0.95f, 0.66f, 0.40f, 1), "%s",
                           menu_text(g_confirm_quit ? UI_MENU_DRAFT_QUIT_NOTE : UI_MENU_DRAFT_RESTART_NOTE));
    bool saving = fs_save_busy() != 0;
    if (saving || (!g_confirm_quit && !cfg_last_save_ok()))
        ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.35f, 1), "%s",
                           menu_text(!saving ? UI_MENU_SAVE_FAILED : g_confirm_quit ? UI_MENU_QUIT_SAVING
                                                                                    : UI_MENU_RESTART_SAVING));
    ImGui::Spacing();
    const char *yes = menu_text(g_confirm_quit ? UI_MENU_QUIT : UI_MENU_RESTART_YES), *no = menu_text(UI_MENU_RESET_NO);
    const ImGuiStyle &st = ImGui::GetStyle();
    float bw = fmaxf(ImGui::CalcTextSize(yes).x, ImGui::CalcTextSize(no).x) + st.FramePadding.x * 4;
    bool close = skin_widget_button(no, ImVec2(bw, 0), scale);
    if (ImGui::IsWindowAppearing()) ImGui::SetItemDefaultFocus();
    ImGui::SameLine(0, st.ItemSpacing.x + skin_cursor_reach(scale));
    ImGui::BeginDisabled(saving);
    if (skin_widget_button(yes, ImVec2(bw, 0), scale)) {
        if (g_confirm_quit) {
            if (opt_draft_any()) rt_log("menu: Quit game with a draft: the %d change(s) are dropped", opt_draft_any());
            gfx_window_request_close();
        } else {
            if (opt_draft_any()) rt_log("menu: Restart with a draft: applying it first");
            if (opt_draft_apply()) gfx_window_request_restart();
        }
    }
    ImGui::EndDisabled();
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false))
        close = true, menu_sound_play(MENU_SOUND_CANCEL);
    if (close) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

static bool g_apply_ask;

enum { POST_NONE, POST_KEEP, POST_RESTART };
static int g_post = POST_NONE;
static bool g_post_open;
static int g_post_frame;
static uint64_t g_keep_deadline_ns;
struct RestartLine {
    std::string text;
    bool display;
};
static std::vector<RestartLine> g_restart_lines;
static const uint64_t KEEP_NS = 15ull * 1000000000ull;

static void keep_reverted(void) {
    for (size_t i = g_restart_lines.size(); i-- > 0;)
        if (g_restart_lines[i].display) g_restart_lines.erase(g_restart_lines.begin() + (std::ptrdiff_t)i);
}

static void leave_menu(void) {
    if (g_modal || g_post != POST_NONE) return;
    if (opt_draft_count() > 0) {
        g_apply_ask = true;
        return;
    }
    opt_draft_discard();
    set_open(0);
}

static void post_queue(int stage) {
    g_post = stage;
    g_post_open = false;
    g_post_frame = GImGui->FrameCount + 1;
}

static void post_next(bool from_keep) {
    if (!from_keep && opt_display_pending()) {
        gfx_window_apply_settings();
        g_keep_deadline_ns = plat_now_ns() + KEEP_NS;
        post_queue(POST_KEEP);
    } else if (!g_restart_lines.empty() && cfg_last_save_ok()) {
        post_queue(POST_RESTART);
    } else {
        g_post = POST_NONE;
        g_restart_lines.clear();
        set_open(0);
    }
}

static struct {
    int kind;
    char text[640];
} g_fit;

static VramLoad vram_load(const char *(*get)(const char *), unsigned window_h) {
    VramLoad l = {0, VRAM_MODE_OFF, 0};
    if (wide_available()) {
        const char *res = get("NX2SARIA_GAME_RES");
        l.res_h = !strcmp(res, "window") ? window_h : (unsigned)atoi(res);
    }
    const bool fsr = !strcmp(get("NX2SARIA_TAA"), "fsr3");
    l.mode = vram_fit_mode(atoi(get("NX2SARIA_DLAA")) != 0 && (!fsr || vk_ffx_available()), get("NX2SARIA_DLSS"));
    const char *fg = get("NX2SARIA_FRAMEGEN");
    l.framegen = !strcmp(fg, "dlss") || (!strcmp(fg, "fsr3") && vk_ffx_framegen_available());
    return l;
}

static void vram_fit_make(void) {
    static const char *const mode_log[] = {"off", "ultra performance", "performance", "balanced", "quality", "full size"};
    static const UiTextId mode_text[] = {UI_MENU_FRAMEGEN_OFF, UI_MENU_DLSS_ULTRA, UI_MENU_DLSS_PERFORMANCE,
                                         UI_MENU_DLSS_BALANCED, UI_MENU_DLSS_QUALITY, UI_MENU_DLSS_DLAA};
    static const PresetHw unknown = {};
    const unsigned window_h = (unsigned)ImGui::GetIO().DisplaySize.y;
    const VramLoad now = vram_load(opt_before, window_h), draft = vram_load(opt_value, window_h);
    const PresetHw *hw = cfg_preset_hw();
    if (!hw) hw = &unknown;
    uint64_t used = 0, limit = 0;
    const bool have = mem_budget_vram_mb(&used, &limit) != 0;
    const int level = have ? mem_budget_level(MEM_VRAM) : MEM_NORMAL, noticed = have ? vram_warn_shown_count() : 0;
    unsigned what = 0, preset_res = 0;
    int preset_mode = VRAM_MODE_OFF;
    g_fit.kind = vram_fit_note(level, noticed, &now, &draft, hw, &what, &preset_res, &preset_mode);
    g_fit.text[0] = 0;
    char items[200] = "", said[240] = "", part[120];
    auto add = [](char *to, size_t cap, const char *piece) {
        const size_t k = strlen(to);
        snprintf(to + k, cap - k, "%s%s", k ? ", " : "", piece);
    };
    if (what & VRAM_SUGGEST_RES) {
        snprintf(part, sizeof part, "resolution %u -> %u", now.res_h, draft.res_h);
        add(said, sizeof said, part);
        snprintf(part, sizeof part, "%s", menu_text(UI_MENU_GAME_RES));
        if (g_fit.kind == VRAM_FIT_PRESET) snprintf(part, sizeof part, "%s %u", menu_text(UI_MENU_GAME_RES), preset_res);
        add(items, sizeof items, part);
    }
    if (what & VRAM_SUGGEST_MODE) {
        snprintf(part, sizeof part, "upscaling mode %s -> %s", mode_log[now.mode], mode_log[draft.mode]);
        add(said, sizeof said, part);
        snprintf(part, sizeof part, "%s", menu_text(UI_MENU_UPSCALING));
        if (g_fit.kind == VRAM_FIT_PRESET)
            snprintf(part, sizeof part, "%s %s", menu_text(UI_MENU_UPSCALING), menu_text(mode_text[preset_mode]));
        add(items, sizeof items, part);
    }
    if (what & VRAM_SUGGEST_FRAMEGEN) {
        add(said, sizeof said, "frame generation off -> on");
        add(items, sizeof items, menu_text(UI_MENU_FRAMEGEN));
    }
    const int pct = limit ? (int)(used * 100 / limit) : 0;
    if (g_fit.kind == VRAM_FIT_MEASURED) {
        if (used >= limit)
            snprintf(g_fit.text, sizeof g_fit.text, menu_text(UI_MENU_VRAM_FIT_FULL), (unsigned long long)used,
                     (unsigned long long)limit, items);
        else
            snprintf(g_fit.text, sizeof g_fit.text, menu_text(level >= MEM_TIGHT ? UI_MENU_VRAM_FIT_MEASURED : UI_MENU_VRAM_FIT_NOTICED),
                     pct, (unsigned long long)used, (unsigned long long)limit, items);
        rt_log("menu: box   vram note: measured (video memory %s, %llu of %llu MiB in use, %d notice(s) shown this session): %s",
               mem_level_name[level], (unsigned long long)used, (unsigned long long)limit, noticed, said);
    } else if (g_fit.kind == VRAM_FIT_PRESET) {
        snprintf(g_fit.text, sizeof g_fit.text, menu_text(UI_MENU_VRAM_FIT_PRESET), preset_gib(hw->vram_mib), items);
        char limits[96] = "";
        if (preset_res) snprintf(limits, sizeof limits, "resolution %u", preset_res);
        if (preset_mode != VRAM_MODE_OFF) {
            snprintf(part, sizeof part, "upscaling mode %s", mode_log[preset_mode]);
            add(limits, sizeof limits, part);
        }
        rt_log("menu: box   vram note: preset (%u GiB of video memory, tier %s gives %s; video memory %s): %s",
               preset_gib(hw->vram_mib), preset_tier_names[vram_fit_tier(hw->vram_mib)], limits, mem_level_name[level], said);
    } else {
        const bool heavier = vram_fit_heavier(&now, &draft) != 0;
        rt_log("menu: box   vram note: none (%s; video memory %s, %llu of %llu MiB in use, %u GiB card)",
               heavier ? "what the draft makes heavier is within the room and the preset" : "nothing the draft changes uses more",
               mem_level_name[level], (unsigned long long)used, (unsigned long long)limit,
               hw->have_gpu ? preset_gib(hw->vram_mib) : 0);
    }
}

static void apply_confirm(float scale) {
    if (g_apply_ask) {
        g_apply_ask = false;
        ImGui::OpenPopup("##apply");
        const int shown = opt_draft_count();
        rt_log("menu: box \"Apply your changes?\" opened: %d line(s)", shown);
        for (int i = 0; i < shown; i++) {
            OptDraftItem it;
            if (opt_draft_item(i, &it))
                rt_log("menu: box   %s: %s -> %s%s", it.label, it.from, it.to, it.restart ? " (after restart)" : "");
        }
        vram_fit_make();
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 0);
    const bool open = ImGui::BeginPopupModal("##apply", nullptr,
                                             ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::PopStyleVar();
    if (!open) return;
    g_modal = true;
    const ImVec2 box = ImGui::GetWindowPos(), box_size = ImGui::GetWindowSize();
    skin_panel(ImGui::GetWindowDrawList(), box, ImVec2(box.x + box_size.x, box.y + box_size.y), scale);
    const ImGuiStyle &st = ImGui::GetStyle();
    const ImVec2 view = ImGui::GetMainViewport()->Size;
    const int n = opt_draft_count();
    bool apply = false, discard = false, keep = false;
    if (n == 0) {
        discard = true;
    } else {
        ImGui::TextUnformatted(menu_text(UI_MENU_DRAFT_TITLE));
        ImGui::Spacing();
        const char *yes = menu_text(UI_MENU_DRAFT_APPLY), *no = menu_text(UI_MENU_DRAFT_DISCARD),
                   *back = menu_text(UI_MENU_DRAFT_KEEP);
        float bw = fmaxf(fmaxf(ImGui::CalcTextSize(yes).x, ImGui::CalcTextSize(no).x), ImGui::CalcTextSize(back).x) +
                   st.FramePadding.x * 4;
        const float gap = st.ItemSpacing.x + skin_cursor_reach(scale);
        const float list_w = fmaxf(fminf(640 * scale, view.x * 0.7f), bw * 3 + gap * 2);
        const float line = ImGui::GetTextLineHeightWithSpacing();
        const float text_w = list_w - st.ScrollbarSize - st.ItemSpacing.x;
        char mark[96];
        snprintf(mark, sizeof mark, "(%s)", menu_text(UI_MENU_AFTER_RESTART));
        const float mark_w = ImGui::CalcTextSize(mark).x, space_w = ImGui::CalcTextSize(" ").x;
        struct Row { std::string text; bool mark, mark_alone; };
        std::vector<Row> rows;
        for (int i = 0; i < n; i++) {
            OptDraftItem it;
            if (!opt_draft_item(i, &it)) break;
            char text[320];
            snprintf(text, sizeof text, "%s: %s -> %s", it.label, it.from, it.to);
            const size_t first = rows.size();
            for (const char *s = text, *end = text + strlen(text); s < end;) {
                const char *e = ImGui::GetFont()->CalcWordWrapPositionA(ImGui::GetFontSize() / ImGui::GetFont()->FontSize, s, end, text_w);
                if (e == s)
                    for (e = s + 1; e < end && (*e & 0xc0) == 0x80;) e++;
                rows.push_back({std::string(s, e), false, false});
                for (s = e; s < end && *s == ' ';) s++;
            }
            if (rows.size() == first) rows.push_back({"", false, false});
            if (!it.restart) continue;
            if (ImGui::CalcTextSize(rows.back().text.c_str()).x + space_w + mark_w <= text_w) rows.back().mark = true;
            else rows.push_back({"", true, true});
        }
        const int most = (int)fmaxf(floorf(view.y * 0.4f / line), 1);
        const int shown = (int)rows.size() < most ? (int)rows.size() : most;
        static float scroll;
        if (ImGui::IsWindowAppearing()) scroll = 0;
        ImGui::BeginChild("##draftlist", ImVec2(list_w, shown * line), ImGuiChildFlags_None, ImGuiWindowFlags_NoNav);
        for (const Row &r : rows) {
            if (!r.mark_alone) ImGui::TextUnformatted(r.text.c_str());
            if (r.mark && !r.mark_alone) ImGui::SameLine(0, space_w);
            if (r.mark) ImGui::TextColored(ImVec4(0.95f, 0.66f, 0.40f, 1), "%s", mark);
        }
        const float dy = (float)(ImGui::IsKeyDown(ImGuiKey_GamepadDpadDown) + ImGui::IsKeyDown(ImGuiKey_DownArrow) -
                                 ImGui::IsKeyDown(ImGuiKey_GamepadDpadUp) - ImGui::IsKeyDown(ImGuiKey_UpArrow));
        const float scroll_most = ((int)rows.size() - shown) * line;
        if (dy == 0) scroll = ImGui::GetScrollY();
        scroll = fminf(fmaxf(scroll + dy * 700 * scale * ImGui::GetIO().DeltaTime, 0), scroll_most);
        ImGui::SetScrollY(roundf(scroll / line) * line);
        ImGui::EndChild();
        ImGui::Spacing();
        if (g_fit.kind != VRAM_FIT_NONE && g_fit.text[0]) {
            ImGui::PushStyleColor(ImGuiCol_Text, g_fit.kind == VRAM_FIT_MEASURED ? ImVec4(0.95f, 0.66f, 0.40f, 1)
                                                                             : ImGui::ColorConvertU32ToFloat4(SKIN_TEXT_DIM));
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + list_w);
            ImGui::TextUnformatted(g_fit.text);
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
            ImGui::Spacing();
        }
        apply = skin_widget_button(yes, ImVec2(bw, 0), scale);
        ImGui::SameLine(0, gap);
        discard = skin_widget_button(no, ImVec2(bw, 0), scale);
        ImGui::SameLine(0, gap);
        keep = skin_widget_button(back, ImVec2(bw, 0), scale);
        if (ImGui::IsWindowAppearing()) ImGui::SetItemDefaultFocus();
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false))
            keep = true, menu_sound_play(MENU_SOUND_CANCEL);
    }
    if (apply || discard || keep)
        rt_log("menu: box \"Apply your changes?\": %s", apply ? "Apply" : keep ? "Continue editing (the draft stays)"
                                                       : n == 0 ? "closed: nothing left in the draft (changed outside)"
                                                                : "Discard");
    if (apply || discard || keep) ImGui::CloseCurrentPopup();
    if (apply && g_pause_tab.load()) {
        std::string list;
        for (int i = 0; i < n; i++) {
            OptDraftItem it;
            if (!opt_draft_item(i, &it)) break;
            list += std::string(i ? "; " : "") + it.label + ": " + it.from + " -> " + it.to + (it.restart ? " (after restart)" : "");
        }
        pause_tab_applied(n, list.c_str());
        g_pause_why = "Apply";
    } else if (discard && g_pause_tab.load()) {
        g_pause_why = "Discard";
    }
    if (apply) {
        g_restart_lines.clear();
        for (int i = 0; i < n; i++) {
            OptDraftItem it;
            if (opt_draft_item(i, &it) && it.restart)
                g_restart_lines.push_back({std::string(it.label) + ": " + it.to, it.display});
        }
        bool ok = opt_draft_apply(true);
        if (ok || opt_display_pending()) post_next(false);
    } else if (discard) {
        opt_draft_discard();
        set_open(0);
    }
    ImGui::EndPopup();
}

static void preset_confirm(float scale) {
    if (g_preset.ask) {
        g_preset.ask = false;
        ImGui::OpenPopup("##preset");
        rt_log("menu: box \"choose this preset?\" opened: %d key(s) set by hand", (int)g_preset.lines.size());
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 0);
    const bool open = ImGui::BeginPopupModal("##preset", nullptr,
                                             ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::PopStyleVar();
    if (!open) return;
    g_modal = true;
    const ImVec2 box = ImGui::GetWindowPos(), box_size = ImGui::GetWindowSize();
    skin_panel(ImGui::GetWindowDrawList(), box, ImVec2(box.x + box_size.x, box.y + box_size.y), scale);
    const ImGuiStyle &st = ImGui::GetStyle();
    const float list_w = fminf(640 * scale, ImGui::GetMainViewport()->Size.x * 0.7f);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + list_w);
    ImGui::TextUnformatted(menu_text(UI_MENU_PRESET_CONFIRM));
    ImGui::Spacing();
    for (const std::string &l : g_preset.lines) ImGui::TextUnformatted(l.c_str());
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    const char *yes = menu_text(UI_MENU_PRESET_CONFIRM_YES), *no = menu_text(UI_MENU_RESET_NO);
    const float bw = fmaxf(ImGui::CalcTextSize(yes).x, ImGui::CalcTextSize(no).x) + st.FramePadding.x * 4;
    bool close = skin_widget_button(no, ImVec2(bw, 0), scale);
    if (ImGui::IsWindowAppearing()) ImGui::SetItemDefaultFocus();
    ImGui::SameLine(0, st.ItemSpacing.x + skin_cursor_reach(scale));
    const bool use = skin_widget_button(yes, ImVec2(bw, 0), scale);
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false))
        close = true, menu_sound_play(MENU_SOUND_CANCEL);
    if (use) {
        rt_log("menu: box \"choose this preset?\": yes");
        opt_context("NX2SARIA_PRESET", UI_MENU_PRESET, g_preset.from.c_str(), g_preset.to.c_str());
        preset_stage(g_preset.choice);
        menu_sound_play(MENU_SOUND_CURSOR);
    } else if (close) {
        rt_log("menu: box \"choose this preset?\": Cancel (nothing staged)");
    }
    if (use || close) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

static void post_confirm(float scale) {
    if (g_post == POST_NONE) return;
    const bool keep_box = g_post == POST_KEEP;
    const char *id = keep_box ? "##keep" : "##restartnow";
    if (!g_post_open) {
        if (GImGui->FrameCount < g_post_frame) return;
        ImGui::OpenPopup(id);
        g_post_open = true;
        if (keep_box) rt_log("menu: box \"Keep these display settings?\" opened: %llu s to answer, then Revert",
                             (unsigned long long)(KEEP_NS / 1000000000ull));
        else rt_log("menu: box \"Some changes take effect after a restart\" opened: %d setting(s)", (int)g_restart_lines.size());
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 0);
    const bool open = ImGui::BeginPopupModal(id, nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::PopStyleVar();
    if (!open) {
        g_post_open = false;
        rt_log("menu: box %s: closed from outside: %s", keep_box ? "\"Keep these display settings?\"" : "restart",
               keep_box ? "Revert" : "Later");
        if (keep_box) {
            if (opt_display_revert()) keep_reverted();
            gfx_window_apply_settings();
            post_next(true);
        } else {
            g_post = POST_NONE;
            set_open(0);
        }
        return;
    }
    g_modal = true;
    const ImVec2 box = ImGui::GetWindowPos(), box_size = ImGui::GetWindowSize();
    skin_panel(ImGui::GetWindowDrawList(), box, ImVec2(box.x + box_size.x, box.y + box_size.y), scale);
    const ImGuiStyle &st = ImGui::GetStyle();
    const float gap = st.ItemSpacing.x + skin_cursor_reach(scale);
    bool yes = false, no = false;
    if (keep_box) {
        const uint64_t now = plat_now_ns();
        const int left = now >= g_keep_deadline_ns ? 0 : (int)((g_keep_deadline_ns - now + 999999999ull) / 1000000000ull);
        if (opt_display_external()) {
            rt_log("menu: box \"Keep these display settings?\": F11 took control: Keep, with F11's state");
            opt_display_keep();
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            post_next(true);
            return;
        }
        ImGui::TextUnformatted(menu_text(UI_MENU_KEEP_TITLE));
        ImGui::Text(menu_text(UI_MENU_KEEP_COUNTDOWN), left);
        ImGui::Spacing();
        const char *ky = menu_text(UI_MENU_KEEP_YES), *kn = menu_text(UI_MENU_KEEP_REVERT);
        float bw = fmaxf(ImGui::CalcTextSize(ky).x, ImGui::CalcTextSize(kn).x) + st.FramePadding.x * 4;
        yes = skin_widget_button(ky, ImVec2(bw, 0), scale);
        ImGui::SameLine(0, gap);
        no = skin_widget_button(kn, ImVec2(bw, 0), scale);
        if (ImGui::IsWindowAppearing()) ImGui::SetItemDefaultFocus();
        const char *why = "button";
        if (left == 0 || ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)) {
            if (left) menu_sound_play(MENU_SOUND_CANCEL);
            why = left ? "Esc or B" : "the countdown ran out";
            no = true;
        }
        if (yes || no) {
            rt_log("menu: box \"Keep these display settings?\": %s (%s, %d s left)", yes ? "Keep" : "Revert", yes ? "button" : why, left);
            if (yes) {
                opt_display_keep();
            } else {
                if (opt_display_revert()) keep_reverted();
                gfx_window_apply_settings();
            }
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            post_next(true);
            return;
        }
    } else {
        ImGui::TextUnformatted(menu_text(UI_MENU_RESTART_NOW_TITLE));
        ImGui::Spacing();
        for (const RestartLine &l : g_restart_lines) ImGui::TextUnformatted(l.text.c_str());
        const bool saving = fs_save_busy() != 0;
        if (saving)
            ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.35f, 1), "%s", menu_text(UI_MENU_RESTART_SAVING));
        ImGui::Spacing();
        const char *ry = menu_text(UI_MENU_RESTART_NOW), *rn = menu_text(UI_MENU_RESTART_LATER);
        float bw = fmaxf(ImGui::CalcTextSize(ry).x, ImGui::CalcTextSize(rn).x) + st.FramePadding.x * 4;
        ImGui::BeginDisabled(saving);
        yes = skin_widget_button(ry, ImVec2(bw, 0), scale);
        ImGui::EndDisabled();
        ImGui::SameLine(0, gap);
        no = skin_widget_button(rn, ImVec2(bw, 0), scale);
        if (ImGui::IsWindowAppearing()) ImGui::SetItemDefaultFocus();
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false))
            no = true, menu_sound_play(MENU_SOUND_CANCEL);
        if (yes || no) rt_log("menu: box restart: %s", yes ? "Restart now" : "Later (saved for the next start)");
        if (yes) {
            gfx_window_request_restart();
        } else if (no) {
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            g_post = POST_NONE;
            g_restart_lines.clear();
            set_open(0);
            return;
        }
    }
    ImGui::EndPopup();
}

static void rows_gyro(void) {
    opt_toggle("NX2SARIA_MOTION", UI_MENU_MOTION, UI_MENU_MOTION_DESC);
    const bool motion = opt_bool("NX2SARIA_MOTION");
    if (!motion) opt_disable_next(UI_MENU_GYRO_NEEDS_MOTION);
    opt_toggle("NX2SARIA_GYRO_CALIBRATION", UI_MENU_GYRO_CALIBRATION, UI_MENU_GYRO_CALIBRATION_DESC);
    static const UiTextId deadzone[] = {UI_MENU_GYRO_DEADZONE_OFF, UI_MENU_GYRO_DEADZONE_LOW,
                                        UI_MENU_GYRO_DEADZONE_STANDARD, UI_MENU_GYRO_DEADZONE_HIGH};
    if (!motion) opt_disable_next(UI_MENU_GYRO_NEEDS_MOTION);
    opt_choice("NX2SARIA_GYRO_DEADZONE", UI_MENU_GYRO_DEADZONE, deadzone, 4, UI_MENU_GYRO_DEADZONE_DESC);
    if (!motion) opt_disable_next(UI_MENU_GYRO_NEEDS_MOTION);
    opt_slider("NX2SARIA_GYRO_TURN_SENS", UI_MENU_GYRO_TURN_SENS, 10, 400, UI_MENU_GYRO_TURN_SENS_DESC, 5, "%.0f %%");
    if (!motion) opt_disable_next(UI_MENU_GYRO_NEEDS_MOTION);
    opt_toggle("NX2SARIA_GYRO_TURN_INVERT", UI_MENU_GYRO_TURN_INVERT, UI_MENU_GYRO_TURN_INVERT_DESC);
    if (!motion) opt_disable_next(UI_MENU_GYRO_NEEDS_MOTION);
    opt_slider("NX2SARIA_GYRO_TILT_SENS", UI_MENU_GYRO_TILT_SENS, 10, 400, UI_MENU_GYRO_TILT_SENS_DESC, 5, "%.0f %%");
    if (!motion) opt_disable_next(UI_MENU_GYRO_NEEDS_MOTION);
    opt_toggle("NX2SARIA_GYRO_TILT_INVERT", UI_MENU_GYRO_TILT_INVERT, UI_MENU_GYRO_TILT_INVERT_DESC);
}

static void rows_controls(void) {
    if (input_direct_mouse_available()) {
        static const UiTextId mouse_mode[] = {UI_MENU_MOUSE_STICK, UI_MENU_MOUSE_DIRECT};
        opt_choice("NX2SARIA_MOUSE_MODE", UI_MENU_MOUSE_MODE, mouse_mode, 2, UI_MENU_MOUSE_MODE_DESC);
    }
    opt_slider("NX2SARIA_MOUSE_SENS", UI_MENU_MOUSE_SENS, 0.05f, 10, UI_MENU_MOUSE_SENS_DESC, 0.05f, "%.2f");
    opt_toggle("NX2SARIA_MENU_CURSOR", UI_MENU_MENU_CURSOR, UI_MENU_MENU_CURSOR_DESC);
    static const UiTextId layout[] = {UI_MENU_PAD_LAYOUT_AUTO, UI_MENU_PAD_LAYOUT_NINTENDO, UI_MENU_PAD_LAYOUT_XBOX,
                                      UI_MENU_PAD_LAYOUT_PLAYSTATION};
    opt_choice("NX2SARIA_PAD_LAYOUT", UI_MENU_PAD_LAYOUT, layout, 4, UI_MENU_PAD_LAYOUT_DESC, pad_layout_note());
    static const UiTextId prompts[] = {UI_MENU_PROMPTS_NINTENDO, UI_MENU_PROMPTS_XBOX, UI_MENU_PROMPTS_PLAYSTATION,
                                       UI_MENU_PROMPTS_STEAMDECK, UI_MENU_PROMPTS_PC};
    bool restart;
    opt_choice("NX2SARIA_PROMPTS", UI_MENU_PROMPTS, prompts, 5, UI_MENU_PROMPTS_DESC, prompts_note(&restart));
    if (sprint_available()) {
        static const UiTextId sprint_button[] = {UI_MENU_SPRINT_OFF, UI_MENU_SPRINT_B, UI_MENU_SPRINT_L3};
        static const UiTextId sprint_mode[] = {UI_MENU_SPRINT_TOGGLE, UI_MENU_SPRINT_HOLD};
        opt_choice("NX2SARIA_SPRINT_BUTTON", UI_MENU_SPRINT_BUTTON, sprint_button, 3, UI_MENU_SPRINT_BUTTON_DESC);
        opt_choice("NX2SARIA_SPRINT_MODE", UI_MENU_SPRINT_MODE, sprint_mode, 2, UI_MENU_SPRINT_MODE_DESC);
    }
    opt_slider("NX2SARIA_RUMBLE_STRENGTH", UI_MENU_RUMBLE_STRENGTH, 0, 300, UI_MENU_RUMBLE_STRENGTH_DESC, 10, "%.0f %%");
    opt_slider("NX2SARIA_RUMBLE_CURVE", UI_MENU_RUMBLE_CURVE, 0.2f, 1.5f, UI_MENU_RUMBLE_CURVE_DESC, 0.05f, "%.2f");
    opt_toggle("NX2SARIA_RUMBLE_TRIGGERS", UI_MENU_RUMBLE_TRIGGERS, UI_MENU_RUMBLE_TRIGGERS_DESC);
    if (!input_rumble_hd_available()) opt_disable_next(UI_MENU_RUMBLE_HD_NEEDS);
    opt_toggle("NX2SARIA_RUMBLE_HD", UI_MENU_RUMBLE_HD, UI_MENU_RUMBLE_HD_DESC);
    opt_group(UI_MENU_GROUP_GYRO);
    rows_gyro();
    opt_group(UI_MENU_GROUP_STICKS);
    double now[2];
    bool pad = input_pad_stick_percent(now) != 0;
    char stick_note[2][64];
    for (int s = 0; s < 2; s++)
        snprintf(stick_note[s], sizeof stick_note[s], menu_text(UI_MENU_STICK_DEADZONE_NOW), now[s]);
    opt_slider("NX2SARIA_LSTICK_DEADZONE", UI_MENU_LSTICK_DEADZONE, 0, 50, UI_MENU_STICK_DEADZONE_DESC, 1, "%.0f %%",
               pad ? stick_note[0] : nullptr, "NX2SARIA_STICK_DEADZONE");
    opt_slider("NX2SARIA_RSTICK_DEADZONE", UI_MENU_RSTICK_DEADZONE, 0, 50, UI_MENU_STICK_DEADZONE_DESC, 1, "%.0f %%",
               pad ? stick_note[1] : nullptr, "NX2SARIA_STICK_DEADZONE");
    opt_slider("NX2SARIA_STICK_SENS", UI_MENU_STICK_SENS, 0.1f, 3, UI_MENU_STICK_SENS_DESC, 0.05f, "%.2f");
    row_walk_key();
    opt_slider("NX2SARIA_WALK_SCALE", UI_MENU_WALK_SCALE, 0.2f, 0.9f, UI_MENU_WALK_SCALE_DESC, 0.01f, "%.2f");
}

static void tab_controls(float scale) {
    opt_begin("##controls", scale);
    rows_controls();
    binds_rows(scale);
    opt_end();
    binds_after_page();
}

static const UiTextId kTabNames[TAB_COUNT] = {UI_MENU_TAB_SCREEN, UI_MENU_TAB_GAME, UI_MENU_TAB_SOUND,
                                              UI_MENU_TAB_CONTROLS, UI_MENU_TAB_AMIIBO, UI_MENU_TAB_CHEATS};
static int g_tab;
static int g_tab_request = -1;
static int g_focus_page = -1;

static bool g_sound_fresh;
static int g_tab_sounded = -1;

static void menu_sounds_frame(void) {
    ImGuiContext &g = *GImGui;
    static int last_frame = -2, last_tab = -1;
    static ImGuiID last_nav;
    bool fresh = g_sound_fresh || g.FrameCount != last_frame + 1;
    g_sound_fresh = false;
    last_frame = g.FrameCount;
    if (!fresh) {
        if (g.ActiveId && g.ActiveIdIsJustActivated) menu_sound_play(MENU_SOUND_DECIDE);
        else if (g.NavId && g.NavId != last_nav) menu_sound_play(MENU_SOUND_CURSOR);
        if (g_tab != last_tab && last_tab >= 0 && g_tab != g_tab_sounded)
            menu_sound_play(g_tab > last_tab ? MENU_SOUND_TAB_RIGHT : MENU_SOUND_TAB_LEFT);
    }
    if (g_tab != last_tab) g_tab_sounded = -1;
    last_nav = g.NavId, last_tab = g_tab;
}

static int g_pause_frames;

static void pause_tab_close(const char *why) {
    g_pause_why = why;
    g_apply_ask = false;
    if (GImGui->OpenPopupStack.Size) ImGui::ClosePopupToLevel(0, false);
    g_modal = false;
    opt_draft_discard();
    set_open(0);
}

static bool g_more_req;

static void pause_tab_rows(void) {
    opt_group(UI_MENU_GRAPHICS);
    row_preset();
    row_fps_limit();
    opt_toggle_value("NX2SARIA_VK_PRESENT", UI_MENU_PRESENT_VSYNC, UI_MENU_PRESENT_DESC, "mailbox", "immediate", vsync_note());
    row_upscaling();
    row_aa();
    row_framegen();
    if (gfx_surface_hdr10() || opt_bool("NX2SARIA_HDR")) row_hdr();
    int dl = opt_bool("NX2SARIA_DLAA"), fsr3 = dl && !strcmp(opt_value("NX2SARIA_TAA"), "fsr3") && vk_ffx_available();
    if (!fsr3 && (strcmp(opt_value("NX2SARIA_UPSCALER"), "fsr") || dl)) opt_disable_next(UI_MENU_SHARPNESS_NEEDS_FSR);
    opt_slider("NX2SARIA_SHARPNESS", UI_MENU_SHARPNESS, 0, 100, UI_MENU_SHARPNESS_DESC, 5, "%.0f %%");
    static const char *const aniso[] = {"game", "2", "4", "8", "16"};
    const char *aniso_names[] = {menu_text(UI_MENU_ANISO_GAME), "2x", "4x", "8x", "16x"};
    opt_choice_list("NX2SARIA_ANISO", UI_MENU_ANISO, aniso, aniso_names, 5, UI_MENU_ANISO_DESC);
    if (wide_available()) {
        static const UiTextId wide[] = {UI_MENU_GAME_ASPECT_16_9, UI_MENU_GAME_ASPECT_WINDOW, UI_MENU_GAME_ASPECT_16_10,
                                        UI_MENU_GAME_ASPECT_3_2, UI_MENU_GAME_ASPECT_21_9, UI_MENU_GAME_ASPECT_32_9};
        opt_choice("NX2SARIA_GAME_ASPECT", UI_MENU_GAME_ASPECT, wide, 6, UI_MENU_GAME_ASPECT_DESC);
        row_wide_placement();
    }
    opt_group(UI_MENU_TAB_SOUND);
    rows_sound();
    opt_group(UI_MENU_TAB_CONTROLS);
    rows_controls();
    opt_group(UI_MENU_TAB_GAME);
    rows_game();
    if (opt_button("##more", UI_MENU_MORE_SETTINGS, UI_MENU_MORE_SETTINGS_DESC)) g_more_req = true;
}

static const char *pause_tab_label(void) {
    return pause_tab_name_kind() == 1 ? menu_text(UI_MENU_PAUSE_TAB) : "NX2Saria";
}

static void pause_tab_header(uint32_t w, uint32_t h, float scale, bool current) {
    PauseTabView v;
    float r[4];
    if (!pause_tab_view(&v) || !gfx_picture_rect(r)) return;
    const float x0 = (r[0] + v.tab[0] * r[2]) * w, y0 = (r[1] + v.tab[1] * r[3]) * h;
    const float x1 = (r[0] + v.tab[2] * r[2]) * w, y1 = (r[1] + v.tab[3] * r[3]) * h;
    if (!(x1 > x0 && y1 > y0)) return;
    const float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f, bw = x1 - x0, bh = y1 - y0;
    ImFontAtlas *atlas = ImGui::GetIO().Fonts;
    ImFont *font = atlas->Fonts.Size ? atlas->Fonts[0] : ImGui::GetFont();
    const char *label = pause_tab_label();
    bool cjk = false;
    for (const unsigned char *c = (const unsigned char *)label; *c; c++) cjk |= *c >= 0xe3 && *c <= 0xe9;
    float px = bh * (cjk ? 0.38f : 0.28f);
    float wide = font->CalcTextSizeA(px, FLT_MAX, 0, label).x;
    if (wide > bw * 0.94f) px *= bw * 0.94f / wide, wide = bw * 0.94f;
    const float plate = fminf(bw, wide + bh * 0.5f), top = r[1] * h, bottom = cy + bh * 0.545f;
    skin_header_tab(ImGui::GetBackgroundDrawList(), ImVec2(cx - plate * 0.5f, top), ImVec2(cx + plate * 0.5f, bottom),
                    ImVec2(cx, cy + bh * 0.06f), scale, current, label, font, px, v.alpha);
}

static void layout_pause_tab(uint32_t w, uint32_t h, float scale) {
    PauseTabView v = {};
    float r[4];
    g_modal = false;
    pause_tab_view(&v);
    g_pause_frames++;
    if (pause_tab_test("fail") && g_pause_frames > 30) {
        pause_tab_close("its page failed to draw (NX2SARIA_PAUSE_SETTINGS_TAB_TEST=fail); the System tab's page comes back");
        return;
    }
    if (!v.page_ok || !gfx_picture_rect(r)) {
        pause_tab_close("the pause menu's page is not seen any more; the System tab's page comes back");
        return;
    }
    opt_draft_sync();
    const ImVec2 a((r[0] + v.page[0] * r[2]) * w, (r[1] + v.page[1] * r[3]) * h);
    const ImVec2 b((r[0] + v.page[2] * r[2]) * w, (r[1] + v.page[3] * r[3]) * h);
    ImGui::SetNextWindowPos(a, ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(b.x - a.x, b.y - a.y), ImGuiCond_Always);
    ImGui::Begin("##pausetab", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                            ImGuiWindowFlags_NoSavedSettings);
    if (g_pause_frames == 1) opt_focus_first();
    opt_reserve_bottom(0);
    opt_begin("##pausetab", scale);
    pause_tab_rows();
    opt_end();
    apply_confirm(scale);
    preset_confirm(scale);
    post_confirm(scale);
    menu_sounds_frame();
    const bool was_modal = g_modal || g_post != POST_NONE || ImGui::GetIO().WantTextInput;
    if (g_more_req && !was_modal) {
        g_more_req = false;
        g_pause_tab.store(0);
        g_more_open = plat_now_ns();
        g_tab_request = TAB_CONTROLS;
        pause_tab_set_open(0, "More settings");
        pause_tab_more(1, 0);
        ImGui::End();
        return;
    }
    g_more_req = false;
    const bool plus = g_pad_plus != 0;
    g_pad_plus = 0;
    const char *key = ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false) || ImGui::IsKeyPressed(ImGuiKey_Q, false) ? "L"
                      : ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false) ? "B"
                      : plus ? "Plus" : nullptr;
    if (!was_modal && key) {
        g_pause_why = key;
        menu_sound_play(MENU_SOUND_CANCEL);
        leave_menu();
    }
    ImGui::End();
}

static void layout_menu(uint32_t w, uint32_t h, float scale) {
    opt_draft_sync();
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), ImVec2((float)w, (float)h), IM_COL32(10, 6, 2, 150));
    float ww = fminf(w * 0.88f, 1400 * scale), wh = fminf(h * 0.86f, 820 * scale);
    ImGui::SetNextWindowPos(ImVec2(w * 0.5f, h * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(ww, wh), ImGuiCond_Always);
    ImTextureID frame_tex;
    const bool framed = menu_art(MENU_ART_FRAME_DECO, &frame_tex, nullptr, nullptr);
    if (framed) ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::Begin("##menu", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                        ImGuiWindowFlags_NoSavedSettings);
    if (framed) ImGui::PopStyleVar();
    const ImVec2 win_p = ImGui::GetWindowPos(), win_s = ImGui::GetWindowSize();
    if (framed) {
        ImDrawList *fdl = ImGui::GetWindowDrawList();
        const float ox = 15 * scale, oy = 30 * scale;
        fdl->PushClipRectFullScreen();
        for (int turned = 0; turned < 2; turned++)
            menu_art_draw9_turned(fdl, MENU_ART_FRAME_DECO, ImVec2(win_p.x - ox, win_p.y - oy),
                                  ImVec2(win_p.x + win_s.x + ox, win_p.y + win_s.y + oy), 96 * scale, 0.2f,
                                  IM_COL32(236, 196, 112, 235), 0, turned);
        fdl->PopClipRect();
    }
    if (g_title_font) ImGui::PushFont(g_title_font);
    {
        ImVec2 at = ImGui::GetCursorScreenPos();
        float th = ImGui::GetTextLineHeight();
        triforce(ImGui::GetWindowDrawList(), at.x + th * 0.6f, at.y + th * 0.02f, th * 0.96f, ImGui::GetColorU32(kGold));
        ImGui::SetCursorScreenPos(ImVec2(at.x + th * 1.5f, at.y));
        ImGui::TextColored(kGold, "Ocarina of Time");
        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        ImGui::TextUnformatted(menu_text(UI_MENU_TITLE));
    }
    const float title_end = ImGui::GetItemRectMax().x;
    if (g_title_font) ImGui::PopFont();
    char game[16], version[96];
    snprintf(game, sizeof game, "%d.%d.%d", rt_game_version / 100, rt_game_version / 10 % 10, rt_game_version % 10);
    snprintf(version, sizeof version, "NX2Saria %s  -  game %s", rt_port_version, game);
    float version_x = win_s.x - ImGui::GetStyle().WindowPadding.x - ImGui::CalcTextSize(version).x;
    if (win_p.x + version_x >= title_end + ImGui::GetStyle().ItemSpacing.x * 2) {
        ImGui::SameLine(version_x);
        ImGui::TextDisabled("%s", version);
    }

    const bool keeps_keys = g_modal || binds_modal() || ImGui::GetIO().WantTextInput;
    int step = keeps_keys                                                                                ? 0
             : ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) || ImGui::IsKeyPressed(ImGuiKey_E, false) ? 1
             : ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false) || ImGui::IsKeyPressed(ImGuiKey_Q, false) ? -1
                                                                                                         : 0;
    const int ntabs = tabs_shown();
    if (step) g_tab_request = g_tab_sounded = (g_tab + step + ntabs) % ntabs;
    if (step) menu_sound_play(step > 0 ? MENU_SOUND_TAB_RIGHT : MENU_SOUND_TAB_LEFT);
    if (g_tab_request >= 0) g_focus_page = g_tab_request;

    const ImGuiStyle &st = ImGui::GetStyle();
    float bottom = ImGui::GetFrameHeight() + st.ItemSpacing.y * 2;
    if (ImGui::BeginTabBar("##tabs", ImGuiTabBarFlags_NoTabListScrollingButtons)) {
        for (int k = 0; k < ntabs; k++) {
            char label[128];
            snprintf(label, sizeof label, "  %s  ###tab%d", menu_text(kTabNames[k]), k);
            bool shown = skin_tab_item(label, menu_text(kTabNames[k]), g_tab_request == k ? ImGuiTabItemFlags_SetSelected : 0, scale, g_title_font);
            if (!shown) continue;
            g_tab = k;
            if (g_focus_page == k) {
                opt_focus_first();
                g_focus_page = -1;
            }
            ImGui::Spacing();
            opt_reserve_bottom(bottom);
            switch (k) {
            case TAB_SCREEN: tab_screen(scale); break;
            case TAB_GAME: tab_game(scale); break;
            case TAB_SOUND: tab_sound(scale); break;
            case TAB_CONTROLS: tab_controls(scale); break;
            case TAB_AMIIBO: tab_amiibo(scale); break;
            case TAB_CHEATS: tab_cheats(scale); break;
            }
            ImGui::EndTabItem();
        }
        g_tab_request = -1;
        ImGui::EndTabBar();
    }

    ImGui::Spacing();
    float y = ImGui::GetCursorPosY();
    {
        float sy = win_p.y + y, fh = ImGui::GetFrameHeight();
        menu_art_draw(ImGui::GetWindowDrawList(), MENU_ART_GUIDE_BAR, ImVec2(win_p.x + st.WindowPadding.x, sy),
                      ImVec2(win_p.x + win_s.x - st.WindowPadding.x, sy + fh), IM_COL32(96, 70, 36, 170));
    }
    if (!cfg_last_save_ok()) {
        ImGui::SetCursorPosY(y + st.FramePadding.y);
        ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.35f, 1), "%s", menu_text(UI_MENU_SAVE_FAILED));
    }
    char shown[80], restart[96];
    const int pending = restart_pending();
    if (pending) snprintf(shown, sizeof shown, menu_text(UI_MENU_RESTART_COUNT), pending);
    else snprintf(shown, sizeof shown, "%s", menu_text(UI_MENU_RESTART_YES));
    snprintf(restart, sizeof restart, "%s###restart", shown);
    const char *cont = menu_text(UI_MENU_CONTINUE), *quit = menu_text(UI_MENU_QUIT);
    float bw = fmaxf(fmaxf(ImGui::CalcTextSize(cont).x, ImGui::CalcTextSize(shown).x), ImGui::CalcTextSize(quit).x) + st.FramePadding.x * 4;
    float gap = st.ItemSpacing.x + skin_cursor_reach(scale);
    const float buttons_x = fmaxf(st.WindowPadding.x, ImGui::GetWindowWidth() - st.WindowPadding.x - bw * 3 - gap * 2);
    const char *unknown = cfg_unknown_keys(NULL);
    if (cfg_last_save_ok() && *unknown) {
        char note[512];
        snprintf(note, sizeof note, menu_text(UI_MENU_INI_UNKNOWN), unknown);
        size_t keep = strlen(note), head = keep > strlen(unknown) ? keep - strlen(unknown) : keep;
        const float room = buttons_x - st.WindowPadding.x - gap;
        while (keep > head && ImGui::CalcTextSize(note).x > room) {
            keep--;
            snprintf(note + keep, sizeof note - keep, "...");
        }
        if (keep > head) {
            ImGui::SetCursorPos(ImVec2(st.WindowPadding.x, y + st.FramePadding.y));
            ImGui::TextColored(ImVec4(0.90f, 0.80f, 0.50f, 1), "%s", note);
        }
    }
    ImGui::SetCursorPos(ImVec2(buttons_x, y));
    if (skin_widget_button(cont, ImVec2(bw, 0), scale)) leave_menu();
    ImGui::SameLine(0, gap);
    if (skin_widget_button(restart, ImVec2(bw, 0), scale)) g_confirm_quit = false, ImGui::OpenPopup("##confirm");
    ImGui::SameLine(0, gap);
    if (skin_widget_button(quit, ImVec2(bw, 0), scale)) g_confirm_quit = true, ImGui::OpenPopup("##confirm");
    restart_confirm(scale);
    apply_confirm(scale);
    preset_confirm(scale);
    post_confirm(scale);
    menu_sounds_frame();
    bool was_modal = g_modal || binds_modal() || ImGui::GetIO().WantTextInput;
    if (!was_modal && (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)))
        leave_menu();
    ImGui::End();
}

static unsigned g_perf_ver = ~0u;
static int g_perf_mode;
static std::atomic<int> g_perf_cycle;
static uint64_t g_perf_last_ns, g_perf_win_ns, g_perf_max_ns;
static int g_perf_frames;
static char g_perf_text[96];

static int perf_mode_of(const char *s) { return !strcmp(s, "full") ? 2 : !strcmp(s, "fps") ? 1 : 0; }

extern "C" void menu_perf_cycle(void) { g_perf_cycle.fetch_add(1); }

static void perf_cycle_once(void) {
    static const char *const modes[] = {"off", "fps", "full"};
    static const UiTextId names[] = {UI_MENU_PERF_OFF, UI_MENU_PERF_FPS, UI_MENU_PERF_FULL};
    int cur = perf_mode_of(cfg_value("NX2SARIA_PERF_OVERLAY")), next = (cur + 1) % 3;
    if (g_open.load(std::memory_order_relaxed)) {
        opt_change("NX2SARIA_PERF_OVERLAY", modes[next], menu_text(UI_MENU_PERF_OVERLAY), menu_text(names[cur]),
                   menu_text(names[next]));
    } else {
        cfg_set("NX2SARIA_PERF_OVERLAY", modes[next]);
        cfg_save();
    }
}

static std::atomic<int> g_cfg_reload;
static uint64_t g_notice_until_ns;
static char g_notice[512];

extern "C" void menu_cfg_reload(void) { g_cfg_reload.store(1); }

static void notice(const char *text) {
    snprintf(g_notice, sizeof g_notice, "%s", text);
    g_notice_until_ns = plat_now_ns() + 3000000000ull;
}

static void preset_notice_tick(bool busy) {
    static bool done;
    if (done || busy || g_notice_until_ns) return;
    done = true;
    if (!cfg_preset_notice_due()) return;
    char name[96], text[sizeof g_notice];
    snprintf(name, sizeof name, "%s", preset_tier_text(cfg_preset_recommended(nullptr)));
    snprintf(text, sizeof text, menu_text(UI_MENU_PRESET_NOTICE), name);
    notice(text);
    g_notice_until_ns = plat_now_ns() + 8000000000ull;
    cfg_preset_notice_done();
}

static void vram_warn_tick(const MenuTarget *t, bool busy) {
    if (!vram_warn_due()) return;
    const int why = g_open.load(std::memory_order_relaxed) ? VRAM_WAIT_SETTINGS
                    : ov_movie_live() ? VRAM_WAIT_MOVIE
                    : frame_step_world_us() < 0 ? VRAM_WAIT_TITLE
                    : wide_menu_open() ? VRAM_WAIT_PAUSE
                    : wide_loading_open() ? VRAM_WAIT_LOADING
                    : busy || g_notice_until_ns ? VRAM_WAIT_NOTICE : VRAM_WAIT_NONE;
    if (why != VRAM_WAIT_NONE) return vram_warn_waits(why);
    unsigned pw = 0, ph = 0;
    wide_picture(&pw, &ph);
    const bool fsr = !strcmp(cfg_value("NX2SARIA_TAA"), "fsr3");
    const bool full = cfg_bool("NX2SARIA_DLAA") && (!fsr || vk_ffx_available()) && !strcmp(cfg_value("NX2SARIA_DLSS"), "dlaa");
    const char *fg = cfg_value("NX2SARIA_FRAMEGEN");
    const bool gen = (!strcmp(fg, "fsr3") && vk_ffx_framegen_available()) || (!strcmp(fg, "dlss") && vk_dlssg_active());
    const unsigned what = vram_warn_suggest(wide_available() && ph > t->height + t->height / 50, full, gen);
    char text[sizeof g_notice], said[240] = "", part[160];
    size_t n = (size_t)snprintf(text, sizeof text, "%s", menu_text(UI_MENU_VRAM_WARN));
    auto line = [&](UiTextId id, UiTextId row, const char *mode) {
        if (n + 1 >= sizeof text) return;
        text[n++] = '\n';
        int m = snprintf(text + n, sizeof text - n, menu_text(id), menu_text(row), mode);
        n = m < 0 || n + (size_t)m >= sizeof text ? sizeof text - 1 : n + (size_t)m;
        size_t k = strlen(said);
        snprintf(said + k, sizeof said - k, "%s%s", k ? ", " : "", part);
    };
    if (what & VRAM_SUGGEST_RES) {
        snprintf(part, sizeof part, "resolution (a %ux%u picture on a %ux%u window)", pw, ph, t->width, t->height);
        line(UI_MENU_VRAM_WARN_RES, UI_MENU_GAME_RES, "");
    }
    if (what & VRAM_SUGGEST_MODE) {
        snprintf(part, sizeof part, "upscaling mode (%s at full size)", fsr ? "fsr3" : "dlss");
        line(UI_MENU_VRAM_WARN_MODE, UI_MENU_UPSCALING, menu_text(fsr ? UI_MENU_FSR3_NATIVE : UI_MENU_DLSS_DLAA));
    }
    if (what & VRAM_SUGGEST_FRAMEGEN) {
        snprintf(part, sizeof part, "frame generation (%s) off", fg);
        line(UI_MENU_VRAM_WARN_FRAMEGEN, UI_MENU_FRAMEGEN, "");
    }
    if (what & VRAM_SUGGEST_PRESET) {
        snprintf(part, sizeof part, "a lower quality preset (the picture fits the window, no full-size upscaling mode, "
                                    "no frame generation)");
        line(UI_MENU_VRAM_WARN_PRESET, UI_MENU_PRESET, "");
    }
    notice(text);
    g_notice_until_ns = plat_now_ns() + VRAM_WARN_SHOW_MS * 1000000ull;
    vram_warn_shown(said);
}

static void cfg_reload_tick(void) {
    if (!g_cfg_reload.exchange(0)) return;
    if (opt_draft_any() || opt_display_pending() || g_post != POST_NONE || (g_open.load() && (g_apply_ask || g_modal))) {
        rt_log("cfg: reload: not run: the settings menu holds changes not applied yet (draft %d, display %d, box %s)",
               opt_draft_any(), opt_display_pending(), g_post != POST_NONE || g_apply_ask || g_modal ? "open" : "none");
        notice(menu_text(UI_MENU_RELOAD_BUSY));
        return;
    }
    CfgReloadResult r;
    char text[160];
    if (!cfg_reload_file(&r)) snprintf(text, sizeof text, "%s", menu_text(UI_MENU_RELOAD_FAILED));
    else if (r.rejected) snprintf(text, sizeof text, menu_text(UI_MENU_RELOAD_REJECTED), r.changed, r.restart, r.rejected);
    else if (r.changed) snprintf(text, sizeof text, menu_text(UI_MENU_RELOAD_DONE), r.changed, r.restart);
    else snprintf(text, sizeof text, "%s", menu_text(UI_MENU_RELOAD_NONE));
    notice(text);
}

static void notice_draw(float scale) {
    if (!g_notice_until_ns) return;
    if (plat_now_ns() >= g_notice_until_ns) {
        g_notice_until_ns = 0;
        return;
    }
    ImDrawList *dl = ImGui::GetForegroundDrawList();
    float padx = 14 * scale, pady = 9 * scale;
    const float wrap = fmaxf(ImGui::GetIO().DisplaySize.x * 0.9f - padx * 2, 64 * scale);
    ImVec2 sz = ImGui::CalcTextSize(g_notice, nullptr, false, wrap);
    ImVec2 p((ImGui::GetIO().DisplaySize.x - sz.x) / 2 - padx, 12 * scale);
    skin_panel(dl, p, ImVec2(p.x + sz.x + padx * 2, p.y + sz.y + pady * 2), scale * 0.6f, 0.9f);
    dl->AddText(nullptr, 0.0f, ImVec2(p.x + padx, p.y + pady), SKIN_TEXT, g_notice, nullptr, wrap);
}

static void perf_gpu_once(const MenuTarget *t) {
    static bool done;
    if (done) return;
    done = true;
    uint32_t ne = 0;
    vkEnumerateDeviceExtensionProperties(t->phys, NULL, &ne, NULL);
    VkExtensionProperties *ep = (VkExtensionProperties *)calloc(ne ? ne : 1, sizeof *ep);
    bool has_bus = false;
    if (ep && vkEnumerateDeviceExtensionProperties(t->phys, NULL, &ne, ep) >= 0)
        for (uint32_t i = 0; i < ne && !has_bus; i++) has_bus = !strcmp(ep[i].extensionName, VK_EXT_PCI_BUS_INFO_EXTENSION_NAME);
    free(ep);
    VkPhysicalDevicePCIBusInfoPropertiesEXT bus = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PCI_BUS_INFO_PROPERTIES_EXT};
    VkPhysicalDeviceIDProperties id = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES, has_bus ? &bus : nullptr};
    VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &id};
    vkGetPhysicalDeviceProperties2(t->phys, &p2);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(t->phys, &mp);
    uint64_t vram = 0;
    for (uint32_t h = 0; h < mp.memoryHeapCount; h++)
        if (mp.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) vram += mp.memoryHeaps[h].size;
    perfmon_set_gpu(p2.properties.deviceName, id.deviceLUIDValid ? id.deviceLUID : NULL, has_bus ? (int)bus.pciBus : -1,
                    (float)(vram >> 20));
}

static void perf_tick(uint64_t now, const MenuTarget *t) {
    if (cfg_version() != g_perf_ver) {
        g_perf_ver = cfg_version();
        int m = perf_mode_of(cfg_value("NX2SARIA_PERF_OVERLAY"));
        if (m != g_perf_mode) {
            g_perf_last_ns = 0, g_perf_text[0] = 0;
            if (m == 2) perf_gpu_once(t);
            perfmon_mode(m);
        }
        g_perf_mode = m;
    }
    if (g_perf_mode != 1) return;
    uint64_t d = now - g_perf_last_ns;
    if (!g_perf_last_ns || d > 2000000000ull) {
        g_perf_win_ns = g_perf_max_ns = 0;
        g_perf_frames = 0;
    } else {
        g_perf_win_ns += d;
        g_perf_frames++;
        if (d > g_perf_max_ns) g_perf_max_ns = d;
        if (g_perf_win_ns >= 500000000ull) {
            double out = gfx_output_fps();
            if (out > 0)
                snprintf(g_perf_text, sizeof g_perf_text, menu_text(UI_PERF_LINE_FPS_FG), g_perf_frames * 1e9 / g_perf_win_ns, out);
            else
                snprintf(g_perf_text, sizeof g_perf_text, menu_text(UI_PERF_LINE_FPS), g_perf_frames * 1e9 / g_perf_win_ns);
            g_perf_win_ns = g_perf_max_ns = 0;
            g_perf_frames = 0;
        }
    }
    g_perf_last_ns = now;
}

static void perf_picture(const MenuTarget *t, PerfPanel *p) {
    unsigned out[2], scene[2];
    res_sizes(out, scene);
    const double share = !res_adjustable() && wide_scene_settable() ? wide_scene_share() : 0;
    if (share > 0) {
        wide_picture(&out[0], &out[1]);
        scene[0] = (unsigned)lround(out[0] * share), scene[1] = (unsigned)lround(out[1] * share);
    }
    char (*v)[64] = p->picture;
    snprintf(v[0], sizeof v[0], menu_text(UI_MENU_OV_FMT_SIZE), t->width, t->height);
    snprintf(v[1], sizeof v[1], menu_text(UI_MENU_OV_FMT_SIZE), (unsigned)t->image_w, (unsigned)t->image_h);
    snprintf(v[2], sizeof v[2], menu_text(UI_MENU_OV_FMT_SCENE), scene[0], scene[1], scene[0] * 100.0 / out[0]);
    upscaling_name(v[3], sizeof v[3]);
    aa_name(v[4], sizeof v[4]);
    double hz = frame_step_limit_hz();
    snprintf(v[5], sizeof v[5], "%s", menu_text(UI_MENU_OV_NONE));
    if (hz > 0) snprintf(v[5], sizeof v[5], menu_text(UI_MENU_OV_FPS), hz);
    snprintf(v[6], sizeof v[6], "%s", menu_text(strcmp(cfg_value("NX2SARIA_VK_PRESENT"), "immediate") ? UI_MENU_ON : UI_MENU_OFF));
    v[7][0] = v[8][0] = 0;
    if (wide_available()) {
        const char *a = cfg_value("NX2SARIA_GAME_ASPECT");
        UiTextId aspect_label = !strcmp(a, "window") ? UI_MENU_GAME_ASPECT_WINDOW : !strcmp(a, "16:10")
                                ? UI_MENU_GAME_ASPECT_16_10 : !strcmp(a, "3:2") ? UI_MENU_GAME_ASPECT_3_2
                                : !strcmp(a, "21:9") ? UI_MENU_GAME_ASPECT_21_9 : !strcmp(a, "32:9")
                                ? UI_MENU_GAME_ASPECT_32_9 : UI_MENU_GAME_ASPECT_16_9;
        snprintf(v[7], sizeof v[7], "%s", menu_text(aspect_label));
        const char *r = cfg_value("NX2SARIA_GAME_RES");
        if (!strcmp(r, "window")) snprintf(v[8], sizeof v[8], "%s", menu_text(UI_MENU_GAME_RES_NATIVE));
        else snprintf(v[8], sizeof v[8], "%sp", r);
    }
    const char *hdr = cfg_value("NX2SARIA_HDR");
    snprintf(v[9], sizeof v[9], "%s", menu_text(!strcmp(hdr, "native") ? UI_MENU_HDR_NATIVE : cfg_bool("NX2SARIA_HDR") ? UI_MENU_HDR_TONEMAPPED : UI_MENU_FRAMEGEN_OFF));
    const char *fg = cfg_value("NX2SARIA_FRAMEGEN");
    const bool fsr_fg = !strcmp(fg, "fsr3") && vk_ffx_framegen_available();
    snprintf(v[10], sizeof v[10], "%s", fsr_fg ? (vk_fg4_active() ? fg4_label() : "AMD FSR 3.1")
                                      : !strcmp(fg, "dlss") ? "NVIDIA DLSS" : menu_text(UI_MENU_FRAMEGEN_OFF));
    if (!strcmp(fg, "dlss") && vk_dlssg_active())
        snprintf(v[10], sizeof v[10], "NVIDIA DLSS %dx",
                 vk_dlssg_multiplier_of(cfg_value("NX2SARIA_FRAMEGEN_MULTIPLIER"), vk_dlssg_multiplier_max(nullptr), nullptr));
    v[11][0] = 0;
    if (cfg_preset_in_use() >= 0) {
        snprintf(v[11], sizeof v[11], "%s", preset_tier_text(cfg_preset_in_use()));
        if (cfg_preset_custom()) snprintf(v[11], sizeof v[11], menu_text(UI_MENU_PRESET_CUSTOM), preset_tier_text(cfg_preset_in_use()));
    }
    p->scene_known = res_adjustable() != 0;
}

static void perf_full_draw(const MenuTarget *t, float scale) {
    static PerfPanel p;
    perfmon_snapshot(&p.s);
    static char ver[96];
    static const char *ver_fmt;
    if (ver_fmt != menu_text(UI_MENU_VERSION)) {
        ver_fmt = menu_text(UI_MENU_VERSION);
        char game[16];
        snprintf(game, sizeof game, "%d.%d.%d", rt_game_version / 100, rt_game_version / 10 % 10, rt_game_version % 10);
        snprintf(ver, sizeof ver, ver_fmt, rt_port_version, game);
    }
    p.version = ver;
    perf_picture(t, &p);
    p.fg_fps = gfx_output_fps() > 0 ? (float)gfx_output_fps() : 0;
    p.under_menu = g_open.load(std::memory_order_relaxed) != 0;
    perf_panel_draw(&p, (float)t->width, scale);
}

static void perf_draw(const MenuTarget *t, float scale) {
    notice_draw(scale);
    if (g_perf_mode == 2) return perf_full_draw(t, scale);
    float w = (float)t->width;
    if (!g_perf_mode || !g_perf_text[0]) return;
    ImDrawList *dl = g_open.load(std::memory_order_relaxed) ? ImGui::GetBackgroundDrawList() : ImGui::GetForegroundDrawList();
    ImVec2 sz = ImGui::CalcTextSize(g_perf_text);
    float padx = 14 * scale, pady = 9 * scale;
    ImVec2 p(w - 12 * scale - sz.x - padx * 2, 12 * scale);
    skin_panel(dl, p, ImVec2(p.x + sz.x + padx * 2, p.y + sz.y + pady * 2), scale * 0.6f, 0.9f);
    dl->AddText(ImVec2(p.x + padx, p.y + pady), SKIN_TEXT, g_perf_text);
}

static void capture_note(const MenuTarget *t, float scale) {
    int number = 0, ok = 0;
    char name[64], line1[160], line2[160] = "";
    if (!bugcap_note(&number, &ok, name)) return;
    snprintf(line1, sizeof line1, menu_text(ok ? UI_MENU_CAPTURE_SAVED : UI_MENU_CAPTURE_FAILED), number);
    if (ok) snprintf(line2, sizeof line2, menu_text(UI_MENU_CAPTURE_FOLDER), name);
    ImDrawList *dl = ImGui::GetForegroundDrawList();
    ImVec2 s1 = ImGui::CalcTextSize(line1), s2 = ImGui::CalcTextSize(line2);
    float padx = 16 * scale, pady = 10 * scale, gap = line2[0] ? 4 * scale : 0;
    float w = (s1.x > s2.x ? s1.x : s2.x) + padx * 2, h = s1.y + (line2[0] ? s2.y : 0) + gap + pady * 2;
    ImVec2 p(((float)t->width - w) * 0.5f, 12 * scale);
    skin_panel(dl, p, ImVec2(p.x + w, p.y + h), scale * 0.6f, 0.92f);
    dl->AddText(ImVec2(p.x + (w - s1.x) * 0.5f, p.y + pady), SKIN_TEXT, line1);
    if (line2[0]) dl->AddText(ImVec2(p.x + (w - s2.x) * 0.5f, p.y + pady + s1.y + gap), SKIN_TEXT_DIM, line2);
}

static PauseTabView g_pause_seen;

static int overlay_frame(uint64_t frame, const MenuTarget *t, const char **shot) {
    *shot = NULL;
    if (t->phys && t->phys != g_gpu_vendor_of) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(t->phys, &props);
        g_gpu_vendor.store(props.vendorID, std::memory_order_relaxed);
        g_gpu_vendor_of = t->phys;
    }
    menu_art_start();
    cheats_frame(frame);
    if (!g_test.parsed) test_parse();
    if (g_test.next_set < g_test.n_sets && frame >= g_test.sets[g_test.next_set].frame) test_apply_sets(frame);
    for (int n = g_perf_cycle.exchange(0); n > 0; n--) perf_cycle_once();
    cfg_reload_tick();
    perf_tick(plat_now_ns(), t);
    amiibo_tick();
    const bool picker_wanted = amiibo_picker_wanted();
    int cap_number, cap_ok;
    char cap_name[64];
    const bool capture_shows = bugcap_note(&cap_number, &cap_ok, cap_name) != 0;
    if (g_booting.load(std::memory_order_relaxed)) g_booting.store(0, std::memory_order_release);
    preset_notice_tick(capture_shows || g_open.load(std::memory_order_relaxed));
    vram_warn_tick(t, capture_shows || amiibo_note_shows());
    bool open = g_open.load(std::memory_order_relaxed);
    static bool was_open;
    bool opened = open && !was_open;
    was_open = open;
    if (!open) {
        g_close_req.store(0);
        g_apply_ask = false;
        if (opt_display_pending()) {
            opt_display_revert();
            gfx_window_apply_settings();
        }
        g_post = POST_NONE;
        g_restart_lines.clear();
        if (opt_draft_any()) opt_draft_discard();
        if (g_test.next_at < g_test.n_at && !g_test.opened && frame >= g_test.at[g_test.next_at]) {
            g_test.opened = frame;
            g_test.page = 0;
            g_tab_request = 0;
            g_debug.store(cfg_get("NX2SARIA_MENU_TEST_DEBUG") != NULL);
            g_picker.store(0);
            if (g_debug.load()) debug_menu_tab(0);
            set_open(1);
            open = true;
        } else if (picker_wanted) {
            g_debug.store(0);
            g_picker.store(1);
            amiibo_picker_begin();
            set_open(1);
            open = true;
        } else if (pause_tab_request_take()) {
            g_debug.store(0);
            g_picker.store(0);
            g_pause_frames = 0;
            g_pause_why = "closed";
            open = change_open(2) != 0;
        } else if (!g_perf_mode && !g_notice_until_ns && !capture_shows && !amiibo_note_shows() && !pause_tab_view(&g_pause_seen)) {
            return 0;
        }
    }
    if (g_failed_flag()) {
        set_open(0);
        return 0;
    }
    uint64_t t0 = plat_now_ns();
    if (!g_ctx_ok) make_context();
    float scale = menu_scale(t->width, t->height);
    if (!vk_up(t) || !fonts_for(scale, t0)) {
        rt_log("menu: Dear ImGui Vulkan back end set-up failed; the menu stays closed");
        g_failed.store(1);
        set_open(0);
        return 0;
    }
    if (g_style_scale != scale) {
        ImGui::GetStyle() = g_base_style;
        ImGui::GetStyle().ScaleAllSizes(scale);
        g_style_scale = scale;
    }
    list_gpus(t->instance);
    ImGuiIO &io = ImGui::GetIO();
    int reset = g_reset.exchange(0);
    open = g_open.load(std::memory_order_acquire);
    if (reset) {
        io.ClearEventsQueue();
        io.ClearInputKeys();
        io.ClearInputMouse();
        binds_cancel();
        g_sound_fresh = true;
        g_last_ns = 0;
        g_focus_page = g_tab_request >= 0 ? g_tab_request : g_tab;
        if (GImGui->OpenPopupStack.Size) ImGui::ClosePopupToLevel(0, false);
        g_modal = false;
    }
    if (reset) g_close_req.store(0);
    if (open && g_close_req.exchange(0)) {
        rt_log("menu: leave requested by F1 or Minus+Plus with a draft open");
        leave_menu();
        open = g_open.load(std::memory_order_acquire);
    }
    io.DisplaySize = ImVec2((float)t->width, (float)t->height);
    double dt = g_last_ns ? (t0 - g_last_ns) / 1e9 : 1 / 60.0;
    io.DeltaTime = (float)(dt < 1e-4 ? 1e-4 : dt > 0.1 ? 0.1 : dt);
    g_last_ns = t0;
    if (!open) {
    } else if (g_test.page < 0) {
        drain_events(io);
        pad_events(io, reset || opened);
    } else {
        plat_mutex_lock(&ev_lock);
        ev_n = 0;
        plat_mutex_unlock(&ev_lock);
    }
    ImGui::NewFrame();
    if (open && g_debug.load(std::memory_order_relaxed)) {
        debug_menu_layout(t->width, t->height, scale);
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || (!io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)))
            set_open(0);
    } else if (open && g_picker.load(std::memory_order_relaxed)) {
        if (!amiibo_picker_layout(t->width, t->height, scale)) set_open(0);
    } else if (open && g_pause_tab.load(std::memory_order_relaxed)) {
        layout_pause_tab(t->width, t->height, scale);
    } else if (open) {
        layout_menu(t->width, t->height, scale);
    }
    const bool tab_mode = g_pause_tab.load(std::memory_order_relaxed) != 0;
    if (pause_tab_on() && (!open || tab_mode)) pause_tab_header(t->width, t->height, scale, open && tab_mode);
    perf_draw(t, scale);
    capture_note(t, scale);
    amiibo_note_draw(t->width, scale);
    ImGui::Render();
    g_cost_ns += plat_now_ns() - t0;
    if (g_test.page >= 0 && frame >= g_test.opened + g_test.per_page * (uint64_t)(g_test.page + 1)) {
        static char path[512];
        const char *dir = cfg_get("NX2SARIA_MENU_TEST_DIR");
        snprintf(path, sizeof path, "%s/menu-test-%llu-%d.png", dir ? dir : ".", (unsigned long long)frame, g_test.page);
        *shot = path;
        if (++g_test.page == (g_debug.load() ? DEBUG_MENU_TABS : tabs_shown())) {
            g_test.page = -1;
            g_test.opened = 0, g_test.next_at++;
            set_open(0);
        } else {
            g_tab_request = g_test.page;
            if (g_debug.load()) debug_menu_tab(g_test.page);
        }
    }
    return ImGui::GetDrawData()->TotalVtxCount > 0;
}

extern "C" int menu_overlay_frame(uint64_t frame, const MenuTarget *t, const char **shot) {
    static bool bind_labels_logged;
    if (!bind_labels_logged) {
        rt_log("menu: bind action labels on (%d rows; default B/X; Swap Jump Controls not read)", binds_action_row_count());
        bind_labels_logged = true;
    }
    int r = overlay_frame(frame, t, shot);
    menu_text_input_follow(g_ctx_ok && g_open.load(std::memory_order_relaxed) && ImGui::GetIO().WantTextInput);
    return r;
}

static std::atomic<int> g_boot_phase;
static std::atomic<long long> g_boot_done, g_boot_total;
static uint64_t g_boot_t0;

extern "C" void menu_boot_set(int phase, long long done, long long total) {
    g_boot_done.store(done, std::memory_order_relaxed);
    g_boot_total.store(total, std::memory_order_relaxed);
    g_boot_phase.store(phase, std::memory_order_release);
}

extern "C" int menu_boot_frame(const MenuTarget *t) {
    if (g_failed_flag() || g_boot_phase.load(std::memory_order_acquire) < 0) return 0;
    if (!g_ctx_ok) make_context();
    float scale = menu_scale(t->width, t->height);
    if (!vk_up(t) || !fonts_for(scale, plat_now_ns())) {
        rt_log("menu: Dear ImGui Vulkan back end set-up failed; no boot screen, and the menu stays closed");
        g_failed.store(1);
        return 0;
    }
    menu_art_start();
    if (g_style_scale != scale) {
        ImGui::GetStyle() = g_base_style;
        ImGui::GetStyle().ScaleAllSizes(scale);
        g_style_scale = scale;
    }
    uint64_t now = plat_now_ns();
    if (!g_boot_t0) g_boot_t0 = now;
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)t->width, (float)t->height);
    double dt = g_last_ns ? (now - g_last_ns) / 1e9 : 1 / 60.0;
    io.DeltaTime = (float)(dt < 1e-4 ? 1e-4 : dt > 0.1 ? 0.1 : dt);
    g_last_ns = now;
    ImGui::NewFrame();

    int phase = g_boot_phase.load(std::memory_order_acquire);
    long long done = g_boot_done.load(std::memory_order_relaxed), total = g_boot_total.load(std::memory_order_relaxed);
    char step[160];
    static const UiTextId steps[] = {UI_BOOT_SHADERS, UI_BOOT_PIPELINES, UI_BOOT_COMPUTE, UI_BOOT_DRIVER_CACHE};
    if (phase >= 1 && phase <= 4)
        snprintf(step, sizeof step, ui_text(steps[phase - 1]), done, total);
    else
        snprintf(step, sizeof step, "%s", ui_text(UI_BOOT_STARTING));
    const char *title = ui_text(UI_BOOT_TITLE);

    ImFont *body = ImGui::GetFont();
    float body_px = ImGui::GetFontSize();
    ImFont *head = g_title_font ? g_title_font : body;
    float head_px = g_title_font ? g_title_font->FontSize * io.FontGlobalScale : body_px * 1.2f;
    float frac = -1;
    if (phase >= 1 && phase <= 4 && total > 0) frac = done <= 0 ? 0 : done >= total ? 1 : (float)done / (float)total;
    float s = (float)fmod((now - g_boot_t0) / 1e9 * 0.8, 2.0);
    skin_boot_panel(ImGui::GetForegroundDrawList(), head, head_px, body, body_px, title, step, frac, s < 1 ? s : 2 - s,
                    (float)t->width, (float)t->height, scale);
    ImGui::Render();
    return ImGui::GetDrawData()->TotalVtxCount > 0;
}

extern "C" void menu_overlay_draw(VkCommandBuffer cb) {
    uint64_t t0 = plat_now_ns();
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cb);
    g_cost_ns += plat_now_ns() - t0;
    if (++g_cost_frames == 600) {
        rt_log("menu: %.3f ms CPU per open frame (layout and recording, mean of 600)", g_cost_ns / 600 / 1e6);
        g_cost_ns = g_cost_frames = 0;
    }
}
