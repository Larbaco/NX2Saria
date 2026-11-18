<!-- languages -->
**English** · [Português (Brasil)](pt-BR/install.md) · [Русский](ru/install.md) · [简体中文](zh-Hans/install.md)
<!-- /languages -->

# Installing and updating NX2Saria

Every release on the [Releases page](https://github.com/Larbaco/NX2Saria/releases) has a Windows setup and a Linux
AppImage. Neither carries any game data: both build the game on your PC from your own `.nup` dump of the Nintendo
Switch 2 edition with update 1.1.1. The [README](../README.md#requirements) lists what your PC needs.

## Windows

1. Download **`NX2Saria-Setup-<version>.exe`** from the release.
2. Put it next to your `.nup` dump and start it.
3. Windows SmartScreen warns on the first start because the setup is not signed: choose *More info*, then *Run
   anyway*.

The setup installs the launcher, which builds the game once and then starts it.

## Linux

1. Download **`NX2Saria-Linux-<version>-x86_64.AppImage`** from the release.
2. Make it executable (`chmod +x NX2Saria-Linux-*.AppImage`) and start it.

Started on a desktop, it opens a window to install, update and play.

## Updating

- **The launcher offers each new release by itself**, from NX2Saria 0.12.1 on.
- **A launcher installed from the original NX2navi project** keeps the update source it was installed with. Set
  `"feed_repo": "Larbaco/NX2Saria"` in its `data/launcher.json` once, and it offers NX2Saria's releases from then on.
- **Your game folder, saves, profiles, mods and `nx2saria.ini` stay where they are** when you update.
- **Pre-releases** reach only the launchers whose *Update Channel* setting is *Preview*. On *Stable*, the default, the
  launcher waits until a release is marked stable.

## Something wrong?

[Known issues](known-issues.md) lists the problems we know of and what avoids them. For anything else, open an issue
at https://github.com/Larbaco/NX2Saria/issues and attach `logs\nx2saria.log` (`logs/nx2saria.log` on Linux) from the
game folder.

[The launcher guide](launcher.md) and [the installer guide](installer.md) describe how both work in detail.
