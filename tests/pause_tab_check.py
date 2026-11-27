# needs: clang
import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

import runtime_include

DESCRIPTION = ("Build and run tests/pause_tab_check.c: the settings tab in the game's pause menu "
               '(runtime/ui/pause_tab_core.h). With --romfs, also check the panes it reads against the game\'s '
               'PaSystemMenu_00 and PaMenuCategory_00.')
ROOT = Path(__file__).resolve().parents[1]
MSYS_BIN = Path(r'C:\msys64\ucrt64\bin')


def compiler():
    if os.environ.get('CC'):
        return os.environ['CC']
    if os.name == 'nt' and (MSYS_BIN / 'clang.exe').exists():
        return str(MSYS_BIN / 'clang.exe')
    return shutil.which('clang') or 'clang'


def tree_of(header, name):
    text = header.read_text(encoding='utf-8')
    start = text.index(f'{name}[] = {{') + len(name) + 6
    body = text[start:text.index('};', start)]
    pairs = []
    for part in body.split('}'):
        nums = [int(n) for n in part.replace('{', ' ').replace(',', ' ').split() if n.strip().isdigit()]
        if len(nums) == 2:
            pairs.append(tuple(nums))
    return pairs


def check_romfs(folder):
    sys.path.insert(0, str(ROOT / 'tools'))
    import layout_dump
    layouts = layout_dump.load(folder)
    header = ROOT / 'runtime' / 'ui' / 'pause_tab_core.h'
    fails = 0
    for lname, table, want in (('PaSystemMenu_00', 'PT_SYSTEM_TREE', {2: 'N_InOut_00'}),
                               ('PaMenuCategory_00', 'PT_CATEGORY_TREE', {3: 'N_Bg_00'})):
        lay = layouts.get(lname)
        if lay is None:
            print(f'FAIL: {lname} is not in the game\'s layouts')
            fails += 1
            continue
        for idx, parent in tree_of(header, table):
            got = lay.panes[idx].parent if idx < len(lay.panes) else None
            if got != parent:
                print(f'FAIL: {lname} pane {idx} has the parent {got}, not {parent}')
                fails += 1
        for idx, name in want.items():
            if lay.panes[idx].name != name:
                print(f'FAIL: {lname} pane {idx} is {lay.panes[idx].name}, not {name}')
                fails += 1
    over = layouts.get('MenuOverlay_00')
    cats = [p.name for p in over.panes if p.name.startswith('Pa_Category_')] if over else []
    if cats != [f'Pa_Category_{i:02d}' for i in range(4)]:
        print(f'FAIL: MenuOverlay_00 has the categories {cats}, not four')
        fails += 1
    if over:
        for idx, parent in tree_of(header, 'PT_OVERLAY_TREE'):
            if over.panes[idx].parent != parent:
                print(f'FAIL: MenuOverlay_00 pane {idx} has the parent {over.panes[idx].parent}, not {parent}')
                fails += 1
        text = header.read_text(encoding='utf-8')
        start = text.index('PT_GAME_X[PT_MOVED] = {') + len('PT_GAME_X[PT_MOVED] = {')
        game_x = [float(v.rstrip('f')) for v in text[start:text.index('}', start)].split(',')]
        moved = [over.panes[8 + i] for i in range(4)] + [over.panes[13], over.panes[14]]
        want = ['Pa_Category_00', 'Pa_Category_01', 'Pa_Category_02', 'Pa_Category_03', 'T_BtnL_00', 'T_BtnR_00']
        for pane, name, x in zip(moved, want, game_x):
            if pane.name != name or abs(pane.x - x) > 0.01:
                print(f'FAIL: MenuOverlay_00 pane {pane.index} is {pane.name} at x {pane.x}, not {name} at {x}')
                fails += 1
        if abs(over.panes[11].y) > 0.01 or abs(over.panes[14].y - 2) > 0.01:
            print(f'FAIL: the System category at y {over.panes[11].y} and R at {over.panes[14].y}, not 0 and 2')
            fails += 1
    print(f"pause_tab_check: the game's layouts {'differ' if fails else 'match'} (PaSystemMenu_00, PaMenuCategory_00, "
          'MenuOverlay_00)')
    return fails


def main():
    ap = argparse.ArgumentParser(description=DESCRIPTION)
    ap.add_argument('--romfs', type=Path, help='a folder with romfs.bin and index.json')
    args = ap.parse_args()
    work = ROOT / 'build' / 'test' / 'pause_tab'
    work.mkdir(parents=True, exist_ok=True)
    exe = work / ('pause_tab_check.exe' if os.name == 'nt' else 'pause_tab_check')
    env = dict(os.environ)
    if os.name == 'nt' and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    sanitize = ['-fsanitize=undefined', '-fsanitize-trap=all']
    if os.name != 'nt':
        sanitize.append('-fsanitize=address')
    rt = ROOT / 'runtime'
    subprocess.check_call([compiler(), '-O1', '-g', '-Wall', '-Wextra', '-Werror', *sanitize, *runtime_include.flags(rt),
                           str(ROOT / 'tests' / 'pause_tab_check.c'), '-o', str(exe), '-lm'], env=env)
    rc = subprocess.run([str(exe)], env=env, timeout=120).returncode
    if args.romfs:
        rc |= check_romfs(args.romfs) != 0
    return rc


if __name__ == '__main__':
    sys.exit(main())
