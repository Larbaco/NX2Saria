# The HUD and the menus on an ultrawide picture (Ocarina of Time 1.1.1)

With `game_aspect` wider than 16:9 the game draws a wider picture and the camera sees more at the sides
(`runtime/widescreen.c`). Its interface keeps its 16:9 shape in the middle of that picture. Two settings choose
where the HUD and the menus go from there. Both are read at start and both do nothing at 16:9.

| Setting (`nx2saria.ini`, `[display]`) | Environment | Values | Default |
|---|---|---|---|
| `wide_hud` | `NX2SARIA_WIDE_HUD` | `center` (the game's layout in the middle 16:9), `edges` (the HUD's corner elements at the picture's sides) | `center` |
| `wide_menus` | `NX2SARIA_WIDE_MENUS` | `full` (fill the screen: the pause, system, map and loading screens laid out over the whole picture), `center` (black bars: those screens in the middle 16:9, the sides black while a pause or system menu, a loading screen or a movie is up) | `full` |

The in-game menu has both on the *Screen* tab, under *Widescreen*: *HUD position* (*Middle 16:9* / *Screen edges*)
and *Menus* (*Fill the screen* / *Black bars*). They exist only for a picture that is not 16:9: while *Widescreen*
shows the game's 16:9 (the menu's draft, before *Apply*) both rows are left out, and they come back as soon as
another shape is picked. A *Window shape* that turns out 16:9 still shows them; the start's log says what applied. As
with every setting read at start, a change waits in the menu's draft until *Apply* and then shows *after restart*.

## How the interface reaches the middle 16:9

The game lays its interface out on a grid of 3840 x 2160 units with the origin at the centre and y up, so the edges of
the screen are at x = -1920 and +1920. The set `wide_hud` of `recomp/patches-v111.txt` scales the first row of the
interface's full-screen projection by 16:9 over the picture's ratio when a layout's canvas is set up. Every layout then
lands in the middle 16:9 with its shape kept, except four screens whose canvas keeps the whole width (the keep list in
`widescreen.c` `wide_init`):

| Screen (vtable) | Layout | Why it keeps the whole width |
|---|---|---|
| item wheel background (main+0x4bed098) | `ShortCutBG_00` | it redraws a capture of the whole picture |
| pause and system menus' background (main+0x4be4fb0) | `MenuBG_00`, `SystemBG_00` | the same |
| event fade (main+0x4be61c8) | `EventFade_00` | a fade of one colour must cover the whole picture |
| loading fade (main+0x4be7b30) | `LoadingFade_00` | the same; its loading icon is stretched with it |

## `wide_hud = edges`

The elements that sit at a side of the middle 16:9 move out by

    out = 1080 * w / h - 1920   grid units, for a picture of w x h

so that after the squeeze they sit at the same side of the whole picture. The children keep the game's own distance
from the corner. `out` is 0 within 1% of 16:9, so a 16:9 picture gets the game's layout back. A picture narrower than
16:9 (16:10, the Steam Deck) stays the game's 16:9 picture with bars above and below (`widescreen.c`
`picture_size`, `gfx_present.c` `gfx_fit_rect`), so nothing moves there either.

The picture's width is a multiple of 8 (`widescreen.c` `picture_size`), which shifts `out` a little with the height:

| `game_aspect` | Picture | Ratio | `out` (units) | Middle 16:9 | Each side |
|---|---|---|---|---|---|
| 16:9 | 2560 x 1440 | 1.778 | 0 | 100% | 0 |
| window, 16:10 | 16:9 at the chosen height, such as 2560 x 1440 (bars above and below) | 1.778 | 0 | 100% | 0 |
| 21:9 | 2560 x 1080, 5120 x 2160 | 2.370 | 640 | 75.0% | 12.5% |
| 21:9 | 3416 x 1440 | 2.372 | 642 | 74.9% | 12.5% |
| window, 3440 x 1440 | 3440 x 1440 | 2.389 | 660 | 74.4% | 12.8% |
| window, 3840 x 1600 | 3840 x 1600 | 2.400 | 672 | 74.1% | 13.0% |
| 32:9 | 3840 x 1080, 5120 x 1440, 7680 x 2160 | 3.556 | 1920 | 50.0% | 25.0% |

The edit is made to the shared interface layouts the game reads once at start,
`UI/Layout2Archive/Common.Product.100.Nin_Ounce_NVN2.blarc.zs` (`runtime/wide_hud.c`). `romfs.c` `wide_hud_overlay`
serves that archive with the edit applied, after the action prompt's edit of the same archive
(`romfs.c` `action_prompt_overlay`), and writes it to `<profile>/wide-hud/`. The archive holds each layout four times,
byte for byte the same: every copy changes, or none does.

### What moves (values of the 1.1.1 files)

| Layout | Pane | x in the file | Side | What hangs on it |
|---|---|---|---|---|
| `PlayerStatus_00` | `N_ScaleL_00` | -1920 | left | hearts (`Pa_HeartGauge_00`), magic meter (`Pa_MpGauge_00`), D-pad guide (`Pa_GuideCross_00`), Stone of Agony (`Pa_AgonyStone_00`) |
| `PlayerStatus_00` | `N_ScaleR_00` | 1920 | right | the item slot (`Pa_SelectItem_00`) and its page dots |
| `PlayerStatus_00` | `N_SubItemBaseFishing_00` | 1270 | right | the two sub-item slots beside it |
| `PlayerStatus_00` | `N_VRS_00` | 1626 | right | an empty pane over the item slot |
| `MapMenu_00` | `N_MiniMap_00` | 0 | right | the field and dungeon minimaps (`N_Field_01` 1490, `N_Dungeon_01` 1536, `N_MiniMapReplace_00`) |
| `MapMenu_00` | `N_EventEndIn_01` | 0 | right | the minimap's frame, floor label and dungeon capture picture |
| `MapMenu_00` | `N_MiniMapCap_00` | 1470 | right | the minimaps' capture panes |
| `MapMenu_00` | `N_VRS_00` | 1536 | right | an empty pane over the minimap |
| `MapMenu_00` | `Pa_KeySmall_00` | 0 | right | the small key and boss key counter, a tab over the minimap |
| `NumDisplay_00` | `N_NumDisplayFirst_00` | 0 | right | the count that slides in from the right when rupees or an item's count change |
| `NumDisplay_00` | `N_NumDisplaySecond_00` | 0 | right | its second row (bottles) |

Animation keys that move with them, in `MapMenu_00`:

| Animation | Pane | Keys | At | Also |
|---|---|---|---|---|
| `MapType` | `N_MapIcon_00` x | 2 of 7 | 1490 | the curve's span (last less first) stays 0 |
| `DungeonMiniMapPattern` | `N_PatternDungeonMini_01` x | 19 of 21 | 1536 | the span goes from 1536 to 1536 + `out` |

Before anything is written, `wide_hud.c` checks that each pane is there with the x above, that no animation keys its
x, and that every pane above it has scale x 1 and no rotation, so that a move in its parent's space is the same move
on the grid. It also checks that each curve has the number of keys above at the minimap's place and the span the
format keeps. Each field is read again after the write. A mod's archive whose layouts differ is left as it is, and
the log says why. One that is already moved by the same amount is served as it is.

### The minimap and the pause menu's map

`MapMenu_00` is both the minimap and the pause menu's full-screen map. Its canvas is set up once, so moving the
whole layout (by its projection, or by its root pane) would push the pause map off to the right too. Under
`N_InChallenge_00` the layout has the pause map (`N_LocalMap_00`, with the dungeon items `N_ItemIcon_00` at 1666, the
floor tabs at -1740 and the map title at -1702), the minimap (`N_MiniMap_00`), the map icons (`N_Common_00`) and the
world map (`N_WorldMap_00`). Only the minimap's own panes move. The icon layers are shared: their x is 0 in the file,
and the game's animations put them at the minimap's place (`MapType`, `DungeonMiniMapPattern`) or at 0 for the pause
map. Only the keys at the minimap's place move, so the game's own animation state places the icons on whichever map
is showing.

### What stays in the middle 16:9, and why

- The action prompt (`DoCommand_00`, keyed by `PosType` to x 234, 724 or 466 near the bottom middle) and the button
  hints (`CtrlGuide_00`): near the player's view, not a corner.
- The markers that follow the 3D world: the targeting pointer (`Pointer_00`), the pick-up window (`PickUpWin_00`),
  the dive gauge (`PlayerStatus3D_00`), speech balloons (`BalloonMessage_00`), enemy information (`EnemyInfo_00`).
  The projection places them; a move would take them off their targets.
- Messages (`MessageInGame_00`, `MessageDialog_00`, `MessageDemo_00`, `MessageLog_01`), the mini-game timer and its
  counts (`MiniGameTimer_00`, `NumDisplayMiniGame_00`, top middle), the horse gauge (`HorseGauge_00`), banners and
  titles (`Location_00`'s large name, `DemoNickname_00`, `OpeningTitle_00`), and every menu.
- The autosave icon (`SaveLoadIcon_00`): it also shows while the system menu saves, which stays in the middle 16:9.
- Tabs that slide in from the edge and are left for later: the cutscene skip prompt (`EventSkip_00` `N_InOut_00`),
  the challenge notice (`ChallengeAttention_00`), the small area name (`Location_00` `N_SmallDisplay_00`), the tips
  panel (`ActionGuideTips_00`) and the microphone hint (`Ocarina_00` `N_GuideMicInput_00`). Each would move the same
  way (a pane, or its keys, by `out`); they are not HUD corners.

There is no separate clock, rupee or key HUD in this version: rupees and item counts show through the
`NumDisplay_00` pop-up and keys through `Pa_KeySmall_00`, which both move with the item slot and the minimap.

## `wide_menus = full`

The menus' layouts are laid out for the picture's width: panes moved out by the menus' distance `u` (the same
`1080 * w / h - 1920` as the HUD's, 640 at 21:9) or grown by `2 * u`, with the animation keys that place them
(`runtime/wide_hud.c`, the operations of group `G_FULL`, each with its reason). The pause menu's pages lie `2u`
further apart, a whole picture's width each, and their columns move out towards the picture's sides.

### The pause menu's backdrop

The pause menu (`PlusMenu_00`) draws its pages on one painted picture, `P_MenuBg_00`: 9200 units wide at x 4280 (from
-320 to 8880) under `N_MenuChange_00`, the pane the page animations slide. The slots of the pages' grids are painted
into it, and the redesign moves the columns above them apart by different distances (the equipment's grid by `u`, the
item grid by `3u`), so a stretched picture would leave its slots under the wrong columns. `wide_hud.c` cuts it into
pieces instead, each moving with its column, and draws a plain stretch of the picture (x 1880 to 2010, where nothing
is painted) over each gap that opens between them:

| Pane | Shows (the picture's x) | From | To | Under |
|---|---|---|---|---|
| `P_MenuBg_00` | 1880 to 2010 | -320 | -320 + u | the seam left of the collection grid |
| `P_MenuBg_00_1` | -320 to 3380 | -320 + u | 3380 + u | the collection grid, the scroll ornament, the equipment's left column |
| `P_MenuBg_00_2` | 1880 to 2010 | 3380 + u | 3380 + 2u | a seam |
| `P_MenuBg_00_3` | 3380 to 4980 | 3380 + 2u | 4980 + 2u | the portrait's oval in the middle of the equipment page |
| `P_MenuBg_00_4` | 1880 to 2010 | 4980 + 2u | 4980 + 3u | a seam |
| `P_MenuBg_00_5` | 4980 to 8880 | 4980 + 3u | 8880 + 3u | the equipment's right grid, the scroll ornament, the item grid |
| `P_MenuBg_00_6` | 1880 to 2010 | 8880 + 3u | 8880 + 4u | a seam up to the item page's information panel |

The first piece is the pane itself; the six others are new panes inserted right after it, each with a picture and a
material of the same name whose texture map shows its part of the picture (its UV scale and translate x). How the file
changes for that is in "The layout format (BFFH)" below. `tools/layout_dump.py` `split_picture`, from which the
previews were made, is the reference: the runtime's split matches it byte for byte.

Each of the four copies of `PlusMenu_00.buil` grows by 4,583 bytes (30,285 to 34,868), at any `u`, so the archive is
laid out again around them (`runtime/sarc.c` `sarc_rebuild`): its header, entries and names as they were, each member
at its old start's alignment, each copy in a range of its own, as the game turns each copy's offsets into pointers in
place. The archive grows from 9,927,489 to 9,945,921 bytes. The game sizes the buffer it reads the archive into from
the resource size table (`System/Resource/ResourceSizeTable.Product.111.Nin_Ounce_NVN2.rsizetable.zs`), whose entry
for `UI/Layout2Archive/Common.Product.100.Nin_Ounce_NVN2.blarc` is 9,931,664, only 4,175 bytes over the game's
archive. `romfs.c` `restbl_room` raises that entry by the growth, rounded up to 32 bytes (9,950,112), in the table the
game would read (the game's, or the one merged with the mods' tables, whose higher entries stay), and serves it from
`<profile>/wide-hud/`, with or without mods. When it cannot, the archive is made again without the split, and
`PlusMenu_00` keeps the game's menu layout: its pages never move without their backdrop. A `PlusMenu_00` the split
does not fit (a mod's) keeps the game's menu layout too, alone, and the line says why.

## `wide_menus = center`

With `full` (the default) the menus fill the screen: their layouts are laid out over the whole picture, and the
present clears nothing. With `center` the menus' contents stay in the middle 16:9, the keep list above stays as it
is, and the present clears the picture's sides outside its middle 16:9 to black while a pause or system menu, a
loading screen or a movie is up (`wide_bars.c` `wide_menu_sides`, `gfx_present.c`, after the picture is put in the
window as on any other frame). Those screens then show the middle 16:9 of their whole-width backgrounds, at their own
width, and black sides; the item wheel keeps its sides, the world going on behind it. Squeezing their backgrounds
instead was the first idea and is left out: the pause background redraws a capture of the whole picture, which squeezed
would land in the middle at the wrong width (as the item wheel's does), and a loading fade squeezed into the middle
would leave the scene showing at the sides while it fades in. The loading icon, stretched over the whole picture with
its layout, would sit in the black side: `wide_hud.c` squeezes it (`LoadingFade_00` `N_IconIn_00`: x 1778 and scale x
1 times 16:9 over the picture's ratio, 1323.2 and 0.744 for 3440 x 1440), which puts it in the middle 16:9's bottom
right corner at its own width, where a squeezed layout would draw it.

The signal is the game's own state of the screens behind those menus. Every interface screen is an object whose byte
+0x1a6 says where it is in its life:

| state | set by |
|---|---|
| 0 closed | main+0xc60a10 (vtable +0x258), once the screen has closed |
| 1 opening | main+0x2d62e0, when the screen is asked to open |
| 2 open | main+0x2cda10 (vtable +0x240) |
| 3 closing | main+0x2d8af0 (vtable +0x248) |

Two classes are watched, by their vtables, the ones the keep list names:

- main+0x4be4fb0, the pause and system menus' background (`MenuBG_00`, `SystemBG_00`): on (1 to 3) for as long as
  such a menu is up, so the sides go black as the menu starts to open and come back once it has closed;
- main+0x4be7b30, the loading fade (`LoadingFade_00`): only while open (2). Its opening and closing are the fade
  itself, during which black sides would show before the rest of the picture is dark.

A movie (`ov_movie2.c` `ov_movie_live`) blacks the sides by itself. The runtime learns the screens' addresses from the
patch set `wide_bars` (`recomp/patches-v111.txt`): the screens' setup (main+0x773b90, vtable +0x80, the one that sets
up the canvas the set `wide_hud` keeps or squeezes) gets a call of the patch page's +0xef0 right after its canvas setup
(main+0x774064), with the screen in x19, where `wide_bars.c` `wide_bars_screen_host` keeps the screens of those two
vtables. The setup runs once when the game makes a screen, which lives on from there; a screen the game destroys gets
another vtable, which the present notices and frees its entry for. `wide_bars` switches on with `center` on a
picture wider than 16:9. The [field-of-view patch](patches-v111.md#gameplay-field-of-view) shares this setup hook to
check the pause and map screens at any aspect ratio. `tests/wide_bars_check.py` checks the site as the generator
lifts it and plays the present against stand-in screens.

Whether the game drew a 3D scene was the signal before, and does not tell a menu from play: under the pause menu the
game keeps drawing 3D into a full-size light target with the scene depth (the selected item's model on the items page,
Link on the equipment page), and DLSS keeps running, so the sides never went black. Its upscaler alone, before that,
flickered in play whenever the game moved its scene to another of its pooled light targets.

## The log

At start, with `game_aspect = 21:9` at 1440 lines, `wide_hud = edges` and `wide_menus = center`:

    wide hud: edges, picture 3416x1440 (2.372:1, 16:9 is 1.778): 642 layout units out: PlayerStatus_00 4 panes, MapMenu_00 5 panes and 21 animation keys, NumDisplay_00 2 panes
    wide hud: PlayerStatus_00 (4 copies, 16 fields): N_ScaleL_00 x -1920 -> -2562, N_ScaleR_00 x 1920 -> 2562, N_SubItemBaseFishing_00 x 1270 -> 1912, N_VRS_00 x 1626 -> 2268
    wide hud: MapMenu_00 (4 copies, 108 fields): N_MiniMap_00 x 0 -> 642, N_EventEndIn_01 x 0 -> 642, N_MiniMapCap_00 x 1470 -> 2112, N_VRS_00 x 1536 -> 2178, Pa_KeySmall_00 x 0 -> 642, MapType N_MapIcon_00 x 2 keys 1490 -> 2132, DungeonMiniMapPattern N_PatternDungeonMini_01 x 19 keys 1536 -> 2178
    wide hud: NumDisplay_00 (4 copies, 8 fields): N_NumDisplayFirst_00 x 0 -> 642, N_NumDisplaySecond_00 x 0 -> 642
    widescreen: menus: LoadingFade_00 (4 copies, 8 fields): N_IconIn_00 x 1778 -> 1332.46, scale x 1 -> 0.749415
    wide hud: the layout archive edited (the HUD's edge elements 642 units out, the loading icon at 0.749 of its width), 140 fields (16 ms)
    RomFS overlay: <profile>/wide-hud/Common.Product.100.Nin_Ounce_NVN2.blarc.zs (... bytes, the HUD at the picture's edges, the loading icon in the middle 16:9) replaces the original
    ...
    widescreen: menus center: black bars: the picture's sides black while a pause or system menu, a loading screen or a movie is up, the loading icon in the middle 16:9's corner

and during the game, the watched screens and their states (at most 48 lines a run):

    widescreen: menus: watching the pause or system menu background (MenuBG_00, SystemBG_00) screen at 0x... (state closed)
    widescreen: menus: the pause or system menu background (MenuBG_00, SystemBG_00) screen at 0x...: closed -> opening at present 2402

and the sides, at most 24 times a run, with what started and ended it:

    widescreen: menus: the picture's sides black from present 2402: a pause or system menu is up (its background on)
    widescreen: menus: the picture's sides shown again at present 4020, after 1618 presents black: the menu's background screen closed

A loading screen says `a loading screen is up (the loading fade open)` and `the loading fade closed`, a movie `a movie
plays` and `the movie ended`. Without the set in the generated C the start says so, and only a movie blacks the sides.

With the defaults, or at 16:9, the start still says what is in effect:

    wide hud: center, picture 3440x1440 (2.389:1, 16:9 is 1.778): no pane moves, the HUD stays in the middle 16:9
    widescreen: menus full: fill the screen: the pause, system and loading screens over the whole picture, with their layouts laid out for its width
    wide hud: edges, picture 2560x1440 (1.778:1, 16:9 is 1.778): no pane moves, the HUD stays in the middle 16:9
    widescreen: menus center: the picture is the game's own 2560x1440, nothing to keep in it
    widescreen: menus center: the picture 3840x2160 is 16:9, nothing to keep in it

With a pad layout by label (`pad_layout = xbox`, `playstation` or `auto`) the action prompt's edit of the same archive
comes first, and the overlay line says `replaces the file another change made`.

With `wide_menus = full` (the default) at 21:9, 2560 x 1080, one line per layout of the redesign (the PlusMenu_00 line
says how its backdrop was cut), the summary with the panes added and the archive's sizes, and the resource size
table's entry raised for it:

    wide menus: full, picture 2560x1080: 640 layout units wider on each side: MapMenu_00 11 panes, LoadingFade_00 1 panes, MenuOverlay_00 8 panes, PlusMenu_00 16 panes and 32 animation keys, ...
    wide menus: MapMenu_00 (4 copies, 44 fields): P_MapBase_00 width 3928 -> 5208, ...
    wide menus: PlusMenu_00 (4 copies, 340 fields, P_MenuBg_00 in 7 pieces, 24 new panes): Pa_Equip_00 x 4280 -> 5560, Pa_Item_00 x 8560 -> 11120, ...
    ...
    wide hud: the layout archive edited (the menus over the whole picture, 640 units wider on each side), 836 fields and 24 new panes, the archive 9927489 -> 9945921 bytes (27 ms)
    RomFS overlay: <profile>/wide-hud/ResourceSizeTable.Product.111.Nin_Ounce_NVN2.rsizetable.zs (... bytes, a larger file's resource size) replaces the original
    RomFS: resource size table: UI/Layout2Archive/Common.Product.100.Nin_Ounce_NVN2.blarc raised 9931664 -> 9950112 for the file's 9927489 -> 9945921 bytes
    RomFS overlay: <profile>/wide-hud/Common.Product.100.Nin_Ounce_NVN2.blarc.zs (... bytes, the menus over the whole picture) replaces the original

With mods the table is the merged one: `replaces the file another change made`. When the entry cannot be raised:

    wide hud: <why>; UI/Layout2Archive/Common.Product.100.Nin_Ounce_NVN2.blarc.zs is made again without the edits that make it larger
    wide menus: the layout archive may not grow for P_MenuBg_00's pieces; PlusMenu_00 keeps the game's menu layout

A layout archive that is not the game's (a mod's), or a field that differs, gives one line, and the archive is served
as it is:

    wide hud: MapMenu_00 N_MiniMapCap_00 x is 1400, not 1470; the layout archive is served as it is (the game's own keeps the HUD and the loading icon where the game lays them out)

`menu: draft   WIDE_HUD: center -> edges` and `menu: draft   WIDE_MENUS: full -> center` show a change made in
the menu when it is applied.

## The layout format (BFFH)

What the edit relies on, found in the 1.1.1 archive (157 layouts, each four times) and checked on every pane and every
animation track of it:

- A layout file starts with `BFFH`, its size at +0x08. Its header has a group of 0x20 bytes per table: the offset of
  the list of name offsets, that of the list of record offsets, that of the names' hashes (CityHash64 of each name,
  which the game's lookup by name compares), then the count. Panes at 0x118, materials at 0x138, pictures at 0x158,
  texts at 0x178, alignment panes at 0x198, parts at 0x1b8, captures at 0x1d8, animations at 0x238. The canvas size is
  at 0x2f8 (3840.0 and 2160.0). Offsets count from the file's start.
- A pane record has 0x44 bytes, parents before children: scale x and y at +0x10 and +0x14, rotation at +0x18,
  translate x and y at +0x1c and +0x20 (in the parent's space), the parent's index at +0x28 (-1 for the root), width
  and height at +0x2c and +0x30, the end of its subtree at +0x34, flags at +0x38 (bits 7-10 the kind of pane, bits
  11-13 and 14-16 the horizontal and vertical origin, bits 17-22 the anchor on the parent). Every root pane is 0 x 0
  at the centre, and no HUD pane is anchored to a side of its parent.
- An animation record starts with the offset of its body. The body lists its tracks (+0x08, count +0x10), its curves
  (+0x18, +0x20), one component byte a curve (+0x28: 0 x, 4 y, 8 z), its constants (+0x38, +0x40) and their
  component bytes (+0x48).
- A track points at a 6-byte target code and holds its first curve (+0x08, 0xffff none), its first constant
  (+0x0a), and the two counts (+0x0c, +0x0d). In the code, bits 8-10 are 7 for a pane's own property, the pane's index
  is in bits 11-14 and 16-18 (its high part), and bit 22 with byte 3 name the property (1 and 0 scale, 1 and 1
  translate, 1 and 2 visibility, 1 and 4 alpha).
- A curve points at its frames and at its values. The values hold the last value less the first at +0x00, the offset
  of the floats at +0x08 and their count at +0x10, as (value, slope) pairs. A change of the first or last key changes
  that span as well; all 3,832 curves of the archive keep it.

What the backdrop's split adds to that (`wide_hud.c` `split_layout`; a study of the game's loader found each point):

- The game turns each offset into a pointer in place as it loads a layout, by walking the records from the header's
  tables, wherever in the file they lie, once per record it reaches. New records can go past the old end of the file,
  as long as the file's size and the string pool section (the second of the two 0x28-byte `BFSI` section headers at
  0xa8, at 0xd0: its start at +0x04 and +0x10, its size at +0x08), which reached the end, cover them. A record that
  holds offsets is never shared: each new pane gets its own copy.
- A picture pane has its material's index at +0x3c and its picture record's at +0x42 (u16). The picture record holds
  its material's index at +0x10 (u16), no window (+0x08 0), and offsets of its colour and corner lists inside its own
  0x88 bytes (+0x00, +0x20, +0x48, +0x50, +0x58, +0x60). The material lists its texture maps (+0x00, count +0x08); with
  one map the list lies at +0x40, right after the record, and the map at +0x48: no texture names (+0x00 0), UV scale
  at +0x10, translate at +0x18, rotation at +0x20.
- The panes' order is the game's index of each pane, so the new panes go right after the split pane and every index
  past it moves: a pane's parent (+0x28) and subtree end (+0x34), the pane a part's record (+0x40) and a capture's
  (+0x04) belong to, and the 7-bit index of each animation track's target code (each code once, whatever it targets).
  New pictures and materials go at the end of their tables, with the u32 array at 0x300 (count 0x308, stride 0x30c, 4),
  one entry per material, grown with them. The names, records and hashes of a table keep one order.

## Checks

`tests/wide_hud_check.py` builds `tests/wide_hud_check.c` against `runtime/wide_hud.c` on layouts and archives it makes
from the format above, with the panes, values and curves of the game's four layouts: the moves, the report lines, a
second pass that finds nothing to do, refusals that leave the file as it was (a pane's x, a scaled or turned parent, a
broken parent chain, a renamed pane, a pane keyed by a curve or a constant, a curve with another count of keys or
another span, another animation at the minimap's place, more tracks or panes than the format names, a copy that
differs, copies or layouts that share bytes, a missing layout, the loading icon's scale changed or keyed), a write that
would change what leads to another field (every byte goes back), each shift alone and both, every prefix of each file,
and the compressed archive. With `--romfs <folder with romfs.bin and index.json>` it edits the game's own archive for
2560x1080 and 3416x1440 (21:9), 5120x1440 (32:9) and the windows 3440x1440, 2880x1200 and 3440x1080, checks that only
the 132 expected words change, at the offsets the 1.1.1 files have, then with the loading icon too (140), and again
after the action prompt's edit of the same archive.

The backdrop's split is checked on a `PlusMenu_00` with the game's records for it (its picture, material, parts,
captures, hashes, per-material array and sections): the pane order, parents, subtree ends, track codes, parts' and
captures' panes, each piece's place, width and UV window, the grown tables, the file's and the pool's sizes, a second
split finding nothing to do, refusals that make nothing (a turned or scaled picture, one with a child or another
origin, two texture maps, a window, records not laid out as the game's, another material, a per-material array of
another length, a pool short of the end, shared records, no hashes, a pane already named like a piece), every prefix;
then in an archive of four copies: each copy in a range of its own at its old alignment, the other members as they
were, the grown frame, a refused copy or an archive that may not grow keeping the game's menu layout, and the
resource size table's entry raised. With `--romfs` the split of the game's `PlusMenu_00` at `u` 640 and 1920 is
compared byte for byte with `tools/layout_dump.py` `split_picture`, every copy in the served archive at 21:9, 32:9 and
3440x1440 is checked, and the game's table entry is raised for the 21:9 archive.

What a run has to show, by shape (screenshots at each of 21:9, 32:9, a 3440x1440 window and 16:10):

For 16:10 and 3:2, Phase A keeps the 16:9 UI projection at the center by compressing its vertical scale to
`aspect / (16/9)`. Gameplay therefore has clear space above and below the HUD. The four full-picture canvases (item-wheel
and pause/system backgrounds, event fade, loading fade) keep their full height, so a fade darkens the scene above and
below the UI band and the pause background redraws its blurred capture across the whole picture. Tall pictures do not
clear those margins to black while these screens are active. Movies use the full height; the save thumbnail still crops
to the centered 16:9. The camera first applies the configurable gameplay FOV, then opens the vertical angle by
`2 atan(tan(v/2) * (16/9) / aspect)` for each lens using the game's 16:9 projection. This keeps the horizontal view
unchanged while showing more above and below. These taller modes do not use the installer's `tall_edits`: its 1.1.1
RomFS layout archives have not been validated for those edits.

- the hearts and magic at the top left corner, the item slot and sub-items at the top right, the minimap with its
  frame, floor label, icons and key tab at the bottom right, in the field and in a dungeon;
- the pause menu's map in its usual place, with its icons on it, and the minimap back in the corner after closing it;
- a rupee pick-up and an item count, next to the item slot;
- the action prompt, a message window, the targeting pointer and an enemy's information, in the middle;
- with `wide_menus = center`: the pause menu on each of its pages (R and L), the map, a loading screen (its icon in
  the middle's corner) and a movie in the middle with black sides, the item wheel, and the return to the game (the
  sides shown again at once); no `widescreen: menus: ... black` line while walking, rolling, Z-targeting, in a
  conversation or across a room or area change outside a loading screen; with `full`, no black sides at all;
- the *Menus* and *HUD position* rows gone while *Widescreen* is at 16:9, back at 21:9, and their labels in the four
  menu languages;
- 16:9 and 16:10: the game's own layout and no black sides.
