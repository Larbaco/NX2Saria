import argparse
import datetime
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

DESCRIPTION = 'Merge the raw profiles of an instrumented build into the execution profile a build compiles with.'
ROOT = Path(__file__).resolve().parent.parent
EXE = '.exe' if sys.platform == 'win32' else ''
DEFAULT_DIR = ROOT / 'build' / 'pgo'
MSYS_BIN = Path(r'C:\msys64\ucrt64\bin')


def llvm_profdata(toolchain):
    if toolchain:
        tool = toolchain / 'bin' / f'llvm-profdata{EXE}'
    else:
        found = sorted((ROOT / 'toolchain').glob(f'llvm-mingw-*/llvm-mingw-*/bin/llvm-profdata{EXE}'))
        on_path = shutil.which('llvm-profdata')
        tool = (found[-1] if found else MSYS_BIN / 'llvm-profdata.exe' if (MSYS_BIN / 'llvm-profdata.exe').is_file()
                else Path(on_path) if on_path else Path(f'llvm-profdata{EXE}'))
    if not tool.is_file():
        sys.exit(f'{tool} not found: pass --toolchain with the folder of the compiler that built the instrumented '
                 'game (docs/building.md, "Profile-guided build", says which package holds llvm-profdata)')
    return tool


def recomp_tree():
    dirty = subprocess.run(['git', '-C', str(ROOT), 'status', '--porcelain', '--', 'recomp'], capture_output=True,
                           text=True).stdout.strip()
    if dirty:
        return None
    return subprocess.run(['git', '-C', str(ROOT), 'rev-parse', 'HEAD:recomp'], capture_output=True,
                          text=True).stdout.strip() or None


def recomp_digest(version, root=ROOT):
    sys.path.insert(0, str(ROOT / 'tools'))
    import make_game
    return make_game.tree_digest(Path(root) / 'recomp', ('*.py', f'patches-{version}.txt'))


def training(build, version):
    try:
        made = json.loads((build / 'pgo' / 'training.json').read_text(encoding='utf-8'))
    except OSError:
        return {}
    except ValueError:
        sys.exit(f'{build / "pgo" / "training.json"} cannot be read')
    if made.get('version') != version:
        sys.exit(f'{build} was built for game version {made.get("version")}, not {version}')
    if made.get('recomp_digest') != recomp_digest(version):
        sys.exit(f'{build} was built from another recomp/ than this checkout\'s: merge from the commit the '
                 'instrumented game was built at')
    return made


def llvm_release(text):
    m = re.search(r'version (\d+\.\d+\.\d+)', text or '')
    return m.group(1) if m else ''


def merge(build, out, version, toolchain):
    raw = sorted((build / 'pgo').glob('*.profraw'))
    if not raw:
        sys.exit(f'no .profraw in {build / "pgo"}: run the instrumented game first')
    tree = recomp_tree()
    if not tree:
        sys.exit('recomp/ has uncommitted changes: the record could not name the generator the profile matches')
    made = training(build, version)
    tool = llvm_profdata(toolchain)
    out.parent.mkdir(parents=True, exist_ok=True)
    tmp = out.with_suffix('.profdata.tmp')
    cmd = [str(tool), 'merge', '--failure-mode=all', '-o', str(tmp), *map(str, raw)]
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode:
        tmp.unlink(missing_ok=True)
        sys.exit(f'llvm-profdata merge failed: {(p.stdout + p.stderr).strip()[-2000:]}')
    tmp.replace(out)
    said = subprocess.run([str(tool), '--version'], capture_output=True, text=True).stdout
    record = {'version': version, 'recomp_tree': tree, 'recomp_digest': recomp_digest(version),
              'merged': datetime.date.today().isoformat(), 'raw_files': len(raw),
              'tool': next((line.strip() for line in said.splitlines() if 'version' in line), ''),
              'compiler': made.get('compiler', '')}
    out.with_suffix('.json').write_text(json.dumps(record, indent=1) + '\n', encoding='utf-8')
    if made.get('compiler') and llvm_release(made['compiler']) != llvm_release(record['tool']):
        print(f'pgo: warning: the game was built with {made["compiler"]} and its counts were merged with '
              f'{record["tool"]}: merge with the llvm-profdata of the same LLVM release (--toolchain)', file=sys.stderr)
    size = sum(f.stat().st_size for f in raw)
    print(f'{out}: {len(raw)} raw files ({size / 2**20:.1f} MiB) -> {out.stat().st_size / 2**20:.1f} MiB; '
          f'recomp tree {tree[:12]}')


def fitting(profile, version='v111'):
    profile = Path(profile)
    rec = profile.with_suffix('.json')
    if not profile.is_file():
        sys.exit(f'{profile} not found')
    try:
        record = json.loads(rec.read_text(encoding='utf-8'))
    except (OSError, ValueError):
        sys.exit(f'{rec} is missing or cannot be read: tools/pgo.py merge writes it beside the profile')
    if record.get('version') != version:
        sys.exit(f'{profile} was trained on game version {record.get("version")}, not {version}')
    if record.get('recomp_digest') != recomp_digest(version):
        sys.exit(f'{profile} was trained on another recomp/ than the one in this checkout (its record names the tree '
                 f'{str(record.get("recomp_tree", "unknown"))[:12]}): the builds would leave it out. Train a profile '
                 'again (docs/building.md, "Profile-guided build") or release without one')
    return record


def place(profile, data, version='v111'):
    fitting(profile, version)
    data = Path(data)
    data.mkdir(parents=True, exist_ok=True)
    shutil.copy2(profile, data / f'pgo-{version}.profdata')
    shutil.copy2(Path(profile).with_suffix('.json'), data / f'pgo-{version}.json')
    return data / f'pgo-{version}.profdata'


def main():
    ap = argparse.ArgumentParser(description=DESCRIPTION)
    sub = ap.add_subparsers(dest='cmd', required=True)
    m = sub.add_parser('merge', help='merge <build>/pgo/*.profraw into one .profdata and its training record')
    m.add_argument('build', type=Path, help='the instrumented build: the game folder of make_game.py --pgo gen, or '
                                            'the build directory of build_game.py --pgo gen')
    m.add_argument('--version', required=True, help='game version the build was made for (v111, v143)')
    m.add_argument('--out', type=Path, help=f'default {DEFAULT_DIR.relative_to(ROOT).as_posix()}/pgo-<version>.profdata')
    m.add_argument('--toolchain', type=Path,
                   help='the folder of the compiler that built the instrumented game, with bin/llvm-profdata: an '
                        'MSYS2 UCRT64 folder or an llvm-mingw directory (default: the newest llvm-mingw under '
                        'toolchain/, else C:\\msys64\\ucrt64, else PATH)')
    a = ap.parse_args()
    out = (a.out or DEFAULT_DIR / f'pgo-{a.version}.profdata').resolve()
    merge(a.build.resolve(), out, a.version, a.toolchain)


if __name__ == '__main__':
    main()
