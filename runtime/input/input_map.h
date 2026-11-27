#pragma once
#include <stddef.h>
#include <stdint.h>

enum {
    B_A = 0, B_B, B_X, B_Y, B_STICK_L, B_STICK_R, B_L, B_R, B_ZL, B_ZR, B_PLUS, B_MINUS, B_LEFT, B_UP, B_RIGHT, B_DOWN,
    B_SL_LEFT = 16, B_SL_UP, B_SL_RIGHT, B_SL_DOWN, B_SR_LEFT, B_SR_UP, B_SR_RIGHT, B_SR_DOWN,
    B_SHAKE = 24,
    ACTION_COUNT = 25
};

enum {
    KEY_NONE = 0,
    KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H, KEY_I, KEY_J, KEY_K, KEY_L, KEY_M,
    KEY_N, KEY_O, KEY_P, KEY_Q, KEY_R, KEY_S, KEY_T, KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z,
    KEY_0, KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9,
    KEY_SPACE, KEY_TAB, KEY_LSHIFT, KEY_LCTRL, KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT,
    KEY_MOUSE_L, KEY_MOUSE_R, KEY_MOUSE_M, KEY_MOUSE_X1, KEY_MOUSE_X2,
    KEY_ENTER, KEY_BACKSPACE, KEY_RSHIFT, KEY_RCTRL, KEY_LALT, KEY_CAPSLOCK, KEY_INSERT, KEY_DELETE, KEY_HOME, KEY_END,
    KEY_PAGEUP, KEY_PAGEDOWN,
    KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6, KEY_F7, KEY_F9, KEY_F12,
    KEY_NUM0, KEY_NUM1, KEY_NUM2, KEY_NUM3, KEY_NUM4, KEY_NUM5, KEY_NUM6, KEY_NUM7, KEY_NUM8, KEY_NUM9,
    KEY_NUM_ADD, KEY_NUM_SUB, KEY_NUM_MUL, KEY_NUM_DIV, KEY_NUM_DECIMAL,
    KEY_MINUS, KEY_EQUALS, KEY_LBRACKET, KEY_RBRACKET, KEY_SEMICOLON, KEY_APOSTROPHE, KEY_COMMA, KEY_PERIOD, KEY_SLASH,
    KEY_BACKSLASH, KEY_GRAVE,
    KEY_COUNT
};

enum {
    PAD_NONE = 0,
    PAD_SOUTH, PAD_EAST, PAD_WEST, PAD_NORTH, PAD_LSTICK, PAD_RSTICK, PAD_L, PAD_R, PAD_START, PAD_BACK,
    PAD_DPAD_LEFT, PAD_DPAD_UP, PAD_DPAD_RIGHT, PAD_DPAD_DOWN, PAD_ZL, PAD_ZR,
    PAD_LPADDLE1, PAD_RPADDLE1, PAD_LPADDLE2, PAD_RPADDLE2,
    PAD_COUNT
};

enum { PAD_LAYOUT_NINTENDO, PAD_LAYOUT_XBOX, PAD_LAYOUT_COUNT };

enum { INPUT_BINDING_MAX = 96, INPUT_PAD_BINDING_MAX = 48 };

typedef struct { uint8_t key; uint8_t action; } Binding;
typedef struct { uint8_t pad; uint8_t action; } PadBinding;
typedef struct InputMap {
    Binding keys[INPUT_BINDING_MAX];
    int nkeys;
    PadBinding pads[PAD_LAYOUT_COUNT][INPUT_PAD_BINDING_MAX];
    int npads[PAD_LAYOUT_COUNT];
} InputMap;

extern InputMap input_map;

void input_map_reset(void);
const char *input_action_name(int action);
const char *input_key_name(int key);
const char *input_pad_name(int pad);
int input_map_bind(const char *button, const char *list);
void input_map_configure(void);

int input_pad_layout_pick(const char *setting, int east_labelled_a);

enum { PROMPTS_NINTENDO, PROMPTS_XBOX, PROMPTS_PC, PROMPTS_PLAYSTATION, PROMPTS_STEAMDECK };
int input_prompts_pick(const char *prompts, const char *pad_layout);
const char *input_prompts_folder(int prompts, const char *pad_layout);
int input_prompts_record(const char *dir, char *out, size_t n);
uint64_t input_map_pad_buttons(const uint8_t down[PAD_COUNT], int layout);

uint64_t input_map_buttons(const uint8_t down[KEY_COUNT]);
int input_map_held(const uint8_t down[KEY_COUNT], int action);
void input_map_stick(const uint8_t down[KEY_COUNT], int stick, int32_t out[2]);
void input_mouse_stick(float dx, float dy, double dt_s, float sens, float cam, int32_t out[2]);
void input_mouse_turn(int32_t dx, int32_t dy, int invert_x, int invert_y, float sens_y, float out[2]);
void input_pad_stick(int32_t x, int32_t y, float dead, int32_t out[2]);
float input_pad_deadzone(int stick);
double input_stick_percent(int32_t x, int32_t y);

int input_map_keys_of(const InputMap *m, int action, uint8_t *out, int max);
int input_map_pads_of(const InputMap *m, int action, int layout, uint8_t *out, int max);
int input_map_set_key(InputMap *m, int action, int slot, int key);
int input_map_set_pad(InputMap *m, int action, int layout, int pad);
void input_map_line(const InputMap *m, int action, char *out, size_t n);
