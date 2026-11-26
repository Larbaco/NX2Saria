# Electron launcher replacement plan

Status: the owner approved the overall plan. The Electron branch has synthetic coverage for Play, first-run operations and installed-game settings on both platforms. The provisioning design is awaiting authorization for implementation after review of the download and disk costs.

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

Each entry stays open until implementation and the required verification are complete. Track automated results on Windows and Linux, owner-observed evidence where applicable, and remaining gaps. `docs/launcher.md` describes the existing behavior.

- [ ] Startup: discover current and legacy installations, paths, state, settings and interrupted work; useful errors and recovery. Node tests cover current and legacy default paths, and owner sessions confirmed automatic detection on both platforms. The standalone Windows registry fallback and the explicit root argument are implemented. Electron launcher tests run `45844664530` passed the native Windows scratch-key test for Unicode and a stale value, and the Linux window tests for picker cancellation and explicit-root startup. The extracted private Linux package `4bbd8059` passed default and explicit startup with the published 0.27.0-rc.4 runtime. The corresponding Windows ZIP passed archive checks and awaits the native notebook session. In the manual picker, cancelling keeps the existing selection, its lock and any open settings panel. Only a confirmed folder choice can replace the current worker. The Linux locale window fixture checks this with an active installation.
- [ ] First run: dump and game-folder selection, detection, shortcuts, update channel and preferences, prerequisites, validation and disk/path constraints. The Linux Electron first-run test covers both proposals, preserving them on cancel or rejection, and both worker tests verify that settings stay unchanged. The Windows and Linux worker tests exercise the install request with a stand-in task. The Linux Electron window test covers progress, cancel, retry and a return to a playable screen. Background dump search proposes the newest valid result and preserves a manual choice. Worker tests use an isolated search directory, and the Linux window test displays the discovered file. The first-run preferences use the existing update settings and Windows shortcut. The first-run requirements check reports missing runtime support or Linux build tools. First installation without the app/runtime and owner validation remain open.
- [ ] Build: tool downloads, extraction, validation, build and rebuild, progress and logs, cancellation, retry, interruption recovery and existing executable protection. First run calls the existing InstallJob. The worker test refuses low space and a playable destination without rewriting settings or the executable, and waits for the job marker to be cleared on success. The Linux window test cancels and retries with a stand-in job. Manual Rebuild uses the existing RebuildJob. A synthetic worker case proves refusal while the old marker exists, completion after cleanup, honoring the cache setting and leaving the last executable untouched. The Linux window visits the ready screen after an explicit Rebuild without automatically starting Play. The interrupted-build cases below prove wait, stop, stale and reused process identities, unreadable markers and recovery by Tk. A complete build of the real game remains owner validation.
- [ ] Play: launch and exit handling, errors, hide/restore behavior, game process/restart tracking and logging. The worker test `test_native_game_restart_retains_the_play_session_and_lock` confirms one session, a busy second worker while the replacement process lives, and the replacement's final exit code. It passed on Linux and Windows in the Electron launcher tests run `45703680179`. Real checks for the latest changes remain open.
- [ ] Updates: check and channels, notes, staged/background downloads, integrity checks, automatic/manual settings, switch, rebuild, cancellation, failed or interrupted update and rollback; never replace a running game. In the persistent-worker Play milestone, the worker only detects and logs a waiting update, reports it in the handshake and after the game, and leaves all staged files untouched. The interface directs the player to the installed launcher to apply it. The next manual check uses the selected feed and channel, shows bounded release notes, and has loopback worker and Linux Electron tests. Staging, automatic checks and application, rebuild after update, handover and rollback remain open.
- [ ] Migration: current and legacy names, settings, game location, saves, profiles, mods, Steam shortcuts and desktop entries; handover and recovery on Windows and Linux.
- [x] Saves: export, optional logs/settings, import from archive or folder, validation, backups and recovery. The synthetic worker passed in the Linux and Windows jobs of Electron launcher tests run `45759008861`. The Linux Electron window verifies picker cancellation, both source types and restoring the backup. The portable package `f53b2481` also passed export, import and backup creation with the published runtime. All automated scenarios use scratch saves.
- [x] Mods: add files/folders, enable/disable, ordering, rebuild and errors. The Linux window verifies folder and archive selection, ordering, conflicts, toggling and invalid inputs. The worker tests passed on Linux and Windows in run `45759008861`, including resuming Play after the stand-in rebuild, refusing a mutation during Play and moving a mod across pages. The Linux portable package `f53b2481` verified folder addition with the published runtime. A real build still has its own acceptance item.
- [ ] Steam: add/update shortcut and artwork where supported, preserve existing shortcuts, Game Mode and Steam Deck interaction. The worker and Linux window tests use the existing VDF writer with scratch roots. They cover updating an existing ID, preserving other entries, artifact generation, open-Steam refusal on Linux, a Flatpak-only installation and completing an accepted write after the window closes. The Steam worker tests passed on both runners in `45763560387`. Native Steam Deck and Game Mode remain open.
- [x] Customization: themes, custom colors, background and dimming, animations, sounds and assets derived from the player's own game. Electron uses the existing theme palette, persists the same choices, and applies custom colors and reduced-motion preferences to the Sanctuary layout. The Linux window exercises theme, color reset, animation and sound settings. A native picker and the shared normalized picture cache are now in place. Worker and Linux window tests cover invalid input, a failed settings write, cancellation, dimming and removal of the retained copy. The worker reads or extracts the game title font and cues through the current Python cache and decoders. A synthetic worker tests the extraction handoff and fixed asset names. The Linux window loads a fixture font, checks both cues at the existing Low gain, and verifies Off. Both CI runners passed in `45797646341`. The extracted Linux private package `211f43a2` applies and removes a synthetic picture using the published 0.27.0-rc.3 runtime.
- [ ] Settings: update behavior, hide on play, build cache, channels, paths, logs and uninstall; preserve existing settings. Electron reads and changes the same Hide Launcher While Playing preference through the lock-owning worker. A synthetic Linux window test covers both values, persistence and preservation of the other settings. The installed-game panel exposes the current channel, update flags, cache preference, folders, registered dump and the Windows memory warning. The settings worker tests passed on both runners in Electron launcher tests run `45760705928`. The Linux window test confirms picker cancellation, validation and preference persistence. Reports and uninstall have their own acceptance entries. The Discord activity control uses the game INI and leaves the launcher settings unchanged in worker and Linux window tests. The Linux administration control uses the same PolicyKit flow and probes; tests simulate confirmation, cancellation, apply and removal. Verify the hide control separately on real Linux and Windows installations.
- [ ] Reports: preserve current consent, choices, preview, comments and unavailable-service behavior. The question appears the first time an installed game's home screen is shown, including after install or update jobs; after a job it has no countdown, while normal startup retains its current countdown. In the persistent-worker Play milestone, the new code neither asks nor sends reports: the game summary stays on disk for the installed launcher to process under its existing consent. Local crash-summary synthesis by `finish_game()` remains allowed when reports are enabled. The Electron reports screen now uses the same question, choices, preview, comment and sending policy. Private packages require the test sending opt-in documented below. Loopback tests verify exact report bytes, the shared sent marker, the Off and disabled states, and a newer session finishing before a previous report is acknowledged. The report tests passed on Linux and Windows in Electron launcher tests run `45761803115`. The installed update must pass its post-update startup reason through the future activation handoff. Private package evidence is recorded below.
- [x] Crash-log offer parity: the installed launcher now offers to send a sanitized, bounded log after an abnormal end. The worker now leaves the existing pending marker when supported by the selected installation. The installed launcher can show the offer while the log still matches that session. Electron must match the eligibility and exclusion rules, the opt-out setting, the pending offer's log-identity check and deferred display after headless Play, the ordering after the session-report question, and the no-input guard. Preserve the four choices (send once, decline, exact preview, or comment with log attached), no automatic retry, and the rule that no log is sent without an explicit click. The preview and payload must use the existing privacy and size limits, including session-summary consent. The Electron window now shows and previews these offers using the active installation’s existing implementation. The worker test checks guarded input, stale logs, opt-out, the disabled switch and exact loopback transmission. The Linux window confirms that the session question precedes the offer, previews the prepared request and verifies marker removal on decline. The opt-in still gates all sending. Both worker jobs passed in run `45762522567`. The extracted Linux package `335a0ab0` showed and previewed an offer after a stand-in fatal exit and consumed the marker after a refusal.
- [ ] Platform features: Windows setup, shortcuts, path constraints and diagnostics; Linux AppImage installation, wrapper, desktop entry, Steam variants and optional udmabuf fix with existing approval behavior. The Linux window and worker exercise the optional fix with a simulated device and PolicyKit helper. Setup, AppImage provisioning and hardware Steam cases remain open.
- [ ] Accessibility and input: keyboard, mouse, gamepad focus and navigation; scrolling, dialogs, resizing and readable layouts in the four languages. The Linux window test exercises keyboard Tab, Enter and Escape, a standard synthetic controller, ignored input without focus, direction, activation, return, scrolling and layout at the minimum window size and 140% zoom. The locale test covers all four languages at the minimum window width and 120% zoom, system fallback, selection and persistence, rollback after a failed write and a translated native dialog title. The Node test checks the labels and formatting fields. The Node, worker and Linux window checks passed in run `45802434603`. Game Mode on a device and language review by the owner remain open.
- [ ] CLI, locks and diagnostics: retain existing supported command-line use, concurrent-job rules, crash/interruption reporting and log privacy; explicitly evaluate how the new UI shares these resources.
- [x] Uninstall: existing choices for retaining or removing the built game and preservation of unrelated installations. Settings now opens the confirmation with Remove off. Scratch worker tests cover both platform helpers, protected data and busy/unsafe refusals. The Linux window tests Escape, reset of the removal choice, the default Keep operation and the log. The native Linux and Windows worker jobs and the Linux window passed in run `45802434603`. The extracted Linux package `4e1c4e31` exercised cancellation and keeping the game with the published app/runtime. Provisioned shell removal stays with migration.

## Standalone Windows installation discovery

An explicit `--launcher-root` path uses that root on either platform. It
bypasses the default and registry searches, including when the argument is
invalid. The worker still owns inspection and the lock. When no argument
is supplied, check the usual platform paths first. The standalone Windows
package consults the Setup's current-user NX2Saria and NX2navi
`InstallLocation` only if none of the usual paths is present. Use the
absolute Windows system PowerShell executable with a fixed, read-only
query, UTF-8 output and a short timeout. Keep profiles and interactive
prompts out of the query and use the machine's existing execution policy.

Accept only absolute paths. Ignore stale registry entries that fail the
usual file, runtime or worker inspection checks. Preserve the busy
result for a recognized installation locked by another worker, so a
second launcher cannot silently switch installations. Keep the picker
available when all candidates fail. With a recognized installation, log
the result and source without the raw path. Test source precedence and
failed queries, and round-trip a Unicode folder and stale value through
a scratch current-user registry key on the Windows runner.

### Private startup and settings packages

Both artifacts were packaged at `4bbd8059`. The extracted Linux package
used the published 0.27.0-rc.4 app and Python runtime under Xvfb. It verified
default and explicit startup, cancellation without replacing the worker,
the Discord activity setting and a clean exit. The Windows ZIP passed
integrity, contents, build ID and notices checks. The owner opened it on a Windows notebook on November 26. The two launcher
logs verify default detection of 0.27.0-rc.4, the saved shadow theme, writes to
the shared hide preference in both directions and clean exit. The notebook
session did not exercise Play. The LEIA-ME covers the new settings and discovery behavior
and gives the initial notebook test sequence.

The same launcher code passed on both CI runners in `45844664530`.
The general Tests workflow passed for the package commit in `45845176105`.
The local package check exercised only the launcher and did not run or
build the game. The local full suite remains on hold.

- Linux `NX2Saria-Electron-Worker-Linux-x64-4bbd8059.tar.gz`, 122,373,433 bytes.
  SHA-256 `2b5da4f0ceaba74f9d018bb6290a717b3e929c1578c8f274a79c407d9c3ca1b8`.
- Windows `NX2Saria-Electron-Worker-Windows-x64-4bbd8059.zip`, 147,006,384 bytes.
  SHA-256 `af50c215e2fcc819a981fd14eb8aa1ad2123710f2ed57d48466890544a92e197`.

### Rebuild after an interrupted app update

Match the Tk home screen for a completed build made with a different kit.
Show the built and active versions, emphasize Rebuild and keep Play
available for the last completed executable. Log both versions whenever
the worker attempts to launch it. If the compile receipt is absent from a
resumable build, indicate that Rebuild is required and refuse Play.
Reinspect when the build finishes and check again after a memory prompt.
Compare known versions using the existing version parser. A checkout
keeps its development behavior.

On the notebook, the Tk update continuation was cancelled before rebuilding.
The previous Electron worker returned `ready` because the old executable
still had a compile receipt. On ordinary startup Tk distinguishes that
older build and offers Rebuild. The `--continue-update` path starts the
automatic post-install job. The later log shows a completed RebuildJob,
without proving the input that started it. A worker subprocess test
reproduced the earlier behavior with the spawn call replaced by a recorder.

Product question for a later review: whether to prohibit Play until an
updated app has a matching game build. The test package retains the
existing launcher behavior for a completed older build.

## Replacement gate

Status is open. A test package is a portable client for a provisioned
installation. Replacement also needs an independently installable launcher
and a verified transition from existing Tk installations.

- **Automatic checks.** The settings and Linux device-control slice
  `34fd2660` passed Electron launcher tests on Ubuntu and Windows in run
  `45804170737`, Tests in `45804172241`, and Content guard in `45804170774`.
  The Linux tests substituted the device and PolicyKit. Startup slice
  `e29fb6f4` passed Electron launcher tests on both runners in
  `45844664530` and Content guard in `45844664417`.
- **Owner evidence.** Two normal Play sessions on Windows are recorded
  below. Linux package `b62829a5` verified automatic detection, hide and
  restore, consecutive sessions, a busy second window and closing the
  launcher with a game still running. The current private package
  `4bbd8059` is available for the native Windows startup and Play check.
  Hardware controller use, Game Mode and real rebuild remain pending.
- **Migration and rollback.** Still requires the owner and releases
  decision on the measured shell download and disk cost. The current
  updater, staged files, app activation and rollback remain with Tk.
  Prove the current and legacy transition and interrupted rollback with a
  scratch install and local feed on Linux and Windows before requesting
  replacement review.
- **Replacement review.** Send the branch head and evidence to releases
  once the inventory and migration checks are complete. The branch work
  ends at that hand-off.

## Validation and known limits

Reuse existing tests where applicable and add meaningful synthetic automated scenarios for NX2Saria and NX2navi layouts, upgrades, data preservation, failed handover, restart and rollback. Use temporary fixtures and fake processes/releases; run platform-specific paths on Windows and Linux. No additional real historical installations are planned. The owner will test the first slice using their local installation; record exactly what was observed.

Telemetry can reveal field issues after distribution only within existing consent and collection behavior. It cannot establish that all migrations preserved data or represent users who do not send reports. This is an accepted limitation of the selected validation strategy, not a substitute for automated evidence.

## First-run operations contract

For a launcher root that already has an app version and Python runtime, show the proposed game folder when
the game is unconfigured or unavailable. Offer a native folder picker in the Electron window. Cancel keeps
the current proposal. For a nonempty directory without a build cache or an NX2Saria or NX2navi name, propose
its `NX2Saria` child, as the installed launcher does. Inspection and choosing a proposed path do not create
the folder or save settings. The implementation will log each result as a default, saved, selected or rejected
proposal. A synthetic installation will cover the default and a selected nonempty folder and verify that its
files and settings are unchanged.

The dump picker opens a native file dialog and validates the selected file with the same NUP and title
check as Tk. A valid saved choice can be shown when first run reopens. Cancel or a rejected selection
keeps the previous proposal. Validation reads only the file's metadata, logs the result without its path
and leaves the installation settings unchanged. Accepting a proposal confirms the game's identity.
The installation job retains its version and content checks. Both the dump and the proposed destination
will be rechecked when installation is explicitly started.

On explicit Begin Installation, the worker uses its last validated dump and destination proposals.
It refuses a new installation while it lacks the lock, has a running game or job, or sees a job
marker that needs recovery. It checks the selected file and target again before modifying either
the settings or game folder. Use the existing path limit, running-game detection and 34 GB check.
Credit a removable partial ROMFS image only at the selected destination and recheck after cleanup.
An existing complete ROMFS retains the current low-space exception. A playable configured
game or a playable game already present at the selected destination cannot be replaced through first run.

After validation, record the two choices and start the existing Python `InstallJob`. Keep its
tool, dump version and build checks. Report progress, allow cancel and wait for complete job
cleanup before showing success, failure or cancellation. Success refreshes the installation
state without starting the game. Cancellation or failure allows retry after cleanup. Closing the
window during installation cancels the job and waits for cleanup as it does for a mod rebuild.
The same worker logs refusal, start and completion without raw dump or destination paths. Tests
use stand-in job steps and verify both state transitions and untouched files on refusal. The Linux Electron test
starts a stand-in build, cancels it, waits for the marker to clear, retries and returns to the ready screen
without spawning the game. The branch CI runs the worker case on both platforms. Actual tool download,
extraction and compilation are covered separately by existing Python tests and were not run by the new UI test.

When no saved selection is valid, start the existing dump search in the background. Reuse the
current search directories, mounted-drive bounds, title check and newest-first order. Offer the
newest valid result as a proposal and keep both the picker and inspection responsive during the
search. A manual choice made before the search finishes takes precedence. Search and selection
do not save settings until Begin Installation. The worker logs one end state with counts and time,
without file names. The worker test uses isolated search directories and verifies newest-first order, responsive inspection, manual precedence and an unchanged settings file. The Linux window test shows that the search continues while the folder picker is in use and that the result becomes a proposal.

The first-run screen shows free disk space next to the proposed folder and explains the space requirement
before confirmation. Use the same measurement as the worker's confirmation, including resumable
partial ROMFS space and the exception for an already complete image. Warn about the current
Windows path limit before a build. Folder inspection only reads; the confirmation still rechecks
disk space and the path. The worker logs numbers and flags without the path. The Linux window test
checks the displayed space and requirement. The worker fixture covers a complete image and exercises
the long-path distinction on Windows.

The first-run screen reads and saves the launcher's existing Check for Updates at Start and
Stable or Preview choices. On Windows it also offers the same Desktop Shortcut that Tk
offers and calls the installed stub to update it. Only explicit player changes write these
preferences. A held lock and an idle first-run installation are required. Leave unrelated
settings untouched and report accepted or rejected changes in the log. Saving update choices
does not start update activity while the Electron update flow is still pending. The automated
tests cover persisted choices, validation, platform availability of the shortcut, and keeping
other settings intact when first run is reopened. The Linux Electron window test changes the channel
and update check, then verifies both choices persist through installation.

Show the existing build prerequisites during first run. Reuse the selected runtime's
Zstandard check and the Linux toolchain's existing compiler and header probes. Inspection
reports a bounded result and leaves system packages, the destination and settings untouched.
On Windows, the install job retains its normal download and setup of the compiler.
Prerequisites are advisory while selecting the source and folder; the existing InstallJob
still checks them when the player starts the build. The tests substitute the existing probes
and verify missing and ready responses without running the game's build. The Linux window test
shows a stand-in missing header before the user starts the build.

Provisioning Python plus the active app on a machine without a launcher remains a later slice.

## Rebuild and interrupted-job recovery contract

On a ready, idle installation, Rebuild runs the same `RebuildJob` as Tk against the saved
game folder and dump. Require the worker lock, an existing playable build, no active session
or job, and no job marker from an earlier session. The worker rechecks external game activity
and lets the existing job protect the last finished executable. As with first install, display
progress and cancellation, wait for marker and claim cleanup before reporting a terminal
outcome, and return to the inspected game without starting Play. On success, honor the same
build-cache preference as Tk. On failure or cancellation, leave the last finished build usable.
A synthetic worker test checks refusal with an existing marker, final cleanup and cache policy.
The Linux window test confirms explicitly rebuilding returns to a playable screen. Mod rebuild
retains its separate, tested resume behavior.

An Electron worker can be killed while its build continues. Reacquiring the launcher lock after
that interruption establishes a new lock owner. The build's status needs its own proof. Leave the marker
in place and block Play and installation mutations until the build process group is confirmed
gone. The worker reports four states: no marker, verified live, verified gone, and unverified.
The Tk startup and its existing marker handling stay unchanged.

### Record and verify ownership before the build can write

Use a versioned marker extension and a launch handshake at the build process seam. Preserve the
current task, game folder and start fields. Tk builds keep writing the current marker format.
Test that the installed Tk startup can consume a worker marker and recover the same interrupted
game or installation. A small Python supervisor shipped inside the existing app registers the
group identity before the worker allows the build to start. It needs no new binary or dependency. Write the complete record atomically
before sending the start acknowledgement. EOF before acknowledgement means the supervisor exits
without starting a build. The supervisor holds the build process handle, the launch control channel until acknowledgement,
and the files for build output and terminal status. The worker reads output independently so a
broken UI or worker pipe cannot abort a useful compile. A normal worker close still cancels
through the current path. The initial supervisor test runs an inert command in a scratch run
directory. It verifies that worker EOF before approval leaves the command unstarted. After
approval the output and terminal result must survive the disappearance of the control channel.
This standalone test passes locally on Linux. Wiring the supervisor into builds and verifying
recovery after killing the worker are separate steps.

The next test exercises a real worker job with a stand-in compiler. The compiler reads the
marker at its own startup and proves the supervisor identity was published first. After the
worker is killed, the compiler must remain alive, finish when released by the test, and leave a
terminal result in its temporary run directory. The same extended marker must remain consumable
by the Tk recovery function, which uses the original task fields. Ordinary cancellation still
waits for the build termination before releasing claims. Local Linux verification passed for
the published identity, the refusal of a second Play while the builder lives, and the terminal
result after killing the worker. The Tk marker reader consumed the result's extended job marker. Only the supervisor records the builder's terminal result after the
verified group drains, then exits as soon as that record is written. Missing terminal status
means the supervisor may have died and live group ownership must be checked.

On Linux, put the supervisor and descendants in a dedicated session and process group. Record
the leader's PID, boot identity and process start time from `/proc/<pid>/stat`. A matching,
live leader keeps the build in the live state. Before declaring it gone, confirm the dedicated
group is drained, including when descendants survive the leader. A tracked PID with a different
start time is gone, and an unrelated group that reused the PID cannot be stopped. Ask the identity-verified supervisor to send SIGTERM to its own session's process group first.
Allow 5 seconds for the compiler to stop. Then signal only verified remaining descendants by
process descriptor with SIGKILL. Hold live process references so a reused identifier cannot
redirect cancellation. If the supervisor is gone, signal only individually verified members.
Treat insufficient visibility or ambiguous remaining members as unverified. See [Linux process
identity](https://man7.org/linux/man-pages/man5/proc_pid_stat.5.html) and [process
descriptors](https://man7.org/linux/man-pages/man2/pidfd_open.2.html).

On Windows, create a fresh named job object before the supervisor starts the compiler. Construct the
name from a digest of the normalized install root and a fresh random token stored in the marker.
The name contains no path or account string. Record the name and generation and admit the supervisor and its descendants without breakaway or
kill-on-close. A reopened object with active members proves the build is alive. A confirmed missing
object or zero active members proves it is gone. An access error leaves it unverified. Stop uses a
handle to that same job object and waits for zero active members. If creation or assignment
is refused by an outer job or the running system, the build still runs. Mark its identity as
PID plus verified process creation time. Keep the supervisor alive through the actual build,
watch it by handle, and stop only proven members. When the fallback has no terminal status and
cannot account for descendants, treat it as unverified. See [Microsoft job
objects](https://learn.microsoft.com/en-us/windows/win32/procthread/job-objects).

### Inspect an interrupted build

Inspection classifies the marker under the worker lock, before returning the Play controls.
Only a verified empty build group, boot change for a valid Linux group identity, or replaced
process identity with the original group confirmed gone allows automatic recovery. Unreadable,
legacy, inaccessible or ambiguous identities stay in place. Inspect reports a short reason and
a normalized job kind, without sending the marker, PID or path to the window. A second worker
must report the synthetic compiler as alive after a killed first worker, then report the
interruption after the build finishes. A test with a reused PID leaves the unrelated process
running and a test with a corrupt marker keeps its bytes intact. These scenarios passed
locally on Linux. The Linux window test shows the unverified state with Play disabled.

### Wait, stop or recover

Wait and Stop are distinct recovery requests and have no effect on a build owned by the
current worker. After either request, the worker monitors the same recorded build identity
without requiring another request from the window. Its sequenced recovery event carries an
installation snapshot when the state changes. A stop refused by the OS leaves the marker
and Play blocked. The test sends Wait before releasing a stand-in compiler, and verifies
the completion notification without polling. A separate Stop test confirms the synthetic
compiler had begun and that stopping it leaves the game executable untouched. The Linux Electron
window test exercises both controls against a stand-in builder and re-enables Play after cleanup.

The window shows when the earlier build is still running. The player can wait while keeping the
launcher open, or confirm Stop Build. Wait watches for the group to exit. Stop targets only the
verified group and waits for it to drain. Play, install and rebuild stay blocked until then.
Confirmed gone consumes the marker through the same recovery rules and messages as Tk. A
playable build returns to the last finished game and offers rebuilding. An unfinished installation
returns to Begin Installation, which resumes the completed steps. A reused PID must leave the
unrelated process untouched.

An unreadable or legacy marker with no verifiable identity stays blocked. Offer Wait and explicit
recovery, without a Stop action. Record the boot identity when the worker first observes this
marker. A later full restart, confirmed by a different boot identity, permits normal recovery.
Before that, the recovery control requires an explicit acknowledgement, "I know no build is
running". Record that as a player confirmation, distinct from platform proof. Never signal a PID
parsed from an unverified record.

Bind the explicit acknowledgement to the marker observed by that worker. A marker replaced
while the confirmation is open must be inspected again before confirmation can proceed. Store
a private hash of the observed marker and the current boot identity so a different boot only
recovers that same marker. Linux uses the kernel boot ID. Windows uses the OS-reported restart
time in UTC from Win32_OperatingSystem. If either the boot source or marker identity cannot
be read, retain the marker. The Windows query is cached within the worker and has a bounded
timeout. See [Win32_OperatingSystem.LastBootUpTime](https://learn.microsoft.com/en-us/windows/win32/cimwin32prov/win32-operatingsystem). The test simulates a new boot identity with the same marker and
checks the explicit confirmation separately, without rebooting either CI runner. The Linux
window test verifies that dismissing the confirmation preserves the marker, and that accepting
it returns to recovery of the installation.

Log the state and reason on first observation and on each change, plus a wait, stop or recovery
request and its result. Use fixed tags and counts, with no path or raw marker contents. The
Electron launcher tests workflow must kill the worker while a stand-in build stays alive and
exercise wait and stop on Linux and Windows. Cover a descendant that outlives the leader,
stale and reused identities, unreadable markers, failed access, the Tk fallback consuming a worker marker,
and a worker death around the
start acknowledgement. Force Windows job assignment to fail in its own fixture so PID mode
is exercised even when the runner allows nested jobs. On Linux, kill the supervisor while its
builder is still alive and refuses SIGTERM. The recovery stop must observe the remaining
session and wait for termination after the five-second grace interval. Check the on-disk marker, untouched unrelated process and usable
installation after every recovery.


### Private operations test packages

Use the same branch commit for both artifacts. The packages contain compiled TypeScript,
the Python 3.14 worker bytecode, and Electron with its upstream notices. They reuse the
selected installation's Python and tools. The extracted Linux package passed under Xvfb against a scratch installation assembled from the
published 0.27.0-rc.1 app and runtime. It inspected the root, persisted the hide setting, played
a stand-in game, showed its fatal message, left the crash offer, recovered a corrupt marker
after confirmation, and recognized a synthetic first-run dump without editing the settings.
The Windows ZIP passed archive integrity and contents checks. Its packaged window has not
been run in this checkpoint. The Windows and Linux jobs for commit `c91e8cf4` passed the
Electron launcher tests workflow run `45730016151`; the Windows job tested the worker natively.
No package includes the selected installation's runtime, toolchain or game. The owner can
exercise first run on a provisioned launcher, explicit Rebuild and the previous Play checks.
The Portuguese LEIA-ME in each artifact lists the exact entry point and the launcher and
game logs. This checkpoint leaves standalone provisioning, the transition and update
implementation pending.


Both private packages use commit `c91e8cf4`. Verify the owner-run evidence separately.

- Linux `NX2Saria-Electron-Worker-Linux-x64-c91e8cf4.tar.gz`, 122,371,108 bytes.
  SHA-256 `b0aa5f2043a4fd75bc0c3cbaee29307b72236f84960e6ab8e5b1725c2bcc005b`.
- Windows `NX2Saria-Electron-Worker-Windows-x64-c91e8cf4.zip`, 146,991,939 bytes.
  SHA-256 `b754a593afdc67a4513acfd810e648bea9353f55fc0b00eaaeb902427a705a47`.


## Update availability check

The first step of the update flow is a manual release check. Use the existing `CheckJob` with
the selected installation's feed and channel. A worker-owned check runs in the background
so the window can still inspect the installation. Coalesce concurrent requests and keep the
existing one-minute pause between forced checks. Treat failure as a visible status and log
the outcome. On success, retain the existing last-check time and last-seen version while
preserving unrelated settings.

The worker sends a version, the availability status and at most 20 plain-text note lines. Each line
is capped at 512 UTF-16 units to fit the protocol frame even when JSON escapes the full text.
The Electron client checks the byte limit per frame while the pipe can combine frames.
The feed token, asset URLs and download headers stay in Python. This slice reads release
metadata. Staging, automatic scheduling and activation retain their separate contracts.
Use a loopback feed and assert that a delayed metadata response leaves inspection responsive,
only one manifest is requested, and the staged files and active app/runtime are unchanged. The Linux Electron window test
opens the manual check and renders the release version and notes from a loopback feed.
The worker continues to use `CheckJob`. A GitHub-style loopback feed
returns an older eligible entry before the newest release. The worker
fetches only the newest manifest, exposes its version and records the
feed-order diagnostic once in `launcher.log`.


## Saves operations

The first Saves operation is export. The worker uses the configured game and the same export
directory preference and Downloads fallback as Tk. The player can choose an existing export
folder and toggle Add Log and Settings. Save only the requested preference. Keep the lock for
each launcher write and block export while a previous build has an unresolved marker. A running
game alone does not block export because the existing export reads the newest complete save.

Export calls Python's existing archive writer. It shows the filename and number of exported
files, and reveals the ZIP. With the log option on, keep the current log-only fallback for a game
that has not saved. With it off, report the existing no-save condition. The ZIP excludes key
files and uses the existing settings scrubber for a report. Selection cancellation preserves
the previous directory. The synthetic worker test verifies both variants, including redaction.
The Linux Electron window test exercises the toggle, folder chooser and export against a
stand-in save. Both export tests passed locally on Linux. Import will use the same Python validation and recovery as Tk in its own test.

A ZIP import uses the newest waiting entry in the game saves folder when one is available,
and otherwise opens the native file picker. Folder import uses the native folder picker and
allows a whole save or one slot. Cancelling a picker leaves all files intact. Show a waiting
in-game import when present and provide the existing Open Saves Folder action. The worker
rechecks for a running game and relies on the existing profile lock before changing saves.

Use the existing Python ZIP validation, transactional replacement and backups. An invalid
archive leaves the active save and pending import intact. A successful import reports the
file count, slot where applicable and backup name. The same saved backup can be selected
again from the folder picker. Save import and export wait for the final response even
when a backup or copy exceeds the normal worker request timeout. Prove this with synthetic folders. Interrupt the replacement
after the old folder moves and verify recovery returns it to the active path. Test archive and
folder selection in the Linux window and exercise the worker on Linux and Windows. The Linux tests passed for archive and folder import,
validation, backups and rollback after a simulated filesystem error.

## Mods operations

Reuse the configured mods folder and the Python scanner. Show each mod's enabled state,
load order, file and patch counts, unsupported cases and the first three conflicts with
their winners. Paginate the list so each response fits the worker protocol. Inspecting
the list does not create or rewrite the mods folder. An explicit Open Mods Folder
request uses the existing desktop action.

Add archives or folders through native pickers. Cancellation leaves the installation
unchanged. Reuse the existing safe extraction, naming and folder normalization. Toggle
only a currently present usable mod and move a currently present mod one place through
the existing load-order APIs. The worker checks for a running game before either change.
Adding another mod remains available while the game is running, as it is in Tk. Mod names
and paths stay out of the Electron log. Python retains its existing shortened diagnostics.

The existing Play flow detects enabled code that needs a rebuild, tracks cancellation and
resumes Play only when the build has finished and released its claims. Tests use synthetic
files to prove import from an archive and folder, conflict resolution, game-running refusal,
persistent ordering, and rebuild after enabling a code mod. The Electron window test uses
scratch pickers and shows invalid inputs without changing the visible list.

### Private Saves and Mods test packages

Both packages use commit `f53b2481`. The included LEIA-ME lists Saves and Mods and
where to find their logs. Linux launch, save export and import, backup creation and
mod addition passed from the extracted tarball under Xvfb. The temporary installation
used the published 0.27.0-rc.1 app and runtime and a stand-in game. The Windows ZIP
passed integrity and content checks. This specific ZIP still awaits a native window
test by the owner. The worker CI already ran on native Windows and Linux.

- Linux `NX2Saria-Electron-Worker-Linux-x64-f53b2481.tar.gz`, 122,388,221 bytes.
  SHA-256 `f4bb9c5c6ca92524ffc8def60c8e55c38e31f18103571223b4b8cc6cc224d5c0`.
- Windows `NX2Saria-Electron-Worker-Windows-x64-f53b2481.zip`, 147,010,377 bytes.
  SHA-256 `b086918a5ba936529301c2570ff5c2ed207b0912bf2ad4dc473adada197cd59c`.

## Installed launcher settings

The Settings panel shows the installed game's folder and registered dump, opens the
game and launcher log folders through the existing platform opener, and changes the
registered dump through a native picker and the same title validation as first run.
Cancellation and an invalid dump leave the previous selection in place. A valid
selection changes only the dump setting. It takes effect when the game is rebuilt.

Read and write the existing channel, update checks on startup, automatic application
of updates, and build cache settings. Include the low-commit warning on Windows,
with the existing memory decision on the next Play request. Keep the existing control
for hiding the launcher while playing. A successful preference write changes only
that key and preserves unknown fields. The panel describes the update preferences
as choices for the installed launcher while activation is deferred. This slice
does not schedule, download or apply an update. The existing manual check still
uses the selected channel and discards a result for an earlier channel.

The worker validates the allowed keys and values and keeps writes behind its lock.
Block changes during an owned game, build or unresolved job marker. Opening the
log folder from Settings remains possible during recovery on an installed game. The worker returns only the requested
view, without feed, account or report secrets. Settings changes use the existing
shortened Python log. The window records operation names and outcomes. Test the
view, typed writes, cancellation, invalid dump refusal, preservation of other keys,
and folder open on synthetic installations. Test the Linux window and exercise
the worker on both CI runners. Report settings, platform administration and
uninstall retain their own acceptance evidence.

## Session Reports in the Electron launcher

A private package starts with network submission disabled. Only
`NX2SARIA_ELECTRON_TEST_SEND_REPORTS=1` enables submission for that invocation.
The switch applies to the automatic session report, explicit comments and the
crash-log offer. It does not change the persisted Session Reports choice or the
existing service address. With the switch absent, a pending report remains on disk
for the installed launcher and the worker records that submission was deferred.
The LEIA-ME names the switch and explains this handover.

When the first ready home screen is shown, run the existing startup checks once
per worker. A normal start opens the existing 30-second question if due. The
first home screen following install or update opens the question without a
countdown. Actual input, including opening the exact preview, stops the countdown.
The affirmative answer, timeout, deliberate window close and refusal go through
`Reporter.answer_question()`, and move the waiting summary aside exactly as Tk does.
An abrupt process exit does not synthesize an answer. The Settings toggle uses
`Reporter.set_consent()`. On is eligible to send pending reports when the test
switch is enabled. Off sends none. Show the same unavailable and retired-service
states as Tk.

Use `Reporter.prepare_session()`, `post()` and `finish_session()` with the existing
payload and sent marker. At startup and after a confirmed game end, submit the
current pending summary if eligible. A report in flight holds its prepared bytes.
An earlier completion must never move a newer pending summary; if another session
finishes in the meantime, prepare and send it using its own summary. If the window
closed during the game, the worker can finish sending for an already enabled
session and then release the lock. Failed sends follow the existing server
response policy. The player sees progress and results for explicit comments.
The worker applies the current text and optional log limits, privacy filtering
and consent to attach the summary. A successful comment clears the draft and a
failed one leaves it available to edit or retry.

Use the current pending summary or the existing example for a read-only session
preview, serialized by the same `payload()` and `body()` functions as Tk.
Transport large preview text in bounded frames while preserving the exact
UTF-8 bytes. Tests use loopback only. Compare the bytes from the worker with the
ones sent by Tk for the same summary and confirm the preview matches. Exercise
both directions of duplicate prevention using the shared sent marker, consent
answers, startup and post-job timing, switch disabled, Off, comments and HTTP
refusal. Validate the Linux window and run synthetic worker cases in both CI jobs.

Crash-log offer display, preview and decisions form a separate change. Keep the
existing order after the session-report question, the log identity, opt-out and
input guard. Use the same worker switch and give the reason when Send is disabled.

## Crash-log offer in Electron

Use the active installation's crash-offer feature when present. Keep the existing
eligibility and marker writer, and use the same pending-marker validation so a
different log can never be offered for the ended game. The home screen looks for
an offer after the session question, and after a game ends when the worker still
has a window. A window closed before the game ends leaves its new marker for the
next launcher. A game running outside this worker blocks the offer.

Show the failure, counts and what would be included. Read the exact prepared
request into the preview over the same bounded byte transport. Ask After a Crash
uses the existing settings key. A one-second input guard protects the offer
from queued game input. Preview can return to the same offer. The deliberate
close and decline use the existing marker and logging policy. Write a Comment
opens the existing comment flow with the attachment selected.

The private-build switch gates Send the Log and all comments, even with reports
already enabled. Keep the offer and preview visible when sending is disabled,
with an explanation beside the disabled action. A send with the switch enabled
uses the worker's prepared request, forgets the marker once and calls the same
network finish policy as Tk. No automatic retry. Show the current-version retired
state and stop offering once the server refuses a crash log in this run.

Prove with scratch app and log fixtures, including a preview larger than one
frame, exact bytes at a local service, rechecking a changed log, opt-out,
guarded input, a declined or consumed marker seen by the other launcher, and a
server refusal. The Electron window verifies the order, preview and disabled
action. Use published Linux app/runtime for portable validation.

### Private Settings and Reports test packages

Both private packages use commit `335a0ab0`. The LEIA-ME covers only Settings,
Session Reports and the crash-log offer. The Windows and Linux worker jobs
passed in run `45762522567`. The extracted Linux package passed under Xvfb
with the published 0.27.0-rc.1 app and runtime, including a visible crash offer
and its exact preview after a synthetic fatal exit. The Windows ZIP passed
integrity, contents and checksum checks. Its native window test is for the
owner. No real sessions or submissions were used.

- Linux `NX2Saria-Electron-Worker-Linux-x64-335a0ab0.tar.gz`, 122,410,912 bytes.
  SHA-256 `706fa674e3d5e245bc7e5797660da9d59ad23e1643a56b6eab103e0ac4912bf0`.
- Windows `NX2Saria-Electron-Worker-Windows-x64-335a0ab0.zip`, 147,034,494 bytes.
  SHA-256 `9e1295cc1c138ba44b5d0df1f42e337ae35ad9efc6122335563a41ab4d7b2245`.

## Adding and updating a Steam shortcut

Show the installed Steam status and whether the existing game shortcut is
already present. An explicit Add to Steam request goes through the worker
and the existing `steam.add()` code, including game artwork when available.
Keep the headless Python entry point, the existing shortcut ID and the other
shortcuts. Point Linux shortcuts at the installed wrapper and Windows
shortcuts at the installed Python Play script. The private Electron binary
is not a Steam target.

On Linux, ask the player to close native Steam before continuing and leave
it closed afterwards. An installation with only Flatpak Steam displays the
existing unsupported message. On Windows, the action may close and restart
Steam as it does in Tk. Show progress while the worker writes the shortcut,
and prevent duplicate requests. An operation accepted before the window
closes finishes under the worker lock. Track detection and result without
Steam account identifiers in the Electron log. Leave the existing Windows
and Linux headless Play and job-lock behavior intact.

Exercise the worker on scratch Steam roots in both runners using the real
VDF update and simulated Steam processes and artwork. Verify account or
wrapper refusal, existing ID preservation, unaffected shortcuts, Linux
open-Steam refusal, and the Windows restart path. The Linux window test
uses synthetic Steam and a stand-in game. Native Steam Deck Game Mode
remains a device check.

## Personalizing the launcher

Use the existing color schemes and three custom color slots. The worker reads
and writes the same settings keys, uses the Tk color-palette helpers, and
returns only validated colors, display options and the saved picture's name.
Changes apply to the Sanctuary layout without replacing it. Keep the same
five dimming choices, the animation toggle and four sound levels. A malformed
preference displays the existing fallback. Saving one choice preserves other
settings, and resetting a color clears its override.

Choose a picture through the native picker and decode it through the existing
Python picture limits. Store the normalized image in the same
`data/background.npy` as the installed launcher. Cancellation, a bad image or
a rejected settings write leaves the previous picture visible. Removing the
background only removes the retained copy. The renderer receives a scaled PNG
of that copy over bounded frames. It never opens the original path itself.
If the saved image is unavailable, use the selected scheme's backdrop.

Reuse the installation's cached game fonts and effects, and the existing
extractors when they are absent. Read and extract only from the selected
game or the saved dump. Missing assets leave the default fonts and a silent
interface. Effect volume and selection/back cues use the existing levels.
No game-derived media is in the package. Web accessibility preferences can
suppress motion even when the launcher setting is enabled.

Test with generated images and sound/font stand-ins, including the native
pickers and renderer transitions on Linux and the shared settings and caches
on both CI runners. Capture detection and writes in the existing logs
without picture or game paths in the Electron log.

### Private Steam and appearance test packages

Both packages use commit `211f43a2`. The LEIA-ME covers Steam and appearance.
The Electron launcher tests passed on Linux and Windows in run `45797646341`,
including the Windows picture decoder and synthetic Steam restart. The extracted
Linux package passed with the published 0.27.0-rc.3 app and runtime, including
the background chooser and cache. The Windows ZIP passed integrity, contents
and checksum verification. Native Windows package interaction and Steam Deck
Game Mode remain device checks.

- Linux `NX2Saria-Electron-Worker-Linux-x64-211f43a2.tar.gz`, 122,429,208 bytes.
  SHA-256 `ff55588f0b7d50669fe3e4f3c3e3f7383c2fd84dfa98d4ba498528d9e90c02ba`.
- Windows `NX2Saria-Electron-Worker-Windows-x64-211f43a2.zip`, 147,053,522 bytes.
  SHA-256 `63a372374d340098bdb3605600079646aef1ffed87be2afa6da2c0f6a55f3b27`.

## Keyboard, controller and language

Keep the browser's native keyboard controls, focus order and field semantics.
Every visible action must be reachable at the minimum window size and while
zoomed. Focus remains visible and follows the active panel or modal. Dialogs
restrict focus to their active controls and return it on exit. Escape and
controller B follow the same back routes. Preserve text editing and native
selection keystrokes.

Use Chromium's standard gamepad mapping for D-pad, left stick, A and B. Move
focus in the active screen, repeat direction deliberately, and accept or
return on a single press. Ignore input while the window is unfocused or
hidden. Return events and activation honor the same disabled controls and
crash-offer guard as mouse and keyboard. Exercise these boundaries with
synthetic gamepads in the Linux window. Steam Deck remains a hardware check.

The launcher has four UI locales: English, Brazilian Portuguese, Russian and
Simplified Chinese. Use the supported language matching the system initially,
with English fallback and an explicit installed-launcher override. The
override belongs to Electron and does not change the game's language or
rewrite Tk settings. Localize controls, status messages and native dialog
labels with stable keys. Keep player data, diagnostic logs and release notes
in their original text. Verify key completeness, formatted messages and
layout bounds in each locale.

### Private language, input and uninstall test packages

Both archives use commit `4e1c4e31` and contain the launcher artifacts and
notices. The Electron launcher tests passed on the Linux and Windows runners
in run `45802434603`. The extracted Linux build passed the window suite for
the published 0.27.0-rc.4 app and runtime. The added packaged uninstall test
cancelled with Escape and kept the scratch game, save and dump after confirming
the default choice. The ZIP passed archive and checksum validation. Native
Windows package interaction, hardware controller use and a production
sandbox check remain open.

- Linux `NX2Saria-Electron-Worker-Linux-x64-4e1c4e31.tar.gz`, 122,367,434 bytes.
  SHA-256 `7a4e2db968b1d0947b90aaf5a5139bf40f0474c69179d9cd4608a0d8432ebb00`.
- Windows `NX2Saria-Electron-Worker-Windows-x64-4e1c4e31.zip`, 147,000,095 bytes.
  SHA-256 `c51568c7e838a0ee247919a71fcbc7fa369bc0afe99dbd171485307fc96ba3d7`.

## Platform and game preferences

Settings exposes the game's Discord activity switch using `discord_rpc` in
the selected game's INI. A read leaves the original INI in place, including
the legacy file; an explicit write uses the existing game INI writer and
preserves its format and unrelated options. Treat a missing value as On.
The worker refuses the write while a game or job owns the installation.
Describe the optional player-supplied application ID without reading or
showing it. Logs contain the switch value and write result, never the ID.

Linux also offers the same optional udmabuf apply or remove operation as
Tk when the existing probe permits it. The window describes the change
and only Continue invokes the existing PolicyKit helper. Reject requests
outside the current offer, while a game or job is active, or when the
installed app has no fix script. On completion, refresh the device probe
and show whether access and the required size are available. Tests replace
approval, the device and privileged commands. The development package
has no new privileged helper and never applies a setting automatically.

## Uninstall contract

Expose the installed launcher's Uninstall action in Settings. The confirmation
starts with **Remove the built game** off and explains that turning it on
removes the game's folder, saves and mods. Cancel or Escape leaves both trees
untouched. Show platform-specific consequences before confirmation. Linux
retains launcher logs. A Steam entry is removed from Steam itself.

Uninstall uses the install root already held by the worker and the game and
dump paths read from that installation's settings. The renderer supplies only
the explicit retain/remove choice. The worker refuses while it lacks the lock
or a game, build, interrupted job or other installation operation is active.
It also refuses checkouts and roots without the installed entry point. A busy
worker keeps the window open and tells the player what to close or wait for.

Use the current platform uninstall plan and deferred removal helper. Preserve
the original dump, the game directory when selected to keep and unrelated
installations, shortcuts and files. On Windows, exclude an owned deletion
candidate if it encloses a protected path. Refuse to remove a drive root,
another launcher, or a game folder containing the original dump. A confirmed request schedules the operation before Electron
closes. Log the selection and result without paths in Electron. Keep the
existing uninstall diagnostic in the worker log. Standalone private shells
remain outside the install tree. Removal of the eventual provisioned shell
belongs to provisioning.

Test cancellation, keep and remove on scratch roots for both platforms,
including an original dump inside a candidate directory, unrelated files,
legacy names, an interrupted job and a running game. Exercise the UI choices
with a synthetic install. The private build is for verifying the confirmation
and cancellation. Automated scenarios perform the actual deletion.

## Provisioning and handover design

The approved direction reuses the Windows setup and stub and the Linux AppImage's external installer. The
following entry points are proposed for review. Keep the existing launcher and its entry points during the
transition. Test provisioning, handover and recovery with a scratch installation and a local feed.

### 1. Components and versions

Make the Electron shell a third, platform-specific artifact. The app ZIP contains the compiled interface
and its matching Python worker. The Python runtime ZIP retains its independent ID. The shell archive
contains the Electron distribution and identifies its Electron version, target platform and architecture,
launcher interface version, archive SHA-256 and size. The app declares which shell interface it accepts.
A compatible app-only update reuses the installed runtime and shell by digest. A security update can
stage a new shell digest against the same app version. Each installation has one active copy per
component, with the previous verified copy retained only for rollback. The shell runs the active app;
the app and its worker continue to ship together.

### 2. Shell update and rollback

The installed Python updater owns downloads and staging. It verifies the complete archive, safely
unpacks it into a sibling staging directory and only then records the digest as ready. Keep each
verified shell under its own digest. The Windows stub enters the active app's Python start script, and
the installed Linux wrapper enters the same script with the installed Python. After Electron and its
worker exit, that startup path is the activation seam. It checks the installation lock, selects the
candidate, and releases the activation lock before the new worker acquires the existing one. A busy
worker is reported as busy, without trying to open a second launcher.

Record a pending activation and previous known-good selection before attempting startup. The new app
must complete the worker handshake and prepare its window before committing the shell choice with an
atomic state-file replacement. A power cut before commit leaves the prior choice intact. On timeout,
failed handshake or early exit, wait for the failed attempt to release the lock, restore the previous
choice, and start Tk. A missing shell does not count as an app rollback. The current app and Python
update and rollback remain in the existing staged-updater path. For a combined change, test a journal
that lets the native stub or wrapper recover the old app and Python before invoking Python if their
swap was interrupted. Keep the Tk entry and previous runnable set until the new set commits. Prove
interruption before and after each pointer change, an executable held open on Windows, and shell-only
and combined rollback.

The trial counts as the first of three successful normal starts. Count each new process once after the
handshake and window readiness are committed. Starting Tk explicitly leaves the count unchanged. A
failed startup resets the streak and retains the previous shell. Prune the previous shell after three
consecutive successful starts of its replacement, once no process uses the previous digest and no
activation is pending. Keep a runnable Tk fallback for older apps that may still be selected by app rollback.

### 3. Existing installations and the first visible launch

An existing Tk install first receives an ordinary compatible app update containing the bridge logic.
The earlier updater can apply that app ZIP without understanding the new shell field. The bridge
then stages the shell on a normal update check, including when the app version is already current,
using the existing channel and automatic update preferences. Activate at the next normal start when
the installation is idle. Preserve the menu and desktop entries, the selected game folder, profiles,
saves, mods and settings. A fresh Windows setup or Linux external installation provisions the same
app, runtime and shell; it then launches through the same entry point as an updated install.

The player keeps using Tk while the shell is missing or pending. A missing or incompatible shell,
early exit or failed worker handshake opens Tk and logs a bounded reason. Successful activation opens
Electron with the existing installation selected. During the transition, a `Launcher UI` setting in
`data/launcher-ui.json` persists `"ui": "auto"` (the default) or `"ui": "tk"`. The separate file survives
updates and rollbacks even when an older Tk version rewrites `launcher.json`. A `--tk` flag forces Tk
for one launch and leaves the saved setting as it was. The switch works through the Windows stub
and Linux wrapper. Headless Steam and CLI keep their Python entry points. Keep Tk in the app until
the replacement gate is approved.

### 4. Provenance

Build app, runtime, shell and installer or AppImage artifacts on the existing GitHub Actions release
path. Include the exact official Electron archive SHA-256 for each platform and check it before unpacking.
Pin actions and Node. Resolve npm dependencies from the lockfile and suppress Electron's automatic binary
download in the release build so the verified input is the only source. Include the shell artifact and
its identity, byte count and digest in the checked manifests and `SHA256SUMS.txt`. Attest the artifact
and verify release provenance through the same workflow as the current files. This design does not use
locally edited or uploaded installer, feed or release assets. See [release pins and provenance](releases/README.md).

### 5. Size and retention

The private x64 packages from commit `fcfbfc6f` are the measured baseline. Windows was 158,994,219 bytes
to download (151.6 MiB) and 375,463,862 bytes of files unpacked (358.1 MiB). Linux was 124,576,200 bytes
to download (118.8 MiB) and 288,132,563 bytes unpacked (274.8 MiB). Both included 1,135,513 bytes of app
and worker files and no additional Python runtime. These totals measure the actual test bundles.

Ship an x64 shell per platform and reuse the installed digest across app versions. Keep the Chromium
locales needed by the four menu languages (`en-US`, `pt-BR`, `ru`, `zh-CN`), including the English
fallback, after checking the actual dialog and rendering paths. The remaining locale files account for
48,085,006 unpacked bytes on Windows (11,975,506 compressed ZIP entry bytes) and 7,523,052 unpacked
bytes on Linux. Repacking the two private artifacts with those locales removed and the app and worker
omitted measured these standalone shell files:

- Windows x64 ZIP. **139.7 MiB** download (146,480,725 bytes), **311.1 MiB** unpacked (326,241,507 bytes).
  Peak for this shell on first install is 450.8 MiB. Staging while keeping an active shell for rollback
  peaks at 762.0 MiB. If a previous shell is already retained and a newer candidate arrives, the peak
  is 1,073.1 MiB.
- Linux x64 tar.gz. **116.2 MiB** download (121,883,208 bytes), **266.5 MiB** unpacked (279,471,924 bytes).
  Peak for this shell on first install is 382.8 MiB. Staging while keeping an active shell for rollback
  peaks at 649.3 MiB. With a previous shell also retained, the peak is 915.8 MiB.

These are shell-only estimates for disk preflight. They add the archived candidate to the unpacked
candidate, active shell and previous shell as applicable, using equal shell sizes for the estimate.
They exclude the game, app, Python, caches and filesystem overhead. Use the actual component sizes when
calculating required space. Verify the filtered artifact and its four language paths in CI.

The checksum-verified Linux 0.26.0 AppImage without a seed is **97.9 MiB** (102,660,600 bytes). Appending
the filtered Linux archive to a scratch copy of its SquashFS, keeping the same zstd compression and
ELF prefix, measured **213.7 MiB** (224,094,712 bytes), an increase of **115.8 MiB** (121,434,112 bytes).
The recommendation is to leave the shell seed out of the normal AppImage. The scratch image was used
only for size measurement.

### 6. Notices

The shell ships Electron's MIT `LICENSE`, the complete `LICENSES.chromium.html` covering Chromium and
bundled components, and the applicable Node.js notices from that distribution. The app and runtime ship
their own project and Python notices. Update the inventories for the exact delivered versions and extend
`shipped_notices_check` to verify the actual artifacts. See [Electron license inventory](electron-launcher-notices.md)
and [third-party notices](../THIRD_PARTY_NOTICES.md).

### 7. Linux sandbox and Steam

Install the shell outside the mounted AppImage, beside the external app and runtime under the managed
root. The AppImage remains the provisioner. Prefer downloading the verified shell separately and keep the
offline seed for a separately approved distribution need. The installed wrapper always opens the external
copy. Keep renderer sandboxing and context isolation enabled. Test normal user namespaces and a distro
that restricts them. Never add the production `--no-sandbox` flag or make a helper inside a user-writable
tree setuid. Any distro-supported policy or root-owned `chrome-sandbox` path requires separate review
and explicit approval. If sandbox startup fails, log the reason and start Tk. [Electron sandboxing](https://www.electronjs.org/docs/latest/tutorial/sandbox) · [Chromium and restricted namespaces](https://chromium.googlesource.com/chromium/src/+/main/docs/security/apparmor-userns-restrictions.md)

Test the external entry in Steam Deck Desktop and Game Mode, including focus after the first game
window and after exit. Flatpak Steam is a distinct boundary. Keep the existing launcher and game's
headless shortcuts working with only their already authorized paths. Test the external GUI from
Flatpak Steam separately and surface an unavailable sandbox or entry point without asking for broad
host permissions. A working desktop launch does not complete either Steam scenario.
[Flathub Steam wrapper](https://github.com/flathub/com.valvesoftware.Steam)

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
- Acknowledge Play immediately with an opening status. Keep the launcher visible until the running game's window reports focus. Show the relevant result if the game fails to start or ends before its window opens. Verify the delay with a synthetic game and record real Windows behavior when the next build is tested.
- Treat an in-game Restart as part of the original Play session. Keep the installation locked, emit no second start or intermediate end, and report the last copy's exit code under the original session ID. Record once when the worker observes the restart. A synthetic executable starts a second copy of itself and holds that copy open so the test can prove the lock stays held before asserting the final result.
- Keep the operating-system launcher lock and job ownership consistent with `launcher/locks.py`; define which process owns it, what happens on UI or worker failure, and how a second launcher is refused. Do not have two independent processes contend for the same instance lock without a designed handover.
- Interface design must describe request identity, event ordering, cancellation, errors, process shutdown and restart; avoid making the renderer responsible for privileged file or process access. Transport and exact messages remain undecided.

### Evidence and acceptance

- Automated synthetic cases cover current and legacy root selection; ready and missing installations; interrupted rebuild with a still-playable old executable; already-running game; failed start; process exit and restart; pre-play decisions; lock contention and worker failure. Use fakes and temporary directories, without private game data.
- Run the worker and Node tests on native Linux and Windows CI runners. On Windows, compile only the stand-in executables required by the worker tests and supply them to the tests. Include the existing Xvfb Electron window suite on Linux. The test workflow runs only for launcher-related changes pushed to `electron-launcher`, uses pinned tool versions and read-only repository access, and does not package a release or build the game.
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
- During the initial Play milestone, the worker does not check, download or install updates, and does not call `BackgroundUpdate` or `install_staged`. It detects and logs a waiting update in the handshake and again after the game; the interface tells the player to open the installed launcher to apply it. A synthetic test compares the waiting marker and staged tree before and after Play. Handover, installation and rollback remain a separate project.
- The new code does not create a `Reporter`, send reports or ask the consent question. A session summary stays on disk for the installed launcher to process under existing consent. `finish_game()` may synthesize a crash summary locally when reports are enabled. A synthetic session must leave the expected summary and produce no requests to a fake server. Full Electron consent and reporting behavior remains a parity gap.
- A job is terminal only when `finished` is set after marker cleanup and release of claims and its nested lock; `result` alone is insufficient. A `FileLock` with `key is None` means the lock could not be opened: proceed as today, log it and report it in the handshake.
- After a delayed memory decision, the worker rechecks the current attempt identity, its own game session and any external game before spawning. Stale answers are ignored; an occupied state receives the same refusal as the initial check. This additional recheck is for the asynchronous worker, not the modal Tk extraction. Include a synthetic race test.

### Worker Play: protocol and event loop design

The worker reserves the original stdout byte stream for UTF-8 JSON frames and redirects ordinary Python stdout to stderr before loading or calling core operations. A single write lock protects complete frames. A dedicated reader thread reads bounded requests and places requests, malformed-frame failures and EOF in a queue; the main loop consumes that queue and periodically observes the active `GameRun`. All responses and unsolicited events use the same serialized writer; events carry a strictly increasing sequence number and a session identity. This allows game-end events while no request arrives and prevents concurrent writes from interleaving.

EOF while idle exits; EOF during a tracked game disables further output and requests, continues monitoring through `finish_game`, and releases the lock only after the game ends. Once jobs are integrated, EOF requests existing job cancellation and waits for `finished`, including cleanup, before exiting; game and job shutdown are independently covered by synthetic tests. The worker never terminates a game because its input or window disappears. On Windows, Electron passes its main process PID to the detached worker. Before reporting ready, the worker opens a synchronization handle for its actual parent and checks that the parent was created before the worker, so a reused PID cannot keep it alive. A signaled handle follows the same shutdown path as EOF and logs `parent exit`. On Linux, killing the Electron main process closes the pipe and the worker receives EOF. An unavailable output stream is treated as a disconnect with the same lifecycle rules. `Context` has no public close method; the current private-file cleanup will be discussed separately with the coordinator before any shared-core change.

The first Play integration added synthetic Linux coverage for start and end events, EOF and malformed input during a game, and memory decisions including an outdated answer and an external game appearing before acceptance. A local TypeScript client can handshake, inspect and close a real worker in a synthetic Linux installation; the Electron window is connected locally for inspection and Play; a synthetic Linux window test passed for two consecutive games, hide/restore, reselecting or cancelling installation selection and closing the window during a third game, which then finished while the worker logged its end. A separate synthetic window case kills the worker: the window returns, Play is disabled and the game process finishes. The synthetic Linux window also displays the waiting-update notice after a game without applying it. The short-lived Play bridge and its obsolete tests have been removed; the UI test also checks job-marker refusal. The initial Linux window tests preceded job integration and the packaged Windows tests listed below. The memory warning keeps Play as the default and preserves the current fallback of continuing if the dialog cannot open; a synthetic test covers that fallback. The worker logs bounded Play, memory and spawn outcomes without paths. Inspection and Play results have validators that reject malformed states and discard unrelated fields before reaching the UI. Its synthetic tests also reject out-of-order events and an end for the wrong session, and disable further requests after a timeout without replaying a mutation. Synthetic Linux and Windows tests also confirm refusal of another Play while the worker is supervising a game or detects an external game. Broken output during the game and immediately before its end is also covered on Linux. A synthetic subprocess survives forced worker termination; a separate Linux test also uses the native spawn path with a temporary executable and confirms it survives forced worker termination. A Windows synthetic executable also completed after its worker was killed using the native spawn path. An initial Windows synthetic run failed because the stand-in child inherited the protocol streams. After matching the real game spawn by redirecting all three streams, the suite passed on Windows (27 passed, two skipped) and Linux (29 passed); The embedded Python from the previous Windows development package also passed synthetic handshake, inspection and game lifecycle tests with the current worker. A temporary packaged Electron window on Windows passed two consecutive synthetic Plays using an executable compiled from the test fixture and the embedded Python. Windows synthetic window hide and restore passed. A Playwright-driven application close during a third synthetic game left the game running to completion and logged worker EOF, but no worker game-end line appeared within 10 seconds; Closing the BrowserWindow directly produced the same observation. Starting the worker detached on Windows fixed the synthetic packaged-window test: the game finished and the worker recorded its end after the window closed. An isolated test with all three worker pipes closed also passed. Those first Windows tests used the embedded development interpreter and only exercised Play. A synthetic game session leaves the staged-update files unchanged and reports the waiting state on completion. Another synthetic session leaves its summary in place while a local fake telemetry server records no requests. Both scenarios passed on Linux and Windows. The later Windows package and job validation are recorded below. Electron inspection and Play use only the persistent worker. The standalone Python inspection command remains available for characterization tests.

The mod-job extraction was merged into main and the Electron branch includes it. Local integration starts a mod rebuild, emits bounded progress, accepts cancellation and waits for `finished`; successful completion resumes Play or asks for a memory decision, and EOF cancels the job. Unit tests use fake jobs and do not spawn a game. A forced worker exit leaves the job marker for a successor to inspect after reacquiring the lock. A Linux synthetic Electron window test covers successful mod-job resumption, cancellation and the next Play without repeatedly rebuilding the same mods. The Windows package failed before compilation when the window requested a rebuild for a synthetic mod. See the validation below. The Tk characterization records that it can observe `result` before `finished`; the worker deliberately waits for cleanup.

### Current synthetic validation and private test package

The merged Electron branch passed the complete Python suite under `xvfb-run`: 183 passed, zero failed and 25 skipped. The Electron unit suite and Linux synthetic windows passed, including mod rebuild success, cancellation and a later Play. The first private source-free Windows development ZIP was verified for archive integrity and tested in a packaged window against a synthetic installation: two Plays hid and restored the window; closing the window during a third left the stand-in game and worker to finish. The copy in the shared test directory has the same SHA-256 as the local archive. The ZIP includes the bundled Electron/Chromium/Node and Python license texts and development-runtime notices. Its final update after the window test changed only the notice file.

GitHub Tests and Content guard passed for branch head `4024d86d`. Three additional synthetic Windows scenarios exercised the packaged window:

- **Second window during a game:** passed. The first window closed and the worker logged `eof game running`. The same synthetic game PID was alive when the second window opened and when it displayed the busy message with Play disabled. The log showed one spawn and the worker recorded completion after the fixture ended the game.
- **Mod rebuild:** blocked by a package defect. A synthetic mod triggered `play status=rebuild` and `job started` through the window. The job returned `failed` before compilation. The bundled Python 3.11.9 lacks a Zstandard reader. The package also lacks the build tool at the path expected by its worker. This finding applies to the earlier ZIP with its own Python.
- **Forced exit:** the first Windows attempt killed the command wrapper reported by Playwright. A corrected test identifies the packaged Electron executable as the worker’s parent and kills that exact PID. The old and replacement ZIPs both passed: an idle worker exited and a new window took the lock. With a game running, the worker stayed alive and another window reported busy until the game finished. Linux passed the same test using SIGKILL and pipe EOF. An independent Windows test kept the writer end of stdin open, killed the worker’s parent and observed the native `parent exit idle` signal and output closure.

The replacement Windows ZIP ships the Electron application and CPython 3.14 worker bytecode. It uses the selected installation's distributed Python and the tools of the release in `app/current.txt`. The worker records the chosen interpreter path, Python version and app path through the existing private-path logger. If Python or the tools are missing or the runtime cannot load 3.14 bytecode, the window directs the player to update or repair with the installed launcher. A first installation starts with the current NX2Saria installer. The synthetic Windows window verified both messages. With the published Python 3.14.9 runtime, a synthetic build tool from the active app verified its own path and the interpreter path. Success resumed Play. Cancelling a second job released the job marker and did not launch again. The dependency probe of the build interpreter now opens stdin as DEVNULL so it cannot inherit the worker pipe. The full worker test module under the Windows release runtime had 44 passing cases and two platform skips. These checks used a fabricated toolchain and game, so the owner's real-game test remains pending. Applying updates, sending session reports, showing the crash-log offer inside Electron and migration remain separate milestones.

The first owner Windows attempt exposed a mismatch between the worker's imports and the published
0.24.0-rc.1 tools. The release audit found that `private_paths.shorten_always()` was the only missing
export used by inspection or Play in the 0.24.0-rc.1 and 0.25.0 archives. The packaged worker uses its own
launcher modules and loads the selected release's tools. The compatibility path uses the older helper's
`Shortener` so paths included in reports remain shortened regardless of the local log setting.
The replacement ZIP passed inspection, synthetic Play and the forced-privacy checks in Windows installations
built from the published app and runtime archives of 0.24.0-rc.1, 0.25.0 and 0.26.0-rc.1. This establishes
the startup and privacy behavior with those archives. The owner later confirmed detection and two normal
consecutive Plays on Windows. The original ZIP did not record Electron window events independently.
The test build leaves report sending with the installed launcher. Only the most recent summary remains
available to that launcher after consecutive sessions in Electron. A change to when reports are sent is
pending the owner's decision.

After an abnormal end, the worker calls the selected installed release's crash-offer helper, if available,
after the game log has been finalized. The helper keeps its existing eligibility and opt-out logic. The new
window still delegates display and any sending to the installed launcher. The published 0.26.0-rc.1 helper
created a readable marker in testing, while 0.25.0 had no helper and left no marker. None of these tests made
a network request.

The Linux x86-64 test archive ships the same Electron and worker application build as the Windows test
ZIP. It uses the selected AppImage installation's Python and tools. For a read-only readiness check,
call the same `inspect_installation()` used by the worker before opening the lock. A normal app session
takes `data/launcher.lock`, records its owner in `data/launcher.lock.owner`, and writes to the launcher logs.
Synthetic windows use Xvfb and a fake game.

The owner tested Linux build `b62829a5` on a real Preview 0.26.0-rc.2 installation. The logs confirmed
automatic detection, the installed Python runtime, hide and restore, two normal game exits in the same
window and refusal of a second instance while the game was running. When the owner explicitly closed the
launcher during play, the worker followed the game through its normal end and exited. The closed launcher
stayed closed. A mod rebuild and an abnormal exit were covered by synthetic tests.

### Worker implementation: handshake and inspection

The first worker slice implements a bounded JSON-lines handshake with exact protocol version 1, validates the selected
launcher root and protocol before writing, acquires the existing lock before `Context`, reports held or unavailable
lock state and detects a complete waiting update without installing it. It supports inspection, structured protocol
errors and idle EOF cleanup. Synthetic Python tests exercise two workers, CLI and Steam contention, malformed input,
the lock-open fallback and staged-file preservation; the TypeScript protocol parser is tested against a real worker
handshake. The synthetic worker tests run on Windows with Python 3.11; they use temporary roots and a fake Steam process and do not use a real game. This slice preceded wiring the Electron window to the worker. See the current validation section for Play, jobs and the owner test status.

## Development logging and dependency inventory

Electron writes `data/logs/launcher-electron.log` for a recognized installation after the logs directory is available.
It records the build hash and process ID, selection origin and brand with the worker version, inspection and Play
results, window visibility, second instance refusal, the Play message kind confirmed by the renderer and normal exit. A game that started and concluded with
an error displays the worker's game-end sentence, which may include a FATAL detail. A spawn failure has a
separate message. Lines omit paths regardless of the Python log setting. Existing files over
1 MiB are reset on opening. The worker still writes its own events to the existing `launcher.log`. The synthetic
window tests verify both logs, including the Electron rows for a second instance sharing the same file. See `docs/electron-launcher-notices.md` for the preliminary runtime and development-dependency license inventory; it is not release clearance.

The development slice now refreshes installation state after a delegated Play ends and permits another Play without reselecting when the refreshed state is ready and unblocked. A Linux Electron test covers two consecutive starts and verifies a stand-in game is still running when the Electron window closes. It does not prove the future worker recovery contract or Windows real-game behavior.

## Portable development test with activity logging

A second private Windows x64 ZIP was built from the Electron development head `9203aeea`, whose Tests and Content guard checks passed. It includes the bounded, path-free development log and refreshed state after Play. Archive integrity and the copy checksum were verified. On a Windows host, the packaged window used its embedded Python to find a synthetic installation, execute two stand-in sessions, hide and restore, and write sanitized development logs. No real game was run in this verification. The owner still needs to test the actual game on the laptop, including two plays, hide/restore and closing the launcher during play; the package is not a release or migration artifact.
