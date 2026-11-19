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
    "en": "With Swap Jump Controls on, B jumps and X puts away.",
    "pt-BR": "Com Swap Jump Controls ligado, B pula e X guarda.",
    "ru": "При включённой Swap Jump Controls B прыгает, а X убирает оружие.",
    "zh-Hans": "开启“交换跳跃按键”后，B 跳跃，X 收起。",
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
require("CalcTextSizeA(fs, FLT_MAX, 0, full_label).x <= label_room" in ROWS,
        "inline labels are not measured against the available row width")
require("inline_action ? full_label : label" in ROWS and
        'snprintf(help, sizeof help, "%s\\n\\n%s", description, action)' in ROWS,
        "a long action label can be clipped instead of moving to the help pane")
require("binds_action_row_count())" in MENU and "default B/X; Swap Jump Controls not read" in MENU,
        "the startup log does not state the default label state")
require('!strncmp(t.value, "BIND_", 5)' in MENU and "BIND_<action>#<cell>" in MENU,
        "the headless menu hook cannot focus a bind cell for a row capture")

print("bind labels: 24 rows, 18 translated label forms, B/X help in 4 languages, narrow-width fallback: ok")
