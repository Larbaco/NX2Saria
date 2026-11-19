// The GPU time of a generated frame through AMD's libraries (vk_fg.cpp, vk_fg4.cpp), for frame generation's log line
// (vk_fg_stats.h): timestamps around the frame's preparation, in the present's command buffer on the game's queue, and
// around its generation, in the command list AMD's swapchain hands its generation callback on its own thread. A frame
// counts once both are in: prepare_end - prepare_begin plus generate_end - generate_begin.
//
// A ring of FGT_SLOTS frames, four timestamps each, reset on the host after they are read (hostQueryReset), so nothing
// is recorded into AMD's command list but the two timestamps. A slot's state moves free -> prepared -> generated on the
// two threads by compare-and-swap; a slot still busy when its frame comes round again leaves that frame untimed, and a
// prepared frame AMD never generated (generation off before its present) is let go FGT_SLOTS / 2 frames later.
// NX2SARIA_FG_GPU_TIME=0 turns it off; without hostQueryReset or timestamps on the game's queue it stays off, and
// the generation is timed only where the present queue's family has timestamps too.
#pragma once
#include <stdint.h>
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

// After the device exists: present_family is the family of the queue AMD's swapchain presents on. 1 when frames are
// timed, *gen whether their generation is (else the preparation alone). Logs one line the first time.
int fgt_init(uint32_t present_family, int *gen);
// Timestamps around the preparation of frame id in cb; begin returns 0 when this frame is not timed (end is then skipped).
int fgt_prepare_begin(VkCommandBuffer cb, uint64_t id);
void fgt_prepare_end(VkCommandBuffer cb, uint64_t id);
// The same around its generation, in the callback's command list (any thread); a frame not prepared with timing is skipped.
int fgt_generate_begin(VkCommandBuffer cb, uint64_t id);
void fgt_generate_end(VkCommandBuffer cb, uint64_t id);
// Frames whose times are in: up to max of them into ms[], their slots free again. now_id: the newest frame id.
int fgt_poll(uint64_t now_id, double *ms, int max);
// The GPU idle (a context made again): slots whose timestamps will never all come (a generation AMD did not run) are
// let go; one being recorded on another thread is left for its owner.
void fgt_idle(void);
void fgt_release(void);   // the device idle

#ifdef __cplusplus
}
#endif
