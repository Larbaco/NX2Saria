# runtime/vk: memory and buffers

The memory side of the Vulkan back end: the game's memory pools as Vulkan buffers (host memory imports, shadow storage,
VRAM mirrors and the write-back of what the GPU wrote), the page write generations that decide what is uploaded again,
the upload batch and the memory report (`vk_memory.c`); the allocator and where its allocations land
(`vk_vma.cpp`, `vk_vma_budget.c`, `vk_headroom.c`); and three small rules kept in headers so that a test checks them
without a device (`vk_mem_heap.h`, `vk_scratch_lru.h`, `vk_up_defer.h`).
[README.md](README.md#the-pages-of-runtimevk) lists the pages of `runtime/vk/`.

What the Switch's GPU and NVN do with memory pools that a port must reproduce: [maxwell.md](../maxwell.md). Where the
area sits in the host: "4. Graphics" in [architecture.md](../architecture.md). The lines the memory report writes:
[logging.md](../logging.md). The images and the textures that live in this memory: `vk-texture.md`.

## `runtime/vk/vk_mem_heap.h`

When the memory report asks for the C runtime heap's summary (`runtime/vk/vk_memory.c` `system_log()`,
`plat_heap_info`). Header only, so that `tests/mem_heap_check.c` checks the rule without a device.

The summary holds the heap's lock while it adds the heap up, and every malloc and free of every thread waits for it.
With a heap of a few GiB that took 54 and 70 ms in two sessions (2026-11-14, RTX 5090 PC), and the frame of the first
report, 30 seconds into each session, took 55 to 82 ms: the longest frame of play in both. One slow summary switched the
question off for the rest of the run, so each session paid for it exactly once.

The summary is therefore not asked for in play at all: only before play (the boot and the title screen, where a long
frame is not seen and the heap is small), and no more once one was slow there. `NX2SARIA_MEM_CENSUS=1`, the switch that
already accepts a late frame for the walk of the address space, asks for it at every report. In play the report gives
what the runtime knows it allocated instead, the same figure from one report to the next, which is what the growth
bookkeeping compares.

### `mem_heap_summary_due()`

- `census`: `NX2SARIA_MEM_CENSUS` is on.
- `in_play`: a save's first frame was shown.
- `slow`: a summary took too long before.

## `runtime/vk/vk_vma.cpp`

The implementation of the Vulkan Memory Allocator (VMA), compiled from its header `vk_mem_alloc.h` with the Vulkan
functions bound statically. VMA is C++ only, so its implementation is compiled here as C++; the rest of the runtime uses
the header's C interface. The file's first line is its attribution and stays in the code.

## `runtime/vk/vk_vma_budget.c`

Video memory by the heaps' live budgets, for every allocation through VMA (`runtime/vk/vk.h` maps `vmaCreateImage`,
`vmaCreateBuffer` and `vmaAllocateMemory` here, and the frees that go with them), and the allocations' sums by owner
("owners", below).

An RX 580 (8 GiB, AMD 23.10) has a 6 GiB device-local heap and a 2 GiB device-local, host-visible one. The heap limits
of `runtime/vk/vk_device.c` (`vram_heap_limits`) are fixed when the device is made: each heap's budget then, less a
tenth for the memory VMA does not see. That memory grows later: AMD's FidelityFX (FSR 3.1 upscaling contexts, up to six,
and frame generation with its swapchain and context) allocates its own, outside VMA. With FSR 3.1 and frame generation
on at 1080p it reached 882 MiB, and heap 0 held 4644 MiB of VMA's beside it: 5526 of a 5526 MiB budget, while heap 2 had
1757 MiB free. The next upload submit failed with `VK_ERROR_OUT_OF_DEVICE_MEMORY` and the game ended (2026-11-11). Only
texture images looked at the budget (`runtime/vk/vk_texture.c` `image_alloc()`); render targets, pool memory, mirrors
and the rest were placed in heap 0 up to VMA's fixed limit.

Here each allocation leaves out the memory types of a device-local heap whose use, as the driver reports it
(`VK_EXT_memory_budget`: VMA's budget, which counts the memory VMA does not see), is within the reserve of its budget,
and VMA places it in another device-local heap with room. Only there: on a GPU with one device-local heap (an RTX 3050
Laptop, 4 GiB) leaving that heap out sent every allocation past the reserve to system memory, more than 4096 of them,
and the process's commit with them: with 16 GiB of RAM the system's commit ran out, host memory imports failed and the
device was lost. Without another device-local heap with room the allocation is made as asked. The reserve
(`NX2SARIA_VRAM_RESERVE_MB`; 0, off, by default; the low-end GPU mode set 512 up to 0.10.1) is room left for allocations
VMA does not make, which can come at once: AMD's contexts are made when the upscaler or the frame generation setting
changes. An allocation that finds no room that way is made as asked, as before. Allocations from a VMA pool and those
for one memory type alone (the headroom's anchors, `runtime/vk/vk_headroom.c`) are made as asked.

### `budget_bits()`

The memory types of `bits` that may take a new allocation: those of device-local heaps within reserve of their budget
are left out. Returns `bits` itself when nothing is left out or nothing would be left.

- `n_moved`: allocations placed outside a full heap.
- `full_seen`: heaps already logged as full (bit per heap).
- `dl`: the types of device-local heaps with room among the bits.
- `if (!dl || out == bits) return bits`: no other video memory with room: as asked, not system memory.

### `all_bits()`

Device-local types of `bits` (0: all), from the allocator's properties: whether the request could land in a heap
`budget_bits()` leaves out.

### Owners

Every allocation made through the wrappers below is counted under the owner of the file that made it, the device-local
heaps apart from the others, and leaves the count when `runtime/vk/vk.h`'s `vmaDestroyImage`, `vmaDestroyBuffer` or
`vmaFreeMemory` frees it. The allocation's user data (unused otherwise) holds 1 + owner \* 2 + local, so the free knows
where it was counted; one made before its owner was known, or by a file that does not include `runtime/vk/vk.h`, has
none and is never counted. The memory report (`runtime/vk/vk_memory.c` `owner_log()`) reads the sums: a category that
only climbs over a session is where a leak shows. A count costs one VMA info query and two atomic adds per allocation
and per free.

### `owner_of()`

- `{"vk_texture.c", VK_OWN_TEXTURES}, {"vk_memory.c", VK_OWN_POOLS}`: mirrors, physical pools, staging.

### `vk_vma_allocate_memory()`

- `if (!ai->pool && (bits & (bits - 1)))`: more than one type: one alone is asked for on purpose.

## `runtime/vk/vk_headroom.c`

VRAM headroom: the next VMA block of a memory type is allocated on a background thread, not on the render thread.

VMA places an allocation in an existing block of its memory type and allocates a new block (`vkAllocateMemory`, an
eighth of the default block size up to all of it, `vk_vma_block_size()`) only when none has room. On the render thread
that took 8-19 ms per 256 MiB block, several times in the first minute of play while VRAM grows (textures from
`create_tex`, mirrors). A new `VkDeviceMemory` made by any other thread (VMA's `pfnAllocate` callback) wakes this thread
for its memory type, which it then checks every 500 ms. A check allocates half a default block of that type (the largest
size VMA still places in blocks, not in dedicated memory). When a block has that much room the allocation lands there
and is freed at once. When none has, VMA allocates a new block here, and the allocation stays as that block's anchor:
VMA deletes an empty block at the next free of its type (`VmaBlockVector::Free`, "we have another empty block"), so a
spare block left empty lasted until the next texture went. The previous anchor is freed then, which gives its half block
to the render thread, so at most half a block per memory type is held back.

Only the memory types textures and VRAM mirrors grow in get a spare (`spare_type()`): the device-local types the CPU
cannot map, or on a GPU that has none of those (integrated, every type mappable), the device-local ones. In one session
the others, the host heap's two types (system memory, charged to the commit) and the device-local mappable one (VRAM
behind the PCIe BAR), each got a 256 MiB spare early on and never needed another; to the end the BAR type held 320 MiB
of blocks for 37 MiB of allocations and one host type 288 MiB for 0.1 MiB, as VMA keeps an empty block until the next
free of its type, and these types seldom free anything.

Under VRAM pressure no headroom is kept: the spare block counts against the budget that decides whether a pool gets its
VRAM mirror (`runtime/vk/vk_memory.c`), so while use plus two blocks passes 85% of the budget the anchor goes and
nothing new is allocated. Nor while the system's commit is low (`vk_texture_commit_low`) where device memory counts
against it: on Windows the driver charges device memory to the commit, and a spare block of each type held up to 256 MiB
of it empty when an allocation that failed there ended the game; an integrated GPU's memory is the machine's RAM.
Linux's limits (`plat_commit_counts_vram`) do not count a discrete card's own memory, so dropping its spare would free
nothing they count and only put the block allocations back on the render thread.

`NX2SARIA_VRAM_HEADROOM=0` turns it off.

- `#define HR_SMALL_HEAP (1ull << 30)`: `VMA_SMALL_HEAP_MAX_SIZE`: smaller heaps use blocks of a heap eighth.

`H`:

- `PlatMutex lock`: held around each top-up; `vk_headroom_stop()` takes it before the allocator goes.
- `PlatEvent ev`: auto-reset: another thread allocated, or stop.
- `volatile uint32_t tid`: the headroom thread's id.
- `volatile int64_t types`: memory types a thread other than this one allocated device memory of.
- `volatile int64_t made`: blocks this thread brought in.
- `volatile int64_t other`: device memory of 32 MiB or more other threads allocated (VMA blocks, large dedicated).
- `VmaAllocation anchor[64]`: per memory type: the half block that keeps the newest spare block from being deleted.
- `volatile int64_t held`: bytes of the anchors, for the log's VRAM breakdown (`vk_headroom_held()`).
- `volatile uint32_t hold_until`: `plat_now_ms()`: no spare block before then (`vk_headroom_release()`).

### `vk_vma_block_size()`

`NX2SARIA_VMA_BLOCK_MB` (16 to 256, default 64): the size of VMA's blocks on heaps over 1 GiB (its
`preferredLargeHeapBlockSize`; VMA's own default is 256 MiB). A freed texture or mirror leaves a hole its block keeps,
and the driver counts the whole block against the VRAM budget and the system's commit until its last allocation goes.
With 256 MiB blocks one session held 2.1 GiB free inside them beside 8.2 GiB of allocations after a few area changes,
every block pinned by a texture or mirror that outlived its neighbours; a smaller block empties, and goes back to the
driver, once fewer allocations have gone. Allocations over half a block get memory of their own (VMA), which matches the
32 MiB from which `create_tex` gives images their own memory.

### `spare_type()`

The memory types a spare block is kept for (the file's notes above say why): device-local without host access, or
device-local at all where the device has no such type.

### `on_allocate()`

- `(void)a, (void)m, (void)size, (void)user`: the trace's mark of the allocation, with its time:
  `runtime/vk/vk_device.c` `timed_alloc()`.
- `vk_texstat_add(VK_TS_DEVMEM, 1, size)`: counted when the render or replay thread made it.

### `on_free()`

`NX2SARIA_TEX_COMPACT`: every device memory VMA gives back to the driver is reported to the texture compaction, which
counts the blocks its pass emptied (`runtime/vk/vk_texture.c` `vk_texture_block_freed()`). Any thread; the callback
takes no lock.

### `vk_headroom_callbacks()`

- `const char *v = cfg_get("NX2SARIA_VRAM_HEADROOM")`: empty is unset: the key has no settings row for `cfg_bool`.
- `if (vk_texture_compact_on()) return &cb_free`: `on_allocate()` does nothing for the headroom while `H.on` is 0.

### `block_size()`

- `return heap <= HR_SMALL_HEAP ? heap / 8 : vk_vma_block_size()`: VMA's `preferredLargeHeapBlockSize`
  (`runtime/vk/vk_device.c`).

### `room()`

- A budget taken as smaller for a test (`NX2SARIA_VRAM_BUDGET_MB`, `vk_mem_vram`) is what the mirrors and the textures
  go by; measured against the driver's budget alone, a capped run kept its spare blocks where the card it stands for
  keeps none. Only then: without a cap the heap's own figure above decides, heap by heap.

### `top_up()`

- `if (H.made == before)`: there was room.

### `headroom_main()`

- `uint64_t watch = 0`: memory types seen so far.

### `vk_headroom_release()`

An allocation the driver refused (`runtime/vk/vk_memory.c` `give_back`): the anchors go, so VMA can use or delete their
blocks, and no spare block is made for `HR_RELEASE_HOLD_MS`. Returns the bytes the anchors held.

### `vk_headroom_stop()`

Before the allocator is destroyed: waits for a running top-up, frees the anchors, and the thread allocates no more.

## `runtime/vk/vk_scratch_lru.h`

Which entry of a small cache of scratch images serves a request for an image of one size and format
(`runtime/vk/vk_exec_draw.c` `fallback_scratch()`). Header only, so that `tests/scratch_lru_check.c` checks it without a
device.

The cache held its first `SCRATCH_N` images for the whole run and refused every other size: each scene size the stand-in
of the game's upscaler blits through takes one, and after a run of window, resolution or DLSS mode changes the blit of a
new size found no image and was left out. Now a request that matches no live entry takes a free one, else the one used
longest ago, which the caller frees (deferred: a submission may still name it) before it makes the new image there. The
images in use stay; one used in no frame for a while is what goes.

`ScrEntry`:

- `uint32_t w, h, f`: the image's size and format (`VkFormat`).
- `uint64_t used`: the frame of its last request.
- `int live`: holds an image.

### `scr_pick()`

The entry for (w, h, f) among `e[0..n-1]`: a live match (`*hit` 1), else a free entry, else the live one used longest
ago (`*evict` 1: its image is to be freed before the slot is reused). A hit's used becomes now; the caller fills a new
entry once its image is made.

## `runtime/vk/vk_up_defer.h`

What an upscaler lets go of while the GPU may still use it (`runtime/vk/vk_dlaa.c`, `runtime/vk/vk_ffx.cpp`,
`runtime/vk/vk_ffx4.cpp`, `runtime/vk/vk_fsr3.c`): a DLSS feature, an FSR upscaling context, or an image frame
generation can still read at a present. Each owner keeps a list and releases an entry with its own callback, on the
thread that makes the upscaler's other calls (`runtime/vk/vk_dlaa.c`'s pass hooks), once the GPU timeline has reached
the value of the last submission that named it (fence; 0: none did) and the present count has reached presents (0: no
such gate). Nothing here waits: the owner checks at its own calls (`upd_take()`), and only a full list or a refused
creation falls back to a wait, in the owner.

A header of its own so that `tests/up_defer_check.c` checks it without a GPU; `runtime/vk/vk_ffx.cpp` and
`runtime/vk/vk_ffx4.cpp` include it as C++, `runtime/vk/vk_dlaa.c` and `runtime/vk/vk_fsr3.c` as C.

`UpdEntry`:

- `uint64_t fence`: timeline value of the last submission that named it (`vk_exec_recording_fence`), 0 none.
- `uint64_t presents`: released once the present count has reached this, 0 at once.
- `int kind`: the owner's: what handle and aux are.

`UpdList`:

- `uint64_t put, released`: so far.

### `upd_put()`

1 when queued, or nothing to queue (a null handle: a half-made object's missing part); 0 when the list is full, and the
caller makes room with its fallback wait first.

- `UpdEntry *e = &l->e[l->n++]`: field by field, which C++ takes as C does.

### `upd_take()`

Releases, in the order they were put, the entries whose fence is done and whose present gate is passed; the others stay,
in their order. The ready ones leave the list before any callback runs, so a callback may put new entries, which wait
for the next take. Returns how many were released.

### `upd_take_all()`

Every entry whatever its gates: the device is idle and nothing reads them any more (teardown).

### `upd_max_fence()`

The highest fence among the entries (0: none named by a submission), for a fallback wait.

### `upd_count()`

The entries of kind still waiting, or of every kind for a kind below 0.
