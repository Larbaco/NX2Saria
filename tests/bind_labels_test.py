#!/usr/bin/env python3
"""Check the bind action catalog in all menu languages and its narrow-row fallback."""

import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
TEXT = (ROOT / "runtime/ui_text.c").read_text(encoding="utf-8")
HEADER = (ROOT / "runtime/ui_text.h").read_text(encoding="utf-8")
BIND_PAGE = (ROOT / "runtime/ui/binds_page.cpp").read_text(encoding="utf-8")
ROWS = (ROOT / "runtime/ui/option_rows.cpp").read_text(encoding="utf-8")
MENU = (ROOT / "runtime/ui/menu.cpp").read_text(encoding="utf-8")


def require(ok, message):
    if not ok:
        raise SystemExit(f"bind labels: FAIL: {message}")


names = re.findall(r"\b(UI_BIND_ACTION_[A-Z]+)\b", HEADER[HEADER.index("// Short action labels"):])
names = list(dict.fromkeys(names))
require(names == [
    "UI_BIND_ACTION_A", "UI_BIND_ACTION_B", "UI_BIND_ACTION_X", "UI_BIND_ACTION_Y",
    "UI_BIND_ACTION_LCLICK", "UI_BIND_ACTION_RCLICK", "UI_BIND_ACTION_L", "UI_BIND_ACTION_R",
    "UI_BIND_ACTION_ZL", "UI_BIND_ACTION_ZR", "UI_BIND_ACTION_PLUS", "UI_BIND_ACTION_MINUS",
    "UI_BIND_ACTION_LEFT", "UI_BIND_ACTION_UP", "UI_BIND_ACTION_RIGHT", "UI_BIND_ACTION_DOWN",
    "UI_BIND_ACTION_MOVE", "UI_BIND_ACTION_CAMERA",
], "the button catalog changed without updating this test")

expected_languages = {"en": "text_en", "pt-BR": "text_pt_br", "ru": "text_ru", "zh-Hans": "text_zh_hans"}
swap_help = {
    "en": "in the game's Options.",
    "pt-BR": "nas opções do jogo.",
    "ru": "в настройках игры включена смена кнопки прыжка.",
    "zh-Hans": "在游戏选项中开启“交换跳跃按键”时",
}
for lang, array in expected_languages.items():
    start = TEXT.index(f"static const char *const {array}[UI_TEXT_COUNT]")
    end = TEXT.index("\n};", start)
    block = TEXT[start:end]
    for name in names:
        match = re.search(rf"\[{name}\]\s*=\s*\"([^\"]*)\"", block)
        require(match and match.group(1), f"{name} has no {lang} label")
    dash = re.search(r"\[UI_BIND_ACTION_LCLICK\]\s*=\s*\"([^\"]*)\"", block)
    require(dash and dash.group(1) == "—", f"left stick click is not marked — in {lang}")
    for name in ("UI_BIND_DESC_B", "UI_BIND_DESC_X"):
        match = re.search(rf"\[{name}\]\s*=\s*\"([^\"]*)\"", block)
        require(match and swap_help[lang] in match.group(1), f"{name} lacks the swap note in {lang}")

require("Travar alvo" in TEXT, "the Brazilian Portuguese ZL term is missing")
require("UI_BIND_ACTION_A + a" in BIND_PAGE and "UI_BIND_ACTION_MOVE" in BIND_PAGE and
        "UI_BIND_ACTION_CAMERA" in BIND_PAGE,
        "not every bind row is mapped to a translated action label")
require("BIND_ACTION_ROWS == 24" in BIND_PAGE and "binds_action_row_count(void)" in BIND_PAGE,
        "the logged row count does not follow the button, movement and camera rows")
require("inline_action" not in ROWS and "full_label" not in ROWS,
        "bind actions are still conditionally shown beside the name")
require('fit_text(dl, label_x, label_right, p.y + fs * 0.08f' in ROWS and
        'fit_text(dl, label_x, label_right, p.y + fs * 1.02f' in ROWS and
        'kGrey, action, false, true' in ROWS,
        "the bind row does not draw a dim second line with ellipsis fitting")
require('strcmp(action, "—")' in ROWS, "the empty left-stick click still displays a dash subtitle")
require('snprintf(help, sizeof help, "%s\\n\\n%s", description, action)' not in ROWS,
        "the action label is still appended to the help pane")
require('ellipsis && tw > room' in ROWS and 'memcpy(shortened + n, "…"' in ROWS and
        'ImVec4 clip(x0, y, x1, y + h)' in ROWS,
        "the action line is not ellipsized and clipped to the label column")

# Captured menu images show that this column fits about 16 Latin/Cyrillic characters and 7 full-width
# Chinese characters at 1280x720. The previous L labels visibly ellipsized at these limits, so this
# deliberately checks the complete text against the capture-calibrated budgets, not fit_text's clipping.
lane_char_budget = {"en": 16, "pt-BR": 17, "ru": 14, "zh-Hans": 7}
for lang, array in expected_languages.items():
    start = TEXT.index(f"static const char *const {array}[UI_TEXT_COUNT]")
    end = TEXT.index("\n};", start)
    block = TEXT[start:end]
    for name in names:
        label = re.search(rf"\[{name}\]\s*=\s*\"([^\"]*)\"", block).group(1)
        require("…" not in label, f"{name} needs ellipses at 1280x720 in {lang}: {label}")
        require(len(label) <= lane_char_budget[lang],
                f"{name} exceeds the capture-calibrated {lane_char_budget[lang]}-character lane in {lang}: {label}")

require(len("Quick item / prev. page") > lane_char_budget["en"] and
        len("Seleção rápida / pág. ant.") > lane_char_budget["pt-BR"],
        "the capture-calibrated budgets no longer reject the labels that visibly clipped")

require('Jump when Swap Jump Controls is on in the game\'s Options.' in TEXT,
        "English B/X help lost the RomFS option name and location")
require('в настройках игры включена смена кнопки прыжка' in TEXT,
        "Russian B/X help should use the descriptive option text")
require('está ligada nas opções do jogo' in TEXT,
        "Brazilian Portuguese B/X help should use the descriptive option text")
require('在游戏选项中开启“交换跳跃按键”时' in TEXT,
        "Chinese B/X help should name the option location")
require("binds_action_row_count())" in MENU and "default B/X; Swap Jump Controls not read" in MENU,
        "the startup log does not state the default label state")
require('!strncmp(t.value, "BIND_", 5)' in MENU and "BIND_<action>#<cell>" in MENU,
        "the headless menu hook cannot focus a bind cell for a row capture")

print("bind labels: 24 rows, all 18 labels pass capture-calibrated 1280x720 limits in 4 languages: ok")
