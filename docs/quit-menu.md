# Quit Game in the game's own menus (1.1.1)

On the console the game is never closed from inside: its menus have no entry for it. `runtime/ui/quit_menu.c` adds one in
two of the game's own screens, for a player who has only a gamepad in hand (a TV, a handheld):

- the title screen's list gets a fourth entry, *Quit Game*, under the game's three;
- the confirmation the pause menu's *Title Screen* opens gets a third button: *Title Screen*, *Cancel*, *Quit Game*.

Players' side: [controls.md](controls.md#quitting-the-game). The implementation notes, with the disassembly of every
patch site: [code/ui.md](code/ui.md), `runtime/ui/quit_menu.c`. This page records what the game has and why the entry
was built this way.

## What the game has

- **The title screen** (`Title_00`, the class `UITitleScreen`, set up at `main+0x34067d0`) makes one button for every
  `Pa_Button_NN` part its layout holds, and the layout holds four. The game uses three (Continue, New Game, Options);
  the fourth, `Pa_Button_03`, is a button like the others whose part pane is not shown. The list's alignment pane lays
  out the buttons that are shown, so a fourth one that is shown gets its place under the others.
- **The title's flow is data** (`Sequence/NormalTitle.module.ainb`): it waits for a button and branches by the
  button's name, and knows `Pa_Button_00` to `Pa_Button_02` only. A press on the fourth button is decided by the button
  and then leads nowhere: the menu stays.
- **The pause menu's System page** (`PaSystemMenu_00`, set up at `main+0xf790e0`) makes its five buttons one by one in
  fields of its own (Save, Load, Options, Special Actions, Title Screen). There is no spare one and none can be added
  from the layout.
- **The game's confirmation window** (`SystemWindow_00`, opened at `main+0x3405a00` with a kind) makes a button for each
  of the three its layout holds and shows two of them (its `BtnNum` animation at frame 2). The kind picks the question
  (the label `%04d` of the kind) and the first button's text (the label `10%02d`); the second button is *Cancel* and
  has the focus when the window opens. Kind 2 is the pause menu's *Return to the title screen? Any unsaved progress
  will be lost.*
- **A button's text** is set by the game's code only: a text pane takes a message through the setter at
  `main+0x43b970`, which the screens call for the panes they name. A label added to a message file reaches no pane by
  itself. The window has a helper for it (`main+0x3405bc0`: the screen, the button's number, the label's number),
  which reads nothing of the window but its layout, a field every screen has.

## Where the entry goes

**The title: the fourth button.** Its part pane is shown (one bit of the pane's flags, written once: the game does not
clear it again), and the game's own alignment, focus, cursor and sounds do the rest. The game sets its text with its
helper, called once more at the end of the title's set-up (a patch site).

**The pause menu: the third button of the *Title Screen* confirmation.** A sixth entry on the System page would need a
button object the game does not make. The window the fifth entry opens already has a third button, so it is shown for
that kind (the animation's frame 3 instead of 2, a patch site), and the game sets its text with a third call of its
helper (a patch site). The window keeps its question and its focus on *Cancel*.

A sixth entry on the System page itself was not built. It would take calling the game's constructor of a page button
(`main+0xf79390`, with memory from the page's heap) and the call that follows it for each button
(`main+0x43c6c0`) from a patch in the page's set-up, a part pane added to `PaSystemMenu_00.buil`, and a branch for it in the pause menu's sequence data, which
names the buttons it knows: three kinds of change to the game instead of one, each with its own way to fail.

## What happens on A

The game never gets the A that is pressed while the focus is on one of the two new buttons: the pad read takes it out
(`quit_menu_poll()`, where the mouse in the menus adds its presses) and asks the window to close, which is what *Quit
game* of the settings menu does (`gfx_window_request_close()`): the log ends with `gfx: window closed`, the session
report says `normal`, the launcher offers no crash log, and the settings and caches written at exit are written. While
the game is writing a save the close waits for it.

Nothing in the game runs for that press. Were it to reach the game: on the title the button is decided and the menu
stays (the sequence has no branch for it; checked with `NX2SARIA_QUIT_MENU_TEST=pass`).

There is no confirmation on the title: nothing is unsaved there. In the pause menu the window is the confirmation.

## The texts

Every text is one the game ships, in the game's language, copied inside that language's message archive
(`Mals/<region>.Product.<n>.sarc.zs`) when the game starts:

| Label added to `LayoutMsg/SystemWindow_00.msbt` | Text of | For |
|---|---|---|
| `1012` | `LayoutMsg/GameOver_00.msbt`, `Pa_Button_01-T_Btn_00` (*Quit Game*) | the fourth title entry and the window's third button |
| `1010` | `LayoutMsg/PlusMenu_00.msbt`, `Pa_System_00-Pa_Button_04-T_Btn_00` (*Title Screen*) | the window's first button, with three shown |
| `1011` | `LayoutMsg/SystemSaveLoad_00.msbt`, `Pa_CheckButton_01-T_Btn_00` (*Cancel*) | the window's second button, with three shown |

The window's own two labels are a bare yes and no in some languages (Dutch *Ja* and *Nee*; Brazilian Portuguese
*Voltar*), which three buttons would make unclear, so with three shown the first two take the explicit labels above.
In the game *Quit Game* is the Game Over screen's way back to the title; here it closes the game.

The archive is served from `<profile>/quit-menu/`; it grows by a few hundred bytes, so its entry in the resource size
table is raised. Only the archive of the game's language is edited (about 0.1 s at start; all sixteen took 1.1 s).

## All or nothing

- `NX2SARIA_QUIT_MENU=0` (`quit_menu = 0` in `nx2saria.ini`) turns the set `quit_menu` off: every site runs the game's
  own instruction and the menus are the game's.
- Each screen is on only when the words at its sites are the ones the sites were written for, and the labels were
  added. A screen that is off shows no button: the title's fourth pane stays hidden, the window opens with two.
- Game versions other than 1.1.1 have no such sites.

## Log

- `quit_menu: on (NX2SARIA_QUIT_MENU): title screen on, pause menu's Title Screen confirmation on; the game's own
  labels in 1 message archive(s) for the language pt-BR (106 ms)`: at start. A screen that is off says why in brackets;
  `quit_menu: off ...` says why nothing is on.
- `quit_menu: title: the fourth button's pane shown (flags ... -> ..., 2 above the button's own)`: the title's entry
  made visible (at most 4 lines).
- `quit_menu: pause: the Title Screen confirmation opens with three buttons (Title Screen, Cancel, Quit Game)`: each
  time the window opens so (at most 16 lines).
- `quit_menu: title: Quit Game chosen; closing the game` or `quit_menu: pause: Quit Game chosen; closing the game`: the
  choice. While a save is being written it says so, and `quit_menu: the save is written (N ms): closing the game`
  follows.
- `quit_menu: pause: the confirmation closed without Quit Game (Title Screen or Cancel)`: the window closed another
  way (at most 16 lines).

## Testing

`NX2SARIA_QUIT_MENU_TEST` takes words, for test runs only: `log` writes the state of the two screens' buttons each
time it changes (`quit_menu: title: state ...`, at most 120 lines); `pass` gives the A on a new button to the game
instead of closing; `notitle` and `nopause` turn one screen off. `tests/quit_menu_check.py` checks the message file
edit (with `--romfs`, on every message archive of a game folder), the labels and the press;
`tests/quit_menu_sites_check.py` the sites.
