"""data/launcher.json: the player's choices and what the launcher remembers between runs."""
import json
import os
import shutil
from pathlib import Path

DEFAULTS = {
    'game_dir': '',               # the built game; empty until the first install picks one
    'dump': '',                   # the .nup (or the folder with exefs/ and romfs/) the game was built from
    'auto_check': True,           # look for a new version when the launcher opens, and every hour while it runs
    'auto_install': True,         # install a downloaded update by itself whenever the game is not running
    'channel': 'stable',          # 'stable' or 'preview' (GitHub pre-releases)
    'hide_while_playing': True,   # hide the window while the game runs, show it again when it exits
    'warn_low_commit': True,      # warn before Play only with a disabled or fixed-size Windows page file
    'keep_cache': True,           # keep build-cache/ (the lift and objects) so an update recompiles only changes
    'desktop_shortcut': True,
    'save_export_dir': '',        # where Saves > Export Save writes its zip; empty: the Downloads folder
    'export_with_log': True,      # Export Save adds the game's latest log and settings file (for a bug report)
    'theme': 'forest',            # the colour scheme (launcher/ui/art.py THEMES)
    'custom_background': '',      # the Custom scheme's backdrop colour, #rrggbb; empty: the first scheme's
    'custom_panel': '',           # the Custom scheme's colour of the panels and the title band
    'custom_buttons': '',         # the Custom scheme's colour of the buttons
    'background': '',             # the picture the player chose as the background; its fitted copy is data/background.npy
    'background_dim': 40,         # how much that picture is darkened, in percent
    'animations': True,           # the cursor glides, screens slide in, sparks drift, and the leaf sprig bobs
    'sounds': 'low',              # the game's select and back sounds: off, low (sound.py DEFAULT_LEVEL), medium, high
    'feed_repo': '',              # owner/name overriding feed.json, for testing a fork's releases
    'github_token': '',           # only for a private release repository; stored as plain text
    'last_check': 0.0,
    'last_seen': '',              # the newest version the player was told about
    'rolled_back': '',            # the last version that failed to start; it is never downloaded or installed by itself
    # session reports (launcher/telemetry.py, docs/telemetry.md): '' not asked yet, 'on' or 'off'; nothing is sent
    # unless 'on'
    'telemetry': '',
    'telemetry_closed': '',       # the launcher version the server told it takes no more reports (HTTP 410)
    'telemetry_asked': '',        # the launcher version whose question about session reports was answered or closed
    'telemetry_notice': '',       # '1': reports were turned on by the question's countdown or by closing it; told once
    'crash_log_offer': 'on',
}

# Keys whose changes save() does not log: bookkeeping of the update check rather than a choice of the player.
UNLOGGED = ('last_check', 'last_seen')
# Keys whose values save() never writes into the log, only whether they are set: players attach the log to public issues.
SECRET = ('github_token',)


def _shown(key, value):
    """A setting's value as save() writes it into the log."""
    if key in SECRET:
        return 'set' if value else 'empty'
    if isinstance(value, bool):
        return 'on' if value else 'off'
    return str(value) if value != '' else "''"


class Settings(dict):
    def __init__(self, path, log=None):
        super().__init__(DEFAULTS)
        self.path = Path(path)
        self.log = log   # save() writes a line here for each setting that changed since the last save
        self.unreadable = False   # the file is there but is no JSON; a copy of it is kept as launcher.json.bad
        try:
            # utf-8-sig: the docs ask players to edit the file by hand, and an editor may save it with a BOM
            data = json.loads(self.path.read_text(encoding='utf-8-sig'))
            if isinstance(data, dict):
                self.update({k: v for k, v in data.items() if k in DEFAULTS and type(v) is type(DEFAULTS[k])})
        except OSError:
            pass
        except ValueError:
            # a typo made by hand: keep what the player wrote before the next save() replaces it with the defaults
            self.unreadable = True
            try:
                shutil.copyfile(self.path, self.path.with_name(self.path.name + '.bad'))
            except OSError:
                pass
        self.saved = dict(self)   # what launcher.json holds, as far as this process knows

    def changes(self):
        """'setting: <key> <before> -> <after>' for each setting that differs from the last save (or from what was read
        at the start), in the order of DEFAULTS."""
        return [f'setting: {k} {_shown(k, self.saved.get(k))} -> {_shown(k, self.get(k))}'
                for k in DEFAULTS if k not in UNLOGGED and self.get(k) != self.saved.get(k)]

    def save(self):
        self.path.parent.mkdir(parents=True, exist_ok=True)
        tmp = self.path.with_name(self.path.name + '.tmp')
        tmp.write_text(json.dumps(dict(self), indent=1), encoding='utf-8')
        os.replace(tmp, self.path)
        lines, self.saved = self.changes(), dict(self)
        if self.log:
            for line in lines:
                self.log(line)

    def reset(self):
        """Default Settings: the choices go back to their defaults; the game folder, dump and history stay."""
        keep = {k: self[k] for k in ('game_dir', 'dump', 'last_check', 'last_seen', 'feed_repo', 'github_token',
                                     'rolled_back', 'telemetry', 'telemetry_closed', 'telemetry_asked',
                                     'telemetry_notice', 'crash_log_offer')}
        self.clear()
        self.update(DEFAULTS)
        self.update(keep)
