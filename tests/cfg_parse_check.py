# needs: clang
import os
import shutil
import subprocess
import sys
from pathlib import Path
import runtime_include

ROOT = Path(__file__).resolve().parents[1]
MSYS_BIN = Path(r'C:\msys64\ucrt64\bin')


def compiler():
    if os.environ.get('CC'):
        return os.environ['CC']
    if os.name == 'nt' and (MSYS_BIN / 'clang.exe').exists():
        return str(MSYS_BIN / 'clang.exe')
    return shutil.which('clang') or 'clang'


CLEAN = '''[frame rate]
pace_smooth = 1
fps_limit = 144
; a comment line
# another
[display]
sharpness = 80
color_gamma = 1.1
vk_present = FIFO
render_scale = 300
dlss_preset = k
pad_layout = xbox_prompts
monitor = 2
'''

BAD = '''[frame rate]
pace_smooth = 0 ; off
fullscreen = false
sharpness = 250
color_gamma = 2
vk_present = vsync
fps_limit = 144 # my monitor
sharpness_typo = 3
cloth_realtime = disabled
window_size = 1280x720 # small
aspect = ; stretch some day
dlss_preset = "k ; quoted"
'''

READ = '''[display]
fullscreen = false
rtss = OFF
hdr_live = Yes
cloth_realtime = on ; the default
pace_smooth = No
vk_present = fifo\t; tear-free
fps_limit = 144 # my monitor
window_size = 1280x720 # small
aspect = ; nothing yet
mods_dir = D:\\My Mods #2
dlss_preset = "k ; quoted"
'''


def run(exe, env, case, text, work, extra_env=None):
    ini = work / f'{case}_{len(text)}.ini'
    ini.write_text(text, encoding='utf-8')
    e = dict(env)
    for k in list(e):
        if k.upper().startswith(('NX2NAVI_', 'NX2SARIA_')):
            del e[k]
    e.update(extra_env or {})
    p = subprocess.run([str(exe), case, str(ini)], env=e, capture_output=True, text=True, errors='replace', timeout=60)
    if p.returncode:
        print(p.stdout, p.stderr)
    return p.returncode, p.stdout


def main():
    work = ROOT / 'build' / 'test' / 'cfg_parse'
    work.mkdir(parents=True, exist_ok=True)
    exe = work / ('cfg_parse_check.exe' if os.name == 'nt' else 'cfg_parse_check')
    env = dict(os.environ)
    if os.name == 'nt' and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    rt = ROOT / 'runtime'
    subprocess.check_call([compiler(), '-std=gnu11', '-O1', '-g', '-Wall', '-Wno-unused-function', *runtime_include.flags(rt),
                           str(ROOT / 'tests' / 'cfg_parse_check.c'), str(rt / 'cfg' / 'cfg.c'), '-o', str(exe)], env=env)
    bad = 0

    def check(ok, what, out=''):
        nonlocal bad
        print(f'{what}: {"ok" if ok else "FAIL"}', flush=True)
        if not ok:
            bad += 1
            print(out)

    rc, out = run(exe, env, 'load', CLEAN, work)
    lines = [l for l in out.splitlines() if l.startswith('log: cfg:') and '=' in l and 'line' in l]
    check(rc == 0 and not lines, 'a file whose values fit logs nothing about them', out)

    rc, out = run(exe, env, 'load', BAD, work, {'NX2SARIA_SHARPNESS': '500', 'NX2SARIA_VK_PRESENT': 'fifo'})
    want = [
        "line 2: pace_smooth = '0', and '; off' after it is a comment",
        "line 4: sharpness = '250' is not a number in 0..100",
        "line 5: color_gamma = '2' is not a number in 0.8..1.2",
        "line 6: vk_present = 'vsync' is none of the menu's choices mailbox|fifo|immediate",
        "line 7: fps_limit = '144', and '# my monitor' after it is a comment",
        "line 9: cloth_realtime = 'disabled' is not 0, 1, on, off, true, false, yes or no",
        "line 10: window_size = '1280x720 # small' a ';' or '#' after the value is part of it",
        "line 11: aspect = '', and '; stretch some day' after it is a comment",
        "line 12: dlss_preset = 'k ; quoted' is none of the menu's choices default|k|j; a ';' or '#' after the value "
        "is part of it",
        "the environment's NX2SARIA_SHARPNESS = '500' is not a number in 0..100",
    ]
    for w in want:
        check(rc == 0 and w in out, f'logged: {w}', out if w not in out else '')
    cfg_lines = [l for l in out.splitlines() if l.startswith('log: cfg: settings file line') and 'unknown key' not in l]
    check(len(cfg_lines) == 9, f'one line each for the nine lines, none for "fullscreen = false" ({len(cfg_lines)})',
          out)
    check(not any(' line 2: ' in l and 'is not 0' in l for l in cfg_lines), 'a cut value is checked as cut', out)
    check('NX2SARIA_VK_PRESENT' not in out, 'an environment value that fits is not logged', out)
    check("got: pace_smooth '0'" in out, 'the value is read without its comment', out)

    rc, out = run(exe, env, 'read', READ, work, {'NX2SARIA_DYN_RES': 'TRUE', 'NX2SARIA_SHADOW_PCF': '0 ; env note'})
    got = {l.split()[1]: l.split(' ', 2)[2] for l in out.splitlines() if l.startswith('got: ')}
    want = {'PACE_SMOOTH': "'0' bool 0 int 0", 'FULLSCREEN': "'0' bool 0 int 0", 'RTSS': "'0' bool 0 int 0",
            'HDR_LIVE': "'1' bool 1 int 1", 'CLOTH_REALTIME': "'1' bool 1 int 1", 'FPS_LIMIT': "'144'",
            'VK_PRESENT': "'fifo'", 'ASPECT': "'(unset)'", 'WINDOW_SIZE': "'1280x720 # small'",
            'DLSS_PRESET': "'k ; quoted'", 'MODS_DIR': "'D:\\My Mods #2'", 'DYN_RES': "'1' bool 1 int 1",
            'SHADOW_PCF': "'0 ; env note' bool 1 int 0"}
    for k, w in want.items():
        check(rc == 0 and got.get(k) == w, f'read: {k} {w}', f'got {got.get(k)!r}\n{out}' if got.get(k) != w else '')
    cut = [l for l in out.splitlines() if 'after it is a comment' in l]
    check(len(cut) == 4 and any("line 7: vk_present = 'fifo', and '; tear-free'" in l for l in cut),
          f'four cut lines, a tab before the comment too ({len(cut)})', out)
    check(not any(w in out for w in ('fullscreen =', 'rtss =', 'hdr_live =', 'pace_smooth =', 'NX2SARIA_DYN_RES =')),
          'no line about switches written as words', out)
    check("the environment's NX2SARIA_SHADOW_PCF = '0 ; env note' is not 0, 1, on, off, true, false, yes or no; a ';' "
          "or '#' after the value is part of it" in out, "the environment's comment stays in its value", out)

    many = '\n'.join(f'sharpness = {200 + i}' if i % 2 else f'fullscreen = no{i}' for i in range(2)) + '\n'
    many = ''.join(f'[s{i}]\nsharpness = {200 + i}\n' for i in range(1)) + ''.join(
        f'{k} = 7\n' for k in ('fullscreen', 'pace_smooth', 'vk_present', 'sharpness', 'color_gamma', 'perf_overlay',
                               'upscaler', 'standin_filter', 'dlss', 'dlss_preset', 'framegen', 'aa', 'aspect',
                               'texture_detail', 'color_preset', 'device_profile', 'mods_rebuild', 'operation_mode',
                               'text_speed', 'sprint_mode'))
    rc, out = run(exe, env, 'load', many, work)
    listed = sum(1 for l in out.splitlines() if l.startswith('log: cfg: settings file line') and "= '7'" in l)
    check(rc == 0 and listed == 16 and 'more values outside their settings' in out,
          f'20 values outside their sets: {listed} lines and a count', out)
    rc, out = run(exe, env, 'load', ''.join(f'vk_present = fifo ; note {i}\n' for i in range(20)), work)
    listed = sum(1 for l in out.splitlines() if 'after it is a comment' in l)
    check(rc == 0 and listed == 16 and '4 more comments after a value are not listed' in out,
          f'20 comments after values: {listed} lines and a count', out)
    full = ''.join(f'cheat_{0x1000 + i:08x} = 1\n' for i in range(600)) + 'low_latency = 1\n'
    rc, out = run(exe, env, 'full', full, work)
    load_lines = [l for l in out.splitlines() if 'has more than 512 keys' in l]
    set_lines = [l for l in out.splitlines() if 'settings table is full' in l]
    check(rc == 0 and len(load_lines) == 1 and 'line 513 (CHEAT_' in load_lines[0] and 'got: low_latency (unset)' in out,
          'a file with 601 keys: one line at load from line 513 on', out)
    check(rc == 0 and len(set_lines) == 1 and 'VK_PRESENT is not kept' in set_lines[0],
          'two menu changes past a full table: one line', out)
    gone = 'the low-end GPU mode was removed'
    unset = "mesh_emulation '(unset)' vram_reserve_mb '(unset)' default '0'"
    for text, extra, want, said in (
            ('low_end_gpu = Off\n', None, f'bool 0 {unset}', 0),
            ('low_end_gpu = 0 ; note\n', None, f'bool 0 {unset}', 0),
            ('low_end_gpu = yes\n', None, f'bool 1 {unset}', 1),
            ('low_end_gpu = 1\n', None, f'bool 1 {unset}', 1),
            ('; nothing\n', {'NX2SARIA_LOW_END_GPU': 'FALSE'}, f'bool 0 {unset}', 0),
            ('; nothing\n', {'NX2SARIA_LOW_END_GPU': '0 ; x'}, f'bool 1 {unset}', 1),
            ('mesh_emulation = 1\nvram_reserve_mb = 512\n', None,
             "bool 0 mesh_emulation '1' vram_reserve_mb '512' default '0'", 0),
            ('low_end_gpu = 1\nmesh_emulation = 1\n', None,
             "bool 1 mesh_emulation '1' vram_reserve_mb '(unset)' default '0'", 1)):
        rc, out = run(exe, env, 'lowend', text, work, extra)
        what = text.strip().replace('\n', ', ') if extra is None else f"the environment's {extra['NX2SARIA_LOW_END_GPU']!r}"
        check(rc == 0 and f'got: {want}' in out and out.count(gone) == said,
              f'low end: {what}: {want}, the removal said {said} time(s)', out)
    rc, out = run(exe, env, 'norow', CLEAN, work, {'NX2SARIA_NO_ROW_CHECK': 'abc'})
    check(rc == 0 and 'got: cfg_int 0 cfg_double 0' in out, 'cfg_int of a set, unparsable key without a row: 0', out)

    rc, out = run(exe, env, 'testset', '[display]\nfullscreen = 0\n', work)
    seg = dict(zip(['start', '1', '2', '3', '4', '5', '6', '7'], out.split('--- save ')))
    writes = {k: [l for l in v.splitlines() if l.startswith('write: ')] for k, v in seg.items()}
    text = {k: v[v.find('write: '):] if 'write: ' in v else '' for k, v in seg.items()}
    check(rc == 0 and 'got: dyn_res 1, menu_lang pt-BR' in seg.get('start', ''),
          'test values are read like any other setting', out)
    check(rc == 0 and not writes.get('1') and
          'log: cfg: menu test values not saved: 2 key(s): DYN_RES, MENU_LANG' in seg.get('1', ''),
          'a save with only test values writes nothing, and says what it left out', out)
    check(rc == 0 and len(writes.get('2', [])) == 1 and 'fullscreen = 1' in text['2'] and 'dyn_res' not in text['2']
          and 'menu_lang' not in text['2'] and 'not saved' not in seg['2'],
          "a player's change beside them is written alone, and the same count is not logged again", out)
    check(rc == 0 and 'dyn_res = 1' in text.get('3', '') and 'menu_lang' not in text.get('3', '') and
          'log: cfg: menu test values not saved: 1 key(s): MENU_LANG' in seg.get('3', ''),
          "the player's cfg_set of a test key makes it the player's, and it is saved", out)
    check(rc == 0 and 'got: kept 1' in seg.get('4', '') and 'menu_lang = pt-BR' in text.get('4', '') and
          sum('menu test values not saved' in seg.get(k, '') for k in '1234') == 2,
          'SAVE=1 (cfg_test_keep) lets the next save write the test values', out)
    check(rc == 0 and 'got: volume 50' in seg.get('5', '') and not writes.get('5') and
          'menu test values not saved: 1 key(s): VOLUME' in seg.get('5', ''),
          "a menu preview of a test value, reverted, leaves it the test's: not saved", out)
    check(rc == 0 and len(writes.get('6', [])) == 1 and 'VOLUME (removed)' in seg.get('6', ''),
          "the menu's reset of a test key is the player's, and saved", out)
    check(rc == 0 and 'vsync = 0' in text.get('7', ''),
          "a test value equal to the player's unsaved change leaves that change the player's", out)

    rc, out = run(exe, env, 'read', READ, work,
                  {'NX2SARIA_FULLSCREEN': '1', 'NX2NAVI_FULLSCREEN': '0'})
    check(rc == 0 and "got: FULLSCREEN '1' bool 1 int 1" in out,
          'NX2SARIA_ wins when both environment spellings are set', out)
    rc, out = run(exe, env, 'read', READ, work,
                  {'NX2SARIA_FULLSCREEN': '', 'NX2NAVI_FULLSCREEN': '0'})
    check(rc == 0 and "got: FULLSCREEN '' bool 0 int 0" in out,
          'an explicitly empty NX2SARIA_ value still takes precedence over the legacy alias', out)
    rc, out = run(exe, env, 'read', READ, work, {'NX2NAVI_FULLSCREEN': '1'})
    check(rc == 0 and "got: FULLSCREEN '1' bool 1 int 1" in out,
          'legacy NX2NAVI_ environment spelling remains supported', out)

    OLDER = '[display]\nNX2NAVI_SHARPNESS = 70\nfps_limit = 90\n'
    env_line = 'log: cfg: NX2NAVI_FULLSCREEN read under its older name; NX2SARIA_FULLSCREEN is the current one'
    file_line = 'log: cfg: settings file line 2: NX2NAVI_SHARPNESS read under its older name; sharpness is the current one'
    rc, out = run(exe, env, 'older', OLDER, work, {'NX2NAVI_FULLSCREEN': '1'})
    check(rc == 0 and "got: FULLSCREEN '1'" in out and "got: SHARPNESS '70'" in out and "got: FPS_LIMIT '90'" in out,
          'an NX2NAVI_ environment variable and an NX2NAVI_ settings file key still apply', out)
    check(rc == 0 and out.count(env_line) == 1 and out.count(file_line) == 1 and out.count('older name') == 2,
          'the log names each older name once, the file read twice', out)
    rc, out = run(exe, env, 'older', OLDER, work,
                  {'NX2SARIA_FULLSCREEN': '0', 'NX2NAVI_FULLSCREEN': '1', 'NX2SARIA_SHARPNESS': '40'})
    check(rc == 0 and "got: FULLSCREEN '0'" in out and "got: SHARPNESS '40'" in out and
          'log: cfg: NX2NAVI_FULLSCREEN is not read: NX2SARIA_FULLSCREEN is set too and wins' in out and env_line not in out,
          'NX2SARIA_ wins over NX2NAVI_ in the environment, the environment over the file, and the log says which', out)
    rc, out = run(exe, env, 'older', OLDER.replace('NX2NAVI_SHARPNESS', 'sharpness'), work, {'NX2SARIA_FULLSCREEN': '1'})
    check(rc == 0 and "got: SHARPNESS '70'" in out and 'older name' not in out and 'is not read' not in out,
          'the current names give no older-name line', out)

    game = work / 'nearby-game'
    game.mkdir(exist_ok=True)
    for old in game.glob('*.ini'):
        old.unlink()
    exe_path = game / 'nx2saria.exe'
    (game / 'nx2saria.ini').write_text('sharpness = 80\n', encoding='utf-8')
    (game / 'nx2navi.ini').write_text('sharpness = 20\n', encoding='utf-8')
    path_env = dict(env, NX2SARIA_TEST_EXE=str(exe_path))
    p = subprocess.run([str(exe), 'nearby', '-'], env=path_env, capture_output=True, text=True, timeout=60)
    check(p.returncode == 0 and "nearby: sharpness '80'" in p.stdout and 'is beside nx2saria.ini and is not read' in p.stdout
          and (game / 'nx2navi.ini').exists(),
          'nx2saria.ini takes precedence when both filenames exist, and the log names the other', p.stdout)
    (game / 'nx2saria.ini').unlink()
    p = subprocess.run([str(exe), 'nearby', '-'], env=dict(path_env, NX2SARIA_TEST_RENAME_FAIL='1'),
                       capture_output=True, text=True, timeout=60)
    check(p.returncode == 0 and "nearby: sharpness '20'" in p.stdout and 'could not be renamed' in p.stdout
          and (game / 'nx2navi.ini').exists() and not (game / 'nx2saria.ini').exists(),
          'a refused rename keeps reading nx2navi.ini under its old name', p.stdout)
    p = subprocess.run([str(exe), 'nearby', '-'], env=path_env, capture_output=True, text=True, timeout=60)
    check(p.returncode == 0 and "nearby: sharpness '20'" in p.stdout and 'renamed ' in p.stdout
          and (game / 'nx2saria.ini').read_text(encoding='utf-8') == 'sharpness = 20\n' and not (game / 'nx2navi.ini').exists(),
          'an nx2navi.ini without an nx2saria.ini is renamed to it, settings and all', p.stdout)
    (game / 'nx2saria.ini').unlink()
    p = subprocess.run([str(exe), 'nearby', '-'], env=path_env, capture_output=True, text=True, timeout=60)
    check(p.returncode == 0 and 'no settings file at ' + str(game / 'nx2saria.ini') in p.stdout,
          'a fresh install with no adjacent INI selects the new filename', p.stdout)
    print(f'cfg_parse_check: {"FAILED" if bad else "ok"}')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
