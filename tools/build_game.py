"""Compile the generated C for one game version and link it with the runtime into build/<version>/nx2saria.exe.

Writes build/<version>/build.ninja and runs MSYS2 UCRT64 Ninja on it. Ninja keeps the build incremental: a chunk is
recompiled only when it, a header it includes or its flags change.

On Linux the same steps make build/<version>/nx2saria with the clang, lld and ninja on PATH (or the clang and lld in --toolchain DIR/bin): the
runtime's *_sdl.* and platform/linux.c take the place of its *_win.*, platform/win32.c and nx2saria.rc, the libraries are
the system's (Vulkan, SDL3, Opus, dav1d, libvpx, zstd; libstdc++ static), and the executable finds a libSDL3.so.0 beside it
first (rpath $ORIGIN). docs/building.md lists the packages.

    .venv\\Scripts\\python tools\\build_game.py                     # generated/v121 -> build/v121/nx2saria.exe
    .venv\\Scripts\\python tools\\build_game.py --modules subsdk0   # one module (links if it has no calls into others)
    .venv\\Scripts\\python tools\\build_game.py --jobs 8
    .venv\\Scripts\\python tools\\build_game.py --out build/v121-t2   # a separate build directory
    .venv\\Scripts\\python tools\\build_game.py --toolchain toolchain/llvm-mingw-<version>/llvm-mingw-<version>-ucrt-x86_64 \\
        --out build/v121-llvmmingw                                    # llvm-mingw instead of MSYS2 UCRT64

Every runtime source is compiled. build/<version>/modules.c carries the layout, the linked function tables, the
expected SHA-256 of each ExeFS file (tools/data/expected-<display version>.json) and the default game paths
(--exefs, --romfs, --profile), which nx2saria.exe uses unless its own command line names others. --exefs and --romfs
default to extracted/<version> in the work folder: the [work] dir of config.local.toml, where tools/extract.py writes
them, else build/. tools/romfs_index.py writes build/<version>/romfs-index.bin and romfs_config.h from the RomFS
directory's index.json (runtime/hle/romfs.c reads the index next to nx2saria.exe). Without that index.json the build stops:
the exe would stop at start. --no-romfs builds it anyway.

Private builds reuse the main build's objects. When --out differs from build/<version>, the main build's generated
objects are up to date (checked with a dry run of its build.ninja) and every header they include from runtime/ is
byte-identical in --runtime (the list comes from the main build's Ninja deps log), build_game.py hardlinks those
objects into <out>/obj and compiles only the runtime. A hardlink is a snapshot: clang and Ninja replace an object by
renaming a new file over it, so a later main rebuild leaves the linked copies alone. Otherwise (a header differs, a
main object is missing or stale, another --gen) it compiles everything and prints one line saying why. --full-build
skips the check.

Object manifest. A flipped bit in an object written by an earlier build stays forever, because Ninja rebuilds an object
only when its inputs change, and forks hardlink the object into every private build. So each build dir keeps
obj/objects.sha256 (SHA-256, size and mtime of every object it compiled). After Ninja finishes and before the link,
build_game.py hashes the objects, compares each one whose size and mtime match its manifest entry, and records the
objects Ninja just rebuilt (new mtime). A mismatch deletes the object, Ninja recompiles it, and the build prints the
summary line. A fork checks the hardlinked generated objects against build/<version>/obj/objects.sha256 (read-only; a
bad shared object stops the fork and asks for a main build run, which recompiles it and breaks the shared inode).

Memory. Ninja runs up to --jobs compiles at once (default one per processor), and each compile goes through the memory
gate (tools/memgate.py wrap): it starts only when the free memory, less what the running compiles may still take and
less a reserve, holds that compile's estimated peak. The estimates live in <out>/memgate/estimates.json: each object's
peak measured in earlier builds, else a size model from the lift manifest's chunk_info, else 400 MiB (C) or 1 GiB (C++)
for a runtime source. Before Ninja, build_game.py writes the gate's settings and every object's need into
<out>/memgate; after the compile phase it reads the peaks the wrappers recorded, learns them and prints a summary (held
compiles, the largest peaks, the peak of all compiles together, the least free memory, how close the estimates came).
--no-mem-gate (or NX2SARIA_BUILD_MEM_GATE=off) starts compiles by the job count alone, and then nothing limits the
compiles by memory; the wrappers still measure. Each build folder has its own gate, so two builds running at once see
each other only through the free memory. The wrapper's command names this Python and tools/memgate.py (relative to the
root where they lie below it): a build with another Python, and the first build after this change, compile every
object again. Under SCCACHE the compiles run in sccache's server, out of the wrapper's reach, so build.ninja leaves
the wrapper out and the gate is off. Each phase also reports its wall time and the peak memory of its processes
(Windows job object accounting: committed private bytes).

--ninja PATH names the Ninja to run; without MSYS2 the bin/ninja.exe of a setup release folder (docs/installer.md) is used.
--toolchain DIR builds with an llvm-mingw tree (DIR/bin/clang.exe) instead of MSYS2 UCRT64 clang. Ninja still comes
from MSYS2. The libraries and headers MSYS2 supplies (Vulkan, SDL3, Opus, dav1d, libvpx, VMA) then come from --deps DIR
(default toolchain/deps-mingw: include/, lib/ with the import libraries, libdav1d.a and libvpx.a, bin/ with the DLLs), and the C++
runtime is libc++ from llvm-mingw. Such a build never reuses another build's generated objects.

Split build. --make-runtime-lib compiles only runtime/ into <out>/libnx2navirt.a (what CI prebuilds). --runtime-lib PATH
then compiles only the generated C and the install-time files (modules.c and INSTALL_SIDE, which bake the player's
game hashes and version in) and links them with that library. The library holds no game-derived data.

Standard library only.
"""
import argparse
import collections
import ctypes
import ctypes.wintypes as wt   # the job-object structures below; importing it works on every OS
import hashlib
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import threading
import time
import urllib.request
from pathlib import Path

import check_isa
import romfs_index
import build_shaders
import memgate
from check_content import GENERATED_MARKER

WIN = os.name == 'nt'
if not WIN:
    import resource
ROOT = Path(__file__).resolve().parent.parent
# In a git worktree (tools/worktree.py) build/ and generated/ are junctions into the main checkout, and resolve() follows
# them. MAIN_ROOT is where they land, so resolved paths still print and compare relative to a root.
MAIN_ROOT = (ROOT / 'build').resolve().parent
UCRT = Path(r'C:\msys64\ucrt64\bin')
EXE = 'nx2saria.exe' if WIN else 'nx2saria'


def on_path(name):
    """name on PATH, else a path that does not exist (the tool check below names it)."""
    found = shutil.which(name)
    return Path(found) if found else Path('/usr/bin') / name


CLANG = UCRT / 'clang.exe' if WIN else on_path('clang')
NINJA = UCRT / 'ninja.exe' if WIN else on_path('ninja')
SDL3_DLLS = [UCRT / 'SDL3.dll', UCRT / 'libiconv-2.dll', UCRT / 'libopus-0.dll'] if WIN else []

# Flags for generated code, plus -march for the FP/SIMD lowering.
# --cpu v2 replaces the level for CPUs without AVX2/FMA; runtime/fma.c then supplies the correctly rounded fma the
# lowering needs (runtime/simd.h).
GEN_FLAGS = ['-O1', '-fno-strict-aliasing', '-Wno-return-type', '-Wno-unused-label', '-fvisibility=hidden',
             '-march=x86-64-v3']
RT_FLAGS = ['-O2', '-g', '-Wall', '-fno-strict-aliasing', '-march=x86-64-v3']


def set_cpu(cpu):
    """Target x86-64-v3 (the default) or x86-64-v2. RT_CPU_LEVEL lets runtime/main.c's self-test check the same
    instruction set the build was compiled for."""
    march = f'-march=x86-64-{cpu}'
    GEN_FLAGS[GEN_FLAGS.index('-march=x86-64-v3')] = march
    RT_FLAGS[RT_FLAGS.index('-march=x86-64-v3')] = march
    RT_FLAGS.append(f'-DRT_CPU_LEVEL={3 if cpu == "v3" else 2}')


# Profile-guided optimisation (--pgo). Both modes compile the generated code at -O2, the level the profile's inlining and
# hot/cold layout work at. A static function's profile name carries its source path by default; the base name alone
# keeps the names equal between the training build and a build from another tree (a worktree, an install).
PGO_NAMES = ['-mllvm', '-static-func-full-module-prefix=false']
PGO_INPUT = None   # the .profdata of --pgo FILE: every compile depends on it, so a new profile rebuilds them


def set_pgo(pgo, out):
    """--pgo gen: instrument the generated code and the runtime; nx2saria.exe adds its counts to
    out/pgo/default_<id>.profraw every minute and at exit (runtime/platform win32.c), across runs. --pgo FILE: compile
    with that merged profile (tools/pgo.py merge). Functions the profile never saw run are optimised for size."""
    global PGO_INPUT
    GEN_FLAGS[GEN_FLAGS.index('-O1')] = '-O2'
    if pgo == 'gen':
        # absolute: the instrumented exe resolves the directory against its own working directory
        flags = [f'-fprofile-generate={cmd_arg((out / "pgo").resolve().as_posix())}']
        RT_FLAGS.append('-DNX2SARIA_PGO_GEN')
        LINK_FLAGS.append('-fprofile-generate')   # links clang_rt.profile
    else:
        PGO_INPUT = Path(pgo).resolve()
        if not PGO_INPUT.is_file():
            sys.exit(f'--pgo {pgo}: no such profile')
        # unprofiled: chunks the training never entered; out-of-date and backend-plugin: functions whose code changed
        # since the training, which then compile without a profile
        flags = [f'-fprofile-use={cmd_path(PGO_INPUT)}', '-Wno-profile-instr-unprofiled',
                 '-Wno-profile-instr-out-of-date', '-Wno-backend-plugin']
    GEN_FLAGS.extend(flags + PGO_NAMES)
    RT_FLAGS.extend(flags + PGO_NAMES)
if WIN:
    # _WIN32_WINNT: Windows 10, the port's minimum. MSYS2's headers default to it, llvm-mingw's (the release toolchain)
    # to Windows 7, which hides the per-monitor DPI functions gfx_window_win.c and dpi_probe_win.c call.
    RT_FLAGS.append('-D_WIN32_WINNT=0x0A00')
    # -Xlinker passes the map path as one argument; -Wl, would split it at any comma in the build dir path
    LINK_FLAGS = ['-fuse-ld=lld', '-Wl,--stack,16777216', '-Xlinker', '"--Map=$builddir/nx2navi.map"']   # the map feeds tools/prof_report.py
    LINK_LIBS = ['-lpsapi', '-lbcrypt', '-lws2_32', '-lvulkan-1', '-luser32', '-lgdi32', '-ldwmapi', '-limm32', '-lwinmm', '-lole32', '-lshell32', '-luuid', '-lSDL3', '-lopus', '-l:libdav1d.a', '-l:libvpx.a', '-l:libzstd.a', '-l:libstdc++.a', '-l:libwinpthread.a']   # Vulkan back end; libvpx and libdav1d static (movies); libzstd static (resolution.c)
    VMA_INCLUDE = UCRT.parent / 'include' / 'vma'   # vk_mem_alloc.h; MSYS2 package vulkan-memory-allocator
else:
    # -no-pie: the lifted modules' function tables (hundreds of thousands of pointers) need no load-time relocation.
    # $ORIGIN: a libSDL3.so.0 next to nx2navi comes before the system's (the release's portable folder).
    # -Xlinker for the map path, as on Windows above: -Wl, would split it at any comma in the build dir path.
    LINK_FLAGS = ['-fuse-ld=lld', '-no-pie', '-Wl,-z,noexecstack', "'-Wl,-rpath,$$ORIGIN'",
                  '-Xlinker', '"-Map=$builddir/nx2navi.map"']
    LINK_LIBS = ['-lvulkan', '-lSDL3', '-lopus', '-ldav1d', '-lvpx', '-lzstd', '-l:libstdc++.a', '-lm', '-lpthread', '-ldl']
    VMA_INCLUDE = Path('/usr/include')   # vk_mem_alloc.h; Arch (AUR) vulkan-memory-allocator, Debian libvulkan-memory-allocator-dev
# Without a system vk_mem_alloc.h, the header pinned here (VMA 3.4.0, the version MSYS2 packages) is fetched once
# into toolchain/vma: one MIT header (THIRD_PARTY_NOTICES.md), checked against its hash.
VMA_FETCH = ('https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/v3.4.0/include/vk_mem_alloc.h',
             '8487b7995ad3b263eb73bc5b9a77d71aa69b6bef5d58a715c02d2663afd81f1a')
VMA_LOCAL = ROOT / 'toolchain' / 'vma'


def vma_include():
    """The directory holding vk_mem_alloc.h: the system's, else toolchain/vma, fetched there on first use."""
    if (VMA_INCLUDE / 'vk_mem_alloc.h').is_file():
        return VMA_INCLUDE
    dst = VMA_LOCAL / 'vk_mem_alloc.h'
    if not dst.is_file():
        url, sha = VMA_FETCH
        print(f'build_game: no {VMA_INCLUDE / "vk_mem_alloc.h"}; fetching VMA 3.4.0 into {VMA_LOCAL}')
        try:
            data = urllib.request.urlopen(url, timeout=60).read()
        except OSError as e:
            sys.exit(f'build_game: fetching {url} failed ({e}); install VMA or put vk_mem_alloc.h in {VMA_LOCAL}')
        if hashlib.sha256(data).hexdigest() != sha:
            sys.exit(f'build_game: {url} does not match the pinned hash')
        VMA_LOCAL.mkdir(parents=True, exist_ok=True)
        dst.write_bytes(data)
    return VMA_LOCAL


def system_vulkan_version(clang, env=None):
    """VK_HEADER_VERSION of the <vulkan/vulkan_core.h> this compiler finds by itself (its own search path, CPATH
    included), 0 when it finds none."""
    probe = '#include <vulkan/vulkan_core.h>\nnx2navi_vulkan_headers VK_HEADER_VERSION\n'
    try:
        p = subprocess.run([str(clang), '-E', '-P', '-x', 'c', '-'], input=probe, capture_output=True, text=True,
                           env=env, errors='replace')
    except OSError:
        return 0
    m = re.search(r'nx2navi_vulkan_headers\s+(\d+)', p.stdout) if p.returncode == 0 else None
    return int(m.group(1)) if m else 0


def vulkan_include(clang=None, env=None):
    """A build with the system's own headers (Linux without --deps): the folder to put ahead of them for
    <vulkan/vulkan.h>, or None when the system's serve. The runtime names Vulkan extensions newer than the headers
    many distributions package, and every file that does fails to compile there (tools/vulkan_pin.py). So:
    toolchain/vulkan-headers when it holds the pinned release (a kit carries it; a checkout once it was fetched), else
    nothing when the system's headers are at least that release, else the pinned release, fetched there once.
    Windows takes MSYS2's headers and returns None."""
    if WIN:
        return None
    import vulkan_pin
    local = ROOT / vulkan_pin.FOLDER
    if vulkan_pin.have(local):
        return local / 'include'
    found = system_vulkan_version(clang or CLANG, env)
    if found >= vulkan_pin.HEADER_VERSION:
        return None
    print(f'build_game: the system\'s Vulkan headers are {f"release {found}" if found else "missing"}; the runtime is '
          f'built against release {vulkan_pin.HEADER_VERSION} ({vulkan_pin.VERSION}) or newer', flush=True)
    return vulkan_pin.resolve(local) / 'include'


# --toolchain: an llvm-mingw tree. The libraries, headers and DLLs MSYS2 supplies come from the deps directory instead.
LLVM_LINK_LIBS = ['-lpsapi', '-lbcrypt', '-lws2_32', '-luser32', '-lgdi32', '-ldwmapi', '-limm32', '-lwinmm', '-lole32', '-lshell32', '-luuid', '-lvulkan-1', '-lSDL3', '-lopus',
                  '-l:libdav1d.a', '-l:libvpx.a', '-l:libzstd.a', '-l:libc++.a', '-l:libc++abi.a', '-l:libunwind.a', '-l:libwinpthread.a']
DEFAULT_DEPS = ROOT / 'toolchain' / 'deps-mingw'
# Runtime sources compiled per install, not in the library: romfs.c includes romfs_config.h, patches.c and ov_zstd.c
# the patches.h of the player's game version (its patch page is the page after that version's main; ov_zstd.c's host
# functions sit on it, and patches.c registers the debug menu's free camera there). mouse_direct.c, sprint.c and heap_retry.c also
# register host callbacks relative to that version's patch page.
# Paths below runtime/.
INSTALL_SIDE = {'hle/romfs.c', 'patches/patches.c', 'hle/ov_zstd.c', 'input/mouse_direct.c', 'patches/sprint.c',
                'patches/heap_retry.c'}
LIB_NAME = 'libnx2navirt.a'
AR = UCRT / 'llvm-ar.exe' if WIN else on_path('llvm-ar')
WINDRES = UCRT / 'windres.exe'   # runtime/nx2saria.rc: the manifest of nx2saria.exe
# GNU windres runs its preprocessor through cmd.exe, and left to find it names the gcc beside it by full path: once that
# path and the .rc's both hold a space, cmd.exe drops the outer quotes and splits the command at the first space. gcc by
# bare name (Ninja's PATH starts with UCRT) and the .rc by its path below MAIN_ROOT keep the command line free of
# spaces. llvm-windres (--toolchain) starts its preprocessor without a shell and keeps its own.
WINDRES_PP = '--preprocessor=gcc --preprocessor-arg=-E --preprocessor-arg=-xc --preprocessor-arg=-DRC_INVOKED'
TOOLCHAIN_BIN = None   # llvm-mingw bin directory, set by main() for --toolchain
DEPS = None            # the deps directory of an llvm-mingw build
# tools/data/expected-<display version>.json for each build version name.
DISPLAY_VERSIONS = {'v0': '1.0.0', 'v110': '1.1.0', 'v112': '1.1.2', 'v121': '1.2.1', 'v143': '1.4.3'}

GiB = 1 << 30


def below_root(p):
    """Resolved p relative to ROOT, else to MAIN_ROOT (the same relative path through the worktree's junctions), else
    absolute."""
    p = Path(p).resolve()
    for root in (ROOT, MAIN_ROOT):
        if p.is_relative_to(root):
            return p.relative_to(root)
    return p


def ninja_path(p):
    """A path for a build line: relative to the repo root where possible, forward slashes, Ninja-escaped."""
    return below_root(p).as_posix().replace('$', '$$').replace(' ', '$ ').replace(':', '$:')


def flag_path(p):
    return below_root(p).as_posix()


def cmd_arg(s):
    """s for a Ninja command line: Ninja-escaped, and quoted when it holds a space (a user or folder name), as clang and
    lld split their Windows command line at unquoted spaces."""
    s = s.replace('$', '$$')
    return f'"{s}"' if ' ' in s else s


def cmd_path(p):
    return cmd_arg(flag_path(p))


def read_layout(gen):
    """[(name, base, size)] from layout.json (same format as recomp/layout.py read(), without its NSO dependency)."""
    data = json.loads((gen / 'layout.json').read_text())
    return [(m['name'], int(m['base'], 16), int(m['size'], 16)) for m in data]


def chunk_files(gen, mod):
    """The C files of one module: the chunks listed in <mod>/manifest.json, then <mod>_table.c."""
    d = gen / mod
    data = json.loads((d / 'manifest.json').read_text())
    entries = data if isinstance(data, list) else data.get('chunks', data.get('files'))
    if entries is None:
        sys.exit(f'{d / "manifest.json"}: no "chunks" or "files" list')
    names = []
    for e in entries:
        name = e if isinstance(e, str) else e.get('file', e.get('path', e.get('name')))
        names.append(Path(name).name)
    table = f'{mod}_table.c'
    if table not in names:
        names.append(table)
    files = [d / n for n in names]
    missing = [str(f) for f in files if not f.is_file()]
    if missing:
        sys.exit(f'{mod}: {len(missing)} files listed in manifest.json are missing, first {missing[0]}')
    return files


def write_if_changed(path, text):
    """Keep the mtime of an unchanged file so Ninja does not rebuild from it."""
    if path.exists() and path.read_text(encoding='utf-8', errors='replace') == text:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding='utf-8', newline='\n')   # Ninja and clang read UTF-8; paths may be non-ASCII


def copy_romfs_patches(version, out):
    """nx2saria.exe loads romfs-patches beside romfs-index.bin (runtime/hle/romfs.c overlay_init). tools/make_romfs_patches.py
    writes them into build/<version> only, so a build in another directory ran without the 60 fps overlay (the quick
    menus' captured background turned black). Copies them when their manifest differs; romfs-patches-xbox,
    romfs-patches-xbox-label and romfs-patches-pc (the same with Xbox or keyboard and mouse button prompts,
    make_romfs_patches.py --prompts) the same way when they were made."""
    for name in ('romfs-patches', 'romfs-patches-xbox', 'romfs-patches-xbox-label', 'romfs-patches-pc'):
        src = ROOT / 'build' / version / name
        dst = out / name
        if src.resolve() == dst.resolve() or not (src / 'manifest.tsv').is_file():
            continue
        if (dst / 'manifest.tsv').is_file() and (dst / 'manifest.tsv').read_bytes() == (src / 'manifest.tsv').read_bytes():
            continue
        shutil.rmtree(dst, ignore_errors=True)
        shutil.copytree(src, dst)
        print(f'romfs patches: copied {name} from {src}')


def work_dir():
    """The work folder: the [work] dir of config.local.toml (relative to the checkout), where tools/extract.py writes
    extracted/; build/ without the file or the key."""
    cfg = ROOT / 'config.local.toml'
    if not cfg.is_file():
        return ROOT / 'build'
    import tomllib
    return ROOT / tomllib.loads(cfg.read_text(encoding='utf-8')).get('work', {}).get('dir', 'build')


def c_string(s):
    return '"' + s.replace('\\', '\\\\').replace('"', '\\"') + '"'


RUNTIME = ROOT / 'runtime'   # --runtime: a copy of runtime/ lets a task build without touching the shared tree
# The runtime's folders by subsystem (docs/architecture.md, "Runtime folders"): their sources compile as the top level's
# do, and each is on the include path of the runtime's sources, which include one another's headers by name. The lifted
# C includes only headers at the top of runtime/, so its command (genflags) does not name them. tools/make_game.py has
# the same list (tests/runtime_dirs_check.py).
RUNTIME_DIRS = ('audio', 'cfg', 'diag', 'gfx', 'hle', 'input', 'nvn', 'pacing', 'patches', 'ui', 'vk')


def other_os(name):
    """A runtime file for another OS: Win32 ones (*_win.*, platform/win32.c, nx2saria.rc) off Windows, the SDL window's
    (*_sdl.*) and platform/linux.c on Windows."""
    stem = name.rsplit('.', 1)[0]
    if WIN:
        return stem.endswith('_sdl') or name == 'linux.c'
    return stem.endswith('_win') or name in ('win32.c', 'nx2saria.rc')


def runtime_sources(skip=()):
    """Every runtime/*.c, *.cpp, platform/*.c, *.rc, the *.c and *.cpp of each RUNTIME_DIRS folder and Dear ImGui of
    this OS except the names in skip, so a task adds a file without editing this script."""
    pats = ('*.c', '*.cpp', 'platform/*.c', '*.rc', *(f'{d}/*.{x}' for d in RUNTIME_DIRS for x in ('c', 'cpp')),
            'third_party/imgui/*.cpp', 'third_party/imgui/backends/*.cpp')
    return sorted(p for pat in pats for p in RUNTIME.glob(pat) if p.name not in skip and not other_os(p.name))


def runtime_include_dirs(rt=None):
    """The include folders of the runtime's sources: runtime/ and each RUNTIME_DIRS folder of rt (default RUNTIME)."""
    rt = Path(rt) if rt else RUNTIME
    return [rt] + [rt / d for d in RUNTIME_DIRS]


def install_side(p):
    """Whether the runtime source p is compiled per install (INSTALL_SIDE)."""
    p = Path(p)
    return p.is_relative_to(RUNTIME) and p.relative_to(RUNTIME).as_posix() in INSTALL_SIDE


def runtime_object(out, src):
    """obj/runtime/<path below runtime>.o, so platform/win32.c cannot collide with a top-level win32.c."""
    src = Path(src)
    rel = src.relative_to(RUNTIME) if src.is_relative_to(RUNTIME) else Path(src.name)
    return out / 'obj' / 'runtime' / rel.with_suffix('.o')


def module_glue(layout, linked, hashes, paths, version):
    """C for build/<version>/modules.c: the layout table, the linked modules' function tables (dispatch.h), the
    expected ExeFS hashes, the game version and the default game paths (host.h)."""
    out = [f'// Module glue written by tools/build_game.py; do not edit. {GENERATED_MARKER}',
           '#include "dispatch.h"', '#include "host.h"', '#include "layout.h"', '']
    for name, base, size in layout:
        u = name.upper()
        out.append(f'_Static_assert(MOD_{u}_BASE == {base:#x}ull && MOD_{u}_SIZE == {size:#x}ull, '
                   f'"layout.h and layout.json disagree on {name}");')
    out += ['', 'const RtLayoutEntry rt_layout[] = {']
    out += [f'    {{"{n}", MOD_{n.upper()}_BASE, MOD_{n.upper()}_SIZE}},' for n, _, _ in layout]
    out += ['};', f'const int rt_layout_count = {len(layout)};', '']
    for name in linked:
        out += [f'extern const uint64_t {name}_fn_addrs[];', f'extern const GuestFn {name}_fn_ptrs[];',
                f'extern const uint64_t {name}_fn_count;']
    out += ['', 'int rt_linked_modules(RtModule *out, int max) {', '    int n = 0;']
    for name in linked:
        u = name.upper()
        out.append(f'    if (n < max) out[n++] = (RtModule){{"{name}", MOD_{u}_BASE, MOD_{u}_SIZE, {name}_fn_addrs, '
                   f'{name}_fn_ptrs, {name}_fn_count}};')
    out += ['    return n;', '}', '']
    out += ['const RtFileHash rt_exefs_sha256[] = {']
    out += [f'    {{{c_string(f)}, {c_string(h)}}},' for f, h in sorted(hashes.items())]
    out += ['};', f'const int rt_exefs_sha256_count = {len(hashes)};', '']
    for name in ('exefs', 'romfs', 'profile'):
        out.append(f'const char rt_default_{name}[] = {c_string(paths[name].as_posix())};')
    number = int(DISPLAY_VERSIONS[version].replace('.', '')) if version in DISPLAY_VERSIONS else 0   # 1.4.3 -> 143
    out += ['const unsigned long long rt_app_pool_bytes = 0xCD500000ull;',   # Switch 1 retail application pool
            'const int rt_nvn_api_version[2] = {55, 15};',   # NVN1: NX2NAVI main requires major 55, minor >= 15
            f'const int rt_game_version = {number};', f'const char rt_port_version[] = {c_string(port_version())};', '']
    return '\n'.join(out)


def port_version():
    """VERSION at the root: the repository's semver, or in a release folder the release string."""
    path = ROOT / 'VERSION'
    return path.read_text(encoding='utf-8').strip() if path.is_file() else 'unknown'


MEMGATE = Path(__file__).resolve().parent / 'memgate.py'
GATE_DIR = 'memgate'                # <out>/memgate: the gate's state folder (memgate.prepare_state) and estimates.json
ESTIMATES = 'estimates.json'
DEFAULT_NEED_C = 400 << 20          # a runtime source's need until its peak has been measured once
DEFAULT_NEED_CXX = 1 << 30
CXX_SUFFIXES = {'.cpp'}
LARGEST_SHOWN = 5
NINJA_GRACE = 5.0                   # seconds Ninja gets to stop its compiles after Ctrl+C before it is killed


def sccache():
    """Whether a compiler cache fronts clang (SCCACHE). Its server runs the compiles, out of the wrapper's process tree,
    so the gate can neither measure nor pace them: build.ninja then leaves the wrapper out."""
    return bool(os.environ.get('SCCACHE'))


def interpreter():
    """This Python for a Ninja command: relative to the root when it lies below it (the usual .venv), else absolute.
    Not resolved: a venv's python is a link whose own path selects the venv."""
    exe = Path(os.path.abspath(sys.executable))
    for root in (ROOT, MAIN_ROOT):
        if exe.is_relative_to(root):
            return cmd_arg(exe.relative_to(root).as_posix())
    return cmd_arg(exe.as_posix())


def gate_command(out):
    """The memory gate's wrapper ahead of a compile command in build.ninja: this Python running tools/memgate.py wrap
    with the state folder <out>/memgate and the object ($out) as the name its need is filed under; '' under SCCACHE.
    It holds nothing that changes between runs, and its paths are relative to the root where they can be; another
    Python still changes it, and with it every compile command."""
    if sccache():
        return ''
    return f'{interpreter()} {cmd_path(MEMGATE)} wrap --state {cmd_path(out / GATE_DIR)} --name $out -- '


def runtime_compiles(out, skip=(), runtime_lib=None, make_lib=False):
    """[(source, object)] of the runtime files the build compiles (see ninja_file): every runtime source but skip;
    make_lib: without INSTALL_SIDE; runtime_lib: only INSTALL_SIDE; modules.c except for make_lib."""
    sources = runtime_sources(skip)
    if make_lib:
        sources = [p for p in sources if not install_side(p)]
    elif runtime_lib:
        sources = [p for p in sources if install_side(p)] + [out / 'modules.c']
    else:
        sources += [out / 'modules.c']
    return [(src, runtime_object(out, src)) for src in sources]


def ninja_file(gen, out, mods, pool_depth, skip=(), prebuilt=False, runtime_lib=None, make_lib=False):
    """build.ninja text, and the object lists per module. With prebuilt the generated objects already exist in
    out/obj and are link inputs without a compile edge. runtime_lib: a prebuilt library replaces the runtime objects
    except INSTALL_SIDE. make_lib: build only that library, from the runtime objects except INSTALL_SIDE."""
    inc = f'-I {cmd_path(RUNTIME)} -I {cmd_path(gen)}'
    vma = f'{cmd_path(DEPS / "include")} -I {cmd_path(DEPS / "include" / "vma")}' if DEPS else cmd_path(vma_include())
    imgui = cmd_path(RUNTIME / 'third_party' / 'imgui')
    dirs = ' '.join(f'-I {cmd_path(d)}' for d in runtime_include_dirs()[1:])
    rt_inc = f'{inc} {dirs} -I {cmd_path(out)} -I {vma} -I {imgui}'   # romfs_config.h, vk_mem_alloc.h (+ deps headers), imgui.h
    vulkan = None if DEPS else vulkan_include()   # deps carries the Vulkan headers itself
    if vulkan:   # first, so that no other folder on the path answers for <vulkan/vulkan.h> (VMA's may be /usr/include)
        rt_inc = f'-I {cmd_path(vulkan)} {rt_inc}'
    libs = [f'-L {cmd_path(DEPS / "lib")}'] + (LLVM_LINK_LIBS if WIN else LINK_LIBS) if DEPS else LINK_LIBS
    # A compiler cache (sccache) may front clang: every lift rewrites the generated chunks with the same contents, so
    # the cached objects save the compile step on the next build.
    cache = f'{cmd_arg(os.environ["SCCACHE"])} ' if os.environ.get('SCCACHE') else ''
    gate = gate_command(out)
    lines = [
        '# Written by tools/build_game.py; do not edit.',
        'ninja_required_version = 1.10',
        f'builddir = {ninja_path(out)}',
        f'cc = {cache}{cmd_arg(CLANG.as_posix())}',
        f'genflags = {" ".join(GEN_FLAGS)} {inc}',
        f'rtflags = {" ".join(RT_FLAGS)} {rt_inc}',
        f'ldflags = {" ".join(LINK_FLAGS)}',
        f'libs = {" ".join(libs)}',
        '',
        'pool gen',
        f'  depth = {pool_depth}',
        '',
        'rule cc_gen',
        f'  command = {gate}$cc $genflags -MD -MF $out.d -c $in -o $out',
        '  deps = gcc',
        '  depfile = $out.d',
        '  description = CC $in',
        '',
        'rule cc_rt',
        f'  command = {gate}$cc $rtflags -MD -MF $out.d -c $in -o $out',
        '  deps = gcc',
        '  depfile = $out.d',
        '  description = CC $in',
        '',
        'rule rc',
        f'  command = {cmd_arg(WINDRES.as_posix())}{"" if "llvm" in WINDRES.name else " " + WINDRES_PP} '
        f'-I {cmd_path(RUNTIME)} -O coff $in -o $out',
        '  description = RC $in',
        '',
        'rule link',
        '  command = $cc $ldflags @$out.rsp $libs -o $out',
        '  rspfile = $out.rsp',
        '  rspfile_content = $in_newline',
        '  description = LINK $out',
        '',
    ]
    objs = {}
    pgo_dep = f' | {ninja_path(PGO_INPUT)}' if PGO_INPUT else ''
    for mod, files in mods.items():
        objs[mod] = []
        for f in files:
            o = out / 'obj' / mod / (f.stem + '.o')
            if not prebuilt:
                lines.append(f'build {ninja_path(o)}: cc_gen {ninja_path(f)}{pgo_dep}')
                lines.append('  pool = gen')
            objs[mod].append(o)
    rt = []
    for src, o in runtime_compiles(out, skip, runtime_lib, make_lib):
        if src.suffix == '.rc':   # windres writes no depfile: the files nx2saria.rc embeds are listed by hand
            lines.append(f'build {ninja_path(o)}: rc {ninja_path(src)} | {ninja_path(RUNTIME / "nx2saria.manifest")} '
                         f'{ninja_path(RUNTIME / "nx2saria.ico")}')
        else:
            lines.append(f'build {ninja_path(o)}: cc_rt {ninja_path(src)}{pgo_dep}')
        rt.append(o)
    objs['runtime'] = rt
    every = [o for v in objs.values() for o in v]
    exe = out / EXE
    if make_lib:
        exe = out / LIB_NAME
        lines += ['', 'rule ar', f'  command = {cmd_arg(AR.as_posix())} rcs $out $in', '  description = AR $out', '', f'build {ninja_path(exe)}: ar ' + ' '.join(ninja_path(o) for o in every),
                  'build objects: phony ' + ' '.join(ninja_path(o) for o in every), f'default {ninja_path(exe)}', '']
        return '\n'.join(lines), objs, exe
    rule = 'link'
    if runtime_lib:
        # Whole archive: the runtime's weak defaults (nvn_procs.c, nvn_api_core.c, vk_sync.c) would otherwise satisfy
        # references before the linker pulls the members with the strong definitions, which would stay out of nx2saria.exe.
        rule = 'link_lib'
        lines += ['', 'rule link_lib',
                  f'  command = $cc $ldflags @$out.rsp -Wl,--whole-archive {cmd_path(runtime_lib)} -Wl,--no-whole-archive $libs -o $out',
                  '  rspfile = $out.rsp', '  rspfile_content = $in_newline', '  description = LINK $out']
    lines += ['', f'build {ninja_path(exe)}: {rule} ' + ' '.join(ninja_path(o) for o in every) +
              (f' | {ninja_path(runtime_lib)}' if runtime_lib else ''),
              'build objects: phony ' + ' '.join(ninja_path(o) for o in every), f'default {ninja_path(exe)}', '']
    return '\n'.join(lines), objs, exe


def main_objects(gen, main_out, rt_dir, mods):
    """({relative object path: the main build's object}, '') when the main build's generated objects can stand in for
    ours, else (None, why). That holds when the main build.ninja compiles the same sources with the same flags, its
    objects are up to date, and every header they include from runtime/ is byte-identical in rt_dir."""
    main_ninja = main_out / 'build.ninja'
    if not main_ninja.is_file():
        return None, f'{flag_path(main_ninja)} not found'
    if gen != (ROOT / 'generated' / main_out.name).resolve():
        return None, 'generated C directory is not the main one'
    lines = main_ninja.read_text(encoding='utf-8').splitlines()
    sources = dict(l[6:].split(': cc_gen ') for l in lines if l.startswith('build ') and ': cc_gen ' in l)
    flags = next((l for l in lines if l.startswith('genflags = ')), '')
    if not flags.startswith('genflags = ' + ' '.join(GEN_FLAGS) + ' -I '):
        return None, 'main build uses other compile flags'
    pairs = {}
    for mod, files in mods.items():
        for f in files:
            o = Path('obj') / mod / (f.stem + '.o')
            theirs = main_out / o
            if sources.get(ninja_path(theirs)) != ninja_path(f):
                return None, f'main build has no edge for {f.name}'
            if not theirs.is_file():
                return None, f'main object {flag_path(theirs)} is missing'
            pairs[o] = theirs
    # Ninja runs in the main checkout: the main build.ninja is relative to it, and the worktree's runtime/ files carry newer
    # mtimes that would make every object look stale. Header equality is checked byte for byte below.
    env = dict(os.environ, PATH=str(UCRT) + os.pathsep + os.environ.get('PATH', '')) if WIN else dict(os.environ)
    targets = [ninja_path(t) for t in pairs.values()]
    for i in range(0, len(targets), 200):
        r = subprocess.run([str(NINJA), '-f', str(main_ninja), '-n', '-d', 'explain'] + targets[i:i + 200], cwd=MAIN_ROOT,
                           env=env, capture_output=True, text=True)
        if r.returncode or 'no work to do' not in r.stdout:
            return None, f'main generated objects are stale ({flag_path(main_out)} needs a build)'
    r = subprocess.run([str(NINJA), '-f', str(main_ninja), '-t', 'deps'], cwd=MAIN_ROOT, env=env, capture_output=True,
                       text=True)
    ours = {flag_path(t) for t in pairs.values()}
    headers, cur = set(), False
    for l in r.stdout.splitlines():
        if l.startswith(' '):
            if cur:
                headers.add(l.strip())
        else:
            cur = l.split(': #deps')[0] in ours
    main_rt = MAIN_ROOT / 'runtime'
    for h in sorted(headers):
        p = Path(h) if Path(h).is_absolute() else MAIN_ROOT / h
        try:
            rel = p.resolve().relative_to(main_rt)
        except ValueError:
            continue
        mine = rt_dir / rel
        if not mine.is_file() or mine.read_bytes() != p.read_bytes():
            return None, f'header {rel.as_posix()} differs in {flag_path(rt_dir)}'
    return pairs, ''


def link_objects(out, pairs):
    """Hardlink the main build's objects into out. Clang and Ninja replace an object by renaming a new file over it,
    so a link keeps the old bytes after a main rebuild. Copies where linking fails."""
    for o, theirs in pairs.items():
        dst = out / o
        dst.parent.mkdir(parents=True, exist_ok=True)
        if dst.exists():
            if os.path.samefile(dst, theirs):
                continue
            dst.unlink()
        try:
            os.link(theirs, dst)
        except OSError:
            shutil.copy2(theirs, dst)


def unlink_shared(out, mods):
    """Drop generated objects still hardlinked to another build, so the next compile writes a new file."""
    for mod, files in mods.items():
        for f in files:
            o = out / 'obj' / mod / (f.stem + '.o')
            if o.exists() and o.stat().st_nlink > 1:
                o.unlink()


MANIFEST = 'objects.sha256'


def file_sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb', buffering=0) as f:
        buf = bytearray(1 << 20)
        while n := f.readinto(buf):
            h.update(memoryview(buf)[:n])
    return h.hexdigest()


def read_manifest(path):
    """{object path below obj/: (sha256, size, mtime_ns)}; empty when the file is missing."""
    man = {}
    if path.is_file():
        for line in path.read_text(encoding='utf-8').splitlines():
            sha, size, mtime, rel = line.split(' ', 3)
            man[rel] = (sha, int(size), int(mtime))
    return man


def write_manifest(path, man):
    write_if_changed(path, ''.join(f'{sha} {size} {mtime} {rel}\n' for rel, (sha, size, mtime) in sorted(man.items())))


def check_objects(out, objects, man, record):
    """Compare objects with the manifest man. An object whose size and mtime match its entry is hashed and must equal
    it; any other object was just (re)built, so with record its hash enters man. Returns (verified, bad, unrecorded):
    the objects with a wrong hash, and with record False the ones without a current entry."""
    verified, bad, unrecorded = 0, [], []
    for o in objects:
        rel = o.relative_to(out / 'obj').as_posix()
        st = o.stat()
        e = man.get(rel)
        if e and e[1] == st.st_size and e[2] == st.st_mtime_ns:
            if file_sha256(o) == e[0]:
                verified += 1
            else:
                bad.append(o)
        elif record:
            man[rel] = (file_sha256(o), st.st_size, st.st_mtime_ns)
            verified += 1
        else:
            unrecorded.append(o)
    return verified, bad, unrecorded


def ninja_build(base, targets, env):
    for i in range(0, len(targets), 200):
        code = subprocess.run(base + targets[i:i + 200], cwd=ROOT, env=env).returncode
        if code:
            sys.exit(code)


def verify_objects(out, objs, borrowed, main_out, pairs, base, env):
    """Verify every object before the link; see the module docstring. borrowed: the hardlinked generated objects of
    main_out (checked against its manifest, never recorded); the others belong to out."""
    t0 = time.monotonic()
    owned = [o for mod, v in objs.items() for o in v if not (borrowed and mod != 'runtime')]
    shared = [o for mod, v in objs.items() for o in v if borrowed and mod != 'runtime']
    man_path = out / 'obj' / MANIFEST
    man = read_manifest(man_path)
    verified, bad, _ = check_objects(out, owned, man, True)
    recompiled = 0
    if bad:
        for o in bad:
            print(f'build_game: object {flag_path(o)} does not match its hash; deleting it to recompile')
            o.unlink()
        ninja_build(base, [ninja_path(o) for o in bad], env)
        recompiled = len(bad)
        verified += check_objects(out, bad, man, True)[0]
    write_manifest(man_path, man)
    if shared:
        main_man = read_manifest(main_out / 'obj' / MANIFEST)
        theirs = [pairs[o.relative_to(out)] for o in shared]
        v, bad, unrecorded = check_objects(main_out, theirs, main_man, False)
        verified += v
        if bad:
            for b in bad:
                print(f'build_game: shared object {flag_path(b)} does not match its hash in '
                      f'{flag_path(main_out / "obj" / MANIFEST)}', file=sys.stderr)
            sys.exit(f'build_game: run tools/build_game.py for {flag_path(main_out)} (it recompiles them), then rebuild '
                     f'{flag_path(out)}')
        if unrecorded:
            print(f'build_game: warning: {len(unrecorded)} shared objects have no current hash in '
                  f'{flag_path(main_out / "obj" / MANIFEST)}; build {flag_path(main_out)} once to record them')
    print(f'objects verified: {verified}, recompiled after a hash mismatch: {recompiled} ({time.monotonic() - t0:.1f} s)')


# ---- The memory gate: what each compile needs, before Ninja; what it took, after

def chunk_costs(gen, mod):
    """{file name: (cost, switch_fn)} of one module's generated C, from its manifest's chunk_info (recomp/gen.py):
    "cost" (the bytes of ordinary C the compile is worth) and "switch_fn" (the bytes of the largest function with a
    jump-table branch, which drives the compiler's peak) when given, else "bytes" and 0. Files it does not list are
    absent (the caller takes their size)."""
    try:
        data = json.loads((gen / mod / 'manifest.json').read_text(encoding='utf-8'))
        entries = (data.get('chunk_info') or []) if isinstance(data, dict) else []
    except (OSError, ValueError):
        entries = []
    out = {}
    for e in entries:
        if not isinstance(e, dict) or not isinstance(e.get('file'), str):
            continue
        cost, switch_fn = e.get('cost'), e.get('switch_fn', 0)
        if not isinstance(cost, int) or cost <= 0:
            cost, switch_fn = e.get('bytes'), 0
        if isinstance(cost, int) and cost > 0:
            out[Path(e['file']).name] = (cost, switch_fn if isinstance(switch_fn, int) and switch_fn > 0 else 0)
    return out


class Compile(collections.namedtuple('Compile', 'src src_bytes cost switch_fn default')):
    """One compile through the gate: its source, the source's size, its cost and switch_fn (the size model's inputs; 0
    for a runtime source) and the need to take until it has been measured (None: the size model's)."""


def gate_compiles(gen, out, mods, prebuilt, runtime):
    """{object name as the wrapper sees $out: Compile} of every compile build.ninja runs through the gate. mods: the
    generated C per module (compiled unless prebuilt); runtime: runtime_compiles() (the .rc goes to windres, not the
    gate)."""
    todo = {}
    if not prebuilt:
        for mod, files in mods.items():
            costs = chunk_costs(gen, mod)
            for f in files:
                size = f.stat().st_size
                cost, switch_fn = costs.get(f.name, (size, 0))
                todo[flag_path(out / 'obj' / mod / (f.stem + '.o'))] = Compile(f, size, cost, switch_fn, None)
    for src, o in runtime:
        if src.suffix == '.rc':
            continue
        size = src.stat().st_size if src.is_file() else 0
        default = DEFAULT_NEED_CXX if src.suffix in CXX_SUFFIXES else DEFAULT_NEED_C
        todo[flag_path(o)] = Compile(src, size, 0, 0, default)
    return todo


def gate_needs(estimates, todo):
    """({name: bytes} for needs.json, Counter of how each need was estimated)."""
    needs, hows = {}, collections.Counter()
    for name, c in todo.items():
        need, how = estimates.need(name, c.src_bytes, c.cost, c.switch_fn, default=c.default)
        needs[name] = need
        hows[how] += 1
    return needs, hows


def gate_off_why(args):
    """Why the gate is off ('--no-mem-gate' or the environment variable's setting), '' when it is on."""
    if getattr(args, 'no_mem_gate', False):
        return '--no-mem-gate'
    value = os.environ.get(memgate.ENV_GATE, '').strip()
    return f'{memgate.ENV_GATE}={value}' if value.lower() in ('off', '0', 'no', 'false') else ''


def gate_start_line(settings, jobs, hows, off):
    """The line before Ninja on the memory gate."""
    if sccache():
        return (f'memory gate: off: the compiles run in sccache\'s server (SCCACHE), out of its reach; up to {jobs} '
                'compiles at once')
    if not settings['enabled']:
        return f'memory gate: off ({off}); up to {jobs} compiles at once'
    ceiling = 'no ceiling' if settings['ceiling'] is None else f'ceiling {memgate.gib(settings["ceiling"])} GiB'
    line = (f'memory gate: on; up to {jobs} compiles at once, at least {min(jobs, settings["min_jobs"])}; keeps '
            f'{memgate.gib(settings["reserve"])} GiB free; {ceiling}; estimates: {hows["learned"]} learned from '
            f'earlier builds, {hows["model"]} from the size model')
    if hows['default']:
        line += (f', {hows["default"]} set at {DEFAULT_NEED_C >> 20} MiB (C) or {DEFAULT_NEED_CXX >> 30} GiB (C++) '
                 'until measured')
    return line


def learn_peaks(estimates, todo, peaks):
    """Record in estimates the peak of each successful compile in peaks (memgate.read_peaks lines) whose object is in
    todo. [(name, peak, seconds)] of those."""
    measured = []
    for p in peaks:
        name, c = p.get('name'), todo.get(p.get('name'))
        peak, seconds = p.get('peak'), p.get('seconds')
        if c is None or p.get('rc') != 0 or not isinstance(peak, int) or peak <= 0 or \
                not isinstance(seconds, (int, float)):
            continue
        estimates.record(name, peak, seconds, c.src_bytes, c.cost, c.switch_fn)
        measured.append((name, peak, seconds))
    return measured


def gate_summary_lines(peaks, measured, estimates, sampler, enabled):
    """The compile phase's closing lines on memory, from the wrappers' peaks.jsonl lines (peaks), the compiles
    recorded from them (measured: learn_peaks), the estimates and the TreeSampler that watched the phase."""
    failed = sum(1 for p in peaks if p.get('rc') != 0)
    lines = [f'memory gate: {len(peaks)} compiles ran' + (f', {failed} failed' if failed else '')]
    if enabled:
        held = [p for p in peaks if isinstance(p.get('held'), (int, float)) and p['held'] > 0]
        if held:
            longest = max(held, key=lambda p: p['held'])
            lines.append(f'memory gate: {len(held)} compiles held for memory, '
                         f'{sum(p["held"] for p in held):.1f} s in all; longest {longest["held"]:.1f} s '
                         f'({longest.get("name")})')
        else:
            lines.append('memory gate: no compile was held for memory')
    parts = []
    if sampler is not None and sampler.peak_at is not None:
        parts.append(f'peak {memgate.gib(sampler.peak)} GiB for Ninja and all compiles together '
                     f'(at {sampler.peak_at:.0f} s)')
    if sampler is not None and sampler.lowest_avail is not None:
        parts.append(f'least free memory {memgate.gib(sampler.lowest_avail)} GiB')
    if parts:
        lines.append('compile memory: ' + ', '.join(parts))
    top = sorted(measured, key=lambda m: m[1], reverse=True)[:LARGEST_SHOWN]
    if top:
        lines.append('largest compiles: ' + ', '.join(f'{name} {peak / GiB:.2f} GiB {seconds:.0f} s'
                                                      for name, peak, seconds in top))
    st = estimates.stats()
    if st['measured'] and st['median'] and st['worst']:
        # stats() gives peak / estimate; the line says estimate / peak, so the lowest is the worst under-estimate
        lines.append(f'estimates against the measured peaks: median {1 / st["median"]:.2f}x, lowest '
                     f'{1 / st["worst"]:.2f}x ({st["worst_name"]})')
    return lines


class TreeSampler:
    """Every poll seconds between start(pid) and stop(): the most memory resident in the process tree under pid
    (Ninja, the gate's wrappers and the compilers) and the least free memory, for the compile phase's summary."""

    def __init__(self, rss=memgate.rss_now, avail=memgate.available, poll=memgate.POLL,
                 clock=time.monotonic):
        self._rss, self._avail, self.poll, self._clock = rss, avail, poll, clock
        self.peak, self.peak_at, self.lowest_avail = 0, None, None
        self._stop = threading.Event()
        self._thread = None
        self._t0 = None

    def start(self, pid):
        self._t0 = self._clock()
        self._thread = threading.Thread(target=self._run, args=(pid,), name='build-sampler', daemon=True)
        self._thread.start()

    def sample(self, pid):
        try:
            rss, avail = self._rss(pid), self._avail()
        except Exception:   # a reading that fails only loses one sample
            return
        if rss > self.peak:
            self.peak, self.peak_at = rss, self._clock() - self._t0
        if avail is not None and (self.lowest_avail is None or avail < self.lowest_avail):
            self.lowest_avail = avail

    def _run(self, pid):
        while not self._stop.wait(self.poll):
            self.sample(pid)

    def stop(self):
        self._stop.set()
        if self._thread is not None:
            self._thread.join()


# ---- Windows job object: peak memory of a process tree
class IoCounters(ctypes.Structure):
    _fields_ = [(n, ctypes.c_ulonglong) for n in ('r_ops', 'w_ops', 'o_ops', 'r_bytes', 'w_bytes', 'o_bytes')]


class BasicLimits(ctypes.Structure):
    _fields_ = [('PerProcessUserTimeLimit', ctypes.c_longlong), ('PerJobUserTimeLimit', ctypes.c_longlong),
                ('LimitFlags', wt.DWORD), ('MinimumWorkingSetSize', ctypes.c_size_t),
                ('MaximumWorkingSetSize', ctypes.c_size_t), ('ActiveProcessLimit', wt.DWORD),
                ('Affinity', ctypes.c_size_t), ('PriorityClass', wt.DWORD), ('SchedulingClass', wt.DWORD)]


class ExtendedLimits(ctypes.Structure):
    _fields_ = [('BasicLimitInformation', BasicLimits), ('IoInfo', IoCounters),
                ('ProcessMemoryLimit', ctypes.c_size_t), ('JobMemoryLimit', ctypes.c_size_t),
                ('PeakProcessMemoryUsed', ctypes.c_size_t), ('PeakJobMemoryUsed', ctypes.c_size_t)]


JOB_OBJECT_EXTENDED_LIMIT_INFORMATION = 9
JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE = 0x2000
CREATE_SUSPENDED = 0x4


def run_measured(cmd, env, on_start=None):
    """Run cmd in its own job object. Returns (exit code, seconds, peak bytes of one process, peak bytes of all).
    Off Windows: the largest resident set of one child process, and 0 for all of them (no job accounting).
    on_start(pid) is called once the process runs."""
    if not WIN:
        t0 = time.monotonic()
        proc = subprocess.Popen(cmd, cwd=ROOT, env=env)
        try:
            if on_start is not None:
                on_start(proc.pid)
            code = proc.wait()
        except BaseException as e:
            # Ctrl+C reached Ninja too (same process group); anything else asks it to stop the same way. Ninja then
            # ends its compiles; it is killed if it has not stopped within NINJA_GRACE.
            if not isinstance(e, KeyboardInterrupt):
                proc.send_signal(signal.SIGINT)
            try:
                proc.wait(NINJA_GRACE)
            except (subprocess.TimeoutExpired, KeyboardInterrupt):
                proc.kill()
                proc.wait()
            raise
        peak = resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss * 1024   # KiB on Linux
        return code, time.monotonic() - t0, peak, 0
    k32 = ctypes.WinDLL('kernel32', use_last_error=True)
    k32.CreateJobObjectW.restype = wt.HANDLE
    k32.CreateJobObjectW.argtypes = [ctypes.c_void_p, wt.LPCWSTR]
    k32.SetInformationJobObject.argtypes = [wt.HANDLE, ctypes.c_int, ctypes.c_void_p, wt.DWORD]
    k32.QueryInformationJobObject.argtypes = [wt.HANDLE, ctypes.c_int, ctypes.c_void_p, wt.DWORD, ctypes.c_void_p]
    k32.AssignProcessToJobObject.argtypes = [wt.HANDLE, wt.HANDLE]
    k32.CloseHandle.argtypes = [wt.HANDLE]
    ntdll = ctypes.WinDLL('ntdll')
    ntdll.NtResumeProcess.argtypes = [wt.HANDLE]

    job = k32.CreateJobObjectW(None, None)
    info = ExtendedLimits()
    info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE   # Ctrl+C stops every compile
    k32.SetInformationJobObject(job, JOB_OBJECT_EXTENDED_LIMIT_INFORMATION, ctypes.byref(info), ctypes.sizeof(info))
    t0 = time.monotonic()
    # Suspended until it is in the job, so the compilers it starts are counted from the first one.
    proc = subprocess.Popen(cmd, cwd=ROOT, env=env, creationflags=CREATE_SUSPENDED)
    if not k32.AssignProcessToJobObject(job, int(proc._handle)):
        print(f'build_game: cannot track memory (error {ctypes.get_last_error()})', file=sys.stderr)
    ntdll.NtResumeProcess(int(proc._handle))
    try:
        if on_start is not None:
            on_start(proc.pid)
        code = proc.wait()
    except KeyboardInterrupt:
        k32.CloseHandle(job)
        raise
    dt = time.monotonic() - t0
    k32.QueryInformationJobObject(job, JOB_OBJECT_EXTENDED_LIMIT_INFORMATION, ctypes.byref(info),
                                  ctypes.sizeof(info), None)
    k32.CloseHandle(job)
    return code, dt, info.PeakProcessMemoryUsed, info.PeakJobMemoryUsed


def pe_sections(path):
    """{section name: virtual size} and SizeOfImage of a PE32+ file."""
    data = path.read_bytes()[:4096]
    pe = int.from_bytes(data[0x3C:0x40], 'little')
    nsec = int.from_bytes(data[pe + 6:pe + 8], 'little')
    opt = pe + 24
    image = int.from_bytes(data[opt + 56:opt + 60], 'little')
    table = opt + int.from_bytes(data[pe + 20:pe + 22], 'little')
    out = {}
    for i in range(nsec):
        s = data[table + 40 * i:table + 40 * (i + 1)]
        out[s[:8].rstrip(b'\0').decode(errors='replace')] = int.from_bytes(s[8:12], 'little')
    return out, image


def elf_sections(path):
    """{section name: size} of the allocated sections of an ELF64 file, and the extent they span in memory."""
    with open(path, 'rb') as f:
        head = f.read(64)
        shoff = int.from_bytes(head[0x28:0x30], 'little')
        shentsize, shnum, shstrndx = (int.from_bytes(head[o:o + 2], 'little') for o in (0x3A, 0x3C, 0x3E))
        f.seek(shoff)
        table = f.read(shentsize * shnum)
        sh = [table[i * shentsize:(i + 1) * shentsize] for i in range(shnum)]
        f.seek(int.from_bytes(sh[shstrndx][0x18:0x20], 'little'))
        names = f.read(int.from_bytes(sh[shstrndx][0x20:0x28], 'little'))
    out, lo, hi = {}, None, 0
    for s in sh:
        flags = int.from_bytes(s[0x08:0x10], 'little')
        addr, size = int.from_bytes(s[0x10:0x18], 'little'), int.from_bytes(s[0x20:0x28], 'little')
        if not flags & 2 or not size:   # SHF_ALLOC
            continue
        name = names[int.from_bytes(s[0:4], 'little'):].split(b'\0', 1)[0].decode(errors='replace')
        out[name] = out.get(name, 0) + size
        lo = addr if lo is None else min(lo, addr)
        hi = max(hi, addr + size)
    return out, hi - (lo or 0)


def objdump_tool():
    """The disassembler for tools/check_isa.py: llvm-objdump of --toolchain, else MSYS2's or the one on PATH."""
    suffix = '.exe' if WIN else ''
    for d in (TOOLCHAIN_BIN, UCRT if WIN else None):
        for name in ('llvm-objdump', 'objdump'):
            if d and (d / (name + suffix)).is_file():
                return str(d / (name + suffix))
    return check_isa.find_objdump()


def mib(n):
    return f'{n / (1 << 20):,.1f} MiB'


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--version', default='v121', help='game version (default v121)')
    ap.add_argument('--cpu', choices=('v2', 'v3'), default='v3',
                    help='target CPU level: v3 (AVX2/FMA, default) or v2 (SSE4.2, for older CPUs; slower)')
    ap.add_argument('--gen', type=Path, help='generated C directory (default generated/<version>)')
    ap.add_argument('--out', type=Path, help='build directory (default build/<version>)')
    ap.add_argument('--modules', help='comma-separated subset of the layout modules (default all)')
    ap.add_argument('--jobs', type=int, help='most compiles at once (default: one per processor); the memory gate '
                                             'starts each one when the free memory holds it')
    ap.add_argument('--compile-only', action='store_true', help='compile, do not link')
    ap.add_argument('--runtime', type=Path, help='runtime source directory (default runtime)')
    ap.add_argument('--full-build', action='store_true',
                    help="compile the generated C even where build/<version>'s objects could be reused")
    ap.add_argument('--skip-runtime', default='',
                    help='comma-separated runtime source file names (cfg.c, not cfg/cfg.c) to leave out (the link needs another definition of what they '
                         'define)')
    ap.add_argument('--exefs', type=Path,
                    help="nx2saria.exe's default ExeFS directory (default <work dir>/extracted/<version>/exefs, the work "
                         "dir being the [work] dir of config.local.toml, else build)")
    ap.add_argument('--romfs', type=Path, help="nx2saria.exe's default RomFS directory with romfs.bin and index.json "
                                               '(default <work dir>/extracted/<version>/romfs)')
    ap.add_argument('--no-romfs', action='store_true',
                    help='build even when the RomFS directory has no index.json (the exe then stops at start)')
    ap.add_argument('--profile', type=Path, help="nx2saria.exe's default profile directory (default build/profile for v121, "
                                                 "build/profile-<version> otherwise, so trying another version leaves "
                                                 "the main profile alone; saves load on 1.2.1 and 1.4.3 alike)")
    ap.add_argument('--toolchain', type=Path, help='llvm-mingw directory (with bin/clang.exe) to build with instead of '
                                                   'MSYS2 UCRT64 clang; on Linux an LLVM directory (bin/clang, '
                                                   'bin/llvm-ar) instead of the clang on PATH')
    ap.add_argument('--ninja', type=Path, help='ninja.exe to build with (default: MSYS2 UCRT64, else bin/ninja.exe of the '
                                               'release folder)')
    ap.add_argument('--sysroot', type=Path, help='Linux: the glibc headers and libraries to compile and link against '
                                                 'instead of the host\'s (the release\'s llvm/sysroot)')
    ap.add_argument('--deps', type=Path, help=f'with --toolchain: include/, lib/ and bin/ of Vulkan, SDL3, Opus, libvpx '
                                              f'and VMA (default {DEFAULT_DEPS.relative_to(ROOT).as_posix()})')
    ap.add_argument('--make-runtime-lib', action='store_true',
                    help=f'build only <out>/{LIB_NAME} from runtime/ (no generated C, no nx2saria.exe)')
    ap.add_argument('--runtime-lib', type=Path, help=f'link against this {LIB_NAME} instead of compiling runtime/ '
                                                    f'({", ".join(sorted(INSTALL_SIDE))} and modules.c still compile)')
    ap.add_argument('--pgo', metavar='gen|FILE',
                    help='profile-guided optimisation, generated code at -O2: gen builds an instrumented nx2saria.exe '
                         '(needs --toolchain on Windows: MSYS2 clang ships no profile runtime); FILE builds with that '
                         '.profdata (tools/pgo.py merge)')
    ap.add_argument('--expected', type=Path,
                    help='expected hashes (default tools/data/expected-<display version>.json)')
    memgate.add_arguments(ap)
    args = ap.parse_args()
    set_cpu(args.cpu)

    gen = (args.gen or ROOT / 'generated' / args.version).resolve()
    out = (args.out or ROOT / 'build' / args.version).resolve()
    if args.pgo:
        if args.pgo == 'gen' and WIN and not args.toolchain:
            sys.exit('--pgo gen: MSYS2 UCRT64 clang ships no profile runtime (clang_rt.profile); pass --toolchain with '
                     'an llvm-mingw directory')
        set_pgo(args.pgo, out)
    extracted = None if args.exefs and args.romfs else work_dir() / 'extracted' / args.version
    paths = {'exefs': (args.exefs or extracted / 'exefs').resolve(),
             'romfs': (args.romfs or extracted / 'romfs').resolve(),
             'profile': (args.profile or ROOT / 'build' / ('profile' if args.version == 'v121'
                                                           else f'profile-{args.version}')).resolve()}
    has_romfs = (paths['romfs'] / 'index.json').is_file()
    if not (has_romfs or args.no_romfs or args.make_runtime_lib or args.compile_only):
        sys.exit(f'{paths["romfs"] / "index.json"} not found: without the RomFS the game stops at start. Run '
                 'tools/extract.py (it writes extracted/<version> into the [work] dir of config.local.toml), pass '
                 '--romfs DIR, or --no-romfs to build without it')
    expected = args.expected
    if not expected:
        if args.version not in DISPLAY_VERSIONS:
            sys.exit(f'no expected hashes known for {args.version}; pass --expected')
        expected = ROOT / 'tools' / 'data' / f'expected-v{DISPLAY_VERSIONS[args.version]}.json'
    hashes = json.loads(Path(expected).read_text())['exefs_sha256']
    global CLANG, AR, WINDRES, NINJA, TOOLCHAIN_BIN, DEPS
    if args.ninja:
        NINJA = args.ninja.resolve()
    elif not NINJA.is_file() and (ROOT / 'bin' / ('ninja' + ('.exe' if WIN else ''))).is_file():
        NINJA = ROOT / 'bin' / ('ninja' + ('.exe' if WIN else ''))   # a setup release folder (docs/installer.md)
    dlls = SDL3_DLLS
    suffix = '.exe' if WIN else ''
    if args.toolchain:
        TOOLCHAIN_BIN = (args.toolchain / 'bin').resolve()
        CLANG = TOOLCHAIN_BIN / f'clang{suffix}'
        AR = TOOLCHAIN_BIN / f'llvm-ar{suffix}'
        WINDRES = TOOLCHAIN_BIN / 'llvm-windres.exe'
    if args.toolchain and WIN or args.deps and (args.toolchain or not WIN):   # Linux: --deps only, never deps-mingw
        DEPS = (args.deps or DEFAULT_DEPS).resolve()
        dlls = sorted((DEPS / 'bin').glob('*.dll')) if WIN else []
    if DEPS and not WIN:
        # A release (--deps): Opus, dav1d, libvpx and zstd go into the executable, so it needs no more than libc,
        # Vulkan and the libSDL3.so.0 beside it
        for lib in ('opus', 'dav1d', 'vpx', 'zstd'):
            LINK_LIBS[LINK_LIBS.index(f'-l{lib}')] = f'-l:lib{lib}.a'
    if args.sysroot and not WIN:
        # Every compile and the link: the game then needs no newer glibc than the sysroot's (the Steam Runtime's)
        sysroot = ['--target=x86_64-linux-gnu', f'--sysroot={cmd_path(args.sysroot.resolve())}']
        GEN_FLAGS[:0] = sysroot
        RT_FLAGS[:0] = sysroot
        LINK_FLAGS[:0] = sysroot
    for tool in (CLANG, NINJA):
        if not tool.is_file():
            sys.exit(f'{tool} not found: install ' + ('MSYS2 UCRT64 clang, lld and ninja' if WIN else 'clang, lld and ninja')
                     + ' (docs/building.md)')
    if not (gen / 'layout.json').is_file():
        sys.exit(f'{gen / "layout.json"} not found: run the generator first')

    layout = read_layout(gen)
    names = [n for n, _, _ in layout]
    wanted = args.modules.split(',') if args.modules else names
    unknown = [m for m in wanted if m not in names]
    if unknown:
        sys.exit(f'unknown modules {unknown}; the layout has {names}')
    linked = [m for m in names if m in wanted]
    # --make-runtime-lib builds runtime/ only, so it never reads the generated chunks
    mods = {} if args.make_runtime_lib else {m: chunk_files(gen, m) for m in linked}

    jobs = max(1, args.jobs or os.cpu_count() or 1)
    missing = [n for n in names if n not in hashes]
    if missing:
        sys.exit(f'{expected}: no exefs_sha256 for {missing}')
    write_if_changed(out / 'modules.c', module_glue(layout, linked, hashes, paths, args.version))
    if has_romfs:
        count, size, dt = romfs_index.write(paths['romfs'], out)
        print(f'romfs index: {count} files, {mib(size)}, {dt:.1f} s')
    else:
        romfs_index.write_placeholder(out)
        print(f'build_game: warning: no {paths["romfs"] / "index.json"}; {EXE} will have no RomFS', file=sys.stderr)
    copy_romfs_patches(args.version, out)
    global RUNTIME
    RUNTIME = (args.runtime or RUNTIME).resolve()
    build_shaders.build(RUNTIME)   # the runtime's own shaders: the headers the sources below include, when missing or stale
    skip = {s for s in args.skip_runtime.split(',') if s}
    main_out = (ROOT / 'build' / args.version).resolve()
    pairs = None
    if args.make_runtime_lib:
        mods, linked, why = {}, [], ''
    elif args.full_build:
        why = '--full-build'
    elif args.toolchain:
        why = f'--toolchain {flag_path(args.toolchain)}'
    elif out == main_out:
        why = ''
    else:
        pairs, why = main_objects(gen, main_out, RUNTIME, mods)
    if pairs is not None:
        link_objects(out, pairs)
        print(f'build_game: reusing {len(pairs)} generated objects of {flag_path(main_out)} (hardlinks)')
    else:
        if out != main_out:   # forks hold hardlinks to the main build's objects; the main build keeps its own
            unlink_shared(out, mods)
        if why:
            print(f'build_game: compiling all generated C: {why}')
    runtime_lib = args.runtime_lib.resolve() if args.runtime_lib else None
    if runtime_lib and not runtime_lib.is_file():
        sys.exit(f'{runtime_lib} not found: build it with --make-runtime-lib')
    # The library leaves INSTALL_SIDE out, so a release that ships without one of them links with undefined symbols.
    missing = sorted(n for n in INSTALL_SIDE if Path(n).name not in skip and not (RUNTIME / n).is_file())
    if runtime_lib and missing:
        sys.exit(f'build_game: {", ".join(missing)} missing from {flag_path(RUNTIME)}: the release must ship every '
                 f'INSTALL_SIDE source beside {runtime_lib.name}')
    text, objs, exe = ninja_file(gen, out, mods, jobs, skip, pairs is not None, runtime_lib,
                                 args.make_runtime_lib)
    write_if_changed(out / 'build.ninja', text)
    gate_dir = out / GATE_DIR
    gate_settings = memgate.settings(args)
    estimates = memgate.Estimates.load(gate_dir / ESTIMATES)
    todo = gate_compiles(gen, out, mods, pairs is not None, runtime_compiles(out, skip, runtime_lib,
                                                                             args.make_runtime_lib))
    gated = not sccache()
    hows = collections.Counter()
    if gated:
        needs, hows = gate_needs(estimates, todo)
        memgate.prepare_state(gate_dir, jobs, gate_settings, needs)

    env = dict(os.environ)
    if WIN or TOOLCHAIN_BIN:
        env['PATH'] = str(TOOLCHAIN_BIN or UCRT) + os.pathsep + os.environ.get('PATH', '')
    base = [str(NINJA), '-f', str(out / 'build.ninja'), '-j', str(jobs)]
    nfiles = sum(len(v) for v in mods.values())
    print(f'build_game: {nfiles} generated files in {len(linked)} modules ({", ".join(linked)}), {jobs} jobs', flush=True)
    print(gate_start_line(gate_settings, jobs, hows, gate_off_why(args)), flush=True)

    sampler = TreeSampler()
    try:
        code, dt, peak_one, peak_all = run_measured(base + ['objects'], env, on_start=sampler.start)
    finally:
        sampler.stop()
    print(f'compile: {dt:.1f} s, peak {mib(peak_one)} per process' + (f', {mib(peak_all)} all processes' if WIN else ''))
    peaks = memgate.read_peaks(gate_dir) if gated else []
    measured = learn_peaks(estimates, todo, peaks)
    if measured:
        try:   # the estimates only help the next build: a file that cannot be written must not stop this one
            estimates.save()
        except OSError as e:
            print(f'memory gate: the compile estimates were not saved ({e}); the next build estimates from the sizes')
    if peaks:
        for line in gate_summary_lines(peaks, measured, estimates, sampler, gate_settings['enabled']):
            print(line)
    if code:
        sys.exit(code)
    for mod, files in objs.items():
        size = sum(o.stat().st_size for o in files)
        print(f'  {mod:8} {len(files):5} objects {mib(size):>14}')
    verify_objects(out, objs, pairs is not None, main_out, pairs, base, env)
    if args.cpu == 'v2':
        # Inline assembly, target attributes and intrinsics pass -march=x86-64-v2 by; one vmovdqa of a YMM register
        # stops a CPU without AVX (tools/check_isa.py)
        fail = check_isa.check_objects(objs['runtime'], objdump_tool(), out / 'obj' / 'runtime' / 'isa-v2.json')
        if fail:
            sys.exit(f'build_game: {fail}')
    if args.compile_only:
        return
    if args.make_runtime_lib:
        exe.unlink(missing_ok=True)   # ar rcs would keep members of deleted sources
    code, dt, peak_one, _ = run_measured(base + [flag_path(exe)], env)
    print(f'link: {dt:.1f} s, peak {mib(peak_one)}')
    if code:
        print(f'build_game: link failed, {flag_path(exe)} is the old one (a running nx2saria.exe from this dir locks it)',
              file=sys.stderr)
        sys.exit(code)
    if args.make_runtime_lib:
        print(f'{below_root(exe)}: {mib(exe.stat().st_size)}')
        return
    for dll in dlls:   # dynamic SDL3 (zlib) and its libiconv (LGPL, unmodified DLL), found next to nx2saria.exe
        shutil.copy2(dll, exe.parent / dll.name)
    sections, image = pe_sections(exe) if WIN else elf_sections(exe)
    print(f'{below_root(exe)}: file {mib(exe.stat().st_size)}, '
          f'image {mib(image)}; ' + ', '.join(f'{k} {mib(v)}' for k, v in sections.items() if k.startswith('.') and v))
    if image > 3 * GiB // 2:
        print('build_game: warning: image over 1.5 GiB; RIP-relative code reaches only 2 GiB', file=sys.stderr)


if __name__ == '__main__':
    main()
