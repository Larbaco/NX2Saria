import os
import shutil
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / 'tools'))
import private_paths  # noqa: E402
from private_paths import Shortener  # noqa: E402

WIN = {'USERPROFILE': r'C:\Users\Ana Souza', 'LOCALAPPDATA': r'C:\Users\Ana Souza\AppData\Local',
       'APPDATA': r'C:\Users\Ana Souza\AppData\Roaming', 'HOMEDRIVE': 'C:', 'HOMEPATH': r'\Users\Ana Souza',
       'HOMESHARE': r'\\fs01\perfis\asouza', 'TEMP': r'C:\Users\ANASOU~1\AppData\Local\Temp',
       'USERNAME': 'Ana Souza'}


def nothing(_p):
    return ''


class WindowsTest(unittest.TestCase):
    def setUp(self):
        self.s = Shortener(WIN, windows=True, short=nothing)

    def test_folders(self):
        s = self.s
        self.assertEqual(s(r'libs: C:\Users\Ana Souza\AppData\Local\Programs\NX2Saria\game\SDL3.dll'),
                         r'libs: %LOCALAPPDATA%\Programs\NX2Saria\game\SDL3.dll')
        self.assertEqual(s('c:/users/ana souza/appdata/roaming/x'), '%APPDATA%/x')
        self.assertEqual(s(r'C:\Users\Ana Souza\Downloads\a.zip'), r'%USERPROFILE%\Downloads\a.zip')
        self.assertEqual(s(r'C:\Users\ANASOU~1\AppData\Local\Temp\x.tmp'), r'%TEMP%\x.tmp')
        self.assertEqual(s(r'\\?\C:\Users\Ana Souza\x'), r'\\?\%USERPROFILE%\x')
        self.assertEqual(s(r'\\fs01\perfis\asouza\Documents'), r'%HOMESHARE%\Documents')

    def test_left_alone(self):
        s = self.s
        for text in (r'C:\Users\Ana Souza2\x', 'player Ana Souza pressed F8', 'build 0.18.4: 12 s',
                     r'C:\Program Files\NX2Saria'):
            self.assertEqual(s(text), text)

    def test_escaped_and_punctuation(self):
        s = self.s
        self.assertEqual(s("No such file: 'C:\\\\Users\\\\Ana Souza\\\\AppData\\\\Local\\\\x'"),
                         "No such file: '%LOCALAPPDATA%\\\\x'")
        self.assertEqual(s(repr(r'C:\Users\Ana Souza\Games')), repr(r'%USERPROFILE%\Games'))
        self.assertEqual(s('saved in C:\\Users\\Ana Souza.'), 'saved in %USERPROFILE%.')
        self.assertEqual(s('C:\\Users\\Ana Souza.CORP\\x'), 'C:\\Users\\%USERNAME%.CORP\\x')

    def test_name_in_a_path(self):
        self.assertEqual(self.s(r'D:\Jogos\Ana Souza\oot'), r'D:\Jogos\%USERNAME%\oot')

    def test_computer_and_domain(self):
        s = Shortener({**WIN, 'COMPUTERNAME': 'ANA-LAPTOP', 'USERDOMAIN': 'ANA-LAPTOP'}, windows=True, short=nothing)
        self.assertEqual(s(r'\\ana-laptop\jogos\oot and ANA-LAPTOP\Ana Souza'),
                         r'\\%COMPUTERNAME%\jogos\oot and %COMPUTERNAME%\%USERNAME%')
        s = Shortener({**WIN, 'COMPUTERNAME': 'ANA-LAPTOP', 'USERDOMAIN': 'SOUZACORP'}, windows=True, short=nothing)
        self.assertEqual(s(r'SOUZACORP\asouza'), r'%USERDOMAIN%\asouza')

    def test_wine(self):
        s = Shortener({'USERPROFILE': r'C:\users\steamuser', 'HOME': '/home/deck', 'USERNAME': 'steamuser'},
                      windows=True, short=nothing)
        self.assertEqual(s(r'Z:\home\deck\Games\oot\x and /home/deck/y'), r'~\Games\oot\x and ~/y')
        self.assertEqual(s('Steam Deck'), 'Steam Deck')


class LinuxTest(unittest.TestCase):
    def test_home_and_link(self):
        s = Shortener({'HOME': '/home/joão silva/', 'USER': 'joão silva'}, windows=False,
                      real=lambda p: '/var/home/joão silva')
        self.assertEqual(s('read /home/joão silva/Games/oot/nx2saria.ini'), 'read ~/Games/oot/nx2saria.ini')
        self.assertEqual(s('/var/home/joão silva/x'), '~/x')
        self.assertEqual(s('/home/JOÃO SILVA/x'), '/home/JOÃO SILVA/x')
        self.assertEqual(s('/home/joão silvas/x'), '/home/joão silvas/x')
        self.assertEqual(s('/mnt/home/joão silva/x'), '/mnt/home/$USER/x')

    def test_short_name(self):
        s = Shortener({'HOME': '/home/ana', 'USER': 'ana'}, windows=False, real=lambda p: p)
        self.assertEqual(s('/srv/banana/anabel/ana_x/x.ana/y'), '/srv/banana/anabel/ana_x/x.$USER/y')
        self.assertEqual(s('/srv/anaé/x'), '/srv/anaé/x')
        self.assertEqual(s('/tmp/claude-1000/-home-ana--t3/x'), '/tmp/claude-1000/-home-$USER--t3/x')
        self.assertEqual(s('ana pressed F8 / again'), 'ana pressed F8 / again')
        self.assertEqual(s('/home/ana/.local and /home/anabel'), '~/.local and /home/anabel')

    def test_common_name(self):
        s = Shortener({'HOME': '/home/user', 'USER': 'user'}, windows=False, real=lambda p: p)
        self.assertEqual(s('/home/user/x /run/user/1000'), '~/x /run/user/1000')

    def test_long_text(self):
        import time
        s = Shortener({'HOME': '/home/ana', 'USER': 'ana'}, windows=False, real=lambda p: p)
        text = ('x' * 100 + ' /run/media/ana/disk\n') * 20000
        t0 = time.monotonic()
        out = s(text)
        self.assertLess(time.monotonic() - t0, 5)
        self.assertEqual(out.count('/run/media/$USER/disk'), 20000)

    def test_computer_name(self):
        s = Shortener({'HOME': '/home/ana', 'USER': 'ana', 'HOSTNAME': 'ana-laptop.lan'}, windows=False,
                      real=lambda p: p, host=lambda: 'maria-pc')
        self.assertEqual(s('smb://MARIA-PC/jogos, ana-laptop.lan and ana-laptop answered'),
                         'smb://%COMPUTERNAME%/jogos, %COMPUTERNAME% and %COMPUTERNAME% answered')
        self.assertEqual(s('maria-pcs xmaria-pc'), 'maria-pcs xmaria-pc')
        s = Shortener({'HOME': '/home/deck', 'USER': 'deck'}, windows=False, real=lambda p: p, host=lambda: 'steamdeck')
        self.assertEqual(s('steamdeck: Steam Deck'), 'steamdeck: Steam Deck')

    def test_refused(self):
        s = Shortener({'HOME': '/', 'USER': 'ab'}, windows=False, real=lambda p: p)
        self.assertEqual(s('/x/ab/y'), '/x/ab/y')
        self.assertEqual(s.prefixes, [])


class SwitchTest(unittest.TestCase):
    def test_off(self):
        home = os.path.expanduser('~')
        text = os.path.join(home, 'x')
        with mock.patch.dict(os.environ, {private_paths.SWITCH: '0'}):
            self.assertEqual(private_paths.shorten(text), text)
            self.assertIn('in full', private_paths.describe())
        with mock.patch.dict(os.environ, {private_paths.SWITCH: '1'}):
            if len(home) > 3:
                self.assertNotIn(home, private_paths.shorten(text))
            self.assertIn('written short', private_paths.describe())


@unittest.skipIf(os.name == 'nt', 'a made-up HOME: Linux')
class LauncherTest(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix='nx2saria-pp-'))
        self.addCleanup(shutil.rmtree, self.tmp, True)
        env = {k: v for k, v in os.environ.items() if not k.startswith(('NX2SARIA_', 'NX2NAVI_'))}
        env.update(HOME=str(self.tmp / 'home' / 'zé'), USER='zé')
        (self.tmp / 'home' / 'zé').mkdir(parents=True)
        patch = mock.patch.dict(os.environ, env, clear=True)
        patch.start()
        self.addCleanup(patch.stop)
        private_paths._default = None
        self.addCleanup(setattr, private_paths, '_default', None)
        self.home = str(self.tmp / 'home' / 'zé')

    def test_launcher_log(self):
        from launcher.context import Context
        from launcher.paths import Layout
        layout = Layout(root=Path(self.home) / 'NX2Saria', app=ROOT).ensure()
        ctx = Context(layout)
        self.addCleanup(ctx._file.close)
        ctx.log(f'play: {self.home}/NX2Saria/game/nx2saria')
        text = (layout.logs / 'launcher.log').read_text(encoding='utf-8')
        self.assertNotIn(self.home, text)
        self.assertIn('play: ~/NX2Saria/game/nx2saria', text)
        self.assertIn('launcher', text)
        self.assertIn('personal folders are written short', text)

    def test_report_zip(self):
        from launcher import saves
        game = Path(self.home) / 'game'
        (game / 'logs').mkdir(parents=True)
        (game / 'logs' / 'nx2saria.log').write_text(f'[host] log: {self.home}/game/logs/nx2saria.log\n', encoding='utf-8')
        (game / 'nx2saria.ini').write_text(f'[mods]\nmods_dir = {self.home}/mods\n', encoding='utf-8')
        z = zipfile.ZipFile(self.tmp / 'r.zip', 'w')
        for f, name in saves._report_files(game):
            saves._add_report_file(z, f, name)
        z.close()
        z = zipfile.ZipFile(self.tmp / 'r.zip')
        log = z.read(saves.REPORT_DIR + 'nx2saria.log').decode()
        ini = z.read(saves.REPORT_DIR + 'nx2saria.ini').decode()
        self.assertEqual(log, '[host] log: ~/game/logs/nx2saria.log\n')
        self.assertEqual(ini, '[mods]\nmods_dir = ~/mods\n')


if __name__ == '__main__':
    unittest.main(verbosity=2)
