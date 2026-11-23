#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void bugcap_frame(uint64_t frame, uint64_t now_ns, uint64_t interval_ns, uint64_t replay_ns);

int bugcap_note(int *number, int *ok, char name[64]);

const char *bugcap_replay_dir(void);
void bugcap_replay_pad(uint64_t *buttons);
void bugcap_replay_tick(uint64_t tick);

#ifdef __cplusplus
}
#endif
