# runtime/ui

The in-game menu: the settings pages and their draft, the menu's look from the game's own textures, fonts and sounds,
the bindings page, the Amiibo page and picker, the Cheats tab, the debug menu's panel, the performance overlay's panel,
the host's folder dialogs, the mouse in the game's own menus, Quit Game in the game's own menus, and the catalog of the text the host shows. The feature
docs:

- [controls.md](../controls.md): the menu, its tabs, the draft and Apply, the bindings.
- [menu-mouse.md](../menu-mouse.md): the mouse in the game's own menus, how the game's objects were found.
- [quit-menu.md](../quit-menu.md): Quit Game in the game's own menus.
- [pause-settings-tab.md](../pause-settings-tab.md): the settings tab in the game's pause menu.
- [amiibo.md](../amiibo.md): the Amiibo tab, the picker, the keys and write-back.
- [performance.md](../performance.md): the performance overlay.
- [presets.md](../presets.md): the quality preset the menu stages.
- [translating.md](../translating.md): the menu's strings and their translations.
- [logging.md](../logging.md): the `menu:`, `menu_mouse:` and `quit_menu:` lines.

"The settings file" on this page is the selected one, `nx2saria.ini` or the older name that still works
([branding.md](../branding.md)). "The game" is the game the port runs; where two game versions differ the version is
named (1.1.1, 1.4.3).

## `runtime/ui/menu.h`

The in-game menu (`runtime/ui/menu.cpp`, Dear ImGui) and its seams to the platform side: the window and its input
(`runtime/ui/menu_win.cpp` on Windows, `runtime/ui/menu_sdl.cpp` with SDL), the gamepad (`runtime/input/input.h`
`input_pad_read()`) and the present path (`runtime/gfx/gfx_present.c`). The menu core calls no window system: events
come in through the `menu_event_*` functions, and the drawing happens in the present path's command buffer.

F1 or Esc on the keyboard, and Minus and Plus held together on the pad (consumed in `runtime/input/input.c`
`poll_merged()`; `runtime/input/input.h` `input_menu_gate()`), open it. F1, Esc, B or *Continue* close it; with
changes not applied yet (the draft, `runtime/ui/option_rows.h`) they open *Apply changes?* first. `menu_toggle()` on
another thread only raises a request that the present thread answers. While the menu is open the game gets no input
(`input_menu`) and keeps running; the cursor is released from the mouse camera. The tabs Screen, Game, Sound, Controls,
Amiibo and Cheats (L / R or Q / E switch them) hold option rows (`runtime/ui/option_rows.h`) over the settings; the
bottom row has *Continue* and *Quit game*.

Test hooks (`runtime/ui/menu.cpp`): `NX2SARIA_MENU_TEST=<frame>[:<frames per page>]` opens the menu at that presented
frame and saves one swapchain shot per tab into `NX2SARIA_MENU_TEST_DIR`;
`NX2SARIA_MENU_TEST_SET=<frame>:<key>=<value>[,...][;...]` sets settings at given frames without input (`SAVE=1`
writes the settings file, `FOCUS=<key>` puts the cursor on that setting's row).

Threads: the event functions are called by the window thread, `menu_toggle()` by whichever thread polls the pad, the
overlay functions by the present path under its lock. ImGui itself runs on the present path only.

### `menu_is_open()`, `menu_toggle()`, `menu_cfg_reload()`, `menu_perf_cycle()`

State, callable from any thread. `menu_toggle()`: F1 / Esc on the keyboard, Minus+Plus on the pad; not before the
game's first frame. `menu_cfg_reload()`: Ctrl+F5, the settings file is read again on the present thread, with a short
notice. `menu_perf_cycle()`: F10, the performance overlay off, fps, full.

### `menu_event_key()`, `menu_event_mouse_button()`, `menu_event_char()`, `menu_event_paste()`

Events from the window adapter, used only while the menu is open. `key` is an `ImGuiKey`; coordinates are in pixels of
the window's client area, which is the swapchain image. Mouse buttons: 0 left, 1 right, 2 middle.
`menu_event_char()`: a character typed, a Unicode code point (a text field takes it). `menu_event_paste()`: the
clipboard's text `gfx_window_paste()` read (`""` for none), on the window thread.

### `menu_debug_enabled()`, `menu_debug_toggle()`

`NX2SARIA_DEBUG_MENU`: F9 belongs to the debug menu (`runtime/ui/debug_menu.cpp`), and `menu_debug_toggle()` is F9.

### `menu_paste_take()`

The text the last `gfx_window_paste()` brought, once: 1 and the text in `out` (`cap` bytes), 0 while none has arrived.

### `menu_overlay_frame()`

The overlay is drawn into the swapchain image after the game image (`runtime/gfx/gfx_present.c`, present lock held).
`MenuTarget` describes it: `queue`'s submissions take `vk_queue_lock`; `format` is the swapchain image's, a colour
attachment; `image_w` and `image_h` are the game image this present shows (its crop); `scaler` is how the last present
scaled it to the window: 0 not, 1 bilinear blit, 2 FSR 1, 3 bilinear shrink + FSR sharpening (RCAS).

`menu_overlay_frame()` lays out this frame's overlay; 1 when there is something to draw. `frame`: the presented frame
(0 unknown). `*shot`: a path when the test hook wants this frame's swapchain image saved, else NULL. The first open
starts ImGui's Vulkan back end and uploads the font. With the menu closed it costs two loads and a compare.

### `menu_overlay_draw()`

Records the frame `menu_overlay_frame()` laid out into `cb`, which is inside dynamic rendering on the image.

### `menu_boot_set()`, `menu_boot_frame()`

The boot screen while `runtime/boot.c` waits for the prewarm: `phase` as `VkPrewarmProgress.phase` (0 starting, 1
shaders, 2 pipelines, 3 compute pipelines, 4 the driver's cache) with its count. `menu_boot_frame()` lays out the
screen for the target like `menu_overlay_frame()` (1: draw it with `menu_overlay_draw()`); only before the guest runs,
from the thread that presents then.

### `menu_vk_release()`

Called before the device goes away (`gfx_window_release_vk`).

### `menu_font_check()`

The check the font atlas runs on a system font, on the embedded font and damaged copies of it: 1 when it takes the
whole font and refuses a cut-off one, one without a character map and one with an unknown tag (`--probe-window`).

### `menu_text_input_follow()`

The window's text input follows the menu's text fields (`runtime/ui/menu_text_input.cpp`): `menu_overlay_frame()`
calls this on each of its frames, those of a closed menu too, with whether the open menu has a field taking typing,
and it calls `gfx_window_text_input()` when that changes.

### `gfx_window_apply_settings()`, `gfx_window_request_close()`, `gfx_window_request_restart()`, `gfx_window_text_input()`, `gfx_window_paste()`, `gfx_window_monitor_size()`, `gfx_window_refresh_hz()`, `gfx_window_dpi()`, `gfx_window_present_mode()`, `gfx_output_fps()`

What the menu asks of the platform side (`runtime/gfx/gfx_window_win.c`, `runtime/gfx/gfx_window_sdl.c`):

- `gfx_window_apply_settings()`: `NX2SARIA_FULLSCREEN` or `NX2SARIA_WINDOW_SIZE` changed, and the window follows.
- `gfx_window_request_close()`: as the window's close button; the process ends.
- `gfx_window_request_restart()`: the same, then the game's executable starts again with the same command line.
- `gfx_window_text_input()`: a menu text field is active (on) or no longer. The window delivers typed characters to
  `menu_event_char()` meanwhile, and a handheld shows its keyboard.
- `gfx_window_paste()`: reads the clipboard on the window thread and hands it to `menu_event_paste()`.
- `gfx_window_monitor_size()`: pixels of the monitor the window is on; 0 when unknown.
- `gfx_window_refresh_hz()`: its refresh rate (`NX2SARIA_FPS_LIMIT=display`); 0 when unknown.
- `gfx_window_dpi()`: the window's DPI (96 at 100% display scale); any thread.
- `gfx_window_present_mode()`: the present mode the swapchain got, as a `NX2SARIA_VK_PRESENT` value (`fifo` when
  mailbox is missing); `""` before the first swapchain. Any thread.
- `gfx_output_fps()`: frames the display gets per second with frame generation (generated ones too); 0 when off.

## `runtime/ui/menu.cpp`

The in-game menu core: Dear ImGui state, the event queue the window adapter fills, gamepad navigation, the Vulkan back
end on the game's device and the menu's pages. It is free of any window system (`runtime/ui/menu.h`). ImGui runs on
the present path only, under the present lock of `runtime/gfx/gfx_present.c`; the event functions only queue.

The Vulkan set-up: dynamic rendering into the swapchain image, an internal descriptor pool, vertex buffers rotated
over more frames than are in flight. The look: colours, rounding, the tab bar with L / R and Q / E, the window over a
dimmed game. The pages are rows of `runtime/ui/option_rows.cpp`. What the player sees: [controls.md](../controls.md).

Includes and declarations:

- `imgui_internal.h` is included for `SetFocusID` and `NavCursorVisible`.
- The file holds a private copy of stb_truetype, the parser Dear ImGui builds its font atlas with, to check a system
  font before the atlas gets it (`font_usable()`). Its asserts only mark the font bad here (`g_font_bad`).
- `gpu_choice.h` is plain C and C++, so it is included without `extern "C"`.
- `host.h` and `vk.h` pull in `cpu.h`, which is C only, so the file declares the functions the menu needs from them,
  as they are declared there. Among them: `vk_ngxfg_wanted` (`runtime/vk/vk_dlaa.c`: DLSS's frame generation through
  NGX itself can run in this run) and `vk_ngxfg_max` (how many frames it may generate per rendered one; 0: it cannot);
  `vk_ffx_available` (`runtime/vk/vk_ffx.cpp`: FSR 3.1 upscaling can run, asked without loading the library) and
  `vk_ffx_framegen_available` (and FSR 3.1 frame generation); `vk_ffx4_available` (`runtime/vk/vk_ffx4.cpp`: AMD's
  FSR 4 runs on this device: its libraries, card and driver); `vk_ffx_fsr4` (`vk_ffx.cpp`: and the setting asks for
  it, `NX2SARIA_FSR4`); `vk_ffx4_retry` (FSR 4 chosen again after failures that turned it off); `vk_ffx4_name`
  ("AMD FSR 4.1.2" once the driver's library was asked); `vk_fg4_available` (`runtime/vk/vk_fg4.cpp`: the frame
  generation of AMD's FSR 4 library runs on this device), `vk_fg4_failed` (it failed in this run, and FSR 3.1's
  generates the frames), `vk_fg4_active` (its swapchain is in place), `vk_fg4_name` (the version the library runs
  here: "3.1.6", "4.0.2"); `vk_texture_detail_auto` (`runtime/vk/vk_texture.c`: the texture detail auto picks on this
  card, 0 full to 2 low); `vk_card_usable` (`runtime/vk/vk_device.c`: the game can run on this card; the Graphics card
  row's discrete ones); `vk_exec_replay_workers_auto` (`runtime/vk/vk_exec.c`: the count `replay_workers = auto` gives
  on this PC); `cheats_frame` (`runtime/patches/cheats.c`).
- The embedded font is Roboto Medium (Apache-2.0, `runtime/ui/fonts/LICENSE-Roboto.txt`) with Latin and Cyrillic
  glyphs. The file comes in with `#embed` where the compiler has it (clang 19 and later define `__has_embed`); the
  `-Wc23-extensions` warning is silenced for `#embed` in C++. An older compiler, which a Linux build with the
  distribution's compiler can meet, includes the same bytes as numbers from `roboto_ttf.h`, which
  `tools/build_shaders.py` writes beside this file from the font when the game is built (a kit carries it).

Open state, readable from any thread:

- `g_open`: the menu is open. `g_reset`: opened or closed since the last laid-out frame, so stale input is dropped.
- `g_open`, the input block and the log line change together under `open_lock`, so they cannot disagree when the
  window thread (F1, Esc), the polling thread (the combo) and the present path (Continue, B) race. `ev_lock` guards
  the event queue.
- `g_debug`: the debug menu (`runtime/ui/debug_menu.cpp`; F9 with `NX2SARIA_DEBUG_MENU = 1`) is laid out in this
  menu's place. Open is the one state for both, with its input, its sounds and the game's blocked pad, and `g_debug`
  says which window it shows.
- `g_picker`: the amiibo picker (`runtime/ui/amiibo_page.cpp`) is laid out in the menu's place the same way, when the
  game starts waiting for an amiibo; F1 and the pad's combination close it like the menu.
- `g_close_req`: leaving the menu with a draft of changes asks the player first (`layout_menu()`'s `##apply`). The pad
  combo and F1 arrive on other threads: they raise `g_close_req`, and the present thread, which owns the draft,
  answers it.
- `g_booting`: set until the game's first frame (`menu_overlay_frame()`), boot screen included. The menu does not open
  then, as it would not be drawn while its keys queued up and changed settings once the game ran.

ImGui and its Vulkan back end (present path): `g_ctx_ok`, the context is made; `g_failed`, the set-up failed for good
(the menu stays closed); `g_vk_ok`, the back end is initialised for `g_vk_fmt`; `g_font_scale`, the scale of the
uploaded font atlas (0 when none is uploaded); `g_style_scale`, the scale the style was last sized for;
`g_base_style`, the menu's style at 1080 lines; `g_cost_ns` and `g_cost_frames`, the CPU time of layout and recording
while open, logged per 600 frames. `kGold` is the gold of the title and its Triforce (the rest of the menu's colours:
`runtime/ui/menu_skin.cpp` `skin_style()`).

### `change_open()`

`want` 0 or 1 sets the state; -1 flips it; 2 opens the menu as the pause menu's settings tab, unless it is already open
(then it returns 0 and nothing changes), so the mode and the open state change together under `open_lock`. Returns the new state. The window events queued before the change go with
it: keys pressed just before a close would otherwise reach the menu when it next opens.

`g_pause_tab`: the menu is open as the pause menu's settings tab (`runtime/ui/pause_tab.c`). The change tells
`pause_tab_set_open()` with `g_pause_why`, the reason it closed, plays the tab sounds instead of the open and close
ones, and a close ends the tab mode.

### `menu_toggle()`

With nothing open, F1 opens the settings (`g_debug` and `g_picker` are cleared); with a window open it closes it. The
settings menu with a draft is not closed here: the present thread asks what to do with it (`g_close_req`).

### `menu_debug_toggle()`

F9: opens the debug menu, and closes it again; with the settings menu open it does nothing.

### `push()`

Events from the window adapter are queued while the menu is open (at most `EV_MAX`, 256); the present path hands them
to ImGui. Whether the menu is open is checked again under the lock: `change_open()` empties the queue after the state
changes, so an event of the closed menu cannot slip in behind that. Of consecutive pointer positions only the newest
is kept.

### `menu_event_char()`

A typed character, for the text fields of the debug menu.

### `menu_event_paste()`, `menu_paste_take()`

The clipboard's text a page asked for (`gfx_window_paste()`): the window thread reads it, the page takes it a frame
later.

### `drain_events()`

ImGui takes the modifier state (`io.KeyShift`, `io.KeyCtrl`: Shift+Tab, Ctrl+Up) from `ImGuiMod_Shift` and
`ImGuiMod_Ctrl` key events only, so the left Shift and Ctrl keys are sent as those too.

### `stick_to_dpad()`

Gamepad navigation maps the Switch buttons of the SDL3 gamepad to ImGui's gamepad keys (`pad_keys[]`). Nintendo
layout: A (east) activates, B (south) cancels. `pad_prev` holds the buttons at the last frame; a button held when the
menu opened counts once released. `stick_dir` is the D-pad button the left stick holds, -1 for none.

The left stick moves the cursor as the D-pad does (ImGui's own stick handling only scrolls the page): past half way
it holds the D-pad direction of its larger axis, kept until that axis falls under 0.35; ImGui repeats a held one. The
stick's y is up.

### `menu_scale()`

Sizes are tuned for 1080 lines and follow the window's height, or its width in a window narrower than 1.47:1, where
the menu's 1400 pixels at 1080 lines would no longer fit 88 % of the width (`layout_menu()`). Under 720 lines at 100%
display scale they stay at the 720 size; that floor grows with the window's DPI, so a small window on a 150% or 200%
display (sizes are physical pixels) still gets text as large as the display scale asks for.

### `triforce()`

The Triforce, its top at (x, y), `h` high, drawn with the title. It is equilateral: width = height * 2 / sqrt(3)
(1.1547).

### `make_context()`

The window switcher on Ctrl+Tab is turned off: the menu is one window, and the switcher would list its inner windows
by their ids.

### `menu_vk_check()`

Dear ImGui's Vulkan back end calls this with each `VkResult` it gets (it went unchecked before): failures are logged
and counted (`g_vk_errors`), so a texture whose descriptor set was not allocated is not drawn with
(`runtime/ui/menu_art.cpp` `upload_rgba()`).

### `vk_up()`

- When the back end is up and the swapchain changed format, the pipelines are made for the new one.
- `DescriptorPoolSize` is 32: the art's sets, and two font atlases while one replaces the other.
- `ImageCount` is 4: vertex buffers rotate per recorded frame, and that is more than the 2 frames in flight of
  `runtime/gfx/gfx_present.c`.

### `font_usable()`

1 when Dear ImGui can take this font file: more than the 100 bytes `AddFontFromMemoryTTF` asserts on, every table of
its directory inside the file (stb_truetype reads tables without bounds checks, so a cut-off file faults when the
glyphs are made), and stb_truetype parses it. ImGui asserts on a font it cannot parse, and an assert ends the process
with 0xc0000409 and no line in the log.

### `menu_font_check()`

The damaged copies it makes: one with the `cmap` tag zeroed (no character map) and one with the first 4 bytes zeroed
(an unknown font tag), beside the cut-off one.

### `add_row_glyphs()`

The glyphs of every string of a catalog row (`runtime/ui/ui_text.c` `ui_text_visit()`), for the font that draws that
language.

### `fonts_up()`

Roboto Medium (Latin, Cyrillic, dashes and quotes; `kRanges[]`: 0x0020-0x00FF, 0x0400-0x052F, 0x2010-0x2027) at 26 px
per 1080 lines, with the glyphs of the Japanese, Korean and Chinese language names merged in from a system font when
there is one (each language is named in its own script; `kCjk[]`). A catalog row also holds menu text (Simplified
Chinese), so every string of the row goes into the builder: the merged font must cover the whole menu of that
language, not only its name. It is made again when the window height changes; the old texture goes after the queue is
idle (`runtime/ui/menu_art.cpp` `menu_font_upload()`).

The game's own interface fonts from the user's RomFS (`runtime/ui/menu_assets.c`) are read once: ClarimoUDPE for the
menu's text (`g_game_font`), CetraDisplayPro for its title and tabs (`g_title_font_data`). Roboto stays merged behind
them for the glyphs they lack; without them the menu keeps Roboto alone.

- Text the menu shows that no catalog row holds, a mod's cheat names and its folder name, both read from disk at
  runtime, is added to the builder too. Their glyphs would otherwise fall back to the missing-glyph box.
- The title font is added after the text font's merges, which go to the font added last.
- What the title face shows in the CJK languages: the menu's title and tabs (Simplified Chinese menu text) and the
  boot screen's title (the game's language).
- `skin_atlas_add()` reserves the boxes' grain tile (`runtime/ui/menu_skin.cpp`), drawn into the atlas once it is
  built; `skin_atlas_fill()` builds the atlas, which the upload then takes as it is.
- ClarimoUDPE's line (ascent to descent, which is all Dear ImGui spaces lines by) leaves wrapped lines touching: the
  line height grows by a sixth over the glyphs' size (`FontSize *= 1.16`), the glyphs stay as rasterized.
- After the upload `ClearInputData()` frees the input: the glyphs are in the texture, and the font files and the
  pixels are not needed again.

### `fonts_for()`

The atlas for a scale. While the scale keeps changing (the window being resized), the atlas is made again only once it
has held for a quarter of a second, and the old one is drawn scaled meanwhile (`io.FontGlobalScale`): each atlas reads
the system fonts from disk and waits for the queue to upload. 0 when no atlas could be made.

### `test_parse()`

The test hooks:

- `NX2SARIA_MENU_TEST=<frame>[,<frame>...][:<frames per page>]`: at each such presented frame (up to 8,
  `TEST_OPENS`) the menu opens on its first tab; each tab stays `<frames per page>` frames (default 30,
  `TEST_PAGE_FRAMES`) and its last frame's swapchain image goes to `NX2SARIA_MENU_TEST_DIR` (default: the working
  directory) as `menu-test-<frame>-<page>.png`; after the last tab the menu closes.
- `NX2SARIA_MENU_TEST_CLICK=<key>:<part>[,...]`: mouse clicks on the rows of the page shown
  (`runtime/ui/option_rows.cpp` `click_test()`).
- `NX2SARIA_MENU_TEST_SET=<frame>:<key>=<value>[,<key>=<value>...][;<frame>:...]`: `cfg_set` of each key (the
  `NX2SARIA_` prefix optional) at that presented frame (up to 16 sets, `TEST_SETS`); it changes live settings with no
  input, menu open or not. The groups `<frame>:<key>=<value>,...` are separated by `;`. The values are the test's
  (`cfg_set_test`): no save writes them, the save at exit included, and the settings file takes them only from the
  pseudo-key `SAVE=1` (`cfg_test_keep`, then `cfg_save`, as a row change does; a failed write shows the menu's save
  note). `FOCUS=<key>` (the `NX2SARIA_` prefix optional) puts the cursor on that setting's row and scrolls it into
  view when the row is next drawn (`opt_focus_key()`); for a bind cell, use `BIND_<action>#<cell>` with cell 0..2.
  `FOCUS` does not switch tabs.

`g_test`: `at[]` holds the `NX2SARIA_MENU_TEST` frames, in order; `n_at` how many and `next_at` the next to open at;
`opened` the frame the hook opened the menu (0 not yet); `page` the tab being shot (-1 none); `n_sets` and `next_set`
the `NX2SARIA_MENU_TEST_SET` groups, in frame order.

### `tabs_shown()`

The tabs are the options this port has for this game.

How many of the tabs the menu shows: all of them while `runtime/patches/cheats.c` `cheats_count()` is above 0, else
the ones before Cheats, so a player without cheat mods sees Screen, Game, Sound, Controls and Amiibo. Cheats is the
last tab of the enum for that reason: the tabs shown are the first `tabs_shown()` of it. The count does not change
while the game runs (the cheat files are read once, at start, before the first frame). `layout_menu()` steps through
that many tabs with L / R and Q / E and draws that many, and the test hook in `overlay_frame()` shoots that many pages;
nothing else may use `TAB_COUNT` as a count of tabs shown (`tests/cheats_tab_check.py`).

The Cheats tab's name is among the texts whose glyphs the title face gets in the CJK languages (`fonts_up()`).

### `test_apply_sets()`

Two pseudo-keys of `NX2SARIA_MENU_TEST_SET` beside `SAVE` and `FOCUS`, for a run with no input that must show what a
draft does:

- `LEAVE=1`: with lines in the draft, asks to leave the menu as F1 does, so the *Apply your changes?* box opens with
  the draft's lines and the page's shot shows it. The log has `menu: box "Apply your changes?" opened: N line(s)` and a
  `menu: box   <label>: <from> -> <to>` line each. With nothing in the draft it only logs
  `menu: test hook: LEAVE with nothing in the draft: no box to open`: leaving would close the menu and the hook's
  pages would stop.
- `APPLY=1`: `runtime/ui/option_rows.cpp` `opt_draft_apply()` as the Restart button calls it, without the boxes
  (every key is written and saved, none waits for *Keep*), and the menu stays open. The log has `menu: test hook:
  Apply of the draft's N change(s) saved` (or `failed`) after the draft's own lines. It writes the settings file.

A cheat's row takes the hook's clicks like any stepper: `NX2SARIA_MENU_TEST_CLICK=CHEAT_<HASH>:r` while the Cheats page
is shown.

### `list_gpus()`

The lists the Screen and Game tabs build. `g_gpu_type[]` is each device's type (`VkPhysicalDeviceType`).
`g_gpu_usable[]`: a discrete one the game can use, 1 or 0, -1 while not asked (`row_gpu()` asks when it needs to).
`g_gpu_n` is -1 while the devices are not listed yet. At most `MAX_GPUS`, 8.

The Vulkan devices are listed in `vkEnumeratePhysicalDevices` order, which `NX2SARIA_GPU_INDEX` counts in
(`runtime/vk/vk_device.c` `pick_device()`). `VK_INCOMPLETE` keeps the first `MAX_GPUS`.

### `row_window_size()`

Window sizes (`kSizes[]`): the common 16:9 sizes that fit the monitor (1280x720, 1366x768, 1600x900, 1920x1080,
2560x1440, 3200x1800, 3840x2160), the 1920x1080 default and the current size, which goes where its area puts it. A
size is shown with U+00D7, the multiplication sign.

### `row_output_res()`

The game resolution (`runtime/gfx/resolution.c`): 16:9 sizes from its smallest, 1280x720 (for weak GPUs), up, the
common 21:9 and 32:9 monitor sizes, the 16:10 screens of the Steam Deck, handhelds and laptops and the 3:2 screens of
laptops and tablets (Surface, Framework; the camera and the HUD follow another shape, `runtime/gfx/resolution.c`
`res_init()`), and the current value when it is another size. A size is shown with U+00D7, the multiplication sign.

### `row_texture_detail()`

Texture detail (`runtime/vk/vk_texture.c`, "texture detail"): large surface textures made without their largest one or
two mip levels. It is read when the first texture is made, so the row notes the restart. The automatic entry names the
level it picks on this card; its text is static because the row keeps the pointer for the frame.

### `row_gpu()`

The Graphics card row. The loader lists the cards in an order that changes (`runtime/vk/gpu_choice.h`), so the card
chosen is kept by its name beside its position: `gpu_index` and `gpu_name`. The row shows the card the draft's two
settings pick, the one with the name wherever it is listed now, as the game picks it (`runtime/vk/vk_device.c`
`pick_device()`): Auto for a `gpu_index` alone that lists a card that is not a discrete one while a discrete one is
usable, which the game takes as Auto. Index 0 of the row is Auto.

Which discrete cards the game can use matters only to a `gpu_index` alone that lists a card that is not a discrete one
(`runtime/vk/gpu_choice.h` `gpuc_legacy_alt()`), so it is asked then, once for each card, and not at every other draw
of the row.

### `mals_regions()`

The regions of the `Mals/<region>.Product.<n>.sarc.zs` files in the RomFS index (`USen`, `EUpl`), listed once; 0 when
the index has none (no RomFS yet). Its count stays -1, not scanned, until then: the index may load after the menu
first draws.

### `row_game_lang()`

The game language: one entry per Mals language file the game ships (`runtime/ui/ui_text.c`
`ui_text_game_langs_in()` from the RomFS index, else `ui_text_game_langs()` by version), each under its own name. A
code that names no entry exactly (older `en`, `fr-CA` on a build without it) selects the first entry of the same
catalog language.

### `row_fps_limit()`

The frame limit (`runtime/pacing/frame_rate.c`): any whole rate from 30 to 240 on a slider (left and right step one,
held they step ten), then the monitor's rate and, where the game steps by the clock, unlimited. Unset, the older
`fps60 = 0` still means 30. The monitor stop shows the rate it gives. The note says when RivaTuner Statistics Server
does the limiting (`runtime/pacing/rtss_limit.c`), cannot change its profile, or let the game run past its limit.

### `vsync_note()`

"in use: ..." under the VSync row when the swapchain does other than the row says: VSync when it is off and the driver
cannot present without waiting, Immediate when it is on and the driver has neither mailbox nor relaxed FIFO
(`runtime/gfx/gfx_present.c` `pick_present_mode()`), so the game tears; NULL otherwise, and before the first
swapchain.

### `row_low_latency()`

Low latency without frame generation (`NX2SARIA_LOW_LATENCY`, `runtime/pacing/vk_lowlat.c`, at once): off, on, and on
with boost where the driver's mode is NVIDIA's (AMD's has no boost; a 2 set there shows as on, which is what it does).
Greyed out with the reason where it cannot run: a driver with neither mode, a run in which DLSS frame generation
brings its own, and NVIDIA's while a frame generation presents the frames (`vk_lowlat_status`). Set and refused by the
driver, it says so. In a run that started with it off the driver's mode is not on the device: the row can be set and
says "after restart".

### `menu_lowlat_restart_for()`

The row's "after restart" for a value of the setting, as `row_low_latency()` gives it: on in a run whose device was
made without the driver's mode (`VK_LOWLAT_RESTART`). `runtime/ui/option_rows.cpp` `live_needs_restart()` asks it for
the draft's list and the restart box after Apply.

### `dlss_offered()`

`g_gpu_vendor` is the PCI vendor of the GPU the game renders on (`menu_overlay_frame()` reads it from the device it is
given; 0 before the first frame). DLSS runs on NVIDIA GPUs only (`0x10de`), so the rows leave it out elsewhere (AMD,
Intel). While the vendor is unknown (no frame yet) DLSS is offered, as before.

### `fsr4_label()`

FSR 4's name in the Upscaling row and the overlay: the version the driver's library runs to its minor number, as AMD
names its releases ("AMD FSR 4.1" for 4.1.2); "AMD FSR 4" until the library was asked (`vk_ffx4_available`).

### `share_native()`

Native (the Upscaling row): the scene drawn at the picture's own size, which the upscaler's stand-in then only decodes
1:1 (`runtime/vk/vk_tonemap.c`), with no DLSS or FSR. It is `scene_share = 1` (`runtime/patches/widescreen.c`
`plain_share()`) with `dlaa = 0`; a `scene_share` of 1 or more set by hand in the settings file, or given by the
quality preset (Ultra), reads the same. `draft`: as the menu's draft has it (a staged preset's value), else as the
game runs now.

### `share_off()`

What leaving Native writes: the key unset, unless what unset reads is itself the whole picture (the preset's Ultra
gives 1): then the table's share, set by hand, so the choice is not undone by the preset under it.

### `scene_note()`

The Scene resolution and Upscaling rows' note on the scene's share (`runtime/patches/widescreen.c`
`wide_scene_restart()`, `heap_share()`): a scene larger than the render-target heap the start made waits for a
restart; at the heap's limit for this picture it stays smaller, and the note says how large it is. NULL for none.

### `upscaling_name()`

The Upscaling row's name of what is set now, for the performance overlay. FSR 3.1 set where it cannot load runs
nothing (`runtime/vk/vk_dlaa.c` `vk_dlaa_fsr3()`): the game scales the picture as the rest says.

### `aa_name()`

The Anti-aliasing row's value for the performance overlay: what `row_aa()` shows, with "TAA + " before it while DLSS's
or FSR's TAA runs with it (as `upscaling_name()` tells it: FSR 3.1 set where it cannot load runs nothing), and TAA
alone for off then. A setting of `taa` reads as the game's SMAA, with the upscaler or alone.

### `upscaling_restart()`

The Upscaling row's NVIDIA DLSS waits for a restart (`vk_dlaa_needs_restart`), where DLSS is offered: the row's note
and the Restart button's count (`restart_pending()`).

### `row_upscaling()`

One upscaling choice over three settings: `NX2SARIA_UPSCALER` (how the picture is fitted to the window),
`NX2SARIA_DLAA` (DLSS or FSR 3.1 on) with `NX2SARIA_TAA` (which of them) and `NX2SARIA_DLSS` (the mode, the render
scale: `runtime/patches/widescreen.c` `wide_present()`). A temporal mode fits the picture with bilinear, the upscaler
having made it; DLSS's model is NVIDIA's own for the mode (`runtime/vk/vk_dlaa.c`, `evaluate`, `dlss_preset` at its
default). The list: bilinear, FSR 1, the DLSS modes where DLSS runs (`dlss_offered()`), the FSR 3.1 modes where FSR
3.1 loads (`vk_ffx_available`); a DLSS or FSR 3.1 mode still set where it does not run stays listed last until another
choice replaces it, so the row shows what is set.

- FSR 3.1's modes take the same render scales as DLSS's (Native AA is FSR's name for DLAA's full size).
- FSR 4 (`runtime/vk/vk_ffx4.cpp`) is the same setting as FSR 3.1 with `NX2SARIA_FSR4` on, and takes the same modes:
  listed where it runs on this device, after FSR 3.1's. Native (`share_native()`) is listed where the scene's share
  can be set (`runtime/patches/widescreen.c` `wide_scene_settable()`), after the two entries that enlarge the game's
  smaller scene.
- What is set now is found from the draft's values; a temporal mode set where it does not run still shows (FSR 3.1
  does).
- The note. FSR 3.1 still set where it cannot load: nothing upscales, so the row says so under its value. A scene
  larger than the render-target heap the start made (Native, or DLAA chosen in a run that started with a smaller
  scene) waits for a restart; at the heap's limit for this picture it stays smaller, and the note says how large it
  is (`wide_scene_restart()`, `heap_share()`).
- Native is `scene_share = 1`. Leaving it for bilinear or FSR 1 unsets the key (the game's own share,
  `PLAIN_SHARE` of `widescreen.c`), or sets the game's share where unset reads as the whole picture (`share_off()`:
  the preset's Ultra); leaving it for DLSS or FSR unsets it, as their own modes size the scene; a share of the whole
  picture set by hand beside DLSS or FSR stays while one of their modes is chosen.
- FSR 4 that failed in a row stays listed (it runs on this device) while FSR 3.1 serves: the row shows FSR 3.1, and
  choosing FSR 4 again tries it anew (`vk_ffx4_retry`).
- The Anti-aliasing row keeps its choice, which runs along with DLSS's or FSR's TAA (`runtime/vk/vk_aa.c`
  `vk_aa_mode()`); its TAA went with the temporal mode, so leaving that leaves the game's SMAA.

### `upscaler_sizes_scene()`

`scene_shares[]` are the shares the Scene resolution row and Dynamic resolution's minimum list: 0.33, 0.4, 0.5, 0.58,
0.67, 0.75, 1.

DLSS or FSR sizes the scene through its mode (`upscaling_name()`'s condition, over the draft's values): a scene share
set in the menu does nothing then.

### `row_scene_res()`

Scene resolution (`NX2SARIA_SCENE_SHARE`, `runtime/patches/widescreen.c` `plain_share()`): the 3D scene's share of the
picture while neither DLSS nor FSR runs (their modes size the scene then; the condition is `upscaling_name()`'s, over
the draft's values), where the share can be set (`wide_scene_settable()`). The HUD and menus are drawn at the
picture's size (`GAME_RES`), so a smaller scene leaves them sharp. Unset reads as the quality preset's share, else the
game's own (the table's 0.75): that entry is named so and takes the place of the fixed share it equals. 100 % is the
Upscaling row's Native. Live, with the Upscaling row's note on the render-target heap (`scene_note()`).

The unset entry goes where its share falls among the fixed ones, and the fixed share it equals is left out. The game's
own share, set by hand beside a preset's, is named as the game's. A share set by hand that is none of these is shown
after them, as it is.

### `row_dyn_res()`

Dynamic resolution (`NX2SARIA_DYN_RES`, `runtime/pacing/dynres.c`), live: the scene's size follows the GPU's time per
frame, through the upscaler's mode while DLSS or FSR runs and through the scene share without one, so the row is shown
either way where the scene's size can be set (`wide_scene_settable()`: the upscaler's modes size the scene through the
same patch set). Its minimum (`NX2SARIA_DYN_RES_MIN`, live) is the floor of the scene share only: shown where
`row_scene_res()` is, with its shares as percentages, and greyed while dynamic resolution is off (the draft's value),
so it reads as part of it. A minimum set by hand that is none of them shows as it is.

A note under the name while it is on with nothing to lower (`dynres.c` logs the same): DLSS or FSR at native size,
which has no lower mode to step to, or a scene resolution at or under the minimum, which leaves no room.

### `row_preset()`

The quality preset (the `cfg_preset_*` functions of `runtime/cfg.h`, `runtime/cfg/preset_pick.h`;
[presets.md](../presets.md)). The row's choices: Recommended (the tier this PC gets, named), then the four tiers; the
value the draft stages, else the player's, else Recommended, and Recommended while a tier chosen on other hardware
gives way to it (`cfg_preset_hw_changed`): choosing Recommended is then a change of its own (`opt_choice_index()`'s
`again`: left on it), which Apply records as the player's choice (`cfg_preset_chosen`). Choosing stages the preset key
and hands back to the preset every key the chosen tier sets that the player set by hand to another value
(`opt_preset_handback()`; staged `""`), so the whole tier applies; when there are such keys a box lists them first
(`preset_confirm()`) and Cancel stages nothing. Apply switches the layer (`runtime/ui/option_rows.cpp`
`opt_draft_apply()`). A tier with keys it sets set by hand to other values shows as custom.

- `g_preset`: `ask`, `layout_menu()` opens the box at its next frame; `choice`, 0 Recommended, 1 and up the tiers;
  `from` and `to`, the row's shown values, for the draft's line.
- `file` is the entry the player's preset line names.
- Custom: keys the player sets to other values than the shown tier gives them.
- Recommended chosen on new hardware: the draft's line starts from the settings file's tier, which gives way now.
- Keys set by hand: the box lists them with the value each takes, and stages the choice on its yes. A line reads
  `<label>: <from> -> <to>`, with `->` because the font has no arrow glyphs.

### `preset_key_label()`

The name a preset key goes by in the box: its row's, with the key after it where one row writes several.

### `preset_stage()`

Stages a choice: the preset key, then `""` for each key handed back (after the preset key, so the draft reads them
against the staged tier).

### `row_aa()`

Anti-aliasing (`runtime/vk/vk_aa.c`, `NX2SARIA_AA`): the game's own SMAA (the default), off, FXAA or SMAA in its
place, or TAA, which is DLSS or FSR at the scene's size, the Upscaling row's temporal modes. Without a temporal mode
set (`dlaa`), TAA is listed where one runs: NVIDIA DLAA where DLSS is offered and `taa` does not name FSR, else FSR 4
Native AA where it runs, else FSR 3.1 Native AA where it loads; choosing it sets that native mode, as the Upscaling
row would. While a temporal mode is set its TAA always runs, and the row picks what runs with it: the game's SMAA,
nothing, FXAA or SMAA, noted as joined by the Upscaling row's TAA. A choice then leaves the upscaler and frame
generation as they are. `taa` itself, set before or in the settings file, shows as the game's SMAA, which is what runs
with the upscaler (or without one, where it cannot run).

FSR 3.1 set where it cannot load runs nothing (the Upscaling row says so): no TAA joins the choice then. The TAA entry
is listed only without a temporal mode, and choosing it sets the native mode of that upscaler, as `row_upscaling()`
sets it.

### `hdr_note()`

Under the HDR value: "after restart" when what is set (the draft's value) waits for one (the game's own HDR, to or
from it: `vk_hdr_restart_for`), "in use: off" when HDR is on but the swapchain is not HDR10 (the display or Windows'
HDR switch is off, `runtime/vk/vk_hdr.c` `vk_hdr_surface_format()`).

### `row_hdr()`

HDR (`runtime/vk/vk_hdr.c`), two kinds: the game's picture tone mapped to the display (1), switched on and off at
once, or the game's own HDR (`native`), which the game takes up at its start. The three brightness rows follow while
either is on. The tone mapped picture's own rows come after them (native HDR is the game's picture as it is): the tone
mapper and the gamma correction, which every tone mapper's picture gets, then the rows of the tone mapper chosen: none
for the game's curve, the grading rows of both others between the rows each has alone. All of them apply at once;
their defaults are one look (the head of `vk_hdr.c` names it).

Whether the tone mapper's rows show follows the draft's value, as the HDR row shows it. `renodrt` is the luminance
mapper; anything else reads as the observer-response mapper, as `vk_hdr.c` reads the setting.

### `row_framegen_mult()`

DLSS frame generation's multiplier (`NX2SARIA_FRAMEGEN_MULTIPLIER`, `runtime/vk/vk_dlssg.c`, at once): 2x, 3x or 4x as
far as this GPU and driver allow (`vk_dlssg_multiplier_max`, from Streamline). While DLSS's frame generation runs, the
row lists only those and shows the value in effect, a higher setting clamped to it, with a note saying why the rest
are missing; before the restart that starts it, or before Streamline reported its limit (the maximum reads 0 and is
taken as 4), it lists all three with the setting as it is. Left out while it is set but does not run: the frame
generation row says why.

### `framegen_ngx()`

NVIDIA's frame generation runs through NGX itself, not through Streamline (the `vk_ngxfg_*` functions of
`runtime/vk/vk_dlaa.c`, the `ng_*` ones of `runtime/gfx/gfx_present.c`): every build but the Windows one.

### `row_framegen_mult_ngx()`

Its multiplier there (`FG_MULT` in `runtime/cfg/cfg.c`, `runtime/gfx/gfx_present.c` `ng_multiplier()`): 2x up to what
the GPU allows, or Dynamic. `vk_ngxfg_max` gives the generated frames per rendered one. A multiplier above what the
GPU allows shows as the highest. Left out while frame generation is set but cannot run: the frame generation row says
why.

### `framegen_restart()`

Frame generation to or from NVIDIA's waits for a restart: AMD's follows the setting, NVIDIA's is set up with the
device (`runtime/vk/vk_dlssg.c`). Through NGX it follows the setting too, once NGX is up, which needs DLSS upscaling
on when the game starts. The row's note and the Restart button's count (`restart_pending()`).

### `fg4_label()`

The frame generation of the library FSR 4 comes with (`runtime/vk/vk_fg4.cpp`), as the row names it: with the version
the library runs on this card, the machine-learned 4.x on a Radeon RX 9000 and 3.1.x on any other.

### `fg4_chosen()`

It is the one in use: set (`NX2SARIA_FSR4_FG`) with FSR 4 as the upscaler, where it runs (`runtime/vk/vk_fg.cpp`,
`use4`). Without an upscaler (`dlaa = 0`) the FSR 3.1 library's runs.

### `row_framegen()`

Frame generation (`runtime/vk/vk_fg.cpp`, at once; NVIDIA's after a restart), with any Upscaling choice:
`runtime/vk/vk_dlaa.c` makes its depth, motion vectors and picture without the HUD with an upscaler or without one
(`fg_only_frame()`). The feature: [frame-generation.md](../frame-generation.md).

- FSR 3.1 set where it cannot load (`vk_ffx_available`) has no frame generation.
- Through NGX (every build but the Windows one) NVIDIA's needs NGX, which comes up only with DLSS upscaling on when
  the game starts (`vk_ngxfg_wanted`): before such a start it waits for a DLSS mode under Upscaling and a restart.
- What the texts promise follows what runs here: frame generation with any choice, NGX's start rule, or none.
- `fsr_win_only`: without DLSS, where FSR 3.1 upscales through the runtime's own passes (`vk_ffx_available`) and its
  frame generation cannot run, since that needs AMD's library and only the Windows build loads it
  (`vk_ffx_framegen_available`): the texts say so. "Needs DLSS or FSR 3.1, which do not run here" read as wrong beside
  FSR 3.1 chosen under Upscaling. On Windows it is always false.
- A note of `UI_TEXT_COUNT` means no note. Where only FSR 3.1's frame generation is missing, the row's greyed-out note
  says why, and "needs DLSS or FSR 3.1" under the value would contradict the Upscaling row above it. What the row says
  then is logged once a run (`told_fsr_win`).
- The choices: off, AMD's where FSR 3.1 loads and NVIDIA DLSS (`runtime/vk/vk_dlssg.c`, through Streamline and the
  helper library `nx2navi_dlssg.dll`) on NVIDIA GPUs, now that Reflex paces it from the frame's start and generated
  frames are presented; elsewhere one stays listed while it is set.
- AMD's has two libraries where FSR 4 runs: the FSR 3.1 library's, and the one of the library FSR 4 comes with, listed
  after it as a choice of its own. That one generates only beside FSR 4 upscaling, so choosing it sets the upscaler to
  FSR 4 as well; both are `NX2SARIA_FRAMEGEN = fsr3`, told apart by `NX2SARIA_FSR4_FG`. It is offered beside FSR 4
  upscaling only: not without an upscaler.
- AMD's follows the setting; NVIDIA's is set up with the device, so only a change to or from it waits for a restart.
  Through NGX it follows the setting too, once NGX is up, which needs DLSS upscaling on when the game starts.
- FSR 3.1's set where it cannot run: "in use: off" and why (`UI_MENU_FSR3_OFF`, or `UI_MENU_FRAMEGEN_OFF_FSR_WIN`).
- NVIDIA's set since the start but not running: "in use: off" and why (`vk_dlssg_status`; the log has the details).
  Through NGX that is NVIDIA's frame generation library missing beside the game, or a GPU it does not run on.
- FSR 3.1's through the runtime's passes (every build but the Windows one): 2x, or off while HDR is on
  (`runtime/gfx/gfx_present.c` `ng_ready()` runs it on SDR swapchains only); one line in the log for each, as the row
  says it.
- The FSR 4 library's runs beside FSR 4 and has no note of its own.
- With NVIDIA's chosen through NGX the multiplier row is `row_framegen_mult_ngx()`, which shows nothing while NGX's
  frame generation cannot run.

### `row_resolution()`

One Resolution row for the picture's height (`NX2SARIA_GAME_RES`, `runtime/patches/widescreen.c` `picture_height()`:
1080, the game's 1440 or more; the row's heights are 1080, 1440, 1800, 2160, 2880) and the window outside fullscreen
(`NX2SARIA_WINDOW_SIZE`): each height is shown as the size of the game's shape at it (16:9 2560 x 1440, 16:10
2304 x 1440, 3:2 2160 x 1440, 21:9 3440 x 1440, 32:9 5120 x 1440, or the window shape), and choosing one sets both.
The window takes it at once (`runtime/gfx/gfx_present.c` applies a changed size at the next present), within the
monitor, and the picture after a restart. Native draws at the window's size and leaves the window as it is. As two
rows of their own, a window of one size took a picture of another and scaled it for nothing.

- The window's shape where the picture follows it: the monitor's in fullscreen, else the window's own (16:9 when
  neither is known).
- The window stays within the monitor, in the same shape (the picture keeps its height and is scaled into it).
- A size is shown with U+00D7, the multiplication sign.
- The "default" mark: what the key reads unset, the quality preset's height (a staged tier's included) or the 1440.

### `row_wide_placement()`

Where the HUD and the menus go on a picture wider than 16:9: `NX2SARIA_WIDE_HUD` (`runtime/hle/romfs.c`
`wide_hud_overlay()` moves the HUD's edge elements out to the picture's sides) and `NX2SARIA_WIDE_MENUS` (`full`: the
menus' layouts fill the picture, `runtime/patches/wide_hud.c`; `center`: black bars at the sides while a menu is up,
`runtime/patches/widescreen.c`). Both are read at start, so a change waits in the draft until Apply and the row says
"after restart". They exist only for a picture that is not 16:9: while the Widescreen row shows the game's 16:9 (its
draft value, `opt_value()`) both rows are left out, as the HDR rows are while HDR is off, and come back as soon as
another shape is picked. A window of 16:9 under "Window shape" is not known here; the start's `wide hud:` and
`widescreen: menus` lines say what applied.

### `tab_screen()`

- The window's size is the Resolution row's where the picture's size can be set (`row_resolution()`); a build without
  the widescreen patch sets draws the game's own size and keeps the window row.
- VSync: On is the default mode, mailbox with its fallbacks (`runtime/gfx/gfx_present.c` `pick_present_mode()`): plain
  fifo would drop the game to 30 at every slow frame on a 60 Hz display. A fifo already saved also reads as on.
- The game resolution row needs the output and scene patch sets (`runtime/gfx/resolution.c` `res_adjustable()`): this
  game's build has none and always draws its own size, so the row would change nothing.
- Widescreen needs the patch sets of `recomp/patches-v111.txt` (`runtime/patches/widescreen.c` `wide_available()`).
- The sharpening runs under FSR 1 (`runtime/vk/vk_fsr.c` `vk_fsr_record()`, RCAS) and FSR 3.1
  (`runtime/vk/vk_ffx.cpp`, its own RCAS); with bilinear or DLSS the slider would do nothing, so it is greyed out.
- Anisotropic filtering of the surface textures that tile (`runtime/vk/vk_texture.c` `aniso_raised()`) applies at
  once (`vk_sampler_aniso_live`).

### `rows_saves()`

The Game tab's Saves group (`runtime/hle/save_share.c`). Export writes the save the game would load next into a zip in
the player's Downloads folder, for another player or, with this run's log and the settings file, a bug report. Import
takes a zip the player picks in the host's file dialog and leaves its save for the next start
(`runtime/hle/fs_host.c` `import_pending()`): the game holds its progress in memory and would write it back over a
save replaced under it. The work runs off the present thread (a thread of its own, or the dialog's); the rows wait
greyed meanwhile, and the line under them says what the last one did, else what an import did at this start.

`g_share_text` is the last result's `UiTextId`, -1 for none; a format that takes `g_share_arg`. `start` is 1 for an
export, 2 for an export for a bug report, 3 for an import. Whether an import waits is a `stat` per frame while the
Game tab shows; the launcher's own import drops a waiting one.

### `share_zip_picked()`

The file dialog's answer, on its thread: the import's checks and the copy into the profile run here. A NULL path is a
cancel, and the last line stays.

### `tab_game()`

- The frame limit row: this game steps by the clock, so the pacer alone sets its rate
  (`runtime/pacing/frame_rate.c`).
- The message window's typing speed (`runtime/patches/text_speed.c`), where the game version has the patch set
  `text_speed`; live. The names follow the values in their order in `runtime/cfg/cfg.c`.
- The game's once-a-day amiibo limit (`runtime/patches/amiibo_daily.c`), where the game version has the patch set
  `amiibo_daily`; live.
- `runtime/discord_rpc.c` reads its setting on its own thread and connects or clears the status within half a second.
- Testing: experimental rows, then the debug menu where the game version has it. `runtime/vk/vk_exec.c` `rw_init()`
  and `tail_enabled()` read these at start: each row notes the restart and the bar's Restart does it. A row whose
  setting does nothing without another one (`rw_init()`) is greyed out until that one is set. Automatic comes first,
  the default (`vk_exec_replay_workers_auto`), named with the count it gives on this PC. The rows below follow the
  rows above by the draft's values (both are read at start, so they wait for Apply).
- The debug menu, a tool for testing with English text of its own, is off unless switched on here.

### `pad_layout_note()`

`g_modal`: a confirmation is open (restart, reset): B and Esc close it, not the menu; L / R stay on the page.

The note under the layout: Automatic with a gamepad connected, the layout it picked ("in use: Xbox"), named
PlayStation for a pad whose bottom button is labelled cross. NULL otherwise.

### `key_action()`

The action a key presses, or -1: the walk key is a key of its own, but Shift is A (dash) in the default bindings, and
a walk key that also presses something walks and does that something at the same time. The bindings come from a
snapshot, and every key of an action is looked at: `runtime/input/input.c` `binds_refresh()` rebuilds the live table
on the polling thread while the menu draws, and an action may hold more than the two keys its cells show.

### `row_walk_key()`

The walk key as the handful of keys a player is likely to pick (none, lshift, lalt, lctrl, tab, capslock). "none"
rather than an empty value closes it: an unset key falls back to the default in the settings table, the same as a
fresh installation. The settings file takes any name `runtime/input/input_map.c` knows (walks of the keyboard sticks,
not of the gamepad). A key the current bindings also use says which action it presses.

### `prompts_note()`

The note under the prompts. The settings name a folder of prompts (`input_prompts_folder`: the prompts and the pad
layout, as a layout that puts A on the right button needs the face buttons named the other way round), but the game
reads its fonts and UI archive once at start (`runtime/hle/romfs.c` `overlay_init()`). While the folder in use
differs: "files missing" when the setup did not write the one asked for, why it was left out at start (damaged, a game
resolution that is not 16:9, a RomFS mod with the same files, `NX2SARIA_ROMFS_PATCHES`), else "prompts change after
restart" with `*restart` set: a restart would bring the prompts asked for. The action prompt's diamond of face buttons
is laid out for the pad layout at start too (`runtime/hle/romfs.c` `action_prompt_overlay()`), whatever the prompts:
while the layout asks for the other places, a restart would lay it out again, so `*restart` is set whatever the note,
and with the folder in use the note is "prompts change after restart". NULL when both match.

- `romfs_action_prompt` gives -1 when the diamond could not be laid out: a restart would not change it.
- A game resolution of another shape needs a folder of its own for the PC prompts (`romfs-patches-pc-<w>x<h>`), which
  the setup does not write: the shape is the reason then, not the setup.

### `restart_pending()`

How many settings wait for a restart, on the Restart button: the restart-only ones whose value differs from the one
the game started with, the draft's value where it holds one (`opt_pending_count()`: Restart applies the draft first),
NVIDIA's upscaling, frame generation and HDR while their rows say "after restart" (their settings are live ones, so
the rows decide; HDR waits for Apply, so its draft value), and the prompts when a changed pad layout asks for others
(the prompts setting itself, when changed, is among the first).

### `restart_confirm()`

The confirmation of the menu's Restart button, over the menu; Cancel has the cursor first. Restart writes the settings
file, then closes the game as Quit does and starts it again (`gfx_window_request_restart()`). It waits while the game
writes a save, and the game stays when the settings file cannot be written, as the changed settings would not come
back. The Quit button's confirmation is the same box (`g_confirm_quit`), and it waits too: the game runs on while the
menu is open, and closing the window finishes only a Commit that has begun (`gfx_window_closed`, `fs_save_flush`), so
a save between writing its files and committing them would be dropped.

With lines in the draft the box adds a note: restarting applies and saves the changes made in the menu (that is what
most are for), quitting drops them.

### `keep_reverted()`

"Apply changes?" when the player leaves the menu with a draft (`runtime/ui/option_rows.h`): every change as "label:
old -> new", the ones read at start marked. Apply writes them and the settings file and closes the menu, Discard puts
the previews back and closes it, Continue editing (the default, and Esc / B) goes back to the pages with the draft
kept. It is the same box as `restart_confirm()`; the list scrolls (wheel, D-pad up / down) when it is long.
`g_apply_ask`: `layout_menu()` opens the box at its next frame.

What follows an Apply (`post_next()`): the "keep these display settings?" box while a timed display key waits for the
answer (`POST_KEEP`), then the "restart now / later" box when an applied setting is read at start (`POST_RESTART`).
They open one ImGui frame after the box before them closed (a popup opened in the frame its predecessor closes would
see its stale stack). `g_post_open`: its popup was opened. `g_post_frame`: the ImGui frame it opens in.
`g_keep_deadline_ns`: `plat_now_ns()` at which the display settings revert.

`RestartLine`: the applied restart-only settings, "label: new value". `display`: the row's keys wait for the keep box
(the Resolution row's game resolution, `OptDraftItem.display`), and its Revert takes the line away.

`keep_reverted()`: the keep box put the display settings back (`opt_display_revert()`), and with them the restart-only
keys of their rows: those lines no longer wait for a restart.

### `leave_menu()`

Does nothing while a box is open already: the player is answering it. With a draft that has nothing to show (a choice
made and made back), the draft is discarded, so the keys it wrote go back.

### `post_next()`

The step after an Apply (`from_keep`: after the keep box). Display keys applied live come first, since their box can
revert the picture; then the restart box; else the menu closes. `gfx_window_apply_settings()` wakes the window: the
window thread follows `FULLSCREEN` / `WINDOW_SIZE`, and `VK_PRESENT` follows at the next present.

### `pad_events()`

In the tab mode the buttons of `NX2SARIA_INPUT_SCRIPT` are added (`runtime/input/input.c` `input_script_buttons()`), so a
test run reaches the tab's rows; Plus, which has no ImGui key, is kept as a press in `g_pad_plus`, only in the tab
mode and dropped on a reset, so a Plus from another menu never closes the tab.

### `apply_confirm()`

- With no lines left, something else (F11) took every change away while the box was open.
- A line reads `<label>: <from> -> <to>`, with `->` because the font has no arrow glyphs.
- The list is as wide as the three buttons at least (640 scaled pixels or 70% of the view when that is more), so it
  and the video memory note use the box's width: the box is sized by its widest part, and at a small window that was
  the buttons, with the list wrapping in two thirds of it.
- The list is laid out in rows of one height. Each line is cut into rows here (`CalcWordWrapPositionA`), not by the
  text item's own wrapping, and the list is a whole number of rows tall (at most 40% of the view), scrolled a whole
  row at a time: no row shows cut in half, and the edge of a scrolled list falls between rows. Held up / down moves
  `scroll` smoothly and the list shows it rounded to a row; the wheel and the scrollbar are taken as they come and
  rounded the same way.
- The "(after restart)" mark is one piece: it follows the line's last row when it fits there whole, else it gets a
  row of its own. It was a text item on the same line before, wrapped in what room was left, which broke it inside a
  word in the languages where it is long.
- Continue editing has the cursor first.
- The restart lines are read before Apply empties the draft.
- A failed write keeps the menu (its bottom row says so), unless display keys wait for their answer.


In the tab mode an Apply logs its changes through `runtime/ui/pause_tab.c` `pause_tab_applied()`, and Apply or
Discard is the reason the tab closed.
### `preset_confirm()`

The box `row_preset()` asks for: the keys set by hand and the values the preset gives them; Cancel (the default, Esc /
B) stages nothing, the yes stages the choice (`preset_stage()`). Same look as `apply_confirm()`.

### `post_confirm()`

The boxes `post_next()` queues. Keep: Keep / Revert, Revert has the cursor first, Esc and B revert, and so does the
timeout (`plat_now_ns`, counted from the Apply). F11 in the meantime is the player taking control: the box closes and
what is on screen stays (`opt_display_external()`), without asking. Restart: Restart now / Later, Later has the cursor
first, Esc and B are Later; the settings are saved already, so Later only closes the menu. After Restart now the box
stays until the window closes.

When the popup is gone (closed from outside), the safe answer is taken.

### `rows_gyro()`

The Controls tab's Gyro group: motion on or off, then how `runtime/input/input.c` (`motion_options_refresh`) treats
the gyro (all live). Motion is staged, so the rows below read it from the draft and are greyed out while it is off
there. The dead zone's names follow the values in their order in `runtime/cfg/cfg.c` (off, low, standard, high).

### `tab_controls()`

- The mouse camera comes first, where a player on keyboard and mouse looks for it; both rows are live. How the mouse
  reaches the game, where the game version has the direct mode's patch set (`runtime/input/mouse_direct.c`; the values
  in their `cfg.c` order, stick then direct), and its sensitivity. The slider stops short of 0, which the settings
  file takes to switch the mouse camera and the cursor capture off.
- The mouse in the game's own menus (`runtime/ui/menu_cursor.c` on 1.4.3, `runtime/ui/menu_mouse.c` on 1.1.1; live;
  hidden elsewhere).
- The prompts row: the setting's values in their `cfg.c` order (nintendo, xbox, playstation, steamdeck, pc). The game
  reads its prompts at start: the row's note says when a restart would bring others, and the bar's Restart button does
  it.
- The sprint button (`runtime/patches/sprint.c`), where the game version has the patch set `sprint`; both rows are
  live. The names follow the values in their `cfg.c` order.
- HD rumble can be switched only while the gamepad in use is one whose actuators take the game's values (a Switch Pro
  Controller or Joy-Con, `runtime/input/input.c` `hd_probe()`); with any other the row is greyed out and says what it
  needs.
- The keyboard's sticks are all or nothing: the walk key scales them to the walk scale (0.2 to 0.9) while it is held
  (`runtime/input/input.c` `walk_get()`).

### `menu_sounds_frame()`

The game's interface sounds (`runtime/ui/menu_sound.c`) for what the frame did: a press or click on an item, the
cursor on another item, another tab by a click (L, R, Q and E play theirs where they are read: `g_tab_sounded`, the
tab they asked for, its sound played). The first frame after opening only records: the open sound has played, and the
cursor going to the first row is part of it. `g_sound_fresh` marks that frame, as the frame count alone does not when
nothing was laid out while the menu was closed.

### `pause_tab_close()`

Ends the tab without asking: the box and any confirmation are closed, the draft is dropped and the menu closes with the
reason, so `runtime/ui/pause_tab.c` shows the System page again.

### `pause_tab_rows()`

The tab's page: the Picture rows of `tab_screen()` a player looks for in a pause menu (the quality preset with its
confirmation, the frame limit, VSync, the upscaler, anti-aliasing, frame generation, HDR where the window's surface
offers HDR10 or it is on, sharpening, anisotropic filtering, and with the widescreen patches the picture's shape and
the HUD's place, whose notes say "after restart"), then Sound (`rows_sound()`), Controls without the bindings
(`rows_controls()`) and Game (`rows_game()`), each under its tab's name as a group, with the same row functions as the
settings menu. Last, "More settings…" (`g_more_req`). Left out: the bindings (their cells wait for a press, and L, B
and Plus close the tab), the saves (a file dialog), the testing rows, the debug menu, Cheats and Amiibo, the window
and the GPU: all in the settings menu, which More settings opens.
### `rows_sound()`

The Sound tab's rows, for the tab and for `tab_sound()`.

### `rows_controls()`

The Controls tab's rows without the bindings page (`binds_rows()`), whose cells wait for a press: for the pause menu's
settings tab and for `tab_controls()`, which adds the bindings after them.

### `rows_game()`

The Game tab's rows a player looks for in a pause menu: the game's language, the operation mode, playing in the
background, the message speed, the amiibo daily limit and Discord. The frame limit is in Picture; the saves'
export and import (a file dialog), the testing rows and the debug menu stay in `tab_game()` only.

### `pause_tab_label()`

The tab's name: "NX2Saria", or `UI_MENU_PAUSE_TAB` with `pause_settings_tab_name = settings`.

### `pause_tab_header()`

The tab's name in the pause menu's header, on the background list, at the box `runtime/ui/pause_tab.c` `header_tick()`
works out, in picture fractions turned into the window's pixels with `gfx_picture_rect()`. The plate runs from the
picture's top down to 0.545 of the box's height below its centre (y 878 in layout units, where the game's plate ends),
as wide as the text and a margin of half the box's height, at most the box. The text's centre is 0.06 of the box's
height below the box's: on the bench the game's category names sit about 13 units lower than the box's centre. The text is in the menu's first font, the game's own text face with the system's CJK glyphs
merged (the title face drew "设置" thin and small), at 0.28 of the box's height, the game's category text size (0.38 for a name with CJK characters, the UTF-8 lead
bytes 0xe3 to 0xe9: on the bench the system's CJK face drew "设置" at two thirds of the game's "系统" at the same size), and
shrinks to fit 0.94 of the box's width ("Configurações").

### `layout_pause_tab()`

The open tab: one window over the page box of `runtime/ui/pause_tab.c` `plus_tick()`, with the rows, the box "Apply your
changes?", the preset's confirmation and the boxes after an Apply. The first row takes the cursor on the first frame.
More settings, with no box up, ends the tab mode without closing the menu: `g_pause_tab` cleared, the settings menu
asked for its Controls tab, `runtime/ui/pause_tab.c` told (`pause_tab_set_open()`, `pause_tab_more()`), and
`g_more_open` keeps the time, so the menu's close logs how long it was open. `g_modal` is cleared at the
start of each frame, as `restart_confirm()` does for the settings menu: the boxes set it while they are up. L (or Q), B (or Esc) and
Plus leave it as Continue leaves the settings menu (`leave_menu()`), unless a box is open. It closes by itself when the
pause menu's page has not been seen for `SEEN_MS` (the pause menu closed under it), and with
`NX2SARIA_PAUSE_SETTINGS_TAB_TEST=fail` 30 frames after it opened.

### `layout_menu()`

`g_tab` is the tab shown; `g_tab_request` selects a tab at the next layout (-1 none); `g_focus_page`: this tab's first
row takes the cursor once it is shown (-1 none).

- `opt_draft_sync()` runs first: a key something else changed (F11) leaves the draft.
- With the game's frame art, the window's own border is 0: the game's frame is the border.
- The game's ornamented frame (`runtime/ui/menu_art.cpp`; nothing without the RomFS's), in gold. Its lines lie inside
  the texture (about 4 % of the width, 7 % of the height in), so the frame is drawn that far outside the window and
  its lines run along the window's edge, clear of the title and the rows; unclipped, it may reach past the window. The
  art holds the top and left edges with their corners; turned half a turn it gives the bottom and right ones.
- The title line: the Triforce, then the game's name and the page title. The port version and the game update are
  right-aligned on the title line: what a bug report needs first; left out where it would run into the title (a narrow
  window, a long title).
- L / R and Q / E step the tabs, except that a text field takes Q and E as letters, and a window of a page keeps the
  tab keys (`g_modal`, `binds_modal()`, `WantTextInput`). A requested tab's first row takes the cursor.
- A tab's label is `"  <name>  ###tab<k>"`: the id stays per language. The tab labels are in the title face, on the
  rows' button (`runtime/ui/menu_skin.cpp` `skin_tab_item()`).
- The page's rows are a `NavFlattened` child of this window itself: ImGui clips the items of a direct child only, so
  behind a second child level the rows scrolled out above the tabs won D-pad moves from the buttons and the tabs (up
  from "Continue" went to the tabs, down from the tabs to "Continue").
- The Amiibo tab is `runtime/ui/amiibo_page.cpp` `tab_amiibo()`: the emulated NFC reader.
- The bottom row: the save status on the left, the buttons on the right, on the game's control-guide bar, which is
  drawn within the bottom row's own height: above it the page's last row may still show.
- The failed-save note stays until a save succeeds; the settings hold for the session.
- Restart says how many settings wait for it (`restart_pending()`); `###restart` keeps the button's id, and so the
  cursor on it, when the count changes.
- The buttons are the save menu's brown buttons, with the game's cursor on the selected one
  (`runtime/ui/menu_skin.cpp`). They are right-aligned; in a window too narrow for all three, from the left edge.
- Lines of the settings file that are no settings of this version do nothing (`runtime/cfg/cfg.c`
  `cfg_unknown_keys()`). That is said here, in the save status's place while saving works, so that a line added by
  hand is not taken for the cause of what the game does: a player took such a line for the fix of a crash. The list is
  cut to the room left of the buttons.
- `apply_confirm()` runs after `restart_confirm()`, which clears `g_modal`; `post_confirm()` runs the boxes that
  follow an Apply.

### `perf_cycle_once()`

The performance overlay (`NX2SARIA_PERF_OVERLAY`), in the top right corner ([performance.md](../performance.md)).
"fps": the frame rate over the presents of the last half second. "full": a panel of the numbers of
`runtime/diag/perfmon.c`: the machine's load and temperatures, the frame rate and frame times with a graph, the render
thread's waits. Off, it costs a `cfg_version` load per present. `g_perf_mode`: 0 off, 1 fps, 2 full. `g_perf_cycle`:
F10 presses not yet handled; the present thread owns the draft, so it changes the mode.

`perf_cycle_once()` is one F10 press, on the present thread. Closed menu: the mode is set and saved at once (the
overlay is a setting that persists outside the menu). Open menu: it is a change of the draft like the row's,
previewed, saved on Apply.

### `cfg_reload_tick()`

Ctrl+F5 (`runtime/ui/menu_win.cpp`, `runtime/ui/menu_sdl.cpp`; F5 alone stays bindable): the settings file is read
again (`cfg_reload_file`) at the next present, on the present thread that owns the draft, and a notice in the top
centre says how it went for a few seconds. It is drawn by `perf_draw()`, so it needs no menu. While the menu holds
changes not applied yet (the draft, the keep-display countdown, the boxes after Apply) a reload would pull settings
from under them: it is refused with a notice instead.

### `preset_notice_tick()`

The one-time notice of the quality preset (`cfg_preset_notice_due`): a player who never chose a preset and whose PC
gets another tier than the game as shipped is told once, at the first frame the game runs with no other notice up,
which preset is in use and where it is set. The state file keeps it shown (`cfg_preset_notice_done`). It stays 8
seconds, longer than other notices: it says how to go back as well.

### `vram_warn_tick()`

Puts the notice of full video memory on screen ([runtime.md](runtime.md)): each overlay frame it asks
`runtime/vram_warn.c` `vram_warn_due()`, and shows the notice through `notice()`, the panel at the top centre the
Ctrl+F5 and quality preset notices use, for `VRAM_WARN_SHOW_MS`. It is never drawn over the settings menu or the
amiibo list (`g_open`), a movie (`runtime/hle/ov_movie2.c` `ov_movie_live()`), the title screen and the file menu
(the loaded save's world clock has not started: `runtime/pacing/frame_rate.c` `frame_step_world_us()` below 0), the
game's own pause or system menu and a loading screen (`runtime/patches/wide_bars.c` `wide_menu_open()` and
`wide_loading_open()`) or another notice (the panel's own, the note after F8, the amiibo note): the notice stays due,
`vram_warn_waits()` logs the reason at each change of it (`runtime/vram_warn.h` `vram_warn_wait_step()`), and it is shown at the first frame that is free, unless a whole minute
without a critical sample ended the stretch meanwhile. While the game starts and while an area loads the memory is
still filling and the renderer has not yet given back what the last area left, so a notice there would name settings
for a shortage that may be gone once the area is up; the samples of that time still count, and the notice comes with
the game in the scene if the level stayed critical. The two screens are known only while the patch sets `wide_bars`
or `fov_menu` are on (`fov_menu` is by default); without them the pause menu and the loading screen do not hold the
notice back.

What it names is read from the settings in effect, as `perf_picture()` reads them for the overlay:

- Resolution: the widescreen patch sets are there (`wide_available()`, else the menu has no Resolution row) and the
  picture (`wide_picture()`) is more than 2% taller than the swapchain image.
- Upscaling: the temporal upscaler runs (`NX2SARIA_DLAA`, and FSR 3.1 only where it loads) in the mode that renders
  the whole picture (`NX2SARIA_DLSS` = `dlaa`). The line names the mode as the row does: `UI_MENU_DLSS_DLAA` or
  `UI_MENU_FSR3_NATIVE`.
- Frame generation: FSR's where it can load, DLSS's where it is active.

The text is `UI_MENU_VRAM_WARN` and one line per setting (`UI_MENU_VRAM_WARN_RES`, `_MODE`, `_FRAMEGEN`, `_PRESET`),
each a printf format whose first `%s` is the menu's own label of the row, so the notice says the name the player will
look for; `_MODE` has a second `%s`, the mode in use. English, Russian, Brazilian Portuguese and Simplified Chinese.
`vram_warn_shown()` gets the same choices in English for the log.

### `vram_load()`, `vram_fit_make()`

The line of the *Apply your changes?* box that says the draft may not fit the video memory
([runtime.md](runtime.md), `runtime/vram_fit.h`). `vram_fit_make()` runs once when the box opens (`apply_confirm()`),
after the draft's lines are logged, and keeps the kind and the text in `g_fit` for the frames the box is up.

`vram_load()` reads one combination through the getter it is given: `runtime/ui/option_rows.cpp` `opt_before()` for
the settings as they were before the draft, `opt_value()` for the draft's (a staged quality preset's values
included). The picture's height is `NX2SARIA_GAME_RES` (`window`: the window's height; 0 without the widescreen sets,
where the menu has no Resolution row), the mode `NX2SARIA_DLSS` while the temporal upscaler is on (`NX2SARIA_DLAA`,
FSR 3.1 only where it loads), frame generation as set (FSR's only where it can load).

The text, in the menu language: `UI_MENU_VRAM_FIT_MEASURED` while the level is tight or critical and
`UI_MENU_VRAM_FIT_NOTICED` when only an earlier notice of this session makes the rule hold (printf formats: the share
in use, the use and the budget in MiB, then the rows' own labels of what got heavier), and `UI_MENU_VRAM_FIT_FULL`
when the use has reached the budget (no share: "100% in use (1102 of 1024 MiB)" read as a mistake), drawn in the
colour of the "after restart" marks; `UI_MENU_VRAM_FIT_PRESET` (the card's GiB, then each row's label with the value
the preset gives it: `Resolution 1080`, `Upscaling Quality`), drawn dim as information. It sits between the list and
the buttons, wrapped to the list's width. The buttons do not change: the note never blocks Apply.

The log gets one `menu: box   vram note:` line each time the box opens: `measured (...)` or `preset (...)` with the
figures and what was named (`resolution 1080 -> 1440`, `upscaling mode quality -> full size`, `frame generation off
-> on`), or `none (...)` with why.

### `notice_draw()`

The notice is wrapped within 90 % of the window: a long notice (the quality preset's, a translation) stays on screen
on a narrow one such as a handheld's 1280 pixels.

### `perf_gpu_once()`

The device the game renders on, for perfmon's GPU counters (by LUID), NVML (by name) and ADL (by PCI bus, else name);
once, before full mode.

### `perf_tick()`

The half-second window starts again at the first frame, or after a gap of more than 2 seconds: a stall the window
should not average in. With frame generation the line has the display's rate too (`gfx_output_fps()`).

### `perf_picture()`

The picture lines: the window, the game image the present shows and the 3D scene it holds
(`runtime/gfx/resolution.c`), then the settings as the menu's rows name them, so the overlay and the menu agree.

- This game's build has no output and scene patch sets, so `res_sizes` holds the other builds' placeholders there: the
  picture and the scene are `runtime/patches/widescreen.c`'s, the scene at the share it writes for the game
  (`wide_present()`; in the first two seconds of a run the game's own dynamic resolution still sizes it).
- FSR 3.1 frame generation set but not loadable reads as off. DLSS's multiplier is shown as set, clamped to what this
  GPU allows.
- The last line is the quality preset in use, custom while keys it sets are set by hand to other values.
- The 3D scene's size is known only where `resolution.c` sets it (the builds with its patch sets); this game sizes its
  own scene dynamically and `res_sizes` would report a fixed 1600x900, so the line is left out (`scene_known`).

### `perf_full_draw()`

The full panel (`runtime/ui/perf_panel.cpp` draws it in the menu's look): the numbers of this frame. The port version
and the game update are formatted once per menu language.

### `perf_draw()`

- With the menu open, the fps line is drawn under its window (the background draw list), over the dimming
  `layout_menu()` drew before.
- It sits 12 pixels (at scale 1) from the top right corner: the game's HUD leaves it empty.
- It is on a small panel in the menu's look (`runtime/ui/menu_skin.cpp`), its frame scaled to the line (0.6 of the
  scale, alpha 0.9).

### `capture_note()`

The note after F8 (`runtime/diag/bugcap.c`): the capture's number and its folder's name for a few seconds, top centre,
in the menu's look. It shows once the capture is written, so the capture's own picture is without it.

### `overlay_frame()`

- The GPU's vendor is read from the target's device, for the rows that depend on it (`dlss_offered()`).
- `menu_art_start()`: the game's interface textures decode on a worker from the first present on.
- `cheats_frame()`: the data cheats and the buttons of the cheats that need them held.
- `amiibo_picker_wanted()` is asked every frame: it follows the reader's state. `bugcap_note()` is the note after F8.
- `g_booting` is cleared: the game runs.
- `opened` marks the frame the menu is first laid out after opening: also when its `g_reset` went to an earlier frame
  of the overlay alone (the menu opened between the two loads), the pad's buttons held now must not count as presses.
- When the menu closed while the keep box was up (F1, a failure), the old display settings return, unsaved. When it
  closed some other way than the box with a draft (the menu failed), the previews go back.
- With `NX2SARIA_MENU_TEST_DEBUG` set the test shoots the debug menu's tabs instead.
- The pause menu's settings tab: its request opens the menu in the tab mode (`g_pause_tab`); while the menu is closed
  the overlay is drawn as long as the tab's name shows in the pause menu's header (`pause_tab_view()`), and the name is
  drawn on every frame the tab is on, selected while the tab is open, and not while the settings menu itself is open
over the pause menu (its dimmed backdrop would have the name over it).
- When the game started waiting for an amiibo, its list opens over the game.
- `g_open` is read again after taking the reset: the menu may have opened since the first read (F1 on the window
  thread while the overlay alone was drawn), and that frame's reset would be spent on an overlay frame, so the first
  open frame ran without its focus row and with the opening key still down.
- On a reset: `io.ClearEventsQueue()` drops events handed over before the close and not yet taken (ImGui spreads them
  over frames); `binds_cancel()`, so a cell waiting for a press when the menu closed binds no press on the next
  opening; on opening the first row has the cursor; a confirmation left open on closing is closed; a close request of
  the menu that just closed is dropped.
- F1 or the pad combo with a draft (`g_close_req`): ask, or close if the box is not needed.
- With the overlay alone, no input reaches ImGui. During the test hook's pages the events are dropped too: the user's
  input in a background test window must not change settings.
- There is no `ImGui_ImplVulkan_NewFrame`: all it does is upload Dear ImGui's own font atlas when the back end holds
  none, which it never does here (`fonts_up()` uploads it, `runtime/ui/menu_art.cpp` `menu_font_upload()`).
- The debug menu closes on Esc, or on B on the pad when no field is being typed in.
- The test hook: this frame's image goes to a file; then the next tab, or the menu closes. After the last tab the next
  `NX2SARIA_MENU_TEST` frame opens it again, and the next frame lays out nothing.

### `menu_overlay_frame()`

A text field takes typing: the window delivers characters (and a handheld shows its keyboard) while one is active.
Every overlay frame lets the window follow, also those `overlay_frame()` leaves early because the menu is closed: when
only the frames the menu laid out did, closing it with a field active (F1, Minus and Plus) left SDL's text input on,
and on Windows the input method attached to the game window, until the menu opened again.

### `menu_boot_frame()`

The boot screen (the wait of `runtime/boot.c` for the prewarm; `gfx_present_boot` presents it on a black frame). A box
in the middle of the screen in the menu's look (`runtime/ui/menu_skin.cpp` `skin_boot_panel()`): the title and the
step with its count and a bar. The input queue is left alone: keys pressed now reach the menu once the game runs.

- A phase of -1 is done: a black frame.
- `menu_art_start()`: the box takes the game's textures as soon as they are decoded, plain shapes until then.
- No `ImGui_ImplVulkan_NewFrame`, for the same reason as in `overlay_frame()`.
- The box has a filled bar while there is a count, else a sweeping segment.
- The title's size is scaled by `io.FontGlobalScale`, as `GetFontSize` scales the body's.

## `runtime/ui/option_rows.h`

Settings pages in two panes (`runtime/ui/option_rows.cpp`): option rows on the left, the selected (controller) or
hovered (mouse) row's description on the right. Each row shows a setting of the settings table (`runtime/cfg.h`) and
changes it through the draft (below; preview settings are written with `cfg_set` at once, the others wait for Apply). A
steps it (toggle, next value), left and right step it, and the mouse steps it on the two framed arrows beside its
value and nowhere else on the row (a click on the row selects it; held on an arrow it repeats). A row marks the
default value, says "after restart" while the value it shows (the draft's, else the setting's) differs from the one
the game started with (`cfg_pending_for`), and is greyed out with a note when an environment variable fixes it.

```cpp
opt_begin("##screen", scale);
opt_toggle("NX2SARIA_FULLSCREEN", UI_MENU_FULLSCREEN, UI_MENU_FULLSCREEN_DESC);
opt_end();
```

`key` is the setting's `NX2SARIA_` name, which must have a `cfg_meta` row. Present path only (ImGui). What the player
sees of the draft and Apply: [controls.md](../controls.md).

### `opt_reserve_bottom()`, `opt_bottom()`

`opt_reserve_bottom()`: the height the next pages leave free under their panes (the menu's bottom row). `opt_bottom()`:
that height, for a tab that lays out its own panes (`runtime/ui/amiibo_page.cpp`).

### `opt_focus_first()`, `opt_focus_take()`, `opt_focus_key()`

`opt_focus_first()`: the next page's first row takes the controller's cursor (opening the menu, switching tabs).
`opt_focus_take()`, for a page that lays out its own widgets (`runtime/ui/amiibo_page.cpp`): true once when its first
item should take the cursor, as `opt_focus_first()` asked; the request is used up. `opt_focus_key()`: the row of `key`
(or a button's id) takes the cursor, scrolled into view, when it is next drawn.

### `opt_disable_next()`, `opt_disable_next_text()`

The next row is greyed out like one the environment fixes, with the note under its label and in its description. The
text of `opt_disable_next_text()` must outlive the frame.

### `opt_group()`, `opt_text()`

Page text that is not a row: a group heading in the accent colour, a wrapped paragraph (`warn`: the warning colour).

### `opt_toggle()`

The return value of every row: true when it changed the setting this frame. `inverted`: the row shows "on" while the
key is 0 (a setting whose key names the opposite). `subtitle`: a short line below the name, shown even when the value
is off.

### `opt_toggle_value()`

An on/off row over a key with named values: off while the key holds `off_value` and on for any other, so a value the
row does not write itself still reads as on. Flipping it writes `on_value` or `off_value`. `note`: as for
`opt_choice()`.

### `opt_choice()`

One name per value of the key's `cfg_meta` values list, in its order. When the key has no fixed default (an empty
`def`), `names[0]` is the automatic choice (the key unset) and `names[1..]` follow the values. `note`: a short line
under the value in place of the "default" mark (NULL: none).

### `opt_choice_list()`

A choice over a list the caller builds (devices, window sizes): `values[i]` is written, `names[i]` shown; `""` means
the key unset. `cur`: the index in effect, -1 to find `cfg_value(key)` in `values` (no match: the value shown as it
is).

### `opt_choice_index()`

A choice whose value is spread over several settings: the caller gives the index in effect and the default's, and
writes the settings itself (`opt_apply()`) when this returns the new index; -1 while unchanged. `key`: the setting the
row stands for (its restart mark and the environment check). `again`: the value shown at `cur` is in effect without
being the setting's own (the quality preset's Recommended while a tier chosen on other hardware gives way to it), so
choosing it is a change: a step past either end (left on the first value, right on the last) returns `cur`.

### `opt_apply()`

What a row writes: `""` unsets the key. The write joins the draft.

### `opt_context()`

The row the `opt_apply()` calls that follow belong to, as `opt_choice_index()` sets it, for a choice made later (in a
box opened by the row): `key` the row's setting, `from` and `to` its shown values.

### `opt_button()`

A row that runs an action: true on A or a click. `id`: unique on the page and not a setting (`##copy_info`); `value`:
shown on the right (`""` none).

### `opt_toggle_text()`, `opt_group_text()`, `opt_text_str()`

Rows and text that are not settings of the settings table: a label and description of the caller's own (the cheats of
the cheat files). `opt_toggle_text()` shows on / off (`value`: shown instead, NULL for On / Off) and returns true on
A, a click, left or right; the caller changes its state. `id`: unique on the page; the environment check uses it.

### `opt_slider()`

`lo`..`hi` in steps of `step` (left and right held for a second go ten times as fast); `fmt` formats the value
(`"%.0f %%"`). `note`: a short line under the value in place of the "default" mark (NULL: none). `fallback`: the
setting that applies while `key` is unset; the row shows its value then, and a change sets `key`. A key with no table
default of its own takes the "default" mark from the fallback's default, and a step onto the fallback's value unsets
`key` again.

### `opt_slider_stops()`

Whole numbers `lo`..`hi` that go on into named stops past `hi` (`stop_values[k]` written, `stop_names[k]` shown): left
and right step one, held they step ten through the numbers and stop at `hi` before the stops; A goes round. `fmt`
formats a number (`"%d FPS"`). `cur`: the value in effect when it differs from `cfg_value(key)` (NULL: `cfg_value`).
`note`: as for `opt_slider()`.

### `opt_bind_header()`, `opt_bind_row()`

Binding rows (remapping on the Controls tab): a label and three cells, each selectable on its own (left / right move
between them). `opt_bind_header()`: the column titles above them. `opt_bind_row()`: `key` is the setting's name
without `NX2SARIA_` (`BIND_A`; the description names it when the environment sets it); a disabled cell shows its text
greyed and takes no cursor; `note` (NULL none) follows the description. Returns the cell A / Enter / a click activated
(0..2), 10 + the cell Delete, Backspace or X cleared, or -1. `opt_focus_key("<key>#<cell>")` puts the cursor on a
cell.

### `opt_value()`, `opt_get()`

The draft. Changes made in the menu are not saved as they are made: they wait in a draft until the player applies
them (`runtime/ui/menu.cpp` asks when the menu is about to close).

- A preview key (a live setting that is cheap to show: volume, sharpness, the brightness, the bindings) is written at
  once with `cfg_set` so the effect shows; the first original is kept and Discard puts it back.
- A staged key only waits; Apply writes it. Staged are every setting read at start, and the live settings
  `FPS_LIMIT`, `VK_PRESENT`, `HDR`, `PAD_LAYOUT`, `WALK_KEY`, `MOTION`, `FULLSCREEN`, `WINDOW_SIZE`, `FOV` and
  `PRESET`, which would swap the picture, the pacing or the buttons under the player, and a cheat's key
  (`runtime/ui/option_rows.cpp` `is_staged()`).

Nothing is written to the settings file before Apply (`opt_draft_apply()`). A change back to the original value leaves
the draft. After Apply has saved the file it writes the session header again (`runtime/session_summary.c`
`session_summary_header()`), so a hard crash's summary carries the settings in use then; once per Apply, never per
frame. Rows read their setting through `opt_value()` and its siblings so a staged change shows; anything else
reads `cfg_value`.

`opt_value()`: the draft's value of a staged key, else `cfg_value`. `opt_get()`: as `opt_value()`, else `cfg_get`
(NULL for a key not set).

### `opt_before()`

The value `key` had in effect before the draft's first change of it (a `Change`'s `orig_eff`), else `cfg_value`: what
the game ran with when the menu opened, whether the key is previewed (already written) or staged.

### `opt_preset_tier()`, `opt_default()`, `opt_is_set()`

The quality preset in the draft (`runtime/ui/menu.cpp` `row_preset()`): while the preset key is staged, a key the
player leaves unset (or the draft hands back to the preset with `""`) reads the staged tier's value
(`cfg_default_tier`) in `opt_value()`, `opt_get()` and the rows' "default" marks, and Apply switches the layer to it
after the draft's writes (`cfg_preset_use_from`, `cfg_preset_chosen`). Discard leaves the layer alone. The feature:
[presets.md](../presets.md).

`opt_preset_tier()`: the tier the draft stages, -1 while it stages none. `opt_default()`: what `key` reads unset, the
staged tier's default, else `cfg_default`. `opt_is_set()`: the player sets `key`, as the draft has it (a staged `""`
is unset).

### `opt_preset_handback()`

The keys that choosing `tier` in the menu hands back to the preset (`row_preset()`): of `cfg_preset_keys`, those
`tier` sets that the player sets, as the draft has them, to another value than the key reads unset with `tier` in use
(`cfg_preset_differs`); not the ones the environment fixes. Fills `names` (`NX2SARIA_` + key) and returns how many.

### `opt_pending_count()`

`cfg_pending_count` over the values the rows show: the restart-only settings whose draft or setting differs from the
value the game started with (the Restart button applies the draft first, so its count takes the draft in).

### `opt_change()`

A change made outside a row (a binding): `key` (with or without `NX2SARIA_`) to `value` (`""` unsets it), named
`label` in the list, which shows `from` -> `to`.

### `opt_draft_any()`, `opt_draft_count()`, `opt_draft_item()`, `opt_draft_sync()`

`opt_draft_any()`: changes in the draft, hidden ones included; any thread. `opt_draft_count()`: the lines the player
is shown. `opt_draft_item()`: the i-th line, 0-based; false past the last. In `OptDraftItem`, `restart` says some key
of the line is read at start only, and `display` that a timed Apply holds it for the "keep these display settings?"
box, whose Revert puts it back. `opt_draft_sync()`: once a frame, a key something else changed leaves the draft.

### `opt_draft_apply()`

Writes the staged keys, releases the previews and saves; false if the write failed. `timed`: `FULLSCREEN`,
`WINDOW_SIZE`, `VK_PRESENT` and `HDR` (not to or from the game's own), with the other staged keys of the row that
changed them (the Resolution row's `GAME_RES`), are written but stay held out of every save until
`opt_display_keep()` (the "keep these?" timer); the caller then wakes the window (`gfx_window_apply_settings()`) and
runs the timer. Without it every key is saved (Restart).

### `opt_display_pending()`, `opt_display_external()`, `opt_display_keep()`, `opt_display_revert()`

`opt_display_pending()`: display keys written by a timed apply and not yet kept or reverted; present thread.
`opt_display_external()`: 1 when something else changed one of them since (F11); the player has control.
`opt_display_keep()`: releases them and saves the settings file; false if the write failed. `opt_display_revert()`:
puts the values from before the apply back (the caller wakes the window again) and returns true, or, when
`opt_display_external()`, keeps what is on screen instead and returns false.

### `opt_draft_discard()`

Puts the previews back and drops the staged keys.

### `opt_save_now()`

Writes the settings file now.

## `runtime/ui/option_rows.cpp`

The two-pane settings pages of `runtime/ui/option_rows.h`: rows on the left, the selected or hovered row's description
on the right. The rows read and write settings, take their text from the menu catalog (`menu_text`), and show the
default, "after restart" and environment notes.

`imgui_internal.h` is included for `SetKeyOwner`, `SetFocusID` and `NavCursorVisible`. `host.h` pulls in `cpu.h`,
which is C only, so the file declares the one function it needs from it, `rt_log`, as it is declared there.

Page state:

- `g_focus_req` and `g_take_first`: `opt_focus_first()` was requested, and this page's first row takes it.
- `g_focus_key`: `opt_focus_key()`, the row that takes the cursor when it is next drawn.
- `g_held`: the last row's left / right has been held a while (steppers go faster).
- `g_bottom`: `opt_reserve_bottom()`, the height kept free under the panes.
- `g_disable_next`: `opt_disable_next()`, the next row is greyed out with `g_disable_note`.
- `g_cursor`: the rows' cursor, drawn once all rows are (`opt_end()`), its glow in the layer under the rows and its
  frame in the one over them: the selected row's box, else the hovered one's (`alpha`: fainter for a hovered row, and
  for one the environment fixes).
- `g_hover`, `g_focus`, `g_first`, `g_shown` (`Desc`): the description pane's text of this frame's hovered and
  selected rows, of the page's first row, and the last one shown (of the page `g_page`: another page starts from its
  first row's, however it was opened).

`RowView`, what a row shows besides its label: `value`, the right side (`""` for none); `is_default`, the value is the
setting's default (the "default" mark, the value not highlighted); `arrows`, a stepper `[<] value [>]`, on which the
mouse changes the value on the two framed arrows only; `note`, under the value instead of the "default" mark (NULL for
none); `col`, the width of the widest value the row can show (`value_width()`; 0: this value's); `end_l` and `end_r`,
the value is the first / last one, so that arrow is drawn faint and does nothing; `subtitle`, under the label, before
any status note.

### `row_margin()`

The room left on each side of a row for its cursor, which is drawn outside the row's box: 0.85 of
`skin_cursor_reach()`, rounded up.

### `desc_set()`

`disabled` is the note of a row `opt_disable_next()` greyed out, else NULL.

### `opt_begin()`

- When the first row takes the cursor, this frame's navigation move is cancelled: it goes to the first row instead.
- The rows sit far enough apart (9 pixels at scale 1) that the selected row's cursor frame clears its neighbours'
  boxes, and a dummy of `row_margin()` keeps the first row's cursor inside the pane.
- The draw list is split in three layers: the cursor's glow under the rows, the rows, the cursor's frame over them
  (`opt_end()` draws the cursor).

### `opt_end()`

A dummy of `row_margin()` keeps the last row's cursor inside the pane. The description shown: the hovered row's
(mouse) wins, else the selected one's (controller), else the last shown, else the first row's. It is drawn on a dark
panel framed like the rows' buttons.

### `value_width()`

The width of the widest of `n` values at the current font size: a stepper's left arrow stands that far from the right
one whatever value shows, so it stays under the pointer while it is clicked through the values.

### `arrow_box()`

One framed arrow of a stepper over [a, b]: a white frame with the arrow in its middle, filled a little while the
pointer is on it and more while it is pressed; faint when it does nothing (`dim`: the row is greyed out or the value
is at this end).

### `click_test()`

A test hook. `NX2SARIA_MENU_TEST_CLICK=<key>:<part>[,<key>:<part>...]`, with `NX2SARIA_MENU_TEST` opening the menu on
the page that has the rows (`runtime/ui/menu.cpp`): mouse clicks on steppers by their setting's name (without
`NX2SARIA_`), `part` being `l` (the left arrow), `r` (the right arrow) or `b` (the row's body, on its label). One
click after another, each over a dozen frames: the pointer goes there, the button goes down and up, and the log says
what the setting is then. So a run checks that the arrows step a setting and that an arrow at its end and a click on
the body do not, with the fonts and sizes of the game. At most 24 clicks.

### `row_text()`

One row: draws it, handles A / click and left / right while selected. Returns 2 (A, or a click on a row without
arrows), -1 / +1 (left / right, or the mouse on a stepper's framed arrow), 3 / 4 (move up / down, movable rows only)
or 0; always 0 for a row the environment fixes (greyed out, still selectable so its description shows). `movable`: the
top / left face buttons, Ctrl+Up / Ctrl+Down and two drawn arrows move it. No row is movable now: the row that was, a
mod's folder in the Mods tab, is gone, and every caller passes `false`.

A stepper takes the mouse on its two framed arrows only: a click anywhere else on the row selects it and changes
nothing. A click on the row used to step to the next value, and the left arrow stood left of the value's text, so it
moved when a value of another width came up and the next click at the same place landed on the row instead.

- The row's box is inset from the pane's sides by the room its cursor needs.
- Its height is whole pixels (`floorf(fs * 2.2)`): ImGui truncates cursor positions, which rounds a fractional height
  the other way above the window's top (negative y), so the page grew a pixel per row scrolled out and a row scrolled
  to the bottom was cut off.
- Its width is at least a pixel: `InvisibleButton` asserts on a zero size (a narrow window), and ImGui reads a
  negative one as "up to that far before the right edge", away from the box drawn.
- ImGui's navigation cursor is transparent: the row's own border shows the selection.
- A row taking the cursor as the page's first sets the scroll to 0 (the page's top: text and a heading above the first
  row show); a row focused by key scrolls as a cursor move would (`ImGuiScrollFlags_KeepVisibleEdgeY`: the last row
  whole).
- `by_mouse` is a click of the mouse, not A / Enter / Space: only a click picks an arrow by where the pointer is.
- The left stick arrives as the D-pad (`runtime/ui/menu.cpp` `pad_events()`).
- Left and right belong to the row (`SetKeyOwner`): navigation would otherwise move sideways into the description
  pane.
- The box is the save menu's brown button, brighter while selected (lift 1.0) or hovered (0.6); the white cursor goes
  round it in `opt_end()`.
- The label line is at 0.25 of the font size from the top and the note line under it at 1.3; `right` is the value's
  right edge.
- The value and its note take at most 60 % of the row: a longer value (a graphics card's full name) is drawn smaller,
  down to 75 % of the font size, then clipped, so the label keeps room and nothing reaches past the box.
- A stepper, `[<] value [>]`: the arrows are drawn (the font has no arrow glyphs), each in a white frame. The frames
  stand in fixed places: the right one at the row's right edge, the left one the width of the row's widest value
  before it. They fit over the note line, in the label line's height (1.3 x 1.02 of the font size, a gap of 0.4). The
  value and its note are centred between the arrows (`value_mid`). A value wider than the room between the arrows is
  drawn smaller, then clipped.
- The pointer is on an arrow within its frame and two pixels around it (at scale 1), so a click on the frame's own
  line counts. A mouse click on the row itself only selects it.
- On an arrow, the press steps, and a held button repeats as a held key does; after a second it also goes ten steps at
  a time over a long range (`opt_slider()`), as held left / right do.
- A movable row has up and down arrows left of the value; a click on one moves the row. A fixed value column (On / Off
  differ in width; at least 2.8 of the font size) keeps the arrows of the rows in one line.
- The note under the value (or the "default" mark) is right-aligned with it, and its start shows when it is too long;
  a stepper's note is centred under its value, as far as the row's right edge lets it. `note_right` is where the
  label's own note stops: short of the value's note, when there is one.
- The label and its note are clipped before the value. A restart-only setting says "after restart" while the value the
  row shows (the draft's, before Apply) differs from the one the game started with (`cfg_pending_for`), so the note
  marks what a restart would change; the description says it whatever the value (`desc_set()`).

### `row()`

A key that nothing on this game version reads (`cfg_supported`) gets no row, and no grey-out is left over for the next
row.

### `norm_key()`

The draft: what the player changed since the menu opened and has not applied.

A change is one of two kinds (`is_staged()`). A preview change is written with `cfg_set` at once, so the effect shows
(a volume slider, the sharpness, the bindings), while its first original is kept: Discard puts it back (`cfg_revert`),
and the entry is held (`cfg_hold`) so no save made meanwhile writes it. A staged change only waits in the draft, the
row shows it (`opt_value()`), and Apply is what writes it. Nothing reaches the settings file before Apply.

The player sees one line per group (`Group`): the row (or the binding) that made the changes, from what it showed to
what it shows. A row that sets several keys (the upscaler) is one group of several changes.

A `Change`: `key` is `NX2SARIA_` + upper case (`KEY_PREFIX` is that prefix's length; the log names the key after it);
`gid` the group; `new_val` as the row wrote it (`""` is the key unset); `orig_eff` the value in effect before the
first change; `orig_val` `cfg_state`'s, for `cfg_revert`; `orig_set`, the player had set the key (`cfg_is_set`) before
the first change. `g_draft_n` is `g_changes.size()`, for any thread (`runtime/ui/menu.cpp` `menu_toggle()`). `g_ctx`
is the row `opt_choice_index()` answered for, which the `opt_apply()` calls that follow belong to.

`norm_key()` gives the draft's spelling of a setting's name: `NX2SARIA_` and the key in upper case, from the key with
that prefix, with the earlier `NX2NAVI_` or with none.

### `is_staged()`

Keys that are live in the table but stay in the draft until Apply: the window, the present mode, HDR and the frame
limit change the whole picture or the pacing at once (HDR makes the swapchain again in another format; the "keep
these?" timer is on all of them but the frame limit), a pad layout or walk key swaps what the buttons do under the
player's hands, and motion starts or stops a device. The quality preset switches its layer of defaults only at Apply
(`opt_draft_apply()`, `cfg_preset_use_from`). Every key the table marks as read at start is staged too.

A key without a row in the settings table is a preview key (the bindings), except a cheat's (`CHEAT_<hash>`): it waits
in the draft until Apply. A preview would have to switch the cheat for the player to judge it, and a cheat writes the
game's memory or switches a patch set, which Discard could not take back.

### `eff_now()`

The applied value of a key, and what a value written for it comes to once `""` (unset) is its default. The quality
preset chosen on other hardware (`cfg_preset_hw_changed`) reads as a value of its own, as the recommended tier is in
use in its place: choosing the same tier again, or Recommended, is a change that Apply makes (`cfg_preset_chosen`).

### `preset_tier_of()`

The tier of a value of the preset key: `""` and `recommended` are the tier this PC gets.

### `staged_tier()`

The tier the draft stages for the preset key, -1 while it stages none.

### `default_of()`

What `""` (unset) comes to: the default of the tier the draft stages, else `cfg_default` (the tier in use).

### `eff_written()`

What a preview key written as `value` is in effect now: the tier in use gives `""`, whatever tier the draft stages.

### `write_key()`

An empty value is the automatic choice: the key unset, its line removed from the settings file.

### `no_effect()`

A change that leaves the key at the value it had. A key set back to `""` (unset) stays in the draft so when a tier
staged after it gives the value it had, as does one a preset choice handed back (`record()`'s `handed`): `game_res =
1080` in the file, Ultra chosen (it hands `GAME_RES` back for its "window"), then Low, whose 1080 is the value the key
had. Apply still writes it, so a line the player had leaves the settings file and a later tier sets the key, but it is
no change: Apply does not count it or list it as one, and it adds no restart (`opt_draft_item()`).

### `shown()`

A value as the log shows it: quoted, or `(default)` for an unset key.

### `record()`

Puts a key at a value in the draft. A change back to the value the key had when the draft began leaves the draft. The
log names a key when it enters the draft and when it leaves it (a slider dragged through many values is not every
step); Apply and Discard list the values they settle on.

A key the player set that a preset choice hands back to the preset (`runtime/ui/menu.cpp` `row_preset()`) stays in the
draft though the preset gives it the same value (`handed`): its line leaves the settings file, so a later tier sets it
again.

### `opt_value()`

With a staged preset, a key the player leaves unset (a preview reset included) shows the staged tier's value.

### `opt_get()`

With a staged preset whose tier changes the key's default, it returns that default, as `cfg_get` gives a layer's
value.

### `opt_int()`

A value that does not parse gives the default's number (`cfg_int`).

### `opt_draft_count()`

The lines the player is shown: the groups whose text changed (an upscaler picked and picked back leaves keys written
but nothing to say).

### `live_needs_restart()`

Live settings that still wait for a restart in some states (the rows' own "after restart" notes in
`runtime/ui/menu.cpp`): the upscaler cannot start in this run (`vk_dlaa_needs_restart`, for `DLAA`, `DLSS`, `TAA` and
`DLSS_PRESET`); NVIDIA frame generation is set up with the device (`vk_dlssg_wanted`) or, through NGX (every build but
the Windows one), needs NGX up since the start (`vk_ngxfg_wanted`; `runtime/ui/menu.cpp` `framegen_restart()`); HDR to
or from the game's own, or on in a run started without the HDR path (`vk_hdr_restart_for`); and low latency on in a
run started with it off (`runtime/ui/menu.cpp` `menu_lowlat_restart_for()`).

### `is_timed()`

The display keys the "keep these?" timer watches after an Apply (`opt_draft_apply()`): `FULLSCREEN`, `WINDOW_SIZE`,
`VK_PRESENT`, and HDR only between off and the tone mapper, as to or from the game's own it waits for a restart
(`live_needs_restart()`).

### `group_timed()`

A row that changed a timed key and others with it is kept or reverted as one: the Resolution row
(`runtime/ui/menu.cpp` `row_resolution()`) sets the window's size and the game resolution together, so a Revert that
put the window back and kept the new resolution for the next start would leave the two apart again.

### `opt_draft_item()`

A preset choice also changes the restart-only keys the player leaves to it (`GAME_RES`, `FOLIAGE`, `SHADOW_PCF`): its
line waits for a restart when one of them, as the staged tier gives it, differs from the start's value.

### `opt_draft_sync()`

Something else changed a key of the draft (F11 toggled the fullscreen the draft also changes, F10 the overlay): the
change that came from outside stays and that key leaves the draft.

### `opt_draft_apply()`

A staged key may have no table row (a cheat's), so the "takes effect after a restart" part of its log line asks the
row only when there is one.

The display keys a "keep these?" timer watches (`runtime/ui/menu.cpp` `post_confirm()`): an Apply with `timed` set
writes `FULLSCREEN`, `WINDOW_SIZE`, `VK_PRESENT` and `HDR` (`is_timed()`), and the other staged keys of a row that
changed one of them (`group_timed()`: the Resolution row's `GAME_RES`), but keeps them held (`cfg_hold`), so no save
made while the timer runs (F10, a cheat, the window closing) writes them; `opt_display_keep()` saves them,
`opt_display_revert()` puts the values from before the Apply back. They are the live keys that change the window or
the swapchain and so can leave the player without a usable picture. HDR to or from the game's own and `GPU_INDEX` wait
for a restart and are not here, nor are the other live keys. A `Pending` holds the key and `applied`, the value in
effect right after the Apply.

- A preset choice (`row_preset()`): its layer switches after the draft's writes, and its log lines start from the
  values the keys had before the draft (a key handed back to the preset was reset, a preview one already).
- A timed key is held (`cfg_hold`) before the write, so no save in between can take the new value.
- The save at the end (`opt_save_now()`) leaves the held display keys out.

### `opt_display_external()`

F11 changed `FULLSCREEN` (or anything else set a pending key) after the Apply.

### `opt_display_revert()`

With `opt_display_external()` the player took control with F11: what is on screen now stays.

### `apply()`

What a row writes: the draft takes it (a preview key is written too), and the cursor sound plays.

### `on_off_width()`

An on / off row is a stepper too: either arrow, A, left or right flips it. The value column is as wide as the wider of
On and Off.

### `opt_toggle()`

`on` is what the row shows (the setting, inverted when `inverted`). Any step flips it.

### `opt_toggle_value()`

The default is the one the player's profile has (`cfg_default`: the Steam Deck's `fifo`), which is also what "on"
writes when it is an on value, so turning the switch off and on again puts the profile's own value back. Any step
flips it.

### `opt_button()`

A or a click; left and right do nothing.

### `opt_toggle_text()`

Off is the plain state: its value is drawn dim, without the "default" mark. Any step flips it.

### `group_divider()`

A group heading: the game's divider ornament across the pane, then the title in the accent colour.

### `column_text()`

Text across the rows' column, inset from its sides as the rows' boxes are and wrapped before their right edge. The
cursor already stands at the inset when `indented` is true (after `group_divider()`).

### `opt_choice_list()`

A value not in the list: left goes to the end, right / A to the start. A / click takes the next value and goes round
to the first; left / right stop at the ends.

### `opt_apply()`

As `apply()`, without its sound: for a row writing several keys. A write that is not under a row (no `g_ctx`) is named
by its key.

### `opt_choice()`

A key with no fixed default gets `""`, the automatic choice, as its first value.

### `opt_slider()`

- Unset, the row shows the value in effect, the fallback's.
- A key without a default of its own (the per-stick dead zones) defaults to its fallback: the "default" mark follows
  the fallback's default, not 0, even once the key is set.
- Held, it goes ten steps at a time over a long range (more than 40 steps).
- A / click past the top goes round to `lo`; left / right stop at the ends.
- Landing on the fallback's value unsets the key (its line leaves the settings file), so the row follows the fallback
  again.

### `opt_slider_stops()`

A whole-number slider from `lo` to `hi` that goes on into named stops past `hi` (the frame limit's monitor rate and
unlimited). Positions are `lo`..`hi` and then `hi + 1 + k` for stop `k`. Left and right step one; held for a second
over a long range (more than 40) they step ten through the numbers, and stop at `hi` before the first named stop, so a
quick sweep up does not run past the last number. A / a click steps one and goes round to `lo` after the last stop.
`cur`: the value in effect (NULL: `opt_value(key)`, the draft's value); a value that is neither a stop nor a number
shows the default's position.

Ten at a time goes among the numbers only: from `hi`, right goes one into the named stops (never past the first).

### `fit_text()`

The binding rows (the remapping rows of the Controls tab) are a label and three cells, each its own navigation item.

Text centred (left-aligned when `centred` is false) in [x0, x1] on a row from y to y + h, at the font size or smaller
so a long name ("Нажатие правого стика", "Стрелка вправо") fits; under 65 % it is clipped instead.

### `bind_columns()`

The label column (34 % of the width) and the three cells of a binding row of width `w`. A cell is a pixel wide at
least: `InvisibleButton` asserts on a zero width.

### `opt_bind_header()`

The titles go over the cells of the binding rows: the same inset (`opt_bind_row()`) and the same columns.

### `opt_bind_row()`

- The row is inset by `row_margin()`: room for the cursor, as the option rows leave.
- A disabled cell is not a navigation item: the cursor skips it.
- ImGui's navigation cursor is transparent: the cell's own border shows the selection. Each cell is a small brown
  button.
- The note (for example what moved here) follows the description while the row is selected or hovered.

## `runtime/ui/menu_skin.h`

The look of the game's own menus (`runtime/ui/menu_skin.cpp`): the save menu's brown button with its inset line and
corner ornaments, the options menu's white selection cursor and its filigree divider, and a dark panel framed like the
buttons. Each is drawn from the game's textures when they are there (`runtime/ui/menu_art.cpp`), else from plain
shapes in the same colours. Sizes are in pixels at 1080 lines; `s` scales them (`runtime/ui/menu.cpp`
`menu_scale()`).

The colours the boxes are drawn in, for text on them: `SKIN_TEXT` (244, 236, 214) for labels on a button,
`SKIN_TEXT_DIM` (206, 188, 150) for notes on a button, `SKIN_GOLD` (236, 196, 104) for headings and accents.

### `skin_art_prepare()`

What the art needs after it is decoded: the cursor's frame is cut out of its texture here (`art` is a `MenuArt`).

### `skin_art_halve()`

One level of an art at half the size of the last: `dst` is `dw` x `dh` (each half of the source's, at least 1).

### `skin_style()`

The settings menu's Dear ImGui style at 1080 lines: the colours of the game's menus (a dark umber panel, warm browns
for frames and what is selected, gold ornaments and accents, parchment text) and the sizes of its frames.

### `skin_atlas_add()`, `skin_atlas_fill()`

The boxes' mottled grain lives in the font atlas: `skin_atlas_add()` before the atlas is built (it reserves the tile),
`skin_atlas_fill()` after it is built and before it is uploaded (it draws the tile). Without them the boxes are plain.

### `skin_button()`

The brown button over [a, b]: a dark rim, a light edge, the fill, and an inset line that stops short of a diamond at
the top-left and bottom-right corners and of a curl at the other two. `lift` 0..1 brightens the fill (hovered or
selected); `dim` mutes it (a row the environment fixes).

### `skin_cursor_reach()`

How far the selection cursor reaches outside the box it surrounds: a box needs that much room on every side.

### `skin_cursor_glow()`, `skin_cursor_frame()`

The selection cursor around the box [a, b], in two parts: a soft white-blue glow, drawn before the box (the box hides
its middle, so it shows only around it), and a white frame a little outside the box, drawn after it. Both breathe
slowly; `alpha` 0..1 (a hovered row's cursor is fainter than the selected one's).

### `skin_filigree()`

The divider: a thin line with a dot at each end and the ornament in its middle, centred on `y` between `x0` and `x1`.

### `skin_panel()`

A dark panel over [a, b] with the button's edge, inset line and corner ornaments (the description pane, the
performance overlay). `alpha` scales its fill.

### `skin_boot_panel()`

The boot screen's box (`runtime/ui/menu.cpp` `menu_boot_frame()`) centred in a w x h frame: the panel, the title in
gold over the divider, the step's line, a bar filled to `frac` (0..1; a negative `frac`: a short segment sweeping at
position `sweep` 0..1, no count yet).

### `skin_widget_button()`

A button widget drawn as `skin_button()` with its label centred and the selection cursor round it while selected or
hovered (the save menu's Load and Cancel). True when pressed (click, A, Enter). `size`: as for `ImGui::Button`.

### `skin_tab_item()`

One tab of the bar: `ImGui::BeginTabItem` drawn as the rows' button, with the game's plate behind the selected tab's
lines and its text in gold. `id`: `BeginTabItem`'s label (it sizes the tab); `text`: what is shown. `font`: the
label's font, NULL for the current one. True when selected: call `ImGui::EndTabItem` then.

### `skin_header_tab()`

A tab's name in the game's pause menu header, drawn without a widget: the selected category's plate (`a` to `b`)
behind it when `current`, the text centred on `text_at` with a soft shadow, in `font` at `font_px`, all at `alpha`.
The text is drawn twice, the second a little to the right (`font_px / 36`, at least 0.6 px), for the weight of the
game's category names.

## `runtime/ui/menu_skin.cpp`

With the game's textures (`runtime/ui/menu_art.cpp`) each piece is those textures tinted and laid over each other in
nine slices. Until they are decoded, or without them, it is plain shapes whose colours and proportions are sampled
from the game: the save menu's Load and Cancel buttons (brown fill, light edge, an inset line about a twentieth of the
height in with a diamond and a curl at alternate corners), the options menu's cursor (a 3 px white frame with a soft
white-blue glow). Without the textures the divider is a plain line with a diamond in its middle and a dot at each end,
a shape of its own, not the game's ornament.

`imgui_internal.h` is included for `NavCursorVisible`.

- The grain: the mottled grain of the game's brown boxes is a tile in the font atlas (`skin_atlas_add()`,
  `skin_atlas_fill()`), `kGrainW` x `kGrainH` = 160 x 48, white with the grain in alpha, drawn over a box darkened in
  some places and lightened in others.
- The colours: a dark umber panel, warm browns for frames and what is selected, gold for the ornaments, accents and
  the cursor, parchment text. `kGreen` and `kOcarina` keep their names for the selected and active controls.
- Where the art's boxes lie in its textures, in texels, measured from the textures. The button's layers are 489 x 133
  (`kBtnW`, `kBtnH`) with the button's box from (20, 19) to (469, 114) (`kBtnL`, `kBtnT`; `kBtnBoxH` 95, `kBtnCut`
  60). The cursor's are 640 x 173 (`kCurW`, `kCurH`) and its box, the button it surrounds, runs from (49, 48) to
  (591, 125) (`kCurX`, `kCurY`; `kCurCut` 70, `kCurScale` 0.72). The panel's box runs from (53, 53) to (1147, 374) in
  1200 x 427 (`kPanelW`, `kPanelH`, `kPanelM`). The selected tab's plate lies from (25, 0) to (336, 151) in its
  360 x 173 texture (`kTabL`, `kTabT`, `kTabR`, `kTabB`).

### `curl()`

A spiral of `turns` turns around `c`, from radius `r` at angle `a0` (radians, clockwise on screen) in to 35 % of it,
in 28 segments.

### `ornament_line()`

The inset line of the button and panel frames, `inset` inside [a, b]: a diamond at the top-left and bottom-right
corners, a curl at the top-right and bottom-left ones, the four lines running between them with a small gap.

The ornaments grow with the box as on the game's buttons (a curl about a tenth of the height across), within limits:
the curl's radius is 0.13 of the shorter side, between 3.5 and 7.0 pixels at scale 1; the diamond is 0.6 of that, the
gap 2.4 and the line 1.4 thick. The top line runs from the diamond into the curl, whose outer turn starts at the top
of its circle (angle -pi/2, 1.3 turns); the right line starts under that curl; the bottom line runs into the other
curl; the left line ends over it.

### `axis()`

One axis of a slice: screen [a, b] from texels [x0, x1] of a texture `size` long, the outer `cut` texels `c` pixels
long. The middle is stretched, or with `tile` repeated at `k` pixels per texel: an odd number of copies, every other
one mirrored, so each copy meets the next on the same texels and the last meets the far corner. Mottled art stretched
across a wide box would turn into streaks.

### `slice()`

The texels (x0, y0)-(x1, y1) of an art over [a, b] in nine slices: the outer `cut` texels on each side at `k` screen
pixels per texel, so corners keep their shape, the rest stretched or tiled. A box smaller than its two corners shrinks
them.

### `btn_layer()`

A button layer over the box [a, b]: the whole texture, reaching past the box by its margins.

### `cursor_layer()`

A cursor layer around the box [a, b], at a fixed size so its frame keeps its width on any box.

### `skin_art_prepare()`

The cursor line's texture is not the line: its alpha climbs from 0 at the outer edge to full about 28 texels in, and
is full inside. The white frame is a band of that ramp, about 3.5 texels wide just outside the box it surrounds (alpha
102 lies about six texels out from the cursor's box), so the frame follows the texture's own corners. The band is
`mid` 102, `full` 16, `soft` 8, in alpha steps; the ramp climbs about 8.7 of them per texel.

### `skin_art_halve()`

A box filter over the 2 x 2 texels each destination texel covers (an odd last row or column repeats). Colour is
weighted by alpha, so what is transparent does not bleed into the edges of the coloured art.

### `menu_art_draw9_turned()`

Nine pieces: the corners at `corner` pixels on screen from the texture's outer `frac` of each side, the edges
stretched between them, the middle left out (frames) or stretched (fill); the texture turned half a turn when
`turned`, its top-left corner piece at the bottom right.

### `grain()`

The grain over [a, b]: the tile repeated at four pixels per texel (its noise wraps, so the copies join unseen), so the
blotches keep their size on any box; darkening where the tile is dense, lightening where its mirror image is. The
mirror image is the tile read from its far corner, over the same piece.

### `skin_atlas_fill()`

The tile is value noise of three octaves on a lattice that wraps, with a little fine speckle: blotches and grain. The
lattice hash multiplies by 374761393, 668265263 and 2246822519 in `unsigned`, so the products wrap.

### `button_art()`

The button from the game's layers; false while they are not there. `plate`: the selected tab's plate under the lines.

### `skin_button()`

The plain button: first the dark rim between the button and what is behind it (28, 22, 14 at 0.92), later the light
edge (126, 104, 68; dimmed 98, 88, 74; 1.4 thick).

### `breath()`

One slow breath every 1.6 s, as the game's cursor pulses.

### `skin_cursor_glow()`

Without the art: 12 filled rounded boxes stacked from the outside in, each faint, so the glow brightens towards the
frame; the box drawn over them hides their middle.

### `skin_cursor_frame()`

Without the art: a little glow in the gap between the box and the frame, then the white frame.

### `filigree_art()`

The divider from the game's art: the ornament (its ink runs from (28, 24) to (200, 65) in 228 x 80) and on each side
the line texture, turned round for the left one: its stroke from x = 38 to 217 stretched, then the dot up to x = 232
at its own proportions, the whole on the row y = 23 of 240 x 43.

### `skin_filigree()`

Without the art, the middle is a diamond outline with a small filled one inside, and a short stroke on each side of
it.

### `skin_panel()`

With the art: the panel's own texture inside its box, with the button's edge and inset line at a size that suits a
large box (0.62 of the scale).

### `skin_widget_button()`

ImGui's own navigation cursor is made transparent: the skin's cursor shows the selection. The glow is drawn under the
button (full for the focused button, 0.55 for a hovered one).

### `skin_tab_item()`

Dear ImGui keeps the bar's behaviour (selecting, sizing, the D-pad) and lays out the tab with everything it would draw
made invisible; the button and the text go over its rectangle here. The button's bottom is raised by 4 pixels at scale
1 to stay clear of the line under the tabs.

### `skin_boot_panel()`

The bar is a sunken track with the frames' brown edge, filled in gold from the left.

### `skin_header_tab()`

The plate is the one the game draws behind its selected category, measured on a bench capture: a dark brown band from
the top of the picture down past the header's edge, (63, 34, 13) at the top to (104, 76, 42) at the bottom, with a
darker line at each side; the text is the categories' cream, (254, 252, 225).

## `runtime/ui/menu_art.h`

The settings menu's art from the game's own interface textures (`runtime/ui/menu_art.cpp`).

`MenuArt`, the arts:

| Id | What it is |
|---|---|
| `MENU_ART_FRAME_DECO` | a frame with ornamented corners |
| `MENU_ART_GUIDE_BAR` | the control guide's bar |
| `MENU_ART_WINDOW_BASE` | a rounded window base |
| `MENU_ART_BTN_RIM` | a soft line along the button's edge, reaching outside it |
| `MENU_ART_BTN_FILL` | the button's shape |
| `MENU_ART_BTN_GRAIN` | the mottling: light at the edge, dark and speckled in the middle |
| `MENU_ART_BTN_EDGE` | the light edge |
| `MENU_ART_BTN_LINE` | the inset line, a diamond and a curl at alternate corners |
| `MENU_ART_CURSOR_GLOW` | the options cursor's soft glow |
| `MENU_ART_CURSOR_LINE` | the options cursor's white frame (`runtime/ui/menu_skin.cpp` `skin_art_prepare()` cuts it out of the texture) |
| `MENU_ART_PANEL_FILL` | a dark brown panel |
| `MENU_ART_DIVIDER` | the options menu's divider ornament |
| `MENU_ART_DIVIDER_LINE` | the line beside the ornament, ending in a dot |
| `MENU_ART_TAB_SELECT` | the plate of the selected category in the game's menu header |

The five `MENU_ART_BTN_*` are the layers of the save menu's Load and Cancel button, in drawing order; they are white
masks except for the grain.

### `menu_art_start()`

Decodes the textures on a worker, once.

### `menu_art()`

The texture for the menu's draw lists and its size in pixels; 0 while it is not there (still decoding, or absent).
Window thread: the first call for an art uploads it.

### `menu_art_release()`

Called before ImGui's Vulkan back end shuts down; the font atlas goes too.

### `menu_font_upload()`

The font atlas's pixels (w x h, RGBA) as a texture for the draw lists in `*tex`; 0 when it could not be made, the
previous atlas then kept. The previous one is freed once the queue has finished with it.

### `menu_vk_errors()`

Results other than `VK_SUCCESS` that Dear ImGui's Vulkan back end reported so far (`runtime/ui/menu.cpp`
`menu_vk_check()`).

### `menu_art_draw()`

Draws an art stretched over [a, b] with a tint; 0 (nothing drawn) while it is not there.

### `menu_art_draw9()`, `menu_art_draw9_turned()`

Draws an art as nine pieces: corners `corner` pixels on screen from the outer `frac` of the texture, edges stretched,
the middle too when `fill`. `menu_art_draw9_turned()` is the same with the texture turned half a turn (`turned` 1): a
top-left corner piece drawn at the bottom right.

## `runtime/ui/menu_art.cpp`

The art comes from the game's own interface textures, which `runtime/ui/menu_assets.c` reads from the user's RomFS:
the frame's corner ornaments, the control guide's bar, and the layers of the buttons, cursor, panels and divider that
`runtime/ui/menu_skin.cpp` puts together. A worker decodes them into RGBA once at start (the archive unpacks to about
240 MB and takes a moment); the window thread uploads each to the GPU with its smaller levels the first time the menu
asks for it, and registers it with Dear ImGui's Vulkan back end. Without the files the menu draws its plain shapes.

- `kNames[]` is in the order of `MenuArt`; `tools/menu_preview.py` reads this list too.
- `MAX_LEVELS`, 6: the levels uploaded for each art. The buttons draw it at about half size and smaller, where a line
  one or two texels wide would break up if sampled from the full image alone.
- `g_state`: 0 not started, 1 decoding, 2 decoded, 3 nothing to show.

### `art_free()`

Frees what `upload_rgba()` made for an art. None of it is in use: the caller waited for the queue or never drew with
it.

### `upload_rgba()`

The pixels (w x h) as a sampled image of up to `max_levels` levels, each halved from the one before, with its view and
Dear ImGui descriptor set in the art; 1 when all of it is there, else 0 with nothing left allocated.

- The levels are built one after the other in a plain allocation and then copied to the staging buffer, because the
  staging memory may be slow to read back.
- The staging buffer is flushed here (`vmaFlushAllocation`): the upload batch flushes its own staging buffers, and
  this one is caller-owned and can be noncoherent.
- When the count of `menu_vk_errors()` moved across the descriptor set's registration, the set was not allocated
  (`menu_vk_check()` logged it) and the art's set is cleared.

### `upload()`

After the upload the decoded pixels stay (a few MB): the back end may start again.

### `menu_font_upload()`

The menu's font atlas (`runtime/ui/menu.cpp` `fonts_up()`), uploaded as the art is: through the runtime's upload
batch, which submits under the queue lock, into memory VMA picks (device-local, else what is left), every step
checked. Dear ImGui's own upload (`ImGui_ImplVulkan_CreateFontsTexture`) checked no result, so with the video memory
full its copy went to an image without memory and the GPU wrote to address 0 (issue #165); and run from
`ImGui_ImplVulkan_NewFrame`, its submit and queue wait took no lock and could meet the game's submissions on the same
queue. The previous atlas goes once the new one is on the GPU and the queue has finished the frames that drew with it.

### `menu_art_release()`

Each art's `failed` is cleared, so it is uploaded again at the next use. The font atlas is freed too: the menu makes
its atlas again after the back end starts again (`fonts_up()`).

## `runtime/ui/menu_assets.h`

The game's own interface fonts and textures for the settings menu, read from the user's RomFS
(`runtime/ui/menu_assets.c`).

### `menu_asset_load()`

A RomFS file, a `.zs` one decompressed with the game's dictionaries; malloc'd, NULL when absent or unreadable.

### `menu_asset_font()`

A `.bfotf` / `.bfttf` font as plain OpenType / TrueType bytes; malloc'd, NULL when absent or not a font.

### `menu_asset_texture()`

Texture `name` of a BNTX archive as RGBA8 (BC4 masks: white, the mask in alpha); 1 with a malloc'd `*rgba`.

## `runtime/ui/menu_assets.c`

The settings menu's look from the game's own files in the user's RomFS, read at the first use: its interface fonts
(`Font2/Scalable`) and the shared interface textures (`UI/Layout2Archive/Common...bntx`). Nothing is shipped; a RomFS
without them leaves the menu as it was.

- `.zs` files are zstd frames, most with one of the dictionaries of `Pack/ZsDic.pack.zs` (a SARC compressed without
  one; the frame names its dictionary by id).
- A `.bfotf` / `.bfttf` font is an OpenType / TrueType file behind an 8-byte header, every big-endian 32-bit word XORed
  with one key: the first word is `0x7f9a0218` under it, the second the payload size.
- BNTX: the platform block at 0x20 (`NX  `, or `Ounc` on Switch 2) counts the textures (+4) and points at their BRTI
  records (+8). A BRTI holds the format (+0x1c), width and height (+0x24, +0x28), the block height of the block-linear
  layout as log2 of GOBs (+0x34), the level size (+0x50), the name (+0x60, a u16-length string) and the level offsets
  (+0x70). The levels are block linear in the Switch 2 GOB layout (`runtime/gfx/tex_deswizzle.c` `tex_bl_copy()`). BC4
  masks become white with the mask as alpha (the menu tints them); ASTC 4x4 goes through the runtime's decoder.

### `dicts_load()`

The dictionaries are read once, by whichever thread asks first (the "menu-art" thread and the present path both do in
the first frames); the others wait for it (`PlatOnce`). Checking `ndicts < 0` instead let a second caller see the 0
set at the start of the read, take it for "no dictionaries" and fail its asset: the menu then kept its fallback fonts
for the whole run.

### `menu_asset_load()`

A frame that does not say its size (`ZSTD_CONTENTSIZE_UNKNOWN`) is streamed into a growing buffer.

### `menu_asset_font()`

The decoded payload must start as OpenType with CFF (`OTTO`) or with TrueType outlines (`0x00010000`).

### `menu_asset_texture()`

The platform block at 0x20 is `NX  ` on Switch and `Ounc` in Switch 2 archives; its layout is the same, so the tag is
not tested. The format kinds read: `0x1d` BC4 (8 bytes a block), `0x2d` ASTC 4x4 (16 bytes a block), `0x0b` R8G8B8A8
(4 bytes, 1 x 1 blocks). The block-linear source is padded out to whole GOB columns and block-height units, because
`tex_bl_copy()` reads them.

## `runtime/ui/menu_sdl.cpp`

The SDL3 adapter of the in-game menu: the window's events to menu events (`runtime/ui/menu.h`), as
`runtime/ui/menu_win.cpp` does for Win32. Window thread (the event loop of `runtime/gfx/gfx_window_sdl.c`, after
`input_sdl_event`).

F1 opens and closes the menu, Esc opens it (with the menu open, Esc goes to the menu, which closes). Esc also releases
the cursor (`runtime/input/input_sdl.c` sees the event first; closing the menu gives the capture back). While the menu
is open, its keys, the mouse and focus changes go to it; the game reads the keyboard through `input_sdl.c` and gets
nothing while the menu is open (`input_menu`). Mouse positions come in window units and go to the menu in pixels.

### `key_of()`

Delete clears a cell of the remapping rows; Q is the previous tab and E the next.

### `menu_sdl_event()`

- Esc with the menu open (opened by F1 too) is not a toggle here: it goes to the menu, which closes itself.
- F9 is the debug menu, while its setting is on (`menu_debug_enabled()`).
- Ctrl+F5 is taken here; F5 alone stays a bindable key.
- A held Esc's repeats are dropped: the press that opened the menu is not queued, so its first repeat would close it.
  After that test the key event falls through to the key-up case (`[[fallthrough]]`).
- `SDL_EVENT_TEXT_INPUT` carries UTF-8, and comes between `SDL_StartTextInput` and `SDL_StopTextInput`
  (`gfx_window_text_input`).

## `runtime/ui/menu_win.cpp`

The Win32 adapter of the in-game menu: window messages to menu events (`runtime/ui/menu.h`). It is the only Win32 code
of the menu; the SDL3 window has its own (`runtime/ui/menu_sdl.cpp`). Window thread. Only the Windows build compiles
this file.

F1 opens and closes the menu, Esc opens it (with the menu open, Esc goes to the menu, which closes). Esc also releases
the cursor (`input_window_msg` runs first; closing the menu gives the capture back). While the menu is open, its keys,
the mouse and focus changes go to it; the game polls the keyboard on its own (`runtime/input/input_win.c`) and gets
nothing while the menu is open (`input_menu`).

`g_held`: the mouse buttons pressed on the open menu and not yet released, one bit each. While any is held the window
has the mouse capture, so the release reaches the menu where the pointer went outside the window (else ImGui would
hold the button down and miss the next click). The game itself only clips the cursor (`input_win.c`); it takes no
capture.

### `key_of()`

Delete clears a cell of the remapping rows; Q is the previous tab and E the next.

### `menu_window_msg()`

- Bit 30 of a key message's `lParam` is auto-repeat. F1, and Esc with the menu closed, toggle only on a press that is
  not a repeat. Esc with the menu open (opened by F1 too) is not a toggle here: it goes to the menu, which closes
  itself.
- F9 is the debug menu, while its setting is on.
- Taking F10 here also keeps F10 from opening the window menu.
- Ctrl+F5 is taken here; F5 alone stays a bindable key.
- A held Esc's repeats are dropped: the press that opened the menu is not queued, so its first repeat would close it.
- A key the menu has no use for returns 0, so F11 and the system keys keep working.
- `WM_CHAR` carries UTF-16 units (`TranslateMessage`); a character outside the BMP comes as a surrogate pair.
- A button's release clears its bit of `g_held` also when the menu closed meanwhile: the capture goes either way.
- `WM_CAPTURECHANGED`: another window took the capture (the `ReleaseCapture` of the button release finds `g_held`
  empty).

## `runtime/ui/menu_text_input.cpp`

The window's text input follows the menu's text fields (`runtime/ui/menu.h` `menu_text_input_follow()`). It is a file
of its own so that a page test can run the same switch the menu does without the menu's Vulkan side. `g_text_on` is
what the window was last told: off at start, as the window makes it.

## `runtime/ui/menu_sound.h`

The settings menu's sounds, from the game's own interface sounds (`runtime/ui/menu_sound.c`).

### `menu_sound_play()`

Menu thread.

### `menu_sound_preload()`

Decodes the sounds now; the first play does it otherwise.

### `menu_sound_mix()`

Audio thread: adds the playing sounds to `frames` of interleaved float output (channels 0 and 1 get them).

## `runtime/ui/menu_sound.c`

The game's own interface sounds (`Sound/Resource/Stream/SE_Sys_*.bwav` in the user's RomFS: cursor, decide, cancel,
the Plus menu's open and close, the tab moves), decoded once and mixed over the game's output in the audio device
callback (`runtime/audio/audio_mixer.c` `sdl_pull()`). Nothing is shipped: a RomFS without them stays silent.

BWAV: `BWAV`, byte order mark, version, CRC32, a prefetch flag (u16) and the channel count (u16), then 0x4C bytes per
channel: codec (u16, 0 PCM16, 1 DSP-ADPCM), pan (u16, 0 left, 1 right, 2 middle), sample rate, sample count (u32), the
count again, 16 DSP coefficients (s16), the absolute offset of the samples (u32), and more. The interface sounds are
48 kHz DSP-ADPCM: 8-byte frames of a header (coefficient pair index, scale exponent) and 14 four-bit samples.

Threads. Menu thread: `menu_sound_play()` starts a voice (a free one, else the oldest). Audio thread:
`menu_sound_mix()` adds the voices to a block. A voice is free while its position is negative (`pos`, a frame; -1
free); the menu thread sets its sound first and its position last (release), the audio thread reads them in the other
order (acquire). `loaded`: 0 not yet, 1 loading or done (`sounds[]` is written before the release store of 2), 2
ready. A decoded sound's `pcm` is interleaved stereo at 48 kHz.

`NX2SARIA_MENU_SOUND_VOLUME` (`menu_sound_volume`, 0 to 100, default 80; 0 is silent) is the level, under the output
volume.

`CURSOR_GAP_NS`, 180,000,000 ns: the least time between a cursor tick and the sound before it. Moving down a list or
changing a value asks for a tick at every step, about 20 a second while a direction is held, and the focus moving with
a press or a tab change is part of that sound: at most five ticks a second are heard. `CURSOR_GAIN`, 0.6: the tick
plays under the sounds of a decision.

### `decode_channel()`

One channel into `dst[2 * i + ch]` (`ch` 0 or 1; a mono pan writes both). 0 when the data does not fit the file.

### `load_one()`

A sound is taken only at 48,000 Hz and up to 10 seconds: the interface sounds are 48 kHz and short. A middle-panned
mono sound is in both channels, so it is scaled by 0.7071.

### `volume()`

The menu's level times the output volume (`NX2SARIA_VOLUME`, as the game's sound has it).

### `menu_sound_play()`

A sound that replaces itself restarts instead of stacking up, and a decision replaces the tick of the same press (a
row changes its value in the frame the press is seen): a voice playing the same sound or the cursor tick, whose
position is under 2,400 frames (started under 50 ms ago), is the one taken.

### `menu_sound_mix()`

`menu_sound_play()` may restart the voice between the two loads: `pos` would then belong to the old sound, past the
end of a shorter new one; such a voice is mixed from the next call on. The position advances by compare-and-exchange,
so only if the menu thread did not restart the voice meanwhile.

## `runtime/ui/menu_cursor.c`

The mouse pointer in the game's own menus in game version 1.4.3 (the title, the inventory, the System tab, Options,
shops, dialogue choices). While a menu screen has the game's focus box (eui's `BoxCursor`, the frame the d-pad moves),
the Windows pointer is released from the mouse camera and shown, the item under it takes the focus as if the d-pad had
moved there (highlight, sound and description included), a left click on an item is A on it and a right click is B.
Nothing is drawn and no game code is called: at each pad read (`runtime/hle/ov_input.c`, `GetNpadStates`, the game
thread) the open screens are read and the focus is moved with the three writes `eui::BoxCursorMgr::moveBoxCursor`
makes, so the game's own `BoxCursorControl` moves the box at its next update. The 1.1.1 counterpart is
`runtime/ui/menu_mouse.c`.

The focus read, the hit test, the stale-hit-pane fallback and the focus writes were written for 1.0.0. Placed in 1.4.3
by hand:

- `eui::ScreenMgr::sInstance`: 1.0.0 `main+0x4640010`, 1.4.3 `main+0x3acf960` (`SCREEN_MGR_V143`). Its
  `createInstance` at `main+0xc613d4` and constructor at `main+0xc61450` match 1.0.0's `main+0x119c5cc` / `+0x119c648`
  instruction for instruction, the object 0x450 bytes with the same field stores, so its layout is 1.0.0's.
- The screen's reserved node (+0x150) and request bits (+0x175, bit 2) and its draw target byte (+0x16c): the same
  stores at 1.4.3 `main+0x87903c`.

The other fields are 1.0.0's. The file logs what it reads the first time a menu opens (`NX2SARIA_MENU_CURSOR_LOG=1`:
every screen change), so a field that moved shows as a menu with no nodes or boxes off the layout.

It acts only while keyboard and mouse are the device used last (`input_kbm_last`): a gamepad player sees the menus
unchanged. `NX2SARIA_MENU_CURSOR` (live, default 1): 0 leaves the menus to the keys and the pad.

The offsets, by the game's type:

| Constant | Value | Of |
|---|---|---|
| `SCREEN_MGR_V143` | `0x3acf960` | `eui::ScreenMgr::sInstance` |
| `MAIN_SIZE` | `0x3adf000` | main's image (code, data, bss) in 1.4.3 |
| `SM_BOX_MGR`, `BM_CONTROLS`, `CTL_ACTIVE`, `CTL_RESERVED` | 0x3f8, 0x28, 0x58, 0x60 | `eui::ScreenMgr`, `BoxCursorMgr`, `BoxCursorControl` |
| `SCR_LAYOUT`, `SCR_DRAW_INFO`, `SCR_NODES`, `SCR_RESERVED`, `SCR_STATE`, `SCR_REQUEST`; `SCR_OPENED` | 0x28, 0x60, 0x80, 0x150, 0x16e, 0x175; 2 | `eui::Screen` |
| `NODE_BUTTON`, `NODE_SCREEN`, `BTN_LAYOUT`, `BTN_FLAGS`, `BTN_HIT_PANE` | 0x18, 0x20, 0x20, 0x40, 0x58 | `eui::BoxCursorNode`, `AnimButton` |
| `BTN_USABLE` | `0x20010` | both bits set: active and enabled (the test of `ButtonGroup::Update`) |
| `DI_PROJ`, `DI_VIEW` | 0x10, 0x50 | `nn::ui2d::DrawInfo`: projection (4 x 4) and view (3 x 4) |
| `LYT_ROOT`, `LYT_SIZE`, `LYT_NAME` | 0x18, 0x28, 0x30 | `nn::ui2d::Layout`: root pane, size, name |
| `PANE_PARENT`, `PANE_SIZE`, `PANE_FLAG`, `PANE_ALPHA`, `PANE_BASE`, `PANE_MTX` | 0x18, 0x50, 0x58, 0x59, 0x5b, 0x70 | `nn::ui2d::Pane` |

`g_pointer`: a menu has the pointer; `input_cursor_allowed` releases the capture then.

### `game_object()`

An object of the game: readable, and its vtable in main's image.

### `pane_shown()`, `pane_box()`, `pane_hit()`

The panes follow the rules of `AnimButton::HitTest` (1.0.0 `main+0x222ea40`, `+0x223b090`). `pane_box()` gives the
pane's box in layout coordinates (origin at the centre, y up) as centre and half size; by the pane's base position the
translation is the left or right edge, or the top or bottom edge, instead of the centre. `Pt` is the pointer: layout
coordinates (`x`, `y`), device coordinates (`nx`, `ny`), and the transform when it is not plain.

### `screen_xf()`

The screen's draw transform (`Xf`), for a screen drawn away from its centred layout space (scale and translation
only).

### `xf_plain()`

True when the transform maps the layout as the plain layout test does (centre to 0,0, corner to 1,1): it adds nothing.

### `focus_now()`

The focused node of an open screen, from the draw targets' box cursors (all 0: none, as in gameplay).

### `nodes_of()`

The screen's focusable nodes: an sead `OffsetList`, head {prev, next} at +0x80, node-to-link offset at +0x94. At most
`MAX_NODES`, 512.

### `stale_hit_panes()`

True when every usable node's shown hit pane lies off the visible layout: the hit panes are stale (the game never uses
touch; the dialogue choices' sit far from where the choices are drawn), so the buttons' own parts panes stand for
them.

### `node_at()`

The usable node under the pointer; 0 for none. With stale hit panes, parts panes can be several items wide, so the
node with the nearest centre wins.

### `move_focus()`

Moves the focus to a node with the writes `moveBoxCursor` makes. The request bit is in a byte eui also writes, so it
is raised again until the focus is there.

### `gameplay_screen()`

Screens with a focus box that are gameplay, not menus: the L ability wheel, the D-pad strips, Autobuild's list, the
Purah Pad's camera and the map (the stick moves its own cursor there).

### `set_pointer()`

The pointer appears in the middle of the picture (`ikb_pointer_set(0.5, 0.5)`), not where the capture left it.

### `picture_pos()`

The pointer on the game's picture, 0..1 from its top left (the present's fit, letterbox or pillarbox).

### `menu_cursor_poll()`

Once per pad read on the game thread: the pointer's hover and clicks, as pad buttons (A, B) for this read.

- With the gamepad as the last device the menus stay as they are on the console: no pointer, the capture kept. Moving
  the mouse (or a key) makes keyboard and mouse the last device and the pointer comes.
- A click's button is held for 60 ms, so the game sees one press whatever its read rate.
- A click on an item that does not have the focus asks for the focus first (`pending_a`); A is pressed once the focus
  is there.

## `runtime/ui/menu_mouse.c`

The mouse in the game's own menus in game version 1.1.1 (the title and its save files, the pause menu and its pages,
Options, the save dialogs). While a menu screen has the game's focus and keyboard and mouse are the device used last,
the pointer is released from the mouse camera and shown; the button under it takes the focus as if the d-pad had moved
there (the game's highlight, slide and sound), a left click on a button is A on it, a right click is B, and the wheel
is the d-pad's up and down (a list scrolls a row a notch). How the game's objects were found, the hit test, the pause
menu's tabs, ultrawide pictures and the log: [menu-mouse.md](../menu-mouse.md).

No game code is called. Each interface screen's update calls `menu_mouse_screen_host()` (the site of the set
`menu_mouse`, `recomp/patches-v111.txt`, `main+0x2d13b0`), on the thread that runs the game's interface, between the
focus manager's read of the pad and its controls' update. For the screen that has the focus there, the screen's
buttons are read and a focus move is written as the game's focus setter (`main+0x86ffc0` and its manager's method
`main+0x3774f70`) writes it: the control's reserved entry, the screen's requested entry and its request bit. The
controls' update (`main+0x768990`) then moves the focus itself. A and B are pad presses the pad read adds
(`menu_mouse_poll()`, from `runtime/input/input.c` `input_poll()`), held 60 ms so the game sees one press whatever its
frame rate.

The game's objects (main offsets):

- The interface system is `*(main+0x4e982e0)` (`UI_SYSTEM`; its vtable `main+0x4c73ce0`, `UI_SYSTEM_VT`). Its screen
  manager is at +0x38, whose focus manager is at +0x60: +0x22 the players whose input it takes (bit 0 player 1), +0x28
  player 1's control.
- A control: +0x78 the focused entry, +0x80 the reserved one, +0xb4 bit 3 the reserved move's "no slide" flag.
- An entry: +0x18 its button, +0x20 its screen. A screen's entries are a list: head at +0x88, first node +0x90, the
  entry at the node less the int32 at +0x9c, the next node at the node's +8.
- A screen: +0x1a6 its state (2 open); +0x1a5, +0x1aa, +0x1ab the entry test's other bytes (`main+0x769d70`); +0x38 its
  controller (+0x40 bit 1: takes input); +0x188 the requested entry; +0x1ae bit 2 the request; +0x1c0 its 2D projection
  (4 x 4); +0x28 its layout (the name).
- A button: +0x40 bit 4 enabled, +0x58 its root pane (shown), +0x60 the pane its focus frame takes (the box).
- A pane (0xb0 bytes, in its layout's array at +0x100): +0x18 its layout; +0x28 its global matrix (3 x 4 by column:
  +0x28 m00, +0x38 m11, +0x4c and +0x50 the translation); +0x58 its size; +0x74 its alpha (0..1); +0x80 the extra size;
  +0xa8 its flags (bit 0 shown, bit 3 alpha inherited, bit 10 an extra height or with bit 9 width, bits 11..17 the base
  position).

`MAIN_SIZE`, `0x4ea0000`: main's image, up to the patch page.

`NX2SARIA_MENU_CURSOR` (live, default 1): 0 leaves the menus to the keys and the pad. `NX2SARIA_MENU_CURSOR_LOG=1`
logs every screen the focus goes to. `NX2SARIA_MENU_MOUSE_PATCH=0` removes the site.

State shared between the interface thread and the pad read:

- `pending[]`: the pad presses the pad read adds. They are counts the interface thread raises and the pad read spends;
  `press_bit[]` gives each kind's button in the order of `MM_A` to `MM_R` (`runtime/ui/menu_mouse_geom.h`).
- `g_walk`: a walk to a clicked pause-menu tab is under way (the interface thread sets it). The pad read sets
  `g_walk_player` when the player's own pad or keys press L, R, the d-pad or the left stick meanwhile, and the walk
  stops.
- `g_pointer`: a menu has the pointer; `input_cursor_allowed` releases the capture then.
- `g_active_ms`: the last interface update that found a menu for the mouse, or a focus on a screen that opens or
  closes (between two menus). `g_menu_ms` (interface thread only): the last update that found a menu for the mouse.
  `g_hidden_ms`: when the pad read last hid the pointer.
- `IDLE_MS`, 350: no menu for that long, and the pointer goes back to the camera.
- `g_why`: `focus_screen()`'s reason for returning 0.
- `NEAR_UNITS`, 80.0: on the pause menu, a pointer this near a box (in layout units; 40 pixels at 1080) is on it.
- `dump_boxes`: the next `entries_of()` logs every box (a menu screen's first, with `NX2SARIA_MENU_CURSOR_LOG=1`).

The log has one line per menu the mouse was used in, written when the focus leaves it (`Stats`, `stats_flush()`), at
most `LOG_MENUS` (48) a session.

### `game_object()`

An object of the game: readable, aligned to 8, and its vtable in main's image.

### `pane_alpha()`

Not transparent when the alpha times 255 is at least 1 (and below 1e6): `(int)a != 0`, as the game truncates. A NaN is
transparent.

### `pane_parent()`

The pane's parent: in its layout's tree, or for the layout's first pane the pane that holds the layout (a part).

### `pane_shown()`

Shown as the entry test takes it (`main+0x769d70`): the pane shown and not transparent, every ancestor shown, and one
whose alpha its children inherit not transparent.

### `why_none()`

Why no screen is for the mouse: one line each time the reason or the objects change (a field that moved in another
game build shows here), at most 12 a session, 60 with `NX2SARIA_MENU_CURSOR_LOG=1`.

### `screen_open_why()`

The screen part of the game's entry test (`main+0x769d70`): open, shown, not blocked, its controller taking input
(+0x40 bit 1). 0 with the reason in `g_why`. The manager argument is unused: the entry test's level floors (the
screen's +0x170 against the manager's +0xdc and +0xe0) are not taken. Read as u32 they refused the save files' list
while it had the focus; the focused entry is the manager's choice.

### `focus_screen()`

The screen of the focused entry; 0 for none (gameplay).

### `tab_log()`

A tab click of the pause menu and its outcome; the first 40 lines a session.

### `pause_page_of()`

The pause menu's page the focused entry is on (0 Collection to 3 System, `runtime/ui/menu_mouse_geom.h`
`mm_pause_page()`), -1 when none is found: the layout that holds the entry's button, or the layout holding that one (a
slot is a part inside its page), up to the page's own layout, whose name is the page's (`PaItemMenu_00` and the
others). The step up reads `LYT_HOST` (+0x108): the pane that holds this layout, in the layout above.

### `screen_units()`

The layout units from the picture's centre to its edge, from the screen's projection (squeezed on ultrawide).

### `entries_of()`

The screen's entries that can take the focus, with their boxes; at most `MAX_ENTRIES`, 256. With `dump_boxes` it logs
each box of a menu as it opens.

### `reserve_focus()`

Moves the focus to an entry of a screen: the writes of the game's focus setter with its flag 0 (a slide, as the d-pad
moves). The method's busy guard (its +0x1f bit 5 with the focused button's +0x40 bit 5) is not taken: it held on the
save files' list the whole time the list waited for input, and the d-pad's moves there do not go through it.

### `drop_presses()`

Everything queued goes when its menu does: in play B is the sword and the d-pad the ocarina, Navi and the boots.

### `verbose_log()`

With `NX2SARIA_MENU_CURSOR_LOG=1`: one more of at most `n` lines of what the pointer does.

### `picture_pos()`

The pointer on the picture, 0..1 from its top left (the present's fit, letterbox or pillarbox). It returns 2 when the
position is the input script's.

### `no_menu_input()`

No menu takes the mouse now: wheel notches and button presses so far are taken as seen, not kept for the next menu.

### `menu_mouse_screen_host()`

Runs at every interface screen's update, before the focus manager's controls take a reserved focus. `x19` (and `x0`)
is the screen. It changes no register.

Its state: `last_screen` is the screen the mouse was for at the last look. `resync`: the pointer was just warped, so
its next position read is the start and no move. For the pause menu (`PlusMenu_00`): its page now (`page`), and a tab
clicked: the page to reach (`tab_goal`), when it was asked (`tab_ms`), when the page last changed (`page_ms`) and when
the last L or R went (`step_ms`). `clicks` holds the button presses already taken.

In order:

- When the pad read took the pointer away, `shown` is cleared so the pointer comes anew.
- While the settings overlay or a text entry has the input and the mouse (`input_game_input_blocked()`), the mouse is
  theirs. When the feature is off, notches and clicks are taken as seen: none is kept for later.
- When no menu has the focus, the last menu's log line is written. A focus still on a screen that opens or closes
  keeps the pointer for up to 1.5 s: one menu giving way to the next.
- An update of a screen other than the focused one returns: the focused one's update comes in this frame too.
- When the focus went to another screen, the last one's line is written and this one's count starts. With
  `NX2SARIA_MENU_CURSOR_LOG=1`, each box of the first 6 menu screens opened is logged.
- A menu for the mouse is a screen where some entry takes the focus, the focused one among them. A screen of the play,
  whose focus sits on nothing shown, never takes the pointer from the camera. The boxes slide with a page, so the
  entries are read each frame.
- With the gamepad used last, the menus are as on the console and the capture is kept.
- When the pointer comes, it is put on the focused button, so nothing moves until the mouse does. Not when it was
  hidden only a moment ago (under 1.5 s, between two menus): then it stays where the player left it. The warp may not
  happen (Wayland), so `resync` is set and where the pointer is gets read before any move counts. The presses made
  earlier are synced away (`mm_clicks_sync()`): they are no clicks.
- Clicks are presses counted at the window's event, so one shorter than a frame is not lost at a low frame rate;
  several between two looks are one. The click that brought the window back to the front is not a menu click.
- A walk to a clicked tab stops for any other wish of the player: a right click, the wheel.
- On the pause menu, the page now is read, and a clicked tab's page is reached one L or R at a time, each once the
  last has landed. When the page went the other way than the walk (the player's own L or R, keys or pad), the walk
  stops.
- Over the pause menu's tab row a wheel notch turns the page: up the one before (L), down the next. Elsewhere a notch
  is the d-pad's up or down: a list moves a row, as the pad moves it.
- A click on another button: A is pressed once the focus is there, and the reservation is renewed until then.
- The first position read after a warp is the start.
- The nearest box (`mm_pick_near()`) is taken only on the pause menu, whose pages have the narrow slots: on a prompt's
  Yes and No a click just outside a button stays outside it.
- A left click on the pause menu's tab row: L or R for a sign, else steps to the page clicked. When that page is the
  one shown there is nothing to walk to, and a walk under way stops.
- A hover asks for the focus once; it asks again after 150 ms if the game has not taken it (a reservation it refused).

### `menu_mouse_poll()`

Once per pad read: the presses the interface thread asked for, one at a time, each held 60 ms and then let go for
60 ms. `hold_bit`, `hold_until` and `gap_until` are the press being given: its bit and the times it is held to and let
go until. The pad is read on more than one thread (a cheat's frame reads it on the render thread): one at a time takes
presses (`busy`), and the others add the one held, if any.

When there has been no menu for the mouse for `IDLE_MS` (gameplay, a movie, the overlay), the pointer goes and the
camera takes the mouse.


### `tab_at()`

The tab row under the pointer: the game's four places (`runtime/ui/menu_mouse_geom.h` `mm_tab_at()`), or the five of
the pause menu's settings tab while it has moved the header (`runtime/ui/pause_tab.c` `pause_tab_five()`,
`runtime/ui/pause_tab_core.h` `pt_five_tab_at()`). A click on the settings tab walks to the System tab and asks
`pause_tab_click_wanted()`.
## `runtime/ui/menu_mouse_geom.h`

The arithmetic and the bookkeeping of the mouse in the 1.1.1 menus (`runtime/ui/menu_mouse.c`), kept free of the
game's memory and of the window so `tests/menu_mouse_check.c` can run them: a button's box from its pane's fields, the
pointer in the menus' layout units, the window position of a point of the layout, the box a point falls in, which
button presses are new clicks, and the pad presses queued for the game.

The layout's units: 3840 x 2160, the origin at the screen's centre, y up. The UI's projection maps them to the
picture. On a picture wider than 16:9 the runtime squeezes its first row (`runtime/patches/widescreen.c`, the set
`wide_hud`) by 16:9 over the picture's ratio, so the layout is drawn in the picture's middle 16:9 and the units reach
±1920 / squeeze across it. A screen the runtime keeps unsqueezed (the menus' backgrounds and the fades) has squeeze 1.
Panes moved or widened by `NX2SARIA_WIDE_HUD` / `WIDE_MENUS` (`runtime/patches/wide_hud.c`) carry their new place in
their matrices, so the box needs no change.

Clicks (`MmClicks`, `mm_clicks_sync()`, `mm_clicks_take()`): the window counts each button's presses as they come, so
a click shorter than a frame is not lost; a click is a count that moved since the last look. Wherever no menu takes
the mouse the counts are taken as seen, so a click given between two menus (on a fade, on a screen that does not take
the focus) is not a click of the next one.

The pad presses queued for the game (`mm_queue()`, `mm_queue_drop()`, `mm_queue_take()`): the interface thread adds,
the pad read takes, one at a time. A and B wait one at most: several quick clicks are one, so a B that closes a menu
has no second B behind it to reach the play (where B is the sword). The wheel's up and down wait three at most, and a
turn the other way drops the rest. L and R (the pause menu's pages) wait one at most, like A and B. Everything queued
is dropped when its menu goes.

The pause menu's tab row (`PlusMenu_00`): the pages' names Collection, Equipment, Items and System (Menu) and the L
and R signs across its top are no buttons of the screen's list, so the hit test finds none there. The game turns its
pages with L and R, one page a press. Their places in layout units (origin at the centre, y up), measured from the
menu as drawn: the names centred at x -693, -232, 229 and 688, the signs at -989 and 989, the row from y 880 to 1080.
The wide menus' edits do not move them. The boxes meet halfway between the centres. `MM_TAB_L` is 4 and `MM_TAB_R` 5.

### `mm_pane_box()`

A pane's box in layout units from the fields `menu_mouse.c` reads (as the game's focus frame takes it,
`main+0x769650`): its size w, h, the scales m00 and m11 and the translation tx, ty of its global matrix, its flags
word (+0xa8: bits 11..17 the base position, bit 10 an extra height of `extra` or, with bit 9, an extra width:
`main+0x769930`). Returns 0 for a box with no area or a value that is not a number.

### `mm_units()`

The layout units from the picture's centre to its edge, across (`*ux`) and down (`*uy`), from the first two diagonal
entries of a screen's 2D projection (the canvas's 4 x 4 at the screen's +0x1c0): 2 / 3840 times the squeeze across and
2 / 2160 down for the game's 3840 x 2160 layouts, so 1920 / squeeze and 1080; another layout size reads as its own.
Returns 0 (and the 16:9 3840 x 2160 values) when the matrix is no such projection.

### `mm_to_layout()`, `mm_to_picture()`

The pointer at (px, py), fractions of the picture from its top left, in layout units; and the inverse, a point of the
layout as fractions of the picture.

### `mm_pick()`

The box (x, y) is in; the smallest when boxes overlap (a slot inside a panel). -1 for none.

### `mm_pick_near()`

When the pointer is in no box: the box nearest to it, if its edge is within `margin` units, else -1. Some of the
game's slots take a narrow box on one side of their picture (the pause menu's bombchu slot: 100 units wide at the
right of a 260-wide slot), and rows leave gaps between their slots, so a click on the middle of a picture or just
beside it would find no button.

### `mm_moved()`

The pointer has moved since the last look by more than a pixel of a 4K picture (0.00025 across, 0.0005 down).

### `mm_queue_take()`

The next press to give the game (A, B, up, down, L, R in that order), -1 for none; safe against a drop on another
thread.

### `mm_tab_at()`

The tab under (x, y): the page 0..3, `MM_TAB_L` or `MM_TAB_R` for a sign, -1 off the row.

### `mm_pause_page()`

The page a pause menu page's layout name stands for (0 Collection `PaCollectMenu_00`, 1 Equipment
`PaEquipmentMenu_00`, 2 Items `PaItemMenu_00`, 3 System `PaSystemMenu_00`), -1 for another.

### `mm_tab_away()`

The page went from `was` to `now` the other way than a walk to `goal` goes: the player's own L or R, which stops the
walk.

### `mm_tab_step()`

The press that brings page `cur` one nearer to page `goal`: `MM_L`, `MM_R`, or -1 when it is there (or either is
unknown). The pages do not wrap past the ends.

## `runtime/ui/quit_menu.c`

Quit Game in the game's own menus in game version 1.1.1: a fourth entry on the title screen and a third button in the
confirmation the pause menu's Title Screen opens. What the game has, why the entry sits where it does, the texts and
the log: [quit-menu.md](../quit-menu.md).

The runtime calls no game code. The set `quit_menu` (`recomp/patches-v111.txt`) has seven sites; three call host
functions, four make the game call its own text helper once more in each screen. The readers of the game's objects
(`ok_at()`, `game_object()`, `pane_parent()`, the offsets) are the ones of `runtime/ui/menu_mouse.c`, whose notes
name each field; they are repeated here so that neither file needs the other.

The title screen's own fields: +0x2c8 the number of its buttons, +0x2d0 their array, +0x2e8 its menu shown
(`TITLE_COUNT`, `TITLE_BUTTONS`, `TITLE_MENU`). The entries of a screen are in the order its buttons were made, so the
title's fourth entry (`TITLE_SPARE`, 3) is `Pa_Button_03` and the window's third (`WINDOW_SPARE`, 2) `Pa_Button_02`.
From a button's root pane the part pane that holds it in the screen's layout is two panes up (`TITLE_PART_DEPTH`, 3,
is the most that is looked at).

State shared between the interface thread and the pad read: `g_title_ms` and `g_pause_ms`, the time the focus was last
seen on the new button of each screen (0 when it is not there); the pad read takes them as true for `ARMED_MS`, 250,
so a screen that stops updating lets go by itself. `g_window`: the window opened with three buttons. `g_quit`: the
choice was made. `g_texts`, `g_title`, `g_pause`: the labels are served, and each screen is on.

### The sites

Each is in a function that saved its x30 in its prologue (`stp x29, x30`), and is followed by a call that sets x30
again before it is read. None is between a flag-setting word and its reader. `tests/quit_menu_sites_check.py` holds
the words.

**`main+0x2d13b0`, every interface screen's update** (`main+0x2d1380`): the cut and the call of the set `menu_mouse`
(`runtime/ui/menu_mouse.c`), so with either set on the page's +0xee0 runs once, and its host function
(`runtime/patches/patches.c` `screens_host()`) calls both files. The host functions change no register.

**`main+0x3405b04`, the window's open** (`main+0x3405a00`):

```
3405a18: mov x20, x1              the kind
3405a1c: mov x19, x0              the window
...
3405ab0: sub w8, w20, #2
3405ab4: cmp w8, #2
3405ab8: b.lo 0x3405b00           kinds 2 and 3
...
3405b00: ldr x0, [x19, #0x2b0]    the BtnNum animation
3405b04: fmov s0, #2.0            <- bl page+0xe20: s0 = 2.0, or 3.0 for kind 2
3405b08: bl 0x2d2d90              the animation's frame
3405b0c: mov x0, x19
3405b10: mov w1, wzr
3405b14: mov w2, #3
3405b18: b 0x3405b34
```

The host function writes s0 (as the fmov does, the rest of v0 zero) and reads x19 and w20. x0 is loaded before the
cut and is not touched; every other argument register is written after the call at `main+0x3405b08`.

**`main+0x3405b38` and `main+0x3405b3c`, between the open's two calls of the helper**:

```
3405b34: bl 0x3405bc0             the helper for button 0 (x0 the window, w1 0, w2 the label's number)
3405b38: mov w1, #1               <- mov w1, #2; mov w2, #12; mov x0, x19; bl 0x3405bc0
3405b3c: mov w2, #1               <- mov w1, #1; mov w2, #1
3405b40: mov x0, x19
3405b44: bl 0x3405bc0             the helper for button 1
3405b48: ldr w8, [x19, #0x2b8]
```

After the call at `main+0x3405b34` returns, every caller-saved register is dead: the next three words write w1, w2 and
x0, and the first read after the second call is of x19, which the helper keeps (it saves x19 to x21). So a third call
between the two, with its own w1, w2 and x0, changes nothing the open reads, and the second cut writes the w1 the first
took the place of. All the two-button kinds pass here (0 and 4 from `main+0x3405b1c`, 1 from `main+0x3405ba0`, 2 and 3
from `main+0x3405b00`), so button 2 gets the text of the label 1012 in each; it is shown for kind 2 only.

**`main+0x3405bfc`, in the helper** (`main+0x3405bc0`):

```
3405bd8: mov x21, x0              the screen
3405be0: mov w19, w2              the label's number
3405be4: mov w20, w1              the button
3405be8: bl 0xa66f70
3405bec: adrp x8, 0x3df6000       "LayoutMsg/SystemWindow_00"
3405bf0: add x8, x8, #0xbcd
3405bf4: adrp x2, 0x3e1e000       "10%02d"
3405bf8: add x2, x2, #0x4dd
3405bfc: mov w3, w19              <- bl page+0xe10: w3 = w19, or 10 or 11 with three buttons
3405c00: str x8, [x29, #0x18]
3405c04: add x0, sp, #8
3405c08: bl 0x6458a0              the label formatted
...
3405c30: adrp x2, 0x3d26000       "Pa_Button_%02d/T_Btn_00"
3405c3c: mov w3, w20
...
3405c64: ldr x0, [x21, #0x28]     the screen's layout: the only field of the screen the helper reads
3405c68: bl 0xa67d30              the text pane takes the message
```

The host function writes x3 only (the mov's zero extension) and reads x19, x20 and x21. x8 and x2 are live across the
cut: a host call leaves the registers it does not write as they are. The helper names the message file itself, so it
gives a button of any screen a text of `LayoutMsg/SystemWindow_00.msbt`: that is why the title's entry takes its text
from the label 1012 there.

**`main+0x3406964` and `main+0x3406968`, the title screen's set-up** (`main+0x34067d0`, x19 the screen from
`main+0x34067ec` on, not written again before `main+0x3406cdc`):

```
3406950: str x0, [x19, #0x2d8]
3406954: ldr x0, [x19, #0x2c0]
3406958: bl 0x2d0fe0
340695c: ldr x0, [x19, #0x2b8]
3406960: bl 0x2d0fe0
3406964: adrp x2, 0x3e04000       <- mov x0, x19; mov w1, #3; mov w2, #12; bl 0x3405bc0
3406968: add x2, x2, #0x8e        <- adrp x2, 0x3e04000; add x2, x2, #0x8e
340696c: mov x0, sp
3406970: mov w3, #1
3406974: strb wzr, [x19, #0x2e8]
3406978: mov w4, wzr
340697c: bl 0x6f99d0
```

The cut follows a call, so every caller-saved register is dead there, and the words after it write x2, x0, w3 and w4
before the next call reads them. The buttons are made by then (the loop at `main+0x34068a4`). The adrp moves one word
down inside its page (0x3406964 and 0x3406968 are both in 0x3406000), so its word is the same. x20 to x23, which the
set-up uses later, are callee-saved and the helper keeps them. The game does not call the window's helper for the
title; it works there because the helper reads only the screen's layout (+0x28, a field of the class every screen
derives from) and looks the pane up by its path, `Pa_Button_03/T_Btn_00`, which the title's layout has.

### `test_flag()`

A word of `NX2SARIA_QUIT_MENU_TEST` ([quit-menu.md](../quit-menu.md), "Testing").

### `quit_menu_wanted()`

The set `quit_menu` is in this build and on, on game 1.1.1.

### `sites_ok()`

The words of the game's image at a screen's sites are the ones the sites were written for (`title_sites[]`,
`pause_sites[]`: the sites that screen needs, the screens' update and the helper's among them). The generator refuses
a build whose module differs at a site, so this is the same check at run time; a difference turns that screen off with
the address and the word in the start line.

### `quit_menu_texts()`

Called once by `runtime/hle/romfs.c` `quit_menu_overlay()` with the number of message archives edited: the start line,
and what is on. Nothing is on unless every archive wanted got its labels.

### `focused_entry()`, `entries_of()`

The entry that has the game's focus (0 for none), and a screen's entries in order.

### `hidden_at()`

How far up from a pane the first pane is that hides it (0 the pane itself, -2 none: it is shown), as the game's entry
test takes it: a pane not shown, or transparent where the alpha is inherited.

### `describe()`, `trace()`

The line of `NX2SARIA_QUIT_MENU_TEST=log`: each entry's button flags, its place, whether it has the focus and what
hides it.

### `title_tick()`

At the title screen's update. A title with other than four entries and four buttons is left alone, with one line.
The fourth button's part pane gets its shown bit when it is the pane that hides the button and at most
`TITLE_PART_DEPTH` up; an ancestor further up (the menu not faded in yet) is the game's. The bit is written once: the
game does not clear it. `g_title_ms` is the time while the screen is open and the focus is on the fourth entry.

### `window_tick()`

At the update of the window that was opened with three buttons: `g_pause_ms` while it is open with the focus on the
third entry. When it leaves the open state without the choice, one line says so and the window is forgotten.

### `quit_menu_window_count_host()`, `quit_menu_window_label_host()`

The host functions of the sites at `main+0x3405b04` and `main+0x3405bfc`. The label 12 becomes 1 (the game's Cancel)
when the labels were not served, so the helper never asks for a label the message file lacks; the button it is for
is hidden then.

### `quit_menu_poll()`

Once per pad read, after the mouse in the menus has added its presses (`runtime/input/input.c` `input_poll()`), so a
click on the entry is taken too. While the focus is on a new button, A does not reach the game and a new press of it
is the choice (`runtime/ui/quit_menu_core.h` `qm_press()`). The window is asked to close once no save is being written
(`runtime/hle/fs_host.c` `fs_save_busy()`), as the settings menu's Quit game waits. The pad is read on more than one
thread: one at a time decides (`busy`), the others drop A while it is held taken.

## `runtime/ui/quit_menu_core.h`

The parts of Quit Game that need neither the game's memory nor the runtime, so `tests/quit_menu_check.c` runs them.

The game's message files (MSBT) as this game writes them: a header of 0x20 bytes (`MsgStdBn`, the byte order mark, the
encoding at 0xc, which is 0, UTF-8; the number of sections at 0xe, two; the file's size at 0x12), then `LBL1` and
`TXT2`, each a tag, its size, 8 bytes of zero and its data, padded to 16 bytes with 0xab. `LBL1`: a count of groups,
then a count and an offset per group, then the labels of each group (a length byte, the name, the index of its text);
a label is in the group its hash names (`qm_label_hash()`: h = h * 0x492 + byte, modulo the groups). `TXT2`: a count,
an offset per text, the texts, each ending in a zero byte. A text's control tags hold zero bytes too, so a text's
length is the next offset less one, never the first zero.

### `qm_msbt_open()`, `qm_msbt_text()`, `qm_msbt_labels()`, `qm_msbt_find()`

A file of that shape, checked whole before anything is read from it: any other (UTF-16, a third section, an offset
outside its section, a text without its zero) is refused. `qm_msbt_find()`: a label's text.

### `qm_msbt_add()`

The file with labels added, written anew with the same number of groups. A label that is there with the same text
counts for nothing, one with another text gets the new one. Returns the number of labels that changed, 0 for none (no
file is made), -1 for a file of another shape.

### `qm_window_label()`

The number of the label a button of the window gets: with three buttons shown, 10 for the first's 3 (Title Screen) and
11 for the second's 1 (Cancel); otherwise the game's.

### `qm_press()`

Whether the pad read takes A away from the game, and whether this is the press that chooses: a press that starts
while the focus is on the entry. An A held since another entry is the game's; an A taken stays taken until it is let
go, also when the focus has gone.

## `runtime/ui/quit_menu_text.h`, `runtime/ui/quit_menu_text.c`

The labels in a message archive, free of the runtime for the test: the header names the message files and labels and
includes nothing of the runtime (`runtime/ui/quit_menu.h`, which the runtime's files include, brings `runtime/cpu.h`
for the host functions, and with it code the test's warnings refuse). `quit_menu_archive()`: a decompressed archive (a
SARC) with the three labels of [quit-menu.md](../quit-menu.md) ("The texts") added to
`LayoutMsg/SystemWindow_00.msbt`, their texts read from the same archive, and the archive laid out again
(`runtime/hle/sarc.c` `sarc_rebuild()`). `quit_menu_build()`: the same on the compressed file, with the dictionary
its zstd frame names. Each returns 1 when made, 0 when the archive has the labels already, -1 with why.

## `runtime/ui/pause_tab.h`, `runtime/ui/pause_tab.c`

The settings tab in the game's pause menu, 1.1.1: what the game has, why the tab is drawn and not laid out, when R is
taken and the log: [pause-settings-tab.md](../pause-settings-tab.md). The menu side is `runtime/ui/menu.cpp`
`layout_pause_tab()`.

The runtime calls no game code and changes no file. The readers of the game's objects (`ok_at()`, `game_object()`,
`pane_parent()`, `pane_shown()`, the offsets) are those of `runtime/ui/menu_mouse.c` and `runtime/ui/quit_menu.c`, whose
notes name each field; they are repeated here so that no file needs another. Its own: the header's +0x328, the number
of categories, and +0x330, their objects (`HDR_COUNT`, `HDR_CATS`), each with its layout's holder at +0x20 (`CAT_LAYOUT`);
a layout's name at +0xf8 (`LYT_NAME`).

`PauseTabView`: the tab's box in the header and the page's box, as fractions of the picture (x0, y0, x1, y1, from the
top left), the name's alpha, and whether each was seen lately (`header` within 100 ms, `page_ok` within `SEEN_MS`,
1500: a shader compile can hold the game's interface for a moment without the pause menu having closed).

State between the game's interface thread, the pad read and the present thread: `g_armed_ms`, the last time the System
tab was ready for R (taken as true for `ARMED_MS`, 250, so a screen that stops updating lets go by itself); `g_req`, a
request to open; `g_open`, the tab is open (the menu tells); `g_view` under `g_lock`. The two hidden panes
(`g_inout_hidden`, `g_bg_hidden`) and the System page's layout (`g_sys_lyt`) are the interface thread's alone: the
bits are written on the thread that draws the panes.

### `pause_tab_test()`

A word of `NX2SARIA_PAUSE_SETTINGS_TAB_TEST`.

### `sites_on()`, `wanted()`, `pause_tab_on()`

The tab needs the screens' site, whose sets are `menu_mouse` and `quit_menu`, game 1.1.1, and
`NX2SARIA_PAUSE_SETTINGS_TAB` set to other than 0. Read once: a settings reload that turned it off while the tab was
open would stop the updates that give the System page back.

### `pause_tab_name_kind()`, `start_line()`

The name setting, and the start line, written at the first pad read.

### `layout_of()`, `layout_named()`, `pane_at()`, `tree_ok()`

A screen's layout, a layout's name, a pane by its index (which must belong to that layout), and the shape check: the
parents of the panes the tab uses, against `runtime/ui/pause_tab_core.h` `PT_SYSTEM_TREE` and `PT_CATEGORY_TREE`.

### `pane_chain_alpha()`

The opacity a pane is drawn at: its own alpha times that of each parent that passes its alpha on (flag 8), 0 when it
or a parent is not shown (`runtime/ui/pause_tab_core.h` `pt_chain_alpha()`). The tab's name and plate take the fourth
category's, so they fade in with the header instead of standing at full alpha while the game's names come up.

### `pane_box()`, `to_picture()`

A pane's box in layout units (`runtime/ui/menu_mouse_geom.h` `mm_pane_box()`), and a box turned into fractions of the
picture with the screen's projection (`mm_units()`; 16:9 taken when it cannot be read).

### `screens_seen()`

The line of `NX2SARIA_PAUSE_SETTINGS_TAB_TEST=screens`: each screen the site runs for, by its layout's name (48 at most).

### `trace()`

The line of `NX2SARIA_PAUSE_SETTINGS_TAB_TEST=log`: why R is or is not taken, when that changes.

### `screen_takes_input()`, `focused_entry()`, `system_layout_of()`

The pause menu is open, shown, not blocked and its controller takes input; the focused entry of player 1; the
`PaSystemMenu_00` layout above the focused button, if any (the walk stops at the pause menu's own layout).

### `drop_system()`

Lets go of the System page's layout. A page it hid gets its shown bit back first, if the pane still belongs to that
layout and the layout is still `PaSystemMenu_00`.

### `pause_tab_click_wanted()`

A click on the tab's name from another page (`runtime/ui/menu_mouse.c` walks to the System tab): the next 4 s, the
first time R would be taken the tab opens by itself.

### `plus_tick()`

At the pause menu's update. Arms R when the focus is on a System page button and the page stands in the middle: its
part pane `Pa_System_00` (the host of the `PaSystemMenu_00` layout) at x within 2 units of 0 and where it was the
update before (`pt_settled()`); at 21:9 with the wide menus the page that shows is also in the middle. While the tab is
open it clears the shown bit of the page's `N_InOut_00`, if it was set, and sets it again once the tab closes; the
layout found when arming is kept, checked against the pause menu's layout each update, so the bit is put back on the
page it was taken from. The page box (`runtime/ui/pause_tab_core.h` `pt_page_box()`, wider with the wide menus spread) goes to the
picture with this screen's projection.

### `static_header()`

The tab's box when the header was not read lately (200 ms): the layout's own categories (`pt_header_boxes()`) through
the pause menu's projection, shown while the pause menu is open and not hidden. Measured on the bench, the System
category's text is at y 1000, where `MenuOverlay_00` puts it.

### `category_layout()`

A category's layout: its +0x20 holds the layout's holder, whose +0x80 is the layout, as a screen's +0x28 does.

### `pause_tab_five()`, `moved_panes()`, `place()`, `respace()`

The header in five places, by moving the panes' own (local) position, which the game turns into the matrix at +0x28
on its next update. Where that position lies in a pane is found, not assumed (`find_trans()`, `g_trans`): the one
offset outside the matrix where all six panes hold the game's x and y (`runtime/ui/pause_tab_core.h`
`pt_find_trans()`); none, or more than one, and the header is left alone. +0x60 holds 1.0 in all six on the bench (a
scale), which the first guess took for the position: the check before writing refused it. While the fourth category is set shown (the Plus menu's header; the Minus
menu clears that bit), the four categories go to x -840, -420, 0 and 420 and the L and R icons to -1100 and 1100
(`runtime/ui/pause_tab_core.h` `PT_FIVE_X`), so the tab gets the fifth place at 840; with the bit clear they go back to
the game's -690, -230, 230, 690 and -990, 990 (`PT_GAME_X`). The panes are the header layout's 8 to 11, 13 and 14,
checked by shape (`PT_OVERLAY_TREE`) and against the categories' own part panes, and written only when all six are
where the game or this tab puts them (and the fourth's y is 0, R's 2); with the bit clear, other places (the Minus
menu's own, if it had them) are left alone and turn nothing off. 150 ms after a move the fourth category's box
must be centred within 3 units of 420 (and `NX2SARIA_PAUSE_SETTINGS_TAB_TEST=nofive` must be off); otherwise
everything goes back, the line says what the pane holds from +0x58,
and the tab keeps the place after the R icon for the session (`g_five_failed`). `g_five` is read by
`runtime/ui/menu_mouse.c` for its clicks on the tab row.

### `header_tick()`

At the header's update (`MenuOverlay_00`). True when it could read the categories' boxes. A header with other than four categories gets no tab (one line). The tab's box is the fifth place
(`pt_five_box()`) once the header is in five places, else it follows the third and fourth categories' part panes
(`pt_tab_box()`), and shows only while the fourth is shown with all its parents:
the Plus menu's header, faded in. While the tab is open the fourth category's plate `N_Bg_00` loses its shown bit (the
category is drawn as not selected), and gets it back when it closes.

### `pause_tab_screen_host()`

Called by `runtime/patches/patches.c` `screens_host()` for every interface screen's update; picks the pause menu and its header by their layouts' names.

### `pause_tab_view()`, `pause_tab_request_take()`, `pause_tab_set_open()`, `pause_tab_applied()`

For `runtime/ui/menu.cpp`: the boxes, the request to open (taken once), the open state with the reason it closed and
the time it was open, and the Apply line. A close drops a request still waiting, so the tab never opens again by
itself.

### `pause_tab_more()`

"More settings…": the line when the tab gives way to the settings menu, and when that menu closes, whether the pause
menu is still seen under it (`g_page_ms` within `SEEN_MS`). The settings menu blocks the game's input while it is open,
so the game stays in its pause menu, and the buttons that close it are held from the game for a second after
(`runtime/input/input.c` `blocked()`): closing it lands back in the pause menu.

### `pause_tab_exit_log()`

At the orderly end (`runtime/gfx/gfx_present.c` `rt_exit_steps()`), with the tab on: one line with the session's
openings (by R, by a click), closings by reason (`CLOSE_WHY`, the reasons `runtime/ui/menu.cpp` passes; anything else
is "other"), Applies and their changes, More settings, and the header's state (`g_five_state`: never shown, in five
places, put back, or left alone and why).

### `pause_tab_poll()`

Once per pad read, with the polls of the mouse in the menus and Quit Game (`runtime/input/input.c` `input_poll()`). A
new R while armed is taken away from the game and asks the menu to open the tab; it stays taken while held
(`runtime/ui/pause_tab_core.h` `pt_press()`). A new left click on the tab's name while armed asks the same; clicks counted while the poll did not run (the menu
open, the poll skipped) are not new. No request is made while one waits. The pad is
read on more than one thread: one at a time decides (`busy`), the others drop R while it is held taken.

## `runtime/ui/pause_tab_core.h`

The parts of the tab that need neither the game's memory nor the runtime, so `tests/pause_tab_check.c` runs them.

- `pt_press()`: the R that opens the tab. R held from before the arming goes to the game; a new R while armed is taken
  and stays taken until let go.
- `pt_tab_box()`: the tab's box from the boxes of the last two categories: as high as the last and `PT_TAB_HALF`
  (0.325) of its width each side of a centre `PT_TAB_STEP` (1.075) of their spacing past it, which at 16:9 is
  1054..1314: between the game's R icon (`T_BtnR_00`, 936..1044) and the rupee counter's icon (1324..1468). The game's
  own plate is 450 wide; there is no room for that much there.
- `pt_settled()`: the page's x in the middle and still.
- `PT_SYSTEM_TREE`, `PT_CATEGORY_TREE`, `pt_tree_ok()`: the parents of the panes the tab relies on, as the game's
  `PaSystemMenu_00` (1 `N_All_00`, 2 `N_InOut_00` under it, the controller picture, the ornament and the buttons
  under that) and `PaMenuCategory_00` (3 `N_Bg_00` under `N_BgIn_00`, with the two plates under it) have them;
  `PT_SYSTEM_INOUT` and `PT_CATEGORY_BG` are the panes written.
- `pt_alpha_clamp()`, `pt_chain_alpha()`: a pane's opacity through its parents (see `pane_chain_alpha()`).
- `pt_name_kind()`: `product` (and unset) or `settings`; anything else is the product's name, said in the start line.
- `pt_header_boxes()`: the third and fourth categories of `MenuOverlay_00` in layout units (x 30..430 and 490..890,
  y 888..1112), for the tab's place when the header object is not found.
- `pt_page_box()`: the page's box in layout units, under the header's plate (its bottom at 382) and over the footer's
  guides (their top at -939): x -1560..1560, y -880..410. With the wide menus spread (`wide_menus = full`, the default)
  it grows by the picture's extra units `u` on each side, as the game's pages do (`runtime/patches/wide_hud.c`).
- `PT_GAME_X`, `PT_FIVE_X`, `pt_spacing()`: the x of the four categories, L and R as the game has them and in five
  places, and which of the two a reading is (within half a unit), or neither. `pt_five_box()`: the fifth place, x
  670..1010, between the moved System category and R. `pt_five_tab_at()`: a click on the tab row in five places: the
  four categories, L (x below -1040), R (above 1040) and the tab (`PT_TAB_SETTINGS`). `PT_OVERLAY_TREE`: the parents of
  the header panes moved (the categories under `N_MenuChange_00`, the icons under `N_InOut_01`).

## `runtime/ui/binds_page.h`

The remapping rows of the menu's Controls tab (`runtime/ui/binds_page.cpp`). Present path only (ImGui). What the player
sees: [controls.md](../controls.md).

### `binds_rows()`, `binds_after_page()`

`binds_rows()` runs inside the page's `opt_begin()` / `opt_end()`: every action's two keys and its controller button,
remapped by selecting a cell and pressing the new one, and the reset of them all. `binds_after_page()` runs after
`opt_end()`: the window that waits for the press.

### `binds_modal()`

True while a cell waits for a press, or its press is still reaching the menu: B, Esc, L / R and the tab keys stay on the
page.

### `binds_cancel()`

The menu opened or closed (`runtime/ui/menu.cpp`): a cell still waiting for a press stops waiting, unchanged. Its
window is closed with the menu's popups.

## `runtime/ui/binds_page.cpp`

The page edits a copy of the input tables (`runtime/input/input_map.h`) and writes the `NX2SARIA_BIND_*` lines that
give the copy; `runtime/input/input.c` `binds_refresh()` applies them at its next poll. The lines join the menu's draft
(`runtime/ui/option_rows.cpp` `opt_change()`), and the settings file is written when the player applies it. The gamepad
cells show and change the table of the button layout in use (`NX2SARIA_PAD_LAYOUT`).

`host.h` pulls in `cpu.h`, which is C only, so the file declares the one function it needs from it, `rt_log`, as it is
declared there.

`kDoneNs`, 350,000,000 ns: the capture window stays this long after the press. The press and its click reach ImGui a
frame or two later and must land on the window, not on a cell (a new capture), on Continue, or as Esc closing the menu.

The capture state `g_cap`: `action` and `cell` are the cell waiting for a press (cell 0 and 1 are the keys, 2 the
controller; `action` -1 is none); `reset` makes the next capture poll take what is held as the start; `start` and
`done` are `plat_now_ns` of the start and of the press, timeout or Esc (`done` 0 while still waiting). `g_note` is what
the last change took or swapped, shown after its row's description. `g_reset_asked`: the reset row was chosen this
frame, and is acted on after the page.

### `action_desc()`

The game gives each D-pad direction its own action, so the actions up to `B_DOWN` each have a description of their own
(`UI_BIND_DESC_A + a`).

### `key_label()`

A key as the player sees it: its catalog name, the numpad format, the character on a punctuation key in the keyboard
layout in use, else its settings-file name in capitals (letters, digits, F keys).

### `pad_label()`

A gamepad button: the label the open pad has on a face button, else its place on the pad.

### `shown_layout()`

The layout the gamepad cells show: the one in effect, or without a gamepad the one the setting names (Xbox for auto).

### `write_lines()`

Writes the `bind_` line of every action whose line for the map differs from the one set; a line at the defaults goes
away. Each goes through `opt_change()`: previewed now, saved on Apply.

### `change()`

Cell 0 and 1: a key (`KEY_NONE` clears it). Cell 2: a gamepad button (`PAD_NONE` clears it).

### `reset_binds()`

Skips an action that is at the defaults already or whose line the environment fixes (`cfg_from_env()`).

### `row_action()`

The third cell (the controller) is enabled only for the actions before `B_SL_LEFT`: the sticks stay sticks, and the
shake is the right stick's. A cell's clear choice calls `change()` with 0, which is `KEY_NONE` and `PAD_NONE`.

### `capture_window()`

The window over the page while a cell waits. The press is polled from the devices (`runtime/input/input.h`
`input_capture()`), so the menu's own keys (Enter, arrows, Q / E) bind like any other. Esc or the timeout
(`CAPTURE_SECONDS`, 5) cancels; a controller button cancels a key cell.

The timeout is tested first: a press polled after it would compare with a held state long gone and bind what is held
now. When the window closes, the cursor goes back on the cell it came from (`opt_focus_key()`).

### `binds_rows()`

There is no row for `B_SHAKE`: the game has no use for it (`bind_shake` still sets a key).

## `runtime/ui/amiibo_page.h`

The Amiibo / NFC menu page (`runtime/ui/amiibo_page.cpp`), and the amiibo list on its own over the game (the picker).
The feature: [amiibo.md](../amiibo.md).

The picker is the amiibo list that `runtime/ui/menu.cpp` opens in the menu's place when the game starts waiting for an
amiibo (`NX2SARIA_AMIIBO_POPUP`). Present path only.

### `amiibo_picker_wanted()`

Called every overlay frame: true on the frame the game starts waiting for an amiibo while the popup setting is on.

### `amiibo_picker_begin()`

The picker opens: the folder is read again and the cursor goes to the list.

### `amiibo_picker_layout()`

Lays the picker out; false when it should close (a figure was tapped, B or Esc, the game stopped waiting).

### `amiibo_note_shows()`, `amiibo_note_draw()`

After a tap from the picker: "Tapped: <figure>" at the top of the screen for a few seconds.

## `runtime/ui/amiibo_page.cpp`

The tab (Dear ImGui): the dumps of the amiibo folder on the left (A or a click taps one), the reader and the figure on
it on the right, and under "Advanced" the folder, the reader's options, the figure's details, the keys file and
write-back. The picker is the same list on its own over the game.

`imgui_internal.h` is included for `SetFocusID` and `ScrollToItem`.

The dumps of the amiibo folder and its subfolders are kept as one list of `Dump`: `path` as
`runtime/hle/amiibo.h` `amiibo_load_file()` takes it, and as `AmiiboInfo.path` then holds it; `label`, the path under
the amiibo folder without the extension ("Young Link", "Zelda/Young Link"); `key`, the label in lower case, for sorting
and the search field. `kMaxDepth`, 3: the subfolders listed below the amiibo folder. `g_root` is the folder read last.

The last action is kept as a catalog id (`g_status_id`) and the figure or file it names (`g_status_arg`), and formatted
when drawn, because the language can change.

*Erase game data* asks twice: the first press arms the button, a second one within `kEraseConfirmS` (3.0) seconds
erases. `g_erase_armed_at` is `ImGui::GetTime()` of the first press, negative while not armed.

`g_cursor_path`: the dump the list's cursor was last on (moved there, clicked or tapped), in the tab or the picker, for
this run. The list opens on it again.

The folder dialog (`runtime/ui/folder_pick.h`) answers on its own thread (`on_folder_picked()`, under `g_pick_lock`);
the page takes the answer when it draws.

`g_note_until` and `g_note_label`: "Tapped: <figure>" after a tap from the picker, shown until `g_note_until`
(`plat_now_ns`).

### `is_dump()`

`amiibo_load_file()` takes whole 540-byte dumps only (`AMIIBO_DUMP_SIZE`), so other files of that name, the keys file
`key_retail.bin` among them, stay out of the list.

### `scan_dumps()`

Reads the amiibo folder (`NX2SARIA_AMIIBO_DIR`, else `./amiibo`) again. A missing folder lists nothing and says so. A
name the narrow encoding cannot hold throws from `string()`; the `try` around the walk keeps the dumps read before it
listed.

### `set_root()`

The folder a player set: saved like a row's change (`runtime/ui/option_rows.cpp` `opt_apply()`); `""` goes back to the
default.

### `tap()`

Loads a dump and puts it on the reader; true when it is on the reader. It disarms the erase button first: a press
armed for the previous figure does not erase this one.

### `figure_name()`

The loaded figure's name as the list shows it: its file name without the extension.

### `text_wrapped_fmt()`

A catalog format and its arguments, wrapped to the pane: a path or a long translation does not fit one line.

### `empty_note()`

The list is empty: the folder is missing or holds no dumps.

### `dump_list()`

The dumps as one selectable line each; A, Enter or a click taps one. The loaded figure is marked: a cyan dot while it
is on the reader, a gold one after it left. `focus`: the cursor goes to the line it was last on (`g_cursor_path`), else
the loaded figure, else the first line. `filter`: only the lines holding `g_filter`. Returns the dump tapped this
frame, or NULL.

- The label is drawn by hand over a `Selectable("##dump")`: a `#` in a file name would otherwise be read as part of
  the item's id.
- A window that appears (the picker, each time it opens) gives the cursor to its first row a frame later unless a row
  says it is the default, so the row to focus calls `SetItemDefaultFocus()` as well as `SetFocusID()`.

### `draw_figure()`

The figure on the right: its name, whether it is on the reader, its nickname and game data, Lift.

### `draw_reader_options()`

The hold time's label is drawn above the slider: beside it the text is cut off.

### `draw_figure_details()`

The loaded figure's identity and stored data, and the button that erases its game data. The game data goes for good
(the port's copy in the profile too), so the button asks for a second press. `###` in its label keeps one id under both
labels: the controller's cursor stays on the button when its label changes.

### `keys_path()`

The keys file `runtime/hle/amiibo.h` `amiibo_reload_keys()` reads: `NX2SARIA_AMIIBO_KEYS`, else `key_retail.bin` in the
amiibo folder (`NX2SARIA_AMIIBO_DIR`, else `./amiibo`).

### `draw_keys()`

The keys: loaded or not, the file in effect, *Reload keys*; and the write-back setting (`NX2SARIA_AMIIBO_WRITEBACK`, a
live setting), saved like a row's change (`opt_apply()`).

### `draw_status()`

The last action, under the reader's state; nothing until there is one.

### `tab_amiibo()`

- `scale` is unused: the widgets take ImGui's own sizes, already scaled with the font.
- `runtime/hle/amiibo.c` reads the keys with the first dump; the tab's first draw reads them too, so its keys line is
  right before any tap.
- When the menu opened or the tab was switched to (`opt_focus_take()`), the folder is read again (dumps added since
  show) and the cursor goes to the list.
- The panes' height leaves `opt_bottom()` free, so Continue and Quit stay visible.

### `amiibo_picker_wanted()`

`amiibo_get_reader_state()` holds "searching" across the game's many restarts of detection while it waits, so the
start of a wait is the change from not searching to searching.

### `amiibo_picker_layout()`

Closes when the reader no longer searches: the game gave up waiting (its own time-out, a cut scene), and there is
nothing left to tap for. Under the list it leaves room for the hint, and for a failed load's line (3 text lines
instead of 1).

## `runtime/ui/cheats_tab.cpp`

### `tab_cheats()`

The Cheats tab: a heading per mod, a row per cheat of its cheat file and a line per group label, in the order
`runtime/patches/cheats.c` `cheats_info()` gives. The menu shows the tab only while there are cheats
(`runtime/ui/menu.cpp` `tabs_shown()`).

A row is drawn by `runtime/ui/option_rows.cpp` `opt_toggle_text()`, so A, left and right flip it and the mouse flips it
on the two framed arrows, as on every other on/off row. Only the code, held-button and data cheats can be switched; the
other kinds are greyed out with the reason.

A flip changes the draft and nothing else: the row hands `opt_change()` the cheat's own setting
(`CheatInfo.key`, `NX2SARIA_CHEAT_<hash>`), the cheat's name as the line's label and On / Off as its from and to. The
row shows the draft's value (`opt_get()`), so a cheat flipped and not yet applied reads as it will be. Apply writes the
setting and saves the file; Discard drops the change; the cheat itself switches when its setting changes
(`runtime/patches/cheats.c` `cheats_frame()`). The tab calls nothing that writes a setting or switches a patch set.

The value a flip writes: `1` to switch on; to switch off, `0` when the settings hold a line for the cheat and unset
(`""`) when they hold none. So a cheat switched on and off again before Apply is back at the value it had, and the
change leaves the draft instead of adding a `cheat_<hash> = 0` line to the file.

When an enabled mod has code cheats or ExeFS patch files the build has not taken up
(`runtime/patches/cheats.c` `cheats_unbuilt()`), the tab's first line says how many and that Play from the launcher
builds them in, the words of the log's line (`UI_MENU_CHEATS_UNBUILT`, two `%s`: the code cheats, then the ExeFS patch
files, each a counted noun from `runtime/ui/ui_text.c` `menu_count()` with `UI_MENU_COUNT_CODE_CHEATS` and
`UI_MENU_COUNT_EXEFS_FILES`; English, Russian, Brazilian Portuguese and Simplified Chinese). A mod's heading counts
its cheats the same way (`UI_MENU_CHEATS_MOD`, `UI_MENU_COUNT_CHEATS`). The Russian, Portuguese and Chinese sentences
say Play in the launcher, which is how those languages put it. There is no button: the game no longer restarts into a
rebuild.

A mod's heading comes first, then a row per cheat in the file's order and a line per group label of the file
(`CHEAT_HEADING`). A label that holds `TID=` is not shown: the file's title line only names the game.

## `runtime/ui/debug_menu.h`

The debug menu's window (`runtime/ui/debug_menu.cpp`), laid out by `runtime/ui/menu.cpp` in the settings menu's place.

### `debug_menu_layout()`

Called between `ImGui::NewFrame` and `Render`, on the present thread.

### `debug_menu_tab()`

The tab to show at the next layout (test hook): 0 to 4.

## `runtime/ui/debug_menu.cpp`

The debug menu's panel (F9 with `debug_menu = 1`). `runtime/ui/menu.cpp` lays it out in the settings menu's place
while it is open, so its input, its sounds and the game's blocked pad are the menu's own. It sits on the left edge, in
the performance overlay's look, with the game in view beside it. What it does to the game is in
`runtime/diag/debug_tools.c`: the game's on/off flags by name, teleporting to the game's own
start positions, starting its events, the game's speed, Link's position, a free camera, and a search of the game's
memory with values to set and to hold. It is plain Dear ImGui with English text: a tool for testing, not part of the
game's interface. Present thread only.

State: `g_tab` is a tab asked for by the test hook (`debug_menu_tab()`), -1 for none. `f_rows` holds the flags that
pass the filter; `f_age` (starts at 1000) counts the frames since the values were read. `t_group` is the teleport
list's group: 0 all, 1 the warp songs' pads, 2 the two sides of the loading zones, 3 boss rooms. `m_type` is the
memory search's value type: 0 for 4 bytes, 1 for 2 bytes, 2 for 1 byte, 3 for a float.

### `flags_filter()`

Sorts the named flags first, by name; the rest by hash, as the back end lists them. In a test run
(`NX2SARIA_DBG_TEST` set) it logs `debug: filter "<text>": N flags shown`.

### `tab_flags()`

With "only on" ticked the list is filtered again whenever the values are read, because values moved. After a flag is
flipped, `f_age` is set to 15 so the values are read again in a few frames: the game applies the change at its next
update.

### `in_group()`

The groups go by the words the game's own names use: a warp song's pad is named after its song (Minuet, Bolero,
Serenade, Requiem, Nocturne, Prelude), a loading zone's far side after where it leads, and a boss's room is an area of
its own whose name ends in `Stage`.

### `status_line()`

The last thing the teleport or an event start did, and whether one is under way.

### `lists_ready()`

The lists come with the flag names, from the game's files: reading them starts once the game's flags are there.

### `tab_memory()`

The type buttons are laid out in two rows, because the panel is narrow.

### `debug_menu_layout()`

A panel on the left edge in the performance overlay's dress (the game's panel art, gold labels, the smaller text), as
tall as the window: the game stays in view beside it while a flag or a value is changed. The ImGui window's own
background is made transparent, because the panel drawn below it is the background.

## `runtime/ui/perf_panel.h`

The full performance overlay's panel (`runtime/ui/perf_panel.cpp`), drawn in the settings menu's look: what
`runtime/ui/menu.cpp` `perf_full_draw()` gathered, laid out in tables on the menu's framed panel with the game's
divider between the sections. The overlay itself: [performance.md](../performance.md).

`PerfPanel`: `s` is `perfmon_snapshot`'s result. `version` is "NX2Saria <version>, game <update>". `picture[12][64]`
holds the window, game image and scene lines, then the settings as the menu names them (upscaling, anti-aliasing, frame
limit, VSync, widescreen, resolution, HDR, frame generation), and the quality preset in use, which is shown before
upscaling; an empty string leaves its line out. `scene_known`: the scene line is shown (`runtime/gfx/resolution.c`
sets the scene's size). `fg_fps`: the frames the display gets with frame generation, 0 without it. `under_menu`: the
menu is open, and the panel goes under its window.

### `perf_panel_draw()`

The panel at the top right corner of a `width`-pixel-wide frame.

## `runtime/ui/perf_panel.cpp`

The panel in the settings menu's look: the menu's framed dark panel (`runtime/ui/menu_skin.cpp`), the numbers in
tables with gold labels and parchment values, the game's divider between the machine, the frames, the per-frame waits
and the picture, and the frame times as a gold line.

`imgui_internal.h` is included for the window's draw cursor (the plot does not widen the panel). `kLabel` is
`SKIN_GOLD` (236, 196, 104) and `kValue` is `SKIN_TEXT` (244, 236, 214).

### `column()`

The panel keeps its width while the numbers change: every column is as wide as the widest text it can show, measured
on samples of 8s (the menu fonts' digits share one width), and the numbers are right-aligned in it. `column()` sets up
a fixed column as wide as the widest of the formats (with their samples) and labels given.

### `cell()`

One cell: a catalog format with one number, or "-" when the machine gives none.

### `divider()`

The game's divider across the panel between two sections, smaller than the menu's (0.72 of the scale).

### `machine_and_frames()`

- The card's memory figure counts every program on it, so this game's own share is shown beside it; where that is
  unknown, the clock takes the cell.
- The frames: the rate over the last second with the lows over the last ten seconds, then the frame times.
- With frame generation (`fg_fps` above 0), a line for what the display gets, generated frames included.

### `frame_plot()`

The frame times of the last `PERF_HISTORY` frames as a gold line over a dark band, as wide as the tables made the
panel. The plot does not count toward that width, so the panel can still shrink.

### `waits()`

Per frame, averaged over the last half second. The labels' column stretches, so the numbers end at the panel's right
edge, and the table still asks for no more than its widest label and number.

### `picture()`

The order of the lines is `{0, 1, 2, 11, 3, 4, 5, 6, 7, 8, 9, 10}`: the preset (11) first among the settings, as the
Graphics group starts with it.

### `perf_panel_draw()`

- The ImGui window's background is transparent: the panel drawn below is the background.
- The window goes under the menu while it is open (the settings stay readable), else over everything.
- The draw list is split in 2 channels: the panel goes under the text, and is drawn once the window's size is known.
- The port version and the game update come first, so a screenshot in a bug report carries both.

## `runtime/ui/folder_pick.h`

The host's own folder and file dialogs, for menu pages that point at a folder (the Amiibo tab) or a file (a shared
save's zip, the Game tab), and the player's Downloads folder: `runtime/ui/folder_pick_win.cpp` with Win32,
`runtime/ui/folder_pick_sdl.c` with SDL. A folder is chosen here or by walking the page's list, not typed.

`FolderPickDone`: `done` gets the chosen folder (UTF-8), or NULL when the player cancelled or the host has no dialog.

### `folder_pick()`

Opens the dialog at `start` (which may be relative or missing) and returns at once; `done` runs later on another
thread, once. The caller keeps one dialog open at a time.

### `file_pick()`

The same for one file: the dialog lists the files whose name ends in `.` and `ext` (`zip`) and offers all files too.

### `folder_downloads()`

The player's Downloads folder (where the system keeps it, also when moved) into `out`, UTF-8. 0 when there is none.

## `runtime/ui/folder_pick_sdl.c`

The SDL front end of `runtime/ui/folder_pick.h`: `SDL_ShowOpenFolderDialog` and `SDL_ShowOpenFileDialog` (the desktop
portal, else zenity), started on the window thread, which runs SDL's event loop that delivers the portal's answer; and
SDL's user folders.

A request holds `start` (absolute, or empty for the dialog's own default), `ext` (a file of this extension; empty for
a folder) and `filters`, which SDL reads until the answer comes, so they live in the request.

### `show()`

The file filter's name is the extension itself, so there is no text to translate.

### `folder_downloads()`

SDL ends the folder with a separator; it is cut off.

## `runtime/ui/folder_pick_win.cpp`

The Win32 front end of `runtime/ui/folder_pick.h`: the shell's open dialog (`IFileOpenDialog`; `FOS_PICKFOLDERS` for a
folder), owned by the game window, on a thread of its own so the present thread that draws the menu keeps running; and
the shell's Downloads folder. Only the Windows build compiles this file.

A request's `ext`: a file of this extension; empty for a folder.

### `set_start()`

The dialog wants an absolute folder that exists; anything else leaves it at the shell's last folder.

### `pick_main()`

The file filter's pattern (`*.<ext>`) is its own name, so there is no text to translate.

### `folder_downloads()`

`FOLDERID_Downloads` is spelled out as a GUID (`374de290-123f-4565-9164-39c4925e467b`), so no import library has to
carry it.

## `runtime/ui/ui_text.h`

Text the host shows the player (`runtime/ui/ui_text.c`): one catalog, keyed by stable ids. The software keyboard reads
it in the game's language (`ui_text()`); the menu and the performance overlay read it in the menu language
(`menu_text()`). How the texts are translated: [translating.md](../translating.md).

`UI_DESC_BYTES`, 2048: the bytes of the menu's description pane (`runtime/ui/option_rows.cpp`, `Desc`), a description
plus the note appended to it ("after restart", "set by environment variable", a disabled row's note). UTF-8 Cyrillic
takes two bytes a letter, so the longest Russian description is about 850 bytes; the probe checks every menu text
leaves room for a note.

### `UiTextId`: what each text is for

The ids come in groups, in this order: the software keyboard; the menu's window, tabs and bottom row; Screen; Game;
Sound; Controls; the Controls tab's remapping rows; the Amiibo tab; the Game tab's Discord row and Testing rows; the
language names; the Cancel of the confirmation boxes; the performance overlay; the Cheats tab; the Saves group; the
capture note; the boot screen; the Picture rows.
`UI_MENU_FIRST` to `UI_MENU_LAST_ID` carry English, Russian, Brazilian Portuguese and Simplified Chinese texts.

An id is added and removed together with its strings in all four languages: when the last code that names it goes,
the id and its strings go in the same change (`tests/ui_text_ids_check.py` fails on an id nothing uses).

For a translator, what a text is for and what its printf arguments take:

| Id | Note |
|---|---|
| `UI_SWKBD_TITLE` | caption of the text-entry window |
| `UI_SWKBD_PROMPT` | prompt when the game gives no header, guide or sub text (the game names a horse) |
| `UI_SWKBD_DEFAULT_NAME` | text the entry starts with when the game gives none, so a controller can confirm it; at most 9 UTF-16 units, no `@`, `%` or `\` |
| `UI_SWKBD_PAD_HINT` | how a controller answers |
| `UI_PERF_LINE_FPS` | printf format of the overlay line in FPS mode: the frame rate |
| `UI_MENU_DEFAULT_MARK` | "default" beside the default value |
| `UI_MENU_AFTER_RESTART` | note on a setting that is read at start |
| `UI_MENU_SET_BY_ENV` | note on a setting an environment variable fixes |
| `UI_MENU_INI_UNKNOWN` | "...: %s": the keys of the settings file that are no settings of this version (`cfg_unknown_keys`) |
| `UI_MENU_VERSION` | "NX2Saria %s, game %s": the port version and the game update, beside the title |
| `UI_MENU_PRESENT_IN_USE` | "in use: %s" under the value when the swapchain got another mode than the setting |
| `UI_MENU_LOW_LATENCY`, `UI_MENU_LOW_LATENCY_DESC`, `UI_MENU_LOW_LATENCY_BOOST`, `UI_MENU_LOW_LATENCY_NO_DRIVER`, `UI_MENU_LOW_LATENCY_DLSSG`, `UI_MENU_LOW_LATENCY_FG`, `UI_MENU_LOW_LATENCY_FAILED` | low latency without frame generation (`runtime/ui/menu.cpp` `row_low_latency()`, `runtime/pacing/vk_lowlat.c`). The last four are the row's notes: why it is greyed out (no such driver mode, DLSS frame generation's own, a frame generation presents the frames) or set and not in use (the driver refused it) |
| `UI_MENU_GRAPHICS` | group heading over the resolution rows |
| `UI_MENU_WIDE_HUD`, `UI_MENU_WIDE_MENUS` and their `_DESC`, `_CENTER`, `_EDGES`, `_FULL` | where the HUD and the menus go on a picture wider than 16:9 (`NX2SARIA_WIDE_HUD`, `NX2SARIA_WIDE_MENUS`) |
| `UI_MENU_TEX_DETAIL_AUTO` | "Automatic: %s", `%s` the level it picks on this card |
| `UI_MENU_AA_TAA_FSR4` | "TAA (%s)": FSR 4's name as the Upscaling row gives it ("AMD FSR 4.1") |
| `UI_MENU_FPS_DISPLAY`, `UI_MENU_FPS_DISPLAY_HZ` | the monitor's refresh rate; the second is a printf format with the rate in Hz, used when it is known |
| `UI_MENU_FPS_RTSS`, `UI_MENU_FPS_RTSS_READONLY` | notes under the frame rate value: RivaTuner Statistics Server (`runtime/pacing/rtss_limit.c`) limits the game, or cannot |
| `UI_MENU_RUMBLE_HD`, `UI_MENU_RUMBLE_HD_DESC`, `UI_MENU_RUMBLE_HD_NEEDS` | HD rumble, and the note on its row while the gamepad in use cannot take it (`runtime/input/input.h` `input_rumble_hd_available()`) |
| `UI_MENU_GROUP_GYRO`, `UI_MENU_GYRO_*` | the Gyro group (`runtime/ui/menu.cpp` `rows_gyro()`): its heading, rows and dead zone levels, and the note on its rows while motion controls are off |
| `UI_MENU_PROMPTS_MISSING`, `UI_MENU_PROMPTS_FAILED`, `UI_MENU_PROMPTS_RESTART`, `UI_MENU_PROMPTS_OFF_SHAPE`, `UI_MENU_PROMPTS_OFF_MODS`, `UI_MENU_PROMPTS_OFF_PATCHES` | the notes under the prompts: their files are missing or damaged, they change after restart, or why they are off |
| `UI_MENU_RESTART_CONFIRM`, `UI_MENU_QUIT_CONFIRM`, `UI_MENU_RESTART_YES`, `UI_MENU_RESTART_SAVING` | the confirmation of the bar's Restart and Quit (its Cancel is `UI_MENU_RESET_NO`) |
| `UI_MENU_RESTART_COUNT` | the bar's Restart with the count of settings waiting for it (printf: `%d`) |
| `UI_MENU_MORE_SETTINGS`, `UI_MENU_MORE_SETTINGS_DESC` | the pause menu's settings tab's last row, which opens the settings menu in its place (`runtime/ui/menu.cpp` `pause_tab_rows()`); the Russian, Portuguese and Chinese texts are new and wait for a speaker's review |
| `UI_MENU_PAUSE_TAB` | the pause menu's settings tab's name with `pause_settings_tab_name = settings` (`runtime/ui/menu.cpp` `pause_tab_label()`); also in the title face's glyphs |
| `UI_MENU_DRAFT_TITLE`, `UI_MENU_DRAFT_APPLY`, `UI_MENU_DRAFT_DISCARD`, `UI_MENU_DRAFT_KEEP`, `UI_MENU_DRAFT_RESTART_NOTE`, `UI_MENU_DRAFT_QUIT_NOTE` | the box that asks about the changes made in the menu when it is left (`runtime/ui/menu.cpp` `apply_confirm()`), and the lines restarting and quitting add to their confirmation while there are such changes |
| `UI_MENU_KEEP_TITLE`, `UI_MENU_KEEP_COUNTDOWN`, `UI_MENU_KEEP_YES`, `UI_MENU_KEEP_REVERT`, `UI_MENU_RESTART_NOW_TITLE`, `UI_MENU_RESTART_NOW`, `UI_MENU_RESTART_LATER` | after Apply: the "keep these display settings?" box with its countdown (`UI_MENU_KEEP_COUNTDOWN`, a printf format with one `%d`), and the "restart now / later" box for settings read at start |
| `UI_MENU_RELOAD_NONE`, `UI_MENU_RELOAD_DONE`, `UI_MENU_RELOAD_REJECTED`, `UI_MENU_RELOAD_FAILED`, `UI_MENU_RELOAD_BUSY` | the notice Ctrl+F5 shows after reading the settings file again (printf formats: changed / after restart [/ rejected]) |
| `UI_BINDS_PRESS_KEY` | "Press a key or mouse button for %s" |
| `UI_BINDS_PRESS_PAD` | "Press a controller button for %s" |
| `UI_BINDS_CANCEL_HINT` | "Esc cancels, or wait %d s" |
| `UI_BINDS_TAKEN` | "%s was taken from %s": the key, the action that lost it |
| `UI_BINDS_SWAPPED` | "%s swapped with %s": the two actions |
| `UI_BIND_A` to `UI_BIND_SHAKE` | the actions, in action order (`runtime/input/input_map.h`, `B_A` to `B_SHAKE`) |
| `UI_BIND_DESC_A` to `UI_BIND_DESC_DOWN`, then `UI_BIND_DESC_MOVE`, `UI_BIND_DESC_CAMERA`, `UI_BIND_DESC_SHAKE` | what an action does in the world, in action order up to the D-pad's Down; the stick directions share Move and Camera |
| `UI_BIND_ACTION_A` to `UI_BIND_ACTION_DOWN`, `UI_BIND_ACTION_MOVE`, `UI_BIND_ACTION_CAMERA` | short action labels shown beside each Switch control in the bind rows; the stick-direction rows share a label |
| `UI_KEY_SPACE` to `UI_KEY_PAGEDOWN` | key names, in key id order `KEY_SPACE` to `KEY_PAGEDOWN` (letters, digits, F keys and punctuation show as themselves) |
| `UI_KEY_NUM_FMT` | "Num %s": a numpad key |
| `UI_PAD_SOUTH` to `UI_PAD_RPADDLE2`, then the PlayStation labels | gamepad buttons, in pad id order `PAD_SOUTH` to `PAD_RPADDLE2`, then the PlayStation face labels |
| `UI_MENU_AMIIBO_DIR` and the ids after it | the Amiibo tab (`runtime/ui/amiibo_page.cpp`): the dump folder and its files, the emulated NFC reader and its state, the amiibo on it and the last action. `UI_MENU_AMIIBO_NO_DIR` and `UI_MENU_AMIIBO_NONE_FOUND` take the folder as `%s` |
| `UI_MENU_AMIIBO_NICKNAME` and the ids after it | the figure's own data (`runtime/hle/amiibo.h` `amiibo_get_figure_data()`): nickname, where it came from, the keys' check, the writes and the game data; erasing the game data (a second press confirms) |
| `UI_MENU_AMIIBO_WRITES` | `%u` the write counter, then `%04u-%02u-%02u` the last write date |
| `UI_MENU_AMIIBO_GAME_DATA` | `%u` bytes, `%08X` the access id |
| `UI_MENU_AMIIBO_KEYS` and the ids after it, `UI_MENU_AMIIBO_WRITEBACK` | the keys file (`runtime/hle/amiibo_crypto.h` `amiibo_keys_loaded()`, `runtime/hle/amiibo.h` `amiibo_reload_keys()`) and the write-back setting (`NX2SARIA_AMIIBO_WRITEBACK`); `UI_MENU_AMIIBO_KEYS_FILE`, `UI_MENU_AMIIBO_KEYS_RELOADED` and `UI_MENU_AMIIBO_KEYS_FAILED` take the file's path as `%s` |
| `UI_MENU_AMIIBO_POPUP`, `UI_MENU_AMIIBO_POPUP_TITLE`, `UI_MENU_AMIIBO_POPUP_HINT` | the picker over the game (`NX2SARIA_AMIIBO_POPUP`): its setting, its title and its controls |
| `UI_MENU_AMIIBO_DAILY_LIMIT`, `UI_MENU_AMIIBO_DAILY_LIMIT_DESC` | the Game tab's switch for the game's once-a-day amiibo limit (`NX2SARIA_AMIIBO_DAILY_LIMIT`) |
| `UI_MENU_REPLAY_WORKERS` and the ids after it | the Game tab's Testing rows (`runtime/ui/menu.cpp` `tab_game()`), English only |
| `UI_MENU_NEEDS_WORKERS`, `UI_MENU_NEEDS_PIPE` | why a row is greyed out |
| `UI_MENU_LANGNAME_*` | language names, each in its own language and the same in every row (the game language list) |
| `UI_MENU_OV_FPS_LABEL`, `UI_MENU_OV_FG_LABEL`, `UI_PERF_LINE_FPS_FG`, `UI_MENU_OV_FRAME_LABEL`, `UI_MENU_OV_FMT_FPS` | the overlay's frame rows: their labels, the rate |
| `UI_MENU_OV_LOW1`, `UI_MENU_OV_LOW01` | printf: the fps of the slowest 1% / 0.1% of the frames, no unit (the overlay column stays narrow) |
| `UI_MENU_OV_FMT_PCT`, `UI_MENU_OV_FMT_TEMP`, `UI_MENU_OV_FMT_POWER`, `UI_MENU_OV_FMT_MHZ`, `UI_MENU_OV_FMT_GB`, `UI_MENU_OV_FMT_GB_OF`, `UI_MENU_OV_GAME_GB`, `UI_MENU_OV_FMT_MS`, `UI_MENU_OV_PER_FRAME` | number formats of the overlay's cells (printf, one number each) |
| `UI_MENU_OV_WINDOW`, `UI_MENU_OV_OUTPUT`, `UI_MENU_OV_SCENE`, `UI_MENU_OV_LIMIT` | the picture lines: window, game image and 3D scene sizes, and the frame limit |
| `UI_MENU_OV_FMT_SIZE` | "%ux%u" |
| `UI_MENU_OV_FMT_SCENE` | "%ux%u, %.0f%%": the scene and its share of the game image's width |
| `UI_MENU_OV_FMT_FSR` | "AMD FSR 1, sharpness %d%%" |
| `UI_MENU_TAB_CHEATS` and the ids after it | the Cheats tab (`runtime/patches/cheats.c`): the file, the kinds of cheat and why one cannot be switched |
| `UI_MENU_CHEATS_NONE` | `%s`: `<build id>.txt` |
| `UI_MENU_CHEAT_KEY_DESC` | `%s`: the buttons |
| `UI_MENU_SAVES` and the ids after it | the Game tab's Saves group (`runtime/ui/menu.cpp` `rows_saves()`, `runtime/hle/save_share.c`): export the save into a zip, import a shared one at the next start. `UI_MENU_SAVE_EXPORTED` to `UI_MENU_SAVE_E_UNSAFE` are printf formats with at most one `%s` (a path, or the zip's NX2Saria version and date) |
| `UI_MENU_SAVE_IMPORT_WAITING` | the import row's value while an import waits for the next start |
| `UI_MENU_SAVE_BUSY` | note on the rows while an export or import runs |
| `UI_MENU_SAVE_EXPORTED` | `%s`: the zip's path |
| `UI_MENU_SAVE_IMPORT_READY` | `%s`: "<NX2Saria version>, <export date>" of the zip |
| `UI_MENU_SAVE_IMPORTED` | `%s`: the folder the replaced save was copied to, at this start |
| `UI_MENU_SAVE_IMPORTED_NEW` | the same with no save to replace |
| `UI_MENU_SAVE_E_WRITE` | `%s`: the folder the zip could not be written into |
| `UI_MENU_SAVE_E_READ` and the `UI_MENU_SAVE_E_*` after it | `%s`: the zip |
| `UI_MENU_CAPTURE_SAVED`, `UI_MENU_CAPTURE_FOLDER`, `UI_MENU_CAPTURE_FAILED` | the note after F8 (`runtime/diag/bugcap.c`, `runtime/ui/menu.cpp` `capture_note()`): `%d` the capture's number, `%s` its folder's name |
| `UI_BOOT_TITLE`, `UI_BOOT_STARTING` | the boot screen while the boot prewarm prepares shaders and pipelines (`runtime/boot.c`, `runtime/ui/menu.cpp` `menu_boot_frame()`) |
| `UI_BOOT_SHADERS`, `UI_BOOT_PIPELINES` | printf: `%lld` done, `%lld` total |
| `UI_BOOT_COMPUTE`, `UI_BOOT_DRIVER_CACHE` | after a driver change (`runtime/vk/vk_pipeline.c` `bf_decide()`): the compute pipelines, then the prepared shader variants built into the driver's own cache; printf as above |
| `UI_MENU_UPSCALING`, `UI_MENU_UPSCALING_DESC`, `UI_MENU_FRAMEGEN`, `UI_MENU_FRAMEGEN_OFF`, `UI_MENU_FRAMEGEN_NOTE`, `UI_MENU_HDR_TONEMAPPED`, `UI_MENU_HDR_NATIVE` | the settings menu's Picture rows for this game (`runtime/ui/menu.cpp` `tab_screen()`): one upscaling choice (bilinear, FSR 1 or a DLSS mode) and the picture's resolution |
| `UI_MENU_UPSCALING_DESC_FSR` | the Upscaling row's description on a GPU DLSS does not run on (not NVIDIA): the menu leaves DLSS out there |
| `UI_MENU_UPSCALING_DESC_NO_FSR` | where FSR 3.1 cannot run (`vk_ffx_available`: on Windows `amd_fidelityfx_vk.dll` does not load, elsewhere the runtime's own passes cannot run on the device, `runtime/vk/vk_fsr3.c`): the menu leaves it out there, and this text says so. Its English text differs by platform: off Windows there is no library to load, and the game's own passes (`runtime/vk/vk_fsr3.c` `vk_fsr3_init()`) found the device cannot run them |
| `UI_MENU_FSR3_OFF` | FSR 3.1 is still set but cannot run: "in use: off" and why |
| `UI_MENU_UPSCALING_NATIVE` | the scene at the picture's own size, no upscaler (`runtime/ui/menu.cpp` `share_native()`) |
| `UI_MENU_FRAMEGEN_NEEDS_DLSS` | DLSS only |
| `UI_MENU_FRAMEGEN_DESC_NONE`, `UI_MENU_FRAMEGEN_NEEDS_NONE` | neither runs |
| `UI_MENU_FRAMEGEN_DESC_FSR_WIN`, `UI_MENU_FRAMEGEN_NEEDS_FSR_WIN`, `UI_MENU_FRAMEGEN_OFF_FSR_WIN` | no DLSS, and FSR 3.1 upscales through the runtime's own passes while its frame generation needs AMD's library, which only the Windows build loads (`runtime/ui/menu.cpp` `row_framegen()`, `fsr_win_only`): the row says that, not that FSR 3.1 does not run. `OFF`: frame generation is still set to FSR 3.1 there |
| `UI_MENU_FRAMEGEN_FSR3_NATIVE`, `UI_MENU_FRAMEGEN_OFF_HDR` | FSR 3.1's frame generation through the runtime's own passes (every build but the Windows one, `runtime/vk/vk_fsr3fg.c`): the row's note while it is chosen, and why it is off while HDR is on (it generates SDR pictures only) |
| `UI_MENU_FRAMEGEN_MULT`, `UI_MENU_FRAMEGEN_MULT_DESC` | DLSS frame generation (`runtime/ui/menu.cpp` `row_framegen()`, `row_framegen_mult()`): the multiplier row, its notes when this GPU and driver allow less than 4x, and the frame generation row's notes when DLSS's does not run |
| `UI_MENU_FG_MULT_DESC`, `UI_MENU_FG_MULT_DYNAMIC` | the multiplier row where DLSS's frame generation runs through NGX itself (`runtime/ui/menu.cpp` `row_framegen_mult_ngx()`) |
| `UI_MENU_FRAMEGEN_MULT_RTX50` | this GPU and driver do 2x only |
| `UI_MENU_FRAMEGEN_MULT_UPTO` | printf: `%d`, the highest multiplier this GPU and driver allow |
| `UI_MENU_FRAMEGEN_MULT_OLD_DLL` | the frame generation helper library (`nx2navi_dlssg.dll`) is older than multi frame generation |
| `UI_MENU_FRAMEGEN_DESC_ANY`, `UI_MENU_FRAMEGEN_DESC_NGX`, `UI_MENU_FRAMEGEN_NOTE_NGX` | frame generation with any Upscaling choice, an upscaler or none (`runtime/vk/vk_dlaa.c` `fg_only_frame()`); through NGX (every build but the Windows one) NVIDIA's needs DLSS upscaling on at the start, its note and description there |
| `UI_MENU_TEX_CLOBBER`, `UI_MENU_TEX_CLOBBER_DESC`, `UI_MENU_TEX_CLOBBER_NOTE` | the Screen tab's *Fix overwritten textures* row (`tab_screen()`, `NX2SARIA_TEX_CLOBBER`) and its subtitle |
| `UI_MENU_PRESET` and the ids after it | the quality preset (`runtime/ui/menu.cpp` `row_preset()`, the `cfg_preset_*` functions of `runtime/cfg.h`): the row, its choices (`UI_MENU_PRESET_RECOMMENDED`: `%s` the tier this PC gets; `UI_MENU_PRESET_CUSTOM`: `%s` a choice's name), its notes (`UI_MENU_PRESET_CUSTOM_NOTE`: `%d` keys set by hand; `UI_MENU_PRESET_REC_NOTE`: `%s` a tier), the one-time notice (`%s` a tier) and the box that lists the keys a choice hands back to the preset |
| `UI_MENU_SCENE_RES`, `UI_MENU_SCENE_RES_DESC`, `UI_MENU_SCENE_RES_GAME`, `UI_MENU_SCENE_RES_NATIVE`, `UI_MENU_SCENE_RES_PRESET` | the Scene resolution row (`runtime/ui/menu.cpp` `row_scene_res()`, `NX2SARIA_SCENE_SHARE`): `%s` a share ("75 %") marked as the game's own, the native size or the preset's |
| `UI_MENU_DYN_RES`, `UI_MENU_DYN_RES_DESC`, `UI_MENU_DYN_RES_MIN`, `UI_MENU_DYN_RES_MIN_DESC`, `UI_MENU_DYN_RES_MIN_OFF`, `UI_MENU_DYN_RES_NO_ROOM`, `UI_MENU_DYN_RES_NATIVE` | the Dynamic resolution rows (`runtime/ui/menu.cpp` `row_dyn_res()`, `NX2SARIA_DYN_RES` and `NX2SARIA_DYN_RES_MIN`): `UI_MENU_DYN_RES_MIN_OFF` is the note of the minimum's row while dynamic resolution is off |

### `ui_text()`

UTF-8 text for an id in the language of `ov_language()`; English when the catalog has no row for that language.

### `ui_text_in()`

The same for an explicit catalog language (`en`, `ja`, `fr`, `de`, `it`, `es`, `nl`, `pt`, `ru`, `zh-Hans`, `zh-Hant`,
`ko`, `pt-BR`); an empty entry gives the English text.

### `ui_text_visit()`

Calls `visit(ctx, text)` for every string the row of `lang` has its own text for (an id that falls back to English is
not visited): the font builder covers the glyphs of the text a language shows (`runtime/ui/menu.cpp`).

### `ui_text_defined()`

1 when the row of `lang` has its own text for the id (no fallback to English), for the probe.

### `ui_text_lang()`

The catalog language for a Switch language code (`en-GB` gives `en`, `zh-CN` gives `zh-Hans`, and so on).

### `ui_text_game_langs()`

The game languages the F1 menu offers for a game build (`rt_game_version`): one entry per Mals language file the build
ships (`UiGameLang`), each with the Switch code `NX2SARIA_LANG` takes and the id of its name. 1.4.3 adds Brazilian
Portuguese (`USpt`). Fills `out` (room for `max` entries) and returns the count.

### `ui_text_game_lang_mals()`

The four-letter prefix of the message archive a game language reads (`USpt` for `pt-BR`), NULL for a language the
list does not have (`runtime/hle/romfs.c` `quit_menu_overlay()`).

### `ui_text_game_langs_in()`

The same list from the Mals files the RomFS ships (this game: Polish and Brazilian Portuguese, no Portugal
Portuguese): `regions` holds the four-letter prefix of each `Mals/<region>.Product...` file (`USen`, `EUpl`); the
version gates are not used. Returns the count.

### `menu_text()`

Menu text: `ui_text_in()` with the menu language. The menu language is `NX2SARIA_MENU_LANG` (`en`, `ru`, `pt-BR`,
`zh-Hans`); unset, the system UI language when the catalog has menu text for it (English, Russian, Brazilian
Portuguese, Simplified Chinese), else English. It follows a live change of the setting.

## `runtime/ui/ui_text.c`

The catalog of the text the host shows the player. One array per catalog language, indexed by `UiTextId`: a new string
gets an id in `runtime/ui/ui_text.h` and an entry in every language that has one (a missing entry falls back to
English). The software keyboard has every language; the menu has English, Russian, Brazilian Portuguese and
Simplified Chinese.

- `LANG_NAMES`: the language names are the same in every row.
- `SWKBD(title, prompt, name, ok, cancel, hint)`: `UI_SWKBD_TITLE`, `UI_SWKBD_PROMPT`, `UI_SWKBD_DEFAULT_NAME` (at
  most 9 UTF-16 units, no `@`, `%` or `\`), `UI_SWKBD_OK`, `UI_SWKBD_CANCEL`, `UI_SWKBD_PAD_HINT`.
- `DLSS_FILE` and `EXE_FILE`: the DLSS model file and the executable, as the player sees them on this platform
  (`nvngx_dlss.dll` and `nx2saria.exe` on Windows, `libnvidia-ngx-dlss.so` and `nx2saria` elsewhere).
- `text_zh_hans`: the Simplified Chinese row covers the software keyboard and the whole menu, so the menu and the
  overlay read Chinese when the menu language is `zh-Hans` (`menu_text()`). The rows are fixed width, so the strings
  stay close to the English length; an id without a Chinese entry falls back to English.

What each id's text is for is in the table under `runtime/ui/ui_text.h` above.

### `ui_text_game_langs()`

`all_langs[]` is every game language with the Mals file it reads: `EUen`/`USen`, `USfr`/`EUfr`, `USes`/`EUes` ship in
every version; `USpt` only from 1.4.3; `EUpl` only in this game (its `since` is 999: never by version,
`ui_text_game_langs_in()` finds it by its file).

### `ui_text_visit()`

A language's own strings, for the font that draws them: the merged system font must cover the whole menu of the
language, not only its name (`runtime/ui/menu.cpp` `fonts_up()`).

For Simplified Chinese it also hands over `file_name_glyphs_zh_hans`, a list of 124 characters that is data, not menu
text: 121 Han characters and `7`, `|`, `×`. They are the characters that only strings of removed menu rows held. The
font atlas holds the glyphs of the strings it is given and nothing else, and the menu also draws names it does not
know in advance, the files of the amiibo folder among them (`runtime/ui/amiibo_page.cpp`); a name with one of these
characters drew before those rows left and would show `?` without the list. With it, the set of glyphs is the one the
catalog gave before. `tests/ui_text_ids_check.py` fails when a character leaves the list or the walk stops handing it
over. A name with a Han character outside the catalog and the list shows `?`, as it always did.

### `resolve_menu_lang()`

The menu language, re-read when `cfg_version` changes. `NX2SARIA_MENU_LANG` names a catalog language (`en`, `ru`,
`pt-BR`, `zh-Hans`); unset, it follows the system UI language, through the same code map the game language uses
(`zh-CN` gives `zh-Hans`). Windows names a region with it (`ru-RU`), so a tag without menu text tries its language
alone (`ru`). A language whose row has no menu text (only the software keyboard does) shows English, so other Windows
locales keep the English menu. Its static buffer is safe because `menu_text()` calls this on one thread at a time, on
a `cfg_version` change.

### `ui_text_count()`

A counted noun in a language's own plural form. Its catalog text holds the forms with `|` between them, each a printf
format with one `%d`: `"%d code cheat|%d code cheats"`. `ui_text_plural_form()` picks one, and a text with fewer forms
than the rule asks for gives its last one, so Simplified Chinese writes a single form. A language without its own text
for the id shows the English text by the English rule. `menu_count()` is the same in the menu language. The menu had
no plural texts before these; a new counted noun gets an id of its own and its forms in each menu language.

### `ui_text_plural_form()`

- Russian, three forms: 0 for a count ending in 1 but not in 11 (1, 21, 101), 1 for one ending in 2 to 4 but not in 12
  to 14 (2, 3, 4, 22), 2 for the rest (0, 5 to 20, 11 to 14).
- Every other language, two forms: 0 for 1, else 1. Brazilian Portuguese writes 0 with the plural here (`0 cheats`).
