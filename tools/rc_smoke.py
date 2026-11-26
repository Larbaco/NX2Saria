#!/usr/bin/env python3
import argparse
import datetime
import fcntl
import hashlib
import json
import os
import re
import shlex
import shutil
import signal
import subprocess
import sys
import time
import urllib.request
from pathlib import Path

USAGE = """Prove a published release candidate on the Linux bench, unattended: the AppImage of the tag installs into a
throwaway root, the installed launcher builds the game from the bench's dump, the game reaches an area and ends
normally, the launcher's window draws, a forced crash gives a whole FATAL line and a summary with hardware, and nothing
leaves the machine. The full mode also updates from the previous stable release to the tag.

  export NX2SARIA_BENCH=<bench folder> NX2SARIA_DUMP=<the game's .nup or dump folder>
  .venv/bin/python tools/rc_smoke.py v0.27.0-rc.1 --quick --detach
  .venv/bin/python tools/rc_smoke.py v0.27.0-rc.1 --only update --from v0.26.0

Stages, in order: install, build, run, window, crash, update (full mode only), privacy. --only and --skip take a
comma-separated list; a stage reuses what an earlier run left in the throwaway root. Each stage prints one PASS or FAIL
line; the exit code is 1 when any stage failed. Logs, the selftest's screens and summary.md go to
<bench>/xbuild/runs/<date>-rc-smoke-<tag>/. The game folders are deleted before the bench lock is let go (--keep-game keeps
them).
"""

REPO = 'Larbaco/NX2Saria'
GITHUB_API = 'https://api.github.com'
DOWNLOAD = f'https://github.com/{REPO}/releases/download'
LINUX_MANIFEST = 'NX2Saria-update-linux.json'
STAGES = ('install', 'build', 'run', 'window', 'crash', 'update', 'privacy')
QUICK = ('install', 'build', 'run', 'window', 'crash', 'privacy')
BUILD_FREE_GB = 36
ROOT_FREE_GB = 3
SELFTEST_SCREENS = 26
CPUS = '0-11'
MEMORY_MAX = '18G'
CRASH_EXIT = 71
DEFAULT_AREA = 'KokiriForest_Child'
SENT = re.compile(r'telemetry: (?:session report|crash log|comment)\b[^\n]*\bsent\b|\(HTTP \d{3}\b')
FATAL_END = re.compile(r'(?:\)|, (?:reading|writing|executing) address (?:0x[0-9a-f]+|0))$')
AMDGPU_HANG = re.compile(r'amdgpu.*(?:timeout|GPU reset|ring \S+ hung)', re.I)

CRASH_DRIVER = r'''
import json, os, signal, sys, time
app, root, game, wait = sys.argv[1], sys.argv[2], sys.argv[3], float(sys.argv[4])
sys.path.insert(0, app)
from launcher import build, game_session, winsys
from launcher.context import Context
from launcher.paths import Layout
from launcher.platform import game_executable
ctx = Context(Layout(root=root))
exe = game_executable(game)
started = time.time()
proc = winsys.start_game(exe)
run = game_session.GameRun(proc, game, started)
log = build.game_log_path(game)
arrived = False
while time.time() - started < wait and proc.poll() is None:
    try:
        with open(log, encoding='utf-8', errors='replace') as f:
            arrived = 'boot map: arrived in' in f.read()
    except OSError:
        pass
    if arrived:
        break
    time.sleep(1)
killed = False
if proc.poll() is None:
    time.sleep(3 if arrived else 0)
    os.kill(proc.pid, signal.SIGSEGV)
    killed = True
try:
    proc.wait(90)
except Exception:
    proc.kill()
    proc.wait()
end = game_session.finish_game(run, ctx.log)
print('@@' + json.dumps({'arrived': arrived, 'killed': killed, 'code': proc.returncode, 'end': end.log,
                         'kind': run.kind, 'fatal': build.last_fatal(game)}), flush=True)
'''

X_CLOSE = r'''
import ctypes, sys, time
x = ctypes.CDLL('libX11.so.6')
x.XOpenDisplay.restype = ctypes.c_void_p
x.XOpenDisplay.argtypes = [ctypes.c_char_p]
x.XDefaultRootWindow.restype = ctypes.c_ulong
x.XDefaultRootWindow.argtypes = [ctypes.c_void_p]
x.XInternAtom.restype = ctypes.c_ulong
x.XInternAtom.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int]
x.XQueryTree.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.c_ulong),
                         ctypes.POINTER(ctypes.POINTER(ctypes.c_ulong)), ctypes.POINTER(ctypes.c_uint)]
x.XFetchName.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.POINTER(ctypes.c_char_p)]
x.XSendEvent.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int, ctypes.c_long, ctypes.c_void_p]
x.XFlush.argtypes = [ctypes.c_void_p]
x.XFree.argtypes = [ctypes.c_void_p]
class ClientMessage(ctypes.Structure):
    _fields_ = [('type', ctypes.c_int), ('serial', ctypes.c_ulong), ('send_event', ctypes.c_int),
                ('display', ctypes.c_void_p), ('window', ctypes.c_ulong), ('message_type', ctypes.c_ulong),
                ('format', ctypes.c_int), ('data', ctypes.c_long * 5), ('pad', ctypes.c_long * 12)]
d = x.XOpenDisplay(sys.argv[1].encode())
if not d:
    sys.exit('no display')
want = sys.argv[2]
def windows(w):
    root, parent, kids, n = ctypes.c_ulong(), ctypes.c_ulong(), ctypes.POINTER(ctypes.c_ulong)(), ctypes.c_uint()
    if not x.XQueryTree(d, w, ctypes.byref(root), ctypes.byref(parent), ctypes.byref(kids), ctypes.byref(n)):
        return []
    found = [kids[i] for i in range(n.value)]
    if n.value:
        x.XFree(kids)
    return found + [k for c in found for k in windows(c)]
named = []
for w in windows(x.XDefaultRootWindow(d)):
    name = ctypes.c_char_p()
    if x.XFetchName(d, w, ctypes.byref(name)) and name.value:
        if want in name.value.decode('utf-8', 'replace'):
            named.append(w)
if not named:
    sys.exit('no window named ' + want)
protocols = x.XInternAtom(d, b'WM_PROTOCOLS', 0)
delete = x.XInternAtom(d, b'WM_DELETE_WINDOW', 0)
for w in named:
    ev = ClientMessage(type=33, window=w, message_type=protocols, format=32)
    ev.data[0], ev.data[1] = delete, 0
    x.XSendEvent(d, w, 0, 0, ctypes.byref(ev))
x.XFlush(d)
print(len(named))
'''


def tag_version(tag):
    return tag[1:] if tag.startswith('v') else tag


def version_key(value):
    base, _, rc = value.partition('-rc.')
    try:
        stable = tuple(int(p) for p in base.split('.'))
        return (*stable, 0, int(rc)) if rc else (*stable, 1, 0)
    except ValueError:
        return ()


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def sums_for(text, name):
    for line in text.splitlines():
        parts = line.split()
        if len(parts) == 2 and parts[1].lstrip('*') == name and re.fullmatch(r'[0-9a-fA-F]{64}', parts[0]):
            return parts[0].lower()
    return None


def read_expect(path):
    out = {}
    if not path or not Path(path).is_file():
        return out
    for raw in Path(path).read_text(encoding='utf-8').splitlines():
        line = raw.strip()
        if not line or line.startswith('#') or ': ' not in line:
            continue
        key, _, text = line.partition(': ')
        out.setdefault(key.strip(), []).append(text)
    return out


def missing_lines(text, wanted):
    return [w for w in wanted if w not in text]


def sent_lines(text):
    return [ln for ln in text.splitlines() if SENT.search(ln)]


def fatal_lines(text):
    out = []
    for ln in text.splitlines():
        body = ln.split('] ', 1)[1] if ln.startswith('[') and '] ' in ln else ln
        if body.startswith('FATAL: '):
            out.append(body)
    return out


def fatal_problem(line):
    if not line:
        return 'no FATAL line in the log'
    if re.search(r'\[(?:host|launcher)\] ', line[7:]):
        return f'another log line runs into it: {line[:160]}'
    if line.endswith(' [line cut]'):
        return 'it ends with [line cut]'
    if 'access violation' in line and not FATAL_END.search(line):
        return f'it does not end the way a crash line ends: {line[-80:]}'
    return ''


def summary_problems(summary, fatal, with_hardware):
    if not isinstance(summary, dict):
        return ['no logs/session-summary.json']
    bad = []
    if (summary.get('session') or {}).get('end') != 'crash':
        bad.append(f'end is {(summary.get("session") or {}).get("end")!r}, not crash')
    got = summary.get('fatal') or ''
    if not got or not fatal or not fatal.startswith(got.rstrip('…')[:60]):
        bad.append(f'its fatal is {got[:80]!r}, not the FATAL line')
    if with_hardware:
        for key, value in (('cpu.name', (summary.get('cpu') or {}).get('name')),
                           ('gpu.name', (summary.get('gpu') or {}).get('name')),
                           ('ram_mb', summary.get('ram_mb'))):
            if value in (None, ''):
                bad.append(f'{key} is empty')
    return bad


def png_problem(path):
    try:
        size = path.stat().st_size
    except OSError:
        return 'missing'
    if size < 1024:
        return f'{size} bytes'
    try:
        from PIL import Image
    except ImportError:
        return ''
    try:
        with Image.open(path) as im:
            lo_hi = im.convert('RGB').getextrema()
    except Exception as e:
        return f'unreadable: {e}'
    if all(lo == hi for lo, hi in lo_hi):
        return 'one colour only'
    return ''


def free_gb(path):
    try:
        return shutil.disk_usage(path).free / 1e9
    except OSError:
        return 0.0


def http_get(url, dest=None):
    req = urllib.request.Request(url, headers={'User-Agent': 'nx2saria-rc-smoke'})
    with urllib.request.urlopen(req, timeout=120) as r:
        if dest is None:
            return r.read()
        tmp = Path(str(dest) + '.part')
        with open(tmp, 'wb') as f:
            shutil.copyfileobj(r, f, 1 << 20)
        os.replace(tmp, dest)
        return None


def ocarina_running():
    return subprocess.run(['pgrep', '-x', 'ocarina'], capture_output=True).returncode == 0


def kernel_hangs(since):
    try:
        out = subprocess.run(['journalctl', '-k', '--no-pager', '--since', f'@{int(since)}'], capture_output=True,
                             text=True, timeout=60).stdout
    except (OSError, subprocess.TimeoutExpired):
        return []
    return [ln for ln in out.splitlines() if AMDGPU_HANG.search(ln)]


class Failed(Exception):
    pass


class Smoke:
    def __init__(self, a):
        self.a = a
        self.tag = a.tag if a.tag.startswith('v') else 'v' + a.tag
        self.version = tag_version(self.tag)
        self.checkout = Path(__file__).resolve().parents[1]
        self.bench = Path(a.bench)
        self.base = Path(a.root or f'/var/tmp/rc-smoke-{self.tag}')
        self.games = Path(a.games or self.bench / 'xbuild' / 'tmp-rc-smoke' / self.tag)
        day = datetime.date.today().isoformat()
        self.runs = Path(a.logs or self.bench / 'xbuild' / 'runs' / f'{day}-rc-smoke-{self.tag}')
        self.lock_path = self.bench / 'xbuild' / '.build.lock'
        self.results = []
        self.notes = []
        self.expect = read_expect(a.expect_file or self.checkout / 'tools' / 'data' / 'rc-smoke' / f'{self.version}.txt')
        for item in a.expect:
            key, _, text = item.partition(': ')
            self.expect.setdefault(key, []).append(text)
        self.lock = None
        self.feed_used = ''

    def home(self, which):
        return self.base / which / 'home'

    def root(self, which):
        return self.home(which) / '.local' / 'share' / 'nx2saria'

    def game(self, which):
        return self.games / which

    def env(self, which, **extra):
        h = self.home(which)
        e = {k: v for k, v in os.environ.items() if not k.startswith(('XDG_', 'NX2SARIA_', 'NX2NAVI_', 'PYTHON'))}
        e.update(HOME=str(h), XDG_DATA_HOME=str(h / '.local' / 'share'), XDG_CONFIG_HOME=str(h / '.config'),
                 XDG_CACHE_HOME=str(h / '.cache'), XDG_STATE_HOME=str(h / '.local' / 'state'),
                 XDG_RUNTIME_DIR=os.environ.get('XDG_RUNTIME_DIR', ''), TMPDIR=str(self.games / 'tmp'),
                 NX2SARIA_TELEMETRY_URL='none')
        e.update({k: str(v) for k, v in extra.items()})
        return e

    def say(self, text):
        print(text, flush=True)
        with open(self.runs / 'rc_smoke.log', 'a', encoding='utf-8') as f:
            f.write(f'{time.strftime("%H:%M:%S")} {text}\n')

    def record(self, stage, ok, why, seconds):
        self.results.append((stage, ok, why, seconds))
        self.say(f'{"PASS" if ok else "FAIL"} {stage}: {why} ({seconds:.0f} s)')

    def take_lock(self):
        self.lock_path.parent.mkdir(parents=True, exist_ok=True)
        self.lock = open(self.lock_path, 'a')
        try:
            fcntl.flock(self.lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            self.say(f'waiting for {self.lock_path} (another build or run holds it)')
            fcntl.flock(self.lock, fcntl.LOCK_EX)
        self.say(f'bench lock taken: {self.lock_path}')

    def release_lock(self):
        if self.lock:
            fcntl.flock(self.lock, fcntl.LOCK_UN)
            self.lock.close()
            self.lock = None
            self.say('bench lock released')

    def run_logged(self, name, cmd, env, timeout=None, cwd=None):
        log = self.runs / f'{name}.log'
        t0 = time.time()
        with open(log, 'w', encoding='utf-8', errors='replace') as f:
            f.write('$ ' + ' '.join(shlex.quote(str(c)) for c in cmd) + '\n')
            f.flush()
            try:
                p = subprocess.run([str(c) for c in cmd], env=env, cwd=cwd, stdout=f, stderr=subprocess.STDOUT,
                                   stdin=subprocess.DEVNULL, timeout=timeout)
                code = p.returncode
            except subprocess.TimeoutExpired:
                code = 124
                f.write(f'\nrc_smoke: stopped after {timeout} s\n')
            f.write(f'\nrc_smoke: exit {code} after {time.time() - t0:.0f} s\n')
        return code, log.read_text(encoding='utf-8', errors='replace')

    def installed(self, which):
        root = self.root(which)
        try:
            ver = (root / 'app' / 'current.txt').read_text(encoding='utf-8').strip()
        except OSError:
            raise Failed(f'nothing installed in {root}: run the install stage') from None
        return root, ver, root / 'python' / 'bin' / 'python3', root / 'app' / ver

    def cli(self, which, name, args, timeout=None, **extra):
        root, _, py, app = self.installed(which)
        return self.run_logged(name, [py, '-I', app / 'launcher' / 'cli.py', '--root', root, *args],
                               self.env(which, **extra), timeout=timeout)

    def settings(self, which, **values):
        path = self.root(which) / 'data' / 'launcher.json'
        try:
            data = json.loads(path.read_text(encoding='utf-8'))
        except (OSError, ValueError):
            data = {}
        data.update(values)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(data, indent=1) + '\n', encoding='utf-8')

    def releases(self):
        if not hasattr(self, '_releases'):
            self._releases = json.loads(http_get(f'{GITHUB_API}/repos/{REPO}/releases?per_page=30'))
        return self._releases

    def newest_preview(self):
        for r in self.releases():
            if r.get('draft'):
                continue
            if any(a.get('name') == LINUX_MANIFEST for a in r.get('assets', [])):
                return r.get('tag_name')
        return None

    def previous_stable(self):
        mine = version_key(self.version)
        best = None
        for r in self.releases():
            if r.get('draft') or r.get('prerelease'):
                continue
            v = tag_version(r.get('tag_name', ''))
            k = version_key(v)
            if k and k < mine and (best is None or k > version_key(tag_version(best))):
                best = r['tag_name']
        return best

    def install_tag(self, which, tag):
        ver = tag_version(tag)
        dl = self.base / 'downloads'
        dl.mkdir(parents=True, exist_ok=True)
        image = dl / f'NX2Saria-Linux-{ver}-x86_64.AppImage'
        sums = http_get(f'{DOWNLOAD}/{tag}/SHA256SUMS.txt').decode('utf-8', 'replace')
        want = sums_for(sums, image.name)
        if not want:
            raise Failed(f'SHA256SUMS.txt of {tag} has no line for {image.name}')
        if not image.is_file() or sha256(image) != want:
            http_get(f'{DOWNLOAD}/{tag}/{image.name}', image)
        got = sha256(image)
        if got != want:
            raise Failed(f'{image.name}: sha256 {got[:16]}, SHA256SUMS.txt says {want[:16]}')
        image.chmod(0o755)
        unpack = self.base / f'appimage-{ver}'
        shutil.rmtree(unpack, ignore_errors=True)
        unpack.mkdir(parents=True)
        code, out = self.run_logged(f'{which}-extract-{ver}', [image, '--appimage-extract'], self.env(which),
                                    timeout=600, cwd=unpack)
        bundle = unpack / 'squashfs-root'
        if code or not (bundle / 'usr' / 'kit' / 'tools' / 'appimage_install.py').is_file():
            raise Failed(f'the AppImage did not unpack (exit {code})')
        self.home(which).mkdir(parents=True, exist_ok=True)
        script = ('import sys; sys.path.insert(0, sys.argv[1]); import appimage_install as a; '
                  'root, py, entry = a.install(sys.argv[2]); print("@@", root, py, entry)')
        code, out = self.run_logged(f'{which}-appimage-install-{ver}',
                                    [bundle / 'usr' / 'python' / 'bin' / 'python3', '-I', '-c', script,
                                     bundle / 'usr' / 'kit' / 'tools', bundle],
                                    self.env(which, APPIMAGE=image), timeout=900)
        shutil.rmtree(unpack, ignore_errors=True)
        if code:
            raise Failed(f'the AppImage installer of {ver} failed (exit {code}): {out.strip().splitlines()[-1][:200]}')
        root, cur, py, app = self.installed(which)
        bad = []
        if cur != ver:
            bad.append(f'app/current.txt says {cur}')
        if (app / 'VERSION').read_text(encoding='utf-8').strip() != ver:
            bad.append('app VERSION differs')
        for need in (root / 'nx2saria-launcher', py, self.home(which) / '.local' / 'share' / 'applications' /
                     'nx2saria.desktop', root / 'data' / 'appimage-install.json'):
            if not Path(need).exists():
                bad.append(f'{Path(need).name} missing')
        if bad:
            raise Failed(f'{ver} installed incompletely: ' + '; '.join(bad))
        return image, want

    def prepare_settings(self, which, game):
        self.settings(which, channel='preview', telemetry='off', telemetry_asked='', crash_log_offer='off',
                      auto_check=True, auto_install=False, desktop_shortcut=False, sounds='off',
                      dump=str(self.dump()), game_dir=str(game))

    def pin_feed(self, which, tag):
        _, _, _, app = self.installed(which)
        (app / 'feed.json').write_text(json.dumps({'type': 'url', 'url': f'{DOWNLOAD}/{tag}/{LINUX_MANIFEST}'}) + '\n',
                                       encoding='utf-8')

    def dump(self):
        if self.a.dump and Path(self.a.dump).exists():
            return Path(self.a.dump)
        raise Failed(f'no dump at {self.a.dump!r}: give --dump or set NX2SARIA_DUMP')

    def stage_install(self):
        newest = self.newest_preview()
        if self.a.pin_url:
            self.feed_used = f'pinned URL {DOWNLOAD}/{self.tag}/{LINUX_MANIFEST}'
        elif newest != self.tag:
            raise Failed(f'{self.tag} is not the newest release on the Preview channel ({newest} is): '
                         'give --pin-url to test it from its own manifest')
        else:
            self.feed_used = 'the real GitHub feed, Preview channel'
        shutil.rmtree(self.base / 'clean', ignore_errors=True)
        image, sha = self.install_tag('clean', self.tag)
        self.prepare_settings('clean', self.game('clean'))
        if self.a.pin_url:
            self.pin_feed('clean', self.tag)
        code, out = self.cli('clean', 'clean-check', ['check'], timeout=300)
        if code or f'up to date: {self.version}' not in out:
            raise Failed(f'launcher.cli check through {self.feed_used}: {out.strip().splitlines()[-2][:200]}')
        return f'{image.name} sha256 {sha[:12]} matches SHA256SUMS.txt; installed {self.version}; feed: {self.feed_used}'

    def wait_for_disk(self):
        deadline = time.time() + self.a.disk_wait
        while True:
            have = free_gb(self.games.parent if self.games.parent.exists() else self.bench)
            if have >= BUILD_FREE_GB:
                return have
            if time.time() >= deadline:
                raise Failed(f'{have:.0f} GB free on the bench disk, a build needs {BUILD_FREE_GB} GB')
            self.say(f'waiting for disk: {have:.0f} GB free, {BUILD_FREE_GB} GB needed')
            time.sleep(60)

    def build(self, which, name):
        game = self.game(which)
        shutil.rmtree(game, ignore_errors=True)
        game.parent.mkdir(parents=True, exist_ok=True)
        have = self.wait_for_disk()
        t0 = time.time()
        code, out = self.cli(which, name, ['install', '--dump', self.dump(), '--game', game])
        minutes = (time.time() - t0) / 60
        if code:
            err = [ln for ln in out.splitlines() if ln.startswith('error:')]
            raise Failed(f'launcher.cli install failed (exit {code}) after {minutes:.0f} min: '
                         f'{(err or out.strip().splitlines()[-2:])[-1][:200]}')
        state = self.build_state(game)
        _, ver, _, _ = self.installed(which)
        if state.get('kit') != ver:
            raise Failed(f'the build state says kit {state.get("kit")!r}, the launcher is {ver}')
        if not (game / 'ocarina').is_file():
            raise Failed('no ocarina in the game folder')
        return f'{ver} built in {minutes:.0f} min ({have:.0f} GB were free)'

    @staticmethod
    def build_state(game):
        try:
            return json.loads((Path(game) / 'build-cache' / 'state.json').read_text(encoding='utf-8'))
        except (OSError, ValueError):
            return {}

    def stage_build(self):
        return self.build('clean', 'clean-build')

    def bench_profile(self, game):
        src = self.bench / 'profile'
        dest = Path(game) / 'profile'
        if dest.is_dir():
            shutil.rmtree(dest)
        shutil.copytree(src, dest, ignore=lambda d, names: [n for n in names if Path(d) == src
                                                             and n in ('cache', 'temp', 'instance.lock')])
        backups = sorted(p.name for p in (dest / 'save-backup').iterdir() if p.is_dir())
        if not backups:
            raise Failed(f'{src} has no save backup')
        return backups[-1]

    def game_env(self, which, **extra):
        return self.env(which, SDL_VIDEODRIVER='offscreen', SDL_AUDIO_DRIVER='dummy', NX2SARIA_AUDIO='none',
                        NX2SARIA_MESH_FALLBACK='1', **extra)

    def game_run(self, which, name):
        game = self.game(which)
        if not (game / 'ocarina').is_file():
            raise Failed(f'no game in {game}: run the build stage')
        if ocarina_running():
            raise Failed('another ocarina is running (someone may be playing): not run')
        save = self.bench_profile(game)
        ini = game / 'nx2saria.ini'
        before = ini.read_bytes() if ini.is_file() else None
        t0 = time.time()
        log = self.runs / f'{name}-game.log'
        code, out = self.run_logged(name, [sys.executable, self.checkout / 'tools' / 'run_game.py', game,
                                           '--save', save, '--boot-map', self.a.area, '--seconds', self.a.seconds,
                                           '--wall', self.a.wall, '--tag', 'rc-smoke', '--log', log],
                                    self.game_env(which), timeout=self.a.wall + 120)
        time.sleep(3)
        bad = []
        if ocarina_running():
            bad.append('an ocarina process is still running after the run')
        hangs = kernel_hangs(t0)
        if hangs:
            bad.append(f'amdgpu: {hangs[0][:160]}')
        after = ini.read_bytes() if ini.is_file() else None
        if before is not None and after != before:
            bad.append('the run changed nx2saria.ini')
        text = log.read_text(encoding='utf-8', errors='replace') if log.is_file() else ''
        _, ver, _, _ = self.installed(which)
        m = re.search(r'version: NX2Saria (\S+), game (\S+)', text)
        if not m or m.group(1) != ver:
            bad.append(f'version line says {m.group(1) if m else "nothing"}, not {ver}')
        if code:
            bad.append(f'run_game exit {code}')
        if f'boot map: arrived in {self.a.area.split(":")[0]}' not in text:
            bad.append(f'no "boot map: arrived in {self.a.area}"')
        if 'end normal' not in text:
            bad.append('no session summary with "end normal"')
        fatal = fatal_lines(text)
        if fatal:
            bad.append(f'FATAL: {fatal[0][:160]}')
        lost = missing_lines(text, self.expect.get('run', []))
        if lost:
            bad.append('missing promised lines: ' + ' | '.join(lost))
        if bad:
            raise Failed('; '.join(bad))
        arrived = re.search(r'boot map: arrived in [^\n]*', text)
        return (f'{m.group(0)}; {arrived.group(0)[:90]}; end normal; no FATAL; '
                f'{len(self.expect.get("run", []))} promised lines found')

    def stage_run(self):
        return self.game_run('clean', 'clean-run')

    def xvfb(self):
        for n in range(90, 140):
            if not Path(f'/tmp/.X11-unix/X{n}').exists() and not Path(f'/tmp/.X{n}-lock').exists():
                break
        else:
            raise Failed('no free X display number')
        proc = subprocess.Popen(['Xvfb', f':{n}', '-screen', '0', '1920x1080x24', '-nolisten', 'tcp'],
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for _ in range(50):
            if Path(f'/tmp/.X11-unix/X{n}').exists():
                return proc, f':{n}'
            time.sleep(0.1)
        proc.kill()
        raise Failed('Xvfb did not start')

    def stage_window(self):
        root, ver, py, app = self.installed('clean')
        shots = self.runs / 'selftest-shots'
        shutil.rmtree(shots, ignore_errors=True)
        shots.mkdir(parents=True)
        xproc, display = self.xvfb()
        bad, said = [], []
        try:
            env = self.env('clean', DISPLAY=display)
            env.pop('WAYLAND_DISPLAY', None)
            code, out = self.run_logged('window-selftest', [py, '-I', app / 'launcher' / 'start.pyw', '--selftest',
                                                            '--shots', shots], env, timeout=600)
            m = re.search(r'selftest: (\d+) screens drawn', out)
            pngs = sorted(shots.glob('*.png'))
            if code or not m:
                bad.append(f'selftest exit {code}, {m.group(0) if m else "no screens-drawn line"}')
            drawn = int(m.group(1)) if m else 0
            if len(pngs) != drawn or drawn < SELFTEST_SCREENS:
                bad.append(f'{len(pngs)} PNGs for {drawn} screens drawn, at least {SELFTEST_SCREENS} expected')
            for p in pngs:
                why = png_problem(p)
                if why:
                    bad.append(f'{p.name}: {why}')
            said.append(f'selftest drew {m.group(1) if m else "?"} screens, {len(pngs)} PNGs checked')
            log_path = root / 'data' / 'logs' / 'launcher.log'
            start = log_path.stat().st_size if log_path.is_file() else 0
            t0 = time.time()
            proc = subprocess.Popen([root / 'nx2saria-launcher'], env=env, stdout=open(self.runs / 'window.log', 'w'),
                                    stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
            new = ''
            while time.time() - t0 < 120 and proc.poll() is None:
                time.sleep(2)
                with open(log_path, encoding='utf-8', errors='replace') as f:
                    f.seek(start)
                    new = f.read()
                if 'update check:' in new and time.time() - t0 > 15:
                    break
            if proc.poll() is not None:
                bad.append(f'the window ended by itself with exit {proc.returncode}')
            else:
                subprocess.run(['import', '-display', display, '-window', 'root', self.runs / 'window-home.png'],
                               capture_output=True, timeout=60)
                closed = subprocess.run([sys.executable, '-I', '-c', X_CLOSE, display, 'NX2Saria'],
                                        capture_output=True, text=True, timeout=30)
                try:
                    code = proc.wait(60)
                except subprocess.TimeoutExpired:
                    proc.terminate()
                    code = proc.wait(30)
                    bad.append('WM_DELETE_WINDOW did not close it')
                if closed.returncode:
                    bad.append(f'closing it: {closed.stderr.strip()[:120]}')
                if code:
                    bad.append(f'the window exit code was {code}')
            with open(log_path, encoding='utf-8', errors='replace') as f:
                f.seek(start)
                new = f.read()
            (self.runs / 'window-launcher.log').write_text(new, encoding='utf-8')
            head = re.search(r'--- NX2Saria launcher (\S+) \(([^)]*)\)', new)
            if not head or head.group(1) != ver:
                bad.append(f'launcher.log names {head.group(1) if head else "no"} version, not {ver}')
            elif 'AppImage install' not in head.group(2):
                bad.append(f'launcher.log calls the install "{head.group(2)}"')
            end = re.search(r'window closed by the player on the (\S+) screen after \d+ s', new)
            if not end or end.group(1) != 'home':
                bad.append(f'it did not close on the home screen ({end.group(0) if end else "no close line"})')
            if 'first run:' in new:
                bad.append('it showed the first-run screen: the installed game was not found')
            if 'Traceback' in new:
                bad.append('a traceback in launcher.log')
            png = self.runs / 'window-home.png'
            if png.is_file() and png_problem(png):
                bad.append(f'window-home.png: {png_problem(png)}')
            said.append(f'the installed launcher {ver} opened on the home screen and closed cleanly'
                        if not bad else '')
        finally:
            xproc.terminate()
            try:
                xproc.wait(10)
            except subprocess.TimeoutExpired:
                xproc.kill()
        if bad:
            raise Failed('; '.join(bad))
        return '; '.join(s for s in said if s)

    def stage_crash(self):
        root, ver, py, app = self.installed('clean')
        game = self.game('clean')
        if not (game / 'ocarina').is_file():
            raise Failed(f'no game in {game}: run the build stage')
        if ocarina_running():
            raise Failed('another ocarina is running (someone may be playing): not run')
        save = self.bench_profile(game)
        src = game / 'profile' / 'save-backup' / save
        users = [d for d in (game / 'profile' / 'save').iterdir() if d.is_dir()]
        if len(users) == 1:
            shutil.rmtree(users[0])
            shutil.copytree(src, users[0])
        shutil.rmtree(game / 'profile' / 'save-tx', ignore_errors=True)
        for f in ('session-summary.json', 'session-summary.sent.json'):
            (game / 'logs' / f).unlink(missing_ok=True)
        self.settings('clean', telemetry='off', crash_log_offer='off')
        t0 = time.time()
        code, out = self.run_logged('crash', [py, '-I', '-c', CRASH_DRIVER, app, root, game, self.a.wall],
                                    self.game_env('clean', NX2SARIA_TELEMETRY_URL='http://127.0.0.1:9',
                                                  NX2SARIA_BOOT_MAP=self.a.area, NX2SARIA_ASSUME_FOCUS='1',
                                                  NX2SARIA_INPUT='none'),
                                    timeout=self.a.wall + 240)
        time.sleep(3)
        for f in ('nx2saria.log', 'session-summary.json', 'session-start.json'):
            if (game / 'logs' / f).is_file():
                shutil.copy2(game / 'logs' / f, self.runs / f'crash-{f}')
        bad = []
        if ocarina_running():
            bad.append('an ocarina process is still running after the crash')
        hangs = kernel_hangs(t0)
        if hangs:
            bad.append(f'amdgpu: {hangs[0][:160]}')
        m = re.search(r'^@@(.*)$', out, re.M)
        res = json.loads(m.group(1)) if m else {}
        if code or not res:
            raise Failed(f'the crash driver exit {code}: {out.strip().splitlines()[-1][:200]}')
        if not res.get('arrived'):
            bad.append('the game had not reached the area when it was crashed')
        if res.get('code') != CRASH_EXIT:
            bad.append(f'exit code {res.get("code")}, expected {CRASH_EXIT}')
        text = (game / 'logs' / 'nx2saria.log').read_text(encoding='utf-8', errors='replace')
        lines = fatal_lines(text)
        raw = lines[-1] if lines else ''
        why = fatal_problem(raw)
        if why:
            bad.append(f'FATAL line: {why}')
        if res.get('fatal') and raw and 'FATAL: ' + res['fatal'] != raw:
            bad.append('the launcher read a different FATAL line from the log')
        try:
            summary = json.loads((game / 'logs' / 'session-summary.json').read_text(encoding='utf-8'))
        except (OSError, ValueError):
            summary = None
        hardware = version_key(ver) >= version_key('0.27.0-rc.1')
        bad += summary_problems(summary, raw[7:], hardware)
        launcher_log = (root / 'data' / 'logs' / 'launcher.log').read_text(encoding='utf-8', errors='replace')
        made = re.findall(r'telemetry: the last run ended in a crash; a summary was made[^\n]*', launcher_log)
        if not made:
            bad.append('launcher.log has no "a summary was made from its FATAL line"')
        lost = missing_lines(launcher_log, self.expect.get('crash-launcher', []))
        if lost:
            bad.append('missing promised lines: ' + ' | '.join(lost))
        if bad:
            raise Failed('; '.join(bad))
        return (f'exit {res.get("code")}; {raw[:110]}; summary end crash, cpu/gpu/ram filled'
                f'{"" if hardware else " (not checked before 0.27.0)"}; launcher: summary made '
                f'{made[-1].split("FATAL line, ", 1)[-1]}')

    def stage_update(self):
        stable = self.a.from_tag or self.previous_stable()
        if not stable:
            raise Failed('no earlier stable release found: give --from')
        old = tag_version(stable)
        shutil.rmtree(self.base / 'update', ignore_errors=True)
        self.install_tag('update', stable)
        self.prepare_settings('update', self.game('update'))
        built = self.build('update', 'update-build-old')
        game = self.game('update')
        before = self.build_state(game)
        if self.a.pin_url:
            self.pin_feed('update', self.tag)
        t0 = time.time()
        code, out = self.cli('update', 'update', ['update'])
        minutes = (time.time() - t0) / 60
        root, cur, py, app = self.installed('update')
        bad = []
        if code:
            err = [ln for ln in out.splitlines() if ln.startswith('error:')]
            bad.append(f'launcher.cli update exit {code}: {(err or out.strip().splitlines()[-1:])[-1][:200]}')
        if cur != self.version:
            bad.append(f'current.txt says {cur}, not {self.version}')
        prev = (root / 'app' / 'previous.txt').read_text(encoding='utf-8').strip() \
            if (root / 'app' / 'previous.txt').is_file() else ''
        if prev != old or not (root / 'app' / old / 'launcher' / 'start.pyw').is_file():
            bad.append(f'rollback not kept: previous.txt {prev!r}, app/{old} '
                       f'{"present" if (root / "app" / old).is_dir() else "gone"}')
        after = self.build_state(game)
        if after.get('kit') != self.version:
            bad.append(f'the game build says kit {after.get("kit")!r} after the update')
        lost = missing_lines(out, self.expect.get('update', []))
        if lost:
            bad.append('the update did not do what the notes say: ' + ' | '.join(lost))
        steps = [ln for ln in out.splitlines() if re.match(r'^(Translate the Code|Compile Changed Files|Swap In|'
                                                           r'Prepare the Shaders|Update Build Tools): ', ln)]
        last = {}
        for ln in steps:
            k, _, v = ln.partition(': ')
            last[k] = v
        if bad:
            raise Failed('; '.join(bad))
        ran = self.game_run('update', 'update-run')
        return (f'{old} ({built}) updated to {self.version} in {minutes:.0f} min; previous.txt {prev}; '
                + ', '.join(f'{k}: {v}' for k, v in last.items()) + f'; {ran.split(";")[0]}')

    def stage_privacy(self):
        found, read = [], 0
        for which in ('clean', 'update'):
            logs = self.root(which) / 'data' / 'logs'
            for name in ('launcher.log', 'launcher.1.log'):
                p = logs / name
                if p.is_file():
                    read += 1
                    found += [f'{which}/{name}: {ln}' for ln in sent_lines(p.read_text(encoding='utf-8',
                                                                                     errors='replace'))]
        if not read:
            raise Failed('no launcher.log to read')
        if found:
            (self.runs / 'privacy.txt').write_text('\n'.join(found) + '\n', encoding='utf-8')
            raise Failed(f'{len(found)} lines show something sent: {found[0][:160]}')
        return f'{read} launcher logs, no session report, crash log or comment sent'

    def cleanup(self):
        shutil.rmtree(self.games / 'tmp', ignore_errors=True)
        if not self.a.keep_game:
            for which in ('clean', 'update'):
                g = self.game(which)
                if g.exists():
                    shutil.rmtree(g, ignore_errors=True)
                    self.say(f'deleted {g}')
            try:
                self.games.rmdir()
            except OSError:
                pass
        if self.a.clean:
            shutil.rmtree(self.base, ignore_errors=True)
            self.say(f'deleted {self.base}')

    def summary(self, stages, started):
        ok = all(r[1] for r in self.results)
        lines = [f'# Release candidate smoke test: {self.tag}', '',
                 f'{"PASS" if ok else "FAIL"}: {sum(r[1] for r in self.results)} of {len(self.results)} stages passed '
                 f'in {(time.time() - started) / 60:.0f} min on {datetime.datetime.now():%Y-%m-%d %H:%M}.', '',
                 f'- Feed: {self.feed_used or "not checked in this run"}',
                 f'- Mode: {"quick" if tuple(stages) == QUICK else "stages " + ", ".join(stages)}', '',
                 '| Stage | Result | What it showed |', '|---|---|---|']
        for stage, good, why, seconds in self.results:
            cell = why.replace('|', '/').replace('\n', ' ')
            lines.append(f'| {stage} | {"PASS" if good else "FAIL"} ({seconds / 60:.0f} min) | {cell} |')
        lines += ['', f'Logs: `{self.runs}`.']
        if self.notes:
            lines += [''] + [f'- {n}' for n in self.notes]
        (self.runs / 'summary.md').write_text('\n'.join(lines) + '\n', encoding='utf-8')
        return ok

    def main(self, stages):
        self.runs.mkdir(parents=True, exist_ok=True)
        started = time.time()
        self.say(f'rc smoke test of {self.tag}: stages {", ".join(stages)}; launcher root {self.base}; '
                 f'game folders {self.games}; logs {self.runs}')
        self.base.mkdir(parents=True, exist_ok=True)
        (self.games / 'tmp').mkdir(parents=True, exist_ok=True)
        if free_gb(self.base) < ROOT_FREE_GB:
            self.record('setup', False, f'{free_gb(self.base):.1f} GB free under {self.base}, {ROOT_FREE_GB} GB '
                        'needed', 0)
            self.summary(stages, started)
            return 1
        self.take_lock()
        try:
            for stage in stages:
                t0 = time.time()
                self.say(f'--- {stage}')
                try:
                    why = getattr(self, f'stage_{stage}')()
                    self.record(stage, True, why, time.time() - t0)
                except Failed as e:
                    self.record(stage, False, str(e), time.time() - t0)
                except Exception as e:
                    self.record(stage, False, f'{type(e).__name__}: {e}', time.time() - t0)
                    import traceback
                    with open(self.runs / 'rc_smoke.log', 'a', encoding='utf-8') as f:
                        traceback.print_exc(file=f)
        finally:
            self.cleanup()
            self.release_lock()
        ok = self.summary(stages, started)
        self.say(f'{"PASS" if ok else "FAIL"}: {self.runs / "summary.md"}')
        return 0 if ok else 1


def chosen(a):
    stages = list(QUICK if a.quick else STAGES)
    if a.only:
        want = [s.strip() for s in a.only.split(',') if s.strip()]
        unknown = [s for s in want if s not in STAGES]
        if unknown:
            sys.exit(f'rc_smoke: unknown stage {", ".join(unknown)} (stages: {", ".join(STAGES)})')
        stages = [s for s in STAGES if s in want]
    if a.skip:
        drop = {s.strip() for s in a.skip.split(',')}
        stages = [s for s in stages if s not in drop]
    return stages


def detach(argv, a):
    day = datetime.date.today().isoformat()
    tag = a.tag if a.tag.startswith('v') else 'v' + a.tag
    runs = Path(a.logs or Path(a.bench) / 'xbuild' / 'runs' / f'{day}-rc-smoke-{tag}')
    runs.mkdir(parents=True, exist_ok=True)
    out = runs / 'rc_smoke.out'
    rest = [x for x in argv if x != '--detach']
    cmd = ['systemd-run', '--user', '--scope', '--quiet', '-p', f'MemoryMax={MEMORY_MAX}', '-p', 'OOMPolicy=continue',
           'taskset', '-c', CPUS, 'nice', '-n', '10', 'ionice', '-c', '2', '-n', '7',
           sys.executable, str(Path(__file__).resolve()), *rest]
    with open(out, 'a') as f:
        p = subprocess.Popen(['nohup', *cmd], stdout=f, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                             start_new_session=True)
    print(f'rc_smoke: running as pid {p.pid} in its own scope (MemoryMax={MEMORY_MAX}, CPUs {CPUS}); '
          f'follow {out}; the result lands in {runs / "summary.md"}')
    return 0


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    ap = argparse.ArgumentParser(description=USAGE, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('tag')
    ap.add_argument('--quick', action='store_true')
    ap.add_argument('--only')
    ap.add_argument('--skip')
    ap.add_argument('--from', dest='from_tag')
    ap.add_argument('--pin-url', action='store_true')
    ap.add_argument('--bench', default=os.environ.get('NX2SARIA_BENCH', ''))
    ap.add_argument('--dump', default=os.environ.get('NX2SARIA_DUMP', ''))
    ap.add_argument('--area', default=DEFAULT_AREA)
    ap.add_argument('--seconds', type=float, default=40)
    ap.add_argument('--wall', type=float, default=300)
    ap.add_argument('--expect', action='append', default=[])
    ap.add_argument('--expect-file')
    ap.add_argument('--root')
    ap.add_argument('--games')
    ap.add_argument('--logs')
    ap.add_argument('--disk-wait', type=float, default=1800)
    ap.add_argument('--keep-game', action='store_true')
    ap.add_argument('--clean', action='store_true')
    ap.add_argument('--detach', action='store_true')
    a = ap.parse_args(argv)
    stages = chosen(a)
    if not a.bench or not (Path(a.bench) / 'xbuild').is_dir():
        sys.exit('rc_smoke: no bench: give --bench or set NX2SARIA_BENCH to the folder that holds xbuild/ and profile/')
    if a.detach:
        return detach(argv, a)
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(143))
    return Smoke(a).main(stages)


if __name__ == '__main__':
    sys.exit(main())
