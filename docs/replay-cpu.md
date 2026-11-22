# Where the replay's CPU time goes

The replay turns the game's NVN command buffers into Vulkan command buffers on the thread the game submitted from. In
the measured places it is what sets the frame: the render thread is busy for the whole frame, between the replay and
its wait for the GPU's fence. This page records a profile of that time, made with `NX2SARIA_REPLAY_PHASES=1`, and what
it says about the ways to shorten it.

## The switch

`NX2SARIA_REPLAY_PHASES=1` (default 0; a developer switch with no menu row) times the replay and writes lines every
600 frames. When it is on, the log says at start:

```
vk_exec: replay phases timed (NX2SARIA_REPLAY_PHASES=1): a line for the submitting thread and one for the replay workers every 600 frames
```

When it is off, it writes nothing and costs nothing.

What it times:
- **The ops, by kind and by code:** draws, mesh draws, dispatches, inline uniform updates and the rest. The submitting
  thread is counted apart from the replay workers (`vk_exec.c` `op_one`, `vk_exec_draw.c` `vk_exec_op_draw`).
- **A draw's phases (`vk_exec_draw.c` `prepare_draw_ex`):**
  - the entry: the pass and the scissor;
  - the shader lookup;
  - the pipeline lookup;
  - its bind and dynamic state;
  - the per-draw hooks;
  - the resource binding, with its four parts: texture descriptors, buffer descriptors, the binding signature, and
    the set packing and pushes;
  - the vertex buffers;
  - the rest: the index buffer, the `vkCmdDraw` call, a skipped draw.
- **The parts of each `vk_exec_record` call:**
  - before the walk: the pins' sweep, the event sync, the mirror's begin and the uniform snapshot's begin;
  - the walk, with the parallel groups' peek, barrier and begin, front pass, hand-out and the submitting thread's own
    chunks;
  - the drain to the last barrier;
  - the mirror's and the uniform snapshot's end.

Each clock read costs about a tenth of a microsecond, and a draw takes about a dozen, which the totals include.

## A profile

**The bench:**
- A Linux PC with an 8-core, 16-thread Intel CPU and an RX 6700 (RADV).
- Uncapped, boot map, 80 s, standing at the start position.
- Kokiri Forest, and Hyrule Field in front of the Market.
- The mesh fallback on (`mesh_fallback = 1`), which this card needs on Linux for other reasons.

**The settings:** the replay ran with the defaults a player has: `replay_workers = auto` (3 workers on this CPU),
`replay_pipe = 0` and `replay_small = 0`. A settings file that sets other values gives about the same frame rate here,
but moves the work between the threads, so a profile has to say which it used.

| Per frame, the submitting thread | Kokiri Forest (48 fps) | Hyrule Field (33 fps) |
|---|---|---|
| `vk_exec_record`, in 7 calls | 13.4 ms | 23.9 ms |
| the uniform snapshot's begin | 1.4 ms | 2.3 ms |
| the mirror's begin | 0.4 ms | 0.5 ms |
| draws | 532 × 7.7 µs = 4.1 ms | 1127 × 6.5 µs = 7.3 ms |
| mesh draws through the fallback | 49 × 34 µs = 1.7 ms | 99 × 32 µs = 3.2 ms |
| dispatches | 105 × 7.7 µs = 0.8 ms | 185 × 5.7 µs = 1.0 ms |
| binds and state (15,000 to 30,000 ops at about 0.07 µs) | 0.9 ms | 1.7 ms |
| the parallel groups' front pass | 0.6 ms | 2.3 ms |

**Why so many draws on the submitting thread:** a group is cut into chunks for the workers only from 64 draws (see
below), so most draws are replayed on the submitting thread. That is 67 and 88 draws a submission here, besides the
chunks it records itself beside the workers.

**A draw on the submitting thread, in µs (Kokiri Forest / Hyrule Field):**

| Phase | Kokiri Forest | Hyrule Field |
|---|---|---|
| entry (pass and scissor) | 2.1 | 2.2 |
| shader | 0.5 | 0.5 |
| pipeline | 0.65 | 0.5 |
| state | 0.6 | 0.4 |
| hooks | 0.2 | 0.2 |
| **binding** | **3.9** | **3.2** |
| of it, textures | 0.5 | 0.3 |
| of it, buffers | 1.0 | 0.9 |
| of it, signature | 0.2 | 0.2 |
| of it, sets and pushes | 1.9 | 1.3 |
| vertex buffers and after | 0.4 | 0.4 |
| the rest | 1.5 | 1.0 |

A worker's draw costs 9 to 10 µs with the same shape, except the entry (0.35 µs), since its chunk has the pass open
already.

## What it says

**The cost is spread out.** No single piece of the per-draw path holds most of it.
- **The binding** is the largest part of a draw, but what is left of it is the driver's work:
  - the sets are written and pushed through update templates (`VkPipelineInfo.tpl`);
  - a push equal to the last one is not recorded (`push_skip`);
  - each uniform and storage buffer's resolution is kept per binding (`pb_take` / `pb_keep`).
- **A cache of the sets already written in a command buffer was tried and dropped.** It had 128 entries per draw
  context, keyed by the whole packed signature. It reused only 5% of the sets (33 of 663 a frame in Kokiri Forest), and
  the sets-and-pushes phase stayed at 2.1 µs. Nearly every signature is unique, because the draws differ in the offsets
  of their uniform buffers.
  - **What would remove most writes:** dynamic uniform buffers, with the offset given at bind time. They change every
    pipeline layout, so every player's pipelines would be built again.
- **The mesh fallback is the largest single block:**
  - 1.7 ms a frame in Kokiri Forest and 3.2 ms in Hyrule Field;
  - about 33 µs a mesh draw, since each one ends the pass, runs its compute passes and opens the pass again;
  - it also ends the parallel groups.
  - The native mesh path has none of this.
  - The fallback is the default on AMD's Windows driver for RDNA 1 and 2, on Intel's cards and on cards without mesh
    shaders.
- **The uniform snapshot's begin, 1.4 to 2.3 ms a frame:**
  - it walks the submission's ops a second time to gather the uniform ranges they bind;
  - it then copies about 360 KiB a submission.
  - It is what keeps the replay from reading uniforms the game rewrote meanwhile, so it was left alone.

## The threshold of the parallel groups (tried, not kept)

A group is cut into chunks that the workers and the submitting thread record at once only from 64 draws (two chunks
of 32, `vk_exec.c` `CHUNK_DRAWS`). Each such group costs the submitting thread about 70 µs (the full barrier before it
and the texture pool's sync), and the GPU two full barriers. So a lower threshold puts more draws on the workers, but
each extra group costs more.

**The test:** a threshold of 32 against 64.
- Setup: `tools/bench.py --boot-map`, Kokiri Forest and Hyrule Field, the player's default replay settings, six pairs
  interleaved.
- **Pairs 1 to 3**, before an unrelated compile started on the same PC:
  - Kokiri Forest: 47.6 / 45.8 / 42.8 fps at 64, against 47.2 / 46.8 / 42.5 at 32;
  - Hyrule Field: 42.8 / 42.0 / 40.6 at 64, against 41.5 / 41.5 / 36.4 at 32.
- **Pairs 4 to 6** ran beside the compile and are noise in both directions.

**The result:** no gain over the noise in either place, so the threshold stays at 64.
