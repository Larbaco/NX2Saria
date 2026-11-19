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

When a pre-release becomes stable, take its pre-release line out of the notes, of the GitHub release and of both
update manifests (`NX2Saria-update.json` and `NX2navi-update.json`, with `SHA256SUMS-windows.txt` again).

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
