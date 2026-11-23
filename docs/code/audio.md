# runtime/audio

The mixing stage of the audio renderer and the audio services around it, the effects' signal processing and the
microphone. The renderer itself (the game's audren service, voices, the render thread) is `runtime/hle/audren.c`. The
player's settings: the output device in [controls.md](../controls.md), the microphone in
[audio-input.md](../audio-input.md), the checks in [building.md](../building.md#checks).

## `runtime/audio/audio_mixer.c`

The mixing stage (voice decode and resampling, splitters, mixes, effects, sinks) and the IAudioDevice service, with
SDL3 audio output.

Per 5 ms frame the render thread (`runtime/hle/audren.c`, renderer lock held) decodes every started voice (PCM8/16/24/32,
float, DSP-ADPCM) along the sample path the voice clock consumes, resamples it linearly, runs the voice biquads, scales
it into its destination mix (a volume the game changed is reached in equal steps across the frame's samples,
`accumulate_ramp`), folds the submixes, runs the mix effects and writes the device sink's mix (stereo, or 5.1 with
`NX2SARIA_AUDIO_CHANNELS`, `output_channels`) into a single-producer single-consumer ring. The SDL audio thread drains
that ring without taking any lock: an empty ring plays silence and never blocks the render thread or the guest.

- `NX2SARIA_AUDIO=none` (or `NX2SARIA_AUDIO_OFF`) mixes without a device.
- `NX2SARIA_AUDIO_DEVICE` picks the playback device (`resolve_audio_device`).
- `NX2SARIA_AUDIO_PREFILL_MS` (0 to 500, default 20, 60 on Linux) is the queued audio the output waits for after start
  or underrun.

The device's clock paces the render thread, as the audio hardware paces the Switch's DSP (`audio_mixer_pace_ns`): a
frame is rendered when the ring holds less than the output target, max(prefill, two device periods), so the queue and
the delay from the game to the speakers stay there and the host and device clocks cannot drift apart. When the render
thread is held up the ring runs out and the device plays silence; the frames that silence stands for are then rendered
and dropped (`audio_mixer_owed`, `NX2SARIA_AUDIO_CATCHUP`), so the sound keeps its place against the picture. Without a
device, while the renderer is stopped and while the device takes nothing (a stall), the render thread keeps real time on
its own timer; what piled up meanwhile is skipped on the SDL thread once the device resumes (more than `QUEUE_SLACK_MS`
above the target plus the device's request), which trims the ring after a device stall.

Constants:

- `RING_FRAMES` 16384 output frames, about 340 ms at 48 kHz.
- `OUT_MAX` 6: output channels at most, 5.1.
- `QUEUE_SLACK_MS` 40: queued audio above the target plus the device's request that `ring_pull` skips.
- `STALL_MS` 50: the device took nothing this long (at least 4 periods), so the timer paces.
- `dm_guest_write_prepare` does nothing: guest memory is plain host memory on Windows.
- `AUD_R_NOT_SUPPORTED` 0x40299 is audio module 153, description 513; `AUD_R_INVALID_ARGUMENT` 0x70899 is module 153,
  description 900.

Types:

- `SplitDest`, for the volume ramps (`audio_mixer_render`): `prev` holds the volumes the destination had in the render
  pass before, `prev_dest` the mix it went to and `prev_buffers` how many of that mix's buffers; `prev_ok` is 0 when that
  pass did not use it.
- `MixPrev`, for the volume ramps: a mix in the render pass before, its volume, the splitter it went to, its buffer
  count, whether in use.
- `EffectState.bad`: the game's parameters for the effect cannot be used, so its sound is passed through
  (`audio_mixer_update_effects`).
- `Mixer`:
  - `offset`: the first mix buffer of each mix id.
  - `device`: the explicitly chosen device, 0 while the system default is open; `device_name` its name, for the log when
    it goes away; `watch_ns` (render thread) when `device_check` last looked.
  - `channels`: output channels, 2 or 6 (`output_channels`).
  - `ready`: the SDL callback's own state; `gap` (the callback's own): an underrun's silence has not ended yet;
    `silent_frames`: the silence played in such gaps so far (`audio_mixer_owed`); `made_up` and `owed_epoch` (render
    thread): how much of it is caught up or forgotten, and the `stop_epoch` that count belongs to.
  - `target`, `period`: the output target and the device period, in frames (`output_open`).
  - `pulled_ns`: `kern_now_ns` of the device's last request, 0 before the first; `pull_gap_max`
    (`NX2SARIA_AUDIO_STATS`): the longest gap between two requests, in ns.
  - `reported`: bits of the one-time reports.
  - `input_sum` (`NX2SARIA_TRACE=audio`): the device sink's inputs, squared and summed.
  - `prefill`: in render blocks.
  - `ramp` (`NX2SARIA_AUDIO_RAMP`): volumes ramp across a frame (`accumulate_ramp`); `snap`: the next pass has no pass
    before it to ramp from (open, start); `mix_prev`: each mix in the pass before; `ramps`, `ramp_passes`: ramps made
    and render passes with at least one (`audio_mixer_ramp_stats`).
  - `trace_pass`, `trace_seen`, `trace_buf`, `trace_pair`, `trace_shared` (`NX2SARIA_TRACE=audio`): the render passes;
    the pass each voice was last mixed in and the wave buffer address it read in that pass; the last shared-buffer pair
    logged for each voice (other voice, address); the "voices share a buffer" lines written (capped).

### `host_volume()`

`NX2SARIA_VOLUME`: the player's volume in percent (0 to 100) on top of the game's own; a live setting (the menu),
parsed again when `cfg_version` changes. Render thread only.

### `guest_range_ok()`

A guest range that `runtime/hle/ipc.c` `ipc_guest()` takes (it ends the process on any other): not 0, not wrapping,
below the guest address limit. It does not prove the memory is mapped.

### `prefill_blocks_for()`

The output: an SDL3 audio stream fed from the ring. How much audio the output waits for before it starts, and again
after an underrun: `NX2SARIA_AUDIO_PREFILL_MS` rounded up to whole 5 ms render blocks; -1 for a value outside 0 to
500 ms. The Linux default is higher: at 20 ms PipeWire's queue (max(prefill, two 1024-sample periods)) underran
thousands of times within minutes in busy scenes, and 60 ms removed the underruns.

### `playback_devices()`

The playback device choice (`NX2SARIA_AUDIO_DEVICE`). SDL's playback device ids sorted by name, ties by id
(`device_order`): the order `--list-audio-devices` prints them and the numbering of the index form. SDL's own order is
not stable: it differed between two enumerations in one process, so an index into it would pick another device from one
start to the next. The caller frees the result with `SDL_free`; SDL audio must be initialised. `*count` is 0 when SDL
reports none.

### `describe_device()`

One line describing device `id` ("name, 2 ch, 48000 Hz"), without a leading index.

### `resolve_audio_device()`

`NX2SARIA_AUDIO_DEVICE` `want` to the SDL device to open, with `how` saying which rule decided. `default` (or empty)
gives `SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK`, which SDL keeps pointed at the system default when Windows changes it. An
all-digit value is a 1-based index into `playback_devices`; more digits than a `long` holds is out of range either way.
Any other text is a case-insensitive substring of `SDL_GetAudioDeviceName`, and the first device in list order that
contains it wins. A value that matches nothing also gives the default device (the game is never silent because of this
setting); `say` then logs the playback devices. SDL audio must be initialised.

### `list_audio_devices()`

`--list-audio-devices` (`runtime/main.c`): each playback device with its index, name, channels and frequency, the one the
system default points at, then what `NX2SARIA_AUDIO_DEVICE` would pick. Needs no game files. Returns the exit code.

### `output_channels()`

`NX2SARIA_AUDIO_CHANNELS`: the output channels, decided at the first call and again when the setting changes
(`audio_channels_live`, which clears `decided`). `stereo` gives 2; `5.1` gives 6 (FL FR C LFE RL RR, the device sink's
order and SDL's); `auto` gives 6 when the chosen playback device (`NX2SARIA_AUDIO_DEVICE`; the system default unless
set) has at least six channels (a 5.1 or 7.1 speaker setup in Windows; SDL places 5.1 on 7.1) and 2 otherwise.
IAudioDevice reports the same count to the game (`GetActiveChannelCount`), which picks its sink layout from it.
`NX2SARIA_AUDIO=none`: 2, without touching SDL. The device it asks for is the choice `output_open` makes again (only
that one logs the playback devices when nothing matches).

### `ring_pull()`

Fills `frames` output frames from the ring (zeros for what is missing). Lock-free; runs on SDL's audio thread.

- When the renderer stopped (a new stop epoch), what was queued before the stop is dropped.
- The prefill waited for is no more than the target the renderer fills the ring to (`output_open` caps it at half the
  ring): a longer prefill (an `audio_prefill_ms` of 180 and up) was never reached, and the output stayed silent for
  good.
- An underrun's silence goes on until the prefill is queued again, and it counts as owed (`audio_mixer_owed`); the wait
  for the first prefill after a start is not such a gap.
- What piled up while the device stalled is skipped, the oldest first, keeping the target plus the request.

### `sdl_pull()`

The SDL stream's callback. The longest gap between two requests is a plain store: a lost race only loses a sample. The
settings menu's sounds are mixed in here (`runtime/ui/menu_sound.c`).

### `stream_open()`

Opens an SDL stream on `dev` and starts it. Returns 0 with the reason in `err` otherwise (no stream is left behind).

### `output_open()`

When the chosen device would not open, the output opens the default device, so the game still has sound.

### `output_timing()`

After `stream_open`: the device's period, the output target that follows from it, and the stall limit. Logs the device.

### `channels_switch()`

Render thread (`audio_mixer_render`): the output takes another channel count after `NX2SARIA_AUDIO_CHANNELS` changed
(`audio_channels_live`). The stream is made again with the new count on the device it was on: destroying it waits for
the SDL callback, the only other reader of the ring and of `m->channels`, and what was queued in the old layout is
dropped. The device sink's log line is written again, with the new output (report bit 6). Mixing without a device, there
is no stream to make.

### `device_check()`

Render thread, from `audio_mixer_pace_ns` while the device takes nothing. An explicitly chosen device
(`NX2SARIA_AUDIO_DEVICE`) can disappear (a virtual sink removed, a USB headset unplugged); SDL keeps only the system
default device alive across that, so the stream of a chosen one just stops being pulled, and nothing in this file polls
SDL's event queue for the removal. This looks once a second whether SDL still knows the device and, if not, moves the
output to the system default; if that fails too, the mixer goes on without a device, as with `NX2SARIA_AUDIO=none`.
Destroying the stream waits for the SDL callback (`sdl_pull`), which takes no lock the render thread holds.

### `audio_mixer_pace_ns()`

The wait it returns is at least 0.5 ms: until the device has taken the ring below the target.

### `audio_mixer_owed()`

Catch-up after a stall (`NX2SARIA_AUDIO_CATCHUP`, `runtime/hle/audren.c` `render_thread()`), for the frames the device
paces. While the render thread is held up the device goes on taking periods: the ring runs out and `ring_pull` plays
silence, first for the rest of that request (the underrun) and then until the prefill is queued again. The device's
clock went on by that silence and the game's sound did not, and `audio_mixer_pace_ns` only fills the ring to its target
again, so everything after the gap would be heard that much later than the picture it belongs to, and every further gap
would add to it.

This says whether a whole render frame of such silence is still owed and takes it off the debt; the caller renders that
frame with `drop` set (`audio_mixer_render`), which moves the game's sound on and queues nothing, so the ring is filled
to the same target afterwards. The debt is counted in the device's own samples, not in host time: the two clocks cannot
drift into it, and a gap of less than a frame waits for the next one. Whatever was owed when the renderer stopped is
forgotten, and so is a debt of more than `AUDIO_CATCHUP_MAX_MS`, with -1 returned: after that long the sound goes on from
now.

### `output_push()`

Render thread only. With a device that stalled (the frame does not fit in the ring) the frame is dropped and counted as
an overrun. Without a device the ring only keeps the latest frames.

### `sink_dump()`

`NX2SARIA_AUDIO_DUMP=<path>`: the sink mix before the clamp as raw interleaved float32 stereo, and every 5 s one log
line with how many frames were rendered less than 1 ms after the one before (a burst refill). File writes on the render
thread: for diagnosis only.

### `level_note()`

Level meter (`NX2SARIA_TRACE=audio`): one line per 5 s of output, and the first nonzero frame.

### `audio_mixer_set_running()`

On a stop the next pass snaps (`snap`): what was mixed before the stop is not the sample before the next pass's first.

### `audio_mixer_close()`

Destroying the stream closes the device and waits for the callback to finish.

### `splitters_walk()`

Routing input: splitters and mixes. The splitter section of an update: the SNDH header with the record counts, one SNDI
record per splitter the update changes with its destination ids and 12 reserved bytes (measured, REV11), then one SNDD
record per destination it changes (0x70, 0x90 or 0xa8 bytes by revision); the section is padded to 16 bytes, and the
game leaves the padding as it is (measured), so the padding is not checked. One walk serves both passes of an update,
so both read the same bytes.

- With `apply` 0 (`audio_mixer_routing_check`: before anything of the update is applied, without the lock) it checks
  the framing only and returns `AUDREN_R_INVALID_UPDATE` on a fault, which refuses the update.
- With `apply` 1 (`audio_mixer_update_routing`, lock held) it stores the records: a record that names a splitter or
  destination the renderer does not have is left out, a destination id in a splitter's list that the renderer does not
  have is stored as -1, which both splitter loops of `audio_mixer_render` skip, and a destination filter with a
  coefficient that is not a finite number is off (`audio_bad` each); its state is cleared, as for a filter the game
  turned off. Revision 12 and later send Q14 filters.

Offsets are 64-bit, so a huge id count cannot wrap a bounds test.

A framing fault (`SPL_FAULT`): with `apply` 0 the update is refused (`AUDIO_REFUSE`). With `apply` 1 it can only come
from the game rewriting its input during its own RequestUpdate, between the two walks; the update is taken by then, so
the walk stops there, the records before it stay applied, the mix section is applied after it, and the log says the
rest of the splitter section is left out (`audio_mixer_update_routing` ignores the result).

### `audio_mixer_routing_check()`

The mix section in the MixInParameterDirtyOnly form: a 0x20-byte header {u32 magic 0xCAFEBABE, u32 count, pad}
followed by the dirty mixes.

### `audio_mixer_mix_layout_check()`

The mixes' buffers as `audio_mixer_render` lays them out: one after another, for the mixes in use, from buffer 0. A
negative count of a mix in use would start the mixes after it before buffer 0, and more buffers than the renderer has
would run past them, so such an update is refused before anything of it is applied; the render's layout check is then an
invariant. The update's records are applied as `audio_mixer_update_routing` applies them (a changed-mix record with an
id the renderer does not have is left out). This check and `audio_mixer_update_routing` each read the mix section from
guest memory, so the check holds for the apply only because the game does not rewrite its input during its own
RequestUpdate (its sending thread waits in the IPC call). At most 256 mixes: `runtime/hle/audren.c` `renderer_new()`
keeps `submix_count` below 256.

### `audio_mixer_update_routing()`

A mix section shorter than its 0x20-byte header was refused by `audio_mixer_routing_check`. In the dirty-only form only
the changed mixes come; `audio_mixer_routing_check` has checked the header against the section's size.

### `effect_bad()`

A delay, reverb or compressor block `audio_effect_invalid` refused, with its values: one line per kind.

### `audio_mixer_update_effects()`

Effect parameters arrive once per IPC update, while the DSP may process several frames. An enabled effect whose
parameters cannot be used is marked bad (`audio_bad` logs it): `audio_mixer_render` passes its sound through, with no
DSP, delay line or reverb state made from those values, until an update brings usable ones.

- A biquad with a channel count outside 1 to 6 is skipped by `mix_simple_effect`, which logs it.
- A limiter with channels outside 1 to 6 gets no delay line, and `mix_limiter` skips it and logs it. One whose longest
  delay is outside 1 to 192000 samples is bad, and checked again at the next update: without a delay line the block
  runs each time.
- The render creates or updates the 3D reverb from these values (`mix_effects`); one with another channel count is
  skipped there.

### `adpcm_next()`

Voice decoding along the voice clock's sample path (`Walk`). DSP-ADPCM: 8-byte frames of a predictor/scale byte and 14
4-bit samples; the history runs across samples.

### `sample_at()`

A buffer is (re)started from its context when it is a new one or the walk went back, else from the frame boundary with
the carried history.

### `walk_skip_empty()`

The same as `skip_empty` in the reference audio renderer: an empty buffer has no samples and is played on arrival.

### `walk_step()`

The same transitions as `advance_voice` in the reference audio renderer.

### `voice_step()`

Source samples per output sample. The guest's sample rate and pitch are not checked: an infinite or huge step would
keep the per-sample loops (`audio_voice_consumed`, `voice_render`) running for good under the renderer's lock, so it is
held to 64 source samples per output sample, far above any pitch the game asks for. NaN gives no progress (0).

### `audio_voice_consumed()`

Source samples the voice consumes over `frames` output samples, counted with the exact accumulation `voice_render` steps
by (float rounding makes `fraction + seconds * rate * pitch` differ from it by one sample on some frames, a seam click);
leaves the fractional position in `v->fraction`.

### `voice_render()`

Decodes `frames` output samples per channel into `out[c][k]`; the renderer advances the voice afterwards by the same
amount.

### `accumulate_ramp()`

Volume ramps (`NX2SARIA_AUDIO_RAMP`, default 1). The game sets volumes once per audio update, and a volume applied to a
whole frame as one constant makes the level jump at the frame's first sample whenever the game moves it (a fade, a pan,
a sound going past the camera): a step in the waveform, heard as a click. The console's renderer goes from the volume of
the frame before to the new one across the frame's samples, and so does this: `from` is the gain the same source went
into this buffer with in the render pass before, sample k gets from + k (to - from) / frames, and the next pass, which
starts at `to`, continues the line. Equal gains take `accumulate`'s constant path, and so does a gain that is not a
finite number (NaN also marks "no gain in the pass before").

### `dest_ramps()`

A splitter destination ramps when the pass before used it and sent it to the same mix: one that is new or was moved
starts at its new volumes.

### `mix_aux()`

Guest custom effects exchange int32 samples in two rings. Each ring has CPU and DSP 0x40-byte headers followed by
samples; only the DSP header's owned cursor is advanced.

The render-pass effects (`mix_aux`, `mix_limiter`, `mix_capture`, `mix_simple_effect`, `mix_effects`) skip an effect
whose dimensions, guest ring or mix buffer indices the game set out of range (nothing of it is read or written in that
pass) and log it once per kind through `audio_bad`; every index is checked before the first write.

### `mix_limiter()`

NaN-safe: an attack or release that is not a number would turn the mix, and every mix it feeds, into NaN, so such a
limiter passes its sound through. A limiter already bad was logged by `audio_mixer_update_effects` (the longest delay
that left it without a delay line) and is not logged again.

### `effect_counter_add()`

The SDK's SampleCounter::Add advances the producer counter itself, not the producer-consumer difference or the ring
cursor. It saturates on difference overflow.

### `mix_simple_effect()`

- A delay, reverb or compressor with a channel count out of range was logged at its update, by its parameters' kind, and
  is bad here.
- Nothing is written before every index is checked: the loop only takes pointers.
- A bad effect has parameters `audio_mixer_update_effects` could not use; it logged them and made no DSP, so its sound
  passes through.
- That every other enabled effect has its DSP is an invariant: `audio_mixer_update_effects` makes the DSP of every
  enabled effect with channels whose parameters pass, and marks the others bad.

### `mix_effects()`

The 3D reverb. With 6 channels from REV11 on, the channel resources use L, R, C, LFE, BL, BR, while the reference DSP
uses the legacy order, so the inputs and outputs are mapped. Nothing is written before every index is checked: the loop
only takes pointers. A bad reverb has values `audio_reverb_invalid` refused (`audio_mixer_update_effects` logged them),
and the reverb is neither made nor updated from them.

### `mix_circular_sink()`

A sink the game set out of range is skipped in this pass: nothing of it is written, and its position stays.

### `audio_mixer_render()`

- The mixes' layout fitting the buffers is an invariant: `audio_mixer_mix_layout_check` refuses an update that would
  leave the mixes so.
- A pass ramps from the pass before it. The first one after the mixer opened or the renderer started has none.
- Each sample format a started voice uses is logged once: one the mixer cannot decode plays as silence.
- With `NX2SARIA_TRACE=audio`, a voice mixed now and not in the last pass is logged with what it plays and where to; a
  pass is 5 ms at 48 kHz (frame samples per pass).
- The voice ramps when the pass before mixed it, with as many channels and into the same mix or splitter. One that has
  just started (`runtime/hle/audren.c` `voice_reset()`; or stopped, paused or out of wave buffers in that pass) and one
  the game moved to another destination start at their new volumes.
- Into a splitter, a voice ramps from the destination's volumes and its own volume of the pass before, whose product is
  the gain that pass mixed with; the destination's volumes are kept once per pass, since other voices may feed it too. A
  destination id of -1 is one the renderer does not have (`splitters_walk`) and is skipped. A destination out of range
  gets nothing, and no gain for the next pass to ramp from.
- `ramp_pass` is the pass after this render, as `runtime/hle/audren.c` `render_thread()` counts it.
- With `NX2SARIA_TRACE=audio`, the voices mixed in the last pass and not in this one are logged with why (a voice never
  mixed has 0), then the voices that read the same wave buffer memory in this pass (one stream heard twice, offset by
  how far apart the two read), once per pair and buffer.
- Submixes are folded deepest first, so every mix is complete before it is folded into its destination. A mix into a
  splitter ramps like a voice into one: from the destination's volumes and the mix's volume of the pass before, when
  that pass had the mix with as many buffers on the same splitter.
- What every splitter destination and every mix went in with is kept for the next pass's ramps here, once per pass,
  after every voice and mix that feeds a destination has read the values of the pass before.
- A catch-up frame (`runtime/hle/audren.c` `render_thread()`) ends before the device sink. Everything above ran, so the
  voices' filters and ramps, the effects and the sinks the game reads back are where they would be had the frame been
  heard; the device gets nothing of it.
- The device sink. With `NX2SARIA_TRACE=audio` the level of each input is summed: it shows whether the game mixes for
  5.1 (`level_note`). The output order is FL FR C LFE RL RR. 5.1 to stereo leaves the LFE out, as the Switch's downmix
  does. `NX2SARIA_AUDIO_DUMP` gets the front pair before clipping. A NaN (a NaN sample, an effect's state gone NaN)
  reaches the device as silence, not full scale.

### `change_event_list()`

IAudioDevice: one output device, stereo or 5.1 (`output_channels`). `change_events` holds the events the game took to
hear of a change of the output device (IAudioDevice's, its two notifications' and the virtual device manager's).
`audio_channels_live` signals them all: the game then asks for the channel count again, as it does on a console whose
output moves from the TV to headphones. A slot is counted before its handle is stored, so `audio_channels_live` skips a
slot that still reads 0 (`runtime/hle/kernel.c` `kern_event_signal()` ends the process on a bad handle).

### `change_event_of()`

One event per kind, made at the kind's first request; every request gets its own readable handle to it. Made per
request, each call took a kernel handle for good, and past 16 the events were no longer signalled. Two first requests at
once each make one: the loser closes its own, which was never listed, so nothing signals a closed handle.

### `audio_channels_live()`

Present path, after a settings change (`runtime/gfx/gfx_present.c` `window_settings_live()`): a changed
`NX2SARIA_AUDIO_CHANNELS` is decided again, and when it gives another channel count the game is told its output device
changed. The game asks `GetActiveChannelCount` again and mixes for the new count; the mixer moves its device to that
count at its next render (`channels_switch`). Measured on Ocarina 1.1.1: one event held, and the game asked within the
same second. Before anything has asked, nothing is signalled: the first call decides.

### `audio_device_handler()`

The IAudioDevice commands:

| Command | Name |
|---|---|
| 0, 6, 14 | ListAudioDeviceName(Auto), ListAudioOutputDeviceName [13.0.0+] |
| 1, 7 | SetAudioDeviceOutputVolume(Auto) |
| 2, 8 | GetAudioDeviceOutputVolume(Auto) |
| 3, 10, 13 | GetActiveAudioDeviceName(Auto), GetActiveAudioOutputDeviceName [13.0.0+] |
| 4, 11, 12 | QueryAudioDeviceSystemEvent, ...InputEvent, ...OutputEvent |
| 5 | GetActiveChannelCount |
| 15 to 18 | [17.0.0+] Acquire/Release Audio{Input,Output}DeviceNotification; an Acquire (15, 17) gets a readable event, signalled with the others |
| 19, 20 | [18.0.0+] SetAudioDeviceOutputVolumeAutoTuneEnabled, IsAudioDeviceOutputVolumeAutoTuneEnabled |
| 21 | [21.0.0+] IsActiveOutputDeviceEstimatedLowLatency |

The command ids are as the system answers them. The host applies no auto-tuned volume, so the answers are "off" and
"no".

### `virtual_device_manager_handler()`

`auddmg:u`, the Switch 2 audio device manager: command 0 of `audio_dmg_handler` opens this virtual device manager. Its
output-side commands and the 0x228-byte property block follow the system service's layout
(INx2AudioVirtualDeviceManager); the ids there are partly guessed. One virtual output device feeds the host mixer; there
are no input devices, so a command not listed here (the input side the SDK's ListVirtualInputDevices reaches) is logged
once and answered with success and a zero count.

| Command | Name |
|---|---|
| 0 | ListVirtualOutputDevices: count, names (0x100 bytes each) |
| 100 | GetDefaultVirtualOutputDevice: name |
| 300 | IsVirtualOutputDeviceStreamFormatSupported(sample type, channels, rate, functionality) |
| 401 | GetVirtualOutputDeviceProperties(name): the block |
| 500 | the state change event |
| 700, 800 | AttachStream, DetachStream |

The property block holds the type, (2), the channels and the sample rate. The channels are this run's output
(`output_channels`), as IAudioDevice's `GetActiveChannelCount` reports: a Switch 2 game (Ocarina) sizes its mix from this
block, so a constant 2 here kept 5.1 at stereo. The device id at 0x220 is nonzero: the SDK's output streams pass it on,
and a zero breaks stream creation.

### `audio_dmg_handler()`

Command 0 is OpenVirtualDeviceManager.

### `sysmix_session_handler()`

`audsmx:u`, the Switch 2 sound mixer (sysmix): command 0 of `audio_sysmix_handler`, OpenUserSession(process handle),
opens a user session; the session's CreateStream (1) makes a stream of a virtual device (the sysmix user session: device
1 is the virtual output). This host renders through audren, has no sysmix output streams yet and no input devices:
CreateStream of a device other than 1 fails with InvalidArgument (as the system does), and device 1 stops with its name
so the first title that needs a sysmix output stream says so. Engage and DisengageStreams (2 and 3) succeed.

## `runtime/audio/audio_effects.h`

Additional DSP effects, with no device or guest-memory access. The parameter blocks are the game's wire layouts (the
`_Static_assert`s hold their sizes).

### `audio_effect_invalid()`

Why the game's parameter block `param` of a delay (3), reverb (4) or compressor (9) cannot be used, as a constant
string, or NULL when it can (and for any other type). `audio_effect_create` and `audio_effect_update` end the process on
such a block, so the caller asks first.

## `runtime/audio/audio_effects.c`

Delay, standard reverb and compressor DSP. Runtime samples are normalized floats.

`early_time` and `early_gain` are indexed by the reverb's EarlyMode: five tables, one per mode, rather than the first
row for every mode.

### `line_set()`

The delay line's ring has `count` slots. The reference DelayLine wraps its cursor at CurrentSampleCount-1 while its taps
index modulo CurrentSampleCount, so once per turn every tap reads a slot that is never written (a one-sample dropout per
tap, audible as grain). The cursor and the taps share one ring length here; each delay is one sample longer than the
reference.

### `delay_invalid()`

What makes a delay, reverb or compressor block unusable for the DSP (`delay_invalid`, `reverb_invalid`,
`compressor_invalid`), or NULL. The game sends these blocks, so `runtime/audio/audio_mixer.c`
`audio_mixer_update_effects()` asks `audio_effect_invalid` before it creates or updates a DSP and passes the effect's
sound through instead; the `validate_*` checks in create and update are then invariants.

### `audio_effect_update()`

The "requires reset" checks are invariants too: `audio_mixer_update_effects` frees the DSP whenever the channels, the
rate or the longest delay differ from the block of the update before, which is the one this DSP was made or last
updated from.

## `runtime/audio/audio_reverb.h`

### `audio_reverb_invalid()`

Why the game's block `p` cannot be used, as a constant string, or NULL. `audio_reverb_create` and `audio_reverb_update`
end the process on such a block, so the caller asks first.

## `runtime/audio/audio_reverb.c`

A compatible 3D reverb network. Samples are normalized floats.

### `audio_reverb_invalid()`

`runtime/audio/audio_mixer.c` asks this before the game's block reaches create or update (and passes the sound through
instead), so `validate`'s fatal is an invariant.

### `audio_reverb_process()`

With six channels the reference's legacy surround channel order (L, R, BL, BR, C, LFE) is kept (`surround`).

## `runtime/audio/audio_capture.c`

The microphone through SDL3 ([audio-input.md](../audio-input.md)).

### `received()`

The SDL stream lock serializes this callback and every ring operation. The oldest frames are discarded on overflow, so
a paused game cannot grow RAM.

### `mic_stop()`

Closing the device unbinds the stream; unread frames remain in the ring.
