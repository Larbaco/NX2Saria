# needs: clang
"""Build and run tests/log_privacy_check.c and tests/log_privacy_env_check.c: the personal folders the game writes short
in its log, the bug capture and the crash report (runtime/log_privacy.c, runtime/log_privacy_core.h).

- log_privacy_check.c, any system, Windows and Linux tables: %LOCALAPPDATA% before %USERPROFILE% (the longest folder
  wins), Windows paths without case and with either separator, the 8.3 form, the \\\\?\\ prefix, a profile on a
  network share, an account whose folder starts the same (C:\\Users\\Ana Souza2) left alone, a home with an accent and
  a space, the resolved home (/var/home), the account name alone inside a path but not inside another word (ana in
  banana, anabel, ana_x, anaé) nor in text without a path, the computer's name as a word anywhere (a UNC path, a
  network line) without case, common account and computer names (user, deck, localhost) not looked for alone, roots
  and short names refused, and the same text when the rewrite runs in small pieces.
- log_privacy_env_check.c, Linux only: runtime/log_privacy.c with the environment of this run: HOME through a symbolic
  link to a folder with an accent and a space (both forms written ~), USER, the start line with the switch on and off
  (log_short_paths = 0 writes everything in full), the computer's name from HOSTNAME with and without its domain,
  log_privacy_str and log_privacy_dup, and the count the exit line gives.

No game files. Usage: log_privacy_check.py
"""
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MSYS_BIN = Path(r'C:\msys64\ucrt64\bin')


def compiler():
    if os.environ.get('CC'):
        return os.environ['CC']
    if os.name == 'nt' and (MSYS_BIN / 'clang.exe').exists():
        return str(MSYS_BIN / 'clang.exe')
    return shutil.which('clang') or 'clang'


def build(out, *sources):
    exe = out / (sources[0].stem + ('.exe' if os.name == 'nt' else ''))
    cmd = [compiler(), '-std=gnu11', '-O1', '-g', '-Wall', '-Wno-unused-function', '-I', str(ROOT / 'runtime'),
           *map(str, sources), '-o', str(exe)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        print(r.stdout + r.stderr)
        sys.exit(f'{sources[0].name}: compile failed')
    return exe


def main():
    out = ROOT / 'build' / 'test' / 'log_privacy'
    out.mkdir(parents=True, exist_ok=True)
    bad = 0

    exe = build(out, ROOT / 'tests' / 'log_privacy_check.c')
    r = subprocess.run([str(exe)], capture_output=True, text=True, encoding='utf-8')
    print(r.stdout, end='')
    bad += r.returncode != 0

    def check(ok, what, text=''):
        nonlocal bad
        print(f'{what}: {"ok" if ok else "FAIL"}')
        if not ok:
            bad += 1
            print(text)

    if os.name == 'nt':
        print('env check: skipped (Linux environment)')
    else:
        exe = build(out, ROOT / 'tests' / 'log_privacy_env_check.c', ROOT / 'runtime' / 'log_privacy.c')
        with tempfile.TemporaryDirectory() as tmp:
            real = Path(tmp) / 'real' / 'zé lima'
            real.mkdir(parents=True)
            link = Path(tmp) / 'link'
            link.symlink_to(real)
            env = {k: v for k, v in os.environ.items() if not k.startswith(('NX2SARIA_', 'NX2NAVI_'))}
            env.update(HOME=str(link), USER='zé lima', HOSTNAME='casa-do-ze.lan')
            args = [f'{link}/Games/oot/nx2saria.log', f'{real}/y', '/srv/zé lima/z', '/srv/zé limas/z', 'plain',
                    'smb: //CASA-DO-ZE/jogos and casa-do-ze.lan']
            r = subprocess.run([str(exe), *args], capture_output=True, text=True, encoding='utf-8', env=env)
            o = r.stdout
            check('log: log: personal folders are written short (log_short_paths = 1): ~ for the home, 2 folders, the '
                  "account name as $USER and the computer's name as %COMPUTERNAME%; the rest of each path is kept" in o,
                  'env: start line, on', o)
            check('str: smb: //%COMPUTERNAME%/jogos and %COMPUTERNAME%\n' in o, 'env: the computer name (HOSTNAME)', o)
            check('str: ~/Games/oot/nx2saria.log\ndup: ~/Games/oot/nx2saria.log\n' in o, 'env: home through a link', o)
            check('str: ~/y\n' in o, 'env: the resolved home', o)
            check('str: /srv/$USER/z\n' in o and 'str: /srv/zé limas/z\n' in o, 'env: the account name alone', o)
            check('str: plain\ndup: (none)\n' in o, 'env: nothing to do', o)
            check('count: 8\n' in o, 'env: count', o)
            check(str(real) not in o and str(link) not in o, 'env: no full home left', o)
            env['NX2SARIA_LOG_SHORT_PATHS'] = '0'
            r = subprocess.run([str(exe), f'{link}/x'], capture_output=True, text=True, encoding='utf-8', env=env)
            o = r.stdout
            check('log: log: paths are written in full, the account and computer names included (log_short_paths = 0)'
                  in o and
                  f'str: {link}/x\ndup: (none)\ncount: 0' in o, 'env: switch off', o)

    print('log_privacy_check:', 'FAILED' if bad else 'ok')
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
