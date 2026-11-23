<!-- languages -->
**English** · [Português (Brasil)](pt-BR/telemetry.md) · [Русский](ru/telemetry.md) · [简体中文](zh-Hans/telemetry.md)
<!-- /languages -->

# Session reports and comments

The launcher can send two things to the project's own server:

- **Session reports:** after you play, a short summary of how the game ran on your PC.
- **Comments:** text you write in *Send a Comment*, sent when you press *Send*.

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
it is never asked. A new installation is asked the first time the launcher opens after the setup, not during it.

No report is sent at the moment reports turn on: the first one is the report of the next session you play. From then
on one report goes each time the game ends, and one more when the game crashed and the launcher made the report from
the log. Nothing is sent while you play.

You can switch session reports on and off at any time in **Settings → Session Reports**, and delete what was sent
there.

A launcher built without a server address shows none of this: no question, no Session Reports screen and no
*Send a Comment*.

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
| `patches` | The names of the port's own patches that were on, from the list in the game's log line `patches:`. The server keeps only names it knows | `["fps60", "ostep", "zstd"]` |
| `mods.count`, `mods.code` | How many mods were in use, and how many of them change the game's code. Never their names | `1`, `0` |
| `fatal` | The game's error line, when the session ended with one, with your folders written short as in the log ([log privacy](log-privacy.md)), at most 300 characters | `null` |

A field the game does not know is `null`. When the game crashed so hard that it could not write the report, the
launcher makes one from the error line in the log: only `version`, `game_version`, `os.family`, `session.end`
(`crash`) and `fatal`, every other field `null`.

Nothing else is collected. In particular the server does not keep the country a report came from.

With the report the launcher sends:

- `install_id`: a random number made by the launcher the first time it is needed and kept in its settings. It is not
  made from anything on your PC; it only lets the reports of one installation be counted together and be deleted
  together.
- `launcher_version`: the launcher's version.

The server adds the time the report arrived, and nothing about where it came from.

## What a comment holds

- Your text, up to 4000 characters, **exactly as you wrote it**. Leave out your e-mail, name or anything else you do
  not want to send.
- The report of your last session, as above, when there is one, unless **Session Reports** is *Off*.
- If you tick *Attach the Last Log Lines*: the last 200 lines of `logs/nx2saria.log`, at most 24 KB, with your home
  folder, account name and computer name written short as in the log ([log privacy](log-privacy.md)). Other folder
  names in those lines, such as where the game is installed, stay as they are.
- The same `install_id`, `launcher_version` and time.

A comment is sent only when you press *Send* on it, whether session reports are on or off.

## What is not sent or not kept

- **Your IP address is not kept.** Every connection to a server carries it, so the server sees it: it uses it at that
  moment, in memory, to limit abuse (at most 20 requests a minute from one address), and never writes it down. It
  keeps no country either. Cloudflare, which runs the server, handles the connection as it does for any website.
- No files: no saves, no settings file, no whole log, no game files, no keys.
- A session report holds no account name, computer name, folder name, e-mail, hardware serial number or mod name:
  the server refuses a report with a field the format does not have. A comment holds what you write and, when you attach them,
  log lines as described above.

## How long it is kept

Reports and comments are deleted by a daily cleanup once they are 180 days old. Only the project's maintainer can read
them, through a page protected by a secret key: counts by version, graphics card, system, options, patches and mods,
frame rates, the most common errors and the comments.

## Deleting your data

**Settings → Session Reports → Delete My Data** deletes every report and comment sent with your `install_id` from the
server, then makes a new `install_id`. It works whether session reports are on or off. The server gives the same
answer whether it held anything or not. The database service keeps its own restore history for a limited time (up to
30 days at Cloudflare); deleted rows leave it as that history ages.

## Turning it off

Set **Settings → Session Reports** to *Off*, or choose *Don't Send* when the launcher asks. The next version of the
launcher asks again once; choose *Don't Send* again to keep them off. If the project ever stops collecting, the
server answers so and the launcher stops sending until its next version.

## Known limits

- Restarting the game from its settings menu starts a new process, which removes the report of the first one before
  the launcher can send it: only the last process's report is sent.
- Writing a comment needs a keyboard (or an on-screen keyboard, such as Steam's); the rest of the screens work with a
  controller.
