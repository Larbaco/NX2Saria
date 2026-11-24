# needs: clang, file:runtime/wb_merge_spv.h
import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import build_game   # noqa: E402  (RT_FLAGS, the compiler and the include folders of the build)
import runtime_include  # noqa: E402

FILES = ['vk/vk_memory.c', 'vk/vk_texture.c', 'gfx/tex_probe.c']
STRICT = ['-Werror=shorten-64-to-32', '-Werror=format', '-Werror=incompatible-pointer-types']
WATCHED = re.compile(r'^(?P<where>[^\n]*?:\d+:\d+): (?:error|warning): (?P<what>[^\n]*?)'
                     r'\[(?:-Werror,)?(?P<flag>-Wshorten-64-to-32|-Wformat[\w-]*|-Wincompatible[\w-]*pointer-types)\]',
                     re.M)


def in_file(where, src):
    path = where.rsplit(':', 2)[0]
    try:
        return Path(path).resolve() == src.resolve()
    except OSError:
        return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--cc', help='the C compiler (default: the build\'s clang)')
    args = ap.parse_args()
    if not (ROOT / 'runtime' / 'wb_merge_spv.h').is_file():
        print('gen_width_check: FAIL: runtime/wb_merge_spv.h is missing: run tools/build_shaders.py first')
        return 1
    cc = args.cc or os.environ.get('CC') or str(build_game.CLANG)
    build_game.set_cpu('v3')
    flags = [f for f in build_game.RT_FLAGS if f != '-g']
    inc = [*runtime_include.flags(), '-I', str(build_game.vma_include())]
    vk = build_game.vulkan_include(cc)
    if vk:
        inc = ['-I', str(vk)] + inc
    env = dict(os.environ)
    if os.name == 'nt':
        env['PATH'] = str(Path(cc).parent) + os.pathsep + env.get('PATH', '')
    bad = 0
    for name in FILES:
        src = ROOT / 'runtime' / name
        p = subprocess.run([cc, '-fsyntax-only', '-std=gnu11'] + flags + STRICT + inc + [str(src)], capture_output=True,
                           text=True, errors='replace', env=env)
        watched = list(WATCHED.finditer(p.stderr))
        hits = [m for m in watched if in_file(m.group('where'), src)]
        for m in hits:
            print(f'gen_width_check: FAIL: {m.group("where")}: {m.group("what").strip()} [{m.group("flag")}]')
        if p.returncode and not watched:
            print(f'gen_width_check: FAIL: runtime/{name} does not compile:\n{p.stderr[-3000:]}')
            bad += 1
        bad += len(hits)
        if not hits and (not p.returncode or watched):
            print(f'gen_width_check: runtime/{name}: no narrowing, format or pointer-type diagnostic'
                  + (f' ({len(watched)} in the headers it includes)' if watched else ''))
    if bad:
        print(f'gen_width_check: {bad} problems')
        return 1
    print('gen_width_check: all checks passed')
    return 0


if __name__ == '__main__':
    sys.exit(main())
