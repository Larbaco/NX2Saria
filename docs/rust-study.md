# Would Rust pay off in NX2Saria? (study, 2026-11-20)

Verdict: **not now**. This page records the evidence and the measurements, so the question can be reopened when one of
the triggers at the end shows up.

## Where the pain is

**Bugs.** 248 fix commits touching `runtime/`, `tools/` or `launcher/` (all of the history, 2026-11-05 to 11-19), sorted
by hand from their messages and files:

| Kind | Fixes | Would Rust have prevented it? |
|---|---|---|
| Memory safety in host C (use after free, overrun, null, double free) | 17 | yes, or a clean panic |
| Unlocked shared state (torn reads, check-then-use) | 7 | it forces the lock; the ordering fix is still ours |
| The game's own code or guest memory | 29 | no: the lifted C does what the ARM64 did |
| GPU, Vulkan, Maxwell, shader translation | 81 | no: the Vulkan layer is `unsafe` in any language |
| Logic, settings, UI, platform, build, packaging | 98 | no |
| Python tools and launcher | 16 | 3 at most (two unhandled errors, one thread race) |

The 24 Rust would have caught are about 10%. They cluster in the replay and memory layer (`vk_memory.c`, `vk_exec*.c`,
`vk_sync.c`, `vk_backend.c`, `vk_texture.c`), which talks to Vulkan, guest memory and the driver at every line: a Rust
version would be `unsafe` at those same boundaries. 13 of them came from one review sweep on 2026-11-13.

**Time.** The measured costs, largest first:

1. The player's build: clang compiling the lifted C, 567-685 s of an 11-13 minute build (about 3,100 s of CPU). The
   language of the generator does not change this; emitting Rust instead of C would compile slower.
2. The GPU driver compiling pipelines: 17-47 s on the bench, 28 minutes to 2 hours on an NVIDIA laptop with a cold
   cache (0.19.1). The C# translator is 10-18 s of a cold boot (about 75 ms per shader over 8 processes).
3. Per frame: about 1,100 draws replayed at about 9 µs each (13.6 ms, 92% CPU) in Kokiri Forest. This is algorithmic
   work in C, not something a language change speeds up.
4. The lifter (Python): 87 s of emission over 8 workers for 297,000 functions, about 700 CPU seconds. It runs on the
   player's PC at install and after a change to `recomp/`, never on a plain rebuild.

The other Python steps are small (RomFS index 0.3-0.8 s; shader inventory and seed about 16 s), and the RomFS pack runs
beside the lift and compile. Run-time zstd is already native libzstd, and ASTC decodes on the GPU (`vk_astc.c`); the CPU
decoder is a fallback and the reference.

## Measurements

**ASTC, C against Rust crates.** `runtime/tex_astc.c` (clang -O2) against the two crates.io decoders behind a C ABI
staticlib (opt-level 3, LTO, `panic = "abort"`), the same 2048x2048 images of random valid blocks, one thread, best of 5:

| Block | C (ours) | astc-decode 0.3.1 | texture2ddecoder 0.1.2 |
|---|---|---|---|
| 4x4 | 98 ms | 191 ms (1.9x), 31% of texels differ | 214 ms (2.2x), 62% differ |
| 6x6 | 66 ms | 138 ms (2.1x), 31% differ | 154 ms (2.3x), 62% differ |
| 8x8 | 55 ms | 121 ms (2.2x), 31% differ | 142 ms (2.6x), 62% differ |

Off-the-shelf crates are twice as slow and not bit-exact with astcenc, which ours is. A hand port would go through the
same LLVM and land at about the C speed: no gain to buy.

**The lifter's profile** (cProfile, 4,000 functions, 1.08 million instructions, 56 s): about 52 µs per instruction,
spread flat. The largest share is text work on the emitted C: regular-expression searches (7.5 s), `regs.resolve` (16 s
cumulative) and `flags.rewrite` (7.8 s). A Rust lifter would likely be 20-50 times faster (about 85 s saved per lift),
but a structured representation in Python, instead of rewriting C text with regular expressions, would likely take
much of that for far less.

**Toolchain cost.** A minimal Rust toolchain (rustc, cargo, std for one target) is about 600 MB; with the docs, 2.1 GB.
The test staticlib built clean in 4.7 s and added about 340 KB to a linked executable. The Windows target that fits
MSYS2 UCRT64 clang and lld is `x86_64-pc-windows-gnullvm`; it was not built here.

## The cost of putting Rust in

- **Player's PC.** Nothing Rust may be compiled there. The model is the shader translator: built in CI, shipped as a
  binary, the player needs no .NET. A Rust library in the runtime has a catch: `make_game.py` `prebuilt_runtime`
  recompiles `runtime/` when the clang version differs, and the per-install files are always compiled. A Rust part
  would need its own prebuilt archive, keyed on its own sources, and never a fallback that needs `cargo`.
- **Interop.** A C header by hand or cbindgen, `extern "C"` and `panic = "abort"`; memory crossing the boundary is
  freed on the side that allocated it.
- **CI.** One more toolchain to pin in `windows-release.yml` and `linux-appimage.yml`, crates pinned by `Cargo.lock`
  and vendored for the same-bytes build, under the provenance attestation the release already makes.
- **Licences.** Most crates are MIT or Apache-2.0, which `THIRD_PARTY_NOTICES.md` already accepts; every crate in the
  dependency tree needs a row and a check that none is GPL.
- **Maintenance.** A third language beside C and Python (and C# in the translator), for one owner and AI threads that
  today read and test C and Python.

## What does not go to Rust

The lifted code (generated C; Rust does not help there and would compile slower), the Vulkan and replay layer (bugs
are GPU semantics, the code is `unsafe` either way), the C# translator (the driver is the cost), the launcher (Python,
decided in G23) and its C bootstrap (small, few bugs).

## Cheaper steps first

- Run the Linux runtime under ThreadSanitizer: 11 of the 24 memory bugs were races, and none has a tool today (ASan
  cannot start because of the game's fixed memory windows; TSan may hit the same limit and must be tried).
- Fuzz the RomFS, SARC and BYML readers with libFuzzer, which the clang already in the toolchain has (`6d2cb837` was in
  `sarc.c`).
- Profile and restructure the lifter's text rewriting in Python before thinking about another language for it.

## What would reopen the question

- A new self-contained part that reads untrusted input (a network service, a mod loader reading players' files): write
  it in Rust from the start, shipped prebuilt.
- The lift becoming a pain for players (frequent `recomp/` changes that each mean a long re-lift) after the Python fix
  above: then a pilot of lifter emission in Rust, shipped as a binary, checked against the Python output byte for byte
  on 1.1.1, 1.2.1 and 1.4.3, with the Python lifter kept as the fallback.
- Memory bugs in the host layer still arriving at the same rate once sanitizers and fuzzing run.
