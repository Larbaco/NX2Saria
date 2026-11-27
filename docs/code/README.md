# Code notes

The source code has no comments ([AGENTS.md](../../AGENTS.md#no-comments-in-the-code)). What a comment would say lives
here, one page per area of the source, grouped by file and by function: why the code is written the way it is, its
limits and traps, the game's addresses, measured numbers, the Maxwell behaviour it matches, and the issues and pull
requests behind it.

- **Implementation notes go here.** A note about how one file or function works, and why, goes on its area's page.
- **A feature's why goes in its feature doc.** What a feature is for, how a player uses it and the design behind it
  stay in the existing doc ([frame-generation.md](../frame-generation.md), [maxwell.md](../maxwell.md),
  [logging.md](../logging.md) and the others); the area's page links to it instead of repeating it.
- **English only.** These pages are not translated: `tools/docs_i18n.py check` refuses a translation of a page here
  ([translating.md](../translating.md)), and still checks their links.
- **Read before you change.** Before changing an area, read its page; a change updates the page in the same pull
  request.

## Areas

Each area has its page. The comments the areas held moved to these pages, one area per pull request, in the order
below, and the code has none now; `tools/xlate/`, the vendored code and the third-party code were never part of it
(see [The checks](#the-checks)). New code puts its notes on its area's page.

| Order | Area | Page | Status |
|---|---|---|---|
| 1 | `runtime/dlssg/`, `runtime/audio/` | [dlssg.md](dlssg.md), [audio.md](audio.md) | extracted |
| 3 | `recomp/` | [recomp.md](recomp.md) | extracted |
| 4 | `runtime/cfg/` | [cfg.md](cfg.md) | extracted |
| 5 | `runtime/pacing/` | [pacing.md](pacing.md) | extracted |
| 6 | `runtime/input/` | [input.md](input.md) | extracted |
| 7 | `runtime/platform/` | [platform.md](platform.md) | extracted |
| 8 | `runtime/nvn/` | [nvn.md](nvn.md) | extracted |
| 9 | `runtime/patches/` | [patches.md](patches.md) | extracted |
| 10 | `runtime/diag/` | [diag.md](diag.md) | extracted |
| 11 | `launcher/` | [launcher.md](launcher.md) | extracted |
| 12 | `runtime/ui/` | [ui.md](ui.md) | extracted |
| 13 | `runtime/` top level | [runtime.md](runtime.md) | extracted; `runtime/cpu.h`, `runtime/simd.h` and `runtime/simd_fp.h` keep their comments until a change next rebuilds the lifted code ([The checks](#the-checks)) |
| 14 | `runtime/gfx/` | [gfx.md](gfx.md) | extracted |
| 15 | `runtime/hle/` | [hle.md](hle.md) | extracted |
| 16 | `tools/` | [tools.md](tools.md) | extracted; `tools/cheats.py`, `tools/exefs_patches.py` and `tools/mod_files.py` keep their comments until a change next alters the lift key of a game folder with mod code ([The checks](#the-checks)) |
| 17 | `tests/` | [tests.md](tests.md): one line per test, without repeating [building.md](../building.md#checks) | extracted |
| 18 | `runtime/shaders/` (not `ffx/`, `ffx3/` and `smaa.glsl`) | [shaders.md](shaders.md) | extracted |
| 19-22 | `runtime/vk/` | [vk-pipeline.md](vk-pipeline.md), [vk-exec.md](vk-exec.md), [vk-memory.md](vk-memory.md), [vk-texture.md](vk-texture.md), [vk-upscalers.md](vk-upscalers.md) | extracted |

When an area is done, its row gets a link to the page and the status "extracted".

### The pages of `runtime/vk/`

`runtime/vk/` is too large for one page. The split below is where each file's notes start; the pull request that
extracts a page may move a file to another one, and the page lists the files it holds.

| Page | Files |
|---|---|
| [vk-pipeline.md](vk-pipeline.md) | pipelines, their caches and the shader modules: `vk_pipeline.c`, `vk_cache_file.h`, `vk_cache_stamp.h`, `vk_build_gate.h`, `vk_play_gate.h`, `vk_warm_hint.h`, `vk_save_retry.h`, `boot_warm.h`, `nvidia_cache.h`, `vk_mesh_fb.c`, `vk_mesh_fb.h`, `vk_mesh_path.h`. Extracted; `vk_pipeline.c` keeps eight comments that five tests cut the source at, until those tests cut at code |
| [vk-exec.md](vk-exec.md) | the device, the replay of the game's command buffers and the synchronisation: `vk.h`, `vk_backend.c`, `vk_device.c`, `vk_caps.c`, `gpu_choice.h`, `vk_exec.c`, `vk_exec.h`, `vk_exec_draw.c`, `vk_exec_xfer.c`, `vk_exec_probe.c`, `vk_rec.c`, `vk_rec.h`, `vk_sync.c`, `vk_sync_hw.h`, `vk_pass_hooks.h`, `vk_index_rewrite.h`, `vk_oq_hold.h`, `vk_target_frames.h`, `vk_frame_owed.h`, `vk_evo_peek.h`, `vk_usnap.c`, `vk_gpa.c`, `vk_gpa.h`, `vk_dump.c`, `vk_crumbs.c` |
| [vk-memory.md](vk-memory.md) | memory and buffers: `vk_memory.c`, `vk_mem_heap.h`, `vk_vma.cpp`, `vk_vma_budget.c`, `vk_headroom.c`, `vk_scratch_lru.h`, `vk_up_defer.h` |
| [vk-texture.md](vk-texture.md) | images, formats and texture decoding: `vk_texture.c`, `vk_formats.c`, `vk_astc.c`, `vk_color.c`, `vk_tex_compact.h`, `vk_tex_detail.h`, `vk_tex_release.h` |
| [vk-upscalers.md](vk-upscalers.md) | upscalers, anti-aliasing, frame generation and the passes after the game's frame: `vk_fsr.c`, `vk_fsr3.c`, `vk_fsr3.h`, `vk_fsr3fg.c`, `vk_fsr3fg.h`, `fsr3_probe.c`, `fsr3fg_probe.c`, `vk_ffx.cpp`, `vk_ffx.h`, `vk_ffx4.cpp`, `vk_ffx4_api.h`, `vk_fg.cpp`, `vk_fg.h`, `vk_fg4.cpp`, `vk_fg_pose.inc`, `vk_fg_reset.h`, `vk_fg_inputs.h`, `vk_fg_set.h`, `vk_fg_stats.h`, `vk_fg_time.c`, `vk_fg_time.h`, `vk_dlaa.c`, `vk_dlaa.h`, `vk_dlssg.c`, `vk_dlssg.h`, `vk_dlssg_device.h`, `vk_aa.c`, `vk_contact.c`, `vk_tonemap.c`, `vk_hdr.c`, `vk_mvcheck.c`, `vk_mvprev.c`, `vk_mvsurvey.c`, `vk_skin_motion.inc`, `skin_pair.h` |

## A page

```markdown
# runtime/audio

What the area does as a whole, in a few lines, and the feature docs that explain it.

## `runtime/audio/audio_mixer.c`

What the file holds and the notes that hold for all of it.

### `mixer_render()`

The notes of one function: why, the limits, the traps, the numbers.
```

A note keeps what the comment had: the game's addresses, the numbers that were measured and how, the issue or pull
request it came from. A comment that only says what the next line does is not moved: it goes.

## Citing the code

The docs cite the code by file and function; the code never cites the docs. A citation is written in one of two forms,
and `tests/code_docs_check.py` reads both:

- **Inline.** A code span with a path from the repository's root, then the functions of that file, each in its own code
  span with `()`, separated by spaces, commas or "and": `runtime/vk/vk_pipeline.c` `cache_path_init()`, or
  `recomp/patches.py` `load()` and `check()`. A path alone cites the file (or the folder, with a final `/`), and a line
  number may follow it as a hint (`runtime/vk/vk_hdr.c:769`), which is not checked.
- **Headings.** A heading that is only a path span names the file for the headings below it; a heading below it that is
  only function spans cites those functions of that file, as in [A page](#a-page). A heading of the same or a higher
  level ends the file's section.

A path starts with one of the source folders (`runtime/`, `recomp/`, `tools/`, `launcher/`, `tests/`).
A function is a name with `()`: `name()`, `Class.method()` in Python, `Class::method()` in C++. A function counts as
there when the file defines it (C and GLSL: a body, a function-like `#define`, or a declaration in a header; Python: a
`def` or `class`, a method inside its class). Not citations, so not checked: a function span not right after a path, a
path with `*`, `<...>` or `{...}` in it (a pattern), and anything inside a fenced block.

- **Another repository.** A section whose paths are in another repository says so with the line
  `<!-- code-docs: other repository -->` (on a line of its own, best right under the heading, with a sentence that
  says which repository for the reader). The check skips the citations from that line to the next heading of the same
  or a higher level; above the page's first heading, to the end of the page.
- **Generated files.** A cited path that does not exist and that git ignores (`git check-ignore`), such as a header the
  build writes, is listed as generated and not checked.

## The checks

Both run in `tests/run.py`, with their planted cases first (`--self-test`), and both fail the run: a comment outside
the exceptions, or a citation of something that does not exist. The two files hold no comments, so their notes are
here.

### `tests/source_comments_check.py`

Counts the comment lines left in the source, per area (`runtime/<folder>`, the top level of `runtime/`, `recomp`,
`tools`, `launcher`, `tests`), prints the table, and exits 1 when a comment is left. With areas or paths as arguments
it looks only there. `--list` prints each line counted, `--kept` each line kept and why, `--exceptions` the list
below.

What it reads: the files git tracks (and new files not ignored) under the source folders, in C, C++, C#, GLSL
(`.comp`, `.frag`, `.vert`, `.glsl`, `.mesh`, `.task`), resource scripts (`.rc`), SPIR-V assembly (`;` comments) and
Python. A comment line is a line that holds any part of a comment; a line of code with a comment after it counts.

Not read: `tools/vendor/` and `runtime/third_party/` (others' code, unmodified), `runtime/shaders/ffx/` and
`runtime/shaders/ffx3/` (AMD FidelityFX, unmodified), `runtime/nvn/nvn.h` (the NVN type definitions, another project's
header, unmodified), `runtime/shaders/smaa.glsl` (the SMAA reference implementation, unmodified),
`recomp/patches-*.txt` (data that `recomp/patches.py` reads),
`generated/`, `build/`, the `*_spv.h` shader headers, any file with the `GENERATED_MARKER`, and `tools/xlate/`, the
shader translator, whose comments go with its next identity change: its identity, `tools/build_xlate.py` `identity()`, reads the patch
files byte for byte, and a new identity makes every player prepare the shaders again.

Kept with their comments for now, and counted and named in a line of their own at every run (`kept with comments: 3
files, ...`), outside their area's count: `runtime/cpu.h`, `runtime/simd.h` and `runtime/simd_fp.h`. They are ours and
their comments are owed: every lifted object depends on them and `tools/make_game.py` compares an object's
dependencies by SHA-256, so one changed byte makes every player's PC recompile the lifted code. The exception ends when
those files are next changed for another reason, in a change that rebuilds the lifted code anyway; their notes are
already on [runtime.md](runtime.md).

The same holds for `tools/cheats.py`, `tools/exefs_patches.py` and `tools/mod_files.py`, in a second line of that
kind: `tools/make_game.py` `mod_digest()` hashes their bytes into the lift key of a game folder that has mod code, so
one changed byte makes the PC of every player with code cheats or ExeFS patches lift the game again. That exception
ends with the next change that alters that key; their notes are already on [tools.md](tools.md).

Kept in the files it reads:

| Kept | Why |
|---|---|
| licence header or notice of copied code | the licence requires it; `THIRD_PARTY_NOTICES.md` lists the files |
| the `GENERATED_MARKER` | the content guard looks for it (`tools/check_content.py`) |
| shebang, Python coding line | the system and Python read them |
| tool and compiler directives | `# noqa`, `# type: ignore`, `# pragma`, `# fmt:`, `# pylint:`; `# needs:` and `# args:` in `tests/` (`tests/run.py` reads them); `// clang-format off`/`on`, `// NOLINT`, `// IWYU pragma:`; `// build_shaders: keep functions` (`tools/build_shaders.py`); a fall-through marker, which GCC's `-Wimplicit-fallthrough` reads (clang does not) |

Every other Python docstring counts as a comment: a function's, a class's, a string on a line of its own. Preprocessor
directives (`#pragma`, `#version`, `#extension`) are code. `//`, `/*` and `#` inside a string or character literal,
a raw string or an `#include <...>` are not comments.

#### `owed()`

The entry of `OWED` that names a file: the files, the date the exception began, what they are, why a changed byte
costs, and when their comments leave. `scan()` keeps such a file's comment lines out of its area and in the report's
`owed` count, which the table's `kept with comments` line and `--exceptions` print.

#### `c_comments()`

The C-family scanner: one pass over the text that steps over string and character literals (with their escapes, and
stopping at an unterminated literal's line end), C++ raw strings, C# verbatim strings, `#include <...>` and the text of
`#error` and `#warning`, and returns each `//` comment (with its backslash continuations) and `/* */` block. On the
tree at the time it was written it found the same comment lines as Pygments' C and C++ lexers in every C-family file.

#### `python_parts()`

Comments come from `tokenize`, so a `#` inside any string, f-strings included, is not one. Docstrings come from `ast`:
every expression statement that is a string constant, each line it spans. A docstring the program prints (`description=__doc__`,
`print(__doc__)`) counts like any other: the text a program shows is a string constant (`DESCRIPTION`, `USAGE`).

#### `license_lines()`

The licence lines of a run of comment lines (or of a docstring), by paragraph: a paragraph with licence text in it
("Permission is hereby granted", "THE SOFTWARE IS PROVIDED", an SPDX line, the zlib terms) is kept whole; a
"Copyright (c) ..." notice is kept from the start of its sentence to the end of that sentence or the line that names
`THIRD_PARTY_NOTICES.md`, unless the sentence goes on after that name (as in "...; see THIRD_PARTY_NOTICES.md): the
spin wait search", whose next line says what the notice covers); the sentence may start on a line above, unless the notice's own line starts it ("Contains
code", "Portions", "Copyright"). A line that names a licence and that licence's file, as in "(MIT, ffx/LICENSE.txt)",
is kept when the file is there, looked for beside the source and from the top of the repository; a path that leads
nowhere keeps nothing. Empty comment lines next to a kept line are kept with it.

#### `classify()`

Sorts each comment line of one file into counted or kept, with the reason.

#### `self_test()`

Planted files in a scratch folder: each kind of line kept, a plain comment and a trailing block counted, comment-like
text in strings and URLs left alone, a docstring the program prints counted like any other, each folder not read, a file
kept with its comments counted on the line of its own and not in its area while a plain comment in the file beside it
still fails, and the check failing on the planted area and passing on one with only directives.

### `tests/code_docs_check.py`

Reads every `docs/code/*.md` (with `--all-docs`, `docs/*.md` too) and lists each cited path that does not exist and
each cited function its file does not define. It exits 1 when there is one. It needs git, for the generated files.

#### `citations()`

The citations of a page as (line, path, function or none), in the two forms of [Citing the code](#citing-the-code),
without those of a section marked as another repository's.

#### `ignored()`

Whether git ignores a path (`git check-ignore -q`): a cited file that does not exist is then a generated one.

#### `defined_in_c()`

Looks for the function in the file with its comments blanked out (`c_comments()` of the comment check): a function-like
`#define`, or the name followed by its parameter list and a body (or, in a header, a `;`), not after `->` or `.` and
not as a call after `return`, `if` or an `=`.

#### `defined_in_python()`

Looks for a `def` or `class` of that name anywhere in the module; `Class.method` wants the method in that class's body.

#### `self_test()`

A planted page with good citations in both forms and one of each fault: a missing file, a function only in a comment,
a function cited after a folder, a missing method, a function heading whose function is gone, a missing file after a
section of another repository; citations the check must not read (a function span after other words, a pattern, a
fenced block, a function heading outside a file's section, the section of another repository); and a file git
ignores, reported as generated.
