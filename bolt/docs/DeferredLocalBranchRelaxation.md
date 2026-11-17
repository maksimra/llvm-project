# Deferred local branch relaxation investigation

## Scope and comparison

The working change is based on `c3285cb84105` on `deferred-stub-creation`.
`LongJmpPass` keeps its early deferred-mode return (after BTI preparation).
All local relaxation decisions now run inside `RewriteInstance::emitAndLink`,
including parallel planning of the fragments that are actually emitted.

The source comparison is `git diff 1293726 9db19db`. In 1293726,
`relaxLocalBranches` first splits terminal B instructions into trampoline BBs,
without adding bytes. It reuses a reachable trampoline to the same target BB,
then tries an end trampoline for a zero-count taken edge. Failing these, it
inverts a reversible branch with a fall-through successor and inserts a B;
an irreversible predicate needs an additional branch to protect fall-through.
It updates the CFG and subsequently finalizes unwind state.

9db19db moves local relaxation into serial emission. For simple functions it
always expands far conditionals inline, never reusing B sites or outlining
cold edges. Its repeated layout/decision passes scan every instruction. For
non-simple functions it shares end trampolines by symbol/addend. c3285cb fixes
the deliberately conservative alignment accounting in 9db19db, using exact MC
padding and prefix offsets, but retains the same branch selection and rescans.

The late planner transfers reachable-B reuse and cold-edge outlining, without
splitting or moving original instructions. It additionally swaps the two
instructions of `B.cond far; B near` when a short inversion reaches near.
Every selected destination is rechecked after growth; failed choices fall back
to monotonic inline expansion. Reuse groups include the addend and CFI epoch.
A reused B that changes destination through a pair swap is revalidated too.

## Profile and unwind evidence

InstructionLowering only lowers tail calls; successor vectors and their
BinaryBranchInfo counts survive. CFG analyses based on call/tail-call opcodes
are no longer valid. The planner reads a zero count only with a valid function
profile, a matching successor symbol, and zero addend. BB counts alone are
insufficient: normalization can produce zero counts without input profile.
External lowered tail calls have no trustworthy successor edge.

Reused B sites require an identical CFI epoch within their fragment. This is
conservative: equivalent rows reached through different CFI instructions are
not recognized. It includes PAC state and opaque escape directives in the
equality proof. New end trampolines use remember_state at their sources and
restore_state in reverse source order. GNU_args_size is restored separately.
The original epilogue's unwind row is therefore not used for the new B.
Opaque CFI or existing remember/restore sequences disable outlining; a
fall-through continuation also requires the source and end epochs to agree.
Inline expansion remains available for simple functions in these cases.
Non-simple functions diagnose an unreachable or unsafe end trampoline.
Fixed-footprint patches may reuse existing instructions but cannot grow.

## Parallelism and work bound

BinaryEmissionPlan uses the same emission predicates and output list as the
emitter, including injected functions omitted by runOnEachFunction's input map.
Serial preparation handles function prefixes, break markers and constant-island
symbol rewriting. Workers use only const instruction/opcode/annotation reads,
const BB label/edge lookups and private containers. Symbolic addends are decoded
without MCExpr evaluation, which can modify variable-symbol state. Unknown
expressions cannot use range proofs or trampoline sharing; the emitted B keeps
the complete original operand. No MCContext/MCStreamer mutation or diagnostic
emission occurs in workers. Symbol creation and application happen in original
emission order, after pool.wait().

The pool is ParallelUtilities::getThreadPool(), with BOLT's thread/task options.
Jobs group at least 32768 instructions to bound small-fragment scheduling costs.
One initial instruction walk records candidate positions and BB byte sizes.
Each cascade iteration recomputes all block offsets, including exact bounded or
unlimited alignment, but only visits branch sites inside the blocks. The fast
path uses the concrete opcode's signed range (CMPBR, TBZ, CBZ/B.cond), maximum
possible inline growth and alignment padding. It excludes cross-fragment
references and nonzero/unrecognized addends. New end-trampoline ranks account
for reverse source order without repeated vector insertion/offset updates.

## Interpretation of the supplied Clang numbers

The supplied total increase is 24.7 s (5.84%); emit/link increases by the same
24.7 s (14.68%). The remaining measured phases sum to 254.7 s in both runs.
This localizes the observed increase to emit/link, but moving work from the old
parallel pass into that timer means the timer difference is not itself the
cost of the new planner. Repeated whole-instruction scans and serialization
are supported by the source comparison and synthetic measurements below.

Each reserved slot occupies 20 bytes. Ten additional slots contribute only
200 of the 63484 extra code-section bytes, leaving 63284 bytes attributable to
other layout/code/alignment changes. The slot count alone cannot explain the
size difference. A complete byte attribution requires the two Clang outputs.

For a cold taken edge, inline inversion turns the formerly hot fall-through
into a taken conditional branch. A conditional/B pair can also gain a redundant
B with the 9db19db form. The regressions demonstrate both changes in instruction
shape and their removal. They support the branch-behavior hypothesis but do not
establish the cause of the reported 33% branch-miss increase or the TU slowdown
from 96.2 to 101.0 s (4.99%). That needs the original AArch64 Clang/profile/TU,
repeated target-machine runs and branch-PC attribution. Those inputs are not
available in this workspace; this x86 host cannot provide native AArch64
runtime measurements.

## Reproduction and validation artifacts

Builds and logs are isolated under `/tmp/bolt-late-plans`; installed build
outputs were not overwritten. `build.py` uses the configured Release/assertions
compile commands and existing LLVM/BOLT archives. Historical emitter/LongJmp
variants use common supporting libraries to isolate relaxation policy; the
saved source files for the old variant match 1293726 exactly. These are focused
comparisons, not complete fresh LLVM builds of each historical commit.

`make-benchmark.py` generates 128 AArch64 functions, each with 32768 NOPs, a
conditional/B pair moved out of short range by reverse BB order, and an FDE.
`benchmark.py` alternates variants and runs three repetitions at 1 and 4 threads.
Results are timer wall times, not CPU-time sums. This is a planning/emission
microbenchmark, not a Clang throughput benchmark.

Median seconds from the final three runs (1 thread / 4 threads):

| Variant | emit/link | BOLT timed region | Whole process wall |
| --- | ---: | ---: | ---: |
| 1293726 policy | 0.3256 / 0.3480 | 2.9016 / 2.6332 | 5.1249 / 4.3591 |
| 9db19db policy | 0.4550 / 0.5209 | 2.4737 / 2.4942 | 4.5147 / 3.8974 |
| Working plans | 0.3504 / 0.3347 | 2.4171 / 2.2555 | 4.5485 / 3.6319 |

The new planning timer measures 0.0502 / 0.0226 s (2.22x speedup).
Emit/link is 23.0% / 35.7% below 9db19db on this fixture. The whole-process
one-thread time does not improve; three runs on a shared laptop are insufficient
to claim a small end-to-end gain. The emit/link samples range from 0.3405 to
0.4262 s at one thread and 0.3174 to 0.3664 s at four threads. Earlier measurements
are kept in `benchmark-initial.json`; they also show an emit/link reduction.
Parent 1293726 plans before the emit/link timer, so that column alone cannot be
used to compare its total relaxation cost with late planning.

The new code is 16780800 bytes versus 16781312 (512 fewer bytes, four per
function). All variants reserve zero slots in this fixture. This directly
measures the conditional/B pair optimization, independently of stub counts.
It does not reproduce the production Clang size difference.

Raw commands and results are in `benchmark.json`, individual logs,
`core-final.log`, `focused-final.log`, `aarch64*.log`, `x86-metadata.log`, and
`determinism-final.log`.

- CoreTests: 67 passed, 33 architecture-dependent skips.
- Four focused local-branch tests passed, including TBZ cascades, exact BB
  alignment/budgets/prefixes, live-flags CMPBR, hot/cold fragments, non-simple
  functions, reachable-B reuse, pair inversion, source-specific PAC/CFA rows,
  reverse-order CMPBR end-trampoline range, and DWARF 4/5/LSDA semantics.
- Unit coverage includes an injected function absent from the input map,
  `(symbol + 16) - 4` in a trampoline relocation, a four-byte fall-through patch
  retaining its exact footprint, and rejection of an unrelaxable patch branch.
- Twelve existing X86 CFI/DWARF/LSDA tests passed.
- AArch64, consolidated across the full run and focused/serial reruns:
  131 passed, nine unsupported, seven input-compilation failures with the
  installed Clang (CMPBR/CREL/pauth-lr/defsym). Those same seven failures
  reproduce with the historical baseline before BOLT runs. Two ordinary
  --relax-exp tests initially exceeded the default 60-second timeout; both
  passed in a separate serial run with a 240-second limit (77.05 s together).
  See `aarch64.log`, `aarch64-final.log`, `focused-final.log`,
  `toolchain-baseline.log` and `slow-retry.log`.
- The final rank/addend changes were rechecked with CoreTests and all four
  focused tests, including the CMPBR end-trampoline displacement boundary.
- `determinism.py` links the DWARF/LSDA fixture with the 128-function workload,
  ensuring workers really run in parallel. With --no-threads/1 thread versus
  four threads, all executable sections, CFI, LSDA, DWARF, BAT, and 130 function
  symbols match: ten sections for DWARF 4 and twelve for DWARF 5. Both outputs
  pass llvm-dwarfdump --verify and the semantic LSDA checker.
- git diff --check passed. No ThreadSanitizer run or native AArch64 runtime
  performance measurement was performed.
