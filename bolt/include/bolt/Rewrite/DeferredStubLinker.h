//===- DeferredStubLinker.h - Deferred AArch64 branch stubs -----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef BOLT_REWRITE_DEFERRED_STUB_LINKER_H
#define BOLT_REWRITE_DEFERRED_STUB_LINKER_H

#include "bolt/Core/DeferredStubs.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ExecutionEngine/JITLink/JITLink.h"
#include <vector>

namespace llvm {
class raw_ostream;
namespace bolt {
class BinaryContext;

/// Owns deferred AArch64 stub placement and final branch repair for one object.
/// The context must keep this alive until all registered passes have run.
class DeferredStubLinker {
public:
  /// Fragment metadata must outlive placement.
  using FragmentMap =
      DenseMap<const jitlink::Block *, const EmittedFragmentSection *>;

  struct Branch {
    jitlink::Block *SourceBlock;
    jitlink::Edge::OffsetT SourceOffset;
  };
  struct Stub {
    jitlink::Symbol *SlotSym;
    SmallVector<Branch, 1> Branches;
  };

  explicit DeferredStubLinker(BinaryContext &BC) : BC(BC) {}

  /// Register placement after pruning and materialization before fixups.
  /// Only call for the main AArch64 ELF object emitted with fragment sections.
  void addPasses(jitlink::PassConfiguration &Config);

  /// Merge private sections into logical output sections in emission order.
  static Error mergeFragmentSections(jitlink::LinkGraph &G,
                                     ArrayRef<EmittedFragmentSection> Fragments,
                                     FragmentMap &FragmentsByBlock);

  /// Reserve shared stub islands ahead of batches of indivisible fragments.
  /// Store section-relative placements in blocks, preserving mandatory and
  /// bounded entry alignments. Optionally log aggregate layout statistics.
  static Expected<std::vector<Stub>>
  reserveStubs(jitlink::LinkGraph &G, const FragmentMap &FragmentsByBlock,
               raw_ostream *Log = nullptr);

private:
  BinaryContext &BC;
  std::vector<Stub> Stubs;

  /// Repair final branches without changing block sizes or placement.
  Error materialize(jitlink::LinkGraph &G);
};

} // namespace bolt
} // namespace llvm

#endif // BOLT_REWRITE_DEFERRED_STUB_LINKER_H
