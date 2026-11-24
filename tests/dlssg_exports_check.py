import contextlib
import inspect
import io
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MSYS_BIN = Path(r'C:\msys64\ucrt64\bin')
sys.path.insert(0, str(ROOT / 'tools'))
import build_dlssg  # noqa: E402

HEADER = '''typedef int (*PFN_nx2sl_version)(void);
typedef void (*PFN_nx2sl_reflex)(int mode);
typedef int ( * PFN_nx2sl_reflex2 )(int mode, unsigned frame_limit_us);
'''
SOURCE = '''__declspec(dllexport) int nx2sl_version(void) { return 3; }
__declspec(dllexport) void nx2sl_reflex(int mode) { (void)mode; }
int nx2sl_not_exported(void) { return 0; }
'''


def compiler():
    if os.environ.get('CC'):
        return os.environ['CC']
    if os.name == 'nt' and (MSYS_BIN / 'clang.exe').exists():
        return str(MSYS_BIN / 'clang.exe')
    return shutil.which('clang') or 'clang'


def refused(fn, *args):
    try:
        with contextlib.redirect_stdout(io.StringIO()):
            fn(*args)
    except SystemExit as e:
        return str(e.code)
    return None


def main():
    if os.name != 'nt':
        print('dlssg_exports_check: Windows only (it builds a DLL)')
        return 0
    work = ROOT / 'build' / 'test' / 'dlssg_exports'
    work.mkdir(parents=True, exist_ok=True)
    (work / 'two.h').write_text(HEADER, encoding='utf-8')
    (work / 'two.c').write_text(SOURCE, encoding='utf-8')
    env = dict(os.environ)
    if MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    dll = work / 'two.dll'
    subprocess.check_call([compiler(), '-shared', '-O1', str(work / 'two.c'), '-o', str(dll)], env=env)
    fails = 0

    def expect(what, got, want):
        nonlocal fails
        if got != want:
            print(f'FAIL {what}: {got!r}, not {want!r}')
            fails += 1

    expect('declared in the test header', build_dlssg.declared_exports(work / 'two.h'),
           {'nx2sl_version', 'nx2sl_reflex', 'nx2sl_reflex2'})
    expect('exported by the test DLL', build_dlssg.pe_exports(dll), {'nx2sl_version', 'nx2sl_reflex'})
    expect('missing from the test DLL', build_dlssg.missing_exports(dll, work / 'two.h'), ['nx2sl_reflex2'])
    declared = build_dlssg.declared_exports()
    for name in ('nx2sl_version', 'nx2sl_init', 'nx2sl_reflex', 'nx2sl_reflex2', 'nx2sl_frames_max', 'nx2sl_shutdown'):
        expect(f'{name} declared in nx2_sl.h', name in declared, True)
    expect('types that are no functions', {'nx2sl_' + t for t in ('Image', 'Camera', 'Log')} & declared, set())

    (work / 'all.c').write_text(''.join(f'__declspec(dllexport) void {n}(void) {{}}\n' for n in sorted(declared)),
                                encoding='utf-8')
    full = work / 'all.dll'
    subprocess.check_call([compiler(), '-shared', '-O1', str(work / 'all.c'), '-o', str(full)], env=env)
    expect('missing from a DLL with every declared export', build_dlssg.missing_exports(full), [])
    expect('check of a DLL with every declared export', refused(build_dlssg.check, full), None)
    why = refused(build_dlssg.check, dll) or ''
    expect('check of the two-export DLL names nx2sl_reflex2', 'nx2sl_reflex2' in why and 'build it again' in why, True)
    expect('check of a missing DLL', 'is missing' in (refused(build_dlssg.check, work / 'none.dll') or ''), True)
    (work / 'tiny.dll').write_bytes(b'MZ')
    for name in ('two.c', 'tiny.dll'):
        expect(f'check of {name}, no DLL', 'no DLL' in (refused(build_dlssg.check, work / name) or ''), True)

    import make_kit
    import make_launcher
    expect('make_launcher.check_dlssg passes the full DLL', refused(make_launcher.check_dlssg, full), None)
    expect('make_launcher.check_dlssg names nx2sl_reflex2',
           'nx2sl_reflex2' in (refused(make_launcher.check_dlssg, dll) or ''), True)
    for mod, call, later in ((make_launcher, 'check_dlssg(a.dlssg)', ('streamline_pin.resolve(', 'shutil.rmtree(')),
                             (make_kit, 'build_dlssg.check(a.dlssg)', ('streamline_pin.resolve(', 'shutil.rmtree('))):
        src = inspect.getsource(mod.main)
        at = src.find(call)
        for what in later:
            expect(f'{mod.__name__}.main checks the DLL before {what}', 0 <= at < src.find(what), True)
    print(f'dlssg_exports_check: {"ok" if not fails else f"{fails} failed"} ({len(declared)} exports in nx2_sl.h)')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
