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
