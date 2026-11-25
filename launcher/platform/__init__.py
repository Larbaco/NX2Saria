import os
from pathlib import Path


def game_executable(game_dir):
    return Path(game_dir) / ('ocarina.exe' if os.name == 'nt' else 'ocarina')
