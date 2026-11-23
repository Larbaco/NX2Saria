# Electron launcher replacement plan

Status: overall plan approved by the owner; the first-slice behavioral contract is a design proposal. No production implementation or migration mechanism has been approved by this document.

Maintain `docs/electron-launcher-plan-visual.html` alongside this plan: update the visual page whenever this plan changes, preserving decisions, open questions and validation limits.

## Goal and decisions

Replace the existing launcher with a unified Electron and TypeScript interface, retaining the Python core initially. Windows and Linux are required; macOS is a future investigation and Electron support does not establish game support. Develop on a separate branch and regularly incorporate main, reviewing launcher changes for parity. The replacement is not ready to ship until the agreed functionality and platform gates below are met.

Keep existing NX2Saria and NX2navi installations working, including game paths, saves, profiles, mods, settings and Steam integration. Existing players must be able to update without reinstalling or selecting their game again. The actual transition mechanism remains to be designed and tested; a bridge release is a candidate, not an established requirement.

The selected visual direction is prototype A, “Sanctuary” (`launcher/prototype/index.html` in this development branch). It is a disposable visual reference, not production code. Rework screen layouts and wording where useful while preserving capabilities. Support mouse, keyboard and controller, including Steam Deck use. Review individual flows as they are developed.

Support the game's four menu languages: English, Brazilian Portuguese, Russian and Simplified Chinese. Define selection, fallback and translation review during design. Preserve existing session-report consent behavior exactly; consult `docs/telemetry.md` and the current implementation before implementing it. Do not infer permission to expand collection or change consent.

## Architecture to investigate

- Electron owns the desktop window and TypeScript interface. The Python core owns existing jobs and platform behavior initially. Port individual parts only when justified and behavior is verified.
- Design a narrow, versioned interface for state, commands, progress, cancellation, errors and lifecycle. Examine ownership of the launcher lock, child process failure, event ordering and restart before choosing a transport. JSON-RPC over standard input/output is an option, not a decision.
- The existing `launcher.cli` is useful evidence but its job progress is human-readable text; `@@` events originate in the game build process and are parsed by `launcher.build`. Do not treat the CLI as a complete machine protocol.
- Limit renderer privileges and explicitly design the window-to-main-process interface, external links and file selection. Package the Python runtime and Electron for each supported platform; measure size and startup impact rather than assuming them.
- Preserve the launcher's existing diagnostic and privacy behavior, including `data/logs/launcher.log`, secret handling and the game logging requirements in `docs/logging.md`. Determine a single owner for each log and lock.
- Preserve Windows setup/stub and Linux AppImage/installed-wrapper behavior as user-facing requirements while investigating their replacement. Document update manifests, version handover, interrupted updates and rollback per platform before selecting a transition design.

## Milestones

1. **Inventory and design.** Complete the parity checklist, map current module interfaces and update paths, identify platform-specific behavior and define the Electron/Python contract. Review this plan before implementation.
2. **First playable slice.** Detect an existing installation, display its state and launch the game on Windows and Linux. The owner performs the real local installation and Play tests. Record environment and observed result separately for each platform; do not call a simulated test a real game run. Packaging and automatic updates can follow later.
3. **Installation and operations.** First-run selection, toolchain, build/rebuild, progress, cancellation, error recovery and settings. Retain existing core behavior and test safe handling of interrupted jobs.
4. **Remaining parity.** Updates, saves, mods, Steam, customization, reports, uninstall and platform-specific behavior. Design and test the transition of existing installs before release.
5. **Replacement gate.** Complete the parity checklist; run relevant automated tests on both platforms; owner validates the first playable slice on each platform; verify migration through synthetic automated scenarios; document open limitations and operational release/rollback procedure. Review before any release or replacement.

Milestones may overlap where dependencies allow; they do not authorize publishing or modifying existing release assets.

## Parity inventory and acceptance

All entries below are **not yet implemented or verified in Electron**. For each, track implementation, automated results on Windows and Linux, owner-observed evidence where applicable, and remaining gaps. The existing behavior is described in `docs/launcher.md`; this checklist is an inventory, not evidence of passing tests.

- [ ] Startup: discover current and legacy installations, paths, state, settings and interrupted work; useful errors and recovery.
- [ ] First run: dump and game-folder selection, detection, shortcuts, update channel and preferences, prerequisites, validation and disk/path constraints.
- [ ] Build: tool downloads, extraction, validation, build and rebuild, progress and logs, cancellation, retry, interruption recovery and existing executable protection.
- [ ] Play: launch and exit handling, errors, hide/restore behavior, game process/restart tracking and logging.
- [ ] Updates: check and channels, notes, staged/background downloads, integrity checks, automatic/manual settings, switch, rebuild, cancellation, failed or interrupted update and rollback; never replace a running game.
- [ ] Migration: current and legacy names, settings, game location, saves, profiles, mods, Steam shortcuts and desktop entries; handover and recovery on Windows and Linux.
- [ ] Saves: export, optional logs/settings, import from archive or folder, validation, backups and recovery.
- [ ] Mods: add files/folders, enable/disable, ordering, rebuild and errors.
- [ ] Steam: add/update shortcut and artwork where supported, preserve existing shortcuts, Game Mode and Steam Deck interaction.
- [ ] Customization: themes, custom colors, background and dimming, animations, sounds and assets derived from the player's own game.
- [ ] Settings: update behavior, hide on play, build cache, channels, paths, logs and uninstall; preserve existing settings.
- [ ] Reports: current consent, timeout, choices, preview and comments, including behavior when reporting is unavailable. Do not change consent behavior.
- [ ] Platform features: Windows setup, shortcuts, path constraints and diagnostics; Linux AppImage installation, wrapper, desktop entry, Steam variants and optional udmabuf fix with existing approval behavior.
- [ ] Accessibility and input: keyboard, mouse, gamepad focus and navigation; scrolling, dialogs, resizing and readable layouts in the four languages.
- [ ] CLI, locks and diagnostics: retain existing supported command-line use, concurrent-job rules, crash/interruption reporting and log privacy; explicitly evaluate how the new UI shares these resources.
- [ ] Uninstall: existing choices for retaining or removing the built game and preservation of unrelated installations.

## Validation and known limits

Reuse existing tests where applicable and add meaningful synthetic automated scenarios for NX2Saria and NX2navi layouts, upgrades, data preservation, failed handover, restart and rollback. Use temporary fixtures and fake processes/releases; run platform-specific paths on Windows and Linux. No additional real historical installations are planned. The owner will test the first slice using their local installation; record exactly what was observed.

Telemetry can reveal field issues after distribution only within existing consent and collection behavior. It cannot establish that all migrations preserved data or represent users who do not send reports. This is an accepted limitation of the selected validation strategy, not a substitute for automated evidence.

## Open design questions

- Which Python process and interface lifecycle preserve locks, job ownership, update restarts and CLI use on both platforms?
- How will existing Windows and Linux launchers receive and start Electron, and recover from failed handover? Determine whether a bridge is needed.
- Which packaging and update artifacts satisfy the current manifests, offline behavior and platform requirements?
- How are input focus, gamepad behavior, language selection and translated strings verified in the new interface?
- What specific game/runtime evidence can the owner collect on each platform, and which unavailable environments remain unverified?

## First playable slice: proposed behavioral contract

This section is a design proposal, not a claim that an implementation or test exists. The user approved the overall plan; implementation choices below should be reviewed as part of the first-slice design.

### Installation and status

- Resolve the launcher root through the existing `Layout` rules, including the legacy environment variable and stub. Read the selected `game_dir` from existing settings. A development build must be explicitly pointed at the intended installation; do not silently use a development directory as evidence of detection.
- Derive availability from existing Python logic, including `build.installed()`: an interrupted rebuild can leave an older executable playable even without a finished compile record. Distinguish no configured game, unavailable/not built, ready, already running, interrupted work and inspection error. Do not call an executable healthy merely because its path exists.
- Display the selected path and useful recovery information. The initial slice need not implement installation or rebuild; where a required recovery flow is unavailable, say so explicitly instead of offering a nonfunctional action. Read-only inspection must not rewrite the player's settings or move files.

### Launch lifecycle

- Reuse or extract the existing behavior in `App.play`, `GameRun`, `App.watch_game` and platform helpers; do not replace it with an unchecked direct spawn. The actual implementation seam remains to be designed because some logic currently lives in the Tk UI.
- Preserve checks for missing executable and already-running game; mods that need a rebuild, the Windows memory preflight and its user decision; and existing launch logging. If a required pre-play flow is not yet available, explain the limitation and do not silently skip it.
- Track launch success, startup failure and termination, including the game's own restart copies and existing fatal/exit handling. Preserve the hide-while-playing preference, window restoration and focus; retain existing report behavior and post-game staged-update behavior when those capabilities are enabled. Do not claim parity until these are actually implemented and verified.
- Keep the operating-system launcher lock and job ownership consistent with `launcher/locks.py`; define which process owns it, what happens on UI or worker failure, and how a second launcher is refused. Do not have two independent processes contend for the same instance lock without a designed handover.
- Interface design must describe request identity, event ordering, cancellation, errors, process shutdown and restart; avoid making the renderer responsible for privileged file or process access. Transport and exact messages remain undecided.

### Evidence and acceptance

- Automated synthetic cases cover current and legacy root selection; ready and missing installations; interrupted rebuild with a still-playable old executable; already-running game; failed start; process exit and restart; pre-play decisions; lock contention and worker failure. Use fakes and temporary directories, without private game data.
- For Windows and Linux separately, record automated results and the owner's manual observation of detection, displayed state, starting and exiting the actual game. The owner conducts real-installation tests. Capture exact versions, environment and relevant sanitized logs, without keys or personal data.
- This slice is accepted only once both platforms have the agreed evidence. It is not a release gate for the full replacement: install, update, migration and all other parity items remain outstanding.

## First-slice interface proposal

Status: proposed for review; not implemented. The first vertical slice is read-only installation inspection. A later slice adds Play. The owner must agree the test seams before applying the TDD workflow.

### Findings that shape the interface

`Context` calls `Layout.ensure()`, opens and may rotate the launcher log; `Settings` may copy malformed settings to `launcher.json.bad`. Therefore constructing a normal `Context` is not a read-only inspection. The inspection path needs a side-effect-free settings reader sharing the current validation semantics, with explicit missing, unreadable and malformed outcomes. It must never return the settings dictionary or token to the renderer. A missing or malformed file cannot safely be presented as a confirmed configured installation.

`build.installed()` handles a usable old executable after an interrupted rebuild; reuse its semantics rather than checking only for a file. `Layout` determines current and legacy root selection, but explicitly choosing an existing root for development must not fall back silently to `build/launcher-dev`.

`FileLock` is owned and shared per Python process, while a running launcher holds it for its lifetime. For the eventual interactive session, the proposed owner is a single long-lived Python worker, including its jobs and log; Electron does not acquire the same lock. If the worker exits, the UI disables operations and reports the failure. This lifecycle remains a proposal until restart, update handover and platform behavior are verified; no silent lock reacquisition or concurrent Tk launcher.

### Proposed public behavior

Electron main starts the Python worker and exposes a small allowlisted interface to the isolated renderer. For the inspection slice, a versioned `inspectInstallation` request accepts an explicitly selected launcher root in development, never an executable path supplied by web content. Its response contains only the resolved root and game path where appropriate, a status (`not_configured`, `not_ready`, `ready`, `running`, `interrupted` or `inspection_error`), and safe, localizable reason codes. Running and interruption are separate facts where they can coexist; statuses and precedence must not hide uncertainty. Return structured errors for malformed or inaccessible data and avoid exposing secrets or arbitrary stack traces. Exact schema and transport remain to be finalized with tests. Do not falsely equate `ready` with a successfully tested game.

The eventual Play operation belongs in the Python worker and owns its lifecycle; it must preserve checks and existing behavior identified above. Do not expose an arbitrary process-spawn operation. Play, update and mutation operations are outside the first inspection implementation slice.

### Proposed test seams

1. **Installation inspection interface:** temporary representative current/legacy roots and build states exercised through the public inspection call. Catches status, path, error and privacy regressions, including no disk writes. Does not prove Electron packaging or actual game execution.
2. **Electron boundary:** a renderer-facing allowlisted request against a controlled worker. Catches message validation, serialization and worker-disconnect handling. Does not prove native OS game behavior.
3. **Owner's real-platform test:** on Windows and Linux, owner checks detection and state, then Play once its separate slice exists. This supplies real-environment evidence; it is not an automated regression suite.

TDD begins with one observable behavior at an agreed seam and proceeds one test and implementation at a time. Real historical installations remain out of scope for migration testing. The visual page must track changes to this proposal.

### Implementation progress

The first inspection slice now has a standalone Python command taking an explicit root and returning a limited JSON result. Initial automated tests cover an unconfigured root, malformed settings without a backup, and a synthetic completed build. Running and interrupted states, Electron communication, Play, platform validation and the rest of the contract remain outstanding. No real installation or game has been tested.

The Electron development slice can open a native folder picker and ask the Python inspection command for a limited result through a restricted preload interface. This is a short-lived process, not the proposed persistent lock-owning worker. Automated boundary tests cover an unavailable process and an empty temporary root. A headless Linux window startup was observed under Xvfb; actual dialog interaction, Windows, Play, packaging and real installations remain unverified.

The inspection now reports a job marker without deciding whether the job is active or interrupted. On Linux it can confirm a matching executable in `/proc`, but no match remains unknown. Windows now uses a read-only process snapshot; a process not observed remains unknown. Play and reliable per-platform liveness remain open.

The development UI now delegates Play to the selected installation’s bundled Python and current `launcher/start.pyw --play`, without a shell. This reuses the installed flow and may open its existing window. It rechecks installation status and conservatively refuses a job marker, including a stale marker. Synthetic tests verify argument handling, missing installation and path traversal rejection; this is not a persistent lock-owning worker or proof of game execution. Complete liveness, cross-platform integration and owner tests remain open.

A Linux Xvfb test now exercises the actual Electron window through selection, Play and marker refusal with a stubbed native dialog, a temporary synthetic installation and a stand-in executable. It does not validate the actual OS picker, Windows, game execution or the future persistent worker. The earlier test-locator failure was a test assumption about an accessible status role, not a launcher failure.

The delegated Play bridge now reports failure before acknowledging process spawn, and the synthetic Linux Electron test covers recovery after an unexecutable interpreter as well as selection, fake Play and a job marker. A spawned Python process does not prove the game opened.

The selection is cleared before opening the folder chooser. Cancellation or inspection failure does not leave Play pointed at a previous selection; the synthetic Electron test covers this case.

The delegated Play UI now reports a nonzero exit or signal as an error rather than a normal end; a zero exit remains inconclusive about whether the game ran. A synthetic Electron window test covers the nonzero case.

On Linux, the Electron window hides only after inspection observes the selected game executable descended from the delegated launcher process and the existing hide preference is enabled. It restores when that delegated process ends; a late inspection response is ignored. Synthetic tests cover both preference values and window visibility. Process visibility can be restricted, so no observed game leaves the window visible; game restarts remain unverified. The owner has observed hide/restore in one real Linux session. Windows uses the same monitor with a read-only process snapshot; actual Windows window behavior remains unverified.

The development window now inspects a standard existing installation on startup: Linux uses absolute `XDG_DATA_HOME` or `~/.local/share`, Windows uses the normal `%LOCALAPPDATA%/Programs` location, with current branding before legacy. A root with `app/current.txt` can be shown even if game settings are missing; the manual picker remains available. The Windows implementation does not yet resolve a redirected UserProgramFiles known folder and has not been validated on Windows. The synthetic Electron test covers startup discovery on Linux.

## Owner-observed Linux session — 2026-11-23

The owner reported that the Electron window hid and reappeared as expected during a local test. The installed launcher log records `play from Steam` starting the configured game at 16:15:37 and a code 0 end at 16:16:56; the game log records the window closing. The Electron origin is supported by the owner report and the test context, not independently identified by that log label. This is evidence for one Linux session only, not validation of Windows, restart, updates, or full feature parity. The owner asked to disregard memory warnings during this test because other workloads were running.

## Windows development test staging — 2026-11-23

A separate development copy was staged at `a separate development directory` on the owner's Windows machine, preserving the pre-existing files in its parent folder. The Windows Electron executable was downloaded and the TypeScript build and Node tests passed on that host: 12 passed, 2 platform-specific tests skipped. No window, native folder selection, installed-game launch, hide/restore or game session has been validated there. The common default NX2Saria and NX2navi `app/current.txt` paths were not found under that SSH account; the owner may need to select the installation manually. This is not a packaged release or migration test.

## Owner-reported Windows session — 2026-11-23

The owner reported that the Electron development copy found the installation automatically, manual selection also worked, the game opened, and the UI reported the flow had ended after the game was closed. This is owner-reported evidence for the tested Windows session. The logs in the shared `the shared test directory` folder were last written at 02:34, so they cannot independently corroborate this later session; no claim is made about its exit code. Automatic hide/restore on Windows, restart behavior, the persistent worker, updates and full parity remain unverified or unimplemented.

## Windows automatic window behavior — synthetic validation

The development bridge now uses a read-only Windows process snapshot and image query to confirm the selected game belongs to the launcher process tree. It enables the same hide/restore monitor used on Linux when the saved preference is enabled. On the owner's Windows machine, Python synthetic tests passed: 10 passed, 2 Linux-only skipped, including matching a descendant and rejecting a same-named executable in another folder. The Electron TypeScript/Node tests passed: 12 passed, 2 Linux-only skipped. No Windows Electron UI or real game was run for this change; the owner must validate actual hide/restore and restart. Unknown or inaccessible process state leaves the window visible.

## Portable Windows test artifact

A Windows x64 development ZIP was prepared without project TypeScript or Python source and with an embedded Python runtime and Electron. It still requires an existing installed launcher and game for Play; no game data is shipped. The packaged Electron window detected a synthetic installation on the owner's Windows machine with Python environment variables deliberately invalid, confirming its embedded inspection runtime, while Windows Python synthetic tests passed 10 with 2 Linux-only skips. The ZIP integrity and copied SHA-256 were verified. No real game or notebook session was tested with this artifact; it is not a release or migration package.

## Persistent Python worker: proposed design for coordinator review

Status: proposal only. It does not authorize implementation, a release, or any change to the current installer. The existing Electron development slice still delegates Play to the installed launcher's `--play` path and does not have this worker.

### Responsibility and ownership

- Electron's main process launches one worker for one selected launcher root and owns its standard streams. The worker owns `Context`, the existing operating-system `FileLock` for that root, the in-process job claims, job state, game lifecycle, settings and the launcher log. The renderer has no direct file, process or network access; the main process exposes only validated, operation-specific capabilities.
- Before changing any files, the worker acquires the same `data/launcher.lock` used by Tk and mutating CLI commands. Existing jobs in that Python process share the lock and claims. A second launcher or mutating CLI receives the existing busy result; `cli check` retains its existing read-only concurrency. Two launcher roots that point to one game are not currently mutually protected; document this existing limitation rather than claiming a stronger guarantee.
- `Context` currently calls `Layout.ensure()`, opens/rotates the log, and `Settings` may copy invalid settings. Do not instantiate it for read-only discovery. Define startup ordering so the lock is acquired before mutable setup, and explicitly decide how to preserve the existing `FileLock` behavior when its file cannot be opened: it currently proceeds without a lock. Do not silently change that behavior without review.
- Extract reusable Play/job orchestration from Tk in independently testable modules; do not call Tk callbacks from the worker or fork their behavior. Preserve pre-play mod preparation and rebuild, running-game detection, memory warning and its response, `GameRun` restart tracking, crash/report behavior, and the post-game staged-update decision. Headless Steam and the CLI retain their supported entry points and share the underlying operations rather than starting a second worker or acquiring the same lock from a different process.

### Protocol and lifecycle

- Proposed transport: UTF-8 newline-delimited JSON on pipes, initiated by Electron with a fixed entry point and root as a separate process argument. A versioned handshake identifies worker and protocol versions and capabilities before commands are enabled; an incompatible handshake is a visible failure, not an attempt to continue with guessed semantics. This is a proposal, not an approved wire format.
- Each request has a unique ID and receives exactly one terminal result or structured error. Unsolicited events identify their session and monotonic sequence; progress and lifecycle events carry stable codes and parameters, not translated strings. The UI localizes text in the four agreed languages. Define size limits, invalid JSON and unknown-method handling, request cancellation, ordering and shutdown in the protocol specification before implementation. Standard output is reserved for protocol frames; ordinary diagnostics use the existing sanitized logger, not stdout. Bound and sanitize stderr and never send tokens, raw settings, arbitrary paths or stack traces to the renderer.
- On window startup, the worker reports startup, root identity, lock outcome, protocol version and enabled features in the existing privacy-preserving launcher log. Log actions, decisions, failure and end with useful counts or outcomes; bound repeated lines. Register any new prefix in the appropriate logging documentation and checks. Preserve consent and report semantics exactly.
- Worker failure or protocol disconnect disables mutation and Play immediately, reports a recoverable error, stops pending requests, and records a sanitized failure when possible. Never automatically replay a mutating command. A newly launched worker must reacquire the OS lock, inspect the interrupted-job marker and reconcile state before offering operations. Do not infer that a game ended just because its supervising worker disappeared. Define and test behavior for an active game and for an active job separately; preserve the current close/cancel and recovery semantics before choosing any forced termination policy.
- A game-start event is emitted only after the existing native spawn succeeds, and a game-end event only after the existing `GameRun` logic finishes tracking restart copies. Electron hides/restores only for that confirmed lifecycle and the saved preference, not on worker spawn or an unrelated process. Preserve foreground/focus and Steam Deck behavior. Unknown observations remain unknown.

### Updates, rollback and CLI

- Background checking and staging can remain inside the lock-owning worker; switching app/runtime while Python or Electron holds files requires a separately designed handover. Keep existing checksum, staged marker, game-running, cancellation and recovery rules. Before applying, quiesce operations, persist required state and release handles/lock in a controlled order. A platform-specific bootstrapper must start the new version and re-establish the protocol/lock, or return to the prior version when startup fails. Windows stub code 86 and its timing, Linux wrapper/`execv`, installed shortcuts and `--continue-update` must be analyzed independently; a bridge release is a candidate, not an assumption.
- The current stub starts Python/Tk and its rollback contract does not know about Electron. Do not switch installed users or publish a bridge until current and legacy synthetic transition, interrupted handover, recovery, and startup-failure tests pass on both platforms. Preserve Python runtime replacement semantics and existing user data. Define ownership of the bootstrapper and exact readiness/rollback point before implementation.
- Preserve `launcher.cli` commands and exit codes, lock contention and `check` behavior, including existing scripts. Decide for each command whether it runs the shared core directly under the existing lock or communicates with a worker; do not require a GUI or silently alter existing CLI output. Steam's headless Play path and existing shortcuts remain functional.

### Proposed implementation order and tests

1. Extract the game lifecycle and core operations behind the already agreed public behavioral seams, with no UI behavior change. Verify current tests and synthetic success, refusal, mod rebuild, memory decision, restart and fatal paths.
2. Add the worker handshake, state query, bounded protocol and deterministic EOF/crash handling against synthetic roots, including malformed requests, incompatible versions, privacy and no unintended writes. Test a second process and CLI lock contention, cancellation, interrupted jobs and worker death on both OSes.
3. Connect Electron through a validated main/preload boundary. Test real Electron windows with synthetic processes, disconnects, delayed/out-of-order frames, recovery, input/focus and hide preference. Do not label a worker spawn or zero exit as proof of game execution.
4. Design and review update handover separately, then test version transitions and rollback with disposable synthetic installations. Only the owner tests the real game; Windows and Linux real-session evidence stays separate from automated results.

Open for coordinator review: transport and version negotiation; precise lock-failure policy; worker shutdown while the game or a job runs; first migration/rollback mechanism; and how to retain per-platform installer and Steam behavior without duplicate lock holders. The Electron/Chromium/npm/Python notice and license inventory must be completed before any distributed package or release; no new dependency is proposed by this design.
