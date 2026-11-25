#ifndef QUIT_MENU_H
#define QUIT_MENU_H

#include <stddef.h>
#include <stdint.h>

#include "cpu.h"

#define QUIT_MENU_FOLDER "quit-menu"
#define QUIT_MENU_MSG_QUIT "LayoutMsg/GameOver_00.msbt"
#define QUIT_MENU_MSG_PAUSE "LayoutMsg/PlusMenu_00.msbt"
#define QUIT_MENU_MSG_SAVE "LayoutMsg/SystemSaveLoad_00.msbt"
#define QUIT_MENU_MSG_WINDOW "LayoutMsg/SystemWindow_00.msbt"
#define QUIT_MENU_MSG_TITLE "LayoutMsg/Title_00.msbt"
#define QUIT_MENU_LABEL_QUIT "Pa_Button_01-T_Btn_00"
#define QUIT_MENU_LABEL_TITLE_SCREEN "Pa_System_00-Pa_Button_04-T_Btn_00"
#define QUIT_MENU_LABEL_CANCEL "Pa_CheckButton_01-T_Btn_00"

int quit_menu_wanted(void);
int quit_menu_archive(uint8_t **sarc, size_t *n, char *why, size_t why_n);
int quit_menu_build(const void *in, size_t n, const void *zsdic, size_t zsdic_n, void **out, size_t *out_n, char *why,
                    size_t why_n);
void quit_menu_texts(int made, int archives, const char *language, double ms, const char *why);

void quit_menu_screen_host(Cpu *c);
void quit_menu_window_count_host(Cpu *c);
void quit_menu_window_label_host(Cpu *c);
void quit_menu_poll(uint64_t *buttons);

#endif
