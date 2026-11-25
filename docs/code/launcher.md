# launcher

The launcher: the window, the installation, the updates, the saves and the session reports. What it does for a player
is in [launcher.md](../launcher.md) and [telemetry.md](../telemetry.md). This page is started: the rest of `launcher/`
keeps its comments until the area is extracted ([README.md](README.md#areas)).

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

### `App.offer_guarded()`

For `GUARD_SECONDS` (1 s) after the question opens, `App.activate()` and `App.back()` do nothing on it: the window
comes to the front the moment the game dies, and a button still held for the game would otherwise choose *Send the
Log*, which is the focused row. Moving the cursor is not guarded, and neither is closing the window, which sends
nothing. The preview (`--screen crash`, the selftest) has no guard.

### `App.crash_offer_desc()`

The sentence of the game's end is shown without its last sentence (`launcher/game_session.py` `LOG_WHERE`, where the
log is): with the longest fatal line the launcher shows (`FATAL_SHOWN`, 240 characters) the text would not fit the
panel with it, and the question is about that very log. The home screen keeps the whole sentence.

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

### `game_session.py`

The game session tracks restarted copies, interprets exit codes and reads or annotates the game log.
The Tk module continues to expose the moved session names for existing callers. Exit code meanings,
normal end markers, log truncation and requested termination signals retain their existing rules.

Exit codes 70, 71 and 72 describe a fatal error, crash report and lost graphics device; Windows status codes
are shown with their known meanings. Requested Linux termination signals do not raise an alarm; SIGKILL is
excluded because the kernel also uses it when memory runs out. A size-capped game log cannot establish whether
the game wrote its own exit line. A launcher-added exit note ends the search for an earlier session.

### `launcher/game_session.py`: the game's end

Once `GameRun.running()` has confirmed that the followed game and any Restart copies have ended, `finish_game()`
asks the run for its `GameEnd`, writes its end line to the launcher log, synthesizes a crash summary if session reports
are enabled, then appends the exit note to the game log if one is due. A hard crash may not leave a session summary;
the summary must be made from the fatal line before the launcher note marks the end of that log's run. The function
returns the `GameEnd`, including its optional message, without a Tk dependency.

`App.watch_game()` still schedules the next check while the run is active. After it ends, the adapter restores, raises
and focuses the window before calling `finish_game()`, then shows any returned message, checks a waiting staged update,
handles after-game reports and finally shows home. A staged update or report action that takes over still short-circuits
the later steps. The consent and report policy are unchanged; the extraction only moves the end processing.

### `launcher/game_session.py`: `end_kind()` and `end_asked()`

`end_kind()` names an end that has a sentence for the player: `fatal`, `crash` and `device_lost` for the codes 70, 71
and 72, `fatal` for a fatal line found after the game restarted itself, and `code <text>` for every other code, with
the text `exit_code_text()` gives. An end without a sentence has no kind, and nothing is asked about it: the window's
close button, the game's own quit and Alt+F4 all end the game with code 0.

`end_asked()` is true for an end by SIGTERM, SIGINT or SIGHUP (`ASKED_SIGNALS`) when the log has no fatal line:
someone asked the game to close, and that is no crash. SIGKILL, SIGSEGV, SIGABRT and, on Windows, Task Manager's End
task (code 1) are not asked ends: a game that hung and was killed is worth its log. It must be called before the
launcher's own line goes into the game's log, which ends the search for the fatal line (`launcher/build.py`
`last_fatal()`); `finish_game()` and `play_only()` call both before `note_exit_in_log()`.

### `launcher/play.py`: Play preflight

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

### `launcher/commit.py`: memory decision

`play_memory_request()` samples Windows commit and the page-file mode, logs the existing values and policy, and returns
`None` when the sample is unavailable or no question is due: enough memory, the warning disabled, a system-managed or
unknown page-file mode, or an estimate that disables the check. Otherwise it returns the remaining memory and game
estimate. `need=None` with `unbounded=True` represents an infinite estimate without a non-finite JSON number.

`play_memory_warning()` recreates the existing warning from the request, including the infinite estimate.
`play_memory_response()` logs the player's answer and returns it; an unavailable dialog is logged and permits Play.
`confirm_play_memory()` retains the synchronous entry point used by Steam and combines these functions with the
existing native dialog, including the fallback that permits Play if the probe or dialog is unavailable.

### `worker.py`: first persistent-worker slice

The worker accepts a newline-delimited UTF-8 JSON handshake with the exact integer protocol version before inspecting or
writing the selected root. A root must be an existing launcher with `app/current.txt` and a data directory. A read-only
inspection rejects invalid settings before `Context` can create a recovery copy. The worker then constructs `Layout`,
takes the existing `FileLock` before `Context`, and reports whether the lock was acquired or could not be opened. A busy
lock fails visibly. It reports its version, capabilities and whether a complete staged update is waiting; detecting it
does not modify it. It answers `inspect` using the existing read-only inspection and logs the result. Inspection and
startup failures return generic errors without exception details. Startup failures release the lock and are logged when
logging is available. Repeated inspection and unknown-method log entries are limited to ten per action, followed by one
suppression line. EOF while idle closes the log and releases the lock. Frames have a 64 KiB limit, IDs are integers or
strings, and malformed frames or a protocol mismatch are rejected without opening the installation. This slice does
not launch a game, manage jobs, send reports or connect the Electron window to the worker.

The in-progress Play worker records bounded action outcomes under `worker:` in addition to the existing `play:` and
`play memory:` lines. It tracks a memory decision by attempt number, rejects stale replies and rechecks for a game
before spawning. Game events are emitted after spawn and after `GameRun` reports completion. The input reader uses a
bounded queue; the main loop remains responsible for following the game after EOF or an output failure. The mod rebuild uses the shared completion policy and waits for `finished` before emitting an end or resuming Play; EOF requests cancellation and still waits for cleanup. On Windows the detached worker receives the Electron main PID. `winsys.LauncherParent` opens the actual parent with
synchronization and query access. It checks both creation times and rejects an already ended or reused PID. The worker
waits at most 200 ms between checking the handle while waiting for requests. It records a parent exit and drains an
active game or job as for EOF. An installed worker checks the active app again under the installation lock, then uses
its `Layout` for tools and writes a runtime line through the existing path filter. `toolchain.zbic_ok()` probes the build
interpreter with stdin closed to input so the probe cannot inherit the worker's protocol pipe.

### `mods_job.py`: mod rebuild completion

`drop_build_cache()` removes the lifted sources and objects when the player does not keep the build cache, while
preserving build state needed for update. Its body is the former Tk method; `App` continues to expose it for other
jobs. `finish_mods_job()` applies this policy after a successful mod rebuild and returns `resume` with cleared failed
mods. A cancelled or failed rebuild retains the waiting mod names and returns its result for the caller to choose the
screen or retry. Tk preserves its existing result timing. A worker calls with `require_finished=True`, which returns
`pending` without cleanup until the job has completed marker, claim and lock cleanup.
