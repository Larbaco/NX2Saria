// Checks of the presentation and sampler paths that must not trust what they are given: the game's crop rectangle
// (gfx_present.c present_image), the swapchain image list a driver or a frame generation library returns
// (gfx_present.c create_swapchain), and how often a failure that can repeat is logged (there and in vk_texture.c
// make_sampler). Own header, without Vulkan, so tests/gfx_guard_check.c checks it alone.
#pragma once

#include <stdint.h>

// What gfx_crop_clip did with a crop.
enum {
    GFX_CROP_WHOLE = 0,    // no crop (none given, or an extent of 0 or less): the whole image, as the game asks
    GFX_CROP_KEPT = 1,     // the crop lies inside the image and is used as it is
    GFX_CROP_CUT = 2,      // it reached past the image's edge (or before its origin) and was cut to the image
    GFX_CROP_OUTSIDE = 3,  // nothing of it is inside the image: the whole image instead of an empty or inverted blit
};

// The source rectangle of a present into out (x, y, width, height): crop is the game's (nvnWindowSetCrop: x, y,
// width, height, guest values, may be NULL) on a w x h image (w, h > 0). A negative origin moves to 0 with the extent
// kept, as before; an extent past the edge is cut there. The edge is compared by subtraction (cw > w - cx, with
// 0 <= cx < w), so no guest value overflows an int, where cx + cw did for an origin or extent near INT_MAX.
static inline int gfx_crop_clip(const int *crop, int w, int h, int out[4]) {
    out[0] = 0, out[1] = 0, out[2] = w, out[3] = h;
    if (!crop || crop[2] <= 0 || crop[3] <= 0) return GFX_CROP_WHOLE;
    int cx = crop[0] < 0 ? 0 : crop[0], cy = crop[1] < 0 ? 0 : crop[1], cw = crop[2], ch = crop[3];
    if (cx >= w || cy >= h) return GFX_CROP_OUTSIDE;
    int cut = crop[0] < 0 || crop[1] < 0;
    if (cw > w - cx) cw = w - cx, cut = 1;
    if (ch > h - cy) ch = h - cy, cut = 1;
    out[0] = cx, out[1] = cy, out[2] = cw, out[3] = ch;
    return cut ? GFX_CROP_CUT : GFX_CROP_KEPT;
}

// Whether a rectangle (x, y, width, height) lies inside a w x h image, compared without overflow.
static inline int gfx_rect_inside(const int r[4], int w, int h) {
    return r[0] >= 0 && r[1] >= 0 && r[2] > 0 && r[3] > 0 && r[0] < w && r[1] < h && r[2] <= w - r[0] &&
           r[3] <= h - r[1];
}

// Whether a swapchain's image list can be used: the count query returned count_r with count images, and the list
// query (with room for count, at most cap) returned list_r with got images. Results are VkResult values: 0 is
// VK_SUCCESS, 5 VK_INCOMPLETE (more images than the room given), below 0 an error. A count of 0, a count past cap
// (an acquire could name an image the arrays have no slot for), a list query that did not succeed (VK_INCOMPLETE
// included) or returned fewer images than counted: the swapchain cannot be used.
static inline int gfx_sc_images_ok(int count_r, uint32_t count, int list_r, uint32_t got, uint32_t cap) {
    return count_r == 0 && count > 0 && count <= cap && list_r == 0 && got == count;
}

// A failure that can repeat every frame is logged the first 8 times and then at each power of two of its count
// (n counts from 1): a session shows that it happened and how often, without a line a frame.
static inline int gfx_fail_log_due(uint32_t n) { return n && (n <= 8 || !(n & (n - 1))); }
