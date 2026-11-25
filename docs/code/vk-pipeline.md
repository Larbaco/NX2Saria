# runtime/vk: pipelines, their caches and the shader modules

This page holds the notes of the pipeline files of `runtime/vk/` ([README.md](README.md#the-pages-of-runtimevk) lists
them). Only the part below has been written here so far: the other notes of these files are still comments in them.

## `runtime/vk/vk_mesh_path.h`

How the game's task and mesh programs are drawn, decided once when the device is made (`runtime/vk/vk_device.c`
`create_device()`). [architecture.md](../architecture.md) describes the mesh fallback itself.

### `mesh_radv_task_in()`, `mesh_radv_task_regression()`

Whether a RADV release is one whose task shaders lose the device.

- **The fault.** Mesa 26.2.0 changed where its shader compiler waits for a shader's memory writes, and a task shader
  since then can mark its record as ready before the payload and the mesh workgroup counts are written. The mesh stage
  then reads what is not there yet and the GPU stops answering: the kernel resets it and the game ends with "the GPU
  device was lost" within seconds of the first mesh draws. A session report shows it on an RX 6800S with Mesa 26.2.7
  after 17 seconds. Mesa's own tracker has it on RX 6000 cards and on the integrated GPUs of the same generation.
- **The range.** `mesh_radv_task_series` has one row for each release series with the fault, and the patch number of
  the first release of that series with the fix, or `MESH_RADV_NO_FIX` while none is out. Today both rows are open:
  26.2.7 and 26.3.1 are the newest releases and neither has the fix, which is on Mesa's main branch since 2026-11-19 and
  queued for both series. A row gets its number only from a published release that was checked to hold the fix; until
  then every later release of the series is drawn for through the fallback too, the fixed one included.
- **Outside it.** Every release before 26.2.0, and every version that is not of a listed series: a later series
  branches from a main branch that has the fix. A snapshot of the main branch says 27.0.0, and some packagers number
  theirs 26.99.99; both are outside. A snapshot built between 2026-10-16, when the main branch became 27.0.0, and the
  fix is not told apart from a fixed one and draws natively: `mesh_fallback = 1` is what is left for it.
- **Which cards.** Every RADV device with mesh shaders in those releases, not only the generation the reports are from.
  The fault is in the code every task shader goes through, with no branch on the hardware generation, so nothing in it
  says that newer cards are safe; the reports name RX 6000 cards only.
- **The version.** `driverVersion` of the device's properties, which Mesa fills as major, minor and patch in the layout
  of `VK_MAKE_VERSION`. The driver is told by its `VkDriverId` (`MESH_DRIVER_RADV` is `VK_DRIVER_ID_MESA_RADV`): another
  driver with the same number is not in the range. `mesh_radv_task_in()` takes the table as an argument so that the
  test can give it a series with a fixed release.

### `mesh_path_decide()`

`radv_task_bug` is `mesh_radv_task_regression()`'s answer. It is read after the settings and after the cases that hold
on any driver, so `mesh_fallback = 1` and `mesh_fallback = 0` act as on every device, and a card without mesh shaders
is drawn for as before. With `mesh_fallback = auto` it takes the fallback, and `mesh_path_fallback_default()` counts it
as the device's default.

The log says what was read and what followed from it (`create_device()`, on RADV with mesh shaders only):

- `vk: mesh path: RADV <version> is inside|outside the releases whose task shaders lose the device (...): mesh_fallback
  = auto takes the mesh fallback|draws mesh programs natively`. Inside, it also says whether the series has a fixed
  release.
- `vk: mesh path: WARNING: mesh_fallback = 0 draws mesh programs natively on this RADV release all the same: the device
  can be lost within seconds`, when the player's setting forces the native path inside the range.
- The `vk: mesh fallback:` line that follows gives the reason as for the other defaults.

`tests/mesh_path_check.py` checks the range release by release and the decision.
