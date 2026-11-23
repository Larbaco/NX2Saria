# runtime/ui

The in-game menu. This page is started: it holds the notes of the code changed since the comments rule. The rest of
`runtime/ui/` keeps its comments until the area is extracted ([README.md](README.md#areas)).

## `runtime/ui/cheats_tab.cpp`

### `tab_cheats()`

When an enabled mod has code cheats or ExeFS patch files the build has not taken up
(`runtime/patches/cheats.c` `cheats_unbuilt()`), the tab's first line says how many and that Play in the launcher builds
them in (`UI_MENU_CHEATS_UNBUILT`, two `%d`: the code cheats, then the ExeFS patch files; English, Russian, Brazilian
Portuguese and Simplified Chinese in `runtime/ui/ui_text.c`). There is no button: the game no longer restarts into a
rebuild. The Ocarina of Time menu has no Cheats tab among its tabs (`runtime/ui/menu.cpp` `kTabNames`), so the tab is
not drawn in this game; the launcher's Mods screen is where a player sees a mod's state.
