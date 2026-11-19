"""Build and run tests/preset_check.c: the quality presets (runtime/preset_pick.h, runtime/cfg.c "quality presets").

- the tier made-up hardware gets (the bench PC's RX 6700 with 10 GB, 16 threads and 32 GB is high), the handheld
  names of Linux's DMI tables and Windows' registry, and what each tier sets: high nothing, no tier frame generation,
  DLSS and FSR 4 only below high and only on the cards that run them, a frame-rate limit only on an integrated GPU or
  a handheld and never above 60 (preset_check.c pure);
- every value of the table read by cfg.c as the settings file's and as a reload's: none outside its setting's
  choices (preset_check.c values);
- cfg_init and cfg_start with made-up hardware, a settings file and a state file (preset_check.c run): the layer's
  place (the player's keys win, the device profile next, then the preset, then the table), the log lines, the state file
  (written only when its text changes, never nx2saria.ini), a tier chosen on other hardware giving way to the
  recommended one until the player chooses again, a probe that cannot run going by the last one, a tool mode's start
  (no probe, no state file written), a state file of garbage, and the menu's calls (cfg_preset_use,
  cfg_preset_chosen, cfg_preset_custom, cfg_preset_use_from, cfg_default_tier, cfg_preset_differs) and a reload of
  the preset line, also after a switch in the menu; the one-time notice (cfg_preset_notice_due, the state file's
  noticed line).

No GPU, no game files.

Usage: preset_check.py
"""
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MSYS_BIN = Path(r'C:\msys64\ucrt64\bin')

RX6700 = ('name=AMD Radeon RX 6700 (RADV NAVI22),vendor=0x1002,device=0x73df,driver=0x1,type=discrete,vram=10224,'
          'mesh=1,cores=8,threads=16,ram=31900')
GTX1650 = ('name=NVIDIA GeForce GTX 1650,vendor=0x10de,device=0x1f82,driver=0x100,type=discrete,vram=4096,mesh=1,'
           'cores=6,threads=12,ram=16000')
RTX4090 = ('name=NVIDIA GeForce RTX 4090,vendor=0x10de,device=0x2684,driver=0x200,type=discrete,vram=24564,mesh=1,'
           'cores=16,threads=32,ram=64000')
RTX4060_LOWRAM = ('name=NVIDIA GeForce RTX 4060,vendor=0x10de,device=0x2882,driver=0x200,type=discrete,vram=8188,'
                  'mesh=1,cores=6,threads=12,ram=11900')
OWNER = ('name=NVIDIA GeForce RTX 3050 6GB Laptop GPU,vendor=0x10de,device=0x25ac,driver=0x88848000,type=discrete,'
         'vram=6017,mesh=1,cores=8,threads=16,ram=16087')
OWNER_INI = ('[display]\nfullscreen = 1\nperf_overlay = fps\nupscaler = bilinear\ndlaa = 1\ndlss = quality\n'
             'framegen = fsr3\ntaa = fsr3\nsharpness = 100\n[frame rate]\nfps_limit = unlimited\n')
FATAL70 = ('name=AMD Radeon RX 6650 XT,vendor=0x1002,device=0x73ef,driver=0x1,type=discrete,vram=8176,mesh=1,cores=6,'
           'threads=12,ram=15667')
DECK = ('name=AMD Custom GPU 0405 (RADV VANGOGH),vendor=0x1002,device=0x163f,driver=0x1,type=integrated,vram=1024,'
        'mesh=1,cores=4,threads=8,ram=14800,hand=Steam Deck')
RX6700_STATE = ('# a state file of an earlier start\nhardware = 1002:73df:1:10224:16:31:none\n'
                'chosen_on = 1002:73df:1:10224:16:31:none\n')
RTX4060_CACHE = ('hardware = 10de:2882:200:8188:12:31:none\ngpu_name = NVIDIA GeForce RTX 4060\ngpu_vendor = 0x10de\n'
                 'gpu_device = 0x2882\ngpu_driver = 0x200\ngpu_type = discrete\ngpu_vram_mib = 8188\ngpu_mesh = 1\n')
HARDWARE_LINE = re.compile(
    r'^log: preset: hardware: (GPU "[^"]*" \(vendor 0x[0-9a-f]{4} device 0x[0-9a-f]{4}, (discrete|integrated|other), '
    r'\d+ MiB video memory, mesh shaders (yes|no), DLSS (yes|no), FSR 4 (yes|no)\)|GPU none \([^)]*\)); '
    r'CPU \d+ cores / \d+ threads; RAM \d+\.\d GiB; handheld [^;]+; (probe \d+\.\d ms|probe skipped: .+)$', re.M)


def compiler():
    if os.environ.get('CC'):
        return os.environ['CC']
    if os.name == 'nt' and (MSYS_BIN / 'clang.exe').exists():
        return str(MSYS_BIN / 'clang.exe')
    return shutil.which('clang') or 'clang'


def main():
    work = ROOT / 'build' / 'test' / 'preset'
    if work.exists():
        shutil.rmtree(work)
    work.mkdir(parents=True)
    exe = work / ('preset_check.exe' if os.name == 'nt' else 'preset_check')
    env = dict(os.environ)
    if os.name == 'nt' and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    for k in list(env):
        if k.upper().startswith(('NX2NAVI_', 'NX2SARIA_')) or k == 'SteamDeck':
            del env[k]
    rt = ROOT / 'runtime'
    subprocess.check_call([compiler(), '-std=gnu11', '-O1', '-g', '-Wall', '-Wno-unused-function', '-I', str(rt),
                           str(ROOT / 'tests' / 'preset_check.c'), str(rt / 'cfg.c'), '-o', str(exe)], env=env)
    bad = 0

    def check(ok, what, out=''):
        nonlocal bad
        print(f'{what}: {"ok" if ok else "FAIL"}', flush=True)
        if not ok:
            bad += 1
            print(out)

    p = subprocess.run([str(exe), 'pure'], env=env, capture_output=True, text=True, timeout=60)
    print(p.stdout, end='')
    check(p.returncode == 0, 'preset_pick.h', p.stderr)

    d = work / 'values'
    d.mkdir()
    p = subprocess.run([str(exe), 'values', str(d)], env=env, capture_output=True, text=True, timeout=60)
    misfits = [l for l in p.stdout.splitlines() if 'settings file line' in l or l.startswith(('FAIL', 'FATAL'))]
    m = re.search(r'values: (\d+) checked', p.stdout)
    check(p.returncode == 0 and not misfits and m and int(m.group(1)) > 20,
          f'every preset value fits its setting, read and reloaded ({m.group(1) if m else 0} values)',
          '\n'.join(misfits) or p.stdout[-2000:])

    runs = 0

    def run(hw, ini=None, state=None, actions=(), extra=None, folder=None):
        """cfg_init and cfg_start in a folder of its own (or folder, kept from an earlier run) with the hardware hw."""
        nonlocal runs
        runs += 1
        f = folder or work / f'run{runs}'
        f.mkdir(exist_ok=True)
        if ini is not None:
            (f / 'nx2saria.ini').write_text(ini, encoding='utf-8')
        if state is not None:
            (f / 'preset-state.txt').write_text(state, encoding='utf-8')
        e = dict(env, NX2SARIA_TEST_HW=hw)
        e.update(extra or {})
        p = subprocess.run([str(exe), 'run', str(f), *actions], env=e, capture_output=True, text=True, timeout=60)
        if p.returncode:
            print(p.stdout, p.stderr)
        return p.returncode, p.stdout, f

    def get(out, what, part=0):
        """What 'got: <what> ...' says in the part'th report (0: after the start, 1: after the first action, ...)."""
        text = out.split('--- after')[part]
        m = re.search(r'^got: ' + re.escape(what) + r' (.*)$', text, re.M)
        return m.group(1) if m else None

    def state_of(folder):
        path = folder / 'preset-state.txt'
        return path.read_text(encoding='utf-8') if path.exists() else ''

    # ---- the bench PC: high, which sets nothing; the state file once, and not again while nothing changed
    rc, out, f = run(RX6700, ini='[display]\nsharpness = 80\n')
    check(rc == 0 and get(out, 'in_use') == 'high' and 'log: preset: high sets nothing: the game as shipped' in out,
          'bench PC: recommended high, which sets nothing', out)
    check(get(out, 'get SCENE_SHARE') == '(unset)' and get(out, 'default SCENE_SHARE') == '0.75'
          and get(out, 'default GAME_RES') == '1440', 'high: the table defaults', out)
    check(bool(HARDWARE_LINE.search(out)) and 'log: preset: recommended high: 10 GiB of video memory' in out
          and 'log: preset: in use high (recommended)' in out, 'the hardware, recommended and in-use lines', out)
    st = state_of(f)
    check('hardware = 1002:73df:1:10224:16:31:none' in st and 'chosen_on' not in st and 'gpu_vram_mib = 10224' in st,
          'the state file: this hardware and its GPU, no choice', st)
    check(not re.search(r'^write: .*nx2saria\.ini$', out, re.M), 'nx2saria.ini is not written', out)
    rc, out, f = run(RX6700, folder=f)
    check(rc == 0 and 'write:' not in out, 'a second start with nothing changed writes no file', out)

    # ---- a GTX 1650: low; a key of the settings file wins over the preset and makes it custom
    rc, out, f = run(GTX1650, ini='[display]\naniso = 16\n')
    check(get(out, 'in_use') == 'low' and get(out, 'get SCENE_SHARE') == '0.5' and get(out, 'get GAME_RES') == '1080'
          and get(out, 'get FOLIAGE') == 'reduced' and get(out, 'get FPS_LIMIT') is None,  # no tier's key on this card
          'GTX 1650: low, its values in effect', out)
    check(get(out, 'get ANISO') == '16' and get(out, 'default ANISO') == '4' and get(out, 'custom') == '1',
          "the settings file's aniso wins; the preset's is the default; the preset is custom", out)
    check('log: preset:   ANISO = 16 (settings file; the preset\'s 4 stays unused)' in out
          and 'log: preset:   SCENE_SHARE = 0.5 (preset)' in out, 'the per-key lines', out)
    check(get(out, 'get TEXTURE_DETAIL') is None, 'a 4 GiB card keeps texture_detail = auto on low: no tier sets it', out)

    # ---- the menu's switch: values and their log lines, restart-only ones pending, the version bumped
    rc, out, f = run(GTX1650, ini='', actions=('use=high', 'use=low'))
    check('log: preset: low -> high: SCENE_SHARE 0.5 -> 0.75' in out and 'log: preset: low -> high: GAME_RES 1080 -> 1440'
          in out and get(out, 'version bumped', 1) == '1', 'cfg_preset_use logs each change and bumps the version', out)
    check(get(out, 'in_use', 1) == 'high' and get(out, 'pending GAME_RES', 1) == '1'
          and get(out, 'pending SCENE_SHARE', 1) == '0', 'after the switch: game_res waits for a restart, scene_share not',
          out)
    check(get(out, 'in_use', 2) == 'low' and get(out, 'pending GAME_RES', 2) == '0', 'and back', out)

    # ---- a tier the player chose: kept, recorded as chosen on this PC, logged against the recommended one
    rc, out, f = run(RTX4090, ini='preset = low\n')
    check(get(out, 'in_use') == 'low' and 'log: preset: in use low (settings file)' in out
          and 'log: preset: low chosen; the recommended for this PC is ultra' in out, "the player's tier", out)
    check(get(out, 'get DLAA') == '1' and get(out, 'get TAA') == 'dlss' and get(out, 'get DLSS') == 'performance',
          'low on an RTX card: DLSS performance', out)
    st = state_of(f)
    check('chosen_on = 10de:2684:200:24564:32:63:none' in st, 'a first choice is recorded as made on this PC', st)
    rc, out, f = run(RTX4090, ini='preset = medium\n', extra={'NX2SARIA_PRESET': 'ultra'})
    check(get(out, 'in_use') == 'ultra' and 'log: preset: in use ultra (environment)' in out
          and get(out, 'get SCENE_SHARE') == '1' and get(out, 'get GAME_RES') == 'window', 'the environment wins', out)

    # ---- new hardware: the chosen tier gives way to the recommended one until the player chooses again
    rc, out, f = run(RTX4090, ini='preset = low\n', state=RX6700_STATE, actions=('chosen',))
    check(get(out, 'in_use') == 'ultra' and get(out, 'hw_changed') == '1'
          and 'log: preset: in use ultra (recommended: the hardware changed since low was chosen)' in out
          and 'log: preset: low was chosen on 1002:73df:1:10224:16:31:none, and this PC is 10de:2684:200:24564:32:63:none'
          in out, 'a tier chosen on other hardware: the recommended one, said so', out)
    check(get(out, 'hw_changed', 1) == '0' and 'chosen_on = 10de:2684:200:24564:32:63:none' in state_of(f),
          'cfg_preset_chosen records this PC and clears the flag', out + state_of(f))
    rc, out, f = run(RTX4090, folder=f)
    check(get(out, 'in_use') == 'low' and get(out, 'hw_changed') == '0', 'the next start keeps the chosen tier', out)
    newer = RTX4090.replace('driver=0x200', 'driver=0x300')
    rc, out, f = run(newer, folder=f)
    check(get(out, 'in_use') == 'low' and get(out, 'hw_changed') == '0', 'a new driver is no new hardware', out)

    # ---- no probe: the last probe's GPU, else nothing known (high)
    rc, out, f = run('skip=a test of a start without the probe,cores=6,threads=12,ram=32000', ini='',
                          state=RTX4060_CACHE)
    check('GPU "NVIDIA GeForce RTX 4060"' in out and 'DLSS yes' in out
          and 'probe skipped: a test of a start without the probe; the GPU as the last probe saw it' in out
          and get(out, 'in_use') == 'high', "a start that cannot probe goes by the last probe's GPU", out)
    check('gpu_name = NVIDIA GeForce RTX 4060' in state_of(f), "and keeps it in the state file", state_of(f))
    rc, out, f = run('skip=a test,cores=6,threads=12,ram=32000', ini='')
    check('GPU none (not probed, and no earlier probe kept)' in out and get(out, 'in_use') == 'high',
          'without one: nothing known, high', out)
    check(bool(HARDWARE_LINE.search(out)), 'the hardware line without a GPU', out)

    # ---- RAM caps: an RTX 4060 with 12 GB is medium, through DLSS quality
    # ---- the owner's laptop and settings file: its own lines keep their values, and a preset chosen in the menu hands
    # back exactly the ones a tier sets on this card (fps_limit, framegen and upscaler are no preset's here)
    rc, out, f = run(OWNER, ini=OWNER_INI)
    check(get(out, 'recommended') == 'medium (6 GiB of video memory)' and get(out, 'in_use') == 'medium'
          and get(out, 'get DLAA') == '1' and get(out, 'get DLSS') == 'quality' and get(out, 'get TAA') == 'fsr3'
          and get(out, 'get GAME_RES') == '1080' and get(out, 'custom') == '1' and get(out, 'handback') == 'DLAA,TAA,DLSS'
          and 'log: preset:   TAA = fsr3 (settings file; the preset\'s dlss stays unused)' in out
          and get(out, 'get FPS_LIMIT') is None and not f.joinpath('nx2saria.ini').read_text().count('preset'),
          "the owner's laptop: medium, its settings file's dlaa, dlss and taa win and are the ones handed back", out)
    rc, out, f = run(FATAL70, ini='')
    check(get(out, 'recommended') == 'high (8 GiB of video memory)' and get(out, 'in_use') == 'high',
          'RX 6650 XT, Ryzen 5 4650G, 16 GB: high', out)

    rc, out, f = run(RTX4060_LOWRAM, ini='')
    check(get(out, 'in_use') == 'medium' and get(out, 'get DLSS') == 'quality' and get(out, 'get GAME_RES') == '1080',
          '12 GB of RAM: medium, DLSS quality on RTX', out)

    # ---- the Steam Deck: low below the device profile
    rc, out, f = run(DECK, ini='', extra={'SteamDeck': '1'})
    check(get(out, 'in_use') == 'low' and get(out, 'get FPS_LIMIT') == '40' and get(out, 'get SCENE_SHARE') == '0.4'
          and 'log: preset:   FPS_LIMIT = 40 (device profile)' in out
          and 'log: preset: Steam Deck: the GPU clock is left to the system' in out, 'Steam Deck: low under the device profile', out)

    # ---- a value that is no tier, and a reload of the preset line
    rc, out, f = run(RX6700, ini='preset = epic\n')
    check(get(out, 'in_use') == 'high' and "log: preset: 'epic' is no preset: recommended" in out
          and "preset = 'epic' is none of the menu's choices" in out, 'a value that is no tier: recommended', out)
    rc, out, f = run(RX6700, ini='', actions=('ini=preset = medium', 'reload'))
    check(get(out, 'in_use', 2) == 'medium' and 'log: preset: the settings file now says medium' in out
          and 'log: preset: high -> medium: SCENE_SHARE 0.75 -> 0.67' in out, 'Ctrl+F5 follows the preset line', out)

    # ---- Ctrl+F5 after the menu switched: the file's line against the one in memory, not the tier at start
    rc, out, f = run(RX6700, ini='preset = low\n', actions=('set=PRESET=medium', 'use=medium', 'save', 'ini=preset = low',
                                                             'reload'))
    check(get(out, 'in_use', 2) == 'medium' and get(out, 'in_use', 5) == 'low'
          and 'log: preset: the settings file now says low' in out, 'Ctrl+F5 after a switch in the menu follows the file',
          out)

    # ---- a tool mode: no probe, the last probe's GPU, no state file written
    rc, out, f = run(RX6700, ini='', state=RTX4060_CACHE, extra={'NX2SARIA_TEST_TOOL': '1'})
    check(rc == 0 and 'probe skipped: a tool mode, not the game itself; the GPU as the last probe saw it' in out
          and 'GPU "NVIDIA GeForce RTX 4060"' in out and 'write:' not in out and state_of(f) == RTX4060_CACHE,
          "a tool mode goes by the state file's GPU and writes nothing", out)
    rc, out, f = run(RX6700, ini='preset = low\n', extra={'NX2SARIA_TEST_TOOL': '1'}, actions=('chosen',))
    check(rc == 0 and 'write:' not in out and not (f / 'preset-state.txt').exists() and get(out, 'in_use') == 'low',
          'a tool mode without a state file: the chosen tier, and still no file', out)

    # ---- a state file of garbage: read without harm, no GPU taken from it
    rc, out, f = run('skip=a test,cores=6,threads=12,ram=32000', ini='',
                     state='garbage\n=\n = x\nchosen_on\ngpu_name =\ngpu_vendor = zz\n\x01\x02\r\n#c = d\n')
    check(rc == 0 and 'GPU none (not probed, and no earlier probe kept)' in out and get(out, 'in_use') == 'high',
          'a state file of garbage', out)
    rc, out, f = run('skip=a test,cores=6,threads=12,ram=32000', ini='', state='gpu_name = Some GPU\ngpu_mesh = 0\n')
    check(rc == 0 and 'GPU none (not probed, and no earlier probe kept)' in out and get(out, 'in_use') == 'high',
          'a kept GPU without its vendor is none', out)

    # ---- the one-time notice: due while the player never chose and this PC gets another tier than high; once
    rc, out, f = run(GTX1650, ini='')
    check(get(out, 'notice_due') == '1', 'a PC that gets low: the notice is due', out)
    rc, out, f = run(GTX1650, folder=f, actions=('notice', 'notice'))
    check('log: preset: notice shown: recommended low in use' in out and out.count('notice shown') == 1
          and get(out, 'notice_due', 1) == '0' and 'noticed = low' in state_of(f)
          and not re.search(r'^write: .*nx2saria\.ini$', out, re.M),
          'shown once: noticed in the state file, nx2saria.ini not written', out + state_of(f))
    rc, out, f = run(GTX1650, folder=f)
    check(get(out, 'notice_due') == '0' and 'noticed = low' in state_of(f), 'not again at the next start', out)
    rc, out, f = run(RTX4060_LOWRAM, folder=f)
    check(get(out, 'notice_due') == '1', 'due again when the recommended tier changes (medium)', out)
    rc, out, f = run(RX6700, ini='')
    check(get(out, 'notice_due') == '0', 'not for a PC that gets high, the game as shipped', out)
    rc, out, f = run(GTX1650, ini='preset = recommended\n')
    check(get(out, 'notice_due') == '0', 'not once the player set the preset, recommended included', out)
    rc, out, f = run(GTX1650, ini='', extra={'NX2SARIA_TEST_TOOL': '1'}, actions=('notice',))
    check(get(out, 'notice_due') == '0' and 'notice shown' not in out, 'not in a tool mode', out)

    # ---- the menu's Apply of a preset choice: defaults of a tier, keys that differ, and the log from the old values
    rc, out, f = run(RX6700, ini='[display]\naniso = 2\n', actions=('handoff=low',))
    check(get(out, 'tier_default low ANISO') == '4' and get(out, 'tier_default medium ANISO') == '16'
          and get(out, 'differs low ANISO') == '1' and get(out, 'differs high ANISO') == '1'
          and get(out, 'differs low ANISO', 1) == '0',
          'cfg_default_tier and cfg_preset_differs', out)
    check('log: preset: high -> low: ANISO 2 -> 4' in out and get(out, 'in_use', 1) == 'low'
          and get(out, 'get ANISO', 1) == '4' and get(out, 'custom', 1) == '0',
          'cfg_preset_use_from logs from the values before the reset', out)

    print(f'preset_check: {"FAILED" if bad else "passed"}')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
