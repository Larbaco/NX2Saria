# runtime/platform

The platform layer: threads, time, locks, atomics, virtual memory, files, processes, sockets and the machine's load
counters. Runtime code calls this instead of the OS, and there is one implementation per OS: `runtime/platform/win32.c`
and `runtime/platform/linux.c`, with the parts built on the layer's own locks in `runtime/platform/common.c`. The
floating-point control register is in the per-architecture header `runtime/platform/fp_x86.h`, inline, because lifted
floating-point code touches it on every operation. Where the layer sits in the host: the bullet "OS calls the host
makes" in [architecture.md](../architecture.md), and `tools/check_platform.py`, which fails when runtime code outside
this folder uses the Win32 API directly. What the Linux build is and how far it is:
[building.md](../building.md#linux-in-progress), which also explains guest memory as dma-bufs (`NX2SARIA_DMABUF`) and
what it measured; the launcher's check of `/dev/udmabuf` is in [launcher.md](../launcher.md#linux-guest-memory). The log
lines this folder writes itself: `platform:` and `exit:` in [logging.md](../logging.md), and the raw `[host] crash_veh:`
and `[host] crash_filter:` notes of a crash on Windows. The checks that drive this code:
[building.md](../building.md#checks).

## `runtime/platform/platform.h`

The interface. Its declarations are grouped as below; a note that holds for one OS only is under that OS's file.
The header includes `runtime/platform/fp_x86.h` and `immintrin.h` on x86-64.

### `plat_stack_guarantee()`

Keeps `PLAT_STACK_GUARANTEE` (32768) bytes of the calling thread's (or fiber's) stack for its own stack overflow: the
crash report then runs on them. The threads and fibers made here get it at their start; `runtime/main.c` calls it for
its thread. Why 32 KiB: `runtime/platform/win32.c` `plat_stack_guarantee()`.

### `plat_thread_new()`, `plat_thread_new_ex()`, `plat_thread_spawn()`, `plat_thread_name()`

`plat_thread_new()` starts `fn(arg)` on a new thread named `name` (shown in debuggers and profilers; may be NULL);
NULL on failure. `plat_thread_new_ex()` is the same with the stack size given: `stack` bytes reserved for the thread
(0: `PLAT_STACK_DEFAULT`, `16u << 20`). The default is the same on every OS, so a thread that fits on Windows fits
elsewhere. `plat_thread_spawn()` starts a thread nobody joins (`plat_thread_new_ex()` and `plat_thread_detach()`); 1 on
success. `plat_thread_name()`: the name the calling thread was made with here, NULL for a thread made elsewhere; it is
kept in the thread itself, so the crash report can read it with any lock held.

### `plat_thread_join()`, `plat_thread_join_ns()`, `plat_thread_detach()`

`plat_thread_join()` waits for `fn` to return, then frees `t`. `plat_thread_join_ns()` waits at most `ns` for `fn` to
return: 1 when it did (`t` is freed), 0 when it still runs (`t` stays valid). `plat_thread_detach()` frees `t`; the
thread runs on.

### `plat_thread_end_hook()`, `plat_thread_end_run()`

`fn` runs on every thread these calls make, when its function has returned and before the thread ends: for what a
thread keeps for itself alone (a decoder context). At most `PLAT_THREAD_END_HOOKS` (4), each added once. The platform
files' thread start functions call `plat_thread_end_run()`.

### `plat_thread_affinity()`, `plat_process_affinity()`

`plat_thread_affinity()` restricts `t` (NULL: the calling thread) to the logical processors in `mask` (bit n =
processor n of the first group) and returns the previous mask, 0 on failure. `plat_process_affinity()` does it for
every thread of the process; 1 on success. On Linux the core `plat_cpu_reserve()` took stays out of the mask for every
thread but its holder, which keeps the processors it has.

### `plat_cpu_reserve()`, `plat_cpu_reserve_sweep()`

The calling thread gets a fast core to itself: the process's other threads are kept off it and the caller on it, with
both logical processors of the core when it has two; threads the caller starts later run with the others. Windows takes
the highest-numbered core of the fastest efficiency class (CPU sets). Linux takes the core of a hybrid CPU's
performance cores that clocks highest, else of the highest capacity or clock, and none when fewer than four cores are
that fast. It returns the logical processors reserved, 0 when it could not; `*which` (when not NULL, `size` bytes)
receives them as text, such as "10,11" (Linux adds what makes the core fast: "10,11 of a performance core").

`plat_cpu_reserve_sweep()` keeps the reserved core reserved: on Linux a thread the holder starts through a library
inherits its mask, the core alone, and gets the others' processors here; once the holder has ended, the core goes back
to every thread. It returns the threads moved, -1 when the core went back. Windows needs none of it (threads without CPU
sets of their own run on the process's default sets, which leave the core out) and returns 0.

### `plat_cpu_count()`, `plat_cpu_usable()`, `plat_thread_cpu_ns()`, `plat_cpu_cores()`, `plat_cpu_cache_die()`

- `plat_cpu_count()`: the logical processors of the machine (system info).
- `plat_cpu_usable()`: the ones this process may run on (its affinity mask): thread pools are sized by it.
- `plat_thread_cpu_ns()`: the CPU time of the thread with id `tid` (`plat_thread_id()`) so far; 0 when unknown.
- `plat_cpu_cores()`: the logical processors the process may use (bit n = processor n, the first 64) into `*all`, and
  the lowest of each physical core among them into `*first`; it returns the number of physical cores, 0 when unknown.
- `plat_cpu_cache_die()`: the logical processors of the process's set (the first 64) that share the largest L3 cache,
  when the L3 caches are not all the same size: the die with AMD's 3D V-Cache on a two-die X3D processor. 0 when they
  are (one die, or equal dies) or the topology is unknown. `*l3_kb`: that cache's size.

### `plat_intr_self()`, `plat_intr_free()`, `plat_intr_stop()`, `plat_intr_resume()`

Interrupting a thread (the asynchronous preemption of `runtime/hle/kernel.c`). A thread makes its own handle
(`plat_intr_self()`, NULL on failure); another thread stops it wherever it runs, reads where it is, may send it
elsewhere, and lets it go on. While it is stopped the caller must not wait for anything the stopped thread could hold.

- `plat_intr_stop()` stops `t` and reads its instruction and stack pointers: 1 when stopped, 0 when it did not stop in
  time (a thread blocked in the OS); `t` then runs on untouched.
- `plat_intr_resume()` lets a stopped `t` go on: at `to` when `to` is nonzero, else where it stopped. 0 when the
  redirection failed (`t` then goes on where it stopped).

### `plat_watch_set()`

Hardware data watchpoints (diagnosis: `runtime/diag/floor_watch.c`). Up to four addresses, each watched for any read or
write of its `len` bytes (1, 2, 4 or 8, the address aligned to it), on every thread the process has when they are set;
threads started later have none. After each access `hit(slot, frames, depth)` runs on the thread that made it, inside
its exception handling: `frames[0]` is the instruction after the access, then up to `PLAT_WATCH_DEPTH` (4) - 1 return
addresses. `hit` must not lock, allocate or log, and its nonzero return clears that thread's watchpoints. `n` = 0 clears
them on every thread. It returns the threads changed; -1 where the platform has none (Linux) or the change failed.

### `plat_fiber_self()`, `plat_fiber_unself()`, `plat_fiber_new()`, `plat_fiber_switch()`, `plat_fiber_free()`

Fibers: execution contexts with stacks of their own that threads switch between; a fiber may resume on another thread.
Each switch keeps the callee-saved registers and the floating-point control state.

- `plat_fiber_self()`: the calling thread's own context as a fiber, once before its first switch.
- `plat_fiber_unself()`: the calling thread stops being a fiber (it is on its own context again).
- `plat_fiber_new()`: a fiber that runs `fn(arg)` on a stack of `stack` bytes (reserved, committed as it grows); `fn`
  never returns.
- `plat_fiber_switch()`: from the fiber the calling thread runs. `plat_fiber_free()`: a fiber nobody runs.

### `plat_jmp_save()`, `plat_jmp_go()`

A jump back to a point saved earlier on the same thread, without unwinding the frames in between (no SEH, no C++
cleanups): the kernel's ExitThread leaves the lifted frames this way (`runtime/hle/kernel.c`). `plat_jmp_save()` returns
0 when it saves and 1 when `plat_jmp_go()` comes back to it; the function that called it must still be running. It keeps
the callee-saved registers and the floating-point control state.

On Windows this is not the C runtime's `setjmp`: a non-unwinding `setjmp` leaves the buffer's frame 0, and the C
runtime's `longjmp` ends the process with 0xc0000409 (fast fail 13) on such a buffer when Control Flow Guard is on for
the process, which Exploit protection can force on any program. `PlatJmp` there is `gpr[10]`, `mxcsr`, `fpcw`, a pad
and `xmm[160]`.

On x86-64 Linux `PlatJmp` is `gpr[8]`, `mxcsr` and `fpcw`: SysV's rbx, rbp, r12-r15, the caller's rsp and the return
address, then MXCSR and the x87 control word (the code is in `runtime/platform/linux.c`). It is not glibc's `jmp_buf`:
glibc mangles rsp, rbp and rip, so `plat_frame_capture()` could not fill one. Any other platform uses `setjmp` and
`longjmp` on a `jmp_buf`.

### `plat_frame_capture()`

The host state of a caller further up this thread's stack, as it will be when the call it is making returns: the stack
is unwound frame by frame (from the caller of this function) until `want(return address, arg)` accepts one, and that
frame's callee-saved registers, stack pointer and return address go to `*out` for `plat_jmp_go()`, which then makes
that call return a second time (eax 1). 1 on success, 0 when no frame is accepted (or on a platform without it).

### `plat_thread_priority()`, `plat_thread_set_priority()`, `plat_thread_priority_get()`, `plat_yield()`, `plat_pause()`

Scheduling priority in Windows' seven levels (`PLAT_PRIO_IDLE` -15, `PLAT_PRIO_LOWEST` -2, `PLAT_PRIO_BELOW_NORMAL` -1,
`PLAT_PRIO_NORMAL` 0, `PLAT_PRIO_ABOVE_NORMAL` 1, `PLAT_PRIO_HIGHEST` 2, `PLAT_PRIO_TIME_CRITICAL` 15): of the calling
thread, or of `t`; `plat_thread_priority_get()` gives the calling thread's level.

On Linux a thread lowered without the right to raise its priority again (CAP_SYS_NICE, or an RLIMIT_NICE of 20) could
never return to normal, and the runtime lowers threads that later take shared locks at normal priority; without that
right every change is skipped and threads keep the normal priority. Levels above normal need the same right.

`plat_yield()` gives the rest of the time slice to another ready thread. `plat_pause()` is the spin-wait hint
(`_mm_pause` on x86-64).

### `plat_now_ns()`, `plat_now_ms()`, `plat_counter()`, `plat_counter_hz()`

`plat_now_ns()`: monotonic, high resolution, arbitrary epoch. `plat_counter()`: the OS's raw monotonic counter (QPC on
Windows), `plat_counter_hz()` per second.

### `plat_timer_1ms()`, `plat_power_full()`

`plat_timer_1ms()` asks the OS for timed waits accurate to 1 ms or better (call it before threads are made).
`plat_power_full()` opts out of the OS's power throttling of an unfocused process: 2 speed and timer, 1 speed only
(Windows 10), 0 not done or nothing to do.

### `plat_scale()`, `plat_scaled()`

`t * num / den` for a fixed `num / den` without a 128-bit division per call (`__udivti3` took 5% of the game's running
samples through `plat_now_ns()` and `rt_ticks`): `t * q + (t * f >> 64)` with `q = num / den` and
`f = (num % den) * 2^64 / den` (`PlatScale`). Monotonic in `t` and at most one below the exact quotient while
`t * (num % den) / den` fits 64 bits.

### `plat_sleep_ns()`, `plat_wait_ns()`, `plat_sleep_ms()`

- `plat_sleep_ns()` returns no earlier than `ns` later; the overshoot is well under 1 ms.
- `plat_wait_ns()` sleeps about `ns` on the OS's high-resolution timer without spinning: it wakes up to
  `PLAT_WAIT_LATE_NS` late, never early. That is 500000 on Windows (the high-resolution waitable timer) and 60000
  elsewhere (a sleep with the 1 us timer slack `plat_timer_1ms()` sets, plus the scheduler's wakeup).
- `plat_sleep_ms()` is the coarse sleep: the scheduler's granularity, no spinning.

### `plat_mutex_init()`, `plat_mutex_lock()`, `plat_mutex_trylock()`, `plat_mutex_unlock()`, `plat_mutex_lock_shared()`, `plat_mutex_trylock_shared()`, `plat_mutex_unlock_shared()`

A `PlatMutex` is a reader-writer lock taken exclusively (`plat_mutex_lock()`) or shared (`plat_mutex_lock_shared()`),
not recursive, like Windows' SRW lock, which it is on Windows. Zero-initialised storage is a valid unlocked lock and an
empty condition variable (`PLAT_MUTEX_INIT` and `PLAT_COND_INIT` spell it). The two trylocks return 1 when acquired.

### `plat_cond_init()`, `plat_cond_wait()`, `plat_cond_wait_ns()`, `plat_cond_wait_ms()`, `plat_cond_signal()`, `plat_cond_broadcast()`

`plat_cond_wait()` wants `m` held exclusively. `plat_cond_wait_ns()` returns 0 on timeout (a spurious wakeup returns 1
too).

### `plat_rmutex_lock()`, `plat_rmutex_trylock()`, `plat_rmutex_unlock()`

`PlatRMutex`: a mutex the thread that holds it may take again; unlocked as many times as locked. Zero-initialised
(`PLAT_RMUTEX_INIT`) is unlocked. `plat_rmutex_trylock()` returns 1 when acquired, or already held by this thread.

### `plat_lock_timing()`, `plat_mutex_lock_at()`, `plat_rmutex_lock_at()`

Lock-wait timing (`NX2SARIA_LOCK_TIME`, `runtime/diag/perf.c`): after `plat_lock_timing(fn)` on a thread, each
`plat_mutex_lock()`, `plat_mutex_lock_shared()` or `plat_rmutex_lock()` there that has to wait calls `fn` with the
address of the code that asked for the lock (for a recursive mutex, the caller of `plat_rmutex_lock()`) and the
nanoseconds it waited. NULL turns it off. Windows only. `plat_mutex_lock_at()` and `plat_rmutex_lock_at()` are the two
locks reporting `site` as the asker.

### `plat_once()`

One-time initialisation: the first caller runs `fn(arg)`, every other caller waits until it returned.
Zero-initialised (`PLAT_ONCE_INIT`) has not run.

### `plat_event_init()`, `plat_event_set()`, `plat_event_reset()`, `plat_event_wait()`, `plat_event_wait_ns()`

An event: set wakes its waiters; an auto-reset event lets one waiter through and clears itself, a manual-reset one
stays set until reset. `plat_event_wait_ns()` returns 1 when the event was set, 0 on timeout.

### `plat_inc32()`, `plat_dec32()`, `plat_add32()`, `plat_xadd32()`, `plat_xchg32()`, `plat_cas32()`, `plat_inc64()`, `plat_dec64()`, `plat_add64()`, `plat_xadd64()`, `plat_xchg64()`, `plat_or64()`, `plat_cas64()`, `plat_xchgp()`, `plat_casp()`

Atomics: full barriers (`PLAT_SC`, sequentially consistent), explicit widths (Windows' `Interlocked*` operate on a
32-bit `LONG`). The value returned: the new one for inc, dec and add, the previous one for xadd, xchg, cas and or.

### `plat_mem_reserve()`, `plat_mem_alloc()`, `plat_mem_commit()`, `plat_mem_commit_error()`, `plat_write_watch_note()`, `plat_mem_decommit()`, `plat_mem_protect()`, `plat_mem_state()`

Virtual memory: ranges reserved at fixed addresses below 2^39, their pages committed, decommitted and protected at
4 KiB granularity (`PLAT_PROT_NONE`, `PLAT_PROT_READ`, `PLAT_PROT_RW`).

- `plat_mem_reserve()` reserves `[addr, addr + n)` inaccessible, exactly there: 0 when any of it is taken. `watch`:
  writes to its pages are tracked for `plat_mem_written()`.
- `plat_mem_alloc()`: `n` bytes of read-write memory anywhere, zero, backed only as pages are first written
  (`plat_mem_protect()` may seal them); NULL on failure. Kept until exit.
- `plat_mem_commit()`: read-write pages inside a reserved range. 1 on success; on failure it writes the native error
  when asked. `plat_mem_commit_error()` turns that error into its kind, number and name for the log.
- `plat_write_watch_note()`: once the first watched range was reserved, 1 and the log line that says whether written
  memory is tracked and, when it is not, why and what it costs (`runtime/platform/write_watch_note.h` `ww_note()`);
  0 before that, and always on Windows, whose write watch the system gives.
- `plat_mem_decommit()`: the contents are dropped; committed again, the pages read zero.
- `plat_mem_protect()`: committed pages only; 1 on success.
- `plat_mem_state()`: the page at `addr`: `PLAT_PROT_*` when committed, else `PLAT_MEM_RESERVED` (-1) or
  `PLAT_MEM_FREE` (-2, not reserved by us). `*end` gets the end of the run of pages in the same state, at most `limit`.

### `plat_mem_census()`

The process's committed private memory and the part of it mapped write-combined (Windows' `PAGE_WRITECOMBINE`: memory
the GPU driver maps uncached for the CPU), with the number of allocations that part spans. 0 where unsupported.

### `plat_mem_written()`, `plat_mem_prepare_write()`

`plat_mem_written()`: the pages of a watched reservation written since the last reset, up to `max` page addresses into
`pages`, in address order; `reset`: those reported start over as unwritten. `UINT64_MAX` when writes to the range are
not tracked.

`plat_mem_prepare_write()`, before the host itself writes `[addr, addr + n)` of a watched reservation: clears the write
watch's protection of those pages in one call, where the watch works by protecting pages (Linux), so the stores do not
fault page by page. The pages count as written. It returns 0 when there is nothing to clear or it cannot be done; the
write is valid either way.

### `plat_mem_dmabuf_ok()`, `plat_mem_dmabuf()`, `plat_mem_dmabuf_done()`

Guest memory as dma-bufs (Linux, `NX2SARIA_DMABUF`, on by default; the dma-buf imports of `runtime/vk/vk_memory.c`): the
watched reservations are shared mappings of one memfd, and `/dev/udmabuf` makes a dma-buf of a page range of it, which
the GPU driver imports like its own memory. The why and the measurements:
[building.md](../building.md#linux-in-progress); the mechanism: [Guest memory as a memfd's](#guest-memory-as-a-memfds).

- `plat_mem_dmabuf_ok()`: 1 when that is so, with udmabuf's size limit in bytes in `*limit` and the first page of a
  shared reservation in `*any`; 0 with the reason in `*why` ("" when `NX2SARIA_DMABUF=0` turned it off).
- `plat_mem_dmabuf()`: a new dma-buf file descriptor over `[addr, addr + n)` (whole pages inside one watched
  reservation), -1 with `errno` on failure. Its pages stay pinned (a decommit zeroes them in place) until
  `plat_mem_dmabuf_done()` is told, after the last user of the dma-buf (the imported memory) is gone; the pages of its
  range that nothing has committed then go back to the system.

Elsewhere these report 0 and -1.

### `plat_mem_fault_hook()`, `plat_mem_access_hook()`

`plat_mem_fault_hook()`: a write that faulted on a read-only page at `addr` goes to `fn` first; `fn` returns 1 when it
made the page writable and the write should run again, 0 to let the fault go on (to the crash handler). One hook per
process.

`plat_mem_access_hook()`: a read or write that faulted at `addr` (`write`: 1 for a write; `ip`: the faulting
instruction) goes to `fn` before the write-fault hook; `fn` returns 1 when it made the page accessible and the access
should run again, 0 to let the fault go on. `fn` sees every access violation of the process and tells its own pages
apart. One hook per process.

### `plat_reexec()`

Starts this program again with the same arguments and ends this process with the new one's exit code (Windows), or
replaces this process with it (Linux). Environment changes made before the call are passed on.

### `plat_crash_hook()`, `plat_crash_context()`, `plat_crash_context_get()`, `plat_crash_filter_reassert()`

A crash goes to `fn`, which reports it and must end the process. The write-fault hook runs first.

- A fault in this executable's own code (an access violation, an illegal instruction, a division by zero, a stack
  overflow) is reported as it happens.
- A fault in another module (a GPU driver, a system library) is reported only when nothing handles it: on Windows from
  the unhandled-exception filter, with the return addresses in this executable on the faulting thread's stack, so the
  report shows which host call ran into the driver; on Linux from the signal handler, without frames. Drivers that
  raise and catch their own exceptions never reach `fn`.
- On Windows `abort()` (a failed assert, `std::terminate`) and an invalid parameter to a C runtime function go there
  too, with the host frames in this executable: the C runtime ends the process for both with 0xc0000409 and no
  exception.

`PlatCrash`:

| Field | Holds |
|---|---|
| `what` | "access violation", "illegal instruction", ... |
| `code` | the Windows exception code or the signal number |
| `rip`, `offset` | the faulting instruction, and its offset in its module |
| `access` | the data access that faulted: 0 read, 1 write, 8 execute; -1 when not an access |
| `addr` | its address when `access` >= 0 |
| `nframes`, `frames[16]` | return addresses in this executable, innermost first: for abort and invalid parameter (`rip` is `frames[0]`) and for a fault in another module; 0 for a fault in this one |
| `module` | the module holding `rip`: NULL for this executable, "" for none (a jump to address 0) |

`plat_crash_context()`: what this thread is doing, named by the crash notes (a stack overflow leaves no room for the
full report); NULL clears it. `s` must stay valid until it is cleared. `plat_crash_context_get()` is the calling
thread's note, for the crash report (NULL: none).

`plat_crash_filter_reassert()` puts the crash hook's last-chance exception filter back in place. 1 when a DLL loaded
later had set its own in its place (`who`: that filter's module and offset), 0 when it was still there. Windows only; 0
elsewhere.

### `plat_mem_read()`, `plat_mem_readable()`, `plat_mem_resident()`

`plat_mem_read()` copies `n` bytes, and returns 0 instead of faulting on unmapped memory. `plat_mem_readable()`: every
page of `[addr, addr + n)` can be read; no fault.

`plat_mem_resident()`: which pages of `[addr, addr + n)` hold memory now (256 pages at most, 1 MiB from `addr`'s page):
`vec[i]` is 1 when the i-th page from `addr`'s page is resident, else 0. A committed page that was never written reads
zero without being resident, and reading it would allocate it (a page of the shared memory file on Linux, a physical
page on Windows); a scan of untouched memory skips such pages. Windows answers from this process's working set, so a
page trimmed from it counts as not resident. 1 on success, 0 when the system cannot say (a page of the range is not
mapped).

### `plat_aligned_alloc()`, `plat_aligned_free()`

Free with `plat_aligned_free()`; NULL on failure.

### `plat_exit()`, `plat_terminate()`

`plat_exit()` ends the process at once: other threads stop, no `atexit` handlers run. `plat_exit_hook`, when set, is
called first (`runtime/main.c`: the queued log lines).

`plat_terminate()` ends the process at once with `code`, and nothing else: no exit hook, no stream flush, and no
library's detach call (`ExitProcess` and `_Exit` run every one). For the ends of a failed run (`runtime/dispatch.c`
`fatal_end()`, `runtime/vk/vk_device.c` `lost_wait_exit()`), which have written their lines themselves and must not wait
on a lock another thread holds.

### `plat_setenv()`, `plat_getenv()`, `plat_process_id()`, `plat_thread_id()`

`plat_setenv()`: a NULL `value` removes the variable; 1 on success. `plat_getenv()`: the environment variable as UTF-8
into `out`; its length, or 0 when unset, empty or too long for `cap`. `plat_thread_id()`: the id of the calling thread,
for unique file names.

### `plat_process_cpu_ns()`, `plat_process_uptime_ns()`, `plat_unix_time_ns()`, `plat_local_time()`, `plat_localtime()`, `plat_gmtime()`, `plat_tz_west()`

- `plat_process_cpu_ns()` and `plat_process_uptime_ns()`: the CPU time of the process since it started (user and
  kernel), and the wall time since it started, in ns; 0 when unknown.
- `plat_unix_time_ns()`: the wall clock, ns since 1970. `plat_local_time()`: the wall clock now in the local time zone.
- `plat_localtime()`: a POSIX time in the local time zone; 0 when out of range. `plat_gmtime()`: the same in UTC.
- `plat_tz_west()`: the local zone's standard offset, seconds west of UTC (UTC-5: 18000).

### `plat_run_self()`

Runs this executable again with the same arguments and `env_name=env_value` added to its environment, for probes that
expect the child to stop with a fatal. Its stdout and stderr go into `out` (NUL-terminated, the rest discarded); it
waits at most `timeout_ms` (0: no limit) and kills it after that. It returns the exit code, -1 when it could not start.

### `plat_random()`

Cryptographically random bytes; 1 on success.

### `plat_file_exists()`, `plat_dir_exists()`, `plat_full_path()`, `plat_file_mkdir_p()`, `plat_file_read()`, `plat_file_write()`

Files. Paths are UTF-8 with either separator; each implementation converts to what the OS wants.

- `plat_file_exists()`: 1 for a file (not a directory). `plat_dir_exists()`: 1 for a directory.
- `plat_full_path()`: `path` made absolute against the working directory (not resolved further); 0 when it does not fit
  `cap`.
- `plat_file_mkdir_p()` creates every missing directory of `path`, itself included.
- `plat_file_read()`: the whole file into a malloc'd buffer with a NUL after the last byte; NULL when missing, empty or
  over 1 GiB.
- `plat_file_write()`: create or truncate; 1 when all `n` bytes landed.

### `plat_file_replace()`, `plat_file_write_atomic()`, `plat_file_write_cached()`

`plat_file_replace()` renames `from` over `to`, replacing it; `durable`: the rename is on the disk at return (written
through). 1 on success.

`plat_file_write_atomic()` writes `path.<pid>.<tid>.tmp`, flushes it to the disk and renames it over `path`: a crash
leaves the old file or the new one, never a torn or zero-filled one. 1 on success.

`plat_file_write_cached()` is the same without waiting for the disk: a crash of the process still leaves the old file
or the new one, but a power cut can leave a torn or zero-filled one. For files whose readers check them and that can be
made again (the shader cache). Each flush costs 1-5 ms on an NVMe drive and much more on a SATA SSD or a hard drive.

### `plat_file_swap_begin()`, `plat_file_swap_end()`

The same swap for data something else writes into memory of ours (a driver's pipeline cache): `plat_file_swap_begin()`
makes `path.<pid>.<tid>.tmp` with `n` bytes and maps it read-write, NULL on failure. The mapped pages belong to the
file, so they take none of the system's commit, which a malloc'd buffer of `n` bytes holds until it is freed.
`plat_file_swap_end()` unmaps the file and, with `ok`, cuts it to `used` bytes, flushes it to the disk and renames it
over `path`; without `ok`, or when one of those fails, it deletes it. 1 when `path` holds the new file.

### `plat_lock_file()`, `plat_unlock_file()`

`plat_lock_file()` takes the file at `path` (made when missing) as a lock no other process can take, held until this
process ends: 1 held (again, when this process holds it already), 0 another process holds it, -1 the file cannot be
made. `plat_unlock_file()` gives a lock `plat_lock_file()` took back (probes, before deleting it).

### `plat_file_flush()`, `plat_file_sync()`, `plat_fseek64()`, `plat_ftell64()`, `plat_file_delete()`, `plat_file_truncate()`, `plat_file_size()`, `plat_file_set_readonly()`, `plat_file_private()`

- `plat_file_flush()` flushes a written file's data to the disk; `plat_file_sync()` is `fflush`, then the file's data
  to the disk. Both 1 on success.
- `plat_fseek64()` and `plat_ftell64()`: `fseek` and `ftell` with 64-bit offsets (a `long` is 32 bits on Windows):
  `_fseeki64` and `_ftelli64` there, `fseeko` and `ftello` elsewhere.
- `plat_file_truncate()`: 1 on success. `plat_file_size()`: the bytes of a file, -1 when missing or unreadable.
- `plat_file_set_readonly()` sets or clears the read-only attribute; 1 on success.
- `plat_file_private()`: only this account may read it (0600, a folder 0700); 1 on success.

### `plat_append_open()`, `plat_append_write()`

`PlatAppend`: a file that only grows and other processes may read meanwhile: each write lands whole at its end, from
any thread, and reaches the file even if the process dies right after (no user-space buffer, no flush to the disk).
`plat_append_open()` creates or truncates `path`; NULL on failure. `plat_append_write()`: 1 when all `n` bytes landed.

### `plat_exit_note_setup()`, `plat_exit_note_modules()`, `plat_exit_orderly()`

An end the game did not ask for: `ExitProcess` (or `exit()`) called by a library, or by the runtime outside
`plat_exit()` and `plat_terminate()`. Windows calls the executable's TLS callback for it, with every other thread
already stopped, and the callback then writes what the log queue held (`queued(&p, &n)`: the bytes as they stand, no
lock) and one line with the return addresses that led to the end into `log`. Nothing for `plat_exit()`,
`plat_terminate()`, `TerminateProcess` from outside, or after `plat_exit_orderly()` (main returning from a tool mode).
`plat_exit_note_modules()` notes the loaded modules, whose names the line gives the addresses (the callback may not ask
the loader): at start and at a focus loss. Windows only; elsewhere they do nothing.

### `plat_file_list()`

Calls `cb(name, ctx)` for each file in `dir` whose name ends in `ext` (".in"); `name` has no directory part. `cb`
returns 0 to stop. It returns 0 when `dir` cannot be listed.

### `plat_file_open()`, `plat_file_open_unbuffered()`, `plat_file_close()`, `plat_file_pread()`, `plat_file_id()`, `plat_file_map()`

`PlatFile`: open files for positional reads: any thread, in parallel on one handle, no file pointer.

- `plat_file_open()`: read-only; NULL when missing or unreadable.
- `plat_file_open_unbuffered()`: the same without the OS's file cache (reads go to the disk into the buffer): offsets,
  sizes and buffers aligned to the sector size.
- `plat_file_pread()`: the bytes read at `off` (0 at or past the end of the file), -1 on an I/O error, -2 when the OS
  refused to write into `buf` (a read-only page).
- `plat_file_id()`: what changes when the file is replaced or written (`PlatFileId`): its volume, file id, size, last
  write and creation (on Linux, status change) times in the OS's units. 0 on failure.
- `plat_file_map()`: a read-only view of the whole file, kept until exit; NULL on failure.

### `plat_fs_kind()`, `plat_fs_os_error()`, `plat_fs_mkdir()`, `plat_fs_rmdir()`, `plat_fs_unlink()`, `plat_fs_rename()`, `plat_fs_link()`, `plat_fs_links()`, `plat_fs_sync_dir()`, `plat_fs_busy_reason()`, `plat_fs_list()`

The host file system under the game's save data (`runtime/hle/fs_host.c`). Calls that can fail return `PLAT_FS_OK` (0)
or the reason (`PLAT_FS_NOT_FOUND`, `PLAT_FS_EXISTS`, `PLAT_FS_NOT_EMPTY`, `PLAT_FS_BUSY`, `PLAT_FS_ERROR`); other
processes may open the same files meanwhile.

- `plat_fs_kind()`: `PLAT_FS_MISSING` (-1), `PLAT_FS_FILE`, `PLAT_FS_DIR` or `PLAT_FS_LINK`: a symbolic link or reparse
  point, never followed.
- `plat_fs_os_error()`: the OS error code (`GetLastError`, `errno`) of this thread's last failed `plat_fs_*` call.
- `plat_fs_mkdir()`: `PLAT_FS_EXISTS` when something is there.
- `plat_fs_rmdir()`: an empty directory (a read-only one too), or a link to one.
- `plat_fs_unlink()`: a file or a link; a read-only one too.
- `plat_fs_rename()` renames a file or directory; it never replaces (`PLAT_FS_EXISTS`). `durable`: the change is on the
  disk at return.
- `plat_fs_link()`: a hard link. `plat_fs_links()`: the file's hard link count, 0 when unknown.
- `plat_fs_sync_dir()`: a directory's entries to the disk, as far as the file system allows.
- `plat_fs_busy_reason()`: why `dir` could not be renamed, for the log: into `out` "no permission to rename it" when
  this process may not delete it, else the programs holding a file under it ("explorer.exe 1234, ..."), else an empty
  string. Windows refuses to rename a directory with any file open under it; elsewhere `out` is always empty.
- `plat_fs_list()`: `cb(name, kind, size, ctx)` for each entry of `dir` but "." and ".."; `cb` returns 0 to stop. It
  returns the listing's error.

### `plat_fs_open()`, `plat_fs_create()`, `plat_file_pwrite()`, `plat_file_set_size()`, `plat_file_fsync()`

`plat_fs_open()`: an existing file for reading and, with `write`, writing; `*err` (may be NULL) gets the reason on
failure. `plat_fs_create()`: a new file for writing; `PLAT_FS_EXISTS` when it is there. `plat_file_pwrite()`: the bytes
written, -1 on error. `plat_file_set_size()` returns `PLAT_FS_*`. `plat_file_fsync()`: the file's data to the disk; 1 on
success.

### `plat_temp_dir()`, `plat_exe_path()`, `plat_exe_base()`, `plat_module_of()`, `plat_library_path()`, `plat_last_sep()`

- `plat_temp_dir()`: the user's temporary directory, no trailing separator.
- `plat_exe_path()`: the path of the running executable; 0 on failure. `plat_exe_base()`: where the executable is
  loaded.
- `plat_module_of()`: the module (executable or library) holding code address `addr`: its file name without directories
  into `name`, its load address into `*base`. 0 when none.
- `plat_library_path()`: the path of the loaded shared library the loader matches to `name` (libSDL3.so.0, SDL3.dll):
  its folder absolute and, on Linux, free of symbolic links; 0 when no loaded module answers to that name.
- `plat_last_sep()`: the last directory separator in `path` ('/' or '\\'), NULL when there is none.

`PLAT_PATH_MAX` (1024) is the size of the runtime's path buffers. `PLAT_EXE_SUFFIX` (".exe" on Windows, "" elsewhere) is
the suffix of the programs the runtime starts (the shader translator, `nx2navi_xlate`).

### `plat_ui_language()`, `plat_user_locale()`, `plat_font_path()`, `plat_os_version()`, `plat_is_wine()`

- `plat_ui_language()`: the user's UI language as a BCP-47 tag ("ru-RU", "en-US"); "" when the system gives none.
  Valid for the process.
- `plat_user_locale()`: the user's regional format (locale) as a BCP-47 tag; "" when the system gives none. Valid for
  the process.
- `plat_font_path()`: a system font file with the glyphs of a catalog language ("ja", "ko", "zh-Hans", "zh-Hant"): the
  menu shows each language's name in its own script. It copies the UTF-8 path into `out`; 0 when the system has none.
- `plat_os_version()`: the operating system and its build ("Windows 10.0.28000") into `out`; 0 when unknown.
- `plat_is_wine()`: 1 when the Windows build runs under Wine; 0 elsewhere.

### `plat_stderr_path()`, `plat_stderr_write()`, `plat_console_setup()`

`plat_stderr_path()`: the file the process's stderr (the log) goes to, as a UTF-8 path; 0 when stderr is not a file (a
console, a pipe).

`plat_stderr_write()` writes the bytes to stderr's file descriptor, past the C runtime's `FILE` stream (on Windows
with its low-level `_write`, which keeps the text mode's line ends): a write that blocks, on a console that holds its
output back, then holds no lock of the stderr stream, and `fflush(stderr)` or `fflush(NULL)` on another thread does not
wait for it. 1 when all of them were written.

`plat_console_setup()`, when stderr is a console window (Windows): turns its QuickEdit mode off, since a selection made
in the window with the mouse holds back every write to it until the selection ends, and passes a close of the console,
Ctrl+C and Ctrl+Break to `on_close`, on a thread of the system's, with the event's name. `on_close` must end the
process: Windows ends it about 5 s after a close in any case. A second Ctrl+C or Ctrl+Break while `on_close` runs ends
the process at once, as without the handler. `plat_exit()` puts QuickEdit back as it was. 1 when stderr is a console;
elsewhere 0, and nothing is changed.

### `plat_clipboard_set()`, `plat_clipboard_get()`, `plat_open_folder()`

The desktop. `plat_clipboard_set()` and `plat_open_folder()` were written for the in-game menu's "Copy system info"
and "Open log folder" rows; the menu has neither row now, and nothing in the game calls the two. `plat_clipboard_get()`
is the menu's paste. `plat_clipboard_set()` replaces the
clipboard's text; 1 on success. `plat_clipboard_get()`: the clipboard's text as UTF-8 in `out` (`cap` bytes with the
NUL; a longer text: "" on Windows, cut at a character elsewhere); 1 when it had text. With SDL only on the window
thread, as SDL asks: the menu goes through `gfx_window_paste()` (`runtime/ui/menu.h`). `plat_open_folder()` shows the
folder in the file manager; 1 on success.

### `plat_lib_open()`, `plat_lib_sym()`

Dynamic libraries: a library some machines lack (a GPU vendor's), loaded at run time. A bare name is searched in the
system directories only. NULL when missing.

### `plat_load_open()`, `plat_load_close()`, `plat_cpu_load()`, `plat_gpu_load()`, `plat_mem_info()`

The machine's load for the performance overlay (the sampling thread of `runtime/diag/perfmon.c`; each call can take
milliseconds). A number the system does not give is -1.

- `plat_load_open()` opens the counters: CPU times, the thermal zones, and the GPU engine and memory counters of the
  adapter whose LUID is `gpu_luid` (8 bytes, Vulkan's `deviceLUID`; NULL: the busiest adapter) or, where the system
  names GPUs by PCI address, on PCI bus `pci_bus` (-1: unknown). The header says it is never NULL; both
  implementations return NULL when their `calloc` fails, and every other call takes a NULL `l`.
- `plat_cpu_load()`, since the previous call on `l` (the first call gives -1): the CPU use of the machine and of this
  process, in percent of all logical processors, and a temperature in degrees Celsius: on Windows the hottest ACPI
  thermal zone, on Linux the CPU's hwmon sensor.
- `plat_gpu_load()`, since the previous call on `l`: the busiest GPU engine type in percent (as Task Manager shows it),
  and the adapter's dedicated memory in use in MB. On Windows for any GPU vendor; on Linux from amdgpu's sysfs files.
- `plat_mem_info()`: physical memory in MB: in use and installed on the machine, and this process's working set.

### `plat_commit_info()`, `plat_commit_enforced()`, `plat_commit_note()`, `plat_commit_counts_vram()`, `plat_page_file_managed()`

- `plat_commit_info()`: the commit charge in MB: this process's private bytes, the system's commit in use and its limit
  (RAM plus page files). An allocation fails when the system's reaches the limit, however much physical memory is free.
  -1 when unknown. Linux overcommits and ends the process instead (the OOM killer), so there the limit is the one with
  the least headroom of the cgroup v2 limits (`memory.max`, `memory.high`) the process lives under and the machine's RAM
  (MemAvailable against MemTotal), with `vm.overcommit_memory = 2` also CommitLimit.
- `plat_commit_enforced()`: whether the limit is one to stay below: always on Windows (an allocation fails at it); on
  Linux whenever the kernel reports a memory limit. 0 when none is known.
- `plat_commit_note()`: the limits the first `plat_commit_info()` call found, for the log, as text into `out` ("memory
  limits: cgroup ... memory.max 12288 MB (...); RAM ...; the one with the least headroom counts (now: ...)"); 0 when
  there is nothing to add to the numbers (Windows, whose one limit is the system's commit).
- `plat_commit_counts_vram()`: whether a discrete GPU's own memory counts against that limit: on Windows the driver
  charges device memory to the system's commit; Linux's limits count system memory only.
- `plat_page_file_managed()`: whether Windows sizes every page file by itself (System managed size): 1; 0 when a page
  file has its sizes set; -1 when unknown, there is none, or not Windows.

### `plat_pressure()`, `plat_pressure_clear()`

The machine's memory and I/O pressure, for the log's one-line account of a minute (`runtime/log_stall.c`). A number the
system does not give is -1 (the totals 0). Linux: the kernel's pressure stall information (`/proc/pressure/memory` and
`io`, kernel 4.20 with `CONFIG_PSI`; "some": the share of the last 10 s at least one task waited on the resource,
"full": all of them did; the totals count microseconds of waiting since the boot), MemAvailable and SwapFree. Windows
has no stall counters: the physical memory available and the system's commit against its limit. A few small reads that
allocate nothing, so a fatal error's report may ask too.

`PlatPressure`: `mem_some10`, `mem_full10`, `io_some10`, `io_full10` in percent; `mem_some_us`, `mem_full_us`,
`io_some_us`, `io_full_us` in microseconds since the boot; `avail_mb` and `total_mb`, the physical memory available and
installed, in MB; `swap_free_mb` and `swap_total_mb`, Linux only, in MB; `commit_used_mb` and `commit_limit_mb`, Windows
only, in MB. `plat_pressure_clear()` makes every figure unknown; it is written without designators because C++ files
include this header.

### `plat_process_memory()`, `plat_handle_count()`, `plat_handle_census()`, `plat_heap_info()`

`plat_process_memory()`: this process's memory in bytes: resident (the working set) and private (Windows: committed
private bytes; Linux: the resident anonymous pages). 0 when unknown.

`plat_handle_count()`: the handles (Windows) or file descriptors (Linux) this process holds; 0 when unknown.

`plat_handle_census()`: the same by kind (`PlatHandleKind`: a 32-byte name and a count): on Windows the object type of
each handle (Event, Section, Thread, File...), on Linux what each file descriptor's link names (socket, pipe, an
anon_inode kind, a /dev folder, file). It fills `out` with up to `max` kinds, unsorted, and returns how many, -1 on
failure; the kinds past `max` are counted in the last entry, named "(more kinds)". It never opens, duplicates or names a
handle. Windows reads the system's whole handle table into a buffer of a few MiB kept between calls: one thread calls it
at a time.

`plat_heap_info()`: the C runtime's malloc heap in bytes (`PlatHeapInfo`), for the memory report
(`runtime/vk/vk_memory.c` `system_log()`): what it took from the system (`committed`) and what of that it handed out
(`used`), and how many other heaps the process has (`other_heaps`; Windows: those DLLs made with `HeapCreate`, the
graphics driver's among them; their sizes are not asked, as a heap made without serialization cannot be read while its
owner uses it). 0 when unknown. It holds the heap's lock while it adds up, so it is called at report time only.
Windows: `HeapSummary` of the heap malloc uses; Linux: glibc's `mallinfo2` over every arena.

### `plat_child_start()`, `plat_child_stop()`, `plat_child_lower()`, `plat_child_write()`, `plat_child_read()`

Child processes: a program with stdin and stdout piped to us (stderr discarded), no console window.

- `plat_child_start()` starts "exe" followed by `arg` (`arg` may be NULL). NULL on failure; `*err` (may be NULL) gets
  the OS error code. Children start one at a time so none inherits another's pipe ends.
- `plat_child_stop()` kills the child and frees `c`.
- `plat_child_lower()` runs the child below every normal-priority process (Windows: the idle priority class; elsewhere
  nice 19), for work that must not slow the game down (`runtime/gfx/gfx_shader.c`, the carry pass).
- `plat_child_write()`: 1 when all `n` bytes were written.
- `plat_child_read()`: the bytes read (at least 1); 0 or -1 when the pipe broke (the child is gone); -2 when nothing
  came for `PLAT_CHILD_READ_MS` while the child kept its end open (a child stuck on its input: asking it again would
  wait as long). `PLAT_CHILD_READ_MS` is 120000; it is defined only when not already, because a check may build the
  child functions with a shorter one.

### `plat_ipc_connect()`, `plat_ipc_read()`, `plat_ipc_write()`, `plat_ipc_close()`

Local IPC: a stream connection to another program of this machine, one listen location per name (Discord's Rich
Presence, `runtime/discord_rpc.c`). On Windows the named pipe `\\.\pipe\<name>`; on Linux the Unix domain socket
`<dir>/<name>` with `<dir>` the first of `$DISCORD_IPC_PATH`, `$XDG_RUNTIME_DIR`, `$TMPDIR` and `/tmp` that has one.

- `plat_ipc_connect()`: NULL when nothing listens under that name.
- `plat_ipc_read()`: up to `n` bytes, waiting at most `ms` for the first of them: the bytes read, 0 none in time, -1
  the connection broke. The rest of what the peer wrote may still be in its buffer: a caller that needs a whole frame
  loops.
- `plat_ipc_write()`: 1 when all `n` bytes were written, 0 when the connection broke.

### `plat_net_resolve()`, `plat_udp_open()`, `plat_udp_port()`, `plat_udp_send()`, `plat_udp_recv()`, `plat_udp_close()`

UDP sockets. One unconnected datagram socket per `PlatUdp`; who a datagram came from is checked by the protocol, not by
the OS. On Windows the first call of any of these starts Winsock. A `PlatAddr` (32 bytes) is an IPv6 address (an IPv4
one as ::ffff:a.b.c.d), the port and the scope id in the same layout on every OS, so two equal addresses compare equal
bytewise (`plat_addr_eq()`) whichever way the OS reported them.

- `plat_net_resolve()`: `host` (a name, or an IPv4 or IPv6 literal without brackets) and `port` into `*out`. It blocks
  while the resolver runs: call it on a thread that may wait, never on the game's. An IPv4 answer is preferred over an
  IPv6 one. 1 on success.
- `plat_udp_open()`: a non-blocking UDP socket bound to `port` (0: any free port) on every local address. `dual_stack`
  1: IPv6 with IPv4 in the same socket (IPv4 alone where the machine has no IPv6); 0: IPv4 only. NULL on failure.
- `plat_udp_port()`: the port it is bound to; 0 on failure.
- `plat_udp_send()`: one datagram to `*to`; it never blocks. 1 when it was sent or the OS dropped it (a full send
  buffer, no route, an ICMP error: a datagram may be lost anyway), 0 when it can never be sent (too long, an address the
  socket cannot reach).
- `plat_udp_recv()`: the next datagram into `buf`, waiting at most `ms` for it; `*from` (may be NULL) gets its sender.
  The bytes received (a datagram longer than `cap` is cut to `cap`), 0 when none came in time, -1 when the socket is
  unusable.

### `plat_addr_set()`, `plat_addr_get()`, `plat_addr_port()`, `plat_addr_eq()`, `plat_addr_str()`

Address helpers that need no OS (`runtime/platform/common.c`); the back ends build addresses with `plat_addr_set()`.
`plat_addr_str()`: "1.2.3.4:5" or "[::1]:5" ("%scope" after a link-local address) into `out`; its length, 0 when it
does not fit `cap`.

### `plat_tcp_listen_loopback()`, `plat_tcp_accept()`, `plat_tcp_recv()`, `plat_tcp_send()`, `plat_tcp_close()`

TCP on the loopback address: a listener that only programs of this machine can reach (the tracker API,
`runtime/diag/tracker_api.c`; [tracker-api.md](../tracker-api.md)), and the connections it accepts. The listener is
bound to 127.0.0.1 and to no other address, so nothing on the network reaches it. Accepted connections send each write
at once (`TCP_NODELAY`): a request and its answer are small, and a held answer would wait for the peer's delayed
acknowledgement. On Windows the first call starts Winsock. A port with connections of an earlier run still closing
(TIME_WAIT) can be bound again at once.

- `plat_tcp_listen_loopback()`: a listener on 127.0.0.1:`port`. NULL when the port is taken or sockets are unavailable;
  `*err` (may be NULL) gets the OS error number (`errno`, or `WSAGetLastError` on Windows) for the log.
- `plat_tcp_accept()`: the next connection on the listener, waiting at most `ms` for one. NULL when none came in time or
  the peer gave up before it was taken (`*err` 0), or when the listener cannot take one (`*err` the OS error, such as no
  descriptor left: the caller waits before it asks again). `*peer` and `err` may be NULL.
- `plat_tcp_recv()`: up to `cap` bytes, waiting at most `ms` for the first of them: the bytes read, 0 none in time, -1
  the peer closed the connection or it broke.
- `plat_tcp_send()`: all `n` bytes, waiting at most `ms` in all for the peer to take them: 1 sent, 0 the connection
  broke or the peer did not read in time (the caller then closes it). A closed peer is an error, never a signal.
- `plat_tcp_close()`: a listener or a connection; NULL does nothing.

### `plat_restart()`, `plat_restart_wait()`

Restart: this program again, in a new process that starts once this one is gone. `plat_restart()` starts this
executable again with the same command line, environment and working folder, and no handle of this process. 1 when it
started; the caller then exits. `plat_restart_wait()`, in a process `plat_restart()` started: waits, at most
`timeout_ms`, until the process that started it has exited. 1 after the wait, -1 on timeout, 0 when there is nothing to
wait for (not started by `plat_restart()`, or that process is gone).

## `runtime/platform/fp_x86.h`

Floating-point control on x86-64: MXCSR (rounding control in bits 13-14, FTZ in bit 15, DAZ in bit 6, the exception
flags and masks). Inline on purpose: lifted floating-point code (`runtime/simd.h`) reads and writes it on every
operation that depends on the guest FPCR.

### `plat_fp_get()`, `plat_fp_set()`

`_mm_getcsr` and `_mm_setcsr`. `PLAT_FP_RC_MASK` (0x6000) is the rounding control: 0 nearest, 0x2000 down, 0x4000 up,
0x6000 toward zero. `PLAT_FP_FTZ` is 0x8000, `PLAT_FP_DAZ` 0x0040.

## `runtime/platform/commit_error.h`

### `plat_commit_win_memory_advice()`

The Win32 allocation errors that mean the system cannot promise more committed memory: 1455
(`ERROR_COMMITMENT_LIMIT`), 8 (`ERROR_NOT_ENOUGH_MEMORY`) and 1450 (`ERROR_NO_SYSTEM_RESOURCES`). Kept as a pure
decision, with the numbers written out, so the fatal wording can be checked without allocating real memory or needing
Windows. The advice names the page file: 32 GB fixed on a 16 GB PC, 32768 MB.

## `runtime/platform/write_watch_note.h`

Whether written memory is tracked on Linux, as a pure decision over the kernel's version and the probe's result, so
`tests/write_watch_check.py` checks it on made-up versions; the launcher makes the same decision
(`launcher/platform/linux.py` `write_watch_verdict()`) and the test holds the two to the same cases.

### `ww_read_number()`, `ww_kernel_old()`

The major and minor of a kernel release string ("6.6.87.2-microsoft-standard-WSL2" is 6.6; a missing minor is 0). 1
when it is older than `WW_MIN_MAJOR`.`WW_MIN_MINOR` (6.7, the release with asynchronous userfaultfd write
protection), 0 when not, -1 when the string does not start with a number.

### `ww_verdict()`

`WW_ON` when the probe worked, whatever the version says (a backport, a version string that cannot be read). A failed
probe is `WW_OLD_KERNEL` on a kernel older than 6.7 and `WW_REFUSED` otherwise: a kernel built without
`CONFIG_USERFAULTFD`, or a sandbox (a container's or a Flatpak's seccomp profile) that blocks the call.

### `ww_note()`

The `mem: write watch` line for each verdict: on, with the kernel; off, with the kernel, the error, the cost (about
1 fps) and what to do.

## `runtime/platform/proc_pressure.h`

Readers of the text Linux gives for memory and I/O pressure (`runtime/platform/linux.c` `plat_pressure()`): the lines
of `/proc/pressure/memory` and `/proc/pressure/io`, and the figures of `/proc/meminfo`. Pure functions over the text,
so `tests/log_stall_check.c` runs them on made-up files. No allocation, no locale: a decimal separator other than "." in
the process's locale does not change what they read (`strtod` would).

### `psi_line()`

Reads the line that starts with `kind` ("some" or "full") out of a pressure file's text:

```
some avg10=0.00 avg60=0.00 avg300=0.00 total=0
```

`avg10` is a percent with two decimals, `total` microseconds. 1 when the line is there and whole, else 0 and the
outputs are left alone.

### `meminfo_kb()`

One figure of `/proc/meminfo` in kB ("MemAvailable:   20835692 kB"): `key` is the name with its colon. 1 when found.

## `runtime/platform/common.c`

The parts of `runtime/platform/platform.h` built on its locks rather than on the OS: recursive mutexes, one-time
initialisation and events, the network address helpers and the thread-end hooks. Every OS shares them.

### `plat_rmutex_lock_at()`, `plat_rmutex_lock()`, `plat_rmutex_trylock()`, `plat_rmutex_unlock()`

The recursive mutex: `owner` is the holder's thread id (0 when free); only the holder writes `depth`.

### `plat_once()`

The state is 0 not run, 1 running, 2 done. Waiters for a running initialiser sleep on one process-wide condition
variable (`once_done` under `once_lock`); initialisers are rare and short.

### `plat_addr_set()`, `plat_addr_get()`, `plat_addr_port()`, `plat_addr_eq()`, `plat_addr_str()`

Network addresses: the OS-free part of the header's UDP sockets. The layout of `PlatAddr.b`: the IPv6 address at 0 (an
IPv4 one as ::ffff:a.b.c.d), the port big-endian at 16, the scope id little-endian at 18, zeros after. In
`plat_addr_str()` the longest run of two or more zero groups of an IPv6 address becomes "::".

### `plat_thread_end_hook()`, `plat_thread_end_run()`

What a thread keeps for itself (the hooks `runtime/platform/platform.h` declares): up to `PLAT_THREAD_END_HOOKS`
functions in `end_hooks`, counted by `n_end_hooks`.

## `runtime/platform/linux.c`

The Linux implementation of `runtime/platform/platform.h`: pthreads, `CLOCK_MONOTONIC`, futex locks and condition
variables, POSIX files, `/proc` and sysfs for the load counters, `posix_spawn` for child processes.

The file defines the write-watch ABI of Linux 6.7 itself (uapi `linux/fs.h` and `linux/userfaultfd.h`: `struct
page_region`, `struct pm_scan_arg`, `PAGEMAP_SCAN`, the `PAGE_IS_*` and `PM_SCAN_*` bits,
`UFFD_FEATURE_WP_UNPOPULATED`, `UFFD_FEATURE_WP_ASYNC`, `UFFD_USER_MODE_ONLY`) for headers older than it: the release
builds against the Steam Runtime's 5.10 headers and runs on newer kernels. The kernel refuses what it lacks at run time.

### `alt_stack()`

A signal stack for the calling thread, so the fault and crash handlers still run when its own stack is exhausted. It is
64 KiB: a signal frame with the AVX-512 state takes several KiB. Never freed: threads that end are few, and a stack
freed while a handler might run on it would be worse.

### `thread_main()`, `plat_thread_name()`, `plat_thread_new_ex()`, `plat_thread_detach()`

`struct PlatThread` holds the kernel's thread id (`tid`) once the thread runs, for `plat_thread_set_priority()`, and a
16-byte name: the kernel keeps 15 characters. The name is copied into the thread's own `thread_name` at its start, as a
detached thread's struct may go. `thread_main()` calls `plat_thread_end_run()` after the thread's function, for what the
other files keep per thread.

The stack is mapped lazily, as Windows reserves it: only the pages a thread touches cost memory.

`cpu_rest` and `cpu_rest_set` are set in the thread that took a core in `plat_cpu_reserve()`: the processors the
threads it starts run on (not that core). Threads inherit the mask of the thread that starts them, which for those would
be the reserved core alone, so `plat_thread_new_ex()` gives them `cpu_rest`.

`plat_thread_detach()`: the thread keeps using `t->fn` and `t->arg`, so the struct stays allocated (a few bytes per
detached thread).

### `plat_thread_join_ns()`

`pthread_timedjoin_np` takes a `CLOCK_REALTIME` deadline, so the deadline is built from that clock.

### `plat_cpu_cores()`

The physical core of each logical processor is in `/sys/devices/system/cpu/cpu<n>/topology/core_id` together with its
package (`physical_package_id`); the lowest-numbered processor of each (package, core) pair is the core's first.

### `plat_cpu_cache_die()`

The L3 cache of each logical processor: the `cache/index<k>` of `/sys/devices/system/cpu/cpu<n>` whose `level` is 3,
its `size` in KiB ("98304K"; a size given in M is multiplied by 1024).

### `plat_process_affinity()`

`cpu_taken`, `cpu_others` and `cpu_taker`: the core `plat_cpu_reserve()` took, as its processors and the thread that
holds them (0 before the call), and the processors every other thread got then or at the last
`plat_process_affinity()`. `cpu_lock` orders the functions that set the masks of every thread.

Linux has no process-wide affinity: each thread of `/proc/self/task` gets the mask, and threads started later inherit
it from the thread that starts them. The core `plat_cpu_reserve()` took stays out of the mask of every thread but its
holder, which keeps the mask it has.

### `plat_thread_cpu_ns()`

The first field of `/proc/self/task/<tid>/schedstat` is the time the thread has run, in ns.

### `intr_handler()`, `plat_intr_self()`, `plat_intr_free()`, `plat_intr_stop()`, `plat_intr_resume()`

Thread interruption: a signal handshake in place of Windows' `SuspendThread` and `GetThreadContext` /
`SetThreadContext`. The caller moves the handle from `INTR_IDLE` to `INTR_REQ` and signals the thread. The thread's
handler stores where it was and moves `INTR_REQ` to `INTR_STOPPED`, then spins until the caller moves it on to
`INTR_GO` (with the redirect in `to`), changes its own return context, and goes back to `INTR_IDLE`. A caller that sees
no `INTR_STOPPED` within `INTR_WAIT_NS` (200000: the thread is blocked in the kernel and runs no handler) takes
`INTR_REQ` back to `INTR_IDLE`; a handler that then finds `INTR_IDLE` returns at once.

The signal is `SIGRTMIN + 4`: glibc keeps the first real-time signals for itself, and `SIGRTMIN` skips them.
`plat_intr_free()` is called by the thread itself as it ends, or by another once the thread no longer runs.

### `plat_watch_set()`

Hardware data watchpoints are Windows' only: the diagnosis that uses them is off here, and the call returns -1.

### Fibers: `plat_fiber_swap` and `plat_fiber_entry`

Fibers are a stack switch of our own, in assembly. glibc's `swapcontext` saves and restores the signal mask with a
system call on every switch, and the game switches fibers often. `plat_fiber_swap` saves the callee-saved registers,
the MXCSR and the x87 control word on the current stack, stores the stack pointer in `*save` and loads the other one. A
new fiber's stack is laid out as `plat_fiber_swap` leaves one, returning into `plat_fiber_entry`, which calls `fn(arg)`
(r12 and r13). `plat_fiber_entry` first aligns rsp (`andq $-16`): `plat_fiber_swap`'s `ret` leaves rsp 8 off, and the
SysV ABI wants it aligned at the call.

`struct PlatFiber`: `stack` is the mapped stack, NULL for a thread's own context.

### `plat_fiber_new()`, `plat_fiber_switch()`

The stack is reserved, not committed (`MAP_NORESERVE`); the lowest page stays inaccessible, so an overflow faults
instead of corrupting. The first frame, ten words below the 16-byte-aligned top: the FPU state in two words (x87
control word 0x037f and MXCSR 0x1f80, the defaults), r15 and r14 (0), r13 (`arg`), r12 (`fn`), rbx and rbp (0), the
return address (`plat_fiber_entry`) and a word of padding.

`plat_fiber_switch()` sets `fiber_cur` on this thread; the switch back into `from` sets it on whichever thread that is.

### `cpu_list_parse()`, `cpus_highest_by()`, `cpus_fast()`, `cpu_pick_core()`

A core for the recording thread (`plat_cpu_reserve()`).

- `cpu_list_parse()`: the processors a sysfs CPU list ("0-15", "0,2,4-7") names, added to `*set`; 0 when `s` is not such
  a list.
- `cpus_highest_by()`: `cpu<n>/<file>` under `root/system/cpu` holds one number per processor (`cpu_capacity`,
  `cpufreq/cpuinfo_max_freq`). When the processors of `in` differ in it by more than a tenth, `*out` gets the ones
  within a tenth of the highest and the result is 1; otherwise 0 and `*out` stays as it was. A processor without the
  file counts as the lowest.
- `cpus_fast()`: the processors of `all` on the fastest kind of core, and what makes them so. A hybrid Intel CPU lists
  its performance cores in `root/cpu_core/cpus`; for the others, cores of different capacity or top clock (a tenth apart
  or less counts as the same) keep the highest; a CPU with one kind of core keeps all of `all`. The names it returns:
  "a performance core", "a core of the highest capacity", "a core of the highest clock", "a core".
- `cpu_pick_core()`: the processors `plat_cpu_reserve()` takes into `*mine`: every processor of `all` on the fast core
  (`cpus_fast()`) that clocks highest, the highest-numbered one when several do. A core is counted at its first
  processor, and a core with a processor outside the fast ones is no fast core. It returns how many processors, 0 when
  fewer than four cores are fast: one of them is then too much to lose. `*kind` names what makes the core fast.

### `plat_cpu_reserve()`

The calling thread gets a fast core to itself (`cpu_pick_core()`): every other thread of the process now running loses
the core's processors, and the caller keeps only them. Threads the caller starts later through `plat_thread_new_ex()`
get the others; a thread started by another one inherits that one's mask, which no longer has the core.
`plat_process_affinity()` leaves the core with the caller. It returns the processors reserved, 0 when no core is fast
enough or the caller's mask cannot be set; `*which` (when not NULL) receives them as text, such as "10,11 of a
performance core".

The sweep over `/proc/self/task` runs up to three passes: a thread started during a pass by one it has not reached yet
is found by the next. A thread that was pinned to the core alone gets the rest.

### `plat_cpu_reserve_sweep()`

A thread the holder starts by other means than `plat_thread_new_ex()` (a library's audio thread, a driver's worker)
inherits its mask, the core alone, after `plat_cpu_reserve()`'s sweep ran, and `plat_process_affinity()` seldom runs
again: each thread whose mask lies within the core, the holder aside, gets the others' processors here. A thread with
anything beside the core in its mask keeps it. Once the holder has ended, nothing would use the core, so it goes back:
the threads with the others' mask get its processors again and the reservation ends. It returns the threads moved, -1
when the core went back.

### `plat_cpu_usable()`

The processors of the process's affinity mask (`taskset`, a launcher's CPU set), every online one when it is unknown.

### `nice_of()`, `may_renice()`

Windows' levels map to nice values: IDLE 19, LOWEST 4, BELOW_NORMAL 2, NORMAL 0, ABOVE_NORMAL -2, HIGHEST -5,
TIME_CRITICAL -10. They are applied only when the process may lower a nice value again (the reason is under the
priority calls of `runtime/platform/platform.h`).

`may_renice()` keeps the lowest nice value this process may set, 100 until probed: from `RLIMIT_NICE` (20 less the
limit, -20 without one), and CAP_SYS_NICE shows as being allowed to set a nice value below the limit's, which the probe
tries on the calling thread (one below its current value, then back) and takes as -20. A change is made only when that
lowest value is 0 or below and the wanted nice value is not below it.

### `plat_timer_1ms()`, `plat_power_full()`

The timer slack (50 us by default) is how late the kernel may wake a sleeping thread to batch wakeups; new threads
inherit it from the thread that creates them. `plat_timer_1ms()` sets it to 1 us (`PR_SET_TIMERSLACK` 1000 ns).
`plat_power_full()` returns 0: there is no per-process power throttling to opt out of.

### `plat_sleep_ns()`

`clock_nanosleep` wakes within the timer slack of its deadline; the last `PLAT_SPIN_NS` (100000) are spent yielding so
the deadline is met closely, as on Windows.

### `lk_sleep()`, `lk_wake()`, `plat_mutex_lock()`, `plat_mutex_lock_shared()`

Locks: a reader-writer lock in one 32-bit word, zero when free. Bit 31 (`LK_W`): a writer holds it; bit 30 (`LK_S`):
some thread sleeps on the word; bits 0-29 (`LK_R`): the readers holding it. A thread that cannot take the lock spins
`LK_SPINS` (64) times, then sets the sleeper bit and waits on the futex; every release that finds the bit clears it and
wakes all sleepers, which retry (and set it again if they still cannot get in). Readers do not enter while the sleeper
bit is set, so a waiting writer is not starved by a stream of readers.

`lk_sleep()` sets the sleeper bit on `s` and sleeps while the word still equals the result.

### `plat_lock_timing()`, `plat_mutex_lock_at()`

Lock-wait timing is Windows only: here `plat_lock_timing()` drops the note, and `plat_mutex_lock_at()` is
`plat_mutex_lock()`.

### `plat_cond_wait()`, `plat_cond_wait_ns()`, `plat_cond_signal()`, `plat_cond_broadcast()`

Condition variables: a sequence number in the first 32 bits. A waiter reads it before releasing the lock and sleeps
only while it is unchanged, so a signal between the release and the sleep is not lost.

### `plat_append_open()`

`O_APPEND` makes each write land whole at the end of the file, from any thread. A `PlatAppend` is the descriptor + 1.

### `sync_dir_of()`

Flushes the directory holding `path`, so a rename into it survives a crash.

### `plat_lock_file()`

An exclusive, non-blocking `flock`. The descriptor stays open until `plat_unlock_file()` or the process ends. Up to
four locks are remembered by path.

### `write_swap()`, `plat_file_swap_begin()`

`write_swap()`: `path.<pid>.<tid>.tmp` renamed over `path`; `durable`: the file and then its folder are synced to the
disk. A rename replaces a read-only file here (only the folder's permission counts) but not on Windows: a file the
player made read-only stays as it is on both, so a `path` that exists and is not writable fails the call.
`plat_file_swap_begin()` keeps a read-only file the same way.

`plat_file_swap_begin()`: `posix_fallocate` allocates the `n` bytes on the disk: a full disk fails here and not at a
page of the mapping, where it would be a bus error.

### `plat_file_set_readonly()`

The owner's write permission stands for Windows' read-only attribute.

### Virtual memory and the write watch

Reservations are `PROT_NONE` `MAP_NORESERVE` mappings; commit is `mprotect` read-write, decommit `MADV_DONTNEED`
(private anonymous pages read zero afterwards) and `PROT_NONE`. Linux keeps no committed/reserved distinction, so a
byte per guest page records it (`pstate`, mapped lazily: only the pages touched cost memory). `PG` is 4096, `PS_LIMIT`
2^39. The states: `PS_FREE`, `PS_RESERVED`, then the committed ones `PS_NONE`, `PS_READ` and `PS_RW`, which are
`PLAT_PROT_*` + `PS_NONE`. `vm_lock` guards the tables.

Write watch: a userfaultfd in asynchronous write-protect mode (Linux 6.7) over the watched reservations. The kernel
clears a page's protection on the first write without a round trip to us, and `PAGEMAP_SCAN` lists the pages whose
protection was cleared and protects them again. Without it (an older kernel), `plat_mem_written()` reports the ranges
as untracked and the back end uploads them in full. `ww_fd` and `pm_fd` are the userfaultfd and `/proc/self/pagemap`;
-1 when the write watch is unavailable.

`ww_open()` runs at the first watched reservation, once (`ww_tried`; it used to run again at each watched reservation
after a failure). It keeps the kernel's version (`uname`), the error and the verdict (`ww_release`, `ww_why`,
`ww_result`, from `write_watch_note.h` `ww_verdict()`) for `plat_write_watch_note()`, which `runtime/memory.c`
`reserve_at()` logs. Without the write watch the game uploads its whole memory every frame: about 2.6 GiB a frame and
1 fps, as a player on an older kernel reported. The line used to go to stderr only, which the log file does not hold.
`NX2SARIA_TEST_NO_UFFD`, read from the environment, skips the probe: `1` fails it as a kernel without the call would
(ENOSYS) with the real version, a version string fails it as that kernel (EINVAL), so a run on a new kernel shows
both lines.

### Guest memory as a memfd's

`NX2SARIA_DMABUF`, read from the environment, since the reservations come before the settings file is read: on by
default where `/dev/udmabuf` opens, udmabuf's size limit is 1 GiB or more and NVIDIA's kernel module is not loaded; 0
off, 1 on wherever it can be. What it is for and what it measured: [building.md](../building.md#linux-in-progress).

The watched reservations are `MAP_SHARED` mappings of one memfd, each at its own offset, so that `/dev/udmabuf` can
make a dma-buf of the game's own pages and the GPU driver import it (the dma-buf imports of `runtime/vk/vk_memory.c`).
A host-pointer import is a userptr object to amdgpu, which the kernel looks up page by page again at every submission
and invalidates whenever the write watch or a freeze changes the pages' protection; a udmabuf pins the memfd's pages
once and never looks at the process's page tables, so protection changes leave the GPU's mapping alone. Everything else
stays as it was: the write watch, the freezes and the access faults work the same on a shared mapping.

Two things differ from private memory, and both are handled here under `vm_lock`:

- Decommitted pages must read zero when committed again. `MADV_DONTNEED` leaves a shared mapping's pages in the memfd,
  so a decommit punches them out of it (`fallocate`) instead. A page a dma-buf pins cannot go that way: the dma-buf
  would keep the old page while the CPU got a new one, and the GPU and the CPU would no longer see the same memory.
  Those pages are zeroed in place (`gm_decommit()`).
- A dma-buf pins every page of its range, allocating the ones nothing has touched. When it is gone
  (`plat_mem_dmabuf_done()`) the pages of its range that are not committed and that no other dma-buf pins are punched
  out, so they cost no memory.

`linux/udmabuf.h` is included where the headers have it; otherwise the file defines the kernel's ABI
(`include/uapi/linux/udmabuf.h`: `struct udmabuf_create`, `UDMABUF_FLAGS_CLOEXEC`, `UDMABUF_CREATE`) itself.

The state, all under `vm_lock`: `gm_state` (0 not decided, 1 the watched reservations are the memfd's, -1 not); `gm_fd`
and `gm_udev`, the memfd and `/dev/udmabuf`; `gm_size`, the bytes of the memfd handed out, and `gm_limit`, udmabuf's
size limit; `gm_res`, the shared reservations and their offsets in the memfd (`GM_RES_MAX` 4); `gm_pins`, the ranges
live dma-bufs pin, one entry per dma-buf; `gm_note`, why the reservations are not a memfd's ("" when
`NX2SARIA_DMABUF=0` or they are).

### `udmabuf_limit()`

The largest dma-buf the udmabuf ioctl accepts, in bytes: the module's `size_limit_mb` parameter
(`/sys/module/udmabuf/parameters/size_limit_mb`), or the module's default of 64 MiB where the parameter cannot be read.

### `gm_open()`

Called with `vm_lock` held: the first watched reservation decides.

- Unset, it is on where it can work and pays. NVIDIA's driver imports host pointers without amdgpu's costs
  (`runtime/vk/vk_memory.c` `mirror_mode()`), and its import of these dma-bufs has not been tried, so where its kernel
  module is loaded (`/sys/module/nvidia`) the memory stays private. `NX2SARIA_DMABUF=1` makes it a memfd's anyway.
- `/dev/udmabuf` must open: many systems give the device to root and group kvm only. The failure is logged
  (`platform: guest memory as dma-bufs (NX2SARIA_DMABUF): ...: guest memory stays private`).
- The game's largest pools need about 300 MiB in one dma-buf: below a `size_limit_mb` of 1024 the back end would keep
  the shadow copies anyway, so unasked the memory stays private.
- The memfd is `nx2navi-guest`, made with `MFD_CLOEXEC | MFD_ALLOW_SEALING`.

### `gm_find()`, `gm_pinned_run()`, `gm_decommit()`

`gm_find()`: the shared reservation holding `[addr, addr + n)`, -1 when none. `gm_pinned_run()`, with `vm_lock` held:
whether `x` is pinned by a live dma-buf; `*end` gets the end of the run of `[x, limit)` that is the same.
`gm_decommit()`, with `vm_lock` held: a decommit of `[addr, addr + n)` in shared reservation `gi`. Unpinned runs are
punched out; in a pinned run only the committed runs are zeroed (made writable first when they are not), since a
reserved page is zero already.

### `plat_mem_reserve()`

A kernel without `MAP_FIXED_NOREPLACE` may put the mapping elsewhere; it is then unmapped and the call fails. For a
shared reservation the memfd grows by the reservation; shrinking is sealed off (`F_SEAL_SHRINK`, at the first one), as
udmabuf requires. When the memfd cannot grow or be sealed, the reservation is made private instead and the log says so
(`platform: the memfd for ... failed (...): private memory there`).

Transparent huge pages would populate (and count as written) 2 MiB at the first write of one page; writes are tracked
per 4 KiB page, as on Windows, so a watched reservation gets `MADV_NOHUGEPAGE`.

### `plat_mem_commit()`

In a shared reservation the commit runs under `vm_lock`: `plat_mem_dmabuf_done()` punches out the pages it finds not
committed.

### `plat_mem_prepare_write()`

`PAGEMAP_SCAN` observes cleared protection as written, just as after an asynchronous write fault.

### `plat_mem_written()`

Written means: write-protection cleared, on a populated page (present or swapped out) that is not the shared zero page.
A page never touched since its commit has no protection marker either and counts as written to the kernel; a read maps
the zero page there. Neither is a write, as on Windows. The scan returns up to 256 regions a call.

### `fault_handler()`, `fault_install()`

One handler for the synchronous faults (SIGSEGV, SIGBUS, SIGILL, SIGFPE). A SIGSEGV that is a read or a write (the
page-fault error code without the instruction-fetch bit, 16) goes to the access hook first, which sees every read or
write fault; one whose error code has the write bit (2) set then goes to the write-fault hook (`runtime/memory.c` thaws
a frozen region); any other fault, in this executable or in a library (Mesa, SDL), then goes to the crash hook
(`runtime/main.c` `crash_handler()`), which reports it and ends the process. Without a crash hook the fault gets the
default action: the handler sets `SIG_DFL` and returns into the faulting instruction, which faults again. It runs on the
thread's signal stack.

### `plat_crash_context()`, `plat_crash_filter_reassert()`, `plat_crash_context_get()`

The crash notes and the last-chance filter are Windows' only: these do nothing and return 0 and NULL.

### The jump: `plat_jmp_save` and `plat_jmp_go`

The two are leaf code, in assembly, that does not move rsp before it leaves, as on Windows. `PlatJmp` is 72 bytes with
`mxcsr` at 64 (a static assert). The saved rsp is `8(%rsp)`: the caller's rsp after the call returns.

### `frame_step()`, `plat_frame_capture()`

The DWARF unwinder of libgcc (already linked for C++), frame by frame from `plat_frame_capture()`'s caller. In each
frame's callback the IP is its return address, the callee-saved registers are as that frame sees them, and
`_Unwind_GetCFA` is its rsp once the call returns (libgcc's context holds the CFA of the frame just unwound). The first
callback is `plat_frame_capture()` itself and is skipped; the walk ends after 256 frames. DWARF registers 3 and 6 are
rbx and rbp.

### `plat_file_open()`

A `PlatFile` is the descriptor + 1, so descriptor 0 is not NULL.

### `plat_fs_unlink()`

Deleting needs write access to the directory, not to the file: a read-only file goes as any other.

### `plat_fs_rename()`

`RENAME_NOREPLACE` keeps Windows' rule that a rename never replaces; a file system without it (`EINVAL`, or a kernel
without the call, `ENOSYS`) gets a check first, which another process could race but the save directories are ours.

### `plat_fs_busy_reason()`

Open files never stop a rename here, so the reason is always empty.

### `plat_library_path()`

The loaded object the loader matches to `name`, by its soname or its file name (a library preloaded under its full
file name still answers for libSDL3.so.0), without loading anything. The loader names it by the path it opened, which
may be relative or go through a symbolic link when an `LD_LIBRARY_PATH` entry is (an empty entry is the current folder):
the folder is made absolute and free of links, so that it compares with `plat_exe_path()`'s, and the file name is kept
as it was opened.

### `plat_thread_id()`

Cached per thread: `plat_rmutex_lock()` asks on every acquire, hundreds of thousands of times a second in play, and
`gettid` is a full syscall on Linux (Windows reads `GetCurrentThreadId` from the TEB). A thread's id never changes, and
the runtime starts child processes with `posix_spawn` only, so no forked copy inherits a stale id. It is `noinline`: a
fiber may resume on another thread, and a caller that inlined this could keep the address of `tid` it computed before a
`plat_fiber_switch()`; as a call it reads the slot of the thread it runs on.

### `locale_tag()`

The first set variable of `names` ("ru_RU.UTF-8") as a BCP-47 tag ("ru-RU") into `tag`; "" for C and POSIX.
`plat_ui_language()` reads `LC_ALL`, `LC_MESSAGES`, `LANG`; `plat_user_locale()` reads `LC_ALL`, `LC_TIME`, `LANG`.

### `plat_font_path()`

Noto CJK as the common distributions and SteamOS install it; one collection (`NotoSansCJK-Regular.ttc`) covers every
CJK language.

### `plat_os_version()`

`PRETTY_NAME` of `/etc/os-release` and the kernel release: "SteamOS (Linux 6.16.12-valve)".

### `plat_console_setup()`

A Windows console's settings only: 0 here.

### `plat_exit()`

In an instrumented build (`NX2SARIA_PGO_GEN` or `NX2NAVI_PGO_GEN`: `--pgo gen` of `tools/make_game.py` or
`tools/build_game.py`) compiler-rt's profile runtime writes its counters from an `atexit` handler, which `_exit` skips;
`__llvm_profile_dump` writes them once and makes later writes no-ops, so `plat_exit()` calls it.

### `plat_exit_note_setup()`, `plat_exit_note_modules()`, `plat_exit_orderly()`, `plat_stack_guarantee()`

The note of an end the game did not ask for is the Windows executable's TLS callback (`runtime/platform/win32.c`
`exit_note_tls()`); nothing here. `plat_stack_guarantee()` is empty too: the crash report runs on the signal stack crash
handling sets up.

### `plat_process_uptime_ns()`

Field 22 of `/proc/self/stat` is the start time in clock ticks after boot; `CLOCK_BOOTTIME` is now after boot. The
fields are read from after the last `)`: the command name may hold spaces.

### `plat_localtime()`, `plat_gmtime()`

`PLAT_TIME_MAX` (32535215999) keeps the range of the Windows CRT's `localtime_s` and `gmtime_s` (1970 to 3000), which
glibc exceeds: the game's time service reports times outside it as invalid on both hosts.

### `plat_run_self()`

The arguments come NUL-separated from `/proc/self/cmdline`.

### `plat_clipboard_get()`

`SDL_GetClipboardText` gives "" when there is none, never NULL but on failure. A text longer than `cap` is cut where a
character starts, not inside one.

### `plat_open_folder()`

`xdg-open` picks the desktop's file manager; it runs detached (`POSIX_SPAWN_SETSID`), so a slow answer does not hold up
the caller, and a thread of its own reaps it (`reap_main()`) so it does not stay a zombie.

### `find_cpu_temp()`, `find_gpu()`, `plat_load_open()`, `plat_cpu_load()`

The machine's load: `/proc/stat` for the machine's CPU time and the process's CPU clock (`plat_process_cpu_ns()`) for
its own, hwmon for the temperature, the amdgpu sysfs files of the render device for the GPU. `struct PlatLoad` keeps the
previous call's figures (`total` 0 before the first), the GPU's `/sys/class/drm/cardN/device` (empty when unknown) and
the CPU's hwmon `temp1_input` (empty when unknown).

- `find_cpu_temp()`: the hwmon device of the CPU package: k10temp or zenpower (AMD), coretemp (Intel), or the first
  that has a `temp1_input`.
- `find_gpu()`: the DRM card with `gpu_busy_percent` (amdgpu) on PCI bus `bus`, the game's device. `device` links to
  the PCI function, "0000:03:00.0": domain, bus, device, function. With `bus` -1 (no `VK_EXT_pci_bus_info`): of
  several, the one with the most VRAM in use. None when the game's GPU has no such file (NVIDIA): the overlay then takes
  its load and VRAM from NVML, not from an idle integrated GPU.
- `plat_load_open()`: Linux has no adapter LUIDs; the PCI bus names the card.
- `plat_cpu_load()`: of the first eight fields of the `cpu` line, idle and iowait are idle; guest time is already
  inside user time. The process time is in ns, the machine's in jiffies summed over every CPU.

### `proc_text()`

A small `/proc` file read whole into `buf` (NUL-terminated) through the descriptor calls alone: nothing here
allocates, so `plat_pressure()` may be asked in a fatal error's report. 0 when it cannot be read (a kernel without PSI
refuses the read of `/proc/pressure/*`).

### `plat_handle_count()`, `plat_handle_census()`

`plat_handle_count()` counts the entries of `/proc/self/fd` less the descriptor of this walk.

`plat_handle_census()`: each descriptor's link, by kind: "socket:[N]" and "pipe:[N]" by their prefix, an anonymous
inode by its kind ("anon_inode:[eventfd]" is eventfd), a device by its `/dev` folder, any other path as file. A
descriptor closed meanwhile is skipped. When there is no room for a new kind, the last entry takes the rest.

### `plat_heap_info()`

glibc's `mallinfo2` (2.33 and later) sums every arena: `arena` is what the arenas took with `brk` and `mmap`, `hblkhd`
the blocks mmapped one by one, `uordblks` what the arenas handed out. Other C libraries leave the figures unknown.

### Memory limits

Linux overcommits, so memory that runs short does not fail an allocation: the OOM killer ends the process, when it
outgrew its cgroup's `memory.max` or when the machine's RAM is gone, and Committed_AS against CommitLimit (which binds
only with `vm.overcommit_memory = 2`) says neither. `plat_commit_info()` reports the limit with the least headroom of

- each cgroup v2 level from this process's cgroup up to the root that sets `memory.max` or `memory.high` (the smaller;
  the kernel throttles at `memory.high`), against that level's `memory.current` less its file cache (`memory.stat`
  `file` without `shmem`): the kernel drops the cache before it ends anything, so a cache that filled the limit is no
  shortage;
- the machine's RAM: MemTotal against MemTotal less MemAvailable;
- with `vm.overcommit_memory = 2` only, where an allocation does fail: CommitLimit against Committed_AS.

The process's share is its resident anonymous memory, the nearest to Windows' private bytes. The sweeps that ask run
several times a second, so a read is reused for 250 ms. The first read keeps every limit it found as text, which the
runtime logs (`plat_commit_note()`): stderr alone does not reach the log file when the launcher starts the game.

### `cg_locate()`, `cg_file()`, `cg_bytes()`, `cg_stat()`

`cg_leaf` is this process's cgroup v2 directory ("" when it has none), `cg_floor` the length of the cgroup2 mount point
at its start.

- `cg_locate()`: the process's cgroup v2 directory: its path from `/proc/self/cgroup` (the `0::` line) below the root of
  the cgroup2 mount that `/proc/self/mountinfo` shows (a container's own cgroup is mounted as the root), else the
  nearest directory above it that exists. A mount of a cgroup below the root of the hierarchy shows the process's path
  without that prefix.
- `cg_file()`: a cgroup file into `buf` as text; 0 when it is missing (the controller is not enabled there).
- `cg_bytes()`: a byte limit file: its value in bytes, -1 for "max" or when missing.
- `cg_stat()`: a `memory.stat` counter in bytes, 0 when it is missing.

### `commit_pick()`, `note_add()`, `commit_cgroups()`, `commit_machine()`, `commit_read()`

`CommitSnap`: `proc`, `used` and `limit` in MB (-1 when unknown), `who` (the limit that is the tightest, for the first
read's log line) and `at` (`plat_now_ms()` of the read, 0 before the first).

- `commit_pick()` makes (limit, used) the snapshot's when its headroom is smaller than the one held.
- `note_add()` appends one limit found to the note of the first read ("; " between them; a full note, `NOTE_MAX` 2048,
  takes no more).
- `commit_cgroups()`: the cgroups from the process's up to the root that limit memory. `memory.stat` starts with `anon`
  and `file`; the counters needed lie in its first lines, so 4096 bytes of it are read.
- `commit_machine()`: the machine's RAM, and the system's commit when `vm.overcommit_memory = 2`.
- `commit_read()` writes `commit_note`, the first read's limits; it is not written after that.

### `plat_commit_note()`, `plat_commit_counts_vram()`, `plat_page_file_managed()`

`plat_commit_note()`: the first read wrote the note under `plat_commit_info()`'s lock; taking that lock here (by calling
it) orders the copy after it. `plat_commit_counts_vram()` is 0: the cgroup's `memory.current` and MemAvailable count
system memory only, and a graphics card's own memory is in neither. `plat_page_file_managed()` is -1: swap is not a page
file Windows sizes, and nothing here advises on its size.

### `plat_child_lower()`, `plat_child_write()`, `plat_child_read()`

`struct PlatChild`: `in` is our write end, `out` our read end. `plat_child_lower()`: a higher nice value needs no
privilege. `plat_child_write()`: a child that died makes the write fail with `EPIPE`; `SIGPIPE` is ignored for the
process (`runtime/main.c`), as Windows has none. `plat_child_read()` polls for `PLAT_CHILD_READ_MS`: a child that never
answers fails the read with -2 after it (a timeout), a failed poll with -1; the reason is under `runtime/platform/win32.c`
`plat_child_read()`.

### `plat_ipc_connect()`, `plat_ipc_read()`, `plat_ipc_write()`

Local IPC: Discord's Rich Presence Unix domain socket. When nothing listens at a path, the next directory may have it.
A read of 0 bytes means the peer closed the socket. The write uses `MSG_NOSIGNAL`: a closed peer is an error, not a
SIGPIPE.

### `addr_to_sockaddr()`, `plat_net_resolve()`, `plat_udp_open()`, `plat_udp_send()`

`addr_to_sockaddr()`: the socket address of `a` for a socket of family `v6` (1: `AF_INET6`, 0: `AF_INET`); 0 when an
IPv4 socket cannot reach it. `plat_net_resolve()` takes an IPv4 answer first. `plat_udp_open()` falls back to IPv4 only,
asked for or all the machine has. `plat_udp_send()`: a full buffer, a missing route or an ICMP error is a lost
datagram; a datagram too long or a socket that is not ours to use can never be sent (`EMSGSIZE`, `EBADF`, `EINVAL`,
`EAFNOSUPPORT`, `ENOTSOCK`).

### `plat_tcp_listen_loopback()`, `plat_tcp_accept()`, `plat_tcp_recv()`, `plat_tcp_send()`

- `plat_tcp_listen_loopback()` sets `SO_REUSEADDR`: a restart within a minute finds the last run's closed connections
  still in TIME_WAIT on this port; Linux lets the new listener bind beside them, and still refuses a second listener on
  the port.
- `plat_tcp_accept()`: a failed `accept4` means the peer gave up between the poll and the accept, or `EINTR`: the caller
  asks again. Any other error is reported: no descriptor or buffer left, where the listener stays readable, so the
  caller waits.
- `plat_tcp_recv()`: 0 bytes means the peer closed its end.
- `plat_tcp_send()` uses `MSG_NOSIGNAL`: a closed peer is an error, not a SIGPIPE. When the peer's buffer is full it
  waits for room until the deadline.

### `process_start()`, `spawn_close_from()`, `spawn_mask_whole()`, `plat_restart()`, `plat_restart_wait()`

Restart. `NX2SARIA_RESTART_WAIT` (`RESTART_WAIT`) names the process that called `plat_restart()`, as its id and start
time (field 22 of `/proc/<pid>/stat`, clock ticks after boot): a process that has only reused the id is not waited for.
Waiting keeps the new process off the old one's files (the log it moves at start, the saves) until the old one is gone.
The new process gets no descriptor past stderr (glibc 2.34's closefrom file action), and the default signal mask and
dispositions, whatever the calling thread had.

- `process_start()`: the start time of process `pid` (field 22 of its `/proc` stat line), 0 when it is gone. The
  command name may hold spaces, so the fields are read from after the last `)`.
- `spawn_close_from()`: glibc 2.34 added `posix_spawn_file_actions_addclosefrom_np`, and the release links against the
  Steam Runtime's 2.31: a weak reference, used when the running glibc has it. Without it the child inherits the open
  descriptors.
- `spawn_mask_whole()`: the calling thread's mask with every processor this process uses, for the time of a
  `posix_spawn`: a child starts with the mask of the thread that spawned it, and after `plat_cpu_reserve()` no thread
  has them all (the recording thread has its core alone, every other thread lacks that core). The game `plat_restart()`
  starts would begin without the core, take the next fast one for its own recording thread, and so have one core fewer
  after every Restart. 1 when the mask was widened: `*saved` is then the mask to put back.
- `plat_restart()` takes the arguments NUL-separated from `/proc/self/cmdline`.
- `plat_restart_wait()` removes the variable, so the processes this one starts do not wait again. The descriptor
  (`pidfd_open`) is taken first: a start time that still matches then proves it is that process, not a new one with the
  id. It reads as ready once the process has exited, reaped or not. No descriptor: the process is already gone.

## `runtime/platform/win32.c`

The Windows implementation of `runtime/platform/platform.h`: `CreateThread`, `QueryPerformanceCounter`,
high-resolution waitable timers, SRW locks and condition variables. A `PlatMutex` holds an `SRWLOCK` and a `PlatCond` a
`CONDITION_VARIABLE` (two static asserts on their sizes).

The includes that have a reason: `malloc.h` for `_get_heap_handle` (`plat_heap_info()`); `winsock2.h` before
`windows.h`, which would otherwise pull in the old `winsock.h`; `aclapi.h` for `SetNamedSecurityInfoW`
(`plat_file_private()`); `tlhelp32.h` for `CreateToolhelp32Snapshot` (`plat_watch_set()`).

The line "Contains code Copyright (c) 2026 Peiikai" above `plat_mem_written()` is the licence notice of code this file
contains, and stays in the file.

### `thread_main()`, `thread_handles_close()`, `plat_thread_name()`

`struct PlatThread`: `state` is 0 running and owned by its maker, 1 detached, 2 its function has returned. The name is
copied into the thread's own `thread_name` at its start, as a detached thread's struct may go.

What a thread made here keeps for itself is given back when its function returns: its handles (the sleep timer of
`plat_wait_ns()`, the events of `plat_file_pread()` and `plat_file_pwrite()`; `thread_handles_close()`), what the other
files keep per thread (`plat_thread_end_hook()`) and, for a detached thread, its `PlatThread`. The game makes and ends
threads all through play, each a host thread: nothing closed them, so every ended thread that had slept, read or
written left up to three kernel handles behind for the rest of the run. A guest thread ends by returning here too
(`runtime/hle/kernel.c`: ExitThread jumps back to the thread's start).

A thread detached while it ran frees its own struct in `thread_main()`: nobody else does.

### `plat_stack_guarantee()`

A stack overflow leaves the faulting thread a guard page's worth of stack, about 12 KiB of which a crash handler gets,
and the report (`runtime/main.c` `crash_handler()`) needs more: `rt_log`'s 2 KiB buffers, the frame walk, and during the
boot's builds a file write first (`plat_file_write()`'s frame is about 4 KiB). It faulted again inside the report, and
the process ended with no FATAL line (the audit's probe: a 16 MiB-reserve thread recursing in 1 KiB frames). With
32 KiB kept back (`SetThreadStackGuarantee`), the probe's report ran whole with room to spare. The guarantee belongs to
the running fiber, so `fiber_start()` asks for it too: a guest fiber's stack is its own (`runtime/hle/kernel.c`
`kern_fiber_init()`). Each thread and fiber commits that much more of its stack.

### `plat_thread_detach()`

The thread still reads `t->fn` and `t->arg` when it starts, so the struct goes when the thread's function has returned
(`thread_main()`), or here when it already has.

### `plat_cpu_cores()`, `plat_cpu_cache_die()`

`plat_cpu_cores()` takes the lowest processor of each core's mask in group 0. `plat_cpu_cache_die()` reads every L3,
also those outside the process's set, and masks the result with the set at the end.

### `plat_intr_self()`, `plat_intr_stop()`, `plat_intr_resume()`

Thread interruption: `SuspendThread`, `GetThreadContext` / `SetThreadContext`, `ResumeThread`. `struct PlatIntr` has
its `CONTEXT` first, which wants 16-byte alignment. The context is read with `CONTEXT_CONTROL | CONTEXT_INTEGER` and
set back whole, so the kernel restores every general-purpose register.

### `watch_unwind()`, `watch_veh()`, `watch_apply()`, `plat_watch_set()`

Hardware data watchpoints: the debug registers DR0-DR3 hold the addresses and DR7 enables them, each for reads and
writes (R/W 11) of its length. A thread's debug registers are part of its context: a helper thread (`watch_apply()`)
lists the process's threads, then stops each, sets them with `SetThreadContext` and lets it go on; it opens every handle
before it stops anything and takes no lock while a thread is stopped, so a thread stopped while holding the heap or
loader lock cannot block it: between the stop and the go-on there is nothing but the context call. The caller waits for
the helper (10 s at most), which also sets the caller's own registers. An access traps after the instruction that made
it, as `EXCEPTION_SINGLE_STEP` with the slot's bit in DR6; `watch_veh()` hands it on and writes the current set into the
context it continues with. So a thread that traps after its registers were cleared, or whose continue would restore
registers from before a change, ends up with the current set too.

- `watch_unwind()`: `frames[0]` is the instruction after the access, then the return addresses the unwind tables give;
  for a leaf function the return address is on top of the stack.
- `watch_veh()`: a single step with no bit in DR6 is a trap-flag step, not a watchpoint, and is passed on.
- `watch_apply()` handles up to `WATCH_THREADS` (4096) threads.
- `plat_watch_set()`: DR7's LEN encoding is 2 for 8 bytes, 3 for 4, 1 for 2, 0 for 1. The hook and the registers are in
  place before `watch_on` says so, and a clear takes `watch_on` away first.

### `plat_fiber_self()`, `fiber_start()`, `plat_fiber_new()`

Windows fibers with `FIBER_FLAG_FLOAT_SWITCH`: the MXCSR and the x87 control word switch with them.

### The jump: `plat_jmp_save` and `plat_jmp_go`

`PlatJmp` holds rbx, rbp, rdi, rsi, r12-r15, the caller's rsp and the return address at 0..72, MXCSR at 80, the x87
control word at 84 and xmm6-xmm15 from 96: the Win64 callee-saved state (256 bytes; a static assert). Both are leaf code
that does not move rsp before it leaves, so they need no unwind data. The saved rsp is `8(%rsp)`: the caller's rsp after
the call returns.

### `plat_frame_capture()`

The x64 unwind tables, frame by frame within the thread's stack limits; a leaf has its return address on top of the
stack. xmm6-xmm15 are consecutive in `CONTEXT`, so they are copied in one piece.

### `plat_cpu_reserve()`

The calling thread gets a core of its own through CPU sets: the process's default sets, which every thread without
sets of its own runs on (those started later too), leave the core out, and the caller's selected sets are that core.
The core is the highest-numbered one of the fastest efficiency class (a performance core on a hybrid CPU), with both
its logical processors when it has two; only processors in group 0 and in the process's affinity mask count. CPU sets
are soft: the scheduler may still use the core when nothing else wants it. It returns the logical processors reserved,
0 when Windows has no CPU sets (before Windows 10) or fewer than two logical processors are left for the other threads;
`*which` (when not NULL, `size` bytes) receives their numbers, such as "10,11". When the caller's sets cannot be set,
the process goes back to every processor.

### `plat_cpu_usable()`

The processors of the process's affinity mask: what `start /affinity`, Task Manager's "Set affinity" or a launcher
leaves this process. A process that spans processor groups (more than 64 logical processors) gets no mask from
`GetProcessAffinityMask` (both masks 0): every active processor counts then.

### `plat_thread_priority()`, `plat_thread_set_priority()`, `plat_thread_priority_get()`

`PLAT_PRIO_*` are the `THREAD_PRIORITY_*` values.

### `plat_timer_1ms()`

The default timer tick is 15.6 ms; a 1 ms wait would cost a full tick. `timeBeginPeriod(1)`.

### `plat_power_full()`

Windows 11 runs a process whose window is not in front (another window focused, a second monitor's overlay) at
efficiency speed (EcoQoS, the E-cores of a hybrid CPU) and may ignore its timer resolution request, which turns the
1 ms waits the pacing and short sleeps rely on into 15.6 ms ones. Opting out keeps full speed and the 1 ms timer. The
ignore-timer-resolution control (`PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION`, 0x4, defined here when the headers
lack it) needs Windows 11; on Windows 10 the call fails and execution speed alone is asked.

### `qpc_ns_init()`, `plat_now_ns()`

`qpc_ns`, the scale from counter ticks to nanoseconds, is set by a constructor before `main`, so no thread reads it
half written.

### `plat_wait_ns()`, `plat_sleep_ns()`

A high-resolution waitable timer per sleeping thread (Windows 10 1803+; one handle per thread, closed when the
thread's function returns, `thread_handles_close()`) wakes within about 0.1-0.5 ms; the last `PLAT_SPIN_NS` (300000)
are spent yielding so the deadline is met closely. Without that timer kind, `Sleep(1)` granularity applies. The due
time is relative, in 100 ns units.

`pread_ev` and `pwrite_ev` are the per-thread events of the overlapped calls of `plat_file_pread()` and
`plat_file_pwrite()`.

### `plat_lock_timing()`

`lock_note` is the calling thread's note.

### `plat_append_write()`

`Offset` and `OffsetHigh` all ones: `WriteFile` appends at the end of the file, atomically against other writers.

### `plat_lock_file()`

The file is opened with no sharing at all: a second process's open fails with a sharing violation; the handle stays
open until `plat_unlock_file()` or the process ends.

### `write_swap()`

`path.<pid>.<tid>.tmp` renamed over `path`; `durable`: flushed to the disk before the rename, which is written
through. The data is written in pieces of 1 GiB: `WriteFile` takes a 32-bit count, so a block of 4 GB or more (a
pipeline cache can grow that far) would otherwise be cut to its low 32 bits and the write refused.

### `plat_file_swap_begin()`, `plat_file_swap_end()`

The mapping object extends the new file to `n` bytes, which allocates them on the disk: a full disk fails here and not
at a page of the view. In `plat_file_swap_end()` the mapping is closed before the file is cut: the file's size can
change only once no mapping of it is left.

### `plat_file_private()`

The file or folder gets an access list of its own with one entry, full control for the account the game runs as, and
inherits none from its folder: a game folder outside the user's own folders is often readable by every account of the
PC. Administrators can still take ownership, as on any file.

### `plat_mem_reserve()`, `plat_mem_decommit()`, `plat_mem_written()`

Virtual memory: `VirtualAlloc` reservations, `MEM_WRITE_WATCH` for the watched ones. Decommitted pages come back
zeroed when committed again. `plat_mem_written()` is `GetWriteWatch` with the reset flag; page addresses are written
straight into the `uint64_t` array (a static assert on the pointer size).

### `plat_mem_dmabuf_ok()`, `plat_mem_dmabuf()`, `plat_mem_dmabuf_done()`

Guest memory as dma-bufs is Linux's: 0, -1 and nothing here.

### `fault_veh()`

A read or write access violation (`ExceptionInformation[0]` 0 or 1) goes to the access hook when there is one, then a
write to the write-fault hook; the handler is registered first, before the crash handler.

### `raw_note()`, `crash_veh()`

`raw_note()`: a line written straight to the standard error handle, for the crash path: no C runtime buffering or
locks. The line gives the exception, its address as module+offset, the data address, the thread and its reserved stack;
a second line gives the thread's crash context when it has one.

`crash_veh()` writes that note for an access violation, a stack overflow and an illegal instruction. For an access
violation at rip 0, a call through a null pointer, the call's return address is on top of the stack, in whichever module
made it: the top four stack words that lie in a module are written as "called from", and then the caller's registers
(the object a call through a table read its pointer from is often one of them). A fault outside this executable's image
is not our code and is left to the handler that expects it. For one inside, the faulting host stack is captured, so the
report can name the runtime frame that took it.

### `crash_from_runtime()`, `abort_signal()`, `invalid_parameter()`

`abort()` and an invalid parameter to a C runtime function raise no exception: the C runtime ends the process with
0xc0000409 after calling the SIGABRT or invalid-parameter handler. The report names the host frames in this
executable; the innermost is the caller of `abort` (assert) or of the C runtime function. Skipped frames: this function
and the handler that called it. The invalid-parameter handler's arguments are NULL and 0 in the release C runtime; its
code is 0xc0000417 (`STATUS_INVALID_CRUNTIME_PARAMETER`).

### `image_name()`

The name a DLL records for itself in its export directory ("amdvlk64.dll"). It is read from its mapped headers, not
through the loader, whose lock another thread may hold when the process crashes.

### `frames_in()`

The return addresses in `[lo, lo + size)` on the crashed thread's stack, innermost first: it unwinds the faulting
context with the x64 unwind tables; a function without a table entry (a leaf, or address 0 after a call through a null
pointer) has its return address on top of the stack. The walk stops at return address 0 (the thread's first frame) and
when the stack pointer leaves the thread's stack, so a damaged stack ends it instead of faulting.

### `crash_filter()`, `plat_crash_hook()`

The last-chance filter: an exception no handler took, in any module. `crash_veh()` reports faults in this executable as
they happen; this one catches the rest, which used to end the process with no line in the log: a fault in a GPU
driver's own thread or in a driver call (`vkQueueSubmit`, `vkAllocateMemory`), a C++ exception a library let escape
(0xe06d7363), a call through a null pointer (rip 0, outside every module). Windows calls it only after every handler
declined, so the exceptions drivers raise and catch themselves never get here; it costs nothing until a crash.
0xc0000374 is `STATUS_HEAP_CORRUPTION`.

`plat_crash_hook()`: `SetUnhandledExceptionFilter` replaces the C runtime's filter; the crash function never returns.

### `plat_crash_filter_reassert()`

The last-chance filter is set once, at the start, before the GPU driver, the window and audio libraries and the movie
decoder load. A DLL that sets its own filter later takes the place of `crash_filter()`, and a fault outside this
executable then ends the process with no line in the log. This puts `crash_filter()` back; when another filter had
taken its place, `who` gets the module and offset of that filter and the call returns 1.

### `plat_reexec()`

A fresh process gets a fresh layout: Windows places DLLs, heaps and thread stacks at random addresses. The parent
waits and exits with the child's code. It calls `plat_exit_orderly()` first: this process ends with `ExitProcess`, on
purpose. `CreateProcessW` may write its command line, so it gets a copy.

### `pgo_flush_thread()`, `pgo_flush_start()`

In an instrumented build (`NX2SARIA_PGO_GEN` or `NX2NAVI_PGO_GEN`: `--pgo gen` of `tools/make_game.py` or
`tools/build_game.py`) compiler-rt's profile runtime writes its counters from an `atexit` handler, which `ExitProcess`
skips; `__llvm_profile_dump` writes them once and makes later writes no-ops.

A long played session must not lose its counts to a crash or a reboot: every minute the counters are added to the
`.profraw` (the file merges, `-fprofile-generate`'s `%m`) and zeroed. Counts taken between the write and the reset are
lost. See [Profile-guided build](../building.md#profile-guided-build).

### `con_ctrl()`, `plat_console_setup()`

The console window. `con_in` is the console's input while its QuickEdit mode is off, `con_mode` the mode as it was.
`fs_save_crash_wait()` is `runtime/hle/fs_host.c`'s: a save Commit under way finishes first, 2 s at most and without a
lock; the weak empty one here is for builds without it, which have no saves.

`con_ctrl()` runs on a thread the system starts for the event. A close returns only when `on_close` does, which it must
not. Log-off and shutdown reach a process with a window as `WM_ENDSESSION` (`runtime/gfx/gfx_window_win.c`), not here.
A second event while `on_close` runs: the player wants out now. Left to the default handler, its `ExitProcess` cut a
save Commit in flight and ran every library's detach; the Commit finishes first, and the process ends with the code the
default handler gives (`STATUS_CONTROL_C_EXIT`, 0xC000013A).

`plat_console_setup()` opens `CONIN$`: the console's input even when stdin was redirected. `ENABLE_EXTENDED_FLAGS`
makes the call change the QuickEdit bit at all.

### `plat_stderr_write()`

`_fileno(stderr)` is the descriptor `fwrite(stderr)` writes to; it is negative when the process has no stderr.

### `plat_exit()`, `plat_terminate()`

`plat_exit()` puts QuickEdit back on in a console that outlives the process (the one of the shell that started it), and
flushes every open stream (`fflush(NULL)`): nothing after it runs the C runtime's own exit.

The process ends with `TerminateProcess`, without the libraries' detach calls that `ExitProcess` makes. The callers
have saved what is to be kept (`runtime/gfx/gfx_present.c` `gfx_window_closed()`: settings, pipeline cache, profile,
saves), and the game's other threads are still running when this is called. `ExitProcess` stops them wherever they are
and then runs every library's detach on this thread: NVIDIA's NGX core, which still held the device and the DLSS
features, read a dead pointer in its detach on the window thread after the window was closed (an access violation in
`_nvngx.dll` after the last save, once in 5 scripted runs that had freed and made a DLSS feature again), and the crash
handler wrote a FATAL for a run that had ended cleanly. The `ExitProcess` after it is not reached.

### `plat_exit_orderly()`, `plat_exit_note_setup()`, `plat_exit_note_modules()`, `exit_note_tls()`

An end the game did not ask for. Two runs of 2026-11-13 ended seconds after a focus loss with exit code 0, no crash
event and no line of their own: an `ExitProcess` by a module in the process, or a kill from outside, could not be told
apart. `ExitProcess` stops every other thread and then calls each module's detach, the executable's TLS callbacks
included; `TerminateProcess` calls none. The callback runs under the loader lock with the other threads gone, so any
lock they held stays held: it uses no C runtime (no formatting, allocation or streams: their locks), does not ask the
loader (`RtlPcToFileHeader` takes its module lock) and names addresses from the module list noted earlier.
`RtlCaptureStackBackTrace` and `WriteFile` take no lock of the process.

The module list is two tables of `EN_MODS` (384) entries: `en_cur` is the table the callback reads (-1: none yet),
`en_lock` orders the writers of the tables (the start, focus losses), and a note fills the table the callback does not
read.

`plat_exit_note_modules()`: each module's size comes from its own image header and its name from
`GetModuleFileNameA`. `GetModuleInformation` and `GetModuleBaseNameA` each walk the whole module list through
`ReadProcessMemory`, so a note took about 27 ms with 290 modules, on the window thread at every focus loss; this takes
about 0.3 ms. The header is read through `ReadProcessMemory` too, which fails for a module unloaded since
`EnumProcessModules` where a plain read would fault.

`exit_note_tls()`: an executable gets `DLL_PROCESS_DETACH` only as the process ends (its reserved argument is NULL
there all the same). The callback is in the executable's TLS callback list (the `.CRT$XL*` sections;
`plat_exit_note_cb` in `.CRT$XLB`), which the TLS directory the image has for its thread-local variables points to;
`_tls_used` is that directory, and a reference to it is kept.

### `plat_setenv()`

`_putenv_s`, not `SetEnvironmentVariableA`: `getenv` reads the C runtime's copy of the environment, which only the
CRT's own calls update. An empty value removes the variable.

### `plat_unix_time_ns()`

`FILETIME` counts 100 ns from 1601; 116444736000000000 of them lie before 1970.

### `plat_file_open()`, `plat_file_map()`

`FILE_FLAG_OVERLAPPED` handles: a read names its offset, and each thread waits on an event of its own, so concurrent
reads on one handle do not share the file pointer or the event. In `plat_file_map()` the mapping handle is closed at
once: the view keeps the mapping alive.

### `fs_kind_of()`, `plat_fs_kind()`

A symbolic link or junction is a link: removed as a link, never followed. Any other reparse point (OneDrive's cloud
placeholders, deduplicated files) is an ordinary file or directory whose content the file system fetches on access.

The reparse tag comes with the find data only; `plat_fs_kind()` asks `FindFirstFileW` only for a reparse point, since
it fails on a path with a trailing separator and reads wildcards.

### `plat_fs_rmdir()`

A read-only directory (copied over from another emulator's save folder) refuses `RemoveDirectoryW`: the attribute is
cleared and the call made again.

### `plat_fs_sync_dir()`

NTFS journals directory entries; flushing the directory handle is as far as Windows goes.

### `rm_load()`, `busy_files()`, `plat_fs_busy_reason()`

Restart Manager (`rstrtmgr.dll`, loaded on first use) names the processes that hold a file open. `rm.start` is set
only when all of its functions are there.

`busy_files()`: the files under `dir` (links not followed), at most `BUSY_FILES` (256) of them, each a malloc'd path.

`plat_fs_busy_reason()`: a rename needs `DELETE` access to the directory: a folder made by an elevated run can refuse
it, which gives "no permission to rename it". With more than eight programs (`ERROR_MORE_DATA`) the list is not filled,
and the text is their number. Otherwise each is the program's display name ("Windows Explorer") and process id.

### `plat_os_version()`

`GetVersionEx` reports the version the manifest asks for; ntdll's `RtlGetVersion` the real one. UBR in the registry is
the update build revision, the number after the build.

### `plat_stderr_path()`

The `\\?\` prefix of the final path is left out.

### `plat_clipboard_set()`, `plat_clipboard_get()`

A clipboard opened without a window gets no owner, and `SetClipboardData` may then fail: a message-only window of the
calling thread owns it for the call. After a successful `SetClipboardData` the system owns the memory. Reading needs no
owner window; a text that does not fit `cap` converts to 0 characters.

### `plat_open_folder()`

The shell can take a while to answer, so `ShellExecuteW` is not called on the caller's thread (the present path) but on
a thread of its own.

### `pdh_load()`, `pdh_query()`, `pdh_items()`

The machine's load: `GetSystemTimes` for the CPU, PDH (`pdh.dll`, loaded on first use) for the thermal zones and the
GPU engine and adapter memory counters that Task Manager reads. `pdh.open` is set only when all of its functions are
there.

`PdhQuery`: one PDH query with its wildcard counters (`\Object(*)\Counter`). A query whose open failed stays NULL.
`pdh_items()`: the counter's instances as a malloc'd array (free it), `*count` of them; NULL when there is no valid data
yet (a rate counter needs two collections).

### `plat_load_open()`, `plat_cpu_load()`, `inst_luid()`, `plat_gpu_load()`

`struct PlatLoad`: the previous call's times (`kernel` 0 before the first); `gpu` is the engine utilisation and the
adapter's dedicated memory; `luid` is "luid_0x<high>_0x<low>" of the adapter, lower case, empty for any.

- `plat_load_open()`: the LUID names the adapter, so the PCI bus is not used. Vulkan's `deviceLUID` is the LUID
  struct's memory: LowPart, then HighPart.
- `plat_cpu_load()`: kernel time includes idle time. The temperature is the hottest of the firmware's ACPI zones, near
  the CPU on most boards (the exact package temperature needs a kernel driver); the counter is in kelvin, and a value
  of 150 degrees Celsius or more is not taken.
- `inst_luid()`: the adapter part of an instance name, "luid_0x..._0x...", lower case, into `out` (40 wide characters;
  `luid_0x%08x_0x%08x` is 26).
- `plat_gpu_load()`: engine instances are "pid_<n>_luid_0x.._0x.._phys_<n>_eng_<n>_engtype_<type>". Per adapter and
  engine type the processes' shares add up; the load is the busiest engine type (32 types at most, capped at 100).
  Adapter memory instances are "luid_0x.._0x.._phys_<n>", in bytes. Without a LUID: the adapter that uses the most.

### `plat_pressure()`

Windows has no stall counters: the physical memory available and the system's commit against its limit.

### `plat_commit_note()`, `plat_commit_counts_vram()`

`plat_commit_note()` adds nothing: the numbers name the one limit there is, the system's commit.
`plat_commit_counts_vram()` is 1: the driver charges the device memory it allocates to the system's commit.

### `plat_page_file_managed()`

The `PagingFiles` value under `Memory Management`, one "<path> <initial MB> <maximum MB>" per page file: a path of
"?:\..." (System managed size on every drive) or no sizes or "0 0" (System managed size on that drive) leaves the size
to Windows. Read once; the setting takes effect only after a restart. A page file with its sizes set is the user's
choice: no advice then.

### `hc_type_names()`, `plat_handle_census()`

The system's handle table (`NtQuerySystemInformation`, class 64, the extended handle information,
`SystemExtendedHandleInformation`) filtered by this process's id, and the object types' names from `NtQueryObject`'s
`ObjectTypesInformation` (class 3), whose entries carry the type index the table uses (Windows 8.1 and later). The
table holds every process's handles; a census took 1.5-4.7 ms over 3.6 MiB of it on the development PC.

`HcType` is one entry of `ObjectTypesInformation`; its name's characters follow it. Its `totals[13]` (object and handle
counts, pool use, their high-water marks, invalid attributes) are not read. The buffer holds the count, then the entries
at pointer alignment. 0xC0000004 is `STATUS_INFO_LENGTH_MISMATCH`: for the type names anything else means done, or
failed; for the table it means the table outgrew the buffer (4 MiB at first), and a larger one is tried, six times at
most. When there is no room for a new kind, the last entry takes the rest.

### `plat_heap_info()`

The UCRT's malloc takes its memory from the heap `_get_heap_handle` names (the process heap); `HeapSummary` sums it.
The other heaps are only counted: one made with `HEAP_NO_SERIALIZE` has no lock to take.

### `plat_child_start()`

`struct PlatChild`: `in` is our write end, `out` our read end.

Pipes are created and handed out one child at a time (the `spawn` lock): a child started meanwhile would inherit the
other pipe ends and keep them open after their owner closed them.

Every child goes into one job that ends its processes when its last handle closes
(`JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`), which is when this process ends: a translator stuck on a program would otherwise
outlive a game that exited or crashed before stopping it. The handle is never closed by hand. A failure leaves the child
as it was before.

### `plat_child_read()`

A child that stays alive but never answers (a translator stuck on one program) used to block the reader for good, and
with it the draw or dispatch waiting for that shader: after `PLAT_CHILD_READ_MS` without a byte the read fails with -2,
apart from the -1 of a broken pipe (the child exited), and the caller stops the child without sending the request again
(`runtime/gfx/gfx_shader.c` `svc_request()`). The pipe is anonymous, so readiness is polled.

### `plat_ipc_connect()`, `plat_ipc_read()`

Local IPC: Discord's Rich Presence named pipe. `plat_ipc_connect()` returns NULL when there is no listener or the pipe
is busy: the caller retries later. In `plat_ipc_read()` a failed peek means the writer closed the pipe.

### `net_ready()`, `udp_socket()`, `plat_net_resolve()`, `plat_udp_open()`, `plat_udp_send()`, `plat_udp_recv()`

UDP sockets. Winsock starts at the first call that needs it (`net_ready()`). `SIO_UDP_CONNRESET` and
`WSA_FLAG_NO_HANDLE_INHERIT` (0x80) are defined here when the headers lack them.

- `addr_to_sockaddr()`: the socket address of `a` for a socket of family `v6` (1: `AF_INET6`, 0: `AF_INET`); 0 when an
  IPv4 socket cannot reach it.
- `plat_net_resolve()` takes an IPv4 answer first.
- `udp_socket()`: a non-blocking UDP socket of family `af` that no child process inherits, with Windows' report of an
  ICMP port-unreachable as a failed receive (`WSAECONNRESET` on an unconnected socket) switched off: a peer that is gone
  is not an error here.
- `plat_udp_open()`: Windows defaults to IPv6 only, so `IPV6_V6ONLY` is cleared; the zeroed address is `in6addr_any`.
  It falls back to IPv4 only, asked for or all the machine has.
- `plat_udp_send()`: a full buffer, a missing route or an ICMP error is a lost datagram; a datagram too long or a
  socket that is not ours to use can never be sent (`WSAEMSGSIZE`, `WSAENOTSOCK`, `WSAEINVAL`, `WSAEAFNOSUPPORT`).
- `plat_udp_recv()`: for a datagram longer than `buf`, Windows reports an error (`WSAEMSGSIZE`) but has filled `buf`.

### `tcp_wait()`, `plat_tcp_listen_loopback()`, `plat_tcp_accept()`, `plat_tcp_recv()`, `plat_tcp_send()`

- `tcp_wait()`: 1 ready, 0 none in time, -1 the socket broke.
- `plat_tcp_listen_loopback()` sets no `SO_EXCLUSIVEADDRUSE`: with it a restarted game could not bind the port while
  the last run's connections are in TIME_WAIT (up to two minutes), and the default bind already refuses a second plain
  listener on the port.
- `plat_tcp_accept()`: the accepted socket inherits the listener's non-blocking mode. A failed `accept` means the peer
  gave up between the poll and the accept: the caller asks again; any other error (no buffer or socket left) is
  reported. The handle's inherit flag is cleared: no child process gets the connection.
- `plat_tcp_recv()`: 0 bytes means the peer closed its end.
- `plat_tcp_send()`: when the peer's buffer is full it waits for room until the deadline.

### `process_created()`, `plat_restart()`, `plat_restart_wait()`

Restart. `NX2SARIA_RESTART_WAIT` (`RESTART_WAIT`) names the process that called `plat_restart()`, as its id and
creation time: a process that has only reused the id is not waited for. Waiting keeps the new process off the old one's
files (the log it moves at start, the saves) until the old one is gone; no handle is inherited, so none of them stays
open in the new one either.

`CreateProcessW` may write its command line, so `plat_restart()` gives it a copy. `plat_restart_wait()` removes the
variable, so the processes this one starts do not wait again; a process that cannot be opened is already gone.
