# Electron launcher replacement plan

Status: overall plan approved by the owner; the first-slice behavioral contract is a design proposal. No production implementation or migration mechanism has been approved by this document.

Maintain `docs/electron-launcher-plan-visual.html` alongside this plan: update the visual page whenever this plan changes, preserving decisions, open questions and validation limits.

## Goal and decisions

Replace the existing launcher with a unified Electron and TypeScript interface, retaining the Python core initially. Windows and Linux are required; macOS is a future investigation and Electron support does not establish game support. Develop on a separate branch and regularly incorporate main, reviewing launcher changes for parity. The replacement is not ready to ship until the agreed functionality and platform gates below are met.

Keep existing NX2Saria and NX2navi installations working, including game paths, saves, profiles, mods, settings and Steam integration. Existing players must be able to update without reinstalling or selecting their game again. The actual transition mechanism remains to be designed and tested; a bridge release is a candidate, not an established requirement.

The selected visual direction is prototype A, “Sanctuary” (`launcher-electron/prototype/index.html` in this development branch). It is a disposable visual reference, not production code. Rework screen layouts and wording where useful while preserving capabilities. Support mouse, keyboard and controller, including Steam Deck use. Review individual flows as they are developed.

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
- [ ] Updates: check and channels, notes, staged/background downloads, integrity checks, automatic/manual settings, switch, rebuild, cancellation, failed or interrupted update and rollback; never replace a running game. In the persistent-worker Play milestone, the worker only detects and logs a waiting update, reports it in the handshake and after the game, and leaves all staged files untouched. The interface directs the player to the installed launcher to apply it. Electron still needs its own update flow and separately designed handover and rollback before parity.
- [ ] Migration: current and legacy names, settings, game location, saves, profiles, mods, Steam shortcuts and desktop entries; handover and recovery on Windows and Linux.
- [ ] Saves: export, optional logs/settings, import from archive or folder, validation, backups and recovery.
- [ ] Mods: add files/folders, enable/disable, ordering, rebuild and errors.
- [ ] Steam: add/update shortcut and artwork where supported, preserve existing shortcuts, Game Mode and Steam Deck interaction.
- [ ] Customization: themes, custom colors, background and dimming, animations, sounds and assets derived from the player's own game.
- [ ] Settings: update behavior, hide on play, build cache, channels, paths, logs and uninstall; preserve existing settings.
- [ ] Reports: preserve current consent, choices, preview, comments and unavailable-service behavior. The question appears the first time an installed game's home screen is shown, including after install or update jobs; after a job it has no countdown, while normal startup retains its current countdown. In the persistent-worker Play milestone, the new code neither asks nor sends reports: the game summary stays on disk for the installed launcher to process under its existing consent. Local crash-summary synthesis by `finish_game()` remains allowed when reports are enabled. Electron still needs the existing question, choices, preview, comments, sending and recovery behavior before parity.
- [ ] Crash-log offer parity: the installed launcher now offers to send a sanitized, bounded log after an abnormal end. Electron must match the eligibility and exclusion rules, the opt-out setting, the pending offer's log-identity check and deferred display after headless Play, the ordering after the session-report question, and the no-input guard. Preserve the four choices (send once, decline, exact preview, or comment with log attached), no automatic retry, and the rule that no log is sent without an explicit click. The preview and payload must use the existing privacy and size limits, including session-summary consent. This remains outside the current worker milestone; no new sending or question is implemented here.
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

Status: the coordinator approved the worker direction and the milestone limits below; implementation is not yet present. This does not authorize a release or any change to the current installer. The existing Electron development slice still delegates Play to the installed launcher's `--play` path. Earlier proposal details in this section are superseded where the approved milestone decisions state otherwise.

### Responsibility and ownership

- Electron's main process launches one worker for one selected launcher root and owns its standard streams. The worker owns `Context`, the existing operating-system `FileLock` for that root, the in-process job claims, job state, game lifecycle, settings and the launcher log. The renderer has no direct file, process or network access; the main process exposes only validated, operation-specific capabilities.
- Before changing any files, the worker acquires the same `data/launcher.lock` used by Tk and mutating CLI commands. Existing jobs in that Python process share the lock and claims. A second launcher or mutating CLI receives the existing busy result; `cli check` retains its existing read-only concurrency. Two launcher roots that point to one game are not currently mutually protected; document this existing limitation rather than claiming a stronger guarantee.
- `Context` currently calls `Layout.ensure()`, opens/rotates the log, and `Settings` may copy invalid settings. Do not instantiate it for read-only discovery. Define startup ordering so the lock is acquired before mutable setup, and explicitly decide how to preserve the existing `FileLock` behavior when its file cannot be opened: it currently proceeds without a lock. Do not silently change that behavior without review.
- Extract reusable Play/job orchestration from Tk in independently testable modules; do not call Tk callbacks from the worker or fork their behavior. Preserve pre-play mod preparation and rebuild, running-game detection, memory warning and its response, `GameRun` restart tracking, crash/report behavior, and the post-game staged-update decision. Headless Steam and the CLI retain their supported Python entry points and existing lock behavior. Steam can start the game while the launcher lock is held if there is no running-job marker; it refuses when the lock is held and a marker names an active job. The worker must detect a game launched by Steam and refuse a second Play without claiming it supervises that external session.

### Protocol and lifecycle

- Proposed transport: UTF-8 newline-delimited JSON on pipes, initiated by Electron with a fixed entry point and root as a separate process argument. A versioned handshake identifies worker and protocol versions and capabilities before commands are enabled; an incompatible handshake is a visible failure, not an attempt to continue with guessed semantics. The approved wire format uses one exact integer protocol version on both sides; a mismatch is a visible error.
- Each request has a unique ID and receives exactly one terminal result or structured error. Unsolicited events identify their session and monotonic sequence; progress and lifecycle events carry stable codes and parameters, not translated strings. The UI localizes text in the four agreed languages. Define size limits, invalid JSON and unknown-method handling, request cancellation, ordering and shutdown in the protocol specification before implementation. Standard output is reserved for protocol frames; ordinary diagnostics use the existing sanitized logger, not stdout. Bound and sanitize stderr and never send tokens, raw settings, arbitrary paths or stack traces to the renderer.
- On window startup, the worker reports startup, root identity, lock outcome, protocol version and enabled features in the existing privacy-preserving launcher log. Log actions, decisions, failure and end with useful counts or outcomes; bound repeated lines. Register any new prefix in the appropriate logging documentation and checks. Preserve consent and report semantics exactly.
- Worker failure or protocol disconnect disables mutation and Play immediately, reports a recoverable error, stops pending requests, and records a sanitized failure when possible. Never automatically replay a mutating command. A newly launched worker must reacquire the OS lock, inspect the interrupted-job marker and reconcile state before offering operations. Do not infer that a game ended just because its supervising worker disappeared. Define and test behavior for an active game and for an active job separately; preserve the current close/cancel and recovery semantics before choosing any forced termination policy.
- A game-start event is emitted only after the existing native spawn succeeds, and a game-end event only after the existing `GameRun` logic finishes tracking restart copies. Electron hides/restores only for that confirmed lifecycle and the saved preference, not on worker spawn or an unrelated process. Preserve foreground/focus and Steam Deck behavior. Unknown observations remain unknown.

### Updates, rollback and CLI

- Background checking and staging can remain inside the lock-owning worker; switching app/runtime while Python or Electron holds files requires a separately designed handover. Keep existing checksum, staged marker, game-running, cancellation and recovery rules. Before applying, quiesce operations, persist required state and release handles/lock in a controlled order. A platform-specific bootstrapper must start the new version and re-establish the protocol/lock, or return to the prior version when startup fails. Windows stub code 86 and its timing, Linux wrapper/`execv`, installed shortcuts and `--continue-update` must be analyzed independently; a bridge release is a candidate, not an assumption.
- The current stub starts Python/Tk and its rollback contract does not know about Electron. Do not switch installed users or publish a bridge until current and legacy synthetic transition, interrupted handover, recovery, and startup-failure tests pass on both platforms. Preserve Python runtime replacement semantics and existing user data. Define ownership of the bootstrapper and exact readiness/rollback point before implementation.
- Preserve `launcher.cli` commands and exit codes, lock contention and `check` behavior, including existing scripts. Decide for each command whether it runs the shared core directly under the existing lock or communicates with a worker; do not require a GUI or silently alter existing CLI output. Steam's headless Play path and existing shortcuts remain functional: a held launcher lock alone does not stop Steam Play; a held lock with a running-job marker does.

### Proposed implementation order and tests

1. Extract the game lifecycle and core operations behind the already agreed public behavioral seams, with no UI behavior change. Verify current tests and synthetic success, refusal, mod rebuild, memory decision, restart and fatal paths.
2. Add the worker handshake, state query, bounded protocol and deterministic EOF/crash handling against synthetic roots, including malformed requests, incompatible versions, privacy and no unintended writes. Test a second process and CLI lock contention, cancellation, interrupted jobs and worker death on both OSes.
3. Connect Electron through a validated main/preload boundary. Test real Electron windows with synthetic processes, disconnects, delayed/out-of-order frames, recovery, input/focus and hide preference. Do not label a worker spawn or zero exit as proof of game execution.
4. Design and review update handover separately, then test version transitions and rollback with disposable synthetic installations. Only the owner tests the real game; Windows and Linux real-session evidence stays separate from automated results.

Open for coordinator review: transport and version negotiation; precise lock-failure policy; worker shutdown while the game or a job runs; first migration/rollback mechanism; and how to retain per-platform installer and Steam behavior without duplicate lock holders. The Electron/Chromium/npm/Python notice and license inventory must be completed before any distributed package or release; no new dependency is proposed by this design.

The coordinator approved the persistent-worker direction: line-delimited UTF-8 JSON with an exact integer protocol version; the handshake reports lock availability and capabilities. A lock file that cannot be opened retains current behavior and is logged and surfaced. EOF cancels an active job through its existing path while the worker continues following an already running game until it ends; the game is never killed because the UI or worker disappears. Characterization tests must establish current Tk behavior first. Migration and rollback are reserved for a separate design in the later parity milestone.

### Approved Play milestone boundaries

- Begin the worker after the game-end extraction is merged: first handshake, exact protocol version, lock from `Layout` before `Context`, inspection and idle EOF; then the shared Play and game lifecycle; then the existing mod-rebuild job and Play resumption. Do not duplicate Tk decisions: propose a small characterized extraction if integration exposes a missing shared decision.
- The worker does not check, download or install updates, and does not call `BackgroundUpdate` or `install_staged`. It detects and logs a waiting update in the handshake and again after the game; the interface tells the player to open the installed launcher to apply it. A synthetic test compares the waiting marker and staged tree before and after Play. Handover, installation and rollback remain a separate project.
- The new code does not create a `Reporter`, send reports or ask the consent question. A session summary stays on disk for the installed launcher to process under existing consent. `finish_game()` may synthesize a crash summary locally when reports are enabled. A synthetic session must leave the expected summary and produce no requests to a fake server. Full Electron consent and reporting behavior remains a parity gap.
- A job is terminal only when `finished` is set after marker cleanup and release of claims and its nested lock; `result` alone is insufficient. A `FileLock` with `key is None` means the lock could not be opened: proceed as today, log it and report it in the handshake.
- After a delayed memory decision, the worker rechecks the current attempt identity, its own game session and any external game before spawning. Stale answers are ignored; an occupied state receives the same refusal as the initial check. This additional recheck is for the asynchronous worker, not the modal Tk extraction. Include a synthetic race test.

### Worker Play: protocol and event loop design

The worker reserves the original stdout byte stream for UTF-8 JSON frames and redirects ordinary Python stdout to stderr before loading or calling core operations. A single write lock protects complete frames. A dedicated reader thread reads bounded requests and places requests, malformed-frame failures and EOF in a queue; the main loop consumes that queue and periodically observes the active `GameRun`. All responses and unsolicited events use the same serialized writer; events carry a strictly increasing sequence number and a session identity. This allows game-end events while no request arrives and prevents concurrent writes from interleaving.

EOF while idle exits; EOF during a tracked game disables further output and requests, continues monitoring through `finish_game`, and releases the lock only after the game ends. Once jobs are integrated, EOF requests existing job cancellation and waits for `finished`, including cleanup, before exiting; game and job shutdown are independently covered by synthetic tests. The worker never terminates a game because its input or window disappears. An unavailable output stream is treated as a disconnect with the same lifecycle rules. `Context` has no public close method; the current private-file cleanup will be discussed separately with the coordinator before any shared-core change.

The in-progress Play implementation has synthetic Linux coverage for start and end events, EOF and malformed input during a game, and memory decisions including an outdated answer and an external game appearing before acceptance. A local TypeScript client can handshake, inspect and close a real worker in a synthetic Linux installation; the Electron window is connected locally for inspection and Play; a synthetic Linux window test passed for two consecutive games, hide/restore, reselecting or cancelling installation selection and closing the window during a third game, which then finished while the worker logged its end. A separate synthetic window case kills the worker: the window returns, Play is disabled and the game process finishes. The synthetic Linux window also displays the waiting-update notice after a game without applying it. The short-lived Play bridge and its obsolete tests have been removed; the UI test also checks job-marker refusal. Jobs, other failure cases, Windows and the complete parity checks remain pending. The memory warning keeps Play as the default and preserves the current fallback of continuing if the dialog cannot open; a synthetic test covers that fallback. The worker logs bounded Play, memory and spawn outcomes without paths. Inspection and Play results have validators that reject malformed states and discard unrelated fields before reaching the UI. Its synthetic tests also reject out-of-order events and an end for the wrong session, and disable further requests after a timeout without replaying a mutation. Synthetic Linux and Windows tests also confirm refusal of another Play while the worker is supervising a game or detects an external game. Broken output during the game and immediately before its end is also covered on Linux. A synthetic subprocess survives forced worker termination; a separate Linux test also uses the native spawn path with a temporary executable and confirms it survives forced worker termination. A Windows synthetic executable also completed after its worker was killed using the native spawn path. An initial Windows synthetic run failed because the stand-in child inherited the protocol streams. After matching the real game spawn by redirecting all three streams, the suite passed on Windows (27 passed, two skipped) and Linux (29 passed); The embedded Python from the previous Windows development package also passed synthetic handshake, inspection and game lifecycle tests with the current worker. A temporary packaged Electron window on Windows passed two consecutive synthetic Plays using an executable compiled from the test fixture and the embedded Python. Windows synthetic window hide and restore passed. A Playwright-driven application close during a third synthetic game left the game running to completion and logged worker EOF, but no worker game-end line appeared within 10 seconds; Closing the BrowserWindow directly produced the same observation. Starting the worker detached on Windows fixed the synthetic packaged-window test: the game finished and the worker recorded its end after the window closed. An isolated test with all three worker pipes closed also passed. Jobs and the real game remain unverified. A synthetic game session leaves the staged-update files unchanged and reports the waiting state on completion. Another synthetic session leaves its summary in place while a local fake telemetry server records no requests. Both scenarios passed on Linux and Windows. These changes are local and incomplete: job cancellation, Windows, Electron integration and the full milestone checks remain unverified.

The mod-job extraction is prepared on a separate branch against the frozen main and merged locally into the Electron branch for integration. Local integration starts a mod rebuild, emits bounded progress, accepts cancellation and waits for `finished`; successful completion resumes Play or asks for a memory decision, and EOF cancels the job. Unit tests use fake jobs and do not spawn a game. A forced worker exit leaves the job marker for a successor to inspect after reacquiring the lock. A Linux synthetic Electron window test covers successful mod-job resumption, cancellation and the next Play without repeatedly rebuilding the same mods. Windows job UI and remaining full validation are pending. The Tk characterization records that it can observe `result` before `finished`; the worker deliberately waits for cleanup.

### Worker implementation: handshake and inspection

The first worker slice implements a bounded JSON-lines handshake with exact protocol version 1, validates the selected
launcher root and protocol before writing, acquires the existing lock before `Context`, reports held or unavailable
lock state and detects a complete waiting update without installing it. It supports inspection, structured protocol
errors and idle EOF cleanup. Synthetic Python tests exercise two workers, CLI and Steam contention, malformed input,
the lock-open fallback and staged-file preservation; the TypeScript protocol parser is tested against a real worker
handshake. The synthetic worker tests run on Windows with Python 3.11; they use temporary roots and a fake Steam process and do not use a real game. The Electron window is not yet connected to the worker. Worker Play, jobs, post-game behavior and owner real-game evidence remain outstanding.

## Development logging and dependency inventory

The current development Electron writes `data/logs/launcher-electron.log` only for a recognized installation whose logs directory already exists; it does not create that directory during inspection. It logs startup version/platform, selection kind and recognized brand (no paths), inspection and Play outcomes, and window visibility. Existing files over 1 MiB are reset on opening; logging failure does not prevent the application from working. A persistent worker will later use the existing privacy-aware launcher logger. See `docs/electron-launcher-notices.md` for the preliminary runtime and development-dependency license inventory; it is not release clearance.

The development slice now refreshes installation state after a delegated Play ends and permits another Play without reselecting when the refreshed state is ready and unblocked. A Linux Electron test covers two consecutive starts and verifies a stand-in game is still running when the Electron window closes. It does not prove the future worker recovery contract or Windows real-game behavior.

## Portable development test with activity logging

A second private Windows x64 ZIP was built from the Electron development head `9203aeea`, whose Tests and Content guard checks passed. It includes the bounded, path-free development log and refreshed state after Play. Archive integrity and the copy checksum were verified. On a Windows host, the packaged window used its embedded Python to find a synthetic installation, execute two stand-in sessions, hide and restore, and write sanitized development logs. No real game was run in this verification. The owner still needs to test the actual game on the laptop, including two plays, hide/restore and closing the launcher during play; the package is not a release or migration artifact.
