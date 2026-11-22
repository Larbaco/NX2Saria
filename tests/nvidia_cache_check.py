# needs: clang
"""Build and run tests/nvidia_cache_check.c: runtime/nvidia_cache.h, what the game puts into the environment for NVIDIA's
shader disk cache (a folder beside the exe, a limit of 8 GiB and, on Windows, the cleanup off; the player's own values
stand), how it counts the folder and the lines the logs say about both.

What the runtime does with the header (vk_device.c nvidia_cache_beside_exe, vk_pipeline.c lw_say_cache_folder, the
setting's rows in cfg.c) is found in the sources here, so a change of that wiring shows in this check even where the
Windows-only code does not run.

Needs a C compiler only: no driver, no GPU and no game files.

Usage: nvidia_cache_check.py
"""
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MSYS_BIN = Path(r'C:\msys64\ucrt64\bin')


def compiler():
    """$CC, else MSYS2 UCRT64's clang, else clang on PATH (as tests/diff/common.py picks it, without its Unicorn)."""
    if os.environ.get('CC'):
        return os.environ['CC']
    if os.name == 'nt' and (MSYS_BIN / 'clang.exe').exists():
        return str(MSYS_BIN / 'clang.exe')
    return shutil.which('clang') or 'clang'


def read(rel):
    return (ROOT / rel).read_text(encoding='utf-8', errors='replace')


def squeeze(src):
    return re.sub(r'"\s*\n\s*"', '', src)   # adjacent string literals as one


# (file, what it must hold, a pattern)
WIRING = [
    ('runtime/vk_device.c', 'vk_driver_cache_env calls nvidia_cache_beside_exe on Windows and Linux',
     r'mesa_submit_thread\(\);\s*#if defined\(_WIN32\) \|\| defined\(__linux__\)\s*nvidia_cache_beside_exe\(\);\s*#endif'),
    ('runtime/vk_device.c', 'the setting is read', r'\.enabled = cfg_bool\("NX2SARIA_NVIDIA_CACHE_DIR"\)'),
    ('runtime/vk_device.c', 'Linux asks for NVIDIA\'s kernel driver as before',
     r'plat_fs_kind\("/proc/driver/nvidia/version"\) == PLAT_FS_FILE'),
    ('runtime/vk_device.c', 'Windows asks for NVIDIA\'s user-mode files',
     r'"%s\\\\System32\\\\nvapi64\.dll"'),
    ('runtime/vk_device.c', 'the three variables come from the header',
     r'plat_getenv\(NVC_PATH_VAR.*plat_getenv\(NVC_SIZE_VAR.*plat_getenv\(NVC_SKIP_VAR'),
    ('runtime/vk_device.c', 'the plan decides',
     r'NvcPlan plan = nvc_plan\(&in\);.*nvc_off_line\(.*nvc_stays_line\(.*nvc_use_line\('),
    ('runtime/vk_device.c', 'the folder is made and tested only when the plan needs it',
     r'if \(nvc_need_folder\(&in\)\) \{'),
    ('runtime/vk_device.c', 'what a restart inherits is the game\'s own, not the player\'s',
     r'plat_getenv\(NVC_MARK_VAR, mark, sizeof mark\) \? nvc_mark_parse\(mark\) : 0;.*'
     r'in\.path_set = plat_getenv\(NVC_PATH_VAR, dir, sizeof dir\) != 0 && !\(ours & NVC_SET_PATH\);.*'
     r'in\.size_set = .*!\(ours & NVC_SET_SIZE\);.*in\.skip_set = .*!\(ours & NVC_SET_SKIP\);'),
    ('runtime/vk_device.c', 'what the game set before and does not set now is taken out',
     r'int stale = nvc_stale\(&plan, ours\);.*if \(stale & NVC_SET_PATH\) plat_setenv\(NVC_PATH_VAR, NULL\);.*'
     r'if \(stale & NVC_SET_SIZE\) plat_setenv\(NVC_SIZE_VAR, NULL\);.*if \(stale & NVC_SET_SKIP\) plat_setenv\(NVC_SKIP_VAR, NULL\);.*'
     r'if \(ours && plan\.verdict != NVC_USE\) plat_setenv\(NVC_MARK_VAR, NULL\);'),
    ('runtime/vk_device.c', 'the game names what it set',
     r'if \(plan\.set & ~refused\) plat_setenv\(NVC_MARK_VAR, mark\);'),
    ('runtime/vk_device.c', 'a variable the environment refuses is said, not claimed',
     r'if \(refused\) \{\s*nvc_refused_line\(line, sizeof line, refused\);\s*rt_log\("%s", line\);\s*return;\s*\}\s*nvc_use_line\('),
    ('runtime/vk_device.c', 'the variables are set from the plan',
     r'plan\.set & NVC_SET_PATH\) && !plat_setenv\(NVC_PATH_VAR.*plan\.set & NVC_SET_SIZE\).*plan\.set & NVC_SET_SKIP\)'),
    ('runtime/vk_pipeline.c', 'the folder line comes before the sample',
     r'static void lw_check\(void\) \{.*?lw_say_cache_folder\(\);.*?lw_probe\('),
    ('runtime/vk_pipeline.c', 'the folder line says what the folder holds',
     r'rt_log\("vk_pipeline: library warm: %s", text\)'),
    ('runtime/vk_pipeline.c', 'the count asks the platform\'s listing', r'nvc_measure\(plat_fs_list, dir, work, sizeof work, &s\)'),
    ('runtime/cfg.c', 'a known name', r'"NVIDIA_CACHE_DIR",\s*//'),
    ('runtime/cfg.c', 'a row in the settings table', r'\{"NVIDIA_CACHE_DIR", "1", "0\|1", RESTART\}'),
    ('runtime/cfg.c', 'a section', r'\{"NVIDIA_CACHE_DIR", "performance"\}'),
    ('nx2saria.ini.example', 'the setting is documented', r'(?m)^# nvidia_cache_dir = 1$'),
]


def wiring():
    """0 when the runtime applies the header where it says."""
    bad = 0
    for rel, what, pattern in WIRING:
        if not re.search(pattern, squeeze(read(rel)), re.S):
            print('nvidia_cache_check: FAIL: %s does not hold: %s' % (rel, what))
            bad = 1
    return bad


def main():
    work = ROOT / 'build' / 'test' / 'nvidia_cache'
    work.mkdir(parents=True, exist_ok=True)
    exe = work / ('nvidia_cache_check.exe' if os.name == 'nt' else 'nvidia_cache_check')
    env = dict(os.environ)
    if os.name == 'nt' and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    subprocess.check_call([compiler(), '-std=gnu11', '-O1', '-Wall', '-Wextra', '-Werror', '-I', str(ROOT / 'runtime'),
                           str(ROOT / 'tests' / 'nvidia_cache_check.c'), '-o', str(exe)], env=env)
    return subprocess.run([str(exe)], env=env).returncode | wiring()


if __name__ == '__main__':
    sys.exit(main())
