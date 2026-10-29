# Late AArch64 stub compaction

`experiment/deferred-stub-compaction` extends the batch implementation behind
`--turn-off-stubs`. Decisions about inter-fragment reach remain in JITLink;
`LongJmpPass::relaxLocalBranches` is shared with `--relax-exp`.

The generic linker only connects the JITLink lifecycle stages.
[`JITLinkAArch64`](../lib/Rewrite/JITLinkAArch64.cpp) owns fragment preparation,
late mapping and fixed-reservation finalization. Its private `StubCompactor`
separates `layout`, `route`, `restoreReservations`, `pack` and `emit`; `run` is
the bounded convergence loop. A slot stores one chain link (`Next`), and its
instruction form is derived from that destination and the selected capacity.
Section bases are planned once per round and reused to check committed mapping.
The preview and final mapper share `layoutCodeSections` in `RewriteInstance`;
they do not maintain separate copies of the code-placement policy.

## What makes relax-exp small

`ClusteredRelaxation` in `lib/Passes/LongJmp.cpp` estimates the final hot/cold
fragment order and groups it into clusters (124 MiB by default). Same-cluster
references remain direct. Cross-cluster references are checked with estimated
thunk growth, then share thunks by destination, including aliases. Calls are
processed farthest first to establish reusable bridges. The default call budget
allows one four-byte B bridge before falling back to ADRP/ADD/BR; ordinary
branches can use multiple B hops. Adjacent cluster boundaries also share ADRP
thunks. Only the instructions selected by that algorithm are emitted.

Thus its small count comes from direct-reference elimination, reuse and B
chains, without twenty-byte speculative slots in the output. Its printed
thunk count and the batch placer's reservation count describe different things.
Both approaches have already run local branch relaxation before this comparison.

## Compaction before address publication

The batch sweep still provides a valid conservative allocation. After allocation,
but before `notifyResolved`, JITLink now:

1. Starts with zero bytes for each reservation and lays out the actual fragment
   bodies, mandatory MC alignment and bounded preferred entry alignment.
2. Obtains section bases through a pure preview of `mapFileSections`, including
   old-text placement, hot/cold ordering, huge-page padding and allocatable data.
   External symbols are resolved against that plan without publishing addresses.
3. Checks original Branch26 edges at those addresses. Direct edges need no slot.
   Remaining edges are visited farthest first and share reachable stubs, including
   across output sections. Canonical destinations preserve aliases and addends.
4. Selects B (4 bytes), ADRP/ADD/BR (12) or MOV/BR (20). B bridges may move to
   fragment boundaries on either side of the source. Sparse boundaries around
   64 MiB and on either side of large bodies support chains. Calls have at most
   one B hop; ordinary branches can have multiple hops. MOV requires a fixed
   load address.
5. Repeats actual linker placement when a selected form grows or unused capacity
   disappears. Every original cross-block Branch26 is checked again, including
   references that required no reservation during the batch sweep.
6. Packs accepted blocks once, materializes stubs and redirects edges. Only then
   does the ordinary mapper commit addresses and JITLink publish symbols and
   apply relocations. The committed section and external addresses must match
   the validated plan.

Body bytes, their internal offsets and their relocation sites remain intact.
Function and fragment symbols move with their blocks, as do unwind and debug
references. Guards still skip islands and function-padding prefixes. Executable
host buffers have bounded spare capacity for moving bridges between sections;
that capacity is not part of their ELF output size.
Selected forms only grow within an attempt, avoiding size oscillation; unused
slots are removed completely. A later placement can therefore retain a larger
selected instruction form even if the shortest form has become reachable.

## Alignment and conservative recovery

Deleting code does not always shorten a displacement. A test starts with a
source at offset 68 and its 64-byte-aligned target at `128 MiB + 64`: the branch
displacement is `128 MiB - 4`. Shrinking an earlier slot from 20 to 12 bytes
moves the source back eight bytes, while the target remains at the same aligned
address. The displacement becomes `128 MiB + 4` and no longer fits.

If routing cannot repair the compact layout, its bounded capacity is exceeded,
or 16 placement rounds do not converge, the implementation restores the original
reserved layout and validates it. `fallback=1` exposes this recovery; it can
retain unused slots and instruction tails. The normal compact path removes both
unused slots and NOP tails. Necessary function/body alignment, explicit input
NOPs and user-requested padding are preserved.

This is linker-owned relaxation over actual emitted blocks and the real section
mapping policy. It does not move global reachability analysis back into an
optimization pass. It does introduce linker placement iterations, bounded at 16,
with no provisional symbol publication or repeated byte copying.

## Diagnostics and validation

The per-section `JITLink batches` and `JITLink layout` lines describe the initial
reservation layout. `JITLink compaction` reports passes, before/after section
bytes, removed bytes, final stub bytes and fallback. With `--print-stub-stats`,
`JITLink stubs` additionally reports original slots, unused original slots,
ADRP/MOV forms, sharing, B forms and the retained count including new chain hops.
`final-bytes` measures allocated stub bytes; under fallback it includes capacity.
No line is printed for each function fragment.

Local comparisons use the same input, profile and other options in both modes:

| Input | relax-exp stub bytes | Deferred stub bytes | relax-exp `.text` + `.text.cold` | Deferred `.text` + `.text.cold` |
| --- | ---: | ---: | ---: | ---: |
| Small profiled split functions | 0 | 0 | 116 | 116 |
| `relax-calls.s` | 12 | 12 | 352,321,608 | 352,321,616 |
| `relax-calls.s`, hot at end | 24 | 20 | 352,321,624 | 352,321,628 |
| `relax-branches-with-thunk-chain.s` | 32 | 24 | 301,990,056 | 301,990,056 |
| Same, hot at end | 24 | 16 | 301,990,048 | 301,990,040 |

All compact layouts converged in two rounds without fallback. The residual
four/eight-byte excesses in the call examples are alignment/body-layout
differences, not unused slots or NOP instruction tails. Independently assembled
fragment bodies must retain the alignment under which MC resolved their internal
padding; compaction does not reassemble those bodies. These measurements do not
establish a universal code-size or execution-time improvement over relax-exp.

Regression tests cover exact ADRP/MOV sizes without NOP tails, B bridges in both
directions, multi-hop chains, sharing and the alignment-sensitive recovery case.
The B-chain test follows the encoded instructions to the intended destinations,
in addition to checking the selected forms and sizes.
`--verify-branch26-range` checks the final graph, including newly materialized B
edges. Large Clang performance and size comparisons remain a separate benchmark;
small synthetic examples cannot establish parity with relax-exp on that workload.

The readability refactor was checked with CoreTests (64 passed, 32
architecture-dependent skips), 20 AArch64 and four X86 lit tests. Thirteen ELF
outputs match the preceding implementation in all allocatable section contents,
addresses, sizes, defined symbols and JITLink statistics. A fourteenth comparison
checks the PIE runtime's instructions and symbols; its inter-block padding was
already uninitialized and varies between runs of the preceding binary, so those
padding bytes are excluded. The main object's code is compared without masking.
