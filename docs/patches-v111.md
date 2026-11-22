# Optional fixes for version 1.1.1

## Gameplay field of view

The display menu's **Field of view** setting and the `fov` INI key set the vertical angle to 70–140% of the game's angle. The default, 100, disables the adjustment. Set `fov_patch=0` to disable the 1.1.1 patch for any value of `fov`. The horizontal angle follows the aspect ratio. The adjustment applies to the gameplay view of the player camera. Cutscenes, pause and map cameras, and first-person aiming keep the game's angle.

Look for `fov: on, 120%` at startup and `fov: gameplay lens adjusted` with the before and after angles in degrees on the first adjusted frame. Set `fov=100` to see `fov: off, 100%`.

## Epona's jump at high frame rates

At a frame rate above 60 FPS, a shorter update can send the jump down the wrong blend path when approaching a fence. The patch uses the game's frame step as the threshold for a normal update, while keeping the slow-motion path for a step shorter than that. Set `horse_jump_fix=0` and restart to disable it.

The startup `patches:` line lists `ohorse on` or `ohorse off`. A changed jump logs `frame rate: horse jump blend` with the update step and threshold, up to four times per session.

## Gohma's ceiling movement at high frame rates

Gohma's steering update uses bounds based on the turn it applied this frame. Because that turn is already scaled by the frame step, a second multiplication by the step can choke the turn rate as Gohma approaches the point on the ceiling. The patch converts the applied turn back to a rate before using it as a bound. Set `gohma_turn_fix=0` and restart to disable it.

The startup `patches:` line lists `ogohma on` or `ogohma off`. An adjusted update logs `frame rate: Gohma steering clamp` with the step, turn and rate bounds, up to four times per session.

## Frozen Zoras at high frame rates

A looping animation with the range `[0, 1]` holds a single pose. On a shorter update its clock now keeps the fractional step until it has a whole one, and its pose pass resolves to the first frame of the verified loop. Set `frozen_zora_fix=0` and restart to disable it.

The startup `patches:` line lists `ozora on` or `ozora off`. The patch logs `frame rate: single-frame loop clock` and `frame rate: single-frame loop pose` with the step or phase before and after the adjustment, up to four times each per session.

## Battle music at high frame rates

With `sound_steps=1`, the sound job still runs at whole 60 Hz steps. The patch holds the battle music's four change bits across skipped updates. The next sound update receives them through the original flag reads, and they expire after that update. Set `battle_music_fix=0` and restart to disable it.

The startup `patches:` line lists `obgm on` or `obgm off`. A restored bit logs `frame rate: battle music change` with the flag byte before and after, up to eight times per session.

## Voice language after restart

The voice resource manager can miss a saved language choice when the game starts. This patch compares the option it reads with the last valid value and requests the game's normal voice refresh when it changed without a write notification. Set `voice_language_fix=0` and restart to disable it.

The startup `patches:` line lists `voice_language on` or `voice_language off`. A refresh logs `voice language: refresh` with the old and new option values, up to eight times per session.

## Speech-balloon anchors

When the wide HUD squeezes the interface into the middle 16:9, this patch applies the inverse horizontal scale to each speech-balloon anchor so it remains over its speaker. Set `balloon_anchor_fix=0` and restart to disable it.

The startup `patches:` line lists `wide_balloon on` or `wide_balloon off`. It logs the UI horizontal scale at startup and the first adjusted anchor's x coordinate before and after.


## The neighbouring map the game preloads

Where two maps' load radii overlap, such as the Market's square, walking and turning around made the game's pick of the neighbouring map to preload flip between them, and each flip cancelled the map that was loading and started the other. The patch holds the map being preloaded until the game's pick has left it for 2 seconds. Set `changebank_hold=0` and restart to disable it; `changebank_hold_ms` sets the time (`0` gives the game's pick at every update and only counts it). See `docs/changebank-hold.md`.

Look for `changebank hold: on, 2000 ms` at startup. A held pick that went back logs `changebank hold: kept <map> preloading while the game chose <other> for N ms`, up to eight times per session, and a minute line counts the game's flips and the held picks.
