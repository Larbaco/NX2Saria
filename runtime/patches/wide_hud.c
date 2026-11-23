#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zstd.h>

#include "host.h"
#include "platform/platform.h"
#include "sarc.h"
#include "wide_hud.h"

#define MAX_RAW (64u << 20)
enum { PANE_TABLE = 0x118, ANIM_TABLE = 0x238 };
enum { PANE_SCALE_X = 0x10, PANE_ROTATE = 0x18, PANE_X = 0x1c, PANE_PARENT = 0x28, PANE_W = 0x2c, PANE_NEEDS = 0x30 };
enum { ANIM_TRACKS = 0x08, ANIM_CURVES = 0x18, ANIM_COMPONENTS = 0x28, ANIM_CONSTANTS = 0x38, ANIM_CONST_COMPONENTS = 0x48 };
enum { NO_INDEX = 0xffff, PANE_KIND = 7, COMPONENT_X = 0, MAX_PANES = 128 };
enum { PROP_SCALE = 0, PROP_TRANSLATE = 1, PROP_SIZE = 0x102 };

enum { G_HUD = 1, G_FULL = 2, G_CENTER = 4 };

enum { OP_MOVE, OP_MOVE_IN, OP_GROW, OP_WIDEN, OP_SQUEEZE, OP_PIN };
typedef struct {
    const char *pane;
    int op, group;
    float f;
    float x, sx, w;
} PaneOp;

typedef struct {
    float value, f;
    int at, group;
} KeyMap;
enum { MAX_MAP = 4 };
typedef struct {
    const char *anim, *pane;
    int prop, keys;
    KeyMap map[MAX_MAP];
    float slope_f;
} KeyOp;

typedef struct {
    const char *layout;
    const PaneOp *ops;
    int nops;
    const KeyOp *keys;
    int nkeys;
    int archive;
} Plan;

static const PaneOp player_status[] = {
    {"N_ScaleL_00", OP_MOVE, G_HUD, -1, -1920, 1, 0},           {"N_ScaleR_00", OP_MOVE, G_HUD, 1, 1920, 1, 0},
    {"N_SubItemBaseFishing_00", OP_MOVE, G_HUD, 1, 1270, 1, 0}, {"N_VRS_00", OP_MOVE, G_HUD, 1, 1626, 1, 0}};
static const PaneOp map_menu[] = {{"N_MiniMap_00", OP_MOVE, G_HUD, 1, 0, 1, 0},
                                  {"N_EventEndIn_01", OP_MOVE, G_HUD, 1, 0, 1, 0},
                                  {"N_MiniMapCap_00", OP_MOVE, G_HUD, 1, 1470, 1, 0},
                                  {"N_VRS_00", OP_MOVE, G_HUD, 1, 1536, 1, 0},
                                  {"Pa_KeySmall_00", OP_MOVE, G_HUD, 1, 0, 1, 0},
    {"P_MapBase_00", OP_GROW, G_FULL, 1, 0, 1, 3928},
    {"P_NotOpen_00", OP_GROW, G_FULL, 1, 0, 1, 4080},
    {"BG_00", OP_GROW, G_FULL, 1, 0, 1, 3840},
    {"N_FloorTab_00", OP_MOVE, G_FULL, -1, -1740, 1, 0},
    {"T_MapTitle_00", OP_MOVE, G_FULL, -1, -1702, 1, 0},
    {"N_ItemIcon_00", OP_MOVE, G_FULL, 1, 1666, 1, 0},
    {"P_PicField_00", OP_MOVE, G_FULL, 1, 1600, 1, 0},
    {"Pa_Guide_01", OP_MOVE, G_FULL, 1, 1796, 1, 0},
    {"P_Vignette_L_00", OP_MOVE, G_FULL, -1, -2040, 1, 0},
    {"P_Vignette_R_00", OP_MOVE, G_FULL, 1, 2040, 1, 0},
    {"P_Overlay_00", OP_GROW, G_FULL, 1, 0, 1, 3840},};
static const KeyOp map_menu_keys[] = {
    {"MapType", "N_MapIcon_00", PROP_TRANSLATE, 7, {{1490, 1, 2, G_HUD}}, 0},
    {"DungeonMiniMapPattern", "N_PatternDungeonMini_01", PROP_TRANSLATE, 21, {{1536, 1, 19, G_HUD}}, 0}};
static const PaneOp num_display[] = {{"N_NumDisplayFirst_00", OP_MOVE, G_HUD, 1, 0, 1, 0},
                                     {"N_NumDisplaySecond_00", OP_MOVE, G_HUD, 1, 0, 1, 0}};
static const PaneOp loading_fade[] = {{"N_IconIn_00", OP_SQUEEZE, G_CENTER, 0, 1778, 1, 0},
    {"N_IconIn_00", OP_PIN, G_FULL, 0, 1778, 1, 0},};
static const PaneOp menu_overlay_full[] = {
    {"P_MenuHeader_00", OP_GROW, G_FULL, 1, 0, 1, 3840},
    {"Pa_HeartGauge_00", OP_MOVE, G_FULL, -1, -1352, 1, 0},
    {"Pa_MpGauge_00", OP_MOVE, G_FULL, -1, -1580, 1, 0},
    {"A_Rupee_00", OP_MOVE, G_FULL, 1, 1652, 1, 0},
    {"P_ReplaceIcon_00", OP_MOVE, G_FULL, 1, 1792, 1, 0},
    {"W_GuideBg_00", OP_MOVE, G_FULL, 1, 2110, 1, 0},
    {"W_GuideBg_01", OP_MOVE, G_FULL, 1, 2110, 1, 0},
    {"A_FirstGuide_00", OP_MOVE, G_FULL, 1, 340, 1, 0},
};
static const PaneOp plus_menu_full[] = {
    {"Pa_Equip_00", OP_MOVE, G_FULL, 2, 4280, 1, 0},
    {"Pa_Item_00", OP_MOVE, G_FULL, 4, 8560, 1, 0},
    {"Pa_System_00", OP_MOVE, G_FULL, 6, 12840, 1, 0},
    {"N_Pos_00", OP_MOVE, G_FULL, 2, 4195, 1, 0},
    {"N_Pos_01", OP_MOVE, G_FULL, 4, 8560, 1, 0},
    {"P_InfoBg_00", OP_MOVE, G_FULL, -0.5, -1079, 1, 0},
    {"P_InfoBg_00", OP_GROW, G_FULL, 0.5, 0, 1, 1682},
    {"P_InfoBg_01", OP_MOVE, G_FULL, 2, 4186, 1, 0},
    {"P_InfoBg_02", OP_MOVE, G_FULL, 4.5, 9660, 1, 0},
    {"P_InfoBg_02", OP_GROW, G_FULL, 0.5, 0, 1, 1742},
    {"P_InfoBg_03", OP_MOVE, G_FULL, 5, 10681, 1, 0},
    {"P_MenuBgAmiibo_00", OP_MOVE, G_FULL, 1, 2932, 1, 0},
    {"P_MenuBgArrow_00", OP_MOVE, G_FULL, 3, 7876, 1, 0},
    {"P_SystemBg_00", OP_MOVE, G_FULL, 5, 11512, 1, 0},
    {"P_HeaderSh_00", OP_GROW, G_FULL, 1, 0, 1, 3840},
    {"P_Black_00", OP_GROW, G_FULL, 1, 0, 1, 3840},
    {"P_Equipment3DFade_00", OP_GROW, G_FULL, 1, 0, 1, 3840},
};
static const KeyOp plus_menu_full_keys[] = {
    {"CollectToEquip", "N_MenuChange_00", PROP_TRANSLATE, 3, {{-3020, -1.411215, 1, G_FULL}, {-4280, -2, 1, G_FULL}}, 0.8971968},
    {"CollectToEquip", "N_MenuChange_01", PROP_TRANSLATE, 3, {{-3020, -1.411215, 1, G_FULL}, {-4280, -2, 1, G_FULL}}, 0.8971968},
    {"EquipToItem", "N_MenuChange_00", PROP_TRANSLATE, 3, {{-4280, -2, 1, G_FULL}, {-7300, -3.411215, 1, G_FULL}, {-8560, -4, 1, G_FULL}}, 0.8971968},
    {"EquipToItem", "N_MenuChange_01", PROP_TRANSLATE, 3, {{-4280, -2, 1, G_FULL}, {-7300, -3.411215, 1, G_FULL}, {-8560, -4, 1, G_FULL}}, 0.8971968},
    {"ItemToSystem", "N_MenuChange_00", PROP_TRANSLATE, 3, {{-8560, -4, 1, G_FULL}, {-11580, -5.411215, 1, G_FULL}, {-12840, -6, 1, G_FULL}}, 0.8971968},
    {"ItemToSystem", "N_MenuChange_01", PROP_TRANSLATE, 3, {{-8560, -4, 1, G_FULL}, {-11580, -5.411215, 1, G_FULL}, {-12840, -6, 1, G_FULL}}, 0.8971968},
    {"SystemToItem", "N_MenuChange_00", PROP_TRANSLATE, 3, {{-12840, -6, 1, G_FULL}, {-9820, -4.588785, 1, G_FULL}, {-8560, -4, 1, G_FULL}}, 0.8971968},
    {"SystemToItem", "N_MenuChange_01", PROP_TRANSLATE, 3, {{-12840, -6, 1, G_FULL}, {-9820, -4.588785, 1, G_FULL}, {-8560, -4, 1, G_FULL}}, 0.8971968},
    {"ItemToEquip", "N_MenuChange_00", PROP_TRANSLATE, 3, {{-8560, -4, 1, G_FULL}, {-5540, -2.588785, 1, G_FULL}, {-4280, -2, 1, G_FULL}}, 0.8971968},
    {"ItemToEquip", "N_MenuChange_01", PROP_TRANSLATE, 3, {{-8560, -4, 1, G_FULL}, {-5540, -2.588785, 1, G_FULL}, {-4280, -2, 1, G_FULL}}, 0.8971968},
    {"EquipToCollect", "N_MenuChange_00", PROP_TRANSLATE, 3, {{-4280, -2, 1, G_FULL}, {-1260, -0.588785, 1, G_FULL}}, 0.8971968},
    {"EquipToCollect", "N_MenuChange_01", PROP_TRANSLATE, 3, {{-4280, -2, 1, G_FULL}, {-1260, -0.588785, 1, G_FULL}}, 0.8971968},
};
static const PaneOp pa_collect_menu_full[] = {
    {"N_ItemAll_00", OP_MOVE, G_FULL, 1, 0, 1, 0},
    {"N_Arrow_00", OP_MOVE, G_FULL, 1, 1850, 1, 0},
};
static const KeyOp pa_collect_menu_full_keys[] = {
    {"Loop", "N_Arrow_00", PROP_TRANSLATE, 3, {{1850, 1, 2, G_FULL}, {1858, 1, 1, G_FULL}}, 0},
};
static const PaneOp pa_equipment_menu_full[] = {
    {"Pa_Epuipment_00", OP_MOVE, G_FULL, 1, -42, 1, 0},
    {"Pa_Epuipment_01", OP_MOVE, G_FULL, 1, -402, 1, 0},
    {"Pa_Epuipment_02", OP_MOVE, G_FULL, 1, -762, 1, 0},
    {"Pa_Epuipment_03", OP_MOVE, G_FULL, 1, -38, 1, 0},
    {"Pa_Epuipment_04", OP_MOVE, G_FULL, 1, -400, 1, 0},
    {"Pa_Epuipment_05", OP_MOVE, G_FULL, 1, -760, 1, 0},
    {"Pa_Epuipment_06", OP_MOVE, G_FULL, 1, -40, 1, 0},
    {"Pa_Epuipment_07", OP_MOVE, G_FULL, 1, -400, 1, 0},
    {"Pa_Epuipment_08", OP_MOVE, G_FULL, 1, -762, 1, 0},
    {"Pa_HeadEquip_00", OP_MOVE, G_FULL, -1, -3026, 1, 0},
    {"Pa_HeadEquip_01", OP_MOVE, G_FULL, -1, -2806, 1, 0},
    {"Pa_HeadEquip_02", OP_MOVE, G_FULL, -1, -3248, 1, 0},
    {"Pa_HeadEquip_03", OP_MOVE, G_FULL, -1, -3030, 1, 0},
    {"Pa_FootEquip_00", OP_MOVE, G_FULL, -1, -3248, 1, 0},
    {"Pa_FootEquip_01", OP_MOVE, G_FULL, -1, -2810, 1, 0},
    {"P_AmiiboIcon_00", OP_MOVE, G_FULL, -1, -1358, 1, 0},
    {"N_Arrow_00", OP_MOVE, G_FULL, -1, -1850, 1, 0},
    {"N_Arrow_01", OP_MOVE, G_FULL, 1, 1850, 1, 0},
};
static const KeyOp pa_equipment_menu_full_keys[] = {
    {"Loop", "N_Arrow_00", PROP_TRANSLATE, 3, {{-1850, -1, 2, G_FULL}, {-1858, -1, 1, G_FULL}}, 0},
    {"Loop", "N_Arrow_01", PROP_TRANSLATE, 3, {{1850, 1, 2, G_FULL}, {1858, 1, 1, G_FULL}}, 0},
};
static const PaneOp pa_item_menu_full[] = {
    {"N_ItemAll_00", OP_MOVE, G_FULL, -1, 0, 1, 0},
    {"N_Arrow_00", OP_MOVE, G_FULL, -1, -1850, 1, 0},
};
static const KeyOp pa_item_menu_full_keys[] = {
    {"Loop", "N_Arrow_00", PROP_TRANSLATE, 3, {{-1850, -1, 2, G_FULL}, {-1858, -1, 1, G_FULL}}, 0},
};
static const PaneOp pa_system_menu_full[] = {
    {"N_Element_00", OP_MOVE, G_FULL, -1, -1330, 1, 0},
    {"Pa_Button_00", OP_MOVE, G_FULL, -1, -1330, 1, 0},
    {"Pa_Button_01", OP_MOVE, G_FULL, -1, -1330, 1, 0},
    {"Pa_Button_02", OP_MOVE, G_FULL, -1, -1330, 1, 0},
    {"Pa_Button_03", OP_MOVE, G_FULL, -1, -1330, 1, 0},
    {"Pa_Button_04", OP_MOVE, G_FULL, -1, -1330, 1, 0},
    {"Pa_SystemController_00", OP_MOVE, G_FULL, 1, 540, 1, 0},
};
static const PaneOp important_menu1_full[] = {
    {"W_Window_00", OP_GROW, G_FULL, 1, 0, 1, 4080},
    {"P_NotOpen_00", OP_GROW, G_FULL, 1, 0, 1, 4080},
    {"P_Overlay_00", OP_GROW, G_FULL, 1, 0, 1, 4080},
};
static const PaneOp challenge_menu1_full[] = {
    {"P_TapestryMask_00", OP_MOVE, G_FULL, 1, 2040, 1, 0},
    {"P_TapestryMask_00", OP_GROW, G_FULL, 1, 0, 1, 4080},
    {"P_TapestryMask_01", OP_GROW, G_FULL, 1, 0, 1, 3840},
    {"P_ChangeFade_00", OP_GROW, G_FULL, 1, 0, 1, 4080},
    {"C_CaptureD_00", OP_GROW, G_FULL, 1, 0, 1, 3840},
    {"P_MaskD_00", OP_GROW, G_FULL, 1, 0, 1, 3840},
    {"C_CaptureU_01", OP_MOVE, G_FULL, -1, -1920, 1, 0},
    {"C_CaptureD_01", OP_MOVE, G_FULL, -1, -1920, 1, 0},
    {"P_MaskU_01", OP_MOVE, G_FULL, -1, -1920, 1, 0},
    {"P_MaskD_01", OP_MOVE, G_FULL, -1, -1920, 1, 0},
    {"Pa_IndexSlideList_00", OP_MOVE, G_FULL, -1, -1334, 1, 0},
    {"Pa_Info_00", OP_MOVE, G_FULL, -1, -658, 1, 0},
    {"W_Vignette_00", OP_GROW, G_FULL, 1, 0, 1, 4080},
    {"W_Vignette_01", OP_GROW, G_FULL, 1, 0, 1, 4080},
    {"P_NotOpen_00", OP_GROW, G_FULL, 1, 0, 1, 4080},
    {"P_Overlay_00", OP_GROW, G_FULL, 1, 0, 1, 3840},
};
static const PaneOp short_cut1_full[] = {
    {"P_Bg_00", OP_GROW, G_FULL, 1, 0, 1, 3840},
    {"C_CaptureL_00", OP_MOVE, G_FULL, -1, -1776, 1, 0},
    {"P_MaskL_00", OP_MOVE, G_FULL, -1, -1776, 1, 0},
    {"C_CaptureR_00", OP_MOVE, G_FULL, 1, 1776, 1, 0},
    {"P_MaskR_00", OP_MOVE, G_FULL, 1, 1776, 1, 0},
    {"Pa_Guide_02", OP_MOVE, G_FULL, 1, 1795, 1, 0},
    {"Pa_SlideListB_00", OP_GROW, G_FULL, 1, 0, 1, 3840},
};
static const PaneOp pa_short_cut_slide_list_full[] = {
    {"S_ScissorHT_00", OP_GROW, G_FULL, 1, 0, 1, 3840},
    {"N_SlideList_00", OP_GROW, G_FULL, 1, 0, 1, 3840},
};
static const PaneOp system_overlay_full[] = {
    {"P_UpSh_00", OP_GROW, G_FULL, 1, 0, 1, 3840},
    {"P_BottomSh_00", OP_GROW, G_FULL, 1, 0, 1, 3840},
    {"P_SystemHeader_00", OP_GROW, G_FULL, 1, 0, 1, 3840},
    {"W_GuideBg_00", OP_MOVE, G_FULL, 1, 2110, 1, 0},
    {"W_GuideBg_01", OP_MOVE, G_FULL, 1, 2174.18, 1, 0},
    {"A_FirstGuide_00", OP_MOVE, G_FULL, 1, 340, 1, 0},
};
static const PaneOp system_option_full[] = {
    {"N_List_00", OP_MOVE, G_FULL, -1, -730, 1, 0},
    {"N_Caption_00", OP_MOVE, G_FULL, 1, 1134, 1, 0},
};
static const PaneOp system_action_guide_full[] = {
    {"S_List_00", OP_MOVE, G_FULL, -1, -1060, 1, 0},
    {"N_Tips_00", OP_MOVE, G_FULL, 1, 800, 1, 0},
};
static const KeyOp system_action_guide_full_keys[] = {
    {"Pattern", "S_List_00", PROP_TRANSLATE, 2, {{-1060, -1, 2, G_FULL}}, 1.81132032},
};
static const PaneOp game_over_full[] = {
    {"C_CaptureBG_00", OP_GROW, G_FULL, 1, 0, 1, 3840},
    {"P_CaptureBG_00", OP_GROW, G_FULL, 1, 0, 1, 3840},
    {"P_BlackBG_00", OP_GROW, G_FULL, 1, 0, 1, 3840},
    {"C_CaptureBlur_00", OP_GROW, G_FULL, 1, 0, 1, 3140},
    {"C_CaptureBlur_01", OP_GROW, G_FULL, 1, 0, 1, 3140},
    {"P_CaptureBlur_00", OP_GROW, G_FULL, 1, 0, 1, 3140},
    {"P_CaptureBlur_01", OP_GROW, G_FULL, 1, 0, 1, 3140},
    {"P_CaptureBlur_Effect_00", OP_GROW, G_FULL, 1, 0, 1, 3140},
    {"P_CaptureBlur_Effect_01", OP_GROW, G_FULL, 1, 0, 1, 3140},
};
static const PaneOp message_log1_full[] = {
    {"C_Capture_00", OP_GROW, G_FULL, 1, 0, 1, 3840},
    {"P_Capture_00", OP_GROW, G_FULL, 1, 0, 1, 3840},
    {"P_Noise_00", OP_GROW, G_FULL, 1, 0, 1, 3840},
    {"N_Scroll_00", OP_MOVE, G_FULL, -1, -1872, 1, 0},
    {"A_GuideAll_00", OP_MOVE, G_FULL, 1, 1840, 1, 0},
};
static const PaneOp staff_roll_end_full[] = {
    {"C_Capture_00", OP_GROW, G_FULL, 1, 0, 1, 3840},
    {"C_Capture_01", OP_GROW, G_FULL, 1, 0, 1, 3840},
    {"P_Capture_01", OP_GROW, G_FULL, 1, 0, 1, 3840},
    {"P_Fade_00", OP_GROW, G_FULL, 1, 0, 1, 3840},
    {"P_Fade_01", OP_GROW, G_FULL, 1, 0, 1, 3840},
    {"P_Capture_02", OP_GROW, G_FULL, 1, 0, 1, 3840},
    {"W_GuideBg_00", OP_MOVE, G_FULL, 1, 2110, 1, 0},
    {"Pa_Guide_00", OP_MOVE, G_FULL, 1, 1795, 1, 0},
};
static const PaneOp title_menu_bg_full[] = {
    {"C_Capture_00", OP_GROW, G_FULL, 1, 0, 1, 3840},
    {"P_Capture_00", OP_GROW, G_FULL, 1, 0, 1, 3840},
    {"P_Noise_00", OP_GROW, G_FULL, 1, 0, 1, 3840},
};
#define N(a) (int)(sizeof a / sizeof *a)
static const Plan plans[] = {
    {"PlayerStatus_00", player_status, N(player_status), NULL, 0, WIDE_HUD_COMMON},
    {"MapMenu_00", map_menu, N(map_menu), map_menu_keys, N(map_menu_keys), WIDE_HUD_COMMON},
    {"NumDisplay_00", num_display, N(num_display), NULL, 0, WIDE_HUD_COMMON},
    {"LoadingFade_00", loading_fade, N(loading_fade), NULL, 0, WIDE_HUD_COMMON},
    {"MenuOverlay_00", menu_overlay_full, N(menu_overlay_full), NULL, 0, WIDE_HUD_COMMON},
    {"PlusMenu_00", plus_menu_full, N(plus_menu_full), plus_menu_full_keys, N(plus_menu_full_keys), WIDE_HUD_COMMON},
    {"PaCollectMenu_00", pa_collect_menu_full, N(pa_collect_menu_full), pa_collect_menu_full_keys, N(pa_collect_menu_full_keys), WIDE_HUD_COMMON},
    {"PaEquipmentMenu_00", pa_equipment_menu_full, N(pa_equipment_menu_full), pa_equipment_menu_full_keys, N(pa_equipment_menu_full_keys), WIDE_HUD_COMMON},
    {"PaItemMenu_00", pa_item_menu_full, N(pa_item_menu_full), pa_item_menu_full_keys, N(pa_item_menu_full_keys), WIDE_HUD_COMMON},
    {"PaSystemMenu_00", pa_system_menu_full, N(pa_system_menu_full), NULL, 0, WIDE_HUD_COMMON},
    {"ImportantMenu_01", important_menu1_full, N(important_menu1_full), NULL, 0, WIDE_HUD_COMMON},
    {"ChallengeMenu_01", challenge_menu1_full, N(challenge_menu1_full), NULL, 0, WIDE_HUD_COMMON},
    {"ShortCut_01", short_cut1_full, N(short_cut1_full), NULL, 0, WIDE_HUD_COMMON},
    {"PaShortCutSlideList_00", pa_short_cut_slide_list_full, N(pa_short_cut_slide_list_full), NULL, 0, WIDE_HUD_COMMON},
    {"SystemOverlay_00", system_overlay_full, N(system_overlay_full), NULL, 0, WIDE_HUD_COMMON},
    {"SystemOption_00", system_option_full, N(system_option_full), NULL, 0, WIDE_HUD_COMMON},
    {"SystemActionGuide_00", system_action_guide_full, N(system_action_guide_full), system_action_guide_full_keys, N(system_action_guide_full_keys), WIDE_HUD_COMMON},
    {"GameOver_00", game_over_full, N(game_over_full), NULL, 0, WIDE_HUD_COMMON},
    {"MessageLog_01", message_log1_full, N(message_log1_full), NULL, 0, WIDE_HUD_COMMON},
    {"StaffRollEnd_00", staff_roll_end_full, N(staff_roll_end_full), NULL, 0, WIDE_HUD_COMMON},
    {"TitleMenuBG_00", title_menu_bg_full, N(title_menu_bg_full), NULL, 0, WIDE_HUD_TITLE},
};
enum { NPLANS = sizeof plans / sizeof *plans };

typedef struct {
    double a, b, x0, x1, f0, f1;
} Piece;
typedef struct {
    const char *layout, *pane;
    int group;
    const Piece *pieces;
    int npieces;
} Split;
static const Piece menu_bg_pieces[] = {
    {1880, 2010, -320, -320, 0, 1},
    {-320, 3380, -320, 3380, 1, 1},
    {1880, 2010, 3380, 3380, 1, 2},
    {3380, 4980, 3380, 4980, 2, 2},
    {1880, 2010, 4980, 4980, 2, 3},
    {4980, 8880, 4980, 8880, 3, 3},
    {1880, 2010, 8880, 8880, 3, 4},
};
static const Split splits[] = {{"PlusMenu_00", "P_MenuBg_00", G_FULL, menu_bg_pieces, N(menu_bg_pieces)}};
enum { NSPLITS = sizeof splits / sizeof *splits };

static const Split *split_of(const char *layout) {
    for (int i = 0; i < NSPLITS; i++)
        if (!strcmp(splits[i].layout, layout)) return &splits[i];
    return NULL;
}
enum { MAX_EDITS = 2048 };

float wide_hud_out(unsigned w, unsigned h) {
    if (!w || !h) return 0;
    double r = (double)w / h, wide = 16.0 / 9.0;
    return r < wide * 1.01 ? 0 : (float)(1080.0 * r - 1920.0);
}

static int groups_of(const WideHudShift *s) {
    return (s->out > 0 ? G_HUD : 0) | (s->menus > 0 ? G_FULL : 0) | (s->squeeze > 0 && s->squeeze < 1 ? G_CENTER : 0);
}
static float unit_of(int group, const WideHudShift *s) {
    return group == G_HUD ? s->out : group == G_FULL ? s->menus : s->squeeze;
}
static int key_groups(const KeyOp *k) {
    int g = 0;
    for (int i = 0; i < MAX_MAP && k->map[i].at > 0; i++) g |= k->map[i].group;
    return g;
}
static int plan_groups(const Plan *plan) {
    int g = 0;
    for (int i = 0; i < plan->nops; i++) g |= plan->ops[i].group;
    for (int i = 0; i < plan->nkeys; i++) g |= key_groups(&plan->keys[i]);
    if (split_of(plan->layout)) g |= split_of(plan->layout)->group;
    return g;
}

int wide_hud_counts_of(int menus, WideHudCount *out, int max) {
    int want = menus ? G_FULL : G_HUD, k = 0;
    for (int i = 0; i < NPLANS && k < max; i++) {
        int panes = 0, keys = 0;
        for (int j = 0; j < plans[i].nops; j++) {
            int seen = 0;
            for (int q = 0; q < j && !seen; q++)
                seen = plans[i].ops[q].group == want && !strcmp(plans[i].ops[q].pane, plans[i].ops[j].pane);
            panes += plans[i].ops[j].group == want && !seen;
        }
        const Split *sp = split_of(plans[i].layout);
        if (sp && sp->group == want) {
            int seen = 0;
            for (int j = 0; j < plans[i].nops && !seen; j++)
                seen = plans[i].ops[j].group == want && !strcmp(plans[i].ops[j].pane, sp->pane);
            panes += !seen;
        }
        for (int j = 0; j < plans[i].nkeys; j++)
            for (int m = 0; m < MAX_MAP && plans[i].keys[j].map[m].at > 0; m++)
                if (plans[i].keys[j].map[m].group == want) keys += plans[i].keys[j].map[m].at;
        if (panes || keys) out[k++] = (WideHudCount){plans[i].layout, panes, keys};
    }
    return k;
}

int wide_hud_counts(WideHudCount *out, int max) { return wide_hud_counts_of(0, out, max); }

static int u32_at(const uint8_t *p, size_t n, uint64_t off, uint32_t *v) {
    if (off > n || n - off < 4) return 0;
    memcpy(v, p + off, 4);
    return 1;
}

static int u64_at(const uint8_t *p, size_t n, uint64_t off, uint64_t *v) {
    if (off > n || n - off < 8) return 0;
    memcpy(v, p + off, 8);
    return 1;
}

static int f32_at(const uint8_t *p, size_t n, uint64_t off, float *v) {
    if (off > n || n - off < 4) return 0;
    memcpy(v, p + off, 4);
    return 1;
}

static const char *str_at(const uint8_t *p, size_t n, uint64_t off) {
    return off < n && memchr(p + off, 0, n - off) ? (const char *)p + off : NULL;
}

static int table(const uint8_t *p, size_t n, size_t slot, uint64_t *names, uint64_t *records, uint32_t *count) {
    return u64_at(p, n, slot, names) && u64_at(p, n, slot + 8, records) && u32_at(p, n, slot + 24, count) &&
           *names <= n && *records <= n && (n - *names) / 8 >= *count && (n - *records) / 8 >= *count;
}

static int table_find(const uint8_t *p, size_t n, size_t slot, const char *name, size_t size, uint64_t *rec) {
    uint64_t names, records;
    uint32_t count;
    if (!table(p, n, slot, &names, &records, &count)) return -1;
    for (uint32_t i = 0; i < count; i++) {
        uint64_t s, r;
        const char *t;
        if (!u64_at(p, n, names + 8 * (uint64_t)i, &s) || !(t = str_at(p, n, s)) || strcmp(t, name)) continue;
        if (!u64_at(p, n, records + 8 * (uint64_t)i, &r) || !r || r > n || n - r < size) return -1;
        *rec = r;
        return (int)i;
    }
    return -1;
}

static int same(float a, float b) { return fabsf(a - b) < 1e-3f; }

static int plain_ancestors(const uint8_t *p, size_t n, const char *layout, const char *pane, uint64_t rec, char *why,
                           size_t why_n) {
    uint64_t names, records, r = rec;
    uint32_t count;
    if (!table(p, n, PANE_TABLE, &names, &records, &count)) return 0;
    for (uint32_t step = 0; step <= count; step++) {
        uint32_t parent;
        if (!u32_at(p, n, r + PANE_PARENT, &parent)) break;
        if (parent == 0xffffffffu) return 1;
        float sx, rot;
        if (parent >= count || !u64_at(p, n, records + 8 * (uint64_t)parent, &r) || r > n || n - r < PANE_NEEDS ||
            !f32_at(p, n, r + PANE_SCALE_X, &sx) || !f32_at(p, n, r + PANE_ROTATE, &rot))
            break;
        if (!same(sx, 1) || !same(rot, 0)) {
            snprintf(why, why_n, "%s %s is inside a pane scaled %g or turned %g", layout, pane, (double)sx, (double)rot);
            return 0;
        }
    }
    snprintf(why, why_n, "%s %s has a damaged chain of parents", layout, pane);
    return 0;
}

enum { MAX_TRACKS = 64 };
typedef struct {
    const char *anim;
    uint64_t curve;
    uint64_t constant;
} Track;

static int x_tracks(const uint8_t *p, size_t n, uint32_t index, int prop, Track *out, int max) {
    uint64_t names, records;
    uint32_t count;
    int found = 0;
    if (!table(p, n, ANIM_TABLE, &names, &records, &count)) return -1;
    for (uint32_t a = 0; a < count; a++) {
        uint64_t s, r, body, tracks, curves, comps, consts, ccomps;
        uint32_t ntracks, ncurves, ncomps, nconsts, nccomps;
        const char *name;
        if (!u64_at(p, n, names + 8 * (uint64_t)a, &s) || !(name = str_at(p, n, s)) ||
            !u64_at(p, n, records + 8 * (uint64_t)a, &r) || !u64_at(p, n, r, &body) ||
            !u64_at(p, n, body + ANIM_TRACKS, &tracks) || !u32_at(p, n, body + ANIM_TRACKS + 8, &ntracks) ||
            !u64_at(p, n, body + ANIM_CURVES, &curves) || !u32_at(p, n, body + ANIM_CURVES + 8, &ncurves) ||
            !u64_at(p, n, body + ANIM_COMPONENTS, &comps) || !u32_at(p, n, body + ANIM_COMPONENTS + 8, &ncomps) ||
            !u64_at(p, n, body + ANIM_CONSTANTS, &consts) || !u32_at(p, n, body + ANIM_CONSTANTS + 8, &nconsts) ||
            !u64_at(p, n, body + ANIM_CONST_COMPONENTS, &ccomps) ||
            !u32_at(p, n, body + ANIM_CONST_COMPONENTS + 8, &nccomps))
            return -1;
        for (uint32_t t = 0; t < ntracks; t++) {
            uint64_t e, code_at;
            uint8_t code[6], head[6];
            if (!u64_at(p, n, tracks + 8 * (uint64_t)t, &e) || !u64_at(p, n, e, &code_at) || code_at > n ||
                n - code_at < 6 || e > n || n - e < 14)
                return -1;
            memcpy(code, p + code_at, 6);
            memcpy(head, p + e + 8, 6);
            uint64_t v = 0;
            for (int i = 5; i >= 0; i--) v = v << 8 | code[i];
            uint32_t kind = (v >> 8) & 7, pane = ((v >> 11) & 0xf) | (((v >> 16) & 7) << 4);
            if (kind != PANE_KIND || pane != index || ((v >> 22) & 1) != !(prop >> 8) || code[3] != (prop & 0xff)) continue;
            unsigned first = head[0] | head[1] << 8, cfirst = head[2] | head[3] << 8, nc = head[4], nk = head[5];
            for (unsigned k = first; first != NO_INDEX && k < first + nc; k++) {
                uint64_t c;
                if (k >= ncurves || k >= ncomps || comps > n || n - comps <= k ||
                    !u64_at(p, n, curves + 8 * (uint64_t)k, &c))
                    return -1;
                if (p[comps + k] != COMPONENT_X) continue;
                if (found < max) out[found] = (Track){name, c, 0};
                if (++found > max) return found;
            }
            for (unsigned k = cfirst; cfirst != NO_INDEX && k < cfirst + nk; k++) {
                if (k >= nconsts || k >= nccomps || ccomps > n || n - ccomps <= k || consts > n ||
                    (n - consts) / 4 <= k)
                    return -1;
                if (p[ccomps + k] != COMPONENT_X) continue;
                if (found < max) out[found] = (Track){name, 0, consts + 4 * (uint64_t)k};
                if (++found > max) return found;
            }
        }
    }
    return found;
}

static int track_holds(const uint8_t *p, size_t n, const Track *t, float v) {
    float f;
    if (!t->curve) return f32_at(p, n, t->constant, &f) ? same(f, v) : -1;
    uint64_t values, data;
    uint32_t count;
    if (!u64_at(p, n, t->curve + 8, &values) || !u64_at(p, n, values + 8, &data) ||
        !u32_at(p, n, values + 16, &count) || data > n || (n - data) / 4 < count)
        return -1;
    for (uint32_t j = 0; j < count; j += 2) {
        memcpy(&f, p + data + 4 * (uint64_t)j, 4);
        if (same(f, v)) return 1;
    }
    return 0;
}

static int centred_ancestors(const uint8_t *p, size_t n, const char *layout, const char *pane, uint64_t rec, char *why,
                             size_t why_n) {
    uint64_t names, records, r = rec;
    uint32_t count;
    Track tr[MAX_TRACKS];
    if (!table(p, n, PANE_TABLE, &names, &records, &count)) return 0;
    for (uint32_t step = 0; step <= count; step++) {
        uint32_t parent;
        float x;
        if (!u32_at(p, n, r + PANE_PARENT, &parent)) break;
        if (parent == 0xffffffffu) return 1;
        if (parent >= count || !u64_at(p, n, records + 8 * (uint64_t)parent, &r) || r > n || n - r < PANE_NEEDS ||
            !f32_at(p, n, r + PANE_X, &x))
            break;
        if (!same(x, 0) || x_tracks(p, n, parent, PROP_TRANSLATE, tr, MAX_TRACKS) ||
            x_tracks(p, n, parent, PROP_SCALE, tr, MAX_TRACKS)) {
            snprintf(why, why_n, "%s %s is inside a pane at x %g or one an animation moves or scales", layout, pane,
                     (double)x);
            return 0;
        }
    }
    snprintf(why, why_n, "%s %s has a damaged chain of parents", layout, pane);
    return 0;
}

static int ancestors_scale(const uint8_t *p, size_t n, const char *layout, const char *pane, uint64_t rec,
                           float *scale, char *why, size_t why_n) {
    uint64_t names, records, r = rec;
    uint32_t count;
    float product = 1;
    if (!table(p, n, PANE_TABLE, &names, &records, &count)) return 0;
    for (uint32_t step = 0; step <= count; step++) {
        uint32_t parent;
        if (!u32_at(p, n, r + PANE_PARENT, &parent)) break;
        if (parent == 0xffffffffu) {
            if (fabsf(product) < 1e-3f) {
                snprintf(why, why_n, "%s %s is inside a pane of scale x 0", layout, pane);
                return 0;
            }
            *scale = product;
            return 1;
        }
        float sx, rot;
        if (parent >= count || !u64_at(p, n, records + 8 * (uint64_t)parent, &r) || r > n || n - r < PANE_NEEDS ||
            !f32_at(p, n, r + PANE_SCALE_X, &sx) || !f32_at(p, n, r + PANE_ROTATE, &rot))
            break;
        if (!same(rot, 0)) {
            snprintf(why, why_n, "%s %s is inside a pane turned %g", layout, pane, (double)rot);
            return 0;
        }
        product *= sx;
    }
    snprintf(why, why_n, "%s %s has a damaged chain of parents", layout, pane);
    return 0;
}

typedef struct {
    uint64_t at;
    float from, to;
} Edit;

static int add(Edit *e, int k, uint64_t at, float from, float to) {
    if (k < MAX_EDITS) e[k] = (Edit){at, from, to};
    return k + 1;
}

static void say(char *report, size_t report_n, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void say(char *report, size_t report_n, const char *fmt, ...) {
    if (!report || !report_n) return;
    size_t at = strlen(report);
    if (at + 1 >= report_n) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(report + at, report_n - at, fmt, ap);
    va_end(ap);
}

static const KeyOp *key_op(const Plan *plan, int groups, const char *pane, int prop, const char *anim) {
    for (int i = 0; i < plan->nkeys; i++) {
        const KeyOp *k = &plan->keys[i];
        if (k->prop == prop && !strcmp(k->pane, pane) && !strcmp(k->anim, anim) && (key_groups(k) & groups)) return k;
    }
    return NULL;
}

static int keyed_covered(const uint8_t *p, size_t n, const Plan *plan, int groups, const char *pane, uint32_t index,
                         int prop, char *why, size_t why_n) {
    Track tr[MAX_TRACKS];
    int keyed = x_tracks(p, n, index, prop, tr, MAX_TRACKS);
    if (keyed < 0 || keyed > MAX_TRACKS) {
        snprintf(why, why_n, "%s's animations of %s cannot be read", plan->layout, pane);
        return 0;
    }
    for (int t = 0; t < keyed; t++)
        if (!key_op(plan, groups, pane, prop, tr[t].anim)) {
            snprintf(why, why_n, "%s %s has its %s keyed by %s", plan->layout, pane,
                     prop == PROP_SCALE ? "scale x" : prop == PROP_SIZE ? "width" : "x", tr[t].anim);
            return 0;
        }
    return 1;
}

static float key_to(const KeyOp *k, const KeyMap *m, float u, float v) {
    return k->prop == PROP_SCALE ? v * (1 + m->f * u / 1920) : v + m->f * u;
}

static int gather(const uint8_t *p, size_t n, const Plan *plan, const WideHudShift *s, int groups, int moved, Edit *e,
                  char *report, size_t report_n, char *why, size_t why_n) {
    int k = 0, said = 0;
    const char *L = plan->layout;
    uint64_t names, records;
    uint32_t npanes;
    if (!table(p, n, PANE_TABLE, &names, &records, &npanes)) {
        snprintf(why, why_n, "%s has a damaged table of panes", L);
        return -1;
    }
    if (npanes > MAX_PANES) {
        snprintf(why, why_n, "%s has %u panes, more than the %d its animations can name", L, npanes, MAX_PANES);
        return -1;
    }
    for (int i = 0; i < plan->nops; i++) {
        const PaneOp *m = &plan->ops[i];
        if (!(m->group & groups)) continue;
        float u = unit_of(m->group, s);
        uint64_t rec;
        int index = table_find(p, n, PANE_TABLE, m->pane, PANE_NEEDS, &rec);
        float x, sx, w;
        if (index < 0 || !f32_at(p, n, rec + PANE_X, &x) || !f32_at(p, n, rec + PANE_SCALE_X, &sx) ||
            !f32_at(p, n, rec + PANE_W, &w)) {
            snprintf(why, why_n, "%s has no pane %s", L, m->pane);
            return -1;
        }
        int moves_x = m->op != OP_GROW && m->op != OP_WIDEN, scales = m->op == OP_WIDEN || m->op == OP_SQUEEZE ||
                                                                       m->op == OP_PIN;
        float ancestors = 1, xto = m->x, sto = m->sx, wto = m->w;
        if (m->op == OP_MOVE && !plain_ancestors(p, n, L, m->pane, rec, why, why_n)) return -1;
        if (m->op == OP_MOVE_IN && !ancestors_scale(p, n, L, m->pane, rec, &ancestors, why, why_n)) return -1;
        if ((m->op == OP_SQUEEZE || m->op == OP_PIN) && (!plain_ancestors(p, n, L, m->pane, rec, why, why_n) ||
                                                         !centred_ancestors(p, n, L, m->pane, rec, why, why_n)))
            return -1;
        switch (m->op) {
        case OP_MOVE: xto = m->x + m->f * u; break;
        case OP_MOVE_IN: xto = m->x + m->f * u / ancestors; break;
        case OP_GROW: wto = m->w + 2 * m->f * u; break;
        case OP_WIDEN: sto = m->sx * (1 + m->f * u / 1920); break;
        case OP_SQUEEZE: xto = m->x * u, sto = m->sx * u; break;
        case OP_PIN: {
            float q = 1920 / (1920 + u), side = m->x < 0 ? -1920.0f : 1920.0f;
            xto = side - (side - m->x) * q, sto = m->sx * q;
            break;
        }
        }
        float xfrom = moved ? xto : m->x, sfrom = moved ? sto : m->sx, wfrom = moved ? wto : m->w;
        if (moves_x && !same(x, xfrom)) {
            snprintf(why, why_n, "%s %s x is %g, not %g", L, m->pane, (double)x, (double)xfrom);
            return -1;
        }
        if (scales && !same(sx, sfrom)) {
            snprintf(why, why_n, "%s %s scale x is %g, not %g", L, m->pane, (double)sx, (double)sfrom);
            return -1;
        }
        if (m->op == OP_GROW && !same(w, wfrom)) {
            snprintf(why, why_n, "%s %s width is %g, not %g", L, m->pane, (double)w, (double)wfrom);
            return -1;
        }
        if (moves_x && !keyed_covered(p, n, plan, groups, m->pane, (uint32_t)index, PROP_TRANSLATE, why, why_n))
            return -1;
        if (scales && !keyed_covered(p, n, plan, groups, m->pane, (uint32_t)index, PROP_SCALE, why, why_n)) return -1;
        if (m->op == OP_GROW && !keyed_covered(p, n, plan, groups, m->pane, (uint32_t)index, PROP_SIZE, why, why_n))
            return -1;
        say(report, report_n, "%s%s", said++ ? ", " : "", m->pane);
        if (moves_x) {
            k = add(e, k, rec + PANE_X, xfrom, xto);
            say(report, report_n, " x %g -> %g", (double)m->x, (double)xto);
        }
        if (m->op == OP_GROW) {
            k = add(e, k, rec + PANE_W, wfrom, wto);
            say(report, report_n, " width %g -> %g", (double)m->w, (double)wto);
        }
        if (scales) {
            k = add(e, k, rec + PANE_SCALE_X, sfrom, sto);
            say(report, report_n, "%s scale x %g -> %g", moves_x ? "," : "", (double)m->sx, (double)sto);
        }
    }
    for (int i = 0; i < plan->nkeys; i++) {
        const KeyOp *km = &plan->keys[i];
        if (!(key_groups(km) & groups)) continue;
        uint64_t rec;
        int index = table_find(p, n, PANE_TABLE, km->pane, PANE_NEEDS, &rec);
        Track tr[MAX_TRACKS];
        int nt = index < 0 ? -1 : x_tracks(p, n, (uint32_t)index, km->prop, tr, MAX_TRACKS), found = 0;
        if (nt < 0 || nt > MAX_TRACKS) {
            snprintf(why, why_n, index < 0 ? "%s has no pane %s" : "%s's animations of %s cannot be read", L, km->pane);
            return -1;
        }
        if (km->prop == PROP_TRANSLATE && !plain_ancestors(p, n, L, km->pane, rec, why, why_n)) return -1;
        const char *what = km->prop == PROP_SCALE ? "scale x" : "x";
        for (int t = 0; t < nt; t++) {
            if (strcmp(tr[t].anim, km->anim)) {
                if (key_op(plan, groups, km->pane, km->prop, tr[t].anim)) continue;
                for (int j = 0; j < MAX_MAP && km->map[j].at > 0; j++) {
                    const KeyMap *m = &km->map[j];
                    if (!(m->group & groups)) continue;
                    float from = moved ? key_to(km, m, unit_of(m->group, s), m->value) : m->value;
                    int holds = track_holds(p, n, &tr[t], from);
                    if (holds < 0) snprintf(why, why_n, "%s's animations of %s cannot be read", L, km->pane);
                    if (holds > 0)
                        snprintf(why, why_n, "%s %s %s is also at %g in %s", L, km->pane, what, (double)from,
                                 tr[t].anim);
                    if (holds) return -1;
                }
                continue;
            }
            if (found++) {
                snprintf(why, why_n, "%s %s %s %s is not one curve of %d keys", L, km->anim, km->pane, what, km->keys);
                return -1;
            }
            if (!tr[t].curve) {
                float v;
                const KeyMap *hit = NULL;
                if (km->keys || !f32_at(p, n, tr[t].constant, &v)) {
                    snprintf(why, why_n, "%s %s %s %s is a constant, not a curve of %d keys", L, km->anim, km->pane,
                             what, km->keys);
                    return -1;
                }
                for (int j = 0; j < MAX_MAP && km->map[j].at > 0 && !hit; j++) {
                    const KeyMap *m = &km->map[j];
                    float from = moved && (m->group & groups) ? key_to(km, m, unit_of(m->group, s), m->value) : m->value;
                    if (same(v, from)) hit = m;
                }
                if (!hit) {
                    snprintf(why, why_n, "%s %s %s %s is %g, a value the plan does not know", L, km->anim, km->pane,
                             what, (double)v);
                    return -1;
                }
                if (hit->group & groups) {
                    float to = key_to(km, hit, unit_of(hit->group, s), hit->value);
                    k = add(e, k, tr[t].constant, moved ? to : hit->value, to);
                }
                continue;
            }
            uint64_t values, data, frames;
            uint32_t count, nframes;
            float span;
            if (!u64_at(p, n, tr[t].curve, &frames) || !u64_at(p, n, tr[t].curve + 8, &values) ||
                !u32_at(p, n, frames + 16, &nframes) || !f32_at(p, n, values, &span) ||
                !u64_at(p, n, values + 8, &data) || !u32_at(p, n, values + 16, &count) || data > n ||
                (n - data) / 4 < count || count != 2 * nframes || (int)nframes != km->keys) {
                snprintf(why, why_n, "%s %s %s %s is not one curve of %d keys", L, km->anim, km->pane, what, km->keys);
                return -1;
            }
            int at[MAX_MAP] = {0};
            float v0 = 0, v1 = 0, first = 0, last = 0;
            for (uint32_t j = 0; j < nframes; j++) {
                float v, slope, nv;
                memcpy(&v, p + data + 8 * (uint64_t)j, 4);
                memcpy(&slope, p + data + 8 * (uint64_t)j + 4, 4);
                nv = v;
                for (int q = 0; q < MAX_MAP && km->map[q].at > 0; q++) {
                    const KeyMap *m = &km->map[q];
                    int on = (m->group & groups) != 0;
                    float u = on ? unit_of(m->group, s) : 0, to = on ? key_to(km, m, u, m->value) : m->value;
                    if (!same(v, moved ? to : m->value)) continue;
                    at[q]++;
                    if (!on) break;
                    nv = to;
                    k = add(e, k, data + 8 * (uint64_t)j, v, to);
                    if (km->prop == PROP_SCALE) {
                        float sto = moved ? slope : key_to(km, m, u, slope);
                        k = add(e, k, data + 8 * (uint64_t)j + 4, slope, sto);
                    }
                    break;
                }
                if (km->prop == PROP_TRANSLATE && km->slope_f != 0 && (km->map[0].group & groups)) {
                    float sto = moved ? slope : slope * (1 + km->slope_f * unit_of(km->map[0].group, s) / 1920);
                    k = add(e, k, data + 8 * (uint64_t)j + 4, slope, sto);
                }
                if (moved) v = nv;
                if (!j) v0 = v, first = nv;
                v1 = v, last = nv;
            }
            for (int q = 0; q < MAX_MAP && km->map[q].at > 0; q++)
                if (at[q] != km->map[q].at) {
                    snprintf(why, why_n, "%s %s %s %s has %d keys at %g, not %d of %d", L, km->anim, km->pane, what,
                             at[q], (double)km->map[q].value, km->map[q].at, km->keys);
                    return -1;
                }
            if (!same(span, (moved ? last - first : v1 - v0))) {
                snprintf(why, why_n, "%s %s %s %s has span %g, not its last value less its first, %g", L, km->anim,
                         km->pane, what, (double)span, (double)(v1 - v0));
                return -1;
            }
            k = add(e, k, values, moved ? last - first : span, last - first);
        }
        if (!found) {
            snprintf(why, why_n, "%s %s does not key %s %s", L, km->anim, km->pane, what);
            return -1;
        }
        for (int j = 0; j < MAX_MAP && km->map[j].at > 0; j++) {
            const KeyMap *m = &km->map[j];
            if (!(m->group & groups)) continue;
            say(report, report_n, "%s%s %s %s %d key%s %g -> %g", said++ ? ", " : "", km->anim, km->pane, what, m->at,
                m->at == 1 ? "" : "s", (double)m->value, (double)key_to(km, m, unit_of(m->group, s), m->value));
        }
    }
    if (k > MAX_EDITS) {
        snprintf(why, why_n, "%s needs %d edits, more than %d", L, k, MAX_EDITS);
        return -1;
    }
    for (int i = 0; i < k; i++)
        for (int j = 0; j < i; j++)
            if (e[i].at < e[j].at + 4 && e[j].at < e[i].at + 4) {
                snprintf(why, why_n, "%s has two moved values in one place", L);
                return -1;
            }
    return k;
}

static const Plan *plan_of(const char *member) {
    for (int i = 0; i < NPLANS; i++) {
        size_t len = strlen(plans[i].layout);
        if (!strncmp(member, plans[i].layout, len) && !strcmp(member + len, ".buil")) return &plans[i];
    }
    return NULL;
}

static int lay_out(uint8_t *p, size_t n, const char *member, const WideHudShift *s, int groups, char *report,
                   size_t report_n, char *why, size_t why_n) {
    const Plan *plan = plan_of(member);
    if (report && report_n) report[0] = 0;
    if (!plan || !(plan_groups(plan) & groups)) return 0;
    if (n < 4 || memcmp(p, "BFFH", 4)) {
        snprintf(why, why_n, "%s is not a BFFH layout", member);
        return -1;
    }
    Edit *e = malloc(2 * MAX_EDITS * sizeof *e), *back = NULL;
    uint8_t(*undo)[4] = malloc(MAX_EDITS * sizeof *undo);
    int result = -1;
    if (!e || !undo) {
        snprintf(why, why_n, "out of memory");
        goto done;
    }
    back = e + MAX_EDITS;
    int k = gather(p, n, plan, s, groups, 0, e, report, report_n, why, why_n);
    if (k < 0) {
        char why2[200];
        if (gather(p, n, plan, s, groups, 1, back, NULL, 0, why2, sizeof why2) >= 0) {
            if (report && report_n) report[0] = 0;
            result = 0;
        }
        goto done;
    }
    int changed = 0;
    for (int i = 0; i < k; i++) {
        memcpy(undo[i], p + e[i].at, 4);
        if (!memcmp(undo[i], &e[i].to, 4)) continue;
        memcpy(p + e[i].at, &e[i].to, 4);
        changed++;
    }
    char why2[200];
    int kb = gather(p, n, plan, s, groups, 1, back, NULL, 0, why2, sizeof why2);
    int ok = kb == k;
    for (int i = 0; ok && i < k; i++) ok = back[i].at == e[i].at && !memcmp(&back[i].to, &e[i].to, 4);
    if (!ok) {
        for (int i = k - 1; i >= 0; i--) memcpy(p + e[i].at, undo[i], 4);
        snprintf(why, why_n, "%s reads otherwise after the edit (%s)", member, kb < 0 ? why2 : "another field moved");
        goto done;
    }
    result = changed;
done:
    free(e);
    free(undo);
    return result;
}

int wide_hud_lay_out(uint8_t *p, size_t n, const char *member, const WideHudShift *s, char *report, size_t report_n,
                     char *why, size_t why_n) {
    return lay_out(p, n, member, s, groups_of(s), report, report_n, why, why_n);
}

#pragma STDC FP_CONTRACT OFF
enum { MAT_TABLE = 0x138, PIC_TABLE = 0x158, PART_TABLE = 0x1b8, CAPTURE_TABLE = 0x1d8 };
enum { PANE_END = 0x34, PANE_FLAGS = 0x38, PANE_MATERIAL = 0x3c, PANE_PICTURE = 0x42, PANE_SIZE = 0x44 };
enum { PIC_WINDOW = 0x08, PIC_MATERIAL = 0x10, PICTURE_BLOB = 0x88, MAT_MAPS = 0x40, MAT_MAP = 0x48,
       MATERIAL_BLOB = 0x40 + 8 + 0x30, MAP_UV_SCALE = 0x10, MAP_UV_TRANSLATE = 0x18, MAP_ROTATE = 0x20 };
enum { FILE_SIZE = 0x08, POOL_SECTION = 0xd0, MATERIAL_EXTRA = 0x300, PART_PANE = 0x40, CAPTURE_PANE = 0x04 };
enum { CODE_SIZE = 6, MAX_NAME = 32 };
static const unsigned picture_pointers[] = {0x00, 0x20, 0x48, 0x50, 0x58, 0x60};

static uint64_t rd64(const uint8_t *s) {
    uint64_t v;
    memcpy(&v, s, 8);
    return v;
}
static uint64_t rot64(uint64_t v, int s) { return s ? v >> s | v << (64 - s) : v; }
static uint64_t len16(uint64_t u, uint64_t v, uint64_t mul) {
    uint64_t a = (u ^ v) * mul;
    a ^= a >> 47;
    uint64_t b = (v ^ a) * mul;
    b ^= b >> 47;
    return b * mul;
}

uint64_t wide_hud_name_hash(const char *name) {
    static const uint64_t k0 = 0xc3a5c85c97cb3127ull, k1 = 0xb492b66fbe98f273ull, k2 = 0x9ae16a3b2f90404full;
    const uint8_t *s = (const uint8_t *)name;
    size_t n = strlen(name);
    uint64_t mul = k2 + n * 2;
    if (n > MAX_NAME) return 0;
    if (n > 16) {
        uint64_t a = rd64(s) * k1, b = rd64(s + 8), c = rd64(s + n - 8) * mul, d = rd64(s + n - 16) * k2;
        return len16(rot64(a + b, 43) + rot64(c, 30) + d, a + rot64(b + k2, 18) + c, mul);
    }
    if (n >= 8) {
        uint64_t a = rd64(s) + k2, b = rd64(s + n - 8);
        return len16(rot64(b, 37) * mul + a, (rot64(a, 25) + b) * mul, mul);
    }
    if (n >= 4) {
        uint32_t x, y;
        memcpy(&x, s, 4);
        memcpy(&y, s + n - 4, 4);
        return len16(n + ((uint64_t)x << 3), y, mul);
    }
    if (n) {
        uint64_t y = s[0] + ((uint64_t)s[n >> 1] << 8), z = n + ((uint64_t)s[n - 1] << 2), v = y * k2 ^ z * k0;
        return (v ^ v >> 47) * k2;
    }
    return k2;
}

static int u16_at(const uint8_t *p, size_t n, uint64_t off, uint16_t *v) {
    if (off > n || n - off < 2) return 0;
    memcpy(v, p + off, 2);
    return 1;
}
static void put16(uint8_t *p, uint64_t at, uint16_t v) { memcpy(p + at, &v, 2); }
static void put32(uint8_t *p, uint64_t at, uint32_t v) { memcpy(p + at, &v, 4); }
static void put64(uint8_t *p, uint64_t at, uint64_t v) { memcpy(p + at, &v, 8); }
static uint64_t put_at(uint8_t *q, uint64_t *at, const void *src, size_t len, unsigned align) {
    *at = (*at + align - 1) & ~(uint64_t)(align - 1);
    uint64_t r = *at;
    if (src) memcpy(q + r, src, len);
    *at += len;
    return r;
}
static void putf(uint8_t *p, uint64_t at, double v) {
    float f = (float)v;
    memcpy(p + at, &f, 4);
}

typedef struct {
    uint64_t names, records, hashes;
    uint32_t count;
} Table;
static int table3(const uint8_t *p, size_t n, size_t slot, Table *t) {
    return table(p, n, slot, &t->names, &t->records, &t->count) && u64_at(p, n, slot + 16, &t->hashes) &&
           (!t->count || t->hashes) && t->hashes <= n && (n - t->hashes) / 8 >= t->count;
}
static const char *entry_name(const uint8_t *p, size_t n, const Table *t, uint32_t i) {
    uint64_t s;
    return u64_at(p, n, t->names + 8 * (uint64_t)i, &s) ? str_at(p, n, s) : NULL;
}
static uint64_t entry_record(const uint8_t *p, size_t n, const Table *t, uint32_t i, size_t size) {
    uint64_t r;
    return u64_at(p, n, t->records + 8 * (uint64_t)i, &r) && r && r <= n && n - r >= size ? r : 0;
}

typedef struct {
    double cx, w, usx, utx;
} Window;
static int window_of(const Piece *pc, double x, double w, double sx, double tx, double u, Window *out) {
    double left = x - w / 2, t0 = pc->x0 + pc->f0 * u, t1 = pc->x1 + pc->f1 * u;
    if (!(left - 1e-3 <= pc->a && pc->a < pc->b && pc->b <= left + w + 1e-3) || t1 < t0) return 0;
    double su0 = (pc->a - left) / w, su1 = (pc->b - left) / w;
    *out = (Window){(t0 + t1) / 2, t1 - t0, sx * (su1 - su0), tx + ((su0 + su1) / 2 - 0.5) * sx};
    return 1;
}

static int code_order(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}
static int track_codes(const uint8_t *p, size_t n, uint64_t **codes) {
    uint64_t names, records;
    uint32_t count;
    size_t k = 0, cap = 0;
    *codes = NULL;
    if (!table(p, n, ANIM_TABLE, &names, &records, &count)) return -1;
    for (uint32_t a = 0; a < count; a++) {
        uint64_t r, body, tracks;
        uint32_t ntracks;
        if (!u64_at(p, n, records + 8 * (uint64_t)a, &r) || !u64_at(p, n, r, &body) ||
            !u64_at(p, n, body + ANIM_TRACKS, &tracks) || !u32_at(p, n, body + ANIM_TRACKS + 8, &ntracks) ||
            tracks > n || (n - tracks) / 8 < ntracks)
            goto fail;
        for (uint32_t t = 0; t < ntracks; t++) {
            uint64_t e, code;
            if (!u64_at(p, n, tracks + 8 * (uint64_t)t, &e) || !u64_at(p, n, e, &code) || code > n ||
                n - code < CODE_SIZE)
                goto fail;
            if (k == cap) {
                uint64_t *grown = realloc(*codes, (cap = cap ? 2 * cap : 64) * sizeof *grown);
                if (!grown) goto fail;
                *codes = grown;
            }
            (*codes)[k++] = code;
        }
    }
    if (k) qsort(*codes, k, sizeof **codes, code_order);
    size_t u = 0;
    for (size_t j = 0; j < k; j++) {
        if (u && (*codes)[j] == (*codes)[u - 1]) continue;
        if (u && (*codes)[j] - (*codes)[u - 1] < CODE_SIZE) goto fail;
        (*codes)[u++] = (*codes)[j];
    }
    return (int)u;
fail:
    free(*codes);
    *codes = NULL;
    return -1;
}

static unsigned code_pane(const uint8_t *c) {
    uint64_t v = 0;
    for (int i = CODE_SIZE - 1; i >= 0; i--) v = v << 8 | c[i];
    return (unsigned)(((v >> 11) & 0xf) | (((v >> 16) & 7) << 4));
}
static void code_set_pane(uint8_t *c, unsigned index) {
    uint64_t v = 0;
    for (int i = CODE_SIZE - 1; i >= 0; i--) v = v << 8 | c[i];
    v = (v & ~((uint64_t)0xf << 11 | (uint64_t)7 << 16)) | (uint64_t)(index & 0xf) << 11 |
        (uint64_t)(index >> 4 & 7) << 16;
    for (int i = 0; i < CODE_SIZE; i++) c[i] = (uint8_t)(v >> (8 * i));
}

typedef struct {
    uint64_t pane, pic, mat;
    uint16_t pic_index, mat_index;
} PieceRecs;

typedef struct {
    double x, w, sx, tx;
} Orig;
static int check_split(const uint8_t *q, size_t qn, const Split *sp, float u, const Orig *orig, char *why,
                       size_t why_n) {
    const char *L = sp->layout, *P = sp->pane;
    Table panes, pics, mats;
    uint64_t rec, *codes = NULL;
    uint32_t extra = (uint32_t)sp->npieces - 1, v32, pool, pool_n;
    int i;
    char name[64];
    if (!table3(q, qn, PANE_TABLE, &panes) || !table3(q, qn, PIC_TABLE, &pics) || !table3(q, qn, MAT_TABLE, &mats) ||
        (i = table_find(q, qn, PANE_TABLE, P, PANE_SIZE, &rec)) < 0 || (uint64_t)i + extra >= panes.count ||
        pics.count <= extra || mats.count <= extra) {
        snprintf(why, why_n, "%s's tables of panes, pictures and materials do not hold %s's pieces", L, P);
        return 0;
    }
    for (uint32_t j = 0; j < panes.count; j++) {
        uint64_t r = entry_record(q, qn, &panes, j, PANE_SIZE), pr;
        uint32_t parent, end, pend;
        if (!r || !u32_at(q, qn, r + PANE_PARENT, &parent) || !u32_at(q, qn, r + PANE_END, &end) ||
            (parent != 0xffffffffu && parent >= j) || end <= j || end > panes.count ||
            (parent != 0xffffffffu && (!(pr = entry_record(q, qn, &panes, parent, PANE_SIZE)) ||
                                       !u32_at(q, qn, pr + PANE_END, &pend) || end > pend || j >= pend))) {
            snprintf(why, why_n, "%s's tree of panes is broken at pane %u", L, j);
            return 0;
        }
    }
    PieceRecs pr[MAX_PANES];
    double ratio = 0, base = 0;
    uint64_t pic_ptr[sizeof picture_pointers / sizeof *picture_pointers];
    for (uint32_t k = 0; k <= extra; k++) {
        PieceRecs *c = &pr[k];
        uint32_t at = (uint32_t)i + k;
        const char *pn = entry_name(q, qn, &panes, at);
        uint64_t hash;
        snprintf(name, sizeof name, k ? "%s_%u" : "%s", P, k);
        if (!pn || strcmp(pn, name) || !u64_at(q, qn, panes.hashes + 8 * (uint64_t)at, &hash) ||
            hash != wide_hud_name_hash(name) || !(c->pane = entry_record(q, qn, &panes, at, PANE_SIZE)) ||
            !u16_at(q, qn, c->pane + PANE_PICTURE, &c->pic_index) ||
            !u16_at(q, qn, c->pane + PANE_MATERIAL, &c->mat_index) || c->pic_index >= pics.count ||
            c->mat_index >= mats.count || (k && (c->pic_index != pics.count - extra + k - 1 ||
                                                 c->mat_index != mats.count - extra + k - 1))) {
            snprintf(why, why_n, "%s's pane %u is not %s's piece %u", L, at, P, k);
            return 0;
        }
        const char *picn = entry_name(q, qn, &pics, c->pic_index), *matn = entry_name(q, qn, &mats, c->mat_index);
        uint16_t pm;
        uint64_t maps, map;
        uint32_t nmaps;
        if (!picn || strcmp(picn, name) || !matn || strcmp(matn, name) ||
            !(c->pic = entry_record(q, qn, &pics, c->pic_index, PICTURE_BLOB)) ||
            !u16_at(q, qn, c->pic + PIC_MATERIAL, &pm) || pm != c->mat_index ||
            !(c->mat = entry_record(q, qn, &mats, c->mat_index, MATERIAL_BLOB)) || !u64_at(q, qn, c->mat, &maps) ||
            !u32_at(q, qn, c->mat + 8, &nmaps) || !u64_at(q, qn, c->mat + MAT_MAPS, &map) ||
            maps != c->mat + MAT_MAPS || nmaps != 1 || map != c->mat + MAT_MAP) {
            snprintf(why, why_n, "%s %s has not a picture and a material of its own", L, name);
            return 0;
        }
        for (size_t o = 0; o < sizeof picture_pointers / sizeof *picture_pointers; o++) {
            uint64_t to;
            u64_at(q, qn, c->pic + picture_pointers[o], &to);
            if (!k) pic_ptr[o] = to - c->pic;
            if (to - c->pic != pic_ptr[o] || to - c->pic >= PICTURE_BLOB) {
                snprintf(why, why_n, "%s %s's picture record does not point inside itself", L, name);
                return 0;
            }
        }
        const Piece *pc = &sp->pieces[k];
        double t0 = pc->x0 + pc->f0 * (double)u, t1 = pc->x1 + pc->f1 * (double)u;
        float x, w, usx, utx;
        f32_at(q, qn, c->pane + PANE_X, &x);
        f32_at(q, qn, c->pane + PANE_W, &w);
        f32_at(q, qn, c->mat + MAT_MAP + MAP_UV_SCALE, &usx);
        f32_at(q, qn, c->mat + MAT_MAP + MAP_UV_TRANSLATE, &utx);
        Window win;
        int placed = same(x, (float)((t0 + t1) / 2)) && same(w, (float)(t1 - t0)), shows;
        if (orig) {
            shows = window_of(pc, orig->x, orig->w, orig->sx, orig->tx, u, &win) && x == (float)win.cx &&
                    w == (float)win.w && usx == (float)win.usx && utx == (float)win.utx;
        } else {
            double r = usx / (pc->b - pc->a), b0 = utx - r * (pc->a + pc->b) / 2;
            if (!k) ratio = r, base = b0;
            shows = fabs(r - ratio) <= 1e-6 * fabs(ratio) + 1e-9 && fabs(b0 - base) < 1e-4;
        }
        if (!placed || !shows) {
            snprintf(why, why_n, "%s %s is at x %g, width %g, uv scale %g, translate %g, not where piece %u goes", L,
                     name, (double)x, (double)w, (double)usx, (double)utx, k);
            return 0;
        }
    }
    uint64_t extra_at;
    if (!u64_at(q, qn, MATERIAL_EXTRA, &extra_at) || (extra_at && (!u32_at(q, qn, MATERIAL_EXTRA + 8, &v32) ||
                                                                   v32 != mats.count))) {
        snprintf(why, why_n, "%s's per-material array is not as long as its %u materials", L, mats.count);
        return 0;
    }
    int nc = track_codes(q, qn, &codes);
    for (int c = 0; c < nc; c++)
        if (code_pane(q + codes[c]) >= panes.count) nc = -1;
    free(codes);
    if (nc < 0) {
        snprintf(why, why_n, "%s's animations name a pane past its %u", L, panes.count);
        return 0;
    }
    if (!u32_at(q, qn, FILE_SIZE, &v32) || v32 != qn || !u32_at(q, qn, POOL_SECTION + 4, &pool) ||
        !u32_at(q, qn, POOL_SECTION + 8, &pool_n) || (uint64_t)pool + pool_n != qn) {
        snprintf(why, why_n, "%s's size or its string pool's does not reach its end", L);
        return 0;
    }
    return 1;
}

static int split_layout(const uint8_t *p, size_t n, const Split *sp, float u, uint8_t **out, size_t *out_n,
                        char *report, size_t report_n, char *why, size_t why_n) {
    const char *L = sp->layout, *P = sp->pane;
    Table panes, pics, mats;
    uint64_t rec, *codes = NULL, parts_n, parts_r, caps_n, caps_r;
    uint32_t extra = (uint32_t)sp->npieces - 1, nparts, ncaps;
    uint8_t *q = NULL;
    char name[64], why2[200];
    int i, ncodes = -1, result = -1;
    *out = NULL;
    if (report && report_n) report[0] = 0;
    if (n < 4 || memcmp(p, "BFFH", 4)) {
        snprintf(why, why_n, "%s is not a BFFH layout", L);
        return -1;
    }
    if (!table3(p, n, PANE_TABLE, &panes)) {
        snprintf(why, why_n, "%s has a damaged table of panes", L);
        return -1;
    }
    snprintf(name, sizeof name, "%s_1", P);
    if (table_find(p, n, PANE_TABLE, name, PANE_SIZE, &rec) >= 0) {
        if (check_split(p, n, sp, u, NULL, why2, sizeof why2)) return 0;
        snprintf(why, why_n, "%s has %s but is not split so (%s)", L, name, why2);
        return -1;
    }
    uint32_t flags, end, v32, pool, pool_n, extra_n = 0, stride = 0;
    float x, w, sx, rot, msx, mtx, mrot;
    uint16_t pic_index, mat_index, pane_mat;
    uint64_t pic = 0, mat = 0, maps, map, window, map_names, pool64, extra_at = 0;
    if (sp->npieces < 1 || (i = table_find(p, n, PANE_TABLE, P, PANE_SIZE, &rec)) < 0) {
        snprintf(why, why_n, "%s has no pane %s", L, P);
        return -1;
    }
    if (panes.count + extra > MAX_PANES) {
        snprintf(why, why_n, "%s would have %u panes, more than the %d its animations can name", L, panes.count + extra,
                 MAX_PANES);
        return -1;
    }
    for (uint32_t j = 0; j < panes.count; j++) {
        uint64_t r = entry_record(p, n, &panes, j, PANE_SIZE);
        uint32_t parent;
        if (!r || !u32_at(p, n, r + PANE_PARENT, &parent) || (parent != 0xffffffffu && parent >= j)) {
            snprintf(why, why_n, "%s has a damaged pane %u", L, j);
            return -1;
        }
        for (uint32_t k = 0; k < j; k++)
            if (entry_record(p, n, &panes, k, PANE_SIZE) == r) {
                snprintf(why, why_n, "%s's panes %u and %u share a record", L, k, j);
                return -1;
            }
        if (parent == (uint32_t)i) {
            snprintf(why, why_n, "%s %s has children", L, P);
            return -1;
        }
    }
    u32_at(p, n, rec + PANE_FLAGS, &flags);
    u32_at(p, n, rec + PANE_END, &end);
    f32_at(p, n, rec + PANE_X, &x), f32_at(p, n, rec + PANE_W, &w);
    f32_at(p, n, rec + PANE_SCALE_X, &sx), f32_at(p, n, rec + PANE_ROTATE, &rot);
    u16_at(p, n, rec + PANE_PICTURE, &pic_index), u16_at(p, n, rec + PANE_MATERIAL, &pane_mat);
    if ((flags >> 7 & 0xf) != 1 || end != (uint32_t)i + 1 || !(w > 0)) {
        snprintf(why, why_n, "%s %s is not a picture pane of its own", L, P);
        return -1;
    }
    if (!same(rot, 0) || !same(sx, 1) || (flags >> 11 & 7) != 2) {
        snprintf(why, why_n, "%s %s is turned %g or scaled %g, or its origin is not at its centre", L, P, (double)rot,
                 (double)sx);
        return -1;
    }
    const char *pn;
    if (!table3(p, n, PIC_TABLE, &pics) || !table3(p, n, MAT_TABLE, &mats) || pic_index >= pics.count ||
        !(pn = entry_name(p, n, &pics, pic_index)) || strcmp(pn, P) ||
        !(pic = entry_record(p, n, &pics, pic_index, PICTURE_BLOB)) || !u16_at(p, n, pic + PIC_MATERIAL, &mat_index) ||
        mat_index != pane_mat || mat_index >= mats.count ||
        !(mat = entry_record(p, n, &mats, mat_index, MATERIAL_BLOB))) {
        snprintf(why, why_n, "%s %s's picture or material is not the one its pane names", L, P);
        return -1;
    }
    uint32_t nmaps;
    u64_at(p, n, pic + PIC_WINDOW, &window);
    u64_at(p, n, mat, &maps), u32_at(p, n, mat + 8, &nmaps), u64_at(p, n, mat + MAT_MAPS, &map);
    u64_at(p, n, mat + MAT_MAP, &map_names);
    if (window || nmaps != 1 || maps != mat + MAT_MAPS || map != mat + MAT_MAP || map_names) {
        snprintf(why, why_n, "%s %s is not a plain picture with one texture map", L, P);
        return -1;
    }
    for (size_t o = 0; o < sizeof picture_pointers / sizeof *picture_pointers; o++) {
        uint64_t to;
        u64_at(p, n, pic + picture_pointers[o], &to);
        if (to < pic || to >= pic + PICTURE_BLOB) {
            snprintf(why, why_n, "%s %s's picture record is not laid out as expected", L, P);
            return -1;
        }
    }
    f32_at(p, n, map + MAP_UV_SCALE, &msx), f32_at(p, n, map + MAP_UV_TRANSLATE, &mtx);
    f32_at(p, n, map + MAP_ROTATE, &mrot);
    if (!same(mrot, 0)) {
        snprintf(why, why_n, "%s %s's texture map is turned %g", L, P, (double)mrot);
        return -1;
    }
    Orig orig = {x, w, msx, mtx};
    Window win[MAX_PANES];
    for (int k = 0; k < sp->npieces; k++)
        if (!window_of(&sp->pieces[k], orig.x, orig.w, orig.sx, orig.tx, u, &win[k])) {
            snprintf(why, why_n, "%s %s's piece %d lies outside the picture or runs backwards", L, P, k);
            return -1;
        }
    for (uint32_t k = 1; k <= extra; k++)
        if (snprintf(name, sizeof name, "%s_%u", P, k) > MAX_NAME) {
            snprintf(why, why_n, "%s %s's pieces' names would be longer than %d bytes", L, P, MAX_NAME);
            return -1;
        }
    if (!table(p, n, PART_TABLE, &parts_n, &parts_r, &nparts) ||
        !table(p, n, CAPTURE_TABLE, &caps_n, &caps_r, &ncaps)) {
        snprintf(why, why_n, "%s has a damaged table of parts or captures", L);
        return -1;
    }
    for (int t = 0; t < 2; t++) {
        uint64_t records = t ? caps_r : parts_r;
        uint32_t count = t ? ncaps : nparts, field = t ? CAPTURE_PANE : PART_PANE;
        for (uint32_t j = 0; j < count; j++) {
            uint64_t r, r2;
            int ok = u64_at(p, n, records + 8 * (uint64_t)j, &r) && r && r <= n && n - r >= field + 4;
            for (uint32_t k = 0; ok && k < j; k++) ok = u64_at(p, n, records + 8 * (uint64_t)k, &r2) && r2 != r;
            if (!ok) {
                snprintf(why, why_n, "%s has a damaged or shared %s record", L, t ? "capture" : "part");
                return -1;
            }
        }
    }
    if ((ncodes = track_codes(p, n, &codes)) < 0) {
        snprintf(why, why_n, "%s's animations' tracks cannot be read", L);
        return -1;
    }
    for (int c = 0; c < ncodes; c++)
        if (code_pane(p + codes[c]) > (unsigned)i && code_pane(p + codes[c]) + extra >= MAX_PANES) {
            snprintf(why, why_n, "%s: a track would name pane %u, past the %d a code can name", L,
                     code_pane(p + codes[c]) + extra, MAX_PANES);
            goto done;
        }
    if (memcmp(p + POOL_SECTION, "BFSI", 4) || !u32_at(p, n, POOL_SECTION + 4, &pool) ||
        !u32_at(p, n, POOL_SECTION + 8, &pool_n) || !u64_at(p, n, POOL_SECTION + 0x10, &pool64) || pool64 != pool ||
        (uint64_t)pool + pool_n != n || !u32_at(p, n, FILE_SIZE, &v32) || v32 != n) {
        snprintf(why, why_n, "%s's string pool section or size does not reach its end", L);
        goto done;
    }
    if (!u64_at(p, n, MATERIAL_EXTRA, &extra_at) ||
        (extra_at && (!u32_at(p, n, MATERIAL_EXTRA + 8, &extra_n) || !u32_at(p, n, MATERIAL_EXTRA + 12, &stride) ||
                      extra_n != mats.count || stride != 4 || extra_at > n || (n - extra_at) / 4 < extra_n))) {
        snprintf(why, why_n, "%s's per-material array holds %u entries of %u bytes, not %u of 4", L, extra_n, stride,
                 mats.count);
        goto done;
    }
    size_t cap = n + 8 + (size_t)extra * (MAX_NAME + 1 + MATERIAL_BLOB + PICTURE_BLOB + PANE_SIZE + 4 * 8) +
                 3 * (8 * ((size_t)panes.count + pics.count + mats.count + 3 * (size_t)extra) + 3 * 8) +
                 4 * ((size_t)mats.count + extra) + 8;
    if (cap > UINT32_MAX || !(q = calloc(cap, 1))) {
        snprintf(why, why_n, "out of memory");
        goto done;
    }
    memcpy(q, p, n);
    for (int t = 0; t < 2; t++) {
        uint64_t records = t ? caps_r : parts_r, r;
        uint32_t count = t ? ncaps : nparts, field = t ? CAPTURE_PANE : PART_PANE, v;
        for (uint32_t j = 0; j < count; j++) {
            u64_at(q, n, records + 8 * (uint64_t)j, &r);
            u32_at(q, n, r + field, &v);
            if (v > (uint32_t)i) put32(q, r + field, v + extra);
        }
    }
    for (uint32_t j = 0; j < panes.count; j++) {
        uint64_t r = entry_record(q, n, &panes, j, PANE_SIZE);
        uint32_t parent, e;
        u32_at(q, n, r + PANE_PARENT, &parent), u32_at(q, n, r + PANE_END, &e);
        if (parent != 0xffffffffu && parent > (uint32_t)i) put32(q, r + PANE_PARENT, parent + extra);
        if (e > (uint32_t)i && j != (uint32_t)i) put32(q, r + PANE_END, e + extra);
    }
    for (int c = 0; c < ncodes; c++)
        if (code_pane(q + codes[c]) > (unsigned)i) code_set_pane(q + codes[c], code_pane(q + codes[c]) + extra);
    putf(q, rec + PANE_X, win[0].cx), putf(q, rec + PANE_W, win[0].w);
    putf(q, map + MAP_UV_SCALE, win[0].usx), putf(q, map + MAP_UV_TRANSLATE, win[0].utx);
    uint64_t at = (n + 7) & ~(uint64_t)7, name_at[MAX_PANES], pane_at[MAX_PANES], pic_at[MAX_PANES],
             mat_at[MAX_PANES];
    for (uint32_t k = 1; k <= extra; k++) {
        size_t len = (size_t)snprintf(name, sizeof name, "%s_%u", P, k) + 1;
        name_at[k] = put_at(q, &at, name, len, 1);
        uint64_t m = mat_at[k] = put_at(q, &at, q + mat, MATERIAL_BLOB, 8);
        put64(q, m, m + MAT_MAPS), put64(q, m + MAT_MAPS, m + MAT_MAP);
        putf(q, m + MAT_MAP + MAP_UV_SCALE, win[k].usx), putf(q, m + MAT_MAP + MAP_UV_TRANSLATE, win[k].utx);
        uint64_t pr = pic_at[k] = put_at(q, &at, q + pic, PICTURE_BLOB, 8);
        for (size_t o = 0; o < sizeof picture_pointers / sizeof *picture_pointers; o++) {
            uint64_t to;
            u64_at(q, n, pic + picture_pointers[o], &to);
            put64(q, pr + picture_pointers[o], to - pic + pr);
        }
        put16(q, pr + PIC_MATERIAL, (uint16_t)(mats.count + k - 1));
        uint64_t pa = pane_at[k] = put_at(q, &at, q + rec, PANE_SIZE, 8);
        putf(q, pa + PANE_X, win[k].cx), putf(q, pa + PANE_W, win[k].w);
        put32(q, pa + PANE_END, (uint32_t)i + k + 1);
        put16(q, pa + PANE_MATERIAL, (uint16_t)(mats.count + k - 1));
        put16(q, pa + PANE_PICTURE, (uint16_t)(pics.count + k - 1));
    }
    const struct {
        size_t slot;
        const Table *t;
        uint32_t insert;
        const uint64_t *records;
    } tables[3] = {{PANE_TABLE, &panes, (uint32_t)i + 1, pane_at},
                   {PIC_TABLE, &pics, pics.count, pic_at},
                   {MAT_TABLE, &mats, mats.count, mat_at}};
    uint64_t lists[3][3];
    for (int t = 0; t < 3; t++) {
        const Table *T = tables[t].t;
        uint32_t count = T->count + extra;
        for (int l = 0; l < 3; l++) {
            uint64_t list = lists[t][l] = put_at(q, &at, NULL, 8 * (size_t)count, 8);
            uint64_t from = l == 0 ? T->names : l == 1 ? T->records : T->hashes;
            for (uint32_t r = 0, old = 0; r < count; r++) {
                uint32_t k = r - tables[t].insert + 1;
                uint64_t v;
                if (r >= tables[t].insert && k <= extra) {
                    snprintf(name, sizeof name, "%s_%u", P, k);
                    v = l == 0 ? name_at[k] : l == 1 ? tables[t].records[k] : wide_hud_name_hash(name);
                } else {
                    u64_at(q, n, from + 8 * (uint64_t)old++, &v);
                }
                put64(q, list + 8 * (uint64_t)r, v);
            }
        }
    }
    uint64_t grown = 0;
    if (extra_at) {
        uint32_t v, src;
        grown = put_at(q, &at, NULL, 4 * ((size_t)mats.count + extra), 8);
        for (uint32_t j = 0; j < mats.count; j++) {
            u32_at(q, n, extra_at + 4 * (uint64_t)j, &v);
            put32(q, grown + 4 * (uint64_t)j, v);
        }
        u32_at(q, n, extra_at + 4 * (uint64_t)mat_index, &src);
        for (uint32_t k = 0; k < extra; k++) put32(q, grown + 4 * ((uint64_t)mats.count + k), src);
    }
    for (int t = 0; t < 3; t++) {
        put64(q, tables[t].slot, lists[t][0]), put64(q, tables[t].slot + 8, lists[t][1]);
        put64(q, tables[t].slot + 16, lists[t][2]), put32(q, tables[t].slot + 24, tables[t].t->count + extra);
    }
    if (extra_at) put64(q, MATERIAL_EXTRA, grown), put32(q, MATERIAL_EXTRA + 8, mats.count + extra);
    put32(q, POOL_SECTION + 8, (uint32_t)(at - pool));
    put32(q, FILE_SIZE, (uint32_t)at);
    if (!check_split(q, (size_t)at, sp, u, &orig, why2, sizeof why2)) {
        snprintf(why, why_n, "%s reads otherwise after the split of %s (%s)", L, P, why2);
        goto done;
    }
    *out = q, *out_n = (size_t)at, q = NULL;
    say(report, report_n, "%s in %d pieces", P, sp->npieces);
    result = (int)extra;
done:
    free(q);
    free(codes);
    return result;
}

int wide_hud_split(const uint8_t *p, size_t n, const char *member, const WideHudShift *s, uint8_t **out, size_t *out_n,
                   char *report, size_t report_n, char *why, size_t why_n) {
    const Plan *plan = plan_of(member);
    const Split *sp = plan ? split_of(plan->layout) : NULL;
    *out = NULL;
    if (report && report_n) report[0] = 0;
    if (!sp || !(sp->group & groups_of(s))) return 0;
    return split_layout(p, n, sp, unit_of(sp->group, s), out, out_n, report, report_n, why, why_n);
}

enum { MAX_COPIES = 16 };
typedef struct {
    size_t at[MAX_COPIES], size[MAX_COPIES];
    unsigned slot[MAX_COPIES];
    int copies;
} Copies;

static int copies_of(const uint8_t *sarc, size_t n, const char *member, Copies *c, char *why, size_t why_n) {
    unsigned next = 0;
    size_t a, s;
    int found;
    c->copies = 0;
    while ((found = sarc_next(sarc, n, member, &next, &a, &s)) == 1) {
        if (c->copies == MAX_COPIES) {
            snprintf(why, why_n, "the layout archive holds %s more than %d times", member, MAX_COPIES);
            return 0;
        }
        c->at[c->copies] = a, c->size[c->copies] = s, c->slot[c->copies++] = next - 1;
    }
    if (found < 0) {
        snprintf(why, why_n, "the layout archive is damaged");
        return 0;
    }
    if (!c->copies) {
        snprintf(why, why_n, "the layout archive has no %s", member);
        return 0;
    }
    for (int i = 0; i < c->copies; i++)
        for (int j = 0; j < i; j++)
            if (!(c->at[i] == c->at[j] && c->size[i] == c->size[j]) && c->at[i] < c->at[j] + c->size[j] &&
                c->at[j] < c->at[i] + c->size[i]) {
                snprintf(why, why_n, "two copies of %s in the layout archive overlap", member);
                return 0;
            }
    return 1;
}

static const char *edit_name(int groups) {
    return groups == G_HUD ? "wide hud" : groups & G_HUD ? "wide hud and menus" : groups & G_FULL ? "wide menus"
                                                                                                    : "widescreen: menus";
}

static const Split *split_in(const Plan *plan, int groups) {
    const Split *sp = split_of(plan->layout);
    return sp && (sp->group & groups) ? sp : NULL;
}

static int trial(const uint8_t *sarc, const Copies *c, const Plan *plan, const char *member, const WideHudShift *s,
                 int groups, char *report, size_t report_n, char *split, size_t split_n, char *why, size_t why_n) {
    char got[1200], cut[120];
    const Split *sp = split_in(plan, groups);
    report[0] = split[0] = 0;
    for (int j = 0; j < c->copies; j++) {
        uint8_t *t = malloc(c->size[j] ? c->size[j] : 1), *o = NULL;
        size_t on = 0;
        if (!t) return -1;
        memcpy(t, sarc + c->at[j], c->size[j]);
        int r = lay_out(t, c->size[j], member, s, groups, got, sizeof got, why, why_n);
        int k = r < 0 || !sp ? 0 : split_layout(t, c->size[j], sp, unit_of(sp->group, s), &o, &on, cut, sizeof cut,
                                                why, why_n);
        free(t);
        free(o);
        if (r < 0 || k < 0) return 0;
        if (r > 0) snprintf(report, report_n, "%s", got);
        if (k > 0) snprintf(split, split_n, "%s", cut);
    }
    return 1;
}

static int archive_groups(int archive, const WideHudShift *s) {
    int g = 0, want = groups_of(s);
    for (int i = 0; i < NPLANS; i++)
        if (plans[i].archive == archive) g |= plan_groups(&plans[i]) & want;
    return g;
}

const char *wide_hud_archive_path(int archive) {
    return archive == WIDE_HUD_TITLE ? WIDE_HUD_TITLE_ARCHIVE : archive == WIDE_HUD_COMMON ? WIDE_HUD_ARCHIVE : NULL;
}

static int archive_in(int archive, uint8_t **sarc, size_t *n, const WideHudShift *s, int *added, char *why,
                      size_t why_n) {
    Copies c[NPLANS];
    char member[64], report[NPLANS][1200], split[NPLANS][120];
    int want = groups_of(s), groups[NPLANS] = {0}, any = 0;
    *added = 0;
    for (int i = 0; i < NPLANS; i++) {
        c[i].copies = 0;
        if (plans[i].archive != archive || !(plan_groups(&plans[i]) & want)) continue;
        any = 1;
        snprintf(member, sizeof member, "%s.buil", plans[i].layout);
        if (!copies_of(*sarc, *n, member, &c[i], why, why_n)) {
            if (plan_groups(&plans[i]) & want & (G_HUD | G_CENTER)) return -1;
            rt_log("wide menus: %s; %s stays as the game lays it out", why, plans[i].layout);
            c[i].copies = 0;
        }
    }
    if (!any) return 0;
    for (int i = 0; i < NPLANS; i++)
        for (int a = 0; a < c[i].copies; a++)
            for (int j = 0; j < i; j++)
                for (int b = 0; b < c[j].copies; b++)
                    if (c[i].at[a] < c[j].at[b] + c[j].size[b] && c[j].at[b] < c[i].at[a] + c[i].size[a]) {
                        snprintf(why, why_n, "%s and %s share bytes in the layout archive", plans[i].layout,
                                 plans[j].layout);
                        return -1;
                    }
    for (int i = 0; i < NPLANS; i++) {
        if (!c[i].copies) continue;
        snprintf(member, sizeof member, "%s.buil", plans[i].layout);
        int g = plan_groups(&plans[i]) & want, r;
        const Split *sp = split_in(&plans[i], g);
        if (sp && s->no_growth) {
            snprintf(why, why_n, "the layout archive may not grow for %s's pieces", sp->pane);
            r = 0;
        } else {
            r = trial(*sarc, &c[i], &plans[i], member, s, g, report[i], sizeof report[i], split[i], sizeof split[i],
                      why, why_n);
        }
        if (r < 0) {
            snprintf(why, why_n, "out of memory");
            return -1;
        }
        if (!r && (g & G_FULL)) {
            char why_full[240];
            snprintf(why_full, sizeof why_full, "%s", why);
            g &= ~G_FULL;
            r = !g ? 1 : trial(*sarc, &c[i], &plans[i], member, s, g, report[i], sizeof report[i], split[i],
                               sizeof split[i], why, why_n);
            if (r < 0) {
                snprintf(why, why_n, "out of memory");
                return -1;
            }
            if (r)
                rt_log("wide menus: %s; %s keeps the game's menu layout%s", why_full, plans[i].layout,
                       g ? " (the HUD's edit of it goes ahead)" : "");
        }
        if (!r) return -1;
        groups[i] = g;
    }
    int total = 0, fields[NPLANS] = {0}, pieces[NPLANS] = {0}, count = sarc_count(*sarc, *n);
    for (int i = 0; i < NPLANS; i++) {
        if (!groups[i]) continue;
        snprintf(member, sizeof member, "%s.buil", plans[i].layout);
        for (int j = 0; j < c[i].copies; j++) {
            int r = lay_out(*sarc + c[i].at[j], c[i].size[j], member, s, groups[i], NULL, 0, why, why_n);
            if (r < 0) return -1;
            fields[i] += r;
        }
        total += fields[i];
    }
    int splits_asked = 0, result = -1, grown = 0;
    for (int i = 0; i < NPLANS; i++) splits_asked |= groups[i] && split_in(&plans[i], groups[i]);
    const uint8_t **with = splits_asked && count > 0 ? calloc((size_t)count, sizeof *with) : NULL;
    size_t *with_n = splits_asked && count > 0 ? calloc((size_t)count, sizeof *with_n) : NULL;
    if (splits_asked && (!with || !with_n)) {
        snprintf(why, why_n, count < 0 ? "the layout archive is damaged" : "out of memory");
        goto done;
    }
    for (int i = 0; i < NPLANS; i++) {
        const Split *sp = groups[i] ? split_in(&plans[i], groups[i]) : NULL;
        for (int j = 0; sp && j < c[i].copies; j++) {
            uint8_t *o = NULL;
            size_t on = 0;
            int k = split_layout(*sarc + c[i].at[j], c[i].size[j], sp, unit_of(sp->group, s), &o, &on, NULL, 0, why,
                                 why_n);
            if (k < 0) goto done;
            if (!k) continue;
            if ((int)c[i].slot[j] >= count || with[c[i].slot[j]]) {
                free(o);
                snprintf(why, why_n, "the layout archive is damaged");
                goto done;
            }
            with[c[i].slot[j]] = o, with_n[c[i].slot[j]] = on;
            pieces[i] += k, grown = 1;
        }
    }
    if (grown) {
        size_t rn = 0;
        uint8_t *rebuilt = sarc_rebuild(*sarc, *n, with, with_n, &rn);
        if (!rebuilt) {
            snprintf(why, why_n, "the layout archive cannot be laid out again around its grown layouts");
            goto done;
        }
        free(*sarc);
        *sarc = rebuilt, *n = rn;
    }
    for (int i = 0; i < NPLANS; i++) {
        if (!fields[i] && !pieces[i]) continue;
        char parts[200] = "";
        if (pieces[i])
            snprintf(parts, sizeof parts, ", %s, %d new panes", split[i], pieces[i]);
        rt_log("%s: %s (%d cop%s, %d fields%s): %s%s", edit_name(groups[i]), plans[i].layout, c[i].copies,
               c[i].copies == 1 ? "y" : "ies", fields[i], parts, report[i],
               strlen(report[i]) + 1 >= sizeof report[i] ? " ..." : "");
        *added += pieces[i];
    }
    result = total + *added;
done:
    for (int k = 0; with && k < count; k++) free((void *)with[k]);
    free(with);
    free(with_n);
    return result;
}

int wide_hud_archive_in(int archive, uint8_t **sarc, size_t *n, const WideHudShift *s, char *why, size_t why_n) {
    int added;
    return archive_in(archive, sarc, n, s, &added, why, why_n);
}

int wide_hud_archive(uint8_t **sarc, size_t *n, const WideHudShift *s, char *why, size_t why_n) {
    return wide_hud_archive_in(WIDE_HUD_COMMON, sarc, n, s, why, why_n);
}

static void shift_words(const WideHudShift *s, char *out, size_t out_n) {
    out[0] = 0;
    if (s->out > 0) say(out, out_n, "the HUD's edge elements %g units out", (double)s->out);
    if (s->menus > 0) say(out, out_n, "%sthe menus over the whole picture, %g units wider on each side", out[0] ? ", " : "",
                          (double)s->menus);
    if (s->squeeze > 0 && s->squeeze < 1)
        say(out, out_n, "%sthe loading icon at %.3f of its width", out[0] ? ", " : "", (double)s->squeeze);
}

int wide_hud_build_in(int archive, const void *in, size_t n, const void *zsdic, size_t zsdic_n, const WideHudShift *s,
                      void **out_z, size_t *out_n) {
    uint64_t t0 = plat_now_ns();
    *out_z = NULL;
    int has = archive_groups(archive, s);
    WideHudShift own = {has & G_HUD ? s->out : 0, has & G_CENTER ? s->squeeze : 1, has & G_FULL ? s->menus : 0,
                        s->no_growth};
    char what[160];
    shift_words(&own, what, sizeof what);
    if (!what[0]) return 0;
    const char *noun = archive == WIDE_HUD_TITLE ? "the title layout archive" : "the layout archive";
    int result = -1;
    uint8_t *dics = NULL, *raw = NULL;
    void *z = NULL;
    ZSTD_DDict *dd = NULL;
    ZSTD_CDict *cd = NULL;
    char why[240] = "";
    unsigned id = ZSTD_getDictID_fromFrame(in, n);
    size_t dn = 0, dic_at = 0, dic_n = 0;
    if (id && (!(dics = zsdic_unpack(zsdic, zsdic_n, MAX_RAW, &dn)) || !zsdic_find(dics, dn, id, &dic_at, &dic_n))) {
        snprintf(why, sizeof why, "no dictionary %u in Pack/ZsDic.pack.zs", id);
        goto done;
    }
    if (id && (!(dd = ZSTD_createDDict(dics + dic_at, dic_n)) || !(cd = ZSTD_createCDict(dics + dic_at, dic_n, 3)))) {
        snprintf(why, sizeof why, "dictionary %u of Pack/ZsDic.pack.zs cannot be loaded", id);
        goto done;
    }
    unsigned long long rn = ZSTD_getFrameContentSize(in, n);
    if (rn == ZSTD_CONTENTSIZE_UNKNOWN || rn == ZSTD_CONTENTSIZE_ERROR || !rn || rn > MAX_RAW) {
        snprintf(why, sizeof why, "the layout archive is not a zstd frame with a size");
        goto done;
    }
    if (!(raw = malloc((size_t)rn))) {
        snprintf(why, sizeof why, "out of memory");
        goto done;
    }
    ZSTD_DCtx *dctx = ZSTD_createDCtx();
    size_t got = !dctx ? 0 : dd ? ZSTD_decompress_usingDDict(dctx, raw, (size_t)rn, in, n, dd)
                                : ZSTD_decompressDCtx(dctx, raw, (size_t)rn, in, n);
    ZSTD_freeDCtx(dctx);
    if (ZSTD_isError(got) || got != rn) {
        snprintf(why, sizeof why, "the layout archive does not decompress");
        goto done;
    }
    size_t raw_n = (size_t)rn;
    int added = 0, changed = archive_in(archive, &raw, &raw_n, s, &added, why, sizeof why);
    if (changed <= 0) {
        result = changed;
        goto done;
    }
    size_t cap = ZSTD_compressBound(raw_n), zn = 0;
    ZSTD_CCtx *cctx = ZSTD_createCCtx();
    if (cctx && (z = malloc(cap)))
        zn = cd ? ZSTD_compress_usingCDict(cctx, z, cap, raw, raw_n, cd)
                : ZSTD_compressCCtx(cctx, z, cap, raw, raw_n, 3);
    ZSTD_freeCCtx(cctx);
    if (!z || !zn || ZSTD_isError(zn)) {
        snprintf(why, sizeof why, "the layout archive cannot be compressed again");
        goto done;
    }
    *out_z = z, *out_n = zn, z = NULL;
    result = 1;
    char grew[120] = "";
    if (added)
        snprintf(grew, sizeof grew, " and %d new panes, the archive %llu -> %zu bytes", added, (unsigned long long)rn,
                 raw_n);
    rt_log("wide hud: %s edited (%s), %d fields%s (%.0f ms)", noun, what, changed - added, grew,
           (double)(plat_now_ns() - t0) / 1e6);
done:
    if (result == 0) rt_log("wide hud: %s has these edits already (%s)", noun, what);
    if (result < 0)
        rt_log("wide hud: %s; %s is served as it is (%s)", why, noun,
               archive == WIDE_HUD_TITLE ? "the game's own keeps the title menu's backdrop in the middle 16:9"
                                         : "the game's own keeps the HUD and the loading icon where the game lays "
                                           "them out");
    free(z);
    free(raw);
    free(dics);
    ZSTD_freeDDict(dd);
    ZSTD_freeCDict(cd);
    return result;
}

int wide_hud_build(const void *in, size_t n, const void *zsdic, size_t zsdic_n, const WideHudShift *s, void **out_z,
                   size_t *out_n) {
    return wide_hud_build_in(WIDE_HUD_COMMON, in, n, zsdic, zsdic_n, s, out_z, out_n);
}
