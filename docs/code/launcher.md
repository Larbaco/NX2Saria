# launcher

The launcher: the window, the installation of the build tools and of the game, the updates, the mods, the saves and
the session reports. What it does for a player is in [launcher.md](../launcher.md), [install.md](../install.md) and
[telemetry.md](../telemetry.md); this page holds the notes of its code, one section per file in the order of the file
names. These files still keep their comments until their notes are here: `launcher/context.py`,
`launcher/game_session.py`, `launcher/gameart.py`, `launcher/gamefiles.py`, `launcher/gameini.py`, `launcher/jobs.py`,
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
