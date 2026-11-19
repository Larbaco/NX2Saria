# Optional fixes for version 1.1.1

## Gameplay field of view

The display menu's **Field of view** setting and the `fov` INI key set the vertical angle to 70–140% of the game's angle. The default, 100, disables the adjustment. Set `fov_patch=0` to disable the 1.1.1 patch for any value of `fov`. The horizontal angle follows the aspect ratio. The adjustment applies to the gameplay view of the player camera. Cutscenes, pause and map cameras, and first-person aiming keep the game's angle.

Look for `fov: on, 120%` at startup and `fov: gameplay lens adjusted` with the before and after angles in degrees on the first adjusted frame. Set `fov=100` to see `fov: off, 100%`.
