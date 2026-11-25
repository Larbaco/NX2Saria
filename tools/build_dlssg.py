import argparse
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

DESCRIPTION = ('Build nx2navi_dlssg.dll (runtime/dlssg/nx2_sl.cpp, DLSS Frame Generation through NVIDIA '
               'Streamline) with MSVC.')

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / 'runtime' / 'dlssg' / 'nx2_sl.h'
VSWHERE = Path(os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)')) / 'Microsoft Visual Studio' / 'Installer' / 'vswhere.exe'


def vcvars():
    if not VSWHERE.is_file():
        sys.exit('Visual Studio (or its Build Tools) is not installed: vswhere.exe is missing')
    path = subprocess.run([str(VSWHERE), '-latest', '-products', '*', '-requires',
                           'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath'],
                          capture_output=True, text=True).stdout.strip()
    bat = Path(path) / 'VC' / 'Auxiliary' / 'Build' / 'vcvars64.bat'
    if not bat.is_file():
        sys.exit(f'no vcvars64.bat under {path or "(no Visual Studio with the C++ tools)"}')
    return bat


def declared_exports(header=HEADER):
    text = Path(header).read_text(encoding='utf-8')
    return {'nx2sl_' + name for name in re.findall(r'\(\s*\*\s*PFN_nx2sl_(\w+)\s*\)', text)}


def pe_exports(path):
    d = Path(path).read_bytes()
    pe = struct.unpack_from('<I', d, 0x3C)[0]
    if d[pe:pe + 4] != b'PE\0\0':
        raise ValueError(f'{path} is not a PE file')
    nsec, opt = struct.unpack_from('<H', d, pe + 6)[0], struct.unpack_from('<H', d, pe + 20)[0]
    magic = struct.unpack_from('<H', d, pe + 24)[0]
    sec = pe + 24 + opt
    rva = struct.unpack_from('<I', d, pe + 24 + (112 if magic == 0x20b else 96))[0]

    def off(r):
        for i in range(nsec):
            va, vs, raw = (struct.unpack_from('<I', d, sec + 40 * i + k)[0] for k in (12, 8, 20))
            if va <= r < va + max(vs, 1):
                return r - va + raw
        return None

    at = off(rva) if rva else None
    if at is None:
        return set()
    n, names = struct.unpack_from('<I', d, at + 24)[0], off(struct.unpack_from('<I', d, at + 32)[0])
    out = set()
    for i in range(n):
        o = off(struct.unpack_from('<I', d, names + 4 * i)[0])
        out.add(d[o:d.index(b'\0', o)].decode('ascii'))
    return out


def missing_exports(dll, header=HEADER):
    return sorted(declared_exports(header) - pe_exports(dll))


def check(dll):
    if not Path(dll).is_file():
        sys.exit(f'{dll} is missing')
    try:
        missing = missing_exports(dll)
    except (ValueError, TypeError, struct.error) as e:
        sys.exit(f'{dll} is no DLL that can be read ({e})')
    if missing:
        sys.exit(f'{dll} lacks exports runtime/dlssg/nx2_sl.h declares: {", ".join(missing)} '
                 '(a DLL built from older sources: build it again)')


def main():
    ap = argparse.ArgumentParser(description=DESCRIPTION)
    ap.add_argument('--sl', type=Path, help='the Streamline SDK folder (holds include/sl.h)')
    ap.add_argument('--vulkan', type=Path, default=Path(r'C:\msys64\ucrt64\include'), help='folder holding vulkan/vulkan.h')
    ap.add_argument('--out', type=Path, default=ROOT / 'build' / 'dlssg')
    ap.add_argument('--check', type=Path, metavar='DLL', help='only check that DLL exports what nx2_sl.h declares')
    a = ap.parse_args()
    if a.check:
        check(a.check)
        print(f'{a.check}: the {len(declared_exports())} exports nx2_sl.h declares')
        return
    if not a.sl:
        ap.error('--sl is required to build')
    if not (a.sl / 'include' / 'sl.h').is_file():
        sys.exit(f'{a.sl / "include" / "sl.h"} is missing')
    a.out.mkdir(parents=True, exist_ok=True)
    inc = a.out / 'include'
    for d in ('vulkan', 'vk_video'):
        if (a.vulkan / d).is_dir():
            shutil.copytree(a.vulkan / d, inc / d, dirs_exist_ok=True)
    src = ROOT / 'runtime' / 'dlssg' / 'nx2_sl.cpp'
    cmd = (f'call "{vcvars()}" >nul && cl /nologo /LD /O2 /EHsc /std:c++17 /W3 /DNOMINMAX /DWIN32_LEAN_AND_MEAN '
           f'/I"{a.sl / "include"}" /I"{inc}" /I"{src.parent}" "{src}" /Fo"{a.out}\\\\" '
           f'/Fe"{a.out / "nx2navi_dlssg.dll"}" /link /NOLOGO')
    with tempfile.NamedTemporaryFile('w', suffix='.cmd', delete=False) as f:
        f.write('@echo off\r\nset "PATH=%PATH%;' + str(VSWHERE.parent) + '"\r\n' + cmd + '\r\n')
        script = f.name
    try:
        r = subprocess.run(['cmd', '/c', script])
    finally:
        os.unlink(script)
    if r.returncode or not (a.out / 'nx2navi_dlssg.dll').is_file():
        sys.exit(f'the build failed ({r.returncode})')
    check(a.out / 'nx2navi_dlssg.dll')
    print(a.out / 'nx2navi_dlssg.dll')


if __name__ == '__main__':
    main()
