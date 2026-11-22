# needs: clang
"""Compile and run the aspect, scene, heap and Vert+ formulas without game data."""
import os
import shutil
import subprocess
from pathlib import Path
import runtime_include

ROOT = Path(__file__).resolve().parents[1]
compiler = os.environ.get('CC') or shutil.which('clang') or shutil.which('cc')
if not compiler:
    raise SystemExit('widescreen_math_check: no C compiler found')
work = ROOT / 'build' / 'test' / 'widescreen_math'
work.mkdir(parents=True, exist_ok=True)
exe = work / 'widescreen_math_check'
subprocess.check_call([compiler, '-std=c11', '-Wall', '-Wextra', *runtime_include.flags(),
                       str(ROOT / 'tests' / 'widescreen_math_check.c'), '-lm', '-o', str(exe)])
subprocess.check_call([str(exe)])
