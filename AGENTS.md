# NX2Saria

Static recompilation of *Ocarina of Time* (Switch 2, ARM64, game version 1.1.1) into a native Windows x86-64 exe with a Vulkan renderer. Design: [docs/architecture.md](docs/architecture.md). Toolchain, build commands and tests: [docs/building.md](docs/building.md). The players' launcher, setup and updates: [docs/launcher.md](docs/launcher.md). Measuring frame rate: [docs/performance.md](docs/performance.md).

## Game content boundary

The repository's additions are all rights reserved (`LICENSE-ADDITIONS`). It never holds game data: game data lives outside it and every derived file stays in ignored folders.

- The user's game and key paths are in `config.local.toml` (ignored). Pass the keys file path to tools; the tools read it themselves. Key values stay out of your output, logs, commands and files.
- Everything extracted or generated goes under `build/` or `generated/`.
- Every file the code generator emits carries the marker defined as `GENERATED_MARKER` in `tools/check_content.py`.
- Run `python tools/check_content.py --staged` before proposing a commit. It is the same check CI runs.

## Reusing code

`refs/` holds the pinned checkout the build reads (`tools/fetch_refs.sh` recreates it). Read-only.

- Copy only MIT, BSD, ISC, zlib or Apache-2.0 code, keep its header, and add a row to `THIRD_PARTY_NOTICES.md`.
- GPL code is never copied: write our own.
- Art goes in only with a clearly free licence (CC0 or equivalent), with a `SOURCE.md` and a `SHA256SUMS` beside it
  (`prompt-art/`, CONTRIBUTING.md "Art from other projects"); `tools/check_content.py` refuses a picture there that the
  list does not hold.
- `tools/vendor/` stays byte-identical to its source; Windows changes go in `tools/extract.py`.

## Code

- Build C with MSYS2 UCRT64 Clang (`C:\msys64\ucrt64\bin`). Generated C uses GNU extensions, so `cl.exe` is out. Call Ninja from that directory by full path or put it first on PATH.
- Two Python environments: `.venv` (3.12) for the lifter and the build, `.venv-py314` for the extraction and shader scripts, which need `compression.zstd`.
- Generated C is never edited by hand: change the generator (`recomp/`).
- The runtime's own shaders are checked in as source only (`runtime/shaders/`, `tools/make_dlaa_mask.py`,
  `tools/make_mvobj_fs.py`). `tools/build_shaders.py` builds `runtime/<name>_spv.h` from them, `build_game.py` runs it,
  git ignores the headers and `check_content.py` refuses them in a commit.
- Every lifter change passes the differential tests against Unicorn ([docs/building.md](docs/building.md), "Checks"). After changing `recomp/jumptables.py`, `tests\jumptables_check.py` reports 0 disagreements on 1.2.1 and 1.4.3.
- `recomp/layout.py` alone decides module bases.
- Graphics bugs: [docs/maxwell.md](docs/maxwell.md) lists the Maxwell behaviour the Vulkan path must match, what goes wrong when it does not, and what our runtime does.

## No comments in the code

Source code has no comments. What a comment would say (why the code is so, its limits and traps, the game's addresses, measured numbers, Maxwell behaviour, issue and pull request references) lives in `docs/code/<area>.md`, grouped by file and function, or in the feature's own doc when it explains a feature. [docs/code/README.md](docs/code/README.md) lists the areas, the layout of a page and the citation format.

- Before changing an area, read its `docs/code/<area>.md`. A change updates that page in the same pull request. New code adds no comment in any area; its notes go on the area's page, which the change starts if the area has none yet.
- Docs cite our code by file and function (`` `runtime/vk/vk_pipeline.c` `cache_path_init()` ``); the code does not cite the docs. Docs do not name other projects or say where code came from: the license header of copied code and its `THIRD_PARTY_NOTICES.md` row carry that.
- Exceptions, kept in the code: the license header or notice of copied code; the `GENERATED_MARKER` and everything under `generated/`; shebang and Python coding lines; what a tool or compiler reads (`# noqa`, `# needs:` and `# args:` of `tests/run.py`, `// build_shaders: keep functions`, `// clang-format off`, `// NOLINT`, a fall-through marker); the part of a module docstring the program prints (`description=__doc__`), until its area turns it into a plain string. Left as they are: `tools/vendor/`, `runtime/third_party/`, `runtime/shaders/ffx/`, `runtime/shaders/ffx3/` and `runtime/nvn/nvn.h` (unmodified third-party code), `recomp/patches-*.txt` (data the generator reads) and `tools/xlate/` (its comments go with the shader translator's next identity change). `runtime/cpu.h`, `runtime/simd.h` and `runtime/simd_fp.h` keep their comments until they are next changed for another reason: one changed byte in them makes every player's PC recompile the lifted code, and the check names them at every run. docs/code/README.md ("The checks") has the full list and the reasons.
- The comments already in the code move out area by area, one pull request per area in the order docs/code/README.md gives: the notes onto the page in one commit, the comments out of the code in the next, with no change to the code itself. Until the last area, `python tests/source_comments_check.py` only warns and prints the comment lines left per area; the pull request that cleans an area proves it with `--strict <area>`. `python tests/code_docs_check.py` checks that every file and function `docs/code/` cites exists, also as a warning until then.

## Every change leaves a trace in the log

A change to the game (a fix, a feature, a patch set, code brought in from elsewhere) shows in `logs/nx2saria.log`, so a
player's normal session tells whether it is on and working:

- One line at start that says whether it is on and with what value. A patch set says it by its name in the `patches:`
  line.
- When it acts, a line that says what it did, with numbers that can be checked: counts, times, before and after. A
  line that can repeat is limited per session or aggregated.
- A test in the style of `tests/` when one can be written.

How to write a line, the limits and the prefix each part uses: [docs/logging.md](docs/logging.md). A new prefix gets
its row there; `python tests/log_prefixes_check.py` fails without it.

## Writing

Code, docs and commit messages are in English. Commit messages are plain sentences without type prefixes.

The player docs also have translations (`README.<lang>.md`, `docs/<lang>/`), made and kept as
[docs/translating.md](docs/translating.md) says. After changing an English page that has them, run
`python tools/docs_i18n.py status`; update the translations you can write, and leave the others to show as stale.
`python tools/docs_i18n.py check` (in CI) fails on a broken link or anchor in any Markdown file.

## Names

NX2Saria is the product name. New code, settings and release files use `nx2saria.ini`, `NX2SARIA_*` and the `NX2Saria-` prefix. The older names that still work, and the ones kept on purpose (stable markers, format identifiers, upstream provenance), are listed in [docs/branding.md](docs/branding.md). Do not rename those, the fork's repository URL or `THIRD_PARTY_NOTICES.md`.
