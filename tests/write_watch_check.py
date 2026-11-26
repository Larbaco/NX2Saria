# needs: clang
import argparse
import errno
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from launcher.platform import linux  # noqa: E402

DESCRIPTION = """Whether written memory is tracked on Linux, and what the game log and the launcher say when it is not
(runtime/platform/write_watch_note.h, launcher/platform/linux.py write_watch_verdict): builds the C decision with the
header alone and checks it, and the launcher's, against made-up kernel versions and probe results:

  - a probe that worked means tracked, whatever the version says (a backport, a version string we cannot read);
  - a failed probe on a kernel older than 6.7 means a kernel too old; on 6.7 or later, or a version we cannot read,
    asynchronous write protection refused (a kernel built without userfaultfd, a sandbox that blocks the call);
  - the game's line names the kernel, the minimum and the about-1-fps cost, and the launcher agrees on every case.

No game files. Usage: python tests/write_watch_check.py [--cc <clang>]
"""

MSYS_BIN = Path(r'C:\msys64\ucrt64\bin')

CASES = [
    ('7.0.0-43-generic', 0, 'on'),
    ('6.7.0', 0, 'on'),
    ('5.4.0-150-generic', 0, 'on'),
    ('banana', 0, 'on'),
    ('6.6.87.2-microsoft-standard-WSL2', errno.EINVAL, 'old'),
    ('6.1.52-valve16-1-neptune-61', errno.EINVAL, 'old'),
    ('5.15.0-91-generic', errno.EPERM, 'old'),
    ('4.19.0', errno.ENOSYS, 'old'),
    ('6.6', errno.EINVAL, 'old'),
    ('6.7.0-arch1-1', errno.ENOSYS, 'refused'),
    ('6.10.3', errno.EINVAL, 'refused'),
    ('10.2.1', errno.EINVAL, 'refused'),
    ('7', errno.EPERM, 'refused'),
    ('', errno.EINVAL, 'refused'),
    ('banana', errno.ENOSYS, 'refused'),
]

HARNESS = r'''
#include <stdio.h>
#include <string.h>
#include "%s"
static const char *names[] = {"on", "old", "refused"};
int main(int argc, char **argv) {
    for (int i = 1; i + 1 < argc; i += 2) {
        int err = atoi(argv[i + 1]);
        int v = ww_verdict(argv[i], err);
        char line[600];
        ww_note(v, argv[i], err ? strerror(err) : "", line, sizeof line);
        printf("%%s\t%%s\n", names[v], line);
    }
    return 0;
}
'''


def main():
    ap = argparse.ArgumentParser(description=DESCRIPTION)
    ap.add_argument('--cc', help='the C compiler (default: MSYS2 UCRT64 clang on Windows, clang elsewhere)')
    args = ap.parse_args()
    win = os.name == 'nt'
    cc = args.cc or (str(MSYS_BIN / 'clang.exe') if win and MSYS_BIN.exists() else 'clang')
    env = dict(os.environ)
    if win and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    header = (ROOT / 'runtime' / 'platform' / 'write_watch_note.h').as_posix()
    fails = 0
    with tempfile.TemporaryDirectory(prefix='nx2saria-write-watch-') as tmp:
        c = Path(tmp) / 'check.c'
        c.write_text('#include <stdlib.h>\n' + HARNESS % header, encoding='utf-8')
        exe = Path(tmp) / ('check.exe' if win else 'check')
        subprocess.check_call([cc, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', str(c), '-o', str(exe)], env=env)
        argv = [str(exe)]
        for release, err, _ in CASES:
            argv += [release, str(err)]
        out = subprocess.run(argv, check=True, capture_output=True, text=True).stdout.splitlines()
    assert len(out) == len(CASES), out
    for (release, err, want), row in zip(CASES, out):
        got, line = row.split('\t', 1)
        py = linux.write_watch_verdict(release, err)
        if got != want or py != want:
            print(f'FAIL: kernel {release!r}, probe errno {err}: the game says {got}, the launcher {py}, wanted {want}')
            fails += 1
        if not line.startswith('mem: write watch ') or (release and release not in line):
            print(f'FAIL: the line for {release!r} does not name the kernel: {line}')
            fails += 1
        if want != 'on' and ('about 1 fps' not in line or '6.7' not in line):
            print(f'FAIL: the line for {release!r} does not say the cost and the minimum: {line}')
            fails += 1
        if want == 'refused' and os.strerror(err) not in line:
            print(f'FAIL: the line for {release!r} does not give the error: {line}')
            fails += 1
        if want == 'on' and 'fps' in line:
            print(f'FAIL: a tracked kernel warns: {line}')
            fails += 1
        text = linux.write_watch_text({'verdict': py, 'release': release, 'error': err})
        if (want == 'on') != (text is None) or (text and (release not in text[1] or '6.7' not in text[1])):
            print(f'FAIL: the launcher text for {release!r}: {text}')
            fails += 1
    for release, err, want in [('5.10.0', None, 'old'), ('6.8.0', None, 'on'), ('banana', None, 'on')]:
        py = linux.write_watch_verdict(release, err)
        if py != want:
            print(f'FAIL: kernel {release!r} not probed: the launcher says {py}, wanted {want}')
            fails += 1
    for value, release, err in [('1', '7.0.0', errno.ENOSYS), ('6.5.0-test', '6.5.0-test', errno.EINVAL)]:
        st = linux.write_watch_status(release='7.0.0', probe=lambda: 0, env={'NX2SARIA_TEST_NO_UFFD': value})
        if (st['release'], st['error']) != (release, err):
            print(f'FAIL: NX2SARIA_TEST_NO_UFFD={value} gives {st}')
            fails += 1
    st = linux.write_watch_status(release='7.0.0', probe=lambda: 0, env={})
    if st['verdict'] != 'on' or not linux.write_watch_log_line(st).startswith('write watch: on'):
        print(f'FAIL: a probe that worked: {st}, {linux.write_watch_log_line(st)}')
        fails += 1
    print(f'write watch: {fails} failures' if fails else f'write watch: ok ({len(CASES)} kernels)')
    return fails != 0


if __name__ == '__main__':
    sys.exit(main())
