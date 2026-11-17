# Boot to a map

`boot_map` starts the game straight in a chosen area and start position from the save of a chosen slot. The title screen
and its file menu never show. It is a test and debugging setting: a run without a window gets to the place it
tests in one area load, and nothing has to press through menus. Code: `runtime/boot_map.c`. Ocarina of Time 1.1.1 only.
On other game versions the setting is ignored, and the log says so.

This page records how the game goes from its title to play, what the setting changes and why there, how to use it
for tests and benchmarks, and what was seen in the first runs. The research was read from the 1.1.1 game files and
code (build id `e0f3ccfa`). **[C]** marks what was read in the files or the code, or seen in a run. **[I]** marks
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

### Controlled runs: tests and benchmarks

A boot map loads a save and then the game saves the new place into it (*Risks*), so a run that should be the same
every time starts from a fixed save on a throwaway copy of the profile. `tools/run_game.py` does that: it copies the
game folder's profile, puts the save in it (`--save`: a `profile/save-backup/<time>` folder, a save folder or an
exported save `.zip`), runs the game for a given game time and deletes the copy:

```sh
python tools/run_game.py <game folder> --save <backup> --boot-map HyruleField_Child:HyruleField_FrontOfMarket \
    --boot-map-time 21:00 --seconds 10 --wall 180 --tag field-night
```

- `--boot-map`, `--boot-map-slot`, `--boot-map-age` and `--boot-map-time` set the four keys.
- The input script defaults to `tools/input/boot_map.txt`, which presses nothing: the title's A presses of
  `load_save.txt` are not needed, and an A on arrival could start a conversation.
- `--seconds` counts game time from the save's first frame, which is the arrival here.
- The run's log goes to `<game folder>/logs/<tag>.log` (`--log` names another); the boot map's lines are printed at
  the end, and the exit code is 3 when there is no `boot map: arrived in <area>` line for the area asked for.
- Without a window or sound on Linux, put `SDL_VIDEODRIVER=offscreen SDL_AUDIO_DRIVER=dummy NX2SARIA_AUDIO=none`
  in front of it.

`tools/bench.py run ... --boot-map` starts each benchmark scene this way: the scene's `warp` step becomes the boot map
(`docs/performance.md`).

The same setting by hand, on a copy of the profile: `NX2SARIA_BOOT_MAP=<area>[:<start position>]` in the
environment, or `boot_map = ...` in `nx2saria.ini`, and the game started with `--profile <copy>`.

### What the log says

Each start writes one line, whether the setting is on or not:

```
boot map: off
boot map: on: HyruleField_Child, start position HyruleField_FrontOfMarket at 54.2 150.3 63.9 (yaw 3.14), slot 2 (asked for), age child (the area's), time from the save
```

When Link appears in the area, a second line gives where he arrived, the slot, and the time since the process started:

```
boot map: arrived in HyruleField_Child (asked HyruleField_Child), slot 2, Link at 54.21 150.31 63.88, 0.0 m from the start position, 21.4 s after the start
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

More lines can come in between. In Master Quest mode, after the first line:

```
boot map: Master Quest mode: the game loads slot_08
```

When the game opens the slot's save (5 values here: the area, the position, the heading, the dungeon room and the
area's age; a time asked for makes 6):

```
boot map: <mount>:/slot_02/progress.sav is read with the boot map's place (5 values changed in what the game reads)
```

When the save the game opens has changed since the start and lacks a value now, or its values moved (a commit in
between), it loads as it is:

```
boot map: <mount>:/slot_02/progress.sav lacks a value this setting changes now (no PlayerStatus.SavePos): the game loads it as it is
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
- The save patch finds each value by its name hash in the file's own sections. `Sequence_CurrentBanc`,
  `PlayerStatus.SavePos` and `PlayerStatus.SavePosRadY` are always needed, `Sequence_IsChildhood` when an age is
  applied (the area's or the one asked for) and the clock when a time is asked for; `DungeonRoom` is changed when the
  save has it. A save without a needed value is refused at start (normal boot), and at the game's read the save then
  loads as it is. The log says which.

Area, start position and coordinates come from the player's own `StartPos.byml` (`debug_tools.c`, read at start).
None of the game's lists is kept in the code.

### A new game instead of a save

A new game is the title's node 13 and `Opening.module`. That path plays the introduction and always goes to
`KokiriForest_Child`, the area named by the root's `FirstBancParam`. Booting to a map from it would mean patching
the root's string and skipping the introduction's boot event. The setting does not do that. It needs a save, and
without one it says so and boots normally.

## Runs

The first runs, on Linux (RX 6700 XT), without a window or sound, each on a copy of the profile, with the game's
build of `d52a663c`. The account had six slots, all in `WaterTemple`; slot 2 was the latest and adult. Every boot map
run showed black loading frames and then the area: no title screen and no file menu (a picture every few seconds).

| Settings | Log | Seen |
|---|---|---|
| `boot_map = KokiriForest_Child` | `boot map: arrived in KokiriForest_Child (asked KokiriForest_Child), slot 2, Link at -46.09 118.44 15.39, 0.0 m from the start position, 19.6 s after the start` | child Link in his house, the area's name shown on arrival |
| `boot_map = HyruleField_Child:HyruleField_FrontOfMarket`, `boot_map_time = 21:00` | 6 values changed; `arrived in HyruleField_Child ... 0.1 m from the start position, 18.4 s after the start` | night, child Link on the drawbridge in front of the market |
| `boot_map = InsideTheDekuTree:Entrance`, `boot_map_age = child`, `boot_map_slot = 5` | 5 values changed; `arrived in InsideTheDekuTree ... slot 5 ... 0.0 m from the start position, 15.5 s after the start` | child Link inside the Deku Tree, map floor A1 |
| `boot_map = HyruleFeld_Child` | `boot map: HyruleFeld_Child is not an area of the game's start positions (StartPos.byml): normal boot` | the title screen |
| none | `boot map: off` | the title screen |

The first run found a bug: a second open of the save was compared with the first by whole edits, and the unused
bytes of each made them differ, so the game loaded the save as it is (`arrived in WaterTemple ... NOT the area asked
for`). Fixed in `d52a663c`. The game writes `logs/nx2saria.log` unless its stderr is a file already (`2> file`); a
run whose output goes through a pipe (`2>&1 | cat > out`) had all three lines in `logs/nx2saria.log`:
`boot map: on: ...`, `... progress.sav is read with the boot map's place (5 values ...)` and `boot map: arrived in
KokiriForest_Child ... 0.0 m from the start position, 15.5 s after the start`.

Through the tools, from the save backup `20261116-185859-539` (its latest slot is 3): `run_game.py --boot-map
KokiriForest_Child --seconds 15` arrived 0.0 m from the start position 15.7 s after the start and exited with 0;
`bench.py run --only kokiri --runs 1 --boot-map` booted to `KokiriForest_Child:ReturnFromDreamWorld` (arrived 0.0 m,
16.0 s) and measured the scene as usual. With `NX2SARIA_TRACE=fs` the game opened every `Sequence/*.ainb` once, 0.9
to 3.0 s after the start, and none again.

## Risks

- **Tested on Linux only, on one account's saves.** *Runs* above lists what was seen. Windows and Master Quest are
  not tried yet.
- **The read path is not a risk.** Every read of a game file reaches the changes **[C]**. The game reads files only
  through `nn::fs::ReadFile`, and the runtime replaces all three overloads of it (`fs_host.c` `ov_read_file`,
  `ov_read_options` and `ov_read_exact`). Each goes to `read_file`, which hands a RomFS handle to `romfs_read` and
  applies a save handle's changes itself.
- **The save changes after the game saves.** The game saves the patched values as the real ones. The area change
  ends with an autosave **[I]**, and from then on the slot holds the new area, age and time. Use a copy of the
  profile, as a capture replay does (`tools/capture_run.py` makes one).
- **The changed graphs stay loaded for the session.** The game reads every sequence graph once, at boot **[C]**
  (*Runs*), so the title's and the slot's graphs keep the boot map's changes after the arrival. Going back to the
  title later in that session goes straight into a load of the same slot again, from the place the game last saved
  in it, and no other slot or new game can be chosen **[I]**. Start the game again for a normal title. The setting is
  meant for single test runs.
- **Skipped steps of the title.** Node 35 also sets `UI_Debug_IsOpenedTitle`, loads and closes `SystemOverlay_00`,
  and runs `SeqOneShotOnTitleBancOptimize`. The list and the check run `OnLoadList` / `OnLoadCheckBancOptimize`. These
  look like preloads that make the next load faster **[I]**; skipping them should cost only time. The steps of
  `LoadCheck`'s *Yes* are also skipped (its loading fade, `ChangePreGamePause`, `GamePauseOn`).
  `PrepareScreensOfLoading` in `TitleToMainWithLoadSave` opens its own loading fade.
- **The facing.** The yaw formula is inferred from the start positions' rotations; the runs' pictures showed Link
  facing a plausible way, not a measured one.
- **A save that never reached the age asked for.** Without a sword or tunic of that age, Link may be missing
  equipment, or the game may refuse something. The overworld areas fix the age, so this concerns dungeons.
- **Other saved state stays the save's**: the map position (`World_PlayerPos`, Vector3 8773f02d), the warp
  points, the flags and the items. A place reached early with a late save (or the reverse) may play events in an
  order the game never meant.
- **No checksum was seen in the save.** Three slots of different content have the same bytes where a checksum
  could sit: the header is zero from 0x0c to 0x28, and the last pair and the file's last 16 bytes are equal in all
  three. A sum over the content would differ from slot to slot. A check the game computes and keeps elsewhere is not
  ruled out. It would refuse the patched save, and the log would show a load with no arrival line.
- **Master Quest:** the game adds 6 to the slot when the account's Master Quest mode is on. The setting reads
  `IsLastSelectedMQMode` in `account.sav`: then it picks the latest among `slot_06` to `slot_11`, and it checks and
  patches `slot_<N+6>`. The +6 also needs a second byte of the save manager (`+0x151`), which was not traced. A
  Master Quest account is untested.

## Checking a change to it

1. `python tests/boot_map_check.py` (`docs/building.md`, *Checks*), `python tests/bench_check.py` and
   `python tests/cfg_rows_check.py`.
2. Without the setting the log has `boot map: off` and the title comes as before.
3. `tools/run_game.py --boot-map` (above) for an overworld area, a start position with `--boot-map-time 21:00`, a
   dungeon with `--boot-map-age child`, and another `--boot-map-slot`: each exits with 0 and prints its arrival line.
   `NX2SARIA_SHOT_DIR` with `NX2SARIA_SHOT_EVERY=2` shows no title or file menu before the area.
4. An unknown area, an unknown start position, an empty slot or `boot_map_time = 25:00` gives one line and the normal
   title (`run_game.py` exits with 3: no arrival).
5. Not yet run: Windows, a Master Quest account.
