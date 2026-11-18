# Gyro

How the runtime turns a controller's gyroscope and accelerometer into the Switch's six-axis sensor state, what the
*Gyro* settings do, why their defaults are what they are, and which log lines show it working. The player's summary is
in [Controls](controls.md#gyro).

## The path

- The game calls `GetSixAxisSensorHandles` and `StartSixAxisSensor` at boot and reads 16 states every frame
  (`runtime/ov_input.c` `six_fill`). Each state holds the acceleration in G, the angular velocity in revolutions a
  second, the angle (the angular velocity summed) and the direction (the controller's axes in a frame whose z is up).
- `runtime/input.c` `motion_main` runs every 4 ms once the game started the sensor. It reads the newest gyro and
  accelerometer sample of the gamepad the game reads, turns it into the Switch controller's axes
  (`input_motion_axes`) and feeds it to `runtime/motion_fusion.c` `motion_feed`. A gamepad needs both sensors:
  SDL's plain pair, or a Joy-Con half's (`sensor_pairs`). SDL 3.4 gives a combined Joy-Con pair the right Joy-Con's
  sensors as the plain pair, so a pair aims with the right Joy-Con.
- `motion_options_refresh` reads the `gyro_*` settings whenever the settings change (`cfg_version`) and hands them to
  the fusion (`motion_set_options`). The game's own `SetGyroscopeZeroDriftMode` is logged and otherwise left to the
  `gyro_deadzone` setting (`ov_input.c` `ov_pub_zero_drift`).

## What each sample goes through (`motion_feed`)

1. **Rest statistics.** Running means over 0.2 s of the raw rate, of the acceleration and of its length, and of both
   readings' spread. The controller is still while the gyro's spread is under 0.015 rad/s and the accelerometer's
   under 0.0173 G (summed over the axes), the mean acceleration is within 0.1 G of 1 G and the mean rate is under
   0.15 rad/s (8.6°/s: no bias is that large, so a steady spin is motion). Once a bias has been learned for 2 s, the
   mean must also be within 0.035 rad/s (2°/s) of it, because a bias moves slowly; if the controller stays still in
   every other way for 3 s away from it, the learned bias was wrong (a smooth slow turn taken for rest) and learning
   starts over.
2. **Bias.** After 0.5 s still, the mean rate is the gyro's bias: learned with a 0.25 s time constant for the first
   2 s of learning, 3 s after. It is subtracted from every sample while `gyro_calibration` is on. It is learned again
   each time the controller lies still. Each controller (by its SDL id) keeps its own; the last four are remembered,
   so switching between two pads keeps both. A reconnected controller gets a new id and learns again.
3. **Dead zone.** A corrected rate shorter than the level counts as none, for the reported rate and the orientation
   alike: the console's zero-drift levels, tight 0.002, standard 0.007 and loose 0.01 revolutions a second.
4. **Orientation.** The rate is integrated (rotation matrix, exponential map) and, while the controller is not being
   accelerated (|a| within 15 % of 1 G), pulled toward the measured gravity by a proportional term (`MOTION_KP`
   0.5/s, about 2 s, as before) and an integral term (`MOTION_KI` 0.1/s², each component limited to 0.1 rad/s). The
   integral takes up a bias the gravity can see while the controller is held and never still; it is cleared while
   the bias is being learned, so the two never correct the same error twice. Yaw has no reference and follows the
   rate.
5. **Turn and tilt.** The rate is split into its part about the world's up (the turn: left and right aim) and the
   rest (the tilt: up and down aim), each multiplied by its factor (`gyro_turn_sens`, `gyro_tilt_sens`, negative when
   inverted). The reported direction is built from the filter's orientation every sample: its heading changes are
   multiplied by the turn factor and its tilt away from level by the tilt factor, so it stays tied to the filter and
   comes back level when the controller does. At 100 % and not inverted the state is the filter's own, bit for bit.

## Defaults and why

- **Calibration on.** A still controller with a bias turns the aim at the bias's rate: a Pro Controller's 0.3°/s is
  18° a minute. Learning only happens while the controller is still to sensor-noise level, and a controller held in a
  hand never is, so aiming is not touched. The known limit: a perfectly smooth, slow turn (a few degrees a second)
  with the controller resting on something is indistinguishable from rest and can be taken for a bias; the next real
  rest replaces it (step 1).
- **Dead zone low (0.002 rev/s, 0.72°/s), not the console's standard.** With the bias taken off, what is left of a
  still controller is sensor noise of about 0.1°/s, which the low level zeroes. Standard (2.5°/s) would also swallow
  slow aiming corrections, which matter when a target is far. Standard and high are there for a controller whose
  noise is larger. Before this change there was no dead zone: the default zeroes only rates that are a drift.
- **Integral gain 0.1/s² with the proportional gain kept at 0.5/s.** The pair is damped (damping ratio 0.8), so the
  tilt settles without overshoot; the gravity pull is the same as before.
- **100 % and not inverted.** The controller's own rate, as before.

With every gyro setting at its default, the game gets the same state as before the change except that a learned bias
is subtracted and rates under 0.72°/s are zeroed.

## Log lines (`logs/nx2saria.log`)

At start and when the settings change (32 lines at most):

    motion: settings: calibration on, dead zone low (0.002 rev/s, 0.72 deg/s), turn 100%, tilt 100%; filter kp 0.50 ki 0.10
    hid: the game sets the gyroscope zero-drift mode standard (1) on handle 0x20003; the gyro dead zone setting decides instead (gyro_deadzone = low)

When a controller with sensors is taken (the Joy-Con fallback names the half):

    input: motion sensors on "Nintendo Switch Pro Controller" (gyro 200 Hz, accelerometer 200 Hz)
    motion: sensors of "Nintendo Switch Pro Controller": no bias learned yet
    motion: sensors of "DualSense Wireless Controller": bias +0.12/-0.40/+0.05 deg/s restored (learned 2310 ms)

During play:

    motion: bias learned +0.31/-0.12/+0.05 deg/s after 1500 ms still ("Nintendo Switch Pro Controller", calibration 1)
    motion: no still moment yet on "...": set the controller down for a second so its gyro bias is learned
    motion: bias relearned from scratch: still for 3000 ms away from the learned bias ("...")
    motion: last 300 s: still 41.2 s in 3 periods, drift while still 0.34 deg/s raw -> 0.00 deg/s reported, turn drift 12.0 deg/min raw -> 0.0 reported, bias learned 3 times, dead zone held 2.1 % of moving samples

The bias line comes once per still period that learned (24 a session). The numbers are the bias on the Switch
controller's x, y and z axes, and the count of still periods that learned for this controller. The aggregate comes
after the first minute of samples and every 5 minutes after (48 a session): *drift while still* is the mean length of
the rate while the controller was still, raw (what the game got before this change) and reported (what it gets now);
*turn drift* is how far that rate would turn the aim sideways in a minute; *dead zone held* is the share of moving
samples the dead zone zeroed.

## Checks

`python tests/motion_fusion_check.py` builds `motion_fusion.c` with stand-ins for the log and runs synthetic
sequences: a still controller with a bias (learned, then zero rate and no drift), biases of 3 and 6°/s, each dead
zone level just under and just over, a quarter turn about up and a quarter pitch, hand tremor (never still), a new
bias after a held phase (learned again), the per-controller cache, the turn and tilt factors (doubling, halving,
inverting, closed loops coming back level, 1/1 exact), the integral term against a bias the gravity can see, a held
then still controller whose heading holds, a smooth slow turn taken for rest and corrected by the next rest,
non-finite samples, the aggregate line, and `ov_probe.c` `probe_feedback`'s quarter turn and restart.

A test run of the game can give the virtual test gamepad a gyro: `NX2SARIA_TEST_PAD=0,0,0,0
NX2SARIA_TEST_PAD_GYRO=0.6,-0.3,0.2` (deg/s, SDL's axes) makes it lie face up with that bias, 4 s still then 2 s
"held", repeating, so the log shows `input: test gamepad gyro: ...`, the sensors line, `bias learned` after each
still phase and the aggregate.

## Trying it with a controller

1. Start the game with the controller connected. In `logs/nx2saria.log` look for `motion: settings:`, `hid: the game
   sets the gyroscope zero-drift mode`, and `input: motion sensors on "<your controller>"`.
2. Put the controller down on a table for 3 seconds. Look for `motion: bias learned ... after 1500 ms still`.
3. Take the slingshot or the bow, aim, and put the controller down on the table while aiming for 10 seconds: the
   aim must not move. Then pick it up and aim at something small far away with slow movements: it should follow.
4. On the Controls tab, *Gyro* group: turn sensitivity 200 % (left and right aim twice as fast), invert turn,
   invert tilt, dead zone High (the slowest movements no longer move the aim). Each change shows a
   `motion: settings:` line.
5. With a Joy-Con pair and a DualSense, repeat steps 1 and 2. Switch between two controllers: `motion: sensors of
   "...": bias ... restored` when one comes back.
6. After playing 5 minutes, the `motion: last 300 s:` line shows the drift before and after.
