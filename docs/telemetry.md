<!-- languages -->
**English** · [Português (Brasil)](pt-BR/telemetry.md) · [Русский](ru/telemetry.md) · [简体中文](zh-Hans/telemetry.md)
<!-- /languages -->

# Session reports and comments

The launcher can send two things to the project's own server, and only when you say so:

- **Session reports:** after you play, a short summary of how the game ran on your PC. Off until you answer the
  launcher's question with *Send*.
- **Comments:** text you write in *Send a Comment*, sent when you press *Send*.

Nothing is sent before you choose. The first time you come back from the game, the launcher asks once, with *Send* and
*Don't Send*, and *See What Is Sent* shows the exact text that would go. You can change the answer at any time in
**Settings → Session Reports**, which says *Not Asked Yet* until you answer.

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
| `display.output`, `display.render_scale`, `display.fullscreen`, `display.game_aspect` | The output resolution, the render scale, fullscreen or window, the aspect ratio | `1920x1080`, `game`, `true`, `16:9` |
| `settings.preset`, `settings.upscaler`, `settings.taa`, `settings.dlss`, `settings.framegen`, `settings.fps_limit`, `settings.vk_present` | The main graphics options. A value that is not one of the option's own choices is sent as `other` | `null`, `fsr`, `dlss`, `dlaa`, `off`, `60`, `fifo` |
| `session.duration_s`, `session.frames`, `session.end` | How long the game ran, how many frames it showed, how it ended: `normal`, `fatal`, `device_lost` or `crash` | `1834`, `109800`, `normal` |
| `frametime.fps_mean`, `frametime.fps_low_1pct`, `frametime.worst_ms`, `frametime.over_25ms`, `frametime.over_100ms` | The average frame rate of the whole session, its 1% low, the slowest frame, and how many frames took over 25 ms and over 100 ms | `59.9`, `52.1`, `88.4`, `14`, `0` |
| `startup.shader_prewarm_ms`, `startup.shaders_translated`, `startup.pipelines_built`, `startup.pipeline_build_ms` | The time spent preparing shaders and pipelines at the start, when there was any | `5400`, `120`, `900`, `3100` |
| `fatal` | The game's error line, when the session ended with one, with your folders written short as in the log ([log privacy](log-privacy.md)), at most 300 characters | `null` |

A field the game does not know is `null`. When the game crashed so hard that it could not write the report, the
launcher makes one from the error line in the log: only `version`, `game_version`, `os.family`, `session.end`
(`crash`) and `fatal`, every other field `null`.

With the report the launcher sends:

- `install_id`: a random number made by the launcher the first time it is needed and kept in its settings. It is not
  made from anything on your PC; it only lets the reports of one installation be counted together and be deleted
  together.
- `launcher_version`: the launcher's version.

The server adds the **country** the report came from, as two letters (`BR`, `DE`), which Cloudflare gives for each
request, and the time it arrived.

## What a comment holds

- Your text, up to 4000 characters, **exactly as you wrote it**. Leave out your e-mail, name or anything else you do
  not want to send.
- The report of your last session, as above, when there is one, unless **Session Reports** is *Off*.
- If you tick *Attach the Last Log Lines*: the last 200 lines of `logs/nx2saria.log`, at most 24 KB, with your folders
  written short.
- The same `install_id`, `launcher_version`, country and time.

Sending a comment does not need session reports to be on: pressing *Send* is the choice.

## What is never sent

- Your IP address is not stored. The server reads the country Cloudflare gives and nothing else about the connection.
- No files: no saves, no settings file, no whole log, no game files, no keys.
- No account name, computer name, folder name, e-mail or hardware serial number. A report with a field the format
  does not have is refused.

## How long it is kept

Reports and comments are deleted 180 days after they arrive. Only the project's maintainer can read them, through a
page protected by a secret key: counts by version, graphics card, system and options, frame rates, the most common
errors and the comments.

## Deleting your data

**Settings → Session Reports → Delete My Data** deletes every report and comment sent with your `install_id` from the server, then makes
a new `install_id`. It works whether session reports are on or off. The server gives the same answer whether it held
anything or not.

## Turning it off

Set **Settings → Session Reports** to *Off*. If the project ever stops collecting, the server answers so and the
launcher stops sending until its next version.
