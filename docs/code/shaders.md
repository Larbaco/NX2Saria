# runtime/shaders

The runtime's own shaders, as source: the passes that stand in for the game's anti-aliasing (`aa_post.comp`, the four
`smaa_*.comp`), the HDR tone mapper spliced into the game's shader and the HDR10 present (`hdr_tonemap.frag`,
`hdr_encode.comp`, `hdr_ui.frag`, `hdr_fullscreen.vert`), the colour adjustment (`color_grade.comp`), the scalers and
sharpeners (`fsr_easu.comp`, `fsr_rcas.comp`, `dlaa_sharpen.comp`, `fallback_tonemap.spvasm`), the FSR 3.1 upscaler and
frame generation passes (`fsr3_*.comp`, `fsr3fi_*.comp`, `fsr3of_*.comp` with their two option headers), the motion
vectors (`dlaa_mv.comp`, `link_motion.frag`), the contact shadows (`contact_shadow.comp`), the texture decoder and
encoder (`astc_decode.comp`, `bc3_encode.comp`), the mesh fallback's two passes and its probe's programs
(`meshfb_*.comp`, `meshfb_probe_*`), and the write-back of video memory into guest memory (`wb_*.comp`).

Each pass is recorded by a file of `runtime/vk/`, named in its section below; that file's page has the C side. Where
the area sits in the host: "4. Graphics" in [architecture.md](../architecture.md), which also has the HDR path, the
anti-aliasing choice, the ASTC paths and the mesh fallback as features. The other feature docs the notes point to:
[frame-generation.md](../frame-generation.md) (FSR 3.1 frame generation), [presets.md](../presets.md) (what each
quality tier switches on, and what each setting costs), [maxwell.md](../maxwell.md) (what the console's GPU does that
these passes reproduce), [controls.md](../controls.md) (the menu rows of HDR and the upscalers),
[building.md](../building.md) (the tools the shader build needs, and the checks).

In the notes below "the game" is the title the build is for; "Ocarina" is its version 1.1.1. "The scene target" is the
game's R11G11B10F target that its composition pass writes; "the composite" is the linear, tonemapped picture that pass
reads. A "word" is 32 bits.

## How the shaders are built

`tools/build_shaders.py` turns every source of this folder into a C header of SPIR-V words,
`runtime/<name>_spv.h`, which the C file that records the pass includes. The headers are build outputs, never committed.

- `.comp`, `.vert` and `.frag` are compiled with `glslc -O` for Vulkan 1.1, `.mesh` and `.task` for Vulkan 1.2 (mesh
  and task shaders need SPIR-V 1.4 or later); every result is checked with `spirv-val`.
- A source that holds the line `// build_shaders: keep functions` is compiled without optimisation, so that its
  functions stay functions in the SPIR-V. Four files have it: `runtime/shaders/hdr_tonemap.frag`, whose function
  `runtime/vk/vk_hdr.c` `tonemap_splice()` finds by its name and copies into the game's shader, and the three programs
  of the mesh fallback's probe (`runtime/shaders/meshfb_probe_m.mesh`, `runtime/shaders/meshfb_probe_tm.mesh`,
  `runtime/shaders/meshfb_probe_t.task`), which have to keep the shape of a translated program: a function `program()`
  that the entry function calls. The line is the one comment these files keep besides licence notices; without it the
  compiler would inline the functions and the SPIR-V would change.
- `.spvasm` is assembled with `spirv-as` for Vulkan 1.3.
- An `#include "name"` is looked for beside the including file, then in `runtime/shaders/ffx3/`.
  `tools/build_shaders.py` `included()` reads the `#include` lines itself to know which files a header depends on.
- A header records the SHA-256 of its source and of the files the source includes
  (`tools/build_shaders.py` `source_hash()`), and is built again when that no longer matches. So a change to a source's
  text that leaves its SPIR-V as it was still builds its header once more.

Nothing else reads the text of these files. `runtime/vk/vk_hdr.c` `tonemap_splice()` reads the compiled tone mapper
(the `OpName` that starts with `nx_tonemap(`, the function's type and its parameters by pointer);
`runtime/vk/vk_exec_draw.c` finds the previous-position input of `link_motion.frag` in its SPIR-V by the decoration
`Location 15`; `runtime/gfx/mesh_fb.c` makes a compute module from the probe's compiled mesh and task programs as it
does from a translated one.

Not part of this page, and left as they are: `runtime/shaders/ffx/` and `runtime/shaders/ffx3/` (the FidelityFX
headers) and `runtime/shaders/smaa.glsl` (the SMAA reference implementation). They are unmodified copies, carry their
own documentation as comments, and the comment check does not read them ([README.md](README.md#the-checks)). The
licence notices at the head of `hdr_tonemap.frag`, `hdr_encode.comp`, `astc_decode.comp` and the 27 FSR 3.1 passes stay
in those files; `THIRD_PARTY_NOTICES.md` has their rows.

## `runtime/shaders/aa_common.glsl`

Shared by the anti-aliasing passes (`runtime/vk/vk_aa.c`) that stand in for the game's SMAA. The game's composition
pass stores the post-processed picture into its R11G11B10F scene target as raw words: each channel is the square root
of its linear value, the three packed together as RGB9E5 (red in bits 0-8, green 9-17, blue 18-26, the shared exponent
in 27-31, Vulkan's E5B9G9R9 layout). The passes here write the same words through an R32_UINT view of that target.

### `aa_pack_rgb9e5()`

RGB9E5 of a non-negative colour: the shared exponent of the largest channel, each channel rounded to 9 bits of it.

- `MAXV` = 65408.0 = (2^9 - 1) / 2^9 * 2^15, the largest value the format holds; the colour is clamped to 0..`MAXV`.
- `e` = max(-16, floor(log2(max(m, 1e-30)))) + 16, with `m` the largest channel: the exponent biased by 15, plus one.
- `scale` = 2^(e - 24) = 2^(e - 15 - 9).
- When the largest channel rounds to 512 or more at that scale, the exponent goes up by one and the scale doubles.

### `aa_encode()`

The stored form of a linear colour: its square root, which is also the space the edges are looked for in.

## `runtime/shaders/aa_post.comp`

The game's SMAA composition replaced (`runtime/vk/vk_aa.c`): mode 0 stores the composite as it is (anti-aliasing off),
mode 1 runs a fast approximate edge smoothing (FXAA) over it first. Input: the game's composite, linear, already
tonemapped. Output: the scene target's raw RGB9E5 words of the square root of each channel
(`runtime/shaders/aa_common.glsl`).

Bindings: 0 `src`, the composite (a sampler); 1 `dst`, the scene target as `r32ui`. Workgroups of 8x8. Push constants
(`Push`):

| Field | Meaning |
|---|---|
| `vec2 inv` | 1 / size |
| `ivec2 size` | the scene target's size |
| `int mode` | 0 copy, 1 FXAA |

Constants:

| Name | Value | Meaning |
|---|---|---|
| `EDGE_MIN` | 0.0312 | no smoothing below this contrast (dark areas) |
| `EDGE_REL` | 0.125 | nor below this share of the brightest neighbour |
| `SUBPIXEL` | 0.75 | how much a thin feature is blurred |
| `STEPS` | 12 | steps of the walk along the edge, each way |
| `STEP_LEN` | 1, 1, 1, 1, 1, 1.5, 2, 2, 2, 2, 4, 8 | the length of each step, in pixels |

The luma weights are 0.299, 0.587, 0.114, applied to the square-rooted colour.

### `fxaa()`

The smoothing works on the square-rooted luma: where the local contrast is high enough, it finds whether the edge
runs across or along the pixel, walks along the edge both ways until the luma pair it follows changes, and moves the
sample point toward the neighbour across the edge by how far the pixel sits from the nearer end of the edge. A blur of
the 3x3 neighbourhood's contrast handles edges thinner than a pixel. The sample at the moved point is bilinear, so the
move blends the two sides.

In order:

- The pixel is returned as it is when the range of the centre and its four neighbours is below
  max(`EDGE_MIN`, brightest * `EDGE_REL`).
- Thin features: how far the centre stands out from the neighbourhood's weighted mean, (2 * (N + S + W + E) + NW + NE
  + SW + SE) / 12, relative to the range; through a smoothstep, squared, times `SUBPIXEL`.
- The edge's direction: horizontal when the luma changes more from row to row than from column to column (second
  differences with weights 0.25, 0.5, 0.25 over the three rows or columns).
- Which side of the pixel the edge is on: the neighbour across it with the larger step. `pair` is the mean luma of the
  pixel and that neighbour, which the walk follows; `grad` is a quarter of the larger step.
- The walk along the edge, both ways, starts half a pixel toward that neighbour and goes on until the pair's luma
  changes by more than a quarter of the step across it (`grad`), or `STEPS` steps are done.
- The offset is 0.5 - (distance to the nearer end) / (the edge's length), and only when the luma at the nearer end
  moves the way the centre's does: else the pixel is outside the edge's span and the offset is 0. The larger of this
  offset and the thin-feature blur is used.

## `runtime/shaders/smaa_setup.glsl`

Settings and resources shared by the SMAA passes (`runtime/vk/vk_aa.c`; the four `smaa_*.comp` below).
`runtime/shaders/smaa.glsl` holds the SMAA reference implementation, unmodified; its pixel- and vertex-shader functions
run here inside compute shaders, one invocation per pixel, with the texture coordinate of the pixel's centre. Its
`discard` of a pixel without edges becomes nothing (`#define discard`): the edge pass then stores the zero edges it
found, which is what the next pass reads for such a pixel anyway.

It sets workgroups of 8x8, `SMAA_GLSL_4`, the preset `SMAA_PRESET_ULTRA`, and `SMAA_RT_METRICS` to the push constant
`pc.metrics`: 1 / width, 1 / height, width, height. The area and search lookup textures the weight pass reads are
`runtime/third_party/smaa/AreaTex.bin` and `runtime/third_party/smaa/SearchTex.bin`, which `runtime/vk/vk_aa.c` uploads.

### `smaa_pixel()`

The pixel this invocation works on and its centre's texture coordinate; false past the image's edge.

## `runtime/shaders/smaa_encode.comp`

SMAA pass 0 (`runtime/vk/vk_aa.c`): the luma SMAA looks for edges in, from the game's linear composite, in the
square-root space the scene target stores (`runtime/shaders/aa_common.glsl`). One channel (`r16f`): SMAA's luma edge
detection takes the dot product of the colour it reads with its luma weights, and a one-channel image reads as
(r, 0, 0), so r holds the luma over the red weight. The weights are those of `SMAALumaEdgeDetectionPS()` in
`runtime/shaders/smaa.glsl`: 0.2126, 0.7152, 0.0722.

Bindings: 0 `src`, the composite; 1 `dst`, the luma image. Push constants: `metrics`, as in `smaa_setup.glsl`.

## `runtime/shaders/smaa_edge.comp`

SMAA pass 1 (`runtime/vk/vk_aa.c`): luma edge detection over pass 0's luma into the edges image (red: an edge on the
pixel's left, green: on its top).

Bindings: 0 `luma`, the image `runtime/shaders/smaa_encode.comp` wrote, with linear filtering; 1 `edges` (`rgba8`).

## `runtime/shaders/smaa_weight.comp`

SMAA pass 2 (`runtime/vk/vk_aa.c`): blending weights from the edges, with the area and search lookup textures.

Bindings: 0 `edges`, linear filtering; 1 `area`, linear filtering; 2 `search`, nearest; 3 `weights` (`rgba8`).

## `runtime/shaders/smaa_blend.comp`

SMAA pass 3 (`runtime/vk/vk_aa.c`): neighbourhood blending of the game's composite by the weights, in linear light (as
SMAA asks of a picture read through an sRGB view), stored into the game's scene target as its raw RGB9E5 words of the
square root (`runtime/shaders/aa_common.glsl` `aa_pack_rgb9e5()` and `aa_encode()`).

Bindings: 0 `color`, the game's composite, linear filtering; 1 `weights`, linear filtering; 2 `dst`, the scene target
as `r32ui`.

## `runtime/shaders/hdr_tonemap.frag`

The HDR tonemap, as a function that `runtime/vk/vk_hdr.c` `vk_hdr_shader_swap()` splices into the game's own tonemap
shader (`tonemap_splice()` does the work). The feature is described in "4. Graphics" of
[architecture.md](../architecture.md); the menu rows in [controls.md](../controls.md).

The file's first lines are a licence notice, which stays in the code, and the line `// build_shaders: keep functions`:
`tools/build_shaders.py` compiles this file without optimisation, as inlining would leave no function to splice.
`main()` only exists so the compiler keeps the functions; `vk_hdr.c` drops it. Its seven inputs `a0`..`a6` stand for
the parameters of `nx_tonemap()`.

Three tone mappers (`p0.w`):

- 9, the observer-response mapper (`scene_tonemap()`, `psycho35()`), the default: the scene light goes through a finite
  cone response in LMS, anchored so that scene grey 0.18 lands where the game's curve puts it (or at 0.18 times the
  gain) and, with the tone slope matched, rising through it as steeply as the game's curve does at its steepest; the
  response saturates at the display's peak. Chroma is the response's residual around its mean, scaled back just enough
  to fit Rec. 709 (the game's scene target is r11g11b10f, which holds no negative component, so a wider fit would be
  clipped channel by channel). Bright colours get a share of the hue bend the game's clamp gives them. It takes the
  scene without local exposure.
- 0, the game's curve: its SDR result at the picture's white.
- 3, the luminance mapper (`drt()`): the game's SDR result is the colour grade, upgraded to the HDR range by luminance
  against a neutral SDR curve of the same input, then a luminance curve towards the peak with exposure, highlights,
  shadows, contrast and flare, and in OkLab the hue of the input, saturation, highlight saturation and blowout.

The present then shows the picture as a display of gamma 2.2 or 2.4 would, unless that is switched off
(`runtime/shaders/hdr_encode.comp`); the peak given to a tone mapper is the display's taken through that correction's
inverse, so that the brightest light still lands on the display's peak.

Settings: five vec4, which `runtime/vk/vk_hdr.c` `vk_hdr_support()` fills and the support buffer carries in its bytes
1440..1519. Their defaults are in brackets. A grading value of 1 is compared for and its step left out, so the
defaults give the observer-response mapper's plain result and not that result times factors that only round to 1.

| Vector | x | y | z | w |
|---|---|---|---|---|
| `p0` | peak nits | game nits (white of the picture) | HUD nits | tone mapper (-1: the game's own SDR picture, as when the swapchain is not HDR10; else as above) [9] |
| `p1` | exposure [1] | highlights [1] | shadows [1] | contrast [1] |
| `p2` | saturation [1] | highlight saturation [1] | blowout [0] | flare [0] |
| `p3` | hue correction [1] | grade strength [1] | gamma correction (0 off, 1: 2.2, 2: 2.4; applied on the present) [1] | tone slope [1] |
| `p4` | hue clip emulation (0..1) [0.5] | mid grey (0: 0.18 times the gain, 1: the game's curve at 0.18) [1] | match tone slope (0 or 1) [1] | unused |

Colour matrices. They are written row by row; `mulm()` is the row-vector product of that layout (`v * m`).

| Matrix | Rows |
|---|---|
| `BT709_TO_AP1` | 0.6130974024, 0.3395231462, 0.0473794514; 0.0701937225, 0.9163538791, 0.0134523985; 0.0206155929, 0.1095697729, 0.8698146342 |
| `AP1_TO_BT709` | 1.7050509927, -0.6217921207, -0.0832588720; -0.1302564175, 1.1408047366, -0.0105483191; -0.0240033568, -0.1289689761, 1.1529723329 |
| `BT709_TO_OKLABLMS` | 0.4122214708, 0.5363325363, 0.0514459929; 0.2119034982, 0.6806995451, 0.1073969566; 0.0883024619, 0.2817188376, 0.6299787005 |
| `OKLABLMS_TO_OKLAB` | 0.2104542553, 0.7936177850, -0.0040720468; 1.9779984951, -2.4285922050, 0.4505937099; 0.0259040371, 0.7827717662, -0.8086757660 |
| `OKLAB_TO_OKLABLMS` | 1.0, 0.3963377774, 0.2158037573; 1.0, -0.1055613458, -0.0638541728; 1.0, -0.0894841775, -1.2914855480 |
| `OKLABLMS_TO_BT709` | 4.0767416621, -3.3077115913, 0.2309699292; -1.2684380046, 2.6097574011, -0.3413193965; -0.0041960863, -0.7034186147, 1.7076147010 |
| `PSY_BT709_TO_LMS` | 0.2896057766, 0.6972466442, 0.0763712786; 0.0901837576, 0.7073490257, 0.1122031464; 0.0155287401, 0.0536093073, 0.5097978305 |
| `PSY_LMS_TO_BT709` | 4.9676196505, -4.9223795294, 0.3391991812; -0.6196828788, 2.0517506515, -0.3587439844; -0.0861520040, -0.0658193830, 1.9889544833 |

The two `PSY_` matrices take Rec. 709 (D65) to the cone fundamentals LMS of the two-degree observer and back. They are
the products of the XYZ matrices of each, worked out in double precision. `PSY_YF_WEIGHTS` = (0.68990272, 0.34832189,
0.0) is the luminance of the observer from L and M. `PSY_D65_WHITE_LMS` = (1.0632236993, 0.9097359297, 0.5789358779)
is `PSY_BT709_TO_LMS` of white. The Rec. 709 luminance weights of `y709()` are 0.2126390059, 0.7151686788,
0.0721923154.

### `hue_oklab()`

The hue (the direction of OkLab's a and b) of `correct` with the chrominance of `incorrect`; the result is clamped to
AP1 (`clamp_ap1()`: no negative component in AP1).

### `gamma_to_srgb()`, `peak_before_gamma()`

`gamma_to_srgb()`: a value of a gamma-g picture as the sRGB value of the same code (the peak's correction).
`peak_before_gamma()`: the display's peak (in picture whites) as a tone mapper is to reach it: what the present's
gamma correction (0 none, 1: 2.2, 2: 2.4) brings to the peak.

### `smoother()`

t^3 (t (6 t - 15) + 10). Both mappers' grading blends with it.

### `highlights()`, `shadows()`, `curve()`

The luminance mapper's steps. `highlights()` and `shadows()` return their input at a setting of 1; the shadows' floor
is mid grey / 16.

`curve()` is the luminance curve: `n` peak nits, `g` contrast, `c_d` mid grey in nits, `t_1` flare; 100 nits reference
(`n_r`), scene grey 0.18 (`c`). Its other constants: `w_g` 0, `r_hit_min` 128, `r_hit_max` 256, with the hit point
interpolated by log(n / 100) over log(10000 / 100).

### `drt()`

The tone mapper by luminance, then hue, saturation and blowout in OkLab; clamped to the peak in Rec. 709. `Drt` holds
its settings; `clamp_zero` clamps to Rec. 709 (no negative channel) before the peak. Mid grey is 0.18 and the peak is
`nits_peak` / 100. The highlight desaturation (`dechroma`) scales chroma by 1 - (y / 100)^(1 - dechroma); the blowout
by (1 - y * 100 / 10000)^(100 |blowout|), or 2 minus that for a negative blowout.

### `neutral_sdr()`

The same curve at SDR range with nothing changed: what the SDR grade is measured against. Its settings: 100 nits,
every factor 1, every offset 0, mid grey 18 nits, clamped to Rec. 709.

### `upgrade()`

The graded SDR colour carried to the HDR range: its luminance gains what the untonemapped colour has above the neutral
SDR curve, in the graded colour's hue.

### `aces_fitted()`

The game's curve and clamp: x (2.51 x + 0.03) / (x (2.43 x + 0.59) + 0.14), clamped to 0..1.

### `psy_mean()`, `psy_rgb_residual()`, `psy_response_logs()`

The observer-response mapper's parts.

- `psy_mean()`: a mean that is exact on vectors of equal components: x + ((y - x) + (z - x)) / 3.
- `psy_rgb_residual()`: a residual in white-normalised LMS as the displacement it is in Rec. 709.
- `psy_response_logs()`: the finite response u of one cone in the log domain: `log_u`, and `log_deficit` = log(1 - u),
  kept as logarithms so that neither end underflows. Above a latent value of 20 the deficit is the tail's logarithm;
  where |log u| is below 0.01 it is a series in log u (terms 1, 1/2, 1/6, 1/24).

### `psycho35()`

Extended linear Rec. 709 in, relative to reference white; the same out, inside Rec. 709 and below `peak`.
`anchor_in_gray` is the scene grey the eye is adapted to, `anchor_out_gray` where it lands; `cone` is the response's
exponent. `highlights`, `shadows`, `contrast` and `purity_scale` (saturation) grade the input; each is left out at 1.

- Highlights and shadows work on the observer's luminance, with the colour scaled with it. The shadows' range ends
  four stops below the anchor (2^-4).
- `source_ratio` is the input against its adaptation.
- Purity (saturation) is applied around the neutral of the same luminance, relative to the adaptation. More purity must
  not turn a valid cone input negative: the common amount is bounded, not single cones.
- The exposure is factored out: `source_ratio` = `drive` * `ratio`, with the largest component of `ratio` 1.
- The response's residual is normalised before it is squared: the length can underflow where the vector itself does
  not. Above a mean response of 0.5 the residual is taken from the deficits.
- The chroma's direction is set half way to the source's.
- A neutral result has nothing to fit and is returned as the mean.
- The exact fit: the chroma is scaled until the nearest face of the cube 0..peak is touched.

### `hue_clip()`

The bend the game's clamp gives a bright colour (a clipped channel stops, the others catch up, the hue turns towards
yellow, cyan or white), given to the unclamped colour: the curve's result, stretched back over the colour's own range
of magnitudes, at the colour's luminance.

### `scene_tonemap()`

The scene (before the exposure gain) through the observer-response mapper, in picture whites. `peak`: the display's
peak in picture whites, as the response is to reach it.

- Zero exposure (`p1.x` 0) is black; the mapper needs a positive anchor.
- `cone` with the tone slope matched (`p4.z`) is 1.3355037565264822: the log-log slope x V'(x) / V(x) of the game's
  curve where V''(x) = 0, at x = 0.120305925. Unmatched it is 1. The tone slope setting (`p3.w`) multiplies it.
- `anchor_out`: scene grey where the game's curve puts it (the curve's value at 0.18, with `p4.y` 1) or at its own
  value 0.18, times the gain and the exposure. `anchor_in` is 0.18.

### `nx_tonemap()`

The spliced function. `x`: the three values the game feeds its fitted ACES curve (its scene light after its colour
matrix, bloom, exposure and, in the gameplay shader, local exposure). `scene`: the same values without local exposure
(rgb) with the exposure gain in them (a; 1 where the splice found none). It returns the colour the shader then stores,
linear Rec. 709 relative to the HUD's white, which the present encodes to PQ (`runtime/shaders/hdr_encode.comp`).

- `p0.w` below 0: the game's own SDR picture, the curve's result.
- Tone mapper 0: the game's curve, at the picture's white.
- Tone mapper 9: the mapper takes the light before the gain (`scene.rgb / scene.a`); without a gain there is no light,
  and the result is black. Its peak is max(peak nits, game nits) / game nits, through `peak_before_gamma()`.
- Any other positive value (3): the luminance mapper, on the upgraded grade (or on `x` itself at a grade strength of
  0), with the peak `p0.x / p0.y` through `peak_before_gamma()`, times 100 as nits. The flare is 0.10 * (`p2.w`)^10,
  the blowout -(`p2.y` - 1), mid grey 18 nits.
- The result is multiplied by `p0.y / p0.z`, the picture's white against the HUD's, and a NaN becomes 0.

## `runtime/shaders/hdr_encode.comp`

HDR10 encode on the present (`runtime/vk/vk_hdr.c` `vk_hdr_encode()`): the game's picture, crop rectangle at offset,
linear Rec. 709 with the HUD's white at 1.0 (an sRGB view decodes the 8-bit window texture), into an RGBA16F image of
PQ code values in Rec. 2020 that FSR 1 or the blit then scale into the HDR10 swapchain. The file's first lines are a
licence notice, which stays in the code.

In order: the gamma correction (the game's sRGB-encoded look shown as a 2.2 or 2.4 display would: decode of the sRGB
code with that power), the conversion to Rec. 2020, a gamut compression that desaturates towards the grey of the same
luminance until no channel is negative (in a gamma-encoded space whose 50% is the scene's mid grey), the HUD's white
in nits, the largest channel scaled down to the peak, and PQ (SMPTE ST 2084).

Bindings: 0 `src` (a sampler), 1 `dst` (`rgba16f`). Workgroups of 8x8. Push constants (`P`): `ivec2 offset`,
`ivec2 size`, and `vec4 p`:

| Component | Meaning |
|---|---|
| `p.x` | nits of 1.0 |
| `p.y` | the display's peak in nits |
| `p.z` | 1 when the source holds sRGB-encoded values in a UNORM format |
| `p.w` | gamma correction (0 none, 1: 2.2, 2: 2.4) |

### `gamma_correct()`

The colour an sRGB code shows on a display of power g, sign kept.

### `pq()`

SMPTE ST 2084, from nits over 10000: m1 0.1593017578125, m2 78.84375, c1 0.8359375, c2 18.8515625, c3 18.6875.
`runtime/shaders/hdr_ui.frag` `pq()` is the same.

### `main()`

- A NaN in the source becomes 0.
- Rec. 709 to Rec. 2020, rows: 0.6274038960, 0.3292830384, 0.0433130657; 0.0690972894, 0.9195403951, 0.0113623156;
  0.0163914388, 0.0880133078, 0.8955952532. The Rec. 2020 luminance weights: 0.2627002120, 0.6779980715, 0.0593017165.
- `MID_GRAY_GAMMA` = log(1 / 10^0.75) / log(0.5), about 2.49: 18% grey encodes to 50%.

## `runtime/shaders/hdr_ui.frag`

The in-game menu on an HDR10 swapchain (`runtime/vk/vk_hdr.c` `vk_hdr_ui_compose()`): ImGui drew it into an RGBA8
image, premultiplied sRGB colour over a clear 0; this turns each pixel back into its colour (with the picture's gamma
correction `p.y`: 0 none, 1: 2.2, 2: 2.4, as `runtime/shaders/hdr_encode.comp` applies it), puts its white at `p.x`
nits and writes the PQ code with its coverage, which the pipeline blends over the picture (in PQ code values, as the
menu blends in sRGB code values on an SDR swapchain). PQ: SMPTE ST 2084; Rec. 709 to Rec. 2020: ITU-R BT.2087 (here
with four decimals: 0.6274, 0.0691, 0.0164, 0.3293, 0.9195, 0.0880, 0.0433, 0.0114, 0.8956, by columns).

Binding 0: `ui`, the menu's image. Push constants: `vec4 p`. A pixel without coverage is discarded.

## `runtime/shaders/hdr_fullscreen.vert`

One triangle over the whole target, for `runtime/shaders/hdr_ui.frag` (`runtime/vk/vk_hdr.c` `vk_hdr_ui_compose()`).
