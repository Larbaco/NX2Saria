# needs: git
# args: --self-test
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / 'tools' / 'rename_legacy_names.py'
sys.path.insert(0, str(ROOT / 'tools'))
import rename_legacy_names as rl  # noqa: E402

RENAMED = (
    ('kernel.c', '    int on = cfg_bool("NX2NAVI_FPS60");\n', '    int on = cfg_bool("NX2SARIA_FPS60");\n'),
    ('patches-v111.txt', 'set fps60    NX2NAVI_FPS60          1\n', 'set fps60    NX2SARIA_FPS60          1\n'),
    ('sync_wake_check.c', '    snprintf(key, sizeof key, "NX2NAVI_%s", name);\n',
     '    snprintf(key, sizeof key, "NX2SARIA_%s", name);\n'),
    ('run_game.py', '    --env NX2NAVI_TRACE=perf --env NX2NAVI_SHOT_DIR=shots\n',
     '    --env NX2SARIA_TRACE=perf --env NX2SARIA_SHOT_DIR=shots\n'),
)
KEPT = (
    ('cfg.c', '    return e ? e : getenv(legacy);   // NX2SARIA_FPS60 first, then NX2NAVI_FPS60\n'),
    ('gameini.py', "PREFIXES = ('NX2SARIA_', 'NX2NAVI_')\n"),
    ('discord_rpc.c', '"(legacy NX2NAVI_DISCORD_APP_ID is also accepted)"\n'),
    ('stutter_map.py', 'NX2NAVI_STUTTER_MAP spelling still works)\n'),
    ('cfg_support.c', '    else if (!strncasecmp(name, "NX2NAVI_", 8)) name += 8;\n'),
    ('check_content.py', 'GENERATED_MARKER = "NX2NAVI_" + "GENERATED_FROM_GAME_CODE"\n'),
    ('linux.c', '#ifdef NX2NAVI_PGO_GEN\n'),
    ('cfg_parse_check.py', "    rc, out = run(exe, env, 'read', READ, work, {'NX2NAVI_FULLSCREEN': '1'})\n"),
)


def rule_cases():
    bad = 0
    for name, line, want in RENAMED:
        got = rl.rename_line(name, line)
        if got != want:
            print(f'legacy_names_check: planted {name}: {line.strip()!r} gave {got.strip()!r}, not {want.strip()!r}')
            bad += 1
    for name, line in KEPT:
        got = rl.rename_line(name, line)
        if got != line:
            print(f'legacy_names_check: planted {name}: the fallback line {line.strip()!r} was renamed to {got.strip()!r}')
            bad += 1
    read = [f for f in rl.files() if f.startswith(rl.SKIP_DIRS)]
    if read:
        print(f'legacy_names_check: the script reads files it must leave alone: {", ".join(read[:5])}')
        bad += 1
    print(f'legacy_names_check: {len(RENAMED)} planted names renamed, {len(KEPT)} fallback lines kept, '
          f'{len(rl.SKIP_DIRS)} folders left out: {"FAILED" if bad else "ok"}')
    return bad


def check_tree(root, script):
    p = subprocess.run([sys.executable, str(script), '--check'], cwd=root, capture_output=True, text=True,
                       errors='replace', timeout=300)
    return p.returncode, p.stdout + p.stderr


def planted_repo():
    with tempfile.TemporaryDirectory(prefix='legacy-names-') as d:
        d = Path(d)
        (d / 'tools').mkdir()
        (d / 'runtime').mkdir()
        shutil.copy2(SCRIPT, d / 'tools' / SCRIPT.name)
        (d / 'runtime' / 'planted.c').write_text('int planted(void) { return cfg_bool("NX2NAVI_FPS60"); }\n',
                                                 encoding='utf-8')
        env = dict(os.environ, GIT_CONFIG_GLOBAL=os.devnull, GIT_CONFIG_SYSTEM=os.devnull)
        for cmd in (['git', 'init', '-q'], ['git', 'add', '-A']):
            subprocess.run(cmd, cwd=d, env=env, check=True, capture_output=True)
        rc, out = check_tree(d, d / 'tools' / SCRIPT.name)
    caught = rc == 1 and 'runtime/planted.c:1' in out
    print(f'legacy_names_check: an older name planted in a scratch repository: {"caught" if caught else "MISSED"}')
    if not caught:
        print(out)
    return 0 if caught else 1


def main():
    bad = rule_cases()
    if '--self-test' in sys.argv[1:]:
        bad += planted_repo()
    rc, out = check_tree(ROOT, SCRIPT)
    if rc:
        print(out.rstrip())
        print('legacy_names_check: the lines above still spell a setting NX2NAVI_*; '
              'python tools/rename_legacy_names.py renames them')
        bad += 1
    else:
        print(f'legacy_names_check: {out.strip().splitlines()[-1]}')
    print(f'legacy_names_check: {"FAILED" if bad else "ok"}')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
