# Launcher installation inspection

`launcher/inspect_installation.py` `inspect_installation()` reads the selected launcher's settings without creating a normal `Context`, which would create directories and write logs and can copy malformed settings. It returns only the root, selected game path and status, never the settings dictionary or its secrets. It uses `build.installed()` for the existing finished-build and interrupted-rebuild rules. Its command-line entry point accepts an explicit root and prints one JSON object.

A `ready` response reflects the existing installation check. The standalone entry point is used by Python
characterization tests. Electron sends `inspect` over the persistent worker pipe and validates its reply with
`parseInspection()`. The TypeScript module no longer contains a short-lived inspection subprocess.

`inspect_installation()` reports the presence of a job marker independently of the installation status. A marker alone cannot distinguish active work from an interrupted job, so the interface must not claim that it does. On Linux, an observed executable in `/proc` confirms that the game is running; no match leaves the result unknown because process visibility may be restricted. On Windows, `launcher/platform/windows.py` `game_running()` reads a process snapshot and process image paths without changing the executable; a match is confirmed only when the path and, when supplied, owner ancestry match. Failure to observe a match remains unknown.

`launcher-electron/src/worker-client.ts` starts a persistent worker, validates its handshake, serializes requests and
tracks replies and sequenced game events. On Windows the worker is started detached so the Electron process can exit while the worker finishes supervising a game. Invalid frames or timed-out requests disconnect it and do not replay a
mutation. `main.ts` validates inspection and Play responses and uses the worker to supervise the game. The worker
owns the launcher lock, reports waiting updates without applying them and continues following a game after the window
closes. The worker uses the shared mod rebuild job when Play requires rebuilding mod code.

`renderer.ts` shows the opening status as soon as Play is clicked and again when a required mod build resumes Play.
`main.ts` waits for the worker's same-session `game_window_ready` event before allowing the saved hide preference to
hide the launcher. The renderer then shows the game as running. A startup error replaces the opening status even
when the game never reported focus.

`launcher-electron/ui-tests/worker-synthetic.cjs` exercises inspection, two synthetic games, hide and restore,
reselection, cancellation that disables Play, refusal while a job marker is present and closing the window during a third game, which finishes while the worker records its end. It also kills the worker during a synthetic game and checks that the window returns, Play is disabled and the game finishes. The folder dialog is stubbed. It uses temporary files and a fake
executable, and is not a real-game test.

`launcher-electron/src/discovery.ts` computes standard roots without writing: on Linux `$XDG_DATA_HOME` when absolute, otherwise `~/.local/share`, with `nx2saria` before `nx2navi`; on Windows `%LOCALAPPDATA%/Programs`, with `NX2Saria` before `NX2navi`. A candidate is recognized by `app/current.txt`; inspection decides whether the game is configured. The Windows lookup uses the ordinary LOCALAPPDATA location and does not yet query a redirected UserProgramFiles known folder. The UI waits for discovery before enabling selection and keeps manual selection available.

Windows process observation uses a read-only Tool Help snapshot and `QueryFullProcessImageNameW` with limited query access. Handles are closed after use. Process ancestry and image paths are observations subject to permissions, process exit and PID races; a missing match is not treated as proof of absence.

`launcher-electron/src/runtime-paths.ts` resolves the runtime and the active app from the chosen installation in a
packaged build. It keeps the explicit development interpreter override in a checkout. The Windows ZIP carries
CPython 3.14 worker bytecode, and the bootstrap adds the active app's tools to its imports.

`launcher-electron/ui-tests/mods-synthetic.cjs` uses a fake mod rebuild job and a temporary executable to exercise
successful resumption, cancellation and the following Play without repeatedly rebuilding the same failed mods.
It does not compile or run the real game.

`launcher-electron/src/dev-log.ts` writes short records to `data/logs/launcher-electron.log` once an existing
installation with a logs directory is recognized. The build step writes the checkout commit to
`dist/build-id.txt`. The main process records startup, selection, inspection, Play results, window visibility,
lock refusal and normal exit. The process ID distinguishes instances using the same file. Startup logs the
build hash. Selection records also include the version in the worker handshake when available. After changing
its Play status text, the renderer acknowledges the message kind for that result. The main process logs one
acknowledgement from the current window, with no message text. A confirmed game end shows the worker's
existing end sentence when there is an error. Failure before spawn and loss of monitoring after spawn report
separate outcomes. Selection records
identify the origin and brand with fixed values. They and all other records omit paths and dialog or error
text, even if the Python path shortening switch is off. Appending the compact normal exit record
synchronously lets it finish before Electron quits.
