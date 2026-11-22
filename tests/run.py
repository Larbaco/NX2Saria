"""Run the checks under tests/ in parallel and print one summary.

Each test is a script that exits 0 when it passes. The runner finds every tests/*_check.py and tests/*_test.py and the
few others EXTRA lists, and runs each with the interpreter the runner runs with, from the repository's root. Run it
with the Python the tests need: .venv (Python 3.12, tools/requirements.txt) for most, .venv-py314 for the ones that
need compression.zstd (`--needs py:compression.zstd` picks those).

A test says what it cannot run without in comment lines of its own file, each at the start of a line:

    # needs: windows
    # needs: sdl3, py:unicorn
    # args: --cc {cc}

`needs` takes one or more requirements, comma separated:

    windows, linux   the operating system
    game             the game's files extracted under build/extracted (tools/extract.py)
    exe              the game's executable, given to the runner with --exe
    xlate            the shader translator, build/xlate/nx2navi_xlate (tools/build_xlate.py)
    gpu              a Vulkan device: on Linux a /dev/dri/renderD* node or /dev/nvidia0; on Windows taken as present
    display          a desktop to open a window on: on Linux $DISPLAY or $WAYLAND_DISPLAY (xvfb-run gives one)
    sdl3             SDL3's development files: the C compiler finds <SDL3/SDL.h> and links -lSDL3
    vulkan           Vulkan headers of 1.4 or later where the C compiler looks (CPATH counts; Ubuntu 24.04's are
                     1.3; tools/vulkan_pin.py fetches the build's into toolchain/vulkan-headers/include)
    vma              vk_mem_alloc.h where the tests look: $VMA_INCLUDE, MSYS2 UCRT64's include/vma, toolchain/vma
                     (tools/build_game.py vma_include fetches it there) or /usr/include
    pkg:<name>       a library pkg-config knows (pkg:sdl3)
    py:<module>      a Python module the runner's interpreter imports (py:unicorn, py:compression.zstd)
    file:<path>      a file or folder, relative to the repository's root
    clang            a C compiler: $CC, else clang on PATH (on Windows MSYS2 UCRT64's, C:\\msys64\\ucrt64\\bin)
    <program>        anything else: a program on PATH (git, objdump), MSYS2 UCRT64's on Windows too
    linux:<req>, windows:<req>   <req>, on that operating system only (linux:sdl3: the Linux build of the test links
                     SDL3, the Windows one does not)

A test whose requirement is missing is skipped, with the requirement as the reason. --with REQ takes a requirement as
present and --without REQ as missing, whatever the machine has.

`args` gives the test's command line. {exe} is the --exe path, {xlate} the translator, {cc} the C compiler ($CC, else
MSYS2 UCRT64's clang on Windows, else clang) and {root} the repository's root.

tests/known_failures.txt lists the tests that fail on main today, each with its reason, optionally only on some
operating systems, and the tests that fail only now and then (flaky):

    name: reason
    name [linux]: reason
    name [flaky]: reason
    name [flaky:case, flaky:other_case]: reason

A known failure still runs. It is counted apart and does not fail the run; one that passes is reported, so its line
can go (and fails the run with --strict). A flaky test counts as passed when it passes; when it fails it is reported
as flaky and does not fail the run. With cases named, only a failure of those unittest cases is flaky (the FAIL: and
ERROR: lines of the test's output); a failure of any other case is a failure.

    python tests/run.py [filters...] [--jobs N] [--timeout S] [--list] [--show-output] [--needs REQ]
                        [--with REQ] [--without REQ] [--exe PATH] [--strict]

A filter keeps the tests whose name holds it (`vk`, `cfg_rows`, `diff/`); with several, a test that holds any of them.
Each test's output goes to build/test-runs/<name>.log, and the last lines of each failure are printed after the
summary (all of it with --show-output). Exit code 0 when every test that ran passed or failed as known, 1 when one
failed or timed out, 2 for a bad command line.
"""
import argparse
import concurrent.futures
import functools
import importlib.util
import os
import re
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TESTS = ROOT / 'tests'
KNOWN_FILE = TESTS / 'known_failures.txt'
LOG_DIR = ROOT / 'build' / 'test-runs'
MSYS_BIN = Path(r'C:\msys64\ucrt64\bin')
XLATE = ROOT / 'build' / 'xlate' / ('nx2navi_xlate.exe' if os.name == 'nt' else 'nx2navi_xlate')

# Tests whose names do not end in _check or _test. Left out: tests/installer_e2e.py, a dev-mode install with the game
# files into a folder it is given, which reports steps rather than passing or failing; tests/maxwell_warp_emu.py, the
# emulator the tensor checks use.
EXTRA = ('excl_mt.py', 'maxwell_warp_emu_selftest.py', 'diff/simd.py', 'diff/atomic.py', 'diff/lift.py',
         'diff/func.py')

NEEDS = re.compile(r'^# needs:(.*)$', re.M)
ARGS = re.compile(r'^# args:(.*)$', re.M)
KNOWN_LINE = re.compile(r'^(\S+?)(?:\s+\[([^\]]+)\])?:\s*(\S.*)$')
FAILED_CASE = re.compile(r'^(?:FAIL|ERROR): (\w+) \(', re.M)   # unittest's line for each case that failed
SYSTEMS = ('linux', 'windows')
TAIL_LINES = 30


def this_system():
    return 'windows' if os.name == 'nt' else 'linux' if sys.platform.startswith('linux') else sys.platform


def discover():
    """Every test as (name, path): the name is the path under tests/ without .py, with / between folders."""
    paths = set(TESTS.glob('*_check.py')) | set(TESTS.glob('*_test.py')) | {TESTS / e for e in EXTRA}
    return [(p.relative_to(TESTS).with_suffix('').as_posix(), p) for p in sorted(paths) if p.is_file()]


def markers(path):
    """(requirements, args template) of a test file."""
    text = path.read_text(encoding='utf-8', errors='replace')
    reqs = [n.strip() for m in NEEDS.finditer(text) for n in m.group(1).split(',') if n.strip()]
    args = ARGS.search(text)
    return reqs, shlex.split(args.group(1)) if args else []


def compiler():
    if os.environ.get('CC'):
        return os.environ['CC']
    if os.name == 'nt' and (MSYS_BIN / 'clang.exe').is_file():
        return str(MSYS_BIN / 'clang.exe')
    return shutil.which('clang') or 'clang'


@functools.cache
def builds(source, *flags):
    """Whether the C compiler builds this program (and links it with flags)."""
    with tempfile.TemporaryDirectory() as tmp:
        src = Path(tmp) / 'probe.c'
        src.write_text(source)
        try:
            r = subprocess.run([compiler(), str(src), '-o', str(Path(tmp) / 'probe.exe'), *flags],
                               capture_output=True, timeout=60)
        except (OSError, subprocess.TimeoutExpired):
            return False
        return r.returncode == 0


SDL3_PROBE = '#include <SDL3/SDL.h>\nint main(void) { return SDL_GetVersion() == 0; }\n'
VULKAN_PROBE = '#include <vulkan/vulkan.h>\n#ifndef VK_API_VERSION_1_4\n#error 1.3\n#endif\nint main(void) { return 0; }\n'


def has_vma():
    places = [os.environ.get('VMA_INCLUDE'), MSYS_BIN.parent / 'include' / 'vma', ROOT / 'toolchain' / 'vma',
              Path('/usr/include')]
    return any(d and (Path(d) / 'vk_mem_alloc.h').is_file() for d in places)


def has_program(name):
    return shutil.which(name) is not None or (os.name == 'nt' and (MSYS_BIN / f'{name}.exe').is_file())


def has(req, args):
    """(present, the reason to skip when it is not) of one requirement."""
    system, sep, rest = req.partition(':')
    if sep and system in SYSTEMS:
        return (has(rest, args) if this_system() == system else (True, ''))
    forced = {r: True for r in args.with_} | {r: False for r in args.without}
    if req in forced:
        return forced[req], f'{req}: turned off with --without'
    if req in SYSTEMS:
        return this_system() == req, f'{req.capitalize()} only'
    if req == 'game':
        found = any((ROOT / 'build' / 'extracted').glob('*/exefs'))
        return found, 'needs the game files extracted under build/extracted'
    if req == 'exe':
        return args.exe is not None, "needs the game's executable (--exe)"
    if req == 'xlate':
        return XLATE.is_file(), 'needs the shader translator (tools/build_xlate.py)'
    if req == 'gpu':
        found = os.name == 'nt' or any(Path('/dev/dri').glob('renderD*')) or Path('/dev/nvidia0').exists()
        return found, 'needs a GPU'
    if req == 'display':
        found = os.name == 'nt' or sys.platform == 'darwin' or bool(
            os.environ.get('DISPLAY') or os.environ.get('WAYLAND_DISPLAY'))
        return found, 'needs a display'
    if req == 'sdl3':
        return builds(SDL3_PROBE, '-lSDL3'), "needs SDL3's development files"
    if req == 'vulkan':
        return builds(VULKAN_PROBE), 'needs Vulkan headers 1.4 or later'
    if req == 'vma':
        return has_vma(), 'needs vk_mem_alloc.h'
    if req.startswith('pkg:'):
        name = req[4:]
        found = has_program('pkg-config') and subprocess.run(['pkg-config', '--exists', name]).returncode == 0
        return found, f'needs {name} (pkg-config)'
    if req.startswith('py:'):
        mod = req[3:]
        try:
            found = importlib.util.find_spec(mod) is not None
        except (ImportError, ValueError):
            found = False
        return found, f'needs the Python module {mod}'
    if req.startswith('file:'):
        return (ROOT / req[5:]).exists(), f'needs {req[5:]}'
    if req == 'clang':
        return bool(os.environ.get('CC')) or has_program('clang'), 'needs clang'
    return has_program(req), f'needs {req}'


def missing(reqs, args):
    return [why for ok, why in (has(r, args) for r in reqs) if not ok]


def read_known(path):
    """{name: (reason, systems or None, flaky)} from the known-failures file, flaky None for a known failure, () for a
    flaky test and the case names for flaky cases; # comments and blank lines are left out."""
    known = {}
    if not path.is_file():
        return known
    for n, line in enumerate(path.read_text(encoding='utf-8').splitlines(), 1):
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        m = KNOWN_LINE.match(line)
        tags = [t.strip() for t in m.group(2).split(',')] if m and m.group(2) else []
        systems = [t for t in tags if t in SYSTEMS] or None
        flaky = [t for t in tags if t == 'flaky' or t.startswith('flaky:')]
        cases = tuple(t[len('flaky:'):] for t in flaky if t != 'flaky')
        bad = not m or len(tags) != len(systems or ()) + len(flaky) or not all(cases) or ('flaky' in flaky and cases)
        if bad:
            raise SystemExit(f'{path.name}:{n}: expected "name: reason" or "name [tags]: reason", the tags among '
                             f'{", ".join(SYSTEMS)}, flaky and flaky:<case>')
        known[m.group(1)] = (m.group(3), systems, cases if flaky else None)
    return known


def flaky_failure(log, cases):
    """Whether a failed run of a flaky test is the flakiness listed: any failure when no case is named, else one in
    which every case that failed is named."""
    if not cases:
        return True
    failed = set(FAILED_CASE.findall(log.read_text(encoding='utf-8', errors='replace')))
    return bool(failed) and failed <= set(cases)


class Runner:
    def __init__(self, timeout):
        self.timeout = timeout
        self.procs = set()
        self.lock = threading.Lock()
        self.stopping = False

    def run(self, name, cmd):
        """(state, seconds, log path): state is 'pass', 'fail' or 'timeout'."""
        log = LOG_DIR / f'{name.replace("/", "__")}.log'
        start = time.monotonic()
        group = ({'creationflags': subprocess.CREATE_NEW_PROCESS_GROUP} if os.name == 'nt'
                 else {'start_new_session': True})
        with open(log, 'wb') as out:
            with self.lock:
                if self.stopping:
                    return 'fail', 0.0, log
                proc = subprocess.Popen(cmd, cwd=ROOT, stdin=subprocess.DEVNULL, stdout=out, stderr=subprocess.STDOUT,
                                        **group)
                self.procs.add(proc)
            try:
                state = 'pass' if proc.wait(timeout=self.timeout) == 0 else 'fail'
            except subprocess.TimeoutExpired:
                kill_tree(proc)
                proc.wait()
                state = 'timeout'
            finally:
                with self.lock:
                    self.procs.discard(proc)
        if os.name != 'nt':
            kill_tree(proc)   # what the test left running in its process group
        return state, time.monotonic() - start, log

    def stop(self):
        with self.lock:
            self.stopping = True
            for proc in self.procs:
                kill_tree(proc)


def kill_tree(proc):
    if os.name == 'nt':
        if proc.poll() is None:
            subprocess.run(['taskkill', '/T', '/F', '/PID', str(proc.pid)], capture_output=True)
    else:
        try:
            os.killpg(proc.pid, signal.SIGKILL)
        except (ProcessLookupError, PermissionError):
            pass


def command(path, template, args):
    values = {'exe': str(args.exe) if args.exe else '', 'xlate': str(XLATE), 'cc': compiler(), 'root': str(ROOT)}
    return [sys.executable, str(path)] + [a.format(**values) for a in template]


def annotate(kind, text):
    if os.environ.get('GITHUB_ACTIONS') == 'true':
        print(f'::{kind}::{text}')


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('filters', nargs='*', help='keep the tests whose name holds one of these')
    ap.add_argument('--jobs', '-j', type=int, default=min(8, os.cpu_count() or 1),
                    help='tests run at once (default: the CPUs, at most 8)')
    ap.add_argument('--timeout', type=float, default=900, help='seconds one test may take (default 900)')
    ap.add_argument('--list', action='store_true', help='list the tests, their requirements and what is missing')
    ap.add_argument('--show-output', action='store_true', help="print a failure's whole output, not its last lines")
    ap.add_argument('--needs', action='append', default=[], metavar='REQ',
                    help='keep only the tests that declare this requirement')
    ap.add_argument('--with', dest='with_', action='append', default=[], metavar='REQ',
                    help='take a requirement as present')
    ap.add_argument('--without', action='append', default=[], metavar='REQ', help='take a requirement as missing')
    ap.add_argument('--exe', type=Path, help="the game's executable, for the tests that need: exe")
    ap.add_argument('--strict', action='store_true', help='a known failure that passes fails the run')
    ap.add_argument('--known', type=Path, default=KNOWN_FILE, help='the known-failures file')
    args = ap.parse_args()
    if args.jobs < 1:
        ap.error('--jobs must be at least 1')
    if args.exe:
        args.exe = args.exe.resolve()

    tests = discover()
    names = {n for n, _ in tests}
    known = read_known(args.known)
    for k in known:
        if k not in names:
            print(f'{args.known.name}: {k} is not a test', file=sys.stderr)
            return 2
    system = this_system()
    here = {k: v for k, v in known.items() if v[1] is None or system in v[1]}
    known = {k: reason for k, (reason, _, kind) in here.items() if kind is None}
    flaky = {k: (reason, cases) for k, (reason, _, cases) in here.items() if cases is not None}

    plan = [(n, p, *markers(p)) for n, p in tests if not args.filters or any(f in n for f in args.filters)]
    if args.needs:
        plan = [t for t in plan if all(r in t[2] for r in args.needs)]
    if not plan:
        print(f'no test matches {" ".join(args.filters + [f"--needs {r}" for r in args.needs])}', file=sys.stderr)
        return 2

    if args.list:
        for n, _, reqs, template in plan:
            miss = missing(reqs, args)
            note = f'  [needs: {", ".join(reqs)}]' if reqs else ''
            note += f'  [args: {" ".join(template)}]' if template else ''
            note += f'  skipped: {"; ".join(miss)}' if miss else ''
            note += f'  (known failure: {known[n]})' if n in known else ''
            note += f'  (flaky: {flaky[n][0]})' if n in flaky else ''
            print(f'{n}{note}')
        return 0

    LOG_DIR.mkdir(parents=True, exist_ok=True)
    results = {}   # name -> (status, seconds, log path or skip reason)
    to_run = []
    for n, p, reqs, template in plan:
        miss = missing(reqs, args)
        if miss:
            results[n] = ('skip', 0.0, '; '.join(miss))
        else:
            to_run.append((n, command(p, template, args)))

    print(f'{len(to_run)} tests to run on {args.jobs} jobs, {len(results)} skipped; Python {sys.version.split()[0]} '
          f'({sys.executable})', flush=True)
    runner = Runner(args.timeout)
    width = max((len(n) for n, _ in to_run), default=0)
    started = time.monotonic()
    pool = concurrent.futures.ThreadPoolExecutor(args.jobs)
    try:
        futures = {pool.submit(runner.run, n, cmd): n for n, cmd in to_run}
        for f in concurrent.futures.as_completed(futures):
            n = futures[f]
            state, secs, log = f.result()
            status = ('fixed' if state == 'pass' else 'known') if n in known else state
            if n in flaky and (state == 'fail' and flaky_failure(log, flaky[n][1]) or
                               state == 'timeout' and not flaky[n][1]):
                status = 'flaky'
            results[n] = (status, secs, log)
            print(f'{status.upper():8} {n:{width}} {secs:7.1f}s', flush=True)
    except KeyboardInterrupt:
        runner.stop()
        pool.shutdown(wait=True, cancel_futures=True)
        print('stopped', file=sys.stderr)
        return 1
    pool.shutdown()
    wall = time.monotonic() - started

    by = {}
    for n in sorted(results):
        by.setdefault(results[n][0], []).append(n)

    for n in by.get('fail', []) + by.get('timeout', []):
        status, secs, log = results[n]
        what = f'timed out after {secs:.0f}s' if status == 'timeout' else 'failed'
        print(f'\n---- {n}: {what} ({log.relative_to(ROOT).as_posix()})')
        lines = log.read_text(encoding='utf-8', errors='replace').splitlines()
        for line in lines if args.show_output else lines[-TAIL_LINES:]:
            print(f'  {line}')

    print('\nSummary')
    if by.get('skip'):
        print(f'  skipped ({len(by["skip"])}):')
        for n in by['skip']:
            print(f'    {n}: {results[n][2]}')
    if by.get('known'):
        print(f'  known failures ({len(by["known"])}, {args.known.name}):')
        for n in by['known']:
            print(f'    {n}: {known[n]}')
    if by.get('flaky'):
        print(f'  flaky, failed this time ({len(by["flaky"])}, {args.known.name}):')
        for n in by['flaky']:
            print(f'    {n}: {flaky[n][0]} ({results[n][2].relative_to(ROOT).as_posix()})')
            annotate('warning', f'tests/{n}.py failed as flaky: {flaky[n][0]}')
    if by.get('fixed'):
        print(f'  known failures that passed ({len(by["fixed"])}): take their lines out of {args.known.name}')
        for n in by['fixed']:
            print(f'    {n}')
            annotate('warning', f'{n} passed: take its line out of tests/{args.known.name}')
    for key, label in (('fail', 'failed'), ('timeout', 'timed out')):
        if by.get(key):
            print(f'  {label} ({len(by[key])}): {" ".join(by[key])}')
            for n in by[key]:
                annotate('error', f'tests/{n}.py {label}')
    counts = ', '.join(f'{len(by.get(k, []))} {label}' for k, label in
                       (('pass', 'passed'), ('fail', 'failed'), ('timeout', 'timed out'), ('skip', 'skipped'),
                        ('known', 'known failures'), ('fixed', 'known failures passed'), ('flaky', 'flaky')))
    print(f'  {counts}; {wall:.0f}s')
    bad = by.get('fail') or by.get('timeout') or (args.strict and by.get('fixed'))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
