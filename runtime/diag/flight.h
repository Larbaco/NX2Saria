#pragma once
#include <stdint.h>

#define FLIGHT_THUMB_W 640
#define FLIGHT_THUMB_H 360

extern int flight_on;
extern int vk_dump_white;

void flight_init(void);
void flight_mem(uint64_t *records, uint64_t *thumbs);
void flight_frame(uint64_t frame, uint64_t now_ns, uint64_t interval_ns, const uint64_t blocked_ns[],
                  const volatile int64_t counts[]);
int flight_capture(uint64_t frame, uint64_t interval_ns, const char *dir);

enum { FLIGHT_TEX_NEW, FLIGHT_TEX_UP, FLIGHT_TEX_REUP, FLIGHT_TEX_RELEASE, FLIGHT_TEX_TAKEBACK, FLIGHT_TEX_DROP,
       FLIGHT_TEX_BYTES,
       FLIGHT_TEX_REG,
       FLIGHT_TEX_BLANK,
       FLIGHT_TEX_TOBLANK,
       FLIGHT_TEX_BLANKSUB,
       FLIGHT_TEX_DIRECT,
       FLIGHT_TEX_UNREG,
       FLIGHT_TEX_N };
enum { FLIGHT_TF_DEPTH = 1, FLIGHT_TF_STENCIL = 2, FLIGHT_TF_MIRRORED = 4, FLIGHT_TF_BOUND = 8, FLIGHT_TF_ASTC = 16,
       FLIGHT_TF_GPU = 32 };
void flight_tex(int kind, uint64_t addr, uint32_t format, uint32_t w, uint32_t h, uint32_t layers, uint32_t flags,
                uint32_t detail, uint32_t us);
const char *vk_texture_start_name(int start);
const char *vk_texture_drop_name(int why);

int flight_write_png(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h);

void vk_thumbs_enable(int n);
void vk_thumbs_frame(uint64_t frame);
int vk_thumbs_read(uint64_t frame, uint8_t *rgba);

extern int flight_flash_on;
void flight_cells(uint64_t frame, const uint8_t *cells);
