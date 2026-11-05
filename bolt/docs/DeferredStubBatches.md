# Deferred AArch64 stub placement

`--turn-off-stubs` defers global AArch64 branch relaxation to JITLink. It requires
an ELF binary with relocations. Local basic-block relaxation still runs in
`LongJmpPass`; inter-function branches are repaired after final addresses are
known. Runtime objects use the ordinary linker path.

## Components and phase boundaries

| Component | Responsibility |
| --- | --- |
| [`Core/DeferredStubs`](../include/bolt/Core/DeferredStubs.h) | Select the deferred mode, describe emitted fragments, detect fall-through and prepare known BTI destinations. |
| `BinaryEmitter` | Emit private `.bolt.fragment.N` sections, final fragment bytes and fall-through guards. Record logical output sections and entry alignment preferences. |
| [`DeferredStubLinker`](../include/bolt/Rewrite/DeferredStubLinker.h) | Validate and merge fragment sections, reserve shared islands and materialize stubs. Own reservation state for one linked object. |
| `JITLinkLinker` | Register passes and provide the common block-layout and rebasing helpers. |
| `ExecutableFileMemoryManager` | Allocate the placed extent and copy blocks at their assigned offsets. |

The emitter and Core helpers do not depend on the linker implementation.
`DeferredStubLinker::addPasses` registers post-prune placement and pre-fixup
materialization. Its owner, the JITLink context, keeps reservations alive until
linking completes. Section mapping supplies virtual bases between these phases.

MC resolves references within a fragment. Separate input sections preserve
inter-fragment relocations and provide insertion boundaries. Before merging,
the linker requires one block per emitted fragment and validates that relocation
addends stay within that fragment. Metadata may refer one past its end; branches
may not. The LSDA's `LPStart-1` arithmetic base is allowed separately.

Block addresses are emission-order keys before placement, section-relative
offsets before allocation, and virtual addresses after mapping. Unplaced blocks
use `UINT64_MAX` so ordering keys cannot be mistaken for final positions.
`getSectionAllocationSize` is valid only after section-relative placement and
before rebasing. Rebasing uses actual buffer offsets and can be repeated.

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

For BTI binaries, Core prepares known emitted destinations and fragment entries
before local relaxation. Existing BTI c/j/jc and implicit PACIASP/PACIBSP landing
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
hot/cold sharing, BTI, runtime isolation and FDEs.
