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
