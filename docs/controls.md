<!-- languages -->
**English** · [Português (Brasil)](pt-BR/controls.md) · [Русский](ru/controls.md) · [简体中文](zh-Hans/controls.md)
<!-- /languages -->

# Controls

Input comes from SDL3 gamepads, the keyboard and the mouse, merged into one Switch controller (`runtime/input/input.c`,
`runtime/input/input_win.c`). Every connected gamepad is kept open and the game reads the one pressed last; a gamepad
switched on in play is found within a second. Buttons are ORed; each stick takes the source with the larger
deflection. Keys and mouse buttons count only while the game window is in the foreground. `NX2SARIA_INPUT=none`
disables both (automated runs). `NX2SARIA_*` settings can also be set in `nx2saria.ini` next to the exe (`input = none`,
`motion = 0`, `rumble = 0`); the environment overrides the file. The older `NX2NAVI_*` names and `nx2navi.ini` still work ([Names and compatibility](branding.md)). `NX2SARIA_RUMBLE_STRENGTH` is the rumble strength in
percent (0 to 300, default 130; 0 is the same as `rumble = 0`). `NX2SARIA_RUMBLE_CURVE` is the exponent applied to the
amplitude before the strength (0.2 to 1.5, default 0.6; lower makes weak pulses stronger, 1 is linear).
`NX2SARIA_RUMBLE_TRIGGERS=1` also drives the impulse triggers of an Xbox pad. `NX2SARIA_DS3=1` turns on the direct reader for a DualShock 3 (see PlayStation 3 controllers).

`NX2SARIA_PAD_LAYOUT` (`pad_layout`, the *Button layout* row on the Controls tab, live) picks which face button is Switch A:

- `nintendo` (default): by position, as on a Switch (the table below).
- `xbox`: south A, east B, west X, north Y, as an Xbox pad labels them; on a PlayStation pad cross is A, circle B, square X, triangle Y.
- `playstation`: the same buttons as `xbox` under the PlayStation name: cross A, circle B, square X, triangle Y.
- `auto`: `nintendo` for a pad whose east button SDL reports labelled A (Switch Pro Controller, Nintendo-layout pads), `xbox` for the others.

The log names the layout in effect (`input: gamepad layout ...`) and each gamepad's first press
(`input: gamepad "...": first press`); the menu shows the layout under *Automatic*. The game's prompts show the Switch
letters unless *Button prompts* says otherwise (below).

ZL and ZR count once the controller has reported its trigger at rest for a whole second, so for at least the first
second after a controller connects its triggers read released. A wireless receiver or a virtual pad that reports a trigger held
from the start would otherwise hold ZL or ZR down for the whole session. The log names each trigger as it starts to
count (`input: gamepad "...": the left trigger counts from now on, after 1.00 s at rest ...`) and, once a session, a
trigger that has read held for ten seconds without resting, with the value it reads
(`input: gamepad "...": the right trigger has read held for 10 s ...`).

The action prompt (top right) shows the four face buttons as a diamond. The game draws it in the Switch places (A right,
B bottom, X top, Y left), which `nintendo` keeps. Under `xbox`, `playstation` and `auto` the diamond follows such a
pad instead (A bottom, B right, X left, Y top), whatever *Button prompts* is.

The game reads the diamond once at start, before a gamepad is open:

- `auto` gets the Xbox diamond, so a Switch Pro Controller under `auto` sees A at the bottom although its A is on the
  right. Pick `nintendo` for that pad.
- A change of layout shows after a restart; until then *Button prompts* says *prompts change after restart*.
- A RomFS mod that changes the action prompt keeps its own diamond. A mod that replaces the same archive for other
  reasons gets the diamond moved on top of its changes.

The log says what happened in a line that starts with `action prompt:`. How the layout is changed:
`runtime/patches/action_prompt.c` (`DoCommand_00` in `UI/Layout2Archive/Common.Product.100.Nin_Ounce_NVN2.blarc.zs`, served
from `<profile>/action-prompt/`).

## Button prompts

`NX2SARIA_PROMPTS` (`prompts`, the *Button prompts* row on the Controls tab) picks the buttons the game's prompts show: `nintendo` (default, the game's own), `xbox`, `playstation`, `steamdeck` or `pc` (*Keyboard and mouse*). It changes three things the game draws:

- the button font every prompt in text uses (dialogue, the button guides, the controls screen): A B X Y, L R ZL ZR, Plus and Minus, the d-pad and the sticks with their directions become that pad's buttons (LB RB LT RT, Menu and View; L1 R1 L2 R2, Options and Create; its d-pad and sticks), or the keys. Power, Home and the console stay;
- the letter in the action prompt (top right);
- the ocarina's note buttons.

A, B, X and Y show the button the *Button layout* presses: with `nintendo` (by position) Switch A is the right face button, so its prompts show B or circle, B shows A or cross, X shows Y or triangle and Y shows X or square; with `xbox`, `playstation` and `automatic` (by label) A shows A or cross, B shows B or circle, X shows X or square and Y shows Y or triangle.

A button remapped to another pad button (a `bind_` line with a `pad:` entry, which the Controls tab writes) shows that pad button instead, in every pad style and layout: with A and B swapped, A's prompts show B's button and B's show A's. The ocarina's notes follow too when the new button is a face button, L or R. The back paddles have no picture, so a button on a paddle keeps the prompt of its own button.

`pc` shows the first key each Switch button has (the table above, or the `bind_` lines of `nx2saria.ini` when the tool runs): E, Ctrl, Space, the mouse with its left button for Y, Q, R, the mouse with its right button for ZL, F, Tab, M, V and C for the stick clicks, 1 to 4 for the d-pad, and W A S D for moving, the directions the prompt means set out as on the keyboard. The camera's prompts show the mouse moving. It follows no *Button layout*.

The game reads the font and the interface textures once at start, so a change shows after a restart (the row says so; the menu's Restart button restarts). A button or key remapped on this tab keeps its old prompt until the folders are made again (a rebuild of the game makes them again when the `bind_` lines changed). The log's `button prompts:` line says which remapped buttons the prompts follow.

The prompts live in overlay folders next to the exe, `romfs-patches-xbox`, `romfs-patches-xbox-label`, `romfs-patches-playstation`, `romfs-patches-playstation-label`, `romfs-patches-steamdeck` and `romfs-patches-steamdeck-label` (by position and by label) and `romfs-patches-pc`, which `runtime/hle/romfs.c` loads in place of the game's files. `tools/pad_prompts.py` draws them on the player's machine from the player's own RomFS and a folder of button art. The art is the repository's `prompt-art` folder: pictures of a CC0 pack, which its `SOURCE.md` names with where they came from. Every launcher release, AppImage and build kit carries it (`tools/prompt_art.py`), and `tools/make_game.py` draws every folder in its last step, again whenever the art, the tools or the `bind_` lines change (so a rebuild after rebinding keys brings the keyboard prompts up to date). By hand:

```
python tools\pad_prompts.py --art prompt-art --game <install folder>
```

`--style xbox|playstation|steamdeck|pc` makes one style; `steamdeck` shows the Deck's own labels (A B X Y where an Xbox pad has them, L1 R1 L2 R2 and L3 R3, Menu and View) and is drawn from the Xbox and PlayStation pictures, as the art has no Deck style; the keys come from `<install folder>\nx2saria.ini` (`--binds` names another). The art folder holds `XGamepad/Default`, `XGamepad/Light`, `P5Gamepad/Default`, `P5Gamepad/Light`, `Keyboard_Mouse/Dark` and `Keyboard_Mouse/White` with the file names of `STYLES` and `KEY_ART` in the tool; a key it has no picture of is written on `Keyboard_Mouse/Blanks`. Without the folders the row says *prompt files missing: update the game, or rebuild it in the launcher* (Rebuild the Game in the Windows launcher, Update in the Linux AppImage) and the Switch prompts stay; so they do when a RomFS mod replaces one of the same files (*off: a RomFS mod replaces their files*). The log has a `button prompts:` line at start, which says where the build drew the prompts from or why it drew none (the game folder's `button-prompts.txt`), and a `RomFS overlay: ... button prompts` line for the folder in use. The tool needs fontTools, Pillow and zstandard (`tools/requirements-py314.txt`).

The actions are the game's own, as its *Basic Controls* screen (Plus menu, System) names them:

| Switch | Gamepad (by position, Nintendo layout) | Keyboard and mouse | Action in the world |
|---|---|---|---|
| Left stick | left stick | W A S D | move |
| Right stick | right stick | mouse; arrow keys as a second source | camera |
| A | east | E, Left Shift | actions (talk, open, pick up, climb, grab); held, dash |
| B | south | Left Ctrl, Backspace | put away, cancel, back |
| X | north | Space | jump |
| Y | west | left mouse button | attack with the sword |
| ZL | left trigger (over 50%) | right mouse button, Z | target (hold or toggle, the game's *Targeting Type* option) |
| ZR | right trigger (over 50%) | F, X | use the selected item (slingshot, bow, boomerang, hookshot, bombs, bottles) |
| L | left shoulder | Q | quick-select items; previous page in the game's menus |
| R | right shoulder | R | quick-select items; next page in the game's menus |
| Left stick click | left stick click | V | (not on the game's controls screen) |
| Right stick click | right stick click | C, middle mouse button | reset the camera behind Link |
| D-pad left | D-pad left | 1, O | play the ocarina |
| D-pad up | D-pad up | 2 | put the accessory on or take it off |
| D-pad right | D-pad right | 3, N | talk to Navi |
| D-pad down | D-pad down | 4 | change boots |
| Plus | Start | Tab | menu: items, equipment, collection, system |
| Minus | Back | M | map |

The game's *Swap Jump Controls* option trades B and X (jump on B, put away on X); the bindings stay on the Switch buttons, so Space then puts away and Left Ctrl jumps. Left Shift is A, so holding it dashes; E presses A once for talking and opening. Esc is unbound in the game; it opens the settings menu and releases the mouse cursor (see Mouse camera). Z and X duplicate ZL and ZR on neighbouring keys. The mouse wheel is unbound in play; in the game's own menus it scrolls (see Mouse in the game's menus). The bindings are the tables of `input_map` in `runtime/input/input_map.c`: the keys (platform-neutral key ids) and one gamepad table per layout (gamepad inputs by position); `runtime/input/input_win.c` translates Win32 virtual keys and SDL buttons.

## Window keys

| Key | Gamepad | Action |
|---|---|---|
| F1 or Esc | Minus and Plus together | open or close the settings menu; the game gets no input while it is open |
| F8 | | bug capture: saves where you are, the camera, the picture, the settings, the log's end and your save into `logs\captures`, and a note on screen names it ([bug-capture.md](bug-capture.md)) |
| F10 | L3 + R3 (both stick clicks) | performance overlay: off, frame rate, full panel; saved to `nx2saria.ini` (with the menu open it is a change of the menu's draft, below) |
| Ctrl+F5 | | read `nx2saria.ini` again and apply the changes without a restart (*Reloading `nx2saria.ini`*, below) |
| F11 | | borderless fullscreen on or off, saved to `nx2saria.ini`; not with Ctrl, Shift or Alt held, which overlays and capture tools use; it wins over a fullscreen change still waiting in the menu's draft |

The window opens on the monitor `NX2SARIA_MONITOR` (`monitor` in `nx2saria.ini`) names, and F11 fullscreen covers the monitor the window is on. `nx2saria.exe --list-monitors` prints each monitor's index, name (on Windows its device name too), desktop rectangle, primary flag and DPI without game files; `nx2saria.exe --monitor <value>` sets `NX2SARIA_MONITOR` for one run and wins over the file. The value is `auto` (default), `primary`, an index from the list (counting from 1), or part of a monitor name (on Windows also a device name such as `display3`; on Linux the name carries the connector, such as `dp-2`); when nothing matches, the log lists the monitors and the window is placed as with `auto`. On Wayland a window cannot place itself: only fullscreen (`fullscreen = 1`) opens on the chosen monitor, and a windowed window opens where the compositor puts it.

The Minus+Plus combo never reaches the game. In the menu the arrow keys, the D-pad or the left stick move, Enter or A selects, Q and E or L and R switch tabs, Esc or B goes back (Switch button names, as in the table above). With the mouse, a click on a row selects it and a setting changes on the two framed arrows beside its value (held, an arrow repeats); a click anywhere else on the row changes nothing.

## Audio output

The sound plays on the system default device, and follows it when the Windows default changes. `NX2SARIA_AUDIO_DEVICE` (`audio_device` in `nx2saria.ini`, section `[audio]`) names another one: a number counting from 1 in the order `nx2saria.exe --list-audio-devices` prints, or part of the device's name, ignoring case. `--list-audio-devices` prints each playback device's index, name, channels and frequency without game files, marks the system default, and shows which device the setting picks; `nx2saria.exe --audio-device <value>` sets `NX2SARIA_AUDIO_DEVICE` for one run and wins over the file. When nothing matches or the device will not open, the log lists the playback devices and the game plays on the system default; a chosen device that disappears while the game runs (a virtual sink removed, a headset unplugged) also moves the sound to the system default. `audio_channels = auto` asks the chosen device how many channels it has.

## Applying changes in the menu

Changes made in the settings menu are a draft until they are applied; nothing is written to `nx2saria.ini` as a row changes (`runtime/ui/option_rows.cpp`).

- A live setting that is cheap to show takes effect at once so it can be judged: the volumes, sharpness, the HDR brightness and tone mapping rows, upscaler, anti-aliasing, aspect, low latency, text speed, the performance overlay, rumble, the sprint button, dead zones and the button bindings. The draft keeps the value it had and Discard puts it back.
- Every setting read at start (its row says *after restart*) and the frame limit, VSync, HDR, button layout, walk key, motion, fullscreen and window size wait in the draft instead, and so does each cheat of the *Cheats* tab (shown when an enabled mod brings cheats); the row shows the pending value and Apply is what changes the game. These are the `is_staged` cases in `option_rows.cpp`.
- Leaving the menu with changes (Esc, B, *Continue*, F1, or Minus and Plus together) opens *Apply your changes?*, a list of `setting: old -> new` with the restart-only ones marked. *Apply* writes the settings, saves `nx2saria.ini` and closes the menu; *Discard* puts the previews back and closes it; *Continue editing* (the default, and Esc or B) returns to the pages with the draft kept. Switching tabs does not ask. F1 and the pad combo only raise a request that the menu's own thread answers, so they never lose a draft.
- After *Apply*, two more boxes can follow, one after the other (each opens a frame after the one before closed). First *Keep these display settings?* when *Fullscreen*, the window's size (the *Resolution* row, or *Window size* on a game version without the widescreen sets), *VSync* or *HDR* (between off and the tone mapper, which makes the swapchain again in the other format) changed: they are the live settings that can leave the player without a usable picture. They take effect at once and are written to `nx2saria.ini` only after *Keep*; the rest of the draft is saved at Apply. The *Resolution* row sets the game resolution with the window's size, and the two go together: its game resolution is saved with *Keep* and put back with *Revert*, and then leaves the restart box below (`group_timed` in `option_rows.cpp`). A 15 second countdown (`plat_now_ns`) runs; *Revert* (the default, and Esc or B), the timeout, closing the menu in any way and quitting put the old values back unsaved, and a save made meanwhile (F10, closing the window) skips them (`cfg_hold`). F11 during the countdown is the player taking control: the box closes and everything applied, F11's fullscreen state included, is kept and saved. Then *Some changes take effect after a restart* when an applied setting is read at start (GPU, HDR to or from the game's own, low latency switched on in a run that started with it off, ...): it lists them with *Restart now* (same restart as the Restart button) and *Later* (the default, and Esc or B), which closes the menu with the values saved for the next start.
- *Restart* applies and saves the draft first, since the restart is what brings the restart-only changes in; its box says so. *Quit game* drops the draft (its box says so), and so does closing the window.
- A change set back to the value it began with leaves the draft. A key something else changes meanwhile (F11 on *Fullscreen*) leaves it too, with the outside value kept. Settings an environment variable fixes are greyed out and never enter it.
- A save made by anything else while the draft is open (F10 with the menu closed, F11) does not write a previewed value: the entry is held out of every save until Apply (`cfg_hold`, `cfg_revert` in `runtime/cfg/cfg.c`).
- Every step is in `logs/nx2saria.log`, so a run can be checked afterwards: `menu: draft +` and `menu: draft -` when a key enters or leaves the draft (preview or staged), `menu: box` for each box opened and the answer given (button, Esc or B, the countdown running out, F11), `menu: draft applied` / `discarded` with each value, `menu: display kept` / `reverted`, `cfg: wrote <path>: <keys>` for every save and `cfg: save leaves out <key>` for a held preview, and `cfg: reload: not run` when Ctrl+F5 waits for the menu.

## Quality presets

*Settings > Screen > Picture* starts with *Quality preset*, which sets the graphics options together: *Recommended (…)*, *Low*, *Medium*, *High* or *Ultra*.

- *High* is the game as shipped. *Low* and *Medium* draw less, for slower PCs and handhelds: a smaller 3D scene in a 1080-line picture; *Low* also has a plainer filter to enlarge the scene and, on an integrated GPU or a handheld only, less texture filtering, fewer grass draws and a 30 or 40 fps limit. On an NVIDIA RTX card they use DLSS, on a Radeon RX 9000 card on Windows FSR 4. *Ultra* draws more, and more on a larger monitor; it is never recommended, only chosen. No preset turns frame generation on.
- *Recommended* is chosen again at every start from the graphics card, the memory and the processor. On a PC that gets *High*, nothing changes. When yours gets *Low* or *Medium* and you never chose a preset, a notice says so once. To play the game as before, choose *High* in the *Quality preset* row (or write `preset = high` in `nx2saria.ini`).
- A preset you choose stays as long as the PC does; when it is not the recommended one, the row says *Recommended for this PC: …*. After a new graphics card, processor or more memory (a driver update does not count), the game uses *Recommended* until you choose again, and the row says *New hardware: the recommended preset is in use*. Choosing *Recommended* there (left on it) and applying keeps it, as choosing any other preset does.
- An option you change by hand keeps your value, and the preset shows as *… (custom)*. Choosing a preset while options it sets are changed by hand to other values first lists them: *Use the preset* gives them the preset's values, *Cancel* keeps everything as it was.
- The choice is part of the draft ([above](#applying-changes-in-the-menu)): *Apply* writes `preset = <tier>` to `nx2saria.ini` (no line for *Recommended*). The resolution, shadows and grass a preset changes take effect after a restart.
- *Scene resolution*, below *Upscaling* while neither DLSS nor FSR is on, sets the size the 3D scene is drawn at, from 33 % of the picture to 100 % (native). Lower is faster and softer, for slow PCs and handhelds; the HUD and menus stay sharp. It takes effect at once; a size above the one the game started with comes after a restart. In this version `render_scale` and `output_res` change nothing: this row and the preset are what lower the scene.
- *Dynamic resolution*, below *Scene resolution*, lowers the 3D scene's size while the graphics card cannot keep up with the *Frame rate limit* and raises it again when it can: a little sharpness in heavy scenes for a steadier frame rate, with the HUD and menus sharp. It aims 10 % under the limit's frame time (15 ms at 60 fps), so the scene shrinks before frames are lost. With DLSS or FSR it steps the upscaling mode down, as far as *Performance* (at DLAA or Native AA it has nothing to step); without them it lowers the scene from the *Scene resolution* you chose down to *Minimum scene resolution* (33 % to 100 %, default 50 %). That row is greyed out while *Dynamic resolution* is off and hidden while DLSS or FSR is on. Both take effect at once, and no preset turns it on. More in [Quality presets](presets.md#dynamic-resolution).
- `logs/nx2saria.log` says what was chosen and why, near its top: `preset: hardware: ...` (what the game found), `preset: recommended <tier>: <reason>`, `preset: in use <tier> (<source>)` and one `preset:   <KEY> = <value> (...)` line per option the preset sets, saying whether the preset, your settings file or the device profile gave it. Attach the log to a report about the picture or the frame rate.

How the presets are chosen and what each one sets: [Quality presets](presets.md).

## Reloading `nx2saria.ini`

Ctrl+F5 reads `nx2saria.ini` again while the game runs, so a line edited in a text editor takes effect without a restart or a trip through the menu. A plain F5 is not the reload: it stays a key that can be bound (Ctrl+F5 would also press an F5 binding, since the game reads the keyboard state; unbind F5 in the *Controls* tab if that gets in the way). The window code (Win32 and SDL alike) sees the Ctrl state with the key press, so the hotkey needs no change to the key bindings.

- Each setting whose line changed, was added or was removed (it then returns to its default) takes its new value; a short notice at the top of the window says *N changed, M after restart*, with *K rejected* appended when some value was refused. Settings the menu marks *after restart* change in memory and are listed in the log with `(takes effect after restart)`.
- A new value is read as at start (a comment after the value is cut, and `on`, `off`, `true`, `false`, `yes` and `no` count for a switch) and checked first (`value_ok` in `runtime/cfg/cfg.c`): with the check the start-up log uses for values outside their settings' choices (a switch, a range, a list of names; a list with a number in it is the menu's presets and is not checked), and with its own rule where the setting takes more than that table lists or stops the game on a bad value (`fps_limit`, `render_scale`, `window_size`, `output_res`, `app_pool`, and `lang` and `operation_mode`, which must match exactly). A value that does not fit is rejected, the current one stays, and the log says `cfg: reload KEY = 'x' rejected: invalid`. At start a mistyped `lang` or `operation_mode` stops the game with a message; a reload never does. Keys the menu does not list (bindings, mod lists, cheats) are not checked: their readers fall back to their defaults by themselves.
- Not reloaded, and each named in `logs/nx2saria.log` (`cfg: reload ...`): a setting the environment sets (`NX2SARIA_*`), and a setting changed in the menu whose write to the file is still pending or that the menu's draft holds. A setting the running game version ignores is stored and logged as `has no effect on this game version`.
- With the settings menu holding changes you have not applied (the draft, the *Keep these display settings?* countdown or the boxes after *Apply*), the reload does not run and the notice says *Close the settings menu to reload nx2saria.ini*.
- The reload never writes the file. In a run that replays a bug capture (`NX2SARIA_CAPTURE_REPLAY`), whose `settings.ini` stands in for `nx2saria.ini`, it does not run and the notice says the file could not be read.

## Remapping

The *Controls* tab of the settings menu lists, under the button layout, every Switch button and the eight stick directions the keys drive (movement and camera), each with two key cells and a controller cell. Each row's description says what the button does in the game. Select a cell, press A or Enter, then press the new key, mouse button or controller button; Esc or five seconds without a press cancel, and a controller button cancels a key cell. Delete, Backspace or X clears the selected cell. A key another action has moves to the new one (the description names the action it left); a controller button swaps with the one it replaces. A remapped controller button is physical: from then on the face buttons (A B X Y) stay where they are in both layouts, until the buttons are put back as the layout in use has them or *Reset all buttons* is chosen. F1, F8, F10, F11 and Esc cannot be bound (the menu, the bug capture and the window keep them). Punctuation keys are named by their place on a US keyboard; the menu shows the character the keyboard layout in use puts there.

The tab changes `nx2saria.ini` lines (they join the draft above and are saved on Apply), one per Switch button or stick direction: `bind_<button> = <entry>, <entry>, ...`. The button is `A B X Y L R ZL ZR PLUS MINUS LEFT UP RIGHT DOWN LCLICK RCLICK`, or a stick direction `LSTICK_LEFT LSTICK_UP LSTICK_RIGHT LSTICK_DOWN` (movement, W A S D by default) and `RSTICK_...` (camera, the arrow keys). An entry is `pad:<name>` or `key:<name>`:

- pad names: `south east west north l r zl zr start back lstick rstick dpad_left dpad_up dpad_right dpad_down lpaddle1 rpaddle1 lpaddle2 rpaddle2` (`zl`, `zr` are the triggers; the paddles are the back buttons of an Xbox Elite, DualSense Edge or Steam Controller, upper pair 1, lower pair 2, and no Switch button has them by default)
- key names: `a` to `z`, `0` to `9`, `f2` to `f7`, `f9`, `f12`, `num0` to `num9`, `space tab enter backspace lshift rshift lctrl rctrl lalt capslock insert delete home end pageup pagedown up down left right`, `num_add num_sub num_mul num_div num_decimal`, `minus equals lbracket rbracket semicolon apostrophe comma period slash backslash grave`, `mouse_l mouse_r mouse_m mouse_x1 mouse_x2`
- `key:default` and `pad:default` keep that part's defaults (`pad:default` is each layout's own button).
- `none` leaves the button unbound.

A line replaces every default of that button, pad and keyboard, so list the keys you still want or use `key:default`. Pad names are physical positions, so a line means the same in both layouts and replaces the layout's own button for that Switch button. Stick directions take keys only: the controller's sticks and the mouse camera always work. Buttons without a line keep the table above. Names are case-insensitive. An invalid entry logs one `input: bind ...` line and is skipped; a line without a valid entry changes nothing. The `NX2SARIA_BIND_<button>` environment variables work like any other setting. A changed line applies at once (`runtime/input/input.c` builds the tables again at its next poll).

A pad that reports its face buttons swapped (A/B and X/Y, as some GuliKit models do) is fixed with:

```
bind_A = pad:south, key:default
bind_B = pad:east, key:default
bind_X = pad:west, key:space
bind_Y = pad:north, key:mouse_l
```

## Mouse camera

Mouse motion (raw input) becomes right-stick deflection. Each presented frame takes the counts moved since the last frame, divides by the frame time and sets the deflection to `counts per second * NX2SARIA_MOUSE_SENS / 3000` of full, held for that frame, so the turn rate does not depend on the frame rate. This is not mouselook: the game's stick curve and dead zone still apply, so very slow movements do nothing and a fast flick saturates. With a camera speed other than 1 (`NX2SARIA_STICK_SENS`) the deflection is divided by that factor before it is limited to full tilt: the turn for a given movement stays the same, and the fastest turn a flick reaches is the game's top speed with the factor in it. The mouse deflection competes with the gamepad and arrow keys by the larger-deflection rule.

`NX2SARIA_MOUSE_MODE` (`mouse_mode`, the *Mouse camera* row on the Controls tab, live) chooses that right-stick deflection (`stick`) or mouselook (`direct`, the default). In `direct` the camera's input update (`main+0x37c300`) takes the raw counts itself, after the game's stick dead zone, clamp and response curve (`recomp/patches-v111.txt`, set `mouse_direct`; `runtime/input/mouse_direct.c` `camera_input_oot`). The camera turns by how far the mouse moved, the same on both axes: about 0.08 degrees a count at `NX2SARIA_MOUSE_SENS` 1, at any frame rate and any `NX2SARIA_STICK_SENS`, with no dead zone and no top speed. The game's own camera sensitivity (its Options) scales the mouse as it scales the stick. A gamepad's right stick and the arrow keys still go through the game's response and add to the mouse. While the game takes no camera input (it switched camera input off, or the pause after a camera reset runs) the mouse is the right stick as above. `NX2SARIA_MOUSE_DIRECT_PATCH=0` turns the patch set off, and the mouse is the right stick whatever the mode. `NX2SARIA_MOUSE_TRACE=1` logs each camera input update (the stick, the counts taken and what they added) and, with `dlaa` on (DLAA, DLSS or FSR 3.1, which read the game's camera), the camera's direction in each frame, for measuring the turn per count.

`NX2SARIA_MOUSE_SENS` (the *Mouse camera sensitivity* row, live; 0 to 10, default 1; higher turns faster, 0 turns the mouse camera and the cursor capture off, the row goes down to 0.05). While the window is the foreground window the cursor is hidden and confined to the client area. It is released on focus loss, minimise, while F11 changes the window, during a size or move loop and while a text entry is open. Esc releases it by hand: the cursor shows, the mouse camera and mouse buttons stop, and the window can be moved, minimised or closed. A mouse click inside the client area captures it again; that click does not reach the game. Activating the window by a click on its title bar or border does not capture, so it can be dragged or closed right away; Alt+Tab or a click in the client area captures as before. `NX2SARIA_INPUT=none` disables the capture too.

`NX2SARIA_STICK_DEADZONE` (0 to 50, default 8) is the gamepad stick dead zone in percent of full deflection, the same for both sticks. It is round: a stick inside the circle reads centred, and the rest of its travel is scaled to start at 0 at the edge, so full deflection still reaches full. Raise it for a stick that drifts. The game's own dead zone applies after it. `NX2SARIA_LSTICK_DEADZONE` and `NX2SARIA_RSTICK_DEADZONE` (the *Left/Right stick dead zone* rows under *Analog sticks* on the Controls tab) set one stick each and fall back to `NX2SARIA_STICK_DEADZONE` while unset, so a worn stick can get a larger zone than the other. With a gamepad connected each row shows its stick's deflection now, before the dead zone (*stick now*): let go of the stick and set the row a few percent above the value it rests at.

The stick direction bits (`StickLLeft` to `StickRDown`) are derived from the sticks at 50% deflection. With no gamepad connected the log says `input: no gamepad`.

Keys tilt a stick all the way, so on the keyboard alone Link always runs. `NX2SARIA_WALK_KEY` (`walk_key`, the *Walk key* row under *Analog sticks* on the Controls tab) names a key that, while held, scales the stick values the keys give (movement and camera keys) to `NX2SARIA_WALK_SCALE` of full (`walk_scale`, the *Walk scale* row; 0.2 to 0.9, default 0.51), so Link walks. The value is a key name of the `bind_` lines, or `none` (the default: off). The row offers Shift, Alt, Ctrl, Tab and Caps Lock and names the button a chosen key also presses: with the default bindings Shift is A, Ctrl is B and Tab is Plus, while Alt and Caps Lock are free. The gamepad's sticks and the mouse camera are not scaled. The default is just above half because half of a key's full value falls one short of the 50% the direction bits need. Both settings apply at once while the game runs.

## Mouse in the game's menus

On game 1.1.1 the mouse works in the game's own menus: the title and its save files, the pause menu and its pages, Options and the save dialogs. While a menu has the focus and the mouse or a key was used last, the cursor is released from the mouse camera and shown on the button that has the focus. Moving the mouse onto a button selects it, as the D-pad would (the game's highlight and sound); a left click on a button is A on it, a right click anywhere is B, and each notch of the wheel is the D-pad's up or down, so a list moves a row. On the pause menu a click on a page's name at the top turns to that page, and the wheel over the names turns pages. Picking up the gamepad hides the cursor and leaves the menus as on the console; the camera takes the mouse back when the menu closes. The settings menu (F1 or Esc) has the mouse first while it is open. The pointer follows the menus where `wide_menus` and `wide_hud` put them on an ultrawide picture.

`NX2SARIA_MENU_CURSOR` (`menu_cursor`, the *Mouse in the game's menus* row on the Controls tab, live; default 1) turns it off with 0. `NX2SARIA_MENU_CURSOR_LOG=1` logs every menu screen the focus goes to. The log has `menu_mouse: on for 1.1.1 ...` at start, a `menu_mouse: screen <name>: ...` line for the first menus opened, and a line per menu the mouse was used in, with the focus moves, clicks, wheel notches and hit-test counts. How it works: [menu-mouse.md](menu-mouse.md).

## Quitting the game

On game 1.1.1 the game's own menus have a way out, for when only the gamepad is in hand. The entry is in the game's language, with the game's own words for it (*Quit Game* in English).

- **Title screen:** the list has a fourth entry under the game's three. A on it closes the game.
- **Pause menu:** on the System page, *Title Screen* opens the game's question, which has a third button: *Title Screen*, *Cancel*, *Quit Game*. The focus starts on *Cancel*. A on *Quit Game* closes the game; what was not saved is lost, as the question says.

The keyboard and the mouse work there as in the game's other menus. The game also closes from the settings menu (F1 or Esc, *Quit game*), by closing the window, and with Alt+F4. While the game is writing a save, the close waits for it.

`NX2SARIA_QUIT_MENU` (`quit_menu` in `nx2saria.ini`; default 1, read at start) turns both off with 0: the game's menus are then as on the console. The log has `quit_menu: on ...` at start, with each screen on or off, and `quit_menu: title: Quit Game chosen; closing the game` or `quit_menu: pause: ...` when it is used. How it works: [quit-menu.md](quit-menu.md).

## HD rumble on Switch controllers

The game makes its vibration for linear actuators: per side a low and a high band, each with a frequency and an amplitude. On most gamepads that is reduced to two motor strengths. With `rumble_hd = 1` in `nx2saria.ini` (`NX2SARIA_RUMBLE_HD`, the *HD vibration (Switch)* row on the Controls tab, live, off by default) a Switch controller gets the values as the game makes them, in the controller's own rumble report (`runtime/input/rumble_hd.c`, `runtime/input/input.c` "HD rumble"):

| Gamepad the game reads | Reports go | Connection |
|---|---|---|
| Switch Pro Controller (`057e:2009`) | through SDL (`SDL_SendGamepadEffect`) | USB and Bluetooth |
| One Joy-Con | through SDL; the stronger side's value, as it has one actuator | Bluetooth, or the USB charging grip |
| A left and a right Joy-Con as one gamepad | to the two Joy-Con's HID devices directly (`runtime/input/input_hd_pair.c`), as SDL takes no rumble data for the combined gamepad | Bluetooth |
| Anything else, the Switch 2 controllers included | the motors, as with `rumble_hd = 0` | |

The log says which it is when a gamepad is taken: `input: HD rumble available for this gamepad: a Pro Controller, through SDL (rumble_hd = 1)`, or `not available` with the reason; the row on the Controls tab is greyed out for a gamepad that cannot take it. With `rumble_hd = 0` nothing is written to the controller and the line says that its driver is asked once the setting is on. The first report a gamepad gets is logged with its bytes (`input: HD rumble: first report written (left ..., right ...)`); a report the controller or its driver refuses is logged too, and the gamepad then rumbles through its motors again. Frequencies are clamped to what the actuators take (41 to 626 Hz in the low band, 82 to 1253 Hz in the high band) and amplitudes to 0..1, the actuators' safe maximum. `rumble_strength` scales the amplitude (its default, 130, is the game's own: the default is above 100 for the motors' sake, and this path divides by it), `rumble_curve` does not apply, and `rumble = 0` silences this path too. A report is written at most every 15 ms (`rumble_hd_ms`, 5 to 50: raise it if a Bluetooth controller stutters or disconnects with HD rumble on), with the newest value only; a value that holds is written again every 30 ms, and the neutral value goes out when the game stops sending for 250 ms, when another gamepad takes over, when the setting is turned off and when the game exits. While HD rumble is on the game makes no dual-motor rumble call for that gamepad. A DualShock 3 on the direct reader keeps its motors either way.

## Sprint button

On 1.1.1, `NX2SARIA_SPRINT_BUTTON` (`sprint_button`, the *Sprint button* row on the Controls tab, live) gives a button that makes Link sprint without rolling first: `l3` (the left stick's click, whose only other use is hiding the text box in a conversation), `b` (the game's B button, the bottom face button of the Nintendo layout), or `off` (the default). `NX2SARIA_SPRINT_MODE` (`sprint_mode`, the *Sprint mode* row) is `toggle` (the default: a press starts the sprint, the next press ends it) or `hold` (Link sprints while the button is held). Link sprints while his input flags hold the dash input; the set `sprint` of `recomp/patches-v111.txt` calls `runtime/patches/sprint.c` after Link's movement update and the stance update that follows load those flags, and the host functions add the dash input while the sprint is on, so the game's own sprint runs with its speed, animation and camera. A toggled sprint goes on through jumps, slides, rolls and landings and ends when Link's state is anything else (an attack, an item, swimming, climbing, a cutscene), when ZL is held, or after four 60 Hz frames standing still (ninety after a press made while standing, before Link has moved), at any frame rate. During play (Link's update ran within the last eight controller polls and no event, such as a conversation, runs) the button is kept from the game, so it does not also fire its own action; a B press is kept only when it begins while Link moves, and then until it is released, so B puts the weapon away as usual otherwise and the game never gets half a press. In the settings menu, a menu of the game, a conversation or a cutscene every button keeps its role (L3 hides the text box), and a press made there starts no sprint (B closes the pause menu). With the game's Swap Jump Controls on, B is the jump, which a `b` sprint button keeps from the game while Link moves: use `l3` then. The set's host functions are on the patch page at +0xf40 and +0xf30. The set runs only while a button is chosen, so with the sprint button off its sites cost nothing. `NX2SARIA_SPRINT_BUTTON_PATCH=0` (or a mod whose code patches the same sites) keeps the patch set off and hides the rows.

## Gyro

The game reads the controller's gyroscope and accelerometer as the Switch's six-axis sensor and aims the slingshot, the bow, the hookshot and the boomerang with it. Any gamepad SDL gives a gyro and an accelerometer works: a Switch Pro Controller, a Joy-Con pair (the right Joy-Con's sensors), a DualSense or a DualShock 4. The *Gyro* group on the Controls tab holds its settings, all live (`runtime/input/input.c` `motion_options_refresh`, `runtime/input/motion_fusion.c`):

| Setting | Row | Values |
|---|---|---|
| `motion` | *Motion controls* | `1` (default) or `0`: the sensor reads a controller lying still |
| `gyro_calibration` | *Gyro calibration* | `1` (default): the gyro's reading at rest, its bias, is learned whenever the controller lies still for a moment and subtracted, so the aim does not drift; `0` passes the readings as the controller sends them |
| `gyro_deadzone` | *Gyro dead zone* | turns slower than the level count as none: `off`, `low` (default, 0.002 revolutions a second, 0.72°/s), `standard` (0.007) or `high` (0.01), the console's three zero-drift levels |
| `gyro_turn_sens`, `gyro_turn_invert` | *Turn sensitivity*, *Invert turn* | percent (10 to 400, default 100) of the turn about the world's up, that is left and right aim; `1` inverts it |
| `gyro_tilt_sens`, `gyro_tilt_invert` | *Tilt sensitivity*, *Invert tilt* | percent (10 to 400, default 100) of the rest of the rotation, up and down aim; `1` inverts it |

Set the controller down for a second after connecting it, on a table, say: the log then says `motion: bias learned +0.31/-0.12/+0.05 deg/s after 1500 ms still ("Nintendo Switch Pro Controller", calibration 1)`, and the bias is learned again each time the controller lies still. Each controller keeps its own. If nothing has been learned after 30 s of play the log says `motion: no still moment yet on "...": set the controller down for a second so its gyro bias is learned`. How it works and what each log line means: [Gyro](gyro.md).

## Nintendo Switch 2 controllers

Windows does not present the Switch 2 Pro Controller as a gamepad SDL can open (the log says `input: no gamepad`), so it needs a bridge program that wakes it and creates a virtual gamepad, for example one that emulates a DualSense: that gives buttons, rumble, gyro and accelerometer. The log then shows `input: gamepad "DualSense Wireless Controller" ... (rumble, gyro and accelerometer, ...)` and `input: motion sensors on`. Bridges that output a DualShock 4 whose gyro and accelerometer never change give buttons and rumble but no motion. Run one bridge at a time; Steam Input can also claim the controller, so close Steam if the bridge does not find it. With several virtual and real gamepads connected the game follows whichever one is pressed.

## PlayStation 3 controllers

With `ds3 = 1` in `nx2saria.ini` (off by default), a DualShock 3 on Windows is read directly (`runtime/input/input_ds3_win.c`) when no driver presents it to SDL or XInput as a gamepad. Plug it in by USB, or pair it with a Bluetooth driver that exposes it as a HID device with its own name, "PLAYSTATION(R)3 Controller"; the log then shows `input: DualShock 3 opened`, the controller's first player light turns on and `input: DualShock 3 is reporting` follows. The reader sends the controller the command that makes it start reporting, which it does not do on its own. It looks for a controller every three seconds while none is open, so one plugged in or switched on later is found without a restart.

| Switch | DualShock 3 by position |
|---|---|
| A, B, X, Y | circle, cross, triangle, square (east, south, north, west: the button layout setting applies as for any pad) |
| ZL, ZR | L2, R2 (over 50% pressure) |
| L, R | L1, R1 |
| Left, right stick click | L3, R3 |
| Plus, Minus | Start, Select |
| D-pad | D-pad |

The PS button is not used. The Controls tab binds and remaps its buttons like any other pad's, with the same pad names. Vibration works through the strength, curve and *Vibration* settings: the controller's light motor can only be on or off, so it comes on from a sixteenth of the amplitude the game asks of it. The accelerometer and the single gyro axis are not used: the game's motion controls need all three gyro axes, so a DualShock 3 gives no motion. `ds3 = 0` in `nx2saria.ini` (the default, live) turns the reader off. A controller that a driver presents in a generic gamepad mode, with another name or report layout, is not recognised by the reader and goes through SDL or XInput instead, as before.

## Scripted input

`NX2SARIA_INPUT_SCRIPT=<file>` replays presses. It is ORed with real input and still applies under `NX2SARIA_INPUT=none`. One event per line, `#` starts a comment:

```
<frame> <button> [hold_frames]            # A B X Y L R ZL ZR PLUS MINUS LEFT UP RIGHT DOWN LCLICK RCLICK; hold defaults to 4
<frame> LSTICK|RSTICK <x> <y> [hold_frames]   # -32767..32767, up is +y
<frame> POINTER <x> <y>                    # the pointer for the game's menus at x, y of the window (0..1 from top left)
<frame> MOUSEL|MOUSER [hold_frames]        # the left or right mouse button, for the game's menus only
<frame> WHEEL <notches>                    # the mouse wheel for the game's menus, up positive
```

`<frame>` counts 60 Hz ticks of game time since the first frame (presented frames at 60 fps). Each event logs once (`input: script frame N tick T: A`).

`NX2SARIA_INPUT_RECORD=<file>` (`input_record` in `nx2saria.ini`) writes the merged input in the same format, one line per button press and per stick segment. The file replays as a script, which makes it the repro to attach to a bug report.

`tools/run_game.py <game folder> --save <save> --seconds <s> --wall <w> [--script <file>] [--shot <png>]` runs the game on a copy of its profile with a save swapped in, under the script (default `tools/input/load_save.txt`: title screen to the first save slot). `<save>` names one of the game's save backups (`profile/save-backup/<time>`; the game keeps the last three) or is a save folder or `.zip` from elsewhere, such as the zipped profile folder of a crash report; a single slot (`slot_NN`) replaces only that slot. The run ends once `<s>` seconds of game time passed after the save loaded, `--shot` saving that frame as a PNG and `--env NX2SARIA_SHOTS=w<from>-<to>:<step>` one every `<step>` seconds of game time along the way (480x270, full size with `NX2SARIA_SHOTS_FULL=1`), or after `<w>` seconds of wall time. `--env NX2SARIA_SHOT_DIR=<dir> --env NX2SARIA_SHOT_AFTER=<s> --env NX2SARIA_SHOT_EVERY=<s>` takes pictures by wall time instead, for a script that never reaches a place where game time counts. On Windows it also takes `build\<ver>` folders and the saves in `build\save-backup`.

`NX2SARIA_TEST_PAD=<lx>,<ly>,<rx>,<ry>` attaches an SDL virtual gamepad at start whose sticks rest at these raw SDL axis values (-32768 to 32767, down is +y), so a drifting or held stick can be tested without a controller: the values go through the gamepad mapping and `NX2SARIA_STICK_DEADZONE` like a real pad's, and `NX2SARIA_INPUT_RECORD` shows what reached the game. `NX2SARIA_TEST_PAD_AT=<ms>` attaches it that long after input starts instead and presses its south button a second later, which tests a controller switched on in play.

## RenderDoc capture (blocked)

`NX2SARIA_RDOC=<frame>[,...]` (`runtime/diag/rdoc.c`; `NX2SARIA_RDOC_DIR`, `NX2SARIA_RDOC_DLL`) loads RenderDoc before `vkCreateInstance`. RenderDoc hides `VK_EXT_external_memory_host`, which the back end requires for guest pool imports (`runtime/vk/vk_memory.c`), so device creation fails under it. A capture needs a copy-based pool fallback first.
