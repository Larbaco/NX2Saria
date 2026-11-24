#include <stdatomic.h>
#include <string.h>

#include "cfg.h"
#include "ov.h"
#include "platform/platform.h"
#include "ui_text.h"

typedef struct { const char *lang; const char *const *text; } Row;

#define LANG_NAMES \
    [UI_MENU_LANGNAME_EN] = "English", [UI_MENU_LANGNAME_JA] = "日本語", [UI_MENU_LANGNAME_FR] = "Français", \
    [UI_MENU_LANGNAME_DE] = "Deutsch", [UI_MENU_LANGNAME_IT] = "Italiano", [UI_MENU_LANGNAME_ES] = "Español", \
    [UI_MENU_LANGNAME_NL] = "Nederlands", [UI_MENU_LANGNAME_RU] = "Русский", \
    [UI_MENU_LANGNAME_ZH_HANS] = "简体中文", [UI_MENU_LANGNAME_ZH_HANT] = "繁體中文", [UI_MENU_LANGNAME_KO] = "한국어", \
    [UI_MENU_LANGNAME_PT_BR] = "Português (Brasil)", [UI_MENU_LANGNAME_EN_GB] = "English (UK)", \
    [UI_MENU_LANGNAME_FR_CA] = "Français (Canada)", [UI_MENU_LANGNAME_ES_419] = "Español (Latinoamérica)", \
    [UI_MENU_LANGNAME_PL] = "Polski"

#define SWKBD(title, prompt, name, ok, cancel, hint) \
    [UI_SWKBD_TITLE] = title, [UI_SWKBD_PROMPT] = prompt, [UI_SWKBD_DEFAULT_NAME] = name, [UI_SWKBD_OK] = ok, \
    [UI_SWKBD_CANCEL] = cancel, [UI_SWKBD_PAD_HINT] = hint, LANG_NAMES

#ifdef _WIN32
#define DLSS_FILE "nvngx_dlss.dll"
#define EXE_FILE "nx2saria.exe"
#else
#define DLSS_FILE "libnvidia-ngx-dlss.so"
#define EXE_FILE "nx2saria"
#endif

static const char *const text_en[UI_TEXT_COUNT] = {
    SWKBD("Text entry", "Enter a name", "Horse", "OK", "Cancel", "Controller: A = OK, B = Cancel"),

    [UI_MENU_TITLE] = "Settings",
    [UI_MENU_TAB_SCREEN] = "Screen", [UI_MENU_TAB_GAME] = "Game", [UI_MENU_TAB_SOUND] = "Sound",
    [UI_MENU_TAB_CONTROLS] = "Controls", [UI_MENU_TAB_AMIIBO] = "Amiibo",
    [UI_MENU_CONTINUE] = "Continue", [UI_MENU_QUIT] = "Quit game",
    [UI_MENU_ON] = "On", [UI_MENU_OFF] = "Off", [UI_MENU_AUTO] = "Automatic", [UI_MENU_DEFAULT_MARK] = "default",
    [UI_MENU_AFTER_RESTART] = "after restart", [UI_MENU_SET_BY_ENV] = "set by environment variable",
    [UI_MENU_SAVE_FAILED] = "could not save settings",
    [UI_MENU_INI_UNKNOWN] = "nx2saria.ini: not settings of this version, ignored: %s",
    [UI_MENU_VERSION] = "NX2Saria %s, game %s",

    [UI_MENU_FULLSCREEN] = "Fullscreen", [UI_MENU_FULLSCREEN_DESC] = "Play in fullscreen. F11 switches it.",
    [UI_MENU_WINDOW_SIZE] = "Window size", [UI_MENU_WINDOW_SIZE_DESC] = "Window size outside fullscreen.",
    [UI_MENU_PRESENT_DESC] = "Stops screen tearing. Off lowers input delay slightly.",
    [UI_MENU_PRESENT_VSYNC] = "VSync", [UI_MENU_PRESENT_IMMEDIATE] = "Immediate",
    [UI_MENU_PRESENT_IN_USE] = "in use: %s",
    [UI_MENU_LOW_LATENCY] = "Low latency",
    [UI_MENU_LOW_LATENCY_DESC] = "The graphics driver holds each frame's start, so the controller is read as late as "
                                 "possible (NVIDIA Reflex, AMD Anti-Lag). It keeps the frame rate limit too. Boost "
                                 "keeps the graphics card at full clock.",
    [UI_MENU_LOW_LATENCY_BOOST] = "On + Boost",
    [UI_MENU_LOW_LATENCY_NO_DRIVER] = "this graphics driver has no low-latency mode",
    [UI_MENU_LOW_LATENCY_DLSSG] = "DLSS frame generation uses its own",
    [UI_MENU_LOW_LATENCY_FG] = "off while frame generation runs on this card",
    [UI_MENU_LOW_LATENCY_FAILED] = "in use: off, the driver refused it (see lowlat in the log)",
    [UI_MENU_GPU] = "Graphics card", [UI_MENU_GPU_DESC] = "The graphics card that runs the game.",
    [UI_MENU_GPU_AUTO] = "Automatic",
    [UI_MENU_PERF_OVERLAY] = "Performance overlay",
    [UI_MENU_PERF_OVERLAY_DESC] = "Shows frame rate and system stats. F10 switches modes.",
    [UI_MENU_PERF_OFF] = "Off", [UI_MENU_PERF_FPS] = "FPS", [UI_MENU_PERF_FULL] = "Full",
    [UI_PERF_LINE_FPS] = "%.0f fps",
    [UI_MENU_GRAPHICS] = "Picture",
    [UI_MENU_OUTPUT_RES] = "Game resolution",
    [UI_MENU_OUTPUT_RES_DESC] = "The resolution the game draws at. Higher needs a stronger GPU.",
    [UI_MENU_UPSCALER_FSR] = "AMD FSR 1", [UI_MENU_UPSCALER_BILINEAR] = "Bilinear",
    [UI_MENU_UPSCALING_NATIVE] = "Native (100%)",
    [UI_MENU_GAME_ASPECT] = "Game aspect",
    [UI_MENU_GAME_ASPECT_DESC] = "Matches the picture to a taller or wider shape. Taller shapes keep 16:9's horizontal view and show more above and below; the HUD stays centered.",
    [UI_MENU_GAME_ASPECT_16_9] = "16:9 (the game's)", [UI_MENU_GAME_ASPECT_WINDOW] = "Window shape",
    [UI_MENU_GAME_ASPECT_16_10] = "16:10", [UI_MENU_GAME_ASPECT_3_2] = "3:2",
    [UI_MENU_GAME_ASPECT_21_9] = "21:9", [UI_MENU_GAME_ASPECT_32_9] = "32:9",
    [UI_MENU_WIDE_HUD] = "HUD position",
    [UI_MENU_WIDE_HUD_DESC] = "Where the HUD sits on a picture wider than 16:9. Middle 16:9 keeps the game's layout; Screen edges puts the hearts and magic top left, the item slot top right, and the minimap, keys and rupee count on the right edge.",
    [UI_MENU_WIDE_HUD_CENTER] = "Middle 16:9", [UI_MENU_WIDE_HUD_EDGES] = "Screen edges",
    [UI_MENU_WIDE_MENUS] = "Menus",
    [UI_MENU_WIDE_MENUS_DESC] = "The pause menu, system menu, map and loading screens on a picture wider than 16:9. Fill the screen lays them out over the whole picture; Black bars keeps them in the middle 16:9, with black sides.",
    [UI_MENU_WIDE_MENUS_FULL] = "Fill the screen", [UI_MENU_WIDE_MENUS_CENTER] = "Black bars",
    [UI_MENU_FOV] = "Field of view",
    [UI_MENU_FOV_DESC] = "How much of the world the camera shows, in percent of the game's own.",
    [UI_MENU_ANISO] = "Texture filtering",
    [UI_MENU_ANISO_DESC] = "Anisotropic filtering of surface textures: ground, roads and walls seen at a shallow angle stay sharp into the distance instead of blurring. Game: the level the game asks for, as on the Switch.",
    [UI_MENU_ANISO_GAME] = "Game",
    [UI_MENU_TEX_DETAIL] = "Texture detail",
    [UI_MENU_TEX_DETAIL_DESC] = "Sharpness of large surface textures. Reduced and Low leave out their largest versions, so that an area fits the video memory of 6 and 8 GB cards; only surfaces seen up close look softer. Automatic picks by the card's video memory.",
    [UI_MENU_TEX_DETAIL_AUTO] = "Automatic: %s",
    [UI_MENU_TEX_DETAIL_FULL] = "Full", [UI_MENU_TEX_DETAIL_REDUCED] = "Reduced", [UI_MENU_TEX_DETAIL_LOW] = "Low",
    [UI_MENU_TEX_CLOBBER] = "Fix overwritten textures",
    [UI_MENU_TEX_CLOBBER_DESC] = "Keeps textures intact when the game draws another image over the same memory. "
                               "This fixes green squares in fog and can use more video memory.",
    [UI_MENU_TEX_CLOBBER_NOTE] = "fixes green squares in fog",
    [UI_MENU_SHARPNESS] = "FSR sharpness",
    [UI_MENU_SHARPNESS_DESC] = "How sharp FSR makes the picture. 0 turns it off.",
    [UI_MENU_SHARPNESS_NEEDS_FSR] = "only with AMD FSR 1 or FSR 3.1",
    [UI_MENU_HDR] = "HDR output",
    [UI_MENU_HDR_DESC] = "Brighter highlights on an HDR display. Turn on HDR in Windows first.",
    [UI_MENU_HDR_PEAK] = "Peak brightness",
    [UI_MENU_HDR_PEAK_DESC] = "Your display's peak brightness in nits.",
    [UI_MENU_HDR_GAME] = "Game brightness",
    [UI_MENU_HDR_GAME_DESC] = "Brightness of white in the game, in nits.",
    [UI_MENU_HDR_UI] = "HUD and menu brightness",
    [UI_MENU_HDR_UI_DESC] = "Brightness of the HUD and this menu, in nits.",
    [UI_MENU_HDR_HUE] = "Hue correction",
    [UI_MENU_HDR_HUE_DESC] = "How much bright colours keep the hue they have before the tone mapper; 0 lets them shift "
                             "as the curve takes them.",
    [UI_MENU_HDR_GAMMA] = "Gamma correction",
    [UI_MENU_HDR_GAMMA_DESC] = "Shows the picture as a 2.2 or BT.1886 (2.4) display would, as the game was made to be "
                               "seen; Off keeps the sRGB curve, with lighter shadows.",
    [UI_MENU_HDR_TONEMAP] = "Tone mapper",
    [UI_MENU_HDR_TONEMAP_DESC] = "Vanilla: the game's own curve at SDR brightness. PsychoV35: the scene through a model "
                                 "of the eye's response, matched to the game's mid grey and tone, up to your display's "
                                 "peak. RenoDRT: the game's SDR look carried up to the peak by brightness.",
    [UI_MENU_HDR_EXPOSURE] = "Exposure",
    [UI_MENU_HDR_EXPOSURE_DESC] = "Brightness of the whole picture before the tone mapper; 100 % leaves it unchanged.",
    [UI_MENU_HDR_HIGHLIGHTS] = "Highlights",
    [UI_MENU_HDR_HIGHLIGHTS_DESC] = "Brightness of the bright parts; 50 leaves them unchanged.",
    [UI_MENU_HDR_SHADOWS] = "Shadows",
    [UI_MENU_HDR_SHADOWS_DESC] = "Brightness of the dark parts; 50 leaves them unchanged.",
    [UI_MENU_HDR_CONTRAST] = "Contrast",
    [UI_MENU_HDR_CONTRAST_DESC] = "Contrast around mid grey; 50 leaves it unchanged.",
    [UI_MENU_HDR_SATURATION] = "Saturation",
    [UI_MENU_HDR_SATURATION_DESC] = "Strength of all colours; 50 leaves it unchanged.",
    [UI_MENU_HDR_HIGHLIGHT_SAT] = "Highlight saturation",
    [UI_MENU_HDR_HIGHLIGHT_SAT_DESC] = "Strength of the colours of the brightest parts; 50 leaves it unchanged.",
    [UI_MENU_HDR_BLOWOUT] = "Blowout",
    [UI_MENU_HDR_BLOWOUT_DESC] = "Turns the brightest parts towards white; 0 keeps their colour.",
    [UI_MENU_HDR_FLARE] = "Flare",
    [UI_MENU_HDR_FLARE_DESC] = "Lifts the darkest parts as a little stray light would; 0 keeps black black.",
    [UI_MENU_HDR_GRADE] = "Colour grading",
    [UI_MENU_HDR_GRADE_DESC] = "How much of the game's own colour grading the HDR picture keeps; 0 is the scene without it.",
    [UI_MENU_HDR_MIDGRAY] = "Mid grey",
    [UI_MENU_HDR_MIDGRAY_DESC] = "Where the scene's mid grey lands. Vanilla: where the game's own curve puts it. Render: "
                                 "at the scene's value, a little darker.",
    [UI_MENU_HDR_MATCH_SLOPE] = "Match tone slope",
    [UI_MENU_HDR_MATCH_SLOPE_DESC] = "Gives the mid tones the contrast of the game's own curve.",
    [UI_MENU_HDR_TONE_SLOPE] = "Tone slope",
    [UI_MENU_HDR_TONE_SLOPE_DESC] = "Steepness of the tone curve through mid grey; 50 leaves it unchanged.",
    [UI_MENU_HDR_HUE_CLIP] = "Hue clip emulation",
    [UI_MENU_HDR_HUE_CLIP_DESC] = "How much bright colours turn as they do where the game's SDR picture clips: fire "
                                  "towards yellow and white. 0 keeps their hue.",
    [UI_MENU_DLSS_DLAA] = "DLAA (native)", [UI_MENU_FSR3_NATIVE] = "Native AA", [UI_MENU_DLSS_QUALITY] = "Quality", [UI_MENU_DLSS_BALANCED] = "Balanced",
    [UI_MENU_DLSS_PERFORMANCE] = "Performance", [UI_MENU_DLSS_ULTRA] = "Ultra Performance",
    [UI_MENU_AA] = "Anti-aliasing",
    [UI_MENU_AA_DESC] = "How the edges of the 3D picture are smoothed. Game SMAA is the game's own, as on the console. Off "
                        "leaves the edges as drawn. FXAA is quick and a little soft. SMAA finds more of the edges than the "
                        "game's and keeps detail sharp. TAA is DLSS or FSR at full size: the steadiest edges in motion; it "
                        "sets the upscaler. With DLSS or FSR set under Upscaling their TAA always runs, and the "
                        "choice here runs along with it. Takes effect at once.",
    [UI_MENU_AA_GAME] = "Game SMAA",
    [UI_MENU_AA_FXAA] = "FXAA",
    [UI_MENU_AA_SMAA] = "SMAA (Ultra)",
    [UI_MENU_AA_TAA_DLSS] = "TAA (NVIDIA DLAA)",
    [UI_MENU_AA_TAA_FSR] = "TAA (AMD FSR 3.1)",
    [UI_MENU_AA_TAA_FSR4] = "TAA (%s)",
    [UI_MENU_AA_WITH_TAA] = "plus TAA from Upscaling",

    [UI_MENU_FPS_LIMIT] = "Frame rate limit",
    [UI_MENU_FPS_LIMIT_DESC] = "The highest frame rate the game draws at. Frame generation adds its frames on top.",
    [UI_MENU_FPS_DISPLAY] = "Monitor rate", [UI_MENU_FPS_DISPLAY_HZ] = "Monitor rate (%.0f Hz)",
    [UI_MENU_FPS_UNLIMITED] = "Unlimited",
    [UI_MENU_FPS_RTSS] = "limited by RivaTuner Statistics Server",
    [UI_MENU_FPS_RTSS_READONLY] = "RivaTuner needs the game run as administrator",
    [UI_MENU_FPS_RTSS_IGNORED] = "RivaTuner does not limit this game; the game does",
    [UI_MENU_FPS_RTSS_TWO] = "RivaTuner's own limit also applies: two limiters",
    [UI_MENU_GAME_LANG] = "Game language", [UI_MENU_GAME_LANG_DESC] = "Language of the game's text and voices.",
    [UI_MENU_GAME_LANG_AUTO] = "Automatic",
    [UI_MENU_OPERATION_MODE] = "Console mode",
    [UI_MENU_OPERATION_MODE_DESC] = "Runs the game as docked or handheld.",
    [UI_MENU_MODE_DOCKED] = "Docked", [UI_MENU_MODE_HANDHELD] = "Handheld",
    [UI_MENU_ASSUME_FOCUS] = "Keep running out of focus",
    [UI_MENU_ASSUME_FOCUS_DESC] = "Keeps the game running when you switch to another window.",
    [UI_MENU_TEXT_SPEED] = "Text speed",
    [UI_MENU_TEXT_SPEED_DESC] = "How fast the game's message windows show their text. Fast types three times as fast; "
                                "instant shows each page at once. Text the game does not let you hurry, such as "
                                "lines timed to a cutscene or a voice, keeps its speed.",
    [UI_MENU_TEXT_SPEED_NORMAL] = "Normal", [UI_MENU_TEXT_SPEED_FAST] = "Fast",
    [UI_MENU_TEXT_SPEED_INSTANT] = "Instant",

    [UI_MENU_VOLUME] = "Volume", [UI_MENU_VOLUME_DESC] = "Overall game volume.", [UI_MENU_MENU_SOUNDS] = "Menu sounds", [UI_MENU_MENU_SOUNDS_DESC] = "Volume of this menu's sounds.",
    [UI_MENU_AUDIO_CHANNELS] = "Speakers",
    [UI_MENU_AUDIO_CHANNELS_DESC] = "Stereo or 5.1 surround sound.",
    [UI_MENU_CHANNELS_AUTO] = "Automatic", [UI_MENU_CHANNELS_STEREO] = "Stereo", [UI_MENU_CHANNELS_51] = "5.1 surround",

    [UI_MENU_MOTION] = "Motion controls", [UI_MENU_MOTION_DESC] = "Aim with the controller's gyro.",
    [UI_MENU_SPRINT_BUTTON] = "Sprint button",
    [UI_MENU_SPRINT_BUTTON_DESC] = "A button that makes Link sprint without rolling first. L3, the left stick's "
                                   "click, has no other use. The game's B button sprints only while Link moves and "
                                   "puts the weapon away as usual otherwise; with the game's Swap Jump Controls on, B "
                                   "is the jump, so use L3.",
    [UI_MENU_SPRINT_OFF] = "Off", [UI_MENU_SPRINT_B] = "The game's B button",
    [UI_MENU_SPRINT_L3] = "L3 (left stick)",
    [UI_MENU_SPRINT_MODE] = "Sprint mode",
    [UI_MENU_SPRINT_MODE_DESC] = "Toggle: press once to sprint and again to stop. The sprint goes on through jumps "
                                 "and ends when Link stops, attacks, rolls, targets, uses an item, swims or climbs. "
                                 "Hold: Link sprints while the button is held.",
    [UI_MENU_SPRINT_TOGGLE] = "Toggle", [UI_MENU_SPRINT_HOLD] = "Hold",
    [UI_MENU_RUMBLE_STRENGTH] = "Vibration strength",
    [UI_MENU_RUMBLE_STRENGTH_DESC] = "How strong the vibration is.",
    [UI_MENU_RUMBLE_CURVE] = "Vibration curve",
    [UI_MENU_RUMBLE_CURVE_DESC] = "Lower values make light vibration stronger.",
    [UI_MENU_RUMBLE_TRIGGERS] = "Trigger vibration (Xbox)",
    [UI_MENU_RUMBLE_TRIGGERS_DESC] = "Also vibrates the triggers on Xbox controllers.",
    [UI_MENU_RUMBLE_HD] = "HD vibration (Switch)",
    [UI_MENU_RUMBLE_HD_DESC] = "Sends the game's detailed vibration to a Switch Pro Controller or Joy-Con as it is. "
                               "The vibration strength still applies: its default is the game's own.",
    [UI_MENU_RUMBLE_HD_NEEDS] = "Needs a Switch Pro Controller or Joy-Con",
    [UI_MENU_GROUP_GYRO] = "Gyro",
    [UI_MENU_GYRO_CALIBRATION] = "Gyro calibration",
    [UI_MENU_GYRO_CALIBRATION_DESC] = "Learns the gyro's reading at rest whenever the controller lies still for a "
                                      "moment and subtracts it, so the aim does not drift. Set the controller down for "
                                      "a second after connecting it.",
    [UI_MENU_GYRO_DEADZONE] = "Gyro dead zone",
    [UI_MENU_GYRO_DEADZONE_DESC] = "Turns slower than the level are ignored; the levels are the console's. Low "
                                   "steadies a still controller and keeps slow aiming; Standard and High steady a "
                                   "controller that jitters but swallow the slowest turns. Off passes every reading.",
    [UI_MENU_GYRO_DEADZONE_OFF] = "Off", [UI_MENU_GYRO_DEADZONE_LOW] = "Low",
    [UI_MENU_GYRO_DEADZONE_STANDARD] = "Standard", [UI_MENU_GYRO_DEADZONE_HIGH] = "High",
    [UI_MENU_GYRO_TURN_SENS] = "Turn sensitivity",
    [UI_MENU_GYRO_TURN_SENS_DESC] = "How far the aim moves left and right for a turn of the controller. 100 % passes "
                                    "the turn unchanged.",
    [UI_MENU_GYRO_TURN_INVERT] = "Invert turn", [UI_MENU_GYRO_TURN_INVERT_DESC] = "Swaps the gyro's left and right.",
    [UI_MENU_GYRO_TILT_SENS] = "Tilt sensitivity",
    [UI_MENU_GYRO_TILT_SENS_DESC] = "How far the aim moves up and down for a tilt of the controller. 100 % passes "
                                    "the tilt unchanged.",
    [UI_MENU_GYRO_TILT_INVERT] = "Invert tilt", [UI_MENU_GYRO_TILT_INVERT_DESC] = "Swaps the gyro's up and down.",
    [UI_MENU_GYRO_NEEDS_MOTION] = "Needs motion controls on",
    [UI_MENU_PAD_LAYOUT] = "Button layout",
    [UI_MENU_PAD_LAYOUT_DESC] = "Sets which button acts as A. Nintendo: by position, A right, B bottom, X top, Y "
                                "left. Xbox and PlayStation: by label, A bottom, B right, X left, Y top (on a "
                                "PlayStation pad cross, circle, square and triangle). Automatic: Nintendo for a pad "
                                "labelled the Nintendo way, by label for the others.",
    [UI_MENU_PAD_LAYOUT_AUTO] = "Automatic", [UI_MENU_PAD_LAYOUT_NINTENDO] = "Nintendo (A right)",
    [UI_MENU_PAD_LAYOUT_XBOX] = "Xbox (A bottom)", [UI_MENU_PAD_LAYOUT_PLAYSTATION] = "PlayStation (A bottom)",
    [UI_MENU_PAD_LAYOUT_IN_USE] = "in use: %s",
    [UI_MENU_PROMPTS] = "Button prompts",
    [UI_MENU_PROMPTS_DESC] = "Which buttons the game's prompts show, in text, in the action prompt and on the "
                             "ocarina's notes. Nintendo: the game's own. Xbox, PlayStation and Steam Deck: that pad's "
                             "buttons (LB, RB, LT, RT, Menu, View; L1, R1, L2, R2, Options, Create; the Deck's A, B, X, "
                             "Y with L1, R1, L2, R2, Menu, View), and A, B, X and Y as the "
                             "button the layout above presses (with Nintendo (A right) the right button does A, so the "
                             "prompts for A show B, or circle). Keyboard and mouse: the key each button has (E, Ctrl, "
                             "Space, left click...), WASD for moving and the mouse for the camera. The launcher draws "
                             "the prompt files when it builds the game, the keys as nx2saria.ini binds them then; when "
                             "they are missing, update the game, or rebuild it (Rebuild the Game in the Windows "
                             "launcher, Update in the Linux AppImage). The game reads its prompts at start, so they "
                             "change after a restart. They are part of the game's files, so a button you remap in "
                             "Controls keeps its old prompt on screen until the game is rebuilt.",
    [UI_MENU_PROMPTS_NINTENDO] = "Nintendo", [UI_MENU_PROMPTS_XBOX] = "Xbox",
    [UI_MENU_PROMPTS_PLAYSTATION] = "PlayStation", [UI_MENU_PROMPTS_STEAMDECK] = "Steam Deck",
    [UI_MENU_PROMPTS_PC] = "Keyboard and mouse",
    [UI_MENU_PROMPTS_MISSING] = "prompt files missing: update the game, or rebuild it in the launcher",
    [UI_MENU_PROMPTS_FAILED] = "prompt files damaged: rebuild the game in the launcher",
    [UI_MENU_PROMPTS_RESTART] = "prompts change after restart",
    [UI_MENU_PROMPTS_OFF_SHAPE] = "off at a game resolution not 16:9",
    [UI_MENU_PROMPTS_OFF_MODS] = "off: a RomFS mod replaces their files",
    [UI_MENU_PROMPTS_OFF_PATCHES] = "off while NX2SARIA_ROMFS_PATCHES is set",
    [UI_MENU_RESTART_CONFIRM] = "Restart the game now? Progress since your last save is lost.",
    [UI_MENU_QUIT_CONFIRM] = "Quit the game? Progress since your last save is lost.",
    [UI_MENU_RESTART_YES] = "Restart",
    [UI_MENU_RESTART_COUNT] = "Restart (%d)",
    [UI_MENU_RESTART_SAVING] = "The game is saving: Restart is possible in a moment.",
    [UI_MENU_QUIT_SAVING] = "The game is saving: Quit is possible in a moment.",
    [UI_MENU_DRAFT_TITLE] = "Apply your changes?",
    [UI_MENU_DRAFT_APPLY] = "Apply",
    [UI_MENU_DRAFT_DISCARD] = "Discard",
    [UI_MENU_DRAFT_KEEP] = "Continue editing",
    [UI_MENU_DRAFT_RESTART_NOTE] = "The changes you made are applied and saved first.",
    [UI_MENU_DRAFT_QUIT_NOTE] = "The changes you have not applied are lost.",
    [UI_MENU_KEEP_TITLE] = "Keep these display settings?",
    [UI_MENU_KEEP_COUNTDOWN] = "Reverting in %d s",
    [UI_MENU_KEEP_YES] = "Keep",
    [UI_MENU_KEEP_REVERT] = "Revert",
    [UI_MENU_RESTART_NOW_TITLE] = "Some changes take effect after a restart",
    [UI_MENU_RESTART_NOW] = "Restart now",
    [UI_MENU_RESTART_LATER] = "Later",
    [UI_MENU_RELOAD_NONE] = "nx2saria.ini reloaded: no changes",
    [UI_MENU_RELOAD_DONE] = "nx2saria.ini reloaded: %d changed, %d after restart",
    [UI_MENU_RELOAD_REJECTED] = "nx2saria.ini reloaded: %d changed, %d after restart, %d rejected",
    [UI_MENU_RELOAD_FAILED] = "nx2saria.ini could not be read",
    [UI_MENU_RELOAD_BUSY] = "Close the settings menu to reload nx2saria.ini",
    [UI_MENU_GROUP_STICKS] = "Analog sticks",
    [UI_MENU_LSTICK_DEADZONE] = "Left stick dead zone", [UI_MENU_RSTICK_DEADZONE] = "Right stick dead zone",
    [UI_MENU_STICK_DEADZONE_DESC] = "How far a stick must move to register. Raise it if a stick drifts.",
    [UI_MENU_STICK_DEADZONE_NOW] = "stick now: %.0f %%",
    [UI_MENU_MOUSE_MODE] = "Mouse camera",
    [UI_MENU_MOUSE_MODE_DESC] = "Direct: the camera turns by how far the mouse moves, as in a PC game, with no stick dead zone or curve (games 1.1.1 and 1.4.3; other versions use stick emulation). Stick emulation: the mouse acts as the right stick.",
    [UI_MENU_MOUSE_STICK] = "Stick emulation",
    [UI_MENU_MOUSE_DIRECT] = "Direct",
    [UI_MENU_MENU_CURSOR] = "Mouse in the game's menus",
    [UI_MENU_MENU_CURSOR_DESC] = "In the game's own menus (title and save files, pause menu, options, shops, dialogue choices) the pointer shows: the item under it is selected, left click confirms, right click goes back and the wheel scrolls a list (on the pause menu: turns the page). Only after the mouse or a key was used last, so a gamepad sees the menus unchanged (games 1.1.1 and 1.4.3).",
    [UI_MENU_MOUSE_SENS] = "Mouse camera sensitivity",
    [UI_MENU_MOUSE_SENS_DESC] = "How fast the mouse turns the camera.",
    [UI_MENU_WALK_KEY] = "Walk key",
    [UI_MENU_WALK_KEY_DESC] = "Hold to walk on the keyboard.",
    [UI_MENU_WALK_KEY_TAKEN] = "also %s",
    [UI_MENU_WALK_SCALE] = "Walk scale",
    [UI_MENU_WALK_SCALE_DESC] = "The tilt the keyboard's sticks get while the walk key is held. Half is a walk, the "
                                "full tilt a run.",
    [UI_MENU_STICK_SENS] = "Stick camera speed",
    [UI_MENU_STICK_SENS_DESC] = "Right stick camera speed.",

    [UI_BINDS_HELP] = "Select a cell and press A or Enter, then the key, mouse button or controller button. Delete or "
                      "X clears a cell. A key in use moves here; a controller button swaps with the one it replaces.",
    [UI_BINDS_COL_KEY1] = "Key 1", [UI_BINDS_COL_KEY2] = "Key 2", [UI_BINDS_COL_PAD] = "Controller",
    [UI_BINDS_GROUP_BUTTONS] = "Buttons", [UI_BINDS_GROUP_MOVE] = "Movement", [UI_BINDS_GROUP_CAMERA] = "Camera",
    [UI_BINDS_PRESS_KEY] = "Press a key or mouse button for %s",
    [UI_BINDS_PRESS_PAD] = "Press a controller button for %s",
    [UI_BINDS_CANCEL_HINT] = "Esc cancels, or wait %d s",
    [UI_BINDS_TAKEN] = "%s was taken from %s", [UI_BINDS_SWAPPED] = "%s swapped with %s",
    [UI_BINDS_RESET] = "Reset all buttons",
    [UI_BINDS_RESET_DESC] = "Restores the default keys and buttons.",
    [UI_BIND_A] = "A", [UI_BIND_B] = "B", [UI_BIND_X] = "X", [UI_BIND_Y] = "Y",
    [UI_BIND_LCLICK] = "Left stick click", [UI_BIND_RCLICK] = "Right stick click", [UI_BIND_L] = "L",
    [UI_BIND_R] = "R", [UI_BIND_ZL] = "ZL", [UI_BIND_ZR] = "ZR", [UI_BIND_PLUS] = "Plus", [UI_BIND_MINUS] = "Minus",
    [UI_BIND_LEFT] = "D-pad left", [UI_BIND_UP] = "D-pad up", [UI_BIND_RIGHT] = "D-pad right",
    [UI_BIND_DOWN] = "D-pad down",
    [UI_BIND_LSTICK_LEFT] = "Move left", [UI_BIND_LSTICK_UP] = "Move forward", [UI_BIND_LSTICK_RIGHT] = "Move right",
    [UI_BIND_LSTICK_DOWN] = "Move back",
    [UI_BIND_RSTICK_LEFT] = "Camera left", [UI_BIND_RSTICK_UP] = "Camera up", [UI_BIND_RSTICK_RIGHT] = "Camera right",
    [UI_BIND_RSTICK_DOWN] = "Camera down",
    [UI_BIND_DESC_A] = "Actions: talk, open, pick up, climb, grab. Hold to dash.",
    [UI_BIND_DESC_B] = "Put away, cancel, back. Jump when Swap Jump Controls is on in the game's Options.",
    [UI_BIND_DESC_X] = "Jump. Put away when Swap Jump Controls is on in the game's Options.",
    [UI_BIND_DESC_Y] = "Attack with the sword.",
    [UI_BIND_DESC_LCLICK] = "Left stick press (not on the game's controls screen).",
    [UI_BIND_DESC_RCLICK] = "Reset the camera behind Link.",
    [UI_BIND_DESC_L] = "Quick-select items; the previous page in the game's menus.",
    [UI_BIND_DESC_R] = "Quick-select items; the next page in the game's menus.",
    [UI_BIND_DESC_ZL] = "Target (hold or toggle: Targeting Type in the game's Options).",
    [UI_BIND_DESC_ZR] = "Use the selected item: slingshot, bow, boomerang, hookshot, bombs, bottles.",
    [UI_BIND_DESC_PLUS] = "Menu: items, equipment, collection, system.", [UI_BIND_DESC_MINUS] = "Map.",
    [UI_BIND_DESC_LEFT] = "Play the ocarina.", [UI_BIND_DESC_UP] = "Put the accessory on or take it off.",
    [UI_BIND_DESC_RIGHT] = "Talk to Navi.", [UI_BIND_DESC_DOWN] = "Change boots.",
    [UI_BIND_DESC_MOVE] = "Moves Link.",
    [UI_BIND_DESC_CAMERA] = "Turns the camera.",
    [UI_BIND_SHAKE] = "Shake the camera stick",
    [UI_BIND_DESC_SHAKE] = "Shakes the right stick. Hold to keep shaking.",
    [UI_BIND_ACTION_A] = "Actions / dash", [UI_BIND_ACTION_B] = "Put away / back", [UI_BIND_ACTION_X] = "Jump",
    [UI_BIND_ACTION_Y] = "Sword attack", [UI_BIND_ACTION_LCLICK] = "—", [UI_BIND_ACTION_RCLICK] = "Reset camera",
    [UI_BIND_ACTION_L] = "Items / prev. pg", [UI_BIND_ACTION_R] = "Items / next pg",
    [UI_BIND_ACTION_ZL] = "Target", [UI_BIND_ACTION_ZR] = "Use item", [UI_BIND_ACTION_PLUS] = "Pause menu",
    [UI_BIND_ACTION_MINUS] = "Map", [UI_BIND_ACTION_LEFT] = "Ocarina", [UI_BIND_ACTION_UP] = "Accessory",
    [UI_BIND_ACTION_RIGHT] = "Talk to Navi", [UI_BIND_ACTION_DOWN] = "Change boots", [UI_BIND_ACTION_MOVE] = "Move",
    [UI_BIND_ACTION_CAMERA] = "Camera",
    [UI_KEY_SPACE] = "Space", [UI_KEY_TAB] = "Tab", [UI_KEY_LSHIFT] = "Left Shift", [UI_KEY_LCTRL] = "Left Ctrl",
    [UI_KEY_UP] = "Up arrow", [UI_KEY_DOWN] = "Down arrow", [UI_KEY_LEFT] = "Left arrow",
    [UI_KEY_RIGHT] = "Right arrow",
    [UI_KEY_MOUSE_L] = "Left mouse", [UI_KEY_MOUSE_R] = "Right mouse", [UI_KEY_MOUSE_M] = "Middle mouse",
    [UI_KEY_MOUSE_X1] = "Mouse 4", [UI_KEY_MOUSE_X2] = "Mouse 5",
    [UI_KEY_ENTER] = "Enter", [UI_KEY_BACKSPACE] = "Backspace", [UI_KEY_RSHIFT] = "Right Shift",
    [UI_KEY_RCTRL] = "Right Ctrl", [UI_KEY_LALT] = "Alt", [UI_KEY_CAPSLOCK] = "Caps Lock", [UI_KEY_INSERT] = "Insert",
    [UI_KEY_DELETE] = "Delete", [UI_KEY_HOME] = "Home", [UI_KEY_END] = "End", [UI_KEY_PAGEUP] = "Page Up",
    [UI_KEY_PAGEDOWN] = "Page Down", [UI_KEY_NUM_FMT] = "Num %s",
    [UI_PAD_SOUTH] = "Bottom button", [UI_PAD_EAST] = "Right button", [UI_PAD_WEST] = "Left button",
    [UI_PAD_NORTH] = "Top button", [UI_PAD_LSTICK] = "Left stick", [UI_PAD_RSTICK] = "Right stick",
    [UI_PAD_L] = "Left bumper", [UI_PAD_R] = "Right bumper", [UI_PAD_START] = "Start", [UI_PAD_BACK] = "Back",
    [UI_PAD_DPAD_LEFT] = "D-pad left", [UI_PAD_DPAD_UP] = "D-pad up", [UI_PAD_DPAD_RIGHT] = "D-pad right",
    [UI_PAD_DPAD_DOWN] = "D-pad down", [UI_PAD_ZL] = "Left trigger", [UI_PAD_ZR] = "Right trigger",
    [UI_PAD_LPADDLE1] = "Left back button", [UI_PAD_RPADDLE1] = "Right back button",
    [UI_PAD_LPADDLE2] = "Left back button 2", [UI_PAD_RPADDLE2] = "Right back button 2",
    [UI_PAD_CROSS] = "Cross", [UI_PAD_CIRCLE] = "Circle", [UI_PAD_SQUARE] = "Square", [UI_PAD_TRIANGLE] = "Triangle",

    [UI_MENU_AMIIBO_DIR] = "Amiibo folder",
    [UI_MENU_AMIIBO_OPEN] = "Open",
    [UI_MENU_AMIIBO_DEFAULT_DIR] = "Default folder",
    [UI_MENU_AMIIBO_REFRESH] = "Refresh",
    [UI_MENU_AMIIBO_NO_DIR] = "Cannot open the folder %s",
    [UI_MENU_AMIIBO_NONE_FOUND] = "No amiibo dumps (540-byte .bin or .nfc files) in %s",
    [UI_MENU_AMIIBO_FILTER] = "Search",
    [UI_MENU_AMIIBO_LIST_HINT] = "A or a click puts the amiibo on the reader.",
    [UI_MENU_AMIIBO_ADVANCED] = "Advanced",
    [UI_MENU_AMIIBO_READER] = "NFC reader",
    [UI_MENU_AMIIBO_STATE] = "Reader:",
    [UI_MENU_AMIIBO_STATE_IDLE] = "ready",
    [UI_MENU_AMIIBO_STATE_SEARCHING] = "the game is waiting for an amiibo",
    [UI_MENU_AMIIBO_STATE_FOUND] = "amiibo on the reader",
    [UI_MENU_AMIIBO_STATE_MOUNTED] = "the game is reading the amiibo",
    [UI_MENU_AMIIBO_STATE_OFF] = "off",
    [UI_MENU_AMIIBO_REMOVE] = "Lift the amiibo",
    [UI_MENU_AMIIBO_AUTO_REMOVE] = "Lift the amiibo by itself after a tap",
    [UI_MENU_AMIIBO_HOLD] = "Time on the reader",
    [UI_MENU_AMIIBO_SECONDS] = "%.1f s",
    [UI_MENU_AMIIBO_NO_TAG] = "No amiibo on the reader.",
    [UI_MENU_AMIIBO_TAG_FILE] = "File: %s",
    [UI_MENU_AMIIBO_TAG_ID] = "Character %s, series %02x, number %04x, type %02x",
    [UI_MENU_AMIIBO_TAG_UID] = "UID: %s",
    [UI_MENU_AMIIBO_READY] = "Ready",
    [UI_MENU_AMIIBO_TAPPED] = "Tapped: %s",
    [UI_MENU_AMIIBO_LOAD_FAILED] = "Not an amiibo dump (540-byte NTAG215): %s",
    [UI_MENU_AMIIBO_REMOVED] = "Amiibo lifted",
    [UI_MENU_AMIIBO_NICKNAME] = "Nickname: %s",
    [UI_MENU_AMIIBO_NOT_REGISTERED] = "No owner or nickname registered",
    [UI_MENU_AMIIBO_SOURCE] = "Figure data: %s",
    [UI_MENU_AMIIBO_SOURCE_NEW] = "new, nothing saved yet",
    [UI_MENU_AMIIBO_SOURCE_SAVED] = "saved by the port",
    [UI_MENU_AMIIBO_SOURCE_DUMP] = "read from the dump",
    [UI_MENU_AMIIBO_DECRYPTED] = "Dump decrypted with the keys",
    [UI_MENU_AMIIBO_NOT_DECRYPTED] = "Dump not decrypted",
    [UI_MENU_AMIIBO_WRITES] = "Writes: %u, last on %04u-%02u-%02u",
    [UI_MENU_AMIIBO_GAME_DATA] = "Game data: %u bytes for access ID %08X",
    [UI_MENU_AMIIBO_NO_GAME_DATA] = "Game data: none",
    [UI_MENU_AMIIBO_HAS_GAME_DATA] = "Game data: saved",
    [UI_MENU_AMIIBO_ERASE] = "Erase game data",
    [UI_MENU_AMIIBO_ERASE_CONFIRM] = "Press again to erase",
    [UI_MENU_AMIIBO_ERASED] = "Game data erased",
    [UI_MENU_AMIIBO_ERASE_FAILED] = "Could not erase the game data",
    [UI_MENU_AMIIBO_KEYS] = "Keys:",
    [UI_MENU_AMIIBO_KEYS_LOADED] = "loaded",
    [UI_MENU_AMIIBO_KEYS_MISSING] = "not loaded",
    [UI_MENU_AMIIBO_KEYS_FILE] = "Keys file: %s",
    [UI_MENU_AMIIBO_RELOAD_KEYS] = "Reload keys",
    [UI_MENU_AMIIBO_KEYS_RELOADED] = "Keys loaded: %s",
    [UI_MENU_AMIIBO_KEYS_FAILED] = "No keys loaded (missing, or not a 160-byte keys file): %s",
    [UI_MENU_AMIIBO_WRITEBACK] = "Write the game's changes into the dump file",
    [UI_MENU_AMIIBO_WRITEBACK_NEEDS_KEYS] = "needs the keys",
    [UI_MENU_AMIIBO_POPUP] = "Show the amiibo list when the game asks for an amiibo",
    [UI_MENU_AMIIBO_POPUP_TITLE] = "The game is waiting for an amiibo",
    [UI_MENU_AMIIBO_POPUP_HINT] = "A: tap   B: close",
    [UI_MENU_AMIIBO_DAILY_LIMIT] = "Amiibo daily limit",
    [UI_MENU_AMIIBO_DAILY_LIMIT_DESC] = "On, as in the game: a figure gives its items once a day, and after that the "
                                        "game says you cannot use it again today. Off: it gives items every time it is "
                                        "tapped. The two figures that give a mask have no limit either way.",
    [UI_MENU_REPLAY_WORKERS] = "CPU helper threads (experimental)",
    [UI_MENU_REPLAY_WORKERS_DESC] = "Uses more of your CPU's cores to prepare each frame for the graphics card. Automatic "
                                    "picks the number for your CPU: half its cores less one, none on four cores or fewer "
                                    "and none on RX 6000 and older Radeon cards. 0 turns it off. 3 gave about 20% more "
                                    "frames a second on an 8-core CPU. If your game crashes with this on, set it to 0.",
    [UI_MENU_REPLAY_PIPE] = "Helpers work ahead (experimental)",
    [UI_MENU_REPLAY_PIPE_DESC] = "The game carries on with the rest of the frame while the helper threads are still "
                                 "busy, instead of waiting for them. The number is how many batches of work the "
                                 "helpers may have at once; 0 turns it off. A small gain: 4 gave 1 to 2% more frames a "
                                 "second.",
    [UI_MENU_REPLAY_SMALL] = "Helpers take small jobs (experimental)",
    [UI_MENU_REPLAY_SMALL_DESC] = "Gives the helper threads the smaller batches of drawing work too, not only the large "
                                  "ones. The number is the smallest batch a helper takes, counted in draw calls; 0 "
                                  "turns it off. A small gain: 16 gave about 2% more frames a second.",
    [UI_MENU_TAIL_EARLY] = "Start next frame early (experimental)",
    [UI_MENU_TAIL_EARLY_DESC] = "The game starts on its next frame while the graphics card is still finishing the "
                                "current one. The biggest gain of these settings: 19 to 29% more frames a second with "
                                "FSR 4 on a Radeon RX 7900 XT, the only card it was measured on. It needs TAA, DLSS, "
                                "FSR 3.1 or FSR 4, and for now an AMD graphics card. Without them it changes nothing.",
    [UI_MENU_NEEDS_WORKERS] = "needs CPU helper threads", [UI_MENU_NEEDS_PIPE] = "needs Helpers work ahead",
    [UI_MENU_DISCORD_RPC] = "Discord Rich Presence",
    [UI_MENU_DISCORD_RPC_DESC] = "Sends an activity with play time to your Discord client. Set your own "
                                 "discord_app_id in nx2saria.ini or NX2SARIA_DISCORD_APP_ID; Discord shows the name "
                                 "registered to that ID.",

    [UI_MENU_RESET_NO] = "Cancel",

    [UI_MENU_OV_FPS] = "%.0f FPS", [UI_MENU_OV_FRAME_MS] = "%.1f ms", [UI_MENU_OV_GAME_PCT] = "game %.0f%%",
    [UI_MENU_OV_MAX_MS] = "max %.1f ms",
    [UI_MENU_OV_FPS_LABEL] = "FPS", [UI_MENU_OV_FG_LABEL] = "FG", [UI_PERF_LINE_FPS_FG] = "%.0f fps (%.0f frame gen)",
    [UI_MENU_OV_FRAME_LABEL] = "Frame", [UI_MENU_OV_FMT_FPS] = "%.0f",
    [UI_MENU_OV_LOW1] = "1%% low %.0f", [UI_MENU_OV_LOW01] = "0.1%% low %.0f",
    [UI_MENU_OV_GPU] = "GPU", [UI_MENU_OV_CPU] = "CPU", [UI_MENU_OV_RAM] = "RAM", [UI_MENU_OV_VRAM] = "VRAM",
    [UI_MENU_OV_NONE] = "-",
    [UI_MENU_OV_REPLAY] = "GPU frame prep", [UI_MENU_OV_GPU_TIME] = "GPU busy per frame",
    [UI_MENU_OV_PIPE_WAIT] = "Pipeline waits",
    [UI_MENU_OV_SHADER_WAIT] = "Shader waits", [UI_MENU_OV_TEX_UPLOADS] = "Texture uploads",
    [UI_MENU_OV_FMT_PCT] = "%.0f%%", [UI_MENU_OV_FMT_TEMP] = "%.0f °C", [UI_MENU_OV_FMT_POWER] = "%.0f W",
    [UI_MENU_OV_FMT_MHZ] = "%.0f MHz", [UI_MENU_OV_FMT_GB] = "%.1f GB", [UI_MENU_OV_FMT_GB_OF] = "/ %.1f GB",
    [UI_MENU_OV_GAME_GB] = "game %.1f GB", [UI_MENU_OV_FMT_MS] = "%.2f ms", [UI_MENU_OV_PER_FRAME] = "%.1f per frame",
    [UI_MENU_OV_WINDOW] = "Window", [UI_MENU_OV_OUTPUT] = "Game image", [UI_MENU_OV_SCENE] = "3D scene",
    [UI_MENU_OV_LIMIT] = "Frame limit",
    [UI_MENU_OV_FMT_SIZE] = "%ux%u", [UI_MENU_OV_FMT_SCENE] = "%ux%u, %.0f%%",
    [UI_MENU_OV_FMT_FSR] = "AMD FSR 1, sharpness %d%%",
    [UI_MENU_TAB_CHEATS] = "Cheats",
    [UI_MENU_CHEATS_NONE] = "No cheats for this game version. A mod's cheats are an Atmosphere cheat file in its "
                            "cheats folder, named after the game's build: mods\\<mod>\\cheats\\%s. Restart the game to "
                            "read them. For cheats that change the game's code, the game rebuilds itself at start (a "
                            "few minutes).",
    [UI_MENU_CHEATS_MOD] = "%s: %s",
    [UI_MENU_CHEAT_CODE_DESC] = "Changes the game's code. Switches at once.",
    [UI_MENU_CHEAT_KEY_DESC] = "Changes the game's code while you hold %s. Switches at once.",
    [UI_MENU_CHEAT_DATA_DESC] = "Writes the game's memory every frame while it is on.",
    [UI_MENU_CHEAT_MASTER] = "always on",
    [UI_MENU_CHEAT_MASTER_DESC] = "The file's master code: it is always on.",
    [UI_MENU_CHEAT_RESTORE] = "not needed",
    [UI_MENU_CHEAT_RESTORE_DESC] = "It only writes the game's own code back. Switching a cheat off already does that.",
    [UI_MENU_CHEAT_REBUILD] = "after the rebuild",
    [UI_MENU_CHEAT_REBUILD_DESC] = "This cheat changes the game's code and is new or edited since the game was built. "
                                   "Play from the launcher builds it in.",
    [UI_MENU_CHEAT_UNSUPPORTED] = "not supported",
    [UI_MENU_CHEAT_UNSUPPORTED_DESC] = "This cheat cannot be used with this version of the game:",
    [UI_MENU_SAVES] = "Saves",
    [UI_MENU_SAVE_EXPORT] = "Export save",
    [UI_MENU_SAVE_EXPORT_DESC] = "Writes your save (every slot) into one zip file in your Downloads folder, to give to "
                                 "another player or attach to a bug report. It never holds your keys or game files.",
    [UI_MENU_SAVE_EXPORT_BUG] = "Export save for a bug report",
    [UI_MENU_SAVE_EXPORT_BUG_DESC] = "The same zip with this run's log and your settings (nx2saria.ini) added, which "
                                     "show what happened and with which settings. Attach it to your bug report.",
    [UI_MENU_SAVE_IMPORT] = "Import a save",
    [UI_MENU_SAVE_IMPORT_DESC] = "Choose a save someone shared (an NX2Saria-save zip). It replaces your save, every slot, "
                                 "at the next start: the game keeps your progress in memory and would write it back "
                                 "over a save replaced now. Your current save is copied to the saves folder first.",
    [UI_MENU_SAVE_IMPORT_WAITING] = "at next start",
    [UI_MENU_SAVE_IMPORT_CANCEL] = "Cancel the import",
    [UI_MENU_SAVE_IMPORT_CANCEL_DESC] = "Keeps your save as it is: the save waiting for the next start is dropped.",
    [UI_MENU_SAVE_BUSY] = "working…",
    [UI_MENU_SAVE_EXPORTED] = "Saved as %s",
    [UI_MENU_SAVE_IMPORT_READY] = "The save (NX2Saria %s) loads at the next start: select Restart below. Your current "
                                  "save is copied to the saves folder first.",
    [UI_MENU_SAVE_IMPORT_CANCELLED] = "Import cancelled; your save stays as it is.",
    [UI_MENU_SAVE_IMPORTED] = "A shared save was imported at this start. Your old save is in %s",
    [UI_MENU_SAVE_IMPORTED_NEW] = "A shared save was imported at this start.",
    [UI_MENU_SAVE_E_NO_SAVE] = "There is no save to export yet: play until the game saves.",
    [UI_MENU_SAVE_E_READ_SAVE] = "A file of your save could not be read; try again in a moment.",
    [UI_MENU_SAVE_E_WRITE] = "The zip could not be written into %s.",
    [UI_MENU_SAVE_E_READ] = "%s could not be read.",
    [UI_MENU_SAVE_E_STAGE] = "The save could not be prepared for the next start; the log says why.",
    [UI_MENU_SAVE_E_NOT_SAVE] = "%s is not a save NX2Saria exported.",
    [UI_MENU_SAVE_E_OTHER_GAME] = "%s is a save of another game.",
    [UI_MENU_SAVE_E_NEWER] = "%s was made by a newer NX2Saria: update NX2Saria first.",
    [UI_MENU_SAVE_E_DAMAGED] = "%s is damaged: download it again.",
    [UI_MENU_SAVE_E_UNSAFE] = "%s was refused: it holds files outside a save, links, or far more than a save.",
    [UI_MENU_CAPTURE_SAVED] = "Bug capture %d saved",
    [UI_MENU_CAPTURE_FOLDER] = "Folder: %s",
    [UI_MENU_CAPTURE_FAILED] = "Bug capture %d could not be saved",
    [UI_BOOT_TITLE] = "Preparing shaders",
    [UI_BOOT_STARTING] = "Starting", [UI_BOOT_SHADERS] = "Shaders: %lld of %lld", [UI_BOOT_PIPELINES] = "Pipelines: %lld of %lld",
    [UI_BOOT_COMPUTE] = "Compute pipelines: %lld of %lld", [UI_BOOT_DRIVER_CACHE] = "Driver cache: %lld of %lld",
    [UI_MENU_UPSCALING] = "Upscaling",
    [UI_MENU_UPSCALING_DESC] = "How the picture is smoothed and scaled to your window. Native draws the 3D scene at "
                               "the picture's full size with no upscaler: the sharpest, and the most GPU work.",
    [UI_MENU_UPSCALING_DESC_FSR] = "How the picture is smoothed and scaled to your window. Native draws the 3D scene "
                                   "at the picture's full size with no upscaler: the sharpest, and the most GPU work.",
    [UI_MENU_FRAMEGEN] = "Frame generation",
    [UI_MENU_FRAMEGEN_OFF] = "Off",
    [UI_MENU_HDR_TONEMAPPED] = "RenoDX", [UI_MENU_HDR_NATIVE] = "Native",
    [UI_MENU_FRAMEGEN_NOTE] = "needs DLSS or FSR 3.1",
    [UI_MENU_UPSCALING_DESC_NO_FSR] = "How the picture is smoothed and scaled to your window. Native draws the 3D scene "
                                      "at the picture's full size with no upscaler: the sharpest, and the most GPU "
#ifdef _WIN32
                                      "work. AMD FSR 3.1 is not offered here: it needs amd_fidelityfx_vk.dll, which "
                                      "cannot be loaded.",
#else
                                      "work. AMD FSR 3.1 is not offered here: the game's own FSR 3.1 cannot run on "
                                      "this GPU or driver (see the fsr3 line in the log for why).",
#endif
    [UI_MENU_FSR3_OFF] = "in use: off, FSR 3.1 is not available",
    [UI_MENU_FRAMEGEN_NEEDS_DLSS] = "choose a DLSS mode under Upscaling first",
    [UI_MENU_FRAMEGEN_DESC_NONE] = "Adds generated frames for smoother motion. Needs DLSS or FSR 3.1, and neither runs "
                                   "here.",
    [UI_MENU_FRAMEGEN_NEEDS_NONE] = "needs DLSS or FSR 3.1, which do not run here",
    [UI_MENU_FRAMEGEN_DESC_FSR_WIN] = "Adds generated frames for smoother motion. FSR 3.1 upscaling runs here through "
                                      "the game's own build of its passes, but FSR 3.1's frame generation needs AMD's "
                                      "library, which only the Windows build can load. Here frame generation needs NVIDIA "
                                      "DLSS on a GeForce RTX 40 or 50 card.",
    [UI_MENU_FRAMEGEN_NEEDS_FSR_WIN] = "FSR 3.1's frame generation runs on Windows only",
    [UI_MENU_FRAMEGEN_OFF_FSR_WIN] = "in use: off, FSR 3.1's frame generation runs on Windows only",
    [UI_MENU_FRAMEGEN_FSR3_NATIVE] = "2x, the game's own FSR 3.1 passes",
    [UI_MENU_FRAMEGEN_OFF_HDR] = "in use: off while HDR is on (SDR only)",
    [UI_MENU_FRAMEGEN_MULT] = "DLSS multiplier",
    [UI_MENU_FG_MULT_DESC] = "Frames on screen for each frame the game draws with DLSS Frame Generation: 2x adds one "
                             "generated frame, up to 6x on an RTX 50 card. Dynamic adds only the frames needed to "
                             "reach your display's refresh rate. Changes take effect at once.",
    [UI_MENU_FG_MULT_DYNAMIC] = "Dynamic",
    [UI_MENU_FRAMEGEN_MULT_DESC] = "Frames on screen for each frame the game draws: 2x adds one generated frame, 3x "
                                   "two, 4x three. Higher is smoother but adds a little delay. The frame limit counts "
                                   "the frames the game draws, so the screen shows the limit times this. 2x needs a "
                                   "GeForce RTX 40 GPU or newer, 3x and 4x an RTX 50.",
    [UI_MENU_FRAMEGEN_MULT_RTX50] = "3x and 4x need an RTX 50 GPU",
    [UI_MENU_FRAMEGEN_MULT_UPTO] = "up to %dx on this GPU and driver",
    [UI_MENU_FRAMEGEN_MULT_OLD_DLL] = "2x only: update the DLSS frame generation files",
    [UI_MENU_FRAMEGEN_UNAVAILABLE] = "in use: off: it needs an RTX 40 or newer; if you have one, see dlssg in the log",
    [UI_MENU_FRAMEGEN_OFF_FILES] = "in use: off, its files are missing",
    [UI_MENU_FRAMEGEN_DESC_ANY] = "Adds generated frames for smoother motion, with any Upscaling choice.",
    [UI_MENU_FRAMEGEN_DESC_NGX] = "Adds generated frames for smoother motion. NVIDIA DLSS needs a DLSS mode under "
                                  "Upscaling when the game starts; it then runs with any Upscaling choice.",
    [UI_MENU_FRAMEGEN_NOTE_NGX] = "needs DLSS when the game starts",
    [UI_MENU_GAME_RES] = "Resolution",
    [UI_MENU_GAME_RES_DESC] = "Size of the game's picture; outside fullscreen the window takes it too. Higher is sharper "
                              "but costs more GPU power.",
    [UI_MENU_GAME_RES_NATIVE] = "Native",
    [UI_MENU_CHEATS_UNBUILT] = "%s and %s are not built into the game: Play from the launcher builds them in.",
    [UI_MENU_COUNT_CODE_CHEATS] = "%d code cheat|%d code cheats",
    [UI_MENU_COUNT_EXEFS_FILES] = "%d ExeFS patch file|%d ExeFS patch files",
    [UI_MENU_COUNT_CHEATS] = "%d cheat|%d cheats",
    [UI_MENU_PRESET] = "Quality preset",
    [UI_MENU_PRESET_DESC] = "Sets the graphics options at once. Recommended is the preset this PC gets from its graphics card, "
        "memory and processor. High is the game as shipped; Low and Medium draw less, for slower PCs and handhelds; "
        "Ultra draws more, and more on a larger monitor; it is never recommended, only chosen. Options you change by hand afterwards "
        "stay as you set them, and the preset then shows as custom.",
    [UI_MENU_PRESET_RECOMMENDED] = "Recommended (%s)",
    [UI_MENU_PRESET_LOW] = "Low",
    [UI_MENU_PRESET_MEDIUM] = "Medium",
    [UI_MENU_PRESET_HIGH] = "High",
    [UI_MENU_PRESET_ULTRA] = "Ultra",
    [UI_MENU_PRESET_CUSTOM] = "%s (custom)",
    [UI_MENU_PRESET_CUSTOM_NOTE] = "set by hand: %d",
    [UI_MENU_PRESET_REC_NOTE] = "Recommended for this PC: %s",
    [UI_MENU_PRESET_HW_NOTE] = "New hardware: the recommended preset is in use",
    [UI_MENU_PRESET_NOTICE] = "Quality preset Recommended (%s) is in use for this PC (Settings > Screen). For the game as before, choose High there.",
    [UI_MENU_PRESET_CONFIRM] = "These settings were changed by hand. The preset sets them to its own values:",
    [UI_MENU_PRESET_CONFIRM_YES] = "Use the preset",
    [UI_MENU_SCENE_RES] = "Scene resolution",
    [UI_MENU_SCENE_RES_DESC] = "Size the 3D scene is drawn at, as a share of the picture. Lower draws the scene smaller and enlarges "
        "it to the picture: faster, for slow PCs and handhelds, but softer. The HUD and menus stay sharp, at the "
        "full resolution. 100 % is Native. Changes take effect at once.",
    [UI_MENU_SCENE_RES_GAME] = "%s (game)",
    [UI_MENU_SCENE_RES_NATIVE] = "%s (native)",
    [UI_MENU_SCENE_RES_PRESET] = "%s (preset)",
    [UI_MENU_DYN_RES] = "Dynamic resolution",
    [UI_MENU_DYN_RES_DESC] = "Lowers the 3D scene's resolution while the graphics card cannot keep up with the Frame "
        "rate limit, and raises it again when it can: a little sharpness in heavy scenes for a steadier frame rate. "
        "The HUD and menus stay sharp. With DLSS or FSR it steps the upscaling mode down, as far as Performance; "
        "without them it lowers the scene resolution, as far as the minimum below. Changes take effect at once.",
    [UI_MENU_DYN_RES_MIN] = "Minimum scene resolution",
    [UI_MENU_DYN_RES_MIN_DESC] = "The lowest Dynamic resolution takes the 3D scene to, as a share of the picture. "
        "Lower keeps the frame rate steadier on a slow PC, but heavy scenes look softer. It never goes above the Scene "
        "resolution you chose. With DLSS or FSR this does not apply: the upscaling mode steps down to Performance "
        "instead. Changes take effect at once.",
    [UI_MENU_DYN_RES_MIN_OFF] = "only with Dynamic resolution on",
    [UI_MENU_DYN_RES_NO_ROOM] = "nothing to lower: Scene resolution is at or under the minimum",
    [UI_MENU_DYN_RES_NATIVE] = "nothing to lower at DLAA or Native AA",
    [UI_MENU_VRAM_WARN] = "Video memory is full, so the game runs slowly. In Settings > Screen:",
    [UI_MENU_VRAM_WARN_RES] = "- %s: choose a lower one",
    [UI_MENU_VRAM_WARN_MODE] = "- %s: choose a lighter mode than %s",
    [UI_MENU_VRAM_WARN_FRAMEGEN] = "- %s: turn it off",
    [UI_MENU_VRAM_WARN_PRESET] = "- %s: choose a lower one",
    [UI_MENU_VRAM_FIT_MEASURED] = "Video memory is already %d%% in use (%llu of %llu MiB). These use more video memory: %s.",
    [UI_MENU_VRAM_FIT_NOTICED] = "Video memory ran out earlier in this session (now %d%% in use, %llu of %llu MiB). These use more video memory: %s.",
    [UI_MENU_VRAM_FIT_PRESET] = "This card has %u GiB of video memory. The recommended preset for it uses: %s.",
    [UI_MENU_VRAM_FIT_FULL] = "Video memory is already full (%llu of %llu MiB). These use more video memory: %s.",
};

static const char *const text_ru[UI_TEXT_COUNT] = {
    SWKBD("Ввод текста", "Введите имя", "Лошадь", "ОК", "Отмена", "Контроллер: A — ОК, B — отмена"),

    [UI_MENU_TITLE] = "Настройки",
    [UI_MENU_TAB_SCREEN] = "Экран", [UI_MENU_TAB_GAME] = "Игра", [UI_MENU_TAB_SOUND] = "Звук",
    [UI_MENU_TAB_CONTROLS] = "Управление", [UI_MENU_TAB_AMIIBO] = "Amiibo",
    [UI_MENU_CONTINUE] = "Продолжить", [UI_MENU_QUIT] = "Выйти из игры",
    [UI_MENU_ON] = "Вкл.", [UI_MENU_OFF] = "Выкл.", [UI_MENU_AUTO] = "Автоматически",
    [UI_MENU_DEFAULT_MARK] = "по умолчанию",
    [UI_MENU_AFTER_RESTART] = "после перезапуска", [UI_MENU_SET_BY_ENV] = "задано переменной окружения",
    [UI_MENU_SAVE_FAILED] = "не удалось сохранить настройки",
    [UI_MENU_INI_UNKNOWN] = "nx2saria.ini: не настройки этой версии, пропущены: %s",
    [UI_MENU_VERSION] = "NX2Saria %s, игра %s",

    [UI_MENU_FULLSCREEN] = "Полный экран", [UI_MENU_FULLSCREEN_DESC] = "Игра на весь экран. F11 переключает.",
    [UI_MENU_WINDOW_SIZE] = "Размер окна", [UI_MENU_WINDOW_SIZE_DESC] = "Размер окна вне полноэкранного режима.",
    [UI_MENU_PRESENT_DESC] = "Убирает разрывы изображения. Выкл. немного снижает задержку.",
    [UI_MENU_PRESENT_VSYNC] = "Вертикальная синхронизация",
    [UI_MENU_PRESENT_IMMEDIATE] = "Без ожидания",
    [UI_MENU_PRESENT_IN_USE] = "сейчас: %s",
    [UI_MENU_LOW_LATENCY] = "Низкая задержка",
    [UI_MENU_LOW_LATENCY_DESC] = "Драйвер видеокарты задерживает начало каждого кадра, чтобы ввод считывался как "
                                 "можно позже (NVIDIA Reflex, AMD Anti-Lag). Он же держит ограничение частоты кадров. "
                                 "Boost не даёт видеокарте снижать частоту.",
    [UI_MENU_LOW_LATENCY_BOOST] = "Вкл. + Boost",
    [UI_MENU_LOW_LATENCY_NO_DRIVER] = "у этого драйвера нет режима низкой задержки",
    [UI_MENU_LOW_LATENCY_DLSSG] = "генерация кадров DLSS использует свой режим",
    [UI_MENU_LOW_LATENCY_FG] = "выключено, пока на этой видеокарте работает генерация кадров",
    [UI_MENU_FRAMEGEN_DESC_ANY] = "Добавляет сгенерированные кадры для более плавного движения, с любым выбором в "
                                  "Upscaling.",
    [UI_MENU_FRAMEGEN_DESC_NGX] = "Добавляет сгенерированные кадры для более плавного движения. Для NVIDIA DLSS "
                                  "нужен режим DLSS в Upscaling при запуске игры; после этого генерация работает с "
                                  "любым "
                                  "выбором в Upscaling.",
    [UI_MENU_FRAMEGEN_NOTE_NGX] = "нужен DLSS при запуске игры",
    [UI_MENU_LOW_LATENCY_FAILED] = "сейчас: выкл., драйвер отказал (см. lowlat в журнале)",
    [UI_MENU_GPU] = "Видеокарта", [UI_MENU_GPU_DESC] = "Видеокарта, на которой работает игра.",
    [UI_MENU_GPU_AUTO] = "Автоматически",
    [UI_MENU_PERF_OVERLAY] = "Индикатор производительности",
    [UI_MENU_PERF_OVERLAY_DESC] = "Показывает частоту кадров и данные системы. F10 меняет режим.",
    [UI_MENU_PERF_OFF] = "Выкл.", [UI_MENU_PERF_FPS] = "FPS", [UI_MENU_PERF_FULL] = "Полный",
    [UI_PERF_LINE_FPS] = "%.0f к/с",
    [UI_MENU_GRAPHICS] = "Изображение",
    [UI_MENU_OUTPUT_RES] = "Разрешение игры",
    [UI_MENU_OUTPUT_RES_DESC] = "Разрешение, в котором рисуется игра. Чем выше, тем больше нагрузка на видеокарту.",
    [UI_MENU_UPSCALER_FSR] = "AMD FSR 1", [UI_MENU_UPSCALER_BILINEAR] = "Билинейное",
    [UI_MENU_UPSCALING_NATIVE] = "Родное (100%)",
    [UI_MENU_UPSCALING_DESC] = "Как картинка сглаживается и масштабируется под окно. «Родное» рисует 3D-сцену в "
                               "полном размере картинки без апскейлера: самая чёткая картинка и самая большая "
                               "нагрузка на видеокарту.",
    [UI_MENU_UPSCALING_DESC_FSR] = "Как картинка сглаживается и масштабируется под окно. «Родное» рисует 3D-сцену в "
                                   "полном размере картинки без апскейлера: самая чёткая картинка и самая большая "
                                   "нагрузка на видеокарту.",
    [UI_MENU_UPSCALING_DESC_NO_FSR] = "Как картинка сглаживается и масштабируется под окно. «Родное» рисует 3D-сцену "
                                      "в полном размере картинки без апскейлера: самая чёткая картинка и самая "
#ifdef _WIN32
                                      "большая нагрузка на видеокарту. AMD FSR 3.1 здесь не предлагается: ему нужен "
                                      "amd_fidelityfx_vk.dll, который не удаётся загрузить.",
#else
                                      "большая нагрузка на видеокарту. AMD FSR 3.1 здесь не предлагается: встроенный "
                                      "в игру FSR 3.1 не может работать на этой видеокарте или с этим драйвером "
                                      "(причина — в строке fsr3 в журнале).",
#endif
    [UI_MENU_GAME_ASPECT] = "Формат игры",
    [UI_MENU_GAME_ASPECT_DESC] = "Подгоняет изображение под более высокий или широкий формат. В высоком формате сохраняется горизонтальный обзор 16:9 и видно больше сверху и снизу; интерфейс остаётся по центру.",
    [UI_MENU_GAME_ASPECT_16_9] = "16:9 (как в игре)", [UI_MENU_GAME_ASPECT_WINDOW] = "По окну",
    [UI_MENU_GAME_ASPECT_16_10] = "16:10", [UI_MENU_GAME_ASPECT_3_2] = "3:2",
    [UI_MENU_GAME_ASPECT_21_9] = "21:9", [UI_MENU_GAME_ASPECT_32_9] = "32:9",
    [UI_MENU_WIDE_HUD] = "Положение интерфейса",
    [UI_MENU_WIDE_HUD_DESC] = "Где находится интерфейс на изображении шире 16:9. «Центр 16:9» сохраняет раскладку игры; «Края экрана» ставит сердца и магию в левый верхний угол, предмет — в правый верхний, а мини-карту, ключи и рупии — к правому краю.",
    [UI_MENU_WIDE_HUD_CENTER] = "Центр 16:9", [UI_MENU_WIDE_HUD_EDGES] = "Края экрана",
    [UI_MENU_WIDE_MENUS] = "Меню",
    [UI_MENU_WIDE_MENUS_DESC] = "Меню паузы, системное меню, карта и экраны загрузки на изображении шире 16:9. «Заполнить экран» располагает их на всём изображении; «Чёрные полосы» оставляет их в середине 16:9 с чёрными краями.",
    [UI_MENU_WIDE_MENUS_FULL] = "Заполнить экран", [UI_MENU_WIDE_MENUS_CENTER] = "Чёрные полосы",
    [UI_MENU_FOV] = "Поле зрения",
    [UI_MENU_FOV_DESC] = "Сколько мира показывает камера, в процентах от игрового.",
    [UI_MENU_ANISO] = "Фильтрация текстур",
    [UI_MENU_ANISO_DESC] = "Анизотропная фильтрация текстур поверхностей: земля, дороги и стены под острым углом остаются чёткими вдали, а не размываются. Игра: уровень, который запрашивает игра, как на Switch.",
    [UI_MENU_ANISO_GAME] = "Игра",
    [UI_MENU_TEX_DETAIL] = "Детализация текстур",
    [UI_MENU_TEX_DETAIL_DESC] = "Чёткость больших текстур поверхностей. «Сниженная» и «Низкая» не загружают их самые крупные версии, чтобы область помещалась в видеопамять карт на 6 и 8 ГБ; мягче выглядят только поверхности вблизи. «Авто» выбирает по объёму видеопамяти.",
    [UI_MENU_TEX_DETAIL_AUTO] = "Авто: %s",
    [UI_MENU_TEX_DETAIL_FULL] = "Полная", [UI_MENU_TEX_DETAIL_REDUCED] = "Сниженная", [UI_MENU_TEX_DETAIL_LOW] = "Низкая",
    [UI_MENU_SHARPNESS] = "Резкость FSR",
    [UI_MENU_SHARPNESS_DESC] = "Насколько FSR повышает резкость. 0 отключает.",
    [UI_MENU_SHARPNESS_NEEDS_FSR] = "только с AMD FSR 1",
    [UI_MENU_HDR] = "Вывод HDR",
    [UI_MENU_HDR_DESC] = "Более яркие блики на HDR-экране. Сначала включите HDR в Windows.",
    [UI_MENU_HDR_PEAK] = "Пиковая яркость",
    [UI_MENU_HDR_PEAK_DESC] = "Пиковая яркость вашего экрана в нитах.",
    [UI_MENU_HDR_GAME] = "Яркость игры",
    [UI_MENU_HDR_GAME_DESC] = "Яркость белого в игре, в нитах.",
    [UI_MENU_HDR_UI] = "Яркость интерфейса и меню",
    [UI_MENU_HDR_UI_DESC] = "Яркость интерфейса и этого меню, в нитах.",
    [UI_MENU_HDR_HUE] = "Коррекция оттенка",
    [UI_MENU_HDR_HUE_DESC] = "Насколько яркие цвета сохраняют оттенок, который был до тональной кривой; при 0 они "
                             "смещаются вместе с кривой.",
    [UI_MENU_HDR_GAMMA] = "Коррекция гаммы",
    [UI_MENU_HDR_GAMMA_DESC] = "Показывает картинку так, как её показал бы экран с гаммой 2.2 или BT.1886 (2.4), для "
                               "которого игра делалась; «Выкл.» оставляет кривую sRGB с более светлыми тенями.",
    [UI_MENU_HDR_TONEMAP] = "Тональная кривая",
    [UI_MENU_HDR_TONEMAP_DESC] = "Vanilla: собственная кривая игры с яркостью SDR. PsychoV35: сцена через модель отклика "
                                 "глаза, согласованная со средним серым и тоном игры, до пиковой яркости экрана. RenoDRT: "
                                 "SDR-вид игры, доведённый до пика по яркости.",
    [UI_MENU_HDR_EXPOSURE] = "Экспозиция",
    [UI_MENU_HDR_EXPOSURE_DESC] = "Яркость всей картинки до тональной кривой; 100 % оставляет её без изменений.",
    [UI_MENU_HDR_HIGHLIGHTS] = "Света",
    [UI_MENU_HDR_HIGHLIGHTS_DESC] = "Яркость светлых участков; 50 оставляет их без изменений.",
    [UI_MENU_HDR_SHADOWS] = "Тени",
    [UI_MENU_HDR_SHADOWS_DESC] = "Яркость тёмных участков; 50 оставляет их без изменений.",
    [UI_MENU_HDR_CONTRAST] = "Контраст",
    [UI_MENU_HDR_CONTRAST_DESC] = "Контраст вокруг среднего серого; 50 оставляет его без изменений.",
    [UI_MENU_HDR_SATURATION] = "Насыщенность",
    [UI_MENU_HDR_SATURATION_DESC] = "Сила всех цветов; 50 оставляет её без изменений.",
    [UI_MENU_HDR_HIGHLIGHT_SAT] = "Насыщенность светов",
    [UI_MENU_HDR_HIGHLIGHT_SAT_DESC] = "Сила цвета самых ярких участков; 50 оставляет её без изменений.",
    [UI_MENU_HDR_BLOWOUT] = "Выгорание",
    [UI_MENU_HDR_BLOWOUT_DESC] = "Уводит самые яркие участки к белому; 0 сохраняет их цвет.",
    [UI_MENU_HDR_FLARE] = "Засветка",
    [UI_MENU_HDR_FLARE_DESC] = "Приподнимает самые тёмные участки, как немного рассеянного света; 0 оставляет чёрное чёрным.",
    [UI_MENU_HDR_GRADE] = "Цветокоррекция игры",
    [UI_MENU_HDR_GRADE_DESC] = "Сколько собственной цветокоррекции игры сохраняет HDR-картинка; 0 — сцена без неё.",
    [UI_MENU_HDR_MIDGRAY] = "Средний серый",
    [UI_MENU_HDR_MIDGRAY_DESC] = "Куда попадает средний серый сцены. Vanilla: туда же, куда его ставит кривая игры. "
                                 "Render: на значение сцены, чуть темнее.",
    [UI_MENU_HDR_MATCH_SLOPE] = "Наклон тона как в игре",
    [UI_MENU_HDR_MATCH_SLOPE_DESC] = "Даёт средним тонам контраст собственной кривой игры.",
    [UI_MENU_HDR_TONE_SLOPE] = "Наклон тона",
    [UI_MENU_HDR_TONE_SLOPE_DESC] = "Крутизна тональной кривой у среднего серого; 50 оставляет её без изменений.",
    [UI_MENU_HDR_HUE_CLIP] = "Имитация сдвига оттенка",
    [UI_MENU_HDR_HUE_CLIP_DESC] = "Насколько яркие цвета сдвигаются так, как при обрезке в SDR-картинке игры: огонь к "
                                  "жёлтому и белому. 0 сохраняет их оттенок.",
    [UI_MENU_DLSS_DLAA] = "DLAA (родное)", [UI_MENU_DLSS_QUALITY] = "Качество", [UI_MENU_DLSS_BALANCED] = "Баланс",
    [UI_MENU_DLSS_PERFORMANCE] = "Производительность", [UI_MENU_DLSS_ULTRA] = "Макс. производительность",
    [UI_MENU_AA] = "Сглаживание",
    [UI_MENU_AA_DESC] = "Как сглаживаются края 3D-картинки. SMAA игры — собственное, как на консоли. «Выкл.» оставляет края "
                        "как есть. FXAA быстрое и слегка мягкое. SMAA находит больше краёв, чем игровое, и сохраняет "
                        "чёткость. TAA — это DLSS или FSR в полном размере: самые устойчивые края в движении; задаёт "
                        "масштабирование. Когда в Upscaling выбран DLSS или FSR, их TAA работает всегда, а "
                        "выбранное здесь — вместе с ним. Действует сразу.",
    [UI_MENU_AA_GAME] = "SMAA игры",
    [UI_MENU_AA_FXAA] = "FXAA",
    [UI_MENU_AA_SMAA] = "SMAA (Ultra)",
    [UI_MENU_AA_TAA_DLSS] = "TAA (NVIDIA DLAA)",
    [UI_MENU_AA_TAA_FSR] = "TAA (AMD FSR 3.1)",
    [UI_MENU_AA_TAA_FSR4] = "TAA (%s)",
    [UI_MENU_AA_WITH_TAA] = "плюс TAA из Upscaling",

    [UI_MENU_FPS_LIMIT] = "Ограничение частоты кадров",
    [UI_MENU_FPS_LIMIT_DESC] = "Максимальная частота кадров, которые рисует игра. Генерация кадров добавляет свои сверх неё.",
    [UI_MENU_FPS_DISPLAY] = "Как у монитора", [UI_MENU_FPS_DISPLAY_HZ] = "Как у монитора (%.0f Гц)",
    [UI_MENU_FPS_UNLIMITED] = "Без ограничения",
    [UI_MENU_FPS_RTSS] = "ограничивает RivaTuner Statistics Server",
    [UI_MENU_FPS_RTSS_READONLY] = "RivaTuner нужен запуск игры от администратора",
    [UI_MENU_FPS_RTSS_IGNORED] = "RivaTuner не ограничивает эту игру; ограничивает игра",
    [UI_MENU_FPS_RTSS_TWO] = "действует и ограничение самого RivaTuner: два ограничителя",
    [UI_MENU_GAME_LANG] = "Язык игры", [UI_MENU_GAME_LANG_DESC] = "Язык текста и озвучки игры.",
    [UI_MENU_GAME_LANG_AUTO] = "Автоматически",
    [UI_MENU_OPERATION_MODE] = "Режим консоли",
    [UI_MENU_OPERATION_MODE_DESC] = "Запускает игру как в док-станции или в портативном режиме.",
    [UI_MENU_MODE_DOCKED] = "В док-станции", [UI_MENU_MODE_HANDHELD] = "Портативный",
    [UI_MENU_ASSUME_FOCUS] = "Работать без фокуса",
    [UI_MENU_ASSUME_FOCUS_DESC] = "Игра продолжает работать, когда вы переключаетесь на другое окно.",
    [UI_MENU_TEXT_SPEED] = "Скорость текста",
    [UI_MENU_TEXT_SPEED_DESC] = "Как быстро окна сообщений игры показывают текст. Быстро: в три раза быстрее; "
                                "мгновенно: каждая страница сразу целиком. Текст, который игра не даёт ускорить, "
                                "например реплики в такт сценке или голосу, идёт с обычной скоростью.",
    [UI_MENU_TEXT_SPEED_NORMAL] = "Обычная", [UI_MENU_TEXT_SPEED_FAST] = "Быстро",
    [UI_MENU_TEXT_SPEED_INSTANT] = "Мгновенно",

    [UI_MENU_VOLUME] = "Громкость", [UI_MENU_VOLUME_DESC] = "Общая громкость игры.", [UI_MENU_MENU_SOUNDS] = "Звуки меню", [UI_MENU_MENU_SOUNDS_DESC] = "Громкость звуков этого меню.",
    [UI_MENU_AUDIO_CHANNELS] = "Динамики",
    [UI_MENU_AUDIO_CHANNELS_DESC] = "Стерео или объёмный звук 5.1.",
    [UI_MENU_CHANNELS_AUTO] = "Автоматически", [UI_MENU_CHANNELS_STEREO] = "Стерео", [UI_MENU_CHANNELS_51] = "Объёмный 5.1",

    [UI_MENU_MOTION] = "Управление движением", [UI_MENU_MOTION_DESC] = "Прицеливание гироскопом контроллера.",
    [UI_MENU_RUMBLE_STRENGTH] = "Сила вибрации",
    [UI_MENU_RUMBLE_STRENGTH_DESC] = "Сила вибрации.",
    [UI_MENU_RUMBLE_CURVE] = "Кривая вибрации",
    [UI_MENU_RUMBLE_CURVE_DESC] = "Чем ниже, тем сильнее слабая вибрация.",
    [UI_MENU_RUMBLE_TRIGGERS] = "Вибрация триггеров (Xbox)",
    [UI_MENU_RUMBLE_TRIGGERS_DESC] = "Вибрация также на курках контроллера Xbox.",
    [UI_MENU_RUMBLE_HD] = "HD-вибрация (Switch)",
    [UI_MENU_RUMBLE_HD_DESC] = "Передаёт детальную вибрацию игры на Switch Pro Controller или Joy-Con без изменений. "
                               "Сила вибрации по-прежнему действует: значение по умолчанию — как в игре.",
    [UI_MENU_RUMBLE_HD_NEEDS] = "Нужен Switch Pro Controller или Joy-Con",
    [UI_MENU_GROUP_GYRO] = "Гироскоп",
    [UI_MENU_GYRO_CALIBRATION] = "Калибровка гироскопа",
    [UI_MENU_GYRO_CALIBRATION_DESC] = "Запоминает показания гироскопа в покое, когда контроллер ненадолго замирает, и "
                                      "вычитает их, чтобы прицел не дрейфовал. После подключения положите контроллер и "
                                      "не трогайте его секунду.",
    [UI_MENU_GYRO_DEADZONE] = "Мёртвая зона гироскопа",
    [UI_MENU_GYRO_DEADZONE_DESC] = "Повороты медленнее порога не учитываются; уровни — как на консоли. Низкая "
                                   "успокаивает неподвижный контроллер и сохраняет медленное прицеливание; "
                                   "Стандартная и Высокая усмиряют дрожащий контроллер, но глушат самые медленные "
                                   "повороты. Выкл.: проходят все показания.",
    [UI_MENU_GYRO_DEADZONE_OFF] = "Выкл.", [UI_MENU_GYRO_DEADZONE_LOW] = "Низкая",
    [UI_MENU_GYRO_DEADZONE_STANDARD] = "Стандартная", [UI_MENU_GYRO_DEADZONE_HIGH] = "Высокая",
    [UI_MENU_GYRO_TURN_SENS] = "Чувствительность поворота",
    [UI_MENU_GYRO_TURN_SENS_DESC] = "Насколько прицел смещается влево и вправо при повороте контроллера. 100 % — поворот "
                                    "без изменений.",
    [UI_MENU_GYRO_TURN_INVERT] = "Инверсия поворота",
    [UI_MENU_GYRO_TURN_INVERT_DESC] = "Инвертирует горизонтальное движение гироскопа.",
    [UI_MENU_GYRO_TILT_SENS] = "Чувствительность наклона",
    [UI_MENU_GYRO_TILT_SENS_DESC] = "Насколько прицел смещается вверх и вниз при наклоне контроллера. 100 % — наклон без "
                                    "изменений.",
    [UI_MENU_GYRO_TILT_INVERT] = "Инверсия наклона",
    [UI_MENU_GYRO_TILT_INVERT_DESC] = "Инвертирует вертикальное движение гироскопа.",
    [UI_MENU_GYRO_NEEDS_MOTION] = "Нужно включить управление движением",
    [UI_MENU_PAD_LAYOUT] = "Раскладка кнопок",
    [UI_MENU_PAD_LAYOUT_DESC] = "Какая кнопка работает как A. Nintendo: по положению, A справа, B снизу, X сверху, "
                                "Y слева. Xbox и PlayStation: по подписи, A снизу, B справа, X слева, Y сверху (на "
                                "контроллере PlayStation крест, круг, квадрат и треугольник). Автоматически: "
                                "Nintendo для контроллера с подписями Nintendo, по подписи для остальных.",
    [UI_MENU_PAD_LAYOUT_AUTO] = "Автоматически", [UI_MENU_PAD_LAYOUT_NINTENDO] = "Nintendo (A справа)",
    [UI_MENU_PAD_LAYOUT_XBOX] = "Xbox (A снизу)", [UI_MENU_PAD_LAYOUT_PLAYSTATION] = "PlayStation (A снизу)",
    [UI_MENU_PAD_LAYOUT_IN_USE] = "сейчас: %s",
    [UI_MENU_PROMPTS] = "Подсказки кнопок",
    [UI_MENU_PROMPTS_DESC] = "Какие кнопки показывают подсказки игры: в тексте, в подсказке действия и на нотах "
                             "окарины. Nintendo: как в самой игре. Xbox и PlayStation: кнопки этого контроллера (LB, "
                             "RB, LT, RT, Menu, View; L1, R1, L2, R2, Options, Create), а A, B, X и Y показаны той "
                             "кнопкой, которую нажимает раскладка выше (в Nintendo (A справа) A делает правая кнопка, "
                             "и подсказки A показывают B или круг). Клавиатура и мышь: клавиша каждой кнопки (E, Ctrl, "
                             "пробел, левый клик...), WASD для движения и мышь для камеры. Файлы подсказок создаются "
                             "лаунчером при сборке игры, клавиши берутся из nx2saria.ini на этот момент; если файлов "
                             "нет, обновите игру или пересоберите её (Rebuild the Game в лаунчере Windows, Update в "
                             "AppImage для Linux). Игра читает подсказки при запуске, так что они меняются после "
                             "перезапуска. Подсказки записаны в файлах игры, поэтому у кнопки, переназначенной на "
                             "вкладке управления, на экране остаётся прежняя подсказка, пока игра не пересобрана.",
    [UI_MENU_PROMPTS_NINTENDO] = "Nintendo", [UI_MENU_PROMPTS_XBOX] = "Xbox",
    [UI_MENU_PROMPTS_PLAYSTATION] = "PlayStation", [UI_MENU_PROMPTS_STEAMDECK] = "Steam Deck",
    [UI_MENU_PROMPTS_PC] = "Клавиатура и мышь",
    [UI_MENU_PROMPTS_MISSING] = "нет файлов подсказок: обновите игру или пересоберите её в лаунчере",
    [UI_MENU_PROMPTS_FAILED] = "файлы подсказок повреждены: пересоберите игру в лаунчере",
    [UI_MENU_PROMPTS_RESTART] = "подсказки сменятся после перезапуска",
    [UI_MENU_PROMPTS_OFF_SHAPE] = "выкл. при разрешении игры не 16:9",
    [UI_MENU_PROMPTS_OFF_MODS] = "выкл.: мод RomFS заменяет их файлы",
    [UI_MENU_PROMPTS_OFF_PATCHES] = "выкл., пока задан NX2SARIA_ROMFS_PATCHES",
    [UI_MENU_RESTART_CONFIRM] = "Перезапустить игру сейчас? Прогресс после последнего сохранения будет потерян.",
    [UI_MENU_QUIT_CONFIRM] = "Выйти из игры? Прогресс после последнего сохранения будет потерян.",
    [UI_MENU_RESTART_YES] = "Перезапустить",
    [UI_MENU_RESTART_COUNT] = "Перезапустить (%d)",
    [UI_MENU_RESTART_SAVING] = "Игра сохраняется: перезапуск будет доступен через мгновение.",
    [UI_MENU_QUIT_SAVING] = "Игра сохраняется: выход будет доступен через мгновение.",
    [UI_MENU_GROUP_STICKS] = "Стики",
    [UI_MENU_LSTICK_DEADZONE] = "Мёртвая зона левого стика", [UI_MENU_RSTICK_DEADZONE] = "Мёртвая зона правого стика",
    [UI_MENU_STICK_DEADZONE_DESC] = "Насколько нужно сдвинуть стик, чтобы он сработал. Увеличьте при дрейфе.",
    [UI_MENU_STICK_DEADZONE_NOW] = "стик сейчас: %.0f %%",
    [UI_MENU_MOUSE_MODE] = "Управление мышью",
    [UI_MENU_MOUSE_MODE_DESC] = "Прямое: камера поворачивается на столько, на сколько сдвинута мышь, как в играх для ПК, без мёртвой зоны и кривой стика (версии игры 1.1.1 и 1.4.3; на других версиях эмуляция стика). Эмуляция стика: мышь работает как правый стик.",
    [UI_MENU_MOUSE_STICK] = "Эмуляция стика",
    [UI_MENU_MOUSE_DIRECT] = "Прямое",
    [UI_MENU_MENU_CURSOR] = "Мышь в меню игры",
    [UI_MENU_MENU_CURSOR_DESC] = "В меню самой игры (заставка и файлы сохранений, меню паузы, настройки, магазины, варианты ответа) появляется указатель: пункт под ним выбирается, левый щелчок подтверждает, правый возвращает назад, колесо прокручивает список (в меню паузы листает страницы). Только если последними использовались мышь или клавиатура, поэтому с геймпадом меню не меняются (версии игры 1.1.1 и 1.4.3).",
    [UI_MENU_MOUSE_SENS] = "Чувствительность мыши",
    [UI_MENU_MOUSE_SENS_DESC] = "Как быстро мышь поворачивает камеру.",
    [UI_MENU_WALK_KEY] = "Клавиша шага",
    [UI_MENU_WALK_KEY_DESC] = "Удерживайте, чтобы идти шагом на клавиатуре.",
    [UI_MENU_WALK_KEY_TAKEN] = "также %s",
    [UI_MENU_WALK_SCALE] = "Сила шага",
    [UI_MENU_WALK_SCALE_DESC] = "Отклонение стиков клавиатуры при удержании клавиши шага. Половина — шаг, полное "
                                "отклонение — бег.",
    [UI_MENU_STICK_SENS] = "Скорость камеры (стик)",
    [UI_MENU_STICK_SENS_DESC] = "Скорость камеры на правом стике.",

    [UI_BINDS_HELP] = "Выберите ячейку и нажмите A или Enter, затем клавишу, кнопку мыши или кнопку контроллера. "
                      "Delete или X очищает ячейку. Занятая клавиша переходит сюда; кнопка контроллера меняется "
                      "местами с той, которую заменяет.",
    [UI_BINDS_COL_KEY1] = "Клавиша 1", [UI_BINDS_COL_KEY2] = "Клавиша 2", [UI_BINDS_COL_PAD] = "Контроллер",
    [UI_BINDS_GROUP_BUTTONS] = "Кнопки", [UI_BINDS_GROUP_MOVE] = "Движение", [UI_BINDS_GROUP_CAMERA] = "Камера",
    [UI_BINDS_PRESS_KEY] = "Нажмите клавишу или кнопку мыши для «%s»",
    [UI_BINDS_PRESS_PAD] = "Нажмите кнопку контроллера для «%s»",
    [UI_BINDS_CANCEL_HINT] = "Esc отменяет, или подождите %d с",
    [UI_BINDS_TAKEN] = "%s: убрано с «%s»", [UI_BINDS_SWAPPED] = "«%s» и «%s» поменялись местами",
    [UI_BINDS_RESET] = "Сбросить все кнопки",
    [UI_BINDS_RESET_DESC] = "Возвращает стандартные клавиши и кнопки.",
    [UI_BIND_A] = "A", [UI_BIND_B] = "B", [UI_BIND_X] = "X", [UI_BIND_Y] = "Y",
    [UI_BIND_LCLICK] = "Нажатие левого стика", [UI_BIND_RCLICK] = "Нажатие правого стика", [UI_BIND_L] = "L",
    [UI_BIND_R] = "R", [UI_BIND_ZL] = "ZL", [UI_BIND_ZR] = "ZR", [UI_BIND_PLUS] = "Плюс", [UI_BIND_MINUS] = "Минус",
    [UI_BIND_LEFT] = "Крестовина влево", [UI_BIND_UP] = "Крестовина вверх", [UI_BIND_RIGHT] = "Крестовина вправо",
    [UI_BIND_DOWN] = "Крестовина вниз",
    [UI_BIND_LSTICK_LEFT] = "Влево", [UI_BIND_LSTICK_UP] = "Вперёд", [UI_BIND_LSTICK_RIGHT] = "Вправо",
    [UI_BIND_LSTICK_DOWN] = "Назад",
    [UI_BIND_RSTICK_LEFT] = "Камера влево", [UI_BIND_RSTICK_UP] = "Камера вверх",
    [UI_BIND_RSTICK_RIGHT] = "Камера вправо", [UI_BIND_RSTICK_DOWN] = "Камера вниз",
    [UI_BIND_DESC_A] = "Действия: поговорить, открыть, подобрать, залезть, схватить. Удерживать — рывок.",
    [UI_BIND_DESC_B] = "Убрать, отмена, назад. Прыжок, если в настройках игры включена смена кнопки прыжка.",
    [UI_BIND_DESC_X] = "Прыжок. Убрать, если в настройках игры включена смена кнопки прыжка.",
    [UI_BIND_DESC_Y] = "Удар мечом.",
    [UI_BIND_DESC_LCLICK] = "Нажатие левого стика (нет на экране управления игры).",
    [UI_BIND_DESC_RCLICK] = "Вернуть камеру за спину Линка.",
    [UI_BIND_DESC_L] = "Быстрый выбор предметов; предыдущая страница в меню игры.",
    [UI_BIND_DESC_R] = "Быстрый выбор предметов; следующая страница в меню игры.",
    [UI_BIND_DESC_ZL] = "Захват цели (удерживать или переключать: тип захвата в настройках игры).",
    [UI_BIND_DESC_ZR] = "Использовать выбранный предмет: рогатка, лук, бумеранг, крюк, бомбы, бутылки.",
    [UI_BIND_DESC_PLUS] = "Меню: предметы, снаряжение, коллекция, система.", [UI_BIND_DESC_MINUS] = "Карта.",
    [UI_BIND_DESC_LEFT] = "Играть на окарине.", [UI_BIND_DESC_UP] = "Надеть или снять аксессуар.",
    [UI_BIND_DESC_RIGHT] = "Поговорить с Нави.", [UI_BIND_DESC_DOWN] = "Сменить сапоги.",
    [UI_BIND_DESC_MOVE] = "Двигает Линка.",
    [UI_BIND_DESC_CAMERA] = "Поворачивает камеру.",
    [UI_BIND_SHAKE] = "Встряхнуть стик камеры",
    [UI_BIND_DESC_SHAKE] = "Трясёт правый стик. Удерживайте, чтобы продолжать.",
    [UI_BIND_ACTION_A] = "Действия / бег", [UI_BIND_ACTION_B] = "Убрать / назад", [UI_BIND_ACTION_X] = "Прыжок",
    [UI_BIND_ACTION_Y] = "Удар мечом", [UI_BIND_ACTION_LCLICK] = "—", [UI_BIND_ACTION_RCLICK] = "Вернуть камеру",
    [UI_BIND_ACTION_L] = "Предм. / назад", [UI_BIND_ACTION_R] = "Предм. / далее",
    [UI_BIND_ACTION_ZL] = "Захват цели", [UI_BIND_ACTION_ZR] = "Исп. предмет", [UI_BIND_ACTION_PLUS] = "Меню паузы",
    [UI_BIND_ACTION_MINUS] = "Карта", [UI_BIND_ACTION_LEFT] = "Окарина", [UI_BIND_ACTION_UP] = "Аксессуар",
    [UI_BIND_ACTION_RIGHT] = "Позвать Нави", [UI_BIND_ACTION_DOWN] = "Сменить сапоги", [UI_BIND_ACTION_MOVE] = "Движение",
    [UI_BIND_ACTION_CAMERA] = "Камера",
    [UI_KEY_SPACE] = "Пробел", [UI_KEY_TAB] = "Tab", [UI_KEY_LSHIFT] = "Левый Shift", [UI_KEY_LCTRL] = "Левый Ctrl",
    [UI_KEY_UP] = "Стрелка вверх", [UI_KEY_DOWN] = "Стрелка вниз", [UI_KEY_LEFT] = "Стрелка влево",
    [UI_KEY_RIGHT] = "Стрелка вправо",
    [UI_KEY_MOUSE_L] = "ЛКМ", [UI_KEY_MOUSE_R] = "ПКМ", [UI_KEY_MOUSE_M] = "СКМ", [UI_KEY_MOUSE_X1] = "Мышь 4",
    [UI_KEY_MOUSE_X2] = "Мышь 5",
    [UI_KEY_ENTER] = "Enter", [UI_KEY_BACKSPACE] = "Backspace", [UI_KEY_RSHIFT] = "Правый Shift",
    [UI_KEY_RCTRL] = "Правый Ctrl", [UI_KEY_LALT] = "Alt", [UI_KEY_CAPSLOCK] = "Caps Lock", [UI_KEY_INSERT] = "Insert",
    [UI_KEY_DELETE] = "Delete", [UI_KEY_HOME] = "Home", [UI_KEY_END] = "End", [UI_KEY_PAGEUP] = "Page Up",
    [UI_KEY_PAGEDOWN] = "Page Down", [UI_KEY_NUM_FMT] = "Num %s",
    [UI_PAD_SOUTH] = "Нижняя кнопка", [UI_PAD_EAST] = "Правая кнопка", [UI_PAD_WEST] = "Левая кнопка",
    [UI_PAD_NORTH] = "Верхняя кнопка", [UI_PAD_LSTICK] = "Левый стик", [UI_PAD_RSTICK] = "Правый стик",
    [UI_PAD_L] = "Левый бампер", [UI_PAD_R] = "Правый бампер", [UI_PAD_START] = "Start", [UI_PAD_BACK] = "Back",
    [UI_PAD_DPAD_LEFT] = "Крестовина влево", [UI_PAD_DPAD_UP] = "Крестовина вверх",
    [UI_PAD_DPAD_RIGHT] = "Крестовина вправо", [UI_PAD_DPAD_DOWN] = "Крестовина вниз", [UI_PAD_ZL] = "Левый курок",
    [UI_PAD_ZR] = "Правый курок",
    [UI_PAD_LPADDLE1] = "Левая задняя кнопка", [UI_PAD_RPADDLE1] = "Правая задняя кнопка",
    [UI_PAD_LPADDLE2] = "Левая задняя кнопка 2", [UI_PAD_RPADDLE2] = "Правая задняя кнопка 2",
    [UI_PAD_CROSS] = "Крест", [UI_PAD_CIRCLE] = "Круг", [UI_PAD_SQUARE] = "Квадрат", [UI_PAD_TRIANGLE] = "Треугольник",

    [UI_MENU_AMIIBO_DIR] = "Папка amiibo",
    [UI_MENU_AMIIBO_OPEN] = "Открыть",
    [UI_MENU_AMIIBO_DEFAULT_DIR] = "Папка по умолчанию",
    [UI_MENU_AMIIBO_REFRESH] = "Обновить",
    [UI_MENU_AMIIBO_NO_DIR] = "Не удаётся открыть папку %s",
    [UI_MENU_AMIIBO_NONE_FOUND] = "В %s нет дампов amiibo (файлов .bin или .nfc по 540 байт)",
    [UI_MENU_AMIIBO_FILTER] = "Поиск",
    [UI_MENU_AMIIBO_LIST_HINT] = "A или щелчок прикладывает amiibo к считывателю.",
    [UI_MENU_AMIIBO_ADVANCED] = "Дополнительно",
    [UI_MENU_AMIIBO_READER] = "NFC-считыватель",
    [UI_MENU_AMIIBO_STATE] = "Считыватель:",
    [UI_MENU_AMIIBO_STATE_IDLE] = "готов",
    [UI_MENU_AMIIBO_STATE_SEARCHING] = "игра ждёт amiibo",
    [UI_MENU_AMIIBO_STATE_FOUND] = "amiibo на считывателе",
    [UI_MENU_AMIIBO_STATE_MOUNTED] = "игра читает amiibo",
    [UI_MENU_AMIIBO_STATE_OFF] = "выключен",
    [UI_MENU_AMIIBO_REMOVE] = "Убрать amiibo",
    [UI_MENU_AMIIBO_AUTO_REMOVE] = "Убирать amiibo само после касания",
    [UI_MENU_AMIIBO_HOLD] = "Время на считывателе",
    [UI_MENU_AMIIBO_SECONDS] = "%.1f с",
    [UI_MENU_AMIIBO_NO_TAG] = "На считывателе нет amiibo.",
    [UI_MENU_AMIIBO_TAG_FILE] = "Файл: %s",
    [UI_MENU_AMIIBO_TAG_ID] = "Персонаж %s, серия %02x, номер %04x, тип %02x",
    [UI_MENU_AMIIBO_TAG_UID] = "UID: %s",
    [UI_MENU_AMIIBO_READY] = "Готово",
    [UI_MENU_AMIIBO_TAPPED] = "Приложен: %s",
    [UI_MENU_AMIIBO_LOAD_FAILED] = "Не дамп amiibo (NTAG215, 540 байт): %s",
    [UI_MENU_AMIIBO_REMOVED] = "Amiibo убран",
    [UI_MENU_AMIIBO_NICKNAME] = "Имя: %s",
    [UI_MENU_AMIIBO_NOT_REGISTERED] = "Владелец и имя не заданы",
    [UI_MENU_AMIIBO_SOURCE] = "Данные фигурки: %s",
    [UI_MENU_AMIIBO_SOURCE_NEW] = "новые, ещё не сохранены",
    [UI_MENU_AMIIBO_SOURCE_SAVED] = "сохранены портом",
    [UI_MENU_AMIIBO_SOURCE_DUMP] = "прочитаны из дампа",
    [UI_MENU_AMIIBO_DECRYPTED] = "Дамп расшифрован ключами",
    [UI_MENU_AMIIBO_NOT_DECRYPTED] = "Дамп не расшифрован",
    [UI_MENU_AMIIBO_WRITES] = "Записей: %u, последняя %04u-%02u-%02u",
    [UI_MENU_AMIIBO_GAME_DATA] = "Данные игры: %u байт для ID доступа %08X",
    [UI_MENU_AMIIBO_NO_GAME_DATA] = "Данные игры: нет",
    [UI_MENU_AMIIBO_HAS_GAME_DATA] = "Данные игры: сохранены",
    [UI_MENU_AMIIBO_ERASE] = "Стереть данные игры",
    [UI_MENU_AMIIBO_ERASE_CONFIRM] = "Нажмите ещё раз, чтобы стереть",
    [UI_MENU_AMIIBO_ERASED] = "Данные игры стёрты",
    [UI_MENU_AMIIBO_ERASE_FAILED] = "Не удалось стереть данные игры",
    [UI_MENU_AMIIBO_KEYS] = "Ключи:",
    [UI_MENU_AMIIBO_KEYS_LOADED] = "загружены",
    [UI_MENU_AMIIBO_KEYS_MISSING] = "не загружены",
    [UI_MENU_AMIIBO_KEYS_FILE] = "Файл ключей: %s",
    [UI_MENU_AMIIBO_RELOAD_KEYS] = "Перезагрузить ключи",
    [UI_MENU_AMIIBO_KEYS_RELOADED] = "Ключи загружены: %s",
    [UI_MENU_AMIIBO_KEYS_FAILED] = "Ключи не загружены (файла нет или это не файл ключей на 160 байт): %s",
    [UI_MENU_AMIIBO_WRITEBACK] = "Записывать изменения игры в файл дампа",
    [UI_MENU_AMIIBO_WRITEBACK_NEEDS_KEYS] = "нужны ключи",
    [UI_MENU_AMIIBO_POPUP] = "Показывать список amiibo, когда игра просит amiibo",
    [UI_MENU_AMIIBO_POPUP_TITLE] = "Игра ждёт amiibo",
    [UI_MENU_AMIIBO_POPUP_HINT] = "A: приложить   B: закрыть",
    [UI_MENU_AMIIBO_DAILY_LIMIT] = "Дневной лимит amiibo",
    [UI_MENU_AMIIBO_DAILY_LIMIT_DESC] = "Вкл., как в игре: фигурка даёт предметы раз в день, после чего игра пишет, "
                                        "что сегодня её больше использовать нельзя. Выкл.: предметы при каждом "
                                        "прикладывании. Две фигурки, которые дают маску, не ограничены в любом случае.",
    [UI_MENU_DISCORD_RPC] = "Discord Rich Presence",
    [UI_MENU_DISCORD_RPC_DESC] = "Отправляет активность и время игры в Discord. Сначала укажите свой discord_app_id "
                                 "в nx2saria.ini или NX2SARIA_DISCORD_APP_ID; Discord показывает имя, зарегистрированное "
                                 "для этого ID.",

    [UI_MENU_RESET_NO] = "Отмена",

    [UI_MENU_OV_FPS] = "%.0f FPS", [UI_MENU_OV_FRAME_MS] = "%.1f мс", [UI_MENU_OV_GAME_PCT] = "игра %.0f%%",
    [UI_MENU_OV_MAX_MS] = "макс. %.1f мс",
    [UI_MENU_OV_FPS_LABEL] = "FPS", [UI_MENU_OV_FRAME_LABEL] = "Кадр", [UI_MENU_OV_FMT_FPS] = "%.0f",
    [UI_MENU_OV_LOW1] = "1%% худших %.0f", [UI_MENU_OV_LOW01] = "0.1%% худших %.0f",
    [UI_MENU_OV_GPU] = "GPU", [UI_MENU_OV_CPU] = "ЦП", [UI_MENU_OV_RAM] = "ОЗУ", [UI_MENU_OV_VRAM] = "Видеопамять",
    [UI_MENU_OV_NONE] = "-",
    [UI_MENU_OV_REPLAY] = "Подготовка кадра для GPU", [UI_MENU_OV_GPU_TIME] = "Занятость GPU за кадр",
    [UI_MENU_OV_PIPE_WAIT] = "Ожидания конвейеров",
    [UI_MENU_OV_SHADER_WAIT] = "Ожидания шейдеров", [UI_MENU_OV_TEX_UPLOADS] = "Загрузки текстур",
    [UI_MENU_OV_FMT_PCT] = "%.0f%%", [UI_MENU_OV_FMT_TEMP] = "%.0f °C", [UI_MENU_OV_FMT_POWER] = "%.0f Вт",
    [UI_MENU_OV_FMT_MHZ] = "%.0f МГц", [UI_MENU_OV_FMT_GB] = "%.1f ГБ", [UI_MENU_OV_FMT_GB_OF] = "/ %.1f ГБ",
    [UI_MENU_OV_GAME_GB] = "игра %.1f ГБ", [UI_MENU_OV_FMT_MS] = "%.2f мс", [UI_MENU_OV_PER_FRAME] = "%.1f за кадр",
    [UI_MENU_OV_WINDOW] = "Окно", [UI_MENU_OV_OUTPUT] = "Изображение игры", [UI_MENU_OV_SCENE] = "3D-сцена",
    [UI_MENU_OV_LIMIT] = "Лимит кадров",
    [UI_MENU_OV_FMT_SIZE] = "%ux%u", [UI_MENU_OV_FMT_SCENE] = "%ux%u, %.0f%%",
    [UI_MENU_OV_FMT_FSR] = "AMD FSR 1, резкость %d%%",
    [UI_MENU_TAB_CHEATS] = "Читы",
    [UI_MENU_CHEATS_NONE] = "Для этой версии игры читов нет. Читы мода — это файл читов Atmosphere в его папке cheats "
                            "с именем по сборке игры: mods\\<мод>\\cheats\\%s. Перезапустите игру, чтобы они "
                            "прочитались. Для читов, меняющих код игры, игра пересоберётся сама при запуске (несколько "
                            "минут).",
    [UI_MENU_CHEATS_MOD] = "%s — %s",
    [UI_MENU_CHEAT_CODE_DESC] = "Меняет код игры. Включается и выключается сразу.",
    [UI_MENU_CHEAT_KEY_DESC] = "Меняет код игры, пока вы держите %s. Включается и выключается сразу.",
    [UI_MENU_CHEAT_DATA_DESC] = "Пока включён, записывает память игры каждый кадр.",
    [UI_MENU_CHEAT_MASTER] = "всегда вкл.",
    [UI_MENU_CHEAT_MASTER_DESC] = "Главный код файла: включён всегда.",
    [UI_MENU_CHEAT_RESTORE] = "не нужен",
    [UI_MENU_CHEAT_RESTORE_DESC] = "Он только возвращает исходный код игры. Выключение чита уже делает это.",
    [UI_MENU_CHEAT_REBUILD] = "после пересборки",
    [UI_MENU_CHEAT_REBUILD_DESC] = "Этот чит меняет код игры и появился или изменился после сборки игры. Его соберёт "
                                   "Play в лаунчере.",
    [UI_MENU_CHEAT_UNSUPPORTED] = "не поддерживается",
    [UI_MENU_CHEAT_UNSUPPORTED_DESC] = "Этот чит нельзя использовать с этой версией игры:",
    [UI_MENU_SAVES] = "Сохранения",
    [UI_MENU_SAVE_EXPORT] = "Экспорт сохранения",
    [UI_MENU_SAVE_EXPORT_DESC] = "Записывает ваше сохранение (все слоты) в один zip-файл в папке «Загрузки», чтобы "
                                 "передать его другому игроку или приложить к сообщению об ошибке. Ключи и файлы игры "
                                 "в него не попадают.",
    [UI_MENU_SAVE_EXPORT_BUG] = "Экспорт для сообщения об ошибке",
    [UI_MENU_SAVE_EXPORT_BUG_DESC] = "Тот же zip с журналом этого запуска и вашими настройками (nx2saria.ini): по ним "
                                     "видно, что произошло и с какими настройками. Приложите его к сообщению об ошибке.",
    [UI_MENU_SAVE_IMPORT] = "Импорт сохранения",
    [UI_MENU_SAVE_IMPORT_DESC] = "Выберите сохранение, которым с вами поделились (zip NX2Saria-save). Оно заменит ваше "
                                 "сохранение, все слоты, при следующем запуске: игра держит прогресс в памяти и "
                                 "записала бы его поверх сохранения, заменённого сейчас. Текущее сохранение сначала "
                                 "копируется в папку saves.",
    [UI_MENU_SAVE_IMPORT_WAITING] = "при следующем запуске",
    [UI_MENU_SAVE_IMPORT_CANCEL] = "Отменить импорт",
    [UI_MENU_SAVE_IMPORT_CANCEL_DESC] = "Оставляет ваше сохранение как есть: сохранение, ждущее следующего запуска, "
                                        "удаляется.",
    [UI_MENU_SAVE_BUSY] = "выполняется…",
    [UI_MENU_SAVE_EXPORTED] = "Сохранено: %s",
    [UI_MENU_SAVE_IMPORT_READY] = "Сохранение (NX2Saria %s) загрузится при следующем запуске: выберите «Перезапустить» "
                                  "внизу. Текущее сохранение сначала копируется в папку saves.",
    [UI_MENU_SAVE_IMPORT_CANCELLED] = "Импорт отменён, ваше сохранение не изменилось.",
    [UI_MENU_SAVE_IMPORTED] = "При этом запуске импортировано сохранение. Прежнее сохранение лежит в %s",
    [UI_MENU_SAVE_IMPORTED_NEW] = "При этом запуске импортировано сохранение.",
    [UI_MENU_SAVE_E_NO_SAVE] = "Экспортировать пока нечего: играйте, пока игра не сохранится.",
    [UI_MENU_SAVE_E_READ_SAVE] = "Не удалось прочитать файл сохранения; попробуйте ещё раз чуть позже.",
    [UI_MENU_SAVE_E_WRITE] = "Не удалось записать zip в %s.",
    [UI_MENU_SAVE_E_READ] = "Не удалось прочитать %s.",
    [UI_MENU_SAVE_E_STAGE] = "Не удалось подготовить сохранение к следующему запуску; причина указана в журнале.",
    [UI_MENU_SAVE_E_NOT_SAVE] = "%s — не сохранение, экспортированное NX2Saria.",
    [UI_MENU_SAVE_E_OTHER_GAME] = "%s — сохранение другой игры.",
    [UI_MENU_SAVE_E_NEWER] = "%s создан более новой версией NX2Saria: сначала обновите NX2Saria.",
    [UI_MENU_SAVE_E_DAMAGED] = "%s повреждён: скачайте его заново.",
    [UI_MENU_SAVE_E_UNSAFE] = "%s отклонён: в нём файлы вне сохранения, ссылки или слишком много данных.",
    [UI_MENU_CAPTURE_SAVED] = "Снимок ошибки %d сохранён",
    [UI_MENU_CAPTURE_FOLDER] = "Папка: %s",
    [UI_MENU_CAPTURE_FAILED] = "Снимок ошибки %d не удалось сохранить",
    [UI_BOOT_TITLE] = "Подготовка шейдеров",
    [UI_BOOT_STARTING] = "Запуск", [UI_BOOT_SHADERS] = "Шейдеры: %lld из %lld", [UI_BOOT_PIPELINES] = "Конвейеры: %lld из %lld",
    [UI_BOOT_COMPUTE] = "Вычислительные конвейеры: %lld из %lld", [UI_BOOT_DRIVER_CACHE] = "Кэш драйвера: %lld из %lld",
    [UI_MENU_CHEATS_UNBUILT] = "Не собрано в игру: %s и %s. Их соберёт Play в лаунчере.",
    [UI_MENU_COUNT_CODE_CHEATS] = "%d чит, меняющий код|%d чита, меняющих код|%d читов, меняющих код",
    [UI_MENU_COUNT_EXEFS_FILES] = "%d файл патчей ExeFS|%d файла патчей ExeFS|%d файлов патчей ExeFS",
    [UI_MENU_COUNT_CHEATS] = "%d чит|%d чита|%d читов",
    [UI_MENU_PRESET] = "Пресет качества",
    [UI_MENU_PRESET_DESC] = "Задаёт все настройки графики сразу. «Рекомендуемый» — пресет, который этот ПК получает по своей "
        "видеокарте, памяти и процессору. «Высокий» — игра в том виде, в каком она вышла; «Низкий» и «Средний» "
        "рисуют меньше, для слабых ПК и портативных устройств; «Ультра» рисует больше, и тем больше, чем крупнее "
        "монитор; он никогда не рекомендуется, только выбирается. Настройки, которые вы потом измените вручную, остаются как вы их задали, и пресет отмечается как "
        "изменённый.",
    [UI_MENU_PRESET_RECOMMENDED] = "Рекомендуемый (%s)",
    [UI_MENU_PRESET_LOW] = "Низкий",
    [UI_MENU_PRESET_MEDIUM] = "Средний",
    [UI_MENU_PRESET_HIGH] = "Высокий",
    [UI_MENU_PRESET_ULTRA] = "Ультра",
    [UI_MENU_PRESET_CUSTOM] = "%s (изменён)",
    [UI_MENU_PRESET_CUSTOM_NOTE] = "изменено вручную: %d",
    [UI_MENU_PRESET_REC_NOTE] = "Рекомендуется для этого ПК: %s",
    [UI_MENU_PRESET_HW_NOTE] = "Новое оборудование: используется рекомендуемый пресет",
    [UI_MENU_PRESET_NOTICE] = "Для этого ПК используется пресет качества «Рекомендуемый (%s)» (Настройки > Экран). Чтобы вернуть игру как раньше, выберите там «Высокий».",
    [UI_MENU_PRESET_CONFIRM] = "Эти настройки изменены вручную. Пресет задаст им свои значения:",
    [UI_MENU_PRESET_CONFIRM_YES] = "Применить пресет",
    [UI_MENU_GAME_RES] = "Разрешение",
    [UI_MENU_GAME_RES_NATIVE] = "Родное",
    [UI_MENU_GAME_RES_DESC] = "Размер картинки игры; вне полноэкранного режима его принимает и окно. Чем выше, тем "
                              "чётче, но тем больше нагрузка на видеокарту.",
    [UI_MENU_SCENE_RES] = "Разрешение сцены",
    [UI_MENU_SCENE_RES_DESC] = "Размер, в котором рисуется 3D-сцена, как доля картинки. Меньшее значение рисует сцену меньше и "
        "растягивает её до картинки: быстрее, для слабых ПК и портативных устройств, но мягче. Интерфейс и меню "
        "остаются чёткими, в полном разрешении. 100 % — родное. Изменения применяются сразу.",
    [UI_MENU_SCENE_RES_GAME] = "%s (игра)",
    [UI_MENU_SCENE_RES_NATIVE] = "%s (родное)",
    [UI_MENU_SCENE_RES_PRESET] = "%s (пресет)",
    [UI_MENU_DYN_RES] = "Динамическое разрешение",
    [UI_MENU_DYN_RES_DESC] = "Снижает разрешение 3D-сцены, пока видеокарта не успевает за ограничением частоты кадров, "
        "и снова повышает его, когда успевает: немного чёткости в тяжёлых сценах ради более ровной частоты кадров. "
        "Интерфейс и меню остаются чёткими. С DLSS или FSR понижает режим апскейлера, не ниже «Производительность»; "
        "без них снижает разрешение сцены, не ниже минимума из строки ниже. Изменения применяются сразу.",
    [UI_MENU_DYN_RES_MIN] = "Мин. разрешение сцены",
    [UI_MENU_DYN_RES_MIN_DESC] = "Самое низкое разрешение, до которого динамическое разрешение опускает 3D-сцену, как "
        "доля картинки. Меньшее значение держит частоту кадров ровнее на слабом ПК, но тяжёлые сцены выглядят мягче. "
        "Выше выбранного «Разрешения сцены» оно не поднимается. С DLSS или FSR не действует: вместо этого режим "
        "апскейлера опускается до «Производительность». Изменения применяются сразу.",
    [UI_MENU_DYN_RES_MIN_OFF] = "только с включённым динамическим разрешением",
    [UI_MENU_DYN_RES_NO_ROOM] = "нечего снижать: разрешение сцены уже на минимуме или ниже",
    [UI_MENU_DYN_RES_NATIVE] = "нечего снижать при DLAA или Native AA",
    [UI_MENU_VRAM_WARN] = "Видеопамять заполнена, поэтому игра идёт медленно. В меню «Настройки > Экран»:",
    [UI_MENU_VRAM_WARN_RES] = "- %s: выберите ниже",
    [UI_MENU_VRAM_WARN_MODE] = "- %s: выберите режим легче, чем %s",
    [UI_MENU_VRAM_WARN_FRAMEGEN] = "- %s: отключите",
    [UI_MENU_VRAM_WARN_PRESET] = "- %s: выберите ниже",
    [UI_MENU_VRAM_FIT_MEASURED] = "Видеопамять уже занята на %d%% (%llu из %llu МиБ). Больше видеопамяти требуют: %s.",
    [UI_MENU_VRAM_FIT_NOTICED] = "В этой сессии видеопамять уже заканчивалась (сейчас занято %d%%, %llu из %llu МиБ). Больше видеопамяти требуют: %s.",
    [UI_MENU_VRAM_FIT_PRESET] = "У этой видеокарты %u ГиБ видеопамяти. Рекомендуемый пресет для неё использует: %s.",
    [UI_MENU_VRAM_FIT_FULL] = "Видеопамять уже заполнена (%llu из %llu МиБ). Больше видеопамяти требуют: %s.",
};

static const char *const text_pt_br[UI_TEXT_COUNT] = {
    SWKBD("Inserir texto", "Insira um nome", "Cavalo", "OK", "Cancelar", "Controle: A = OK, B = Cancelar"),

    [UI_MENU_TITLE] = "Configurações",
    [UI_MENU_TAB_SCREEN] = "Tela", [UI_MENU_TAB_GAME] = "Jogo", [UI_MENU_TAB_SOUND] = "Som",
    [UI_MENU_TAB_CONTROLS] = "Controles", [UI_MENU_TAB_AMIIBO] = "Amiibo",
    [UI_MENU_CONTINUE] = "Continuar", [UI_MENU_QUIT] = "Sair do jogo",
    [UI_MENU_ON] = "Ligado", [UI_MENU_OFF] = "Desligado", [UI_MENU_AUTO] = "Automático",
    [UI_MENU_DEFAULT_MARK] = "padrão",
    [UI_MENU_AFTER_RESTART] = "após reiniciar", [UI_MENU_SET_BY_ENV] = "definido por variável de ambiente",
    [UI_MENU_SAVE_FAILED] = "não foi possível salvar as configurações",
    [UI_MENU_INI_UNKNOWN] = "nx2saria.ini: não são configurações desta versão, ignoradas: %s",
    [UI_MENU_VERSION] = "NX2Saria %s, jogo %s",

    [UI_MENU_FULLSCREEN] = "Tela cheia", [UI_MENU_FULLSCREEN_DESC] = "Jogar em tela cheia. F11 alterna.",
    [UI_MENU_WINDOW_SIZE] = "Tamanho da janela",
    [UI_MENU_WINDOW_SIZE_DESC] = "Tamanho da janela fora da tela cheia.",
    [UI_MENU_PRESENT_DESC] = "Evita cortes na imagem. Desligado reduz um pouco o atraso.",
    [UI_MENU_PRESENT_VSYNC] = "VSync", [UI_MENU_PRESENT_IMMEDIATE] = "Imediato",
    [UI_MENU_PRESENT_IN_USE] = "em uso: %s",
    [UI_MENU_LOW_LATENCY] = "Baixa latência",
    [UI_MENU_LOW_LATENCY_DESC] = "O driver de vídeo segura o início de cada quadro, para ler o controle o mais tarde "
                                 "possível (NVIDIA Reflex, AMD Anti-Lag). Ele também mantém o limite de quadros. Boost "
                                 "mantém a placa de vídeo em frequência máxima.",
    [UI_MENU_LOW_LATENCY_BOOST] = "Ligado + Boost",
    [UI_MENU_LOW_LATENCY_NO_DRIVER] = "este driver de vídeo não tem modo de baixa latência",
    [UI_MENU_LOW_LATENCY_DLSSG] = "a geração de quadros DLSS usa o seu próprio",
    [UI_MENU_LOW_LATENCY_FG] = "desligado enquanto a geração de quadros roda nesta placa",
    [UI_MENU_FRAMEGEN_DESC_ANY] = "Adiciona quadros gerados para um movimento mais suave, com qualquer opção em "
                                  "Upscaling.",
    [UI_MENU_FRAMEGEN_DESC_NGX] = "Adiciona quadros gerados para um movimento mais suave. O NVIDIA DLSS precisa de um "
                                  "modo DLSS em Upscaling quando o jogo inicia; depois funciona com qualquer opção em "
                                  "Upscaling.",
    [UI_MENU_FRAMEGEN_NOTE_NGX] = "precisa do DLSS ao iniciar o jogo",
    [UI_MENU_FRAMEGEN_DESC_FSR_WIN] = "Adiciona quadros gerados para um movimento mais suave. O upscaling FSR 3.1 roda "
                                      "aqui pelas passagens que o próprio jogo compila, mas a geração de quadros do FSR "
                                      "3.1 precisa da biblioteca da AMD, que só a versão para Windows carrega. Aqui a "
                                      "geração de quadros precisa do NVIDIA DLSS numa GeForce RTX 40 ou 50.",
    [UI_MENU_FRAMEGEN_NEEDS_FSR_WIN] = "a geração de quadros do FSR 3.1 só roda no Windows",
    [UI_MENU_FRAMEGEN_OFF_FSR_WIN] = "em uso: desligada, a geração de quadros do FSR 3.1 só roda no Windows",
    [UI_MENU_FRAMEGEN_FSR3_NATIVE] = "2x, passagens FSR 3.1 do próprio jogo",
    [UI_MENU_FRAMEGEN_OFF_HDR] = "em uso: desligada com HDR (só SDR)",
    [UI_MENU_TEX_CLOBBER] = "Corrigir texturas sobrescritas",
    [UI_MENU_TEX_CLOBBER_DESC] = "Mantém as texturas intactas quando o jogo desenha outra imagem na mesma memória. "
                               "Corrige os quadrados verdes na névoa e pode usar mais memória de vídeo.",
    [UI_MENU_TEX_CLOBBER_NOTE] = "corrige quadrados verdes na névoa",
    [UI_MENU_LOW_LATENCY_FAILED] = "em uso: desligado, o driver recusou (veja lowlat no log)",
    [UI_MENU_GPU] = "Placa de vídeo", [UI_MENU_GPU_DESC] = "A placa de vídeo usada pelo jogo.",
    [UI_MENU_GPU_AUTO] = "Automática",
    [UI_MENU_PERF_OVERLAY] = "Indicador de desempenho",
    [UI_MENU_PERF_OVERLAY_DESC] = "Mostra a taxa de quadros e dados do sistema. F10 alterna.",
    [UI_MENU_PERF_OFF] = "Desligado", [UI_MENU_PERF_FPS] = "FPS", [UI_MENU_PERF_FULL] = "Completo",
    [UI_PERF_LINE_FPS] = "%.0f fps",
    [UI_MENU_GRAPHICS] = "Imagem",
    [UI_MENU_OUTPUT_RES] = "Resolução do jogo",
    [UI_MENU_OUTPUT_RES_DESC] = "A resolução do jogo. Mais alta exige mais da placa de vídeo.",
    [UI_MENU_UPSCALER_FSR] = "AMD FSR 1", [UI_MENU_UPSCALER_BILINEAR] = "Bilinear",
    [UI_MENU_UPSCALING_NATIVE] = "Nativo (100%)",
    [UI_MENU_UPSCALING_DESC] = "Como a imagem é suavizada e ajustada à janela. Nativo desenha a cena 3D no tamanho "
                               "total da imagem, sem upscaler: a mais nítida, e a que mais exige da GPU.",
    [UI_MENU_UPSCALING_DESC_FSR] = "Como a imagem é suavizada e ajustada à janela. Nativo desenha a cena 3D no "
                                   "tamanho total da imagem, sem upscaler: a mais nítida, e a que mais exige da GPU.",
    [UI_MENU_UPSCALING_DESC_NO_FSR] = "Como a imagem é suavizada e ajustada à janela. Nativo desenha a cena 3D no "
                                      "tamanho total da imagem, sem upscaler: a mais nítida, e a que mais exige da "
#ifdef _WIN32
                                      "GPU. O AMD FSR 3.1 não é oferecido aqui: ele precisa do amd_fidelityfx_vk.dll, "
                                      "que não pode ser carregado.",
#else
                                      "GPU. O AMD FSR 3.1 não é oferecido aqui: o FSR 3.1 próprio do jogo não roda "
                                      "nesta GPU ou neste driver (veja o motivo na linha fsr3 do log).",
#endif
    [UI_MENU_GAME_ASPECT] = "Formato do jogo",
    [UI_MENU_GAME_ASPECT_DESC] = "Ajusta a imagem a um formato mais alto ou mais largo. Formatos altos mantêm a visão horizontal de 16:9 e mostram mais acima e abaixo; o HUD fica centralizado.",
    [UI_MENU_GAME_ASPECT_16_9] = "16:9 (o do jogo)", [UI_MENU_GAME_ASPECT_WINDOW] = "Formato da janela",
    [UI_MENU_GAME_ASPECT_16_10] = "16:10", [UI_MENU_GAME_ASPECT_3_2] = "3:2",
    [UI_MENU_GAME_ASPECT_21_9] = "21:9", [UI_MENU_GAME_ASPECT_32_9] = "32:9",
    [UI_MENU_WIDE_HUD] = "Posição do HUD",
    [UI_MENU_WIDE_HUD_DESC] = "Onde o HUD fica numa imagem mais larga que 16:9. 16:9 central mantém o layout do jogo; Bordas da tela põe os corações e a magia no canto superior esquerdo, o item no superior direito, e o minimapa, as chaves e as rupias na borda direita.",
    [UI_MENU_WIDE_HUD_CENTER] = "16:9 central", [UI_MENU_WIDE_HUD_EDGES] = "Bordas da tela",
    [UI_MENU_WIDE_MENUS] = "Menus",
    [UI_MENU_WIDE_MENUS_DESC] = "O menu de pausa, o menu do sistema, o mapa e as telas de carregamento numa imagem mais larga que 16:9. Preencher a tela os distribui pela imagem inteira; Barras pretas os mantém no 16:9 central, com as laterais pretas.",
    [UI_MENU_WIDE_MENUS_FULL] = "Preencher a tela", [UI_MENU_WIDE_MENUS_CENTER] = "Barras pretas",
    [UI_MENU_SHARPNESS] = "Nitidez do FSR",
    [UI_MENU_SHARPNESS_DESC] = "Nitidez aplicada pelo FSR. 0 desliga.",
    [UI_MENU_SHARPNESS_NEEDS_FSR] = "só com AMD FSR 1",

    [UI_MENU_FPS_LIMIT] = "Limite de quadros",
    [UI_MENU_FPS_LIMIT_DESC] = "A taxa máxima de quadros que o jogo desenha. A geração de quadros soma os dela por cima.",
    [UI_MENU_ASSUME_FOCUS] = "Continuar rodando fora de foco",
    [UI_MENU_ASSUME_FOCUS_DESC] = "Mantém o jogo rodando quando você troca de janela.",
    [UI_MENU_TEXT_SPEED] = "Velocidade do texto",
    [UI_MENU_TEXT_SPEED_DESC] = "A rapidez com que as janelas de mensagem do jogo mostram o texto. Rápida escreve três "
                                "vezes mais rápido; instantânea mostra cada página de uma vez. Textos que o jogo não "
                                "deixa apressar, como falas sincronizadas com uma cena ou uma voz, mantêm a velocidade.",
    [UI_MENU_TEXT_SPEED_NORMAL] = "Normal", [UI_MENU_TEXT_SPEED_FAST] = "Rápida",
    [UI_MENU_TEXT_SPEED_INSTANT] = "Instantânea",
    [UI_MENU_FPS_DISPLAY] = "Taxa do monitor", [UI_MENU_FPS_DISPLAY_HZ] = "Taxa do monitor (%.0f Hz)",
    [UI_MENU_FPS_UNLIMITED] = "Sem limite",
    [UI_MENU_FPS_RTSS] = "limitado pelo RivaTuner Statistics Server",
    [UI_MENU_FPS_RTSS_READONLY] = "o RivaTuner pede o jogo como administrador",
    [UI_MENU_FPS_RTSS_IGNORED] = "o RivaTuner não limita este jogo; o jogo limita",
    [UI_MENU_FPS_RTSS_TWO] = "o limite do próprio RivaTuner também vale: dois limitadores",
    [UI_MENU_GAME_LANG] = "Idioma do jogo", [UI_MENU_GAME_LANG_DESC] = "Idioma dos textos e das vozes do jogo.",
    [UI_MENU_GAME_LANG_AUTO] = "Automático",
    [UI_MENU_OPERATION_MODE] = "Modo do console",
    [UI_MENU_OPERATION_MODE_DESC] = "Roda o jogo como na base ou no modo portátil.",
    [UI_MENU_MODE_DOCKED] = "Modo TV", [UI_MENU_MODE_HANDHELD] = "Modo portátil",

    [UI_MENU_VOLUME] = "Volume", [UI_MENU_VOLUME_DESC] = "Volume geral do jogo.", [UI_MENU_MENU_SOUNDS] = "Sons do menu", [UI_MENU_MENU_SOUNDS_DESC] = "Volume dos sons deste menu.",

    [UI_MENU_MOTION] = "Controles de movimento", [UI_MENU_MOTION_DESC] = "Mira com o giroscópio do controle.",
    [UI_MENU_RUMBLE_STRENGTH] = "Intensidade da vibração",
    [UI_MENU_RUMBLE_STRENGTH_DESC] = "Intensidade da vibração.",
    [UI_MENU_RUMBLE_CURVE] = "Curva da vibração",
    [UI_MENU_RUMBLE_CURVE_DESC] = "Valores menores reforçam a vibração fraca.",
    [UI_MENU_RUMBLE_TRIGGERS] = "Vibração dos gatilhos (Xbox)",
    [UI_MENU_RUMBLE_TRIGGERS_DESC] = "Também vibra os gatilhos do controle Xbox.",
    [UI_MENU_RUMBLE_HD] = "Vibração HD (Switch)",
    [UI_MENU_RUMBLE_HD_DESC] = "Envia a vibração detalhada do jogo a um Pro Controller do Switch ou Joy-Con como ela "
                               "é. A intensidade da vibração continua valendo: o valor padrão é a do jogo.",
    [UI_MENU_RUMBLE_HD_NEEDS] = "Requer um Pro Controller do Switch ou Joy-Con",
    [UI_MENU_GROUP_GYRO] = "Giroscópio",
    [UI_MENU_GYRO_CALIBRATION] = "Calibração do giroscópio",
    [UI_MENU_GYRO_CALIBRATION_DESC] = "Aprende a leitura do giroscópio em repouso sempre que o controle fica parado "
                                      "por um momento e a desconta, para a mira não derivar. Deixe o controle "
                                      "parado por um segundo depois de conectá-lo.",
    [UI_MENU_GYRO_DEADZONE] = "Zona morta do giroscópio",
    [UI_MENU_GYRO_DEADZONE_DESC] = "Giros mais lentos que o nível são ignorados; os níveis são os do console. "
                                   "Baixa firma um controle parado e mantém a mira lenta; Padrão e Alta firmam um "
                                   "controle que treme, mas engolem os giros mais lentos. Desligada deixa passar "
                                   "todas as leituras.",
    [UI_MENU_GYRO_DEADZONE_OFF] = "Desligada", [UI_MENU_GYRO_DEADZONE_LOW] = "Baixa",
    [UI_MENU_GYRO_DEADZONE_STANDARD] = "Padrão", [UI_MENU_GYRO_DEADZONE_HIGH] = "Alta",
    [UI_MENU_GYRO_TURN_SENS] = "Sensibilidade horizontal",
    [UI_MENU_GYRO_TURN_SENS_DESC] = "Quanto a mira se move para a esquerda e a direita ao girar o controle. 100 % passa "
                                    "o giro sem mudança.",
    [UI_MENU_GYRO_TURN_INVERT] = "Inverter horizontal",
    [UI_MENU_GYRO_TURN_INVERT_DESC] = "Inverte o movimento horizontal do giroscópio.",
    [UI_MENU_GYRO_TILT_SENS] = "Sensibilidade vertical",
    [UI_MENU_GYRO_TILT_SENS_DESC] = "Quanto a mira se move para cima e para baixo ao inclinar o controle. 100 % passa "
                                    "a inclinação sem mudança.",
    [UI_MENU_GYRO_TILT_INVERT] = "Inverter vertical",
    [UI_MENU_GYRO_TILT_INVERT_DESC] = "Inverte o movimento vertical do giroscópio.",
    [UI_MENU_GYRO_NEEDS_MOTION] = "Requer os controles de movimento ativados",
    [UI_MENU_PAD_LAYOUT] = "Layout dos botões",
    [UI_MENU_PAD_LAYOUT_DESC] = "Define qual botão funciona como A. Nintendo: pela posição, A à direita, B embaixo, "
                                "X em cima, Y à esquerda. Xbox e PlayStation: pela letra, A embaixo, B à direita, X à "
                                "esquerda, Y em cima (num controle PlayStation xis, círculo, quadrado e triângulo). "
                                "Automático: Nintendo para um controle com as letras da Nintendo, pela letra para os "
                                "outros.",
    [UI_MENU_PAD_LAYOUT_AUTO] = "Automático", [UI_MENU_PAD_LAYOUT_NINTENDO] = "Nintendo (A à direita)",
    [UI_MENU_PAD_LAYOUT_XBOX] = "Xbox (A embaixo)", [UI_MENU_PAD_LAYOUT_PLAYSTATION] = "PlayStation (A embaixo)",
    [UI_MENU_PAD_LAYOUT_IN_USE] = "em uso: %s",
    [UI_MENU_GROUP_STICKS] = "Analógicos",
    [UI_MENU_LSTICK_DEADZONE] = "Zona morta do analógico esquerdo",
    [UI_MENU_RSTICK_DEADZONE] = "Zona morta do analógico direito",
    [UI_MENU_STICK_DEADZONE_DESC] = "Quanto o analógico precisa mover para responder. Aumente se houver drift.",
    [UI_MENU_STICK_DEADZONE_NOW] = "analógico agora: %.0f %%",
    [UI_MENU_MOUSE_SENS] = "Sensibilidade da câmera no mouse",
    [UI_MENU_MOUSE_SENS_DESC] = "Quão rápido o mouse gira a câmera.",

    [UI_BINDS_HELP] = "Selecione uma célula e aperte A ou Enter, depois a tecla, o botão do mouse ou o botão do "
                      "controle. Delete ou X limpa a célula. Uma tecla já em uso vem para cá; um botão do controle "
                      "troca de lugar com o que ele substitui.",
    [UI_BINDS_COL_KEY1] = "Tecla 1", [UI_BINDS_COL_KEY2] = "Tecla 2", [UI_BINDS_COL_PAD] = "Controle",
    [UI_BINDS_GROUP_BUTTONS] = "Botões", [UI_BINDS_GROUP_MOVE] = "Movimento", [UI_BINDS_GROUP_CAMERA] = "Câmera",
    [UI_BINDS_PRESS_KEY] = "Aperte uma tecla ou um botão do mouse para %s",
    [UI_BINDS_PRESS_PAD] = "Aperte um botão do controle para %s",
    [UI_BINDS_CANCEL_HINT] = "Esc cancela, ou aguarde %d s",
    [UI_BINDS_TAKEN] = "%s saiu de %s", [UI_BINDS_SWAPPED] = "%s trocou de lugar com %s",
    [UI_BINDS_RESET] = "Restaurar todos os botões",
    [UI_BINDS_RESET_DESC] = "Restaura as teclas e os botões padrão.",
    [UI_BIND_A] = "A", [UI_BIND_B] = "B", [UI_BIND_X] = "X", [UI_BIND_Y] = "Y",
    [UI_BIND_LCLICK] = "Clique do analógico esquerdo", [UI_BIND_RCLICK] = "Clique do analógico direito",
    [UI_BIND_L] = "L", [UI_BIND_R] = "R", [UI_BIND_ZL] = "ZL", [UI_BIND_ZR] = "ZR", [UI_BIND_PLUS] = "Mais",
    [UI_BIND_MINUS] = "Menos",
    [UI_BIND_LEFT] = "Direcional para a esquerda", [UI_BIND_UP] = "Direcional para cima",
    [UI_BIND_RIGHT] = "Direcional para a direita", [UI_BIND_DOWN] = "Direcional para baixo",
    [UI_BIND_LSTICK_LEFT] = "Mover para a esquerda", [UI_BIND_LSTICK_UP] = "Mover para a frente",
    [UI_BIND_LSTICK_RIGHT] = "Mover para a direita", [UI_BIND_LSTICK_DOWN] = "Mover para trás",
    [UI_BIND_RSTICK_LEFT] = "Câmera para a esquerda", [UI_BIND_RSTICK_UP] = "Câmera para cima",
    [UI_BIND_RSTICK_RIGHT] = "Câmera para a direita", [UI_BIND_RSTICK_DOWN] = "Câmera para baixo",
    [UI_BIND_DESC_A] = "Ações: conversar, abrir, pegar, escalar, agarrar. Segure para correr.",
    [UI_BIND_DESC_B] = "Guardar, cancelar, voltar. Pular quando a troca do botão de pulo está ligada nas opções do jogo.",
    [UI_BIND_DESC_X] = "Pular. Guardar quando a troca do botão de pulo está ligada nas opções do jogo.",
    [UI_BIND_DESC_Y] = "Atacar com a espada.",
    [UI_BIND_DESC_LCLICK] = "Pressionar o analógico esquerdo (não aparece na tela de controles do jogo).",
    [UI_BIND_DESC_RCLICK] = "Voltar a câmera para trás do Link.",
    [UI_BIND_DESC_L] = "Seleção rápida de itens; a página anterior nos menus do jogo.",
    [UI_BIND_DESC_R] = "Seleção rápida de itens; a próxima página nos menus do jogo.",
    [UI_BIND_DESC_ZL] = "Travar alvo (segurar ou alternar: tipo de mira nas opções do jogo).",
    [UI_BIND_DESC_ZR] = "Usar o item escolhido: estilingue, arco, bumerangue, gancho, bombas, garrafas.",
    [UI_BIND_DESC_PLUS] = "Menu: itens, equipamento, coleção, sistema.", [UI_BIND_DESC_MINUS] = "Mapa.",
    [UI_BIND_DESC_LEFT] = "Tocar a ocarina.", [UI_BIND_DESC_UP] = "Pôr ou tirar o acessório.",
    [UI_BIND_DESC_RIGHT] = "Falar com a Navi.", [UI_BIND_DESC_DOWN] = "Trocar de botas.",
    [UI_BIND_DESC_MOVE] = "Move o Link.",
    [UI_BIND_DESC_CAMERA] = "Gira a câmera.",
    [UI_BIND_SHAKE] = "Sacudir o analógico da câmera",
    [UI_BIND_DESC_SHAKE] = "Balança o analógico direito. Segure para continuar.",
    [UI_BIND_ACTION_A] = "Ações / correr", [UI_BIND_ACTION_B] = "Guardar / voltar", [UI_BIND_ACTION_X] = "Pular",
    [UI_BIND_ACTION_Y] = "Ataque de espada", [UI_BIND_ACTION_LCLICK] = "—", [UI_BIND_ACTION_RCLICK] = "Recentrar câmera",
    [UI_BIND_ACTION_L] = "Itens / pág. ant.", [UI_BIND_ACTION_R] = "Itens / pág. seg.",
    [UI_BIND_ACTION_ZL] = "Travar alvo", [UI_BIND_ACTION_ZR] = "Usar item", [UI_BIND_ACTION_PLUS] = "Menu de pausa",
    [UI_BIND_ACTION_MINUS] = "Mapa", [UI_BIND_ACTION_LEFT] = "Ocarina", [UI_BIND_ACTION_UP] = "Acessório",
    [UI_BIND_ACTION_RIGHT] = "Falar com Navi", [UI_BIND_ACTION_DOWN] = "Trocar botas", [UI_BIND_ACTION_MOVE] = "Mover",
    [UI_BIND_ACTION_CAMERA] = "Câmera",
    [UI_KEY_SPACE] = "Espaço", [UI_KEY_TAB] = "Tab", [UI_KEY_LSHIFT] = "Shift esquerdo",
    [UI_KEY_LCTRL] = "Ctrl esquerdo",
    [UI_KEY_UP] = "Seta para cima", [UI_KEY_DOWN] = "Seta para baixo", [UI_KEY_LEFT] = "Seta para a esquerda",
    [UI_KEY_RIGHT] = "Seta para a direita",
    [UI_KEY_MOUSE_L] = "Mouse esquerdo", [UI_KEY_MOUSE_R] = "Mouse direito", [UI_KEY_MOUSE_M] = "Mouse do meio",
    [UI_KEY_MOUSE_X1] = "Mouse 4", [UI_KEY_MOUSE_X2] = "Mouse 5",
    [UI_KEY_ENTER] = "Enter", [UI_KEY_BACKSPACE] = "Backspace", [UI_KEY_RSHIFT] = "Shift direito",
    [UI_KEY_RCTRL] = "Ctrl direito", [UI_KEY_LALT] = "Alt", [UI_KEY_CAPSLOCK] = "Caps Lock", [UI_KEY_INSERT] = "Insert",
    [UI_KEY_DELETE] = "Delete", [UI_KEY_HOME] = "Home", [UI_KEY_END] = "End", [UI_KEY_PAGEUP] = "Page Up",
    [UI_KEY_PAGEDOWN] = "Page Down", [UI_KEY_NUM_FMT] = "Num %s",
    [UI_PAD_SOUTH] = "Botão de baixo", [UI_PAD_EAST] = "Botão da direita", [UI_PAD_WEST] = "Botão da esquerda",
    [UI_PAD_NORTH] = "Botão de cima", [UI_PAD_LSTICK] = "Analógico esquerdo", [UI_PAD_RSTICK] = "Analógico direito",
    [UI_PAD_L] = "Bumper esquerdo", [UI_PAD_R] = "Bumper direito", [UI_PAD_START] = "Start", [UI_PAD_BACK] = "Back",
    [UI_PAD_DPAD_LEFT] = "Direcional para a esquerda", [UI_PAD_DPAD_UP] = "Direcional para cima",
    [UI_PAD_DPAD_RIGHT] = "Direcional para a direita", [UI_PAD_DPAD_DOWN] = "Direcional para baixo",
    [UI_PAD_ZL] = "Gatilho esquerdo", [UI_PAD_ZR] = "Gatilho direito",
    [UI_PAD_LPADDLE1] = "Botão traseiro esquerdo", [UI_PAD_RPADDLE1] = "Botão traseiro direito",
    [UI_PAD_LPADDLE2] = "Botão traseiro esquerdo 2", [UI_PAD_RPADDLE2] = "Botão traseiro direito 2",
    [UI_PAD_CROSS] = "Cruz", [UI_PAD_CIRCLE] = "Círculo", [UI_PAD_SQUARE] = "Quadrado", [UI_PAD_TRIANGLE] = "Triângulo",

    [UI_MENU_AMIIBO_DIR] = "Pasta de amiibo",
    [UI_MENU_AMIIBO_OPEN] = "Abrir",
    [UI_MENU_AMIIBO_DEFAULT_DIR] = "Pasta padrão",
    [UI_MENU_AMIIBO_REFRESH] = "Atualizar",
    [UI_MENU_AMIIBO_NO_DIR] = "Não foi possível abrir a pasta %s",
    [UI_MENU_AMIIBO_NONE_FOUND] = "Nenhum dump de amiibo (arquivos .bin ou .nfc de 540 bytes) em %s",
    [UI_MENU_AMIIBO_FILTER] = "Buscar",
    [UI_MENU_AMIIBO_LIST_HINT] = "A ou um clique encosta o amiibo no leitor.",
    [UI_MENU_AMIIBO_ADVANCED] = "Avançado",
    [UI_MENU_AMIIBO_READER] = "Leitor NFC",
    [UI_MENU_AMIIBO_STATE] = "Leitor:",
    [UI_MENU_AMIIBO_STATE_IDLE] = "pronto",
    [UI_MENU_AMIIBO_STATE_SEARCHING] = "o jogo está esperando um amiibo",
    [UI_MENU_AMIIBO_STATE_FOUND] = "amiibo no leitor",
    [UI_MENU_AMIIBO_STATE_MOUNTED] = "o jogo está lendo o amiibo",
    [UI_MENU_AMIIBO_STATE_OFF] = "desligado",
    [UI_MENU_AMIIBO_REMOVE] = "Afastar o amiibo",
    [UI_MENU_AMIIBO_AUTO_REMOVE] = "Afastar o amiibo sozinho depois de encostar",
    [UI_MENU_AMIIBO_HOLD] = "Tempo no leitor",
    [UI_MENU_AMIIBO_SECONDS] = "%.1f s",
    [UI_MENU_AMIIBO_NO_TAG] = "Nenhum amiibo no leitor.",
    [UI_MENU_AMIIBO_TAG_FILE] = "Arquivo: %s",
    [UI_MENU_AMIIBO_TAG_ID] = "Personagem %s, série %02x, número %04x, tipo %02x",
    [UI_MENU_AMIIBO_TAG_UID] = "UID: %s",
    [UI_MENU_AMIIBO_READY] = "Pronto",
    [UI_MENU_AMIIBO_TAPPED] = "Encostado: %s",
    [UI_MENU_AMIIBO_LOAD_FAILED] = "Não é um dump de amiibo (NTAG215, 540 bytes): %s",
    [UI_MENU_AMIIBO_REMOVED] = "Amiibo afastado",
    [UI_MENU_AMIIBO_NICKNAME] = "Apelido: %s",
    [UI_MENU_AMIIBO_NOT_REGISTERED] = "Sem dono nem apelido registrados",
    [UI_MENU_AMIIBO_SOURCE] = "Dados da figura: %s",
    [UI_MENU_AMIIBO_SOURCE_NEW] = "novos, nada salvo ainda",
    [UI_MENU_AMIIBO_SOURCE_SAVED] = "salvos pelo port",
    [UI_MENU_AMIIBO_SOURCE_DUMP] = "lidos do dump",
    [UI_MENU_AMIIBO_DECRYPTED] = "Dump descriptografado com as chaves",
    [UI_MENU_AMIIBO_NOT_DECRYPTED] = "Dump não descriptografado",
    [UI_MENU_AMIIBO_WRITES] = "Gravações: %u, a última em %04u-%02u-%02u",
    [UI_MENU_AMIIBO_GAME_DATA] = "Dados do jogo: %u bytes para o ID de acesso %08X",
    [UI_MENU_AMIIBO_NO_GAME_DATA] = "Dados do jogo: nenhum",
    [UI_MENU_AMIIBO_HAS_GAME_DATA] = "Dados do jogo: salvos",
    [UI_MENU_AMIIBO_ERASE] = "Apagar dados do jogo",
    [UI_MENU_AMIIBO_ERASE_CONFIRM] = "Aperte de novo para apagar",
    [UI_MENU_AMIIBO_ERASED] = "Dados do jogo apagados",
    [UI_MENU_AMIIBO_ERASE_FAILED] = "Não foi possível apagar os dados do jogo",
    [UI_MENU_AMIIBO_KEYS] = "Chaves:",
    [UI_MENU_AMIIBO_KEYS_LOADED] = "carregadas",
    [UI_MENU_AMIIBO_KEYS_MISSING] = "não carregadas",
    [UI_MENU_AMIIBO_KEYS_FILE] = "Arquivo de chaves: %s",
    [UI_MENU_AMIIBO_RELOAD_KEYS] = "Recarregar chaves",
    [UI_MENU_AMIIBO_KEYS_RELOADED] = "Chaves carregadas: %s",
    [UI_MENU_AMIIBO_KEYS_FAILED] = "Nenhuma chave carregada (arquivo ausente ou não é um arquivo de chaves de 160 bytes): %s",
    [UI_MENU_AMIIBO_WRITEBACK] = "Gravar as alterações do jogo no arquivo do dump",
    [UI_MENU_AMIIBO_WRITEBACK_NEEDS_KEYS] = "precisa das chaves",
    [UI_MENU_AMIIBO_POPUP] = "Mostrar a lista de amiibos quando o jogo pedir um amiibo",
    [UI_MENU_AMIIBO_POPUP_TITLE] = "O jogo está esperando um amiibo",
    [UI_MENU_AMIIBO_POPUP_HINT] = "A: encostar   B: fechar",
    [UI_MENU_AMIIBO_DAILY_LIMIT] = "Limite diário dos amiibos",
    [UI_MENU_AMIIBO_DAILY_LIMIT_DESC] = "Ligado, como no jogo: um amiibo dá itens uma vez por dia, e depois disso o "
                                        "jogo diz \"Não é possível usar este amiibo novamente hoje\". Desligado: ele "
                                        "dá itens toda vez que é encostado. Os dois amiibos que dão máscara não têm "
                                        "limite de nenhum jeito.",
    [UI_MENU_DISCORD_RPC] = "Discord Rich Presence",
    [UI_MENU_DISCORD_RPC_DESC] = "Envia uma atividade e o tempo de jogo ao Discord. Primeiro informe seu próprio "
                                 "discord_app_id no nx2saria.ini ou NX2SARIA_DISCORD_APP_ID; o Discord mostra o nome "
                                 "registrado para esse ID.",

    [UI_MENU_RESET_NO] = "Cancelar",

    [UI_MENU_OV_FPS] = "%.0f FPS", [UI_MENU_OV_FRAME_MS] = "%.1f ms", [UI_MENU_OV_GAME_PCT] = "jogo %.0f%%",
    [UI_MENU_OV_MAX_MS] = "máx. %.1f ms",
    [UI_MENU_OV_FPS_LABEL] = "FPS", [UI_MENU_OV_FRAME_LABEL] = "Quadro", [UI_MENU_OV_FMT_FPS] = "%.0f",
    [UI_MENU_OV_LOW1] = "1%% piores %.0f", [UI_MENU_OV_LOW01] = "0.1%% piores %.0f",
    [UI_MENU_OV_GPU] = "GPU", [UI_MENU_OV_CPU] = "CPU", [UI_MENU_OV_RAM] = "RAM", [UI_MENU_OV_VRAM] = "VRAM",
    [UI_MENU_OV_NONE] = "-",
    [UI_MENU_OV_REPLAY] = "Preparo de quadro da GPU", [UI_MENU_OV_GPU_TIME] = "GPU ocupada por quadro",
    [UI_MENU_OV_PIPE_WAIT] = "Esperas de pipeline",
    [UI_MENU_OV_SHADER_WAIT] = "Esperas de shader", [UI_MENU_OV_TEX_UPLOADS] = "Envios de textura",
    [UI_MENU_OV_FMT_PCT] = "%.0f%%", [UI_MENU_OV_FMT_TEMP] = "%.0f °C", [UI_MENU_OV_FMT_POWER] = "%.0f W",
    [UI_MENU_OV_FMT_MHZ] = "%.0f MHz", [UI_MENU_OV_FMT_GB] = "%.1f GB", [UI_MENU_OV_FMT_GB_OF] = "/ %.1f GB",
    [UI_MENU_OV_GAME_GB] = "jogo %.1f GB", [UI_MENU_OV_FMT_MS] = "%.2f ms", [UI_MENU_OV_PER_FRAME] = "%.1f por quadro",
    [UI_MENU_OV_WINDOW] = "Janela", [UI_MENU_OV_OUTPUT] = "Imagem do jogo", [UI_MENU_OV_SCENE] = "Cena 3D",
    [UI_MENU_OV_LIMIT] = "Limite de quadros",
    [UI_MENU_OV_FMT_SIZE] = "%ux%u", [UI_MENU_OV_FMT_SCENE] = "%ux%u, %.0f%%",
    [UI_MENU_OV_FMT_FSR] = "AMD FSR 1, nitidez %d%%",
    [UI_MENU_FOV] = "Campo de visão",
    [UI_MENU_FOV_DESC] = "Quanto do mundo a câmera mostra, em porcentagem do original do jogo.",
    [UI_MENU_ANISO] = "Filtragem de texturas",
    [UI_MENU_ANISO_DESC] = "Filtragem anisotrópica das texturas de superfície: chão, estradas e paredes vistos em "
                           "ângulo raso continuam nítidos ao longe em vez de borrar. Jogo: o nível que o jogo pede, "
                           "como no Switch.",
    [UI_MENU_ANISO_GAME] = "Jogo",
    [UI_MENU_TEX_DETAIL] = "Detalhe das texturas",
    [UI_MENU_TEX_DETAIL_DESC] = "Nitidez das texturas grandes de superfície. Reduzido e Baixo deixam de fora as versões "
                                "maiores delas, para que uma área caiba na memória de vídeo de placas de 6 e 8 GB; só as "
                                "superfícies vistas de perto ficam mais suaves. Automático escolhe pela memória de vídeo "
                                "da placa.",
    [UI_MENU_TEX_DETAIL_AUTO] = "Automático: %s",
    [UI_MENU_TEX_DETAIL_FULL] = "Completo", [UI_MENU_TEX_DETAIL_REDUCED] = "Reduzido", [UI_MENU_TEX_DETAIL_LOW] = "Baixo",
    [UI_MENU_HDR] = "Saída HDR",
    [UI_MENU_HDR_DESC] = "Realces mais brilhantes em telas HDR. Ative o HDR no Windows antes.",
    [UI_MENU_HDR_PEAK] = "Brilho máximo",
    [UI_MENU_HDR_PEAK_DESC] = "Brilho máximo da sua tela em nits.",
    [UI_MENU_HDR_GAME] = "Brilho do jogo",
    [UI_MENU_HDR_GAME_DESC] = "Brilho do branco no jogo, em nits.",
    [UI_MENU_HDR_UI] = "Brilho do HUD e do menu",
    [UI_MENU_HDR_UI_DESC] = "Brilho da interface e deste menu, em nits.",
    [UI_MENU_HDR_HUE] = "Correção de matiz",
    [UI_MENU_HDR_HUE_DESC] = "Quanto as cores brilhantes mantêm o matiz que tinham antes da curva de tons; 0 deixa que "
                             "mudem com a curva.",
    [UI_MENU_HDR_GAMMA] = "Correção de gama",
    [UI_MENU_HDR_GAMMA_DESC] = "Mostra a imagem como uma tela de gama 2.2 ou BT.1886 (2.4) mostraria, como o jogo foi "
                               "feito para ser visto; Desligado mantém a curva sRGB, com sombras mais claras.",
    [UI_MENU_HDR_TONEMAP] = "Curva de tons",
    [UI_MENU_HDR_TONEMAP_DESC] = "Vanilla: a curva do próprio jogo, com brilho SDR. PsychoV35: a cena por um modelo da "
                                 "resposta do olho, ajustada ao cinza médio e ao tom do jogo, até o pico da sua tela. "
                                 "RenoDRT: o visual SDR do jogo levado até o pico pelo brilho.",
    [UI_MENU_HDR_EXPOSURE] = "Exposição",
    [UI_MENU_HDR_EXPOSURE_DESC] = "Brilho de toda a imagem antes da curva de tons; 100 % a deixa como está.",
    [UI_MENU_HDR_HIGHLIGHTS] = "Realces",
    [UI_MENU_HDR_HIGHLIGHTS_DESC] = "Brilho das partes claras; 50 as deixa como estão.",
    [UI_MENU_HDR_SHADOWS] = "Sombras",
    [UI_MENU_HDR_SHADOWS_DESC] = "Brilho das partes escuras; 50 as deixa como estão.",
    [UI_MENU_HDR_CONTRAST] = "Contraste",
    [UI_MENU_HDR_CONTRAST_DESC] = "Contraste em torno do cinza médio; 50 o deixa como está.",
    [UI_MENU_HDR_SATURATION] = "Saturação",
    [UI_MENU_HDR_SATURATION_DESC] = "Força de todas as cores; 50 a deixa como está.",
    [UI_MENU_HDR_HIGHLIGHT_SAT] = "Saturação dos realces",
    [UI_MENU_HDR_HIGHLIGHT_SAT_DESC] = "Força das cores das partes mais claras; 50 a deixa como está.",
    [UI_MENU_HDR_BLOWOUT] = "Estouro",
    [UI_MENU_HDR_BLOWOUT_DESC] = "Leva as partes mais claras para o branco; 0 mantém a cor delas.",
    [UI_MENU_HDR_FLARE] = "Flare",
    [UI_MENU_HDR_FLARE_DESC] = "Ergue as partes mais escuras como um pouco de luz difusa faria; 0 mantém o preto preto.",
    [UI_MENU_HDR_GRADE] = "Gradação de cor",
    [UI_MENU_HDR_GRADE_DESC] = "Quanto da gradação de cor do próprio jogo a imagem HDR mantém; 0 é a cena sem ela.",
    [UI_MENU_HDR_MIDGRAY] = "Cinza médio",
    [UI_MENU_HDR_MIDGRAY_DESC] = "Onde fica o cinza médio da cena. Vanilla: onde a curva do próprio jogo o coloca. "
                                 "Render: no valor da cena, um pouco mais escuro.",
    [UI_MENU_HDR_MATCH_SLOPE] = "Igualar inclinação do tom",
    [UI_MENU_HDR_MATCH_SLOPE_DESC] = "Dá aos tons médios o contraste da curva do próprio jogo.",
    [UI_MENU_HDR_TONE_SLOPE] = "Inclinação do tom",
    [UI_MENU_HDR_TONE_SLOPE_DESC] = "Inclinação da curva de tons no cinza médio; 50 a deixa como está.",
    [UI_MENU_HDR_HUE_CLIP] = "Emulação do corte de matiz",
    [UI_MENU_HDR_HUE_CLIP_DESC] = "Quanto as cores brilhantes viram como onde a imagem SDR do jogo estoura: o fogo "
                                  "para o amarelo e o branco. 0 mantém o matiz delas.",
    [UI_MENU_PROMPTS] = "Ícones de botões",
    [UI_MENU_PROMPTS_DESC] = "Quais botões os ícones do jogo mostram: no texto, no ícone de ação e nas notas da "
                             "ocarina. Nintendo: os do próprio jogo. Xbox e PlayStation: os botões desse controle (LB, "
                             "RB, LT, RT, Menu, Exibir; L1, R1, L2, R2, Options, Create), e A, B, X e Y aparecem como "
                             "o botão que o layout acima aperta (com Nintendo (A à direita) o botão da direita faz A, "
                             "então os ícones de A mostram B, ou círculo). Teclado e mouse: a tecla de cada botão (E, "
                             "Ctrl, Espaço, clique esquerdo...), WASD para andar e o mouse para a câmera. O launcher "
                             "desenha os arquivos dos ícones quando monta o jogo, com as teclas como o nx2saria.ini as "
                             "define nessa hora; se eles faltarem, atualize o jogo ou monte-o de novo (Rebuild the Game "
                             "no launcher do Windows, Update no AppImage do Linux). O jogo lê os ícones ao iniciar, "
                             "então eles mudam depois de reiniciar. Eles fazem parte dos arquivos do jogo, então um "
                             "botão remapeado em Controles continua com o ícone antigo na tela até o jogo ser montado "
                             "de novo.",
    [UI_MENU_PROMPTS_NINTENDO] = "Nintendo",
    [UI_MENU_PROMPTS_XBOX] = "Xbox",
    [UI_MENU_PROMPTS_PLAYSTATION] = "PlayStation",
    [UI_MENU_PROMPTS_PC] = "Teclado e mouse",
    [UI_MENU_MOUSE_MODE] = "Câmera com o mouse",
    [UI_MENU_MOUSE_MODE_DESC] = "Direta: a câmera gira o quanto o mouse se move, como num jogo de PC, sem zona morta nem curva do analógico (jogos 1.1.1 e 1.4.3; as outras versões usam a emulação do analógico). Emulação do analógico: o mouse age como o analógico direito.",
    [UI_MENU_MOUSE_STICK] = "Emulação do analógico",
    [UI_MENU_MOUSE_DIRECT] = "Direta",
    [UI_MENU_MENU_CURSOR] = "Mouse nos menus do jogo",
    [UI_MENU_MENU_CURSOR_DESC] = "Nos menus do próprio jogo (título e arquivos de save, menu de pausa, opções, lojas, escolhas de diálogo) o ponteiro aparece: o item sob ele fica selecionado, o clique esquerdo confirma, o direito volta e a roda rola uma lista (no menu de pausa, vira a página). Só depois de usar o mouse ou uma tecla por último, então com um controle os menus ficam como antes (jogos 1.1.1 e 1.4.3).",
    [UI_MENU_PROMPTS_MISSING] = "faltam os arquivos dos ícones: atualize o jogo ou monte-o de novo no launcher",
    [UI_MENU_PROMPTS_FAILED] = "arquivos dos ícones danificados: monte o jogo de novo no launcher",
    [UI_MENU_PROMPTS_RESTART] = "os ícones mudam após reiniciar",
    [UI_MENU_PROMPTS_OFF_SHAPE] = "desligados numa resolução de jogo que não é 16:9",
    [UI_MENU_PROMPTS_OFF_MODS] = "desligados: um mod de RomFS substitui os arquivos deles",
    [UI_MENU_PROMPTS_OFF_PATCHES] = "desligados enquanto NX2SARIA_ROMFS_PATCHES está definido",
    [UI_MENU_RESTART_CONFIRM] = "Reiniciar o jogo agora? O progresso desde o último salvamento é perdido.",
    [UI_MENU_QUIT_CONFIRM] = "Sair do jogo? O progresso desde o último salvamento é perdido.",
    [UI_MENU_RESTART_YES] = "Reiniciar",
    [UI_MENU_RESTART_COUNT] = "Reiniciar (%d)",
    [UI_MENU_RESTART_SAVING] = "O jogo está salvando: dá para reiniciar em um instante.",
    [UI_MENU_QUIT_SAVING] = "O jogo está salvando: dá para sair em um instante.",
    [UI_MENU_DRAFT_TITLE] = "Aplicar as alterações?",
    [UI_MENU_DRAFT_APPLY] = "Aplicar",
    [UI_MENU_DRAFT_DISCARD] = "Descartar",
    [UI_MENU_DRAFT_KEEP] = "Continuar editando",
    [UI_MENU_DRAFT_RESTART_NOTE] = "As alterações feitas são aplicadas e salvas antes.",
    [UI_MENU_DRAFT_QUIT_NOTE] = "As alterações não aplicadas são perdidas.",
    [UI_MENU_KEEP_TITLE] = "Manter estas configurações de tela?",
    [UI_MENU_KEEP_COUNTDOWN] = "Revertendo em %d s",
    [UI_MENU_KEEP_YES] = "Manter",
    [UI_MENU_KEEP_REVERT] = "Reverter",
    [UI_MENU_RESTART_NOW_TITLE] = "Algumas alterações só valem depois de reiniciar",
    [UI_MENU_RESTART_NOW] = "Reiniciar agora",
    [UI_MENU_RESTART_LATER] = "Depois",
    [UI_MENU_RELOAD_NONE] = "nx2saria.ini recarregado: sem alterações",
    [UI_MENU_RELOAD_DONE] = "nx2saria.ini recarregado: %d alterada(s), %d após reiniciar",
    [UI_MENU_RELOAD_REJECTED] = "nx2saria.ini recarregado: %d alterada(s), %d após reiniciar, %d rejeitada(s)",
    [UI_MENU_RELOAD_FAILED] = "Não foi possível ler o nx2saria.ini",
    [UI_MENU_RELOAD_BUSY] = "Feche o menu de configurações para recarregar o nx2saria.ini",
    [UI_MENU_STICK_SENS] = "Velocidade da câmera (analógico)",
    [UI_MENU_STICK_SENS_DESC] = "Velocidade da câmera no analógico direito.",
    [UI_MENU_WALK_KEY] = "Tecla de andar",
    [UI_MENU_WALK_KEY_DESC] = "Segure para andar no teclado.",
    [UI_MENU_WALK_KEY_TAKEN] = "também %s",
    [UI_MENU_WALK_SCALE] = "Intensidade do andar",
    [UI_MENU_WALK_SCALE_DESC] = "A inclinação dos analógicos do teclado enquanto a tecla de andar está segura. Metade "
                                "é andar, a inclinação total é correr.",
    [UI_MENU_DLSS_DLAA] = "DLAA (nativo)",
    [UI_MENU_DLSS_QUALITY] = "Qualidade",
    [UI_MENU_DLSS_BALANCED] = "Equilibrado",
    [UI_MENU_DLSS_PERFORMANCE] = "Desempenho", [UI_MENU_DLSS_ULTRA] = "Ultra Desempenho",
    [UI_MENU_AA] = "Antisserrilhado",
    [UI_MENU_AA_DESC] = "Como as bordas da imagem 3D são suavizadas. SMAA do jogo é o próprio, como no console. Desligado "
                        "deixa as bordas como desenhadas. FXAA é rápido e um pouco suave. SMAA encontra mais bordas que o do "
                        "jogo e mantém os detalhes nítidos. TAA é o DLSS ou o FSR em tamanho cheio: as bordas mais "
                        "estáveis em movimento; define o escalonamento. Com DLSS ou FSR em Upscaling, o TAA deles "
                        "sempre roda, e a escolha daqui roda junto. Vale na hora.",
    [UI_MENU_AA_GAME] = "SMAA do jogo",
    [UI_MENU_AA_FXAA] = "FXAA",
    [UI_MENU_AA_SMAA] = "SMAA (Ultra)",
    [UI_MENU_AA_TAA_DLSS] = "TAA (NVIDIA DLAA)",
    [UI_MENU_AA_TAA_FSR] = "TAA (AMD FSR 3.1)",
    [UI_MENU_AA_TAA_FSR4] = "TAA (%s)",
    [UI_MENU_AA_WITH_TAA] = "mais o TAA do Upscaling",
    [UI_MENU_TAB_CHEATS] = "Cheats",
    [UI_MENU_CHEATS_NONE] = "Nenhum cheat para esta versão do jogo. Os cheats de um mod são um arquivo de cheats do "
                            "Atmosphere na pasta cheats dele, com o nome da build do jogo: mods\\<mod>\\cheats\\%s. "
                            "Reinicie o jogo para lê-los. Para cheats que alteram o código do jogo, o jogo se recompila "
                            "sozinho ao iniciar (alguns minutos).",
    [UI_MENU_CHEATS_MOD] = "%s: %s",
    [UI_MENU_CHEAT_CODE_DESC] = "Altera o código do jogo. Liga e desliga na hora.",
    [UI_MENU_CHEAT_KEY_DESC] = "Altera o código do jogo enquanto você segura %s. Liga e desliga na hora.",
    [UI_MENU_CHEAT_DATA_DESC] = "Escreve na memória do jogo a cada quadro enquanto está ligado.",
    [UI_MENU_CHEAT_MASTER] = "sempre ligado",
    [UI_MENU_CHEAT_MASTER_DESC] = "O código mestre do arquivo: está sempre ligado.",
    [UI_MENU_CHEAT_RESTORE] = "desnecessário",
    [UI_MENU_CHEAT_RESTORE_DESC] = "Ele só escreve de volta o código original do jogo. Desligar um cheat já faz isso.",
    [UI_MENU_CHEAT_REBUILD] = "após recompilar",
    [UI_MENU_CHEAT_REBUILD_DESC] = "Este cheat altera o código do jogo e é novo ou foi editado depois que o jogo foi "
                                   "compilado. O Play no launcher o compila.",
    [UI_MENU_CHEAT_UNSUPPORTED] = "não suportado",
    [UI_MENU_CHEAT_UNSUPPORTED_DESC] = "Este cheat não pode ser usado com esta versão do jogo:",
    [UI_MENU_SAVES] = "Saves",
    [UI_MENU_SAVE_EXPORT] = "Exportar save",
    [UI_MENU_SAVE_EXPORT_DESC] = "Grava seu save (todos os slots) em um arquivo zip na pasta Downloads, para passar a "
                                 "outro jogador ou anexar a um relato de bug. Ele nunca contém suas chaves nem arquivos "
                                 "do jogo.",
    [UI_MENU_SAVE_EXPORT_BUG] = "Exportar para relato de bug",
    [UI_MENU_SAVE_EXPORT_BUG_DESC] = "O mesmo zip com o log desta execução e suas configurações (nx2saria.ini), que "
                                     "mostram o que aconteceu e com quais configurações. Anexe-o ao seu relato de bug.",
    [UI_MENU_SAVE_IMPORT] = "Importar save",
    [UI_MENU_SAVE_IMPORT_DESC] = "Escolha um save que alguém compartilhou (um zip NX2Saria-save). Ele substitui seu save, "
                                 "todos os slots, na próxima inicialização: o jogo mantém seu progresso na memória e o "
                                 "gravaria por cima de um save substituído agora. Seu save atual é copiado antes para a "
                                 "pasta saves.",
    [UI_MENU_SAVE_IMPORT_WAITING] = "na próxima inicialização",
    [UI_MENU_SAVE_IMPORT_CANCEL] = "Cancelar a importação",
    [UI_MENU_SAVE_IMPORT_CANCEL_DESC] = "Mantém seu save como está: o save que aguardava a próxima inicialização é "
                                        "descartado.",
    [UI_MENU_SAVE_BUSY] = "trabalhando…",
    [UI_MENU_SAVE_EXPORTED] = "Salvo como %s",
    [UI_MENU_SAVE_IMPORT_READY] = "O save (NX2Saria %s) é carregado na próxima inicialização: selecione Reiniciar abaixo. "
                                  "Seu save atual é copiado antes para a pasta saves.",
    [UI_MENU_SAVE_IMPORT_CANCELLED] = "Importação cancelada; seu save continua como estava.",
    [UI_MENU_SAVE_IMPORTED] = "Um save compartilhado foi importado nesta inicialização. Seu save anterior está em %s",
    [UI_MENU_SAVE_IMPORTED_NEW] = "Um save compartilhado foi importado nesta inicialização.",
    [UI_MENU_SAVE_E_NO_SAVE] = "Ainda não há save para exportar: jogue até o jogo salvar.",
    [UI_MENU_SAVE_E_READ_SAVE] = "Um arquivo do seu save não pôde ser lido; tente de novo daqui a pouco.",
    [UI_MENU_SAVE_E_WRITE] = "Não foi possível gravar o zip em %s.",
    [UI_MENU_SAVE_E_READ] = "Não foi possível ler %s.",
    [UI_MENU_SAVE_E_STAGE] = "Não foi possível preparar o save para a próxima inicialização; o log diz o motivo.",
    [UI_MENU_SAVE_E_NOT_SAVE] = "%s não é um save exportado pelo NX2Saria.",
    [UI_MENU_SAVE_E_OTHER_GAME] = "%s é um save de outro jogo.",
    [UI_MENU_SAVE_E_NEWER] = "%s foi feito por um NX2Saria mais novo: atualize o NX2Saria primeiro.",
    [UI_MENU_SAVE_E_DAMAGED] = "%s está danificado: baixe-o de novo.",
    [UI_MENU_SAVE_E_UNSAFE] = "%s foi recusado: contém arquivos fora de um save, links ou dados demais.",
    [UI_MENU_CAPTURE_SAVED] = "Captura de bug %d salva",
    [UI_MENU_CAPTURE_FOLDER] = "Pasta: %s",
    [UI_MENU_CAPTURE_FAILED] = "Não foi possível salvar a captura de bug %d",
    [UI_BOOT_TITLE] = "Preparando shaders",
    [UI_BOOT_STARTING] = "Iniciando", [UI_BOOT_SHADERS] = "Shaders: %lld de %lld", [UI_BOOT_PIPELINES] = "Pipelines: %lld de %lld",
    [UI_BOOT_COMPUTE] = "Pipelines de computação: %lld de %lld", [UI_BOOT_DRIVER_CACHE] = "Cache do driver: %lld de %lld",
    [UI_MENU_CHEATS_UNBUILT] = "%s e %s não estão compilados no jogo: o Play no launcher os compila.",
    [UI_MENU_COUNT_CODE_CHEATS] = "%d cheat de código|%d cheats de código",
    [UI_MENU_COUNT_EXEFS_FILES] = "%d arquivo de patch ExeFS|%d arquivos de patch ExeFS",
    [UI_MENU_COUNT_CHEATS] = "%d cheat|%d cheats",
    [UI_MENU_PRESET] = "Preset de qualidade",
    [UI_MENU_PRESET_DESC] = "Ajusta as opções gráficas de uma vez. Recomendado é o preset que este PC recebe pela placa de vídeo, "
        "memória e processador. Alto é o jogo como foi lançado; Baixo e Médio desenham menos, para PCs mais fracos e "
        "portáteis; Ultra desenha mais, e mais ainda num monitor maior; nunca é recomendado, só escolhido. As opções que você mudar "
        "à mão depois ficam como você deixou, e o preset passa a aparecer como personalizado.",
    [UI_MENU_PRESET_RECOMMENDED] = "Recomendado (%s)",
    [UI_MENU_PRESET_LOW] = "Baixo",
    [UI_MENU_PRESET_MEDIUM] = "Médio",
    [UI_MENU_PRESET_HIGH] = "Alto",
    [UI_MENU_PRESET_ULTRA] = "Ultra",
    [UI_MENU_PRESET_CUSTOM] = "%s (personalizado)",
    [UI_MENU_PRESET_CUSTOM_NOTE] = "mudadas à mão: %d",
    [UI_MENU_PRESET_REC_NOTE] = "Recomendado para este PC: %s",
    [UI_MENU_PRESET_HW_NOTE] = "Hardware novo: o preset recomendado está em uso",
    [UI_MENU_PRESET_NOTICE] = "O preset de qualidade Recomendado (%s) está em uso neste PC (Configurações > Tela). Para o jogo como antes, escolha Alto ali.",
    [UI_MENU_PRESET_CONFIRM] = "Estas opções foram mudadas à mão. O preset vai trocá-las pelos valores dele:",
    [UI_MENU_PRESET_CONFIRM_YES] = "Usar o preset",
    [UI_MENU_GAME_RES] = "Resolução",
    [UI_MENU_GAME_RES_NATIVE] = "Nativa",
    [UI_MENU_GAME_RES_DESC] = "Tamanho da imagem do jogo; fora da tela cheia, a janela também fica com ele. Maior é mais "
                              "nítido, mas exige mais da placa de vídeo.",
    [UI_MENU_SCENE_RES] = "Resolução da cena",
    [UI_MENU_SCENE_RES_DESC] = "Tamanho em que a cena 3D é desenhada, como parte da imagem. Menor desenha a cena menor e a amplia até "
        "a imagem: mais rápido, para PCs fracos e portáteis, mas menos nítido. O HUD e os menus continuam nítidos, na "
        "resolução cheia. 100 % é Nativo. As mudanças valem na hora.",
    [UI_MENU_SCENE_RES_GAME] = "%s (jogo)",
    [UI_MENU_SCENE_RES_NATIVE] = "%s (nativo)",
    [UI_MENU_SCENE_RES_PRESET] = "%s (preset)",
    [UI_MENU_DYN_RES] = "Resolução dinâmica",
    [UI_MENU_DYN_RES_DESC] = "Baixa a resolução da cena 3D enquanto a placa de vídeo não acompanha o Limite de quadros "
        "e volta a subi-la quando ela dá conta: um pouco de nitidez nas cenas pesadas por uma taxa de quadros mais "
        "estável. O HUD e os menus continuam nítidos. Com DLSS ou FSR, desce o modo do upscaler até Desempenho; sem "
        "eles, baixa a resolução da cena até o mínimo abaixo. As mudanças valem na hora.",
    [UI_MENU_DYN_RES_MIN] = "Resolução mínima da cena",
    [UI_MENU_DYN_RES_MIN_DESC] = "O menor tamanho a que a Resolução dinâmica leva a cena 3D, como parte da imagem. "
        "Menor mantém a taxa de quadros mais estável num PC fraco, mas as cenas pesadas ficam menos nítidas. Nunca "
        "passa da Resolução da cena escolhida. Com DLSS ou FSR isto não vale: o modo do upscaler desce até Desempenho "
        "no lugar. As mudanças valem na hora.",
    [UI_MENU_DYN_RES_MIN_OFF] = "só com a Resolução dinâmica ligada",
    [UI_MENU_DYN_RES_NO_ROOM] = "nada a baixar: a Resolução da cena já está no mínimo ou abaixo",
    [UI_MENU_DYN_RES_NATIVE] = "nada a baixar com DLAA ou Native AA",
    [UI_MENU_VRAM_WARN] = "A memória de vídeo acabou, e por isso o jogo está lento. Em Configurações > Tela:",
    [UI_MENU_VRAM_WARN_RES] = "- %s: escolha uma menor",
    [UI_MENU_VRAM_WARN_MODE] = "- %s: escolha um modo mais leve que %s",
    [UI_MENU_VRAM_WARN_FRAMEGEN] = "- %s: desligue",
    [UI_MENU_VRAM_WARN_PRESET] = "- %s: escolha um mais baixo",
    [UI_MENU_VRAM_FIT_MEASURED] = "A memória de vídeo já está em %d%% (%llu de %llu MiB). Usam mais memória de vídeo: %s.",
    [UI_MENU_VRAM_FIT_NOTICED] = "A memória de vídeo acabou mais cedo nesta sessão (agora em %d%%, %llu de %llu MiB). Usam mais memória de vídeo: %s.",
    [UI_MENU_VRAM_FIT_PRESET] = "Esta placa tem %u GiB de memória de vídeo. O preset recomendado para ela usa: %s.",
    [UI_MENU_VRAM_FIT_FULL] = "A memória de vídeo já está cheia (%llu de %llu MiB). Usam mais memória de vídeo: %s.",
};

static const char *const text_zh_hans[UI_TEXT_COUNT] = {
    SWKBD("文字输入", "请输入名字", "马儿", "确定", "取消", "手柄：A 确定，B 取消"),

    [UI_MENU_TITLE] = "设置",
    [UI_MENU_TAB_SCREEN] = "显示",
    [UI_MENU_TAB_GAME] = "游戏",
    [UI_MENU_TAB_SOUND] = "声音",
    [UI_MENU_TAB_CONTROLS] = "操作",
    [UI_MENU_TAB_AMIIBO] = "amiibo",
    [UI_MENU_CONTINUE] = "继续",
    [UI_MENU_QUIT] = "退出游戏",
    [UI_MENU_ON] = "开",
    [UI_MENU_OFF] = "关",
    [UI_MENU_AUTO] = "自动",
    [UI_MENU_DEFAULT_MARK] = "默认",
    [UI_MENU_AFTER_RESTART] = "重启后生效",
    [UI_MENU_SET_BY_ENV] = "由环境变量设定",
    [UI_MENU_SAVE_FAILED] = "无法保存设置",
    [UI_MENU_INI_UNKNOWN] = "nx2saria.ini：以下不是此版本的设置，已忽略：%s",
    [UI_MENU_VERSION] = "NX2Saria %s，游戏 %s",
    [UI_MENU_FULLSCREEN] = "全屏",
    [UI_MENU_FULLSCREEN_DESC] = "全屏运行。按 F11 切换。",
    [UI_MENU_WINDOW_SIZE] = "窗口大小",
    [UI_MENU_WINDOW_SIZE_DESC] = "非全屏时的窗口大小。",
    [UI_MENU_PRESENT_DESC] = "防止画面撕裂。关闭可略微降低延迟。",
    [UI_MENU_PRESENT_VSYNC] = "垂直同步",
    [UI_MENU_PRESENT_IMMEDIATE] = "立即",
    [UI_MENU_PRESENT_IN_USE] = "使用中：%s",
    [UI_MENU_LOW_LATENCY] = "低延迟",
    [UI_MENU_LOW_LATENCY_DESC] = "显卡驱动推迟每一帧的开始，尽可能晚地读取手柄输入（NVIDIA Reflex、AMD Anti-Lag），"
                                 "并由它保持帧率上限。增强让显卡保持最高频率。",
    [UI_MENU_LOW_LATENCY_BOOST] = "开启 + 增强",
    [UI_MENU_LOW_LATENCY_NO_DRIVER] = "此显卡驱动没有低延迟模式",
    [UI_MENU_LOW_LATENCY_DLSSG] = "DLSS 帧生成使用自带的低延迟",
    [UI_MENU_LOW_LATENCY_FG] = "此显卡运行帧生成时关闭",
    [UI_MENU_FRAMEGEN_DESC_ANY] = "添加生成的帧，使动作更流畅，可搭配 Upscaling 中的任意选项。",
    [UI_MENU_FRAMEGEN_DESC_NGX] = "添加生成的帧，使动作更流畅。NVIDIA DLSS 需要在游戏启动时于 Upscaling 中选择 DLSS "
                                  "模式；之后可搭配 Upscaling 中的任意选项。",
    [UI_MENU_FRAMEGEN_NOTE_NGX] = "需要在游戏启动时开启 DLSS",
    [UI_MENU_LOW_LATENCY_FAILED] = "使用中：关闭，驱动拒绝了（见日志中的 lowlat）",
    [UI_MENU_GPU] = "显卡",
    [UI_MENU_GPU_DESC] = "运行游戏所用的显卡。",
    [UI_MENU_GPU_AUTO] = "自动",
    [UI_MENU_PERF_OVERLAY] = "性能浮层",
    [UI_MENU_PERF_OVERLAY_DESC] = "显示帧率和系统信息。按 F10 切换模式。",
    [UI_MENU_PERF_OFF] = "关",
    [UI_MENU_PERF_FPS] = "FPS",
    [UI_MENU_PERF_FULL] = "完整",

    [UI_PERF_LINE_FPS] = "%.0f fps",

    [UI_MENU_GRAPHICS] = "画面",
    [UI_MENU_OUTPUT_RES] = "游戏分辨率",
    [UI_MENU_OUTPUT_RES_DESC] = "游戏的渲染分辨率。越高越吃显卡。",
    [UI_MENU_UPSCALER_FSR] = "AMD FSR 1",
    [UI_MENU_UPSCALER_BILINEAR] = "双线性",
    [UI_MENU_UPSCALING_NATIVE] = "原生（100%）",
    [UI_MENU_UPSCALING_DESC] = "画面如何平滑并缩放到窗口。原生：以画面的完整尺寸绘制 3D 场景，不使用超分辨率："
                               "最清晰，GPU 负担也最大。",
    [UI_MENU_UPSCALING_DESC_FSR] = "画面如何平滑并缩放到窗口。原生：以画面的完整尺寸绘制 3D 场景，不使用超分辨率："
                                   "最清晰，GPU 负担也最大。",
    [UI_MENU_UPSCALING_DESC_NO_FSR] = "画面如何平滑并缩放到窗口。原生：以画面的完整尺寸绘制 3D 场景，不使用超分辨率："
#ifdef _WIN32
                                      "最清晰，GPU 负担也最大。此处不提供 AMD FSR 3.1：它需要 "
                                      "amd_fidelityfx_vk.dll，但无法加载。",
#else
                                      "最清晰，GPU 负担也最大。此处不提供 AMD FSR 3.1：游戏自带的 FSR 3.1 "
                                      "无法在此 GPU 或驱动上运行（原因见日志中的 fsr3 一行）。",
#endif
    [UI_MENU_GAME_ASPECT] = "游戏画面比例",
    [UI_MENU_GAME_ASPECT_DESC] = "让游戏画面适配更高或更宽的比例。较高比例保留 16:9 的水平视野，并显示更多上方和下方区域；HUD 保持居中。",
    [UI_MENU_GAME_ASPECT_16_9] = "16:9（游戏原生）", [UI_MENU_GAME_ASPECT_WINDOW] = "窗口比例",
    [UI_MENU_GAME_ASPECT_16_10] = "16:10", [UI_MENU_GAME_ASPECT_3_2] = "3:2",
    [UI_MENU_GAME_ASPECT_21_9] = "21:9", [UI_MENU_GAME_ASPECT_32_9] = "32:9",
    [UI_MENU_WIDE_HUD] = "HUD 位置",
    [UI_MENU_WIDE_HUD_DESC] = "画面宽于 16:9 时 HUD 的位置。“居中 16:9”保持游戏原有布局；“屏幕边缘”将心和魔力放在左上角，道具栏放在右上角，小地图、钥匙和卢比数放在右侧边缘。",
    [UI_MENU_WIDE_HUD_CENTER] = "居中 16:9", [UI_MENU_WIDE_HUD_EDGES] = "屏幕边缘",
    [UI_MENU_WIDE_MENUS] = "菜单",
    [UI_MENU_WIDE_MENUS_DESC] = "画面宽于 16:9 时的暂停菜单、系统菜单、地图和加载画面。“填满屏幕”将其铺满整个画面；“黑边”将其保持在中间 16:9，两侧为黑色。",
    [UI_MENU_WIDE_MENUS_FULL] = "填满屏幕", [UI_MENU_WIDE_MENUS_CENTER] = "黑边",
    [UI_MENU_FOV] = "视野",
    [UI_MENU_FOV_DESC] = "镜头能看到多大范围，以游戏自身视野的百分比表示。"
                          "仅限游戏版本 1.4.3。",
    [UI_MENU_SHARPNESS] = "FSR 锐度",
    [UI_MENU_SHARPNESS_DESC] = "FSR 的锐化强度。0 为关闭。",
    [UI_MENU_SHARPNESS_NEEDS_FSR] = "仅限 AMD FSR 1",
    [UI_MENU_FPS_LIMIT] = "帧率上限",
    [UI_MENU_FPS_LIMIT_DESC] = "游戏绘制的最高帧率。帧生成的帧在此之上另算。",
    [UI_MENU_FPS_DISPLAY] = "显示器刷新率",
    [UI_MENU_FPS_DISPLAY_HZ] = "显示器刷新率（%.0f Hz）",
    [UI_MENU_FPS_UNLIMITED] = "不限制",
    [UI_MENU_FPS_RTSS] = "由 RivaTuner Statistics Server 限制",
    [UI_MENU_FPS_RTSS_READONLY] = "RivaTuner 需要以管理员身份运行游戏",
    [UI_MENU_FPS_RTSS_IGNORED] = "RivaTuner 未限制本游戏；由游戏自行限制",
    [UI_MENU_FPS_RTSS_TWO] = "RivaTuner 自身的限制也在生效：两个限制器",
    [UI_MENU_GAME_LANG] = "游戏语言",
    [UI_MENU_GAME_LANG_DESC] = "游戏文本和语音的语言。",
    [UI_MENU_GAME_LANG_AUTO] = "自动",
    [UI_MENU_OPERATION_MODE] = "主机模式",
    [UI_MENU_OPERATION_MODE_DESC] = "以底座或掌机模式运行游戏。",
    [UI_MENU_MODE_DOCKED] = "底座模式",
    [UI_MENU_MODE_HANDHELD] = "掌机模式",
    [UI_MENU_ASSUME_FOCUS] = "后台保持运行",
    [UI_MENU_ASSUME_FOCUS_DESC] = "切换到其他窗口时游戏继续运行。",
    [UI_MENU_TEXT_SPEED] = "文字速度",
    [UI_MENU_TEXT_SPEED_DESC] = "游戏对话框显示文字的速度。快速为三倍速度；即时则每页立即全部显示。"
                                "游戏不允许加快的文字（例如与过场动画或语音同步的台词）保持原速。",
    [UI_MENU_TEXT_SPEED_NORMAL] = "正常", [UI_MENU_TEXT_SPEED_FAST] = "快速",
    [UI_MENU_TEXT_SPEED_INSTANT] = "即时",
    [UI_MENU_VOLUME] = "音量",
    [UI_MENU_MENU_SOUNDS] = "菜单音效", [UI_MENU_MENU_SOUNDS_DESC] = "本菜单的音效音量。",
    [UI_MENU_VOLUME_DESC] = "游戏总音量。",
    [UI_MENU_MOTION] = "体感操作",
    [UI_MENU_MOTION_DESC] = "使用手柄陀螺仪瞄准。",
    [UI_MENU_RUMBLE_STRENGTH] = "振动强度",
    [UI_MENU_RUMBLE_STRENGTH_DESC] = "震动强度。",
    [UI_MENU_RUMBLE_CURVE] = "振动曲线",
    [UI_MENU_RUMBLE_CURVE_DESC] = "数值越低，轻微震动越强。",
    [UI_MENU_RUMBLE_TRIGGERS] = "扳机振动（Xbox）",
    [UI_MENU_RUMBLE_TRIGGERS_DESC] = "同时震动 Xbox 手柄的扳机。",
    [UI_MENU_RUMBLE_HD] = "HD 振动（Switch）",
    [UI_MENU_RUMBLE_HD_DESC] = "将游戏的细腻振动原样发送到 Switch Pro 手柄或 Joy-Con。振动强度仍然有效：默认值即游戏原本的强度。",
    [UI_MENU_RUMBLE_HD_NEEDS] = "需要 Switch Pro 手柄或 Joy-Con",
    [UI_MENU_GROUP_GYRO] = "陀螺仪",
    [UI_MENU_GYRO_CALIBRATION] = "陀螺仪校准",
    [UI_MENU_GYRO_CALIBRATION_DESC] = "手柄静止片刻时记下陀螺仪的静止读数并将其减去，防止准星漂移。连接手柄后请将其放下静置一秒。",
    [UI_MENU_GYRO_DEADZONE] = "陀螺仪死区",
    [UI_MENU_GYRO_DEADZONE_DESC] = "转动慢于所选档位时视为不动，档位与主机相同。低：消除静止手柄的微小抖动，并保留缓慢瞄准；"
                                   "标准和高：让抖动的手柄更稳定，但会忽略最慢的转动。关：所有读数都会通过。",
    [UI_MENU_GYRO_DEADZONE_OFF] = "关", [UI_MENU_GYRO_DEADZONE_LOW] = "低",
    [UI_MENU_GYRO_DEADZONE_STANDARD] = "标准", [UI_MENU_GYRO_DEADZONE_HIGH] = "高",
    [UI_MENU_GYRO_TURN_SENS] = "转向灵敏度",
    [UI_MENU_GYRO_TURN_SENS_DESC] = "转动手柄时准星左右移动的幅度。100 % 即按原样传递转动。",
    [UI_MENU_GYRO_TURN_INVERT] = "反转转向",
    [UI_MENU_GYRO_TURN_INVERT_DESC] = "将陀螺仪的左右方向反转。",
    [UI_MENU_GYRO_TILT_SENS] = "倾斜灵敏度",
    [UI_MENU_GYRO_TILT_SENS_DESC] = "倾斜手柄时准星上下移动的幅度。100 % 即按原样传递倾斜。",
    [UI_MENU_GYRO_TILT_INVERT] = "反转倾斜",
    [UI_MENU_GYRO_TILT_INVERT_DESC] = "将陀螺仪的上下方向反转。",
    [UI_MENU_GYRO_NEEDS_MOTION] = "需要开启体感操作",
    [UI_MENU_PAD_LAYOUT] = "按键布局",
    [UI_MENU_PAD_LAYOUT_DESC] = "设置哪个按键作为 A 键。Nintendo：按位置，A 在右、B 在下、X 在上、Y 在左。Xbox 和 PlayStation："
                                "按标注，A 在下、B 在右、X 在左、Y 在上（PlayStation 手柄上为叉、圆圈、方块和三角）。自动："
                                "Nintendo 标注的手柄用 Nintendo，其他手柄按标注。",
    [UI_MENU_PAD_LAYOUT_AUTO] = "自动",
    [UI_MENU_PAD_LAYOUT_NINTENDO] = "Nintendo（A 在右）",
    [UI_MENU_PAD_LAYOUT_XBOX] = "Xbox（A 在下）",
    [UI_MENU_PAD_LAYOUT_PLAYSTATION] = "PlayStation（A 在下）",
    [UI_MENU_PAD_LAYOUT_IN_USE] = "使用中：%s",
    [UI_MENU_GROUP_STICKS] = "摇杆",
    [UI_MENU_LSTICK_DEADZONE] = "左摇杆死区", [UI_MENU_RSTICK_DEADZONE] = "右摇杆死区",
    [UI_MENU_STICK_DEADZONE_DESC] = "摇杆需要推动多少才生效。摇杆漂移时请调高。",
    [UI_MENU_STICK_DEADZONE_NOW] = "当前摇杆：%.0f %%",
    [UI_MENU_MOUSE_SENS] = "鼠标视角灵敏度",
    [UI_MENU_MOUSE_SENS_DESC] = "鼠标转动视角的速度。",

    [UI_BINDS_HELP] = "选中一个格子后按 A 或 Enter，再按键、鼠标键或手柄按键。"
                      "Delete 或 X 清除格子。已占用的按键会移到这里；"
                      "手柄按键与它替换的那个对调。",
    [UI_BINDS_COL_KEY1] = "按键 1",
    [UI_BINDS_COL_KEY2] = "按键 2",
    [UI_BINDS_COL_PAD] = "手柄",
    [UI_BINDS_GROUP_BUTTONS] = "按键",
    [UI_BINDS_GROUP_MOVE] = "移动",
    [UI_BINDS_GROUP_CAMERA] = "视角",
    [UI_BINDS_PRESS_KEY] = "请为“%s”按下一个键或鼠标键",
    [UI_BINDS_PRESS_PAD] = "请为“%s”按下一个手柄按键",
    [UI_BINDS_CANCEL_HINT] = "Esc 取消，或等待 %d 秒",
    [UI_BINDS_TAKEN] = "%s 已从 %s 移走",
    [UI_BINDS_SWAPPED] = "%s 已与 %s 对调",
    [UI_BINDS_RESET] = "重置所有按键",
    [UI_BINDS_RESET_DESC] = "恢复默认按键。",
    [UI_BIND_A] = "A",
    [UI_BIND_B] = "B",
    [UI_BIND_X] = "X",
    [UI_BIND_Y] = "Y",
    [UI_BIND_LCLICK] = "左摇杆按下",
    [UI_BIND_RCLICK] = "右摇杆按下",
    [UI_BIND_L] = "L",
    [UI_BIND_R] = "R",
    [UI_BIND_ZL] = "ZL",
    [UI_BIND_ZR] = "ZR",
    [UI_BIND_PLUS] = "+ 键",
    [UI_BIND_MINUS] = "- 键",
    [UI_BIND_LEFT] = "十字键左",
    [UI_BIND_UP] = "十字键上",
    [UI_BIND_RIGHT] = "十字键右",
    [UI_BIND_DOWN] = "十字键下",
    [UI_BIND_LSTICK_LEFT] = "向左移动",
    [UI_BIND_LSTICK_UP] = "向前移动",
    [UI_BIND_LSTICK_RIGHT] = "向右移动",
    [UI_BIND_LSTICK_DOWN] = "向后移动",
    [UI_BIND_RSTICK_LEFT] = "视角左转",
    [UI_BIND_RSTICK_UP] = "视角上抬",
    [UI_BIND_RSTICK_RIGHT] = "视角右转",
    [UI_BIND_RSTICK_DOWN] = "视角下压",
    [UI_BIND_DESC_A] = "动作：对话、打开、拾取、攀爬、抓住。按住可冲刺。",
    [UI_BIND_DESC_B] = "收起、取消、返回。在游戏选项中开启“交换跳跃按键”时为跳跃。",
    [UI_BIND_DESC_X] = "跳跃。在游戏选项中开启“交换跳跃按键”时为收起。",
    [UI_BIND_DESC_Y] = "挥剑攻击。",
    [UI_BIND_DESC_LCLICK] = "按下左摇杆（游戏的操作说明中没有）。",
    [UI_BIND_DESC_RCLICK] = "把视角转回林克身后。",
    [UI_BIND_DESC_L] = "快捷选择道具；游戏菜单中的上一页。",
    [UI_BIND_DESC_R] = "快捷选择道具；游戏菜单中的下一页。",
    [UI_BIND_DESC_ZL] = "注目（按住或切换：游戏选项中的注目方式）。",
    [UI_BIND_DESC_ZR] = "使用所选道具：弹弓、弓、回旋镖、钩爪、炸弹、瓶子。",
    [UI_BIND_DESC_PLUS] = "菜单：道具、装备、收集、系统。",
    [UI_BIND_DESC_MINUS] = "地图。",
    [UI_BIND_DESC_LEFT] = "吹奏陶笛。",
    [UI_BIND_DESC_UP] = "戴上或摘下饰品。",
    [UI_BIND_DESC_RIGHT] = "和娜薇说话。",
    [UI_BIND_DESC_DOWN] = "更换靴子。",
    [UI_BIND_DESC_MOVE] = "移动林克。",
    [UI_BIND_DESC_CAMERA] = "转动镜头。",
    [UI_BIND_SHAKE] = "晃动视角摇杆",
    [UI_BIND_DESC_SHAKE] = "晃动右摇杆。按住可持续晃动。",
    [UI_BIND_ACTION_A] = "动作 / 冲刺", [UI_BIND_ACTION_B] = "收起 / 返回", [UI_BIND_ACTION_X] = "跳跃",
    [UI_BIND_ACTION_Y] = "挥剑攻击", [UI_BIND_ACTION_LCLICK] = "—", [UI_BIND_ACTION_RCLICK] = "重置镜头",
    [UI_BIND_ACTION_L] = "道具 / 前页", [UI_BIND_ACTION_R] = "道具 / 后页",
    [UI_BIND_ACTION_ZL] = "注目", [UI_BIND_ACTION_ZR] = "使用道具", [UI_BIND_ACTION_PLUS] = "暂停菜单",
    [UI_BIND_ACTION_MINUS] = "地图", [UI_BIND_ACTION_LEFT] = "陶笛", [UI_BIND_ACTION_UP] = "饰品",
    [UI_BIND_ACTION_RIGHT] = "呼叫娜薇", [UI_BIND_ACTION_DOWN] = "更换靴子", [UI_BIND_ACTION_MOVE] = "移动",
    [UI_BIND_ACTION_CAMERA] = "镜头",
    [UI_KEY_SPACE] = "空格",
    [UI_KEY_TAB] = "Tab",
    [UI_KEY_LSHIFT] = "左 Shift",
    [UI_KEY_LCTRL] = "左 Ctrl",
    [UI_KEY_UP] = "上方向键",
    [UI_KEY_DOWN] = "下方向键",
    [UI_KEY_LEFT] = "左方向键",
    [UI_KEY_RIGHT] = "右方向键",
    [UI_KEY_MOUSE_L] = "鼠标左键",
    [UI_KEY_MOUSE_R] = "鼠标右键",
    [UI_KEY_MOUSE_M] = "鼠标中键",
    [UI_KEY_MOUSE_X1] = "鼠标侧键 4",
    [UI_KEY_MOUSE_X2] = "鼠标侧键 5",
    [UI_KEY_ENTER] = "回车",
    [UI_KEY_BACKSPACE] = "退格",
    [UI_KEY_RSHIFT] = "右 Shift",
    [UI_KEY_RCTRL] = "右 Ctrl",
    [UI_KEY_LALT] = "Alt",
    [UI_KEY_CAPSLOCK] = "Caps Lock",
    [UI_KEY_INSERT] = "Insert",
    [UI_KEY_DELETE] = "Delete",
    [UI_KEY_HOME] = "Home",
    [UI_KEY_END] = "End",
    [UI_KEY_PAGEUP] = "Page Up",
    [UI_KEY_PAGEDOWN] = "Page Down",
    [UI_KEY_NUM_FMT] = "小键盘 %s",
    [UI_PAD_SOUTH] = "下方按键",
    [UI_PAD_EAST] = "右侧按键",
    [UI_PAD_WEST] = "左侧按键",
    [UI_PAD_NORTH] = "上方按键",
    [UI_PAD_LSTICK] = "左摇杆",
    [UI_PAD_RSTICK] = "右摇杆",
    [UI_PAD_L] = "左肩键",
    [UI_PAD_R] = "右肩键",
    [UI_PAD_START] = "Start",
    [UI_PAD_BACK] = "Back",
    [UI_PAD_DPAD_LEFT] = "十字键左",
    [UI_PAD_DPAD_UP] = "十字键上",
    [UI_PAD_DPAD_RIGHT] = "十字键右",
    [UI_PAD_DPAD_DOWN] = "十字键下",
    [UI_PAD_ZL] = "左扳机",
    [UI_PAD_ZR] = "右扳机",
    [UI_PAD_LPADDLE1] = "左背键",
    [UI_PAD_RPADDLE1] = "右背键",
    [UI_PAD_LPADDLE2] = "左背键 2",
    [UI_PAD_RPADDLE2] = "右背键 2",
    [UI_PAD_CROSS] = "叉键",
    [UI_PAD_CIRCLE] = "圆圈键",
    [UI_PAD_SQUARE] = "方块键",
    [UI_PAD_TRIANGLE] = "三角键",

    [UI_MENU_AMIIBO_DIR] = "amiibo 文件夹",
    [UI_MENU_AMIIBO_OPEN] = "打开",
    [UI_MENU_AMIIBO_DEFAULT_DIR] = "默认文件夹",
    [UI_MENU_AMIIBO_REFRESH] = "刷新",
    [UI_MENU_AMIIBO_NO_DIR] = "无法打开文件夹 %s",
    [UI_MENU_AMIIBO_NONE_FOUND] = "%s 中没有 amiibo 转储（540 字节的 .bin 或 .nfc 文件）",
    [UI_MENU_AMIIBO_FILTER] = "搜索",
    [UI_MENU_AMIIBO_LIST_HINT] = "按 A 或点击即可把 amiibo 放上读取器。",
    [UI_MENU_AMIIBO_ADVANCED] = "高级",
    [UI_MENU_AMIIBO_READER] = "NFC 读取器",
    [UI_MENU_AMIIBO_STATE] = "读取器：",
    [UI_MENU_AMIIBO_STATE_IDLE] = "就绪",
    [UI_MENU_AMIIBO_STATE_SEARCHING] = "游戏正在等待 amiibo",
    [UI_MENU_AMIIBO_STATE_FOUND] = "amiibo 已放在读取器上",
    [UI_MENU_AMIIBO_STATE_MOUNTED] = "游戏正在读取 amiibo",
    [UI_MENU_AMIIBO_STATE_OFF] = "已关闭",
    [UI_MENU_AMIIBO_REMOVE] = "拿开 amiibo",
    [UI_MENU_AMIIBO_AUTO_REMOVE] = "放上后自动拿开 amiibo",
    [UI_MENU_AMIIBO_HOLD] = "停留时间",
    [UI_MENU_AMIIBO_SECONDS] = "%.1f 秒",
    [UI_MENU_AMIIBO_NO_TAG] = "读取器上没有 amiibo。",
    [UI_MENU_AMIIBO_TAG_FILE] = "文件：%s",
    [UI_MENU_AMIIBO_TAG_ID] = "角色 %s，系列 %02x，编号 %04x，类型 %02x",
    [UI_MENU_AMIIBO_TAG_UID] = "UID：%s",
    [UI_MENU_AMIIBO_READY] = "就绪",
    [UI_MENU_AMIIBO_TAPPED] = "已放上：%s",
    [UI_MENU_AMIIBO_LOAD_FAILED] = "不是 amiibo 转储（NTAG215，540 字节）：%s",
    [UI_MENU_AMIIBO_REMOVED] = "已拿开 amiibo",
    [UI_MENU_AMIIBO_NICKNAME] = "昵称：%s",
    [UI_MENU_AMIIBO_NOT_REGISTERED] = "未登记主人和昵称",
    [UI_MENU_AMIIBO_SOURCE] = "amiibo 数据：%s",
    [UI_MENU_AMIIBO_SOURCE_NEW] = "新的，尚未保存",
    [UI_MENU_AMIIBO_SOURCE_SAVED] = "由本移植保存",
    [UI_MENU_AMIIBO_SOURCE_DUMP] = "读取自转储",
    [UI_MENU_AMIIBO_DECRYPTED] = "已用密钥解密转储",
    [UI_MENU_AMIIBO_NOT_DECRYPTED] = "转储未解密",
    [UI_MENU_AMIIBO_WRITES] = "写入次数：%u，最后写入 %04u-%02u-%02u",
    [UI_MENU_AMIIBO_GAME_DATA] = "游戏数据：%u 字节，访问 ID %08X",
    [UI_MENU_AMIIBO_NO_GAME_DATA] = "游戏数据：无",
    [UI_MENU_AMIIBO_HAS_GAME_DATA] = "游戏数据：已保存",
    [UI_MENU_AMIIBO_ERASE] = "清除游戏数据",
    [UI_MENU_AMIIBO_ERASE_CONFIRM] = "再按一次以清除",
    [UI_MENU_AMIIBO_ERASED] = "已清除游戏数据",
    [UI_MENU_AMIIBO_ERASE_FAILED] = "无法清除游戏数据",
    [UI_MENU_AMIIBO_KEYS] = "密钥：",
    [UI_MENU_AMIIBO_KEYS_LOADED] = "已加载",
    [UI_MENU_AMIIBO_KEYS_MISSING] = "未加载",
    [UI_MENU_AMIIBO_KEYS_FILE] = "密钥文件：%s",
    [UI_MENU_AMIIBO_RELOAD_KEYS] = "重新加载密钥",
    [UI_MENU_AMIIBO_KEYS_RELOADED] = "已加载密钥：%s",
    [UI_MENU_AMIIBO_KEYS_FAILED] = "未加载密钥（文件不存在或不是 160 字节的密钥文件）：%s",
    [UI_MENU_AMIIBO_WRITEBACK] = "将游戏的更改写入转储文件",
    [UI_MENU_AMIIBO_WRITEBACK_NEEDS_KEYS] = "需要密钥",
    [UI_MENU_AMIIBO_POPUP] = "游戏请求 amiibo 时显示 amiibo 列表",
    [UI_MENU_AMIIBO_POPUP_TITLE] = "游戏正在等待 amiibo",
    [UI_MENU_AMIIBO_POPUP_HINT] = "A：放上   B：关闭",
    [UI_MENU_AMIIBO_DAILY_LIMIT] = "amiibo 每日限制",
    [UI_MENU_AMIIBO_DAILY_LIMIT_DESC] = "开（与游戏相同）：每个 amiibo 每天只给一次道具，之后游戏会提示今天无法再使用该 amiibo。关：每次放上都会给道具。给予面具的两个 amiibo "
                                        "本来就没有限制。",
    [UI_MENU_DISCORD_RPC] = "Discord Rich Presence",
    [UI_MENU_DISCORD_RPC_DESC] = "向 Discord 发送活动和游戏时长。请先在 nx2saria.ini 或 NX2SARIA_DISCORD_APP_ID "
                                 "中设置自己的应用 ID；Discord 会显示该 ID 注册的名称。",
    [UI_MENU_RESET_NO] = "取消",
    [UI_MENU_OV_FPS] = "%.0f FPS",
    [UI_MENU_OV_FRAME_MS] = "%.1f ms",
    [UI_MENU_OV_GAME_PCT] = "游戏 %.0f%%",
    [UI_MENU_OV_MAX_MS] = "最长 %.1f ms",
    [UI_MENU_OV_FPS_LABEL] = "FPS", [UI_MENU_OV_FRAME_LABEL] = "帧时间", [UI_MENU_OV_FMT_FPS] = "%.0f",
    [UI_MENU_OV_LOW1] = "1%% 低 %.0f", [UI_MENU_OV_LOW01] = "0.1%% 低 %.0f",
    [UI_MENU_OV_GPU] = "GPU",
    [UI_MENU_OV_CPU] = "CPU",
    [UI_MENU_OV_RAM] = "内存",
    [UI_MENU_OV_VRAM] = "显存",
    [UI_MENU_OV_NONE] = "-",
    [UI_MENU_OV_REPLAY] = "GPU 帧准备",
    [UI_MENU_OV_GPU_TIME] = "每帧 GPU 忙碌时间",
    [UI_MENU_OV_PIPE_WAIT] = "管线等待",
    [UI_MENU_OV_SHADER_WAIT] = "着色器等待",
    [UI_MENU_OV_TEX_UPLOADS] = "纹理上传",
    [UI_MENU_OV_FMT_PCT] = "%.0f%%",
    [UI_MENU_OV_FMT_TEMP] = "%.0f °C",
    [UI_MENU_OV_FMT_POWER] = "%.0f W",
    [UI_MENU_OV_FMT_MHZ] = "%.0f MHz",
    [UI_MENU_OV_FMT_GB] = "%.1f GB",
    [UI_MENU_OV_FMT_GB_OF] = "/ %.1f GB",
    [UI_MENU_OV_GAME_GB] = "游戏 %.1f GB",
    [UI_MENU_OV_FMT_MS] = "%.2f ms",
    [UI_MENU_OV_PER_FRAME] = "每帧 %.1f",
    [UI_MENU_OV_WINDOW] = "窗口",
    [UI_MENU_OV_OUTPUT] = "游戏画面",
    [UI_MENU_OV_SCENE] = "3D 场景",
    [UI_MENU_OV_LIMIT] = "帧率上限",
    [UI_MENU_OV_FMT_SIZE] = "%ux%u",
    [UI_MENU_OV_FMT_SCENE] = "%ux%u，%.0f%%",
    [UI_MENU_OV_FMT_FSR] = "AMD FSR 1，锐度 %d%%",
    [UI_MENU_ANISO] = "纹理过滤",
    [UI_MENU_ANISO_DESC] = "表面纹理的各向异性过滤：以小角度看到的地面、道路和墙壁在远处依然清晰，而不会模糊。游戏：使用游戏请求的级别，与 Switch 上相同。",
    [UI_MENU_ANISO_GAME] = "游戏",
    [UI_MENU_TEX_DETAIL] = "纹理细节",
    [UI_MENU_TEX_DETAIL_DESC] = "大型表面纹理的清晰度。“降低”和“低”不加载它们最大的版本，使一个区域能放进 6 GB 和 8 GB 显卡的显存；只有近处的表面看起来略微柔和。“自动”根据显卡的显存选择。",
    [UI_MENU_TEX_DETAIL_AUTO] = "自动：%s",
    [UI_MENU_TEX_DETAIL_FULL] = "完整", [UI_MENU_TEX_DETAIL_REDUCED] = "降低", [UI_MENU_TEX_DETAIL_LOW] = "低",
    [UI_MENU_HDR] = "HDR 输出",
    [UI_MENU_HDR_DESC] = "在 HDR 显示器上呈现更亮的高光。请先在 Windows 中开启 HDR。",
    [UI_MENU_HDR_PEAK] = "峰值亮度",
    [UI_MENU_HDR_PEAK_DESC] = "显示器的峰值亮度（尼特）。",
    [UI_MENU_HDR_GAME] = "游戏亮度",
    [UI_MENU_HDR_GAME_DESC] = "游戏中白色的亮度（尼特）。",
    [UI_MENU_HDR_UI] = "HUD 和菜单亮度",
    [UI_MENU_HDR_UI_DESC] = "界面和本菜单的亮度（尼特）。",
    [UI_MENU_HDR_HUE] = "色调校正",
    [UI_MENU_HDR_HUE_DESC] = "明亮色彩保留色调映射前色调的程度；0 让它们随曲线偏移。",
    [UI_MENU_HDR_GAMMA] = "伽马校正",
    [UI_MENU_HDR_GAMMA_DESC] = "按游戏设计时的观看方式，以 2.2 或 BT.1886（2.4）显示器的效果呈现画面；关闭则保留 sRGB 曲线，暗部更亮。",
    [UI_MENU_HDR_TONEMAP] = "色调映射",
    [UI_MENU_HDR_TONEMAP_DESC] = "Vanilla：游戏自身的曲线，SDR 亮度。PsychoV35：场景经过人眼响应模型，与游戏的中灰和影调对齐，直到显示器的峰值亮度。RenoDRT：按亮度把游戏的 SDR 观感延伸到峰值。",
    [UI_MENU_HDR_EXPOSURE] = "曝光",
    [UI_MENU_HDR_EXPOSURE_DESC] = "色调映射前整个画面的亮度；100 % 保持不变。",
    [UI_MENU_HDR_HIGHLIGHTS] = "高光",
    [UI_MENU_HDR_HIGHLIGHTS_DESC] = "明亮部分的亮度；50 保持不变。",
    [UI_MENU_HDR_SHADOWS] = "阴影",
    [UI_MENU_HDR_SHADOWS_DESC] = "暗部的亮度；50 保持不变。",
    [UI_MENU_HDR_CONTRAST] = "对比度",
    [UI_MENU_HDR_CONTRAST_DESC] = "中灰周围的对比度；50 保持不变。",
    [UI_MENU_HDR_SATURATION] = "饱和度",
    [UI_MENU_HDR_SATURATION_DESC] = "所有色彩的强度；50 保持不变。",
    [UI_MENU_HDR_HIGHLIGHT_SAT] = "高光饱和度",
    [UI_MENU_HDR_HIGHLIGHT_SAT_DESC] = "最亮部分的色彩强度；50 保持不变。",
    [UI_MENU_HDR_BLOWOUT] = "过曝",
    [UI_MENU_HDR_BLOWOUT_DESC] = "让最亮的部分趋向白色；0 保留其颜色。",
    [UI_MENU_HDR_FLARE] = "眩光",
    [UI_MENU_HDR_FLARE_DESC] = "像少量杂散光一样提亮最暗的部分；0 保持纯黑。",
    [UI_MENU_HDR_GRADE] = "调色",
    [UI_MENU_HDR_GRADE_DESC] = "HDR 画面保留游戏自身调色的程度；0 为不含调色的场景。",
    [UI_MENU_HDR_MIDGRAY] = "中灰",
    [UI_MENU_HDR_MIDGRAY_DESC] = "场景中灰落在哪里。Vanilla：游戏自身曲线给出的位置。Render：场景的原值，稍暗。",
    [UI_MENU_HDR_MATCH_SLOPE] = "匹配影调斜率",
    [UI_MENU_HDR_MATCH_SLOPE_DESC] = "让中间调具有游戏自身曲线的对比度。",
    [UI_MENU_HDR_TONE_SLOPE] = "影调斜率",
    [UI_MENU_HDR_TONE_SLOPE_DESC] = "影调曲线在中灰处的陡峭程度；50 保持不变。",
    [UI_MENU_HDR_HUE_CLIP] = "色调裁切模拟",
    [UI_MENU_HDR_HUE_CLIP_DESC] = "明亮色彩像游戏 SDR 画面裁切处那样偏转的程度：火焰偏向黄色和白色。0 保持其色调。",
    [UI_MENU_PROMPTS] = "按键提示",
    [UI_MENU_PROMPTS_DESC] = "游戏的按键提示显示哪种按键：包括文字、动作提示和陶笛的音符。Nintendo：游戏原有的提示。"
                             "Xbox 和 PlayStation：该手柄的按键（LB、RB、LT、RT、Menu、View；L1、R1、L2、R2、Options、Create），"
                             "A、B、X、Y 显示为上方布局所按下的按键（使用 Nintendo（A 在右）时由右侧按键执行 A，因此 A 的提示显示 B 或圆圈）。"
                             "键盘和鼠标：每个按键对应的键（E、Ctrl、空格、左键……），WASD 移动，鼠标控制镜头。"
                             "提示文件由启动器在构建游戏时生成，键位以当时 nx2saria.ini 中的设置为准；"
                             "如果缺少提示文件，请更新游戏或重新构建（Windows 启动器中的 Rebuild the Game，Linux AppImage 中的 Update）。"
                             "游戏在启动时读取按键提示，因此重启后才会改变。"
                             "提示属于游戏文件，在“操作”中重新映射的按键在重新构建游戏之前仍显示原来的提示。",
    [UI_MENU_PROMPTS_NINTENDO] = "Nintendo",
    [UI_MENU_PROMPTS_XBOX] = "Xbox",
    [UI_MENU_PROMPTS_PLAYSTATION] = "PlayStation",
    [UI_MENU_PROMPTS_PC] = "键盘和鼠标",
    [UI_MENU_MOUSE_MODE] = "鼠标视角",
    [UI_MENU_MOUSE_MODE_DESC] = "直接：镜头按鼠标移动的距离转动，与电脑游戏相同，没有摇杆死区和响应曲线（游戏 1.1.1 和 1.4.3；其他版本使用摇杆模拟）。摇杆模拟：鼠标相当于右摇杆。",
    [UI_MENU_MOUSE_STICK] = "摇杆模拟",
    [UI_MENU_MOUSE_DIRECT] = "直接",
    [UI_MENU_MENU_CURSOR] = "在游戏菜单中使用鼠标",
    [UI_MENU_MENU_CURSOR_DESC] = "在游戏自身的菜单中（标题和存档、暂停菜单、选项、商店、对话选项）显示指针：指针下的项目被选中，左键确认，右键返回，滚轮滚动列表（在暂停菜单中翻页）。仅在最后使用的是鼠标或键盘时生效，因此使用手柄时菜单保持不变（游戏 1.1.1 和 1.4.3）。",
    [UI_MENU_PROMPTS_MISSING] = "缺少按键提示文件：请在启动器中更新或重新构建游戏",
    [UI_MENU_PROMPTS_FAILED] = "按键提示文件已损坏：请在启动器中重新构建游戏",
    [UI_MENU_PROMPTS_RESTART] = "按键提示重启后生效",
    [UI_MENU_PROMPTS_OFF_SHAPE] = "游戏分辨率不是 16:9 时关闭",
    [UI_MENU_PROMPTS_OFF_MODS] = "已关闭：RomFS 模组替换了它们的文件",
    [UI_MENU_PROMPTS_OFF_PATCHES] = "设置了 NX2SARIA_ROMFS_PATCHES 时关闭",
    [UI_MENU_RESTART_CONFIRM] = "现在重启游戏？上次存档之后的进度将会丢失。",
    [UI_MENU_QUIT_CONFIRM] = "退出游戏？上次存档之后的进度将会丢失。",
    [UI_MENU_RESTART_YES] = "重启",
    [UI_MENU_RESTART_COUNT] = "重启（%d）",
    [UI_MENU_RESTART_SAVING] = "游戏正在存档：稍后即可重启。",
    [UI_MENU_QUIT_SAVING] = "游戏正在存档：稍后即可退出。",
    [UI_MENU_STICK_SENS] = "摇杆镜头速度",
    [UI_MENU_STICK_SENS_DESC] = "右摇杆的镜头速度。",
    [UI_MENU_WALK_KEY] = "步行键",
    [UI_MENU_WALK_KEY_DESC] = "按住可用键盘步行。",
    [UI_MENU_WALK_KEY_TAKEN] = "同时为 %s",
    [UI_MENU_WALK_SCALE] = "步行幅度",
    [UI_MENU_WALK_SCALE_DESC] = "按住步行键时键盘摇杆的推动幅度。一半为走路，推满为奔跑。",
    [UI_MENU_DLSS_DLAA] = "DLAA（原生）",
    [UI_MENU_DLSS_QUALITY] = "质量",
    [UI_MENU_DLSS_BALANCED] = "平衡",
    [UI_MENU_DLSS_PERFORMANCE] = "性能",
    [UI_MENU_DLSS_ULTRA] = "超级性能",
    [UI_MENU_AA] = "抗锯齿",
    [UI_MENU_AA_DESC] = "3D 画面边缘的平滑方式。游戏 SMAA 是游戏自带的，与主机相同。关闭则保留原始边缘。FXAA 快速，略显柔和。SMAA 比游戏自带的找到更多边缘，并保持细节清晰。TAA 即全尺寸的 DLSS 或 "
                        "FSR：运动中边缘最稳定；它会设置放大方式。在 Upscaling 中选择 DLSS 或 FSR 时，它们的 TAA 始终运行，"
                        "此处的选择与其一同运行。立即生效。",
    [UI_MENU_AA_GAME] = "游戏 SMAA",
    [UI_MENU_AA_FXAA] = "FXAA",
    [UI_MENU_AA_SMAA] = "SMAA（极高）",
    [UI_MENU_AA_TAA_DLSS] = "TAA（NVIDIA DLAA）",
    [UI_MENU_AA_TAA_FSR] = "TAA（AMD FSR 3.1）",
    [UI_MENU_AA_TAA_FSR4] = "TAA（%s）",
    [UI_MENU_AA_WITH_TAA] = "另加 Upscaling 的 TAA",
    [UI_MENU_TAB_CHEATS] = "金手指",
    [UI_MENU_CHEATS_NONE] = "此游戏版本没有金手指。模组的金手指是它的 cheats 文件夹中以游戏构建命名的 Atmosphere "
                            "金手指文件：mods\\<模组>\\cheats\\%s。重启游戏后才会读取。对于修改游戏代码的金手指，"
                            "游戏会在启动时自行重新构建（几分钟）。",
    [UI_MENU_CHEATS_MOD] = "%s：%s",
    [UI_MENU_CHEAT_CODE_DESC] = "修改游戏代码。立即开关。",
    [UI_MENU_CHEAT_KEY_DESC] = "按住 %s 时修改游戏代码。立即开关。",
    [UI_MENU_CHEAT_DATA_DESC] = "开启时每帧写入游戏内存。",
    [UI_MENU_CHEAT_MASTER] = "始终开启",
    [UI_MENU_CHEAT_MASTER_DESC] = "文件的主代码：始终开启。",
    [UI_MENU_CHEAT_RESTORE] = "无需使用",
    [UI_MENU_CHEAT_RESTORE_DESC] = "它只会写回游戏原本的代码。关闭金手指时已经会这样做。",
    [UI_MENU_CHEAT_REBUILD] = "重新构建后",
    [UI_MENU_CHEAT_REBUILD_DESC] = "此金手指修改游戏代码，并且是在游戏构建后新增或修改的。在启动器中点击 Play 会把它构建进去。",
    [UI_MENU_CHEAT_UNSUPPORTED] = "不支持",
    [UI_MENU_CHEAT_UNSUPPORTED_DESC] = "此金手指不能用于此游戏版本：",
    [UI_MENU_SAVES] = "存档",
    [UI_MENU_SAVE_EXPORT] = "导出存档",
    [UI_MENU_SAVE_EXPORT_DESC] = "将你的存档（所有存档位）写入“下载”文件夹中的一个 zip 文件，可分享给其他玩家或附在错误报告中。"
                                 "其中不含你的密钥和游戏文件。",
    [UI_MENU_SAVE_EXPORT_BUG] = "导出存档用于错误报告",
    [UI_MENU_SAVE_EXPORT_BUG_DESC] = "同样的 zip，另附本次运行的日志和你的设置（nx2saria.ini），可看出发生了什么以及使用了哪些"
                                     "设置。请将其附在错误报告中。",
    [UI_MENU_SAVE_IMPORT] = "导入存档",
    [UI_MENU_SAVE_IMPORT_DESC] = "选择别人分享的存档（NX2Saria-save zip）。它会在下次启动时替换你的存档（所有存档位）：游戏"
                                 "把进度保存在内存中，若现在替换，游戏会把进度写回并覆盖它。你当前的存档会先复制到 saves "
                                 "文件夹。",
    [UI_MENU_SAVE_IMPORT_WAITING] = "下次启动时",
    [UI_MENU_SAVE_IMPORT_CANCEL] = "取消导入",
    [UI_MENU_SAVE_IMPORT_CANCEL_DESC] = "保留你的存档不变：丢弃等待下次启动的存档。",
    [UI_MENU_SAVE_BUSY] = "处理中…",
    [UI_MENU_SAVE_EXPORTED] = "已保存为 %s",
    [UI_MENU_SAVE_IMPORT_READY] = "该存档（NX2Saria %s）将在下次启动时载入：请选择下方的“重启”。你当前的存档会先复制到 saves "
                                  "文件夹。",
    [UI_MENU_SAVE_IMPORT_CANCELLED] = "已取消导入，你的存档保持不变。",
    [UI_MENU_SAVE_IMPORTED] = "本次启动时已导入分享的存档。你原来的存档在 %s",
    [UI_MENU_SAVE_IMPORTED_NEW] = "本次启动时已导入分享的存档。",
    [UI_MENU_SAVE_E_NO_SAVE] = "还没有可导出的存档：请先游玩直到游戏存档。",
    [UI_MENU_SAVE_E_READ_SAVE] = "无法读取存档文件；请稍后再试。",
    [UI_MENU_SAVE_E_WRITE] = "无法将 zip 写入 %s。",
    [UI_MENU_SAVE_E_READ] = "无法读取 %s。",
    [UI_MENU_SAVE_E_STAGE] = "无法为下次启动准备存档；原因见日志。",
    [UI_MENU_SAVE_E_NOT_SAVE] = "%s 不是 NX2Saria 导出的存档。",
    [UI_MENU_SAVE_E_OTHER_GAME] = "%s 是其他游戏的存档。",
    [UI_MENU_SAVE_E_NEWER] = "%s 由更新版本的 NX2Saria 制作：请先更新 NX2Saria。",
    [UI_MENU_SAVE_E_DAMAGED] = "%s 已损坏：请重新下载。",
    [UI_MENU_SAVE_E_UNSAFE] = "%s 被拒绝：其中含有存档以外的文件、链接或过多的数据。",
    [UI_MENU_CAPTURE_SAVED] = "错误快照 %d 已保存",
    [UI_MENU_CAPTURE_FOLDER] = "文件夹：%s",
    [UI_MENU_CAPTURE_FAILED] = "错误快照 %d 无法保存",
    [UI_BOOT_TITLE] = "正在准备着色器",
    [UI_BOOT_STARTING] = "正在启动", [UI_BOOT_SHADERS] = "着色器：%lld / %lld", [UI_BOOT_PIPELINES] = "管线：%lld / %lld",
    [UI_BOOT_COMPUTE] = "计算管线：%lld / %lld", [UI_BOOT_DRIVER_CACHE] = "驱动缓存：%lld / %lld",
    [UI_MENU_CHEATS_UNBUILT] = "有 %s和 %s尚未构建进游戏：在启动器中点击 Play 会把它们构建进去。",
    [UI_MENU_COUNT_CODE_CHEATS] = "%d 个修改代码的金手指",
    [UI_MENU_COUNT_EXEFS_FILES] = "%d 个 ExeFS 补丁文件",
    [UI_MENU_COUNT_CHEATS] = "%d 个金手指",
    [UI_MENU_PRESET] = "画质预设",
    [UI_MENU_PRESET_DESC] = "一次性设置各项图形选项。“推荐”是本机根据显卡、内存和处理器得到的预设。“高”即游戏发售时的原样；“低”和“中”绘制更少，适合较慢的电脑和掌机；“极高”绘制更多，显示器越大越多；它从不被推荐，只能手动选择。之后手动更改的选项会保持你的设置，预设随即显示为自定义。",
    [UI_MENU_PRESET_RECOMMENDED] = "推荐（%s）",
    [UI_MENU_PRESET_LOW] = "低",
    [UI_MENU_PRESET_MEDIUM] = "中",
    [UI_MENU_PRESET_HIGH] = "高",
    [UI_MENU_PRESET_ULTRA] = "极高",
    [UI_MENU_PRESET_CUSTOM] = "%s（自定义）",
    [UI_MENU_PRESET_CUSTOM_NOTE] = "手动更改：%d 项",
    [UI_MENU_PRESET_REC_NOTE] = "本机推荐：%s",
    [UI_MENU_PRESET_HW_NOTE] = "硬件已更换：正在使用推荐预设",
    [UI_MENU_PRESET_NOTICE] = "本机正在使用画质预设“推荐（%s）”（设置 > 显示）。如需恢复之前的游戏画面，请在那里选择“高”。",
    [UI_MENU_PRESET_CONFIRM] = "以下设置曾被手动更改。预设会把它们改为预设的值：",
    [UI_MENU_PRESET_CONFIRM_YES] = "使用预设",
    [UI_MENU_GAME_RES] = "分辨率",
    [UI_MENU_GAME_RES_NATIVE] = "原生",
    [UI_MENU_GAME_RES_DESC] = "游戏画面的尺寸；非全屏时窗口也采用该尺寸。越高越清晰，但对显卡的负担也越大。",
    [UI_MENU_SCENE_RES] = "场景分辨率",
    [UI_MENU_SCENE_RES_DESC] = "3D 场景的绘制尺寸，以占画面的比例计。较低时场景绘制得更小，再放大到画面：更快，适合较慢的电脑和掌机，但更模糊。HUD 和菜单保持清晰，使用完整分辨率。100% 即原生。更改立即生效。",
    [UI_MENU_SCENE_RES_GAME] = "%s（游戏）",
    [UI_MENU_SCENE_RES_NATIVE] = "%s（原生）",
    [UI_MENU_SCENE_RES_PRESET] = "%s（预设）",
    [UI_MENU_DYN_RES] = "动态分辨率",
    [UI_MENU_DYN_RES_DESC] = "显卡跟不上帧率上限时降低 3D 场景的分辨率，跟得上时再升回：在吃力的场面牺牲一点清晰度，换取更稳定的帧率。HUD 和菜单保持清晰。使用 DLSS 或 FSR 时逐级降低超分辨率模式，最低到“性能”；不使用时降低场景分辨率，最低到下方的最低值。更改立即生效。",
    [UI_MENU_DYN_RES_MIN] = "最低场景分辨率",
    [UI_MENU_DYN_RES_MIN_DESC] = "动态分辨率可把 3D 场景降到的最低分辨率，以占画面的比例计。较低时在较慢的电脑上帧率更稳定，但吃力的场面会更模糊。不会高于所选的场景分辨率。使用 DLSS 或 FSR 时此项不生效：改为把超分辨率模式降到“性能”。更改立即生效。",
    [UI_MENU_DYN_RES_MIN_OFF] = "仅在开启动态分辨率时可用",
    [UI_MENU_DYN_RES_NO_ROOM] = "无可降低：场景分辨率已等于或低于最低值",
    [UI_MENU_DYN_RES_NATIVE] = "在 DLAA 或 Native AA 下无可降低",
    [UI_MENU_VRAM_WARN] = "显存已满，游戏因此变慢。请在“设置 > 显示”中：",
    [UI_MENU_VRAM_WARN_RES] = "- %s：调低",
    [UI_MENU_VRAM_WARN_MODE] = "- %s：选择比 %s 更轻的模式",
    [UI_MENU_VRAM_WARN_FRAMEGEN] = "- %s：关闭",
    [UI_MENU_VRAM_WARN_PRESET] = "- %s：选择更低的一档",
    [UI_MENU_VRAM_FIT_MEASURED] = "显存已使用 %d%%（%llu / %llu MiB）。以下设置会占用更多显存：%s。",
    [UI_MENU_VRAM_FIT_NOTICED] = "本次游戏中显存曾经用尽（当前已使用 %d%%，%llu / %llu MiB）。以下设置会占用更多显存：%s。",
    [UI_MENU_VRAM_FIT_PRESET] = "这块显卡有 %u GiB 显存。为它推荐的预设使用：%s。",
    [UI_MENU_VRAM_FIT_FULL] = "显存已满（%llu / %llu MiB）。以下设置会占用更多显存：%s。",
};

#define SWKBD_ONLY(name, ...) static const char *const name[UI_TEXT_COUNT] = {SWKBD(__VA_ARGS__)}
SWKBD_ONLY(text_ja, "文字入力", "名前を入力してください", "ウマ", "OK", "キャンセル", "コントローラー: A = OK、B = キャンセル");
SWKBD_ONLY(text_fr, "Saisie de texte", "Entrez un nom", "Cheval", "OK", "Annuler", "Manette : A = OK, B = Annuler");
SWKBD_ONLY(text_de, "Texteingabe", "Gib einen Namen ein", "Pferd", "OK", "Abbrechen", "Controller: A = OK, B = Abbrechen");
SWKBD_ONLY(text_it, "Inserimento testo", "Inserisci un nome", "Cavallo", "OK", "Annulla", "Controller: A = OK, B = Annulla");
SWKBD_ONLY(text_es, "Introducir texto", "Introduce un nombre", "Caballo", "Aceptar", "Cancelar",
           "Mando: A = Aceptar, B = Cancelar");
SWKBD_ONLY(text_nl, "Tekstinvoer", "Voer een naam in", "Paard", "OK", "Annuleren", "Controller: A = OK, B = Annuleren");
SWKBD_ONLY(text_pt, "Inserir texto", "Introduz um nome", "Cavalo", "OK", "Cancelar", "Comando: A = OK, B = Cancelar");
SWKBD_ONLY(text_zh_hant, "文字輸入", "請輸入名字", "馬兒", "確定", "取消", "手把：A 確定，B 取消");
SWKBD_ONLY(text_ko, "문자 입력", "이름을 입력하세요", "말", "확인", "취소", "컨트롤러: A 확인, B 취소");

static const Row rows[] = {
    {"en", text_en}, {"ja", text_ja}, {"fr", text_fr}, {"de", text_de}, {"it", text_it}, {"es", text_es},
    {"nl", text_nl}, {"pt", text_pt}, {"ru", text_ru}, {"zh-Hans", text_zh_hans}, {"zh-Hant", text_zh_hant},
    {"ko", text_ko}, {"pt-BR", text_pt_br},
};

const char *ui_text_lang(const char *code) {
    static const struct { const char *code, *lang; } map[] = {
        {"en-US", "en"}, {"en-GB", "en"}, {"fr-CA", "fr"}, {"es-419", "es"},
        {"zh-CN", "zh-Hans"}, {"zh-TW", "zh-Hant"},
    };
    for (size_t i = 0; i < sizeof map / sizeof *map; i++)
        if (!strcmp(code, map[i].code)) return map[i].lang;
    return code;
}

static const struct { const char *code; UiTextId name; int since; const char *mals; } all_langs[] = {
    {"en-US", UI_MENU_LANGNAME_EN, 0, "USen"}, {"en-GB", UI_MENU_LANGNAME_EN_GB, 0, "EUen"},
    {"ja", UI_MENU_LANGNAME_JA, 0, "JPja"}, {"fr", UI_MENU_LANGNAME_FR, 0, "EUfr"},
    {"fr-CA", UI_MENU_LANGNAME_FR_CA, 0, "USfr"}, {"de", UI_MENU_LANGNAME_DE, 0, "EUde"},
    {"it", UI_MENU_LANGNAME_IT, 0, "EUit"}, {"es", UI_MENU_LANGNAME_ES, 0, "EUes"},
    {"es-419", UI_MENU_LANGNAME_ES_419, 0, "USes"}, {"pt-BR", UI_MENU_LANGNAME_PT_BR, 143, "USpt"},
    {"nl", UI_MENU_LANGNAME_NL, 0, "EUnl"}, {"ru", UI_MENU_LANGNAME_RU, 0, "EUru"},
    {"zh-Hans", UI_MENU_LANGNAME_ZH_HANS, 0, "CNzh"}, {"zh-Hant", UI_MENU_LANGNAME_ZH_HANT, 0, "TWzh"},
    {"ko", UI_MENU_LANGNAME_KO, 0, "KRko"}, {"pl", UI_MENU_LANGNAME_PL, 999, "EUpl"},
};

int ui_text_game_langs(int game_version, UiGameLang *out, int max) {
    int n = 0;
    for (size_t i = 0; i < sizeof all_langs / sizeof *all_langs && n < max; i++)
        if (game_version >= all_langs[i].since) out[n++] = (UiGameLang){all_langs[i].code, all_langs[i].name};
    return n;
}

int ui_text_game_langs_in(const char *const *regions, int nregions, UiGameLang *out, int max) {
    int n = 0;
    for (size_t i = 0; i < sizeof all_langs / sizeof *all_langs && n < max; i++)
        for (int r = 0; r < nregions; r++)
            if (!strcmp(regions[r], all_langs[i].mals)) {
                out[n++] = (UiGameLang){all_langs[i].code, all_langs[i].name};
                break;
            }
    return n;
}

static const Row *find_row(const char *lang) {
    for (size_t i = 0; i < sizeof rows / sizeof *rows; i++)
        if (!strcmp(rows[i].lang, lang)) return &rows[i];
    return NULL;
}

int ui_text_defined(const char *lang, UiTextId id) {
    const Row *r = find_row(lang);
    return (unsigned)id < UI_TEXT_COUNT && r && r->text[id] && *r->text[id];
}

const char *ui_text_in(const char *lang, UiTextId id) {
    if ((unsigned)id >= UI_TEXT_COUNT) return "";
    return ui_text_defined(lang, id) ? find_row(lang)->text[id] : rows[0].text[id];
}

const char *ui_text(UiTextId id) { return ui_text_in(ui_text_lang(ov_language()), id); }

void ui_text_visit(const char *lang, void (*visit)(void *ctx, const char *text), void *ctx) {
    const Row *r = find_row(lang);
    if (!r) return;
    for (int id = 0; id < (int)UI_TEXT_COUNT; id++)
        if (r->text[id] && *r->text[id]) visit(ctx, r->text[id]);
}

static const char *resolve_menu_lang(void) {
    const char *v = cfg_value("NX2SARIA_MENU_LANG");
    const char *sys = plat_ui_language();
    const char *lang = v && *v ? ui_text_lang(v) : sys && *sys ? ui_text_lang(sys) : "en";
    if (ui_text_defined(lang, UI_MENU_TITLE)) return lang;
    static char base[16];
    size_t n = strcspn(lang, "-");
    if (n < sizeof base && lang[n]) {
        memcpy(base, lang, n), base[n] = 0;
        if (ui_text_defined(base, UI_MENU_TITLE)) return find_row(base)->lang;
    }
    return "en";
}

static const char *menu_lang(void) {
    static _Atomic(const char *) lang = "en";
    static _Atomic unsigned seen = ~0u;
    unsigned v = cfg_version();
    if (v != atomic_load_explicit(&seen, memory_order_relaxed)) {
        atomic_store_explicit(&lang, resolve_menu_lang(), memory_order_relaxed);
        atomic_store_explicit(&seen, v, memory_order_relaxed);
    }
    return atomic_load_explicit(&lang, memory_order_relaxed);
}

const char *menu_text(UiTextId id) { return ui_text_in(menu_lang(), id); }

int ui_text_plural_form(const char *lang, int n) {
    unsigned u = n < 0 ? 0u - (unsigned)n : (unsigned)n;
    if (!strcmp(lang, "ru")) {
        unsigned d = u % 10, h = u % 100;
        return d == 1 && h != 11 ? 0 : d >= 2 && d <= 4 && (h < 12 || h > 14) ? 1 : 2;
    }
    return u != 1;
}

const char *ui_text_count(const char *lang, UiTextId id, int n, char *buf, size_t size) {
    const char *form = ui_text_in(lang, id);
    for (int k = ui_text_plural_form(ui_text_defined(lang, id) ? lang : "en", n); k > 0; k--) {
        const char *bar = strchr(form, '|');
        if (!bar) break;
        form = bar + 1;
    }
    char one[256];
    snprintf(one, sizeof one, "%.*s", (int)strcspn(form, "|"), form);
    snprintf(buf, size, one, n);
    return buf;
}

const char *menu_count(UiTextId id, int n, char *buf, size_t size) { return ui_text_count(menu_lang(), id, n, buf, size); }
