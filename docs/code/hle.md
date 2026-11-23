# runtime/hle

The high-level emulation of the Switch's system software, Horizon: the kernel under the game's SDK (threads, waits,
virtual cores, fibers, handles and events), the SVC entry, the two real IPC services and the rest of IPC, the file
system (save data, RomFS and its archives), the audio renderer, the overrides that replace SDK functions with host code
(`ov_*.c`), the movie decoders, amiibo and nfp, the software keyboard, the offline network and the shared save. Three
probes check the area without a GPU: `--probe-kernel`, the movie probe and the override probe.

Where the area sits in the host, and how threads and cores are modelled: "3. Host" and "Threads and cores" in
[architecture.md](../architecture.md). The feature docs the notes below point to: [amiibo.md](../amiibo.md),
[audio-input.md](../audio-input.md), [heap-retry.md](../heap-retry.md), [log-privacy.md](../log-privacy.md),
[controls.md](../controls.md), [performance.md](../performance.md). The log lines of the area: [logging.md](../logging.md).
The checks that drive this code: [building.md](../building.md#checks).

In the notes below "the game" is the title the build is for, by version (1.1.0, 1.1.1, 1.2.1, 1.4.3). `main+0x...` is an
address in the game's main module, `sdk+0x...` one in its SDK module. "Horizon" is the console's operating system and
"the SDK" the game's own copy of the `nn::` libraries, which runs as lifted code. "G" is the kernel's global lock.
A file whose first line is a licence notice keeps that line in the code; the rows of `THIRD_PARTY_NOTICES.md` say what
each notice covers.

## `runtime/hle/kernel.h`

Kernel internals shared by `runtime/hle/kernel.c`, `runtime/hle/svc.c` and the kernel probe
(`runtime/hle/kernel_probe.c`). It is not part of the `runtime/host.h` contract.

- The `KERN_R_*` values are Horizon kernel results (module 1).
- `KERN_SYM_INITIALIZE_FIBER` and the other `KERN_SYM_*` names are the fiber overrides, linked as `HOST_OVERRIDE`
  entries in `kernel.c`.
- `kern_seed_tls()` puts the running thread's TLS where the SDK looks for it (TLR+0x1F0).
- `kern_backtrace()` prints the guest frame-pointer chain from `c->x[29]` to stderr.
- Probe support: `kern_thread_state()` returns `KERN_WAIT_*`, `KERN_THREAD_EXITED`, or -1 for a bad handle;
  `kern_core_stats()` gives the virtual-core holders now and the most ever at once; `kern_cores()` the virtual cores
  that run threads (6, or `NX2SARIA_CORES`). `kern_test_cv_old_order` makes WaitProcessWideKeyAtomic release the lock
  before setting the key (the old order, the probe's negative control); `kern_test_cv_hook` is called right after the
  lock release, holding the kernel lock; `svc_test_exit_hook`, when set, is what svc 0x07 calls instead of ending the
  process.

The file starts with a UTF-8 byte order mark, which the comment removal left in place.

## `runtime/hle/kernel.c`

Threads, synchronization, virtual cores and fibers of the Horizon kernel.

One guest thread is one host thread with a 256 MiB reserved host stack (every guest BL is a host call) and its own
`Cpu`. The SDK's `nn::os` runs as lifted code; only the kernel under it is the host. Every wait hangs on one global
lock G with one condition variable per thread; the wake order is the kernel's: lower priority value first, then first
come. The semantics follow the public Horizon kernel (KConditionVariable, KAddressArbiter, KSynchronization).

The file carries three licence notices, each above the part it covers: at the top, above the asynchronous preemption
and above the spin waits. They stay in the code.

### Constants

- `R_INVALID_ADDRESS` (0xCC01) is Horizon's InvalidAddress (a misaligned address).
- `NCORE` is 6: the application cores. Switch 2 titles get cores 0-5 (this title's NPDM ThreadInfo capability: core
  min 0, max 5); a Switch 1 title uses 0-2 and never names 3-5, so 6 virtual cores serve both.
- `HOST_STACK` (256 MiB) is reserved and committed on demand.
- `THREAD_OBJ_SIZE` (0x400) is the "current thread object" at TLR+0x1F8.
- `MAIN_STACK` (1 MiB) is `main.npdm` META+0x1C, `MAIN_PRIO` (44) META+0xE, `MAIN_CORE` (0) META+0xF.
- The `S_*` states are the scheduler's accounting states (`NX2SARIA_TRACE=sched`).

### `Event`

`serial` is a number no other Event of the run gets (set holding G; `g_ev_serial` is the last one given), so a kept
writable handle can be checked to still name the Event it named when it was kept: `runtime/hle/kernel.c`
`kern_bind_system_event()` and `system_event_set()`.

### `RtThread`

- `preempt` is the first member: `rt_dispatch` reads it (`kern_preempt_pending` of `runtime/host.h`). `host_depth` is
  the second member: the host calls and SVCs in progress, while `kern_host_track` (`runtime/host.h`).
- `cpu` is the thread's own context, `cur` the context running now: `cpu`, or the current fiber's.
- The current wait: `wseq` is the registration order (equal priority wakes first come first), `next` the wait list,
  `all` the list of every thread.
- Virtual core: `cur_core` is atomic: the core held while running guest code, -1 while blocked.
- `exit_jmp` is the thread start; ExitThread jumps back to it.
- Fibers: `base_fiber` is the host thread as a host fiber, after the first switch; `fiber` the current guest fiber, NULL
  on the thread's own context; `leaving` the fiber just switched away from, released by whoever resumes next;
  `exit_pending` an ExitThread on a fiber: unwind once back on the thread's own context.
- `NX2SARIA_TRACE=sched` accounting (holding G): `tid` is the host thread id; `st` the `S_*` state since `st_since`;
  `wobj` what the current wait is on (owner handle, key, address or first handle); `whs` and `wn` the handles of a
  WaitSynchronization that blocks, in the waiter's own memory (holding G); `waker` the thread whose signal ended the
  last wait (0: a host thread or a timeout); `blk_since` when the current blocking wait began, 0 while not blocked
  (holding G; `wait_watch()`); `blk_told` that `wait_watch()` logged the current wait; `wake_at` and `grant_at` when a
  wait was signalled and a core granted, until the thread runs again; `lw_ns`, `lw_n`, `lw_max`, `lg_ns`, `lg_n`,
  `lg_max` those latencies, and `sl_n`, `sl_req`, `sl_act` what SleepThread asked and took; `sp_n`, `sp_ho` and
  `sp_site` the spin wait passes and core handoffs and the last loop head (the thread writes them); `target`, as a
  core waiter, the owner `core_kick()` asked to give its core up for it (holding G).
- `NX2SARIA_ASYNC_PREEMPT` (holding G): `hthr` is the host thread, for `plat_intr_stop` and resume, open while it may
  own a core; `ap_rip` the interrupted host rip `kern_ap_stub` returns to; `ap_last` the last redirect attempt on this
  owner; `ap_pending` that it was redirected to `kern_ap_stub`, which has not reached `kern_ap_yield()` yet.
- `NX2SARIA_CORE_DIAG` (holding G, except `dg_host_end`, which only the thread itself writes): as a waiter, `dg_since`
  is the core wait's start and `dg_cls` the `DG_*` class at the start of the wait; as an owner, `dg_kick` is the
  preempt request, `dg_host_end` the return from the host call or SVC it was in at the request, `dg_pc` and `dg_lr`
  the guest pc and lr at the checkpoint that gave the core up, `dg_host_kick` that it was in a host call or SVC at the
  request. `dg_svc` is the thread's latest SVC (what an owner in a host call is in). `dg_rel` is, as a waiter, the
  owner that released.

### `core_current()` `core_current_unlocked()`

G serializes core ownership and the queue. The waiter may also poll `cur_core` outside G: every access is atomic,
`core_grant()` publishes the assignment with release and that poll uses acquire; G protects the other ownership
fields.

### `tcur_now()`

The current thread, read again on every call. Guest fibers migrate between OS threads (Switch 2 titles' job systems
resume any fiber on any worker): code that resumes after `plat_fiber_switch` may be on another OS thread, and the
compiler takes a thread-local address to be the same throughout a function, so such code calls this instead.

### `crit_add()` `crit_frame()`

`NX2SARIA_CRIT=<first>-<last frame>` with `NX2SARIA_TRACE=sched`: every state change and wake of the guest threads and
every present over those frames, written to `NX2SARIA_CRIT_OUT` (default `crit.bin`) at the last one, for
`tools/crit_path.py`, which walks the chain of waits back from each present (holding G). `NX2SARIA_CRIT=w<lo>-<hi>`
takes world-clock seconds instead (`frame_step_world_us`): from the first present at or after lo to the first after
hi, so a session covers the same game time at any frame rate.

- `CritEv` kinds are `CRIT_STATE`, `CRIT_WAKE` and `CRIT_FRAME`. `a` is the thread; `b` its waker (STATE) or the waker
  (WAKE); `st` the new `S_*` state.
- `CRIT_N` is 1 << 22 records: 128 MiB, about 2000 fight frames.
- `g_crit_lo` and `g_crit_hi` are frames, `UINT64_MAX` in world mode until the world clock fixes them; `g_crit_wlo` and
  `g_crit_whi` are, in world mode, the window in microseconds of world clock, else 0.
- `crit_frame()` runs at each present (holding G): it starts recording at `NX2SARIA_CRIT`'s first frame, and stops and
  writes the file at its last. In world mode it fixes the frames as the clock passes lo and hi. The file: "CRIT", the
  thread count and the event count (u32 each); per thread its handle, priority, flags (1 main, 2 presents) and host
  tid (u32 each); then the `CritEv` records.

### `wait_add()` `acct()`

The `NX2SARIA_TRACE=sched` accounting (holding G). `g_render` is the thread that presents (sched trace, core diag).
`g_rw` and `g_mw` are the render and the main thread's waits by object and waker. `g_pre_req`, `g_pre_n`, `g_pre_ns`,
`g_pre_max` and `g_pre_stale` count the preempt requests, the releases after one and their latency. `g_sync_calls` and
`g_sync_blocks` count the WaitSynchronization calls and the blocks inside them; `g_sync_woken` and `g_sync_spared`
such waits a signal woke, and those it left asleep. `acct()` closes the current state's interval and enters `st`.

### `h_new()` `h_peek()` `h_get()`

The handle table; the caller holds G.

### `pick()`

The waiter to wake in kernel order (the wait primitives hold G). Lock waits match by lock address, condvar waits by
key, arbiter waits by address.

### `sync_watches()` `wake_sync_waiters()`

`sync_watches()` is 1 when one of the handles `w` waits on names `obj`: an event (either a readable handle of it) or a
thread.

`wake_sync_waiters()`: `obj` became signalled, an event (its `Event`) or a thread that exited. The WaitSynchronization
waits that hold a handle of it are woken to look again. Every other such wait stays asleep: woken, it took a virtual
core (`block()`: `core_take()`, which asks a running thread of lower priority for its core when every core is held),
found nothing signalled and blocked again, and the audio renderer alone signals its event 200 times a second, each
time waking every thread that waits for a message, a timer or the end of another thread. `NX2SARIA_SYNC_WAKE_ALL=1`
wakes them all, as before ("ab": in the B blocks of `NX2SARIA_AB`); the sched line counts the waits woken and left
asleep.

### `block_inner()`

The platform's condition wait with a timeout counts milliseconds (`plat_cond_wait_ms`): the wait rounds up (late is
legal, early is not) and re-checks the deadline against the clock, because the wait may also end early. A wait without
a timeout reads no clock (G is held here).

### Virtual cores

Horizon runs at most one thread per core, and the game keys per-core data (allocators, job queues) by the current
core number without locks. A guest thread therefore holds one of the application cores while it runs guest code and
gives it up only where Horizon would switch threads (waits, sleep, yield, exit, blocking host calls); a free core goes
to the best waiting thread. A waiter better than a holder asks one holder to give its core up
(`runtime/hle/kernel.c` `core_kick()`), which it does at its next SVC or indirect call (`kern_checkpoint()`,
`rt_dispatch`), as Horizon runs the better thread at once; guest code without either keeps the core until it reaches
one.

`free_cores = 1` (`NX2SARIA_FREE_CORES`) turns the gate off, for experiments only: the game does not run that way. A
thread that holds no core reports its ideal core to GetCurrentProcessorNumber, so the game's job workers all report
core 0 and run its per-core code at once: on 1.2.1 two of them add the same per-core NVN control memory, which retires
the command handle the other just recorded, and CallCommands finds no recording at start (#212); 1.4.3 corrupts a heap
in `main+0x82e00` or hangs at the title menu. The start log and every crash report say so (`kern_free_cores_note()`).

G protects the list of core waiters (`g_core_wait`); the atomic hint `g_core_waiting` is updated under G after each
insertion or removal. Readers outside G only test whether the list might be nonempty and take G before inspecting it;
a missed waiter is seen on a later pass. `g_active_cores` is `NX2SARIA_CORES` (`core_allowed()`).

### `cores_enabled()`

`NX2SARIA_FREE_CORES` set to "0" keeps the gate, like unset.

### `core_allowed()`

`NX2SARIA_CORES` = 1..`NCORE`: how many virtual cores run threads (default all `NCORE`). A core at or above the count
is folded onto core (c mod count): a thread pinned to core 5 with `NX2SARIA_CORES=3` runs on core 2, one thread per
core at a time as always (diagnosis of races that more parallel cores expose; the Steam Deck profile's four).

### `core_seen()`

The core number a thread sees (GetCurrentProcessorNumber) while it holds virtual core c. With all `NCORE` cores that
is c. With fewer it is one of the thread's own cores that fold onto c, its ideal core first: the game keys per-core
data by this number without locks, so a thread pinned to core 5 must go on using core 5's data when it takes turns
with core 1's threads on one virtual core. Seeing 1 there, it used core 1's data in turn with core 1's own threads,
and the game died in its draw set-up within seconds of the first frame in play (issue #445; with `NX2SARIA_CORES=4` on
any machine). No two threads that run guest code see the same number: they hold different virtual cores, and the
numbers that fold onto different virtual cores differ.

### Asynchronous preemption

`NX2SARIA_ASYNC_PREEMPT`, on unless 0. Horizon gives a core to a strictly better thread the moment that thread is
ready. Here a better waiter only marks the owner, which gives the core up at its next SVC or indirect call; an owner
in a stretch of game code with neither keeps it (`NX2SARIA_CORE_DIAG`: 0.5-1.1 ms of thread-time per frame, waits to
16 ms and more). With it on, three parts, all of them necessary:

- The waiter interrupts the owner (`plat_intr_stop`) when it has waited `AP_FIRST_NS`, then every millisecond: if the
  owner's host rip is in the lifted modules' code (`g_gen_lo`..`g_gen_hi`: the first and last function start) and its
  host depth is 0 (not in a host call or SVC, nor in game code a host call made), it holds no host lock, so its rip is
  moved to `kern_ap_stub`, which saves every volatile register, gives the core up like a checkpoint
  (`runtime/hle/kernel.c` `kern_ap_yield()`) and jumps back. Otherwise it is only marked, as before.
- Priority inheritance (`runtime/hle/kernel.c` `eff_prio()`): a thread owning a guest mutex counts at the priority of
  the best thread blocked on it, for granting, kicking, preempting and yielding. Without it the game stopped at its
  first "Continue" (a better thread took the core of the low-priority owner of a mutex it then waited for).
- Horizon's preemption disable: the SDK's spin locks (1.2.1 sdk `f_70051242a8`, `f_7005124360` and siblings) count
  TLR+0x100 (disable_count) while they try or hold the lock, and after clearing it call SVC 0x36 when TLR+0x102
  (interrupt_flag) is set. Such an owner is not preempted, at a checkpoint either: interrupt_flag is set and the
  switch happens at that SVC. Preempting it anyway hung the game at "Continue" (the lock holder lost its core to a
  thread that then spun on that lock).

`NX2SARIA_ASYNC_PREEMPT=0` keeps the old scheduling exactly: raw priorities, no interruption, no disable_count; `=2`
keeps inheritance and the preemption disable without interrupting owners (`g_ap_int` is whether owners are
interrupted).

- `AP_FIRST_NS` (250000): the waiter spins this long before the first interruption (most waits end sooner).
  `AP_GAP_NS` (1000000): it interrupts the same owner at most this often.
- `g_ap_log` counts the redirects logged so far (`AP_LOG_N`, 40).
- `ap_stub` is the stub for this processor, chosen by `ap_init()`.

### `cpu_keeps_ymm()`

Whether the processor has AVX and the system saves YMM state across thread switches (OSXSAVE, XCR0 bits 1 and 2).

### `eff_prio()` `prio_of()`

`eff_prio()` is Horizon's priority inheritance: `o` counts at the priority of the best thread blocked on a guest mutex
it owns (the owner is read from the mutex word, so it follows hand-offs). Holding G. `prio_of()` is the priority the
virtual cores go by: raw with `NX2SARIA_ASYNC_PREEMPT=0`, as before.

### `ap_tlr()` `ap_disabled()`

TLR+0x100 is disable_count and TLR+0x102 interrupt_flag. Any thread may read them; the flag is set from outside as the
kernel does.

### The stub

`kern_ap_stub` is entered in place of the interrupted instruction of top-level game code. It keeps every register the
game code may have live (volatile GPRs, flags, YMM0-15, MXCSR; the C calls preserve the rest), gives the core up in
`runtime/hle/kernel.c` `kern_ap_yield()` and jumps back to `kern_ap_rip()`, whose value it first stores in the return
slot it reserved at entry, so a stack walk sees the interrupted function as its caller. No `ret`: the jump keeps
call/return pairs balanced. The waiter never writes the owner's stack.

There are two bodies of the stub (`runtime/hle/kernel_ap_stub.h`). `kern_ap_stub_ymm` saves YMM0-15 and is the one
used wherever the processor and the system keep YMM state (`ap_stub`, chosen by `ap_init()`). The lifted code of such
a PC is compiled with AVX2 and keeps data in the upper halves, which every function the stub calls is free to clear,
whatever the runtime itself was compiled for. `kern_ap_stub_xmm` saves XMM0-15 with SSE moves: code on a processor
without AVX has nothing in the upper halves, and a VEX move there is an illegal instruction.

### `ap_try()`

The waiter `w` interrupts its target owner if that runs top-level game code. Holding G; the owner is stopped only
between `plat_intr_stop` and `plat_intr_resume`, during which nothing here takes a lock. When the owner's preemption
is disabled (disable_count nonzero), the interrupt flag is set instead: SVC 0x36 switches.

### The core diagnosis

`NX2SARIA_CORE_DIAG=1`: why ready threads wait for a virtual core, and for how long. Each core wait is split into
classes: the owner asked to give the core up ran game code (`DG_GAME`, the part an asynchronous preemption would
remove), was in a host call or SVC that kept the core (`DG_HOST`), every owner the waiter may displace was of better or
equal priority (`DG_EQ`: Horizon waits too), or the worse owners were claimed by better waiters (`DG_NOCORE`). Per 60
presents: time per frame and a wait-length histogram per class, the owners that cost most; a line per present over
25 ms; a line per wait with over 2 ms of `DG_GAME` (where the owner reached its checkpoint), and one host RIP sample of
the owner per such wait (exe RVA, for `nx2navi.map`).

- `DG_NB` is 8 wait lengths: under 0.05, 0.25, 1, 2, 4, 8 and 16 ms, and longer.
- The window struct keeps `ns`, `n` and `hist` for this window and `own`, this window's owners by class. `g_dgf` and
  `g_dgfr` are this frame's times for all threads and for the render thread.
- `g_dg_rel` is the owner `core_put()` is releasing, for `core_grant()`.

### `kern_host_left()`

A tracked host call or SVC returned to depth 0 with a preempt request pending (`runtime/host.h`); on the thread
itself.

### `dg_class0()` `dg_done()` `dg_sample()`

- `dg_class0()`: the waiter's class at the start of its wait, after `core_kick()` claimed (or did not claim) an owner
  for it.
- `dg_done()`: the waiter got a core: it splits [`dg_since`, now] by what the owner that released it did after its
  preempt request.
- `dg_sample()`: once per long `DG_GAME` or `DG_HOST` wait: where the owner's host thread runs (module and offset),
  its latest SVC and its guest lr. Holding G; the owner is resumed before anything that could take a lock it holds.

### `kern_core_diag_frame()`

Called by `perf_present` at every present: a line for a present over 25 ms, and the 60-present summary. Every 600
frames it also logs the `NX2SARIA_ASYNC_PREEMPT` counters since the start.

### `core_put()`

For the sched trace it measures the time from the preempt request to this release. Waiters that were to interrupt `t`
(`core_kick()`) forget it: `t` owns no core now, and once it exits and its handle closes (`thr_free()`), a waiter's
next `ap_try()` would read and interrupt freed memory.

### `core_better_waiter()`

Whether a waiter better than `t` may run on core `c`.

### `core_kick()`

Asks holders to give their cores up, one holder per waiter: waiters best first, each claims the worst holder worse
than itself on a core it may run on that no better waiter claimed (a request already pending is reused). Asking every
worse holder made several drop their cores for one waiter; the spare cores went to worse waiters, which asked again.

### `core_take()`

Most waits end within microseconds, so a waiter that may interrupt spins `AP_FIRST_NS` before interrupting. Then it
waits on its condition variable until signalled, or, when `ap_try()` left the owner with its core, for one
millisecond before trying again.

### Long waits

A freeze's first clue. A guest thread that waits `NX2SARIA_WAIT_WATCH_S` seconds for a lock, or for another thread to
end, is logged once per wait with the thread it waits for and what that one is doing: a game that stops with nothing
in its log then names the chain. Waits on condition variables, events and addresses are left out: idle worker threads
wait on them for good. Off by default (0), as its thread takes the kernel lock every 2 s; 10 is a good value for a hang
report. At most `WAIT_WATCH_LINES` (24) lines are logged.

### `wait_pass()` `wait_lines_log()`

One walk over the guest threads, G held: each one that has waited `limit` ns or longer for a lock or for another
thread's end gets a line, at most `max` of them. `mark`: a wait gets its line once (`wait_watch()`); the hang watch's
report (`kern_wait_report()`) lists every such wait as it stands. The lines go into `out`, each ending in a newline,
and the caller logs them once G is released (`wait_lines_log()`): a log call can wait for room in the log queue
(`runtime/logq.c`), and a log thread stuck in a write, one of the stalls the hang watch reports, would then hold G and
every guest thread with it. It returns the lines written.

`WAIT_LINE_CAP` is 384: a line is its fixed words, two numbers and the 160 bytes of `wait_watch_holder()`. For a lock
wait the lock word's owner field is the holder's handle. A line that finds no room is left for a later pass.

### `kern_wait_report()`

The hang watch's report (`runtime/diag/hang_watch.c`): the guest threads' lock and thread-end waits as they stand. G
is only tried, ten times 10 ms apart: its holder may be the thread that hangs, and the watch adds no wait of its own.

### `kern_core_away()`

With the sched trace on, the wait's object is the caller as an exe RVA (for `nx2navi.map`).

### `rt_spin_gate()`

Spin waits: the `spin` lines of `recomp/patches.py`, found by `tools/spin_waits.py`. A pass of a guest loop that polls
memory until another thread changes it. On the Switch it fills only its own core, where a better thread pinned there
preempts it; here it holds a virtual core and a host core, and pollers that take a lock on every pass hand its cache
line between their host cores so the thread whose item it is rarely gets it (a job queue's pollers do this). Every
pass pauses the host core once. A wait is a run of passes through the same loop at most `SPIN_GAP_NS` (20000) apart;
once one has lasted `NX2SARIA_SPIN_HANDOFF_US` (default 50) and a thread waits for a virtual core, each pass gives the
core to the waiter, whatever its priority, yields the host CPU and takes a core again. Shorter waits keep the core: a
lock held for a moment would otherwise make the next pass wait for another thread's next SVC. `NX2SARIA_SPIN=0` keeps
the core always (the gate's own cost only).

An empty value of `NX2SARIA_SPIN` is unset: `cfg_bool` of a key without a settings row would stop the game on it. A
different loop head, or a pass more than `SPIN_GAP_NS` after the last, starts a new wait. A core waiter missed at the
unlocked test of `g_core_waiting` is seen at the next pass.

### `kern_checkpoint()`

A pending preempt request, at an SVC or an indirect call. The core goes over only if a better thread still waits for
it, so `core_put()` grants it to a waiter better than `t`; a stale request (that waiter got another core) is dropped.
With preemption disabled the interrupt flag is set and the switch comes at SVC 0x36. `core_put()` clears `preempt`,
after the sched trace reads it.

### `kern_ap_rip()` `kern_ap_yield()`

Called by `kern_ap_stub`, on the interrupted owner: where to jump back to, then the core given up as at a checkpoint.

### `ap_sync()`

SVC 0x36 SynchronizePreemptionState: a preemption deferred by disable_count happens here.

### `kern_sched_report()`

`NX2SARIA_TRACE=sched`: every 120 presents, the time split of the busiest threads (running, waiting for a core, host
calls) and of the render thread, the render and the main thread's longest waits by object and by the thread whose
signal ended them, and the preemption latency. Each thread's open interval is closed first; the render thread is
listed first. Resetting a thread's spin counters is racy against the thread's own increments: a few passes may be
lost.

### `lock_handoff()` `lock_acquire_for()`

The guest locks: the tag word is the owner handle | `WAITERS_BIT`. `lock_handoff()` releases the lock at `addr`: it
hands it to the best waiter, or clears it. `lock_acquire_for()`: a condvar waiter takes its lock, directly if free
(and wakes), else it becomes a lock waiter behind the others.

### `thr_new()` `thr_link()` `thr_free()`

- `thr_new()`: the TLR (0x200) and the "current thread object" the SDK reads through TLR+0x1F8 are one guest-visible
  block. The SDK reads the TLS pointer at TLR+0x1F0 and makes it TPIDR_EL0.
- `thr_link()` (holding G): Horizon publishes the running thread's handle at TLR+0x110;
  `nn::os::SdkMutexType::IsLockedByCurrentThread` compares it with a mutex's owner word, so without it every unlocked
  mutex reads as held by this thread.
- `thr_free()` (holding G): `t` has exited and its handle is closed.

### `thread_finish()`

The end of every guest thread, on its own host context after the unwind: give the core up, signal the thread object,
free it if its handle is already closed. `t` is not touched after the unlock. Its host thread handle is freed first:
no waiter interrupts a thread without a core.

### `exit_now()`

ExitThread: back to the thread start with `plat_jmp_go`, which unwinds nothing: no SEH unwinding through lifted
frames. (`__builtin_setjmp` is not usable: on Win64 LLVM saves the frame address, which is not rbp when the frame
register is offset, and the landing code then reads its locals through the wrong base.)

### `ap_open_self()`

The calling host thread, for `ap_try()` (`NX2SARIA_ASYNC_PREEMPT`) and `dg_sample()` (`NX2SARIA_CORE_DIAG`).

### `sleep_ns()`

The OS timer ends a wait late by up to `PLAT_WAIT_LATE_NS` (Windows' high-resolution waitable timer: 0.3-0.5 ms; the
SDK's 20 us polling sleeps, main thread and job workers, about 1.5 per frame each, took 460-515 us). Sleeps up to
twice that (`SLEEP_SPIN_NS`) spin on the clock instead (1 ms on Windows, 120 us on Linux); the virtual core is already
given up.

### `sig_obj()`

1 when the object is signaled (holding G).

### `handle_close()`

CloseHandle: an event goes with its last end; a thread object once it has also exited.

### `cv_hook()`

`kern_test_cv_old_order` and `kern_test_cv_hook` are the hooks of the probe's condvar negative control.

### `kern_svc_inner()`

The thread and synchronization SVCs:

| SVC | Call | Notes |
|---|---|---|
| 0x08 | CreateThread(-> w1 handle; x1 entry, x2 arg, x3 stack top, w4 priority, w5 core) | |
| 0x09 | StartThread(w0) | |
| 0x0A | ExitThread | on a guest fiber it unwinds from the thread's own context (`kern_fiber_switch()` resumes it) |
| 0x0B | SleepThread(x0 ns) | 0, -1 and -2 yield |
| 0x0C | GetThreadPriority(w1 handle -> w1) | |
| 0x0D | SetThreadPriority(w0 handle, w1 priority) | |
| 0x0E | GetThreadCoreMask(w2 handle -> w1 core, x2 mask) | |
| 0x0F | SetThreadCoreMask(w0 handle, w1 ideal core, x2 mask) | -1 process default, -2 and -3 keep the ideal core |
| 0x10 | GetCurrentProcessorNumber | the core seen on the virtual core held (`core_seen()`) |
| 0x11, 0x12, 0x17 | SignalEvent(writable), ClearEvent(either end), ResetSignal(readable) | |
| 0x16 | CloseHandle(w0) | |
| 0x18 | WaitSynchronization(-> w1 index; x1 handles, w2 count, x3 timeout) | |
| 0x19 | CancelSynchronization(w0 thread) | |
| 0x1A | ArbitrateLock(w0 owner handle, x1 address, w2 tag) | |
| 0x1B | ArbitrateUnlock(x0 address) | |
| 0x1C | WaitProcessWideKeyAtomic(x0 lock, x1 key, w2 tag, x3 timeout) | |
| 0x1D | SignalProcessWideKey(x0 key, w1 count) | count <= 0: all |
| 0x25 | GetThreadId(w1 handle -> x1) | |
| 0x34 | WaitForAddress(x0 address, w1 type, x2 value, x3 timeout) | type 0 if less, 1 decrement if less, 2 if equal, 3 if equal 64 |
| 0x35 | SignalToAddress(x0 address, w1 type, w2 value, w3 count) | type 0 wake, 1 increment if equal, 2 if equal modify by the number of waiters |
| 0x36 | SynchronizePreemptionState | |
| 0x45 | CreateEvent(-> w1 writable, w2 readable) | |

- **Yield** (SleepThread 0, -1, -2): hands the core to a waiting thread of equal or better priority. Most yields find
  nobody waiting for a core: those take neither G nor the priority scan over every blocked thread (`prio_of()`), which
  the game's yield loops paid thousands of times a second. Like `rt_spin_gate()`, a thread that queues after this check
  is found by the next yield, and kicks the owner itself in `core_take()` meanwhile. The yielding thread's own priority
  is worked out only once a waiter may run on this core.
- **SetThreadCoreMask:** out-of-range cores and mask bits are clamped (Horizon refuses them, and the SDK aborts on
  that): an ideal core of `NCORE` or more would reach GetCurrentProcessorNumber and the game's per-core arrays. A
  thread that changes its own mask while on a core the new mask forbids moves off it.
- **SynchronizePreemptionState:** the SDK's spin locks call it after clearing TLR+0x100 (disable_count) when TLR+0x102
  (interrupt_flag) is set; `rt_svc`'s checkpoint has already honoured a pending preempt.
- **WaitProcessWideKeyAtomic:** the key is set before the lock is released. The SDK's Signal reads the key in user
  mode and skips svc 0x1D when it is 0, and user mode takes the lock without G: releasing first lets another thread
  take the lock, change the condition, read key 0 and wake nobody, after which this thread waits forever. Linking into
  the wait list happens in `block()`, still holding G, so svc 0x1D cannot come between. The other branch is the probe's
  negative control: the old order (release, then set the key). On a timeout user mode takes the lock itself.
- **WaitForAddress type 3** is 19.0.0+ WaitIfEqual64: the whole 64-bit word against x2; libc++'s `std::atomic::wait`
  on Switch 2 uses it through `nn::os::WaitForAddress`.
- **SignalToAddress type 2:** waking all (count <= 0) with waiters subtracts 2, not 1 (Horizon's
  `KAddressArbiter::SignalAndModifyByWaitingCountIfEqual`).
- **WaitSynchronization** follows Horizon's order (`KSynchronizationObject::Wait`): a signalled object, then timeout
  0, then a pending CancelSynchronization. The cancel is for the next wait that would block: a poll or a wait on a
  signalled object leaves it pending. A cancel that wakes this wait (CancelSynchronization wakes it with
  `R_CANCELLED`; a signal wakes it with -1 to look again) ends it cancelled, signalled objects or not. Horizon does
  not clear an event on a wait: the SDK clears auto-clear events itself (ResetSignal), and a server clears its own
  event when there is nothing left to report (the applet message queue in `runtime/hle/ipc.c` does). An earlier
  auto-clear here hid the applet server never clearing.

### `kern_handle_hold()` `kern_handle_copy()`

Handles and events for host services (`runtime/hle/ipc.c` and the service overrides).

- `kern_handle_hold()` is `kern_handle_get()` that also runs `hold(obj)` holding G, so the caller can take a reference
  before a CloseHandle on another thread drops the handle's own: `handle_close()` clears the slot holding G and only
  then calls `ipc_handle_closed`.
- `kern_handle_copy()` copies n bytes of a handle's object holding G, for objects without a reference count
  (`KERN_TMEM`, freed by its CloseHandle): 1 when `h` is a handle of that kind, else 0 and nothing copied.

### `kern_event_readable()`

Copy-handle transfer of the readable end: each call gives the client its own handle to the same event.

### System events

System events of host services: the SDK's own `nn::os::CreateSystemEvent` creates the kernel event (SVC 0x45) on a
guest thread; the writable handle is kept, so any thread (host audio tick included) can signal it later.

The guest may destroy such an event and attach the same SystemEventType again: the game does that with the nfp
activate and deactivate events before each detection. `nn::os::DestroySystemEvent` zeroes the state byte at +0x28
(`SE_STATE`: 0 not initialised, 1 event, 2 inter-process event; `nn::os::TryWaitSystemEvent` aborts on 0) and closes
both handles, so a kept handle counts only while that byte is nonzero.

Each entry also keeps the serial number of the `Event` its handle named at bind time (`h_new()` hands a closed number
to the next new object at once, so the number alone does not say it is still that event), and whether the object lies
in guest memory: host services keep some in their own allocations (`runtime/hle/ov_movie2.c` `Video.in_ev` and
`out_ev`, `runtime/hle/net_offline.c` `HostTask.event`), which `mem_readable` does not know and the game never
destroys. A service that frees such an object gives its entry back first (`runtime/hle/kernel.c`
`kern_unbind_system_event()`, from `runtime/hle/ov_movie2.c` `ov_dtor()`).

Lock order: G before `g_se_lock` (`system_event_set()`); `kern_bind_system_event()` and `kern_unbind_system_event()`
never hold both.

### `se_find()`

The entry's writable handle, 0 when `ev` is not bound; `*serial` (when not NULL) gets the Event's serial number.

### `kern_bind_system_event()`

A bound event the guest destroyed since is created again and its table entry takes the new writable handle. A serial
of 0 matches no Event: such an entry is never signalled.

When the table is full: entries in guest memory are never removed, and a long session binds events at new addresses
(two per reload of a save). An entry in guest memory whose event the guest destroyed (its state byte is 0, or its
memory is gone) gives its slot. Entries in host memory count as live: the game never destroys them, their services
remove them before freeing them (`kern_unbind_system_event()`), and taking a live one would make the service's next
signal bind again on a host thread, which is fatal.

### `system_event_set()`

Sets (set 1, waking its waiters) or clears a bound event, holding G from reading the entry to the change. It acts only
while the entry's writable handle still names the Event the binder saw: DestroySystemEvent closes both handles under
G, and the closed number goes to the next new object at once, so a stale entry would otherwise end the process
(`kern_event_signal()`'s fatal on a closed handle) or set an unrelated event. A stale entry is skipped and logged
once.

### `kern_signal_system_event()` `kern_clear_system_event()`

`kern_signal_system_event()`: an event not bound yet is bound on the spot with manual clear, which needs a guest
thread. A bound event the guest destroyed is not signalled: its handles are closed and the kernel hands their numbers
to new objects. `kern_clear_system_event()` clears a bound event (one never bound or since destroyed is left alone).

### `kern_unbind_system_event()`

For a service about to free a SystemEventType it bound in its own memory (`runtime/hle/ov_movie2.c` `ov_dtor()`), with
nothing left to signal it: on a guest thread the SDK's DestroySystemEvent zeroes the state byte and closes both kernel
handles, which would otherwise stay open for the rest of the run; then the table entry goes, and its slot is free for
the next bind. The destroy comes first, so a signal still in flight finds the event destroyed
(`kern_signal_system_event()`) or its handle no longer naming it (`system_event_set()`) and skips it, rather than
finding no entry and binding again. Off a guest thread, or when the SDK lacks the function, only the entry goes. The
static `destroy` is 1 when the function was looked up and is not loaded.

### `kern_system_event_live()`

A SystemEventType that still holds an event: mapped guest memory with its state byte nonzero. For services that keep
their own lists of bound events and reuse the slots of destroyed ones (`runtime/gfx/gfx_present.c`
`gfx_vsync_bind()`).

### Fibers

The SDK switches fibers by swapping register context, which breaks "RET = C return". Each guest fiber gets a host
fiber (its own 256 MiB reserved host stack) and its own `Cpu`, so a suspended fiber's registers and host frames stay
untouched while others run, and a fiber may resume on another thread. A switch carries the thread state (TLR, TLS
pointer, FPCR/FPSR) over to the target's `Cpu` and clears the exclusive monitor.

### `fiber_resumed()`

After a host fiber switch: release the context left behind, and finish an ExitThread issued on a fiber. The host fiber
resumes with its own MXCSR (the fibers of `runtime/platform/platform.h` switch it), so the guest's FPCR is applied
again.

### `fiber_main()`

A new fiber starts at the top level (host depth 0); each context restores its own depth on resume. When the fiber's
function returns NULL as the next fiber, the switch goes back to the thread's own context (of the thread now running
it).

### `fiber_switch_to()`

The host depth is this context's host frames. Resumed, maybe on another OS thread, this context's frames are now that
thread's (`t` is the thread that suspended it, which runs something else meanwhile).

### `kern_fiber_initialize()` `kern_fiber_finalize()` `kern_fiber_switch()` `kern_fiber_current()`

- InitializeFiber(x0 FiberType*, x1 function, x2 argument, x3 stack, x4 stack size, w5 flags).
- FinalizeFiber(x0 FiberType*).
- SwitchToFiber(x0 FiberType*). SwitchToFiber(nullptr) returns to the thread's own context (the SDK's `sdk+0x1693c0`
  takes the thread's original fiber for a null argument); Switch 2 titles use it to leave a fiber.
- GetCurrentFiber() -> x0, NULL on the thread's own context.

### `svc_wait_note()` `kern_svc()`

`NX2SARIA_SVC_SLOW_MS` (default 0, off) logs each kernel service that blocks longer than that, with the service
number. A stalled frame shows up as time in the replay; this says whether the guest was waiting in a service, and which
one, instead of leaving it to a sampler to find.

`NX2SARIA_SVC_WAITS=1` (diagnosis, default 0): the guest main thread's time in each kernel service, per service and
the guest call chain that made it (the lr and up to three callers from the guest's frame records), logged every 10 s
as milliseconds per second with the chain named (`ldr_describe`: the SDK's symbols). It shows what the game's main
thread waits for between frames: standing in Hyrule Field on an RX 580 it spent about a fifth of its time blocked in
one service call. The key has no settings row, so `kern_svc()` calls `cfg_bool` only on a set value (an unset key is
fatal there).

### `kern_sizes()`

`NX2SARIA_TRACE=perf` "mem: sizes" (`runtime/diag/perf.c`): guest threads, the handle table, the system-event table
and fibers, each against its fixed size (the tables are fatal when full). Beside the guest's handles, the OS handles of
the process (`plat_handle_count` of `runtime/platform/platform.h`): they grew at every area load of a session while the
guest's stayed flat.

## `runtime/hle/kernel_ap_stub.h`

The body of `kern_ap_stub` (`runtime/hle/kernel.c`, [Asynchronous preemption](#asynchronous-preemption)), included
once per register width with `AP_NAME` the function's name, `AP_VMOV` and `AP_V` the move and the register prefix that
save the vector registers, and `AP_VZEROUPPER` the instruction that ends the use of the upper halves (or nothing). It
has no include guard: `kernel.c` includes it twice.

- Windows version: the entry reserves the return slot with `leaq -8(%rsp), %rsp`, not `sub`: the flags are still the
  interrupted code's until `pushfq`. The 576 bytes below hold: 0 the shadow space, 32 the MXCSR saved, 36 the MXCSR
  default, 64 YMM0-15 (XMM0-15 without AVX). The return slot is at 72(%rbp): rbp + 8 pushes + `pushfq`.
- System V version: rsi and rdi are volatile there too, and the interrupted code may keep data in the 128 bytes below
  its stack pointer (the red zone), so the return slot and the saves go below that: with the red zone out of the way
  the slot is at entry - 136 (`leaq -136(%rsp), %rsp`, again not `sub`); the exit restores rsp to the entry value and
  jumps through the slot. The 576 bytes hold the MXCSR saved at 32, the MXCSR default at 36 and
  YMM0-15 (XMM0-15 without AVX) at 64. The return slot is at 88(%rbp): rbp + 10 pushes + `pushfq`.

## `runtime/hle/kernel_probe.c`

`--probe-kernel`: the kernel's waits, wake orders, virtual cores, fibers and ExitThread on guest threads whose entries
are host functions, then real lifted code on four guest threads: main's PLT stub for `nn::os::GetSystemTick`, through
the lifted sdk function, to `rt_ticks`. Every probe thread is created with SVC 0x08 and drives the kernel through
`rt_svc`, the entry the lifted code uses.

### `wait_state()`

Polls (sleeping, so the virtual core is free) until the thread reaches a state; 5 s at most.

### `user_lock()` `user_unlock()`

The user-mode lock of the SDK: the tag word is the owner handle | the waiters bit.

### `cv_waiter()`

Waits forever; it is woken holding the lock.

### `cv_signaller()` `cv_order_case()`

`cv_signaller()` is another thread's user-mode critical section and Signal, run inside the waiter's svc 0x1C right
after the lock release: take the lock, read the key (the SDK's Signal skips svc 0x1D on 0), release.
`cv_order_case()` then does what the SDK's Signal does: svc 0x1D only when the key seen was not 0.

### `test_arbiter()`

In order: wait type 0 (if less than); wait type 1 (decrement if less than) with signal type 1 (increment if equal);
wait type 2 (if equal) with signal type 2 (modify by the number of waiters if equal). Waking all (count <= 0) with
waiters subtracts 2 (Horizon's `KAddressArbiter::SignalAndModifyByWaitingCountIfEqual`). Then a timeout and an invalid
type, and wait type 3: the whole 64-bit word against x2 (`runtime/hle/kernel.c` `kern_svc_inner()`, case 0x34); an
address off 8 bytes is refused.

### `event_waiter()` `test_events()`

`EvJob.pre`: a first wait on `h0` with timeout `t0` (result `r0`, index `idx0`) before the wait on `h`.

A pending cancel is for the next WaitSynchronization that would block (Horizon's `KSynchronizationObject::Wait`:
signalled objects first, then timeout 0, then the cancel): a poll times out and a wait on a signalled object succeeds,
both leaving it pending; the next blocking wait ends cancelled. The blocking waits have a 500 ms timeout, so a cancel
used up too early shows as a timeout instead of a hang.

### `core_worker()` `test_cores()`

Virtual cores: counts the threads running guest code at once, and that no two of them share a core number.
`PROBE_CORES` is 8, at least `NCORE` of `runtime/hle/kernel.c`. A core number past the cores is a clash too. The gate
is the kernel's own switch (`NX2SARIA_FREE_CORES`). As many threads run guest code at once as there are virtual cores,
where the host has more CPUs than that.

### `pin_worker()` `test_pinned_cores()`

Pinned threads: each sees the number of the core it is pinned to, also where `NX2SARIA_CORES` folds that core onto
another one (`runtime/hle/kernel.c` `core_seen()`), and no two that run guest code at once see the same number. Run
the probe with `NX2SARIA_CORES=4` (the Steam Deck profile's value) to check the folded case; with all cores it holds
trivially. `PIN_CORES` is 6, `NCORE` of `kernel.c`. Two threads per core, so each core's own threads take turns too.

### `fiber_fn()` `test_fibers()`

Two fibers switch to each other; each checks that its callee-saved registers, sp and tpidr survive. A fiber counts a
turn when the other fiber did not run in between, and returns 0 to go back to the thread.

### `test_exit()`

ExitThread unwinding: from three nested host calls deep, and from a fiber.

### `tick_worker()` `test_lifted_ticks()`

Lifted code: main's PLT stub for `nn::os::GetSystemTick` -> lifted sdk -> `rt_ticks`. The host functions entered by
dispatch meanwhile must be only the four thread entries, so no call reached a host override.

### `test_exit_process()`

svc 0x07 ExitProcess: the path up to the final process exit (logged, pipeline cache saved, logs flushed) runs with the
test hook in its place, which must see exit code 0; the SVC then returns to the probe.

### `test_platform()`

The platform layer (`runtime/platform/win32.c` or `runtime/platform/linux.c`): a thread handing values over a condition
variable, and the overshoot of `plat_sleep_ns` (1 ms): the median of 21 is under 0.5 ms; frame pacing depends on it.

### `probe_kernel()`

`hostfn_register` of the `KERN_SYM_*` names gives the fiber functions the loader's addresses.

## `runtime/hle/svc.c`

The SVC entry of the lifted code: preemption checkpoint, trace, then the thread and synchronization SVCs
(`runtime/hle/kernel.c`), IPC (`runtime/hle/ipc.c`), memory (`runtime/memory.c`), Break, OutputDebugString, GetInfo and
ExitProcess. The SVCs the game has but never reaches on its boot path stop with a named fatal, the caller and the guest
backtrace: the `unimplemented` table names 21, of which 0x36 SynchronizePreemptionState is taken by
`runtime/hle/kernel.c` `kern_svc()` before the table is looked at, so 20 can reach that fatal.

`PROGRAM_ID` (0x0400486024dec000) is the game's title id (GetInfo 18); `NX2SARIA_APPLICATION_ID` of
`runtime/hle/fs_host.c` is the same.

### `main_stack_query()`

QueryMemory sees the guest main thread's stack, which `runtime/memory.c` does not know: Stack inside it, the free
ranges around it clipped (the way `memory.c` clips moved stacks).

### `get_info()`

GetInfo(w1 id, w2 handle, x3 sub id -> x1). What `mem_info` does not answer:

| Id | Value | What |
|---|---|---|
| 0 | 0x3f | core mask: cores 0-5 (Switch 2 application cores; `NCORE` of `runtime/hle/kernel.c`) |
| 1 | 0x0ffffffff0000000 | priority mask |
| 8 | 0 | debugger attached |
| 10 | 0 | idle tick count |
| 11 | random | random entropy |
| 18 | `PROGRAM_ID` | |
| 20 | the user exception context | the SDK keeps for instance the dying message at +0x1C0 |
| 23 | 1 | |
| 29, 30 | 0 | a region the SDK records at `nnosInitialize` (address, then size) |
| 33 | 0x1000 | VAMM control region size: mapped at 0x300000000 and made into a unit heap |

### `rt_svc()`

With `kern_host_track`, the SVC is counted in the thread's host depth (`hostfn_tracked` of `runtime/hostfn.c`).

### `svc_body()`

| SVC | Call |
|---|---|
| 0x01 | SetHeapSize(x1 size -> x1 address) |
| 0x04 | MapMemory(x0 dst, x1 src, x2 size) |
| 0x06 | QueryMemory(x0 MemoryInfo*, x2 address -> w1 page info) |
| 0x07 | ExitProcess |
| 0x26 | Break(x0 reason, x1 info, x2 info size); bit 31 of the reason: notification only |
| 0x27 | OutputDebugString(x0 text, x1 size) |

- **Break:** the SDK keeps the abort text in the user exception context, so any readable run of it is shown. The
  object the aborting code was working on (x19) is shown too: its first bytes carry the state machine's byte.
- **ExitProcess** is handled as closing the window: the orderly end (`rt_exit_steps` of
  `runtime/gfx/gfx_present.c`), exit code 0. It happens once, shared with the window's close: a close already ending
  the process keeps this thread there, and the exit line is that close's alone; this thread coming back would resume
  guest code after its ExitProcess, so it ends the process instead. The log line is written after that guard, which
  also lets the line reach the log file past its cap (`log_sink` of `runtime/main.c`).

## `runtime/hle/ipc.h`

In-process Horizon IPC: HIPC messages with CMIF or TIPC payloads, answered in place in the caller's message buffer.
The message framing follows the HIPC, CMIF and TIPC wire layouts. Services are host implementations looked up by name
through `sm:`.

- `IPC_R_BAD_HANDLE` (0xE401) is the kernel's invalid handle; `IPC_R_UNKNOWN_COMMAND` (0xF601) is sf's unknown command
  (module 10, description 123).
- `IpcRequest`: `type` is the HIPC type (4, 5, 6, 7 CMIF; 16 and above TIPC) and `cmd` the command id; `data` the
  command arguments after the CMIF header (TIPC: after the HIPC header).
- `IpcResponse`: `xout` is the pointer data the service placed into the client's C buffers; `domain` says the reply
  carries a domain message header; `domain_objects` counts the objects the service appended to the client's domain.
- `IpcSession` is one server object. Every kernel handle to it (the first one, and CloneCurrentObject copies) holds a
  reference, and so does a request on it while it runs; the last reference calls `close(state)`. A domain's table holds
  one on each of its objects, and each object one on its domain (`held_root`), until the client closes the object's id
  (CloseVirtualHandle) or its last handle to the domain itself. The domain lock of `runtime/hle/ipc.c` guards
  `objects`, `nobjects`, `subs` and `dom_closed`.
  - `is_domain`: ConvertCurrentObjectToDomain ran, and requests carry a domain header.
  - `root`: the domain session every object id belongs to (objects share it).
  - `objects[32]`: object 1 is this session, then what the service handed out; NULL is a closed one. `nobjects`: the
    slots used so far (closed ones are given out again).
  - `event`: the readable event handed to the client (applet state changes).
  - `held_root`: the domain this object holds a reference on (`ipc_domain_add()`).
  - `subs`: as a domain, the references its objects hold on it. `dom_closed`: as a domain, its client closed it and its
    table let the objects go.
- `ipc_domain_add()` registers an object inside a domain: the table takes the object's first reference (the caller's),
  the object one on the domain. It returns the 1-based id, 0 when the table is full or the domain closed.
- `ipc_session_new()` returns a handle. `ipc_guest()` gives a client buffer as a host pointer (identity map), range
  checked.
- `IpcTransferMemory` is transfer memory from svcCreateTransferMemory (0x15), a `KERN_TMEM` handle object.
- `ipc_sessions_live()` and `ipc_sessions_released()`: the server objects alive now and closed so far (the probe
  checks that CloseHandle releases them).
- `ipc_backtrace()` prints the guest frame chain of the request being served, for fatal reports from services.
- `ipc_applet_event_init()` is called once at boot, before the guest starts. `ipc_applet_push_message()` queues an
  AppletMessage and signals the applet event.

## `runtime/hle/ipc.c`

In-process Horizon IPC: SVCs 0x15, 0x1F, 0x21 and 0x22, the `sm:` service manager and the service table. The SDK
client code runs unchanged: requests are read from the calling thread's message buffer (the TLR, or the user buffer of
0x22) and answered in place. Unknown ports, services and commands stop with their name and id instead of returning a
fake success. The file also holds `--probe-ipc`. Like `kernel.h`, it starts with a UTF-8 byte order mark, in front of
its licence line.

`CMIF_IN_MAGIC` (0x49434653) is "SFCI" and `CMIF_OUT_MAGIC` (0x4f434653) "SFCO". `t_req` is the guest context of the
request being served.

### The applet lifecycle

The application's lifecycle, as the system's applet manager runs it: one state-changed event per applet, signalled
while a message is waiting and cleared by the server once the queue drains (Horizon does not clear it on a wait). An
*application* learns it has focus through FocusStateChanged (15) followed by GetCurrentFocusState == InFocus;
ChangeIntoForeground (1) is the library-applet message and an application ignores it, which is what parked
`nn::oe::InitializeApplet` on the event. `AM_R_NO_MESSAGES` (0x680) is the "no messages" result this SDK checks for.

### `applet_proxy_handler()`

A stub object the applet proxy commands are answered on: the interfaces exist, their calls succeed. The getters each
hand out one of those objects, which is what the SDK stores and dereferences. Object ids belong to the client's whole
domain, so the object is added to the root.

### `applet_stub_handler()`

The interfaces themselves. Only the calls the game makes while booting are modelled: the common state getter hands
out the applet's state-changed event, and everything else reports success. On the common state getter:

| Command | Call |
|---|---|
| 0 | GetEventHandle: the applet state-changed event |
| 1 | ReceiveMessage: the next queued applet message, else "no messages" |
| 5 | GetOperationMode (matches the oe override) |
| 6 | GetPerformanceMode: boost when docked |
| 9 | GetCurrentFocusState |

On the graphics scale controller, command 10 is GetCurrentGraphicsScale: it answers {10, 0, 0}; see
`runtime/hle/ov_system.c` `ov_oe_scaling_parameters()`.

### `ipc_domain_add()`

`g_domains` guards the domain tables, their counts of their objects' references on them and their closed flags
(`IpcSession`). It is taken only for sessions in a domain, and never held across a handler or a close callback. The
object gets the first free slot: a closed object's id is given out again, so a client that opens and closes objects in
a loop never runs out of the 32. The object counts as a live server object from there; its last reference counts it
out.

### `applet_oe_handler()`

`appletOE`: the application-environment service. `nn::oe::Initialize` opens it; the calls that matter for boot answer
through symbol overrides (focus, performance, language), so a permissive handler that reports success and writes
nothing is enough until a command needs real data; then it is added here. Commands 0 and 10 are
OpenApplicationProxy: they hand the client a domain object, whose id inside the domain is written into the reply
data.

### `aud_user_handler()`

`aud:u`: the Switch 2 audio entry point (`nn::audio::CreateUserClientSessionByHipc`). It hands out the Switch 1
managers as sub-objects (command ids inferred from the calls the SDK makes): 2 -> IAudioRendererManager (the
`audren:u` manager), 1 -> IAudioOutManager (not served by this host).

### `csrng_handler()`

`csrng`: the SDK's cryptographic random source. GenerateRandomBytes (0) is its one command; the host's generator
fills the output buffer. The SDK sends it as a B buffer (a contributor's trace: one B buffer, no others); a C buffer
is filled too, with its reply entry, for a caller that lets the buffer type be chosen. No start or session logged on
our own PCs opens the service, but the game imports the call, and without the service a request for it ended the game
("sm: service csrng is not implemented").

The `services` table lists what `sm:` hands out (`audren:u`, `nfp:user`, `appletOE`, `aud:u`, `auddmg:u`, `audsmx:u`,
`csrng`). Anything else is a named fatal.

### `session_hold()` `session_release()`

`session_hold()` is the callback of `kern_handle_hold`: a request's reference, dropped through `ipc_handle_closed()`
once the request is answered.

`session_release()` drops one reference on a session; `sub`: the one an object of a domain held on that domain. A
session in no domain takes no lock here. A domain whose remaining references are all its objects' has lost its
client: its table lets the objects go, and each object that nothing else holds is closed and lets go of the domain in
turn.

### `respond()`

A CMIF domain reply has a 16-byte header with the objects appended, then SFCO. CMIF keeps alignment slack in the word
count (16 more bytes than TIPC).

### `control()`

The CMIF control requests:

| Command | Call |
|---|---|
| 0 | ConvertCurrentObjectToDomain: the session answers domain-tagged requests from now on |
| 1 | CopyFromCurrentDomain(object id): a handle to that object of the domain (a closed id is a bad handle) |
| 2, 4 | CloneCurrentObject and CloneCurrentObjectEx |
| 3 | QueryPointerBufferSize (`POINTER_BUFFER_SIZE`, 0x8000) |

### `dispatch()`

`held` is a domain object the request holds a reference on. Object ids belong to the client's domain, not to this
object. A domain request has a 16-byte domain header that names the object (1-based), then the usual SFCI payload.
Domain command 2 is CloseVirtualHandle: the object leaves the domain (never the domain's own); there is no answer.
This SDK addresses the domain's own object as 0, which is taken as 1. An id the client closed answers
`IPC_R_BAD_HANDLE`.

### `sm_handler()`

`sm:`, the service manager, in CMIF, and in TIPC as the SDK's sm client speaks it since 12.0.0. Command 0 is
RegisterClient / Initialize (pid), command 1 GetServiceHandle(name).

### `ipc_svc()`

| SVC | Call |
|---|---|
| 0x15 | CreateTransferMemory(x1 address, x2 size, w3 permission -> w1 handle) |
| 0x1F | ConnectToNamedPort(x1 name -> w1 handle) |
| 0x21, 0x22 | SendSyncRequest(w0 handle) and SendSyncRequestWithUserBuffer(x0 buffer, x1 size, w2 handle) |

The request holds a reference on the session until it is answered: a CloseHandle of the session on another thread
meanwhile leaves the object, and its close callback (`renderer_close` frees the audio renderer the handler uses), to
whichever drops the last reference.

### The probe

`--probe-ipc`: hand-built CMIF and TIPC requests in the guest main thread's TLR go through `rt_svc`, the entry the
lifted SVC wrappers use: `sm:` -> `audren:u` -> GetWorkBufferSize -> an audio renderer with the null mixer, whose
frame event the probe times while it sends RequestUpdate every frame (`PROBE_FRAMES`, 200). A child process checks the
unknown-service fatal.

- `msg_build()`: with a process id the kernel fills it in, so two words are skipped. In a domain message the 16-byte
  alignment pad is reserved in the data size.
- `call()`: one request on the TLR message buffer; it returns the kernel result of SendSyncRequest, and the service
  result is in `r->result`.
- `g_param` is a renderer like the game's: 48 kHz, 240-sample (5 ms) frames, final mix plus submixes, two sinks,
  REV11.
- `update_build()`: the client leaves the id of an unused slot zero. Voice 0 is one PCM16 voice with a 100 ms buffer,
  consumed after 20 frames.
- `probe_renderer()`: GetWorkBufferSize(AudioRendererParameterInternal) -> u64; OpenAudioRenderer(param, pad, work
  size, aruid; pid; copy transfer memory, current process); command 7 is QuerySystemEvent. Then every frame, as the
  game's audio thread does: wait for the event, clear it, RequestUpdate.
- `probe_main()`: `csrng` GenerateRandomBytes into a B buffer, as the SDK sends it; two requests give different
  bytes.
- `probe_unknown_service()` re-runs this executable with the same arguments and `PROBE_CHILD_ENV` set; it must stop
  with the named fatal.

### `sdk_prepare()` `probe_sdk_client()`

The game's own `nn::audio` client: main's PLT stubs -> lifted sdk -> SVCs -> this file.

Without `nn::init::Start` the SDK's own initialisation has not run; the probe sets up the two things the audio client
path reads (found by running it):

- the "current thread object" at TLR+0x1F8 is the kernel's zeroed placeholder. SdkMutex compares its lock tag with
  ThreadType+0x1B0 (`sdk+0x13186c`), which `nnosInitialize` sets to the thread's handle; a zero there makes every free
  mutex look owned by the caller (`__cxa_guard_acquire` in `nn::sm::Initialize` aborts);
- sdk's full DT_INIT needs `nn::os` initialised (its TLS slot constructor reaches ThreadManager), which is the boot
  sequence's job. The audio client only needs its two sf heap constructors: 16 KiB ExpHeaps on static buffers, stored
  at `sdk+0xd37e90` and `sdk+0xd3bec0` (CreateAudioRendererManagerByHipc reads the second). The 1.4.3 SDK creates that
  heap lazily; its OpenAudioRenderer takes `nn::audio::detail::g_RendererResourceSemaphore` first, which the
  constructor at `sdk+0x221fb0` initializes (two renderers).

`probe_sdk_client()` turns the IPC trace on to log the requests the SDK sends during set-up; the first update is
logged, the rest is quiet. An AudioRendererHandle is 16 bytes, passed in x0 and x1. At the end the 1.2.1 SDK closes
the renderer, device and manager sessions and keeps sm; the 1.4.3 SDK keeps the `audren:u` manager it created lazily,
beside sm.

### `ipc_applet_event_init()`

Creates the applet state-changed event at boot and queues the launch message: the application starts in focus (the
applet manager: SetFocusState(InFocus) -> FocusStateChanged for an application).

## `runtime/hle/fs.h`

The `nn::fs` host layer: the read-only RomFS (`runtime/hle/romfs.c`) and writable save, temporary and cache storage
under the profile directory (`runtime/hle/fs_host.c`). Both register their `nn::fs` overrides with `HOST_OVERRIDE`;
this header is what the two files and the probe share.

- Results: `FS_R_UNSUPPORTED_OPEN` (0x6402) is the RomFS's answer to any open mode but read; `FS_R_INVALID` (0xe402)
  its answer to a bad handle, offset or argument. `FS_OPEN_*` is `nn::fs::OpenMode`, `FS_ENTRY_*`
  `nn::fs::DirectoryEntryType`.
- Handles: RomFS handles are 0x1_0000_0000 + j (`ROMFS_HANDLE_BASE`); host storage uses 0x2_0000_0000 + j for files
  and 0x3_0000_0000 + j for directories.
- `romfs_owns()`: the path is on the mounted RomFS ("<mount>:..."). `romfs_handle()`: `h` is a RomFS file handle.
  `romfs_mount()`: `nn::fs::MountRom`.
- `romfs_load()`: a whole file by its RomFS-relative path, malloc'd (or NULL).
- `romfs_list()`: the children of RomFS directory `path`: `fn(name, is_dir, size, ctx)` once per file and once per
  subdirectory, in index order; `fn` returns 0 to stop. `FS_R_PATH_NOT_FOUND` when `path` is not a directory.
- `romfs_file_count()`: the files in the index (`fs_init()` first). `romfs_entry()`: the path, image offset and size
  of index row i. `romfs_image_path()`: the `romfs.bin` path `fs_init()` opened. `romfs_patches_loaded()`: a
  romfs-patches manifest gave at least one file. `romfs_overlay()`: the RomFS path of overlay k.
- The button prompts (`input_prompts_folder`): `romfs_prompts()` is the folder loaded at start, NULL for the game's
  own prompts; `romfs_prompts_ready()` whether a folder had a manifest at start; `romfs_prompts_off()` why the folder
  the settings asked for at start was not loaded (`ROMFS_PROMPTS_OK` for any other folder: it was not asked for).
  romfs-patches-pc goes by that name also where romfs-patches-pc-<w>x<h> stands in for it (`runtime/hle/romfs.c`
  `overlay_init()`).
- `romfs_action_prompt()`: the pad layout (`PAD_LAYOUT_*` of `runtime/input/input_map.h`) the action prompt's diamond
  of face buttons has from start (`runtime/patches/action_prompt.c`); -1 when the layout by label was asked for but the
  layout file could not be changed.
- `romfs_read_patch()`: bytes changed in what the game reads of an original RomFS file (`runtime/patches/boot_map.c`):
  from `offset` on, the `n` bytes `orig` become `repl` in every read of `rel` that covers them, while the read patches
  are on. Before the guest starts. 0 when `rel` is not in the RomFS, an overlay or a mod serves it, `n` is over 16, the
  file is shorter, or its bytes there are not `orig` (`why` says which). `romfs_read_patches_off()` ends all of them;
  `romfs_load()` always gives the original.
- `fs_trace()`: `NX2SARIA_TRACE=fs`, one line per open.
- `fs_host_set_profile()`: the profile directory; before the first storage mount only (default
  `rt_config()->profile`).
- `fs_host_lock_profile()`: the one-game-per-profile lock; fatal when another copy holds it (`rt_boot`, before the
  prewarm; `storage_init()` of `fs_host.c`).
- `fs_save_dir()`: `<profile>/save/<account>`, the committed save's folder, where a tracker reads the slots; "" until
  storage is made.
- `fs_save_busy()`: a save is being written: a save file open for writing, or save data changed in the last 3 s and
  not committed.
- `fs_save_flush()`: waits for a save Commit still running in the background; every exit path calls it before the
  process ends. `fs_save_wait()`: waits for a save Commit under way, refusing none later (the orderly end's first
  step, `rt_exit_steps` of `runtime/gfx/gfx_present.c`).
- A shared save (`runtime/hle/save_share.c`). `fs_save_export()`: `visit(ctx, path below the save with '/', bytes,
  size)` for every file of the save the game would load next, under the commit lock; `visit` returns 0 to fail. It
  returns the number of files (0: no save yet), -1 when one could not be read or `visit` failed.
- `fs_save_load()`: the bytes of `rel` (a path below the save with '/', "slot_02/progress.sav") in the save the game
  would load next, malloc'd; NULL when the file is absent, empty or unreadable. Storage starts first (a waiting import
  becomes the save).
- `fs_save_import_stage()`: the menu's import: `count` files (relative '/' paths, bytes) become
  `<profile>/save-import`, which the next start makes the save, backing up the save it replaces; one waiting already
  is replaced. 0 on success. `fs_save_import_waiting()`: 1 while an import waits for the next start (0 before the
  first storage use). `fs_save_import_cancel()` drops it; 0 on success. `fs_save_import_done()`: the backup of the
  save an import replaced at this start, "-" when there was none, "" when no import ran.
- `fs_save_snapshot()`: a copy of the newest committed save (every slot) in `<profile>/<folder>/<UTC time>/`,
  flushed, taken while no Commit runs; the newest `keep` copies stay. `prefix` (NULL: none) goes before the time, and a
  copy whose name it begins is never pruned, so a first copy outlasts the ones made later. 1 and the copy's path in
  `out` (empty when nothing is saved yet: nothing to copy), 0 when it could not be made (logged). Slow (the whole
  save): not on the game's thread.

## `runtime/hle/romfs_path.h`

RomFS path validation shared by the runtime (`runtime/hle/romfs.c`) and its probe (`runtime/hle/fs_host.c`
`probe_fs()`). Its first line is a licence notice.

### `romfs_path_valid_relative()`

A RomFS-relative path: non-empty, no leading '/', no '\', no empty, "." or ".." component.

### `romfs_path_collapse()`

Copies `path` into `out` (`cap` bytes) with each run of '/' after the first ':' folded into one, as a file system
reads "a//b". Switch 2 voice requests join "Voice/Resource/" and "/Stream/..." (Ocarina's
"content://Voice/Resource//Stream/USen_...bwav"). 0 when it does not fit. "<mount>:/" and one more '/' are kept.

### `romfs_path_relative()`

"<mount>:/<path>" -> "<path>", or NULL. Audio stream requests join a mount root and a leading-slash resource,
producing "content://Sound/...": repeated root separators are skipped; traversal, backslashes and empty interior
components are still rejected.

## `runtime/hle/fs_host.c`

The `nn::fs` host implementation of the functions main imports. RomFS paths and handles go to the read-only backend
(`runtime/hle/romfs.c`); save, temporary and cache storage are host directories under the profile directory
(`rt_config()->profile`, default `build/profile`):

| Folder | What |
|---|---|
| `<profile>/save/<uid hi><uid lo>/` | committed application save data, created on first use as the system launcher does for titles that declare save data |
| `<profile>/save-tx/` | the save mount's working copy and Commit's staging ([Save transactions](#save-transactions)) |
| `<profile>/save-backup/<time>/` | the three committed saves before the current one |
| `<profile>/save-import/` | a save the menu imported, made the save at the next start ([A shared save](#a-shared-save)) |
| `<profile>/temp/` | emptied at start |
| `<profile>/cache/<index>/` | cache storage |

The save mount is journaled as on Horizon: changes go to a working copy and become the committed save atomically at
Commit; a crash or an Unmount before Commit leaves the previous committed save. Commit returns at once and the copy
runs on a background thread; `fs_save_flush()` lets the exit paths wait for it. Paths are UTF-8 (`plat_fs_*` of
`runtime/platform/platform.h`).

- `NX2SARIA_APPLICATION_ID` (0x0400486024dec000) is the game's title id, as `PROGRAM_ID` of `runtime/hle/svc.c` tells
  it.
- Mount kinds: `M_SAVE` is the working copy, `M_SAVE_RO` the committed save.
- `File`: `h` NULL is a free slot; `save`: on a save-data mount; `users`: the calls using `h` now (`file_of()` to
  `file_release()`); `closing`: CloseFile came while `users` > 0, and the last of them closes `h`. `users` and
  `closing` are read without `lock` (`file_release()`). `boot_map`: the boot map changes what is read of this file
  (`boot_map_save_open` of `runtime/patches/boot_map.c`).
- `Dir`: the directory's entries at open; `used` 0 is a free slot.

### `list_dir()`

Directory listings: every entry but "." and "..", taken whole before the caller changes the directory. `join()` gives
dir/name in a malloc'd string.

### `save_touched()` `fs_save_busy()`

Save in progress: a save-data file open for writing, or save data changed in the last 3 s and not committed yet (the
game writes a slot's files back to back and then commits; between the files nothing is open for a moment). Every
change to save data comes to `save_touched()` first, which waits for a Commit under way: none lands in a commit's
snapshot.

### `make_dir()` `make_dirs()`

A junction or symbolic link to a folder counts as the folder: a player who moved the profile to another drive links
it back, and the start stopped at "cannot create directory" for it. `make_dirs()` makes `p` and every missing parent.

### `remove_tree()` `remove_tree_retry()`

`remove_failed` holds the first path `remove_tree()` could not delete and the OS error code, for the message of a
fatal. `remove_tree()` removes a directory tree; a link (symbolic link, junction) is removed as a link, never followed:
a link to a file goes by unlink, a link to a directory by rmdir. 0 also when nothing is there.

`remove_tree_retry()` is `remove_tree()`, tried again every 50 ms for up to 2 s (`MOVE_TRIES`, 40, also `move_dir()`'s)
while another process (the previous run still exiting, a virus scanner, a sync client) holds a file in the tree. The
failing path and its OS error stay in `remove_failed`.

### Save transactions

The save mount's root is a working copy, made from the newest save when the first save mount appears. Commit copies
it to a staging directory, which a rename makes a finished commit, and swaps that in for the committed save:

| Folder | What |
|---|---|
| `<profile>/save/<uid>/` | the committed save: what the next start loads when no finished commit waits |
| `<profile>/save-tx/work/` | the working copy, the save mount's root; deleted at the last Unmount and at start |
| `<profile>/save-tx/new/` | Commit's staging copy of the working copy |
| `<profile>/save-tx/ready-<n>/` | a finished commit not swapped in yet: the newest save |
| `<profile>/save-tx/old/` | the committed save a Commit replaced, on its way to save-backup |

Commit: (1) copy work to new and flush every file (`plat_file_fsync`; FlushFileBuffers on Windows) and directory, a
file equal to the newest save's as a hard link to it; (2) rename new to ready-<n>, the commit point, and delete the
ready-<n> it supersedes; (3) rename save/<uid> to old; (4) rename ready-<n> to save/<uid>; (5) move old to
save-backup/<UTC time> and keep the newest three (`SAVE_BACKUPS`). Every rename is durable (`plat_fs_rename`).

Windows refuses (3) while any file under save/<uid> is open, in this or another process (a virus scanner, Explorer's
preview, a sync tool), and a folder an elevated run made can refuse it too. ready-<n> then stays the newest save:
later working copies are made from it, and the next Commit and the next start try (3) to (5) again.

The game's Commit returns 0 once the steps are queued; a thread runs them (95-135 ms on the game thread before, in
the 1.4.3 fight save) while every change to the working copy waits in `save_touched()`, and Unmount and the next Commit
wait too. A crash in that time loses this Commit only, and a failed one is logged instead of returned.

The recovery rule at start (`save_recover()`):

- new present: an unfinished copy, deleted (rollback). An older build renamed save/<uid> away before its finished
  copy: with save/<uid> and ready-<n> absent, new is that copy and becomes save/<uid>;
- ready-<n> present: the highest n is the newest save and (3) to (5) run (roll forward); the others are deleted;
- save/<uid> absent: with no ready-<n> either, old goes back to save/<uid>.

Then old, if present, goes to save-backup, and work (changes no Commit took) is deleted.

State: `ready_root` is the newest finished commit not swapped in, empty when none; `ready_seq` the highest ready-<n>
this run made or found; `tx_lock` is taken before `lock`, never inside it; `save_mounts` counts the `M_SAVE` mounts:
the working copy exists while it is above 0. Probe hook: Commit stops at its `tx_crash_at`-th step (from 1) and leaves
the disk as a process killed there would (`crashed()`).

### `move_dir()`

Renames a directory, durably; retried every 50 ms, `tries` times in all, while another process (a virus scanner, the
indexer) holds a file in it. 0 on success. A folder rename cannot cross drives: save/ linked to another drive than
save-tx/ fails every commit here (link the whole profile folder instead), and the log line says so.

### `copy_file()` `same_file()`

`copy_file()` copies one file; the source may be open for writing (a save file the game has not closed). 0 on
success. `same_file()`: whether files a and b hold the same bytes; 0 when either cannot be read.

### `copy_tree()`

Copies the tree `src` to `dst`, which must not exist; links are skipped. `durable`: every file is flushed, and every
directory as far as the file system allows. `base` (or NULL) is the committed save: a file whose bytes equal base's
file at the same path becomes a hard link to it instead of a copy (`*nlinked`), as committed files are never written
in place (the game writes the working copy). A save commit then writes the 3-4 files of the slot the game saved
instead of all 82 (300-380 ms of flushes on the game thread). 0 on success.

### `same_inode()` `same_tree()`

`same_inode()`: whether paths a and b are one file (hard links of it). `same_tree()`: whether tree a holds the names
and kinds of tree b and each file of it as a hard link: what `copy_tree()` makes from a working copy whose every file
equals base's.

### `backup_names()` `rotate_backup()`

`backup_names()`: the backups' folder names (digits first), sorted: oldest first, as `rotate_backup()` names them in
creation order.

`rotate_backup()` moves old to save-backup/<UTC time> and deletes all backups but the newest `SAVE_BACKUPS`. The name
always sorts after every existing one: a commit in the same millisecond as the newest backup (or after the clock went
back) takes its time and the next -NNN suffix. A name that sorted lower would be the first one pruned, losing the
backup just made. With no suffix on the newest, k stays 0; the suffix is raised until the name sorts after the newest,
even after an unpadded "-1" of older builds.

### `newest_save()`

The newest save: the finished commit waiting to be swapped in, else the committed save.

### `swap_in()`

Steps (3) to (5): swaps the finished commit `ready_root` in for the committed save. 0 on success; on failure
`ready_root` stays the newest save. (3) is tried for 200 ms only (`SWAP_TRIES`, 4): the commit is safe in
`ready_root`, the next Commit or start tries again, and the game's next change to its save data waits for this one. An
old left by a failed (5) is rotated first: (3) needs its name. When (4) fails old is moved back; else save/<uid> stays
absent and the next start rolls forward.

### `ready_number()` `ready_scan()` `save_recover()`

`ready_number()`: the number n of a "ready-<n>" name, 0 for any other name. `ready_scan()`, at startup: the newest
finished commit into `ready_root`; the older ones, which it supersedes, are deleted. `save_recover()`, at startup:
finish or roll back an interrupted Commit and drop uncommitted changes (the rule above).

### `work_create()` `work_discard()`

`work_create()`: the first save mount gets a fresh working copy of the newest save. It uses `remove_tree_retry()`: the
last unmount's delete may have been held up by a scanner, as at the start. `work_discard()`: at the last save unmount,
changes no Commit took are dropped.

### `save_commit_locked()`

Commit of the save mount, `tx_lock` held: steps (1) to (5) above; a failure before (2) leaves the newest save as it
was, one after it leaves the commit waiting in `ready_root`. When the game rewrote equal bytes (every file linked and
the tree the same as the base) there is no save and no backup. A `ready_root` that the new commit supersedes is
removed: the working copy it was made from held all of it.

### `save_wait_commit()` `fs_save_wait()` `fs_save_flush()`

The background commit: `commit_pending` counts the game's Commits whose thread has not run `save_commit_locked()` to
its end yet. A count, not a flag: with two Commits entered at once the first thread's end cleared the flag while the
second still waited for `tx_lock`, and a save write then went into the working copy the second was copying.
`exiting` (under `pend_lock`): `fs_save_flush()` ran, the process is ending.

`fs_save_wait()` is the orderly end's first step (`rt_exit_steps` of `runtime/gfx/gfx_present.c`): a Commit under way
finishes before the quick put-backs and the slow pipeline save, and a later one is still taken; `fs_save_flush()`, the
end's last step, refuses those. `fs_save_flush()`: the window closed: a Commit already started finishes first, and none
starts afterwards, as the process exit would cut it off halfway (the startup recovery then keeps the previous one;
here it is kept outright).

### `fs_save_crash_wait()`

The process is about to end on a crash or a fatal error (`crash_handler` of `runtime/main.c`, `rt_fatal` of
`runtime/dispatch.c`). The game's Commit returns once its commit is queued, so the game may already have told the
player the save is done while the commit thread still copies (95-135 ms): ending now would leave an unfinished copy,
which the next start rolls back. The thread goes on while this one waits, for at most `CRASH_WAIT_MS` (2000), without
taking a lock (the failing thread may hold one). Not when the failing thread is the one committing: its commit cannot
finish.

### `fs_save_snapshot()`

A prefix that begins with a digit is refused: a name that begins with a digit is one the pruning may take. Two copies
in one millisecond get a numbered name. `tx_lock` is held so no Commit swaps the save while it is copied. With nothing
saved yet it returns 1: nothing to go back to. In the pruning the newest `keep` copies stay; their names sort by time;
a prefixed one is never pruned.

### `save_commit()`

It waits for the previous Commit first. `save_writers` is raised so `fs_save_busy()` holds until the thread is done.

### A shared save

`runtime/hle/save_share.c`: the export reads the newest save, the menu's import waits for the next start.

| Folder | What |
|---|---|
| `<profile>/save-import/` | a save the menu took from a zip: it becomes the save at the next start |
| `<profile>/save-import.part/` | that save while the menu writes it; renamed to save-import when complete |

The import cannot replace the save while the game runs: the game holds its progress in memory and writes it back at
its next save, over the imported files, and the save mount's working copy is made once per mount. So the start takes
it (`runtime/hle/fs_host.c` `import_pending()`), in `storage_init()` after `save_recover()` and before the game mounts
its save: the save the game would load is copied to `<game folder>/saves/backup <local time>`
(`save_share_backup_dir`, where the launcher keeps the backups of its imports), then the import becomes a finished
commit (ready-<n>) and swaps in as one (`swap_in()`), which also moves the replaced save to save-backup. When the copy
fails the import waits for the next start.

`import_done` is the backup folder of the import this start made the save; "-" with none to back up. In
`import_pending()` an empty account folder has nothing to keep, so its backup folder is removed; a waiting
`ready_root` is removed, being the save the backup holds.

### `export_tree()` `fs_save_load()` `fs_save_export()`

`export_tree()` visits every file under `dir` (`rel`: its path below the save, "" at the top), in name order; links
are left out. -1 when a file cannot be read (`plat_file_read` gives NULL for an empty file as well, so the size is
checked). `fs_save_load()` and `fs_save_export()` hold `tx_lock`: the commit thread holds it while it swaps a commit
in, and the committed save is never written in place, so no commit swaps the save meanwhile.

### `stage_file()` `fs_save_import_stage()` `fs_save_import_waiting()`

`stage_file()` writes one file and flushes it; 0 on success. `fs_save_import_stage()` starts storage first, so the
start's own `import_pending()` has run: this import waits for the next start. `fs_save_import_waiting()`: the menu
asks every frame; before the game's first storage use the start's own `import_pending()` has not run, and this is no
reason to run it on the present thread, so it answers 0.

### `fs_host_lock_profile()`

One game per profile: a second one's start would take the first one's commit in progress for an interrupted one and
delete it, and both would write into one working copy (save-tx/work), losing or mixing their saves. `rt_boot` takes
the lock before the shader prewarm, so a second copy ends at once instead of after the prewarm's seconds of work on
every core and the gigabytes of memory it commits; `storage_init()` takes it again (`plat_lock_file` keeps one handle
per path, so the second call holds it already).

### `storage_init()`

`save_recover()` runs before `save_root` is created: an absent `save_root` means a commit to complete.

### `fs_save_dir()`

The committed save's folder, with its slot_00..05 (a tracker reads the save files there: the status of
`runtime/diag/tracker_api.c`); "" until the save storage is made, at the game's first storage use. `save_root` does
not change after that.

### `mount_add()` `component_ok()` `host_path_kind()` `save_change_path()`

- `mount_add()` refuses the RomFS mount's name (`FS_R_ALREADY_EXISTS`).
- `component_ok()`: a path component every host stores under exactly this name: not empty, ".", "..", no reserved
  character, no trailing dot or space (Windows strips those, which would alias two names).
- `host_path_kind()`: "name:/a/b" -> host path; 0 on success; `*kind` gets the mount's kind (`M_SAVE` ...) when
  `kind` is given.
- `save_change_path()`: a change to save data through a path (create, delete, rename, directories) marks the save as
  in progress.

### `mount_save_ro()`

Other titles' save data (data-link bonuses) is not present on this host.

### `unmount()` `commit()`

`unmount()` waits for a Commit first: else the working copy could go before a Commit queued just now has copied it.
`commit()` of a save mount: durability comes from the staging copy, not the working copy's handles.

### `file_of()` `file_closed()` `file_clear()` `file_release()` `close_file()`

A guest handle's host file is used outside `lock` (a read or write may take long, and a save write waits for a Commit
first, `save_touched()`), so a CloseFile on another thread could close it in the middle of a call. Windows hands a
closed HANDLE's value to the next file opened, so the call went on in another file: a save write could land in the
wrong one. Each call now counts itself in the slot from `file_of()` to `file_release()`. CloseFile closes at once when
no call is under way, and otherwise marks the slot; the last call out closes it. Nothing waits for another call: a
call takes `lock` exclusive at its end only when it finds the slot marked, so the fsync loop a Commit runs under the
shared lock (`commit()`) does not hold up the end of every read and write. The slot (and with it the guest's handle
value) is not handed out again before the close, and a call that comes after the CloseFile gets
`FS_R_INVALID_ARGUMENT`, as a closed handle does.

- `file_of()`: `closing` is stored only under the exclusive lock. Shared holders count themselves at once;
  `close_file()` reads the count exclusive.
- `file_closed()`: after the host file's close, the save's bookkeeping, as a closed save file needs.
- `file_clear()` empties a slot, under the exclusive lock. `users` and `closing` are stored as atomics: the call that
  has just left the slot may still read `closing` without the lock (`file_release()`). `open_file()` fills only `h`,
  `mode`, `save` and `boot_map`.
- `file_release()`: the end of a call that `file_of()` let in. The last call out of a slot CloseFile closed meanwhile
  closes the file. The decrement and `close_file()`'s mark are a sequentially consistent pair: either this call sees
  `closing`, or `close_file()` sees `users` at 0 and closes the file itself. Only the first case takes the lock, and
  its check under the lock keeps the close to one.
- `close_file()`: the mark first, then the count (`file_release()` does the reverse): a call that is still under way
  closes it.

### `host_read_at()`

Positional reads: the bytes read at `off`, 0 at the end, -1 on an error. The buffer is thawed first (`mem_thaw`): the
kernel's write would fail on a frozen page instead of faulting. A result of -2 is a read-only page in the buffer: a
frozen region missed its thaw, and the log says so.

### `open_file()`

A save file read to be loaded while a boot map is pending: its reads get the boot map's place
(`runtime/patches/boot_map.c`). `users` and `closing` are 0 in a free slot (`file_clear()`).

### `write_file()`

The range check is offset + size without the sum: a size near 2^64 would wrap it below the file's size. With
`NX2SARIA_WB_PROBE` the kernel reads the buffer, and a probed page would fail the write instead of faulting; so would
a heap page `NX2SARIA_HEAP_LAZY` has not committed yet, so the buffer is thawed while probing. Bit 0 of the option is
WriteOptionFlag_Flush.

### `delete_file()` `rename_file()`

DeleteFile on a directory answers `FS_R_INVALID_ARGUMENT`. A rename across mounts answers `FS_R_INVALID_PATH`.

### `open_directory()` `read_directory()` `directory_entry_count()`

- `open_directory()`: Switch 2 titles list RomFS folders (content:/Foliage/...); the Switch 1 title never did.
- `read_directory()` holds `lock` to the end: CloseDirectory on another guest thread frees the listing this copies
  from. A name over 768 bytes is skipped: `nn::fs` cannot name it.
- `directory_entry_count()`: `Result GetDirectoryEntryCount(int64_t *out, DirectoryHandle h)`: the entries
  ReadDirectory would still return. The shared lock is against a CloseDirectory on another guest thread.

### The overrides

AAPCS64 thunks. FileHandle, DirectoryHandle, CacheStorageListHandle and ApplicationId are 8-byte structs passed in
one register; Uid, ReadOption and WriteOption go by reference. `ov_set_priority()` does nothing: the fs priority only
orders I/O inside the system fs process; host I/O is unordered. `S_MOUNT_ROM`, `S_QUERY_CACHE` and `S_INITIALIZE` are
the symbols `runtime/hle/romfs.c` overrides.

### `probe_fs()`

`--probe-fs`: every check calls the overrides the way guest code does: by synthetic address through `rt_call`, with
AAPCS64 arguments in a `Cpu`, on this (probe) thread. It prints counts only, never RomFS paths.

- `HEAD_BYTES` (8 MiB): a sampled file is compared whole up to this size, else head (and `TAIL_BYTES`, 64 KiB) tail.
- `fs_call()`: the override registered for `sym`, called through its synthetic address with up to 6 arguments; x0..x18
  hold caller-saved garbage first. It returns x0.
- `compare_range()` reads [pos, pos+n) of the open RomFS handle `h` through one of the three ReadFile overloads, and
  compares it with an independent read of `romfs.bin` at the index offset.
- `probe_romfs()` samples 64 files (`SAMPLES`) and the largest file beyond the 64. A read at EOF gives 0 bytes, one
  past EOF `FS_R_INVALID`. The same file is opened under "rom:/", "rom://" and "rom:///"; its folder is a directory;
  traversal and writes are refused. Overlays (`NX2SARIA_ROMFS_PATCHES`, `NX2SARIA_ROMFS_MODS`): served in place of the
  original, added files and their folders exist, bytes equal the overlay file.
- `probe_restart()`: the process "dies" here: the save mount goes without Unmount, and the next start runs
  `save_recover()` and mounts again.
- `tx_commit()`: Commit through the override, then a wait for its background thread, as the checks read the disk
  next. Not with `fs_save_flush()`: that also marks the process as exiting, and every later Commit of the probe would
  be refused.
- `tx_write()` fills a save file, created when absent, with byte g through the overrides. `tx_read()`: the byte a
  save file is filled with, read through the overrides; -1 absent, -2 short or mixed. `tx_read_committed()`: the same,
  read from the committed save on disk. `tx_links_committed()`: the hard links of a file of the committed save on
  disk; 0 when absent.
- `probe_save_tx()`, the save transactions: restarts, uncommitted and committed changes, a crash at every step of a
  Commit. In order: uncommitted changes (the game reads them; the committed save and the next start do not see them);
  committed changes survive a restart, and an Unmount without Commit drops changes; a crash at step k of a Commit,
  k = 1, 2, ... until a Commit completes (the next start sees all of the old generation or all of the new one); a
  Commit writes the changed file and links the unchanged one to the committed save it replaces; a Commit that changed
  no byte (the game rewrites its files at start) makes no new save and no backup; the background commit (a write right
  after Commit waits for it and stays out of it; an Unmount right after Commit waits for it and keeps it); a file of
  the committed save held open by another program (a virus scanner, Explorer's preview, a sync tool): Windows refuses
  to rename a directory with an open file under it, so the committed save cannot be swapped out, the game and a
  restart still see every Commit, and the first start after the file is closed swaps them in; the read-only mount
  shows the committed save and refuses changes; a run that left read-only folders and files in the working copy
  (attributes another emulator's save carried): the next start deletes them and names the failing path if it cannot.
- `probe_storage()`: create, write, commit; paths; directories; reopen after unmount (the bytes come back through the
  overrides and are on disk); resize, rename, delete; copies of the save (the first one, prefixed, outlasts the
  rotation of the later ones); temporary and cache storage. At the end `storage_init()`'s one-game-per-profile lock,
  held open until then, is given back before the folder is removed.
- `probe_fs()` calls `nn::fs::InitializeForApplication` (`fs_init()` again, idempotent).

### `fs_host_sizes()`

`NX2SARIA_TRACE=perf` "mem: sizes" (`runtime/diag/perf.c`): open save-data files and directory listings against their
tables.

## `runtime/hle/romfs.c`

The read-only game RomFS, backed by the player's extracted image.

Input: `<romfs>/romfs.bin` (`rt_config()->romfs`), whose six IVFC levels and SHA-256 `tools/extract.py` verified, and
`romfs-index.bin` next to the executable (`NX2SARIA_ROMFS_INDEX` overrides), which `tools/romfs_index.py` wrote
together with `romfs_config.h`. `fs_init()` checks the index's SHA-256 and layout and the image size. The full 17 GB
hash runs only with `NX2SARIA_ROMFS_VERIFY=full`; a proof file (`.romfs-verified-v1` next to the index, or in
`NX2SARIA_ROMFS_PROOF_DIR`) keyed by the file's volume, file id, size and times skips it on later starts;
`NX2SARIA_ROMFS_VERIFY=force` ignores the proof.

Reads are positional (`plat_file_pread` of `runtime/platform/platform.h`), so threads read the image in parallel
without a file-pointer lock. `NX2SARIA_ROMFS_MMAP=1` maps the image instead.

Overlays: files listed in a directory's `manifest.tsv` are served in place of the originals. Each line names a RomFS
path, the overlay's size and SHA-256 and the original's SHA-256 ("-" adds a file the RomFS lacks; it is numbered after
the RomFS's own files, file_count + k in path order). The directories, in this order: `NX2SARIA_ROMFS_PATCHES` and
`NX2SARIA_ROMFS_MODS`; unset or "0" = none. A missing path, a size or hash mismatch or a changed original stops the
start. An overlay file is opened when the game opens it; its identity (volume, file id, size, last write) is kept from
the start check, and an open that finds another file there stops the game.

Player mods (`runtime/patches/mods.c`): the files under each enabled mod's RomFS root are overlays too, checked by identity
only (no hash, no manifest). A path several overlays give is served by one: a player mod higher in the order beats a
lower one, any player mod beats the `NX2SARIA_ROMFS_MODS` manifest, which beats romfs-patches (`overlay_contest()`). A
mod path that matches a RomFS path only without case is served under the RomFS spelling.

The resource size table: with a player mod enabled, the game's table is rebuilt at start with the tables the mods
ship merged in and the entries of the mods' files raised to what they need (`restbl_overlay()`), and served from
`<profile>/mods/` in place of the game's or a mod's table.

- `NH` (65536) is the RomFS handle slots; `READ_CHUNK` (64 MiB) the largest single read; `ADDED` (`UINT64_MAX`) an
  added overlay file's index until `overlay_init()` numbers it.
- `Identity` has no padding.
- `Overlay`: `prio` says which overlay serves a path two of them give (the higher): romfs-patches 0, the
  `NX2SARIA_ROMFS_MODS` manifest 1, a player mod 2 + its distance from the bottom of the order. `mod` is its index in
  `mods_at` + 1, 0 for any other overlay.
- `index_dir` is the directory of `romfs-index.bin`: the proof file's default place.
- `NROM_NAMES` (4): the names the RomFS is mounted under: one for the Switch 1 title; Switch 2 titles mount it again
  under other names (`nn::fs::MountRom(const char *)`), each an alias of the same image.
- `handles[]` holds the file index + 1 (0 is free); `handle_ovl[]` the overlay slot + 1 of an open handle (0: the
  original); `handle_file[]` the handle's open overlay file, when `handle_ovl` is set; `overlays` is sorted by RomFS
  index after `overlay_init()`; `open_ms` and `open_n` are `open_note()`'s: per file, the last open and the opens in a
  row; `added` the RomFS paths of the added files, sorted (`added[k]` is index file_count + k); `patches_loaded` that
  a romfs-patches manifest gave a file (`romfs_patches_loaded()`).

### `rom_relative()`

The path relative to the RomFS root when `path` is "<a mounted name>:/...", else NULL. Repeated '/' inside the path
read as one (`romfs_path_collapse` of `runtime/hle/romfs_path.h`); the result lives in a per-thread buffer until the
thread's next call.

### `read_at()`

Positional reads: the bytes read at `off` (0 at or past the end of the file), -1 on an I/O error. As in
`runtime/hle/fs_host.c` `host_read_at()`, the buffer is thawed first (the kernel's write would fail on a frozen page
instead of faulting), and -2 means a read-only page in the buffer: a frozen region missed its thaw.

### `sha_range()` `verify_image()`

SHA-256 over a file range, streaming; and the full image verification with a proof file (`RomfsProof`).

### `file_index()` `folder_exists()` `overlay_slot()`

The index lookup. `file_index()`: the index of RomFS path `p` (an added file: file_count + k), or `UINT64_MAX`.
`folder_exists()`: 1 when a RomFS file or an added one lies under `folder` (a path ending in '/'). `overlay_slot()`:
the overlay slot + 1 that replaces RomFS file i, or 0.

### `overlay_load()`

Loads and checks one overlay folder. A failed check stops the game, except with `optional` set (a prompts
folder): the folder is dropped, the reason logged, and 0 returned. It returns 1 when the folder is loaded or has no
manifest. `prio` is the `Overlay` field (0 for the port's own folders, 1 for the `NX2SARIA_ROMFS_MODS` manifest).

### `index_any_case()`

Player mods (`runtime/patches/mods.c`): every file under an enabled mod's RomFS root is taken by identity (no hash: a texture
pack is gigabytes and the player changes it at will; `romfs_open()` still stops on a file that changed after the
start). `folded` is the RomFS indices sorted by name ignoring ASCII case, built on the first mod path that misses.

`index_any_case()`: the RomFS index of `name`, or of the RomFS path equal to it but for case (a mod made on Windows,
where an emulator's folder lookup ignores case); `UINT64_MAX` for a path the RomFS lacks.

### `other_version()`

1 when `path` names a file of one game version only (Product.<n> in a name: the resource size table, the actor
database and other version-bound files); the RomFS lacking it means it is another version's.

### `is_restbl()` `shipped_add()` `restbl_find()` `restbl_collect()`

The resource size table's inputs: the tables the overlays carry, before `overlay_contest()` keeps one of them.
`restbl_rel` is the RomFS path of the game's table, "" when it has none. `Shipped.mod` is the ModInfo index + 1, 0 for
the port's folders; `other` says it was made for another game version.

- `is_restbl()`: 1 when `rel` is a resource size table of any game version.
- `restbl_find()`: the game's table: the one `System/Resource/ResourceSizeTable.*.rsizetable.zs` of the RomFS.
- `restbl_collect()`: the tables the overlays carry for this game version, every one of them (`overlay_contest()`
  keeps one).

### `mod_walk_cb()` `mod_walk()`

`MOD_WALK_DEPTH` (32): folders nest at most this deep in a mod's RomFS folder: each level costs a stack frame (two
4 KiB paths here and the listing's own), and no RomFS path of the game is deeper.

`mod_walk_cb()` takes one entry of a mod's RomFS folder. A resource size table for another game version is still
kept as shipped: its entries for this mod's files still apply. `mod_walk()`: the files under `dir` (a mod's RomFS
root; `rel`: the RomFS path of `dir`, "" at the root) as overlays of mod k. A folder that cannot be listed (no
permission, a path longer than Windows allows) loses its files, and the log says so.

### `contest_order()` `overlay_contest()`

`contest_order()`: what overlays compete on: the RomFS index, an added file's path; the higher `prio` first.
`overlay_contest()`: one overlay per path: the highest `prio` serves it; the others are dropped and counted on their
mod.

### `mods_overlay_init()`

A mod with ExeFS patches waits for the rebuild that applies them: half of a mod is worse than none.

### `added_init()`

Numbers the added files in path order after the RomFS's own (file_count + k); `overlay_contest()` left one per path.

### `read_served()` `served_overlay()` `serve_generated()`

- `read_served()`: RomFS file `rel` as the game would read it: from an overlay that replaces it (`*base`), else from
  the image. malloc'd; NULL when it is missing, unreadable or 16 MiB or more (logged with `keeps`, what the game does
  without it).
- `served_overlay()`: the overlay that serves RomFS file `rel` now (the last one, as `read_served()` picks it), or
  NULL. A pointer `read_served()` gave goes stale once anything adds an overlay, since the table can move
  (`serve_generated()`'s realloc): ask again.
- `serve_generated()` serves `out[0..out_n)` (malloc'd, freed here) in place of RomFS file `rel` and of an overlay
  that replaces it (`base`): written to `<profile>/<sub>/<file name of rel>`. `what` names the change in the log. 0
  when it cannot be written. A base under the profile folder is one an earlier change wrote here (the action
  prompt's), not a mod's.

### `content_size()` `restbl_unpack()`

The resource size table (`runtime/hle/restbl.c`). `content_size()`: the decompressed size of a file's bytes as the
game unpacks them: the content size its zstd frame records (.zs; the game's files and zstd's own tools write it), four
times the bytes for a frame without one, else its size. `restbl_unpack()`: a table from its zstd bytes; NULL when they
are not one.

### `restbl_overlay()`

With a player mod (or the `NX2SARIA_ROMFS_MODS` manifest) serving files: the game's table with every shipped table of
this version merged in (an entry larger than the game's wins), the entries a mod's table for another version gives for
that mod's files, and each served mod file's entry raised to what its size needs (`restbl_grow` for a file the game
has, `restbl_estimate` for an added one). It never lowers the game's entries.

Of another version's table only the entries of the files its mod serves are taken. A file with no entry in the game's
table is left: the game sizes it without the table (streamed). For the estimate a mesh codec file counts at 8 times
its size: it unpacks to several times its size.

### `restbl_room()`

The resource size table with the entry of `rel` (a file of the game's that a change of ours serves larger: its
decompressed size from `was` to `now`) raised to hold it (`restbl_fit` of `runtime/hle/restbl.c`: the growth added to
the entry the table served has, a mod's higher merged entry included), served in its place and written to
`<profile>/<sub>/`. The game sizes the buffer it reads a file into from its entry, so a larger file needs a larger
one. It runs after `restbl_overlay()`, whose table, when it served one, is the one raised. 1 when served, 0 with why.

### `heapdef_overlay()`

The HeapDef table a bigger game resolution needs (`runtime/gfx/resolution.c`): its texture heap and that heap's
parent grow. The base is an overlay that replaces the table (a mod's heap sizes stay), else the RomFS original; the
grown table is written to `<profile>/resolution/` and served in the base's place. Switch titles name it for NX
(`System/HeapSize/Product.Nin_NX_NVN.HeapDef.byml.zs`), Switch 2 ones (Ocarina) for Ounce.

### `camera_overlay()`

The player camera's pack (`CAMPACK`, `Pack/Actor/PlayerCamera.pack.zs`) with its stick speeds multiplied by
`NX2SARIA_STICK_SENS` (`runtime/input/camera_speed.c`). The base is an overlay that replaces the pack (a mod's camera
stays), else the RomFS original; the dictionary is the RomFS's own. It runs also at factor 1: `cam_pack_build` reads
the game's own values, which a change during the run needs.

### `action_prompt_overlay()`

The action prompt's diamond of face buttons for the pad layout at start (`runtime/patches/action_prompt.c`). By
position (the Switch's places) the game's own layout is right; by label the Common layout archive the game would read
(a mod's, else the RomFS's) is served with the diamond laid out again, written to `<profile>/action-prompt/`. A pad is
not open yet this early, so auto counts as by label, as for the prompts' folders (`input_prompts_folder` of
`runtime/input/input_map.c`). `action_prompt_for` is what `romfs_action_prompt()` returns.

### `wide_hud_overlay()`

The HUD at the picture's edges (`NX2SARIA_WIDE_HUD = edges`) and the loading screen's icon in the middle 16:9
(`NX2SARIA_WIDE_MENUS = center`), `runtime/patches/wide_hud.c`: the Common layout archive the game would read (a mod's,
the action prompt's above, else the RomFS's) is served with them, for the picture `wide_init` sets up, written to
`<profile>/wide-hud/`. On a build with the widescreen sets, one line says the HUD's mode, the picture's shape and what
each layout moves, whether or not anything does, so a player's log shows the setting in effect. The feature:
[wide-hud.md](../wide-hud.md).

- Without `wide_available()` it returns: no squeeze to place anything against (`runtime/cfg/cfg_support.c`: the
  settings do nothing).
- `wider` is 0 for a picture of 16:9 or narrower. The log lists what the HUD's edges, then the menus' redesign, change
  in each layout.
- The Common layout archive (the HUD, the loading icon and most of the menus' redesign) comes first, then the title
  screen's when the menus fill the picture: each is the file the game would read (a mod's, else the RomFS's), edited
  and written to `<profile>/wide-hud/`.
- A larger archive (the pause menu's backdrop in pieces) needs a larger resource size table entry; without one the
  game's buffer would be too small, so the archive is made again without what grew it.
- The base is asked for again before serving: `restbl_room()` may have added the resource size table's overlay, which
  can move the table.

### The prompts' folders

The button prompts' folders (`input_prompts_folder`) and whether each had a manifest next to `romfs-index.bin` at
start; the one the settings asked for at start, the one `overlay_init()` loaded (NULL: the game's own prompts) and why
the first did not load (`ROMFS_PROMPTS_*`). The menu compares them with the settings in effect (`prompts_note` of
`runtime/ui/menu.cpp`). romfs-patches-pc keeps that name, but at a game resolution of another shape than 16:9 its
files come from `pc_dir`, romfs-patches-pc-<w>x<h> (`overlay_init()`).

`PROMPTS_PADS` is 3, the pad styles: Xbox, PlayStation, Steam Deck. `PROMPTS_FOLDERS` is `PROMPTS_PADS` * 2 + 1: each
pad style by position and by label, then PC. `prompts_style()` and `prompts_value()` give a style's name in the log
and its `NX2SARIA_PROMPTS` value.

### `overlay_clash()` `overlay_drop()`

`overlay_clash()`: the first RomFS file that the overlays [a, b) and [c, d) both replace, or NULL. The resource size
table is no clash: `restbl_overlay()` merges both tables. `overlay_drop()` drops the overlays [a, b), the ones after
them moving down.

### `overlay_init()`

`NX2SARIA_ROMFS_PATCHES` unset: romfs-patches next to `romfs-index.bin` if it has a manifest
(`tools/make_romfs_patches.py`). A game resolution that is not 16:9 takes romfs-patches-<w>x<h> instead when it is
there: the same overlay with the UI laid out for that shape (`make_romfs_patches.py --aspect`), when the game version
has the camera aspect patch the layout goes with (`res_wants_wide_overlay`).

With prompts = xbox, playstation or steamdeck, that pad's prompts folder for the pad layout takes its place
(`input_prompts_folder`): the game's button font and interface textures with that pad's buttons drawn in
(`tools/pad_prompts.py`). The game reads the font and the archives once at boot, so the prompts stay until restart
even though the layout itself changes live. Each folder replaces romfs-patches as a whole and the setup builds no
folder of another shape with a pad's prompts, so a game resolution that is not 16:9 keeps the Switch prompts. So does a
RomFS mod (the `NX2SARIA_ROMFS_MODS` manifest or a player mod) that replaces one of the prompts' fonts: romfs-patches
loads in their place, so the mod's file is not served beside prompts files made for the game's own. A mod's resource
size table is no clash: `restbl_overlay()` merges it with the prompts' one.

prompts = pc takes romfs-patches-pc whatever the pad layout, and at a game resolution of another shape
romfs-patches-pc-<w>x<h> (`make_romfs_patches.py --prompts pc --aspect`): it carries that shape's UI layout too, so it
replaces romfs-patches-<w>x<h> as romfs-patches-pc replaces romfs-patches. The player's side:
[controls.md](../controls.md#button-prompts).

- The folders are each pad's two (`nintendo`, `xbox` layouts), then the PC one.
- The log says where the build drew the Xbox, PlayStation, Steam Deck and keyboard prompts from, or why it drew none
  (its record in the game folder), whatever the prompts asked for: a report's log then says which prompts the Controls
  menu can offer.
- A shaped PC folder is loaded in place of romfs-patches-<w>x<h>, whose UI layout it carries.
- `wide_hud_overlay()` runs after the action prompt's: it edits the same archive, as that one serves it.
- After `overlay_contest()` left one overlay per file, the generated ones replace in place.

### `index_path()`

`romfs-index.bin` next to the executable, or `NX2SARIA_ROMFS_INDEX`.

### `romfs_load()`

The whole of a RomFS file by its path from the RomFS root, for host-side readers (`runtime/ui/menu_sound.c`: the
game's own interface sounds); malloc'd, NULL when the file is absent or not in the image (an added mod file). The
original file, not a mod's replacement. It opens the RomFS when nothing has yet: the boot screen (`menu_boot_frame` of
`runtime/ui/menu.cpp`) loads the menu's fonts before the guest's first file access.

### `movie_skipped()`

`NX2SARIA_MOVIES=off` skips movies: the RomFS reports .webm files as absent, and the game's movie player
(`main+0x3b5eb40`), which opens the file with `nn::fs::OpenFile` before it creates a decoder, abandons the movie
cleanly. Movies play otherwise (`runtime/hle/ov_movie2.c` decodes them).

### `open_note()`

A file the game cannot load, damaged or refused for any other reason: its loader opens it again and again and in the
end does without it, and whatever needs it stays out of the world. `tools/romfs_check.py` finds damaged files before
play; this names the ones that still fail, in the log. One timestamp and one count per file, read and written only
here, where a file is opened (a few thousand times a session), without a lock: a race at worst logs a line twice.
`RETRY_OPENS` is 24 opens, each less than `RETRY_GAP_MS` (3000) after the last; a file reloaded in play stays far
below.

### `area_name()` `scene_note()`

`NX2SARIA_TRACE=perf` and the stutter map (`runtime/diag/stutter_map.c`): an area's static placement,
`Banc/<area>/<area>_<age>_Static.bcett.byml.zs`, is read once when the area loads (the game streams a neighbouring area
the same way): `perf_scene` names it.

### `area_missing_fatal_note()` `area_missing()`

The game asked for an area's static placement and the RomFS has none: the save being loaded (or the game's own state)
names an area this game version does not have. The game does not check for that and reads through a null area object a
moment later (a child save naming HyruleCastle_Child on 1.1.1, whose castle has one placement for both ages). The log
names the area once, and the crash report that follows repeats it, so the report explains itself.
`area_missing_name` is the first such area, written once under lock; the fatal note reads it.

### `romfs_open()`

- `NX2SARIA_NPLN=off`: the online client's configuration (`npln_config.json`) is absent (diagnosis of the crashes that
  follow its start).
- With the perf trace or the stutter map on, `scene_note()` runs: the stutter map counts hitches by area.
- Opening `CAMPACK` calls `cam_pack_opened`: its values come into memory at the start's factor
  (`runtime/input/camera_speed.c`).

### `handle_source()`

The handle's index row (an added file's is empty: its bytes are all in its overlay), and where its bytes come from:
the overlay file, or the original's range of the image.

### `romfs_read_patch()` `romfs_read_patches_off()` `read_patch_apply()`

Read patches: a few bytes of an original file, changed in the reads that cover them (at most `READ_PATCH_MAX`, 8).
They are registered before the guest starts and only switched off later, so `romfs_read()` looks at the table without a
lock. `read_patch_apply()`: the read patches of the original file at image offset `base` that fall into the `n` bytes
read at `offset`.

### `romfs_list()`

The directory listing over the sorted index: every path under "<p>/" is contiguous, and so is every path under one
subdirectory, so one pass emits each file and each distinct subdirectory once. Files the RomFS-mods manifest adds
(`added[]`) are not listed yet.

### `ov_mount_rom()` `ov_query_cache()` `ov_initialize()`

- `nn::Result nn::fs::MountRom(const char *name, void *cache, size_t cache_size)`, and
  `nn::Result nn::fs::MountRom(const char *name)`, the Switch 2 SDK's form, without a cache.
- `nn::Result nn::fs::QueryMountRomCacheSize(size_t *out)`: no cache needed, the host reads the image directly.
- `void nn::fs::InitializeForApplication()`: SDK-internal; it opens the index instead of the fs service session.

### `romfs_sizes()`

`NX2SARIA_TRACE=perf` "mem: sizes" (`runtime/diag/perf.c`): open RomFS handles against the table.

## `runtime/hle/sarc.h`

SARC archives and the zstd dictionaries of `Pack/ZsDic.pack.zs` (`runtime/hle/sarc.c`).

- `sarc_next()`: the next member of the SARC `p[0..n)` from member `*next` on that is named `name` (any member when
  `name` is NULL): 1 with its start and size, `*next` moved past it; 0 when no such member is left; -1 when the archive
  is damaged (its header, or the found member's range).
- `sarc_member()`: the first member named `name`: 1 with its start and size; 0 when there is none or the archive is
  damaged.
- `sarc_count()`: the number of member entries of the SARC `p[0..n)`; -1 when its header is damaged.
- `sarc_rebuild()`: the SARC `p[0..n)` with the data of some members replaced: entry i (of `sarc_count()`'s) takes
  `with[i][0..with_n[i])` in place of its own bytes, or keeps them where `with[i]` is NULL. The header, the entries'
  order, hashes and names, and SFNT stay; the members' data is laid out again in the order the archive has it, each
  member at the first offset past the one before that keeps its start's alignment (the largest power of two dividing
  it, up to 8 KiB), and the entries' ranges and the archive's size follow. Entries that shared one range share it still
  unless one of them is replaced; a replaced entry gets a range of its own. malloc'd, `*out_n` its size; NULL when the
  archive is damaged, two entries' ranges overlap without being the same, or out of memory.
- `zsdic_unpack()`: `Pack/ZsDic.pack.zs`'s archive (`z[0..zn)`, a frame compressed without a dictionary) unpacked:
  malloc'd, `*n` its size; NULL when it is not a zstd frame that says its size, or that size is over `max`.
- `zsdic_find()`: the dictionary with id `id` among the members of that archive (`dics[0..n)`): 1 with its start and
  size; 0 when none has it (and always for id 0, the id of a frame without a dictionary).

## `runtime/hle/sarc.c`

SARC archives and the zstd dictionaries of `Pack/ZsDic.pack.zs`. A SARC is a little-endian header ("SARC", its length
at +4, the byte-order mark 0xfeff at +6, the archive's size at +8, the start of the members' data at +12), then SFAT:
its member count at +6 and from +12 one 16-byte entry per member (name hash, attributes, start and end relative to the
data). An entry's attributes hold a nonzero top byte when the member has a name, and in the low 24 bits the name's
offset in SFNT's string table divided by 4; SFNT's 8-byte header follows the entries. The game's archives may hold
several members under one name (`UI/Layout2Archive/Common...blarc.zs` holds each layout four times), so members are
walked one by one. `sarc_rebuild()` lays the members out again when some of them change size.

### `sarc_head()`

The header's fields: SFAT's start, its entry count and the start of the members' data. 0 when it is damaged. The
entries end where SFNT starts.

### `sarc_rebuild()`

- The members' data starts after their entries and the name table's header (SFNT, 8 bytes): a data offset inside them
  would let the copies write over entries still to be read, so such an archive is refused.
- `Range` is an entry's range of the members' data, and the entry's number. `at[]` is each entry's new start, from the
  data's.
- The ranges are laid out in the archive's order: a kept range that is the same as the last kept one shares its place.
  A range that lies inside the range before without being the same one fails. `MAX_ALIGN` is 0x2000.
- What followed the last member follows it still. A total over `UINT32_MAX` fails.
- The entries' old ranges are read from the input, which nothing writes: they are the ranges checked above.

## `runtime/hle/restbl.h`

The game's resource size table (`runtime/hle/restbl.c`):
`System/Resource/ResourceSizeTable.Product.<n>.Nin_NX_NVN.rsizetable.zs`, a zstd-compressed RESTBL. The game sizes the
buffer it loads a file into from the file's entry, so a mod file larger than the original needs a larger entry, and a
file a mod adds needs one of its own. `runtime/hle/romfs.c` merges the tables the mods ship and raises the entries of
the files they serve (`runtime/hle/romfs.c` `restbl_overlay()`).

- `restbl_parse()`: the table of a decompressed RESTBL (copied); NULL when it is not one.
- `restbl_key()`: the table's key of a RomFS path: the path without a trailing .zs or .mc, into `buf`.
- `restbl_get()`: 1 with `*size` when `key` has an entry.
- `restbl_raise()`: the entry of `key` raised to `size` (added when missing); 1 when the table changed.
- `restbl_merge()`: every entry of `shipped` larger than `base`'s (or missing from `base`) raised in `t`; the number
  of entries raised.
- `restbl_take()`: each entry of `from` whose key one of `keys[0..n)` names, raised in `t`; the number raised.
- `restbl_write()`: the table as a decompressed RESTBL (malloc'd); NULL when out of memory.
- `restbl_estimate()`: the entry a file of the key's type and decompressed size needs when the game has none for it;
  0 for a type the table never lists (streamed sound and video).
- `restbl_grow()`: the entry of a file the game lists with `entry` for its own copy of `old` bytes, now `new_size`
  bytes: raised by the growth times the type's factor, never lowered. `scaled`: the sizes are compressed ones (a mesh
  codec .mc file), whose entry grows in proportion.
- `restbl_fit()`: the entry of `key` raised for a file whose decompressed size grows from `was` to `now`:
  `restbl_grow()` on the entry the table has (a mod's merged entry above the game's stays above it),
  `restbl_estimate()` when it has none. 1 with `*from` (0 when the table had no entry) and `*to`; 0 when the key's type
  has no entries (streamed sound and video).

## `runtime/hle/restbl.c`

The resource size table (`runtime/hle/restbl.h`). The layout of a decompressed RESTBL, little-endian:

```
"RESTBL", u32 version (1), u32 name length (160), u32 hashed count, u32 named count
hashed entries {u32 crc32 of the key, u32 size}, sorted by the hash
named entries {char key[name length], u32 size}: the keys whose hash collides with another key's
```

A key is a RomFS path without .zs or .mc. The estimates come from the 1.4.3 table against its own files: most types
need the decompressed size plus at most 8 KiB (headers, alignment); a few need a multiple of it (bfres 1.5, bkres
2.05, bgyml 3.85, asb 1.4, bstar 1.2, bphsc 1.1). A type the table does not list takes 1.5 times its size plus 8 KiB.

`Restbl.named` holds the named entries as in the file: the key (name_len bytes, NUL padded) then a u32 size.

### `named_of()` `hashed_at()` `raise_hash()`

`named_of()`: the named entry of `key`, or NULL. `hashed_at()`: the position of `hash` in the hashed entries, or where
it would go. `raise_hash()`: the hashed entry of `hash` raised to `size` (added when missing); 1 when the table
changed.

### `type_factor()`

The factor a type's entry needs over its decompressed size (1000 = 1.0); 0 for a type the table never lists.

## `runtime/hle/save_zip.h`

The zip file of a shared save (`runtime/hle/save_share.c`; the launcher writes and reads the same file,
`launcher/saves.py` `export_save()`): a zip in memory with stored or deflated members, its members read back with
their CRC-32 checked, raw deflate both ways, and the top-level keys of its `manifest.json`. Plain C on the C library
alone, so `tests/save_zip_check.py` builds it by itself and checks it against Python's zipfile and zlib.

- `sz_crc32()`: the zip CRC-32; crc 0 to start, the last result to go on.
- `sz_deflate()`: raw deflate (RFC 1951) of `src[0..n)`: one block of the fixed Huffman codes over LZ77 matches. A
  malloc'd buffer of `*out_n` bytes, NULL when out of memory.
- `sz_inflate()`: raw deflate data `src[0..n)` into exactly `dst_n` bytes at `dst`: stored, fixed and dynamic blocks.
  0 on success, -1 when the data is damaged, ends early or holds another number of bytes.
- Writing: the members go into one buffer as they come, then the central directory after them. `SzWriter.failed`:
  out of memory, or past the 4 GiB a zip without its 64-bit extension holds.
  - `sz_writer_init()`: every member gets this local time (the zip's DOS time stamp, two-second steps).
  - `sz_writer_add()`: a member: its name relative with '/' separators, its bytes deflated when that makes them
    smaller, else stored.
  - `sz_writer_finish()`: the whole archive (`*n` bytes; the caller frees it), NULL when any add failed. It frees the
    writer's own memory either way.
- Reading. `SzEntry`: `name` as stored, with '/' separators; `data` the offset of its bytes in the zip; `is_dir` a
  folder entry (its name ends in '/'); `is_link` a symbolic link (a Unix mode in its attributes).
  - `sz_read_dir()`: the members of the zip `zip[0..n)` from its central directory: a malloc'd array in `*out` (the
    caller frees it) and its length in `*count`. `SZ_OK` or why not: not a zip, damaged, or what this reader leaves
    out (encryption, a 64-bit zip, a method but stored and deflate, a name of 256 bytes or more).
  - `sz_extract()`: the bytes of member `e`, checked against its CRC-32: malloc'd with a NUL after them (the caller
    frees them), NULL with `*err` set to the `SZ_E_*` reason.
- The manifest: a JSON object whose values the reader needs are strings and integers at its top level.
  - `sz_json_str()`: the string value of `key` into `out` (cut to `cap` - 1 bytes; \u escapes outside ASCII become
    '?'). 1 when found.
  - `sz_json_int()`: the integer value of `key`. 1 when found and an integer.
  - `sz_json_escape()`: `s` as the inside of a JSON string ('"', '\' and control characters escaped) into `out`, cut
    to fit `cap`. It returns `out`.

## `runtime/hle/save_zip.c`

The zip file of a shared save (`runtime/hle/save_zip.h`). Little-endian fields as the zip format (PKWARE APPNOTE) lays
them out; deflate as RFC 1951 defines it.

### `sz_crc32()`

CRC-32 with the zip polynomial, reflected; one table built on first use. Two threads may build it at once: they write
the same values.

### Inflate

`len_base` and the tables beside it are the deflate tables shared by both directions: length codes 257..285 and
distance codes 0..29.

- `get_bits()`: `need` (0..16) bits, the first read in the lowest bit; past the end of the input: 0 and `bad` set.
- `Huff` is a canonical Huffman code: how many codes of each length, and the symbols in code order.
- `huff_build()`: the code of `lengths[0..n)` (0: the symbol is unused). 0 when usable: no length is over-subscribed
  (an incomplete code is allowed, as a stream with a single distance code has one; a code it lacks then reads as
  damaged).
- `huff_decode()`: the next symbol of code `h`, the bits read one at a time (Huffman codes are packed from their first
  bit); -1 damaged.
- `inflate_codes()`: the symbols of one block until its end code. A match's copy overlaps on purpose.
- `inflate_stored()` drops the rest of the current byte first.
- `inflate_fixed()`: two threads may build the fixed codes at once: they write the same values.
- `inflate_dynamic()`: the code lengths' own code is built in `lit` for the time being. A block without its end code
  (no length for symbol 256) is damaged.

### `sz_deflate()`

Deflate: greedy LZ77 over a 32 KiB window (hash chains of 3-byte keys), the fixed Huffman codes.

- `put_code()`: a Huffman code goes out from its first bit: reversed into the LSB-first stream. `put_literal()` writes
  a literal/length symbol in the fixed code.
- `CHAIN` (48) is the candidates tried per position: logs repeat a lot, saves little.
- The output's capacity is n + n / 8 + 64: a literal takes at most 9 bits. The header bits are 1 (the last block) and
  1 in two bits (fixed Huffman codes).
- In the match search only a longer match can win; a chain ends at an older entry overwritten by a newer one. Every
  position a match covers goes into the chains. The stream is padded out to the byte.
- The overflow check at the end cannot happen with the bound above; it is kept as the guard `put_bits()` relies on.

### `room()` `sz_writer_add()` `sz_writer_finish()`

Writing a zip. Offsets and sizes are 32-bit without the 64-bit extension, so `room()` fails past 0xffffffff. A member
is stored when deflate does not help. Local header: version needed 20 (deflate). Central directory: made by 20
(MS-DOS attributes, version 2.0); the extra, comment, disk, internal attributes and external attributes fields are 0.

### `sz_read_dir()`

Reading a zip. The end record: its signature within the last 22 bytes plus the longest comment (0xffff). A spanned
archive and the 64-bit zip (0xffff entries, or a directory size or offset of 0xffffffff) are unsupported; so is a
member that is encrypted, uses another method, has a long name or is a 64-bit member. A link is a Unix entry whose
mode is S_IFLNK (made-by host 3, mode & 0170000 == 0120000).

### `json_string()` `json_skip()` `json_find()`

The manifest's JSON. `json_string()`: the string at `p` (its opening quote) into `out` (NULL: skipped); `*cut` set
when it did not fit. It returns the byte after its closing quote, NULL when it is not a string. `json_skip()`: past
one value: a string, an object or array (nested), or a number or literal. `json_find()`: the value of `key` at the top
level of the object `json`, NULL when it has none.

## `runtime/hle/save_share.h`

Sharing a save (`runtime/hle/save_share.c`): the menu's Export writes the save the game would load next into one zip,
for a bug report or another player, and its Import checks such a zip and leaves its save for the game's next start
(`runtime/hle/fs_host.c` `import_pending()`). The launcher writes and reads the same zip (`launcher/saves.py`
`export_save()` and `import_zip()`).

| Result | When |
|---|---|
| `SAVE_SHARE_NO_SAVE` | export: the game has not saved yet |
| `SAVE_SHARE_READ_FAILED` | export: a save file could not be read; import: the zip could not be read |
| `SAVE_SHARE_WRITE_FAILED` | export: the zip could not be written; import: its save could not be put in place |
| `SAVE_SHARE_NOT_A_SAVE` | import: not a zip of an NX2Saria save (no manifest, another format, no save files) |
| `SAVE_SHARE_OTHER_GAME` | import: the manifest names another game |
| `SAVE_SHARE_NEWER` | import: a newer NX2Saria made it, in a format this one does not know |
| `SAVE_SHARE_DAMAGED` | import: a member's data or CRC is wrong |
| `SAVE_SHARE_UNSAFE` | import: a path outside the save, a link, or far more than a save |

- `save_share_export()` writes `NX2Saria-save-<local date>_<time>[-bug-report].zip` into `dir` (made when missing);
  `bug_report` adds this run's log and `nx2saria.ini` (or an existing `nx2navi.ini`). `path` gets the zip's path.
  Never a key file.
- `save_share_import()` checks the zip at `zip_path` (manifest, game, paths, sizes, CRCs) and stages its save for the
  next start. `detail` gets "<NX2Saria version>, <export date>" from its manifest ("?" for what it lacks).
- `save_share_pending()`: 1 while an import waits for the next start. `save_share_cancel()` drops the waiting import;
  0 on success.
- `save_share_default_dir()`: where Export writes and Import's file dialog opens: the player's Downloads folder, else
  the game folder. 0 when neither is known.
- `save_share_backup_dir()`: a new folder name `<game folder>/saves/backup <local date> <time>` (with " (2)" and up
  when taken), the saves folder made; the folder itself is not. Where the launcher keeps the backups of its imports.
  0 when it cannot be named.

## `runtime/hle/save_share.c`

Sharing a save (`runtime/hle/save_share.h`). The zip, as the launcher's `launcher/saves.py` `export_save()` writes
it:

| Member | What |
|---|---|
| `manifest.json` | {"format": "nx2navi-save", "format_version": 1, "game", "title_id", "game_version", "nx2navi", "exported" (UTC), "exported_by", "files", "bug_report"} |
| `save/...` | the save's files as the game keeps them (`account.sav`, `option.sav`, `slot_00/progress.sav`, ...) |
| `bug-report/...` | with a bug report: this run's log and settings; an import leaves them out |

An import accepts a zip whose manifest has this format, a version up to `FORMAT_VERSION` (1) and the game's title id,
whose save/ paths stay inside the save, with no links, at most `MAX_FILES` (4000) files and `MAX_BYTES` (256 MiB),
among them a file of this game's save (`save_file()`). Every member's CRC-32 is checked before anything is written.

`TITLE_ID` ("0400486024dec000") is the game's title id (`NX2SARIA_APPLICATION_ID` of `runtime/hle/fs_host.c`).

### `exe_dir()` `same_text()` `key_file()` `save_file()` `safe_rel()`

- `exe_dir()`: the executable's folder, where `nx2saria.ini` (or a legacy `nx2navi.ini`), logs and saves are. 0 when
  unknown.
- `same_text()` compares ASCII in any case.
- `key_file()`: key files (a name ending in `.keys` or `.keys.txt`) never go into a zip or out of one, wherever a
  stray copy sits in a save folder.
- `save_file()`: a file only this game's save has: what makes the zip's save this game's.
- `safe_rel()`: a relative path that stays inside the save: '/' separators, no empty, "." or ".." part, no drive or
  backslash.

### `add_settings_file()`

The settings file for a bug report: as it is, except that the values of the private settings
(`runtime/cfg/cfg_private.h`) read "(set, not shown)". A report goes to people the player does not know. A folder in
the account's home is written short as well (`runtime/log_privacy.c`), as in the log beside it:
[log-privacy.md](../log-privacy.md).

### `save_share_export()`

Two exports in one second: the second is named with "(2)" (up to 99), as the launcher names them.

### `check_manifest()` `save_share_import()`

`check_manifest()` gives the manifest's verdict on the zip; `detail` gets its NX2Saria version and export date. In
`save_share_import()` the save's members are all checked before any is extracted.

## `runtime/hle/audren.h`

`audren:u`, the audio renderer service: the RequestUpdate wire formats, the server-side renderer state, and the mixer
interface (`runtime/audio/audio_mixer.c`: the real mixer with SDL3 output; its notes are on [audio.md](audio.md)).
The wire layouts follow the public `audren:u` wire format. Its first line is a licence notice.

- `AudrenVoiceInV2`: REV15 widens only the two filter records (Q14 -> float); the fields after them move by 24 bytes.
- `AUDREN_REV(n)` is "REVn" (0x30564552 + (n << 24)). `AUDREN_WORK_BUFFER_BYTES` is 0x10000: the host keeps its state
  in host memory.
- `AUDIO_CATCHUP_MAX_MS` (3000): catch-up after a stall (`NX2SARIA_AUDIO_CATCHUP`; `runtime/hle/audren.c`
  `render_thread()`, `runtime/audio/audio_mixer.c` `audio_mixer_owed()`): a gap longer than this is not caught up, the
  sound goes on from now.

### `ServerVoice`

The server-side voice: the DSP's view of one client voice.

- `pending[4]`: buffers sent while the voice was not playing: valid once it plays.
- `state`: `SV_*`: the client's Start, Stop and Pause, plus Stopping until the next frame.
- `flush` and `flush_from`: FlushWaveBuffers since the last frame, from the client's current buffer.
- `adpcm`: the DSP-ADPCM decode cursor (buffer slot, next sample) and history.
- `biquad_state[2][6][2]`: per filter and channel, the transposed direct-form II delays.
- `dry_pass`: `NX2SARIA_AUDIO_STATS`: 1 + the render pass in which the voice ran out of queued buffers before its end
  of stream (`runtime/hle/audren.c` `advance_voice()`), 0 otherwise.
- Volume ramps (`runtime/audio/audio_mixer.c` `audio_mixer_render()`): what the voice was mixed with in its last
  render pass. The pass after it starts every gain there and reaches the new volumes at its end; `voice_reset()` of
  `audren.c` forgets it. `ramp_pass` is 1 + the render pass the voice was last mixed in, 0 when it has none to ramp
  from; `ramp_mix` the mix and `ramp_splitter` the splitter it went to then, and `ramp_channels` its channel count: a
  pass with another of these starts at the new volumes; `ramp_buffers`, into a mix: how many of the mix's buffers
  `ramp_gain` holds; `ramp_volume`, into a splitter: the voice's volume then (the destinations keep their own);
  `ramp_gain[6][24]`, into a mix: the gain channel c went into buffer b with (NaN: nothing went in).

### Refused values

`AUDREN_R_INVALID_UPDATE` (0x5299): a RequestUpdate this host refuses (`runtime/hle/audren.c` `update_check()`,
`runtime/audio/audio_mixer.c` `audio_mixer_routing_check()` and `audio_mixer_mix_layout_check()`): audio module 153,
description 41, the update's sections do not match the renderer. The SDK's RequestUpdateAudioRenderer hands it back to
its caller (as it does descriptions 42 and 43) and aborts on any other result; the game ignores it and sends its next
update. Nothing of a refused update is applied.

The `AB_*` values are the kinds of game value this host cannot use (`runtime/hle/audren.c` `audio_bad()`), one per
site; each is logged once per renderer. Three groups: the RequestUpdate is refused (`AB_UPD_BUFFERS` to
`AB_MIX_LAYOUT`); one record of the update is left out and the rest is applied (`AB_CHANNEL_ID` to `AB_MIX_ID`); one
effect or sink is passed through or skipped (`AB_LIMITER_SIZE` to `AB_SINK_INDEX`).

- `audio_bad()`: a value the game sent that this host cannot use: the caller refuses the update or leaves the one
  record, effect or sink out. It logs the first of each kind per renderer, with the game frame, and counts every one;
  with `NX2SARIA_AUDIO_STRICT=1` it ends the process instead, as most of these values did before 0.11.0. Any thread;
  it takes no lock but the log's.
- `AUDIO_REFUSE()` is `audio_bad()` for a refused RequestUpdate, as an expression that gives
  `AUDREN_R_INVALID_UPDATE`.

### `AudioRenderer`

- `state` is 0 started, 1 stopped; `event` the writable handle.
- `stats`: `NX2SARIA_AUDIO_STATS` (`audren.c`), NULL when off.
- `strict`: `NX2SARIA_AUDIO_STRICT` (OpenAudioRenderer): `audio_bad()` ends the process.
- `bad_seen[AB_KINDS]`: the kinds `audio_bad()` logged so far (atomic exchange; any thread). `bad_count`: the times a
  value was left out (per update or per render pass) or an update refused (atomic).
- `refused` and `refused_run`: updates refused, and refused in a row (the thread that sends RequestUpdate).

### The mixer interface

`audren_manager_handler()` is IAudioRendererManager, served under "audren:u" (the service table of
`runtime/hle/ipc.c`). `audren_renderers_live()` counts the renderers whose render tick runs.

Every call of the mixer interface (`runtime/audio/audio_mixer.c`) runs with `r->lock` held except open, close and
set_running, the render thread's `audio_mixer_pace_ns()`, `audio_mixer_owed()` and `audio_mixer_out_stats()`
(`runtime/hle/audren.c` `render_thread()` and `stats_report()` call them without it), and
`audio_mixer_routing_check()`. The render thread's three touch only atomics and state that only the render thread
changes: `device_check` and `channels_switch`, which close and open the output stream, run on that thread too, and
`audio_mixer_close()` runs after `renderer_close()` has joined it. `audio_mixer_routing_check()` runs on the game's
update thread before it takes the lock and reads only the renderer's fixed counts and the game's input.

- `audio_mixer_routing_check()`: before an update is applied, without the renderer lock: the framing of the splitter
  and mix sections in the layout of `revision`. 0, or `AUDREN_R_INVALID_UPDATE` (logged through `audio_bad()`).
- `audio_mixer_mix_layout_check()`: renderer lock held, before the update is applied: the mixes in use, as the update
  leaves them, fit the renderer's mix buffers (`audio_mixer_render()` lays them out one after another). 0, or
  `AUDREN_R_INVALID_UPDATE`.
- `audio_mixer_update_routing()`: after both checks passed. A record whose ids the renderer does not have is left out
  (`audio_bad()`).
- `audio_mixer_update_effects()`: an enabled effect whose parameters the DSP cannot use is passed through by
  `audio_mixer_render()` (`audio_bad()`).
- `audio_mixer_render()`: after `voices_prepare()` of `audren.c`, before voices advance. `drop`: a catch-up frame
  (`render_thread()`). It is mixed in full, so the voices' filters, the effects and the sinks the game reads back
  advance as for any frame, and nothing of it is queued for the device.
- `audio_mixer_pace_ns()`: device pacing for the render thread at `kern_now_ns` `now`: 0 renders the next frame now, a
  positive value waits that many ns (the ring is at its target), -1 leaves the pace to the render thread's timer (no
  device, renderer stopped, device stalled).
- `audio_mixer_owed()`: catch-up for the frames the device paces (render thread, lock not held). 1: the device has
  played at least one render frame of silence for want of rendered sound that is not made up for yet; that frame is
  taken off what is owed and the caller renders one frame with `drop` set. 0: nothing is owed. -1: more than
  `AUDIO_CATCHUP_MAX_MS` was owed and is forgotten.
- `audio_mixer_out_stats()` (`AudioOutStats`): `NX2SARIA_AUDIO_STATS`: the output's counters since it opened, and the
  longest gap between two device requests since the last call (which starts that measure again). False without a
  device.
- `audio_mixer_ramp_stats()`: `NX2SARIA_AUDIO_STATS`: the render passes in which at least one volume was ramped
  (`NX2SARIA_AUDIO_RAMP`), and the ramps (one source channel into one mix buffer), since the mixer opened.
- `audio_voice_consumed()`: the source samples over one frame; it advances `v->fraction`.
- `audio_buffer_empty()`: no samples to decode: played as soon as it is reached.
- `audio_device_handler()` is IAudioDevice. `audio_channels_live()`: after a settings change, another
  `NX2SARIA_AUDIO_CHANNELS` reaches the game and the mixer. `audio_dmg_handler()` is `auddmg:u` and
  `audio_sysmix_handler()` `audsmx:u` (both Switch 2).

## `runtime/hle/audren.c`

`audren:u`. The SDK's `nn::audio` client builds RequestUpdate input unchanged; this service validates every section
against the header, keeps server-side voice state, advances it in real time (one frame of sample_count / sample_rate
seconds per render tick, signalling the renderer's event) and writes the output status the client expects. The mixer
behind it is `runtime/audio/audio_mixer.c`. The file's first two lines are a licence notice.

A value in the game's RequestUpdate is not a reason to end the process (`audio_bad()`): an update whose sections do
not match the renderer is refused whole with `AUDREN_R_INVALID_UPDATE`, and a record, effect or sink whose values the
renderer cannot use is left out of an update that is otherwise applied. `NX2SARIA_AUDIO_STRICT=1` ends the process
there instead. OpenAudioRenderer's parameters (`need()`) and the request's buffer descriptors (`ipc_guest` of
`runtime/hle/ipc.c`) still end it.

`gfx_counters.frames` is the game frame a stats line names, to match the frametime lines.

### The statistics

`NX2SARIA_AUDIO_STATS` (default 0; 1 on): where a crackle comes from. A gap in the sound has one of two sources here.
The output device took a period and the ring held less (an underrun: silence until the prefill is queued again),
because the render thread came late: a long gap between two of its passes, or a long wait for the renderer lock, which
the game's audio thread holds while its RequestUpdate is read. Or the game's audio thread came late: on the console it
sends RequestUpdate after each render pass and keeps every streamed voice's queue filled; a voice whose queue runs out
before its end of stream ("dry") plays silence until its next buffer arrives ("late", counted when that comes within
`AS_LATE_PASSES` passes, by how many passes it missed). One line per `AS_WINDOW_NS` (10 s) in which something went
wrong (and once a minute regardless), plus a line for each of the first `AS_EVENTS` underruns and late game updates,
with the game frame (the frame numbers of the frametime lines).

- `AS_LATE_PASSES` (40) is 200 ms: a buffer later than that is taken for a new sound, not a late one.
- `AS_GAP_NS` (15 ms): a render pass more than this after the one before (a 10 ms device period + 5).
- `AS_LOCK_NS` (2 ms): a wait for the renderer lock longer than this.
- `AS_UPDATE_NS` (20 ms): a game update more than this after the one before (four passes). `AS_UPDATE_EVENT_NS` is
  30 ms.
- `AudrenStats`, by who writes it. The render thread (under the lock): passes, gaps between them, lock waits, the
  ring's level when a pass began; `out_underruns`, `out_missing`, `out_skipped` and `out_overruns` are the output's
  counters at the window's start; `seen_underruns` the last count an event line was written for. The game's
  RequestUpdate (under the lock): the `upd_*` fields. Voices (under the lock): `dry`, and `late[3]` for a next buffer
  late by 1-2 passes, 3-8, 9-`AS_LATE_PASSES`; `ramp_passes` and `ramps` are the mixer's volume ramp counts at the
  window's start (`audio_mixer_ramp_stats`). Catch-up (`render_thread()`, under the lock): `caught_up` frames rendered
  and dropped, `gave_up` gaps over `AUDIO_CATCHUP_MAX_MS`, which are left as they are. `voices_ns` is the last voice
  census line (the game's update, under the lock); `voices_most` the most voices in use a census found.

`g_game_prio` is `NX2SARIA_AUDIO_GAME_PRIO` (default 0): the host priority of the game's thread that sends
RequestUpdate. 0 leaves it alone. 1 raises it to the highest while the update is read, so the render thread (highest
priority) never waits for the renderer lock behind a game thread the host has preempted for busier threads. 2 also
keeps that thread above normal from its first update on, so it is not queued behind the shader and pipeline workers
when it wakes for the next render pass.

### `audio_bad()`

Without strict, a kind already seen returns before anything is formatted: it formats only once. In strict mode
`ipc_backtrace` prints the game's frame chain when the game's update thread is here; nothing on the render thread.

### `need()`

For OpenAudioRenderer's and GetWorkBufferSize's parameters, and host allocations: once per renderer, the SDK's fixed
values, not the game's updates.

### `renderer_new()`

A new renderer is stopped (state 1).

### `take()`

The next `n` bytes, or NULL when they run past the header's total (`update_check()` refuses the update).

### `voice_reset()`

`ramp_pass` goes to 0: a new voice starts at its volumes, with no ramp from what the slot's last voice was mixed with.

### `update_voice()`

`request_update()` has checked the id against the voice count and, for a voice in use, its wave buffer index. The play
state is kept as the DSP keeps it: Stop on a voice that is not stopped becomes Stopping, which the next render frame
turns into Stopped after giving back every queued buffer. A new buffer waits until a render frame finds the voice
playing.

### `audio_buffer_empty()` `skip_empty()`

No samples (no memory, no size or an empty range): the DSP counts the buffer as played when it reaches it, so a
stream's empty last buffer does not hold up the buffers queued behind it. In `skip_empty()` an endless empty loop
starves the voice, as on the DSP.

### `voices_prepare()`

Each render frame, before mixing: flush, stop, start. FlushWaveBuffers: from the client's current buffer, the queued
ones count as played. Stopping: every queued buffer counts as played; unsent ones are dropped. With
`NX2SARIA_AUDIO_STATS`, the buffer a dry voice waited for is counted by the passes it missed.

### `output_header()`

The output header of an update in the layout of `revision`, section by section: the SDK reads every section back and
aborts when a size differs from the one it expects. The effect output record is 0x90 bytes from REV9, 0x10 before
(`effect_out_bytes()`).

### `update_check()`

`UpdateIn` is an update's sections as `update_check()` found them, and the output header it checked against the
output buffer.

Every check of an update's framing, before anything of it is applied, in the order the sections come; without the
renderer lock (it reads the renderer's fixed counts and the game's input). The SDK builds each section from the
renderer's own counts, so a mismatch means this host reads another layout than the one the game wrote (a revision),
not a value the game chose. 0, or `AUDREN_R_INVALID_UPDATE` after one `audio_bad()` line. `TAKE()` is the next
section, or the update refused when it runs past the header's total.

### `note_refused()`

A refused update, on the thread that sends RequestUpdate only (the SDK sends from one; a second sender would only race
these diagnostic counts). The game ignores the result and goes on; while refusals go on, its sounds stop following it.

### `request_update()`

0, or `AUDREN_R_INVALID_UPDATE` with nothing of the update applied and nothing written to `output`.

- The first update's sizes are logged, also when it is refused.
- `audio_mixer_mix_layout_check` is the one check that needs the mixes as they are: under the lock, before the first
  write.
- From there on a value the renderer cannot use leaves its one record out (`audio_bad()`); the rest is applied.
- An unused voice slot can have a zero id: its own voice is released and voice zero is left playing.
- The output: `update_check()` has checked its size against the output buffer. EffectStatus is Enabled 3 or Disabled
  4. The performance section (0x10) has nothing written; the behavior section (`AUDREN_BEHAVIOR_OUT`, 0xb0) reports
  no errors.
- With `NX2SARIA_AUDIO_STATS`: the game's cadence and how long its update held the lock. The voice census, once per
  window: the voices the game holds. Its sound engine takes one per sound from a budget of the renderer's voice count,
  so sounds that are never let go show as a count that climbs from area to area until new sounds are refused.

### `advance_voice()`

The render tick: one frame each time the output device has taken the ring below its target (`audio_mixer_pace_ns`), or
every sample_count / sample_rate seconds of the host clock without a device, while stopped or while it stalls; after a
stall the frames that are owed are rendered and dropped (`render_thread()`, catch-up).

An endless empty loop `skip_empty()` kept starves the voice, and its zero length would make the loop go round for
good, so the walk stops there. With `NX2SARIA_AUDIO_STATS` a voice is marked dry when the queue ran out within this
pass, not at an end of stream (which zeroes `played`).

### `stats_pass()` `stats_report()`

`stats_pass()`: `NX2SARIA_AUDIO_STATS`, render thread: the pass that begins at `now` (lock held after waiting
`lock_ns` for it). `paced`: the device paces this pass (the timer's passes, without a device or in a stall, keep their
own cadence). `stats_report()`: render thread after a pass, lock not held: the event lines and the window line. When
the window is reset `upd_last_ns` and `upd_pass_last` stay: the next gap is measured from the last update.

### `render_thread()`

**The gap between device-paced frames.** When the device pulls a period (1024 samples on PipeWire, 4-5 frames) the
ring drops below its target and the frames would otherwise render back to back. On hardware the DSP renders one frame
every 5 ms and the game's audio thread works in between; AUX effects need that time to fill the return ring, or
`mix_aux` reads stale samples and the frame clicks at its seams. Device-paced frames stay at least 4/5 of a frame
apart, which still refills a period well before the device drains it. Linux only: on Windows SDL pulls 480-sample
periods, and a sink dump (`NX2SARIA_AUDIO_DUMP`) of the bursts there showed no more seam spikes than chance, so a wait
on this thread buys nothing.

**Catch-up** (`NX2SARIA_AUDIO_CATCHUP`, default 1). The console's audio hardware takes a frame every 5 ms whatever the
rest of the system does, so the game's sound keeps its place against the picture. Here a frame that is not rendered
when it is due is time the sound loses for good: the device plays silence meanwhile, rendering goes on where it
stopped, and everything after the gap is heard that much later than the picture it belongs to. So after a stall the
frames that are owed are rendered and dropped: mixed in full, which moves the voices, the effects and what the game
reads back on as if they had been heard, and not queued for the device, whose queue is then filled as before. While
the device paces, it says what is owed: the silence it played for want of frames (`audio_mixer_owed`), asked before
anything is waited for or queued, so the dropped frames come right after the gap. On the timer a frame is owed when
its deadline passed a whole frame ago. A gap of more than `AUDIO_CATCHUP_MAX_MS` (a debugger, the PC asleep) is not
caught up: the sound goes on from now. With the setting off nothing is dropped, and the timer queues frames up to
100 ms late back to back and skips a longer gap.

While the device paces, `next` is kept at now + one frame: where the timer goes on if the device stops pacing. On the
timer, a thread that fell far behind resynchronises.

### `renderer_handler()`

IAudioRenderer:

| Command | Call |
|---|---|
| 0 | GetSampleRate |
| 1 | GetSampleCount |
| 2 | GetMixBufferCount |
| 3 | GetState |
| 4, 10 | RequestUpdate and RequestUpdateAuto: out(output), out(performance), in(input) |
| 5 | Start |
| 6 | Stop |
| 7 | QuerySystemEvent |
| 8 | SetRenderingTimeLimit |
| 9 | GetRenderingTimeLimit (100 when never set) |
| 12, 13 | SetVoiceDropParameter and GetVoiceDropParameter |

- RequestUpdate raises the thread's priority around the update when `NX2SARIA_AUDIO_GAME_PRIO` is set. For command 10
  the C buffers get their reply entries only when the update was not refused: the SDK reads no output of a refusal.
- Set/GetVoiceDropParameter are 15.0.0+ (the system's audio renderer service): the factor the DSP uses to drop voices
  when a frame overruns its time. The host mixer renders every voice, so the value is only kept (`voice_drop_bits`,
  the float 1.0 until set).

### `audren_manager_handler()`

IAudioRendererManager:

| Command | Call |
|---|---|
| 0 | OpenAudioRenderer(param, pad, work size, aruid; copy tmem, process; pid) |
| 1 | GetWorkBufferSize(param) -> u64 |
| 2, 4 | GetAudioDeviceService(aruid), GetAudioDeviceServiceWithRevisionInfo(revision, aruid) |

OpenAudioRenderer takes a copy of the transfer memory made under the kernel lock: a CloseHandle of the transfer memory
on another thread frees the object.
