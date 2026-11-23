#pragma once
#include <stdint.h>

#include "rumble_hd.h"

void input_poll(uint64_t *buttons, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry);
int input_sdl_version(int *built);
int input_kbm_last(void);
int input_direct_mouse_active(void);
int input_direct_mouse_enabled(void);
int input_direct_mouse_available(void);
#define INPUT_OOT_COUNT_X (0.08 / 1.78)
#define INPUT_OOT_COUNT_Y 0.08
void input_direct_mouse_radial(int32_t dx, int32_t dy, float sensitivity, float cursor[2], int32_t out[2]);
typedef struct { float distance; uint64_t pulse_until, neutral_until; int direction; } InputMouseRow;
void input_direct_mouse_row(int32_t dx, float sensitivity, uint64_t now, InputMouseRow *row, int32_t out[2]);
int input_direct_mouse_selector_active(void);
void input_direct_mouse_sample(int enabled, int32_t *dx, int32_t *dy);
void menu_cursor_poll(uint64_t *buttons);
int menu_cursor_pointer(void);
void menu_mouse_poll(uint64_t *buttons);
int menu_mouse_pointer(void);
int input_game_input_blocked(void);
int input_menu_pointer(float *nx, float *ny);
uint32_t input_menu_mouse_presses(int right);
int32_t input_menu_wheel_take(void);
void input_poll_ui(uint64_t *buttons, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry);
void input_block(int on);
void input_menu(int open);
int input_menu_gate(int *held, int menu_open, uint64_t *buttons, int32_t sticks[4]);
int input_overlay_gate(int *held, uint64_t *buttons);
int input_pad_read(uint64_t *buttons, int32_t *lx, int32_t *ly);
int input_pad_layout(void);
int input_pad_stick_percent(double pct[2]);
enum { PAD_LABEL_NONE, PAD_LABEL_A, PAD_LABEL_B, PAD_LABEL_X, PAD_LABEL_Y, PAD_LABEL_CROSS, PAD_LABEL_CIRCLE,
       PAD_LABEL_SQUARE, PAD_LABEL_TRIANGLE };
int input_pad_label(int pad);
enum { INPUT_CAPTURE_NONE, INPUT_CAPTURE_KEY, INPUT_CAPTURE_PAD, INPUT_CAPTURE_CANCEL };
struct InputMap;
void input_map_snapshot(struct InputMap *m);
int input_capture(int reset, int *id);
uint32_t input_key_char(int key);
int input_flight_key(void);
int input_dump_key(void);

void input_cursor_suspend(int on);
int input_cursor_released(void);

int input_rumble(uint16_t low, uint16_t high, uint32_t ms);
uint16_t rumble_motor(float a, float curve, float strength);
int input_rumble_triggers_enabled(void);
int input_rumble_hd_available(void);
int input_rumble_hd_on(void);
void input_rumble_hd(const RumbleHdValue v[2], const int live[2], float strength);
void input_rumble_hd_stop(void);

void input_motion_start(void);
int input_motion_enabled(void);
void input_motion_axes(const float sdl_acc[3], const float sdl_gyro[3], double acc_g[3], double gyro[3]);

typedef struct {
    int64_t delta_ns, sampling;
    float acc[3], gyro[3], angle[3], dir[9];
    uint32_t attributes, reserved;
} MotionState;
_Static_assert(sizeof(MotionState) == 0x60, "SixAxisSensorState ABI");
typedef struct MotionFusion MotionFusion;
MotionFusion *motion_new(void);
MotionFusion *motion_global(void);
void motion_feed(MotionFusion *m, const double acc_g[3], const double gyro_rad_s[3], uint64_t now_ns);
int motion_read(MotionFusion *m, MotionState *out, int max, uint64_t now_ns);
void motion_restart(MotionFusion *m);

typedef struct {
    int calibrate;
    double deadzone_rps;
    double turn, tilt;
} MotionOptions;
#define MOTION_DEADZONE_LOW 0.002
#define MOTION_DEADZONE_STANDARD 0.007
#define MOTION_DEADZONE_HIGH 0.01
#define MOTION_KP 0.5
#define MOTION_KI 0.1
#define MOTION_OPTIONS_DEFAULT ((MotionOptions){1, MOTION_DEADZONE_LOW, 1.0, 1.0})
void motion_set_options(MotionFusion *m, const MotionOptions *o);
void motion_source(MotionFusion *m, uint64_t id, const char *name);
