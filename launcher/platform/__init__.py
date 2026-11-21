"""Operating-system services used by the launcher.

The existing launcher.winsys module remains the stable call surface while platform-specific
implementations move behind it in small, reviewable steps.
"""
import os
from pathlib import Path


def game_executable(game_dir):
    """The built game's executable in its selected game folder."""
    return Path(game_dir) / ('ocarina.exe' if os.name == 'nt' else 'ocarina')
