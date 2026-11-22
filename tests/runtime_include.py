"""The include flags of the runtime's sources for the tests that compile them: runtime/ and its folders by subsystem
(tools/build_game.py RUNTIME_DIRS), as the build puts them on the path, since the sources include one another's headers
by name. Not a test itself."""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.append(str(ROOT / 'tools'))
from build_game import runtime_include_dirs  # noqa: E402


def flags(rt=None):
    """['-I', <rt>, '-I', <rt>/audio, ...]: rt is a runtime/ folder, this checkout's by default."""
    return [a for d in runtime_include_dirs(rt or ROOT / 'runtime') for a in ('-I', str(d))]
