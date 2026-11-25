<!-- languages -->
**English** · [Português (Brasil)](pt-BR/known-issues.md) · [Русский](ru/known-issues.md) · [简体中文](zh-Hans/known-issues.md)
<!-- /languages -->

# Known issues

Problems we know of in the current release, and what avoids them. When one is fixed, it leaves this list and the
release notes say so. The [README](../README.md#status-and-known-issues) has the general state of the project.

## Graphics

- **AMD FSR 3.1 and FSR 4 can show visual glitches on Windows**: black or dark patches and flicker while the camera or
  Link moves when the scene is rendered smaller than the picture, and trails behind moving characters. Bilinear
  upscaling, or the scene at the window's size with no upscaler, avoids the patches. Reported to the original project
  as tangerineturtle1/NX2navi#794, tangerineturtle1/NX2navi#475 and tangerineturtle1/NX2navi#736.
- **FSR 4 on Radeon RX 6000 (RDNA 2)**: AMD's driver offers FSR 4 on these cards and it runs, but it costs far more GPU
  time than on newer cards (about 12 ms a frame at 1440p on an RX 6750 XT). FSR 3.1 is the lighter choice there.
- **On Linux with Mesa 26.2 or 26.3, Radeon cards with mesh shaders (RX 6000 and newer) draw part of the scene through
  the mesh fallback.** The task shaders of those Mesa releases have a fault that ended the game with "the GPU device
  was lost" seconds into a session (seen on an RX 6800S). The game takes the mesh fallback there by itself: grass and
  other such objects cost some frame rate, and appear a moment late the first time they are seen in a run. The log's
  `vk: mesh path:` line says when it does. No Mesa release has the fix yet; when one does, a later version of the game
  draws natively on it again. `mesh_fallback = 0` in `nx2saria.ini` draws natively on any Mesa.
- **On Linux, the game can end with "the GPU device was lost" on Radeon RX 400 and RX 500 cards.** Session reports
  show it on RX 480 and RX 580 with Mesa 26.2 and 26.3, seconds or minutes into a session, and far more often at 45
  frames per second and above than at 30. The cause is not known yet. *Frame rate limit* at 30 makes it rarer. If it
  happens to you, please open an issue with `logs/nx2saria.log` and the kernel's lines after the crash
  (`journalctl -k -b`).

## Frame generation

- **On Linux it is 2x and SDR only.** With HDR on, the menu row says it is off, and why.
- **Fast camera turns close to a wall smear at the edges** in generated frames, and so does Navi's glow. This is usual
  for frame generation when the game renders around 40 fps. A higher rendered frame rate makes it smaller.

## Shaders and pipelines

- **The first start after an install, an update or a driver update, and the first visit to an area, can pause** while
  shaders and pipelines are built. Later starts read them from the caches.
- **On NVIDIA, an update or the first start keeps building the shaders again** (the update stays on "Built ... shaders
  for your graphics card" for many minutes, or every start pauses as after an install) when the driver's shader cache
  is full: the driver then drops older shaders to make room, and the game builds them again. In the NVIDIA Control
  Panel, under Manage 3D settings, set **Shader Cache Size** to 10 GB or Unlimited.
- **A game ended by force** rather than closed may leave the pipeline cache unwritten after the cache was renewed. The
  next run writes it instead.

## Map trackers

- **On the title screen, Ocarina Atlas searches the game's memory** for Link until a save is loaded. The tracker API
  holds this to about two seconds of reading a minute, and the log says once when the limit is reached.

## Reporting a problem

Open an issue at https://github.com/Larbaco/NX2Saria/issues and attach `logs\nx2saria.log` (`logs/nx2saria.log` on
Linux) from the game folder.
