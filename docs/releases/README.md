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
2. **Tag `vX.Y.Z` (or `vX.Y.Z-rc.N`) on that commit, pushed over SSH.** The push starts
   `.github/workflows/release.yml`:
   - `check`: the tag names `VERSION`'s version, its notes exist (without a pre-release line when there is no
     suffix), and its commit is on `main`. A tag on a commit outside `main` publishes nothing.
   - `windows` and `linux`: `windows-release.yml` (setup, app zip, runtime zip, both manifests, with the notes of the
     tag's commit) and `linux-appimage.yml` (the AppImage), built from that commit with read access only and no cache.
   - `attest`: the files are exactly the release's, the manifests give their real sizes and SHA-256 and carry the
     tag's notes; it writes `SHA256SUMS.txt` and a provenance attestation (`actions/attest-build-provenance`) over
     all seven files. Only this job can sign attestations.
   - `publish`: the only job that can write to the repository. It uploads the attested files to a draft, then
     publishes the draft as a **pre-release**, whether or not the tag has a suffix. The release page is the notes with
     a pre-release line on top and a line that links to "Verify your download" at the end
     (`tools/release_files.py body`).
3. **Stable:** once the owner has tried a version without a suffix, `gh workflow run release-promote.yml -f
   tag=vX.Y.Z` drops the page's pre-release line and marks the release stable and Latest. It uploads nothing: every
   file and its attestation stay as built. Release candidates stay pre-releases.

If a run fails, it leaves at most a draft. "Re-run failed jobs" on that run, or `gh workflow run release.yml --ref
vX.Y.Z` (started on the tag itself, so that the attestations name the tag and its commit; started on a branch it
stops), makes the release again, and the draft is replaced. A published release is never changed: started on one, the
workflow stops unless the release already holds exactly the files it built, and a fix goes out as the next version
(rc.N+1 or a patch), as the version rules below say. When the notes of a manifest must change, that is a new version
too. Release immutability (Settings, General, Releases) is to be turned on once the first release of this flow has
published with every attestation verifying; GitHub then refuses any change to a published release's files and tag.

The release workflow runs the `release.yml` of the tagged commit, and `check` refuses a commit outside `main`. A
repository ruleset on `refs/tags/v*` that lets only the owner create tags, and nobody update or delete them, keeps a
tag from pointing anywhere else. Players check the tag with `--source-ref` ("Verify your download").

`windows-release.yml` and `linux-appimage.yml` can be started by hand for a test build of a branch or a tag: they
publish nothing, have no write access and keep their files as the run's artifact for 14 days.

The workflows use only the run's `GITHUB_TOKEN`, with `permissions: contents: read` at the top of each file and more
only in the job that needs it; every action is pinned by commit SHA with its version in a comment, and downloads of
other projects' files are pinned by SHA-256 (AMD's FSR libraries, NVIDIA's DLSS and Streamline, appimagetool and the
AppImage runtime). The repository runs no workflow on `pull_request_target`, and no release job reads a cache.

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
