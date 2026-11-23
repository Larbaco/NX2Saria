#pragma once
#include <stdint.h>

typedef struct { float amp_low, freq_low, amp_high, freq_high; } RumbleHdValue;

enum {
    RUMBLE_HD_SIDE = 4,
    RUMBLE_HD_DATA = 8,
    RUMBLE_HD_REPORT_ID = 0x10,
    RUMBLE_HD_REPORT = 10,
    RUMBLE_HD_AMP_STEPS = 100,
    RUMBLE_HD_NEUTRAL_REPORTS = 3,
};
#define RUMBLE_HD_LOW_MIN 40.875885f
#define RUMBLE_HD_LOW_MAX 626.286133f
#define RUMBLE_HD_HIGH_MIN 81.75177f
#define RUMBLE_HD_HIGH_MAX 1252.572266f
#define RUMBLE_HD_STALE_NS 250000000ull
#define RUMBLE_HD_REFRESH_NS 30000000ull
#define RUMBLE_HD_GAP_NS 15000000ull

extern const uint8_t rumble_hd_neutral[RUMBLE_HD_DATA];

int rumble_hd_amp_step(float a);
float rumble_hd_step_amp(int step);

void rumble_hd_encode(const RumbleHdValue *v, float strength, uint8_t out[RUMBLE_HD_SIDE]);

void rumble_hd_data(const RumbleHdValue v[2], const int live[2], float strength, int one, uint8_t out[RUMBLE_HD_DATA]);

void rumble_hd_report(const uint8_t data[RUMBLE_HD_DATA], unsigned number, uint8_t *out, unsigned n);

typedef struct {
    uint8_t want[RUMBLE_HD_DATA];
    uint64_t want_at;
    int have;
    uint8_t sent[RUMBLE_HD_DATA];
    uint64_t sent_at;
    int any;
    int neutral_left;
} RumbleHd;

void rumble_hd_post(RumbleHd *h, const uint8_t data[RUMBLE_HD_DATA], uint64_t now);
int rumble_hd_due(const RumbleHd *h, int on, uint64_t now, uint64_t gap_ns, uint8_t out[RUMBLE_HD_DATA]);
void rumble_hd_sent(RumbleHd *h, const uint8_t data[RUMBLE_HD_DATA], uint64_t now);
int rumble_hd_idle(const RumbleHd *h, int on, uint64_t now);
int rumble_hd_buzzing(const RumbleHd *h);
void rumble_hd_forget(RumbleHd *h);
