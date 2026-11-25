# runtime/diag

The diagnosis tools of the runtime: frame timing and the frame-time summary, the performance overlay's numbers, the
sampling profiler, the flight recorder, bug captures, the stutter map, the hang watch, the debug menu's access to the
game, the tracker API, the system info text and a few probes. None of it changes what the game does unless a tester
asks (the debug menu, a replay of a bug capture, a test script). The feature docs:

- [bug-capture.md](../bug-capture.md): what F8 writes, a test run from a capture, the flight recorder and its triggers.
- [performance.md](../performance.md): the benchmark, the `perf:` and `frametime:` lines, the stutter map, the hang
  watch, the test steps of `NX2SARIA_DBG_TEST`.
- [tracker-api.md](../tracker-api.md): the tracker API's protocol, safety, cost and log;
  [hyrule-tracker.md](../hyrule-tracker.md): the tracker it was made for.
- [controls.md](../controls.md#renderdoc-capture-blocked): the RenderDoc capture and why it is blocked.
- [logging.md](../logging.md): the prefixes this folder logs with (`bugcap:`, `debug:`, `flight:`, `floor watch:`,
  `frametime:`, `hang watch:`, `mem:`, `perf:`, `prof:`, `rdoc:`, `stutter`, `tracker:` and the others in its table).

"The settings file" on this page is the selected one, `nx2saria.ini` or the older name that still works
([branding.md](../branding.md)).

## `runtime/diag/bugcap.h`

Bug captures: F8 writes what is needed to find a bug again where the player saw it, as one folder under the log
folder, and `NX2SARIA_CAPTURE_REPLAY` starts a run at a capture's place with its camera and its settings. The folder's
files and the steps: [bug-capture.md](../bug-capture.md).

### `bugcap_frame()`

Called by `runtime/diag/perf.c` `perf_present()`, every present on the render thread: one record into the ring of
frame times, and the triggers (F8 while the game window is in front, `NX2SARIA_CAPTURE_AT=<frame>[,<frame>...]`, a
replay's own capture).

### `bugcap_note()`

The note on screen after a capture (`runtime/ui/menu.cpp`, the present path): 1 while it shows, with the capture's
number, whether it was written, and its folder's name ("003-20261113-141502").

### `bugcap_replay_dir()`, `bugcap_replay_pad()`, `bugcap_replay_tick()`

A replay (`NX2SARIA_CAPTURE_REPLAY=<a capture's folder>`). `bugcap_replay_dir()`: the folder, or NULL without one;
`runtime/cfg/cfg.c` reads the capture's `settings.ini` in place of the settings file then and writes neither. The
other two run on the game's thread at its pad read (`runtime/diag/debug_tools.c` `dbg_pad()` and `dbg_tick()`):
`bugcap_replay_pad()` presses A until the save is loaded, `bugcap_replay_tick()` then teleports to the capture's area,
walks Link to where he stood, puts the free camera where the camera was and takes a capture of its own for the
comparison.

## `runtime/diag/bugcap.c`

What a player's F8 writes, and the run that starts from it.

A capture is one folder, `<log folder>/captures/<number>-<YYYYMMDD-HHMMSS>/` (`NX2SARIA_CAPTURE_DIR` names another
base):

| File | What it holds |
|---|---|
| `capture.txt` | Written last, so a folder that has it is complete. "key = value" lines under [sections]: when, versions, the area, Link's position and rotation, the camera of the frame on screen and the game's own, the window and picture, frame times around the press, the machine, and the settings that differ from the defaults |
| `frame.png` | The presented frame as the window got it, overlay included, without this capture's own note |
| `frames.tsv` | The last `RING` (600: ten seconds at 60 a second) presents before the press and the `POST` (30) after it: frame, interval, replay time |
| `log-tail.txt` | The end of this run's log |
| `settings.ini` | Every setting with a value, as lines of the settings file, without the private ones (`runtime/cfg/cfg_private.h`), and the values the quality preset gave the others (`preset_lines()`): what a replay runs with |
| `NX2Saria-save-*.zip` | The save the game would load next (`runtime/hle/save_share.c`), which a replay starts from |
| `flags.tsv`, `texts.tsv` | The game's on/off flags and its 64-character texts, by the hash of their names |
| `flight/` | The flight recorder's dump of the same moment, when `flight = 1` (`runtime/diag/flight.c`) |

The press is taken on the render thread (`bugcap_frame()`), which reads what cannot wait: the position, the camera of
the frame being shown, the frame times. It asks the present path for the next frame's pixels (`vk_present_grab`) and
the game's camera update for its own choice (`dbg_camera_watch()`), and a thread of its own waits for both, writes the
files and then lets the note show. One capture at a time: a press while one is being written is logged and dropped.

A replay (`NX2SARIA_CAPTURE_REPLAY=<folder>`) is a test run: `runtime/cfg/cfg.c` takes the folder's `settings.ini` in
place of the selected settings file, the environment still on top, and writes no settings file. On the game's thread
`bugcap_replay_pad()` presses A until the save is loaded and `bugcap_replay_tick()` then goes where the capture was
taken with the debug menu's own tools (`runtime/diag/debug_tools.c`): the game's change of area to the start position
nearest to the place, Link's walk to it, the free camera at the captured camera. Link is walked, not put: his position
cannot be written (see `runtime/diag/debug_tools.c` below). At the end the replay takes a capture of its own, into
`<folder>/replays` unless `NX2SARIA_CAPTURE_DIR` says otherwise, whose `capture.txt` names the original and how far
from it Link ended.

State of the file:

- `job`: the capture being taken. Filled by the render thread at the press, then read by the writer; `grab` comes from
  the present path and `post` from the render thread meanwhile, each behind its own count. `seen0[2]`: the game's
  camera updates seen before this capture watched them. `replay_note[400]`: for a replay's capture, what it was a
  replay of and how close it came.
- `busy`: a capture is being taken or written. `ring[RING]` and `ring_n` are the render thread's.
- `note_until`: the note shows until then (`plat_now_ns`, `NOTE_NS` 6 s); `note_number`, `note_ok` and the name are
  set before it.
- `rp_src`: `state` 0 not looked at, 1 a replay, -1 none; `text[16384]` is the replayed `capture.txt`.
- `replay_wants`: the replay's own capture, asked for by the game's thread, with the lines of `replay_note[400]` for
  its `capture.txt`, written before `replay_wants`.

### `replay_source()`

`NX2SARIA_CAPTURE_REPLAY` is read from the environment only: a replay is one run, never a saved setting.

### `put_timing()`

`overlay_fps`, `overlay_1pct_low_fps` and `overlay_frame_ms_max` are written when the performance overlay was on
(`fps > 0`): its numbers of the last second.

### `write_log_tail()`

The end of the log: the last `LOG_TAIL` (256 KiB) bytes from the first whole line on. The game holds the log open for
writing: the C library's `fopen` shares it, where `plat_file_open` is refused. The head line gives the log's own path
with the personal folder written short, like the lines below it.

### `run_key()`

Keys that are of one run and not of the game's settings: what a test run's harness sets (scripted input, shots,
dumps, the window kept in the background). `capture.txt` lists them; `settings.ini`, which a replay runs with, leaves
them out.

### `setting_cb()`

- A key that starts with `CAPTURE_` is a replay's own and is skipped.
- The file's line of a key the environment sets too is skipped: the environment's counts.
- A folder in the account's home is written short (`runtime/log_privacy.c`), as in the log.
- A key set to what it would be anyway (its default) is not listed among those that differ.

### `preset_lines()`

The quality preset (`runtime/cfg/cfg.c`, "quality presets"): its name in `capture.txt`, and in `settings.ini` the
value of each key it can set that the player did not, where that differs from the table's default. A replay runs on
high, which sets nothing (`runtime/cfg/cfg.c` `preset_init()`): its PC's own preset would otherwise give these keys
values of another PC.

### `angles_of()`

A direction as the free camera's angles (`runtime/diag/debug_tools.c`): it looks along (cos p sin y, sin p,
cos p cos y).

### `put_camera()`

First the game's own cameras (`game_camera`, `event_camera`), as its update chose them in the frames after the press;
then, when the capture has it, what the frame on screen was drawn with.

### `writer()`

The frame's pixels come with the next present, the game's camera with its next update, the frames after the press as
they are shown. The writer waits for the pixels until 2 s after the press, then for the `POST` frames until 0.7 s
after the press.
`player_yaw` is `atan2` of the rotation matrix's third column, the way Link faces, as the free camera's yaw counts
it. The system info text's lines ("OS: ...") become "os = ..." keys. `capture.txt` appears last and whole: written as
a part file and renamed.

### `start()`

Reads what the game holds now (the position first): the writer comes later.

### `bugcap_frame()`

After a capture the game's camera update has had its frames (8 presents): the watch ends and the patch site goes
quiet again. `NX2SARIA_CAPTURE_AT=<frame>[,<frame>...]` takes captures without a key, for test runs.

### `src_get()`

The value of a key in the replayed `capture.txt` ("key = value" at a line's start); 0 without it.

### `bugcap_replay_pad()`

`rp_loaded`: Link has appeared, and the function stops pressing A. `rp_held`: Link stands still on his walk (a
conversation holds him), and it presses A again, 4 reads in every 60: a conversation that began where the teleport
arrived (the game's own, on a save that had not been there) is taken to its end by A. Before the load, through the
title screen and the file menu: A for four reads, every hundred, from the fifth second on (`reads > 300`).

### `bugcap_replay_tick()`

The states are `R_NEW`, `R_WAIT`, `R_LIST`, `R_WARP`, `R_ROUTE`, `R_ROUTING`, `R_CAMERA`, `R_CAMERA_PUT`, `R_SETTLE`
and `R_DONE`.

- `NX2SARIA_CAPTURE_REPLAY_CAMERA=0`: the game's own camera stays.
- `R_WAIT`: the save has just loaded, and the game takes no change of area in its first moments (3 s). Then the flags
  are refreshed and the names asked for: the start positions are read with the flag names.
- `R_LIST` runs on into `R_WARP` (the one fall-through of the file).
- `R_WARP`: a refusal (an event still runs after the load) is asked again in a moment (4 s).
- `R_ROUTING`: `still_at` and `still_ns` are where Link was when he last moved, and when. Link not moving at all (less
  than 0.05 m summed over the three axes) for two seconds is no wall (he would slide along it or sidestep): the game
  holds him, for a conversation or an event. A is pressed until he moves again, and the walk is asked for once more
  should the route have given up meanwhile (given up while he was held: at most 3 walks, within 120 s).
- `R_CAMERA`: after a second Link has stopped. The free camera goes on without the sticks (`dbg_freecam_set(1, 0,
  8.0f)`): standing, the pad still plays the game.
- `R_CAMERA_PUT`: three ticks later. The free camera's first update took the game's view; now it gets the captured
  one.
- `R_SETTLE`: 3 s for the picture's history and the area's detail to settle, then the replay's own capture.

## `runtime/diag/debug_tools.h`

The debug menu's access to the game. Everything but `dbg_tick()` is for the menu's thread.

- `dbg_tick()`: the game's thread, at its pad read (`runtime/hle/ov_input.c`): does what the menu asked for.
- `dbg_pad()`: the pad's state before the game gets it: the free camera takes the sticks and four buttons while it
  flies.
- `dbg_freecam_host()`: the free camera's patch site (`recomp/patches-v111.txt`, set `freecam`).
- `dbg_fov_host()`: the gameplay lens's patch site (`recomp/patches-v111.txt`, set `fov`).
- `DBG_GAME_DATA_SLOT` (0x4c7e180): main's slot (1.1.1) that holds where the game-data manager's pointer is: the
  manager is `**[main+this]`. The debug menu reads its flags (`runtime/diag/debug_tools.c` `gmd()`), the tracker API
  its values (`runtime/diag/tracker_api.c`, `nx2saria.game_data`).
- `dbg_supported()`: this game build's structures are known (Ocarina of Time 1.1.1).
- `dbg_player_pos()`: Link's position in metres; 0 while there is no player. `dbg_player_rot()`: Link's rotation, a
  3x3 matrix by rows; 0 while there is no player.
- `dbg_area()`: the area the game is in, by its name in the game's files; 0 unknown.
- `dbg_texts()`: every 64-character text of the game data (the area among them) with the hash of its name; returns
  their number. `dbg_name_hash()`: the hash the game knows a flag or a text by.
- `dbg_speed_factor()`, `dbg_speed_set()`: the game's speed: 1 normal, 0.05 to 8 (`runtime/pacing/frame_rate.c`
  multiplies the game's step).
- `dbg_flags_refresh()`: the game's on/off flags, sorted by hash; it reads their values and returns their number (0
  until a save is loaded). A change is made by the game's own function at the next frame. `dbg_flag_name()`: NULL
  when no name was found for it.
- `dbg_names()`: 0 not started, 1 being read from the game's files, 2 read (`*found` of them).
- Teleporting and events: `dbg_places()`, `dbg_place_pos()`, `dbg_warp()`, `dbg_event_start()`, `dbg_busy()`,
  `dbg_status()`. The places are the game's areas with their named start positions, and the events its event files
  that hold a script: both lists come from the player's game files with the flag names (0 until `dbg_names()` answers
  2). `dbg_warp()` changes area as the game's own doors do; `dbg_event_start()` starts an event by its name. One at a
  time: nothing is taken while `dbg_busy()`. `dbg_status()` is the last thing that happened, in words.
  `dbg_place_pos()` is never "": a teleport without a listed position takes the area's first.
- `dbg_place_nearest()`: the start position of the area nearest to xyz (metres) among those with coordinates: its
  name and distance; 0 none.
- `dbg_start_pos()`: any thread, at once (`runtime/patches/boot_map.c`, before the guest starts): start position
  `pos` of the area read from the game's file, the area's default (its entry 0) for an empty `pos`; `*name` the
  entry's name, `xyz` and `rot` (radians) its Trans and Rot. 0 with why when it is not there.
- The free camera: the game draws from a camera of the menu's instead of its own. With sticks on, the pad flies it
  and Link stands; with sticks off it stays where it is and the pad plays the game. Speed in metres a second.
  `dbg_freecam_available()`: the build has the camera's patch set. `dbg_freecam_reset()`: back to the game's view,
  free again from there. `dbg_freecam()`: on or off. `dbg_freecam_angles()`: radians; it looks along (cos p sin y,
  sin p, cos p cos y). `dbg_freecam_place()`: the game's thread: the free camera, which is on, put at a place and
  direction outright.
- `dbg_camera_watch()`, `dbg_camera_seen()`: the game's own camera, before the free camera stands in for it.
  `dbg_camera_watch(1)` has the game's camera update report it (it does anyway while the free camera is on);
  `dbg_camera_seen()` gives the last position, target and up vector of the camera that follows Link (`which` 0) or of
  the events' camera (1) and how many updates were seen so far.
- `dbg_route_to()`: the game's thread: Link runs to a point of the area (x, z in metres), reached within `near_m`.
  `dbg_route_state()`: 1 on his way, 0 there or no route, 2 given up. `dbg_route_stop()`: the game's thread: the
  route ends where Link is.
- `dbg_freecam_flying()`: on, with the sticks and ground avoidance: `runtime/vk/vk_dlaa.c` reads the ground's
  distances back. `dbg_freecam_avoid_set()`, `dbg_freecam_avoid()`: ground avoidance (off by default): the camera
  stays a metre above ground-like surfaces. `dbg_freecam_ground()`: the replay's thread: nine view distances of the
  lower view (three rows of three, the top row first; 0 sky), with the tangent of half the view's vertical angle and
  its width over its height.
- Memory: values of 1, 2 or 4 bytes in the game's heap. `dbg_search_new()` returns the number of places.
  `dbg_search_next()`: mode 0 equals value, 1 changed, 2 unchanged, 3 greater, 4 less. `dbg_search_count()`: `*over`
  says the first search found more than are kept. `dbg_freeze()`: written every frame while on; 0 when the list is
  full.

## `runtime/diag/debug_tools.c`

The debug menu's hands in the game (`runtime/ui/debug_menu.cpp` is its window, F9 with `debug_menu = 1`). Ocarina of
Time 1.1.1.

Reads go straight to the game's memory from the menu's thread, through `plat_mem_read`, which answers 0 instead of
faulting. Changes are queued and made on the game's own thread at its pad read (`dbg_tick()`, from
`runtime/hle/ov_input.c`), between two of the game's frames: a flag through the game's own function, so everything
that follows a flag in the game follows this change too; a memory value by a plain write into memory the game may read
and write.

**The game's on/off flags ("game data").** The manager is `**[main+0x4c7e180]`; +0x10 the number of flags, +0x18
their entries (0x18 bytes each, the value's byte at +0x16), +0x20 a table of {hash, index} pairs probed from
hash % size (+0x2c the size). main+0x20d9c0 (manager, u8 *out, hash) reads a flag and main+0x682b20 (manager, value,
hash) queues a change, which the manager applies at its next update. A flag is known to the game by the MurmurHash3
of its name only; the names shown are those found in the player's own game files (the event flows), hashed here and
matched (`dbg_names()`), so nothing of them is kept in this code.

**The game's speed.** The runtime gives the game its step every frame (`runtime/pacing/frame_rate.c`
`ocarina_step_write()`, the display interval); the menu's factor multiplies that step, so 0.5 is half speed for the
whole game and its interface.

**Link's position** is shown but not written: it is the actor's copy (`[[main+0x4e371d0]+0x480]+0x31c`), which the
physics writes every frame. Writing it, or every copy of it in the heap, moved Link back within three frames.

**Teleporting** is the game's own change of area, as a loading zone, a warp song or a cutscene makes it: the game's
event script reserves the next area and the name of a start position in it (game data texts), and asks the sequence
that runs during events to change area (the enum `Sequence_ControlInEvent` = `ChangeBancOnEvent`). `warp_tick()` does
the same during a short event of the game that only waits (`DefaultParentEvent`, 30 frames), so the game unloads the
area, loads the next one and places Link exactly as it does for its own doors. The places offered are the ones in the
player's game files (`Banc/BancBasedResource/StartPos.byml.zs`: each area with its named start positions, the warp
songs' pads and both sides of every loading zone among them); none of that list is kept in this code.

**An event** (a cutscene, a conversation, a scripted moment) is started by its file's name through the game's event
manager, as the game's own actors start them.

**The free camera** stands in for the game's camera inside the game's own camera update (`dbg_freecam_host()`), so
the game itself draws from it.

### `wr()`

A write into memory the game may read and write: 0 when the place is not such memory.

### `dbg_player_rot()`

The actor's rotation beside its position: nine floats, a 3x3 matrix by rows (`runtime/diag/bugcap.c` writes them as
they are).

### `ask()`

What the menu asks for is queued here (`Q_FLAG`, `Q_WRITE`; `Q_MAX` 256 requests, `FREEZE_MAX` 64 frozen values) and
done by `dbg_tick()`.

### `flags_list()`

The list of the game's flags from its hash table. 0 until the manager exists (a save is loaded). `flag_pairs`: the
flags by hash, sorted, made once the game's table is there; `flag_name`, parallel: the name found for the hash, or
NULL; `flag_value`, parallel: the value at the last `dbg_flags_refresh()`. `names_state`: 0 not asked, 1 being read,
2 read.

### `dbg_flags_refresh()`

Under `flags_lock`: the menu's thread, the game's thread (`test_script()`, `warp_tick()`) and a bug capture's writer
(`runtime/diag/bugcap.c`) all ask, and the list is made by whichever asks first.

### `murmur3()`

The hash of the flags' names. Its tail switch falls through from 3 bytes to 2 to 1 (the two fall-through markers of
the file).

### `names_in()`

Every run of name characters in a file is hashed and looked up among the flags.

### `sarc_member()`

A member of a SARC archive: its place and size, 0 when it is not there.

### `byml_key()`

The i-th key of a BYML file's key table (a node 0xc2: its count, then the offsets of its texts).

### `byml_vec3()`

Three floats at node `off` of the given kind: an array of three floats (node 0xc0: its count, the kinds of its items
padded to four bytes, then the items; a float is kind 0xd2, its bits in place), or found in a dictionary under the
first key that holds one, a key whose name says where (Translate, Trans, Pos) taken before the others. `depth` bounds
the search. `*key`: the key it was found under.

### `places_in()`

The start positions file: a dictionary (node 0xc1: its count, then entries of a key's number in 24 bits, the value's
kind and the value's offset) of areas, each a dictionary of its start positions. The places and the events come from
the player's game files (`names_thread()`). A `Place` holds `xyz[3]`, where it is in the area in metres, when the
file gives it (`has_xyz`). `places`: by area, as the game's file lists them; the texts stay in the file's buffer.
`place_xyz_key`: the key the positions' coordinates were found under, for the log. `event_names`: the event files
that hold a script, by name.

### `byml_dict_vec3()`

The three floats under key `name` of a dictionary node (0xc1) at `off`: 0 when it has no such array of three floats.

### `dbg_start_pos()`

The start position `pos` of an area in the game's `StartPos.byml`, read here and now (`runtime/patches/boot_map.c`
asks before the guest starts, long before `names_thread()` reads the menu's list). An empty `pos`, or none, is the
area's entry 0: what the game itself takes for an area's default (main+0xb679e0) and for a name the area does not
list. `*name` gets the entry's name ("" for the area's unnamed one), `xyz` its Trans and `rot` its Rot (radians). 1
found; 0 with why: the area, or the named position, is not in the file, or the file cannot be read.

### `names_thread()`

An event file that holds a script (the count at 0x20) can be started by its name. The decompressed start positions
file (under 16 MiB) is kept: the places' texts are in it. With `NX2SARIA_DBG_TEST` set (test runs) every place is
logged with its coordinates, to plan routes (`route_pad()`).

### `dbg_names()`

0 not started (no flags yet), 1 being read, 2 there. Starts the reading once the flags are listed.

### `place_after()`

The start position of an area that comes after `pos` in the game's list, the first one for a `pos` the area does not
list (or none given), NULL for an area without any. A change of area to a start position the area does not have puts
Link where nothing holds him: he falls below the map for good, and the game refuses events while he falls.

### `dbg_place_nearest()`

The start position of an area nearest to xyz among those the game's file gives coordinates for (a bug capture names
it, and a replay of the capture arrives there, `runtime/diag/bugcap.c`). 0 while the list is not read or the area has
none.

### `gmd_entry()`

The game data's other kinds: the manager holds a section of 0x60 bytes for each kind (the kind's number times 0x60:
+0x10 the entries' count, +0x18 the entries, +0x20 the {hash, index} table, +0x2c its size). An entry of a
64-character text (`GD_TEXT64`, kind 16) is 0x108 bytes, the text's pointer at +0x68 (a second one at +0x18) and its
room at +0x70. main+0x3e0980 (manager, value, hash) queues the change of an enum, whose value is the hash of the
value's name.

### `dbg_texts()`

Every 64-character text of the game data with its name's hash (a bug capture lists them, `runtime/diag/bugcap.c`):
the area, the reserved next area and start position among them. Returns how many it visited.

### `dbg_area()`

The area the game is in: its text `Sequence_CurrentBanc`. 0 while the game has none.

### `text_set()`

A text written into the entry's own buffers (game thread). 0: the game has no such text.

### `event_request()`

Events. main+0xd79f10 builds an event request in a block of 0x2000 bytes: the event's name goes into the buffer its
+0x1a90 points to (the room at +0x1a98). main+0xd7a820 (the event manager `**[main+0x4c7e090]`, request) hands it in
and answers whether the event starts; the request's two lists at +0x1968 and +0x1950 are then destroyed
(main+0x20b6f0, their table main+0x4974b20), as the game's own request node does (main+0x32db1a0).

### `warp_tick()`

The teleport and the event start: asked for by the menu, done here on the game's thread. States `W_IDLE`, `W_ASKED`,
`W_EVENT`, `W_FLIGHT`, `W_SETTLE`; `SETTLE_TICKS` 180, `FALL_TRIES` 3, `RETRY_EVERY` 30, `RETRY_FOR` 1200,
`LIST_WAIT` 600.

- `W_SETTLE`: Link arrived and is watched for a while; one who falls `FALL_M` (25 m; an arrival's own drop is a step
  or two) below where he arrived has no ground under him, and the change of area is asked again for the area's next
  start position (`place_after()`), up to `FALL_TRIES` times.
- The game refuses events while Link is in the air, so such a request is tried again every `RETRY_EVERY` ticks until
  `RETRY_FOR` ticks have passed.
- The `warp` state: `area`, `pos` and `event` are what the menu asked for (under `q_lock`); `at` is the tick of the
  next step, or the one at which the wait for the arrival ends; `retry_until` and `waited` are for `W_ASKED`: refused
  requests are tried again until then, and the ticks spent on the list; `from[3]` where Link stood; `land` the height
  he arrived at; `tries` the start positions tried after falls. `status[160]` (under `q_lock`) is the last thing that
  happened, for the menu.
- The area's own list of start positions decides the position: it is read with the flag names, which a test run that
  never opened the menu has not started yet, so the tick waits for it up to `LIST_WAIT` ticks.
- The game's event that only waits (`DefaultParentEvent`): the change of area is asked for while it runs. When the
  request is refused Link still falls: the game takes it once he lands, or the time runs out.
- What the game's own script reserves (its actor's `ReserveBancChange`): the next area (`Sequence_ReservedNextBanc`)
  and the start position's name (`Sequence_ReservedNextStartPosName`), no event at the arrival, the event held during
  the change, a black fade, and the request itself.
- At the arrival `dbg_freecam_reset()`: a free camera left behind in the old area's place starts again from the
  game's view.

### `dbg_search_new()`

The memory search: values of 1, 2 or 4 bytes in the game's heap. A new search: every place in the heap, aligned to
the size, that holds the value. Returns how many (at most `RESULT_MAX`, 2,000,000, are kept; `dbg_search_count()`'s
`over` says there were more). A region that is free, or not the game's to read and write (`perm & 3 != 3`), is
skipped.

### `dbg_search_next()`

Keeps the results whose value now passes: mode 0 equals value, 1 changed, 2 unchanged, 3 greater, 4 less than at the
search before (unsigned; a float is searched by its bits with mode 0).

### `dbg_freecam_host()`

The free camera. The game's camera update (main+0x4c48c0) chooses the frame's position and target (doubles) and up
vector between the camera's own and an override, and at main+0x4c4e50 has them in d0-d2, d3-d5 and s7, s16, s6,
before it stores them into the look-at camera and makes its matrix. The patch set `freecam`
(`recomp/patches-v111.txt`) sends that instruction here while the free camera is on: the registers get the free
camera's vectors, so the game draws, culls and loads detail for the view it is given, in play and in cutscenes, and
Link, the world and the game's own camera work go on as they were. The pad's sticks fly it (`dbg_pad()`, from
`runtime/hle/ov_input.c` before the game gets the pad's state).

The function is the patch site's host function, on the patch page at +0xf70. `runtime/patches/patches.c` registers
it: that file is compiled with the game version's `patches.h`, which has the page's address.

- For a scene camera, what the game chose is kept for a bug capture (`dbg_camera_seen()`); the up vector's registers
  are 7, 16 and 6.
- `fc.capture`: start where the game's camera is, looking where it looks.
- The up vector given is (0, 1, 0).
- At the end `s17 = (float)d0`: the instruction this call stands in for, `fcvt s17, d0`.

The `fc` state: `on`, `capture` and `sticks` are the menu's (on; take the game's view at the next update; the sticks
fly it); `avoid` the menu's ground avoidance (`dbg_pad()`), off unless asked for; `speed` in metres a second (8 at
start); `pos[3]` and `reach` are the game thread's: where it is, and the game's distance to its target (5 at start);
`ground[9]`, `ground_tan` and `ground_aspect` are `dbg_freecam_ground()`'s, under `ground_lock`, and `ground_ns` when
they came (0 none).

`cam_seen[2]`: the game's own camera as its update chose it, before the free camera stands in: kept per scene camera
(0 the one that follows Link, 1 the one events direct) while a bug capture watches (`dbg_camera_watch()`) or the free
camera is on, which is when the patch site calls `dbg_freecam_host()` at all. `cam_watch`: how many watch it: a bug
capture, and Link's walk to a captured place.

### `dbg_freecam_ground()`

The replay's thread (`runtime/vk/vk_dlaa.c` `fcam_probe()`): view distances read back from the lower part of the view
a few frames ago, three rows of three (0.6, 0.78 and 0.96 of the way down; 0.2, 0.5 and 0.8 across), 0 where it saw
the sky. Kept for `dbg_pad()` (`ground_under()`).

### `ground_under()`

How far below the camera the ground is, in metres, from the view distances above, or a negative number when they show
no ground within `reach` metres ahead. Each sample is a point of the scene: its height against the camera and its
distance ahead follow from the view's angles and the camera's pitch. In each of the three columns the lowest sample
counts as ground only when the surface from it to the one above it rises by less than `GROUND_SLOPE` (0.6, rise over
run: about 30 degrees): the ground the camera flies over, a field or a gentle hill. Whatever stands on it or in front
of the camera (a character, a rock, a wall, a tree) is steeper and is left alone, so the camera can go right up to
it; taken for ground too, as at first, anything within three metres in the lower view sent the camera upwards on its
way to it.

The samples in the view, -1..1 with up positive: x -0.6, 0.0, 0.6; y -0.2, -0.56, -0.92. Per row the code has the
point's height against the camera and how far ahead it is, level; a column needs its lowest sample and the one above
it.

### `scene_camera()`

The update runs for each of the game's cameras, every frame: the one that follows Link and the one its events direct
are the scene's, and the free camera stands in for both; the camera of the interface's 3D models and the one the
sound follows stay the game's. A camera's state (x19) has its actor at +0x10, the actor its name at +0x20. It returns
1 for the camera that follows Link, 2 for the events' camera, 0 for another.

### `dbg_fov_host()`

After the original s0 load at main+0x4c52f8, before the game stores the lens angle and derives the frustum.

- The event manager's (`**[main+0x4c7e090]`) active event at +0x68 is the one read by main+0xbcc100.
- The aiming action's initializer (main+0x27a2510) gets the PlayerCamera work at component +0x5b8. Its enter and exit
  methods (main+0x279f9a0 and main+0x27a1230) toggle bit 8 of work +0x7d8.
- Camera zero's bit 0 at +0x88 stayed set in gameplay, so it cannot identify the message log.

### `wide_lens_host()`

The `wide_aspect` set shares the lens-load site with the configurable gameplay FOV. It applies that setting first,
then opens the vertical angle for any 16:9 camera when the chosen picture is narrower, including cutscenes and
events.

### `cam_patch_sync()`

The patch site calls `dbg_freecam_host()` while the free camera is on or a bug capture watches the game's camera.

### `dbg_freecam_place()`

The game's thread: the free camera, which must be on, at a place and direction given outright (a replay of a bug
capture, `runtime/diag/bugcap.c`), instead of where the game's camera is.

### `dbg_freecam_set()`

Logs `debug: free camera on` or `off` when it changes: what a tester changed, for the log.

### `contains_nocase()`

Whether `s` holds the `len` characters at `part`, in any case.

### `route_pad()`

The route (test runs: `NX2SARIA_DBG_TEST` `route` and `routeto`): Link runs through points of the area on his own, so
a walking test crosses the field instead of pressing into its walls. The left stick is turned from the way he moves
towards the next point (the game's camera follows behind him, so a stick to the side turns him), with an integral
that takes out the camera's own angle; a point counts as reached within `ROUTE_NEAR` (4.0) metres. When he gains less than
a metre on it in 2.5 seconds he is stuck (a wall, a fence, a steep slope): he sidesteps for two seconds, three times
to one side and then to the other, and after `ROUTE_STUCK` (10) such tries the route ends.

A route asked for with a smaller distance (`dbg_route_to()`, a replay of a bug capture) walks the last three metres,
so that Link stops on the point instead of running rings round it, and is steered by the game's own camera
(`dbg_camera_seen()`): the stick moves Link along the camera's view and across it, so the stick's angle is the angle
between the view and the way to the point, with nothing to estimate. In a room that matters: the estimate starts with
a run straight ahead, which in Link's house put him on the furniture beside the door, three metres from the point,
for good.

The function runs on the game's thread: the left stick while a route runs. Stick right turns Link right, which lowers
atan2(x, z) (the free camera's own frame: ahead (sin yaw, cos yaw), right (-cos yaw, sin yaw)). The stick's angle
from straight ahead, right positive, is `-(1.2 * err + integ)`. By camera, the view is taken level: ahead (fx, fz),
right (-fz, fx), as the free camera's frame has them, and only while the camera was seen in the last 250 ms. The
sidestep takes the same side three times before the other: a wall is gone round, where turns to alternate sides kept
him pressed against a cliff face south of the Kakariko Village gate.

The `route` state (at most `ROUTE_MAX`, 16, points): `by_camera`: steered by the game's camera, which is watched
meanwhile (`dbg_route_to()`); `cam_n`, `cam_ns`: the camera updates seen at the last look, and when their number last
grew; `within`: metres, a point counts as reached within them; `pt[][2]`: x, z in metres; `heading`, `integ`:
radians, the way he moved last and the integral of the turn.

### `route_start()`

Ends the camera watch of a walk that this route replaces.

### `dbg_route_to()`

The game's thread: Link runs to one point of the area, reached within `near_m` metres. `dbg_route_state()`: 1 on his
way, 0 there (or no route), 2 given up (stuck).

### `dbg_pad()`

The game's thread, with the pad's state before the game gets it. First `bugcap_replay_pad()` (a replay of a bug
capture presses A until the save is loaded) and `route_pad()`. Then, while the free camera has the sticks, the left
one moves it where it looks and sideways, the right one turns it, ZR and ZL raise and lower it, R makes it four times
as fast and L four times as slow, and the game is told that none of those is touched, so Link stays where he is.
Nothing stops it: it goes through walls, objects and the ground, below which the picture shows the void under the
map. Ground avoidance (the menu's switch, off unless asked for) keeps it a metre above ground-like surfaces instead.

`look` is where it looks, (cos p sin y, sin p, cos p cos y); `right` is the view's direction crossed with up,
(-cos y, 0, sin y).

Ground avoidance, when it is on: where the ground under the view (`ground_under()`, from `runtime/vk/vk_dlaa.c`
`fcam_probe()` with DLSS, DLAA or FSR on) is less than a metre below, the camera goes no lower and rises to that
metre, instead of going on into a field or a gentle hill and under the map. Objects and walls do not count. The reach
ahead grows with the speed: the distances are a few frames old.

### `fly_tick()`

`camfly` moves the free camera as `camgo` does, in equal steps over that many ticks: a flight across an area for a
run that measures what the moving view costs (video memory, frame time) without a route Link can get stuck on. A
`camfly` that comes while another runs takes over from where the camera is. The `fly` state is per tick: `d[3]`,
`yaw`, `pitch`, and `left`, the ticks to go.

### `step_due()`

A step written `@<ms>:` in place of `<tick>:` waits for the world clock instead (game time since the save's first
frame, `frame_step_world_us`): pad reads come as often as frames, so a tick is a different moment at another frame
rate, and a benchmark (`tools/bench.py`) needs its teleport and route at the same game time as its input script's
marks.

### `test_script()`

`NX2SARIA_DBG_TEST` holds the menu's requests without the menu, for test runs, as steps separated by `;`:

```
<tick>:flag <hash hex> <0|1>
<tick>:speed <factor>
<tick>:find <value> <size>
<tick>:next <mode> <value>
<tick>:setfirst <value>
<tick>:holdfirst <value>
<tick>:show [<hash hex>]
<tick>:warp <area> [<start position>]
<tick>:event <name>
<tick>:text <game data text's name>
<tick>:freecam <0|1> <sticks 0|1> <speed>
<tick>:camavoid <0|1>
<tick>:camgo <dx> <dy> <dz> [<yaw> <pitch>]
<tick>:camfly <dx> <dy> <dz> <yaw> <pitch> <ticks>
<tick>:camreset
<tick>:campos
<tick>:route <x> <z> [<x> <z> ...]
<tick>:routeto <part of a start position's name>[/<another part>] ...
```

Ticks count the game's pad reads from the player's first appearance. `camgo` moves the free camera by hand. `route`
takes points in metres; `routeto` takes the area's own start positions, in order, each named by a part of its name,
with the parts between slashes as alternatives. The places and events are read with the flag names as the menu
starts them, for `warp`, `event` and `routeto`.

### `pos_trace()`

`NX2SARIA_POS_TRACE=<file>`: Link's position at every tick of the game's update, one line each
("<tick> <ms> <x> <y> <z>", the milliseconds of the host clock from the first line), for movement checks at different
frame rates (a jump's height and length, a climb). A developer switch; nothing else changes.

### `dbg_tick()`

The game's thread, at its pad read. `bugcap_replay_tick()` comes first (a replay of a bug capture: the teleport, the
walk and the camera, `runtime/diag/bugcap.c`), then `warp_tick()`. A flag change logs `debug: flag <hash> set to on`
or `off`: what a tester changed, for the log.

## `runtime/diag/dpi_probe_win.c`

`--probe-dpi` (Windows only): the process DPI awareness that `nx2saria.manifest` declares, and what the primary
monitor reports under it. A DPI-unaware process gets every window and monitor size scaled down by the display scale
and its output stretched back up by Windows; the probe (`probe_dpi()`) fails unless the process is per-monitor aware
(v2). Needs no game files and no GPU.

## `runtime/diag/flight.h`

The flight recorder (`NX2SARIA_FLIGHT=1`): the last frames' timing and event counts plus a thumbnail each
(`FLIGHT_THUMB_W` x `FLIGHT_THUMB_H`, 640x360), written to disk with a bug capture (F8, `runtime/diag/bugcap.c`),
after a slow frame or after a one-frame flash, so a glitch or stutter seen in play can be read after the fact. The
triggers for a reader: [bug-capture.md](../bug-capture.md#the-flight-recorder).

`flight_on` is `NX2SARIA_FLIGHT=1`, set once by `flight_init()` before the first present. `vk_dump_white` is
`runtime/vk/vk_dump.c`'s, `NX2SARIA_DUMP_WHITE`.

### `flight_init()`

`runtime/diag/perf.c` `prof_start()` calls it.

### `flight_mem()`

Its memory in bytes, for the memory report (`runtime/vk/vk_memory.c` `system_log()`): the records and texture events
in process memory, and the thumbnail ring in host-visible memory. 0 while it is off.

### `flight_frame()`

From `runtime/diag/perf.c` `perf_present()`, once per present on the render thread: the interval since the previous
present, the render thread's blocked time per `PERF_*` kind in this frame, and the running `PERF_CNT_*` totals.

### `flight_capture()`

A bug capture at this frame (the render thread, before or after `flight_frame()`): the recorder dumps the frames
around it into `dir`. 0 when the recorder is off or busy with another dump.

### `flight_tex()`

Texture events of the recorded frames (`runtime/vk/vk_texture.c`), written with each dump as `events.tsv`. Each
carries the frame whose counts in `frames.tsv` it is part of. Any thread: the ring takes no lock. A no-op until
`flight_init()` turned the recorder on.

| Kind | Event |
|---|---|
| `FLIGHT_TEX_NEW` | a texture made (with how its content started) |
| `FLIGHT_TEX_UP`, `FLIGHT_TEX_REUP` | uploaded, uploaded again |
| `FLIGHT_TEX_RELEASE`, `FLIGHT_TEX_TAKEBACK` | released by the game, taken back after a release |
| `FLIGHT_TEX_DROP` | destroyed |
| `FLIGHT_TEX_BYTES` | after UP and REUP: what the upload read (`detail`: a hash of 64 probes of its storage; `us`: how many of the probes were all zero), so a second upload tells new bytes from the same |
| `FLIGHT_TEX_REG` | a slot registered with a CPU-written texture (`us`: the slot; `detail`: what it named before: 0 nothing, 1 another descriptor at this storage, 2 other storage, 3 a finalized texture) |
| `FLIGHT_TEX_BLANK` | a shader sampled a slot that has no image of its own and follows none: its stand-in was on screen (`us`: the slot; `detail`: bit 0 the slot is finalized, bit 1 it is held out) |
| `FLIGHT_TEX_TOBLANK` | the pool's entry of a slot went from a texture's view to the stand-in (`us`: the slot; `detail`: `vk_texture_slot_why_locked`'s bits, what the slot's own texture was then) |
| `FLIGHT_TEX_BLANKSUB` | a slot that lost its picture during a submission still had none when the next one began: the submission's draws read its stand-in (`us` and `detail` as TOBLANK) |
| `FLIGHT_TEX_DIRECT` | a draw's own texture binding (not the pool's) named a slot and got the stand-in: that draw samples it whatever the slot resolves to later (`us` and `detail` as TOBLANK; once a frame) |
| `FLIGHT_TEX_UNREG` | a shader sampled a pool slot that is not registered when its marks are read (`us`: the slot) |

The texture's flags (`FLIGHT_TF_*`): a depth or stencil image, mirrored (the CPU writes its storage), bound into pool
memory, an ASTC texture, written by the GPU (`VkTexImage.gpu_owned`).

`detail`: for NEW how the content started (`vk_texture_start_name()`), for UP and REUP the WriteTexels calls since
the last upload, for DROP why (`vk_texture_drop_name()`). `us`: an upload's time, else 0. The two name functions are
`runtime/vk/vk_texture.c`'s: the names of `Tex.og_start` and of `drop_tex`'s reasons.

### `flight_write_png()`

A w x h RGBA picture as a compressed RGB PNG (Paeth rows, fixed-Huffman deflate); any thread, recorder on or off.

### `vk_thumbs_enable()`, `vk_thumbs_frame()`, `vk_thumbs_read()`

`runtime/gfx/gfx_present.c`: the present thread blits every presented frame into a host-visible ring of n thumbnails.

- `vk_thumbs_enable()`: before the first present; n slots of `FLIGHT_THUMB_W` x `FLIGHT_THUMB_H`.
- `vk_thumbs_frame()`: the present thread names the frame of the `vk_present_image` call that follows.
- `vk_thumbs_read()`: copies the frame's thumbnail as RGBA when the ring still holds it complete; 0 when it was never
  written, is still in flight or was overwritten. Takes the present lock for one slot copy (about 20 us); any thread.

### `flight_cells()`

The flash trigger (`runtime/diag/flight_flash.h`): while `flight_flash_on` (set once by `flight_init()`),
`runtime/gfx/gfx_present.c` hands in the cell means (`flash_cells()`, `FLASH_CELLS` bytes) of each thumbnail when it
completes, on the present thread, in frame order. `flight_cells()` judges the frame before and leaves a flash for
`flight_frame()` to dump.

## `runtime/diag/flight.c`

The recorder is a ring of the last `NX2SARIA_FLIGHT_FRAMES` (600) presents, one record each (interval, render-thread
blocked time per perf kind, event counts, the game's time step) and a 640x360 thumbnail each
(`runtime/gfx/gfx_present.c`, blitted on the GPU by the present thread). A dump goes to
`<dir>/<YYYYMMDD-HHMMSS>-f<frame>/`: `events.tsv` (the texture events of its frames, `flight_tex()`), `f<frame>.png`
per thumbnail and `frames.tsv`, written in that order by a background thread. Triggers:

- a bug capture (F8 while the game window has focus, `runtime/diag/bugcap.c`: its dump goes into the capture's own
  folder, as `flight/`);
- a frame interval over `NX2SARIA_FLIGHT_SPIKE_MS` (100; 0 off; at most one dump per 10 s, `FLIGHT_SPIKE_GAP_NS`);
- `NX2SARIA_FLIGHT_AT=<frame>[,<frame>...]` and `NX2SARIA_FLIGHT_EVERY=<n>` (every n-th frame: with n under the
  ring's length the dumps of a run hold every frame of it but the last n);
- a one-frame flash (`NX2SARIA_FLIGHT_FLASH`, 1; at most one dump per 30 s, `FLIGHT_FLASH_GAP_NS`; see
  `flight_cells()` below).

A dump holds the frames up to `FLIGHT_POST` (30) presents after the trigger, so the aftermath is in it too.
`tools/flight_view.py` reads it.

Records come from `runtime/diag/perf.c` `perf_present()` on the render thread; the counts are differences of the
`NX2SARIA_TRACE=perf` running totals and of `vk_exec_frame`. Off, the only cost is the `flight_on` branch in
`perf_present()` and in `runtime/vk/vk_backend.c`.

Notes on the file's data:

- Record columns after frame, time and interval: blocked time per `PERF_*` kind (us), then the counts (`C_SUBMITS` to
  `C_XLATED`, then one per `PERF_CNT_*` in the order of `runtime/host.h`).
- `Rec`: `step_page` and `step_game` come from `frame_step_flight`: the step in 60 Hz frames on the patch page and in
  the game; `guest_presents` is the game's presents so far, which the two steps belong to.
- `TexEv`: a texture event (`flight_tex()`). `seq` is the event's index + 1 once it is written, 0 while a writer
  fills the slot, so the copy `start_dump()` takes leaves out a slot that is being written or was reused meanwhile.
- `TEX_RING` (`1u << 18`): events kept: 14 MiB; an area load makes a few thousand in one frame.
- `DumpJob`: `n` records, oldest first; `ev` the texture events of the records' frames, oldest first; `ev_from` the
  first frame with all its events kept, when the ring had dropped earlier ones (0: all).
- `recorded`: records written so far. `trig_dir`: the pending dump's folder when its trigger names one
  (`flight_capture()`), else "". `cost_ns`, `cost_max_ns`: render-thread time in `flight_frame()`, for the dump's log
  line. `tex_n`: texture events written so far.
- `flash_track` is the present thread's. `flash_found`: (frame << 8 | cells counted) of the newest flash, 0 once
  `flight_frame()` took it. `last_flash_ns`, `flash_n` are the render thread's: the last flash dump's time, flashes
  found so far.

### `flight_init()`

Without `NX2SARIA_FLIGHT_DIR` the dumps go to `<exe dir>/../flight`: the `flight` folder beside the folder the
executable is in.

### `flight_mem()`

The recorder's memory for the memory report (`runtime/vk/vk_memory.c` `system_log()`): the frame records and the
texture events in process memory, and the thumbnail ring, which `runtime/gfx/gfx_present.c` keeps in host-visible
memory of VMA's (counted there too).

### `flight_cells()`

The flash trigger: a frame whose picture differs strongly from the frame before and the frame after while those two
agree, which is what a one-frame flicker looks like (`runtime/diag/flight_flash.h` has the test and what it leaves
out). The present thread hands in each thumbnail's cell means as the thumbnail completes
(`runtime/gfx/gfx_present.c` `thumb_cells()`), a few presents after its frame, and `flight_cells()` judges the frame
before it there, once both its neighbours are known: a frame beside one without a thumbnail is not judged. A flash
found is left in `flash_found` for the render thread, whose `flight_frame()` starts the dump: named for the flash's
frame, some presents back by then, and no more often than `FLIGHT_FLASH_GAP_NS`. A flash not yet taken gives way to
the newer one.

Only with thumbnails kept; `NX2SARIA_FLIGHT_FLASH=0` turns it off, `NX2SARIA_FLIGHT_FLASH_DIFF` and
`NX2SARIA_FLIGHT_FLASH_CELLS` are the test's two thresholds. Their defaults (24 of 255, 3 cells) were tried on the
thumbnails of earlier dumps, 48000 frames of scripted runs. In the runs of builds that showed the scene white for
single frames they flagged about 980 frames; the ones looked at were such white frames (68 to 133 cells each) and one
frame with other lighting than its neighbours (9 cells). In the 16000 frames of the runs without that fault they
flagged nothing, and neither did a threshold of 10. Play with more effects on screen than those runs had is not
measured.

### `flight_tex()`

The release fence after `seq = 0`: a reader that sees the fields being written sees `seq` 0 first. The event's frame
is the one `flight_frame()` records next: `gfx_counters.frames` counts the presents so far, and the counts of this
present's record (`frames.tsv`) gather until the next one.

### `copy_events()`

The texture events from frame `first` on, oldest first, into the job (render thread, at the dump's start). When the
ring has wrapped, events older than the oldest kept one are gone, and the oldest kept event's frame may have lost
earlier events of its own: `ev_from` is the next frame on. A slot whose `seq` changed was reused while it was copied
and is left out.

### `write_events()`

`<dir>/events.tsv`: one line per texture event of the dump's frames. Like the dump's other files it is written as a
part file and renamed when complete (`runtime/part_file.h`): the game may close under the dump thread.

### `put_sym()`, `deflate_fixed()`, `write_png()`

The PNG writer (RGB, Paeth rows, fixed-Huffman deflate). `put_sym()` writes RFC 1951 3.2.6's fixed literal/length
code. `deflate_fixed()` makes the zlib stream of its input: one fixed-Huffman block, greedy matches through a hash of
the last position of each 3-byte key (`1 << HBITS` entries, `HBITS` 15). `PngBufs` for a w x h picture: `raw`
(1 + 3w)h bytes, `z` 9/8 of that + 64, `head` `1 << HBITS` entries. Every row uses filter 4, Paeth. The stream goes
into one IDAT chunk, so one over `UINT32_MAX` bytes is refused.

### `flight_write_png()`

A picture of any size as a compressed PNG, with buffers of its own (a bug capture's frame, `runtime/diag/bugcap.c`;
any thread, with the recorder on or off). 0 when memory runs out or the file cannot be written.

### `dump_main()`

The dump thread. `write_events()` comes first: it takes a moment, the thumbnails seconds, and a game closed meanwhile
keeps it. The thumbnails are read oldest first: each slot is copied out long before the presents that go on meanwhile
reach it again. The newest few complete only when their present fence has been waited for, so they get a second try
(two passes).

### `trigger()`

One dump pending at a time; its window covers this frame.

### `interval_of()`

The interval recorded for a frame in ns, 0 when the ring no longer holds it; the newest records are looked at first.

### `flight_capture()`

A bug capture (`runtime/diag/bugcap.c`, the render thread): the dump of the frames around it goes to `dir`. 0 when
the recorder is off or a dump is pending already, whose window covers this frame and which goes where its own trigger
said.

### `flight_frame()`

Per frame, on the render thread. `vk_exec_frame` grows until `vk_exec_frame_log` resets it, which
`NX2SARIA_TRACE=gfx` does right after this call. A flash `flight_cells()` found since the last present has its frame
a few presents back (the thumbnails complete late): the dump carries that frame's number and interval, and like any
other ends `FLIGHT_POST` presents after it. When a dump is pending already, its frames hold this one.

## `runtime/diag/flight_flash.h`

The flight recorder's flash trigger (`runtime/diag/flight.c` `flight_cells()`): whether a frame's picture is a
one-frame flash, judged from the thumbnails of three frames in a row. A flash is a frame that differs strongly from
the frame before and from the frame after while those two agree with each other: a texture that was blank for one
frame, a draw that went missing, a picture from somewhere else shown once. Own header so
`tests/flight_flash_check.c` checks it without a GPU.

The pictures are compared as `FLASH_CELLS_X` x `FLASH_CELLS_Y` (16 x 9) cells of mean brightness (`flash_cells()`),
which noise hardly changes and a moving camera changes slowly. A cell counts for the flash when

- the frame's mean is brighter than both neighbours' or darker than both, by more than `diff` (of 255) each, and
- the two neighbours are close to each other in that cell and in the eight cells around it: no further apart than a
  quarter of what the frame differs by.

The frame is a flash when at least `min_cells` cells count, and the neighbours agree over the picture as a whole: in
no cell are they further apart than `diff`, and in at most `FLASH_MOVING` (`FLASH_CELLS / 4`) cells further than a
quarter of it. What that leaves out:

- a camera cut: the frame agrees with the neighbour on its side of the cut;
- a fade: the frame lies between its neighbours;
- steady motion: a shape that crosses a cell makes that cell's mean rise and fall, and in the frame at the top the
  cell is unlike both neighbours, which are alike. But what left the cell went into the next one, so the neighbours
  differ there by as much: the cells around are what tells a shape passing from a flash in place. A shape that jumps
  further than a cell a frame leaves its marks further away, where the frame before and the frame after differ by as
  much as a flash does: such a frame is not judged;
- fast motion and anything else that changes much of the picture between the neighbours: not judged.

It cannot tell a fault from a flash the game means (lightning, the white frame of a hit), and a flash while the
camera turns fast goes unseen: the F8 dump is for those.

### `flash_cells()`

The cell means of a picture of w x h pixels, four bytes a pixel with red or blue first (the weights are the same for
both, so the thumbnail's byte order does not matter): (first + 2 * second + third) / 4 over the pixels read. It reads
every `FLASH_STEP`-th (2) pixel of every `FLASH_STEP`-th row: a 640x360 thumbnail has cells of 40x40 pixels, of which
this reads 400 each: 57600 pixels a picture.

### `flash_test()`

The test over the cell means of three frames in a row: the number of cells that count for a flash of `cur` when it is
one (at least `min_cells`), else 0. `apart[]` is how far apart the two neighbours are in each cell; a cell where they
are further apart than `diff` ends the test (something crossed this cell between the two neighbours), and more than
`FLASH_MOVING` cells in which they disagree mean the picture moves. Per cell: a frame between its neighbours does not
count; `by` is what the frame differs by from the nearer neighbour; `around` is the neighbours' largest distance in
this cell and those around it.

### `flash_note()`

`FlashTrack` holds the cell means of the last thumbnails, by frame. A thumbnail completes a few presents after its
frame and some frames have none (a present that was skipped), so each slot names the frame it holds; 0 is an empty
slot (frames with a thumbnail start at 1). `FLASH_KEEP` is 4: the frame judged and its two neighbours fall into three
different slots.

`flash_note()` takes the cell means of `frame`'s thumbnail and judges the frame before it, the newest whose two
neighbours can both be known. It returns `flash_test()`'s count when frame - 1 is a flash; 0 when it is none, and
when it or frame - 2 has no thumbnail here: a frame without both neighbours is not judged.

## `runtime/diag/floor_watch.h`

`NX2SARIA_FLOOR_WATCH`: which code reads and writes the floor under Link, from hardware watchpoints on the addresses
`runtime/pacing/frame_rate.c`'s floor scan found. Diagnosis, 1.1.1, Windows; off by default.

- `floor_watch_wanted()`: the setting, read once: 1 on.
- `floor_watch_found()`: the scan's survivors once it has them (`runtime/pacing/frame_rate.c` `floor_scan()`): n
  addresses of float triplets, their values at that present, whether each lies in the platform's rigid body
  controller unit, and Link's position. 1 when the watch runs from now on.
- `floor_watch_present()`: each present from then on (`runtime/pacing/frame_rate.c` `frame_step_present()`), with the
  game's frames per update. 0 once the watch is done.

## `runtime/diag/floor_watch.c`

`NX2SARIA_FLOOR_WATCH` (`floor_watch` in the settings file, 0 by default; diagnosis, 1.1.1, Windows): which code
reads and writes the moving floor under Link, at 60 fps and above it. It takes its addresses from
`runtime/pacing/frame_rate.c`'s floor scan (which it switches on), once the scan has the values below Link that move
with him at 60 fps:

- the candidates: up to two survivors inside the platform's rigid body controller unit (the object with vtable
  main+0x4bf8a88, where the platform's own position is kept), then one survivor of each distinct object (more than
  0x200 bytes from the ones taken) at the height most survivors share below Link (the platform's pivot, 2 m under him
  on the Deku Tree's floating platform), `FW_CAND` (12) in all;
- a pass watches them four at a time (the debug registers, `runtime/platform/platform.h` `plat_watch_set()`) for
  `FW_ROUND_NS` (1.5 s) each, every read or write of the 8 bytes at a survivor (its x and y; 4 bytes, x alone, when
  it is not 8-byte aligned). Each access counts its call chain (the instruction after it and three return addresses)
  in a lock-free table (`FW_SLOTS`, 8192 chains, open addressing; a full table drops new ones), with the candidate
  and the kind: a write when the value changed since the candidate's last access, else a read;
- the first pass runs as soon as the candidates are known (the scan arms at 60 fps); the second (`FW_PASSES` is 2) at
  the next stretch of 120 presents (`FW_HIGH_STRETCH`) at 120 fps or more (frames per update at most 0.5,
  `FW_HIGH_FRAMES`) with Link on the floor (his height over the pivot within 0.2 m of what it was at the first pass,
  at most 4 m from it sideways). After each pass the log gets the pass's frame rate, each candidate's reads and
  writes a present, and its `FW_SHOW` (40) most counted chains, each frame named by the executable's symbol table
  (lifted functions are `f_<guest address>`, shown with their main offset), so the code that reads the floor at
  60 fps and not at 120 can be compared.

A round ends early after `FW_ROUND_HITS` (200000) accesses; after that each thread that traps again has its
watchpoints cleared. Each access costs an exception (several microseconds), so a pass slows the game for its few
seconds; symbol lookup after a pass holds one present for up to a second. Off it costs one test a present in
`runtime/pacing/frame_rate.c`.

A table slot's `key` 0 means free. The watch's `state`: 0 no candidates, 1 waiting for a pass's frame rate, 2 a pass
runs, 3 done.

### `fw_hit()`

On the thread that made the access, inside its exception handling: no lock, no allocation, no log. The value now is
read through the system (a read from here would trap again): a write changed it.

### `floor_watch_found()`

First the controller unit's own triplets (at most two), then the height most survivors share below Link: the pivot
that the floor's copies (body, matrices) hold.

### `on_floor()`

Link on the floor: his height over the floor's pivot as at the first pass, and not far from it sideways.

### `round_arm()`

A candidate that can no longer be read is gone with its area and is skipped.

### `frame_text()`

A lifted function is named after its guest address: `f_7002c215e0` is main+0x2c215e0.

### `report()`

The report of a pass. The frames' names come from the executable's own symbol table
(`runtime/diag/prof_syms.c` `prof_syms_each()`).

### `floor_watch_present()`

In state 1 it waits for the pass's frame rate, with Link on the floor: the first pass wants frames per update within
0.05 of 1.0, the second at most `FW_HIGH_FRAMES`.

## `runtime/diag/hang_watch.c`

The hang watch: a report in the log when nothing moves for `NX2SARIA_HANG_WATCH_S` seconds (default 20; 0 off). A
present, a GPU submission or the completion of one that never comes back (a deadlock, a driver wait that does not
return, a GPU hang without a reset) left a log that just stopped, and nothing in it said where the threads were.

Its own thread reads, once a second and without a lock, what moves while the game runs: the frames shown
(`gfx_counters.frames`) and the GPU submissions issued and done (`runtime/vk/vk_sync.c` `vk_sync_progress()`). Two
stalls are reported:

- no frame shown for N s while the guest runs, the window is in front and not minimized and the process is not
  ending: the game stops presenting when it loses the focus, so a focus change starts the count again, and the boot
  screen (before the guest starts) is not judged;
- submissions issued and not done, with "done" unchanged for N s, whatever the focus.

One report per stall, and a new stall needs progress first; at most `HW_REPORTS` (3) a run. The report names the
stall, who holds the present, queue and window-start locks (`runtime/gfx/gfx_present.c` `gfx_lock_holders()`), the
guest threads' lock and thread-end waits (`runtime/hle/kernel.c` `kern_wait_report()`), and then writes the log out
with `logq_flush`, which leaves the log thread on: `rt_log_sync` would make every later line a write on the thread
that logs it, drawing threads included, in a run that may yet recover. The watch never ends the process, never calls
the device, and takes no lock a drawing thread waits for, but for the kernel lock it tries during a report
(`runtime/hle/kernel.c` `kern_wait_report()`).

### `hw_step()`

One second's reading: which stall to report now, if any (`HW_NONE`, `HW_PRESENT`, `HW_GPU`).

- `HwBeat` is what the watch reads each second; its `judge_present` says the guest runs, the window is in front and
  not minimized, and the process is not ending.
- `HangWatch`: `frames`, `frames_at`: the frame count last seen, and since when nothing moved while judged; `done`,
  `done_at`: the same for the submissions done, while some are outstanding; `present_told`, `gpu_told`: this stall is
  reported.
- Progress, or a second not judged, starts the count again.
- A GPU stall stops the frames too: one report for both.

## `runtime/diag/mem_growth.h`

The memory report's growth bookkeeping, the part without a GPU: which of the report's figures grew and by how much,
since the previous report and since the first report in play, and which of them only ever climbed.
`runtime/vk/vk_memory.c` (`growth_log()`, `growth_rank_log()`) feeds it the figures of each report and logs what
these functions write; `tests/mem_growth_check.py` runs this file alone.

A figure is a byte count the report gives (device-local memory, textures, the process's commit, the game's heap, the
shader caches, ...), one category each. The first report taken once the game is in play (the loaded save's world
clock runs, `frame_step_world_us`) is the base of the session: what the boot and the title screen took is no growth.
Before it a report is compared with the one before it only. A category "only climbs" when, from the base on, it rose
by at least `MG_NOISE` against the report before at least once and never fell by that much: a figure that comes down
again after an area change is a cache doing its work, one that never does is a leak candidate.

- `MG_MAX` 32: categories at most. `MG_NOISE` (`1ull << 20`, 1 MiB): a change smaller than this between two reports
  is neither a rise nor a fall.
- `MgCat`: `first` at the base (the first report in play); `prev`, `last` at the report before the last, and at the
  last; `ups`, `downs`: reports from the base on that rose or fell by `MG_NOISE` or more against the one before.
- `MgBook`: `lines` reports taken; `play_lines` of them from the base on, the base included; `play`: the base is
  taken.

### `mg_cat()`

A new category; its index, -1 when the book is full. Categories are added before the first report.

### `mg_take()`

A report's figures, `v[i]` for category i. `in_play`: the game is in play now; the first such report is the base, and
later ones count as in play whatever `in_play` says (a return to the title screen does not end the session).

### `mg_put()`

Appends printf output at `out + *k`, never past `cap`; `*k` counts what would have been written, so a full buffer
stays cut at its end (and the caller can tell by `*k >= cap`).

### `mg_grew()`

The growth since the last two reports, for one log line: "since the previous report: textures +312 MiB, process
commit +420 MiB; since play began: textures +3800 MiB (only climbs), ...". A category is named in the first list when
it grew by more than `step` since the report before the last, in the second when it grew by more than `step` since
the base; the second list is there from the base on. Writes nothing when no category is named. Returns the number of
categories named in either list; `out` holds a string whenever `cap > 0`.

### `mg_rank()`

The categories ranked by their growth since the base, the largest first: those that grew by `MG_NOISE` or more, each
marked when it only climbs, then those that fell by that much (the largest fall first), then how many stayed within
`MG_NOISE`. The order comes from an insertion sort by growth, which is stable: ties keep the order the categories
were added in. Returns the number of categories that grew; 0 with nothing written before the base.

## `runtime/diag/perf_bound.h`

Which side holds the frame back, for the second line of the 60-frame window ("perf: frame N: ... bound: X",
`runtime/diag/perf.c` `perf_gpu_line()`). Header only, so that `tests/perf_bound_check.c` checks the rule without a
game run.

### `perf_bound()`

| Verdict | When |
|---|---|
| `pace` | the frame waited for pacing (vsync, the frame limiter) at least 15% of the window (`PERF_PACE_SHARE` 0.15) |
| `gpu` | the GPU was busy for 80% of the frame interval or more (`PERF_GPU_BUSY_SHARE` 0.8); without timestamps: threads waited for fences for half the interval or more (`PERF_FENCE_SHARE` 0.5) |
| `serial` | the GPU and the CPU took turns: the GPU sat idle waiting for the next submission for a fifth of the interval or more (`PERF_STARVED_SHARE` 0.2), and threads waited for GPU fences for a quarter of it or more (`PERF_SERIAL_WAIT_SHARE` 0.25). Neither side is full, and the frame is close to the sum of the two. A cheaper GPU frame or a cheaper replay both shorten it |
| `cpu` | otherwise |

Why `serial` is its own verdict: standing in Kokiri Forest on an RX 6700 (RADV, 2026-11-19) the window line said cpu
in every window while, per frame of 33 ms, the GPU was busy 22.5 ms and idle 11 ms (9.6 ms of it waiting for the next
submission) and the game's threads waited 17 ms for GPU fences. The game waits after a frame's first submission
(`runtime/vk/vk_sync.c`, "the executed timeline"), and that small submission is queued behind the frame before's
large one, so the CPU's work and the GPU's do not overlap. Calling that frame cpu-bound sends a reader to the replay
alone, and calling it gpu-bound to the resolution alone.

`PerfBoundIn`: `interval_ms` the frame interval over the window; `pace_share` the share of the window spent in pacing
waits; `have_gpu`: GPU busy time from timestamps; `gpu_ms` GPU busy per frame; `have_idle`: the GPU's idle time split
by calibrated timestamps; `starved_ms` GPU idle per frame with no submission queued (waiting for the CPU); `wait_ms`
the render thread's and the other threads' GPU fence waits per frame.

## `runtime/diag/perf_census.h`

The part of a frame that an address-space walk of `NX2SARIA_MEM_CENSUS` overlapped, charged to the census
(`runtime/diag/perf.c` `ft_frame()`). `runtime/vk/vk_memory.c` `census_main()` walks on a thread of its own, but the
walk holds the process's address-space lock, which the replaying thread's write-watch queries
(`runtime/vk/vk_memory.c` `vk_mem_mirror_begin()`) and every page fault take too. In a session of 2026-11-13, 63 of
the 66 hitches the stutter map charged to the replay sat beside such a walk, with a 20-34 ms write-watch query in
them; the presenting thread's 600-frame memory report (`runtime/vk/vk_memory.c` `vk_mem_census()`) ran at the same
frames, and that log cannot tell which of the two held them up. The charge assumes the walk did, which a play-test
with `mem_census = 0` checks: the time the frame overlapped the walk is taken out of the frame's replay part first,
then out of its unattributed part, never more than the overlap and never so much that the census part grows past the
frame. Apart from `perf.c` so that `tests/census_charge_check.py` builds it alone.

`CENSUS_WALK_MAX_NS` (2 s): a walk takes 100-260 ms, so one seen running for longer than this is a torn read:
`ft_frame()` loaded the last walk's start, then the 0 that `runtime/diag/perf.c` `perf_census_walk()` (called with 1)
stores as the end before it stores the next start. Taken as running, that walk would cover the whole frame.

### `census_charge()`

The frame is [now - d, now]; the walk [w0, w1]: w0 0 when no walk was seen, w1 0 (or older than w0, the end of the
walk before) while it runs, then reaching now. Returns the time moved into `*census`.

## `runtime/diag/perf.c`

Frame timing (`NX2SARIA_TRACE=perf`, the frame-time summary, the performance overlay's counts), the A/B switches
(`rt_ab()`, `rt_knob()`), the processor placement (`NX2SARIA_ONE_PER_CORE`, `NX2SARIA_CACHE_DIE`) and a few things
done once per present. The sampling profiler (`NX2SARIA_PROFILE`) is the OS's (`runtime/diag/prof_win.c`); without
one the weak `prof_sampler_start()`, `prof_sampler_write()`, `prof_sampler_frame()` and `prof_commit_census()` here do
nothing. How to read the lines: [performance.md](../performance.md).

`perf_on` says event counts are taken: `NX2SARIA_TRACE=perf`, `NX2SARIA_FLIGHT=1` or the full performance overlay.
Wait times are always taken (the frame-time summary). `perf_base` is the part fixed at start. `overlay` is the
performance overlay's mode (`perf_set_overlay()`): 0 off, 1 fps, 2 full. `gpu_want` (`perf_gpu_timing_want()`): a
feature reads GPU time (dynamic resolution).

### `log_sync_at_exit()`

Registered before the other exit handlers, so it runs after them (`atexit`'s order is the reverse): a process that
ends through `exit()` rather than `plat_exit`, whose log sync this is, gets the lines of the frame-time summary, the
stutter map and everything queued before them into the log instead of losing them with the log thread's queue.

### `prof_start()`

`atexit(perf_summary_flush)`: `runtime/boot.c`'s `_exit` path goes through `prof_write()` instead. When the sampler
runs, `prof_write()` is an exit handler too: a `--seconds` run ends in `_exit`, and `runtime/boot.c` calls
`prof_write()` itself first.

### `perf_end()`

`NX2SARIA_TRACE=perf`: the frame interval and the time the render thread spends waiting, logged every 60 presents.
The flight recorder (`NX2SARIA_FLIGHT=1`, `runtime/diag/flight.c`) takes the same numbers per frame. `wait_ns`,
`frame_ns` and `worst_ns` (each `PERF_N`) are the window's totals, this frame's, and the slowest frame's. On the
render thread without an asynchronous replay one thread adds: no atomics on the synchronous path.

### `perf_end_any()`

The waits of other threads that hold the frame all the same: a game may acquire its window image on a thread other
than the one that presents (`runtime/pacing/vk_lowlat.c` `vk_lowlat_pace()`; its note is in
[pacing.md](pacing.md)), and `perf_end()` leaves those waits out, so the pacer's wait in the acquire showed nowhere.
They gather in `any_ns` and are added to `frame_ns` and `wait_ns` by the present (`perf_present()`) before the
frame's parts are read.

### `perf_gpu_wait()`

`other_wait_ns`: the waits on threads other than the render thread, over the window; `render_wait_ns`: the same on
the render thread.

### `perf_replay()`, `perf_replay_cpu()`

`replay_ns`: summed over the window, any thread; `frame_replay_ns`: the same since the previous present (the
frame-time summary). `frame_replay_cpu_ns` and its period sum are the replaying thread's CPU time over the same
spans: the rest of the replay's wall time it was waiting (a lock, the recorder) or not running (preempted).

### `perf_count()`

`counts[PERF_CNT_N]` are running totals: the perf log and the flight recorder take differences.

### `perf_mark_begin()`, `perf_mark_add()`, `perf_mark_end()`

`NX2SARIA_TRACE=perf` work marks (`PERF_M_*` in `runtime/host.h`): any thread adds; the present takes the frame's
sums (`mark_frame()`), names them on a slow frame's line and adds them to the 60-frame window's line. `mk_ns`, `mk_n`,
`mk_bytes` and `mk_max` hold what came since the last present; `mk_win` is the 60-frame window's, present thread
only.

### `mark_frame()`

Takes the sums since the last present and adds them to the window.

### `mark_text()`

"; on any thread: file reads 12.3 ms (40, 5.1 MiB, longest 2.0 ms), ..." for the kinds with work, appended to the
line.

### `perf_segment()`, `perf_scene()`

`NX2SARIA_TRACE=perf` segments and scenes. The present thread keeps the running segment's frame times in 50 us bins
(`SEG_BINS` 4096, up to 204.8 ms; slower frames land in the last bin, the worst is kept apart), so a segment's 1% low
is exact to 50 us at any frame rate. `perf_segment()` and `perf_scene()` come from any thread (the input script's
guest thread, the game's loading threads) and leave a request under `seg_lock` for the present: `seg_next` the
requested segment's name, `scene_name` the areas loaded since the last snapshot, `seg_asked`, and `scene_at`, the
last area load (`plat_now_ns`; 0 none since the last snapshot). `area_load_ns` is the last area load for
`perf_area_load_ns()`, 0 none.

### `perf_size()`

The "mem: sizes" lines (`PerfSizes` in `runtime/host.h`): one line per group; a line that grows past the log's line
size goes out and the group goes on in the next.

### `hc_main()`

`NX2SARIA_HANDLE_CENSUS=1` with `NX2SARIA_TRACE=perf`: the process's OS handles by kind
(`runtime/platform/platform.h` `plat_handle_census()`) at the 600-frame report, when their count (the sizes line's
"process handles") grew by `HC_GROWTH` (16) or more since the last census. In a session of 2026-11-13 that count rose
at every area load, 683 to 1095 in 11 minutes, and never fell, while the guest's handles stayed at 116-122: this says
which kind grows. The census reads the system's whole handle table (2-5 ms and a buffer of a few MiB), so it runs on
one thread of its own, started with the trace and woken by the report; a new thread per census would add its own
Thread handle to what it counts. `hc_frame` is the report's frame, for the line.

### `hc_report()`

The 600-frame report (`perf_present()`): wakes the census thread, which decides whether the count grew enough.

### `mem_snapshot()`

The memory lines of a segment's end or a settled scene: the back end's (`vk_mem_snapshot`), then the sizes. Their
time on the presenting thread delays the next frame: its "report" part.

### `wtxt()`

The world clock as the segment and scene lines give it: seconds, or "-" before the loaded save's first frame.

### `mem_text()`

The process's memory and the VRAM in use, for the segment and scene lines.

### `seg_close()`

The 1% low is the upper edge of the bin that holds the slowest 1% of frames (`(frames + 99) / 100`).

### `seg_frame()`

Each present while the trace is on: the frame into the running segment (d 0: the first present), then a requested
segment change and a settled scene's snapshot. The first present opens the "boot" segment.

### `ft_frame()`

The frame-time summary, always on: every 60 s of play and at exit one line of frame count, mean fps, 1% low and the
frames over 25/50/100 ms, then the worst frames of the period (`FT_WORST` 5) with the render thread's waits in them.
Per frame: a histogram increment (`FT_BINS` 1024: 0.25 ms bins up to 256 ms; slower frames land in the last bin), the
compare against the worst list, a memset of the wait array. No allocation.

`FtWorst`: `at` is when its present came (`kern_now_ns`): `log_stamp` turns it into the local time the line carries;
`op_ns`, `op_n`, `max_ns`, `hold_ns`, `max_op` are the frame's replay ops (`NX2SARIA_REPLAY_TIME`); `replay_cpu` the
replaying thread's CPU time in the frame's replay.

The causes (`FT_C_*`) say what a frame over 25 ms spent its time on, so a count of slow frames says where to look
instead of only that they happened. A frame is charged to the largest part when that part is at least a quarter of
it, and to "unattributed" otherwise: the part of a slow frame none of the recorded waits explains (the GPU, or a
stall outside them).

- The replay's part is its time without the pipeline, translation and texture waits, which all happen inside it (they
  are waits on the replay thread): with them in, every frame that waited 900 ms for a pipeline was charged to
  "replay", the larger number, and a log could not tell a pipeline stall from a slow replay.
- "census" is the address-space walk of `NX2SARIA_MEM_CENSUS`: `census_check()`'s on the presenting thread after a
  present, whose time is charged to the next frame, which it delays, and the part of a frame
  `runtime/vk/vk_memory.c` `census_main()`'s walk on its own thread overlapped (`runtime/diag/perf_census.h`
  `census_charge()`).
- "report" is the memory report's time on the presenting thread (`runtime/vk/vk_memory.c` `vk_mem_census()` every 600
  frames with the perf trace, every 3600 without; the snapshots and sizes lines here), charged to the next frame too.
- "hand-off" is the wait for the present thread to take the previous picture (`PERF_HANDOFF`): its own pacing and its
  present call, an overlay's frame limiter included.
- `cause_short` in `runtime/diag/stutter_map.c` and `tools/stutter_map.py` read these names, in this order.

The parts are worked out for a frame over 25 ms, and for the stutter map's hitches, which are 12 ms at least.

State: `census_part_ns` (present thread): the census walks since the last present (`census_check()`);
`report_part_ns`: the memory report's time since the last present (`perf_report_part()`), any thread; `walk_t0`,
`walk_t1`: `census_main()`'s last walk (`perf_census_walk()`), `walk_t1` 0 while it runs; `census_walked`: the frame
`census_check()` walked last. `ft.slow_n`, `ft.slow_ns`: frames over 25 ms by cause; `ft.worst`: slowest first.
`summary_t0`: start of the current period (0: not started). The session's numbers are the same as `ft`'s but never
reset (`perf_session_stats()`, for `runtime/session_summary.c`).

`ft_frame()` returns the replay time since the previous present. `add` 0: the exit's summary is written (`ft_stop()`),
so the frame is not added to it. The first present starts the period; there is no interval yet, and
`log_pressure_baseline` is taken there: the first minute's system line counts its stalls from it.

### `ft_enter()`, `ft_leave()`, `ft_stop()`

The exit's summary (`perf_summary_flush()`) runs on the exiting thread while the present thread may still add frames
to `ft` and write its own reports (`ft_frame()`, `session_report()`), with no lock between them: the exit lines came
out torn, or twice. `ft_stop()` marks the end (`ft_ending`), after which the present thread adds nothing, and waits a
little for an update already under way (`ft_busy`); it is the exiting thread that waits, never the present thread.
Both sides are sequentially consistent, so either the present thread sees `ft_ending` or `ft_stop()` sees it counted
in `ft_busy`.

### `perf_replay_op()`, `perf_replay_op_at()`

`NX2SARIA_REPLAY_TIME=1`: how the replay splits between the kinds of op it walks, reported with the frame-time
summary. The replay is recorded on one thread, so these need no atomics. The kinds are `PERF_RP_*` in
`runtime/host.h`.

`fr_ns`, `fr_n`, `fr_max_ns`, `fr_max_op`: the replay ops since the last present, so a frame over 25 ms charged to
the replay can say what its replay was: how many ops of each kind, their time, the slowest op and how long the
texture lock was held over 1 ms (`NX2SARIA_LOCK_TIME`). The present thread takes and clears them in `ft_frame()`; the
replay adds. `sr_*`: over the period, the frames over 25 ms charged to the replay, their sums, and how often each op
was a frame's slowest. One replay thread at a time records ops; a race with the present only misnames the frame's
slowest op.

### `lt_add()`, `perf_lock_hold()`, `perf_qlock_hold()`, `perf_lock_timing_here()`

`NX2SARIA_LOCK_TIME=1` (default 0): the replay threads time every lock they have to wait for
(`runtime/platform/platform.h` `plat_lock_timing()`), kept per asking site (`LT_N` 512), and the frame-time summary
lists the sites that waited longest since the last summary, with the recorder's full-ring and drain waits. Sites are
code addresses; "ref" is `perf_lock_timing_here()`'s, which the link map names, so the others resolve as offsets from
it. `lh`: texture lock holds over 1 ms (`perf_lock_hold()`); `lq`: queue lock holds of 5 ms or more
(`perf_qlock_hold()`); `fr_hold_ns`: the texture lock's holds over 1 ms since the last present (`ft_frame()`).

### `lt_top()`

The eight sites of a table with the most time since the last summary, appended to the line (kept sorted by this
period's time); returns the period's total and sets `*count`, then starts a new period.

### `lt_report()`

The lock lines of a summary: the replay threads' waits with the recorder's, then the texture lock's and the queue
lock's long holds.

### `events_whole_once()`

The events of the whole run (`runtime/log_stall.c`), once: the orderly end writes them, and a fatal error or a second
call during that end (the pipeline save takes seconds) finds them written. A run that ends before its first present
has no frame-time period to carry them, so the ends write them on their own then: refusals of the physical memory map
are counted from the start of the boot, and a boot that ends in a fatal error would have the first line of each and
no count.

### `ft_report()`

The 1% low is the upper edge of the bin that holds the slowest 1% of frames. The replay of the frames over 25 ms
charged to it is given per frame. Beside the window, whatever the frames did: the events whose own lines are limited
or left out, counted (the whole run's at the end), and the machine's memory and I/O pressure, so a stall that has a
cause outside the game shows in the minute it happened in (`runtime/log_stall.c`).

### `perf_focus_lost()`, `session_report()`

The session's reports when the window loses the focus in play (the loaded save's world clock runs): the frame-time
summary of the period so far (which starts a new period), the stutter map and the memory report's growth ranking, at
most every `SESSION_REPORT_GAP_NS` (60 s). They are written at the exit too (`perf_summary_flush()`,
`vk_mem_exit_log`), but a process ended from outside, or by a path that skips them, leaves none: a session of
2026-11-13 ended 8 s after its window lost the focus, after 7 minutes of play, with no ranking (one every 10 minutes
then) and no map of its last minute. `runtime/gfx/gfx_present.c` `gfx_window_focus_changed()` asks for them; the
present thread writes them at its next present (`perf_present()`). Formatting only, after copying the stutter map
under its lock (`runtime/diag/stutter_map.c` `stutter_report()`): no device call, no wait.

### `vk_texstat_frame()`

The weak stand-in of the renderer's per-frame texture work (`runtime/vk/vk_texture.c`): it closes the frame's sums;
`d` is the frame's interval.

### `perf_summary_flush()`

At exit (`prof_write()`). `rt_log_ending` first: the summary and the stutter map reach the log file past its cap
(`runtime/main.c` `log_sink()`). An end before the first present writes the whole run's events on its own
(`summary_t0` is also 0 after a first end: written once).

### `perf_summary_flush_try()`

A fatal error's frame-time summary (`runtime/dispatch.c` `fatal_end()`), on the failing thread, which may hold any
lock: the running period's "frametime" lines and the stutter map so far, the map only when its lock is free. The back
end's exit lines (`gfx_exit_log()`) and the trace's segments (`seg_close()`) are left out: they read state the
failure may have left half made. The present thread's numbers may be caught mid-frame if it still runs past
`ft_stop()`'s wait.

### `perf_note_wc()`, `perf_wc_recent()`

The write-combined memory of the process (device-local memory the driver maps into it, which Windows charges to the
process's private bytes) as the latest walk of the address space counted it: `runtime/vk/vk_memory.c` `census_main()`
every 600 frames and the commit census (`census_check()`), both only with `NX2SARIA_MEM_CENSUS=1`. `vram_used`: the
driver's device-local figure at that walk, so that a later estimate can add what the device memory grew since.
`wc_bytes`, `wc_vram` and `wc_at` are under `wc_lock`; `wc_at` 0: no walk yet.

### `host_commit()`

The process's commit without the device memory charged to it: the private bytes less the write-combined memory of the
last walk and less what the driver's device-local figure grew since (before any walk, less that figure alone). The
device-local figure (heapUsage) lies 0.2-1.1 GiB under the write-combined total, so it serves for the change only.
Where the commit does not count device memory (`plat_commit_counts_vram`) it is the private bytes.

### `census_check()`

Where the committed memory lies (`NX2SARIA_MEM_CENSUS=1` only), every 3600 frames and when the process's own commit
(`host_commit()`) rose 1 GiB over the highest it was at a census: the process has run out of commit in long sessions
while the game's own pools stayed flat. The walk holds the address-space lock the game's threads take at every page
fault and runs here on the presenting thread, so each one delays the next frame by 130-260 ms (`ft_frame()` charges
it to "census"). In a session of 2026-11-13 the trigger read the private bytes, which take in the device memory
Windows charges to the process, so every gigabyte of textures re-armed it: 22 of the 33 walks came from it, 120
frames apart in bursts, while the process's own commit grew 307 MiB in 7 minutes of play. So the trigger reads the
commit without the device memory against the highest it was at a census (a figure that falls and rises again does
not re-arm it), and a growth census waits `CENSUS_GAP_NS` (60 s) after the last walk. The scheduled ones stay.

This frame's memory report (`runtime/vk/vk_memory.c` `heap_log()`, after this present) leaves out its own walk: one
is enough, and the second, 100-240 ms on `census_main()`'s thread, delayed the frames after it once more. One walk
serves both lines: `prof_commit_census()` where there is one, else `mem_guest_census()`. The highest commit is taken
again after the walk, with the walk's own write-combined figure.

### `perf_gpu_line()`

The second line of the 60-frame window: GPU busy time (timestamps), the time threads waited for GPU fences, VRAM and
process memory, and which side holds the frame back (`runtime/diag/perf_bound.h` `perf_bound()`: pace, gpu, serial or
cpu, and why serial is apart). The first window logs the rule once. With calibrated timestamps a third line splits
the GPU's idle time between timed parts: the part was submitted only after the previous one had ended (the GPU waited
for the CPU's replay or the game), or it was already queued (untimed work such as uploads and presents ran there, or
a wait held it).

### `rt_ab()`

`period` is in microseconds of the world clock; 0: off. Before the world clock runs the answer is A.

### `rt_knob()`, `knob_warn()`

`rt_knob()` reads "0", false, off and no (any case) as off, "ab" as the A/B blocks, and anything else as on, as
`runtime/cfg/cfg.c` `cfg_bool()` does; the settings file's loader has already cut a comment after a value of a key
whose row has a set of values ("0 ; a note", `runtime/cfg/cfg.c` `cut_comment()`). A value other than 0, 1, ab and
those words (true, on and yes for on) is logged once per switch; the names are string literals, so the table
(`KNOB_WARNED` 32) holds their pointers (a full table logs no more). A slot found holding the name is logged already
(another thread may have taken this slot for it just now).

### `cpu_placement()`, `perf_cpu_start()`

`NX2SARIA_ONE_PER_CORE=1`: the process keeps the first logical processor of each physical core, so busy threads (the
render thread, the guest cores, the job workers) never share a core's execution units with another; "ab" switches it
with the A/B blocks. Off by default until measured here; the same rule is reported to give +4-10% on an 8-core SMT
CPU.

`NX2SARIA_CACHE_DIE=1`, the default: on a processor whose dies have L3 caches of different sizes (AMD's two-die X3D
processors, 3D V-Cache on one die) the process runs on the die with the larger cache, so the guest cores, the render
thread and the recorder share one L3 and never cross dies. Windows' own placement of games on that die (game mode
with AMD's driver) depends on recognizing the exe as a game. 0 leaves the placement to the OS; "ab" switches it with
the A/B blocks. No change on one-die and Intel processors.

The placement is set at start (`perf_cpu_start()`, before the thread pools count the usable processors) and checked
at every present for the A/B switches. The rules narrow the processors the process was given (`start /affinity`, a
launcher's or a benchmark's pin), never widen them; with both off nothing is set. `cpu_applied` is the mask set last,
0 none; a process never restricted stays as it was given.

The largest L3 is a reason to stay on its processors only where it serves enough cores to run the game: two dies of
one design, one of them with stacked cache (a two-die X3D desktop part, 8 cores a die). A die of fewer cores holds
the game to them: the 4 large cores of a mobile part whose 8 small cores share a smaller L3, or the 6 of a 12-core
X3D part, where the recording thread's own core (`runtime/vk/vk_exec.c` `rec_claim()`) left 5.

### `crash_filter_guard()`

`NX2SARIA_CRASH_FILTER_GUARD` (default 1; 0 off): once a second, the crash report's last-chance exception filter is
put back if a DLL loaded after the start (a GPU driver, an overlay) set its own in its place. Without it, a fault in
such a DLL ends the process with no FATAL line in the log. The first eight replacements are logged with the module.

### `reserve_sweep()`

A thread the recording thread starts through a library inherits its reserved core, and the core would stay reserved
after the recording thread ended (`runtime/platform/platform.h` `plat_cpu_reserve_sweep()`): every `RESERVE_SWEEP`
(300) presents both are put right.

### `perf_present()`

- `pace_any`: other threads' pacing in the window. Other threads' waits for this frame (`perf_end_any()`) are taken
  before its parts are read.
- `ft_enter()` answers 0 once the exit's summary is written. `d` is 0 on the first present: `ft_frame()` only starts
  the period.
- The overlay's only cost while it is off is its branch.
- `bugcap_frame()`: F8, a bug capture (`runtime/diag/bugcap.c`); otherwise one record into its ring.
- `NX2SARIA_PERF_SLOW_MS` (default 50): frames at least this long get a line of their own. In that line "drain" is
  the wait for the present thread's previous job; "handoff" the wait to hand it the next one; "replay" the replays
  since the last present, the pipeline, translation and texture waits included (the rest of a slow frame is the
  game's own work).
- A frame whose render thread waited 20 ms or more for GPU fences: at the next present, the GPU's timed parts that
  ended in its interval (`runtime/vk/vk_sync.c` `vk_gpu_parts_log()`), read then, so that the completion thread has
  read them.
- The pacing share counts other threads' pacing too (`perf_end_any()`: the acquire's wait of a game that acquires its
  window images on a thread other than the presenting one, which the one-time "nvn: the game acquires" line of
  `runtime/nvn/nvn_api_obj.c` `nvnWindowAcquireTexture` names). That wait can overlap the render thread's own work,
  so for such a game the shares can add up to more than 100%. "render busy" takes out only the render thread's own
  pacing.
- Every 600 frames: the sizes and the handle census, beside `runtime/vk/vk_memory.c`'s 600-frame memory lines, and
  like them the next frame's "report".

## `runtime/diag/perfmon.h`

Performance overlay numbers: the game's frame rate and frame times, the render thread's waits from the perf counters,
and the machine's CPU, memory and GPU load and temperatures. A value the machine does not give is negative. The
licence notice of the copied parts stays at the top of the file.

`PerfStats` (`PERF_HISTORY` 120: frame times kept for the graph):

- the game's frames (presents), over the last second: `fps`, `frame_ms_avg`, `frame_ms_max`;
- `low1_fps`, `low01_fps`: fps of the slowest 1% and 0.1% of the frames over the last ten seconds (-1: too few frames
  yet);
- `frame_ms[]`: oldest first;
- CPU: the whole machine and this process, percent of all logical processors;
- memory, in MB;
- GPU: load and dedicated memory from the system's counters, temperature, power and clock from NVML (NVIDIA) or ADL
  (AMD);
- `vram_game_mb`: of `vram_used_mb`, which counts every program on the card, what this process holds (the driver's
  figure for the device-local heaps, `perfmon_set_vram_game()`); -1 unknown;
- per frame, averaged over the last half second: replay (NVN to Vulkan recording), GPU busy time (timestamps; -1
  without them), the render thread's waits for pipeline builds, shader translation and texture uploads, and the
  textures uploaded.

### `perfmon_mode()`

Overlay mode 0 off, 1 fps, 2 full (`NX2SARIA_PERF_OVERLAY`), from the present path when the setting changes. Full
starts the machine sampling thread (twice a second) and turns on the event counts and GPU timestamps; leaving full
stops them. Off: `perf_present()` skips `perfmon_frame()` and nothing samples.

### `perfmon_set_gpu()`

The Vulkan device the game renders on: its name, its LUID (8 bytes; NULL when the driver gives none), its PCI bus (-1
unknown) and its device-local memory. Before the first `perfmon_mode(2)`.

### `perfmon_set_vram_game()`

How the sampling thread asks for the device-local memory this process holds, in MB (`runtime/vk/vk_memory.c`
`vk_mem_device_used_mb()`; negative when the driver does not report it). Called twice a second while the overlay is
full.

### `perfmon_shown()`

A picture went out (the present thread, `runtime/vk/vk_backend.c`): the overlay's frame times are the intervals
between these.

### `perfmon_frame()`

A game frame was presented (`perf_present()`, the render thread, only while the overlay is on): the time since the
last one, the render thread's waits of this frame (`PERF_N` entries, `runtime/host.h`), its replay time and the
running count of texture uploads.

### `perfmon_snapshot()`

The latest numbers (the overlay, present path).

## `runtime/diag/perfmon.c`

Frame times come from the game's presents (`perf_present()`, only while the overlay is on); the per-frame waits from
the perf counters `runtime/diag/perf.c` keeps. The machine's numbers are sampled
twice a second on a thread of their own (some calls take milliseconds), which runs only while the overlay is full.
The OS calls sit behind `runtime/platform/platform.h` (`plat_cpu_load()`, `plat_mem_info()`, `plat_gpu_load()`). GPU
load and video memory come from the system's counters for every vendor. Temperature, power and clock come from the
GPU vendor's driver library, loaded at run time: NVML on NVIDIA, ADL's PM log on AMD; on a machine without either
their numbers stay -1. The sampler stops when the overlay leaves full. The licence notices (the overlay's copied
parts and the ADL declarations) stay at the top of the file.

`g_lock` guards everything in the file. `g_stats`: the machine numbers the sampler wrote; the rest is made per
snapshot. `g_ring_at[]`: when each frame ended.

The lows: a histogram of the frame times (`runtime/diag/perf.c`'s 0.25 ms bins, `LOW_BINS` 1024) over the last
`LOW_SLICES` (10) seconds, kept as one slice per second so the oldest second drops out whole. Per frame: one
increment. The percentiles are read twice a second. `g_win`: the per-frame counters, summed since `t0` (the overlay's
lines are averages over half a second).

### `interval_add()`, `perfmon_shown()`

The frame times the overlay shows (the rate, the lows, the graph) are those of the pictures as they go out: the
present thread reports each one (`perfmon_shown()`). The game's present calls come at other times: with the replay on
the submitting thread, work held for an event is replayed in the next frame's first submission, so the calls came 36
and 9 ms apart in turn in a village while the pictures went out every 17 to 27 ms, and the graph drew a sawtooth the
screen did not show. A frame held back (`runtime/vk/vk_backend.c` `frame_hold()`) is not shown and lengthens its
interval. Without a present thread reporting (another back end) the game's present calls are taken, as before.

`g_shown_last`: when the last picture went out; 0: none yet. `g_shown_report`: when `perfmon_shown()` last ran:
`perfmon_frame()` leaves the intervals to it for a second. `interval_add()` runs with the lock held; at the next
second the histogram's next slice starts empty. A gap of a second or more between pictures is a load or a pause, not
a frame time.

### `frames_summary()`

Lock held. The sums are over the last second; the graph is oldest first, and frames not seen yet read 0.

### `lows_summary()`

The fps of the slowest 1% and 0.1% of the frames in the window (the bin's upper edge, as `runtime/diag/perf.c`'s
summary does); -1 when the window holds too few frames. Lock held.

### `counters_summary()`

Every half second: the window's sums as per-frame averages into `g_stats`, and a new window. Lock held. No GPU time
over a window of frames means no timestamps on this device (or not yet set up): -1.

### `nvml_open()`

NVML (NVIDIA): loaded once on the sampler thread, kept for the process. The device taken is the one Vulkan renders on
(by name; an NVIDIA name only matches an NVIDIA GPU), else none: on a machine that renders on another vendor's GPU,
NVML's numbers would belong to the wrong card.

### `nvml_sample()`

The system's counters come first for the load. The temperature asked for is sensor 0 (`NVML_TEMPERATURE_GPU`), the
clock is type 0 (`NVML_CLOCK_GRAPHICS`).

### `adl_open()`

ADL (AMD): loaded once on the sampler thread, kept for the process. `ADL2_New_QueryPMLogData_Get` reads the PM log
the driver keeps for Radeon RX 5000 and later (the numbers Adrenalin's overlay shows); older GPUs answer with an
error and their numbers stay -1. Windows only: the Linux driver gives these through sysfs, which this does not read.
`AdapterInfo` is declared in its Windows layout; `adl.log` is the sampler thread's buffer.
The ADL declarations the licence notice at the top of the file covers are `AdapterInfo`, `ADLSingleSensorData`
(`AdlSensor` here), `ADLPMLogDataOutput` (`AdlPmLog`), the `ADL_PMLOG_*` sensor numbers and the `ADL2_*` signatures.

The adapter taken is the one Vulkan renders on: the PCI bus when the device gave one, else the name. ADL lists one
index per display output of a GPU; the first whose PM log answers is taken. ADL writes the PCI vendor id 0x1002 as
decimal 1002.

### `adl_sample()`

The system's counters come first for the load. The power is the whole card's draw where the driver reports it (RDNA 4
reports no ASIC power), else the GPU chip's.

### `sampler()`

The machine sampler: runs while `want` is set, twice a second. `alive` is cleared in the same locked section as the
last `want` check: a new `perfmon_mode(2)` starts a new thread.

### `perfmon_mode()`

Entering full clears the machine numbers: "-" until the first sample, not the numbers of the last time. Off: the next
start begins a new graph and a new counter window.

### `perfmon_snapshot()`

The time is taken under the lock: a frame pushed between the two would be newer than now, and `now - at` wrapping
would end the frame window at once (0 fps on the overlay).

## `runtime/diag/prof.h`

Between `runtime/diag/perf.c` (frame timing, always built) and the OS's sampling profiler
(`runtime/diag/prof_win.c` on Windows).

- `prof_render_tid`: the thread that presents (`perf_present()`). `prof_main_tid`: the guest main thread
  (`perf_main_thread()`). `prof_worker_tid[16]`: the replay workers (`runtime/vk/vk_exec.c` `rw_main()`), for
  `NX2SARIA_PROFILE_REPLAY=2`. `prof_replay_tid`: the asynchronous replay (`perf_replay_thread()`): its waits hold up
  the frame too.
- The sampler; `perf.c`'s weak versions do nothing. `prof_sampler_start()`: `NX2SARIA_PROFILE`: starts sampling; 1
  when it runs. `prof_sampler_write()`: writes the profile once (`prof_write()`). `prof_sampler_frame()`: every
  present; `d` is the interval, 0 on the first.
- `prof_commit_census()`: `NX2SARIA_MEM_CENSUS` (`runtime/diag/perf.c` `census_check()`): logs where the process's
  committed memory lies and the guest regions' committed bytes from one walk of the address space; `*wc` the
  write-combined total. 0 where there is no such walk (`perf.c`'s weak version): `mem_guest_census` walks the guest
  regions then.

## `runtime/diag/prof_syms.h`

### `prof_syms_write()`

Writes `sym <address> <function>+0x<offset>` for each of the n addresses (any order, repeats allowed) that falls in
the image loaded at `base`, from the COFF symbol table of the file at `exe_path`. Returns the lines written, -1 when
the file has no symbol table or cannot be read.

### `prof_syms_each()`

The same lookup with a callback in place of the lines: `fn(ctx, address, function, offset)` for each address named,
in address order, once each. Returns the addresses named, -1 as above.

## `runtime/diag/prof_syms.c`

Names for a sampling profile's exe addresses from the exe's own COFF symbol table (`runtime/diag/prof_win.c`).
A MinGW link keeps the table in the file (`PointerToSymbolTable`), so a
profile names its functions without the link map: for each address the profile lists,
`sym <address> <function>+<offset>`. Plain file reads, no Windows headers, so the parser builds and is tested
anywhere.

### `prof_syms_each()`

- `CSym.str` is the string-table offset, 0 for a short name.
- A section counts as executable with `CNT_CODE` or `MEM_EXECUTE` (0x20000020).
- The string table follows the symbols: its first 4 bytes are its size.
- The symbols are read in chunks; functions in executable sections are kept. Auxiliary records are skipped, and so
  are section symbols (a name that starts with '.' or is empty).
- The addresses are sorted, so the symbol index only grows.

## `runtime/diag/prof_win.c`

The sampling profiler (`NX2SARIA_PROFILE=<path>`), Windows only. `tools/prof_report.py` reads the profile. The
per-frame timing of `NX2SARIA_TRACE=perf` is `runtime/diag/perf.c`'s.

A host thread wakes every 1 ms and, for every other thread, suspends it, reads Rip and resumes it. The sampler never
allocates or takes a lock between `SuspendThread` and `ResumeThread` and never runs guest code. The profile is text:

```
nx2navi-prof 1
base <hex>
period_us
samples N
thread <tid> <samples> <in-exe samples>
module <base> <size> <path>
rip <hex> <count>                      the top PROF_TOP Rips
stack <count> <rip> <caller> ...       NX2SARIA_PROFILE_RENDER or NX2SARIA_PROFILE_CHAINS only, the top PROF_STACKS call chains
```

With `NX2SARIA_PROFILE_RENDER` (the render thread) or `NX2SARIA_PROFILE_CHAINS=1` (every thread, chains of all
threads summed: for short frame ranges, as each sample copies a stack) the sampler also copies up to `STK_COPY` bytes
of the suspended thread's stack and, after `ResumeThread`, unwinds the copy with the `.pdata` tables
(`RtlVirtualUnwind`), so it takes no lock while the thread is suspended. The unwind stops at the first frame outside
the copy; deeper callers are cut.

`NX2SARIA_PROFILE_SLOW=<ms>` keeps every thread's chains of the last half second in a ring (a thread outside the exe,
mostly waiting, copies 8 KiB of stack, one in it `STK_COPY`; a thread that ran no cycle since its previous sample is
not suspended and repeats its previous chain; a sample that took over 2 ms to stop its thread, which happens while
the thread is in a long kernel call, is written as `tstall <tid> <ms into the window> <ms stopping> <rip>...` with
the chain where the thread stopped) and, after a present at least `<ms>` after the previous one, writes that
interval's samples (plus the 20 ms before) to `<NX2SARIA_PROFILE>.f<frame>`: the profile format, one `stack 1` line
per sample, and `tstack <tid> <ms into the window> <rip>...` lines that keep the thread and the time. At most 64
dumps (`SLOW_DUMPS`).

Sizes and state:

- `PROF_SLOTS` (`1u << 16`): the Rip hash table, open addressing; a full table drops new Rips. `PROF_TOP` 4000: more
  than the 200 asked for: the report sums per function. `PROF_THREADS` 512.
- `STK_SLOTS` (`1u << 15`): the call-chain hash table, open addressing; a full table drops new chains. `PROF_STACKS`
  6000. `STK_COPY` (64 KiB): stack bytes copied per sample. `STK_SLACK` (64 KiB): zeros after the copy: a frame
  reaching past it reads zeros, and a zero Rip ends the chain; the slack of `stk_buf` is never written. `PROF_DEPTH`
  24: frames kept per call chain, the sampled Rip first.
- `ThreadSlot`: `cycles`, `depth` and `last` are for `NX2SARIA_PROFILE_SLOW` only: the thread's cycle count and chain
  at its previous sample.
- `only_render`: `NX2SARIA_PROFILE_RENDER=1`, sample the render thread only. `only_main`: `NX2SARIA_PROFILE_MAIN=1`,
  the guest main thread only. `only_replay`: `NX2SARIA_PROFILE_REPLAY=1`, the replay thread only, without call
  chains; `=2`: it and the replay workers (`runtime/vk/vk_exec.c` `rw_main()`), with chains under
  `NX2SARIA_PROFILE_CHAINS`. `chains`: call chains are kept (`NX2SARIA_PROFILE_RENDER` or
  `NX2SARIA_PROFILE_CHAINS`).
- `cur_frame`: the last presented frame. `frame_lo`, `frame_hi`: `NX2SARIA_PROFILE_FRAMES=<lo>-<hi>`: sample only
  while presents lo..hi-1 are pending. `world_lo`, `world_hi`: `NX2SARIA_PROFILE_FRAMES=w<lo>-<hi>`: only while the
  world clock is in [lo, hi) s (kept in us).
- `SLOW_RING` (`1u << 15`): about 0.5 s of samples of every thread. `stalls[STALL_RING]` (1024): `t` is when the stop
  began; `rip[PROF_DEPTH - 1]` holds the stop's ns. `slow_ns`: `NX2SARIA_PROFILE_SLOW` in ns, 0 off. `dump_lo`,
  `dump_hi`, `dump_frame`: the render thread asks for a window, the sampler writes it.

### `count_stack()`

`h |= 1`: 0 marks a free slot.

### `walk_mods_init()`, `walkable()`

Modules the unwinder walks through freely: the exe and system DLLs that stay loaded for the process's life. Any other
module (a driver or vendor DLL, which may load and unload while the game starts) is pinned while its frame unwinds,
and a module being unloaded ends the chain: unwinding one read its freed unwind tables and crashed the sampler
(`NX2SARIA_PROFILE_SLOW` at start). The unwind runs after `ResumeThread`, so taking the loader lock here is safe.

### `unwind_copy()`

Unwinds the stack copied to `stk_buf` from real address `lo` (n bytes). Registers that point into the copied range
are moved into the copy, so frame-pointer frames read the copy too. Returns the number of frames. A Rip in no loaded
module (or one being unloaded): the frame is kept, its callers are not. A function without unwind data is a leaf: the
return address is on top.

### `refresh_threads()`

Re-lists the process's threads: keeps the handle of a thread seen before, opens new ones, closes the gone ones. A
thread that exited loses its counts with the slot.

### `slow_dump()`

The sampler, between ticks: the samples of the window the render thread asked for.

### `is_worker()`

`NX2SARIA_PROFILE_REPLAY=2`: the replay workers are sampled with the replay thread.

### `sampler_main()`

- The timer's due time is -10000 (100 ns units): 1 ms.
- The world clock is -1 before it starts.
- `NX2SARIA_PROFILE_SLOW`: a thread whose cycle count did not change is still waiting, and its previous chain is
  repeated.
- `GetThreadContext` returns once the thread has stopped in user mode.
- A thread stopped between its `sub rsp` and the stack probe has Rsp in the guard page: touching it would fault, so
  the stack is copied only when Rsp's page is committed and not a guard page. What is copied is the committed stack
  above Rsp, up to the cap.
- The cycle count is read after `ResumeThread`: it then holds the cycles the sample itself cost.

### `prof_sampler_write()`

A sampler that does not end within 2 s is stuck: no half-written counts are written. The exe's own symbol table names
the addresses, so the profile needs no link map.

### `prof_commit_census()`

`NX2SARIA_MEM_CENSUS` (`runtime/diag/perf.c` `census_check()`): the process's committed memory by kind, and the
allocations of the process's own memory holding the most (`CENSUS_TOP` 12, kept sorted, largest first, by
`census_keep()`), so a log shows what grows: the guest's memory, a heap, the graphics driver's mappings or loaded
images. One walk of the address space with `VirtualQuery`, tens of milliseconds, so only now and then; it also counts
the guest ranges' committed bytes (`mem_guest_ranges`), whatever their kind, which `mem_guest_census` would walk a
second time.

Kinds of committed memory: images and mapped views by type; of the private memory, the write-combined views (device
memory the driver maps into the process; Windows charges it to the process's commit), the guest's ranges, and the
rest, the process's own ("other private"). The driver places its views anywhere in the free address space, below
`RT_ADDRESS_LIMIT` too: with every private region under it counted as guest, "other private" was 0 MiB in all 33
censuses of a session (2026-11-13) and the guest's figure grew with the textures.

Allocations by committed size: under 256 KiB, under 4 MiB, under 64 MiB, larger. A few large ones growing is a pool
or cache; many small ones is a stream of allocations that are not freed. An allocation's regions are contiguous: its
run ends where the next allocation begins, reserved gaps inside it (a heap segment's uncommitted tail) included. The
walk ends when the address wraps: the top of the address space.

### `prof_sampler_frame()`

From `perf_present()`, every present: the frame the sampler's `NX2SARIA_PROFILE_FRAMES` window counts, and a
`NX2SARIA_PROFILE_SLOW` dump of a present at least `slow_ns` after the previous one (`d` 0 on the first present). The
end of a `NX2SARIA_PROFILE_FRAMES` window writes the profile then: a test run ended by a timeout never reaches the
exit handlers (`prof_write()`).

## `runtime/diag/rdoc.c`

RenderDoc in-app capture (a debug tool; blocked for now, see
[controls.md](../controls.md#renderdoc-capture-blocked)). `NX2SARIA_RDOC=<frame>[,<frame>...]` captures those
presented frames; `NX2SARIA_RDOC_DIR` is the output directory (default `build/captures`); `NX2SARIA_RDOC_DLL`
overrides the library's path (`renderdoc.dll`, `librenderdoc.so`). Only the API fields used are declared in the file,
in the layout of RenderDoc's in-application API 1.4.1 (`RENDERDOC_API_1_4_1` of its `renderdoc_app.h`, which is
MIT-licensed), so no RenderDoc header is in the repository. In `RdocApi`, `fn2_10[9]` stands for the nine entries
from `SetCaptureOptionF32` to `UnloadCrashHandler`.

### `rdoc_init()`

Option ids of `eRENDERDOC_CaptureOption`: 8 RefAllResources, 9 SaveAllInitials, 12
AllowUnsupportedVendorExtensions. The back end imports guest pages with `VK_EXT_external_memory_host`, which
RenderDoc hides unless 12 is set.

### `rdoc_after_present()`

Called by `vk_be_present` after frame `frame` was queued and the present drained. A capture of frame N starts after
frame N-1's present finished and ends after frame N's present, so it holds frame N's submits and its blit/present.

## `runtime/diag/stutter_map.h`

The stutter map (`NX2SARIA_STUTTER_MAP=1`): for a play session, which frames hitched, in which area, what the frame
waited for, and which pipelines and shaders draws met missing, so the next pipeline list and shader preparation can
take them in. `stutter_map_on` is set once at start (`stutter_map_init()`); every hook returns at once while it is 0.

- `stutter_area()`: an area's load (`runtime/diag/perf.c` `perf_scene()`, from `runtime/hle/romfs.c`): hitches from
  here on are counted in that area, and those within `SM_LOAD_NS` of the load apart from play.
- `stutter_load()`: the texture pool began or ended making an area's textures (`runtime/vk/vk_exec_draw.c`
  `pool_loading`): frames within `SM_LOAD_NS` of it count as an area load's too, in the area of the last
  `stutter_area()`.
- `stutter_miss()`: a draw met its pipeline (`shader` 0) or one of its shaders (`shader` 1) missing and the replay
  thread spent `ns` on it before it was bound or skipped (`runtime/vk/vk_exec_draw.c`, beside `wait_note`). `vi`,
  `fi`: the vertex (or mesh) and fragment shaders' input ids, NULL when not known (still translating, or no fragment
  stage).
- `stutter_ready()`: a pipeline a draw missed became usable `ms` after the miss (`runtime/vk/vk_pipeline.c`
  `ready_note()`): `recorded` 1 when `pipeline-keys.bin` (or the seed list) named it, `fresh` 1 when one of its
  shaders was translated in this session (not prepared).
- `stutter_frame()`: every present (`runtime/diag/perf.c` `ft_frame()`): `d` the frame's interval, `part` the time it
  went to by cause (`names[i]`, n of them; the last one is the part nothing recorded explains), `cause` the one the
  frame is charged to.
- `STUTTER_CAUSES` (10): the number of causes, `perf.c`'s `FT_C_N` (`perf.c` asserts it), which `stutter_map.c`'s
  `cause_short` and the areas' counts are sized by; n is that many.
- `stutter_report()`: the map so far; every 5 minutes of play, at a focus loss in play and at exit.
  `stutter_report_as()`: `try_only` 1: only when its lock is free (0: it was not, nothing written).

## `runtime/diag/stutter_map.c`

What a play session's hitches were, area by area, and which pipelines and shaders the draws met missing, so a long
session says where the remaining stutters are and what the next pipeline list (`tools/pipeline_list.py` merge) and
shader preparation must take in. Off by default; while it is off every hook returns at its first test. For a reader
of the lines: [performance.md](../performance.md#the-stutter-map).

A hitch is a frame that stands out from the frames around it: its interval is over 2.5 times (`SM_HITCH_FACTOR`) the
median of the 32 frames before it (`SM_MEDIAN_N`) and at least 12 ms (`SM_HITCH_MIN_NS`), or over 25 ms
(`SM_HITCH_ANY_NS`), and both of the next two frames (`SM_AFTER`) are under two thirds of it (`SM_AFTER_RATIO` 1.5);
a frame over `SM_HITCH_SURE_NS` (100 ms) is one whatever follows. At 140 fps that is a frame over 17.5 ms followed by
shorter ones. The look at the next two frames keeps a change of pace out: the game drawing its menus or a scene at a
steady 60 or 30 fps after 140 is a slower rate, not a run of hitches, and the median takes the new rate within 16
frames. The decision waits two presents; the line and the counts carry what the frame itself waited for.

Frames within `SM_LOAD_NS` (3 s) of an area's load count apart, as the load's: an area load shows as a few long
frames that are not a stutter in play. An area load is the game's (`stutter_area()`) or the texture pool's
(`stutter_load()`: the pool makes an area's textures, `runtime/vk/vk_exec_draw.c` `pool_loading`), which comes
without the game's when it streams the next area in.

Consecutive long frames are one episode, a hitch counted once with their summed time: a change of pace is a run of
more than `SM_EPISODE_MAX` (8) long frames none of which stands out from the ones after it, and an episode ends at
its last frame that stood out once more than `SM_EPISODE_MAX` such frames follow that one. Three frames of 87, 91 and
96 ms after an area load (2026-11-13) each counted as a change of pace before, as each was followed by one as long.

What a hitch went to comes from the frame-time summary's split (`runtime/diag/perf.c` `ft_frame()`: pipeline,
translation, texture, fence, pacing, hand-off, replay, census, report, unattributed), with the draws that met a
pipeline or a shader missing between the previous present and this one (`stutter_miss()`, from the replay thread).
Each hitch gets a "stutter:" line, at most `SM_LINES_PER_S` (5) a second and `SM_LINES_MAX` (4000) in all; the counts
go on past that. Every `SM_REPORT_NS` (5 minutes) of play, when the window loses the focus in play
(`runtime/diag/perf.c` `session_report()`) and at exit, "stutter map" lines rank the areas by their hitches and the
pipelines by the time draws lost to them (`SM_TOP_AREAS` and `SM_TOP_PIPES`, 40 each), each with whether this install
had recorded it before (the pipeline list or an earlier run) and whether one of its shaders was translated in play
rather than prepared.

Threads: `stutter_area()` runs on the game's loading threads, `stutter_miss()` on the replay thread,
`stutter_ready()` on the pipeline workers (under `runtime/vk/vk_pipeline.c`'s `P.lock`; `sm_lock` is taken inside it
and never takes it), `stutter_frame()` and the periodic report on the present thread. The area table's play counts
belong to the present thread; the rest is under `sm_lock`. A report copies the map under `sm_lock` and sorts and logs
the copy without it.

Sizes and state:

- `SM_AREAS` 128: areas kept by name; later ones share the last slot. `SM_CAUSES` is `STUTTER_CAUSES`. `SM_PIPES`
  4096: pipelines kept for the ranking (open addressing). `SM_RING` 256: draws that met something missing, between
  two presents.
- `Area`: `ns`, `frames`, `over25` belong to the present thread; `misses`, `shader_misses`, `miss_ns`: draws that met
  a pipeline / a shader missing.
- `Pipe`: `key` is the vertex (or mesh) and fragment input ids' first four bytes each; 0: not known. `area`: where
  draws first met it.
- `n_areas` starts at 1: `areas[0]` is from the start to the first area load. `load_at`: `kern_now_ns` of the last
  area load. `ring_head` (under `sm_lock`): draws noted so far; `ring_tail` (present thread): draws taken so far.
- `recent[SM_MEDIAN_N]`: the last frames' intervals, every frame (hitches too, so a new pace is learnt).
- `Cand`: a frame that may be a hitch, waiting for the `SM_AFTER` frames after it: its line (made when it happened,
  with the draws that met something missing then), its area and what it is charged to. `t_s`: seconds since the
  first frame. `tail`: where the line's text after its head ("stutter: frame ... in <area>[, after its load]:")
  starts.
- `ep`: the episode being gathered: decided candidates of consecutive frames, the first one's line and area
  (`cand_decide()`). `last`, `total`: the last frame in it; the frames' summed intervals. `n`, `hitch`, `open`:
  frames in it; one of them stood out from the frames after it (or was over the sure line). `hit_last`, `hit_total`,
  `hit_n`: the episode up to its last frame that stood out: that frame, the summed intervals and the frames to it (0:
  none stood out).
- `pace_run`: more than `SM_EPISODE_MAX` long frames, none standing out: a new pace, until it ends.

### `pipe_at()`

`sm_lock` held. The entry for a key (mesh apart), made on first use; NULL once the table is full.

### `stutter_miss()`

When the present has not taken the oldest ring entry yet, it is overwritten and counted in `ring_lost`.

### `take_misses()`

The draws noted since the previous present: counted for the area, and the first few named in the line.

### `median_recent()`

The median of the last `SM_MEDIAN_N` frames' intervals (fewer at the start).

### `episode_close()`

The episode ends: a hitch with its frames' summed time, counted in its first frame's area and charged to that frame's
cause, and its line logged; a single frame that did not stand out from the ones after it is a change of pace. An
episode's line has the head with the episode's time; the parts and the draws are its first frame's.

### `cand_decide()`

A candidate the `SM_AFTER` frames after it have been seen for: it stands out (a hitch) or the frames after it are as
long (paced). It joins the episode of the frame before it, or starts one. More than `SM_EPISODE_MAX` paced frames
after the episode's last frame that stood out (or from its start, when none did) are a change of pace: they and the
paced frames right after them are left out, and an episode with a frame that stood out ends at that frame and is
counted. At 40 fps or less every frame is over `SM_HITCH_ANY_NS` and so a candidate: without the cut at the last
frame that stood out, the first hitch took in every frame after it until the rate rose, and a 30 fps session counted
one hitch seconds long in place of its five.

### `stutter_frame()`

This frame is one of the frames after each waiting candidate (at most two others wait). An open episode closes when
the frame after its last was no candidate.

`cause_short` is in the order of `runtime/diag/perf.c`'s causes (`FT_C_*`); "hand-off" is the wait for the present
thread to take the previous picture, "census" `NX2SARIA_MEM_CENSUS`'s walk of the address space, "report" the memory
report on the presenting thread.

### `exit_tail()`

At the exit: the hitches in the run's last frames, which `cand_decide()` and `episode_close()` would have counted had
the run gone on: the open episode, and the candidates still waiting for their next frames (the last two frames, three
in a run of long ones). A waiting candidate counts as one that did not stand out unless it is over
`SM_HITCH_SURE_NS` (a hitch whatever follows), and joins the episode of the frame before it as `cand_decide()` joins
it; a run past `SM_EPISODE_MAX` without a hitch is a change of pace, which goes on while candidates are paced. Added
to the exit's printed totals only, and only read: the exit's report runs on another thread (`runtime/diag/perf.c`
`perf_summary_flush()`, from the window's close or the game's exit) while the presenting thread, which owns this
state, may still be in `stutter_frame()`; a race there miscounts these last frames at worst. Other reports leave them
to the next one.

### `stutter_report_as()`

The map as a report reads it: copied under `sm_lock`, then sorted and logged without it. The replay thread
(`stutter_miss()`) and the pipeline workers (`stutter_ready()`, inside `runtime/vk/vk_pipeline.c`'s `P.lock`, which
the draw path takes too) wait for `sm_lock`, and a report sorts up to `SM_PIPES` entries and logs about 85 lines. The
pipelines' area names are read from the copy (their area index is below the copied count).

`try_only`: `sm_lock` is only tried (a fatal error's summary, `runtime/diag/perf.c` `perf_summary_flush_try()`, on a
thread that may hold it); 0 when it was held and nothing was written.

## `runtime/diag/sysinfo.h`

What a bug report needs about the machine. It was written for the menu's "Copy system info" and "Open log folder"
rows, which the menu no longer has. Its callers now: the bug capture takes `sysinfo_text()` and `sysinfo_log_folder()`
(`runtime/diag/bugcap.c`), and the performance panel and the session summary take `sysinfo_cpu_name()`.

- `sysinfo_text()`: plain text, one item per line: OS and build, CPU, RAM, the GPU and its driver as the Vulkan
  device reports them, and every setting with a value (the selected settings file, the menu, the environment).
  Technical text for the developers, in English like the log. The length written (truncated to n - 1), or 0 on
  failure.
- `sysinfo_cpu_name()`: the CPU's brand string ("AMD Ryzen 7 7800X3D 8-Core Processor"); "" when the CPU has none.
- `sysinfo_log_folder()`: the folder with the log: the folder of `logs\nx2saria.log` when this run writes it, else of
  the file stderr goes to, else of `nx2saria.exe`. 0 on failure.

## `runtime/diag/sysinfo.c`

The OS calls sit behind `runtime/platform/platform.h`; the GPU and driver come from the Vulkan device the game
renders on.

### `private_setting()`

The private settings (`runtime/cfg/cfg_private.h`) stay out of a text that ends up in bug reports: only whether they
are set. The header's one list serves every place that hides them.

## `runtime/diag/tracker_api.c`

The tracker API (`NX2SARIA_TRACKER_API`, `tracker_api` in the settings file; off by default): a read-only view of
the game's memory for a map tracker running on the same PC, through `127.0.0.1:<tracker_api_port>` (6545 by default).
The protocol is the newline-delimited JSON one that trackers already speak to a console emulator's debug server, so a
tracker that reads this game there reads it here too; [tracker-api.md](../tracker-api.md) describes every field, and
its sections "Safety", "Cost" and "The log" are the feature's side of the notes below.

The protocol, as the code answers it:

- A request is one line holding a JSON object `{"id": <number or string>, "method": <name>, "params": {...}}`; the
  answer is one line `{"id": <the same>, "result": {...}}` or
  `{"id": ..., "error": {"code": <n>, "message": <text>}}`, in the order the requests came. Nothing is ever sent
  without a request.
- `status`: whether the game runs (it has presented a frame), the title id, this port's name ("nx2saria"), main's
  build id, the game version, the port's version, the API's version (`TRACKER_API_VERSION`, 1), the methods, and the
  folder of the save the game loads and writes (`runtime/hle/fs_host.c` `fs_save_dir()`), so a tracker that reads the
  save files reads this game's and not another install's.
- `read_memory {address, size, as: "hex"}`: up to `READ_MAX` bytes of guest memory as lowercase hex.
- `search_memory {pattern (hex), start, end, regions: "all" | "heap", max_hits, budget_ms}`: the addresses where the
  bytes appear. "all" reads the game's modules first and ends there when they hold the pattern (a tracker finds main
  by its MOD0 header that way, and asks for more copies than main has); then the heap, then the other regions.
- `nx2saria.state`: this port's own summary, which needs no knowledge of the game's structures: the area the game is
  in and Link's position and rotation (`runtime/diag/debug_tools.c` `dbg_area()`, `dbg_player_pos()`,
  `dbg_player_rot()`), whether the game data is there, the presents so far, and where main and the heap are.
- `nx2saria.game_data {kind, hash, if_digest, filter}`: the game's progress values of one kind (Bool, Int, Enum,
  String64, String64Array, Int64, Bool64bitKey) with the keys the save files know them by, so a tracker that reads
  the save files reads the running game the same way, without the game-data manager's place or layout.

**What can be read.** Guest addresses are host addresses in this runtime (`runtime/host.h`), and `mem_query` answers
for the whole address space below 2^39, the host's own allocations included (as memory the game cannot have). So a
read or a search covers only the game's own regions (`runtime/memory.c` `mem_guest_ranges()`: its modules, stack,
heap, mapped physical memory and thread storage, never the host-function window), and within them what the game's
memory map says it can read. The runtime's memory, and what it read (the keys, the settings), is outside those
regions. Nothing is written.

**How it reads.** A copy, `plat_mem_read`, which answers 0 instead of faulting, so a page the game unmaps meanwhile
or the GPU wrote back and the CPU has not fetched yet (`runtime/memory.c` `lazy_fault()`) fails the read and is not
fetched. A read commits heap pages the lazy heap left reserved (they read zero), as `runtime/pacing/frame_rate.c`
`read_guest()` does. A search copies 1 MiB pieces of the committed, readable runs (`plat_mem_state`), and outside the
modules only the pages that hold memory now (`plat_mem_resident`): reading a page the game never wrote would allocate
it, and a tracker searches the heap often while it finds no player. No lock of `runtime/memory.c` is held while
copying.

**Cost.** Nothing on the game's threads. One host thread listens, and each client (`CLIENTS_MAX`, 4, at most) has a
host thread of its own; none holds a virtual core (`runtime/hle/kernel.c` `core_take()`: only guest threads take
one). Searches run one at a time, each until its budget from the request's arrival (`BUDGET_MAX_MS`, 3000, at most;
`BUDGET_DEFAULT_MS` 1000), at most `SEARCH_BYTES_PER_MS`, and all of them together read at most
`SEARCH_BYTES_PER_MIN` a minute; past that a search answers what it found.

**Safety.** Off unless the player turns it on; bound to 127.0.0.1 alone (`runtime/platform/platform.h`
`plat_tcp_listen_loopback()`), so nothing on the network reaches it; read-only. A line whose first byte past blanks
is not '{' closes the connection at once: a web page that sends to the port (an HTTP request, a TLS handshake)
arrives that way, and it could never read an answer anyway (this is no HTTP server). A line of `LINE_CAP` bytes
without its end, a request left unfinished for `LINE_MS`, a client silent for `IDLE_MS`, and a fifth client are
closed.

**The log.** One line at start (off; or on, where it listens, and where the heap is), a line when a client sends its
first request and one when it leaves, with its counts (`CLIENT_LOGS`, 20, per session), the first answer of each
method and the first refusal of each kind, and while clients ask a summary every `SUMMARY_MS` (10 minutes;
`SUMMARY_LOGS`, 36, per session). A connection closed without a byte is a program checking whether the port is taken
(a tracker does that every few seconds before it starts a reader of its own): the first one is logged, the others
are counted in the summaries.

Constants:

| Name | Value | Meaning |
|---|---|---|
| `TITLE_ID` | "0400486024DEC000" | the game's title id (`launcher/gamefiles.py`, `TITLE_ID`) |
| `PORT_NAME` | "nx2saria" | `status`'s "emulator": what the client shows as the game's source |
| `HEAP_POINTER_FLOOR` | 0x1000000000 | trackers take a heap pointer below 64 GiB for none ([tracker-api.md](../tracker-api.md)) |
| `LINE_CAP` | 4096 | a request is small: the longest, a search, carries a 128-digit pattern |
| `LINE_MS` | 5000 | from a request's first byte to its end |
| `READ_MAX` | 256 KiB | bytes a `read_memory` may ask for |
| `PATTERN_MAX` | 64 | bytes of a search pattern |
| `PIECE` | 1 MiB | bytes a search copies at a time (`plat_mem_resident`'s limit) |
| `SEARCH_BYTES_PER_MS` | 4 MiB | 4 GiB/s at most for one search |
| `SEARCH_BYTES_PER_MIN` | 8 GiB | a minute, copied by all of them: two seconds of reading at most; `tests/tracker_api_check.py` builds with a smaller one |
| `IDLE_MS` | 60000 | a client that sends nothing this long is closed |
| `SEND_MS` | 2000 | an answer the client does not take this long closes it |
| `ACCEPT_ERRORS_MAX` | 50 | failed accepts in a row before the listener is made again |
| `CLIENT_LOGS` | 20 | `tests/tracker_api_check.py` opens more clients than that, and builds with its own |

State: `methods_text`: the methods this run answers, as `status` lists them (`tracker_api_init()`). `search_tokens`
(under `search_lock`): bytes searches may still read. `win_req`, `win_errors`, `win_bytes`, `win_checks`: the
summary's window, atomics.

### `client_log_slot()`

1 while the session's client lines last; the last one says what follows.

### `shown()`

Bytes a client sent, for the log: printable ASCII as it is, anything else as '.', at most `cap - 1` of them.

### `str()`

The file has a strict JSON reader for the one object a request is: its "id", "method" and the flat members of
"params". In `Req`, `id` is the id as it goes back: a JSON number, or the string as sent with its quotes ("" none);
`method` is printable ASCII ('.' for anything else), so it can go into the log.

`str()` reads a JSON string into `out` (`cap` 0: skipped), escapes decoded, `\u` as '?'. `*cut` (may be NULL) is set
when it did not fit `cap - 1` bytes. 0 when it is not a JSON string.

### `number()`

A JSON number as JSON spells it: `-?(0|[1-9][0-9]*)(.[0-9]+)?([eE][+-]?[0-9]+)?`. 0 when there is none.

### `parse_request()`

The request on one line; 0 when the line is not one JSON object, or its id does not fit (the connection then closes:
an answer without its id would be taken for the answer to another request). A string id is kept as sent, quotes and
escapes included: valid JSON.

### `param_u64()`

An address or a count: a JSON number (a whole one), or a string of hex digits after 0x, or of decimal digits. 0 when
it is absent or anything else.

### `out_text()`

Text that goes into a JSON string: the game's area names and file names are plain ASCII; anything else is dropped.

### `out_json_string()`

A string as JSON spells it, its bytes kept (a path is UTF-8): quotes and backslashes escaped (a Windows path has
them), control characters as `\u00XX`.

### `do_status()`

`nx2saria.game_data` is listed where the game data is known (1.1.1).

### `guest_regions()`

The game's own regions (`mem_guest_ranges`) a read or a search may cover: every one but the host-function window.

### `guest_copy()`

Copies [a, a + n) when it lies inside one of the game's own regions and the game can read all of it now: 1, else 0.

### `take_bytes()`

Takes n bytes of the searches' minute (`search_lock` held): 0 when the minute's bytes are spent.

### `copy_scan()`

[a, a + n) copied and scanned, when the minute's bytes allow it; a copy that fails (a page went away meanwhile) is
scanned page by page.

### `resident_scan()`

The runs of resident pages of [a, a + n), each copied and scanned; nothing when residency is unknown.

### `readable_run()`

The end of the run of committed, readable pages at a (at most `limit`), or 0 when the page at a is not one of them;
`*skip` then gets the end of the run of pages like it.

### `search_region()`

The pattern in [a, end), a range of the game's memory map it can read, in pieces of at most `PIECE` bytes, each piece
starting len - 1 bytes before the last one's end, so a pattern across two pieces is found once. A piece ends at most
`PIECE` past a's page, so it spans at most the pages `plat_mem_resident` answers for. A run that is reserved and
never committed (the lazy heap), inaccessible, or not ours has nothing to read and is skipped.

In `Search`: `buf` is `PIECE` bytes, the copy being searched; `lo`, `hi` the asked range, [lo, hi); `resident_only`:
outside the modules, the pages that hold memory now.

### `search_range()`

Every readable part of [base, base + size) clipped to the asked range, by the game's own memory map (`mem_query`).

### `do_search()`

The budget counts from the request's arrival, waiting for another search included. One search at a time
(`search_lock`): each one reads memory for up to its budget. The modules first, then the heap, then the rest; "all"
ends after the modules when they hold the pattern. The log gets the first answer, and the first that found something:
a tracker's first searches look where this port has nothing.

### `gd_records()`, `do_game_data()`

The game data (`nx2saria.game_data`). The game keeps its progress in a game-data manager,
`**[main+DBG_GAME_DATA_SLOT]` in version 1.1.1 (`runtime/diag/debug_tools.c` `gmd()`). Its values are the ones the
save files hold, under the same keys: the MurmurHash3 of each value's name. For each kind of value the manager has a
section of 0x60 bytes at the kind's number times 0x60, the kinds numbered as the save files number them: the entries'
count (u32) at +0x10, the entries at +0x18, a table of {u32 hash, u32 index} pairs at +0x20 (hash 0 for an empty
pair, probed from hash % size on) and the table's size (u32) at +0x2c. Every entry of a kind starts with the same
pointer into main, the kind's vtable (Bool's and Int's are known); that tells a real section from one read at the
wrong place or stride. The game fills the sections at start, before its first frame, with its game data list's
defaults (a save's load then sets the save's values); until then their counts are 0.

The placed objects' flags (Gold Skulltulas, Heart Pieces in the world: each object's flag under its placement's
64-bit hash) are a table of 16-byte entries sorted by their u64 key, a u32 class at +8 (bit 0x40: the game clears it
again, a respawning object) and the value byte at +12; its count (u32) is at manager+0xc58 (`GD_KEYS_COUNT`) and its
address at +0xc60. A value is read as the game holds it now: a change the game has queued shows after its next
update.

The answer is each value with its key, sorted by key, as little-endian records in hex: a tracker that reads the save
files reads the running game with the keys it already has. A digest of the records lets a tracker that polls skip an
answer that did not change (`if_digest`). Each request copies one kind, through `guest_copy()` (the game's own
regions only), on this thread, one request at a time (`gd_lock`), and refuses rather than answering a value it could
not read: -32002 when asking again may answer (no game data yet, a copy the game changed meanwhile, a value it cannot
read now), -32003 when it will not (another game version, a kind not laid out as 1.1.1 lays it out).

The kinds (`GdKind`: `stride` bytes of one entry; `at`: for `GD_INLINE` the value in the entry, for `GD_TEXT` the
text's pointer, for `GD_TEXTS` the texts' count; `size`: the value's bytes for `GD_INLINE`; `record`: bytes of one
record in the answer, 0 when they vary; `max`: most entries taken for the kind; `vtable`: the entries' vtable,
main+this, where it is known, 0: any one pointer into main):

| Kind | Number | Layout | Stride | At | Size | Record | Max | Vtable | The record |
|---|---|---|---|---|---|---|---|---|---|
| Bool | 0 | `GD_INLINE` | 0x18 | 0x16 | 1 | 8 | 200000 | main+0x4c20658 | u32 hash, u32 the value (0 or 1) |
| Int | 2 | `GD_INLINE` | 0x28 | 0x1c | 4 | 8 | 100000 | main+0x4c20718 | u32 hash, s32 value |
| Enum | 6 | `GD_INLINE` | 0x38 | 0x1c | 4 | 8 | 50000 | any | u32 hash, u32 the hash of the value's name |
| String64 | 16 | `GD_TEXT` | 0x108 | 0x68 | | 68 | 10000 | any | u32 hash, 64 bytes of text, zero after its end |
| String64Array | 17 | `GD_TEXTS` | 0x90 | 0x80 | | varies | 2000 | any | u32 hash, u32 n, n x 64 bytes of text |
| Int64 | 22 | `GD_INLINE` | 0x30 | 0x20 | 8 | 12 | 50000 | any | u32 hash, s64 value |
| Bool64bitKey | 32 | `GD_KEYS` | 16 | | | 16 | 200000 | any | u64 key, u32 class, u8 set (0 or 1), 3 zero |

`GD_FORMAT` 1: the records' layout above; a change of it gives another number. `GD_TABLE_MAX` 1000000: pairs of a
kind's {hash, index} table. `GD_TEXT_ELEMENTS` 64: texts of one String64Array value. `GD_TEXTS_STRIDE` 0x50: a
String64Array element, its text at +0x0c (`GD_TEXTS_AT`). `GD_ANSWER_MAX` 8 MiB: bytes of records in one answer
(String64Array's texts could be many). `gd_not_loaded` ("the game has not loaded its game data yet") is the usual
wait, logged once.

`gd_records()` makes one kind's records, sorted by key (malloc'd): 1, or 0 with the answer's `err` and `why`.

- A kind the loaded game data holds nothing of gives no records (and no error).
- The {hash, index} pairs: all of them, or the one of the hash, probed as the game probes
  (`runtime/diag/debug_tools.c` `gmd_entry()`). Each index must be below the count and appear once, each hash once.
- The entries: every one (each checked for the kind's vtable), or the one asked for and the first. A vtable of 0 is
  an entry the game is still making.
- `GD_TEXTS`: a record is 8 bytes and the texts of each value; the element count is at +0x80, their address at +0x88.
- At the end the section must be as it was when the copies began: else the indices may be of another table than the
  entries.

`do_game_data()` logs a refusal there: the wait for the game data once, anything else once per kind.

### `gd_ready()`

The manager is there and the game has loaded its game data.

### `text_record()`

A text of 64 bytes as it goes into a record: the bytes up to its first NUL, zero after it.

### `gd_keys()`

The placed objects' flags, sorted by key: all of them, or the set ones. The head read is the count (its low 32 bits)
and the address. A count over the kind's maximum is read a second time: the same count twice is no load in progress.
The game keeps the entries sorted, without key 0, so a key not above the one before refuses the answer. The kept
records are written in place, at or before the entry being read.

### `gd_fields()`

The fields of a kind's section this reader uses, as they are now: count, entries, table, table size.

### `gd_unsupported()`

A kind that does not read as 1.1.1 keeps it: -32003 when its section is still as it was read (else the game was
changing it, as while it makes its game data, and -32002).

### `gd_loaded()`

The game has loaded its game data: its Bool section has entries.

### `not_a_request()`, `starts_wrong()`

`starts_wrong()`: the first byte past blanks of the line being received is not '{': no JSON request starts that way.
In `Client`, `sent_any` says it sent a byte (a connection closed without one was a port check) and `why` how it
ended, for its closing line.

### `handle_line()`

One whole line: 1 when the connection goes on.

### `client_main()`

- `plat_thread_priority(PLAT_PRIO_NORMAL)` at its start: a Linux thread starts at its creator's level, the
  listener's lower one.
- A large answer's room (over 1 MiB) is not kept.
- A line that starts wrong is closed at its first byte, before any line end.
- Bytes that keep coming do not finish a request either: past `LINE_MS` the client "left a request unfinished for
  5 s".
- A client that sent nothing and closed the connection was a check whether the port is taken.

### `listener_main()`

After a failed accept it sleeps 100 ms: the listener stays readable while no descriptor is left, and this keeps it
from a busy loop.

### `tracker_api_init()`

The build id is copied here, once: its getter is not thread-safe. The start line and the refusals name the methods as
`status` answers them.
