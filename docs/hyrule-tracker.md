# hyrule-tracker and NX2Saria

[hyrule-tracker](https://github.com/jv-prado/hyrule-tracker) is a tracker for this game. This page records what it
takes to use it with NX2Saria: how it reads the game, what breaks here, what the fork and the tracker would change, and
how it shares one game-state API with the map tracker studied in [ocarina-atlas.md](ocarina-atlas.md) (branch
`atlas-integration`).

Status: in progress. The sections below are filled as each part of the study is done.

## Plan

1. How hyrule-tracker gets the game's data: transport, protocol, addresses and structures, language, platforms,
   license.
2. Where each piece of data it needs is in NX2Saria (game version 1.1.1, main build `e0f3ccfa`): the game-data
   manager's flags by name hash, their kinds and the read recipe for each.
3. What works as it is, what breaks, and what the fork and the tracker change.
4. One game-state API for both trackers: the tracker API of `atlas-integration` (`runtime/tracker_api.c`, the debug
   protocol on `127.0.0.1:6545`), extended with what this tracker needs, instead of a second server.
5. Pull request drafts, licenses, the expected log lines, and the checklist for the first run.

The game is not built or run for this study.
