# needs: clang
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path
import runtime_include

ROOT = Path(__file__).resolve().parents[1]
MSYS_BIN = Path(r'C:\msys64\ucrt64\bin')
UI = ROOT / 'runtime' / 'ui'

BYPASSES = ('cheats_set', 'cfg_set', 'cfg_set_test', 'cfg_reset', 'cfg_save', 'opt_save_now', 'rt_patch_switch')


def compilers():
    cc = os.environ.get('CC') or (str(MSYS_BIN / 'clang.exe') if os.name == 'nt' and (MSYS_BIN / 'clang.exe').exists()
                                  else shutil.which('clang') or 'clang')
    cxx = os.environ.get('CXX') or (str(Path(cc).with_name(Path(cc).name.replace('clang', 'clang++')))
                                    if 'clang' in Path(cc).name else shutil.which('clang++') or 'clang++')
    return cc, cxx


def code_of(src):
    src = re.sub(r'/\*.*?\*/', '', src, flags=re.S)
    return '\n'.join(re.sub(r'//.*', '', line) for line in src.splitlines())


def body_of(code, head):
    start = code.index(head)
    depth, i = 0, code.index('{', start)
    for j in range(i, len(code)):
        depth += (code[j] == '{') - (code[j] == '}')
        if not depth:
            return code[i:j + 1]
    raise AssertionError(f'{head}: no end')


def menu_checks(menu):
    bad = []
    enum = re.search(r'^enum \{ TAB_SCREEN,[^}]*\};\n', menu, re.M)
    shown = re.search(r'^static int tabs_shown\(void\) \{[^\n]*\}\n', menu, re.M)
    if not enum or not shown:
        return ['runtime/ui/menu.cpp: the tabs\' enum or tabs_shown() was not found'], ''
    if not re.search(r'TAB_CHEATS,\s*TAB_COUNT\s*\}', enum.group(0)):
        bad.append('runtime/ui/menu.cpp: TAB_CHEATS is not the last tab of the enum, which tabs_shown() cuts off')
    layout = body_of(menu, 'static void layout_menu(')
    if not re.search(r'case TAB_CHEATS:\s*tab_cheats\(scale\);', layout):
        bad.append('runtime/ui/menu.cpp layout_menu(): no case TAB_CHEATS that calls tab_cheats(scale)')
    if not re.search(r'const int ntabs = tabs_shown\(\);', layout):
        bad.append('runtime/ui/menu.cpp layout_menu(): the tab count is not tabs_shown()')
    names = re.search(r'kTabNames\[TAB_COUNT\] = \{([^}]*)\}', menu)
    if not names or names.group(1).split(',')[-1].strip() != 'UI_MENU_TAB_CHEATS':
        bad.append('runtime/ui/menu.cpp kTabNames: the last name is not UI_MENU_TAB_CHEATS')
    rest = menu.replace(enum.group(0), '').replace(shown.group(0), '').replace('kTabNames[TAB_COUNT]', '')
    for m in re.finditer(r'\bTAB_COUNT\b', rest):
        line = rest[:m.start()].count('\n') + 1
        bad.append(f'runtime/ui/menu.cpp: TAB_COUNT used as the count of tabs shown (line {line} without the enum '
                   'and tabs_shown() lines): use tabs_shown()')
    return bad, enum.group(0) + shown.group(0)


def tab_checks(tab):
    bad = []
    body = body_of(tab, 'void tab_cheats(float scale)')
    for name in BYPASSES:
        if re.search(r'\b' + name + r'\s*\(', body):
            bad.append(f'runtime/ui/cheats_tab.cpp tab_cheats(): calls {name}(), which acts outside the draft')
    if not re.search(r'opt_toggle_text\(c->key,[^;]*\{\s*opt_change\(c->key,', body, re.S):
        bad.append('runtime/ui/cheats_tab.cpp tab_cheats(): the toggle row does not hand its change to opt_change()')
    return bad


def cheats_checks(cheats):
    bad = []
    for name in ('cfg_set', 'cfg_save', 'cfg_reset'):
        if re.search(r'\b' + name + r'\s*\(', cheats):
            bad.append(f'runtime/patches/cheats.c: calls {name}(): a cheat follows its setting and does not write it')
    frame = body_of(cheats, 'void cheats_frame(uint64_t frame)')
    if not re.search(r'^\{\s*follow_settings\(\);', frame):
        bad.append('runtime/patches/cheats.c cheats_frame(): follow_settings() is not its first call')
    return bad


def main():
    menu = code_of((UI / 'menu.cpp').read_text(encoding='utf-8'))
    bad, cut = menu_checks(menu)
    bad += tab_checks(code_of((UI / 'cheats_tab.cpp').read_text(encoding='utf-8')))
    bad += cheats_checks(code_of((ROOT / 'runtime' / 'patches' / 'cheats.c').read_text(encoding='utf-8')))
    for b in bad:
        print('FAIL ' + b)
    if not cut:
        return 1
    work = ROOT / 'build' / 'test' / 'cheats_tab'
    if work.exists():
        shutil.rmtree(work)
    work.mkdir(parents=True)
    (work / 'menu_tabs_cut.h').write_text(cut, encoding='utf-8')
    env = dict(os.environ)
    if os.name == 'nt' and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    for k in list(env):
        if k.upper().startswith(('NX2NAVI_', 'NX2SARIA_')) or k == 'SteamDeck':
            del env[k]
    cc, cxx = compilers()
    rt = ROOT / 'runtime'
    inc = [*runtime_include.flags(rt), '-I', str(rt / 'third_party' / 'imgui'), '-I', str(ROOT / 'tests'),
           '-I', str(work)]
    keep = ROOT / 'build' / 'test' / 'cheats-tab-imgui'
    keep.mkdir(parents=True, exist_ok=True)
    objs = []
    for src in sorted((rt / 'third_party' / 'imgui').glob('imgui*.cpp')):
        obj = keep / (src.stem + '.o')
        if not obj.is_file() or obj.stat().st_mtime < src.stat().st_mtime:
            subprocess.check_call([cxx, '-std=gnu++17', '-c', '-O1', '-w', *inc, str(src), '-o', str(obj)], env=env)
        objs.append(str(obj))
    for src, lang in ((UI / 'option_rows.cpp', 'c++'), (ROOT / 'tests' / 'cheats_tab_check.cpp', 'c++'),
                      (rt / 'cfg' / 'cfg.c', 'c'), (UI / 'ui_text.c', 'c')):
        obj = work / (src.stem + '.o')
        std = ['-std=gnu++17'] if lang == 'c++' else ['-std=gnu11']
        subprocess.check_call([cxx if lang == 'c++' else cc, '-x', lang, *std, '-c', '-O1', '-g', '-Wall',
                               '-Wno-unused-function', *inc, str(src), '-o', str(obj)], env=env)
        objs.append(str(obj))
    exe = work / ('cheats_tab_check.exe' if os.name == 'nt' else 'cheats_tab_check')
    subprocess.check_call([cxx, *objs, '-o', str(exe), '-lm'], env=env)
    game = work / 'game'
    game.mkdir()
    ini = game / 'nx2saria.ini'
    ini.write_text('[cheats]\ncheat_0000beef = 1\n', encoding='utf-8')
    p = subprocess.run([str(exe), str(game)], env=dict(env, NX2SARIA_TEST_HW='cores=8,threads=16,ram=16000'),
                       capture_output=True, text=True, timeout=60)
    print(p.stdout, end='')
    print(p.stderr, end='')
    want = ['log: menu: draft + CHEAT_0123ABCD: (default) -> \'1\' (staged: waits for Apply)',
            'log: menu: draft   CHEAT_0123ABCD: (default) -> \'1\', saved',
            'log: menu: draft   CHEAT_0000BEEF: \'1\' -> \'0\', saved']
    for line in want:
        if line not in p.stdout.splitlines():
            bad.append(f'the draft\'s log has no line "{line[5:]}"')
            print('FAIL ' + bad[-1])
    saved = ini.read_text(encoding='utf-8')
    for line in ('cheat_0123abcd = 1', 'cheat_0000beef = 0'):
        if not re.search(r'^' + re.escape(line) + r'\s*$', saved, re.M):
            bad.append(f'nx2saria.ini after Apply has no line "{line}"')
            print('FAIL ' + bad[-1])
    if bad or p.returncode:
        return 1
    print('cheats_tab_check: passed')
    return 0


if __name__ == '__main__':
    sys.exit(main())
