# ThreadSanitizer on the Linux bench

The opt-in bench build instruments the host runtime with Clang's ThreadSanitizer (TSan). The generated game C is not
instrumented: a race involving an access from that code can be missed. Vulkan drivers and other prebuilt libraries are
not necessarily instrumented either. A clean report is not proof that the game has no races.

Keep this build out of releases. `NX2SARIA_BENCH_TSAN=1` requires an output folder named `g25-tsan`; the build marks it
and refuses to use it for an ordinary build. It compiles the runtime rather than linking the prebuilt runtime. The host
object keys and link key include the sanitizer flags, and old unsanitized host object keys are not adopted. The generated
game's compile commands stay unchanged. Use a separate build cache and do not copy the instrumented objects into a kit.

Use the bench's serialized branch-build script with the environment variable set and put the whole script in a memory
limited scope: its shader preparation and configuration probe can also start the executable. The bench script takes the
shared build lock. Make sure no game is running first, check free disk space and use a scratch directory for reports.
For example, after setting `BENCH` to the bench root, `CHECKOUT` to this checkout and `SCRATCH` to an existing scratch
directory:

```sh
pgrep -x ocarina   # stop here if it prints a process
systemd-run --user --scope -p MemoryMax=18G -p MemorySwapMax=0 \
  env NX2SARIA_BENCH_TSAN=1 NX2SARIA_PGO=off SDL_VIDEODRIVER=offscreen \
  SDL_AUDIO_DRIVER=dummy NX2SARIA_AUDIO=none \
  TSAN_OPTIONS="halt_on_error=0:history_size=3:second_deadlock_stack=1:log_path=$SCRATCH/tsan:suppressions=$CHECKOUT/tools/tsan.supp" \
  "$BENCH/xbuild/build-branch.sh" g25-tsan "$CHECKOUT"
```

The script builds the committed checkout, not uncommitted changes. Confirm the scope actually has `memory.max` set to
18 GiB and `memory.swap.max` to zero on the bench; do not run the game if the limit cannot be enforced. Use the same
memory limit and the shared lock for each separate run. Keep a copy of the game's ini and compare it afterward. Run
offscreen with dummy audio; do not use Xvfb. For a repeatable scene use `tools/run_game.py --boot-map` with a copied
profile and backed-up save as described in [boot-to-map.md](boot-to-map.md). Keep game logs and TSan reports in the
scratch directory, and verify that `ocarina` has exited.

Before treating an empty report as evidence, run the instrumented executable with `NX2SARIA_TSAN_PROBE=1` in a separate
short probe and verify that its TSan report names `tsan_probe_writer` and a data race. The switch is compiled only into
the sanitizer build. Leave it unset during game runs. TSan writes a file per process using `log_path`; preserve all files,
including any from re-execution. `halt_on_error=0` allows more reports, while an explicit wall-clock timeout keeps the
first run short. A TSan process may use substantially more memory than the ordinary game.

Classify each distinct report from its stacks and synchronization: a real race, a documented benign race, or a false
positive with evidence. Fix real races in separately reviewable changes. Add entries to `tools/tsan.supp` only for
investigated benign reports or false positives and explain each entry there. Hardware or driver synchronization may not
be visible to TSan; do not suppress an unexplained report merely because it involves Vulkan.
