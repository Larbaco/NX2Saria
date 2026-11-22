# Model heaps that run full

Ocarina of Time 1.1.1 can crash a few frames after a save is loaded: the thread that rebuilds a model's materials
("OptimizeChangeBank") takes memory from a small heap of that model's own, does not check the result, and writes
through a null pointer when the heap is full. The patch set `heap_retry` (`recomp/patches-v111.txt`,
`runtime/patches/heap_retry.c`) catches each of those allocations and takes the memory from the heap's parents instead. Game
version 1.1.1 only; on by default.

**[C]** marks what was read in the game's code (build `e0f3ccfa`) or seen in a run. **[I]** marks what is inferred.

## What the game does

- **[C]** `main+0x4366e0` rebuilds a material block (`x0`; its second argument, kept at `[sp + 0x38]`, names the heap
  for the CPU parts at `+8`). It takes:
  - two CPU buffers with the heap's `tryAlloc` (vtable `+0x30`, alignment 8), called at `main+0x43690c` and
    `main+0x436da0`. Each result is the base of a bump allocator at block `+0xc8` and `+0xa8`. Nothing checks it for
    null. With a null base the bump allocator hands out guest address 0, and the next store, in `main+0x42cb30`
    (called at `main+0x436d44`), writes guest address `0x8`;
  - GPU memory through `main+0x502050`, called at `main+0x436f18` and `main+0x437094` with a request on the stack
    (`+0` the byte count, `+8` the heap, `+0x20` the name of what it is for). A failure with a heap named goes on with
    no memory pool. A failure leaves nothing allocated. A request without a heap uses the game's default heap.
- **[C]** A heap: `+0x20` its name, `+0x28` its memory, `+0x30` the memory's size, `+0x38` its parent; vtable `+0x30`
  `tryAlloc(heap, size, alignment)`.
- **[C]** Blocks go back where they came from. The material block's destructor (`main+0x91f940`) frees `+0xa8` and
  `+0xc8` through `main+0x1fdea0`, the free by address, which looks up the heap whose range holds the block (it reads
  `+0x28` and `+0x30`). The GPU memory's release (`main+0x5020f4`) uses the same function. **[I]** So a block a parent
  heap gave goes back to that parent. This was read in the code, not traced in a run.
- **[C]** No log on the bench (`branch/*/logs`) and no issue in this repository shows the crash. The address `0x8` and the
  return address `main+0x436d48` are what a report of it would show.

## What the set changes

| Site | Original | Now |
|---|---|---|
| `main+0x436910` | `stp xzr, x21, [x27, #0xd0]` | the same, then `bl page+0xed0` (`heap_retry_cpu`) |
| `main+0x436db4` | `stp xzr, x22, [x27, #0xb0]` | the same, then `bl page+0xed0` |
| `main+0x436f18` | `bl main+0x502050` | `bl page+0xec0` (`heap_retry_gpu`) |
| `main+0x437094` | `bl main+0x502050` | `bl page+0xec0` |

- `heap_retry_cpu` runs right after the `tryAlloc`, before the function stores `x0` as the buffer's base. When `x0` is
  null, it calls `tryAlloc` on each parent up to the root (at most 8) with the same size (`x21` or `x22`) and
  alignment, and puts the first block it gets in `x0`.
- `heap_retry_gpu` calls the game's `main+0x502050`. When that fails and the request names a heap, it calls it again
  with the request's heap set to each parent, then to none (the game's default heap). It then puts the request's heap
  back.
- A parent is used only when the child's memory lies inside the parent's memory. With any other layout the climb stops
  instead of calling through a wrong pointer.
- When no heap has room, the game ends with a fatal error that says which allocation it was, its size and which heap
  refused it, and tells the player what to do: start again and load the save, and report it with the log and the save
  if it stops at the same place. The heaps are the game's own memory, of a fixed size, so the message says that
  closing other programs or lowering graphics settings does not help. Without the set the game would crash writing to
  guest address `0x8`.
- The set runs only on the code it was written for. The generator stops on a site whose word differs from the
  executable's, as for every set. At the start, `heap_retry.c` `heap_retry_start` also compares the four words in the
  loaded image with 1.1.1's, and on a difference it switches the set off and logs which word differs; the game then
  runs its own code there. A mod whose ExeFS patch writes one of these sites switches the set off too
  (`tools/exefs_patches.py`, `runtime/patches/mods.c`: "mods: ... replaces the port's patch set heap_retry; it stays off").
  The start line comes after both, so it says whether the set really runs.
- Why these places: the `blr` to `tryAlloc` cannot be patched (a patch site cannot be a `br`, `blr` or `ret`), so the
  site is the word after it. The `blr` has already made every caller-saved register and the flags free, so the call
  to the host function changes nothing the function still needs.

## Settings and log

| Variable | Meaning |
|---|---|
| `NX2SARIA_HEAP_RETRY` | `0` turns the set off (the game as shipped). Default `1`. |
| `NX2SARIA_HEAP_RETRY_TEST_FAIL` | Test: `<cpu>[,<gpu>]`. The first `<cpu>` CPU buffers and `<gpu>` GPU memories at these sites are refused, as a full heap would refuse them. A refused CPU buffer stays allocated in the model's heap. |
| `NX2SARIA_HEAP_RETRY_TEST_SHIPPED` | Test, with the one above: a refused allocation is not tried again and goes on as shipped, which reproduces the crash. |

The lines in `logs/nx2saria.log`:

```
patches: ..., heap_retry on (page 0x7004ea0000)
heap retry: on: a model's buffer or GPU memory that its full heap refuses is taken from the heap's parents (patch set heap_retry; NX2SARIA_HEAP_RETRY=0 turns it off)
heap retry: model buffer at main+0x43690c, 16792 bytes: heap 0x... "<name>" (64 KB) refused it; given by 0x... "<name>" (16384 KB), 1 level up; so far 1 blocks, 16 KB from other heaps at this site
heap retry: this session (the set on at the end) 1 model buffers (16 KB) and 0 GPU memories (0 KB) taken from other heaps; 1 refusals by a full heap, 0 by the test switch, 0 left as shipped
```

The retry lines stop after 32 a session. After that, each site logs its total every 64th block. The last line comes at
an orderly end (`gfx_present.c` `rt_exit_steps`). A count that keeps growing over a long session would point to blocks
that never go back.

## Checks

- `python tests/heap_retry_check.py` lifts the four sites as the generator does and runs the host functions over
  stand-in heaps (`docs/building.md`).
- On the bench: a run with `boot_map` and `NX2SARIA_HEAP_RETRY_TEST_FAIL` loads a save and forces the refusals.

## Runs

Linux bench (RX 6700, RADV), commit `e0fa4219`, no window and no sound, `boot_map = KokiriForest_Child` from slot 5 of
a fixed save, 30 s of game time each **[C]**:

| Run | Switches | What the log shows |
|---|---|---|
| retry | `NX2SARIA_HEAP_RETRY_TEST_FAIL=3,2` | Five refusals while the area loaded: three CPU buffers (4408, 288 and 35640 bytes) and two GPU memories (256 and 44 bytes), from `"Model/SystemModel.SceneMaterial.bfres"` (36 KB) and `"Model/Terrain.TeraTerrain.bfres"` (177 KB). Each was given by their parent `"NinVirtualAddressHeap"` (2870268 KB), one level up. Link arrived, the run reached its 30 s and ended normally. The end line counts 3 model buffers and 2 GPU memories, 5 refusals by the test switch. No log line the normal run lacks. |
| shipped | `NX2SARIA_HEAP_RETRY_TEST_FAIL=1`, `NX2SARIA_HEAP_RETRY_TEST_SHIPPED=1` | The first CPU buffer refused and left null, as shipped: `FATAL: access violation (0xb) ... writing address 0x8 in lifted function f_700042cb30 = main+0x42cb30`, before Link arrived. This is the crash the set prevents. |
| normal | none | No refusal. Link arrived, the end line counts 0 of everything. |

The first try of the retry run found a bug of its own: `mem_readable` answers 0 for the game thread's stack, so the
function's stack slot read as no heap and the retry ended in its fatal error. The stack slot and the request are now
read as the game reads them (`heap_retry.c` `own64`).

**[I]** In these runs a model's heap hangs directly under `NinVirtualAddressHeap`, of about 2.7 GiB, so a block the
model's heap has no room for finds room one level up.

## Risks

- **[I]** The parents now give memory to this thread. The game's own allocations from those heaps come from other
  threads, so the heaps must take calls from several threads. They do for the game's own use.
- **[I]** A model heap destroyed without the material block's destructor would leave blocks a parent gave allocated
  there. The session's total in the log shows how much went to parents.
