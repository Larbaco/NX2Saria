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
