# Launcher installation inspection

`launcher/inspect_installation.py` `inspect_installation()` reads the selected launcher's settings without creating a normal `Context`, which would create directories and write logs and can copy malformed settings. It returns only the root, selected game path and status, never the settings dictionary or its secrets. It uses `build.installed()` for the existing finished-build and interrupted-rebuild rules. Its command-line entry point accepts an explicit root and prints one JSON object.

This is an initial inspection slice. Running-game detection, interrupted-job reporting, a persistent worker and Electron integration remain unimplemented. A `ready` response reflects the existing installation check, not proof that the game has run.

`launcher-electron/src/inspection.ts` `inspect()` starts the inspection command with a bounded timeout and output size and validates its response before returning a limited result. `launcher-electron/src/main.ts` lets the user select the root using the native dialog; its preload exposes only that operation. This development slice does not own the launcher lock and performs no game operations. It is not the persistent worker described in the plan.

`inspect_installation()` reports the presence of a job marker independently of the installation status. A marker alone cannot distinguish active work from an interrupted job, so the interface must not claim that it does. On Linux, an observed executable in `/proc` confirms that the game is running; no match leaves the result unknown because process visibility may be restricted. On Windows, `launcher/platform/windows.py` `game_running()` reads a process snapshot and process image paths without changing the executable; a match is confirmed only when the path and, when supplied, owner ancestry match. Failure to observe a match remains unknown.

`launcher-electron/src/worker-client.ts` starts a persistent worker, validates its handshake, serializes requests and
tracks replies and sequenced game events. Invalid frames or timed-out requests disconnect it and do not replay a
mutation. `main.ts` validates inspection and Play responses and uses the worker to supervise the game. The worker
owns the launcher lock, reports waiting updates without applying them and continues following a game after the window
closes. The mod rebuild path and full platform validation remain incomplete.

`launcher-electron/ui-tests/worker-synthetic.cjs` exercises inspection, two synthetic games, hide and restore,
reselection, cancellation that disables Play, refusal while a job marker is present and closing the window during a third game, which finishes while the worker records its end. It also kills the worker during a synthetic game and checks that the window returns, Play is disabled and the game finishes. The folder dialog is stubbed. It uses temporary files and a fake
executable, and is not a real-game test.

`launcher-electron/src/discovery.ts` computes standard roots without writing: on Linux `$XDG_DATA_HOME` when absolute, otherwise `~/.local/share`, with `nx2saria` before `nx2navi`; on Windows `%LOCALAPPDATA%/Programs`, with `NX2Saria` before `NX2navi`. A candidate is recognized by `app/current.txt`; inspection decides whether the game is configured. The Windows lookup uses the ordinary LOCALAPPDATA location and does not yet query a redirected UserProgramFiles known folder. The UI waits for discovery before enabling selection and keeps manual selection available.

Windows process observation uses a read-only Tool Help snapshot and `QueryFullProcessImageNameW` with limited query access. Handles are closed after use. Process ancestry and image paths are observations subject to permissions, process exit and PID races; a missing match is not treated as proof of absence.

`launcher-electron/src/runtime-paths.ts` `runtimePaths()` uses the bundled Python and backend under Electron resources in a packaged build, and preserves the development interpreter override in a checkout. The Windows development ZIP ships Python bytecode rather than project Python source; the embedded Python search path includes the resources directory.
