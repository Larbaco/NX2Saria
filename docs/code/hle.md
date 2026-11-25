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
| 29, 30 | 0 | a region the SDK records at `nnosInitialize`: its address (29), then what id 30 asks of it |
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

### `quit_menu_overlay()`

The labels of Quit Game in the game's own menus (`runtime/ui/quit_menu_text.c`, [quit-menu.md](../quit-menu.md)): the
message archive of the game's language (`Mals/<region>.Product.<n>.sarc.zs`, the region from
`runtime/ui/ui_text.c` `ui_text_game_lang_mals()` for `ov_language()`; every message archive when the language has no
region in that list) is served with the labels added, written to `<profile>/quit-menu/`. The base is the file the game
would read (a prompts folder's or a mod's, else the RomFS's). The archive grows, so its resource size is raised
(`restbl_room()`); an archive that cannot be raised, written or edited counts as not made, and
`runtime/ui/quit_menu.c` `quit_menu_texts()` turns the feature off unless all were made. One language's archive takes
about 0.1 s; the sixteen of the game took 1.1 s, most of it the size table packed again for each.

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

## `runtime/hle/ov.h`

Shared by the system overrides, `runtime/hle/ov_system.c`, `runtime/hle/ov_input.c`, `runtime/hle/ov_media.c` and
`runtime/hle/ov_gfx.c`, and their probe. `oe_push_message`, `oe_set_focus` and `ov_nv_state` are in
`runtime/gfx/gfx.h` (the window posts focus messages, the NVN layer allocates through the recorded callbacks).

- `ov_operation_mode_live()`: a changed `NX2SARIA_OPERATION_MODE` goes to the game.
- `ov_handheld()`: `NX2SARIA_OPERATION_MODE=handheld` (default docked); fatal on another value.
- `ov_language()`: the game's language, a Switch language code ("en-US", "zh-Hans", ...).
- `ov_shared_font_dir()`: `NX2SARIA_SHARED_FONTS`, else `<romfs>/../../firmware/shared-fonts`.
- `ov_bfttf_size()`: the decoded font size of a BFTTF image, 0 if it is not one.

## `runtime/hle/ov_system.c`

System service overrides: `nn::oe`, applet, am, prepo, nfp, err, aoc, pctl, diag, account, time and pl, and the six
observers. Nothing here talks to a Horizon service; each call is answered by the host. The stub rule: queries and
initialisation succeed with neutral values, data paths stop with a named fatal. Its first line is a licence notice.

Environment:

| Setting | What |
|---|---|
| `NX2SARIA_LANG=<Switch language code>` | the game's language as given ("ja", "en-GB", "zh-Hant", ...) |
| `NX2SARIA_LOCALE=<BCP 47 tag>` | else the language for this locale ("pt-BR"), else the host's user locale; a language the game does not ship becomes en-US |
| `NX2SARIA_OPERATION_MODE=docked\|handheld` | operation and performance mode reported to the game (default docked) |
| `NX2SARIA_ASSUME_FOCUS=1\|0` | 1 (default, the Game tab's row): GetCurrentFocusState always reports in focus, whatever the window says; 0: out of focus while the window is, as on a Switch |
| `NX2SARIA_SHARED_FONTS=<dir>` | `nn::pl` shared fonts (`tools/extract_fonts.py` output), default `<romfs>/../../firmware/shared-fonts` |

Results: `NFP_R_DEVICE_NOT_FOUND` (0x8073) is nfp's "no NFC reader on the host"; `ACCOUNT_R_INVALID` (0x7c67) the
result for an unknown user handle or uid.

### `ov_handheld()` `ov_operation_mode_live()`

`NX2SARIA_OPERATION_MODE`: 1 handheld (OperationMode 0, PerformanceMode Normal 0), 0 docked (1, Boost 1). It follows
the setting; the game is told of a change as the console tells it (`ov_operation_mode_live()`).
`ov_operation_mode_live()` runs after a settings change (`window_settings_live` of `runtime/gfx/gfx_present.c`): a new
mode reaches the game through the applet messages OperationModeChanged and PerformanceModeChanged, which make it read
the mode again.

### `switch_language()` `ov_language()`

`switch_language()`: the Switch language for a BCP 47 tag; en-US for a language the game does not ship. A three-digit
region is a UN M.49 area (es-419). Brazil's Portuguese is a language of its own on the Switch (pt-BR, Mals/USpt);
Portugal's is pt.

`ov_language()`: the game's language as a Switch language code (`NX2SARIA_LANG`, else `NX2SARIA_LOCALE`, else the
host's user locale, `plat_user_locale`). The game accepts "zh-Hans" and "zh-Hant" for Chinese and falls back to
Japanese for "zh-CN" and "zh-TW", so those two spellings, which older `nx2navi.ini` files and menus wrote, are mapped
to them. `NX2SARIA_LANG` has no fixed default: `NX2SARIA_LOCALE` or the user locale decides when it is unset.

### `ov_desired_language()`

`LanguageCode GetDesiredLanguage()`: a char[8] in x0.

### `ov_oe_initialize()`

`nn::oe::Initialize` opens the `appletOE` service (served by `runtime/hle/ipc.c`, which supports domains) and builds
the module's own service object, which the oe wrappers the game calls internally dereference. It must run: it is
observed (`HOST_OBSERVER`), and the guest definition is called instead of being replaced.

### `ov_oe_graphics_scale()` `ov_oe_scaling_parameters()`

`void SetGraphicsScaleChangedNotificationEnabled(bool)`: a thin wrapper over the appletOE service object; the
notification is a no-op on a desktop window.

`GraphicsScalingParameters GetGraphicsScalingParameters()`: 12 bytes returned in x0 (words 0-1) and w1 (word 2). Word
0 is a scale *mode*, not a float: the SDK's own caller (`sdk+0x59a690`) maps 10 -> 0, 20 -> 1, 30 -> 2 and aborts on
anything else (an earlier {1.0f, 1.0f} here hit that abort). 10 is the console's default (the graphics scale
controller's current scale); the other two words are zero.

### `ov_performance_config()` `ov_cpu_boost()` `ov_focus_mode()`

`void SetPerformanceConfiguration(PerformanceMode mode, int config)`; `void SetCpuBoostMode(CpuBoostMode mode)`;
`void SetFocusHandlingMode(FocusHandlingMode mode)`, which is the nnMain checkpoint of `--boot`.

### `oe_set_focus()` `ov_focus_state()`

`FocusState GetCurrentFocusState()`: 1 in focus, 2 out of focus (`nn::oe::FocusState`). The game sets focus handling
mode 2: on a Switch the system then suspends the whole process while it is out of focus and sends Resume when it
returns. Here only the game's main thread stops (it waits in PopNotificationMessage for the next message) while its
loader threads, audio and clocks run on, and a load interrupted that way was seen to never finish (Linux, 1.4.3). So
`NX2SARIA_ASSUME_FOCUS=1`, the default, keeps it at 1; with 0 it follows the window through `oe_set_focus()`. The
caller of `oe_set_focus()` also pushes `OE_MSG_FOCUS_STATE_CHANGED`.

The setting is read on every focus change rather than once at start: a focus change is rare, the lookup is a short
walk of the loaded ini entries, and the Game tab's row then takes effect the next time the window is left, without a
restart.

### The oe, applet, am, prepo and err calls

| Function | SDK call | Note |
|---|---|---|
| `ov_get_balance()` | `void GetExpectedVolumeBalance(float *left, float *right)` | |
| `ov_set_balance()` | `void SetExpectedVolumeBalance(float left, float right)` | |
| `ov_active_time()` | `TimeSpan GetProgramTotalActiveTime()` | nanoseconds since the process started (19.2 MHz ticks * 625 / 12) |
| `ov_inactivity_extended()` | `void SetUserInactivityDetectionTimeExtended[Unsafe](bool)` | the host never puts the console to sleep |
| `ov_finish_logo()` | `void FinishStartupLogo()` | no system logo is drawn on the host |
| `ov_launch_parameter()` | `bool TryPopLaunchParameter(size_t *size, void *buffer, size_t capacity)` | launched directly, no parameter |
| `ov_try_pop()` | `bool TryPopNotificationMessage(Message *out)` | |
| `ov_pop()` | `Message PopNotificationMessage()` | blocks like a Horizon wait, so the thread gives its virtual core up meanwhile |
| `ov_previous_program()` | `int applet::GetPreviousProgramIndex()` | started directly, not from an ExecuteProgram chain: -1 |
| `ov_aruid()` | `AppletResourceUserId applet::` / `am::GetAppletResourceUserId()` | one application, one stable non-zero id (1) |
| `ov_report_save()` | `Result PlayReport::Save()` and `Save(const account::Uid &)` | `nn::prepo`: play reports stay local (no Nintendo servers) |
| `ov_result_ok()` | RequestImmediateTransmission, AddInternetConnectionStatus | |
| `ov_report_event()` | `Result AsyncContext::GetCompletionEvent(SystemEvent *out)` | Switch 2: `Result PlayReport::Send(const account::Uid &)` replaces Save (kept local like it); `out` becomes a signalled event; `AsyncContext::GetResult()` |
| `ov_report_limit_event()` | `SystemEvent *GetOverSaveFrequencyLimitEvent()` | an event that never fires (no save limit here); the static `ev[0x40]` is an `nn::os::SystemEventType` |
| `ov_show_error()` | `void err::ShowError(Result)` | the game's error dialog, logged |

### The nfp calls

`nn::nfp`: NFC reader and amiibo support. Every `nn::nfp` import of the game has an override here, so the SDK's own
nfp code (whose session Initialize never opens) is not reached. `runtime/hle/amiibo.c` keeps the figure's data and
application area. `nfp_device` ({1, 0, 0, 0, 0, 0, 0, 0}) is the DeviceHandle of the one emulated reader. The feature:
[amiibo.md](../amiibo.md).

| Function | SDK call |
|---|---|
| `ov_nfp_list()` | `Result ListDevices(DeviceHandle *out, int *count, int max)` |
| `ov_nfp_availability()` | `Result AttachAvailabilityChangeEvent(os::SystemEventType *event)` |
| `ov_nfp_device_state()` | `DeviceState GetDeviceState(const DeviceHandle &)` |
| `ov_nfp_mount()` | `Result Mount(const DeviceHandle &, ModelType, MountTarget)` |
| `ov_nfp_get_npad_id()` | `Result GetNpadId(u32 *npad_id, const DeviceHandle &)`: 0, Handheld / Player 1 |
| `ov_nfp_attach_activate()` | `Result AttachActivateEvent(os::SystemEventType *event, const DeviceHandle &)` |
| `ov_nfp_attach_deactivate()` | `Result AttachDeactivateEvent(os::SystemEventType *event, const DeviceHandle &)` |
| `ov_nfp_start_detection()` | `Result StartDetection(const DeviceHandle &)` |
| `ov_nfp_stop_detection()` | `Result StopDetection(const DeviceHandle &)` |
| `ov_nfp_get_tag_info()` | `Result GetTagInfo(TagInfo *out, const DeviceHandle &)` |
| `ov_nfp_get_model_info()` | `Result GetModelInfo(ModelInfo *out, const DeviceHandle &)` |
| `ov_nfp_get_register_info()` | `Result GetRegisterInfo(RegisterInfo *out, const DeviceHandle &)` |
| `ov_nfp_get_common_info()` | `Result GetCommonInfo(CommonInfo *out, const DeviceHandle &)` |
| `ov_nfp_open_app_area()` | `Result OpenApplicationArea(const DeviceHandle &, u32 access_id)` |
| `ov_nfp_get_app_area()` | `Result GetApplicationArea(void *out, size_t *out_size, const DeviceHandle &, size_t size)` |
| `ov_nfp_set_app_area()` | `Result SetApplicationArea(const DeviceHandle &, const void *data, size_t size)` |
| `ov_nfp_flush()` | `Result Flush(const DeviceHandle &)` |
| `nfp_create_app_area()` | `Result CreateApplicationArea` / `RecreateApplicationArea(const DeviceHandle &, const ApplicationAreaCreateInfo &)`. ApplicationAreaCreateInfo: u32 access id at 0, u32 initial data size at 4, u64 initial data address at 8 (size and address 0 without initial data) |

The amiibo settings applets have no window: each gives the answer the applet gives when the player confirms at once,
with the reader's DeviceHandle in the first argument.

- `ov_nfp_owner_settings()`: `Result StartNicknameAndOwnerSettings(DeviceHandle *out, bool *registered, RegisterInfo
  *out_info, const AmiiboSettingsStartParam &, const TagInfo &[, const RegisterInfo &])`.
- `ov_nfp_restorer()`: `Result StartRestorer(DeviceHandle *out, const AmiiboSettingsStartParam &, const TagInfo &)`:
  a figure here is never damaged, so there is nothing to restore.
- `ov_nfp_game_data_eraser()`: `Result StartGameDataEraser(DeviceHandle *out, const AmiiboSettingsStartParam &, const
  TagInfo &)`: drops the loaded figure's application area.

### `ov_aoc_list()` `ov_aoc_count()`

SDK-internal initialisation. 1.4.3's main lists add-on content: there is none
(`int ListAddOnContent(AddOnContentIndex *out, int offset, int count)`). `int CountAddOnContent()`: the SDK's own reads
the aoc service object the overridden Initialize never opens.

### `ov_nifm_initialize()` `ov_nifm_available()` `ov_nifm_request()` `ov_nifm_error()`

`nn::nifm`: this build is offline. Initialize succeeds without the `nifm:u` session, the network is never available, a
request completes at once (unaccepted), and the calls that report its error return NetworkCommunicationDisabled (nifm
module 110, description 1111: `NIFM_R_COMMUNICATION_DISABLED`, 0x8ae6e).

### `observe()` `obs_address_region()` `obs_memory_pages()`

The observers log, then run the guest definition. `obs_address_region()`:
`Result os::AllocateAddressRegion(uintptr_t *out, size_t size)`; its line is logged only with the host-call trace
(20,000 lines a session otherwise, each a write on a game thread). `obs_memory_pages()`:
`Result os::AllocateMemoryPages(uintptr_t address, size_t size)`.

### The account

`nn::account`: one offline local user whose Uid survives restarts in `<profile>/account.uid`.

- `adopt_cb()` and `adopt_save_uid()` (holding `account_lock`): 1 when `<profile>/save` holds exactly one folder named
  as `runtime/hle/fs_host.c` names an account's (<uid hi><uid lo>, 32 hex digits); `local` is then that account.
- `load_uid()` (holding `account_lock`): a file that is there but cannot be asked for its size is no reason for a new
  account: its saves would vanish, so that is fatal. With `account.uid` lost (a profile copied without it, or the save
  folder alone put in place) the one account folder in save/ is the player's, and taking its name keeps that save;
  otherwise a new account is made.
- `account_uid()` (`runtime/host.h`): the save directory of `fs_host.c` is named after this Uid.
- `valid_handle()` holds `account_lock`.

| Function | SDK call |
|---|---|
| `ov_account_initialize()` | `void Initialize()` |
| `ov_open_user()` | `bool TryOpenPreselectedUser(UserHandle *out)` |
| `ov_close_user()` | `void CloseUser(const UserHandle &)` |
| `ov_user_id()` | `Result GetUserId(Uid *out, const UserHandle &)` |
| `ov_user_existence()` | `Result GetUserExistence(bool *out, const Uid &)` |
| `ov_nickname()` | `Result GetNickname(Nickname *out, const Uid &)`: 33 bytes, NUL-terminated UTF-8 |

### The clocks

`nn::time`: the host clock and time zone. User and network clocks follow the host wall clock; the steady clock counts
seconds of the host's millisecond clock (`plat_now_ms`), with one random source id per process. ClockSnapshot has the
public 0xD0-byte layout, because game code reads its fields through inline accessors.

- `make_source()` reads the time zone once (`plat_tz_west`), before the first localtime.
- `days_from_civil()` and `civil_from_days()`: days since 1970-01-01 of a proleptic Gregorian date, and back (H.
  Hinnant's civil calendar algorithms).
- `utc_calendar()`: the UTC calendar fields of a POSIX time. 0 if the year does not fit CalendarTime. 1970-01-01 was a
  Thursday.
- `local_calendar()`: the local calendar fields of a POSIX time in the host's time zone. The zone name is "UTC" or the
  UTC offset ("+03", "-0330"), as tzdata names zones without an abbreviation; Windows has no abbreviations. 0 outside
  localtime's range.
- `location_name()`: the IANA name of the standard offset: Etc/GMT+5 is UTC-5 (POSIX sign); "UTC" for a zone off whole
  hours.

| Function | SDK call | Note |
|---|---|---|
| `ov_current_time()` | `Result StandardUserSystemClock` / `StandardNetworkSystemClock::GetCurrentTime(PosixTime *out)` | |
| `ov_steady_now()` | `time_point StandardSteadyClock::now()` | the int64 seconds in x0 only. The SDK caller (`sdk+0x3d1d88`) reads x0; returning the 0x18-byte struct through x8 would write through a stale pointer |
| `ov_steady_timespec()`, `ov_user_timespec()` | `Result GetStandardSteadyClockCurrentTimespec(timespec *)` / `GetStandardUserSystemClockCurrentTimespec(timespec *)` | the SDK libc's `clock_gettime(CLOCK_MONOTONIC / CLOCK_REALTIME)` once a title selects the `nn::time` clocks (`sdk+0xaf0f8` mode 3; Switch 2 titles do, via `std::chrono::steady_clock`). The SDK's own versions need the time service state the overridden Initialize never builds, fail, and libc++ then throws `std::system_error` |
| `ov_to_calendar()` | `Result ToCalendarTime(CalendarTime *out, CalendarAdditionalInfo *info, const PosixTime &t)` | local time |
| `ov_to_calendar_utc()` | `CalendarTime ToCalendarTimeInUtc(const PosixTime &t)` | 8 bytes in x0 |
| `ov_to_posix_utc()` | `PosixTime ToPosixTimeFromUtc(const CalendarTime &c)` | months out of 1..12 carry into the year, like timegm |
| `ov_snapshot_init()` | `ClockSnapshot::ClockSnapshot()` | |
| `ov_snapshot_create()` | `static Result ClockSnapshot::CreateWithStandardSystemClock(ClockSnapshot *out)` | |
| `ov_span_between()` | `Result CalculateSpanBetween(TimeSpan *out, const ClockSnapshot &a, const ClockSnapshot &b)` | nanoseconds |
| `ov_user_difference()` | `TimeSpan CalculateStandardUserSystemClockDifferenceByUser(const ClockSnapshot &a, const ClockSnapshot &b)` | the change of the user clock offset between the snapshots (manual adjustments) in nanoseconds |

### The shared fonts

`nn::pl` shared fonts. The game asks for them only from its font loader, only with Simplified Chinese, Korean or
Traditional Chinese text, and only for type 0; `tools/extract_fonts.py` writes them from the user's firmware. Each type
(`SHARED_FONT_TYPES`, 6) is read on first use into guest-visible memory as the BFTTF file the pl shared memory holds;
Address is 8 bytes past the file start (after the BFTTF header) and Size the decoded font size. The load state is
always Loaded: the game spins until it is, so missing files stop with a named fatal at Address/Size instead of a hang.
`NX2SARIA_SHARED_FONTS=<dir>` overrides `<romfs>/../../firmware/shared-fonts`.

- `ov_shared_font_dir()` looks for shared-fonts next to the executable (`tools/package_build.py`), else the
  repository's.
- `ov_bfttf_size()`: the decoded font size of a BFTTF image, 0 if it is not one: big-endian words XORed with key =
  word0 ^ `BFTTF_MAGIC`, word1 = font size, then an sfnt (TrueType or OpenType) font.
- `font_data()`: the font of type t, read and checked on first use; a named fatal when it is missing or damaged.

## `runtime/hle/ov_input.c`

`nn::hid` overrides: one connected controller fed by `runtime/input/input.c` `input_poll()` (gamepad and keyboard;
nothing pressed with `NX2SARIA_INPUT=none`). Docked (default) it is a full-key controller as player 1 (id 0, style bit
0); with `NX2SARIA_OPERATION_MODE=handheld` it is the handheld controller (id 0x20, style bit 1) and player 1 is not
connected, which is the controller the game reads in that mode. Mouse and keyboard report no input. The controller has
two vibration devices (left, right), played on the gamepad's motors or, with `NX2SARIA_RUMBLE_HD=1`, as they are on a
Switch controller's linear actuators (`NX2SARIA_RUMBLE=0`: no devices), and a six-axis sensor fed by the gamepad's gyro
and accelerometer (`NX2SARIA_MOTION=0`, live: reads at rest). State layouts follow the HID shared-memory format. Its
first line is a licence notice. The player's side: [controls.md](../controls.md), [gyro.md](../gyro.md).

### The npad calls

| Function | SDK call | Note |
|---|---|---|
| `ov_initialize_npad()` | `void InitializeNpad()` | |
| `ov_supported_ids()` | `void SetSupportedNpadIdType(const NpadIdType *ids, size_t count)` | |
| `ov_supported_styles()` | `void SetSupportedNpadStyleSet(NpadStyleSet styles)` | |
| `ov_style_set()` | `NpadStyleSet GetNpadStyleSet(const NpadIdType &id)` | |
| `npad_states()` | `int GetNpadStates(<State> *out, int count, const NpadIdType &id)` | the newest state only. `want`: the style of the overload, 0 for the Joy-Con overloads (never connected) |
| `ov_hold_type()` | `void SetNpadJoyHoldType(NpadJoyHoldType type)` | |
| `ov_disconnect()` | `void DisconnectNpad(const NpadIdType &id)` | only the controller in use (handheld mode disconnects player 1) |
| `ov_controller_support()` | `Result ShowControllerSupport(ControllerSupportResultInfo *out, const ControllerSupportArg &arg)` | the one controller is already connected, so the applet returns at once with one player on it |
| `ov_mouse_states()` | `int GetMouseStates(MouseState *out, int count)` | a mouse that is not connected |
| `ov_keyboard_state()` | `void GetKeyboardState(KeyboardState *out)` | a keyboard with no key down |

### `npad_states()`

A frame's first pad read is where a driver's low-latency sleep belongs (`runtime/pacing/vk_lowlat.c`
`vk_lowlat_frame_start()`): the wait comes before the input is sampled, so the frame is drawn from the newest input.
Nothing happens while that is off. After the state is read:

- `boot_map_tick`: a boot map ends once Link is in the world (`runtime/patches/boot_map.c`);
- `dbg_tick`: the debug menu's changes to the game, between two of its frames (`runtime/diag/debug_tools.c`);
  `dbg_pad`: its free camera takes the sticks while it flies;
- `sprint_pad`: the sprint button's presses, kept from the game during gameplay (`runtime/patches/sprint.c`);
- `text_speed_poll`: the `text_speed` setting onto the patch page (`runtime/patches/text_speed.c`);
- `amiibo_daily_poll`: the `amiibo_daily_limit` setting onto its patch set (`runtime/patches/amiibo_daily.c`).

### Vibration and the six-axis sensor

Handles are the SDK's (`nn::hid::utility`, `sdk+0x4a6e00` and `sdk+0x4a6ab0`): byte 0 the style index (3 full-key, 4
handheld), byte 1 the controller id, byte 2 the device (vibration 0 left / 1 right, six-axis 2). Every call under the
public functions goes through `nn::hid::detail` (sdk's PLT), answered here instead of by the hid service, including
the batches VibrationTarget queues through `detail::EnqueueVibrationValue` into `detail::SendVibrationValues`.

`VibrationValue` is {amplitude low, frequency low, amplitude high, frequency high}: 0..1 and Hz, neutral 0/160/0/320.
`VibrationDeviceInfo`: type 1 is a linear resonant actuator; position 1 left, 2 right.

- `RUMBLE_STALE_NS` (250 ms): a side the game has not updated for 250 ms is silent.
- `RUMBLE_REFRESH_NS` (50 ms): an unchanged value is sent again every 50 ms (its 250 ms start over).
- `RUMBLE_MIN_GAP_NS` (10 ms): a changed value goes out at most every 10 ms (the game streams every few ms).
- `vib_last`, `vib_at`, `sent_at`, `sent_low`, `sent_high` and `vib_allowed` (PermitVibration) are under `vib_lock`.
  `six_running` is set from StartSixAxisSensor to StopSixAxisSensor.

### `rumble_cfg()`

`NX2SARIA_RUMBLE_STRENGTH` (percent, 0..300, default 130; 0 = off) and `NX2SARIA_RUMBLE_CURVE` (exponent on the
amplitude, 0.2..1.5, default 0.6). HD rumble is strong at low amplitude, an ERM motor hardly turns there, so weak
values are lifted by the curve (0.1 -> 0.25) and the strength scales the result (saturating at 65535; 0.66 and up
reach full drive at 130 %). Both are live settings (the menu): read again whenever `cfg_version` changes.
`RUMBLE_STRENGTH_DEFAULT` (1.3) is the default of `RUMBLE_STRENGTH` in `runtime/cfg/cfg.c`, as a factor.

### `handle_id()`

The SDK's id check (the utility functions): players 1-8, other (0x10) and handheld (0x20); the public functions abort
on another id.

### `ov_vibration_handles()` `ov_six_axis_handles()`

`int GetVibrationDeviceHandles(VibrationDeviceHandle *out, int count, const NpadIdType &id, NpadStyleSet style)`:
`utility::GetVibrationDeviceHandles`' table, the first style bit in its order; none with `NX2SARIA_RUMBLE=0`. A
strength of 0 keeps the devices (silent), so the menu can turn rumble up again.

`int GetSixAxisSensorHandles(SixAxisSensorHandle *out, int count, const NpadIdType &id, NpadStyleSet style)`:
`utility::GetSixAxisSensorHandle`'s order (one sensor per style, two for the Joy-Con pair). Also with
`NX2SARIA_MOTION=0`: the sensor then reads at rest (`six_fill()`), so the menu can turn motion on while the game runs.

### `vibration_side()`

The vibration device (0 left, 1 right) a handle names on the connected controller, or -1 (logged once).

### `clamp01()` `light_share()`

`clamp01()` turns NaN into 0. `light_share()`: the share of a band at f Hz that goes to the light (high-frequency,
right) motor: none at 160 Hz and below (the low band's neutral frequency), all at 320 Hz and above (the high band's),
log scale between.

### `hd_update()`

Switch HD rumble as it is, for a gamepad whose actuators take it (`input_rumble_hd_on` of `runtime/input/input.h`;
`NX2SARIA_RUMBLE_HD`): each side's newest value goes on with both bands' frequency and amplitude, the amplitude scaled
by the rumble strength over its default alone: the default strength is above 100 % to make up for what two motors
lose, and an actuator that plays the value itself needs none of that, so at the default it gets the game's amplitude
exactly and the setting makes it weaker or stronger from there (the curve is for motors). A side the game has not
updated for 250 ms, or any side while vibration is not permitted, is at rest. `runtime/input/input.c` writes the
reports and no longer drives that gamepad's motors, so `rumble_update()`'s own call reaches only the DualShock 3
reader then. Under `vib_lock`.

### `rumble_update()`

Switch HD rumble -> `SDL_RumbleGamepad`. Each side's two bands split by frequency between the heavy (low) and the
light (high) motor (`light_share()`) and add up as energy per motor (root of the summed squares); the stronger side
drives each motor. A side the game has not updated for 250 ms is silent. Each send lasts 250 ms and is renewed every
50 ms while the game streams, so the pad stops by itself when the game stops sending (pause, freeze). Under
`vib_lock`.

### The detail calls

| Function | SDK call | Note |
|---|---|---|
| `ov_vib_init()` | `Result detail::InitializeVibrationDevice(const VibrationDeviceHandle &)` | |
| `ov_vib_send()` | `Result detail::SendVibrationValue(const VibrationDeviceHandle &, const VibrationValue &)` | |
| `ov_vib_send_many()` | `Result detail::SendVibrationValues(const VibrationDeviceHandle *, const VibrationValue *, int count)` | |
| `ov_vib_actual()` | `Result detail::GetActualVibrationValue(VibrationValue *out, const VibrationDeviceHandle &)` | the last value sent |
| `ov_vib_info()` | `Result detail::GetVibrationDeviceInfo(VibrationDeviceInfo *out, const VibrationDeviceHandle &)` | |
| `ov_vib_mounted()` | `Result detail::IsVibrationDeviceMounted(bool *out, const VibrationDeviceHandle &)` | |
| `ov_vib_permitted()` | `Result detail::IsVibrationPermitted(bool *out)` | |
| `ov_vib_permit()` | `Result detail::PermitVibration(bool)` | off silences the gamepad at once |
| `ov_six_start()` | `Result detail::StartSixAxisSensor(const SixAxisSensorHandle &)` | angle and direction start over |
| `ov_six_stop()` | `Result detail::StopSixAxisSensor(const SixAxisSensorHandle &)` | |
| `ov_six_states()` | `Result detail::GetSixAxisSensorStates(int *count, SixAxisSensorState *out, int max, const SixAxisSensorHandle &)` | |
| `ov_six_state()` | `Result detail::GetSixAxisSensorState(SixAxisSensorState *out, const SixAxisSensorHandle &)` | |

### `six_fill()`

All `max` entries, newest first: the game reads all 16 it asks for and ignores the count (`main+0x2a268e0`). A short
history repeats its oldest sample. No motion (sensor not started, no gamepad, one without a gyro,
`NX2SARIA_MOTION=0`): a controller lying still, not marked connected.

### The public wrappers

The public wrappers a Switch 2 title imports. The Switch 1 title imports `nn::hid::detail::*`; Switch 2 titles import
the public functions, whose direct calls into `detail::` no override reaches, so each public one is answered here with
the detail implementation above. Joy-Con 2 mouse (Nmouse), Jcms and sensor fusion have no host device: no handles,
starts that do nothing.

| Function | SDK call | Note |
|---|---|---|
| `ov_pub_six_start()` | `void StartSixAxisSensor(const SixAxisSensorHandle &)` / `void StopSixAxisSensor(...)` | |
| `ov_pub_six_states()` | `int GetSixAxisSensorStates(SixAxisSensorState *out, int count, const SixAxisSensorHandle &)` | the count filled |
| `ov_pub_zero_drift()` | `void SetGyroscopeZeroDriftMode(const SixAxisSensorHandle &, const GyroscopeZeroDriftMode &)` | the sensor is left as it is. The dead zone the mode would pick is the player's `gyro_deadzone` setting instead (`motion_options_refresh` of `runtime/input/input.c`, applied by `runtime/input/motion_fusion.c`); the mode the game asks for is only logged, the first call and each change of mode, 8 lines at most. The line names the level in force: `motion_options_refresh` takes any other `gyro_deadzone` value as low |
| `ov_pub_nothing()` | `void FinalizeSensorFusion()`, `FinalizeNmouse()`, the Jcms functions | nothing to do |
| `ov_pub_vib_init()`, `ov_pub_vib_send()` | `void InitializeVibrationDevice(const VibrationDeviceHandle &)` / `void SendVibrationValue(handle, value)` | |
| `ov_pub_vib_ounce_mounted()` | `bool IsOunceVibrationDeviceMounted(const VibrationDeviceHandle &)` | Ounce is the Switch 2's own actuator; the host's rumble devices count as mounted |
| `ov_pub_no_handles()` | `int GetSensorFusionHandles(...)` / `int GetNmouseHandles(NmouseHandle *, int, uint32_t)` | none |
| `ov_pub_nmouse_types()` | `void GetNmouseTypeSet(BitFlagSet<32, NmouseType> *out, NmouseHandle)` | empty |

## `runtime/hle/ov_gfx.c`

Graphics bring-up. Its first line is a licence notice.

- `nv::*`: record the game's allocator callbacks and graphics work memory (guest addresses) for the NVN layer.
- `nn::vi::*`: display, layer and native window identities; the vsync event is bound here.
- `nvnBootstrapLoader`: the end of `--boot` (`rt_boot_event` "nvn"); otherwise the synthetic address of an NVN proc
  (`runtime/nvn/nvn_procs.c`). The `nvnBootstrapLoader` stop is the entry to the NVN layer.

`VI_R_INVALID` (0xf601) is the result for a call on an unknown display or layer.

| Function | SDK call | Note |
|---|---|---|
| `ov_graphics_allocator()` | `void nv::SetGraphicsAllocator(AllocateFunction, FreeFunction, ReallocateFunction, void *user)` | |
| `ov_devtools_allocator()` | `void nv::SetGraphicsDevtoolsAllocator(AllocateFunction, FreeFunction, ReallocateFunction, void *user)` | |
| `ov_initialize_graphics()` | `void nv::InitializeGraphics(void *memory, size_t size)` | Switch 2 titles pass (nullptr, 0): that SDK also has `nv::InitializeGraphics()` and the driver takes its memory from the allocator `nv::SetGraphicsAllocator` installed. Only a half-given region is an error |
| `ov_vi_initialize()` | `void nn::vi::Initialize()` | the identities are small zeroed blocks in the guest-visible kernel region, in case SDK code reads them |
| `ov_open_display()` | `Result nn::vi::OpenDefaultDisplay(Display **out)` | |
| `ov_create_layer()` | `Result nn::vi::CreateLayer(Layer **out, Display *display)` | |
| `ov_native_window()` | `Result nn::vi::GetNativeWindow(void **out, Layer *layer)` | a placeholder the NVN layer recognises |
| `ov_vsync_event()` | `Result nn::vi::GetDisplayVsyncEvent(os::SystemEventType *event, Display *display)` | an auto-clear system event |
| `ov_nvn_bootstrap()` | `void *nvnBootstrapLoader(const char *name)` | the first NVN call. With `--boot`, the first one ends the boot (`rt_boot_event`), which returns when `--seconds` goes on |

### `ov_nvn_procs_register()`

The NVN procs need their synthetic addresses before `hostfn_seal`; the loader seals at the end of `rt_boot`, so the
registration runs at start-up (a constructor) instead of through `runtime/boot.c`.

### The Switch 2 display queries

The SDK versions reach the display and layer objects through the vi service; the overridden OpenDefaultDisplay and
CreateLayer hand out host stand-ins, so the SDK code would call through them (`main+0x4dc1e0` hit a null virtual in
ClearLayerExtendedSwapInterval). A desktop window is SDR, fixed 60 Hz, without VRR; HDR metadata and swap-interval
changes are accepted and dropped.

- `vi_hdr_meta()`: the game's HDR content metadata (native HDR): the swapchain's comes from the settings.
- `ov_vi_hdr_status()`: `Result QueryDisplayHdrStatus(bool *enabled, HdrOutputFormat *format, uint16_t *peak, const
  Display *)`. With native HDR (`runtime/vk/vk_hdr.c` `vk_hdr_native()`): an HDR10 display (format 1, which makes the
  game present PQ in 10 bits) of the settings' peak; otherwise SDR, as a desktop window is.
- `ov_vi_paper_white()`: `Result QueryDisplayHdrPaperWhite(uint16_t *nits, const Display *)`: the SDR reference white
  (203); with native HDR the paper white of the settings (`NX2SARIA_HDR_GAME_NITS`).
- `ov_vi_refresh_rate()`: `Result QueryDisplayCurrentRefreshRate(RefreshRate *rate, bool *variable, const Display *)`:
  RefreshRate is taken to be a 32-bit Hz value (a guess: no reference describes it); 60, not variable.
- `ov_vi_vrr_supported()`: `Result QueryDisplayVrrSupported(bool *supported, const Display *)`.

### `ov_nv_init_check()`

`void nv::InitCheck()`: the SDK's checks its own "graphics initialised" state, which the overridden InitializeGraphics
never sets, and aborts (subsdk0's `movie::VideoDecoder::Initialize` calls it). The host's record answers instead.

## `runtime/hle/ov_media.c`

The hardware Opus decoder (`nn::codec`), decoded with libopus on the host. The software keyboard (`nn::swkbd`) is in
`runtime/hle/swkbd.c`. Its first line is a licence notice.

`CODEC_R_INVALID` (0x1fcd1) is the "invalid argument" result for `nn::codec`. `OPUS_WORK_SIZE` (0x9000) is what the
game is told to reserve: libopus stereo decoder state (about 26 KiB) plus 4 KiB.

`Decoder` is one record per initialised decoder object (the guest's HardwareOpusDecoderType). The registry lock only
guards the list; a decoder's own lock serialises its libopus state, so different streams decode in parallel. Lock
order: registry, then decoder (a caller takes the decoder lock before it drops the registry lock, so Finalize cannot
free a decoder another thread is using). `acquire()` returns the decoder with its use lock held.

| Function | SDK call | Note |
|---|---|---|
| `ov_work_size()` | `size_t GetHardwareOpusDecoderWorkBufferSize(int rate, int channels, int option)` | |
| `ov_initialize()` | `Result InitializeHardwareOpusDecoder(HardwareOpusDecoderType *d, int rate, int channels, void *work, size_t size, int option)` | the state lives in libopus on the host; the guest's work buffer is reserved but unused. Initialising the same object again without Finalize replaces its decoder, after waiting for a decode in flight |
| `ov_finalize()` | `void FinalizeHardwareOpusDecoder(HardwareOpusDecoderType *d)` | a decode in flight finishes first; the registry lock keeps new ones out |
| `ov_sample_rate()`, `ov_channel_count()` | `int GetHardwareOpusDecoderSampleRate` / `GetHardwareOpusDecoderChannelCount(const HardwareOpusDecoderType *d)` | |
| `ov_reset()` | `Result ResetHardwareOpusDecoder(HardwareOpusDecoderType *d)` | |
| `ov_decode()` | `Result DecodeOpusInterleavedWithHardware(HardwareOpusDecoderType *d, size_t *consumed, int *samples, int16_t *out, size_t out_size, const void *in, size_t in_size)` | the input is Nintendo's frame: an 8-byte big-endian header {payload size, final range} and the Opus packet. A header range of 0 is not a recorded value (some encoders write 0 into every packet), so it is not checked. A range mismatch is logged once and the samples are kept |

## `runtime/hle/ov_audio_input.c`

Audio input for the `nn::audio` input-stream calls the game imports (the Ocarina can be played into a microphone):
each stream reads the default recording device through `runtime/audio/audio_capture.c`, which SDL converts to the
format the game asks for. The calls are overridden in the game (`HOST_OVERRIDE`) because the audio service refuses
input streams. A stream that failed to open is unknown to every call here, so purging or closing it is safe; before,
the game purged a stream that never opened and crashed. `NX2SARIA_MICROPHONE=0` (default 1, `mic_on()`) refuses every
input stream, as the game saw before. The feature: [audio-input.md](../audio-input.md).

`BAD_INPUT` (0x70899) is `nn::audio`'s invalid-argument result (module 153, description 900), as in
`runtime/audio/audio_mixer.c`. `input_name` ("DefaultMicrophone") is the name the game is given for the default
device.

## `runtime/hle/ov_zstd.c`

The game's zstd decoder on the host's libzstd. Most RomFS files of the game (1.4.3) are zstd frames, many with a
dictionary from `Pack/ZsDic.pack.zs`; the decoder is BSD zstd linked into main. The patch set "zstd"
(`recomp/patches-v143.txt`) sends every call of ZSTD_decompressMultiFrame (`main+0x3a1f0`) and of
ZSTD_decompress_usingDDict (`main+0x3a1e0`) to the two host functions of this file, on the patch page. Lifted, the
decoder ran its literal and match copies as chains of small functions with the registers in `Cpu`; here the same
frames decode in native code.

- Results match the guest's: libzstd decodes the same format, and every result that is an error goes to the guest
  decoder again (it writes its own error code; a frame libzstd rejects may still be one the game's version accepts).
- A guest ZSTD_DDict (zstd's struct: dictBuffer, dictContent, dictSize first) becomes a host ZSTD_DDict made from a
  copy of its content, cached by guest address, content address, size and a fingerprint of the content.
- Inputs of at least `AWAY_BYTES` give the virtual core up while they decode (other guest threads run on it).

`NX2SARIA_ZSTD_NATIVE=0` keeps the guest decoder (the patch set switch); `NX2SARIA_ZSTD_CHECK=1` decodes each call
both ways, keeps the guest's result and logs every call where the two differ.

Ocarina of Time 1.1.1 has the same decoder at other addresses (`recomp/patches-v111.txt`: three calls of
ZSTD_decompressMultiFrame, `main+0xeb380`). Its loading threads decode most data through the game's stream feed
(`main+0xf5c80`): `host_stream()`. The host decodes there by default since a check run (`zstd_check = 1`) over a
village and a field: 3,072 streams, 31.7 GiB of output, compared byte for byte with the game decoder's, none
different. Until then an unset key sent every call on to the game's decoder, which took about a fifth of the running
time of the game's threads in a village and a second of each area load. `zstd_native = 0` keeps the game's.

- `GUEST_MULTIFRAME` is ZSTD_decompressMultiFrame: 0x70000eb380 on 1.1.1, 0x700003a1f0 on 1.4.3.
  `GUEST_USING_DDICT` (0x700003a1e0) is 1.4.3 main's ZSTD_decompress_usingDDict (1.1.1 has no site for it).
  `GUEST_STREAM` is 0x70000f5c80.

### `fingerprint()` `dict_for()`

`fingerprint()`: size, both ends and a sample of the middle: what tells a new dictionary at a reused address from the
old one. `dict_for()`: the host dictionary for a guest ZSTD_DDict or raw dictionary bytes (`guest` == 0); NULL when
its fields look wrong. The host dictionary is made from a copy: the guest may free or reuse its buffer. Entries are
never freed (another thread may be decoding with one); a full table sends further dictionaries to the guest decoder.
Another thread may have made the same one meanwhile, which is then used.

### `dctx_thread_end()` `host_decode()` `tracked_decode()`

`dctx` is a thread's one-shot decoder context (`host_decode()`), freed when the thread ends
(`plat_thread_end_hook`): the game makes and ends threads all through play, and each that had decoded left its context
behind, about 100 KiB.

`host_decode()` decodes on this thread's host context. It returns libzstd's result; `*ok` is 0 when the inputs cannot
be taken (a guest destination is checked by `decode()`). A ZSTD_DDict is read as its first three fields: dictBuffer,
dictContent, dictSize. `tracked_decode()` makes the host call inside the thread's host depth, as `runtime/hostfn.c`
counts it for asynchronous preemption.

### `trusted()`

Whether this build's sites may decode on the host: unless `zstd_native = 0` says the game's own decoder (the patch
set's switch already leaves the sites alone then on the other builds; 1.1.1 reads the key here as well), or in the
check.

### `decode()`

x0 dctx, x1 dst, x2 capacity, x3 src, x4 size, then (dict, dict size, ddict) as the entry has them. `guest`: the
guest function with the same arguments, `n` of them.

In place (the input inside the output buffer): the result depends on how the decoder's writes overtake its reads,
which libzstd does not promise to do as the game's version does, and a host decode that failed half way would leave
the guest decoder a damaged input. Such calls stay in the guest. On an error or inputs that cannot be taken, the guest
decoder decides (and writes its own error code).

### The stream feed

The stream feed of Ocarina of Time 1.1.1 (`main+0xf5c80`), which decodes most of what the loading threads read. The
game's decode job (`main+0xf5bf0`) hands each chunk it read to feed(S, src, int size, ddict) and looks only at the
result: 0 the stream is done, bit 31 set an error, anything else go on. The feed drives zstd's bufferless decoder:

| Offset | Field |
|---|---|
| S+0x00 u8 | phase: 1 begin, 2 running, 3 failed, 4 done |
| S+0x08 u64 | where the next output goes |
| S+0x10 i32 | output room left |
| S+0x14 u32 | content left |
| S+0x18 u64 | the DCtx workspace |
| S+0x20 u64, +0x28 i32, +0x2c i32 | carry buffer, its size, its fill |
| S+0x30 u64 | the DCtx (set at begin) |

Begin reads the frame header from the first chunk: a header that is not complete in it, a content size that is not in
it, or a begin that fails gives phase 3 and 0xffffffff. Then each feed decodes what its input completes, moves +0x08,
+0x10 and +0x14 by the bytes it wrote and returns +0x14; when the frame ends it sets phase 4. Input after the frame's
end gives phase 3 and 0xfffffffd, a decode error phase 3 and 0xfffffffe (the output is too small) or 0xffffffff. A
feed in phase 3 returns 0xffffffff and in phase 4 0xfffffffd.

`host_stream()` owns a stream from its begin or not at all: the guest DCtx never sees the blocks a host stream
decoded, so there is no falling back half way. It takes a stream whose first chunk holds a whole header of a plain
frame with its content size, whose output has room for all of that content and does not overlap the chunk, whose
dictionary (if any) the host can take, and for which a slot is free; every other stream, and every feed of a stream it
did not begin, goes to the game's feed. For its own streams it keeps the guest's fields and results as above, decoding
with ZSTD_decompressStream straight into the game's output. That output is one buffer for the whole frame
(ZSTD_d_stableOutBuffer): the decoder keeps no window of its own and reads earlier output where the game has it, and
a feed that finds the output anywhere but where the last one left it fails the stream.

The game's job stops feeding at a result of 0, which a frame gives once its content is out even when its last bytes
(the checksum) are still to come: such a stream keeps its slot for those bytes but is marked spent, and a new stream
takes a spent slot when no free one is left.

With `NX2SARIA_ZSTD_CHECK=1` the game's feed decodes every stream as before, the host decodes the same chunks into a
buffer of its own, and the two outputs are compared when the game ends the stream or has all its content out ("zstd
check: stream ..."). The check also reports a game output that did not advance through one buffer. `--probe-zstd`
(`probe_zstd()`) runs host streams on frames it makes itself.

`Stream`: `s` is the guest stream state's address (0: free); `dctx` stays with the slot from stream to stream;
`shadow` is, in check mode, the host's output; `content` the frame's size and `pos` the bytes the host wrote; `dst0`
and `cap0` the game's output at the begin: every feed writes on into this one buffer; `in` the compressed bytes fed;
`spent`: all the content is out, the game feeds no more, and the slot may be taken; `done`, `bad` and `moved` are
check mode's: the host reached the frame's end or failed; the game's output moved.

### `stream_take()` `stream_drop_of()`

`stream_take()`: the slot of `s`, or a free one for it, or one whose stream is spent; NULL when there is none (that
stream decodes in the game). `stream_drop_of()` drops the slot of stream state `s`, if it has one. It is found and
cleared under one hold of the lock: between a `stream_find()` and a `stream_drop()` another thread can take a spent
slot for its own stream (`stream_take()`), and the drop then cleared that thread's slot, which a third thread took as
free and reset the decoder context in use.

### `stream_dctx()` `stream_run()`

`stream_dctx()`: a context for streams: the output is one buffer for the whole frame, so the decoder allocates no
window (a window of its own would be as large as the frame's, and a frame may name one as large as its content), and
it takes any window size, as the game's decoder does. `stream_run()` feeds `in` to the stream's context until the
input is used up, the frame ends (0), an error, or nothing moves. Inside the thread's host depth and with the core
given up for large chunks, as `tracked_decode()`.

### `stream_begin()`

A stream at its begin (phase 1): its slot, the host context ready for the frame, or NULL when the game's feed is to
decode it (see above). In check mode the guest's state is left alone. An output without room for the content fails
part way in the game, with what fits written: that is left to the game. So is an in-place stream. Outside check mode
it writes what the game's begin leaves in its state.

### `stream_feed()`

A feed of a host stream: the guest's fields and result as the game's feed leaves them. With nothing to feed it
returns the content left, as the game. Input after the frame's end, and nothing moved with input left, both fail the
stream (phase 3).

### `stream_check()`

Check mode: the host decodes the chunk into its own buffer, the game's feed runs as always and its result stands;
when the game ends the stream, or has all its content out, the two outputs are compared.

- A spent stream was compared already: the game was not expected to feed on after a result of 0.
- `done` is 2 for input after the frame's end.
- A host stream writes into the buffer the stream began with: the game's output has to advance through it alone
  (`moved`).
- "All out" is phase 2 with a result of 0: the job feeds no more, with or without the frame's last bytes. Such a
  stream keeps the slot in case the game does feed on; nothing more is compared.
- When the game failed, the host must not have a clean frame.

### `host_stream()`

A slot left for a stream state that begins again is dropped first: a stream the game gave up without ending it.

### `probe_feed()` `probe_stream()` `probe_zstd()`

`--probe-zstd`: host streams on frames made here, fed through `stream_begin()` and `stream_feed()` with a stream
state laid out as the game's. It checks the output and the state's fields after every feed, the frame's end, the
streams that are left to the game, input after the end, a damaged frame, a moved output and the slots. It needs no
game files.

- `probe_feed()`: one feed as `host_stream()` runs it outside check mode: the result, or 1 << 32 when the stream is
  the game's.
- `probe_stream()` feeds `frame[0, n)` in chunks of `chunk` bytes after a first one of `first`.
- The test data is random runs and repeats of earlier bytes from up to 2 MiB back: matches that reach far into earlier
  output. A stream with its content out and its checksum unread is spent: the next stream takes its slot when none is
  free.

### `zstd_sizes()`

`NX2SARIA_TRACE=perf` "mem: sizes" (`runtime/diag/perf.c`): host dictionaries (never freed) and stream slots taken.

## `runtime/hle/movie.h`

What `runtime/hle/ov_movie.c` (the host `movie::Decoder`) and `runtime/hle/movie_probe.c` share. The game reaches the
decoder through the `HOST_OVERRIDE` entries named in `movie_symbols`; the probe calls the same entries with a `Cpu` of
its own and stand-ins for the guest functions the decoder calls. Its first line is a licence notice.

- The `MOVIE_*` entries are in the order of `movie_symbols[]`: the imported constructor, then the 19 slots of the
  vtable for `movie::Decoder` (`subsdk0+0x560700`..`0x560790`, each an ABS64 relocation by name).
- `MovieGuest`: the guest addresses of the functions the decoder calls with `rt_call`: `nn::os::SignalEvent`,
  `movie::Buffer` and `movie::MediaData` methods, the vtable address the constructor stores (vtable + 0x10), and the
  original code of every entry for decoders left to the library. Filled from the loaded modules at the first entry
  call, or by the probe.
- `MOVIE_EV_*` index DecoderEvents: an `EventType *` per event.
- `movie_set_guest()` (probe): stand-ins instead of the modules. `movie_set_clock()` (probe): a fake clock (ns) for
  the pacing; NULL is the real one. `movie_set_pace()` (probe): 0 hands every decoded frame over at the next tick.
  `movie_stats()`: the counters of the decoder at guest object `obj`; 0 if none. `movie_quiet()`: 1 when its worker is
  idle and cannot take more input now.

## `runtime/hle/ov_movie.c`

`movie::Decoder` for VP9 on the host with libvpx (BSD-3-Clause, linked statically). Its first line is a licence
notice.

The Switch 1 title's pre-rendered movies (`Event/Movie/*.webm`, 28 files: VP9 profile 0, 1920x1080, 30 fps, video
only) play through the SDK movie library in subsdk0, Android's MediaCodec over NVIDIA NvMM. Its decoder opens NVDEC
through NvRm, whose first NvOsDrvOpen (`sdk+0x3f9050`) aborts with "Use of nv::InitializeGraphics is required."
because `runtime/hle/ov_gfx.c` takes `nv::InitializeGraphics` over; nvdrv and `mm:u` are not served either. The game's
own demuxer (`movie::Extractor`) runs as guest code; only the decoder is replaced.

main imports the constructor and calls the decoder through the vtable for `movie::Decoder`, whose slots are ABS64
relocations by name: the `HOST_OVERRIDE` entries replace them. A VP9 buffer-mode decoder (type 3, mode 1) is a host
object, +0 the library's vtable, +8 the host `Movie` (where the library keeps its DecoderImpl); any other decoder runs
the original code. Behaviour follows DecoderImpl:

- output format 4 is packed NV12 (Y plane, then interleaved UV; stride = width), copied into the client's Buffer;
- input and output buffers are indices handed out FIFO, as MediaCodec does; input flag 4 / output flag 4 = end of
  stream;
- client events {input available, output available, format changed, error} (DecoderEvents, 4 EventType pointers) are
  signalled with `nn::os::SignalEvent` on a guest thread.

Decoding runs on one host worker thread per decoder (libvpx adds its own tile threads), never on the game's threads:
SendInputBufferForDecode queues the input and returns. Events the worker owes (input free, format changed, error) are
signalled at the next entry call or present, whichever comes first.

Pacing: the game shows one decoded frame per game frame, which plays a 30 fps movie at 30 fps only at 30 fps. So
decoded frames are handed over when their presentation time is due, counted from the first frame (`movie_tick()`, at
every present on the presenting guest thread), at most one per present. When the game falls behind, a frame more than
1.5 periods late is dropped if the next one is due too, so the picture keeps time with the movie's sound.
`NX2SARIA_MOVIE_PACE=0` hands every decoded frame over at the next present; `NX2SARIA_MOVIE_HOST=0` leaves every
decoder to the library (which aborts at the first movie).

`Movie`: `guest` is the `movie::Decoder` object; `events` the guest `DecoderEvents *` (an `EventType *` per
`MOVIE_EV_*`); `in[]` are in the kernel region: the game writes the compressed frames there; `pending[]` is sent, not
decoded yet (the worker takes them in order); `out[]` holds NV12 frames; `decoded[]` is decoded, not due yet;
`ready[]` is handed over: AcquireOutputBufferIndex returns these; `owed` the events to signal on the next guest thread
(a bit per `MOVIE_EV_*`); `t0` the host time of the first frame handed over (pacing) and `pts0` its presentation time
(us); `gen` is bumped by Flush, Stop, Reset and destruction: a decode in flight is dropped. `g_count` counts the live
host decoders: `movie_tick()` returns at once when it is 0. Lock order: `g_lock`, then `Movie.mu`.

### `guest_init()` `pass()`

`guest_init()` resolves the guest functions the first time any entry runs (the modules are loaded and relocated by
then). `pass()` runs the original code of an entry with the caller's arguments; its result stays in x0.

### `collect()` `fire()`

Events are signalled after the locks are released: the woken game thread calls straight back in. `collect()` runs
with `m->mu` held.

### `to_nv12()`

I420 (libvpx) -> packed NV12: Y rows, then rows of interleaved U, V.

### `worker()`

The worker takes inputs in order, decodes them outside the lock into a reserved output slot, and publishes the frame.
A reserved slot is in no queue, so the game cannot release it. A decode whose generation changed meanwhile (flushed,
stopped or reset: the queues were reset after it) is dropped.

### `quiesce()` `release_codec()`

`quiesce()`, with `m->mu` held: invalidates a decode in flight and waits for the worker to leave it. It returns
whether the calling guest thread gave its core up; the caller calls `kern_core_back` after releasing `m->mu`.
`release_codec()` runs with `m->mu` held and the worker quiesced.

### `due_at()` `release_due()`

`due_at()`: the host time a decoded frame is due (ns); end of stream is due as soon as it is first in line.
`release_due()`: one present: drop leading frames more than 1.5 periods late while the next one is due too, then hand
over at most one due frame (end of stream right after the last frame).

### The entries

| Function | SDK call | Note |
|---|---|---|
| `ov_ctor()` | `Decoder(DecoderType type, DecoderOutputFormat format, DecoderMode mode, int priority)` | |
| `destroy()` | | leaves the object as the library's destructor does (the vtable, no DecoderImpl) |
| `ov_d0()` | the deleting destructor | frees the host state, then the library's deleting destructor frees the object (impl now 0) |
| `ov_d2()` | the base-object destructor (D2) | 1.4.3's vtable and a PLT call name the complete-object destructor (D1); without virtual bases it is D2 |
| `ov_configure()` | `Status Configure(MediaData *format)` | |
| `ov_register_events()` | `Status RegisterClientEvents(DecoderEvents *events)` | |
| `ov_get_output_format()` | `Status GetOutputFormat(MediaData *format)` | `set_format()` sets "color-format" 21, COLOR_FormatYUV420SemiPlanar |
| `ov_flush()` | | drops queued work; the next input starts a new decode (after a seek: a key frame) |
| `ov_acquire_input()` | `Status AcquireInputBufferIndex(int *index, int *remaining)` | |
| `ov_get_input()` | `Status GetInputBuffer(int index, Buffer *buffer)` | |
| `ov_send_input()` | `Status SendInputBufferForDecode(int index, int offset, int size, long time, unsigned flags)` | after end of stream the buffer is handed back |
| `ov_acquire_output()` | `Status AcquireOutputBufferIndex(int *index, long *time, unsigned *flags, int *remaining)` | paced frames become ready only at presents (`movie_tick()`) |
| `ov_get_output()` | `Status GetOutputBuffer(int index, Buffer *buffer)` | |
| `ov_get_output_size()` | `Status GetOutputBufferSize(int *size, int *count)` | |
| `ov_release_output()` | `Status ReleaseOutputBufferIndex(int index)` | |
| `ov_get_output_buffer_format()` | `Status GetOutputBufferFormat(int index, MediaData *format)` | |
| `ov_get_output_md()` | `Status GetOutputBuffer(int index, Buffer *buffer, MediaData *format)` | |

## `runtime/hle/ov_movie2.c`

Switch 2 movie playback: `movie::VideoDecoder` (subsdk0) answered by the host with dav1d.

The title's movies are WebM with AV1 video only (`Event/Movie/*.webm`, 1920x1080 60 fps, 8-bit 4:2:0, limited range
BT.709); their sound is a separate stream the game plays alongside (`Sound/Resource/Stream/<movie>.bwav`), so a movie
that runs for its real length is also what makes the title sequence audible. The SDK's `nn::movie::MediaExtractor`
demuxes as it is; subsdk0's VideoDecoder sits on `nn::codec::video` (a video server this host does not have), so the
public VideoDecoder methods the game imports are replaced here. VideoFrame stays subsdk0's own object; its layout
(subsdk0 `VideoFrame::SetBuffer` / `SetPixelPlaneData` / `SetTimeStamp` / `SetEndOfStreamState`, sizeof 0x90):

| Offset | Field |
|---|---|
| +0x08 + 8 i | plane i data (plane 0 = the buffer of SetBuffer) |
| +0x28 + 8 i | plane i filled size |
| +0x48 | buffer capacity |
| +0x78 | time stamp (TimeSpan, ns) |
| +0x80 | pixel format |
| +0x84 | pixel storage type |
| +0x88 | end of stream (byte) |

`nn::movie::Buffer` (sdk): +0x00 data, +0x08 capacity, +0x10 filled size, +0x18 time stamp, +0x20 flags (8 = end of
stream, as the game sets it at `main+0x3b60014`).

Decoding is asynchronous, as on the hardware: DecodeAsync queues a copy of the sample and returns; one host thread
per decoder runs dav1d and converts each picture to NV12 (the game's frame format: it sizes each frame buffer width *
height * 3 / 2 and reads two planes) and to RGBA for the window. Decoding on the caller's thread stalled the game's
movie loop (it also acquires frames) by up to 90 ms on heavy frames: visible stutter.

Back-pressure: while `QREADY` samples or pictures wait, DecodeAsync answers `R_BUSY` and the input-ready event is
clear; the game's decode step (`main+0x3b5e600`) treats any non-zero result as "try again" on that event.
AcquireVideoFrame answers 0x7b when nothing is ready (subsdk0 `VideoDecoder::AcquireVideoFrame`'s own result).

The game draws the frame with its own shaders (a YUV compute pass). Each frame is debanded first (flat areas averaged
with sub-level precision), so the encoder's flattened gradients (skies, clouds) do not show as hard bands, and written
back to NV12 with a 4x4 ordered dither. With `NX2SARIA_MOVIE_OVERLAY=1` the host instead shows the frame over the game
image (`gfx_movie_frame_own` of `runtime/gfx/gfx_present.c`), converted here: BT.709 limited range, bilinear chroma,
the same dither. `NX2SARIA_MOVIE_DEBAND=0` turns the deband off.

`NX2SARIA_MOVIES=off` skips movies instead (`runtime/hle/romfs.c` `movie_skipped()`); this title then quits, so for
testing use `NX2SARIA_MOVIE_SKIP=<seconds>`: a movie ends as if it had played to its end once its samples reach that
time (the next sample becomes the end-of-stream marker, later ones are dropped), and the game goes on as after a whole
movie.

- `R_BUSY` (0x7d): DecodeAsync while the queues are full: the game retries on its input-ready event.
  `BUFFER_FLAG_EOS` is 0x8.
- `QMAX` (8): decoded pictures held at most. `INMAX` (8): samples waiting for the decoder thread at most. `QREADY`
  (4): input-ready while fewer than this many samples and pictures wait. `NFRAMES` (16): registered VideoFrames.
  `CQMAX` (2): decoded pictures waiting for the convert thread at most.
- `Decoded` is a picture for the convert thread.

### `Video`

- `lock` guards the queues, frames and flags; `codec` guards dav1d: the decoder thread while it decodes, Flush and
  Finalize. `work` is signalled when a sample arrived, or on quit.
- `run_index`: which movie of the run this is, counted from 1 as decoders come up (for the log); `movies_started`
  counts the decoders that came up this run.
- `convert` and `conv` (a decoded picture arrived, a slot freed, or quit): the convert thread debands and converts
  what dav1d decoded while the decoder thread decodes the next picture: one after the other on the decoder thread the
  two took longer than the 16.7 ms a 60 fps movie frame has.
- `y16`, `u16`, `v16`: the convert thread's deband planes (16-bit Y, U and V, `plane_room` samples of luma), kept from
  picture to picture and made again only for a larger one: made per picture they were 6.2 MB of fresh pages each at
  1080p, 60 times a second. `finalize()` frees them once the convert thread is joined.
- `in_ev` and `out_ev` (0x40 bytes each) are an `nn::os::SystemEventType` each.
- `generation` is bumped by Flush: pictures of an older one are dropped.
- `live`: counted in `movies_live`: from Initialize, or a sample after a Flush, to the end-of-stream picture, Finalize
  or the destructor.
- `ts0` and `shown0_ns`: the first shown picture's time stamp and when it was shown, for the pacing report's distance
  between the picture and real time; `clock_on`: they hold (cleared by Flush and Initialize).
- `rel_lag` and `rel_lag0`: the game fetches pictures ahead into a list of its own (`main+0x3b61df0`) and lets go of
  each when the next one takes its place on screen (ReleaseVideoFrame), so the distance to real time is measured there
  too: the newest released picture's, and how far it moved since the first released one (the drift), in ns.

### `ov_movie_live()` `set_live()`

`movies_live` counts the decoders playing (`Video.live`); `movie_cap_hz` of `runtime/pacing/frame_rate.c` holds the
frame rate meanwhile. In `set_live()` the guest's decode and acquire threads may race, so the flag is exchanged.

`t_wait_ns`, `t_send_ns`, `t_get_ns` and `t_make_ns` are the time summed for the pacing report (one movie plays at a
time): on the decoder thread waiting for the game's next sample, `dav1d_send_data` and `dav1d_get_picture`; on the
convert thread `make_picture()` (deband and conversion). `n_made` is the pictures converted.

### `update_events()` `clear_queues()`

Both run under `v->lock`.

### `deband_table()` `deband_edge()` `deband()`

Deband (the approach of mpv's deband filter, our own code): the movies' flat gradients (skies, clouds) are encoded as
blocks of one level with hard contours between them. For each sample, four neighbours at a pseudo-random offset of
4..16 samples (rotated per pixel, so no pattern repeats) are compared with it; when all lie within `DEBAND_THRESH` (3)
levels the area is flat and the sample becomes their mean with 4 bits of fraction, else it keeps its value. Edges and
texture differ by more than that and stay as they are. Output: plane * 16.

The deband taps are radii 4..16 at golden-angle rotations, so every tap of a sample at least `DEBAND_REACH` (16) away
from each edge lies inside the plane. `deband_edge()` handles one sample near an edge: a tap outside the plane keeps
the sample as it is; otherwise it returns the mean, * 16.

`deband()`: each sample becomes the mean of four taps around it (a rotated pair of mirrored offsets) when all four
lie within `DEBAND_THRESH` of it, else stays as it is; the result is * 16, keeping the mean's fraction. Samples away
from the edges, nearly all of them, skip the bounds tests and choose without a branch: 28 ms on one thread for a 1080p
frame became 16 ms, and in play the stripes no longer overran the 16.7 ms a 60 fps movie frame has. Each offset is
kept as a distance in the plane: (+dx, +dy) and (-dy, +dx); their mirrors negate.

### `to_rgba()`

Debanded planes (* 16) -> RGBA, BT.709 limited range. Chroma is interpolated bilinearly between the 4:2:0 samples
(centred between luma rows, co-sited with even columns); a 4x4 Bayer threshold is added before the final rounding to 8
bits, which turns the remaining sub-level steps into fine noise instead of bands.

The two chroma rows around a luma row are weighted 3:1 toward the nearer one. An even column is on the chroma sample,
an odd one halfway. Chroma is * 16 * 16 (plane * 16, weights summing to 16), shifted right by 4 to * 16. The luma term
is (Y - 16 * 16) * 298, in * 16 * 256; the dither is under one output step in the * 4096 unit.

### `to_nv12()` `stripe_run()`

The overlay conversion runs in `NSTRIPES` (6) horizontal stripes on short-lived threads, in two phases (deband every
plane, then convert), since the convert reads chroma rows of the neighbouring stripe. One thread took 27 ms per 1080p
frame: too slow for 60 fps. `Stripe.py`, `pu`, `pv`, `ys` and `cs` are phase 2's: the decoder's 8-bit planes and their
strides (luma, chroma).

`to_nv12()` writes the debanded planes back into the NV12 frame (the game converts it itself), with a 4x4 ordered
dither: the 4 fraction bits become texel-sized noise instead of bands. Phase 2 of `stripe_run()` debands straight
from the decoder's planes, so the NV12 rows can be written at once.

### `stripe_worker()` `run_stripes()`

The `NSTRIPES` - 1 helper threads are made once and kept. Made per call (two calls per picture, 30-60 pictures a
second) they were about 300 new threads a second for as long as a movie played, and every DLL in the process (the GPU
driver's, overlays and audio hooks) sets itself up for each new thread: a cost per thread, and with an AMD driver on an
RX 580 the process commit grew by about 300 MB a second during the title movie until the system ran out (FATAL "movie:
out of memory for a 1920x1080 frame" at 45.7 GB). One caller at a time (`call_lock`); a worker that could not be
started leaves its stripe to the caller.

`SP`: `call_lock` allows one `run_stripes()` at a time (several decoders may exist); `lock` guards the other fields;
`gen` is bumped per batch; `pending` is the helper stripes of the batch not finished; `nworkers` the started helpers
(stripes 1..nworkers), -1 when not tried yet. The stripe height is even, so chroma rows split at the same places. The
stripes of helpers that could not start run on the caller.

### `movie_oom_log()` `planes_for()`

`movie_oom_log()`: out of memory for a decoded frame: the frame is dropped (the game shows the previous one a little
longer) and the first drop of a run is logged. The game used to stop here; on a PC whose commit charge reached its
limit for a moment, that ended a run that the next second would have had memory for. `planes_for()`: the Video's
deband planes for a w x h picture (convert thread). 0: no memory for them; the frame is then dropped as for any
allocation of a picture, and no planes are kept.

### `make_picture()`

One dav1d picture (8- or 10-bit 4:2:0) -> NV12 with 8-bit samples, and its RGBA; convert thread.

- The host overlay (`NX2SARIA_MOVIE_OVERLAY=1`) takes an RGBA frame; without it the game converts the NV12 frame
  itself. A frame the PC has no memory for is dropped, not the game.
- `NX2SARIA_MOVIE_DEBAND=0` keeps the decoded planes.
- The usual case (8-bit, debanded, the game converts the frame): one pass per stripe debands the decoder's own planes
  and writes the dithered NV12 rows. Reading the decoder's planes instead of an NV12 copy of them gives the same
  samples, and nothing reads the frame while it is written, so each stripe finishes its rows alone: one round of
  threads per frame instead of two, and no single-threaded copy before it.
- Otherwise: debanded planes (`NX2SARIA_MOVIE_DEBAND=0` keeps the decoded ones), then the overlay's RGBA or the
  dithered NV12.

### `deliver()` `hand_over()` `clear_converts()`

- `deliver()` queues a decoded picture, waiting while the queue is full (the game acquires at the movie's pace);
  decoder thread.
- `hand_over()` queues a decoded picture (or the end-of-stream marker) for the convert thread, waiting while it has
  `CQMAX`; decoder thread. On quit the picture is released here.
- `clear_converts()` releases the pictures still waiting for the convert thread; under `v->lock`.

### `decoder_thread()` `stop_thread()` `finalize()`

In `decoder_thread()`, a flush (`ov_flush()`) between taking the sample and the codec means the sample belongs to the
stream before it, and fed into the flushed decoder its picture would come out after the seek, so it is dropped. The
end-of-stream marker goes after the last picture, through the same queue. `stop_thread()` clears the converts before
`dav1d_close`: the pictures are the decoder's. `finalize()` frees the deband planes: the convert thread, their only
user, has been joined.

### The entries

| Function | SDK call | Note |
|---|---|---|
| `ov_ctor()`, `ov_dtor()` | `VideoDecoder()` / `~VideoDecoder()` | the object's first 8 bytes hold the host record |
| `ov_initialize()` | `Result Initialize(void *memory, size_t size, MediaMimeType mime, VideoDecoderOutputFormat format, VideoCodecMode)` | |
| `ov_ok()` | `Result Configure(const MediaProperty *, const void *configuration, size_t)` | the AV1 sequence header also comes in band with the first key frame, which dav1d reads |
| `ov_finalize()` | `Result Finalize()` | |
| `ov_register()` | `Result RegisterVideoFrame(VideoFrame **frames, int count)` | |
| `get_event()` | `Result GetInputReadyEvent(SystemEventType **out)` / `GetOutputReadyEvent(SystemEventType **out)` | |
| `ov_flush()` | `Result Flush()` | samples and pictures not yet acquired are dropped (a seek or a restart follows) |
| `ov_decode()` | `Result DecodeAsync(const nn::movie::Buffer *sample)` | queued for the decoder thread |
| `ov_acquire()` | `Result AcquireVideoFrame(VideoFrame **out)` | the oldest decoded picture in a free registered frame |
| `ov_release()` | `Result ReleaseVideoFrame(VideoFrame *)` | |

Skipping movies (`NX2SARIA_MOVIES=off`) leaves the subsdk0 decoder in place: the movie never opens, so it is not
called.

- `ov_dtor()`: the decoder and convert threads are joined, so nothing signals the events now: they are destroyed and
  leave the kernel's table of system events before their memory goes (`runtime/hle/kernel.c`
  `kern_unbind_system_event()`). Kept, each decoder left two dead entries and their kernel handles, and a long session
  filled the table.
- `vk_dump_movie_frame` is `runtime/vk/vk_dump.c`'s (`NX2SARIA_DUMP_MOVIE`).
- `ov_initialize()`: frame threading is on (8 threads; dav1d picks the delay): heavy stretches of the title movie took
  longer than a frame to decode one picture at a time (max_frame_delay 1) and the game's loop waited up to 94 ms for
  them. The delay costs nothing here, as decoding runs ahead on its own thread; end of stream drains the delayed
  pictures.
- `ov_register()`: each frame buffer starts as NV12 black (Y 16, UV 128 in its last third). The game converts a frame
  it holds before a picture arrives; zeros there are YUV (0, 0, 0), green, in stripes where rows were partly written.
- `ov_flush()`: the generation is bumped, so the decoder and convert threads drop what they are making; `clock_on`
  and `rel_on` are cleared: after a seek the report's real-time reference starts again from the next picture.
- `ov_decode()`: `skip_ns` is `NX2SARIA_MOVIE_SKIP` (-1 off). Finalize on another thread may have closed the decoder
  since the first test. `finalize()` takes `v->lock` after it clears `v->dav1d`, so read under `v->lock` it is either
  still set (the sample is queued before `finalize()`'s `clear_queues()`, which drops it, and `ov_finalize()` or the
  destructor then clears `live`) or already NULL: no sample makes a finalized decoder live again, which would hold the
  movie frame rate cap (`movie_cap_hz` of `runtime/pacing/frame_rate.c`). After the early end, what the demuxer still
  sends is dropped; the early end itself is the first sample at or past the skip time. With no memory for the sample
  the game is told to try again, as with a full queue. A sample that is not the end, with no end queued, makes the
  decoder live: playing again after its end (a Flush clears `eos_queued`).
- `ov_acquire()`: the game's YUV pass reads the frame buffer on the GPU. A host write into pool storage is one the
  page write tracking may not see in time (a mirror or texture over it kept its older or still-empty bytes: frames
  with stripes of zeros, which NV12 shows as green), so the back end is told directly (`nvn_guest_written`). The
  pacing report comes every 600 frames (10 s): the gaps between acquisitions and their time stamps, and how far the
  picture is behind the time since the first one (positive: the picture is late). The overlay takes the RGBA buffer.
  At the end of stream the log says how the movie kept time: the picture on screen against the clock, at its start and
  its end.

## `runtime/hle/movie_probe.c`

`--probe-movie`: the host movie decoder on one of the game's movies from the RomFS. A small Matroska/WebM demuxer
reads the video frames of `Event/Movie/<name>.webm` (the smallest one, or the first whose path contains
`NX2SARIA_PROBE_MOVIE`). The probe then drives the decoder through the same `HOST_OVERRIDE` entries the game calls,
with a `Cpu` of its own; the guest functions the decoder calls (`nn::os::SignalEvent`, `movie::Buffer`,
`movie::MediaData`) are host stand-ins registered as host functions, so `rt_call` reaches them as it reaches the SDK's.

VP9 (`movie::Decoder`, `runtime/hle/ov_movie.c`):

1. unpaced: every frame in, every frame out: frame count, size, output format, PTS order, events, decode cost;
2. paced at 60 presents per second (fake clock): at most one frame per present, one frame per 2 presents, no drops;
3. paced at 15 presents per second: late frames are dropped, the frame handed over is the one due.

AV1 (`movie::VideoDecoder`, `runtime/hle/ov_movie2.c`), in real time, played as the game plays a movie
(`av1_paced()`):

1. 30 updates a second: every picture is shown, one per update, in time-stamp order, the end of stream after the last,
   and the picture ends seconds behind real time (the slow motion of a slow game);
2. 60 updates a second: every picture is shown within 50 ms of real time.

Without the RomFS (no game data) it prints why and exits 0. `NX2SARIA_PROBE_MOVIE_SHOT=<file.ppm>` saves the middle
frame of a VP9 movie.

### `vint()` `demux()`

WebM: the VP9 track's frames (`Frame.pts` is in microseconds). `vint()` reads an EBML variable-length integer: an
element ID keeps its length marker, a size drops it (all ones = unknown size). `demux()` walks the file linearly,
entering the master elements on the way to the frames and skipping the rest:

| Id | Element |
|---|---|
| 0x18538067, 0x1549A966, 0x1654AE6B, 0xAE, 0xE0, 0xA0 | Segment, Info, Tracks, TrackEntry, Video, BlockGroup (entered) |
| 0x1F43B675 | Cluster (entered) |
| 0x2AD7B1 | TimecodeScale (ns) |
| 0xD7 | TrackNumber |
| 0x86 | CodecID |
| 0xB0, 0xBA | PixelWidth, PixelHeight |
| 0xE7 | Cluster Timecode |
| 0xA3, 0xA1 | SimpleBlock, Block |

### The stand-ins

`PBuf` stands in for `movie::Buffer` and `PMd` for `movie::MediaData`. `s_orig_d0()` is the library's deleting
destructor. `call()` drives the entries as the game does.

### `save_ppm()`

`NX2SARIA_PROBE_MOVIE_SHOT=<file.ppm>`: the middle frame as RGB (BT.709, limited range), to look at the NV12 output.

### `paced()`

A paced run over the first `limit` frames with a present every `period` ns of fake time; it returns 0 on success and
fills the per-present counts. The lag is how late the frame is shown (us).

### `av1_feed()` `av1_paced()`

AV1: this title's movies (`runtime/hle/ov_movie2.c`), played as the game plays them. The game's VideoFrame (0x90
bytes: +0x08 its buffer, +0x48 the buffer's capacity, +0x78 time stamp, +0x88 end of stream) and its samples
(`nn::movie::Buffer`: +0x00 data, +0x10 size, +0x18 time stamp in ns, +0x20 flags, 8 = end of stream).

`AV1_NO_FRAME` is 0x7b and `AV1_BUSY` 0x7d. `AV1_HELD` (2): the frames the probe registers, so the pictures it holds
at most, two as the game registers for its movies. `Av1Run`: pictures shown; updates; updates that began more than
4 ms after their time; how far behind real time the last and the worst picture shown were; the last one's time stamp
(ns).

`av1_feed()`: samples until the decoder answers busy, then the end of stream after the first `limit` frames.

`av1_paced()`: the first `limit` frames played as the game plays a movie (its decode step `main+0x3b5e600`, its
update `main+0x3b5ca60`): samples go in whenever the decoder takes them; an update every `period` ns of real time
takes every picture there is a free frame for, shows the oldest picture it holds and gives that frame back. The
movie's time starts with its first picture; the run ends with the end-of-stream marker. While the movie plays the
decoder is fed until the next update is due. A decoder that loses a picture or the end-of-stream marker must fail the
probe, not hang it (100000 updates at most). Before the first picture, with nothing decoded yet, the probe waits for
it (5 s at most).

### `probe_av1()`

1. a game at 30 updates a second (240 frames at most): every picture, one per update, in order, the end of stream
   last; slow motion;
2. a game at 60 updates a second (180 frames at most): every picture in time.

### `probe_movie()`

Stand-ins and entries: the vtable the constructor stores is a host address nothing calls.

1. unpaced: every 30th frame's mean luma of a centre row band is checked: a decoded picture, not a blank buffer;
2. paced at 60 presents per second (150 frames at most): one frame every second present; the span is the presents
   the frames span at 60 per second;
3. paced at 15 presents per second (120 frames at most): about every second frame is dropped, the shown one stays in
   time.

## `runtime/hle/ov_probe.c`

`--probe-services`: every override of `runtime/hle/ov_system.c`, `runtime/hle/ov_input.c`, `runtime/hle/ov_media.c`,
`runtime/hle/swkbd.c` and `runtime/hle/ov_gfx.c` called the way guest code calls it: by its synthetic address through
`rt_call` / `rt_dispatch`, with AAPCS64 arguments, on the guest main thread. It checks that each one matches a
relocation of the loaded modules, then the values: language, operation mode, notification messages, account, time
round trips, controllers, codec state, graphics records, the system events behind nfp and vi, the software keyboard
with arguments built by the guest SDK (answers from `NX2SARIA_SWKBD_ANSWER`, then the window pressed by
`NX2SARIA_SWKBD_TEST`) and the shared fonts when `tools/extract_fonts.py` has written them. The named fatal stubs, and
the account UID of a second run, are checked in child processes (this executable again,
`NX2SARIA_PROBE_SERVICES_CHILD=<case>`).

The override table is grouped by owner: `ov_system.c`'s `nn::oe`; applet, am and oe; the SDK-internal ones; account;
time; pl; `ov_input.c`; `ov_media.c`; `ov_gfx.c`. The fatal stubs each run in their own child process: the case name,
the override, what its FATAL line must say and an environment variable the child runs with (for the shared fonts a
font directory that does not exist, `NO_FONTS`). `gc` is the guest thread the calls run on.

### `call8()`

The override called through its synthetic address with x0..x7 = a[0..7]; caller-saved registers hold garbage, the
callee-saved ones a pattern that must survive. It returns x0.

### `set_env()`

It is `cfg_setenv`; a NULL value removes the variable.

### `event_works()` `event_set()`

`event_works()`: a system event bound by an override: not signalled, then signalled through the kernel, then
(auto-clear) cleared. `event_set()`: a manual-clear system event an override signalled when binding it: set, and still
set on a second wait.

### `probe_app_area()`

The application area on the made-up figure `probe_applet()` loaded, through the overrides as the game calls them: the
results without an area and for another access id, create, open, set, flush and get, the write counter in CommonInfo,
the figure's data saved in the profile and read back from there by a second load, recreate, the three settings applets
and the menu's erase. 1 when every check held.

ApplicationAreaCreateInfo is u32 access id, u32 initial data size, u64 initial data address. Saved in the profile: a
second load of the dump reads the figure back from the .dat. The settings applets answer at once with the reader's
handle.

### `probe_applet()`

- The emulated reader is there from the start (`runtime/hle/amiibo.c`), so the availability event is set when it is
  attached.
- The application area calls without a mounted figure answer device not found as well.
- The game attaches again before each detection: DestroySystemEvent on the attached event, then Attach* on the same
  SystemEventType. Both events must work again afterwards.
- The test figure. An amiibo's NTAG215 header: the UID with its two check bytes, static lock 0F E0, capability
  container F1 10 FF EE. The loader takes it and refuses the same file with a plain NDEF container (E1). The id is
  Link (Archer): character 0x0100, variant 0, figure type 0, model number 0x0353, series 9, format 02. The figure's
  data of this made-up UID goes to `<profile>/amiibo/04112233445566.dat` (`runtime/hle/amiibo.c` `dat_paths()`): gone
  before the test, so the figure starts new, and after it. No real figure has this UID.
- TagInfo carries the 7-byte UID without its check byte; ModelInfo has the series at 3, the model number little-endian
  at 4 and the figure type at 6.

### `probe_time()`

951782400 is 2000-02-29 00:00:00 UTC (the leap day). Month 13 carries into the next year (2023-13-01 is 1704067200).
The steady clock's source id is at ClockSnapshot +0x98 (+0x90: value): a span across sources answers 0x18074.

### `probe_feedback()`

Vibration and six-axis handles, values and states with no gamepad motion (the sensor is stopped before the reads that
check the rest state), the SDL -> Switch axis mapping and the sensor fusion on a private instance. `STD_GRAVITY`
(9.80665) is SDL_STANDARD_GRAVITY.

- `NX2SARIA_MOTION` is live: the handle stays (the sensor reads at rest while it is 0).
- Values with no amplitude: a connected gamepad stays still.
- SDL axes: lying flat (push +y) is Switch gravity -z, standing on the bottom edge is -y; yaw about SDL +y is +z.
- Fusion: flat, then 90 degrees a second about z for 1 s: a quarter turn, x axis now along y.
- StartSixAxisSensor again in the middle of the turn (the bow drawn again): the states from before it are gone, and
  the first one after it starts a fresh angle in a fresh frame; sampling numbers go on.

### `probe_swkbd()`

The software keyboard (`runtime/hle/swkbd.c`): a ShowKeyboardArg built by the SDK's own setters, as the horse naming
does. `ARG_WORK` is 0x518 on 1.4.3 and 0x4d0 otherwise: SDK 20.5 grew KeyboardConfig. `u16()` makes a NUL-terminated
UTF-16 copy of UTF-8 in guest memory. The SDK setters write where `swkbd.c` reads. The cases:

| Case | What |
|---|---|
| "Epona" -> "Epona" | the game's horse naming (`main+0x1ecbf64`) |
| "ABCDEFGHIJKL" -> "ABCDEFGHI" | at most 9 |
| "a@b%c\d e/f" -> "abcd e/f" | '@', '%' and '\' disabled |
| "<cancel>" | cancelled |
| "<initial>" -> the default name | no initial text |
| "<initial>" -> "Zelda" | initial text from the work buffer |
| two CJK characters and "Ab" | UTF-8 out |
| two CJK characters and an emoji, at most 3 | no half surrogate pair |
| "a1b2c3" -> "123" | number pad |
| "ab", minimum 3 | shorter than the minimum: cancelled |
| the window, "ok" | OK pressed |
| the window, "cancel" | Cancel pressed |

After the window the game reads nothing until everything is released; with nothing held one read lifts that.

### `probe_menu_gate()`

The menu's input gate: Minus+Plus is consumed, a menu that is open leaves the game a neutral pad. In order: Minus
alone, a frame before Plus, reaches the guest unchanged; both down: the guest sees neither, the menu toggles once;
still held with the menu open: no second toggle, a neutral pad; the menu is open and the pad is neutral whatever is
pressed; released, then pressed again toggles again; Plus released first while Minus is still down: Minus must not
leak, and a new Minus press after both are up passes.

### `probe_fonts()`

Shared fonts: the extracted files when present (the missing case is a fatal child).

### `probe_menu_text()`

The host catalog (`runtime/ui/ui_text.c`): every menu id has its own English, Russian, Brazilian Portuguese and
Simplified Chinese text, the software keyboard keeps its game-language rows, and `menu_text` follows
`NX2SARIA_MENU_LANG`.

- The description pane keeps the whole text and a note after it (`desc_set` of `runtime/ui/option_rows.cpp`).
- The Saves group's lines are formats of one path or detail (`rows_saves` of `runtime/ui/menu.cpp`): at most one
  "%s", nothing else.
- Other languages fall back to English for the menu, and keep their own keyboard texts.
- Language names: the same string in every row.
- The game language list: pt-BR only where the build ships USpt; the Switch variants are entries of their own.
- The overlay formats keep their conversions.
- A Brazilian game language gets the Brazilian keyboard row, Portugal's Portuguese keeps its own.
- `menu_text` follows the setting live.

### `run_child()` `probe_children()`

`run_child()` runs this executable again with the same command line and `NX2SARIA_PROBE_SERVICES_CHILD=<mode>`, its
stdout and stderr into `out`. It returns the exit code. In the "nvn" child `nvnBootstrapLoader` returns the synthetic
address of a proc (`runtime/nvn/nvn_procs.c`), 0 for an unknown name.

### `probe_services()`

Names bound by the loader: each override and observer probed here must match a relocation (1.2.1 lists no add-on
content, so `aoc.ListAddOnContent` may be unbound there). Every linked override is either probed here or belongs to
another probe or owner (`nn::fs`: the fs probe; fibers: the kernel probe; `movie::Decoder`: `--probe-movie`; memcpy:
the graphics write-back in `runtime/nvn/nvn_api_core.c`).

## `runtime/hle/amiibo.h`

The amiibo loader and state management subsystem (540-byte NTAG215 binary dumps). What the player sees, the keys
file, the write-back and the settings: [amiibo.md](../amiibo.md).

- `AMIIBO_APP_AREA_SIZE` (0xD8) is the bytes of game data a figure holds.
- `AmiiboDeviceState` corresponds to the DeviceState of `nn::nfp` / `nn::nfc`. `AmiiboInfo` is the figure's
  identification.
- The declarations are grouped: the subsystem's lifecycle (`amiibo_init()`); the device emulation's status
  (`amiibo_set_device_present()`); tag detection and its lifecycle (`amiibo_start_detection()`); tag mounting
  (`amiibo_mount()`); the state query; file loading and the actions that trigger a tap (`amiibo_load_file()`); the
  timer tick called during menu and frame updates (`amiibo_tick()`); the options (`amiibo_set_auto_remove()`); tag
  inspection (`amiibo_has_tag()`); and the extraction of the Horizon buffers, called by the nfp IPC service and the
  overrides (`amiibo_get_tag_info()`).
- `amiibo_get_reader_state()`: the state the menu shows: searching also within a second after a stop, since the game
  restarts detection many times a second while it waits for a tag. The game reads `amiibo_get_device_state()`.

The figure's own data: owner, nickname, write counts and the game's application area. It is kept per figure UID in
`<profile>/amiibo/<uid>.dat`; seeded from the dump when the keys decrypt it, and written back into the dump file
(encrypted and signed again) when `NX2SARIA_AMIIBO_WRITEBACK` is on and the keys read it.

- `AmiiboDataSource`: `AMIIBO_SOURCE_NEW` (0): nothing stored yet: defaults (registered, nickname from the file name,
  no area); `AMIIBO_SOURCE_SAVED` (1): `<profile>/amiibo/<uid>.dat`; `AMIIBO_SOURCE_DUMP` (2): the decrypted dump.
- `AmiiboFigureData`: `decrypted`: the keys decrypted and verified this dump; `registered`: owner and nickname set;
  `nickname[41]`: UTF-8, at most 10 characters; `app_area_size`: the bytes the game last wrote, at most
  `AMIIBO_APP_AREA_SIZE`.
- `amiibo_get_figure_data()`: a copy of the loaded figure's data. False without a loaded dump.
- `amiibo_erase_app_area()`: the menu's "erase game data": drops the application area and saves the figure. False
  without a loaded dump.
- `amiibo_reload_keys()` loads (or reloads) the keys from `NX2SARIA_AMIIBO_KEYS`, or `<amiibo folder>/key_retail.bin`
  when that is unset, and decrypts the loaded dump again. True when keys are loaded afterwards.

The `nn::nfp` application area calls (the overrides of `runtime/hle/ov_system.c`, IUser of `runtime/hle/nfp.c`): each
returns an `nn::nfp` result: 0 or one of these. Without a mounted figure every call returns
`AMIIBO_R_DEVICE_NOT_FOUND`.

| Result | Value | When |
|---|---|---|
| `AMIIBO_R_DEVICE_NOT_FOUND` | 0x8073 | no figure on the reader (module 115, description 64) |
| `AMIIBO_R_AREA_NOT_CREATED` | 0x10073 | the figure has no application area yet (description 128) |
| `AMIIBO_R_WRONG_ACCESS_ID` | 0x13073 | the application area belongs to another access id (description 152) |
| `AMIIBO_R_AREA_EXISTS` | 0x15073 | CreateApplicationArea on a figure that has one (description 168) |

- `amiibo_get_app_area()`: the application area cut to `cap` bytes into `out`; `*out_size` the bytes copied.
- `amiibo_set_app_area()` changes the application area in memory only; `amiibo_flush_figure()` stores it.
- `amiibo_create_app_area()`: create (`recreate` false: `AMIIBO_R_AREA_EXISTS` when there is one) or recreate an
  application area for `access_id` with `size` bytes of initial data, then stores the figure as
  `amiibo_flush_figure()` does.
- `amiibo_flush_figure()` counts and dates a write, saves the figure's data and, with `NX2SARIA_AMIIBO_WRITEBACK` on
  and a decrypted dump, writes it back into the dump file (encrypted and signed again; `<dump>.bak` first, once).

## `runtime/hle/amiibo.c`

The amiibo dump loader, the Horizon NFP state emulation and the figure's own data: owner, nickname, write counts and
the game's application area (`runtime/hle/amiibo.h`).

`AMIIBO_PROGRAM_ID` (0x0400486024dec000) is the game's title id, as `PROGRAM_ID` of `runtime/hle/svc.c` tells it.

Two locks, always taken in this order: `g_amiibo_io_lock`, then `g_amiibo_lock`, never the other way round.
`g_amiibo_lock` guards the state and is held for memory work only, so the menu thread's per-frame queries
(`amiibo_has_tag()`, `amiibo_get_reader_state()`) never wait for a disk. `g_amiibo_io_lock` serialises the keys,
decryption and encryption, loading a dump and the file writes of a commit, so two commits reach the disk in the order
they were made. `g_dump`, `g_has_dump` and `g_info` change only while both are held, so holding `g_amiibo_io_lock`
alone reads them.

- `g_dump` is the 540-byte raw NTAG215 binary dump.
- `g_fig` is the loaded figure's data (`amiibo_get_figure_data()`); `g_title_id` (the program that created the
  application area) and `g_app_write_counter` (the writes of the application area) are what `AmiiboFigureData` does
  not carry.
- `g_plain` and `g_fields`, while `g_fig.decrypted`: the decrypted dump and its fields, which each commit brings up to
  date with `g_fig` so that a write-back (`amiibo_flush_figure()`) stores the game's changes and keeps the rest of the
  tag as it was.
- `g_keys_tried` (under `g_amiibo_io_lock`): the keys file was read once (`keys_load_io()`).

### `amiibo_validate_dump()`

- The NTAG215 Capability Container (CC) at page 3 (offset 0x0C). An amiibo's is F1 10 FF EE, not the E1 of a plain
  NDEF tag; the CC is not encrypted, so encrypted dumps carry it too.
- UID sanity: the UID (bytes 0..2 and 4..7) cannot be all zeroes or all 0xFF.
- The ISO 14443-3 / NTAG215 Bit Check Character (BCC) verification: page 0 byte 3 (BCC0) = 0x88 ^ UID[0] ^ UID[1] ^
  UID[2]; page 2 byte 0 (BCC1) = UID[3] ^ UID[4] ^ UID[5] ^ UID[6].

### `copy_uid()` `parse_dump_info()`

`copy_uid()`: the 7-byte UID of an NTAG215 dump: bytes 0..2, then 4..7. Byte 3 is the check byte BCC0.
`parse_dump_info()`: the amiibo ID at offset 0x54 (8 bytes, not encrypted): character ID and variant, figure type,
model number (big-endian), series, format version 02.

### `today()` `default_nickname()` `dat_paths()`

- `today()`: today's date on the host clock, as the figure stores it; the year is kept to 2000..2127, the years a
  figure's date field holds.
- `default_nickname()`: the nickname of a figure without one: the dump's file name without its extension, at most 10
  characters (UTF-8, cut on a character boundary: a character is its lead byte, then its continuation bytes) and 40
  bytes.
- `dat_paths()`: `<profile>/amiibo` and `<profile>/amiibo/<UID as 14 lower-case hex digits>.dat`. False without a
  profile folder or when a path does not fit.

### `dat_build()` `dat_parse()`

The .dat file: one figure's data, little-endian (`DAT_VERSION` 1):

| Offset | Field |
|---|---|
| 0x00 | "NXAD" |
| 0x04 | u32 version (1) |
| 0x08 | UID (7 bytes) |
| 0x0F | flags: 1 registered, 2 application area |
| 0x10 | u16 write counter |
| 0x12 | setup date: u16 year, u8 month, u8 day |
| 0x16 | last write date, the same layout |
| 0x1A | u16 application area writes |
| 0x1C | u32 access id |
| 0x20 | u32 application area bytes written |
| 0x24 | u32 0 |
| 0x28 | u64 title id of the program that created the application area |
| 0x30 | nickname, UTF-8, NUL-padded (48 bytes) |
| 0x60 | application area (`AMIIBO_APP_AREA_SIZE` bytes) |

`dat_build()` holds `g_amiibo_lock`. `dat_parse()`: a .dat file into `g_fig` (all but `source` and `decrypted`),
`g_title_id` and `g_app_write_counter`. False, with nothing changed, when it is not a .dat of this version or belongs
to another UID. Holding `g_amiibo_lock`.

### `seed_from_fields()` `figure_load_locked()`

`seed_from_fields()`: `g_fig` from the decrypted fields in `g_fields`. Holding `g_amiibo_lock`.
`figure_load_locked()`: the loaded figure's data, in the order of `amiibo.h`: the .dat in the profile, else the
decrypted dump, else defaults (registered, nickname from the file name, today's date, no application area). `g_dump`,
`g_info` and, when `dec`, `g_plain` and `g_fields` are set. Holding `g_amiibo_lock`.

### `writeback_on()` `keys_load_io()`

`writeback_on()`: `NX2SARIA_AMIIBO_WRITEBACK`: "1" writes the game's changes back into the dump file. It is read
through `cfg_get`, which needs no settings-table row and takes no lock.

`keys_load_io()`: the keys file: `NX2SARIA_AMIIBO_KEYS`, else `<NX2SARIA_AMIIBO_DIR, or ./amiibo>/key_retail.bin`. The
keys loaded before are dropped either way (`amiibo_keys_load`, which logs what it found). Holding
`g_amiibo_io_lock`, not `g_amiibo_lock`.

### `commit_prepare_locked()` `commit_write_io()`

`AmiiboCommit` is what a commit writes, gathered under `g_amiibo_lock` and written after it is released: `dat` and
the .dat's folder and path; `dump` and the encrypted dump, to `dump_path`.

`commit_prepare_locked()`: a write to the figure (Flush, Create, Recreate, the erase): counts it, dates it, brings
`g_fields` up to date and gathers the files: always the .dat; with writeback, when the keys read the dump, the dump
encrypted again (`g_dump` takes it too). Holding both locks. `commit_write_io()` writes the files of a commit, holding
`g_amiibo_io_lock`, not `g_amiibo_lock`. The dump as it is on disk goes to `<dump>.bak` once, before the first
write-back changes it.

### `amiibo_start_detection()` `amiibo_stop_detection()`

While the game waits for a tag it stops and starts detection again several times a second (tens with the menu open),
so the log names the first few and then a count every 200th start. It also stops detection whenever it finds the
reader active (GetDeviceState 1 to 4: `main+0xeb8be0` and others), several times a frame, so stops are counted on
their own and named at powers of two only: a state that kept the reader active once logged 2.2 million stops in
seconds. `g_detect_stop_ns` is the last stop: the menu holds "searching" across the game's restarts.

A new detection clears `g_mounted`: it finds a figure left on the reader again, and the game mounts it again.

### `amiibo_get_device_state()`

As `nn::nfp`: found and mounted only while detection runs; StopDetection leaves the reader Initialized with a figure
still on it. The game stops detection after it read a figure and again each time it finds the reader in any other
state, so a figure that stayed mounted after the stop kept a game thread stopping it until the figure left the reader
(auto-remove, 2.5 s by default; for good with auto-remove off). The figure's data stays readable after the stop
(`g_mounted` is cleared at the next start), as before.

### `amiibo_load_file()`

The figure's saved data is read after any commit of it has reached the disk. The first dump loads the keys: the
settings are read by then. The static `plain` and `fields` are under `g_amiibo_io_lock`.

### `amiibo_get_tag_info()` `amiibo_get_model_info()` `amiibo_get_common_info()` `amiibo_get_register_info()`

- TagInfo: the UUID at offset 0x00 (10 bytes at most), its length (7) at 0x0A, the protocol at 0x20 (TypeA = 1), the
  tag type at 0x24 (Type2 / NTAG215 = 2).
- `nn::nfp::ModelInfo` from the amiibo ID at 0x54: character ID and variant in the tag's byte order (bytes 0-2),
  series at 3, model number little-endian at 4 (big-endian on the tag), figure type at 6. The game tells figures of
  one character apart by the model number.
- `nn::nfp::CommonInfo` (0x40 bytes): last write date {u16 year, u8 month, u8 day} at 0, write counter u16 at 4,
  figure format version at 6, application area size u32 at 8; little-endian.
- `nn::nfp::RegisterInfo` (0x100 bytes): the owner's Mii as a CharInfo at 0 (0x58 bytes), the setup date {u16 year, u8
  month, u8 day} at 0x58, the nickname UTF-8 NUL-terminated at 0x5C (up to 0x29 bytes), font region at 0x85. The Mii
  is a minimal default CharInfo: the tag's own Mii is not converted. A figure without an owner reports the nickname a
  new one gets (`default_nickname()`). An unregistered tag stores date 0 (2000-00-00), for which today's date is
  given.

### `amiibo_reload_keys()`

`g_dump` and `g_has_dump` change only under `g_amiibo_io_lock`, which this holds, so it reads them without
`g_amiibo_lock`. A figure with nothing of it stored yet (`AMIIBO_SOURCE_NEW`) is seeded with the tag's own data.

### `amiibo_get_app_area()` `amiibo_set_app_area()`

`amiibo_get_app_area()` gives the whole area, cut to the caller's buffer: the game checks that the size it gets is the
size it asked for. In `amiibo_set_app_area()` the bytes past the new data read back as zero.

## `runtime/hle/amiibo_crypto.h`

Amiibo tag encryption: the keys file the player supplies, decrypting a 540-byte NTAG215 dump and checking its two
signatures, and encrypting and signing it again after the game changed it (`runtime/hle/amiibo.c`
`amiibo_flush_figure()`).

- `AMIIBO_KEYS_SIZE` (160): two 80-byte master keys: the data key, then the tag key.
- `amiibo_keys_load()` loads the keys file. False when it is missing, has another size or fails the format check; the
  keys loaded before are dropped either way. Key bytes are never logged, printed or written anywhere.
- `AmiiboPlain` is a decrypted dump. Its layout is private to `runtime/hle/amiibo_crypto.c`; it is read and changed
  through `amiibo_plain_read()` and `amiibo_plain_write()`.
- `amiibo_decrypt()` decrypts `tag` into `out` and checks both signatures. False without keys or when a signature
  does not match: the figure is then used without its stored data (UID and model only).
- `amiibo_encrypt()` encrypts and signs `plain` into `tag`. False without keys.
- `AmiiboFields`: the fields of a decrypted figure that `nn::nfp` reports or the game writes: `flags` (`AMIIBO_FLAG_*`:
  `AMIIBO_FLAG_REGISTERED` 0x10, owner and nickname set; `AMIIBO_FLAG_APP_AREA` 0x20, an application area exists);
  `write_counter`, the figure's writes; the setup date (owner registration); the last write date; `nickname[41]`,
  UTF-8 (at most 10 characters), NUL-terminated; `mii[0x60]`, the owner's Mii as the tag stores it; `title_id`, the
  program that created the application area; `app_write_counter`, the writes of the application area; `access_id`,
  the application area's owner.
- `amiibo_plain_write()` stores every field of `in` (the nickname encoded again) and leaves the rest of `p` as it
  was.
- `amiibo_crypto_selftest()`: known-answer tests of AES-128, HMAC-SHA256 and a decrypt/encrypt round trip with
  made-up keys. It logs the first failure and returns false. Called by `runtime/hle/ov_probe.c`.

## `runtime/hle/amiibo_crypto.c`

Amiibo tag encryption (`runtime/hle/amiibo_crypto.h`): the player's keys file, decrypting and verifying a 540-byte
NTAG215 dump, and signing and encrypting it again after `runtime/hle/amiibo.c` changed the figure's data. The
repository holds no key: the keys come from the file the player supplies ([amiibo.md](../amiibo.md#keys-file)).

Two 80-byte master keys make the keys file: the data key, which encrypts and signs the figure's data, then the tag
key, which signs the parts of the tag that never change. Neither is used directly. Each dump derives its own AES key,
AES counter and HMAC key from each master key and from the tag's UID, salt and (for the data key) write counter, so
the same figure data encrypts differently on every tag and after every write.

An `AmiiboPlain` keeps the tag's own byte layout with the two encrypted regions decrypted in place: everything outside
them, the two signatures included, sits where the tag has it. Encrypting a plain image that nobody changed gives back
the dump it came from, byte for byte.

Key bytes, derived keys and decrypted data never reach the log. Buffers that held them are wiped before they go out
of scope (`wipe()`, which the compiler cannot drop as a dead store).

### The layouts

One master key, 80 bytes:

| Offset | Field |
|---|---|
| 0x00 `MK_HMAC_KEY` | 16 bytes: the key of the derivation HMAC |
| 0x10 `MK_TYPE` | 14 bytes: a NUL-terminated name; it and its NUL start every derivation seed |
| 0x1E `MK_RESERVED` | 1 byte, unused |
| 0x1F `MK_MAGIC_SIZE` | how many of the 16 magic bytes the seed takes, at most 16 |
| 0x20 `MK_MAGIC` | 16 bytes |
| 0x30 `MK_XOR_PAD` | 32 bytes, XORed with the tag's salt in the seed |

The tag (and the plain image, which has the same layout):

| Offset | Field |
|---|---|
| 0x000 `T_UID` | 8 bytes: UID bytes 0-2, check byte 0, UID bytes 3-6; signed and part of the seed |
| 0x010 `T_HEADER` | 0xA5, not signed |
| 0x011 `T_COUNTER` | u16 BE: writes of the figure. Seeds the data key, so each write re-keys the data |
| 0x013 `T_VERSION` | format version, 0 |
| 0x014 `T_SETTINGS` | the first encrypted region, 0x20 bytes: |
| 0x014 `T_FLAGS` | `AMIIBO_FLAG_*` in bits 4-5, the font region in bits 0-3 |
| 0x015 `T_COUNTRY` | country code of the console that registered the owner |
| 0x016 `T_CRC_COUNTER` | u16 BE: changes of the console checksum below |
| 0x018 `T_SETUP_DATE` | packed date, u16 BE: owner registration |
| 0x01A `T_WRITE_DATE` | packed date, u16 BE: last write |
| 0x01C `T_CONSOLE_CRC` | u32 BE checksum of the last console that wrote the figure |
| 0x020 `T_NICKNAME` | 10 UTF-16 BE code units, NUL-padded, not terminated when all 10 are used |
| 0x034 `T_TAG_HMAC` | 32 bytes: HMAC of the tag key over UID, model info and salt |
| 0x054 `T_MODEL` | 8 bytes model info, then 4 more signed bytes; part of the static signed block |
| 0x060 `T_SALT` | 32 bytes chosen at manufacture; seeds both keys |
| 0x080 `T_DATA_HMAC` | 32 bytes: HMAC of the data key over the header, the plain data and the static block |
| 0x0A0 `T_DATA` | the second encrypted region, 0x168 bytes, up to 0x208: |
| 0x0A0 `T_MII` | 0x60 bytes: the owner's Mii and its checksum, kept raw |
| 0x100 `T_TITLE_ID` | u64 BE: the program that created the application area |
| 0x108 `T_APP_COUNTER` | u16 BE: writes of the application area |
| 0x10A `T_ACCESS_ID` | u32 BE: the owner of the application area |
| 0x130 `T_APP_AREA` | `AMIIBO_APP_AREA_SIZE` bytes of game data |
| 0x208 `T_DATA_END` | dynamic lock bytes and the NTAG215 configuration follow, not signed |

`STATIC_SIZE` is model info, 4 bytes and the salt: 0x2C. `TAG_SIGNED_SIZE` is 0x34. `DATA_SIGNED_SIZE` is 0x1DF.
`g_keys_gen` is bumped by every change of the keys, so the self-test restores only what it replaced.

### `same()`

Equality without an early exit, so the time taken does not tell how many leading bytes of a signature matched.

### `keys_copy()`

A private copy of the keys, so the crypto runs outside the lock. False without keys.

### `base_seed()` `derive()`

`base_seed()`: 64 bytes that tie the derived keys to this tag: the write counter, 14 zero bytes, the UID page twice,
the salt.

`derive()`: the seed is the master key's type string with its NUL, the first 16 - magic size bytes of the base seed
followed by the magic bytes (16 bytes together), base seed bytes 0x10-0x1F, and base seed bytes 0x20-0x3F XOR the pad.
Two blocks of HMAC(master HMAC key, u16 BE block number || seed) give 64 bytes, of which the first 48 are the derived
AES key, AES counter and HMAC key. A type string without its NUL or a magic size over 16 gives a zeroed key:
`amiibo_keys_load()` refuses such keys; the test only keeps the copies in bounds.

### `crypt_regions()` `sign_tag()` `sign_data()`

- `crypt_regions()`: AES-128-CTR over both encrypted regions as one stream: the settings, then the data. Its own
  inverse.
- `sign_tag()`: the tag signature: UID page, then model info, the 4 bytes after it and the salt.
- `sign_data()`: the data signature over the plain image: write counter, version and settings, the data, the tag
  signature, then the tag-signed block (UID page, model info, salt).

### `decrypt_with()` `encrypt_with()`

`decrypt_with()` copies the tag before `out` is cleared: `out` may be the tag itself. `encrypt_with()`: the tag
signature first, since the data signature covers it; both over the plain bytes; then the encryption.

### `date_unpack()` `date_pack()`

Packed date: years since 2000 in bits 9-15, month in bits 5-8, day in bits 0-4.

### `utf8_put()` `utf8_get()` `nickname_decode()` `nickname_encode()`

- `utf8_put()` appends `cp` as UTF-8 when it fits with room left for the NUL; it returns the new length (`n` when it
  did not fit).
- `utf8_get()`: one code point from the `n` bytes at `s` (n >= 1); it returns the bytes it took. Malformed input reads
  as U+FFFD: a bad lead or continuation byte takes 1 byte, a well-formed overlong, surrogate or out-of-range sequence
  takes all its bytes.
- `nickname_decode()`: 10 UTF-16 BE code units, up to the first zero unit, as NUL-terminated UTF-8. A surrogate
  without its partner reads as U+FFFD. `cap` >= 31 always holds the whole name (at most 3 bytes per unit).
- `nickname_encode()`: UTF-8 (up to `cap` bytes or the NUL) as 10 UTF-16 BE code units, zero-padded. Characters past
  the 10th unit are dropped, and a character needing a surrogate pair is dropped whole when only one unit is left.

### `amiibo_plain_write()`

In the flags byte the font region and other bits stay.

### The self-test

Every key in the self-test is made up from a constant pattern.

- `selftest_primitives()`: FIPS-197 appendix C.1; SP 800-38A F.5.1, CTR-AES128 (the counter carries out of the last
  byte between blocks 1 and 2); key 10 11 .. 1f, counter ff .. ff fe, plaintext 00 01 .. 27 (2.5 blocks): the counter
  wraps through all 16 bytes to zero after the second block; RFC 4231 test cases 2 (short key) and 6 (131-byte key,
  hashed first). The CTR test also runs in place, and is its own inverse.
- `selftest_keys()`: two made-up master keys: a pattern, with type strings of two lengths and both magic sizes in use
  (14 for the data key, so the write counter enters its seed; 16 for the tag key).
- `selftest_fields()`: the nickname is ASCII, a 2-byte and a 4-byte character.
- `field_byte()`: the bytes `amiibo_plain_write()` may change.
- `selftest_plain()` plants a lone high surrogate, then 'A'.
- `selftest_tag()`: the format with explicit keys: a known answer computed by an independent model of the format,
  round trips, and single-byte changes that must break a signature: a byte in each of data (0x150), UID (0x003), tag
  signature (0x040), write counter (0x011), salt (0x060), settings (0x014) and model info (0x055).
- `keys_swap()` replaces the loaded keys with `k` (`have` false: none) unless another caller changed them since `*gen`
  was taken; then `*gen` is the new generation. False when the keys changed meanwhile, and nothing is replaced.
- `selftest_public()`: the public calls: without keys, then with the made-up keys, then the player's keys go back.
  While the made-up keys are in place (well under a millisecond) a dump decrypted on another thread would fail and
  load without its data; the caller, `runtime/hle/ov_probe.c` under `--probe-services`, runs no game code that loads
  one.

## `runtime/hle/nfp.h`

The Nintendo Figurine Platform (`nfp:user`) IPC service emulation. `nfp_user_manager_handler()` is `sm:`'s handler for
"nfp:user" (`nn::nfp::detail::IUserManager`).

## `runtime/hle/nfp.c`

The `nfp:user` IPC service (`nn::nfp::detail::IUserManager` and IUser). `g_device_handle` is the simulated NFC device
handle: id 1. The three events of `NfpUserState` are writable handles.

### `nfp_user_handler()`

The handler for `nn::nfp::detail::IUser`:

| Command | Call |
|---|---|
| 0 | InitializeNfp(u64 aruid, u64 m_pid, pid, buffer<unknown, 5>) |
| 1 | FinalizeNfp() |
| 2 | ListDevicesNfp() -> (s32 total_out, buffer<DeviceHandle, 0xa>) |
| 3 | StartDetectionNfp(DeviceHandle) |
| 4 | StopDetectionNfp(DeviceHandle) |
| 5 | MountNfp(DeviceHandle, u32 ModelType, u32 MountTarget); 0x8073 (device or tag not found) without a tag |
| 6 | UnmountNfp(DeviceHandle) |
| 7 | OpenApplicationAreaNfp(DeviceHandle, u32 access_id) |
| 8 | GetApplicationAreaNfp(DeviceHandle) -> (u32 size, buffer<data, 0x6>) |
| 9 | SetApplicationAreaNfp(DeviceHandle, buffer<data, 0x5>) |
| 10 | FlushNfp(DeviceHandle) |
| 11 | RestoreNfp(DeviceHandle): a figure here is never damaged |
| 12 | CreateApplicationAreaNfp(DeviceHandle, u32 access_id, buffer<initial data, 0x5>) |
| 13 | GetTagInfoNfp(DeviceHandle) -> buffer<TagInfo, 0x1a> |
| 14 | GetRegisterInfoNfp(DeviceHandle) -> buffer<RegisterInfo, 0x1a> |
| 15 | GetCommonInfoNfp(DeviceHandle) -> buffer<CommonInfo, 0x1a> |
| 16 | GetModelInfoNfp(DeviceHandle) -> buffer<ModelInfo, 0x1a> |
| 17 | GetActivateEventHandleNfp(DeviceHandle) -> handle<copy> |
| 18 | GetDeactivateEventHandleNfp(DeviceHandle) -> handle<copy> |
| 19 | GetStateNfp() -> u32 state: 1, Initialized |
| 20 | GetDeviceStateNfp(DeviceHandle) -> u32 device_state |
| 21 | GetNpadIdNfp(DeviceHandle) -> u32 npad_id: 0, Handheld / Player 1 |
| 22 | GetApplicationAreaSizeNfp(DeviceHandle) -> u32 size (0xD8) |
| 23 | GetAvailabilityChangeEventHandleNfp() -> handle<copy> |
| 24 | RecreateApplicationAreaNfp(DeviceHandle, u32 access_id, buffer<initial data, 0x5>) |

The application area calls share `runtime/hle/amiibo.c`'s backend with the `nn::nfp` overrides
(`runtime/hle/ov_system.c`).

### `nfp_user_manager_handler()`

The handler for `nn::nfp::detail::IUserManager` (`nfp:user`). Command 0 is CreateUserInterface() -> a move handle to
IUser. The availability event is signalled immediately, so the game detects the reader.

## `runtime/hle/swkbd.h`

`nn::swkbd::ShowKeyboard` (`runtime/hle/swkbd.c`) and its front end, the window that takes the text
(`runtime/hle/swkbd_win.c` with Win32, `runtime/hle/swkbd_sdl.c` with SDL).

- `KB_LIMIT` (500) is the applet's longest text.
- `SwkbdRequest` is the request, front-end independent: `prompt[0x101]` is the header, else the guide, else the sub
  text (UTF-16, NUL-terminated; may be empty); `initial` the starting text (the game's, else the catalog's default
  name); `game_initial` says the starting text is the game's.
- `text_entry_run()` shows the text entry and waits for the answer, on the guest thread that called (its virtual core
  given up, the game's input blocked). It returns 1 with the text in `out` (`n` units), 0 when cancelled.
- `swkbd_utf16_to_utf8()`: UTF-16 (`n` units) -> NUL-terminated UTF-8 in `cap` bytes, cut at a character boundary; it
  returns the bytes written.
- `swkbd_utf8_to_utf16()`: UTF-8 -> UTF-16, at most `cap` units (a character that does not fit whole is dropped); bad
  bytes become U+FFFD.
- `swkbd_allowed()`: whether the applet would let this character through (single UTF-16 units; pairs are always
  allowed). Disabled-key bits as the applet's: 1 space, 2 '@', 3 '%', 4 '/', 5 '\', 6 digits.

## `runtime/hle/swkbd.c`

`nn::swkbd::ShowKeyboard`: text entry for the game. Its first line is a licence notice. On the Switch the call starts
the keyboard library applet, which takes the screen until the player confirms or cancels. The game calls it in one
place, naming a horse at a stable (`main+0x1ecbf64` -> `main+0x1b2b3a0`: UserName preset, at most 9 characters, '@',
'%' and '\' disabled, UTF-16 out, no header, guide or initial text; an empty answer counts as not named).

The text entry is a window of the host. Front end: `text_entry_run()` (`runtime/hle/swkbd.h`; `swkbd_win.c` with
Win32, `swkbd_sdl.c` with SDL), run on the guest thread that called (the call blocks, as on the Switch). The thread
gives its virtual core up meanwhile, so the other guest threads keep running; it holds no lock across the wait. The
text starts as the game's initial text, else a default name from the catalog (`runtime/ui/ui_text.c`), selected, so a
controller can confirm it: A = OK, B = cancel (each after it was seen released); Enter / Esc and the buttons work too.
While the window is open, and after it closes until everything is released, the game reads no input (`input_block`).
`text_entry_run()` is the only front-end seam: an overlay (ImGui) can replace it.

Environment:

| Setting | What |
|---|---|
| `NX2SARIA_SWKBD_ANSWER=<text>\|<cancel>\|<initial>` | answer without a window (probes, scripted runs); `<initial>` keeps the starting text (the game's, else the default name) |
| `NX2SARIA_SWKBD_TEST=ok\|cancel` | open the window, then press OK / Cancel on the first timer tick |

`ARG_WORK`: the argument is KeyboardConfig then the work buffer {pointer, size}: the offsets are read from the SDK
setters and checked against the SDK by `--probe-services`. SDK 20.5 (1.4.3) put a text-check callback at +0x4c8 and
grew KeyboardConfig from 0x4d0 to 0x518 bytes (SetInitialText and SetUserWordList read the work buffer there); the
fields below +0x3d8 stay.

### `clean()`

Drops disallowed characters and keeps at most `max` units without splitting a surrogate pair.

### `ov_show_keyboard()`

`Result ShowKeyboard(String *out, const ShowKeyboardArg &arg)`; String = {pointer, byte size}. One keyboard at a
time (the applet is modal too). Of the output's units one is kept for the terminator.

## `runtime/hle/swkbd_win.c`

The Win32 front end of the software keyboard (`runtime/hle/swkbd.h`): a modal window owned by the game window, run
on the guest thread that called. The text starts as the game's initial text, else a default name from the catalog
(`runtime/ui/ui_text.c`), selected, so a controller can confirm it: A = OK, B = cancel (each after it was seen
released); Enter / Esc and the buttons work too. `NX2SARIA_SWKBD_TEST=ok|cancel` opens the window, then presses OK /
Cancel on the first timer tick. Only the Windows build compiles this file.

`B_A` (0), `B_B` (1) and `B_PLUS` (10) are the bits of `runtime/input/input.h`. `Entry`: `result` is 0 open, 1 OK, -1
cancelled; `armed` the buttons seen released since the window opened; `test` is `NX2SARIA_SWKBD_TEST`: 1 press OK, -1
press Cancel.

### `entry_proc()`

A button counts as pressed when it is down now and was up at the previous tick.

### `text_entry_run()`

Shows the window and waits for the answer. It returns 1 with the text in `out` (`n` units), 0 when cancelled.

- The process is per-monitor DPI aware: the font and the layout are measured at the game window's DPI, the monitor the
  dialog opens on, not at the system DPI that plain SystemParametersInfo and AdjustWindowRectEx use.
- Enter on the Cancel button cancels; elsewhere it confirms.
- The owner is enabled again before the window goes, so activation returns to the game window.

## `runtime/hle/swkbd_sdl.c`

The software keyboard's front end for SDL windows (`runtime/hle/swkbd.h`). Until the in-game text field exists (an
ImGui entry in the menu overlay, which SDL_StartTextInput can pair with the Steam Deck's on-screen keyboard), the text
entry takes the starting text at once, as `NX2SARIA_SWKBD_ANSWER=<initial>` does: the game's initial text, else the
default name from the catalog. The game asks only for a horse's name. `NX2SARIA_SWKBD_TEST=cancel` stands for Cancel
pressed, as in the Win32 window.

## `runtime/hle/net_offline.c`

The offline network: `nn::socket` and the `nnsocket*` C API for a title that brings its network stack up at boot
(Switch 2 titles start it on a worker thread and abort if `nn::socket::Initialize` fails). This build has no network:
Initialize succeeds, no socket can be made (ENETDOWN), name lookups fail (EAI_NONAME), and every other call on a
descriptor that is not an eventfd fails with EBADF. eventfd is real: the stack's worker threads wake each other through
it and through nnsocketPoll, so there is a counter with a kernel event each, and waits are made with
WaitSynchronization on the calling guest thread (SVC 0x18 through `kern_svc`, as the SDK's own wait would). errno
values are the BSD ones the SDK uses (`nn::socket::Errno`).

- `NIFM_R_COMMUNICATION_DISABLED_NET` (0x8ae6e) is nifm module 110, description 1111.
- `EFD_BASE` (0x4000): eventfd descriptors are above anything a guest socket table would hand out.
- `spare[]`: the kernel events of closed eventfds, reused by the next EventFd. They are never closed: a thread can
  still be blocked on one (EventFdRead, nnsocketPoll) when another closes the descriptor, and a freed handle number is
  soon reused by another kernel object, which corrupted that thread's wait (round 15 crashes).
- `last_errno` is one value for all threads: nothing here succeeds partially.

### `guest_wait()`

WaitSynchronization(handles, n, ns) on the calling guest thread. It returns the result; `*index` is the signalled
one.

### The socket calls

| Function | SDK call | Note |
|---|---|---|
| `ov_initialize()`, `ov_finalize()`, `ov_is_initialized()` | `Result Initialize(const Config &)` / `Result Finalize()` / `bool IsInitialized()` | |
| `ov_eventfd()` | `int EventFd(uint64_t initval, EventFdFlags flags)` | |
| `ov_eventfd_write()` | `int EventFdWrite(int fd, uint64_t value)` | |
| `ov_eventfd_read()` | `int EventFdRead(int fd, uint64_t *value)` | blocks while the counter is 0 (no nonblocking flag is honoured yet) |
| `ov_badf()` | `int SetSockOpt(int, Level, Option, const void *, uint32_t)` | EBADF |
| `ov_close()` | nnsocketClose | the closed eventfd's event is signalled to wake its waiters: they find the descriptor gone (EBADF) or look again |
| `ov_poll()` | `int nnsocketPoll(pollfd *fds, nfds_t n, int timeout_ms)` | eventfds report POLLIN while their counter is nonzero; any other descriptor reports POLLNVAL. With nothing to wait on and a timeout it sleeps (SVC 0x0B, SleepThread); with nothing to wait on, forever, it reports none instead of hanging |
| `ov_getaddrinfo()` | `int nnsocketGetAddrInfo(...)` / `...WithOptions(...)` | no name resolves offline |

### The ssl calls

`nn::ssl`: initialised, never used (no socket connects). Contexts are created and accept imports; the built-in CA
list is empty: GetBuiltInCertificateBufSize asks for room for one zeroed (terminating) record (`SSL_EMPTY_LIST`, 0x40)
and GetBuiltInCertificates hands back that zeroed record.

- `ov_ssl_ca_size()`: `Result BuiltInManager::GetBuiltInCertificateBufSize(uint32_t *out, CaCertificateId *ids,
  uint32_t count)`.
- `ov_ssl_ca_list()`: `Result BuiltInManager::GetBuiltInCertificates(BuiltInCertificateInfoBase **out, uint8_t *buf,
  uint32_t size, CaCertificateId *ids, uint32_t count)`.
- `ov_ssl_import()`: `Result Context::Import{ClientPki,ServerPki,Crl}(uint64_t *id, ...)`: an id for the caller to
  keep.

### `ov_nsd_environment()` `ov_nsd_resolve()`

`nn::nsd`: the server environment is production ("lp1", as the console answers); `ResolveEx(Fqdn *out, const Fqdn
&in)` copies the 0x100-byte name (nothing resolves offline anyway).

### The asynchronous tasks

`nn::time::AsyncEnsureNetworkClockAvailabilityTask` and the `nn::account` asynchronous NSA calls. The SDK versions run
as `nn::async` tasks over the time and account service objects, which the host never creates (it overrides
`nn::time::Initialize` and `nn::account::Initialize`); on the executor thread that corrupted memory at random (round
15 crashes). Here a task completes at once and reports the network as unavailable. The object's first 8 bytes hold the
host record (the task's real size is unknown; every SDK object is larger than a pointer).

`ACCOUNT_R_NSA_UNAVAILABLE` (0x1907c) is account module 124, description 200: a failure the caller handles.
`HostTask.event` (0x40 bytes reserved) is an `nn::os::SystemEventType` (0x30 bytes).

| Function | SDK call | Note |
|---|---|---|
| `ov_clock_task_ctor()`, `ov_task_dtor()` | `AsyncEnsureNetworkClockAvailabilityTask()` (C1) and its destructor | the record. The event lives in the record: its entry in the system-event table goes back before the record is freed, or the table would count it live for the rest of the run (`runtime/hle/kernel.c` `kern_unbind_system_event()`) |
| `ov_clock_task_init()`, `ov_clock_task_start()` | `Result Initialize(EventClearMode)` / `Result StartTask()` | done at once |
| `ov_clock_task_event()` | `SystemEvent *GetFinishEvent()` | `nn::os::SystemEvent` holds its SystemEventType at offset 0 |
| `ov_account_async()`, `ov_account_event()`, `ov_account_result()` | `AsyncContext()` / `~AsyncContext()`; `Result EnsureNetworkServiceAccountIdTokenCacheAsync(AsyncContext *, const UserHandle &)`; `Result AsyncContext::GetSystemEvent(SystemEvent *out)`; `Result AsyncContext::GetResult()` | `out` becomes a signalled event |
| `ov_account_nsa()` | `Result EnsureNetworkServiceAccountAvailable(const UserHandle &)`, `GetNetworkServiceAccountId(NsaId *, handle)`, `LoadNetworkServiceAccountIdTokenCache(size_t *, char *, size_t, handle)` | no network service account offline |
