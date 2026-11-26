import argparse
import collections
import concurrent.futures
import errno
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import threading
import time
import traceback
from pathlib import Path
from typing import NamedTuple

KIT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(KIT / 'tools'))
import nup  # noqa: E402
import memgate  # noqa: E402
import mod_files  # noqa: E402
import private_paths  # noqa: E402
import prompt_art  # noqa: E402
from check_content import GENERATED_MARKER  # noqa: E402

DESCRIPTION = ("Build Ocarina of Time (Switch 2, game 1.1.1) natively for Windows or Linux from the player's "
               'own game files.')

TITLE_ID = 0x0400486024DEC000
MAIN_BUILD_ID = 'e0f3ccfaf50065ae4ea01eb5aeb13b1a'
EXEFS_FILES = ('main', 'main.npdm', 'sdk', 'subsdk0', 'rtld')
MODULES = ('main', 'sdk', 'subsdk0', 'rtld')
ALIGN = 16
STATE_VERSION = 2
STEPS = ('exefs', 'romfs', 'lift', 'compile', 'shaders', 'prompts', 'scan')
OWN_KEY_STEPS = ('prompts', 'scan')
UNAFFECTED = {('romfs', 'lift'), ('lift', 'shaders'), ('compile', 'shaders')}
PROMPT_PACKAGES = {'zstandard': 'zstandard', 'fonttools': 'fontTools', 'pillow': 'PIL'}
PROMPTS_WRITTEN = re.compile(r'^.+: \d+ files, the font grew by -?\d+ bytes$')
SCAN_CHECKED = re.compile(r'^checked (\d+) of (\d+)$')
SCAN_TOOL = '55ea0c150aaf9dac93a2e6fd3b4ca11e1c572b5eee8b6bd74960535964384634'
PROMPT_TOOLS = 'd65e5ed91e94f72b4623eb41b6dbdbaf42ae0dde20ab73254c0387f5f0e8fe7e'

LIFT_FIXES = [
    ('sdk', 'void f_700579a430(Cpu *c) {\n    FN_ENTER(0x700579a430ull);\n',
     'void f_700579a430(Cpu *c) {\n    c->x[0] = 0; return;   /* bring-up: GpuErrorHandler (make_game.py) */\n'
     '    FN_ENTER(0x700579a430ull);\n'),
    ('sdk', '    { uint64_t ea = c->x[19] + 0x1b8ull; ST64(ea, x8); }\n    x8 = (uint64_t)(c->tpidrro);\n'
     '    { uint64_t ea = x8 + 0x1f0ull; uint64_t v = LD64(ea); x8 = (uint64_t)(v); }\n    c->tpidr = x8;\n',
     '    { uint64_t ea = c->x[19] + 0x1b8ull; ST64(ea, x8); }\n    x8 = (uint64_t)(c->tpidrro);\n'
     '    { uint64_t ea = x8 + 0x1f0ull; uint64_t v = LD64(ea); x8 = (uint64_t)(v); }\n'
     '    if (x8) c->tpidr = x8;   /* zero slot: keep the host\'s TLS (make_game.py) */\n'),
]
WIN = os.name == 'nt'
if WIN:
    LINK_LIBS = ['-lvulkan-1', '-lSDL3', '-lopus', '-lvpx', '-l:libdav1d.a', '-lzstd', '-l:libatomic.a', '-lbcrypt', '-lws2_32',
                 '-limm32', '-luuid', '-lole32', '-luser32', '-lshell32', '-lgdi32', '-ladvapi32', '-lwinmm']
else:
    LINK_LIBS = ['-lvulkan', '-lSDL3', '-lopus', '-lvpx', '-ldav1d', '-lzstd', '-l:libstdc++.a', '-latomic',
                 '-lm', '-lpthread', '-ldl']
SYSTEM_DLLS_SKIP = {'vulkan-1.dll'}
EXE_NAME = 'ocarina.exe' if WIN else 'ocarina'
XLATE_NAME = 'nx2navi_xlate.exe' if WIN else 'nx2navi_xlate'
GENERATED_HEADERS = ('layout.h', 'patches.h', 'romfs_config.h')
RUNTIME_LIB = 'libnx2navirt.a'
FFX4_DLLS = ('amd_fidelityfx_loader_vk.dll', 'amd_fidelityfx_upscaler_vk.dll')
FFX4_FG_DLL = 'amd_fidelityfx_framegeneration_vk.dll'
RUNTIME_DIRS = ('audio', 'cfg', 'diag', 'gfx', 'hle', 'input', 'nvn', 'pacing', 'patches', 'ui', 'vk')
RUNTIME_PATTERNS = ('*.c', '*.cpp', 'platform/*.c', *(f'{d}/*.{x}' for d in RUNTIME_DIRS for x in ('c', 'cpp')),
                    'third_party/imgui/*.cpp', 'third_party/imgui/backends/*.cpp')
INCLUDE = re.compile(r'^[ \t]*#[ \t]*include[ \t]+"([^"]+)"', re.M)

RUN_SCRIPT = ('import os, runpy, sys; s = sys.argv[1]; sys.argv = sys.argv[1:]; '
              'sys.path.insert(0, os.path.dirname(os.path.abspath(s))); runpy.run_path(s, run_name="__main__")')

EVENTS = False
GATE_ARGS = None
OUT_LOCK = threading.Lock()


def say(msg):
    with OUT_LOCK:
        print(msg, flush=True)


def emit(ev, **fields):
    if EVENTS:
        line = '@@' + json.dumps({'ev': ev, **fields}, ensure_ascii=False)
        with OUT_LOCK:
            print(line, flush=True)


def fail(msg):
    emit('error', message=msg)
    with OUT_LOCK:
        print(f'\nerror: {msg}', file=sys.stderr, flush=True)
    sys.exit(1)


def error_text(e):
    if isinstance(e, OSError) and e.strerror:
        if e.errno == errno.ENOSPC:
            return f'the disk is full ({e.filename})' if e.filename else 'the disk is full'
        return e.strerror + (f': {e.filename}' if e.filename else '')
    return f'{type(e).__name__}: {e}' if str(e) else type(e).__name__


class Background:
    def __init__(self, fn):
        self.exc = None
        self.thread = threading.Thread(target=self._run, args=(fn,), name='romfs', daemon=True)
        self.thread.start()

    def _run(self, fn):
        try:
            fn()
        except BaseException as e:   # noqa: B036 - handed to join()
            self.exc = e

    def failed(self):
        return self.exc is not None

    def join(self):
        self.thread.join()
        if self.exc is not None:
            raise self.exc


def py_cmd(script, *args):
    flags = ['-I'] if sys.flags.isolated else []
    return [sys.executable, *flags, '-c', RUN_SCRIPT, str(script), *map(str, args)]


def sha256_bytes(data):
    return hashlib.sha256(data).hexdigest()


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        while b := f.read(8 << 20):
            h.update(b)
    return h.hexdigest()


def tree_digest(root, patterns):
    h = hashlib.sha256()
    files = sorted({p for pat in patterns for p in Path(root).glob(pat) if p.is_file() and '__pycache__' not in p.parts})
    for p in files:
        h.update(p.relative_to(root).as_posix().encode() + b'\0')
        h.update(p.read_bytes())
        h.update(b'\0')
    return h.hexdigest()


class Source:
    def __init__(self, a):
        self.n = None
        if a.nup:
            try:
                self.n = nup.Nup(a.nup)
            except (OSError, nup.NupError) as e:
                fail(f'cannot read {a.nup}: {e}')
            pkgs = [p for p in self.n.packages if p['title_id'] in (TITLE_ID, TITLE_ID | 0x800)]
            if not pkgs:
                ids = ', '.join(f"{p['title_id']:016X}" for p in self.n.packages)
                fail(f'this .nup holds {ids}, not Ocarina of Time (0400486024DEC000)')
            pkgs.sort(key=lambda p: (p['kind'] != 1, -p['version']))
            self.exefs = {}
            for name in EXEFS_FILES:
                for p in pkgs:
                    fi = dict(self.n.tree(p['exefs'])).get(name) if p['exefs'] < self.n.ndir else None
                    magic = b'META' if name.endswith('.npdm') else b'NSO0'
                    if fi is not None and self.n.size(fi) >= 0x100 and next(self.n.chunks(fi))[:4] == magic:
                        self.exefs[name] = fi
                        break
            self.romfs = []
            for p in pkgs:
                if p['romfs'] < self.n.ndir:
                    self.romfs = list(self.n.tree(p['romfs']))
                if self.romfs:
                    break
            st = os.stat(a.nup)
            self.identity = f'nup {os.path.abspath(a.nup)} {st.st_size} {int(st.st_mtime)}'
        else:
            self.exefs_dir, self.romfs_dir = Path(a.exefs), Path(a.romfs)
            for d in (self.exefs_dir, self.romfs_dir):
                if not d.is_dir():
                    fail(f'{d} is not a folder')
            self.exefs = {name: self.exefs_dir / name for name in EXEFS_FILES if (self.exefs_dir / name).is_file()}
            self.romfs = [(p.relative_to(self.romfs_dir).as_posix(), p) for p in self.romfs_dir.rglob('*') if p.is_file()]
            h = hashlib.sha256()
            for rel, f in sorted([(n, self.exefs[n]) for n in self.exefs] + self.romfs):
                st = f.stat()
                h.update(f'{rel}\0{st.st_size}\0{st.st_mtime_ns}\n'.encode('utf-8', 'surrogateescape'))
            self.identity = (f'folders {self.exefs_dir.resolve()} {self.romfs_dir.resolve()} {len(self.romfs)} '
                             f'{h.hexdigest()[:32]}')
        missing = [x for x in EXEFS_FILES if x not in self.exefs]
        if missing:
            fail(f'the ExeFS lacks {", ".join(missing)} (or holds something else under that name)')
        if not self.romfs:
            fail('the RomFS is empty')

    def read_exefs(self, name):
        return self.n.read(self.exefs[name]) if self.n else self.exefs[name].read_bytes()

    def romfs_size(self, ref):
        return self.n.size(ref) if self.n else ref.stat().st_size

    def romfs_chunks(self, ref):
        if self.n:
            yield from self.n.chunks(ref)
            return
        with open(ref, 'rb') as f:
            while b := f.read(nup.CHUNK):
                yield b


class BuiltSource:
    def __init__(self, out, state):
        self.out = out
        if state.get('version') != STATE_VERSION or not state.get('input'):
            fail(f'{out} holds no finished build of this kit to rebuild from: give --nup, or --exefs and --romfs')
        for step in ('exefs', 'romfs'):
            if step not in state.get('steps', {}):
                fail(f'the last build in {out} did not finish its {step} step: give the game files again')
        missing = [n for n in EXEFS_FILES if not (out / 'exefs' / n).is_file()]
        missing += [f'romfs/{n}' for n in ('romfs.bin', 'index.json') if not (out / 'romfs' / n).is_file()]
        if missing:
            fail(f'{out} lacks {", ".join(missing)}: give the game files again')
        self.identity = state['input']

    def read_exefs(self, name):
        return (self.out / 'exefs' / name).read_bytes()


def check(src):
    main = src.read_exefs('main')
    if main[:4] != b'NSO0':
        fail('main is not an NSO module')
    build = main[0x40:0x50].hex()
    if build != MAIN_BUILD_ID:
        fail(f'this is not game version 1.1.1: main has build {build[:8]}, the recomp supports build {MAIN_BUILD_ID[:8]}. '
             'Install the 1.1.1 update and dump the game again.')
    return main


def flat_nso(raw, name):
    flags = struct.unpack_from('<I', raw, 0xC)[0]
    out = bytearray(raw[:0x100])
    for i, seg in enumerate(('text', 'ro', 'data')):
        file_off, mem_off, size = struct.unpack_from('<III', raw, 0x10 + i * 0x10)
        stored = struct.unpack_from('<I', raw, 0x60 + i * 4)[0]
        data = raw[file_off:file_off + (stored if flags >> i & 1 else size)]
        if flags >> i & 1:
            if flags & 0x80:
                try:
                    import zbic
                except ImportError:
                    fail('the game\'s modules need the zbic package: pip install zbic')
                data = zbic.decompress(data)
            else:
                import lz4.block
                data = lz4.block.decompress(data, uncompressed_size=size)
        if len(data) != size:
            fail(f'{name}: {seg} unpacks to {len(data):#x} bytes, its header says {size:#x}')
        if flags >> (3 + i) & 1 and hashlib.sha256(data).digest() != raw[0xA0 + 0x20 * i:0xC0 + 0x20 * i]:
            fail(f'{name}: the {seg} segment is damaged (sha256 mismatch)')
        struct.pack_into('<III', out, 0x10 + i * 0x10, len(out), mem_off, size)
        struct.pack_into('<I', out, 0x60 + i * 4, size)
        out += data
    struct.pack_into('<I', out, 0xC, flags & ~0x87)
    return bytes(out)


def step_exefs(src, out):
    dst = out / 'exefs'
    dst.mkdir(parents=True, exist_ok=True)
    for i, name in enumerate(EXEFS_FILES):
        raw = src.read_exefs(name)
        data = flat_nso(raw, name) if name in MODULES else raw
        (dst / name).write_bytes(data)
        say(f'  {name:9} {len(raw) / 1e6:6.1f} MB -> {len(data) / 1e6:6.1f} MB')
        emit('progress', step='exefs', done=i + 1, total=len(EXEFS_FILES), unit='files')


def exefs_digest(out):
    return tree_digest(out / 'exefs', EXEFS_FILES)


ROMFS_READ_AHEAD = 64 << 20


def unreadable(rel, err):
    if isinstance(err, nup.NupError):
        why = str(err).split(': ', 1)[-1]
        if why.startswith('damaged (') and why.endswith(')'):
            why = why[len('damaged ('):-1]
        fail(f'the dump is damaged: {rel}: {why}. Copy the dump to this PC again, or dump the game again, and then '
             'try again')
    fail(f'cannot read {rel} from the game files: {error_text(err)}')


def romfs_read_groups(files):
    group, buffered = [], 0
    for entry in files:
        size = entry[2]
        ahead = size if size <= ROMFS_READ_AHEAD else 0
        if buffered + ahead > ROMFS_READ_AHEAD or len(group) == 256:
            yield group
            group, buffered = [], 0
        group.append(entry)
        buffered += ahead
    if group:
        yield group


def step_romfs(src, out, jobs):
    dst = out / 'romfs'
    dst.mkdir(parents=True, exist_ok=True)
    files = sorted(((rel, ref, src.romfs_size(ref)) for rel, ref in src.romfs), key=lambda e: e[0].encode())
    total = sum(size for _, _, size in files)
    part = dst / 'romfs.bin.part'
    try:
        part.unlink(missing_ok=True)
    except OSError as e:
        fail(f'cannot remove {part}, left by an earlier run ({e.strerror or e}): close whatever has it open')
    free = shutil.disk_usage(dst).free
    if free < total + (1 << 30):
        fail(f'{dst} needs {total / 1e9:.1f} GB free for the RomFS image, it has {free / 1e9:.1f} GB')
    say(f'  {len(files)} files, {total / 1e9:.1f} GB')
    entries, h, pos, done, t0 = [], hashlib.sha256(), 0, 0, time.time()
    emit('progress', step='romfs', done=0, total=total, unit='bytes')

    def load(e):
        _, ref, size = e
        if size > ROMFS_READ_AHEAD:
            return None
        try:
            return b''.join(src.romfs_chunks(ref))
        except (nup.NupError, OSError) as err:
            return err

    def pieces(rel, ref, data):
        if isinstance(data, Exception):
            unreadable(rel, data)
        if data is not None:
            yield data
            return
        try:
            yield from src.romfs_chunks(ref)
        except (nup.NupError, OSError) as err:
            unreadable(rel, err)

    try:
        with open(part, 'wb', buffering=16 << 20) as img, concurrent.futures.ThreadPoolExecutor(jobs) as ex:
            for batch, group in enumerate(romfs_read_groups(files)):
                for (rel, ref, size), data in zip(group, ex.map(load, group)):
                    pad = -pos % ALIGN
                    if pad:
                        img.write(b'\0' * pad)
                        h.update(b'\0' * pad)
                        pos += pad
                    offset = pos
                    for chunk in pieces(rel, ref, data):
                        img.write(chunk)
                        h.update(chunk)
                        pos += len(chunk)
                    if pos - offset != size:
                        fail(f'{rel} changed size while packing the RomFS: expected {size} bytes, read {pos - offset}')
                    entries.append({'path': rel, 'offset': offset, 'size': size})
                    done += size
                emit('progress', step='romfs', done=done, total=total, unit='bytes')
                if batch % 40 == 0:
                    say(f'  RomFS {done / 1e9:5.1f} of {total / 1e9:.1f} GB, {time.time() - t0:.0f} s')
    except BaseException:
        try:
            part.unlink(missing_ok=True)
        except OSError:
            pass
        raise
    part.replace(dst / 'romfs.bin')
    index = {'status': 'pass', 'romfs_size': pos, 'romfs_sha256': h.hexdigest(), 'files': entries}
    (dst / 'index.json').write_text(json.dumps(index), encoding='utf-8')
    say(f'  RomFS image {pos / 1e9:.2f} GB in {time.time() - t0:.0f} s')


EMITTED = re.compile(r'emitted (\d+)/(\d+) chunks')


def lift_digest():
    h = hashlib.sha256(recomp_digest().encode())
    h.update(repr(LIFT_FIXES).encode())
    return h.hexdigest()


def recomp_digest(kit=None):
    return tree_digest((kit or KIT) / 'recomp', ('*.py', 'patches-v111.txt'))


MOD_STAGES = (('cheats.py', 'patches-cheats.txt', 'cheats.status', 'code cheats'),
              ('exefs_patches.py', 'patches-exefs.txt', 'exefs.status', 'ExeFS patches'))
MOD_RECORD = 'mod-code.json'


def mod_code(out):
    return mod_files.code_record(out / 'mods', mod_files.main_build_id(out / 'exefs' / 'main'))


def mod_digest(out):
    code = mod_code(out)
    if not code:
        return ''
    h = hashlib.sha256(json.dumps(code, sort_keys=True).encode())
    for tool in ('cheats.py', 'exefs_patches.py', 'mod_files.py'):
        h.update(sha256_file(KIT / 'tools' / tool).encode() if (KIT / 'tools' / tool).is_file() else b'no tool')
    return h.hexdigest()


def lifter_env():
    env = dict(os.environ)
    args = GATE_ARGS if GATE_ARGS is not None else argparse.Namespace()
    if getattr(args, 'mem_reserve', None) is not None:
        env[memgate.ENV_RESERVE] = str(args.mem_reserve)
    if getattr(args, 'no_mem_gate', False):
        env[memgate.ENV_GATE] = 'off'
    return env


def run_lifter(out, lift, patches):
    cmd = py_cmd(KIT / 'recomp' / 'gen.py', '--exefs', out / 'exefs', '--out', lift, '--patches', patches)
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding='utf-8',
                         errors='replace', bufsize=1, env=lifter_env())
    for line in p.stdout:
        line = line.rstrip('\n')
        say(f'  {line}')
        m = EMITTED.search(line)
        if m:
            emit('progress', step='lift', done=int(m.group(1)), total=int(m.group(2)), unit='chunks')
    return p.wait()


def mod_patches(out, cache, plain):
    patches, done = plain, []
    code = mod_code(out)
    for tool, written, status, what in MOD_STAGES:
        (cache / status).unlink(missing_ok=True)
        if not code:
            continue
        cmd = py_cmd(KIT / 'tools' / tool, '--exefs', out / 'exefs', '--mods', out / 'mods', '--patches', patches,
                     '--out', cache / written, '--status', cache / status)
        r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding='utf-8',
                           errors='replace')
        for line in r.stdout.splitlines():
            say(f'  {line}')
        if r.returncode:
            (cache / status).unlink(missing_ok=True)
            say(f'  the mods\' {what} could not be built in (see above): the game is built without them')
        else:
            patches = cache / written
            done.append(status)
    return patches, done, code


def step_lift(out, cache):
    lift = cache / 'lift'
    plain = KIT / 'recomp' / 'patches-v111.txt'
    (cache / MOD_RECORD).unlink(missing_ok=True)
    patches, done, code = mod_patches(out, cache, plain)
    if run_lifter(out, lift, patches):
        if patches == plain:
            fail('the code generator failed (see above)')
        say('  the code generator refused the mods\' code: the game is lifted without it')
        for status in done:
            (cache / status).unlink(missing_ok=True)
        if run_lifter(out, lift, plain):
            fail('the code generator failed (see above)')
    if code:
        (cache / MOD_RECORD).write_text(json.dumps(code, indent=1, sort_keys=True), encoding='utf-8')
    for mod, old, new in LIFT_FIXES:
        hits = [p for p in sorted((lift / mod).glob('*.c')) if old in p.read_text(encoding='utf-8')]
        if len(hits) != 1 or hits[0].read_text(encoding='utf-8').count(old) != 1:
            fail(f'a bring-up fix found its anchor {len(hits)} times in the lifted {mod}, not once')
        p = hits[0]
        p.write_text(p.read_text(encoding='utf-8').replace(old, new), encoding='utf-8', newline='\n')


def c_string(text):
    return json.dumps(text, ensure_ascii=False)


def modules_c(lift, out, gen):
    layout = json.loads((lift / 'layout.json').read_text(encoding='utf-8'))
    L = [f'/* {GENERATED_MARKER}: module table, written by tools/make_game.py modules_c. Do not edit or commit. */',
         '#include "dispatch.h"', '#include "host.h"', '#include "layout.h"', '']
    for m in layout:
        n = m['name']
        L += [f'extern const uint64_t {n}_fn_addrs[];', f'extern const GuestFn {n}_fn_ptrs[];',
              f'extern const uint64_t {n}_fn_count;']
    L += ['', 'const RtLayoutEntry rt_layout[] = {']
    L += [f'    {{"{m["name"]}", MOD_{m["name"].upper()}_BASE, MOD_{m["name"].upper()}_SIZE}},' for m in layout]
    L += ['};', f'const int rt_layout_count = {len(layout)};', '', 'int rt_linked_modules(RtModule *out, int max) {',
          '    int n = 0;']
    for m in layout:
        n, u = m['name'], m['name'].upper()
        L.append(f'    if (n < max) out[n++] = (RtModule){{"{n}", MOD_{u}_BASE, MOD_{u}_SIZE, {n}_fn_addrs, {n}_fn_ptrs, '
                 f'{n}_fn_count}};')
    L += ['    return n;', '}', '', 'const RtFileHash rt_exefs_sha256[] = {']
    L += [f'    {{"{name}", "{sha256_file(out / "exefs" / name)}"}},' for name in EXEFS_FILES]
    L += ['};', f'const int rt_exefs_sha256_count = {len(EXEFS_FILES)};', '',
          f'const char rt_default_exefs[] = {c_string(str(out / "exefs"))};',
          f'const char rt_default_romfs[] = {c_string(str(out / "romfs"))};',
          f'const char rt_default_profile[] = {c_string(str(out / "profile"))};',
          'const int rt_game_version = 111;',
          'const unsigned long long rt_app_pool_bytes = 9809ull << 20;   /* the Switch 2 application pool */',
          'const int rt_nvn_api_version[2] = {231, 40};   /* NVN2: main asks for major 231, minor 40 or later */',
          f'const char rt_port_version[] = {c_string(kit_version())};']
    p = gen / 'modules.c'
    write_if_changed(p, '\n'.join(L) + '\n')
    if WIN:
        write_if_changed(gen / 'shim.c', '/* Defined by the SDL window path only; this build uses the Win32 one. */\n'
                                         'void menu_sdl_event(void *e) { (void)e; }\n')
    return p


def write_if_changed(path, text):
    if not path.is_file() or path.read_text(encoding='utf-8') != text:
        path.write_text(text, encoding='utf-8')


def kit_version():
    v = KIT / 'VERSION'
    return v.read_text(encoding='utf-8').strip() if v.is_file() else '0.0.1-ocarina'


def pe_imports(path):
    d = Path(path).read_bytes()
    pe = struct.unpack_from('<I', d, 0x3C)[0]
    nsec, opt = struct.unpack_from('<H', d, pe + 6)[0], struct.unpack_from('<H', d, pe + 20)[0]
    sec = pe + 24 + opt
    rva = struct.unpack_from('<I', d, pe + 24 + 120)[0]

    def off(r):
        for i in range(nsec):
            va, vs, raw = (struct.unpack_from('<I', d, sec + 40 * i + k)[0] for k in (12, 8, 20))
            if va <= r < va + max(vs, 1):
                return r - va + raw
        return None

    names, at = [], off(rva) if rva else None
    while at is not None:
        name_rva = struct.unpack_from('<I', d, at + 12)[0]
        if not name_rva:
            break
        o = off(name_rva)
        names.append(d[o:d.index(b'\0', o)].decode('ascii').lower())
        at += 20
    return names


def copy_if_changed(src, dst):
    dst = Path(dst)
    s = os.stat(src)
    try:
        d = os.stat(dst)
        if d.st_size == s.st_size and sha256_file(src) == sha256_file(dst):
            return False
    except FileNotFoundError:
        pass
    replacement = None
    try:
        fd, name = tempfile.mkstemp(dir=dst.parent, prefix='.nx2navi-copy-')
        os.close(fd)
        replacement = Path(name)
        shutil.copy2(src, replacement)
        os.replace(replacement, dst)
    except PermissionError:
        fail(f'cannot replace {dst}: close the game and check output-directory permissions')
    finally:
        if replacement is not None:
            try:
                replacement.unlink(missing_ok=True)
            except OSError:
                pass
    return True


DLSS_DLL, DLSS_NOTICE = 'nvngx_dlss.dll', 'nvngx_dlss-NOTICE.txt'
DLSS_SO_PREFIX, DLSS_SO_NOTICE = 'libnvidia-ngx-dlss.so.', 'libnvidia-ngx-dlss-NOTICE.txt'
DLSSG_DIR = 'dlssg'
DLSSG_FILES = ('nx2navi_dlssg.dll', 'sl.interposer.dll', 'sl.common.dll', 'sl.dlss_g.dll', 'sl.pcl.dll', 'sl.reflex.dll',
               'nvngx_dlssg.dll')


def notice_sha256(path):
    try:
        for line in path.read_text(encoding='utf-8').splitlines():
            if line.startswith('sha256 '):
                return line.split()[1].lower()
    except (OSError, UnicodeDecodeError):
        pass
    return None


def dlss_files(folder, linux):
    return sorted(p for p in folder.glob(DLSS_SO_PREFIX + '*' if linux else DLSS_DLL) if p.is_file())


def place_dlss(out, linux=False):
    note_name = DLSS_SO_NOTICE if linux else DLSS_NOTICE
    srcs, note = dlss_files(KIT / 'bin', linux), KIT / 'bin' / note_name
    if not srcs:
        return None
    src = srcs[-1]
    beside = dlss_files(out, linux)
    for dst in beside:
        s, d = os.stat(src), os.stat(dst)
        if dst.name != src.name or d.st_size != s.st_size or int(d.st_mtime) != int(s.st_mtime):
            have = sha256_file(dst)
            if have != notice_sha256(out / note_name) and have != sha256_file(src):
                return f"{dst.name} kept: the one already beside the game is not the kit's (another DLSS version)"
    copy_if_changed(src, out / src.name)
    for dst in beside:
        if not dst.samefile(out / src.name):
            dst.unlink()
    if note.is_file() and (not (out / note_name).is_file() or (out / note_name).read_bytes() != note.read_bytes()):
        shutil.copy2(note, out / note_name)
    return None


def place_dlssg(out):
    src = KIT / 'bin' / DLSSG_DIR
    if not all((src / n).is_file() for n in DLSSG_FILES):
        return 0
    files = sorted(p for p in src.iterdir() if p.is_file())
    for p in files:
        copy_if_changed(p, out / p.name)
    return len(files)


def copy_dlls(exe, msys_bin, out):
    seen, todo, copied = set(), [exe], []
    while todo:
        for name in pe_imports(todo.pop()):
            src = msys_bin / name
            if name in seen or name in SYSTEM_DLLS_SKIP or not src.is_file():
                continue
            seen.add(name)
            copy_if_changed(src, out / src.name)
            copied.append(src.name)
            todo.append(src)
    return copied


def parse_deps(text):
    text = text.replace('\\\r\n', ' ').replace('\\\n', ' ')
    tokens, cur, i = [], '', 0
    while i < len(text):
        c = text[i]
        if c == '\\' and i + 1 < len(text) and text[i + 1] in ' #':
            cur += text[i + 1]
            i += 2
            continue
        if c == '$' and text[i + 1:i + 2] == '$':
            cur += '$'
            i += 2
            continue
        if c.isspace():
            if cur:
                tokens.append(cur)
            cur = ''
        else:
            cur += c
        i += 1
    if cur:
        tokens.append(cur)
    for k, t in enumerate(tokens):
        if t.endswith(':'):
            return tokens[k + 1:]
    return tokens


class ObjectCache:
    def __init__(self, path, roots=None):
        self.path = path
        self.roots = {k: os.path.normcase(os.path.abspath(v)) for k, v in (roots or {}).items()}
        try:
            self.entries = json.loads(path.read_text(encoding='utf-8')) if path.is_file() else {}
        except (OSError, ValueError):
            self.entries = {}
        self._sha = {}

    def norm(self, p):
        a = os.path.normcase(os.path.abspath(p))
        for name, root in self.roots.items():
            if a == root or a.startswith(root + os.sep):
                return name + a[len(root):].replace(os.sep, '/')
        return a

    def real(self, p):
        for name, root in self.roots.items():
            if p.startswith(name):
                return root + p[len(name):].replace('/', os.sep)
        return p

    def file_sha(self, p):
        p = os.path.normcase(os.path.abspath(p))
        if p not in self._sha:
            try:
                self._sha[p] = sha256_file(p)
            except OSError:
                self._sha[p] = None
        return self._sha[p]

    def _same(self, p, rec):
        path = self.real(p)
        if isinstance(rec, str):
            return self.file_sha(path) == rec
        digest, size, mtime = rec
        try:
            st = os.stat(path)
        except OSError:
            return False
        if st.st_size == size and st.st_mtime_ns == mtime:
            return True
        return st.st_size == size and self.file_sha(path) == digest

    def current(self, obj, key):
        e = self.entries.get(obj.name)
        return (e is not None and e.get('key') == key and obj.is_file()
                and all(self._same(p, rec) for p, rec in e.get('deps', {}).items()))

    def record(self, obj, key, dep_file, src):
        try:
            deps = parse_deps(Path(dep_file).read_text(encoding='utf-8', errors='replace'))
        except OSError:
            deps = []
        out = {}
        for p in deps or [str(src)]:
            try:
                st = os.stat(p)
                out[self.norm(p)] = [self.file_sha(p), st.st_size, st.st_mtime_ns]
            except OSError:
                out[self.norm(p)] = [None, -1, -1]
        self.entries[obj.name] = {'key': key, 'deps': out}

    def forget(self, obj):
        self.entries.pop(obj.name, None)

    def keep_only(self, names):
        gone = [n for n in self.entries if n not in names]
        for n in gone:
            del self.entries[n]
        return len(gone)

    def adopt(self, obj, key, old_keys, old_kits=()):
        e = self.entries.get(obj.name)
        if not e or e.get('key') not in old_keys or not obj.is_file():
            return 0
        kit = self.roots.get('<kit>')
        kits = [os.path.normcase(os.path.abspath(k)) for k in old_kits]
        deps = {}
        for p, rec in e.get('deps', {}).items():
            path = self.real(p)
            a = os.path.normcase(os.path.abspath(path))
            for k in kits:
                if kit and a.startswith(k + os.sep):
                    path = kit + a[len(k):]
                    break
            digest = rec if isinstance(rec, str) else rec[0]
            if not digest or self.file_sha(path) != digest:
                return 0
            st = os.stat(path)
            deps[self.norm(path)] = [digest, st.st_size, st.st_mtime_ns]
        self.entries[obj.name] = {'key': key, 'deps': deps}
        return 1

    def save(self):
        tmp = self.path.with_suffix('.tmp')
        tmp.write_text(json.dumps(self.entries), encoding='utf-8')
        tmp.replace(self.path)


def compiler_ids(clang, env):
    p = subprocess.run([str(clang), '--version'], capture_output=True, text=True, env=env, errors='replace')
    st = os.stat(clang)
    return (sha256_bytes(f'{p.stdout}\n{st.st_size}'.encode()),
            sha256_bytes(f'{p.stdout}\n{st.st_size}\n{int(st.st_mtime)}'.encode()))


def compiler_id(clang, env):
    return compiler_ids(clang, env)[0]


def object_key(cmd, src, dst, cache, salt, sysroot=False):
    flags = [c for c in cmd[1:] if c not in (str(src), str(dst)) and (sysroot or not c.startswith('--sysroot='))]

    def rooted(f, name, root):
        head = PGO_USE if f.startswith(PGO_USE) else ''
        path = os.path.normcase(f[len(head):])
        return head + name + path[len(root):].replace(os.sep, '/') if path.startswith(root) else f
    for name, root in cache.roots.items():
        flags = [rooted(f, name, root) for f in flags]
    return sha256_bytes(json.dumps([salt, [os.path.basename(cmd[0])] + flags, cache.file_sha(src)]).encode())


def legacy_key(src, dst, cache, salt, clang, clangxx, kit, lift, gen, vma):
    rt = Path(kit) / 'runtime'
    cpp = src.suffix in ('.cpp', '.cxx')
    guest = dst.name.startswith(('main_', 'sdk_', 'subsdk0_'))
    cmd = [str(clangxx if cpp else clang), '-c', '-std=gnu++17' if cpp else '-std=gnu11', '-O1' if guest else '-O2',
           '-fno-strict-aliasing', '-w', '-I', str(rt), '-I', str(lift), '-I', str(gen),
           '-I', str(rt / 'third_party' / 'imgui'), '-I', str(vma), '-o', str(dst), str(src)]
    flags = [c for c in cmd if c not in (str(src), str(dst))]
    return sha256_bytes(json.dumps([salt, flags, cache.file_sha(src)]).encode())


def earlier_kits():
    app = KIT.parent
    if app.name != 'app':
        return []
    kits = {p for p in app.iterdir() if p.is_dir() and p != KIT and (p / 'runtime').is_dir()}
    try:
        prev = (app / 'previous.txt').read_text(encoding='utf-8').strip()
        if prev and app / prev != KIT:
            kits.add(app / prev)
    except OSError:
        pass
    return sorted(kits)


def default_jobs(cpus=None):
    cpus = cpus or os.cpu_count() or 4
    return cpus, f'{cpus} processors'


def gate_settings():
    args = GATE_ARGS if GATE_ARGS is not None else argparse.Namespace()
    s = memgate.settings(args)
    off = ''
    if not s['enabled']:
        off = '--no-mem-gate' if getattr(args, 'no_mem_gate', False) else \
            f'{memgate.ENV_GATE}={os.environ.get(memgate.ENV_GATE, "").strip()}'
    return s, off


def make_gate(jobs):
    return memgate.from_args(GATE_ARGS if GATE_ARGS is not None else argparse.Namespace(), jobs,
                             log=lambda line: say(f'  {line}'))


def jobs_line(jobs, why):
    s, off = gate_settings()
    if off:
        return f'compiles at a time: up to {jobs} ({why}) (memory gate off: {off})'
    limit = 'no ceiling' if s['ceiling'] is None else f'at most {memgate.gib(s["ceiling"])} GiB estimated in all'
    return (f'compiles at a time: up to {jobs} ({why}); the memory gate starts each one when the free memory holds it '
            f'(keeps {memgate.gib(s["reserve"])} GiB free, {limit})')


SOURCE_WAIT, SOURCE_PAUSE = 60, 2


def source_size(src):
    src = Path(src)
    end = time.monotonic() + SOURCE_WAIT
    told = False
    while True:
        try:
            return src.stat().st_size
        except OSError as e:
            away = not src.parent.is_dir()
            if not away or time.monotonic() >= end:
                fail(f'cannot read {src} ({e.strerror or e}): '
                     + ('the drive or network share with the game folder stopped answering; check it and run the '
                        'build again' if away else
                        'the file is gone from the build cache; delete the lift folder in the build cache and run '
                        'the build again, which makes it anew'))
            if not told:
                say(f'  {src.parent} does not answer; waiting up to {SOURCE_WAIT} s for the drive to come back')
                told = True
            time.sleep(SOURCE_PAUSE)


CACHE_SAVE_EVERY, CACHE_SAVE_PAUSE = 25, 2.0

DEFAULT_NEED_C = 400 << 20
DEFAULT_NEED_CXX = 1 << 30
CXX_SUFFIXES = ('.cpp', '.cc', '.cxx')


def compile_order(jobs, estimates):
    seconds = {i: estimates.objects[name]['seconds'] for i, name, _, how in jobs if how == 'learned'}
    rates = sorted(seconds[i] / cost for i, _, cost, _ in jobs if i in seconds and cost > 0)
    rate = rates[len(rates) // 2] if rates else None

    def key(job):
        i, _, cost, _ = job
        if rate is None:
            return cost
        return seconds[i] if i in seconds else cost * rate
    return [job[0] for job in sorted(jobs, key=key, reverse=True)]


def compile_all(srcs, cmd_for, cache, jobs, env, progress=None, salt='', stop=None, alts=None, *, gate=None,
                estimates=None, costs=None, plan=None, measured=None):
    todo, keys = [], {}
    adopted = 0
    for src, dst in srcs:
        keys[dst] = object_key(cmd_for(src, dst), src, dst, cache, salt)
        if cache.current(dst, keys[dst]):
            continue
        if alts and cache.adopt(dst, keys[dst], *alts(src, dst)):
            adopted += 1
            continue
        todo.append((src, dst))
    reused = len(srcs) - len(todo)
    if adopted:
        say(f'  {adopted} objects an earlier version compiled are kept: the same compile of the same sources')

    measure = gate is not None or estimates is not None
    est = estimates if estimates is not None else memgate.Estimates()
    own_gate = gate is None
    if own_gate:
        gate = memgate.Gate(max(1, jobs), enabled=False, log=say)
    costs = costs or {}
    info = {}
    planned = []
    for i, (src, dst) in enumerate(todo):
        size = source_size(src)
        lifted = Path(src) in costs
        cost, switch_fn = costs[Path(src)] if lifted else (size, 0)
        default = None if lifted else DEFAULT_NEED_CXX if Path(src).suffix in CXX_SUFFIXES else DEFAULT_NEED_C
        need, how = est.need(dst.name, size, cost if lifted else 0, switch_fn, default=default)
        info[dst] = (size, cost if lifted else 0, switch_fn, need)
        planned.append((i, dst.name, cost, how))
    todo = [todo[i] for i in compile_order(planned, est)]
    if plan:
        plan(collections.Counter(how for *_, how in planned))

    turn = [0]
    turn_cv = threading.Condition()

    def cc(index, job):
        src, dst = job
        dep = dst.with_suffix('.d')
        with turn_cv:
            while turn[0] != index:
                turn_cv.wait()
        try:
            if stop and stop():
                return src, dst, dep, None, '', None, False
            lease = gate.acquire(dst.name, info[dst][3], cancel=stop)
            if lease is None:
                return src, dst, dep, None, '', None, False
        finally:
            with turn_cv:
                turn[0] += 1
                turn_cv.notify_all()
        try:
            if stop and stop():
                return src, dst, dep, None, '', None, False
            cmd = cmd_for(src, dst)
            cmd[1:1] = ['-MMD', '-MF', str(dep)]
            if measure:
                r = memgate.run(cmd, env=env, on_start=lease.started)
                return src, dst, dep, r.returncode, r.stderr_tail, r, lease.pressed
            p = subprocess.run(cmd, capture_output=True, text=True, env=env, errors='replace')
            return src, dst, dep, p.returncode, p.stderr[-1500:], None, False
        finally:
            gate.release(lease)

    def save_estimates():
        if estimates is not None and estimates.path is not None:
            estimates.save()

    done, bad = reused, []
    unsaved, saved_at = 0, time.monotonic()
    if progress:
        progress(done, len(srcs))
    try:
        with concurrent.futures.ThreadPoolExecutor(max(1, jobs)) as ex:
            for result in concurrent.futures.as_completed([ex.submit(cc, i, job) for i, job in enumerate(todo)]):
                src, dst, dep, rc, err, run, pressed = result.result()
                done += 1
                if rc is None:
                    continue
                if rc:
                    bad.append((src, err))
                    cache.forget(dst)
                else:
                    cache.record(dst, keys[dst], dep, src)
                    if run is not None:
                        size, cost, switch_fn, _ = info[dst]
                        est.record(dst.name, run.peak_bytes, run.seconds, size, cost, switch_fn, keep_higher=pressed)
                        if measured is not None:
                            measured.append((dst.name, run.peak_bytes, run.seconds))
                    unsaved += 1
                if unsaved >= CACHE_SAVE_EVERY and time.monotonic() - saved_at >= CACHE_SAVE_PAUSE:
                    try:
                        cache.save()
                        save_estimates()
                    except OSError:
                        pass
                    unsaved, saved_at = 0, time.monotonic()
                if progress:
                    progress(done, len(srcs))
    finally:
        if own_gate:
            gate.close()
    cache.save()
    try:
        save_estimates()
    except OSError:
        pass
    return len(todo), reused, bad


def report_failures(bad):
    for src, err in bad[:3]:
        print(f'--- {src}\n{err}', file=sys.stderr)
    print('did not compile: ' + ', '.join(Path(src).name for src, _ in bad), file=sys.stderr, flush=True)


def runtime_headers(rt):
    try:
        import build_shaders
    except ModuleNotFoundError:
        return
    stale = build_shaders.stale(rt)
    if stale:
        say(f'  building {len(stale)} of the runtime\'s own headers (runtime/*_spv.h, the menu font)')
        build_shaders.build(rt, stale)


def preflight(msys, jobs, sources=None, work=None):
    clang, clangxx, env, vma = toolchain(msys)
    version = clang_version(clang, env)
    sdl = sdl3_choice(clang, env)
    say(f'check: {sdl3_line(clang, version, sdl)}')
    rt = KIT / 'runtime'
    runtime_headers(rt)
    inc = ['-I', str(rt), *runtime_includes(rt), '-I', str(rt / 'third_party' / 'imgui'), '-I', str(vma)]
    vulkan = vulkan_headers(clang, env)
    cmd_for = runtime_cmd(clang, clangxx, inc, first=vulkan + (sdl.include if sdl else []))
    if sources is None:
        tainted = per_install_names()
        sources = [p for p, _ in runtime_sources() if not per_install(p, tainted)]

    gate = make_gate(jobs)

    def check(src):
        cmd = [c for c in cmd_for(src, Path('unused.o')) if c not in ('-c', '-o', 'unused.o')]
        need = (DEFAULT_NEED_CXX if Path(src).suffix in CXX_SUFFIXES else DEFAULT_NEED_C) // 2
        with gate.acquire(Path(src).name, need):
            p = subprocess.run([cmd[0], '-fsyntax-only', *cmd[1:]], capture_output=True, text=True, env=env,
                               errors='replace')
        return src, p.returncode, p.stderr[-1500:]

    t0 = time.time()
    try:
        with concurrent.futures.ThreadPoolExecutor(max(1, jobs)) as ex:
            bad = [(src, err) for src, rc, err in ex.map(check, sources) if rc]
    finally:
        gate.close()
    if bad:
        report_failures(bad)
        lacking = sorted(set(re.findall(r"fatal error: '([^']+)' file not found", ''.join(e for _, e in bad))))
        fail(f'{len(bad)} of the runtime\'s {len(sources)} files do not compile on this machine, so the build would stop '
             f'in its compile step. ' +
             (f'These headers are missing: {", ".join(lacking)}. Install the development packages '
              f'README-BuildKit.md lists under "Linux".' if lacking else
              f'The compiler ({version}) or a library\'s headers are older than the runtime needs: the releases are '
              f'built with clang 23, and the Vulkan headers come ' +
              (f'from {vulkan[1]}' if vulkan else 'from the system') + '. The errors above name what is missing.'))
    if work:
        Path(work).mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='nx2navi-check-', dir=work) as tmp:
        probe, exe = Path(tmp) / 'probe.c', Path(tmp) / 'probe'
        probe.write_text(SDL3_PROBE, encoding='ascii')
        ld = (['-fuse-ld=lld'] if shutil.which('ld.lld') else []) + ['-no-pie', '-Wl,-rpath,$ORIGIN']
        p = subprocess.run([str(clangxx), *ld, *(sdl.include if sdl else []), '-o', str(exe), '-x', 'c', str(probe),
                            '-x', 'none', *link_libs(sdl)], capture_output=True, text=True, env=env, errors='replace')
        if p.returncode:
            print(p.stderr[-2000:], file=sys.stderr)
            fail('the libraries the game links are not all there (the linker\'s lines above name the missing ones): '
                 'install the development packages README-BuildKit.md lists under "Linux". '
                 'On Fedora the static C++ library is its own package, libstdc++-static.' +
                 (' The compiler finds SDL3\'s headers, but the linker no libSDL3: install SDL3\'s development '
                  'package, or build with the kit\'s SDL3: NX2SARIA_SDL3=kit'
                  if sdl and sdl.source == 'system' and 'SDL3' in p.stderr else ''))
        if sdl and sdl.source == 'kit':
            shutil.copy2(sdl.lib, Path(tmp) / SDL3_SO)
        try:
            r = subprocess.run([str(exe)], capture_output=True, text=True, env=env, errors='replace', timeout=60)
        except (OSError, subprocess.TimeoutExpired) as e:
            r = subprocess.CompletedProcess([str(exe)], -1, '', error_text(e))
    if r.returncode:
        print(r.stderr[-2000:], file=sys.stderr)
        lib = missing_library(r.stderr)
        package = SDL3_NEEDS.get(lib) if sdl and sdl.source == 'kit' else None
        ended = f'it was ended by signal {-r.returncode}' if r.returncode < 0 else f'exit code {r.returncode}'
        fail(f'a program linked against the game\'s libraries does not start here' +
             (f': {lib} is missing' if lib else
              f' ({ended}' + ('; its lines above say why)' if r.stderr.strip() else ')')) +
             (f'. The kit\'s SDL3 needs it; on Ubuntu 24.04 and Linux Mint 22 the package {package} holds it '
              f'(sudo apt install {package})' if package else
              '. Install the package that holds it, or build with the kit\'s SDL3: NX2SARIA_SDL3=kit'
              if lib == SDL3_SO else '. Install the package that holds it' if lib else ''))
    loaded = r.stdout.strip()
    say(f'check: {len(sources)} runtime files and the libraries pass' +
        (f', and SDL3 {loaded} loads' if sdl and loaded else '') + f', {time.time() - t0:.0f} s')


def steps_to_run(state, identity, out, until=None):
    runs = []
    for name in STEPS[:STEPS.index('shaders') + 1]:
        if (any((earlier, name) not in UNAFFECTED for earlier in runs)
                or state['steps'].get(name) != step_key(name, identity, out)):
            runs.append(name)
        if name == until:
            break
    return runs


GiB = 1 << 30
SPACE_BUILD_CACHE, SPACE_REBUILD, SPACE_SHADERS = 2 * GiB, 1 * GiB, 4 * GiB
SPACE_IMAGE_MARGIN = 1 * GiB


def space_needs(runs, out, image_bytes=0, carried=False):
    needs = []
    if 'romfs' in runs:
        needs.append(('the RomFS image', image_bytes + SPACE_IMAGE_MARGIN))
    if 'compile' in runs:
        lifted = (out / 'build-cache' / 'lift' / 'layout.json').is_file()
        needs.append(('the build cache (the translated code and its compiled files)',
                      SPACE_REBUILD if lifted else SPACE_BUILD_CACHE))
    if 'shaders' in runs and not carried and not shaders_begun(out):
        needs.append(('the prepared shaders', SPACE_SHADERS))
    return needs


def shaders_begun(out):
    cache = out / 'shader-cache'
    if (cache / 'shader-cache.pack').is_file():
        return True
    try:
        with os.scandir(cache) as it:
            return any(e.name.endswith('.spv') for e in it)
    except OSError:
        return False


def check_space(out, needs):
    need = sum(n for _, n in needs)
    if not need:
        return
    free = shutil.disk_usage(out).free
    try:
        free += (out / 'romfs' / 'romfs.bin.part').stat().st_size
    except OSError:
        pass
    if free >= need:
        return
    parts = ', '.join(f'{n / 1e9:.1f} GB for {what}' for what, n in needs)
    fail(f'{out} has {free / 1e9:.1f} GB free and this build needs {need / 1e9:.1f} GB there: {parts}. Free '
         f'{(need - free) / 1e9:.1f} GB more on that disk, or build into a folder on another one')


def probe_toolchain(msys, work):
    clang, clangxx, env, _ = toolchain(msys)
    repair = (f'If the launcher installed these tools, delete the folder {msys.parent} and try again: it unpacks '
              f'them anew. If you installed MSYS2 yourself, install the packages README-BuildKit.md lists')
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    try:
        src, obj, exe = work / 'probe.c', work / 'probe.o', work / ('probe.exe' if WIN else 'probe')
        src.write_text('int main(void) { return 0; }\n', encoding='ascii')
        ld = ['-fuse-ld=lld'] if (msys / 'bin' / 'ld.lld.exe').is_file() else []
        steps = (('compile', clang, [str(clang), '-c', '-std=gnu11', '-O1', '-o', str(obj), str(src)]),
                 ('link', clangxx, [str(clangxx), *ld, '-o', str(exe), str(obj), '-L', str(msys / 'lib'), *LINK_LIBS]))
        for what, tool, cmd in steps:
            try:
                p = subprocess.run(cmd, capture_output=True, text=True, env=env, errors='replace')
            except OSError as e:
                fail(f'{tool} does not start ({error_text(e)}): the build tools are damaged, or security software '
                     f'blocks them. {repair}')
            if not p.returncode:
                continue
            said = (p.stderr.strip().splitlines() or [f'exit code {p.returncode}'])[-1][-300:]
            if what == 'compile':
                fail(f'{tool} cannot compile a test program ({said}): the build tools are incomplete, or security '
                     f'software blocks them. {repair}')
            lacking = sorted(set(re.findall(r'unable to find library (-l\S+)', p.stderr)))
            fail('the build tools cannot link the game: '
                 + (f'{msys / "lib"} lacks {", ".join(lacking)}' if lacking else f'the linker says: {said}')
                 + f'. {repair}')
    finally:
        shutil.rmtree(work, ignore_errors=True)
    say('check: the build tools compile and link a test program')


def runtime_sources():
    rt = KIT / 'runtime'
    out = []
    for pat in RUNTIME_PATTERNS:
        for p in sorted(rt.glob(pat)):
            if (p.stem.endswith('_sdl') or p.name == 'linux.c') if WIN else (p.stem.endswith('_win') or p.name == 'win32.c'):
                continue
            out.append((p, 'rt_' + p.relative_to(rt).as_posix().replace('/', '_').rsplit('.', 1)[0] + '.o'))
    return out


def runtime_includes(rt):
    return [a for d in RUNTIME_DIRS for a in ('-I', str(rt / d))]


def per_install_names(generated=GENERATED_HEADERS):
    rt = KIT / 'runtime'
    headers = {}
    for p in list(rt.rglob('*.h')) + list(rt.rglob('*.inc')):
        headers[p.name] = {os.path.basename(i) for i in INCLUDE.findall(p.read_text(encoding='utf-8', errors='replace'))}
    tainted = set(generated)
    grew = True
    while grew:
        grew = False
        for name, incs in headers.items():
            if name not in tainted and incs & tainted:
                tainted.add(name)
                grew = True
    return tainted


def per_install(src, tainted):
    incs = {os.path.basename(i) for i in INCLUDE.findall(Path(src).read_text(encoding='utf-8', errors='replace'))}
    return bool(incs & tainted)


def clang_version(clang, env):
    p = subprocess.run([str(clang), '--version'], capture_output=True, text=True, env=env, errors='replace')
    return (p.stdout.splitlines() or [''])[0].strip()


def runtime_digest():
    return tree_digest(KIT / 'runtime', ('**/*',))


CPU_V2 = {'__SSE3__', '__SSSE3__', '__SSE4_1__', '__SSE4_2__', '__POPCNT__', '__LAHF_SAHF__',
          '__GCC_HAVE_SYNC_COMPARE_AND_SWAP_16'}
CPU_V3 = CPU_V2 | {'__AVX__', '__AVX2__', '__BMI__', '__BMI2__', '__F16C__', '__FMA__', '__LZCNT__', '__MOVBE__',
                   '__XSAVE__'}


def cpu_level(clang, env):
    probe_env = dict(env, NIX_ENFORCE_NO_NATIVE='0')
    for name in env:
        if name.startswith('NIX_ENFORCE_NO_NATIVE_'):
            probe_env[name] = '0'
    p = subprocess.run([str(clang), '-march=native', '-E', '-dM', '-x', 'c', os.devnull], capture_output=True,
                       text=True, env=probe_env, errors='replace')
    have = set(re.findall(r'^#define (__\w+) ', p.stdout, re.M)) if p.returncode == 0 else set()
    level = 3 if CPU_V3 <= have else 2 if CPU_V2 <= have else 0
    if level:
        return level
    flags = cpu_flags()
    have = {macro for flag, macro in CPUINFO_MACROS.items() if flags and flag in flags}
    return 3 if CPU_V3 <= have else 2 if CPU_V2 <= have else 0


def clang_ident(clang, env):
    try:
        p = subprocess.run([str(clang), '--version'], capture_output=True, text=True, env=env, errors='replace')
    except OSError:
        return None
    v, t = re.search(r'clang version (\S+)', p.stdout), re.search(r'^Target: (\S+)', p.stdout, re.M)
    return (v.group(1), t.group(1)) if p.returncode == 0 and v and t else None


def fast_clang(msys, clang, env):
    if not WIN:
        return None
    fast = Path(msys).parent / 'clang64' / 'bin' / 'clang.exe'
    if not fast.is_file():
        return None
    mine = clang_ident(clang, env)
    return fast if mine and clang_ident(fast, env) == mine else None


PGO_NAME = 'pgo-v111.profdata'
PGO_RAW = 'pgo'
PGO_TRAINING = 'training.json'
PGO_USE = '-fprofile-use='
PGO_NAMES = ['-mllvm', '-static-func-full-module-prefix=false']
PGO_MODE = ''


def setting_env(name, default=''):
    current = name if name.startswith('NX2SARIA_') else name.replace('NX2NAVI_', 'NX2SARIA_', 1)
    legacy = current.replace('NX2SARIA_', 'NX2NAVI_', 1)
    return os.environ[current] if current in os.environ else os.environ.get(legacy, default)


def pgo_mode():
    mode = (PGO_MODE or setting_env('NX2SARIA_PGO')).strip().lower()
    return mode if mode in ('gen', 'off') else ''


def pgo_file():
    for d in (KIT / 'data', KIT / 'build' / 'pgo'):
        if (d / PGO_NAME).is_file():
            return d / PGO_NAME
    return None


PGO_IDS = {}


def pgo_id(profile):
    try:
        st = os.stat(profile)
        at = (str(profile), st.st_size, st.st_mtime_ns)
        if at not in PGO_IDS:
            h = hashlib.sha256(sha256_file(profile).encode())
            try:
                h.update(profile.with_suffix('.json').read_bytes())
            except OSError:
                pass
            PGO_IDS[at] = h.hexdigest()
        return PGO_IDS[at]
    except OSError:
        return 'unreadable'


def pgo_tag():
    mode, profile = pgo_mode(), pgo_file()
    if mode == 'gen':
        return '|pgo gen'
    if not profile:
        return ''
    return '|pgo off' if mode == 'off' else f'|pgo {pgo_id(profile)}'


def pgo_record(profile):
    rec = profile.with_suffix('.json')
    try:
        trained = json.loads(rec.read_text(encoding='utf-8'))['recomp_digest']
    except OSError:
        return f'its record {rec.name} is missing or cannot be read'
    except (ValueError, KeyError, TypeError):
        return f'its record {rec.name} does not name the recomp/ it was trained on'
    return '' if trained == recomp_digest() else 'it was trained on another recomp/ than this kit\'s'


def pgo_use_flags(profile):
    return [PGO_USE + str(profile), *PGO_NAMES]


def pgo_gen_flags(out):
    return [f'-fprofile-generate={(Path(out) / PGO_RAW).as_posix()}', *PGO_NAMES, '-DNX2SARIA_PGO_GEN']


def pgo_trial(profile, compilers, env, work):
    try:
        work.mkdir(parents=True, exist_ok=True)
        src, obj = work / 'trial.c', work / 'trial.o'
        src.write_text('int nx2navi_pgo_trial(int x) { return x + 1; }\n', encoding='ascii')
        for cc in compilers:
            cmd = [*map(str, cc), '-c', '-std=gnu11', '-O2', '-w', *pgo_use_flags(profile), '-o', str(obj), str(src)]
            try:
                p = subprocess.run(cmd, capture_output=True, text=True, env=env, errors='replace')
            except OSError as e:
                return f'{cc[0]} does not start: {error_text(e)}'
            if p.returncode:
                return (p.stderr.strip().splitlines() or [f'exit code {p.returncode}'])[-1][-300:]
        return ''
    except OSError as e:
        return error_text(e)
    finally:
        shutil.rmtree(work, ignore_errors=True)


def pgo_profile(clang, env, work, fast=None, msys=None):
    profile = pgo_file()
    if not profile:
        return None, 'this kit has no execution profile'
    if pgo_mode() == 'off':
        return None, f'the execution profile {profile.name} is left out (NX2SARIA_PGO=off)'
    if not WIN:
        return None, f'the execution profile {profile.name} is for the Windows build'
    why = pgo_record(profile)
    if not why:
        said = pgo_trial(profile, [[clang]] + ([[fast, f'--sysroot={msys}']] if fast else []), env, work)
        why = said and f'the compiler cannot read it ({said})'
    return (None, f'the execution profile {profile.name} is not used: {why}') if why else (profile, '')


def pgo_salt(cid, profile):
    return sha256_bytes(f'{cid}|pgo {pgo_id(profile)}'.encode()) if profile else cid


def pgo_gen_check(clangxx, env, work, msys, ld):
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    try:
        src = work / 'trial.c'
        src.write_text('int main(void) { return 0; }\n', encoding='ascii')
        p = subprocess.run([str(clangxx), *ld, '-fprofile-generate', '-o', str(work / ('trial.exe' if WIN else 'trial')),
                            '-x', 'c', str(src)], capture_output=True, text=True, env=env, errors='replace')
    finally:
        shutil.rmtree(work, ignore_errors=True)
    if p.returncode:
        said = (p.stderr.strip().splitlines() or [f'exit code {p.returncode}'])[0][-300:]
        fail('an instrumented build (NX2SARIA_PGO=gen) needs the compiler\'s profile runtime, and these build tools do '
             f'not link it ({said}). ' +
             (f'Unpack the MSYS2 package mingw-w64-ucrt-x86_64-compiler-rt of this clang\'s version into {msys.parent}'
              if WIN else 'Install the compiler-rt package of this clang\'s version'))


def pgo_gen_record(out, clang, env):
    raw = Path(out) / PGO_RAW
    raw.mkdir(parents=True, exist_ok=True)
    record = {'version': 'v111', 'recomp_digest': recomp_digest(), 'compiler': clang_version(clang, env),
              'kit': kit_version()}
    (raw / PGO_TRAINING).write_text(json.dumps(record, indent=1) + '\n', encoding='utf-8')


SWITCH_OFF = ('0', 'off', 'false', 'no')


def guest_opt_ini(out):
    if setting_env('NX2SARIA_GUEST_OPT').strip():
        return
    found = ''
    try:
        ini = Path(out) / 'nx2saria.ini'
        if not ini.is_file(): ini = Path(out) / 'nx2navi.ini'
        for line in ini.read_text(encoding='utf-8', errors='replace').splitlines():
            key, eq, value = line.partition('=')
            key = key.strip().lower()
            value = value.split('#')[0].split(';')[0].strip().strip('"')
            if eq and key in ('guest_opt', 'nx2saria_guest_opt', 'nx2navi_guest_opt') and value:
                found = value
    except OSError:
        pass
    if found:
        os.environ['NX2SARIA_GUEST_OPT'] = found


def runtime_cmd(clang, clangxx, inc, level=0, fast=None, msys=None, first=(), pgo=(), tsan=False):
    guest_opt = '-O2' if pgo or setting_env('NX2SARIA_GUEST_OPT').strip() == '2' else '-O1'
    def cmd_for(src, dst):
        cpp = src.suffix in ('.cpp', '.cxx')
        guest = dst.name.startswith(('main_', 'sdk_', 'subsdk0_'))
        march = [f'-march=x86-64-v{level}'] if guest and level else []
        cc = [str(fast), f'--sysroot={msys}'] if guest and fast else [str(clangxx if cpp else clang)]
        declspec = ['-fdeclspec'] if cpp and not WIN else []
        return [*cc, '-c', '-std=gnu++17' if cpp else '-std=gnu11', guest_opt if guest else '-O2',
                *march, '-fno-strict-aliasing', '-w', *declspec, *pgo,
                *(['-fsanitize=thread', '-g', '-fno-omit-frame-pointer'] if tsan and not guest else []),
                *([] if guest else first), *inc, '-o', str(dst),
                str(src)]
    return cmd_for


def make_runtime_lib(dest, msys, jobs, obj=None):
    dest = Path(dest).resolve()
    dest.mkdir(parents=True, exist_ok=True)
    obj = Path(obj).resolve() if obj else dest / 'obj'
    obj.mkdir(parents=True, exist_ok=True)
    clang, clangxx, env, vma = toolchain(msys)
    rt = KIT / 'runtime'
    tainted = per_install_names()
    srcs = [(p, obj / name) for p, name in runtime_sources() if not per_install(p, tainted)]
    inc = ['-I', str(rt), *runtime_includes(rt), '-I', str(rt / 'third_party' / 'imgui'), '-I', str(vma)]
    cache = ObjectCache(obj / 'objects.json', {'<kit>': KIT})
    if pgo_mode() == 'gen':
        fail('the prebuilt runtime is never an instrumented one: an instrumented build (NX2SARIA_PGO=gen) compiles '
             'runtime/ itself')
    profile, why = pgo_profile(clang, env, obj / 'pgo-trial')
    say(f'  the runtime is compiled with the execution profile {profile.name}' if profile else f'  {why}')
    t0 = time.time()
    gate = make_gate(jobs)
    try:
        compiled, reused, bad = compile_all(srcs, runtime_cmd(clang, clangxx, inc, first=vulkan_headers(clang, env),
                                                              pgo=pgo_use_flags(profile) if profile else ()),
                                            cache, jobs, env,
                                            lambda d, t: (d % 20 == 0 or d == t) and say(f'  {d} of {t}'),
                                            salt=pgo_salt(compiler_id(clang, env), profile), gate=gate)
    finally:
        gate.close()
    for line in gate.summary_lines():
        say(f'  {line}')
    if bad:
        report_failures(bad)
        fail(f'{len(bad)} runtime files did not compile')
    lib = dest / RUNTIME_LIB
    lib.unlink(missing_ok=True)
    rsp = dest / 'ar.rsp'
    rsp.write_text('\n'.join('"%s"' % o.as_posix() for _, o in srcs), encoding='utf-8')
    ar = setting_env('NX2SARIA_AR') or (str(msys / 'bin' / 'ar.exe') if WIN else shutil.which('ar'))
    if not ar:
        fail('ar is not on PATH: install binutils or set NX2SARIA_AR (NX2NAVI_AR remains an alias)')
    if subprocess.run([ar, 'rcs', str(lib), '@' + str(rsp)], env=env).returncode:
        fail('the runtime library could not be archived')
    meta = {'runtime': runtime_digest(), 'compiler': clang_version(clang, env),
            'objects': sorted(o.name for _, o in srcs), 'pgo': pgo_id(profile) if profile else ''}
    (dest / 'runtime-lib.json').write_text(json.dumps(meta, indent=1), encoding='utf-8')
    rsp.unlink(missing_ok=True)
    say(f'{lib}: {len(srcs)} runtime files ({compiled} compiled, {reused} kept), {lib.stat().st_size / 1e6:.1f} MB, '
        f'{time.time() - t0:.0f} s')


def prebuilt_runtime(clang, env):
    lib, meta = KIT / 'lib' / RUNTIME_LIB, KIT / 'lib' / 'runtime-lib.json'
    try:
        m = json.loads(meta.read_text(encoding='utf-8'))
    except (OSError, ValueError):
        return None
    if not lib.is_file() or m.get('runtime') != runtime_digest() or m.get('compiler') != clang_version(clang, env):
        say('  the prebuilt runtime does not match this kit or compiler: compiling runtime/')
        return None
    return lib, set(m.get('objects', []))


def select_prebuilt_runtime(clang, env, gen_counts, tsan):
    return None if gen_counts or tsan else prebuilt_runtime(clang, env)


def allow_legacy_object(tsan, dst):
    return not tsan or dst.name.startswith(('main_', 'sdk_', 'subsdk0_'))


def romfs_index(out, gen):
    if subprocess.run(py_cmd(KIT / 'tools' / 'romfs_index.py', '--romfs', out / 'romfs', '--out', gen)).returncode:
        fail('the RomFS index could not be written')


def windres_command(msys, rco):
    pp = ('-E', '-xc', '-DRC_INVOKED')
    return [str(msys / 'bin' / 'windres.exe'), '--use-temp-file', f'--preprocessor={msys / "bin" / "gcc.exe"}',
            *(f'--preprocessor-arg={a}' for a in pp), '-O', 'coff', '-i', 'nx2saria.rc', '-o', str(rco)]


RC_LINE = re.compile(r'(\d+)\s+(24|ICON)\s+"([^"]+)"')


def resource_file(rt):
    def resource(typ, name, data, flags):
        head = struct.pack('<HHHHIHHII', 0xffff, typ, 0xffff, name, 0, flags, 0x0409, 0, 0)
        return struct.pack('<II', len(data), 8 + len(head)) + head + data + b'\0' * (-len(data) % 4)

    out = bytearray(struct.pack('<II', 0, 32) + struct.pack('<HHHHIHHII', 0xffff, 0, 0xffff, 0, 0, 0, 0, 0, 0))
    for line in (rt / 'nx2saria.rc').read_text(encoding='utf-8').splitlines():
        m = RC_LINE.fullmatch(line.strip())
        if not m:
            continue
        name, kind, data = int(m.group(1)), m.group(2), (rt / m.group(3)).read_bytes()
        if kind == '24':
            out += resource(24, name, data, 0x1030)
            continue
        reserved, ico_type, n = struct.unpack_from('<HHH', data, 0)
        if reserved or ico_type != 1 or not n:
            fail(f'runtime/{m.group(3)} is not an icon file')
        group = struct.pack('<HHH', 0, 1, n)
        for i in range(n):
            w, h, colors, res0, planes, bits, size, off = struct.unpack_from('<BBBBHHII', data, 6 + 16 * i)
            out += resource(3, i + 1, data[off:off + size], 0x1010)
            group += struct.pack('<BBBBHHIH', w, h, colors, res0, planes, bits, size, i + 1)
        out += resource(14, name, group, 0x1030)
    return bytes(out)


def compile_resources(msys, rt, obj, env, lld=False):
    if lld:
        res = obj / 'nx2navi.res'
        data = resource_file(rt)
        if not res.is_file() or res.read_bytes() != data:
            res.write_bytes(data)
        return res
    rco, new = obj / 'nx2navi_rc.o', obj / 'nx2navi_rc.o.new'
    tmp = obj / 'windres-tmp'
    shutil.rmtree(tmp, ignore_errors=True)
    tmp.mkdir()
    try:
        r = subprocess.run(windres_command(msys, new), cwd=rt, env=dict(env, TMP=str(tmp), TEMP=str(tmp)),
                           capture_output=True, text=True, errors='replace')
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    if r.returncode:
        new.unlink(missing_ok=True)
        fail(f'windres failed on runtime/nx2saria.rc: {r.stderr.strip()[:400]}')
    if rco.is_file() and rco.read_bytes() == new.read_bytes():
        new.unlink()
    else:
        os.replace(new, rco)
    return rco


def toolchain(msys):
    if WIN:
        return (msys / 'bin' / 'clang.exe', msys / 'bin' / 'clang++.exe',
                dict(os.environ, PATH=str(msys / 'bin') + os.pathsep + os.environ.get('PATH', '')), msys / 'include' / 'vma')
    for tool in ('clang', 'clang++', 'ld.lld'):
        if not shutil.which(tool):
            fail(f'{tool} is not on PATH: install the packages README-BuildKit.md lists under "Linux"')
    if os.environ.get('VMA_INCLUDE'):
        vma = Path(os.environ['VMA_INCLUDE'])
        if not (vma / 'vk_mem_alloc.h').is_file():
            fail(f'{vma / "vk_mem_alloc.h"} is missing: correct VMA_INCLUDE')
    else:
        from build_game import vma_include
        vma = vma_include()
    return Path(shutil.which('clang')), Path(shutil.which('clang++')), dict(os.environ), vma


def vulkan_headers(clang, env):
    if WIN:
        return []
    try:
        from build_game import vulkan_include
        folder = vulkan_include(clang, env)
    except ImportError:
        return []
    return ['-I', str(folder)] if folder else []


SDL3_SO = 'libSDL3.so.0'
SDL3_PLACED = 'sdl3-beside.txt'
SDL3_NEEDS = {'libX11.so.6': 'libx11-6', 'libXext.so.6': 'libxext6', 'libXcursor.so.1': 'libxcursor1',
              'libXi.so.6': 'libxi6', 'libXfixes.so.3': 'libxfixes3', 'libXrandr.so.2': 'libxrandr2',
              'libXss.so.1': 'libxss1', 'libXtst.so.6': 'libxtst6', 'libxkbcommon.so.0': 'libxkbcommon0',
              'libwayland-client.so.0': 'libwayland-client0', 'libwayland-cursor.so.0': 'libwayland-cursor0',
              'libwayland-egl.so.1': 'libwayland-egl1', 'libdecor-0.so.0': 'libdecor-0-0', 'libpulse.so.0': 'libpulse0',
              'libasound.so.2': 'libasound2t64', 'libfribidi.so.0': 'libfribidi0'}


SDL3_PROBE = ('#include <stdio.h>\n#include <SDL3/SDL.h>\nint main(void) {\n    int v = SDL_GetVersion();\n'
              '    printf("%d.%d.%d\\n", SDL_VERSIONNUM_MAJOR(v), SDL_VERSIONNUM_MINOR(v), SDL_VERSIONNUM_MICRO(v));\n'
              '    return 0;\n}\n')


class Sdl3(NamedTuple):
    source: str
    version: str
    include: list
    lib: str
    why: str


def sdl3_version(clang, env, inc=()):
    try:
        p = subprocess.run([str(clang), *inc, '-E', '-dM', '-x', 'c', '-'], input='#include <SDL3/SDL_version.h>\n',
                           capture_output=True, text=True, env=env, errors='replace')
    except OSError:
        return ''
    if p.returncode:
        return ''
    parts = [re.search(rf'^#define SDL_{k}_VERSION (\d+)', p.stdout, re.M) for k in ('MAJOR', 'MINOR', 'MICRO')]
    return '.'.join(m.group(1) for m in parts) if all(parts) else '?'


def kit_sdl3():
    try:
        import sdl3_pin
    except ImportError:
        fail('this kit carries no SDL3 of its own (tools/sdl3_pin.py): install SDL3\'s development package')
    folder = KIT / sdl3_pin.FOLDER
    if sdl3_pin.have(folder):
        return folder
    if not os.access(KIT, os.W_OK):
        fail(f'this kit carries no SDL3 ({folder}) and its folder is read-only: install SDL3\'s development package, '
             f'or use a newer build kit')
    try:
        return sdl3_pin.resolve(folder)
    except SystemExit as e:
        fail(str(e.code))
    except OSError as e:
        fail(f'SDL3 cannot be put into {folder}: {error_text(e)}')


def sdl3_choice(clang, env):
    if WIN:
        return None
    want = setting_env('NX2SARIA_SDL3').strip().lower()
    if want not in ('', 'system', 'kit'):
        fail(f'NX2SARIA_SDL3 is "{want}": it takes system or kit, or nothing for the system\'s SDL3 when the compiler '
             f'finds it')
    system = '' if want == 'kit' else sdl3_version(clang, env)
    if want == 'system' and not system:
        fail('NX2SARIA_SDL3=system, but the compiler finds no SDL3/SDL_version.h: install SDL3\'s development package, '
             'or leave NX2SARIA_SDL3 unset to build with the kit\'s SDL3')
    if system:
        return Sdl3('system', system, [], '-lSDL3', 'NX2SARIA_SDL3=system' if want else
                    'the compiler finds its headers; NX2SARIA_SDL3=kit takes the kit\'s')
    folder = kit_sdl3()
    inc = ['-I', str(folder / 'include')]
    return Sdl3('kit', sdl3_version(clang, env, inc) or '?', inc, str(folder / 'lib' / SDL3_SO),
                f'{folder}: ' + ('NX2SARIA_SDL3=kit' if want else 'the compiler finds no SDL3 headers of the system\'s'))


def link_libs(sdl):
    return [sdl.lib if lib == '-lSDL3' else lib for lib in LINK_LIBS] if sdl else LINK_LIBS


def link_key_libs(sdl):
    if not sdl or sdl.source != 'kit':
        return link_libs(sdl)
    lib = Path(sdl.lib)
    try:
        where = '<kit>/' + lib.relative_to(KIT).as_posix()
    except ValueError:
        where = lib.as_posix()
    return [f'{where}@{sha256_file(lib)}' if x == sdl.lib else x for x in link_libs(sdl)]


def sdl3_tag():
    want = setting_env('NX2SARIA_SDL3').strip().lower()
    return f'|sdl3 {want}' if want and not WIN else ''


def sdl3_line(clang, version, sdl):
    line = f'the compiler is {version} ({clang})'
    return line + (f'; SDL3 {sdl.version} from the {sdl.source} ({sdl.why})' if sdl else '')


def missing_library(stderr):
    m = re.search(r'error while loading shared libraries: ([^:\s]+): cannot open shared object file', stderr)
    return m.group(1) if m else ''


def place_sdl3(out, cache_dir, sdl):
    if not sdl:
        return ''
    dst, record = out / SDL3_SO, cache_dir / SDL3_PLACED
    if sdl.source == 'kit':
        copy_if_changed(sdl.lib, dst)
        record.write_text(sha256_file(dst) + '\n', encoding='ascii')
        return f'SDL3 {sdl.version} from the kit: {SDL3_SO} beside {EXE_NAME}'
    if not dst.is_file():
        record.unlink(missing_ok=True)
        return f'SDL3 {sdl.version} from the system'
    try:
        placed = record.read_text(encoding='ascii').strip()
    except (OSError, ValueError):
        placed = ''
    kit_lib = KIT / 'toolchain' / 'sdl3' / 'lib' / SDL3_SO
    beside = sha256_file(dst)
    if beside == placed or (kit_lib.is_file() and beside == sha256_file(kit_lib)):
        dst.unlink()
        record.unlink(missing_ok=True)
        return f'SDL3 {sdl.version} from the system: the kit\'s {SDL3_SO} an earlier build put beside {EXE_NAME} is removed'
    return (f'SDL3 {sdl.version} from the system, but the {SDL3_SO} beside {EXE_NAME}, which this build did not put '
            f'there, is the one the game loads (the game log\'s libs: line names it)')


def xlate_source():
    for d in (KIT / 'bin', KIT / 'build' / 'xlate'):
        if (d / XLATE_NAME).is_file():
            return d
    fail(f'{XLATE_NAME} is missing: build it with tools/build_xlate.py')


def xlate_id():
    for d in (KIT / 'bin', KIT / 'build' / 'xlate'):
        if (d / XLATE_NAME).is_file():
            return sha256_file(d / 'nx2navi_xlate.id')
    return 'missing'


LIFTED_BUILD = 'lifted-build.json'
LIFTED_OBJECTS = ('main_', 'sdk_', 'subsdk0_')


def lifted_level(obj, now, update):
    if not update:
        return now
    try:
        rec = json.loads((obj / LIFTED_BUILD).read_text(encoding='utf-8'))
        return min(int(rec['level']), now)
    except (OSError, ValueError, KeyError, TypeError):
        pass
    try:
        entries = json.loads((obj / 'objects.json').read_text(encoding='utf-8'))
        old = any(isinstance(rec, str) for name, e in entries.items() if name.startswith(LIFTED_OBJECTS)
                  for rec in e.get('deps', {}).values())
    except (OSError, ValueError, AttributeError):
        old = False
    return 0 if old else now


def retune_due(obj, msys):
    clang, _, env, _ = toolchain(msys)
    try:
        return int(json.loads((obj / LIFTED_BUILD).read_text(encoding='utf-8'))['level']) != cpu_level(clang, env)
    except (OSError, ValueError, KeyError, TypeError):
        return True


def write_runtime_map(full, dst):
    lifted = re.compile(r'\bf_[0-9a-f]{4,}\b')
    try:
        with open(full, encoding='utf-8', errors='replace') as i, open(dst, 'w', encoding='utf-8') as o:
            for line in i:
                if not lifted.search(line):
                    o.write(line)
    except OSError:
        pass


COMPILE_MEMORY = 'compile-memory.json'
LARGEST_SHOWN = 5


def chunk_costs(lift):
    costs = {}
    for mod in ('main', 'sdk', 'subsdk0'):
        try:
            manifest = json.loads((lift / mod / 'manifest.json').read_text(encoding='utf-8'))
            entries = manifest.get('chunk_info') or []
        except (OSError, ValueError, AttributeError):
            entries = []
        info = {e['file']: e for e in entries if isinstance(e, dict) and isinstance(e.get('file'), str)}
        for p in sorted((lift / mod).glob('*.c')):
            e = info.get(p.name, {})
            cost, switch_fn = e.get('cost'), e.get('switch_fn', 0)
            if not isinstance(cost, int) or cost <= 0:
                cost, switch_fn = e.get('bytes'), 0
            if not isinstance(cost, int) or cost <= 0:
                cost, switch_fn = source_size(p), 0
            costs[p] = (cost, switch_fn if isinstance(switch_fn, int) and switch_fn > 0 else 0)
    return costs


def gate_start_line(gate, hows):
    if not gate.enabled:
        return f'memory gate: off; up to {gate.max_jobs} at once'
    ceiling = 'no ceiling' if gate.ceiling is None else f'ceiling {memgate.gib(gate.ceiling)} GiB'
    line = (f'memory gate: on; up to {gate.max_jobs} at once, at least {gate.min_jobs}; keeps '
            f'{memgate.gib(gate.reserve)} GiB free; {ceiling}; estimates: {hows["learned"]} learned from earlier '
            f'builds, {hows["model"]} from the size model')
    if hows['default']:
        line += f', {hows["default"]} set at {DEFAULT_NEED_C >> 20} MiB (C) or {DEFAULT_NEED_CXX >> 30} GiB (C++) ' \
                'until measured'
    return line


def compile_memory_lines(gate, estimates, measured):
    lines = gate.summary_lines()
    s = gate.summary()
    if s['enabled']:
        lines = lines[:2]
    parts = []
    if s['peak_rss_at'] is not None:
        parts.append(f'peak {memgate.gib(s["peak_rss"])} GiB for all compiles together (at {s["peak_rss_at"]:.0f} s)')
    if s['lowest_avail'] is not None:
        parts.append(f'least free memory {memgate.gib(s["lowest_avail"])} GiB')
    if parts:
        lines.append('compile memory: ' + ', '.join(parts))
    top = sorted(measured, key=lambda m: m[1], reverse=True)[:LARGEST_SHOWN]
    if top:
        lines.append(f'largest compiles ({len(top)} of {len(measured)}): ' + ', '.join(f'{name} {peak / memgate.GIB:.2f} GiB {seconds:.0f} s'
                                                      for name, peak, seconds in top))
    st = estimates.stats()
    if st['measured'] and st['median'] and st['worst']:
        lines.append(f'estimates against the measured peaks: median {1 / st["median"]:.2f}x, lowest '
                     f'{1 / st["worst"]:.2f}x ({st["worst_name"]})')
    return lines


def step_compile(out, cache_dir, msys, jobs, romfs_job=None, update=False):
    lift, gen, obj = cache_dir / 'lift', cache_dir / 'gen', cache_dir / 'obj'
    gen.mkdir(parents=True, exist_ok=True)
    obj.mkdir(parents=True, exist_ok=True)
    rt = KIT / 'runtime'
    runtime_headers(rt)
    clang, clangxx, env, vma = toolchain(msys)
    if not romfs_job:
        romfs_index(out, gen)
    srcs = [(p, obj / f'{mod}_{p.stem}.o') for mod in ('main', 'sdk', 'subsdk0') for p in sorted((lift / mod).glob('*.c'))]
    gen_counts = pgo_mode() == 'gen'
    ld = ['-fuse-ld=lld'] if ((msys / 'bin' / 'ld.lld.exe').is_file() if WIN else shutil.which('ld.lld')) else []
    if gen_counts:
        pgo_gen_check(clangxx, env, cache_dir / 'pgo-trial', msys, ld)
    tsan = os.environ.get('NX2SARIA_BENCH_TSAN') == '1'
    prebuilt = select_prebuilt_runtime(clang, env, gen_counts, tsan)
    runtime = [(p, obj / name) for p, name in runtime_sources()]
    if prebuilt:
        runtime = [(p, o) for p, o in runtime if o.name not in prebuilt[1]]
        say(f'  the runtime comes prebuilt; {len(runtime)} runtime files are compiled here')
    srcs += runtime
    srcs.append((modules_c(lift, out, gen), obj / 'modules.o'))
    if WIN:
        srcs.append((gen / 'shim.c', obj / 'shim.o'))
    inc = ['-I', str(rt), '-I', str(lift), '-I', str(gen), '-I', str(rt / 'third_party' / 'imgui'), '-I', str(vma)]
    cid, cid_old = compiler_ids(clang, env)
    now = cpu_level(clang, env)
    level = lifted_level(obj, now, update)
    guest_opt_ini(out)
    if setting_env('NX2SARIA_GUEST_OPT').strip() == '2':
        say('  lifted code at -O2 (NX2SARIA_GUEST_OPT): a long build')
    if level == now:
        say(f'  the lifted code is built for x86-64-v{level}, this processor\'s level' if level else
            '  the lifted code is built for generic x86-64: the processor\'s level could not be read')
    else:
        say(f'  the lifted code is built for {f"x86-64-v{level}" if level else "generic x86-64"}, as the objects kept '
            f'from before this update were (Rebuild the Game compiles it for this processor\'s level)')
    fast = fast_clang(msys, clang, env)
    if fast:
        say(f'  the lifted code compiles with {fast}')
    vulkan = vulkan_headers(clang, env)
    if vulkan:
        say(f'  the Vulkan headers come from {vulkan[1]}')
    sdl = sdl3_choice(clang, env)
    if sdl:
        say(f'  {sdl3_line(clang, clang_version(clang, env), sdl)}')
    first = vulkan + (sdl.include if sdl else [])
    first += runtime_includes(rt)
    profile = None
    if gen_counts:
        pgo = pgo_gen_flags(out)
        say(f'  an instrumented build (NX2SARIA_PGO=gen): the lifted code at -O2 and a slower game, which adds its '
            f'counts to {out / PGO_RAW} every minute and at exit')
    else:
        profile, why = pgo_profile(clang, env, cache_dir / 'pgo-trial', fast, msys)
        pgo = pgo_use_flags(profile) if profile else []
        say(f'  the lifted code (at -O2) and the runtime are built with the execution profile {profile.name}'
            if profile else f'  {why}')
    cmd_for = runtime_cmd(clang, clangxx, inc, level, fast, msys, first=first, pgo=pgo, tsan=tsan)
    plain = runtime_cmd(clang, clangxx, inc, level, None, msys)
    no_vulkan = runtime_cmd(clang, clangxx, inc, level, fast, msys)
    fast_old = (cid_old + compiler_ids(fast, env)[1]) if fast else None
    kits = earlier_kits()

    def alts(src, dst):
        if not allow_legacy_object(tsan, dst):
            return [], kits
        if pgo:
            return [], kits
        if sdl and sdl.include and not dst.name.startswith(('main_', 'sdk_', 'subsdk0_')):
            return [], kits
        keys = [object_key(plain(src, dst), src, dst, cache, cid_old, sysroot=True)]
        if vulkan:
            keys.append(object_key(no_vulkan(src, dst), src, dst, cache, cid))
        if fast:
            keys.append(object_key(cmd_for(src, dst), src, dst, cache, fast_old, sysroot=True))
        keys += [legacy_key(src, dst, cache, cid_old, clang, clangxx, k, lift, gen, vma) for k in kits]
        return keys, kits

    t0 = time.time()
    last = [0.0]

    def progress(done, total):
        now = time.time()
        if done == total or now - last[0] >= 0.25:
            last[0] = now
            emit('progress', step='compile', done=done, total=total, unit='files')
        if done % 50 == 0 or done == total:
            say(f'  compiled {done} of {total}, {now - t0:.0f} s')

    say(f'  {len(srcs)} files, up to {jobs} compiles at once')
    cache = ObjectCache(obj / 'objects.json', {'<kit>': KIT})
    estimates = memgate.Estimates.load(obj / COMPILE_MEMORY)
    costs = chunk_costs(lift)
    gate = make_gate(jobs)
    measured = []
    announced = []

    def plan(hows):
        if sum(hows.values()) and not announced:
            announced.append(True)
            say(f'  {gate_start_line(gate, hows)}')
    late = []
    if romfs_job:
        tainted = per_install_names(('romfs_config.h',))
        late = [(s, o) for s, o in runtime + srcs[-(2 if WIN else 1):] if per_install(s, tainted)]
        say(f'  {len(late)} files wait for the RomFS index: {", ".join(s.name for s, _ in late)}')
    early = [j for j in srcs if j not in late]
    salt = pgo_salt(cid, profile)
    gated = dict(gate=gate, estimates=estimates, costs=costs, measured=measured)
    try:
        compiled, reused, bad = compile_all(early, cmd_for, cache, jobs, env, lambda d, t: progress(d, len(srcs)),
                                            salt=salt, stop=romfs_job.failed if romfs_job else None, alts=alts,
                                            plan=plan, **gated)
        if romfs_job:
            if romfs_job.thread.is_alive():
                say('  waiting for the RomFS image')
            romfs_job.join()
            if not bad:
                romfs_index(out, gen)
                c2, r2, bad = compile_all(late, cmd_for, cache, jobs, env,
                                          lambda d, t: progress(len(early) + d, len(srcs)), salt=salt, alts=alts,
                                          plan=plan, **gated)
                compiled, reused = compiled + c2, reused + r2
    finally:
        gate.close()
    say(f'  {compiled} compiled, {reused} unchanged since the last build')
    if compiled:
        for line in compile_memory_lines(gate, estimates, measured):
            say(f'  {line}')
    if bad:
        report_failures(bad)
        fail(f'{len(bad)} files did not compile')
    if cache.keep_only({o.name for _, o in srcs}):
        cache.save()
    (obj / LIFTED_BUILD).write_text(json.dumps({'level': level, 'pgo': 'gen' if gen_counts else
                                                pgo_id(profile) if profile else ''}), encoding='utf-8')

    objs = [o for _, o in srcs] + ([compile_resources(msys, rt, obj, env, lld=(msys / 'bin' / 'ld.lld.exe').is_file())]
                                   if WIN else [])
    exe = out / EXE_NAME
    rsp = cache_dir / 'link.rsp'
    rsp.write_text('\n'.join('"%s"' % o.as_posix() for o in objs), encoding='utf-8')
    if WIN:
        ld = ld + ['-Wl,--stack,16777216']
    else:
        ld = ld + ['-no-pie', '-Wl,-z,noexecstack', '-Wl,-rpath,$ORIGIN']
    if gen_counts:
        ld.append('-fprofile-generate')
    if tsan:
        ld.append('-fsanitize=thread')
    ld += ['-Xlinker', '--Map=' + (cache_dir / 'ocarina.map').as_posix()]
    lib = ['-Wl,--whole-archive', str(prebuilt[0]), '-Wl,--no-whole-archive'] if prebuilt else []
    stamp = [[o.name, o.stat().st_size, o.stat().st_mtime_ns] for o in objs]
    if prebuilt:
        stamp.append([prebuilt[0].name, prebuilt[0].stat().st_size, sha256_file(prebuilt[0])])
    libs = link_libs(sdl)
    link_key = sha256_bytes(json.dumps([link_key_libs(sdl), ld, cid, 'map'] + stamp).encode())
    state_link = cache_dir / 'link.key'
    if exe.is_file() and state_link.is_file() and state_link.read_text(encoding='utf-8') == link_key:
        say(f'  {EXE_NAME} is current, not linked again')
    else:
        emit('phase', step='compile', phase='link')
        say('  linking')
        tmp = out / (EXE_NAME + '.new')
        lib_dir = ['-L', str(msys / 'lib')] if WIN else []
        p = subprocess.run([str(clangxx), *ld, '-o', str(tmp), '@' + str(rsp), *lib, *lib_dir, *libs],
                           capture_output=True, text=True, env=env, errors='replace')
        if p.returncode:
            print(p.stderr[-4000:], file=sys.stderr)
            tmp.unlink(missing_ok=True)
            fail('the link failed')
        try:
            os.replace(tmp, exe)
        except PermissionError:
            tmp.unlink(missing_ok=True)
            fail(f'{exe} is in use: close the game and try again')
        write_runtime_map(cache_dir / 'ocarina.map', out / 'nx2navi.map')
        state_link.write_text(link_key, encoding='utf-8')
    placed = place_sdl3(out, cache_dir, sdl)
    if placed:
        say(f'  {placed}')
    copy_if_changed(gen / 'romfs-index.bin', out / 'romfs-index.bin')
    xlate = xlate_source()
    note_cache_translator(out)
    for name in (XLATE_NAME, 'nx2navi_xlate.id'):
        copy_if_changed(xlate / name, out / name)
    if (KIT / 'bin' / 'amd_fidelityfx_vk.dll').is_file():
        copy_if_changed(KIT / 'bin' / 'amd_fidelityfx_vk.dll', out / 'amd_fidelityfx_vk.dll')
    if all((KIT / 'bin' / n).is_file() for n in FFX4_DLLS):
        for name in FFX4_DLLS:
            copy_if_changed(KIT / 'bin' / name, out / name)
        if (KIT / 'bin' / FFX4_FG_DLL).is_file():
            copy_if_changed(KIT / 'bin' / FFX4_FG_DLL, out / FFX4_FG_DLL)
    kept = place_dlss(out, linux=not WIN)
    if kept:
        say(f'  {kept}')
    if WIN and place_dlssg(out):
        say('  DLSS Frame Generation: nx2navi_dlssg.dll, Streamline and nvngx_dlssg.dll beside the game')
    beside = copy_dlls(exe, msys / 'bin', out) if WIN else []
    say(f'  {EXE_NAME} {exe.stat().st_size / 1e6:.0f} MB' + (f'; beside it: {", ".join(beside)}' if beside else ''))
    (out / 'profile').mkdir(exist_ok=True)
    if gen_counts:
        pgo_gen_record(out, clang, env)


SHADER_PROGRESS = re.compile(r'^prepare-shaders: progress (translate|build) (\d+) (\d+)\s*$')
SHADERS_LEFT_LINE = re.compile(r'^prepare-shaders: \d+ of \d+ variants built into the driver cache .*; (\d+) still building '
                               r'after \d+ s each are left to the game')
SHADERS_LEFT = {'n': 0}
SHADER_STALL_S = 180
SHADER_BEAT_S = 10


def shader_progress(phase, done, total, quiet_s, elapsed_s, build_only=False):
    clock = f'{int(elapsed_s) // 60}:{int(elapsed_s) % 60:02d}'
    if phase is None:
        fraction, detail = 0.0, f'Reading the game\'s shaders ({clock} so far)'
    elif phase == 'translate':
        fraction, detail = 0.5 * done / max(total, 1), f'Translated {done:,} of {total:,} shaders'
    else:
        part = done / max(total, 1)
        fraction = part if build_only else 0.5 + 0.5 * part
        detail = f'Built {done:,} of {total:,} shaders for your graphics card'
    if quiet_s >= SHADER_STALL_S:
        detail += (f'. No change for {int(quiet_s) // 60} min, but it is still running: one large shader can take '
                   'a while')
    return {'step': 'shaders', 'fraction': fraction, 'detail': detail}


SHADER_IDS = 'identities.txt'
HEX64 = re.compile(r'[0-9a-fA-F]{64}')


def cache_identities(out):
    try:
        text = (out / 'shader-cache' / SHADER_IDS).read_text(encoding='ascii', errors='replace')
    except OSError:
        return []
    return [line.strip().lower() for line in text.splitlines() if HEX64.fullmatch(line.strip())]


def note_cache_translator(out):
    cache, ids = out / 'shader-cache', out / 'shader-cache' / SHADER_IDS
    if not cache.is_dir() or ids.exists():
        return
    try:
        old = (out / 'nx2navi_xlate.id').read_text(encoding='ascii', errors='replace').strip()[:64]
    except OSError:
        return
    if not HEX64.fullmatch(old):
        return
    tmp = ids.with_name(SHADER_IDS + '.tmp')
    tmp.write_text('# Translator identities this shader cache holds translations of, newest first\n' + old.lower() + '\n',
                   encoding='ascii', newline='\n')
    os.replace(tmp, ids)


def ini_choice(raw):
    v = raw.strip()
    if not (len(v) >= 2 and v[0] == v[-1] == '"'):
        m = re.search(r'[ \t][;#]', raw)
        if m:
            v = raw[:m.start()].strip()
    return v[1:-1] if len(v) >= 2 and v[0] == v[-1] == '"' else v


def shader_carry(out):
    v = setting_env('NX2SARIA_SHADER_CARRY').strip()
    if not v:
        try:
            ini = out / 'nx2saria.ini'
            if not ini.is_file(): ini = out / 'nx2navi.ini'
            for line in ini.read_text(encoding='utf-8-sig', errors='replace').splitlines():
                k, eq, raw = line.strip().partition('=')
                if eq and k.strip().upper().removeprefix('NX2SARIA_').removeprefix('NX2NAVI_') == 'SHADER_CARRY' and ini_choice(raw):
                    v = ini_choice(raw)
        except OSError:
            pass
    if v.lower() in SWITCH_OFF:
        return False
    return (out / 'shader-prepared.bin').is_file() and bool(cache_identities(out))


PREPARED_HEAD = struct.Struct('<8sII')
SEED_FILES = ('seed-programs.bin', 'seed-answers.bin')


def prepared_from(out):
    try:
        with open(out / 'shader-prepared.bin', 'rb') as f:
            data = f.read(PREPARED_HEAD.size + 96)
    except OSError:
        return None
    if len(data) < PREPARED_HEAD.size:
        return {}
    magic, version, _rec = PREPARED_HEAD.unpack_from(data)
    if magic != b'NX2NSPRP' or version != 2 or len(data) < PREPARED_HEAD.size + 96:
        return {}
    body = data[PREPARED_HEAD.size:]
    return {'programs': body[:32].hex(), 'answers': body[32:64].hex(), 'translator': body[64:96].hex()}


def seed_changed(out):
    was = prepared_from(out)
    if not was:
        return 'the shaders prepared before do not say which shader seed they were prepared from'
    try:
        now = {'programs': sha256_file(out / SEED_FILES[0]), 'answers': sha256_file(out / SEED_FILES[1])}
    except OSError:
        return ''
    if (was['programs'], was['answers']) != (now['programs'], now['answers']):
        return 'the shader seed changed since the shaders were prepared'
    return ''


def shader_run(cmd, cwd=None, build_only=False):
    proc = subprocess.Popen(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                            encoding='utf-8', errors='replace', bufsize=1)
    t0 = time.monotonic()
    state = {'phase': None, 'done': 0, 'total': 0, 'moved': t0}
    lock = threading.Lock()
    stop = threading.Event()

    def send():
        now = time.monotonic()
        emit('progress', **shader_progress(state['phase'], state['done'], state['total'], now - state['moved'],
                                           now - t0, build_only))

    def heartbeat():
        while not stop.wait(SHADER_BEAT_S):
            with lock:
                send()

    beat = threading.Thread(target=heartbeat, daemon=True)
    beat.start()
    last_log = 0.0
    SHADERS_LEFT['n'] = 0
    try:
        for line in proc.stdout:
            m = SHADER_PROGRESS.match(line)
            if not m:
                left = SHADERS_LEFT_LINE.match(line)
                if left:
                    SHADERS_LEFT['n'] = int(left.group(1))
                say(line.rstrip('\n'))
                continue
            phase, done, total = m.group(1), int(m.group(2)), int(m.group(3))
            with lock:
                if (phase, done) != (state['phase'], state['done']):
                    state['moved'] = time.monotonic()
                state.update(phase=phase, done=done, total=total)
                send()
            if time.monotonic() - last_log >= 30 or done == total:
                last_log = time.monotonic()
                say(f'  shaders: {"translated" if phase == "translate" else "built"} {done} of {total}, '
                    f'{time.monotonic() - t0:.0f} s')
    finally:
        rc = proc.wait()
        proc.stdout.close()
        stop.set()
        beat.join()
    return rc


def refresh_pipeline_seed(out):
    lst = KIT / 'tools' / 'data' / 'pipeline_list.bin'
    if not lst.is_file():
        return
    import pipeline_list
    got = pipeline_list.unpack(lst.read_bytes())
    if got:
        (out / 'seed-pipeline-keys.bin').write_bytes(got[1])


def warm_driver_cache(out):
    exe = out / EXE_NAME
    if not exe.is_file() or not (out / 'shader-prepared.bin').is_file():
        return
    refresh_pipeline_seed(out)
    say("  the prepared shaders are built into the graphics driver's cache again")
    t0 = time.monotonic()
    try:
        rc = shader_run([str(exe), '--warm-shaders'], cwd=str(out), build_only=True)
    except OSError as e:
        rc = str(e)
    if rc:
        say(f'  that did not finish ({rc}); the game does it in the background while it runs')
    elif SHADERS_LEFT['n']:
        say(f"  driver's cache warm in {time.monotonic() - t0:.0f} s, but for {left_text(SHADERS_LEFT['n'])}")
    else:
        say(f"  driver's cache warm in {time.monotonic() - t0:.0f} s")


def left_text(n):
    return (f"{n} shader{'s' if n != 1 else ''} the graphics driver was still building: the rest continues in the game, "
            'in the background')


DRIVER_CACHE_VENDORS = ('NVIDIA', 'AMD', 'Intel')
DRIVER_CACHE_SCAN_S = 20
DRIVER_CACHE_ENV = '__GL_SHADER_DISK_CACHE_PATH'


def driver_cache_folders(out, windows=WIN, local_appdata=None):
    found = []
    if (Path(out) / 'nvidia-cache').is_dir():
        found.append(('nvidia-cache', Path(out) / 'nvidia-cache'))
    if windows:
        base = os.environ.get('LOCALAPPDATA') if local_appdata is None else local_appdata
        if base:
            root = Path(base)
            for vendor in DRIVER_CACHE_VENDORS:
                try:
                    entries = list(os.scandir(root / vendor))
                except OSError:
                    continue
                names = sorted(e.name for e in entries if e.is_dir() and 'cache' in e.name.lower())
                found += [(f'%LOCALAPPDATA%\\{vendor}\\{name}', root / vendor / name) for name in names]
            if (root / 'D3DSCache').is_dir():
                found.append(('%LOCALAPPDATA%\\D3DSCache', root / 'D3DSCache'))
    else:
        if (Path(out) / 'mesa-cache').is_dir():
            found.append(('mesa-cache', Path(out) / 'mesa-cache'))
    custom = os.environ.get(DRIVER_CACHE_ENV)
    if custom and Path(custom).is_dir():
        found.append((DRIVER_CACHE_ENV, Path(custom)))
    seen, once = set(), []
    for label, folder in found:
        try:
            key = os.path.normcase(os.path.realpath(folder))
        except OSError:
            key = str(folder)
        if key not in seen:
            seen.add(key)
            once.append((label, folder))
    return once


def folder_size(folder, budget_s=DRIVER_CACHE_SCAN_S):
    total = files = 0
    why = ''
    pending = [str(folder)]
    t0 = time.monotonic()
    while pending:
        try:
            with os.scandir(pending.pop()) as it:
                for e in it:
                    try:
                        if e.is_dir(follow_symlinks=False):
                            pending.append(e.path)
                        else:
                            total += e.stat(follow_symlinks=False).st_size
                            files += 1
                    except FileNotFoundError:
                        continue
                    except OSError:
                        why = 'some of it could not be read'
        except FileNotFoundError:
            continue
        except OSError:
            why = 'some of it could not be read'
        if pending and time.monotonic() - t0 > budget_s:
            return total, files, 'the count stopped'
    return total, files, why


def say_driver_caches(out, when):
    try:
        deadline = time.monotonic() + DRIVER_CACHE_SCAN_S
        parts = []
        for label, folder in driver_cache_folders(out):
            size, files, why = folder_size(folder, max(0.0, deadline - time.monotonic()))
            parts.append(f'{label} {size / 2**30:.2f} GB in {files:,} files' + (f' ({why})' if why else ''))
        if parts:
            say(f'  graphics driver caches {when}: ' + '; '.join(parts))
    except Exception as e:  # noqa: BLE001 - a report must never stop the step
        say(f'  graphics driver caches {when}: not counted ({e})')


def step_shaders(out, update=False):
    say_driver_caches(out, 'before the step')
    try:
        shaders_work(out, update)
    finally:
        say_driver_caches(out, 'after the step')


def shaders_work(out, update=False):
    for d in (KIT / 'bin', KIT / 'build' / 'xlate'):
        if (d / XLATE_NAME).is_file():
            note_cache_translator(out)
            for name in (XLATE_NAME, 'nx2navi_xlate.id'):
                copy_if_changed(d / name, out / name)
            break
    cmd = [sys.executable, '-u', str(KIT / 'tools' / 'prepare_shaders.py'), str(out), '--romfs', str(out / 'romfs'),
           '--exefs', str(out / 'exefs'), '--no-list', '--exe', EXE_NAME]
    why = ''
    carry = update and shader_carry(out)
    if carry and prepared_from(out) is not None:
        if shader_run(cmd + ['--no-run']) == 0:
            why = seed_changed(out)
        if why:
            say(f'  {why}: preparing them again')
    if carry and not why:
        for seed in SEED_FILES:
            (out / seed).unlink(missing_ok=True)
        say('  the shaders prepared before this update stay in use; the game translates them again in the background')
        if loose_shaders(out) >= PACK_LOOSE_MIN:
            emit('progress', step='shaders', fraction=0.0, detail='Folding the shader cache into one file')
            pack_shader_cache(out)
        warm_driver_cache(out)
        return
    rc = shader_run(cmd)
    if not rc and SHADERS_LEFT['n']:
        say(f"  shaders prepared, but for {left_text(SHADERS_LEFT['n'])}")
    if rc:
        fail('preparing the shaders failed (the lines above say why); the game still runs, compiling shaders as it '
             'meets them. Run the build again to retry this step')
    for seed in SEED_FILES:
        (out / seed).unlink(missing_ok=True)
    pack_shader_cache(out)


PACK_LOOSE_MIN = 1000


def loose_shaders(out):
    try:
        with os.scandir(out / 'shader-cache') as it:
            return sum(1 for e in it if e.name.endswith('.spv'))
    except OSError:
        return 0


def pack_shader_cache(out):
    tool = KIT / 'tools' / 'pack_shader_cache.py'
    if not tool.is_file() or not (out / 'shader-cache').is_dir():
        return
    t0 = time.monotonic()
    r = subprocess.run([sys.executable, '-u', str(tool), str(out / 'shader-cache')], stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, text=True, encoding='utf-8', errors='replace')
    for line in r.stdout.splitlines():
        say('  ' + line)
    if r.returncode:
        say(f'  packing the shader cache failed (code {r.returncode}); the game reads the loose files instead')
    else:
        say(f'  shader cache packed in {time.monotonic() - t0:.0f} s')


def prompt_art_dir():
    art = KIT / prompt_art.FOLDER
    return art if art.is_dir() else None


PROMPTS_RECORD = 'button-prompts.txt'


def record_prompts(out, line):
    say(f'prompts: {line}')
    try:
        (out / PROMPTS_RECORD).write_text(private_paths.shorten(line) + '\n', encoding='utf-8', newline='\n')
    except OSError as e:
        say(f'prompts: could not write {out / PROMPTS_RECORD} ({error_text(e)})')


def prompts_drawn(out):
    followed = prompt_art.binds_followed('\n'.join(bind_lines(out)))
    return f'drawn from {prompt_art.source_of(prompt_art_dir())}' + (f'; {followed}' if followed else '')


def prompts_none():
    return (f'not drawn: this version carries no button art ({KIT / prompt_art.FOLDER} is missing), so the game keeps '
            'its own prompts')


def bind_lines(out):
    try:
        ini = out / 'nx2saria.ini'
        if not ini.is_file(): ini = out / 'nx2navi.ini'
        text = ini.read_text(encoding='utf-8', errors='replace')
    except OSError:
        return []
    lines = (ln.split('#')[0].split(';')[0].strip() for ln in text.splitlines())
    return [ln for ln in lines if ln.lower().startswith('bind_')]


def step_prompts(out):
    total = len(prompt_art.OVERLAYS)
    proc = subprocess.Popen(py_cmd(KIT / 'tools' / 'pad_prompts.py', '--art', prompt_art_dir(), '--game', out),
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding='utf-8',
                            errors='replace', bufsize=1)
    done = 0
    emit('progress', step='prompts', fraction=0.0, detail=f'Drawing {total} sets of button prompts')
    try:
        for line in proc.stdout:
            line = line.rstrip('\n')
            if line.startswith('{'):
                continue
            say(f'  {line}')
            if PROMPTS_WRITTEN.match(line):
                done += 1
                emit('progress', step='prompts', fraction=round(done / total, 4),
                     detail=f'Drew {done} of {total} sets of button prompts')
    finally:
        rc = proc.wait()
        proc.stdout.close()
    if rc:
        record_prompts(out, 'not drawn: drawing them failed (the build log says why)')
        fail('drawing the button prompts failed (the lines above say why); the game still runs. Run the build again '
             'to retry this step')
    record_prompts(out, prompts_drawn(out))


AWAKE = None


def keep_awake():
    global AWAKE
    if os.name != 'nt':
        if AWAKE is None:
            try:
                AWAKE = subprocess.Popen(['systemd-inhibit', '--what=sleep', '--mode=block', '--who=NX2Saria',
                                          '--why=Building the game', 'cat'], stdin=subprocess.PIPE,
                                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                         start_new_session=True)
            except OSError:
                pass
        return
    try:
        import ctypes
        ES_CONTINUOUS, ES_SYSTEM_REQUIRED = 0x80000000, 0x00000001
        ctypes.windll.kernel32.SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED)
    except (OSError, AttributeError):
        pass


def load_state(path):
    try:
        state = json.loads(path.read_text(encoding='utf-8')) if path.is_file() else {}
    except (OSError, ValueError):
        state = {}
    if state.get('version') == 1:
        done = state.get('done', [])
        state = {'version': STATE_VERSION, 'input': state.get('input'), 'steps': {}}
        for step in ('exefs', 'romfs'):
            if step in done:
                state['steps'][step] = step_key(step, state['input'])
    return state


CPUINFO = Path('/proc/cpuinfo')
CPUINFO_MACROS = {'pni': '__SSE3__', 'ssse3': '__SSSE3__', 'sse4_1': '__SSE4_1__', 'sse4_2': '__SSE4_2__',
                  'popcnt': '__POPCNT__', 'lahf_lm': '__LAHF_SAHF__', 'cx16': '__GCC_HAVE_SYNC_COMPARE_AND_SWAP_16',
                  'avx': '__AVX__', 'avx2': '__AVX2__', 'bmi1': '__BMI__', 'bmi2': '__BMI2__', 'f16c': '__F16C__',
                  'fma': '__FMA__', 'abm': '__LZCNT__', 'movbe': '__MOVBE__', 'xsave': '__XSAVE__'}


def cpu_flags():
    if os.name == 'nt':
        return None
    try:
        lines = CPUINFO.read_text(encoding='ascii', errors='replace').splitlines()
    except OSError:
        return None
    flags = []
    for line in lines:
        key, _, rest = line.partition(':')
        if key.strip() == 'flags':
            flags.append(set(rest.split()))
    return set.intersection(*flags) if flags else None


def cpu_tag():
    if os.name != 'nt':
        have = cpu_flags()
        if have is None:
            return 'any'
        return ''.join('1' if f in have else '0' for f in ('sse4_2', 'avx2', 'avx512f'))
    try:
        import ctypes
        present = ctypes.windll.kernel32.IsProcessorFeaturePresent
        return ''.join('1' if present(f) else '0' for f in (38, 40, 41))
    except (OSError, AttributeError):
        return 'any'


def step_scan(out):
    proc = subprocess.Popen(py_cmd(KIT / 'tools' / 'romfs_check.py', '--game', out),
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding='utf-8',
                            errors='replace', bufsize=1)
    emit('progress', step='scan', fraction=0.0, detail='Checking the game files')
    try:
        for line in proc.stdout:
            line = line.rstrip('\n')
            m = SCAN_CHECKED.match(line)
            if m:
                emit('progress', step='scan', fraction=round(int(m[1]) / max(int(m[2]), 1), 4),
                     detail=f'Checked {m[1]} of {m[2]} files')
            else:
                say(f'  {line}')
    finally:
        rc = proc.wait()
        proc.stdout.close()
    if rc:
        fail('checking the game files failed (the lines above say why); the game still runs. Run the build again to '
             'retry this step')


def step_key(step, identity, out=None):
    if step in ('exefs', 'romfs'):
        return f'{step}-1|{identity}'
    if step == 'scan':
        written = 'written' if (out / 'romfs-check.json').is_file() else 'missing'
        tool = KIT / 'tools' / 'romfs_check.py'
        return f'scan-1|{identity}|{SCAN_TOOL if tool.is_file() else "no tool"}|{written}'
    if step == 'prompts':
        binds = sha256_bytes(json.dumps(bind_lines(out)).encode())
        written = all((out / f / 'manifest.tsv').is_file() for f in prompt_art.OVERLAYS)
        return (f'prompts-1|{identity}|{tree_digest(prompt_art_dir(), ("**/*.png",))}|{PROMPT_TOOLS}|{binds}'
                f'|{"written" if written else "missing"}')
    base = f'lift|{exefs_digest(out)}|{lift_digest()}'
    mods = mod_digest(out) if step in ('lift', 'compile') else ''
    lift = f'{base}|mods {mods}' if mods else base
    if step == 'lift':
        return lift

    def compile_of(lift):
        return (f'compile|{lift}|{tree_digest(KIT / "runtime", ("**/*",))}|{sha256_file(Path(__file__))}'
                f'|{kit_version()}|cpu {cpu_tag()}')
    if step == 'compile':
        return compile_of(lift) + pgo_tag() + sdl3_tag() + ('|bench-tsan' if os.environ.get('NX2SARIA_BENCH_TSAN') == '1' else '')
    return (f'shaders-1|{compile_of(base)}|{sha256_file(KIT / "tools" / "prepare_shaders.py")}'
            f'|{xlate_id()}')


def out_path(arg):
    return Path(os.path.abspath(arg)) if WIN else Path(arg).resolve()


def prepare_tsan_output(out, bench_tsan):
    if bench_tsan and out.name != 'g25-tsan':
        raise ValueError('the bench ThreadSanitizer build requires a separate g25-tsan output folder')
    marker = out / '.bench-tsan'
    if not bench_tsan and marker.exists():
        raise ValueError('this output belongs to the bench ThreadSanitizer build')
    if bench_tsan:
        if (out / EXE_NAME).exists() and not marker.exists():
            raise ValueError('the output already contains an ordinary build')
        out.mkdir(parents=True, exist_ok=True)
        marker.touch()


def main():
    try:
        build_main()
    except nup.NupError as e:
        fail(f'the dump is damaged: {e}. Copy the dump to this PC again, or dump the game again, and then try again')
    except Exception as e:   # noqa: BLE001 - whatever it is, the player gets its message
        with OUT_LOCK:
            traceback.print_exc()
        fail(f'the build stopped: {error_text(e)}')


def build_main():
    global EVENTS, PGO_MODE, GATE_ARGS
    ap = argparse.ArgumentParser(description=DESCRIPTION)
    ap.add_argument('--nup', help='the game as one .nup file')
    ap.add_argument('--exefs', help='the game\'s exefs folder (with --romfs, instead of --nup)')
    ap.add_argument('--romfs', help='the game\'s romfs folder')
    ap.add_argument('--rebuild', action='store_true',
                    help='build again from the game files the last run unpacked into --out (after a kit update)')
    ap.add_argument('--retune', action='store_true',
                    help="with --rebuild: compile the lifted code for this processor's level even when the objects "
                         'kept from before an update were compiled for another (Rebuild the Game)')
    ap.add_argument('--out', help='the folder for the finished game')
    ap.add_argument('--check', action='store_true', help='only check the game\'s version; builds nothing')
    ap.add_argument('--msys', default=r'C:\msys64\ucrt64',
                    help='MSYS2 UCRT64 folder (default C:\\msys64\\ucrt64; Windows only: Linux takes clang and lld from PATH)')
    ap.add_argument('--jobs', type=int,
                    help='most compiles at once (default: one per processor); the memory gate starts each one when '
                         'the free memory holds it')
    ap.add_argument('--events', action='store_true', help='machine-readable progress lines on stdout ("@@" + JSON)')
    ap.add_argument('--make-runtime-lib', metavar='DIR', help='build the prebuilt runtime library of a release into DIR')
    ap.add_argument('--obj', help='with --make-runtime-lib: the folder for its objects')
    ap.add_argument('--pgo', choices=('gen', 'off'),
                    help='gen: build an instrumented game, which adds its execution counts to <out>/pgo/*.profraw '
                         '(for tools/pgo.py merge); off: build without the execution profile of the kit. Default: the '
                         'environment variable NX2SARIA_PGO (NX2NAVI_PGO remains an alias), else build with the profile of the kit when it fits')
    ap.add_argument('--until', choices=STEPS, help=argparse.SUPPRESS)
    memgate.add_arguments(ap)
    a = ap.parse_args()
    if os.environ.get('NX2SARIA_BENCH_TSAN') == '1' and (WIN or a.make_runtime_lib):
        ap.error('the bench ThreadSanitizer build is Linux-only and cannot make a prebuilt runtime')
    EVENTS = a.events
    PGO_MODE = a.pgo or ''
    GATE_ARGS = a
    jobs_why = '--jobs'
    if not a.jobs or a.jobs < 1:
        a.jobs, jobs_why = default_jobs()
    if a.make_runtime_lib:
        make_runtime_lib(a.make_runtime_lib, Path(a.msys), a.jobs, a.obj)
        return
    given = bool(a.nup) + bool(a.exefs or a.romfs) + bool(a.rebuild)
    if given != 1 or (not a.nup and not a.rebuild and not (a.exefs and a.romfs)):
        ap.error('give --nup, both --exefs and --romfs, or --rebuild')
    if a.rebuild and not a.out:
        ap.error('--rebuild needs --out')
    out = out_path(a.out) if a.out else None
    cache = out / 'build-cache' if out else None
    state_path = cache / 'state.json' if cache else None
    state = load_state(state_path) if state_path else {}

    emit('step', step='check', state='start')
    src = BuiltSource(out, state) if a.rebuild else Source(a)
    check(src)
    say('check: Ocarina of Time 1.1.1 (build e0f3ccfa)')
    emit('check', version='1.1.1', build=MAIN_BUILD_ID[:8])
    emit('step', step='check', state='done')
    if a.check:
        return
    if not out:
        ap.error('--out is needed to build')
    bench_tsan = os.environ.get('NX2SARIA_BENCH_TSAN') == '1'
    try:
        prepare_tsan_output(out, bench_tsan)
    except ValueError as e:
        ap.error(str(e))
    msys = Path(a.msys)
    if WIN:
        for need in ('bin/clang.exe', 'bin/clang++.exe', 'bin/SDL3.dll', 'lib/libSDL3.dll.a', 'include/vma'):
            if not (msys / need).exists():
                fail(f'{msys / need} is missing: install MSYS2 and the packages README-BuildKit.md lists')
    else:
        for tool in ('clang', 'clang++', 'ld.lld'):
            if not shutil.which(tool):
                fail(f'{tool} is not on PATH: install the packages README-BuildKit.md lists under "Linux"')
    packages = {'numpy': 'numpy', 'lz4': 'lz4', **({} if a.rebuild else {'zbic': 'zbic'}),
                **(PROMPT_PACKAGES if prompt_art_dir() else {})}
    for pip_name, mod in packages.items():
        try:
            __import__(mod)
        except ImportError as e:
            extra = ' fonttools pillow' if prompt_art_dir() else ''
            fail(f'the Python package {pip_name} is missing or does not load ({e}): '
                 f'pip install numpy lz4 zstandard zbic{extra}')
    cache.mkdir(parents=True, exist_ok=True)
    if pgo_mode() == 'gen':
        os.environ['LLVM_PROFILE_FILE'] = str(cache / 'pgo-build' / 'build_%m.profraw')
    reset = state.get('version') != STATE_VERSION or state.get('input') != src.identity
    if reset:
        state = {'version': STATE_VERSION, 'input': src.identity, 'steps': {}}
    state['kit'] = kit_version()
    if a.rebuild and a.retune and retune_due(cache / 'obj', msys):
        state['steps'].pop('compile', None)
    keep_awake()
    ahead = steps_to_run(state, src.identity, out, a.until)
    check_space(out, space_needs(ahead, out, sum(src.romfs_size(ref) for _, ref in src.romfs) if 'romfs' in ahead else 0,
                                 a.rebuild and shader_carry(out)))
    if WIN and 'compile' in ahead:
        probe_toolchain(msys, cache / 'toolchain-check')
    if not WIN and any(state['steps'].get(n) != step_key(n, src.identity, out) for n in STEPS[:STEPS.index('compile') + 1]):
        preflight(msys, a.jobs, work=cache / 'toolchain-check')
    say(jobs_line(a.jobs, jobs_why))
    t0 = time.time()
    romfs_job = None
    runs = {'exefs': lambda: step_exefs(src, out), 'romfs': lambda: step_romfs(src, out, min(a.jobs, 8)),
            'lift': lambda: step_lift(out, cache),
            'compile': lambda: step_compile(out, cache, msys, a.jobs, romfs_job, a.rebuild and not a.retune),
            'shaders': lambda: step_shaders(out, a.rebuild), 'prompts': lambda: step_prompts(out),
            'scan': lambda: step_scan(out)}
    state_lock = threading.Lock()

    def save():
        with state_lock:
            tmp = state_path.with_name(state_path.name + '.tmp')
            tmp.write_text(json.dumps(state, indent=1), encoding='utf-8')
            os.replace(tmp, state_path)

    if reset:
        save()

    def run(name):
        t = time.time()
        runs[name]()
        with state_lock:
            state['steps'][name] = step_key(name, src.identity, out)
        save()
        say(f'{name}: {time.time() - t:.0f} s')
        emit('step', step=name, state='done', seconds=round(time.time() - t, 1))

    def lift_gone(name):
        return (name == 'lift' and not (cache / 'lift' / 'layout.json').is_file()
                and state['steps'].get('compile') != step_key('compile', src.identity, out))

    try:
        for name in STEPS:
            if name == 'prompts' and not prompt_art_dir():
                say('prompts: none to draw, the kit has no button art')
                record_prompts(out, prompts_none())
                emit('step', step=name, state='skip')
            elif state['steps'].get(name) == step_key(name, src.identity, out) and not lift_gone(name):
                say(f'{name}: done before')
                if name == 'prompts':
                    record_prompts(out, prompts_drawn(out))
                emit('step', step=name, state='skip')
            else:
                with state_lock:
                    for later in STEPS[STEPS.index(name):]:
                        if later == name or not ({name, later} & set(OWN_KEY_STEPS) or (name, later) in UNAFFECTED):
                            state['steps'].pop(later, None)
                if name == 'lift':
                    save()
                say(f'{name}:')
                emit('step', step=name, state='start')
                if name in ('shaders', 'prompts', 'scan') and romfs_job:
                    romfs_job.join()
                if name == 'romfs' and a.until != 'romfs':
                    romfs_job = Background(lambda: run('romfs'))
                else:
                    run(name)
            if romfs_job and romfs_job.failed():
                romfs_job.join()
            if name == a.until:
                break
        if romfs_job:
            romfs_job.join()
    except (SystemExit, Exception):
        if romfs_job and romfs_job.thread.is_alive():
            say('waiting for the RomFS image before stopping')
            romfs_job.thread.join()
        raise
    save()
    say(f'\nfinished in {(time.time() - t0) / 60:.1f} min: {out / EXE_NAME}')
    emit('end', exe=str(out / EXE_NAME), seconds=round(time.time() - t0, 1), kit=kit_version())


if __name__ == '__main__':
    main()
