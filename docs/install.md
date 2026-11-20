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
3. The build needs a compiler and a few libraries from your distribution. When one is missing, the AppImage says which
   and the command that installs them. On Ubuntu 24.04, Linux Mint 22 and the other distributions on Ubuntu:

   ```sh
   sudo apt install clang lld libopus-dev libvpx-dev libdav1d-dev libzstd-dev libvulkan-dev
   ```

   SDL3, which Ubuntu 24.04 does not package, comes with the AppImage.

On first start, the AppImage installs the launcher and its Python runtime in `~/.local/share/nx2saria`, then opens the
installed launcher. The Applications menu entry points to that installed copy, so you can move or delete the AppImage
afterward. Keep it if you want to repair or reinstall the launcher. If you used the earlier AppImage window, the new
launcher adopts the dump and game folder recorded in `~/.config/nx2saria/appimage.json` (also the older `nx2navi`
location) without moving either folder.

## Updating

- **The launcher offers each new release by itself**, from NX2Saria 0.12.1 on.
- **A launcher installed from the original NX2navi project** keeps the update source it was installed with. Set
  `"feed_repo": "Larbaco/NX2Saria"` in its `data/launcher.json` once, and it offers NX2Saria's releases from then on.
- **Your game folder, saves, profiles, mods and `nx2saria.ini` stay where they are** when you update.
- **Pre-releases** reach only the launchers whose *Update Channel* setting is *Preview*. On *Stable*, the default, the
  launcher waits until a release is marked stable.

## Verify your download

From NX2Saria 0.19.0-rc.1 on, every file of a release is built and uploaded by GitHub Actions, with no file passing
through anyone's PC. Each file has a **provenance attestation**: a record signed by GitHub that says which repository,
workflow and commit built it. If a file had been changed after the build, its attestation would not match it.

**Check the attestation** with the [GitHub CLI](https://cli.github.com/) (`gh`, signed in to any GitHub account), in
the folder you downloaded to:

```sh
gh attestation verify NX2Saria-Setup-0.19.0.exe --repo Larbaco/NX2Saria \
  --signer-workflow Larbaco/NX2Saria/.github/workflows/release.yml --source-ref refs/tags/v0.19.0 \
  --deny-self-hosted-runners
```

(On Windows, in PowerShell, write it on one line without the `\`.)

Put the name of the file you downloaded in place of `NX2Saria-Setup-0.19.0.exe`, and its version in
`refs/tags/v0.19.0`: it works the same for the AppImage, the Windows and Linux app and runtime zips, all three update
manifests, and `SHA256SUMS.txt`. For a good file it prints `✓ Verification succeeded!` and the workflow
that built it. Any other answer means the file is not the one GitHub Actions built for that version: do not run it,
and tell us in an issue.

**Check the checksums.** `SHA256SUMS.txt` on the release lists the SHA-256 of every other file. It shows the download
arrived whole; together with the attestation of `SHA256SUMS.txt` itself, it also shows where every file came from.

- Linux: `sha256sum -c --ignore-missing SHA256SUMS.txt` prints `OK` for each file you downloaded.
- Windows (PowerShell): `Get-FileHash NX2Saria-Setup-0.19.0.exe` prints the SHA-256; it must be the one on that
  file's line of `SHA256SUMS.txt` (the case of the letters does not matter).

The launcher checks each update by itself the second way: Windows downloads `NX2Saria-update.json`; Linux downloads
`NX2Saria-update-linux.json`. Each platform reads only its own manifest and installs an app or runtime file only when
its size and SHA-256 match. Releases older than 0.19.0-rc.1 were uploaded by hand and have no attestation.

## Something wrong?

[Known issues](known-issues.md) lists the problems we know of and what avoids them. For anything else, open an issue
at https://github.com/Larbaco/NX2Saria/issues and attach `logs\nx2saria.log` (`logs/nx2saria.log` on Linux) from the
game folder.

[The launcher guide](launcher.md) and [the installer guide](installer.md) describe how both work in detail.
