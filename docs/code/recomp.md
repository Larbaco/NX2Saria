# recomp

The static recompiler: it reads the game's ExeFS modules and writes C, one function per guest function, which the
build compiles into the game. The design is in [architecture.md](../architecture.md) ("2. CPU: lift ARM64 to C"); the
differential tests that check the lifter are in [building.md](../building.md#checks). The patch files
`recomp/patches-v*.txt` are data the generator reads and keep their `#` comments.

Text the generator writes into the C it emits is output, not a comment of this code, and stays as it is: the
`GENERATED_MARKER` header line (`recomp/gen.py` `header()`), the header comments of `layout.h` (`recomp/layout.py`
`write()`) and `patches.h` (`recomp/patches.py` `header_text()`), the `/* hint */`, `/* cache maintenance */`, `/* prfm */`,
`/* stg */` and `/* pac/aut: no-op */` statements of `recomp/lifter.py`, and the `/* <reason> */` after an `rt_udf` trap
(`recomp/module.py` `Module.lift()`), which `recomp/gen.py` `Emitter.scan()` reads back.

## `recomp/gen.py`

Generates C for the ExeFS modules: function discovery, then parallel emission into chunked C files.

```
gen.py --exefs build/extracted/v121/exefs --out generated/v121 [--modules main,subsdk0,sdk] [--jobs N]
       [--limit N] [--chunk-fns N] [--chunk-bytes B] [--big-fn B] [--patches FILE]
```

Output, under `--out`:

| File | What |
|---|---|
| `layout.h`, `layout.json` | module bases (`recomp/layout.py`) |
| `<mod>/<mod>_NNNN.c` | one `void f_<addr>(Cpu *c)` per guest function, about `--chunk-bytes` of compile cost per file |
| `<mod>/<mod>_table.c` | sorted `<mod>_fn_addrs[]`, `<mod>_fn_ptrs[]` and `<mod>_fn_count` for `rt_dispatch` |
| `<mod>/manifest.json` | chunk list, function count, statistics and timings |
| `patches.h` | code patch sets for the runtime (`recomp/patches.py`; no sets without `--patches`) |

Function discovery runs in this process, vectorised over the module's `.text` words. Function starts are the module
seeds (`.eh_frame_hdr`, exported FUNC symbols, RELATIVE relocation targets), every BL target, and code addresses
materialised by ADR or ADRP+ADD (except an ADR that anchors a jump table: one with a `br` in the next 16
instructions, or one an ADD register form uses as its base right after). A function is [start, next start). A direct
branch whose target lies outside its function makes the target a new start, until no new start appears.

Emission runs in worker processes, one chunk per task. Each worker loads the modules itself, lifts one function at a
time and resolves its jump tables (`recomp/jumptables.py`), so memory per worker does not grow with the module. Chunks
are sized from a per-instruction estimate of the C they produce, weighted by what that C costs to compile (a function
with a jump-table `br` costs more than its size), so their names and contents only depend on the input; a chunk whose
text did not change is not rewritten and keeps its mtime.

The imports after the `sys.path` setup carry `# noqa: E402`, which stays.

### The size and cost estimates

Estimated bytes of C per instruction, by class, for chunking (`estimate_sizes`): least squares over every function of
main, subsdk0 and sdk (1.2.1) gives 62 (`EST_INT`), 75 (`EST_SIMD`) and 48 (`EST_SIMD_LDST`) bytes per instruction, 38
per function (`EST_FN`: signature, FN_ENTER, closing brace) and 4.5 per instruction of a function with a `br`
(switches; the all-label fallback costs about 60 per instruction but is rare); the median error per function is 7%.
`EST_BR_LABELS` 10 is charged per instruction of a function with a `br`: switch cases, or labels if it falls back.

Compile cost of a function, in bytes of ordinary C (`estimate_costs`, the manifest's chunk "cost"). Measured with clang
at -O1, one compile at a time, over chunks and one-function files of the 1.1.1 lift: seconds are about 0.4 + 2.2 per MB
of C + 4.5 per MB squared of each function with a `br`. Ordinary C compiles in time linear in its size, but the switch a
jump-table `br` becomes makes clang's time grow with the square of the function's size (a 3.9 MB function with a switch
takes 78 s and 922 MiB; a 2.5 MB one without, 6 s and 276 MiB). So a function with a switch of size s costs
s + s * s // `COST_Q`, `COST_Q` = 1 MB * 2.2 / 4.5 (490,000). The estimate cannot tell the switches apart before
emission: it charges every function that holds any BR Xn, that is every function that may hold a switch, including
those whose `br` is a plain indirect branch and emits none. The manifest's "cost" and "switch_fn" come from the emitted
text and count only the functions where the emitter wrote a switch: a resolved jump table or the all-label fallback
(`chunk_text`).

### `discover()`

Function starts of a module as a sorted int64 array of word indices into `mod.words`, plus discovery statistics.

- B and BL: imm26. BL targets are function starts; B targets join the fixpoint.
- B.cond (the lifter accepts o1 = 1 too), CBZ/CBNZ: imm19 at bit 5. TBZ/TBNZ: imm14 at bit 5.
- ADR: pc + imm21. ADRP: page + (imm21 << 12), then ADD (immediate, 64-bit, unshifted) of the same register within 8
  instructions. Targets must be aligned and in `.text`.
- Clang's jump tables add a table entry to an ADR anchor, the first case block, then `br`. That anchor is not a
  function (it would split the switch): an ADR with a `br` in the next 16 instructions is not a seed, nor is one whose
  register is the base of an ADD (shifted or extended register) in the next 4 instructions
  (`adr xA, L; ldrb wO, [..]; add xA, xA, wO, lsl #2`), however far the scheduler moved the `br`.

### `estimate_sizes()`

(Estimated bytes of C per function, whether the function holds a BR Xn) as two arrays, for chunking only (the manifest
records the real sizes). The classes: `(w & 0x0E000000) == 0x0E000000` FP/AdvSIMD data processing;
`((w & 0x0A000000) == 0x08000000)` with bit 26 set, FP/SIMD load/store; `(w & 0xFFFFFC1F) == 0xD61F0000`, BR Xn.

### `estimate_costs()`

Estimated compile cost of every function in bytes of ordinary C, from `estimate_sizes`: a function that may hold a
switch (`with_br`: it holds a BR Xn) is charged as if it did (`COST_Q`).

### `partition()`

Contiguous [i0, i1) function ranges: at most `max_fns` functions and about `max_cost` of estimated compile cost each; a
function whose cost passes `big_fn` gets a file of its own, so costly functions compile in parallel.

### `Emitter`

C for the functions of one module. `starts` is the set of every function start of the module. `patched` is
{address: [(set index, [replacement words])]} (`recomp/patches.py`); `spins` is {spin wait back-edge: its head}
(`recomp/patches.py` `spin` lines); `reg_locals` is the number of registers per function in C locals
(`recomp/regs.py`); `kept` is `regs.CALLEE_SAVED` (not reloaded after a call) unless `--no-callee-saved-trust`.

### `Emitter.scan()`

Labels and jump tables of one function: {pc: sorted targets} for resolved tables, 'all' for an unresolved computed br.
A replacement's branch into the function (a patched site) needs its label too. A trap's reason is read back from the
`/* ... */` that `Module.lift` wrote after `rt_udf`.

### `Emitter.replacements()`

The lifted words of each alternative of the patched site a, in set order. Only the last word of an alternative may
branch (bl, b, b.cond / cbz / tbz, ret).

### `Emitter.flow()`

Successors of each instruction inside the function [entry, end). Index len(insns) stands for falling through into the
next function and has none; ret and a trap have none either. A patched site has the successors of all its
alternatives.

### `Emitter.flag_plan()`

The `recomp/flags.py` plan for the function: each instruction's flag use over the flow `succ` (`flow`). A `br` through
`rt_dispatch` is an exit, and so is a table's default.

### `Emitter.patched_effect()`

Flag effect of a patched site: every alternative (the original and each replacement) must write the same flags, in at
most one word each; the site reads flags when any alternative does (a b.cond reads them).

### `Emitter.function()`

(Lines of C, referenced function starts) for the guest function [entry, end).

- An unresolved computed br re-enters the function through `fb_<entry>`, a C function of its own, which local flags
  would not survive; such functions keep NZCV in `c->nzcv`. The same holds for registers in locals (`recomp/regs.py`):
  `fb_<entry>` keeps them in the Cpu too.
- `fb_<entry>` switches on `c->pc` to any instruction. A goto to a shared dispatch block inside the body instead turns
  every instruction into a loop header, and clang's loop passes then take tens of seconds on one function.
- A patched site is emitted as `if (switch) { replacement } else if ... else { original }` (recomp/patches.py), one
  lifted word at a time.
- `body` holds (instruction index, or None for a label; line): a line belongs to the instruction it lifts.
- A pass of a spin wait's back-edge is `rt_spin_gate(c, <head>); goto <head>`.
- A function whose last instruction is not a terminator (or is a patched site) falls through into the next function.

### `chunk_text()`

C text of one chunk. `fns`: [(entry, end)]. `sizes`, if given, receives (length of the function's text, whether it
holds a switch) per function: a switch is what a resolved jump table or the all-label fallback emits. Every referenced
function gets a prototype, including ones defined later in this chunk.

### `write_if_changed()`

Returns True if the file was written. An unchanged file keeps its mtime, so the build skips it. A changed one is
written under a temporary name and renamed into place, so a reader never sees it half written.

### `_retry()`

Windows refuses to rewrite or delete a file another process has open or mapped (clang maps its sources, so a build
running on the output holds them; so can a scanner or an editor). Waits up to 30 s for it.

### `_worker_init()`

Worker state `_W`: 'mods' {name: (path, base)}, 'starts' {name: sorted array}, 'emitters' {name: Emitter}, 'opts' (the
Emitter keyword arguments).

### `_emit_chunk()`

Worker: writes one chunk and returns its statistics. Test hook (`LIFT_TEST_OOM_ABOVE`): a pool wider than its value raises the
MemoryError a busy machine does, so the retry can be exercised. The compile cost comes from the emitted text: the
chunk's bytes plus the quadratic part of each function with a switch (a resolved jump table or the all-label fallback;
a `br` emitted as a plain dispatch does not count).

### `remove_stale()`

Deletes chunks an earlier run wrote beyond this run's count, and temporary files an interrupted run left.

### `WorkerLost`

Peak memory of one emit worker on 1.4.3 is 1.3 GiB (`WORKER_MEM`, 1.5 GiB with margin); the parent holds about 0.7 GiB.
`MEM_SHARE`
0.8 is the part of the free memory the workers may take, the rest is for the system and other programs. `GATE_POLL` 0.5
is the seconds between two looks at the free memory and the pool while waiting (`dispatch`). `WORKER_LOST_S` (120) is
the seconds without a result after a worker of the pool was replaced before `dispatch` gives up on the tasks it held: a
chunk takes seconds to emit, a worker the system killed (for memory) takes its task with it, and the pool never reports
that task.

### `lift_gate()`

(Enabled, reserve in bytes, why) of the memory gate for the emission: the compile gate's settings (`tools/memgate.py`
settings: `NX2SARIA_BUILD_MEM_GATE`, `NX2SARIA_BUILD_MEM_RESERVE`, else its default reserve). A malformed setting does not
stop the lift: it is reported and the default reserve applies. Off: reserve 0, and `why` says so.

### `pick_jobs()`

Worker count and the reason for it: `--jobs` if given, else the CPU count capped by the free memory. The workers may
take `MEM_SHARE` of the free memory, and no more than leaves `reserve` free (bytes; None: the compile gate's,
`lift_gate`); the free memory is the gate's reading (`memgate.available`), which on Windows also counts the commit left.

### `GateStats`

What `dispatch` saw of the free memory: the least reading, how many times it held dispatch and for how long.

### `_worker_pids()`

The process ids of a multiprocessing pool's workers (empty for a pool that does not show them).

### `dispatch()`

Runs `fn` on each of `tasks` in `pool`, calling `on_result` with each result as it completes. At most `jobs` tasks are
outstanding, and the next one is submitted only while the free memory (`available()`; None: unknown) is at least
`reserve`, as the compile gate admits compiles; below it dispatch waits, looking again every `poll` seconds and at each
completion. One task is always outstanding while any is left, so a machine short of memory still progresses. An
exception a task raised (a MemoryError among them) is raised here; the tasks still outstanding are then lost with the
pool. `stats` (`GateStats`) gathers the readings. `available` defaults to `memgate.available`.

The pool replaces a worker that died, but the task it held never completes. So dispatch never waits without looking:
once the pool's workers (`workers(pool)`, a set of process ids) changed, no result for `lost_after` seconds (counted
from the change or the last result, whichever is later) raises `WorkerLost`.

### `emit_all()`

Runs `fn` on every task of `pending` (module, number, ...) in `make_pool(jobs)` through `dispatch`. A MemoryError or a
worker lost with its chunk (`WorkerLost`: the system killed it, most likely for memory) restarts the pool with half the
workers on the tasks with no result yet, down to one worker, where it is raised. Chunk writes are atomic and
idempotent, so a chunk lost with a failed pool is simply emitted again.

### `setting_env()`

Reads the new environment spelling first, then its legacy alias; a set empty value still takes precedence.

### `main()`

The costliest chunks are emitted first, so the pool does not end on a long tail.

## `recomp/lifter.py`

AArch64 to C static recompiler. Integer, memory and control flow are translated to C. FP/SIMD instructions (and
CRC32) are either re-emitted as inline assembly (`simd='asm'`, AArch64 hosts), with the guest V registers loaded from
and stored to the Cpu struct around each batch, so their semantics are bit-exact by construction, or lowered to
portable C (`simd='c'`, `recomp/simd_c.py` and `runtime/simd.h`).

`OFF_X`, `OFF_SP` and `OFF_PC` are offsets in the Cpu struct and must match `runtime/cpu.h`.

### `set_reg()`

Statement writing `expr` to Rd (zero-extending 32-bit results).

### `Insn`

The result of lifting one instruction. `kind`: 'op', 'asm', 'b', 'bcond', 'bl', 'blr', 'br', 'ret', 'trap'. `code`: C
statements (for 'op'), or the condition expression for 'bcond'. `target`: the branch target (an int) or a register
expression. `asm`: the `AsmInsn` for 'asm'.

### `AsmInsn`

An FP/SIMD instruction to pass through as inline asm. `vregs`: the set of V register numbers touched; `gprs`: a list of
(bit position, guest register or 'sp'); `consts`: a list of (bit position, constant) loaded into a scratch register.

### `Lifter.__init__()`

`base` is the absolute load address of the module (guest equals host). `simd_c` imports `lifter` at module level, so
the import of `simd_c` here cannot move to the top.

### `Lifter.lift()`

`pc` is absolute; returns an `Insn`. Padding, trap words (e7ffdefe) and header data become a trap if ever executed.

### `Lifter.dp_imm()`

Data processing (immediate), by op: 0 and 1 PC-relative (ADR, ADRP); 2 add/sub immediate; 4 logical immediate; 5 move
wide; 6 bitfield, with opc 2 UBFM, 0 SBFM, 1 BFM; 7 EXTR.

### `Lifter.branch_sys()`

Branches and system instructions: `bits(w, 26, 5) == 0b00101` B/BL; `bits(w, 25, 7) == 0b0101010` with bit 4 clear
B.cond; `bits(w, 25, 6) == 0b011010` CBZ/CBNZ; `0b011011` TBZ/TBNZ; `bits(w, 25, 7) == 0b1101011` branch register;
`bits(w, 24, 8) == 0b11010100` exception generation.

BRAA/BLRAA/RETAA/RETAB (pointer authentication, key A or B; op3 0b000010 and 0b000011): the lift runs with no signing
keys, so the authenticate step is a no-op and the branch itself is the ordinary one. RETAA/RETAB carry Rn = 11111 and
return through X30.

### `Lifter.system()`

Hints (op0 0, CRn 2, op1 3, Rt 31) become a `/* hint */` statement and barriers (CRn 3, op1 3, Rt 31) a fence (CLREX,
op2 2, clears the exclusive monitor); SYS (op0 1) handles DC ZVA (op1 3, CRn 7, CRm 4, op2 1) and lifts other cache
maintenance as nothing; MRS (L set) reads system registers.

### `Lifter.ldst()`

`bits(w, 27, 3) == 0b011` with bits 24-25 clear is a literal load.

### `Lifter.stg()`

STG (post-index, signed offset, pre-index). Horizon runs without MTE, so storing an allocation tag changes no memory;
only the base writeback remains (1.4.3 sdk+0x640050).

### `Lifter.excl_pair()`

LDXP/LDAXP/STXP/STLXP. Two W registers form one 64-bit exclusive, two X registers one 128-bit.

### `Lifter.cas()`

CAS{A}{L}{B,H}: Rs is the compare value and receives the old memory value.

### `Lifter.casp()`

CASP{A}{L}: the register pairs (Rs, Rs+1) are compared and (Rt, Rt+1) stored as one 64- or 128-bit access.

### `Lifter.atomic()`

LSE LD<op>{A}{L}{B,H} and SWP{A}{L}{B,H}: Rt receives the old memory value, Rs is the operand. Rs 31 with opc 1 or 4 is
LDAPR (byte, half, word, dword): a load-acquire RCpc into Rt.

### `Lifter.ldst_pair()`

opc 1 of the load form is LDPSW.

### `Lifter.ldst_reg()`

The immediate forms: kind 0 and 2 unscaled and unprivileged, 1 post-index, 3 pre-index. The writeback is written
before the register load, so a load into Rn wins.

### `Lifter.ldst_simd()`

FP/SIMD loads and stores are passed through as asm; the general address registers are remapped. A literal load becomes
an LDR (immediate, offset 0) through a scratch register; `bit 31 == 0` with bits 24-29 0b001100 or 0b001101 are the
structure forms; `bits(w, 27, 3) == 0b101` the pairs.

### `Lifter.dp_reg()`

Data processing (register): bits 24-27 0b1010 logical shifted register; 0b1011 add/sub shifted or extended register,
and 3-source when bit 28 is set; op2 0 add/sub with carry; 2 conditional compare; 4 conditional select; 1-source when
bit 30 is set.

- PACIA/PACIB/PACDA/PACDB/AUTIA/AUTIB/AUTDA/AUTDB (opcode 0-7), their Z forms (8-15) and XPACI/XPACD (16-17): pointer
  authentication. Lifted code never signs pointers, so signing, authenticating and stripping all leave Rd unchanged (as
  RETAB/BRAA are lifted as RET/BR). The Switch 2's hardened `_setjmp` signs x30 with PACIB and checks its low bits
  afterwards.
- CRC32 (opcode 16-23): the host has the same instruction.
- PACGA Xd, Xn, Xm|SP: a 32-bit code of (Xn, Xm) in the top half. Its only use is a checksum compared against a second
  PACGA of the same values (the SDK's `_setjmp`/`longjmp` guard), so a constant code keeps every such comparison equal.

### `Lifter.fp_simd()`

FP/SIMD data processing: `(w & 0x5F20FC00) == 0x1E200000` FP to and from integer; `(w & 0x5F200000) == 0x1E000000` FP
to and from fixed point; `(w & 0x9FE08400) == 0x0E000400` with bit 29 clear AdvSIMD copy; `(w & 0xBFE08C00) ==
0x0E000000` TBL/TBX. Register 31 is zr in these encodings, so it is not passed as a general register.

### `emit_asm_batch()`

Emits one inline-asm block for a run of FP/SIMD instructions.

## `recomp/simd_c.py`

Portable C lowering of AArch64 FP / AdvSIMD / crypto / CRC32 instructions (lifter `--simd=c`). Every encoding is decoded
here at lift time and turned into C over the Cpu struct, calling the static inline helpers in `runtime/simd.h` with
constant fields (sizes, lanes, ops), so clang folds the helpers' switches. It covers ARMv8.0-A + Crypto + CRC32 (the
Switch's Cortex-A57); unallocated or later-architecture encodings (FP16 arithmetic, RDM, dot product, FCMA, JSCVT, ...)
raise `Unsupported`.

`FOP3` maps the vector three-same FP encodings, (U, a, opcode), to their FOP.

### `lop()`

The C name of a lane op (the enum in `runtime/simd.h`), with the unsigned flag.

### `vset()`

C that writes V register d whole (`runtime/simd.h` VSET, VSET128): `e` is a scalar of at most 64 bits, which leaves
the bits above it zero, or, with `full`, a 128-bit value.

### `same()`

A lane-wise op, all operands the same size.

### `SimdC`

The FP/SIMD decoder emitting portable C. Its methods return C statements or raise `Unsupported`. The sections: top
level, crypto, copy / permute / table, three same, three different, two-register misc, across lanes / pairwise,
modified immediate, shift by immediate, indexed element, scalar FP, loads / stores, CRC32.

### `SimdC.dp()`

By op0: `(op0 & 0b1101) == 0b0101` AdvSIMD scalar; `(op0 & 0b1001) == 0` AdvSIMD vector; `(op0 & 0b0101) == 0b0001`
scalar FP.

### `SimdC.copy()`

By imm4: 0 DUP (element), 1 DUP (general), 3 INS (general), 5 SMOV, 7 UMOV.

### `SimdC.permute()`

By kind: 1 UZP, 2 TRN, else ZIP.

### `SimdC._pair_idx()`

Lane indices (into SHUF(a, b)) of the left and right operands of a pairwise op; for Q=0 the padding lanes point at a's
upper half.

### `SimdC.three_same_fp()`

U 0 with opc 0b11001 is FMLA / FMLS: the multiplicand is negated for FMLS.

### `SimdC.three_diff()`

opc 14 PMULL; 4 and 6 ADDHN/RADDHN/SUBHN/RSUBHN, which narrow into the low or high half; 1 and 3 the wide forms.

### `SimdC.misc2()`

opc 0, or 1 with U clear: REV64 / REV32 / REV16; 0b10010 and 0b10100: XTN / SQXTUN / SQXTN / UQXTN; 0b10011 with U: SHLL.

### `SimdC.misc2_fp()`

FP two-register misc, vector and scalar: opc 0b10110 and 0b10111 FCVTN / FCVTL / FCVTXN; 0b01111 FABS / FNEG;
0b11000 and 0b11001 FRINT*; 0b11010 and 0b11011, or 0b11100 without a, FCVT[NPMZA][SU]; 0b11100 URECPE / URSQRTE, and
without a SCVTF / UCVTF; scalar FRECPE / FRSQRTE.

### `SimdC.modimm()`

cmode 0-7 odd and 8-11 odd: ORR / BIC; op set with cmode other than 14 and 15: MVNI.

### `SimdC.shift_imm()`

opc 0b11100 and 0b11111: the fixed-point conversions; 0b10100: SSHLL / USHLL; 0 and 2: SSHR/USHR and SSRA/USRA.

### `SimdC.fp_dp3()`

o1 and o0 pick the negations: FMADD 00, FMSUB 01 (-n), FNMADD 10 (-a, -n), FNMSUB 11 (-a).

### `SimdC.fp_dp1()`

A half-precision source (type 3) has only FCVT.

### `SimdC.fp_int()`

opc 6 and 7 are FMOV.

### `SimdC.ldst()`

`bits(w, 27, 3) == 0b011` with bits 24-25 clear is a literal load.

### `SimdC.ldst_struct()`

Bit 24 clear: multiple structures, where LD1/ST1 (selem 1) move whole registers; set: a single structure.

## `recomp/flags.py`

Function-local NZCV for `recomp/gen.py`.

The lifter writes flags the way the guest does: every flag-setting instruction packs N, Z, C and V into `c->nzcv`
(`subs64()` and friends in `runtime/cpu.h`) and every reader unpacks them (`COND_xx(c)`). Compilers keep NZCV inside a
function, and AAPCS64 neither passes it to a callee nor preserves it across a call. So a generated function keeps its
flags in a local `Flags F` (`runtime/cpu.h`), which clang holds in registers, and writes `c->nzcv` only where it leaves
for another function other than by a call or a return (`plan`).

A flag-setting instruction of a known kind (subtract, add or logic result; 32 or 64 bits) stores its operands in F.a
and F.b. A reader all of whose reaching definitions are of one kind computes its condition from those operands, so
`cmp x1, x2; b.lt` becomes a signed compare in C. Instructions whose result is only known packed (CCMP, FCMP, MSR NZCV,
ADCS) write F.nz, the packed form; a reader whose definitions are of several kinds reads F.nz, and those definitions then
also write it.

A read that can see flags a callee left reads the caller's flags from before the call instead. AAPCS64 code never reads
a callee's flags, so such a read only sits on a path that does not run: in NX2Saria it follows calls to noreturn
functions (asserts) that the compiler placed before another branch's target. `Plan.after_call` counts them.

### `effect()`

(Reads flags, kind of the flags written or None) of one instruction's C.

### `Plan`

Per instruction: the kind its flag reads use, whether its flag definition also writes F.nz, and what it writes to
`c->nzcv` before it leaves for another function. `exit[i]`: the kind of the flags to publish in `c->nzcv` at a tail
exit, or None. `entry`: F.nz starts as `c->nzcv`, because flags from the entry reach a read or an exit. `entry_read`:
they reach a read, not only an exit. `after_call`: the reads that can see flags a callee left.

### `Plan.decl()`

The declaration of F at the top of the function.

### `Plan.publish()`

The statement writing the flags to `c->nzcv` at instruction i's tail exit ('' when it need not).

### `Plan.rewrite()`

The code of instruction i with its flag reads and writes on F.

### `plan()`

`effects[i]`: (reads, kind or None); `succ[i]`: successor indices inside the function; `calls`: the indices of calls,
whose callee may leave any flags; `exits`: indices that can leave for another function other than by a call or a return
(a tail branch, falling into the next function, a br through `rt_dispatch`).

Discovery can start a function inside another one (a branch target, a materialised address), and the code before it
then falls or branches into it with live flags. So a tail exit publishes the flags in `c->nzcv`, and a function whose
reads can see its entry flags starts F.nz from `c->nzcv`. Calls and returns publish nothing: AAPCS64 code neither passes
flags to a callee nor reads a callee's, so a call's flags count as no definition. Flags from the entry alone are still
in `c->nzcv`.

The inner `use` gives the kind a use of the definitions r reads; several kinds read the packed form, which they then
all write. Several kinds, or none (unreachable), give 'nz'.

## `recomp/regs.py`

Function-local general registers for `recomp/gen.py`.

The lifter reads and writes the guest's X0-X30 and SP in the Cpu (`c->x[n]`, `c->sp`), so every guest instruction
loads and stores memory, and the C compiler cannot keep a register in a host register across a guest store: guest
stores go through pointers that may alias the Cpu. A generated function instead copies the `LOCALS` registers it uses
most into locals at its entry (xN, sp_), works on those, and syncs with the Cpu only where something else reads or
writes them: before a call, an SVC, a trap, a return or a tail exit it stores the registers it changed since the last
sync (spill), and after an SVC it loads them all again (reload). The other registers stay in the Cpu.

After a call it reloads only X0-X18 and X30: AAPCS64 has the callee preserve X19-X29 and SP, so their locals still hold
what the Cpu holds. What would break that does not occur in NX2Saria: main and subsdk0 import neither longjmp nor the
C++ unwinder, sdk's longjmp, `__cxa_throw` and `_Unwind_*` have no direct callers, and the runtime writes no register
above X3 of a running thread's Cpu except X30, which `rt_call` restores (`runtime/hostfn.c`). Guest fibers each run on
a host fiber of their own with their own Cpu (`runtime/hle/kernel.c`), and ExitThread leaves by longjmp without
returning. `gen.py --no-callee-saved-trust` reloads those registers too.

Only plain assignments write registers in the lifted C (`c->x[n] = ...`, `c->sp = ...`), and no code takes a register's
address, so the rewrite is textual. A register counts as dirty from a write until the next spill on some path: `plan`
runs that forward over the function's control flow, so a call spills only what changed since the previous call or the
entry. Spilling a register that is not dirty is harmless (the local equals the Cpu), so where a case is unclear the plan
errs on spilling more.

Constants:

- `ESCAPE`: the lifted instructions that hand the Cpu to the runtime: SVCs return, BRK and undefined words do not.
- `SP` 31: the index of SP in the register sets.
- `SPILL`, `RELOAD`, `RELOAD_CALL`: the markers gen.py puts where the Cpu must be current, resolved by `Plan.resolve`:
  spill before; reload all, or reload what a call may change, after.
- `LOCALS` 6: locals per function, the most used registers. More cost more than they save, as clang then spills them
  to the stack and the syncs stay (300 hot NX2Saria functions: fewest instructions and memory operands at 4-6).

### `refs()`

The registers a text reads or writes, once per occurrence.

### `Plan`

`regs`: the sorted registers the function keeps in locals; `spill`: per instruction, the registers to store at its
sync points; `kept`: the registers a call leaves as they were (`CALLEE_SAVED`, or none).

### `Plan.decl()`

The locals at the top of the function, loaded from the Cpu.

### `Plan.resolve()`

The text of instruction i with its registers on the locals and its sync markers expanded.

### `plan()`

`texts[i]`: instruction i's C with markers; `succ[i]`: successor indices inside the function; `clean`: the instructions
after which every local equals the Cpu (a call or an SVC that spills and reloads on every path through it); `n_locals`:
how many registers to keep in locals; `kept`: the registers a call does not reload. An instruction not reached from the
entry in the plan's flow may have anything dirty. A sync point stores what was dirty on entry to its instruction and
what the instruction wrote before it (`c->x[30]` of a call): all the instruction's writes, which is at most a little
more.

## `recomp/jumptables.py`

Static jump-table resolution for a computed `br`.

Clang lowers a dense switch to a bounds check (`cmp wI, #N; b.hi default`, or `b.ls` to a dispatch block placed
elsewhere), a table load (`ldrb/ldrh/ldrsw wO, [xT, xI, lsl #s]`) and an add to an anchor (`adr xB, L;
add xB, xB, wO, sxtb #2`, or the table address itself). The resolver walks back from the `br` over its block and the
predecessor blocks (`_explore`), evaluates each path forward symbolically (`_run`), and enumerates the values the path
conditions (bounds checks, ccmp chains, cbz/tbz) or the construction (masks, bit inserts, selects of constants) allow
for the unknowns the target depends on (`_solve`). Table entries come from `.text`/`.rodata` of the unrelocated image.
Anything it cannot prove is rejected.

Values are expression tuples over the registers at the top of the path. 32/64-bit views are normalised
(`lo(zx(e)) = e`, `uxtw`, `and #0xffffffff`, ...) so that the copies and extensions clang inserts compare equal.
Registers at a path top come from a function-wide must-analysis (`_facts`): constants, small constant sets, terms over
named values, zero-extension. That carries table bases clang hoists out of loops and index copies made before them.

A function is resolved as a whole (`_function`): table targets are blocks and edges for the facts and the paths, and
the tables and the facts are iterated to a common fixpoint. Assumptions: control enters [entry, end) only at the entry,
at targets of direct branches (seen module-wide), at exception landing pads (`.eh_frame` LSDAs) and through its own
table jumps; an unresolved table never enters another switch between its bounds check and its `br`. Work per function
is capped (`MAX_STEPS`); past the cap nothing in the function is resolved.

The limits: `MAX_BLOCK` 64 instructions in one block; `MAX_DEPTH` 4 predecessor blocks above the dispatch block;
`MAX_PATHS` 32 paths explored per br; `MAX_COUNT` 4096 table entries; `MAX_CANDS` 8 candidates combined in pairs and
triples; `MAX_SET` 8 constants a register fact may hold; `MAX_STEPS` 2^19 work units per function (instructions
evaluated, blocks met, assignments tried); `MAX_ROUNDS` 8 rounds of the per-function table/fact iteration.

The sections of the file: expressions, symbolic execution of one path, control flow, enumeration, exception landing
pads, per-function resolution, interface.

### `lo()`

The low 32 bits of a 64-bit value.

### `bfm()`

UBFM/SBFM of the sz-bit value x.

### `bfi()`

BFM (bfi, bfxil): bits of `src` inserted into `dst`.

### `extend()`

An extended-register operand: the option applied to the 64-bit register value e, result sz bits.

### `_State`

`regs`: register 31 is SP. `epoch`: the memory version; loads of different versions are different values. `mem`:
[(base, offset, size, value)], the stores of this version, disjoint. `conds`: the conditions the branches on the path
prove (`_edge`).

### `_State.kill()`

r gets an unknown value; a 32-bit write leaves the upper half zero.

### `_State.clobber()`

Memory changed in an unknown way.

### `_State.store()`

`value` (64-bit for 8 bytes, else 32-bit) stored at `addr`. Stores that may overlap are forgotten: same base and
overlapping offsets, or a different base.

### `_State.load()`

The sz-bit value of the size-byte load at `addr`, forwarded from a store when the same bytes were stored.

### `_addr_imm()`

Base and offset of the load/store register forms (immediate and register offset): bit 24 set unsigned offset; bit 21
set register offset; mode 1 post-index; mode 3 pre-index.

### `_ldst()`

`(w & 0x3B000000) == 0x18000000` load literal; `(w & 0x3A000000) == 0x28000000` pair; size 3 of the PRFM form;
`(w & 0xBE000000) == 0x0C000000` SIMD structures (multiple and single); `(w & 0x3F000000) == 0x08000000` exclusives,
acquire/release; `(w & 0x3B200C00) == 0x38200000` LSE atomics.

### `_dp_imm()`

op 0 and 1 ADR, ADRP; 2 add/sub immediate; 4 logical immediate; 5 move wide; 6 bitfield (opc 1 BFM: bfi, bfxil); ROR
is EXTR with Rn = Rm.

### `_dp_reg()`

`(w & 0x1F000000) == 0x0A000000` logical shifted register; `(w & 0x1F200000) == 0x0B000000` add/sub shifted register;
`(w & 0x1F200000) == 0x0B200000` add/sub extended register; `(w & 0x1FE00000) == 0x1A400000` with bit 29 ccmp, ccmn
(rmif and setf leave the flags unknown); `(w & 0x1FE00800) == 0x1A800000` without bit 29 csel, csinc, csinv, csneg;
everything else (adc, sbc, 1/2/3-source) gives an unknown. Equal expressions are equal values, so a select carries what
picks the arm: the condition and the flags record, or a fresh unknown when the flags are not known (two `cset wN, ne`
on different compares differ).

### `_fp_simd()`

`(w & 0x5F203C00) == 0x1E202000` or `(w & 0x5F200C00) == 0x1E200400` fcmp, fccmp; `(w & 0x5F200000) == 0x1E000000` or
`(w & 0x5F20FC00) == 0x1E200000` conversions with a general register (rmode/opcode 0, 1, 4, 5, 6: to a general register,
fcvt* and fmov Rd, Vn); `(w & 0x9FE08400) == 0x0E000400` with imm4 5 or 7 smov, umov.

### `_step()`

The effect of one non-branch instruction: `(w & 0xFC000000) == 0x94000000` or `(w & 0xFFFFFC1F) == 0xD63F0000` bl, blr;
`(w & 0xFFFFF01F)` 0xD503201F or 0xD503301F hints and barriers; `(w & 0xFFF00000) == 0xD5300000` mrs;
`(w & 0xFFF00000) == 0xD5100000` msr (system register), which may write NZCV; `(w & 0xFFF80000) == 0xD5080000` sys: dc,
ic, tlbi.

### `_edge()`

Records the condition a conditional branch edge proves: `(w & 0x7E000000) == 0x34000000` cbz, cbnz;
`(w & 0x7E000000) == 0x36000000` tbz, tbnz.

### `_init()`

The registers at the top of a path, from the function-wide facts (`_facts`).

### `_Exhausted`

The per-function step budget ran out.

### `_run()`

The symbolic state at the br that ends path [(start, last, taken)] (top block first) and the br register's value.
`taken` says which edge of a conditional branch at `last` the path follows; None for any other block end
(fall-through, unconditional branch, table jump).

### `_branch_target()`

The target of a direct branch (b, b.cond, cbz/cbnz, tbz/tbnz), else None.

### `_no_fallthrough()`

b, b.al, br/ret/eret (the branch-register forms other than blr), udf, brk, hlt.

### `_branch_index()`

(targets, sources) of every direct branch in `.text`, sorted by target; cached per module.

### `branch_sources()`

{target: [sources]} of the direct branches anywhere in `.text` that land in [entry, end).

### `_Cfg`

The basic blocks of [entry, end) with direct, fall-through and table edges. `tables` maps each table-shaped br to its
targets so far, or None when it is unresolved and may reach any block. Leaders are the entry, direct branch targets,
landing pads, table targets and the instruction after each block-ending instruction. `unknown` holds the blocks entered
in ways the function does not show: the entry and the targets of branches from other functions. `preds` is
[(last instruction of the predecessor, taken)] per block.

### `_Cfg.block()`

The start of the block holding a.

### `_Cfg.path_preds()`

[(start, last, taken)] predecessor segments of block b, or None if some entry is not visible. Unresolved table jumps are
not counted: a table is assumed never to enter another switch's dispatch path after its bounds check.

### `_facts()`

{block start: {reg: fact}} for the registers whose value is known the same way on every path into the block.

A fact is a constant, a small set of constants ('set', values), a term, or 'z' (a zero-extended 32-bit value). Terms are
expressions over names ('v', A, r) / ('vw', A, r): the 64-bit / 32-bit value the most recent execution of instruction A
wrote to r. They keep copies and extensions of one value equal across blocks (`mov w12, w8` before a loop, the check on
w8 inside it). A name dies when its instruction executes again.

A forward must-analysis over `cfg`, optimistic for blocks not yet reached. An exception landing pad is entered from the
calls of its call-site ranges with the caller-saved registers clobbered; an unresolved table jump may enter any block.
None if the analysis does not settle. A block whose input is the same as in the last round is skipped: the transfer
gives the same output.

### `_clean()`

e is built from constants and names only, and is small (larger values get a name).

### `_names()`

e mentions a name written by instruction a.

### `_Unknown`

The value depends on something the assignment does not fix (a register, or memory other than `.text` and `.rodata`).

### `_eval()`

The value of e with the subexpressions in env [(expr, value)] fixed.

### `_const_mem()`

[a, a+n) lies in `.text` or `.rodata` (no relocations, never written).

### `_flags()`

The possible NZCV values of the flags record f under env; None for one that is not determined.

### `_violated()`

The path condition rec is provably false under env.

### `_bounds()`

[(expr, values)] that the path conditions bound: x <= N, x < N, x == N, x == 0. An edge the fixed flags alone could
take bounds nothing.

### `_indices()`

The subexpressions of the addresses of the loads in e (candidate table indices).

### `_domain()`

The values e can take by construction (masks, narrow extracts, selects of constants), or None. A mask m gives every
submask of m; a bfm with no sign extension and a shift below the width and below 8 is a ubfiz.

### `_solve()`

The targets of t over the values the path proves for the unknowns in it, or None. Candidates are values bounded by path
conditions and, with masks, values bounded by construction. One, two or three candidates must together fix t; every
assignment the path conditions rule out is dropped. A mask is a sound bound but may far exceed the table; with a bounds
check on the path (of some other value) it is only taken if no larger than that check. A combination that fixes t for
some values only gives None; the first feasible value that leaves t unfixed rules that combination out.

### `_enc()`

The DWARF pointer encoding at image offset o: (value as image offset or plain number, next offset).

### `landing_pads()`

{landing pad: [(start, end)]}: the call-site ranges of each pad, from `.eh_frame` and the LSDAs.

### `_explore()`

The targets of the table jump at `br_pc` over every path into its dispatch block, or None.

### `_round()`

The resolution of every br in `brs` over the blocks, facts and predecessors that the table edges `tables` give.

### `_function()`

{br: set of targets or None} for the table-shaped brs of [entry, end); cached for the last function.

Table edges change the blocks, the facts and the predecessors the paths may take, so the function is resolved as a
whole. Growth: start without table edges, resolve, add the targets found as edges, until no table grows (a table found
unresolved here may still resolve once the blocks before it are reached). Check: give every unresolved table edges to
every block, resolve again, drop any table that no longer resolves and grow any whose result its edges do not cover (an
unresolved one included), until all are covered. By induction over an execution, the facts and every resolved table
then hold together. The work is capped at `MAX_STEPS` evaluated instructions; a function that needs more gets no
tables.

### `table_targets()`

Every target the `br` at `br_pc` can reach through a proved table index, anywhere in `.text`; None if unproved.

### `resolve()`

The targets of the `br` at `br_pc` when it dispatches through a bounds-checked jump table inside [entry, end). `mod` is a
`recomp/module.py` `Module`. Returns a sorted list of distinct target addresses, every one in [entry, end) and 4-byte
aligned, or None when the br is not a recognised table jump.

### `_table_shape()`

The br value is a code address, or address arithmetic over a loaded entry or an image address. A value loaded as a
whole is a pointer (vtables, function-pointer arrays in `.data`): a call, not a table.

### `_refers()`

e contains a load or an image address and something unknown or loaded.

### `is_computed()`

True if the br register comes from address arithmetic (a table) rather than a load (a pointer call). Decided on the
straight-line code before the br alone, so it does not depend on the function bounds.

## `recomp/patches.py`

The code patch table for the generator: guest instructions replaced in the lifted C, switchable at run time.

Lifted code cannot be byte-patched when the game runs, so a patch is applied when the C is generated. A patch file
(`gen.py --patches`) has three kinds of lines; `#` starts a comment, fields are separated by whitespace:

```
set  <name> <env> <default>               a patch set: a run-time switch <env>=0/1, <default> when it is unset
site <set> <module> <address> <original> <replacement> [<replacement> ...]
spin <module> <head> <back-edge> [<back-edge> ...]
```

`<address>` is a module offset (hex), `<original>` the instruction word the module has there (checked; a mismatch stops
the generator), each `<replacement>` an instruction word (hex) or `@page:x<n>`, which is `adrp x<n>, <patch page>`
encoded for the site's address. The patch page is the first 4 KiB page after main's image (`recomp/layout.py` leaves
the rest of the 2 MiB-aligned module slot free); the runtime commits it and owns its contents. Words in use, by byte
offset:

| Offset | Use |
|---|---|
| +0..15 | the frame-rate step (`runtime/pacing/frame_rate.c`) |
| +16..55 | the output, swapchain and scene sizes (`runtime/gfx/resolution.c` `res_init()`) |
| +56..59 | the camera's aspect ratio (`runtime/gfx/resolution.c` `res_init()`, set `aspect`) |
| +60..63 | the field-of-view factor (`runtime/gfx/resolution.c` `res_fov_live()`, set `fov`) |
| +92..95 | the sun shadow map size (`runtime/gfx/resolution.c` `shadow_init()`, set `shadow`) |
| +776..779 | the ocarina staff's last note signal (set `ostaff`, written by the game's code) |

A new set takes words outside these. (The host functions some sets call sit higher on the page; their docs give the
offsets.)

The replacement words are lifted at the site's address in place of the original instruction, and the generated C reads
`if (rt_patch_on[<set>]) { replacement } else { original }`: one byte load and a branch per executed site. Several sets
may patch one address (cheats that write different values there): `if (a) {..} else if (b) {..} else {..}`, the first
set in file order wins. The original may be any instruction but br, blr and ret; a replacement's last word may be a bl
(lifted with the site's return address), a b or b.cond / cbz / tbz (its target joins the function's labels when inside
it) or a ret; earlier words must not branch. Every alternative of a site writes the same flags. The generator writes
`patches.h` (set names, switches, the page address) next to `layout.h` for the runtime.

A `spin` line names a spin wait found by `tools/spin_waits.py`: a loop with head `<head>` that only another thread can
end, and the direct branches back to it (module offsets). Each back-edge (checked: a b, b.cond, cbz / cbnz or tbz / tbnz
to `<head>`) is lifted as `rt_spin_gate(c, <head>); goto <head>`, which keeps every register and only adds time
(`runtime/hle/kernel.c` `rt_spin_gate()`).

### `Table`

`sets`: [(name, env, default)]; `sites`: {module: {offset: [(set index, [word | ('page', reg)], original)]}} (the
alternatives of a site in file order); `page`. `spins`: {module: {back-edge offset: head offset}}.

### `Table.spins_for()`

{absolute back-edge address: absolute head address}.

### `Table.for_module()`

{absolute address: [(set index, [words])]} with `@page` resolved.

### `load()`

A `Table` from a patch file; `mods` is `layout()` output [(name, base, size)].

### `check()`

Stops on a site whose original word differs from the module's.

### `branch_target()`

The target of the direct branch w at a (b, b.cond, cbz / cbnz, tbz / tbnz), or None.

## `recomp/layout.py`

The guest address layout of the loaded modules, computed from their sizes.

Guest addresses equal host addresses, and the lifter writes absolute addresses into the generated C, so the generator
and the runtime must agree on every module base. This is the single place that decides them: the generator calls
`layout` and writes the result into `generated/<version>/layout.h` and `layout.json`, which the runtime and the build
read.

Modules are placed in Horizon's load order (rtld is dropped; the host links the modules): main, subsdk0..9, sdk. Each
base is aligned to 2 MiB. Everything must stay below 2^39, the limit of the addresses IPC buffers can encode.

### `layout()`

[(name, base, size)] for the modules present in the ExeFS directory, in load order.

### `write()`

`layout.json` for the Python tools and `layout.h` for the runtime.

### `read()`

[(name, base, size)] from a `layout.json` written by `write`.

## `recomp/module.py`

One loaded NSO module as the generator and its analyses see it: the image at a guest base, `.text` as words, function
seeds, and bounded-cache instruction lifting. The function seeds and the `.eh_frame_hdr` reader carry a licence notice
(kept as a comment in the file).

`LIFT_CACHE` (2^16) is the number of lifted instructions kept per module; the analyses revisit nearby addresses only.

### `_read_encoded()`

A DWARF-encoded pointer: format 0x0B sdata4, 0x03 udata4, 0x0C and 0x04 sdata8 and udata8; application 0x10 pcrel,
0x30 datarel.

### `Module.read()`

The little-endian integer of n bytes at guest address a (unrelocated image).

### `Module.lift()`

The lifted instruction at a; `Unsupported` becomes an `rt_udf` trap with the reason in a C comment.

### `Module.eh_frame_functions()`

Function starts from `.eh_frame_hdr`'s binary search table (absolute addresses).

### `Module.seeds()`

Known function starts: `.eh_frame_hdr`, exported FUNC symbols, `R_AARCH64_RELATIVE` targets in `.text`.

## `recomp/nso.py`

The NSO loader: decompresses the segments, builds the memory image, parses MOD0 and the dynamic info.

### `Nso.__init__()`

Segment flag 0x80 is ZBIC (Switch 2 modules): Nintendo's zstd variant.

### `Nso._parse_mod0()`

The dynsym count comes from DT_HASH's nchain, or from DT_GNU_HASH (1.4.3's sdk and subsdk0 have no DT_HASH). DT_RELR
(1.4.3): packed RELATIVE relocations whose addend is the word in place. An even entry is an address (relocate it,
continue after it); an odd entry is a bitmap of the next 63 words.

### `Nso.symbol_count()`

The number of `.dynsym` entries: DT_HASH's nchain, else one past the last symbol a DT_GNU_HASH chain reaches.
