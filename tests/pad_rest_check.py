# needs: clang
import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

DESCRIPTION = """A gamepad axis counts only once it has been seen at rest (runtime/input/input.c pad_axis): builds pad_axis, cut from
input.c by its text, with stand-ins for the SDL calls, the clock and the log it uses, and checks that

  - a stick axis held from the start (a trigger mapped onto a stick, a virtual pad that reports a held axis) reads 0, and
    keeps reading 0 while it stays held; once it has been at rest it reads what the device reports, held or not;
  - a trigger counts only after one second at rest without a break: one reading at rest before it sticks (a 2.4 GHz
    receiver, a virtual pad) leaves it at 0, and a break in the rest starts the second again;
  - a trigger at rest from the start loses no press after its first second, and its release is logged once with the wait;
  - a trigger that never rests is logged once per session with the value it reads;
  - each axis and each gamepad has its own state.

No game files, no SDL. Usage: python tests/pad_rest_check.py [--cc <clang>]
"""

ROOT = Path(__file__).resolve().parents[1]
MSYS_BIN = Path(r'C:\msys64\ucrt64\bin')

HARNESS = r'''
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct SDL_Gamepad { int16_t axis[6]; const char *name; } SDL_Gamepad;
typedef enum { SDL_GAMEPAD_AXIS_LEFTX, SDL_GAMEPAD_AXIS_LEFTY, SDL_GAMEPAD_AXIS_RIGHTX, SDL_GAMEPAD_AXIS_RIGHTY,
               SDL_GAMEPAD_AXIS_LEFT_TRIGGER, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER } SDL_GamepadAxis;
static int16_t SDL_GetGamepadAxis(SDL_Gamepad *p, SDL_GamepadAxis a) { return p->axis[a]; }
static const char *SDL_GetGamepadName(SDL_Gamepad *p) { return p->name; }
static uint64_t now_ns;
static uint64_t plat_now_ns(void) { return now_ns; }
static char logged[64][256];
static int nlogged;
static void rt_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (nlogged < 64) vsnprintf(logged[nlogged++], sizeof logged[0], fmt, ap);
    va_end(ap);
}
static int log_count(const char *a, const char *b) {
    int n = 0;
    for (int i = 0; i < nlogged; i++) n += strstr(logged[i], a) && (!b || strstr(logged[i], b));
    return n;
}
enum { MAX_PADS = 8 };
static SDL_Gamepad *pads[MAX_PADS];
static int npads;
%s
static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL: " __VA_ARGS__); putchar('\n'); fails++; } } while (0)
#define MS 1000000ull
static void open_pad(SDL_Gamepad *p) { pad_state_open(npads); pads[npads++] = p; }
int main(void) {
    const SDL_GamepadAxis LT = SDL_GAMEPAD_AXIS_LEFT_TRIGGER, RT = SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
    SDL_Gamepad real = {{0}, "Pad A"}, ghost = {{0}, "Pad B"};
    now_ns = 5000 * MS;
    open_pad(&ghost), open_pad(&real);
    ghost.axis[SDL_GAMEPAD_AXIS_RIGHTX] = 32767;
    real.axis[SDL_GAMEPAD_AXIS_RIGHTX] = 20000;
    for (int i = 0; i < 100; i++) {
        CHECK(pad_axis(&ghost, SDL_GAMEPAD_AXIS_RIGHTX) == 0, "a held axis read %%d at poll %%d", pad_axis(&ghost, SDL_GAMEPAD_AXIS_RIGHTX), i);
        CHECK(pad_axis(&real, SDL_GAMEPAD_AXIS_RIGHTX) == 0, "the player's held stick read before release");
    }
    CHECK(pad_axis(&real, SDL_GAMEPAD_AXIS_LEFTX) == 0 && pad_axis(&real, SDL_GAMEPAD_AXIS_LEFTY) == 0, "resting axes");
    real.axis[SDL_GAMEPAD_AXIS_LEFTX] = -30000;
    CHECK(pad_axis(&real, SDL_GAMEPAD_AXIS_LEFTX) == -30000, "a stick axis seen at rest once reads the device: %%d",
          pad_axis(&real, SDL_GAMEPAD_AXIS_LEFTX));
    real.axis[SDL_GAMEPAD_AXIS_RIGHTX] = 3000;
    CHECK(pad_axis(&real, SDL_GAMEPAD_AXIS_RIGHTX) == 3000, "a released stick reads the device");
    real.axis[SDL_GAMEPAD_AXIS_RIGHTX] = 32767;
    CHECK(pad_axis(&real, SDL_GAMEPAD_AXIS_RIGHTX) == 32767, "and then reads full tilt");
    CHECK(pad_axis(&ghost, SDL_GAMEPAD_AXIS_RIGHTX) == 0, "another gamepad's axis keeps its own state");

    SDL_Gamepad recv = {{0}, "Receiver"};
    open_pad(&recv);
    CHECK(pad_axis(&recv, LT) == 0, "a receiver's trigger at rest for one reading");
    recv.axis[LT] = 32767;
    for (int i = 0; i < 600; i++, now_ns += 16 * MS)
        CHECK(pad_axis(&recv, LT) == 0, "a trigger at rest for one reading and then held counted as pressed (%%d at poll %%d)",
              pad_axis(&recv, LT), i);

    SDL_Gamepad brk = {{0}, "Pad C"};
    open_pad(&brk);
    for (int i = 0; i < 40; i++, now_ns += 16 * MS) CHECK(pad_axis(&brk, LT) == 0, "a trigger counting its second");
    brk.axis[LT] = 30000;
    for (int i = 0; i < 10; i++, now_ns += 16 * MS) CHECK(pad_axis(&brk, LT) == 0, "a held trigger in its second");
    brk.axis[LT] = 0;
    for (int i = 0; i < 40; i++, now_ns += 16 * MS) pad_axis(&brk, LT);
    brk.axis[LT] = 30000;
    CHECK(pad_axis(&brk, LT) == 0, "a break in the rest starts the second again (0.64 s at rest since the break)");
    brk.axis[LT] = 0;
    pad_axis(&brk, LT);
    now_ns += 1000 * MS;
    CHECK(pad_axis(&brk, LT) == 0, "a trigger after a full second at rest reads the device");
    brk.axis[LT] = 30000;
    CHECK(pad_axis(&brk, LT) == 30000, "a trigger released for use reads a press: %%d", pad_axis(&brk, LT));

    SDL_Gamepad norm = {{0}, "Pad D"};
    open_pad(&norm);
    int lost = 0, presses = 0;
    for (int i = 0; i < 600; i++, now_ns += 10 * MS) {
        int16_t want = i > 100 && i %% 20 < 8 ? 32767 : 0;
        norm.axis[LT] = norm.axis[RT] = want;
        if (want) presses++;
        lost += pad_axis(&norm, LT) != want;
        lost += pad_axis(&norm, RT) != want;
    }
    CHECK(presses > 100 && lost == 0, "a trigger at rest from the start lost %%d of %%d readings after its first second", lost,
          2 * presses);
    CHECK(log_count("\"Pad D\"", "left trigger") == 1 && log_count("\"Pad D\"", "right trigger") == 1,
          "one release line per trigger");
    CHECK(log_count("\"Pad D\"", "1.00 s at rest") == 2, "the release line gives the wait");

    SDL_Gamepad stuck = {{0}, "Pad E"};
    open_pad(&stuck);
    stuck.axis[RT] = 16384;
    for (int i = 0; i < 1300; i++, now_ns += 16 * MS) CHECK(pad_axis(&stuck, RT) == 0, "a trigger that never rests");
    CHECK(log_count("\"Pad E\"", "16384") == 1, "a trigger that never rests is logged once with its value");
    for (int i = 0; i < 1300; i++, now_ns += 16 * MS) pad_axis(&recv, LT);
    CHECK(log_count("never", NULL) + log_count("has not rested", NULL) == 1, "the never-rests line is once per session");

    SDL_Gamepad stray = {{32767}, "Stray"};
    stray.axis[LT] = 32767;
    CHECK(pad_axis(&stray, SDL_GAMEPAD_AXIS_LEFTX) == 32767 && pad_axis(&stray, LT) == 32767,
          "a gamepad not open is read as it is");
    for (int i = 0; i < nlogged; i++) printf("log: %%s\n", logged[i]);
    printf(fails ? "pad rest: %%d failures\n" : "pad rest: ok\n", fails);
    return fails != 0;
}
'''


def cut(text, start, end_marker):
    a = text.index(start)
    b = text.index(end_marker, a)
    return text[a:b]


def main():
    ap = argparse.ArgumentParser(description=DESCRIPTION)
    ap.add_argument('--cc', help='the C compiler (default: MSYS2 UCRT64 clang on Windows, clang elsewhere)')
    args = ap.parse_args()
    win = os.name == 'nt'
    cc = args.cc or (str(MSYS_BIN / 'clang.exe') if win and MSYS_BIN.exists() else 'clang')
    env = dict(os.environ)
    if win and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    src = (ROOT / 'runtime' / 'input' / 'input.c').read_text(encoding='utf-8')
    body = cut(src, 'enum { AXIS_REST', '\nstatic int pad_pressed_logged;')
    with tempfile.TemporaryDirectory(prefix='nx2saria-pad-rest-') as tmp:
        c = Path(tmp) / 'check.c'
        c.write_text(HARNESS % body, encoding='utf-8')
        exe = Path(tmp) / ('check.exe' if win else 'check')
        subprocess.check_call([cc, '-O2', '-Wall', '-Wno-unused-function', str(c), '-o', str(exe)], env=env)
        return subprocess.call([str(exe)])


if __name__ == '__main__':
    sys.exit(main())
