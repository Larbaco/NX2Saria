#ifndef PAUSE_TAB_H
#define PAUSE_TAB_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float tab[4];
    float page[4];
    float alpha;
    int header, page_ok;
} PauseTabView;

int pause_tab_on(void);
int pause_tab_test(const char *word);
int pause_tab_name_kind(void);
void pause_tab_poll(uint64_t *buttons);
int pause_tab_request_take(void);
void pause_tab_set_open(int open, const char *why);
int pause_tab_view(PauseTabView *v);
void pause_tab_applied(int changes, const char *list);

#ifndef __cplusplus
#include "cpu.h"
void pause_tab_screen_host(Cpu *c);
#endif

#ifdef __cplusplus
}
#endif

#endif
