# Model heaps that run full

Ocarina of Time 1.1.1 can crash a few frames after a save is loaded: the thread that rebuilds a model's materials
("OptimizeChangeBank") takes memory from a small heap of that model's own, does not check the result, and writes
through a null pointer when the heap is full. The patch set `heap_retry` (`recomp/patches-v111.txt`,
`runtime/heap_retry.c`) catches each of those allocations and takes the memory from the heap's parents instead. Game
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
- When no heap has room, the game ends with a fatal error that says which allocation it was and which heap refused it.
  Without the set it would crash writing to guest address `0x8`.
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

## Risks

- **[I]** The parents now give memory to this thread. The game's own allocations from those heaps come from other
  threads, so the heaps must take calls from several threads. They do for the game's own use.
- **[I]** A model heap destroyed without the material block's destructor would leave blocks a parent gave allocated
  there. The session's total in the log shows how much went to parents.
