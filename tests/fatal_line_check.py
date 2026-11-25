# needs: exe
# args: {exe}
"""Run the game executable's busy crash probes and check that the FATAL line of a crash or a fatal error comes out
whole, on a line of its own, while other threads log (runtime/main.c crash_handler, runtime/dispatch.c fatal_end).

  - --probe-crash-busy: four threads log without a pause and the main thread faults inside strlen.
  - --probe-fatal-busy: the same threads, and the main thread ends through rt_fatal.
  - --probe-crash-stuck: a thread holds the lock of the log's stderr writes for good and the main thread faults; the
    report still comes out, after one line that says the lock was given up.

Each busy probe runs RUNS times in a folder of its own, so no summary or header lands in a game folder.

No game files, no GPU, no window.

Usage: fatal_line_check.py PATH_TO_GAME_EXE
"""
import re
import subprocess
import sys
import tempfile

RUNS = 20
CRASH_EXIT = 71
FATAL_EXIT = 70
TIMEOUT = 60

HOST = r'(?:0x[0-9a-fA-F]+|[0-9a-fA-F]{8,16}|\(nil\))'
CRASH_LINE = re.compile(r'^FATAL: access violation \(0x[0-9a-f]+\) at host ' + HOST +
                        r' \((?:outside every module|[^()\[\]]+\+0x[0-9a-f]+)\)'
                        r'(?:, (?:reading|writing|executing) address 0x[0-9a-f]+)?$')
FATAL_LINE = 'FATAL: probe-fatal-busy: a fatal error while 4 threads log'
STUCK_NOTE = '[host] log: the lock of the stderr writes was still held after'


def run(exe, mode, cwd):
    try:
        p = subprocess.run([exe, mode], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, cwd=cwd, timeout=TIMEOUT)
    except subprocess.TimeoutExpired:
        return None, []
    return p.returncode, p.stdout.decode('utf-8', 'replace').splitlines()


def fatal_lines(lines):
    return [ln for ln in lines if 'FATAL' in ln]


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    exe = sys.argv[1]
    fails = []
    red = {}
    cases = [('--probe-crash-busy', CRASH_EXIT, lambda ln: bool(CRASH_LINE.match(ln))),
             ('--probe-fatal-busy', FATAL_EXIT, lambda ln: ln == FATAL_LINE)]
    for mode, want, whole in cases:
        red[mode] = 0
        for i in range(RUNS):
            with tempfile.TemporaryDirectory() as cwd:
                code, lines = run(exe, mode, cwd)
            fatal = fatal_lines(lines)
            bad = []
            if code is None:
                bad.append('no end within %d s' % TIMEOUT)
            elif code != want:
                bad.append('exit code %d, expected %d' % (code, want))
            if len(fatal) != 1 or not whole(fatal[0]):
                bad.append('FATAL lines %r' % fatal[:3])
            if bad:
                red[mode] += 1
                if red[mode] == 1:
                    fails.append('%s run %d: %s' % (mode, i + 1, '; '.join(bad)))
        if red[mode]:
            fails.append('%s: %d of %d runs had a FATAL line that was not whole or alone' % (mode, red[mode], RUNS))

    with tempfile.TemporaryDirectory() as cwd:
        code, lines = run(exe, '--probe-crash-stuck', cwd)
    fatal = fatal_lines(lines)
    if code is None:
        fails.append('--probe-crash-stuck: no end within %d s: the report waited for the held lock' % TIMEOUT)
    else:
        if code != CRASH_EXIT:
            fails.append('--probe-crash-stuck: exit code %d, expected %d' % (code, CRASH_EXIT))
        if len(fatal) != 1 or not CRASH_LINE.match(fatal[0]):
            fails.append('--probe-crash-stuck: FATAL lines %r' % fatal[:3])
        if sum(ln.startswith(STUCK_NOTE) for ln in lines) != 1:
            fails.append('--probe-crash-stuck: no one line that says the lock was given up')

    for f in fails:
        print('fatal_line_check: FAIL:', f)
    if not fails:
        print('fatal_line_check: passed (%d runs of each busy probe, every FATAL line whole and alone; a held stderr '
              'lock given up once)' % RUNS)
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
