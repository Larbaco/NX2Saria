# needs: clang
"""Build and run the mesh pipeline key reporting guard without a GPU or game data."""
import argparse
import os
import shutil
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MSYS_BIN = Path(r'C:\msys64\ucrt64\bin')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--work', type=Path, default=ROOT / 'build' / 'test' / 'mesh-pipeline-check')
    a = ap.parse_args()
    a.work = a.work.resolve()
    a.work.mkdir(parents=True, exist_ok=True)
    cc = os.environ.get('CC') or (str(MSYS_BIN / 'clang.exe') if os.name == 'nt' and (MSYS_BIN / 'clang.exe').is_file()
                                  else shutil.which('clang') or 'clang')
    exe = a.work / ('mesh_pipeline_check.exe' if os.name == 'nt' else 'mesh_pipeline_check')
    env = dict(os.environ)
    if os.name == 'nt' and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    subprocess.run([cc, '-std=gnu11', '-O1', '-Wall', '-Werror', str(ROOT / 'tests' / 'mesh_pipeline_check.c'),
                    '-o', str(exe)], check=True, env=env)
    return subprocess.run([str(exe)], env=env).returncode


if __name__ == '__main__':
    raise SystemExit(main())
