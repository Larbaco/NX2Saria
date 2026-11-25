#ifndef QUIT_MENU_H
#define QUIT_MENU_H

#include <stddef.h>
#include <stdint.h>

#include "cpu.h"

#define QUIT_MENU_FOLDER "quit-menu"

int quit_menu_wanted(void);
int quit_menu_build(const void *in, size_t n, const void *zsdic, size_t zsdic_n, void **out, size_t *out_n, char *why,
                    size_t why_n);
void quit_menu_texts(int made, int archives, double ms, const char *why);

void quit_menu_screen_host(Cpu *c);
void quit_menu_window_count_host(Cpu *c);
void quit_menu_window_label_host(Cpu *c);
void quit_menu_poll(uint64_t *buttons);

#endif
