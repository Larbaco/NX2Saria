# The mouse in the game's menus (1.1.1)

`runtime/menu_mouse.c` lets the mouse drive Ocarina of Time 1.1.1's own menus: hover moves the focus, left click is A,
right click is B, the wheel is the D-pad's up and down. Players' side: [controls.md](controls.md#mouse-in-the-games-menus).
This page records how it works and why it was built this way.

## Two ways to move the game's focus

- **Call the game's focus setter from the host.** `main+0x86ffc0(screen, button, flag)` is what the game's screens call
  (from `main+0x775090`). A host function would run it from a hook on a menu screen's update, with the guest registers
  saved around the call. It gives exactly the game's behaviour. The cost is a guest call from the runtime, at a point
  where the game does not call it, with its registers, stack and re-entrancy to look after. It also needs locks if the
  screens' updates ran on several threads.
- **Write what the setter writes, and let the game do the rest.** The setter only finds the entry and hands it to the
  focus manager's method `main+0x3774f70`, which writes four fields: the control's reserved entry (+0x80), the
  control's "no slide" bit (+0xb4 bit 3), the screen's requested entry (+0x188) and its request bit (+0x1ae bit 2). The
  controls' update (`main+0x768990`) takes that reservation in the same frame and makes the move (the old button's
  unfocus, the new one's highlight, the cursor's slide and sound). This is how `runtime/menu_cursor.c` drives 1.4.3's
  menus.

We use the second. No game code is called, so nothing in the game runs in an order it never ran in. The writes are the
ones the game's own method makes, so the move is the game's (highlight, slide, sound), and the hover keeps working the
way the D-pad does.

## Where it runs

The set `menu_mouse` (`recomp/patches-v111.txt`) adds one observer site at `main+0x2d13b0`, in every interface
screen's update. The screen manager (`main+0x3781e00`) calls that update for each screen, one after another, inside
the interface update (`main+0x3bc6d00`). That update first has the focus manager read the pad, then runs the screens,
then has the controls take a reserved focus. The host function (`menu_mouse_screen_host`) therefore runs on the
interface's own thread, before the reservation is taken, and changes no register. It acts only on the screen that has
the focus. That screen is found from the interface system `*(main+0x4e982e0)`, then its screen manager (+0x38), then
the focus manager (+0x60), then player 1's control (+0x28), then the focused entry (+0x78), then that entry's screen
(+0x20).

A, B and the wheel's up and down are pad presses. The interface thread counts them, and the pad read
(`menu_mouse_poll`, from `input.c` `input_poll`) adds them one at a time, each held 60 ms and then released for 60 ms,
so the game sees one press edge whatever its frame rate. A click on a button that does not have the focus yet reserves
the focus first and presses A once the control has it (given up after 300 ms).

## Hit test

A screen's entries are a list (head +0x88, first node +0x90, the entry at the node less the int32 at +0x9c). An entry
can take the focus as the game's entry test (`main+0x769d70`) decides:

- the screen is open (+0x1a6 = 2, +0x1a5 not negative, +0x1aa and +0x1ab bit 0 clear);
- the button is enabled (+0x40 bit 4);
- its root pane (+0x58) is shown and not transparent;
- every ancestor of the root pane is shown, and an ancestor whose alpha its children inherit is not transparent.

The box is the pane the game's focus frame takes (button +0x60): its size, the scales and translation of its global
matrix, and its base position (`runtime/menu_mouse_geom.h` `mm_pane_box`). It is in the layout's units: 3840 x 2160,
origin at the centre, y up. Where boxes overlap, the smallest wins.

## Ultrawide pictures

On a picture wider than 16:9 the runtime squeezes the first row of the interface's projection (set `wide_hud`), so a
layout is drawn in the picture's middle 16:9 and reaches ±1920 / squeeze units across the whole picture. The
`wide_menus` and `wide_hud` edits move and widen panes in the layout files, so their matrices already hold the new
places. Only the pointer needs the squeeze: the layout units the picture spans are read from each screen's own
projection (+0x1c0: 2 / 3840 times the squeeze across, 2 / 2160 down), so the screens the runtime keeps unsqueezed
(backgrounds, fades) and a layout of another size map right too. With `wide_menus = full` a button
moved out to the edge is hit where it is drawn; with `center` the pointer over the black sides finds no button.

## The device and the camera

The pointer shows only while keyboard and mouse are the device used last (`input_kbm_last`). A gamepad player sees the
menus unchanged and keeps the capture. While the pointer shows, `input_cursor_allowed` is false, so the capture is
released and the mouse camera gets no counts. The bound mouse buttons (Y, ZL) never reach the game, because
`poll_merged` drops mouse buttons while the cursor is not captured. When no menu has had the focus for 350 ms
(gameplay, a movie), the pad read takes the pointer away and the camera captures again. A focus still on a screen
that opens or closes counts as a menu for up to 1.5 s, so going from one menu to the next keeps the pointer; when it does go for a
moment, it comes back where the player left it instead of on the focused button. While the settings overlay or
a text entry is open, the host function does nothing: the overlay has the mouse.

## Log

- `menu_mouse: on for 1.1.1 (NX2SARIA_MENU_CURSOR 1; set menu_mouse on, the screens' site main+0x2d13b0)`: at the
  first pad read. It says `off` with the reason, or `missing from this build`.
- `menu_mouse: the screens' site runs (thread N); interface system found`: the first time the site runs.
- `menu_mouse: screen <name>: K of N entries take the focus, layout W x H units across the picture`: each menu screen the focus goes to (the
  first 24; all of them with `NX2SARIA_MENU_CURSOR_LOG=1`).
- `menu_mouse: <name>: focus moved H, left clicks C (A a, on no button m), right clicks (B) b, wheel notches w; hit
  test h on a button, o off`: when the focus leaves a menu the mouse was used in, for another screen or for none
  (at most 48 a session).
- `menu_mouse: no menu for 350 ms: the pointer hidden, the mouse back to the camera`: the menus closed while the
  pointer showed (the first 8 times).
- `menu_mouse: no menu for the mouse: <reason> (...)`: why no screen is for the mouse, each time the reason or the
  objects change (12 lines; 60 with `NX2SARIA_MENU_CURSOR_LOG=1`). In play it says `no focused entry`.

## Testing

`NX2SARIA_INPUT_SCRIPT` takes `POINTER x y`, `MOUSEL`/`MOUSER [hold]` and `WHEEL n` lines
([controls.md](controls.md#scripted-input)). They feed only this module, so a windowless run can point and click.
`tests/menu_mouse_check.py` checks the box and pointer arithmetic on 16:9, 21:9 and 32:9 pictures.

## Not done

- The game also has a touch path in its menus: `main+0x377b260` turns the interface pad's pointer into a hit test,
  and the game reads `nn::hid` mouse states at `main+0x3b28780`. It is not used. Whether that mouse reaches the
  interface pad's pointer is unknown, and touch gives no hover.
- Key Items' stones, the map's floors and the world map are not the framework's buttons, and the pause menu's tab
  bar has no entries. Hover and clicks do nothing there; L, R, the keys and the wheel still work.
