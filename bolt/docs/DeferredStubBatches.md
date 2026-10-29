# Deferred AArch64 stub placement

This document describes the conservative batch reservation stage behind
`--turn-off-stubs`. The `experiment/deferred-stub-compaction` branch additionally
[compacts these reservations before publishing addresses](DeferredStubCompaction.md).

## Start here

The placement policy is in `AArch64StubPlacer::run` in
[`JITLinkAArch64.cpp`](../lib/Rewrite/JITLinkAArch64.cpp). It processes one output
section with one forward sweep; B, C and D are local indices into its original
fragment blocks:

```text
B [current batch] C [placed look-ahead] [new island] D [unplaced fragments]
```

1. Advance D, placing whole fragments while leaving a 16 MiB island allowance
   within Branch26 range (128 MiB) of B. At least one fragment is accepted.
2. Select C: approximately 32 MiB of code, also bounded by the number of branches
   not proven direct in the already placed prefix, at 20 bytes per candidate.
   This is an upper bound: sharing can further reduce the slots actually needed.
   A large fragment remains indivisible.
3. `reserveBatch` visits B..C in source-address order. A proven direct branch
   needs nothing; otherwise reuse a reachable slot or append one to this batch's
   island at D. Advance B to C.

An island is a JITLink block from its first slot onward. It grows without a
content buffer while slots are added; each reservation immediately gets a symbol
in that block. At the end of the batch, allocate exactly its final size and fill
it with NOPs. No slots means no island. There are no separate reservation offsets,
second symbol-creation pass, batch objects or saved layout records.

Fall-through guards are wired separately to the next original fragment's entry,
skipping any `--pad-funcs-before` prefix. The last continuation is placed after
all tail islands. They do not change batching.

Excluding proven-direct branches from the capacity estimate avoids needless
batch splits, but does not guarantee fewer reservations: a larger batch can
process later sources before the next iteration advances D to their targets.
The 32 MiB code limit and the 16 MiB look-ahead island allowance still apply.

## Sharing and mold's A window

The policy follows [mold's forward sweep](https://github.com/rui314/mold/blob/331ac663668dc0b1f4949403854613b9b1913025/elf/thunks.rs).
Mold uses A to expire symbol marks for old, unreachable thunk groups. Here,
`SharedStubs` maps a canonical destination to its latest slot and checks exact
reachability for each branch. Source addresses increase: a reachable slot cannot
become too far forward, and once the latest slot is too far backward, all older
slots are too. Thus no A index or expiration queue is needed. JITLink edges need
not be ordered, so `reserveBatch` sorts Branch26 pointers without reordering the
graph's edges.

Without the compaction extension, `JITLinkAArch64::finalize` keeps direct branches
or reuses a materialized stub by numeric target address, including across output sections.
Otherwise it fills the reserved slot with ADRP/ADD/BR or MOV/BR. Unused slots
remain NOPs. This phase changes neither block sizes nor placement.

## Why this does not duplicate LongJmpPass layout

There is one owner of each decision:

| Stage | Responsibility |
| --- | --- |
| MC emission | Final fragment body bytes, internal references and alignments; record the bounded entry alignment separately. |
| `JITLinkAArch64::mergeFragments` | Validate relocations, group blocks by logical section, preserve emission order and associate blocks with fragment metadata. No byte placement. |
| Post-prune placement | Apply mandatory block alignment, then bounded entry alignment; assign conservative section-relative addresses and insert islands. |
| Memory manager | Allocate the resulting extent and copy at those offsets. No packing decisions. |
| Late compaction (experimental) | Preview actual section bases, select final stub sizes, remove unused slots and validate all cross-block Branch26 edges before packing. |
| Section mapping | Commit virtual bases; rebase from actual block-buffer positions. |
| `JITLinkAArch64::finalize` (without compaction) | Check final distances and fill or reuse slots. No growth or relayout. |

The 16 MiB allowance is a bound for selecting D, not a prediction of the final
island size. Every actual slot is checked for reach and capacity. Unplaceable
indivisible fragments are diagnosed before allocation.

Function entry alignment is not a mandatory block alignment. Private fragment
sections retain their minimum instruction alignment and any stronger alignment
required by their already assembled bodies. The logical output section retains
the section-base alignment. Placement applies the preferred function alignment
only if its padding fits the main/cold fragment's `MaxAlignmentBytes`; zero
disables the preference. See [the alignment analysis](DeferredStubAlignment.md)
for the original code-size defect, measurements and remaining reservation costs.

Block addresses are emission-order keys before placement, section-relative
offsets before allocation, and virtual addresses after mapping. Unplaced blocks
use `UINT64_MAX`, so direct-branch checks cannot mistake ordering keys for final
offsets. Section order and virtual bases are chosen by `RewriteInstance`.

`getSectionAllocationSize` measures the buffer required by the placed blocks.
It is valid after section-relative placement and before virtual-address rebasing;
it is not a general query for section contents at any graph stage.

The batch sweep uses no estimated function layout or cluster graph. The
compaction extension can insert branch chains and iterate over the actual linker
layout before addresses are committed. Existing local basic-block relaxation in
`LongJmpPass` still runs inside each fragment.

## Existing per-function sections

BOLT already emits `.local.text.<function>` and `.local.cold.text.<function>`
in non-relocation mode (`--relocs=0`). `AssignSections` assigns shared output
section names in relocation mode. The same distinction exists in archived
[standalone BOLT](https://github.com/facebookarchive/BOLT/blob/01f471e7108f04b66fd7d3689f3cae3ec8ff6449/src/Passes/BinaryPasses.cpp#L1143-L1176),
[v1.0.1](https://github.com/facebookarchive/BOLT/blob/81e6ecaeea7ffd80d88f2b4017e2384e9e753fca/src/Passes/BinaryPasses.cpp#L1143-L1176)
and [LLVM 14](https://github.com/llvm/llvm-project/blob/llvmorg-14.0.0/bolt/lib/Passes/BinaryPasses.cpp#L1217-L1252).
No independent per-function-section emission option was found in these sources;
`--generate-link-sections` only writes a linker-script section list.

Deferred emission uses the existing emitter and MC streamer, adding private
`.bolt.fragment.N` names and their logical output-section mapping. This preserves
inter-fragment relocations until JITLink and provides indivisible insertion
boundaries. The ELF builder's one-block-per-fragment invariant is checked before
merging. This emission policy already exists in the base commit.

Switching to `--relocs=0` would also select in-place rewriting, keep main functions
at their original addresses and limit their size to the original footprint.
It cannot provide this emission policy in the relocation/reordering pipeline.

## Batch diagnostics

Deferred linking reports aggregate statistics per output section through
`BC.outs()`. The `BOLT-INFO: JITLink batches:` line gives the number of batches,
fragments, stub islands and reserved slots, followed by the section's layout
byte counts. Individual fragment names, batch ranges, look-ahead boundaries and
island offsets are not printed. Batches reusing earlier islands still contribute
to the batch count, but do not increase the island or slot counts.

These are placement/reservation statistics, before virtual address mapping and
materialization. Final ADRP/MOV/unused/shared counts remain controlled by
`--print-stub-stats`. No additional command-line option is required for batch
diagnostics. The optional `raw_ostream *Log` parameter lets graph unit tests
exercise placement without producing informational output.

Each section also reports a `BOLT-INFO: JITLink layout:` summary:
`total-bytes = emitted-bytes + stub-bytes + alignment-bytes`.
`emitted-bytes` includes internal MC padding, requested function padding and
guards; `guard-bytes` is the guard subset of that count. `alignment-bytes` counts
only gaps between blocks, including island alignment. These counts describe the
section extent, excluding gaps between different output sections or file-level
padding. Together with final stub statistics they separate alignment overhead
from unused reservations.

## Validation

The graph tests cover both Branch26 limits, unordered edges, aliases/addends,
alignment gaps, empty markers, the last guard, indivisible fragments, actual
content copying and repeated rebasing. Focused ELF tests cover ADRP/MOV, PIE,
hot/cold sharing, runtime isolation and FDEs.

The synthetic 160 MiB three-caller case uses one reserved/materialized stub,
compared with two before batching; a graph test covers fifteen callers over
224 MiB sharing one island. These are regressions, not production benchmarks.

The batch-diagnostics validation logs and build helpers are in
`/tmp/bolt-batch-info`; the preceding placement-refactor checks are in
`/tmp/bolt-compact-placement`.
CoreTests: 61 passed, 31 architecture-dependent skips. All 13 focused lit tests
pass. A comparison of 13 outputs with the preceding implementation also matches
executable section layout and function addresses, sizes and bytes.
The allocation-size helper rename was checked with CoreTests and two focused
lit tests; those logs are in `/tmp/bolt-allocation-size`.
Archived sources and saved `--keep-tmp` section-emission examples remain in
`/tmp/bolt-a-and-sections-review`. Changed translation units use the configured
Release/assertions compiler and linker commands with existing LLVM archives;
a full LLVM rebuild is not run.
