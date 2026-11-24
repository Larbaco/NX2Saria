# tests (second part)

The tests from `tests/make_game_gate_test.py` on, in the order of their file names. A check that is a Python file and a
C file of the same name has one section, under the Python file: the Python file builds the C file with the header or
source it names and runs it; the C program prints each failed check with its line and exits 1 on any. Where such a
check has a `compiler()`, it takes `$CC`, else MSYS2 UCRT64's clang, else the clang on `PATH`, as
`tests/diff/common.py` picks it but without needing Unicorn. "No GPU, no game files" is said once per section where it
holds.

## `tests/make_game_gate_test.py`

`tools/make_game.py`'s compiles through the memory gate (`tools/memgate.py`), with a stand-in script as the compiler
(it writes the object and its dependency file after a short wait, so that compiles overlap). No network, no game files.

- `compile_all` starts the compiles in the order of their cost (a lifted chunk's from the lift's manifest, another
  source's size) or of the seconds an earlier build measured, and the gate admits them in that order. In the case
  `big.c` is the largest file but the cheapest chunk and `rt.c` is not lifted, so its size is its cost; with no free
  memory the first compile starts (`min_jobs`) and the next is held for memory. A source that is not a lifted chunk is
  saved with cost 0, which keeps it out of the size model's scale.
- A learned time orders before the cost: with `b.o` 1 s, `c.o` 2 s and `d.o` 10 s measured last time, their median rate
  (`c.o`'s, 1 s per 1000 bytes of cost) puts `a.o`, not built before, at 4 s.
- A stop ends the wait of a compile held for memory at once; neither it nor the one behind it compiles.
- A peak measured while the free memory was below the 1 GiB reserve keeps a higher record (64 GiB planted, far above any
  real peak); one measured without pressure replaces it.
- Each successful compile's peak and time go into the estimates, which are saved; a compile the gate held shows in its
  summary; the lines the build prints about the gate (the closing lines with one peak at half its estimate and one at
  twice).
- `chunk_costs` reads `cost` and `switch_fn` from a module's `manifest.json`, `bytes` from a manifest older than `cost`,
  and a file's size when the manifest does not list it.
- `LifterEnvTest`: `recomp/gen.py` `lift_gate()` keeps the compiles' reserve and follows `--no-mem-gate`; without
  options the environment passes as it is.

## `tests/make_game_sdl3_test.py`

`tools/make_game.py` on Linux with or without the system's SDL3 (issue #11: Ubuntu 24.04 and Linux Mint 22 package
none). No network, no game files. `stopped()` gives (the value, '') of a call, or (None, the error text
`make_game.fail` printed).

- `sdl3_choice` takes the system's SDL3 when the compiler finds its headers, so a distribution that packages SDL3
  builds with the same commands as before (the link line of before, so nothing links again), else the build kit's
  (`tools/sdl3_pin.py`). `NX2SARIA_SDL3` = `kit` or `system` decides instead (the older spelling `NX2NAVI_SDL3` is read
  too), and a wrong value or a choice that cannot be met stops with the reason.
- The kit's SDL3 goes ahead of the system's folders for the runtime's sources only, the link names the kit's library by
  its path, and the check's program runs with the library beside it, as the game will. The link key names the kit by
  its place in the kit, not by the folder an AppImage is mounted in this time; with the setting unset `sdl3_tag()` is
  empty, the compile key of every game built so far.
- The kit's `libSDL3.so.0` goes beside the game, and goes away again when the game is linked against the system's (the
  runpath would load the kit's copy ahead of the system's); a copy with the kit's bytes is still the kit's after the
  build cache was deleted; a `libSDL3.so.0` this build did not put there stays, with a line that says so.
- `PreflightWithTheKitSdl3Test` runs `make_game.preflight` with every command recorded instead of run: the check's
  program is built and run in the build cache, not in the system's temporary folder; a program that does not start for
  want of a library the kit's SDL3 needs names it and the package that holds it; one ended by a signal leaves nothing
  on its stderr; headers without a library point to the kit's, and there is no program to run without its link.
- `Sdl3VersionTest` compiles `make_game.SDL3_PROBE` against headers that declare what it uses as `SDL3/SDL.h` does.
- `KitsCarrySdl3PinTest`: `make_game.py` imports `sdl3_pin` inside a `try`, so a kit without it would fail only on a
  Linux system without SDL3, asking for a package Ubuntu 24.04 does not have. The zip kit lists it, and the AppImage's
  kit copies all of `tools/` (`tools/make_appimage.py`) and carries the files besides.
- The build log's line names the compiler, its version and where SDL3 came from.

## `tests/maxwell_warp_emu.py`

A 32-lane Maxwell warp emulator for checking the SM86 tensor lowering on the CPU. It is not a test:
`tests/tensor_lowering_check.py` runs the full check of the tensor lowering on top of it (guards, untouched registers,
several initial fills) and `tests/maxwell_warp_emu_selftest.py` checks the emulator itself.

`tools/xlate/AmpereLowering.cs` rewrites SM86 instructions as Maxwell (64-bit) instructions, and the patched shader
translator turns those into SPIR-V. This file executes the Maxwell words the way that translator does: fields are
decoded exactly as its `Decoders/InstDecoders.cs`, opcodes are matched with the bit patterns of
`Decoders/InstTable.cs`, and each instruction does what its `Instructions/*.cs` emit for it, not what a real Maxwell
GPU does where the two differ. With it, the result of one lowered SM86 instruction (HMMA, LDSM, MOVM, HFMA2.RELU, ...)
can be compared with vectors captured on real hardware without running the game.

State per lane: R0-R254 (RZ = R255 reads 0, writes are dropped), P0-P6 (PT = P7 is true, writes dropped), the lane id,
local memory; per warp: shared memory. Every lane is active at the start; a guard (bits 16-19: predicate 16-18,
inverted 19) masks the writes of one instruction, EXIT retires lanes. Branches must be uniform across the active lanes.

Translator semantics it follows, and real hardware may not:

- Half-precision arithmetic (HADD2, HMUL2, HFMA2) works in F32 on the unpacked halves and rounds to F16 once at the end
  (`InstEmitFloatArithmetic` `EmitHadd2Hmul2` / `EmitHfma2`, `InstEmitHelper` `GetHalfPacked`: `PackHalf2x16`). HFMA2
  is therefore an F32 fused multiply-add rounded to F32, then to F16.
- That rounding is skipped when the packed value goes straight into another unpack: `Optimizer.PropagatePack` turns
  `UnpackHalf2x16(PackHalf2x16(a, b))` into a (or b). So F2F.F16.F32 then F2F.F32.F16 of the same register, or HFMA2
  feeding HFMA2, passes the F32 value on unrounded; an integer instruction (SHFL, LOP, ISCADD, ...) in between keeps
  the rounding, a MOV does not. Modelled per register (`Warp.halves()`); a guarded write and a branch end it.
- FTZ, FMZ and the rounding-mode fields of FFMA / FMUL / FADD / HFMA2 / HMUL2 are ignored, as the translator ignores
  them. Denormals are kept (IEEE); saturation is clamp(x, 0, 1) with NaN giving 0 (GLSL `FClamp` as
  min(max(x, 0), 1) on a GPU whose min/max return the non-NaN operand).
- PRMT is not implemented in the translator (1.3.2): `InstEmit.PrmtR/PrmtI/PrmtC/PrmtRc` only log "not implemented",
  so the destination keeps its old value. That is the default of the `prmt` argument; `prmt="maxwell"` gives the
  hardware byte permute instead, to see what the lowering intended.
- SHFL follows the translator's `HelperFunctionManager` `GenerateShuffle*` helpers (clamp and segment mask from operand
  C, the lane's own value where the source lane is out of range).
- LDS / LDL read the 32-bit word at (address >> 2) and extract a sub-word at (address & 3) * 8 (`InstEmitMemory`
  `EmitLoad`); STS of 8 and 16 bits stores those bytes at the address.
- Condition codes (.CC, .X) are not modelled: an instruction that writes or reads them raises.
- NaN results carry numpy's payload and sign. Neither is defined in SPIR-V, so result comparisons (`same_value()`) let
  a NaN match any NaN for float results.

Library use: `run(words, regs_in, smem=None)` takes Maxwell 64-bit words and returns a (32, 256) uint32 array (row =
lane); `run_warp(words, regs_in, smem=None)` returns the whole `Warp` (regs, preds, smem, warnings). Command line, for
vectors written by the tensor ground-truth capture:
`python tests/maxwell_warp_emu.py <vectors.json> [--xlate EXE] [--prmt ryujinx|maxwell] [--case NAME] [--trace] [--fill HEX] [--exact]`.
In `main()` one broken case does not stop the run.

`UnknownOpcode` is a Maxwell word the emulator has no decoder entry for; `Unsupported` a known instruction in a mode or
form it does not implement.

### Bit-level helpers

- `u32()`: a lane vector (or broadcast scalar) as uint32, wrapping Python ints to 32 bits. `sext()`: sign-extends a
  bits-wide field to a Python int.
- `half_lo()` is `UnpackHalf2x16Low`: the low 16 bits as F16, widened to F32 (exact); `half_hi()` the high ones.
  `pack_half()` is `PackHalf2x16`: each F32 rounded to F16 (round to nearest even, numpy's conversion), lo in bits
  0-15.
- `fma_f32()`: F32 fused multiply-add, rounded once (`FPFusedMultiplyAdd` -> GLSL.std.450 `Fma`, fused on the target
  GPUs). a * b is exact in F64 (2 x 24 significant bits). The F64 sum s may be inexact; TwoSum gives its error e
  exactly (s + e == a * b + c). Rounding s to F32 is then correct except when s lies exactly halfway between two F32
  values, where the sign of e decides the direction: the code takes the F32 neighbour of r on the side of s when e
  points towards it. F32 halfway points are F64 values, so no other case can double-round.
- `saturate()` is `FPSaturate`: clamp to [0, 1], NaN to 0 (fmax/fmin return the non-NaN operand). `abs_neg()` is
  `FPAbsNeg`: abs first, then negate (sign-bit operations in SPIR-V; numpy's abs/negative act on the sign too).

### `Ins`

One Maxwell word with the field accessors `InstDecoders.cs` uses, the fields shared by most decoders first.
`Ins.imm20()` is `InstDecoders` `Imm20`: bits 20-38, bit 19 from bit 56. `Ins.imm20_sint()` is `InstEmitHelper`
`Imm20ToSInt`; `Ins.imm20_float()` is `Imm20ToFloat`.

### `Warp`

Registers not given by the caller start as `fill` (SPIR-V locals are undefined unless the job zeroes them; a poison
value such as 0x7fc0dead shows reads of registers the lowering never wrote).

The translator's `Optimizer.PropagatePack` turns an `UnpackHalf2x16` of a `PackHalf2x16` result into the pack's F32
operand, so that half is never rounded to F16. Kept per lane and register: the F32 halves of the pack that wrote the
register (`sh_ok`), until the register is written otherwise. A write under a guard ends it in every lane: the
translator puts the instruction in an if-block and merges old and new value in a phi, which is no pack. `unconditional`
says the current instruction's guard is PT (`run_warp()` sets it).

Register file access: `R(n)` reads a lane vector, `W(n, v, mask)` writes the lanes in mask. `halves`: the F32 values a
`PackHalf2x16` rounded into v; `copy_from`: v is a copy of that register (MOV), pack and all. `Warp.halves()` gives
`UnpackHalf2x16Low` / `High` of register n as F32, after PropagatePack (the pack's operand where one wrote n).
`Warp.cbuf_word()` is c[slot][word_offset * 4] (`InstDecoders` `CbufOffset` counts words).

### Instruction handlers

Each handler takes (warp, ins, mask): mask = the active lanes whose guard holds. Lanes outside mask must not change.
What each follows in the translator:

| Handler | Follows |
|---|---|
| `op_mov_r()` | `InstEmitMove` `MovR`: SrcA in bits 20-27; a Copy, which PropagateCopy sees through |
| `op_mov_i()` | `MovI`: `GetSrcImm(op.Imm20)`, the raw 20-bit field (not shifted or sign-extended) |
| `_iadd()` | `InstEmitIntegerArithmetic` `EmitIadd`: AvgMode 1 negates B, 2 negates A |
| `_iadd3()` | `EmitIadd3` `Extend`: HalfSelect 1 = low 16 bits, 2 = high 16 bits |
| `_iscadd()` | `EmitIscadd`: (A << shift), then the AvgMode negations, then A + B |
| `_shl()` | `InstEmitShift` `EmitShl`: .M masks the count to 5 bits, else counts >= 32 give 0 |
| `_shr()` | `EmitShr`: .BREV (40) reverses A first, .S32 (48), .M (39) |
| `_lop_pred()` | `EmitLopPredWrite` |
| `_lop()` | `InstEmitIntegerLogical` `EmitLop` |
| `op_lop32i()` | `Lop32i`: no predicate output (DestPred PT) |
| `_lop3()` | the LOP3 truth table (`Lop3Expression.GetFromTruthTable`): bit (a << 2 \| b << 1 \| c) of lut, per bit |
| `_prmt()` | `InstEmit.PrmtR` / `PrmtI`: "not implemented", nothing is emitted, the destination keeps its value |
| `op_sel_r()` | `InstEmitMove` `EmitSel`: P ? A : B |
| `_isetp()` | `InstEmitIntegerComparison` `EmitIsetp` (not extended) |
| `_p2r()`, `op_p2r_i()` | `InstEmitPredicate` `EmitP2r`; `P2rI` takes the raw Imm20 as mask |
| `_r2p()`, `op_r2p_i()` | `InstEmitMove` `EmitR2p`; `R2pI` takes `Imm20ToSInt` |
| `op_s2r()` | `InstEmitMove` `S2r`: SR_LANEID; one warp, so TID.X is the lane and TID.Y/Z are 0 |
| `op_shfl()` | `InstEmitWarp` `Shfl` + `HelperFunctionManager` `GenerateShuffle*` (subgroup size 32); modes 0 IDX, 1 UP (signed compare, `ICompareGreaterOrEqual`), 2 DOWN, 3 BFLY. A predicated SHFL becomes an if-block in SPIR-V: reading a lane outside it is undefined |
| `_mem_load()` | `InstEmitMemory` `EmitLoad`: per word, the word at (address >> 2), sub-words extracted at (address & 3) * 8 |
| `_mem_store()` | `EmitStore`: 32-bit words at (address >> 2); shared 8/16-bit stores write those bytes at the address, local ones insert into the word at (address & 3) * 8. Lanes store in lane order (the last lane wins a conflict) |
| `op_f2f_r()` | `InstEmitConversion` `F2fR` / `EmitF2F`; the rounding field is `IntegerRound`: 4 round, 5 floor, 6 ceil, 7 trunc; an F16 result is `WriteFP`: `PackHalf2x16(x, 0)` |
| `op_mufu()` | `InstEmitMultifunction` `Mufu`: `FPAbsNeg` of Ra, the function in F32, `FPSaturate` |
| `_ffma()`, `op_ffma32i()` | `EmitFfma` (rounding mode and FMZ ignored); `Ffma32i`: C is the destination register |
| `_fmul()` | `EmitFmul`: A times the scale first, then A * B |
| `_fadd()` | `EmitFadd` |
| `_hadd_hmul()` | `EmitHadd2Hmul2`: F32 add/multiply per half, saturate, pack |
| `_hfma()` | `EmitHfma2`: F32 fused multiply-add per half, saturate, pack |
| `op_hmul2_r()` | `Hmul2R`: the NegA bit (31) negates B |
| `op_hfma2_r()` | `Hfma2R`: NegA (31) negates B, NegC (30) |
| `op_hfma232i()` | `Hfma232i`: C is the destination register, H1_H0 |

Half precision follows `InstEmitHelper` `GetHalfSrc` / `GetHalfUnpacked` / `GetHalfPacked`:

- `half_src()`: the two F32 values of a packed-half register operand: swizzle 0 (H1_H0), 1 (F32: the register as one
  F32 twice), 2 (H0_H0), 3 (H1_H1); then abs, neg. The halves come through `Warp.halves()` (PropagatePack).
- `half_imm_pair()`: `GetHalfSrc(immH0, immH1)`: each 10-bit field shifted left 6 as an F16.
- `half_pack()`: `GetHalfPacked`: 0 F16 pair, 1 F32 (lane 0's result), 2 MRG_H0 (high half kept), 3 MRG_H1 (low half
  kept). Returns the register value and the F32 halves `PackHalf2x16` rounded (None for the F32 format). The old halves
  are `GetHalfDest`: an `UnpackHalf2x16` of Rd.

Control flow is handled by the executor: EXIT retires lanes, BRA moves the warp.

### `decode()`

(name, handler) of a Maxwell word. The patterns are written as the translator's `Decoders/InstTable.cs` writes them
(bit 63 first, x = any). The most specific matching pattern wins, as `InstTable`'s XBits ordering does.

### `slot_address()`

Byte address of instruction k in a compute program as `AmpereLowering.Lower` lays it out (a scheduling word first in
every 32 bytes; `--lower` prints the instruction words only).

### `run_warp()`, `run()`

`run_warp()` executes the Maxwell words on one warp and returns its final state; `regs_in` in any form `load_regs()`
takes. Where control flow joins (branch targets) the translator merges registers in phis, and an `UnpackHalf2x16` of a
phi is not folded: the PropagatePack state (`Warp.sh_ok`) is dropped there and at every BRA (conservative: a register
no path writes needs no phi).

### Vector files and the `--lower` bridge

- `parse_int()`: an int, or a hex string (with or without 0x). `parse_reg()`: ("R", n) or ("P", n) for a register name:
  an int, "R12", "12", "RZ", "UR3" (needs urbase), "P0", "PT".
- `load_regs()` normalises register specs to {lane: {(kind, n): int}}. It accepts {lane: {reg: value}} with lane keys
  as ints or strings (and "*" / "all" for every lane, applied first so per-lane entries override it), or a list with
  one entry per lane; each lane entry a dict {reg: value} or a list of values indexed by register number; values ints
  or hex strings. `_normalised()` passes `load_regs()` output through unchanged (keys (kind, n)) and loads anything
  else.
- `lower()` runs `nx2navi_xlate --lower` on SM86 instructions (a list of [w0, w1, w2, w3] or one such list) and returns
  (Maxwell words, urbase, local bytes).
- `_fmt()`: a register value as hex plus its F32 and half-pair readings.

### Comparing results

A float result is right when its bits are, except that any NaN matches any NaN: SPIR-V leaves the payload and sign of a
NaN to the host (Vulkan spec, "Precision and Operation of SPIR-V Instructions"), and this emulator's numpy arithmetic
picks its own, while SM86 hardware writes 0x7fff for every F16 NaN result. Whether a value is NaN is the lowering's
doing; which NaN is not. Moves (LDSM, MOVM) and integer results compare bit for bit.

- `result_kind()`: how the destination registers of an SM86 instruction compare: "f16x2" (two F16 values), "f32" or
  "bits". `sm86` is [w0, w1, w2, w3] or a list of them (the last one decides). Opcode 0x23C is HMMA, with .F32 (bit 76)
  or .F16 results; (op & 0x1FF) in 0x030, 0x031, 0x032 is HADD2 / HFMA2 / HMUL2, with F32 results when bit 78 is set.
- `same_value()`: got matches want: equal bits, or for float kinds NaN where want is NaN.
- `ulps()`: distance in units in the last place (the larger of the two halves for "f16x2"); 0 for matching NaNs, a
  huge number for NaN against a number. +0 and -0 are 0 apart (`same_value()` still tells them apart).
- `run_case()` lowers, runs and compares one vector case and returns (ok, lines of report). Float results compare with
  `same_value()` unless exact. LDSM cases: "lane_addresses" hold Ra per lane when `regs_in` does not, relative to
  "smem", which sits at shared address "smem_base". Shared memory is padded to 64 KiB so stray reads stay in range.

## `tests/maxwell_warp_emu_selftest.py`

Self-test of `tests/maxwell_warp_emu.py`: hand-encoded Maxwell sequences against hand-computed results. The encoders
use the field positions of the translator's `Decoders/InstDecoders.cs`; `PT` is the guard field for predicate PT
(7 << 16), and `isetp_i()` encodes ISETP.cmp Pd, PT, Ra, imm, PT (AND).

`part1()` runs words encoded in the file:

- IADD32I, MOV32I, IADD3 with a negated operand, ISCADD; S2R lane id, LOP32I, SHL/SHR, LOP3, SHR by register.
- SHFL.IDX: the whole warp by a lane register ((lane + 1) % 32); a segment of 4 lanes (0x1C03: lane (t & ~3) | 1);
  segments of 8 with clamp 3 (0x1803), where index 5 is invalid and index 2 valid; BFLY 1; DOWN 2 with clamp 0 and
  with 0x1F; UP 1. Out of range lanes keep their own value.
- STS / LDS: 32, 16 and 8 bits, sign extension, 64 and 128 bits, the immediate offset (word [0x100 + 4t] = t, then
  R12-R15 = words 0..3 of the table).
- F2F: F32 from either F16 half (exact), F16 from F32 (round to nearest even, high half zero): 1 + 2^-11 is a tie and
  goes to even (1.0), 1 + 3 * 2^-11 to 1 + 2^-9, 70000.0 overflows to inf.
- HFMA2: per half F32 FMA then F16; neg C; saturate. (1 + 2^-10)^2 = 1 + 2^-9 + 2^-20 -> F16 1 + 2^-9; 1 + 2^-11 is an
  F16 tie -> 1.0.
- `Optimizer.PropagatePack`: an F16 result unpacked again without an integer operation in between is the unrounded F32
  value, directly or through a MOV; a LOP in between, or a guarded pack (P0 = lane < 32, true everywhere), keeps the
  F16 rounding. HFMA2 feeding HFMA2: 40000 * 2 is inf as F16, times 0.5 again.
- FFMA: rounded once. The exact a*b + c lies just below an F32 halfway point; F64 alone would round to the tie and then
  to even (0x3f800002), the fused result is 0x3f800001.
- PRMT: a no-op as the translator translates it, the Maxwell byte permute otherwise (0xbb99: sign of bytes 1 and 3).
- Predicates: ISETP (P0 = lane < 16), a guarded write, an inverted guard, EXIT of some lanes (lanes >= 16), a uniform
  BRA over one instruction (the skipped one would write 0xDEAD).
- Unknown opcodes raise with the word.

`part2()`, when `build/xlate/nx2navi_xlate.exe` exists, lowers SM86 instructions with `nx2navi_xlate --lower` and
checks the emulated result against exactly representable reference values (small integers, so every F16 rounding is
exact and only the fragment layout can be wrong). `tests/tensor_lowering_check.py` checks the same lowerings against
hardware results.

- `hmma_case()`: HMMA.{1688,16816}.F16 R4, R8, R12, R16 with small integers; returns (sm86, regs_in, expected D regs).
- LDSM.16.M88.4 R4, [R1] and LDSM.16.MT88.1 R8, [R1]: matrix m row r at 0x200 * m + 0x20 * r (16 bytes used), element
  (m, row, col) = 0x1000 * m + 0x100 * row + col. Transposed, a lane t holds column t/4 of rows 2(t%4) (low) and
  2(t%4)+1 (high); else row t/4, columns 2(t%4) and 2(t%4)+1.
- MOVM.16.MT88 R2, R1: element (row, col) = 8 * row + col at lane 4 row + col / 2, half col % 2.
- HFMA2.RELU R0, R1, R2, R3 (PT) as the translator runs it, whatever the scratch registers held before: the translator
  drops PRMT, so `tools/xlate/AmpereLowering.cs` `Half` builds the mask with integer operations. Negative sums become
  +0, positive ones stay (sums -2.0 and -0.75; 4.0 and -0.75).

## `tests/mem_budget_check.py`

Builds and runs `tests/mem_budget_check.c` on `runtime/mem_budget.h`, which turns the free share of the video memory
and of the system memory into a level (roomy, normal, tight, critical) with hysteresis, for the owners of memory that
can be given back. No GPU, no game files, any Python.

- The room itself: 8000 of 32000 free is 250 per mille; use past the limit and an unknown limit give no room;
  `mem_room_min` takes the tighter share and the smaller amount.
- Levels from a first sample, from roomy, as `runtime/mem_budget.c` starts (`room_pm()` is a share on a 32 GiB side):
  the lines at 40%, 20% and 8% belong to the level below; a large share of a tiny side (600 per mille, 200 MB) is
  critical, since less than `MEM_FLOOR_MB` free is critical at any share.
- Down at once, up only past the line by `MEM_HYST_PM`; a share that hovers around the 20% line (18.5% to 21.5%)
  changes the level once, not at every sample.

## `tests/mem_commit_launcher_test.py`

The launcher commit warning's pure threshold and message, with the system probe and the dialog stubbed. Yes is the
default button: `MessageBoxW` returns 6 (IDYES) and the flags carry neither `MB_DEFBUTTON2` nor `MB_DEFBUTTON3`
(`flags & 0x300` is 0).

## `tests/mem_commit_message_check.py`

Builds and runs `tests/mem_commit_message_check.c`, the pure Win32 commit-message selector; no Windows or game files
are needed. The advice is given for 1455 (`ERROR_COMMITMENT_LIMIT`), 8 (`ERROR_NOT_ENOUGH_MEMORY`) and 1450
(`ERROR_NO_SYSTEM_RESOURCES`), and not for 14 (`ERROR_OUTOFMEMORY`) or 487 (`ERROR_INVALID_ADDRESS`).

## `tests/memgate_test.py`

`tools/memgate.py`: the admission rule, the in-process `Gate` over fake free memory, resident sizes and clock (`Fake`:
free memory, resident sizes by pid, a clock and a log the tests set), the `Estimates` file, `run()`'s exact peak on a
real small child, and the `wrap` command line end to end with two wrappers at once. No game files; stdlib only; the
children allocate at most about 50 MB each (`ALLOC`: 50 MB, written, so resident).

- `AdmitTest`, the rule both gates share: owed is need less what is resident (2 GiB needed, 0.5 GiB resident: 1.5 GiB
  still owed); a compile without a pid yet owes all its need.
- `GateTest` (`GateTest.background()` acquires in a thread and appends the lease to `done` once admitted). Owed memory
  holds until the compile is resident: with 1 GiB of a 2 GiB need resident and already counted in the free memory, 3
  GiB free less 1 GiB still owed covers 2 GiB. Reserve and ceiling: a 5 GiB compile the memory holds is still over the
  limit by the 1 MiB, and runs alone by `min_jobs`. Unknown free memory admits what the RAM holds (three compiles of 3
  GiB: 9 GiB promised). Waiters are served in order: a small one that would fit does not pass a big one, and the order
  is the one the gate gave out under its own lock. A compile held for memory leaves the gate at once when its `cancel`
  turns true: `acquire` returns None (also when cancelled before it is even its turn), the waiters behind it move up,
  and nothing is left to release. A lease knows the least free memory while it ran (the first admitted by `min_jobs`,
  the second on 8 GiB free, then 1 GiB, below the 2 GiB reserve, while both run).
- `EstimatesTest`. After a re-chunk a chunk of the same name and size can hold another switch: its `switch_fn` must be
  within 20 % too, unless both are below `SWITCH_SMALL`. A peak measured while the memory was short (`keep_higher`)
  does not lower the record; without pressure the new peak is taken. The model is checked against the peaks it was
  drawn from (clang 20, -O1), within 15 %. A source with no cost is not part of the learned scale, and a scale far off
  is clamped. An entry from before `switch_fn` reads it as 0; garbled files are empty.
- `RunTest`: with SIGCHLD ignored `wait4` finds no child to wait for; the exit code comes from `Popen` and the peak is
  unknown.
- `AvailableTest`: on Windows the free memory is no more than what can still be committed (a small page file runs out
  first).
- `WrapTest`, wrappers at once over one state folder, as Ninja runs them (`WrapTest.finish()` gives (exit code, stdout,
  stderr) of each; a wrapper still running after the timeout fails the test; the child script can wait for a file
  another process makes). Gate off: each compile waits until the other has started, so both end well only if they ran
  at the same time. A small compile arriving after a held big one waits behind it, though the memory would hold it
  now; the big one, once running, owes all its need, so the third waits for it to end. Ended and reused entries are
  dropped (the test plants its own pid, as a wrapper that started at another time would leave it).
- `CgroupLimitTest`: `total_ram()` under a cgroup v2 limit is the tightest `memory.max` on the way up, so the reserve
  and the rule for unknown free memory go by what the build may use; under 6 GiB the reserve is 1.5 GiB, since 10 % of
  6 GiB is less than the minimum.

## `tests/mem_growth_check.py`

Builds and runs `tests/mem_growth_check.c` on `runtime/diag/mem_growth.h`, the memory report's growth bookkeeping:
which of the report's figures grew since the previous report and since play began, and which only ever climbed. No
GPU, no game files, any Python.

The checks feed the book reports as `runtime/vk/vk_memory.c` `growth_take()` does, once a period: a boot and a title
screen before the world clock runs, then play.

- `basics()`: the first report has nothing to compare with. Before play only the previous report counts, and only more
  than the step: +32, +64 and +64 are none past "more than 64", textures +65 is one. The base is the first figures in
  play, with no growth since play yet; in play both lists are given; a later report with no change keeps the second
  list and empties the first; when the world clock stops (title screen) it is still play.
- `noise()`: changes under `MG_NOISE` are neither rises nor falls (just under a MiB up; a MiB up from there; a MiB
  down); a fall of `MG_NOISE` ends "only climbs".
- `falls_and_cut()`: falls are ranked largest first after the growth; a cut buffer stays terminated and the count is
  still right (textures and other, twice).
- `session()`: the 15-minute session of 2026-11-13 at 1440p, one report a minute, with the figures of the log's
  `mem: by owner` and `mem: host` lines. Boot and title screen (the outside-VMA memory falls by 610 MiB when the boot's
  compute pipelines go), then play, where the textures and the device-local memory jump by gigabytes at the first area
  and then move with the areas, and the process's commit with them. The base is the first report in play, after the
  jump: the textures grew by 850 MiB since, and came down twice. 12 reports name a growth: the base and every report
  after it.

## `tests/mem_heap_check.py`

Builds and runs `tests/mem_heap_check.c` on `runtime/vk/vk_mem_heap.h`: when the memory report asks for the C runtime
heap's summary (`runtime/vk/vk_memory.c` `system_log()`), which holds every allocation of the game while it runs. Never
in play, where it held them for 54 to 70 ms at the first report of a session, unless `NX2SARIA_MEM_CENSUS`
(`mem_census`) asks for it. No GPU, no game files, any Python.

`tests/mem_heap_check.c` `session()` is a session as the report sees it: a report every 3600 presents, play from
`play_at` on; the summary takes `ms` milliseconds each time and counts as slow past 2 ms (`vk_memory.c`
`HEAP_SLOW_NS`), as `system_log()` notes it. It returns the summaries taken in play, and through `*before` those
before it.

- The sessions of 2026-11-14: play from the first seconds on, the first report at present 3600 in play, the summary 54
  and 70 ms. The rule before asked there once (the hitch); now it asks never.
- A long title screen: two reports before play, each fast there, then play: asked twice, both before play.
- A slow summary before play (a heap that is large already at the title screen): once, and no more.
- With the census switch every report asks, in play too, slow or not.

## `tests/mem_need_check.py`

Builds and runs `tests/mem_need_check.c` on `runtime/mem_need.h`: whether the machine has the commit the game needs
(`runtime/memory.c` `mem_start_note()`). Without the differential tests' packages (an `ImportError`) it makes the same
compiler choice without them.

- The machine the need was measured on, before the game starts: 41,515 of 73,154 MB in use, the process at 369 MB;
  32,008 MB for the game: enough.
- The same machine with the game in play, 65,840 of 73,154 MB in use and 24,000 MB of them the game's: what the game
  holds counts toward its need, so a check made late does not call a fitting game short.
- A 32 GB machine with an 8 GB page file and 12 GB in use by other programs: 28 GB free, enough; with 20 GB in use, 20
  GB free: short. A 16 GB machine with a 4 GB page file never fits.
- The boundary: exactly the need is enough, one MB less is short.
- Not asked (0), and a platform without a known limit (-1): never short. A process reading of -1 (unknown) adds nothing
  to the room.

With the game executable's path (`mem_need_check.py [PATH_TO_GAME_EXE]`), `probe()` also runs the executable's
`--probe-memneed` (it reads this machine's commit, prints the answer and starts nothing) three times: with
`mem_need_mb` = 0 (not checked), with 1 (any machine has it) and with a value no machine has, where the log's warning
line must appear. It returns 0 when each answers as it must. Without a commit limit (a Linux machine with none) nothing
is short: the forced case cannot be shown there.

## `tests/menu_mouse_check.c`

The arithmetic of the mouse in Ocarina of Time 1.1.1's menus (`runtime/ui/menu_mouse_geom.h`); `tests/menu_mouse_check.py`
builds and runs it. No game files.

- A button's box from its pane's fields as the game's focus frame takes them. The flags word holds the base position h
  (0 left, 1 centre, 2 right) and v (0 top, 1 centre, 2 bottom). A centred pane 200 x 100 at (300, -50), scale 1; a
  left-top base, where the translation is the left edge and the top edge (y up: the box hangs below it); a
  right-bottom base with scale 2 and a mirrored scale (the size counts, not the sign); the extra size
  (main+0x769930): bit 10 with bit 9 clear adds to the height, with bit 9 set to the width; no area, or not a number:
  no box.
- The layout units across the picture from the projection: 16:9 gives 3840 x 2160; 21:9 (64:27) is squeezed by 0.75,
  so ±2560 = 1920 + u with u = 640, where `NX2SARIA_WIDE_MENUS` = `full` puts the menus' edge panes; 32:9 gives ±3840;
  a 1280 x 720 layout ±640 x ±360; a matrix that is no projection gives the 16:9 values and 0.
- The pointer in layout units and back. On 16:9 the picture's corners are the layout's; on 21:9 the layout's 3840 sit
  in the middle 16:9 (from 1/8 to 7/8 of the width).
- The box under a point is the smallest of those it is in (a slot inside its panel), -1 off them all; edges count as
  in. A `wide_menus` = `full` button moved out to x 2250 on a 21:9 picture is reached by the pointer at 95% of the
  width.
- Movement: a pixel of a 4K picture is movement, less is not.
- Clicks: a press counts once; a press given while no menu takes the mouse (synced there) is no click of the next
  menu: between menus a quick second click lands on the fade.
- The queue: A and B wait one at most, the wheel three; a turn the other way drops the rest; a drop empties it (four
  quick right clicks on the pause menu queue one B; the queue is dropped when the menu goes because a B closed it). L
  and R wait one at most, like A and B, and go with their menu.
- The nearest box when the pointer is in none: the bombchu slot's narrow box (-1196..-1096, -413..-193 as the 16:9
  pause menu has it) beside the bomb slot's, and a click on the middle of the bombchu's picture (-1225, -300).
- The pause menu's tab row: the four pages' names and the L and R signs, off the row nothing; a tab's place in the
  window and back, on 16:9 (the notebook) and on 21:9 with the menus over the whole picture; the page from its
  layout's name, and the step toward a clicked tab. A walk to a clicked tab stops when the page goes the other way
  (the player's own L or R), not on its own steps.

## `tests/meshfb_batch_check.py`

Runs the game executable's `--probe-meshfb` twice, with the mesh fallback's batch off and on
(`NX2SARIA_MESH_FALLBACK_BATCH`, `runtime/vk/vk_exec_draw.c` `mfb_draw()`, `runtime/gfx/mfb_batch.h`), validation on
in both runs. The probe (`runtime/gfx/mesh_fb_probe.c`) draws mesh programs of the runtime's own through the replay,
with and without a task stage, with direct and GPU-written counts, one and two in one pass; it compares each picture
of the fallback with the mesh pipeline's (where the device has mesh shaders), and checks that a draw past the mesh
buffer and a task draw past the task buffer are drawn whole up to what fits and are counted. With the batch on, a
task program's first draws (no history yet) go one by one in a pass whose other draws are batched, and the passes are
cut; the probe's `batch:` line must show draws batched, some one by one for want of history, passes cut, and no draw
without a pre-pass command buffer.

`run()` reads the probe's output. The draws' checks: none failed, the last check of the draw part ran, and with
batching on the batch line came. Validation errors while drawing count; those of the device's creation (extensions
newer than the layer) and of its destruction (objects the probe's teardown leaves, such as the pre-pass command pool)
are the probe's, not the batch's, and are only printed. So is how the process ended after the checks. The usage text
the test prints is its `USAGE` string.

## `tests/mesh_fb_hold_check.py`

Builds and runs `tests/mesh_fb_hold_check.c` on `runtime/gfx/mesh_fb_hold.h`, the table behind the mesh fallback's
depth hold (`runtime/vk/vk_exec_draw.c`): a list whose colour draw was skipped for its pipeline holds its depth-only
draw for a few presents, and no longer. No GPU, no game files, any Python. The two lists are record addresses as the
game's memory has them (0x59220f600 and 0x26d876000).

- A frame's order is depth first, colour after: the stamp of one frame holds the depth draw of the next.
- While the colour draw keeps waiting, each frame stamps again and the hold goes on; when it is drawn the stamp goes,
  and the depth draw comes back in the next frame.
- A colour draw that no longer comes (the list left the view): the hold lapses by itself.
- A draw of one list does not clear another list's stamp, in whatever slot each lies.
- The present count's 24 bits wrap (77 hours at 60 fps): the hold counts across the wrap. The highest address the
  game's memory has keeps its bits above the count.
- Every index is inside the table, and a field of records (256 records of 12 bytes) spreads over it.

## `tests/mesh_invariant_check.py`

Mesh positions made Invariant (`runtime/vk/vk_pipeline.c` `spirv_mesh_invariant()`): builds the function, cut from
`vk_pipeline.c` by its text, and runs it on modules made in the test. No game files. Its `--help` text is the string
`DESCRIPTION`.

- A mesh shader whose position is a member of the per-vertex block (glslang's `gl_MeshVerticesEXT`); one whose position
  is an array variable of its own, as `tools/xlate/MeshSpirv.cs` declares it (an Output variable of vec4[max vertices]
  decorated BuiltIn Position), assembled; the same block mesh shader with the position already invariant; a vertex
  shader.
- The two mesh shaders get one Invariant decoration on the position (`OpMemberDecorate` on the block member,
  `OpDecorate` on the variable), the rewritten modules pass spirv-val, a second pass changes nothing, and the
  already-invariant mesh shader and the vertex shader are left alone.
- `invariants()` gives (`OpDecorate` Invariant count, `OpMemberDecorate` Invariant count) of a module; the table in
  `main()` maps a name to (changed, the Invariant decorations wanted after the rewrite as that pair).

## `tests/mesh_path_check.py`

Builds and runs `tests/mesh_path_check.c` on `runtime/vk/vk_mesh_path.h`: how the game's task and mesh programs are
drawn (`runtime/vk/vk_device.c` `create_device()`): natively, through the mesh fallback, the mesh emulation or not at
all, for each device kind and setting. No GPU, no game files, any Python.

- The default (auto, emulation off) by device.
- Intel's cards with mesh shaders take the fallback on every Intel driver: holes in the fog on Arc B390 and B580 with
  the native path (issue #13), which 0.18.2's 32-lane task stage and invariant positions did not mend.
- `mesh_fallback` = 0 gives an RX 6000 card native mesh pipelines back, as 0.10.1 drew them; `mesh_fallback` = 1 on
  every device.
- The mesh emulation acts only without mesh shaders, and then before the fallback. An NVIDIA card never takes the
  emulation, which ended the game inside its driver (GTX 10 cards in the low-end GPU mode of 0.10.1, which set
  `mesh_emulation` = 1).
- A value the settings table does not list reads as auto. The reason names the driver default, and how to turn it off.
- `mesh_fallback_batch` = auto batches where the fallback is the device's default (where `mesh_fallback` = auto would
  choose it).

## `tests/mesh_pipeline_check.c`

Injects cache-key results into the reporting guard used by `mesh_prepared_check` (`runtime/vk/vk_exec_draw.c`).

## `tests/mfb_batch_check.py`

Builds and runs `tests/mfb_batch_check.c` on `runtime/gfx/mfb_batch.h`, the rule of the mesh fallback's batch
(`runtime/vk/vk_exec_draw.c` `mfb_draw()`, `NX2SARIA_MESH_FALLBACK_BATCH`): which draws are batched and which go one
by one (the way back), the parts of the buffers they take, and a program's history. No GPU, no game files, any Python.

`tests/mfb_batch_check.c` `sim_draw_ex()` is the draw path's order as `mfb_draw()` has it, without Vulkan:
`mfb_batch_need`, then `mfb_batch_steps` (the same function `mfb_draw()` calls) with the cut, the pre-pass command
buffer and the part as calls on the simulation, and `mfb_count`. A draw one by one after parts were taken closes them
(`runtime/vk/vk_mesh_fb.c`, `closed`; `vk_meshfb_one_by_one`): the next batched draw cuts. A draw one by one ends its
pass (`vk_exec_barrier`). The end of a pre-pass command buffer (a cut, or `pre_end`) starts the parts over. `FS` (100)
is the parts' floor in these checks (`mfb_floor`).

- The reasons, in their order.
- The parts' sizes. The floor is 1/`MFB_FLOOR_DIV` of the buffer, in the program's workgroups, never under the least.
  The case seen in Kokiri Forest: a program that had asked for 32 at most asked for 454.
- The cut: at a pass's first batched draw, and again whenever the pass it served ended.
- The parts: in turn, aligned, never past the buffers, and nothing taken when one does not fit.
- The history grows from the records read back. A history is taken once it has settled: `MFB_MIN_CALM` records in a
  row without a jump (more than twice the most). A batched draw short of its part excludes the program, which is taken
  back after `MFB_READMIT` calm records, until `MFB_MAX_SHORTS`. The draws already in flight come back short together:
  one exclusion counts once.
- A pass as `mfb_draw()` sees it: the way back to one by one, its reasons and the cuts. Closed with the pass still open
  (a draw one by one that did not end it): the next batched draw cuts too. A stage that reads a texture or image goes
  one by one, without a cut. When the cut fails the draw goes one by one (no pre-pass), and the draws after it are
  tried again. When the buffers fill, the draw that does not fit cuts and starts the parts over; a part larger than
  the buffers goes one by one. A batched draw whose stages failed to record is not batched, and its cut was for
  nothing.
- The task overflow (`runtime/shaders/meshfb_clamp.comp` keeps the count asked for): a batched draw short of its task
  part excludes.

## `tests/mods_romfs_check.py`

The mods folder served as RomFS overlays (`runtime/hle/romfs.c`, `runtime/patches/mods.c`): load order, added files,
case, Unicode. Run as `python tests/mods_romfs_check.py --exe build/v121-<tag>/nx2saria.exe --version v121`; it needs
the extracted RomFS of the version. The `--help` description is the string `DESCRIPTION`.

It builds a mods folder in a temporary directory with files of its own bytes at real RomFS paths (from
`build/extracted/<version>/romfs/index.json`), runs `nx2saria.exe --probe-fs` on it and reads the `mods:` lines of the
log. `--probe-fs` compares the bytes it reads through the RomFS with every overlay file, so a wrong winner fails it
too. `run()` gives the probe a profile of its own beside `mods`, since the merged resource size table is written
there. In `main()` one planted file is of another game version's (no RomFS has a `Product.999` table: counted, never
served) and one sits in a mod manager's `romfslite` folder.

`restbl_case()` uses a second folder for the merged resource size table the start writes to the profile
(`runtime/hle/romfs.c` `restbl_overlay()`): a grown file's entry, an added file's, a shipped table's and another
version's table's (a file smaller than the original: only its table raises it). The table's layout (RESTBL): "RESTBL",
u32 version, u32 name length, u32 hashed count, u32 named count, {u32 crc32, u32 size} sorted, then {char[name length],
u32 size}. `restbl_pack()` returns a table with the sizes of its hashed entries set from `entries` (crc32 -> size,
existing hashes only).

## `tests/motion_fusion_check.py`

Builds and runs `tests/motion_fusion_check.c`: `runtime/input/motion_fusion.c` on made-up sensor samples, 4 ms apart,
with a stand-in for the log (printed, and the lines kept for the checks) and no settings from the platform layer. The
program prints a PASS or FAIL line per case. `static_checks()` also checks that `motion_fusion.c` makes no log call
between taking the instance's lock and the last unlock in the functions that log. No game files, no GPU.

Build notes: `runtime/platform/linux.c`'s clipboard calls SDL3; the check never uses the clipboard, so a stand-in keeps
it from needing SDL3's headers and library. The platform files are the runtime's, built as the runtime builds them;
the strict warnings are for `motion_fusion.c` and the driver. `runtime/simd_fp.h` `FOK` ors two compares on purpose,
and clang 18 warns on it.

The simulated controller (`Sim`) has a true orientation T (controller -> world, z up), rotated by the true rate; its
sensors add `bias` (rad/s), `gyro_noise` (rad/s RMS per axis) and `acc_noise` (G RMS per axis). `sim_pitch()` sets the
true pose turned about the controller's x by a number of degrees from flat; `sim_run()` gives n samples turning at the
true rate w (rad/s, controller axes), the accelerometer reading gravity in controller axes. The error measured is the
angle between the reported up (the reference z in controller axes) and the true one, in degrees. `RAW` is no
calibration and no dead zone: the sensor's own rate.

1. Lying still with a bias and a little noise (5 s): learned within 1.5 s of being still, then the reported rate is
   none and the angle stops, with one log line per still period.
2. No calibration: the bias goes through without a dead zone; each dead zone level zeroes a rate just under it and
   passes one just over it unchanged.
3. Known rotations from lying flat: a quarter turn about up, a quarter pitch and back.
4. Held in the hand (35 s, 2 deg/s RMS of gyro noise in all, 0.01 G): tremor and a slow pan are never still, and
   nothing is learned.
5. Re-learning: still with a bias, held, then still with a bias 0.3 deg/s off it. In 3 s the controller is still from
   about 0.8 s, so the period learns for the 1 s that logs it; over 6 s, past 2 s learned in all, the bias follows with
   a 3 s time constant.
6. The bias of each controller comes back with it; five more sources push the oldest out of the four kept.
7. The turn and tilt factors scale the rate, the angle and the direction. With 1 and 1 the reported rate is the input's
   and the direction the filter's, bit for bit; back to 1/1 snaps to it.
8. The integral term: a constant bias about a horizontal axis that is never learned (calibration off), tilted 30 deg,
   over 60 s, against the proportional term alone.
9. The aggregate line after 60 s.
10. The quarter turn and restart that `runtime/hle/ov_probe.c` `probe_feedback()` checks.

Further cases: (a) larger biases lying still are learned too (the step rule waits until the bias has been learned for 2
s); (b) held pitched for 120 s with a bias about the controller's z, long enough for the integral to take part of it
up, then set down flat in 2 s and left still for 10 s and 50 s more: once the bias is learned, the integral no longer
turns the heading of the still controller (`heading_deg()` is the x axis' heading); (c) turn and tilt factors over
closed and non-commuting paths (`loops()`: five loops of 30 deg turns about the controller's x and y; a 45 deg/s turn
about the world's up): the reported orientation stays tied to the filter's; (d) a known limit: a smooth slow turn with
little noise looks like a still controller with a bias; (e) samples with values that are not finite move nothing and
poison nothing; (f) a wrong bias (a smooth pan of 3.5 s learned as one) is learned again once the controller lies
truly still (60 s, no bias).

## `tests/mouse_direct_check.py`

Checks the direct camera hook's register contract and raw-count routing without game data
(`python tests/mouse_direct_check.py --cc <clang>`; the `--help` text is the string `DESCRIPTION`). One block of
stand-ins for the rest of the runtime serves both harnesses. The sites are Ocarina 1.1.1's
(`recomp/patches-v111.txt`, set `mouse_direct`): the two sites of `camera_input_oot` and `camera_activity_oot`.

## `tests/mouse_stick_check.c`

The mouse camera in stick mode (`runtime/input/input_map.c` `input_mouse_stick()`): raw counts to a right-stick
deflection with the camera speed taken out of it. The game multiplies the right stick's turn rates by the camera
speed, so the deflection is divided by it, and the division comes before the limit to full tilt: a slow movement turns
as far as at camera speed 1, and a fast flick reaches full tilt, which the game turns into its top speed with the
factor in it. `tests/mouse_stick_check.py` builds and runs it with `runtime/input/input_map.c`; the stand-ins are what
`input_map.c` needs from the rest of the runtime, none of it reached from `input_mouse_stick()`. No game files.

- A slow movement: the deflection is the one at camera speed 1 divided by the factor (while that is below full tilt),
  so the game's multiplication gives the same turn. Full tilt is at 3000 counts a second.
- A fast flick reaches full tilt at every camera speed, in each direction. The case that was wrong: at camera speed 1.5
  a flick stopped at two thirds of full tilt, the top speed of camera speed 1.
- Nothing to divide by: no deflection.

## `tests/movie_timeline_check.py`

Compares both movie timestamp patches with Unicorn and their lifted C. It needs Unicorn and a C compiler (`--cc` or
`CC`), no game files and no GPU; the generated harness files stay in `build/test/movie_timeline`. The `--help` text is
the string `DESCRIPTION`. `main()` builds one synthetic layer -> controller -> player -> video surface chain for both
engines. The samples are the selected 60 Hz video's millisecond-rounded PTS at different redraw rates: pauses,
repeated pictures and skipped pictures must follow PTS, never the redraw count; the last ones hold a picture and then
start a new movie.

## `tests/nvidia_cache_check.py`

Builds and runs `tests/nvidia_cache_check.c` on `runtime/vk/nvidia_cache.h`: what the game puts into the environment
for NVIDIA's shader disk cache (a folder beside the exe, a limit of 8 GiB and, on Windows, the cleanup off; the
player's own values stand), how it counts the folder and the lines the logs say about both. It needs a C compiler
only: no driver, no GPU and no game files.

`wiring()` finds in the sources what the runtime does with the header (`runtime/vk/vk_device.c`
`nvidia_cache_beside_exe()`, `runtime/vk/vk_pipeline.c` `lw_say_cache_folder()`, the setting's rows in
`runtime/cfg/cfg.c`), so a change of that wiring shows in this check even where the Windows-only code does not run; it
returns 0 when the runtime applies the header where it says. Its table holds (file, what it must hold, a pattern);
`squeeze()` reads adjacent string literals as one.

In `tests/nvidia_cache_check.c` the lister answers from a planted tree: `empties` are folders that are there and hold
nothing, `denied` folders the system refuses (`fake_list()` answers 5, `PLAT_FS_ERROR` of `runtime/platform/platform.h`),
`listed` counts the lister's calls, and a folder some entry sits in below it exists too. `deaf_list()` goes on after
the callback says stop; `deep_deaf_list()` gives a folder and a file in each, whatever the callback says; `deep_list()`
one folder inside each one, for ever. `measure()` plants a sentinel past `cap`: the walk never writes past it.

- The plan: scenarios written out by hand (`in_of(windows, enabled, driver, path_set, size_set, skip_set, folder_ok)`),
  then what holds for every combination of the seven inputs: where the folder's state is not asked (`nvc_need_folder`)
  it changes nothing, and Linux never differs from what it did before: the folder, then the limit.
- The variables a restart inherits: the game names what it set, and a start takes those for the game's. `nvc_stale`:
  what the game set before and does not set now is taken out of the environment.
- The lines. Linux: the words the log has always had, built in the check with the old format. Windows: the three
  variables, with their values and whose they are. The lines when nothing is set, and when the environment refuses a
  variable. A buffer too short takes what fits, ends with a NUL and is not passed.
- `nvc_commas` (1234567 into a short buffer is cut, never overrun) and `nvc_size_text` (1739461181 bytes is 1.62 GiB).
- `nvc_measure`. Windows paths join with a backslash, the root's own kind. The path buffer: a folder whose path does
  not fit is said, the rest counted ("/a/bb" needs 6 bytes with its NUL; with 2 the root alone does not fit; with 3
  the root fits to the last byte and "/a/bb" does not). The entry limit; a lister that goes on after the callback said
  stop is not counted past the limit. The depth limit: a folder inside every folder, for ever, and nothing is counted
  after it, whatever the lister does with the callback's answer.

## `tests/ocarina_log_check.py`

Builds `runtime/pacing/frame_rate.c`'s ocarina log (`ocarina_log_init`, `ocarina_watch`, `ocarina_end`, cut out of the
file) with stand-ins for the patch sets, the patch page and the log, and plays it the page words the sets `orecord`
(+264) and `ostaff` (+776) write. A stretch of ocarina use gives one line after two quiet seconds, with its presents,
the presents the recorder clock moved on and the notes the staff moved (rises of its signal), and nothing comes while
neither word changes. Without the sets the log watches nothing and says so at the start. No GPU, no game files, any
Python. `cut()` takes `frame_rate.c` from the ocarina log's first `#define` to the end of `ocarina_watch`;
`compiler()` here takes `$CC`, else MSYS2 UCRT64's clang, else the clang on `PATH`.

## `tests/ocarina_timing_check.py`

Checks the set `orecord` of `recomp/patches-v111.txt`: the ocarina recorder's clock carries each update's fraction of
a 60 Hz frame on the patch page's +264 (`FRACTION`) instead of cutting it. It needs Unicorn, numpy and a C compiler
(`--cc` or `CC`), no game files; harness files stay in `build/test/ocarina_timing`. The `--help` text is the string
`DESCRIPTION`. `SIZE` (0x4ea0000) is a stand-in main size: the patch page lands at main+0x4ea0000, as on 1.1.1.
`GAME` (0x1e390008) is the game's word, `fcvtzu w8, s0`.

1. `check_table()`, the table: the set's switch and default, its four sites, their original word and the nine
   replacement words, the page word resolved to `adrp x8` of the patch page at each site, and no other set's word
   loading or storing the fraction's bytes through an unsigned offset. The set is found by its name: sets added after
   it go after it in the table.
2. `check_lifted()`, the lifted words (`recomp/lifter.py`, as the generator lifts them) against the game's word and a
   float32 model. `fcvtzu()` is `fcvtzu w, s`: toward zero, NaN and negatives to 0, saturated at 2^32 - 1; `model()`
   is the words in float32 with round to nearest: (w8, the new fraction's bits). Whole steps of one and two frames
   give the game's count, and the fraction stays +0.0. The steps of the 40 to 240 fps limits count 3600 frames a
   minute, each update the floor or the ceiling of the step; at 144 Hz it is 5 in every 12 updates, and the float step
   is a little under 5/12, so the first 12 count 4. A carried fraction: 0.75 + 0.5 gives one frame and leaves 0.25.
   Steps the game never gives (NaN, negative, infinite, huge): the game's count, a fraction in [0, 1), and the next
   whole step counts one. v0 keeps the step, its upper lanes too.
3. `check_unicorn()`: the nine words, the `adrp` included, run from the site's address at each site (FP and SIMD on:
   `CPACR_EL1` = 0x300000) against the lifted C, where x8 is set to the page buffer as the `adrp` sets it to the page,
   over random steps and fractions: registers and the page agree bit for bit.

`build()`: each function sets the host's floating-point mode from `c->fpcr` as the runtime does on entry to guest code
(`runtime/cpu.h` `rt_fpcr_sync()`) and puts Python's back after the words; x8 is the page, set by the caller as the
`adrp` would. `Harness` is a Cpu and a page buffer for the lifted words; `Harness.run()` makes one call and returns
w8. The Cpu changes only in x8, v16-v18 and FPSR, and the page only at +264.

## `tests/oq_hold_check.py`

Builds and runs `tests/oq_hold_check.c` on `runtime/vk/vk_oq_hold.h`: the count a samples-passed report writes when
the full occlusion query ring left some of its draws uncounted (`runtime/vk/vk_exec_xfer.c` `oq_report_count()`,
`NX2SARIA_OQ_HOLD`): the larger of what was counted and the record's last complete count. No GPU, no game files, any
Python.

- Nothing remembered: the old rule, what was counted and at least 1.
- A record reported complete, then a frame that missed draws: the last count holds. What was counted is a lower bound:
  above the held count it wins.
- A newer complete report replaces the held count, an occluded one too.
- Other records keep their own counts; a record next to it is another one. A record that shares a slot with another
  forgets the older count, never takes the other's.
- A report with no CPU-visible record (address 0) is neither remembered nor held.
- The 1024 records of a frame's queries spread over the table: few collisions among consecutive records.

## `tests/pack_shader_cache_test.py`

`tools/pack_shader_cache.py` on a fake shader cache: the pack layout `runtime/gfx/gfx_shader.c` reads (sorted index,
bounds, data), loose files folded in and deleted, a second run merging new loose files, a loose file winning over its
packed copy, malformed files skipped and `--keep`. No game files; stdlib only.

`USE` is a use record as `gfx_shader.c` `use_store()` writes it; `read_pack()` returns {(kind, name hex): bytes},
checking what `gfx_shader.c` `pack_open()` checks. `PackTest.shader()` makes `in/` again, as `in_store` does after a
pack removed it: `in/` is removed when empty. A bad loose copy leaves the packed one in place; a bad file that is
nowhere else is skipped, and never deleted unless packed.

## `tests/part_file_check.py`

Builds `tests/part_file_check.c` with the platform layer and runs it: `runtime/part_file.h`, which the runtime's
detached threads write their files through (the flight recorder's dump). A file written piece by piece gets its name
only when it is complete, so a game that closes under the writing thread leaves the older file or none, never one cut
short. The C side checks the helper itself; the Python side then kills a process in the middle of a write, as the
window's close ends the dump thread, and looks at what it left: the older file whole under its name, the half in the
`.part` file. The build adds `stand_ins()` of `tests/platform_stand_ins.py` for what the platform layer calls outside
it. No game files, no GPU. `part_file_check <folder>` runs the checks in that folder;
`part_file_check <folder> hang` writes half of `<folder>/killed.tsv`, prints "writing" and sleeps: the Python side
kills it there, as a closed game ends the thread that writes a dump. `rt_log()` in the C file takes the platform
layer's lines.

- A new file: nothing under its name while it is written, the whole file after.
- Over an older file: the old one stays whole until the new one is complete.
- Given up (the writer met an error): the older file stays, the part goes.
- A folder that is not there: nothing opens, and closing nothing is no success. A name that does not fit the path
  buffers.

## `tests/pass_hooks_check.py`

Builds and runs `tests/pass_hooks_check.c` on `runtime/vk/vk_pass_hooks.h`: which pass begin runs the frame's pass
hooks (`runtime/vk/vk_exec.c` `vk_exec_pass_begin()`). A parallel group's hooks run once, at its first chunk's first
pass: a pass that chunk begins again later must not put its own target back as the upscaler's light target after the
hooks of later passes ran. No GPU, no game files, any Python.

The C file's stand-ins: the hook as `vk_dlaa_pass` keeps the light target (the target of the last pass that ran it); a
group as `vk_exec.c` keeps it (whether its first chunk began a pass); `pass_begin()` is a pass begin of a thread that
records no chunk (group NULL), or of chunk `idx` of a group, into a target.

- The rule itself.
- One thread: a pass begun twice (after a barrier) changes nothing, the last pass of the stream keeps the light target.
- A group whose chunks the workers record while the submitting thread waits for it: its passes begin in any order after
  the first chunk's (the first chunk's pass again, after a texture lookup waited for its turn), and the pass after the
  group comes after all of them.
- The submitting thread goes on past the group: the scene pass after it begins once the group's first chunk began its
  pass (`vk_exec_hooks_wait` let it through: the group's hooks ran), and that chunk begins its pass again after that
  (late: after a lookup's turn, a barrier, a pass of the replay's own). The scene pass is later in the stream and keeps
  the light target; the hooks running at the second begin gave the upscaler the earlier pass's image for a frame.
- Two groups out and the pass after them: each first chunk begins its pass after the group before began its own
  (`chunk_record`), and each begins it again out of order.
- A first chunk whose first pass begin waited (a deferred lookup returns before the hooks): the hooks run at the begin
  that goes through, which is the one that marks the group.

## `tests/pb_torn_check.py`

Checks how `pipeline-binaries.bin` keeps its whole records (`runtime/vk/vk_pipeline.c` `pb_index()` and `pb_keep()`,
`NX2SARIA_PIPE_BINARIES=1`). `pb_init` keeps the whole records of a file that ends inside one (the process ended
during an append, or a disk filled) or holds a malformed one, and cuts the file back to them; the walk must say where
the last whole record ends, and `pb_keep()` must cut the file there. The check cuts the file's record layout and
`pb_hash`, `pb_find`, `pb_insert`, `pb_index` and `pb_keep` out of `vk_pipeline.c`, writes files of whole records, one
with a torn last record and one with a malformed record in the middle, and checks the offset and the index the walk
gives, the length `pb_keep()` leaves on the disk, that a second walk reaches the end of the cut file, and that a
record appended after the cut is read back. `plat_file_truncate` is a stand-in that cuts the file with the C library.
No GPU, no game files, any Python; it needs the Vulkan headers of MSYS2 UCRT64 (`vulkan/vulkan.h`).

## `tests/perf_bound_check.py`

Builds and runs `tests/perf_bound_check.c` on `runtime/diag/perf_bound.h`: which side holds the frame back in the
60-frame window line of the perf trace (`runtime/diag/perf.c` `perf_gpu_line()`): pace, gpu, serial or cpu. It needs a
C compiler only: no Vulkan headers, no GPU and no game files.

- Kokiri Forest on an RX 6700 (RADV, 2026-11-19, 0.19.0-rc.2): 33.6 ms frames, GPU busy 22.5 ms, idle waiting for the
  next submission 9.6 ms, the game's threads waiting 17 ms for GPU fences. The old rule said cpu.
- The same window without the idle split (no calibrated timestamps): the rule cannot see the turns, cpu as before.
- A full GPU wins over serial. Pacing wins over everything.
- The CPU alone: the GPU starves but nothing waits for it.
- Threads wait for the GPU but the GPU always had work queued: not taking turns, and not full: cpu.
- The edges are inclusive: a fifth starved and a quarter waiting.
- Without timestamps the fence waits decide, as before.
- An idle split with no interval (the first window of a run): no division by zero, cpu.

## `tests/perf_summary_check.py`

`tools/perf_summary.py` reads the present side of a log: lines made with the runtime's format strings give the present
percentiles, the present thread's window averages and the render thread's hand-off share, and the lines of older logs
(no hand-off share on the perf line, no present call or hand-off on the `present:` lines) still parse.

The format strings are copied into the test and must be kept in step with the runtime:
`runtime/vk/vk_backend.c` `present_main()`'s `TRACE=present` line and `present_window()`'s line, as the C source has
them, and `runtime/diag/perf.c` `perf_present()`'s window line, its head to the shares (the test fills in the rest).
`source_has()` checks that they are still the runtime's; the C sources split them over string literals, so adjacent
literals on following lines are joined first. `c_fmt()` turns a C printf format with `%llu`, `%lld` and `%u` into
Python's `%d`, `%%` kept.

The sample: 100 presents of the new form with intervals 16.0..16.99 ms, then one of 40 ms; the present call 0.1..1.09
ms; the hand-off 2.0 ms each but one of 30 ms. Of the 101 intervals p50 is the 51st smallest (16.50), p99 the 100th
(16.99), max 40.

## `tests/pgo_build_test.py`

The profile-guided build of `tools/make_game.py` and `tools/pgo.py`, with a stand-in for the compiler process. No game
files.

Which flags a build compiles with: a kit without an execution profile (the flags and keys it always had), a kit with
one that fits (the lifted code at -O2 and every compile with the profile), a record that names another `recomp/`, a
record that is missing or names none, a profile the compiler refuses in the trial compile, `NX2SARIA_PGO=off`, and an
instrumented build (`NX2SARIA_PGO=gen`). The object keys: the same profile in a kit at another place keeps them,
another profile changes them. `tools/pgo.py`: the record a merge writes names the `recomp/` a kit compares, a game
built from another `recomp/` stops the merge, and a release refuses a profile of another tree.
`tools/check_content.py` refuses profiles in a commit.

- `Compiler` stands in for `subprocess.run` in `make_game.py`: it records every command, answers `--version`, writes
  the object and the dependency file of a compile, and refuses a command with `-fprofile-use=` when `refuse` is set.
- `PgoBuildTest.add_profile()` puts a profile in the kit's `data/` with a record that names the kit's `recomp/`
  ('fits'), another one ('other'), none ('empty'), or without a record (None). `PgoBuildTest.commands()` gives (the
  lifted code's command, a runtime file's) with the flags, without the compiler and the paths (up to
  `-I <runtime> -o <object> <source>`). `PgoBuildTest.make_real_recomp()` makes the test kit's `recomp/` this
  checkout's, so that what `make_game.py` records in the kit is what `tools/pgo.py`, which reads the checkout,
  compares it with.
- Without a profile there is nothing to try: no compiler runs, `pgo_tag()` is empty (the compile step's key is what it
  was) and `pgo_salt('compiler', None)` is `'compiler'` (so are the object keys: the flags and the compiler's id
  alone, the key a kit before profiles gave).
- A fitting profile: one trial compile by each compiler that will read the file, the fast one with its sysroot. A
  source checkout takes the profile from `build/pgo`, but a release's `data/` comes first.
- A record of another tree leaves the profile out, decided before any compiler runs; the same record fits once the
  kit's `recomp/` is the one it names, and not after the lifter changed again.
- A profile the compiler refuses is left out: one compile says so, not every compile of the build; the second
  compiler's refusal counts too.
- `--pgo off` (`PGO_MODE` 'off') leaves a fitting profile out; `NX2SARIA_PGO` with a value that is no mode is as
  without it.
- An instrumented build counts into the game folder: what the game was built from goes beside the counts, and
  `tools/pgo.py` reads it back. Build tools without the profile runtime stop an instrumented build; with the runtime
  there is nothing to say.
- A launcher update puts the kit into another folder: the same profile there keeps every object. Another profile, or a
  build that leaves it out, compiles again (four objects each time: no profile, the kit brought one, another, left
  out).
- `PgoToolTest.profdata()` stands in for llvm-profdata. The merge's record is what `make_game.py` works out for a kit
  with this checkout's `recomp/`: the profile fits such a kit.
- `RealCompilerTest` uses the real compiler when MSYS2 UCRT64's clang is installed (and an llvm-profdata, when one is
  found): a file that is no profile (a profile's first bytes and nothing after them) is refused by the trial compile,
  a real one is taken. `TEXT_PROFILE` is one function with one counter, as llvm-profdata reads text.

## `tests/pipeline_cache_file_check.py`

Builds and runs `tests/pipeline_cache_file_check.c` on `runtime/vk/vk_cache_file.h`: the read of `pipeline-cache.bin`
(`runtime/vk/vk_pipeline.c` `cache_load()`) with 64-bit sizes. The C program makes a sparse file just over 2 GB in the
folder given as `argv[1]` (`build/test/pipeline_cache_file`; it takes no disk space, and about 2 GB of memory for the
seconds the read lasts), with a marker at its start and at its end, and the Python side removes it. `HEADER` (32) is
the size of the header every pipeline cache starts with (`VkPipelineCacheHeaderVersionOne`). The file helper writes
`head` at the start, `tail` as the last bytes, and a hole between them.

- No file: a first run, which is not a refusal. A file of a header alone holds no pipeline: refused with its size. A
  small file: read whole.
- Just over 2 GB: a 32-bit tell measures it as negative. Under a limit it is refused before any read, and its real
  size comes back for the log line; with no limit, and with a limit above its size, it is read whole.

## `tests/pipeline_list_test.py`

`tools/pipeline_list.py` on made-up game folders: records folded once, a key whose shader the folder's answers do not
name left out, the compute variants taken, a list made before as an input, `--mesh-only`, a folder of another record
layout left out, the file read back, and what `tools/prepare_shaders.py` takes from a list for its seed. Also
`tools/check_content.py` refusing such files in a commit. No game files; stdlib only.

`REC` (464) is a `KeyRec` of the runtime that wrote the test's files: three ids, two flags, the state. `answer()` makes
a `shader-answers.bin` record: stage, code hash, input id, 147 bytes of answers, the pad byte. The file the tool
writes is the layout the runtime reads as it is (`pipeline-keys.bin`). `PipelineList.test_seed_variants()`:
`prepare_shaders.list_variants` gives the variants of shaders the game version has, once, with the id left to the
runtime; the case is skipped on an `ImportError`, since its shader inventory needs Python 3.14's `compression.zstd`.

## `tests/platform_bulk_write_test.c`

`plat_mem_prepare_write` on Linux (`runtime/platform/linux.c`): every page of a prepared range counts as written, a
reset leaves it clean, and a later store to one page is still seen. No game files. It is built and run by hand from
the repository root, and exits 77 when the write watch is not available on this kernel:

```sh
clang -O2 -ffunction-sections -fdata-sections -Iruntime -c runtime/platform/linux.c -o /tmp/plat.o
clang -O2 -Wall -Iruntime tests/platform_bulk_write_test.c /tmp/plat.o -Wl,--gc-sections -lpthread -ldl -o /tmp/bulk
/tmp/bulk
```

## `tests/platform_dmabuf_test.c`

Guest memory as a memfd's (`runtime/platform/linux.c`, `NX2SARIA_DMABUF`): a dma-buf of a watched range maps the very
pages the CPU writes; a decommit inside it zeroes them in place (the dma-buf still sees the CPU's pages), one outside
it reads zero when committed again; the write watch keeps working. No GPU, no game files. Built like
`tests/platform_bulk_write_test.c`, with this file and `-o /tmp/dmabuf`; exit 77 means no `/dev/udmabuf` or no write
watch here. `memfd_bytes()` is the guest memfd's allocated bytes.

- The write watch is read and reset until a call has fewer than it could take.
- A dma-buf of the first 8 MiB maps the guest's own pages; a write through it, as a GPU write, is seen by the guest and
  not by the write watch.
- Protection changes (a freeze, an armed lazy range) leave the dma-buf alone.
- A decommit inside the dma-buf: zero in place, the same pages for both. One that straddles the dma-buf's end: zero
  both sides when committed again. One outside any dma-buf: the pages leave the memfd and read zero.
- A dma-buf over pages nothing committed yet allocates them; when it is gone they go back, the committed ones stay.
- Outside the reservation, or not whole pages: no dma-buf.

## `tests/platform_linux_test.py`

Fast checks for Linux platform services; no display, game data or build tools required. The repository root goes on
`sys.path` for `launcher/`, when the file is run as `python tests/platform_linux_test.py`.

- `UserDirsTest`: the desktop's own Downloads, Desktop and Documents folders (`user-dirs.dirs`), which a desktop set
  up in another language translates: the dump search and Export Save look there too (`launcher/winsys.py`
  `known_folder()`). A folder set to the home folder is turned off; with no file there, none.
- `OtherDrivesTest`: the dump search also looks on the disks mounted under the system's mount folders (a partition
  shared with Windows), and the dump and folder rows open the desktop's chooser, which reaches them. Only local disks
  mounted now count: an autofs placeholder is not listed (listing it would mount the disk) while the disk itself,
  mounted, is; a placeholder not mounted now, a network share and another account's removable disk are left alone;
  SteamOS's SD card counts; the mount folder itself is no drive below it. The search goes two levels into a drive (a
  file three levels down is not looked at; the report counts home, the drive, its folder and the 2 below) and stops in
  time.
- The desktop's chooser: a name that is no UTF-8 comes back as it is; a cancelled zenity does not ask kdialog too; on
  KDE its own comes first; a broken zenity (255) falls to kdialog. The chooser says in the log which it was and what
  it gave. "All files" stays in the filter: the desktop's patterns are case-sensitive, and `GAME.NUP` would not match
  `*.nup`.

## `tests/platform_restart_mask_test.c`

`plat_restart` on Linux (`runtime/platform/linux.c`): the game it starts may run on every processor this one could
before `plat_cpu_reserve` took a core for the recording thread, whichever thread asks for the restart. No game files.
Built like `tests/platform_bulk_write_test.c`, with this file and `-o /tmp/restart_mask`; exit 77 means this machine
has no core `plat_cpu_reserve` would take. The test starts itself again through `plat_restart`; that copy (it sees
`NX2SARIA_RESTART_WAIT`) only writes how many processors it may run on, and the test waits up to 5 s for what it
wrote (-1 when it never came). The restart is asked for from a thread that existed before the core was taken, as the
game's main thread does, and from the thread that holds the core.

## `tests/platform_stand_ins.py`

Not a test: the stand-ins the tests that link the platform layer alone build with (`tests/platform_stand_ins.c`), so
they need neither the rest of the runtime nor SDL3's development files: the platform layer's own calls out are
answered there. `stand_ins()` gives the compiler arguments that add the stand-ins to a build in the folder `work`: its
include folder and source; `include()` those for the stand-in SDL3 header, written under `work`, the one SDL3 header
`runtime/platform/linux.c` includes, with the calls it makes.

`tests/platform_stand_ins.c` stands in for what the platform layer (`runtime/platform/linux.c`,
`runtime/platform/win32.c`) calls outside it: the settings' environment read (`runtime/cfg/cfg.c` `cfg_env()`), as
`cfg.c` has it for a name outside the settings' prefixes (the environment's own value), and, on Linux, SDL's clipboard
(`plat_clipboard_set` / `plat_clipboard_get`): no clipboard, so a set fails and a get finds nothing.

## `tests/play_gate_check.py`

Builds and runs `tests/play_gate_check.c` on `runtime/vk/vk_play_gate.h`, the play gate of the background pipeline
builds (`runtime/vk/vk_pipeline.c` `play_rest()`): they rest while the frame shown in play has `bg_build_play_draws`
draws or more and was shown within half a second, never in a light moment (an area load, the boot screen's warm, a
preparation) or with the setting at 0; and what the log's line "background builds paused in play: N frames, M builds
deferred" counts. Without the differential tests' packages (an `ImportError`) it makes the same compiler choice
without them.

What the builders do at rest, and what it changes in the frame rate, shows only in a run: the start's line "background
builds rest in play", the line above at each save in play, and [performance.md](../performance.md) ("Background builds
in play") for a measurement with `tools/bench.py`. `LOG_LINES` are the log's lines a run is checked by, as the formats
in the runtime's source; `log_lines()` returns 0 when each is in the runtime's source as its format string, adjacent
string literals read as one.

- The game in play: a frame of 2,850 draws shown 16 ms ago. Builds rest. At the threshold they rest, one draw under it
  they go on: a menu, the pause screen, the title.
- No frame shown for `PLAY_GATE_SHOWN_MS`: a load or a hang, not play. Builds go on.
- A light moment whatever the frame: an area load, the boot screen waiting for the warm, a preparation.
- The setting at 0: they build in every frame, as before. A setting of 1: every frame shown in play is heavy.
- The counts: a session where two builders rest over three heavy frames with 5 queued builds, a third queued during
  the rest, then a light frame lets them go on. A heavy frame while nobody rests yet is not counted; the first builder
  finds the queue's 5 builds, the second finds them counted already; a light frame is not counted.
- The line comes when the counts moved, and once for them. More ends than begins never leave a negative count of
  builders at rest, which would count no frame again.

## `tests/present_held_check.py`

Builds `tests/present_held_check.c`: `runtime/pacing/vk_present_held.h`, the present that waits for held work, with
one presenting thread and three submitting threads. Every frame must reach the present callback exactly once and in
frame order, never two callbacks at once, and a submitting thread must never wait for the lock. The argument `old`
runs the unlocked check-then-clear of 0.10.1 (`vk_backend.c` `ph_flush` before `vk_present_held.h`) through the same
harness and only prints what it sees, to show that the harness catches the race.

- The stand-in for `runtime/vk/vk_backend.c` `ph_out()`: `present_image`'s work is the spin, the window a second caller
  would fall into. The stand-in for `ph_why_after()`: out when nothing is held any more, or `PH_MAX` submissions after
  it began to wait.
- `stress()`: `FRAMES` (200000) is the least; the stress goes on until both threads have put held presents out. Held
  presents went out from a submitting thread (after the work or at `PH_MAX`) and from the presenting one (at the next
  present), so the lock was in the way of both. How soon that happens is the scheduler's: the stress runs until it has
  (`FRAMES_MOST` at the most), not for a fixed count. `PH_MAX` alone takes 8 submissions in a row that leave work
  held, which a machine with few cores schedules rarely: `steps()` checks it. In the loop an odd step clears `held`
  (`held_resume` ran what it could), and the other branch is `vk_be_present`'s synchronous one.
- `steps()`: one thread, step by step. `PH_MAX` submissions after it began to wait put a present out though work is
  still held.
- `contended()`: the lock held elsewhere: a submitting thread does not wait, the presenting thread waits and is
  counted (the present is armed, another thread holds the lock and puts the held present out).
