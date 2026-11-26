# launcher

The launcher: the window, the installation of the build tools and of the game, the updates, the mods, the saves and
the session reports. What it does for a player is in [launcher.md](../launcher.md), [install.md](../install.md) and
[telemetry.md](../telemetry.md); this page holds the notes of its code, one section per file in the order of the file
names. `launcher/build.py` still keeps its comments until its notes are here. `launcher/platform/udmabuf_fix.sh` and
`launcher/native/app.manifest` are not read by the comment check and keep their comments. A `# noqa` line stays in the
code with whatever follows it on the line.

## `launcher/__init__.py`

The package of the launcher: it installs the build tools, builds the game from the player's own dump, starts it and
keeps the port up to date ([launcher.md](../launcher.md)).

It uses the standard library only, plus numpy for drawing the window (`launcher/ui/art.py`), zstandard where Python
lacks `compression.zstd`, and truststore for Windows' own certificate check (`launcher/net.py`). tkinter draws the
window; everything that does work runs without it (`launcher/cli.py`).

## `launcher/__main__.py`

`python -m launcher [--selftest [--shots DIR]]`: the launcher window (`launcher/ui/app.py`).

## `launcher/build.py`

Running `tools/make_game.py` for the launcher: the command line, its "@@" progress lines (`make_game.py --events`) and
cancelling it with every compiler it started. Also what the launcher reads of a game folder: whether it is built,
whether the game runs from it, and the game's log.

### `command()`

Runs `make_game.py` with `pydeps/` (zbic) and the script's folder first on `sys.path`; the embeddable Python's `._pth`
file leaves both off.

The build runs below normal priority (`BELOW_NORMAL_PRIORITY_CLASS` in `Run`): a compile on every processor for ten
minutes and more otherwise takes the PC from whatever the player does meanwhile, which the job screen invites ("Keep
using your PC if you like"). On an idle PC it costs nothing. Windows hands a below-normal process's class on to the
processes it starts, so the compilers, the lifter's workers and the shader preparation that `make_game.py` starts run
that way too.

### `has_prompt_art()`

True when this version holds button art (`app/<version>/prompt-art`, `tools/prompt_art.py` `FOLDER`): `make_game.py`
then draws the button prompts in a step of its own, and the jobs show a row for it.

### `rebuild_args()`

`retune` is Rebuild the Game, which compiles the game's code for this processor's level. An update keeps the level its
objects were compiled for, so that it compiles only what changed (`tools/make_game.py` `lifted_level()`).

### `Run`

One `make_game.py` process. `on_event(dict)` is called for each "@@" line and `on_line(str)` for every other line;
`Run.wait()` returns the exit code; `Run.cancel()` ends the process and its children. `dropped` counts the lines and
events whose handler raised.

`Run._read()` reads the build's output to its end, whatever the handlers do. A handler that raises (the launcher's log
on a full disk, a drawing error) loses its line, counted in `dropped`, and the reading goes on: when this thread ended
on the exception nobody read the pipe any more, the build stopped at its next line once the pipe was full, and the job
waited for it for good. A line that starts with "@@" and is not an event (not JSON, or JSON that is no object) goes to
`on_line`.

### `_write_open_error()`

0 when the path opens for writing (it is closed again at once), else Windows' error code for the refusal. The open
asks for `GENERIC_WRITE`, grants every share mode (`FILE_SHARE_READ`, `FILE_SHARE_WRITE`, `FILE_SHARE_DELETE`), and
uses `OPEN_EXISTING` and `FILE_ATTRIBUTE_NORMAL`; with every share mode granted a sharing violation comes only from a
running image.

### `game_running()`

True when the built game runs from the game folder: Windows refuses to open a running exe for writing, with a sharing
violation. A read-only exe (a copy restored from a backup or read-only media) is refused too, with access denied,
though nothing runs it: its read-only attribute is cleared and the open tried again, as the next build replaces the
file anyway. Access denied for another reason (an access list that denies the player writes, Controlled Folder Access)
is no running game either: taken for one, it kept Play saying "already running" and a rebuild failing with "the game
is running" for an exe that nothing ran. `log`, when given, gets a line for that case, which a build's failure to
replace the exe (`tools/make_game.py`) then has beside it. An exe that opened, or that is gone meanwhile, is not
running.

### `build_state()`, `built_version()`, `scan_report()`

`build_state()`: the game folder's `build-cache/state.json` (`make_game.py`), or `{}`. `built_version()`: the kit
version that last built the game folder ('' when none finished). `scan_report()`: what the check of the game files
found (`tools/romfs_check.py` writes `<game>/romfs-check.json`): the paths of the damaged files and how many of them
got a stand-in, or None when the folder has no readable report.

### `unpacked()`, `installed()`

`unpacked()`: true when the game files a rebuild reads are in the game folder (`tools/make_game.py` `BuiltSource`
names the same ones): the unpacked ExeFS, the RomFS image and its index.

`installed()`: true when the game folder holds a game the player can start: a finished build, or the exe of the last
finished build beside a build that stopped later. `make_game.py` drops a step's record when a step before it runs
again, so an update that lifted again and then stopped in the compile (an error, Cancel, a power cut) leaves
`state.json` without `compile` and `built_version()` says '': the old exe is still there and playable, and a rebuild
continues from the unpacked game files without the dump (`can_rebuild()`).

### `game_log_paths()`, `game_log_path()`, `last_fatal()`

The game's log as the window (`launcher/ui/app.py`) and the session reports (`launcher/telemetry.py`) read it.
`game_log_paths()` gives the current and legacy paths so the worker can record both at the start of Play.
`game_log_path()` gives the new log path, or an existing log under the older name until the new path is present.
`FATAL_TAIL` (256 KiB) is how many bytes of the game's log `last_fatal()` reads from its end.

`last_fatal()`: the last FATAL line of the game's latest run, without its "FATAL: ", or ''. The game writes one before
it exits with 70 (`runtime/dispatch.c` `rt_fatal()`), 71 (`runtime/main.c` `crash_handler()`) or 72
(`runtime/vk/vk_device.c` `vk_device_lost_check()`) into `logs/nx2saria.log` (older installations may still have
`logs/nx2navi.log`). A line this launcher added for an earlier run's end (`launcher/game_session.py`
`note_exit_in_log()`) ends the search: what stands before it is not this run's.

### `fatal_whole()`

In a log of a game from before each line of the crash report was one write (`runtime/main.c` `crash_handler()`),
another thread's line can sit inside the FATAL line, with the rest of it on the next line:
`FATAL: access violation (0xb) at host 0x7f00 (libc.so.6+0x98e31)[host] input: ...` and then
`, reading address 0x3e80010beb5`. `last_fatal()` hands the line and the next one to `fatal_whole()`. The line is cut
where a second log prefix starts (`FATAL_INTRUDER`: the game's `[host] ` or the launcher's `[launcher] `); a `[word] `
of the line's own text, as in a mod's folder name, is no prefix. The next line is joined back only when it is the tail
the crash report writes after what was cut: after `(`, the module and `)` with an optional address
(`FATAL_MODULE_TAIL`); after `)`, the address alone (`FATAL_ADDRESS_TAIL`). `%#llx` writes zero as `0`, not `0x0`
(`FATAL_HEX`), and a Windows module's name can hold spaces. Any other next line is left out, and only the cut stays. A
line whose prefix comes at its very start has no head to keep, and stays as it is, so the crash is still counted. The
session report (`launcher/telemetry.py` `synthesize_crash()`) and the window's message read the line through
`last_fatal()`, so the server groups such a crash with its whole twins.

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

**The script's own folder leaves `sys.path`.** Run as a file without `-I`, Python puts the script's folder,
`launcher/`, first on `sys.path`, and `launcher/platform` then stands in for the standard `platform` module for every
`import platform`: ours and the standard library's own. In 0.27.0-rc.2 and rc.3 that ended `cli.py` in
`launcher/platform/linux.py` `write_watch_status()` (`AttributeError: module 'platform' has no attribute 'release'`),
and with it the `continue-update` that every installed launcher's `update` starts without `-I`: the version changed and
the game was not rebuilt. So `cli.py` takes its folder out of `sys.path` before anything else is imported, as
`launcher/start.pyw` does. `update` still starts the new version without `-I`, as every installed launcher does:
that is the call the test below guards. The rule for the package: no module of `launcher/` imports a standard module by a
name that `launcher/` also has (today only `platform`; `launcher/platform/linux.py` reads the kernel's release and
machine from `os.uname()`), and every entry point run as a file takes its own folder out of the path.
`tests/launcher_script_path_test.py` runs `cli.py` as a script without `-I` with the test's Python and the system's,
checks that its folder is gone from `sys.path`, and fails on an import of such a name anywhere in `launcher/`.

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

## `launcher/context.py`

What every part of the launcher shares: the folder layout, the settings and the log (`data/logs/launcher.log`, the run
before it in `launcher.1.log`; [launcher.md](../launcher.md#the-launchers-log)). The import of `private_paths` from
`tools/` comes after the path is set, hence its `# noqa: E402`, which stays.

### `install_kind()`

How this launcher was installed, for the start line: a checkout of the repository; on Windows the setup's install
(`NX2Saria.exe` beside `app/`) or an unpacked release; on Linux the AppImage's install (the `nx2saria-launcher` script
beside `app/`, which the AppImage writes and the menu entry starts), and whether the AppImage started this run (it
passes `APPIMAGE` on) or the menu entry did.
When the worker creates the context, the header identifies Electron as the caller and keeps the installation kind,
for example `AppImage install, started by Electron`.

### `system_text()`

The system for the start line: the Windows version, or the Linux distribution and the graphical session.

### `Context`

The settings get the context's log, so that each changed setting gets a line when it is saved
(`launcher/settings.py` `Settings.save()`). On Linux it reads udmabuf's state and the write watch's
(`write_watch`, `launcher/platform/linux.py` `write_watch_status()`) once, at start, and logs both.

### `Context.log()`

One line (or several) into `launcher.log`. A log that cannot be written (a full disk, a file closed under it, which
raises `ValueError`) loses the line and nothing else: the callers log from the build's reader thread and from the
window's timers, where an exception would stop the build's output from being read or the timer from running. The
account's folders are written short (`tools/private_paths.py`): players attach this log to public issues.

The packaged worker loads the path helper from the active installation. If that version has no
`shorten_always()`, the context uses its `Shortener` directly. Report text and crash summaries are shortened
regardless of the setting that controls paths in local logs.

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

## `launcher/inspect_installation.py`

`inspect_installation()` reports readiness, the configured game directory, the effective hide preference,
the presence of a job marker and any confirmed running game. It reads settings directly and never opens
`Context`, takes the lock or writes a recovery file. Missing or unreadable settings and invalid build state
have separate result codes. `main()` exposes the same inspection to callers that use the standalone command.

On Linux, the process check compares the configured executable with `/proc/<pid>/exe`. With an owner PID,
`_descends_from()` walks parent IDs from `/proc/<pid>/stat` and stops if the chain ends or repeats. On Windows
the inspection uses `launcher/platform/windows.py` `game_running()`. A process that cannot be confirmed
leaves the state unknown.

## `launcher/jobs.py`

The launcher's long tasks, run on a worker thread: install, update, rebuild, the update check and the background
staging of an update. Each job is a list of steps the window shows as rows; the window reads the job's state on a
timer (`rev` changes on every change) and never blocks on it. The same jobs run without the window from
`launcher/cli.py`. `BackgroundUpdate` decides when a staged update installs.

- `RECHECK` (3600) is the seconds between update checks while the launcher stays open, hidden during play included.
- `PLAIN_FAILURES` are failures whose message says all there is to say: the log gets one line for them
  (`failure_line()`), not a traceback. A refused update check wrote 22 lines of two tracebacks into `launcher.log`
  each time.
- `MARKER` is `data/job-running.json`: the job that is writing into the game folder, from its start to its end
  (`Job._main()`). A job that ends, also by Cancel or an error, removes it and has told its result on screen. One the
  launcher was stopped in (closed by the system, a power cut, a crash) leaves it, and the next start says so
  (`interrupted()`).
- `SCAN_EXPECTED` are the game files `tools/romfs_check.py` reports as damaged on every copy of the game looked at so
  far: they are that way in the game's own data, not through a player's dump, so a new dump does not change them. The
  three textures get a stand-in; the game does without the fourth file.

### `interrupted()`

The job the launcher was stopped in, as it wrote it (`{'task', 'kind', 'game_dir', 'started'}`), or `None`. The marker
is removed: it is told once. It must be called while this process holds the launcher folder's lock and runs no job, or
the marker read is a running job's.

### `running_job()`

The job writing into the game folder as `MARKER` says it, or `None`. The marker stays. It is only worth asking while
another process holds the launcher folder's lock: a marker that a launcher stopped in its job left behind reads the
same (`interrupted()` tells that one and removes it).

### `failure_line()`

A failure in one line for the log: its kind, its message and what it came from.

### `scan_summary()`

`(the scan row's value, a sentence for the home screen)` for what the check of the game files found
(`launcher/build.py` `scan_report()`); two empty strings when nothing is damaged. The files of `SCAN_EXPECTED` are
told as what they are, the same ones as on every copy: the player learns what the check did and that nothing needs
doing. Other files are counted and pointed to, without advice the check cannot back.

### `prompt_rows()`

The prompts row, for a version with button art (`tools/make_game.py` draws the prompts after the shaders), or none.

### `Step`

`pace` holds `(time, fraction)` of recent progress, at most 400 entries, for `Job.eta()`.

### `Job`

- `claims` is what the job changes while it runs, for the lock between jobs (`launcher/locks.py` `claim()`): `game`
  is the game folder and the build tools, `app` the app folder's versions and the downloads. A job with claims also
  holds the launcher folder's lock between processes while it runs; one without (the update check) changes no file and
  runs beside anything.
- `task` is the job as the holder of a lock is named (`Job.doing()`).
- `result` is `None` while the job runs, then `ok`, `failed` or `cancelled`. `after` is what the window does next
  (`restart`, ...). `note` is a sentence for the home screen after the job (`Job.note_scan()`).
- `_lock` is held for every change of a step and of `rev`, and by a reader that wants the steps as one change left
  them (`Job.locked()`). The worker, the thread that reads the build's output and the window's thread (cancel) all
  change the job. It is reentrant: `progress()` begins a step, and every change ends in `Job._touch()`.

### `Job.eta()`

The step running now is estimated from its recent pace (its progress over the last minute, as a fraction per second),
and the steps still to come from the seconds per weight of the steps already done. A job-wide fraction does not work:
a step can take far more or less time than its weight says (Compile Changed Files is over half the weight of an update
and can take seconds), and the shader step then read "about 1 minute left" with 20 minutes to go. A step's own average
does not work either: Prepare the Shaders fills the first half of its bar with the translation and the second with the
much slower build into the driver's cache.

### `Job.locked()`

The lock to read the steps under (`with job.locked():`): they are then as one change left them, a step's state with
the value and fraction that belong to it. It is held only to copy what is read, never across a wait.

### `Job._touch()`

`rev` is counted up under the lock: `rev += 1` from two threads at once can write an older count back, and a reader
that had seen the newer one then takes the next change for one it has drawn already. The job's last change is its
result, so the window stayed on the job screen of a job that had ended.

### `Job.log()`

The line is written short first (`shorten()`): the job screen and Copy Report show these lines, with the account's
folders short. The write to the launcher's log happens outside the lock, because it waits for the disk.

### `Job.last_lines()`

The last `n` lines of the job's output, copied under the lock: the thread that reads the build's output appends while
the window draws them, and a deque changed while it is read raises.

### `Job.doing()`

The job in a few words ("the rebuild of the game"), for the one a lock refuses because of it.

### `Job._hold()`

Takes what the job needs to itself before it runs (`launcher/locks.py`): the launcher folder against other processes,
then its claims against the other jobs of this one. It raises `Busy`, with nothing held, when either is taken, and
returns what `Job._main()` lets go of when the job has ended.

### `Job._mark()`

Leaves `MARKER` for the time the job writes into the game folder; true when it was written.

### `Job._main()`

Every failure ends the job with a message; the log has the traceback (or one line, for `PLAIN_FAILURES`).

### `Job.note_scan()`

After a build: what the check of the game files found goes into its row, and when the check ran in this job, into the
note the home screen shows and into the log. The check itself only printed its counts into the build's lines, which
scroll away, so a player never learned that files were flagged or that stand-ins are in use. A check kept from an
earlier build shows its count in the row only.

### `Job.make_game()`

Runs `tools/make_game.py`; its steps drive the job's through `step_map` (its step to the job's step id, or `None`).

- A `progress` event with a `fraction` comes from a step that words its own progress (the shaders).
- A Cancel pressed before the build started (while the tools were checked) found no process to stop: the job stops
  just before it starts the process, and a Cancel pressed while the process starts stops it at once.

### `InstallJob.args()`

The arguments of `tools/make_game.py`. A game folder whose game files an earlier run already unpacked and checked
(`launcher/build.py` `can_rebuild()` and `unpacked()`) is built on from those, for this processor's level as a first
build is: the dump is not read again, so an install that stopped after the RomFS image goes on although the dump was
moved or deleted since, and the 26 GB image is not packed a second time.

### `InstallJob.run()`

The check for a path too long (`launcher/paths.py` `long_path_problem()`) is repeated here for the command line; the
window asks first (`launcher/ui/app.py` `App.begin_install()`).

### `RebuildJob`

`retune = True`: Rebuild the Game compiles the game's code for this processor's level (`launcher/build.py`
`rebuild_args()`).

### `ModsRebuildJob`

Play with a mod whose code cheats or ExeFS patches the game was not built with (`launcher/mods.py` `code_waiting()`):
the game is built again with them, compiling only what they change, and then starts (`launcher/ui/app.py`
`App.play()`).

### `UpdateDownloadJob`

Before the restart: download, check and stage the release, then switch `app/current.txt` to it. A checkout runs from
the repository and has nothing to switch to: there the job ends after the check, with `after` set to `staged`.

### `ContinueUpdateJob`

After the restart, in the new version: bring the tools to its lock and rebuild the game. `retune = False`: an update
keeps the compiled game's level, so it compiles only what changed. Its claims are `game` and `app`: the rebuild, then
`launcher/updater.py` `prune()` in the app folder.

### `StageJob`

A release downloaded, checked and unpacked beside the running version without switching to it (`launcher/updater.py`
`stage()`), while the launcher runs and the game too. Its thread runs at background priority, so the game shares only
the network with it. No screen shows it: the home screen's Updates row reads it.

### `BackgroundUpdate`

The update the launcher fetches by itself, and when it goes in ([launcher.md](../launcher.md#in-the-background)). The
window calls `BackgroundUpdate.poll()` on its timer, `BackgroundUpdate.checked()` after each update check, and
`BackgroundUpdate.install()` when the launcher starts, when the game exits, when a game that held an install up has
closed, and when the player selects Updates.

A newer release is staged once per update check (`StageJob`); a staging that fails is logged and tried again after the
next check. A staged release installs only while no copy of the game runs, no job holds the window and no staging is
under way, and by itself only with Install Updates Automatically on. Installing switches `app/current.txt` to it and
restarts the launcher (`restart(args)`, the stub), whose new version finishes in `ContinueUpdateJob`. A checkout has
nothing to switch to: there (`enabled` false) nothing is staged or installed.

| Field | What it holds |
|---|---|
| `job` | the `StageJob` running, or the last one |
| `tried` | the release staged, or found staged, since the last update check |
| `asked` | the player selected Updates during the download: it installs once staged |
| `waiting` | an install had to wait (the game ran, a download ran): the window tries again |
| `restarting` | the last install restarted the launcher |
| `error` | why the last install failed |

- `BackgroundUpdate.ready()`: the staged release an install would take now (the dict of `launcher/updater.py`
  `staged_info()`), or `None`. With `release` (the last check's answer), a staged release newer than it is passed
  over: the feed no longer offers it.
- `BackgroundUpdate.checked()`: an update check ended; the release it found is staged at the next poll, also one that
  failed before.
- `BackgroundUpdate.poll()`: reports a staging that ended (`staged` or `failed`), or starts staging the release when
  it is newer than the running version and not staged yet (`started`); `None` when nothing changed. `busy` means a job
  holds the window (an install, update or rebuild); staging waits for it, as an update's `prune()` clears the same
  folders.
- `BackgroundUpdate.install()`: installs the staged release now unless something stops it, and restarts into it.
  `asked`: the player selected Updates (now or during the download), which Install Updates Automatically off does not
  stop. `running`: the window knows the game runs (the process it started). `game_dir`: a copy started elsewhere counts
  too (`launcher/build.py` `game_running()`). It returns the version installed, or an empty string (then `waiting` says
  whether it is worth trying again). When the stub does not start, the new version runs at the launcher's next start.

## `launcher/locks.py`

Who may work in a launcher folder: one process at a time, and in that process one job at a time on the same thing
([launcher.md](../launcher.md#one-launcher-one-job)).

Two launchers on one folder each ran their own build into the same game folder, wrote the same settings and unpacked
an update over each other. So:

- `FileLock` is a lock between processes: a file locked by the operating system (`msvcrt.locking` on Windows, `flock`
  elsewhere), which lets go of it when the process ends, however it ends. The window takes the one of its folder
  (`instance_lock` of `launcher/paths.py` `Layout`) for as long as it is open, `launcher/cli.py` for a command, and
  every job that changes files while it runs (`launcher/jobs.py` `Job`). A process that holds it already shares it
  with its own jobs. Who holds it is written beside it (`<lock>.owner`), so that the one refused can say so.
- `claim()` is the lock between the jobs of one process: a job names what it changes (the `claims` of
  `launcher/jobs.py` `Job`: the game folder and its build tools, the app folder), and a second job on the same thing is
  refused with the first one's name.

Both refuse with `Busy`, whose text names the holder; neither waits (`FileLock` for a few seconds when asked to, for a
launcher that is closing for its restart).

`_files` maps a lock file's path to the open file and how many `FileLock`s of this process share it. `_claims` maps
(folder, what is claimed) to who claimed it.

### `_os_lock()`

Locks the open file for this process without waiting. `OSError` when another open file holds the lock.

### `owner()`

Who holds the lock file, as its holder wrote it ("the launcher window (process 1234, since 14:02)"), or "another
NX2Saria" when that cannot be read. It is only worth asking after the lock was refused: the note of a holder that has
ended stays.

### `FileLock`

The lock file held for this process until `FileLock.release()` or the process's end. `what` says who holds it, for the
one that is refused. It raises `Busy` ("<the holder> is using <where>"; `where` is the lock's folder unless given) when
another process still holds it after `wait` seconds. A lock file that cannot be made or opened (a folder that cannot
be written) locks nothing and refuses nobody: the launcher must still start there.

- `FileLock._take()`: true when this process holds the file now (or cannot lock at all), false when another process
  does.
- `FileLock.release()`: lets go of the lock. The file is closed when the last `FileLock` of this process on it lets
  go, and closing the file releases the operating system's lock. Calling it again does nothing.

### `claim()`

Claims each of `names` in the folder for `who` (a job's description), all or none. It raises `Busy` with the holder of
the first one that is taken, and returns what `release_claim()` takes.

## `launcher/mods.py`

The game's mods folder, for the launcher's Mods screen and Play ([launcher.md](../launcher.md#mods)).

The game reads `<game>/mods` (`mods_dir` in `nx2saria.ini` names another folder; an existing `nx2navi.ini` remains
supported): one folder per mod, each part of it where `tools/mod_files.py` says (`runtime/patches/mods.c`). The load
order and the switches are the game's own settings, `mods_order` and `mods_disabled` in `nx2saria.ini` (or its
existing `nx2navi.ini`), which this module writes as `runtime/cfg/cfg.c` does: the top mod wins a file two mods
replace, and a mod nobody switched off is on.

A mod as players download it often does not sit that way, so `prepare()` puts the mods folder in order before the game
starts, and `add()` does the same for an archive or a folder the player picks:

- An archive in the mods folder (`.zip`; `.7z` and `.rar` through the system's `tar`, which reads them on Windows 10
  and 11) is unpacked into a folder of its own name, unless that folder is there already. The archive stays: removing
  the folder and the archive removes the mod, and switching it off keeps both.
- A mod folder with nothing of a mod at its top and one folder inside, down to where the mod's parts are, gets that
  folder's contents (an archive unpacked into a folder of its own name, holding the mod's folder).
- A `romfs`, `romfslite`, `exefs` or `cheats` folder put straight into the mods folder becomes a mod of its own (the
  Atmosphere layouts, `atmosphere/`, `contents/` and the title's folder, already are one as they are).

Nothing is deleted: files only move inside the mods folder, and a mod that holds several folders with mod files (a
download with options to pick from) stays as it is, with a note.

Mods that change the game's code (code cheats, ExeFS patches) need the game built with them (the lift step of
`tools/make_game.py`). `code_waiting()` names the enabled ones whose files the last build did not take in
(`build-cache/mod-code.json`), and Play rebuilds the game first then. The build reads them from `<game>/mods` only.

The import of `mod_files` from `tools/` comes after the path is set, hence its `# noqa: E402`, which stays.

- `LOOSE_PARTS` are a mod's parts that are no mod when they sit straight in `mods/`.
- `MOD_TOP` is what a folder holds at its top when a mod starts there (the places of `runtime/patches/mods.c`
  `find_root()`).
- `STAND_INS` ("Damaged file stand-ins") is the launcher's own mod (`tools/romfs_check.py`), shown as such.
- `WINDOWS_DEVICES` are the names Windows keeps for devices, with or without an extension and in any case: a file
  called so is the device.
- `STAGING` (`.adding-mod`) is beside the mods folder: where `add()` puts a mod together before it moves in.
  `STAGING_STALE` (24 hours, in seconds) is the age after which a staging folder is one a stopped launcher left.

### `mods_dir()`

The mods folder the game reads: `mods_dir` in `nx2saria.ini` or an existing `nx2navi.ini`, else `mods`.

### `_quoted()`

A value as `runtime/cfg/cfg.c` `put_value()` writes it: quoted when the blanks at its ends or the quotes around it
would be lost.

### `order()`

The names in the game's order (`runtime/patches/mods.c` `by_order()`): the ones `wanted` names first, in its order,
then the rest by name (byte order of their UTF-8, as `strcmp`).

### `set_enabled()` and `move()`

`set_enabled()` switches a mod on or off for the game's next start (`mods_disabled`); `move()` moves a mod `d` places
up (negative) or down in the load order (`mods_order`). Both write the current settings file, or the existing legacy
one.

### `Mod`

| Field | What it holds |
|---|---|
| `files` | RomFS paths it replaces, lower case, `/` separated (with details) |
| `patches` | ExeFS patch files for this game's `main` |
| `other_version` | ExeFS patch files for another version |
| `cheat_file` | its cheat file for this game's `main` |
| `code_module` | `subsdk*` or `main` in its `exefs`: code the port cannot run |
| `other_game` | the title id of another game its Atmosphere folders name |
| `several` | folders that each hold a mod (a download with options) |
| `unsupported` | why the last build could not take an ExeFS patch file of it |
| `waiting` | code the last build did not take in |

`Mod.problem()` says why the game leaves this mod off whatever its switch says (`runtime/patches/mods.c` `scan_cb()`),
and is empty when it does not.

### `_same_build()`

Whether `hex_id` (trailing zeros may be left out) names `main`'s build id; true when `main`'s is unknown, as in
`runtime/patches/mods.c`.

### `_pchtxt_build()`

The build id a pchtxt file names (its last `@nsobid-` line before `@stop`), empty when none.

### `_status()`

`{key: (kind, note)}` of a build status file (`build-cache/exefs.status`, `tools/exefs_patches.py`).

### `_mod_folders_below()`

The folders under a folder (down to `depth` levels) that hold a mod at their top.

### `scan()`

The mods of the mods folder in the game's load order, with their switches; with `details`, what each holds.

### `overlaps()`

`{mod name: [(other mod name, files both replace, whether this one wins)]}` among the enabled mods with RomFS files:
the mod higher in the load order wins a file two replace.

### `built_code()`

`{"<mod>/<path>": sha256}` of the mods' code the last build took in (`MOD_RECORD` of `tools/make_game.py`).

### `code_waiting()`

The enabled mods whose code cheats or ExeFS patches the game was not built with (or with other ones). Only the game
folder's own mods folder is built from.

### `_unique()`

`folder / name`, or the first free "name 2", "name 3" and so on (for a file, "stem (2).txt" and so on).

### `_merge_into()`

Moves the entries of `src` into `dst` (made when missing) and removes `src`: a folder `dst` holds already is merged
into, and a file of a name `dst` holds already moves in under a free name (a readme beside the wrapped folder).

### `unwrap()`

A mod folder with nothing of a mod at its top and one folder inside (files beside it, a readme, do not count), down to
`UNWRAP_DEPTH` (4) levels until the mod starts: that folder's contents move up. True when it did.

The chain's top folder may share its name with a folder of the mod (`MyMod/romfs/romfs`), so it is moved aside first.
The mod's folders go up first, then whatever lay beside them on the way down (a readme), so nothing stays behind.

### `windows_name_problem()`

Why Windows cannot keep a file or folder under a name, empty when it can: a device's name (`CON`, `NUL.txt`), a dot or
a space at its end (Windows drops them, so the file gets another name than the one the game asks for), or a character
Windows does not allow (a colon makes a hidden stream of another file).

### `_safe_members()`

`ModError` when a file of an archive (its member names) would land outside the archive's own folder, or, on Windows
(`windows=None` asks the system), under another name than its own or not at all (`windows_name_problem()`): unpacked
all the same, such a file is missing where the game looks for it, with no word about it. The game for Linux reads such
names as they are, so there they pass.

### `unpack()`

Unpacks an archive into `dest` (a new folder); `ModError` when it cannot be. A zip's unpacked size is compared with the
free space first (`launcher/net.py` `space_problem()`), so a full disk is told before a file is written, not as an
error halfway through.

### `prepare()`

Puts the mods folder in order for the game, as the notes at the top of this section say, and returns what it did, one
sentence each.

- An archive whose name Windows cannot keep as a folder is refused before the folder is looked for: Windows says a
  device's name exists in every folder.
- An Atmosphere layout straight in the mods folder is a mod as it is, and a link (or a junction) is the player's own
  arrangement: neither is unwrapped.

### `_staging()`

A new, empty folder for `add()` to put a mod together in. It is beside the mods folder, not in it: the game and the
launcher take every folder of the mods folder for a mod, and beside it is the same disk, so the finished mod moves in
with one rename. It is the mods folder itself when nothing can be made beside it. What a launcher that was stopped in
the middle left there a day ago or longer is removed.

### `_move_in()`

Renames the finished mod folder into the mods folder under `name`, or the first free name after it (looked for now,
not before the copy: a folder of that name may have come meanwhile), and returns where it is.

- `EXDEV` means the mods folder is on another disk than its parent (a mount, a junction): the folder is moved by
  copying.
- Another error is tried again after 0.3 s, three times in all: a scanner may still hold a file just written.

### `add()`

Adds an archive or a folder the player picked as a mod: unpacked or copied into the mods folder under its name (a free
one), in order for the game. Returns the new mod's folder.

The mod is put together in a folder beside the mods folder (`_staging()`) and moves in only when it is complete. A
copy that stops halfway (a full disk, a file that cannot be read, the launcher closed) leaves nothing in the mods
folder: copied straight into it, the half that had arrived was a mod of its own, switched on, at the game's next
start. The staging folder is removed in a `finally`, so also on Ctrl+C and when the window closes.

When the player picked the mod's `romfs` (or `atmosphere`) folder itself, a folder is made around it.

## `launcher/mods_job.py`

`drop_build_cache()` removes the lifted sources and objects when the player does not keep the build cache, while
preserving build state needed for update. Its body is the former Tk method; `App` continues to expose it for other
jobs. `finish_mods_job()` applies this policy after a successful mod rebuild and returns `resume` with cleared failed
mods. A cancelled or failed rebuild retains the waiting mod names and returns its result for the caller to choose the
screen or retry. Tk preserves its existing result timing. A worker calls with `require_finished=True`, which returns
`pending` without cleanup until the job has completed marker, claim and lock cleanup.

### `drop_build_cache()`

With Keep Build Cache off it deletes the lifted C and the objects (about 1.5 GB) but keeps `state.json`, which an
update needs to rebuild without the dump; the next build then lifts and compiles everything again.

## `launcher/native/common.h`

Shared by `NX2Saria.exe` (`launcher/native/nx2saria.c`) and `NX2Saria-Setup.exe` (`launcher/native/setup.c`): paths,
small files, shortcuts and the uninstall entry. Wide-character Win32 throughout, so folder names in any language work.

`APP_USER_MODEL_ID` is the launcher window's taskbar identity (`launcher/winsys.py` `app_id()`): a shortcut that
carries it is the one the taskbar groups and pins the window under, with the shortcut's icon.

- `read_line()`: the first line of a small UTF-8 text file, trimmed, as wide text; 0 when absent or empty.
- `version_cmp()`: compares dotted versions ("0.3.11" > "0.3.9"); build metadata after `+` is ignored.
- `make_shortcut()`: a `.lnk` that starts the target (with its own icon).
- `shortcut_path()`: the Desktop or Start menu shortcut's path.
- `shortcut_in_root()`: a legacy shortcut may belong to a separate install, so it is replaced or removed only when it
  points into this root.
- `set_shortcut()`: creates (`on`) or removes the Desktop or the Start menu shortcut to `root\NX2Saria.exe`.
- `write_uninstall_entry()`: the entry in Settings > Apps that runs `NX2Saria.exe --uninstall`.

## `launcher/native/nx2saria.c`

`NX2Saria.exe`: the launcher's own program, in the launcher folder (`launcher/paths.py`).

1. A Python staged by an update (`python.next`) replaces `python\` while nothing runs from it.
2. The release named by `app\current.txt` starts: `python\pythonw.exe app\<version>\launcher\start.pyw` with this
   program's arguments, the launcher folder as working folder and in `NX2SARIA_LAUNCHER_ROOT`.
3. When that release fails to start (exit code 86 from `launcher/start.pyw`, or any non-zero exit within six seconds)
   and `app\previous.txt` names another complete release, `current.txt` goes back to it and it starts with
   `--rolled-back <version>`.

Its own commands, for the launcher and the setup: `--desktop-shortcut on|off` and `--shortcuts on|off` (Desktop and
Start menu). It has no console window (`-mwindows`).

### `swap_python()`

When `python\` cannot be moved aside it is in use: the swap is tried again at the next start.

### `start_release()`

Starts the release; returns its exit code if it ended within `QUICK_MS`, else -1 (still running).

## `launcher/native/res.rc`

The resources of `NX2Saria.exe` and `NX2Saria-Setup.exe`: the original leaf sprig icon (`nx2saria.ico`, written by
`tools/make_launcher.py` from `launcher/ui/art.py` `sprig_ico()` into the build folder and found through `-I`) and
the manifest, `launcher/native/app.manifest`.

## `launcher/native/setup.c`

`NX2Saria-Setup.exe`: the one-click setup. It is this program with the launcher's folder appended as a zip
([launcher.md](../launcher.md#releases)).

It unpacks the zip into `%LOCALAPPDATA%\Programs\NX2Saria` (no administrator rights), adds the Start menu and Desktop
shortcuts and the entry in Settings > Apps, and starts `NX2Saria.exe` with `--from <the setup's folder>`, where the
launcher looks for the player's dump first. A small window in the launcher's colours shows while it unpacks. When the
folder already holds a newer version than the setup carries, the setup leaves the files alone (the launcher updates
itself) and only repairs the shortcuts.

The end of the file is a trailer: "NX2NSET1", the zip's offset and size (u64 little endian) and the version (24 bytes,
NUL padded), written by `tools/make_launcher.py`. The zip is read here with zlib's inflate, so the setup needs nothing
from Windows beyond its own DLLs.

Options: `--dest DIR`, `--no-shortcuts`, `--no-launch`, `--quiet` (no window; for tests).

The `result` of the `Setup` state is 0 when all went well, else the id of an error message.

### `draw_sprig()`

An original three-leaf sprig in the launcher's forest palette; no artwork derived from the game.

### `make_dirs()`

Makes every folder above the path, which is a file's.

### `unzip()`

The end-of-central-directory record is read as the last 22 bytes: the zip has no comment.

### `installed_ok()`

Whether the installed release can start: its launcher (`launcher/native/nx2saria.c` `release_ok()`) and the Python it
runs on.

### `work()`

A newer launcher (one that updated itself) stays, unless it cannot start: the stub sends the player to this setup to
repair just that, and the setup's own release then replaces it.

### `wWinMain()`

- Without `--dest` and without the known folder, the target comes from `LOCALAPPDATA`: `GetEnvironmentVariableW`
  returns 0 when the variable is unset and `PATHN` or more when it does not fit (the buffer is then left unwritten);
  16 characters of room are kept for `\Programs`.
- The current install's folder is preferred; when only the folder of the port's earlier name exists, it is updated in
  place.

## `launcher/net.py`

HTTPS for the launcher: JSON requests and file downloads that resume, report progress, can be cancelled and are
checked against a sha256. It uses the standard library (`urllib`), and on Windows the truststore package of the
launcher's Python for Windows' own certificate check.

Two checks can accept a server's certificate, tried in turn by `request()`:

- Python's: OpenSSL with the certificates of Windows' ROOT and CA stores (`ssl.create_default_context`). It knows only
  the certificates already in those stores, while Windows holds few roots and downloads the others the first time a
  Windows program needs them. And it takes a stored certificate before the server's copy of the same name and key: a
  store that still holds an old, expired cross-signature of the root a server's chain leads to makes it report
  "certificate has expired" for a valid chain (the chain of repo.msys2.org on a PC whose CA store still holds the
  "ISRG Root X2" cross-signature that expired in September 2025).
- Windows': Windows' own chain engine, which Windows programs check servers with (`CertGetCertificateChain` and
  `CertVerifyCertificateChainPolicy`, through `truststore.SSLContext`). It weighs every path the stores and the server
  allow, prefers a valid one, and downloads a missing root from Windows Update.

A certificate both refuse ends the request with `CertificateError`: what each check said, whether the PC's clock
agrees with the server's, and what to do.

- `CLOCK_TOLERANCE` (3600) is the seconds between this PC's clock and a server's beyond which this PC's is wrong.
- `_CHECKS` is `[(name, SSLContext)]` in the order `request()` tries them. `_ACCEPTED` maps a host to the index into
  `_CHECKS` of the check that accepted its certificate last.
- `SPACE_MARGIN` (32 MiB) is what a disk keeps free beyond what is about to be written to it (`space_problem()`).
- `PART_OF` (`.of`) names `<dest>.part.of`: what the part file beside it is a part of (`download()`).

### `CertificateError`

No certificate check accepts a server's certificate. Asking again gets the same answer, so `download()` does not
retry it.

### `_windows_check()`

An `SSLContext` that leaves the certificate to Windows' own verifier, or `None` off Windows, in a Python without
truststore (a checkout's own Python, or `crypt32` not reachable through ctypes) and under Wine. Wine's `crypt32`
accepts a chain that stops at an issuer it does not know (`CERT_TRUST_IS_PARTIAL_CHAIN` passes its SSL policy), and
its roots are the host's, which Python's check reads already.

### `_plain_ok()`

Plain HTTP only to this PC (the tests' local servers): anything fetched from elsewhere comes over TLS.

### `_Redirect`

Follows a redirect only to https (or to this PC), and drops the `Authorization` header when the redirect leaves the
host it was meant for: `urllib` copies every header given to the request onto the redirected one, and GitHub sends
asset downloads on to its file servers.

### `request()`

The open response to a GET of the address (or `method`, with `data` as its body: the session reports' POST and
DELETE, `launcher/telemetry.py`). An https address is tried with each certificate check in turn, first the one that
accepted the host last, until one accepts the server; `CertificateError` when none does. Plain http is refused but for
this PC's own address.

### `server_clock()`

The time (epoch seconds) of the `Date` header a plain-HTTP HEAD request answers with, without following a redirect, or
`None` without an answer. A PC whose clock is wrong cannot check any certificate, so its clock is compared over HTTP;
the answer is not authenticated and only chooses the advice of an error message.

An HTTP error (the redirect to https, or any other status) carries a `Date` that is as good. No answer, whatever the
reason (refused, timed out, not HTTP), gives `None`.

### `certificate_problem()`

The message of a `CertificateError`: what each check in `refused` (`[(name, error)]`) said, then the advice that fits
this PC's clock.

### `_check_eof()`

`HTTPResponse.read(n)` accepts an early end of a body with `Content-Length` and leaves the missing byte count in
`length`; this turns that into an error.

### `get_bytes()`

The body of a GET, at most `limit` bytes. A cut connection, a timeout or a server error (5xx, 429) is tried again
after 1, then 2 seconds, as those of `download()` are: one connection reset during Gather Build Tools (the package
index read by `launcher/toolchain.py` `ensure_zbic()`) ended the install. A refused certificate or another HTTP error
is not tried again.

### `NoSpace`

The disk has no room for the file. Asking again or another server gets the same answer, so `download()` does not
retry it, and it keeps the part file: the download goes on from it once there is room.

### `free_bytes()`

The free bytes of the disk that holds the folder, asked of the nearest folder above it that exists; `None` when that
cannot be asked.

### `space_problem()`

Empty when the disk of the folder has room for `need` more bytes and `SPACE_MARGIN`, or when its free space cannot be
asked; else the sentence that says there is no room for `what`, how much it takes and how much is free. It is asked
before a download or an unpack starts: a disk that fills in the middle otherwise ends it with an error that names no
cause and leaves half of it behind.

### `download()`

Fetches the address to `dest` through `dest.part`, resuming a part file a cut connection left. `progress(done, total)`
is called after every chunk; `cancel()` returning true raises `Cancelled`. A wrong size or sha256 deletes the file and
raises `DownloadError`. A `dest` already holding the expected sha256 is kept without a request. `NoSpace` is raised,
before anything is written, when the disk has no room for the rest of the file (the expected size, else the size the
server names).

A part file is resumed only as a part of the same file: `dest.part.of` beside it names the file it was begun for, by
the expected sha256 (the same file from any server) or, without one, by its address. A part begun for another file
under the same name (a release published again with other bytes) is deleted with a line for the log, where resuming
it ended in "sha256 ..., expected ..." as if the download were damaged. A part without that note (an older
launcher's) is resumed as before, and so is one whose note could not be written.

- A server that ignores `Range` (an answer other than 206 to a resumed request) makes the download start over.
- The whole file's size for progress is the expected one, else what the server says is still to come plus what the
  part file holds, else unknown (0: progress then gets the bytes so far as the total). An earlier form,
  `length + have or size or 0`, took the part file's size for the whole when a resumed answer named no length, and the
  bar ran past its end.
- A server that sends more than the expected size is stopped: it would fill the disk.
- HTTP 416 with a part file means the part is complete or longer than the file: it is deleted and the download starts
  over.
- HTTP 401, 403, 404 and 410 and any 3xx (a redirect `_Redirect` refused) are not tried again.
- `ENOSPC` while writing means the disk filled meanwhile: trying again cannot help, and `NoSpace` is raised.

## `launcher/paths.py`

`install_folder()` keeps the chosen folder if it is empty, has a build cache, or already has an NX2Saria or
NX2navi name. Otherwise it returns the NX2Saria child. The Tk picker saves this result as it always did.
The Electron worker uses it as a proposal without creating the folder or saving settings.

Where the launcher keeps everything ([launcher.md](../launcher.md#the-launchers-folder)).

An installed launcher (`NX2Saria-Setup.exe` puts it in `%LOCALAPPDATA%\Programs\NX2Saria`) is one folder:

| Path | What it is |
|---|---|
| `NX2Saria.exe` | `launcher/native/nx2saria.c`: starts `python\pythonw.exe` on `app\<current>\launcher\start.pyw` |
| `python/` | the embeddable Python with tkinter, numpy, lz4 and zstandard (`tools/make_launcher.py`) |
| `app/current.txt` | the version the stub starts; `app/previous.txt` the one before it, kept for a rollback |
| `app/<version>/` | one release: `launcher/`, `runtime/`, `recomp/`, `tools/`, `bin/`, `VERSION`, `toolchain.lock.json` |
| `pydeps/` | zbic, downloaded from PyPI on this PC (`launcher/toolchain.py` `ensure_zbic()`) |
| `toolchain/msys64/ucrt64` | MSYS2's clang and libraries, downloaded from the MSYS2 repository on this PC |
| `toolchain/pkgs/` | the downloaded package files, kept while the lock names them |
| `data/launcher.json` | settings; `data/logs/` logs; `data/cache/` the fonts read from the player's game |
| `game/` | the built game, unless the player picks another folder |

Run from a checkout of the repository (`python -m launcher`), the app folder is the repository and everything else
goes to `build/launcher-dev/`, which git ignores.

On Windows a file's whole path may be 259 characters (`WINDOWS_PATH_MAX`) for the programs that do not ask for more,
and the build's tools (the MSYS2 compiler, linker and resource compiler, `tar`) are among them whatever the system's
long path setting. `LONGEST_IN_GAME` (128) is the room the game folder's own files need below it: the longest path a
built game folder held was 106 characters
(`romfs-patches-playstation-label\System\Resource\ResourceSizeTable...rsizetable.zs`), and a file is written under a
temporary name up to 22 characters longer before it gets its own.

### `ansi_safe()`

True when the path survives the Windows ANSI code page: the MSYS2 tools and the game's own build read their arguments
through it, so a folder name outside it breaks the build. Always true elsewhere.

### `long_path_problem()`

Empty when the files of a game built into the folder fit Windows' path length, else the sentence that says how long
the folder's path is and how long it may be. Always empty elsewhere. Without it a build into a deep folder stops
somewhere in the middle with "file not found" for a file that is there.

### `fixed_drive()`

True when the drive (`X:\`) is a fixed disk (`GetDriveTypeW` giving `DRIVE_FIXED`), not a network, removable or optical
drive. True where that cannot be asked.

### `Layout`

`instance_lock` (`data/launcher.lock`) is held by whichever process works in this folder: the window while it is open,
`launcher/cli.py` for a command and every job that changes files (`launcher/locks.py`).

- `Layout.default_game_dir()`: `<root>/game`, or a `Games` folder on the fixed drive with the most free space when the
  root's drive has less than 40 GB free or the root's path would break the build. A mapped network share or a USB
  stick is no place for 30 GB of build, so only fixed drives are looked at.
- `Layout.python_for_builds()`: the console `python.exe` beside the launcher's `pythonw.exe` (no window: the build
  starts it with `CREATE_NO_WINDOW`), or this interpreter in a checkout.

### `free_gb()`

Measures space on the nearest existing parent for game folders yet to be created.

### `partial_image_gb()` and `drop_partial_image()`

`partial_image_gb()` is the size of what an install that was cut short left of the RomFS image
(`romfs/romfs.bin.part`, up to 26 GB). The next install writes the image anew (`tools/make_game.py` `step_romfs()`),
so that space is the install's to use: the first-run screen counts it as free. `drop_partial_image()` removes that
file, so that it does not count against the free space an install asks for.

The Tk launcher and the worker share these helpers and the `INSTALL_SPACE_GB` threshold.

## `launcher/platform/__init__.py`

Operating-system services used by the launcher. `launcher/winsys.py` remains the stable call surface while
platform-specific implementations move behind it in small steps.

### `game_executable()`

The built game's executable in its selected game folder: `ocarina.exe` on Windows, `ocarina` elsewhere.

## `launcher/platform/linux.py`

The Linux services behind the launcher's compatibility surface, `launcher/winsys.py`.

- `APPIMAGE_OWNED` is what the AppImage install (`tools/appimage_install.py`) puts in the launcher's folder besides
  `data/`: the start script (`_write_wrapper()` there), the menu icon, the app versions, the Python runtime and the
  folders an install or an update leaves while it swaps it, the Python packages and the build tools.
- `KEPT_DATA` is what Uninstall keeps of `data/`: the launcher's logs, which the player may still want for a report.
- `UNINSTALL_SCRIPT` is run by `sh` once the launcher has gone: `$1` is its process id, `$2` the `launcher.log` the
  last line goes to, then the paths to delete, `--`, and the folders to delete only when empty. The wait is cut at
  30 s; a file the launcher still has open is only unlinked, which Linux allows.
- `USER_DIRS` maps the names `launcher/winsys.py` `known_folder()` takes to the keys of `user-dirs.dirs` for them.
- `LOCAL_FILESYSTEMS` are the filesystems of local disks, whose mount points the dump search looks through. Not
  network ones (a slow or unreachable server would hold the search), not autofs placeholders (listing one mounts the
  disk behind it), and not the system's own (proc, tmpfs, overlay and the like).

### `make_game_toolchain_args()` and `ensure_toolchain()`

Linux uses its system compiler and libraries, which `tools/make_game.py` discovers itself, so there are no toolchain
arguments. `ensure_toolchain()` checks the build's prerequisites (`toolchain_missing()`) and installs the launcher's
own zbic package.

### `refresh_installed_wrapper()`

Writes the installed start script again from this app version, so that later starts have its rollback behaviour.

### `game_running()`

Checks the runtime's profile lock first, then `/proc` for Linux builds from before profile locking.

### `child_subreaper()`

Turns `PR_SET_CHILD_SUBREAPER` (`prctl` option 36) on or off for the launcher's process. It is on while the game runs
(`launcher/winsys.py` `start_game()`): the copy the game's Restart spawns (`runtime/platform/linux.c`
`plat_restart()`) is the old process's child, and once that one ends it becomes the launcher's, whose wait gives its
exit code. It is off again when the run ended (`launcher/winsys.py` `game_finished()`), so that the processes a build
leaves behind are not adopted and kept as zombies.

### `_runs()`

Whether the process folder (`/proc/<pid>`) runs the expected file; false once it has ended or is a zombie, whose `exe`
link cannot be read.

### `GameCopy`

A copy of the game this launcher did not start itself (`game_copy()`), with `poll()`, `returncode` and `pid` as
`subprocess.Popen` has them; `ended` is true once it has ended, also when its code could not be read. A copy that is
the launcher's child (adopted, `child_subreaper()`) is waited for, which gives its code; another is taken as ended
once its `/proc` folder no longer runs the game's file, so a process id used again is not taken for the game.

### `game_copy()`

A live process running the file `exe` other than the process id `skip`, as a `GameCopy`; `None` when there is none.
The file is compared by its resolved path, so the game of another install is not taken for this one.

### `ended_copies()`

Waits for the launcher's children that ended as zombies named as the file `exe` (the `comm` of the `stat` line, which
the kernel cuts to 15 characters), other than the process id `skip`: a copy of the Restart that ended before the
launcher looked for it (`launcher/ui/app.py` `App.watch_game()` looks once a second). It returns their exit codes, the
latest started last, by field 22 of the `stat` line, the start time.

### `desktop_entry()`, `appimage_install()`, `_exec_line()` and `_starts()`

- `desktop_entry()`: the menu entry the AppImage writes (`tools/appimage_install.py` `_write_desktop()`).
- `appimage_install()`: whether the folder holds the AppImage's install, by its start script or the record of what it
  installed (`tools/appimage_install.py` `_write_wrapper()` and `_write_install_manifest()`). Uninstall deletes nothing
  elsewhere.
- `_exec_line()`: the `Exec` line `tools/appimage_install.py` `_write_desktop()` writes for the start script, quoted as
  `tools/appimage_gui.py` `exec_quote()` quotes it.
- `_starts()`: whether the menu entry starts that script: another install's entry is left alone.

### `_within()`

Whether a path is the folder or lies inside it, by the paths as given and as resolved (a symbolic link on the way
makes the two differ).

### `_unsafe_game_folder()`

Why the folder must not be deleted as the game folder, or empty: it is or holds the launcher's folder or the home
folder, or it is a mount point (a drive's or a card's root).

### `uninstall_plan()`

`(paths to delete with all they hold, folders to delete only when that left them empty, (path, why) kept)` for the
launcher's folder ([launcher.md](../launcher.md#uninstalling)). The game folder goes only when `remove_game` names
it, and never when it holds the launcher's folder or the home folder or is a drive's root (`_unsafe_game_folder()`).
`data/logs` stays (`KEPT_DATA`), and so does every path that holds one of `keep` (the game folder when it stays, the
dump) or lies in it: a game folder inside `data/` is not deleted with `data/`.

### `uninstall_command()`

The `sh` command line that deletes `gone`, then the empty folders of `empty`, once the process `pid` has ended.

### `udmabuf_status()`, `run_udmabuf_fix()`, `udmabuf_log_line()` and `udmabuf_after_log_line()`

[launcher.md](../launcher.md#linux-guest-memory) says what udmabuf is to the game.

- `udmabuf_status()` describes udmabuf without privilege; its paths and probes are parameters so that tests keep the
  check isolated.
- `run_udmabuf_fix()` asks PolicyKit to run the contents of the packaged script
  (`launcher/platform/udmabuf_fix.sh`), once the player confirmed, through the system shell.
- `udmabuf_log_line()` is one bounded diagnostic line for the launcher's log at start;
  `udmabuf_after_log_line()` describes the device right after an approved fix action.

### `write_watch_verdict()`, `write_watch_probe()`, `write_watch_status()`, `write_watch_log_line()` and `write_watch_text()`

Whether the game will track written memory on this kernel, said before Play: without it the game runs much slower
([known-issues.md](../known-issues.md#linux-kernel)).

- `write_watch_probe()` makes the game's own probe through `ctypes` (`runtime/platform/linux.c` `ww_open()`): the
  userfaultfd call in user mode only, the API handshake asking for asynchronous write protection, and opening
  `/proc/self/pagemap`; 0 when all worked,
  the errno when not, `None` where it could not be made (another architecture, no `syscall` in the C library). The
  numbers are the kernel's: the call is 323 on x86-64 and 282 on arm64, `UFFDIO_API` 0xC018AA3F, the features
  `UFFD_FEATURE_WP_ASYNC` (1 << 15) and `UFFD_FEATURE_WP_UNPOPULATED` (1 << 13).
- `write_watch_verdict()` is `runtime/platform/write_watch_note.h` `ww_verdict()` in Python ('on', 'old', 'refused');
  a probe not made decides by the version alone. `tests/write_watch_check.py` holds the two to the same cases.
- `write_watch_status()` reads the version and probes; `NX2SARIA_TEST_NO_UFFD` fails the probe as the game's does.
- `write_watch_log_line()` is the launcher log's line at start; `write_watch_text()` the home screen's row value and
  text, or `None` when tracked.

### `os_name()`

The distribution as `/etc/os-release` names it (`PRETTY_NAME`, else `NAME` and `VERSION_ID`), or "Linux".

### `session_text()`

The graphical session for the launcher's start line: its type (x11, wayland, tty) and desktop, from the variables the
display manager sets, or from which display variable is there when it sets none.

### `user_dir()`

The folder the desktop keeps for a name (`downloads`, `desktop`, `documents`, `pictures`), as
`$XDG_CONFIG_HOME/user-dirs.dirs` names it: translated ("Área de Trabalho", "Documentos") on a desktop set up in
another language. `None` when the file names none, or names the home folder itself (the desktop's way of turning one
off).

### `_unescape_mount()`

A mount point as `/proc/self/mounts` writes it, with its octal escapes (`\040` for a space) undone.

### `mounted_drives()`

The local disks mounted under `/mnt`, `/media/<user>` and `/run/media`, where a player's other drives are (an NTFS
partition shared with Windows, a second disk; SteamOS mounts the SD card at `/run/media/mmcblk0p1` or
`/run/media/<user>/<label>`), in the order mounted. Only filesystems mounted now: nothing here mounts a disk.

### `pick_path()`

The desktop's own chooser for a file (`kind` `file`) or a folder (`folder`): kdialog on KDE, else zenity, else
kdialog. It returns `(tool, path)`, with an empty path when the player cancelled, and `(None, '')` when neither is
installed or both failed, and the caller falls back to Tk's chooser. `patterns` are `(label, '*.ext')` pairs for a
file.

Exit code 0 gives the path as the file system has it, also when it is not UTF-8; both tools exit with 1 when the
player cancelled.

### `_sdl_library()`

Loads the kit's SDL3 first, then the system's copy, and returns a short label of the source for the log.

### `Gamepad`

Reads SDL3 game controller input, including Steam Input's virtual controller on Steam Deck.

### `keep_awake()`

Holds a systemd sleep inhibitor for the duration of a foreground build job.

### `toolchain_missing()`

Checks the compiler and the headers the Linux build requires, with guidance per distribution; it changes no package.

## `launcher/platform/windows.py`

`game_running()` snapshots Windows processes with Toolhelp. It filters by executable name, optionally
checks the parent chain against the given owner PID, then gets the candidate's full path through a process
handle. The check accepts equal normalized paths or file identities. Windows can refuse access or end a
process between those calls. An unverified candidate is skipped, and the snapshot and process handles
are closed when their queries are finished.

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

## `launcher/saves.py`

`export_destination()` shares the same preference and existing-directory check between Tk and
the worker. A missing or invalid saved folder falls back to `default_export_dir()`.


Importing a save into the built game, and the saves folder of the game folder
([launcher.md](../launcher.md#sharing-a-save), [launcher.md](../launcher.md#importing-a-save-folder)).

The game keeps its save in `<game>/profile/save/<account>/` (`runtime/hle/fs_host.c` `storage_init()`). The folder is
named after the local account in `<game>/profile/account.uid`: 16 bytes, two little-endian 64-bit halves, the low one
first; the name is the high half then the low half, 16 hex digits each (`runtime/hle/ov_system.c` `load_uid()`). A
game that never ran has no account yet; the import makes one as the game would. Saves are committed through
`<game>/profile/save-tx/`, where a finished commit (`ready-<n>`) is newer than `save/<account>`: an import clears that
folder, so the game loads the imported save.

`<game>/saves` is the player's side. Import Saves takes a save put there first (a shared save's zip, its files, or the
folder that holds them); otherwise the player picks a zip or a folder. Before the save is replaced it is copied to
`saves/backup <time>`, and a save taken from `saves/` moves to `saves/imported <time>`, so nothing is lost and nothing
is imported twice.

A shared save is one zip (`export_save()`, `import_zip()`): a player attaches it to a bug report, and whoever fixes it
imports it to reach the same place in the game. The game's settings menu exports and imports the same zip; its import
waits in `<game>/profile/save-import` for the game's next start.

A save folder can come from a Switch save backup (the save's files), an emulator (nested folders named by user and
title ids, gone down while each holds only one folder), another game folder of this port or its profile folder (the
account folder inside).

A whole save is `account.sav`, `option.sav` and the folders `slot_00` to `slot_05`, each with `progress.sav` and
`caption.sav`. A folder with `progress.sav` at its top is one slot: it replaces that slot (the slot named by its
folder, else `slot_00`) and the rest of the current save stays, as the game reads `progress.sav` only inside a slot
folder.

- `MAX_FILES` (4000) and `MAX_BYTES` (1 GiB): a save is a few files of a few MB; more is a folder picked by mistake.
- `OURS` matches what this module writes into `saves/` (`backup <date>`, `imported <date>`).

### The shared save's zip

`NX2Saria-save-<date>_<time>[-bug-report].zip` holds the save and a manifest:

| Member | What it is |
|---|---|
| `manifest.json` | `FORMAT`, `FORMAT_VERSION`, the game's title id and version, the port's version, the export time |
| `save/...` | the save's files as the game keeps them (`account.sav`, `option.sav`, `slot_00/progress.sav`, ...) |
| `bug-report/...` | with `bug_report`: the game's latest log and its settings file; an import leaves them out |

The game's own menu writes and reads the same zip (`runtime/hle/save_share.c`), with the same manifest keys.

- `PENDING` (`save-import`) is `profile/save-import`: what the game's menu took from a zip, imported at the game's
  next start.
- `SAVE_FILE` matches what makes a folder this game's save (`account.sav`, `option.sav`, `slot_NN/progress.sav`).
- `PRIVATE_SETTINGS` are the settings whose values stay on the player's PC (`runtime/cfg/cfg_private.h` has the same
  list): the menu's test hook, and what the co-op of versions up to 0.9.2 kept in the settings file, which a player's
  file may still hold.

### `_rmtree()`

Deletes a folder, read-only files in it too (`shutil.rmtree` stops at them on Windows); what still fails stays.

### `ensure_saves_dir()`

`<game>/saves` with its `README.txt`, which is written again when it is missing or an older launcher's.

### `account_dir()`

The game's save folder for its local account, making the account as the game does when it has none.

### `_fresh()` and `_fresh_file()`

`folder/name`, or `folder/"name (2)"` and so on when that is taken (two imports in one second); `_fresh_file()` does
the same for `<stem><suffix>`.

### `find_save()`

`(source folder, top-level names to leave out)` of the save in a folder, or `(None, ())` when it holds none.

- `profile/save` or `save` inside it means another game folder, or its profile folder. A finished commit in the
  `save-tx` beside it is newer than its account folder and is taken first.
- Nested id folders are gone down, at most 8 levels, while the folder holds a single folder and no file.
- A backup this module wrote keeps the replaced save's journal (`save-tx`) beside it, which is not part of the save
  and is left out.

### `_one_slot()`

The slot folder the files belong in when the source is a single slot, `None` when it is a whole save.

### `waiting()`

`(source, names to leave out)` of a save put into `<game>/saves`, or `(None, ())`. A zip there (one of
`export_save()`) comes first, the newest one; its source is the zip file.

### `_files()`

Relative paths of the files to import; `SaveError` when there are too many or too large to be a save.

### `_ready()` and `_newest()`

`_ready()` gives the newest finished commit (`ready-<n>`) in a `save-tx` folder, or `None`. `_newest()` gives the
folder the game would load now, in the order of `runtime/hle/fs_host.c` `save_recover()`: a finished commit still
waiting in `save-tx`, else the account folder, else an interrupted commit's new save, else the save it replaced.

### `game_running()`

`launcher/build.py` `game_running()`: on Windows, `ocarina.exe` cannot be opened for writing while it runs.

### `profile_lock()`

The game's own lock on its profile, held while the save is replaced. The game takes `<game>/profile/instance.lock`
when it starts and keeps it until it ends (`runtime/hle/fs_host.c` `fs_host_lock_profile()`): on Windows by opening the
file without sharing, elsewhere with `flock`. Taken here the same way, it says for certain whether the game runs
(`SaveError` when it does), and a game started while it is held ends at once with its "another copy is running"
message instead of mounting a save that is half replaced. `game_running()` alone, asked before the player picked a
file, left the whole time of that dialog and of the copy for the game to start in. A lock file that cannot be opened
for another reason locks nothing: the import goes on as before.

- Windows: `CreateFileW` with read and write access, no sharing, made when missing, which is the call of
  `runtime/platform/win32.c` `plat_lock_file()`. Error 32, `ERROR_SHARING_VIOLATION`, means the game has it open.
- Elsewhere: `flock` with `LOCK_EX | LOCK_NB`, the call of `runtime/platform/linux.c` `plat_lock_file()`. A file
  system without locks locks nothing.

### `pending_import()`

The save the game's menu took from a shared zip and left in `<game>/profile/save-import` for its next start
(`runtime/hle/fs_host.c` `import_pending()`), or `None`.

### `import_save()`

Makes the save in a folder (`None`: the one waiting in `<game>/saves`) the game's save. The folder may also be a zip
of `export_save()` (`import_zip()`). It returns `{'files', 'source', 'backup', 'slot'}`; `backup` is the folder the
replaced save went to, or `None` when there was none.

- The saves folder itself, picked in the dialog, means what waits in it, as the button takes it.
- A shared save's zip put into `saves/` moves out once imported, as a folder does.
- What was put into `saves/` moves out whole (`_put_away()`, into `saves/imported <stamp>`), so the next import does
  not take any of it again. When the save sits in a folder that was put there (a game folder, an emulator's id
  folders), that top folder moves.

### `_replace()` and `_replace_held()`

Copies the files of the source (relative paths) into the game's save: the whole save, or with `slot` only that slot
folder and the rest of the current save kept. The save the game would load now goes to `saves/backup <stamp>` first.
It returns `(account folder, backup folder or None)`. The game's profile lock is held meanwhile (`profile_lock()`):
`SaveError` when the game runs.

- With a slot, the rest of the save stays; a slot's files left at the top by mistake do not.
- A save copied from read-only media keeps the attribute through `copy2`, and the game rewrites its files, so each
  copied file is made writable.
- The journal of the replaced save goes out of `save-tx` (the game would load a finished commit first, or complete an
  interrupted one): what holds files moves into the backup, beside the save it was copied from, so a copy the backup's
  own choice missed (an interrupted commit's other half) is kept too.
- The imported save goes into place before the old one is deleted; a failed rename puts the old one back, and the
  journal too: a finished commit waiting in `save-tx` is newer than the account folder, and without it the game would
  load the older save although nothing was imported.
- A save the game's menu took for its next start (`PENDING`) would replace this one then: it is removed, because this
  import is the newer choice.

### `_key_file()`

Key files never go into a zip, wherever a stray one was copied (a save folder taken from another tool).

### `default_export_dir()`

The player's Downloads folder (where the system keeps it), else the Desktop, else the home folder.

### `scrub_settings()`

The bytes of the game's settings file for a bug report: every private setting that has a value reads
`key = (set, not shown)`; everything else, comments and line ends included, stays byte for byte (`cfg_ini_scrub()` in
`runtime/cfg/cfg_private.h` does the same for the game's own export).

### `short_paths()`

The bytes of a log or settings file with the account's folders written short (`tools/private_paths.py`); bytes that
are not UTF-8 pass through as they are.

### `_add_report_file()` and `_report_files()`

A report file goes in under its current archive name, and the settings are scrubbed under either file name
(`nx2saria.ini`, `nx2navi.ini`). Both the log and the settings have the account's folders written short: a log from a
game run with `log_short_paths = 0`, or from a version before it, has them in full, and a report goes to people the
player does not know. `_report_files()` takes the current files first and falls back to the old names without moving
or overwriting them.

### `_export_log()`

The bug report of a game without a save (it stopped before its first one):
`NX2Saria-log-<date>_<time>-bug-report.zip` with the game's latest log, `nx2saria.ini` and a manifest that counts no
save files; `import_zip()` refuses it as a zip without a save. `SaveError` when the game has written no log either:
then there is nothing to report with.

### `export_save()`

Writes the save the game would load now into one zip in `dest` (`default_export_dir()` when `None`), for a bug report
or another player. `bug_report` adds the game's latest log and `nx2saria.ini`, and still writes those two when the
game has no save yet (`_export_log()`): a game that stops before its first save is the one its player wants to report.
Never a key file. It returns `{'path', 'files', 'extras'}` (`files` 0: no save; `extras`: the names of the bug report
files that were there).

- Without `account.uid` nothing is exported: `account_dir()` would make the account, which an export must not.
- A running game may swap a commit in while its files are read (`runtime/hle/fs_host.c` `swap_in()`), so the read is
  tried up to three times.

### `_member_path()`

The relative path of a zip member below `save/`, or `SaveError` when it could land outside the save.

### `check_manifest()`

`SaveError` unless the manifest is one of a save of this game that this version can read.

### `read_zip()`

The manifest of a zip of `export_save()` and its save files as `[(ZipInfo, relative Path)]`, checked: the manifest
(`check_manifest()`), every path inside the save, no links, at most `ZIP_MAX_FILES` files and `ZIP_MAX_BYTES`, and
among them the files of this game's save (`SAVE_FILE`). `SaveError` otherwise. Members outside `save/` (the manifest,
the bug report files) are passed over.

### `import_zip()`

Makes the save in a zip of `export_save()` the game's save (a whole save: every slot), backed up first as
`import_save()` does. It returns the dict of `import_save()` with the zip's manifest under `manifest`. The CRC of each
member is checked as it is read.

## `launcher/settings.py`

`data/launcher.json`: the player's choices and what the launcher remembers between runs.

`DEFAULTS`, the keys with notes:

| Key | Default | What it is |
|---|---|---|
| `game_dir` | empty | the built game; empty until the first install picks one |
| `dump` | empty | the `.nup` (or the folder with `exefs/` and `romfs/`) the game was built from |
| `auto_check` | on | look for a new version when the launcher opens, and every hour while it runs |
| `auto_install` | on | install a downloaded update by itself whenever the game is not running |
| `channel` | `stable` | `stable` or `preview` (GitHub pre-releases) |
| `hide_while_playing` | on | hide the window while the game runs, show it again when it exits |
| `warn_low_commit` | on | warn before Play only with a disabled or fixed-size Windows page file |
| `keep_cache` | on | keep `build-cache/` (the lift and the objects) so an update recompiles only changes |
| `save_export_dir` | empty | where Saves > Export Save writes its zip; empty: the Downloads folder |
| `export_with_log` | on | Export Save adds the game's latest log and settings file (for a bug report) |
| `theme` | `forest` | the colour scheme (`THEMES` of `launcher/ui/art.py`) |
| `custom_background` | empty | the Custom scheme's backdrop colour, `#rrggbb`; empty: the first scheme's |
| `custom_panel` | empty | the Custom scheme's colour of the panels and the title band |
| `custom_buttons` | empty | the Custom scheme's colour of the buttons |
| `background` | empty | the picture the player chose as the background; its fitted copy is `data/background.npy` |
| `background_dim` | 40 | how much that picture is darkened, in percent |
| `animations` | on | the cursor glides, screens slide in, sparks drift, and the leaf sprig bobs |
| `sounds` | `low` | the game's select and back sounds: off, low (`DEFAULT_LEVEL` of `launcher/ui/sound.py`), medium, high |
| `feed_repo` | empty | `owner/name` overriding `feed.json`, for testing another repository's releases |
| `github_token` | empty | only for a private release repository; stored as plain text |
| `last_seen` | empty | the newest version the player was told about |
| `rolled_back` | empty | the last version that failed to start; it is never downloaded or installed by itself |
| `telemetry` | empty | session reports (`launcher/telemetry.py`, [telemetry.md](../telemetry.md)): empty not asked yet, `on` or `off`; nothing is sent unless `on` |
| `telemetry_closed` | empty | the launcher version the server told it takes no more reports (HTTP 410) |
| `telemetry_asked` | empty | the launcher version whose question about session reports was answered or closed |
| `telemetry_notice` | empty | `1`: reports were turned on by the question's countdown or by closing it; told once |

- `UNLOGGED` are the keys whose changes `Settings.save()` does not log: bookkeeping of the update check rather than a
  choice of the player.
- `SECRET` are the keys whose values `Settings.save()` never writes into the log, only whether they are set: players
  attach the log to public issues. `_shown()` gives a setting's value as it goes into the log.

### `Settings`

- `log`: `Settings.save()` writes a line there for each setting that changed since the last save.
- `unreadable`: the file is there but is no JSON; a copy of it is kept as `launcher.json.bad`. That is a typo made by
  hand, and the copy keeps what the player wrote before the next save replaces it with the defaults.
- `saved`: what `launcher.json` holds, as far as this process knows.
- The file is read as `utf-8-sig`: the docs ask players to edit the file by hand, and an editor may save it with a
  byte order mark.

### `Settings.changes()`

`setting: <key> <before> -> <after>` for each setting that differs from the last save (or from what was read at the
start), in the order of `DEFAULTS`.

### `Settings.save_hide_while_playing()`

The worker rereads the settings object and changes only `hide_while_playing`, preserving other fields,
including those unknown to its build. It shares the Tk launcher's atomic file replacement. A successful
change records the usual `setting:` line. On a failed write the in-memory preference stays as it was.

### `Settings.reset()`

Default Settings: the choices go back to their defaults; the game folder, the dump and the history stay.

`Settings.save_install_choices()` writes the confirmed dump and game folder together. It rereads
the settings file and preserves unrelated fields, including fields the current worker does not
understand. The replacement is atomic and the launcher logs changed fields through the usual
settings logger.


## `launcher/start.pyw`

What `NX2Saria.exe` runs: `python\pythonw.exe app\<version>\launcher\start.pyw [arguments]`.

The embeddable Python puts only its `._pth` entries on `sys.path`, so the release folder (the parent of `launcher/`)
goes first here. A failure before the window is up is written to `data/logs/crash.log` and ends the process with exit
code 86 (`STARTUP_FAILED`), which tells the stub to go back to the previous version (`launcher/native/nx2saria.c`
`wWinMain()`). The stub waits six seconds for that code (`QUICK_MS`), so the failure is shown in a message box only
when the stub has no version to go back to or no longer waits (`quiet_exit()`): a box holds the exit code back until
the player closes it, and a new version that could not start then stayed the current one.

`STUB_WAITS` is 5.0 s: the stub's wait for this process's exit code is `QUICK_MS`, 6 s (`launcher/native/nx2saria.c`
`start_release()`), and an exit this long after the process was made still reaches it, with a second to spare for the
exit itself.

### `rollback_target()`

The version the stub goes back to when this one ends with `STARTUP_FAILED`: the one `app/previous.txt` names, when it
is another than this one and its folder holds a launcher. Empty when there is none.

### `process_age()`

Seconds since Windows made this process, which is when the stub began to wait for it, from two file times of 100 ns
ticks. Infinite where that cannot be read, so that the failure is then shown rather than left to a stub that may have
stopped waiting.

### `quiet_exit()`

True when a failed start ends without a message box: the stub goes back to the previous version on the exit code
(`rollback_target()`), and that version's home screen says which version did not start. Not when Steam started this
version without the stub (`--play`, the `PLAY_TEXT` of `launcher/steam.py`), and not once the stub has stopped waiting
(`process_age()`): nobody acts on the exit code then, and without the box nothing would be seen.

### `short_paths()`

The text with the account's folders written short (`tools/private_paths.py`), or as it is when that module cannot
load: the failure being reported may be the one that keeps it from loading.

### `crash()`

The failure into `data/logs/crash.log` and, with `box`, into a message box that waits for the player. The account's
folders are written short in both: `crash.log` is attached to public issues.

### `run()`

The script's folder is `sys.path[0]`. It contains the package `launcher/platform/`, whose name would shadow Python's
standard-library `platform` module for third-party packages on Linux (zstandard on Python 3.12, for example), so that
folder is taken off the path first.

## `launcher/steam.py`

Add to Steam (Customize Launcher): the game as a non-Steam shortcut in the player's Steam library, with the port's
leaf icon and the game's title screen and logo as its pictures (`launcher/gameart.py`).

Steam keeps the shortcuts of each account in `<Steam>/userdata/<account id>/config/shortcuts.vdf`, a binary key-value
file: a map is `0x00 name 0x00 ... 0x08`, a string `0x01 name 0x00 value 0x00`, a 32-bit integer
`0x02 name 0x00 value` (`0x07` is a 64-bit one). The file is the map `shortcuts` holding one map per shortcut, named
"0", "1", and so on. Each holds `appid`, `AppName`, `Exe`, `StartDir`, `icon`, `LaunchOptions` and Steam's own fields;
the other shortcuts are read and written back as they are, and a shortcut of ours already there (by its name or its
id) is updated in place. Its pictures go into `config/grid` named by the shortcut's id: `<id>p.png` the cover,
`<id>.png` the wide tile, `<id>_hero.png` the background, `<id>_logo.png` the logo, and the icon file the shortcut
names. The id is CRC-32 of `Exe` and `AppName` as Steam writes them, with the top bit set.

Steam reads the file when it starts and writes it back when it closes, so a running Steam would neither see the new
shortcut nor keep it. Windows asks Steam to close and starts it again; Linux asks the player to close Steam first.

The shortcut starts the launcher's `pythonw.exe` on `data/steam_play.pyw`, which starts this launcher with `--play`:
the game runs from its own folder with its console hidden, and the Python process waits for it, so Steam shows the
game as running for as long as it is (the game's own Restart included). Starting `ocarina.exe` directly would open a
console window beside the game, and `NX2Saria.exe` ends a few seconds after it started the launcher.

### `vdf_load()`

A binary VDF as a list of `[type, key, value]`; a map's value is such a list. Raises `SteamError` when malformed.

### `appid()`

The id Steam gives a shortcut: CRC-32 of its `Exe` and `AppName` fields with the top bit set (unsigned).

### `add_shortcut()`

`shortcuts.vdf` (empty bytes for none) with our shortcut added or updated; returns `(new bytes, id)`.

- A shortcut already there keeps its `appid`, which keeps Steam's per-app playtime, collections and artwork.
- The fields are sorted in Steam's order: `appid` and the strings first, then the switches and `tags`.

### `find_steam()`, `steam_roots()` and `is_flatpak_root()`

`find_steam()` gives `(Steam's folder, executable)` or `None`; on Linux it checks the native and the Flatpak data
roots. `steam_roots()` gives the Linux Steam data roots that exist, for native and Flatpak installs.
`is_flatpak_root()` says whether a root is inside the Steam Flatpak's private data folder
(`.var/app/com.valvesoftware.Steam`).

### `accounts()`

The config folders of the accounts that signed in to Steam on this PC (`config/loginusers.vdf`), else of every account
folder under `userdata`.

### `added()`

True when an account of this PC has our shortcut.

### `steam_running()`

On Linux it looks in procfs for the native launcher script, Steam's executable, its web helper or the Flatpak client.

### `close_steam()`

Asks Steam to close and waits for it; false when it is still running after `timeout` seconds. Once Steam is gone it
waits 2 s more: Steam writes its files on the way out.

### `add()`

Adds the game to Steam (or updates its shortcut under the port's earlier name): the pictures from the player's game,
the sprig icon, the shortcut in every account's `shortcuts.vdf`, Steam closed first and started again. Returns the
sentence the launcher shows.

- A checkout runs under its own interpreter, so `pythonw.exe` is looked for beside it when the launcher's Python
  folder has none.
- Steam comes back also when a file could not be written (the restart is in a `finally`), started detached
  (`DETACHED_PROCESS`, `0x00000008`).

### `_add_linux()`

Writes one Linux non-Steam shortcut per account, with Steam closed, and a backup before each atomic update.

## `launcher/telemetry.py`

Session reports and comments ([telemetry.md](../telemetry.md)): what the launcher sends to the project's own server,
and when. The window asks about session reports at the first start of each version while they are not on
(`Reporter.question_due()`, `Reporter.answer_question()`): Don't Send keeps them off; Send, closing the question and
its countdown turn them on.

- A session report is the summary the game writes when it ends (`logs/session-summary.json` in the game folder), sent
  with the launcher's version. Nothing is sent unless the settings' `telemetry` is `on`: `Reporter.prepare_session()`
  refuses everything else itself, so no caller can send without it.
- A comment is text the player wrote in Send a Comment, with the last session's summary and, when asked for, the last
  lines of the game's log. Its own Send button is the player's consent: it does not need `telemetry` on.
- Neither carries an id of the installation, and the server keeps no IP address: nothing sent can be deleted on
  request, because the server cannot tell whose it is. It deletes every report and comment after 180 days.

A run that ended in a hard crash writes no summary; the launcher makes one from the crash's FATAL line
(`synthesize_crash()`), with the hardware and settings of the header the game wrote during that run
(`logs/session-start.json`) and every value it cannot know left null.

The server's address is `TELEMETRY_URL` (the project's own Worker, `telemetry/` in the repository), which
`NX2SARIA_TELEMETRY_URL` replaces (a local server for tests). With no address nothing is sent and the window shows
nothing of session reports. No Tk here: the window (`launcher/ui/app.py`) runs the requests on a thread through
`Tasks`.

| Constant | Value | What it limits |
|---|---|---|
| `TIMEOUT` | 10 | seconds a request may take; a report is never worth a longer wait |
| `QUESTION_SECONDS` | 30 | the question after an update turns reports on after this long without any input |
| `MAX_ERROR_LINES` | 5 | `launcher.log` lines about failed requests in one launcher run |
| `SESSION_LIMIT` | 8 KB | bytes of a session report's body the server takes |
| `FEEDBACK_LIMIT` | 40 KB | bytes of a comment's body the server takes |
| `TEXT_LIMIT` | 4000 | characters of a comment |
| `TAIL_LINES` | 200 | lines of the game's log a comment may carry |
| `TAIL_LIMIT` | 24 KB | bytes of those lines |
| `FATAL_LIMIT` | 300 | characters of the FATAL line in a summary |
| `LOG_TAIL_READ` | 256 KB | bytes read from the end of the game's log for its last lines |

A `Request` has a `kind` of `session` or `feedback`. A `Result` has a `reason` for the player, empty when it is `ok`.

### `server_url()` and `enabled()`

`server_url()` is the server's address without a trailing slash: `NX2SARIA_TELEMETRY_URL` when set, else
`TELEMETRY_URL`. `NX2SARIA_TELEMETRY_URL=none` is no server at all: nothing is asked, shown or sent. `enabled()` says
whether this launcher has a server to send to. Without one the window shows nothing of session reports or comments: no
question, no Session Reports screen, so no build without a server asks what it cannot do.

### `empty_summary()` and `example_summary()`

`empty_summary()` is a summary with every leaf null but the ones the format requires
([telemetry.md](../telemetry.md)). `example_summary()` is the summary *See What Is Sent* shows before the game has
written one: made-up values of a plausible PC.

### `printable()`

The text without control characters (those in `keep` stay): the server refuses them in a report.

### `pending_summary()`, `last_summary()` and `mark_sent()`

- `pending_summary()`: the summary of `logs/session-summary.json`, which nothing has sent or declined yet, or `None`.
- `last_summary()`: the last session's summary for a comment: the pending one, else the one sent or declined last.
  `None` when the player turned session reports off (`telemetry` is `off`): a comment never carries a report they said
  no to.
- `mark_sent()`: the pending summary becomes `session-summary.sent.json` (replacing the one before it): sent, refused
  by the server, or declined by the player. It is not offered again.

### `log_started()`

When the current game log was begun, as near as the files tell: the game moves the log before it to `nx2saria.1.log`
when it starts (`runtime/main.c` `log_open()`), so that file's last write came before this run. 0 when there is no
such file (the first run).

### `_covered()`

Whether a summary (pending or already sent) was written during the run whose log began at `since`.

### `log_version()`

`(the port's version, the game's version)` of the game log's `version:` line (`runtime/main.c` `main_run()`), or
`None`.

### `_log_has()`

Whether the game's current log holds a text, read in pieces: the log may be 64 MiB.

### `_fits()`

Whether a value has the type of the example summary's value of its field: null always does, and a whole number does
where a decimal is expected.

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

`session_header()` returns `(the header, '')` or `(None, why there is none)`. `_take_header()` copies the header's
values of `HEADER_KEYS` into the summary.

`synthesize_crash()` makes the summary when the game's log holds a FATAL line of its last run (`launcher/build.py`
`last_fatal()`) and no summary was written during that run (`runtime/main.c` `crash_handler()` exits with 71 after its
FATAL lines): `logs/session-summary.json` gets `session.end` `crash`, the FATAL line with the account's folders
written short and cut to 300 characters, the versions of the log's `version:` line and this PC's `os.family`. Nothing
is taken from an earlier summary, and without a version line nothing is made. It returns the summary made, or `None`.
It must run before the launcher adds its own line to the log (`launcher/game_session.py` `note_exit_in_log()`), which
ends the search of `last_fatal()`.

### `log_tail()`

The last lines of the game's log for a comment: at most `lines` lines and `limit` bytes (UTF-8), the last ones kept,
the account's folders written short. `None` when there is no log.

### `payload()` and `body()`

`payload()` is the session report exactly as it is sent: what *See What Is Sent* shows and `Reporter.send_session()`
posts. It carries no id of the installation: nothing in it tells two reports of one PC from those of two PCs alike.
`body()` gives the bytes of a request's JSON body; *See What Is Sent* shows these same bytes as text.

### `forget_install_id()`

Removes the installation id an earlier version of the launcher made and kept in the settings file. Reports and
comments carry none, so the launcher keeps none. The settings no longer know the key (the `DEFAULTS` of
`launcher/settings.py`), so it is looked for in the file itself, and saving writes the file without it.

### `clean_text()`

A comment as it is sent: line ends as `\n`, no other control characters but tabs, no blank lines around it.

### `Reporter`

Sends for one launcher run. Each of `Reporter.send_session()` and `Reporter.send_feedback()` is a `prepare_*` (the
checks and the request, on the caller's thread), `Reporter.post()` (the network, which the window runs on a thread)
and a `finish_*` (the settings and the files, on the caller's thread again).

- `Reporter.error()`: a failed request's line for `launcher.log`; `MAX_ERROR_LINES` in one run, then silence.
- `Reporter.closed()`: whether the server said it takes no more reports (410) from this launcher version.
- `Reporter.post()`: sends the request and returns `(HTTP status, None)`, or `(None, why)` when no answer came. It
  never raises: no connection, a timeout, a refused address are all the same to the player.
- `Reporter._answer()`: the answer as `launcher.log` names it: the HTTP status, or why none came.

### `Reporter.prepare_session()` and `Reporter.finish_session()`

`Reporter.prepare_session()` gives the request of the pending session report, or a `Result` saying why there is none.
The consent check lives here, not only in the window: nothing is sent unless the settings' `telemetry` is `on`.

`Reporter.finish_session()` marks the summary as sent also for an answer below 500 other than 429: the server will
refuse it again, and it is not offered on every start.

### `Reporter.prepare_feedback()`

The request of a comment, or a `Result` saying why it cannot go. Sending a comment is its own consent: the player
wrote it and pressed Send, so it does not need session reports on; it goes with the last session's summary (when there
is one) and, with `attach_log`, the last lines of the game's log. Escaped characters can make the log lines grow, so a
body over `FEEDBACK_LIMIT` loses the oldest quarter of those lines until it fits.

### `Reporter.set_consent()`

Session reports switched in Settings. Switching them off there counts as this version's answer: the question comes
again with the next version (`Reporter.question_due()`), not at the next start.

### `Reporter.question_due()`, `Reporter.answer_question()` and `Reporter.take_notice()`

The question after an update (the consent screen of `launcher/ui/app.py`).

- `Reporter.question_due()`: whether this start asks about session reports: there is a server, reports are not on,
  and this launcher version has not asked yet. Reports that are on are never asked about; whoever answered Don't Send,
  switched them off in Settings or never answered is asked once by each new version.
- `Reporter.answer_question()`: the question's end. `send` and `don't send` are the two buttons; `closed` (the
  window's close, Alt+F4, Esc) and `timeout` (`QUESTION_SECONDS` without any input) turn reports on too, and are told
  once at the next start. Only Don't Send keeps them off. True when reports are on now.
- `Reporter.take_notice()`: true once after reports were turned on by the countdown or by closing the question, if
  they are still on: the next start tells it.

### `Tasks`

Requests run on threads and finished on the window's: `Tasks.start()` runs `work()` on a thread, and `Tasks.poll()`,
called by the window's timer (`launcher/ui/app.py` `App.tick()`), calls `done(result)` for each that ended, as
`launcher/jobs.py` `BackgroundUpdate.poll()` hands the window a staging's end. `Reporter.post()` never raises;
anything else that does must not lose `done()`, so it becomes a "no connection" result. `Tasks.wait()` is for tests:
it polls until no request runs.

## `launcher/toolchain.py`

The build tools, installed on the player's PC: MSYS2's UCRT64 clang and the libraries `tools/make_game.py` links (the
packages `README-BuildKit.md` lists), and the zbic Python package ([launcher.md](../launcher.md#installing)).

No MSYS2 shell and no pacman: the packages and everything they depend on are read from the repository's database,
downloaded, checked against the sha256 the database (or the release's lock) names, and unpacked into
`toolchain/msys64/ucrt64` as pacman would lay them out. A release pins the exact package files in
`toolchain.lock.json` (`make_lock()`, written by `tools/make_launcher.py`), so every player builds with the compiler
the release was tested with; the repository keeps old package files. A checkout without a lock resolves the newest
packages instead.

Beside it goes the clang of MSYS2's CLANG64 environment (`toolchain/msys64/clang64`), the same LLVM release built with
profile-guided optimisation: it compiles the lifted code about a fifth faster into the same objects, reading UCRT64's
headers (`tools/make_game.py` `fast_clang()`). Only `clang.exe`, the DLLs it loads and its own headers are unpacked.

zbic (GPL-2.0) unpacks the game's compressed program files. The release does not carry it: the launcher downloads the
wheel for its own Python from PyPI and unpacks it into `pydeps/`, as `pip install zbic` would.

- `ROOTS` has `lld` because the game links several times faster with it than with GNU ld.
- The CLANG64 package list is exactly the packages `clang.exe` loads DLLs from, not the dependency closure, which adds
  the linker, the LLVM tools and the CLANG64 C runtime the lifted code does not use. They come from repo.msys2.org
  (`FAST_REPO`), with mirror.msys2.org as the second of `FAST_MIRRORS`.

### `zstd_missing()`

Why this Python cannot read zstd files, or empty when it can. The MSYS2 packages and the game's files are zstd: Python
3.14 reads them with `compression.zstd`, an older Python needs the zstandard package. The release's Python has both;
a checkout runs under whatever Python started it, and without either the build stopped at its first unpack with
nothing but "No module named 'zstandard'".

### `_open_tar()`

A tarfile over a `.tar.zst` (or plain or gzip tar) file or bytes.

### `parse_db()`

`{name: desc}` for a pacman sync database (`ucrt64.db`): each `desc` is `{FIELD: [lines]}`.

### `resolve()`

Every package the roots need, roots first; names a package provides resolve to the package.

### `make_lock()`

The lock a release ships: the resolved package files of the repository's current database. With `fast_repo`
(`FAST_REPO`), also CLANG64's clang, when that repository has the same clang version as UCRT64's.

### `load_lock()`

The release's `toolchain.lock.json`, or a lock resolved from the live database in a checkout.

### `_wanted()`

Whether a package member is unpacked: UCRT64's files without the documentation; of CLANG64's, `clang.exe`, the DLLs
and clang's own headers.

### `_members()`

The members of a `.pkg.tar.zst` (or plain tar), read as a stream, member by member, so a 350 MB package never sits in
memory whole. Yields `(tarfile, member)`.

### `_extract_package()`

Unpacks one package into `dest` (the `msys64` folder): the members `_wanted()` keeps. A wanted hard link needs its
target on disk, and a stream cannot go back for it: a selective unpack (`env`) first reads the headers for the targets
of the wanted links (CLANG64's `clang.exe` is one) and unpacks them too.

### `install()`

Downloads and unpacks the lock's packages into `toolchain/msys64` (through `msys64.new`, swapped in at the end).
`progress(phase, done, total)` is called with the phase `download` (bytes) or `unpack` (packages).

- Room for the downloads and for the unpacked tools beside the ones in use (`msys64.new`) is asked before the first
  download: a disk that fills while a package unpacks otherwise ends the step with an error that names no cause. A
  package's installed size is in the lock; of a CLANG64 package only a few files are unpacked (`_wanted()`), for which
  its download size stands in.
- `NoSpace` of `launcher/net.py` ends the step at once: another server has the same file for the same full disk.
- Files kept from an earlier download are checked against their sha256 too.

### `wheel_tag()`

The wheel platform tags this interpreter loads, best first.

### `zbic_ok()`

Whether zbic loads from `pydeps/` in the interpreter the build runs with. It asks a fresh process, because this one may
have cached that `pydeps/` did not exist; on Windows the process is started with `CREATE_NO_WINDOW` (`0x08000000`).
The probe closes stdin to avoid inheriting the worker's protocol pipe.

### `ensure_zbic()`

A member of the wheel is refused when it would land outside `pydeps/`; the check is on the resolved path, so a sibling
such as `pydeps2` does not pass either.

## `launcher/ui/__init__.py`

The launcher's window: `launcher/ui/art.py` draws the sprites, `launcher/ui/app.py` lays out the screens and handles
input.

## `launcher/ui/app.py`

The launcher's window, laid out like the game's options menu ([launcher.md](../launcher.md#look)): a wooden title band
with the leaf sprig and the port's name in the game's Cetra Display across the whole top (the window's own title bar:
drag it to move the window, minimise and close at its right end), a list of rows on the left (a bronze label cell and
a dark value cell each), a white glowing cursor on the selected row, filigree dividers between groups and a
description box on the right that explains the selected row.

The window is one Tk canvas at 1000 x 640 logical pixels, scaled to the display. The pictures come from
`launcher/ui/art.py`; text, flat cells and the orange choices are canvas items. Screens:

| Screen | What it holds |
|---|---|
| `first_run` | the dump, the install folder, three choices and Begin Installation |
| `home` | Play, Updates, the game folder, Saves, Rebuild the Game, Customize Launcher, Settings |
| `customize` | the colour scheme, the player's own background picture, animations and sounds; Add to Steam |
| `color` | the Custom scheme's colour picker: a colour square, a hue bar and the hex code |
| `saves` | Export Save (a zip for a bug report or another player), Import a Save (such a zip), a save folder |
| `job` | the steps of an install, update or rebuild, with their progress, and Cancel; after a failure Try Again and Copy Report |
| `settings` | the launcher's choices, the folders, the logs and Uninstall |
| `reports` | session reports and comments (`launcher/telemetry.py`): Session Reports, See What Is Sent, Send a Comment |
| `consent` | the question about session reports, at the first start of each new version while they are not on: Send (focused), Don't Send, See What Is Sent, and a countdown that turns them on |
| `sent_view` | the exact JSON a session report sends, in a read-only text box in the description's place |
| `comment` | Send a Comment: a text box for the comment, Attach the Last Log Lines, Send and Cancel |
| `uninstall` | what to remove, then Uninstall or Cancel |

`DEPTH` also knows the screens `mods`, `mod_order`, `udmabuf` and `crash_offer`.

Mouse, keyboard (arrows, Enter or Space, Esc, F5) and an XInput gamepad (d-pad or stick, A, B, Y) all move the cursor.

The look is the player's (Customize Launcher): `launcher/ui/art.py` `set_theme()` fills the palette every picture is
drawn from, a picture of the player's own can replace the backdrop (`launcher/ui/picture.py`), and unless Animations
is off the cursor glides between rows, a new screen slides in, sparks drift over the backdrop and the sprig bobs in
the title band (`App.animate()`). `launcher/ui/sound.py` plays the game's own decide and cancel sounds, read from the
player's game, for a choice and for going back.

The import of `FATAL_TAIL` from `launcher/build.py` is for the tests; its `# noqa: F401` line stays.

### The layout constants

All in logical pixels unless said otherwise.

| Constant | Value | What it is |
|---|---|---|
| `BAND` | (0, 1000, 56) | x0, x1, height: the whole top of the window, which is also its title bar |
| `PIC_TOP` | 60 | the player's background picture starts under the band and its two lines |
| `TITLE_BUTTONS` | min 908 to 954, close 954 to 1000 | x ranges on the band |
| `PANEL` | (112, 60, 580) | x0, top, x1 |
| `ROWS` | (140, 552, 96) | x0, x1, top |
| `LABEL_W` | 222 | 54 % of the row |
| `DESC` | (614, 336) | x, width |
| `SPARKS` | 22 | the dust drifting over the backdrop |
| `MODS_PAGE` | 7 | mods on a page of the Mods screen, beside its other rows |
| `ORDER_PAGE` | 10 | mods on a page of the Load Order screen |
| `PICKER_BOX` | (614, 100, 336, 452) | the box of the Custom scheme's picker (`App.draw_picker()`): x, top, width, height |
| `SQUARE` | (667, 160, 897, 390) | the picker's colour square: x0, y0, x1, y1 (the hue bar has the same form) |
| `DIMS` | 0, 20, 40, 60, 80 | Background Dimming, in percent |
| `SENT_BOX`, `COMMENT_BOX` | | the text boxes of See What Is Sent and Send a Comment, in the description box's place (`PICKER_BOX`): x0, y0, x1, y1 |

- `P` is the colour scheme in use, the `PALETTE` of `launcher/ui/art.py`, which `set_theme()` changes in place.
- `FRAME_MS` is the time between two steps of the animations, 15 ms; they move by the time that really passed. On X11,
  Tk blends every picture with transparency in software and reads the screen back to do it, so each step costs far
  more than on Windows: the animations step half as often there (33 ms) and look the same.
- `REPORT_VALUES` is the settings' `telemetry` as the Session Reports row shows it.
- `FORCED_CHECK_PAUSE` (60) is the seconds after an update check before the Updates row or F5 starts another. Each
  check is a request to GitHub, which allows 60 an hour for an address: a player who selected the row 25 times in 90
  seconds used the allowance up, and every check of the next hour was refused.
- `UPDATE_TAKES` says how long an update's rebuild takes, for the Updates row. No single figure holds: a version that
  changed little compiles a few files and links, minutes; one that changed the lifter translates and compiles the
  game's code again, and one that changed what the shaders are built with builds all of them into the graphics
  driver's cache again (`tools/make_game.py` `warm_driver_cache()`). Players' updates took 24, 34 and 45 minutes where
  the text said "a few minutes".

### `Row`

`parts`, for a row of kind `pair`, is `[(rect, label, callback)]`.

### `measure()` and `fit()`

`measure()` is `font.measure(text)`, remembered: the same labels are measured at every redraw (`App.fit_font()`,
`fit()` and the rows' values), and on Linux each measure is a round trip to the X server's fonts that took 1 to 2 ms.
A Font's name is never reused, so a font made again (`App.reload_fonts()`) is measured again. `fit()` shortens a text
in the middle with an ellipsis so that it fits a width in pixels.

### `App`

The fields with notes:

| Field | What it holds |
|---|---|
| `closed` | why the window was closed, once `App.on_close()` has run |
| `bg` | the update fetched in the background and installed when the game is not running (`launcher/jobs.py` `BackgroundUpdate`); a checkout and the selftest have nothing to switch to |
| `look_message` | why the last background picture was not taken, in Customize Launcher's descriptions |
| `save_message` | the last import's result, shown in the import rows' descriptions |
| `export_message` | the last Export Save result, shown in its description |
| `steam_message` | the last Add to Steam result, shown in its description |
| `mods_message` | what the last change on the Mods screen did, shown in its descriptions |
| `mods_page` | the page of the Mods and Load Order screens |
| `mods_failed` | the mods whose code a build before Play could not take in: Play no longer waits for them |
| `steam_busy` | Add to Steam is making the pictures or writing Steam's files |
| `pick` | the colour picker: `{'slot', 'h', 's', 'v'}` (HSV in 0..1) of the colour being picked |
| `pick_drag` | `square` or `bar` while the mouse drags on the picker |
| `hex_entry` | the box the hex code is typed into |
| `look_after` | the redraw a picked colour waits for (`App.look_soon()`) |
| `game_run` | the Play being followed (`launcher/game_session.py` `GameRun`), until `App.watch_game()` sees its end |
| `reporter`, `tasks` | session reports and comments (`launcher/telemetry.py`): the requests run on threads, polled by `App.tick()` |
| `question` | the question about session reports while it is open: `ask` (its answer counts) or `preview` (`--screen consent` and the selftest: nothing changes) |
| `question_deadline` | `time.monotonic()` at which the open question turns reports on; `None` when stopped |
| `question_left` | the seconds its countdown last drew |
| `sent_return` | the screen See What Is Sent goes back to |
| `sent_text` | the text See What Is Sent shows: the bytes a report sends |
| `sent_example` | that text was made from an example, as the game has written no summary yet |
| `session_sending` | a session report is on its way |
| `comment_draft` | the comment as typed so far |
| `comment_log` | Attach the Last Log Lines |
| `comment_message` | the last Send's result |
| `overlay` | the text box of See What Is Sent or Send a Comment |
| `typing` | the comment's box has the keyboard |
| `sprig_at`, `halo_k` | where `App.step_backdrop()` last put the sprig, and the halo's frame |
| `lit` | the row drawn as the focused one (`App.render()`, `App.refocus()`) |
| `moving` | `App.animate()` is stepping (Animations on, the window shown and in front) |
| `framed` | whether the system's frame is there; on Windows the band is the title bar (`launcher/winsys.py` `frameless()`) |

The look is the colour scheme, the player's background picture at the window's size (`None`: the scheme's own
backdrop) and the sounds (none while the selftest draws the screens). The animations' state is the clock and the last
step's time, the cursor's place on its way to the focused row, how far the screen still stands from its place after a
change, the sparks, the sprig and its halo.

### `App.set_icon()`

The leaf sprig as the window's icon: Tk's photo icon everywhere, and on Windows the `.ico` set on the window itself,
so the taskbar button shows the launcher's icon rather than Python's.

### `App.load_fonts()` and `App.fit_font()`

`App.load_fonts()` takes the game's Cetra Display when this PC has decoded it from the player's game
(`data/cache/fonts`), else the system's interface font. Every text is in Cetra Display; only the build log keeps a
monospaced face so its lines align. `App.fit_font()` gives the font, or the same face a few pixels smaller so that the
text fits the width (down to `smallest` of its size).

### `App.fonts_from_game()`

Decodes the game's fonts and its interface sounds in the background, redraws with the fonts and takes up the sounds.

Before the fonts are decoded again, the cached files are unregistered (`launcher/winsys.py` `unregister_font()`). They
are registered by `App.load_fonts()` and were not usable, or this would not run: registered, Windows keeps them open,
and decoding them again failed with "Permission denied" at every start, so a cache file that had gone bad was never
replaced.

### `App.animated()`

Animations on (Customize Launcher); never while the selftest draws the screens, whose pictures must not depend on the
moment they are taken.

### `App.load_picture()` and `App.see_through()`

`App.load_picture()` loads the player's background at the size of the window below the title band, when one is set
and its kept copy is readable. A copy an older launcher kept for the whole window is made again from the picture when
the file is still there, so no more of it is cut off than the new shape needs. `App.see_through()` says how solid the
panel and the description box are: 0.8, a little see-through, over the player's own picture.

### `App.draw_static()`

- The sparks are made over the backdrop, under the panel and the band.
- A soft green halo behind the sprig pulses while the animations run.
- `sprig_at` and `halo_k` are reset: the sprig is drawn again at its place and first frame, and `App.step_backdrop()`
  starts over.

### `App.make_sparks()`

The dust over the backdrop: `SPARKS` soft dots of three sizes, each with its own speed, sway and twinkle, at places of
their own from the first frame on. None while Animations is off. A spark's numbers are in logical pixels: its speed
upwards a second, how far it sways to each side and how fast.

### `App.apply_look()` and `App.draw_title_buttons()`

`App.apply_look()` draws everything again after the colour scheme, the background or Animations changed.
`App.draw_title_buttons()` draws minimise and close at the band's right end, lit when the mouse is over them.

### `App.move()`, `App.side()` and `App.back()`

- On `sent_view`, up and down scroll the JSON; Back is the only row.
- On a switch, left is On and right is Off. A switch or a choice that changed plays the select sound; a `pair` row
  does not.
- On the comment screen, B or Esc first leaves the comment's box.
- On `consent`, Esc or B closes the question, which turns reports on as the screen says.

`App.home_row()` gives the index of the home screen's row with a label, for coming back to it.

### `App.on_click()` and `App.on_drag()`

The band moves the window, like a title bar. Where the system does not help with that (`launcher/winsys.py`
`start_drag()` returning false, on Linux), `App.on_drag()` moves the window itself.

### `App.on_close()`

Closes the window: the × of the title band or of the system's frame, Alt+F4, or a signal that ends the process
(`main()`, `launcher/errors.py` `on_end_signals()`), whose handler ran at `time.monotonic()` `signalled`. The line it
logs tells a closed window from one that died, whose log ends without it, and for a signal how long the close took.

- The player closed the window over the open question: reports turn on, as the screen says. A signal that ends the
  process is no answer: the next start asks again.
- A background staging under way is cancelled: what it downloaded is resumed, and its unfinished folder replaced,
  next time.

### `App.start()`

- After the stub went back from a version, that version goes into the setting `rolled_back`: it is never installed
  again by itself.
- A missing zstd module is told at once, not when Begin Installation stops at the build tools.
- `launcher/jobs.py` `interrupted()` is asked before any job of this start leaves its own marker.
- `python.old*` and `python.failed*` in the launcher's folder are removed: the Python an update replaced
  (`launcher/native/nx2saria.c`).
- After a rollback the stub passes the failed version's `--continue-update` on to this one: the game was never rebuilt
  for the new version, so there is nothing to finish, and home tells why the version is back.
- A game whose last update stopped in the compile also counts as installed (`launcher/build.py` `installed()`): the
  start goes to home, where Play starts the build from before the update and Rebuild the Game finishes it, not to the
  first-run screen and its dump.
- When the launcher was stopped in a job, the message says so. `tools/make_game.py` keeps the old executable until the
  new one is linked and records each finished step, so the game folder is playable and a rebuild goes on from there;
  the player is told that it is due.
- `--play` with a built game reaches here only for Play from Steam with a mod's code to build in first
  (`play_only()`).

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

Session reports on the first home screen of an installed game in a run of the launcher (never over the first run's
screen or a job's): a summary left by a run this launcher did not watch (Steam, a shortcut, a crash) goes if reports
are on; reports the question's countdown or its closing turned on are told once; and the first start of a new version
asks while they are not on (`launcher/telemetry.py` `Reporter.question_due()`).

With `after` set (the home screen came from a job's end) the question opens without its countdown
(`App.open_question()` with `countdown=False`): a build takes minutes and the player may not be at the PC, and a
countdown that ran out then would turn reports on for someone who never saw the question. The question waits instead;
its first line is the one of a stopped countdown. The log says `telemetry: the question opens after <job>, without its
countdown`, and `App.start()` says `telemetry: the question waits for the update to finish` when it starts the update's
job with a question due.

The notice of reports turned on (`launcher/telemetry.py` `Reporter.take_notice()`) is taken only when no message is on
screen. After a job the home screen always carries the job's message ("NX2Saria is now ..."), and taking the notice
there would clear it and log it as told without showing it; it stays for the next start with a free message line.

### `App.open_screen()`

`--screen`: one of the session reports' screens at once, for looking at it or taking its picture
([launcher.md](../launcher.md)). Nothing is sent by opening one. Without a server (`launcher/telemetry.py`
`enabled()`) none opens.

### `App.show()`

Another screen slides into place: from the right when it lies deeper (`DEPTH`), from the left on the way back.

### `App.log_first_run()`

The first-run screen's folder and dump into `launcher.log`: the line that tells, in a log that ends here, that the
window was up and what it offered.

### `App.rows_first_run()` and `App.begin_install()`

- The free space shown counts the partial image (`partial_image_gb()`), because Begin Installation removes it.
- `App.begin_install()` tells a path too long for Windows there (`launcher/paths.py` `long_path_problem()`), not as a
  missing file halfway through the build, and removes the partial image before the free space is measured.

### `App.discord_row()`

The game's `discord_rpc` setting (`runtime/discord_rpc.c`), kept in `nx2saria.ini` or an existing `nx2navi.ini` as the
game's own menu keeps it; the game reads it when it starts. It is on unless the file says 0, as the game reads it
(`cfg_bool`, default on); a settings file that is there but unreadable shows the game's default.

It is not switched while the game runs, as the mods' switches are not (`App.switch_mod()`): the game writes the same
file when a menu setting changes. Each side reads the file, changes its own lines and puts a new file in place, so of
two changes in the same moment the second drops the first. The game reads this setting only when it starts, and its
own menu switches it meanwhile.

### `App.rows_home()`

An update that stopped in the compile left no finished build on record, but the executable of the build before it: the
Rebuild the Game row then says the rebuild is unfinished.

On Linux, a kernel on which the game cannot track written memory (`Context.write_watch`,
`launcher/platform/linux.py` `write_watch_text()`) puts a Linux Kernel row under Play and the warning first in Play's
text, which shows on the screen the launcher opens on.

### `App.rows_customize()`

- After the scheme changed, the rows are made again: Background Color and Button Color are there only while the scheme
  is Custom. The cursor stays on Color Scheme, the first row.
- A change of the Sounds choice plays the select sound, so the new loudness is heard at once.

### The Custom scheme's colour picker

- `App.pick_step()`: hue by 5 degrees along the hue bar, saturation and brightness by 5 % (the arrows and the
  gamepad).
- `App.pick_changed()`: the picked colour into the settings and onto the screen: the picker at once, the launcher's
  colours a moment later (`App.look_soon()`), so dragging over the square stays smooth. `final`: saved to
  `launcher.json`.
- `App.picker_at()`: the colour under the mouse at (`x`, `y`) on the square or the bar into the picker; it returns
  which of the two it was (`part`: the one being dragged, which takes the mouse also outside it), or `None`.
- `App.type_hex()`: a box over the Hex Code row to type a colour into, like `#3A7BD5`; Enter takes it, Esc leaves it.
- `App.parse_hex()`: `(r, g, b)` in 0..255 of a typed hex code (`#3A7BD5`, `3A7BD5` or `#37D`), or `None`.
- `App.draw_picker()`: the colour picker in the description box's place: the slot's name, the colour square of the
  picked hue with a ring on the picked colour, the hue bar, and the colour itself with its hex code.

### `App.steam_row()` and `App.add_to_steam()`

Add to Steam: the game in the Steam library, started from there without the launcher (`launcher/steam.py`). Any
exception of the work is caught and logged with its traceback: a Steam folder in a state nobody expected must say so
instead of ending the launcher.

### Keyboard and controller in the Electron window

Chromium retains the native keyboard sequence. The controller uses the standard
mapping. Its directions move among visible, enabled controls in the active
screen; left and right change a selected dropdown, and the right stick scrolls.
A activates the focused control. B returns from its panel or takes the active
dialog's cancel route. Losing focus pauses controller input until it returns to
neutral. The renderer logs first use without including a hardware identifier.

### Electron interface language

The Electron main process resolves the first supported system locale, or
English. `launcher/worker_preferences.py` `language()` reads the installed
override and `set_language()` atomically changes its locale field. The worker
requires the installation lock before changing it. Unknown fields are retained
and the Tk settings and game language are independent.

`launcher-electron/src/locale.ts` maps system tags and formats named values.
`launcher-electron/src/messages.ts` holds the four translations. Use the source
label as the catalog key. Mark static elements with `data-message`, passing
dynamic messages through the active catalog. Interpolate runtime values after
choosing the translation. File names, paths, raw diagnostics and release notes
keep their original form. A failed preference write keeps the prior selection.
The log records the canonical locale choice and outcome.

### Electron appearance preferences

The appearance view uses the existing palette, custom slots, dimming steps and
sound levels. The worker validates a choice and writes one key without
rewriting unknown configuration fields. A reset clears only the relevant
color override. The Electron main process validates every swatch before the
window applies its theme variables. The animation preference controls the
window transitions and the system reduced-motion preference takes precedence.

The picker writes a normalized copy through `picture.keep()`. Replacing the
shared background cache keeps a temporary copy of the previous image to restore
if the settings write fails. Removing the background affects only the retained
cache. The renderer sees a scaled PNG in bounded frames. The picker supplies the
original path directly to the worker, and the Electron log keeps only outcomes.

The worker can fill the existing title-font and effect caches using
`gamefiles.extract_fonts()` and `extract_sounds()`. It publishes only fixed
asset names and sends the cached bytes in bounded frames. The window registers
a valid title font with Chromium and plays the select and back effects at the
same gain as Tk. The default font and silence cover absent or invalid assets.

### Steam in Electron

The worker uses the same Steam detection, binary VDF and art writer as Tk.
The window reads availability and starts an explicit Add or Update request.
One background task owns the request until the existing writer finishes or
reports an error. Further requests observe the running task. The worker keeps
its installation lock until an accepted task finishes, even when the window
closes first. The existing headless shortcut still uses the installed wrapper
on Linux or the installed Python script on Windows. The Electron log records
detection and results without Steam account identifiers.

### Electron Session Reports

`Reports` in `launcher/worker_reports.py` uses the existing `Reporter` and
`Tasks`. Its question is started when the renderer first presents a ready home
screen. The worker owns the deadline, stops it on input and persists an answer
through the same Python settings logic as Tk. Electron acknowledges a deliberate
window close before closing the worker's input. Pipe loss alone leaves the
question unanswered.

The private build uses `NX2SARIA_ELECTRON_TEST_SEND_REPORTS` to gate all
submission. With it unset, an enabled session report is left for Tk and the
worker logs that decision once. With it set to `1`, prepared session requests
retain their exact bytes. The worker compares the pending summary's file identity
before acknowledging delivery, and leaves a newer pending summary available
for its own request. A worker closing normally drains report tasks within its
shutdown budget. No transport response changes the consent logic in `Reporter`.

Preview returns an immutable byte snapshot in bounded base64 frames. The main
process assembles the frames and decodes UTF-8 before the window writes them as
text. Session reports use the pending summary or the existing example. A
comment goes through `Reporter.prepare_feedback()` and `finish_feedback()`.
Only the outcome, settings choice and attachment flag go to the Electron log.

The crash-log offer uses the active installation's `crash_offer.py` when present.
It loads the same pending marker and request, keeps the request for exact preview,
and revalidates the marker before consuming it. The worker enforces the
one-second guard for navigation and activation. Deliberate window close follows
the existing close path. The test switch gates the send action, while declining
consumes the marker and writing a comment selects the log attachment. The
worker uses the existing HTTP finish policy, including stopping further offers
after an HTTP 400 refusal. The Electron log records decisions without payloads.

### Electron settings for an installed game

The Settings panel reads the configured game folder, dump, update channel, update
preferences and build-cache preference. The low-commit warning is only editable on
Windows. The main process receives the registered dump path for the native file
picker and passes only its filename to the renderer. The worker checks the
replacement with the first-run dump validator and writes only the dump key.

Each write uses `Settings.save_preference()` to preserve unknown keys and is refused
during an owned game, a job, recovery or failure to acquire the launcher lock.
The Windows memory check and the jobs read the same in-memory preferences once a
write succeeds. The update settings are stored for the installed launcher. The
manual check reads the selected channel. Opening the game or log folder accepts a
named target, resolved in Python with the existing desktop opener. This is
available in read-only Settings during game or job recovery.

### Electron Mods

The worker reads the ordered list with `mods.scan()` and the same conflict winners as Tk.
It sends six rows at a time and up to three conflicts per row. Listing leaves the filesystem
alone. Opening the folder explicitly uses `ensure_mods_dir()` and the platform opener.
Addition uses `mods.add()`. Changing a usable mod's enabled flag or moving a row checks
for both an owned game session and an externally started game. The worker blocks mutations
during a job or unresolved recovery. Move returns the selected row's new page so it remains
visible across page boundaries.

The Electron main process validates the response fields and accepts files and folders through
its native picker. The renderer reads a fresh ordered page after each successful change.
Code waiting to be built goes through the same Play job and receipt as Tk. The two launcher
logs record the action and result. Python errors are shortened before being sent to the window.

### The Saves screen

- `App.save_waiting()`: a save put into the game folder's saves folder (a shared zip, a save folder), waiting to be
  imported.
- `App.saves_row()`: the row of the screen that exports and imports saves; orange while a save waits in the saves
  folder.
- `App.export_save()`: an export with no save files comes from a game that stopped before its first save, and the
  message says that the report files alone were exported.
- `App.import_saves()`: Import a Save (`folder` false) takes what waits in the saves folder, else a zip the player
  picks. Import a Save Folder (`folder` true) takes a folder the player picks; the saves folder itself takes what
  waits there.
- `App.imported_text()`: what an import did, for the import rows' descriptions.

### `App.mod_list()` and `App.mods_row()`

`App.mod_list()` gives the game folder's mods in the game's load order (empty before the game is installed or when the
folder cannot be read). `App.mods_row()` is the row of the Mods screen; orange while a mod's code waits to be built
into the game.

### `App.staged_desc()`

The Updates row of a release downloaded in the background (`launcher/jobs.py` `BackgroundUpdate.ready()`).

### `App.check_done()`

It calls `BackgroundUpdate.checked()` of `launcher/jobs.py`: the release found is staged at the next tick, also one
whose staging failed before.

### `App.watch_game()`

Runs once a second while the game Play started runs, its Restart's copies too (`launcher/game_session.py`
`GameRun.running()`); then the window comes back and the end goes into `launcher.log`, into the game's log when that
has no line of it, and on home when there is something to tell (`launcher/game_session.py` `game_end()`). The window
takes the keyboard focus when it comes back: the pad and the animations follow it (`launcher/winsys.py`
`in_front()`).

### `App.start_update()`, `App.install_staged()` and `App.background_event()`

- `App.start_update()`, the Updates row: install the release staged in the background, have the one downloading
  install once it is staged, or else download it here with its steps on screen (as when the background download
  failed).
- `App.install_staged()`: install the release staged in the background when nothing stops it
  (`launcher/jobs.py` `BackgroundUpdate.install()`) and close for the restart into it; true when it was installed.
- `App.background_event()`: a background staging started or ended (`launcher/jobs.py` `BackgroundUpdate.poll()`).
  When it failed and the player had chosen to update, the release is downloaded on the job screen instead, where its
  error and Try Again show.

### Session reports and comments

The screens of `launcher/telemetry.py` ([telemetry.md](../telemetry.md)).

- `App.return_focus()`: the row to come back to on a screen: the label's, else home's Play or the reports screen's
  first row.
- `App.after_game_reports()`: after the game ended (`App.watch_game()`) or when the launcher opens (`App.start()`):
  with session reports on, the pending summary goes in the background; else nothing is sent. The question about them
  is asked by the start, not here. It always returns false (no screen opens). Without a server there is no question
  and nothing is made or sent.
- `App.send_pending()`: the pending session report, sent on a thread (`launcher/telemetry.py`
  `Reporter.prepare_session()` refuses it unless reports are on).
- `App.open_sent()`: See What Is Sent: the bytes the pending report would send (`body()` of `payload()` in
  `launcher/telemetry.py`), or an example's when the game has written none.
- `App.send_comment()`: Send on the comment screen. Pressing it is the player's consent for this comment: it goes also
  with session reports off (`launcher/telemetry.py` `Reporter.prepare_feedback()`).

### `App.open_question()`

The question about session reports ([telemetry.md](../telemetry.md#when-the-launcher-asks)). The start opens it at the
first start of a launcher version while reports are not on. Send is focused. It ends with Send or Don't Send; closing
it (Esc, B, the window's close) and `QUESTION_SECONDS` without any input turn reports on as well, which the screen
says in its first line, with the seconds left. Any input stops the countdown for good: a key, a button, the mouse
moving over the window, opening See What Is Sent.

- `App.question_touched()`: any input while the question is open; the countdown stops and does not start again.
- `App.question_seconds()`: the whole seconds left on the open question's countdown; `None` when it is stopped.
- `App.question_tick()`: from `App.tick()`; the countdown's end turns reports on, and each new second is drawn.
- `App.set_waiting_aside()`: the summary that waited when the question was answered is never sent: the first report is
  the one of the next session played.
- `App.question_end()`: the question's end: `send`, `don't send`, `closed` or `timeout` (`launcher/telemetry.py`
  `Reporter.answer_question()`). A preview changes nothing. After `closed` and `timeout` the notice that reports are on
  is shown now, and once more at the next start (`App.start_reports()`).

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

### `App.open_overlay()`

The text boxes are one Tk `Text` over the description box's place: that of `sent_view` (read-only, the JSON) or of
`comment` (the comment being written). The window's own keys (arrows, Enter, Space, Esc) are not the box's: the
window's tag is left out of the box's binding tags, so they reach only the box while it has the keyboard.

- `App.comment_changed()`: each change of the comment: cut to `TEXT_LIMIT` characters, the count on its row and under
  the box.
- `App.set_typing()`: the cursor goes to Your Comment while the box has the keyboard.
- `App.side_box()`: the description box's frame in its place (`PICKER_BOX`), for the screens that put a text box in
  it.

### The job screen

- `App.rows_job()` reads the steps under the job's lock (`launcher/jobs.py` `Job.locked()`): each step as one change
  of the worker left it, not a state with the value before it.
- `App.job_focus()`: the cursor sits on the running step, or on Try Again after a failure.
- `App.copy_report()` writes the account's folders short (`tools/private_paths.py`): a report is pasted into public
  issues.
- `App.job_finished()`: a job with `after` set to `play` built the mods' code in (`App.play()`), and the game starts.
  The job's `note` is what the file check found (`launcher/jobs.py` `Job.note_scan()`).

### `App.tick()` and `App.minute()`

A drawing error must not stop either timer: it is logged, and the timer is set again. `App.minute()` runs once a
minute, also while the window is hidden during play: it looks for a new version an hour after the last look (a new
one then downloads in the background), and installs a staged release that waited for a copy of the game the launcher
did not start to close.

### `App.animate()`

One step of everything that moves: the cursor on its way to the focused row and its breath, the screen sliding into
place, the sparks and the sprig. Each moves by the time since the last step (the timer comes when Tk gets to it, 16 to
50 ms apart), at most a tenth of a second's worth. With Animations off, while the window is hidden or minimised, and
while another window is in front, nothing moves and the timer runs slowly (300 ms). The last covers a launcher left
open behind the game (Hide Launcher While Playing off): drawing every 15 ms took about 0.6 of a CPU core from the game
for a window nobody saw. A drawing error must not stop the timer.

- `App.step_slide()`: the slide decays by `exp(-dt * 16)`, most of the way in a fifth of a second.
- `App.step_cursor()`: one breath of the cursor takes 1.6 s; its glide closes by `1 - exp(-dt * 22)`, there in about
  a tenth of a second.

### `App.step_backdrop()`

The sparks, the sprig and its halo, each touched only when it lands on another pixel or picture: they drift less than
a pixel a step, and Tk draws an item again whenever it is moved or changed, also to where it was. On X11 that drawing
blends each picture with what is under it in software, and the sparks moved at every step took two thirds of the
window's processor time.

- A spark goes up, swaying, and comes back in at the bottom once it is under the title band. Its twinkle is one of
  three pictures (0, 1 or 2).
- The sprig rises and sinks two pixels every 2.4 s while its halo pulses.

### `App.render()`

A screen that slides is drawn in place, then put where the slide has got to (`App.step_slide()`).

### `App.draw_row_tagged()` and `App.refocus()`

`App.draw_row_tagged()` draws row `i` with its items tagged `row<i>`, so that `App.refocus()` can draw it again alone.

`App.refocus()`: the focus went from row `old` to the focused one on the same screen, and only those two rows, the
cursor and the description are drawn again. The whole screen was drawn again for each row the mouse crossed, about
55 ms on Linux, where Tk blends each picture with what is under it in software, so the cursor lagged behind the mouse.
The whole screen is still drawn when it slides in, on a job's screen (its rows change as it runs) and with the colour
picker open. The row drawn lit may not be `old`: a click or a cancelled chooser moves the focus without drawing it,
so the row in `lit` is drawn again too.

### `App.fit_rows()`

The height of each row and the gap between rows (logical pixels) that end the rows above `bottom`. A screen with more
rows than the window holds (Settings with every row, a job with every step) narrows its gaps first, then its dividers,
and last makes its rows a little lower.

### `App.draw_row()`

A row of style `swatch` is a colour of the Custom scheme: a patch of it and its hex code. A row with an arrow shows
its value centred in the cell's room left of the arrow.

### `App.draw_cursor()`

The cursor is drawn where it has got to and `App.animate()` takes it on to the row (`App.step_cursor()`); on a new
screen, while one slides in and with Animations off it stands on the row at once. It does not glide while
`App.animate()` stands still (another window in front): the cursor would stay behind on the old row.

### `App.draw_desc()`

- `fine` is paragraphs in the small font: a long explanation under its leading lines.
- The block is measured by drawing it at a provisional top; then everything moves to the final top.
- The top never goes up behind the screen's heading: a block's first lines are the ones to read.

### `single_instance()`

The launcher folder's lock (`launcher/locks.py`) for this window's life. It raises `Busy`, naming the holder, when
another launcher or a command of `launcher/cli.py` still holds it after `wait` seconds (the launcher an update restart
replaces may still be closing).

### `tell()`

A sentence for the player before the window is up: a message box that waits for OK. With no display and no toolkit it
goes to the terminal that started the launcher.

### `play_only()`

`--play`, from the Steam shortcut (`launcher/steam.py`): the game starts from its folder with its console hidden, as
Play starts it, and this process waits until it and the copies its own Restart starts have ended, so Steam shows the
game as running all that time. `None` when there is no built game: the launcher's window opens instead.

- The window or a command may run a job in the game folder meanwhile (a rebuild, an update's install, the mods'
  build): the game started now would be the build that job is replacing, which then fails to replace it, and
  `launcher/mods.py` `prepare()` would unpack into the folder the job reads. Such a job holds the launcher folder's
  lock and leaves the `MARKER` of `launcher/jobs.py`. The lock is only tried, and let go at once: held, it would keep
  the window from opening.
- With a mod's code waiting, it returns `None`: the window builds the mods' code in, then starts the game
  (`App.start()` with `--play`).
- The end is told as Play's is (`App.watch_game()`), with no window: a box for an end worth one. The logs come first,
  since the box waits for OK and Steam may end this process meanwhile.
- The crash summary is made (`launcher/telemetry.py` `synthesize_crash()`) before the note ends the log's run; the
  window sends it when it opens.

### `main()`

- A second window on the same folder would run its own jobs into the same game folder and settings. The first one may
  be out of sight (hidden while the game runs), so the player is told who holds the folder.
- `launcher/errors.py` `install()` takes the window's errors, and `finish()` there undoes it at the end.
- The handler of an end signal does not close the window itself. Python runs it between two steps of whatever the
  window was doing (a redraw, a timer): closing there would destroy the canvas under that code. The window closes
  from the event loop instead (`root.after(0, ...)`), and the exit code is 128 plus the signal's number.
- An idle window would act on the signals only at its next event, hence `launcher/errors.py` `wake_for_signals()`.
- Uninstall, or the selftest, ends the window without `App.on_close()`; the log then gets `window ended on the
  <screen> screen after <n> s`.

## `launcher/ui/art.py`

The window's pictures, drawn with numpy into RGBA arrays and handed to Tk as PNG data.

Everything is drawn here from shapes and colours: the misty green backdrop, the wooden title band, the bronze label
cells with their mottled texture, the description box, the white glowing cursor, the filigree divider, the leaf sprig
and the small icons. Tk's canvas draws lines and polygons without antialiasing on Windows, so every curved or soft
shape is a picture; flat rectangles and text stay canvas items.

Colours follow the game's options menu ([launcher.md](../launcher.md#look)). Sizes are in logical pixels times the
display scale `s`. `PALETTE` holds the colours of the colour scheme in use: `set_theme()` fills it from `THEMES`, each
scheme the first one with its woods, accents, backdrop and glow turned to other hues. The Custom scheme takes three
colours the player picks on a colour square (`color_square()`, `hue_bar()`): one for the backdrop, one for the panels
and the title band, one for the buttons.

- `BASE` is the first colour scheme, Forest: bronze wood on a misty dark green. Its `scene_0` to `scene_3` are the
  backdrop field's four heights, `mist` its mist, `tint_edge`, `tint_side` and `tint_mid` the see-through tint over it
  (edge, side, middle), `vignette` the corners. `wood_0` to `wood_3` are the title band's wood from top to bottom, and
  `band_line_dark` and `band_line_light` the two lines under it. `glow_near` and `glow_far` are the cursor's glow near
  and far from its frame, `spark` the sparks drifting over the backdrop, `halo` the sprig's halo.
- `GROUPS` are the colours a scheme turns together, each group to one hue.
- `THEMES` are the colour schemes of Customize Launcher, in the order the row steps through them. Each group is
  `(hue in degrees or None to keep each colour's own, saturation factor, lightness factor, saturation added)`, applied
  to the first scheme's colours (`recolor()`), so a scheme keeps the first one's light and dark steps. `set` gives
  colours as they are.
- `CUSTOM_SLOTS` is the Custom scheme: each colour the player picks takes the place of one colour of the first scheme
  (`ref`), and the rest of that colour's part of the window follows it, lighter and darker as in the first scheme
  (`follow()`). Each part is `(ref, the colours that follow it, the text on it, which turns dark when the picked colour
  is light)`. The buttons are the rows' label cells, their value cells and the orange choices in them; the panel is the
  list panel, the description box, the dividers and the title band. The colours start as the first scheme's.
- `_KEPT` holds the blurred masks and the noise, which depend on sizes only, not on colours: kept there, a change of
  the colour scheme paints them again in its colours without computing them again (most of the time a redraw took).
  The arrays kept are read, never written. `kept()` empties the store at 160 entries; the row and box sizes of every
  screen stay far below that.
- `FILIGREE_PATHS` is the game's divider ornament, traced from the options menu: two pointed lens ends, S-waves into
  spiral hooks and a diamond knot under the middle, in a 120 x 38 box (centre x 60).

### `recolor()`, `hexcode()` and `follow()`

- `recolor()`: a colour turned to `hue` (degrees; `None` keeps its own), its saturation times `sat` plus `add`, its
  lightness times `light`.
- `hexcode()`: `#rrggbb` of `r`, `g`, `b` in 0..1.
- `follow()`: a colour moved as `ref` moves to `chosen`: the hue of `chosen`, and its lightness and saturation shifted
  the way those of `ref` shift, so `ref` becomes `chosen` exactly and the colours lighter or darker than `ref` stay so.

### `custom_palette()`

The Custom scheme's colours from `{slot: '#rrggbb'}` (`CUSTOM_SLOTS`); a slot left out or not a colour keeps the first
scheme's. Text on a light button or panel turns dark, so it stays readable, and its shadow light.

- A slot set to the first scheme's own colour keeps that scheme's colours as written, without the arithmetic.
- The choices come out lighter than the cells: when the orange of the buttons is brighter than 0.65, the text on it
  turns dark too.

### `theme_palette()` and `set_theme()`

`theme_palette()` gives the colours of a scheme (an unknown name: the first scheme's). `set_theme()` fills `PALETTE`
with them, in place because the window holds a reference, and returns the name in effect. `custom` is the Custom
scheme's colours, `{slot: '#rrggbb'}`.

### `png_bytes()`, `dib_icon()` and `ico_bytes()`

- `png_bytes()`: the PNG of an HxWx4 float array in 0..1 (straight alpha) or of a `uint8` array.
- `dib_icon()`: one `.ico` entry as a 32-bit bitmap with its AND mask (what every Windows API and Tk read), rows
  bottom-up.
- `ico_bytes()`: a Windows `.ico` of square HxWx4 images: bitmaps up to 64 pixels, PNG above (as Windows' own icons
  do).

### `over()`

Composites `src` over `dst` at (`x`, `y`), straight alpha, clipped to `dst`. Onto a solid picture (the backdrop's
layers) it is a plain mix, and the result stays solid. Where both are clear the summed colour is 0 and stays 0 after
the division by the alpha, which is floored at `1e-6`.

### `paint()`

Paints a colour through a coverage mask (HxW, 0..1) onto a picture. With a mask of the picture's size it is `over()`
with one colour for the whole layer, without making the layer.

### The drawing primitives

- `vgradient()`: HxWx3 colours down the height through `(position 0..1, colour)` stops.
- `hgradient_alpha()`: HxW alphas across the width through `(position, alpha)` stops.
- `hgradient_rgba()`: an RGBA layer whose colour and alpha run across the width through `(position, colour, alpha)`
  stops.
- `value_noise()`: smooth noise in 0..1 with features about `period` pixels apart.
- `fractal()`: fractal noise around mid grey, about `contrast` wide (the range of an SVG `feTurbulence` turned grey).
- `overlay()`: the CSS `mix-blend-mode: overlay` of a grey noise (HxW) over colours, at an opacity.
- `box_blur()`: three box blurs of radius `r` over a 2-D array (close to a gaussian).
- `rect_mask()`: the coverage of the rectangle [`x0`, `x1`) x [`y0`, `y1`) with fractional edges.
- `polygon_mask()`: the antialiased coverage of a convex polygon (pixel coordinates), supersampled `ss` x `ss`.
- `stroke_mask()`: the antialiased coverage of polylines (lists of (`x`, `y`)) stroked with round joins and caps.
- `shadow()`: a soft shadow of a mask: blurred, offset, as an RGBA layer of the same size.
- `svg_polylines()`: points along an SVG path made of M, L, C and Z commands (the filigree).
- `hsv_rgb()`: HSV to RGB on arrays (or numbers) in 0..1; an H... x 3 array.

### `backdrop()`

The whole window behind the panels: a dim misty field under a see-through tint of the colour scheme, darker at the
sides. `picture` is the player's own background instead of the field, an array of `H - top` rows and `W` columns in
0..1 put below the title band (`top`: where the band ends), darkened by `dim` (0 as it is, up to 1 the scheme's
darkest colour) so the text over it stays readable.

### `color_square()`, `hue_bar()` and `picker_mark()`

- `color_square()`: the colour square of the Custom scheme's picker for one hue (0..1): saturation from grey on the
  left to the full colour on the right, brightness from black at the bottom to full at the top.
- `hue_bar()`: the picker's hue bar: every hue from red on the left through yellow, green, cyan, blue and magenta to
  red.
- `picker_mark()`: the ring that shows the picked colour on the square and the bar: white with a dark edge inside and
  out.

### The window's pieces

- `spark()`: one spark of the dust drifting over the backdrop: a soft round dot of the scheme's spark colour, `size`
  pixels across, brightest in its middle, which is white.
- `halo_frames()`: the title sprig's light: a soft disc of the scheme's halo colour, `n` frames of one slow pulse.
- `band()`: the wooden title band, its two-line bottom edge and the shadow under it; `(image, extra height below)`.
- `panel()`: the left list panel with the shadow on its right. `alpha` below 1 lets the player's own background show
  through it.
- `label_cell()`: a bronze label cell: gradient, mottled texture, 1 px border and the 1 px dark ring around the row.
- `desc_box()`: with `alpha` below 1 the player's own background shows through the box, not through its border.
- `glow_frames()`: the selection cursor around a `w` x `h` box: a 3 px white frame 6 px outside it with a soft glow of
  the scheme's colour inside and out, `n` frames of one breath. Returns `(frames, margin)`, where the box sits at
  (`margin`, `margin`).
- `filigree()`: the divider: a thin line with a dot at each end and the ornament in the middle; `(image, height)`.
- `sprig_icon()`: an original three-leaf sprig on the forest and gold palette, at the size Windows or Tk requests.
- `sprig_ico()`: the icon of the launcher, the setup and the taskbar at the sizes Windows asks for (16, 20, 24, 32,
  40, 48, 64, 96, 128 and 256).
- `title_glyph()`: the minimise bar or the close cross of the band's buttons, in the band's light text colour.
- `framed_icon()`: the square-framed icon of a big row (play or refresh), like the icon frames of the game's menu rows.
- `badge()`: the orange "!" disc beside Updates when a new version is ready (the "!" itself is text).

## `launcher/ui/picture.py`

The player's own background picture (Customize Launcher): read a picture file into a numpy array, fit it to the shape
of the window below its title band and keep that copy in `data/background.npy`, so the launcher shows it without the
file it came from. Fitting cuts the picture to that shape around its middle and scales it evenly: it is never
stretched.

Pillow reads the file where the launcher's Python has it (a release's does: `tools/requirements-launcher.txt`), turned
the way its EXIF data says. Without Pillow (a checkout's Python may lack it), GDI+ reads it on Windows (PNG, JPEG,
BMP, GIF, TIFF) and Tk elsewhere (PNG, GIF; the pixels come over as a PPM file). The kept copy is 8-bit RGB at
`KEEP_W` x `KEEP_H` (1600 x 928), 1.6 times the logical size of the window below the band (1000 x 580): enough for a
display scaled to 200 %, and four and a half megabytes on disk.

`TYPES` are the file types the dialog offers: what the reader in use takes (`load()`).

### `PictureError`

The file is no picture this PC can read; the text is for the player.

### `_load_pillow()`

The picture as an H x W x 3 `uint8` array, read by Pillow and turned upright (a phone's photo says in its EXIF data
which way is up). Pillow raises its own errors, `OSError` and `SyntaxError` for files it cannot read; all become
`PictureError`.

### `_load_gdiplus()`

The picture as an H x W x 3 `uint8` array, read by GDI+ (part of Windows). The bits are locked with
`ImageLockModeRead` and `PixelFormat32bppARGB` (`0x26200A`): GDI+ converts whatever the file holds. A negative stride
means bottom-up rows.

### `read_ppm()`

A binary PPM (P6, 8 bits a channel) as an H x W x 3 `uint8` array. Its header is four fields (magic, width, height,
maximum) separated by white space, with comments from `#` to the line end.

### `_load_tk()`

The picture read by Tk (PNG, GIF) and written out as a PPM, which is three bytes a pixel after a short header.

### `load()`

The picture file as an H x W x 3 `uint8` array. `master` is a Tk window, needed where Tk reads the file. On Windows
without `gdiplus.dll`, Tk's own reader is used.

### `resize()`

The picture (H x W x 3) at `w` x `h`: whole pixels averaged first where it shrinks to less than half (`k` x `k`
blocks, so a much larger picture does not alias), then a bilinear interpolation. Returns `float32` when it
interpolates.

### `cover()`

The picture cut to the shape `w` : `h` around its middle and resized to `w` x `h`, so it fills the room with nothing
stretched; `uint8`, or `float32` in 0..255 with `as_float`. A picture wider than the window loses its sides, a taller
one its top and bottom.

### `keep()`, `kept_shape()` and `kept()`

- `keep()` reads the picture, fits it to the window's shape and writes it to `store` (`data/background.npy`).
- `kept_shape()` gives `(height, width)` of the kept background, or `None`.
- `kept()` gives the kept background at `w` x `h` as `float32` in 0..1, cut to that shape when it has another (one
  kept by an older launcher for the whole window), or `None` when there is none (or it is not what `keep()` wrote).

## `launcher/ui/selftest.py`

`--selftest [--shots DIR]`: draw every screen with made-up states, optionally save a PNG of each, and close. Nothing
runs: the jobs are built and their steps set by hand ([launcher.md](../launcher.md#tests)).

### `grab_gdi()`

Windows: copies the screen rectangle with GDI (`BitBlt` with `SRCCOPY`, `0x00CC0020`) and writes it as PNG
(`launcher/ui/art.py` `png_bytes()`); no extra packages. It uses private library handles with every argument typed: a
handle is a pointer, and one above 2^31 passed as a plain int failed with "int too long to convert", so some screens
went without their picture.

### `grab()`

A PNG of the window: GDI on Windows, else PIL's `ImageGrab`, else ImageMagick's `import` on X11.

### `ahead_of()`

A version a minor step past the given one: the made-up release must be newer than the launcher drawing it, or the
update screens show "Up to Date" instead (a fixed 0.3.11 did from 0.4.0 on).

### `states()`

The made-up states, one inner function each. The ones with notes:

- `rc2_release`: a release with the notes of a real release file, kept newer than the installed launcher without
  editing its `VERSION` file.
- `shaders`: the counts and the wording that `tools/make_game.py` `shader_progress()` sends.
- `update_staged`: a release downloaded in the background, waiting for the game to be closed.
- `color_scheme`: another colour scheme on the home screen, then the first one again.
- `custom_picker`: the Custom scheme with two picked colours, and its picker on the button colour. It sets the theme
  back at its end for the next screen's colours; its own screen is drawn already.
- `mods_screen`: a game folder of made-up mods, removed when the selftest closes.
- `consent`: the question about session reports as a preview: no countdown runs and nothing changes or is sent.

## `launcher/ui/sound.py`

The launcher's sound effects: two of the game's own interface sounds, the ones its menus make for a choice and for
going back, read from the player's game (`launcher/gamefiles.py` `extract_sounds()` decodes them into
`data/cache/sounds`, as `runtime/ui/menu_sound.c` plays them in the in-game menu). No sound ships with the launcher
and none is made here: until the player's game has been read, and for a sound it does not hold, the launcher is
silent.

They play at Low until the player picks another loudness, or Off, in Customize Launcher (the Sounds row). An effect is
the game's sound as a 16-bit WAV in memory, scaled to the loudness. Windows plays it with `PlaySound` from that
memory; elsewhere the WAV goes to a file in the cache folder and the first of `pw-play`, `paplay` and `aplay` found
plays it. One effect sounds at a time: a new one cuts the last. Moving the cursor makes no sound, as it would cut a
choice's sound short every time the pointer lands on a row of the next screen.

| Effect | When | The game's sound |
|---|---|---|
| `select` | a row was chosen, a switch or a choice changed, Play, the end of a build | its decide sound |
| `back` | back to the screen before, something could not be done | its cancel sound |

- `LEVELS` is the Sounds choice: the loudest sample's share of full scale (off 0.0, low 0.18, medium 0.4, high 0.75).
- `DEFAULT_LEVEL` (`low`) is that of a new `launcher.json` (the `DEFAULTS` of `launcher/settings.py`), and also what a
  value it cannot hold becomes.
- `EFFECTS` are the names `Sounds.play()` takes: the game's sounds of `SOUNDS` in `launcher/gamefiles.py`.

### `wav_bytes()`

A 16-bit WAV of samples (-1..1, interleaved when there are two channels) at a level (0..1 of full scale).

### `read_wav()`

A 16-bit PCM WAV as written by `launcher/gamefiles.py` `pcm_to_wav()`: `(rate, channels, samples in -1..1 with its
loudest at 1)`, or `None` when it is another kind of file.

### `Sounds`

Plays the effects at the level of the Sounds choice. Every failure is silent: a PC without a sound device, or without
a player program, just has a quiet launcher.

| Field | What it holds |
|---|---|
| `level` | the level in effect: `DEFAULT_LEVEL` unless the player chose another |
| `game` | the game's sounds decoded from the player's files: name to `(rate, channels, samples)` |
| `wavs` | name to WAV bytes at the current level (Windows plays from this memory: it must stay) |
| `files` | name to the WAV file, off Windows |
| `player` | off Windows: the program that plays a WAV file |

- `Sounds.load_game()` takes up the game's sounds decoded into `<cache>/sounds` (again after
  `launcher/gamefiles.py` `extract_sounds()` wrote them).
- `Sounds.stop()` ends what is sounding, before the memory it plays from goes.
- `Sounds.play()` on Windows calls `PlaySoundW` with `SND_ASYNC | SND_NODEFAULT | SND_MEMORY`: `PlaySound` reads the
  bytes while it plays, and `wavs` keeps them until the level changes (`Sounds.set_level()` stops the sound first).
  With no device or no player it logs once and stays quiet from then on.

## `launcher/updater.py`

Updating the launcher's own release folder (`app/`, `launcher/paths.py`; [launcher.md](../launcher.md#updating)).

An update is staged beside the running version and switched to with two small files, so a failed or interrupted
update never leaves a half-written release in use:

1. The app archive goes to `data/downloads`, is checked against the manifest's size and sha256 and unpacks into
   `app/<version>.part`, which is renamed to `app/<version>` once it is complete and holds the expected `VERSION`.
2. A new Python (the manifest's runtime id differs from `python/runtime-id.txt`) unpacks into `python.next` on
   Windows; Linux stages it beside the app and switches it while keeping `python.old` for recovery.
3. `app/previous.txt` gets the running version and `app/current.txt` the new one. Windows starts the native stub
   again; Linux starts the installed wrapper, which restores `previous.txt` and `python.old` if startup exits with
   code 86.
4. The new version rebuilds the game (`launcher/jobs.py`), then `prune()` removes every version but these two.

The launcher also stages a release in the background while it runs, the game included (`stage()`), and installs it
later, when the game is not running (`install_staged()`). Such a release waits in `app/<version>` with its Python,
when it brings one, in `app/<version>/python.staged` rather than `python.next`: the stub swaps `python.next` in at
every start, and the running version must not start on another Python before `current.txt` names the new one.
`app/<version>/staged.json`, written last, says the release is complete; a folder without it is never installed and
is staged again.

- `STAGED` (`staged.json`), in `app/<version>`: the background staging of that release finished.
- `STAGED_PYTHON` (`python.staged`), in `app/<version>`: the Python the release brings, until it installs.

### `_linux_runtime()`

Installed Linux runtimes have the relocatable `bin/python3` entry point of python-build-standalone.

### `safe_extract()`

Unpacks a zip into `dest`, refusing absolute names and names that climb out of it, and, before anything is written, a
zip whose files do not fit the disk (`launcher/net.py` `space_problem()`).

### `stage_app()`

Downloads, checks and unpacks the release's app archive into `app/<version>`; returns that folder.

### `stage_runtime()`

Unpacks a new Python into `dest` (`python.next`, for the stub to swap in at the next start); false when the running
one is current.

### `prerelease()`

True for a GitHub pre-release or a version with a pre-release tag (`0.5.0-rc1`): only the Preview channel installs it.

### `stage()`

Downloads, checks and unpacks a release into `app/<version>`, with the Python it brings in `python.staged` there,
without switching to it; `staged.json`, written last, marks it complete. Earlier staged releases it supersedes are
removed. Returns the folder.

### `staged_info()`

What `staged.json` says about `app/<ver>` (`{'version', 'runtime', 'prerelease', 'notes'}`) when `stage()` finished it
and it is still whole (its launcher, its `VERSION`, the Python it needs), else `None`.

### `staged()`

The newest release `stage()` left complete that is newer than the running version, as `staged_info()` gives it, or
`None`. A pre-release counts only on the Preview channel; `skip` is a version that failed to start.

### `install_staged()`

Makes a release `stage()` finished the one the stub starts at its next start: its Python goes to `python.next` for the
stub to swap in, `current.txt` names it (`previous.txt` the running version), and `staged.json` goes, so a version the
stub rolls back from is not installed again by itself.

- On Linux the live Python moves to `python.old`. A running launcher has already made it through
  `launcher/ui/app.py` `App.start()`, which removes stale `python.old` backups, so a leftover there can only be from
  an earlier failed start, and it is replaced with this live runtime.
- When a later step fails after the Python was moved, it is moved back: the running version stays, and it must not
  start on the new Python.

### `switch_to()`

Makes a version the one the stub starts, keeping the running one as previous.

### `rollback()`

Back to the version of `previous.txt`; false when there is none to go back to.

### `prune()`

Removes every app version but the current and the previous one and a staged release newer than the current one
(`_waiting()`), and stale downloads (`.part`, `.zip` and `.of` files). `python.old` is not removed here: it is the
rollback runtime until the launcher's wrapper confirms that the new version started.

## `launcher/versions.py`

Release versions: "0.3.11", optionally with build metadata ("0.3.11+abc1234") that never orders releases.

### `_pre_key()`

A pre-release's order as semver 2.0.0 gives it: dot-separated identifiers compared one by one, numbers as numbers
(`rc.9` before `rc.10`) and before text, and a shorter list before a longer one it starts.

### `parse()`, `newer()` and `display()`

`parse()` gives `(numbers, pre-release)` for comparing; a pre-release sorts before its release, and a malformed
version raises `ValueError`. `newer()` is true when version `a` is newer than version `b`. `display()` is the version
without build metadata.

## `launcher/winsys.py`

What the launcher needs from the operating system: crisp drawing on scaled displays, the game's fonts for this process
only, starting the game and the stub, shortcuts, the uninstall entry and a gamepad. Windows first; the other branches
exist so the window can be developed and tested on Linux, and call into `launcher/platform/linux.py`.

`OWNED` is what the launcher puts in its folder (`launcher/paths.py`, plus the stub's `python.next` and
`python.old-<n>`). Uninstall deletes only these: the default game folder is `<root>\game`, and a folder given to the
setup with `--dest` may hold other things.

The lines `os.startfile(path)` of `open_path()` and the `explorer` call of `reveal()` keep their `# noqa` comments.

### `dpi_aware()`

Draw at the display's real resolution instead of letting Windows stretch a 96-dpi window (blurry).

### `app_id()`

Groups the window under the launcher's own taskbar icon rather than Python's.

### `_hwnd()`

The Win32 window of a Tk toplevel (the wrapper around Tk's child window).

### `_post()`

Queues a message for the window instead of sending it. A sent message runs the window's handler inside this ctypes
call, which has released Python's lock; when that handler dispatches Tk events (the move loop of a title bar drag
does), Tk calls back into Python without the lock and Python aborts. A posted message is handled by Tk's own event
loop, where calling back into Python is allowed.

### `frameless()`

Windows: drops the system title bar (the window draws its own), but keeps a taskbar button, Alt+Tab, minimise from the
taskbar and Alt+F4. False elsewhere, where the window keeps its frame. The window is withdrawn and shown again 10 ms
later: the taskbar takes a changed window style only when the window is shown again.

### `minimize()`

Tk refuses to iconify a window without a frame, so Windows is asked (`ShowWindowAsync` with `SW_MINIMIZE`, 6);
asynchronously, for the reason given under `_post()`.

### `start_drag()`

Windows: lets the system move the window from the mouse's current press (as on a title bar, with snapping), by
posting `WM_NCLBUTTONDOWN` (`0x00A1`) with `HTCAPTION` (2). False elsewhere: the caller moves the window itself.

### `set_window_icon()`

Windows: the window's and the taskbar button's icon, straight from an `.ico`, in the large and the small size:
`ICON_BIG` at `SM_CXICON` x `SM_CYICON` (metrics 11, 12) and `ICON_SMALL` at `SM_CXSMICON` x `SM_CYSMICON` (49, 50),
loaded with `LoadImageW` (`IMAGE_ICON`, `LR_LOADFROMFILE`) and set by posting `WM_SETICON` (`0x0080`).

### `register_font()` and `unregister_font()`

`register_font()` makes a font file usable by this process only (`AddFontResourceExW` with `FR_PRIVATE`, `0x10`); Tk
then finds it by family name. True on success. `unregister_font()` gives up a font file it added, so the file can be
written again: Windows keeps a registered font file open, and writing it then fails with "Permission denied".

### `reveal()`

Shows a file in its folder, selected in Explorer (a file to drag into a bug report); elsewhere it opens the folder.

### `known_folder()`

The folder the desktop keeps for a name (`downloads`, `desktop`, `documents`, `pictures`): on Windows the shell's,
also when the player moved it to another drive; on Linux the one `user-dirs.dirs` names, translated on a desktop set
up in another language (`launcher/platform/linux.py` `user_dir()`). `None` when there is none. The Windows branch uses
private handles of `ole32` and `shell32`, so the argument types it sets stay there.

### `pick()`

A file (`kind` `file`) or folder (`folder`) the player chooses, or empty when they cancel. On Linux it is the
desktop's chooser (zenity or kdialog, `launcher/platform/linux.py` `pick_path()`), which has the desktop's places and
bookmarks and reaches the drives under `/mnt` and `/media`; Tk's own, a bare list of folders, only where neither is
installed. On Windows it is Tk's, which is the system's. `mustexist`: Tk's folder chooser takes only a folder that
exists (the desktop's always do). One line goes into the log: which chooser, and what it gave.

### `system_commit_info()`, `page_file_is_system_managed()` and `page_files_system_managed()`

- `system_commit_info()`: the system-wide commit used, the current limit and the physical RAM in MiB. `None` off
  Windows.
- `page_file_is_system_managed()`: true for a system-managed page file, false for a fixed-size or disabled one, `None`
  when Windows cannot say.
- `page_files_system_managed()`: classifies the `PagingFiles` entries as `runtime/platform/win32.c` does; `None` means
  malformed or unknown. An empty value means the value exists and Windows has no page files configured: false.

### `confirm_play()`

Yes plays anyway, No cancels. Steam calls it without a parent window. The box is
`MB_YESNO | MB_ICONWARNING | MB_SETFOREGROUND` (`0x4 | 0x30 | 0x10000`), with Yes as the default button; the answer
`IDNO` is 7.

### `start_game()`

Starts the game from its own folder and returns the process to watch. `ocarina.exe` is a console program and the
launcher has no console, so without `CREATE_NO_WINDOW` Windows opened an empty console window beside the game, titled
with the executable's path. With it the game still has a console (hidden), which its restarts inherit. On Linux the
launcher becomes a child subreaper first, so that the Restart's copy becomes its own to wait for
(`launcher/platform/linux.py` `child_subreaper()`).

### `GameCopy`

A process of the game this launcher did not start, held by a handle so that its exit code can still be read once it
has ended (`game_copy()`). It has `poll()`, `returncode` and `pid` as `subprocess.Popen` has them; `ended` is true
once it has ended, also when its code could not be read (`returncode` then stays `None`).

### `game_finished()`

The run of the game is over (`launcher/game_session.py` `GameRun.running()`): on Linux the launcher stops adopting
orphans and waits for the copies of the game it adopted that ended before it looked (`launcher/platform/linux.py`
`child_subreaper()` and `ended_copies()`), whose exit codes it returns, the latest last. Nothing to do elsewhere: an
empty list.

### `game_copy()`

Windows: the running process whose image is the file `exe`, other than the process id `skip`, as a `GameCopy`; `None`
when there is none or it cannot be opened. The game's own Restart (`runtime/gfx/gfx_present.c` `gfx_window_closed()`)
starts the executable again (`runtime/platform/win32.c` `plat_restart()`) and ends the process before it with code 0:
following the copy is how the launcher learns the code the game really ended with. The image is compared as a file,
not as a name, so the game of another install is not taken for this one.

### `restart()` and `stub_run()`

`restart()` starts the launcher again through its stub (which swaps in a staged Python and starts the version of
`app/current.txt`). False in a checkout, which has no stub. `stub_run()` runs the stub for one of its own commands
(shortcuts); true when it succeeded.

### `background_thread()`

Windows: puts the calling thread in background mode (`SetThreadPriority` with `THREAD_MODE_BACKGROUND_BEGIN`), which
lowers its processor, disk and memory priority, so work done beside the game takes only what the game leaves. False
where that is not possible. It uses a private handle of `kernel32`: the argument types set there touch no other
caller.

### `uninstall_plan()`

`(paths to delete with all they hold, folders to delete only when that left them empty)`, in order. A kept game folder
stays whether or not it lies inside the launcher's folder. A toolchain outside the launcher's folder (it went to
`C:\NX2Saria`, `launcher/paths.py` `ansi_safe()`) is deleted too, and its parent when that left it empty.

### `uninstall_script()`

The `cmd.exe` line that deletes them once the launcher has exited: a file with `del`, a folder with `rmdir /s`, the
folders that must be empty with a plain `rmdir` (which leaves a folder that still holds something). A path holding `%`
is left out: `cmd.exe` expands `%NAME%` even inside quotes, so a folder named `C:\%HOMEPATH%` would delete another.

### `uninstall()`

Removes the shortcuts, the uninstall entry and, once this process has exited, what the launcher put in its folder (and
the game folder when `remove_game` names it; [launcher.md](../launcher.md#uninstalling)). A kept game folder and its
saves stay, also when it lies inside the launcher's folder. `keep`: on Linux, paths that stay whatever folder holds
them (the game folder when it is not removed, the dump).

- The script is passed as one command line, as `cmd.exe` reads it: a list would have its quotes escaped as `\"`,
  which `cmd.exe` does not understand, and no `rmdir` would find its folder.
- On Linux it is the AppImage's install that is deleted, by `sh` once this process has gone
  (`launcher/platform/linux.py` `uninstall_plan()`).

### `in_front()`

Whether the window the user is typing into belongs to this process. XInput reads the pad whichever window is in front,
so without this check the launcher, left open behind the game, took the presses meant for the game: A activated its
focused row (it started the game five times in five minutes and tried a rebuild), the stick moved its focus and Y
checked for updates. Elsewhere Tk knows whether one of its windows has the keyboard focus, which the window manager
gives the window in front (SDL3 reads the pad there whichever window is in front too). True where it cannot tell.

- Steam's Game Mode (`GAMESCOPE_WAYLAND_DISPLAY` or `SteamDeck` set) shows one window at a time, and the pad is the
  only input there: always true.
- On Windows, no window in front (the desktop is switching, a lock screen) means nothing is meant for the launcher:
  false.

### `_WindowsGamepad` and `Gamepad`

`_WindowsGamepad` polls the first XInput pad; `poll()` returns the buttons pressed since the last call (edges), the
left stick counted as the d-pad. `poll(False)` still reads the pad but returns nothing: a button held while another
window was in front does not count as pressed when this one comes back. `Gamepad` is the stable surface over both:
XInput on Windows and the SDL3 game controller on Linux.

### `LauncherParent`

The detached Windows worker holds a handle with synchronization and query access to Electron's main process.
`LauncherParent.open()` verifies the actual parent PID, confirms it was created before the worker, and
rejects an already ended or reused PID. The worker checks the handle every 200 ms while waiting for a request
and treats parent exit like EOF.

### `launcher/build.py` `SupervisedRun`

The Electron worker's build runner starts the supervisor from the same app code and Python
runtime. It extends the existing marker atomically with the supervisor identity and only then
acknowledges startup. The runner tails the supervisor's output file through the normal build
progress parser. A cancel request is a flag in the private run directory. If the runner cannot
observe a terminal result, the job keeps its marker for recovery. A completed job clears its
marker and removes the supervisor's temporary files. Tk and CLI still create the usual `Run`.

## `launcher/build_recovery.py`

`inspect_marker()` classifies an interrupted build without clearing its marker. On Linux it
checks the boot, supervisor start tick and live session membership. On Windows it reopens the
named job or checks process creation time and the terminal record. It treats invalid, legacy and
inaccessible markers as unverified. The worker consumes only a verified finished marker through
the existing job recovery function. The renderer receives the normalized state and kind. The
log also records a reason, with no process information from the marker.

`request_stop()` uses the verified Windows job handle or asks the matching supervisor to
cancel. On Linux, `signal_linux_orphan()` handles a remaining session whose leader died,
rechecking each process reference before sending a signal. The worker observes the marker
with a bounded polling interval after a Wait or Stop request. A sequenced recovery snapshot
uses session zero, since it belongs to neither a new job nor a game. The Electron client
validates that snapshot before restoring the appropriate controls.

`rebooted_since_observation()` keeps the marker fingerprint and boot identity in a private
sidecar for an unverified marker. The same marker observed in a different boot can be
consumed under the worker lock. Linux reads its kernel boot ID. On Windows,
`launcher/platform/windows.py` `last_boot_utc()` asks the OS for its last restart time once,
through the system PowerShell executable. If that query or the marker read fails, there is
no automatic reboot recovery. The explicit confirmation must still match the marker
inspected earlier. Player confirmation and reboot evidence have distinct log reasons.

## `launcher/build_supervisor.py`

`supervise()` reports the identity of its Linux session or Windows build group. It waits for a
single start acknowledgement; EOF before that line exits without creating a build log or
starting the command. The build receives a file for its output so losing the worker pipe does
not change how the compiler finishes. The supervisor waits for the main command and remaining
members, replaces the terminal result file atomically and exits. Linux sends SIGTERM to the dedicated group and permits a five-second grace period. After
that the supervisor uses process descriptors for verified members. On Windows, the supervisor
opens and verifies each named job member by handle before stopping it.

`launcher/platform/linux.py` `process_identity()` reads the process group, session and creation
tick from procfs. `session_members()` is used to drain the build session after the main command
exits, excluding the supervisor and zombies.

`launcher/platform/windows.py` `BuildJobGroup` identifies the supervisor by process creation
time and attempts to assign it to a named job. The job name contains a digest of the install
root and a unique token. If assignment is refused the build can still run with the PID identity.
The named job counts remaining processes without killing them when a handle closes.

## `launcher/worker.py`

`main()` accepts newline-delimited UTF-8 JSON over pipes. It checks the exact integer protocol version
and the selected installation before opening the lock or constructing `Context`. The handshake returns the
active version, capabilities, lock state and whether a complete update is waiting. The worker checks the
installed app again under the lock and uses its Python tools. It detects staged updates without modifying them.
Busy, incompatible and bad requests return error codes without exception details.

The reader accepts integers and strings as request IDs and caps each frame at 64 KiB. It queues requests for
the main loop. The worker sends frames as bytes through one output lock and directs regular stdout output to
stderr. Repeated action logs are limited to ten per category, followed by one suppression line. Idle EOF
releases the lock. On EOF, parent exit or a broken output pipe during a game, the worker follows the game
through completion. During a job it requests cancellation and waits for marker, lock and claim cleanup.

`inspect` uses the read-only installation check. `set_hide_while_playing` accepts a boolean while idle
and requires a held lock. It acknowledges the change after the settings write succeeds.

First-run preference requests allow the existing update channel and check-at-start setting,
plus desktop shortcut on Windows. The worker requires an idle first run and a held lock.
It records the Windows shortcut preference and asks the same installed stub as Tk to
apply it. The request does not schedule an update.

`first_run_requirements()` displays the runtime's existing Zstandard status and reuses Linux's
compiler and header probes with a bounded timeout. The result is advisory. It checks no game files
and requests no package installation. The install job still performs its normal toolchain checks.

`install_destination()` offers a proposed game folder for an unconfigured or unavailable game. The worker
uses the saved path if present and otherwise asks `Layout.default_game_dir()`. A picker result must be an
absolute existing directory and uses the shared `install_folder()` policy. The worker refuses the proposal
during a Play, memory decision or job. The log records the source or rejection, without the folder path.
`install_space()` supplies free space including the partial image, the 34 GB threshold, and flags for
a complete image and the Windows path limit. The same values are exposed to the first-run screen.

`install_dump()` checks a picker result or saved choice with `launcher/gamefiles.py` `is_game_nup()`.
The check recognizes the game's title in the container metadata. The build still validates the
version and contents. No settings are changed, and the worker logs a selected, saved, unset or
rejected outcome without the dump's filename.

`search_install_dump()` runs the Tk launcher's finder in a background thread, using the parent of
the launcher root and the normal search folders and mounted drive limits. Only the worker loop
may accept a result into the draft. It keeps a prior manual choice and logs completion with
counts and time. The status request is nonblocking, so the renderer can poll for the result
without delaying inspection or manual selection.


The worker retains the last successful proposals until confirmation. `prepare_install()` rechecks
the source, destination, path limit and running game and protects playable targets. On a valid
target it accounts for a partial ROMFS image and checks free space again after that image is
removed. A completed ROMFS image keeps the existing low-space exception.

The install request requires the lock and refuses an active game, job, memory decision or remaining
job marker. It saves the two choices, honors the existing desktop shortcut setting and runs
`launcher/jobs.py` `InstallJob`. `job_ended` follows marker and claim cleanup. Its `complete`
outcome refreshes the Electron installation screen. It never resumes Play after installation.


The rebuild request requires a playable saved game and asks for `launcher/jobs.py` `RebuildJob`
with the saved source and destination. As with installation, a job marker, an active game or
job, or an unavailable lock prevents starting it. Explicit installs and rebuilds report
`complete` on success, honor the existing build-cache preference and return to the inspected
screen. After a successful mod rebuild, the worker resumes Play instead.


`checked_update()` completes the existing `launcher/jobs.py` `CheckJob` on the main worker loop.
The worker accepts a single manual check at a time and uses the same one-minute start throttle
as Tk. The job only reads the feed; the main loop records the existing settings metadata and
exports the availability, version and bounded plain notes. Each note line is limited in
UTF-16 units, including characters outside the Basic Multilingual Plane, so the complete ASCII-
escaped frame stays within the pipe limit. The Electron client applies that limit to individual
frames even when the pipe delivers part of one frame and another frame together. A response from an earlier channel
is discarded if the channel changes. The check event is independent of game and build sessions.


`export_save()` keeps the existing ZIP writer and the selected destination. The protocol
returns the generated filename, file count and whether there are diagnostic attachments.
Setting changes require the lock and store only the chosen field. Read-only inspection
does not create the export folder. The worker retains the original no-save and log-only
behaviors and reveals a successfully written archive.

`import_save()` accepts a waiting entry or an absolute path from the selected picker.
It rechecks whether the game is running before entering the existing Python import.
The existing profile lock, archive validation, backup, and replacement or rollback paths
remain responsible for the files. The protocol identifies the imported slot or whole
save, file count and backup basename. The window can reopen a backup through folder
import and can open the game saves folder through the existing platform function.
Import and export wait for the copy result rather than using the short request deadline.


For Play, a memory decision has an attempt number. The worker rejects a stale answer and checks for another
game before resuming. Mod rebuilds use the shared completion policy and wait for `finished` before resuming
Play or reporting the result. Sequenced game events identify the session. They mark confirmed spawn, window
focus when observed and completion confirmed by `GameRun`.

The main loop records `worker: game restart observed` when `GameRun.running()` first identifies a replacement
copy. The restart stays in the same sequence and session. The worker keeps its lock and reports the final copy's
exit through the original game-end event.

`GameWindowLog` takes initial offsets from both possible game log paths, then reads bounded chunks. It
tracks file identity and truncation so a stale focus line cannot end the opening state. The worker emits
`game_window_ready` once per session upon seeing the game's focus-gained line. This signal comes from native
window events on Windows and SDL on Linux. The Electron window stays visible until focus is confirmed.

After the game log is finalized, `defer_crash_offer()` calls the selected installed version's
`launcher/crash_offer.py` for an abnormal end if that module exists. The installed helper checks eligibility,
opt-out and log identity and may leave a local marker for the installed launcher. It runs before a disconnected
worker exits. A failure to load the helper or write the marker is logged and the worker still reports game end.
Session sending and crash-offer display remain with the installed launcher.
