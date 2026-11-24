#pragma once
#include <stdint.h>

extern int stutter_map_on;

void stutter_map_init(void);
void stutter_area(const char *name);
void stutter_load(void);
void stutter_miss(int shader, int mesh, const uint8_t *vi, const uint8_t *fi, uint64_t ns, int skipped);
void stutter_ready(int mesh, const uint8_t *vi, const uint8_t *fi, double ms, int recorded, int fresh, int fast);
#define STUTTER_CAUSES 10
void stutter_frame(uint64_t frame, uint64_t now, uint64_t d, const uint64_t *part, const char *const *names, int n,
                   int cause);
void stutter_report(const char *tag);
int stutter_report_as(const char *tag, int try_only);
