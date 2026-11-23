# tools

The build, packaging and release tools. This page is started: it holds the notes of the code that has had no comments
from the start, the licence parts of the two release scripts. The rest of `tools/` keeps its comments until the area is
extracted ([README.md](README.md#areas)). What ships with which licence is in
[THIRD_PARTY_NOTICES.md](../../THIRD_PARTY_NOTICES.md); `tests/shipped_notices_check.py` keeps it in step with the
scripts ([tests.md](tests.md)).

## `tools/make_appimage.py`

`KIT_DIRS` holds `licenses/`: the licence texts `THIRD_PARTY_NOTICES.md` names that are not beside their code (the
shader translator's, MacNativeGames', the AppImage runtime's). They go into the kit, `usr/kit/licenses/`.

## `tools/make_launcher.py`

### `build_app()`

Copies `licenses/` into `app/<version>/` beside `runtime/` and `recomp/`, for the same texts. The app zip carries it, so
an update brings it without a new Python runtime.

## `tools/package_launcher_linux.py`

### `main()`

`APP_DIRS` holds `licenses/`, so the Linux app zip carries the same texts as the AppImage's kit.
