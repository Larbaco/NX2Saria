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
licence notices at the head of `hdr_tonemap.frag`, `hdr_encode.comp`, `astc_decode.comp` and the 26 FSR 3.1 passes stay
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

## `runtime/shaders/color_grade.comp`

Colour adjustment on the present (`runtime/vk/vk_color.c`): the game's picture, crop rectangle at offset, into an
RGBA16F image the FSR 1 pass or the blit then scales to the window. Brightness, contrast around mid-grey (0.5),
saturation against Rec. 709 luma (0.2126, 0.7152, 0.0722), then a power curve: the order and ranges of the colour
profiles of issue #136. The result is clamped to 0..1.

Bindings: 0 `src` (a sampler), 1 `dst` (`rgba16f`). Workgroups of 8x8. Push constants (`P`):

| Field | Meaning |
|---|---|
| `ivec2 offset` | the crop rectangle's corner in `src` |
| `ivec2 size` | its size, the size of `dst` |
| `vec4 adj` | brightness (added), contrast, saturation, gamma (exponent) |

## `runtime/shaders/fsr_easu.comp`

FSR 1's edge-adaptive upscale (EASU) of the game's output to the swapchain size (`runtime/vk/vk_fsr.c`), and of the
scene to the picture's size where the game's own upscaler ran (`runtime/vk/vk_tonemap.c`, with `SQUARE` set). It
includes the two FidelityFX headers of `runtime/shaders/ffx/`, `ffx_a.h` and `ffx_fsr1.h`, which are unmodified. Each
invocation writes four pixels of a 16x16 tile (a workgroup of 64 invocations), the layout the filter's own sample
uses; the constants come from `FsrEasuConOffset`, so the game's crop rectangle can start anywhere in its image.

Bindings: 0 `src` (a sampler), 1 `dst` (`rgba16f`). Push constants (`Params`):

| Field | Meaning |
|---|---|
| `vec4 view` | crop width, crop height, source image width, source image height |
| `vec4 out_` | output width, output height, crop x, crop y |

`SQUARE` is specialization constant 0, which only `runtime/vk/vk_tonemap.c` sets: its source holds the square root of
each linear value (the game's post pass stores the picture so) and its output is linear, so what the filter gives is
squared. The filter runs on the stored values either way: square roots are the perceptual encoding it is made for,
and the game's own upscaler filters them too. Unset (the present): the filtered value as it is.

## `runtime/shaders/fsr_rcas.comp`

FSR 1's robust contrast-adaptive sharpening (RCAS) of the EASU result (`runtime/vk/vk_fsr.c`), with the same two
headers of `runtime/shaders/ffx/` and the same tile layout as `runtime/shaders/fsr_easu.comp`.

Bindings: 0 `src`, 1 `dst` (`rgba16f`). Push constants (`Params`): `vec4 size`: width, height, sharpness in stops (0 =
most), unused.

## `runtime/shaders/dlaa_sharpen.comp`

A second pass of RCAS over FSR 3.1's picture in its scaling modes (`runtime/vk/vk_dlaa.c` `fsr3_sharpen()`), with the
two headers of `runtime/shaders/ffx/`. The picture is linear scene light without an upper bound, and RCAS limits its
sharpening by each channel's distance from 0 and from 1: every texel is taken through c / (1 + max(c)) on the way in,
which keeps hue and maps any light into 0..1, and through the inverse on the way out. The inverse divides by
max(1 - max(c), 1 / 16384), so light up to 16,383 comes back as it went in.

Bindings and push constants as `runtime/shaders/fsr_rcas.comp`: `vec4 size`: width, height, sharpness in stops (0 =
most), unused.

## `runtime/shaders/fallback_tonemap.spvasm`

Scale and decode for the untranslatable scene upscaler (`runtime/vk/vk_exec_draw.c`
`untranslated_dispatch_fallback()`). Its input is the post pass's picture with each channel stored as the square root
of its linear value (the game's program 102 packs sqrt(composite) into RGB9E5), and its output is linear (the HUD is
drawn over it, the frame's last pass encodes to sRGB). So: filter in the stored (sqrt) values, as the game's upscaler
does, then square. The pass replaced a decode with the sRGB curve, which darkened mid-tones and lifted the darkest
values.

It is written in SPIR-V assembly and assembled with `spirv-as`: when it was written the build had no GLSL compiler.
(It has one now, `glslc`, for the other shaders of this folder.) As GLSL it is:

```glsl
layout(local_size_x = 8, local_size_y = 8) in;
layout(binding = 0) uniform sampler2D src;
layout(binding = 1, rgba16f) uniform writeonly image2D dst;
layout(push_constant) uniform P { vec2 inv; float unused; float pad; ivec2 size; } pc;
void main() {
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    if (p.x >= pc.size.x || p.y >= pc.size.y) return;
    vec3 s = textureLod(src, (vec2(p) + 0.5) * pc.inv, 0.0).rgb;
    imageStore(dst, p, vec4(s * s, 1.0));
}
```

`src` is the stored picture, with linear filtering; `dst` is the pass output of `runtime/vk/vk_tonemap.c`, blitted
into the target. The push constants sit at offsets 0 (`inv`), 8, 12 and 16 (`size`).

## The FSR 3.1 passes

`runtime/shaders/fsr3_options.h` and `runtime/shaders/fsr3fg_options.h` are included by the passes below and by
nothing else: they are GLSL, with a C header's name. Each pass is a pass file of FSR 3.1 that includes one of the two
option headers and then the unmodified headers of `runtime/shaders/ffx3/`; `tools/build_shaders.py` compiles it into
`runtime/<name>_spv.h`. Each file starts with its licence notice, which stays in the code; `THIRD_PARTY_NOTICES.md`
says what was changed in them. The feature: [frame-generation.md](../frame-generation.md) for frame generation,
[presets.md](../presets.md) for the upscaler's modes.

### `runtime/shaders/fsr3_options.h`

The build options of the FSR 3.1 upscaler passes (`fsr3_*.comp`, recorded by `runtime/vk/vk_fsr3.c`), included after
`#version` and the `#extension` lines, for the inputs `runtime/vk/vk_dlaa.c` hands over: linear HDR scene light, motion vectors at the render
size and without the jitter, depth not inverted. Everything runs in 32-bit floats (no 16-bit arithmetic); the
reprojection's Lanczos weights come from the 128-entry table `runtime/vk/vk_fsr3.c` `make_shared()` uploads.
`FFX_FSR3UPSCALER_OPTION_APPLY_SHARPENING` is 0 unless the including file defined it first.

### `runtime/shaders/fsr3fg_options.h`

The build options of the FSR 3.1 frame generation passes (`fsr3fi_*.comp`, frame interpolation, and `fsr3of_*.comp`,
optical flow; recorded by `runtime/vk/vk_fsr3fg.c`), for the inputs `runtime/vk/vk_dlaa.c` hands over: motion vectors
at the render size and without the jitter, depth not inverted (0 at the near plane). Everything runs in 32-bit floats.
The colour of an SDR swapchain image is sRGB-encoded UNORM, which the passes take from their constants (backbuffer
transfer function 0), not from an option. It is included at the same place, after `#version` and the `#extension` lines.

### The upscaler's passes

Recorded by `runtime/vk/vk_fsr3.c`, with the options of `runtime/shaders/fsr3_options.h`:

- `runtime/shaders/fsr3_prepare_inputs.comp`
- `runtime/shaders/fsr3_luma_pyramid.comp`
- `runtime/shaders/fsr3_shading_change_pyramid.comp`
- `runtime/shaders/fsr3_shading_change.comp`
- `runtime/shaders/fsr3_prepare_reactivity.comp`
- `runtime/shaders/fsr3_luma_instability.comp`: the luma history it writes, at binding 7, is declared in this file
  (`rw_luma_history`, with `StoreLumaHistory()`) as `rgba16f` instead of by the callbacks header, which names it
  `rgba8`: the image (`IMG`, `I_LH0` and `I_LH1` in `runtime/vk/vk_fsr3.c`) is RGBA16F and holds luma above 1 in HDR,
  which an `rgba8` store would clamp.
- `runtime/shaders/fsr3_accumulate.comp`
- `runtime/shaders/fsr3_accumulate_sharpen.comp`: the accumulate pass with sharpening on (it defines
  `FFX_FSR3UPSCALER_OPTION_APPLY_SHARPENING` as 1 before the include).
- `runtime/shaders/fsr3_rcas.comp`

The two accumulate passes and `fsr3_rcas.comp` require `GL_EXT_shader_image_load_formatted`: the declaration of
`rw_upscaled_output` needs it.

### The frame generation passes

Recorded by `runtime/vk/vk_fsr3fg.c`, with the options of `runtime/shaders/fsr3fg_options.h`. Frame interpolation:

- `runtime/shaders/fsr3fi_setup.comp`
- `runtime/shaders/fsr3fi_reconstruct_and_dilate.comp`
- `runtime/shaders/fsr3fi_reconstruct_previous_depth.comp`
- `runtime/shaders/fsr3fi_game_motion_vector_field.comp`
- `runtime/shaders/fsr3fi_game_vector_field_inpainting_pyramid.comp`
- `runtime/shaders/fsr3fi_optical_flow_vector_field.comp`
- `runtime/shaders/fsr3fi_disocclusion_mask.comp`
- `runtime/shaders/fsr3fi_interpolate.comp`
- `runtime/shaders/fsr3fi_inpainting_pyramid.comp`
- `runtime/shaders/fsr3fi_inpainting.comp`

`fsr3fi_interpolate.comp` and `fsr3fi_inpainting.comp` require `GL_EXT_shader_image_load_formatted`: the declaration
of `rw_output` needs it.

Optical flow:

- `runtime/shaders/fsr3of_prepare_luma.comp`
- `runtime/shaders/fsr3of_luminance_pyramid.comp`
- `runtime/shaders/fsr3of_scd_histogram.comp`
- `runtime/shaders/fsr3of_scd_divergence.comp`
- `runtime/shaders/fsr3of_search.comp`
- `runtime/shaders/fsr3of_filter.comp`
- `runtime/shaders/fsr3of_scale.comp`

## `runtime/shaders/dlaa_mv.comp`

Camera motion vectors for DLAA (`runtime/vk/vk_dlaa.c` `mv_pass()`): each pixel's depth is taken back to the world
through this frame's view-projection and forward through last frame's; the vector is last frame's window position
minus this frame's, in render pixels, without the jitter (DLSS's default convention). Objects that move on their own
get only the camera's motion here: the per-object pass (`runtime/vk/vk_exec_draw.c` `vk_mvobj_execute()`) or the depth
check below corrects theirs.

The camera block is the game's scene uniform block (bound as c4 in the scene's vertex shaders): vec4 rows 3-6 are the
view-projection, rows of a row-major 4x4 with OpenGL clip depth (-w..w). The replay's viewports have negative heights,
so window y grows downwards from ndc y = +1, and depth is z_ndc * 0.5 + 0.5. The shader reads rows 3-6 of the two
blocks at bindings 2 and 3, inverts the first and multiplies by the second. `mv_pass()` does not hand it the two
cameras as they are: it works out last frame's view-projection times the inverse of this frame's in double precision
and fills the first block with the identity and the second with that product.

The bias mask. With `pc.bias` > 0 the pass also writes DLSS's bias mask (the characters' mask of
`runtime/vk/vk_dlaa.c`): G-buffer target 0 carries an object class in green, above 64/255 for characters, creatures
and what they hold (Link 253, others 125, held items 160; terrain 0, foliage 32). A pixel gets `pc.bias` when a
character covers it now, or covered the place its surface came from last frame (the previous class mask, read through
the motion vector, 3x3): there the history holds a moving character that the camera-only vector does not follow, which
is the trail. `chars_out` keeps this frame's mask for the next; `chars_prev` is last frame's (`pc.prev_ok` 0: none).

The depth check. Each pixel's view distance is also kept for the next frame (`depth_out`; `depth_prev` is last
frame's, `pc.zprev_ok` 1 when it holds one). With `pc.zfit` > 0 (a frame without object motion and without the class
buffer: Ocarina's) the vector is checked against it. The camera's motion predicts how far the surface was last frame
at the place it came from; where last frame's depth there disagrees, the surface moved on its own. The commonest such
surface is the character the camera follows, which stays where it is on screen: when last frame's depth at the same
pixel agrees, the vector is zero instead. Either way such a pixel gets the bias mask (`pc.zmask`), so DLSS takes this
frame's colour where its history holds something else (the trail behind a running Link). `pc.dz`: the projection's
depth terms (rows 2 and 3, columns 2 and 3) that take ndc depth back to view distance.

Bindings (workgroups of 8x8):

| Binding | Name | What |
|---|---|---|
| 0 | `depth_tex` | this frame's depth |
| 1 | `mv_out` | the vectors, `rg16f` |
| 2 | `cur` | `vec4 r[16]`, std140: this frame's block |
| 3 | `prev` | `vec4 r[16]`, std140: last frame's block |
| 4 | `ids_tex` | G-buffer target 0, the classes |
| 5 | `mask_out` | the bias mask, `r8` |
| 6 | `chars_prev` | last frame's class mask, `r8` |
| 7 | `chars_out` | this frame's class mask, `r8` |
| 8 | `depth_prev` | last frame's view distances, `r32f` |
| 9 | `depth_out` | this frame's view distances, `r32f` |

Push constants (`P`; `MvPush` in `runtime/vk/vk_dlaa.c` has the same layout, std430, `dz` at offset 32):

| Field | Meaning |
|---|---|
| `vec2 size` | render size in pixels |
| `vec2 jitter` | this frame's jitter, pixels, as the scene was drawn (window x right, y down) |
| `float bias` | the mask value for character pixels; 0: no mask (bindings 4-7 unused) |
| `float prev_ok` | 1: `chars_prev` holds last frame's class mask at this size |
| `float zfit` | 1: the depth check; 0: off |
| `float zprev_ok` | 1: `depth_prev` holds last frame's view distances at this size |
| `vec4 dz` | the projection's [2][2], [2][3], [3][2], [3][3] (row-major) |
| `float zmask` | the depth check's mask value (0: the vectors are checked, no mask) |

### `dist()`

View distance from ndc depth; 0 for the sky and other points at or past the far plane (a denominator below 1e-12, or
a distance of 1e7 and more).

### `miss()`

How far last frame's distance near window position `w` is from the distance expected there, relative: the closest of
the 3x3 pixels around it, so a reprojection that lands a pixel across a silhouette (foliage, a roof's edge) is not
taken for motion. 0 off screen (nothing to compare: the surface just came into view, DLSS handles that), 1e9 where
last frame had only sky there.

### `main()`

- `win` is where this pixel's surface sits without the jitter: the pixel's centre minus `pc.jitter`.
- The vector stays zero where the world point's w is within 1e-12 of zero or last frame's clip w is 1e-6 or less.
- The depth check's tolerance, `TOL`, is 0.04. 4 %: the depth of a still surface reprojected through the camera lands
  within about 1 % of last frame's (sub-pixel position and the depth buffer's precision); a character a step away from
  the wall behind it is 10 % and more.
- A pixel whose reprojected depth misses by more than `TOL` counts as moved; when the depth at its own pixel agrees
  within `TOL`, it kept its place on screen (the followed character) and its vector is zero.
- With the depth check on, `mask_out` gets moved * `pc.zmask`; with `pc.bias` > 0 the class mask is written after it
  and replaces that value.

## `runtime/shaders/link_motion.frag`

The fragment shader of the per-object motion pass that checks the previous position's visibility
(`runtime/vk/vk_exec_draw.c` `link_motion_fs()`). Its input `previous`, at location 15, is the previous clip position:
`link_motion_fs()` patches that location in the compiled shader to the previous-position output of the game's vertex
shader, finding it by `OpDecorate ... Location 15`, which no other input of the shader has.

Binding 1, the block `Motion` (std140): `viewport` holds the size and the current jitter; `history` the previous
jitter and the previous projection's clip.w coefficients. Binding 2, `previousDistance`: last frame's view distances.
Output 0: the motion, the previous window position minus this fragment's (without the current jitter).

### `main()`

- A triangle seen from the other side last frame, or edge-on then, has no matching patch of visible surface: the
  fragment is discarded unless the area of the previous window position's derivatives is above 0.05, the previous w is
  positive and the previous position is inside the viewport.
- The distance expected at the previous position is |(previous.w - history.w) / history.z|, compared with the one
  `previousDistance` holds there.
- The tolerance accounts for subpixel sampling on sloped triangles, and is capped so a nearby occluder still wins:
  min(0.01 * expected, 0.0005 * expected + 0.75 * (|slope x| + |slope y|)), with the slope of the expected distance
  taken over last frame's window.

## `runtime/shaders/contact_shadow.comp`

Screen-space contact shadows (`runtime/vk/vk_contact.c`). Each pixel of the game's lit scene colour marches a short way
toward the sun through the linear depth target; when the ray passes behind something close in front of it, the pixel
is in a contact shadow the shadow maps are too coarse to show, and its colour is darkened. What switches it on:
[presets.md](../presets.md).

Bindings (workgroups of 8x8): 0 `lin_depth`, the game's linear depth: view distance / far; 1 `scene`
(`r11f_g11f_b10f`), the lit scene colour, darkened in place. Push constants (`Params`):

| Field | Meaning |
|---|---|
| `vec4 proj` | projection x scale, y scale, far plane, strength (0..1) |
| `vec4 light` | view-space direction toward the sun (xyz), march length in metres at 10 m (w) |
| `vec4 size` | width, height, 1/width, 1/height |
| `vec4 opt` | x: debug (1: write the shadow factor as grey) |

### `view_pos()`, `to_uv()`

`view_pos()`: the view-space point of a texture coordinate; `z` is the distance in front of the camera. `to_uv()` is
its inverse.

### `ign()`

Interleaved gradient noise: a per-pixel start offset that hides the step pattern.
fract(52.9829189 * fract(dot(px, (0.06711056, 0.00583715)))).

### `main()`

- Skipped: a depth of 0 or less, or 0.999 and more (nothing drawn, or the sky), and a distance above 80.
- The surface's normal comes from the depth of its neighbours (the nearer of each pair, so an edge takes the side the
  pixel is on), turned toward the camera. A surface turned away from the sun is already unlit in the game's own
  shading: a march from it runs into the surface itself at once, and darkening it again turned a character with the
  sun behind him brown and black. The march counts only on surfaces facing the sun, fading in over the first part of
  that range: smoothstep(0.05, 0.3, n . light).
- The march grows with distance, as one pixel covers more ground: its length is `light.w` * clamp(z / 10, 0.4, 3).
  The thickness, 0.15 + 0.01 z, keeps thin blades from hiding the sky.
- 16 steps (`STEPS`), the first at (0.25 + noise) of a step. The march ends when the point comes within 0.05 of the
  camera's plane or leaves the screen.
- `delta` is how far the scene at that pixel is in front of the ray point. A hit is a delta above 0.02 + 0.002 z and
  below the thickness. A hit close to the pixel shadows more than one at the end: 1 - step / `STEPS`.
- The darkening stands for the sunlight the pixel loses; the sky's and the bounced light stay, so even a full hit at
  full strength keeps part of the colour: `MAX_DARK` is 0.6. It fades out between 40 and 80 of distance.
- A factor of 0.999 and more writes nothing.

## `runtime/shaders/astc_decode.comp`

ASTC LDR 2D decode to RGBA8 on the GPU, one invocation per texel; `runtime/vk/vk_astc.c` records the dispatch. Its
first two lines are a licence notice, which stays in the code. It follows the CPU decoder, `runtime/gfx/tex_astc.c`,
so the output is the same bytes as `astc_decode_ldr()` there and as astcenc's 8-bit decode: endpoints expand to 16
bits (linear v * 257, sRGB v << 8 | 0x80), texel = (e0 * (64 - w) + e1 * w + 32) >> 6, top 8 bits (in the shader one
shift by 14). Error blocks are magenta (`MAGENTA`, 0xFFFF00FF); HDR endpoint modes give magenta endpoints for their
partition. `--probe-astc` (`runtime/gfx/astc_probe.c`) compares the two decoders bit for bit. The ASTC paths as a
feature: [architecture.md](../architecture.md); the decoder's notes: [gfx.md](gfx.md).

A workgroup of 64 lanes takes up to four blocks of one block row (`main()`): every lane decodes its block's header,
the block's lanes share the integer-sequence decode of its weights and colour values through shared memory, and each
lane then interpolates its own texels.

Bindings (set 0, std430): 0 `Blocks`, `uvec4 blk[]`, one entry a 16-byte block; 1 `Texels`, `uint px[]`; 2 `Tables`,
`uint tab[]`: uint 0..11 are the byte offsets of the weight unquantization rows (QUANT_2..QUANT_32), 12..27 those of
the colour rows (QUANT_6 up to the level before QUANT_256); the bytes follow. `runtime/vk/vk_astc.c` `fill_tables()`
writes them.

Push constants (`Params`; `Push` in `runtime/vk/vk_astc.c`):

| Field | Meaning |
|---|---|
| `uint in_base` | first block of slice 0 (uvec4 index into `blk`) |
| `uint out_base` | first texel of slice 0 (index into `px`) |
| `uint width, height` | the level's size in texels |
| `uint bw, bh` | block footprint |
| `uint bx` | blocks per row |
| `uint bslice` | blocks per slice |
| `uint srgb` | not 0: the sRGB endpoint expansion |
| `uint bl` | 1: `blk` holds the level's block-linear storage from `in_base` (`bl_block()`), 0: tight rows |
| `uint th, td` | its block height and depth in GOBs, log2 |
| `uint nvn2` | Switch 2 order of the 16-byte pieces in a GOB |

### `gob_off()`, `bl_block()`

`bl_block()`: the uvec4 index, from `in_base`, of block (bxi, byi) of slice z in block-linear storage: GOBs of 64 bytes
x 8 block rows, in blocks of 2^th GOBs high and 2^td deep, blocks in x, then y, then z order
(`runtime/gfx/tex_deswizzle.c` `nvn_bl_offset()`, with x the block's byte offset in its row and y its block row). A
GOB is 512 bytes; `gob_off()` is the offset inside one, in either console's order.

### `rbits()`

n (<= 32) bits at `pos`; bits past 127 read as zero.

### `q_bits()`, `q_trit()`, `q_quint()`

Quantization levels QUANT_2 .. QUANT_256 (0..20): the bits, trits and quints of the tables `q_bits`, `q_trits` and
`q_quints` in `runtime/gfx/tex_astc.c`, worked out instead of looked up.

### `ise_at()`

The raw ISE value `idx` (trit or quint << bits | bits) of a sequence of `count` values at bit `start`.
`runtime/gfx/tex_astc.c` `ise_decode()` reads the sequence front to back; this reads one value's group only. Bits of
values past `count` are not read.

- Trits: value k of a group has nb bits at k * nb + ts[k], then tb[k] trit bits; ts = {0, 2, 4, 5, 7}, tb = {2, 2, 1,
  2, 1}. A group is 5 values, 5 * nb + 8 bits.
- Quints: qs = {0, 3, 5}, qb = {3, 2, 2}. A group is 3 values, 3 * nb + 7 bits.

### `unpack_endpoints()`

`v`: the partition's unquantized colour values. False for an HDR mode.

### `header()`

`Hdr` is what every texel of a block needs from its header (`runtime/gfx/tex_astc.c` `astc_decode_block()` up to the
weight infill). `konst` is the colour of a void-extent or error block; `solid` says `konst` is the whole block.

- A void extent is a block mode whose low 9 bits are 0x1FC.
- The block mode follows `runtime/gfx/tex_astc.c` `block_mode_2d()`.
- Then the colour endpoint modes. A block whose colour values would be quantized below QUANT_6 (level 4) is an error
  block.

### `main()`

A workgroup decodes `bpw` blocks of one block row when a block has 64 texels or fewer (64 / texels of them), else one
block: `S` lanes per block (its texels, at most 64). The lanes of a block decode its weights and colour values into
shared memory, the first `pcount` lanes then unpack the endpoints, and each lane writes its texels.
`runtime/vk/vk_astc.c` works out the same blocks per workgroup for its dispatch.

| Shared | Holds |
|---|---|
| `MAXB` = 4 | blocks a workgroup takes at most: at 4x4, 4 blocks of 16 lanes |
| `s_w[MAXB][64]` | unquantized weights 0..64 (dual plane: interleaved as in the bit stream) |
| `s_cv[MAXB][18]` | unquantized colour values |
| `s_e0[MAXB][4]`, `s_e1[MAXB][4]` | 16-bit endpoints, two channels per uint |

## `runtime/shaders/bc3_encode.comp`

RGBA8 texels to BC3 blocks on the GPU, one invocation per 4x4 block: the second pass of the ASTC decode when
`NX2SARIA_ASTC_BC` is on (`runtime/vk/vk_astc.c`, which records the dispatch), so a decoded ASTC texture takes 8 bits
per texel in video memory instead of 32. Texels past the right or bottom edge repeat the last column or row.

Colour: the BC1 four-colour block of the box around the block's colours, its diagonal turned to the sign of the
colours' covariance and inset by 1/16 of the range (the bounding-box method), then each texel takes the nearest of the
four palette entries along that axis. Alpha: the eight-value BC3 alpha block between the block's smallest and largest
alpha. The bytes are what the texels hold, so an sRGB image keeps sRGB-encoded values, as BC3_SRGB expects.

The texel bytes are read as integers, so the alpha endpoints are exactly the block's least and greatest alpha: a texel
that is fully transparent or fully opaque keeps 0 or 255, wherever the source's own block edges fall inside the 4x4
block, and an alpha test keeps the outline of cut-out foliage. Through `unpackUnorm4x8` an opaque 255 could come back
as 254.99 on a GPU whose division is not exact and be cut to 254 by the conversion to uint.

With the specialization constant `BC1` (id 0) true (a pipeline of its own, `runtime/vk/vk_astc.c` `bc1_on()`) the
blocks are BC1, for a texture whose every texel is opaque ("ASTC as BC1" in `runtime/vk/vk_texture.c`): the colour
half alone, as BC1 reads the same four-colour block when c0 > c1, and when c0 == c1 every index is 0 (c0 in BC1's
three-colour mode too). The texels are the same as the BC3 block's, at half the size.

Bindings (set 0, std430): 0 `Texels`, `uint px[]`; 1 `Blocks`, `uint blk[]`: 4 words a BC3 block, 2 a BC1 one.
Workgroups of 8x8. Push constants (`Params`; `BcPush` in `runtime/vk/vk_astc.c`):

| Field | Meaning |
|---|---|
| `uint in_base` | first texel of slice 0 (index into `px`) |
| `uint out_base` | first block of slice 0 |
| `uint width, height` | the level's size in texels |
| `uint bx` | blocks per row |
| `uint bslice` | blocks per slice |

### `main()`

- Alpha: a0 > a1 selects the eight-value mode; index 0 is a0, 1 is a1, 2..7 the six steps from a0 to a1. The step `s`
  runs 0 = a0 .. 7 = a1. The 48 index bits start at bit 16 of the first word and run into the second.
- Colour: the box diagonal along the covariance of red and blue with green: where red (or blue) falls as green rises,
  that channel's ends are swapped.
- c0 > c1 is the four-colour mode, so the two are swapped when c0 < c1. The step `s` runs 0 = e0 .. 3 = e1; the
  palette order is e0, e1, 2/3 e0 + 1/3 e1, 1/3 e0 + 2/3 e1.

## `runtime/shaders/meshfb_scan.comp`

The mesh fallback's pass between a draw's task dispatch and its mesh dispatch (`runtime/vk/vk_mesh_fb.c`
`vk_meshfb_scan()`, `runtime/gfx/mesh_fb.c`): every task workgroup wrote how many mesh workgroups it starts into its
payload (word 1). This lists them in task order, so that mesh workgroup g finds its task and its index among that
task's in list entry g (the task in the low 24 bits, the index above them), and writes the draw's record: how many
mesh workgroups to dispatch and how many instances to draw. The fallback as a feature: "4. Graphics" in
[architecture.md](../architecture.md); its C side: [gfx.md](gfx.md).

One thread walks the tasks (a workgroup of 1): the order of the list is the order the primitives are drawn in, which
has to be the same in every frame and in the depth and colour passes of one frame.

The record: words 0-2 the task dispatch (x written before this pass), 4-6 the mesh dispatch, 8-11 the draw (vertices,
instances, first vertex, first instance), 12 the mesh workgroups the tasks asked for, 13 the task workgroups.

Bindings (set 0): 0 `Rec`, the record buffer; 1 `Task`, the task buffer; 2 `List`; 3 `Mesh`, the mesh buffer, read
for its size alone. Push constants (`Params`):

| Field | Meaning |
|---|---|
| `uint rec` | the record's first word in the record buffer |
| `uint task_words` | words of a task workgroup's payload in the task buffer |
| `uint fan` | mesh workgroups a task workgroup starts at most; bit 31: the clamp wrote word 13 (the task workgroups asked for before it cut the dispatch, `runtime/shaders/meshfb_clamp.comp`), which stays |
| `uint slot_words` | words of a mesh workgroup's output in the mesh buffer |
| `uint verts` | vertices drawn per mesh workgroup |
| `uint max_groups` | workgroups one dispatch may have (`maxComputeWorkGroupCount`) |

### `main()`

The tasks walked are the count asked for, cut to what the task buffer holds and to 2^24. The room is what the mesh
buffer and the list hold, at most `max_groups`. A task starts at most min(`fan`, 255) mesh workgroups. Word 12 gets
what the tasks asked for, words 4 and 9 what fits.

## `runtime/shaders/meshfb_clamp.comp`

The mesh fallback's pass between a draw's record and its first dispatch, for a draw whose workgroup count the GPU wrote
(`runtime/vk/vk_mesh_fb.c` `vk_meshfb_clamp()`, `runtime/vk/vk_exec_draw.c` `mfb_draw()`). The record's copies put
that count into the first dispatch and, without a task stage, into the draw's instances as it is; a garbage count (the
game's culling pass reading memory nothing wrote) would launch up to millions of workgroups that write nothing and
instances that draw nothing, past the buffers' end, and keep the GPU busy past the driver's timeout. The count is cut
to what the buffers hold, which changes nothing that is drawn; the record's report word keeps the count asked for, so
the log names such a draw (`runtime/vk/vk_mesh_fb.c` `record_check()`). For a task count that word (13) is written
here, before the cut: the scan would read the count already cut (`runtime/shaders/meshfb_scan.comp` leaves it to this
pass then), and a batched draw short of its task part would never be seen (`runtime/vk/vk_mesh_fb.c`
`record_check()`, `runtime/gfx/mfb_batch.h` `mfb_hist_note()`).

The record is the one of `runtime/shaders/meshfb_scan.comp`: words 0-2 the task dispatch, 4-6 the mesh dispatch, 8-11
the draw (vertices, instances, first vertex, first instance), 12 the mesh workgroups asked for, 13 the task
workgroups.

Binding 0 (set 0): `Rec`, the record buffer. A workgroup of 1. Push constants (`Params`):

| Field | Meaning |
|---|---|
| `uint rec` | the record's first word in the record buffer |
| `uint task` | 1: the count is the task dispatch's (word 0); 0: the mesh dispatch's and the draw's instances (4, 9) |
| `uint room` | workgroups the buffers hold, at most what one dispatch may have |

## The mesh fallback probe's programs

The four shaders of `--probe-meshfb` (`runtime/gfx/mesh_fb_probe.c`). The three programs keep the line
`// build_shaders: keep functions`: they are in the shape the translator gives a program (`tools/xlate/MeshSpirv.cs`),
and `runtime/gfx/mesh_fb.c` makes its compute module from each, as it does from a translated program.

### `runtime/shaders/meshfb_probe_m.mesh`

A mesh program without a task stage: the program itself is a function (`program()`) that writes its stage buffer into
shared memory, and the entry function calls it once and hands the buffer to the mesh pipeline. The layout is that of
`MeshInfo` in `tools/xlate/AmpereLowering.cs` for 64 vertices, 32 triangles and 8 output components (position, generic
vector 0): vertex v's component l is word ((v >> 5) * 8 + l) * 32 + (v & 31) (`VWORD`), the primitive count word 512
(`COUNT_WORD`), the index bytes from byte 2052 (`INDEX_BYTE`). The shared memory is 540 words.

Workgroup g draws cell g of a grid: nine vertices (28 to 36, across the two vertex groups) and up to eight triangles,
none in every fifth cell. One thread writes the index bytes: they share words.

Binding 1 (set 0): `Params`, `vec4 tint; uvec4 grid`, with the grid's columns and rows.

### `runtime/shaders/meshfb_probe_tm.mesh`

The mesh program that `runtime/shaders/meshfb_probe_t.task` feeds, in the same shape. Its shared memory (612 words)
holds the task payload behind the stage buffer, from word 540 (`PAYLOAD`). The workgroup id is its index among its
task's mesh workgroups: payload word 4 + id names the cell it draws, and the mark in the last payload word (71) names
its task, which the colour shows when either is wrong (magenta). As in the other mesh program, one thread writes the
index bytes, which share words; `Params` is the same block.

### `runtime/shaders/meshfb_probe_t.task`

A task program in the same shape: the program writes its payload into shared memory, and the entry function copies
that out and starts the mesh workgroups the program counted into word 1. The payload is 288 bytes (72 words): the
header, then the program's data from word 4.

Task workgroup t starts 0 to 4 mesh workgroups (the uniform block's `shift` moves which tasks start how many: (3 t +
shift) mod 5) and tells each the grid cell it draws (4 t + its index); the last payload word carries a mark the mesh
program checks, 0x600df00d + t.

### `runtime/shaders/meshfb_probe_f.frag`

The fragment stage of the probe's mesh programs: the interpolated generic vector 0.

## `runtime/shaders/wb_merge.comp`

Write-back merge of a VRAM mirror (`runtime/vk/vk_memory.c` `wb_merge_record()`): copies a 32-bit word of what the
GPU left in the mirror into the pool's imported buffer (guest memory) only where it differs from the snapshot taken
before the submission's GPU work, so words the GPU did not change keep what the CPU wrote meanwhile. The memory model
it serves: [maxwell.md](../maxwell.md).

One invocation per word of the listed ranges (workgroups of 64); ranges are sorted by their first invocation, and an
invocation finds its range by bisection. A range: x the word offset in the mirror and imported bindings, y the word
offset in the snapshot binding, z the words, w the first invocation.

With `track` set (shadow storage) a word that changed also marks its 4 KiB page of guest memory in `Pages`: `page0` is
the bindings' start within its page, so the CPU's write-back at the fence reads only the marked pages of the shadow
(`runtime/vk/vk_memory.c` `shadow_wb_cb()`).

Bindings (set 0, std430): 0 `Post`, the mirror; 1 `Pre`, the snapshot; 2 `Dst`, the imported buffer; 3 `Ranges`,
`uvec4 rng[]`; 4 `Pages`. Push constants: `nranges`, `total` (invocations with a word), `page0`, `track`.

## `runtime/shaders/wb_mergelist.comp`

Write-back merge into a list (`runtime/vk/vk_memory.c` `wb_merge_record()` with `NX2SARIA_MERGE_CPU`): as
`runtime/shaders/wb_merge.comp`, a word the GPU left different from the snapshot taken before the submission's GPU
work goes back to guest memory, but as an entry (its place among the submission's list words: `gbase` + invocation,
the value) in a device-local list, which `runtime/shaders/wb_listcopy.comp` copies to host memory in whole lines and
the completion thread stores (`runtime/vk/vk_memory.c` `merge_apply()`) before the fence counts as done.

Why: `wb_merge.comp`'s word-sized writes into host memory across PCIe cost the memory controller a read-modify-write
of their line each: 0.9-1.0 ms of GPU a frame in Kokiri Forest for 0.6 MiB of scattered words.

Past the list's room (`pc.cap`) a word is written directly, as `wb_merge.comp` does. With `pc.check` set
(`NX2SARIA_MERGE_CPU_CHECK`) the GPU writes the listed words too.

One invocation per word of the listed ranges (workgroups of 64); the ranges are those of `wb_merge.comp` (x: word
offset in the mirror and imported bindings, y: word offset in the snapshot binding, z: words, w: first invocation).
It needs `GL_KHR_shader_subgroup_basic` and `GL_KHR_shader_subgroup_ballot`: a subgroup counts its changed words with
a ballot, one of its invocations takes that many entries from the list's counter, and each changed word takes its
place among them. The count is the same in every invocation of the subgroup, so a subgroup without a changed word
leaves together.

Bindings (set 0, std430): 0 `Post`, 1 `Pre`, 2 `Dst`, 3 `Ranges`, as the merge's; 4 `List`: `uint n` and three pad
words, then `uvec2 ent[]`. Push constants: `nranges`, `total`, `gbase`, `cap`, `check`.

## `runtime/shaders/wb_listcopy.comp`

Copies a merge list (`runtime/shaders/wb_mergelist.comp`: a 16-byte header with the count, then 8-byte entries) from
device-local memory to host memory for the completion thread (`runtime/vk/vk_memory.c` `merge_apply()`): 16 bytes an
invocation, the invocations of a subgroup consecutive, so the writes across PCIe go out in whole lines.

Bindings (set 0, std430): 0 `Src`, 1 `Dst`, both `uvec4[]`. Workgroups of 256. Push constant: `cap`, the list's room.
The 16-byte chunks copied are 1 + (n + 1) / 2: the header and n entries.

## `runtime/shaders/wb_diffcopy.comp`

Write-back by difference of a VRAM mirror's large written ranges (`runtime/vk/vk_memory.c` `wb_merge_record()` with
`NX2SARIA_WB_DIFF`). 16 invocations cover a 64-byte line of consecutive words; when the GPU changed any word of it
against the snapshot taken before the submission's GPU work, they copy the whole line, else nothing. Whole lines keep
the writes into guest memory across PCIe in full packets and spare the memory controller a read-modify-write (the
merge's word-sized writes cost it); a line the GPU left alone keeps what the CPU wrote meanwhile.

One invocation per word (workgroups of 256); ranges sorted by their first invocation (x: word offset in the mirror and
imported bindings, y: word offset in the snapshot binding, z: words, w: first invocation, w = x mod 32 so that the
clusters of 16 line up with the 64-byte lines of the 128-byte aligned bindings; the invocations before a range's w are
idle). An invocation past its range's words (the padding before the next one) is idle too. It needs
`GL_KHR_shader_subgroup_basic` and `GL_KHR_shader_subgroup_clustered`.

Bindings (set 0, std430): 0 `Post`, 1 `Pre`, 2 `Dst`, 3 `Ranges`, as the merge's. Push constants: `nranges`, `total`.
