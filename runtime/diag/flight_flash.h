#pragma once
#include <stdint.h>
#include <string.h>

#define FLASH_CELLS_X 16
#define FLASH_CELLS_Y 9
#define FLASH_CELLS (FLASH_CELLS_X * FLASH_CELLS_Y)
#define FLASH_MOVING (FLASH_CELLS / 4)
#define FLASH_STEP 2

static inline void flash_cells(const uint8_t *px, int w, int h, uint8_t out[FLASH_CELLS]) {
    for (int cy = 0; cy < FLASH_CELLS_Y; cy++) {
        int y0 = cy * h / FLASH_CELLS_Y, y1 = (cy + 1) * h / FLASH_CELLS_Y;
        uint32_t sum[FLASH_CELLS_X] = {0}, n[FLASH_CELLS_X] = {0};
        for (int y = y0; y < y1; y += FLASH_STEP) {
            const uint8_t *row = px + (size_t)y * (size_t)w * 4;
            for (int cx = 0; cx < FLASH_CELLS_X; cx++) {
                int x0 = cx * w / FLASH_CELLS_X, x1 = (cx + 1) * w / FLASH_CELLS_X;
                uint32_t s = 0;
                for (int x = x0; x < x1; x += FLASH_STEP) s += row[4 * x] + 2u * row[4 * x + 1] + row[4 * x + 2];
                sum[cx] += s;
                n[cx] += (uint32_t)(x1 - x0 + FLASH_STEP - 1) / FLASH_STEP;
            }
        }
        for (int cx = 0; cx < FLASH_CELLS_X; cx++)
            out[cy * FLASH_CELLS_X + cx] = n[cx] ? (uint8_t)(sum[cx] / (4 * n[cx])) : 0;
    }
}

static inline int flash_test(const uint8_t prev[FLASH_CELLS], const uint8_t cur[FLASH_CELLS],
                             const uint8_t next[FLASH_CELLS], int diff, int min_cells) {
    uint8_t apart[FLASH_CELLS];
    int moving = 0, agree = diff / 4;
    for (int i = 0; i < FLASH_CELLS; i++) {
        int d = (int)prev[i] - (int)next[i];
        apart[i] = (uint8_t)(d < 0 ? -d : d);
        moving += apart[i] > agree;
        if (apart[i] > diff) return 0;
    }
    if (moving > FLASH_MOVING) return 0;
    int n = 0;
    for (int y = 0; y < FLASH_CELLS_Y; y++)
        for (int x = 0; x < FLASH_CELLS_X; x++) {
            int i = y * FLASH_CELLS_X + x, a = (int)cur[i] - (int)prev[i], b = (int)cur[i] - (int)next[i];
            if ((a > 0) != (b > 0)) continue;
            if (a < 0) a = -a, b = -b;
            int by = a < b ? a : b;
            if (by <= diff) continue;
            int around = 0;
            for (int v = y > 0 ? y - 1 : 0; v <= y + 1 && v < FLASH_CELLS_Y; v++)
                for (int u = x > 0 ? x - 1 : 0; u <= x + 1 && u < FLASH_CELLS_X; u++)
                    if (apart[v * FLASH_CELLS_X + u] > around) around = apart[v * FLASH_CELLS_X + u];
            n += around * 4 <= by;
        }
    return n >= min_cells ? n : 0;
}

#define FLASH_KEEP 4
typedef struct {
    uint64_t frame[FLASH_KEEP];
    uint8_t cells[FLASH_KEEP][FLASH_CELLS];
} FlashTrack;

static inline int flash_note(FlashTrack *t, uint64_t frame, const uint8_t cells[FLASH_CELLS], int diff, int min_cells) {
    if (!frame) return 0;
    unsigned s = (unsigned)(frame % FLASH_KEEP);
    t->frame[s] = frame;
    memcpy(t->cells[s], cells, FLASH_CELLS);
    if (frame < 3) return 0;
    unsigned c = (unsigned)((frame - 1) % FLASH_KEEP), p = (unsigned)((frame - 2) % FLASH_KEEP);
    if (t->frame[c] != frame - 1 || t->frame[p] != frame - 2) return 0;
    return flash_test(t->cells[p], t->cells[c], t->cells[s], diff, min_cells);
}
