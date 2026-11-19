# Contributing

## The one hard rule

Nothing that came out of the game goes into the repository: no game files, keys, extracted data, recompiled C, compiled binaries, shaders, shader caches, fonts, audio or movies. Not in commits, not in issues, not in pull requests. Only information about the game goes in: addresses, hashes, patch code we wrote, shader and bind-state identifiers.

Our own shaders go in as source only (`runtime/shaders/`, `tools/make_dlaa_mask.py`, `tools/make_mvobj_fs.py`); the build compiles them (`tools/build_shaders.py`), and the same checks refuse a compiled one.

Two checks enforce this:

- CI runs `tools/check_content.py` on every push and pull request.
- The same check runs before each local commit once you enable the hook:

  ```sh
  git config core.hooksPath .githooks
  ```

A failing check is never bypassed with `--no-verify`. If it flags a file by mistake, change the check in a separate pull request and say why.

Links to game files, keys or firmware in issues or pull requests get the issue closed and the content removed.

## Code from other projects

Code may be copied only from projects under MIT, BSD, ISC, zlib or Apache-2.0. Each copy keeps its original header and gets a row in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) with the source commit.

GPL code is never copied, and none goes into the game binary. Unicorn (GPL) is fine as a separate test tool.

## Art from other projects

Art goes into the repository only when its licence is clearly free (CC0 or an equivalent public-domain dedication) and
its origin is recorded. Its folder holds a `SOURCE.md` (author, pack and version, the page it came from with an archived
copy, the licence as that page states it, the date it was checked, the download's SHA-256) and a `SHA256SUMS` of every
picture, which go in unmodified. `tools/check_content.py` refuses a picture of `prompt-art/` (the button prompt art)
that is not listed or differs from its SHA-256. Art of unknown or restricted licence stays out of the repository and
of the releases.

## Reporting bugs

Open an issue at https://github.com/Larbaco/NX2Saria/issues; its forms ask for the details below. Include the port version and game update from the title line of the settings menu (F1), your GPU and driver, the log (`logs\nx2saria.log` in the game folder; after a crash, copy it before you start the game again), and the steps to reproduce. Screenshots are fine; recordings of long gameplay are not needed.

## Pull requests

- One topic per pull request.
- Describe the problem, then what changed.
- Say how you tested it: which version, which scene, how long.
- A change to the lifter (`recomp/`) or the CPU headers passes the differential tests in [docs/building.md](docs/building.md), "Checks".
- Commit messages are plain sentences, without `feat:` or other prefixes.
- AI-assisted code is welcome. You are responsible for having read and run it.

## Code and docs

- Code, comments, commit messages and documentation are in English.
- The player docs are also translated into Brazilian Portuguese, Russian and Simplified Chinese. A correction to a
  translation, a review by a speaker of its language, or a new language is welcome: [docs/translating.md](docs/translating.md)
  says how, and `python tools/docs_i18n.py check` checks links, anchors and the language links before you push.
- Generated code is never edited by hand: change the generator.
