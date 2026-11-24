#pragma once
#include <stddef.h>
#include <stdint.h>

#ifndef __cplusplus
#include "cpu.h"
void dbg_tick(Cpu *c);
void dbg_pad(uint64_t *buttons, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry);
void dbg_freecam_host(Cpu *c);
void dbg_fov_host(Cpu *c);
#endif

#define DBG_GAME_DATA_SLOT 0x4c7e180ull

#ifdef __cplusplus
extern "C" {
#endif
int dbg_supported(void);

int dbg_player_pos(float xyz[3]);
int dbg_player_rot(float m[9]);
int dbg_area(char out[64]);
int dbg_texts(void (*cb)(uint32_t hash, const char *text, void *ctx), void *ctx);
uint32_t dbg_name_hash(const char *name);
float dbg_speed_factor(void);
void dbg_speed_set(float factor);

int dbg_flags_refresh(void);
uint32_t dbg_flag_hash(int i);
int dbg_flag_value(int i);
const char *dbg_flag_name(int i);
void dbg_flag_set(uint32_t hash, int on);
int dbg_names(int *found);

int dbg_places(void);
const char *dbg_place_area(int i);
const char *dbg_place_pos(int i);
void dbg_warp(const char *area, const char *pos);
int dbg_place_nearest(const char *area, const float xyz[3], char name[64], float *dist);
int dbg_start_pos(const char *area, const char *pos, char name[64], float xyz[3], float rot[3], char *why, size_t cap);
int dbg_events(void);
const char *dbg_event_name(int i);
void dbg_event_start(const char *name);
int dbg_busy(void);
void dbg_status(char out[160]);

int dbg_freecam_available(void);
void dbg_freecam_set(int on, int sticks, float speed);
void dbg_freecam_reset(void);
int dbg_freecam(int *sticks, float *speed, double pos[3]);
void dbg_freecam_angles(float *yaw, float *pitch);
void dbg_freecam_place(const double pos[3], float yaw, float pitch);
void dbg_camera_watch(int on);
uint64_t dbg_camera_seen(int which, double pos[3], double at[3], float up[3]);
void dbg_route_to(float x, float z, float near_m);
int dbg_route_state(void);
void dbg_route_stop(void);
int dbg_freecam_flying(void);
void dbg_freecam_avoid_set(int on);
int dbg_freecam_avoid(void);
void dbg_freecam_ground(const float *dist, int n, float tan_half_y, float aspect);

int dbg_search_new(uint32_t value, int size);
int dbg_search_next(int mode, uint32_t value);
int dbg_search_count(int *over);
uint64_t dbg_search_addr(int i);
int dbg_search_size(void);
void dbg_search_clear(void);
int dbg_mem_read(uint64_t addr, int size, uint32_t *v);
void dbg_mem_write(uint64_t addr, uint64_t value, int size);
int dbg_freeze(uint64_t addr, uint64_t value, int size, int on);
int dbg_frozen(uint64_t addr);
int dbg_frozen_count(void);
void dbg_unfreeze_all(void);
#ifdef __cplusplus
}
#endif
