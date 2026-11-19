"""A memory gate for parallel compiles (stdlib only).

The builds start each compile only when the memory free right now holds it. Each object's need is estimated from its
peak resident memory in earlier builds (Estimates), or from a size model before it has been built once. The gate
(Gate in-process, `wrap` across processes for Ninja) admits a compile when the free memory, less what the running
compiles may still take and less a reserve kept for the rest of the machine, covers that need.

Command line, the rule command of each compile in a Ninja build:
    python tools/memgate.py wrap --state DIR --name OBJ -- <compiler command...>
"""
import argparse
import collections
import ctypes
import json
import os
import subprocess
import sys
import threading
import time
from pathlib import Path
from typing import NamedTuple

try:
    import hostmem
except ImportError:   # started as a script with an isolated sys.path
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import hostmem

MIB = 1 << 20
GIB = 1 << 30

# Estimates. MARGIN covers the variation of one object's peak between builds (different compiler flags, a slightly
# changed source). FLOOR is a compile's smallest believable need: the compiler's own code and data.
#
# The model, for an object not built before, is a straight line in two sizes of its C source: all its bytes, and the
# bytes of its largest function holding a jump-table branch (switch_fn, from the lift's manifest), whose switch makes
# the optimiser's memory grow much faster than ordinary code does. Peaks measured with clang 20 at -O1: a 0.01 MB
# chunk 119 MiB (the compiler alone); ordinary 3 to 3.9 MB chunks 270 to 340 MiB; a chunk whose largest function is
# 2.5 MB without a switch 276 MiB; chunks whose largest function has a switch and is 1.8 MB 465 MiB, 2.8 MB 663 MiB,
# 3.9 MB 922 MiB; main_table.c, 14.5 MB of tables, 582 MiB (the line over-estimates such data-only sources). The
# learned scale (Estimates.scale) corrects the line per machine and compiler once enough objects have been recorded.
MARGIN = 1.15
FLOOR = 64 * MIB
MODEL_BASE = 120 * MIB
MODEL_PER_BYTE = 50 * MIB / 1_000_000          # 50 MiB per million bytes of C
MODEL_PER_SWITCH_BYTE = 150 * MIB / 1_000_000  # 150 MiB per million bytes of the largest function with a switch
LEARN_TOLERANCE = 0.20          # a recorded peak still applies while the source size is within 20 % of the recorded
SWITCH_SMALL = 100_000          # ...and its switch_fn too, unless both the recorded and the new one are below this
SCALE_MIN_SAMPLES = 8           # recorded objects with a cost needed before the model's scale is learned
SCALE_RANGE = (0.5, 3.0)        # the learned scale is kept within this, so a few odd objects cannot run away with it

# The memory kept free for the rest of the machine: at least RESERVE_MIN, or RESERVE_SHARE of the total RAM.
RESERVE_MIN = 3 * GIB // 2
RESERVE_SHARE = 0.10

STDERR_TAIL = 1500              # characters of a compile's stderr that run() keeps
PIPE_GRACE = 5.0                # seconds run() still reads a compile's output after it ended (a descendant left
                                # running may hold the pipes open; the build does not wait for it)
HOLD_LOG_EVERY = 15.0           # seconds between two "holding" lines
POLL = 0.25                     # seconds between two looks at the free memory while a compile is held
WAIT_NOTE = 10.0                # seconds a wrapper waits before it says so on stderr
LOCK_POLL = 0.02                # seconds between two tries for the state folder's lock on Windows
REPLACE_TRIES = (0.05, 0.1, 0.2, 0.3, 0.4)   # pauses between renames refused on Windows (a reader has the file open)

ENV_LIMIT = 'NX2SARIA_BUILD_MEM_LIMIT'
ENV_RESERVE = 'NX2SARIA_BUILD_MEM_RESERVE'
ENV_MIN_JOBS = 'NX2SARIA_BUILD_MIN_JOBS'
ENV_GATE = 'NX2SARIA_BUILD_MEM_GATE'

WINDOWS = sys.platform == 'win32'
PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
STILL_ACTIVE = 259
ERROR_ACCESS_DENIED = 5
TH32CS_SNAPPROCESS = 0x2


def gib(n):
    return f'{n / GIB:.1f}'


# ---------------------------------------------------------------------------------------------------------------------
# Measuring a compile

class Result(NamedTuple):
    returncode: int
    stderr_tail: str
    peak_bytes: int          # peak memory of the process: resident (and on POSIX its waited-for descendants'), on
                             # Windows the larger of the working set and the private commit; 0 when unknown
    seconds: float
    peak_commit: int = 0     # Windows: peak private commit (already counted in peak_bytes); 0 elsewhere


if WINDOWS:
    import ctypes.wintypes as wt

    class _Counters(ctypes.Structure):
        """PROCESS_MEMORY_COUNTERS_EX."""
        _fields_ = [('cb', wt.DWORD), ('PageFaultCount', wt.DWORD)] + \
                   [(n, ctypes.c_size_t) for n in ('PeakWorkingSetSize', 'WorkingSetSize', 'QuotaPeakPagedPoolUsage',
                                                   'QuotaPagedPoolUsage', 'QuotaPeakNonPagedPoolUsage',
                                                   'QuotaNonPagedPoolUsage', 'PagefileUsage', 'PeakPagefileUsage',
                                                   'PrivateUsage')]

    class _ProcessEntry(ctypes.Structure):
        """PROCESSENTRY32W."""
        _fields_ = [('dwSize', wt.DWORD), ('cntUsage', wt.DWORD), ('th32ProcessID', wt.DWORD),
                    ('th32DefaultHeapID', ctypes.c_size_t), ('th32ModuleID', wt.DWORD), ('cntThreads', wt.DWORD),
                    ('th32ParentProcessID', wt.DWORD), ('pcPriClassBase', wt.LONG), ('dwFlags', wt.DWORD),
                    ('szExeFile', wt.WCHAR * 260)]

    _K32 = None

    def _kernel32():
        global _K32
        if _K32 is None:
            k = ctypes.WinDLL('kernel32', use_last_error=True)
            k.OpenProcess.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
            k.OpenProcess.restype = wt.HANDLE
            k.CloseHandle.argtypes = [wt.HANDLE]
            k.K32GetProcessMemoryInfo.argtypes = [wt.HANDLE, ctypes.POINTER(_Counters), wt.DWORD]
            k.GetExitCodeProcess.argtypes = [wt.HANDLE, ctypes.POINTER(wt.DWORD)]
            k.GetProcessTimes.argtypes = [wt.HANDLE] + [ctypes.POINTER(wt.FILETIME)] * 4
            k.CreateToolhelp32Snapshot.argtypes = [wt.DWORD, wt.DWORD]
            k.CreateToolhelp32Snapshot.restype = wt.HANDLE
            k.Process32FirstW.argtypes = [wt.HANDLE, ctypes.POINTER(_ProcessEntry)]
            k.Process32NextW.argtypes = [wt.HANDLE, ctypes.POINTER(_ProcessEntry)]
            _K32 = k
        return _K32

    def _counters(handle):
        """The memory counters of an open process handle, or None."""
        c = _Counters()
        c.cb = ctypes.sizeof(c)
        if not _kernel32().K32GetProcessMemoryInfo(handle, ctypes.byref(c), c.cb):
            return None
        return c

    def _children_map():
        """{parent pid: [child pids]} of every process now, from a process snapshot."""
        k = _kernel32()
        snap = k.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
        if not snap or snap == wt.HANDLE(-1).value:
            return {}
        out = collections.defaultdict(list)
        try:
            entry = _ProcessEntry()
            entry.dwSize = ctypes.sizeof(entry)
            ok = k.Process32FirstW(snap, ctypes.byref(entry))
            while ok:
                if entry.th32ProcessID != entry.th32ParentProcessID:
                    out[entry.th32ParentProcessID].append(entry.th32ProcessID)
                ok = k.Process32NextW(snap, ctypes.byref(entry))
        finally:
            k.CloseHandle(snap)
        return out

    def _working_set(pid):
        k = _kernel32()
        h = k.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
        if not h:
            return 0
        try:
            c = _counters(h)
            return c.WorkingSetSize if c else 0
        finally:
            k.CloseHandle(h)


def _pump(pipe, sink, keep):
    """Read a pipe to its end; write each chunk to sink (a binary stream) when given; keep the chunks when keep is a
    list (trimmed to the last STDERR_TAIL*4 bytes as it goes, enough for STDERR_TAIL characters). A pipe closed under
    it by run() ends the reading."""
    try:
        while True:
            try:
                chunk = pipe.read(65536)
            except (OSError, ValueError):
                break
            if not chunk:
                break
            if sink is not None:
                try:
                    sink.write(chunk)
                    sink.flush()
                except (OSError, ValueError):
                    sink = None
            if keep is not None:
                keep.append(chunk)
                if sum(map(len, keep)) > STDERR_TAIL * 8:
                    joined = b''.join(keep)[-STDERR_TAIL * 4:]
                    keep[:] = [joined]
    finally:
        try:
            pipe.close()
        except OSError:
            pass


def _binary(stream):
    return getattr(stream, 'buffer', stream)


def run(cmd, env=None, cwd=None, on_start=None, echo=False):
    """Run a compile and measure it: its exit code, the last STDERR_TAIL characters of its stderr, its exact peak
    resident memory and its wall time. stdout is read and dropped. on_start(popen) is called once the process runs
    (the gate reads its resident memory by pid while it compiles). With echo, stdout and stderr are also passed through
    to this process's own, as they arrive."""
    start = time.monotonic()
    # Unbuffered pipe objects: closing one while its reader thread waits in read() needs no lock the reader holds.
    p = subprocess.Popen(cmd, env=env, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0)
    err = []
    readers = [threading.Thread(target=_pump, args=(p.stdout, _binary(sys.stdout) if echo else None, None), daemon=True),
               threading.Thread(target=_pump, args=(p.stderr, _binary(sys.stderr) if echo else None, err), daemon=True)]
    for t in readers:
        t.start()
    if on_start is not None:
        try:
            on_start(p)
        except BaseException:
            p.kill()
            p.wait()
            raise
    peak = commit = 0
    reaped = False
    if hasattr(os, 'wait4'):
        # Reap the child here rather than through Popen, for its resource usage: ru_maxrss is the peak resident set of
        # the child and of the descendants it waited for. Setting returncode keeps Popen from reaping it again.
        try:
            _, status, usage = os.wait4(p.pid, 0)
            p.returncode = os.waitstatus_to_exitcode(status)
            peak = usage.ru_maxrss * (1 if sys.platform == 'darwin' else 1024)   # bytes on macOS, KiB elsewhere
            reaped = True
        except ChildProcessError:
            pass   # SIGCHLD is ignored, so the system reaped it already: Popen.wait() gives what it can, no peak
    if not reaped:
        p.wait()
        if WINDOWS:
            # The process object stays readable while Popen holds its handle, after the process has ended. Commit is
            # what runs out on Windows (the page file backs it), so the peak is the larger of the two.
            try:
                c = _counters(int(p._handle))
                if c:
                    commit = c.PeakPagefileUsage
                    peak = max(c.PeakWorkingSetSize, commit)
            except (OSError, AttributeError, ValueError):
                pass
    for t in readers:
        t.join(PIPE_GRACE)
    for pipe in (p.stdout, p.stderr):
        try:
            pipe.close()
        except OSError:
            pass
    tail = b''.join(err).decode('utf-8', errors='replace')[-STDERR_TAIL:]
    return Result(p.returncode, tail, peak, time.monotonic() - start, commit)


def _proc_children(pid):
    """The child pids of a Linux process, from the children lists of its threads."""
    out = []
    try:
        tasks = os.listdir(f'/proc/{pid}/task')
    except OSError:
        return out
    for tid in tasks:
        try:
            with open(f'/proc/{pid}/task/{tid}/children', 'rb') as f:
                out.extend(int(x) for x in f.read().split())
        except (OSError, ValueError):
            pass
    return out


def rss_now(pid, children=None):
    """Bytes resident right now in a process and all its descendants; 0 when it cannot be read (a process that has
    ended, a system without /proc). On Windows, children is a process snapshot (_children_map) to reuse for several
    processes; without it one is taken."""
    if pid is None:
        return 0
    if WINDOWS:
        try:
            if children is None:
                children = _children_map()
            total, seen, todo = 0, set(), [pid]
            while todo:
                p = todo.pop()
                if p in seen:
                    continue
                seen.add(p)
                total += _working_set(p)
                todo.extend(children.get(p, ()))
            return total
        except (OSError, AttributeError, ValueError):
            return 0
    if not os.path.isdir('/proc/self'):
        return 0
    page = os.sysconf('SC_PAGE_SIZE')
    total, seen, todo = 0, set(), [pid]
    while todo:
        p = todo.pop()
        if p in seen:
            continue
        seen.add(p)
        try:
            with open(f'/proc/{p}/statm', 'rb') as f:
                total += int(f.read().split()[1]) * page
        except (OSError, ValueError, IndexError):
            continue
        todo.extend(_proc_children(p))
    return total


def _rss_reader(rss):
    """rss for one admission decision: on Windows the default reader then shares one process snapshot."""
    if WINDOWS and rss is rss_now:
        try:
            children = _children_map()
        except (OSError, AttributeError, ValueError):
            children = {}
        return lambda pid: rss_now(pid, children)
    return rss


def total_ram():
    """Bytes of memory the build may use: the physical memory, on Linux no more than the cgroup limit it runs
    under; None when unknown."""
    if WINDOWS:
        status = hostmem._MemStatus()
        status.dwLength = ctypes.sizeof(status)
        try:
            if ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(status)):
                return status.total
        except (OSError, AttributeError):
            pass
        return None
    try:
        phys = os.sysconf('SC_PAGE_SIZE') * os.sysconf('SC_PHYS_PAGES')
    except (OSError, ValueError, AttributeError):
        return None
    limit = _cgroup_limit()
    return phys if limit is None else min(phys, limit)


def _cgroup_limit():
    """Linux: the tightest memory.max of this process's cgroup and the cgroups above it (cgroup v2), or None. A build
    run under such a limit can use no more than it, so the reserve and the rule for unknown free memory go by it."""
    tightest = None
    for _, text in hostmem.cgroup_limits():
        if text == 'max':
            continue
        try:
            tightest = int(text) if tightest is None else min(tightest, int(text))
        except ValueError:
            continue
    return tightest


def _commit_left():
    """Windows: bytes the system can still commit (ullAvailPageFile of GlobalMemoryStatusEx: RAM and page file
    together, less what is committed), or None. A compile that cannot commit fails however much RAM is free."""
    status = hostmem._MemStatus()
    status.dwLength = ctypes.sizeof(status)
    if ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(status)):
        return status.apf
    return None


def available():
    """The free memory the gate goes by: hostmem.available_ram(), and on Windows no more than the commit left
    (_commit_left), which runs out first on a PC with a small page file. None when neither can be read."""
    avail = hostmem.available_ram()
    if WINDOWS:
        try:
            left = _commit_left()
        except (OSError, AttributeError, ValueError):
            left = None
        if left is not None:
            avail = left if avail is None else min(avail, left)
    return avail


def default_reserve(total=None):
    """The memory kept free while compiles run: RESERVE_MIN, or RESERVE_SHARE of the total RAM when that is more."""
    if total is None:
        total = total_ram()
    return max(RESERVE_MIN, int(total * RESERVE_SHARE)) if total else RESERVE_MIN


def start_time(pid):
    """When a process started, as a number that differs between two processes given the same pid one after the other
    (Linux: clock ticks since boot, field 22 of /proc/<pid>/stat; Windows: its creation time). None where unknown."""
    if WINDOWS:
        try:
            k = _kernel32()
            h = k.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
            if not h:
                return None
            try:
                times = [wt.FILETIME() for _ in range(4)]
                if not k.GetProcessTimes(h, *[ctypes.byref(t) for t in times]):
                    return None
                return times[0].dwHighDateTime << 32 | times[0].dwLowDateTime
            finally:
                k.CloseHandle(h)
        except (OSError, AttributeError):
            return None
    try:
        with open(f'/proc/{pid}/stat', 'rb') as f:
            text = f.read()
        return int(text[text.rindex(b')') + 2:].split()[19])   # the fields after the name start at field 3
    except (OSError, ValueError, IndexError):
        return None


def _alive(pid, started=None):
    """Whether a process with this pid exists; when started is given, also that it is the process that started then
    (not a later one given the same pid)."""
    if started is not None:
        now = start_time(pid)
        if now is not None:
            return now == started
    if WINDOWS:
        k = _kernel32()
        h = k.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
        if not h:
            return ctypes.get_last_error() == ERROR_ACCESS_DENIED
        try:
            code = wt.DWORD()
            return bool(k.GetExitCodeProcess(h, ctypes.byref(code))) and code.value == STILL_ACTIVE
        finally:
            k.CloseHandle(h)
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    except OSError:
        return False
    return True


# ---------------------------------------------------------------------------------------------------------------------
# Estimates that learn

def model(src_bytes, switch_fn=0):
    """The model's peak for a source of src_bytes whose largest function with a jump-table branch has switch_fn bytes,
    before scale and margin."""
    return MODEL_BASE + MODEL_PER_BYTE * max(0, src_bytes) + MODEL_PER_SWITCH_BYTE * max(0, switch_fn)


def _median(values):
    s = sorted(values)
    n = len(s)
    if not n:
        return None
    return s[n // 2] if n % 2 else (s[n // 2 - 1] + s[n // 2]) / 2


def _close(new, recorded):
    return abs(new - recorded) <= LEARN_TOLERANCE * recorded


class Estimates:
    """Per-object peaks of earlier builds, kept as JSON:
    {"version": 1, "objects": {name: {"peak": bytes, "seconds": float, "src_bytes": int, "cost": int,
    "switch_fn": int}}}. cost is the bytes of ordinary C the compile's time is worth (the callers order compiles by
    it); src_bytes and switch_fn feed the size model; a lifted chunk has a cost above 0, other sources 0. Safe to use
    from several threads."""

    VERSION = 1

    def __init__(self, path=None, objects=None):
        self.path = Path(path) if path is not None else None
        self.objects = dict(objects or {})
        self._scale = None
        self._estimated = {}   # name -> (how, bytes) of this build's estimates
        self._ratios = {}      # name -> recorded peak / this build's estimate
        self._lock = threading.RLock()

    @classmethod
    def load(cls, path):
        """The estimates stored at path; empty when the file is missing or unreadable. Never raises."""
        objects = {}
        try:
            data = json.loads(Path(path).read_text(encoding='utf-8'))
            if isinstance(data, dict) and data.get('version') == cls.VERSION and isinstance(data.get('objects'), dict):
                for name, e in data['objects'].items():
                    try:
                        objects[str(name)] = {'peak': int(e['peak']), 'seconds': float(e['seconds']),
                                              'src_bytes': int(e['src_bytes']), 'cost': int(e['cost']),
                                              'switch_fn': int(e.get('switch_fn', 0))}
                    except (KeyError, TypeError, ValueError, OverflowError):   # 1e400 as a peak: int() overflows
                        continue
        except (OSError, ValueError, UnicodeDecodeError, RecursionError):   # deeply nested JSON from a damaged file
            pass
        return cls(path, objects)

    def save(self):
        """Write the file atomically: a temporary file next to it, then a rename over it. A rename Windows keeps
        refusing (another process has the file open) is retried a few times, then skipped: the estimates are only a
        help, the next save writes them. True when written."""
        path = self.path
        path.parent.mkdir(parents=True, exist_ok=True)
        tmp = path.with_name(f'{path.name}.{os.getpid()}.{threading.get_ident()}.tmp')
        with self._lock:
            text = json.dumps({'version': self.VERSION, 'objects': self.objects}, sort_keys=True)
            try:
                tmp.write_text(text, encoding='utf-8')
                return _replace(tmp, path)
            finally:
                try:
                    tmp.unlink()
                except OSError:
                    pass

    def record(self, name, peak, seconds, src_bytes, cost, switch_fn=0, keep_higher=False):
        """Keep a successful compile's peak (callers record only those). keep_higher: the compile ran while the free
        memory was below the gate's reserve (Lease.pressed), where the system swaps or compresses pages and the
        measured peak can come out low; a higher peak recorded before for the same object then stays."""
        if not peak or peak <= 0:
            return
        with self._lock:
            old = self.objects.get(name)
            if keep_higher and old is not None and old['peak'] > peak:
                peak = old['peak']
            self.objects[name] = {'peak': int(peak), 'seconds': float(seconds), 'src_bytes': int(src_bytes),
                                  'cost': int(cost), 'switch_fn': int(switch_fn)}
            self._scale = None
            if name in self._estimated and self._estimated[name][1] > 0:
                self._ratios[name] = peak / self._estimated[name][1]

    def scale(self):
        """How far real peaks are from the model: the median of peak / model() over the recorded lifted chunks (the
        objects with a cost), kept within SCALE_RANGE; 1.0 until SCALE_MIN_SAMPLES of them are recorded."""
        with self._lock:
            if self._scale is None:
                ratios = [e['peak'] / model(e['src_bytes'], e['switch_fn']) for e in self.objects.values()
                          if e['cost'] > 0]
                if len(ratios) < SCALE_MIN_SAMPLES:
                    self._scale = 1.0
                else:
                    self._scale = min(SCALE_RANGE[1], max(SCALE_RANGE[0], _median(ratios)))
            return self._scale

    def need(self, name, src_bytes, cost, switch_fn=0, default=None):
        """(bytes, how) for a compile. 'learned': the recorded peak, while the source size is close to the recorded
        one and so is its switch_fn (each within LEARN_TOLERANCE, or both switch_fn below SWITCH_SMALL): after the lift
        groups its functions anew, a chunk of the same name and size can hold other code. Otherwise 'default' when the caller gives one (sources the size model does not describe), else 'model'.
        cost is not used here; it is the caller's order of compiles and is kept with the record."""
        with self._lock:
            e = self.objects.get(name)
            if e is not None and _close(src_bytes, e['src_bytes']) and \
                    ((switch_fn < SWITCH_SMALL and e['switch_fn'] < SWITCH_SMALL) or _close(switch_fn, e['switch_fn'])):
                est, how = max(FLOOR, int(e['peak'] * MARGIN)), 'learned'
            elif default is not None:
                est, how = int(default), 'default'
            else:
                est, how = max(FLOOR, int(model(src_bytes, switch_fn) * self.scale() * MARGIN)), 'model'
            self._estimated[name] = (how, est)
            return est, how

    def stats(self):
        """How this build's estimates were made, and how good they were: for each object both estimated and recorded,
        peak / estimate (above 1: the compile took more than estimated)."""
        with self._lock:
            hows = collections.Counter(how for how, _ in self._estimated.values())
            out = {'learned': hows['learned'], 'model': hows['model'], 'default': hows['default'],
                   'scale': self.scale(), 'measured': len(self._ratios), 'median': None, 'worst': None,
                   'worst_name': None}
            if self._ratios:
                worst = max(self._ratios, key=self._ratios.get)
                out.update(median=_median(self._ratios.values()), worst=self._ratios[worst], worst_name=worst)
            return out


# ---------------------------------------------------------------------------------------------------------------------
# Admission

def owed(running_needs_and_rss):
    """Memory the running compiles may still take: for each, its need less what it already holds (all of its need
    while its process is not known yet, rss None)."""
    return sum(n if rss is None else max(0, n - rss) for n, rss in running_needs_and_rss)


def admit(running_needs_and_rss, need, avail, reserve, ceiling, running, min_jobs, max_jobs, total=None):
    """Whether a compile needing `need` bytes may start now, with `running` compiles running whose (need, rss) are in
    running_needs_and_rss. Fewer than min_jobs running: always (a build always progresses). max_jobs running: never.
    Otherwise the estimated needs together stay under the ceiling, and the free memory (avail) less what the running
    compiles still owe and less the reserve covers the need. When the free memory cannot be read (avail None), the
    needs together with the reserve must fit in the machine's RAM (total); with that unknown too, only the min_jobs
    compiles run."""
    if running < min_jobs:
        return True
    if running >= max_jobs:
        return False
    promised = sum(n for n, _ in running_needs_and_rss)
    if ceiling is not None and promised + need > ceiling:
        return False
    if avail is None:
        return total is not None and promised + need + reserve <= total
    return avail - owed(running_needs_and_rss) - reserve >= need


# ---------------------------------------------------------------------------------------------------------------------
# The gate, for a thread pool in this process

class Lease:
    """One admitted compile. started() gives the gate its process; release it through the gate (or leave its with
    block) once the compile has ended."""

    def __init__(self, gate, name, need):
        self.gate, self.name, self.need = gate, name, need
        self.pid = None
        self.order = 0            # 1 for the first compile the gate admitted, 2 for the next...
        self.held = 0.0           # seconds the gate held it for memory
        self.sampled_peak = 0     # the highest resident memory the gate's sampler saw it use
        self.lowest_avail = None  # the least free memory the gate saw while it ran (at its admission and sampling)

    @property
    def pressed(self):
        """Whether the free memory fell below the gate's reserve while it ran: its measured peak may be low then."""
        return self.lowest_avail is not None and self.lowest_avail < self.gate.reserve

    def started(self, popen_or_pid):
        pid = getattr(popen_or_pid, 'pid', popen_or_pid)
        with self.gate._cond:
            self.pid = pid

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.gate.release(self)


class Gate:
    """Admits compiles by the memory free now (see admit). Waiters are served in the order they asked; a waiter never
    passes the one before it, so a large compile held for memory is not starved by small ones. When the free memory
    falls below the reserve the gate only stops starting compiles; it never ends running ones."""

    def __init__(self, max_jobs, min_jobs=1, reserve=None, ceiling=None, avail=available, rss=rss_now,
                 clock=time.monotonic, poll=POLL, log=print, enabled=True, total=None):
        self.max_jobs = max(1, int(max_jobs))
        self.min_jobs = min(self.max_jobs, max(1, int(min_jobs)))
        self.reserve = default_reserve() if reserve is None else int(reserve)
        self.ceiling = None if ceiling is None else int(ceiling)
        self.enabled = enabled
        self.total = total_ram() if total is None else int(total)   # RAM; decides only when avail() is None
        self._avail, self._rss, self._clock, self.poll, self._log = avail, rss, clock, poll, log
        self._cond = threading.Condition()
        self._queue = collections.deque()
        self._ticket = 0
        self._running = []
        self._t0 = clock()
        self._sampler = None
        self._stop = threading.Event()
        self._last_log = None
        self._last_avail = None   # the free memory the last admission decision read
        self._suppressed = 0
        self.started_count = 0
        self.most_at_once = 0
        self.holds = 0
        self.held_total = 0.0
        self.longest_hold = 0.0
        self.longest_hold_name = None
        self.peak_rss = 0
        self.peak_rss_at = None
        self.lowest_avail = None

    def _state(self):
        """(need, rss) of each running compile; rss None for one whose process is not known yet."""
        rss = _rss_reader(self._rss)
        return [(lease.need, None if lease.pid is None else rss(lease.pid)) for lease in self._running]

    def acquire(self, name, need, cancel=None):
        """Block until a compile of this need may start; the Lease to hand back to release(). cancel: called while
        the compile waits; once it returns true the wait ends and acquire returns None (nothing to release)."""
        with self._cond:
            if self._sampler is None:
                self._sampler = threading.Thread(target=self._sample, name='memgate-sampler', daemon=True)
                self._sampler.start()
            ticket = self._ticket
            self._ticket += 1
            self._queue.append(ticket)
            held_since = None
            try:
                while True:
                    if cancel is not None and cancel():
                        return None
                    if self._queue[0] == ticket:
                        if self._decide(name, need, held_since is not None):
                            break
                        if held_since is None:
                            held_since = self._clock()
                    self._cond.wait(self.poll)
            finally:
                self._queue.remove(ticket)
                self._cond.notify_all()
            lease = Lease(self, name, need)
            lease.lowest_avail = self._last_avail
            if held_since is not None:
                lease.held = self._clock() - held_since
                self.held_total += lease.held
                if lease.held >= self.longest_hold:
                    self.longest_hold, self.longest_hold_name = lease.held, name
            self._running.append(lease)
            self.started_count += 1
            lease.order = self.started_count
            self.most_at_once = max(self.most_at_once, len(self._running))
            return lease

    def _decide(self, name, need, already_held):
        self._last_avail = None
        running = len(self._running)
        if not self.enabled:
            return running < self.max_jobs
        if running < self.min_jobs:
            return True
        if running >= self.max_jobs:
            return False
        state = self._state()
        avail = self._avail()
        self._last_avail = avail
        if admit(state, need, avail, self.reserve, self.ceiling, running, self.min_jobs, self.max_jobs, self.total):
            return True
        if not already_held:
            self.holds += 1
            self._hold_line(name, need, avail, owed(state), running)
        return False

    def _hold_line(self, name, need, avail, promised, running):
        now = self._clock()
        if self._last_log is not None and now - self._last_log < HOLD_LOG_EVERY:
            self._suppressed += 1
            return
        more = f' +{self._suppressed} more held' if self._suppressed else ''
        self._last_log, self._suppressed = now, 0
        free = 'unknown' if avail is None else f'{gib(avail)} GiB'
        ceiling = '' if self.ceiling is None else f', limit {gib(self.ceiling)} GiB'
        self._log(f'memory gate: holding {name} (needs {gib(need)} GiB; {free} free, {gib(promised)} GiB promised to '
                  f'the {running} compiles running, reserve {gib(self.reserve)} GiB{ceiling}){more}')

    def release(self, lease):
        with self._cond:
            if lease in self._running:
                self._running.remove(lease)
            self._cond.notify_all()

    def _sample(self):
        """Every poll seconds while compiles run: the least free memory and the most resident memory of all the
        running compiles together, for the summary."""
        while not self._stop.wait(self.poll):
            with self._cond:
                running = list(self._running)
                leases = [lease for lease in running if lease.pid is not None]
                busy = bool(running)
            if not busy:
                continue
            try:
                avail = self._avail()
                rss = _rss_reader(self._rss)
                sizes = [(lease, rss(lease.pid)) for lease in leases]
            except Exception:   # a reading that fails only loses one sample
                continue
            total = sum(s for _, s in sizes)
            with self._cond:
                for lease, s in sizes:
                    lease.sampled_peak = max(lease.sampled_peak, s)
                if avail is not None:
                    for lease in running:
                        if lease.lowest_avail is None or avail < lease.lowest_avail:
                            lease.lowest_avail = avail
                if avail is not None and (self.lowest_avail is None or avail < self.lowest_avail):
                    self.lowest_avail = avail
                if total > self.peak_rss:
                    self.peak_rss, self.peak_rss_at = total, self._clock() - self._t0

    def close(self):
        """Stop the sampler."""
        self._stop.set()
        if self._sampler is not None:
            self._sampler.join()

    def summary(self):
        with self._cond:
            return {'started': self.started_count, 'most_at_once': self.most_at_once, 'holds': self.holds,
                    'held_seconds': self.held_total, 'longest_hold': self.longest_hold,
                    'longest_hold_name': self.longest_hold_name, 'peak_rss': self.peak_rss,
                    'peak_rss_at': self.peak_rss_at, 'lowest_avail': self.lowest_avail, 'reserve': self.reserve,
                    'ceiling': self.ceiling, 'enabled': self.enabled, 'max_jobs': self.max_jobs,
                    'min_jobs': self.min_jobs}

    def summary_lines(self):
        s = self.summary()
        if not s['enabled']:
            return [f'memory gate: off; {s["started"]} compiles started, at most {s["most_at_once"]} at once '
                    f'(cap {s["max_jobs"]})']
        lines = [f'memory gate: {s["started"]} compiles started, at most {s["most_at_once"]} at once '
                 f'(cap {s["max_jobs"]}); reserve {gib(s["reserve"])} GiB'
                 + ('' if s['ceiling'] is None else f', limit {gib(s["ceiling"])} GiB')]
        if s['holds']:
            lines.append(f'memory gate: {s["holds"]} compiles held for memory, {s["held_seconds"]:.1f} s in all; '
                         f'longest {s["longest_hold"]:.1f} s ({s["longest_hold_name"]})')
        else:
            lines.append('memory gate: no compile was held for memory')
        if s['peak_rss_at'] is not None:
            lines.append(f'memory gate: peak memory of all compiles together {gib(s["peak_rss"])} GiB, '
                         f'{s["peak_rss_at"]:.0f} s in')
        if s['lowest_avail'] is not None:
            lines.append(f'memory gate: least free memory {gib(s["lowest_avail"])} GiB')
        return lines


# ---------------------------------------------------------------------------------------------------------------------
# Options shared by the build scripts

def add_arguments(parser):
    g = parser.add_argument_group('memory gate')
    g.add_argument('--mem-limit', type=float, metavar='GIB', default=None,
                   help=f'most memory the running compiles may be estimated to take together (or {ENV_LIMIT})')
    g.add_argument('--mem-reserve', type=float, metavar='GIB', default=None,
                   help=f'memory kept free for the rest of the machine; default 1.5 GiB or 10 %% of the RAM, '
                        f'whichever is more (or {ENV_RESERVE})')
    g.add_argument('--min-jobs', type=int, metavar='N', default=None,
                   help=f'compiles that may always run, whatever the free memory; default 1 (or {ENV_MIN_JOBS})')
    g.add_argument('--no-mem-gate', action='store_true',
                   help=f'start compiles by the job count alone: nothing then limits them by memory (or {ENV_GATE}=off)')


def _env_number(name, kind):
    text = os.environ.get(name, '').strip()
    if not text:
        return None
    try:
        return kind(text)
    except ValueError:
        raise SystemExit(f'{name}={text!r} is not a number') from None


def gate_on(args):
    """False when --no-mem-gate (args, may be None) or the environment (ENV_GATE off, 0, no or false) turns the gate
    off."""
    return not getattr(args, 'no_mem_gate', False) and \
        os.environ.get(ENV_GATE, '').strip().lower() not in ('off', '0', 'no', 'false')


def settings(args):
    """The gate's settings from the options, else the environment, else the defaults:
    {'min_jobs', 'reserve' (bytes), 'ceiling' (bytes or None), 'enabled'}."""
    limit = getattr(args, 'mem_limit', None)
    if limit is None:
        limit = _env_number(ENV_LIMIT, float)
    reserve = getattr(args, 'mem_reserve', None)
    if reserve is None:
        reserve = _env_number(ENV_RESERVE, float)
    min_jobs = getattr(args, 'min_jobs', None)
    if min_jobs is None:
        min_jobs = _env_number(ENV_MIN_JOBS, int)
    enabled = gate_on(args)
    return {'min_jobs': max(1, min_jobs if min_jobs is not None else 1),
            'reserve': default_reserve() if reserve is None else max(0, int(reserve * GIB)),
            'ceiling': None if limit is None or limit <= 0 else int(limit * GIB),
            'enabled': enabled}


def from_args(args, max_jobs, log=print):
    s = settings(args)
    return Gate(max_jobs, min_jobs=s['min_jobs'], reserve=s['reserve'], ceiling=s['ceiling'], log=log,
                enabled=s['enabled'])


# ---------------------------------------------------------------------------------------------------------------------
# The gate across processes, for Ninja

NEEDS, LEDGER, LOCK, CONFIG, PEAKS = 'needs.json', 'gate.json', 'gate.lock', 'config.json', 'peaks.jsonl'


def prepare_state(state, max_jobs, gate_settings, needs):
    """Write a state folder for `wrap`: config.json from settings() and the job cap, needs.json ({object: bytes}),
    and an empty ledger. Earlier peaks.jsonl lines are dropped."""
    state = Path(state)
    state.mkdir(parents=True, exist_ok=True)
    (state / CONFIG).write_text(json.dumps(dict(gate_settings, max_jobs=max_jobs)), encoding='utf-8')
    (state / NEEDS).write_text(json.dumps(needs), encoding='utf-8')
    (state / LEDGER).write_text(json.dumps(_empty_ledger()), encoding='utf-8')
    try:
        (state / PEAKS).unlink()
    except OSError:
        pass


def read_peaks(state):
    """The lines of peaks.jsonl, skipping any that do not parse."""
    try:
        text = (Path(state) / PEAKS).read_text(encoding='utf-8', errors='replace')
    except OSError:
        return []
    out = []
    for line in text.splitlines():
        try:
            out.append(json.loads(line))
        except ValueError:
            continue
    return out


class _Locked:
    """An exclusive lock on the state folder's gate.lock, held only while the ledger is read, decided on and written."""

    def __init__(self, state):
        self.path = Path(state) / LOCK

    def __enter__(self):
        self.f = open(self.path, 'a+b')
        try:
            if WINDOWS:
                import msvcrt
                while True:
                    self.f.seek(0)
                    try:
                        msvcrt.locking(self.f.fileno(), msvcrt.LK_NBLCK, 1)
                        break
                    except OSError:   # another wrapper has it; it holds it only for a moment
                        time.sleep(LOCK_POLL)
            else:
                import fcntl
                fcntl.flock(self.f.fileno(), fcntl.LOCK_EX)
        except BaseException:
            self.f.close()
            raise
        return self

    def __exit__(self, *exc):
        try:
            if WINDOWS:
                import msvcrt
                self.f.seek(0)
                msvcrt.locking(self.f.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                import fcntl
                fcntl.flock(self.f.fileno(), fcntl.LOCK_UN)
        finally:
            self.f.close()


def _read_json(path, default):
    try:
        return json.loads(Path(path).read_text(encoding='utf-8'))
    except (OSError, ValueError, UnicodeDecodeError):
        return default


def _replace(tmp, path):
    """os.replace, retried while Windows refuses it because another process has the file open; False when it kept
    refusing."""
    for pause in REPLACE_TRIES + (None,):
        try:
            os.replace(tmp, path)
            return True
        except PermissionError:
            if pause is None:
                return False
            time.sleep(pause)


def _write_json(path, data):
    """Write a JSON file atomically; False when it could not be written."""
    tmp = path.with_name(f'{path.name}.{os.getpid()}.{threading.get_ident()}.tmp')
    try:
        tmp.write_text(json.dumps(data), encoding='utf-8')
        return _replace(tmp, path)
    except OSError:
        return False
    finally:
        try:
            tmp.unlink()
        except OSError:
            pass


def _empty_ledger():
    return {'next': 0, 'running': [], 'waiting': []}


def _entry_alive(e):
    return isinstance(e, dict) and isinstance(e.get('pid'), int) and _alive(e['pid'], e.get('start'))


def _load_ledger(state):
    """The ledger: {'next': the next waiter's number, 'running': [entry], 'waiting': [entry]}, each entry
    {pid, start (the wrapper's process start time), need, name, ticket}, without the entries whose wrapper has ended.
    The second value says whether any were dropped."""
    data = _read_json(state / LEDGER, None)
    if isinstance(data, list):          # a ledger of running entries only
        data = dict(_empty_ledger(), running=data)
    if not isinstance(data, dict):
        data = _empty_ledger()
    led = {'next': data.get('next') if isinstance(data.get('next'), int) else 0}
    dropped = False
    for key in ('running', 'waiting'):
        entries = data.get(key) if isinstance(data.get(key), list) else []
        live = [e for e in entries if _entry_alive(e) and isinstance(e.get('need', 0), (int, float))]
        dropped |= len(live) != len(entries)
        led[key] = live
    if led['waiting']:
        led['next'] = max(led['next'], 1 + max(int(e.get('ticket', 0)) for e in led['waiting']))
    return led, dropped


def wrap(state, name, cmd, avail=available, rss=rss_now, poll=POLL, total=None):
    """Wait for the gate, run the compile, record its peak; the compile's exit code.

    Wrappers wait in the order they arrived: each takes a number in the ledger's waiting list, and only the live
    waiter with the lowest number may be admitted, so a large compile held for memory is not passed by the small ones
    Ninja keeps starting."""
    state = Path(state)
    state.mkdir(parents=True, exist_ok=True)
    config = _read_json(state / CONFIG, {})
    if not isinstance(config, dict):
        config = {}
    needs = _read_json(state / NEEDS, {})
    need = needs.get(name) if isinstance(needs, dict) else None
    if not isinstance(need, (int, float)):
        need = max(FLOOR, int(model(0) * MARGIN))
    need = int(need)
    enabled = config.get('enabled', True)
    max_jobs = int(config.get('max_jobs') or 1 << 30)
    min_jobs = int(config.get('min_jobs') or 1)
    reserve = config.get('reserve')
    reserve = default_reserve() if reserve is None else int(reserve)
    ceiling = config.get('ceiling')
    if total is None:
        total = total_ram()
    me = os.getpid()
    mine = {'pid': me, 'start': start_time(me), 'need': need, 'name': name, 'ticket': None}
    held = 0.0
    if enabled:
        first = time.monotonic()
        waited = noted = False
        try:
            while True:
                with _Locked(state):
                    led, changed = _load_ledger(state)
                    if mine['ticket'] is None:
                        mine['ticket'] = led['next']
                        led['next'] += 1
                    if not any(e['pid'] == me for e in led['waiting']):
                        led['waiting'].append(dict(mine))   # first look, or an earlier write was lost
                        changed = True
                    head = min(led['waiting'], key=lambda e: e.get('ticket', 0))
                    reader = _rss_reader(rss)
                    running = [(int(e.get('need', 0)), reader(e['pid'])) for e in led['running']]
                    free = avail()
                    if head['pid'] == me and admit(running, need, free, reserve, ceiling, len(running), min_jobs,
                                                   max_jobs, total):
                        led['waiting'] = [e for e in led['waiting'] if e['pid'] != me]
                        led['running'].append(dict(mine))
                        _write_json(state / LEDGER, led)   # a lost write only leaves this compile uncounted
                        break
                    if changed:
                        _write_json(state / LEDGER, led)
                waited = True
                if not noted and time.monotonic() - first >= WAIT_NOTE:
                    noted = True
                    free_text = 'unknown' if free is None else f'{gib(free)} GiB'
                    sys.stderr.write(f'memgate: {name} waits for memory (needs {gib(need)} GiB, {free_text} free, '
                                     f'{gib(owed(running))} GiB promised to {len(running)} running)\n')
                    sys.stderr.flush()
                time.sleep(poll)
        except BaseException:
            _leave(state, me)
            raise
        if waited:
            held = time.monotonic() - first
    try:
        result = run(cmd, echo=True)
    except OSError as e:
        sys.stderr.write(f'memgate: cannot run {cmd[0]}: {e}\n')
        result = Result(127, '', 0, 0.0)
    finally:
        if enabled:
            _leave(state, me)
    line = json.dumps({'name': name, 'peak': result.peak_bytes, 'peak_commit': result.peak_commit,
                       'seconds': round(result.seconds, 3), 'held': round(held, 3), 'rc': result.returncode}) + '\n'
    try:
        with _Locked(state):
            with open(state / PEAKS, 'a', encoding='utf-8') as f:
                f.write(line)
    except OSError as e:
        sys.stderr.write(f'memgate: cannot record the peak of {name}: {e}\n')
    rc = result.returncode
    return 128 - rc if rc < 0 else rc   # a signal's number as a shell reports it


def _leave(state, pid):
    """Take a wrapper out of the ledger. A failure is reported, never raised: the compile's exit code matters more,
    and the next wrapper drops the entry anyway once this process has ended."""
    try:
        with _Locked(state):
            led, _ = _load_ledger(state)
            led['running'] = [e for e in led['running'] if e['pid'] != pid]
            led['waiting'] = [e for e in led['waiting'] if e['pid'] != pid]
            if not _write_json(state / LEDGER, led):
                sys.stderr.write('memgate: cannot update the ledger; the next compile corrects it\n')
    except OSError as e:
        sys.stderr.write(f'memgate: cannot update the ledger: {e}\n')


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    if '--' not in argv:
        raise SystemExit('usage: memgate.py wrap --state DIR --name OBJ -- <command...>')
    split = argv.index('--')
    parser = argparse.ArgumentParser(prog='memgate.py', description=__doc__.split('\n')[0])
    sub = parser.add_subparsers(dest='what', required=True)
    w = sub.add_parser('wrap', help='run one compile through the gate')
    w.add_argument('--state', required=True, help='the gate\'s state folder')
    w.add_argument('--name', required=True, help='the object\'s name in needs.json')
    args = parser.parse_args(argv[:split])
    cmd = argv[split + 1:]
    if not cmd:
        raise SystemExit('memgate.py wrap: no command after --')
    return wrap(args.state, args.name, cmd)


if __name__ == '__main__':
    sys.exit(main())
