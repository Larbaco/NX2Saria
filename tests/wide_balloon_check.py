# needs: clang
"""Check the 1.1.1 speech balloon patch and its horizontal anchor on synthetic inputs."""
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'recomp'))
from lifter import Lifter  # noqa: E402
from patches import load  # noqa: E402

MSYS_BIN = Path(r'C:\msys64\ucrt64\bin')
MAIN, PAGE, SITE, HOST = 0x7000000000, 0x4ea0000, 0x20c2c8, 0xe50


def check_site():
    table = load(ROOT / 'recomp/patches-v111.txt', [('main', MAIN, PAGE)])
    (index, words, original), = table.sites['main'][SITE]
    assert table.sets[index] == ('wide_balloon', 'NX2SARIA_BALLOON_ANCHOR_FIX', 1)
    assert original == words[0] == 0x1e210841
    assert len(words) == 2
    call = Lifter(MAIN, simd='c').lift(words[1], MAIN + SITE)
    assert call.kind == 'bl' and call.target == table.page + HOST
    print('wide_balloon_check: the site keeps the Y multiply and calls the anchor callback', flush=True)


def main():
    check_site()
    work = ROOT / 'build' / 'test' / 'wide_balloon'
    work.mkdir(parents=True, exist_ok=True)
    source = (ROOT / 'runtime' / 'patches' / 'widescreen.c').read_text()
    start = source.index('static float balloon_ui_scale')
    end = source.index('void wide_init(', start)
    (work / 'wide_balloon.inc').write_text(source[start:end])
    env = dict(os.environ)
    if os.name == 'nt' and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    cc = env.get('CC') or shutil.which('clang', path=env.get('PATH')) or 'clang'
    exe = work / ('wide_balloon_check.exe' if os.name == 'nt' else 'wide_balloon_check')
    subprocess.check_call([cc, '-std=gnu11', '-Wall', '-Werror', '-Wno-bitwise-instead-of-logical',
                           '-I', str(ROOT / 'runtime'), '-I', str(work),
                           str(ROOT / 'tests' / 'wide_balloon_check.c'), '-o', str(exe), '-lm'], env=env)
    return subprocess.run([str(exe)], env=env).returncode


if __name__ == '__main__':
    sys.exit(main())
