# launcher

The launcher: the window, the installation of the build tools and of the game, the updates, the mods, the saves and
the session reports. What it does for a player is in [launcher.md](../launcher.md), [install.md](../install.md) and
[telemetry.md](../telemetry.md); this page holds the notes of its code, one section per file in the order of the file
names. These files still keep their comments until their notes are here: `launcher/context.py`, `launcher/jobs.py`,
`launcher/locks.py`, `launcher/mods.py`, `launcher/mods_job.py`, `launcher/native/common.h`,
`launcher/native/nx2saria.c`, `launcher/native/res.rc`, `launcher/native/setup.c`, `launcher/net.py`,
`launcher/paths.py`, `launcher/platform/__init__.py`, `launcher/platform/linux.py`, `launcher/saves.py`,
`launcher/settings.py`, `launcher/start.pyw`, `launcher/steam.py`, `launcher/telemetry.py`, `launcher/toolchain.py`,
`launcher/ui/__init__.py`, `launcher/ui/app.py`, `launcher/ui/art.py`, `launcher/ui/picture.py`,
`launcher/ui/selftest.py`, `launcher/ui/sound.py`, `launcher/updater.py`, `launcher/versions.py`,
`launcher/winsys.py`, and `launcher/build.py`.

## `launcher/__init__.py`

The package of the launcher: it installs the build tools, builds the game from the player's own dump, starts it and
keeps the port up to date ([launcher.md](../launcher.md)).

It uses the standard library only, plus numpy for drawing the window (`launcher/ui/art.py`), zstandard where Python
lacks `compression.zstd`, and truststore for Windows' own certificate check (`launcher/net.py`). tkinter draws the
window; everything that does work runs without it (`launcher/cli.py`).

## `launcher/__main__.py`

`python -m launcher [--selftest [--shots DIR]]`: the launcher window (`launcher/ui/app.py`).

## `launcher/cli.py`

The launcher without its window, for scripts, tests and anyone who prefers a console
([launcher.md](../launcher.md#command-line)). The first line of `--help` is the string `DESCRIPTION`.

```
python -m launcher.cli check                       is there a newer release?
python -m launcher.cli update                      download and stage it, then finish in the new version
python -m launcher.cli tools                       install the build tools and zbic
python -m launcher.cli install --dump F --game D   build the game (the window's Begin Installation)
python -m launcher.cli rebuild [--game D]          rebuild after an update (the window's Rebuild the Game)
python -m launcher.cli fonts [--dump F]            decode the game's font for the window
python -m launcher.cli continue-update OLD         what the new version runs after `update`
```

In an installed launcher it is `python\python.exe app\<version>\launcher\cli.py <command>`: the file puts the release
folder on `sys.path` itself, as `launcher/start.pyw` does, and sets `__package__` so that its relative imports work
when it is run as a script (the `# noqa` lines of those imports stay). The exit code is 0 on success, 1 on failure and
3 when cancelled.

### `follow()`

Runs a job in the console and prints each step's state as it changes. The steps are read under the job's lock
(`launcher/jobs.py` `Job.locked()`), so each is seen as one change of the worker left it.

### `main()`

`check` reads the feed and changes nothing, so it runs beside an open launcher and takes no lock. Every other command
changes the launcher's folder or the game's, as the window's jobs do: it holds the folder's lock (`launcher/locks.py`
`FileLock`) from before it writes the settings until it ends, and is refused while the window or another command holds
it.

### `run()`

One command. `held` is the launcher folder's lock.

- `update` is refused before anything is downloaded while the game runs: the new version rebuilds the game
  (`launcher/jobs.py` `ContinueUpdateJob`), which cannot replace a running executable. The window's Updates row refuses
  the same way (`launcher/ui/app.py` `App.start_update()`).
- `update` lets go of the lock before it starts the new version's `continue-update`, which takes the lock itself.

## `launcher/commit.py`

The Windows commit check before Play, kept independent of the launcher's window and of its graphics dependencies.

`DEFAULT_MEM_NEED_MB` (24576) is `MEM_NEED_DEFAULT_MB` of `runtime/mem_need.h`. All amounts are in MiB.

### `game_commit_need()`

The game's estimated commit in play, with the same precedence of environment and settings file as `runtime/cfg/cfg.c`.

### `commit_warning()`

The text of the optional question before Play, from the system's commit headroom at that moment and the game's
estimate; empty when `needs_commit_warning()` says there is enough.

### `fatal_summary()`

Keeps the advice a player can act on (close programs, enlarge the page file) in the shortened fatal message the
launcher shows, for the three Win32 codes of a commit that Windows could not promise.

### `set_warning_enabled()`

Saves the player's choice about the warning and leaves its value in `launcher.log`.

### `play_memory_request()`, `play_memory_warning()`, `play_memory_response()` and `confirm_play_memory()`

`play_memory_request()` samples Windows commit and the page-file mode, logs the existing values and policy, and returns
`None` when the sample is unavailable or no question is due: enough memory, the warning disabled, a system-managed or
unknown page-file mode, or an estimate that disables the check. Otherwise it returns the remaining memory and game
estimate. `need=None` with `unbounded=True` represents an infinite estimate without a non-finite JSON number.

`play_memory_warning()` recreates the existing warning from the request, including the infinite estimate.
`play_memory_response()` logs the player's answer and returns it; an unavailable dialog is logged and permits Play.
`confirm_play_memory()` retains the synchronous entry point used by Steam and combines these functions with the
existing native dialog, including the fallback that permits Play if the probe or dialog is unavailable.

## `launcher/crash_offer.py`

The crash log a player may send after an abnormal end: when one is due, what it holds and how it is sent. No Tk here.

### `game_ended()`

Called at every end of the game, by `App.watch_game()` and by `play_only()`. It never uses the network: for an end
worth a question it writes `logs/crash-offer.json` in the game folder (the end's kind, the sentence for the player,
and the size and modification time of the game's log) and the window asks later. It runs after the launcher added
its own line to the game's log (`launcher/game_session.py` `note_exit_in_log()`), so the recorded size and time are
the log's final ones.

No file is written, and one line says why, when the end has no sentence (a normal end: no line either), when the game
was asked to close (`launcher/game_session.py` `end_asked()`), when the setting `crash_log_offer` is `off`, and when
there is no server (no line: nothing of session reports exists then).

Lines: `telemetry: crash log offer: waits for the launcher's main screen (end: <kind>)`, and `... not made: <why>
(end: <kind>)`.

### `pending()`

The waiting question, or `None`. The file is dropped, with a `telemetry: crash log offer: dropped: <why>` line, when
the game's log no longer has the recorded size and time (the game ran again and moved that log away, or wrote to it),
when the setting was switched off since, and when the file cannot be read. Size and time together are the identity of
"the log of that session": the game starts a new `nx2saria.log` at each start and the launcher writes to it only once,
before the stamp is taken.

### `log_parts()`

The start and the end of the game's log as lists of lines, and whether together they are the whole log.

- The end is the last `TAIL_LINES` (200) lines within `TAIL_LIMIT` (56 KB), the last ones kept. Over 184 logs, 200
  lines weighed 46 KB at the median and 58 KB at most, so the line count is the limit that usually applies; the
  comment's 24 KB (`launcher/telemetry.py` `log_tail()`) holds about 107 lines.
- The start runs from the first line to the line `patches:` inclusive, within `HEAD_LINES` (100) and `HEAD_LIMIT`
  (16 KB). Over the same logs that line was line 84 at the median and line 94 at most, 11.6 KB and 13.5 KB. A log
  without it (a crash before it) gives the start up to the caps.
- The start stops where the end begins, by byte offset in the file, so no line is sent twice; a short log is all in
  the end, and the start is empty.
- Only `HEAD_READ` (128 KB) from the start and `TAIL_READ` (512 KB) from the end of the file are read: the log may
  be 64 MiB.
- Lines are split on line feeds only, as the server counts them, and each is written short and stripped of control
  characters as a comment's log lines are.

### `clean()` and the lines sent

Every log line that leaves the PC goes through `launcher/telemetry.py` `sent_line()`, which writes the personal
folders short with `tools/private_paths.py` `shorten_always()`: `NX2SARIA_LOG_SHORT_PATHS=0` in the launcher's
environment does not turn it off, and a log the game wrote in full (`log_short_paths = 0`) is written short on the
way out. The same holds for a comment's attached lines (`log_tail_lines()`) and for the error line of the summary
made after a hard crash (`synthesize_crash()`). The file on disk is only read. *See What Is Sent* shows the body, so
it shows the short form.

`sent_line()` returns a `Line`, a string that remembers whether the shortening changed it. `shortened_note()` makes
one line for `launcher.log` from the lines that are really sent (after the body's trimming), logged once per send,
by `finish()` here and by `Reporter.finish_feedback()`: `telemetry: personal folders written short in N of M log
lines for sending; the log on disk is not changed (<why>)`. It is written when the switch is off in the launcher's
environment, or when any line changed (the game's log had the folders in full). A log that was already short adds
no line. The line holds counts only.

### `session_summary()`

The session report that goes with the crash log: the pending one, else the one already sent or declined, but only
when its file was written after the log began (`launcher/telemetry.py` `log_started()`). A game that was killed
writes no summary, and the one on disk is then an earlier session's, which must not be attached to this crash.
Nothing when Session Reports is *Off*, the rule of a comment.

### `build()`

The body, at most `BODY_LIMIT` (96 KB, the server's cap). JSON escapes can grow the log lines (each backslash of a
Windows path becomes two bytes), so a body over the cap loses the oldest quarter of the end's lines until it fits.
`text` is always empty: the player wrote nothing. The body has `kind` and `log_head`, which a comment's body never
has, so a comment still works against a server older than crash logs.

### `finish()`

The server's answer. Nothing is tried again, whatever the answer: the question was asked once and its file is gone.
A `400` means a server older than crash logs, or a launcher version that is not a published release (a development
build); it sets `Offers.refused`, and `App.offer_crash_log()` shows no more questions in that run of the launcher,
dropping what waits with `telemetry: crash log offer: not shown: the server refused one in this run`. A `410` closes
reports for this launcher version, as for a session report.

Lines: `telemetry: crash log sent (HTTP 201, <start> + <end> lines, <bytes> bytes of log, <bytes> bytes in all;
<counts>)`, `telemetry: crash log not sent (HTTP 400): this server does not take it; not tried again, and no more
offers in this run of the launcher (<counts>)`, `telemetry: crash log not sent (<answer>); not tried again
(<counts>)`.

### `log_state()`

The line of a launcher's start, and of each change of the setting: `telemetry: crash log offer: on (Settings >
Session Reports > Ask After a Crash)`, or `off`.

## `launcher/errors.py`

Errors after the window is up, written where a player's report finds them.

`launcher/start.pyw` writes an error that keeps the window from opening into `data/logs/crash.log`. Once the window is
up, Python reports an exception in a window callback (a click, a key, a timer) or in a background thread only on
stderr. stderr is nowhere under `pythonw.exe` on Windows, and on Linux it is the terminal or the session's error file.
The window then goes on as if nothing happened. A fault in native code (Tk, SDL3 through ctypes) ends the process with
nothing written at all. This module sends the first two to `launcher.log` (one line) and `crash.log` (the traceback),
the third to `crash.log`, and on Linux it logs the signals that end the window.

Native faults are caught off Windows only. On Windows `faulthandler` also reports exceptions that are raised and then
handled, such as the COM and RPC exceptions of the shell's file chooser and its extensions, and `crash.log` would then
read like a crash every time the player chose a file.

- `END_SIGNALS` are the signals that end the window when nobody closed it: SIGTERM (kill, a logout, a shutdown) and
  SIGHUP (the terminal that started it was closed).
- `_fault_file` stays open for the process's life: `faulthandler` writes to it from the fault itself.
- `WAKE_TICK_MS` (250) is how often a window that cannot listen to a socket runs Python to see a signal (Windows).

### `summary()`

`<type>: <message> (in <function>, <file>:<line>)` for the innermost frame of the traceback.

### `record()`

One line into `launcher.log` and the whole traceback into `crash.log`. Both shorten the account's folders
(`tools/private_paths.py`): players attach them to public issues.

### `install()`

Sends exceptions in background threads to `record()` and, off Windows, fatal faults (a crash in native code) to
`crash.log` through `faulthandler`. It returns whether `faulthandler` is on; `finish()` undoes that part.

### `finish()`

At the window's end: `faulthandler` off, and `crash.log` removed when it was opened for `faulthandler` and nothing was
written. A player attaches the file that is there, so an empty one must not stay.

### `window_hook()`

For `tk.Tk.report_callback_exception`: an exception in a window callback is recorded, and the window goes on, as Tk
lets it.

### `on_end_signals()`

Sets `handler(name)` for each of `END_SIGNALS` this system has, in place of the default that ends the process without
a word, and returns the names it took. A signal that cannot be set (not the main thread, or not on this system) is
skipped.

### `wake_for_signals()`

Has the window's event loop act on a signal at once. Python runs a signal's handler only when it next runs Python
code, and an idle Tk window waits inside Tcl for its next event: a SIGTERM to a window nobody touches (a logout, a
shutdown) waited for that event, and a window that got none never closed.

- On POSIX the signal writes a byte to a socket (`signal.set_wakeup_fd`) that the window listens to
  (`createfilehandler`), which wakes the loop and runs the handler. By the time the socket's callback drains it, the
  handler has already run: Python ran that callback.
- Tk on Windows cannot listen to a socket: there a timer runs Python every `WAKE_TICK_MS`. `use_socket=False` takes
  the timer anywhere (tests). The timer stops by itself when the window is gone.
- It returns `(how, undo)`: `how`, for the log, is `woken through a socket` or `checked every 0.25 s`; `undo()` puts
  back what was there before, and is called after the event loop.

## `launcher/feed.py`

Where updates come from ([launcher.md](../launcher.md#updating)).

Every release carries `NX2Saria-update.json`, which `tools/make_launcher.py` writes:

```
{"format": 1, "version": "0.3.11", "build": "0.3.11+abc1234", "notes": "...",
 "app":     {"name": "NX2Saria-app-0.3.11.zip", "size": n, "sha256": "..."},
 "runtime": {"id": "py3.14.9-1", "name": "NX2Saria-runtime-py3.14.9-1.zip", "size": n, "sha256": "..."},
 "toolchain": "<lock digest>"}
```

The feed (`app/<version>/feed.json`, or the player's override in the settings) is one of:

- `{"type": "github", "repo": "owner/name"}`: the newest GitHub release (or pre-release, on the preview channel) that
  has the manifest; a token reads a private repository.
- `{"type": "url", "url": "https://.../NX2Saria-update.json"}`: one manifest; its files sit beside it.

`DEFAULT` is this repository's releases; its earlier name on GitHub redirects to it.

### `Release`

`assets` maps a file name to its address. `headers` are extra request headers for those addresses (a private
repository's token).

### `Release.asset()`

`(url, headers, size, sha256)` of the manifest's `app` or `runtime` file. The name becomes a file in `downloads/`, so
only a bare file name is taken. Every file is checked against its size and sha256, so a manifest without them is
refused rather than downloaded unchecked.

### `retry_text()`

When GitHub takes requests again, for the message of a refused one: "after 14:32 (in 25 minutes)". It comes from
`X-RateLimit-Reset`, the time the hourly allowance is whole again, when the response says the allowance is used up
(`X-RateLimit-Remaining: 0`), else from `Retry-After`, the seconds to wait. It is empty when the response says neither
or says something that cannot be right (more than a day). A wait of zero or less means the allowance is whole again by
this PC's clock, and the text is "in a minute".

### `_github()`

- GitHub answers 403 or 429 when the allowance of requests is used up; both give the rate-limit message.
- The API lists the newest release first: the first one the channel takes that has a manifest is the answer.
- With a token, a private repository's files come through the API with the token (`Accept:
  application/octet-stream`).

### `latest()`

The newest release the feed offers on the channel, or `None` when it has none.

### `plain_notes()`

Release notes as short lines for the window: markdown bullets and headings without their marks.

## `launcher/game_session.py`

The game session tracks restarted copies, interprets exit codes and reads or annotates the game log.
The Tk module continues to expose the moved session names for existing callers. Exit code meanings,
normal end markers, log truncation and requested termination signals retain their existing rules.

Exit codes 70, 71 and 72 describe a fatal error, crash report and lost graphics device; Windows status codes
are shown with their known meanings. Requested Linux termination signals do not raise an alarm; SIGKILL is
excluded because the kernel also uses it when memory runs out. A size-capped game log cannot establish whether
the game wrote its own exit line. A launcher-added exit note ends the search for an earlier session.

### `exit_code_text()`

The exit code as the game's log and Windows show it: hex for a Windows status, with its meaning when known. Off
Windows a negative code is `subprocess`'s for a process a signal ended, shown with the signal's name.

### `log_has_own_exit()`

Whether the end of the current or legacy game log holds a line of the game's own end (`OWN_EXIT_LINES`). It is `None`
when the log cannot be read, or when it was cut at its size cap (`LOG_CUT_LINE`) and so cannot tell. A line this
launcher added for an earlier run's end (`note_exit_in_log()`) ends the search.

### `note_exit_in_log()`

Adds the exit code to the end of the game's log, so a log a player sends shows how the run ended even when the process
died before it could write a line itself.

### `log_is_new()`

Whether the selected game log was written at or after `started` (`time.time()` before the game was started). A run
that ends before it moves and opens its log (`runtime/main.c` `log_open()`) leaves the log of a run before it in
place. FAT and exFAT drives keep a file's time to 2 s, hence the slack.

### `shown_path()`

A path inside the game folder as the player's system writes it, for the window's sentences: `logs\nx2saria.log` on
Windows, `logs/nx2saria.log` elsewhere.

### `game_end()`

What the launcher says about an end of the game (`launcher/ui/app.py` `App.watch_game()`, `play_only()`): a `GameEnd`
of the line for `launcher.log`, the sentence for the player (empty for none), the text `note_exit_in_log()` adds to
the game's log (empty for none), and `alarm`: whether the end is worth a message box where no window shows the
sentence. An alarm is a FATAL line, a code of `FATAL_CODES` or a Windows status; not 0, nor 1, which Task Manager's End
task gives.

`code` is the exit code of the run's last process, or `None` when it is not known, which happens only for a copy the
game's own Restart started that could not be followed (`GameRun`). `started` is `time.time()` before the game was
started, `restarted` whether it restarted itself on the way.

### `GameRun`

A Play of the game, followed through the copies its own Restart starts. The Restart (`runtime/gfx/gfx_present.c`
`gfx_window_closed()`) starts the executable again (`runtime/platform/win32.c` `plat_restart()`) and ends the process
before it with code 0, so the code the game really ended with is its last copy's: once the process followed has ended
with 0 while the executable still runs, the copy is looked for (`launcher/winsys.py` `game_copy()`) and followed from
then on. On Linux the Restart (`runtime/platform/linux.c` `plat_restart()`) spawns the copy as a child of the old
process; `launcher/winsys.py` `start_game()` makes the launcher a child subreaper, so the copy becomes the launcher's
child once the old process has ended and its code can be waited for.

`proc` is the process followed, `None` while a copy could not be found (it ended before the next look, or the system
did not show it): it is looked for again each second while the game still runs, and the end's code is not known if
none is found. `started` is `time.time()` before the start (for `log_is_new()`).

- `GameRun.alive()`: whether the process followed runs; nothing else is looked at. A `GameCopy` of
  `launcher/winsys.py` whose exit code could not be read has ended all the same.
- `GameRun.running()`: whether the game still runs, this launcher's process or another of the same executable
  (`launcher/build.py` `game_running()`); asked once a second until it says no.
- `GameRun.finished()`: the run is over. A copy the Restart started that ended before `GameRun.running()` looked for
  it is the run's last process, and its code is the end's (`launcher/winsys.py` `game_finished()`).
- `GameRun.end()`: `game_end()` for this run, once `GameRun.running()` said no.

### `finish_game()`

Once `GameRun.running()` has confirmed that the followed game and any Restart copies have ended, `finish_game()`
asks the run for its `GameEnd`, writes its end line to the launcher log, synthesizes a crash summary if session reports
are enabled, then appends the exit note to the game log if one is due. A hard crash may not leave a session summary;
the summary must be made from the fatal line before the launcher note marks the end of that log's run. The function
returns the `GameEnd`, including its optional message, without a Tk dependency.

`App.watch_game()` still schedules the next check while the run is active. After it ends, the adapter restores, raises
and focuses the window before calling `finish_game()`, then shows any returned message, checks a waiting staged update,
handles after-game reports and finally shows home. A staged update or report action that takes over still short-circuits
the later steps. The consent and report policy are unchanged; the extraction only moves the end processing.

### `end_kind()` and `end_asked()`

`end_kind()` names an end that has a sentence for the player: `fatal`, `crash` and `device_lost` for the codes 70, 71
and 72, `fatal` for a fatal line found after the game restarted itself, and `code <text>` for every other code, with
the text `exit_code_text()` gives. An end without a sentence has no kind, and nothing is asked about it: the window's
close button, the game's own quit and Alt+F4 all end the game with code 0.

`end_asked()` is true for an end by SIGTERM, SIGINT or SIGHUP (`ASKED_SIGNALS`) when the log has no fatal line:
someone asked the game to close, and that is no crash. SIGKILL, SIGSEGV, SIGABRT and, on Windows, Task Manager's End
task (code 1) are not asked ends: a game that hung and was killed is worth its log. It must be called before the
launcher's own line goes into the game's log, which ends the search for the fatal line (`launcher/build.py`
`last_fatal()`); `finish_game()` and `play_only()` call both before `note_exit_in_log()`.

## `launcher/gameart.py`

The pictures of the game's Steam shortcut (`launcher/steam.py`), made from the player's own game: nothing ships.

| Picture | What it is |
|---|---|
| `logo` | the title screen's logo with the Master Sword and the Hylian Shield (`Logo_USEU_00`), cut to its outline |
| `hero` | the title screen's moonlit field, a frame of its first movie (`Event/Movie/Dm_Title_0005.webm`), 1920 x 620 |
| `portrait` | that frame with the logo over it, 600 x 900 (the library's cover) |
| `wide` | the same at 920 x 430 (the library's wide tile) |

The textures come from the interface archive `UI/Layout2Archive/Title...bntx.zs` as `runtime/ui/menu_assets.c` reads
them: a BNTX's BRTI records give format, size, block height and data; the level is block linear in the Switch 2 GOB
layout (`runtime/gfx/tex_deswizzle.c` `tex_bl_copy()`); BC4 masks become white with the mask as alpha; ASTC 4x4 goes
through the runtime's own decoder, `runtime/gfx/tex_astc.c`, built here into a library with the toolchain's clang (the
launcher builds the game with it, so it is on every PC that has a game to add).

The movie is WebM with AV1 video: the frames from the key frame before `TITLE_AT` up to it go into an IVF file for the
toolchain's `dav1d.exe`, whose last picture (8-bit 4:2:0, limited range BT.709) becomes the background. Every picture
is written as PNG with `launcher/ui/art.py` `png_bytes()`; nothing here needs an imaging package.

- `TITLE_AT` (12.0) is seconds into the movie: the field under the full moon, as the title screen shows it first.
- `RUNTIME_DIRS` are the runtime's folders by subsystem, the same list as `RUNTIME_DIRS` of `tools/build_game.py`
  (`tests/runtime_dirs_check.py` compares the two): the headers of `tex_astc.c` include others by name from them.
- `_MASTERS` are the EBML ids of the WebM elements that hold others: Segment, Cluster, BlockGroup, Tracks, TrackEntry
  and Video.

### `bntx_textures()`

Each texture of a BNTX file: name, format, size, block height, data offset and size.

### `deswizzle()`

The first level as tightly packed rows of `row` bytes (`runtime/gfx/tex_deswizzle.c` `tex_bl_copy()`).

### `bc4()`

White with the mask as alpha (`runtime/ui/menu_assets.c` `bc4_decode()`).

### `astc_library()`

`runtime/gfx/tex_astc.c` of this release as a library in `data/cache`, built once per source with the toolchain.

### `texture()`

The named texture as an HxWx4 `uint8` array (BC4 or ASTC 4x4), or `None` when the file has no such texture.

### `webm_frames()`

`(time in ms, key frame, data)` of every block of a single-track WebM, and the video's width and height. An element of
unknown size runs to the end of its parent. The blocks are SimpleBlock (`0xA3`, whose flags say key frame) and Block
(`0xA1`).

### `movie_frame()`

The picture of the movie's data at `at_s` seconds, decoded by dav1d, or by ffmpeg on Linux. The conversion to RGB uses
the BT.709 coefficients on limited-range values.

### `resize()`

`img` (HxWxC, `uint8` or float) at `w` x `h`: whole-pixel box averaging first when it shrinks by 2 or more, then
bilinear. Returns `float32`.

### `premultiplied_resize()`

A resize with the colours weighted by alpha, so transparent texels add no dark fringe; `uint8` straight alpha out.

### `trim()`, `place()`, `crop()` and `with_logo()`

- `trim()`: the picture cut to the box of its visible texels, with `pad` texels around.
- `place()`: `src` (`uint8` RGBA, straight alpha) over `dst` (`uint8` RGB) at `x`, `y`.
- `crop()`: the largest `w`:`h` part of the frame centred near (`cx`, `cy`), given as fractions, scaled to `w` x `h`
  (`uint8` RGB).
- `with_logo()`: the background with the logo scaled to `width_share` of its width, centred at height `cy` (a
  fraction).

### `make()`

`{name: HxWx4 uint8}` for `logo`, `hero`, `portrait` and `wide`, from the player's game; `ArtError` when the game's
files or the tools to read them are not there. The moon sits right of the middle and high: every cut keeps it, and the
hero keeps the horizon too.

## `launcher/gamefiles.py`

The player's game files, as far as the launcher reads them itself: finding a `.nup` dump, the game's Cetra Display
font for the window (`runtime/ui/menu_assets.c` reads the same font for the in-game menu's titles) and the game's
interface sounds for the window's sound effects (`runtime/ui/menu_sound.c` plays the same files in the in-game menu).

They come from the built game's `romfs/` (`romfs.bin` and `index.json`, `tools/make_game.py`) or, before the first
build, straight from the `.nup`. They are decoded once into `data/cache/fonts` and `data/cache/sounds`, which stay on
the player's PC like the rest of what is built from their game. Nothing of the game ships with the launcher.

- `.zs` files are zstd frames, most with one of the dictionaries of `Pack/ZsDic.pack.zs` (a SARC, compressed without a
  dictionary; a frame names its dictionary by id).
- A `.bfotf` font is an OpenType file behind an 8-byte header, every big-endian 32-bit word XORed with one key: the
  first word is `0x7f9a0218` under it, the second the payload size.
- A `.bwav` sound: "BWAV", a byte order mark, a version, a CRC32, a prefetch flag and the channel count (u16 at
  `0x0e`), then `0x4c` bytes per channel from `0x10`: the codec (u16: 0 PCM16, 1 DSP-ADPCM), the pan (u16: 0 left,
  1 right, 2 middle), the sample rate and the sample count (u32 each), 16 DSP coefficients (s16 from `+0x10`) and the
  offset of the samples in the file (u32 at `+0x30`). DSP-ADPCM: 8-byte frames of a header byte (coefficient pair,
  scale exponent) and 14 four-bit samples.

`FONTS` maps a role to its RomFS path; the window sets all its text in Cetra Display. `SOUNDS` maps the window's sound
effect (`launcher/ui/sound.py`) to the game's interface sound.

The other drives' search (`find_dumps()` with `drives`) goes two folder levels below each drive's root
(`D:\Games\NX2Saria`, a data disk's `Games/Switch`), `DRIVES_DEPTH`, through at most `FOLDERS_PER_LEVEL` folders of
each, for at most `DRIVES_SECONDS` (5.0) in all. On a 1 TB NTFS partition two levels were 502 folders, listed in
0.3 s; three levels were 3373 folders and 1.2 s.

### `candidate_dirs()`

The folders the dump is looked for in: `extra` first, then Downloads, Desktop and Documents (the desktop's own,
translated or moved, and the English names), then the home folder.

### `is_game_nup()`

True when the file is a `.nup` holding the game (title `0400486024DEC000` or its update).

### `find_dumps()`

`.nup` files with the game in the usual places (the setup's folder first), then in the folders `drives` names (the
mounted disks, `launcher/platform/linux.py` `mounted_drives()`) `DRIVES_DEPTH` levels deep, newest first. `report`, a
dict, gets `folders` (how many were looked in) and `stopped` (true when the drives' search ran out of time).

Its inner `look()` puts the game's `.nup` files of one folder into the result and returns that folder's first
`FOLDERS_PER_LEVEL` folders, from the same listing. A folder with tens of thousands of entries is read one entry at a
time, so the deadline holds inside it.

### `RomfsImage`, `NupRomfs` and `open_romfs()`

`RomfsImage` reads the built game's `romfs/romfs.bin` through its `index.json`; `NupRomfs` the RomFS of the game's
update (or base) package inside a `.nup`. `open_romfs()` gives a reader for the built game, else the dump, else
`None`.

### `_zstd()`

Python 3.14 has `compression.zstd`; before it the zstandard package is used.

### `_dict_id()`

The id a zstd dictionary carries (magic `0xEC30A437`, then the id).

### `sarc_members()`

`{name: bytes}` of a SARC archive.

### `bfotf_to_otf()`

The OpenType (or TrueType) bytes inside a `.bfotf` or `.bfttf`, or `None`.

### `font_family()`

The family name in an OpenType `name` table (name id 16, else 1), or `None`.

### `cached_fonts()` and `extract_fonts()`

`cached_fonts()` gives `{role: (path, family)}` of the fonts decoded earlier. `extract_fonts()` decodes the game's
Cetra Display into `data/cache/fonts` and gives the same for the fonts found. A font file that already has the same
bytes is not written again: a file this process registered as a font stays as it is.

### `bwav_to_pcm()`

A `.bwav` as `(sample rate, frames)`, the frames a list of `(left, right)` 16-bit samples, or `None` when it is no
BWAV of one or two channels this reads. A middle-panned channel goes to both sides at 0.707 when it is the only one.

### `pcm_to_wav()`

A 16-bit stereo WAV of the frames, a list of `(left, right)` samples.

### `cached_sounds()` and `extract_sounds()`

`cached_sounds()` gives `{effect: path}` of the game's interface sounds decoded earlier. `extract_sounds()` decodes
the game's select and back sounds into `data/cache/sounds` as WAV files and gives the same for the ones found.

## `launcher/gameini.py`

The game's own settings, `nx2saria.ini` (or the legacy `nx2navi.ini`) in the game folder.

The file is read the way `runtime/cfg/cfg.c` reads it: a UTF-8 byte order mark is skipped, lines starting with `#`,
`;` or `[` are not settings, a key is matched without regard to case and with or without a prefix (`NX2SARIA_`, or the
older `NX2NAVI_` one), a value in double quotes loses them, an empty value leaves the setting as it was (its default,
or an earlier line's value), and the last line with a value wins. For a key whose row in `runtime/cfg/cfg.c` has a set
of values (`choices=True`: numbers, ranges, words), a blank followed by `;` or `#` starts a comment after a value that
is not in double quotes, as `runtime/cfg/cfg.c` `cut_comment()` reads it; free text (a path, a name, the list of mods)
keeps its whole value. Sections do not matter to the game; a key the file does not have yet goes under its section, as
the game writes it.

A change rewrites only the line it touches, through a temporary file: the other lines keep their bytes (the byte order
mark, line ends, text that is not UTF-8). A file that exists but cannot be read is never rewritten, as
`runtime/cfg/cfg.c` `cfg_save_to()` does not: rebuilding it from nothing would lose its lines.

### `_path()`

`nx2saria.ini` in the game folder. An `nx2navi.ini` there without one (the file's name in earlier releases) is renamed
to it the first time the launcher looks, as the game does at its start (`runtime/cfg/cfg.c` `find_ini()`), so the
folder keeps one settings file with every setting in it; where the rename fails, the old file is read and written
under its name.

- Something else by the legacy name that is not a file is left as it is, and read (and refused) as before.
- The rename must not replace an existing file. On Windows `rename` refuses to; elsewhere `rename` would replace it,
  so the file is hard-linked under the new name, which refuses an existing file too, and the old name is removed.

### `_text()`

The file's text with its own line ends (bytes that are not UTF-8 kept as surrogates), empty when the file does not
exist. Any other error is raised.

### `_setting()`

`(key, value)` of a settings line, or `None`. With `choices` the key has a set of values, so its value ends at a
comment (the first blank followed by `;` or `#` after the `=`, with `key = ; note` giving an empty value), unless it is
in double quotes.

### `get()`

The value the file gives the key (empty when it gives none: the game's default applies); `choices` as in `_setting()`.
Raises `OSError` when the file exists but cannot be read.

### `put()`

Writes `key = value`: on the key's last line that has a value as the game reads it (`choices` as in `_setting()`; else
its last line) when the file has one, else under `[section]` (made at the end when the file has none). The line is
written again whole, without a comment it had, as `runtime/cfg/cfg.c` `cfg_save_to()` writes it. Raises `OSError`,
with the file unchanged, when it exists but cannot be read or the new one cannot be put in place. When the section's
header is the file's last line and has no line end, it gets one before the new line goes under it.

## `launcher/mods_job.py`

`drop_build_cache()` removes the lifted sources and objects when the player does not keep the build cache, while
preserving build state needed for update. Its body is the former Tk method; `App` continues to expose it for other
jobs. `finish_mods_job()` applies this policy after a successful mod rebuild and returns `resume` with cleared failed
mods. A cancelled or failed rebuild retains the waiting mod names and returns its result for the caller to choose the
screen or retry. Tk preserves its existing result timing. A worker calls with `require_finished=True`, which returns
`pending` without cleanup until the job has completed marker, claim and lock cleanup.

## `launcher/play.py`

`prepare()` decides in the old window's order: whether the executable exists, whether this launcher already follows
an active game, whether the game executable is already running outside this launcher, mod preparation and any needed
rebuild, then the Windows memory check. The external game can have come from another launcher, a shortcut or a test run;
its session is not owned by this window. Refused starts and decisions keep their existing `play:` and `mods:` log lines.

The returned `PlayPreparation.status` tells the caller what to do:

- `not_built`: show that the game has not been built and render the screen.
- `own_game`: return without changing the screen; this launcher is already watching its own game.
- `external_game`: show that the game is already running and render the screen.
- `rebuild`: start `ModsRebuildJob` with `waiting`, show its message and return. A mod's code cheats or ExeFS patches
  must be lifted into the game before launch. `built=True` means that rebuild just ended: skip mod preparation and do
  not ask to rebuild again. If the same mod list already failed to build, start without its code instead; otherwise a
  build that cannot finish would prevent the player from playing at all.
- `memory`: ask the player using the structured `memory` request, then pass the answer immediately to `start()`.
- `ready`: call `start()` without a memory question.

During mod preparation, `mod_message` is the last line returned by `mods.prepare()` and `home_message` is the last
line saying a mod was not unpacked. The window copies each non-`None` value to its matching message so that the latter
is still shown when the launcher returns home. Preparation and waiting-list read errors retain their existing log and
continue without blocking Play.

`start()` returns `(GameRun, None)` after a successful spawn, `(None, error)` when the spawn raises `OSError`, or
`(None, None)` when the player declines the memory warning. It logs the existing start, refusal or failure line. The
Tk adapter owns the prompt, starting the existing rebuild job, select sound, window visibility and watch timer.

## `launcher/telemetry.py`

### `synthesize_crash()` and `session_header()`

A hard crash writes no summary, so the launcher makes one from the FATAL line, the log's `version:` line and the
header the game wrote during that run (`logs/session-start.json`, `runtime/session_summary.c`
`session_summary_header()`). The header is that run's only when the current `logs/nx2saria.log` holds the line of its
writing (`HEADER_LINE`): the game starts a new log at each start and removes the old header before it writes its own,
so after a restart from the settings menu the header used is the one of the copy that crashed, and a header from the
copy before it is never taken. The whole log is read in pieces for the line: it may be 64 MiB, and the line of a later
Apply can be far from the start.

- From the header, only `HEADER_KEYS` (os, cpu, ram_mb, gpu, display, settings, startup, patches, mods), each value
  only where it has the type the format gives its field (`example_summary()`); anything else stays null, and keys
  the format does not have are dropped. The session's times and frame rates stay null whatever the header says.
- No header: a missing file, an empty one, one that is not JSON with the summary's `schema`, or one whose writing the
  log does not name. The summary is then the one of before, with only the versions, `os.family`, `session.end` and
  `fatal`.
- The line: `telemetry: the last run ended in a crash; a summary was made from its FATAL line, with the hardware and
  settings of logs/session-start.json (<n> of <m> fields null)`, or `..., without hardware or settings: <why> (<n> of
  <m> fields null)`, where `<why>` says which of the four it was.

## `launcher/ui/app.py`

### `App.home_reports()`

What a run of the launcher does about session reports, once, the first time the home screen of an installed game is up:
it calls `App.start_reports()` and sets `reports_started`. It does nothing with `--selftest`, on any other screen, or
while the game folder holds no game (`launcher/build.py` `installed()`), and those calls leave `reports_started` unset
so that a later one still counts.

Called from `App.start()` on a normal start, and from `App.job_finished()` and `App.leave_job()` after they show the
screen a job returns to, with the job's name (`launcher/jobs.py` `Job.doing()`) as `after`. So the three starts that
reach the home screen through a job ask there too: the start that finishes an update (`--continue-update`), the first
installation, and an update that failed and was left with Back. Before this the question was asked only by a normal
start: the start that finished an update never asked, the session played from it was not reported, and the question
came one start later. A failed job stays on the job's screen, which is not `home`, so nothing is asked over its error.
The starts that leave `App.start()` early (a staged update to install, Play from Steam) still do not ask.

### `App.start_reports()`

With `after` set (the home screen came from a job's end) the question opens without its countdown
(`App.open_question()` with `countdown=False`): a build takes minutes and the player may not be at the PC, and a
countdown that ran out then would turn reports on for someone who never saw the question. The question waits instead;
its first line is the one of a stopped countdown. The log says `telemetry: the question opens after <job>, without its
countdown`, and `App.start()` says `telemetry: the question waits for the update to finish` when it starts the update's
job with a question due.

The notice of reports turned on (`launcher/telemetry.py` `Reporter.take_notice()`) is taken only when no message is on
screen. After a job the home screen always carries the job's message ("NX2Saria is now ..."), and taking the notice
there would clear it and log it as told without showing it; it stays for the next start with a free message line.

### `App.offer_guarded()`

For `GUARD_SECONDS` (1 s) after the question opens, `App.activate()` and `App.back()` do nothing on it: the window
comes to the front the moment the game dies, and a button still held for the game would otherwise choose *Send the
Log*, which is the focused row. Moving the cursor is not guarded, and neither is closing the window, which sends
nothing. The preview (`--screen crash`, the selftest) has no guard.

### `App.offer_crash_log()`

Opens the question after an abnormal end ([telemetry.md](../telemetry.md#when-the-game-ends-with-an-error)) when one
waits in the game folder (`launcher/crash_offer.py` `pending()`), and returns whether it opened. It opens only on the
home screen, never while the question about session reports is open, while this launcher follows a game or another
copy of the game runs (`launcher/saves.py` `game_running()`); in those cases the file stays and a later call asks.
Three callers, which are the only places the question can appear:

- `App.watch_game()`, after the game's end was told and the home screen is shown. A staged update that installs at
  that moment returns before it, so the question waits for the launcher that starts after the update.
- `App.start_reports()`, on the first home screen of a run, when no question about session reports is due. This is
  the start that asks about an end the window did not watch (`play_only()`).
- `App.question_end()`, after the question about session reports was answered, so that one comes first.

The request is prepared when the question opens (`launcher/crash_offer.py` `prepare()`), and the same bytes are what
*See What Is Sent* shows (`App.open_offer_sent()`) and what *Send the Log* posts. When it cannot be prepared (an empty
log, a server that answered 410 to this version) nothing is shown, and the log line says why.

`launcher.log` gets `telemetry: crash log offer: shown (end: <kind>; <start> + <end> lines, <bytes> bytes of log)`.

### `App.offer_end()`

The question's end: `send`, `don't send`, `closed` (Esc, B, the window's close) or `comment` (*Write a Comment*). It
removes the waiting file first, so no end of the game is asked about twice. Everything but `send` is counted as
declined and logged as `telemetry: crash log offer: declined (<how>; end: <kind>; <counts>)`. `send` posts on a thread
(`launcher/telemetry.py` `Tasks`) and shows the result as the home screen's message.

`App.on_close()` calls it with `leave=True` when the player closes the window over the question. A signal that ends
the launcher is no answer, as for the question about session reports: the file stays and the next start asks. When
any question was shown in the run, `App.on_close()` logs `telemetry: crash log offers in this run of the launcher:
N shown, N sent, N declined, N failed`; each outcome line carries the same counts, so a window that was killed still
left them. The counts exist to tell, from players' logs, whether a PC that crashes every minute makes the question a
nuisance: it is asked at every such end.

### `App.crash_offer_desc()`

The sentence of the game's end is shown without its last sentence (`launcher/game_session.py` `LOG_WHERE`, where the
log is): with the longest fatal line the launcher shows (`FATAL_SHOWN`, 240 characters) the text would not fit the
panel with it, and the question is about that very log. The home screen keeps the whole sentence.
