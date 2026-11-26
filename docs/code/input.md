# runtime/input

The host's input: the gamepads (SDL3, and on Windows XInput and a DualShock 3 read directly), the keyboard and the
mouse of the window system's back end (Win32 or SDL), merged into one Switch npad for `nn::hid`
(`runtime/hle/ov_input.c`); the binding tables and their remapping; rumble, with the game's own values for a Switch
controller's linear actuators (HD rumble); the six-axis sensor built from a gamepad's gyro and accelerometer; the mouse
camera, as a right-stick deflection or fed into the game's camera input update directly; the stick's camera speed; and
the player's model ownership that selective motion vectors read. What these are for and how a player uses them:
[controls.md](../controls.md) ([Remapping](../controls.md#remapping), [Button prompts](../controls.md#button-prompts),
[Mouse camera](../controls.md#mouse-camera), [HD rumble on Switch controllers](../controls.md#hd-rumble-on-switch-controllers),
[Gyro](../controls.md#gyro), [PlayStation 3 controllers](../controls.md#playstation-3-controllers),
[Scripted input](../controls.md#scripted-input)), [gyro.md](../gyro.md) for the sensor fusion, its defaults and its
checks, [menu-mouse.md](../menu-mouse.md) for the mouse in the game's menus, and the bullet "Window, keyboard and
mouse" under the host in [architecture.md](../architecture.md). The log lines: `input:`, `hid:`, `motion:`, `mouse:`,
`mousetrace:`, `camera speed:`, `motion direct:` and `motion ownership:` in [logging.md](../logging.md). The checks
that drive this code: [building.md](../building.md#checks).

## `runtime/input/input.h`

The host input that `nn::hid` samples (`runtime/input/input.c`, with `runtime/input/input_win.c` or
`runtime/input/input_sdl.c`): an SDL3 gamepad and the keyboard as one Switch npad. It also declares the seams other
folders call: the menus' mouse, the text entry's block, the menu's gamepad, the rumble, the motion sensor and the sensor
fusion of `runtime/input/motion_fusion.c`.

### `input_poll()`

The bits of HidNpadButton: A=0 B=1 X=2 Y=3 StickL=4 StickR=5 L=6 R=7 ZL=8 ZR=9 Plus=10 Minus=11 Left=12 Up=13
Right=14 Down=15, and the stick directions (StickLLeft..Down 16-19, StickRLeft..Down 20-23, derived from the sticks).
Sticks are -32767..32767, up is +y. `NX2SARIA_INPUT=none` reports no input. Callable from any thread.

### `input_sdl_version()`

The SDL the process loaded (`SDL_GetVersion`) and, into `*built`, the one its headers were (`SDL_VERSION`), both as
`SDL_VERSIONNUM` values: input is the SDL part both platforms share (`runtime/main.c` `log_libs()`).

### `input_kbm_last()`

1 when keyboard and mouse are the device the player used last, 0 when the gamepad is: what is shown for pointing
(`runtime/ui/menu_cursor.c` shows the pointer in the game's menus only for the keyboard and mouse).

### `input_direct_mouse_active()`, `input_direct_mouse_enabled()`, `input_direct_mouse_available()`

Direct mouse input is consumed by the game's camera-input update, after its stick response. While that update accepts
camera input, the HID right stick holds only the controller and the keyboard. Selectors route the mouse separately: a
persistent radial cursor for the ability wheel, discrete steps for the horizontal quick lists.
`input_direct_mouse_available()`: the game version has the patch set and it is on, so the mode can be chosen.

`INPUT_OOT_COUNT_X` and `INPUT_OOT_COUNT_Y`: Ocarina 1.1.1's camera input update (`runtime/input/mouse_direct.c`
`camera_input_oot()`; `recomp/patches-v111.txt`). Measured with a scripted stick, its processed stick at one turns the
camera 1 degree up or down and 1.78 degrees sideways (StickSensitivityLat2LngRatio) per 60 Hz frame; the game's own
camera sensitivity option scales both. The two are what one mouse count adds to the processed stick per frame at
sensitivity 1, so a count is 0.08 degrees on either axis: 0.08 / 1.78 and 0.08.

### `input_direct_mouse_radial()`, `input_direct_mouse_row()`, `input_direct_mouse_sample()`

A radial selector uses a persistent virtual cursor while its opening button is held. Horizontal quick selectors get
short, separated steps (`InputMouseRow`) instead of a latched radial-cursor direction. `input_direct_mouse_sample()`
consumes the direct mouse counts, including the one-shot `MOUSE` events of an input script; `enabled` 0 discards the
scripted counts and leaves the real mouse for menu input. Live counts need the cursor captured and the window in front.

### `menu_cursor_poll()`, `menu_cursor_pointer()`, `menu_mouse_poll()`, `menu_mouse_pointer()`

`runtime/ui/menu_cursor.c`: the pointer in the game's menus (1.4.3). The poll runs once per pad read and adds a click's
A or B; the pointer call says a menu has the pointer, so the mouse camera's capture is released.
`runtime/ui/menu_mouse.c`: the same for 1.1.1's menus (the poll adds the A, B, up and down the mouse asked for).
`runtime/ui/quit_menu.c` `quit_menu_poll()` runs after them in `input_poll()`: it takes A away while the focus is on a
Quit Game entry of the game's menus, a click's A included.
`runtime/ui/pause_tab.c` `pause_tab_poll()` runs last: it takes R away when R opens the pause menu's settings tab.

### `input_game_input_blocked()`

The game gets no input now: a text entry or the in-game menu (the overlay) is open, or one just closed.

### `input_menu_pointer()`, `input_menu_mouse_presses()`, `input_menu_wheel_take()`

The mouse for the game's own menus (`runtime/ui/menu_mouse.c`), from the window or from `NX2SARIA_INPUT_SCRIPT`'s
`POINTER`, `MOUSEL`, `MOUSER` and `WHEEL` lines. The pointer is in fractions of the window, 0..1 from its top left; it
returns 0 when the window is not in front, the pointer is outside it or the camera has it captured, and 2 when the script
placed it. The presses are the left (0) or right (1) button's presses so far, counted at the event, so a click shorter
than a frame counts. The wheel call returns the notches since the last call, up positive. Any thread.

### `input_poll_ui()`, `input_block()`, `input_menu()`

`input_poll_ui()` is the same as `input_poll()` for host UI (the text entry): never blocked. `input_block(1)` while a
text entry is open: `input_poll()` then reports nothing; `input_block(0)` when it closes: nothing until all buttons and
stick directions are released. `input_menu()` is the same for the in-game menu (`runtime/ui/menu.h`): 1 when it opens
(`input_poll()` reports nothing, the cursor is released), 0 when it closes (nothing until every button and stick
direction is released). It is independent of `input_block()`.

### `input_menu_gate()`, `input_overlay_gate()`, `input_pad_read()`

`input_pad_read()`: the gamepad alone, for the menu: Switch button bits and the left stick (up +y), 0 without a
connected gamepad. `input_menu_gate()`: the Minus+Plus combo and the neutral pad while the menu is open
(`runtime/input/input_map.c`, free of the window system). `*held` is the caller's memory of the combo, the sticks are
lx, ly, rx, ry; it returns 1 in the poll where the combo goes down (toggle the menu once). `input_overlay_gate()`: L3+R3,
1 in the poll both go down.

### `input_pad_layout()`, `input_pad_stick_percent()`, `input_pad_label()`

`input_pad_layout()`: the gamepad layout in effect (`PAD_LAYOUT_*` of `runtime/input/input_map.h`;
`NX2SARIA_PAD_LAYOUT`, live), -1 without a gamepad. `input_pad_stick_percent()`: how far the open gamepad's left
(`pct[0]`) and right (`pct[1]`) sticks are from the centre now, in percent of full deflection, before the dead zone: the
menu shows it on the dead zone rows, so a drifting stick's resting value can be read off. 0 (and both 0) without a
connected gamepad. `input_pad_label()`: the label on a face button of the open gamepad (`PAD_SOUTH`..`PAD_NORTH`, as SDL
reports it); `PAD_LABEL_NONE` for another button, an unknown label or no gamepad. All three from any thread.

### `input_map_snapshot()`, `input_capture()`

The menu's remapping rows (Controls tab). `input_map_snapshot()` copies the tables in effect (after any changed
`NX2SARIA_BIND_*` line). `input_capture()` polls the keys, the mouse buttons and the gamepad for a press: `reset` 1
takes what is held now as the start (a held key counts once released and pressed again) and returns `NONE`; after that
it returns `KEY` or `PAD` with `*id` the key or pad id (`runtime/input/input_map.h`) pressed since the last call, or
`CANCEL` for Esc. Keys count only while the game window is in front. `struct InputMap` is only declared here, without
including `input_map.h`: `runtime/hle/swkbd_win.c` has button names of its own.

### `input_key_char()`, `input_flight_key()`, `input_dump_key()`

`input_key_char()`: the character on a punctuation key (`KEY_MINUS`..`KEY_GRAVE`) in the keyboard layout in use, 0 for
another key. `input_flight_key()`: F8, the bug capture's key (`runtime/diag/bugcap.c`; it has no `KEY_` id), held while
the game window is in front. `input_dump_key()`: Insert, the frame dump's key (`runtime/vk/vk_dump.c`,
`NX2SARIA_DUMP=key`), held; on Windows also while another window is in front, as a player on a gamepad often leaves the
focus elsewhere. Unbound in the game's default layout.

### `input_cursor_suspend()`, `input_cursor_released()`

The mouse camera and the cursor (`runtime/input/input_win.c`, the Win32 window thread). The window procedure calls
`input_window_msg()` first, for raw mouse input, cursor hiding and confinement; it returns 1 with `*res` set when the
message is handled. `input_cursor_suspend(1)` releases the cursor around an F11 window change, `(0)` captures it again
once the client area has its new size. `input_cursor_released()`: Esc or a non-client activation released the cursor
(the probe reads it).

### `input_rumble()`, `rumble_motor()`, `input_rumble_triggers_enabled()`

`input_rumble()`: `SDL_RumbleGamepad` on the open gamepad, the low (heavy, left) and high (light, right) motor 0..65535
for `ms` milliseconds; 1 if a gamepad with rumble took it. Any thread. `rumble_motor()`: the motor value for a
normalised amplitude, pow(a, curve) * strength, saturated to 0..65535; pure. `input_rumble_triggers_enabled()`:
`NX2SARIA_RUMBLE_TRIGGERS=1`, `input_rumble()` also drives the impulse triggers (Xbox) with the high motor's value.

### `input_rumble_hd_available()`, `input_rumble_hd_on()`, `input_rumble_hd()`, `input_rumble_hd_stop()`

HD rumble (`runtime/input/input.c`, [HD rumble](#hd-rumble), and `runtime/input/rumble_hd.h`): the game's vibration
values as they are, for a gamepad with linear actuators (a Switch Pro Controller, a Joy-Con or a pair of them). All from
any thread.

- `input_rumble_hd_available()`: the gamepad the game reads is one of these (the menu's row).
- `input_rumble_hd_on()`: ... and `NX2SARIA_RUMBLE_HD` is 1 (live) and `NX2SARIA_RUMBLE` is not 0: `input_rumble()`
  then leaves that gamepad's motors alone, and the caller posts every new value with `input_rumble_hd()`.
- `input_rumble_hd()`: the game's newest value per side (`v[0]` left, `v[1]` right; `live[s]` 0: that side is at
  rest), the amplitudes scaled by `strength` (1 = as they are). It returns at once: a thread of `input.c` writes the
  reports, the newest value only, and the neutral value once the game stops posting.
- `input_rumble_hd_stop()`: before the process ends, the neutral value for a controller that is still vibrating.

### `input_motion_start()`, `input_motion_enabled()`, `input_motion_axes()`

Motion (`NX2SARIA_MOTION=0` turns it off, live). `input_motion_start()` starts the sensor thread once: every 4 ms it
reads the gamepad's gyro and accelerometer (when it has them) and feeds `motion_global()`; any thread.
`input_motion_enabled()`: `NX2SARIA_MOTION` is not "0", a live setting read again when `cfg_version` changes; off, the
sensor reads at rest. `input_motion_axes()`: SDL's sensor values (accelerometer m/s^2, gyro rad/s, SDL's gamepad axes)
to the Switch controller frame `motion_feed()` takes (acceleration in G as gravity reads, rotation in rad/s); the mapping
is under `input_motion_axes()` in the `input.c` section below.

### `motion_new()`, `motion_global()`, `motion_feed()`, `motion_read()`, `motion_restart()`, `motion_set_options()`, `motion_source()`

`runtime/input/motion_fusion.c`: `nn::hid::SixAxisSensorState` and the sensor fusion that produces it. `MotionState`
is that state, 0x60 bytes (a static assert holds the ABI): `acc` in G, `gyro` in revolutions a second, `angle` in
revolutions, `dir` the controller's axes in the reference frame; `attributes` bit 0 is "connected".

- `motion_new()`: an instance with `MOTION_OPTIONS_DEFAULT` and no sensor source named yet. `motion_global()`: the
  gamepad's, fed by `input.c`'s motion thread.
- `motion_read()`: newest first; 0 when there was no sample in 200 ms.
- `motion_restart()`: the angle and the direction start over at the next sample; the learned bias stays.
- `motion_source()`: the controller whose sensors feed the instance from now on (`id` nonzero and unique per
  connection, SDL's joystick id for one; `name` for the log). A different one restarts the orientation and its rest
  statistics; the bias learned for each of the last few controllers is kept and comes back when that one feeds again, a
  new one starts with none learned.

`MotionOptions`, set from the `gyro_*` settings by `input.c` (live). The bias is the rate a still controller reads; it is
learned while the controller lies still and taken off every sample (`calibrate`, `gyro_calibration`). Rates under the
dead zone (`deadzone_rps`, `gyro_deadzone`: revolutions a second, on the length of the rate after the bias) count as
none: the console's zero-drift levels. The rate the game gets, and the angle and direction built from it, are split
into the part about the world's up (the turn, X: left and right) and the rest (the tilt, Y: up and down), each
multiplied by its factor (`turn`, `tilt`: `gyro_turn_sens` and `gyro_tilt_sens` as factors, negative when
`gyro_turn_invert` or `gyro_tilt_invert`); a negative factor is that axis inverted, and 1 and 1 give the sensor's own
rate.

- `MOTION_DEADZONE_LOW` 0.002 revolutions a second: the console's tight zero-drift mode; `MOTION_DEADZONE_STANDARD`
  0.007 its standard mode, `MOTION_DEADZONE_HIGH` 0.01 its loose mode.
- `MOTION_KP` 0.5 1/s: the filter's pull toward the measured gravity. `MOTION_KI` 0.1 1/s^2: its integral term, the
  gyro bias the gravity can see.

## `runtime/input/input.c`

Host input for `nn::hid` (`runtime/hle/ov_input.c`): an SDL3 gamepad (no SDL window needed), the keyboard and the mouse,
merged into Switch npad button bits and stick axes. The button order follows HidNpadButton. The key and pad bindings are
the tables in `runtime/input/input_map.c` (`nx2saria.ini` overrides them). The keyboard and the mouse come from the
window system's back end (`runtime/input/input_wsys.h`: `input_win.c` with Win32, `input_sdl.c` with SDL), which also
owns the cursor capture. The gamepad also rumbles (`input_rumble()`; a Switch controller's linear actuators can take the
game's own values instead, [HD rumble](#hd-rumble)) and, when it has a gyro and an accelerometer, feeds the six-axis
sensor from a thread of its own (`input_motion_start()`). Every SDL call on the gamepad is made under `lock`. Keys and
their map: [controls.md](../controls.md).

State:

- `state`: 0 not started, 1 running, -1 disabled or SDL failed (set to 2, keyboard only, at the end of `start()`).
- `g_block`, `g_menu`: the text entry and the host menu, 0 off, 1 open, 2 waiting for the release
  (`blocked()`).
- `g_kbm_last`: the device the player used last (`input_kbm_last()`), 1 keyboard and mouse, 0 the gamepad. A pad counts
  once a button is down or a stick is past half way (`PAD_USED` 16384: a drifting stick never switches), the mouse once
  it moved `MOUSE_USED` (6) raw counts within one poll (a slow deliberate move does, a desk bump does not), the keyboard
  with any key or mouse button.
- `pad_rumble`: the open gamepad has rumble motors; `pad_labels`: the labels of its south, east, west and north buttons
  (`PAD_LABEL_*`), read from any thread; `pad_east_a`: it labels its east face button A (the Nintendo layout under
  `NX2SARIA_PAD_LAYOUT=auto`).
- `pad_layout`: the `PAD_LAYOUT_*` in effect for the open gamepad, -1 to pick again; live (`cfg_version`).
  `pad_layout_shown`: the same for the menu (any thread), -1 without a gamepad.
- `sensor_pad`: the gamepad whose sensors the motion thread set up, 0 none; `sensor_ok`: it has both sensors, enabled;
  `sensor_gyro`, `sensor_acc`: the pair read (`sensors_ready()`).
- `motion_thread`: 0 not started, 1 running, -1 off (`NX2SARIA_MOTION=0`, no SDL).
- `pad_sdl`: the SDL buttons of the gamepad inputs (`PAD_*` of `input_map.h`), by position; `PAD_ZL` and `PAD_ZR` are
  the triggers (axes).
- `mouse_sens`: `NX2SARIA_MOUSE_SENS`, 0 = mouse camera off, live (`cfg_version`); clamped to 0..10.
- `mouse_turn_seen`, `mouse_turn_logged`, `mouse_turn_y`: what `mouse_turn_log()` last said.
- `stick_dead`: `input_pad_deadzone()` of the left and right stick, live (`cfg_version`).

### `script_load()`, `script_apply()`

`NX2SARIA_INPUT_SCRIPT=<file>`: lines `<frame> <button> [hold]` or `<frame> LSTICK|RSTICK <x> <y> [hold]`, ORed into
the real input. The game menus' mouse (`runtime/ui/menu_mouse.c`) has its own lines: `<frame> POINTER <x> <y>` places
the pointer at those fractions of the window (0..1 from its top left; it stays there until the next `POINTER`),
`<frame> MOUSEL|MOUSER [hold]` holds the left or right mouse button for it, `<frame> WHEEL <notches>` turns the wheel (up
positive). They reach only that module (`input_menu_pointer()`, `input_menu_mouse_presses()`,
`input_menu_wheel_take()`), never the key bindings.

`<frame>` and its hold are 60 Hz ticks of game time since the first present (`frame_step_ticks()`: presents at 60 fps),
so the title menus get the presses at the same game time at any frame limit. A line `@<ms> ...` is timed instead: `<ms>`
and its hold are milliseconds of the world clock (`frame_step_world_us()`, game time since the loaded save's first
frame), so it lands at the same game time at any frame rate and load length. A timed event shorter than a frame is
applied once. In a script with timed lines (a recording) the untimed ones belong before the world clock: they stop when
it starts, as the load ends at another tick in each run (`script_mixed`). `<frame> MARK <name>` presses nothing: with
`NX2SARIA_TRACE=perf` it ends the running segment of frame times and starts one called `<name>`
(`runtime/diag/perf.c` `perf_segment()`).

- `ScriptEv.at`, `.hold`: ticks, or microseconds of the world clock when timed (a timed line's milliseconds times 1000;
  the default hold is 4 ticks, or 66,667 us). `.mouse`: the line is for the menus' mouse (`SM_POINTER`, `SM_LEFT`,
  `SM_RIGHT`, `SM_WHEEL`). A `MOUSE` line's counts are a displacement, never a held speed: its hold is 1.
- `script_state`: 0 not loaded, 1 loaded. `script_mouse`: one-shot raw counts, taken by the camera hook; under `lock`.
- `script_ptr`, `script_ptr_on`, `script_wheel`: the menus' mouse from the script, the pointer (the float bits of x and
  y; `script_ptr_on` once placed) and the wheel notches not taken yet; written under `lock`, read by any thread.
  `script_presses`: the `MOUSEL` and `MOUSER` lines started so far.

In `script_apply()` a one-shot line (a mark, mouse counts, the pointer placed, the wheel) fires late too: a poll may
skip its tick. A pointer placed, a wheel turned or a mouse button held by the script count as the keyboard and mouse
used last, as the mouse moving would.

### `rec_line()`, `rec_close_stick()`, `rec_flush_all()`, `rec_apply()`

`NX2SARIA_INPUT_RECORD=<file>` (no default: not recording when unset) writes the merged input as
`NX2SARIA_INPUT_SCRIPT` lines, one per button press and per stick segment, so a played session replays as a script. A
stick segment ends when an axis moves by more than `REC_STICK_STEP` (2048). An event that begins on the world clock is
written timed (`@<ms>`, hold in ms), one before it in ticks (`frame_step_ticks()`). When the world clock starts, held
buttons and sticks are cut there and go on as timed events from `@0`. `rec_line()` writes `<tick> <what> <hold>` or
`@<ms> <what> <hold ms>`, from tick `from` and world clock `from_w` to now.

- `rec_state`: 0 not opened, 1 recording, -1 off.
- `rec_down`: the tick + 1 at which each button went down, 0 while up; `rec_down_w`: the world clock then, -1 before it
  started.
- `rec_stick_from`: the tick + 1 at which the current non-zero stick segment began; `rec_stick_w` its world clock.
- `rec_last_w`: the world clock at the previous apply.

### `input_direct_mouse_sample()`

The script's `MOUSE` counts and, while the cursor is captured, the window is in front and the cursor allowed, the real
mouse's; the sum saturates at the 32-bit range. With a recording on, each sample with counts is written as a `MOUSE`
line.

### `name_matches()`, `walk_get()`

The walk key: the keyboard's stick samples scaled down while a key is held (`NX2SARIA_WALK_KEY`,
`NX2SARIA_WALK_SCALE`). A keyboard key is all or nothing, so every keyboard-driven stick sample is +/-32767 and the game
only ever sees a full tilt. Holding this key scales those two samples, which gives the half tilt a gamepad gets from its
analog range, so the keyboard can walk as well as run. Only the keyboard is scaled: the gamepad's axes and the mouse
camera sample their own values. `walk_key`: a `KEY_*`, `KEY_NONE` = off ("none", the default); `walk_scale`: 0.2 to 0.9;
both live (`cfg_version`).

The ini takes the key names the Controls tab writes; a hand-written line may spell them in any case (`name_matches()`).
The default scale is 0.51 rather than 0.5: a keyboard sample is 32767, and 32767 * 0.5 = 16383 falls one short of
`stick_bits()`' `STICK_DIR` (16384), which would leave the game's stick-direction bits clear while the walk key is held.
The value in the settings table carries that default; this code only rejects what the table's range does not allow.

### `input_mouse_turned()`, `mouse_turn_log()`

`input_mouse_turned()`: the mouse's counts with `NX2SARIA_MOUSE_INVERT_X`, `NX2SARIA_MOUSE_INVERT_Y` and
`NX2SARIA_MOUSE_SENS_Y` applied (`input_mouse_turn()`), for the camera in both mouse modes: the stick mode here
(`mouse_camera()`) and the direct mode in `runtime/input/mouse_direct.c`. Read at every call, so a change is live. The
selectors (the wheel and the quick lists) and the menus' pointer take the counts as they are: inverting them would turn
a wheel the wrong way round. An input recording (`input_record`) keeps the counts as they are too, and a replay goes
through the same settings.

`mouse_turn_log()`: from the poll, one line `input: mouse camera: mouse_invert_x = ..., mouse_invert_y = ...,
mouse_sens_y = ...` when the three differ from the defaults at start, and again when they change while the game runs.
With the defaults nothing is said.

### `mouse_camera()`

Under `lock`. Selector 1 is a radial wheel, 2..4 are distinct horizontal quick lists; only the wheel keeps a virtual
cursor. When a selector closes, the mouse's movement is taken and dropped, so the wheel's last movement does not reach
the camera. While direct mouse input is active the camera update takes the raw counts itself, once, outside the HID
stick path, so this returns no deflection. In the stick mode a frame's counts are taken once per presented frame, over
that frame's time (4 ms to 100 ms); when frames stop for more than 0.1 s the deflection is not held.

### `stick()`

One gamepad stick through the dead zone; SDL's Y grows downwards, the Switch's upwards, so the Y axis is negated.

### `up_main()`, `sdl_start()`, `sdl_enter()`, `sdl_leave()`

SDL's gamepad update, on a thread of its own. `SDL_UpdateGamepads` reads the controllers, and when a device came or went it first finds every device again, 100 ms
and more inside the system's input libraries. It ran on the game's polling thread with the input lock held: a controller
that disconnected (a wireless one going to sleep, a cable pulled) gave a burst of 120 to 240 ms frames, one per device
notification, with the game's other pollers queued on the lock.

The pad thread (`up_main()`) makes SDL's initialisation and every update. A poll asks for an update and waits for it,
`UP_WAIT_NS` at most (`sdl_enter()`): an ordinary update is done in microseconds and the poll reads fresh state, as
before; one that runs into the device search is not, and the poll goes on with what it read last. `sdl_lock` is held
around every SDL gamepad call: by the pad thread for an update, by everyone else through `sdl_enter()` or a try
(`plat_mutex_trylock`), never by waiting, so no thread of the game stands behind the device search.

- `UP_WAIT_NS` 2 ms: how long a poll waits for its update. `UP_FRESH_NS` 0.5 ms: polls this soon after an update share
  it (the game reads several pads in a row).
- `up_lock` guards the update's fields; `up_gen` counts the updates finished, `up_at` is when the last one finished and
  `up_t0` when the running one began; `up_stale` counts the polls that went on without an update; `up_test_ns`
  (`--probe-input`): the next update takes this long, as a device search does.

`up_main()`: SDL drops gamepad presses while it thinks the program is in the background, and it judges that from windows
of its own: the Win32 window (`runtime/gfx/gfx_window_win.c`) is not one, and keyboard focus is gated here
(`ikb_foreground()`), so `SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS` is set. On Windows the window is our own, so nothing
reads SDL's event queue, yet every axis and button change of a gamepad is queued there: at SDL's cap (about 65,000
events, several MB) each further one fails and sets SDL's error text. The state the getters read does not come from the
queue, so each update flushes it. An update of 20 ms or more is logged (16 lines at most) with the polls that went on
with their last state.

`sdl_start()`, under `lock`: starts the pad thread and waits for SDL's initialisation on it; 1 when SDL's gamepads can
be used. `sdl_enter()`, under `lock`: brings SDL's gamepad state up to date and takes `sdl_lock` for the caller's SDL
calls; 1 with it held (`sdl_leave()` gives it back), 0 when the update is not done within `UP_WAIT_NS`, and then nothing
of SDL may be called. During the device search the first poll waited for it, the ones after it do not; and when the next
update began already, for another poller, the try fails too.

### HD rumble

The game's vibration values as they are, for a controller with linear actuators. A Switch Pro Controller and a Joy-Con
play a frequency and an amplitude per band, which is what the game sends (`runtime/hle/ov_input.c` `VibrationValue`);
`SDL_RumbleGamepad` drives them at two fixed frequencies. With `NX2SARIA_RUMBLE_HD=1` (live, off by default) the primary
gamepad, when it is one of these, gets the game's values in the controller's own rumble report
(`runtime/input/rumble_hd.c`) and no dual-motor call any more (`input_rumble()`):

- `HD_SDL`, a Pro Controller or a single Joy-Con: `SDL_SendGamepadEffect` takes the report for these controllers and
  writes it with SDL's own report number, over USB and over Bluetooth. With the setting on, `hd_probe()` tries the call
  with the neutral value when the gamepad becomes the primary, so the log says at once whether it is taken. With the
  setting off nothing is written: the controller is known by its ids, and the first report after the setting is turned
  on is the trial (`hd_wrote()`; a refusal sends the gamepad back to its motors).
- `HD_PAIR`, a left and a right Joy-Con that SDL presents as one gamepad: SDL refuses the call for that gamepad, so the
  reports go to the two HID devices directly (`runtime/input/input_hd_pair.c`).

Every other gamepad keeps the motors' path, the Switch 2 controllers too (other product ids, another protocol). The
game's thread only posts its newest values (`input_rumble_hd()`). A thread of its own writes the reports (`hd_main()`):
every `HD_TICK_NS` (5 ms) while there is something to write it asks `rumble_hd_due()`, which spaces the reports, keeps
only the newest value, repeats a held one, and turns values the game stopped renewing into the neutral one, so a paused
or frozen game does not leave the controller vibrating. The thread sleeps once the neutral value is out. The neutral
value is also written when another gamepad becomes the primary (`hd_pad_leave()`), when the setting is turned off
(`hd_tick()`) and before the process ends (`input_rumble_hd_stop()`).

The ids: Nintendo's vendor 0x057E, the left Joy-Con 0x2006, the right 0x2007, the Pro Controller 0x2009, the Joy-Con
charging grip 0x200E.

State:

- `pad_hd`, under `lock`: how the primary takes rumble reports (`HD_*`). `hd_shown`, any thread: 0 the primary takes
  none, 1 it takes them, 2 for its single actuator. `sdl_rumbling`, under `lock`: the primary's motors were last set to
  more than 0 (`input_rumble()`).
- `hd_lock` guards the fields below; it is taken after `lock`, never before it. `hd`: what the primary is to get and what
  it got. `hd_kick`: work for the thread, a post or a pair that left. `hd_pair_left`: the pair stopped being the
  primary, its two devices are to be closed. `hd_ended`: `input_rumble_hd_stop()` ran, nothing is written any more.
  `hd_thread`: 0 not started, 1 running, -1 it could not be started (set under `hd_lock`).
- `hd_first_logged`, under `lock`: the primary's first report was logged (`hd_wrote()`).

#### `hd_setting()`

`NX2SARIA_RUMBLE_HD` is on and rumble is not off altogether (`NX2SARIA_RUMBLE=0`); live (`cfg_version`). Any thread.

#### `hd_gap_ns()`

The time between two reports: `NX2SARIA_RUMBLE_HD_MS`, 5 to 50 ms, `RUMBLE_HD_GAP_NS` when unset; live. The HD thread
only.

#### `hd_probe()`

Under `lock` and `sdl_lock`: how gamepad `p` takes rumble reports (`HD_*`), with `*one` set for a single actuator and the
words for the log in `how`. The controllers are known by Nintendo's vendor id, their product ids and the kind SDL reads
from the controller itself: a pad of another make that only looks like one to SDL stays on its motors. With the setting
off nothing is written to the controller.

#### `hd_pad_leave()`

Under `lock`, and under `sdl_lock` when `connected` is set: the primary stops being the primary (another gamepad takes
over, or it went away). A controller the HD path left vibrating gets the neutral value, and what was written to it does
not count for the next primary. For a pair the HD thread closes the two devices, after a neutral report to each.

#### `hd_wrote()`

Under `lock`: the report with data `d` was written at `now` (`ok`), or the controller or its driver did not take it: the
gamepad then goes back to its motors until it becomes the primary again. The primary's first report is logged with its
bytes, so a log shows that the path is in use and what the controller was sent.

#### `hd_tick()`

One round of the HD rumble thread: writes the report that is due, if one is; 1 when nothing is left to write until the
game posts again. While SDL updates (`sdl_lock` held) the report waits a round. A gamepad that went away is let go by the
next scan (`scan_pads()`). SDL puts its own report number in. A pair's report waits for the two writes, so it is not
written under `lock`; after `input_rumble_hd_stop()` it writes nothing (`hd_pair_end()`), and the result counts only
unless the pair left or the stop came meanwhile (`hd_ended` is written under `lock` as well).

#### `input_rumble_hd_stop()`

The locks are tried for a while (50 times 1 ms each) and not waited for: a thread of the game may hold one for good by
now. `hd_lock` not got means a fatal error or a crash on the thread that writes the reports (`runtime/dispatch.c`
`rt_rumble_stop_bounded()`). SDL writes the report on a thread of its own, so the stop waits 30 ms for it before the
process ends; a pair gets 100 ms (`hd_pair_end()`).

### `pad_state_open()`, `pad_primary()`, `sensor_pair_has()`, `map_joysticks()`, `scan_pads()`, `pad_axis()`, `pad_active()`, `pad_follow()`

The gamepads. Every connected gamepad is kept open (`MAX_PADS` 8); `pad` is the one the game reads (and the one that rumbles and feeds
motion): the first one found, then whichever one the player presses (`pad_follow()`). Taking only the first gamepad SDL
lists left the game deaf when that one was a device nobody holds: a virtual pad of Steam Input or DS4Windows, a wheel, a
receiver's idle second slot, listed before the controller in the player's hands. `pad_st`, per open gamepad
(`pad_state_open()` when it opens, moved with it when another closes): when it opened, bit `a` of `rest` set once axis
`a` counts, and for each trigger whether it is at rest now (`timing`) and since when (`rest_since`), and whether it
reads held now (`holding`) and since when (`held_since`). `trigger_logs`
and `trigger_stuck_logged` limit the trigger lines of `pad_axis()` per session. `pad_pressed_logged`: the primary's
first press was logged (once per primary).

`sensor_pairs`: the gyro and accelerometer pairs a gamepad may have, in the order they are taken: the gamepad's own,
then a Joy-Con half's. SDL gives a combined Joy-Con pair the right Joy-Con's sensors as the plain pair already; the
right and left pairs are for drivers that list only those. `sensor_pair_has()`, under `lock` and `sdl_lock`: 1 when
gamepad `p` has pair `i`.

`pad_primary()`, under `lock`: makes `p` the gamepad the game reads, with its labels, rumble and layout; logs it.

`map_joysticks()`: joysticks SDL has no gamepad mapping for are not gamepads to it, and this file opens gamepads only.
Known ones get a mapping built from their own GUID (so it matches whatever bus, version and name checksum SDL put in
it); any other is logged once with its name, ids and GUID. The known one (`known_joysticks`): a Switch that presents its
Joy-Con over USB from its boot menu's tools, "Nyx Joy-Con", VID 0x11EC, PID 0xA7E1, a plain HID joystick: X, Y (left
stick), Z, Rz (right stick) from 0 to 255 with up at 0, a hat for the D-pad, and buttons 1-12 B, A, Y, X (south, east,
west, north), L, R, ZL, ZR, Minus, Plus, the left and right stick clicks. Capture and Home are not sent.

`scan_pads()`, under `lock`: closes the gamepads that went away and opens the ones that came; the primary falls back to
the first open one when it left. Logs each change. The game is always told one controller is connected, so a disconnect
read as nothing pressed and play went on uncontrolled (the console would stop and ask for a controller). When the
primary goes and no other gamepad takes its place, the menu opens: the player sees it, and it takes the keyboard and
mouse until it is closed.

`pad_axis()`, under `lock`: axis `a` of open gamepad `p`, or 0 until the axis has been seen at rest (`AXIS_REST` 8192,
within a quarter of its range of the middle; a trigger's rest is 0). A device that reports an axis held from the start (a
virtual pad nobody holds, a trigger or pedal mapped onto a stick, a stick that rests far off its middle) would otherwise
turn the camera on its own and, counted as pressed, take the gamepad role from the controller in the player's hands for
good (`pad_follow()`). A stick the player holds at the start counts once it has been let go.

A trigger (ZL, ZR) counts only after `TRIGGER_REST_NS` (1 s) at rest without a break; a reading past `AXIS_REST` starts
the second again. One reading at rest was not enough: a 2.4 GHz receiver or a virtual pad (Switch2Connect, "Retro
Controller") can report a trigger at rest for an instant, or report 0 before its first real report, and then held for
good, and the game read ZL or ZR pressed for the whole session. The price is the first second after a gamepad opens, in
which its triggers read 0. Each release is logged with its wait and the time since the gamepad opened (`TRIGGER_LOGS`
16 lines a session); a trigger not released that has read held for `TRIGGER_STUCK_NS` (10 s) without a reading at
rest is logged once a session with the value it reads (a receiver that rests near 16384 never counts). The 10 s run
from the first held reading, not from when the gamepad opened: a second gamepad nobody read for a while and then pressed
in its first second is not called stuck. Sticks keep the single reading. The
time between two readings at rest counts as rest: a gamepad nobody reads (not the primary, and the game not polling) is not watched meanwhile.
`tests/pad_rest_check.py` builds the function with stand-ins for SDL, the clock and the log.

`pad_active()`, under `lock`: 1 when `p` has a button down, a trigger pulled or a stick past half way (a drifting stick
does not count, nor an axis not yet seen at rest). `pad_follow()`, under `lock`: when the primary is idle and another
open gamepad is pressed, that one becomes the primary.

### `pad_layout_get()`

Under `lock`: the layout of the open gamepad, picked again when a setting or the gamepad changed; logs each change.

### `binds_refresh()`

The `NX2SARIA_BIND_*` values last applied, one per line (`binds_applied`); the tables are built again when one changes
(the menu's remapping rows write them while the game runs). Under `lock`.

### `test_noise()`, `test_pad_gyro_send()`, `test_pad_make()`, `test_pad_attach()`, `test_pad_tick()`

The test gamepad. `NX2SARIA_TEST_PAD=<lx>,<ly>,<rx>,<ry>` (test runs): an SDL virtual gamepad whose sticks rest at these raw SDL axis
values (-32768..32767, down is +y), attached before the first scan so it is the gamepad the game reads: drift and full
deflection go through the same mapping and dead zone as a real pad. `NX2SARIA_TEST_PAD_AT=<ms>`: attached that long
after input starts instead, like a controller switched on in play, and its south button pressed for half a second one
second later (the log then shows the hot-plug and the press arriving: `scan_pads()`, `pad_physical()`).

`NX2SARIA_TEST_PAD_GYRO=<x>,<y>,<z>` (test runs, with `NX2SARIA_TEST_PAD`): the test gamepad also has a gyro and an
accelerometer (200 Hz each). It lies face up (the push against gravity is +y) and its gyro reads that bias (deg/s, SDL's
gamepad axes) plus a little noise, in a repeating script of `TEST_STILL_NS` (4 s) still and then `TEST_HELD_NS` (2 s)
"held" (much more noise on both sensors: 3 deg/s and 0.05 G against 0.05 deg/s still), so the log shows the gyro bias
learned while still and learned again after each held phase (`runtime/input/motion_fusion.c`).

- `test_joy` is kept open: a virtual joystick's axes and buttons are set through an open handle.
- `test_attach_ns`, `test_press_ns`: when to attach and when to press, for `NX2SARIA_TEST_PAD_AT` (0: done).
- `test_gyro`, `test_gyro_bias` (rad/s), `test_gyro_t0` (when its script began), `test_gyro_rng`: the noise's
  generator, a fixed seed, so every run reads the same samples. `test_noise()`: a uniform number in [-1, 1) from a small
  linear congruential generator.
- `test_pad_gyro_send()`, under `lock` and `sdl_lock`: one gyro and accelerometer sample of the script. SDL applies it
  at the next `SDL_UpdateGamepads` (`up_main()`); `test_pad_tick()` and `motion_main()` both send, so a sample comes at
  least every 4 ms while the motion thread runs with a gamepad open and its `sdl_enter()` succeeds (not while SDL finds
  devices again).
- `test_pad_tick()`, under `lock` and `sdl_lock`, every poll: the delayed attach and the press of
  `NX2SARIA_TEST_PAD_AT`, and the test gyro's sample.

### `start()`

SDL's initialisation runs on the pad thread (`up_main()`). The first scan holds `sdl_lock` itself: the pad thread
updates only when asked, and nobody has asked yet. Without SDL the state becomes 2, keyboard only.

### `stick_bits()`, `pick()`

`stick_bits()` sets the four direction bits of a stick past `STICK_DIR` (16384, half way). `pick()` keeps the longer of
two stick values.

### `shake_stick()`

The shake key (`B_SHAKE`): the game detaches a part fused to the held object when the right stick swings left and right
a few times. A press swings it for `SHAKE_NS` (0.8 s), holding keeps it swinging. Each side lasts `SHAKE_SIDE_STEPS` (2)
updates of the game, counted by its presents (`runtime/pacing/frame_rate.c` `frame_step_presents()`), as two frames at
the game's 30 fps did, so the game sees the same swings at any frame rate. A side of a fixed time (two 30 fps frames,
67 ms) lasted 8 updates at 120 fps and 16 at 240. Returns the stick x, 0 while not shaking. Under `lock`.

### `pad_physical()`

Under `lock` and inside `sdl_enter()` (the gamepads' state is up to date), state 1: the open gamepad's inputs by
position (`pd[PAD_*]` != 0 held); 0 without a connected gamepad. The scan runs every `RESCAN_MS` (1 s), not only without
a gamepad: a second controller, or one that wakes up after the start, is found. The paddles read false on a pad without
them. The primary's first press is logged once, so the log shows that its presses arrive.

### `xinput_read()`

Windows. Without a gamepad SDL can open, Xbox controllers are read through XInput directly (`xinput1_4`, else
`xinput9_1_0`): SDL may miss a pad another program or driver presents only as XInput. Slots 0-3, the first connected
one; a slot found empty is asked again once a second, as `XInputGetState` on an empty slot takes milliseconds. Under
`lock`: the first connected XInput pad's inputs by position (`PAD_*`) and sticks (y up); 0 without one. A trigger counts
as pressed over 128.

### `ds3_pressed()`, `ds3_primary()`

Windows. `ds3_pressed()`: 1 when the DualShock 3 reader (`runtime/input/input_ds3_win.c`) has input beyond rest, a
button down or a stick past half way. `ds3_primary()`, under `lock`: the DualShock 3 reader's inputs when it is the
controller the game follows, else 0. Next to an SDL gamepad the one pressed last is followed, as between two SDL gamepads
(`pad_follow()`); the same DualShock 3 that a driver also presents to SDL is two sources of one device, and either one
gives the same presses.

### `pad_read()`, `pad_read_now()`

Under `lock`, state 1: the gamepad's buttons (ORed into `*b`, Switch bits after `NX2SARIA_PAD_LAYOUT` and the
`NX2SARIA_BIND_*` lines) and sticks; 0 without one. While SDL is finding devices again (`sdl_enter()`) the answer is the
last one read: what was held stays held for those moments.

### `input_pad_stick_percent()`

Not while SDL updates: this reading is skipped then. The game's poll keeps the state fresh (`sdl_enter()`).

### `poll_merged()`

Mouse buttons count only while the cursor is captured, and a button already held when the capture began (the click that
refocused the window) is ignored until it is released. Not while a game menu has the pointer
(`runtime/ui/menu_cursor.c`, `runtime/ui/menu_mouse.c`): its clicks are A and B there, also in the moment before the
window thread has released the capture.

A key scan is one key state system call per key (about 95). The game reads several pads in a row: reads within a
millisecond of a scan get its answers again.

The buttons whose selector the right stick picks from: L (the wheel, 1), up (2), left (3) and right (4). None in Ocarina
(1.1.1): its D-pad acts at once (ocarina, accessory, Navi, boots), and with L or R held the camera input update goes on
taking the mouse; where an update stops taking it, the mouse is the right stick again (`mouse_camera()`).

A press or a full-way stick on the pad wins over keys held in the same poll: the player picked it up. Minus and Plus
held together (gamepad or keys) open or close the menu and never reach the game; L3+R3 step the performance overlay
(F10).

### `input_script_buttons()`

The buttons `NX2SARIA_INPUT_SCRIPT` holds now, for the settings menu while it is the pause menu's tab
(`runtime/ui/menu.cpp` `pad_events()`), which reads the pad and not the game's merged input. Each line still logs
once, whichever poll reaches it first.

### `input_cursor_allowed()`, `input_block()`, `input_menu()`, `blocked()`, `input_poll()`

Text entry (`swkbd_*.c`): while it is open the game reads no input, as behind the keyboard applet; after it closes the
game reads none until every button and stick direction is released, so the press that closed it stays there. `g_block`
is 0 off, 1 text entry open, 2 closed and waiting for the release. The cursor is released while it is open. The in-game
menu (`runtime/ui/menu.cpp`) blocks the game's input the same way through `g_menu` (0 closed, 1 open, 2 closed and
waiting for the release), and the cursor is released while it is open, for the menu's mouse; when it closes, a capture
that Esc gave up comes back.

`g_block_closed`, `g_menu_closed`: when `g_block` and `g_menu` last went to 2 (`plat_now_ns`). The wait for the release
ends after `RELEASE_WAIT_NS` (1 s) even with something held: an input that never reads as released (a button another
program holds down, a key whose release went to another window, a stick that rests outside its dead zone) would
otherwise keep the game deaf until restart (#462, #494). A press that closed the menu is let go well within the wait; one
still held after it is meant.

`blocked()`: 1 while `flag` (`g_block`, `g_menu`) blocks the game's input; state 2 ends at the first poll with nothing
held, or with something still held `RELEASE_WAIT_NS` after `closed`. A stick axis counts as held as well: with the walk
key the keyboard's sticks move part way without setting a direction bit, so a gate that watched the buttons alone would
let that movement through the moment the menu or the text entry closes. A flag reopened meanwhile stays.

### `input_sdl_version()`

`SDL_GetVersion` is safe before `SDL_Init`.

### `input_capture()`

A DualShock 3 the reader has: its presses can be bound too.

### `input_rumble()`

One call is skipped while SDL updates. When HD rumble has this gamepad (`hd_tick()`) its motors are stopped once and
get nothing more, so the reports with the game's values are the only rumble it is sent. `SDL_RumbleGamepadTriggers`
returns false on a pad without impulse triggers, which is ignored. The DualShock 3 call does nothing unless the reader
has a controller.

### `input_motion_axes()`

SDL's gamepad sensor axes (`SDL_sensor.h`, a gamepad held in front of you): x to the right, y up out of the face (lying
flat face up, +y points at the ceiling), z toward the player (out of the bottom edge). The accelerometer reads the push
against gravity in m/s^2 (lying flat: y = +9.81); the gyro reads rad/s, counter-clockwise positive (right hand). The
Switch's six-axis axes (`nn::hid`): x to the right, y toward the top edge (away from the player), z out of the face;
acceleration as gravity reads (lying flat: z = -1 G). So Switch x = SDL x, Switch y = -SDL z, Switch z = SDL y: a
rotation (right-handed to right-handed), so the gyro keeps its sense, while the acceleration is also negated (push to
gravity) and scaled to G.

### `sensors_ready()`

Under `lock` and `sdl_lock`: enables the sensors again when the open gamepad changed; 1 if it has a gyro and an
accelerometer (`sensor_pairs`, the first pair it has that turns on) and they are on. Nothing drains SDL's event queue,
so there is no event per sample (the data still updates, `SDL_SendJoystickSensor`). Half a pair is of no use: a gyro
whose accelerometer does not turn on is turned off again.

`source_pending`, `source_id`, `source_name`: the controller whose sensors `motion_main()` hands to the fusion next
(`motion_source()`), set by `sensors_ready()` under `lock` and taken by `motion_main()` under `lock`; the call itself is
made after the lock is let go.

### `motion_options_refresh()`

Motion thread only: the gyro settings (`gyro_calibration`, `gyro_deadzone`, `gyro_turn_sens`, `gyro_tilt_sens`,
`gyro_turn_invert`, `gyro_tilt_invert`; live) into the fusion's options (`motion_set_options()`). Read again when
`cfg_version` changes; the options in force are logged the first time and at each change (32 lines at most). A dead
zone name other than off, low, standard and high is low, the default. The sensitivities are clamped to 10..400 percent.

### `motion_main()`

250 Hz: the gamepads update (reports read, rumble expiry) and the newest gyro and accelerometer sample goes into the
fusion. Without a gamepad with sensors, or with motion off (`NX2SARIA_MOTION=0`, live), nothing is fed and the six-axis
sensor reads at rest. No sample while SDL finds devices again. The fusion's own lock is never taken under `lock`
(`motion_source()`, `motion_set_options()`, `motion_restart()` and `motion_feed()` are called after it is let go), so
the two never nest. With `NX2SARIA_TEST_PAD_GYRO` the test sample is sent every round, not only at the game's polls.
Motion turned back on: the orientation and angle from before it went off describe a controller that has moved since
without being followed, so they start over at the next sample (the learned bias stays).

### `probe_gamepad()`

`--probe-input`: the gamepad path on SDL's virtual test gamepad (`NX2SARIA_TEST_PAD`). The pad thread comes up, a poll
reads the gamepad, polls are quick, and while an update takes as long as a device search (`up_test_ns`) the polls go on
with their last state instead of waiting for it. Needs no game files and no controller. 400 ordinary polls each ask the
pad thread for an update (0.7 ms apart, past `UP_FRESH_NS`); then an update that takes 300 ms: the first poll waits
`UP_WAIT_NS` for it, the others not at all, and all get the stick; after it ends (120 ms later) polls are fresh again.

## `runtime/input/input_wsys.h`

Between `runtime/input/input.c` (gamepad, merging, scripts) and the keyboard and mouse of the window system's back end
(`runtime/input/input_win.c` with Win32, `runtime/input/input_sdl.c` with SDL). Any thread. The first part is what the
back end provides, the last line what `input.c` provides the back end.

### `ikb_foreground()`, `ikb_down()`, `ikb_escape()`

The game window is in front, so keys count. A key id (`KEY_*` of `runtime/input/input_map.h`) held, the mouse buttons
included. Esc held: it has no key id, as it belongs to the menu.

### `ikb_pointer()`, `ikb_pointer_set()`, `ikb_captured()`

The pointer in the client area, 0..1 from its top left; 0 when the window is not in front or the pointer is outside.
`ikb_pointer_set()` moves the pointer there (the window in front only). `ikb_captured()`: the cursor is captured for
the mouse camera.

### `ikb_mouse_take()`, `ikb_mouse_travel()`, `ikb_mouse_presses()`, `ikb_wheel_take()`

The raw mouse motion since the last call, in counts. The raw counts the mouse moved in total (|dx| + |dy|), captured or
not. The presses of the left (0) or right (1) mouse button so far, counted at the window's event: a click shorter than a
frame still counts. The wheel notches since the last call, up positive (as the system scrolls).

### `ikb_cursor_update()`, `ikb_menu_closed()`, `input_cursor_allowed()`

`ikb_cursor_update()`: what `input_cursor_allowed()` says may have changed. `ikb_menu_closed()`: the menu closed, so a
capture that Esc gave up comes back. `input_cursor_allowed()` (from `input.c`): no text entry or menu open, the mouse
camera on, input not disabled.

## `runtime/input/input_win.h`

The Win32 window's input seams: `runtime/gfx/gfx_window_win.c`'s window procedure calls these first. Windows only.

### `gfx_window_handle()`

In `runtime/gfx/gfx_window_win.c`; NULL before `gfx_window_start`.

### `menu_window_msg()`

The menu's Win32 adapter (`runtime/ui/menu_win.cpp`), called next: F1 and Esc open or close the menu; while it is open,
keys, mouse and focus go to it. Returns 1 with `*res` set when the message is fully handled.

## `runtime/input/input_win.c`

The Win32 back end of the keyboard and the mouse (`runtime/input/input_wsys.h`; `input.c` merges them with the gamepad):
Win32 virtual keys to the key ids of `runtime/input/input_map.h`, the mouse camera's raw motion (raw input, `WM_INPUT`)
and the cursor capture. Keys and their map: [controls.md](../controls.md).

### `key_vk()`

The Win32 virtual key of a key id (the one place that knows them), 0 for an id without a key. `GetAsyncKeyState` reads
the physical buttons: with Windows' "switch primary and secondary buttons" on, the primary (left) button is the physical
right one, as the window's own `WM_LBUTTONDOWN` has it, so the left and right mouse keys follow `SM_SWAPBUTTON`.

### `input_key_char()`

The top bit of `MapVirtualKeyW`'s answer marks a dead key, and is cleared.

### `cursor_update()`

The mouse camera: raw motion accumulated by the window thread, one stick sample per presented frame.

- `mouse_dx`, `mouse_dy`: raw counts since the last frame sample. `mouse_travel`: raw counts moved in total, captured or
  not (`ikb_mouse_travel()`). `presses`: the left and right button presses so far (`ikb_mouse_presses()`).
  `wheel_delta`: `WM_MOUSEWHEEL` deltas not taken yet (`WHEEL_DELTA` a notch, up positive).
- `cur_captured`: the cursor hidden and confined to the client area. `cur_active`: `WM_ACTIVATEAPP`, the window's
  application is active. `cur_suspend`: F11, or a modal size or move loop under way. `cur_lock` serialises
  `cursor_update()` (the window thread and the `input_block()` callers).
- `cur_rel`, bits: `CUR_REL`, Esc or activation by a click on the title bar or border, until a client click; `CUR_ESC`,
  set with `CUR_REL` by Esc (not when it was already set): closing the menu gives the capture back. One word, changed by
  one compare-and-swap (`cur_rel_update()`).

`cursor_update()` hides the cursor and confines it to the client area while the window is focused, not suspended, not
behind a text entry or the menu, and the mouse camera is on; it releases it otherwise. Safe from any thread. The first
40 changes each way are logged, so a run's log shows the camera got its mouse, or gave it up.

### `input_window_msg()`

The window procedure's hook (`runtime/gfx/gfx_window_win.c` calls it first); returns 1 with `*res` set when the message
is fully handled.

- `WM_CREATE` registers the generic mouse (usage page 1, usage 2) for raw input.
- `WM_INPUT` is left to `DefWindowProc` as well, which must see it for cleanup.
- `WM_MOUSEWHEEL` is counted for the game's menus (`ikb_wheel_take()`); the menu's own handler still sees it.
- `WM_SYSCOMMAND`: Alt pressed alone would enter the window menu loop, which stalls the game: Alt is a game key.
- `WM_KEYDOWN`: Esc is unbound in the game layout: it releases the cursor and opens the menu (`menu_window_msg()`).
- `WM_LBUTTONDOWN` and the others: client area only; `poll_merged()` drops the click that captures.
- `WM_MOUSEACTIVATE` comes before `WM_ACTIVATEAPP`: a title bar or border click must not capture.
- `WM_ACTIVATEAPP`: Alt+Tab back in captures again.

### `ikb_wheel_take()`

Whole notches since the last call; a smooth wheel's part of a notch stays for the next.

## `runtime/input/input_sdl.h`

The SDL3 window's input seams (Linux and the other hosts without Win32): `runtime/gfx/gfx_window_sdl.c` owns the window
and its event loop, `runtime/input/input_sdl.c` the keyboard, the mouse and the cursor, `runtime/ui/menu_sdl.cpp` the
menu's events. Window thread, except where noted.

### `gfx_window_sdl()`, `gfx_window_sdl_density()`, `gfx_window_sdl_run()`

In `runtime/gfx/gfx_window_sdl.c`. The game window, NULL before `gfx_window_start`; any thread. Pixels per window unit
(mouse positions are in units); any thread. `gfx_window_sdl_run()` runs `fn` on the window thread and waits; any
thread.

### `input_sdl_window()`, `input_sdl_event()`, `input_sdl_idle()`

The game window was created; every event, first; no event for a while, so a cursor change asked for from another thread
is applied.

### `menu_sdl_event()`

In `runtime/ui/menu_sdl.cpp`: every event, after `input_sdl_event()`.

## `runtime/input/input_sdl.c`

The SDL3 back end of the keyboard and the mouse (`runtime/input/input_wsys.h`; `input.c` merges them with the gamepad):
SDL scancodes to the key ids of `runtime/input/input_map.h`, the mouse camera's relative motion and the cursor capture
(SDL's relative mouse mode). Key and button state comes from the window's events (window thread) and is read by any
thread. Keys and their map: [controls.md](../controls.md).

The capture follows the Win32 back end's rules (`runtime/input/input_win.c`): captured while the window has the focus,
the mouse camera is on and no text entry or menu is open; Esc releases it until a click in the window (and closing the
menu opened by that Esc gives it back); losing the focus releases it until the window has the focus again.

State:

- `mouse_dx`, `mouse_dy`: relative motion since the last `ikb_mouse_take()`, whole counts; `rem_x`, `rem_y` (window
  thread): the fractions SDL reported beyond them.
- `captured`: relative mouse mode is on. `cur_rel` with `CUR_REL` and `CUR_ESC` as in `input_win.c`: released by Esc or
  a click elsewhere; by Esc.
- `wake_event` (`SDL_RegisterEvents`): wakes the window thread for a cursor change.
- `ptr_x`, `ptr_y`: the pointer outside relative mode (`runtime/ui/menu_mouse.c` points at the game's menus with it), in
  fractions of the window, 0..1 from its top left, as float bits so any thread reads them whole; `ptr_inside`: the
  pointer is over the window. `ptr_warp`: `ikb_pointer_set()` asked the window thread to move it to `warp_x`, `warp_y`.
- `mouse_travel`: counts the mouse moved in total, captured or not (|dx| + |dy|). `presses`: the left and right button
  presses so far (`ikb_mouse_presses()`). `wheel_steps`: wheel notches not taken yet, up positive (`ikb_wheel_take()`);
  `wheel_rem` (window thread): the part of a notch a smooth wheel left over.
- `warp_motion` (window thread): the next motion event is a warp's.

### `key_sc()`, `key_of_sc()`

The SDL scancode of a key id, 0 for an id without a key (the mouse buttons); `key_of_sc()` is its inverse, built on first
use, 0 for none.

### `input_key_char()`

A keycode with the `SDLK_SCANCODE_MASK` bit names no character.

### `pointer_at()`, `pointer_read()`, `cursor_apply()`

The cursor, on the window thread. `pointer_at()`: the pointer at window coordinates (x, y), in the window's units
(points), as fractions of its size: the same fractions of the drawable in pixels, whatever the pixel density, so they
compare with `gfx_picture_rect`'s. `pointer_read()`: the pointer as SDL has it now (after the capture ends SDL puts it
back where it chooses).

`cursor_apply()` logs the first 40 changes, so a run's log shows the camera got its mouse, or gave it up. A warp asked
for (`ikb_pointer_set()`) is made once the capture is off; SDL ignores or limits one on some systems (Wayland), so the
pointer is read again afterwards (a compositor may leave it where it was), and the motion event the warp makes is not
the player's.

### `ikb_cursor_update()`

Any thread: the window thread applies the change at its next event.

### `input_sdl_event()`

- Esc is unbound in the game layout: it releases the cursor and opens the menu (`runtime/ui/menu_sdl.cpp`).
- A click in the window captures again.
- The travel counts the player's mouse only: not a warp's motion, not a touch screen's or a pen's.
- The wheel gives whole notches; a smooth wheel's fractions add up to them. It counts in the direction the system
  scrolls (natural scrolling kept, as Windows' `WM_MOUSEWHEEL` has it), and only in front.
- Losing the focus: coming back (Alt+Tab) captures again, and the keys are cleared, as their key-ups go elsewhere.

### `ikb_pointer()`, `ikb_pointer_set()`

In relative mode the system pointer is hidden and stays put, so there is no pointer then. `ikb_pointer_set()`, any
thread: the window thread warps the pointer at its next event (SDL's video calls belong to it).

## `runtime/input/input_map.h`

Keyboard, mouse and gamepad bindings, free of any window system: the tables map platform-neutral key and pad ids to
Switch actions. `runtime/input/input_win.c` translates Win32 virtual keys and `runtime/input/input_sdl.c` SDL scancodes
to the key ids, `runtime/input/input.c` SDL's gamepad buttons and axes to the pad ids (each in one place). The remapping
rows of the menu's Controls tab (`runtime/ui/binds_page.cpp`) edit a copy of the tables and write the `bind_` lines;
`input.c` applies changed lines while the game runs (`binds_refresh()`). The layout:
[controls.md](../controls.md#remapping).

- The actions are the bit numbers of HidNpadButton (`runtime/input/input.h`); 16..23 are the stick directions, which keys
  drive as stick deflection. `B_SHAKE` is a key of its own, never a button bit: a press shakes the right stick left and
  right for the game (`runtime/input/input.c` `shake_stick()`), which detaches a fused part from the held object. A pad
  shakes its own stick.
- The keys and mouse buttons. F1, F10, F11 and Esc belong to the menu and the window, F8 to the bug capture
  (`runtime/diag/bugcap.c`): they have no ids. The letters, the digits, the mouse buttons and the numpad digits are
  consecutive ids. The punctuation keys are those of a US keyboard, by position; the character on the key follows the
  keyboard layout.
- The gamepad inputs by position (the Nintendo layout, as SDL names them), platform-neutral like the key ids. ZL and ZR
  are the analog triggers. The paddles are the back buttons of an Xbox Elite, DualSense Edge or Steam Controller (SDL's
  `LEFT_PADDLE1` and so on); no Switch button has them by default, a bind line or the remapping rows add them.
- The gamepad layouts (`NX2SARIA_PAD_LAYOUT`): which face button is Switch A. Nintendo: by position, east A, south B,
  west Y, north X. Xbox: south A, east B, west X, north Y, as an Xbox pad labels them (cross A, circle B on a PlayStation
  pad).
- `InputMap`: the keyboard and mouse table and one gamepad table per layout. `input_map` is the tables in effect; the
  defaults are the layout in [controls.md](../controls.md), and `input_map_reset()` restores them.

### `input_action_name()`, `input_key_name()`, `input_pad_name()`

The script and `nx2saria.ini` names, any case when parsed; NULL for an id without one. Actions: A B X Y LCLICK RCLICK L R
ZL ZR PLUS MINUS LEFT UP RIGHT DOWN, then LSTICK_LEFT LSTICK_UP LSTICK_RIGHT LSTICK_DOWN and the same for RSTICK (the
stick directions the keys drive), then SHAKE. Keys: a-z, 0-9, space, tab, lshift and the rest (`key_names` in
`input_map.c`). Pads: south, east and the rest (`pad_names`).

### `input_map_bind()`

Replaces every binding of the action `button` (an action name) with `list`: comma-separated `pad:<name>` and
`key:<name>` entries, `key:default` or `pad:default` for that part's defaults, or `none` for no input. A part the list
names neither way is left unbound. Pad names are physical buttons, so pad entries go into the gamepad table of every
layout; `pad:default` keeps each layout's own button. The stick directions and SHAKE take keys only. Invalid entries are
logged and skipped; when none is valid the action keeps its bindings. 1 when applied.

### `input_map_configure()`

Applies the `NX2SARIA_BIND_<action>` settings (`cfg_get`) over the defaults: at start, and again when one of them
changes.

### `input_pad_layout_pick()`

The layout for a `NX2SARIA_PAD_LAYOUT` value: "nintendo" or "xbox" (any case); "playstation" is the Xbox layout under
its PlayStation name (cross A, circle B, square X, triangle Y); "xbox_prompts", 0.1.9's name for the Nintendo layout
with the Xbox prompts, maps the buttons as "nintendo". Anything else ("auto", NULL) picks Nintendo when the pad's east
button is labelled A (a Switch Pro Controller, a GuliKit) and Xbox otherwise.

### `input_prompts_pick()`

Button prompts (`NX2SARIA_PROMPTS`, [controls.md](../controls.md#button-prompts)): the controller whose buttons the
game's prompts show. Nintendo: the game's own. Xbox, PlayStation and Steam Deck: that pad's buttons (LB RB LT RT, Menu
and View; L1 R1 L2 R2, Options and Create; the Deck's A B X Y with L1 R1 L2 R2, Menu and View), and the face buttons
named by the label of the button the pad layout presses (`runtime/hle/romfs.c` `overlay_init()`,
`tools/pad_prompts.py`). PC: the key or mouse button the keyboard defaults press each button with, the mouse for the
right stick. The value is "nintendo", "xbox", "playstation", "steamdeck" or "pc", any case. Unset or empty: Xbox while
the pad layout setting is "xbox_prompts", else Nintendo.

### `input_prompts_folder()`

The folder next to `romfs-index.bin` that holds those prompts for a `NX2SARIA_PAD_LAYOUT` value, NULL for the game's
own. The Nintendo layout plays a pad by position, so its folder names the face buttons the other way round
(`romfs-patches-xbox` and `romfs-patches-steamdeck`: the right button does A and is labelled B;
`romfs-patches-playstation`: it is circle); xbox and auto put every Switch button on the button labelled with its letter,
or on cross, circle, square and triangle in that order (auto picks the layout per pad by its labels), so they share
`romfs-patches-<style>-label`. The PC prompts follow no pad: `romfs-patches-pc` whatever the layout
(`runtime/hle/romfs.c` `overlay_init()` serves `romfs-patches-pc-<w>x<h>` under that name at a game resolution of
another shape).

### `input_prompts_record()`

The record the build leaves in the game folder `dir` of where its button prompts came from, or why there are none
(`button-prompts.txt`, `tools/make_game.py` `record_prompts()`): its first line, without the line end, into `out` (`n`
bytes, cut short to fit). 0, with `out` empty, when the folder has no record (a build older than the record, or a game
folder made by hand) or its first line is empty.

### `input_map_pad_buttons()`, `input_map_buttons()`, `input_map_held()`, `input_map_stick()`

`down[pad]` (or `down[key]`) != 0 for the inputs held; the Switch button bits (0..15) the layout's pad table, or the key
table, presses. `input_map_held()`: 1 when a key of `action` is held. `input_map_stick()`: x and y of the stick (0
left, 1 right) from its four direction actions; up is +y; a diagonal is scaled to the unit circle.

### `input_mouse_turn()`

The mouse's counts for the camera: x negated with `invert_x`, y negated with `invert_y` and then multiplied by
`sens_y`, held to 0.1..10 (a value that is not a number is 1). The defaults (0, 0, 1) give the counts as floats,
unchanged. A factor on the vertical alone keeps `NX2SARIA_MOUSE_SENS` the one speed of both axes. In floats, so a
fraction of a count is not lost to rounding before the camera scales it. `tests/mouse_stick_check.c` checks it.

### `input_mouse_stick()`

Mouse motion in counts (`input_mouse_turn()`'s, so floats) over `dt_s` seconds to right-stick deflection: full deflection at 3000 / `sens` counts a
second. `cam` is the factor the game multiplies the right stick's turn rates by (`cam_speed_applied`). The deflection is
divided by it, so the turn per count stays what `sens` says, and only then clamped: a clamp taken first would hold the
mouse's fastest turn at the game's unmultiplied top speed, however high the factor. Clamped to the unit circle; up
(negative dy) is +y.

### `input_pad_stick()`, `input_pad_deadzone()`, `input_stick_percent()`

`input_pad_stick()`: a gamepad stick (x, y in -32768..32767, up is +y) through a round dead zone of `dead` (a fraction of
full deflection, 0..0.5): inside the circle the stick reads 0, and the rest of the travel is scaled to start at 0 at the
edge, so a drifting stick rests at 0 and full deflection still reaches full. The result stays inside the unit square.
`input_pad_deadzone()`: the dead zone of gamepad stick 0 (left) or 1 (right), as a fraction of full deflection:
`NX2SARIA_LSTICK_DEADZONE` or `NX2SARIA_RSTICK_DEADZONE` when set, else `NX2SARIA_STICK_DEADZONE` (both sticks), percent
clamped to 0..50. `input_stick_percent()`: how far a stick is from the centre, in percent of full deflection (100 at an
axis's end; a square gate's corner reads more).

### `input_map_keys_of()`, `input_map_pads_of()`, `input_map_set_key()`, `input_map_set_pad()`, `input_map_line()`

The remapping rows' edits, on a copy of `input_map`; pure.

- `input_map_keys_of()`, `input_map_pads_of()`: the action's keys (in table order) or its gamepad buttons in `layout`,
  at most `max`; returns how many.
- `input_map_set_key()`: key slot 0 or 1 of `action` becomes `key`; `KEY_NONE` clears the slot. A key another action
  has moves here: returns that action, -1 when none lost it.
- `input_map_set_pad()`: the action's gamepad button in `layout` becomes `pad`; `PAD_NONE` clears it. The action that
  had `pad` takes the action's old button (a swap): returns it, -1 when none. A remapped button is physical, so the face
  buttons (A B X Y) and the changed actions get the result in every layout: the layout setting no longer moves them. A
  change that puts every button of `layout` back at its default restores the defaults of every layout instead.
- `input_map_line()`: the `bind_` value that gives the action its bindings of `m`: "" when they are the defaults (no line
  needed), else the entries, with `key:default` and `pad:default` for a part at its defaults, or "none".

## `runtime/input/input_map.c`

The binding tables (keyboard and mouse, and gamepad), their `nx2saria.ini` overrides, the pure mapping functions and the
remapping rows' edits (`runtime/input/input_map.h`). No Win32 or SDL here. `DIAGONAL` 23170 is `STICK_MAX` / sqrt(2).

`DEFAULT_KEYS`, Ocarina's controls (its Basic Controls screen, `LayoutMsg/PlusMenu_00` of the game's messages): A acts
and, held, dashes; B puts away; X jumps; Y is the sword; ZL targets; ZR is the selected item; L and R quick-select items;
the d-pad plays the ocarina (left), puts the accessory on (up), calls Navi (right) and changes boots (down); the right
stick's press resets the camera. So E acts and Left Shift, held, dashes (both A); Space jumps; Left Ctrl and Backspace
put away; the mouse buttons attack (left), target (right) and reset the camera (middle); O and N are the ocarina and
Navi besides 1 to 4. W A S D move; the arrow keys are the camera's second source. SHAKE (the right-stick shake) has no
key: Ocarina has no use for it, and `bind_shake` still sets one.

`DEFAULT_PADS`, by position: south is Switch B, east A, west Y, north X (the Nintendo layout); the Xbox layout swaps A
with B and X with Y (south A, as the Xbox labels; cross A on a PlayStation pad). The triggers are ZL and ZR.

`key_names`: each group starts at its id; the ids of a group are consecutive.

### `keys_put()`

The action's keys become `list[0..n)`: its old ones go, the new ones are appended (the table keeps its order otherwise).

### `input_map_bind()`

A bind line names physical buttons: the same in every layout.

### `input_map_set_key()`

The key, once in this action's list, leaves its old place first.

### `pads_at_defaults()`, `input_map_set_pad()`

`pads_at_defaults()`: every action has its default gamepad buttons in `layout`. In `input_map_set_pad()` the other action
gives up `pad` and takes this action's old button; a map put back as it was restores the defaults, so the layout setting
moves the face buttons again.

### `same_set()`

The same bindings in any order (the menu appends; a line lists them its own way).

### `input_prompts_record()`

The first line; a longer one is cut short, as the log line it goes into is.

### `input_prompts_folder()`

The folders are read at start, before a pad is open: auto counts as Xbox, as for a pad without Nintendo labels. A
Nintendo-labelled pad under auto plays by label too (its A is on the button labelled A), so it matches as well.

### `input_pad_stick()`

Past the edge (a square gate's corner) the length is 1.

### `input_pad_deadzone()`

The stick's own dead zone when it reads as a number; else (unset, or "8%" and other typos, whose row has no default of
its own and would read as 0, no dead zone at all) the shared `stick_deadzone`.

### `chord()`, `input_menu_gate()`, `input_overlay_gate()`

Minus and Plus held together open or close the menu. `*held` is latched from the poll where both go down until neither
is down (the caller keeps it). Every poll of the latch reports neither to the game, so releasing one button first does
not leak the other; Minus alone before Plus reaches the game unchanged. With the menu open the game sees a neutral pad.
It returns 1 in the poll where both go down: the caller toggles the menu once. Both stick clicks held together step the
performance overlay as F10 does (a gamepad has no F10: the Steam Deck in Game Mode); the same latch as the menu's, and
the game sees neither click while it holds.

## `runtime/input/input_ds3.h`

The DualShock 3 reader of the Windows build (`runtime/input/input_ds3_win.c`): the controller's own HID reports, read
directly, for when no driver presents it to SDL or XInput as a gamepad. Windows only; `input.c` calls it from its
gamepad poll. `NX2SARIA_DS3=0` (`ds3 = 0` in `nx2saria.ini`) turns it off. Controls:
[controls.md](../controls.md#playstation-3-controllers).

### `ds3_read()`

One poll, under `input.c`'s lock: 1 while a DualShock 3 is connected and reporting, with its inputs by position in
`pd[PAD_*]` (every entry written; `PAD_ZL` and `PAD_ZR` from the trigger pressure) and its sticks in `raw[]` as lx, ly,
rx, ry in -32767..32767, up is +y, before any dead zone. 0 (`pd` and `raw` untouched) without one. It also finds a
controller that is plugged in or switched on later, and lets go of one that went away.

### `ds3_rumble()`

Rumble for the connected controller, under `input.c`'s lock: `low` (0..65535) drives the heavy (left) motor, `high` the
light (right) motor, which the controller can only switch on or off (on from 1/16 of full strength). It lasts `ms`
milliseconds; 0 stops it. 1 when a controller took it.

### `ds3_decode()`, `ds3_output_report()`

The report formats (`runtime/input/input_ds3.c`, any OS). `DS3_TRIGGER_ON` 128: the L2 and R2 pressure, 0..255, that
counts as pressed. `ds3_decode()`: input report 0x01 (`n` bytes, the report id first) to inputs by position in
`pd[PAD_*]` (every entry written) and the sticks in `raw[]` as lx, ly, rx, ry (-32767..32767, up is +y, rest 0); 0, with
`pd` and `raw` untouched, for another report. `ds3_output_report()`: output report 0x01 into `o` (`n` bytes, 36 or
more; zeroed first): the heavy motor at `left` (0..255), the light motor on when `right` is nonzero, both for `dur`
steps of 10 ms (0 stops them), and the player light `led` (1..4, 0 none).

## `runtime/input/input_ds3.c`

The DualShock 3's report formats, free of any operating system (`runtime/input/input_ds3.h`): the input report to inputs
by position and the output report that drives the motors and the player light. `tests/ds3_check.c` checks both;
`runtime/input/input_ds3_win.c` moves the reports.

### `ds3_decode()`

Byte 3's bits 4 to 7 are triangle (north), circle (east), cross (south) and square (west). The triggers go by pressure,
as an XInput trigger does; a report without it (`n` < 20) has the switch only. The controller's y grows downwards, the
game's upwards.

### `ds3_output_report()`

The four lights' blink timing (five bytes each from byte 11): steady.

## `runtime/input/input_ds3_win.c`

The DualShock 3 reader of the Windows build (`runtime/input/input_ds3.h`): the controller's HID reports read directly with
`hid.dll`. The controller sends nothing until it is told to (a feature report, sent on connect), and SDL and XInput get
nothing from it without a driver that does this and presents it as a gamepad; with one, SDL or XInput already see it and
this file is only a second reader of the same pad (`input.c` follows the one pressed last then, `ds3_primary()`).

`hid.dll` and `setupapi.dll` are loaded at run time, so the exe links no extra library and starts without them. A scan
for the controller runs on a thread of its own every `SCAN_MS` while none is open: walking every HID device takes
milliseconds, which the game's poll must never wait for. Everything else (open, read, rumble, close) runs on the caller's
thread, under `input.c`'s lock.

The report formats are in `runtime/input/input_ds3.c`. The accelerometer and the single (yaw) gyro are not read: the
game's six-axis sensor needs all three gyro axes.

Constants: Sony's vendor 0x054C, the DualShock 3's product 0x0268. `SCAN_MS` 3000: between scans while no controller is
open. `OPEN_RETRY_MS` 1000: between attempts to open a path the scan found. `SILENT_NS` 2 s: no report this long is not
reporting (the read stays queued, and a report revives it). `HidAttr` is `HIDD_ATTRIBUTES`, `HidCaps` `HIDP_CAPS` (64
bytes); `HidP_GetCaps` returns an NTSTATUS, 0x110000 on success. `api_state`: 0 not tried, 1 loaded, -1 unavailable.

### `scan_once()`, `scan_thread()`

The scan, on its own thread. `found_path` is written by the scan thread, then `found` set; the poll reads it. `found`:
`found_path` holds a controller. `open_now`: the poll holds a controller open, so the scan rests. `scan_on`:
`NX2SARIA_DS3` is on (default off), so the scan runs. `scan_once()`: the path of the first DualShock 3 among the HID
devices that are present, copied into `out`; 0 when there is none. It opens each device with access 0: enough to ask
for the ids, and it works on a device another program has open.

### `drop()`, `send_output()`, `try_open()`, `parse()`, `pump()`, `rumble_expire()`

The open controller, on the poll's thread. `s.ev` is the read's event, made once; `s.pending`: a `ReadFile` is queued;
`s.buf` its buffer; `s.pd` and `s.raw` the newest report; `s.last_report` the `plat_now_ns` of the newest valid report,
0 none yet; `s.reported`: logged that a report came in; `s.motors_on`: the controller's motors are driven now;
`s.sent_left`, `s.sent_right`: the last output report's motors; `s.sent_at`: when it went out; `s.until`: when the
rumble ends (`plat_now_ns`), 0 none.

- `drop()` clears the handle only: the buffers stay where a cancelled read may still write.
- `send_output()`: output report 0x01, the motors and the player light; `left` 0..255 (heavy), `right` 0 or 1 (light),
  `dur` in 10 ms steps.
- `try_open()`: nothing found, or too soon, returns at once. A pad that presents itself in another report layout (a
  driver's generic mode) would read as garbage here, so a device whose product string has no "PLAYSTATION" is left to
  SDL. The reports start only after feature report 0xF4, "start sending". Player 1's light shows that the controller is
  ours.
- `pump()` reads every report that came in since the last poll; the newest wins.
- `rumble_expire()` ends a rumble whose time is up: the controller stops by itself after the duration it was given, and
  this is the same moment from our side, so the next rumble starts from a known state.

### `ds3_read()`

Live: the setting is read again when one changed. `NX2SARIA_DS3` is off by default (the settings table in
`runtime/cfg/cfg.c`): the reader opens the pad read-write.

### `ds3_rumble()`

It resends when a motor changed or the controller's own timer is about to run out; the game renews a rumble every 50 ms,
which is more often than the controller needs. The light motor is on from 0x1000 of 65535.

## `runtime/input/rumble_hd.h`

HD rumble: the game's vibration values for a controller with linear actuators (Switch Pro Controller, Joy-Con), as the
bytes of the controller's rumble output report, and the decision when a report is written. Such an actuator takes a
frequency and an amplitude for a low and a high band, which is what the game sends (`runtime/hle/ov_input.c`
`VibrationValue`), so nothing is reduced to motor strengths here. Pure: no SDL and no platform calls;
`runtime/input/input.c` (`hd_tick()`) writes what this file decides. Checked by `tests/rumble_hd_check.py`.

`RumbleHdValue`: one actuator's value as the game sends it, amplitudes 0..1, frequencies in Hz.

Constants:

- `RUMBLE_HD_SIDE` 4: the bytes of one actuator's value in a report. `RUMBLE_HD_DATA` 8: those of a report's two, the left
  actuator's, then the right one's.
- `RUMBLE_HD_REPORT_ID` 0x10: the output report that carries nothing but rumble data. `RUMBLE_HD_REPORT` 10: its bytes
  that count, the id, the report's number and the data.
- `RUMBLE_HD_AMP_STEPS` 100: the highest amplitude step written (amplitude 1.0); the steps above it overdrive the
  actuator.
- `RUMBLE_HD_NEUTRAL_REPORTS` 3: how many times in a row the neutral value is written when the vibration ends.
- `RUMBLE_HD_LOW_MIN` 40.875885, `RUMBLE_HD_LOW_MAX` 626.286133, `RUMBLE_HD_HIGH_MIN` 81.75177, `RUMBLE_HD_HIGH_MAX`
  1252.572266: the frequencies an actuator takes, the low band's and the high band's ends in Hz; others are clamped to
  them.
- `RUMBLE_HD_STALE_NS` 250 ms: values the game has not renewed for this long count as neutral. `RUMBLE_HD_REFRESH_NS`
  30 ms: a value that still holds is written again this often. `RUMBLE_HD_GAP_NS` 15 ms: the default time between two
  reports (`rumble_hd_due()`'s gap).
- `rumble_hd_neutral`: the value of an actuator at rest (no amplitude, 160 Hz and 320 Hz), twice.

`RumbleHd`, what one controller is to get and what it got, all zero at the start: `want` the newest data from the game
(`rumble_hd_post()`), `want_at` when it came, `have` once there is one; `sent` the data of the last report written
(`rumble_hd_sent()`), `sent_at` when, `any` once there is one; `neutral_left` the neutral reports still to write after the
last one.

### `rumble_hd_amp_step()`, `rumble_hd_step_amp()`

The amplitude step (0..`RUMBLE_HD_AMP_STEPS`) nearest to `a`, and that step's amplitude. A NaN or a value below half of
the first step is step 0; 1 and everything above it is the last step.

### `rumble_hd_encode()`

One actuator's value as its four report bytes. `strength` scales both amplitudes (1 = as the game sent them) before they
are clamped to 0..1. With both amplitudes at step 0 the bytes are the neutral value's, whatever the frequencies.

### `rumble_hd_data()`

A report's data from the game's newest value per side (`v[0]` left, `v[1]` right). `live[s]` 0: that side is at rest
(never sent, stale, or vibration not permitted). `one`: the controller has a single actuator (one Joy-Con), so the
stronger side's value goes into both halves.

### `rumble_hd_report()`

The output report: the id, the number (0..15, going round), the data, zeros up to `n` bytes (`n` >= `RUMBLE_HD_REPORT`).

### `rumble_hd_post()`, `rumble_hd_due()`, `rumble_hd_sent()`, `rumble_hd_idle()`, `rumble_hd_buzzing()`, `rumble_hd_forget()`

- `rumble_hd_post()`: the game's newest data; it replaces what was not written yet (nothing is queued).
- `rumble_hd_due()`: 1 when a report is to be written at `now`, with its data in `out`. `on` 0: the controller gets the
  neutral value whatever the game sent (the setting is off, the process ends). A changed value waits until `gap_ns` have
  passed since the last report; a value that still holds is written again every `RUMBLE_HD_REFRESH_NS`, since the
  actuators stop by themselves when no report comes; data older than `RUMBLE_HD_STALE_NS` is neutral; the neutral value
  is written `RUMBLE_HD_NEUTRAL_REPORTS` times, `gap_ns` apart, and then nothing until the game posts something else.
- `rumble_hd_sent()`: the report with `data` was written at `now`.
- `rumble_hd_idle()`: 1 when nothing is to be written until the next `rumble_hd_post()` (or until `on` changes).
- `rumble_hd_buzzing()`: 1 while the last report written was not the neutral value: the actuators may still be running.
- `rumble_hd_forget()`: another controller takes the reports; what was written no longer counts, the game's newest data
  stays.

## `runtime/input/rumble_hd.c`

HD rumble for controllers with linear actuators: the encoding of a vibration value and the report schedule
(`runtime/input/rumble_hd.h`). The byte layout and the step tables are those of the Switch controllers' rumble output
report.

One actuator's value is four bytes, for a high band (hf, 9 bits; its amplitude ha, 7 bits) and a low band (lf, 7 bits;
its amplitude la, 7 bits):

```
byte 0  hf bits 0-7
byte 1  hf bit 8 in bit 0, ha in bits 1-7
byte 2  lf in bits 0-6, la bit 0 in bit 7
byte 3  0x40 + la bits 1-6
```

Frequencies are steps of 1/32 octave above 10 Hz: n = round(32 * log2(f / 10)). The low band takes n 0x41..0xBF (40.9
to 626.3 Hz) as lf = n - 0x40, the high band n 0x61..0xDF (81.8 to 1252.6 Hz) as hf = (n - 0x60) * 4. Amplitudes are
steps 0..100 on a scale that is finer where the amplitude is larger (`rumble_hd_step_amp()`), written to both bands as
ha = la = step. Steps above 100 exist in the format and drive the actuator past what it is built for;
`rumble_hd_amp_step()` never returns one.

### `rumble_hd_step_amp()`

The amplitude scale: step 0 is off, step 1 the smallest amplitude there is (`AMP_STEP1` 0.007843), steps 2 to 15 a
quarter octave apart from `AMP_STEP2` 0.011823, 16 to 31 a sixteenth, 32 to 100 a thirty-second, ending at 1.0.

### `rumble_hd_amp_step()`

Below half of the first step, a NaN included, is step 0. The three ranges meet half way (on the log scale) between their
neighbouring steps: 1 and 2 (0.00963), 15 and 16 (0.1150), 31 and 32 (0.2276).

### `freq_step()`

The frequency step of `f` Hz within `lo`..`hi` (step numbers); a NaN or a frequency at or below 0 is the lowest.

### `rumble_hd_data()`

How strong a side is, for a controller that can play only one: the two bands' amplitudes as energy (the sum of their
squares).

### `target()`

What the controller is to play at `now`: the game's data while it is fresh and the path is on, else the neutral value.

### `rumble_hd_due()`

A controller that got nothing yet is at rest already, so the neutral value is not due for it.

## `runtime/input/input_hd_pair.h`

HD rumble for a pair of Joy-Con (`runtime/input/input_hd_pair.c`): the rumble output report written to each Joy-Con's
HID device, next to SDL, which reads the two as one gamepad and takes no rumble data for it. `runtime/input/input.c`
calls it (`hd_tick()`).

### `hd_pair_find()`

Looks for a left and a right Joy-Con among the HID devices and keeps their paths for `hd_pair_write()`; 1 when both are
there. Opens nothing. Any thread; it takes the time of a HID device listing.

### `hd_pair_write()`

Writes one rumble report to each Joy-Con: the left half of `data` to the left one, the right half to the right one. The
first call after `hd_pair_find()` opens the two devices. 0 when a device could not be opened or written (both are closed
then, and SDL's error text says why), and after `hd_pair_end()`. It blocks for as long as the writes take: the HD rumble
thread only.

### `hd_pair_is_open()`, `hd_pair_close()`, `hd_pair_end()`

`hd_pair_is_open()`: 1 while the two devices are open. `hd_pair_close()`: writes the neutral value to both Joy-Con, when
`neutral` is set, and closes them; `wait_ms` is how long to wait for a write still running on another thread before
giving up (the process is ending), 0 waits for it. `hd_pair_end()`: the process is ending (`input.c`
`input_rumble_hd_stop()`): `hd_pair_close(1, wait_ms)`, and `hd_pair_write()` writes nothing from here on, so a report the
HD thread chose before the stop cannot open the two devices again after it.

## `runtime/input/input_hd_pair.c`

HD rumble for a pair of Joy-Con (`runtime/input/input_hd_pair.h`). SDL presents a left and a right Joy-Con as one
gamepad and drives its two halves at fixed frequencies; the call that hands a Switch controller raw rumble data
(`runtime/input/input.c` `hd_tick()`, `SDL_SendGamepadEffect`) is refused for the combined gamepad. So for a pair this
file writes the rumble output report itself, through SDL's HID interface, to the two HID devices SDL also has open:

- Each Joy-Con is opened a second time (the system shares a HID device between handles) and gets output report 0x10
  (`runtime/input/rumble_hd.c` `rumble_hd_report()`), 49 bytes, the length of a Bluetooth output report. A Joy-Con by
  itself has no other connection: its USB charging grip is another product id and is left to the motors' path.
- The report carries a number that goes round 0..15. Each handle here counts its own; the controller plays a report
  whatever number it carries, so SDL's count for its own reports is not disturbed.
- While HD rumble is on, `input.c` makes no dual-motor call for the gamepad (`input_rumble()`), so these reports are the
  only rumble the Joy-Con get. SDL still writes its own commands (player lights, sensors on), which carry the rumble
  value SDL last set, the one at rest: such a command silences the actuators until the next report from here, at most
  `RUMBLE_HD_REFRESH_NS` later.
- A report holds a left and a right value and a Joy-Con plays the one of its side; both halves get the same value here,
  so nothing depends on which half a Joy-Con reads.

With several pairs connected the first left and the first right Joy-Con listed are taken.

State: `path_lock` guards `path`, the HID paths `hd_pair_find()` saw last (left, right). `dev_lock` guards `dev` and
`number` and is held across a write; `number` is the next report's number, per device. `ended`: `hd_pair_end()` ran, so
`hd_pair_write()`, under `dev_lock`, writes nothing.

### `close_devices()`, `write_side()`

Under `dev_lock`. `write_side()`: one report with `v` in both halves to device `s`; 1 when the whole report was taken.

### `hd_pair_write()`

`ended` is read under the write's lock: no stop comes between it and the write.

### `hd_pair_end()`

`ended` is set before the lock is taken: a write that holds it now is the last one.

## `runtime/input/motion_fusion.c`

Its first line is the licence notice of the code it contains, and stays.

Six-axis sensor states (`nn::hid::SixAxisSensorState`) from gyro and accelerometer samples. The input per sample is in
the Switch controller frame (x to the right, y toward the top edge, z out of the face): acceleration in G including
gravity, as gravity reads (lying face up z = -1 G), and the rotation rate in rad/s. The output per sample is as the
Switch reports it: acceleration in G, angular velocity in revolutions a second, angle = the integrated revolutions per
axis, direction = the controller's three axes in a reference frame whose z is up. The steps each sample goes through
(`motion_feed()`) are in [gyro.md](../gyro.md#what-each-sample-goes-through-motion_feed), with the defaults and why; in
short:

- Rest statistics: 0.2 s running means of the raw rate, of |acceleration| and of both readings' spread. The controller
  is still while both spreads are at sensor-noise level, |acceleration| is 1 G, the mean rate is small (a steady fast
  spin is not still) and, once the bias has been learned for 2 s, close to it (a bias changes slowly, a bigger step is
  motion). A smooth turn with little noise, slower than the small-rate limit, counts as still too. Still but for that
  step for 3 s means the learned bias was wrong: it is learned again from scratch.
- Bias: after 0.5 s still, the mean rate is the gyro's bias, learned fast (0.25 s) for the first 2 s and slowly (3 s)
  after, and taken off every sample (`MotionOptions.calibrate`). Each controller keeps its own (`motion_source()`).
- Dead zone: a corrected rate shorter than `MotionOptions.deadzone_rps` counts as none.
- Orientation: the rate integrated, pulled toward the measured gravity while the controller is not being accelerated
  (|a| within 15 % of 1 G) by a proportional term (`MOTION_KP`: about 2 s) and an integral term (`MOTION_KI`) that
  takes up a bias the gravity can see while the controller moves; it is cleared while the bias is being learned. Yaw has
  no reference: it follows the rate. Samples with a value that is not finite move nothing.
- Turn and tilt: the reported rate is the rate's part about the world's up (turn) and the rest (tilt), each times its
  factor (`MotionOptions`). The reported orientation is built from the filter's every sample: its heading about the
  world's up advances by turn times the filter's, its tilt away from up is tilt times the filter's, about the same axis.
  With both factors 1 it is the filter's own, bit for bit.

The newest `MOTION_RING` (64) samples are kept; reads are newest first. A restart (StartSixAxisSensor) drops the samples
before it: reads return nothing until the next sample, which starts a fresh frame and angle; the sampling numbers go
on. The feeding thread writes, guest threads read, both under the instance's lock (shared for reads). Log lines are
formatted under the lock and written after it (`Lines`, up to 3 a sample); each kind has a cap per process, all
instances together: `CAP_BIAS` 24, `CAP_SOURCE` 16, `CAP_NO_STILL` 16, `CAP_AGG` 48.

Constants:

- `STALE_NS` 200 ms: no sample for this long, no motion.
- `REST_TAU` 0.2 s: the rest statistics' running means.
- `STILL_GYRO_VAR` (0.015 rad/s)^2: the rate's spread summed over the axes; `STILL_ACC_VAR` (0.0173 G)^2, likewise;
  `STILL_ACC_G` 0.1: |mean |a| - 1 G|.
- `STILL_MAX_RATE` 0.15 rad/s: no bias is this large, a steady spin above it is motion. `STILL_BIAS_STEP` 0.035 rad/s:
  a mean this far from a learned bias is motion, not the bias moving.
- `RELEARN_MS` 3000: still but that far from the bias this long, and the bias is learned again.
- `SEED_VAR` 0.01: the spreads' value at a restart of the statistics, so nothing is still at once.
- `LEARN_AFTER_MS` 500: still this long before the mean is taken as the bias. `LEARN_FAST_MS` 2000: learned this long
  in all, and the slow time constant from then on. `LEARN_TAU_FAST` 0.25 s, `LEARN_TAU_SLOW` 3 s.
- `LEARN_LOG_MS` 1500: the `still_ms` at which a still period's learning is logged (1 s of learning).
- `EI_MAX` 0.1 rad/s: each component of the filter's integral.
- `NO_STILL_S` 30: fed this long with nothing learned, the hint to set the controller down.
- `AGG_FIRST_S` 60 and `AGG_EVERY_S` 300: the aggregate line after the first minute of samples and every 5 minutes after.
- `MOTION_CACHE` 4: the controllers whose bias is kept.

`MotionCached.used`: the switch count when the controller was last left; the oldest is replaced. `MotionFusion`:

- `first`: `produced` at the last restart; older samples are not read.
- `R`: the filter's orientation, controller to reference; row i is reference axis i in controller coordinates. `Rout`:
  the reported one, `R` with the turn and tilt factors applied (`report_orientation()`). `psi_prev`, `psi_out`: `R`'s
  heading at the last sample and the reported heading, in rad. `swing_axis`: the last axis `R`'s tilt was about, kept
  while upside down, where it has none.
- `bias`, `learned_ms`, `periods`: the gyro bias (rad/s) of the current source, its learning time and the still periods
  that learned. `ei`: the filter's integral term, rad/s.
- The rest statistics: `rest_valid`, the means and spreads, `still_ms`, `period_logged`; `near_still`: the last sample
  was still but for the bias-step test; `off_bias_ms`: how long it has been so.
- The source: `source_id`, `switches`, `name`, `source_fed_s`, `no_still_logged`, and `cache`.
- The aggregate's window: `fed_s`, `agg_next_s`, the `w_*` counts; `w_raw_mag`, `w_out_mag`, `w_raw_turn`,
  `w_out_turn` are sums over the still samples, in deg/s.

### `motion_new()`

The identity orientation is what a read shows before a sample it can align to.

### `orthonormalize()`

By rows, from the up row (the one the gravity correction keeps right).

### `align_to()`

A fresh orientation: z up against the measured gravity, x as close to the controller's x as that allows.

### `rotate()`

The body-frame rate `w` (rad/s, controller coordinates): R = R * exp([w] dt).

### `correction()`

The gravity pull, in controller coordinates: while the controller is not being accelerated (|a| within 0.15 G of 1 G),
the error between the measured and the estimated down feeds the integral and, times `MOTION_KP`, the correction; the
integral always does.

### `axis_angle()`

The rotation by angle a about the unit axis k: I + sin a [k] + (1 - cos a) [k]^2.

### `report_orientation()`

The reported orientation from the filter's, split as R = Rz(psi) * S: the swing S is the smallest rotation taking the
world's up in controller coordinates (u = R[2]) to e_z, by theta about the axis u x e_z; the twist Rz(psi) is the heading
about the world's up that is left. Rout = Rz(psi_out) * S', where S' turns about the same axis by theta * tilt and
psi_out advances by turn times each sample's change of psi. Built anew each sample, so the scaled orientation keeps to
the filter's: integrating a scaled rate would drift, since rotations do not commute. Upside down (u at -e_z) the axis is
not defined and the last one is kept; around face down the axis and psi turn fast with small motions, so with tilt != 1
the reported orientation can jump there, not only at the exact pole. `fresh`: the reported heading starts at R's. R S^T
= Rz(psi), whose first column is (cos psi, sin psi, 0): the rows of R times S's first row. The change of psi is wrapped
into [-pi, pi).

### `rest_update()`

Updates the rest statistics with one sample and returns whether the controller is still; `near_still` is the same test
without the step from the learned bias.

### `aggregate()`

The aggregate line: drift is the mean length of the rate over the still samples (raw: the sensor's, as the game got it
with no correction; reported: what it gets now). Turn drift is the mean of the rate's signed component along the world's
up (the filter's up, `R[2]`) over the still samples, times 60: how far a resting controller turns the aim sideways in a
minute. Dead zone held = the moving samples whose rate the dead zone zeroed.

### `motion_feed()`

- A gap of more than 50 ms (no controller, the sensors were off): no integration across it, the statistics start over,
  and the step is 4 ms.
- The rest statistics and the bias: a sample that is not finite leaves them as they are. Still in every way but far from
  the learned bias for 3 s: no motion is that steady, so the bias was learned wrong (a smooth slow turn taken for one).
  Learning starts over; else that bias would hold off every still period.
- While the bias is learned the integral is cleared: it took up the bias the gravity could see while the controller
  moved. With the bias now learned and taken off the rate, keeping it would correct the bias twice, and its part about
  the world's up, which no gravity can pull back, would turn the heading of a controller lying still.
- Then the corrected rate and the dead zone (the dead zone is in revolutions a second, so it is compared with the rate's
  length over 2 pi), then the orientation and the turn and tilt. A sample that is not finite moves nothing, and a
  restart waits for one it can align to. Direction i of the state is controller axis i in the reference frame.
- Last, the logs' counts.

### `motion_restart()`

`last_ns` is cleared, so the first sample after the restart integrates nothing across the stop. The rest statistics are
reset here: with `last_ns` cleared no gap is seen, and statistics from before a long stop would carry over.

### `motion_source()`

The leaving source's bias is kept: in its own entry, else an empty one, else the one left longest ago. A different
controller's sensors start the angle and direction over at its first sample, aligned to its gravity: the reads show a
step in an otherwise continuous stream, since nothing relates the old controller's heading and angle to the new one's.
The samples so far stay readable (no restart of the stream, unlike `motion_restart()`). After `CAP_SOURCE` lines one
more says that further changes are not logged.

## `runtime/input/motion_owner.h`

The player's model ownership for selective motion vectors (`runtime/input/motion_owner.c`).

### `motion_owner_observe()`, `motion_owner_capture()`

`motion_owner_observe()` records the model responsible for an address query at a recognised model-binding call site.
`motion_owner_capture()` captures the identities next to the submission's uniform bytes, before the asynchronous
replay.

## `runtime/input/motion_owner.c`

The supported game build exposes a player equipment component with generation-checked actor links. Selection follows
those links and the equipment's model-binding parent; proximity is never an identity.

### `owner_init()`

`NX2SARIA_SELECTIVE_MOTION` (`selective_motion`; default 1, 0 off: the estimate from colour and depth). On, every guest
submission walks the player's actor, equipment and models in guest memory (`motion_owner_capture()`), each scene draw
is looked up in that list (`runtime/vk/vk_fg_pose.inc` `pose_owner()`), and the player's meshes are drawn a second time
into the motion vectors DLSS, FSR 3.1 and frame generation read.

### `actor_link()`

A slot replaced during the read is rejected (the link and the table's generation are read again). This is not a
replacement for the game's actor locking: unknown layouts and any observed disagreement fail closed, and the run-time
lifetime checks remain required.

### `equipment_flags()`

This classification is reached only for a generation-checked actor attached to the player, so a matching resource
elsewhere in the world cannot acquire this policy. Head armour is deliberately excluded: its cap can deform
independently of the attachment.

### `collect_models()`

Only the implementation whose getter returns its model collection directly is decoded (the getter at +0xf8 is
main+0x9d7160), and only the model implementation used by the traced skeleton and shape binding path (+0x40 is
main+0x735430).

### `refresh()`

The equipment has eight static and thirteen dynamic slots, each a 24-byte actor link.

### `motion_owner_observe()`

Both inline binding paths keep the shape object in their stack frame: the saved pointer is read rather than temporary
registers reused during binding. An entry is never evicted during this producer frame: a collision must not erase an
ambiguity.

### `motion_owner_present()`

On by default, so the summary line comes every 7200 presents: every minute or two, not every two seconds.

## `runtime/input/mouse_direct.c`

Direct mouse input for 1.4.3. The camera controller's input update (main+0x874c1c) turns the HID stick floats into a
radial dead zone, clamps the radius to one and applies StickExponent. The mouse enters after all three, at main+0x874cb4,
before the game's X/Y inversion and input-activity flags. It never becomes a HID stick value. Free orbit removes that
sample again before the camera's additional vertical square and velocity filters, and adds raw-count angular deltas at
its spherical-angle output instead. Object manipulation has a separate post-curve hook. The ordinary stick's result
stays, so a controller still uses the game's own response.

This is deliberately not a render-camera override: the game remains responsible for aiming, camera collision, lock-on
and scripted cameras. Which of its other modes consume this camera input needs in-game verification. The player's side:
[controls.md](../controls.md#mouse-camera).

The host call sites are on the patch page: `HOST_MOUSE_DIRECT` +0xfd0, `HOST_MOUSE_ACTIVITY` +0xfc0,
`HOST_MOUSE_ORBIT_BEGIN` +0xfb0, `HOST_MOUSE_ORBIT_ANGLES` +0xfa0, `HOST_MOUSE_HAND` +0xf90. `MouseSample.pad` is the
controller input after the inversion, `.motion` the mouse input, `.angle` the mouse's yaw and pitch in degrees.

### `direct_mode()`

`cfg_value`: unset is the settings table's default (direct), as the menu shows it.

### `input_direct_mouse_row()`

Reversing direction cancels any remainder from the previous drag. A fast flick is bounded to one pending step (two
steps of 32 counts); a stopped mouse never starts another pulse, however often HID is polled. A pulse and the pause
after it last 32 ms each, and the neutral is published even when a slow frame skipped the pulse's deadline.

### `orbit_track()`

`sample_lock` held. Camera objects can coexist while the game blends between their outputs (16 are tracked).

### `orbit_pitch_offset()`, `orbit_pitch_target()`, `orbit_pitch_release()`

main+0x191d78 builds the pitch target from +0x15c and +0x11c; the orbit update filters +0x108 toward it and adds
(1 - +0x88) * +0x10c at output. Updating only +0x108 makes the camera spring back to the unchanged target.

### `camera_input()`

A host call at an instruction site, not an AAPCS64 function replacement. It keeps every guest register except s9 and
s10, which carry the processed camera input, and makes the displaced store of s9 at [x20 + 0x420]. The game multiplies
camera input by its 30 Hz frame ratio later. The mouse is divided by that exact ratio rather than by wall-clock time, so
an equal distance of mouse motion gives equal input at every frame rate. Frame generation's frames are not the game's
(`frame_step_game_hz`).

The counts go through `runtime/input/input.c` `input_mouse_turned()` (the player's inversions and vertical factor) before
the motion, the angles and the stick values are made from them.

### `orbit_begin()`

Free camera has another signed-square curve for its vertical axis, plus mode-dependent velocity filters. Only this mouse
sample is removed before that controller runs. At its final spherical-angle output the degrees are added directly, and
its persistent pitch and yaw move by the same amounts, so the next update starts at the new orientation. The controller
still handles its gamepad input, following, collision and focus normally. The hooks take a step when the mouse moved in
it, or while the mouse holds the pitch. A step of the gamepad alone (no mouse motion, no hold: a player who never moved
the mouse) keeps the game's input. When the mouse moved, its part of this step's stick goes to the final angles instead.
The displaced instruction is `ldr x8, [x0, #8]`.

### `orbit_angles()`

The game can recalculate its pitch after `orbit_begin()` (following, mode changes and another filter), so the pitch is
integrated from our last requested angle and the final output corrected even on stationary frames. The pitch is held
within -89.9 to 89.9 degrees. The displaced instruction is `ldr s12, [sp, #0x44]`.

### `hand_input()`

Object manipulation reads the player's manipulation input rather than the camera's processed stick. The mouse is added
after that controller's per-axis power curves (main+0x1d59460), keeping the keyboard and controller contribution.

### `camera_activity()`

The game also uses a 10% activity threshold after processing the stick. A one-count mouse movement must count as
active, even when its displacement is smaller than that threshold. The site is the displaced `ldp d9, d8, [sp, #8]`
just before the update's last `str w8, [x20, #0x418]`: w8 holds the flags then (bit 0 raw input, bit 4 any input, bits
3 and 5 processed input below the threshold), so they are changed in w8; a write to +0x418 would be overwritten by that
store. No flag is changed for controller-only input.

### `mouse_trace()`, `camera_input_oot()`, `camera_activity_oot()`

Ocarina 1.1.1 (`recomp/patches-v111.txt`, set `mouse_direct`). Its camera input update is main+0x37c300: the same steps as above with other registers and offsets. At the site s1 and
s0 hold the processed x and y (both zero while the camera-reset timer at +0x5e0 runs), x20 the input object and x21 the
flags (bit 0 camera input off, bit 1 invert x, bit 2 invert y). The two stores, the inversion and the idle test that
follow read s1 and s0, so the counts added here go through them as the stick's part does.

- `OOT_STICK` 0x538: the processed stick, x, and y at +0x53c.
- `OOT_RESET_TIMER` 0x5e0: the 60 Hz frames left of the pause after a camera reset (the right stick's click).
- `OOT_FLAG_IDLE` 0x10: in the flags (+0x530), the processed stick is under the game's 10% idle threshold.

`mouse_trace()`: `NX2SARIA_MOUSE_TRACE=1` (test runs), one line per camera input update, with the stick the update made,
the counts taken and what they added. `runtime/vk/vk_dlaa.c` adds the camera's direction per frame, so a scripted run
gives degrees per count.

`camera_input_oot()`: the game multiplies the processed stick by the frames it steps and by its camera speeds, which
`NX2SARIA_STICK_SENS` scales (`runtime/input/camera_speed.c`): divided by both, a count turns the camera by the same
angle at every frame rate and stick speed. Nothing after the site clamps the sum, so a fast flick is not cut short. The
displaced instruction is `str s1, [x20, #0x538]`. The counts go through `input_mouse_turned()` first, as in
`camera_input()`: the player's inversions and vertical factor come before the game's own inversion of the flags, which
the player sets in the game's options.

`camera_activity_oot()`: the idle test follows the first site and already counts the mouse's part, but one slow count
adds less than the game's 10% threshold: an update the mouse moved in is not idle. The site is the displaced
`ldp d9, d8, [sp, #0x10]` before the update's last `str w8, [x20, #0x530]`, with the flags in w8.

### `mouse_direct_register()`

Ocarina has two sites of its own and no orbit or held-object site.

## `runtime/input/camera_speed.c`

The camera speed: `NX2SARIA_STICK_SENS` (0.1 to 3, default 1) multiplies the game's own right-stick camera speeds. The
game's Options menu offers five camera sensitivities, Very Low to Very High; their turn rates are the array
StickSensitivity {0.8, 1.2, 1.6, 2.0, 2.4} in
`Component/PlayerCameraParam/PlayerCamera.game__component__PlayerCameraParam.bgyml` inside
`Pack/Actor/PlayerCamera.pack.zs`. `runtime/hle/romfs.c` serves a copy of that pack with every entry multiplied
(`cam_pack_build()`), so the setting raises the camera's top speed as well as its speed at a partial tilt, at any of the
five sensitivities. The mouse camera reaches the game as a right-stick deflection (`runtime/input/input.c`
`mouse_camera()`), divided by the same factor (`cam_speed_applied()`) before it is limited to full tilt
(`input_mouse_stick()`), so `NX2SARIA_MOUSE_SENS` alone sets the mouse's turn per movement, and its top speed is the
game's with the factor in it.

The pack is a SARC archive compressed with the dictionary `pack.zsdic` of `Pack/ZsDic.pack.zs`. A float in a BYML array
is stored in the array itself, so the multiplied values take the bytes of the old ones and no offset moves.

During a run (`cam_speed_tick()`) the pack served stays the one made at start, and the game has read it by the time the
player changes the setting. So a change is written into the game's memory instead: the array as the game holds it (the
five values in a row, times the factor in effect) is looked for in the guest's heap and every copy found is overwritten
with the values times the new factor. The game reads the array each time it steps the camera, so the camera's speed
follows at once. When the game opens the pack again (`cam_pack_opened()`, from `runtime/hle/romfs.c`), what it reads
holds the start's factor again, and the search runs once more a moment later.

State: `applied`, the factor the game's values in memory carry (what `cam_speed_applied()` reports); `base`, the game's
own StickSensitivity values, from the pack as it is before multiplying, and `base_n` how many (0 until
`cam_pack_build()` has read the pack, or when it could not); `served_k`, the factor of the pack the game reads from the
RomFS, the start's. During a run: `copies` (at most `COPIES`, 16), where the game's memory held the array at the last
search; `searching`, the search thread runs; `reopened`, the game opened the pack since the last search
(`cam_pack_opened()`); `due_ns`, when `cam_speed_tick()` starts the next search, 0 none due; `tries`, the searches left
after this one that found nothing; `asked`, the factor the last search was started for.

### `scale_sensitivity()`

The StickSensitivity floats of the BYML `p[0..n)`, multiplied by `k`; their count, 0 when the file has no such array.
The key's offset is added in `size_t`: a mod's key offset near 2^32 would wrap the 32-bit sum below `n`.

### `cam_pack_build()`

The frame names its dictionary (`pack.zsdic`) by id; `Pack/ZsDic.pack.zs` holds them. The original values are kept for
what a change during the run looks for in memory (`cam_speed_tick()`). At factor 1 the game's own pack is served as it
is.

### `factor()`

The array at factor `k`, as `cam_pack_build()`'s multiplication gives it.

### `rewrite()`

A pass over the guest's heap and alias regions for the array at factor `k`, byte for byte, in memory the game may read
and write (not free); each copy found becomes the array at factor `to` and is remembered (`copies`, when `fresh` is
set). Returns how many were rewritten. It runs on its own thread: the regions are read 16 MiB at a time (`PIECE`) under
`mem_read_hold`, so the game's own mapping calls wait for one piece at most. Heap pages the lazy heap never committed
hold nothing and are skipped.

Pages the lazy write-back has armed (GPU-written buffers the CPU has not read since, never the camera table) are left
alone: a read would fetch each armed range from the GPU inside the fault handler (`runtime/memory.c` `lazy_fault()`)
while the game runs, and hold the game's mapping calls meanwhile. A piece that holds some is gone through in `STEP`
(64 KiB) pieces, without the armed ones; the first search that leaves some out says so, since with
`NX2SARIA_WB_PROBE` every piece counts as armed and the search then finds nothing. A copy found may be on a
write-protected page of a tracked region, which is thawed first. A copy across two pieces is found because the next
piece starts a little before this one's end.

### `rewrite_known()`

The copies of the last search, still holding the array at factor `k`, rewritten to factor `to`: 1 when every one of them
did (a step of the menu's slider then takes no search), 0 when one is gone or changed, with none of them touched (all
are checked first, then all written).

### `search_main()`

What the game holds: the array at the factor in effect, and at the start's after it read the pack again (new copies
beside the ones known).

### `cam_speed_tick()`

Every present (`runtime/gfx/gfx_present.c`). A factor other than the one in effect goes to the copies the last search
found at once; without them (the first change of a run, or the game moved them) a search starts, and a few more (3, 4 s
apart) while one finds nothing (the game is still loading). A moment (1.5 s) after the game opened the pack again a
search looks for the start's factor, which is what the game has then read. No search starts while one runs: a search
writes `applied` and the copies.
