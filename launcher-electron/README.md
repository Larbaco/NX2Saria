# Electron launcher development test

This development build uses a persistent Python worker for inspection, Play and mod rebuilds. It requires an existing
launcher installation and game. It does not install or update the launcher, send session reports or ask for consent.
Waiting updates are shown but must be applied with the installed launcher. Crash-log offers remain a future parity item.

On Linux, from this directory, run `npm ci` and then `npm start`. Set `NX2SARIA_PYTHON` to a Python interpreter able to
import the launcher dependencies if the default `python3` is not suitable. Select the existing launcher root containing
`data/launcher.json`, not the game folder. Only the owner should test with the real game.

For a manual test, check discovery and selection, Play twice, hide and restore when enabled, closing the window during a
game. The Windows ZIP uses Python 3.14 and the build tools from the selected installation. Use the current NX2Saria
installer for a first installation and the installed launcher to update or repair an older one. After testing, copy
`data/logs/launcher-electron.log`, `data/logs/launcher.log` and the game's log to the usual private log folder.
In this test build, only the last session's summary stays available for the installed launcher to send when it
next opens with session reports on.

`npm test` runs the TypeScript tests. On Linux, `xvfb-run -a npm run test:ui` opens Electron windows against temporary
installations and synthetic executables; it never runs the real game.
