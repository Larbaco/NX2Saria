"""The stand-ins the tests that link the platform layer alone build with (tests/platform_stand_ins.c), so they need
neither the rest of the runtime nor SDL3's development files: the platform layer's own calls out are answered there.
Not a test itself."""
from pathlib import Path

HERE = Path(__file__).resolve().parent

# The one SDL3 header runtime/platform/linux.c includes, with the calls it makes.
SDL_CLIPBOARD = '''// A stand-in for SDL's clipboard header (tests/platform_stand_ins.py): the platform layer's clipboard calls.
#pragma once
#include <stdbool.h>
bool SDL_SetClipboardText(const char *text);
char *SDL_GetClipboardText(void);
void SDL_free(void *p);
'''


def stand_ins(work):
    """The compiler arguments that add the stand-ins to a build in the folder work: its include folder and source."""
    inc = Path(work) / 'stand-ins'
    (inc / 'SDL3').mkdir(parents=True, exist_ok=True)
    (inc / 'SDL3' / 'SDL_clipboard.h').write_text(SDL_CLIPBOARD)
    return ['-I', str(inc), str(HERE / 'platform_stand_ins.c')]
