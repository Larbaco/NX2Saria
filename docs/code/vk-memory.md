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

## `runtime/vk/vk_memory.c`

Vulkan memory of the graphics layer: NVN memory pools as Vulkan buffers, the virtual-pool page table, GPU address
resolution, page write generations for re-uploads, and the upload batch (command buffer, staging buffers, deferred
destruction) that `runtime/vk/vk_texture.c` records into.

Virtual pools have 64 KiB pages. A texture is stale when a covered page carries a generation newer than its upload;
every write-watch query resets the OS bits, so each query stamps the pages it saw with a new generation shared by all
users. Virtual pools are a GPU-side page table over device-local memory and dirty pages come from GetWriteWatch
(`mem_written_pages`), so neither host VM aliasing nor page protection (a fault handler) is needed.

### `qpc()`

Timestamps for the tracking cost, in ns; `qpc_ms()` converts.

### Pools

- `typedef struct { struct PoolRec *phys; uint64_t off; } VPage`: virtual page -> physical pool and offset.
- `#define FLUSH_RANGES 32`: flush ranges a mirrored pool keeps between two syncs (`flush_add()`).

`PoolRec`:

- `uint64_t lo, size`: GPU address range of the pool.
- `uint64_t import_lo, import_hi`: `POOL_IMPORT`: the imported host range (aligned outwards).
- `VkDeviceMemory mem`: `POOL_IMPORT`.
- `struct Span *span`: `POOL_IMPORT` as a dma-buf: the span whose memory buf is bound to ("dma-buf imports").
- `VmaAllocation alloc`: `POOL_PHYS`.
- `VPage *pt`: `POOL_VIRT`: size / 64 KiB entries.
- `volatile int32_t used`: resolved at least once: the GPU may still read it at finalize.
- `uint8_t tex_off`: `POOL_IMPORT`: its import was given back, as texture storage ("texture storage pools" below); a
  resolve imports it again.
- `NX2SARIA_TRACE=perf` dirty census (`vk_mem_dirty_submit()`), replay thread under the submit lock only:
- `uint32_t bound_sub`: last submit that bound it as a buffer (`dirty_sub`), 0 never.
- `uint64_t synced_gen`: page generation its lazy-mirror count covers; `DIRTY_UNSYNCED` until scanned.
- `uint8_t bufbound`: ever bound as a vertex, index, uniform, storage or indirect buffer.
- `uint8_t untracked`: the last dirty scan found no write watch over it.
- VRAM mirror (`POOL_IMPORT`, "VRAM mirror" below). mstate and mbuf change on the replay thread only.
- `uint8_t mstate`: `MS_NONE` until the first buffer use, then `MS_ON` or `MS_REFUSED`.
- `uint32_t gpa_rec`: `POOL_IMPORT`: its record in the device address table (`runtime/vk/vk_gpa.c`), 0 none.
- `uint8_t gpa_flip`: as pend: the next prologue records the store that hands its table record to the mirror
  (`gpa_flip_cmds()`); 0 when the table names the mirror, or none exists.
- `struct GpaMap { uint64_t lo, hi; uint32_t rec; } *vmaps`: `POOL_VIRT`: its mapped ranges in the table.
- `VkBuffer mbuf`: `MS_ON`: device-local copy of [`import_lo`, `import_hi`), same offsets as buf.
- `uint32_t acc_sub`: replay: the last submission that synced the mirror (`mirror_sync()`). Relaxed atomic (`acc_of()`,
  `acc_set()`): the replay workers read it on `vk_mem_use()`'s unlocked fast path while another one's `mirror_sync()`
  sets it under `use_lock`.
- `uint32_t pm_frame`: replay: the frame (pframe) of `pm_cur`.
- `uint8_t pm_cur, pm_prev`: replay: the places in their frame (`vk_sub_in_frame`, 7 the rest) of the submissions that
  synced it, in frame `pm_frame` and in the one before (`NX2SARIA_PRESNAP_PLACE`).
- `uint32_t wb_sub`: replay: the last submission that wrote mirror bytes back to guest memory.
- `uint64_t wb_fence`: replay: that submission's fence (`mirror_capture()` waits for no write-back).
- `uint8_t wb_kept`: replay: a write-back left bytes in the mirror alone (`wb_skip()`): never evicted.
- `uint32_t retry_sub`: replay: a mirror refused for the VRAM budget or a failed allocation is asked again from here, 0
  never.
- `uint8_t refused_why`: replay: the `MR_*` reason of its refusal, for the counts when it is asked again.
- `uint8_t asked_room`: replay: its refusal already asked the textures for room (once until it gets one).
- `uint8_t twin`: `MS_ON`: the table's writes reach guest memory too (`twin_wanted()`); 0: the mirror only.
- `uint32_t vd_wrote_sub, vd_use_sub`: verify mode: the last submission that bound it written / used its mirror.
  `vd_use_sub` is a relaxed atomic: every replay worker that uses the mirror stores the same value.
- `uint64_t vd_wrote_n`: verify mode: written bindings of it so far.
- `struct WbSpan { uint64_t lo, hi, fence; } *cws`: replay: the byte ranges of its merges the CPU stores.
- `int ncws, ccws`: (`NX2SARIA_MERGE_CPU`) and their values, until done.
- `uint64_t mgen`: `gen_lock`: every page write stamped up to this generation is in the mirror (or in a copy queued
  before its next use).
- `FlushRange fl[FLUSH_RANGES + 1]`: `flush_lock`: `nvnMemoryPoolFlushMappedRange` pages not copied yet (`flush_add()`).
- `int fl_n`: `flush_lock`: ranges in fl; read without the lock to skip an empty list.
- `uint64_t fl_calls, fl_sum`: `flush_lock`: the calls and bytes behind fl, for `mirror_sync()`'s statistics.
- `uint64_t hot_bytes`: replay: uploads, write-backs and merges since the last hot check (`mirror_hot`).
- `uint64_t hot_up`: of them the uploads: pages the CPU wrote (`mirror_hot` leaves such pools mirrored).
- `VmaAllocation salloc`: Shadow storage (`POOL_IMPORT` in shadow mode, "shadow storage" below): buf is a host-visible
  copy of the guest pages.
- `uint8_t *smap`: its mapping; offsets as in buf (from `import_lo`).
- `uint64_t sgen`: `gen_lock`: every page write stamped up to this generation is in the shadow.
- `uint64_t sborn`: the value of the first submission that can have used the shadow (`shadow_pool()`).
- `volatile uint8_t stouched`: on the touched list: the GPU may use it in the submission being recorded.
- `uint32_t stab_i`: `shadow_lock`: 1 + its index in stab (the table's shadows), 0 when not there.
- `struct SBase { uint64_t page; uint8_t *base; } *sb; uint32_t nsb, csb`: `shadow_lock`: base copies of the pages a
  noted GPU write covers (`merge_page()`), open addressing by page address; csb a power of two, page 0 an empty slot.
- `uint32_t *smark; uint64_t *tpg; uint32_t ntpg, ctpg; uint8_t tlisted`: `shadow_lock` ("Pages a shader stores into
  through the device address table" below): a mark per page of [`import_lo`, `import_hi`), the hash of the shadow page
  as its last whole copy-in left it, 0 not known; the pages found stored into (guest addresses), and whether the pool is
  on the list of pools that have such pages.
- `uint64_t t_over`: whole copy-ins that found such a store in the page they overwrote, so far.
- `uint64_t t_over_log`: `t_over` at the last log line.
- `uint64_t s_in`: `NX2SARIA_SHADOW_CHECK`: bytes copied in whole since the last log line.
- `NX2SARIA_TRACE=perf` per-pool mirror traffic since the last `mirror_pool_log()`, replay thread (the log reads and
  clears them without a lock: a count lost to the race only blurs a diagnostic).
- `uint64_t st_pages, st_flush, st_wb, st_merge`: bytes: written pages and flush ranges copied up, GPU writes back.


### The pool index and its generations

- `static int shadow_on`: shadow mode: imported pools get shadow storage instead (`vk_mem_class_log()`).

`pool_gen`: Bumped under `pool_lock` (exclusive) by every change to the index or a page table: `vk_mem_resolve()`'s
per-thread hits stay valid while it is unchanged.

`mirror_gen`: Bumped when a buffer use may get another buffer for the same span (a mirror created, refused or forgotten)
and at every submission's start (mirror syncs are per submission). `vk_mem_gen()`: both together.

- `static pidx pool_index`: `POOL_IMPORT`, `POOL_PHYS` and `POOL_VIRT` records by GPU address.
- `static PoolRec **phys_list`: physical pools by key, for MapVirtual requests.

### `pool_usage()`

- `if (vk_gpa_wanted()) u |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT`: pages of the device address table.

### Dma-buf imports

Where guest memory is a memfd's (Linux, `NX2SARIA_DMABUF`, on by default: `runtime/platform/linux.c` "Guest memory as a
memfd's" in [platform.md](platform.md)), a CPU-visible pool is imported as a dma-buf of its own pages, not as a host
pointer and not as a shadow copy. It keeps what an import is to the rest of this file (the GPU's writes are in guest
memory once the GPU has made them, the device address table's twin is guest memory, mirrors sync from it and write back
into it), without amdgpu's costs for host pointers: those are userptr objects, which the kernel looks up page by page at
every submission and invalidates whenever the write watch or a freeze re-protects their pages (about 0.2 frames a second
in Kokiri Forest), which is why Linux had shadow storage. A dma-buf's pages are pinned once and the process's page
tables never matter to it again: the write watch, the freezes and the lazy write-back's armed pages leave the GPU alone.
RADV puts every imported memory object in every submission, at about 0.6 us each (RX 6800 XT, Linux 7.3, Mesa 26.2: 2.6
ms a `vkQueueSubmit` with 4096 of them, 0.04 ms with 64), and the game keeps over 4,000 pools, so a pool does not get a
dma-buf of its own: guest addresses are cut into aligned spans of `NX2SARIA_DMABUF_MB` (64 by default), the first pool
made in a span imports the span's dma-buf, and every pool is a buffer bound into the memory of a span that holds it, at
its offset. A pool that no span holds gets a new one over the aligned range around it, cut back at the spans already
there (`span_get()`): spans overlap only where such a pool itself lies in an older span, because every span's pages
count in amdgpu's GTT use (an overlap twice) and stay pinned. A span whose last pool is gone stays `SPAN_KEEP` frames
for the pools the game makes again at the same addresses, then is freed (`span_gc()`); its dma-buf goes with the memory.

A new span pins its pages: about 13 us per MiB the game had touched and 260 us per MiB it had not, which are allocated
then (the same RX 6800 XT machine), so a 64 MiB span costs 1-17 ms once. A span over udmabuf's size limit
(/sys/module/udmabuf/parameters/`size_limit_mb`) is not made; such a pool, or one outside the memfd, falls back to a
host-pointer import, which a shared mapping's pages refuse on RADV: it gets no GPU buffer (logged).

- `static int dmabuf_on`: `mirror_mode()`.
- `static int dmabuf_exts`: `vk_mem_dmabuf_caps()`: the device has `VK_KHR_external_memory_fd` and `_dma_buf`.
- `static uint64_t dmabuf_limit`: udmabuf's size limit, bytes.
- `static PlatMutex span_lock = PLAT_MUTEX_INIT`: after `pool_lock`.
- `static Span **spans`: `span_lock`.
- `static uint32_t dmabuf_type = UINT32_MAX`: `span_lock`: the memory type of the imports, asked at the first one.
- `static int64_t sp_live_bytes`: `span_lock`.
- `static uint64_t sp_made, sp_made_bytes, sp_ns, sp_max_ns, sp_freed, sp_freed_bytes, sp_fail, sp_fallback`:
  `span_lock`, since the log.
- `static uint64_t sp_fail_all`: `span_lock`.
- `static volatile uint64_t span_frame`: the frame `vk_mem_census()` last saw.

### `dmabuf_pick_type()`

The memory type of the imports, from the types the driver offers for the dma-buf fd: cached where offered, as guest
memory is to the CPU (RADV: the GPU snoops it); not one of AMD's device-coherent or device-uncached types, which the
third pass takes only when nothing else is offered. Sets `dmabuf_type` (`UINT32_MAX`: none fits); returns the offer.

### `vk_mem_dmabuf_probe()`

After device creation, before the first pool and before anything asks for the storage mode (`vk_mem_lazy_decide()`): one
page of guest memory is made a dma-buf, imported and bound to a buffer, as every pool will be. Any step refused turns
the dma-buf imports off for the run: the pools are shadow copies, as with `NX2SARIA_DMABUF=0.` A refusal later, for one
span, leaves its pools without a GPU buffer (`span_log()` counts them), so the probe asks once for what all need.
`dmabuf_any` is the page: a page of guest memory a dma-buf can be made of.

- `dmabuf_close(fd)`: a failed import leaves the descriptor with us.

### `span_fail()`

`span_lock` held.

### `span_create()`

`span_lock` held: a new span over [lo, hi), NULL when udmabuf or the import refuses it.

- `if (dmabuf_type == UINT32_MAX)`: every dma-buf of guest memory has the same types: asked once.
- `dmabuf_type = UINT32_MAX - 1`: not asked again: every span fails.
- A buffer with device address usage needs memory allocated for device addresses (`runtime/vk/vk_gpa.c`).
- `else dmabuf_close(fd)`: a failed import leaves the descriptor with us; a successful one took it.

### `span_get()`

`span_lock` held: the span a pool's [lo, hi) is bound into, with a reference for it; NULL when none can be made. al is
the alignment the pool's buffer needs inside the span's memory.

A new span covers the `span_size`-aligned range around the pool, less what other spans hold below and above the pool: a
pool across an edge of an older span then pins only its own pages twice, not the older span's whole range. With the
whole aligned range, Hyrule Field's area loads left 11.5-11.7 GiB of spans counted in amdgpu's GTT use; cut back,
9.5-9.6 GiB, over the same 9.3 GiB of distinct pages, at the same frame rate (RX 6800 XT, 2 pairs).

- `for (int i = 0; i < nspans; i++)`: no span holds [lo, hi): none reaches below lo and above hi at once.
- `if (al > 1) a = lo - (lo - a) / al * al`: the pool's offset in the span a multiple of al.
- The aligned range may leave the memfd's reservation: the pool's own pages then.

### `span_gc()`

Frees the spans no pool has used for `SPAN_KEEP` frames (all: every span without a pool). A span without a reference is
out of the GPU's work: a pool's reference goes when its record is destroyed, after the GPU is done with it.

- `for (int i = 0; i < ng; i++)`: the dma-buf goes with the memory; then its pages are let go.

### `span_report()`

The spans' report: at frames 600 and 1800 (every 60 frames with `NX2SARIA_TRACE=perf`), and at the end of the run
(`vk_mem_exit_log()`), so a session that went on for an hour also says what happened to its spans after the start: the
counts "since the last line" run from the line at frame 1800 to the end. when names the moment ("at frame 600", "at exit
(frame 41022)"). `wait_ms`: how long to wait for `span_lock`, 0 for as long as it takes. A span's creation holds the
lock while it makes the dma-buf and imports it (2.7 s at the longest in a session), so the end of the run, whose seconds
are few (`runtime/gfx/gfx_present.c` `rt_exit_steps()`), waits 0.2 s and writes a line that says so when the lock stays
held.

### `import_dmabuf()`

A pool's buffer bound into a span's memory; 0 when no span can be made for it.

### `import_pool()`

CPU-visible storage: the guest pages themselves become the buffer's memory (`VK_EXT_external_memory_host`, or a dma-buf
of them: "dma-buf imports" above).

- A buffer with device address usage needs memory allocated for device addresses (`runtime/vk/vk_gpa.c`).

### `alloc_physical()`

Physical pool: device-local memory of the pool size that both buffers and optimal-tiling images can be bound to.

- `if (res != VK_SUCCESS)`: video memory full (VMA heap limits, `runtime/vk/vk_device.c`): any memory the pool can live
  in.
- `if (vk_fix_zero())`: Switch pool memory starts zeroed; once per pool, not per image placed in it.

### `index_put()`

- `pidx_remove(&pool_index, r->key, NULL)`: an object initialized again without Finalize.

### `destroy_rec()`

- `for (uint32_t i = 0; i < r->csb; i++) free(r->sb[i].base)`: shadow base copies `shadow_forget()` left (shutdown).

### `pool_init()`

- The GPU never sees a `GPU_NO_ACCESS` pool, except a physical one (0x189; 1.4.3 also 0x18c, CPU-visible) through the
  virtual pools that map it. Either gets device-local memory: 1.4.3's CPU never writes its 0x18c pool's storage
  (unchanged over 3600 frames of the intro), so the guest storage and the GPU copy need no sync.
- A pool of zero bytes (the game makes some now and then) holds no address the GPU can use: it gets no record. A record
  would import 0 bytes (invalid usage; the driver refuses the memory) and put an empty range into the index at the
  pool's address, where a lookup of the pool that starts there found the empty one first. `vk_pool_fini()` finds no
  record for it and does nothing.
- `mem_thaw((uintptr_t)p->mem, p->size)`: the import may need write access to the pages.
- The host heap full: what holds only texture storage makes room, and the import is tried once more.
- Not fatal for the game: the pool works for the CPU, and `vk_mem_resolve()` reports its addresses as unbacked.
- The imported pages, or the shadow that stands for them, go into the device address table: shaders that read a pointer
  from memory reach them there. A shadow in the table is kept up to date for the table's readers whether or not the
  replay resolves it (`vk_mem_shadow_sync()`).

### `gpa_cover()`

`vk_gpa_unmap`: the record of another live imported pool that holds the 4 KiB page at a whole (overlapping imports of
the same guest memory), 0 for none.

### `pool_fini()`

- `for (int i = 0; i < pool_index.n; i++)`: virtual pages that still map it become unmapped.
- `vk_gpa_clear(v->lo + k * NVN_VPOOL_PAGE, v->lo + (k + 1) * NVN_VPOOL_PAGE)`: the buffer goes away.
- Off the device address table first; the buffer outlives the submissions that may still read the old pages.
- `if (r->kind == POOL_IMPORT) mem_thaw(r->lo, r->size)`: the storage goes back to the guest writable.
- Images cut from this range go first; a used buffer or memory waits until the GPU is done with it, since the guest gets
  the storage back when this returns.
- `if (narrow)`: the Vulkan objects outlive the upload batches and the submission being recorded (`pool_wait_add()`).
- A record that waits for the next submission is on the retiring list until its callback runs: when no submission
  follows (a pool freed at the end), `vk_mem_shutdown()` destroys it. Left to the callback alone, a shadow copy's buffer
  was still allocated when the allocator went, which stops the process there. A mirrored pool's record and buffers
  outlive the submission being recorded, whose sync copies and write-backs may name them (the game frees a pool only
  once the GPU is done with it, but its CPU writes may still be queued).
- The queue was not drained.
- An unused physical pool may still be read through the virtual pages that mapped it (device addresses, which do not
  mark it used): its buffer outlives the submission being recorded too.

### `vk_pool_init()`

`NX2SARIA_TRACE=perf`: a pool's initialize and finalize, with their imports, allocations, uploads and GPU waits, marked
as work a frame can wait for (`runtime/diag/perf.c` `perf_mark_add()`).

### `gpa_map_virtual()`

`pool_lock` held: the device address table follows a MapVirtual request on [lo, hi) of virtual pool v. Ranges it covers
whole leave the table (their records retire); the pages then name the physical pool's buffer at poff, or none.

### `vk_pool_map_virtual()`

Requests were checked by the front end (`runtime/nvn/nvn_api_core.c`); apply them in order.

### `rec_class()`

Per-thread copies of resolved import and physical pools that overlap no other index entry (so `pidx_find` picks them for
any range inside), valid while `pool_gen` is unchanged: the replay resolves a few dozen pools thousands of times per
frame (16 vertex streams and every buffer binding of each draw). Direct-mapped by the 4 KiB page of the address, so a
hit is one compare however many pools a frame touches. `NX2SARIA_RESOLVE_CHECK=1` compares every hit with the locked
lookup and stops on a difference.

### `mem_resolve()`

`touch`: the GPU may read the pool's buffer in the submission being recorded, so its shadow goes on the
touched list (shadow storage). A lookup (`vk_mem_lookup()`) leaves it off: the caller reads guest memory itself or only
asks what backs the address. Shaders that reach a pool through the device address table resolve nothing: the part they
run in brings every pool of the table up to date (`vk_mem_shadow_sync()`).

- `resolve_off`: `resolve_locked()` met a pool whose import was given back (`tex_pool_back()`).

### `resolve_locked()`

The pool index lookup under the lock; a non-overlapped import or physical pool goes into the address's cache slot.

- `h->gen = __atomic_load_n(&pool_gen, __ATOMIC_RELAXED)`: stable: changed only under the exclusive lock.
- `if (ok && !ph->used) plat_xchg32(&ph->used, 1)`: read first: the flag is only ever set.

### Census

### `vk_mem_count()`

Every caller is the replay (`vk_exec*`, under `vk_sync_submit`'s lock), about ten adds per draw: one writer at a time,
so a plain add, not a locked one. An add racing the census's reset may be lost.

### The periods of the census

- `#define HOLES_PERIOD 3600`: frames between two "mem: holes" lines (`holes_log()`).
- `static volatile uint64_t mirror_frame`: the frame `vk_mem_census()` last saw, for `mirror_sync()`'s log lines.

### `vk_mem_snapshot()`

`NX2SARIA_TRACE=perf`: the 600-frame memory lines now, at a segment's end and once a scene has settled
(`runtime/diag/perf.c`).

### `vk_mem_census()`

- The report runs here, on the presenting thread, so its time is timed: the 600-frame frames of a session with
  `NX2SARIA_TRACE=perf` carried 10-17 ms nothing else accounted for (2026-11-13), and the walks of the texture entries,
  the pipelines and the C runtime heap's summary all happen in it.
- `perf_report_part(h2 - r0)`: the next frame's "report" part (`runtime/diag/perf.c` `ft_frame()`), not unattributed.
- `if (vk_texture_compact_due()) compact_look()`: `NX2SARIA_TEX_COMPACT`: 0 at once with the setting off.
- `mirror_log(frame), shadow_log(frame), span_log(frame)`: `mirror_log()` also logs the write-watch line.
- `shadow_log(frame)`: `NX2SARIA_SHADOW_CHECK`: the shadow lines at that rate without the perf trace.
- `if (frame == 1200)`: loading is over: the first window starts here.
- Frame 600 as well: runs on slow machines end before 1800.

### `budget_cap()`

`NX2SARIA_VRAM_BUDGET_MB` (test; unset or 0: the device's): the device-local budget every decision here and in
`runtime/vk/vk_texture.c` compares with is at most this many MiB, so a large card behaves as a small one does under
pressure (mirrors refused or evicted, textures dropped). The heap lines still log what the driver reports.

### NX2SARIA_VRAM_BALLAST_MB

`NX2SARIA_VRAM_BALLAST_MB` (test; unset or 0: none): that much device-local memory is taken when the allocator is made
and held until it goes, in buffers of `BALLAST_PIECE` of their own memory. `NX2SARIA_VRAM_BUDGET_MB` above changes only
the number the decisions compare with; this fills the card for real, so the driver's own behaviour at its limit (slow
allocations, memory moved to system RAM, refusals) shows on a large card as it does on an 8 GB one or beside another
program's use.

### `vk_mem_ballast()`

`runtime/vk/vk_device.c`, right after the allocator is made (before `vk_ctx` answers).

### `vk_mem_ballast_release()`

Before the allocator goes.

### `vk_mem_vram_live()`

`vk_mem_vram()`'s use less the free space inside VMA's device-local blocks: the driver counts a block whole for as long
as one allocation lives in it, and the next allocation of its type takes a hole first. The evictions measure with this:
a texture or mirror freed in the middle of a block left the driver's figure where it was, so an eviction that waited for
that figure to fall went on until nearly every idle texture and mirror was gone (16.2 GiB in use, 9.2 GiB of it free
inside blocks after the evictions).

### Page write generations

Two-level table over the guest address space (below 2^39): 256 MiB chunks of 65,536 page generations, allocated when a
page of the chunk is first stamped. 0 = never stamped.

Generations are 64 bits and never run out (at 336 a frame and 240 fps, 2^64 lasts millions of years). At 32 bits, 336 a
frame (the most measured, 0.10.0) and 240 fps ran them out after about 15 h of play, and past the wrap a stamp from
before it compared newer than every later check and one made after it older: textures and pool mirrors missed the CPU's
writes, and pages nothing wrote read as written at every check. Below 2^32 every value and every compare is what it was
at 32 bits.

- `static uint64_t gen_now`: last generation handed out (`gen_lock`; read without it atomically).
- `static uint32_t n_gen_chunks`: chunks `slot_in()` allocated, both tables (`gen_lock`; `vk_mem_sizes()` reads it).

The static assertion on the atomic's lock-freedom: `lazy_clean()`, `cpu_wrote_since()`, `verify_cb()` and
`vk_mem_sizes()` read generations without `gen_lock`: 64-bit loads and stores must be single instructions on every
target.

### `gen_mark()`

When the count reaches 2^32, where the 32-bit generations of earlier versions ran out, the log says so once, with the
frame, the hours of play and the rate, so a long session's log shows how far it got. `gen_lock` held exclusively; the
log takes no lock of this file.

### `gen_seed()`

`NX2SARIA_GEN_SEED` (a test; default 0): the run's generations start after this value, so a run reaches 2^32, where the
32-bit generations of earlier versions ran out, or the low 32 bits passing through 0 again (2^40 - a few), within
seconds. The first `gen_next()` reads it, before anything is stamped, and goes on from it or from `gen_now`, the larger.
Parsed as 64 bits with strtoull (strtoul stops at 2^32 - 1 where unsigned long is 32 bits, as on Windows), decimal or 0x
hexadecimal, and capped at `GEN_SEED_MAX`, far from the 64-bit end. It only ever raises the counter: a lower one would
leave stamps newer than it, and they would read as written at every check. `gen_lock` held exclusively; `cfg_get` and
the log take no lock of this file.

### `gen_next()`

`gen_lock` held exclusively: the only place a generation is handed out. The store is atomic: `vk_mem_sizes()` and
`verify_cb()` read `gen_now` without `gen_lock`.

- `if (__builtin_expect(g == 1, 0)) g = gen_seed() + 1`: the run's first generation.

### The region maxima, the CPU-side tables and the write-watch counts

`reg_max`: the newest generation stamped on any page of each 2 MiB region (the write-watch snapshot regions below): the
mirror sync skips regions with nothing newer than its pool's mirror.

`cgen_chunk`: The same two tables without the stamps of a mirror's own write-back (`vk_gpu_written_back()`): what the
CPU and the host paths wrote. The mirror sync reads these, so the bytes a mirror wrote back into guest memory are not
copied up into it again at its next sync: the storage buffers the game's compute shaders write went down and came back
up every frame, 20 MiB a frame over PCIe in Kokiri Forest (2026-11-09).

`ww_pages`: Every write-watch query of the generation table (`gen_lock` held exclusively): calls, bytes and time since
the last mirror summary, all consumers together; the queries that found written pages (they also flush TLBs) apart.

`slow_more_max`: `NX2SARIA_TRACE=perf`: a write-watch query, freeze or its follow-up query that took over 4 ms gets a
line (the first 200). A frame some ten frames after an area load took 51.6 ms of replay with 1.2 ms of it in the
replay's ops; the replaying thread's samples sat in GetWriteWatch under `vk_mem_mirror_begin()` and in the freeze's
VirtualProtect, and no thread of the process could be sampled for 28 ms in between.

Past the 200 lines the calls are counted, and `slow_mem_window()` gives the count, their time and the longest every 60
frames: a session of 2026-11-13 used the 200 lines up by frame 36,387, and its last area load had none.

### `slow_mem_window()`

`NX2SARIA_TRACE=perf`, every 60 frames.

### Frozen regions

A 2 MiB region whose pages nothing wrote for `freeze_idle` submissions is made read-only
(`runtime/memory.c` `mem_freeze()`), and write-watch queries skip it until a write or a file read thaws it. Most of the
pool memory the mirror and the texture checks cover is static (meshes, textures), and a query costs about 1 us per MiB
(4-5 on an 8th-gen i7), so this keeps the queries to the regions the CPU writes. The region need not lie wholly in
pools: NVN pools are small and not 2 MiB aligned, so most regions also hold other guest memory, and a rule that refused
those left 340 of 436 queried MiB a frame unfrozen in town although nothing had written them for 300 submissions. The
other memory is safe to freeze for the same reason pools are: the only kernel writes into guest memory are file reads,
which thaw their buffer first (`runtime/hle/fs_host.c`, `runtime/hle/romfs.c`), and every user-mode write faults and
thaws.

- `static int freeze_idle`: 0: off.
- `static uint32_t ww_ep = 1`: `gen_lock`: the submission being recorded (`vk_mem_mirror_begin()`).
- `static uint32_t reg_last[RT_ADDRESS_LIMIT >> REG_SHIFT]`: `gen_lock`: `ww_ep` of the region's last stamp.
- `static uint32_t reg_tried[RT_ADDRESS_LIMIT >> REG_SHIFT]`: `gen_lock`: `ww_ep` of its last freeze candidacy.

`reg_queued`: `gen_lock`: the region is in fcand. While the passes are held a region still idle came up again every
`freeze_idle` submissions and went in a second time; `mem_frozen` skipped the copies, but the queue grew with them.

- `static uint32_t *fcand`: `gen_lock`: regions to try at the next freeze pass.
- `static uint64_t n_frz_refused`: `gen_lock`: candidates `mem_freeze` refused.

### `slot_in()`

- Published with release: `lazy_clean()` and `cpu_wrote_since()` load the chunk pointers without `gen_lock`.

### `stamp_page_as()`

`gen_lock` held exclusively; g is the newest generation handed out. back: a mirror's write-back, which leaves the
CPU-side tables alone. The slots are stored atomically: `lazy_clean()`, `cpu_wrote_since()` and `verify_cb()` read them
without `gen_lock`.

### `vk_mem_changed()`

- `if (n < sizeof gen_pages / sizeof *gen_pages) break`: a full answer may have left pages for the next call.

### Write-watch snapshots per submission

The guest address space in 2 MiB regions, each queried at most once per epoch (a
guest submission), at the first check that covers it. A query costs about 1.3 us plus 0.55 us per MiB, so one per region
instead of one per texture. The snapshot only adds observations to the generation table: a page written before it
carries a generation newer than any check before it, and a page written after it keeps its OS bit until the next query
over it (at the latest the next epoch's snapshot).

- `static uint32_t snap_at[SNAP_REGIONS]`: epoch of the region's last snapshot; 0 never.

### `snap_regions()`

`gen_lock` held exclusively. Queries the regions r0..r1 that were not snapped in epoch, one query per run of them;
returns the number of queries.

- `if (mem_frozen(r << SNAP_SHIFT))`: nothing in it changed since its freeze.
- `uint64_t k = r`: one query over a run of regions not seen in this epoch.

### `snap_widen_n()`

`NX2SARIA_SNAP_WIDEN`=`<regions>` (default 0, off; the low-end GPU mode set 8 up to 0.10.1): a texture check that has to
query widens the query to up to this many 2 MiB regions on each side, inside the import pool that holds the texture, so
the textures of one pool are snapped by one query a submission instead of one each. With freezing off
(`NX2SARIA_FREEZE_IDLE`) an RX 580 in Hyrule Field made about 1,130 texture queries a frame, about 6 MiB each on average
incl. repeats, 9.4 ms with the pools'.

### `snap_widen()`

R0..r1 widened inside the import pool that holds [lo, hi); `pool_lock` taken here (before `gen_lock`, as everywhere).

- `uint64_t p0 = (r->import_lo + (1ull << SNAP_SHIFT) - 1) >> SNAP_SHIFT`: whole regions of the pool only.
- `uint64_t p1 = r->import_hi >> SNAP_SHIFT`: [p0, p1).

### `vk_mem_changed_snap()`

- Pages only in regions stamped after since (`reg_max`; `REG_SHIFT` is `SNAP_SHIFT`): an unchanged texture of 4 MiB was
  1,024 page reads, at about 1,100 texture checks a frame.
- `*now = gen_now`: every observation so far is in the table.

### `vk_mem_unchanged_why_locked()`

- `for (uint64_t r = r0; r <= r1; r++)`: `REG_SHIFT` is `SNAP_SHIFT`: `reg_max` is the newest stamp in the region.

### The snapshot of many ranges at once

For the texture pool's freshness check (`runtime/vk/vk_texture.c`
`vk_texture_pool_fresh_locked()`): `vk_mem_snap_note_locked()` notes the regions of a range that have no snapshot in
epoch yet, and `vk_mem_snap_noted_locked()` queries what was noted, one query per run of adjacent regions. Asked range
by range (`vk_mem_snap_range_locked()`), every such region got a query of its own, because a texture rarely spans two
regions and the list is in no address order. While the player runs through a town the game writes about 32 textures into
new storage every four or five frames, a region the CPU wrote is not frozen again for `freeze_idle` submissions, and so
100 to 500 regions of the texture pools were queried one by one at every check, 2.7 checks a frame: the check took 0.5
to 1.5 ms against 0.11 ms standing still (5 to 10 queries), about 3 us a query (five sessions of one player, RTX 5090,
2026-11-12). About half of a 2 MiB query is the call itself: in a process of its own, 400 of them took 727 us and one
query over the same 800 MiB 360 us, with 0, 8 or 64 pages written. The regions queried and the pages stamped are the
same, and every one is stamped before any entry of the list is looked at, as before: each entry's regions were queried
ahead of its own look. A run is asked in one query only where it lies inside one watched reservation (`mem_watched`): a
query across the edge of one fails as a whole, so the regions of such a run are left to the queries by range, and
`*left` says that there were some.

- `static uint32_t snap_noted[SNAP_REGIONS]`: `gen_lock`: `snap_pass` when the region is in `snap_want`.
- `static uint32_t *snap_want`: `gen_lock`: the regions noted since the last `vk_mem_snap_noted_locked()`.

### Dirty census

`NX2SARIA_TRACE=perf`.

What a device-local mirror of the buffer-bound pools would upload. The replay notes every imported pool it binds as a
vertex, index, uniform, storage or indirect buffer (`vk_mem_bound_note()`). At the start of each submit one write-watch
query per run of such pools stamps the pages written since with a new generation, in the shared generation table (no
second consumer of the OS bits: texture checks see the stamps), and the pages of those pools are counted:

- eager: pages stamped after the previous submit's scan, in every buffer-bound pool (upload at every submit);
- lazy: pages stamped after the pool's previous count, in pools the previous submit bound (upload when bound);
- First use: the size of a pool the first time it is bound (a mirror copies the whole pool then).

Classes: `CPU_CACHED`, `CPU_UNCACHED`, other (`CPU_NO_ACCESS`). Logged as `mem: dirty` every 60 frames.

- `#define DIRTY_GAP 0x100000ull`: pools closer than this share one query (the gap's pages are stamped too).
- `static uint32_t dirty_sub = 1`: submit lock: the current submit number.
- `static uint64_t dirty_gen`: submit lock: the generation of the last scan.
- `static uint64_t first_pending[DC_N], ever_n[DC_N], ever_bytes[DC_N]`: submit lock.
- `static PoolRec **dlist`: submit lock: the buffer-bound pools of a scan.
- `static PlatMutex dirty_lock = PLAT_MUTEX_INIT`: everything below.
- `static DirtyAcc dfr, dwin`: this frame, this window.
- `static uint32_t dwin_kib[DIRTY_WIN][DC_N]`: eager KiB of each frame of the window.

### `query_stamp()`

`gen_lock` held exclusively. 0: the range is not write-watched.

### Shadow storage

Where importing guest memory costs too much (Linux: re-protecting pages that amdgpu imported as a userptr invalidates
the buffer, about 1.3 ms per 16 MiB at the next submit, and the import's write pinning clears the write watch), a
CPU-visible pool gets a host-visible buffer of its own instead, at the offsets the import would have:

- CPU -> shadow: right before each submit (`vk_mem_shadow_sync()`), the pools the replay resolved since the last one are
  queried for written pages; pages stamped after a pool's sgen are copied in by the CPU.
- The device address table (`runtime/vk/vk_gpa.c`) names the shadows, and its readers reach pools the replay never
  resolves: the instanced foliage's and Navi's data, through pointers read from other buffers. Without a sync those
  shadows held what guest memory held when they were made, and Kokiri Forest's trees were bare trunks, without ferns or
  grass tufts. So a guest submission whose record binds the table (`vk_mem_shadow_table_read()`) syncs every shadow in
  the table, once, before the first part recorded after the binding is submitted (`vk_mem_shadow_sync()`), and those
  that have a VRAM mirror, which the table then names, queue the same pages for upload. Once is enough: the copy-ins
  read the guest submission's write-watch snapshots (`snap_regions()`), which a second sync in the same guest submission
  would find unchanged. A sync in every part that used the table ran about three times a frame outdoors. Measured on
  Linux (RTX 4090, Kokiri Forest, by the change's author): a sync at every guest submission, about six a frame, took 9.8
  ms and 888 write-watch queries a frame; one in the guest submissions that read the table 3.9 ms and 92.
- GPU -> guest: the replay notes each GPU write into CPU-visible memory at record time (`vk_mem_gpu_pending()`) and
  reports it at the fence (`vk_gpu_written()`), which copies the GPU's bytes back into guest memory.
- Pages both write: a GPU write is noted with the whole range its binding names, and the CPU writes other fields of the
  same records meanwhile. Each page such a range covers keeps a base copy from its first note on, the bytes guest memory
  and the shadow last agreed on. A copy-in takes only the bytes where guest memory differs from the base (the CPU's
  writes) and the write-back at the fence only those where the shadow differs from it (the GPU's), and either moves the
  base along (`merge_page()`). A byte both changed takes the CPU's value when a copy-in comes first and the GPU's when
  the fence does. Whole-range copies both ways lost the CPU's bytes: the game writes a lighting parameter into a buffer
  whose binding its compute passes write every frame, the GPU only ever read zero there, the lighting coefficients and
  then the volumetric fog came out NaN, and the fog pass drew the 3D scene black.
- The game must not get ahead of the write-backs. With an import the GPU's bytes are in guest memory when the GPU has
  stored them; here they get there in a completion callback, after the fence. A CPU write that should follow them and
  lands before that callback is overtaken: the write-back puts the GPU's older bytes over it and moves the base copy,
  and a rewrite with the base copy's value (a reset to zero) is no difference at any time. So the wait after a frame's
  first submission covers the write-backs with shadow storage (`runtime/vk/vk_sync.c` "the executed timeline", which
  tells what the game rewrites there and what a frame looked like without it).
- Pages a shader stores into through the device address table (`runtime/vk/vk_gpa.c`): the store goes to the pool's VRAM
  mirror and to its twin, which here is the shadow, not guest memory. No binding names the range, so nothing notes it
  (`vk_mem_gpu_pending()`): the page has no base copy and guest memory never gets the bytes. The game's culling passes
  store counts and records this way into pages whose other words the CPU rewrites. For one such page P, a word X the
  shaders store and a word Y the CPU writes:
  - Guest submission A (with the pass that stores): the CPU wrote Y, so A's sync copies P in whole (`shadow_copy()`) and
    queues it for the mirror (`mirror_sync()`). X in the shadow and in the mirror is now guest memory's X, which no
    store ever reached. A runs and stores X = v into both.
  - Guest submission B (a later one that reads X through the table), the CPU has not written P since A's write-watch
    snapshot: B's sync leaves P alone and its draws read v.
  - B, the CPU has written Y again between the two snapshots: B's sync copies P in whole and uploads it, X is guest
    memory's stale word again in both copies, and the draws read that.

  An import does not have the
  fault: its twin is guest memory, and a page copied up from there holds the store. Measured (`NX2SARIA_SHADOW_CHECK`, a
  standing run outdoors, RTX 5090 with `NX2SARIA_SHADOW=1`, 2026-11-10): the stores go into one page each of ten pools
  of 20 and 44 KiB, and whole copy-ins overwrite one exactly four times a frame (240 in every 60 frames): A's case, the
  copy that precedes the frame's stores, which does no harm. B's second case would add to the four and was not seen in
  1,800 frames; nothing proves it cannot happen where the frame's timing differs. (Counted when every part that used the
  table synced; a guest submission syncs once now, from the same write-watch snapshots, and the counts were not taken
  again.) `NX2SARIA_SHADOW_TABLE_STORES=1` keeps the stores. The pages are found by their content: each whole copy-in
  leaves a hash of the page it wrote (smark), and the next one hashes the shadow page first. A different hash means
  something stored into the shadow in between, and with no base copy on the page that was a store through the table.
  That copy-in still overwrites the store (nothing tells its bytes from the CPU's), and from then on the page keeps a
  base copy: copy-ins take the CPU's bytes only, and the fence of every submission whose record bound the table writes
  the stored bytes of all such pages back to guest memory (`table_wb_cb()`), as the fence of a noted write does. Guest
  memory needs them: a word the CPU resets each frame to the value it had before the stores would otherwise never differ
  from its base, and the reset would never reach the GPU. It is off by default, for three reasons. The fault it removes
  was not seen. Its hashes cost 1.8 ms a frame of the replay there (`copy_marked()`). And it puts these pages under the
  rule of the bullet above: a count the CPU resets before that write-back has landed keeps counting from its old value,
  where the whole copy-in of today resets it every time; whether the game resets any of these words from the CPU is not
  known.

`NX2SARIA_SHADOW_CHECK=1` counts, with the shadow line and at its rate every 60 frames (`shadow_table_log()`): the whole
copy-ins that overwrote a store through the table, per pool; the write-backs that moved bytes into a page the CPU had
written since its copy-in (`cpu_wrote_since()`); the bytes both sides changed since their base copy (taken from the CPU
at a copy-in, from the GPU at a fence); what the copy-ins cost by kind.

`NX2SARIA_SHADOW=1`/0 forces the mode on or off; the default is on for Linux and Wine, except for NVIDIA's own driver on
Linux, where imports ran about twice as fast on 0.7.1 (`mirror_mode()`).

- `static PlatMutex shadow_lock = PLAT_MUTEX_INIT`: after `pool_lock`; before `gen_lock`.
- `static PoolRec **stouch`: `shadow_lock`: pools resolved since the last sync.
- `static uint64_t s_sb_pages`: `shadow_lock`: base copies kept (`sbase_make()`).
- `static int64_t s_live_n, s_live_bytes`: `shadow_lock`.
- `static uint64_t s_syncs, s_copied, s_queries, s_untracked, s_wb_bytes, s_ticks`: `shadow_lock`: since the last log.
- `static uint64_t s_wb_seen, s_wb_ticks`: `shadow_lock`: bytes the fence write-backs compared, their time.
- `static uint64_t s_q_ticks`: `shadow_lock`: the copy-ins' write-watch queries.

The pages a shader stores into through the device address table. The two switches are read in `mirror_mode()`.
`s_learn` (`NX2SARIA_SHADOW_TABLE_STORES`: such pages get a base copy and a write-back) and `s_check`
(`NX2SARIA_SHADOW_CHECK`: the counts below marked "check") are defined with `vk_mem_dmabuf_probe()`, which may turn
shadow storage on late.

- `static PoolRec **tpools`: `shadow_lock`: the pools that have such pages.
- `static uint64_t s_tab_pages`: `shadow_lock`: such pages, all pools.

`s_tab_over`: `shadow_lock`, since the last log:

- `static uint64_t s_tab_over`: whole copy-ins that overwrote a store through the table.
- `static uint64_t s_tab_wb, s_tab_wb_seen, s_tab_wb_ticks, s_tab_wb_jobs`: `table_wb_cb()`: bytes moved and compared,
  time, runs.
- `static uint64_t s_marked, s_mark_ticks`: pages copied in whole with a mark; check: the time of those copies.
- `static uint64_t s_plain_ticks`: check: the time of the whole copies made without marks.
- `static uint64_t s_both_cpu, s_both_gpu`: check: bytes both sides changed, taken from the CPU / from the GPU.
- `static uint64_t s_in_merge, s_in_nomirror`: check: bytes copied in by `merge_page()`; whole, into pools without a
  mirror.
- `static uint64_t s_tab_ticks, s_tab_syncs`: the sync for the table's readers: its time (part of `s_ticks`), its runs.
- `static PoolRec **stab`: `shadow_lock`: the shadows in the device address table (`PoolRec.stab_i`).
- `static uint32_t stab_want`: the last guest submission whose record bound the table.
- `static uint64_t s_t_over_all`: `shadow_lock`: `s_tab_over` since the start.
- `static uint64_t s_late_pages, s_late_bytes`: `shadow_lock`, check: write-backs into pages the CPU wrote since their
  copy-in.
- `static uint64_t s_stale_pages`: `shadow_lock`: write-backs left out, the shadow could not hold them
  (`shadow_write_back()`).

### `shadow_pool()`

- Cached where the device has it: the fence copies read it back on the CPU.
- `r->sborn = vk_sync_issued() + 1`: the submission being recorded now, or the next one.
- `r->sgen = gen_now`: the copy below holds every write stamped so far; later stamps are copied again.

### Base copies of the pages both write

("Pages both write" above). `shadow_lock` held throughout.

### `sbase_find()`

The base copy of guest page pg of r, NULL when it has none.

### `sbase_make()`

Page pg of r gets a base copy unless it has one. Taken from the shadow: no GPU write was noted on the page before, so
the shadow holds what guest memory held at its last copy-in, and guest memory differs from it by the CPU's writes since.

- `if ((r->nsb + 1) * 4 > r->csb * 3)`: at most three quarters full.

### `byte_mask()`

0xff in each byte of x that is not zero, 0 in the others.

### `store_bytes()`

The bytes of v that m selects into dst (little-endian), the other bytes of dst left as they are: the other side may be
writing them at the same time.

### `merge_page()`

N bytes of one page with base copy b: the bytes where src differs from the base go into dst and into the base. src and
dst are guest memory and the shadow, one way or the other; each src byte that moves is read once, as its side may be
writing it. Returns the bytes moved.

- Most of a page is unchanged: a 64-byte block equal to its base is passed over with one vector compare. A byte its side
  writes after the compare is a later write, which the next copy-in or write-back takes.
- `memcpy(b + i, &s, 8)`: the bytes m leaves out are equal in both.

### Pages a shader stores into through the device address table

("shadow storage" above). `shadow_lock` held.

### `page_mark()`

The mark of a 4 KiB page: a hash of its bytes, never 0. Four sums that do not wait for each other.

### `table_page_add()`

Guest page pg of r was stored into through the table: it keeps a base copy from here on, taken from the shadow as the
whole copy-in just left it (guest memory's bytes), and `table_wb_cb()` writes its stores back.

### `both_changed()`

`NX2SARIA_SHADOW_CHECK`: the bytes of [0, n) that both sides changed since the base copy b and that now differ, which a
merge of src into dst is about to settle in src's favour.

### `copy_marked()`

Copies the whole guest pages [a, e) of r into its shadow, none of which has a base copy. Each page's mark tells whether
the shadow page still holds what the last such copy left: if not, a shader stored into it through the table meanwhile,
and this copy overwrites that store ("Pages a shader stores into" above says why it cannot be kept, and what the page
gets from here on).

Only with `NX2SARIA_SHADOW_TABLE_STORES` or `NX2SARIA_SHADOW_CHECK` (`shadow_copy()`). A mark is two hashes of the page,
one of them of bytes the copy would not have read: outdoors, where 2,400 pages a frame are copied in whole, the marks
cost 1.8 ms a frame of the replay (3.09 against 1.32 ms of copy-ins, 67 against 75 fps, RTX 5090 with
`NX2SARIA_SHADOW=1`, 2026-11-10).

- `if (s_learn)`: merged from now on: its mark is not read again.
- `*m = page_mark(s)`: of the shadow, not of guest memory, which the CPU may be writing.

### `shadow_forget()`

`vk_pool_fini()`, after the pool left the index: off the touched list before its record goes.

- `if (r->stab_i)`: the last one takes its place.

### `shadow_copy()`

`shadow_lock` held: copies guest [a, b) into the shadow; of a page with a base copy only the CPU's bytes
(`merge_page()`).

- `if (r->nsb)`: the plain run up to the next page with a base copy.
- `if (e == a)`: a starts on such a page: its part of [a, b).
- The copies are of whole pages (`shadow_sync_pool()`); the marks need the device address table, whose stores they find,
  and one of the two switches.

### `shadow_sync_pool()`

`shadow_lock` and `gen_lock` held: stamps the pool's written pages and copies those newer than its shadow. The pool's
regions are queried through the write-watch snapshots of the guest submission (`snap_regions()`, epoch), which the
texture checks and the mirror syncs share: each 2 MiB region at most once per guest submission. A query of every touched
pool at every part's submit was about 380 write-watch calls and 10 ms a frame of the submitting thread in Kokiri Forest,
while the GPU waited for the next part (RTX 4090, 2026-11-09). A page the CPU writes after its region's snapshot goes in
at the next guest submission's, as it does into a VRAM mirror. cpu (`wb_no_reup()`): the pages are read from the tables
without the stamps of a mirror's write-back (`stamp_page_as()`), since that write-back went through the shadow, which
holds those bytes already; with the other tables every storage buffer the game's compute shaders wrote was compared
again at the next copy-in.

- `} else`: outside the snapshot regions, or before the first guest submission: one query per unfrozen run.
- `if (!tracked)`: nothing tells which pages changed: all of them.
- `const uint64_t *ch = (cpu ? cgen_chunk : gen_chunk)[a >> GEN_CHUNK_SHIFT]`: a 2 MiB region lies in one chunk.

### `vk_mem_gpu_pending()`

Record time: the GPU may write [addr, addr+size) of CPU-visible memory. With shadow storage the pages it covers get
their base copies before the submission's copy-in (`vk_mem_shadow_sync()`) can reach them.

### `wb_no_reup()`

`NX2SARIA_WB_NO_REUP` (default 1; 0 = off): the pages a VRAM mirror wrote back into guest memory are not copied up into
it again at its next sync. `vk_gpu_written()` stamps the pages the GPU wrote, at the fence, so the texture cache
re-reads them; the same stamps made `mirror_sync()` copy them back up into the mirror the write-back had read them from:
the storage buffers the game's compute shaders write went down and came back up every frame, about 57 of the 60 MiB
copied up a frame outdoors (RTX 4080 SUPER, 2026-11-08) and 20 MiB a frame in Kokiri Forest (RTX 4090, PCIe 3.0,
2026-11-09). A write into a mirror (`vk_mem_mirrored()`, noted at record time) stamps only the texture tables
(`vk_gpu_written_back()`); `mirror_sync()` reads the CPU-side ones (`stamp_page_as()`), which a CPU write or a host path
still stamps.

### `shadow_bulk_write()`

`NX2SARIA_SHADOW_BULK_WRITE=1` (default 0): before a write-back stores into guest memory, the write watch's protection
of the whole range is cleared in one call (`plat_mem_prepare_write`). Where the watch protects pages (Linux), the first
store into each page otherwise takes a fault of its own; the pages count as written either way.

### `cpu_wrote_since()`

`NX2SARIA_SHADOW_CHECK`: the CPU wrote guest page pg after the copy-in that sgen stands for: a write-watch snapshot
stamped it later, or its write-watch bit is set and no snapshot has read it yet. A write-back that moves bytes into such
a page may have come after a CPU write it should have come before ("The game must not get ahead of the write-backs"
above); a page of which the CPU wrote other bytes counts too.

No `gen_lock`, as `lazy_clean()`: a stamp that lands meanwhile only blurs a count.

### `shadow_write_back()`

Completion thread: the GPU's bytes of [addr, addr+size) from the shadow into guest memory (`merge_page()` against the
base copies; a page without one whole). Returns the bytes moved.

A page without a base copy is one no write was noted on in this record (`vk_mem_gpu_pending()` notes every write the
replay records). It gets here when the pool the write was noted on was finalized before the write's fence and another
was made at its address: `shadow_forget()` dropped the base copies, and the new record's shadow is a copy of guest
memory from its own start that holds no byte of that write. Copied whole, it put those older bytes over everything the
CPU had written to the page since, for no gain. So the page is left alone when the record is younger than the submission
whose fence this is (sborn), or when the CPU has written the page since the shadow's last copy-in (`cpu_wrote_since()`:
a pool made again while that submission was still being recorded has the same value). Any other such page goes whole as
before: a write nothing noted, into a shadow that holds it.

- `int bulk = shadow_bulk_write()`: before the locks: its first call logs.
- `uint64_t value = vk_sync_cb_value()`: the fence being completed; a call from elsewhere reports submitted work.
- `int late = base && s_check && cpu_wrote_since(pg, r->sgen)`: before the merge's own stores into the page.
- `else if (r->sborn > value || cpu_wrote_since(pg, r->sgen)) s_stale_pages++`: the shadow cannot hold the write.
- `else memcpy((void *)(uintptr_t)a, s, pe - a), moved += pe - a`: a write never noted as pending.

### `table_wb_cb()`

Completion thread, at the fence of a submission whose record bound the device address table (`vk_mem_shadow_sync()`):
what the shaders stored into the pages found so far ("Pages a shader stores into" above) goes from the shadow into guest
memory, the bytes that differ from the page's base copy. A page whose shadow equals its base is passed over before guest
memory is touched, so a frozen region stays frozen. The pages that changed are stamped as a mirror's write-back is
(`swb_run()`): the stores are in the mirror already.

- `static uint64_t *wrote`: this thread only: the pages that changed.
- `int nwrote = 0, bulk = shadow_bulk_write(), back = wb_no_reup()`: before the locks: their first calls log.
- `if (!n) continue`: the store came after the compare above: the next fence takes it.

### `gpu_written()`

- back: the bytes are in a VRAM mirror, and its write-back brings them into the shadow; that submission's
  `shadow_wb_cb()`, after the replay's own callbacks, then copies them into guest memory.
- `back = back && wb_no_reup()`: read before `gen_lock`: its first call logs.

### `vk_gpu_written_back()`

- `NX2SARIA_WB_PROBE`: the bytes are in guest memory now; note who reads them next. With shadow storage they get there
  later, in the submission's `shadow_wb_cb()`, whose thaw would end the probe at once: that job arms it instead.
- `if (size && wb_lazy() && vk_mem_lookup(addr, size, &sp) && sp.pool)`: or newer ones stay in the mirror from here on.
- The check and the arm under `pool_lock`: `mirror_evict()` releases what is armed in the pool's range and drops its
  mirror under the exclusive lock, and `pool_fini()` takes the pool off the index under it. Without the lock an arm
  could land between the two and name the evicted mirror, which `mem_lazy_skip` gives back again later.

### `shadow_table_log()`

`shadow_lock` held, with the shadow line: the pages a shader stores into through the device address table ("shadow
storage" above), and with `NX2SARIA_SHADOW_CHECK` (`pool_lock` held shared too, for the index) what the copy-ins did and
cost and the pools whose stores were overwritten most since the last line.

- The pools that took the most whole pages, and what all pools of each flag set took.

### `shadow_log()`

- `if (s_check) plat_mutex_lock_shared(&pool_lock)`: before `shadow_lock`: `shadow_table_log()` walks the index.

### `vk_mem_dirty_submit()`

- `if (!(rt_trace & RT_TRACE_PERF) || vk_mirror_on) return`: the mirror's own line replaces the census.
- `uint32_t prev = dirty_sub`: the submit whose replay noted the bindings.
- `for (int i = 0; i < n;)`: one query per run of pools (sorted by address).
- `if (fresh) continue`: counted as first use.
- `if (lo < covered) lo = covered`: overlapping pools: each page once.

### `dirty_frame()`

One line per (kind, flags) group of the live pools, then the bytes the replay bound or copied per frame by pool class
since the previous census (the first one: since frame 1200). Logged at frame 1800 (in the world from the run-game
saves), at frame 600, and with `NX2SARIA_TRACE=perf` every 1800 frames.

### VRAM mirror of buffer-bound pools

An imported pool keeps vertices, indices, constants and storage buffers in system memory, so a discrete GPU reads them
over PCIe on every draw (GTX 1050 Ti and RTX 3090 Ti: about 110 ms of GPU time per frame in the field). In mirror mode
(device class discrete, `NX2SARIA_MEMORY`) an imported pool gets a device-local buffer over its imported range the first
time the replay uses it as a vertex, index, uniform, storage or indirect buffer (`vk_mem_use()`). From then on those
bindings, and the buffer copies, fills and uniform updates that touch the pool (`vk_mem_pick()`), use the mirror at the
same offsets. Guest memory stays what the CPU reads and writes; textures and every host path that reads or writes pool
bytes keep using it.

- CPU -> mirror: the first access of a submission to a mirror syncs it (`mirror_sync()`): the 2 MiB write-watch
  snapshots of its regions for this epoch (`snap_regions()`, shared with the texture checks: the OS bits keep one
  consumer, the shared page generations) and every page the CPU-side tables (`cgen_chunk`) stamped after the mirror's
  mgen goes into a copy. Where a region of the pool is not write-watched, so do the `nvnMemoryPoolFlushMappedRange`
  ranges, each on its own: one span from the lowest to the highest flush of a 252 MiB pool copied up to the whole pool
  for a few MiB flushed, 30-50 ms of GPU time over PCIe 3.0 x8 in crowded towns (`NX2SARIA_FLUSH_SPAN=1` restores it).
  Where the write watch covers the pool, a flushed page the CPU wrote is a written page already, and the game's flushes
  of whole pools it had not written cost 47-57 MiB of copies a frame in Kokiri Forest (2026-11-09);
  `NX2SARIA_FLUSH_UPLOAD=1` copies them anyway. The first use copies the whole range. The copies run in a prologue
  command buffer that `runtime/vk/vk_sync.c` submits right before the part that accessed the mirror first, between full
  barriers. `vk_mem_mirror_begin()` snaps the regions of the pools the previous submission accessed in a few merged
  queries.
- GPU -> guest: what the replay writes into a mirror (storage buffers its shaders write, buffer copies, fills, uniform
  updates, texture-to-buffer copies: `vk_mem_gpu_wrote()`) goes back into the imported buffer at the end of the
  submission, exact byte ranges, before the fence signals (`vk_mem_mirror_end()`). At the fence those pages get a new
  generation for the texture checks (`vk_gpu_written_back()`) but not in the CPU-side tables: the mirror holds the
  bytes.
- `NX2SARIA_MEMORY=verify`: every submission also copies up to `VERIFY_BYTES` of mirror pages, round robin, into host
  memory; at its fence the pages the CPU has not written since their sync are compared with guest memory.

A pool stays imported when its mirror would not fit the VRAM budget, its range is not write-watched, it overlaps another
pool or the allocation fails.

- `#define RETRY_SUBS 600`: a pool refused for the budget asks again this many submissions later (eviction).
- `static int mirror_gap = 2`: pre-snap: hot pools at most this many 2 MiB regions apart share one query.
- `static uint32_t pframe`: replay: frames begun (`vk_mem_mirror_begin()`: a place not after the last one's).
- `static int pplace = 99`: replay: the place of the last submission `vk_mem_mirror_begin()` saw.

### `presnap_place()`

`NX2SARIA_PRESNAP_PLACE=1` (default), 0, ab: a submission's pre-snap (`vk_mem_mirror_begin()`) takes the write-watch
snapshots of the pools the submission at its place in the last frame synced, not those the submission before it synced.
In Kokiri Forest the frame's first submission (#0) and its fourth (#3) use no pool, and their pre-snaps covered the
pools of the submission before them, #5's and #2's: 36.8 queries over 282 MiB a frame, 0.23-0.31 ms; then #2 and #5,
whose submissions before them use no pool either, queried their pools one at a time at their first use, 63.5 queries
over 254 MiB.

### `pm_note()`

replay: r synced in the submission being recorded.

### `pm_last()`

The places that synced r in the last frame.

### The mirrors' lists and counts

- `static PoolRec **mlist`: `pool_lock`: mirrored pools by `import_lo`.
- `static uint32_t msub`: replay: number of the submission being recorded.
- `static uint32_t mepoch`: replay: its write-watch snapshot epoch (`vk_texture_epoch`).
- `static MRange *pend, *wbl`: replay: copies for the next prologue; GPU writes into mirrors this submission.
- `static VkBufferCopy *mregions`: replay: `vkCmdCopyBuffer` regions.
- `static int flush_cap = FLUSH_RANGES`: ranges a pool keeps; 1 with `NX2SARIA_FLUSH_SPAN=1`.
- `static int flush_upload`: `NX2SARIA_FLUSH_UPLOAD=1`: flush ranges are copied where the write watch sees them too.

`NX2SARIA_FLUSH_SKIP_CLEAN` (`flush_clean_bytes` in the 0.5.0 and 0.6.0 fixes: flushed ranges upload only the pages the
CPU wrote) is this default since 0.6.1: a flush range of a write-watched pool is not copied at all; the key stays known
so an `nx2navi.ini` that sets it reads without a warning, and does nothing.

`MirrorAcc`:

- `uint64_t snap_bytes, merge_bytes, merge_dispatches, snap_allocs`: write-back merge.
- `uint64_t diff_bytes, diff_dispatches`: write-back by difference (`NX2SARIA_WB_DIFF`).
- `uint64_t page_bytes, flush_n, flush_bytes, flush_calls, flush_sum`: what `mirror_sync()` queues, by source.
- `uint64_t flush_skipped`: flush ranges not copied: the write watch covers them.
- `uint64_t cap_bytes, cap_ranges, cap_left, cap_t`: `mirror_capture()`: copied by the CPU, left to the GPU copy, its
  time.
- `static MirrorAcc mrep`: replay: this submission.
- `static MirrorAcc macc`: `mirror_stat_lock`: since the last summary.

### `mirror_mode()`

- `int mode = discrete ? 1 : 0`: 0 import, 1 mirror, 2 verify.
- Submissions a region stays unwritten before it is frozen (`NX2SARIA_FREEZE_IDLE`; 0: no freezing). 60 was chosen when
  a thaw by a write cost about 60 us: in a fight 100 of the 257 MiB queried a frame lay in regions idle for 30-299
  submissions. While the game streams an area, though, it creates and finalizes up to 1,500 memory pools a second, the
  driver pins and unpins the imported ones, and every VirtualQuery and VirtualProtect in the process waits behind that:
  a thaw took 10-20 ms with `frz_lock` held, which the replay needs too. The streaming buffers, idle for a few frames at
  a time, were frozen and thawed about five times a second, and walking from Kakariko Village through the Bottom of the
  Well to the Graveyard had a frame of 25-90 ms in 18-24% of its seconds (11% with freezing off, at 3-3.6 ms a frame
  more of write-watch queries over 3.5 GiB). At 1800 (about 3 s) a walk through Hyrule Field and Ganon's Castle froze
  0.1 regions a second, thawed none, queried 14 MiB in 0.03 ms a frame, and had such a frame in 1 of 253 seconds.
- NVIDIA's own driver imports guest memory without the cost shadow storage avoids on amdgpu (re-protecting imported
  pages), so there the copies are only the slower path: in Kokiri Forest on 0.7.1 (RTX 4090, driver 615.78) imports with
  VRAM mirrors ran at 51.9 frames a second and the copies at 27.4, and the copies hold about 5 GB. What a shader stores
  through the device address table into a pool with a shadow copy also does not come back to guest memory
  (`runtime/vk/vk_gpa.c`). The pools stay imports unless `NX2SARIA_SHADOW` asks for the copies. The open driver for the
  same cards keeps the copies: it is not measured. The AppImage's `play.sh` has set the same for this driver since
  0.6.0.
- Guest memory a memfd's (`NX2SARIA_DMABUF`, `runtime/platform/linux.c`): the pools are dma-buf imports ("dma-buf
  imports"), whatever the driver, unless `NX2SARIA_SHADOW=1` asks for the copies. A device that cannot import dma-bufs
  keeps the copies: a shared mapping's pages are no host-pointer import on RADV.
- The game's largest pools take about 300 MiB, each in one dma-buf: below 1 GiB (the module's default is 64 MB) some
  would get none, so the copies stay.
- Import is the integrated-GPU mode. On a discrete card every draw reads the pools over PCIe ("VRAM mirror of
  buffer-bound pools" above has the measurements), which is the difference between playable and 5 fps when it is not
  intended.

### `copy_ranges()`

Records the ranges as one `vkCmdCopyBuffer` per pool, overlapping and adjacent ranges merged; `to_mirror`: buf -> mbuf,
else mbuf -> buf. Returns the commands recorded; adds what they move to `*bytes` and `*copies`.

### `refuse()`

- A failed device-local allocation is asked again as a budget refusal is: memory that was short at the pool's first use
  may be there 600 submissions later, and a pool never asked again was read over the bus until the game finalized it.
  Untracked and overlapping pools stay refused: nothing about them changes.
- `static volatile int32_t logged[MR_N]`: four lines per reason: the refusals asked again would repeat theirs.

### Eviction

`NX2SARIA_MIRROR_EVICT`, default on; 0 = a pool over the budget stays imported, as before.

A pool whose mirror would not fit the VRAM budget stayed imported for good, however hot, while mirrors nothing had used
for minutes kept their VRAM; draws that read an imported pool over PCIe bring a discrete GPU down to a few frames per
second. Now a new mirror that does not fit takes the room of mirrors no submission synced for `MIRROR_IDLE` submissions,
least recently synced first (`mirror_evict()`), and only when they free enough; when they cannot, idle textures are
asked to make the rest (`vk_texture_want_room`) and the pool asks again `RETRY_SOON` submissions later, once per refusal
series; after that, and for any other pool refused for the budget, every `RETRY_SUBS` submissions (`vk_mem_use()`).
Asked every `RETRY_SOON` submissions without end, 113 refused pools made 1,795 retries in 6,000 frames. Mirrors never go
to make room for textures: evicted under VRAM pressure during a menu, 395 of them were refused again when the world came
back and the game ran at 4 fps. The budget counts the free space inside VMA's blocks as room (`vk_mem_vram_live()`).

A victim was neither synced, used nor written back in the submission being recorded (it has nothing in the copy,
write-back or snapshot lists), and every GPU write to it went back to guest memory at the end of the submission that
made it (`vk_mem_mirror_end()`), so the imported buffer holds everything. The device address table moves its pages back
to the imported buffer (`vk_gpa_remap_plain`); a shader of a submission in flight that reads the old record still finds
the mirror, destroyed only once the submission being recorded is done (`vk_defer_buffer()`). A mirror without a twin
(`twin_wanted()`) is never a victim: what shaders wrote into it through the table is in the mirror alone. The pool is
mirrored again, from a whole copy, on its next buffer use. Its freed bytes count as room at once (`evict_room()`), until
the GPU is past the submission and the destroy shows in VMA's figures.

- `#define MIRROR_SHARE 2`: the mirrors' share of the budget that the budget check never refuses: one in this many.
- `static volatile int64_t m_over_n, m_over_bytes`: mirrors made although the budget check failed (`mirror_create()`).
- `static uint64_t room_bytes, room_fence`: replay: bytes evicted that the budget may still count, until `room_fence`.

### `evict_unlocked()`

`NX2SARIA_WB_LAZY_UNLOCKED` (`runtime/memory.c` "Ranges in flight"), read once on the replay
thread, where `mirror_evict()` and `hot_check()` run: 1, the armed ranges of the mirrors they take away are fetched
before `pool_lock` is taken for the eviction; 0, under `pool_lock`, as in 0.10.1.

### `mirror_evict()`

Replay thread: evicts mirrors idle for at least `min_idle` submissions, least recently synced first, until need bytes
are freed; none when the candidates cannot free that much. Returns the bytes freed.

- `for (int i = 0; i < nm; i++)`: the MAXV least recently synced candidates, oldest first.
- `if (!r->twin && r->gpa_rec) continue`: the table's writes are in the mirror alone.
- `if (r->wb_kept) continue`: so are the bytes `wb_skip()` kept from guest memory.
- `uint64_t can = 0`: what all the candidates would free: short of need, none goes (no mirror lost for nothing).
- The victims, as the loop below takes them: their ranges and mirrors. What only a victim's mirror holds (its armed
  ranges the replay left there, `runtime/memory.c` "Lazy write-back") comes down before the mirror goes, and before the
  device address table names the imported buffer again: a shader still running reads through the table as it changes,
  and the fetch waits for all the work handed over. With `NX2SARIA_WB_LAZY_UNLOCKED=1` (`evict_unlocked()`) that fetch,
  one call for all the victims, runs here without `pool_lock`, which the completion thread's arms and every pool lookup
  wait for; with the setting at 0 it runs under the lock below, victim by victim, as in 0.10.1. The victims are mirrored
  still, so an arm can land on them before the lock is taken again: the release under the lock below takes that, usually
  nothing. The mirrored list changes meanwhile only by `vk_pool_fini()`, which takes a pool off it under the lock:
  `mirror_create()`, `hot_check()` and this run on the replay thread, under `use_lock` while chunks record. A victim
  finalized meanwhile is skipped below and the next candidate taken in its place, its fetch under the lock as in 0.10.1
  (rare: a finalize between the two locks). rel holds every candidate's range and mirror; the first nr are the victims.
- All under the exclusive lock: `vk_pool_fini()`, on another thread, takes a pool off the list under it, so a victim
  still on the list is a live pool here, and a finalize that follows finds it imported, its table pages on the imported
  buffer.
- `if (!on || r->mbuf != (VkBuffer)(uintptr_t)rel[k][2]) continue`: finalized meanwhile: its mirror is fini's.
- `mem_lazy_release((const uint64_t (*)[3])&rel[k], 1)`: all of it at 0 or past nr, else what was armed since.
- `mirror_forget(r)`: off the list, `mirror_gen` bumped: no buffer use gets the mirror from here on.
- `plat_mutex_lock(&flush_lock)`: flushes not copied yet: the next mirror starts from a whole copy.
- `for (int i = 0; i < n; i++) vk_defer_buffer(dead[i], dead_a[i])`: after the submission being recorded.

### Twins

`NX2SARIA_GPA_TWIN_MAX_KB`, default 0: every mirror has one.

Shaders write through the device address table into both a mirror and its pool's imported buffer, guest memory
(`runtime/vk/vk_gpa.c`, "The twin"): such writes are not recorded as GPU writes, so with the mirror alone the CPU never
saw them and a sync of a page the CPU wrote copied the stale guest page over them. On a discrete GPU the second write
crosses PCIe: on PCIe 3.0 the twins cost about 3 ms of GPU time a frame in Kokiri Forest (`NX2SARIA_GPA_TWIN=0` against
on). The writes that went wrong without them were the foliage counts and records in pools of 20 and 44 KiB.
`GPA_TWIN_MAX_KB=n` gives a twin only to mirrors of pools up to n KiB; a larger mirror keeps the table's writes to
itself (its record names the mirror twice, which sends the second write to the table's scratch page) and its buffer
fills stay in the mirror (`vk_mem_guest_twin()`). Such a mirror is never evicted (`mirror_evict()`), since guest memory
lacks what the shaders wrote into it. For measuring which pools need a twin: a pool that does and is left without draws
stretched or flickering foliage.

- `static int64_t twin_max = -1`: bytes; 0 no limit.

### When the device address table takes up a new mirror

`NX2SARIA_GPA_LATE`, default 1; 0 = at once, as before.

A mirror is made while the replay records a part of a submission, and its first copy of the pool is in that part's
prologue. The table is memory the shaders read as they run, so a change of it made here, at record time, reaches every
part already handed to the queue (the submissions before, and the earlier parts of this one), which run before that
copy: they read the mirror through the table, memory nothing had written yet, and wrote into it, and on a card over its
budget the block was one the driver had not made resident for them. A GTX 1650 4 GB lost its device that way at area
loads in every session: six reports of a GPU read fault at byte 0 of a mirror made in the faulting submission, each the
first byte of a new 2 MiB-aligned block, with mirrors being made over the budget; with the table kept empty the same
routes ran without a loss.

So the table is not touched here. The prologue that holds the mirror's first copy also holds, right after it, a store
into the pool's record (`vk_gpa_flip_cmd`): the GPU runs it in its place in the queue, so the parts before it read and
write guest memory through the table as if no mirror existed, and the parts from this one on read the mirror and write
both. What the earlier parts wrote through the table is in guest memory when the first copy reads it there (a twinned
pool's first copy is always the GPU's own copy from the imported buffer, `mirror_capture()`), so the mirror misses none
of it, and no shader reads through the table a state older than the one its place in the queue gives it.

- `static int n_gpa_flip`: as pend: mirrors whose record the next prologue hands over (`PoolRec.gpa_flip`).
- `static uint64_t n_gpa_late`: replay: records handed to their mirror in queue order, so far.

### `vk_mem_gpa_late()`

`--probe-vkmem`.

### `gpa_flip_cmds()`

Prologue, after the copies into the mirrors: the stores that hand the new mirrors' records over. Everything queued
before is done with the table before the stores run, and everything after sees them.

### `mirror_create()`

Replay thread: the pool's mirror on its first buffer use.

- `const uint64_t lo = r->lo`: the index is searched by it again when the mirror joins the list.
- `if (i < 0) return`: finalized meanwhile.
- `if (used + n + budget / 8 > budget && evict)`: room from mirrors nothing synced for a while.
- Over the budget still. The mirrors keep their place up to `MIRROR_SHARE` of the budget whatever else fills the VRAM:
  on a 6 GiB card a loaded area's textures alone pass the budget before the game's buffers are first used, every pool
  was refused, and the GPU read vertices, indices and storage over PCIe at each draw: 70 to 250 ms of GPU time a frame,
  5 to 13 fps, in two reports from RTX 3060 Laptop cards. A mirror the driver cannot keep in VRAM is paged to system
  memory, where the imported pool already is, so a mirror never reads slower than its pool, and what is drawn from every
  frame is not what the system pages out first. The textures are still asked for the room. Past that share a pool is
  refused as before.
- `if (evict && !r->asked_room)`: idle textures make the rest; asked again once they had the time.
- `r->asked_room = 1`: later refusals wait `RETRY_SUBS`: the textures had nothing to give.
- `__atomic_store_n(&r->mgen, gen_now, __ATOMIC_RELEASE)`: the whole-range copy below holds every write so far.
- The pool's buffers and vertices, read by every frame: the last video memory Windows moves out (memory priority).
- The device address table names the mirror: the GPU's writes go there, and the sync copies the CPU's. With a twin they
  also go to the imported buffer; without one (`twin_wanted()`) the record names the mirror twice. The table takes the
  mirror up in queue order, after its first copy (`gpa_late()`). Both are decided before `pool_lock` is taken:
  each may read its setting and log once.
- The pool may have been finalized on another thread since the check above, without a mirror to forget (its state said
  refused meanwhile): on the list, the mirror would outlive its record and be freed twice, or never. Its buffer goes
  once the GPU is past the submission being recorded, which nothing of it has reached.
- The mirror's state is complete before `pool_lock` drops. `pool_fini()` takes the pool off the list (`mirror_forget()`)
  only when it finds `MS_ON` there, and `destroy_rec()` frees the record without looking at the list: a finalize between
  the insertion and a later `MS_ON` left a freed record on mlist, and with `NX2SARIA_GPA_LATE=0` a table record that
  named the destroyed buffer. `acc_sub` before mstate: `vk_mem_use()`'s unlocked fast path reads mstate, then `acc_sub`.
- `r->gpa_flip = 1, n_gpa_flip++`: `vk_mem_mirror_prologue()`, with the copy `range_add()` queues below.

### `mirror_forget()`

- `pool_lock` exclusive: off the mirrored list.
- `if (r->gpa_flip) r->gpa_flip = 0, n_gpa_flip--`: gone before its first copy: its record stays as it is.

### Finalized pools

`retiring`: finalized pools waiting for `pool_retire()` (`pool_fini()` says which); `vk_mem_shutdown()` destroys those
whose callback never ran.

`pool_dead`: finalized pools the GPU is done with, destroyed by the next `vk_mem_collect()` outside every lock
(`pool_dead_drain()`). `pool_retire()` runs in the completion thread's callbacks, before it publishes done, so every
fence waiter waited for those destroys (`vmaDestroyBuffer` of the mirror, `vkDestroyBuffer` and `vkFreeMemory` of the
imported memory, which the driver unpins: up to 77 ms in one 240-submission window at an area change). It also runs from
`pool_wait_collect()` under `cmd_lock`, in the upload flush of whichever thread collects first: the replay's
per-submission flush or the game's presenting thread, with every upload and deferral waiting on `cmd_lock` meanwhile.
Under `retire_lock`.

- `n_pool_destroyed`: so far, for the "mem: sizes" line.

`pd_n`, `pd_ns` and `pd_max_ns`: the drains since the last "mem: sizes" line, by caller (`PD_BY_*`): records destroyed,
their time and the longest drain, in ns. The drain runs on whichever collect swaps the list out first, so part of it can
fall on the game's thread in `pool_fini()`; the line tells the two apart. `vk_mem_sizes()` takes and clears them.

### `pool_retiring()`

From `pool_fini()`: the record is kept until `pool_retire()` (or `vk_mem_shutdown()`).

### `pool_retire()`

The GPU is done with the pool: off the retiring list, onto `pool_dead`.

- Completion thread: destroys a finalized pool's record.

### `pool_dead_drain()`

`vk_mem_collect()` and `vk_mem_collect_try()` only, holding no lock: destroys the records `pool_retire()` put on
`pool_dead`. by names the caller for the "mem: sizes" line (`PD_BY_*`).

- `if (!n)`: another collect took them first.

### `mirror_sync()`

Replay thread, the first access of this submission: queues the copies of the pages written since the last sync.

- `const uint64_t *ch = cgen_chunk[a >> GEN_CHUNK_SHIFT]`: a 2 MiB region lies in one chunk.
- `__atomic_store_n(&r->mgen, gen_now, __ATOMIC_RELEASE)`: `lazy_clean()` reads it without `gen_lock`.
- `r->hot_up += sum`: the CPU wrote them (`mirror_hot`).
- `if (!untracked && !flush_upload)`: the write watch saw every write in them: the pages above.

### Shadow storage: the copy-ins before a part is submitted

And the device address table's readers.

### `shadow_pool_clean()`

`gen_lock` held: nothing of the pool was stamped since its shadow's last copy-in. Its regions have this epoch's
write-watch snapshot, none of them is untracked, and none holds a stamp newer than the shadow's sgen, which then stays
as it is. With cpu the stamps are the CPU-side tables', as `shadow_sync_pool()` reads them. The table's sync asks this
of every shadow in the table before it runs `shadow_sync_pool()` with its own timer reads, snapshot walk and region
loop: outdoors about 3,800 pools are in the table and a few hundred were written.

### `vk_mem_shadow_sync()`

- `int cpu = vk_mirror_on && wb_no_reup()`: before the locks: its first call logs.
- The table's readers: the record of this guest submission bound the table, and no part of it has synced yet. The
  callers are one at a time (`runtime/vk/vk_sync.c` `prologue()` under the submission lock, and the probe).
- `static uint32_t tab_epoch`: the guest submission the table's shadows were last synced for.
- `static uint64_t wb_fence`: the last fence that got the write-back job.
- What this submission's shaders store through the table goes back to guest memory at its fence, once (`table_wb_cb()`).
  Asked before the copy-ins below: the sync whose copy-ins find the first such page leaves it to the next guest
  submission that binds the table.
- `for (int i = 0; table && i < nstab; i++)`: the shadows the replay did not resolve.
- `if (r1 < SNAP_REGIONS) s_queries += snap_regions(r0, r1, epoch)`: so that `shadow_pool_clean()` can tell.
- A shadow with a VRAM mirror: the table names the mirror, which takes the same pages unless this guest submission
  synced it already (a pool bound as a buffer). The uploads read the shadows just brought up to date, and the prologue
  records them ahead of the part, after the write-backs of the submissions before it. No mirror is made for a reader of
  the table.

### The replay workers

`use_lock`: the replay workers (`runtime/vk/vk_exec.c`) bind and write from several threads at once: while a chunk
records, a mirror's first use in a submission (creation, sync) and the notes of GPU writes go under `use_lock`. A use
the submission already synced takes no lock, nor does the replay outside a parallel group (it alone records then).

### `vk_mem_use()`

- A refused pool not due for another try stays imported: nearly every pool in the field (two have mirrors).
- `vk_mem_note_live()`: the GPU reads this pool's guest memory as it runs (`vk_mem_take_live()`).

### `mem_use_locked()`

- `plat_dec64(&m_refused_n[r->refused_why])`: counted again if refused again.
- `r->mstate = MS_NONE, r->retry_sub = 0`: refuse sets it again.
- `if (r->mstate != MS_ON)`: the GPU reads this pool's guest memory as it runs (staged mirror uploads).

### `vk_mem_pick()`

- `if (r->mstate != MS_ON)`: guest memory itself, read by the GPU when it runs (`vk_mem_take_live()`).

### Write-back merge

A storage buffer a shader writes whole records of does not go back to guest memory as a plain copy. The CPU may write
fields of the same records while the GPU runs: the game's particle emitters write a new particle's whole record into a
free slot of a buffer that the simulation shaders update (the waterfall splashes and mist, 2026-11-02), and a plain copy
puts the mirror's stale copy of those words back over them. Instead:

- Record time (`gpu_wrote()` from `vk_mem_gpu_wrote_shader()`): each distinct range of the submission gets room in a
  snapshot chunk (`snap_add()`);
- Each part's prologue, after the CPU -> mirror copies: the mirror's words of the ranges the part added are copied into
  their snapshots (`snap_copies()`), the state before the part's GPU work. Words of an earlier part's writes that a
  later range overlaps are covered by the earlier range's own snapshot;
- `vk_mem_mirror_end()`: `runtime/shaders/wb_merge.comp` writes each 32-bit word that differs between the mirror and the
  snapshot into the pool's imported buffer (`wb_merge_record()`), one dispatch per pool and group of ranges.

Words the GPU left unchanged keep the CPU's value; a word both changed takes the GPU's, as a plain copy would.
`NX2SARIA_WB_MERGE=0` restores the plain copies; so does a pipeline or allocation failure.

- `#define MERGE_SPAN (64ull << 20)`: the bytes of one pool a merge dispatch binds at most.
- `#define MERGE_WORDS (65535u * 64u)`: the words one merge dispatch covers at most.
- `#define MERGE_MAX_LIMIT (256ull << 20)`: the snapshot pool's largest size (`snap_pool_cap()`).

### `merge_max()`

Larger written ranges stay plain copies (`NX2SARIA_WB_MERGE_MAX`, MiB, default 32). A plain copy writes the whole range
over PCIe at the end of each submission even when the shader changed a few words; a merge copies it within video memory
and writes only the words that changed. With a 4 MiB limit the field ran about 46 MiB of plain write-back a frame in
ranges of about 5.5 MiB, just above it; at 16 the same outdoor route went from a median 47.5 to 67.3 fps and from 11.3
to 7.2 ms of GPU time a frame (RTX 4080 SUPER, 120 fps limit, 2026-11-08). Kokiri Forest writes seven storage buffer
ranges of 4-15 MiB a frame; at 32, merged in `MERGE_PIECE` parts so that no dispatch covers more than `MERGE_WORDS`, the
plain copies of about 4 ms of GPU time a frame on PCIe 3.0 went (RTX 4090, 2026-11-09).

### Write-back by difference and the snapshot chunks

`NX2SARIA_WB_DIFF=1` (default), 0, ab: the written ranges over `DIFF_MIN`, up to `DIFF_MAX`, go back by difference
(`runtime/shaders/wb_diffcopy.comp`) instead of by the merge: snapshotted like the merge's, then the whole 64-byte lines
the GPU changed are copied and the rest is left alone. The merge writes word by word, and word-sized writes into host
memory cost the memory controller a read-modify-write each. In Kokiri Forest the compute programs bind whole pools of
6-15 MiB as writable, 46 MiB a frame, of which 3-4 % of the words change: by difference 5.4 % of the 46 MiB is copied,
in whole lines.

- `#define DIFF_WORDS (65535u * 256u)`: the words, with each range's alignment padding, one difference dispatch covers.
- `typedef struct { int n; SnapChunk c[]; } SnapFree`: a submission's chunks and range lists, pooled at its fence.
- `static SnapRange *snl`: replay: this submission's merged ranges; [0, `snl_copied`) have their snapshot copy.
- `static SnapChunk *chunks`: replay: this submission's snapshot chunks and merge range lists.

Chunks a finished submission used (`snap_free()`) wait here for `chunk_new()`, so a frame with merges creates no buffer
once the pool holds the frames in flight: a VMA allocation that needs a new memory block costs 8-11 ms on the replay
thread. Chunk sizes are powers of two so that one fits the next request; what the pool cannot hold is destroyed. The
pool holds about three frames of Kokiri Forest's snapshots (65 MiB a frame); at 16 MiB it held less than one frame's and
two buffers a frame were created and destroyed again (2026-11-09).

### `snap_pool_cap()`

The pool holds up to 1/64 of the VRAM budget, between 64 and 256 MiB: room for merges of multi-MiB ranges across the
submissions in flight. Its chunks prefer device memory and are not evictable, so a small card keeps less of them.



### `chunk_is_mlist()`

The merge lists' chunks (`NX2SARIA_MERGE_CPU`: one in device memory and its copy in host memory a submission, each twice
its merge ranges' bytes) stay outside the bound of `snap_pool_cap()`, up to `SNAP_POOL_LISTS` of them: when the
snapshots of the frames in flight had filled the pool while a list was out, the list's two chunks were destroyed at
every fence and made again for the next frame, 12-15 ms a frame in place of 8.

### The merge's pipelines

`WM`:

- `VkPipeline pipe, dpipe`: the merge, the difference copy.
- `int ltried, lok; VkDescriptorSetLayout ldsl, cdsl; VkPipelineLayout llayout, clayout; VkPipeline lpipe, cpipe;
  VkSemaphore asem`: `NX2SARIA_MERGE_CPU`: the merge into a list (`runtime/shaders/wb_mergelist.comp`), its copy to host
  memory (`runtime/shaders/wb_listcopy.comp`), and the timeline `merge_apply()` signals with a submission's value once
  it stored its list.
- `uint64_t asem_val`: completion thread: the value signalled last.

### `comp_pipe()`

A compute pipeline over nb storage buffers (push descriptors) with pc bytes of push constants.

### `diff_init()`

The difference copy's pipeline (`runtime/shaders/wb_diffcopy.comp`): the merge's bindings.

### `wb_direct()`

The write-back by difference and the merge through the CPU write guest memory directly: not with shadow storage, whose
bytes reach guest memory in the submission's `shadow_wb_cb()`, nor with the lazy write-back, whose fetch copies the
mirror over a range at any time.

### `wb_diff()`

`NX2SARIA_WB_DIFF`; after `merging()`, which makes the pipeline.



### `merge_cpu_mode()`

`NX2SARIA_MERGE_CPU=1` (default), 0, ab: a submission's merge goes back through the CPU.
`runtime/shaders/wb_mergelist.comp` compares as `runtime/shaders/wb_merge.comp`, but puts each changed word (its place
among the submission's list words, its value) into a list in device-local memory; `runtime/shaders/wb_listcopy.comp`
copies the list to host memory in whole lines, and the completion thread stores the words into guest memory
(`merge_apply()`) before the submission's value counts as done and before its other callbacks, the GPU events among them
(`vk_completion_add_first`). `runtime/shaders/wb_merge.comp`'s 4-byte writes into host memory cost the memory controller
a read-modify-write of their line each: in Kokiri Forest 0.9-1.0 ms of GPU a frame for 0.6 MiB of words, in the frame's
third submission, which its last and longest waits for. In the order the GPU's writes had:

- A prologue's copies from guest memory and a later submission's GPU write-backs into bytes a list still has to store
  wait for it (the replay thread waits: `apply_wait()`, on `WM.asem`, which `merge_apply()` signals), since a list
  stored after them would put older words over theirs;
- The lists are stored in submission order, and the list has room for every word of its ranges.

The CPU's stores set the write-watch bits of their pages, so those pages go up again at their next use (the uploads 1.2
to 3.0 MiB a frame there). Only a submission with at least `NX2SARIA_MERGE_CPU_MIN` KiB of merge ranges (default 256)
and at most `MERGE_CPU_MAX` (32 MiB: the list has room for every word of the ranges, twice their bytes) uses a list; the
others merge on the GPU.

`NX2SARIA_MERGE_CPU_CHECK=1`: the GPU merges as without it and the CPU compares the list with what the GPU wrote (the
cpu merge line counts the words that differ: a CPU write meanwhile, or a fault).

Measured there: one list a frame of 165,000-170,000 words, stored in 0.31-0.41 ms; the check run found them all equal to
the GPU's merge but 121 words the game zeroed as it exited. The GPU time 6.80 to 5.72 and 6.95 to 5.80 ms a frame in two
A/Bs (the merge 1.12 to 0.17 ms).

### `merge_cpu_min()`

`NX2SARIA_MERGE_CPU_MIN`, in bytes.

### `merge_cpu()`

A submission whose merge ranges come to `merge_bytes` goes through a list (decided once, at its write-back).

### The statistics and the jobs of `NX2SARIA_MERGE_CPU`

The statistics (any thread): lists stored, their words, the CPU time; the replay thread's
waits for a list ([0] a write-back, [1] a prologue's copies, [2] the executed wait) and their time; words past the room,
without a range, different from the GPU's (`NX2SARIA_MERGE_CPU_CHECK`).

- `typedef struct { uint32_t first, words; uint64_t host; } MergeSpan`: list places [first, first + words): from host.

`MergeJob`:

- `const uint8_t *map`: the host copy: the count, 12 bytes, then (place, word) pairs.
- `MergeSpan t[]`: by first.

### `merge_apply()`

Completion thread, before the other callbacks of its value.

- `if (g - j->t[k].first >= j->t[k].words)`: not in the last word's range.
- `if (j->fence > WM.asem_val)`: in check mode too: the comparison is where the store would be.

### The ring of list values

`mc_ring`: the values of the newest submissions with a list to store, in value order: the replay thread adds them, any
waiter looks for the newest at or below the value it waits for (`vk_mem_merge_cpu_wait()`).

- `mc_n`: atomic, lists so far.

### `apply_wait()`

Returns once the lists up to submission value v are stored (0: at once); the replay thread, or a waiter. why:
`mc_waits`' index.

- `vk_sync_release_held(v)`: a submission held for batching (`runtime/vk/vk_sync.c`) goes to the queue now.

### `vk_mem_merge_cpu_wait()`

The wait for a submission's own commands (`vk_sync_exec_wait`) leaves out the completion thread, and with it the lists
of the submissions before: the GPU's merges of those are through by then, in queue order, and the game goes on to
rewrite what they wrote. A list stored after that put the last frame's words over the game's (white frames while the
camera turns). So that wait ends once the lists up to its value are stored too, before the results the game reads
(`runtime/vk/vk_sync.c` `results_run()`), which come after the write-backs before them as with the GPU's merge. Any
thread.

### `chunk_new()`

A chunk of at least size bytes in `chunks[]`: the smallest fitting one of the pool, else a new buffer; -1 when the
allocation fails. host: 0 device memory (snapshots), `CHUNK_LIST` host memory the CPU writes (range lists),
`CHUNK_MARKS` host memory the CPU reads back (the merge's page marks, `shadow_wb_cb()`), `CHUNK_MLIST` device memory (a
merge list, `NX2SARIA_MERGE_CPU`), `CHUNK_MCOPY` its copy in host memory the CPU reads.

- `VK_BUFFER_USAGE_TRANSFER_SRC_BIT`: the source of `mirror_capture()`'s copies.

### `snap_add()`

Record time: room in a snapshot chunk for [off, off+size) of r's mirror, once per submission. 0: plain copy instead.

- `for (int i = nchunks; i-- > 0;)`: the newest snapshot chunk, if the range fits.

### `snap_copies()`

Prologue: the snapshot copies of the ranges added since the last prologue.

### `snap_cmp()`

The merges first, then the differences.

### `merge_dispatch()`

One dispatch: ranges s[0..n) share a pool and a chunk and lie in [base, base + span) of the pool. marks: the chunk of
the page marks, its first at `marks_off`, or -1 when the dispatch marks none (the range list stands in for the binding,
which the shader then does not write).

- `rec_push(WM.push, cb, VK_PIPELINE_BIND_POINT_COMPUTE, WM.layout, 0, 5, w)`: in call order (`runtime/vk/vk_rec.h`).

### `list_dispatch()`

One merge dispatch into the submission's list lbuf (`runtime/shaders/wb_mergelist.comp`, `NX2SARIA_MERGE_CPU`), as
`merge_dispatch()`; gbase: the list place of its first word, cap: the list's room.

- `rec_push(WM.push, cb, VK_PIPELINE_BIND_POINT_COMPUTE, WM.llayout, 0, 5, w)`: in call order (`runtime/vk/vk_rec.h`).

### `diff_dispatch()`

One difference dispatch (`runtime/shaders/wb_diffcopy.comp`), as `merge_dispatch()`.

- `rec_push(WM.push, cb, VK_PIPELINE_BIND_POINT_COMPUTE, WM.dlayout, 0, 4, w)`: in call order (`runtime/vk/vk_rec.h`).

### `diff_first()`

A difference range's first invocation: the first at or after total whose place within 32 matches the range's first word
x, so that the shader's clusters of 16 invocations cover whole 64-byte lines of the 128-byte aligned binding.

### `merge_group()`

The ranges s[i..j) one merge or difference dispatch covers (s sorted by `snap_cmp()`): one kind, pool and chunk, within
`MERGE_SPAN` and `MERGE_WORDS` (a difference: `DIFF_WORDS`, its span from a 128-byte boundary). Returns j; `*base`,
`*end`: the span, `*total`: its invocations.

### Shadow storage: the GPU's writes into VRAM mirrors back into guest memory

The mirror's write-back (`vk_mem_mirror_end()`) merges or copies them into the shadow, and the submission's job, added
to its fence after the replay's own callbacks, copies them on into guest memory: of a merge dispatch only the pages its
shader marked (`runtime/shaders/wb_merge.comp`), of a copy every page, compared with its base copy (`merge_page()`). The
fence callback of each noted range (`gpu_written()`) compared all of it: 65 MiB of storage buffers a frame in Kokiri
Forest, 9 ms a frame of the completion thread, behind which the game's fence waits sat (RTX 4090, 2026-11-09).

- `typedef struct { uint64_t addr, size; uint32_t mark0; } SwbRange`: guest range; its first page's mark, or `SWB_ALL`.
- `static SwbRange *swl`: replay: this submission's merge dispatches.
- `static int swl_marks = -1`: replay: their page marks' chunk, -1 none.

### `swb_run()`

Completion thread: [a, b) into guest memory, and the pages that changed stamped as the mirror's write-back.

### `shadow_wb_cb()`

Completion thread.

- `uint64_t run = 0`: start of the run of marked pages; guest address 0 is never a pool's.
- `NX2SARIA_WB_PROBE` (`vk_gpu_written_back()`): every range is in guest memory now; a later range's thaw would end the
  probe of a region an earlier one armed, so the probes come last.

### `swb_job()`

`vk_mem_mirror_end()`, before the snapshot chunks go back at the fence (`snap_free()` reuses the marks' chunk): the job
of this submission's merge dispatches and copies.

### `wb_merge_record()`

`vk_mem_mirror_end()`: the merge and difference dispatches of this submission's ranges. Returns the commands recorded; 0
with nsnl left as it was when the range list cannot be allocated (the caller copies them plainly). With shadow storage
each dispatch also marks the pages it changed, for `swb_job()`. fence: the submission's value; `*listed`: 1 when its
merges went into a list the CPU stores at the fence (`NX2SARIA_MERGE_CPU`).

- The dispatches: ranges of one kind, pool and snapshot chunk, within `MERGE_SPAN` bytes and `MERGE_WORDS` words.
- `uint64_t nmarks = 0`: bytes of page marks, each dispatch's at an aligned offset.
- `uint64_t mwords = 0`: the words of the merges.
- The range lists: at most one 16-byte entry per range, each dispatch's list at an aligned offset.
- `chunks[list].used = UINT64_MAX`: not a snapshot chunk.
- `NX2SARIA_MERGE_CPU`: the merges into a list in device memory (ml), copied to host memory (mh) for `merge_apply()`;
  room for every word of the ranges, so none is written by the GPU past it.
- `int kind = diff ? 1 : job ? 2 : 0`: a difference, a merge into the list, a merge.
- `uint32_t first = 0`: the range's first invocation.
- `if (job)`: the list to host memory in whole lines, for `merge_apply()` at the fence.
- `uint64_t groups = (1 + (mwords + 1) / 2 + 255) / 256`: 16 bytes an invocation, at most 256 groups.

### `snap_free()`

Completion thread: the submission is done with its chunks.

### Lazy write-back

`NX2SARIA_WB_LAZY`; the default by platform and driver, `runtime/wb_lazy_choice.h`; `runtime/memory.c` "Lazy
write-back".

A range a shader wrote goes back to guest memory once and is then armed (`vk_gpu_written_back()`, `mem_lazy_arm`). While
it stays armed, `gpu_wrote()` leaves the later writes of its pages in the mirror: no snapshot, no merge, no copy. When
the CPU touches a page, `runtime/memory.c` calls `lazy_fetch()` with the mirror and the imported buffer of the range: a
copy of its own, sent after every part handed over so far (the held ones first), and waited for. The part that is still
being recorded when that happens has already left its writes out: at its end (`lazy_part_check()`, from
`vk_mem_mirror_end()` or from the prologue of a part split off earlier) it writes back, as ever, what it skipped and
finds no longer armed; the snapshot for that merge is taken in the part's own prologue, before its GPU work. From that
check to the part's hand-over (`vk_mem_lazy_handed()`) `runtime/memory.c`'s window keeps a fault from releasing a range
unseen. Fetches run on several threads at once (`runtime/memory.c` "Ranges in flight": mostly no `runtime/memory.c` lock
is held around one, see `lazy_fetch()`), each in a slot of its own: a command pool (used by one thread at a time), its
command buffer and a fence. Fetches past `LZ_SLOTS` at once wait 20 us at a time for a slot (`lz_slot_waits` counts
them).

- `static volatile uint32_t lz_slot_free`: bit i: slot i is free.
- `static volatile int64_t lz_slot_waits`: fetches that found every slot busy, so far.
- `static volatile int32_t lz_state`: 0 not asked, 1 on, 2 off.
- `static LzSkip *lz_skip`: replay: what the part being recorded left in the mirror.
- `static int lz_win`: replay: the part's window is open (`lazy_part_check()`).
- `static uint64_t lz_skipped`: replay: bytes left in the mirror since the last log line.

### `lazy_fetch()`

`runtime/memory.c`'s fetch, with no `runtime/memory.c` lock held (`frz_lock` in a thaw under the lock: a mapping change,
the lazy heap's commit or a write fault `lazy_fault` did not take; and with `NX2SARIA_WB_LAZY_UNLOCKED=0`): each copy
`cp[i]`, [off, off + size) of the mirror h0 into the imported buffer h1, after all the GPU work handed over so far, in
one submission with one wait.

- `if (vk_device_is_lost() || n <= 0) return`: the run is ending (`runtime/vk/vk_device.c` `vk_device_lost_check()`):
  bytes stay.
- `vk_sync_flush_held()`: the held submissions wrote first.

### `lazy_clean()`

`runtime/memory.c`'s test before it arms [lo, hi) of the pool ctx (`frz_lock` held there, the pages already
inaccessible): the mirror holds every CPU write to them. A fetch copies the mirror over the whole range, not the merge
of the bytes the GPU changed that a write-back does, so a CPU write that came after the range's write-back reached guest
memory and before this arm, and that no sync of the mirror has carried up yet, would be overwritten by the mirror's
older bytes: a page stamped after the mirror's last sync (the CPU-write table, mgen), or a write-watch bit nobody has
read yet. No lock: the pages are inaccessible, so nothing stamps them meanwhile; the mirror's mgen only grows.

- `return mem_written_peek(lo, hi - lo, p, 1) == 0`: `UINT64_MAX` (not watched) counts as written.

### `vk_mem_lazy_decide()`

Whether the lazy write-back is wanted on this device, decided and logged once, when the driver is
known (`runtime/vk/vk_device.c` `create_device()`, after `vk_caps_init`): the player's setting, else the default of the
platform and driver (`runtime/wb_lazy_choice.h`). Never in import mode, where no pool has a mirror; with shadow storage,
where a fetch would copy the mirror into the pool's buffer, which is guest memory only when the pool is imported (a
shadow's bytes reach guest memory in the submission's `shadow_wb_cb()`); or with the write-back probe, which watches the
same pages.

- `lz_wanted`: -1 not decided, 0 off, 1 on.
- `lz_why`: the reason the log gave.

### `wb_lazy()`

- `int made = 0`: the slots that could be made, in order; one is enough.
- `vkDestroyCommandPool(c->device, lz_slot[made].pool, NULL)`: frees its command buffer too.

### `wb_skip()`

`NX2SARIA_WB_SKIP_MB` (default 0, off), a test switch for drivers that run without the lazy write-back (AMD's, and
outside Windows: `runtime/wb_lazy_choice.h`): there, what a binding written by a shader or a copy holds goes back to
guest memory at the end of every submission, up to 66 MiB a frame in Hyrule Field. With it, a written range of at least
this many MiB goes back only for its first `WB_SKIP_HEAD` bytes and the rest stays in the mirror. The CPU reads a few
words at the start of such buffers (12 bytes into ranges of 6.3 and 15.2 MiB: "Lazy write-back" in
[runtime.md](runtime.md), `runtime/memory.c`), which the head still carries; a CPU read further in would see older
bytes, and a CPU write further in would have the next sync copy that page's older guest bytes over what the GPU wrote
there. The pool's mirror is never evicted from then on, since guest memory lacks those bytes.

### The counts of GPU writes

- `static int64_t m_wb_skipped`: bytes `wb_skip()` kept in mirrors since the last summary.
- `static volatile uint64_t n_gpu_wrote`: `vk_mem_gpu_wrote_count()`.

### `gpu_wrote()`

- `if (lo > run)`: not armed: written back as ever.

### `lazy_part_check()`

The end of the part being recorded: what it left in the mirror for a range that is no longer armed (the CPU touched it
meanwhile) is written back after all. Opens `runtime/memory.c`'s window, which `vk_mem_lazy_handed()` closes.

### `vk_mem_lazy_part_end()`

`runtime/vk/vk_sync.c`, before a part's prologue is recorded: the check for a part that was split off (the last part's
ran in `vk_mem_mirror_end()` already).

### `vk_mem_lazy_handed()`

`runtime/vk/vk_sync.c`, once the part is held or handed to the queue: a fetch from here on has its writes.

### `gpu_wrote_range()`

- A binding to the end of its pool (no size) can span hundreds of MiB: those stay copies.
- `if (size > DIFF_MIN && size <= DIFF_MAX && wb_diff())`: whole, by difference.
- `range_add(&wbl, &nwbl, &cwbl, r, sp->offset + o, size - o)`: what found no snapshot room.

### `flush_add()`

Adds [a, b) to l, n sorted ranges that do not touch; returns the new count. Above cap ranges (l holds cap + 1) the two
nearest merge, so a full list copies the fewest bytes that were not flushed; cap 1 keeps one span over all flushes.

### `vk_mem_flush_add_probe()`

- `if (n != 3 || l[0].lo != 0x1000 || l[1].lo != 0x4000 || l[2].hi != 0x9000) return 1`: kept apart, sorted.
- `if (n != 3 || l[0].hi != 0x3000) return 2`: touching ranges join.
- `if (n != 1 || l[0].lo != 0x800 || l[0].hi != 0x9000) return 3`: one covers several.
- `n = flush_add(l, n, 0x12000, 0x13000, 2)`: over the cap: nearest merge.
- `if (n != 1 || l[0].lo != 0x10000 || l[0].hi != 0x41000) return 5`: `NX2SARIA_FLUSH_SPAN=1`.

### `freeze_pass()`

`gen_lock` exclusive: freezes up to `FREEZE_PER_SUB` candidate regions still idle. The query after
the protection collects the writes made before it took effect.

A freeze is a VirtualQuery and a VirtualProtect of 2 MiB, some microseconds as a rule; in the second after an area load,
while the loaders write and the driver imports and locks new memory, each took 8 to 17 ms. The newly loaded regions all
go idle together, so 32 of them came up in one submission: its mirror begin took 70.5 ms and the frame 78 ms, and one
freeze of 9 ms followed in each of the next 20 frames. A freeze only saves later write-watch queries (about a
microsecond per MiB per submission), so it can wait: a pass stops once it has spent `freeze_budget_ns`, and a freeze
over `FREEZE_SLOW_NS` holds the passes for `FREEZE_BACKOFF_NS`, doubled for each slow one up to 2 s.
`NX2SARIA_FREEZE_BUDGET_US` (default 1000; 0: no budget, no backoff and no hold at loads, as before).

The doubling starts over only once `NX2SARIA_FREEZE_QUIET_MS` (default 5000) have passed without a slow freeze. It
started over at the first fast freeze after a hold, and the slow ones are slow for seconds after a load while fast ones
come between them: after a load in Kakariko (2026-11-13) a 4-5 ms freeze came every 250 ms, 16 frames apart, five in a
row, and 4-10 ms freezes made frames of 25-52 ms cross the stutter line. 0: as before, a fast freeze starts the doubling
over.

And no pass runs while the texture pool makes an area's textures (`vk_exec_pool_loading`: the frames are held, and a
freeze in the load's frame took 32.4 ms) nor for `NX2SARIA_FREEZE_LOAD_HOLD_MS` (default 3000; 0: off) after that or
after an area's load (`perf_area_load_ns`), when the loaders write and the driver imports the new memory. The regions
that go idle meanwhile stay queued and are frozen after it; until then their write-watch queries go on, a few
microseconds per MiB per submission for those seconds. `runtime/freeze_hold.h` has the rules.

- `freeze_budget_ns`: ~0: not read yet.
- `static FrzHold frz`: `gen_lock`.
- `static uint64_t frz_held_passes, frz_slow, frz_load_held`: `gen_lock`.

### Hot mirrors

`NX2SARIA_MIRROR_HOT` (percent, 0 off): a mirrored pool whose copies each frame (CPU pages up, GPU writes back) reach
this share of its size goes back to being read and written by the GPU in system memory. On an RX 580 in Kokiri Forest a
handful of 6-16 MiB pools the CPU refills every frame and compute shaders write took 66 MiB of uploads and 76 MiB of
write-backs a frame through the mirrors, the GPU idle 7.8 ms a frame with those copies queued; read in place, only the
bytes the shaders touch cross the bus. Checked every `HOT_FRAMES` frames at the start of a submission, when the previous
one has queued its write-backs and nothing is pending for the next: the pool leaves the mirrored list, the device
address table points at its imported buffer again, and the mirror is freed once the submissions that may still use it
are done (`vk_defer_buffer()`, as `mirror_evict()`). Pools under `HOT_MIN` a frame stay mirrored. Only pools the CPU
does not write go: a pool read in place gives the GPU the game's bytes as they are when it reads them, not as they were
at the submit (the mirror's copy). The game refills its per-frame pools for the next frame while the replay
(asynchronous, up to a frame behind) and the GPU still draw the last one from them: the small grass and moss, drawn from
instances in such pools, flickered and slid over the ground as the camera turned, and with 0.8.0's mesh fallback every
mesh flashed (RX 580, Kokiri Forest, 2026-11-11; 18 pools of 2-15 MiB had gone, each refilled by the CPU every frame). A
pool with more than `HOT_UP_MAX` of CPU writes a frame stays mirrored.

### `hot_check()`

- `const char *v = cfg_value("NX2SARIA_MIRROR_HOT")`: the settings row's default (0, off) when unset.
- `if (fr - hot_frame0 < HOT_FRAMES || npend || nwbl) return`: nsnl: the previous submission's, recorded already.
- `uint64_t rel[8][3]`: each candidate's range and mirror, for `mem_lazy_release`.
- `r->hot_bytes = (uint64_t)per`: kept for the log line below.
- `r->hot_bytes = 0`: !twin with a table record: the table's writes are in the mirror alone (`mirror_evict()`).
- What only the mirrors hold comes down first, without `pool_lock` with `NX2SARIA_WB_LAZY_UNLOCKED=1`, as in
  `mirror_evict()` (which says why, and why the release under the lock below usually finds nothing); under it with the
  setting at 0. A candidate finalized while the lock was down is not replaced: its mirror goes with the pool, and the
  pools past the eighth wait for the next check as ever.
- `struct { uint64_t lo, n, per; uint32_t flags; } note[8]`: for the log line: a record may go once the lock drops.
- `if (!on || r->mbuf != (VkBuffer)(uintptr_t)rel[k][2]) continue`: finalized meanwhile: its mirror is fini's.
- `mem_lazy_release((const uint64_t (*)[3])&rel[k], 1)`: all of it at 0, else what was armed since.
- As `mirror_evict()`, under the exclusive lock (`vk_pool_fini()` takes pools off the list under it), but for good:
  `MS_REFUSED` with no retry (`retry_sub` 0), so neither `vk_mem_use()` nor the budget retries mirror it again.
- `plat_mutex_lock(&flush_lock)`: flushes not copied yet: the pool is read in place from now on.
- The pages name the imported buffer alone again (the twin was that buffer).
- `vk_defer_buffer(dead[k], dead_a[k])`: after the submission being recorded.

### `vk_mem_mirror_begin()`

- `if (!vk_mirror_on)`: import mode: the texture checks' snapshots still skip frozen regions.
- `nsnl = snl_copied = nchunks = 0`: the previous submission handed its chunks to `snap_free()`.
- `if (pl <= pplace) pframe++`: a new frame, though its first submissions may have had nothing to record.

### `span_add()`

A pool's spans in value order: those done go; one that overlaps or touches the last of its value joins it, and past
`SPAN_MAX` a value's spans become one (more bytes wait than need to, none fewer).

### `span_wait()`

The newest value of a span over [lo, hi) before `fence`.

### `cpu_span_add()`

`NX2SARIA_MERGE_CPU`: the bytes a submission's list stores (`merge_apply()`), until done.

### `cpu_span_wait()`

The newest list before `fence` that stores into [lo, hi) of r; done or not (the caller compares).

### The CPU's capture of written pages

`NX2SARIA_MIRROR_CAPTURE_KB`: the pages the CPU wrote since a VRAM mirror's last sync are copied out of guest memory by
the CPU when the part that needs them is handed to the queue, up to this many KiB a part, into a host buffer the
submission owns (a chunk of the snapshot pool, given back at its fence); the prologue copies them from there into the
mirror. Without it the prologue copies them from guest memory when the GPU runs it, a frame or more after the game's
submit when the GPU is behind: the GPU then reads whatever the game wrote there meanwhile, values meant for a later
frame. A pool stays with the GPU-time copy while a write-back of its mirror into guest memory is still on the GPU (the
CPU would read guest bytes that write-back has not reached yet and copy them over the mirror's newer ones), and when its
device address table writes reach guest memory as well (a twin: what they write is not recorded); so does a range with a
page that cannot be read without a fault (`mem_copy_readable`: a lazily armed range). The whole-pool copy of a mirror's
first use is larger than the limit and stays with the GPU.

Unset or 0 with `NX2SARIA_MIRROR_STAGE=1` (which the low-end GPU mode set up to 0.10.1): `CAPTURE_STAGE_KB`. A
submission whose prologue copied everything this way reads nothing of guest memory late, so its submit wait ends once
the replay has submitted it (`vk_sync_recorded_wait`); one with a range left to the GPU-time copy, or a draw that reads
a pool without a mirror, is noted (`vk_mem_note_live()`) and its submit wait is the GPU's. The first form of this
(0.8.0) copied while the replay recorded and left out neither the write-backs in flight nor the twins.

- `static volatile int32_t mirror_live`: the submission being recorded had the GPU read guest memory.

### `vk_mem_wb_lazy()`

The lazy write-back runs (`runtime/vk/vk_exec_draw.c` `op_update_ub()`).

### The capture's lists

- `static MRange *capm`: replay: `mirror_capture()`'s merged ranges.
- `static uint64_t *caps`: replay: the chunk offset of each, `UINT64_MAX` for one left to the GPU-time copy.
- `static uint8_t **capd, *capok`: replay: `mem_copy_readable`'s destinations and results.
- `static uint64_t (*capsrc)[2]`: replay: and its sources.

### `mirror_capture()`

Prologue, before `copy_ranges()`: takes the ranges of pend the CPU copies (`capture_cap()`) out of it and records their
copies from the chunk into the mirrors; what is left in pend goes the GPU-time way.

- `for (int i = 0; i < npend; i++)`: merged as `copy_ranges()` merges them.
- `chunks[k].used = UINT64_MAX`: not a snapshot chunk.
- The copies out of guest memory, all under one hold of `runtime/memory.c`'s freeze lock.

### `vk_mem_mirror_prologue()`

- `if (npend && merge_cpu_mode())`: after the merge lists still to be stored there (`NX2SARIA_MERGE_CPU`).
- `if (npend) vk_mem_note_live()`: left to the GPU-time copy: the GPU reads guest memory late.
- `if (snl_copied < nsnl)`: the snapshots read what the uploads above wrote.
- `gpa_flip_cmds(cb)`: after the new mirrors' first copies.

### Verify mode

### `page_dirty()`

The CPU wrote the page since the mirror's sync that mgen records: stamped later, or its write-watch bit is set.

- `return mem_written_peek(a, 1ull << GEN_PAGE_SHIFT, p, 1) != 0`: `UINT64_MAX` (not watched) counts as written.

### `verify_cb()`

Completion thread.

- `if (wb != j->sub && wb - j->sub < 0x80000000u)`: a later submission wrote back into it.
- `gen_lock`: a write-watch query clears the page's bit and stamps it in one hold (`vk_mem_changed()`, `mirror_sync()`),
  so the check sees one or the other; without it a page between the two counted as a mismatch.

### `vk_mem_mirror_end()`

- `lazy_part_check()`: before the merges are recorded: what the last part skipped and has to write back after all.
- The merge reads the mirror the submission's shaders wrote and writes the imported buffers, which earlier commands may
  have written too: its writes are ordered after theirs as well (a write after a write needs the write access on both
  sides of the dependency).
- `if (!m)`: no range list: copy them plainly.
- `if (merge_cpu_mode())`: this submission's GPU write-backs land after the lists still to be stored there.
- `if (shadow_on && (nswl || nwbl)) swb_job(fence)`: before `snap_free()` takes back the marks' chunk.
- `if (nchunks)`: the last part's prologue still records snapshot copies into them; back to the pool at the fence.
- `if (nsnl || nwbl) vk_exec_note_results(VK_RES_WRITEBACK)`: write-backs: guest memory gets the GPU's bytes at the
  fence.

### `pool_score()`

`NX2SARIA_TRACE=perf`, every 600 frames: the mirrored pools that moved the most bytes since the last list, per frame:
what their CPU writes and flushes copied up, how often a submission synced them, and what the GPU's writes sent back
(merged or copied). A pool whose upload is close to its size every frame is rewritten whole each frame.

### `mirror_pool_log()`

- `int k = nt < POOL_LOG_TOP ? nt++ : nt - 1`: insertion into `top[]`, largest first.

### `mirror_log()`

One summary line: with `NX2SARIA_TRACE=perf` every 60 frames, else at frames 600 and 1800.

- `NX2SARIA_WB_LAZY_TOUCH`=`<n>` (test, 0 off): at each of these lines this thread reads a byte of n armed ranges, as
  the game's CPU might, so the fault, the fetch and the hold run without waiting for the game to do it.

### The heaps and the address-space census

After `vram_log()`: every heap's usage as the driver reports it (`VK_EXT_memory_budget`) beside VMA's share
of it, VMA by memory type, and the process's private commit with its write-combined part (host-visible memory the driver
maps uncached), to tell which memory a long session grows. When the write-combined total (VMA's blocks where that is not
known) grew by 2 GiB since the last look, VMA's detailed map goes to vma-frame<N>.json beside the log, 4 at most. The
process's part walks the whole address space (`plat_mem_census`), 100-240 ms in a long session, so it runs on a thread
of its own (`census_main()`) and logs a line of its own; a look while the last walk still runs goes without it.

### `census_main()`

- `runtime/diag/perf.c` `ft_frame()` charges the frames the walk overlapped to "census". The line gives the frames
  presented when the walk began and ended (`span_frame`, which `vk_mem_census()` sets at every present) and its length:
  the frame numbers are the ones the "stutter: frame" and "perf: slow frame" lines carry, so it can be set beside the
  frames it held up.
- `perf_note_wc(wc, used)`: `runtime/diag/perf.c` `host_commit()` and the memory report (`system_log()`) take the device
  memory out.
- `static int dumps = -1`: -1: no baseline yet.
- Through a temporary file and a rename: nobody waits for this thread when the game closes, and a map cut short there
  would read as a whole one.

### `heap_log()`

- The address-space walk of the census (`plat_mem_census`, 100-240 ms of VirtualQuery) runs on its own thread, but it
  holds the process's address-space lock, which the game's threads take at every page fault and every VirtualAlloc: the
  present after each walk arrived 65-120 ms late. `NX2SARIA_MEM_CENSUS=1` asks for it. Not when this frame's present
  walked already (`runtime/diag/perf.c` `census_check()`, every 3600 frames and on commit growth).

### `pool_census_log()`

After the vram line: the imported pools the GPU reached through a resolve (a binding, copy or texture placement) and
those it never did, which hold host memory the driver counts in the host heap all the same (the 4.3 GiB imported at the
title screen exceed the 3.9 GiB host heap of an integrated GPU with 16 GB of RAM); then the VRAM mirrors no submission
synced for `MIRROR_IDLE` submissions, which a scene that needs the room could take back.

- `uint32_t sub = __atomic_load_n(&msub, __ATOMIC_RELAXED)`: the replay's; a log line may read it a submission late.

### `vram_use_log()`

After the heap lines: the driver's figure for the device-local heaps split by what holds it. VMA's blocks hold the
textures with their own memory (live and released), the VRAM mirrors, the physical pools, the ASTC decoder's scratch,
the headroom's spare blocks (their anchors), everything else VMA placed, and the free space inside the blocks, which the
driver counts as used until a block empties. The rest of the driver's figure was never allocated through VMA: the
swapchain, DLSS and frame generation (Streamline), compiled pipelines and the driver's own memory; DLSS's share as its
statistics report it, when known.

### `mirror_rows()`

The VRAM mirrors that lie in VMA's blocks, for the texture block table (`runtime/vk/vk_texture.c` "compaction"): a
mirror in a block keeps the block. malloc'd, `*n` rows; NULL with none. Read under the shared pool lock: a mirror comes
onto the list and leaves it, with its allocation, under the exclusive one (`mirror_create()`, `mirror_evict()`,
`mirror_forget()`).

- `if (ai.dedicatedMemory) continue`: memory of its own: in no block.

### `compact_look()`

`NX2SARIA_TEX_COMPACT`, when `vk_texture_compact_due` says the holes are due a look: the texture cache gets the mirrors'
rows, taken here because the pool lock is not to be taken under the texture lock, and plans a pass or not
(`runtime/vk/vk_texture.c` `vk_texture_compact_plan()`).

### `holes_log()`

Every `HOLES_PERIOD` frames (600 with `NX2SARIA_TRACE=perf`), in every run: how much of VMA's device-local blocks is
holes, and how much of that a compaction of the textures could give back. The first two figures are VMA's own (blocks
and allocations, memory of its own counted in both). The rest is of the memory type the textures use most, from the
block table (`runtime/vk/vk_texture.c` `compact_table()`): the blocks that hold a texture, with the textures and VRAM
mirrors in them; what is left of those blocks is holes and whatever else VMA placed there, which the table cannot tell
apart. A block at most half full by what is listed is "sparse"; when everything listed in it is a texture that could be
copied elsewhere or dropped (`runtime/vk/vk_tex_compact.h`), emptying it gives the whole block back. That last sum is
the most a compaction could return now: less when such a block holds memory of another kind too.

Without the perf trace the vram lines above are not logged, and a player's log of a long session said nothing about the
holes: four traced sessions of one PC ended with 1.4 to 2.2 GiB free inside 8.3 to 9.6 GiB of blocks.

- What keeps the other half-full blocks: a session of 2026-11-14 at 5120x2160 held 3 to 5 GiB free inside 140 to 180
  blocks, 48 to 85 of them at most half full, and the line said only that 1 to 18 of those could be emptied.

### `outside_watch()`

With `NX2SARIA_TRACE=perf` every 60 frames: a line when the device-local memory VMA did not allocate (`vram_use_log()`'s
"outside VMA") moved by `OUTSIDE_STEP` or more since the last line, so the log places it beside what did it. In one
session it grew from 0.6 to 2.6 GiB at the title screen, in two steps of about 1 GiB while the boot's compute pipelines
were being built (`runtime/vk/vk_pipeline.c`, stage library prebuild), and DLSS and frame generation add theirs when
they start.

### `vk_mem_lost_report()`

With the device lost report (`runtime/vk/vk_device.c`): the video memory in use and the budget the driver gives the
game. A lost device with the memory at or over its budget points at the memory; one far below it does not, and the
report of a card with 8 GB that ended this way held neither number.

### `vk_mem_fatal_note()`

The settings that change how the game's memory reaches the GPU, as they are in effect, for a fatal error's summary line
(`runtime/vk/vk_device.c` `fatal_note()`): the memory mode and the lazy write-back with its reason and state. No lock,
any thread.

### `vk_mem_stage_log()`

One line per boot stage (`runtime/vk/vk_pipeline.c` calls it after the pipeline cache load, the shader prewarm, the
recorded pipelines and the compute pipeline prebuild): the device-local memory in use, the part of it VMA did not
allocate (`outside_watch()`'s "outside VMA": pipelines and the driver's own, before the swapchain and the upscaler
exist), and the process's commit and working set. Subtracting two lines gives what a stage left behind: on an installed
copy the compute pipeline prebuild alone left 2.1 GiB of device memory on an RTX 5090, held by twenty very large compute
shaders no run dispatches (`runtime/vk/vk_pipeline.c`, "compute pipelines the boot keeps"), which this line was added to
place.

### `vram_log()`

With `NX2SARIA_TRACE=perf` every 600 frames: what holds device-local memory, to see what grows over a long session.
VMA's blocks and allocations on the device-local heaps, texture entries with their own memory, the VRAM mirrors and the
objects waiting for their last submission to retire.

### `vk_mem_class_log()`

Device class: INTEGRATED -> uma; a discrete GPU with a `DEVICE_LOCAL` heap of 1 GiB or more beside a heap that is not
device-local (host memory) -> discrete; otherwise uma (import everywhere). Sets the mirror mode.

### Upload batches and deferred destruction

- `typedef struct { VkBuffer buf; VmaAllocation alloc; VkDeviceSize size; } Staging`: size: the `VkBuffer`'s own.
- `typedef struct { VkBuffer buf; VmaAllocation alloc; uint8_t *map; } StgChunk`: a staging arena chunk.

`Batch`:

- `StgChunk *chunk`: arena chunks this batch's uploads took ranges of (`vk_staging_at()`), the last one open.
- `VkDeviceSize chunk_used`: bytes of the last chunk handed out.
- `uint64_t bytes`: staging bytes of its buffers (`upload_cap`).
- `int ts`: its pair of upload timestamps (`up_ts_*`), -1: not timed.
- `uint64_t sub_ns`: when it was handed to the queue (`plat_now_ns`), for the GPU timeline (`vk_gtl_note`).
- `static Batch *spare`: completed batches whose command buffer and fence are reused.

### Upload GPU time

With `NX2SARIA_TRACE=perf`: a timestamp at the start and the end of each upload batch, read when it retires. The
replay's GPU time (`runtime/vk/vk_sync.c`) times only its own parts; upload batches go to the queue as submissions of
their own, with the ASTC decoder's dispatches in them, so their time showed only as GPU idle "with it queued".
`cmd_lock` held throughout.

- `static int up_ts_state`: 0 not set up, 1 on, -1 off.
- `static uint64_t up_ts_ns, up_ts_n, up_ts_max_ns, up_ts_missed, up_n_batches`: since the last log line.

### `up_ts_read()`

`b` retired: its fence signalled.

### `up_ts_log()`

Every 60 frames.

### The batches' sequence

- `static uint64_t next_seq, done_seq`: batches handed out / completed (all batches up to `done_seq`).
- `static uint64_t tl_noted`: newest timeline value `runtime/vk/vk_sync.c` reported submitted.

### The staging cache and the staging arena

Staging buffers of a finished batch go back to a cache instead of the allocator: an upload-heavy scene creates and
retires the same few sizes thousands of times a frame (a texture re-upload, a texel copy, an ASTC decode), and each
`vmaCreateBuffer`/`vmaDestroyBuffer` pair walks the pool's block list under its lock. Bounded in count and bytes; a
buffer that does not fit is destroyed as before. The batch is complete when this runs, so nothing reads the bytes again.
Sizes are the `VkBuffers`' own: an allocation can be larger than its buffer, and a copy reads only the buffer's range.
`cmd_lock` held (retire, and `vk_staging()` between `vk_upload_begin()` and `vk_upload_end()`).

The staging arena: an upload of at most `ARENA_MAX` bytes takes an `ARENA_ALIGN`-aligned range of a persistent mapped
chunk (`vk_staging_at()`) instead of a buffer of its own. The chunks belong to the open batch while it records and go
back to the free list when its fence signals, so a frame's few hundred small texture uploads cost a pointer bump each
instead of a `vmaCreateBuffer` and `vmaDestroyBuffer` under the allocator's lock. `ARENA_KEEP` free chunks stay; the
rest are destroyed. 256-byte alignment covers every `bufferOffset` rule of a buffer-to-image copy.
`NX2SARIA_STG_ARENA=0` gives every upload its own buffer again.

`STG_FLUSH`: an upload batch holding this much staging (its buffers and arena chunks) is submitted at the outermost
`vk_upload_end()`, whatever `NX2SARIA_UPLOAD_BATCH_MB` says ("The staging pool" below).

### `batch_staging()`

`cmd_lock` held: the staging bytes of batch b (its buffers and arena chunks).

### `staging_cache_take()`

The cached staging buffer that fits n with the least waste: at most four times the request, so a small upload does not
take a block-sized buffer another upload wanted.

### `retire()`

Batches complete in submission order (one queue): retire from the front while their fences are signalled.

### The finalize of a used pool

(`pool_fini()`.) The finalize returned only once
the queue was idle and every upload batch was through (`vkQueueWaitIdle`), since the guest gets the storage back when it
returns. During an area load the queue also holds the load's texture uploads, which name no pool's storage: entering
Kokiri Forest, one finalize waited 418 ms for them, the frame took 462 ms. The game finalizes a pool once its own work
with it is done, as on the console, so the finalize now waits for the submissions issued so far (the game's work, the
mirror copies and write-backs in it) and returns. The pool's buffer and memory, which images cut from it
(`vk_texture_range_gone`) and the upload batches open or in flight may still name, go later: once those batches and the
submission being recorded are through (`pool_wait_collect()`, from the upload collector). `NX2SARIA_POOL_FINI_IDLE=1`
waits for the idle queue as before.

- `static PoolWait *pwait`: `retire_lock`.
- `static uint64_t pf_n, pf_ns, pf_max_ns, pf_logged`: narrowed finalizes, their waits (any thread; statistics only).

### `pool_wait_add()`

- Every upload batch handed out so far, and the submission another thread may be recording (`vk_sync_submit` sets issued
  after it), as defer stamps a deferred object.

### `pool_wait_collect()`

`cmd_lock` held (`collect_locked_out()`): the waiting pools whose batches and submission are through go now.

- `for (int i = 0; i < n; i++) pool_retire(go[i])`: off the retiring list; `vk_mem_collect()` destroys them.

### `collect_locked_out()`

The objects the GPU is done with are destroyed here (`out` != NULL: moved to `*out`, for the caller to destroy without
`cmd_lock`; `*nout` of them).



- `if (d->kind == DEAD_HOST)`: not a Vulkan object: nothing for the recorder's handle check.

### `NX2SARIA_UPLOAD_BATCH_MB`

`upload_cap`, from `NX2SARIA_UPLOAD_BATCH_MB` (0, the default, = no limit of its own; the low-end GPU mode set 256 up to
0.10.1): once the open batch's staging buffers hold that much, the outermost `vk_upload_end()` submits it. It does so
too once the batch's staging reaches `STG_FLUSH`, so that an area load's staging stays inside the staging pool's blocks
("The staging pool" below): 0 therefore means `STG_FLUSH` (64 MiB), not "never", and a value above `STG_FLUSH` changes
nothing, the 256 and the 128 to 1024 its menu row offered included. A save load made thousands of textures between two
replay submissions, and every one of them went into one batch: about 1.5 GB of raw ASTC blocks plus the decoder's work
in a single submission, which AMD's 23.10 driver on an RX 580 refused with `VK_ERROR_OUT_OF_DEVICE_MEMORY`. Submitted
early, a batch still precedes the replay submission that uses its textures (the queue keeps order), and its staging is
freed as it retires.

- `upload_cap` is `UINT64_MAX - 1` until it is read.

### `vk_mem_oom_report()`

An out-of-memory failure of a submit: every heap's use against its budget (the driver's numbers with
`VK_EXT_memory_budget`), VMA's blocks and allocations on it, and the upload batches, so the log names what ran out.
Takes no lock but the allocator's own: it may run with `cmd_lock` held.

### The staging pool

Staging memory comes from a pool of its own that keeps `STG_MIN_BLOCKS` blocks from its first use (at boot). In the
default pools VMA creates a block under the lock of its memory type (`VmaBlockVector::Allocate`), so a block made by the
headroom thread (`runtime/vk/vk_headroom.c`) or a burst of uploads held every staging allocation of the render thread:
one took 47-50 ms, for a few bytes, in the first frame after a save load. Larger buffers than a block go to the default
pool.

The pool's blocks are also all the staging an area load gets (`stg_create()`). Entering Hyrule Field uploads about 3,100
textures (1.2 GiB) in one frame; each upload kept its staging until its batch completed, so that frame made 221 new
staging buffers, 1.1 GiB of new host memory. On Linux every new block is a fresh kernel allocation that asks for 2-4 MiB
pages first, and with the free 2 MiB blocks used up the kernel compacted memory for each: on an RX 6800 XT (amdgpu, 32
GB) the load frame then took 2.4 to 4.7 s instead of 0.4, the texture thread in the kernel all along, the compaction
scanning millions of pages per half second. Now a load stays inside the blocks made at boot: a batch whose staging
reaches `STG_FLUSH` is submitted at the end of that upload (`vk_upload_end()`), and a staging buffer that does not fit
the blocks takes the room of the kept buffers and idle arena chunks, then of the batches in flight, which it waits for
in order (a wait covers all the queue's work submitted ahead of that batch, tens of ms on a heavy frame, with `cmd_lock`
held). Only when none of that makes room (the open batch's own staging, staging buffers their callers hold, the blocks'
fragmentation) does the pool grow by a block, and the log says so.

A discrete GPU keeps `STG_MIN_BLOCKS` blocks, 256 MiB of host memory for the whole run (128 MiB up to 0.9.2); an
integrated one, whose host memory is the machine's RAM and where low memory ended the game before
(`vk_mem_give_back()`), keeps `STG_MIN_BLOCKS_UMA`, 128 MiB, and its loads wait for batches sooner.

- `static uint64_t stg_room_cache, stg_room_wait, stg_room_new`: `cmd_lock`: how `stg_create()` found room (logged).
- `static uint64_t stg_wait_ticks, stg_wait_max`: `cmd_lock`: qpc ticks `stg_create()` waited for batches.

### `stg_create()`

`vmaCreateBuffer` of a staging buffer (`*ai` names `stg_pool`) inside the pool's blocks: when they are full, the kept
staging buffers and the arena's idle chunks go, then the oldest batch in flight is waited for and retired, until the
buffer fits. chunk: the buffer is an arena chunk, and `VK_INCOMPLETE` says that a retired batch gave chunks back to
`arena_free`, to take one of those instead. Without a batch to wait for (or once the device is lost) the pool grows. The
wait runs with `cmd_lock` held, and cannot leave it: the caller is inside `vk_upload_begin()` .. `vk_upload_end()` with
the open batch's command buffer in hand, and the staging buffer it asks for belongs to that batch, so letting go of the
lock would let another thread submit the batch under it. Meanwhile a replay submission that wants to send its uploads
(`vk_upload_flush()`) waits for the lock; its own commands would queue behind the same batches, so what it loses is
recording time, not GPU time. How long that is on a load is not measured anywhere yet: the waits are timed here and
logged (the first four, then at every doubling of their count), so a played session's log says.

### `vk_mem_give_back()`

A staging buffer the driver refused (`vk_staging()`): the buffers kept for reuse go back to the driver before one more
try, with the headroom's spare blocks (`vk_headroom_release`): the staging cache, the arena's free chunks and the
write-back snapshot pool, up to some hundred MiB. On an integrated GPU the device-local heap is the machine's RAM and
fills before the commit checks see anything; staging buffers of 11 KiB to 16 MiB refused there ended the game in Hyrule
Field and on the first scene of play (#465, #567). The replay's transient buffers and descriptor pools do the same
(`runtime/vk/vk_exec_draw.c` `scratch()`, `alloc_set`). Returns the bytes given back.

### The memory report past VMA's figures

Every report (`owner_log()`) adds three lines. "mem: outside VMA" follows the driver's usage and budget per heap against
VMA's blocks in it, so that what VMA never allocated (the driver's usage less VMA's blocks) can be followed over a
session, and counts what the runtime itself made outside VMA where no size can be known: the driver gives none for a
pipeline, a stage library or a descriptor pool (the boot stage lines, `vk_mem_stage_log()`, place their memory by
difference: 1.5 GiB for the recorded pipelines on an RTX 5090); the swapchain's pixels and DLSS's and FSR's figures are
sizes. "mem: system" splits the process's commit by owner: the game's heap and physical maps, the device-local memory on
Windows (the driver charges it to the process: in a session of 2026-11-13 the commit rose 4.6 GB at the first area while
VMA's device-local allocations rose 3.4 GiB, and fell with them later), VMA's host-visible blocks, the C runtime heap
with the shares of it the runtime can name, the data of the driver's pipeline caches, and the rest. "mem: growth" names
what grew by more than `GROWTH_STEP` since the report before and since the first report in play; every `RANK_EVERY_NS`
of play and at exit (`vk_mem_exit_log()`) "mem: growth ranking" ranks every category by its growth since play began and
marks those that only climbed (`runtime/diag/mem_growth.h`). Every figure is read at the report: sums the owners keep,
walks of the pipeline, shader and texture tables under their locks (a fraction of a millisecond, once a report), the C
runtime heap's summary. Nothing is counted per frame.

- `#define IDLE_MIN 10`: pipelines no lookup reached for this many minutes count as unused.
- `#define GROWTH_STEP (64ull << 20)`: `growth_take()` names a category that grew by more than this.
- `RANK_EVERY_NS`, the ranking's period in play: every 2 minutes, as a session ended without its exit lines after 7
  minutes of play had no
ranking at all with one every 10 (2026-11-13); a ranking is one line of figures the book already holds.

- `#define HEAP_SLOW_NS 2000000ull`: a heap summary that took longer is not asked for again.

`ReportFig`:

- `uint64_t usage, blocks, allocs`: the device-local heaps: the driver's usage, VMA's blocks and allocations.
- `uint64_t hblocks`: VMA's blocks on the other heaps.
- `uint64_t tex, pools, vma_other`: VMA's device-local allocations: textures, pools (mirrors and physical), the rest.
- `uint64_t heap_c, maps`: the game's heap as committed, its physical maps.

### `import_bytes()`

The game's memory imported for the GPU (`VK_EXT_external_memory_host`): the driver counts it in a host heap's usage.

### The figures the growth follows

`SysFig`: the figures of `system_log()` that the growth bookkeeping follows.

### `system_log()`

- The C runtime heap's summary holds the heap's lock while it adds up, and every malloc with it: asked before play only
  (and with `NX2SARIA_MEM_CENSUS` at every report), and no more once one was slow (`runtime/vk/vk_mem_heap.h`).
- The shaders' SPIR-V as held (`runtime/gfx/gfx_shader.c` "SPIR-V at rest"): against what it would be uncompressed, how
  many shaders hold it compressed alone, and the decodes readers' pins made, those on the drawing threads apart.
- `uint64_t cn, cb, cidle_n, cidle_b`: the NVN recording copies (`runtime/nvn/nvn_api_cmd.c`), malloc'd.
- Windows charges the device-local memory the driver allocates to the process's commit (`runtime/platform/platform.h`
  `plat_commit_counts_vram()`): the driver's figure, VMA's blocks and what VMA never allocated alike. The driver's views
  of that memory (write-combined, counted by a walk of the address space, `NX2SARIA_MEM_CENSUS`) came to 0.2-1.1 GiB
  more than its device-local figure in a session of 2026-11-13, and that part went to "other": "other system memory"
  grew 1,283 MiB in play while the process's own commit grew 307. With a walk less than a minute old the gap at that
  walk is a term of its own.
- `mem_budget_text(room, sizeof room)`: the levels the owners that give memory back go by (`runtime/mem_budget.h`).
- `uint32_t handles = plat_handle_count()`: they rose at area loads and never fell in a session of 2026-11-13.

### Growth bookkeeping

`runtime/diag/mem_growth.h` has the book. `grow_names`: the categories, in the order the lines name them.

- `static PlatMutex grow_lock = PLAT_MUTEX_INIT`: the book, between the report's thread and the exit's.
- `static uint64_t grow_t0, grow_ranked`: `plat_now_ns` at the first report in play, at the last ranking.

### `vk_mem_exit_log()`

At the process's end (the window's close, the game's exit, a timed run's end): the session's ranking, once it reached
play, from the figures of the last report; nothing is asked of the device here.

- `char when[48]`: the dma-buf spans of the run's end, which the lines at frames 600 and 1800 leave out.

### `vk_mem_rank_log()`

The same ranking at a moment of the caller's (`runtime/diag/perf.c` `session_report()`, when the window loses the
focus).

### The memory report

Every `HOLES_PERIOD` presents (600 with `NX2SARIA_TRACE=perf`), after the holes line, in every run.

The device-local memory the driver counts for the game split by what holds it: VMA's allocations by owner
(`runtime/vk/vk_vma_budget.c`, "owners"), with the textures split further by the texture cache's census
(`vk_texture_census`) and the pools into VRAM mirrors and physical pools; the free space inside VMA's blocks; and what
VMA never allocated (compiled pipelines, descriptor memory, the swapchain, the DLSS and frame generation features, the
driver's own), with DLSS's share as its statistics report it. Then the host-visible allocations (staging above all) and
the process's system memory with the game's own heap. Then the released textures' reuse (`vk_texture_reuse_log`). A
category that only climbs from line to line over a long session is a leak. The figures are sums the owners keep already,
one driver query, VMA's budget query and one walk of the texture entries (`vk_texture_census`), at the present that
logs; `vk_mem_census()` logs what the report took.

### `owner_log()`

- Read without their locks: an upload holds `cmd_lock` while it waits for a batch (`stg_create()`), tens of milliseconds
  the present would wait too; a figure a log reads a moment late does no harm.

### `staging_fail_now()`

`NX2SARIA_STAGING_FAIL_AT=n` (test): the n-th staging buffer the driver makes is taken as refused for memory once, so
the give-back runs (`vk_mem_give_back()`).

### `vk_staging()`

- `if (!alloc)`: the batch owns it, so a cached one of a fitting size serves.
- The staging pool can grow only in 64 MiB blocks of its one memory type. A smaller allocation may still fit in a
  default block or another host-visible type. Keep the same mapping requirements.
- `if (alloc) *alloc = a`: the caller destroys it (`vmaDestroyBuffer`) after `vk_upload_flush()`(1).

### `vk_staging_at()`

A range of the staging arena for an upload of the open batch (between `vk_upload_begin()` and `vk_upload_end()`): `*buf`
and `*off` name it, and the batch owns it until its fence signals. Larger requests, and every request with
`NX2SARIA_STG_ARENA=0`, get a buffer of their own at offset 0 (`vk_staging()`).

- `if (stg_pool) aci.pool = stg_pool`: an eighth of a staging block (`STG_BLOCK`).
- `if (r == VK_INCOMPLETE) k = arena_free[--narena_free]`: one already free, or one a waited-for batch gave back.
- `open_b.bytes += ARENA_CHUNK`: the staging the batch holds (`upload_cap`).

### `vk_upload_flush()`

- `if (vk_device_is_lost()) return 0`: the run is ending (`runtime/vk/vk_device.c` `vk_device_lost_check()`): no submit,
  no wait.
- The driver had no memory to take the batch, which ended the game here (five logs of three reports, all on integrated
  GPUs, one with 9 GB of commit to spare: the device's own heap was full). A refused submit leaves the command buffer
  and the fence as they were, so it can be made again: first the batches in flight are waited for, which gives their
  staging back and lets the objects that waited for them be destroyed, then the kept buffers and spare blocks go back to
  the driver (`vk_mem_give_back()`), as for a refused staging buffer. Only a second refusal ends the run.
- A lost device ends the wait at once and leaves the fence unsignalled: retire would take nothing and this loop would
  spin for good, holding the upload lock, instead of the run ending with the device's report.

### `defer()`

- The open batch may record into the object: it counts as the newest user. The GPU must also be past every submission
  that names it. `tl_noted` covers the submitted ones; an object the replay lets go of while it records a submission
  (`vk_exec_recording_fence`) may be named by that one too, not yet submitted: `runtime/vk/vk_texture.c`
  `slot_resolve()` hands a draw a released entry, and `entry_of` destroys released entries at that address later in the
  same submission. Stamped with the last submitted value, the object went as soon as the GPU reached it, before the
  submission that names it ran, and with the recorder thread (`runtime/vk/vk_rec.c`) behind, before the recorder made
  the call that names it. So every deferral is stamped with the submission being recorded. `NX2SARIA_DEFER_STAMP=0`
  brings back the old stamp.
- `vk_exec_recording_fence` knows only the recording thread's own submission. A texture a guest thread releases (the
  synchronous replay: `vk_texture_release`, `entry_of`, `sweep_released` on the game's loader threads) can be named by
  the submission another thread records meanwhile: that one is issued + 1 (`vk_sync_submit` sets issued after it).

### `vk_defer_buffer_try()`

For a caller that holds a lock the upload path takes inside `cmd_lock`: `runtime/vk/vk_astc.c` `vk_astc_scratch_trim()`
holds the decoder's lock, which `runtime/vk/vk_astc.c` `scratch_get()` takes under `vk_upload_begin()`'s `cmd_lock`, so
waiting for `cmd_lock` there would reverse that order. defer takes `cmd_lock` again, which the recursive lock allows.

### `collect_held()`

The objects go once `cmd_lock` is released: destroying an image and its memory takes up to milliseconds (the collect's
0.3 slow ones a frame took a median 0.26-0.43 ms, up to 1.9 ms), and the replay's uploads (`vk_upload_begin()`) wait for
`cmd_lock`. So do the finalized pools (`pool_dead`): destroyed here only, until shutdown.

`cmd_lock` held; released here.

### `vk_mem_collect()`

`pool_fini()`'s collect, on the game's thread (its only caller): the finalized pools it finds go here too, counted apart
from the completion thread's on the "mem: sizes" line.



### `vk_mem_collect_try()`

The completion thread's collect (`runtime/vk/vk_sync.c` `completion_main()`), after each new done. It only tries
`cmd_lock`: an upload flush holds that lock while it waits for the queue lock behind a present (FIFO, a frame limiter
inside the present call, frame generation) or for its batches' fences, and the single completion thread waiting there
held back the next value's done and every fence waiter for it. The deferred objects then go at a later completion, or
with any upload flush (`collect_locked()`); after `COLLECT_TRY_MAX` busy tries in a row it waits as `vk_mem_collect()`
does, which keeps the deferred list bounded. The finalized pools need only `retire_lock`: they go either way.

- `collect_skips`: collects that found `cmd_lock` busy, so far (the "mem: sizes" line).
- `busy`: busy tries in a row; the completion thread is the only caller.

### `vk_mem_shutdown()`

- `for (int i = 0; i < ndead; i++) dead[i].seq = 0, dead[i].tl = 0`: the device is idle.
- The staging cache and the arena's free chunks live in `stg_pool`: before it goes.
- `if (stg_pool) vmaDestroyPool(c->vma, stg_pool)`: after retire: no staging buffer is left in it.
- `nm = npend = nwbl = nsnl = snl_copied = nswl = n_gpa_flip = 0`: mirrors went with their pools.
- `nstouch = nstab = 0, s_sb_pages = 0`: shadows too (their base copies went with their records).
- `for (int i = 0; i < nretiring; i++) destroy_rec(c, retiring[i])`: finalized ones whose callback never ran.
- `npwait = 0`: those waiting for `pool_wait_collect()` were on the retiring list too.
- `for (int i = 0; i < npool_dead; i++) destroy_rec(c, pool_dead[i])`: retired ones no collect destroyed yet.
- `span_gc(0, 1)`: every pool is gone: so are the spans' references.

### `vk_mem_sizes()`

`NX2SARIA_TRACE=perf` "mem: sizes" (`runtime/diag/perf.c`): pool records, VRAM mirrors, the deferred destruction list,
upload batches, the staging cache and arena, the write-back merge snapshot pool and the shadow storage's base copies.
Counters only: `cmd_lock` is taken by every upload and release, so nothing is walked under it.

- Where those destroys ran since the last line, and how long they took (`pool_dead_drain()`).
- The page generations handed out so far (`gen_now`, 64 bits: they do not run out).
- Their tables: the chunks of both (8 bytes a page of each 256 MiB of guest address space stamped, allocated in full
  though only the pages holding stamps become resident) and the per-region maxima `reg_max` and `creg_max`.

### Texture storage pools

`NX2SARIA_POOL_TEX_RELEASE`, default on.

An imported pool is host memory the driver maps for the GPU and counts in its host heap, which Windows holds to half of
the installed memory (7.4 GiB of budget with 16 GB). Standing in Hyrule Field the game had 4.3 GiB imported in pools no
submission ever resolved, 2.7 GiB of it in three pools that hold nothing but the storage of textures the CPU provides:
the GPU samples those through the images the texture cache made from them and never reads the pool. With the video
memory past its budget too, the driver demoted device memory into the same heap, and once that heap was full a new
pool's import failed (`VK_ERROR_OUT_OF_DEVICE_MEMORY`) and the GPU wrote through a missing mapping: the device was lost.
Every 30 presents (`vk_texstat_frame`) such a pool (never resolved, no mirror, at least `TEX_POOL_PCT` percent of its
import CPU-provided texture storage, `vk_texture_cpu_ranges`) gives its import back: off the device address table, then
its buffer and memory once the GPU is past every submission issued until then. A resolve that meets it later imports it
again (`tex_pool_back()`) before it answers, so a binding still gets the pool.

### `tex_pool_free()`

Completion thread.

### `tex_pool_back()`

The replaying thread, no lock held: imports the pool at addr again; 1 when it was given back and now is not.

- `mem_thaw(r->lo, r->size)`: the import may need write access to the pages.

### `vk_mem_tex_pools_now()`

A failed import (`pool_init()`): gives back what it can now; 1 when that was anything.




### `vk_mem_tex_pools()`

- Candidates under the shared lock, given back one by one under the exclusive lock (still in the index, unused).

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
