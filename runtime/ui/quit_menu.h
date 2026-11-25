#ifndef QUIT_MENU_H
#define QUIT_MENU_H

#include <stddef.h>
#include <stdint.h>

#include "cpu.h"
#include "quit_menu_text.h"

int quit_menu_wanted(void);
void quit_menu_texts(int made, int archives, const char *language, double ms, const char *why);

void quit_menu_screen_host(Cpu *c);
void quit_menu_window_count_host(Cpu *c);
void quit_menu_window_label_host(Cpu *c);
void quit_menu_poll(uint64_t *buttons);

#endif
