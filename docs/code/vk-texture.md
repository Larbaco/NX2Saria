# runtime/vk: images, formats and texture decoding

The texture side of the Vulkan back end: the cache of images made for the game's textures, their uploads from the game's
memory, the descriptor and sampler slots, the write-back of what the GPU drew, and what gives way when memory runs short
(`vk_texture.c`); the format table (`vk_formats.c`); the ASTC decoder on the GPU (`vk_astc.c`); the colour adjustment on
the present (`vk_color.c`); and three rules kept in headers so that a test checks them without a device
(`vk_tex_compact.h`, `vk_tex_detail.h`, `vk_tex_release.h`). [README.md](README.md#the-pages-of-runtimevk) lists the
pages of `runtime/vk/`.

What the Switch's GPU and NVN do with textures that a port must reproduce: [maxwell.md](../maxwell.md). Where the area
sits in the host: "4. Graphics" in [architecture.md](../architecture.md). The log lines of the texture cache:
[logging.md](../logging.md). The memory the images live in, the page write generations and the upload batch:
[vk-memory.md](vk-memory.md). The CPU's ASTC decoder and the deswizzle: [gfx.md](gfx.md).

## `runtime/vk/vk_texture.c`

Vulkan textures of the graphics layer: a `VkImage` cache keyed by storage GPU address plus descriptor, uploads through
staging buffers with block-linear deswizzle and ASTC decode (`runtime/vk/vk_astc.c`), re-uploads when page write
generations (`runtime/vk/vk_memory.c`) show the storage written, ReadTexels / WriteTexels, the texture and sampler
descriptor slots, and `VkSamplers`.

A texture that mirrors its pool memory (`tex_mirrored()`) stops re-uploading once the GPU rendered into it (the
"GPU-owned" rule); a finalized slot follows live GPU content or samples its own image as kept, but never revives freed
CPU-backed storage.

Every image lives in `VK_IMAGE_LAYOUT_GENERAL`. Uploads are recorded into the upload batch of `runtime/vk/vk_memory.c`;
the replay submits it (`vk_upload_flush`) before a submission that samples the textures.

`ImgKey`: what makes two images interchangeable: the parameters `create_tex()` makes them with (`img_key_of()`). Image
reuse and the memory requirement cache match on it.

`Tex`:

- `VkTexImage pub`: first: `vk_texture_get()` hands out &pub.
- `tex_t t`: the newest texture object seen with this key.
- `VmaAllocation alloc`: own memory; NULL when bound into pool memory.
- `uint64_t bytes`: the size of alloc (0 without own memory), for the released entries' total.
- `uint64_t blk_mem, blk_size`: where alloc lies (`own_memory()`): the device memory handle and size of its block.
- `uint8_t blk_type, blk_own`: that memory's type; `blk_own`: memory of its own, not a part of one of VMA's blocks.
- `VmaAllocation bound_to`: the pool memory it is bound into.
- `uint64_t wgen`: page generation of the last upload check.
- `uint32_t checked`: `submit_epoch` of the last check from a draw binding.
- `uint32_t use_round`: `look_round` when a lookup last reached it (touch).
- `uint64_t use_frame`: `gfx_counters.frames` then.
- `uint64_t made_frame`: `gfx_counters.frames` when it was made or taken back as a new one (residency).
- `uint64_t samp_frame`: `gfx_counters.frames` when it was made, a marked shader last sampled it, or a lookup other than
  the texture pool refresh reached it ("disuse").
- `uint8_t retiring`: evicted by disuse, in `retiring[]`, not destroyed yet: the pool refresh leaves it out.
- `uint64_t retire_takes`: `vk_pool_takes` when it was retired.
- `uint64_t retire_frame`: `gfx_counters.frames` when it was retired.
- `uint64_t retire_due`: `vk_sync_issued` once every pool set was taken since (0: not yet); destroyed after it.
- `uint64_t up_retry`: `gfx_counters.frames` from which an upload that had no memory is tried again (upload).
- `uint64_t uid`: names the entry in its bucket's released set (`tex_gen_flip()`); 0 until its first release.
- `uint64_t rel_frame`: `gfx_counters.frames` when it was released (`drop_stale()`).
- `uint32_t back_gap`: most presents between a release and its taking back so far (`stale_after()`).
- `uint32_t n_up`: uploads of this entry (`vk_texture_debug()`).
- `int last_ch`: last `vk_mem_changed` result of `make_fresh()`: 1 written, 0 clean, -1 not tracked.
- `struct Tex *next_at`: other entries at the same storage address.
- `uint8_t gpu_dirty`: mirrored, and the GPU wrote the image after the last write-back to the storage.
- `uint64_t gpu_gen`: page generation when the GPU write that set `gpu_dirty` was recorded.
- `uint64_t gpu_seq`: order of that GPU write among all of them (`gpu_writes`).
- `uint64_t gpu_fence`: fence of the submission that made that write (0: not recorded in one).
- `uint64_t run_frame`: mirrored: the frame of the newest GPU write.
- `uint8_t run`: frames in a row up to `run_frame` with a GPU write, counted up to `WB_RUN`.
- `uint64_t wr_fence`: any entry: newest submission recorded to write its image (render, copy, clear, storage binding);
  ReadTexels waits for it instead of draining the replay queue.
- `uint8_t listed`: in `owned[]`.
- `uint8_t *snap`: a texture that fills no page: its storage bytes when the GPU write was recorded.
- `uint32_t wt_n`: WriteTexels into the storage since the last upload.
- `uint64_t chash`: `content_hash()` of the storage at its last upload (`chash_wanted()`).
- `uint8_t chash_ok`: chash holds it.
- `uint8_t chash_miss`: over `CHASH_MAX`: comparisons in a row that found other bytes (`CHASH_MISSES`).
- `struct Tex *rel_prev, *rel_next`: while released: the list of released entries, oldest first.
- `ImgKey ik`: the image's creation parameters.
- `uint8_t reusable`: its image may go to a new texture once it is released (own memory, GPU-only content).
- `uint8_t reused`: its image may still be in use from before (a released entry's, or its own restarted by
  `start_at()`): the first barrier waits for all earlier work.
- `uint8_t ru_on`: in the reuse list of ik.
- `uint8_t superseded`: released, and another descriptor's entry was made at its address since (`start_at()`).
- `uint32_t rel_epoch`: `submit_epoch` when it was released.
- `struct Tex *ru_prev, *ru_next`: the reuse list of ik, newest release first.
- `uint64_t gw_frame`: `gfx_counters.frames` at its creation or newest GPU write (`overwrite_others()`).
- `uint64_t scan_frame`: one more than the frame `vk_texture_gpu_written()` last looked for overwritten entries.
- `uint64_t ow_frame`: `gfx_counters.frames` when overwritten was set.
- `uint8_t overwritten`: released, and a texture at another address was drawn over its storage (`overwrite_others()`).
- `uint64_t kept_frame`: `drop_overwritten()`: the present (plus one) in which it was last left because it is used.
- `uint8_t in_gwin`: in `gwin[]`.
- `int pidx`: 1 + its index in `pent[]` (`vk_texture_pool_note_locked()`); 0: not listed.
- `int ph`: 1 + its index in `pref[]` (`pool_ref()`); 0: none.
- The fields from `had` to `og_back_frame`: where the entry and its content came from ("Origins" below), for the lines
  of `NX2SARIA_TRACE=perf`.
- `uint8_t had`: `HAD_*`: what put content into the image of an entry the CPU does not write.
- `uint8_t og_start`: `START_*`: how its content started (`start_at()`).
- `uint8_t og_drop`: `DROP_*`: the drop of this descriptor's last entry, when the ring still knew it.
- `uint8_t og_reimg`: made with the image of a released entry (`create_tex()`, reuse).
- `uint8_t bc1_try`: ASTC as BC3 that its first upload makes BC1 when every block is opaque (upload).
- `uint8_t said`: `marks_check()` wrote its line for this entry.
- `uint8_t sfill`: split fill: the CPU does not write it, and its image is the storage's bytes: 1 as a CPU-written one's
  until the GPU writes it, 2 once (`from_storage()`).
- `uint32_t og_backs`: times it was taken back after a release (`entry_of()`).
- `uint64_t og_frame`: `gfx_counters.frames` when it was made.
- `uint64_t og_drop_frame`: `gfx_counters.frames` of the drop `og_drop` names.
- `uint64_t og_back_frame`: `gfx_counters.frames` when it was last taken back.
- `VkTexImage *flat3d`: sampled as a 3D texture: its one-slice 3D copy (`vk_texture_flat3d_locked()`), else NULL.
- `uint32_t flat3d_up, flat3d_layer`: `n_up` and the layer of the copy's last refresh.
- `uint64_t flat3d_wr`: `wr_fence` then.
- `uint64_t used_frame`: presented frame (`gfx_counters.frames`) when a draw binding or lookup last named it.
- `uint8_t in_rt`: in `rts[]` (bound into pool memory, the GPU wrote it).
- `uint8_t compacting`: a compaction pass lists it (`CP.item`): `destroy_tex` takes it off the list.
- `uint8_t clobbered`: another image of another layout wrote over its pool memory since its own last write.

`TexSlot`: a slot keeps what `vk_texture_by_id()` resolved for it, so a draw binding skips the key lookup and the
address chain: the entry (ce), valid while the generation of its address (agen) is still cgen, the view and `has_view`
handed out with it, and the `submit_epoch` of the last freshness check through the slot (cepoch). Hot fields first: a
lookup that hits touches one or two cache lines.

- `struct Tex *ce`: NULL: nothing cached (registered since, or never resolved).
- `uint32_t cgen`: agen of the slot's address when ce was resolved.
- `uint8_t chas_view, used, has_view, finalized`: finalized survives eviction of the released Tex entry.
- `uint8_t cheld`: ce is NULL as the texture was evicted by disuse, valid while cgen is (`held_out()`).
- `uint8_t cwant`: a shader sampled its stand-in (`vk_pool_marks_read()`): `slot_resolve()` makes it at once.
- `uint8_t fresh`: registered and not resolved to a texture since (`vk_texture_register()`): `shown_take()`.
- `uint64_t khash`: hash of the descriptor key and view, 0 for CPU-written textures (`vk_texture_slot_khash_locked()`).
`SampSlot`: `s` is `vk_sampler_for()` of `p` once looked up, `sp` its point-sampled form
(`vk_sampler_point_by_id_locked()`), sc its comparing form (`vk_sampler_compare_by_id_locked()`), `sd[n - 1]` its form
for a texture made without n levels (`vk_sampler_detail_by_id_locked()`).

### The counts, the rate and the clock

- `static Tex *rel_head, *rel_tail`: released entries in release order (`sweep_released()` drops from the head).

`ru_n`, `ru_wt_n` and their bytes and times: re-uploads since the last perf line (`tex_lock`): all, and those of
textures a WriteTexels wrote since their last upload.

- `static uint64_t ru_same_n, ru_same_bytes`: re-uploads skipped: the pages were written, the texture's bytes were not.

`big_same_n` to `big_hash_bytes`: the same for textures over `CHASH_MAX` (`NX2SARIA_TEX_SAME_BIG`), since the last pool
log line: comparisons that found the bytes unchanged and their bytes, comparisons that found others, and uploads that
took a hash with their bytes.

`tex_alias`: `NX2SARIA_TEX_ALIAS` (default on; =0: a new image over live storage starts empty): a new descriptor over
the storage of a live image that the GPU wrote gets that image's content (`alias_copy()`). Counts since the last log
line.

`n_dinit_inline`, `n_dinit_inpass`, `n_dinit_upload`: new depth and stencil images (not reused) `init_new()` started at
the replay's position, those of them for which an open pass was ended, and those that went in the upload batch (no
replay recording), since the last perf line.

- `static uint32_t submit_epoch = 1`: bumped per queue submit: draw bindings check a texture's storage once per submit.

The thresholds further down that count presents (`STALE_FRAMES`, `GPU_STALE_FRAMES`, `OVERWRITTEN_KEEP`,
`DISUSE_FRAMES`) were set at 60 presents a second. Without a limit the game presents 85 to 240 a second (a menu), and
"120 presents" was then half a second and "30 s without a sample" 7 to 20 s: under memory pressure textures went three
times as readily as meant, and the ones still on screen showed their dark stand-ins until they were made again.
`tex_rate` is the presents per second as a multiple of 60 in 1/256, the highest of the last `RATE_N` seconds so that one
slow second does not shorten the thresholds; PRESENTS(n) is n presents at 60 in today's presents.
`NX2SARIA_TEX_RATE_LOW` (default 1; 0: as before): the rate follows the presents below 60 a second too, down to 15
(`runtime/vk/vk_tex_release.h` `trl_rate_sample()`). It stopped at 60, so at 21 presents a second a texture stayed out
of the VRAM pressure's reach for 5.7 s after its release and not 2, and the released ones held 2.1 GiB of a 12 GiB card
that was over its budget.

- `#define RATE_SAY_UNDER 240u`: the log's line: under 56 presents a second (a limit of 60 measures 59 now and then).

The wall clock of the presents (`runtime/vk/vk_tex_release.h` TrlClock): `rate_tick()` notes the present count about
once a second, so that the released textures' hold (`drop_aged()`) and the take-back ages (`vk_texture_reuse_log()`)
count seconds, not presents at whatever rate the game runs. `tex_lock`.

The seconds are seconds of play, not of the wall: a gap between two submissions counts at most `PLAY_GAP_MAX_NS`. A
window minimized for ten minutes (`freeze_idle`), a game stopped at a breakpoint or a long load then does not age the
released textures past their hold while nothing could take them back, and the first frames after it find the textures
the frames just before it released.

- `static uint64_t pclock_t0`: `kern_now_ns` at the first tick.
- `static uint64_t play_ns`: seconds of play so far, in ns (`rate_tick()`).
- `static uint64_t play_last`: `kern_now_ns` at the last submission `rate_tick()` saw.

### `rate_tick()`

Each submission, `tex_lock` held.

- One line at each crossing of `RATE_SAY_UNDER`, `RATE_SAY` times a run: the log then shows which waits the drops went
  by.

### The generations

`owned`, under `owned_lock`: mirrored textures the GPU wrote into, sorted by address (`vk_texture_cpu_stale()`,
`vk_texture_write_back()`). Changed under `tex_lock` and `owned_lock`; read under either.

- `static uint64_t max_owned`: largest storage size in `owned[]`.
- `static uint64_t gpu_writes`: GPU writes into mirrored textures so far: `Tex.gpu_seq`.

`tex_gen`: bumped whenever the entry a slot resolves to can change: an entry created or destroyed, released or taken
back into use, or a registered slot finalized (the follow reads `by_key`, `by_addr` and the entry and slot lifetime
flags).

`slots_gen`: bumped with `tex_gen` and whenever a slot is registered: what `vk_texture_by_id()` returns for any slot may
have changed.

- `static uint32_t samp_gen = 1`: bumped whenever a sampler slot is registered: `vk_sampler_by_id()` may answer
  otherwise.

`samp_retry_at`: after a sampler that could not be made (`make_sampler()`): when the next attempt is due
(`plat_now_ns`), 0 when none is pending. Until then `sampler_for()` makes no sampler that is not cached; at that time
`vk_sampler_gen()` bumps `samp_gen` once, so the descriptor caches and the sampler pool look their samplers up again and
the slots left without one try again.

- `static uint64_t n_checks`: draw-binding storage checks (`make_fresh_snap()`) since the last perf line.

`agen`, `tex_gen` per storage address (hashed): a slot resolves through the entries at its own address only
(`slot_resolve()`), so a slot's resolution stays valid while its address's generation does. The game finalizes and
initializes transient textures every frame: about 90 `tex_gen` and 160 `slots_gen` bumps per frame in the monsters
fight, one every 16 draws, which emptied every cache keyed on the global generations. 65536 buckets, the most the 16-bit
index of sinfo, `slot_bucket` and the dirty ring's bucket entries holds: with 4096, about 8 slots shared each bucket,
and every transient texture made or released at one address made the texture pool refresh (`runtime/vk/vk_exec_draw.c`
`pool_sync_entries()`) refill its neighbours too, most of its 3-3.7 ms per frame in Kokiri Forest.

`sinfo`: per slot id, for `vk_texture_slot_token()` without the lock: the registration serial (bits 16-47; 0: never
registered) and the address generation index (bits 0-15), one word so a reader gets a consistent pair. Fixed-size (slot
ids are 20 bits), unlike tslots, which moves when it grows.

- `static uint64_t n_same_regs`: `vk_texture_register()` calls that changed nothing (`vk_texture_same_regs()`).

### The dirty ring

Slots that may resolve differently since a position, for the texture pool descriptors (`runtime/vk/vk_exec_draw.c`
`pool_sync_entries()`), which otherwise looked at every registered slot after each change: the game registers slots all
through a submission, and the refresh walked the whole table about twenty times per submission (3.5 ms). A ring of
changes, each a slot id (registered) or an address bucket (an entry at that agen index changed: every slot registered
over it, kept in per-bucket lists). Lock held for all of it.

- `static int32_t *slot_next, *slot_prev`: per slot id: its bucket list links (`slot_prev` -2: in no list).

### The states of a bucket

`n_bumps`, `n_site` and `n_undone`: `tex_gen_bump()` calls, by site: destroy, create, take-back, release; and the
releases and take-backs that went back to a generation their bucket's state had before (`tex_gen_flip()`), for
`vk_texture_bucket_stats()`.

`agen_top`: the highest generation each bucket ever had. A new state takes the next one after it, not the one after the
current value, so a value a bucket went back from (`tex_gen_flip()`) is never given to another state.

`bepoch`: what a slot at an address resolves to depends on the entries there and on which of them are released
(`slot_resolve()`). Per bucket: epoch counts every change other than a release or a take-back (an entry made, destroyed,
retired, held or taken back with more than its flag changing), sig is the released entries' uids XORed, which a release
and a take-back each flip. (bucket, epoch, sig) names the state; gs remembers the generation each state got, so a bucket
that comes back to a state gets that state's generation back.

`in_lookup`: a slot lookup is resolving (`vk_texture_by_id_locked()` around `slot_resolve()`). The lookup's callers
stamp what they keep with the generation they read before it: the slot's own cache, the pool's tokens, the descriptor,
type and feedback memos (`runtime/vk/vk_exec_draw.c`). A lookup that itself changes the state (it takes a released entry
back for a slot registered since the release) leaves such a stamp naming the state it left, with a result that state
would not give: the next release of that entry is later than the slot's registration, the slot is stale then
(`slot_resolve()`), and the kept result would hand out the released entry, whose storage the game may be unmapping. So
the state such a lookup leaves loses its remembered generation (`tex_gen_flip()`): it gets a new one when the bucket is
next in it, and the stamp never matches again.

### `tex_gen_flip()`

`NX2SARIA_TEX_GEN_UNDO` (default 1; 0: every release and take-back is a new generation). The game finalizes and
initializes transient textures all through a frame: in a large town about 40,000 releases and as many take-backs per
3,600 submissions, each a bump of its address bucket, and the buckets with the most slots (1,300 registered over one
address) were the ones bumped most. The texture pool refresh refilled about 6,000 slots per submission for it. A release
or a take-back changes one entry's released flag and nothing else, so it moves the bucket between states it has usually
been in before: the textures the game keeps released and takes back for a moment, and those it releases and takes
straight back. The bucket then gets the generation that state had, and whatever was resolved in that state counts as
current again: the pool's tokens, the descriptor memo and the slots' own cached entries. A state seen for the first time
gets a new generation (`agen_top`), and any other change starts a new epoch, so a generation never stands for two
states. The global generations still move, so their readers look, and find the tokens equal. plain: the take-back of an
entry that was superseded or overwritten while released changes more than the flag, and is a change like any other.

One input of `slot_resolve()` is not in the state: the stale rule reads the slot's finalized flag, which a release sets
on the slots registered with its descriptor and a registration clears. A slot is stale at every later release once it is
at one, so what it resolves to in a released state stays right; the one that is not stale takes the entry back, which is
the lookup-made change `in_lookup` deals with.

- `if (in_lookup)`: the state this lookup leaves is not to get its generation again.

### Slot groups

The GPU-written slots of one address bucket that share a descriptor hash (`TexSlot.khash`) resolve alike under the same
address and entry generations, as the texture pool's memo has it (`runtime/vk/vk_exec_draw.c` `pool_fill()`). The game
keeps up to ~400 descriptors of each transient render target registered over its storage, and the fullest bucket, 1,281
slots, moves 8 times a submission in Kokiri Forest: 12,800 of the 17,600 slots a submission's dirty walks refilled held
their views already. The dirty walk can hand each group to the pool whole (`vk_texture_dirty_slots_locked()`'s gfn),
which leaves its members alone when their resolution did not change (`NX2SARIA_POOL_GROUP`). A slot is in its bucket's
own list (khash 0: a CPU-written texture) or in its group's member list, through the same links. A slot that joins a
group goes to its tail and leaves the group's stamp as it was: its registration put it on the dirty ring, so the walk
that follows refills it on its own, and the head, whose state the pool tests, is a member from before. (The game's 118
registrations a submission cleared the stamps of the groups they joined, and the walk at a submission's start went
member by member as before: 11,300 of 11,550 slots.)

- `static uint64_t n_grouped`: slots in groups.
- `static int32_t bucket_ghead[AGEN_N]`: per bucket: its first group (-1: none; set with the lists).
- `static int32_t *slot_grp`: per slot id: its group, -1 none.
- `static nvn_hmap grp_by = {.klen = sizeof(GrpKey)}`: -> group + 1.

### `slot_place()`

Places slot id in bucket b: at the tail of its group when it has a descriptor hash, else at the head of the bucket's own
list (the lists exist once a slot was registered: `vk_texture_register()`).

- `if (slot_prev[id] != -2)`: out of its old list.
- `int g = grp_get(kh, b)`: may move grp.

### `vk_texture_dirty_slots_locked()`

A bucket listed several times since `*pos` is walked once a round: the walk reads its slots' tokens after every change
listed before the round began (the lock is held). Changes the walk itself makes (an entry a lookup created) are the next
round's. A bucket's slot groups go to gfn when given, else their members to fn.

- `for (int32_t g = bucket_ghead[b]; g >= 0; g = grp[g].gnext)`: the walk registers nothing.

### `same_but_swizzle()`

Keys equal apart from the descriptor's swizzle selectors: the same storage, format and geometry.

### `follow_size_ok()`

Whether a slot (key k) may follow entry p, another descriptor at the same storage, when the slot's own entry is released
(`slot_resolve()`). The game's per-frame render targets share storage, so the live entry at an address is often a target
of another size that the frame laid out there: an effect's 1130x86 strip over the 480x270 half-size lighting term at a
1280x720 scene. Followed, the compute pass that writes the term and the full-screen pass that reads it both got the
strip's image, the term read back as zero over most of the screen and a jump attack's hit effect drew the right 44% of
the scene black. A slot follows only an entry of its own size; block-compressed formats are left out of the test, as a
GPU encoder renders their blocks through a smaller uncompressed view of the same storage. Counted in `n_follow_refused`.

### `mirror_refused()`

Textures whose content the CPU provides in pool memory. Display, compressible, zcull, depth, multisample and GPU-only
storage stays GPU-owned. `mirror_refused()`: NULL for such a texture, else the rule that leaves it out, in words for a
log line.

### Images

- The `DROP_*` values: why an entry was destroyed (`drop_tex()`), in the order of `drop_names`.
- The `START_*` values: how an entry's content started (`start_at()`; `Tex.og_start`), in the order of `start_names`.
- The `HAD_*` values: what put content into the image of an entry the CPU does not write (`Tex.had`); a GPU write is
  `pub.gpu_owned`.

### `bind_pool_memory()`

Binds `img` into the pool memory behind its GPU address (physical pools, virtual pools through the page table) when the
memory type, alignment and contiguous size allow; 0 otherwise.

An image that needs more bytes than its NVN storage (`guest_size`) is not bound (`pool_fits()`): the device's layout
puts texels or metadata in the excess, which lies over the next texture of the game's heap, so a draw into either one
scrambles the other. Depth: AMD's RDNA2 driver (26.10.1) puts the depth compression metadata after the depth data, and
above 1080p each depth write scrambled the next texture (garbage band at the top, pink glow, blocks in the grass, a blue
checkered scene in the menu). Colour: on an RX 6800 XT (26.10.1) the local exposure grid of 1.1.1's tonemap (32x24x64
rg16f, 3D) is laid out larger than its storage, and the light target the tonemap draws into starts 0x200 bytes after
that storage; the draw overwrote the grid's top slices while it sampled them, the grid then gave the brightest pixels a
gain of 2^-50, and lanterns and daylight turned to flickering black patches. `NX2SARIA_TEX_OVERSIZE_ALIAS=1` binds such
colour images as before (depth images stay out).

### `format_compressed()`

`vkCmdClearColorImage` takes no block-compressed format.

### Image reuse

`NX2SARIA_TEX_REUSE`, default on: a GPU-only texture that needs an image of its own takes the image of a released entry
made with the same parameters (ImgKey) instead of a new one, which then starts over as a new image would
(`init_image()`: undefined content, cleared). The game places its per-frame render targets with a linear allocator
behind small targets whose size follows an effect on screen (77x77 to 97x97), so every few frames the targets after them
move to new addresses, new descriptors whose images were made again: `vkCreateImage`, the memory requirements and the
binding of a dedicated allocation cost 8% of the replay in Kokiri Forest. The released entries of each parameter set are
kept in a list, newest release first; `create_tex()` takes the one nearest in address of those released at least two
frames earlier, never during the current submission's replay, whose later parts may still use it, and an overwritten one
("Moved render targets" below) before any other.

- `static nvn_hmap reuse = {.klen = sizeof(ImgKey)}`: ImgKey -> the newest released reusable entry.
- `static uint64_t n_reused, n_reuse_new`: since the last perf line: images taken from a released entry, made anew.
- `static uint64_t n_restarts`: superseded entries taken back (`start_at()`).

### Moved render targets

The game keeps about a thousand texture objects for its per-frame render targets and lays the targets out anew every
frame with a linear allocator in one GPU-only pool. A target early in that layout changes its size with an effect on
screen (811x811, then 778x778 a frame later in Kakariko Village), and every target after it then lies about 100 KB
further on: eleven targets, 55 MiB of images, at new addresses in each frame the size changes, each a new entry with an
image of its own, as few of them can lie in the pool memory itself (`pool_fits()`). The game finalizes an old texture
object only when it needs the object again, 90 to 300 frames later, and a descriptor in the texture pool goes on naming
it, so the entries were neither released nor idle: after 6,000 frames of walking through the village 1,043 of them held
6.1 GiB of device memory, against 233 MiB in the targets still drawn, and once released they waited `GPU_STALE_FRAMES`
more. A driver that charges device memory to the system's commit kept the commit low from then on, and the idle textures
of the area went and came back (`commit_evict()`).

On the console those targets are views of one pool's memory: when another target is drawn over a target's bytes, its
content is gone. `overwrite_others()` says so here. When the GPU writes a texture in the GPU-only address window
(`nvn_gpu_only`), each entry at another address whose storage overlaps it is marked overwritten and released as if the
game had finalized it. Its image then goes to the next texture made with the same parameters (`create_tex()`), or away
`OVERWRITTEN_KEEP` presents later (`drop_overwritten()`), or as early as an intact entry's while memory is short
(`drop_stale()`). Left alone are an entry at the same address (`start_at()` decides about those), an entry the written
texture lies inside (a level or a layer of it, which a game draws through a texture of its own) and an entry written in
this frame or the one before: two layouts that take turns within a frame keep their images. The mark goes when the game
takes the entry back (`entry_of()`). gwin: the entries this applies to (reusable, in the window), sorted by address;
`gwin_span`: the largest storage size one of them had, which bounds how far below an address an overlapping entry can
start.

- `static uint64_t n_overwritten, n_over_drops`: since the last perf line: entries marked, dropped by
  `drop_overwritten()`.

### The reuse's waits

`REUSE_UNUSED`: the presents a released entry must have gone unused before its image may go to another texture
(`create_tex()`): the pool's marks reach an entry a few presents after the draw that sampled it.

`REUSE_UNUSED_FULL`: the same for an overwritten entry while the card is past the VRAM pressure's share of its budget
(`over_share`): what the marks need and no more. A mark is read at most `MARKS_RB` copies of `MARKS_EVERY` submissions
after its draw (`runtime/vk/vk_exec_draw.c` `vk_exec_pool_marks()`), three frames of six submissions, so after eight
presents without a stamp none is on its way. At 3840x2160 the targets moved in 16 presents hold 3.1 GiB, which a 16 GiB
card does not have beside the 10 GiB of a town.

- `static int over_share`: `sweep_pressure()`: the driver's figure of the memory in use is past that share.

`REUSE_LOOK`: the released entries of one parameter set `create_tex()` looks at, newest release first. While the game's
layout moves in every frame, each frame releases every target of a set and none of them is unused yet for `REUSE_UNUSED`
presents: with 128 looked at, eight targets of one set a frame hid every older entry behind the young ones and each
texture got a new image.

`n_reuse_young`, `n_reuse_used`, `n_reuse_slot`: since the last perf line: released entries `create_tex()` passed by as
released too lately, as used, as named by a slot.

- `static uint64_t n_reuse_named`: released entries left out of the reuse because a current slot names them.

### `slot_names()`

`NX2SARIA_TEX_REUSE_NAMED` (default 0; 1: as before, any released entry's image may go): a registered slot that is not
finalized names e's descriptor, so a shader can still sample e through the pool. The game finalizes a texture object
while another one it initialized at the same storage with the same parameters stays registered (`vk_texture_release()`
leaves that slot current), and the entry they share is released all the same. Its image then went to the next texture of
its parameters two frames later (`create_tex()`, reuse), the entry was dropped, and the slot had nothing to show: the
pool does not make an entry for storage the CPU cannot read, so the slot kept its stand-in until the game drew into the
texture again. The game's 384x384 shadow maps are such textures: in a player's recording four slots of them were sampled
as stand-ins in the frame after three of their images had gone to other shadow maps, and the front of a house lost its
shadow for that frame. Such an entry keeps its image.

- `if (!n_reuse_named++ || !(n_reuse_named & (n_reuse_named - 1)))`: the first, then at each power of two.

### Split fill

`NX2SARIA_TEX_SPLIT_FILL`, `tex_split_fill` in `nx2saria.ini` or an existing `nx2navi.ini`: 1 the default, 0 off, 2
filled once.

Two descriptors can name one storage the CPU writes: a texture whose content the cache takes from the CPU
(`tex_mirrored()`), and a second one with a texture flag for which `tex_mirrored()` refuses. The second has no upload of
its own: its slot shows the first one's image for as long as that entry is live (`slot_resolve()`). Once the first is
released, a lookup of the second's slot makes the second an entry of its own (`entry_of()`), `start_at()` destroys the
released first one, and the new image has nothing to start from: cleared, never uploaded, and from then on what the slot
shows, also after the first descriptor is registered again and gets a new entry beside it. On the console both
descriptors read the same bytes. `runtime/gfx/tex_probe.c` `test_split()` makes the sequence and reads the image back:
zeros.

With split fill, a new entry the CPU does not write, at storage the CPU can see, starts with those bytes:

- By a copy on the GPU from the released CPU-written entry `start_at()` is about to destroy, when that image holds the
  storage as it is now (uploaded, and no write since its last check) or content the GPU wrote, and `alias_compatible()`
  allows;
- Else from the storage itself, uploaded as a CPU-written texture's is (upload).

With 1 the image then follows the storage as a CPU-written one's does (`from_storage()`: `make_fresh()`, the pool's
list) until the GPU or WriteTexels writes it. The texture pool refresh makes such an entry also for a slot whose
descriptor the game has let go of, long before the next texture's bytes arrive in that memory, and when both descriptors
are registered again the slot finds that entry: filled once (2), it shows the texture that was there before. Left out:
depth, stencil and multisample images, buffer textures, pools the CPU has no access to, storage over `SPLIT_FILL_MAX`,
and an entry made with a released entry's image, whose first transition is recorded at the replay's position, after the
upload batch (`init_new()`); so `create_tex()` gives an entry that is to be filled a new image. Nothing is filled where
a live entry at the address holds content the GPU wrote (that is what the new one should show, and `start_at()` copies
it when it can), nor with `NX2SARIA_TEX_ALIAS` off.

What follows the storage also takes what a stale descriptor's entry does not need: an entry made for a slot the game let
go of is uploaded again whenever its pages are written, by the next textures loaded into that memory, read in its own
format. The perf line counts those ("filled again after a write"); 2 does without them.

What it costs: an upload (a copy out of the storage, a decode for ASTC) for each such entry that finds no image to copy
from, where the image was only cleared before. A render target in a CPU pool that the game makes anew often pays that
every time and no longer takes a released entry's image; the perf line counts the fills, their bytes and their time.
Until the GPU first writes it, its storage is also checked for writes once a submission. What it can show: the bytes in
the storage, whatever they are. For a render target that is the last picture the game or a write-back left there, where
the image was zero before: a pass that reads a new target before anything drew into it sees that older picture instead
of nothing.

`n_split_copy` to `n_split_big`: since the last perf line: entries started by a copy from the released entry they
replace, filled from storage (with the bytes and the time), filled again after a write; left empty for a reused image,
for their size.

- `static uint64_t split_total[2]`: copies and fills so far (`vk_texture_split_counts()`).

### `vk_texture_split_fill_set()`

A test's hook (`runtime/gfx/tex_probe.c`): the mode from here on, and the copies and fills made so far.

### `split_wants()`

Whether split fill gives a new entry for t its storage's bytes, apart from the size (`SPLIT_FILL_MAX`).

### `from_storage()`

Whether e's image takes its content from the storage: a texture the CPU writes, or one split fill fills.

### `pool_fits()`

Whether an image with these requirements can lie in the pool memory at e's address (`*sp`: that memory).

- `oversize_alias`: `NX2SARIA_TEX_OVERSIZE_ALIAS` (`bind_pool_memory()`).
- `if (!req->size) return 0`: an image the device could not lay out (`image_size()`).

### The memory requirements of each parameter set

`img_reqs`: the memory requirements of each parameter set (ImgKey) an image was made with; images made with the same
parameters have the same requirements. With them `create_tex()` makes no image that `pool_fits()` would turn down: the
game's moving render targets mostly miss the device's alignment, and making the image only to read its requirements
(`vkGetImageMemoryRequirements` does most of the driver's layout work) cost 5% of the replay in Kokiri Forest.

- `static nvn_hmap img_reqs = {.klen = sizeof(ImgKey)}`: ImgKey -> `VkMemoryRequirements` (malloc'd).
- `static uint64_t n_bind_skips`: since the last perf line.
- `#define DEDICATED_MIN ((VkDeviceSize)32 << 20)`: `create_tex()`: images this large get memory of their own.

### `res_dedicated()`

With the texture residency on ("Residency" below) a texture the CPU provides gets memory of its own from this size:
moved to system memory it gives its memory back whole, where in one of VMA's blocks it left a hole the driver goes on
counting (no block in Hyrule Field held only cold textures).

### `block_views_ok()`

Agl's BC4 encoder draws a BC texture's blocks through an RGBA16UI view, one texel per 8-byte block. Vulkan does it with
an uncompressed view of a `BLOCK_TEXEL_VIEW_COMPATIBLE` image; `EXTENDED_USAGE` lets the BC image carry the
`COLOR_ATTACHMENT` usage that only such views support. Whether the device takes that combination for this format and
shape.

### `image_size()`

The memory the device wants for an image made from ci: 0 when it cannot lay it out. RADV on GFX9 (Vega, Renoir) hands
back a 0-byte image for a mipmapped block-compressed 3D image at least as deep as it is wide in blocks, with
`COLOR_ATTACHMENT` usage and no STORAGE: that usage asks for a thin 3D layout (Mesa `radv_image.c`), for which the GFX9
address library has no swizzle mode left (`gfx9addrlib.cpp`), and the error is dropped; VMA then refuses the 0-byte
allocation. The format properties call it supported, so only the size shows it.

### `init_image()`

UNDEFINED -> GENERAL; new depth / stencil images start cleared (the game copies depth without stencil, and the stencil
of those images must not be whatever the memory held).

A reused image (`create_tex()`) was another texture's: the transition waits for every earlier command, which may still
read or write it.

- GPU-only storage that got memory of its own starts at zero, as the pool it stands for (`runtime/vk/vk_memory.c`
  `alloc_physical()`). Images aliasing pool memory are not cleared: that would erase what other textures rendered there;
  mirrored images are uploaded from their storage on first use.

### `depth_init_inline()`

`NX2SARIA_DEPTH_INIT_INLINE` (default 1; 0: in the upload batch, as before): a new depth or stencil image goes through
`init_image()` at the replay's position too. Its clear writes the image's memory, and an image bound into pool memory
(`bind_pool_memory()`) shares that with the live texture at its address; from the upload batch, which runs before the
whole part being recorded (`runtime/vk/vk_sync.c` `split_part()`), the clear can zero what earlier commands of the part
still read as that texture. The game makes a new depth descriptor over a live one about 40 times a second at the castle
town gate (a 1280x720 DEPTH32F over a 640x360 one, its sun shadow maps as they resize), and for one frame a few times a
minute the shadowing went fully dark. At the replay's position `vk_exec_inline_begin`'s barrier orders the clear after
them.

### `init_new()`

A new entry's first transition. A reused image goes through it at the replay's current position: earlier commands of the
part being recorded may still use the image as the texture it was, and the upload batch runs before the part. So does a
new depth or stencil image (`depth_init_inline()`).

### `shadow_scale()`

`NX2SARIA_SHADOW_SCALE` (1 to 4): the game's shadow maps are made this many times larger on each side. The game renders
its sun cascades into a 960x960 DEPTH16 2D array of 4 layers and its static shadows into 1440x1440 and 720x720 arrays of
2, whatever the output resolution, and shrinks the cascades under load (872, 848, 800, 480 and so on). Those are made
larger here: render passes into them scale the game's viewports and scissors (`runtime/vk/vk_exec.c` `resolve_rt()`,
`pass_scale`), copies scale their regions (`runtime/vk/vk_exec_xfer.c` `sub_resolve()`), draws into them scale the
game's slope-scaled depth bias (`runtime/vk/vk_exec_draw.c` `slope_bias()`), and shaders sample them with normalised
coordinates, so the game sees nothing of it. A map whose storage the CPU writes (mirrored) or that aliases pool memory
keeps its size, as its texels must match the guest layout.

### `shadow_pcf()`

`NX2SARIA_SHADOW_PCF` (default 0): every depth-compare sampler filters linearly. Through a nearest filter each of the
game's shadow compares tests one texel and returns 0 or 1, so a shadow edge is a staircase of shadow map texels, and the
sun cascades follow the camera, so the stairs crawl along the ground as it moves (Hyrule Field's hills). Through a
linear filter the GPU tests the 2x2 texels around the coordinate and blends the four results (percentage-closer
filtering), which the game's own four compares then smooth further. With `NX2SARIA_SHADOW_SCALE` above 1 the samplers
filter linearly anyway (`make_sampler()`). Read once: samplers live until shutdown.

### `compare_sampler_note()`

The filters of the depth-compare samplers made, each kind once (8 at most): whether `NX2SARIA_SHADOW_PCF` changes
anything shows in the log (with every one already linear it does not). Caller holds the lock.

### ASTC as BC1

`NX2SARIA_ASTC_BC1`, `runtime/vk/vk_astc.c`.

A texture the CPU provides whose ASTC blocks all decode opaque is held as BC1, not BC3: the encoder then writes only the
colour half of the BC3 blocks it would have written (`runtime/shaders/bc3_encode.comp`), which BC1 decodes to the same
texels, in half the memory. Every ASTC texture is made BC3 (`create_tex()`); its first upload checks its blocks inside
the copy into staging it makes anyway (`astc_copy_opaque()`, `runtime/gfx/tex_astc.c` `astc_blocks_opaque()`), and when
all are opaque the entry gets a BC1 image in place of the BC3 one before the decode (`astc_reformat()`). A check of its
own where the texture is made cost 1.7 s of the render thread over a seven-minute teleport tour; the image made again
costs a fraction of that. The check covers every 16-byte block of the storage that is copied, the padding between levels
and layers included (zeros there decode as error blocks, opaque magenta; other bytes can only make it stricter). Each
later upload into a BC1 image checks again: the game may load other content into the same storage, and content with
alpha gets a BC3 image back before it is decoded. Blocks of the endpoint modes with alpha are decoded to tell; past
`ASTC_OPAQUE_DECODES` of them in one texture it counts as not opaque, as the check would cost more than it saves. Blocks
the CPU untiles first (not `astc_bl_regions()`) are checked in the storage before the copy (`astc_opaque()`).

### `astc_opaque()`

1 when every block of t's storage at addr decodes opaque; 0 when one does not or the storage is not mapped.

### Texture detail

`NX2SARIA_TEXTURE_DETAIL` = auto | full | reduced | low, `texture_detail` in `nx2saria.ini` or an existing
`nx2navi.ini`; read at the first texture, so a change waits for a restart.

The material textures of a loaded area are what fills a small card, and three quarters of a mipmapped texture is its
largest level. Over the budget Windows moves device memory into system memory and the frame rate falls to a fraction (a
tester's RTX 3060 Laptop, 6 GiB with a budget of 4,730 MiB, ran the world at 5 to 15 fps).

Measured on an RTX 5090 with the budget taken as those 4,730 MiB (`NX2SARIA_VRAM_BUDGET_MB`), a walk through Castle Town
at 1440p with DLSS Balanced, device memory in use at its peak and the textures in it:

| Level | In use | Textures | Of them ASTC as BC3 | ASTC texels decoded |
|---|---|---|---|---|
| full | 6,907 MiB | 4,302 MiB | 2,852 MiB | 2,589 M |
| reduced | 5,472 MiB | 2,357 MiB | 908 MiB | 826 M |
| low | 4,987 MiB | 1,997 MiB | 548 MiB | 503 M |

About 1.1 GiB of each is outside the textures and buffers (DLSS alone reports 0.7 GiB). What a level does not touch
stays: about 0.8 GiB of BC4, BC5 and BC7 textures the game ships in those formats, 0.65 GiB of render targets and 0.9
GiB of buffer mirrors.

Reduced makes such a texture without its largest level, low without its two largest when it is 1024x1024 or more
(`detail_skip()` decides). The image then holds the game's levels from `VkTexImage.skip` on: image level i is the game's
level i + skip, at that level's own size, so a level and a texel rectangle inside it mean the same in both and only the
level index moves. Every path from a game level to the image subtracts skip: the upload and its ASTC regions (upload,
`astc_bl_regions()`), WriteTexels and the write-back (`image_texels()`, `write_back_n()`), the views
(`runtime/vk/vk_exec_draw.c` `get_view()`) and the sampler's LOD clamp (`vk_sampler_detail_by_id_locked()`). Shaders
sample with normalised coordinates and derivatives, which pick the same game level as before wherever it is kept.

Only a texture for which that holds is reduced: an ASTC texture this runtime decodes (not one the device samples as
ASTC, whose copies run on the image, `runtime/vk/vk_exec_xfer.c` `sub_resolve()`), a 2D texture or 2D array,
block-linear, one sample, mipmapped, `DETAIL_MIN` texels or more each way, whose content the CPU provides
(`tex_mirrored()`). That leaves out render targets and storage images (no ASTC format can be either), 3D and cube
textures, the small and unmipped ones (interface art, glyph atlases, lookup tables, ramps) and whatever the GPU copies,
as texture copies of a decoded ASTC texture are not replayed at all. Two more are left whole:

- A texture made over a live one that holds GPU-written content, which `start_at()` copies in texel for texel;
- A texture a shader reads by texel position or whose size it asks for (`exact`): a fetch at a level the image lacks, or
  coordinates worked out from the size of another texture, would read the wrong texels. The translated shaders do that
  only through fixed bindings (`runtime/gfx/gfx_shader.c` `reflect_exact()`; none of 44,081 prepared shaders of game
  1.1.1 does it through the texture pool), so the replay sees the texture at the binding and asks for it whole
  (`vk_texture_exact_by_id_locked()`). The key then stays whole for the session.

The size of a 2D array is also asked for by the layer clamp the translator puts before array samples; that reads the
layer count alone, which no level changes, and does not count (`reflect_exact`).

auto: full from `DETAIL_FULL_MB` of device-local budget on, low under `DETAIL_LOW_MB`, reduced between. Textures give
way from 80% of the budget on (`sweep_pressure()`), so a level wants its use at or under that: full, 6.9 to 7.5 GiB in
the town and in Hyrule Field; reduced, 5.5 GiB, on 8 GiB cards (their budget is about 7.2 GiB); and every 6 GiB card,
whose budget stays under 5.5 GiB, gets low.

Full was chosen from 8,192 MiB, which is the cards of 10 GiB (their budgets start near 9 GiB). What full takes is more
than its textures, though: after a tour of eleven areas at 2560x1440 the driver counted 12.7 GiB in use on a 24 GiB
card, 4.6 GiB of it free space inside the allocator's blocks, which the pressure check leaves out and the driver does
not (`vk_mem_vram_live`). With that card filled to the room of an 8 GiB one, memory went to system memory and the same
tour ran at 29.6 fps with a 1% low of 11.4; with reduced, 42.8 and 36.4, and nothing spilled. So full starts at 10,240
MiB of budget, the cards of 12 GiB (a 12 GiB card's budget was 11,313 MiB in a player's log). An integrated GPU's budget
is a share of the system's memory, not memory of its own: one reported 10,361 MiB on a handheld with 16 GiB of memory in
all, auto chose full, and the game ended when a save was loaded until its player set reduced. auto never chooses full
there.

The choice itself is `runtime/vk/vk_tex_detail.h`'s `tex_detail_auto`, with the three budgets
(`tests/tex_detail_check.c`).

The levels above assume the ASTC textures end as BC3 (or stay ASTC), a byte a texel. Where they stay RGBA8 (astc = cpu,
a card without the decoder, `astc_bc` = 0) they take four times that, which is what one level left out gives back: under
this budget auto then goes one step lower. A card with 8 GiB that decoded on the CPU held two texture arrays of 852 MiB
each, was 560 MiB over its budget and lost its device.

- `static nvn_hmap exact = {.klen = sizeof(TexKey)}`: keys made whole whatever the setting; value 1.
- `static uint64_t n_exact`: textures made whole again so far.
- `static int64_t detail_budget_mb = -1`: the device-local budget auto went by, in MiB; 0: the device reports none.

### `detail_shared()`

The device's video memory is the system's own (an integrated GPU): "Texture detail" above says why auto never chooses
full there.

### `vk_texture_detail_auto()`

- `if (mb < 0 && !vk_ctx()) return 0`: asked before there is a device: nothing is kept.
- `if (mb < 0)`: read once: the budget moves with what else uses the card, the choice must not.

### `vk_texture_detail_name()`

The texture detail in effect, for a fatal error's summary line. No lock, any thread.

### `detail_most()`

The most levels a texture is made without: 0 full, 1 reduced, 2 low. `detail_chosen` holds the answer once it is made.

### `detail_skip()`

The top mip levels the image of `t` is made without (the rule of "Texture detail"). w, h, levels: the image as it would
be whole.

### Where texture images go when video memory runs short

Where texture images go when video memory runs short. An RX 580 (8 GiB, AMD 23.10) shows its memory as a 6 GiB
device-local heap and a 2 GiB device-local, host-visible one. Asked for device-local memory without a budget, VMA took
the first heap every time: a save load put 5.6 GiB into it against a budget of 5.1 GiB while the other heap held 69 MiB,
and the next submit failed with `VK_ERROR_OUT_OF_DEVICE_MEMORY`. Each step below allocates within the heaps' budgets
(`VK_EXT_memory_budget`): device-local memory of any heap; the second allocates as before, past the budget, where the
driver keeps it in video memory or moves it out itself. System memory is never asked for: on a GPU with one device-local
heap (an RTX 3050 Laptop, 4 GiB) a step for any memory within the budget put every texture past the budget in system
memory, 4096 and more images, which the process's commit then held: with 16 GiB of RAM the system's commit ran out, host
memory imports failed, and the GPU wrote through the address of a pool left without a buffer (device lost).

### `note_placement()`

`tex_lock` held.

### `image_alloc()`

- With pageable video memory (`NX2SARIA_VRAM_PAGEABLE`) Windows moves allocations out by their priority when the card is
  full: a texture goes before a render target, which every frame draws and reads. With the same priority for all, the
  GPU's frame in Hyrule Field took 19 ms in some runs and 45 to 50 ms in others, the textures the shaders sampled all in
  video memory.
- `note_placement(c, *a, i ? 2 : 0)`: 2: past the budget.

### `suballoc_image()`

A texture whose content the CPU provides (`tex_mirrored()`), up to `SUBALLOC_MAX`, takes memory from VMA's device
blocks: the driver prefers a dedicated allocation for most images, and `vmaCreateImage` follows that, so a scene load
(about 2700 new textures in one frame entering Kokiri Forest) made as many `vkAllocateMemory` calls, 43% of that 680 ms
frame. `vmaAllocateMemory` with the plain requirements sub-allocates instead, from the blocks the headroom thread keeps
ahead (`runtime/vk/vk_headroom.c`), within the budget: past it, the next device-local memory type that has room
(`image_alloc()` in `vma_image()`). Render targets keep `vmaCreateImage` and the driver's preference (`vma_image()`).
`VK_NULL_HANDLE` when the image does not take that memory.

- An image the driver requires (not only prefers) dedicated memory for keeps it: `vma_image()`.
- `.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, .priority = 0.3f}`: `image_alloc()`.

### `vma_image()`

An image made with `vmaCreateImage`, by `image_alloc()`'s steps. One of `DEDICATED_MIN` or more gets memory of its own:
freed inside one of VMA's 256 MiB blocks it left a hole the driver went on counting, with the rest of the block, against
the budget and the system's commit (0.5 to 1.8 GiB of such free space in a session at 5120x2160), until the block
emptied; its own memory goes back when it does.

### `own_memory()`

`e` has memory of its own (`e->alloc`): its size, and the block of VMA's it lies in, which the block table reads without
asking VMA for every entry (`compact_table()`). An allocation keeps its place for as long as it lives.

### Overwritten pool memory

`NX2SARIA_TEX_CLOBBER`. The game places transient render targets over each other in its memory pools, and the replay
binds their images into the same memory (`bind_pool_memory()`). An image of another format or size that renders over an
image's memory leaves it holding the other image's texels in the GPU's own tiled and compressed layout, which reads back
as unrelated numbers. The game's volumetric fog reads its 360x202 RGBA32F fog-volume texture at the start of a frame,
before it draws it again; at the end of the previous frame the 1440x810 R11G11B10F bloom target had been drawn over that
memory, and the fog lighting took those numbers as volume distances and types: teal splotches in the fog of Kokiri
Forest, on AMD and NVIDIA. An image read while in that state gets memory of its own from then on (`private_keys`), so it
keeps what it last drew; the read that found it takes an empty image once. `NX2SARIA_TEX_CLOBBER=0` keeps such images in
pool memory.

- `static Tex **rts`: pool-bound images the GPU wrote, sorted by address.
- `static uint64_t max_rt`: largest storage size in `rts[]`.
- `static uint64_t n_unaliased_period, n_unalias_kept`: since the last perf line: given own memory, left in pool memory.

### `mark_clobbered()`

`e`, bound into pool memory, was just written by the GPU: its own content is current, and the pool-bound images of
another layout whose storage overlaps it no longer hold theirs.

- `const char *why`: the same texels through both: a compatible image of the same shape (another size tiles apart).

### `create_tex()`

`defer_init`: the caller records the image's first transition (`init_image()` or `alias_copy()`).

- ASTC as BC1: a texture the CPU provides, not one that lies in pool memory, may turn BC1 at its first upload.
- `uint32_t skip = detail_skip(t, k, &f, w, h, levels, samples)`: texture detail: the image starts at this level.
- NVN2 titles (API 200+) number the texture flags differently: the game's compute targets (the title movie's RGBA8
  frames) carry 0x1, NVN1's DISPLAY. Every single-sample colour texture whose format allows it can be a storage image
  there, so a compute write into one is never dropped.
- Contact shadows (`runtime/vk/vk_contact.c`) darken the game's B10G11R11 scene colour in place from a compute shader.
- `VkImageCreateFlags flags = ds ? 0 : VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT`: format-reinterpreting views.
- Cube compatible only for cube targets: the replay makes cube views of nothing else (`runtime/vk/vk_exec_draw.c`
  `get_view()` checks the image's own view type). The flag on every square array of 6+ layers changed how AMD's Windows
  driver lays such an image out, and terrain material arrays sampled through it came out in rainbow blocks that moved
  with the mip level (seen on an RX 580 and a 7800 XT).
- NVN renders into a 3D texture slice by slice (a view's min layer is the depth plane, `nvn_mtl.m`), e.g. the grading
  LUT of the menu background blur. Vulkan needs a 2D view of the slice for that.
- `NX2SARIA_BLOCK_VIEWS_3D` (default 0): a 3D BC image also gets block views. On RDNA2 cards (driver 26.10.1, issue #5)
  the 32x32x32 BC4 volume the game creates at the logo, made `BLOCK_TEXEL_VIEW_COMPATIBLE` and `2D_ARRAY_COMPATIBLE`,
  took a GPU write just past the end of its memory: device lost at the first submit after it when the image ended its
  memory block. Draws into a 3D BC image go through a stand-in instead (`runtime/vk/vk_exec.c` `bc_target()`).
- `VkImageCreateInfo bc = ci`: BC formats: renderable through views of their blocks (`block_views_ok()`).
- `VkImageUsageFlags hidden = 0`: creation usage the replay never uses (views restrict theirs, `runtime/vk/vk_exec.c`).
- STORAGE usage keeps RADV on GFX9 from the thin layout (`image_size()`), so agl's BC4 encoder still renders into the
  volume; else the image is made without block views and its encoder passes are left out.
- Feedback loops (`runtime/vk/vk.h` `VkCtx.feedback`): every attachable image may be sampled by a draw of a pass that
  renders it, so it gets the usage now; adding it when a loop shows up would mean recreating the image (that lost the
  grass tile maps once).
- Storage in a physical or virtual pool: alias the pool memory, unless an image of another layout overwrote it there
  before it was read (`private_keys`).
- `if (q && !pool_fits(c, e, q, L.size, &sp))`: it would not bind there (`img_reqs`).
- An entry split fill is to fill gets a new image: `start_at()` uploads into it in the upload batch ("Split fill"
  above).
- `if (alias_has_gpu_content(p)) { to_fill = 0; break; }`: `start_at()` copies that in instead.
- `if (!img && gpu_only && !to_fill && tex_reuse_on())`: the image of a released entry with these parameters ("Image
  reuse" above).
- Not one released in this frame or the one before: the game releases its render targets and registers them again every
  frame, and DLSS's scene target (2560x1440 at a fixed address in Kokiri Forest) lost its image to a moving target of
  the same size in between, and DLSS its history (`runtime/vk/vk_dlaa.c` `evaluate()`), several times a second. Not one
  at this address either: its own descriptor comes back for it (`start_at()`). Of the others the one nearest in address,
  so a target the allocator moves keeps the image it had; an overwritten one first (`overwrite_others()`): nothing can
  ask for its content again.
- An overwritten one goes by what drew with it or sampled it (`sampled_lately()`), whatever slot still holds or names
  it. The game finalizes the texture object of a target it has moved only when it needs the object again, and until then
  the object's slot holds the entry: the pool stamps such an entry as used in every submission (`pool_touch()`), and the
  slot names it (`slot_names()`). By those two an overwritten target's image could not go to the target that took its
  place. While a shadow map in Kokiri Forest changed its size in every frame (1141x1141 to 1917x1917, a texel or two a
  frame, as the camera moved), every target laid out behind it moved with it: 945 images were made in 60 frames and 17
  reused, the released entries reached 1,931 with 8.6 GiB within five seconds of the area's load at 3416x1440, and a
  player's card of 20 GiB was full twice in five minutes (image creation failed, 4 fps, then the game ended). The slot
  of an entry whose image went shows its stand-in, as it does once `drop_overwritten()` took the entry.
- Still drawn with or sampled: its content is asked for.
- Not one a current slot still names (`slot_names()`), asked only of a candidate that would be the best.
- `vk_exec_image_gone(img)`: the views and records of the texture it was.
- A new image's handle can be one a destroyed image had. Views are purged with the image (`destroy_tex`), so none should
  be cached under it: one that is was made from a destroyed texture's record after its purge, and would reach this
  image's memory through the dead one's. Dropped here, and counted in the log.
- `if (!(p->swz[0] | p->swz[1] | p->swz[2] | p->swz[3]))`: descriptors built without `nvnTextureBuilderSetDefaults`.

### The released entries' list

- `static uint64_t rel_bytes`: own memory of the released entries (`Tex.bytes`).

- `static Tex *sweep_at`: where `sweep_released()` goes on.
- `static Tex *aged_at`: where `drop_aged()` goes on.
- `static Tex *press_at`: where `sweep_pressure()`'s walk of the released entries goes on.

### The pool's entry list

`PEnt`: the entries the bindless texture pool's slots resolved to, for a submission's freshness check
(`make_fresh_snap()` once per entry), which looked at every slot instead: 33,000 slots, 1.5 ms of the replay thread a
submission in Kokiri Forest. `runtime/vk/vk_exec_draw.c` `pool_fill()` notes a slot's entry after it resolved the slot;
only entries whose content the CPU provides are listed (the check does nothing for the others; one the GPU took over,
`gpu_owned`, never uploads again and leaves), and an entry stays listed until it is destroyed: the list holds every
entry a slot resolves to, and some that no slot does any more. Each keeps its storage range and the page generation of
its last check here, so that the check of an entry whose 2 MiB regions took no write since (`vk_mem_unchanged_locked`,
one shared lock for the whole list) touches neither the entry nor the lock: 5,600 entries took 0.77 ms a submission one
by one. Texture lock held. rel is the entry's released state (`mark_released()`, `entry_of()`'s take-back): the check
does nothing for a released entry, and with the state here it does not touch the entry to find that out. On a card with
room the sweep keeps up to 8,192 released entries, they stay listed, and since stays as it was at their release, so each
one whose regions took a write later missed the region check at every submission from then on: 1,000 to 6,300 entries a
check in Kakariko and Castle Town, a third to a half of the list.

- `typedef struct { Tex *e; uint64_t addr, size, since; uint8_t ok, rel; } PEnt`: ok: uploaded as of since.

The counts up to `n_presnap_q`: the region check's misses (`vk_mem_unchanged_why_locked`): not uploaded, a region
without this epoch's snapshot yet, one untracked, one written since, nothing to check; and the snapshot queries its own
pass made (`NX2SARIA_POOL_PRESNAP`).

### The entries the pool's slots hold

`PRef`: for each, how many slots' last fill resolved to it (`runtime/vk/vk_exec_draw.c` `pool_hold()`; a fill that
shared another slot's lookup counts for the entry that lookup found). The walk over every slot looked each slot up in
each submission, which left its entry's use stamp (touch): the idle clock reads it, and `start_at()`'s alias rule reads
the frame of an entry with GPU content. The check by list is no lookup, so it leaves the stamps from these counts
(`pool_touch()`): in each submission for the GPU-owned entries, in turn for the others, each once in `POOL_TOUCH_DIV`
submissions (`IDLE_ROUNDS` is 240 of them). An entry keeps its place until it is destroyed and no slot holds it any
more: a destroyed one is refilled out of its slots by the bump of its bucket.

- `typedef struct { Tex *e; uint32_t refs; int next; uint8_t gown; } PRef`: e NULL: destroyed, or free (refs 0, next).
- `static int npref, cpref, pref_free`: `pref_free`: 1 + the first free place.

### `pool_forget()`

From `destroy_tex()`.

### `pool_gpu_owned()`

`e` becomes GPU-owned: the list's check does nothing for it any more, and its use stamp is left in each submission.

### `vk_texture_pool_note_locked()`

- The caller's lookups left the slot resolved under its bucket's generation, unless it made none (a fill without a token
  or a kind): an entry cached under another generation may be destroyed, so the slot is looked up first.

### `pool_presnap()`

Released entries are left out: a slot resolves to none of those whose content the CPU provides (`slot_resolve()`'s stale
rule), and the game may be unmapping their storage.

`NX2SARIA_POOL_PRESNAP=1` (default), 0, ab: the check takes the write-watch snapshot of an entry's regions that have
none in this submission yet before it looks at them (one exclusive hold of the generation table's lock for the list), so
an unchanged entry in such a region passes there. Without it 97 % of the 5,600 entries missed the region check for want
of a snapshot and went the long way, one entry and one lock at a time. With it 97 % pass, 3.5 snapshot queries a
submission: the check 0.50 to 0.19 ms a submission, the replay thread -0.37 ms against the control.

### `pool_presnap_runs()`

`NX2SARIA_POOL_PRESNAP_RUNS=1` (default), 0, ab: those snapshots are taken for the whole list first, one write-watch
query per run of adjacent regions (`runtime/vk/vk_memory.c` `vk_mem_snap_noted_locked()`), instead of one per region as
the list comes to it. The log's "snapshot queries" of the pool freshness check count either.

### `vk_texture_pool_fresh_locked()`

`pool_by_list`: the submission being recorded checked the pool's entries by this list: for the idle clock it walked the
pool (`vk_texture_new_submit()`).

- `int by_range = ps`: each entry's regions are asked for as the list comes to it.
- `if (p->rel)`: released: nothing to check (the loop below would pass it by).
- `for (int k = nq; k-- > 0;)`: from the end: an entry leaving swaps the last one into its place.
- `uint64_t m0 = e->uploaded ? 0 : kern_now_ns()`: a first upload is part of making the entry.

### The stale rule's counts

`resolve_stale`: `slot_resolve()`'s last answer was none because of its stale rule (`vk_texture_slot_retry_locked()`).

`n_fin_own`, `n_fin_other`, `n_stale` and their `_hm` forms, with `NX2SARIA_TRACE=perf`, since the last perf line: slots
a finalize marked (its own texture object's), slots at the storage with the same parameters registered with another
texture object, which a finalize leaves current (`vk_texture_release()`), and lookups the stale rule answered with the
stand-in (`slot_resolve()`). `_hm`: of them, the terrain's streamed heightmaps (R32F 260x260), whose stand-in draws a
chunk of ground at height zero.

### `vk_texture_flat3d_locked()`

A flat texture sampled as a 3D one. Maxwell samples a texture by its own descriptor, whatever the shader's instruction
says: a 2D texture under a 3D sample gives its texel at (u, v), and the third coordinate does nothing. Vulkan has no 3D
view of a 2D image, so `get_view` (`runtime/vk/vk_exec_draw.c`) found no view for such a binding and the draw sampled a
stand-in. Such a texture now gets a 3D image of one slice with its texels, all of its levels, made in the upload batch
and copied again when the texture was uploaded or drawn into since (`n_up`, `wr_fence`) or another layer is asked for; a
3D view of that gives the 2D texel for any w. The copy is recorded in the upload batch, which runs before the part that
samples it, so a texture the GPU draws every frame is sampled as it was a part or a frame before. Small plain colour
textures only: `FLAT3D_MAX` texels, one sample, not block-compressed (BC rejects 3D on some drivers).
`NX2SARIA_TEX_FLAT3D=0`: the stand-in, as before. Texture lock held; NULL when the texture does not qualify or the image
cannot be made.

- `Tex *e = (Tex *)img`: pub is the first member.

### Origins

`NX2SARIA_TRACE=perf`; a run without it tests the flag and counts the drops, nothing more.

A texture drawn without its content leaves nothing in the log: no call fails and the image exists. What can still be
told afterwards is where the entry came from, so each entry keeps how it was made and how its content started (`og_*`,
had), and the last `DROPS_N` destroyed entries keep their descriptor and the reason they went. An entry made for a
descriptor that is still in that ring says which drop it follows (`origin_made()`), and the lines about one address list
what was dropped there (`others_at()`). The users are `marks_check()`, which names an entry a shader sampled while
nothing had ever put content into its image, and the lines of `start_at()`.

- `static uint64_t n_drop_by[DROP_N]`: entries destroyed since the last perf line, by reason.

### `flight_ev()`

An entry's event for the flight recorder (`runtime/diag/flight.h` `flight_tex()`, `FLIGHT_TF_*`): a no-op while the
recorder is off.

### `flight_bytes()`

For the flight recorder, after an upload of e: FNV-1a over 64 probes of 64 bytes spread over its storage, and how many
of them were all zero. Two uploads of one texture with the same hash read the same bytes at the probes; a first one with
zero probes read storage the game had not filled.

### `key_hash()`

FNV-1a; `make_key()` zeroes the key's padding.

### Drops for a low commit that come back

`tex_lock` held throughout. A texture dropped because the system's commit ran low frees its memory only for as long as
the game does not ask for it again; when it does, it is made and uploaded again and holds the same memory as before.
With other programs holding 49 of an 84 GB limit, the commit sat at the low line for whole sessions: 340 to 590 released
textures a second went, most were made again within seconds (178 re-uploads a second against 21 without the drops), and
the uploads inside the draws held the texture lock and the game's threads behind it, a 35-45 ms frame several times a
second, while the commit stayed where it was. So each drop under the commit rule notes its texture (by its key,
`CB_SLOTS` slots, the newest wins), and a texture made again within `CB_WITHIN` presents counts as come back. Over each
`CB_PERIOD`, when at least `CB_MIN_DROPS` went and half or more came back, the commit drops (released entries and idle
live ones) pause for `CB_HOLD_MIN` presents, doubled at each such period up to `CB_HOLD_MAX`; a period in which fewer
came back ends the doubling. Below half the low line (`commit_critical`) nothing pauses: there the commit running out
ends the game.

- `static uint64_t n_commit_back, n_commit_held`: since the last perf line.

### `commit_back_note()`

A new entry.

### `commit_drops_held()`

1 while the commit drops pause ("Drops for a low commit that come back"); judges the period that ended.

### The drop counts

- `static uint64_t n_drop_wb`: entries written back before their drop since the last perf line.

### `origin_made()`

A new entry: the frame, and the drop of its descriptor's last entry when the ring still holds it.

- `for (uint64_t i = 1; i <= n; i++)`: newest first.

### `desc_text()`

"fmt 0x25 (vk 37) 256x256x1, 9 levels, 1 layers, target 1, samples 0, flags 0x1, pool flags 0x22".

### `origin_text()`

"made at frame 100 again after a drop at frame 90 (under commit pressure); started empty over a released entry; ...".

### `no_content()`

Whether nothing ever put content into e's image: an entry the CPU writes that was not uploaded, or one it does not
write, at an address the CPU can see, that the GPU never wrote and that got no copy, no texels and no split fill.

### `said_at()`

Lines per address for one kind of event: up to `most` each, in a table of the first `SAID_N` addresses that had one (a
later address always gets its line), so a session's limit is by address, not a count of lines.

### `others_at()`

The other descriptors at e's address: every entry in the cache with its state, and what the ring knows was dropped
there, newest first. skip: an entry the caller describes itself (NULL: none).

### `log_new()`

`NX2SARIA_LOG_TEX_NEW`=`<frame>` (diagnostics): from that presented frame on, the first `LOG_NEW_MAX` textures made are
logged with what was at their address and, when the same key was destroyed before, how many frames ago and from where
(`destroy_tex`'s caller, an address the map file names: "destroy_tex at" gives the reference). For the steady creations
a still scene shows (about seven a frame in Kokiri Forest), each with its image, memory and upload.

`tex_lock` held.

### `lock()`

`hold_t0`, with `NX2SARIA_LOCK_TIME`: the outermost hold of the lock is timed from here, and one over 1 ms is reported
with the code that took it (`perf_lock_hold`); `lock()` is not inlined so that its return address names that code.

While the replay workers record (`vk_exec_shared`) up to nine threads take the lock for a few microseconds at each
draw's lookups. The OS lock spins only briefly before its waiter sleeps, and a sleeping waiter woke tens of microseconds
after the lock was free: in Hyrule Field the workers spent about a third of their recording asleep here
(`bind_resources`, `shader_for`). So a waiter then spins up to `TEX_SPINS` pauses for the lock to come free first.

### `destroy_tex()`

- `if (log_new_at >= 0)`: `NX2SARIA_LOG_TEX_NEW`: who destroyed which key, for the log of new textures.
- `if (e->pub.image) vk_exec_image_gone(e->pub.image)`: none when another texture took it (`create_tex()`, reuse).
- `if (e->flat3d)`: its 3D copy goes the same way.
- The record goes once the GPU is past the submission being recorded, as its image: the replay keeps &`e->pub` as a
  render target for the rest of a command buffer (`runtime/vk/vk_exec.c` `resolve_target()`) and DLSS for the frame, and
  this runs on whichever thread released the texture. Freed here, a pass that began again read the freed record, cached
  a view of the dead image (`vk_exec_target_view`), and a later image given the same handle drew through it into freed
  memory.

### The limit of released entries

Released entries with another descriptor at the same address are replaced; past `RELEASED_MAX` released entries one goes
at each release (it comes back on demand, a mirrored one re-uploaded from its storage). The game finalizes and
initializes transient textures every frame, so a released entry is mostly taken back within frames. Dropping all of them
at once when the count passed 4096 destroyed thousands of images in one frame: 333-422 ms frames every 40-50 s on a GTX
1050 Ti, then the textures still in use were created and uploaded again.

The one that goes is the oldest release that nothing sampled or drew into lately (`samp_frame`) or whose content is gone
(overwritten), not the oldest release as such. A released entry is not dead to the game (`drop_stale()` has the
reasons): a descriptor registered for it still resolves to it. The game reads its scene picture in every frame through a
second descriptor at the picture's address (RGB9E5, 1920x1080, the input of its own upscaler) whose entry was released
once, a few seconds into the area, and was looked up from then on: it headed the list for the rest of the session. One
frame at the limit, as it was:

1. The count of released entries passes `RELEASED_MAX` at a release, and the head of the list, that entry, is destroyed
   although the frame before sampled it;
2. Later in the frame the game's descriptor is looked up, finds no entry, and one is made again; its content is copied
   from the released scene target at its address (`start_at()`);
3. The frame comes out with the 3D scene black and the interface intact; the next one is whole again. Why the frame with
   the remade entry is black was not traced.

The count reaches the limit where released entries pile up: with the game's own upscaler in use (upscaler = fsr) its
render targets lie at new addresses in every frame, 2 to 13 new entries a frame, and the count went from 540 to 2,040
within 2,400 frames in Hyrule Field. Measured there (RTX 5090, 2026-11-11, Link running, 13,630 frames kept of each
run): 8 black frames in 5 of 7 runs. Where the remake was logged (3 runs, 4 black frames) each is the frame after it,
and where the drop was logged (2 runs) it is this sweep taking that entry as the count first reaches the limit; in one
of the two runs without a black frame that came after the frames kept. With the rule below a run of 18,730 frames
dropped 834 released entries past the limit from frame 11,600 on and showed no black frame. A session with DLSS as the
upscaler had the same black frame at Lon Lon Ranch while its log showed 2,032 to 2,047 released entries, kept there by a
low system commit; that it is the same drop is read from those counts, not logged.

"Lately" is by sampling, not by lookups. The first form of this rule spared every entry a lookup still reached (idle),
and the texture pool's refresh looks up every registered slot in each submission that draws through the pool: an entry
whose descriptor the game merely leaves registered counts as looked up for good. With the game's own upscaler in use its
render targets move to new addresses in every frame and each leaves such an entry behind, 3 to 7 a frame: over a town
all 2,049 released entries were "looked up", the sweep fell back to the oldest lookup, which among entries all stamped
in the same submission is no order at all, and took entries still drawn: 200 to 450 a window of 60 frames, with
buildings black or wrongly lit for single frames. `samp_frame` is stamped when a marked shader samples the entry's slot
(`vk_pool_marks_read()`) and by every lookup that is not the pool refresh (touch: a draw's fixed binding, a render
target, a copy), so a target the game draws into or reads is recent by it and one it only keeps registered is not.

With this rule a player's session of 30,000 frames through eight areas kept the list at its limit, dropped 50,182
entries past it, and showed none of those frames (2026-11-11, a hand test; no scripted run was made). When every
released entry was sampled within `SWEEP_UNSAMPLED` presents, the one sampled longest ago goes.

That rule holds from `RELEASED_HARD` entries on. Between `RELEASED_MAX` and `RELEASED_HARD` only an entry that loses
nothing goes: one that comes back intact or an overwritten one, released `STALE_FRAMES` presents ago or earlier (a
younger one is mostly taken back within frames). An entry whose content exists only in its image can be used again long
after anything sampled it. In Kokiri Forest the count stays at `RELEASED_MAX`, and two render targets at one address (a
40x24x64 volume and a 2240x1260 picture), released at the area's load and last sampled within seconds of it, went as
unsampled about two minutes later; the game used them again after that, they were made again empty, and one frame came
out with the scene black. On an 85 s route there (Link running, six runs a batch): a black frame in 10 of 24 runs with
the oldest release going; in 1 of 12 with the unsampled one going (the two targets went in 7 of those runs and were made
again empty in 4; the black frame is one in which the sweep took a row of render targets); in 0 of 18 with such entries
kept. Kept, the count reached 6,200 in the runs that pass the busiest part of the route early and 2,300 to 2,500 in the
others; frame times and the peak of the device memory in use were the same in all three forms, the device memory at the
end of a run 0.3 to 0.5 GiB higher with 6,200 kept. A kept entry bound into pool memory owns no memory and goes with its
pool (`vk_texture_range_gone()`); the memory of the others is bounded by `NX2SARIA_TEX_RELEASED_MB` (`drop_stale()`).

Each call looks at up to `SWEEP_LOOK` entries from where the last one stopped (`sweep_at`; `rel_unlink()` moves it off
an entry that leaves): with thousands of entries that stay, a walk from the head at every release would cross them all.
`RELEASED_MAX` is the floor of that limit; `released_max()` raises it by the card (`NX2SARIA_TEX_RELEASED_MAX` sets it).

### `released_max()`

The limit of released entries past which the sweep drops those that lose nothing: one per 2 MiB of the video memory
budget, from `RELEASED_MAX` to `RELEASED_MAX_TOP`. In Castle Town market on a 16 GB card (RTX 5060 Ti) with 2048 the
sweep dropped 412 entries in a session and a few textures blinked black for a moment; with 8192 (and
`NX2SARIA_TEX_RELEASED_MB` = 4096) it dropped none, 6,863 were kept and none blinked. Their memory stays bounded by
`NX2SARIA_TEX_RELEASED_MB` and the VRAM pressure. Read once the budget is known.

### `sweep_released()`

- `const uint64_t stale = PRESENTS(120)`: `STALE_FRAMES`.
- `if (!e || now - e->rel_frame < stale) { e = rel_head; break; }`: the rest is younger: from the oldest again.
- An entry the game took back before waits out `GAP_TIMES` the longest of those releases (`stale_after()`) nor one a
  draw or a shader used since (`used_lately()`): a finalized slot may still show it (`idle()`).

### The hard bound

Past `RELEASED_HARD` the count is a hard bound: entries go whatever they cost. The rule was the first entry in release
order that was overwritten or that nothing sampled for `SWEEP_UNSAMPLED` presents, of whatever kind, render targets
included, whose drop "The limit of released entries" above ties to a black frame; and each drop walked the list from its
head again. Most entries there cost little to make again: at the end of a session of 2026-11-13, when the count sat at
16,384 for a minute, 12,811 of 15,734 released entries owned no memory. So the entries now go cheapest first, by the
classes of `runtime/vk/vk_tex_release.h` `trl_hard_class()`: overwritten ones; ones that come back intact, released
longer ago than their `stale_after()` and not looked up lately; other ones that come back intact; ones whose content
only the GPU made; the last three only when nothing sampled them for `SWEEP_UNSAMPLED` presents; while every entry was
sampled lately, the one sampled longest ago, as before. Within a class in release order. One walk of the list takes a
batch down to `RELEASED_HARD` - `HARD_SLACK` entries, so the next `HARD_SLACK` releases need no walk.
`NX2SARIA_TEX_HARD_ORDER=0`: the rule before.

- `static uint64_t n_hard_by[TRL_HC_N]`: entries dropped past `RELEASED_HARD` since the last perf line, by class.

Each call of `sweep_hard()` is timed: it walks every released entry and drops a batch under the texture lock, on the
thread that releases (a guest thread's finalize, the replay's `overwrite_others()`), and nothing measured what a batch
costs. A call of `HARD_LOG_NS` or more gets a line (16 at most); the texture origins perf line sums them.

- `static uint64_t n_hard_calls, ns_hard, ns_hard_max`: since the last perf line.

### `sweep_hard()`

`tex_lock` held.

- `const uint64_t stale = PRESENTS(120)`: `STALE_FRAMES`.
- `if (nreleased > cap && hard_order())`: room for the whole list; without it the rule before.
- `for (Tex *e = rel_head; e && !gone; e = e->rel_next)`: in release order.
- `for (Tex *e = rel_head; e && n < cap; e = e->rel_next)`: in release order.
- The picks the GPU drew into go back to their storage first, in one upload batch with one wait (`write_back_n()`), as
  `drop_tex()` would write each of them back for `DROP_HARD` with a GPU wait of its own: up to `HARD_BATCH` waits on the
  thread that releases, the replay's `overwrite_others()` included. `write_back_n()` clears `gpu_dirty`, so `drop_tex()`
  then finds nothing to write back; the same entries go, in the same order.

### VRAM pressure

`NX2SARIA_TEX_VRAM_PRESSURE` (percent of the device-local budget, default `VRAM_PRESSURE_DEFAULT`; 0 = off): above it,
each submit drops up to `PRESSURE_STEP` of the oldest released entries, so a 4 GiB card does not keep `RELEASED_MAX`
released images (each ASTC one decoded to RGBA8) while the heap spills into system memory. A few per submit, for the
reason above. Only entries that come back intact are dropped: mirrored ones the GPU never wrote, re-uploaded from their
storage. A render target lives only in its image; dropping it brought the texture back empty (black flashes in a Depths
boss fight, ~170 "left uninitialised" aliases a minute).

The default lies under the seven eighths of the budget past which a pool gets no VRAM mirror (`runtime/vk/vk_memory.c`
`mirror_create()`), and the evictions aim 2% under it, so textures give way before mirrors are refused: on an 8 GiB card
that leaves the mirrors about 0.7 GiB, on a 24 GiB one 2 GiB. With the default at 90 the order was the other way round:
at 88% of a 22 GiB budget, full of textures of areas left long before, 0.4 MiB mirrors were refused and the draws that
read their pools over PCIe brought the game to 11-20 fps, while no texture had been asked to go.

- `#define EVICT_LOOK 1024`: released entries one pressure check looks at (`sweep_pressure()`).

### The idle clock

When a lookup last reached an entry: `entry_of()` (every texture get and slot resolution) and the per-submit check of a
slot lookup (`vk_texture_by_id_locked()`). The texture pool refresh looks every registered slot up once in each
submission that draws through the pool (`runtime/vk/vk_exec_draw.c` `pool_sync_entries()`), so an entry no lookup
reached for `IDLE_ROUNDS` such submissions and `STALE_FRAMES` presents has no live descriptor naming it: the game keeps
the texture object, nothing samples it. The clock is `look_round`, which only a submission whose slot checks reached
half of the registered pool slots moves on (`vk_texture_new_submit()`): counted in submissions, a pause screen or a fade
that draws nothing through the pool made the whole world's textures idle within seconds.

With `NX2SARIA_POOL_FAST` the refresh checks the entries by list instead (`vk_texture_pool_fresh_locked()`), which is no
lookup. Such a submission counts for the clock all the same, and the entries the pool's slots hold get their stamp from
the count of those slots (`pool_touch()`), each at least once in `POOL_TOUCH_DIV` submissions: an entry a slot names
never comes near `IDLE_ROUNDS`.

- `static uint32_t look_round = 1`: submissions whose lookups walked the texture pool.
- `static uint32_t round_checks`: slot checks of the submission being recorded (`vk_texture_by_id_locked()`).
- `static uint32_t pool_slots`: registered slots the pool covers (id < `VK_POOL_TEX`).
- `static int pool_ctx`: the texture pool refresh is looking slots up (`vk_texture_pool_ctx()`).

### `touch()`

- `if (!pool_ctx) e->samp_frame = e->use_frame`: a draw binds it, or the game or the replay reads it otherwise.

### The released entries' size bound and the commit

`NX2SARIA_TEX_RELEASED_MB` (default a thirty-second of the video memory budget, 384 MiB to 1 GiB,
`released_cap_bytes()`; 0 = no limit) bounds the own memory of the released entries. With the hold on (`drop_aged()`,
the default) they hold what the last minute released, up to the bound; `sweep_pressure()` lets young entries past it
while the card and the system have room (`runtime/vk/vk_tex_release.h` `trl_keep()`). Before the hold the bound was all
that limited them: `RELEASED_MAX` alone kept 5-7 GiB of released render targets and decoded textures after an hour of
play at a 2160p game resolution, device memory the driver also charges to the system's commit (the process's private
bytes reached 31 GiB). A sixteenth (1 to 4 GiB) still let them hold 1.3 GiB on a 32 GB PC whose commit then ran out at
an area load (device lost); only entries released `STALE_FRAMES` presents ago go, so the transient ones the game takes
back within frames stay. `NX2SARIA_TEX_COMMIT_FREE_MB` (default the larger of 4 GiB and a tenth of the commit limit; 0 =
off) lowers that bound to `COMMIT_LOW_KEEP` while less of the system's commit is free: an allocation that fails there
ends the game. On Linux nothing fails at the limit: the OOM killer ends the game when its cgroup or the machine runs out
of memory, and `plat_commit_info` reports the tighter of the two. Both drop only stale entries (`drop_stale()`), a few
per check like the VRAM pressure.

`NX2SARIA_TEX_PROC_COMMIT_MB` (default the installed memory, at least `PROC_COMMIT_MIN_MB`; 0 = off) is a bound on this
process's own commit that is logged when passed; it counted the commit as low too until it was found that no texture
dropped could bring the process back under it (`commit_is_low()`, at its end). The system's figure depends on the page
file and on what else runs: with a 64 GB limit it turned low at 59 GB in use, when this process alone held 42 GB on a PC
with 32 GB of memory, about half of it device memory, with the textures of five areas left behind. A process is not
meant to need more commit than the PC has memory: beyond that it lives on the page file, whatever its size. The bound
holds until the process is a thirty-second under it, so that it does not flip with every texture freed. The floor keeps
it out of the way on PCs with less memory, where a bound at the installed memory could hold all through play: the
system's figure decides there, as before. On Linux the process's figure is its resident anonymous memory, which cannot
pass the installed memory, so only a bound set below that holds there; a cgroup's limit and the machine's RAM act
through the system's figure.

Where a discrete GPU's memory counts against the commit (`plat_commit_counts_vram`: Windows), the bound compares the
commit less the device-local memory the driver reports in use (`vk_mem_vram`): that memory is on the card, not in RAM or
the page file, and the VRAM pressure and `tex_disuse` already bound it. Counted in, it held the bound all through Hyrule
Field on a 16 GB PC with a 16 GB card (RTX 5060 Ti): the process at 28.9 to 30.7 GB, 9 to 10 GB of it video memory,
while the system had 8 to 10 GB of its 49 GB limit free, and the ground flickered white as textures gave way. So on such
a PC the 42 GB process above, half of it device memory, no longer reaches this bound: the system's figure and the VRAM
pressure are what act there. The part of a card's memory the driver has moved into shared system memory is subtracted
too, which it should not be; the driver does not report it apart.

- `static uint64_t released_cap`: bytes; 0 = not read yet, `UINT64_MAX` = no limit.
- `static long long commit_free_mb = -2`: -2: not read yet, -1: the default.
- `static long long proc_commit_mb`: this process's bound in MB; 0 = none.
- `static int commit_low, commit_logs`: `commit_low`: either of the two below at the last check.
- `static int sys_low, proc_high`: the system's commit is low; this process's is above its bound.

`commit_free_now`: the system's commit at the last check, for the room young released entries past the bound may take
(`trl_room`): MB free and the free space under which it counts as low; -1 free: not tracked.

### `released_cap_bytes()`

- `const char *by = ""`: which term set the bound, for the line below.
- By the card: a thirty-second of its budget, 384 MiB to 1 GiB (`runtime/vk/vk_tex_release.h` `trl_cap_mb()`, which has
  the run that set it). What the released textures hold at their fullest stays in use for the rest of the session, as
  free space inside the allocator's blocks once they are dropped, so the bound is the price of the cache; until 0.11.0
  it was a quarter of the budget, up to 8 GiB. The two pressure rules still come first: released entries go before
  anything else above `NX2SARIA_TEX_VRAM_PRESSURE` of the budget, and the bound falls to `COMMIT_LOW_KEEP` while the
  commit is low.

### `commit_is_low()`

- `if (!plat_commit_enforced()) commit_free_mb = proc_commit_mb = 0`: no limit known.
- `char note[2048]`: the platform layer's own lines go to stderr only, so the log file gets this through here.
- `commit_critical = limit - used < want / 2`: the pause for drops that come back ends here.
- `double dev = 0`: device-local memory in use, MB: in the commit (`plat_commit_counts_vram`), not in RAM.
- `if (low != sys_low && commit_logs < 40)`: a few lines, however often it flips.
- A page file Windows sizes grows only slowly and by little, so the limit stays near the installed memory plus what the
  page file held at the boot: on a 16 GB PC with a 16 GB card it ran low in Hyrule Field at 32 GB, and textures gave way
  (white or black ground). A fixed page file of 32 GB or more raised the limit to 49 GB.
- Only the system's figure makes textures give way. The process's bound compares its commit less the video memory in
  use, and a texture dropped lowers both by the same amount: past the bound nothing a drop does could bring it back
  under, and textures went one after the other for as long as the rest of the process stayed large, the ground
  flickering white with the system's commit to spare (#434). What is above it is the host's own memory (the game's heap,
  pipelines, caches); the bound is logged, and the system's figure still acts when the commit runs low.

### Stale released entries

Drops up to `PRESSURE_STEP` released entries that own memory and were released `STALE_FRAMES` presents ago or earlier,
oldest first and the intact ones before the others, while the released entries own more than keep. A transient texture
is taken back within frames, so dropping it frees nothing: the game initializes it again and it is created again, empty
if it is a render target. Under commit pressure that a system-managed page file never relieves, dropping young ones
destroyed 2160p render targets about every frame for whole sessions. Frame generation also reads a frame's depth target
at the host present (`runtime/vk/vk_dlssg.c` `vk_dlssg_frame()` tags it), after the blit deferred destruction waits for.

A released entry is not dead to the game. A descriptor registered for a render target still resolves to its released
entry (`slot_resolve()`: a finalized slot with no live texture at its storage gets its own), and the game can sample it
or take it back long after the release. In Kakariko Village, with the system's commit low, render targets dropped at
`STALE_FRAMES` came back empty and the scene was drawn black for one frame every two to three seconds (interface
intact), for as long as the commit stayed low. So no entry goes while lookups still reach it (idle), and one whose
content exists only in its image waits `GPU_STALE_FRAMES`, or `GAP_TIMES` the longest it stayed released before the game
took it back when that is longer. An overwritten entry has no content left to wait for (`overwrite_others()`): it goes
with the intact ones, whether or not a descriptor still names it.

### `idle()`

Nor is an entry idle that a shader sampled within `STALE_FRAMES` presents (`samp_frame`, `vk_pool_marks_read()`): a
finalized slot keeps showing its released entry (`stale_kept()`), and the pool's check passes released entries by, so no
lookup reaches such an entry while every frame samples it. Counted idle, it was dropped past the released entries' size
cap, and the slot showed the stand-in until the game registered it again: in a player's recording of Castle Town,
finalized slots lost their picture in the very frames such entries were dropped.

### `keep_taken()`

`NX2SARIA_TEX_KEEP_TAKEN` (default 1; 0: as before): an entry that comes back intact waits out `GAP_TIMES` the longest
it stayed released before the game took it back, as one whose content lives only in its image does, and not
`STALE_FRAMES` alone. Coming back intact is not free: the descriptor finds no entry, the frame samples its stand-in
while the entry is made and uploaded again, and the next frame has it. Kakariko's hazy air is drawn with ASTC textures
the game releases and takes back every few hundred frames; with the released entries at `RELEASED_MAX` the sweep dropped
about 30 of them as soon as their 120 presents were up, and the frame that needed them again came out without the haze,
darker and sharper, once every few seconds.

### `drop_stale()`

Returns the entries dropped: `PRESSURE_STEP`, more may follow.

- `for (Tex *e = rel_head, *next; e && now - e->rel_frame >= stale; e = next)`: in release order.

### `used_lately()`

A draw, a read or the pool's marks used e within `within` presents of now (touch, `vk_pool_marks_read()`).

### `sampled_lately()`

A draw bound e, the game or the replay read it, or a marked shader sampled a slot that shows it, within `within`
presents of now (`samp_frame`). `used_lately()` also counts the pool's own stamp, which every GPU-owned entry a slot
still holds gets in each submission whether or not anything samples the slot (`pool_touch()`): for such an entry "used"
says that the game has not finalized the slot's texture object yet. Asked only while the marks tell what is sampled
(`marks_tell()`); without them a slot's sampling is not known and `used_lately()` stands.

### `drop_overwritten()`

Drops up to `PRESSURE_STEP` entries that were marked overwritten `OVERWRITTEN_KEEP` presents ago or earlier
(`overwrite_others()`): the image reuse found no texture to give their images to in that time.

Not one that was used since (`NX2SARIA_TEX_OVERWRITTEN_USED=0`: those too, as before): the mark says another target was
drawn over the entry's storage, not that nothing reads the entry. In a player's recording the entry of a second
descriptor at the scene target's address (format 0x40 beside the target's 0x3f, both 2400x1350) was dropped here and
made again in the same frame, its content copied from a released entry at that address, whose bits are the scene in the
other format. That frame's picture was nearly black with its colours exchanged (brightness 17 against 102 in the frames
around it); the frames before and after were as usual. Whatever looks such an entry up after its drop makes it again
that way, so an entry something still uses stays; so does one the image reuse would take (`create_tex()`,
`REUSE_UNUSED`). full: the card is past the VRAM pressure's share of its budget by the driver's own figure
(`sweep_pressure()`). The wait is then `OVERWRITTEN_FULL` presents, four times what the image reuse leaves an entry
alone for there: an image no texture took in that time holds nothing the game can ask for, and the memory it keeps for
ten seconds more is what the next image fails for. Not sooner: at ten presents this took the images the reuse was about
to hand on, 7,301 in a run at 3840x2160 that then made 7,672 images and reused 5,226.

- `n_over_full`: since the last perf line: of `n_over_drops`, dropped with the card that full.
- `used`: drawn with or sampled (`sampled_lately()`); a slot that merely still holds the entry does not keep it.
- `if (e->kept_frame != now + 1 && (!n_kept++ || !(n_kept & (n_kept - 1))))`: the first, then at powers of two.
- `e->ow_frame = now`: looked at again `OVERWRITTEN_KEEP` presents from now.

### Released textures by age

`runtime/vk/vk_tex_release.h` has the take-back figures behind the hold.

`NX2SARIA_TEX_RELEASED_HOLD` (seconds, default `TRL_HOLD_S`, at most `TRL_HOLD_MAX_S`; 0 = no hold): a released entry
that owns memory goes once it has gone unused that long, twice that for content only the GPU made, whatever the size
bound says. Unused counts from the release or the last sample of the released image, whichever is later, in seconds of
the wall clock (`secs_since()`). The rules `drop_stale()` keeps hold here too: not before `stale_after()` (an entry the
game once took back after a long gap waits `GAP_TIMES` that gap) and not while it is not idle (a lookup other than the
pool's reached it, or a shader sampled it, lately). Nor an entry the GPU drew into since its last write-back: its drop
would first copy it back to the game's memory and wait for the GPU (`drop_tex()`, `write_back_n()`), a stall the sweep
is not to make in play; such an entry stays to the size bound, as before the hold. Each check looks at up to `AGED_LOOK`
entries from where the last one stopped (`aged_at`), in release order, and starts from the oldest again at the first one
released less than the hold ago: the rest are younger.

- `static int rel_hold = -1`: seconds; -1: not read yet.
- `static uint64_t n_age_drops, n_age_bytes`: since the last `vk_texture_reuse_log()` line.

### `drop_aged()`

`tex_lock` held.

- `if (!e || secs_since(e->rel_frame) < hold)`: the rest is younger: from the oldest next time.

### Eviction of live entries

`NX2SARIA_TEX_EVICT` (default on; 0 = only released entries go under VRAM pressure, as before): above the VRAM pressure
threshold, once the released entries that come back intact are gone, live entries go too, least recently looked up
first: those that come back intact and that no lookup reached for `IDLE_ROUNDS` pool walks and `STALE_FRAMES` presents
(touch: no live descriptor names them). A later lookup creates the image again and uploads it from its storage, as for a
texture seen the first time. Up to `EVICT_STEP` entries per check, until the device-local use is 2% of the budget under
the threshold. The use counts the free space inside VMA's blocks as free (`vk_mem_vram_live`): a freed texture leaves a
hole the driver still counts, and measured that way the evictions went on until nearly every idle texture was gone. The
next check waits until the GPU is past the submissions that may still name the images, as their memory goes back to VMA
only then (`vk_defer_image`). A VRAM mirror that did not fit asks for room the same way, below the threshold too
(`vk_texture_want_room()`): a pool without its mirror costs far more than a texture uploaded again. Its request stands
as the use the mirror fits at (`room_level`) for `ROOM_CHECKS` checks or until the use is down there
(`sweep_pressure()`): taken as bytes to free at the one check that read it, a request for 205 MiB got the `EVICT_STEP`
textures of that check, 26 MiB, or nothing when the check fell before `evict_wait`, and was gone. A walk of the whole
table that finds no candidate is not made again for `EVICT_EMPTY_WAIT` submissions (two pressure checks;
`idle_empty_wait`, which only `evict_idle()` reads, so that `evict_disused()` still runs): under a standing pressure or
mirror request with nothing idle, every eighth submission walked every texture again. An entry turns idle only after
`IDLE_ROUNDS` pool walks, so the wait delays an eviction by a few submissions at most.

- `static uint64_t evict_wait`: `vk_sync_fence_done` value the next eviction waits for.
- `static uint64_t idle_empty_wait`: `vk_sync_issued` value the next walk of `evict_idle()` waits for.
- `static uint64_t n_evict, n_evict_bytes`: since the last perf line.
- `static uint64_t n_evict_empty, ns_evict_empty`: empty walks of `evict_idle()` and their time, as long.
- `static volatile int64_t room_wanted`: bytes refused VRAM mirrors asked for (`vk_texture_want_room()`).
- `static uint64_t room_level`: `tex_lock`: the device-local use at which those mirrors fit.
- `static uint32_t room_checks`: `tex_lock`: checks the request still stands for; 0 = none stands.

### `vk_texture_want_room()`

- `int64_t cur = __atomic_load_n(&room_wanted, __ATOMIC_RELAXED)`: the largest request since the last check.

### `evict_idle()`

`tex_lock` held; returns the bytes freed.

- `for (uint32_t i = 0; i < by_key.cap; i++)`: the `EVICT_STEP` oldest candidates, oldest first.
- `if (!nv)`: the walk of the whole table found nothing: none again for `EVICT_EMPTY_WAIT` submissions.

### Disuse

`NX2SARIA_TEX_DISUSE`, default on; `runtime/vk/vk_pipeline.c` "texture pool marks", `runtime/vk/vk_exec_draw.c` "pool
marks".

The game keeps the textures of the areas it has loaded registered in the pool, and the pool refresh looks every
registered slot up, so `idle()` never holds for them, and their memory, which the driver charges to the system's commit,
only grew. Marked shaders tell which slots they sample: `vk_pool_marks_read()` sets `samp_frame` of the texture of each
marked slot. Where `evict_idle()` frees too little, `evict_disused()` takes live textures that come back intact and that
nothing sampled for `DISUSE_FRAMES` presents, least recently sampled first, in two steps:

- Retired (retire): the entry stays whole, but the pool refresh no longer resolves a slot to it (`slot_resolve()`), so
  every slot that names it gets the kind's stand-in. Any use in the meantime takes it back as it is (`retire_cancel()`):
  a lookup other than the pool refresh, or a mark on one of its slots (the stand-in was sampled).
- Destroyed (`retire_sweep()`) once the texture pool has taken every set of its ring since, each brought up to date when
  taken (`runtime/vk/vk_exec_draw.c` `pool_take_set()`, `pool_catch_up`), after a sync that left no slot behind wrote
  its stand-in (`vk_pool_takes` counts only such takes), and the GPU has finished the submissions recorded until then:
  no set can be bound with its view any more, and no submission that bound one is still running. Its key is held then: a
  held key makes no entry in the pool refresh (`held_out()`), so the slot keeps its stand-in; a mark on the slot drops
  the hold and the next refresh makes the texture again, as does any other lookup.

An image freed at the eviction itself, as before, faulted the GPU once (a read of freed memory in a pass, 0.6.1.14)
without a path to it found in the code; the delay leaves no window in the pool sets, and the cancels count uses the
marks missed. `NX2SARIA_TEX_DISUSE=2` retires and never destroys (stand-ins only, nothing freed): it tells the marks and
stand-ins from the freeing as the cause of a fault. Only while every module that indexes the pool was marked and two
reads arrived: an unmarked shader could sample what the marks call unused.

Two minutes, not the thirty seconds it was: a texture out of view that long and then seen again shows its stand-in for
the frames until its mark is read, which is a flash of missing ground or a dark object each time the camera turns back,
and a batch of such textures gave back 0.4 to 31 MiB in a recorded session. The textures of an area left behind, which
are what the memory grows by, still go two minutes after it.

- `static nvn_hmap holds = {.klen = sizeof(TexKey)}`: value: 1.

### `marks_tell()`

Whether the pool's marks tell which textures the shaders sample: every module that indexes the pool is marked, no read
was missed and two have arrived.

### The marks' state and the disuse counts

Whether the scene is being drawn, from the marks: `marks_peak` is the most slots one read marked lately (in 1/256,
halving in about a minute of reads), `marks_live` the reads in a row that marked at least a quarter of it. A pause
screen, a menu or a fade draws next to nothing through the pool for longer than `DISUSE_SECONDS`: every texture of the
scene behind it then counted as unsampled, went under memory pressure, and the scene came back as stand-ins.
`evict_disused()` waits for two such reads in a row: the first one after a pause is what notes the scene's textures as
sampled again.

- `static int resolve_held`: `slot_resolve()` left the slot out for a held key or a retired entry.
- `static uint64_t n_disuse, n_disuse_bytes, n_disuse_back`: since the last perf line.

`n_disuse_diverted`: since the last perf line: eviction candidates kept because another descriptor is registered at
their address (`shared_address()`), and holds dropped by a mark that another entry at the address had taken
(`vk_pool_marks_read()`).

- `static uint64_t n_retire_cancel, n_retire_freed`: since the last perf line.
- `static uint64_t n_retire_stalled`: retired textures taken back because they never came due.
- `static Tex **retiring`: retired entries not destroyed yet.

### `retire_cancel()`

Takes a retired entry back as it is: something uses it. Its slots resolve to it again (the bump).

- `e->retiring = 0`: it leaves `retiring[]` at the next `retire_sweep()`.
- In use as of now: while retired the pool refresh left it out, so it carries no use stamp of that time and counted as
  idle the moment it came back. A sweep over the budget (`sweep_pressure()` calls `retire_sweep()` and then
  `evict_idle()`) could evict it in the same call that took it back.

### `retire_remove()`

From `destroy_tex()`: the entry goes another way (released and dropped, shutdown).

### `retire_sweep()`

Each submission: destroys the retired entries the pool sets and the GPU are done with ("Disuse" above). `TEX_DISUSE=2`
keeps them.

- `if (!e->retiring) continue`: taken back.
- The takes that count are those of syncs that left no slot behind, and a PC that has no creation time over in most
  frames leaves some behind in nearly every sync: there a retired texture was never destroyed and its slots showed the
  stand-in for as long as that lasted, minutes in one report. One that is still not due after `RETIRE_PATIENCE` presents
  is taken back and counts as sampled now.

### `held_out()`

A lookup of key k that found no entry: 1 when the pool refresh asks for a held key (it gets the stand-in). Any other
lookup ends the hold: the game uses the texture.

### `slot_names_other()`

`tex_lock` held. Whether a registered slot at e's address names another descriptor than e's: the same bytes through
another swizzle or format, or a view the game draws through. Such a texture is not retired by disuse. Its slots do not
all come back by the marks: a slot of the other descriptor that followed e gets an image of its own once e is gone, an
empty one when the descriptor is not a CPU-written one, and from then on that image takes the marks of every slot at the
address, so the held key of e was never dropped and the slots that named e showed the stand-in for good: the ground of
Hyrule Field and of Lon Lon Ranch drawn flat and see-through after 6,663 textures went under low commit and 928 came
back.

### `disuse_evict_on()`

`NX2SARIA_TEX_DISUSE_EVICT` (default 0; 1 = textures no shader sampled for a while are retired under pressure, as
before). Off since players' ground went flat and see-through after such evictions and stayed so (Hyrule Field, Lon Lon
Ranch; 1,441 retired on arriving in a village, 82 back): with the eviction off the same play kept its ground, and
neither of the two holes closed in the way back (`shared_address()`, the hold drop in `vk_pool_marks_read()`) acted in
the session that still lost it. What takes the retired texture's place for good is not known yet; a draw that samples
the stand-in once into a picture the game keeps would do it, and no mark can undo that. The shaders keep their marks
(`NX2SARIA_TEX_DISUSE`), so turning this on changes no pipeline. Released textures and the idle ones the game no longer
names still give way under pressure.

### `evict_disused()`

`tex_lock` held; returns the bytes retired (freed a few submissions later), counting those still retiring as freed.

- `if (retiring_bytes >= want) return retiring_bytes`: what is on its way out covers it.
- `for (uint32_t i = 0; i < by_key.cap; i++)`: the `EVICT_STEP` least recently sampled candidates, oldest first.
- A candidate whose address another descriptor shares stays (`shared_address()`), and counts as sampled now, so the walk
  over its bucket is made once per disuse time and the next candidates get their turn.
- Keys of textures the game has let go of pile up: keep those a slot still holds out (cheld). Dropping them all would
  make the pool refresh create every evicted texture again while memory is short.
- `for (; n < nv && freed < want; n++)`: retire: the pool refresh gives their slots the stand-in from now on.
- `pool_unlist(e)`: no slot resolves to it in the pool refresh; taken back, the refill of its slots lists it again.

### `slot_note()`

The registered slots at e's address, for `marks_check()`'s line: how many, those of e's own descriptor ("f": finalized),
those whose last lookup gave e (a slot of another descriptor among them follows e), and the other descriptors' with
format and flags; up to 8 ids of each.

### `marks_check()`

`NX2SARIA_TRACE=perf`, from `vk_pool_marks_read()`: a shader sampled slot id, and nothing has ever put content into e,
the entry the slot shows or one credited with the mark (`no_content()`): the picture drawn through it is an empty or
undefined image, which nothing else reports. One line per entry and up to `MARK_LINES` per address (`said_at()`); the
marked slots that met such an entry are counted in the texture origins line whether or not a line was written.

- `static uint64_t n_mark_empty, n_mark_none`: since the last perf line: marked slots that met such an entry; that show
  none.

- An entry the CPU writes is uploaded by its first check, which one just made (again) has not had yet.

### `vk_pool_marks_read()`

- A shader whose ring of noted slots overflowed (`VK_POOL_MARK_OVERFLOW`) sampled slots this read does not name: every
  registered slot counts as sampled then, so that no texture in use is taken for unused.
- `if (!tslots[id].used)`: sampled, and not a registered slot now: what it showed is not known here.
- `if (s->ce && s->cgen == agen[sinfo[id] & 0xffff])`: what the slot resolves to now.
- `if (s->ce->retiring) retire_cancel(s->ce)`: not expected: retiring bumps the slot's generation.
- `if (trace && !s->ce && s->cgen == agen[sinfo[id] & 0xffff]) n_mark_none++`: its last lookup gave no entry.
- `if (e->retiring) retire_cancel(e)`: retired, and its stand-in was sampled: back as it is.
- Its own entry is released: the slot shows it where no live entry at the address is followed (`slot_resolve()`), so it
  counts as sampled too. The image reuse and `drop_overwritten()` go by this stamp (`sampled_lately()`).
- The slot has no entry of its own: it samples another descriptor's image at its address (`slot_resolve()` follows one
  of the same target, as a second descriptor with another swizzle does). That image is what was sampled. With the mark
  given to the slot's own key alone, a texture sampled only through such slots was never noted as sampled while the
  slots' cached resolution was out of date, which it is at most reads in a bucket the game keeps changing: it was
  retired as unused after two minutes, and a mark on its stand-in could not take it back. While the pool's syncs kept
  leaving slots behind, a retired texture was never destroyed either (`retire_sweep()` waits for complete takes), so its
  slots kept the stand-in: the ground of the field drawn without its textures and its height.
- `if (flight_on && !followed)`: nothing of its own and nothing followed: its stand-in was on screen.
- The slot's own key may be held although an entry at its address took the mark: the slot shows the stand-in then
  (`slot_resolve()` follows fewer entries than the loop above credits), and only this drop makes its texture again. Left
  to the case where the loop found nothing, such a slot kept the stand-in for good.
- `if (holds.n && nvn_hm_del(&holds, &k))`: the stand-in was sampled: the next pool refresh makes it.
- `s->cwant = 1`: on screen as a stand-in: made at the next refresh, whatever the frame's creation time.

### `commit_evict()`

`NX2SARIA_TEX_COMMIT_EVICT` (default on; 0 = off): while the commit is low (`commit_is_low()`), idle live textures go as
under VRAM pressure (`evict_idle()`), `COMMIT_EVICT_STEP` at a time, once the released entries gave what they could:
they are down to `COMMIT_LOW_KEEP`, or `drop_stale()` found less than a full step to drop. The driver charges device
memory to the commit too, and the eviction above starts only at `NX2SARIA_TEX_VRAM_PRESSURE` of the device's budget: on
a 16-32 GB card the textures of every area visited stayed until the commit ran out (27-37 GB processes, then a failed
allocation or a device lost, #177 #203 #215), as nothing else gave them back while they stayed below that share of the
video memory. Waiting for the released entries to be down to `COMMIT_LOW_KEEP`, as before, never ended where the game
releases and takes back 200-300 textures a frame: 2,048 released entries held 1.4-2.0 GiB, none of them stale, and with
the commit low for minutes no live texture went (a 42 GB process). The entries the game takes back stay either way:
`drop_stale()` leaves them, and the evictions below take live textures only.

`tex_lock` held, the commit is low and the released entries gave what they could.

### `rel_census()`

The released entries hold more than their bound (`released_cap_bytes()`) after `drop_stale()` took what it may: one line
that says what keeps the rest, at most every `REL_CENSUS_SUBS` submissions and `REL_CENSUS_LINES` times a run. The bound
is no bound for an entry a lookup still reaches, and the pool stamps every GPU-owned entry that a slot's last fill
resolved to in each submission (`pool_touch()`), a finalized slot's released one too: at 5120x2160 a session's log had
989 and then 2,172 released entries holding 9.2 and 10.1 GiB against a bound of 4,008 MiB, and its overwritten render
targets "last used 1 presents ago". Each entry counts once, under the first reason that holds. The same line, at most
every `REL_CENSUS_ROOM_NS` of wall time and `REL_CENSUS_ROOM_LINES` times a run, while they hold more than the bound
itself but not a quarter more than what they may keep (room 1), and only while `trl_keep` lets them keep more than the
bound: with room on the card it lets them keep twice the bound, and a session that held 7,150 MiB against a bound of
4,008 MiB (2026-11-13) never logged what kept them. Without that room what they may keep is the bound, and the line said
"kept past it while the card has room" of entries `drop_stale()` was taking.

`tex_lock` held.

- The young ones by what they wait for: fewer than `STALE_FRAMES` since the release; content only the GPU made
  (`GPU_STALE_FRAMES` or the gap); intact and waiting out the gap (`stale_after()`) with nothing reaching them, which
  the VRAM pressure takes; intact, waiting it out and still reached.

### `sweep_pressure()`

`tex_lock` held.

- `if (submit_epoch & 7) return`: the budget and commit queries every 8 submits.
- `if (wanted)`: the use at which the refused mirror fits; the lowest of the requests that stand.
- `if (room_checks && used <= room_level) room_checks = 0`: made: the pool gets its mirror when it asks again.
- Released entries that come back intact, by the rule `drop_stale()` keeps: released `STALE_FRAMES` ago and reached by
  no lookup since (idle). The game releases and takes back hundreds of textures a frame in towns, and the release list
  is mostly those: dropped, each was made and uploaded again within frames and freed nothing. With the budget taken as
  7.5 GiB (`NX2SARIA_VRAM_BUDGET_MB`), teleports from Hyrule Field through Kakariko Village, the Graveyard, Death
  Mountain Trail and Kokiri Forest dropped 82,288 under the pressure and the card stayed over its budget throughout;
  with this rule 34,699 went, a fifth fewer textures were made again, the render thread waited a third less on textures,
  and 4 draws met a texture gone where 11 had. Each check looks at up to `EVICT_LOOK` entries from where the last one
  stopped (`press_at`; `rel_unlink()` moves it off an entry that leaves), and starts from the oldest again at the first
  entry released less than `STALE_FRAMES` ago or at the list's end: a walk from the head passed every entry it could not
  drop, up to `RELEASED_HARD` of them, at every check. At `PRESSURE_HARD_PCT` of the budget, while the shaders' marks
  are read (a shader that cannot be marked stops it), an entry that comes back intact goes once no shader sampled it for
  `HARD_UNSAMPLED` presents, however recently it was released and whether lookups still reach it or not,
  `PRESSURE_HARD_STEP` at a time until the overrun is covered. At an area change the game releases the textures of the
  area left behind, several GiB, while the next area's are made, and the texture pool keeps looking up the slots it
  holds for them: none was idle, `PRESSURE_STEP` went a check once `STALE_FRAMES` had passed, and the card stayed 2-3
  GiB over its budget for seconds after each change while the driver moved memory out to the system's (an RTX 4090, a
  2160p game resolution and ASTC decoded to RGBA8: released entries 7.6 GiB beside 11.8 of live ones, 5 to 15 fps). The
  load itself draws nothing, so waiting for the marks to show the scene again (`marks_live`) waited past it. A lookup is
  not a draw: such an entry that is drawn again is made again from its storage, as below the budget. The walk goes on
  from `press_at` as below; past the young entries too, since they are what it is for.
- `if (!e || (!hard && now - e->rel_frame < STALE_FRAMES))`: the rest is younger: from the oldest next time.
- `int low = commit_is_low(), more = 0`: more: the released entries have stale ones left for the next check.
- `int held = low && commit_drops_held()`: the commit drops came back (`commit_back_note()`).
- The driver's figure, holes in VMA's blocks counted: the pressure above goes by the memory in use without them
  (`vk_mem_vram_live`), and a new block or an image with memory of its own cannot use a hole. A card at 100 % of its
  budget by this figure was at 81 % by the other, 3.1 GiB of it holes, and nothing gave way.
- The bound, or more of it while the hold empties the released entries within minutes and the card and the system have
  room (`runtime/vk/vk_tex_release.h` `trl_keep()`): a young entry dropped there is one the game may still take back.
- `if (keep != UINT64_MAX && rel_bytes > keep + keep / 4) rel_census(keep, 0)`: well over it: what holds the rest.
- `else if (keep > cap && rel_bytes > cap) rel_census(cap, 1)`: kept past the bound while the card has room.

### Per-frame texture work

`runtime/vk/vk.h` `VK_TS_*`.

Sums for the frame being made, by the render and replay threads (atomics: both may add at once). `vk_texstat_frame()`,
called at each present (`runtime/diag/perf.c` `ft_frame()`), takes them; a frame whose texture work (creation, upload
and upload flush) reaches `NX2SARIA_TEXUP_LOG_MS` gets a "texup:" line, so an area change's hitch shows where its time
went (default 0: no lines; 16 is a useful value).

### Uploads

### `texel_alpha_one()`

- `default: *elem = 4; return 1`: 0x23, 0x24.

### `expand_rgb()`

3-component texels to 4 (n texels).

### `astc_check()`

`NX2SARIA_ASTC=check`: the compute decoder's texels of n regions (gpu: the regions one after the other) against
`astc_decode_ldr` of the same raw blocks (raw: the staging bytes the regions point into).

### The parts of an upload and the content hash

`UPLOAD_PART`: the staging bytes of one part of a texture upload decoded or copied on the CPU (`upload()`). With
`NX2SARIA_ASTC=cpu` a large ASTC array decodes to RGBA8 in one go: a staging buffer of 676682336 bytes failed (-2) on an
Intel UHD with 8 GB of shared memory and the game closed before the title screen.

- `#define UPLOAD_PART (32ull << 20)`: fits the staging pool (`vk_staging` takes up to half of its 64 MiB block).

A texture of at most `CHASH_MAX` bytes keeps a hash of its storage as it was uploaded. A write to its pages then
re-uploads it only when its own bytes changed: the game's frames rewrite hundreds of small textures' pages (shared with
data the CPU writes, or written with the same bytes), about 420 re-uploads a frame of 1.5 KiB each, and one costs a
staging buffer, a copy and a barrier, several microseconds, where hashing 1.5 KiB takes a fraction of one. A CPU write
during the hash sets the pages' write bits again, so the next check sees it.

`NX2SARIA_TEX_SAME_BIG` (default 1 since 0.11.0; ab): textures up to `CHASH_BIG_MAX` keep the hash too. While the player
runs through a town the game moves about 32 textures to new storage every four or five frames, back to back in one
block, and a frame after their first upload every page of the block shows a write again. Of the ones the hash covers, 0
to 4 in a hundred are uploaded again (128x128, 171x171, 256x256 BC1: 9 of 440 in one recording of 600 frames); of the
others, whatever their size and format, 75 to 94 in a hundred (256x256 BC3, 341x341 BC1, everything from 512x512 up):
3,034 uploads in those 600 frames beside 4,233 first ones, their ASTC ones decoded and encoded on the GPU a second time.
The frames of those second uploads take 0.9 to 2.5 ms longer than the frames without uploads and the frames after them
wait 0.3 to 1.3 ms longer for the GPU (six recordings of one player, RTX 5090, 2026-11-12). What the hash costs: the
bytes are read once more at the first upload and once at the comparison, 15 to 18 us each for 300 KiB (17 to 20 GB/s,
`content_hash()`), together about what the second upload takes on the submitting thread (27 us for 300 KiB). The gain is
the GPU's work, and a run through Hyrule Field measured it (2026-11-14, RTX 5090, about three minutes of running, the
same route each time): without the hash 44,000 to 52,000 second uploads of 33 to 47 GiB of storage in four runs of six
(9,000 and 11,000 in the other two), one 1536x1536 texture 29 times within 300 frames with the same bytes each time, 66
thousand million texels decoded on the GPU in a run; with it 592 second uploads of 195 MiB, 24 GiB found unchanged, and
24 thousand million texels decoded, which is the first uploads. The second uploads took 2.9 s of the submitting thread
in such a run, the two hashes about 3 s. A texture the CPU really rewrites would pay the hash for nothing each time:
after `CHASH_MISSES` comparisons in a row that found other bytes its uploads take no hash any more. An area load takes
none either (`vk_exec_pool_loading`): it uploads gigabytes in a few frames. `NX2SARIA_TEX_SAME_BIG=0` hashes only the
textures up to `CHASH_MAX`, as before 0.11.0. The pool log's buckets line counts the comparisons and the hashed bytes.

### `content_hash()`

- Eight chains side by side over 64 bytes a step, folded into one: a chain's multiply waits for the one before it, so
  the chains, which wait for nothing of each other, read eight times what one reads in the same time. A step is a
  bijection of its chain for the word it takes and of the word for the chain, as the single chain's is, and so is each
  fold. Measured on textures read from memory (Ryzen 7 9850X3D, clang -O2): one chain 7 GB/s, four 10, eight 17 to 20,
  sixteen no more; a copy of the same bytes runs at 70.

### `astc_bl_regions()`

ASTC for the GPU decoder without the CPU deswizzle: the storage goes into staging as the game laid it out, in one copy,
and the decoder reads each block from its block-linear place (`VkAstcRegion.bl`, `runtime/shaders/astc_decode.comp`
`bl_block()`). The CPU deswizzle moved every 16 bytes through the GOB arithmetic and was most of an ASTC upload's CPU
time when an area loads. Fills ar (n = levels \* layers, level-major as upload's regions) and returns 1 when every level
and layer qualifies: block-linear, starting 16-byte aligned, its layout inside the storage.
`NX2SARIA_ASTC_GPU_DESWIZZLE=0` keeps the CPU deswizzle. levels counts the image's levels: level lv of the image is the
game's level lv + skip (texture detail), which is where its blocks lie and what its size is.

### `astc_arena()`

`NX2SARIA_ASTC_ARENA=1` (the default): the raw ASTC blocks for the GPU decoder stage in a range of the staging arena
(`vk_staging_at`) like the other formats, not in a `VkBuffer` of their own. `vk_astc_record` reads its source only
through a buffer copy at each region's src, so the range's offset is added to the regions. A buffer per texture cost a
`vmaCreateBuffer` and, after the batch, a `vmaDestroyBuffer` each; an area load uploads thousands. The check mode keeps
its own buffer (`astc_check()` reads the blocks at the regions' src offsets). 0: a buffer per texture. With the kept
scratch, a Kakariko walk at 1440p (2026-11-11) went from 342 device memory allocations and 6 frames over 50 ms to 233
and 3.

### The wait before an upload is tried again

`UP_RETRY_FRAMES`: how long an upload whose decode had no memory waits for its next try (upload, `up_wait()`): half a
second of frames. A try costs the copy of the texture's blocks into staging memory, so not every frame.

### `up_wait()`

1 while a texture that is not uploaded waits for its next try (`e->up_retry`; 0 for one never tried).

### `astc_reformat()`

ASTC as BC1: e gets an image of the other format, made with the same parameters, in place of its own: BC1 (bc 2) for the
opaque content of its first upload, BC3 (bc 1) for content with alpha about to go into its BC1 image. The old image and
its views go once the GPU is past the submission being recorded, as a destroyed texture's do (`destroy_tex`), so
commands recorded with it keep it; the new one starts as a new image (`init_image()`) in the upload batch, which runs
before the part being recorded (in cb, the batch upload has open, or its own), and the slots that resolve to e look it
up again (`tex_gen_bump()`). 0 when no image could be made; e keeps its own.

- `vk_exec_image_gone(img)`: views cached under the handle of a destroyed image (`create_tex()`).

### `astc_copy_opaque()`

The upload's copy of size bytes of ASTC storage into staging, with the opaque check of its blocks: 1 when every block is
opaque and `*decoded` stays within `ASTC_OPAQUE_DECODES`. A large texture's copy and check run on the ASTC decode
workers (`runtime/gfx/tex_astc_mt.c` `astc_blocks_opaque_mt()`), a small one's here.

### `chash_wanted()`

Whether an upload of e takes the hash of its storage: always up to `CHASH_MAX`; above it, up to `CHASH_BIG_MAX`, with
`NX2SARIA_TEX_SAME_BIG`, outside an area load, and while the comparisons have not found other bytes `CHASH_MISSES` times
in a row ("The parts of an upload and the content hash" above).

### `tmp_alloc()`

The host temporaries of the CPU's texture work, counted for the memory report (`vk_texture_host()`): the copies an
upload decodes ASTC or expands RGB from (upload), the tight ASTC blocks a texel write decodes (`image_texels()`) and the
texels a write-back reads (`write_back_n()`). Each lives for one upload or write-back; the report gives the bytes in use
when it asks and the most at once since it last asked. Two atomics per temporary, not per frame.

### `upload()`

Copies the whole storage into the image (every level and layer the image has). An image made without its top levels
(texture detail, `VkTexImage.skip`) has the game's level lv + skip at its level lv: the loops run over the image's
levels and read the storage at the game's.

- `int no_mem = 0`: a part's ASTC decode had no memory: the texture is uploaded again later.
- A stale slot (the bindless pools look every slot up): no upload; a later write to mapped storage uploads it
  (`make_fresh_snap()`).
- `e->chash_ok = (uint8_t)chash_wanted(e)`: the hash of what this upload copies (`make_fresh_snap()` compares it).
- Read with the storage held mapped, as the copies below: the game unmaps a texture's memory on its own thread.
- `static __thread char ctx[160]`: what this thread copies, for a crash report.
- `int agpu = astc && vk_astc_gpu(), acheck = agpu && vk_astc_mode() == VK_ASTC_CHECK`: raw blocks to the GPU.
- `uint64_t src_bytes = 0`: the storage bytes the image's levels take (all of the storage unless levels are left out).
- The GPU decoder reads the blocks in place (`astc_bl_regions()`); the check mode compares against tight blocks.
- ASTC as BC1: a BC1 image takes opaque content only, and a BC3 one that may turn BC1 (`bc1_try`) does at its first
  upload when the content is opaque (`astc_reformat()`). The blocks the GPU decoder takes as they lie (gdsw) are checked
  as they are copied; the others are checked here first. Without memory for a BC3 image the upload waits as one without
  memory for its decode does (`no_mem`); without memory for a BC1 one the texture stays BC3.
- A CPU decode or copy stages at most `UPLOAD_PART` bytes per staging buffer (a region larger than that alone) and
  records one copy per buffer into the open upload batch (`vk_upload_end` may submit a batch that holds much staging
  between two parts; queue order and the barriers keep them in order). The GPU decoder takes the raw blocks of every
  region from one buffer.
- `if (gdsw) size = t->L.size`: the storage as it is (one part: the GPU decoder takes every region at once).
- Without its top levels, only the levels the image has: each region's blocks as they lie in the storage, one after the
  other (the top level is three quarters of the bytes, and an array keeps each layer's levels together, so the kept
  levels are no single range).
- The part stages in a range of the staging arena (`vk_staging_at`, its offset added to the regions below), or in a
  buffer of its own at offset 0 when larger than an arena range; the GPU decoder takes one with `NX2SARIA_ASTC_ARENA=0`
  or in the check mode (`astc_arena()`).
- `uint64_t c0 = kern_now_ns()`: the storage copies and decodes of this part (`VK_TS_COPY`).
- The part's copies out of the storage, with it held mapped (`mem_read_hold`): the game releases a texture's memory on
  its own thread, and a decommit in the middle of a copy faulted (`tex_bl_copy`). Only the copies run held, so a waiting
  unmap and the guest's memory calls behind it never wait for the staging block or a decode. Storage released since the
  check above uploads as zeros, as the game no longer has that texture.
- The image holds zeros, not the bytes hashed above: the same bytes mapped back there upload again.
- `else if (held) par_memcpy(stg, t->mem, size)`: large blocks over the `par_jobs` pool.
- `for (k = 0; k < n; k++)`: `astc_bl_regions()` checked that each lies inside the storage.
- `} else if (nvn_deswizzle_mt(d, lv + sk, ly, t->mem, tmp ? tmp : dst, rb))`: large levels: `runtime/par_jobs.c`.
- `mem_read_release()`: the part's reads of the storage are done.
- `for (k = k0; k < k1 && !gdsw; k++)`: the decodes, from the copies (gdsw: ar already names the blocks).
- `tmp_free(tmp, (size_t)rb * rows * sl)`: the size the first loop asked for (raw).
- `for (k = 0; k < n; k++) ar[k].src += soff`: a range of the arena (`astc_arena()`); 0 in a buffer of its own.
- Content with alpha for a BC1 image: a BC3 one first, or no decode for now (-1); opaque content at a BC3 image's first
  upload: a BC1 one (it stays BC3 when none can be made).
- `e->chash_ok = 0`: the image does not hold the bytes hashed above: the next write uploads, equal or not.
- `if (acheck)`: waits for the GPU.
- The decode had no memory (`vk_astc_record`: its scratch buffer failed while the video memory was used up), so the
  image does not hold this content: a new image holds nothing, an older one what it held before. Counted as uploaded, it
  stayed that way until the game next wrote the storage, which for most textures is never. It counts as not uploaded
  instead, and `make_fresh()` and `make_fresh_snap()` upload it again `UP_RETRY_FRAMES` frames on, when the evictions
  under memory pressure have had time to make room; again and again until one succeeds.

### `fills_no_page()`

Whether e's storage fills no 4 KiB page of its own, so the write tracking can only see writes to pages it shares.

### The copy of a texture that fills no page

A texture that fills no page keeps a copy of its storage bytes from when a GPU write to it was recorded
(`vk_texture_gpu_written()`), up to `SNAP_MAX` bytes; `wb_exact` (`NX2SARIA_WB_EXACT`, default on) enables it.

### `storage_rewritten()`

`vk_mem_changed` over the pages wholly inside e's storage (all of its pages when it fills none): a neighbour that shares
the first or last page does not count as a write to e. A texture that fills no page shares all of its pages, so there a
page write counts only when e's own bytes differ from the copy taken at the GPU write. The game's Purah Pad camera
renders the ID, screen rect and depth of what is under the reticle into 4x4 targets of 512 bytes, two slots used in
turn, and the CPU reads them a frame later. One slot's rect target shares its page with data the CPU writes every frame:
without the copy its renders were never written back, the game read a zero rect every other frame and the target's name
label blinked. `NX2SARIA_WB_EXACT=0` restores the page-only decision.

### `same_content()`

The pages of an uploaded texture show a write (or are not tracked): whether its own bytes are still those it was
uploaded with (upload, `content_hash()`). WriteTexels since the upload always counts as a change.

- `static int on = -1`: `NX2SARIA_TEX_SAME=0`: every write to the pages uploads again (diagnosis).
- `if (!on || !e->chash_ok || e->wt_n) return 0`: `chash_ok`: the upload took the hash (`chash_wanted()`).
- `if (!mem_read_hold(e->pub.addr, e->t.L.size)) return 0`: unmapped since: not the same; the upload deals with it.

### `make_fresh()`

A mirrored texture is uploaded on first use and again when its storage pages carry a newer write generation. So is one
split fill fills (`from_storage()`), while it is not released: the game may be unmapping a released one's storage, and
nothing keeps a slot from resolving to it as the stale rule does for a CPU-written one (`slot_resolve()`).

- `vk_mem_changed(e->pub.addr, size, 0, &e->wgen)`: resets the write bits before the storage is read.
- `if (ch && !same_content(e)) upload(e)`: 1 written, -1 not tracked: both upload again, unless the bytes match.

### `make_fresh_snap()`

`make_fresh()` for draw bindings: the page check answers from this submission's write-watch snapshot
(`runtime/vk/vk_memory.c` `vk_mem_changed_snap()`) instead of a system call per texture. A write the snapshot of the
texture's region missed (made after it, during this replay) keeps its write-watch bit and is caught by the next
submission's snapshot.

### Aliases

`NX2SARIA_TEX_ALIAS`.

### `img_block()`

Bytes per block and block size of the image's `VkFormat` (ASTC decodes to RGBA8, RGB expands to RGBA); 0 when the
`VkFormat` substitutes another one of unknown size.

- `if (e->f.astc_bc) { *bw = *bh = 4; return e->f.astc_bc == 2 ? 8 : 16; }`: BC1 or BC3.

### `alias_compatible()`

The new entry e can take src's content texel for texel: colour, one sample, the same image type and storage layout, and
image formats of one block size (0 with a reason in `*why` when not). Block-compressed images need the same shape;
uncompressed ones take the region both cover (`alias_copy()`). The game moves render targets whose size follows an
effect on screen, so a descriptor made over a live target's memory is often a few texels larger or smaller than it. The
game reads the bytes there as its content (the same memory on the console); started empty instead, the scene went black
for a frame about once a second while the camera turned, from 0.6.1 on (issue #253).

- `if (p->skip || q->skip) return *why = "made without its top levels (texture detail)", 0`: level i is not level i.

### `alias_has_gpu_content()`

`src` holds content the storage bytes do not: the GPU wrote it and, for a mirrored texture, it was not written back and
the CPU has not rewritten the storage since (the new entry's upload then reads the newer bytes).

### `alias_copy()`

Maxwell reads the same bytes through both descriptors: src's content goes into the new image e at the replay's current
position, before any later command of the submission uses e. Through a buffer, since bound images share their memory and
image copies may not overlap; e leaves UNDEFINED between the two halves, so its first transition cannot clobber src's
bytes before they are read. 0 when no replay is recording (e is then not initialised yet).

- The region both images cover (`alias_compatible()`: the same shape for block-compressed ones); the rest of a larger
  new image starts as `init_image()` would leave it.

### Descriptors whose image creation failed

After a failure (VRAM exhausted, an unsupported format) a descriptor is not tried again for 2 s, so a draw that names
one every frame does not repeat a failing allocation on every use. Texture lock held.

### `start_line()`

`NX2SARIA_TRACE=perf`: a line for each event of the two kinds that can leave a descriptor showing an image without
content, up to `START_LINES` per address and kind (`said_at()`).

- over: e, which the CPU writes, was made over the live entry `over` and is filled from its storage. `over` stays as it
  is, and a slot that names its descriptor goes on showing it, whatever it holds.
- gone: e, which the CPU does not write, was made at an address the CPU can see, and the released CPU-written entry
  there was destroyed (gone: its description, `gone_rel`: the frame of its release). Nothing uploads e; a slot of e's
  descriptor that showed the destroyed entry's image (`slot_resolve()` follows it while it is live) shows e's from now
  on.

flag: e was taken back (over); how e started (gone): 0 empty, 1 with a copy of another entry's content, 2 with a copy of
the destroyed entry's, 3 filled from its storage (the last two: split fill).

- `static uint64_t n_cpu_empty[3]`: since the last perf line: entries the CPU does not write, at an address it can see,
  that started without content: on their own, over a released entry, over a live one.

### `split_fill()`

Split fill from the storage: e, new and through its first transition, gets its storage's bytes in the upload batch
(`make_fresh()`, as a CPU-written texture's first use) and is marked to follow them or not (`Tex.sfill`). 0 when e is
left as it is: storage over `SPLIT_FILL_MAX`, an image taken from a released entry ("Split fill" above), storage not
mapped, or an upload that had no memory, which `make_fresh()` tries again later.

### `depth_alias_log()`

`NX2SARIA_DEPTH_ALIAS_LOG=1`: `start_at()` logs each new depth entry over a live texture.

### `start_at()`

The start of e's content at its address, for a new entry and for a superseded one taken back (`taken_back`): another
descriptor's released entry there is superseded, and a live one holding GPU-written content is the alias source,
preferring the one that differs only in swizzle (`same_but_swizzle()`). A superseded entry is kept for its own
descriptor, which takes it back here: the game alternates descriptors at some addresses every frame (a 2560x1440 target
and a 480x270 one in Kokiri Forest), and destroying the released one made both images again each frame. It starts over
as a new entry would (alias copy or `init_image()`) when a texture at its address holds GPU content written since its
release (newer), and else keeps its own: the memory still holds what it held then. The game gives its scene picture
(r11g11b10f) a second descriptor at the same address that nothing draws into (rgb9e5, read raw by the post pass), and
the picture, taken back about once a second and restarted empty, went black for a frame (issue #253). Released mirrored
entries are still destroyed: their content is the storage's, uploaded when an entry is made. A released entry that holds
GPU content gives it to e when no live texture there holds any (rsrc): its memory on the console still holds what it
drew, and a new descriptor made for that picture after the old one was let go read it there (a black frame with FSR 3.1
and its frame generation, two presents long).

- `int gone = 0, rel_other = 0`: released entries at the address: CPU-written ones destroyed here, others superseded.
- `char gone_desc[224] = ""`: trace: the last destroyed one, described while it existed.
- Split fill: e is to start with its storage's bytes; msrc: the released CPU-written entry whose image holds them
  (destroyed after the copy), `msrc_gpu`: it holds what the GPU wrote instead, `msrc_gen`: the storage's generation.
- `const char *why = "no live texture with GPU content"`: for the log of an alias left empty.
- `int newer = 0`: taken back: a texture at this address holds GPU content written since e's release.
- `int live_content = 0`: a live texture at this address holds GPU content.
- Each new depth or stencil entry over a live texture, with the frame and whether the two are bound into pool memory
  (the same allocation: they share bytes), to line up with a dark frame.
- `if (taken_back && !newer && !e->pub.mirrored)`: its image still holds its content.
- `if (!tex_alias)`: `create_tex()` initialised a new entry.
- Split fill only where nothing live holds content the storage lacks: that content is what e should show.
- `if (fill && msrc && alias_copy(e, msrc))`: from the released entry this one replaces.
- `if (!msrc_gpu && e->t.L.size <= SPLIT_FILL_MAX)`: the storage's bytes as of `msrc_gen`: followed from there.
- `if (filled)`: nothing more: e is not CPU-written.
- `if (e->pub.mirrored)`: the content is src's GPU render, newer than the storage bytes.
- An entry the CPU does not write, at an address the CPU can see, that starts without content.
- What an empty start over live memory was, to tell a black frame's cause; depth buffers start empty over another
  target's memory at every area load, harmlessly, and are left out.

### Deferred lookups

`n_deferred`: by what the lookup found: no entry, a retired one, a released one.

`tex_deferred`: a replay worker's chunk before its turn (`runtime/vk/vk_exec.c` "replay workers"): a lookup that would
make or take back an entry returns nothing and sets this; the chunk replays the op again once every earlier chunk is
done, so entries are made, initialised and taken back in the order of the stream, as the replay would.

### `lookup_quiet()`

A lookup of k that finds e (NULL: no entry) and needs no turn (`vk_exec_turn_due`): it returns nothing without changing
anything (the pool refresh asking for a held key or a retired entry, a creation that failed lately: the texture pool
refresh looks its unresolved slots up at every sync), or it is the submitting thread's past a group (`vk_exec_behind`)
and takes back a released entry whose image stayed its own. That keeps its content and is ordered after the group's
chunks anyway: one of them that looks the entry up later finds it in use, as it would have after taking it back itself.
A transient render target the frame releases and takes back at every pass that draws it did that after nearly every
group (Hyrule Field, 2026-11-12).

### `entry_of()`

- `if (e && e->retiring)`: retired by disuse: the pool refresh leaves it out, any other lookup takes it back.
- `if (!e && private_keys.n)`: another swizzle of an image that has memory of its own (unalias): the same image.
- `uint64_t c0 = kern_now_ns()`: the entry's creation (`VK_TS_CREATE`): image, memory, first transition.
- `e->samp_frame = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED)`: new: not disused before it is drawn.
- `e->samp_frame = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED)`: in use again, as a new one.
- `if (e->pidx) pent[e->pidx - 1].rel = 0`: the pool's check looks at it again.
- `if (e->superseded)`: its image may still be in use: the first barrier waits for all earlier work.

### `vk_texture_debug()`

`vk_dump`: upload bookkeeping of an image `vk_texture_get()` / `vk_texture_by_id()` returned. `pages_written`: whether
its storage pages were written since the last check (1, 0, -1 not tracked); the query leaves that check's state alone.

- `const Tex *e = (const Tex *)img`: pub is the first member.

### The fence write-back's settings

Render targets the CPU reads without NVN's invalidate (`vk_texture_write_back_small()`); `small_pending`: one may be
dirty. `wb_limit`: targets up to this size (bytes) always go back when the CPU waits for a fence,
`NX2SARIA_WB_FENCE_LIMIT`, read once. A larger one goes back unless the GPU wrote it in each of the last `WB_RUN` frames
(`wb_wanted()`). `wb_trace`: `NX2SARIA_TRACE=gfx`, read together with it.

### `drop_tex()`

`destroy_tex()` with the reason, which every caller gives.

An entry the GPU drew into since its last write-back holds texels its storage lacks. Dropped for a reason that leaves
the storage to the game (a budget, a commit or video memory squeeze, its release, a replacement at its address), a
texture the game makes again from that storage (the drawn-once maps the fence write-back is for, moved or remade) would
start from what the storage held before the GPU drew. Such an entry goes back first when the fence write-back would take
it (`wb_wanted()`) and the CPU has not rewritten its storage since; the commit drop pause counts it the same either way.
Not an entry another target drew over (`DROP_OVERWRITTEN`: the storage is the newer one's), nor at shutdown.

### `wb_wanted()`

Whether the fence write-back takes a dirty target. A target the GPU draws every frame is scratch the CPU does not read:
in Hyrule Field three 256x256 RGBA8 targets and a 256x256 RG8 one, which cost 2.7 ms a frame to write back. The game's
260x260 RG8 grass maps, which its fire spread reads after a fence wait (#132), are drawn once per change, or up to 9
frames in a row while a tile loads.

### `mark_released()`

`tex_lock` held; e is not released.

- `if (e->pidx) pent[e->pidx - 1].rel = 1`: the pool's check passes it by (`vk_texture_pool_fresh_locked()`).

### `overwrite_others()`

`e`, in the GPU-only address window, is written by the GPU in frame fr: the entries its storage runs over are
overwritten ("Moved render targets" above).

- An entry no draw ever wrote is not a moved render target. The game fills a pair of 1120x630 R8 targets another way,
  leaves them for some hundred frames and takes them back (Kokiri Forest): destroyed in between, they came back empty
  and the lit ground was dark for one frame.

### `vk_texture_gpu_written()`

- `make_fresh(e)`: a partial render keeps what the storage held before.
- `if (e->scan_frame != now + 1 && nvn_gpu_only(e->key.addr))`: once per frame and entry.
- `e->chash_ok = 0`: the image now holds the GPU's content: matching storage bytes no longer mean "unchanged".
- `if (e->pub.skip)`: texture detail leaves out what the GPU can write (`detail_skip()`); said if it ever does.
- `storage_rewritten(e, 0, &e->gpu_gen)`: CPU writes up to here are older than this render.

### `vk_texture_release()`

- Staleness belongs to the registered descriptor, not the cached image: a released Tex can be destroyed before the slot
  looks again (eviction, the released cap, `start_at()` for a mirrored one). A registered texture may also be finalized
  before its image is created. Only the slots this texture object registered (its initialization serial): a slot
  registered with another texture initialized at the same storage with the same parameters names that one, which is
  still live. The game streams terrain heightmaps (R32F 260x260) through a fixed set of storage and initializes and
  registers the next chunk's texture there before it finalizes the previous one; finalizing the new registration left
  the slot stale, the draws sampled the zero stand-in, and the chunk was drawn at height zero, tens of metres below the
  ground.
- `if (s->t.serial != t->serial)`: another texture object's registration: it stays current.
- `changed = 0`: the image's release already invalidated the address bucket.

### Descriptor slots

### `vk_texture_register()`

- `if (id >= VK_POOL_TEX)`: the translator clamps pool handles to the pool's last entry: shaders cannot reach it.
- `} else if (id >= VK_POOL_TEX_CARRIED)`: within the pool, past what a shader of the earlier length reaches.
- The same texture and view registered again (the game rewrites much of its descriptor pool every frame): the slot keeps
  its serial and resolved entry, so the texture pool refresh (`runtime/vk/vk_exec_draw.c` `pool_sync_entries()`) does
  not resolve it again. Only while that entry is live: after a release, a registration is what makes a slot current
  again (`slot_resolve()`'s stale rule), so it goes the full way. A slot held out of the pool (cheld) keeps that too:
  the same descriptor names the same held texture. Both only while the resolution is current (cgen): a destroyed entry
  bumped the generation and its record may be freed, so ce is not read past that.
- `if (!tex_mirrored(t))`: FNV-1a over the key (`make_key()` zeroes its padding) and the view.

### `vk_texture_slot_khash_locked()`

The descriptor hash of a GPU-written slot: two slots with equal hashes, the same address generation and no entry
created, destroyed, released or taken back between their lookups (`vk_texture_entry_gen()`) resolve alike. 0 for an
unregistered or CPU-written slot (its resolution depends on when it was registered: `slot_resolve()`'s stale rule).

### The creation budget of slot lookups

`create_budget`, the entry creation budget of slot lookups (`vk_texture_create_budget_locked()`): -1 none. The bindless
pools look up every registered slot, and most of a pool is stale descriptors (storage freed or reused); with a budget a
lookup creates at most that many entries, only for mapped storage, and notes the slots it left for later
(`create_deferred`).

### `want_take()`

A slot whose stand-in a shader sampled is on screen as that stand-in: its texture is made by the next lookup even with
the budget at 0 (the frame's creation time spent, `runtime/vk/vk_exec_draw.c` `pool_create_ns()`), `WANT_PER_FRAME` of
them a present. Left to the budget, it stayed dark for as long as the refresh had no time over, about a second in a
town.

### `vk_texture_pool_shown_locked()`

`NX2SARIA_POOL_SHOWN_FIRST` (default 1; 0: such a slot waits for the budget like any other): the slot being looked up
shows a texture in the pool's set now (`runtime/vk/vk_exec_draw.c` `pool_fill()`), or the game has registered it since
its last lookup that found a texture (`TexSlot.fresh`). When its own texture is not made, the game has registered the
slot with another texture and draws with it next: it is made at this lookup, beside the count budget and the frame's
creation time, `SHOWN_PER_FRAME` of them a present. A slot that showed nothing when it was registered counts too: the
game takes slots whose texture it finalized some time ago for the next ones, and one of those was an effect's texture
with a transparent ground: drawn with the stand-in it was a flat grey patch for one frame at each of two rounds of
registrations four frames apart, in a spot where the effect is not visible otherwise. The game streams its textures by
detail level and registers about 32 slots with their next level every four or five frames while the player moves (2,300
to 3,800 registrations in 600 frames of two recordings). A refresh made 16 of them and wrote the stand-in into the set
for the others until its next round, and the set is read as it is when its submission goes to the queue: an object whose
slot was among the others was drawn grey for a frame (three lamp posts, the windows of a house front in the recordings).
An area load registers thousands of slots that showed the area before; past the per-present count they wait for the
budget as before.

### `vk_texture_pool_make_ns_take()`

What the pool's lookups have spent making entries (`entry_of()` under the budget) and uploading them the first time,
since the last call: the time `runtime/vk/vk_exec_draw.c` `pool_create_ns()` limits in a frame. Texture lock held where
it is added.

### `stale_kept()`

`NX2SARIA_TEX_STALE_KEPT` (default 1; 0: the stand-in, as before): a finalized slot of a CPU-written texture resolves to
its own entry while that entry holds the texture's content: uploaded or drawn by the GPU, and neither superseded by
another descriptor at its address nor overwritten. The game finalizes texture objects on its own thread ahead of the
replay, which still draws the frame the game recorded before that; on the console the memory still holds the texels
those draws read. Kakariko moves about 30 of its textures to new addresses every four or five frames: each move
finalized the old descriptors, the replay's draws of the frame before then met finalized slots, and their stand-ins took
the village's haze out of a frame a few times a minute (thousands of stand-in lookups in 60 frames). The image is
sampled as it is: the lookup neither takes the entry back nor reads its storage (`vk_texture_by_id_locked()`).

### `data_take()`

`NX2SARIA_POOL_DATA_FIRST` (default 1; 0: it waits for the budget like any other): a slot of a texture that is not
block-compressed and holds at most `DATA_FIRST_BYTES` is made at its lookup too, beside the count budget and the frame's
creation time, `DATA_PER_FRAME` of them a present. The budget exists for the pictures the game streams, ASTC that has to
be decoded; a plain-format texture is as often the game's data (a terrain height map, a lookup table, a mask), and a
draw that reads the stand-in in its place for a frame puts geometry or lighting in the wrong place instead of showing a
surface a little late. It costs an upload and no decode.

- `if (!nvn_fmt_bpb(s->t.d.format, &bw, &bh) || bw > 1 || bh > 1) return 0`: unknown or block-compressed.

### `slot_resolve()`

The entry slot `s` resolves to, and the view and `has_view` a caller gets with it (the slot's, with the descriptor's
swizzle when the entry is another descriptor at the same storage). NULL when the slot names no texture.

- `if (!e || e->released || s->finalized)`: a finalized slot follows live GPU content, not newly reused CPU bytes.
- Prefer the live entry that differs from the slot descriptor only in its swizzle: transient render targets share
  storage, and the newest one at the address is usually another size. The map screen samples its 2250x1080 map target
  through an (R,G,B,1) descriptor and followed a 960x60 target at the same address.
- A slot whose content the CPU writes (`tex_mirrored()`) follows another image only for content its own bytes do not
  hold, the GPU's: the game rasterizes each UI caption on the CPU into a small r8 glyph atlas and reuses the memory of a
  freed one for the next, of another width. Following the older 256x39 atlas at the address, the replay uploaded the new
  128-wide bytes with a 256-texel row, and the Basic Controls screen's ZL caption ("Target") sampled a mix of its glyphs
  and stale ones. Without such an image the slot gets its own below (`entry_of()`), uploaded from its storage with its
  own geometry.
- Only an entry of the slot's target: the game's volumetric fog registers a 3D two-channel volume (32x24x64) over the
  storage of the bloom pass's 480x270 2D target; followed, the fog computes and the fog pass got that 2D image, no 3D
  view can be made of it (`runtime/vk/vk_exec_draw.c` `get_view()`) and they read and wrote stand-ins. With the target
  kept they get the volume itself.
- A finalized CPU descriptor must not recreate its image from freed storage. The slot keeps this state even after its
  released image was evicted; registering the descriptor again makes it current. While its own entry still holds the
  texture (`stale_kept()`), the slot samples that image as it is.
- `else if (!e && !cpu_slot) e = own && own->superseded ? NULL : own`: a superseded one: `entry_of()` takes it back.
- Evicted by disuse: the pool refresh keeps the stand-in, without spending the creation budget on it.
- Unmapped storage first: such a slot has nothing to make, now or later. Asked after the budget, every stale slot a
  refresh met with the budget at 0 was noted as left: in a busy frame (its creation time spent) that was about a
  thousand slots at once, which `runtime/vk/vk_exec_draw.c` `pool_sync_entries()` took for an area load, several times a
  second in some places, and each time lifted the count budget and the frame's creation time.
- `if (e && e->retiring)`: retired by disuse: the pool refresh gets the stand-in, any other lookup takes it back.
- A followed texture is another descriptor at the same storage: its image holds the content, but sampling takes the slot
  descriptor's swizzle. The UI renders text into RGBA8 and samples it through a (A,A,A,A) descriptor; with the render
  target's RGBA swizzle the text blur saw R = 1 everywhere and drew solid boxes behind tips and labels.
- `if (!(sw[0] | sw[1] | sw[2] | sw[3]))`: descriptors built without `nvnTextureBuilderSetDefaults`.

### `vk_texture_slot_why_locked()`

- `const Tex *ce = s->ce && s->cgen == agen[sinfo[id] & 0xffff] ? s->ce : NULL`: an older one may be gone.
- What the slot's last lookup kept (`vk_texture_by_id_locked()`): an entry, and whether its image is a volume.

### `vk_texture_bucket_stats()`

Diagnosis for the texture pool log: address-generation bumps since the last call, the slots of the fullest buckets and
the bumps those buckets took. Texture lock held by none: takes it.

- `uint64_t hot = 0`: bumps weighted by the slots they invalidate.
- `if (used < n && big_hash_n + big_same_n + big_diff_n)`: `NX2SARIA_TEX_SAME_BIG`.

### `vk_sampler_gen()`

- `uint64_t at = __atomic_load_n(&samp_retry_at, __ATOMIC_RELAXED)`: a sampler that was not made is due again.

### `unalias()`

`e` was read while marked clobbered (`mark_clobbered()`). It and the descriptors that differ from it only in swizzle
(the same texels: the game renders through one and samples through another) are dropped, and come back as one image in
memory of its own (`create_tex()`, `entry_of()`). A live descriptor of another format or geometry over its storage reads
those bytes through the pool memory, which an image of its own would no longer update: e then stays in pool memory (the
clobbered read is what it was). 1 when e was dropped.

- `destroy_tex(e)`: bumps the address generation: every slot naming it or a sibling resolves again.

### `vk_texture_by_id_locked()`

- Resolved again: an entry at its address was created, dropped or released, or the slot registered (`s->ce` cleared); an
  empty result is not kept, as image creation may succeed later, except a held one for the pool refresh (cheld): the
  pool walks every slot each submission, and the hold ends with a generation bump.
- `s->ce = slot_resolve(s, &v, &hv)`: `entry_of()` may create an entry and bump g: g read before it.
- A draw binding: the texture is in use, whether or not the pool's refresh checked it first in this submission (by list
  that check is no lookup, `vk_texture_pool_fresh_locked()`, so the stamp of the use is left here too).
- `if (e && e->clobbered && unalias(e))`: read after another image overwrote its pool memory: own memory from now on.
- Bindings of one submit share one check, answered from the submit's write-watch snapshot (guest writes during a replay
  race the GPU read on hardware too). After one check an entry is either uploaded or never uploads (not mirrored,
  GPU-owned or no CPU layout), so a slot that checked its entry in this submit has nothing left to do. A held slot
  counts as checked for `look_round`: the textures evicted by disuse are registered slots all the same.
- A released entry here is a finalized slot's own (`stale_kept()`): the storage may be the game's to free or reuse.
- `uint64_t m0 = pool_ctx && !e->uploaded ? kern_now_ns() : 0`: the pool's lookup: as in its list check.

### `vk_texture_exact_by_id_locked()`

Texture detail: the texture slot id resolves to was made without its top levels, and a shader binding reads its texels
by position or asks for its size (`runtime/vk/vk_exec_draw.c` `tex_info_full()`, `GfxShaderRes.exact`). Its key joins
exact, the entry goes and the lookup makes it again whole, uploaded from its storage as a texture seen for the first
time. A lookup that is out of its creation budget leaves the stand-in for now, as for any new texture. Texture lock
held.

- `Tex *e = s->ce && s->cgen == agen[sinfo[id] & 0xffff] ? s->ce : NULL`: what the caller's lookup just resolved.
- `if (exact.n >= EXACT_MAX) nvn_hm_clear(&exact, NULL)`: never seen; they would be found again one by one.

### `vk_texture_slot_swz()`

`vk_dump`: the swizzle selectors of the descriptor registered in slot id; 0 when the slot is empty.

### `vk_texture_new_submit()`

- The last one walked the pool, or checked its entries by list.

### Texel reads and writes

### `storage_texels()`

Pool bytes <-> caller rows of the region alone (`nvn_texels_box`), not through a linear copy of the whole level: for a
small region of a large texture that cost tens of milliseconds while the guest thread held its virtual core and the
caller the texture lock (monsters2 replay, frames 765-790).

### `read_aspect()`

- `switch (e->t.d.format)`: packed depth-stencil storage has no single-aspect copy.

### Reads through staging

`TexRead`: an image region read through a staging buffer. `texels_read_record()` records the copy into the open upload
batch; `texels_read_take()` copies the rows out once that batch is done (`vk_upload_flush`(1)) and frees the buffer.
Several records before one flush cost one GPU round trip instead of one each.

- `size_t slice, ib`: ib: bytes per texel of an expanded RGB image, 0 when the image holds the storage layout.

### `texels_read_record()`

- `if (g->level < e->pub.skip || g->level - e->pub.skip >= e->pub.levels) return 0`: a level the image lacks.

### `image_texels()`

Image <-> caller rows through a staging buffer; the GPU copy is waited for.

- Texture detail: a level the image was made without takes no texels (the storage holds them, `vk_write_texels()`); the
  others are the image's level `g->level` - skip, the rectangle unchanged.
- `e->had |= HAD_TEXELS, e->sfill = 0`: its content is no longer the storage's (split fill).
- `size_t slice = ib ? texels * ib : g->wb * g->hb`: agpu: the raw blocks, decoded by `vk_astc_record`.
- A BC3 image takes whole 4x4 blocks, or a part that ends on the mip's edge; a region of a 5x5 or 6x6 footprint may end
  inside a block (`vk_astc_record` checks the start corner).
- `if (agpu && !bc_cut)`: `bc_cut`: nothing recorded, logged below as not decoded.

### `vk_read_texels()`

ReadTexels: the pool bytes are the content of a mirrored texture the GPU has not rendered into; every other texture is
read back from its image.

### `vk_write_texels()`

WriteTexels: a mirrored texture gets the texels in its pool bytes (and in its image once the GPU owns it); every other
texture in its image.

### CPU reads of GPU-written storage

A mirrored texture the GPU rendered into holds its texels in its image only; its storage keeps what the CPU last put
there. The game may still read that storage with the CPU: the game moves its generated grass maps between pools with
memcpy once the GPU is done, and the copy then carried zeros. Such a read writes the image back first.

### `write_back_n()`

Every level and layer of every texture in v is copied to staging in one upload batch with one wait: one GPU round trip
per level and layer took 0.1 ms each, and a 260x260 grass map has 9 levels. The storage is then written in the order of
v, so where two textures share storage the later one wins.

- `for (uint32_t lv = linear ? 0 : e->pub.skip; lv < levels; lv++)`: the game's levels the image holds.

### `owned_dirty()`

Mirrored textures overlapping [addr, addr+size) that the GPU wrote since their last write-back.

### `vk_texture_storage_bound_locked()`

A storage image may be written by the shaders of the submission being recorded (texture lock held,
`runtime/vk/vk_exec_draw.c` `tex_info()`). Its storage is not marked GPU-owned: only the write fence for ReadTexels.

- `Tex *e = (Tex *)img`: pub is the first member.

### `vk_texture_dirty_fence()`

The newest submission recorded to write a GPU-written mirrored texture overlapping [addr, addr+size); 0: none.

### `vk_texture_write_back()`

The image is the newest content only while nothing wrote the storage since the GPU render: the game finalizes grass
maps, reuses their storage for new data and memcpys that data on, and writing the old image back first handed the copy a
stale map or stale resource bytes (wrong textures in the distance). Every hit is checked before any is written back,
since a write-back is itself a CPU write to storage another hit may share; the hits then go back oldest render first, so
where two descriptors share storage the newer render wins.

- `int n = owned_dirty(addr, size, hit), k = 0`: `owned[]` cannot change while `tex_lock` is held.

### The write-back at a fence wait

The game's grass system renders a 128x128 RGBA8 map of cut and trampled grass into a CPU-cached pool every frame and its
CPU code reads the pool bytes without `nvnMemoryPoolInvalidateMappedRange` (cut reactions: blades, insects, the cut
itself). On the Switch the bytes are there once the frame's fence has passed. So when the CPU waits for a fence,
mirrored targets rendered by work up to that fence go back to their storage (`wb_wanted()`: targets over `wb_limit` only
when not drawn every frame), unless the CPU rewrote it since. Targets drawn every frame still go back on an invalidate,
memcpy or ReadTexels.

### `wb_note()`

`NX2SARIA_TRACE=gfx`: a dirty target the fence write-back leaves alone because it is over the limit and drawn every
frame (what a CPU read after the wait would miss), or sends back although it is over the default limit. One line per
target address and outcome; the table is small and a full one stops logging.

### Samplers

### `sampler_features()`

Caller holds the lock. `sf.aniso`: `NX2SARIA_ANISO`, 0 = the game's.

### `aniso_raised()`

`NX2SARIA_ANISO` raises the level for filtered, mipmapped colour samplers that repeat on U and V: the surface textures,
which tile, so ground and walls seen at a shallow angle stay sharp into the distance. Depth-compare (shadow) samplers,
nearest or unmipmapped ones (data maps, LUTs) and clamped ones keep the game's level: clamped samplers read screen-sized
buffers, whose coordinates can stretch steeply at an object's edge, where anisotropic taps reach across it and leave a
speckled fringe (the inventory's character preview). The game's own wrap modes decide: a mirror-clamp one maps to
`MIRRORED_REPEAT` where the device lacks mirror-clamp-to-edge (`make_sampler()`). Caller holds the lock, after
`sampler_features()`.

### `make_sampler()`

- `if (mf <= NVN_MIN_FILTER_LINEAR) si.maxLod = si.minLod + 0.25f`: not mipmapped: stay on the base level.
- `d->aniso` holds `NX2SARIA_ANISO`'s level where it applies (`sampler_for()`, `aniso_raised()`).
- A depth-compare sampler set to nearest returns 0 or 1 per tap, and the game's shaders smooth shadow edges by a few
  taps spaced for its own shadow map texels. With `NX2SARIA_SHADOW_SCALE` those taps cover a fraction of the area and
  the edges turn hard and stepped; linear filtering makes every tap a 2x2 PCF instead. `NX2SARIA_SHADOW_PCF` does the
  same at the game's own map size (`shadow_pcf()`).
- Not made: the driver ran out of samplers, of custom border colour samplers or of memory. Once more without the custom
  border colour, the reduction mode and anisotropy, which a driver bounds or allocates for, and that sampler stands for
  these parameters; otherwise none, and the draw's descriptor takes the default sampler (`runtime/vk/vk_exec_draw.c`
  `tex_info_full()`, `pool_sync_entries`) until `sampler_for()` tries again. The game used to end here.

### `vk_sampler_register()`

- `__atomic_add_fetch(&samp_gen, 1, __ATOMIC_RELAXED)`: `vk_sampler_gen()` bumps it too, unlocked.

### DLSS upscaling

`dlss_bias` (`vk_sampler_dlss_bias()`): the scene's material textures, the filtered mipmapped ones that repeat, are
sampled for the output's resolution; clamped ones (post effects, LUTs, the UI) keep the game's bias.

### `vk_sampler_dlss_bias()`

- `for (int i = 0; i < nsslots; i++)`: looked up again (the key moves).
- `__atomic_add_fetch(&samp_gen, 1, __ATOMIC_RELAXED)`: `vk_sampler_gen()` bumps it too, unlocked.

### `vk_sampler_aniso_live()`

`NX2SARIA_ANISO`, read again at each present after a settings change (`runtime/vk/vk_backend.c` `present_image()`,
beside the DLSS bias of `vk_dlaa_frame`): the raised level is part of the sampler key (`sampler_for()`), so on a change
the slots look their samplers up again and the descriptor caches follow `samp_gen`, as for `vk_sampler_dlss_bias()`. The
old level's samplers stay until shutdown like every sampler: one set per level at most. Before the first sampler there
is nothing to do: `sampler_features()` reads the setting then.

- `for (int i = 0; i < nsslots; i++)`: looked up again (the key moves).
- `__atomic_add_fetch(&samp_gen, 1, __ATOMIC_RELAXED)`: `vk_sampler_gen()` bumps it too, unlocked.

### `sampler_make_for()`

Caller holds the lock. gated: a lookup of a slot's sampler, which within `SAMP_RETRY_NS` of a failure makes none; 0 for
`vk_sampler_for()` (the default samplers every other falls back to, made once at the start, and the probes).

- `if (dlss_bias != 0 && dlss_biased(&k)) k.lod_bias += dlss_bias`: part of the key: samplers live until shutdown.
- `if (aniso_raised(&k)) k.aniso = sf.aniso`: likewise.
- Within `SAMP_RETRY_NS` of a sampler that was not made, no new one is tried: a slot without one asks at each lookup,
  which would call into the driver for every draw that binds it.
- `if (gated && at && plat_now_ns() < at) return VK_NULL_HANDLE`: the default sampler meanwhile.
- `if (!v)`: never cached (`nvn_hm_put` takes no null), so it is made again after the wait.

### `vk_sampler_by_id_locked()`

Samplers live until shutdown, so a slot keeps the one its parameters map to until it is registered again.

### `vk_sampler_compare_by_id_locked()`

A depth-compare read through a sampler without a compare is undefined in Vulkan. NVIDIA's driver compares anyway; AMD's
returns the raw depth, so the game's shadow tests (which sample this way) come out wrong there. Such a read gets the
slot's sampler with the compare on: the game's function when it set a valid one, LEQUAL otherwise, as NVIDIA's driver
compares. `NX2SARIA_SHADOW_COMPARE=0` binds the game's sampler as it is.

### `vk_sampler_detail_by_id_locked()`

Texture detail: a texture made without its top `skip` levels has the game's level i + skip at its level i, and the LOD
the device works out for it is the game's less skip, so the level a sample lands on is the game's wherever that level is
kept. A sampler's LOD clamp counts the game's levels and has to move down with them: a clamp to levels 0-2 left alone
would let the image's level 2 through, the game's level 4. The bias is relative and stays. A clamp that is open (from 0,
up to more levels than a texture has) is the same sampler. Only draws that bind texture and sampler as one resource come
here (`runtime/vk/vk_exec_draw.c` `tex_info_full()`); the sampler pool's entries serve every texture and keep the game's
clamp.

- `if (k.lod_max < 32) k.lod_max = fmaxf(k.lod_max - (float)skip, 0)`: 32 and more: above any texture's last level.
- `static int told`: a clamp that leaves levels out; one up to a texture's last level changes nothing.

### Compaction

`runtime/vk/vk_tex_compact.h`.

The block table: which of VMA's blocks hold textures, and what else is known to lie in them (the caller's rows: the VRAM
mirrors, `runtime/vk/vk_memory.c` `mirror_rows()`). VMA does not say what a block holds, so the table is built from the
entries' own allocations (`Tex.blk_mem`, `own_memory()`); memory of another kind in a block is not in it.

What may be done with a texture's allocation (`compact_what()`):

- drop: a released entry the size cap would drop too (`drop_stale()`'s rule: it comes back intact, was released
  `stale_after()` presents ago, and no lookup reaches it);
- move: an entry whose image holds what its storage holds and nothing else (`comes_back_intact()`: the CPU provides it,
  the GPU never wrote it), uploaded, with no upload waiting, that no command ever wrote (`wr_fence`) and no shader
  sampled for `COMPACT_IDLE` presents (`samp_frame`, by the pool's marks: without them nothing moves), single-sampled,
  not depth or stencil, and not larger than `most`;
- stay: everything else, and every row of the caller.

The table is of one memory type, the device-local one whose blocks hold the most texture memory: a move lands in the
type it came from, and the allocator's free bytes are counted per heap.

`CompTable`:

- `Tex **ae`: per allocation: its entry; NULL for a row that is not a texture's.
- `nvn_hmap by_mem`: device memory handle -> 1 + the block's index in b.
- `uint32_t type`: the memory type the table is of.
- `int marks`: the pool's marks tell which textures are sampled (`compact_marks()`).

### What keeps an allocation

What keeps an allocation `compact_what()` leaves where it is, for the holes line (`vk_texture_holes()`): a VRAM mirror
(a row of the caller's, no entry), a texture whose content only the GPU made, one a shader sampled within `COMPACT_IDLE`
presents, or one that cannot move for another reason (`compact_what()`'s list). A sparse block that one kind alone keeps
is what a change to that kind's placement or rule would give back.

### `compact_table()`

`tex_lock` held. other[0..nother): allocations of another kind the caller knows, which stay.

### The pass

`NX2SARIA_TEX_COMPACT`, default 0; read at start.

With the setting on, the holes are closed by moving textures: every `COMPACT_PERIOD` presents, when the free space
inside the device-local blocks is at least `NX2SARIA_TEX_COMPACT_MIN_MB` (one block while the video memory in use is
above the VRAM pressure threshold), the block table is built and `runtime/vk/vk_tex_compact.h` chooses the blocks to
empty (`vk_texture_compact_plan()`, at a present). Each submission then takes a few of the listed textures
(`compact_step()`):

- A released one the size cap would drop is dropped (`drop_tex()`, `DROP_COMPACT`), as that rule does it;
- Any other gets a new image in a hole of a block that is not being emptied (`compact_move()`): the allocator is asked
  for existing blocks only, and it fills the fullest first; the old image is copied into it on the GPU, in the upload
  batch, and the entry takes the new image and allocation. Nothing is decoded or uploaded again.

Moving was chosen over releasing because releasing has nothing to take: the entries a compaction would have to release
are live ones the texture pool still names, which `idle()` never holds for (in four traced sessions the idle part of 4
to 5 GiB of CPU-provided textures was 0 MiB), and the path that takes such entries away (`evict_disused()`) is off by
default for the stand-ins it left on screen. A moved texture never shows a stand-in: until a slot is filled again it
names the old image, which holds the same texels.

Who holds a moved texture's old image or its views, and what becomes of each:

- The entry itself (pub.image, alloc, the block fields): swapped under the texture lock; every later lookup, upload,
  read and write-back takes the new image.
- Slots and whatever was resolved through them (a slot's cached entry and view, the texture pool's tokens and
  descriptors, the draws' descriptor memos): `tex_gen_bump()` moves the generation of the entry's address, as for an
  entry made or destroyed, so each is resolved again and gets a view of the new image.
- The texture pool's descriptor sets: the current set is rewritten by the next sync, the others when they are next
  taken. The old image and its views stay until the pool has taken every set of its ring since the move
  (`vk_pool_takes`, as `retire_sweep()` waits) and the GPU is past the submissions recorded until then
  (`compact_old_sweep()`).
- The replay's view cache, the stand-ins of 3D BC targets and the HDR path's notes, all keyed by the image handle
  (`vk_exec_image_gone`): purged when the old image goes, not before, so a submission that holds one of its views keeps
  a live view. Views of the new image are made on demand under its own handle.
- Command buffers recorded or in flight, a submission held at a wait, and the submission being recorded: they name the
  old image or its views, which live on with their texels unchanged; `vk_defer_image` then waits for the submission
  being recorded as for any destroyed texture.
- The upload batch: the copy is recorded behind every upload of the old image (the batch and the queue keep their order)
  and ahead of every submission recorded after the move.
- Uploads in progress: none can be. A candidate is uploaded with no retry waiting, an upload records all its parts under
  the texture lock, and the first upload of an ASTC texture that changes its format (`astc_reformat()`) is behind it.
- Render targets, the lists of GPU-written textures (owned, rts, gwin), the image reuse lists, and the inputs of DLSS,
  FSR and frame generation: all of these hold textures the GPU wrote, which never move (`comes_back_intact()`,
  `wr_fence`).
- A flat texture's 3D copy (flat3d) and a compute pass's depth twin (`runtime/vk/vk_exec_draw.c` `twin_update()`):
  images of their own, made from the entry's image when used; the copy keeps its content, the twin is copied again for
  each use.

The work is bounded by the frame: `COMPACT_FRAME_N` textures and `NX2SARIA_TEX_COMPACT_FRAME_MB` of them per presented
frame, none while the texture pool loads an area, while this frame already spent `COMPACT_WORK_NS` making or uploading
textures, or right after a frame that took a quarter longer than the frames before it (`compact_slack()`). A texture
larger than the frame's bytes is not moved at all, so no frame copies more than that.

A chosen block the pass cannot finish is given up: one of its textures came into use, or no hole outside the chosen
blocks took it. A block emptied of its textures that the allocator does not give back holds memory of another kind (or
is the one empty block the allocator keeps per memory type); it is not chosen again for `COMPACT_HELD_FRAMES`. A pass
that gave nothing back doubles the wait for the next look, up to `COMPACT_WAIT_MAX`.

- `#define COMPACT_PASS_FRAMES PRESENTS(3600)`: a pass still not through its list after this many presents ends there.
- `#define COMPACT_SETTLE PRESENTS(120)`: presents a finished pass waits for its blocks to be freed.
- `#define COMPACT_PASS_MAX ((uint64_t)1 << 30)`: bytes one pass moves at most.
- `#define COMPACT_OLD_MAX ((uint64_t)64 << 20)`: the moved textures' old images hold this much at most while they wait.
- `typedef struct { VkImage img; VmaAllocation alloc; uint64_t bytes, takes, due, frame; int vram; } CompOld`: vram:
  residency's, in video memory.

`CP`:

- `int on`: -1: the settings are not read yet.
- `volatile int32_t active`: a pass runs; set and cleared under the texture lock, read without it.
- `volatile uint64_t next_look`: `gfx_counters.frames` from which the holes are looked at again.
- `uint64_t wait`: presents to the look after the next pass (0: `COMPACT_PERIOD`).
- `CompVictim v[COMPACT_VICTIMS]`: the blocks the pass empties.
- `CompItem *item`: their textures, block by block in the plan's order.
- `uint64_t frame0, done_frame`: `gfx_counters.frames` when the pass began, when its list was through.
- `uint64_t plan_ns, step_ns`: time the plan took (a present), and the steps (the submitting thread).
- `uint64_t holes0`: free bytes inside the device-local blocks when it began.
- `uint64_t frame, frame_n, frame_b`: this presented frame's count and bytes.
- `CompOld old[COMPACT_OLD_N]`: moved textures' old images, waiting for the pool sets and the GPU.
- `struct { uint64_t mem, until; } held[COMPACT_HELD_N]`: blocks emptied and not given back.
- `volatile uint64_t last_ns, avg_ns`: the last frame's interval and a running mean of them (`compact_frame_time()`).
- `uint64_t passes, returned`: passes so far, and what they gave back.

### `compact_frame_time()`

Each present (`vk_texstat_frame()`).

### `compact_slack()`

Whether the frame being made has time to spare ("The pass" above).

### `compact_held()`

`mem` is a block a pass emptied of its textures and the allocator kept (`compact_finish()`).

### `vk_texture_block_freed()`

The allocator gave device memory back to the driver (`runtime/vk/vk_headroom.c` `on_free()`; any thread, no lock): a
block of the pass counts as returned.

### `compact_forget()`

From `destroy_tex()`: `e` goes another way while the pass lists it.

### `compact_old_sweep()`

The old images of moved textures whose views no pool set can hold any more, and no submission that held one still runs
("The pass" above): their views and they go, as a destroyed texture's.

- The takes that count are those of syncs that left no slot behind (`runtime/vk/vk_exec_draw.c` `pool_sync()`), and with
  no creation time to spare, or nothing drawn through the pool, there may be none for a long time. The old images wait
  all the same: `COMPACT_OLD_MAX` bounds what they hold, and the pass moves nothing more.

### `compact_move()`

`e` gets a new image in a hole of a block the pass does not empty, with the old image's texels ("The pass" above). 0
when no such hole took it; e is as it was then.

- Existing blocks of the old allocation's memory type only: a new block, or another type's, would be no gain.
- `if (ok && compact_victim((uint64_t)(uintptr_t)at.deviceMemory))`: only a block being emptied had the room.

### `tex_move()`

`e` takes img, bound to a, with the texels of its current image copied in on the GPU in the upload batch; the caller
keeps the old image and allocation until no pool set and no submission can name them (`compact_old_sweep()`).

- `vk_exec_image_gone(img)`: views cached under the handle of a destroyed image (`create_tex()`).
- `for (uint32_t lv = 0; lv < e->pub.levels;)`: every level whole, all layers at once.

### `compact_finish()`

The pass is over: its line, the blocks the allocator kept, and when the holes are looked at next.

- `} else`: emptied of its textures and still there.

### `compact_step()`

Each submission, texture lock held (`vk_texture_new_submit()`): the old images that can go, then this frame's share of
the pass.

- `if (CP.at < CP.nitem && now - CP.frame0 > COMPACT_PASS_FRAMES)`: the frames never had the time: what is left stays.
- `if (e && CP.frame_b && CP.frame_b + e->bytes > CP.frame_bytes) break`: the next frame's.
- `if (!e) continue`: destroyed since the plan.
- `if (e->blk_mem != v->mem) continue`: its image was made again since (`astc_reformat()`): no longer in the block.
- As it is now: sampled, written or taken back since the plan, it may no longer move.
- `int waiting = 0`: blocks emptied whose memory the deferred destroys have not freed yet.

### `vk_texture_compact_due()`

At a present, no lock: whether the holes are to be looked at now (`runtime/vk/vk_memory.c` `vk_mem_census()`, which then
gathers the VRAM mirrors' rows for `vk_texture_compact_plan()`).

### `vk_texture_compact_plan()`

Looks at the holes and, when emptying blocks would give enough back, starts a pass ("The pass" above).

- `if (CP.active || CP.nold || now < CP.next_look)`: the last pass's old images first.
- `for (int i = 0; i < t.na; i++)`: the block's textures; n is at most `COMPACT_VICTIMS`.

### `vk_texture_holes()`

- `compact_on()`: reads the frame's bytes, which are also the largest texture that moves.
- The sparse blocks a plan could not choose, by what stays in them.

### Statistics and shutdown

### `vk_texture_census()`

A walk over the entries under the texture lock, with the sizes the entries keep (`Tex.bytes`): about 0.1 ms with ten
thousand of them, at most once a `HOLES_PERIOD` (`runtime/vk/vk_memory.c` `owner_log()`) without the perf trace.

### `vk_texture_host()`

The texture cache's host memory for the memory report (`runtime/vk/vk_memory.c` `system_log()`): the entries with their
write-back snapshots (`SNAP_MAX` each), the slot tables (fixed once the first slot is registered,
`vk_texture_register()`) and the CPU's temporaries (`tmp_alloc()`): those in use now and the most at once since the last
call, which starts the next span. The same walk as `vk_texture_census()`, at the same time.

### Reuse of released textures

`vk_texture_reuse_log()`.

How often a released entry is taken back and how long it had been released then, and how often an entry is made again
after a drop that a bound or a pressure made (the image made and its content uploaded or decoded once more). The two
tell what the bounds on the released entries cost: an entry taken back after minutes is one a smaller bound may have
dropped, and one made again soon after its drop is one the bound did drop. Counted since the last line; the dropped keys
are remembered in a table of `DROPPED_N` slots indexed by the key's hash, a newer drop taking the slot. Ages are wall
seconds (`secs_since()`); 1-3 min and 3-5 min lie either side of the default hold (`TRL_HOLD_S`).

- `static uint64_t rb_n[AGE_N], rb_bytes[AGE_N]`: take-backs by the time released (`tex_lock`).
- `static uint64_t ra_n[AGE_N], ra_bytes, ra_why[DROP_N]`: made again by the time since the drop, and by its reason.

The same split by kind and own memory, which decides whether a shorter hold for some kind could save memory without a
take-back finding its entry gone: an entry that comes back intact from the game's memory, one another target was drawn
over, one whose content only the GPU made; each under 1 MiB of own memory (none included) or more.

- `static uint64_t rkb_n[RK_N][AGE_N], rkb_bytes[RK_N][AGE_N], rka_n[RK_N][AGE_N]`: `tex_lock`.
- `static struct { uint64_t kh, frame; uint8_t why, kind; } dropped[DROPPED_N]`: `tex_lock`.

### `age_of()`

Seconds.

### `reuse_taken_back()`

`tex_lock` held; e is being taken back, `rel_frame` still its release.

### `drop_costs()`

Drops of an entry the game may still ask for: past a bound, or under a pressure, or unsampled.

### `reuse_dropped()`

`tex_lock` held.

- `dropped[kh % DROPPED_N].kind = (uint8_t)reuse_kind(e)`: the entry is gone when it is made again.

### `reuse_made()`

`tex_lock` held; e was just made.

### `vk_texture_reuse_log()`

- The split by kind and own memory: one group per kind that had any, the take-backs and the makes again by age.

### `vk_texture_census_log()`

With `NX2SARIA_TRACE=perf` every 600 frames, after `runtime/vk/vk_memory.c`'s vram line: what the live entries' own
memory holds. By kind: decoded ASTC (as BC3 or RGBA8) and other textures whose content the CPU provides, all of which a
re-upload brings back (`comes_back_intact()`), and the GPU-written ones (render targets, textures the GPU drew or copied
into), whose content lives only in the image. "idle" is the part no lookup reached for `IDLE_ROUNDS` pool walks and
`STALE_FRAMES` presents (touch). Then the entries that share their storage address with another live entry (each
descriptor of the same storage gets an image of its own), the formats holding the most, and the largest entries.

- `uint64_t sk_n[3] = {0}, sk_b[3] = {0}`: texture detail: entries made without 1 and 2 levels, and their memory.
- `struct { uint64_t b; uint32_t w, h, d, levels; int fmt, gpu; } big[4] = {{0}}`: copied: read after unlock.
- The addresses whose extra entries hold the most, with their largest few entries (copied: read after unlock).
- `for (uint32_t i = 0; i < by_addr.cap; i++)`: live entries with own memory at one address, beyond the largest.
- `if (detail_most())`: nothing more in the line at full detail.
- `for (int x = 0; x < nfm; x++)`: by bytes, largest first.
- Which entries share an address: each descriptor of the same storage (another size, format or level count) gets an
  image of its own, and its memory beside the others'.

### `vk_texture_shutdown()`

- `for (int i = 0; i < CP.nold; i++)`: moved textures' old images (`compact_old_sweep()`).
- `free(img_reqs.keys), free(img_reqs.vals), free(reuse.keys), free(reuse.vals)`: reuse: emptied by `destroy_tex`.
- `if (sinfo) memset(sinfo, 0, sizeof *sinfo << 20)`: no slot is registered: tokens 0.

### `vk_texture_sizes()`

`NX2SARIA_TRACE=perf` "mem: sizes" (`runtime/diag/perf.c`): the texture cache's entries (the bytes are in the "mem:
vram" line), the released ones, those retired by disuse, the sampler cache, the image requirements cache, the disuse
holds, the reuse keys, and the pool's texture and sampler slots. Counters, and a walk of at most 4096 sampler slots.

### Residency

`NX2SARIA_TEX_RESIDENCY`, default on with the texture pool marks.

Standing in Hyrule Field at full texture detail the live textures held 4.1 GiB, of which the shaders sampled 1.5 GiB in
the last 30 presents (the pool marks, "disuse"); with the released ones, the render targets, the VRAM mirrors and the
driver's own the game used 7.4 GiB of a 6 GiB card's 5.1 GiB budget. Past the budget the driver kept memory in system
memory whatever its use and the GPU took 40 to 90 ms a frame where it takes 20 under it (9 against 28 fps). Here the
textures nothing samples make the room instead. The measure is the driver's figure (`vk_mem_vram`), which counts VMA's
blocks whole: a texture moved out of a block that keeps others leaves a hole the driver goes on counting (1.3 GiB of
them when textures were taken one by one). So textures leave by block:

- out: while the figure is above the budget less `RES_MARGIN`, or a sampled texture waits in system memory for room, the
  blocks whose textures are all cold (no marked shader sampled them for `RES_COLD` presents, or released) by at least
  `RES_BLOCK_COLD` percent of their texture bytes give their cold textures to system memory, the coldest blocks first; a
  texture with memory of its own is a block by itself. An emptied block goes back to the driver once its old images have
  gone.
- back: a texture in system memory that a shader sampled in the last presents gets video memory again, in a hole of a
  block first (no new memory), else in new memory while the figure allows it.

A move is `compact_move()`'s: a new image, a copy on the GPU in the upload batch, the entry takes it (`tex_move()`), and
the old image goes once no pool set and no submission can name it. Nothing is decoded or uploaded again and no slot
shows a stand-in: a texture in system memory is sampled from there, over the bus, until it is back. `RES_FRAME_BYTES`
bounds the copies of a presented frame. A released texture that no lookup reaches is dropped instead of copied (it comes
back from its storage), and one made by an area load that no shader sampled since is cold after `RES_NEW` presents
(`res_cold()`). The host heaps hold what moved out beside the game's imported pools, and are guarded too
(`res_host_free()`). Textures of 1 MiB or more get memory of their own (`RES_DEDICATED_MIN`), so that a move gives it
back whole, and render targets keep a higher priority than textures for the pageable memory (`image_alloc()`).

- `#define RES_HOT 3`: presents: sampled this lately, it is wanted in video memory.
- `#define RES_WARM PRESENTS(15)`: presents: cold enough to make room for one sampled lately (`res_plan()`).

A texture sampled within `RES_HOT` presents comes back into new memory up to the budget less this, not `RES_MARGIN`: in
Hyrule Field a 263 MiB texture array sampled every frame waited in system memory with 330 MiB under the budget, and the
GPU's frame took 2 to 8 ms longer. Coming back puts the figure past the target, so cold blocks then go out.

`RS`:

- `int on`: -1 not read yet.
- `CompOld old[RES_OLD_N]`: the old images of moved textures, until they can go (`res_old_sweep()`).
- `uint64_t frame, frame_b`: the presented frame of the moves counted in `frame_b`.
- `uint64_t scan_frame`: the presented frame the texture table was last walked in.
- `uint64_t used, budget, used_frame`: the driver's figure at `used_frame` less `pend_b`, give or take what moved since.
- `uint64_t *out_blk`: this frame's blocks to empty (sorted device memory handles).
- `int nout_blk, waiting`: waiting: sampled textures left in system memory for want of room.
- `uint64_t wait_b`: the smallest of those, in bytes: the room `res_plan()` makes.
- `uint64_t cold_after`: presents without a sample that make a texture cold this frame (`res_plan()`).
- `uint64_t pend_b`: video memory of moved textures' old images, freed once no work names them.
- `uint64_t host_used, host_budget`: the host heaps' figures at `used_frame`.

### `res_sys()`

`e`'s image lies in a heap that is not video memory.

### `res_old_sweep()`

`compact_old_sweep()`'s rule.

### `res_move()`

`e` gets an image in system memory (`to_sys`) or in video memory, with its texels. hole: video memory only in blocks
that exist. 0 when there was no memory for it.

- Video memory: a large one memory of its own (`RES_DEDICATED_MIN`), which no hole of a block takes.

### `res_host()`

The host heaps: the textures moved out live there beside the game's imported pools, whose import fails once the heaps
are full (`VK_ERROR_OUT_OF_DEVICE_MEMORY`), and a pool without its import lost the device when the GPU went through it.
Moves out stop while the host heaps' use is within `RES_HOST_MARGIN` of their budget, and the textures in system memory
that no lookup reaches (idle) give way, released ones first (`res_host_free()`).

### `res_host_free()`

`tex_lock` held: drops up to want bytes of textures in system memory that come back intact and that no lookup reaches,
released ones first; returns the bytes dropped.

### `vk_texture_host_room()`

Any thread, no lock held: a pool's import failed for want of host memory; 1 when textures gave way for bytes.

### `res_cold()`

Whether `e` may go to system memory. A texture no shader has sampled since it was made counts as cold after `RES_NEW`
presents: the made stamp keeps it from the disuse rules until it is drawn, and an area load makes gigabytes of them at a
few frames a second, which held the video memory 1 GiB past its budget for the whole load (the device was lost in about
half the loads into Hyrule Field at full texture detail). What is drawn then comes back (`residency_step()`, back).

### `resblk_age()`

The coldest (oldest samples) first.

### `res_plan()`

The blocks of video memory to empty this frame (`RS.out_blk`), by the rule of "Residency" above.

- Room for the smallest sampled texture that waits in system memory, past the target. The textures sampled within
  `RES_COLD` alone can fill the video memory: then the room comes from those sampled least lately, down to `RES_WARM`
  presents ago (the coldest blocks go first, and only as many as the room needs). A 263 MiB texture array that Hyrule
  Field samples every frame stayed in system memory for good in about half the runs (180 MiB under the target, with a
  fixed 64 MiB asked for it), and the GPU's frame took 2 to 4 ms longer.
- `if (b[i].age < b[m - 1].age) b[m - 1].age = b[i].age`: a block is as cold as its warmest cold texture.

### `residency_step()`

Each submission, texture lock held (`vk_texture_new_submit()`), as "Residency" above says.

- The old images of the textures moved out still count in the driver's figure until they go, a few frames later:
  counted, they made the room asked for look taken, and new textures took it meanwhile.
- The walks below go over every texture: once a presented frame, not at each of its few dozen submissions, where they
  cost the submitting threads about 0.7 ms a frame in Hyrule Field.
- Back first: into holes, then into new memory while the figure allows it.
- out: whole cold blocks, while above the target or something waits.
- A released entry of a chosen block is dropped, not copied: what it holds comes back from its storage, and the game
  releases hundreds of textures a minute in Hyrule Field with the camera still, which the copies to system memory took
  250 MiB of GPU copies a minute for and freed later all the same. One a lookup still reaches (idle) is copied as a live
  one is.
- `uint64_t sys_n = 0, sys_b = 0, sys_hot = 0`: in system memory, and of it sampled within `RES_HOT` presents.

## `runtime/vk/vk_formats.c`

NVN format -> `VkFormat` for the textures of the graphics layer.

The table is indexed by the NVNformat values of `runtime/nvn/nvn.h` and covers all of them, ASTC decodes to RGBA8 unless
the device samples it (`runtime/vk/vk_astc.c`, `NX2SARIA_ASTC`), D24S8 is always D32S8 (no probing: the RX 9070 lacks
it), 3-component formats expand to 4 components, and formats with no Vulkan equivalent are reported with a reason.

- `F_RGB`, `F_X`: `expand_rgb`, `alpha_one`.

`Row`: `chan` is the component order `vk` holds (`TexVkFormat.chan`), zero when it is NVN's. `alt`, `alt_chan`: a
mandatory format standing in, with its order, when the device lacks `vk`.

The 16-bit packed formats: NVN names their components from the least significant bit, as it does for RGB10A2
(A2B10G10R10): RGB565 holds R in bits 0-4, RGB5A1 R in bits 0-4 and A in bit 15. Vulkan names them from the most
significant bit, so RGB565 is B5G6R5. RGB5A1's counterpart A1B5G5R5 needs `VK_KHR_maintenance5`, so it stands in as
A1R5G5B5 with R and B swapped by the view swizzle: sampling is right, rendering into it would store R and B swapped (no
pass of the game does). The terrain's normal (RGB5A1) and blend (RGB565) maps read with the old order stretched the
ground textures along one axis.

`rows`:

- 0x39, RGBA4: A4B4G4R4 (core in 1.3, optional), else B4G4R4A4 whose A, R, G, B hold NVN's R, G, B, A.
- 0x51-0x68 (integer storage sampled as float) and 0x6B, 0x6C: unmapped; `tex_vk_format()` gives the reason.
- 0x79-0x94 ASTC: handled in `tex_vk_format()`.

`astc`: ASTC block sizes in the order of NVN 0x79..0x86 (linear) and 0x87..0x94 (`sRGB`), with the Vulkan formats they
stand for.

### `tex_vk_format()`

- `if (vk_astc_native(o->ideal))`: `NX2SARIA_ASTC` auto / native on a device that samples ASTC: raw blocks, like BC.

## `runtime/vk/vk_astc.c`

ASTC textures on the GPU: the `NX2SARIA_ASTC` knob, a compute decoder that turns raw blocks into the RGBA8 image the CPU
decoder (`runtime/gfx/tex_astc.c`) would have written, and the native ASTC formats where the device samples them itself.

`NX2SARIA_ASTC_BC` = 1 (gpu mode only; unset: by the video memory and RAM, `create_bc_pipeline()`): a second pass
(`runtime/shaders/bc3_encode.comp`) encodes the decoded texels to BC3 and the image is BC3, 8 bits per texel instead of
32. For GPUs with little video memory, at the cost of BC3's quality: textures look slightly blockier, and data textures
(normal maps) lose some precision. `NX2SARIA_ASTC_BC1` = 1 (default; with BC3 on): a texture whose every ASTC block
decodes opaque (`runtime/vk/vk_texture.c` "ASTC as BC1") is BC1 instead, the colour half of the BC3 blocks the same pass
would write: the same texels at 4 bits per texel.

`NX2SARIA_ASTC` = auto (default): native when the device reports `textureCompressionASTC_LDR` and the format's features,
else gpu. gpu: compute decode into RGBA8 (`runtime/shaders/astc_decode.comp`). native: like auto, but logs when the
device cannot. cpu: `astc_decode_ldr_mt` on the uploading thread, as before. check: gpu, and every upload is also
decoded on the CPU and compared (waits for the GPU; for testing only).

The decode is recorded into the upload batch (`runtime/vk/vk_memory.c`) in chunks of at most 16 MiB of texels: a chunk's
raw blocks go from staging to a device-local scratch buffer, its dispatches write RGBA8 texels behind them, and a
buffer-to-image copy puts them into the image; the next chunk reuses the scratch after a barrier. The next texture
reuses it too, behind the same barrier, while it records into the same command buffer (`scratch_get()`): a buffer per
texture, each kept until its batch completed, made a scene load (2641 textures in one frame in Kokiri Forest) hold
gigabytes of scratch at once and allocate 256 MiB device blocks inline, about half a second of the load frame. A larger
scratch replaces it; the old one is freed once the batches recorded so far completed (`vk_defer_buffer`).

`wunq_data`: the unquantization tables, of `runtime/gfx/tex_astc.c`, as the shader reads them (Tables in
`runtime/shaders/astc_decode.comp`): 12 + 16 row offsets in bytes, then the bytes. Kept in step with
`runtime/gfx/tex_astc.c` by the oracle (`--probe-astc`), which compares every mode.

`Push`:

- `uint32_t bl, th, td, nvn2`: block-linear source (`VkAstcRegion.bl`) and its GOB order (Switch 2 when nvn2).

`G`:

- `VkDevice dev`: the device the objects below belong to (probes recreate it).
- `int mode`: `VK_ASTC_*`; -1 before the first `vk_astc_mode()` with a device.
- `int failed`: pipeline creation failed on dev: gpu falls back to cpu.
- `VkPipeline bc_pipe`: the BC3 encoder (`NX2SARIA_ASTC_BC`): the decoder's layout, bindings 0 and 1.
- `VkPipeline bc1_pipe`: the same shader writing BC1 blocks (`NX2SARIA_ASTC_BC1`).
- `int bc`: 1: ASTC images are BC3 (`vk_astc_bc()`).
- `int bc1`: 1: opaque ones are BC1 (`vk_astc_bc1()`).
- `VkBuffer tab`: device-local copy of the tables.
- `n_chunks`: the breadcrumb number of the next scratch chunk.

`NX2SARIA_ASTC_TIME=1` (or `vk_astc_time_enable()`): timestamps around the dispatches of each scratch chunk (the decode
alone) and around a whole `vk_astc_record()` call (one texture upload or WriteTexels: copies and barriers included),
read back by later calls. Per footprint: texels and dispatch time; a new largest call is logged with both times. Needs
`hostQueryReset` (a slot is reset on the host after its read).

- `static int t_on = -1`: -1: knob not read yet.
- `static struct { uint64_t texels; uint32_t id; uint8_t fp, busy, whole; } t_slot[TQ]`: whole: the call, else a chunk.
- `static struct { uint64_t texels, chunks; double us; } t_fp[13 * 13]`: [bw \* 13 + bh]: dispatch time of the chunks.
- `#define TA 64`: calls in flight, by id % TA.

### `time_init()`

`t_lock` held.

### `time_poll_locked()`

Takes in the slots whose timestamps are available; `t_lock` held.

### `time_slot()`

A free slot for a call's id (`t_lock` held); -1 when the ring is full.

### `reset_for()`

Objects of the current device; under `g_lock` (exclusive).

- `memset(&G.dsl, 0, sizeof G - offsetof(__typeof__(G), dsl))`: the old device's objects went with it.

### `NX2SARIA_ASTC_BC`

`NX2SARIA_ASTC_BC=1`: the BC3 encoder, when the device samples BC3 (every desktop GPU does); 0 leaves ASTC as RGBA8.
Unset: on when the largest device-local heap is under 20 GiB. There RGBA8 ASTC, four times the size of BC3, fills video
memory (a GTX 1650 4 GiB went from 10-23 to 3-12 ms of GPU time a frame, issue #87). The game in Kokiri Forest holds
15.9 GiB of VRAM with RGBA8 and 8.2 GiB with BC3, at the same GPU time and no difference to see in the frame, so 8-16
GiB cards take BC3 too; BC3 loses some precision (normal maps), so 24 and 32 GiB cards keep RGBA8 when the PC has the
memory for it. Windows charges a process's video memory to its commit charge, so RGBA8 textures also count against RAM
plus page file: on a 32 GiB card with 31 GiB of RAM a long session in the world held 17 GiB of texture memory, 29.8 GiB
of VRAM and 45.7 GiB of commit, and a second process beside it ran the system's commit out. So RGBA8 needs 48 GiB of RAM
as well.

- `#define BC_KEEP_RGBA8_RAM_MB (48.0f * 1024 - 1024)`: a 48 GiB PC reports a little under 48 GiB.

### `bc_pipeline()`

The encoder's pipeline: BC3 blocks, or with bc1 BC1 blocks (`runtime/shaders/bc3_encode.comp`'s specialization constant
BC1).

### `bc1_on()`

`NX2SARIA_ASTC_BC1` (with BC3 on): opaque ASTC textures as BC1, when the device samples and copies into both BC1
formats.

### `vk_astc_gpu()`

- `plat_mutex_lock_shared(&g_lock)`: native mode, a format the device lacks: the decoder when it exists.

### The scratch

`CHUNK_OUT`, the scratch per `vk_astc_record()`: texels of at most this many bytes at a time (or one region, when
larger), so a large texture array (169 M texels, 677 MB as RGBA8, on monsters2) does not need its whole RGBA8 copy as
scratch. The scratch is made per upload batch and kept until that batch completes (`scratch_keep()`), and an area load
records many batches in one frame: 12 scratches in the frame that uploaded 2617 textures entering an area, all alive at
once. So each is kept to half a VMA block (`vk_vma_block_size`'s default 64 MiB), where VMA still places it in a block,
not in memory of its own: 16 MiB of texels, their blocks and BC3 fit in 32 MiB.

- `#define SCRATCH_MIN (32ull << 20)`: the shared scratch's usual least size: one chunk's texels plus its blocks and BC3
  (smaller only after the device ran out of memory: `scratch_get()`'s second try).

`S`: the shared scratch and the command buffer it was last recorded into. An upload batch's command buffer is used again
only after that batch completed (`runtime/vk/vk_memory.c`: completed batches go to the spare list), so the same handle
means the open batch, ordered by a barrier, or a finished one. `g_lock` held.

### `scratch_keep()`

`NX2SARIA_ASTC_SCRATCH_KEEP=1` (the default): the scratch outlives its upload batch. Every batch goes to `gfx_queue`, so
the reuse barrier at the start of the next batch's first chunk (`vk_astc_record()`) orders it after the earlier batches'
copies and dispatches, as it does between two textures of one batch; it then also waits for their writes (WAW). With a
scratch per batch, each submission that uploaded an ASTC texture made a new 64 MiB device buffer and deferred the old
one (324 in one Kokiri Forest to Hyrule Field run), and one that found no room in a VMA block allocated a new block on
the replay thread during an area load (8-19 ms per block on NVIDIA, `runtime/vk/vk_headroom.c`). A scratch grown past
`SCRATCH_MIN` for a large texture goes back to `SCRATCH_MIN` once no texture needed more for `SCRATCH_SHRINK_NS`. On a
Kakariko walk at 1440p (2026-11-11) the scratch was made 3 times instead of 60, the walk allocated device memory 265
times instead of 342, and `NX2SARIA_ASTC=check` found 0 of 1920 textures differing from the CPU decode. 0: a scratch per
batch.

### `vk_astc_scratch_fail()`

A test's hook (`runtime/vk/vk.h`): the next n requests for the scratch buffer fail, as when the video memory is used up.

### `scratch_get()`

A scratch of at least `need` bytes for recording into `cb`; `*reused`: earlier work (in `cb`, or with the keep rule in
an earlier batch) may still use it. 0 on failure.



- With the keep rule, a grown scratch is replaced by a small one after a while without large textures.
- The usual scratch leaves room for later textures. This upload can still fit without the spare bytes.
- The old one may still be read by its command buffer: freed once the batches recorded so far completed.

### `vk_astc_scratch_trim()`

Every present (`runtime/vk/vk_texture.c` `vk_texstat_frame()`): a scratch grown past `SCRATCH_MIN` goes once no texture
needed more for `SCRATCH_SHRINK_NS`, without waiting for the next ASTC upload, which is when `scratch_get()` makes it
small again: in a session of 2026-11-13 the decoder held 224 MiB for the 70 s after an area load and from a later load
to the session's end. Nothing is made here: the next request makes the usual scratch, as after `scratch_get()`'s own
shrink. The buffer is freed once the upload batches recorded so far completed (`vk_defer_buffer_try`), as there. Both
locks are only tried. `scratch_get()` holds `g_lock` while VMA makes a buffer, up to 19 ms for a new block. The upload
path takes `cmd_lock` first (`runtime/vk/vk_memory.c` `vk_upload_begin()`) and `g_lock` inside it (`vk_astc_record()`,
`scratch_get()`), so waiting for `cmd_lock` while holding `g_lock` would deadlock with an upload, and waiting at all
would hold the present for as long as an upload flush waits for its fences. When either lock is busy the trim gives way;
the next present tries again. `NX2SARIA_ASTC_SCRATCH_TRIM=0`: the grown scratch stays until the next upload, as in
0.10.1.

### `vk_astc_scratch_bytes()`

The shared scratch's size, for the log's VRAM breakdown (scratches still owned by an upload batch not counted).

### `region_in()`

The source bytes a region reads: its tight blocks, or for a block-linear one the whole of its level as laid out in GOB
blocks (columns of 64 bytes, rows of 8 \* 2^th block rows, layers of 2^td slices), padding included.

### `region_bc()`

The encoded blocks of a region: 16 bytes per 4x4 block for BC3 (bc 1), 8 for BC1 (bc 2).



### `vk_astc_record()`

`BcPush` is the `Params` block of `runtime/shaders/bc3_encode.comp`.



- `for (int i = 0; i < n; i++)`: a BC copy starts on a block corner; a WriteTexels region of a 5x5 or 6x6 footprint may
  not.
- Chunks of consecutive regions; scratch = the largest chunk's blocks, then its texels, then (bc) its BC3 blocks.
- `VkDeviceSize al = c->props.limits.minStorageBufferOffsetAlignment`: the texels bind at `cap_in`, the BC3 blocks after
  them.
- `return -1`: no memory at this moment: the caller tries again later (`runtime/vk/vk_texture.c` `upload()`).
- `if (need_tab)`: first use on this device: the tables go up in this batch, before the dispatch.
- `if (chunks || reused)`: the previous chunk's (or texture's) dispatches and copies are done with the scratch.
- `int slot = -1`: the timestamps bracket this chunk's dispatches: the first waits for the blocks copy.
- `uint32_t bpw = bw * bh <= 64 ? 64 / (bw * bh) : 1`: blocks per workgroup, as `runtime/shaders/astc_decode.comp`
  `main()`.
- `if (bc)`: the chunk's texels to BC3 (or BC1) blocks behind them; the image copy then takes the blocks.
- `uint32_t bb = bc == 2 ? 8 : 16`: bytes per block.

### `vk_astc_release()`

- `if (c && S.buf && S.dev == c->device) vmaDestroyBuffer(c->vma, S.buf, S.alloc)`: the device is idle here.

## `runtime/vk/vk_color.c`

Colour adjustment on the present (declared in `runtime/vk/vk.h`): brightness, contrast, saturation and gamma of the
game's picture, after the game and before the FSR 1 pass or the blit scale it to the window. Settings
`NX2SARIA_COLOR_BRIGHTNESS` (-30..30, % added), `NX2SARIA_COLOR_CONTRAST` and `NX2SARIA_COLOR_SATURATION` (%, 100 =
unchanged) and `NX2SARIA_COLOR_GAMMA` (exponent, 1 = unchanged), read when the settings change. `NX2SARIA_COLOR_PRESET`
names a profile (`vk_color_preset()`) whose values win over the four, so a profile set in `nx2navi.ini` or by
`NX2SARIA_MENU_TEST_SET` applies without the menu; custom uses the four. At the default values nothing is recorded and
the present is what it was. Design and preset values: the colour profiles of issue #136.

One compute pass (`runtime/shaders/color_grade.comp`) reads the crop rectangle of the game's image and writes an RGBA16F
image of its size, which `present_image` then hands to FSR or the blit in place of the game's image. The HUD is part of
that picture and is adjusted with it; the in-game menu is drawn later, straight into the swapchain image, and is not.
The source view of slot s is destroyed when slot s records again (`present_image` has waited for that slot's fence); the
output image is shared by all slots, and each frame's first barrier orders it after the previous frame's reads.

`C`:

- `VkImage img`: the adjusted picture, RGBA16F at the crop size.
- `VkImageView src_view[SLOTS]`: the source view each slot's last frame read.
- `unsigned seen`: `cfg_version` the values below were read at.
- `float adj[4]`: brightness (added), contrast, saturation, gamma.

### `ensure_image()`

- Another crop size (a resolution change): earlier presents may still read the old image.

### `adjusting()`

The four values from the profile, or from their own settings under custom (or an unknown profile name); re-read when the
settings change. 1 when any differs from "unchanged".

- The menu has no colour rows (`runtime/ui/menu.cpp` `tab_screen()`): values left in `nx2navi.ini` could not be undone
  there, so the pass is off and the picture is the game's own.

### `vk_color_record()`

- The game's writes to src are visible; last frame's reads of the output are done (UNDEFINED: its old content goes).
- Later submissions that write src wait for this read; FSR and the blit read the output.

## `runtime/vk/vk_tex_compact.h`

Texture memory compaction, the part without a GPU: from a table of what lies in the allocator's blocks, how much of them
is holes that could go back to the driver, and which blocks to empty for it. `runtime/vk/vk_texture.c` ("compaction")
builds the table and does what the plan says; `tests/tex_compact_check.py` runs this file alone.

The allocator hands out device memory in blocks (`runtime/vk/vk_headroom.c` `vk_vma_block_size()`) and gives a block
back to the driver only when its last allocation went. Textures stream in and out all through a session, so after a
while most blocks hold a few live allocations between holes, and the driver counts every such block whole. A block comes
back when what is left in it goes elsewhere: a texture that only holds what the game's memory holds too can be copied
into a hole of a fuller block (move) or dropped and made again when it is next used (drop). Anything else stays, and a
block with one such allocation cannot be emptied.

The table lists the allocations the caller knows: every texture with memory of its own, and whatever else it can name
(the VRAM mirrors). A block may hold more than that, so the listed bytes of a block are a lower bound of what it holds,
and "size less listed" an upper bound of its holes. Both errors are on the safe side for the room check: the free bytes
outside the chosen blocks are taken as the allocator's total less everything the chosen blocks could hold free. What the
plan cannot see is an unlisted allocation in a chosen block; such a block is emptied of its textures and stays, and the
caller marks it held for the next plans.

`TcpAlloc`: one listed allocation: the index of its block, its size, and what may be done with it.

`TcpBlock`:

- `uint64_t id`: the caller's name for the block (its device memory handle).
- `uint8_t held`: not to be chosen: emptied before and not given back.
- `uint64_t move, drop, stay`: filled by `tcp_tally()`: listed bytes by what may be done with them.
- `uint8_t victim`: `tcp_plan()` chose it.

`TcpParams`:

- `uint64_t free_bytes`: free bytes inside all blocks of the memory type, as the allocator counts them.
- `uint32_t sparse_pct`: a block is chosen only when its listed allocations fill at most this share of it.
- `uint32_t room_pct`: the moves may take at most this share of the free bytes outside the chosen blocks.
- `uint64_t min_gain`: no plan unless the chosen blocks are at least this large together.
- `uint64_t max_move`: bytes one plan moves at most.
- `uint32_t max_blocks`: blocks one plan chooses at most.

`TcpStats`: what the table holds, for the log: every listed block, those at most `sparse_pct` full, and those of them a
plan could choose (nothing listed in them stays, and they are not held).

- `uint64_t block_bytes, listed_bytes`: all listed blocks: their sizes, the allocations listed in them.
- `uint64_t move_bytes, drop_bytes, stay_bytes`: the listed allocations by kind.
- `uint64_t sparse_bytes, sparse_listed`: the sparse blocks: sizes, listed allocations.

`TcpPlan`:

- `uint32_t n`: blocks chosen.
- `uint64_t gain`: their sizes: what goes back when each empties.
- `uint64_t move, drop`: bytes to move and to drop for it.

### `tcp_tally()`

Sums the allocations of `a[]` into their blocks. An allocation naming no block of `b[]` is left out.

### `tcp_sparse()`

At most pct percent of the block is listed allocations. A block listed fuller than its size (a wrong size from the
caller) is not sparse.

### `tcp_emptiable()`

Whether a plan may choose the block: sparse, something in it to move or drop, nothing that stays, not held.

### `tcp_stats()`

After `tcp_tally()`.

### `tcp_plan()`

After `tcp_tally()`: chooses the blocks to empty, the one that takes the fewest moved bytes first (of two alike, the
larger), while what must move still fits the room outside the chosen blocks, and marks them (`TcpBlock.victim`).
order[0..n) gets their indices in the order chosen, n at most `max_order`; `*out` the sums. Returns n, 0 with nothing
marked when the chosen blocks together are smaller than `min_gain`.

- `uint64_t inside = 0`: the most the chosen blocks can hold free: their sizes less what is listed in them.
- `b[i].victim`: 0 not looked at, 1 chosen, 2 looked at and left (no room for it).

## `runtime/vk/vk_tex_detail.h`

The level `texture_detail` = auto picks (`runtime/vk/vk_texture.c` "texture detail", `vk_texture_detail_auto`). Header
only, so that `tests/tex_detail_check.c` checks the choice without a device.

Full from `DETAIL_FULL_MB` of device-local budget on, low under `DETAIL_LOW_MB`, reduced between; under
`DETAIL_RGBA8_MB` one step lower where the ASTC textures stay uncompressed. "Texture detail" in the notes of
`runtime/vk/vk_texture.c` says where each number comes from.

### `tex_detail_auto()`

- `mb`: the device-local budget in MiB, 0 when the device reports none.
- `shared`: the video memory is the system's own (an integrated GPU), whose budget is a share of the PC's memory: never
  full.
- `rgba8`: the ASTC textures stay RGBA8 here (astc = cpu, a card without the decoder, `astc_bc` = 0).

Returns 0 full, 1 reduced, 2 low.

## `runtime/vk/vk_tex_release.h`

Released textures by age, the part without a GPU: how long a released texture is kept, how many bytes the released ones
may hold before the size bound drops young ones, and the wall clock that turns a present count into seconds.
`runtime/vk/vk_texture.c` ("released textures by age", `drop_aged` and `sweep_pressure`) does what these say;
`tests/tex_release_check.py` runs this file alone.

The game releases a texture when what uses it leaves the view and takes it back when that returns. The cache keeps a
released texture's image so that the take-back finds it; a texture taken back after its image was dropped is made again
(created and uploaded, or decoded on the GPU), and the frame that asks for it samples its stand-in. How long after the
release the take-backs come is what decides how long an image is worth keeping. Two sessions on an RTX 5090 (2026-11-12,
one at the old Balanced, one at the old Low; `runtime/vk/vk_texture.c` `vk_texture_reuse_log()`) took back, per line of
3600 presents: under 2 s 22,000 to 121,000; 2 to 10 s 1 to 126; 10 to 60 s 0 to 156; 1 to 5 min and longer none.
Balanced kept every release up to its bound of 4 GiB, so its zero past a minute is not a dropped texture's absence:
nothing released for longer was asked for again. A released texture not taken back within a minute was never taken back.

So a released texture goes once it has gone unused for the hold (`TRL_HOLD_S` by default, `NX2SARIA_TEX_RELEASED_HOLD`),
the longest gap seen, whether or not the bound is reached: the cache then holds what the last minute released, not what
the bound allows, and it empties within a minute or two of the last area change. Unused counts from the release or from
the last time a shader sampled the released image (a finalized slot may show it), whichever is later. The hold is the
same for a texture uploaded from the game's memory and for an ASTC one the GPU decodes: either made again shows its
stand-in for a frame, the decode only costs more of the render thread. A texture whose content only the GPU made (a
render target) cannot be made again at all, it comes back empty: it waits twice the hold.

The size bound (`trl_cap_mb()`: a thirty-second of the video memory budget, `TRL_CAP_MIN_MB` to `TRL_CAP_MAX_MB`) is
what decides how much video memory the cache costs: past it the oldest released textures go even when young, and with
the hold on, young ones past it are kept while the card and the system have room (`trl_room()`), up to `TRL_ROOM_TIMES`
the bound. Until 0.11.0 the hold was 180 s and the bound a quarter of the budget (4 GiB on a 32 GiB card, 8 with room).
A run through Hyrule Field showed what that bought (2026-11-14, RTX 5090, 5120x2160 with DLSS Performance, about three
minutes of running; the game keeps 4.8 GiB of textures live there and lets go of and loads about 190 a second): the
released textures held 5.3 GiB on average and 6.4 at most, the video memory in use went from 11.1 to 17.9 GiB, and of
what they held the game took back 1,494 MiB within 2 to 10 s of the release, 171 MiB within 10 to 60 s and 10 MiB later,
beside 33,000 textures it loaded anew in that time. The memory does not come back when the cache shrinks either: a block
of the allocator goes back to the driver only when its last allocation went, so with a hold of 20 s in the same run the
released textures fell to 0.5 GiB while the free space inside the blocks rose to 4.9 GiB, and the driver's figure stayed
at 15.8. What the cache may hold at its fullest is therefore what the session costs for good. With a bound of 1 GiB (2
with room) the same run held 13.4 GiB on average and 14.8 at most, 3.3 GiB less, made 1,170 MiB of textures again over
the run and kept its frame rate (78 against 75 fps, within what two runs differ by).

- `enum { TRL_STORAGE, TRL_GPU }`: content made again from the game's memory; content only the GPU made.
- `#define TRL_HOLD_S 60`: the default hold, in seconds.
- `#define TRL_HOLD_MAX_S 900`: the longest hold the setting takes: twice it fits the clock below.
- `#define TRL_ROOM_TIMES 2`: with room, the released textures may hold this many times the bound.
- `#define TRL_ROOM_MARGIN 10`: room: the card at least this many points of its budget under the VRAM pressure.
- `#define TRL_CAP_SHARE 32`: the default bound: this share of the video memory budget.
- `#define TRL_CAP_MIN_MB 384`: the bound is at least this much (the render targets a frame lets go of and takes back
  the next frame come to about 70 MiB at 2560x1080; they are not dropped, but they count).
- `#define TRL_CAP_MAX_MB 1024`: and at most this much.
- `#define TRL_CAP_UNKNOWN_MB 512`: without a budget (no `VK_EXT_memory_budget` and no heap size).

### `trl_cap_mb()`

The default bound in MiB for a video memory budget in MiB (0: not known).

### `trl_hold()`

The hold of a class, in seconds; 0: no hold.

### `trl_aged()`

Whether a released texture unused for `unused_s` seconds has passed its hold.

### `trl_room()`

Whether the card and the system have room for young released textures past the bound: the device-local memory in use
(the driver's figure, holes counted) is `TRL_ROOM_MARGIN` points or more under the VRAM pressure's share of the budget,
and the system's commit has at least twice the free space under which it counts as low (`commit_free_mb` below 0: the
commit is not tracked here, which counts as no room).

### `trl_keep()`

The bytes the released textures may hold before the bound drops young ones (`drop_stale`'s keep): the bound, or
`TRL_ROOM_TIMES` it while the hold is on and there is room. cap `UINT64_MAX`: no bound.

### The hard count of released textures

Past the hard count of released textures (`runtime/vk/vk_texture.c` `sweep_hard()`, `RELEASED_HARD`) entries go whatever
they cost, cheapest first by class: one drawn over (its content is gone anyway); one that comes back intact from the
game's memory, released longer ago than its usual gap before a take-back and not looked up lately (`past_gap`); another
one that comes back intact; one whose content only the GPU made, which would come back empty. The last three only when
nothing sampled them lately (`TRL_HC_LATELY`, kept).

### `trl_hard_pick()`

Up to need entries to drop: `cls[i]` is the class of the i-th released entry in release order, n of them; out gets their
indices, the cheapest class first and in release order within a class. Returns how many; 0 when every entry was sampled
lately (the caller then takes the one sampled longest ago).

### The wall clock of the presents

The cache stamps its textures with present counts (`gfx_counters.frames`), and the presents per second change with the
scene and the menus: a hold counted in presents would be shorter in wall time after a fast stretch. The clock notes the
present count once a second or so (tick) and answers how many seconds ago a present was, rounded down to the tick after
it: a texture is taken as at most that old, never older.

- `#define TRL_CLOCK_N 2048`: ticks kept: at least 2048 s, more than twice `TRL_HOLD_MAX_S`.

`TrlClock`:

- `uint64_t frame[TRL_CLOCK_N]`: the present count at each tick.
- `uint32_t sec[TRL_CLOCK_N]`: the tick's second of the run.
- `uint32_t n`: ticks so far.

### `trl_clock_age()`

Seconds from present `frame` to `now_sec`: from the first tick at or after it. A present after the last tick is 0 s old;
one before the oldest tick kept is as old as that tick, at least `TRL_CLOCK_N` - 1 seconds.

- `uint32_t lo = first, hi = c->n - 1`: the first tick with frame >= frame lies in [lo, hi].

### The rate the short waits go by

`runtime/vk/vk_texture.c` counts them in presents (PRESENTS(n): n presents at 60 a second: how long after its release a
texture may go, how long without a sample makes one idle) and scales them by the presents a second, kept as a multiple
of 60 in 1/256. Above 60 they always were scaled. Below 60 they were not: the rate stopped at 60, so "120 presents" was
4 s at 30 presents a second and 5.7 s at 21, and every texture the game let go of stayed out of the VRAM pressure's
reach that much longer. The game lets go by the second, as the player moves, so what the window holds grows with its
length: in a tester's log (RTX 4070 SUPER at 2560x1440, 2026-11-14) it held 442 MiB at 60 presents a second and 2,105
MiB at 21, on a card then 600 MiB over its budget. The rate follows the presents down to `TRL_RATE_MIN` now (low 1; 0:
stops at 60, as before).

- `#define TRL_RATE_MIN 64u`: 15 presents a second: fewer count as 15.
- `#define TRL_RATE_MAX 2048u`: 480 presents a second.

### `trl_rate()`

The rate of the last n samples (one a second): the highest of them, so that a slow second or a few (an area load, a
movie's start) shorten nothing, and only n slow seconds in a row do. A sample not taken yet (0) counts as 60 a second:
the first n seconds of a run never go below it.

### `trl_presents()`

`n` presents at 60 a second, in presents at the rate: the same wall time.
