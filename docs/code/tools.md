# tools

The build, packaging and release tools. This page is started: it holds the notes of the code that has had no comments
from the start, the licence parts of the two release scripts. The rest of `tools/` keeps its comments until the area is
extracted ([README.md](README.md#areas)). What ships with which licence is in
[THIRD_PARTY_NOTICES.md](../../THIRD_PARTY_NOTICES.md); `tests/shipped_notices_check.py` keeps it in step with the
scripts ([tests.md](tests.md)).

## `tools/build_game.py`

The developer build of a game version from its generated C (`docs/building.md`). The notes of its code that changed
since the comments rule are here; the rest of the file still has its comments.

### `runtime_compiles()`

The runtime files a build compiles: every runtime source but those `--skip-runtime` names, and the `modules.c` the
build writes. There is no split into a prebuilt library and per-install sources any more: that existed for the setup
that installed the game from a release folder, which is gone; the launcher's prebuilt runtime is
`tools/make_game.py` `make_runtime_lib()`.

### `ninja_file()`

The `build.ninja` text and the object lists per module. With `prebuilt` the generated objects already exist in
`out/obj` and are link inputs without a compile edge. Off Windows the runtime's flags take `-fdeclspec`:
`runtime/third_party/ffx_api/ffx_api.h` declares its entry points `__declspec(dllexport)` without a condition, so
`runtime/vk/vk_ffx.cpp` and `runtime/vk/vk_fg.cpp` do not compile on Linux without it (`tools/make_game.py`
`runtime_cmd()` adds the same flag).

## `tools/make_appimage.py`

`KIT_DIRS` holds `licenses/`: the licence texts `THIRD_PARTY_NOTICES.md` names that are not beside their code (the
shader translator's, MacNativeGames', the AppImage runtime's). They go into the kit, `usr/kit/licenses/`.

`PYTHON_REMOVED` lists the extension modules taken out of the AppImage's Python, each with why. Nothing the launcher,
the kit's tools or the game build imports needs them; `tests/shipped_notices_check.py` checks the list and greps the
imports. `_dbm` is the one so far: it links Berkeley DB 6.0.19, whose Sleepycat licence asks for the complete source of
every program that uses it. `dbm.ndbm` is its only user; `dbm.dumb` still works without it.

`RUNTIME_PINS` is what the Linux runtime's id is made from (`tools/linux_runtime.py` `runtime_id()`): the packages and
the modules taken out, so that a change of either brings the launcher a new runtime. `tools/package_launcher_linux.py`
`main()` takes the same tuple for the runtime zip.

### `removed_modules()`

The files of a Python folder's `PYTHON_REMOVED` modules, in `lib/python3*/lib-dynload/`.

### `main()`

Before anything is built, `tools/python_licences.py` `check_build()` stops on a Python that is not the build its
licence texts are pinned for. After the packages are installed, the removed modules are deleted, and
`pbs_licences()` and `cpython_license()` of that file put the texts of what python-build-standalone links into the
Python, and CPython's incorporated software, into `usr/kit/licenses/python/`.

## `tools/make_launcher.py`

### `build_app()`

Copies `licenses/` into `app/<version>/` beside `runtime/` and `recomp/`, for the same texts. The app zip carries it, so
an update brings it without a new Python runtime.

### `python_texts()`

Writes `app/<version>/licenses/python/cpython-license.rst` (`tools/python_licences.py` `cpython_license()`): CPython's
licences of the software it incorporates, for what the embeddable Python's own `LICENSE.txt` leaves out (expat,
mpdecimal). Then every library `WINDOWS_TEXTS` names has its text, found by one of its lines, in the file that table
says, or the release stops. The texts live in the app zip, so the Python runtime and its id do not change.

## `tools/package_launcher_linux.py`

### `main()`

`APP_DIRS` holds `licenses/`, taken from the AppImage's kit like `toolchain/` and `prompt-art/`, so the Linux app zip
carries the same texts as the kit, the Python's among them.

## `tools/python_licences.py`

The licence texts of the libraries linked into the Python each release ships, taken at release time from pinned
downloads of the same Python version (`tools/make_launcher.py`, `tools/make_appimage.py`). The archives are downloaded
once into `build/dl` and checked against their SHA-256. A new Python version, or a uv that installs another build,
needs new pins here; `tools/make_appimage.py` stops when the Python it is given is another build.

- `CPYTHON_SOURCE`: CPython's source archive of `VERSION`. Its `Doc/license.rst` ("Licenses and Acknowledgements for
  Incorporated Software": OpenSSL, expat, libffi, zlib, libmpdec, mimalloc and the code CPython itself carries) goes
  into `licenses/python/` of both releases as `CPYTHON_TEXT`. The Windows embeddable Python's own `LICENSE.txt` appends
  the texts of bzip2, libffi, Zstandard, OpenSSL's Apache-2.0 and Tcl/Tk, but not those of expat and mpdecimal.
- `PBS_FULL`: python-build-standalone's "full" archive of the build the AppImage's Python is: what
  `.github/workflows/linux-appimage.yml`'s `uv python install VERSION` installs, the "install_only" build of
  `PBS_BUILD`. That build carries CPython's licence alone; the full archive's `python/licenses/` has one text per
  library it links in, and only that folder goes into the AppImage.
- `WINDOWS_TEXTS`: the libraries linked into the Windows embeddable Python (and the Tcl/Tk of `tcltk.msi`), each with
  the release file that holds its text and one line of that text. `None` is a licence that asks for no text with a
  binary: SQLite and libtommath are public domain, liblzma public domain (0BSD since 5.6).
- `LINUX_TEXTS`: the libraries python-build-standalone links into the AppImage's Python, each with its file in the full
  archive's `python/licenses/`. Berkeley DB is not among them: `tools/make_appimage.py` removes `_dbm`, the one module
  that links it (its text still comes with the folder).

### `fetch()`

The pinned file in the cache, downloaded when missing or different, through a `.part` file; a wrong SHA-256 stops.

### `cpython_license()`

`Doc/license.rst` of the source archive, written as `CPYTHON_TEXT` in the folder given.

### `check_build()`

Why a Python folder is not the build `PBS_FULL` describes, or `None`: its `BUILD` file names the python-build-standalone
release and `bin/python3` the version.

### `pbs_licences()`

The full archive's `python/licenses/*` into the folder given, unpacked with the system's `tar --zstd` (the AppImage is
built with Python 3.12, whose `tarfile` reads no zstd); every name `LINUX_TEXTS` lists must be among them.
