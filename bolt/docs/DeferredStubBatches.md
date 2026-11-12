# Deferred AArch64 stub placement

`--turn-off-stubs` defers AArch64 branch relaxation to JITLink. It requires an
ELF binary with relocations and does not run `LongJmpPass`. Both local and
inter-function branches are repaired after final addresses are known. Runtime
objects use the ordinary linker path.

## Components and phase boundaries

| Component | Responsibility |
| --- | --- |
| [`Core/DeferredStubs`](../include/bolt/Core/DeferredStubs.h) | Select the deferred mode, emit safe branch forms and their records, and detect fall-through. |
| `BinaryEmitter` | Emit private `.bolt.fragment.N` sections, conditional branch reservations and fall-through guards. Record logical output sections and entry alignment preferences. |
| [`DeferredStubLinker`](../include/bolt/Rewrite/DeferredStubLinker.h) | Import branch records, validate and merge fragment sections, reserve shared islands, select conditional forms and materialize stubs. Own reservation state for one linked object. |
| `PrepareDeferredBTI` | Prepare known indirect destinations before CFG finalization. |
| `JITLinkLinker` | Register passes and provide the common block-layout and rebasing helpers. |
| `ExecutableFileMemoryManager` | Allocate the placed extent and copy blocks at their assigned offsets. |

The emitter and Core helpers do not depend on the linker implementation.
`DeferredStubLinker::addPasses` registers pre-prune branch import, post-prune
placement and pre-fixup branch selection followed by stub materialization. Its
owner, the JITLink context, keeps reservations alive until linking completes.
Section mapping supplies virtual bases between these phases.

MC resolves non-branch references within a fragment. Separate input sections
preserve inter-fragment relocations and provide insertion boundaries. Before merging,
the linker requires one block per emitted fragment and validates that relocation
addends stay within that fragment. Metadata may refer one past its end; branches
may not. The LSDA's `LPStart-1` arithmetic base is allowed separately.

Block addresses are emission-order keys before placement, section-relative
offsets before allocation, and virtual addresses after mapping. Unplaced blocks
use `UINT64_MAX` so ordering keys cannot be mistaken for final positions.
`getSectionAllocationSize` is valid only after section-relative placement and
before rebasing. Rebasing uses actual buffer offsets and can be repeated.

## Conditional branch reservations

Short branches must reach JITLink even when MC could resolve their target in
the same input section. Local size estimates do not account for the basic-block
alignment applied during emission. Reserving an expansion after emission would
also invalidate resolved references and unwind metadata inside the fragment.

For simple functions, emit a safe long form immediately: invert the condition
over a `B` in eight bytes, or use `condition -> taken; B continue; taken: B
target` in twelve bytes when flag-preserving inversion is impossible. Non-simple
functions retain their internal offsets and reserve trampolines at the end of
the fragment, behind a skip branch. Fixed-size patches cannot reserve extra code.

Each tail trampoline inherits the unwind state of its source branch. Emission
saves distinct source states with CFI remember-state directives and emits the
pool in reverse order, restoring the corresponding state before each group.
Replaying the CFI suffix after the pool recovers the body's final state for
ordinary fall-through and its island guard. This preserves complete unwind
rules, including CFA expressions and return-address signing state.

The non-allocatable `.bolt.branch_info` section contains 32-byte records. Three
ordinary Pointer64 relocations identify the source, actual destination including
its addend, and optional long leg. The remaining bytes store the original branch
encoding with zero displacement, its signed byte-displacement bit width, and
its inline reservation size. Code contains only safe local fixups or numeric
placeholder displacements. This also handles CMPBR, which lacks an ELF branch
relocation. B/BL use the same records without reserving extra instructions.

Before pruning, import these records into fallback Branch26 edges and remove
the metadata section. Placement therefore sees every potential long leg. After
mapping and external lookup, check the exact original branch displacement. If
it fits, restore that branch, fill its unused reservation with NOPs and disable
the long-leg fixup. Otherwise, keep the long form and let ordinary stub
materialization repair its Branch26 edge. Tail trampolines require an additional
exact short-range check. No size, symbol offset, or placement changes here.

This initial implementation reserves space for every symbolic conditional
branch. Unused bytes remain in the output; it does not compact reservations.
Code-only measurement emits the same branch forms without linker records.
The ADR/LDR preparation pass always uses ADRP forms for movable functions in
deferred mode. It does not predict local displacements from function sizes.
This may add an instruction even when the final destination would fit ADR/LDR.

## Placement

`AArch64StubPlacer::run` in
[`DeferredStubLinker.cpp`](../lib/Rewrite/DeferredStubLinker.cpp) processes each
executable section in one forward sweep. B, C and D index its original blocks:

```text
B [current batch] C [placed look-ahead] D [unplaced fragments]
                                       ^ new island
```

1. Advance D, placing whole fragments while leaving a 16 MiB island allowance
   within Branch26 range (128 MiB) of B. Accept at least one fragment.
2. Select C: approximately 32 MiB of code, also bounded by the number of branches
   not proven direct in the placed prefix, at 20 bytes per candidate. Sharing
   can reduce the actual number of slots. Large fragments remain indivisible.
3. Visit B..C in source-address order. A proven direct branch needs no slot;
   otherwise reuse a reachable reservation or append one to the island at D.
   Advance B to C. Accepted fragment placements never change.

An island is a JITLink block from its first slot onward. It grows without a
content buffer while slots are added, and each slot immediately gets a symbol.
At the end of the batch, allocate its final size and fill it with NOPs. No slots
means no island. Every actual slot is checked for reach and capacity; fragments
that cannot be served are diagnosed before allocation.

The sharing map identifies destinations by addressable object plus offset and
addend, or by absolute address. Only the latest reservation is needed for each
destination: source addresses increase, so if that slot is too far backward,
all older ones are too. Branch edges are sorted by source offset without
reordering the graph's edges.

Fall-through guards target the next original fragment's entry, skipping any
prefix padding and intervening islands. The section-end guard is processed last;
its continuation can be placed after all reservations without a speculative slot.

## Materialization and BTI

After mapping, keep branches that fit directly. Otherwise reuse a reachable
materialized stub by numeric destination, including across output sections, or
fill the reserved slot. ADRP/ADD/BR uses 12 bytes; MOVZ/MOVK/BR uses 20 bytes and
requires a fixed load address. Unused bytes remain NOPs. Materialization never
changes block sizes or placement.

For BTI binaries, a preparation pass adds known emitted destinations and fragment
entries before CFG finalization. Existing BTI c/j/jc and implicit PACIASP/PACIBSP landing
pads are preserved. At materialization, validate the exact indirect destination,
including addends. An existing NOP can be replaced with BTI c without changing
the layout, including in original code left in place. Diagnose destinations
without a compatible landing pad or a patchable NOP. Direct branches need no pad.

## Diagnostics and coverage

Placement reports aggregate batch, island, slot and byte counts per output
section through `BC.outs()`. `--print-stub-stats` additionally reports final
ADRP/MOV/unused/shared counts and supports the existing relaxation modes.
`--verify-branch26-range` checks final Branch26 distances before fixups.

Graph tests in `bolt/unittests/Core/DeferredStubLinker.cpp` cover range limits,
unordered edges, aliases/addends, alignment gaps, bounded entry preferences,
section-end guards and indivisible fragments. The common layout tests in
`JITLinkLinker.cpp` cover copying, empty markers and repeated rebasing. ELF tests
in `bolt/test/AArch64/jitlink-*.s` and `.test` cover emission, ADRP/MOV stubs, PIE,
hot/cold sharing, BTI, runtime isolation and FDEs. Local branch tests cover BB
alignment at range boundaries, near-form selection, CMPBR with live flags,
tail trampolines and conditional long legs entering the shared island pipeline.
