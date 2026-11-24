<!-- languages -->
**English** · [Português (Brasil)](pt-BR/telemetry.md) · [Русский](ru/telemetry.md) · [简体中文](zh-Hans/telemetry.md)
<!-- /languages -->

# Session reports and comments

The launcher can send three things to the project's own server:

- **Session reports:** after you play, a short summary of how the game ran on your PC.
- **Comments:** text you write in *Send a Comment*, sent when you press *Send*.
- **Crash logs:** the start and the end of the game's log, sent when the game ended with an error and you press
  *Send the Log*.

## When the launcher asks

The first time a new version of the launcher opens, it asks **Help improve NX2Saria?** with three choices:

- **Send** turns session reports on. It is the selected button, so Enter chooses it.
- **Don't Send** keeps them off. It is the only choice that does.
- **See What Is Sent** shows the exact text of a report.

**Session reports also turn on by themselves** in two cases, which the question's first line says while it is open:

- when you close the question without choosing: Esc, the window's close button or Alt+F4;
- after 30 seconds without any input. The line counts the seconds down. Any input stops the countdown for good: a
  key, a controller button, moving the mouse over the window, opening *See What Is Sent*.

When reports were turned on in one of these two ways, the launcher says so once the next time it opens: *Session
reports are on. You can change this in Settings > Session Reports.*

The question comes back at the first start of **every new version** as long as session reports are not on: if you
chose *Don't Send*, and if you switched them off in Settings. It is asked once for each version. While reports are on
it is never asked.

The question opens on the launcher's main screen, never over an installation or an update in progress. When the
launcher opens to finish an update, or has just built the game for the first time, it asks as soon as that is done.
No countdown runs then, because you may be away from the PC: the question waits, and reports turn on only when you
choose *Send* or close it.

No report is sent at the moment reports turn on: the first one is the report of the next session you play. From then
on one report goes each time the game ends, and one more when the game crashed and the launcher made the report from
the log. Nothing is sent while you play.

You can switch session reports on and off at any time in **Settings → Session Reports**.

A launcher built without a server address shows none of this: no question, no Session Reports screen, no
*Send a Comment* and no question after a crash.

The server is a small program of this project on Cloudflare Workers with a Cloudflare D1 database. Its whole source is
in the repository: [`telemetry/`](../telemetry/). No analytics service of anyone else is used.

## What a session report holds

The game writes it to `logs/session-summary.json` in the game folder when it ends, so you can read it before anything
is sent. A report has these fields and nothing else; the server refuses a report with any other field.

| Field | What it is | Example |
|---|---|---|
| `schema` | The format's name and version | `nx2saria.session/1` |
| `version` | NX2Saria's version | `0.19.0` |
| `game_version` | The game's version | `1.1.1` |
| `os.family`, `os.name`, `os.wine` | Windows or Linux, its name and build, whether it runs under Wine | `windows`, `Windows 11 (build 26100)`, `false` |
| `cpu.name`, `cpu.threads` | The processor's model name and its number of threads | `AMD Ryzen 5 5600 6-Core Processor`, `12` |
| `ram_mb` | The PC's memory, in MB | `16384` |
| `gpu.name`, `gpu.vendor`, `gpu.device`, `gpu.driver`, `gpu.vram_mb` | The graphics card's model and its PCI ids, the driver's name and version, its video memory | `NVIDIA GeForce RTX 3060`, `0x10de`, `0x2504`, `NVIDIA 581.57`, `12288` |
| `display.output`, `display.render_scale`, `display.fullscreen`, `display.game_aspect`, `display.picture` | The output resolution, the render scale, fullscreen or window, the aspect ratio setting, and the size of the picture the game drew | `1920x1080`, `game`, `true`, `16:10`, `2304x1440` |
| `settings.preset`, `settings.upscaler`, `settings.taa`, `settings.dlss`, `settings.framegen`, `settings.fps_limit`, `settings.vk_present` | The quality preset in use (`low`, `medium`, `high`, `ultra`, or `custom` when you changed one of its settings) and the main graphics options. A value that is not one of the option's own choices is sent as `other` | `high`, `fsr`, `dlss`, `dlaa`, `off`, `60`, `fifo` |
| `session.duration_s`, `session.frames`, `session.end` | How long the game ran, how many frames it showed, how it ended: `normal`, `fatal`, `device_lost` or `crash` | `1834`, `109800`, `normal` |
| `frametime.fps_mean`, `frametime.fps_low_1pct`, `frametime.worst_ms`, `frametime.over_25ms`, `frametime.over_100ms` | The average frame rate of the whole session, its 1% low, the slowest frame, and how many frames took over 25 ms and over 100 ms | `59.9`, `52.1`, `88.4`, `14`, `0` |
| `startup.shader_prewarm_ms`, `startup.shaders_translated`, `startup.pipelines_built`, `startup.pipeline_build_ms` | The time spent preparing shaders and pipelines at the start, when there was any | `5400`, `120`, `900`, `3100` |
| `patches` | The names of the port's own patches that were on, from the list in the game's log line `patches:`. The server keeps only names it knows. A patch set built from a mod's code patches is never listed | `["fps60", "ostep", "zstd"]` |
| `mods.count`, `mods.code` | How many mods were in use, and how many of them change the game's code. Never their names | `1`, `0` |
| `fatal` | The game's error line, when the session ended with one, with your folders written short as in the log ([log privacy](log-privacy.md)), also when you turned that off for your own log, at most 300 characters | `null` |

Names are cut at 64 characters (the system's, the processor's, the graphics card's and the driver's), control
characters are dropped, and `version` is the release without its build suffix.

A field the game does not know is `null`. When the game crashed so hard that it could not write the report, the
launcher makes one from the error line in the log: only `version`, `game_version`, `os.family`, `session.end`
(`crash`) and `fatal`, every other field `null`.

Nothing else is collected. In particular the server does not keep the country a report came from.

With the report the launcher sends only `launcher_version`, the launcher's version. It sends **no id of your
installation**: nothing in a report says which PC it came from, or that two reports came from the same one. A
launcher that made such an id in an earlier version removes it from its settings the first time it opens.

The server adds the time the report arrived, and nothing about where it came from.

## What a comment holds

- Your text, up to 4000 characters, **exactly as you wrote it**. Leave out your e-mail, name or anything else you do
  not want to send.
- The report of your last session, as above, when there is one, unless **Session Reports** is *Off*.
- If you tick *Attach the Last Log Lines*: the last 200 lines of `logs/nx2saria.log`, at most 24 KB, with your home
  folder, account name and computer name written short as in the log ([log privacy](log-privacy.md)), also when you
  turned that off for your own log. Other folder names in those lines, such as where the game is installed, stay as
  they are.
- The `launcher_version` and the time, as with a report. No id of your installation.

A comment is sent only when you press *Send* on it, whether session reports are on or off.

## When the game ends with an error

When the game ends with a fatal error, a crash, a lost graphics device or any other exit code that is not a normal
end, the launcher's main screen asks **Send the log of this crash?** under the sentence that says how the game ended,
with four choices:

- **Send the Log** sends the log lines described below, once. It is the selected button.
- **Don't Send** sends nothing.
- **See What Is Sent** shows the exact text that would go.
- **Write a Comment** opens *Send a Comment* with *Attach the Last Log Lines* ticked, for telling what you were doing.
  The crash log itself is then not sent: the comment goes as any other comment does.

**Nothing is sent unless you choose *Send the Log*.** There is no countdown, and closing the question in any other way
(Esc, B, the window's close button) is the same as *Don't Send*. For the first second after the question appears it
takes no input, so that a button still held from the game does not answer it. It is asked once for each such end, with
session reports on or off, and a log that could not be sent is not tried again.

It is not asked when you closed the game yourself (the window's close button, the game's own quit, Alt+F4), nor on
Linux when a signal asked the game to end (SIGTERM, SIGINT or SIGHUP: a logout, or another program stopping it) and
its log has no fatal error. A game that was killed (Task Manager's *End task*, SIGKILL) is asked about, because a game
that hung is worth a log.

When the game was started without the launcher's window (Play in Steam, a shortcut), nothing is asked or sent at that
moment: the question waits and appears the next time the launcher opens, if the game's log is still the one of that
session. If you played again meanwhile, it is dropped without asking. The question never opens over an installation,
an update or a rebuild, nor while the game runs. When the question about session reports is due at the same start,
that one comes first.

### What a crash log holds

- **The start of the game's log:** from its first line to the line `patches:`, at most 100 lines and 16 KB. These
  lines name your system, graphics card and driver, the options in use and the patches that are on; in the logs
  measured they are about 84 lines and 12 KB.
- **The end of the game's log:** its last 200 lines, at most 56 KB. 200 lines weigh about 46 KB, so the 200 lines are
  the limit that usually applies; when the lines are longer than usual the 56 KB cut first, and fewer lines go, the
  last ones kept.
- **After a short session these two parts are the whole log.** When the log has no more lines than that, every line of
  it is sent, each one once. The question and *See What Is Sent* say how many lines go.
- In those lines your home folder, account name and computer name are written short as in the log ([log
  privacy](log-privacy.md)), also when you turned that off for your own log. Other folder names, such as where the
  game is installed, stay as they are, and so do the names of the mods in use, which the log lists.
- The report of that session, as above, when there is one, unless **Session Reports** is *Off*.
- The `launcher_version` and the time, as with a report. No id of your installation, and no text of yours.
## What is not sent or not kept

- **Your IP address is not kept.** Every connection to a server carries it, so the server sees it: it uses it at that
  moment, in memory, to limit abuse (at most 20 requests a minute from one address), and never writes it down. It
  keeps no country either. Cloudflare, which runs the server, handles the connection as it does for any website.
- No files: no saves, no settings file, no game files, no keys.
- **No log lines go unless you press the button that sends them:** *Send* on a comment with *Attach the Last Log
  Lines* ticked, or *Send the Log* after a crash. A crash log of a short session can be the whole log, as described
  above; a session report never holds log lines other than the error line.
- **No id of your installation.** The launcher makes none, keeps none and sends none.
- A session report holds no account name, computer name, folder name, e-mail, hardware serial number or mod name:
  the server refuses a report with a field the format does not have. A comment holds what you write and, when you attach them,
  log lines as described above. A crash log holds log lines, in which the game writes the names of folders outside
  your home folder and the names of the mods in use as they are.

## How long it is kept

Reports, comments and crash logs are deleted by a daily cleanup once they are 180 days old. Only the project's
maintainer can read them, through a page protected by a secret key: counts by version, graphics card, system, options,
patches and mods, frame rates, the most common errors, the comments and the crash logs.

## Why nothing can be deleted on request

A report has no id of your installation and the server keeps no IP address, so the server cannot tell which reports
are yours: there is nothing it could look up and delete for you. Every report, comment and crash log is deleted when
it is 180 days old. The database service keeps its own restore history for a limited time (up to 30 days at
Cloudflare); deleted rows leave it as that history ages.

A comment is the one thing that holds what you choose to put in it. It is sent exactly as written, so leave out
anything that says who you are.

## Turning it off

If you do not want to send reports, set **Settings → Session Reports** to *Off*, or choose *Don't Send* when the
launcher asks. The next version of the
launcher asks again once; choose *Don't Send* again to keep them off. If the project ever stops collecting, the
server answers so and the launcher stops sending until its next version.

If you do not want to be asked after a crash, set **Settings → Session Reports → Ask After a Crash** to *Off*. No
crash log is ever sent without the question.

## Known limits

- Restarting the game from its settings menu starts a new process, which removes the report of the first one before
  the launcher can send it: only the last process's report is sent.
- Writing a comment needs a keyboard (or an on-screen keyboard, such as Steam's); the rest of the screens work with a
  controller.
