# Boot to a map

`boot_map` starts the game straight in a chosen area and start position from the save of a chosen slot. The title screen
and its file menu never show. It is a test and debugging setting: a run without a window gets to the place it
tests in one area load, and nothing has to press through menus. Code: `runtime/boot_map.c`. Ocarina of Time 1.1.1 only.
On other game versions the setting is ignored, and the log says so.

This page records how the game goes from its title to play, what the setting changes and why there, and what is
known, inferred or untested. Everything below was read from the 1.1.1 game files and code (build id `e0f3ccfa`).
Nothing of it was run before this page was written. **[C]** marks what was read in the files or the code. **[I]** marks
what is inferred.

## Using it

Environment or `nx2saria.ini` (`[diagnostics]`). Every key is read once, at start:

| Key | Value | Default |
|---|---|---|
| `boot_map` | `<area>[:<start position>]`, for example `KokiriForest_Child`, `HyruleField_Adult:HyruleField_FrontOfMarket`, `InsideTheDekuTree:Entrance` | unset: the normal boot |
| `boot_map_slot` | `0` to `5`, or `latest` | `latest`: the slot whose file-menu date is the newest |
| `boot_map_age` | `child`, `adult` or `save` | `save`. An overworld area's name fixes the age (`_Child`, `_Adult`), so this matters in dungeons and other areas used by both ages |
| `boot_map_time` | `HH:MM`, 24 hours, or `save` | `save` |

Area names are the game's own. They are the keys of `Banc/BancBasedResource/StartPos.byml`, and the debug menu's
teleport list shows them (F9 with `debug_menu = 1`). An area without a start position, or with an empty one,
uses the area's default: the first entry of its list, which is where the game itself puts Link for an unknown name.

Example of a run without a window (the bench's `CLAUDE.md`):

```sh
SDL_VIDEODRIVER=offscreen SDL_AUDIO_DRIVER=dummy NX2SARIA_AUDIO=none \
  NX2SARIA_BOOT_MAP=HyruleField_Child:HyruleField_FrontOfMarket NX2SARIA_BOOT_MAP_SLOT=2 \
  timeout 150 ./run.sh linux-test
```

### What the log says

Each start writes one line, whether the setting is on or not:

```
boot map: off
boot map: on: HyruleField_Child, start position HyruleField_FrontOfMarket at 54.2 150.3 63.9 (yaw 3.14), slot 2 (the latest), age child (the area's), time from the save
```

When Link appears in the area, a second line gives where he arrived, the slot, and the time since the process started:

```
boot map: arrived in HyruleField_Child (asked HyruleField_Child), slot 2, Link at 54.21 150.31 63.88, 3.1 m from the start position, 21.4 s after the start
```

When the destination cannot be used, one line says why, and the game boots as it always does. The other boot map
lines do not follow:

```
boot map: HyruleFeld_Child is not an area of the game's start positions (StartPos.byml): normal boot
boot map: slot 4 has no save (slot_04/progress.sav): normal boot
boot map: Sequence/NormalTitle.module.ainb is not the 1.1.1 file this setting knows (Root does not start a Simultaneous node): normal boot
boot map: the save of slot 2 lacks a value this setting changes (no PlayerStatus.SavePos): normal boot
```

A bad value of any of the four keys fails the same way, as do a file a mod or a RomFS overlay serves and a game
version other than 1.1.1.

Two more lines can come in between. When the game opens the slot's save:

```
boot map: <mount>:/slot_02/progress.sav is read with the boot map's place (6 values changed in what the game reads)
```

When Link has not appeared 300 s after the start, the changes stop, and the game's return to its title is its own:

```
boot map: no arrival 300 s after the start (the save or the area did not load; see the lines above): the boot map's changes stop
```

A boot map run must use a copy of the profile (`--profile`, as `tools/capture_run.py` does). The game saves the place it
starts in, through its own autosave, into the slot it loaded (see *Risks*).

## The title to play, in the game

The game's flow from boot to play is a set of AI graphs (AINB, version 0x408) under `Sequence/` in the RomFS.
`FormalSequence.root.ainb` is the root. `Sequence/FileEntry/FileEntry.Product.111.byml` replaces six modules with their
`.110` variants (BeforeTitle, CloseTitle, DestroySceneCommon, LoadResidentScreen, Option, ZeldaNotes) **[C]**.

### The root graph **[C]**

```
Enter: Sequential
  SeqOneshotRequestLoadAndSerializeCaption, SeqExecuteWaitCaptionMgrStable      (the file menu's cards)
  SeqOneShotBootupBackgroundWorkMgrSetStartState(2), SeqExecuteWaitBootupBackgroundWorkMgr
  -> Sequential: BeforeInitializeCommon, SeqOneShotAdjustSceneHeap, BeforeTitle
     -> Title.module (FirstBancParam = Work/Banc/GameBancParam/KokiriForest_Child...gyml)
        -> command 'Event' -> Event.module -> 'Main' -> Main.module (play), PlusMenu, MinusMenu, ShortCut, back to Title
```

The modes are graph modules that change on transition commands (`Event`, `Main`, `Title`, `PouchMenu`, ...). No game
data value selects title or play. No `Sequence_ControlInSystem` exists **[C]**: the only `Sequence_*` enums are
`Sequence_ControlInEvent` (Idle, ResetGimmick, BloodyMoon, ChangeBancOnEvent, MemoryShortageBloodyMoon,
ResetAIScheduleNpc), `Sequence_BancChangeFadePattern` and `Sequence_LatestRetryType`.

### BeforeTitle, OpenTitle **[C]**

`BeforeTitle.110` loads the layout archives (Common, Title, ZeldaNotes) and the title screens (`Title_00`, the event
fades, `Talk_00`). It closes the scenery, creates the terrain system, loads the resident events, and starts the event
`Title_Background` (the scene behind the title). Then `SeqOneShotFinishStartupLogo` and `OpenTitle.module` run.
OpenTitle loads and opens `Title_00` and leaves it invisible. It sets the button captions: two buttons without a
save, three with one, from `SeqQueryIsExistSave`. Last come `Sequence_ReservedBootEventName := ''`,
`SeqOneShotSetSequenceInitializeEnd(true)` and `SeqOneShotSetValidAutoSave(true)`.

### Title and NormalTitle **[C]**

`Title.module` runs `NormalTitle.module` beside `SeqExeucteBeforehandLoadEventRequest(Opening_LoadingKokiriForest)`.

`NormalTitle.module`'s command `Root` starts node 35, which:

- sets `UI_Debug_IsOpenedTitle`;
- loads and closes `SystemOverlay_00`;
- runs `SeqOneShotOnTitleBancOptimize`;
- clears `IsEndHandheld`, `IsEndDual` and `IsEndFullKey`;
- then, from `SeqQueryIsExistSave`, shows `Title_00` and waits on `SeqExecuteWaitScreenButtonInput(Title_00)`.

With a save:

- `Pa_Button_00` (*Continue*) opens `ConfirmationLoad.module`. Its `Yes` leads to node 42:
  `Sequential(Simultaneous(SeqOneShotRequestPause(SceneTransition), SeqOneShotSetEnableDynamicSSAO(true)),
  TitleToMainWithLoadSave.module)`. `TitleToMainWithLoadSave`'s `Main` ends the module with `Event`.
- `Pa_Button_01` (*New game*) opens `ConfirmationNewGame`. Its `Yes` leads to node 13:
  - `Sequence_BancChangeFadePattern`, `PrepareScreensOfLoading`, `CloseTitle`;
  - `SeqExecuteSaveGameData` of the account, `SeqOneShotSetMQMode`;
  - `SeqExecuteResetGameData` 3 and 5, `OnResetGameData`, `AfterLoadSaveData`;
  - `Opening.module`, which plays the introduction and loads `KokiriForest_Child`.
- `Pa_Button_02` opens the options.

### The file menu **[C]**

- `ConfirmationLoad` waits for the autosave and the caption manager. It sets `SelectedSaveDataListBtnIndex := -1`
  and runs `LoadList`.
- `LoadList` opens `SystemLoadList_00`, waits for A (`SeqQueryControllerTrigMaskChecker.TrigA`) and the decided
  button, then reads `CaptionData.SelectedSavePathIndex`. At -1 it loops; otherwise it goes to `Check`. That value is
  written by the list's decide handler: main+0x33ce110 calls main+0x9b39e0 with `CaptionData` /
  `SelectedSavePathIndex` and the card's slot.
- `LoadCheck` opens `SystemSaveLoad_00`. `Pa_CheckButton_00` (*Yes*) starts the loading fade, unloads the menu
  screens, and returns `Yes`.

### TitleToMainWithLoadSave: where the save is loaded **[C]**

```
Sequential
  PrepareScreensOfLoading(IsLongFade, IsPlayLoadingFade)
  Simultaneous: LoadResidentScreen(CloseTitle), SeqExecuteSaveGameData(account), SeqOneShotSetMQMode(IsLastSelectedMQMode)
  ChangeBanc.module(
      BancParam = "Work/Banc/GameBancParam/" + Sequence_CurrentBanc + ".game__banc__GameBancParam.gyml",
      IsLoadFromSaveData = IsOverrideTraverseBasePos = IsOverrideCancelSleepRot = IsEnableFastLoad = True,
      OverrideTraverseStartPosName = '',
      OverrideTraversePos = PlayerStatus.SavePos,
      OverrideCancelSleepRot = SeqCalcSavePlayerForward(PlayerStatus.SavePosRadY))
    child DoAfterDestroyBanc: OnLoadGameSaveData, SeqOneShotSetDisableMainEvent(false), SeqOneShotReleasePause
  -> StateEnd 'Main'
```

`ChangeBanc` runs, in order:

1. `BeforeChangeBanc`.
2. `UnloadBanc`, which unloads the title's area.
3. The `DoAfterDestroyBanc` child. This is where `OnLoadGameSaveData` runs.
4. `SetupGameDataOnSceneChange`.
5. `LoadBanc`.
6. `AfterChangeBanc`.

`LoadBanc`'s inputs are query nodes, which run when they are read. The area and the position therefore come from the
save just loaded **[I]**: it follows from the order above, and saves could not resume otherwise.

`OnLoadGameSaveData`:

```
Sequential
  Simultaneous: SeqExecuteResetGameData 3, 0, 8; OnResetGameData   (System_IsSaveLoading := true)
  BoolSelector IsSaveTemporaryStorage
    false: SeqOneShotGameSafetyMgrStopSafety,
           SeqExecuteLoadSaveData(Index <- CaptionData.SelectedSavePathIndex, LoadType 0),
           SeqOneShotSetCaptionMgrLatestIndex(Index <- the same)
  AfterLoadSaveData   (System_IsSaveLoading := false, ...)
```

`SeqExecuteLoadSaveData`'s entry is main+0x32c1db0 **[C]**. It finds the save manager (component 12 of the game
system `*(main+0x4e97648)`) and marks its pending request (`*(manager+0x148)`). `+0x138` gets the bit of the file kind.
`+0x10` gets the index when the index is 0 or more, plus 6 when Master Quest is chosen (bytes `+0x150` and `+0x151`).
Without an index, `+0x10` keeps what it held. The manager's update reads `slot_%02d/progress.sav`.

### Where Link appears **[C]**

`LoadBanc` reads the area file (`SeqOneShotLoadBancParam`), sets `Sequence_CurrentBanc` from its name, and runs
`OnLoadBancParam`, which picks the place:

1. `DebugParam.IsOverrideTraversePos` (Bool 7325ce62, not saved, never reset) true: `DebugParam.OverrideTraversePos`.
2. The area's `IsAlwaysStartFromStartPos` (no area sets it): the area's default.
3. Input `IsOverrideTraverseBasePos` true:
   - an empty `OverrideTraverseStartPosName`: the vector `OverrideTraversePos`. A loaded save takes this path.
   - a name: `SeqQueryStartPosByName` (main+0x3302490). If the area does not list the name, the area's default.
4. Otherwise the area's default.

The area's default is `SeqQueryGetStartPosFromGameBancParam` (main+0xb679e0), which takes entry 0 of the area's
dictionary in `StartPos.byml`. The entries are sorted, so entry 0 is the `''` entry where the area has one. Otherwise
it is the first name in alphabetical order.

A save holds no start position name. It holds `Sequence_CurrentBanc` (String64 1d6189da),
`PlayerStatus.SavePos` (Vector3 c884818d) and `PlayerStatus.SavePosRadY` (Float 1ea93bd8), with
`Sequence_IsChildhood` (Bool cea220d3) and `DungeonRoom` (Int 9b529f63) beside them.

### Age **[C]**

`LoadBancImpl` picks the age, then creates Link and his pouch:

- With `DebugParam.IsSetChildhoodManually`, Link takes the age in `Sequence_IsChildhood`.
- Otherwise, in an area whose `GameBancParam` has `IsForceSwitchChildOrAdult`, `Sequence_IsChildhood` becomes the
  area's `IsChildWhenChangeBanc`. That covers every overworld `*_Child` and `*_Adult` area, `HyruleCastle` and
  `GerudoFortress_Adult`.
- Elsewhere (dungeons, boss rooms), Link keeps `Sequence_IsChildhood`.

So the area's name fixes the age outside dungeons. Doors pick the destination by age themselves
(`NextBancParamNameChild` / `NextBancParamNameAdult`, main+0x29bf380). The game never appends an age to a base name.

### Time of day **[C]**

The clock is saved in UInt64 ef10b80f (its name was not found) as `{u32 seconds of the day 0..86399, i32 day}`. When
a save loads, `SeqOneShotOnloadGameSaveDataWorldMgr` (main+0x3452050, then main+0x3425090) reads it and sets the
clock. The event node `ExecuteWmSetTime` (main+0x2890e80) sets the clock during play through main+0x34522d0(WorldMgr
`*(main+0x4e7eab0)`, seconds).

### Saves on disk **[C]**

`profile/save/<account uid>/` holds `account.sav`, `option.sav` and `slot_00` to `slot_05`, each with `progress.sav`
and `caption.sav`. The game data list's `MetaData` names them. Master Quest uses slots 6 to 11.

Every file has the same layout:

- the magic `0x01020304`, a version (`0x002d80eb`) and the data offset;
- zeros up to 0x28;
- `{u32 name hash, u32 value}` pairs up to the data offset. A pair `{0, k}` starts the section of type k: 0 Bool,
  2 Int, 4 Float, 6 Enum, 10 Vector3, 16 String64, 24 UInt64, and so on (the game data's type order);
- the data area.

Bool, Int, Float, Enum and UInt values are inline. Vector3, String64, Int64 and UInt64 values are offsets into the
data area. No checksum was found **[I]**: the header carries none, and the last pair holds the type hash of the file.

`caption.sav` is the file-menu card: `CaptionData.Date.Year` (Int 9811a3f7), `.Month` (dfd840d3), `.Day` (bd46f485),
`.Hour` (23f3d75e), `.Minute` (27853bf7) and `.Second` (5ee50bed), plus the place name, hearts and the picture. The
game's "latest" slot (`SeqQueryGetLatestSaveIndex`, main+0x33ad5d0) sorts the cards by that date. Nothing stores
the last slot used.

`runtime/fs_host.c` serves the newest committed save through a working copy (`save-tx/work`) and commits by
renaming. A save import (`save-import`, `tools/capture_run.py`) becomes the save at the next start.

## What the setting does, and why there

Three points could force the change from the title to play:

1. **Press through the menus** (`bugcap.c` replays do this). It works without new code, but the title and the file
   menu show for some seconds. The slot depends on the list's cursor, and the timing on how fast each screen answers.
2. **Change game data from the game's thread** (the pad read, `debug_tools.c`): `SelectedSavePathIndex` before the
   load, then `Sequence_CurrentBanc` and `SavePos` after it. The window after the load is the problem. The save is
   loaded inside `ChangeBanc`, and `LoadBanc` may read the area in the same frame. A write from the next pad read
   may come after the area is chosen. It also needs the setters of struct members and vectors, which were not
   traced.
3. **Change what the game reads.** The graphs and the save are files that the runtime serves (`romfs.c`,
   `fs_host.c`). Changing a few bytes of them as they are read leaves every step to the game's own code, in the
   game's own order, with no timing.

The setting takes the third way. While a boot map is pending:

- **`Sequence/NormalTitle.module.ainb`**: the command `Root` starts node 42 instead of node 35. That is the
  *Continue → Yes* sequence (`RequestPause`, `TitleToMainWithLoadSave`), so `Title_00`, which `OpenTitle` left
  invisible, never shows. The file menu is never opened. The patch is two bytes: the command's node index at file
  offset 0x88.
- **`Sequence/OnLoadGameSaveData.module.ainb`**: the `Index` input of `SeqExecuteLoadSaveData` and of
  `SeqOneShotSetCaptionMgrLatestIndex` is unlinked from the `SelectedSavePathIndex` query. It becomes a constant,
  the slot: node -1, flags `0x800000` as every unlinked input in the graphs has, value N.
- **`slot_NN/progress.sav`**, as the game reads it:
  - `Sequence_CurrentBanc` := the area;
  - `PlayerStatus.SavePos` := the start position's `Trans`;
  - `PlayerStatus.SavePosRadY` := the yaw of its `Rot`. With R = Rz·Ry·Rx, the yaw is atan2 of the forward vector's
    x and z **[I]**: the axis order was not traced;
  - `DungeonRoom` := -1, so that the save's dungeon room does not go with Link to another place **[I]**;
  - `Sequence_IsChildhood` := the age, when one is asked for or the area fixes it;
  - the low word of ef10b80f := the time of day, when one is asked for.

  The file on disk does not change.

`romfs.c` and `fs_host.c` change only the bytes the game reads, and only while the boot map is pending. The first
pad read with Link in the world ends it. The log line names the area he is in, and the patches stop. The setting
patches only the original RomFS files. If a mod or a RomFS overlay serves one of those files, the setting refuses and
the game boots normally.

Each patch checks the bytes it replaces before it is used:

- The AINB patches find their nodes by name and type in the file: the command `Root`, node 35 a `Simultaneous`, the
  `Sequential` whose children are the pause and `TitleToMainWithLoadSave.module`, and the two nodes' linked `Index`
  inputs. A file that differs from 1.1.1 in any of those makes the setting refuse, with the log line above.
- The save patch finds each value by its name hash in the file's own sections. A save without one of the five values
  makes the setting refuse at that read, and the game then loads the save as it is. The log says so.

Area, start position and coordinates come from the player's own `StartPos.byml` (`debug_tools.c`, read at start).
None of the game's lists is kept in the code.

### A new game instead of a save

A new game is the title's node 13 and `Opening.module`. That path plays the introduction and always goes to
`KokiriForest_Child`, the area named by the root's `FirstBancParam`. Booting to a map from it would mean patching
the root's string and skipping the introduction's boot event. The setting does not do that. It needs a save, and
without one it says so and boots normally.

## Risks

- **Untested.** This was written from the game's files and code. Nothing was run. The first run settles what the
  checklist below asks.
- **The save changes after the game saves.** The game saves the patched values as the real ones. The area change
  ends with an autosave **[I]**, and from then on the slot holds the new area, age and time. Use a copy of the
  profile, as a capture replay does (`tools/capture_run.py` makes one).
- **The patched graphs may stay loaded.** If the game reads the graphs once and keeps them, going back to the
  title later in the same session goes straight back into the save. Loading another slot from the title then loads
  the boot map's slot. The setting is meant for single test runs.
- **Skipped steps of the title.** Node 35 also sets `UI_Debug_IsOpenedTitle`, loads and closes `SystemOverlay_00`,
  and runs `SeqOneShotOnTitleBancOptimize`. The list and the check run `OnLoadList` / `OnLoadCheckBancOptimize`. These
  look like preloads that make the next load faster **[I]**; skipping them should cost only time. The steps of
  `LoadCheck`'s *Yes* are also skipped (its loading fade, `ChangePreGamePause`, `GamePauseOn`).
  `PrepareScreensOfLoading` in `TitleToMainWithLoadSave` opens its own loading fade.
- **The facing.** The yaw formula is inferred. Link may face another way than at the start position.
- **A save that never reached the age asked for.** Without a sword or tunic of that age, Link may be missing
  equipment, or the game may refuse something. The overworld areas fix the age, so this concerns dungeons.
- **Other saved state stays the save's**: the map position (`World_PlayerPos`, Vector3 8773f02d), the warp
  points, the flags and the items. A place reached early with a late save (or the reverse) may play events in an
  order the game never meant.
- **No checksum was seen in the save.** If one exists, the game would refuse the patched save. The log would then
  show a load that does not arrive, with no arrival line.
- **Master Quest:** the game adds 6 to the slot when the account's Master Quest mode is on. The setting reads
  `IsLastSelectedMQMode` in `account.sav`: then it picks the latest among `slot_06` to `slot_11`, and it checks and
  patches `slot_<N+6>`. The +6 also needs a second byte of the save manager (`+0x151`), which was not traced. A
  Master Quest account is untested.

## When the build is allowed: what to run

1. `python tests/boot_map_check.py` (docs/building.md, *Checks*) and `python tests/cfg_rows_check.py`.
2. Build the game as usual (`tools/build_game.py`). The patch file is unchanged, so the translator's output is the
   same.
3. Without the setting, a normal start: the log has `boot map: off`, and the title, the file menu and a save load
   behave as before.
4. On a copy of the profile (`--profile`), with a save in slot 0 to 5, in the bench's run without a window:

   ```sh
   SDL_VIDEODRIVER=offscreen SDL_AUDIO_DRIVER=dummy NX2SARIA_AUDIO=none \
     NX2SARIA_BOOT_MAP=KokiriForest_Child timeout 150 ./run.sh linux-test -- --profile <copy>
   ```

   The log should have `boot map: on: ...`, then `... progress.sav is read with the boot map's place ...`, then
   `boot map: arrived in KokiriForest_Child ...` a few metres from the start position. A `NX2SARIA_CAPTURE_AT` shot,
   or the window, shows that no title or file menu came first.
5. The same with a start position (`HyruleField_Child:HyruleField_FrontOfMarket`), with an adult area
   (`HyruleField_Adult`) from a child save, with a dungeon and an age (`InsideTheDekuTree`, `boot_map_age = child`),
   with `boot_map_time = 21:00` (night), and with `boot_map_slot` set to each slot that has a save.
6. Check the facing: Link should face the way the area's start position faces (the yaw formula is inferred).
7. The refusals: an unknown area, an unknown start position, an empty slot, `boot_map_time = 25:00`. Each should give
   one line and a normal title.
8. Go back to the title from the pause menu after a boot map run, and note what happens (see *Risks*: the graphs
   may stay changed).
9. After each run, compare the profile copy's slot with the original. The slot changes only after the game's own
   save, and then it holds the boot map's place.
