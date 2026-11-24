# needs: display, clang, py:numpy, py:lz4, py:zstandard, py:PIL, py:fontTools, py:zbic, py:tkinter
# needs: linux:clang++, linux:ld.lld, linux:pkg:opus, linux:pkg:vpx, linux:pkg:dav1d, linux:pkg:libzstd, linux:pkg:vulkan
import contextlib
import gc
import hashlib
import http.server
import io
import json
import os
import shutil
import ssl
import stat
import struct
import subprocess
import sys
import tarfile
import tempfile
import threading
import time
import unittest
import zipfile
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / 'tools'))
os.environ['NX2SARIA_TELEMETRY_URL'] = 'none'

from launcher import build, commit, feed, gameini, gamefiles, jobs, locks, net, saves, settings, toolchain, updater, versions, winsys  # noqa: E402
from launcher.context import Context, shorten  # noqa: E402
from launcher.paths import Layout  # noqa: E402
from launcher.platform import game_executable  # noqa: E402
import runtime_include  # noqa: E402

GAME_EXE = game_executable('.').name
GAME_LOG = 'logs\\nx2saria.log' if os.name == 'nt' else 'logs/nx2saria.log'

try:
    from compression import zstd as ZSTD
except ImportError:
    ZSTD = None


class Server:
    def __init__(self, folder, routes=None, cert=None):
        folder, routes = Path(folder), routes if routes is not None else {}
        self.hits = []
        server = self

        class H(http.server.BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def do_HEAD(self):
                server.hits.append((self.path, dict(self.headers)))
                self.send_response(301)
                self.send_header('Location', 'https://127.0.0.1' + self.path)
                self.send_header('Content-Length', '0')
                self.end_headers()

            def do_GET(self):
                server.hits.append((self.path, dict(self.headers)))
                if self.path in routes:
                    status, headers, body = routes[self.path]
                    self.send_response(status)
                    for k, v in headers.items():
                        self.send_header(k, v)
                    if 'Content-Length' not in headers:
                        self.send_header('Content-Length', str(len(body)))
                    self.end_headers()
                    self.wfile.write(body)
                    return
                p = folder / self.path.lstrip('/').split('?')[0]
                if not p.is_file():
                    self.send_error(404)
                    return
                data = p.read_bytes()
                rng = self.headers.get('Range')
                if rng and rng.startswith('bytes='):
                    start = int(rng[6:].split('-')[0])
                    if start >= len(data):
                        self.send_error(416)
                        return
                    self.send_response(206)
                    self.send_header('Content-Range', f'bytes {start}-{len(data) - 1}/{len(data)}')
                    data = data[start:]
                else:
                    self.send_response(200)
                self.send_header('Content-Length', str(len(data)))
                self.end_headers()
                try:
                    self.wfile.write(data)
                except ConnectionError:
                    pass

        self.httpd = http.server.ThreadingHTTPServer(('127.0.0.1', 0), H)
        self.url = f'http://127.0.0.1:{self.httpd.server_address[1]}/'
        if cert:
            ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            ctx.load_cert_chain(cert)
            self.httpd.socket = ctx.wrap_socket(self.httpd.socket, server_side=True)
            self.url = 'https' + self.url[4:]
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()

    def close(self):
        self.httpd.shutdown()
        self.httpd.server_close()


def sha(data):
    return hashlib.sha256(data).hexdigest()


def make_cert(folder, start, end):
    folder = Path(folder)
    folder.mkdir(parents=True)
    (folder / 'index.txt').write_text('')
    (folder / 'serial.txt').write_text('01\n')
    (folder / 'ca.cnf').write_text('[ca]\ndefault_ca = d\n[d]\ndatabase = index.txt\nserial = serial.txt\n'
                                   'new_certs_dir = .\ndefault_md = sha256\npolicy = p\ncopy_extensions = copy\n'
                                   'unique_subject = no\n[p]\ncommonName = supplied\n')
    def stamp(t):
        return time.strftime('%Y%m%d%H%M%SZ', time.gmtime(t))

    def run(*args):
        subprocess.run(['openssl', *args], cwd=folder, check=True, capture_output=True)

    run('req', '-new', '-newkey', 'ec', '-pkeyopt', 'ec_paramgen_curve:P-256', '-nodes', '-keyout', 'key.pem',
        '-out', 'req.csr', '-subj', '/CN=127.0.0.1', '-addext', 'subjectAltName=IP:127.0.0.1')
    run('ca', '-batch', '-config', 'ca.cnf', '-selfsign', '-keyfile', 'key.pem', '-in', 'req.csr', '-out', 'cert.pem',
        '-startdate', stamp(start), '-enddate', stamp(end), '-notext')
    pem = folder / 'both.pem'
    pem.write_bytes((folder / 'cert.pem').read_bytes() + (folder / 'key.pem').read_bytes())
    return pem


class CountingContext(ssl.SSLContext):
    def wrap_socket(self, *args, **kwargs):
        self.handshakes = getattr(self, 'handshakes', 0) + 1
        return super().wrap_socket(*args, **kwargs)


def client_context(trust=None):
    ctx = CountingContext(ssl.PROTOCOL_TLS_CLIENT)
    if trust:
        ctx.load_verify_locations(cafile=trust)
    else:
        ctx.load_default_certs()
    return ctx


def tar_bytes(files, compress=True):
    buf = io.BytesIO()
    mode = 'w:gz' if not (compress and ZSTD) else 'w'
    with tarfile.open(fileobj=buf, mode=mode) as t:
        for name, data in files.items():
            info = tarfile.TarInfo(name)
            info.size = len(data)
            t.addfile(info, io.BytesIO(data))
    data = buf.getvalue()
    return ZSTD.compress(data) if compress and ZSTD else data


def desc(name, version, filename, data, depends=(), provides=()):
    lines = ['%FILENAME%', filename, '', '%NAME%', name, '', '%VERSION%', version, '', '%CSIZE%', str(len(data)), '',
             '%ISIZE%', str(len(data) * 3), '', '%SHA256SUM%', sha(data), '']
    if depends:
        lines += ['%DEPENDS%', *depends, '']
    if provides:
        lines += ['%PROVIDES%', *provides, '']
    return '\n'.join(lines).encode()


def fake_repo(folder, damage=None):
    P = toolchain.PREFIX
    files = {'clang': {'ucrt64/bin/clang.exe': b'clang', 'ucrt64/bin/clang++.exe': b'clang++'},
             'sdl3': {'ucrt64/bin/SDL3.dll': b'sdl', 'ucrt64/lib/libSDL3.dll.a': b'implib'},
             'vulkan-memory-allocator': {'ucrt64/include/vma/vk_mem_alloc.h': b'// vma'},
             'vulkan-loader': {'ucrt64/lib/libvulkan-1.dll.a': b'implib'},
             'llvm-libs': {'ucrt64/bin/libLLVM.dll': b'llvm', 'ucrt64/share/doc/llvm/README': b'skipped'}}
    deps = {'clang': [P + 'llvm-libs=23.1', P + 'gcc'], 'sdl3': [P + 'vulkan']}
    entries = {}
    for root in list(toolchain.ROOTS) + ['llvm-libs']:
        name = P + root
        fn = f'{name}-1.0-1-any.pkg.tar.zst'
        data = tar_bytes({'.PKGINFO': b'pkg', **files.get(root, {f'ucrt64/share/{root}/x': b'x'})})
        if damage == root:
            (folder / fn).write_bytes(data[:-3] + b'bad')
        else:
            (folder / fn).write_bytes(data)
        entries[f'{name}-1.0-1/desc'] = desc(name, '1.0-1', fn, data, deps.get(root, ()),
                                             [P + 'vulkan'] if root == 'vulkan-loader' else ())
    (folder / 'ucrt64.db').write_bytes(tar_bytes(entries))


def compiled_sleeper(exe, ms, code):
    cc = next((c for c in (r'C:\msys64\ucrt64\bin\clang.exe', shutil.which('clang')) if c and Path(c).is_file()),
              None)
    if not cc:
        return False
    exe = Path(exe)
    exe.parent.mkdir(parents=True, exist_ok=True)
    src = exe.with_suffix('.c')
    src.write_text(f'#include <windows.h>\nint main(void) {{ Sleep({ms}); return {code}; }}\n', encoding='utf-8')
    subprocess.run([cc, '-O1', '-o', str(exe), str(src)], check=True, capture_output=True, timeout=300)
    return True


def fake_fast_repo(folder, clang_version='1.0-1'):
    folder = folder / 'clang64'
    folder.mkdir()
    P = toolchain.FAST_PREFIX
    files = {'clang': {'clang64/bin/clang.exe': b'clang64', 'clang64/bin/clang-repl.exe': b'not needed',
                       'clang64/lib/clang/23/include/immintrin.h': b'// x86'},
             'llvm-libs': {'clang64/bin/libLLVM-23.dll': b'llvm', 'clang64/share/doc/x': b'skipped'}}
    entries = {}
    for root in toolchain.FAST_PACKAGES:
        name, ver = P + root, clang_version if root == 'clang' else '1.0-1'
        fn = f'{name}-{ver}-any.pkg.tar.zst'
        data = tar_bytes({'.PKGINFO': b'pkg', **files.get(root, {f'clang64/bin/{root}.dll': b'dll'})})
        (folder / fn).write_bytes(data)
        entries[f'{name}-{ver}/desc'] = desc(name, ver, fn, data, (), ())
    (folder / 'clang64.db').write_bytes(tar_bytes(entries))


class Tmp(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix='nx2navi-test-'))
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.layout = Layout(root=self.tmp / 'launcher', app=ROOT).ensure()


class VersionTest(unittest.TestCase):
    def test_order(self):
        self.assertTrue(versions.newer('0.3.11', '0.3.10'))
        self.assertTrue(versions.newer('0.4', '0.3.99'))
        self.assertFalse(versions.newer('0.3.10+abc', '0.3.10'))
        self.assertTrue(versions.newer('1.0.0', '1.0.0-rc1'))
        self.assertTrue(versions.newer('0.19.0-rc.10', '0.19.0-rc.9'))
        self.assertTrue(versions.newer('0.19.0-rc.2', '0.19.0-rc.1'))
        self.assertTrue(versions.newer('0.19.0', '0.19.0-rc.12'))
        self.assertTrue(versions.newer('0.19.0-rc.1', '0.18.7'))
        self.assertTrue(versions.newer('0.19.1-rc.1', '0.19.0'))
        self.assertTrue(versions.newer('0.19.0-rc.1.1', '0.19.0-rc.1'))
        self.assertTrue(versions.newer('0.19.0-rc', '0.19.0-1'))
        self.assertFalse(versions.newer('0.19.0-rc.1+abc', '0.19.0-rc.1'))
        self.assertEqual(versions.display('0.19.0-rc.1+abc1234'), '0.19.0-rc.1')
        self.assertEqual(versions.display('0.3.11+abc1234'), '0.3.11')
        with self.assertRaises(ValueError):
            versions.parse('banana')


PLAYER_INI = (
    b'# my settings\r\n'
    b'[display]\r\n'
    b'async_compute = 1\r\n'
    b'fullscreen = 1\r\n'
    b'window_size = 1920x1080\r\n'
    b'gpu_index = 0\r\n'
    b'perf_overlay = fps\r\n'
    b'vk_present = mailbox\r\n'
    b'upscaler = bilinear\r\n'
    b'dlaa = 1\r\n'
    b'framegen = off\r\n'
    b'taa = fsr3\r\n'
    b'hdr = 0\r\n'
    b'aspect = fit\r\n'
    b'game_res = 1800\r\n'
    b'sharpness = 80\r\n'
    b'aniso = 2\r\n'
    b'assume_focus = 1\r\n'
    b'texture_detail = auto\r\n'
    b'\r\n'
    b'[input]\r\n'
    b'bind_a = key:default, pad:south\r\n'
    b'bind_b = key:default, pad:east\r\n'
    b'bind_x = key:default, pad:north\r\n'
    b'bind_y = key:default, pad:west\r\n'
    b'\r\n'
    b'[language]\r\n'
    b'operation_mode = docked\r\n'
    b'\r\n'
    b'[performance]\r\n'
    b'wb_lazy = 0\r\n'
    b'vram_safe_mode = 1\r\n'
    b'exception_handler = 1\r\n'
    b'\r\n'
    b'[frame rate]\r\n'
    b'fps_limit = 120')


class GameIniTest(Tmp):
    def test_new_file_gets_the_section(self):
        gameini.put(self.tmp, 'discord', 'discord_rpc', '1')
        self.assertEqual((self.tmp / 'nx2saria.ini').read_bytes(), b'[discord]\ndiscord_rpc = 1\n')
        self.assertEqual(gameini.get(self.tmp, 'discord_rpc'), '1')

    def test_new_ini_wins_and_legacy_ini_is_left_untouched(self):
        current, legacy = self.tmp / 'nx2saria.ini', self.tmp / 'nx2navi.ini'
        current.write_text('sharpness = 80\n')
        legacy.write_text('sharpness = 20\n')
        self.assertEqual(gameini.get(self.tmp, 'sharpness'), '80')
        gameini.put(self.tmp, 'display', 'sharpness', '60')
        self.assertEqual(current.read_text(), 'sharpness = 60\n')
        self.assertEqual(legacy.read_text(), 'sharpness = 20\n')

    def test_only_the_key_line_changes(self):
        ini = self.tmp / 'nx2saria.ini'
        ini.write_bytes(b'\xef\xbb\xbf[display]\r\nfullscreen = 1\r\n# caf\xe9\r\n# discord_rpc = 1\r\n[discord]\r\n'
                        b'NX2SARIA_DISCORD_RPC = "1"\r\ndiscord_app_id = 42')
        self.assertEqual(gameini.get(self.tmp, 'DISCORD_RPC'), '1')
        gameini.put(self.tmp, 'discord', 'discord_rpc', '0')
        self.assertEqual(ini.read_bytes(), b'\xef\xbb\xbf[display]\r\nfullscreen = 1\r\n# caf\xe9\r\n# discord_rpc = 1\r\n'
                         b'[discord]\r\ndiscord_rpc = 0\r\ndiscord_app_id = 42')
        self.assertEqual(gameini.get(self.tmp, 'discord_rpc'), '0')
        self.assertFalse((self.tmp / 'nx2saria.ini.tmp').exists())

    def test_missing_key_goes_under_its_section(self):
        ini = self.tmp / 'nx2saria.ini'
        ini.write_bytes(b'[discord]\ndiscord_app_id = 42\n[display]\nfullscreen = 1')
        self.assertEqual(gameini.get(self.tmp, 'discord_rpc'), '')
        gameini.put(self.tmp, 'discord', 'discord_rpc', '1')
        self.assertEqual(ini.read_bytes(), b'[discord]\ndiscord_rpc = 1\ndiscord_app_id = 42\n[display]\nfullscreen = 1')

    def test_an_empty_line_leaves_the_earlier_value(self):
        ini = self.tmp / 'nx2saria.ini'
        ini.write_bytes(b'discord_rpc = 0\ndiscord_rpc =\n')
        self.assertEqual(gameini.get(self.tmp, 'discord_rpc'), '0')
        gameini.put(self.tmp, 'discord', 'discord_rpc', '1')
        self.assertEqual(ini.read_bytes(), b'discord_rpc = 1\ndiscord_rpc =\n')

    def test_an_unreadable_file_is_not_rewritten(self):
        (self.tmp / 'nx2navi.ini').mkdir()
        with self.assertRaises(OSError):
            gameini.get(self.tmp, 'discord_rpc')
        with self.assertRaises(OSError):
            gameini.put(self.tmp, 'discord', 'discord_rpc', '1')
        self.assertTrue((self.tmp / 'nx2navi.ini').is_dir())

class PlayerIniTest(Tmp):

    def test_the_launchers_setting_adds_one_line(self):
        ini = self.tmp / 'nx2saria.ini'
        ini.write_bytes(PLAYER_INI)
        self.assertEqual(gameini.get(self.tmp, 'wb_lazy'), '0')
        self.assertEqual(gameini.get(self.tmp, 'async_compute'), '1')
        gameini.put(self.tmp, 'discord', 'discord_rpc', '0')
        self.assertEqual(ini.read_bytes(), PLAYER_INI + b'\r\n\r\n[discord]\r\ndiscord_rpc = 0\r\n')
        gameini.put(self.tmp, 'discord', 'discord_rpc', '1')
        self.assertEqual(ini.read_bytes(), PLAYER_INI + b'\r\n\r\n[discord]\r\ndiscord_rpc = 1\r\n')

    def test_a_comment_after_a_value_as_the_game_reads_it(self):
        ini = self.tmp / 'nx2saria.ini'
        body = (b'[frame rate]\r\npace_smooth = 0 ; off\r\nfps_limit = 144 # my monitor\r\nvk_present = fifo\t; x\r\n'
                b'aspect = ; later\r\ndlss_preset = "k ; quoted"\r\n[mods]\r\nmods_dir = D:\\My Mods #2\r\n'
                b'mods_disabled = a #1|b ; c\r\n')
        ini.write_bytes(body)
        for key, want in (('pace_smooth', '0'), ('fps_limit', '144'), ('vk_present', 'fifo'), ('aspect', ''),
                          ('dlss_preset', 'k ; quoted')):
            self.assertEqual(gameini.get(self.tmp, key, choices=True), want, key)
        self.assertEqual(gameini.get(self.tmp, 'fps_limit'), '144 # my monitor')
        self.assertEqual(gameini.get(self.tmp, 'mods_dir'), 'D:\\My Mods #2')
        self.assertEqual(gameini.get(self.tmp, 'mods_disabled'), 'a #1|b ; c')
        from launcher import mods
        self.assertEqual(mods._names(self.tmp, 'MODS_DISABLED'), ['a #1', 'b ; c'])
        ini.write_bytes(b'vk_present = fifo\r\nvk_present = ; later\r\n')
        gameini.put(self.tmp, 'display', 'vk_present', 'mailbox', choices=True)
        self.assertEqual(ini.read_bytes(), b'vk_present = mailbox\r\nvk_present = ; later\r\n')
        ini.write_bytes(b'pace_smooth = 0 ; off\r\n')
        gameini.put(self.tmp, 'performance', 'pace_smooth', '1', choices=True)
        self.assertEqual(ini.read_bytes(), b'pace_smooth = 1\r\n')

class RebuildArgsTest(Tmp):
    def test_retune_only_for_rebuild_the_game(self):
        self.assertIn('--retune', build.rebuild_args(self.layout, 'G:/game', True))
        self.assertNotIn('--retune', build.rebuild_args(self.layout, 'G:/game'))
        self.assertTrue(jobs.RebuildJob.retune)
        self.assertFalse(jobs.ContinueUpdateJob.retune)


class SettingsTest(Tmp):
    def test_roundtrip_and_reset(self):
        s = settings.Settings(self.layout.settings)
        self.assertTrue(s['auto_check'])
        s['auto_check'], s['game_dir'], s['channel'] = False, 'D:/g', 'preview'
        s.save()
        t = settings.Settings(self.layout.settings)
        self.assertEqual((t['auto_check'], t['game_dir'], t['channel']), (False, 'D:/g', 'preview'))
        t.reset()
        self.assertEqual((t['auto_check'], t['game_dir'], t['channel']), (True, 'D:/g', 'stable'))

    def test_defaults(self):
        chosen = ('auto_check', 'hide_while_playing', 'keep_cache', 'channel')
        want = (True, True, True, 'stable')
        s = settings.Settings(self.layout.settings)
        self.assertEqual(tuple(s[k] for k in chosen), want)
        s.update(auto_check=False, hide_while_playing=False, keep_cache=False, channel='preview')
        s.save()
        t = settings.Settings(self.layout.settings)
        self.assertEqual(tuple(t[k] for k in chosen), (False, False, False, 'preview'))
        t.reset()
        self.assertEqual(tuple(t[k] for k in chosen), want)

    def test_bad_types_ignored(self):
        self.layout.settings.write_text(json.dumps({'auto_check': 'yes', 'channel': 'preview', 'junk': 1}))
        s = settings.Settings(self.layout.settings)
        self.assertEqual((s['auto_check'], s['channel']), (True, 'preview'))
        self.assertNotIn('junk', s)

    def test_bom_and_typo(self):
        self.layout.settings.write_bytes(b'\xef\xbb\xbf' + json.dumps({'game_dir': 'D:/g'}).encode())
        s = settings.Settings(self.layout.settings)
        self.assertEqual(s['game_dir'], 'D:/g')
        self.assertFalse(s.unreadable)
        typo = '{"game_dir": "D:/g", "github_token": "t",}'
        self.layout.settings.write_text(typo, encoding='utf-8')
        s = settings.Settings(self.layout.settings)
        self.assertTrue(s.unreadable)
        self.assertEqual(s['game_dir'], '')
        s.save()
        self.assertEqual(self.layout.settings.with_name('launcher.json.bad').read_text(encoding='utf-8'), typo)


class SettingChangesTest(Tmp):
    def test_each_changed_setting_is_logged_once(self):
        lines = []
        s = settings.Settings(self.layout.settings, log=lines.append)
        s['channel'], s['auto_check'], s['last_check'], s['github_token'] = 'preview', False, 123.0, 'ghp_secret'
        s.save()
        self.assertEqual(lines, ['setting: auto_check on -> off', 'setting: channel stable -> preview',
                                 'setting: github_token empty -> set'])
        s.save()
        self.assertEqual(len(lines), 3)
        s['dump'] = '/home/ana/Downloads/game.nup'
        s.save()
        self.assertEqual(lines[3:], ["setting: dump '' -> /home/ana/Downloads/game.nup"])
        t = settings.Settings(self.layout.settings, log=lines.append)
        t['channel'] = 'stable'
        t.save()
        self.assertEqual(lines[4:], ['setting: channel preview -> stable'])

    def test_the_context_logs_the_changes(self):
        ctx = Context(self.layout)
        self.addCleanup(ctx._file.close)
        ctx.settings['sounds'] = 'off'
        ctx.settings.save()
        self.assertIn('setting: sounds low -> off', ''.join(ctx.log_tail()))


class UninstallTest(Tmp):
    def tree(self, root, rels):
        for rel in rels:
            p = root / rel
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text('x')

    def test_plan_keeps_the_game_and_what_is_not_ours(self):
        from types import SimpleNamespace
        root = self.tmp / 'NX2Saria'
        self.tree(root, ('NX2Saria.exe', 'app/current.txt', 'python.old-3/x', 'game/profile/save/1/a.sav', 'mine.txt'))
        layout = SimpleNamespace(root=root, toolchain=root / 'toolchain')
        gone, empty = winsys.uninstall_plan(layout)
        self.assertNotIn(root / 'game', gone)
        self.assertNotIn(root, gone)
        self.assertIn(root / 'python.old-3', gone)
        self.assertEqual(empty, [root])
        gone, _ = winsys.uninstall_plan(layout, str(root / 'game'))
        self.assertEqual(gone[0], root / 'game')
        other = self.tmp / 'C NX2Saria' / 'toolchain'
        gone, empty = winsys.uninstall_plan(SimpleNamespace(root=root, toolchain=other))
        self.assertIn(other, gone)
        self.assertNotIn(other.parent, gone)
        self.assertEqual(empty, [root, other.parent])

    @unittest.skipUnless(os.name == 'nt', 'runs the cmd.exe script')
    def test_script_on_windows(self):
        from types import SimpleNamespace
        root = self.tmp / 'NX2Saria ÿ x'
        self.tree(root, ('NX2Saria.exe', 'app/1/VERSION', 'python/pythonw.exe', 'python.old-12/x', 'pydeps/z',
                         'data/launcher.json', 'toolchain/msys64/a', 'game/profile/save/1/a.sav', 'mine.txt'))
        layout = SimpleNamespace(root=root, toolchain=root / 'toolchain')
        script = winsys.uninstall_script(*winsys.uninstall_plan(layout)).replace('ping -n 3 127.0.0.1 >nul', 'echo')
        subprocess.run(f'cmd.exe /c "{script}"', capture_output=True)
        self.assertEqual(sorted(p.name for p in root.iterdir()), ['game', 'mine.txt'])
        self.assertTrue((root / 'game/profile/save/1/a.sav').is_file())
        (root / 'mine.txt').unlink()
        script = winsys.uninstall_script(*winsys.uninstall_plan(layout, str(root / 'game')))
        subprocess.run(f'cmd.exe /c "{script.replace("ping -n 3 127.0.0.1 >nul", "echo")}"', capture_output=True)
        self.assertFalse(root.exists())

    def test_script_leaves_out_percent_paths(self):
        script = winsys.uninstall_script([Path('C:/%HOMEPATH%'), Path('C:/x/a')], [Path('C:/%TEMP%/b')])
        self.assertNotIn('%', script)
        self.assertIn('a', script)


@unittest.skipUnless(sys.platform.startswith('linux'), 'the AppImage install and sh')
class LinuxUninstallTest(Tmp):
    def install(self, name="NX2Saria it's $mine 100%"):
        home = self.tmp / 'home'
        root = home / '.local' / 'share' / name
        for rel in ('nx2saria-launcher', 'nx2saria.png', 'app/current.txt', 'app/0.21.1/VERSION',
                    'python/bin/python3', 'python.old/bin/python3', 'pydeps/zbic/__init__.py', 'toolchain/x',
                    'data/launcher.json', 'data/appimage-install.json', 'data/launcher.lock', 'data/cache/fonts/a',
                    'data/downloads/a.part', 'data/logs/launcher.log', 'game/ocarina', 'game/saves/a.zip',
                    'game/profile/save/1/save.dat', 'game/logs/nx2saria.log', 'mine.txt'):
            p = root / rel
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text('x')
        apps = home / '.local' / 'share' / 'applications'
        apps.mkdir(parents=True)
        entry, other = apps / 'nx2saria.desktop', apps / 'other' / 'nx2saria.desktop'
        from appimage_gui import exec_quote
        entry.write_text(f'[Desktop Entry]\nType=Application\nName=NX2Saria\nExec={exec_quote(root / "nx2saria-launcher")}\n')
        other.parent.mkdir()
        other.write_text('[Desktop Entry]\nExec=/opt/elsewhere/nx2saria-launcher\n')
        return root, home, entry, other

    def left(self, root):
        return sorted(p.relative_to(root).as_posix() for p in root.rglob('*'))

    def test_the_plan_keeps_the_game_the_logs_and_what_is_not_ours(self):
        from launcher.platform import linux
        root, home, entry, other = self.install()
        gone, empty, kept = linux.uninstall_plan(root, entry=entry, home=home)
        self.assertEqual(empty, [root / 'data', root])
        self.assertEqual(kept, [(root / 'data' / 'logs', "the launcher's logs")])
        self.assertIn(entry.with_name('nx2saria.desktop.new'), gone)
        for rel in ('nx2saria-launcher', 'nx2saria.png', 'app', 'python', 'python.old', 'pydeps', 'toolchain',
                    'data/launcher.json', 'data/appimage-install.json', 'data/cache', 'data/downloads'):
            self.assertIn(root / rel, gone)
        self.assertIn(entry, gone)
        for p in (root / 'game', root / 'data' / 'logs', root / 'mine.txt', root, other):
            self.assertNotIn(p, gone)
        self.assertNotIn(other, linux.uninstall_plan(root, entry=other, home=home)[0])
        gone, _, kept = linux.uninstall_plan(root, str(root / 'game'), entry=entry, home=home)
        self.assertEqual(gone[0], root / 'game')
        for folder, why in ((root, "it holds the launcher's folder"), (root.parent, "it holds the launcher's folder"),
                            (home, "it holds the launcher's folder"), (Path('/'), "it holds the launcher's folder")):
            gone, _, kept = linux.uninstall_plan(root, str(folder), entry=entry, home=home)
            self.assertNotIn(folder, gone)
            self.assertEqual(kept[0], (folder, f'the game folder, as {why}'))
        elsewhere, homes = self.tmp / 'xdg' / 'nx2saria', self.tmp / 'homes'
        elsewhere.mkdir(parents=True)
        (homes / 'ana').mkdir(parents=True)
        gone, _, kept = linux.uninstall_plan(elsewhere, str(homes), entry=entry, home=homes / 'ana')
        self.assertNotIn(homes, gone)
        self.assertEqual(kept[0], (homes, 'the game folder, as it holds the home folder'))
        link = self.tmp / 'link'
        link.symlink_to(root.parent)
        self.assertNotIn(link, linux.uninstall_plan(root, str(link), entry=entry, home=home)[0])

    def test_what_the_player_keeps_stays_whatever_holds_it(self):
        from launcher.platform import linux
        root, home, entry, _ = self.install()
        (root / 'data' / 'game').mkdir()
        gone, _, kept = linux.uninstall_plan(root, keep=[root / 'data' / 'game', root / 'toolchain' / 'x'],
                                             entry=entry, home=home)
        self.assertNotIn(root / 'data' / 'game', gone)
        self.assertNotIn(root / 'toolchain', gone)
        self.assertIn((root / 'toolchain', f'it holds {root / "toolchain" / "x"}'), kept)
        self.assertIn(root / 'data' / 'cache', gone)
        dump = root / 'game' / 'OoT.nup'
        gone, _, kept = linux.uninstall_plan(root, str(root / 'game'), keep=[dump], entry=entry, home=home)
        self.assertNotIn(root / 'game', gone)
        self.assertIn((root / 'game', f'it holds {dump}'), kept)

    def test_the_script_waits_for_the_launcher_then_deletes(self):
        from launcher.platform import linux
        root, home, entry, other = self.install()
        gone, empty, _ = linux.uninstall_plan(root, entry=entry, home=home)
        launcher = subprocess.Popen(['sleep', '1'])
        waiter = threading.Thread(target=launcher.wait)
        waiter.start()
        started = time.monotonic()
        subprocess.run(linux.uninstall_command(launcher.pid, gone, empty, root / 'data' / 'logs' / 'launcher.log'),
                       check=True, timeout=60)
        waiter.join()
        self.assertGreater(time.monotonic() - started, 0.8)
        self.assertEqual(self.left(root), ['data', 'data/logs', 'data/logs/launcher.log', 'game', 'game/logs',
                                           'game/logs/nx2saria.log', 'game/ocarina', 'game/profile',
                                           'game/profile/save', 'game/profile/save/1', 'game/profile/save/1/save.dat',
                                           'game/saves', 'game/saves/a.zip', 'mine.txt'])
        self.assertFalse(entry.exists())
        self.assertTrue(other.is_file())
        self.assertRegex((root / 'data' / 'logs' / 'launcher.log').read_text(),
                         r'\d\d:\d\d:\d\d uninstall: removed 13 paths after the window closed, 0 could not be '
                         r'removed\n$')

    def test_uninstall_with_the_game_leaves_nothing_of_ours(self):
        root, home, entry, other = self.install()
        (root / 'mine.txt').unlink()
        start = ('import sys, types\nfrom pathlib import Path\nfrom launcher import winsys\n'
                 'root = Path(sys.argv[1])\n'
                 'layout = types.SimpleNamespace(root=root, logs=root / "data" / "logs", dev=False, '
                 'stub=root / "none", toolchain=root / "toolchain")\n'
                 'log = root / "data" / "logs" / "launcher.log"\n'
                 'def line(text):\n'
                 '    with open(log, "a") as f:\n'
                 '        f.write(text + "\\n")\n'
                 'winsys.uninstall(layout, sys.argv[2], line)\n')
        env = {'PATH': os.environ.get('PATH', '/usr/bin:/bin'), 'HOME': str(home),
               'XDG_DATA_HOME': str(home / '.local' / 'share')}
        subprocess.run([sys.executable, '-c', start, str(root), str(root / 'game')], cwd=ROOT, env=env, check=True,
                       timeout=60)
        log = root / 'data' / 'logs' / 'launcher.log'
        deadline = time.time() + 30
        while time.time() < deadline and 'uninstall: removed' not in log.read_text():
            time.sleep(0.1)
        text = log.read_text()
        self.assertIn('uninstall: removing once the window has closed: ', text)
        self.assertIn(f'uninstall: kept: {root / "data" / "logs"} (the launcher\'s logs)\n', text)
        self.assertIn('uninstall: removed 14 paths after the window closed, 0 could not be removed', text)
        self.assertEqual(self.left(root), ['data', 'data/logs', 'data/logs/launcher.log'])
        self.assertFalse(entry.exists())
        self.assertTrue(other.is_file())

    def test_a_folder_without_the_appimage_install_is_left_alone(self):
        from types import SimpleNamespace
        root = self.tmp / 'release'
        (root / 'app').mkdir(parents=True)
        lines = []
        with mock.patch('launcher.winsys.start_detached') as detached:
            winsys.uninstall(SimpleNamespace(root=root, logs=root / 'data' / 'logs', dev=False, stub=root / 'none',
                                             toolchain=root / 'toolchain'), '', lines.append)
        detached.assert_not_called()
        self.assertEqual(lines, [f'uninstall: {root} holds no AppImage install; nothing was removed'])
        self.assertTrue((root / 'app').is_dir())

    def test_a_checkout_is_not_uninstalled(self):
        lines = []
        with mock.patch('launcher.winsys.start_detached') as detached:
            winsys.uninstall(Layout(root=self.tmp / 'w', app=ROOT), '', lines.append)
        detached.assert_not_called()
        self.assertEqual(lines, ['uninstall: a checkout is not uninstalled; nothing was removed'])


class ToolchainTest(Tmp):
    def test_zstd_missing(self):
        with mock.patch.dict(sys.modules, {'compression.zstd': None, 'zstandard': mock.MagicMock()}):
            self.assertEqual(toolchain.zstd_missing(self.layout), '')
        with mock.patch.dict(sys.modules, {'compression.zstd': None, 'zstandard': None}):
            msg = toolchain.zstd_missing(self.layout)
            ctx = Context(self.layout)
        self.assertIn('zstandard', msg)
        self.assertIn('-m pip install -r', msg)
        self.assertIn('requirements-launcher.txt', msg)
        self.assertEqual(ctx.zstd_missing, msg)
        self.assertTrue(any(shorten(msg) in line for line in ctx.log_tail()))

    def test_resolve_and_install(self):
        repo = self.tmp / 'repo'
        repo.mkdir()
        fake_repo(repo)
        srv = Server(repo)
        self.addCleanup(srv.close)
        lock = toolchain.make_lock(srv.url)
        names = [p['name'] for p in lock['packages']]
        self.assertEqual(names[0], toolchain.PREFIX + 'clang')
        self.assertIn(toolchain.PREFIX + 'llvm-libs', names)
        self.assertEqual(names.count(toolchain.PREFIX + 'vulkan-loader'), 1)
        self.assertFalse(toolchain.is_current(self.layout, lock))
        phases = []
        toolchain.install(self.layout, lock, progress=lambda ph, d, t: phases.append(ph), log=lambda m: None)
        self.assertTrue(toolchain.is_current(self.layout, lock))
        self.assertTrue((self.layout.msys / 'bin' / 'clang.exe').is_file())
        self.assertFalse((self.layout.msys / 'share' / 'doc').exists())
        self.assertIn('download', phases)
        self.assertIn('unpack', phases)
        hits = len(srv.hits)
        toolchain.install(self.layout, lock, log=lambda m: None)
        self.assertEqual(len(srv.hits), hits)
        self.assertEqual(toolchain.lock_digest(lock), toolchain.installed(self.layout)['digest'])

    def test_fast_clang_beside(self):
        repo = self.tmp / 'repo'
        repo.mkdir()
        fake_repo(repo)
        fake_fast_repo(repo)
        srv = Server(repo)
        self.addCleanup(srv.close)
        lock = toolchain.make_lock(srv.url, fast_repo=srv.url + 'clang64/')
        self.assertEqual(lock.get('fast_repo'), srv.url + 'clang64/')
        self.assertEqual(sum(p.get('env') == 'clang64' for p in lock['packages']), len(toolchain.FAST_PACKAGES))
        toolchain.install(self.layout, lock, log=lambda m: None)
        fast = self.layout.msys.parent / 'clang64'
        self.assertEqual((fast / 'bin' / 'clang.exe').read_bytes(), b'clang64')
        self.assertTrue((fast / 'bin' / 'libLLVM-23.dll').is_file())
        self.assertTrue((fast / 'lib' / 'clang' / '23' / 'include' / 'immintrin.h').is_file())
        self.assertFalse((fast / 'bin' / 'clang-repl.exe').exists())
        self.assertFalse((fast / 'share').exists())
        self.assertTrue(toolchain.is_current(self.layout, lock))

    def test_fast_clang_left_out_on_another_version(self):
        repo = self.tmp / 'repo'
        repo.mkdir()
        fake_repo(repo)
        fake_fast_repo(repo, clang_version='2.0-1')
        srv = Server(repo)
        self.addCleanup(srv.close)
        lock = toolchain.make_lock(srv.url, fast_repo=srv.url + 'clang64/')
        self.assertNotIn('fast_repo', lock)
        self.assertIn('2.0-1', lock['fast_skipped'])
        self.assertFalse(any(p.get('env') for p in lock['packages']))

    def test_damaged_package_refused(self):
        repo = self.tmp / 'repo'
        repo.mkdir()
        fake_repo(repo, damage='opus')
        srv = Server(repo)
        self.addCleanup(srv.close)
        lock = toolchain.make_lock(srv.url)
        with self.assertRaises(toolchain.ToolchainError):
            toolchain.install(self.layout, lock, log=lambda m: None)
        self.assertFalse(toolchain.is_current(self.layout, lock))

    def test_wheel_tag(self):
        tag = toolchain.wheel_tag()[0]
        self.assertTrue(tag.startswith(f'cp{sys.version_info.major}{sys.version_info.minor}-'))


class NetTest(Tmp):
    def test_get_bytes_retries_a_cut_connection(self):
        import urllib.error
        calls = []

        def flaky(url, headers=None, timeout=30, version='dev'):
            calls.append(url)
            if len(calls) == 1:
                raise ConnectionResetError(10054, 'An existing connection was forcibly closed by the remote host')
            if len(calls) == 2:
                raise urllib.error.HTTPError(url, 503, 'busy', {}, None)
            return io.BytesIO(b'{"ok": 1}')

        with mock.patch.object(net, 'request', flaky), mock.patch.object(net.time, 'sleep') as slept:
            self.assertEqual(net.get_json('https://example.invalid/x.json'), {'ok': 1})
        self.assertEqual(len(calls), 3)
        self.assertEqual(slept.call_count, 2)

        def missing(url, headers=None, timeout=30, version='dev'):
            calls.append(url)
            raise urllib.error.HTTPError(url, 404, 'not found', {}, None)

        calls.clear()
        with mock.patch.object(net, 'request', missing), mock.patch.object(net.time, 'sleep'):
            with self.assertRaises(urllib.error.HTTPError):
                net.get_bytes('https://example.invalid/missing')
        self.assertEqual(len(calls), 1)

        def down(url, headers=None, timeout=30, version='dev'):
            raise ConnectionResetError(10054, 'reset')

        with mock.patch.object(net, 'request', down), mock.patch.object(net.time, 'sleep'):
            with self.assertRaises(ConnectionResetError):
                net.get_bytes('https://example.invalid/down')

    def test_early_eof_retries_metadata_and_resumes_file(self):
        data = b'{"build":123}'
        (self.tmp / 'asset').write_bytes(data)
        cut = (200, {'Content-Length': str(len(data))}, data[:7])
        routes = {'/asset': cut}
        srv = Server(self.tmp, routes)
        self.addCleanup(srv.close)
        url = srv.url + 'asset'
        with mock.patch.object(net.time, 'sleep', side_effect=lambda _: routes.clear()):
            self.assertEqual(net.get_json(url), {'build': 123})
            routes['/asset'] = cut
            dest = self.tmp / 'download'
            net.download(url, dest, sha256=sha(data), size=len(data))
        self.assertEqual(dest.read_bytes(), data)
        self.assertEqual([h.get('Range') for _, h in srv.hits], [None, None, None, 'bytes=7-'])

    def test_download_resume_and_checks(self):
        data = os.urandom(3 * net.CHUNK + 123)
        (self.tmp / 'f.bin').write_bytes(data)
        srv = Server(self.tmp)
        self.addCleanup(srv.close)
        dest = self.tmp / 'out' / 'f.bin'
        dest.parent.mkdir()
        (dest.parent / 'f.bin.part').write_bytes(data[:1000])
        net.download(srv.url + 'f.bin', dest, sha256=sha(data), size=len(data))
        self.assertEqual(dest.read_bytes(), data)
        self.assertEqual(srv.hits[-1][1].get('Range'), 'bytes=1000-')
        with self.assertRaises(net.DownloadError):
            net.download(srv.url + 'f.bin', self.tmp / 'out' / 'g.bin', sha256='0' * 64)
        self.assertFalse((self.tmp / 'out' / 'g.bin').exists())
        with self.assertRaises(net.Cancelled):
            net.download(srv.url + 'f.bin', self.tmp / 'out' / 'h.bin', cancel=lambda: True)
        with self.assertRaises(net.DownloadError):
            net.download(srv.url + 'missing.bin', self.tmp / 'out' / 'm.bin')

    def test_the_download_total(self):
        data = os.urandom(2 * net.CHUNK + 500)
        have = 1000

        class Answer(io.BytesIO):
            status = 206
            length = None

            def __init__(self, headers):
                super().__init__(data[have:])
                self.headers = headers

        def fetch(headers, **kw):
            dest = self.tmp / 'total' / 'f.bin'
            shutil.rmtree(dest.parent, ignore_errors=True)
            dest.parent.mkdir()
            dest.with_name('f.bin.part').write_bytes(data[:have])
            seen = []
            with mock.patch.object(net, 'request', lambda url, h, timeout, version: Answer(headers)):
                net.download('https://example.invalid/f.bin', dest, progress=lambda d, t: seen.append((d, t)), **kw)
            self.assertEqual(dest.read_bytes(), data)
            self.assertEqual(seen[-1][0], len(data))
            return seen

        for d, t in fetch({}):
            self.assertEqual(t, d)
        for d, t in fetch({'Content-Length': str(len(data) - have)}):
            self.assertEqual(t, len(data))
        for d, t in fetch({}, size=len(data)):
            self.assertEqual(t, len(data))
        for d, t in fetch({'Content-Length': '7'}, size=len(data)):
            self.assertEqual(t, len(data))

    def test_a_part_file_of_another_download_is_not_resumed(self):
        old, new = os.urandom(6000), os.urandom(6000)
        (self.tmp / 'f.bin').write_bytes(new)
        srv = Server(self.tmp)
        self.addCleanup(srv.close)
        dest = self.tmp / 'out' / 'f.bin'
        dest.parent.mkdir()
        part, note = dest.with_name('f.bin.part'), dest.with_name('f.bin.part.of')
        part.write_bytes(old[:2000])
        note.write_text(f'sha256 {sha(old)}')
        lines = []
        net.download(srv.url + 'f.bin', dest, sha256=sha(new), size=len(new), log=lines.append)
        self.assertEqual(dest.read_bytes(), new)
        self.assertIsNone(srv.hits[-1][1].get('Range'))
        self.assertEqual(len(lines), 1)
        self.assertIn('f.bin.part was begun for another file', lines[0])
        self.assertFalse(part.exists() or note.exists())
        dest.unlink()
        part.write_bytes(new[:2000])
        note.write_text(f'sha256 {sha(new)}')
        other = srv.url.replace('127.0.0.1', 'localhost')
        net.download(other + 'f.bin', dest, sha256=sha(new), size=len(new), log=lines.append)
        self.assertEqual(srv.hits[-1][1].get('Range'), 'bytes=2000-')
        self.assertEqual((dest.read_bytes(), len(lines)), (new, 1))
        dest.unlink()
        part.write_bytes(new[:3000])
        net.download(srv.url + 'f.bin', dest, sha256=sha(new), size=len(new), log=lines.append)
        self.assertEqual(srv.hits[-1][1].get('Range'), 'bytes=3000-')
        self.assertEqual(dest.read_bytes(), new)
        with self.assertRaises(net.Cancelled):
            net.download(srv.url + 'f.bin', self.tmp / 'out' / 'c.bin', sha256=sha(new), cancel=lambda: True)
        self.assertEqual((self.tmp / 'out' / 'c.bin.part.of').read_text(), f'sha256 {sha(new)}')
        self.layout.downloads.mkdir(parents=True, exist_ok=True)
        (self.layout.downloads / 'x.zip.part.of').write_text('sha256 0')
        updater.prune(self.layout)
        self.assertEqual(list(self.layout.downloads.iterdir()), [])

    def low_disk(self, free):
        import collections
        usage = collections.namedtuple('usage', 'total used free')
        return mock.patch.object(net.shutil, 'disk_usage', lambda path: usage(10 ** 12, 0, free[0]))

    def test_the_free_space_is_asked_before_a_download(self):
        data = os.urandom(5000)
        (self.tmp / 'f.bin').write_bytes(data)
        srv = Server(self.tmp)
        self.addCleanup(srv.close)
        dest = self.tmp / 'out' / 'f.bin'
        dest.parent.mkdir()
        part = dest.with_name('f.bin.part')
        part.write_bytes(data[:1000])
        free = [40 << 20]
        with self.low_disk(free):
            self.assertEqual(net.space_problem(dest.parent, 1 << 20, 'x'), '')
            self.assertEqual(net.space_problem(self.tmp / 'not' / 'made' / 'yet', 1 << 20, 'x'), '')
            with self.assertRaises(net.NoSpace) as refused:
                net.download(srv.url + 'f.bin', dest, sha256=sha(data), size=64 << 20)
            self.assertEqual(srv.hits, [])
            self.assertIn('no room for f.bin on ', str(refused.exception))
            self.assertIn('MB needed, 42 MB free', str(refused.exception))
            self.assertEqual(part.read_bytes(), data[:1000])
            free[0] = 1 << 20
            with self.assertRaises(net.NoSpace):
                net.download(srv.url + 'f.bin', dest, sha256=sha(data))
            self.assertEqual(len(srv.hits), 1)
            self.assertEqual(part.read_bytes(), data[:1000])
            free[0] = 1 << 30
            net.download(srv.url + 'f.bin', dest, sha256=sha(data), size=len(data))
            self.assertEqual(dest.read_bytes(), data)
            self.assertEqual(srv.hits[-1][1].get('Range'), 'bytes=1000-')

        class Full(io.BytesIO):
            status, length, headers = 200, None, {}

            def read(self, n=-1):
                raise OSError(28, 'No space left on device')

        with mock.patch.object(net, 'request', lambda url, h, timeout, version: Full()), \
                mock.patch.object(net.time, 'sleep') as slept, self.assertRaises(net.NoSpace) as refused:
            net.download('https://example.invalid/g.bin', self.tmp / 'out' / 'g.bin')
        slept.assert_not_called()
        self.assertIn('is full: g.bin could not be written', str(refused.exception))
        with mock.patch.object(net.shutil, 'disk_usage', side_effect=OSError('no such drive')):
            self.assertEqual(net.space_problem(dest.parent, 1 << 40, 'x'), '')

    def test_the_free_space_is_asked_before_an_unpack(self):
        z = make_app_zip(self.tmp, '0.9.0', extra={'big.bin': bytes(3 << 20)})
        free = [2 << 20]
        with self.low_disk(free):
            with self.assertRaises(updater.UpdateError) as refused:
                updater.safe_extract(z, self.tmp / 'x')
            self.assertIn('no room for the files of NX2Saria-app-0.9.0.zip', str(refused.exception))
            self.assertFalse((self.tmp / 'x').exists())
            from launcher import mods
            with self.assertRaises(mods.ModError) as refused:
                mods.unpack(z, self.tmp / 'mod')
            self.assertIn('no room for its files', str(refused.exception))
            self.assertFalse((self.tmp / 'mod').exists())
            lock = {'packages': [{'name': 'p', 'version': '1', 'filename': 'p.pkg.tar.zst', 'sha256': '0' * 64,
                                  'csize': 5 << 20, 'isize': 50 << 20}]}
            with mock.patch.object(net, 'download') as fetched, self.assertRaises(toolchain.ToolchainError) as refused:
                toolchain.install(self.layout, lock, log=lambda line: None)
            fetched.assert_not_called()
            self.assertIn('no room for the build tools', str(refused.exception))
            self.assertIn('(91 MB needed, 2 MB free)', str(refused.exception))
            free[0] = 1 << 30
            updater.safe_extract(z, self.tmp / 'x')
        self.assertEqual((self.tmp / 'x' / 'big.bin').stat().st_size, 3 << 20)

    def test_plain_http_redirects_and_oversize(self):
        with self.assertRaises(net.DownloadError):
            net.request('http://example.invalid/f.bin')
        (self.tmp / 'f.bin').write_bytes(b'0123456789')
        routes = {}
        srv = Server(self.tmp, routes)
        self.addCleanup(srv.close)
        other = srv.url.replace('127.0.0.1', 'localhost')
        routes['/same'] = (302, {'Location': srv.url + 'f.bin'}, b'')
        routes['/away'] = (302, {'Location': other + 'f.bin'}, b'')
        token = {'Authorization': 'Bearer secret'}
        self.assertEqual(net.get_bytes(srv.url + 'same', token), b'0123456789')
        self.assertEqual(srv.hits[-1][1].get('Authorization'), 'Bearer secret')
        self.assertEqual(net.get_bytes(srv.url + 'away', token), b'0123456789')
        self.assertEqual(srv.hits[-2][1].get('Authorization'), 'Bearer secret')
        self.assertIsNone(srv.hits[-1][1].get('Authorization'))
        dest = self.tmp / 'out' / 'o.bin'
        with self.assertRaises(net.DownloadError):
            net.download(srv.url + 'f.bin', dest, size=4)
        self.assertFalse(dest.exists() or dest.with_name('o.bin.part').exists())

    def test_server_clock(self):
        srv = Server(self.tmp)
        self.addCleanup(srv.close)
        t = net.server_clock(srv.url)
        self.assertIsNotNone(t)
        self.assertLess(abs(t - time.time()), 5)
        self.assertEqual([h[0] for h in srv.hits], ['/'])
        srv.close()
        self.assertIsNone(net.server_clock(srv.url, timeout=2))


@unittest.skipUnless(shutil.which('openssl'), 'needs the openssl command')
class CertificateTest(Tmp):
    def setUp(self):
        super().setUp()
        saved = net._CHECKS, dict(net._ACCEPTED), net.server_clock
        self.addCleanup(lambda: setattr(net, '_CHECKS', saved[0]) or setattr(net, '_ACCEPTED', saved[1]) or
                        setattr(net, 'server_clock', saved[2]))
        net._ACCEPTED = {}
        self.data = os.urandom(100_000)
        (self.tmp / 'f.bin').write_bytes(self.data)

    def serve(self, start, end):
        cert = make_cert(self.tmp / f'cert{start}', start, end)
        srv = Server(self.tmp, cert=cert)
        self.addCleanup(srv.close)
        return srv, cert

    def test_refused_certificate(self):
        now = time.time()
        srv, cert = self.serve(now - 400 * 86400, now - 300 * 86400)
        check = client_context(trust=cert)
        net._CHECKS = [("Python's check", check)]
        asked = []
        cases = ((now + 400 * 86400, "This PC's clock reads"), (now - 3 * 86400, 'set the date, time and time zone'),
                 (now + 60, "This PC's clock agrees with 127.0.0.1's. Security software"),
                 (None, "Check that this PC's date, time and time zone are right"))
        for i, (theirs, says) in enumerate(cases):
            net.server_clock = lambda url, timeout=10, version='dev', t=theirs: asked.append(url) or t
            t0 = time.time()
            with self.assertRaises(net.CertificateError) as cm:
                net.download(srv.url + 'f.bin', self.tmp / 'out' / 'f.bin', sha256=sha(self.data))
            self.assertLess(time.time() - t0, 5)
            msg = str(cm.exception)
            self.assertIn("the certificate of 127.0.0.1 was not accepted (Python's check: certificate has expired)",
                          msg)
            self.assertIn(says, msg)
            self.assertEqual(check.handshakes, i + 1)
        self.assertEqual(asked, ['http://127.0.0.1/'] * len(cases))
        self.assertEqual(srv.hits, [])
        self.assertFalse((self.tmp / 'out' / 'f.bin.part').exists())

    def test_second_check_accepts(self):
        now = time.time()
        srv, cert = self.serve(now - 86400, now + 2 * 86400)
        refuses, accepts = client_context(), client_context(trust=cert)
        net._CHECKS = [("Python's check", refuses), ("Windows' check", accepts)]
        dest = net.download(srv.url + 'f.bin', self.tmp / 'out' / 'f.bin', sha256=sha(self.data))
        self.assertEqual(dest.read_bytes(), self.data)
        self.assertEqual((refuses.handshakes, accepts.handshakes), (1, 1))
        self.assertEqual(net._ACCEPTED, {'127.0.0.1': 1})
        self.assertEqual(net.get_bytes(srv.url + 'f.bin'), self.data)
        self.assertEqual((refuses.handshakes, accepts.handshakes), (1, 2))
        net._CHECKS = [("Python's check", refuses)]
        net.server_clock = lambda *a, **k: None
        with self.assertRaises(net.CertificateError) as cm:
            net.get_bytes(srv.url + 'f.bin')
        self.assertIn("(Python's check: self-signed certificate)", str(cm.exception))


def make_app_zip(folder, ver, extra=None):
    app = folder / f'app-{ver}'
    (app / 'launcher').mkdir(parents=True)
    (app / 'launcher' / 'start.pyw').write_text('# test\n')
    (app / 'VERSION').write_text(ver + '\n')
    z = folder / f'NX2Saria-app-{ver}.zip'
    with zipfile.ZipFile(z, 'w') as zf:
        for p in app.rglob('*'):
            if p.is_file():
                zf.write(p, p.relative_to(app).as_posix())
        for name, data in (extra or {}).items():
            zf.writestr(name, data)
    return z


def manifest_for(z, ver):
    return {'format': 1, 'version': ver, 'notes': '- One\n- Two',
            'app': {'name': z.name, 'size': z.stat().st_size, 'sha256': sha(z.read_bytes())}}


class FeedTest(Tmp):
    def test_whats_new_notes_are_plain_and_cover_the_four_release_fixtures(self):
        from pathlib import Path
        root = Path(__file__).resolve().parents[1]
        for version in ('v0.18.6', 'v0.18.7', 'v0.19.0-rc.1', 'v0.19.0-rc.2'):
            notes = feed.plain_notes((root / 'docs' / 'releases' / f'{version}.md').read_text(encoding='utf-8'))
            self.assertTrue(notes, version)
            self.assertTrue(all('http' not in line.lower() and '[' not in line and ']' not in line for line in notes), version)
        self.assertEqual(feed.plain_notes('[Install](https://example.test) · |\n- **Bold** and *italic*'),
                         ['Bold and italic'])

    def test_release_tag_rejects_a_links_line_in_the_first_six_nonempty_lines(self):
        import release_files
        root = self.tmp
        (root / 'VERSION').write_text('0.19.0-rc.2\n')
        notes = root / 'docs' / 'releases'
        notes.mkdir(parents=True)
        path = notes / 'v0.19.0-rc.2.md'
        path.write_text('One\nTwo\nThree\nFour\nFive\n[Install](https://example.test) ·\n')
        with self.assertRaisesRegex(release_files.Problem, 'first six non-empty lines'):
            release_files.check_tag('v0.19.0-rc.2', root)
        path.write_text('One\nTwo\nThree\nFour\nFive\nSix\n[Install](https://example.test) ·\n')
        self.assertTrue(release_files.check_tag('v0.19.0-rc.2', root))

    def test_asset_needs_a_plain_name_size_and_sha256(self):
        good = {'name': 'a.zip', 'size': 10, 'sha256': '0' * 64}
        r = feed.Release({'format': 1, 'version': '1.0.0', 'app': good}, assets={'a.zip': 'https://x/a.zip'})
        self.assertEqual(r.asset('app')[2:], (10, '0' * 64))
        for bad in ({'name': 'a.zip'}, dict(good, sha256='x'), dict(good, size=0), dict(good, size='10'),
                    dict(good, name='../a.zip'), dict(good, name='C:a.zip'), dict(good, name='sub\\a.zip')):
            r = feed.Release({'format': 1, 'version': '1.0.0', 'app': bad}, assets={bad['name']: 'https://x/a'})
            with self.assertRaises(feed.FeedError, msg=repr(bad)):
                r.asset('app')

    def serve_github(self):
        self.routes = {}
        srv = Server(self.tmp, self.routes)
        self.addCleanup(srv.close)
        return srv

    def publish(self, releases):
        self.routes['/repos/o/r/releases?per_page=20'] = (200, {'Content-Type': 'application/json'},
                                                          json.dumps(releases).encode())

    def release(self, srv, tag, ver, pre=False, manifest=True):
        z = make_app_zip(self.tmp, ver)
        m = self.tmp / f'm-{ver}.json'
        m.write_text(json.dumps(manifest_for(z, ver)))
        assets = [{'name': z.name, 'browser_download_url': srv.url + z.name, 'url': srv.url + z.name}]
        if manifest:
            assets.append({'name': feed.MANIFEST, 'browser_download_url': srv.url + m.name, 'url': srv.url + m.name})
        return {'tag_name': tag, 'prerelease': pre, 'draft': False, 'body': 'notes', 'html_url': 'x', 'assets': assets}

    def test_github_channels(self):
        srv = self.serve_github()
        self.publish([self.release(srv, 'v0.4.0-rc1', '0.4.0-rc1', pre=True),
                      self.release(srv, 'v0.3.12', '0.3.12', manifest=False), self.release(srv, 'v0.3.11', '0.3.11')])
        f = {'type': 'github', 'repo': 'o/r', 'api': srv.url.rstrip('/')}
        self.assertEqual(feed.latest(f, 'stable', platform='win32').version, '0.3.11')
        self.assertEqual(feed.latest(f, 'preview', platform='win32').version, '0.4.0-rc1')
        r = feed.latest(f, 'stable', token='secret', platform='win32')
        self.assertEqual(r.headers.get('Authorization'), 'Bearer secret')
        self.assertTrue(any(h.get('Authorization') == 'Bearer secret' for _, h in srv.hits))
        self.assertEqual(feed.plain_notes('## Head\n- **One**\n* `Two`\n<!-- x -->'), ['Head', 'One', 'Two'])

    def test_windows_feed_ignores_linux_assets_in_ten_asset_release(self):
        srv = self.serve_github()
        win_zip = make_app_zip(self.tmp, '0.3.11')
        win_runtime = 'NX2Saria-runtime-py3.14.9-1.zip'
        win_setup = 'NX2Saria-Setup-0.3.11.exe'
        win_manifest_data = manifest_for(win_zip, '0.3.11')
        win_manifest_data.update(runtime={'id': 'py3.14.9-1', 'name': win_runtime, 'size': 3,
                                           'sha256': '0' * 64},
                                  setup={'name': win_setup, 'size': 3, 'sha256': '1' * 64})
        win_manifest = self.tmp / 'windows.json'
        win_manifest.write_text(json.dumps(win_manifest_data))
        legacy_manifest = self.tmp / 'legacy.json'
        legacy_manifest_data = dict(win_manifest_data, version='0.3.10')
        legacy_manifest.write_text(json.dumps(legacy_manifest_data))
        linux_manifest = self.tmp / feed.LINUX_MANIFEST
        linux_manifest.write_text(json.dumps({'format': 1, 'version': '9.9.9', 'app': {}}))
        names = [win_zip.name, win_runtime, win_setup,
                 feed.MANIFEST, feed.LEGACY_MANIFEST, feed.LINUX_MANIFEST,
                 'NX2Saria-app-linux-x86_64-0.3.11.zip', 'NX2Saria-runtime-linux-x86_64-py3.14.9-1.zip',
                 'NX2Saria-Linux-0.3.11-x86_64.AppImage', 'SHA256SUMS.txt']
        assets = [{'name': n, 'browser_download_url': srv.url + n, 'url': srv.url + n} for n in names]
        self.routes['/' + feed.MANIFEST] = (200, {'Content-Type': 'application/json'}, win_manifest.read_bytes())
        self.routes['/' + feed.LEGACY_MANIFEST] = (200, {'Content-Type': 'application/json'}, legacy_manifest.read_bytes())
        self.routes['/' + feed.LINUX_MANIFEST] = (200, {'Content-Type': 'application/json'}, linux_manifest.read_bytes())
        self.routes['/' + win_zip.name] = (200, {'Content-Type': 'application/zip'}, win_zip.read_bytes())
        self.publish([{'tag_name': 'v0.3.11', 'prerelease': False, 'draft': False, 'assets': assets}])
        rel = feed._github({'repo': 'o/r', 'api': srv.url.rstrip('/')}, 'stable', '', 'dev', platform='win32')
        self.assertEqual(rel.manifest['version'], '0.3.11')
        self.assertEqual(rel.asset('app')[0], srv.url + win_zip.name)
        self.assertEqual(rel.manifest['setup']['name'], win_setup)
        self.assertEqual(rel.asset('runtime')[0], srv.url + win_runtime)
        self.assertEqual([path for path, _ in srv.hits if path in ('/' + feed.MANIFEST,
                                                                   '/' + feed.LEGACY_MANIFEST)],
                         ['/' + feed.MANIFEST])
        linux_rel = feed._github({'repo': 'o/r', 'api': srv.url.rstrip('/')}, 'stable', '', 'dev', platform='linux')
        self.assertEqual(linux_rel.manifest['version'], '9.9.9')

    def test_a_refused_check_says_when_to_try_again(self):
        srv = self.serve_github()
        reset = int(time.time()) + 1500
        self.routes['/repos/o/r/releases?per_page=20'] = (
            403, {'X-RateLimit-Remaining': '0', 'X-RateLimit-Reset': str(reset)}, b'{"message": "rate limit"}')
        f = {'type': 'github', 'repo': 'o/r', 'api': srv.url.rstrip('/')}
        with self.assertRaises(feed.FeedError) as c:
            feed.latest(f, 'stable', platform='win32')
        at = time.strftime('%H:%M', time.localtime(reset))
        self.assertRegex(str(c.exception), rf'^GitHub refused the request \(rate limit\); try again after {at} '
                                           r'\(in 2[45] minutes\)$')
        now = 1700000000
        text = feed.retry_text
        self.assertTrue(text({'X-RateLimit-Remaining': '0', 'X-RateLimit-Reset': str(now + 61)}, now)
                        .endswith(' (in 2 minutes)'))
        self.assertTrue(text({'X-RateLimit-Remaining': '0', 'X-RateLimit-Reset': str(now + 60)}, now)
                        .endswith(' (in 1 minute)'))
        self.assertEqual(text({'X-RateLimit-Remaining': '0', 'X-RateLimit-Reset': str(now - 5)}, now), 'in a minute')
        self.assertTrue(text({'Retry-After': '120'}, now).endswith(' (in 2 minutes)'))
        self.assertTrue(text({'Retry-After': '120'}, now).startswith('after '))
        self.assertEqual(text({'X-RateLimit-Remaining': '12', 'X-RateLimit-Reset': str(now + 600)}, now), '')
        for nothing in ({}, None, {'X-RateLimit-Remaining': '0', 'X-RateLimit-Reset': 'soon'},
                        {'X-RateLimit-Remaining': '0', 'X-RateLimit-Reset': str(now + 10 ** 6)}):
            self.assertEqual(text(nothing, now), '')
        self.routes['/repos/o/r/releases?per_page=20'] = (403, {}, b'{}')
        with self.assertRaises(feed.FeedError) as c:
            feed.latest(f, 'stable')
        self.assertEqual(str(c.exception), 'GitHub refused the request (rate limit); try again in an hour')
        ctx = Context(self.layout)
        self.addCleanup(ctx._file.close)
        ctx.settings['feed_repo'] = 'o/r'
        with mock.patch.object(feed, 'GITHUB_API', srv.url.rstrip('/')):
            j = jobs.CheckJob(ctx).run_inline()
        self.assertEqual((j.result, j.error), ('failed', 'GitHub refused the request (rate limit); try again in an hour'))
        ctx._file.flush()
        log = [line for line in (self.layout.logs / 'launcher.log').read_text(encoding='utf-8').splitlines()
               if 'FeedError:' in line]
        self.assertEqual(len(log), 1, log)
        self.assertIn('FeedError: GitHub refused the request (rate limit); try again in an hour (HTTPError: HTTP Error '
                      '403', log[0])
        self.assertNotIn('Traceback', log[0])

    def test_other_failures_keep_their_traceback(self):
        ctx = Context(self.layout)
        self.addCleanup(ctx._file.close)
        j = jobs.Job(ctx, [])
        j.run = lambda: [][1]
        j.run_inline()
        ctx._file.flush()
        log = (self.layout.logs / 'launcher.log').read_text(encoding='utf-8')
        self.assertIn('Traceback (most recent call last)', log)
        self.assertIn('IndexError', log)

    def test_url_feed_and_update(self):
        z = make_app_zip(self.tmp, '0.9.0')
        (self.tmp / feed.LINUX_MANIFEST).write_text(json.dumps(manifest_for(z, '0.9.0')))
        srv = Server(self.tmp)
        self.addCleanup(srv.close)
        rel = feed.latest({'type': 'url', 'url': srv.url + feed.MANIFEST}, platform='linux')
        self.assertEqual(rel.version, '0.9.0')
        self.layout.apps.mkdir(parents=True)
        (self.layout.apps / 'current.txt').write_text('0.8.0\n')
        old = self.layout.apps / '0.8.0' / 'launcher'
        old.mkdir(parents=True)
        (old / 'start.pyw').write_text('')
        for stale in ('0.7.0', '0.6.0'):
            (self.layout.apps / stale).mkdir()
        folder = updater.stage_app(self.layout, rel)
        self.assertTrue((folder / 'launcher' / 'start.pyw').is_file())
        updater.switch_to(self.layout, '0.9.0')
        self.assertEqual((updater.current(self.layout), updater.previous(self.layout)), ('0.9.0', '0.8.0'))
        updater.prune(self.layout)
        self.assertEqual(sorted(p.name for p in self.layout.apps.iterdir() if p.is_dir()), ['0.8.0', '0.9.0'])
        self.assertTrue(updater.rollback(self.layout))
        self.assertEqual(updater.current(self.layout), '0.8.0')
        self.assertFalse(updater.rollback(self.layout))

    def test_windows_url_feed_and_update(self):
        z = make_app_zip(self.tmp, '0.9.0')
        (self.tmp / feed.MANIFEST).write_text(json.dumps(manifest_for(z, '0.9.0')))
        srv = Server(self.tmp)
        self.addCleanup(srv.close)
        rel = feed.latest({'type': 'url', 'url': srv.url + feed.MANIFEST}, platform='win32')
        self.assertEqual(rel.version, '0.9.0')
        self.assertEqual(rel.manifest['app']['name'], z.name)

    def test_unsafe_or_wrong_archives(self):
        z = make_app_zip(self.tmp, '1.0.0', extra={'../evil.txt': 'x'})
        with self.assertRaises(updater.UpdateError):
            updater.safe_extract(z, self.tmp / 'x')
        self.assertFalse((self.tmp / 'evil.txt').exists())
        z2 = make_app_zip(self.tmp, '1.0.1')
        m = manifest_for(z2, '1.0.2')
        (self.tmp / feed.MANIFEST).write_text(json.dumps(m))
        srv = Server(self.tmp)
        self.addCleanup(srv.close)
        rel = feed.Release(m, assets={z2.name: srv.url + z2.name})
        with self.assertRaises(updater.UpdateError):
            updater.stage_app(self.layout, rel)
        self.assertFalse((self.layout.apps / '1.0.2').exists())


class BackgroundUpdateTest(Tmp):
    def setUp(self):
        super().setUp()
        self.srv = Server(self.tmp)
        self.addCleanup(self.srv.close)
        self.layout = self.installed('0.4.0')
        self.game = str(self.tmp / 'game')
        self.running = False
        saved = build.game_running
        build.game_running = lambda game_dir, exe=None, log=None: self.running
        self.addCleanup(setattr, build, 'game_running', saved)
        self.restarts = []

    def installed(self, ver):
        root = self.tmp / 'nx'
        app = root / 'app' / ver
        (app / 'launcher').mkdir(parents=True)
        (app / 'launcher' / 'start.pyw').write_text('')
        (app / 'VERSION').write_text(ver + '\n')
        (root / 'app' / 'current.txt').write_text(ver + '\n')
        (root / 'python').mkdir()
        (root / 'python' / 'runtime-id.txt').write_text('py-1\n')
        layout = Layout(root=root, app=app).ensure()
        self.assertFalse(layout.dev)
        return layout

    def publish(self, ver, runtime='', pre=False, damaged=False):
        z = make_app_zip(self.tmp, ver)
        m = manifest_for(z, ver)
        if runtime:
            r = self.tmp / f'NX2Saria-runtime-{runtime}.zip'
            with zipfile.ZipFile(r, 'w') as zf:
                zf.writestr('python.exe', 'MZ')
                zf.writestr('runtime-id.txt', runtime + '\n')
            m['runtime'] = {'id': runtime, 'name': r.name, 'size': r.stat().st_size, 'sha256': sha(r.read_bytes())}
        if damaged:
            m['app']['sha256'] = '0' * 64
        (self.tmp / f'm-{ver}.json').write_text(json.dumps(m))
        rel = feed.latest({'type': 'url', 'url': self.srv.url + f'm-{ver}.json'})
        rel.prerelease = pre
        return rel

    def context(self):
        ctx = Context(self.layout)
        self.addCleanup(lambda: ctx._file and ctx._file.close())
        return ctx

    def background(self, ctx):
        return jobs.BackgroundUpdate(ctx, lambda args: self.restarts.append(args) or True)

    def stage(self, bg, rel):
        self.assertEqual(bg.poll(rel), 'started')
        deadline = time.time() + 60
        while bg.staging() and time.time() < deadline:
            time.sleep(0.02)
        return bg.poll(rel)

    def state(self):
        return updater.current(self.layout), updater.previous(self.layout), self.restarts

    def test_staging_leaves_the_running_version(self):
        bg = self.background(self.context())
        rel = self.publish('0.5.0', runtime='py-2')
        self.assertEqual(self.stage(bg, rel), 'staged')
        folder = self.layout.apps / '0.5.0'
        self.assertTrue((folder / 'launcher' / 'start.pyw').is_file())
        self.assertEqual(updater.read_text(folder / updater.STAGED_PYTHON / 'runtime-id.txt'), 'py-2')
        self.assertFalse((self.layout.root / 'python.next').exists())
        self.assertEqual(self.state(), ('0.4.0', '', []))
        self.assertEqual(bg.ready(rel)['version'], '0.5.0')
        self.assertIsNone(bg.poll(rel))
        bg.checked()
        self.assertIsNone(bg.poll(rel))
        self.assertFalse(bg.staging())
        older = feed.Release({'format': 1, 'version': '0.4.5', 'app': {}})
        self.assertIsNone(bg.ready(older))
        updater.prune(self.layout)
        self.assertTrue((folder / updater.STAGED).is_file())

    def test_ready_marker(self):
        rel = self.publish('0.5.0', runtime='py-2')
        updater.stage_app(self.layout, rel)
        self.assertIsNone(updater.staged(self.layout))
        with self.assertRaises(updater.UpdateError):
            updater.install_staged(self.layout, '0.5.0')
        self.assertEqual(updater.current(self.layout), '0.4.0')
        updater.stage(self.layout, rel)
        self.assertEqual(updater.staged(self.layout)['version'], '0.5.0')
        self.assertIsNone(updater.staged(self.layout, skip='0.5.0'))
        py = self.layout.apps / '0.5.0' / updater.STAGED_PYTHON
        shutil.move(py, self.tmp / 'py')
        self.assertIsNone(updater.staged(self.layout))
        shutil.move(self.tmp / 'py', py)
        self.assertIsNotNone(updater.staged(self.layout))
        (self.layout.apps / '0.5.0' / updater.STAGED).write_text('{"version": "0.')
        self.assertIsNone(updater.staged(self.layout))
        updater.stage(self.layout, rel)
        updater.stage(self.layout, self.publish('0.6.0-rc1', pre=True))
        self.assertIsNone(updater.staged(self.layout, 'stable'))
        self.assertEqual(updater.staged(self.layout, 'preview')['version'], '0.6.0-rc1')
        self.assertFalse((self.layout.apps / '0.5.0').exists())
        self.assertTrue((self.layout.apps / '0.4.0').is_dir())

    def test_staged_version_installs_after_a_restart(self):
        bg = self.background(self.context())
        rel = self.publish('0.5.0', runtime='py-2')
        self.assertEqual(self.stage(bg, rel), 'staged')
        bg = self.background(self.context())
        self.assertEqual(bg.ready()['version'], '0.5.0')
        self.assertEqual(bg.install(self.game, why='the launcher started'), '0.5.0')
        self.assertEqual(self.state(), ('0.5.0', '0.4.0', [['--continue-update', '0.4.0']]))
        self.assertTrue(bg.restarting)
        self.assertEqual(updater.read_text(self.layout.root / 'python.next' / 'runtime-id.txt'), 'py-2')
        self.assertFalse((self.layout.apps / '0.5.0' / updater.STAGED).exists())
        self.assertTrue(updater.rollback(self.layout))
        ctx = self.context()
        ctx.settings['rolled_back'] = '0.5.0'
        bg = self.background(ctx)
        self.assertIsNone(bg.poll(rel))
        self.assertEqual(bg.install(self.game, rel), '')
        self.assertEqual(len(self.restarts), 1)

    def test_an_update_leaves_the_games_settings_file(self):
        game = self.layout.root / 'game'
        (game / 'profile').mkdir(parents=True)
        ini = game / 'nx2navi.ini'
        ini.write_bytes(PLAYER_INI)
        before = sorted(p.relative_to(game).as_posix() for p in game.rglob('*'))
        bg = self.background(self.context())
        rel = self.publish('0.5.0', runtime='py-2')
        self.assertEqual(self.stage(bg, rel), 'staged')
        self.assertEqual(ini.read_bytes(), PLAYER_INI)
        self.assertEqual(bg.install(str(game), rel, why='the launcher started'), '0.5.0')
        self.assertEqual(self.state(), ('0.5.0', '0.4.0', [['--continue-update', '0.4.0']]))
        updater.prune(self.layout)
        self.assertEqual(ini.read_bytes(), PLAYER_INI)
        self.assertEqual(sorted(p.relative_to(game).as_posix() for p in game.rglob('*')), before)
        gone, _ = winsys.uninstall_plan(self.layout)
        self.assertFalse([p for p in gone if p == game or p in ini.parents])

    def test_no_install_while_the_game_runs(self):
        bg = self.background(self.context())
        rel = self.publish('0.5.0')
        self.assertEqual(self.stage(bg, rel), 'staged')
        self.running = True
        self.assertEqual(bg.install(self.game, rel, why='the launcher started'), '')
        self.assertTrue(bg.waiting)
        self.assertEqual(bg.install(self.game, rel, asked=True), '')
        self.running = False
        self.assertEqual(bg.install(self.game, rel, running=True), '')
        bg.job = jobs.StageJob(bg.ctx, feed.Release({'format': 1, 'version': '0.5.1', 'app': {}}))
        self.assertEqual(bg.install(self.game, rel), '')
        self.assertEqual(self.state(), ('0.4.0', '', []))
        self.assertTrue((self.layout.apps / '0.5.0' / updater.STAGED).is_file())

    def test_install_after_the_game_exits(self):
        ctx = self.context()
        self.assertTrue(ctx.settings['auto_install'])
        bg = self.background(ctx)
        self.running = True
        rel = self.publish('0.5.0')
        self.assertEqual(self.stage(bg, rel), 'staged')
        self.assertEqual(bg.install(self.game, rel, why='the download finished'), '')
        self.assertEqual(self.state(), ('0.4.0', '', []))
        self.running = False
        self.assertEqual(bg.install(self.game, rel, why='the game exited'), '0.5.0')
        self.assertEqual(self.state(), ('0.5.0', '0.4.0', [['--continue-update', '0.4.0']]))
        self.assertFalse(bg.waiting)

    def test_nothing_automatic_when_the_setting_is_off(self):
        ctx = self.context()
        ctx.settings['auto_install'] = False
        bg = self.background(ctx)
        rel = self.publish('0.5.0')
        self.assertEqual(self.stage(bg, rel), 'staged')
        for why in ('the launcher started', 'the game exited', 'the game closed', 'the download finished'):
            self.assertEqual(bg.install(self.game, rel, why=why), '')
        self.assertFalse(bg.waiting)
        self.assertEqual(self.state(), ('0.4.0', '', []))
        bg.asked = True
        self.assertEqual(bg.install(self.game, rel, why='the download finished'), '0.5.0')
        self.assertEqual(self.state(), ('0.5.0', '0.4.0', [['--continue-update', '0.4.0']]))

    def test_failed_download_waits_for_the_next_check(self):
        ctx = self.context()
        bg = self.background(ctx)
        rel = self.publish('0.5.0', damaged=True)
        self.assertEqual(self.stage(bg, rel), 'failed')
        self.assertIn('0.5.0 was not staged', (self.layout.logs / 'launcher.log').read_text(encoding='utf-8'))
        self.assertIsNone(bg.poll(rel))
        self.assertIsNone(bg.ready(rel))
        self.assertEqual(bg.install(self.game, rel), '')
        bg.checked()
        self.assertEqual(self.stage(bg, rel), 'failed')
        self.assertEqual(self.state(), ('0.4.0', '', []))


def sarc(files):
    names = b''
    offs = []
    for n in files:
        offs.append(len(names))
        b = n.encode() + b'\0'
        names += b + b'\0' * (-len(b) % 4)
    data, ents = b'', b''
    for (n, d), no in zip(files.items(), offs):
        start = len(data)
        data += d
        ents += struct.pack('<IIII', 0, 0x01000000 | (no // 4), start, len(data))
        data += b'\0' * (-len(data) % 8)
    sfat = b'SFAT' + struct.pack('<HHI', 0xC, len(files), 0x65) + ents
    sfnt = b'SFNT' + struct.pack('<HH', 8, 0) + names
    base = 0x14 + len(sfat) + len(sfnt)
    head = b'SARC' + struct.pack('<HHIIHH', 0x14, 0xFEFF, base + len(data), base, 0x100, 0)
    return head + sfat + sfnt + data


def bfotf(otf, key=0x1234ABCD):
    body = otf + b'\0' * (-len(otf) % 4)
    words = struct.unpack(f'>{len(body) // 4}I', body)
    return struct.pack('>II', 0x7F9A0218 ^ key, len(otf) ^ key) + struct.pack(f'>{len(words)}I',
                                                                             *(w ^ key for w in words))


@unittest.skipUnless(ZSTD, 'needs compression.zstd (Python 3.14)')
class FontTest(Tmp):
    FONT = Path('/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf')

    @unittest.skipUnless(FONT.is_file(), 'needs DejaVu Sans')
    def test_font_from_romfs_image(self):
        otf = self.FONT.read_bytes()
        self.assertEqual(gamefiles.bfotf_to_otf(bfotf(otf)), otf)
        self.assertIsNone(gamefiles.bfotf_to_otf(b'\0' * 64))
        samples = [otf[i:i + 512] for i in range(0, min(len(otf), 400000), 997)]
        d = ZSTD.train_dict(samples, 16384)
        packed = ZSTD.compress(bfotf(otf), zstd_dict=d)
        self.assertNotEqual(ZSTD.get_frame_info(packed).dictionary_id, 0)
        files = {'Pack/ZsDic.pack.zs': ZSTD.compress(sarc({'zs.zsdic': d.dict_content})),
                 gamefiles.FONTS['title']: packed}
        romfs = self.tmp / 'game' / 'romfs'
        romfs.mkdir(parents=True)
        entries, blob = [], b''
        for path, data in sorted(files.items()):
            blob += b'\0' * (-len(blob) % 16)
            entries.append({'path': path, 'offset': len(blob), 'size': len(data)})
            blob += data
        (romfs / 'romfs.bin').write_bytes(blob)
        (romfs / 'index.json').write_text(json.dumps({'files': entries}))
        got = gamefiles.extract_fonts(self.layout, self.tmp / 'game')
        self.assertEqual(got['title'][1], 'DejaVu Sans')
        self.assertEqual(Path(got['title'][0]).read_bytes(), otf)
        self.assertEqual(gamefiles.cached_fonts(self.layout)['title'][1], 'DejaVu Sans')
        path = Path(got['title'][0])
        winsys.register_font(path)
        self.assertEqual(gamefiles.extract_fonts(self.layout, self.tmp / 'game')['title'][1], 'DejaVu Sans')
        winsys.unregister_font(path)
        path.write_bytes(b'not a font')
        self.assertEqual(Path(gamefiles.extract_fonts(self.layout, self.tmp / 'game')['title'][0]).read_bytes(), otf)


class PromptArtTest(Tmp):
    def make_art(self):
        import prompt_art as pa
        pads, keys = self.tmp / 'pack' / 'Source', self.tmp / 'keys' / 'Source'
        wanted = {rel for style in pa.STYLES.values() for part in style.values() for rel in part.values()
                  if rel != pa.OPTIONS}
        wanted |= {pa.SHARE, *pa.BLANKS.values(), f'{pa.KM}Dark/T_E_Key_Dark.png', f'{pa.KM}White/T_E_Key_White.png',
                   f'{pa.KM}Dark/T_F12_Key_Dark.png'}
        for rel in wanted | {'XGamepad/Default/T_X_Unused.png', f'{pa.KM}Retro/T_E_Key_Retro.png'}:
            path = (keys if rel.startswith(pa.KM) else pads) / rel
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(rel.encode())
        return pa, pads, keys, wanted

    def test_art_files(self):
        pa, pads, keys, wanted = self.make_art()
        for folders in ([pads, keys], [pads, keys / 'Keyboard_Mouse'], [pads / 'XGamepad', pads / 'P5Gamepad', keys]):
            self.assertEqual(set(pa.art_files(folders)), wanted)
        dest = self.tmp / 'release' / pa.FOLDER
        n, size = pa.copy_art([pads, keys], dest)
        copied = {p.relative_to(dest).as_posix(): p.read_bytes() for p in dest.rglob('*') if p.is_file()}
        self.assertEqual(copied, {rel: rel.encode() for rel in wanted})
        self.assertEqual((n, size), (len(wanted), sum(len(rel) for rel in wanted)))
        other = self.tmp / 'other' / 'Keyboard_Mouse'
        other.mkdir(parents=True)
        self.assertEqual(pa.art_roots([pads, keys, other])['Keyboard_Mouse'], other.parent)
        self.assertEqual(pa.art_roots([pads, keys])['Keyboard_Mouse'], keys)
        for folders, says in (([pads, keys, other.parent], 'in both'),
                              ([pads, keys / 'Keyboard_Mouse', other], 'in both'),
                              ([pads], 'no Keyboard_Mouse folder'), ([self.tmp / 'release'], 'holds none of')):
            with self.assertRaises(SystemExit) as e:
                pa.art_files(folders)
            self.assertIn(says, str(e.exception.code))
        (pads / pa.SHARE).unlink()
        with self.assertRaises(SystemExit) as e:
            pa.art_files([pads, keys])
        self.assertIn('T_X_Share-1.png', str(e.exception.code))

    def test_the_repository_carries_its_art(self):
        import prompt_art as pa
        files = pa.art_files([pa.REPO_ART])
        sums = dict(reversed(ln.split('  ', 1))
                    for ln in (pa.REPO_ART / 'SHA256SUMS').read_text(encoding='utf-8').splitlines())
        self.assertEqual(set(files), set(sums))
        on_disk = {p.relative_to(pa.REPO_ART).as_posix() for p in pa.REPO_ART.rglob('*') if p.is_file()}
        self.assertEqual(on_disk, set(sums) | {pa.SOURCE, 'SHA256SUMS'})
        for rel, path in files.items():
            self.assertEqual(hashlib.sha256(path.read_bytes()).hexdigest(), sums[rel], rel)
        self.assertEqual(pa.source_of(pa.REPO_ART), 'JulioCacko, FREE Input Prompts Pack 1.4 (CC0 1.0)')
        source = (pa.REPO_ART / pa.SOURCE).read_text(encoding='utf-8')
        for must in ('CC0 1.0 Universal. Free to use in any project, commercial or non-commercial. No attribution '
                     'required (but appreciated!).', 'Creative Commons Zero v1.0 Universal',
                     'f6c75921b9d6ab2cb2b5249f547c7b7ba19e22da9f4f3b45693cf0912f25ded0',
                     'https://juliocacko.itch.io/free-input-prompts', 'https://web.archive.org/web/'):
            self.assertIn(must, source)
        other = [self.tmp / 'pack']
        self.assertEqual(pa.release_art(None, False), [pa.REPO_ART])
        self.assertEqual(pa.release_art(other, False), other)
        self.assertIsNone(pa.release_art(None, True))
        with self.assertRaises(SystemExit):
            pa.release_art(other, True)
        dest = self.tmp / 'release' / pa.FOLDER
        n, size = pa.copy_art([pa.REPO_ART], dest)
        self.assertEqual((n, (dest / pa.SOURCE).read_text(encoding='utf-8')), (len(sums), source))
        self.assertEqual(pa.source_of(dest), pa.source_of(pa.REPO_ART))
        self.make_art()
        pack = self.tmp / 'pack' / 'Source'
        self.assertIn('has no SOURCE.md', pa.source_of(pack))
        (pack / pa.SOURCE).write_text('# some art\n', encoding='utf-8')
        self.assertIn('has no "Source:" line', pa.source_of(pack))

    def test_check_content_holds_the_art_to_its_list(self):
        import check_content as cc
        sums = {'XGamepad/Default/T_X_A_White.png': hashlib.sha256(b'picture').hexdigest()}
        self.assertIsNone(cc.art_problem(Path('prompt-art/XGamepad/Default/T_X_A_White.png'), b'picture', sums))
        self.assertIn('not listed', cc.art_problem(Path('prompt-art/XGamepad/Default/T_X_B_White.png'), b'picture', sums))
        self.assertIn('not listed', cc.art_problem(Path('prompt-art/new.png'), b'picture', sums))
        self.assertIn('differs', cc.art_problem(Path('prompt-art/XGamepad/Default/T_X_A_White.png'), b'changed', sums))
        for path in ('prompt-art/SOURCE.md', 'prompt-art/SHA256SUMS', 'docs/images/nx2saria-banner.png'):
            self.assertIsNone(cc.art_problem(Path(path), b'anything', sums), path)
        here = os.getcwd()
        os.chdir(ROOT)
        try:
            listed = cc.art_sums(False)
        finally:
            os.chdir(here)
        self.assertEqual(len(listed), 281)
        for rel in listed:
            self.assertIsNone(cc.art_problem(Path('prompt-art') / rel, (ROOT / 'prompt-art' / rel).read_bytes(), listed))

    def test_release_carries_what_the_prompts_need(self):
        import ast
        import make_kit
        import make_launcher
        pip = {'PIL': 'pillow', 'fontTools': 'fonttools'}
        local, packages = set(), set()
        for tool in ('pad_prompts.py', 'button_icons.py', 'prompt_art.py'):
            for node in ast.walk(ast.parse((ROOT / 'tools' / tool).read_text(encoding='utf-8'))):
                names = [a.name for a in node.names] if isinstance(node, ast.Import) else \
                    [node.module] if isinstance(node, ast.ImportFrom) and node.module else []
                for root in (n.split('.')[0] for n in names):
                    if (ROOT / 'tools' / f'{root}.py').is_file():
                        local.add(f'{root}.py')
                    elif root not in sys.stdlib_module_names:
                        packages.add(pip.get(root, root))
        self.assertEqual(packages, {'zstandard', 'pillow', 'fonttools'})
        reqs = (ROOT / 'tools' / 'requirements-launcher.txt').read_text(encoding='utf-8')
        pinned = {ln.split('==')[0].strip().lower() for ln in reqs.splitlines()
                  if '==' in ln and not ln.startswith('#')}
        self.assertLessEqual(packages, pinned)
        for tools in (make_launcher.TOOLS, make_kit.TOOLS):
            self.assertLessEqual(local | {'pad_prompts.py'}, set(tools))

    def test_launcher_release_carries_what_make_game_imports(self):
        import ast
        import make_kit
        import make_launcher
        tree = ast.parse((ROOT / 'tools' / 'make_game.py').read_text(encoding='utf-8'))
        guarded = set()
        for node in ast.walk(tree):
            if isinstance(node, ast.Try) and any(
                    isinstance(h.type, ast.Name) and h.type.id in ('ImportError', 'ModuleNotFoundError')
                    for h in node.handlers):
                for inner in node.body:
                    guarded |= {id(n) for n in ast.walk(inner)}
        needed = set()
        for node in ast.walk(tree):
            names = [a.name for a in node.names] if isinstance(node, ast.Import) else \
                [node.module] if isinstance(node, ast.ImportFrom) and node.module else []
            for root in (n.split('.')[0] for n in names):
                if (ROOT / 'tools' / f'{root}.py').is_file() and id(node) not in guarded and root != 'build_game':
                    needed.add(f'{root}.py')
        self.assertLessEqual(needed, set(make_launcher.TOOLS))
        self.assertLessEqual(needed, set(make_kit.TOOLS))


def nso(build_id=b'\0' * 16, size=0x300):
    head = bytearray(0x100)
    head[0:4] = b'NSO0'
    segs = [(0x100 + i * size, i * 0x10000, size) for i in range(3)]
    for i, (fo, mo, sz) in enumerate(segs):
        struct.pack_into('<III', head, 0x10 + i * 0x10, fo, mo, sz)
        struct.pack_into('<I', head, 0x60 + i * 4, sz)
    head[0x40:0x50] = build_id
    return bytes(head) + b''.join(bytes([i + 1]) * size for i in range(3))


def write_nup(path, exefs, romfs, title_id=0x0400486024DEC000):
    import nup as nupmod
    pool = bytearray()

    def name(s):
        off = len(pool)
        pool.extend(s.encode())
        return off, len(s.encode())

    dirs, files = [], []

    def add_tree(tree):
        idx = len(dirs)
        dirs.append(None)
        kids = []
        for n, v in sorted(tree.items()):
            if isinstance(v, dict):
                kids.append(('d', add_tree(v), n))
            else:
                files.append([n, v])
                kids.append(('f', len(files) - 1, n))
        dirs[idx] = kids
        return idx

    root = add_tree({'exefs': exefs, 'romfs': _nest(romfs)})
    nd = len(dirs)
    dir_rows, child_list = [], []
    for kids in dirs:
        first = len(child_list)
        for kind, i, _n in kids:
            child_list.append(i if kind == 'd' else nd + i)
        dir_rows.append((first, len(kids)))
    dir_names = {root: ''}
    for kids in dirs:
        for kind, i, n in kids:
            if kind == 'd':
                dir_names[i] = n
    exefs_dir = next(i for k, i, n in dirs[root] if k == 'd' and n == 'exefs')
    romfs_dir = next(i for k, i, n in dirs[root] if k == 'd' and n == 'romfs')
    toc_head = 0x30 + nupmod.PACKAGE.size + nd * nupmod.DIRECTORY.size + len(files) * nupmod.FILE.size + \
        4 * len(child_list)
    blob_off = nupmod.SUPER.size
    data = bytearray()
    file_rows = []
    for n, v in files:
        no, nl = name(n)
        file_rows.append((no, nl, len(v), blob_off + len(data), len(v), hashlib.sha256(v).digest(), 0))
        data.extend(v)
    dir_name_rows = [name(dir_names[i]) for i in range(nd)]
    toc_off = blob_off + len(data)
    toc = bytearray(struct.pack('<IIQQQQQ', 0x01434F54, 1, nd, len(files), toc_head, len(pool), 0))
    toc += nupmod.PACKAGE.pack(title_id, root, exefs_dir, romfs_dir, 0, 0, 0, 0, 0, 0, 0, b'\0' * 20)
    for i in range(nd):
        toc += nupmod.DIRECTORY.pack(*dir_name_rows[i], *dir_rows[i], 0)
    for no, nl, size, off, stored, digest, codec in file_rows:
        toc += nupmod.FILE.pack(no, nl, size, off, stored, digest, codec, 0, 0, b'\0' * 12)
    toc += struct.pack(f'<{len(child_list)}I', *child_list)
    toc += pool
    total = toc_off + len(toc)
    sup = nupmod.SUPER.pack(b'NUP\x01', 0x80, 1, toc_off, len(toc), hashlib.sha256(bytes(toc)).digest(), blob_off,
                            total, 1, 0, b'\0' * 16, b'\0' * 32)
    Path(path).write_bytes(sup + bytes(data) + bytes(toc))


def _nest(flat):
    tree = {}
    for p, v in flat.items():
        parts = p.split('/')
        node = tree
        for part in parts[:-1]:
            node = node.setdefault(part, {})
        node[parts[-1]] = v
    return tree


def made_up_game(out, steps=('exefs', 'romfs', 'lift', 'compile', 'shaders'), exe=True, kit='0.6.1'):
    import make_game
    out = Path(out)
    for d in ('exefs', 'romfs', 'build-cache'):
        (out / d).mkdir(parents=True, exist_ok=True)
    main = bytearray(0x100)
    main[:4], main[0x40:0x50] = b'NSO0', bytes.fromhex(make_game.MAIN_BUILD_ID)
    for n in make_game.EXEFS_FILES:
        (out / 'exefs' / n).write_bytes(bytes(main) if n == 'main' else b'x' * 64)
    (out / 'romfs' / 'romfs.bin').write_bytes(bytes(16))
    (out / 'romfs' / 'index.json').write_text('{}')
    if exe:
        (out / GAME_EXE).write_bytes(b'MZ')
    ident = 'nup X 1 1'
    keys = {s: f'{s}-1|{ident}' if s in ('exefs', 'romfs') else f'old-{s}' for s in steps}
    (out / 'build-cache' / 'state.json').write_text(json.dumps(
        {'version': make_game.STATE_VERSION, 'input': ident, 'kit': kit, 'steps': keys}))
    return out


FAKE_WINDRES = r'''
import json, os, re, sys
from pathlib import Path
a = sys.argv[1:]
tmp = os.environ['TMP']
for n in ('cc1', 'cc2'):
    open(os.path.join(tmp, n), 'w').close()
Path(__file__).with_suffix('.json').write_text(json.dumps(
    {'argv': a, 'cwd': os.getcwd(), 'tmp': tmp, 'temp': os.environ['TEMP']}))
if os.environ.get('FAKE_WINDRES_FAIL'):
    sys.exit('windres: preprocessing failed.')
text = open(a[a.index('-i') + 1]).read()
for name in re.findall(r'^\s*\w+\s+\w+\s+"([^"]+)"', text, re.M):
    open(name, 'rb').close()
open(a[a.index('-o') + 1], 'wb').write(b'coff')
'''

FAKE_PAD_PROMPTS = r'''
import json, sys
from pathlib import Path
import prompt_art
a = sys.argv[1:]
art, game = Path(a[a.index('--art') + 1]), Path(a[a.index('--game') + 1])
assert (art / 'XGamepad').is_dir(), art
if (art / 'fail').exists():
    sys.exit('the art says to fail')
with open(game / 'prompt-runs.txt', 'a') as f:
    f.write('run\n')
for name in prompt_art.OVERLAYS:
    (game / name).mkdir(exist_ok=True)
    (game / name / 'manifest.tsv').write_text('# path\n')
    print(f'{game / name}: 3 files, the font grew by 12 bytes', flush=True)
print(json.dumps({'status': 'pass', 'folders': []}))
'''


def prepared_list(programs, answers, translator, records=1):
    return b'NX2NSPRP' + struct.pack('<II', 2, 36) + programs + answers + translator + bytes(36 * records)


class MakeGameTest(Tmp):
    def setUp(self):
        super().setUp()
        import make_game
        self.mg = make_game
        exefs = {'main': nso(bytes.fromhex(make_game.MAIN_BUILD_ID)), 'sdk': nso(), 'subsdk0': nso(), 'rtld': nso(),
                 'main.npdm': b'META' + b'\0' * 0x100}
        self.romfs = {'a.txt': b'hello', 'dir/b.bin': os.urandom(5000), 'dir/sub/c': b'c' * 33}
        self.nup = self.tmp / 'game.nup'
        write_nup(self.nup, exefs, self.romfs)
        self.msys = self.tmp / 'msys'
        for need in ('bin/clang.exe', 'bin/clang++.exe', 'bin/SDL3.dll', 'lib/libSDL3.dll.a'):
            (self.msys / need).parent.mkdir(parents=True, exist_ok=True)
            (self.msys / need).write_bytes(b'')
        (self.msys / 'include' / 'vma').mkdir(parents=True)
        self.layout.pydeps.mkdir(parents=True, exist_ok=True)
        (self.layout.pydeps / 'zbic.py').write_text('def decompress(b):\n    return b\n')
        self.out = self.tmp / 'game out'
        self.real_probe = make_game.probe_toolchain
        probe = mock.patch.object(make_game, 'probe_toolchain', lambda msys, work: None)
        probe.start()
        self.addCleanup(probe.stop)
        self.real_driver_cache_folders = make_game.driver_cache_folders
        folders = mock.patch.object(make_game, 'driver_cache_folders', lambda out, **kw: [])
        folders.start()
        self.addCleanup(folders.stop)

    def run_mg(self, *args):
        events, lines = [], []
        run = build.Run(build.command(self.layout, list(args)), events.append, lines.append)
        code = run.wait()
        return code, events, lines

    def test_pipeline_rebuild_and_keys(self):
        code, ev, lines = self.run_mg('--nup', self.nup, '--out', self.out, '--msys', self.msys, '--until', 'romfs')
        self.assertEqual(code, 0, '\n'.join(lines))
        states = [(e['step'], e['state']) for e in ev if e['ev'] == 'step']
        self.assertEqual(states, [('check', 'start'), ('check', 'done'), ('exefs', 'start'), ('exefs', 'done'),
                                  ('romfs', 'start'), ('romfs', 'done')])
        self.assertTrue(any(e['ev'] == 'progress' and e['step'] == 'romfs' and e['done'] == e['total'] for e in ev))
        index = json.loads((self.out / 'romfs' / 'index.json').read_text())
        blob = (self.out / 'romfs' / 'romfs.bin').read_bytes()
        for e in index['files']:
            self.assertEqual(blob[e['offset']:e['offset'] + e['size']], self.romfs[e['path']])
            self.assertEqual(e['offset'] % 16, 0)
        flat = (self.out / 'exefs' / 'main').read_bytes()
        self.assertEqual(flat[0x40:0x50].hex(), self.mg.MAIN_BUILD_ID)
        code, ev, _ = self.run_mg('--nup', self.nup, '--out', self.out, '--msys', self.msys, '--until', 'romfs')
        self.assertEqual([e['state'] for e in ev if e['ev'] == 'step' and e['step'] in ('exefs', 'romfs')],
                         ['skip', 'skip'])
        self.nup.rename(self.tmp / 'moved.nup')
        code, ev, lines = self.run_mg('--rebuild', '--out', self.out, '--msys', self.msys, '--until', 'romfs')
        self.assertEqual(code, 0, '\n'.join(lines))
        self.assertEqual([e['state'] for e in ev if e['ev'] == 'step' and e['step'] in ('exefs', 'romfs')],
                         ['skip', 'skip'])
        st = json.loads((self.out / 'build-cache' / 'state.json').read_text())
        self.assertEqual(st['steps']['exefs'], self.mg.step_key('exefs', st['input']))
        self.assertNotIn(self.mg.kit_version(), st['steps']['romfs'])

    def test_rebuild_without_build(self):
        code, ev, lines = self.run_mg('--rebuild', '--out', self.tmp / 'empty', '--msys', self.msys)
        self.assertEqual(code, 1)
        self.assertTrue(any(e['ev'] == 'error' and 'rebuild' in e['message'] for e in ev))

    def test_wrong_version_refused(self):
        bad = self.tmp / 'bad.nup'
        write_nup(bad, {'main': nso(b'\x11' * 16), 'sdk': nso(), 'subsdk0': nso(), 'rtld': nso(),
                        'main.npdm': b'META' + b'\0' * 0x100}, {'a': b'a'})
        code, ev, _ = self.run_mg('--nup', bad, '--check')
        self.assertEqual(code, 1)
        self.assertTrue(any(e['ev'] == 'error' and '1.1.1' in e['message'] for e in ev))

    @unittest.skipUnless(shutil.which('cc'), 'needs a C compiler')
    def test_object_cache_survives_a_moved_kit(self):
        kits = []
        for name in ('0.3.10', '0.3.11'):
            kit = self.tmp / 'app' / name / 'runtime'
            kit.mkdir(parents=True)
            (kit / 'h.h').write_text('#define V 1\n')
            (kit / 'a.c').write_text('#include "h.h"\nint a(void) { return V; }\n')
            kits.append(kit.parent)
        obj = self.tmp / 'obj'
        obj.mkdir()

        def go(kit):
            rt = kit / 'runtime'
            srcs = [(rt / 'a.c', obj / 'rt_a.o')]
            cache = self.mg.ObjectCache(obj / 'objects.json', {'<kit>': kit})
            return self.mg.compile_all(srcs, lambda s_, d: ['cc', '-c', *runtime_include.flags(rt), '-o', str(d), str(s_)], cache,
                                       1, dict(os.environ), salt='cc')
        self.assertEqual(go(kits[0])[:2], (1, 0))
        self.assertEqual(go(kits[1])[:2], (0, 1))
        (kits[1] / 'runtime' / 'h.h').write_text('#define V 2\n')
        self.assertEqual(go(kits[1])[:2], (1, 0))

    def main_with(self, romfs, lift, compile_, rebuild=False):
        sys.path.insert(0, str(self.layout.pydeps))
        saved = (self.mg.step_romfs, self.mg.step_lift, self.mg.step_compile, sys.argv)
        self.mg.step_romfs, self.mg.step_lift, self.mg.step_compile = romfs, lift, compile_
        source = ['--rebuild'] if rebuild else ['--nup', str(self.nup)]
        sys.argv = ['make_game.py', *source, '--out', str(self.out), '--msys', str(self.msys),
                    '--until', 'compile']
        try:
            with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                self.mg.main()
            return 0
        except SystemExit as e:
            return e.code
        finally:
            self.mg.step_romfs, self.mg.step_lift, self.mg.step_compile, sys.argv = saved
            sys.path.remove(str(self.layout.pydeps))

    def test_retry_interrupted_lift_after_cache_cleanup(self):
        layout = self.out / 'build-cache' / 'lift' / 'layout.json'
        ran = []

        def lift(out, cache):
            ran.append('lift')
            layout.parent.mkdir(exist_ok=True)
            layout.write_text('[]')

        def compile_(out, cache, msys, jobs, romfs_job, update):
            ran.append('compile')
            self.assertTrue(layout.is_file())

        def interrupted(out, cache):
            lift(out, cache)
            raise SystemExit(1)

        with mock.patch.object(self.mg, 'WIN', True):
            self.assertEqual(self.main_with(self.mg.step_romfs, lift, compile_), 0)
            self.nup.unlink()
            ran.clear()
            shutil.rmtree(layout.parent)
            self.assertEqual(self.main_with(self.mg.step_romfs, lift, compile_, rebuild=True), 0)
            self.assertEqual(ran, [])

            with mock.patch.object(self.mg, 'kit_version', return_value='updated kit'):
                self.assertEqual(self.main_with(self.mg.step_romfs, interrupted, compile_, rebuild=True), 1)
                self.assertEqual(self.main_with(self.mg.step_romfs, lift, compile_, rebuild=True), 0)
                self.assertEqual(self.main_with(self.mg.step_romfs, lift, compile_, rebuild=True), 0)
            self.assertEqual(ran, ['lift', 'lift', 'compile'])

    def test_romfs_beside_lift_and_compile(self):
        real_romfs, seen = self.mg.step_romfs, {}

        def romfs(src, out, jobs):
            time.sleep(0.5)
            real_romfs(src, out, jobs)

        def lift(out, cache):
            seen['lift_beside_romfs'] = not (out / 'romfs' / 'index.json').exists()

        def compile_(out, cache, msys, jobs, romfs_job=None, update=False):
            seen['job'] = romfs_job is not None
            romfs_job.join()
            seen['image_after_join'] = (out / 'romfs' / 'romfs.bin').is_file()
            (out / self.mg.EXE_NAME).write_bytes(b'MZ')
        self.assertEqual(self.main_with(romfs, lift, compile_), 0)
        self.assertEqual(seen, {'lift_beside_romfs': True, 'job': True, 'image_after_join': True})
        steps = json.loads((self.out / 'build-cache' / 'state.json').read_text())['steps']
        self.assertEqual(set(steps), {'exefs', 'romfs', 'lift', 'compile'})

    def test_a_repeated_romfs_step_does_not_repeat_the_lift(self):
        calls = []

        def lift(out, cache):
            calls.append('lift')
            (cache / 'lift').mkdir(exist_ok=True)
            (cache / 'lift' / 'layout.json').write_text('[]')

        def compile_(out, cache, msys, jobs, romfs_job=None, update=False):
            calls.append('compile')
            if romfs_job:
                romfs_job.join()
            (out / self.mg.EXE_NAME).write_bytes(b'MZ')
        self.assertEqual(self.main_with(self.mg.step_romfs, lift, compile_), 0)
        self.assertEqual(calls, ['lift', 'compile'])
        state = self.out / 'build-cache' / 'state.json'
        st = json.loads(state.read_text())
        for step in ('romfs', 'compile'):
            del st['steps'][step]
        state.write_text(json.dumps(st))
        (self.out / 'romfs' / 'romfs.bin').unlink()
        self.assertEqual(self.main_with(self.mg.step_romfs, lift, compile_), 0)
        self.assertEqual(calls, ['lift', 'compile', 'compile'])
        self.assertTrue((self.out / 'romfs' / 'romfs.bin').is_file())
        self.assertEqual(set(json.loads(state.read_text())['steps']), {'exefs', 'romfs', 'lift', 'compile'})
        st = json.loads(state.read_text())
        del st['steps']['exefs']
        state.write_text(json.dumps(st))
        self.assertEqual(self.main_with(self.mg.step_romfs, lift, compile_), 0)
        self.assertEqual(calls, ['lift', 'compile', 'compile', 'lift', 'compile'])

    def test_shader_progress_reaches_the_launcher(self):
        kit = self.tmp / 'kit'
        (kit / 'tools').mkdir(parents=True)
        (kit / 'bin').mkdir()
        self.out.mkdir()
        for name in (self.mg.XLATE_NAME, 'nx2navi_xlate.id'):
            (kit / 'bin' / name).write_bytes(b'current translator')
            (self.out / name).write_bytes(b'old translator')
        (kit / 'tools' / 'prepare_shaders.py').write_text(
            'import time\n'
            'print("reading the inventory", flush=True)\n'
            'time.sleep(0.6)\n'
            'for d in (0, 50, 100):\n'
            '    print(f"prepare-shaders: progress translate {d} 100", flush=True)\n'
            '    time.sleep(0.05)\n'
            'print("prepare-shaders: progress build 10000 100000", flush=True)\n'
            'time.sleep(0.6)\n'
            'for d in (99998, 99999, 100000):\n'
            '    print(f"prepare-shaders: progress build {d} 100000", flush=True)\n')
        for name, value in (('KIT', kit), ('EVENTS', True), ('SHADER_BEAT_S', 0.1), ('SHADER_STALL_S', 0.3)):
            self.addCleanup(setattr, self.mg, name, getattr(self.mg, name))
            setattr(self.mg, name, value)
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            self.mg.step_shaders(self.out)
        for name in (self.mg.XLATE_NAME, 'nx2navi_xlate.id'):
            self.assertEqual((self.out / name).read_bytes(), b'current translator')
        lines = buf.getvalue().splitlines()
        events = [json.loads(ln[2:]) for ln in lines if ln.startswith('@@')]
        self.assertIn('reading the inventory', lines)
        self.assertFalse([ln for ln in lines if ln.startswith('prepare-shaders: progress')])
        self.assertTrue(all(e['ev'] == 'progress' and e['step'] == 'shaders' for e in events))
        fractions = [e['fraction'] for e in events]
        self.assertEqual(fractions, sorted(fractions))
        self.assertEqual(fractions[-1], 1.0)
        details = [e['detail'] for e in events]
        self.assertTrue(any(d.startswith("Reading the game's shaders") for d in details))
        self.assertIn('Translated 50 of 100 shaders', details)
        self.assertGreaterEqual(sum(1 for e in events if e['fraction'] == 0.55), 2)
        self.assertTrue(any(d.startswith('Built 10,000 of 100,000') and 'still running' in d for d in details))
        for done in ('99,998', '99,999'):
            event = next(e for e in events if e['detail'].startswith(f'Built {done} of 100,000'))
            self.assertEqual(int(event['fraction'] * 100), 99)
        self.assertEqual([e['fraction'] for e in events if e['detail'] == 'Translated 50 of 100 shaders'][0], 0.25)

    def test_a_build_only_run_fills_the_whole_bar(self):
        p = self.mg.shader_progress
        self.assertEqual(p('build', 0, 42142, 0, 1, build_only=True)['fraction'], 0.0)
        self.assertEqual(p('build', 21071, 42142, 0, 600, build_only=True)['fraction'], 0.5)
        self.assertEqual(p('build', 42142, 42142, 0, 1200, build_only=True)['fraction'], 1.0)
        self.assertEqual(p('build', 21071, 42142, 0, 600)['fraction'], 0.75)
        self.assertEqual(p('build', 21071, 42142, 0, 600, build_only=True)['detail'],
                         'Built 21,071 of 42,142 shaders for your graphics card')
        self.out.mkdir()
        (self.out / 'shader-prepared.bin').write_bytes(b'x')
        exe = self.out / self.mg.EXE_NAME
        exe.write_bytes(b'MZ')
        script = self.tmp / 'warm.py'
        script.write_text('for d in (0, 25, 100):\n    print(f"prepare-shaders: progress build {d} 100", flush=True)\n')
        real = self.mg.subprocess.Popen

        def popen(cmd, **kw):
            self.assertEqual(cmd, [str(exe), '--warm-shaders'])
            return real([sys.executable, str(script)], **kw)
        buf = io.StringIO()
        with mock.patch.object(self.mg, 'EVENTS', True), mock.patch.object(self.mg.subprocess, 'Popen', popen), \
                contextlib.redirect_stdout(buf):
            self.mg.warm_driver_cache(self.out)
        events = [json.loads(ln[2:]) for ln in buf.getvalue().splitlines() if ln.startswith('@@')]
        self.assertEqual([e['fraction'] for e in events if e['detail'].startswith('Built')], [0.0, 0.25, 1.0])

    def test_a_warm_that_leaves_stalled_builds_to_the_game_succeeds(self):
        self.out.mkdir()
        (self.out / 'shader-prepared.bin').write_bytes(b'x')
        exe = self.out / self.mg.EXE_NAME
        exe.write_bytes(b'MZ')
        stalled = self.tmp / 'stalled.py'
        stalled.write_text(
            'print("prepare-shaders: progress build 99 100", flush=True)\n'
            'print("prepare-shaders: variant ' + 'ab' * 32 + ' (compute, 900000 SPIR-V words) still building after '
            '301 s", flush=True)\n'
            'print("prepare-shaders: 99 of 100 variants built into the driver cache on 12 threads in 400000 ms; 1 still '
            'building after 300 s each are left to the game, which goes on with them in the background '
            '(NX2SARIA_PREPARE_STALL_S)", flush=True)\n')
        done = self.tmp / 'done.py'
        done.write_text('print("prepare-shaders: progress build 100 100", flush=True)\n')
        real = self.mg.subprocess.Popen
        for script, want, unwanted in ((stalled, 'the rest continues in the game, in the background', 'did not finish'),
                                       (done, "driver's cache warm in", 'the rest continues')):
            def popen(cmd, script=script, **kw):
                return real([sys.executable, str(script)], **kw)
            buf = io.StringIO()
            with mock.patch.object(self.mg.subprocess, 'Popen', popen), contextlib.redirect_stdout(buf):
                self.mg.warm_driver_cache(self.out)
            text = buf.getvalue()
            self.assertIn(want, text)
            self.assertNotIn(unwanted, text)
        self.assertTrue(self.mg.left_text(1).startswith('1 shader the graphics driver was still building'))
        self.assertTrue(self.mg.left_text(3).startswith('3 shaders the graphics driver was still building'))

    def test_the_driver_cache_folders_are_found_and_counted(self):
        folders = self.real_driver_cache_folders
        local = self.tmp / 'local'
        gl = local / 'NVIDIA' / 'GLCache'
        (gl / 'ab').mkdir(parents=True)
        (gl / 'ab' / 'x.bin').write_bytes(b'x' * 1000)
        (gl / 'y.toc').write_bytes(b'y' * 24)
        (local / 'NVIDIA' / 'NvBackend').mkdir()
        (local / 'NVIDIA' / 'OtherCache').write_bytes(b'a file, not a folder')
        (local / 'NVIDIA' / 'DXCache').mkdir()
        (local / 'AMD' / 'VkCache').mkdir(parents=True)
        (local / 'Intel' / 'Other').mkdir(parents=True)
        (local / 'D3DSCache').mkdir()
        (local / 'Unrelated' / 'Cache').mkdir(parents=True)
        self.out.mkdir()
        with mock.patch.dict(os.environ):
            os.environ.pop(self.mg.DRIVER_CACHE_ENV, None)
            win = folders(self.out, windows=True, local_appdata=str(local))
            self.assertEqual([label for label, _ in win],
                             ['%LOCALAPPDATA%\\NVIDIA\\DXCache', '%LOCALAPPDATA%\\NVIDIA\\GLCache',
                              '%LOCALAPPDATA%\\AMD\\VkCache', '%LOCALAPPDATA%\\D3DSCache'])
            self.assertEqual(folders(self.out, windows=True, local_appdata=str(self.tmp / 'none')), [])
            self.assertEqual(folders(self.out, windows=True, local_appdata=''), [])
            self.assertEqual(folders(self.out, windows=False), [])
            (self.out / 'mesa-cache').mkdir()
            (self.out / 'nvidia-cache').mkdir()
            self.assertEqual([label for label, _ in folders(self.out, windows=False)], ['nvidia-cache', 'mesa-cache'])
            self.assertEqual([label for label, _ in folders(self.out, windows=True, local_appdata=str(local))],
                             ['nvidia-cache', '%LOCALAPPDATA%\\NVIDIA\\DXCache', '%LOCALAPPDATA%\\NVIDIA\\GLCache',
                              '%LOCALAPPDATA%\\AMD\\VkCache', '%LOCALAPPDATA%\\D3DSCache'])
            self.assertEqual(folders(self.out, windows=True, local_appdata=''), [('nvidia-cache', self.out / 'nvidia-cache')])
            self.assertEqual(folders(self.out, windows=True, local_appdata=str(self.tmp / 'none')),
                             [('nvidia-cache', self.out / 'nvidia-cache')])
            custom = self.tmp / 'nvcache'
            os.environ[self.mg.DRIVER_CACHE_ENV] = str(custom)
            self.assertNotIn(self.mg.DRIVER_CACHE_ENV, [label for label, _ in folders(self.out, windows=False)])
            custom.mkdir()
            self.assertEqual([label for label, _ in folders(self.out, windows=False)],
                             ['nvidia-cache', 'mesa-cache', self.mg.DRIVER_CACHE_ENV])
            self.assertEqual(folders(self.out, windows=True, local_appdata=str(local))[-1][0], self.mg.DRIVER_CACHE_ENV)
            os.environ.pop(self.mg.DRIVER_CACHE_ENV)
            listed = len(folders(self.out, windows=True, local_appdata=str(local)))
            try:
                (local / 'Intel' / 'ShaderCache').symlink_to(gl, target_is_directory=True)
            except (OSError, NotImplementedError):
                pass
            else:
                self.assertEqual(len(folders(self.out, windows=True, local_appdata=str(local))), listed)
        size = self.mg.folder_size
        self.assertEqual(size(gl), (1024, 2, ''))
        self.assertEqual(size(local / 'D3DSCache'), (0, 0, ''))
        self.assertEqual(size(self.tmp / 'missing'), (0, 0, ''))
        self.assertEqual(size(gl, budget_s=-1), (24, 1, 'the count stopped'))
        flat = local / 'AMD' / 'VkCache'
        (flat / 'only.bin').write_bytes(b'z' * 7)
        self.assertEqual(size(flat, budget_s=-1), (7, 1, ''))
        with mock.patch.object(self.mg.os, 'scandir', side_effect=PermissionError('denied')):
            self.assertEqual(size(gl), (0, 0, 'some of it could not be read'))

        class Refused:
            path = 'refused'

            def is_dir(self, follow_symlinks=True):
                return False

            def stat(self, follow_symlinks=True):
                raise PermissionError('denied')

        class Listing:
            def __enter__(self):
                return iter([Refused()])

            def __exit__(self, *exc):
                return False

        with mock.patch.object(self.mg.os, 'scandir', lambda path: Listing()):
            self.assertEqual(size(gl), (0, 0, 'some of it could not be read'))
        with mock.patch.object(self.mg.os, 'scandir', side_effect=FileNotFoundError('gone')):
            self.assertEqual(size(gl), (0, 0, ''))
        try:
            (gl / 'loop').symlink_to(gl, target_is_directory=True)
        except (OSError, NotImplementedError):
            pass
        else:
            total, files, why = size(gl, budget_s=5)
            self.assertEqual((files, why), (3, ''))
            self.assertGreater(total, 1024)

    def test_the_shaders_step_says_what_the_driver_caches_held_before_and_after(self):
        kit = self.tmp / 'kit'
        (kit / 'tools').mkdir(parents=True)
        (kit / 'tools' / 'prepare_shaders.py').write_text(
            'print("the step runs here", flush=True)\nprint("prepare-shaders: progress build 1 1", flush=True)\n')
        cache = self.tmp / 'drivercache'
        cache.mkdir()
        self.out.mkdir()
        self.addCleanup(setattr, self.mg, 'KIT', self.mg.KIT)
        self.mg.KIT = kit
        sizes = iter([(2 ** 30, 41203, ''), (3 * 2 ** 30, 41203, '')])
        other = self.tmp / 'other'
        other.mkdir()

        def folders(out):
            return [('%LOCALAPPDATA%\\NVIDIA\\GLCache', cache), ('%LOCALAPPDATA%\\AMD\\VkCache', other)]

        def size(folder, budget_s=0):
            if folder == other:
                return 0, 0, 'the count stopped'
            return next(sizes)

        buf = io.StringIO()
        with mock.patch.object(self.mg, 'driver_cache_folders', folders), \
                mock.patch.object(self.mg, 'folder_size', size), contextlib.redirect_stdout(buf):
            self.mg.step_shaders(self.out)
        lines = buf.getvalue().splitlines()
        before = [i for i, ln in enumerate(lines) if ln.startswith('  graphics driver caches before the step: ')]
        after = [i for i, ln in enumerate(lines) if ln.startswith('  graphics driver caches after the step: ')]
        marker = lines.index('the step runs here')
        self.assertEqual((len(before), len(after)), (1, 1))
        self.assertLess(before[0], marker)
        self.assertLess(marker, after[0])
        self.assertEqual(lines[before[0]], '  graphics driver caches before the step: %LOCALAPPDATA%\\NVIDIA\\GLCache '
                                           '1.00 GB in 41,203 files; %LOCALAPPDATA%\\AMD\\VkCache 0.00 GB in 0 files '
                                           '(the count stopped)')
        self.assertTrue(lines[after[0]].startswith('  graphics driver caches after the step: %LOCALAPPDATA%\\NVIDIA\\GLCache '
                                                   '3.00 GB in 41,203 files; '))
        sizes = iter([(5, 1, ''), (6, 1, '')])
        buf = io.StringIO()
        with mock.patch.object(self.mg, 'driver_cache_folders', lambda out: [('c', cache)]), \
                mock.patch.object(self.mg, 'folder_size', size), \
                mock.patch.object(self.mg, 'shaders_work', side_effect=SystemExit(1)), \
                contextlib.redirect_stdout(buf), self.assertRaises(SystemExit):
            self.mg.step_shaders(self.out)
        self.assertEqual(buf.getvalue().count('graphics driver caches'), 2)
        for error in (OSError('access denied'), RuntimeError('odd')):
            buf = io.StringIO()

            def broken(out, error=error):
                raise error

            with mock.patch.object(self.mg, 'driver_cache_folders', broken), contextlib.redirect_stdout(buf):
                self.mg.step_shaders(self.out)
            self.assertIn(f'graphics driver caches before the step: not counted ({error})', buf.getvalue())
            self.assertIn(f'graphics driver caches after the step: not counted ({error})', buf.getvalue())
        buf = io.StringIO()
        with mock.patch.object(self.mg, 'driver_cache_folders', lambda out: []), contextlib.redirect_stdout(buf):
            self.mg.step_shaders(self.out)
        self.assertNotIn('graphics driver caches', buf.getvalue())
        given = []

        def size_given(folder, budget_s=0):
            given.append(budget_s)
            return 0, 0, ''

        clock = iter([100.0, 110.0, 135.0])
        with mock.patch.object(self.mg, 'driver_cache_folders', folders), mock.patch.object(self.mg, 'folder_size', size_given), \
                mock.patch.object(self.mg.time, 'monotonic', lambda: next(clock)), contextlib.redirect_stdout(io.StringIO()):
            self.mg.say_driver_caches(self.out, 'x')
        self.assertEqual(given, [self.mg.DRIVER_CACHE_SCAN_S - 10.0, 0.0])
        for update in (False, True):
            with mock.patch.object(self.mg, 'shaders_work') as work, contextlib.redirect_stdout(io.StringIO()):
                self.mg.step_shaders(self.out, update)
            work.assert_called_once_with(self.out, update)

    def test_shaders_step_packs_the_cache(self):
        kit = self.tmp / 'kit'
        (kit / 'tools').mkdir(parents=True)
        (kit / 'tools' / 'prepare_shaders.py').write_text('print("prepare-shaders: progress build 1 1", flush=True)\n')
        cache = self.out / 'shader-cache'
        (cache / 'in').mkdir(parents=True)
        (cache / ('ab' * 32 + '.spv')).write_bytes(b'\x03\x02\x23\x07' * 8)
        (cache / 'in' / ('cd' * 32 + '.in')).write_bytes(b'TKSI' * 4)
        self.addCleanup(setattr, self.mg, 'KIT', self.mg.KIT)
        self.mg.KIT = kit
        with contextlib.redirect_stdout(io.StringIO()):
            self.mg.step_shaders(self.out)
        self.assertFalse((cache / 'shader-cache.pack').exists())
        shutil.copy(ROOT / 'tools' / 'pack_shader_cache.py', kit / 'tools')
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            self.mg.step_shaders(self.out)
        self.assertTrue((cache / 'shader-cache.pack').is_file())
        self.assertEqual(sorted(p.name for p in cache.iterdir()), ['shader-cache.pack'])
        self.assertIn('shader cache packed', buf.getvalue())

    def test_carried_update_packs_many_loose_shaders(self):
        kit = self.tmp / 'kit'
        (kit / 'tools').mkdir(parents=True)
        shutil.copy(ROOT / 'tools' / 'pack_shader_cache.py', kit / 'tools')
        cache = self.out / 'shader-cache'
        cache.mkdir(parents=True)
        for i in range(3):
            (cache / (f'{i:02x}' * 32 + '.spv')).write_bytes(b'\x03\x02\x23\x07' * 8)
        for name, value in (('KIT', kit), ('PACK_LOOSE_MIN', 4), ('shader_carry', lambda out: True)):
            self.addCleanup(setattr, self.mg, name, getattr(self.mg, name))
            setattr(self.mg, name, value)
        with contextlib.redirect_stdout(io.StringIO()):
            self.mg.step_shaders(self.out, update=True)
        self.assertFalse((cache / 'shader-cache.pack').exists())
        (cache / ('03' * 32 + '.spv')).write_bytes(b'\x03\x02\x23\x07' * 8)
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            self.mg.step_shaders(self.out, update=True)
        self.assertEqual(sorted(p.name for p in cache.iterdir()), ['shader-cache.pack'])
        self.assertIn('stay in use', buf.getvalue())
        self.assertIn('shader cache packed', buf.getvalue())

    def test_carried_update_warms_the_drivers_cache(self):
        calls = []
        for name, value in (('shader_carry', lambda out: True),
                            ('shader_run', lambda cmd, cwd=None, build_only=False: calls.append((cmd, cwd)) or self.rc)):
            self.addCleanup(setattr, self.mg, name, getattr(self.mg, name))
            setattr(self.mg, name, value)
        self.rc = 0
        self.out.mkdir(parents=True, exist_ok=True)
        with contextlib.redirect_stdout(io.StringIO()):
            self.mg.step_shaders(self.out, update=True)
        self.assertEqual(calls, [])
        (self.out / self.mg.EXE_NAME).write_bytes(b'MZ')
        with contextlib.redirect_stdout(io.StringIO()):
            self.mg.step_shaders(self.out, update=True)
        self.assertEqual(calls, [])
        (self.out / 'shader-prepared.bin').write_bytes(prepared_list(b'p' * 32, b'a' * 32, b't' * 32))
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            self.mg.step_shaders(self.out, update=True)
        self.assertEqual(calls[0][0][-1], '--no-run')
        self.assertEqual(calls[1:], [([str(self.out / self.mg.EXE_NAME), '--warm-shaders'], str(self.out))])
        self.assertIn("driver's cache warm", buf.getvalue())
        self.rc = 3
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            self.mg.step_shaders(self.out, update=True)
        self.assertIn('did not finish (3)', buf.getvalue())
        self.assertEqual(len(calls), 4)

    def test_prepared_list_says_what_it_was_prepared_from(self):
        self.out.mkdir(parents=True, exist_ok=True)
        self.assertIsNone(self.mg.prepared_from(self.out))
        lst = self.out / 'shader-prepared.bin'
        lst.write_bytes(b'NX2NSPRP' + struct.pack('<II', 1, 36) + bytes(36 * 3))
        self.assertEqual(self.mg.prepared_from(self.out), {})
        lst.write_bytes(prepared_list(b'p' * 32, b'a' * 32, b't' * 32)[:60])
        self.assertEqual(self.mg.prepared_from(self.out), {})
        lst.write_bytes(prepared_list(b'p' * 32, b'a' * 32, b't' * 32, records=2))
        self.assertEqual(self.mg.prepared_from(self.out),
                         {'programs': '70' * 32, 'answers': '61' * 32, 'translator': '74' * 32})

    def test_update_prepares_again_when_the_seed_changed(self):
        calls = []

        def run(cmd, cwd=None, build_only=False):
            calls.append(cmd[-1])
            if cmd[-1] == '--no-run':
                (self.out / 'seed-programs.bin').write_bytes(self.programs)
                (self.out / 'seed-answers.bin').write_bytes(self.answers)
            return 0
        for name, value in (('shader_carry', lambda out: True), ('shader_run', run),
                            ('pack_shader_cache', lambda out: None)):
            self.addCleanup(setattr, self.mg, name, getattr(self.mg, name))
            setattr(self.mg, name, value)
        self.out.mkdir(parents=True, exist_ok=True)
        (self.out / self.mg.EXE_NAME).write_bytes(b'MZ')
        self.programs, self.answers = b'NX2NSPRG programs 1', b'NX2NSANS answers 1'
        digest = lambda b: hashlib.sha256(b).digest()  # noqa: E731

        def update():
            calls.clear()
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                self.mg.step_shaders(self.out, update=True)
            self.assertFalse((self.out / 'seed-programs.bin').exists() or (self.out / 'seed-answers.bin').exists())
            return buf.getvalue()
        lst = self.out / 'shader-prepared.bin'
        lst.write_bytes(prepared_list(digest(self.programs), digest(self.answers), b't' * 32))
        self.assertIn('stay in use', update())
        self.assertEqual(calls, ['--no-run', '--warm-shaders'])
        lst.write_bytes(prepared_list(digest(self.programs), digest(self.answers), b'u' * 32))
        self.assertIn('stay in use', update())
        self.programs = b'NX2NSPRG programs 2'
        text = update()
        self.assertIn('the shader seed changed since the shaders were prepared: preparing them again', text)
        self.assertNotIn('stay in use', text)
        self.assertEqual(calls, ['--no-run', self.mg.EXE_NAME])
        lst.write_bytes(prepared_list(digest(self.programs), digest(b'other answers'), b't' * 32))
        self.assertIn('preparing them again', update())
        lst.write_bytes(b'NX2NSPRP' + struct.pack('<II', 1, 36) + bytes(36))
        self.assertIn('do not say which shader seed', update())
        self.assertEqual(calls, ['--no-run', self.mg.EXE_NAME])
        lst.write_bytes(prepared_list(digest(self.programs), digest(self.answers), b't' * 32))
        self.assertIn('stay in use', update())

    def test_romfs_failure_stops_the_build(self):
        ran, lifting, failed = [], threading.Event(), threading.Event()

        def romfs(src, out, jobs):
            lifting.wait(5)
            failed.set()
            self.mg.fail('the RomFS image could not be written')

        def lift(out, cache):
            lifting.set()
            failed.wait(5)
            time.sleep(0.1)
            ran.append('lift')

        def compile_(out, cache, msys, jobs, romfs_job=None, update=False):
            ran.append('compile')
        self.assertEqual(self.main_with(romfs, lift, compile_), 1)
        self.assertEqual(ran, ['lift'])
        steps = json.loads((self.out / 'build-cache' / 'state.json').read_text())['steps']
        self.assertNotIn('romfs', steps)

    def test_shaders_key_names_the_translator_the_build_uses(self):
        from unittest import mock
        kit = self.tmp / 'kit'
        (kit / 'tools').mkdir(parents=True)
        (kit / 'tools' / 'prepare_shaders.py').write_text('')
        xlate = kit / 'build' / 'xlate'
        xlate.mkdir(parents=True)
        (xlate / self.mg.XLATE_NAME).write_bytes(b'')
        (xlate / 'nx2navi_xlate.id').write_text('xlate 1\n')

        def key():
            return self.mg.step_key('shaders', 'game', self.out)
        with mock.patch.object(self.mg, 'KIT', kit):
            first = key()
            (xlate / 'nx2navi_xlate.id').write_text('xlate 2\n')
            self.assertNotEqual(key(), first)
            (kit / 'bin').mkdir()
            (kit / 'bin' / self.mg.XLATE_NAME).write_bytes(b'')
            (kit / 'bin' / 'nx2navi_xlate.id').write_text('xlate 3\n')
            from_bin = key()
            (xlate / 'nx2navi_xlate.id').write_text('xlate 4\n')
            self.assertEqual(key(), from_bin)
            shutil.rmtree(kit / 'bin')
            shutil.rmtree(xlate)
            self.assertTrue(key().endswith('|missing'))
            with contextlib.redirect_stderr(io.StringIO()) as err, self.assertRaises(SystemExit):
                self.mg.xlate_source()
            self.assertIn('tools/build_xlate.py', err.getvalue())

    def test_prompts_step(self):
        from unittest import mock
        kit = self.tmp / 'kit'
        (kit / 'tools').mkdir(parents=True)
        (kit / 'bin').mkdir()
        (kit / 'bin' / self.mg.XLATE_NAME).write_bytes(b'')
        (kit / 'bin' / 'nx2navi_xlate.id').write_text('xlate 1\n')
        (kit / 'tools' / 'prepare_shaders.py').write_text('')
        for name in ('prompt_art.py', 'button_icons.py'):
            shutil.copy2(ROOT / 'tools' / name, kit / 'tools' / name)
        (kit / 'tools' / 'pad_prompts.py').write_text(FAKE_PAD_PROMPTS)
        art = kit / 'prompt-art' / 'XGamepad' / 'Default'
        art.mkdir(parents=True)
        (art / 'T_X_A_White.png').write_bytes(b'picture one')
        (kit / 'prompt-art' / 'SOURCE.md').write_text('# Test art\n\nSource: Test pack 2 (CC0 1.0)\n')
        record = self.out / self.mg.PROMPTS_RECORD
        ran = []

        def fake(name):
            def step(*args, **kw):
                ran.append(name)
            return step

        def build():
            ran.clear()
            buf = io.StringIO()
            argv = ['make_game.py', '--nup', str(self.nup), '--out', str(self.out), '--msys', str(self.msys),
                    '--events']
            with mock.patch.object(sys, 'argv', argv), contextlib.redirect_stdout(buf), \
                    contextlib.redirect_stderr(io.StringIO()):
                self.mg.main()
            events = [json.loads(ln[2:]) for ln in buf.getvalue().splitlines() if ln.startswith('@@')]
            log = self.out / 'prompt-runs.txt'
            runs = log.read_text().count('run') if log.exists() else 0
            return sorted(ran), [e for e in events if e.get('step') == 'prompts'], runs

        def states(events):
            return [e['state'] for e in events if e['ev'] == 'step']

        sys.path.insert(0, str(self.layout.pydeps))
        self.addCleanup(sys.path.remove, str(self.layout.pydeps))
        with mock.patch.multiple(self.mg, KIT=kit, PROMPT_PACKAGES={}, EVENTS=False, step_romfs=fake('romfs'),
                                 step_lift=fake('lift'), step_compile=fake('compile'), step_scan=fake('scan'), step_shaders=fake('shaders'),
                                 preflight=lambda *a, **k: None):
            others, ev, runs = build()
            self.assertEqual((others, runs), (['compile', 'lift', 'romfs', 'scan', 'shaders'], 1))
            self.assertEqual(states(ev), ['start', 'done'])
            fractions = [e['fraction'] for e in ev if e['ev'] == 'progress']
            sets = len(self.mg.prompt_art.OVERLAYS)
            self.assertEqual(sets, 7)
            self.assertEqual(fractions, [round(i / sets, 4) for i in range(sets + 1)])
            details = [e['detail'] for e in ev if e['ev'] == 'progress']
            self.assertEqual(details[-1], f'Drew {sets} of {sets} sets of button prompts')
            self.assertIn('prompts', json.loads((self.out / 'build-cache' / 'state.json').read_text())['steps'])
            self.assertEqual(record.read_text(), 'drawn from Test pack 2 (CC0 1.0)\n')
            record.unlink()
            self.assertEqual(build(), ([], [{'ev': 'step', 'step': 'prompts', 'state': 'skip'}], 1))
            self.assertEqual(record.read_text(), 'drawn from Test pack 2 (CC0 1.0)\n')
            (self.out / 'nx2navi.ini').write_text('vsync = on\n')
            self.assertEqual(build()[2], 1)
            (self.out / 'nx2navi.ini').write_text('vsync = on\nbind_a = key:f   # jump\n')
            others, ev, runs = build()
            self.assertEqual((others, states(ev), runs), ([], ['start', 'done'], 2))
            shutil.rmtree(self.out / 'romfs-patches-pc')
            self.assertEqual(build()[2], 3)
            (art / 'T_X_B_White.png').write_bytes(b'picture two')
            self.assertEqual(build()[2], 4)
            st = json.loads((self.out / 'build-cache' / 'state.json').read_text())
            del st['steps']['lift']
            (self.out / 'build-cache' / 'state.json').write_text(json.dumps(st))
            others, ev, runs = build()
            self.assertEqual((others, states(ev), runs), (['compile', 'lift'], ['skip'], 4))
            (kit / 'prompt-art' / 'fail').write_bytes(b'')
            (art / 'T_X_Y_White.png').write_bytes(b'picture three')
            with self.assertRaises(SystemExit):
                build()
            self.assertEqual(record.read_text(), 'not drawn: drawing them failed (the build log says why)\n')
            (kit / 'prompt-art' / 'fail').unlink()
            self.assertEqual(build()[2], 5)
            self.assertEqual(record.read_text(), 'drawn from Test pack 2 (CC0 1.0)\n')
            shutil.rmtree(kit / 'prompt-art')
            self.assertEqual(build(), ([], [{'ev': 'step', 'step': 'prompts', 'state': 'skip'}], 5))
            self.assertTrue((self.out / 'romfs-patches-xbox' / 'manifest.tsv').is_file())
            self.assertTrue(record.read_text().startswith('not drawn: this version carries no button art ('))

    def test_an_update_keeps_the_settings_file(self):
        from unittest import mock
        kit = self.tmp / 'kit'
        for d in ('tools', 'bin', 'recomp'):
            (kit / d).mkdir(parents=True)
        (kit / 'VERSION').write_text('0.7.0\n')
        (kit / 'recomp' / 'gen.py').write_text('# the lifter of 0.7.0\n')
        (kit / 'bin' / self.mg.XLATE_NAME).write_bytes(b'translator')
        (kit / 'bin' / 'nx2navi_xlate.id').write_text('xlate 1\n')
        (kit / 'tools' / 'prepare_shaders.py').write_text('print("prepare-shaders: progress build 1 1", flush=True)\n')
        for name in ('prompt_art.py', 'button_icons.py'):
            shutil.copy2(ROOT / 'tools' / name, kit / 'tools' / name)
        (kit / 'tools' / 'pad_prompts.py').write_text(FAKE_PAD_PROMPTS)
        art = kit / 'prompt-art' / 'XGamepad' / 'Default'
        art.mkdir(parents=True)
        (art / 'T_X_A_White.png').write_bytes(b'picture one')
        ran = []

        def fake(name):
            def step(*args, **kw):
                ran.append(name)
            return step

        def build(*source):
            buf = io.StringIO()
            argv = ['make_game.py', *source, '--out', str(self.out), '--msys', str(self.msys), '--events']
            with mock.patch.object(sys, 'argv', argv), contextlib.redirect_stdout(buf), \
                    contextlib.redirect_stderr(io.StringIO()):
                self.mg.main()
            events = [json.loads(ln[2:]) for ln in buf.getvalue().splitlines() if ln.startswith('@@')]
            return [e['step'] for e in events if e['ev'] == 'step' and e['state'] == 'start']

        sys.path.insert(0, str(self.layout.pydeps))
        self.addCleanup(sys.path.remove, str(self.layout.pydeps))
        ini = self.out / 'nx2navi.ini'
        with mock.patch.multiple(self.mg, KIT=kit, PROMPT_PACKAGES={}, EVENTS=False, step_lift=fake('lift'),
                                 step_compile=fake('compile'), step_scan=fake('scan'),
                                 preflight=lambda *a, **k: None):
            self.assertEqual(build('--nup', str(self.nup)),
                             ['check', 'exefs', 'romfs', 'lift', 'compile', 'shaders', 'prompts', 'scan'])
            self.assertFalse(ini.exists())
            ini.write_bytes(PLAYER_INI)
            names = sorted(p.name for p in self.out.iterdir())
            (kit / 'VERSION').write_text('0.7.1\n')
            (kit / 'recomp' / 'gen.py').write_text('# the lifter of 0.7.1\n')
            ran.clear()
            steps = build('--rebuild')
            self.assertEqual(steps, ['check', 'lift', 'compile', 'shaders', 'prompts'])
            self.assertEqual(ran, ['lift', 'compile'])
            self.assertEqual(ini.read_bytes(), PLAYER_INI)
            self.assertEqual(sorted(p.name for p in self.out.iterdir()), names)
            self.assertEqual(json.loads((self.out / 'build-cache' / 'state.json').read_text())['kit'], '0.7.1')
            self.assertEqual(build('--rebuild'), ['check'])
            self.assertEqual(ini.read_bytes(), PLAYER_INI)
            st = json.loads((self.out / 'build-cache' / 'state.json').read_text())
            st['steps'] = {}
            (self.out / 'build-cache' / 'state.json').write_text(json.dumps(st))
            self.assertEqual(build('--nup', str(self.nup)),
                             ['check', 'exefs', 'romfs', 'lift', 'compile', 'shaders', 'prompts', 'scan'])
            self.assertEqual(ini.read_bytes(), PLAYER_INI)

    @unittest.skipIf(os.name == 'nt', 'uses a POSIX script as the compiler')
    def test_largest_source_compiles_first(self):
        src, obj = self.tmp / 'src', self.tmp / 'obj'
        src.mkdir()
        obj.mkdir()
        for name, size in (('small.c', 10), ('big.c', 5000), ('mid.c', 800)):
            (src / name).write_text('/*' + 'x' * size + '*/\n')
        order = self.tmp / 'order.txt'
        cc = self.tmp / 'fakecc'
        cc.write_text(f'#!{sys.executable}\nimport sys\nargs = sys.argv[1:]\n'
                      'dep, out, s = args[args.index("-MF") + 1], args[args.index("-o") + 1], args[-1]\n'
                      f'open({str(order)!r}, "a").write(s.rsplit("/", 1)[-1] + "\\n")\n'
                      'open(out, "w").close(); open(dep, "w").write(out + ": " + s + "\\n")\n')
        cc.chmod(0o755)
        srcs = [(src / n, obj / (n[:-2] + '.o')) for n in ('small.c', 'big.c', 'mid.c')]
        cache = self.mg.ObjectCache(obj / 'objects.json')
        compiled, reused, bad = self.mg.compile_all(srcs, lambda s, d: [str(cc), '-c', '-o', str(d), str(s)], cache,
                                                    1, dict(os.environ))
        self.assertEqual((compiled, bad), (3, []))
        self.assertEqual(order.read_text().split(), ['big.c', 'mid.c', 'small.c'])

    def fake_compiles(self, count, held=None):
        src, obj = self.tmp / 'src', self.tmp / 'obj'
        src.mkdir()
        obj.mkdir()
        srcs = []
        for i in range(count):
            (src / f'f{i:02d}.c').write_text(f'int f{i};' + ' ' * (10 * i) + '\n')
            srcs.append((src / f'f{i:02d}.c', obj / f'f{i:02d}.o'))

        def run(cmd, **kw):
            dep, out, source = cmd[cmd.index('-MF') + 1], cmd[cmd.index('-o') + 1], cmd[-1]
            if held and Path(source).name == held[0]:
                held[1].wait(20)
            Path(out).write_bytes(b'object')
            Path(dep).write_text(out.replace('\\', '/') + ': ' + source.replace('\\', '/') + '\n')
            return mock.Mock(returncode=0, stderr='')
        return srcs, run, obj

    def test_a_cut_compile_keeps_its_finished_objects(self):
        srcs, run, obj = self.fake_compiles(60)
        on_disk = []

        class Cut(Exception):
            pass

        def cut_at_40(done, total):
            path = obj / 'objects.json'
            on_disk.append(len(json.loads(path.read_text())) if path.is_file() else 0)
            if done == 40:
                raise Cut

        def go(progress=None):
            cache = self.mg.ObjectCache(obj / 'objects.json')
            return self.mg.compile_all(srcs, lambda s, d: ['cc', '-c', '-o', str(d), str(s)], cache, 1,
                                       dict(os.environ), progress)
        with mock.patch.object(self.mg.subprocess, 'run', run), mock.patch.object(self.mg, 'CACHE_SAVE_PAUSE', 0):
            with self.assertRaises(Cut):
                go(cut_at_40)
            self.assertEqual(on_disk[24:27], [0, 25, 25])
            self.assertFalse((obj / 'objects.tmp').exists())
            self.assertEqual(go()[:2], (35, 25))
            self.assertEqual(go()[:2], (0, 60))

    def test_objects_are_recorded_while_the_largest_file_compiles(self):
        release = threading.Event()
        srcs, run, obj = self.fake_compiles(30, held=('f29.c', release))
        seen = []

        def progress(done, total):
            path = obj / 'objects.json'
            if not release.is_set() and path.is_file() and len(json.loads(path.read_text())) >= 25:
                seen.append(done)
                release.set()
        cache = self.mg.ObjectCache(obj / 'objects.json')
        with mock.patch.object(self.mg.subprocess, 'run', run), mock.patch.object(self.mg, 'CACHE_SAVE_PAUSE', 0):
            compiled, reused, bad = self.mg.compile_all(srcs, lambda s, d: ['cc', '-c', '-o', str(d), str(s)], cache,
                                                        4, dict(os.environ), progress)
        self.assertEqual((compiled, reused, bad), (30, 0, []))
        self.assertTrue(seen, 'nothing was saved before the largest file finished')
        self.assertEqual(len(json.loads((obj / 'objects.json').read_text())), 30)

    def test_the_default_job_count_follows_the_processors(self):
        self.assertEqual(self.mg.default_jobs(24), (24, '24 processors'))
        with mock.patch.object(self.mg.os, 'cpu_count', return_value=12):
            self.assertEqual(self.mg.default_jobs(), (12, '12 processors'))

    def test_the_build_uses_the_default_job_count(self):
        made_up_game(self.out)
        seen = []

        def compile_(out, cache, msys, jobs, romfs_job=None, update=False):
            seen.append(jobs)
            self.mg.fail('stop here')
        with mock.patch.object(self.mg, 'default_jobs', lambda: (3, 'stubbed')):
            code, text = self.main_rebuild(lambda out, cache: None, compile_)
            self.assertIn('compiles at a time: up to 3 (stubbed)', text)
            code, text = self.main_rebuild(lambda out, cache: None, compile_, '--jobs', '5')
            self.assertIn('compiles at a time: up to 5 (--jobs)', text)
        self.assertEqual(seen, [3, 5])

    def test_runtime_split(self):
        tainted = self.mg.per_install_names()
        self.assertTrue({'layout.h', 'patches.h', 'romfs_config.h'} <= tainted)
        names = {p.name: self.mg.per_install(p, tainted) for p, _ in self.mg.runtime_sources()}
        for f in ('loader.c', 'memory.c', 'romfs.c', 'patches.c'):
            self.assertTrue(names[f], f)
        self.assertFalse(names['vk_device.c'])
        self.assertLess(sum(names.values()), len(names) // 4)

    def test_parse_deps(self):
        text = 'C:/o/x.o: C:/src/x.c C:/inc/a\\ b.h \\\n  C:/inc/c.h\n'
        self.assertEqual(self.mg.parse_deps(text), ['C:/src/x.c', 'C:/inc/a b.h', 'C:/inc/c.h'])

    def test_copy_if_changed_equal_metadata(self):
        modes = (0o666,) if os.name == 'nt' else (0o444, 0o555)
        for mode in modes:
            with self.subTest(mode=oct(mode)):
                src, dst = self.tmp / f'source-{mode}.id', self.tmp / f'installed-{mode}.id'
                for path, data in ((src, b'7' * 64 + b'\n'), (dst, b'1' * 64 + b'\n')):
                    path.write_bytes(data)
                    os.utime(path, (1, 1))
                    path.chmod(mode)
                self.assertEqual(src.stat().st_size, dst.stat().st_size)
                self.assertEqual(src.stat().st_mtime_ns, dst.stat().st_mtime_ns)
                self.assertTrue(self.mg.copy_if_changed(src, dst))
                self.assertEqual(dst.read_bytes(), src.read_bytes())
                self.assertEqual(dst.stat().st_mode & 0o777, src.stat().st_mode & 0o777)
                self.assertFalse(self.mg.copy_if_changed(src, dst))

    def test_copy_if_changed_makes_no_folder(self):
        out = self.tmp / 'share'
        out.mkdir()
        src, dst = self.tmp / 'new.bin', out / 'installed.bin'
        src.write_bytes(b'new')
        dst.write_bytes(b'old')
        refused = mock.patch.object(self.mg.tempfile, 'TemporaryDirectory', side_effect=PermissionError('refused'))
        with refused, mock.patch.object(self.mg.os, 'mkdir', side_effect=PermissionError('refused')):
            self.assertTrue(self.mg.copy_if_changed(src, dst))
        self.assertEqual(dst.read_bytes(), b'new')
        self.assertEqual([p.name for p in out.iterdir()], ['installed.bin'])
        src.write_bytes(b'newer')
        with mock.patch.object(self.mg.shutil, 'copy2', side_effect=OSError('the share went away')):
            with self.assertRaises(OSError):
                self.mg.copy_if_changed(src, dst)
        self.assertEqual(dst.read_bytes(), b'new')
        self.assertEqual([p.name for p in out.iterdir()], ['installed.bin'])

    def test_source_size_waits_for_a_share(self):
        src = self.tmp / 'lift' / 'a.c'
        src.parent.mkdir()
        src.write_bytes(b'int a;\n')
        real_stat, real_is_dir = Path.stat, Path.is_dir
        down = {'left': 3}

        def stat(p, *a, **k):
            if p == src and down['left'] > 0:
                raise FileNotFoundError(2, 'The network path was not found', str(p))
            return real_stat(p, *a, **k)

        def is_dir(p):
            if p == src.parent and down['left'] > 0:
                down['left'] -= 1
                return False
            return real_is_dir(p)

        lines = []
        with mock.patch.object(Path, 'stat', stat), mock.patch.object(Path, 'is_dir', is_dir), \
                mock.patch.object(self.mg.time, 'sleep') as sleep, mock.patch.object(self.mg, 'say', lines.append):
            self.assertEqual(self.mg.source_size(src), 7)
        self.assertEqual(sleep.call_count, 3)
        self.assertEqual(len(lines), 1)
        down['left'] = 10 ** 6
        clock = iter(range(0, 10 ** 6, 5))
        with mock.patch.object(Path, 'stat', stat), mock.patch.object(Path, 'is_dir', is_dir), \
                mock.patch.object(self.mg.time, 'sleep'), mock.patch.object(self.mg, 'say'), \
                mock.patch.object(self.mg.time, 'monotonic', lambda: next(clock)), \
                mock.patch.object(self.mg, 'fail', side_effect=SystemExit) as fail:
            with self.assertRaises(SystemExit):
                self.mg.source_size(src)
        self.assertIn('stopped answering', fail.call_args[0][0])
        down['left'] = 0
        src.unlink()
        with mock.patch.object(self.mg.time, 'sleep') as sleep, \
                mock.patch.object(self.mg, 'fail', side_effect=SystemExit) as fail:
            with self.assertRaises(SystemExit):
                self.mg.source_size(src)
        sleep.assert_not_called()
        self.assertIn('lift folder', fail.call_args[0][0])

    def test_out_path_keeps_a_drive_letter(self):
        with mock.patch.object(self.mg, 'WIN', True), \
                mock.patch.object(Path, 'resolve', side_effect=AssertionError('resolve() was called')):
            got = self.mg.out_path('game')
        self.assertEqual(got, Path(os.path.abspath('game')))
        self.assertTrue(got.is_absolute())
        with mock.patch.object(self.mg, 'WIN', False):
            self.assertEqual(self.mg.out_path(self.tmp / 'x' / '..' / 'game'), (self.tmp / 'game').resolve())

    @unittest.skipUnless(shutil.which('cc'), 'needs a C compiler')
    def test_object_cache(self):
        src = self.tmp / 'src'
        src.mkdir()
        (src / 'h.h').write_text('#define V 1\n')
        (src / 'a.c').write_text('#include "h.h"\nint a(void) { return V; }\n')
        (src / 'b.c').write_text('int b(void) { return 2; }\n')
        obj = self.tmp / 'obj'
        obj.mkdir()
        srcs = [(src / 'a.c', obj / 'a.o'), (src / 'b.c', obj / 'b.o')]

        def cmd_for(s, d):
            return ['cc', '-c', '-I', str(src), '-o', str(d), str(s)]

        def go():
            cache = self.mg.ObjectCache(obj / 'objects.json')
            return self.mg.compile_all(srcs, cmd_for, cache, 2, dict(os.environ), salt='cc')
        self.assertEqual(go()[:2], (2, 0))
        self.assertEqual(go()[:2], (0, 2))
        (src / 'h.h').write_text('#define V 3\n')
        self.assertEqual(go()[:2], (1, 1))
        (src / 'b.c').write_text('int b(void) { return syntax error }\n')
        compiled, reused, bad = go()
        self.assertEqual((compiled, len(bad)), (1, 1))
        self.assertNotIn('b.o', json.loads((obj / 'objects.json').read_text()))

    def test_dlss_beside_the_game(self):
        import dlss_pin
        from unittest import mock
        kit, out = self.tmp / 'kit', self.tmp / 'game'
        (kit / 'bin').mkdir(parents=True)
        out.mkdir()

        def kit_dll(data, when):
            (kit / 'bin' / 'nvngx_dlss.dll').write_bytes(data)
            os.utime(kit / 'bin' / 'nvngx_dlss.dll', (when, when))
            (kit / 'bin' / 'nvngx_dlss-NOTICE.txt').write_text(f'NVIDIA DLSS\n\nsha256 {hashlib.sha256(data).hexdigest()}\n')

        dll, note = out / 'nvngx_dlss.dll', out / 'nvngx_dlss-NOTICE.txt'
        with mock.patch.object(self.mg, 'KIT', kit):
            self.assertIsNone(self.mg.place_dlss(out))
            self.assertFalse(dll.exists())
            kit_dll(b'dlss one', 1_700_000_000)
            self.assertIsNone(self.mg.place_dlss(out))
            self.assertEqual(dll.read_bytes(), b'dlss one')
            self.assertTrue(note.is_file())
            self.assertIsNone(self.mg.place_dlss(out))
            kit_dll(b'dlss two', 1_700_100_000)
            self.assertIsNone(self.mg.place_dlss(out))
            self.assertEqual(dll.read_bytes(), b'dlss two')
            self.assertIn(hashlib.sha256(b'dlss two').hexdigest(), note.read_text())
            dll.write_bytes(b"the player's own DLSS")
            self.assertIn('kept', self.mg.place_dlss(out))
            self.assertEqual(dll.read_bytes(), b"the player's own DLSS")
        (self.tmp / 'n.txt').write_text(dlss_pin.notice_text(), encoding='utf-8')
        self.assertEqual(self.mg.notice_sha256(self.tmp / 'n.txt'), dlss_pin.SHA256)
        self.assertEqual((dlss_pin.NAME, dlss_pin.NOTICE), (self.mg.DLSS_DLL, self.mg.DLSS_NOTICE))

    def test_dlss_library_beside_the_game_on_linux(self):
        from unittest import mock
        kit, out = self.tmp / 'kit', self.tmp / 'game'
        (kit / 'bin').mkdir(parents=True)
        out.mkdir()
        note = out / 'libnvidia-ngx-dlss-NOTICE.txt'

        def kit_lib(version, data, when):
            for old in (kit / 'bin').glob('libnvidia-ngx-dlss.so.*'):
                old.unlink()
            lib = kit / 'bin' / f'libnvidia-ngx-dlss.so.{version}'
            lib.write_bytes(data)
            os.utime(lib, (when, when))
            (kit / 'bin' / 'libnvidia-ngx-dlss-NOTICE.txt').write_text(
                f'NVIDIA DLSS {version}\n\nsha256 {hashlib.sha256(data).hexdigest()}\n')

        def beside(folder=out):
            return sorted(p.name for p in folder.iterdir())

        with mock.patch.object(self.mg, 'KIT', kit):
            self.assertIsNone(self.mg.place_dlss(out, linux=True))
            self.assertEqual(beside(), [])
            (kit / 'bin' / 'nvngx_dlss.dll').write_bytes(b'the Windows DLL')
            self.assertIsNone(self.mg.place_dlss(out, linux=True))
            self.assertEqual(beside(), [])
            kit_lib('310.9.1', b'dlss one', 1_700_000_000)
            self.assertIsNone(self.mg.place_dlss(out, linux=True))
            self.assertEqual(beside(), ['libnvidia-ngx-dlss-NOTICE.txt', 'libnvidia-ngx-dlss.so.310.9.1'])
            self.assertEqual((out / 'libnvidia-ngx-dlss.so.310.9.1').read_bytes(), b'dlss one')
            self.assertIn(hashlib.sha256(b'dlss one').hexdigest(), note.read_text())
            self.assertIsNone(self.mg.place_dlss(out, linux=True))
            self.assertEqual(len(beside()), 2)
            kit_lib('310.10.0', b'dlss two', 1_700_100_000)
            self.assertIsNone(self.mg.place_dlss(out, linux=True))
            self.assertEqual(beside(), ['libnvidia-ngx-dlss-NOTICE.txt', 'libnvidia-ngx-dlss.so.310.10.0'])
            self.assertEqual((out / 'libnvidia-ngx-dlss.so.310.10.0').read_bytes(), b'dlss two')
            self.assertIn(hashlib.sha256(b'dlss two').hexdigest(), note.read_text())
            mine = self.tmp / 'mine'
            mine.mkdir()
            (mine / 'libnvidia-ngx-dlss.so.310.4.0').write_bytes(b"the player's own DLSS")
            self.assertIn('libnvidia-ngx-dlss.so.310.4.0 kept', self.mg.place_dlss(mine, linux=True))
            self.assertEqual(beside(mine), ['libnvidia-ngx-dlss.so.310.4.0'])
            (mine / 'libnvidia-ngx-dlss.so.310.4.0').unlink()
            (mine / 'libnvidia-ngx-dlss.so.310.10.0').write_bytes(b"the player's own build")
            self.assertIn('kept', self.mg.place_dlss(mine, linux=True))
            self.assertEqual((mine / 'libnvidia-ngx-dlss.so.310.10.0').read_bytes(), b"the player's own build")
            (out / 'libnvidia-ngx-dlss.so.310.4.0').write_bytes(b"the player's own DLSS")
            self.assertIn('kept', self.mg.place_dlss(out, linux=True))
            self.assertEqual(len(beside()), 3)
            win = self.tmp / 'win'
            win.mkdir()
            self.assertIsNone(self.mg.place_dlss(win))
            self.assertEqual(beside(win), ['nvngx_dlss.dll'])

    def test_dlss_pin_for_linux(self):
        import dlss_pin
        from unittest import mock
        p = dlss_pin.pin(linux=True)
        self.assertEqual(p.name, f'libnvidia-ngx-dlss.so.{dlss_pin.VERSION}')
        self.assertTrue(p.name.startswith(self.mg.DLSS_SO_PREFIX))
        self.assertFalse(p.notice.startswith(self.mg.DLSS_SO_PREFIX))
        self.assertEqual(p.notice, self.mg.DLSS_SO_NOTICE)
        self.assertEqual(dlss_pin.pin().name, dlss_pin.NAME)
        self.assertNotEqual((p.size, p.sha256), (dlss_pin.SIZE, dlss_pin.SHA256))
        (self.tmp / 'n.txt').write_text(dlss_pin.notice_text(linux=True), encoding='utf-8')
        self.assertEqual(self.mg.notice_sha256(self.tmp / 'n.txt'), p.sha256)
        self.assertIn(p.name, dlss_pin.notice_text(linux=True))
        self.assertNotIn('nvngx_dlss.dll', dlss_pin.notice_text(linux=True))
        data = b'a DLSS library'
        lib, cache = self.tmp / 'file.so', self.tmp / 'dl'
        lib.write_bytes(data)
        fetched = []

        def download(url, dest, sha256=None, size=None):
            fetched.append((url, Path(dest).name, sha256, size))
            Path(dest).parent.mkdir(parents=True, exist_ok=True)
            Path(dest).write_bytes(data)
            return dest

        pinned = dict(LINUX_SIZE=len(data), LINUX_SHA256=hashlib.sha256(data).hexdigest(),
                      SIZE=len(data) + 1, SHA256=hashlib.sha256(data + b'!').hexdigest())
        with mock.patch.multiple(dlss_pin, **pinned), mock.patch('launcher.net.download', download), \
                contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(dlss_pin.resolve(lib, cache, linux=True), lib)
            with self.assertRaises(SystemExit):
                dlss_pin.resolve(lib, cache)
            lib.write_bytes(b'another DLSS!')
            with self.assertRaises(SystemExit):
                dlss_pin.resolve(lib, cache, linux=True)
            with self.assertRaises(SystemExit):
                dlss_pin.resolve(self.tmp / 'missing.so', cache, linux=True)
            self.assertEqual(fetched, [])
            self.assertEqual(dlss_pin.resolve(None, cache, linux=True), cache / p.name)
            self.assertEqual(fetched, [(p.url, p.name, hashlib.sha256(data).hexdigest(), len(data))])
            self.assertEqual(dlss_pin.resolve(None, cache), cache / f'nvngx_dlss-{dlss_pin.VERSION}.dll')
            self.assertEqual(fetched[1][:2], (dlss_pin.URL, f'nvngx_dlss-{dlss_pin.VERSION}.dll'))
            bin_dir = self.tmp / 'bin'
            bin_dir.mkdir()
            dlss_pin.place(cache / p.name, bin_dir, linux=True)
            self.assertEqual((bin_dir / p.notice).read_text(encoding='utf-8'), dlss_pin.notice_text(linux=True))
        self.assertEqual(sorted(x.name for x in bin_dir.iterdir()), sorted([p.notice, p.name]))
        self.assertEqual((bin_dir / p.name).read_bytes(), data)

    def test_compile_key_ignores_file_time_and_sysroot(self):
        exe = self.tmp / 'cc.exe'
        shutil.copy2(sys.executable, exe)
        a = self.mg.compiler_ids(exe, dict(os.environ))
        os.utime(exe, (1_700_000_000, 1_700_000_000))
        b = self.mg.compiler_ids(exe, dict(os.environ))
        self.assertEqual(a[0], b[0])
        self.assertNotEqual(a[1], b[1])
        src, dst = self.tmp / 'a.c', self.tmp / 'a.o'
        src.write_text('int a;')
        cache = self.mg.ObjectCache(self.tmp / 'objects.json', {'<kit>': self.tmp / 'kit'})
        plain = ['clang.exe', '-c', '-O1', '-o', str(dst), str(src)]
        fast = ['clang.exe', '--sysroot=C:/msys64/ucrt64', '-c', '-O1', '-o', str(dst), str(src)]
        self.assertEqual(self.mg.object_key(plain, src, dst, cache, 'id'), self.mg.object_key(fast, src, dst, cache, 'id'))
        self.assertNotEqual(self.mg.object_key(plain, src, dst, cache, 'id', sysroot=True),
                            self.mg.object_key(fast, src, dst, cache, 'id', sysroot=True))

    def test_vulkan_headers_go_first_for_the_runtime_only(self):
        inc = ['-I', 'rt', '-I', '/usr/include']
        with_vk = self.mg.runtime_cmd('clang', 'clang++', inc, 3, first=['-I', 'kit/vulkan'])
        without = self.mg.runtime_cmd('clang', 'clang++', inc, 3)
        guest = (Path('lift/main_0001.c'), Path('obj/main_main_0001.o'))
        self.assertEqual(with_vk(*guest), without(*guest))
        for src in (Path('rt/vk_device.c'), Path('rt/ui/menu.cpp')):
            cmd, plain = with_vk(src, Path('obj/rt_x.o')), without(src, Path('obj/rt_x.o'))
            self.assertEqual(cmd.count('-I'), plain.count('-I') + 1)
            self.assertLess(cmd.index('kit/vulkan'), cmd.index('rt'))
            self.assertLess(cmd.index('kit/vulkan'), cmd.index('/usr/include'))

    def test_early_check_tries_the_runtime_before_the_long_steps(self):
        srcs = [self.tmp / n for n in ('good.c', 'bad1.c', 'bad2.c', 'bad3.c', 'bad4.cpp')]
        for s in srcs:
            s.write_text('int x;\n')
        ran = []

        def compiler(errors):
            def run(cmd, **kw):
                ran.append([str(c) for c in cmd])
                name = next((Path(c).name for c in cmd if Path(str(c)).name.startswith(('good', 'bad'))), '')
                bad = name.startswith('bad')
                return subprocess.CompletedProcess(cmd, 1 if bad else 0, '', errors.get(name, 'error: old') if bad else '')
            return run

        def check(errors, sources):
            ran.clear()
            err = io.StringIO()
            with mock.patch.multiple(self.mg, toolchain=lambda msys: (Path('clang'), Path('clang++'), {}, Path('vma')),
                                     clang_version=lambda clang, env: 'clang version 18.1.3',
                                     vulkan_headers=lambda clang, env: ['-I', 'kit/vulkan'],
                                     sdl3_choice=lambda clang, env: self.mg.Sdl3('system', '3.4.18', [], '-lSDL3',
                                                                                 'its headers are found'),
                                     runtime_headers=lambda rt: None, EVENTS=False), \
                    mock.patch.object(self.mg.subprocess, 'run', compiler(errors)), \
                    contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(err):
                try:
                    self.mg.preflight(self.msys, 2, sources)
                    return None, err.getvalue()
                except SystemExit:
                    return 'stopped', err.getvalue()

        stopped, text = check({}, srcs)
        self.assertEqual(stopped, 'stopped')
        self.assertIn('did not compile: bad1.c, bad2.c, bad3.c, bad4.cpp', text)
        self.assertIn('4 of the runtime\'s 5 files', text)
        self.assertIn('clang version 18.1.3', text)
        self.assertIn('kit/vulkan', text)
        compiles = [c for c in ran if '-fsyntax-only' in c]
        self.assertEqual(len(compiles), 5)
        for c in compiles:
            self.assertEqual(c[1], '-fsyntax-only')
            self.assertFalse({'-c', '-o'} & set(c))
            self.assertLess(c.index('kit/vulkan'), c.index(str(self.mg.KIT / 'runtime')))
        self.assertEqual(len(ran), 5)
        stopped, text = check({'bad1.c': "bad1.c:1:10: fatal error: 'SDL3/SDL.h' file not found"}, srcs[:2])
        self.assertIn('These headers are missing: SDL3/SDL.h', text)
        stopped, text = check({}, srcs[:1])
        self.assertIsNone(stopped)
        link, probe = ran[-2], ran[-1]
        self.assertNotIn('-fsyntax-only', link)
        self.assertEqual(link[-len(self.mg.LINK_LIBS):], self.mg.LINK_LIBS)
        self.assertIn('-Wl,-rpath,$ORIGIN', link)
        self.assertEqual(probe, [link[link.index('-o') + 1]])

    def test_pinned_vulkan_headers(self):
        import vulkan_pin
        buf = io.BytesIO()
        with tarfile.open(fileobj=buf, mode='w:gz') as tar:
            for name, data in (('include/vulkan/vulkan_core.h', b'#define VK_HEADER_VERSION 363\n'),
                               ('include/vulkan/vulkan.h', b'#include "vulkan_core.h"\n'),
                               ('include/vk_video/vulkan_video_codecs_common.h', b'\n'),
                               ('include/vulkan/vulkan.hpp', b'// C++\n'), ('registry/vk.xml', b'<registry/>'),
                               ('include/vulkan/../../outside.h', b'x'), ('LICENSE.md', b'Apache-2.0 OR MIT\n')):
                info = tarfile.TarInfo(vulkan_pin.TOP + name)
                info.size = len(data)
                tar.addfile(info, io.BytesIO(data))
        archive = self.tmp / 'headers.tar.gz'
        archive.write_bytes(buf.getvalue())
        dest = self.tmp / 'toolchain' / 'vulkan-headers'
        with self.assertRaises(SystemExit), contextlib.redirect_stdout(io.StringIO()):
            vulkan_pin.resolve(dest, archive)
        self.assertFalse(vulkan_pin.have(dest))
        pinned = (vulkan_pin.SOURCE[0], hashlib.sha256(buf.getvalue()).hexdigest())
        with mock.patch.object(vulkan_pin, 'SOURCE', pinned):
            self.assertEqual(vulkan_pin.resolve(dest, archive), dest)
        files = sorted(f.relative_to(dest).as_posix() for f in dest.rglob('*') if f.is_file())
        self.assertEqual(files, ['LICENSE.md', 'include/vk_video/vulkan_video_codecs_common.h',
                                 'include/vulkan/vulkan.h', 'include/vulkan/vulkan_core.h', vulkan_pin.STAMP])
        self.assertFalse((self.tmp / 'outside.h').exists() or (self.tmp / 'toolchain' / 'outside.h').exists())
        self.assertTrue(vulkan_pin.have(dest))
        self.assertEqual(vulkan_pin.resolve(dest), dest)
        self.assertEqual(vulkan_pin.header_version((dest / 'include' / 'vulkan' / 'vulkan_core.h').read_text()), 363)
        self.assertEqual(vulkan_pin.header_version('#define VK_HEADER_VERSION_COMPLETE 1\n'), 0)
        (dest / vulkan_pin.STAMP).unlink()
        self.assertFalse(vulkan_pin.have(dest))

    def test_embedded_file_as_a_header_of_bytes(self):
        import build_shaders
        rt = self.tmp / 'runtime'
        font = rt / 'ui' / 'fonts' / 'Roboto-Medium.ttf'
        font.parent.mkdir(parents=True)
        data = bytes(range(256)) * 3 + b'\r\n\x00tail'
        font.write_bytes(data)
        header = rt / 'ui' / 'roboto_ttf.h'
        with mock.patch.object(build_shaders, 'ASSEMBLED', {}), contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(build_shaders.stale(rt), [font])
            self.assertEqual(build_shaders.build(rt), 1)
            self.assertEqual(build_shaders.stale(rt), [])
            numbers = ''.join(ln for ln in header.read_text(encoding='utf-8').splitlines() if not ln.startswith('//'))
            self.assertEqual(bytes(int(n) for n in numbers.split(',') if n), data)
            font.write_bytes(data + b'!')
            self.assertEqual(build_shaders.stale(rt), [font])
        menu = (ROOT / 'runtime' / 'ui' / 'menu.cpp').read_text(encoding='utf-8')
        self.assertIn('#ifdef __has_embed\n#embed "fonts/Roboto-Medium.ttf"\n#else\n#include "roboto_ttf.h"\n#endif', menu)
        self.assertEqual(build_shaders.EMBEDDED, {'ui/fonts/Roboto-Medium.ttf': 'ui/roboto_ttf.h',
                                                  'third_party/smaa/AreaTex.bin': 'smaa_area_tex.h',
                                                  'third_party/smaa/SearchTex.bin': 'smaa_search_tex.h'})

    def test_objects_of_an_earlier_version_are_taken_over(self):
        old, new, lift, obj = self.tmp / 'app' / '0.5.0', self.tmp / 'app' / '0.6.1', self.tmp / 'lift', self.tmp / 'obj'
        for d in (new / 'runtime', lift, obj):
            d.mkdir(parents=True)
        (new / 'runtime' / 'cpu.h').write_text('#define CPU 1\n')
        src, dst = lift / 'main_0000.c', obj / 'main_main_0000.o'
        src.write_text('int f(void) { return 1; }\n')
        dst.write_bytes(b'object')
        sha = lambda p: hashlib.sha256(Path(p).read_bytes()).hexdigest()
        legacy = {'key': 'old-key', 'deps': {str(src).replace('\\', '/'): sha(src),
                                             str(old / 'runtime' / 'cpu.h').replace('\\', '/'): sha(new / 'runtime' / 'cpu.h')}}
        (obj / 'objects.json').write_text(json.dumps({dst.name: legacy}))
        cache = self.mg.ObjectCache(obj / 'objects.json', {'<kit>': new})
        self.assertEqual(cache.adopt(dst, 'new-key', ['other-key'], [old]), 0)
        self.assertEqual(cache.adopt(dst, 'new-key', ['old-key'], [old]), 1)
        e = cache.entries[dst.name]
        self.assertEqual(e['key'], 'new-key')
        self.assertIn('<kit>/runtime/cpu.h', e['deps'])
        self.assertTrue(cache.current(dst, 'new-key'))
        (obj / 'objects.json').write_text(json.dumps({dst.name: legacy}))
        (new / 'runtime' / 'cpu.h').write_text('#define CPU 2\n')
        cache = self.mg.ObjectCache(obj / 'objects.json', {'<kit>': new})
        self.assertEqual(cache.adopt(dst, 'new-key', ['old-key'], [old]), 0)

    def test_lifted_level_kept_on_updates(self):
        obj = self.tmp / 'obj'
        obj.mkdir()
        self.assertEqual(self.mg.lifted_level(obj, 3, update=False), 3)
        self.assertEqual(self.mg.lifted_level(obj, 3, update=True), 3)
        (obj / 'objects.json').write_text(json.dumps({'main_main_0000.o': {'key': 'k', 'deps': {'a.c': 'ab'}}}))
        self.assertEqual(self.mg.lifted_level(obj, 3, update=True), 0)
        (obj / 'objects.json').write_text(json.dumps({
            'main_main_0000.o': {'key': 'k', 'deps': {'a.c': ['ab', 1, 2]}}, 'rt_amiibo.o': {'key': 'k', 'deps': {'b.c': 'cd'}}}))
        self.assertEqual(self.mg.lifted_level(obj, 3, update=True), 3)
        cache = self.mg.ObjectCache(obj / 'objects.json')
        self.assertEqual(cache.keep_only({'main_main_0000.o'}), 1)
        self.assertEqual(list(cache.entries), ['main_main_0000.o'])
        (obj / 'objects.json').write_text(json.dumps({'main_main_0000.o': {'key': 'k', 'deps': {'a.c': 'ab'}}}))
        (obj / self.mg.LIFTED_BUILD).write_text(json.dumps({'level': 2}))
        self.assertEqual(self.mg.lifted_level(obj, 3, update=True), 2)
        self.assertEqual(self.mg.lifted_level(obj, 1, update=True), 1)
        self.assertEqual(self.mg.lifted_level(obj, 3, update=False), 3)

    def test_retune_due_when_the_level_differs(self):
        obj = self.tmp / 'obj'
        obj.mkdir()
        with mock.patch.object(self.mg, 'cpu_level', lambda clang, env: 3),                 mock.patch.object(self.mg, 'toolchain', lambda msys: (None, None, {}, None)):
            self.assertTrue(self.mg.retune_due(obj, None))
            (obj / self.mg.LIFTED_BUILD).write_text(json.dumps({'level': 0}))
            self.assertTrue(self.mg.retune_due(obj, None))
            (obj / self.mg.LIFTED_BUILD).write_text(json.dumps({'level': 3}))
            self.assertFalse(self.mg.retune_due(obj, None))

    def test_updates_skip_shader_preparation(self):
        out = self.tmp / 'game'
        (out / 'shader-cache').mkdir(parents=True)
        self.assertFalse(self.mg.shader_carry(out))
        (out / 'shader-prepared.bin').write_bytes(b'x')
        self.assertFalse(self.mg.shader_carry(out))
        (out / 'shader-cache' / 'identities.txt').write_text('# translators\n' + 'ab' * 32 + '\n')
        self.assertTrue(self.mg.shader_carry(out))
        (out / 'nx2navi.ini').write_text('[performance]\nNX2SARIA_SHADER_CARRY = "0"\n')
        self.assertFalse(self.mg.shader_carry(out))

    def test_shader_carry_reads_the_switch_as_the_game(self):
        out = self.tmp / 'game'
        (out / 'shader-cache').mkdir(parents=True)
        (out / 'shader-prepared.bin').write_bytes(b'x')
        (out / 'shader-cache' / 'identities.txt').write_text('# translators\n' + 'ab' * 32 + '\n')
        ini = out / 'nx2navi.ini'
        env = {k: v for k, v in os.environ.items() if k.upper() != 'NX2SARIA_SHADER_CARRY'}
        with mock.patch.dict(os.environ, env, clear=True):
            for text, carry in (('shader_carry = off\n', False), ('shader_carry = FALSE\n', False),
                                ('shader_carry = no\n', False), ('shader_carry = 0 ; note\n', False),
                                ('shader_carry = 0\t# note\n', False), ('shader_carry = 1 # on\n', True),
                                ('shader_carry = on\n', True), ('shader_carry = "0 ; quoted"\n', True),
                                ('shader_carry = 0\nshader_carry = on\n', True),
                                ('shader_carry = on\nshader_carry = off\n', False),
                                ('shader_carry = 0\nshader_carry = ; nothing\n', False)):
                ini.write_text(text)
                self.assertEqual(self.mg.shader_carry(out), carry, text)
            ini.write_text('shader_carry = 0\n')
            with mock.patch.dict(os.environ, {'NX2SARIA_SHADER_CARRY': '1'}):
                self.assertTrue(self.mg.shader_carry(out))
            ini.write_text('shader_carry = 1\n')
            with mock.patch.dict(os.environ, {'NX2SARIA_SHADER_CARRY': 'Off'}):
                self.assertFalse(self.mg.shader_carry(out))

    def test_guest_opt_from_the_ini_counts_without_the_low_end_mode(self):
        out = self.tmp / 'game'
        out.mkdir()
        env = {k: v for k, v in os.environ.items() if k.upper() not in ('NX2NAVI_GUEST_OPT', 'NX2SARIA_GUEST_OPT')}
        for low in ('', 'low_end_gpu = 1\n', 'low_end_gpu = yes\n', 'low_end_gpu = 0\n', 'low_end_gpu = off\n',
                    'low_end_gpu = 0 ; note\n'):
            (out / 'nx2navi.ini').write_text('guest_opt = 2\n' + low)
            with mock.patch.dict(os.environ, env, clear=True):
                self.mg.guest_opt_ini(out)
                self.assertEqual(os.environ.get('NX2SARIA_GUEST_OPT'), '2', low)
        (out / 'nx2navi.ini').write_text('low_end_gpu = 1\n')
        with mock.patch.dict(os.environ, env, clear=True):
            self.mg.guest_opt_ini(out)
            self.assertIsNone(os.environ.get('NX2SARIA_GUEST_OPT'))
        (out / 'nx2navi.ini').write_text('guest_opt = 2\n')
        with mock.patch.dict(os.environ, dict(env, NX2SARIA_GUEST_OPT='1'), clear=True):
            self.mg.guest_opt_ini(out)
            self.assertEqual(os.environ.get('NX2SARIA_GUEST_OPT'), '1')

    def test_cache_from_before_carrying_names_its_translator(self):
        out = self.tmp / 'game'
        out.mkdir()
        old = 'B72C60033CE90FC475EF5B4D397555DDA9A52AD0FDF480B8A4E292ED925A6DE7'
        (out / 'nx2navi_xlate.id').write_text(old + '\r\n')
        self.mg.note_cache_translator(out)
        self.assertFalse((out / 'shader-cache' / 'identities.txt').exists())
        (out / 'shader-cache').mkdir()
        self.mg.note_cache_translator(out)
        ids = out / 'shader-cache' / 'identities.txt'
        lines = ids.read_bytes().decode('ascii').split('\n')
        self.assertTrue(lines[0].startswith('#'))
        self.assertEqual(lines[1], old.lower())
        self.assertEqual(self.mg.cache_identities(out), [old.lower()])
        (out / 'nx2navi_xlate.id').write_text('cd' * 32)
        self.mg.note_cache_translator(out)
        self.assertEqual(self.mg.cache_identities(out), [old.lower()])
        ids.unlink()
        (out / 'nx2navi_xlate.id').write_text('not an identity')
        self.mg.note_cache_translator(out)
        self.assertFalse(ids.exists())

    def test_dlssg_beside_the_game(self):
        import streamline_pin
        from unittest import mock
        kit, out = self.tmp / 'kit', self.tmp / 'game'
        src = kit / 'bin' / 'dlssg'
        src.mkdir(parents=True)
        out.mkdir()
        self.assertEqual(streamline_pin.FILES, self.mg.DLSSG_FILES)
        with mock.patch.object(self.mg, 'KIT', kit):
            for n in self.mg.DLSSG_FILES[:-1]:
                (src / n).write_bytes(n.encode())
            self.assertEqual(self.mg.place_dlssg(out), 0)
            self.assertFalse((out / 'nx2navi_dlssg.dll').exists())
            (src / 'nvngx_dlssg.dll').write_bytes(b'ngx')
            (src / 'streamline-LICENSE.txt').write_text('MIT')
            self.assertEqual(self.mg.place_dlssg(out), len(self.mg.DLSSG_FILES) + 1)
            for n in self.mg.DLSSG_FILES:
                self.assertEqual((out / n).read_bytes(), (src / n).read_bytes())
            self.assertEqual((out / 'streamline-LICENSE.txt').read_text(), 'MIT')

    def test_streamline_pin(self):
        import streamline_pin
        from unittest import mock
        files = {n: f'{n} of the SDK'.encode() for n in streamline_pin.SDK_FILES}
        pins = {n: hashlib.sha256(d).hexdigest() for n, d in files.items()}
        cache = self.tmp / 'dl'
        cache.mkdir()
        archive = cache / f'streamline-sdk-v{streamline_pin.VERSION}.zip'
        with zipfile.ZipFile(archive, 'w') as z:
            for n, d in files.items():
                z.writestr(n, d)
            z.writestr('bin/x64/sl.nis.dll', b'a plugin the game does not load')
        fetched = []

        def download(url, dest, sha256=None, size=None):
            fetched.append(url)
            self.assertEqual(Path(dest), archive)
            return dest

        with mock.patch.dict(streamline_pin.SDK_FILES, pins), mock.patch('launcher.net.download', download):
            sdk = streamline_pin.resolve(None, cache)
            self.assertEqual(fetched, [streamline_pin.URL])
            self.assertFalse(archive.exists())
            self.assertEqual(streamline_pin.wrong_files(sdk), [])
            self.assertFalse((sdk / 'bin' / 'x64' / 'sl.nis.dll').exists())
            self.assertEqual(streamline_pin.resolve(None, cache), sdk)
            self.assertEqual(len(fetched), 1)
            self.assertEqual(streamline_pin.resolve(sdk, cache), sdk)
            (sdk / 'bin' / 'x64' / 'sl.common.dll').write_bytes(b'another Streamline')
            with self.assertRaises(SystemExit):
                streamline_pin.resolve(sdk, cache)
            (sdk / 'bin' / 'x64' / 'sl.common.dll').write_bytes(files['bin/x64/sl.common.dll'])
            with zipfile.ZipFile(archive, 'w') as z:
                for n, d in files.items():
                    z.writestr(n, b'changed' if n == 'license.txt' else d)
            with self.assertRaises(SystemExit):
                streamline_pin.extract(archive, self.tmp / 'x')
            dll = self.tmp / 'nx2navi_dlssg.dll'
            dll.write_bytes(b'our frame generation shim')
            dest = self.tmp / 'bin' / 'dlssg'
            streamline_pin.place(dll, sdk, dest)
        self.assertEqual(sorted(p.name for p in dest.iterdir()),
                         sorted((*streamline_pin.FILES, *streamline_pin.TEXTS, streamline_pin.NOTICE)))
        self.assertEqual((dest / 'streamline-LICENSE.txt').read_bytes(), files['license.txt'])
        note = (dest / streamline_pin.NOTICE).read_text(encoding='utf-8')
        self.assertIn(f'Streamline {streamline_pin.VERSION} SDK', note)
        self.assertIn(f'nx2navi_dlssg.dll sha256 {hashlib.sha256(dll.read_bytes()).hexdigest()}', note)
        self.assertIn(f'nvngx_dlssg.dll sha256 {pins["bin/x64/nvngx_dlssg.dll"]}', note)

    @unittest.skipIf(os.name == 'nt', 'a script stands in for windres.exe')
    def test_compile_resources(self):
        msys, obj = self.tmp / 'tool chain' / 'ucrt64', self.out / 'build-cache' / 'obj'
        windres = msys / 'bin' / 'windres.exe'
        windres.parent.mkdir(parents=True)
        windres.write_text(f'#!{sys.executable}\n' + FAKE_WINDRES)
        windres.chmod(0o755)
        obj.mkdir(parents=True)
        rt = self.tmp / 'the kit' / 'runtime'
        rt.mkdir(parents=True)
        for name in ('nx2saria.rc', 'nx2saria.manifest', 'nx2saria.ico'):
            shutil.copy2(ROOT / 'runtime' / name, rt / name)
        rco = self.mg.compile_resources(msys, rt, obj, dict(os.environ))
        self.assertEqual((rco, rco.read_bytes()), (obj / 'nx2navi_rc.o', b'coff'))
        seen = json.loads(windres.with_suffix('.json').read_text())
        args = seen['argv']
        self.assertEqual(seen['cwd'], str(rt))
        self.assertIn('--use-temp-file', args)
        self.assertIn(f'--preprocessor={msys / "bin" / "gcc.exe"}', args)
        self.assertEqual([a for a in args if a.startswith('--preprocessor-arg=')],
                         ['--preprocessor-arg=-E', '--preprocessor-arg=-xc', '--preprocessor-arg=-DRC_INVOKED'])
        self.assertEqual(args[args.index('-i') + 1], 'nx2saria.rc')
        self.assertFalse([a for a in args if a.startswith(('-I', '--include-dir', '-D', '-U'))])
        self.assertEqual(seen['tmp'], seen['temp'])
        self.assertEqual(Path(seen['tmp']).parent, obj)
        self.assertEqual(sorted(p.name for p in obj.iterdir()), ['nx2navi_rc.o'])
        before = rco.stat().st_mtime_ns
        time.sleep(0.02)
        self.mg.compile_resources(msys, rt, obj, dict(os.environ))
        self.assertEqual(rco.stat().st_mtime_ns, before)
        err = io.StringIO()
        with self.assertRaises(SystemExit), contextlib.redirect_stderr(err):
            self.mg.compile_resources(msys, rt, obj, dict(os.environ, FAKE_WINDRES_FAIL='1'))
        self.assertIn('windres failed on runtime/nx2saria.rc: windres: preprocessing failed.', err.getvalue())
        self.assertEqual(sorted(p.name for p in obj.iterdir()), ['nx2navi_rc.o'])

    def test_a_cut_romfs_image_does_not_count_as_used_space(self):
        import collections
        mb = 1 << 20

        class Src:
            romfs = [(f'f{i}.bin', i) for i in range(3)]

            def romfs_size(self, ref):
                return mb

            def romfs_chunks(self, ref):
                yield bytes([ref]) * mb
        budget = 3 * mb + (1 << 30) + mb
        usage = collections.namedtuple('usage', 'total used free')

        def disk_usage(path):
            held = sum(f.stat().st_size for f in Path(path).rglob('*') if f.is_file())
            return usage(budget, held, budget - held)
        romfs = self.out / 'romfs'
        romfs.mkdir(parents=True)
        (romfs / 'romfs.bin.part').write_bytes(bytes(2 * mb))
        with mock.patch.object(self.mg.shutil, 'disk_usage', disk_usage), \
                contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            self.mg.step_romfs(Src(), self.out, 2)
        self.assertEqual((romfs / 'romfs.bin').stat().st_size, 3 * mb)
        self.assertFalse((romfs / 'romfs.bin.part').exists())
        (romfs / 'romfs.bin').unlink()
        budget -= 2 * mb
        err = io.StringIO()
        with mock.patch.object(self.mg.shutil, 'disk_usage', disk_usage), \
                contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(err), \
                self.assertRaises(SystemExit):
            self.mg.step_romfs(Src(), self.out, 2)
        self.assertIn('GB free for the RomFS image', err.getvalue())

    def test_a_damaged_dump_names_the_file(self):
        raw = bytearray(self.nup.read_bytes())
        raw[raw.find(self.romfs['dir/b.bin']) + 100] ^= 0xFF
        self.nup.write_bytes(raw)
        events, lines = [], []
        run = build.Run(build.command(self.layout, ['--nup', self.nup, '--out', self.out, '--msys', self.msys,
                                                    '--until', 'romfs']), events.append, lines.append)
        self.assertEqual(run.wait(), 1)
        errors = [e['message'] for e in events if e['ev'] == 'error']
        self.assertEqual(len(errors), 1, lines)
        self.assertIn('the dump is damaged: dir/b.bin: size or sha256 differs', errors[0])
        self.assertIn('dump the game again', errors[0])
        self.assertNotIn('file 6', errors[0])
        self.assertEqual(run.error, errors[0])
        self.assertFalse(any('Traceback' in ln for ln in lines))
        self.assertEqual(sorted(p.name for p in (self.out / 'romfs').iterdir()), [])
        state = json.loads((self.out / 'build-cache' / 'state.json').read_text())
        self.assertNotIn('romfs', state['steps'])

    def test_an_unexpected_error_reaches_the_launcher_with_its_message(self):
        made_up_game(self.out)

        def broken(out, cache):
            raise ValueError('the patch table has no line 12')

        def full(out, cache):
            raise OSError(28, 'No space left on device', str(out / 'build-cache' / 'lift' / 'main_0001.c'))
        with mock.patch.object(self.mg, 'EVENTS', False):
            code, text = self.main_rebuild(broken, None, '--events')
            self.assertEqual(code, 1)
            errors = [json.loads(ln[2:]) for ln in text.splitlines() if ln.startswith('@@') and '"error"' in ln]
            self.assertEqual([e['message'] for e in errors],
                             ['the build stopped: ValueError: the patch table has no line 12'])
            self.assertIn('Traceback (most recent call last)', text)
            self.assertIn('error: the build stopped: ValueError', text)
            code, text = self.main_rebuild(full, None, '--events')
            self.assertEqual(code, 1)
            self.assertIn('error: the build stopped: the disk is full (', text)
            self.assertIn('main_0001.c)', text)
        self.assertEqual(self.mg.error_text(FileNotFoundError(2, 'No such file or directory', 'x.bin')),
                         'No such file or directory: x.bin')
        self.assertEqual(self.mg.error_text(MemoryError()), 'MemoryError')

    def test_a_game_folder_outside_the_basic_plane_compiles(self):
        fairy, han = chr(0x1F9DA), chr(0x20BB7)
        out = self.tmp / f'Games {fairy}' / f'V\u00edctor \u30bc\u30eb\u30c0 {han}ya'
        lift, gen = self.tmp / 'lift', self.tmp / 'gen'
        for d in (out / 'exefs', lift, gen):
            d.mkdir(parents=True)
        for name in self.mg.EXEFS_FILES:
            (out / 'exefs' / name).write_bytes(b'x')
        (lift / 'layout.json').write_text(json.dumps([{'name': 'main'}]))
        text = self.mg.modules_c(lift, out, gen).read_text(encoding='utf-8')
        import check_content
        self.assertIn(check_content.GENERATED_MARKER, text.splitlines()[0])
        self.assertIn('generated from game code', check_content.problems_for(gen / 'modules.c'))
        for half in ('\\ud83e', '\\ud842'):
            self.assertNotIn(half, text.lower())
        want = 'const char rt_default_exefs[] = "' + str(out / 'exefs').replace('\\', '\\\\') + '";'
        self.assertIn(want, text)
        self.assertEqual(self.mg.c_string('a"b\\c\n' + fairy), '"a\\"b\\\\c\\n' + fairy + '"')
        cc = shutil.which('clang') or shutil.which('cc') or shutil.which('gcc')
        env = dict(os.environ)
        if not cc and (Path(r'C:\msys64\ucrt64\bin') / 'clang.exe').is_file():
            cc = r'C:\msys64\ucrt64\bin\clang.exe'
            env['PATH'] = r'C:\msys64\ucrt64\bin' + os.pathsep + env['PATH']
        if not cc:
            self.skipTest('the text is right; compiling it needs a C compiler')

        def compiles(source, *include):
            obj = source.with_suffix('.o')
            r = subprocess.run([cc, '-c', '-std=gnu11', '-w', *include, '-o', str(obj), str(source)],
                               capture_output=True, text=True, errors='replace', env=env)
            self.assertEqual(r.returncode, 0, r.stderr[-2000:])
            self.assertIn(str(out / 'exefs').encode('utf-8'), obj.read_bytes())
        strings = [ln for ln in text.splitlines() if ln.startswith('const char rt_')]
        self.assertEqual(len(strings), 4)
        (self.tmp / 'strings.c').write_text('\n'.join(strings) + '\n', encoding='utf-8')
        compiles(self.tmp / 'strings.c')
        said = subprocess.run([cc, '--version'], capture_output=True, text=True, errors='replace', env=env).stdout
        if 'clang' in said:
            (gen / 'layout.h').write_text('#define MOD_MAIN_BASE 0x7000000000ull\n'
                                          '#define MOD_MAIN_SIZE 0x1000000ull\n')
            compiles(gen / 'modules.c', *runtime_include.flags(), '-I', str(gen))

    def test_the_disk_space_of_every_step_is_checked_first(self):
        import collections
        mg, gib = self.mg, 1 << 30
        usage = collections.namedtuple('usage', 'total used free')
        self.out.mkdir()
        first = mg.space_needs(['exefs', 'romfs', 'lift', 'compile', 'shaders'], self.out, 26_240_000_000)
        self.assertEqual([n for _, n in first], [26_240_000_000 + gib, 2 * gib, 4 * gib])
        (self.out / 'build-cache' / 'lift').mkdir(parents=True)
        (self.out / 'build-cache' / 'lift' / 'layout.json').write_text('[]')
        self.assertEqual([n for _, n in mg.space_needs(['lift', 'compile', 'shaders'], self.out, carried=True)], [gib])
        self.assertEqual(mg.space_needs(['shaders'], self.out, carried=True), [])
        self.assertEqual(mg.space_needs([], self.out), [])
        (self.out / 'shader-cache').mkdir()
        self.assertEqual([n for _, n in mg.space_needs(['shaders'], self.out)], [4 * gib])
        (self.out / 'shader-cache' / '0123.spv').write_bytes(b'')
        self.assertEqual(mg.space_needs(['shaders'], self.out), [])
        (self.out / 'shader-cache' / '0123.spv').unlink()
        (self.out / 'shader-cache' / 'shader-cache.pack').write_bytes(b'')
        self.assertEqual(mg.space_needs(['shaders'], self.out), [])
        shutil.rmtree(self.out / 'shader-cache')

        def check(free, needs):
            err = io.StringIO()
            with mock.patch.object(mg.shutil, 'disk_usage', lambda p: usage(500 * gib, 0, free)), \
                    contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(err):
                try:
                    mg.check_space(self.out, needs)
                    return ''
                except SystemExit:
                    return err.getvalue()
        self.assertEqual(check(40 * gib, first), '')
        text = check(30 * gib, first)
        self.assertIn('has 32.2 GB free and this build needs 33.8 GB there', text)
        self.assertIn('27.3 GB for the RomFS image, 2.1 GB for the build cache', text)
        self.assertIn('4.3 GB for the prepared shaders. Free 1.5 GB more on that disk', text)
        self.assertEqual(check(0, []), '')
        image = [('the RomFS image', 10 << 20)]
        self.assertIn('needs 0.0 GB there', check(19 << 19, image))
        (self.out / 'romfs').mkdir()
        (self.out / 'romfs' / 'romfs.bin.part').write_bytes(bytes(1 << 20))
        self.assertEqual(check(19 << 19, image), '')

    def test_an_update_without_room_stops_before_its_lift(self):
        import collections
        made_up_game(self.out)
        usage = collections.namedtuple('usage', 'total used free')
        ran = []
        with mock.patch.object(self.mg.shutil, 'disk_usage', lambda p: usage(10 ** 12, 0, 200 << 20)):
            code, text = self.main_rebuild(lambda out, cache: ran.append('lift'), lambda *a, **k: ran.append('compile'),
                                           '--until', 'compile')
        self.assertEqual((code, ran), (1, []))
        self.assertIn('has 0.2 GB free and this build needs 2.1 GB there: 2.1 GB for the build cache', text)
        with mock.patch.object(self.mg.shutil, 'disk_usage', lambda p: usage(10 ** 12, 0, 50 << 30)):
            code, text = self.main_rebuild(lambda out, cache: ran.append('lift'), lambda *a, **k: ran.append('compile'),
                                           '--until', 'compile')
        self.assertEqual((code, ran), (0, ['lift', 'compile']))

    def test_the_steps_a_build_will_run(self):
        mg = self.mg
        made_up_game(self.out)
        state = json.loads((self.out / 'build-cache' / 'state.json').read_text())
        ident = state['input']
        self.assertEqual(mg.steps_to_run(state, ident, self.out, 'compile'), ['lift', 'compile'])
        for name in ('lift', 'compile'):
            state['steps'][name] = mg.step_key(name, ident, self.out)
        self.assertEqual(mg.steps_to_run(state, ident, self.out, 'compile'), [])
        del state['steps']['romfs']
        self.assertEqual(mg.steps_to_run(state, ident, self.out, 'compile'), ['romfs', 'compile'])
        self.assertEqual(mg.steps_to_run({'steps': {}}, ident, self.out, 'romfs'), ['exefs', 'romfs'])

    def test_the_build_tools_are_tried_before_the_long_steps(self):
        mg = self.mg
        work = self.tmp / 'check'
        seen = []

        def compiler(compile_rc=0, link_rc=0, stderr='', start=None):
            def run(cmd, **kw):
                seen.append(cmd)
                if start:
                    raise start
                if '-c' in cmd:
                    self.assertTrue(Path(cmd[-1]).is_file())
                    return mock.Mock(returncode=compile_rc, stderr=stderr)
                return mock.Mock(returncode=link_rc, stderr=stderr)
            return run

        def probe(**kw):
            err = io.StringIO()
            with mock.patch.object(mg.subprocess, 'run', compiler(**kw)), \
                    mock.patch.object(mg, 'toolchain', lambda msys: (msys / 'bin' / 'clang.exe', msys / 'bin' /
                                                                     'clang++.exe', dict(os.environ), msys)), \
                    contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(err):
                try:
                    self.real_probe(self.msys, work)
                    return ''
                except SystemExit:
                    return err.getvalue()
        self.assertEqual(probe(), '')
        self.assertEqual([Path(c[0]).name for c in seen], ['clang.exe', 'clang++.exe'])
        self.assertTrue(set(mg.LINK_LIBS) <= set(seen[1]))
        self.assertFalse(work.exists())
        text = probe(compile_rc=1, stderr="clang: error: unable to execute command: program not executable\n")
        self.assertIn('clang.exe cannot compile a test program (clang: error: unable to execute command', text)
        self.assertIn(f'delete the folder {self.msys.parent} and try again', text)
        text = probe(link_rc=1, stderr='ld.lld: error: unable to find library -lSDL3\n'
                                       'ld.lld: error: unable to find library -lopus\nclang++: error: linker failed\n')
        self.assertIn(f'the build tools cannot link the game: {self.msys / "lib"} lacks -lSDL3, -lopus.', text)
        text = probe(link_rc=1, stderr='ld.lld: error: undefined symbol: WinMain\n')
        self.assertIn('the linker says: ld.lld: error: undefined symbol: WinMain', text)
        text = probe(start=PermissionError(13, 'Access is denied'))
        self.assertIn('clang.exe does not start (Access is denied)', text)
        self.assertIn('security software', text)
        self.assertFalse(work.exists())
        made_up_game(self.out)
        called = []
        with mock.patch.object(mg, 'probe_toolchain', lambda msys, work: called.append(work)), \
                mock.patch.object(mg, 'WIN', True):
            self.main_rebuild(lambda out, cache: None, lambda *a, **k: None, '--until', 'compile')
            self.assertEqual(called, [self.out / 'build-cache' / 'toolchain-check'])
            self.main_rebuild(lambda out, cache: None, lambda *a, **k: None, '--until', 'compile')
        self.assertEqual(len(called), 1)

    def main_rebuild(self, lift, compile_, *more):
        buf = io.StringIO()
        argv = ['make_game.py', '--rebuild', '--out', str(self.out), '--msys', str(self.msys), *more]
        with mock.patch.object(self.mg, 'step_lift', lift), mock.patch.object(self.mg, 'step_compile', compile_), \
                mock.patch.object(self.mg, 'keep_awake', lambda: None), mock.patch.object(sys, 'argv', argv), \
                contextlib.redirect_stdout(buf), contextlib.redirect_stderr(buf):
            try:
                self.mg.main()
                return 0, buf.getvalue()
            except SystemExit as e:
                return e.code, buf.getvalue()

    def test_an_update_that_stops_in_the_compile_keeps_the_game(self):
        made_up_game(self.out)
        self.assertEqual(build.built_version(self.out), '0.6.1')

        def stop(*a, **k):
            self.mg.fail('3 files did not compile')
        code, _ = self.main_rebuild(lambda out, cache: None, stop)
        self.assertEqual(code, 1)
        st = json.loads((self.out / 'build-cache' / 'state.json').read_text())
        self.assertEqual(sorted(s for s in st['steps'] if s != 'shaders'), ['exefs', 'lift', 'romfs'])
        self.assertEqual(build.built_version(self.out), '')
        self.assertTrue(build.can_rebuild(self.out))
        self.assertTrue(build.installed(self.out))
        (self.out / GAME_EXE).unlink()
        self.assertFalse(build.installed(self.out))
        self.assertFalse(build.installed(''))
        self.assertFalse(build.installed(None))
        self.assertFalse(build.installed(self.tmp / 'no such folder'))


def logic_digest(path):
    import ast
    text = Path(path).read_text(encoding='utf-8')
    skip = set()
    for node in ast.walk(ast.parse(text)):
        body = getattr(node, 'body', None)
        if (isinstance(node, (ast.Module, ast.ClassDef, ast.FunctionDef, ast.AsyncFunctionDef)) and body
                and isinstance(body[0], ast.Expr) and isinstance(getattr(body[0], 'value', None), ast.Constant)
                and isinstance(body[0].value.value, str)):
            skip.update(range(body[0].lineno, body[0].end_lineno + 1))
    lines = [ln.rstrip() for n, ln in enumerate(text.splitlines(), 1) if n not in skip]
    return hashlib.sha256('\n'.join(ln for ln in lines if ln and not ln.lstrip().startswith('#')).encode()).hexdigest()


SCAN_LOGIC = ('55ea0c150aaf9dac93a2e6fd3b4ca11e1c572b5eee8b6bd74960535964384634',
              '758faaaaca013ed78f21359270cb92196e7f3ffd3aa30a2d06a9edfa0de1a502')


class FakeImage:
    def __init__(self, files, dicts=()):
        self.files = files
        self.dicts = {d.dict_id(): d for d in dicts}

    def read(self, entry):
        return self.files[entry['path']][:entry['size']]

    def decompressor(self, dict_id):
        import zstandard
        return zstandard.ZstdDecompressor(dict_data=self.dicts[dict_id]) if dict_id else zstandard.ZstdDecompressor()


class FileCheckTest(Tmp):
    def setUp(self):
        super().setUp()
        try:
            import zstandard  # noqa: F401
        except ImportError:
            self.skipTest('needs zstandard')
        import romfs_check
        self.rc = romfs_check

    SOME, SAME = object(), object()

    @staticmethod
    def old_problem(rc, image, entry, piece=1 << 22):
        import zstandard
        data = image.read(entry)
        if len(data) != entry['size']:
            return 'shorter than the index says'
        if not entry['path'].endswith('.zs'):
            return 'nothing but zeros' if len(data) >= 16 and not any(data) else None
        if data[:4] != rc.ZSTD_MAGIC:
            return 'no compressed frame at its start'
        try:
            params = zstandard.get_frame_parameters(data)
            if params.dict_id and params.dict_id not in image.dicts:
                return None
            out = image.decompressor(params.dict_id).decompressobj()
            size = 0
            for at in range(0, len(data), piece):
                size += len(out.decompress(data[at:at + piece]))
        except zstandard.ZstdError as e:
            return f'does not decompress ({e})'
        if params.content_size and size != params.content_size:
            return f'decompresses to {size} bytes, not {params.content_size}'
        return None

    def cases(self):
        import random
        import zstandard
        z = zstandard.ZstdCompressor(level=3)
        twelve = bytes(i % 12 for i in range(256))
        big_raw = random.Random(1).randbytes(16 << 20).translate(twelve)
        big = z.compress(big_raw)
        self.assertGreater(len(big), 6 << 20)
        small = z.compress(random.Random(2).randbytes(300 << 10).translate(twelve))
        mid = len(big) // 2
        samples = [bytes([65 + i % 7]) * 40 + f'sample {i} of the dictionary'.encode() * 3 for i in range(400)]
        dic = zstandard.train_dictionary(4096, samples)
        with_dict = zstandard.ZstdCompressor(level=3, dict_data=dic).compress(b''.join(samples[:50]))
        no_size = zstandard.ZstdCompressor(level=3, write_content_size=False).compress(big_raw[:1 << 20])
        some, same, span = self.SOME, self.SAME, 300 << 10
        return dic, [
            ('whole, small', 'Tex/a.bntx.zs', small, None),
            ('whole, large', 'Tex/b.bntx.zs', big, None),
            ('zeros before the frame', 'Tex/c.bntx.zs', bytes(16) + small, 'no compressed frame at its start'),
            ('damaged at the start', 'Tex/d.bntx.zs', big[:4] + bytes(60) + big[64:], some),
            ('damaged after the start', 'Tex/e.bntx.zs', big[:64] + bytes(span) + big[64 + span:], some),
            ('damaged in the middle', 'Tex/f.bntx.zs', big[:mid] + bytes(span) + big[mid + span:], some),
            ('damaged near the end', 'Tex/g.bntx.zs', big[:-9000] + bytes(4096) + big[-9000 + 4096:], some),
            ('a little damage in the middle', 'Tex/v.bntx.zs', big[:mid] + bytes(64) + big[mid + 64:], same),
            ('cut in the middle', 'Tex/h.bntx.zs', big[:mid], some),
            ('cut at 4 MiB', 'Tex/i.bntx.zs', big[:4 << 20], some),
            ('cut at 64 KiB', 'Tex/j.bntx.zs', big[:64 << 10], some),
            ('cut by one byte', 'Tex/k.bntx.zs', big[:-1], some),
            ('small, cut', 'Tex/l.bntx.zs', small[:len(small) // 2], some),
            ('all zeros', 'Model/m.bfres.zs', bytes(1 << 20), 'no compressed frame at its start'),
            ('all zeros, not packed', 'Param/n.bgyml', bytes(64), 'nothing but zeros'),
            ('not packed, fine', 'Param/o.bgyml', b'YB' + bytes(62), None),
            ('not packed, a few zeros', 'Param/p.bgyml', bytes(8), None),
            ('only the magic', 'Tex/q.bntx.zs', big[:4], some),
            ('with a dictionary', 'Pack/r.pack.zs', with_dict, None),
            ('with a dictionary, damaged', 'Pack/s.pack.zs', with_dict[:40] + bytes(40) + with_dict[80:], some),
            ('no size in its frame', 'Tex/t.bntx.zs', no_size, same),
            ('no size in its frame, damaged', 'Tex/u.bntx.zs', no_size[:9000] + bytes(4096) + no_size[13096:], same),
            ('no size in its frame, cut', 'Tex/w.bntx.zs', no_size[:len(no_size) // 2], same),
        ]

    def test_the_piece_size_does_not_change_the_result(self):
        rc = self.rc
        dic, cases = self.cases()
        self.assertEqual(rc.PIECE, 1 << 16)
        for dicts in ((dic,), ()):
            image = FakeImage({path: data for _, path, data, _ in cases}, dicts)
            for name, path, data, want in cases:
                if not dicts and 'dictionary' not in name:
                    continue
                entry = {'path': path, 'size': len(data)}
                was = self.old_problem(rc, image, entry)
                with mock.patch.object(rc, 'PIECE', 1 << 22):
                    large = rc.problem(image, entry)
                small = rc.problem(image, entry)
                self.assertEqual((large, small), (was, was), name)
                if want is self.SOME:
                    if dicts or 'dictionary' not in name:
                        self.assertTrue(was, f'{name}: not found damaged')
                elif want is not self.SAME:
                    self.assertEqual(was, want, name)
        image = FakeImage({'Tex/a.bntx.zs': cases[0][2]})
        self.assertEqual(rc.problem(image, {'path': 'Tex/a.bntx.zs', 'size': len(cases[0][2]) + 1}),
                         'shorter than the index says')

    def test_bytes_after_the_frame_do_not_depend_on_the_piece_size(self):
        rc = self.rc
        _, cases = self.cases()
        small, big = cases[0][2], cases[1][2]
        tails = {'Tex/a.bntx.zs': small + bytes(200 << 10),
                 'Tex/b.bntx.zs': big + bytes(5 << 20),
                 'Tex/c.bntx.zs': small + small}
        image = FakeImage(tails)
        for path, data in tails.items():
            entry = {'path': path, 'size': len(data)}
            with mock.patch.object(rc, 'PIECE', 1 << 22):
                self.assertIsNone(rc.problem(image, entry), path)
            self.assertIsNone(rc.problem(image, entry), path)
        entry = {'path': 'Tex/b.bntx.zs', 'size': len(tails['Tex/b.bntx.zs'])}
        self.assertIn('multiple times', self.old_problem(rc, image, entry))
        self.assertIn('multiple times', self.old_problem(rc, image, {'path': 'Tex/a.bntx.zs',
                                                                    'size': len(tails['Tex/a.bntx.zs'])}, 1 << 16))

    def test_the_scan_key_is_the_one_installs_have(self):
        import make_game
        out = self.tmp / 'game'
        out.mkdir()
        ident = 'nup C:\\dumps\\game.nup 28000000000 1760000000'
        released = '55ea0c150aaf9dac93a2e6fd3b4ca11e1c572b5eee8b6bd74960535964384634'
        self.assertEqual(make_game.step_key('scan', ident, out), f'scan-1|{ident}|{released}|missing')
        (out / 'romfs-check.json').write_text('{}')
        self.assertEqual(make_game.step_key('scan', ident, out), f'scan-1|{ident}|{released}|written')

    def test_the_file_checks_logic_is_the_recorded_one(self):
        import make_game
        now = (make_game.SCAN_TOOL, logic_digest(ROOT / 'tools' / 'romfs_check.py'))
        self.assertEqual(now, SCAN_LOGIC,
                         "tools/romfs_check.py's logic changed. If the change can alter what the check finds or "
                         'writes, give make_game.SCAN_TOOL a new value, so that installs check their game files '
                         'again; either way record the new pair in SCAN_LOGIC (tests/launcher_test.py).')
        a = self.tmp / 'a.py'
        a.write_text('"""Doc."""\nX = 1\n\n\ndef f():\n    """Doc."""\n    # why\n    return X  \n')
        b = self.tmp / 'b.py'
        b.write_text('"""Another."""\n# a comment\nX = 1\n\ndef f():\n    """Another\n    one."""\n    return X\n')
        c = self.tmp / 'c.py'
        c.write_text('"""Doc."""\nX = 2\n\n\ndef f():\n    """Doc."""\n    # why\n    return X  \n')
        self.assertEqual(logic_digest(a), logic_digest(b))
        self.assertNotEqual(logic_digest(a), logic_digest(c))


PROMPT_LOGIC = ('ad59f2c4533f6ea2630b916ef5f3695f72daffe9b2b7663eea4168a4f08e4249',
                {'pad_prompts.py': '9692b7632e4570df0814149a3d70f729645835f74eab1a9b34b6d963d81efca0',
                 'prompt_art.py': '07f536b3d6f79199010f5c7bcf0ee1d3281ec56fd7f7a6def9cf1282a824eb30',
                 'button_icons.py': '3804e371a030965bc9eb4226f333b611651f25efaa67ee523b404edc3257818a'})


def load_pad_prompts():
    import types
    stubs = {}
    try:
        import fontTools  # noqa: F401
    except ImportError:
        for name, classes in (('fontTools', ()), ('fontTools.pens', ()), ('fontTools.ttLib', ('TTFont',)),
                              ('fontTools.pens.basePen', ('BasePen',)), ('fontTools.pens.boundsPen', ('BoundsPen',)),
                              ('fontTools.pens.recordingPen', ('RecordingPen',)),
                              ('fontTools.pens.reverseContourPen', ('ReverseContourPen',)),
                              ('fontTools.pens.t2CharStringPen', ('T2CharStringPen',)),
                              ('fontTools.pens.transformPen', ('TransformPen',))):
            stubs[name] = types.ModuleType(name)
            for cls in classes:
                setattr(stubs[name], cls, type(cls, (), {}))
    with mock.patch.dict(sys.modules, stubs):
        for name in ('pad_prompts', 'button_icons'):
            sys.modules.pop(name, None)
        import pad_prompts
    return pad_prompts


class PromptToolsTest(Tmp):
    def setUp(self):
        super().setUp()
        try:
            import zstandard  # noqa: F401
            self.pp = load_pad_prompts()
        except ImportError as e:
            self.skipTest(f'needs zstandard and Pillow: {e}')

    def test_a_file_unpacks_to_the_same_bytes_in_pieces(self):
        import random
        import zstandard
        pp = self.pp
        seven = bytes(i % 7 for i in range(256))
        big = random.Random(5).randbytes(24 << 20).translate(seven)
        small = b'a small file ' * 100
        samples = [bytes([65 + i % 7]) * 40 + f'sample {i} of the dictionary'.encode() * 3 for i in range(400)]
        dic = zstandard.train_dictionary(4096, samples)
        z = zstandard.ZstdCompressor(level=3)
        with_dict = zstandard.ZstdCompressor(level=3, dict_data=dic).compress(b''.join(samples[:50]))
        no_size = zstandard.ZstdCompressor(level=3, write_content_size=False).compress(big[:3 << 20])

        def whole(raw, d=None):
            return zstandard.ZstdDecompressor(dict_data=d).decompressobj().decompress(raw)
        packed = z.compress(big)
        self.assertGreater(len(packed), 4 * pp.PIECE)
        for name, raw, d in (('large', packed, None), ('small', z.compress(small), None), ('empty', z.compress(b''), None),
                             ('with a dictionary', with_dict, dic), ('no size in its frame', no_size, None),
                             ('bytes after the frame', z.compress(small) + bytes(300 << 10), None),
                             ('two frames', z.compress(small) + packed, None)):
            self.assertEqual(pp.unpack(raw, d), whole(raw, d), name)
            with mock.patch.object(pp, 'PIECE', 1000):
                self.assertEqual(pp.unpack(raw, d), whole(raw, d), name)
        self.assertEqual(pp.unpack(packed), big)
        with self.assertRaises(zstandard.ZstdError):
            pp.unpack(packed[:64] + bytes(300 << 10) + packed[64 + (300 << 10):])
        root = self.tmp / 'romfs'
        for path, data in (('Font2/plain.bin', small), ('UI/big.bntx.zs', packed), ('UI/dict.bntx.zs', with_dict),
                           (pp.DICTIONARIES, z.compress(sarc({'zs.zsdic': dic.as_bytes()})))):
            (root / path).parent.mkdir(parents=True, exist_ok=True)
            (root / path).write_bytes(data)
        romfs = pp.Romfs(root)
        self.assertEqual(romfs.load('Font2/plain.bin'), (small, None))
        self.assertEqual(romfs.load('UI/big.bntx.zs'), (big, None))
        content, used = romfs.load('UI/dict.bntx.zs')
        self.assertEqual((content, used.dict_id()), (b''.join(samples[:50]), dic.dict_id()))

    def test_the_prompts_key_is_the_one_installs_have(self):
        import make_game
        kit = self.tmp / 'kit'
        art = kit / 'prompt-art' / 'XGamepad' / 'Default'
        art.mkdir(parents=True)
        (art / 'T_X_A_White.png').write_bytes(b'picture')
        out = self.tmp / 'game'
        out.mkdir()
        ident = 'nup C:\\dumps\\game.nup 28000000000 1760000000'
        released = 'ad59f2c4533f6ea2630b916ef5f3695f72daffe9b2b7663eea4168a4f08e4249'
        with mock.patch.object(make_game, 'KIT', kit):
            pictures = make_game.tree_digest(kit / 'prompt-art', ('**/*.png',))
            binds = make_game.sha256_bytes(b'[]')
            self.assertEqual(make_game.step_key('prompts', ident, out),
                             f'prompts-1|{ident}|{pictures}|{released}|{binds}|missing')

    def test_the_prompt_tools_logic_is_the_recorded_one(self):
        import make_game
        now = (make_game.PROMPT_TOOLS, {name: logic_digest(ROOT / 'tools' / name) for name in PROMPT_LOGIC[1]})
        self.assertEqual(now, PROMPT_LOGIC,
                         'The logic of the tools that draw the button prompts changed. If the change can alter what '
                         'they write, give make_game.PROMPT_TOOLS a new value, so that installs draw their prompts '
                         'again; either way record the new pair in PROMPT_LOGIC (tests/launcher_test.py).')


FAKE_MAKE_GAME = r'''
import json, sys, time
def ev(**k): print('@@' + json.dumps(k), flush=True)
if '--fail' in open(sys.argv[0] + '.mode').read():
    ev(ev='step', step='check', state='start'); ev(ev='error', message='this is not game version 1.1.1'); sys.exit(1)
mode = open(sys.argv[0] + '.mode').read()
for s in ('check', 'exefs', 'romfs', 'lift', 'compile', 'shaders', 'prompts') + (('scan',) if '--scan' in mode else ()):
    ev(ev='step', step=s, state='start')
    if s == 'compile':
        ev(ev='progress', step=s, done=5, total=10, unit='files'); ev(ev='phase', step=s, phase='link')
    print('working on', s, flush=True)
    ev(ev='step', step=s, state='done', seconds=0.1)
ev(ev='end', exe='x')
'''


class JobTest(Tmp):
    def make_ctx(self, mode):
        app = self.tmp / 'app'
        (app / 'tools').mkdir(parents=True)
        (app / 'VERSION').write_text('0.3.10\n')
        script = app / 'tools' / 'make_game.py'
        script.write_text(FAKE_MAKE_GAME)
        (app / 'tools' / 'make_game.py.mode').write_text(mode)
        ctx = Context(Layout(root=self.tmp / 'root', app=app))
        return ctx

    def patch_tools(self):
        saved = toolchain.load_lock, toolchain.is_current, toolchain.ensure_zbic
        toolchain.load_lock = lambda layout, version='dev': {'packages': []}
        toolchain.is_current = lambda layout, lock: True
        toolchain.ensure_zbic = lambda *a, **k: None
        self.addCleanup(lambda: setattr(toolchain, 'load_lock', saved[0]) or
                        setattr(toolchain, 'is_current', saved[1]) or setattr(toolchain, 'ensure_zbic', saved[2]))

    def test_install_job(self):
        self.patch_tools()
        ctx = self.make_ctx('ok')
        j = jobs.InstallJob(ctx, self.tmp / 'g.nup', self.tmp / 'game').run_inline()
        self.assertEqual(j.result, 'ok', j.error)
        self.assertEqual([s.state for s in j.steps], [jobs.SKIPPED] + [jobs.DONE] * 6 + [jobs.SKIPPED, jobs.DONE])
        self.assertEqual(j.step('check').value, 'Version 1.1.1')
        self.assertTrue(any('working on lift' in ln for ln in j.lines))
        self.assertEqual(j.overall(), 1.0)
        self.assertIsNone(j.step('prompts'))

    def test_install_job_with_button_art(self):
        self.patch_tools()
        ctx = self.make_ctx('ok')
        (ctx.layout.app / 'prompt-art').mkdir()
        j = jobs.InstallJob(ctx, self.tmp / 'g.nup', self.tmp / 'game').run_inline()
        self.assertEqual(j.result, 'ok', j.error)
        self.assertEqual([s.id for s in j.steps][-4:], ['shaders', 'prompts', 'scan', 'ready'])
        self.assertEqual(j.step('prompts').state, jobs.DONE)
        self.assertEqual([s.id for s in jobs.RebuildJob(ctx, self.tmp / 'game').steps][-3:], ['shaders', 'prompts', 'scan'])

    def test_failed_job(self):
        self.patch_tools()
        ctx = self.make_ctx('--fail')
        j = jobs.InstallJob(ctx, self.tmp / 'g.nup', self.tmp / 'game').run_inline()
        self.assertEqual(j.result, 'failed')
        self.assertIn('1.1.1', j.error)
        self.assertEqual(j.step('check').state, jobs.FAILED)

    def scan_job(self, mode, report):
        self.patch_tools()
        ctx = self.make_ctx(mode)
        game = self.tmp / 'game'
        game.mkdir()
        if report is not None:
            (game / 'romfs-check.json').write_text(json.dumps(report))
        j = jobs.InstallJob(ctx, self.tmp / 'g.nup', game).run_inline()
        self.assertEqual(j.result, 'ok', j.error)
        ctx._file.flush()
        return j, (ctx.layout.logs / 'launcher.log').read_text(encoding='utf-8')

    def test_the_file_check_result_is_shown(self):
        known = sorted(jobs.SCAN_EXPECTED)
        textures = [p for p in known if p.startswith('Tex/')]
        report = {'status': 'pass', 'files': 186587, 'damaged': {p: 'no compressed frame at its start' for p in known},
                  'repaired': {p: 'Tex/Other.bntx.zs' for p in textures},
                  'not_repaired': [p for p in known if p not in textures]}
        j, log = self.scan_job('ok --scan', report)
        self.assertEqual(j.step('scan').state, jobs.DONE)
        self.assertEqual(j.step('scan').value, '4 Flagged, 3 Stand-Ins')
        self.assertEqual(j.note, 'The file check flagged 4 game files, the same as in every copy of the game: 3 textures '
                                 'got a stand-in, and nothing needs doing.')
        self.assertIn('file check: The file check flagged 4 game files', log)
        for text in (j.note, j.step('scan').value):
            self.assertNotIn('dump', text.lower())

    def test_other_damaged_files_are_counted_and_pointed_to(self):
        report = {'damaged': {'Tex/A_Nrm.bntx.zs': 'no compressed frame at its start', 'Model/B.bfres.zs': 'x'},
                  'repaired': {'Tex/A_Nrm.bntx.zs': 'Tex/C_Nrm.bntx.zs'}, 'not_repaired': ['Model/B.bfres.zs']}
        j, _ = self.scan_job('ok --scan', report)
        self.assertEqual(j.step('scan').value, '2 Flagged, 1 Stand-In')
        self.assertIn('flagged 2 game files as damaged: 1 texture got a stand-in', j.note)
        self.assertIn('romfs-check.json', j.note)
        self.assertNotIn('dump', j.note.lower())
        self.assertEqual(jobs.scan_summary(['Model/B.bfres.zs'], 0)[1],
                         'The file check flagged 1 game file as damaged: none got a stand-in, so what uses them still '
                         'loads. romfs-check.json in the game folder lists them.')

    def test_a_clean_or_kept_file_check_says_nothing_on_home(self):
        j, _ = self.scan_job('ok --scan', {'damaged': {}, 'repaired': {}, 'not_repaired': []})
        self.assertTrue(j.step('scan').value.startswith('Done'))
        self.assertEqual(j.note, '')
        shutil.rmtree(self.tmp / 'game')
        shutil.rmtree(self.tmp / 'app')
        j, _ = self.scan_job('ok', {'damaged': {p: 'x' for p in jobs.SCAN_EXPECTED}, 'repaired': {}})
        self.assertEqual(j.step('scan').state, jobs.SKIPPED)
        self.assertEqual(j.step('scan').value, 'Unchanged · 4 Flagged, 0 Stand-Ins')
        self.assertEqual(j.note, '')
        self.assertIsNone(build.scan_report(self.tmp / 'no game'))
        (self.tmp / 'game' / 'romfs-check.json').write_text('not json')
        self.assertIsNone(build.scan_report(self.tmp / 'game'))

    def test_home_shows_the_file_check_note(self):
        App, a = mock_app(self, self.tmp / 'game')
        a.job = mock.Mock(kind='install', result='ok', error='', after=None,
                          note='The file check flagged 4 game files, the same as in every copy of the game: 3 '
                               'textures got a stand-in, and nothing needs doing.')
        a.settings['keep_cache'] = True
        with mock.patch('launcher.ui.app.winsys.keep_awake'):
            App.job_finished(a)
            self.assertEqual(a.message, 'The game is built. Select Play. ' + a.job.note)
            a.job.note = ''
            App.job_finished(a)
        self.assertEqual(a.message, 'The game is built. Select Play.')

    @unittest.skipUnless(os.name == 'nt', 'Windows priority classes')
    def test_the_build_and_what_it_starts_run_below_normal_priority(self):
        script = self.tmp / 'priority.py'
        script.write_text(
            'import ctypes, subprocess, sys\n'
            'k = ctypes.windll.kernel32\n'
            'k.GetCurrentProcess.restype = ctypes.c_void_p\n'
            'k.GetPriorityClass.argtypes = [ctypes.c_void_p]\n'
            'print(sys.argv[1], hex(k.GetPriorityClass(k.GetCurrentProcess())), flush=True)\n'
            'if sys.argv[1] == "build":\n'
            '    subprocess.run([sys.executable, __file__, "compiler"])\n')
        lines = []
        run = build.Run([sys.executable, str(script), 'build'], lambda ev: None, lines.append)
        self.assertEqual(run.wait(), 0, lines)
        self.assertEqual(lines, ['build 0x4000', 'compiler 0x4000'])

    def test_the_build_is_read_to_its_end_when_a_handler_fails(self):
        child = [sys.executable, '-c',
                 'import json\n'
                 'for i in range(20000):\n'
                 '    print("line", i, "x" * 60)\n'
                 'print("@@[1]")\n'
                 'print("@@" + json.dumps({"ev": "end"}))\n']
        events = []

        def on_line(line):
            raise OSError(28, 'No space left on device')

        def on_event(ev):
            events.append(ev)
            raise RuntimeError('a drawing error')
        run = build.Run(child, on_event, on_line)
        try:
            code = run.proc.wait(timeout=60)
        except subprocess.TimeoutExpired:
            run.cancel()
            self.fail('the build never ended: nobody read its output any more')
        run.wait()
        self.assertEqual(code, 0)
        self.assertEqual(events, [{'ev': 'end'}])
        self.assertEqual(run.dropped, 20002)

    def test_a_log_that_cannot_be_written_loses_only_the_line(self):
        ctx = self.make_ctx('ok')
        real = ctx._file
        self.addCleanup(real.close)
        ctx._file = mock.Mock()
        ctx._file.write.side_effect = OSError(28, 'No space left on device')
        ctx.log('a line while the disk is full')
        j = jobs.Job(ctx, [])
        j.log('a line of the build')
        self.assertEqual(list(j.lines), ['a line of the build'])
        ctx._file = real
        ctx.log('the disk has room again')
        real.close()
        ctx.log('a line after the file was closed')
        text = (ctx.layout.logs / 'launcher.log').read_text(encoding='utf-8')
        self.assertIn('the disk has room again', text)
        self.assertNotIn('while the disk is full', text)

    def test_an_install_goes_on_from_the_unpacked_game_files(self):
        ctx = self.make_ctx('ok')
        game = self.tmp / 'game'
        first = jobs.InstallJob(ctx, self.tmp / 'g.nup', game).args()
        self.assertEqual(first[:2], ['--nup', str(self.tmp / 'g.nup')])
        made_up_game(game, steps=('exefs', 'romfs'), exe=False)
        again = jobs.InstallJob(ctx, self.tmp / 'g.nup', game).args()
        self.assertEqual(again[:2], ['--rebuild', '--retune'])
        self.assertNotIn('--nup', again)
        (game / 'romfs' / 'romfs.bin').unlink()
        self.assertEqual(jobs.InstallJob(ctx, self.tmp / 'g.nup', game).args()[0], '--nup')

    def test_a_reader_sees_each_step_as_one_change_left_it(self):
        ctx = self.make_ctx('ok')
        self.addCleanup(ctx._file.close)
        j = jobs.Job(ctx, ['lift', 'compile'])
        stop = threading.Event()
        wrong, errors = [], []

        def work():
            while not stop.is_set():
                for sid in ('lift', 'compile'):
                    j.begin(sid, 'starting')
                    j.progress(sid, 0.5, 'half')
                    j.finish(sid)
                    j.log(f'working on {sid}')
                    with j.locked():
                        j.step(sid).state, j.step(sid).value, j.step(sid).fraction = jobs.PENDING, 'Waiting', 0.0

        def read():
            try:
                while not stop.is_set():
                    with j.locked():
                        seen = [(s.state, s.value, s.fraction) for s in j.steps]
                    for state, value, fraction in seen:
                        ok = {jobs.PENDING: value == 'Waiting' and fraction == 0.0,
                              jobs.RUNNING: value.endswith('%') and value == f'{int(fraction * 100)}%',
                              jobs.DONE: value.startswith('Done') and fraction == 1.0}[state]
                        if not ok:
                            wrong.append((state, value, fraction))
                    j.last_lines(6)
            except Exception as e:   # noqa: BLE001 - whatever a reader meets is the failure
                errors.append(e)

        threads = [threading.Thread(target=work), threading.Thread(target=read), threading.Thread(target=read)]
        for t in threads:
            t.start()
        time.sleep(0.5)
        stop.set()
        for t in threads:
            t.join(30)
        self.assertEqual((wrong[:3], errors), ([], []))

        counted = jobs.Job(ctx, [])
        touchers = [threading.Thread(target=lambda: [counted._touch() for _ in range(20000)]) for _ in range(4)]
        for t in touchers:
            t.start()
        for t in touchers:
            t.join(60)
        self.assertEqual(counted.rev, 80000)
        try:
            from launcher.ui.app import App
        except ImportError:
            return
        a = mock.MagicMock()
        a.job = jobs.InstallJob(ctx, self.tmp / 'g.nup', self.tmp / 'game')
        a.job.begin('tools')
        a.job.progress('tools', 0.25)
        rows = App.rows_job(a)
        self.assertEqual((rows[0].kind, rows[0].value, rows[0].fraction), ('progress', '25%', 0.25))
        self.assertEqual([r.kind for r in rows].count('divider'), 1)
        self.assertEqual(rows[-1].kind, 'action')


HOLDER = ('import sys; sys.path.insert(0, sys.argv[1]); from launcher import locks; '
          'lock = locks.FileLock(sys.argv[2], sys.argv[3]); print("held", flush=True); sys.stdin.read()')


class LockTest(Tmp):
    make_ctx, patch_tools = JobTest.make_ctx, JobTest.patch_tools

    def holder(self, layout, what='the launcher window'):
        p = subprocess.Popen([sys.executable, '-c', HOLDER, str(ROOT), str(layout.instance_lock), what],
                             stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)

        def end():
            p.kill()
            p.wait()
            p.stdin.close()
            p.stdout.close()
        self.addCleanup(end)
        self.assertEqual(p.stdout.readline().strip(), 'held')
        return p

    def test_a_second_launcher_is_refused_and_told_who_holds_the_folder(self):
        other = self.holder(self.layout)
        with self.assertRaises(locks.Busy) as refused:
            locks.FileLock(self.layout.instance_lock, 'the launcher window', where=self.layout.root)
        self.assertIn(f'the launcher window (process {other.pid}, since ', str(refused.exception))
        self.assertIn(str(self.layout.root), str(refused.exception))
        try:
            from launcher.ui import app
        except ImportError as e:
            self.skipTest(f'no window toolkit: {e}')
        said = []
        with mock.patch.object(app, 'tell', said.append), mock.patch.object(app.tk, 'Tk') as window, \
                mock.patch.object(app.winsys, 'dpi_aware'), mock.patch.object(app.winsys, 'app_id'), \
                mock.patch.object(app, 'single_instance', lambda layout, wait: locks.FileLock(
                    layout.instance_lock, 'the launcher window', where=layout.root)):
            self.assertEqual(app.main(['--root', str(self.layout.root)]), 0)
        window.assert_not_called()
        self.assertEqual(len(said), 1)
        self.assertIn(f'NX2Saria is already running: the launcher window (process {other.pid}', said[0])

    def test_the_lock_goes_when_its_process_dies(self):
        other = self.holder(self.layout)
        other.kill()
        other.wait()
        mine = locks.FileLock(self.layout.instance_lock, 'the launcher window', wait=10)
        self.addCleanup(mine.release)
        job = locks.FileLock(self.layout.instance_lock, 'a job')
        job.release()
        job.release()
        self.assertIn(locks._key(self.layout.instance_lock), locks._files)
        mine.release()
        self.assertNotIn(locks._key(self.layout.instance_lock), locks._files)

    def test_a_folder_that_cannot_hold_a_lock_file_refuses_nobody(self):
        gone = self.tmp / 'no such folder' / 'launcher.lock'
        with locks.FileLock(gone, 'the launcher window'), locks.FileLock(gone, 'another'):
            self.assertFalse(gone.exists())

    def test_a_job_does_not_run_while_another_process_holds_the_folder(self):
        self.patch_tools()
        ctx = self.make_ctx('ok')
        self.addCleanup(ctx._file.close)
        other = self.holder(ctx.layout, '"rebuild" from the command line')
        j = jobs.InstallJob(ctx, self.tmp / 'g.nup', self.tmp / 'game').run_inline()
        self.assertEqual(j.result, 'failed')
        self.assertIn(f'"rebuild" from the command line (process {other.pid}', j.error)
        self.assertIn('try again', j.error)
        self.assertEqual({s.state for s in j.steps}, {jobs.PENDING})
        self.assertFalse(any('working on' in ln for ln in j.lines))
        check = jobs.Job(ctx, [])
        check.run = lambda: None
        self.assertEqual(check.run_inline().result, 'ok')
        other.stdin.close()
        other.wait()
        j = jobs.InstallJob(ctx, self.tmp / 'g.nup', self.tmp / 'game').run_inline()
        self.assertEqual(j.result, 'ok', j.error)
        self.assertEqual(locks._files, {})

    def test_two_jobs_on_one_folder(self):
        self.patch_tools()
        ctx = self.make_ctx('ok')
        self.addCleanup(ctx._file.close)
        started, go = threading.Event(), threading.Event()

        class Slow(jobs.RebuildJob):
            def run(self):
                started.set()
                go.wait(30)

        first = Slow(ctx, self.tmp / 'game').start()
        self.addCleanup(go.set)
        self.assertTrue(started.wait(30))
        second = jobs.RebuildJob(ctx, self.tmp / 'game').run_inline()
        self.assertEqual(second.result, 'failed')
        self.assertEqual(second.error, 'the rebuild of the game is still running: wait for it to end, or cancel it')
        self.assertEqual({s.state for s in second.steps}, {jobs.PENDING})
        install = jobs.InstallJob(ctx, self.tmp / 'g.nup', self.tmp / 'game').run_inline()
        self.assertEqual(install.result, 'failed')
        self.assertIn('the rebuild of the game is still running', install.error)

        class Download(jobs.Job):
            claims = ('app',)
            task = 'the background download of 9.9.9'

            def run(self):
                pass

        self.assertEqual(Download(ctx, []).run_inline().result, 'ok')
        elsewhere = Context(Layout(root=self.tmp / 'other root', app=ctx.layout.app))
        self.addCleanup(elsewhere._file.close)
        self.assertEqual(jobs.InstallJob(elsewhere, self.tmp / 'g.nup', self.tmp / 'game 2').run_inline().result, 'ok')
        go.set()
        for _ in range(300):
            if first.result is not None:
                break
            time.sleep(0.1)
        self.assertEqual(first.result, 'ok')
        again = jobs.InstallJob(ctx, self.tmp / 'g.nup', self.tmp / 'game').run_inline()
        self.assertEqual(again.result, 'ok', again.error)
        self.assertEqual((locks._files, locks._claims), ({}, {}))

    def test_a_job_the_launcher_was_stopped_in_is_told_at_the_next_start(self):
        self.patch_tools()
        ctx = self.make_ctx('ok')
        self.addCleanup(ctx._file.close)
        marker = ctx.layout.data / jobs.MARKER
        during = []

        class Watched(jobs.RebuildJob):
            def run(self):
                during.append(json.loads(marker.read_text(encoding='utf-8')))

        self.assertEqual(Watched(ctx, self.tmp / 'game').run_inline().result, 'ok')
        self.assertEqual((during[0]['task'], during[0]['kind'], during[0]['game_dir']),
                         ('the rebuild of the game', 'rebuild', str(self.tmp / 'game')))
        self.assertFalse(marker.exists())
        self.assertIsNone(jobs.interrupted(ctx))

        class Failing(jobs.RebuildJob):
            def run(self):
                raise build.BuildError('the build stopped')

        self.assertEqual(Failing(ctx, self.tmp / 'game').run_inline().result, 'failed')
        self.assertFalse(marker.exists())
        check = jobs.Job(ctx, [])
        self.assertFalse(check._mark())
        self.assertTrue(jobs.InstallJob(ctx, self.tmp / 'g.nup', self.tmp / 'game')._mark())
        self.assertTrue(marker.exists())
        self.assertEqual(jobs.interrupted(ctx)['task'], 'the installation of the game')
        self.assertFalse(marker.exists())
        self.assertIsNone(jobs.interrupted(ctx))
        marker.write_text('{"task": ')
        self.assertIsNone(jobs.interrupted(ctx))
        self.assertFalse(marker.exists())

    def test_the_window_says_which_job_did_not_finish(self):
        game = made_up_game(self.tmp / 'game')
        App, a = mock_app(self, game)
        a.args.selftest = False
        a.ctx.zstd_missing = ''
        self.assertTrue(jobs.RebuildJob(a.ctx, game)._mark())
        App.start(a)
        a.show.assert_called_once_with('home')
        self.assertIn('stopped during the rebuild of the game', a.message)
        self.assertIn('Rebuild the Game finishes', a.message)
        a.ctx._file.flush()
        self.assertIn('the launcher was stopped during the rebuild of the game',
                      (self.layout.logs / 'launcher.log').read_text(encoding='utf-8'))
        App, a = mock_app(self, self.tmp / 'no game yet')
        a.args.selftest = False
        a.ctx.zstd_missing = ''
        self.assertTrue(jobs.InstallJob(a.ctx, self.tmp / 'g.nup', self.tmp / 'no game yet')._mark())
        App.start(a)
        a.show.assert_called_once_with('first_run')
        self.assertIn('stopped during the installation of the game', a.message)
        self.assertIn('Begin Installation goes on', a.message)
        App, a = mock_app(self, game)
        a.args.selftest = False
        a.ctx.zstd_missing = ''
        App.start(a)
        self.assertEqual(a.message, '')

    def test_a_failed_job_lets_go_of_the_folder(self):
        self.patch_tools()
        ctx = self.make_ctx('--fail')
        self.addCleanup(ctx._file.close)
        self.assertEqual(jobs.InstallJob(ctx, self.tmp / 'g.nup', self.tmp / 'game').run_inline().result, 'failed')
        self.assertEqual((locks._files, locks._claims), ({}, {}))

    def test_the_command_line_takes_the_folders_lock(self):
        from launcher import cli
        ctx = self.make_ctx('ok')
        ctx._file.close()
        root = ['--root', str(ctx.layout.root)]
        ran = []

        def run(a, ctx, held):
            ran.append((a.cmd, held is not None and locks._key(ctx.layout.instance_lock) in locks._files))
            ctx._file.close()
            return 0
        err = io.StringIO()
        with mock.patch.object(cli, 'run', run), contextlib.redirect_stderr(err):
            self.assertEqual(cli.main(root + ['tools']), 0)
            self.assertEqual(ran, [('tools', True)])
            self.assertEqual(locks._files, {})
            other = self.holder(ctx.layout)
            self.assertEqual(cli.main(root + ['rebuild']), 1)
            self.assertEqual(cli.main(root + ['install', '--dump', 'd.nup', '--game', str(self.tmp / 'game')]), 1)
            self.assertEqual(len(ran), 1)
            self.assertEqual(cli.main(root + ['check']), 0)
            self.assertEqual(ran[-1], ('check', False))
        self.assertIn(f'error: the launcher window (process {other.pid}', err.getvalue())
        self.assertIn('try again', err.getvalue())

    def test_the_command_line_update_refuses_while_the_game_runs(self):
        import argparse
        from launcher import cli
        ctx = mock.MagicMock()
        ctx.settings = {'game_dir': str(self.tmp / 'game')}
        err = io.StringIO()
        with mock.patch('launcher.cli.build.game_running', return_value=True) as running, \
                mock.patch('launcher.cli.jobs.CheckJob') as check, \
                mock.patch('launcher.cli.jobs.UpdateDownloadJob') as download, contextlib.redirect_stderr(err):
            self.assertEqual(cli.run(argparse.Namespace(cmd='update'), ctx, mock.MagicMock()), 1)
        running.assert_called_once_with(str(self.tmp / 'game'))
        check.assert_not_called()
        download.assert_not_called()
        self.assertIn('error: the game is running', err.getvalue())


class SavesTest(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix='nx2navi-saves-'))
        self.game = self.tmp / 'game'
        self.game.mkdir()

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def files(self, folder, names):
        for n, data in names.items():
            (folder / n).parent.mkdir(parents=True, exist_ok=True)
            (folder / n).write_bytes(data)
        return folder

    def test_account_folder_as_the_game_names_it(self):
        (self.game / 'profile').mkdir()
        lo, hi = 0x1122334455667788, 0x99aabbccddeeff00
        (self.game / 'profile' / 'account.uid').write_bytes(lo.to_bytes(8, 'little') + hi.to_bytes(8, 'little'))
        self.assertEqual(saves.account_dir(self.game).name, f'{hi:016x}{lo:016x}')
        fresh = self.tmp / 'fresh'
        acct = saves.account_dir(fresh)
        self.assertEqual(len((fresh / 'profile' / 'account.uid').read_bytes()), 16)
        self.assertEqual(saves.account_dir(fresh), acct)

    def test_save_folders_found(self):
        plain = self.files(self.tmp / 'backup', {'save.dat': b'1', 'option.dat': b'2'})
        self.assertEqual(saves.find_save(plain)[0], plain)
        emu = self.files(self.tmp / 'emu', {'0000000000000000/' + 'a' * 32 + '/0100000000010000/save.dat': b'1'})
        self.assertEqual(saves.find_save(emu)[0].name, '0100000000010000')
        other = self.files(self.tmp / 'other', {'profile/save/' + 'b' * 32 + '/save.dat': b'1', 'ocarina.exe': b''})
        self.assertEqual(saves.find_save(other)[0].name, 'b' * 32)
        self.assertEqual(saves.find_save(other / 'profile')[0].name, 'b' * 32)
        (self.tmp / 'empty').mkdir()
        self.assertIsNone(saves.find_save(self.tmp / 'empty')[0])

    def test_import_backs_up_and_clears_the_journal(self):
        acct = saves.account_dir(self.game)
        self.files(acct, {'save.dat': b'old'})
        tx = self.game / 'profile' / 'save-tx'
        self.files(tx, {'ready-3/save.dat': b'newer', 'work/save.dat': b'w'})
        src = self.files(self.tmp / 'mine', {'save.dat': b'imported', 'sub/extra.bin': b'x'})
        r = saves.import_save(self.game, src, log=lambda m: None)
        self.assertEqual(r['files'], 2)
        self.assertEqual((acct / 'save.dat').read_bytes(), b'imported')
        self.assertEqual((acct / 'sub' / 'extra.bin').read_bytes(), b'x')
        self.assertEqual(list(tx.iterdir()), [])
        self.assertEqual((r['backup'] / 'save.dat').read_bytes(), b'newer')
        self.assertTrue((self.game / 'saves' / 'README.txt').is_file())
        self.assertTrue((src / 'save.dat').is_file())

    def test_one_slot_replaces_only_that_slot(self):
        acct = saves.account_dir(self.game)
        self.files(acct, {'account.sav': b'a', 'option.sav': b'o', 'slot_00/progress.sav': b'old0',
                          'slot_01/progress.sav': b'old1', 'slot_01/extra.sav': b'e'})
        src = self.files(self.tmp / 'slot_01', {'slot_01/progress.sav': b'new1', 'slot_01/caption.sav': b'c1'})
        r = saves.import_save(self.game, src, log=lambda m: None)
        self.assertEqual(r['slot'], 'slot_01')
        self.assertEqual((acct / 'slot_01' / 'progress.sav').read_bytes(), b'new1')
        self.assertEqual((acct / 'slot_01' / 'caption.sav').read_bytes(), b'c1')
        self.assertFalse((acct / 'slot_01' / 'extra.sav').exists())
        self.assertFalse((acct / 'progress.sav').exists())
        for name, data in (('slot_00/progress.sav', b'old0'), ('account.sav', b'a'), ('option.sav', b'o')):
            self.assertEqual((acct / name).read_bytes(), data)
        self.assertEqual((r['backup'] / 'slot_01' / 'progress.sav').read_bytes(), b'old1')

    def test_a_slot_under_another_name_is_the_first_slot(self):
        src = self.files(self.tmp / 'my save', {'progress.sav': b'p', 'caption.sav': b'c'})
        r = saves.import_save(self.game, src, log=lambda m: None)
        acct = saves.account_dir(self.game)
        self.assertEqual(r['slot'], 'slot_00')
        self.assertIsNone(r['backup'])
        self.assertEqual((acct / 'slot_00' / 'progress.sav').read_bytes(), b'p')

    def test_a_slot_clears_a_slot_left_at_the_top_of_the_save(self):
        acct = saves.account_dir(self.game)
        self.files(acct, {'progress.sav': b'stray', 'caption.sav': b'stray', 'option.sav': b'o'})
        src = self.files(self.tmp / 'slot_02', {'progress.sav': b'p'})
        r = saves.import_save(self.game, src, log=lambda m: None)
        self.assertFalse((acct / 'progress.sav').exists())
        self.assertFalse((acct / 'caption.sav').exists())
        self.assertEqual((acct / 'option.sav').read_bytes(), b'o')
        self.assertEqual((acct / 'slot_02' / 'progress.sav').read_bytes(), b'p')
        self.assertEqual((r['backup'] / 'progress.sav').read_bytes(), b'stray')

    def test_a_whole_save_replaces_the_save(self):
        acct = saves.account_dir(self.game)
        self.files(acct, {'slot_00/progress.sav': b'old', 'slot_03/progress.sav': b'old3', 'option.sav': b'o'})
        src = self.files(self.tmp / 'whole', {'account.sav': b'a', 'option.sav': b'n', 'slot_00/progress.sav': b'new'})
        r = saves.import_save(self.game, src, log=lambda m: None)
        self.assertIsNone(r['slot'])
        self.assertEqual((acct / 'slot_00' / 'progress.sav').read_bytes(), b'new')
        self.assertEqual((acct / 'option.sav').read_bytes(), b'n')
        self.assertFalse((acct / 'slot_03').exists())

    def test_a_backup_comes_back_without_its_journal(self):
        src = self.files(self.tmp / 'backup 2026-11-09 05.42.06', {'option.sav': b'o', 'slot_00/progress.sav': b'p',
                                                                   'save-tx/work/option.sav': b'w'})
        saves.import_save(self.game, src, log=lambda m: None)
        acct = saves.account_dir(self.game)
        self.assertEqual((acct / 'slot_00' / 'progress.sav').read_bytes(), b'p')
        self.assertFalse((acct / 'save-tx').exists())

    def test_import_from_the_saves_folder(self):
        d = saves.ensure_saves_dir(self.game)
        self.assertIsNone(saves.waiting(self.game)[0])
        self.files(d, {'emu/0000000000000000/' + 'c' * 32 + '/0100000000010000/save.dat': b'emu'})
        self.assertIsNotNone(saves.waiting(self.game)[0])
        r = saves.import_save(self.game, log=lambda m: None)
        self.assertIsNone(r['backup'])
        self.assertEqual((saves.account_dir(self.game) / 'save.dat').read_bytes(), b'emu')
        left = sorted(e.name for e in d.iterdir())
        self.assertEqual(left[0], 'README.txt')
        self.assertTrue(left[1].startswith('imported '))
        self.assertIsNone(saves.waiting(self.game)[0])
        self.files(d, {'save.dat': b'loose'})
        r = saves.import_save(self.game, log=lambda m: None)
        self.assertEqual((saves.account_dir(self.game) / 'save.dat').read_bytes(), b'loose')
        self.assertEqual((r['backup'] / 'save.dat').read_bytes(), b'emu')
        self.assertIsNone(saves.waiting(self.game)[0])

    def test_picking_the_saves_folder_takes_what_waits_there(self):
        acct = saves.account_dir(self.game)
        self.files(acct, {'save.dat': b'mine'})
        d = saves.ensure_saves_dir(self.game)
        with self.assertRaises(saves.SaveError):
            saves.import_save(self.game, d, log=lambda m: None)
        self.assertEqual((acct / 'save.dat').read_bytes(), b'mine')
        self.files(d, {'save.dat': b'put there'})
        r = saves.import_save(self.game, d, log=lambda m: None)
        self.assertEqual(sorted(p.name for p in acct.iterdir()), ['save.dat'])
        self.assertEqual((acct / 'save.dat').read_bytes(), b'put there')
        self.assertEqual((r['backup'] / 'save.dat').read_bytes(), b'mine')

    def test_an_interrupted_commit_is_backed_up(self):
        for half in ('new', 'old'):
            with self.subTest(half=half):
                shutil.rmtree(self.game, ignore_errors=True)
                self.game.mkdir()
                acct = saves.account_dir(self.game)
                tx = self.game / 'profile' / 'save-tx'
                self.files(tx, {f'{half}/save.dat': b'only copy'})
                src = self.files(self.tmp / f'mine-{half}', {'save.dat': b'imported'})
                r = saves.import_save(self.game, src, log=lambda m: None)
                self.assertEqual((acct / 'save.dat').read_bytes(), b'imported')
                self.assertEqual((r['backup'] / 'save.dat').read_bytes(), b'only copy')
                self.assertEqual((r['backup'] / 'save-tx' / half / 'save.dat').read_bytes(), b'only copy')

    def test_a_failed_swap_keeps_the_old_save(self):
        acct = saves.account_dir(self.game)
        self.files(acct, {'save.dat': b'old'})
        src = self.files(self.tmp / 'mine', {'save.dat': b'imported'})
        real = saves.os.replace

        def replace(a, b):
            if Path(a).name == 'import':
                raise PermissionError(5, 'Access is denied')
            return real(a, b)
        saves.os.replace = replace
        try:
            with self.assertRaises(PermissionError):
                saves.import_save(self.game, src, log=lambda m: None)
        finally:
            saves.os.replace = real
        self.assertEqual((acct / 'save.dat').read_bytes(), b'old')

    def test_a_failed_swap_keeps_the_finished_commit(self):
        acct = saves.account_dir(self.game)
        self.files(acct, {'save.dat': b'older'})
        tx = self.game / 'profile' / 'save-tx'
        self.files(tx, {'ready-4/save.dat': b'newest'})
        src = self.files(self.tmp / 'mine', {'save.dat': b'imported'})
        real = saves.os.replace

        def replace(a, b):
            if Path(a) == acct:
                raise PermissionError(32, 'The process cannot access the file')
            return real(a, b)
        saves.os.replace = replace
        try:
            with self.assertRaises(PermissionError):
                saves.import_save(self.game, src, log=lambda m: None)
        finally:
            saves.os.replace = real
        self.assertEqual((tx / 'ready-4' / 'save.dat').read_bytes(), b'newest')
        self.assertEqual((acct / 'save.dat').read_bytes(), b'older')
        self.assertFalse((tx / 'import').exists())

    @unittest.skipUnless(os.name == 'nt', 'the running check is Windows only')
    def test_a_read_only_exe_is_not_a_running_game(self):
        exe = self.game / 'ocarina.exe'
        exe.write_bytes(b'MZ')
        os.chmod(exe, stat.S_IREAD)
        self.assertFalse(build.game_running(self.game))
        self.assertFalse(saves.game_running(self.game))
        self.assertTrue(os.access(exe, os.W_OK))

    @unittest.skipUnless(os.name == 'nt', 'the running check is Windows only')
    def test_only_a_sharing_violation_is_a_running_game(self):
        exe = self.game / 'ocarina.exe'
        exe.write_bytes(b'MZ')
        answers = []
        with mock.patch('launcher.build._write_open_error', side_effect=lambda p: answers.pop(0)) as opened:
            answers[:] = [32]
            self.assertTrue(build.game_running(self.game))
            answers[:] = [0]
            self.assertFalse(build.game_running(self.game))
            answers[:] = [5]
            lines = []
            self.assertFalse(build.game_running(self.game, log=lines.append))
            self.assertEqual(len(lines), 1)
            self.assertIn('cannot be opened for writing (access denied)', lines[0])
            self.assertEqual(opened.call_count, 3)
            os.chmod(exe, stat.S_IREAD)
            answers[:] = [5, 32]
            self.assertTrue(build.game_running(self.game))
            self.assertTrue(os.access(exe, os.W_OK))
            os.chmod(exe, stat.S_IREAD)
            answers[:] = [5, 5]
            lines = []
            self.assertFalse(build.game_running(self.game, log=lines.append))
            self.assertEqual(len(lines), 1)
            self.assertEqual(answers, [])
        self.assertEqual(build._write_open_error(exe), 0)
        sleeper = self.game / 'sleeper.exe'
        if not compiled_sleeper(sleeper, 3000, 0):
            self.skipTest('needs a C compiler')
        proc = winsys.start_game(sleeper)
        try:
            self.assertEqual(build._write_open_error(sleeper), build.ERROR_SHARING_VIOLATION)
            self.assertTrue(build.game_running(self.game, exe='sleeper.exe'))
        finally:
            proc.kill()
            proc.wait(10)
        self.assertFalse(build.game_running(self.game, exe='sleeper.exe'))

    def test_read_only_files_import_writable(self):
        acct = saves.account_dir(self.game)
        self.files(acct, {'save.dat': b'old'})
        src = self.files(self.tmp / 'mine', {'save.dat': b'imported'})
        os.chmod(src / 'save.dat', stat.S_IREAD)
        os.chmod(acct / 'save.dat', stat.S_IREAD)
        try:
            saves.import_save(self.game, src, log=lambda m: None)
            self.assertTrue(os.access(acct / 'save.dat', os.W_OK))
            self.assertEqual([p.name for p in (self.game / 'profile' / 'save-tx').iterdir()], [])
        finally:
            os.chmod(src / 'save.dat', stat.S_IREAD | stat.S_IWRITE)

    def test_another_installs_finished_commit_is_its_save(self):
        other = self.files(self.tmp / 'other', {'profile/save/' + 'b' * 32 + '/save.dat': b'older',
                                                'profile/save-tx/ready-2/save.dat': b'newest'})
        self.assertEqual(saves.find_save(other)[0].name, 'ready-2')

    def test_refusals(self):
        acct = saves.account_dir(self.game)
        self.files(acct, {'save.dat': b'1'})
        with self.assertRaises(saves.SaveError):
            saves.import_save(self.game, self.game / 'profile', log=lambda m: None)
        big = self.tmp / 'big'
        big.mkdir()
        for i in range(saves.MAX_FILES + 1):
            (big / f'{i}.txt').write_bytes(b'')
        with self.assertRaises(saves.SaveError):
            saves.import_save(self.game, big, log=lambda m: None)
        self.assertEqual((acct / 'save.dat').read_bytes(), b'1')

    @staticmethod
    def game_lock(path):
        if os.name == 'nt':
            import ctypes
            from ctypes import wintypes
            k = ctypes.WinDLL('kernel32', use_last_error=True)
            k.CreateFileW.restype = wintypes.HANDLE
            k.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p,
                                      wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
            k.CloseHandle.argtypes = [wintypes.HANDLE]
            h = k.CreateFileW(str(path), 0xC0000000, 0, None, 4, 0x80, None)
            if h in (None, wintypes.HANDLE(-1).value):
                return None
            return lambda: k.CloseHandle(h)
        import fcntl
        f = open(path, 'a+b')
        try:
            fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError:
            f.close()
            return None
        return f.close

    def test_no_import_while_the_game_holds_its_profile(self):
        acct = saves.account_dir(self.game)
        self.files(acct, {'save.dat': b'old'})
        src = self.files(self.tmp / 'theirs', {'save.dat': b'new'})
        lock = self.game / 'profile' / 'instance.lock'
        let_go = self.game_lock(lock)
        self.assertIsNotNone(let_go)
        with self.assertRaises(saves.SaveError) as refused:
            saves.import_save(self.game, src, log=lambda m: None)
        self.assertIn('the game is running', str(refused.exception))
        self.assertEqual((acct / 'save.dat').read_bytes(), b'old')
        self.assertFalse(saves.saves_dir(self.game).exists())
        self.assertFalse((self.game / 'profile' / 'save-tx' / 'import').exists())
        let_go()
        started = []
        real_copytree = shutil.copytree

        def copytree(a, b, *args, **kw):
            started.append(self.game_lock(lock))
            return real_copytree(a, b, *args, **kw)
        with mock.patch.object(shutil, 'copytree', copytree):
            r = saves.import_save(self.game, src, log=lambda m: None)
        self.assertEqual(started, [None])
        self.assertEqual((acct / 'save.dat').read_bytes(), b'new')
        self.assertEqual((r['backup'] / 'save.dat').read_bytes(), b'old')
        let_go = self.game_lock(lock)
        self.assertIsNotNone(let_go)
        let_go()


class SaveShareTest(unittest.TestCase):
    SAVE = {'account.sav': b'acct', 'option.sav': b'opt', 'slot_00/progress.sav': b'p0', 'slot_00/caption.sav': b'c0',
            'slot_02/progress.sav': b'p2'}

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix='nx2navi-share-'))
        self.game = self.tmp / 'game'
        self.out = self.tmp / 'out'
        self.out.mkdir()

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def files(self, folder, names):
        for n, data in names.items():
            (folder / n).parent.mkdir(parents=True, exist_ok=True)
            (folder / n).write_bytes(data)
        return folder

    def game_with_save(self, game, names=None):
        self.files(saves.account_dir(game), names or self.SAVE)
        return game

    def zip_of(self, members, manifest=None):
        path = self.tmp / f'made-{len(list(self.tmp.glob("made-*")))}.zip'
        with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED) as z:
            if manifest is not False:
                m = manifest or {'format': saves.FORMAT, 'format_version': 1, 'title_id': '0400486024dec000',
                                 'game_version': '1.1.1', 'nx2navi': '0.6.1', 'exported': '2026-11-09T17:30:05Z'}
                z.writestr(saves.MANIFEST, json.dumps(m))
            for n, data in members.items():
                z.writestr(n, data)
        return path

    def test_export_writes_one_zip_of_the_newest_save(self):
        self.game_with_save(self.game)
        tx = self.game / 'profile' / 'save-tx'
        self.files(tx, {'ready-7/option.sav': b'newer', 'ready-7/slot_00/progress.sav': b'p0 newer',
                        'work/option.sav': b'not committed'})
        self.files(self.game, {'logs/nx2navi.log': b'log', 'nx2navi.ini': b'[graphics]'})
        r = saves.export_save(self.game, self.out, version='0.6.1', log=lambda m: None)
        self.assertRegex(r['path'].name, r'^NX2Saria-save-\d{4}-\d\d-\d\d_\d\d-\d\d-\d\d\.zip$')
        self.assertEqual(r['extras'], [])
        self.assertEqual([p.name for p in self.out.iterdir()], [r['path'].name])
        with zipfile.ZipFile(r['path']) as z:
            names = sorted(z.namelist())
            self.assertEqual(names, ['manifest.json', 'save/option.sav', 'save/slot_00/progress.sav'])
            self.assertEqual(z.read('save/option.sav'), b'newer')
            m = json.loads(z.read('manifest.json'))
        self.assertEqual((m['format'], m['format_version'], m['title_id'], m['game_version'], m['nx2navi'],
                          m['files'], m['bug_report']), (saves.FORMAT, 1, '0400486024dec000', '1.1.1', '0.6.1', 2,
                                                         False))
        self.assertRegex(m['exported'], r'^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ$')
        r2 = saves.export_save(self.game, self.out, log=lambda m: None)
        self.assertNotEqual(r2['path'], r['path'])

    def test_a_bug_report_adds_the_log_and_settings_and_never_keys(self):
        self.game_with_save(self.game, dict(self.SAVE, **{'prod.keys': b'secret', 'sub/title.keys': b'secret'}))
        self.files(self.game, {'logs/nx2navi.log': b'the log', 'logs/nx2navi.1.log': b'older', 'nx2navi.ini': b'[x]',
                               'prod.keys': b'secret'})
        r = saves.export_save(self.game, self.out, bug_report=True, log=lambda m: None)
        self.assertTrue(r['path'].name.endswith('-bug-report.zip'))
        self.assertEqual(r['extras'], ['nx2saria.log', 'nx2saria.ini'])
        with zipfile.ZipFile(r['path']) as z:
            names = z.namelist()
            self.assertEqual(z.read('bug-report/nx2saria.log'), b'the log')
            self.assertEqual(z.read('bug-report/nx2saria.ini'), b'[x]')
            self.assertTrue(json.loads(z.read('manifest.json'))['bug_report'])
            self.assertFalse(any(b'secret' in z.read(n) for n in names))
        self.assertFalse([n for n in names if 'keys' in n])
        self.assertEqual(len([n for n in names if n.startswith('save/')]), len(self.SAVE))

    def test_new_report_paths_win_when_new_and_legacy_files_both_exist(self):
        self.game_with_save(self.game)
        self.files(self.game, {'logs/nx2saria.log': b'current log', 'logs/nx2navi.log': b'old log',
                               'nx2saria.ini': b'[current]', 'nx2navi.ini': b'[legacy]'})
        report = saves.export_save(self.game, self.out, bug_report=True, log=lambda m: None)
        with zipfile.ZipFile(report['path']) as z:
            self.assertEqual(z.read('bug-report/nx2saria.log'), b'current log')
            self.assertEqual(z.read('bug-report/nx2saria.ini'), b'[current]')
        self.assertEqual((self.game / 'logs' / 'nx2navi.log').read_bytes(), b'old log')
        self.assertEqual((self.game / 'nx2navi.ini').read_bytes(), b'[legacy]')

    def test_a_bug_report_keeps_the_private_settings_values_out(self):
        ini = (b'\xef\xbb\xbf[coop]\r\ncoop = 1\r\n  Coop_Room = H7K3WM-BCDFGH-234567\r\ncoop_server=relay.example:24680\r\n'
               b'# coop_name = in a comment\r\ncoop_name =\r\ncoop_accept = 1a2b3c4d\nfps_limit = 60')
        self.game_with_save(self.game, self.SAVE)
        self.files(self.game, {'logs/nx2navi.log': b'the log', 'nx2navi.ini': ini})
        r = saves.export_save(self.game, self.out, bug_report=True, log=lambda m: None)
        with zipfile.ZipFile(r['path']) as z:
            got = z.read('bug-report/nx2saria.ini')
        self.assertEqual(got, b'\xef\xbb\xbf[coop]\r\ncoop = 1\r\n  Coop_Room = (set, not shown)\r\n'
                              b'coop_server= (set, not shown)\r\n# coop_name = in a comment\r\ncoop_name =\r\n'
                              b'coop_accept = (set, not shown)\nfps_limit = 60')
        for secret in (b'H7K3WM', b'relay.example', b'1a2b3c4d'):
            self.assertNotIn(secret, got)
        self.assertEqual(saves.scrub_settings(b'[x]'), b'[x]')
        self.assertEqual(saves.scrub_settings(b''), b'')

    def test_no_save_no_export(self):
        with self.assertRaises(saves.SaveError):
            saves.export_save(self.game, self.out, log=lambda m: None)
        self.assertFalse((self.game / 'profile' / 'account.uid').exists())
        saves.account_dir(self.game).mkdir(parents=True)
        with self.assertRaises(saves.SaveError):
            saves.export_save(self.game, self.out, log=lambda m: None)
        self.assertEqual(list(self.out.iterdir()), [])

    def test_a_bug_report_without_a_save_holds_the_log_and_settings(self):
        with self.assertRaises(saves.SaveError):
            saves.export_save(self.game, self.out, bug_report=True, log=lambda m: None)
        self.assertFalse(self.out.exists() and list(self.out.iterdir()))
        self.files(self.game, {'logs/nx2navi.log': b'FATAL: it stopped', 'nx2navi.ini': b'[x]', 'prod.keys': b'secret'})
        with self.assertRaises(saves.SaveError):
            saves.export_save(self.game, self.out, log=lambda m: None)
        for account in (False, True):
            if account:
                saves.account_dir(self.game).mkdir(parents=True)
            with self.subTest(account=account):
                r = saves.export_save(self.game, self.out, bug_report=True, version='0.7.1', log=lambda m: None)
                self.assertRegex(r['path'].name, r'^NX2Saria-log-.*-bug-report( \(\d+\))?\.zip$')
                self.assertEqual((r['files'], r['extras']), (0, ['nx2saria.log', 'nx2saria.ini']))
                with zipfile.ZipFile(r['path']) as z:
                    self.assertEqual(sorted(z.namelist()), ['bug-report/nx2saria.ini', 'bug-report/nx2saria.log',
                                                            'manifest.json'])
                    self.assertEqual(z.read('bug-report/nx2saria.log'), b'FATAL: it stopped')
                    m = json.loads(z.read('manifest.json'))
                    self.assertEqual((m['format'], m['files'], m['bug_report'], m['nx2navi']),
                                     (saves.FORMAT, 0, True, '0.7.1'))
                with self.assertRaises(saves.SaveError):
                    saves.import_zip(self.game_with_save(self.tmp / f'other{account}'), r['path'], log=lambda m: None)
        self.assertFalse(list(self.out.glob('*.part')))

    def test_a_shared_save_replaces_the_save_and_keeps_a_backup(self):
        other = self.game_with_save(self.tmp / 'other')
        shared = saves.export_save(other, self.out, bug_report=True, version='0.6.1', log=lambda m: None)['path']
        self.game_with_save(self.game, {'option.sav': b'mine', 'slot_05/progress.sav': b'mine5'})
        acct = saves.account_dir(self.game)
        self.files(self.game / 'profile' / 'save-tx', {'ready-2/option.sav': b'mine newer'})
        r = saves.import_save(self.game, shared, log=lambda m: None)
        self.assertEqual(r['manifest']['nx2navi'], '0.6.1')
        self.assertIsNone(r['slot'])
        got = {p.relative_to(acct).as_posix(): p.read_bytes() for p in acct.rglob('*') if p.is_file()}
        self.assertEqual(got, self.SAVE)
        self.assertFalse((acct / 'nx2navi.log').exists())
        self.assertEqual((r['backup'] / 'option.sav').read_bytes(), b'mine newer')
        self.assertEqual(r['backup'].parent, self.game / 'saves')
        self.assertEqual(list((self.game / 'profile' / 'save-tx').iterdir()), [])
        self.assertTrue(shared.is_file())

    def test_a_zip_in_the_saves_folder_is_taken_first(self):
        self.game_with_save(self.game)
        shared = saves.export_save(self.game_with_save(self.tmp / 'other', {'option.sav': b'theirs'}), self.out,
                                   log=lambda m: None)['path']
        d = saves.ensure_saves_dir(self.game)
        self.files(d, {'loose.sav': b'x'})
        shutil.copy2(shared, d / shared.name)
        src, _ = saves.waiting(self.game)
        self.assertEqual(src, d / shared.name)
        r = saves.import_save(self.game, log=lambda m: None)
        self.assertEqual((saves.account_dir(self.game) / 'option.sav').read_bytes(), b'theirs')
        self.assertFalse((saves.account_dir(self.game) / shared.name).exists())
        done = [e for e in d.iterdir() if e.name.startswith('imported ')]
        self.assertEqual([p.name for p in done[0].iterdir()], [shared.name])
        self.assertEqual(saves.waiting(self.game)[0], d)
        self.assertIsNotNone(r['backup'])
        shutil.copy2(shared, d / 'again.zip')
        r = saves.import_save(self.game, d, log=lambda m: None)
        self.assertEqual(r['source'].name, 'again.zip')

    def test_an_import_in_the_launcher_drops_the_game_menus_waiting_import(self):
        self.game_with_save(self.game)
        self.files(self.game / 'profile', {'save-import/option.sav': b'from the menu',
                                           'save-import.part/option.sav': b'half'})
        self.assertIsNotNone(saves.pending_import(self.game))
        shared = saves.export_save(self.game_with_save(self.tmp / 'other'), self.out, log=lambda m: None)['path']
        saves.import_save(self.game, shared, log=lambda m: None)
        self.assertIsNone(saves.pending_import(self.game))
        self.assertFalse((self.game / 'profile' / 'save-import.part').exists())

    def test_zips_that_are_refused(self):
        acct = saves.account_dir(self.game_with_save(self.game))
        good = {'save/option.sav': b'o', 'save/slot_00/progress.sav': b'p'}
        manifest = {'format': saves.FORMAT, 'format_version': 1, 'title_id': '0400486024dec000'}
        notzip = self.tmp / 'not.zip'
        notzip.write_bytes(b'PK not really')
        cases = {
            'not a zip': notzip,
            'no manifest': self.zip_of(good, manifest=False),
            'another format': self.zip_of(good, dict(manifest, format='something-else')),
            'a newer format': self.zip_of(good, dict(manifest, format_version=2)),
            'no format version': self.zip_of(good, {'format': saves.FORMAT, 'title_id': '0400486024dec000'}),
            'another game': self.zip_of(good, dict(manifest, title_id='0100000000010000')),
            'a path outside the save': self.zip_of(dict(good, **{'save/../../evil.sav': b'x'}), manifest),
            'an absolute path': self.zip_of(dict(good, **{'save//abs.sav': b'x'}), manifest),
            'a drive': self.zip_of(dict(good, **{'save/C:/evil.sav': b'x'}), manifest),
            'a backslash': self.zip_of(dict(good, **{'save/..\\evil.sav': b'x'}), manifest),
            'not this game': self.zip_of({'save/save.dat': b'x', 'save/user/0001.bin': b'y'}, manifest),
            'no save': self.zip_of({'bug-report/nx2navi.log': b'l'}, manifest),
        }
        for why, path in cases.items():
            with self.subTest(why=why):
                with self.assertRaises(saves.SaveError):
                    saves.import_save(self.game, path, log=lambda m: None)
                self.assertEqual((acct / 'option.sav').read_bytes(), self.SAVE['option.sav'])
        self.assertFalse((self.tmp / 'evil.sav').exists())
        self.assertFalse((self.game / 'saves').exists() and
                         [e for e in (self.game / 'saves').iterdir() if e.name.startswith('backup ')])

    def test_a_link_in_the_zip_is_refused(self):
        acct = saves.account_dir(self.game_with_save(self.game))
        path = self.zip_of({'save/option.sav': b'o'})
        with zipfile.ZipFile(path, 'a') as z:
            info = zipfile.ZipInfo('save/slot_00/progress.sav')
            info.external_attr = (stat.S_IFLNK | 0o777) << 16
            z.writestr(info, '../../../outside')
        with self.assertRaises(saves.SaveError):
            saves.import_save(self.game, path, log=lambda m: None)
        self.assertEqual((acct / 'option.sav').read_bytes(), self.SAVE['option.sav'])

    def test_a_damaged_zip_changes_nothing(self):
        acct = saves.account_dir(self.game_with_save(self.game))
        path = self.zip_of({'save/option.sav': b'theirs' * 200, 'save/slot_00/progress.sav': b'p' * 300})
        data = bytearray(path.read_bytes())
        at = data.index(b'save/option.sav') + len('save/option.sav') + 4
        data[at] ^= 0xff
        path.write_bytes(bytes(data))
        with self.assertRaises(saves.SaveError):
            saves.import_save(self.game, path, log=lambda m: None)
        self.assertEqual((acct / 'option.sav').read_bytes(), self.SAVE['option.sav'])
        self.assertFalse((self.game / 'profile' / 'save-tx' / 'import').exists())

    def test_the_zip_of_the_games_menu_imports(self):
        path = self.tmp / 'from-the-game.zip'
        with zipfile.ZipFile(path, 'w') as z:
            z.writestr(saves.MANIFEST, json.dumps({'format': saves.FORMAT, 'format_version': 1, 'exported_by': 'game',
                                                   'title_id': '0400486024DEC000', 'game_version': '1.1.1'}))
            z.writestr('save/option.sav', b'stored', zipfile.ZIP_STORED)
            z.writestr('save/slot_01/progress.sav', b'deflated' * 50, zipfile.ZIP_DEFLATED)
        r = saves.import_save(self.game, path, log=lambda m: None)
        self.assertIsNone(r['backup'])
        acct = saves.account_dir(self.game)
        self.assertEqual((acct / 'slot_01' / 'progress.sav').read_bytes(), b'deflated' * 50)

    @unittest.skipUnless(os.name == 'nt', 'the shell\'s known folders are Windows only')
    def test_the_default_export_folder(self):
        d = saves.default_export_dir()
        self.assertTrue(d.is_dir())
        self.assertIn(d, (winsys.known_folder('downloads'), winsys.known_folder('desktop'), Path.home()))


class IconTest(unittest.TestCase):
    def test_leaf_sprig_icon_has_packed_green_pixels(self):
        try:
            from launcher.ui import art
        except ImportError:
            self.skipTest('needs numpy')
        self.assertEqual(art.sprig_icon(64).shape, (64, 64, 4))
        data = art.sprig_ico()
        count = struct.unpack_from('<H', data, 4)[0]
        sizes = [data[6 + 16 * i] or 256 for i in range(count)]
        self.assertEqual(sizes[0], 16)
        self.assertIn(256, sizes)
        off = struct.unpack_from('<I', data, 6 + 12)[0]
        self.assertEqual(struct.unpack_from('<Iii', data, off), (40, 16, 32))
        big = struct.unpack_from('<I', data, 6 + 16 * (count - 1) + 12)[0]
        self.assertEqual(data[big:big + 4], b'\x89PNG')
        ix = sizes.index(64)
        offset = struct.unpack_from('<I', data, 6 + 16 * ix + 12)[0]
        self.assertIn(bytes((0xa0, 0xcb, 0xa9, 0xff)), data[offset + 40:offset + 40 + 64 * 64 * 4])


class LookTest(Tmp):
    def setUp(self):
        super().setUp()
        try:
            from launcher.ui import art, picture, sound
        except ImportError:
            self.skipTest('needs numpy')
        self.art, self.picture, self.sound = art, picture, sound

    def test_every_colour_scheme_has_every_colour(self):
        art = self.art
        self.assertEqual(art.theme_palette('forest'), art.BASE)
        for name, t in art.THEMES.items():
            pal = art.theme_palette(name)
            self.assertEqual(set(pal), set(art.BASE), name)
            for key, value in pal.items():
                self.assertRegex(value, r'^#[0-9a-f]{6}$', f'{name} {key}')
            self.assertTrue(t['label'])
        grouped = [k for keys in art.GROUPS.values() for k in keys]
        self.assertEqual(len(grouped), len(set(grouped)), 'a colour in two groups')
        self.assertLessEqual(set(grouped), set(art.BASE))

    def test_set_theme_changes_the_palette_in_place(self):
        art = self.art
        held = art.PALETTE
        try:
            self.assertEqual(art.set_theme('lake'), 'lake')
            self.assertIs(art.PALETTE, held)
            self.assertNotEqual(held['panel'], art.BASE['panel'])
            self.assertEqual(held['salmon'], art.BASE['salmon'])
            self.assertEqual(art.set_theme('no such scheme'), 'forest')
            self.assertEqual(dict(held), art.BASE)
            self.assertEqual(art.set_theme(None), 'forest')
        finally:
            art.set_theme('forest')

    def test_recolor_keeps_light_and_dark(self):
        import colorsys
        art = self.art

        def hls(c):
            return colorsys.rgb_to_hls(*(int(c[i:i + 2], 16) / 255 for i in (1, 3, 5)))
        for c in ('#7a5c32', '#2a2019', '#d69127'):
            h, l, _s = hls(art.recolor(c, 200))
            self.assertAlmostEqual(h * 360, 200, delta=4)
            self.assertAlmostEqual(l, hls(c)[1], delta=0.01)
        self.assertEqual(art.recolor('#7a5c32', None), '#7a5c32')

    def test_a_picture_is_cut_to_the_windows_shape(self):
        import numpy as np
        pic = self.picture
        wide = np.zeros((100, 400, 3), dtype=np.uint8)
        wide[:, 150:250] = 255
        out = pic.cover(wide, 160, 100)
        self.assertEqual(out.shape, (100, 160, 3))
        self.assertGreater(out[:, 40:120].mean(), 200)
        tall = np.zeros((400, 100, 3), dtype=np.uint8)
        tall[150:250] = 255
        self.assertEqual(pic.cover(tall, 160, 100).shape, (100, 160, 3))
        self.assertGreater(pic.cover(tall, 160, 100).mean(), 120)
        big = np.full((2048, 3200, 3), 77, dtype=np.uint8)
        self.assertTrue((pic.cover(big, 1600, 1024) == 77).all())

    def test_the_kept_background(self):
        import numpy as np
        art, pic = self.art, self.picture
        src = self.tmp / 'mine.png'
        img = np.zeros((90, 300, 4), dtype=np.float32)
        img[..., 0], img[..., 3] = 1.0, 1.0
        src.write_bytes(art.png_bytes(img))
        store = self.tmp / 'background.npy'
        if pic.Image is None and os.name != 'nt':
            self.skipTest('needs Pillow, or Tk with a display')
        fitted = pic.keep(src, store)
        self.assertEqual(fitted.shape, (pic.KEEP_H, pic.KEEP_W, 3))
        back = pic.kept(store, 1000, 640)
        self.assertEqual(back.shape, (640, 1000, 3))
        self.assertEqual(str(back.dtype), 'float32')
        self.assertAlmostEqual(float(back[..., 0].min()), 1.0, places=3)
        self.assertAlmostEqual(float(back[..., 1:].max()), 0.0, places=3)
        plain = art.backdrop(200, 128, 1.0, pic.kept(store, 200, 128), 0.0)
        dimmed = art.backdrop(200, 128, 1.0, pic.kept(store, 200, 128), 0.8)
        self.assertEqual(plain.shape, (128, 200, 4))
        self.assertGreater(plain[64, 100, 0], 0.8)
        self.assertLess(dimmed[64, 100, 0], 0.35)
        self.assertEqual(float(dimmed[..., 3].min()), 1.0)
        self.assertIsNone(pic.kept(self.tmp / 'none.npy', 100, 64))
        store.write_bytes(b'not numpy')
        self.assertIsNone(pic.kept(store, 100, 64))
        text = self.tmp / 'notes.txt'
        text.write_text('no picture')
        with self.assertRaises(pic.PictureError):
            pic.keep(text, store)
        with self.assertRaises(pic.PictureError):
            pic.load(self.tmp / 'gone.png')

    def test_the_custom_scheme(self):
        art = self.art
        self.assertEqual(art.custom_palette({}), art.BASE)
        slots = art.CUSTOM_SLOTS
        self.assertEqual(list(slots), ['background', 'panel', 'buttons'])
        parts = [k for _, keys, _ in slots.values() for k in keys]
        self.assertEqual(len(parts), len(set(parts)), 'a colour in two parts')
        self.assertEqual(set(parts), set(art.GROUPS['scene'] + art.GROUPS['wood'] + art.GROUPS['accent']))
        pal = art.custom_palette({'background': '#3A7BD5', 'panel': '#203040', 'buttons': '#802020'})
        self.assertEqual(pal['tint_mid'], '#3a7bd5')
        self.assertEqual(pal['panel'], '#203040')
        self.assertEqual(pal['label_mid'], '#802020')
        only = art.custom_palette({'buttons': '#802020'})
        self.assertLessEqual({k for k in art.BASE if only[k] != art.BASE[k]}, set(slots['buttons'][1]))
        for k in ('label_top', 'label_bottom', 'value', 'orange', 'orange_light'):
            self.assertNotEqual(only[k], art.BASE[k], k)
        for k in ('panel', 'desc', 'wood_1', 'window', 'tint_mid'):
            self.assertEqual(only[k], art.BASE[k], k)
        self.assertEqual(round(art.hls(only['orange'])[0] * 360), 0)
        self.assertGreater(art.hls(only['orange'])[1], art.hls(only['label_mid'])[1])
        panel = art.custom_palette({'panel': '#203040'})
        self.assertLessEqual({k for k in art.BASE if panel[k] != art.BASE[k]}, set(slots['panel'][1]))
        self.assertEqual(panel['label_mid'], art.BASE['label_mid'])
        for slot, (ref, keys, _) in slots.items():
            for k in keys:
                if art.hls(art.BASE[k])[1] > art.hls(art.BASE[ref])[1] + 0.02:
                    self.assertGreater(art.hls(pal[k])[1], art.hls(pal[ref])[1], k)
        self.assertGreater(art.luminance(pal['text']), 0.6)
        light = art.custom_palette({'buttons': '#f0e0a0'})
        self.assertLess(art.luminance(light['text']), 0.3)
        self.assertGreater(art.luminance(light['desc_text']), 0.6)
        light = art.custom_palette({'panel': '#f0e0a0'})
        self.assertLess(art.luminance(light['desc_text']), 0.3)
        self.assertGreater(art.luminance(light['text']), 0.6)
        self.assertEqual(art.custom_palette({'buttons': 'red', 'background': None}), art.BASE)
        held = art.PALETTE
        try:
            self.assertEqual(art.set_theme('custom', {'buttons': '#802020', 'other': '#000000'}), 'custom')
            self.assertEqual(held['label_mid'], '#802020')
            self.assertEqual(art.CUSTOM, {'background': art.BASE['tint_mid'], 'panel': art.BASE['panel'],
                                          'buttons': '#802020'})
            art.set_theme('custom', {})
            self.assertEqual(dict(held), art.BASE)
        finally:
            art.set_theme('forest', {})

    def test_the_colour_square(self):
        import numpy as np
        art = self.art
        sq = art.color_square(40, 30, 0.0)
        self.assertEqual(sq.shape, (30, 40, 4))
        self.assertTrue(np.allclose(sq[0, -1, :3], [1, 0, 0], atol=0.05))
        self.assertLess(sq[-1, :, :3].max(), 0.05)
        self.assertLess(np.ptp(sq[0, 0, :3]), 0.05)
        bar = art.hue_bar(60, 4)
        self.assertTrue(np.allclose(bar[0, 20, :3], [0, 1, 0], atol=0.1))
        self.assertTrue(np.allclose(bar[0, 40, :3], [0, 0, 1], atol=0.1))

    def test_a_typed_hex_code(self):
        try:
            from launcher.ui.app import App
        except ImportError:
            self.skipTest('needs tkinter')
        parse = App.parse_hex
        self.assertEqual(parse('#3A7BD5'), (58, 123, 213))
        self.assertEqual(parse(' 3a7bd5 '), (58, 123, 213))
        self.assertEqual(parse('#37d'), (0x33, 0x77, 0xdd))
        self.assertIsNone(parse('#3A7BD'))
        self.assertIsNone(parse('blue'))

    def test_the_picture_starts_below_the_band_and_is_never_stretched(self):
        import numpy as np
        art, pic = self.art, self.picture
        store = self.tmp / 'background.npy'
        old = np.zeros((1024, 1600, 3), dtype=np.uint8)
        old[:, :800, 0] = 255
        np.save(store, old)
        self.assertEqual(pic.kept_shape(store), (1024, 1600))
        back = pic.kept(store, 1000, 580)
        self.assertEqual(back.shape, (580, 1000, 3))
        self.assertGreater(back[:, 490, 0].min(), 0.95)
        self.assertLess(back[:, 510, 0].max(), 0.05)
        img = art.backdrop(200, 128, 1.0, np.ones((116, 200, 3), dtype=np.float32), 0.0, top=12)
        self.assertLess(img[6, 100, :3].max(), 0.2)
        self.assertGreater(img[20, 100, :3].min(), 0.7)

    def test_ppm(self):
        import numpy as np
        px = bytes(range(18))
        a = self.picture.read_ppm(b'P6\n# a comment\n3 2\n255\n' + px)
        self.assertEqual(a.shape, (2, 3, 3))
        self.assertEqual(a.tobytes(), px)
        self.assertEqual(a.dtype, np.uint8)
        with self.assertRaises(self.picture.PictureError):
            self.picture.read_ppm(b'P5\n1 1\n255\n\0')

    @staticmethod
    def bwav(channels, rate=48000):
        out = bytearray(b'BWAV' + struct.pack('<HHIHH', 0xfeff, 1, 0, 0, len(channels)))
        at = 0x10 + 0x4c * len(channels)
        for codec, pan, count, data in channels:
            info = bytearray(0x4c)
            struct.pack_into('<HHIII', info, 0, codec, pan, rate, count, count)
            struct.pack_into('<I', info, 0x30, at)
            out += info
            at += len(data)
        for _codec, _pan, _count, data in channels:
            out += data
        return bytes(out)

    def game_sounds(self, names=('Do', 'Cancel')):
        dump = self.tmp / 'dump'
        (dump / 'exefs').mkdir(parents=True, exist_ok=True)
        (dump / 'exefs' / 'main').write_bytes(b'')
        stream = dump / 'romfs' / 'Sound' / 'Resource' / 'Stream'
        stream.mkdir(parents=True, exist_ok=True)
        loud = struct.pack('<4h', 0, 16000, -16000, 8000)
        for name in names:
            (stream / f'SE_Sys_{name}00.bwav').write_bytes(self.bwav([(0, 0, 4, loud), (0, 1, 4, loud)]))
        return gamefiles.extract_sounds(self.layout, '', str(dump))

    def test_the_games_interface_sounds(self):
        sound = self.sound
        mono = self.bwav([(0, 2, 3, struct.pack('<3h', 1000, -2000, 3000))])
        self.assertEqual(gamefiles.bwav_to_pcm(mono), (48000, [(707, 707), (-1414, -1414), (2121, 2121)]))
        frame = bytes([0x08, 0x1f, 0x70]) + bytes(5)
        both = self.bwav([(1, 0, 3, frame), (1, 1, 2, frame)])
        self.assertEqual(gamefiles.bwav_to_pcm(both), (48000, [(256, 256), (-256, -256), (1792, 0)]))
        for bad in (b'', b'RIFF' + bytes(60), mono[:0x30], self.bwav([(7, 2, 3, bytes(6))]), mono[:-2]):
            self.assertIsNone(gamefiles.bwav_to_pcm(bad))
        got = self.game_sounds(('Do',))
        (self.tmp / 'dump' / 'romfs' / 'Sound' / 'Resource' / 'Stream' / 'SE_Sys_Cancel00.bwav').write_bytes(b'no sound')
        self.assertEqual(set(got), {'select'})
        self.assertEqual(gamefiles.cached_sounds(self.layout), got)
        with mock.patch.object(sound.os, 'name', 'posix'), mock.patch.object(sound.shutil, 'which', return_value=None):
            s = sound.Sounds(self.layout.cache, 'high')
        self.assertEqual(set(s.game), {'select'})
        wav = s.wav('select')
        self.assertEqual(wav[:4] + wav[8:16], b'RIFFWAVEfmt ')
        self.assertEqual(struct.unpack_from('<I', wav, 4)[0], len(wav) - 8)
        self.assertEqual(struct.unpack_from('<HHI', wav, 20), (1, 2, 48000))
        peak = max(abs(v) for v in struct.unpack_from('<8h', wav, 44))
        self.assertAlmostEqual(peak / 32767, sound.LEVELS['high'], delta=0.01)
        self.assertFalse(sound.Sounds(self.tmp / 'empty', 'high').game)

    def test_only_the_games_sounds_play_at_low_unless_set(self):
        sound = self.sound
        self.assertEqual(settings.DEFAULTS['sounds'], 'low')
        self.assertEqual(sound.DEFAULT_LEVEL, 'low')
        self.assertEqual(set(gamefiles.SOUNDS), set(sound.EFFECTS))
        empty = self.tmp / 'empty'
        self.game_sounds()
        with mock.patch.object(sound.os, 'name', 'posix'), mock.patch.object(sound.shutil, 'which',
                                                                             return_value='/usr/bin/aplay'), \
                mock.patch.object(sound.subprocess, 'Popen') as popen:
            quiet = sound.Sounds(empty, 'high')
            quiet.play('select')
            quiet.play('back')
            popen.assert_not_called()
            quiet.cache = self.layout.cache
            self.assertTrue(quiet.load_game())
            quiet.play('select')
            self.assertEqual(popen.call_count, 1)
            s = sound.Sounds(self.layout.cache)
            self.assertEqual(s.level, 'low')
            s.play('select')
            self.assertEqual(popen.call_count, 2)
            s.set_level('off')
            s.play('select')
            self.assertEqual(popen.call_count, 2)
            s.set_level('nonsense')
            self.assertEqual(s.level, 'low')
            s.play('back')
            s.play('move')
            s.play('no such effect')
            self.assertEqual(popen.call_count, 3)
            with open(popen.call_args[0][0][1], 'rb') as f:
                self.assertEqual(f.read(4), b'RIFF')
            peak = max(abs(v) for v in struct.unpack_from('<8h', s.wav('back'), 44))
            self.assertAlmostEqual(peak / 32767, sound.LEVELS['low'], delta=0.01)

    def test_the_look_settings_survive_and_fall_back(self):
        path = self.tmp / 'launcher.json'
        s = settings.Settings(path)
        self.assertEqual((s['theme'], s['background'], s['background_dim'], s['animations'], s['sounds']),
                         ('forest', '', 40, True, 'low'))
        s['theme'], s['sounds'], s['background_dim'] = 'shadow', 'high', 60
        s.save()
        t = settings.Settings(path)
        self.assertEqual((t['theme'], t['sounds'], t['background_dim']), ('shadow', 'high', 60))
        path.write_text(json.dumps({'theme': 5, 'animations': 'yes', 'background_dim': '40'}))
        u = settings.Settings(path)
        self.assertEqual((u['theme'], u['animations'], u['background_dim']), ('forest', True, 40))


def have_display():
    if os.name == 'nt':
        return True
    if not os.environ.get('DISPLAY'):
        return False
    try:
        import tkinter  # noqa: F401
        import numpy  # noqa: F401
        return True
    except ImportError:
        return False


class SteamTest(Tmp):
    def other(self):
        from launcher import steam
        exe = '"C:\\Games\\Other\\other.exe"'
        entry = [[steam.T_INT, 'appid', struct.pack('<I', steam.appid(exe, 'Other'))], [steam.T_STR, 'AppName', 'Other'],
                 [steam.T_STR, 'Exe', exe], [steam.T_STR, 'StartDir', '"C:\\Games\\Other\\"'],
                 [steam.T_STR, 'icon', ''], [steam.T_INT, 'LastPlayTime', struct.pack('<I', 1234)],
                 [steam.T_STR, 'sortas', ''], [steam.T_MAP, 'tags', [[steam.T_STR, '0', 'Favorite']]]]
        return steam.vdf_dump([[steam.T_MAP, 'shortcuts', [[steam.T_MAP, '0', entry]]]])

    def test_round_trip_add_and_update(self):
        from launcher import steam
        b = self.other()
        self.assertEqual(b[-4:], b'\x08\x08\x08\x08')
        self.assertEqual(steam.vdf_dump(steam.vdf_load(b)), b)
        self.assertFalse(steam.has_shortcut(b))
        new, sid = steam.add_shortcut(b, '"C:\\NX2Saria\\python\\pythonw.exe"', '"C:\\NX2Saria"',
                                      '"C:\\NX2Saria\\data\\steam_play.pyw"', 'C:\\grid\\x_icon.png')
        self.assertEqual(sid, steam.appid('"C:\\NX2Saria\\python\\pythonw.exe"', steam.NAME))
        self.assertTrue(sid & 0x80000000)
        self.assertTrue(new.startswith(b[:-2]))
        self.assertTrue(steam.has_shortcut(new))
        items = steam.vdf_load(new)[0][2]
        self.assertEqual([k for _, k, _ in items], ['0', '1'])
        ours = {k: v for _, k, v in items[1][2]}
        self.assertEqual(ours['AppName'], steam.NAME)
        self.assertEqual(struct.unpack('<I', ours['appid'])[0], sid)
        self.assertEqual(ours['LaunchOptions'], '"C:\\NX2Saria\\data\\steam_play.pyw"')
        self.assertEqual(ours['AllowOverlay'], struct.pack('<I', 1))
        again, _ = steam.add_shortcut(new, '"D:\\NX2Saria\\python\\pythonw.exe"', '"D:\\NX2Saria"', '', 'x')
        items = steam.vdf_load(again)[0][2]
        self.assertEqual(len(items), 2)
        self.assertEqual({k: v for _, k, v in items[1][2]}['StartDir'], '"D:\\NX2Saria"')
        with self.assertRaises(steam.SteamError):
            steam.vdf_load(b[:-5])

    def test_legacy_shortcut_is_rebranded_in_place(self):
        from launcher import steam
        exe = '"C:\\NX2navi\\python\\pythonw.exe"'
        old, _ = steam.add_shortcut(b'', exe, '"C:\\NX2navi"', '"old-script.pyw"', 'old-icon.png',
                                    name=steam.LEGACY_NAMES[0])
        self.assertTrue(steam.has_shortcut(old))
        current, sid = steam.add_shortcut(old, '"C:\\NX2Saria\\python\\pythonw.exe"', '"C:\\NX2Saria"',
                                          '"new-script.pyw"', 'new-icon.png')
        entries = steam.vdf_load(current)[0][2]
        self.assertEqual(len(entries), 1)
        self.assertEqual(sid, steam.appid(exe, steam.LEGACY_NAMES[0]))
        ours = {key: value for _, key, value in entries[0][2]}
        self.assertEqual(ours['AppName'], steam.NAME)
        self.assertEqual(ours['icon'], 'new-icon.png')
        self.assertTrue(steam.has_shortcut(current))

    def test_new_file_and_accounts(self):
        from launcher import steam
        new, _ = steam.add_shortcut(b'', '"x.exe"', '"."', '', '')
        self.assertTrue(steam.has_shortcut(new))
        root = self.tmp / 'Steam'
        for a in ('0', '1046310423', '22'):
            (root / 'userdata' / a / 'config').mkdir(parents=True)
        (root / 'config').mkdir()
        (root / 'config' / 'loginusers.vdf').write_text('"users"\n{\n\t"76561199006576151"\n\t{\n\t}\n}\n')
        expected = sorted([root / 'userdata' / '1046310423' / 'config', root / 'userdata' / '22' / 'config']) \
            if sys.platform.startswith('linux') else [root / 'userdata' / '1046310423' / 'config']
        self.assertEqual(steam.accounts(root), expected)
        (root / 'config' / 'loginusers.vdf').unlink()
        self.assertEqual(steam.accounts(root), sorted([root / 'userdata' / '1046310423' / 'config',
                                                       root / 'userdata' / '22' / 'config']))

    def test_example_shortcuts_vdf_round_trips_byte_for_byte(self):
        from launcher import steam
        example = (ROOT / 'tests' / 'data' / 'steam' / 'shortcuts-example.vdf').read_bytes()
        self.assertEqual(steam.vdf_dump(steam.vdf_load(example)), example)
        original_entry = steam.vdf_load(example)[0][2][0][2]
        changed, _ = steam.add_shortcut(example, '"/home/ana/.local/share/nx2saria/nx2saria-launcher"',
                                        '"/home/ana/.local/share/nx2saria"', '--play', '')
        other = steam.vdf_load(changed)[0][2][0][2]
        self.assertEqual(steam.vdf_dump([[steam.T_MAP, '0', original_entry]]),
                         steam.vdf_dump([[steam.T_MAP, '0', other]]))


class LinuxSteamTest(Tmp):
    def setUp(self):
        super().setUp()
        if not sys.platform.startswith('linux'):
            self.skipTest('Linux Steam paths and process checks')
        self.home = self.tmp / 'home'
        self.home.mkdir()

    def roots_with_accounts(self):
        from launcher import steam
        roots = (self.home / '.steam' / 'steam', self.home / '.local' / 'share' / 'Steam',
                 self.home / '.var' / 'app' / 'com.valvesoftware.Steam' / '.local' / 'share' / 'Steam')
        for index, root in enumerate(roots, 410100):
            (root / 'userdata' / str(index) / 'config').mkdir(parents=True)
        return roots

    def test_finds_native_and_flatpak_and_every_account(self):
        from launcher import steam
        roots = self.roots_with_accounts()
        self.assertEqual(steam.steam_roots(self.home), list(roots))
        self.assertEqual(steam.native_steam_roots(self.home), list(roots[:2]))
        self.assertEqual([cfg.parent.name for _number, cfg in steam.flatpak_accounts(self.home)], ['410102'])
        self.assertEqual([cfg.parent.name for root in roots for cfg in steam.accounts(root)],
                         ['410100', '410101', '410102'])

    def test_refuses_open_steam_and_writes_backup_then_updates_in_place_when_closed(self):
        import numpy as np
        from launcher import gameart, steam
        roots = self.roots_with_accounts()
        configs = [root / 'userdata' / str(410100 + i) / 'config' for i, root in enumerate(roots)]
        example = (ROOT / 'tests' / 'data' / 'steam' / 'shortcuts-example.vdf').read_bytes()
        (configs[0] / 'shortcuts.vdf').write_bytes(example)
        wrapper = self.layout.root / 'nx2saria-launcher'
        wrapper.write_text('#!/bin/sh\n')
        game_dir = self.tmp / 'game'
        game_dir.mkdir()
        (game_dir / 'ocarina').write_text('fake game')
        expected_sid = steam.appid(f'"{wrapper}"', steam.NAME)
        pixels = np.zeros((1, 1, 4), dtype=np.uint8)
        logs = []
        with mock.patch('pathlib.Path.home', return_value=self.home), \
                mock.patch('launcher.steam.steam_running', return_value=True):
            with self.assertRaisesRegex(steam.SteamError, 'Steam is open'):
                steam.add(self.layout, game_dir, log=logs.append)
        self.assertEqual([line.rsplit(': ', 1)[-1] for line in logs],
                         ['skipped Flatpak Steam; default sandbox has no host access', 'Steam is open', 'Steam is open'])
        self.assertFalse(any((cfg / 'shortcuts.vdf').exists() for cfg in configs[1:]))
        self.assertFalse(any((cfg / 'shortcuts.vdf.nx2saria-bak').exists() for cfg in configs))

        logs.clear()
        fake_art = {'portrait': pixels, 'wide': pixels, 'hero': pixels, 'logo': pixels}
        with mock.patch('pathlib.Path.home', return_value=self.home), \
                mock.patch('launcher.steam.steam_running', return_value=False), \
                mock.patch.object(gameart, 'make', return_value=fake_art):
            self.assertEqual(steam.add(self.layout, game_dir, log=logs.append), 'Added to Steam.')
            first = [(cfg / 'shortcuts.vdf').read_bytes() for cfg in configs[:2]]
            self.assertEqual(steam.add(self.layout, game_dir, log=logs.append), 'Updated in Steam.')
        self.assertEqual([line.rsplit(': ', 1)[-1].split(': ', 1)[-1].split()[0] for line in logs],
                         ['skipped', 'created', 'created', 'skipped', 'updated', 'updated'])
        self.assertIn(f'created shortcut {expected_sid}', logs[1])
        for index, cfg in enumerate(configs[:2]):
            vdf = cfg / 'shortcuts.vdf'
            self.assertEqual((cfg / 'shortcuts.vdf.nx2saria-bak').read_bytes(), first[index])
            entries = steam.vdf_load(vdf.read_bytes())[0][2]
            self.assertEqual(len(entries), 2 if index == 0 else 1)
            ours_entry = next(entry[2] for entry in entries if steam.has_shortcut(
                steam.vdf_dump([[steam.T_MAP, 'shortcuts', [entry]]])))
            ours = {key: value for _typ, key, value in ours_entry}
            self.assertEqual(ours['Exe'], f'"{wrapper}"')
            self.assertEqual(ours['StartDir'], f'"{self.layout.root}"')
            self.assertEqual(ours['LaunchOptions'], '--play')
            self.assertTrue((cfg / 'grid' / f'{expected_sid}_icon.png').is_file())
        self.assertFalse((configs[2] / 'shortcuts.vdf').exists())
        self.assertFalse((configs[2] / 'grid').exists())

    def test_flatpak_only_does_not_get_a_broken_shortcut(self):
        from launcher import steam
        root = self.home / '.var' / 'app' / 'com.valvesoftware.Steam' / '.local' / 'share' / 'Steam'
        config = root / 'userdata' / '410102' / 'config'
        config.mkdir(parents=True)
        logs = []
        with mock.patch('pathlib.Path.home', return_value=self.home), \
                mock.patch('launcher.steam.steam_running', return_value=False):
            with self.assertRaisesRegex(steam.SteamError, 'Steam Flatpak sandbox'):
                steam.add(self.layout, self.tmp, log=logs.append)
        self.assertEqual(logs, ['steam: account 1: skipped Flatpak Steam; default sandbox has no host access'])
        self.assertFalse((config / 'shortcuts.vdf').exists())
        from launcher.ui.app import App
        app = object.__new__(App)
        app.steam_flatpak_logged = False
        app.ctx = mock.Mock()
        with mock.patch('pathlib.Path.home', return_value=self.home):
            row = app.steam_row()
        self.assertEqual(row.value, 'Flatpak not supported')
        self.assertEqual(row.style, 'dim')
        self.assertIsNone(row.on)
        app.ctx.log.assert_called_once_with(
            'steam: account 1: skipped Flatpak Steam; default sandbox has no host access')

    def test_artwork_failure_still_creates_the_shortcut_and_leaf_icon(self):
        from launcher import gameart, steam
        root = self.home / '.local' / 'share' / 'Steam'
        config = root / 'userdata' / '410101' / 'config'
        config.mkdir(parents=True)
        wrapper = self.layout.root / 'nx2saria-launcher'
        wrapper.write_text('#!/bin/sh\n')
        game_dir = self.tmp / 'game'
        game_dir.mkdir()
        (game_dir / 'ocarina').touch()
        logs = []
        with mock.patch('pathlib.Path.home', return_value=self.home), \
                mock.patch('launcher.steam.steam_running', return_value=False), \
                mock.patch.object(gameart, 'make', side_effect=gameart.ArtError('neither dav1d nor ffmpeg found')):
            self.assertEqual(steam.add(self.layout, game_dir, log=logs.append), 'Added to Steam.')
        self.assertIn('artwork unavailable: neither dav1d nor ffmpeg found', logs[0])
        entries = steam.vdf_load((config / 'shortcuts.vdf').read_bytes())[0][2]
        ours = {key: value for _typ, key, value in entries[0][2]}
        sid = steam.appid(f'"{wrapper}"', steam.NAME)
        self.assertTrue((config / 'grid' / f'{sid}_icon.png').is_file())
        self.assertFalse((config / 'grid' / f'{sid}p.png').exists())

    def test_detects_native_script_binary_and_flatpak_in_fake_proc(self):
        from launcher import steam
        proc_root = self.tmp / 'proc'
        examples = (('steam.sh', 'bash', b'/bin/bash\0/home/example/.steam/steam.sh\0'),
                    ('ubuntu12_32', 'steam', b'/home/example/.steam/ubuntu12_32/steam\0'),
                    ('flatpak', 'steam', b'/app/bin/steam\0'))
        for index, (_kind, comm, cmdline) in enumerate(examples, 1):
            proc = proc_root / str(index)
            proc.mkdir(parents=True)
            (proc / 'comm').write_text(comm + '\n')
            (proc / 'cmdline').write_bytes(cmdline)
            self.assertTrue(steam.steam_running(proc_root))
            shutil.rmtree(proc)
        self.assertFalse(steam.steam_running(proc_root))

    def test_no_steam_logs_a_short_diagnostic(self):
        from launcher import steam
        logs = []
        with mock.patch('pathlib.Path.home', return_value=self.home):
            with self.assertRaisesRegex(steam.SteamError, 'Steam was not found'):
                steam.add(self.layout, self.tmp, log=logs.append)
        self.assertEqual(logs, ['steam: no Steam installation'])


class WhilePlayingTest(Tmp):
    def test_the_games_restart_does_not_bring_the_launcher_back(self):
        try:
            from launcher.ui import app
        except ImportError as e:
            self.skipTest(f'no window toolkit: {e}')
        App, GameRun = app.App, app.GameRun
        a = mock.MagicMock()
        a.settings = {'game_dir': str(self.tmp)}
        a.game_run = GameRun(mock.MagicMock(returncode=0, pid=11), str(self.tmp), time.time())
        a.game_run.proc.poll.return_value = 0
        with mock.patch('launcher.ui.app.saves.game_running', return_value=True), \
                mock.patch('launcher.ui.app.winsys.game_copy', return_value=None) as copy:
            App.watch_game(a)
        copy.assert_called_once_with(self.tmp / GAME_EXE, skip=11)
        a.root.deiconify.assert_not_called()
        a.root.after.assert_called_once()
        with mock.patch('launcher.ui.app.saves.game_running', return_value=False):
            App.watch_game(a)
        a.root.deiconify.assert_called_once()

    def test_the_restarted_copy_is_followed_for_its_exit_code(self):
        try:
            from launcher.ui import app
        except ImportError as e:
            self.skipTest(f'no window toolkit: {e}')
        App, GameRun = app.App, app.GameRun
        log = self.tmp / 'logs' / 'nx2navi.log'
        log.parent.mkdir()
        log.write_text('[host] gfx: window closed; restarting\n', encoding='utf-8')
        a = mock.MagicMock()
        a.message = ''
        a.settings = {'game_dir': str(self.tmp)}
        a.install_staged.return_value = False
        a.game_run = GameRun(mock.MagicMock(returncode=0, pid=11), str(self.tmp), time.time() - 60)
        a.game_run.proc.poll.return_value = 0
        copy = mock.MagicMock(returncode=None, pid=12, ended=False)
        copy.poll.return_value = None
        with mock.patch('launcher.ui.app.saves.game_running', return_value=True), \
                mock.patch('launcher.ui.app.winsys.game_copy', return_value=copy):
            App.watch_game(a)
            App.watch_game(a)
        self.assertIs(a.game_run.proc, copy)
        a.root.deiconify.assert_not_called()
        log.write_text('[host] boot\nFATAL: vk: the GPU device was lost (seen at: q). The run ends here\n',
                       encoding='utf-8')
        copy.returncode = 72
        copy.poll.return_value = 72
        with mock.patch('launcher.ui.app.saves.game_running', return_value=False):
            App.watch_game(a)
        a.ctx.log.assert_called_once_with('the game ended with code 72 after restarting itself; its log says: FATAL: '
                                          'vk: the GPU device was lost (seen at: q). The run ends here')
        self.assertIn('(code 72): vk: the GPU device was lost', a.message)
        self.assertIn('[launcher] the game ended with code 72 (the graphics device was lost',
                      log.read_text(encoding='utf-8'))
        self.assertIsNone(a.game_run)

    def test_a_restarted_copy_that_could_not_be_followed_ends_with_its_code_unknown(self):
        try:
            from launcher.ui import app
        except ImportError as e:
            self.skipTest(f'no window toolkit: {e}')
        App, GameRun = app.App, app.GameRun
        log = self.tmp / 'logs' / 'nx2navi.log'
        log.parent.mkdir()

        def run(text):
            log.write_text(text, encoding='utf-8')
            a = mock.MagicMock()
            a.message = ''
            a.settings = {'game_dir': str(self.tmp)}
            a.install_staged.return_value = False
            a.game_run = GameRun(mock.MagicMock(returncode=0, pid=11), str(self.tmp), time.time() - 60)
            a.game_run.proc.poll.return_value = 0
            with mock.patch('launcher.ui.app.saves.game_running', return_value=True), \
                    mock.patch('launcher.ui.app.winsys.game_copy', return_value=None):
                App.watch_game(a)
            with mock.patch('launcher.ui.app.saves.game_running', return_value=False):
                App.watch_game(a)
            return a

        a = run('[host] log: x\n[host] perf: the last line of play\n')
        a.ctx.log.assert_called_once_with('the game ended after restarting itself; the exit code of its last run is '
                                          'not known')
        self.assertEqual(a.message, '')
        text = log.read_text(encoding='utf-8')
        self.assertIn('[launcher] the game ended with code unknown (the game had restarted itself;', text)
        self.assertNotIn('code 0', text)
        a = run('[host] log: x\nFATAL: fs: another copy of the game is running\n')
        a.ctx.log.assert_called_once_with('the game ended after restarting itself; its log says: FATAL: fs: another '
                                          'copy of the game is running')
        self.assertIn("A copy started by the game's Restart ended with a fatal error: fs: another copy", a.message)
        self.assertNotIn('[launcher]', log.read_text(encoding='utf-8'))

    def test_the_games_settings_file_is_left_alone_while_it_runs(self):
        try:
            from launcher.ui.app import App
        except ImportError as e:
            self.skipTest(f'no window toolkit: {e}')
        game = self.tmp / 'game'
        game.mkdir()
        (game / 'nx2navi.ini').write_text('[discord]\ndiscord_rpc = 1\n', encoding='utf-8')
        a = mock.MagicMock()
        a.message = ''
        a.game_busy.return_value = True
        row = App.discord_row(a, str(game))
        before = (game / 'nx2navi.ini').read_bytes()
        row.on(False)
        self.assertEqual((game / 'nx2navi.ini').read_bytes(), before)
        self.assertTrue(row.get())
        self.assertIn('Quit the game first', a.message)
        a.game_busy.return_value = False
        row.on(False)
        self.assertEqual(gameini.get(game, 'discord_rpc'), '0')
        self.assertFalse(row.get())
        self.assertEqual(a.message, '')
        a.game_busy.return_value = True
        a.settings = {'game_dir': str(game)}
        App.switch_mod(a, mock.Mock(), False)
        self.assertEqual(gameini.get(game, 'MODS_DISABLED'), '')
        self.assertIn('Quit the game first', a.mods_message)

    def ended(self, code, log_text, started=None):
        from launcher.ui.app import App, GameRun
        log = self.tmp / 'logs' / 'nx2navi.log'
        log.parent.mkdir(exist_ok=True)
        if log_text is not None:
            log.write_text(log_text, encoding='utf-8')
        a = mock.MagicMock()
        a.message = ''
        a.settings = {'game_dir': str(self.tmp)}
        a.game_dir.return_value = str(self.tmp)
        proc = mock.MagicMock(returncode=code)
        proc.poll.return_value = code
        a.game_run = GameRun(proc, str(self.tmp), time.time() - 60 if started is None else started)
        a.install_staged.return_value = False
        with mock.patch('launcher.ui.app.saves.game_running', return_value=False):
            App.watch_game(a)
        return a, log

    @unittest.skipUnless(os.name == 'nt', 'Windows processes')
    def test_a_game_copy_is_found_by_its_exe_file(self):
        mine, other = self.tmp / 'mine' / 'sleeper.exe', self.tmp / 'other' / 'sleeper.exe'
        if not compiled_sleeper(mine, 4000, 72):
            self.skipTest('needs a C compiler')
        other.parent.mkdir()
        shutil.copy(mine, other)
        proc = winsys.start_game(mine)
        try:
            copy = winsys.game_copy(mine)
            self.assertIsNotNone(copy)
            self.assertEqual(copy.pid, proc.pid)
            self.assertIsNone(copy.poll())
            self.assertIsNone(winsys.game_copy(mine, skip=proc.pid))
            self.assertIsNone(winsys.game_copy(other))
            upper = winsys.game_copy(Path(str(mine).upper()))
            self.assertEqual(upper.pid, proc.pid)
            proc.wait(30)
            self.assertEqual(upper.poll(), 72)
            self.assertEqual(copy.poll(), 72)
            self.assertTrue(copy.ended)
            self.assertIsNone(copy.handle)
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait(10)

    def test_a_lost_device_shows_the_games_fatal_line(self):
        try:
            from launcher.ui import app
        except ImportError as e:
            self.skipTest(f'no window toolkit: {e}')
        fatal = 'vk: the GPU device was lost (seen at: q). The run ends here'
        a, log = self.ended(72, f'[host] x\nFATAL: {fatal}\n[host] fatal: memory commit: this process 9000 MB\n')
        self.assertEqual(a.message, f'The game ended with a fatal error (code 72): {fatal}. Its log is '
                                    f'{GAME_LOG} (older builds: nx2navi.log).')
        a.ctx.log.assert_called_once_with(f'the game ended with code 72; its log says: FATAL: {fatal}')
        self.assertIn('[launcher] the game ended with code 72 (the graphics device was lost',
                      log.read_text(encoding='utf-8'))
        self.assertTrue(app.exit_code_text(72).startswith('72 (the graphics device was lost'))
        a, _ = self.ended(72, '[host] vk: device fault: none\n')
        self.assertIn('The game ended with code 72 (the graphics device was lost', a.message)

    def test_a_fatal_line_after_code_0_is_the_restarted_copys(self):
        try:
            from launcher.ui import app   # noqa: F401
        except ImportError as e:
            self.skipTest(f'no window toolkit: {e}')
        a, log = self.ended(0, '[host] x\nFATAL: vk: the GPU device was lost (seen at: x)\n')
        self.assertEqual(a.message, "A copy started by the game's Restart ended with a fatal error: vk: the GPU device "
                                    f'was lost (seen at: x). Its log is {GAME_LOG} (older builds: nx2navi.log).')
        a.ctx.log.assert_called_once_with('the game ended after restarting itself; its log says: FATAL: vk: the GPU '
                                          'device was lost (seen at: x)')
        self.assertNotIn('[launcher]', log.read_text(encoding='utf-8'))
        old = time.time() - 3600
        log.write_text('FATAL: an earlier run started without the launcher\n', encoding='utf-8')
        os.utime(log, (old, old))
        a, _ = self.ended(0, None, started=time.time())
        self.assertEqual(a.message, '')
        a.ctx.log.assert_called_once_with('the game ended with code 0')

    def test_a_log_cut_at_its_cap_says_nothing_of_its_end(self):
        try:
            from launcher.ui import app
        except ImportError as e:
            self.skipTest(f'no window toolkit: {e}')
        a, log = self.ended(0, '[host] perf: x\n[host] log: the file reached 64 MiB, the rest of this run goes to '
                               'stderr only\n')
        self.assertIsNone(app.log_has_own_exit(self.tmp))
        a.ctx.log.assert_called_once_with('the game ended with code 0')
        self.assertNotIn('[launcher]', log.read_text(encoding='utf-8'))

    def play_from_steam(self, code, log_text, lock_held=False):
        from launcher.ui import app
        (self.tmp / GAME_EXE).write_bytes(b'MZ')
        log = self.tmp / 'logs' / 'nx2navi.log'
        log.parent.mkdir(exist_ok=True)
        log.write_text(log_text, encoding='utf-8')
        ctx = mock.MagicMock()
        ctx.settings = {'game_dir': str(self.tmp)}
        ctx.layout = self.layout
        proc = mock.MagicMock(returncode=code, pid=5)
        proc.poll.return_value = code
        told = []

        def tell(text):
            told.append((text, log.read_text(encoding='utf-8'), list(ctx.log.call_args_list)))
        held = mock.patch('launcher.ui.app.locks.FileLock', side_effect=locks.Busy('the launcher window is using x')) \
            if lock_held else contextlib.nullcontext()
        with mock.patch('launcher.ui.app.winsys.start_game', return_value=proc) as self.start_game, \
                mock.patch('launcher.ui.app.saves.game_running', return_value=False), \
                mock.patch('launcher.ui.app.mods.prepare', return_value=[]), \
                mock.patch('launcher.ui.app.build.can_rebuild', return_value=False), \
                mock.patch('launcher.ui.app.time.sleep'), \
                mock.patch('launcher.ui.app.tell', side_effect=tell), held:
            self.assertEqual(app.play_only(ctx), 0)
        return ctx, told, log

    def test_play_from_steam_waits_for_a_job_in_the_game_folder(self):
        try:
            from launcher.ui import app   # noqa: F401
        except ImportError as e:
            self.skipTest(f'no window toolkit: {e}')
        marker = self.layout.data / jobs.MARKER
        marker.write_text(json.dumps({'task': 'the rebuild of the game', 'kind': 'rebuild'}), encoding='utf-8')
        ctx, told, _ = self.play_from_steam(0, '[host] x\n', lock_held=True)
        self.start_game.assert_not_called()
        self.assertEqual([t[0] for t in told],
                         ['NX2Saria is busy with the rebuild of the game. Start the game when it is done.'])
        ctx.log.assert_called_once_with('play from Steam: not started: the launcher is busy with the rebuild of the '
                                        'game')
        self.assertTrue(marker.is_file())
        ctx, told, _ = self.play_from_steam(0, '[host] gfx: window closed; exiting\n')
        self.start_game.assert_called_once()
        self.assertEqual(told, [])
        marker.unlink()
        ctx, told, _ = self.play_from_steam(0, '[host] gfx: window closed; exiting\n', lock_held=True)
        self.start_game.assert_called_once()
        self.assertEqual(told, [])

    def test_an_end_of_a_game_started_from_steam_is_told(self):
        try:
            from launcher.ui import app   # noqa: F401
        except ImportError as e:
            self.skipTest(f'no window toolkit: {e}')
        ctx, told, log = self.play_from_steam(70, '[host] x\nFATAL: fs: another copy of the game is running\n')
        self.assertEqual(len(told), 1)
        text, game_log, calls = told[0]
        self.assertEqual(text, 'The game ended with a fatal error (code 70): fs: another copy of the game is running. '
                               f'Its log is {GAME_LOG} (older builds: nx2navi.log).')
        self.assertIn('[launcher] the game ended with code 70', game_log)
        self.assertIn(mock.call('play from Steam: the game ended with code 70; its log says: FATAL: fs: another copy '
                                'of the game is running'), calls)
        _, told, _ = self.play_from_steam(0xC0000005, '[host] x\n')
        self.assertEqual([t[0] for t in told], ['The game ended with code 0xC0000005 (access violation). Its log is '
                                                f'{GAME_LOG} (older builds: nx2navi.log).'])
        ctx, told, log = self.play_from_steam(0, '[host] x\n[host] gfx: window closed; exiting\n')
        self.assertEqual(told, [])
        ctx.log.assert_called_with('play from Steam: the game ended with code 0')
        ctx, told, log = self.play_from_steam(1, '[host] perf: the last line\n')
        self.assertEqual(told, [])
        ctx.log.assert_called_with('play from Steam: the game ended with code 1')
        self.assertIn('[launcher] the game ended with code 1 at', log.read_text(encoding='utf-8'))

    def test_a_fatal_exit_shows_the_games_fatal_line(self):
        try:
            from launcher.ui import app
        except ImportError as e:
            self.skipTest(f'no window toolkit: {e}')
        fatal = 'vk: vkQueueSubmit2 of 3 held submissions up to value 83931 failed (-4)'
        a, log = self.ended(70, f'[host] log: x\nFATAL: an earlier one\n[host] more\nFATAL: {fatal}\n'
                                '[host] fatal: memory commit: this process 9000 MB, the system 30000 of 32000 MB\n')
        self.assertEqual(a.message, f'The game ended with a fatal error (code 70): {fatal}. Its log is '
                                    f'{GAME_LOG} (older builds: nx2navi.log).')
        a.ctx.log.assert_called_once_with(f'the game ended with code 70; its log says: FATAL: {fatal}')
        self.assertIn('[launcher] the game ended with code 70', log.read_text(encoding='utf-8'))
        long = 'access violation (0xc0000005) at host 0x7ff6 (nx2navi+0x1234), reading address 0x10 ' + 'x' * 300
        a, log = self.ended(71, f'FATAL: {long}\n  a line of the report\n')
        self.assertIn('(code 71): access violation (0xc0000005)', a.message)
        self.assertIn('…. Its log is', a.message)
        self.assertLess(len(a.message), app.FATAL_SHOWN + 120)
        self.assertIn(long, a.ctx.log.call_args[0][0])
        a, _ = self.ended(70, None)
        self.assertIn('The game ended with code 70 (a fatal error', a.message)
        a, log = self.ended(0xC0000005, f'FATAL: {fatal}\n')
        self.assertEqual(a.message, 'The game ended with code 0xC0000005 (access violation). Its log is '
                                    f'{GAME_LOG} (older builds: nx2navi.log).')
        log.unlink()
        self.assertEqual(app.last_fatal(self.tmp), '')
        self.assertEqual(app.last_fatal(''), '')

    def test_an_end_with_code_0_is_logged_and_noted_without_the_games_exit_line(self):
        try:
            from launcher.ui import app
        except ImportError as e:
            self.skipTest(f'no window toolkit: {e}')
        a, log = self.ended(0, '[host] log: x\n[host] gfx: window closed; exiting\n[host] perf: after it\n')
        a.ctx.log.assert_called_once_with('the game ended with code 0')
        self.assertNotIn('[launcher]', log.read_text(encoding='utf-8'))
        a, log = self.ended(0, '[host] log: x\n[host] perf: frame 39660: the last line of play\n')
        a.ctx.log.assert_called_once_with('the game ended with code 0, and its log has no line of its own end')
        text = log.read_text(encoding='utf-8')
        self.assertIn("[launcher] the game ended with code 0 (the log has no line of the game's own end", text)
        self.assertIs(app.log_has_own_exit(self.tmp), False)
        log.write_text('FATAL: x\n', encoding='utf-8')
        self.assertIs(app.log_has_own_exit(self.tmp), True)
        log.write_text('[host] guest ExitProcess at main+0x10; exiting\n', encoding='utf-8')
        self.assertIs(app.log_has_own_exit(self.tmp), True)
        log.unlink()
        self.assertIsNone(app.log_has_own_exit(self.tmp))
        a, _ = self.ended(0, None)
        a.ctx.log.assert_called_once_with('the game ended with code 0')

    def test_the_fatal_line_is_looked_for_in_the_logs_end(self):
        try:
            from launcher.ui import app
        except ImportError as e:
            self.skipTest(f'no window toolkit: {e}')
        log = self.tmp / 'logs' / 'nx2navi.log'
        log.parent.mkdir()
        log.write_bytes(b'FATAL: far back\n' + b'[host] a line\n' * 40000 + b'FATAL: near the end \xff\n[host] last\n')
        self.assertGreater(log.stat().st_size, app.FATAL_TAIL)
        self.assertEqual(app.last_fatal(self.tmp), 'near the end \ufffd')
        log.write_bytes(b'FATAL: far back\n' + b'[host] a line\n' * 40000)
        self.assertEqual(app.last_fatal(self.tmp), '')


class MemorySettings(dict):
    def save(self):
        pass


def mock_app(test, game):
    try:
        from launcher.ui.app import App
    except ImportError as e:
        test.skipTest(f'no window toolkit: {e}')
    a = mock.MagicMock()
    a.settings = MemorySettings({'game_dir': str(game), 'auto_check': False})
    a.args = mock.Mock(rolled_back='', uninstall=False, continue_update=None)
    a.ctx = Context(test.layout)
    test.addCleanup(a.ctx._file.close)
    a.layout = test.layout
    a.install_staged.return_value = False
    a.bg.ready.return_value = None
    a.bg.staging.return_value = False
    a.release = a.check = None
    a.check_error = a.message = ''
    a.checked_at = 0
    return App, a


def fake_linux_game(folder, seconds=2, code=0):
    folder.mkdir(parents=True, exist_ok=True)
    exe = folder / 'ocarina'
    exe.write_text(f'#!/bin/sh\necho "$PWD $(cut -d" " -f6 /proc/$$/stat)" > started.txt\nsleep {seconds}\nexit {code}\n',
                   encoding='utf-8')
    exe.chmod(0o755)
    return exe


RESTARTING_GAME_C = r'''#include <spawn.h>
#include <stdlib.h>
#include <unistd.h>
extern char **environ;
int main(void) {
    if (!getenv("FAKE_GAME_RESTARTED")) {
        char exe[4096];
        ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
        if (n <= 0) return 9;
        exe[n] = 0;
        setenv("FAKE_GAME_RESTARTED", "1", 1);
        pid_t pid;
        char *args[] = {exe, NULL};
        if (posix_spawn(&pid, exe, NULL, NULL, args, environ)) return 9;
        usleep(300000);
        return 0;
    }
    const char *ms = getenv("FAKE_COPY_MS"), *code = getenv("FAKE_COPY_CODE");
    usleep((useconds_t)(ms ? atoi(ms) : 3000) * 1000);
    return code ? atoi(code) : 3;
}
'''


@unittest.skipUnless(sys.platform.startswith('linux'), 'the Linux game and its processes')
class LinuxPlayTest(Tmp):
    def log(self, a):
        return ''.join(a.ctx.log_tail(200))

    def play(self, a, deadline=30):
        from launcher.ui.app import App
        App.play(a)
        end = time.time() + deadline
        while a.game_run is not None and time.time() < end:
            time.sleep(0.1)
            App.watch_game(a)
        self.assertIsNone(a.game_run, 'the game run did not end')

    def test_play_starts_the_linux_game_and_logs_its_end(self):
        game = self.tmp / 'game'
        exe = fake_linux_game(game)
        App, a = mock_app(self, game)
        a.game_run = None
        a.settings['hide_while_playing'] = True
        real, started = subprocess.Popen, []

        def popen_and_keep(*args, **kw):
            started.append(real(*args, **kw))
            return started[-1]
        with mock.patch('launcher.winsys.subprocess.Popen', side_effect=popen_and_keep) as popen:
            self.play(a)
        popen.assert_called_once()
        args, kw = popen.call_args
        self.assertEqual(args[0], [str(exe)])
        self.assertEqual(kw['cwd'], str(game))
        self.assertTrue(kw['start_new_session'])
        self.assertNotIn('creationflags', kw)
        self.assertIsNone(winsys.system_commit_info())
        where, sid = (game / 'started.txt').read_text().split()
        self.assertEqual(Path(where), game)
        self.assertEqual(int(sid), started[0].pid)
        a.root.withdraw.assert_called_once()
        a.root.deiconify.assert_called_once()
        text = self.log(a)
        self.assertIn(f'play: {exe}\n', text)
        self.assertIn('the game ended with code 0\n', text)
        self.assertNotIn('play: not started', text)
        self.assertNotIn('play memory:', text)
        self.assertEqual(a.message, '')

    def test_the_game_runs_in_its_own_session(self):
        game = self.tmp / 'game'
        exe = fake_linux_game(game, seconds=1)
        self.addCleanup(winsys.game_finished, exe)
        proc = winsys.start_game(exe)
        self.assertEqual(proc.wait(30), 0)
        where, sid = (game / 'started.txt').read_text().split()
        self.assertEqual(int(sid), proc.pid)
        self.assertNotEqual(int(sid), os.getsid(0))

    def test_every_play_that_does_not_start_says_why(self):
        game = self.tmp / 'game'
        App, a = mock_app(self, game)
        a.game_run = None
        App.play(a)
        self.assertIn(f'play: not started: the game is not built ({game / "ocarina"})\n', self.log(a))
        self.assertEqual(a.message, 'The game is not built yet.')
        a.settings['game_dir'] = ''
        App.play(a)
        self.assertIn('play: not started: the game is not built (no game folder is selected)\n', self.log(a))
        exe = fake_linux_game(game)
        a.settings['game_dir'] = str(game)
        with mock.patch('launcher.ui.app.saves.game_running', return_value=True):
            App.play(a)
        self.assertIn(f'play: not started: the game is already running ({exe})\n', self.log(a))
        with mock.patch('launcher.ui.app.commit.play_memory_request', return_value=commit.MemoryRequest(1, 2, False)), \
                mock.patch('launcher.ui.app.winsys.confirm_play', return_value=False):
            App.play(a)
        self.assertIn(f'play: not started: the memory warning was declined ({exe})\n', self.log(a))
        with mock.patch('launcher.ui.app.winsys.start_game', side_effect=PermissionError(13, 'Permission denied')):
            App.play(a)
        self.assertIn(f'play: not started: could not start the game: [Errno 13] Permission denied ({exe})\n',
                      self.log(a))
        self.assertIsNone(a.game_run)

    def restarting_play(self, **env):
        cc = shutil.which('cc') or shutil.which('clang') or shutil.which('gcc')
        if not cc:
            self.skipTest('needs a C compiler')
        game = self.tmp / 'game'
        game.mkdir()
        src = self.tmp / 'restarting.c'
        src.write_text(RESTARTING_GAME_C, encoding='utf-8')
        exe = game / 'ocarina'
        subprocess.run([cc, '-O1', '-o', str(exe), str(src)], check=True, capture_output=True, timeout=120)
        App, a = mock_app(self, game)
        a.game_run = None
        keep = {k: v for k, v in os.environ.items() if not k.startswith('FAKE_')}
        with mock.patch.dict(os.environ, {**keep, **env}, clear=True):
            self.play(a)
        self.assertIn(f'play: {exe}\n', self.log(a))
        import ctypes
        flag = ctypes.c_int(-1)
        ctypes.CDLL(None).prctl(37, ctypes.byref(flag), 0, 0, 0)
        self.assertEqual(flag.value, 0)
        a.root.deiconify.assert_called_once()
        return a

    def test_the_restarted_copy_is_adopted_and_its_code_read(self):
        a = self.restarting_play()
        self.assertIn('the game ended with code 3 after restarting itself\n', self.log(a))
        from launcher.ui.app import LOG_WHERE
        self.assertEqual(a.message, f'The game ended with code 3. {LOG_WHERE}')

    def test_a_copy_that_ended_before_it_was_looked_for_gives_its_code(self):
        a = self.restarting_play(FAKE_COPY_MS='0', FAKE_COPY_CODE='70')
        self.assertRegex(self.log(a), r'the game ended with code 70 \(a fatal error[^\n]*\) after restarting itself\n')

    def test_only_ended_copies_of_the_game_are_waited_for(self):
        from launcher.platform import linux
        root = self.tmp / 'proc'
        stats = {'10': '10 (ocarina) Z 500 ', '11': '11 (ocarina) Z 501 ', '12': '12 (ocarina) S 500 ',
                 '13': '13 (sh) Z 500 ', '14': '14 (ocarina) Z 500 ', 'self': ''}
        for pid, head in stats.items():
            (root / pid).mkdir(parents=True)
            start = {'10': '900', '14': '800'}.get(pid, '1')
            (root / pid / 'stat').write_text(head + ' '.join(['0'] * 17 + [start, '0']) if head else '')
        waited = []

        def wait(pid):
            waited.append(pid)
            return pid, (pid - 10) << 8
        self.assertEqual(linux.ended_copies('/games/x/ocarina', proc_root=root, me=500, wait=wait), [4, 0])
        self.assertEqual(sorted(waited), [10, 14])
        waited.clear()
        self.assertEqual(linux.ended_copies('/games/x/ocarina', skip=10, proc_root=root, me=500, wait=wait), [4])

    def test_a_copy_is_found_by_its_exe_file(self):
        from launcher.platform import linux
        game = self.tmp / 'game'
        exe = fake_linux_game(game)
        procs = {'1': '/usr/bin/other', '2': str(exe), '3': str(exe) + ' (deleted)', 'self': '', 'x': ''}
        root = self.tmp / 'proc'
        for pid in procs:
            (root / pid).mkdir(parents=True)

        def readlink(path):
            if not Path(path).parent.is_dir():
                raise FileNotFoundError(path)
            return procs[Path(path).parent.name]
        self.assertEqual(linux.game_copy(exe, skip=3, proc_root=root, readlink=readlink).pid, 2)
        self.assertEqual(linux.game_copy(exe, skip=2, proc_root=root, readlink=readlink).pid, 3)
        self.assertIsNone(linux.game_copy(game / 'other', proc_root=root, readlink=readlink))
        copy = linux.GameCopy(2, exe.resolve(), proc_root=root, readlink=readlink)
        with mock.patch('os.waitpid', side_effect=ChildProcessError):
            self.assertIsNone(copy.poll())
            self.assertFalse(copy.ended)
            procs['2'] = '/usr/bin/other'
            self.assertIsNone(copy.poll())
            self.assertTrue(copy.ended)
        copy = linux.GameCopy(3, exe.resolve(), proc_root=root, readlink=readlink)
        with mock.patch('os.waitpid', side_effect=ChildProcessError):
            (root / '3').rmdir()
            self.assertIsNone(copy.poll())
            self.assertTrue(copy.ended)

    def test_an_end_by_a_signal_names_it(self):
        try:
            from launcher.ui import app
        except ImportError as e:
            self.skipTest(f'no window toolkit: {e}')
        end = app.game_end(str(self.tmp), -11)
        self.assertEqual(end.log, 'the game ended with code -11 (ended by SIGSEGV)')
        self.assertEqual(end.message, f'The game ended with code -11 (ended by SIGSEGV). {app.LOG_WHERE}')
        self.assertTrue(end.alarm)
        end = app.game_end(str(self.tmp), -15, restarted=True)
        self.assertEqual(end.log, 'the game ended with code -15 (ended by SIGTERM) after restarting itself')
        self.assertFalse(end.alarm)

    @unittest.skipUnless(os.environ.get('DISPLAY'), 'needs a display (xvfb-run on Linux)')
    def test_the_window_plays_the_game_and_comes_back(self):
        import signal
        home, root = self.tmp / 'home', self.tmp / 'w'
        home.mkdir()
        game = self.tmp / 'game'
        exe = fake_linux_game(game)
        (game / 'build-cache').mkdir()
        (game / 'build-cache' / 'state.json').write_text(json.dumps({'kit': '0.21.0', 'steps': {'compile': {}}}))
        (root / 'data').mkdir(parents=True)
        (root / 'data' / 'launcher.json').write_text(json.dumps(
            {'game_dir': str(game), 'auto_check': False, 'auto_update': False, 'hide_while_playing': True}))
        env = {k: v for k, v in os.environ.items() if k in ('DISPLAY', 'XAUTHORITY', 'PATH', 'LANG')}
        env.update(HOME=str(home), XDG_DATA_HOME=str(home / 'data'), XDG_CONFIG_HOME=str(home / 'config'),
                   XDG_CACHE_HOME=str(home / 'cache'), XDG_STATE_HOME=str(home / 'state'),
                   NX2SARIA_TELEMETRY_URL='none')
        start = (
            'import sys\nimport launcher.platform.linux as linux\nlinux.mounted_drives = lambda *a, **k: []\n'
            'from launcher.ui import app\n'
            'start, watch = app.App.start, app.App.watch_game\n'
            'def started(self):\n'
            '    start(self)\n'
            '    def press():\n'
            '        self.ctx.log(f"test: screen {self.screen}")\n'
            '        self.focus = next(i for i, r in enumerate(self.rows) if r.label == "Play")\n'
            '        self.activate()\n'
            '        self.root.after(500, lambda: self.ctx.log(f"test: window {self.root.state()} while playing"))\n'
            '    self.root.after(1500, press)\n'
            'def watched(self):\n'
            '    watch(self)\n'
            '    if self.game_run is None:\n'
            '        self.root.update_idletasks()\n'
            '        self.ctx.log(f"test: window {self.root.state()} after the game")\n'
            'app.App.start, app.App.watch_game = started, watched\n'
            'sys.exit(app.main(sys.argv[1:]))\n')
        p = subprocess.Popen([sys.executable, '-c', start, '--root', str(root)], cwd=ROOT, env=env,
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        log = root / 'data' / 'logs' / 'launcher.log'
        deadline = time.time() + 60
        while time.time() < deadline and 'after the game' not in (log.read_text() if log.is_file() else ''):
            if p.poll() is not None:
                break
            time.sleep(0.2)
        p.send_signal(signal.SIGTERM)
        out, err = p.communicate(timeout=30)
        text = log.read_text()
        self.assertIn('test: screen home\n', text, err)
        self.assertIn(f'play: {exe}\n', text)
        self.assertIn('test: window withdrawn while playing\n', text)
        self.assertIn('the game ended with code 0\n', text)
        self.assertIn('test: window normal after the game\n', text)
        self.assertNotIn('play: not started', text)
        self.assertNotIn('error in the window', text)
        self.assertEqual(Path((game / 'started.txt').read_text().split()[0]), game)


class BeginInstallTest(Tmp):
    def test_a_cut_romfs_image_does_not_block_the_install(self):
        import collections
        game = self.tmp / 'game'
        part = game / 'romfs' / 'romfs.bin.part'
        part.parent.mkdir(parents=True)
        part.write_bytes(b'cut short')
        usage = collections.namedtuple('usage', 'total used free')

        def disk_usage(path):
            return usage(10 ** 12, 0, 20 * 10 ** 9 if part.exists() else 40 * 10 ** 9)
        App, a = mock_app(self, game)
        a.settings['dump'] = str(self.tmp / 'game.nup')
        a.settings['desktop_shortcut'] = False
        a.game_dir.return_value = str(game)
        with mock.patch('launcher.ui.app.shutil.disk_usage', disk_usage):
            App.begin_install(a)
        self.assertFalse(part.exists())
        job = a.run_job.call_args[0][0]
        self.assertIsInstance(job, jobs.InstallJob)
        self.assertEqual(job.game_dir, str(game))

    def test_the_first_run_screen_counts_a_cut_romfs_image_as_free(self):
        from launcher.ui import app
        game = self.tmp / 'game'
        (game / 'romfs').mkdir(parents=True)
        self.assertEqual(app.partial_image_gb(game), 0.0)
        (game / 'romfs' / 'romfs.bin.part').write_bytes(bytes(2500000))
        self.assertEqual(app.partial_image_gb(game), 0.0025)
        App, a = mock_app(self, game)
        a.settings['dump'] = str(self.tmp / 'game.nup')
        a.game_dir.return_value = str(game)
        with mock.patch.object(app, 'free_gb', lambda p: 5.0), mock.patch.object(app, 'partial_image_gb', lambda p: 26.0):
            rows = {r.label: r for r in App.rows_first_run(a) if r.label}
        self.assertIn('31 GB free', rows['Install Folder'].desc[0])
        self.assertIn('(31 GB free)', rows['Begin Installation'].desc[0])

    def test_a_game_folder_whose_path_is_too_long_is_refused_first(self):
        from launcher import paths
        most = paths.WINDOWS_PATH_MAX - 1 - paths.LONGEST_IN_GAME
        fits = 'C:\\' + 'g' * (most - 3)
        deep = fits + 'g'
        with mock.patch.object(paths.os, 'name', 'nt'), \
                mock.patch.object(paths.os.path, 'abspath', lambda p: p):
            self.assertEqual(paths.long_path_problem(fits), '')
            self.assertEqual(paths.long_path_problem(''), '')
            said = paths.long_path_problem(deep)
            self.assertIn(f'is {most + 1} characters long', said)
            self.assertIn(f'at most {most} characters', said)
        with mock.patch.object(paths.os, 'name', 'posix'):
            self.assertEqual(paths.long_path_problem('/' + 'g' * 4000), '')
        if os.name != 'nt':
            return
        App, a = mock_app(self, deep)
        a.settings['dump'] = str(self.tmp / 'game.nup')
        a.game_dir.return_value = deep
        App.begin_install(a)
        a.run_job.assert_not_called()
        self.assertTrue(a.message.startswith('The path of the game folder is'))
        ctx = JobTest.make_ctx(self, 'ok')
        self.addCleanup(ctx._file.close)
        j = jobs.InstallJob(ctx, self.tmp / 'g.nup', deep).run_inline()
        self.assertEqual(j.result, 'failed')
        self.assertIn('characters long', j.error)
        self.assertEqual(j.step('tools').state, jobs.PENDING)


class UpdateTextTest(Tmp):
    def test_the_updates_row_does_not_promise_a_few_minutes(self):
        from launcher.ui import app
        App, a = mock_app(self, self.tmp / 'game')
        a.release = mock.Mock(version='9.9.9', notes='')
        texts = [' '.join(App.update_desc(a)['text']), App.staged_desc(a, {'version': '9.9.9', 'notes': ''})['note']]
        for text in texts:
            self.assertNotIn('in a few minutes', text)
            self.assertIn(app.UPDATE_TAKES, text)
            self.assertIn('saves and settings stay', text)
        self.assertIn('half an hour or more', app.UPDATE_TAKES)


class ForcedCheckTest(Tmp):
    def test_one_forced_update_check_a_minute(self):
        from launcher.ui import app
        App, a = mock_app(self, self.tmp / 'game')
        a.screen = 'home'
        with mock.patch('launcher.ui.app.jobs.CheckJob') as check:
            a.check_started = time.time() - 10
            App.check_now(a, force=True)
            check.assert_not_called()
            self.assertEqual(a.message, 'NX2Saria looked for updates less than a minute ago.')
            a.show.assert_called_once_with('home', a.focus)
            a.check_started = time.time() - app.FORCED_CHECK_PAUSE - 1
            App.check_now(a, force=True)
            check.assert_called_once()
            self.assertGreater(a.check_started, time.time() - 5)
            a.check, a.check_started = None, time.time() - 10
            App.check_now(a)
            self.assertEqual(check.call_count, 2)


class StartScriptTest(Tmp):
    def release(self, ver, broken=False):
        app = self.tmp / 'root' / 'app' / ver / 'launcher'
        (app / 'ui').mkdir(parents=True)
        shutil.copy(ROOT / 'launcher' / 'start.pyw', app / 'start.pyw')
        (app / '__init__.py').write_text('')
        (app / 'ui' / '__init__.py').write_text('')
        (app / 'ui' / 'app.py').write_text("raise RuntimeError('this version is broken')\n" if broken else
                                           'def main():\n    return 0\n')
        return app / 'start.pyw'

    def load(self, script):
        import importlib.machinery
        import importlib.util
        loader = importlib.machinery.SourceFileLoader('nx2navi_start_script', str(script))
        mod = importlib.util.module_from_spec(importlib.util.spec_from_loader(loader.name, loader))
        loader.exec_module(mod)
        return mod

    def test_which_version_the_stub_goes_back_to(self):
        root = self.tmp / 'root'
        s = self.load(self.release('0.7.2', broken=True))
        self.assertEqual(s.rollback_target(str(root)), '')
        (root / 'app' / 'previous.txt').write_text('0.7.1\n')
        self.assertEqual(s.rollback_target(str(root)), '')
        self.release('0.7.1')
        self.assertEqual(s.rollback_target(str(root)), '0.7.1')
        (root / 'app' / 'previous.txt').write_bytes(b'\xef\xbb\xbf 0.7.1 \r\n')
        self.assertEqual(s.rollback_target(str(root)), '0.7.1')
        self.assertTrue(s.quiet_exit(str(root), [], 1.0))
        self.assertTrue(s.quiet_exit(str(root), ['--continue-update', '0.7.1'], 1.0))
        self.assertFalse(s.quiet_exit(str(root), [], 5.5))
        self.assertFalse(s.quiet_exit(str(root), [], float('inf')))
        self.assertFalse(s.quiet_exit(str(root), ['--play'], 1.0))
        (root / 'app' / 'previous.txt').write_text('0.7.2\n')
        self.assertEqual(s.rollback_target(str(root)), '')
        self.assertFalse(s.quiet_exit(str(root), [], 1.0))
        if os.name == 'nt':
            self.assertLess(s.process_age(), 3600)
        else:
            self.assertEqual(s.process_age(), float('inf'))

    def test_a_broken_version_ends_with_86_and_no_box(self):
        root = self.tmp / 'root'
        script = self.release('0.7.2', broken=True)
        self.release('0.7.1')
        (root / 'app' / 'previous.txt').write_text('0.7.1\n')
        env = dict(os.environ, NX2SARIA_LAUNCHER_ROOT=str(root))
        try:
            r = subprocess.run([sys.executable, str(script)], env=env, capture_output=True, text=True, timeout=30)
        except subprocess.TimeoutExpired:
            self.fail('start.pyw did not end: it waits in a message box')
        self.assertEqual(r.returncode, 86, r.stderr)
        log = (root / 'data' / 'logs' / 'crash.log').read_text(encoding='utf-8')
        self.assertIn('this version is broken', log)

    def test_a_version_that_starts_returns_its_code(self):
        root = self.tmp / 'root'
        script = self.release('0.7.2')
        r = subprocess.run([sys.executable, str(script)], env=dict(os.environ, NX2SARIA_LAUNCHER_ROOT=str(root)),
                           capture_output=True, text=True, timeout=30)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertFalse((root / 'data' / 'logs' / 'crash.log').exists())


class UnfinishedUpdateTest(Tmp):
    def app(self, game):
        return mock_app(self, game)

    def test_the_window_opens_on_home(self):
        game = made_up_game(self.tmp / 'game', steps=('exefs', 'romfs', 'lift'))
        App, a = self.app(game)
        App.start(a)
        a.show.assert_called_once_with('home')
        a.find_dumps.assert_not_called()
        (game / GAME_EXE).unlink()
        App, a = self.app(game)
        App.start(a)
        a.show.assert_called_once_with('first_run')

    def test_an_update_goes_on_with_the_game(self):
        game = made_up_game(self.tmp / 'game', steps=('exefs', 'romfs', 'lift'))
        App, a = self.app(game)
        a.args.continue_update = '0.6.1'
        App.start(a)
        job = a.run_job.call_args[0][0]
        self.assertIsInstance(job, jobs.ContinueUpdateJob)
        self.assertEqual(job.game_dir, str(game))

    def test_home_says_how_to_finish(self):
        game = made_up_game(self.tmp / 'game', steps=('exefs', 'romfs', 'lift'))
        App, a = self.app(game)
        rows = {r.label: r for r in App.rows_home(a) if r.label}
        self.assertEqual(rows['Rebuild the Game'].value, 'Not Finished')
        self.assertTrue(any('Rebuild the Game finishes it' in d and 'dump is not needed' in d for d in rows['Play'].desc))
        made_up_game(game)
        rows = {r.label: r for r in App.rows_home(a) if r.label}
        self.assertEqual(rows['Rebuild the Game'].value, 'Built with 0.6.1')


class LauncherLogTest(Tmp):
    def ctx(self):
        ctx = Context(self.layout)
        self.addCleanup(ctx._file.close)
        return ctx

    def log(self, ctx):
        return ''.join(ctx.log_tail(200))

    def test_the_start_line_says_how_it_was_installed_and_on_what(self):
        from launcher import context
        ctx = self.ctx()
        self.assertIn(f'--- NX2Saria launcher {ctx.version} (', self.log(ctx))
        self.assertEqual(context.install_kind(Layout(root=self.tmp / 'r', app=ROOT)), 'checkout')
        if os.name == 'nt':
            return
        app = self.tmp / 'root' / 'app' / '0.21.0'
        app.mkdir(parents=True)
        (app.parent / 'current.txt').write_text('0.21.0\n')
        layout = Layout(app=app)
        self.assertEqual(layout.root, self.tmp / 'root')
        self.assertEqual(context.install_kind(layout, {}), 'release folder')
        (layout.root / 'nx2saria-launcher').write_text('#!/bin/sh\n')
        self.assertEqual(context.install_kind(layout, {'APPIMAGE': '/home/ana/NX2Saria.AppImage'}),
                         'AppImage install, started by the AppImage')
        self.assertEqual(context.install_kind(layout, {}), 'AppImage install, started from its menu entry or script')
        if sys.platform.startswith('linux'):
            release = self.tmp / 'os-release'
            release.write_text('NAME="Linux Mint"\nVERSION_ID="22"\nPRETTY_NAME="Linux Mint 22"\n')
            env = {'XDG_SESSION_TYPE': 'x11', 'XDG_CURRENT_DESKTOP': 'X-Cinnamon', 'DISPLAY': ':0'}
            self.assertEqual(context.system_text(env, release), 'Linux Mint 22, x11 session, desktop X-Cinnamon')
            self.assertIn('platform linux; ', self.log(ctx))

    def test_an_error_in_a_window_callback_is_logged(self):
        from launcher import errors
        ctx = self.ctx()
        try:
            {}['no such row']
        except KeyError:
            errors.window_hook(ctx)(*sys.exc_info())
        line = [ln for ln in ctx.log_tail() if 'error in the window' in ln]
        self.assertEqual(len(line), 1)
        self.assertIn("KeyError: 'no such row' (in test_an_error_in_a_window_callback_is_logged, launcher_test.py:",
                      line[0])
        crash = (self.layout.logs / 'crash.log').read_text(encoding='utf-8')
        self.assertIn('in the window:', crash)
        self.assertIn('Traceback (most recent call last)', crash)

    def test_errors_in_threads_and_fatal_faults_are_kept(self):
        import faulthandler
        from launcher import errors
        hook = threading.excepthook
        self.addCleanup(setattr, threading, 'excepthook', hook)
        ctx = self.ctx()
        self.addCleanup(errors.finish, ctx)
        self.assertEqual(errors.install(ctx), os.name != 'nt')
        self.assertTrue(faulthandler.is_enabled())

        def fail():
            raise RuntimeError('the dump search failed')
        t = threading.Thread(target=fail, name='dumps')
        t.start()
        t.join()
        self.assertIn('error in the background (dumps): RuntimeError: the dump search failed', self.log(ctx))
        if os.name == 'nt':
            return
        self.assertEqual(Path(errors._fault_file.name), self.layout.logs / 'crash.log')
        faulthandler.dump_traceback(errors._fault_file)
        errors._fault_file.flush()
        self.assertIn('test_errors_in_threads_and_fatal_faults_are_kept',
                      (self.layout.logs / 'crash.log').read_text(encoding='utf-8'))

    def test_an_empty_crash_log_is_not_left_behind(self):
        from launcher import errors
        ctx = self.ctx()
        crash = self.layout.logs / 'crash.log'
        errors.install(ctx)
        errors.finish(ctx)
        self.assertFalse(crash.exists())
        crash.write_text('Fatal Python error: Segmentation fault\n')
        errors.install(ctx)
        errors.finish(ctx)
        self.assertTrue(crash.exists())

    @unittest.skipUnless(hasattr(__import__('signal'), 'SIGHUP'), 'SIGHUP is POSIX only')
    def test_the_end_signals_reach_a_handler(self):
        import signal
        from launcher import errors
        for name in errors.END_SIGNALS:
            self.addCleanup(signal.signal, getattr(signal, name), signal.getsignal(getattr(signal, name)))
        got = []
        self.assertEqual(errors.on_end_signals(got.append), ['SIGTERM', 'SIGHUP'])
        os.kill(os.getpid(), signal.SIGHUP)
        os.kill(os.getpid(), signal.SIGTERM)
        self.assertEqual(got, ['SIGHUP', 'SIGTERM'])

    def test_closing_the_window_is_logged_once(self):
        App, a = mock_app(self, self.tmp / 'game')
        a.screen, a.opened, a.closed, a.job = 'first_run', time.monotonic() - 69, '', None
        App.on_close(a)
        App.on_close(a)
        lines = [ln for ln in a.ctx.log_tail() if ' window ' in ln]
        self.assertEqual(len(lines), 1)
        self.assertRegex(lines[0], r'window closed by the player on the first_run screen after 69 s$')
        a.root.destroy.assert_called_once()
        a.job = mock.Mock(result=None, task='installing the game', title='')
        a.closed = ''
        App.on_close(a, 'ended by SIGTERM')
        self.assertIn('window ended by SIGTERM on the first_run screen', self.log(a.ctx))
        self.assertIn('; installing the game cancelled', self.log(a.ctx))
        a.job.cancel.assert_called_once()

    def test_the_first_run_screen_logs_its_folder_and_dump(self):
        from launcher.ui import app
        App, a = mock_app(self, '')
        a.settings['game_dir'] = ''
        a.game_dir.return_value = str(self.tmp / 'nx2saria' / 'game')
        with mock.patch.object(app, 'free_gb', lambda p: 54.4):
            App.log_first_run(a)
        self.assertIn(f'first run: install folder {self.tmp / "nx2saria" / "game"} (default), 54 GB free there; dump '
                      'not chosen yet', self.log(a.ctx))

    @unittest.skipUnless(os.environ.get('DISPLAY') or os.name == 'nt', 'needs a display (xvfb-run on Linux)')
    def test_a_first_run_window_ended_by_a_signal(self):
        import signal
        if not hasattr(signal, 'SIGTERM') or os.name == 'nt':
            self.skipTest('signals that end a process are POSIX only')
        home, root = self.tmp / 'home', self.tmp / 'w'
        (home / 'Downloads').mkdir(parents=True)
        env = {k: v for k, v in os.environ.items() if k in ('DISPLAY', 'XAUTHORITY', 'PATH', 'LANG')}
        env.update(HOME=str(home))
        start = ('import sys\nimport launcher.platform.linux as linux\nlinux.mounted_drives = lambda *a, **k: []\n'
                 'from launcher.ui.app import main\nsys.exit(main(sys.argv[1:]))\n')
        p = subprocess.Popen([sys.executable, '-c', start, '--root', str(root)], cwd=ROOT, env=env,
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.addCleanup(lambda: p.poll() is None and (p.kill(), p.communicate()))
        log = root / 'data' / 'logs' / 'launcher.log'
        deadline = time.monotonic() + 180
        text = ''
        while p.poll() is None and time.monotonic() < deadline and not (
                'window: SIGTERM and SIGHUP close it, woken through a socket' in text and 'dump search:' in text):
            time.sleep(0.05)
            text = log.read_text() if log.is_file() else ''
        self.assertIn('dump search:', text, f'the window did not get to its dump search (exit {p.poll()})')
        self.assertIn('window: SIGTERM and SIGHUP close it, woken through a socket', text)
        sent = time.monotonic()
        p.send_signal(signal.SIGTERM)
        out, err = p.communicate(timeout=30)
        took = time.monotonic() - sent
        self.assertEqual(p.returncode, 128 + signal.SIGTERM, err)
        self.assertLess(took, 10, 'the window took that long to act on SIGTERM')
        text = log.read_text()
        self.assertIn('first run: install folder ', text)
        self.assertIn('; dump not chosen yet', text)
        self.assertIn('dump search: none found (', text)
        self.assertNotIn('error in the window', text)
        self.assertRegex(text, r'window ended by SIGTERM on the first_run screen after \d+ s, \d+\.\d\d s after the '
                               r'signal\n$')

    def idle_window_ended(self, use_socket):
        import signal
        if not hasattr(signal, 'SIGTERM') or os.name == 'nt':
            self.skipTest('signals that end a process are POSIX only')
        start = ('import sys, tkinter as tk\nfrom launcher import errors\nroot = tk.Tk()\nroot.withdraw()\n'
                 'errors.on_end_signals(lambda name: root.after(0, root.destroy))\n'
                 f'how, undo = errors.wake_for_signals(root, use_socket={use_socket})\n'
                 'root.after(0, lambda: print(how, flush=True))\nroot.mainloop()\nundo()\n'
                 'import signal\nprint("wakeup fd put back" if signal.set_wakeup_fd(-1) == -1 else "", flush=True)\n')
        p = subprocess.Popen([sys.executable, '-c', start], cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                             text=True)
        self.addCleanup(lambda: p.poll() is None and (p.kill(), p.communicate()))
        how = p.stdout.readline().strip()
        self.assertEqual(how, 'woken through a socket' if use_socket else 'checked every 0.25 s', p.poll())
        stat = Path(f'/proc/{p.pid}/task/{p.pid}/stat')
        deadline = time.monotonic() + 60
        while stat.exists() and time.monotonic() < deadline and stat.read_text().rsplit(')', 1)[1].split()[0] != 'S':
            time.sleep(0.01)
        sent = time.monotonic()
        p.send_signal(signal.SIGTERM)
        out, err = p.communicate(timeout=30)
        took = time.monotonic() - sent
        self.assertEqual(p.returncode, 0, err)
        self.assertEqual(out.strip(), 'wakeup fd put back', err)
        return took

    @unittest.skipUnless(os.environ.get('DISPLAY') or os.name == 'nt', 'needs a display (xvfb-run on Linux)')
    def test_an_idle_window_acts_on_a_signal_at_once(self):
        self.assertLess(self.idle_window_ended(True), 5)

    @unittest.skipUnless(os.environ.get('DISPLAY') or os.name == 'nt', 'needs a display (xvfb-run on Linux)')
    def test_an_idle_window_without_the_socket_checks_for_a_signal(self):
        self.assertLess(self.idle_window_ended(False), 5)

    def test_windows_takes_the_timer(self):
        import signal
        try:
            import tkinter  # noqa: F401  (wake_for_signals imports it; not while os.name is patched)
        except ImportError as e:
            self.skipTest(f'no window toolkit: {e}')
        from launcher import errors
        for tk_has_it, name in ((True, 'nt'), (False, 'posix')):
            root = mock.Mock()
            if not tk_has_it:
                root.tk = mock.Mock(spec=[])
            with mock.patch.object(errors.os, 'name', name), mock.patch.object(signal, 'set_wakeup_fd') as wake:
                how, undo = errors.wake_for_signals(root)
            self.assertEqual(how, 'checked every 0.25 s')
            wake.assert_not_called()
            if tk_has_it:
                root.tk.createfilehandler.assert_not_called()
            self.assertEqual(root.after.call_args.args[0], 250)
            tick = root.after.call_args.args[1]
            tick()
            self.assertEqual(root.after.call_count, 2)
            undo()
            tick()
            self.assertEqual(root.after.call_count, 2)


@unittest.skipUnless(os.environ.get('DISPLAY') or os.name == 'nt', 'needs a display (xvfb-run on Linux)')
class RefocusTest(Tmp):
    def window(self):
        import tkinter as tk
        from launcher.ui import app
        ctx = Context(self.layout)
        self.addCleanup(ctx._file.close)
        root = tk.Tk()
        self.addCleanup(root.destroy)
        with mock.patch('launcher.platform.linux.mounted_drives', lambda *a, **k: []):
            a = app.App(root, ctx, app.parse_args(['--root', str(self.layout.root)]))
        root.update()
        return a

    def items(self, a):
        c = a.canvas
        out = []
        for i in c.find_withtag('dyn'):
            opts = {k: c.itemcget(i, k) for k in ('image', 'text', 'fill') if k in c.itemconfigure(i)}
            out.append((c.type(i), tuple(round(v) for v in c.coords(i)), tuple(sorted(opts.items())),
                        tuple(sorted(t for t in c.gettags(i) if not t.startswith('row') and t != 'current'))))
        return sorted(out)

    def test_refocus_draws_what_render_draws(self):
        a = self.window()
        a.settings['animations'] = False
        for screen in ('customize', 'first_run', 'settings', 'uninstall'):
            a.show(screen)
            rows = a.focusables()
            self.assertGreater(len(rows), 1, screen)
            for old, new in zip(rows, rows[1:] + rows[:1]):
                a.focus = old
                a.render()
                a.focus = new
                a.refocus(old)
                partial = self.items(a)
                a.render()
                self.assertEqual(partial, self.items(a), (screen, old, new))
            a.focus = rows[0]
            a.render()
            a.focus = rows[1]
            old, a.focus = a.focus, rows[2 % len(rows)]
            a.refocus(old)
            partial = self.items(a)
            a.render()
            self.assertEqual(partial, self.items(a), (screen, 'focus moved without drawing'))
        for k in range(20):
            old, a.focus = a.focus, rows[k % len(rows)]
            a.refocus(old)
        n = len(a.canvas.find_all())
        a.render()
        self.assertEqual(n, len(a.canvas.find_all()))


class WindowTest(Tmp):
    def test_selftest(self):
        r = subprocess.run([sys.executable, '-m', 'launcher', '--root', str(self.tmp / 'w'), '--selftest'],
                           cwd=ROOT, capture_output=True, text=True, timeout=180)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn('27 screens drawn', r.stdout)


class TelemetryHiddenTest(Tmp):
    def make_app(self, url):
        try:
            import tkinter as tk
            from launcher.ui import app
        except ImportError as e:
            self.skipTest(f'no window toolkit: {e}')
        from launcher import telemetry
        env = {k: v for k, v in os.environ.items() if k != telemetry.URL_ENV}
        if url:
            env[telemetry.URL_ENV] = url
        patch = mock.patch.dict(os.environ, env, clear=True)
        patch.start()
        self.addCleanup(patch.stop)
        tele = mock.patch.object(telemetry, 'TELEMETRY_URL', '')
        tele.start()
        self.addCleanup(tele.stop)
        game = self.tmp / 'game'
        (game / 'logs').mkdir(parents=True)
        (game / 'logs' / telemetry.SUMMARY).write_text(json.dumps(telemetry.example_summary()), encoding='utf-8')
        ctx = Context(self.layout)
        self.addCleanup(lambda: ctx._file and ctx._file.close())
        ctx.settings['game_dir'], ctx.settings['sounds'] = str(game), 'off'
        try:
            root = tk.Tk(className='NX2Saria')
        except tk.TclError as e:
            self.skipTest(f'no display: {e}')

        made = []

        def close():
            for after in root.tk.call('after', 'info'):
                root.after_cancel(after)
            root.destroy()
            if made:
                made[0].__dict__.clear()
            gc.collect()
        self.addCleanup(close)
        quiet = mock.patch.object(app.App, 'find_dumps', lambda a: None)
        quiet.start()
        self.addCleanup(quiet.stop)
        a = app.App(root, ctx, app.parse_args(['--root', str(self.layout.root), '--selftest']))
        made.append(a)
        root.update()
        return a, ctx

    def test_no_server_no_reports_in_the_window(self):
        a, ctx = self.make_app('')
        self.assertNotIn('Session Reports', [r.label for r in a.rows_settings()])
        a.start_reports()
        self.assertNotEqual(a.screen, 'consent')
        self.assertIsNone(a.question)
        a.open_screen('consent')
        self.assertNotEqual(a.screen, 'consent')
        self.assertEqual(ctx.settings['telemetry'], '')
        self.assertNotIn('telemetry_id', ctx.settings)

    def test_the_override_turns_them_on(self):
        a, ctx = self.make_app('http://127.0.0.1:9')
        self.assertIn('Session Reports', [r.label for r in a.rows_settings()])
        a.start_reports()
        self.assertEqual(a.screen, 'consent')
        a.question_end("don't send")


class ModsTest(Tmp):
    def setUp(self):
        super().setUp()
        from launcher import mods
        self.mods = mods
        self.game = self.tmp / 'game'
        made_up_game(self.game)
        self.folder = self.game / 'mods'
        self.folder.mkdir()

    def file(self, rel, text='x'):
        p = self.folder / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text)
        return p

    def tree(self):
        return sorted(p.relative_to(self.folder).as_posix() for p in self.folder.rglob('*') if p.is_file())

    def test_downloads_are_put_where_the_game_looks(self):
        with zipfile.ZipFile(self.folder / 'Zipped.zip', 'w') as z:
            z.writestr('Zipped/romfs/Actor/a.bfres', 'a')
            z.writestr('Zipped/README.md', 'r')
        with zipfile.ZipFile(self.folder / 'Escape.zip', 'w') as z:
            z.writestr('../outside.txt', 'e')
        self.file('Wrapped/readme.txt')
        self.file('Wrapped/Wrapped v2/readme.txt')
        self.file('Wrapped/Wrapped v2/Wrapped/romfs/UI/b.txt')
        self.file('romfs/Actor/c.bfres')
        self.file('atmosphere/contents/0400486024DEC000/romfs/d.txt')
        self.file('Options/Option A/romfs/e')
        self.file('Options/Option B/romfs/e')
        done = self.mods.prepare(self.game)
        self.assertEqual(self.tree(), [
            'Escape.zip', 'Options/Option A/romfs/e', 'Options/Option B/romfs/e', 'Unnamed mod/romfs/Actor/c.bfres',
            'Wrapped/readme (2).txt', 'Wrapped/readme.txt', 'Wrapped/romfs/UI/b.txt', 'Zipped.zip',
            'Zipped/README.md', 'Zipped/romfs/Actor/a.bfres',
            'atmosphere/contents/0400486024DEC000/romfs/d.txt'])
        self.assertFalse((self.game / 'outside.txt').exists())
        self.assertTrue(any(line.startswith('Escape.zip was not unpacked') for line in done))
        self.assertEqual(self.mods.prepare(self.game), [line for line in done if 'Escape.zip' in line])
        found = {m.name: m for m in self.mods.scan(self.game)}
        self.assertEqual(sorted(n for n, m in found.items() if m.usable),
                         ['Unnamed mod', 'Wrapped', 'Zipped', 'atmosphere'])
        self.assertIn('options', found['Options'].problem)
        self.assertEqual(found['Zipped'].files, ['actor/a.bfres'])

    def test_what_the_game_cannot_use_is_named(self):
        self.file('Plugin/exefs/subsdk9')
        self.file('Other/atmosphere/contents/01007EF00011E000/romfs/x')
        self.file('Readme only/readme.txt')
        self.file('Old patch/exefs/0123456789ABCDEF0123456789ABCDEF.ips', 'PATCHEOF')
        self.file('Patch/exefs/E0F3CCFAF50065AE4EA01EB5AEB13B1A.ips', 'PATCHEOF')
        found = {m.name: m for m in self.mods.scan(self.game)}
        self.assertIn('code module', found['Plugin'].problem)
        self.assertIn('01007EF00011E000', found['Other'].problem)
        self.assertIn('nothing the game reads', found['Readme only'].problem)
        self.assertIn('another version', found['Old patch'].problem)
        self.assertEqual((found['Patch'].problem, found['Patch'].patches), ('', 1))

    def test_switches_and_order_are_the_games_settings(self):
        for name in ('beta', 'Alpha', ' Lead'):
            self.file(f'{name}/romfs/same.txt', name)
        self.assertEqual([m.name for m in self.mods.scan(self.game)], [' Lead', 'Alpha', 'beta'])
        self.mods.set_enabled(self.game, 'Alpha', False)
        self.mods.move(self.game, 'beta', -2)
        self.mods.set_enabled(self.game, ' Lead', False)
        self.mods.set_enabled(self.game, 'Alpha', True)
        self.assertEqual(gameini.get(self.game, 'MODS_ORDER'), 'beta| Lead|Alpha')
        self.assertEqual(gameini.get(self.game, 'MODS_DISABLED'), ' Lead')
        self.assertIn('mods_disabled = " Lead"', (self.game / 'nx2saria.ini').read_text())
        found = self.mods.scan(self.game)
        self.assertEqual([(m.name, m.enabled) for m in found], [('beta', True), (' Lead', False), ('Alpha', True)])
        self.assertEqual(self.mods.overlaps(found), {'beta': [('Alpha', 1, True)], 'Alpha': [('beta', 1, False)]})

    def test_names_windows_cannot_keep_are_refused_in_an_archive(self):
        problem = self.mods.windows_name_problem
        for name in ('CON', 'con.pchtxt', 'NUL.tar.gz', 'Com1', 'LPT9.txt', 'aux .txt', 'trailing.', 'trailing ',
                     'file.txt:stream', 'what?.bfres', 'tab\there'):
            self.assertTrue(problem(name), name)
        for name in ('console.txt', 'COM10.txt', 'a.b.c', '.hidden', 'nul_tex.bntx.zs', 'Actor', 'my mod (2)',
                     'Ünïcödé'):
            self.assertEqual(problem(name), '', name)
        safe = self.mods._safe_members
        safe(['romfs/Actor/a.bfres', 'romfs/Actor/', 'readme.txt'], windows=True)
        for bad in ('exefs/CON.pchtxt', 'romfs/nul/a.txt', 'romfs/Actor/foo.', 'romfs/Actor /a.txt',
                    'romfs/file.txt:stream', 'romfs\\AUX.bin'):
            with self.assertRaises(self.mods.ModError) as refused:
                safe(['romfs/ok.txt', bad], windows=True)
            self.assertIn(f'({bad})', str(refused.exception))
            safe(['romfs/ok.txt', bad], windows=False)
        with self.assertRaises(self.mods.ModError):
            safe(['../outside.txt'], windows=False)
        with zipfile.ZipFile(self.folder / 'Device.zip', 'w') as z:
            z.writestr('exefs/CON.pchtxt', '@stop\n')
            z.writestr('romfs/a.txt', 'a')
        done = self.mods.prepare(self.game)
        if os.name == 'nt':
            self.assertEqual(len(done), 1)
            self.assertIn('Device.zip was not unpacked: it holds a file Windows cannot keep under its name '
                          '(exefs/CON.pchtxt): Windows keeps that name for a device', done[0])
            self.assertEqual(self.tree(), ['Device.zip'])
            with self.assertRaises(self.mods.ModError):
                self.mods.add(self.game, self.folder / 'Device.zip')
            self.assertEqual(self.tree(), ['Device.zip'])
        else:
            self.assertEqual(self.tree(), ['Device.zip', 'Device/exefs/CON.pchtxt', 'Device/romfs/a.txt'])

    def test_a_picked_archive_or_folder_is_added(self):
        src = self.tmp / 'Downloads'
        src.mkdir()
        with zipfile.ZipFile(src / 'Picked.zip', 'w') as z:
            z.writestr('romfs/a.txt', 'a')
        (src / 'Folder Mod' / 'Inner' / 'exefs').mkdir(parents=True)
        (src / 'Folder Mod' / 'Inner' / 'exefs' / 'E0F3CCFAF50065AE.ips').write_text('PATCHEOF')
        (src / 'romfs').mkdir()
        (src / 'romfs' / 'b.txt').write_text('b')
        self.assertEqual(self.mods.add(self.game, src / 'Picked.zip').name, 'Picked')
        self.assertEqual(self.mods.add(self.game, src / 'Picked.zip').name, 'Picked 2')
        self.assertEqual(self.mods.add(self.game, src / 'Folder Mod').name, 'Folder Mod')
        self.assertEqual(self.mods.add(self.game, src / 'romfs').name, 'Unnamed mod')
        self.assertEqual(self.tree(), ['Folder Mod/exefs/E0F3CCFAF50065AE.ips', 'Picked 2/romfs/a.txt',
                                       'Picked/romfs/a.txt', 'Unnamed mod/romfs/b.txt'])
        self.assertTrue((src / 'Picked.zip').is_file())
        with self.assertRaises(self.mods.ModError):
            self.mods.add(self.game, src / 'Folder Mod' / 'Inner' / 'exefs' / 'E0F3CCFAF50065AE.ips')
        self.assertEqual(list(self.game.glob(self.mods.STAGING + '*')), [])

    def test_a_mod_that_does_not_arrive_whole_leaves_nothing(self):
        src = self.tmp / 'Downloads' / 'Big Mod'
        (src / 'romfs').mkdir(parents=True)
        for n in range(4):
            (src / 'romfs' / f'{n}.txt').write_text('x')
        self.file('Kept/romfs/k.txt')
        real_copytree = shutil.copytree
        for stop in (OSError(28, 'No space left on device'), KeyboardInterrupt()):
            copied = []

            def copy(a, b, stop=stop, copied=copied):
                if len(copied) == 2:
                    raise stop
                copied.append(b)
                return shutil.copy2(a, b)

            def copytree(a, b, *args, copy=copy, **kw):
                if args or kw:
                    return real_copytree(a, b, *args, **kw)
                return real_copytree(a, b, copy_function=copy)
            with mock.patch.object(shutil, 'copytree', copytree), self.assertRaises(type(stop)):
                self.mods.add(self.game, src)
            self.assertEqual(len(copied), 2)
            self.assertEqual(self.tree(), ['Kept/romfs/k.txt'])
            self.assertEqual(list(self.game.glob(self.mods.STAGING + '*')), [])
        (src.parent / 'Broken.zip').write_bytes(b'PK\x03\x04 not a zip')
        with self.assertRaises(self.mods.ModError):
            self.mods.add(self.game, src.parent / 'Broken.zip')
        self.assertEqual(self.tree(), ['Kept/romfs/k.txt'])
        self.assertEqual([m.name for m in self.mods.scan(self.game)], ['Kept'])
        self.assertEqual(self.mods.add(self.game, src).name, 'Big Mod')
        self.assertEqual(len(self.tree()), 5)

    def test_a_mod_is_put_together_beside_the_mods_folder(self):
        src = self.tmp / 'Downloads' / 'Late'
        (src / 'romfs').mkdir(parents=True)
        (src / 'romfs' / 'a.txt').write_text('a')
        left = self.game / self.mods.STAGING
        (left / 'mod' / 'romfs').mkdir(parents=True)
        old = time.time() - 2 * self.mods.STAGING_STALE
        os.utime(left, (old, old))
        fresh = self.game / (self.mods.STAGING + ' 2')
        fresh.mkdir()
        seen = []
        real_copytree = shutil.copytree

        def copytree(a, b, *args, **kw):
            if not seen:
                seen.append((Path(b), self.tree()))
                (self.folder / 'Late' / 'romfs').mkdir(parents=True)
            return real_copytree(a, b, *args, **kw)
        with mock.patch.object(shutil, 'copytree', copytree):
            target = self.mods.add(self.game, src)
        self.assertEqual(seen[0][1], [])
        self.assertEqual(seen[0][0].parent.parent, self.game)
        self.assertEqual(target.name, 'Late 2')
        self.assertEqual(self.tree(), ['Late 2/romfs/a.txt'])
        self.assertFalse(left.exists())
        self.assertEqual(list(self.game.glob(self.mods.STAGING + '*')), [fresh])

    def test_code_waits_for_a_build_that_saw_it(self):
        import make_game
        cheat = self.file('Cheats/cheats/E0F3CCFAF50065AE.txt', '[Moon jump]\n04000000 00000000 D503201F\n')
        self.file('Textures/romfs/a.txt')
        self.assertEqual(self.mods.code_waiting(self.game), ['Cheats'])
        self.mods.set_enabled(self.game, 'Cheats', False)
        self.assertEqual(self.mods.code_waiting(self.game), [])
        self.mods.set_enabled(self.game, 'Cheats', True)
        (self.game / 'build-cache' / 'mod-code.json').write_text(json.dumps(make_game.mod_code(self.game)))
        self.assertEqual(self.mods.code_waiting(self.game), [])
        cheat.write_text('[Moon jump]\n04000000 00000000 D65F03C0\n')
        self.assertEqual(self.mods.code_waiting(self.game), ['Cheats'])

    def test_code_cheats_take_mains_base_from_the_layout(self):
        import importlib
        import cheats
        import layout
        self.assertEqual(layout.ORDER[0], 'main')
        self.assertEqual(cheats.BASE, layout.FIRST_BASE)
        self.addCleanup(importlib.reload, cheats)
        with mock.patch.object(layout, 'FIRST_BASE', 0x7100000000):
            self.assertEqual(importlib.reload(cheats).BASE, 0x7100000000)

    def test_the_mods_code_is_in_the_lift_key_and_not_the_shaders_key(self):
        import make_game
        kit = self.tmp / 'kit'
        for d in ('tools', 'recomp', 'runtime', 'bin'):
            (kit / d).mkdir(parents=True)
        for f in ('tools/prepare_shaders.py', 'tools/cheats.py', 'tools/exefs_patches.py', 'tools/mod_files.py',
                  'recomp/patches-v111.txt', 'bin/' + make_game.XLATE_NAME, 'bin/nx2navi_xlate.id', 'VERSION'):
            (kit / f).write_text('1\n')

        def keys():
            return {s: make_game.step_key(s, 'game', self.game) for s in ('lift', 'compile', 'shaders')}
        with mock.patch.object(make_game, 'KIT', kit):
            plain = keys()
            self.file('Textures/romfs/a.txt')
            self.assertEqual(keys(), plain)
            self.assertNotIn('|mods ', plain['lift'])
            self.file('Patch/exefs/E0F3CCFAF50065AE.ips', 'PATCHEOF')
            with_patch = keys()
            self.assertTrue(with_patch['lift'].startswith(plain['lift'] + '|mods '))
            self.assertNotEqual(with_patch['compile'], plain['compile'])
            self.assertEqual(with_patch['shaders'], plain['shaders'])
            self.assertEqual(list(make_game.mod_code(self.game)), ['Patch/exefs/E0F3CCFAF50065AE.ips'])


if __name__ == '__main__':
    unittest.main(verbosity=2)
