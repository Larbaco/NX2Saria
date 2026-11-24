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

### `game_session.py`

The game session tracks restarted copies, interprets exit codes and reads or annotates the game log.
The Tk module continues to expose the moved session names for existing callers. Exit code meanings,
normal end markers, log truncation and requested termination signals retain their existing rules.

Exit codes 70, 71 and 72 describe a fatal error, crash report and lost graphics device; Windows status codes
are shown with their known meanings. Requested Linux termination signals do not raise an alarm; SIGKILL is
excluded because the kernel also uses it when memory runs out. A size-capped game log cannot establish whether
the game wrote its own exit line. A launcher-added exit note ends the search for an earlier session.
