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

make_game's compile step prints (build_game prints the same lines after its compile phase):

```
compiles at a time: up to 8 (--jobs); the memory gate starts each one when the free memory holds it (...)
  memory gate: on; up to 8 at once, at least 1; keeps 3.1 GiB free, no ceiling; estimates: ...
  memory gate: holding <object> (needs ... GiB; ... GiB free, ... GiB promised to the N compiles running, reserve ...)
  ...
  N compiled, M unchanged since the last build
  memory gate: N compiles started, at most K at once; H held for memory, T s in all, the longest ...
  compile memory: peak ... GiB for all compiles together (at ... s), least free memory ... GiB
  largest compiles: main_0282.o 0.92 GiB 78 s, ...
  estimates against the measured peaks: median ...x, lowest ...x (<object>)
```

Hold lines come at most once per 15 s, with a count of the ones left out.
