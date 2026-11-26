# Release notes

`docs/releases/vX.Y.Z.md` is written for players. It is the text of the GitHub release, and the release workflow puts
it into the update manifest, which the launcher's *What's New* shows. Since 0.15.0, each release has two files:

- **`docs/releases/vX.Y.Z.md`, the notes (about 150 words at most).** What a player notices, in their words.
- **`docs/changelog/vX.Y.Z.md`, the details.** Option names, log lines, measurements and how each change works, for
  testers and for reading a report's log.

## The notes

1. **Three to five bullets first, one line each, with no line break inside one.** The launcher shows the first four to
   six lines of the file, one line per row (`launcher/feed.py` `plain_notes`). Say what changes for the player. Name a
   setting only when the player has to change it, and put it in backticks.
2. **Then plain lines that read well in the launcher too:** that saves, mods and settings stay, and for a pre-release
   what has not been tried yet. No links on these lines: the launcher shows the markdown as it is.
3. **Last, one line of links**, with full `https://github.com/Larbaco/NX2Saria/blob/main/...` addresses (relative links
   do not work on the release page): [install guide](../install.md), [known issues](../known-issues.md) and the
   release's changelog file.

   Keep that links line after the first six non-empty lines. `tools/release_files.py tag` checks this because
   launchers 0.18.x and 0.19.0-rc.x display only six lines and do not strip Markdown links.

Install steps and the known issues that carry from release to release are not repeated in the notes: they live in
`docs/install.md` and `docs/known-issues.md`. Update `docs/known-issues.md` in the release commit when a problem is
found or fixed. Its translations (`docs/<lang>/known-issues.md`) are brought up to date in the same commit where
possible; `python tools/docs_i18n.py status` lists the stale ones ([translating.md](../translating.md)).

**The notes of a version without a suffix have no pre-release line.** The update manifests carry the notes of the
tag's commit and never change after the build, and that version becomes the stable release later. The release page
says by itself that it is a pre-release for as long as it is one (see below); the launcher knows it from GitHub, not
from the text. `tools/release_files.py tag` refuses such a line, and the release workflow stops on it. The notes of a
release candidate may say it is a test build, since that stays true.

## How a release is made

Every release is made by GitHub Actions alone, so that no file of it passes through anyone's PC and players can check
that (`docs/install.md` "Verify your download"). Nobody uploads, replaces or edits a release file by hand.

1. **Release commit on `main`:** `VERSION`, `docs/releases/vX.Y.Z.md` and `docs/changelog/vX.Y.Z.md`, after the merge
   commits it ships.
2. **Tag `vX.Y.Z` (or `vX.Y.Z-rc.N`) on that commit, pushed over SSH, by itself:** GitHub starts no push event for a
   push of more than three tags at once. The push starts `.github/workflows/release.yml`:
   - `check`: the tag names `VERSION`'s version, its notes exist (without a pre-release line when there is no
     suffix), its commit is on `main` (a tag on a commit outside `main` publishes nothing), and the tag has no
     published release yet (when it has one, the run ends here, before any build).
   - `windows` and `linux`: `windows-release.yml` (setup, app zip, runtime zip, both manifests, with the notes of the
     tag's commit) and `linux-appimage.yml` (the AppImage), built from that commit with read access only and no cache.
   - `attest`: the files are exactly the release's, the manifests give their real sizes and SHA-256 and carry the
     tag's notes; it writes `SHA256SUMS.txt` and a provenance attestation (`actions/attest-build-provenance`) over
     all ten files. Only this job can sign attestations.
   - `publish`: the only job that can write to the repository. It checks the files against `SHA256SUMS.txt`, runs the
     players' `gh attestation verify` on each of the ten (with the tag's commit and GitHub's own runners besides),
     uploads them to a draft and publishes the draft as a **pre-release**, whether or not the tag has a suffix. The
     release page is the notes with a pre-release line on top (unless a release candidate's notes have their own)
     and a line that links to "Verify your download" at the end (`tools/release_files.py body`).
3. **Smoke test of what was published, on the Linux bench:** `tools/rc_smoke.py vX.Y.Z-rc.N --quick --detach` installs
   the tag's AppImage in a throwaway root, builds the game with the installed launcher, runs it to an area, opens the
   window, crashes the game once and checks that nothing was sent; without `--quick` it also updates from the last
   stable release. It writes `summary.md` beside its logs, and a FAIL is a finding about the release, fixed in the next
   version ([code/tools.md](../code/tools.md#toolsrc_smokepy)). The lines the notes promise go in
   `tools/data/rc-smoke/<version>.txt` before the run.
4. **Stable:** once the owner has tried a version without a suffix, `gh workflow run release-promote.yml -f
   tag=vX.Y.Z` (from `main`) drops the page's pre-release line and marks the release stable and Latest. It uploads
   nothing. It promotes only a release that GitHub Actions published, whose `SHA256SUMS.txt` carries `release.yml`'s
   attestation for that tag and lists exactly the release's files, and only when no stable release is as new.
   Release candidates stay pre-releases.

If a run fails, it leaves at most a draft. "Re-run failed jobs" on that run, or `gh workflow run release.yml --ref
vX.Y.Z` (started on the tag itself, so that the attestations name the tag and its commit; started on a branch it
stops), makes the release again, and the draft is replaced. A published release is never changed, and a fix goes out
as the next version (rc.N+1 or a patch), as the version rules below say. When the notes of a manifest must change,
that is a new version too.

`windows-release.yml` and `linux-appimage.yml` can be started by hand for a test build of a branch or a tag: they
publish nothing, have no write access and keep their files as the run's artifact for 14 days.

### Repository settings the owner turns on

None of these is on yet (2026-11-19: no ruleset, `main` unprotected, release immutability off).

- **Release immutability** (Settings → General → Releases → "Enable release immutability"), once the first release of
  this flow has published with every attestation verifying: GitHub then refuses any change to a published release's
  files and tag.
- **A tag ruleset** (Settings → Rules → Rulesets → New tag ruleset): target `refs/tags/v*`, enforcement Active, rules
  "Restrict creations", "Restrict updates" and "Restrict deletions", and in the bypass list only the owner (the
  Repository admin role). GitHub runs the `release.yml` of the tagged commit, so this keeps anyone else from making a
  release tag or moving one; players check the tag with `--source-ref` ("Verify your download").
- **`main` protected** (a branch ruleset on `main`, or Settings → Branches): "Require a pull request before merging",
  "Block force pushes" and "Restrict deletions", with the owner in the bypass list for release commits.

### What the release build takes, and how each is pinned

The workflows use only the run's `GITHUB_TOKEN`, with `permissions: contents: read` at the top of each file and more
only in the job that needs it. The repository runs no workflow on `pull_request_target`, and no release job reads or
writes a cache.

| Pinned by | What |
|---|---|
| Commit SHA (version in a comment) | every action: checkout, setup-dotnet, setup-uv, setup-python, upload- and download-artifact, attest-build-provenance |
| SHA-256 | AMD's FSR 3.1 and FSR 4 libraries (the workflow), NVIDIA's DLSS (`tools/dlss_pin.py`), the Streamline SDK (`tools/streamline_pin.py`), the Vulkan headers (`tools/vulkan_pin.py`), SDL3 (`tools/sdl3_pin.py`), python.org's embeddable Python 3.14.9 and Tcl/Tk (`tools/make_launcher.py` `PINS`), the MSYS2 packages of the release's toolchain lock, which the build resolves on its day and records with their SHA-256 (`launcher/toolchain.py`), uv 0.13.9 (setup-uv `checksum`), appimagetool 1.9.1, type2-runtime 20251108, LunarG's Vulkan SDK 1.4.363.1 (used only when Ubuntu's archive fails), Pillow 13.0.0 (`tools/ci_requirements_linux.txt`, `--require-hashes`), CPython 3.14.9's source archive and python-build-standalone 20261021's full archive, whose licence texts of the Python each release ships go into `licenses/python/` (`tools/python_licences.py`) |
| Version, checked by the pinned uv | the CPython 3.14.9 and 3.12 builds uv installs (uv checks each download against the SHA-256 it carries) |
| Version only | numpy 2.5.5 on Windows (`uv run --with` does not check a hash: pending), the .NET SDK `10.0.x` (the newest patch), the NuGet packages of the shader translator's build (the versions its projects name) |
| The runner's package manager, by name | the Linux runner's `glslc` and `spirv-tools` (Ubuntu's signed archive) and the Windows runner's MSYS2 `shaderc` and `spirv-tools` (MSYS2's signed repository), at their versions of the day; the runner images themselves |

## Version numbers

Since 0.19.0 (decided on 2026-11-19), `VERSION` follows these rules. NX2Saria stays at 0.x until the owner calls the
game complete and stable on Windows and Linux; that release is 1.0.0.

| Part | Goes up when | Examples |
|---|---|---|
| **0.X.0** (minor) | Something new, or something the player sees change: a feature, a setting, a different default. Also any update that makes players translate the shaders again or compile the whole game again. | the ultrawide HUD, the build by free memory, a shader translator change |
| **0.X.Y** (patch) | Fixes only: no new setting except a switch to turn the fix itself off, no default changed. | the AppImage on Ubuntu 24.04, the button prompts |
| **0.X.Y-rc.N** | A test build on the launcher's Preview channel before that version is stable. N counts from 1. The stable release is then made from the same code with the suffix removed (a new release commit and tag; it is built again by the release workflow). | 0.19.0-rc.1, 0.19.0-rc.2, then 0.19.0 |

- A release candidate is always a GitHub pre-release; a version without a suffix is published stable (Latest) once
  the owner has tried it on Windows. A fix found during the candidates goes into the next candidate, not into the
  stable number.
- The launcher orders versions as semver 2.0.0 does (`launcher/versions.py`): 0.19.0-rc.1 < 0.19.0-rc.10 < 0.19.0 <
  0.19.1-rc.1. Launchers from 0.18.7 and older compare the suffix as text, so keep N below 10 until most players are on
  0.19.0 or later.
- Each candidate has its own short notes (`docs/releases/v0.19.0-rc.1.md`) and changelog; the stable release's notes
  cover everything since the last stable one, because Stable-channel players skip the candidates.
