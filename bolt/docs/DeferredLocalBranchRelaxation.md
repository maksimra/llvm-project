# Deferred local branch relaxation

Local relaxation is enabled only by the emitter call inside
`RewriteInstance::emitAndLink()`. `LongJmpPass` retains its deferred-mode early
return. There is no public planning object, candidate engine or custom scheduler.

The late algorithm follows `LongJmpPass::relaxLocalBranches`:

1. Lay out the fragment, accounting for MC alignment budgets and body prefixes.
2. Register terminal B instructions and B's introduced by earlier expansion as
   reusable trampolines.
3. For a far conditional, first reuse a reachable B to the same symbol/addend.
4. For a zero-count taken edge, try a shared end-of-fragment trampoline.
5. Otherwise expand inline, preserving flags for irreversible CMPBR predicates.
6. Repeat until the selected branches fit, recomputing the full layout only
   after inline growth; otherwise revisit the recorded branch references.

The conditional/B pair optimization additionally uses `B.!cond near; B far`
when near fits the short encoding. A failed reuse or pair choice is rechecked
and falls back to inline expansion. Unrecognized expressions retain their full
operand and cannot use sharing or local range proofs.

When updating debug info, synthetic terminal Bs with local successors are split
into separate BBs before planning. Instructions and trailing CFIs keep their
emission order; alignment stays on the original BB. Lowered tail calls,
non-simple functions and patches are left alone. Reuse requires no CFI change
between source and trampoline. End trampolines save source rows and restore them in reverse source
order; GNU_args_size is emitted explicitly.
Opaque CFI and differing unwind rows at a fall-through continuation disable
outlining. Fixed-footprint patches cannot acquire extra instructions.

Synthetic branch BBs have no input range. Existing debug-range translation can
therefore stop at the original BB's end, excluding the added B without a
separate instruction-end address map.

InstructionLowering preserves successor vectors and counts but removes tail-call
annotations. Cold-edge decisions therefore require a valid function profile,
a matching successor and zero addend. Normalized zero counts alone are not
profile evidence. External lowered tail calls do not provide such an edge.

Parallelism uses the existing BOLT runOnEachFunction scheduler. Each function's
fragments are processed independently; injected functions are included through
the output list. Workers only read instructions/CFG/expressions. Constant-island
symbol preparation, label creation and MCStreamer operations remain serial.

The supplied Clang timing increase is 24.7 s, entirely within emit/link. Moving
work into that timer does not by itself identify the new planner's cost.
Ten reserved slots contribute 200 bytes directly out of the 63484-byte growth;
the rest requires attribution to emitted instructions and alignment. Cold-edge
inversion and redundant B instructions can change dynamic branch behavior, but
the reported 33% branch-miss increase and 96.2-to-101.0 s TU slowdown still need
the original AArch64 Clang/profile/TU and target-machine measurements.

Validation artifacts for this rewrite are in `/tmp/bolt-local-transfer`.

Validation: 67 CoreTests passed (33 architecture skips); the broad AArch64 run
passed 129 tests, excluding the seven known Clang input-compilation failures and
two slow non-deferred tests. After the offset-cache change, all four focused
local-branch tests and CoreTests passed again. The shape regression now also
checks reuse of a B introduced by an earlier inline expansion. Twelve existing
CFI/DWARF/LSDA tests passed. Executable sections, DWARF 4/5, CFI, LSDA, BAT and
function symbols match with one and four threads; DWARF and LSDA checks passed.

On the 128-function/16 MiB fixture, three-run median emit/link times are:

| Local relaxation | 1 thread | 4 threads |
| --- | ---: | ---: |
| 9db19db | 0.4135 s | 0.3797 s |
| Simplified late loop | 0.3384 s | 0.3146 s |

The fixture emits 512 fewer code bytes and reserves zero slots in both variants.
Historical emitter/LongJmp variants use common supporting libraries. These are
synthetic measurements, not a repeat of the supplied Clang/TU benchmark.
Commands, raw timings and validation logs are in `/tmp/bolt-local-transfer`.
`git diff --check` passes.

The variant that splits synthetic branch BBs and removes instruction-end maps
passes 26 selected lit tests and 67 CoreTests (33 architecture skips). DWARF 4/5,
CFI, LSDA, BAT and function symbols match between one- and four-thread runs.
The updated checks also cover a conditional/B pair split across adjacent BBs.
Logs and comparison scripts are in `/tmp/bolt-split-branches`.
