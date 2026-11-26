# A settings tab in the game's pause menu (1.1.1, experimental)

`pause_settings_tab = 1` in `nx2saria.ini` (`NX2SARIA_PAUSE_SETTINGS_TAB`; default 0, read at start) puts a fifth tab
after the pause menu's System tab, for a player who has only a gamepad in hand. It shows the menu's Picture rows in
the pause menu itself, with the same draft and Apply as the settings menu. The settings menu and its shortcut stay as
they are. The implementation notes: [code/ui.md](code/ui.md), `runtime/ui/pause_tab.c`.

## What the player sees

- The tab's name after the System tab's, between the game's R icon and the rupee counter: "NX2Saria" (`pause_settings_tab_name = product`, the
  default), or the menu language's "Settings" (`pause_settings_tab_name = settings`; English where the menu has no
  translation). It shows only while the pause menu's header shows its four categories, so never in the Minus menu.
- R on the System tab opens it, and so does a click on its name while the System tab is shown. The System tab's page
  is hidden and the Picture rows are drawn in its place; the name gets the selected tab's plate and the System
  category gives up its own.
- L, B or Plus closes it. With changes in the draft the box "Apply your changes?" asks first, as the settings menu
  does when it is left. The System tab's page comes back.
- The rows are the settings menu's own: the frame limit, VSync, the upscaler, frame generation, sharpening and
  anisotropic filtering. A row whose setting takes a restart says so, as in the settings menu.

## What the game has (1.1.1)

- The pause menu (`PlusMenu_00`, set up at `main+0xf79404`) lays its four pages on one strip: `Pa_Collect_00`,
  `Pa_Equip_00`, `Pa_Item_00`, `Pa_System_00`, each an object the set-up makes by name in a field of its own
  (+0x308 to +0x320). The page change plays one of six animations named for a pair of neighbours (`CollectToEquip`
  to `EquipToCollect`, the fields +0x2b0 to +0x2d8), which slide `N_MenuChange_00` and `N_MenuChange_01`. There is no
  animation past the System page: R there changes nothing.
- The pages change from the sequence `Sequence/PlusMenu.module.ainb`: `SeqMenuTabChangeDirectionSelector`,
  `SeqExecuteTransitionPlusMenu` (from and to a category) and `SeqGetCurrentMinusMenuCategory`. The System page's five
  buttons are fixed, and the sequence branches by their names.
- The header (`MenuOverlay_00`, shared with the Minus menu) counts the `Pa_Category_NN` parts its layout holds
  (`main+0x33e5b44`, the count at +0x328, the category objects at +0x330). Its refresh (`main+0xa69220`) gives each a
  label and the selected state by a category number: 0 Collection, 1 Threads of Time, 2 Map, 3 Equipment, 4 Items,
  5 System, 6 Key Items (`main+0xa6ab50`, the labels of `LayoutMsg/MenuOverlay_00.msbt`). The Plus menu fills the
  four parts with 0, 3, 4 and 5; the Minus menu fills three and hides the fourth.

## Why the tab is drawn, not laid out

A fifth `Pa_Category_04` added to `MenuOverlay_00` would be counted, but the refresh gives no label and no selected
state to a part past the fourth, and the Minus menu hides only the fourth. A real fifth category would need three
patches of the game's code (a label for the part at `main+0xa6ab50`, its selected state in `main+0xa69220`, its
hiding in the Minus menu's branch of the same function), a fifth page made with the game's page constructor, two
more page animations, and new states in the AINB sequence, which this port can only edit in place today. That is
the native tab; this one is the first step.

So the tab changes no file of the game and patches no game code. It reads the pause menu and its System page through
the screens' site (`main+0x2d13b0`, the sets `menu_mouse` and `quit_menu`), and writes two bits while it is open:
the shown bit of the System page's `N_InOut_00` (the pane the game fades out with `InContent` when its Options
opens; the game never sets that bit itself) and the one of the System category's plate, `N_Bg_00`. Both are set
again when the tab closes. With `pause_settings_tab = 0` nothing is read or written.

## When R is taken

R is taken only while all of these hold, checked at the pause menu's own update and true for 250 ms after:

- the focus is on an entry of the pause menu, for player 1, and the pause menu takes input (open, not hidden, not
  blocked);
- that entry is a button of the System page (`PaSystemMenu_00` among its layouts);
- the System page is in the middle of the screen and did not move since the last update (no page change playing);
- the System page's layout has the shape the tab knows;
- the header shows its fourth category.

Save, Load, Options, Special Actions and the Title Screen confirmation take the focus to their own screens, so R goes
to the game there. A check that fails leaves R to the game: the worst case is a tab that does not open.

## Off

- `pause_settings_tab = 0` or unset: off.
- A game version other than 1.1.1: off.
- Both `menu_mouse` and `quit_menu` off: off, since the screens' site does not run.
- A header with other than four categories (a mod's): no tab.

## Log

- `pause_tab: on (NX2SARIA_PAUSE_SETTINGS_TAB): a fifth tab, "NX2Saria", after the pause menu's System tab; ...` at
  start, or `pause_tab: off ...` with the reason.
- `pause_tab: the header's category 3 has no box ...` (at most 4 lines): the header could not be read, and the tab
  stands where the game's layout puts the categories; the System category then keeps its plate while the tab is open.
- `pause_tab: header: the System category at x 490..890, the tab at 1054..1314 (layout units); its plate found`: once,
  the first time the header shows.
- `pause_tab: R on the System tab: the tab opens` (or `a click on the tab`), `pause_tab: the tab opened`,
  `pause_tab: the System tab's page hidden (N_InOut_00 flags ... -> ...)`.
- `pause_tab: Apply from the tab: 1 change(s): VSync: On -> Off`.
- `pause_tab: the tab closed (L) after 5230 ms` (or `B`, `Plus`, `Apply`, `Discard`, a failure), then `pause_tab: the
  System tab's page shown again (...)`.

## Testing

`NX2SARIA_PAUSE_SETTINGS_TAB_TEST` takes words, for test runs only: `log` writes why R is or is not taken each time
that changes (at most 120 lines); `screens` names each screen the screens' site runs for; `fail` makes the page fail to draw 30 frames after the tab opens, which closes it
and gives the System page back. While the tab is open, `NX2SARIA_INPUT_SCRIPT` reaches its rows too.
`tests/pause_tab_check.py` checks the R press, the tab's place, the page's settling and the layouts' shape (with
`--romfs`, against the game's own `PaSystemMenu_00`, `PaMenuCategory_00` and `MenuOverlay_00`).
