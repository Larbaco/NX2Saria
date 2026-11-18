# The build's memory gate

The game's build compiles about 730 C files. Until 0.18 the build sized its job count once, at the start, from the
free memory at 768 MiB a compile (`tools/make_game.py`), or from a fixed budget at 768 MiB a job
(`tools/build_game.py`), and then ran that many compiles whatever happened next. That number was too small where
memory was plentiful and could still be too large: the free memory changes during a build (a browser, the RomFS step
beside the compile, another build), and one compile can need three times another.

Now every compile starts through a memory gate (`tools/memgate.py`):

- A compile starts when the memory free right now, less what the compiles already running may still grow into, less a
  reserve kept for the rest of the machine, holds the compile's estimated peak.
- At least one compile always runs (`--min-jobs`, default 1), so a build on a small machine still finishes.
- `--jobs` is the most compiles at once (default: one per logical processor).
- The free memory is `MemAvailable` on Linux, capped by the process's cgroup limit, and the available physical memory
  on Windows, capped by its job object's limit (`tools/hostmem.py`). When it cannot be read, compiles start only while
  their estimates and the reserve fit in the machine's RAM.
- The gate never stops a running compile. When the free memory falls below the reserve, it only starts nothing new.

## Estimates that learn

Each compile's peak resident memory is measured exactly when it ends (`wait4` on Linux; on Windows the larger of the
peak working set and the peak commit) and kept per object. The next build estimates each object by its last peak
plus 15 %, as long as its source changed size by less than 20 %. An object never measured is estimated from the size
model below, scaled by how far the model was off for the objects that were measured.

Where the estimates live: `<game>/build-cache/obj/compile-memory.json` (make_game), `<out>/memgate/estimates.json`
(build_game). Deleting the file only makes the next build start from the model again.

## The size model, and what makes a compile costly

Measured on the Linux bench (clang 20.1.2, `-O1 -march=x86-64-v3`, one compile at a time, game 1.1.1's lift):

| file | C | functions | peak | time |
|---|---|---|---|---|
| ordinary chunks | 3.0 to 3.9 MB | 250 to 1,700 | 270 to 340 MiB | 5.6 to 11.6 s |
| a small chunk | 0.01 MB | 4 | 119 MiB | 0.4 s |
| one function with a switch | 3.91 MB | 1 | 922 MiB | 77.9 s |
| one function with a switch | 2.80 MB | 1 | 663 MiB | 40.6 s |
| one function with a switch | 1.79 MB | 1 | 465 MiB | 19.8 s |
| one function without one | 2.48 MB | 1 | 276 MiB | 6.0 s |
| `main_table.c` | 14.5 MB of tables | 0 | 582 MiB | 4.8 s |

Size alone does not make a compile costly: a function that holds a jump-table switch (`br` through a table) does. Its
compile time grows with the square of its size, about 4.5 s per MB², where the rest of the code costs about 2.2 s per
MB. So:

- peak memory ≈ 120 MiB + 50 MiB per MB of C + 150 MiB per MB of the chunk's largest function with a switch;
- compile time ≈ 0.4 s + 2.2 s per MB of C + 4.5 s per MB² of each function with a switch.

`recomp/gen.py` writes both inputs into each module's `manifest.json` (`chunk_info`: `cost`, the bytes of ordinary C
the chunk's compile time is worth, and `switch_fn`, the bytes of its largest function with a switch). The builds start
the costliest compiles first: by their measured time once known, else by cost.

## Chunks by cost

`recomp/gen.py` closes a chunk at about `--chunk-bytes` (3,000,000) of estimated cost instead of size, and gives a
function whose cost passes `--big-fn` (300,000) a file of its own. A function's cost is its estimated size, plus
size² / 490,000 for one that may hold a switch (2.2 s per MB over 4.5 s per MB²). Simulated on game 1.1.1 with the
model above, the costliest chunk of several functions drops from 8.1 s to 7.0 s, and the chunks go from 548 to 580;
the build's total compile time stays the same (3,121 s against 3,134 s of predicted single-core time). The costly
compiles are the single giant functions with a switch, which no grouping can split; the gate's order starts them
first.

Only which file holds each function changes: every function's text stays byte for byte the same.
`tools/lift_compare.py OLD NEW` proves it for two lift folders (each function's text, the function tables, the
layout and patch headers, and that every call has its prototype).

## Options

Both `tools/make_game.py` and `tools/build_game.py` take them; each environment variable stands for its option when
the option is not given.

| option | environment | default | what it does |
|---|---|---|---|
| `--jobs N` | | processors | the most compiles at once |
| `--min-jobs N` | `NX2SARIA_BUILD_MIN_JOBS` | 1 | compiles that may always run, whatever the free memory |
| `--mem-reserve GIB` | `NX2SARIA_BUILD_MEM_RESERVE` | 1.5 GiB or 10 % of RAM, the larger | memory kept free for the rest of the machine |
| `--mem-limit GIB` | `NX2SARIA_BUILD_MEM_LIMIT` | none | the most the running compiles may be estimated to take together |
| `--no-mem-gate` | `NX2SARIA_BUILD_MEM_GATE=off` | on | start compiles by the job count alone; nothing then limits them by memory |

## In `build_game.py` (Ninja)

Each compile rule runs `python tools/memgate.py wrap --state <out>/memgate --name $out -- <compiler>`. The wrappers
share a ledger in `<out>/memgate` (under a file lock) with first-come order, so a big compile waiting for memory is not
passed by the small ones Ninja keeps starting. A wrapper held for more than 10 s says so on its compile's output.
Each build folder has its own gate: two builds at once see each other only through the free memory. With `SCCACHE`
the compiles run in sccache's server, so the commands stay as they were and the gate is off. The rule command names
the Python that runs the wrapper, so a build with another Python, and the first build after this change, compile every
object again.

## The build log

make_game's compile step prints these lines (build_game prints the same ones after its compile phase). From the bench
build of 2026-11-18 at 8 jobs with `NX2SARIA_BUILD_MEM_LIMIT=2`, the second build of the folder:

```
compiles at a time: up to 8 (--jobs); the memory gate starts each one when the free memory holds it (keeps 3.1 GiB free, at most 2.0 GiB estimated in all)
  memory gate: on; up to 8 at once, at least 1; keeps 3.1 GiB free; ceiling 2.0 GiB; estimates: 761 learned from earlier builds, 0 from the size model
  memory gate: holding main_main_0300.o (needs 0.7 GiB; 20.2 GiB free, 1.8 GiB promised to the 2 compiles running, reserve 3.1 GiB, limit 2.0 GiB)
  ...
  761 compiled, 0 unchanged since the last build
  memory gate: 761 compiles started, at most 8 at once (cap 8); reserve 3.1 GiB, limit 2.0 GiB
  memory gate: 380 compiles held for memory, 804.3 s in all; longest 46.4 s (main_main_0300.o)
  compile memory: peak 1.7 GiB for all compiles together (at 319 s), least free memory 16.3 GiB
  largest compiles (5 of 761): main_main_0310.o 0.91 GiB 78 s, main_main_0312.o 0.65 GiB 46 s, main_main_0300.o 0.63 GiB 35 s, main_main_table.o 0.57 GiB 5 s, main_main_0266.o 0.52 GiB 28 s
  estimates against the measured peaks: median 1.15x, lowest 1.10x (main_main_0479.o)
```

- `estimates:` says where the estimates came from: learned from earlier builds, the size model, or the 400 MiB (C)
  and 1 GiB (C++) set for the runtime's files until they are measured.
- Hold lines come at most once per 15 s, with a count of the ones left out.
- `estimates against the measured peaks` is the estimate over the measured peak: below 1 means an object took more
  than its estimate, which the reserve covers.

## Measured on the bench

Linux bench, 16 threads, 31 GiB of RAM, clang 20.1.2, `xbuild/build-branch.sh` with its 8 jobs, one build at a time,
2026-11-18. Before: main at 88a7ef11; after: this change. The compile figures are each build's compile of every
object from scratch; the free memory and the compilers' resident memory were sampled once a second outside the build.

| | before | after | after, learned, 2 GiB ceiling |
|---|---|---|---|
| lift | 96 s (the PC busy with other work) | 75 s | done before |
| files compiled | 729 | 761 | 761 |
| compile | 564 s | 555 s | 881 s |
| compilers' memory together, peak | 2.9 GiB | 3.3 GiB (gate: 3.3 GiB) | 1.7 GiB (gate) |
| least free memory while compiling | 17.1 GiB | 17.7 GiB (gate: 17.6 GiB) | 16.3 GiB (gate) |
| compiles held for memory | (no gate) | 0 | 380, 804 s in all |
| estimates against peaks | | median 1.17x, lowest 0.46x | median 1.15x, lowest 1.10x |

- At 8 jobs on this PC the gate never had to hold a compile: 8 compiles take about 3 GiB and 17 GiB were free. The
  compile time is the same. What changes is what happens where memory is short: there the gate starts fewer compiles
  instead of running the PC out of memory, as the run with a 2 GiB ceiling shows.
- The first build estimates from the model. Its worst miss was main_0029, a 0.8 MB function without a switch: 399 MiB
  measured against 161 MiB estimated, well inside the reserve. The second build used the measured peaks, and no
  estimate fell short.
- Single big functions compile slower beside others than alone: the 3.9 MB one took 78 s alone and 112 s among 8.
- The lift used more memory than the compile: the free memory fell to 8.5 GiB while `recomp/gen.py` emitted its
  chunks (its workers are sized by the free memory when it starts, not gated).
- Every function the two lifts define is byte for byte the same (`tools/lift_compare.py`: 249,960 in main, 45,907 in
  sdk, 1,547 in subsdk0), and boot and boot map runs of both builds from the same save reach the same place
  (Kokiri Forest, Link at -46.09 118.44 15.39).
