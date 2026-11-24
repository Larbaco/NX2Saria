import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.append(str(ROOT / 'tools'))
from build_game import runtime_include_dirs  # noqa: E402


def flags(rt=None):
    return [a for d in runtime_include_dirs(rt or ROOT / 'runtime') for a in ('-I', str(d))]
