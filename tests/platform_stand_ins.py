from pathlib import Path

HERE = Path(__file__).resolve().parent

SDL_CLIPBOARD = '''// A stand-in for SDL's clipboard header (tests/platform_stand_ins.py): the platform layer's clipboard calls.
#pragma once
#include <stdbool.h>
bool SDL_SetClipboardText(const char *text);
char *SDL_GetClipboardText(void);
void SDL_free(void *p);
'''


def include(work):
    inc = Path(work) / 'stand-ins'
    (inc / 'SDL3').mkdir(parents=True, exist_ok=True)
    (inc / 'SDL3' / 'SDL_clipboard.h').write_text(SDL_CLIPBOARD)
    return ['-I', str(inc)]


def stand_ins(work):
    return include(work) + [str(HERE / 'platform_stand_ins.c')]
