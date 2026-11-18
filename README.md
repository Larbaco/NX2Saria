<h1 align="center">
  <img src="docs/images/nx2saria-banner.png" alt="NX2Saria" width="1200">
</h1>

<p align="center">
  <strong>Ocarina of Time, recompiled into a native PC game</strong><br>
  Windows &amp; Linux · x86-64 · Vulkan
</p>

<p align="center">
  <a href="https://github.com/Larbaco/NX2Saria/releases/latest"><img alt="Latest release" src="https://img.shields.io/github/v/release/Larbaco/NX2Saria?label=release"></a>
</p>

<!-- languages -->
<p align="center"><strong>English</strong> · <a href="README.pt-BR.md">Português (Brasil)</a> · <a href="README.ru.md">Русский</a> · <a href="README.zh-Hans.md">简体中文</a></p>
<!-- /languages -->

<p align="center">
  <a href="docs/install.md"><strong>Install</strong></a> ·
  <a href="https://github.com/Larbaco/NX2Saria/releases">Releases</a> ·
  <a href="docs/controls.md">Controls</a> ·
  <a href="docs/known-issues.md">Known issues</a> ·
  <a href="docs/building.md">Build from source</a>
</p>

**NX2Saria** turns *The Legend of Zelda: Ocarina of Time* for Nintendo Switch 2 (game update **1.1.1**) into a native
Windows and Linux game. The game's code is statically recompiled to x86-64 and drawn through Vulkan: no emulator runs
underneath.

It is for players who own the game and can dump it from their own console. **To start, download the Windows setup or
the Linux AppImage from the [latest release](https://github.com/Larbaco/NX2Saria/releases/latest) and follow
[the install guide](docs/install.md).** The launcher builds the game on your PC from your dump, then keeps it up to
date.

> [!IMPORTANT]
> **Bring your own game.** No game files are included. You need your own legally obtained copy, dumped from your
> own console. Instructions for obtaining those files are not provided. The finished build contains material
> from your game: keep it for your own use and do not distribute it.

## Why NX2Saria

- **Install once, get updates in the launcher.** The setup and the AppImage build the game from your dump. The
  launcher offers each new release (*Stable* by default, *Preview* for pre-releases) and keeps your saves, mods and
  settings. [Install and update →](docs/install.md)
- **Linux with the full picture.** AMD FSR 3.1 upscaling and FSR 3.1 frame generation run through the game's own
  shader passes, so Linux needs no AMD library, on any GPU whose Vulkan driver runs them. On the RX 6700 (RADV) it was
  tested on, frame generation cost about 4 ms of GPU time a frame at 3440x1440. [Linux setup →](docs/README-BuildKit.md#linux)
- **Ultrawide that uses the whole screen.** On 21:9 and 32:9 the pause, map and system menus fill the screen, or stay
  in the middle with black sides if you prefer. The HUD can move its hearts, items and minimap out to the screen's
  corners. [HUD and menus on ultrawide →](docs/wide-hud.md)
- **Map and item trackers, live.** [Ocarina Atlas](docs/ocarina-atlas.md) 2.3.0 and later follows Link on the map and
  ticks off collectibles as you play; [hyrule-tracker](docs/hyrule-tracker.md) support is in an open pull request on
  its side. The tracker API is read-only, reachable only from your PC, and off until you set `tracker_api = 1`.
  [Tracker API →](docs/tracker-api.md)
- **Smoother while shaders are still being prepared.** After an update or a driver update, background pipeline builds
  wait for the light moments (menus, loads) instead of competing with play. In the Kokiri Forest at 1280x720 with an
  empty driver cache, the average went from about 25 to 27-28 fps and the slowest 1% of frames from 64-70 ms to 59 ms.
  Fewer crashes and wrong pictures when the window changes size or the driver refuses a setting.
  [Details →](docs/changelog/v0.15.0.md)
- **Amiibo without the fuss.** When the game asks for an amiibo, a list of your own figure dumps opens over the game.
  Each figure keeps its data, write-back is optional, and the daily limit can be switched off.
  [Amiibo guide →](docs/amiibo.md)
- **Settings you apply when ready.** Menu changes preview at once and are saved only when you choose **Apply**.
  **Ctrl+F5** reloads `nx2saria.ini` while the game runs, and rows the game version ignores are hidden.
- **Community fixes, gathered.** Fixes and speed-ups from the wider community, such as AMD texture fixes, video memory
  kept within budget and contact shadows, are merged in and credited to their authors in the
  [release notes](docs/releases/).
- **Made for testing.** `boot_map` starts the game straight in an area from a save, without the title screen
  ([boot to a map](docs/boot-to-map.md)); `tools/bench.py` measures scenes the same way twice
  ([performance](docs/performance.md)); recent changes write what they do to the game's log, so a report shows
  whether they are on and working.

## On any PC

- **Frame rate:** 60 fps by default, or a limit from 30 to 240, the display's refresh rate or unlimited.
- **Widescreen:** the window's shape, 21:9 or 32:9. Movies stay at 16:9; the HUD and menus can too.
- **Image quality:** resolutions from 1080p through 4K and beyond, upscaling, anti-aliasing, frame generation and HDR.
- **Controls:** keyboard, mouse and gamepads, remapping, rumble and gyro.
- **In game:** **F1**, or **Minus + Plus** on a gamepad, opens the settings; **F10** shows the performance overlay.
  [Controls and menu →](docs/controls.md) · [Every setting →](nx2saria.ini.example)

<details>
<summary><strong>Graphics options by platform</strong></summary>

- **Upscaling and anti-aliasing:** NVIDIA DLSS / DLAA on RTX GPUs, AMD FSR 3.1 on any GPU that runs it, AMD FSR 4 on
  supported Radeon GPUs on Windows, or FSR 1 and bilinear scaling.
- **Frame generation:** NVIDIA DLSS on RTX 40 / 50 GPUs (on Linux it needs NVIDIA's separate DLSS-G library and DLSS
  upscaling on at startup). AMD FSR 3.1 uses AMD's library on Windows; on Linux the game runs its passes itself, 2x and
  SDR only, with any upscaling choice.
- **HDR:** the game's native HDR or an enhanced tone-mapped picture, with display, game and HUD brightness controls.
  Enhanced HDR falls back to SDR for the run when the display format it needs is not available.

What is offered depends on the platform, GPU, driver and libraries. [The build kit guide](docs/README-BuildKit.md)
covers setup; [the architecture guide](docs/architecture.md) describes the renderer.

</details>

## Getting started

### Requirements

- **Game:** your own `.nup` dump of the Nintendo Switch 2 edition with update **1.1.1**. The build checks the version;
  do not assume another update is compatible.
- **CPU and memory:** x86-64 with SSE4.2, 16 GB of RAM or more. On 16 GB, read the
  [memory and page file guidance](docs/README-BuildKit.md#playing) before playing.
- **Storage:** about 34 GB free for the game, besides the dump; the Windows launcher and its tools take about 1.5 GB more.
- **Graphics:** Vulkan 1.3 with `VK_EXT_external_memory_host` and `VK_EXT_mesh_shader`, and a current driver.
- **System:** Windows 10 or later, or Linux with kernel 6.7 or later.

### Install

Every [release](https://github.com/Larbaco/NX2Saria/releases) has `NX2Saria-Setup-<version>.exe` for Windows and
`NX2Saria-Linux-<version>-x86_64.AppImage` for Linux. [The install guide](docs/install.md) has the steps, including
how a launcher installed from NX2navi switches to NX2Saria's releases.

### Build from source

[Building from source](docs/building.md) sets up the toolchain and a checkout; [the build kit guide](docs/README-BuildKit.md)
builds from a prepared kit. On Linux, once [the packages](docs/README-BuildKit.md#linux) are installed:

```sh
python3 -I tools/make_game.py --nup "/path/to/your-game.nup" --out "$HOME/Games/Ocarina"   # add --jobs 4 on 16 GB
"$HOME/Games/Ocarina/ocarina"
```

## Status and known issues

**In development.** The game is playable, but graphics, stability and performance still vary by scene, hardware and
driver.

- The first start after an install or a driver update, and the first visit to an area, can pause while shaders and
  pipelines are built.
- A 16 GB system can run out of memory in Hyrule Field: see the [memory guidance](docs/README-BuildKit.md#playing).
- Some integrated AMD GPUs can fail at the title screen because of host memory heap limits.
- Visual bugs and crashes remain, including a rare crash in the game's particle code. Long Linux sessions and Steam
  Deck hardware are still being validated ([Linux notes](docs/building.md#linux-in-progress)).

[Known issues](docs/known-issues.md) lists the open problems of the current release and what avoids them.

**Found a bug?** Open an issue on the [issue tracker](https://github.com/Larbaco/NX2Saria/issues) and attach
`logs\nx2saria.log` (`logs/nx2saria.log` on Linux) from the game folder. Say which release or commit, OS, GPU and
driver you use and how to reproduce it; [CONTRIBUTING.md](CONTRIBUTING.md) has the full list. Check attachments for
private paths, and never upload game files or keys.

## Documentation

| Topic | Guides |
|---|---|
| Playing | [Install and update](docs/install.md) · [Controls](docs/controls.md) · [Settings](nx2saria.ini.example) · [Amiibo](docs/amiibo.md) · [Known issues](docs/known-issues.md) |
| Trackers | [Tracker API](docs/tracker-api.md) · [Ocarina Atlas](docs/ocarina-atlas.md) · [hyrule-tracker](docs/hyrule-tracker.md) |
| Releases | [Release notes](docs/releases/) · [Changelogs](docs/changelog/) |
| Building | [Build kit](docs/README-BuildKit.md) · [From source](docs/building.md) · [Launcher](docs/launcher.md) · [Installer](docs/installer.md) |
| Testing | [Performance](docs/performance.md) · [Boot to a map](docs/boot-to-map.md) |
| Contributing | [Contribution guide](CONTRIBUTING.md) · [Architecture](docs/architecture.md) · [Maxwell renderer notes](docs/maxwell.md) |

**Names.** The settings file is `nx2saria.ini`, environment variables start with `NX2SARIA_` and release files with
`NX2Saria-`. Settings, saves and profiles from NX2navi keep working; [names and compatibility](docs/branding.md)
lists the older names.

**Game content.** Build outputs stay in the ignored `build/` and `generated/` folders, and the runtime's shaders are
kept as source. Run `python tools/check_content.py` before committing.

## Credits and origins

NX2Saria is maintained by **[Larbaco](https://github.com/Larbaco)** as an independent project. It is a fork of
**[NX2navi](https://github.com/tangerineturtle1/NX2navi)**, whose recompilation pipeline, runtime, renderer and
launcher are its foundation: credit for them belongs to NX2navi's authors. Changes from community contributors are
credited in the release notes, and every contribution keeps its author in the commit history.

Thanks to the [Triforce project](https://git.triforceproject.com/triforce/source) for its ideas: the design of its
native frame generation shaped NX2Saria's FSR 3.1 frame generation on Linux, and its ideas are behind the play gate,
the pipeline cache stamp and the presentation checks. None of its code is used here.

The NX2Saria name and artwork identify this project. They do not imply an official NX2navi release or a Nintendo
project. Some file and setting names keep the NX2navi spelling on purpose, for compatibility;
[names and compatibility](docs/branding.md) lists them.

## License and notices

The source is public to read. Its additions and changes are all rights reserved
([LICENSE-ADDITIONS](LICENSE-ADDITIONS)): no part of them may be copied or published without written permission.
The base import stays under its [MIT License](LICENSE), and third-party code keeps its own license
([THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)). No license grants anything for Nintendo's game.

NX2Saria is an independent, non-commercial fan project, not affiliated with or endorsed by Nintendo.
*The Legend of Zelda*, *Ocarina of Time* and *Nintendo Switch* are trademarks of Nintendo.
