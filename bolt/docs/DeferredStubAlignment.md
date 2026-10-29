# Deferred fragment alignment and code size

This analysis concerns the batch implementation at `4617f8120332`, classic
`LongJmpPass`, and `--relax-exp`. Large-input figures below are supplied benchmark
results, not a rerun of that workload. The small-input results were measured
locally before and after the alignment change.

This report describes the alignment fix and the remaining costs of the original
batch allocator. The subsequent [compaction experiment](DeferredStubCompaction.md)
removes unused reservations and instruction tails before committing addresses.

## The original alignment defect

`BinaryEmitter::emitFunction` traditionally emits multiple functions into one
MC section. It first aligns to `Function.getMinAlign()`, then optionally to
`Function.getAlign()` with a main/cold byte budget. A zero budget skips the
second directive. `ensureMinAlignment(BC.AlignFunctions)` constrains the **section
base**, not every function in that section.

Deferred emission reuses this code with one private `.bolt.fragment.N` section
per fragment. This changes the effect of both operations:

1. `ensureMinAlignment(BC.AlignFunctions)` gives every fragment a mandatory
   alignment of at least 64 bytes with the default options.
2. An entry `emitCodeAlignment` at offset zero needs no actual padding, but
   `MCObjectStreamer::emitValueToAlignment` still raises ELF `sh_addralign` to
   the requested alignment, regardless of the byte budget.
3. `ELFLinkGraphBuilder` transfers `sh_addralign` to `jitlink::Block`.
4. `alignToBlock` correctly enforces this now incorrect mandatory requirement
   at every fragment boundary. JITLink blocks have no maximum-padding budget.

Removing only `ensureMinAlignment` therefore does not fix profiled functions
that still emit the preferred entry directive. Using `Function.getAlign()` as
the block alignment also loses the budget. Using only `getMinAlign()` can
violate the alignment of constant islands and basic blocks already assembled
inside the fragment.

The compact aligner makes this especially costly: on AArch64, functions without
a profile normally request only their minimum alignment. Private sections
previously overrode that choice with an unconditional 64-byte requirement.
Cold split fragments also lost their usually smaller padding budgets.

## Measured reproductions

The first input contains `_start` (`bl f0; ret`) followed by 10,000 four-byte
`ret` functions. All modes use `--lite=0 --print-stub-stats
--verify-branch26-range`, with no profile.

| Mode | Output `.text`, bytes | Reserved slots |
| --- | ---: | ---: |
| Classic | 40,008 | 0 |
| Relax | 40,008 | 0 |
| Batch before | 640,004 | 0 |
| Batch after | 40,008 | 0 |

The entire 599,996-byte excess in this input is inter-fragment padding. No stub
reservation, materialization, or fall-through guard is involved. The fixed
batch `.text` is also byte-for-byte identical to classic and relax, including
their pre-change outputs.

A second input has two profiled functions, each with 32 hot bytes and eight cold
bytes before local branch relaxation. Both functions are split.

| Mode | `.text` | `.text.cold` | Reserved slots / unused |
| --- | ---: | ---: | ---: |
| Classic | 96 | 16 | 0 / 0 |
| Batch before | 140 | 72 | 2 / 2 |
| Batch after | 140 | 16 | 2 / 2 |

The 56-byte cold gap disappears. The remaining 44-byte difference from classic
is 40 reserved bytes plus eight extra bytes of local conditional-branch
relaxation, minus four bytes of hot alignment padding. This is a concrete
example of growth beyond reservations with a second cause besides alignment.

These inputs and logs are in `/tmp/bolt-fragment-alignment`. The small
hot/cold case is also covered by `test/AArch64/jitlink-function-alignment.s`.

## Interpreting the supplied large-input numbers

| Quantity | Bytes |
| --- | ---: |
| Classic `.text + .text.cold` | 142,713,728 |
| Batch `.text + .text.cold` | 183,937,724 |
| Increase | 41,223,996 |
| Increase in `.text` | 6,982,592 |
| Increase in `.text.cold` | 34,241,404 |
| All 273,344 reserved slots, at 20 bytes | 5,466,880 |
| 244,323 unused slots | 4,886,460 |
| Trailing eight NOP bytes in 29,021 ADRP slots | 232,168 |
| Classic stub instructions: 107,185 B + 2,387 ADRP/ADD/BR | 457,384 |

About 89.4% of batch slots are unused. The reported `slots` count is not the
number of materialized stubs: only 29,021 slots contain ADRP stubs in this run.
Likewise, classic's mostly four-byte B stubs and deferred's fixed 20-byte
reservations are different units for a code-size comparison.

About 83.1% of the measured section-size increase is in `.text.cold`, consistent
with losing compact cold-code alignment. This locates the growth but does not
by itself separate cold padding from cold stub reservations or body changes.

Subtracting batch reservations from the increase leaves 35,757,116 bytes. If
classic's stub instruction bytes are also removed from the baseline, the
remaining difference is 36,214,500 bytes. This remainder contains differences
in inter-fragment alignment, emitted bodies, guards and internal MC padding;
it is not a measurement of alignment alone. The defect above can account for
tens of megabytes at a million-fragment scale, but attributing the full
remainder requires the original input/profile and new layout statistics.

The final ELF file also includes metadata and file/segment padding. Its size
change is not an additional measurement of instruction growth. Section gaps
are not part of the sum of individual `.text` and `.text.cold` sizes.

## Implemented placement contract

Emission records `PreferredAlignment` and the selected main/cold
`MaxAlignmentBytes` in `EmittedFragmentSection`. It keeps section-base alignment
on the logical output section. The private section emits minimum entry
alignment and all original body alignment directives, but omits the preferred
entry directive.

The ELF block alignment thus preserves the assumptions under which MC resolved
the fragment body. During actual post-prune placement, `placeBlock`:

1. Calls `alignToBlock` for mandatory body alignment.
2. Computes padding to `PreferredAlignment` from that position.
3. Adds it only when it fits `MaxAlignmentBytes`.
4. Assigns the block's actual section-relative address.

This happens inside the existing placement sweep, including its look-ahead.
Accepted fragments do not move again. Allocation and mapping keep using those
placements. Runtime objects and the classic/relax emission paths keep their
existing alignment policy.

The function-padding prefix is recorded as `EntryOffset`: fall-through guards
must target the next fragment's entry after this prefix, rather than executing
its zero-filled `--pad-funcs-before` bytes. A far-guard regression now tests this
using real padding instead of relying on the old unconditional alignment bug.

Independent fragments can still need stronger base alignment than a function
in a single MC section. MC has already resolved internal alignment padding;
constant islands, for example, must remain aligned after relocation. Matching
every byte of classic's internal padding would require keeping more MC layout
decisions unresolved or changing the indivisible-fragment representation.

## Remaining reservation overhead

`isProvenDirect` proves reachability only for defined targets already placed in
the same output section. It conservatively reserves slots for other sections,
unplaced forward targets and unresolved symbols. Reservation sharing is also
section-local. `repairEdges` checks final addresses and shares materialized
stubs across sections, but deliberately cannot shrink the allocation. Unused
slots remain NOPs, and ADRP slots retain eight bytes of capacity for a MOV stub.

Fall-through guards add four emitted bytes where required; local conditional
branch relaxation can add B trampolines before JITLink. Both belong to emitted
fragment bytes, not to the 20-byte island-slot counter. Extra alignment can
also push more branches out of range and cause additional reservations.

Reducing these costs is a separate linker-placement change. A general solution
needs output-section bases and symbol resolution available to the layout owner,
so it can decide cross-section reachability before committing island capacity.
Simply deleting unused slots in `repairEdges` would invalidate established
addresses and fixups. A safe follow-up is to unify section-base assignment and
island placement in the linking phase; any convergence or compaction must occur
before publishing symbol addresses and applying fixups. It need not return to
optimization-pass estimates.

The new per-section diagnostic makes the current accounting explicit:

```text
total-bytes = emitted-bytes + stub-bytes + alignment-bytes
```

`guard-bytes` is a subset of `emitted-bytes`. Internal MC alignment and explicit
function padding also remain in `emitted-bytes`; `alignment-bytes` counts gaps
between JITLink blocks. Final `--print-stub-stats` supplies unused/materialized
counts. These are the counters to collect when rerunning the large benchmark.

## Validation

The configured Release/assertions compile/link commands were used to rebuild
the changed translation units, `BinaryContext` (fragment-metadata destruction),
the memory manager, and CoreTests against the existing LLVM/BOLT archives.
The executable is `/tmp/bolt-fragment-alignment/llvm-bolt`; the main build tree's
older executable was not replaced. The full LLVM tree was not rebuilt.

CoreTests: 62 passed, 31 architecture-dependent skips. Sixteen focused lit tests
cover entry budgets (including zero and the exact limit), cold budgets,
constant islands, runtime block alignment, ADRP/MOV stubs, sharing, Branch26
boundaries, fall-through guards, FDEs, and the existing classic/relax cases.
The large Clang runtime, RSS and performance-counter benchmark was not rerun.
