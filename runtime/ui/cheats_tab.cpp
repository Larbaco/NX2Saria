// The Cheats tab of the in-game menu: the cheats of the mods' cheat files (cheats.c), a heading per mod in the Mods
// tab's order, then a row per cheat in file order and a line per group label of the file; a cheat that cannot be
// switched here is greyed out with the reason.
#include <stdio.h>
#include <string.h>

#include "imgui.h"
#include "ui/option_rows.h"

extern "C" {
#include "cfg.h"
#include "cheats.h"
#include "menu_sound.h"
#include "ui_text.h"
}

extern "C" void tab_cheats(float scale) {
    opt_begin("##cheats", scale);
    static char line[512], desc[768], buttons[192];
    int n = cheats_count();
    if (!n) {
        snprintf(line, sizeof line, menu_text(UI_MENU_CHEATS_NONE), cheats_file()[0] ? cheats_file() : "<build id>.txt");
        opt_text_str(line);
        opt_end();
        return;
    }
    int code, patches;
    cheats_unbuilt(&code, &patches);
    if (code || patches) {
        char cheats[200], files[200];
        snprintf(line, sizeof line, menu_text(UI_MENU_CHEATS_UNBUILT),
                 menu_count(UI_MENU_COUNT_CODE_CHEATS, code, cheats, sizeof cheats),
                 menu_count(UI_MENU_COUNT_EXEFS_FILES, patches, files, sizeof files));
        opt_text_str(line);
    }
    for (int i = 0; i < n; i++) {
        const CheatInfo *c = cheats_info(i);
        if (!i || strcmp(c->mod, cheats_info(i - 1)->mod)) {
            int listed = 0;
            for (int j = i; j < n && !strcmp(cheats_info(j)->mod, c->mod); j++)
                listed += cheats_info(j)->kind != CHEAT_HEADING;
            char count[160];
            snprintf(line, sizeof line, menu_text(UI_MENU_CHEATS_MOD), c->mod,
                     menu_count(UI_MENU_COUNT_CHEATS, listed, count, sizeof count));
            opt_group_text(line);
        }
        UiTextId text = UI_MENU_CHEAT_CODE_DESC;
        switch (c->kind) {
        case CHEAT_HEADING:
            if (!strstr(c->name, "TID=")) opt_text_str(c->name);   // the file's title line only names the game
            continue;
        case CHEAT_CODE: break;
        case CHEAT_KEY: text = UI_MENU_CHEAT_KEY_DESC; break;
        case CHEAT_DATA: text = UI_MENU_CHEAT_DATA_DESC; break;
        case CHEAT_MASTER: text = UI_MENU_CHEAT_MASTER_DESC, opt_disable_next(UI_MENU_CHEAT_MASTER); break;
        case CHEAT_RESTORE: text = UI_MENU_CHEAT_RESTORE_DESC, opt_disable_next(UI_MENU_CHEAT_RESTORE); break;
        case CHEAT_REBUILD: text = UI_MENU_CHEAT_REBUILD_DESC, opt_disable_next(UI_MENU_CHEAT_REBUILD); break;
        case CHEAT_UNSUPPORTED:
            text = UI_MENU_CHEAT_UNSUPPORTED_DESC, opt_disable_next(UI_MENU_CHEAT_UNSUPPORTED);
            break;
        }
        if (c->kind == CHEAT_KEY)
            snprintf(desc, sizeof desc, menu_text(text), cheats_buttons(c->buttons, buttons, sizeof buttons));
        else if (c->kind == CHEAT_UNSUPPORTED)
            snprintf(desc, sizeof desc, "%s %s", menu_text(text), c->note);
        else
            snprintf(desc, sizeof desc, "%s", menu_text(text));
        const bool switched = c->kind == CHEAT_CODE || c->kind == CHEAT_KEY || c->kind == CHEAT_DATA;
        const char *set = switched ? opt_get(c->key) : nullptr;
        const bool on = switched ? set && *set && strcmp(set, "0") != 0 : cheats_on(i) != 0;
        if (opt_toggle_text(c->key, c->name, desc, on) && switched) {
            opt_change(c->key, on ? cfg_is_set(c->key) ? "0" : "" : "1", c->name, menu_text(on ? UI_MENU_ON : UI_MENU_OFF),
                       menu_text(on ? UI_MENU_OFF : UI_MENU_ON));
            menu_sound_play(MENU_SOUND_CURSOR);
        }
    }
    opt_end();
}
