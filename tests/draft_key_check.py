"""The settings menu's draft names each setting NX2SARIA_ and its key (runtime/ui/option_rows.cpp norm_key), and every
comparison of a name with the NX2SARIA_ or NX2NAVI_ prefix uses the prefix's own length.

1. norm_key, cut out of option_rows.cpp with KEY_PREFIX and built with a small harness, gives NX2SARIA_HDR for HDR,
   hdr, NX2SARIA_HDR, nx2saria_hdr and the earlier NX2NAVI_HDR, and the log's spelling (the name after KEY_PREFIX
   characters) is HDR. When the rebrand made the prefix NX2SARIA_ (9 characters) but kept a length of 8 here, the draft
   wrote NX2SARIA_NX2SARIA_HDR, a name no setting has: no change in the menu took effect.
2. No strncmp, strncasecmp, memcmp or std::string compare in runtime/, launcher/native/ or tools/ compares a literal
   starting with NX2 (any case) over a length other than the literal's, and option_rows.cpp cuts the prefix from a key
   only through KEY_PREFIX.

No GPU, no game files, any Python.

Usage: draft_key_check.py
"""
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MSYS_BIN = Path(r'C:\msys64\ucrt64\bin')

HARNESS = r'''
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>

namespace {
%s
}

static int fails;
static void expect(const char *in, const char *want) {
    std::string got = norm_key(in);
    if (got != want) std::printf("FAIL norm_key(\"%%s\"): \"%%s\", expected \"%%s\"\n", in, got.c_str(), want), fails++;
}

int main() {
    for (const char *in : {"HDR", "hdr", "NX2SARIA_HDR", "nx2saria_hdr", "NX2NAVI_HDR", "Nx2Navi_Hdr"}) expect(in, "NX2SARIA_HDR");
    expect("FPS_LIMIT", "NX2SARIA_FPS_LIMIT");
    expect("NX2NAVI_BIND_JUMP", "NX2SARIA_BIND_JUMP");
    std::string k = norm_key("window_size");
    if (std::strcmp(k.c_str() + KEY_PREFIX, "WINDOW_SIZE"))
        std::printf("FAIL the log's name: \"%%s\", expected \"WINDOW_SIZE\"\n", k.c_str() + KEY_PREFIX), fails++;
    if (fails) return 1;
    std::printf("draft_key_check: norm_key passed\n");
    return 0;
}
'''

# strncmp(a, "lit", n), strncasecmp(a, "lit", n), memcmp(a, "lit", n), and s.compare(0, n, "lit")
CALL = re.compile(r'\b(?:strncmp|strncasecmp|_strnicmp|memcmp)\s*\([^;"]*?"((?i:nx2)[^"\\]*)"\s*,\s*(\d+)\s*\)')
COMPARE = re.compile(r'\.compare\s*\(\s*0\s*,\s*(\d+)\s*,\s*"((?i:nx2)[^"\\]*)"\s*\)')


def compiler():
    """$CXX, else MSYS2 UCRT64's clang++, else clang++ on PATH."""
    if os.environ.get('CXX'):
        return os.environ['CXX']
    if os.name == 'nt' and (MSYS_BIN / 'clang++.exe').exists():
        return str(MSYS_BIN / 'clang++.exe')
    return shutil.which('clang++') or 'clang++'


def cut(src):
    """option_rows.cpp's KEY_PREFIX line and norm_key."""
    prefix = re.search(r'^constexpr size_t KEY_PREFIX = .*\n', src, re.M)
    fn = re.search(r'^std::string norm_key\(const char \*key\) \{\n.*?\n\}\n', src, re.M | re.S)
    assert prefix and fn, 'runtime/ui/option_rows.cpp: KEY_PREFIX or norm_key was not found'
    return prefix.group(0) + fn.group(0)


def lengths():
    """Every prefix comparison with a literal starting NX2 whose length is not the literal's."""
    bad = []
    files = [p for d in ('runtime', 'launcher/native', 'tools') for p in (ROOT / d).rglob('*')
             if p.suffix in ('.c', '.cpp', '.h') and 'third_party' not in p.parts and 'vendor' not in p.parts]
    for p in files:
        for no, line in enumerate(p.read_text(encoding='utf-8', errors='replace').splitlines(), 1):
            code = line.split('//')[0]
            found = [(lit, int(n)) for lit, n in CALL.findall(code)] + [(lit, int(n)) for n, lit in COMPARE.findall(code)]
            bad += [f'{p.relative_to(ROOT).as_posix()}:{no}: "{lit}" compared over {n} characters, it has {len(lit)}'
                    for lit, n in found if n != len(lit)]
    return bad


def main():
    src = (ROOT / 'runtime' / 'ui' / 'option_rows.cpp').read_text(encoding='utf-8')
    bad = lengths()
    bad += [f'runtime/ui/option_rows.cpp:{src[:m.start()].count(chr(10)) + 1}: a key cut after {m.group(1)} characters, '
            'not KEY_PREFIX' for m in re.finditer(r'c_str\(\)\s*\+\s*(\d+)', src)]
    for b in bad:
        print('FAIL ' + b)
    work = ROOT / 'build' / 'test' / 'draft_key'
    work.mkdir(parents=True, exist_ok=True)
    cpp = work / 'draft_key_check.cpp'
    cpp.write_text('// Made by tests/draft_key_check.py from runtime/ui/option_rows.cpp.\n' + HARNESS % cut(src),
                   encoding='utf-8')
    exe = work / ('draft_key_check.exe' if os.name == 'nt' else 'draft_key_check')
    env = dict(os.environ)
    if os.name == 'nt' and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    subprocess.check_call([compiler(), '-std=c++17', '-O1', '-Wall', '-Werror', str(cpp), '-o', str(exe)], env=env)
    rc = subprocess.run([str(exe)], env=env).returncode
    if bad or rc:
        return 1
    print('draft_key_check: prefix lengths passed')
    return 0


if __name__ == '__main__':
    sys.exit(main())
