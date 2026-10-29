//===- JITLinkAArch64.h - Late AArch64 branch relaxation ---------*- C++
//-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef BOLT_REWRITE_JITLINKAARCH64_H
#define BOLT_REWRITE_JITLINKAARCH64_H

#include "bolt/Core/Linker.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ExecutionEngine/JITLink/JITLink.h"

#include <vector>

namespace llvm::bolt {

class BinaryContext;
struct EmittedFragmentSection;

/// Owns the main object's fragment placement and late branch relaxation.
/// Runtime objects use the generic linker layout instead.
class JITLinkAArch64 {
public:
  using FragmentMap =
      DenseMap<const jitlink::Block *, const EmittedFragmentSection *>;
  using SectionBases = DenseMap<const BinarySection *, uint64_t>;
  /// Null Bases means resolve against the committed section addresses.
  using SymbolResolver =
      function_ref<uint64_t(StringRef, const SectionBases *)>;
  struct Branch {
    jitlink::Block *SourceBlock;
    jitlink::Edge::OffsetT SourceOffset;
  };
  struct Stub {
    jitlink::Symbol *SlotSym;
    SmallVector<Branch, 1> Branches;
  };

  JITLinkAArch64(BinaryContext &BC, jitlink::LinkGraph &G) : BC(BC), G(G) {}
  /// Merge emitted fragments and reserve a conservative, reachable layout.
  Error prepare();
  /// Compact before address publication, then commit and check the mapping.
  Error mapSections(const BOLTLinker::SectionsMapper &Map,
                    const BOLTLinker::SectionsMapper &Plan,
                    SymbolResolver Resolve);
  /// Materialize fixed reservations after lookup when no planner is available.
  Error finalize();

  static bool isExecutable(const jitlink::Section &Section) {
    return (Section.getMemProt() & orc::MemProt::Exec) != orc::MemProt::None;
  }
  static Error verifyBranch26Range(jitlink::LinkGraph &G);

  // Graph-level stages also used by placement and compaction unit tests.
  static Error mergeFragments(jitlink::LinkGraph &G,
                              ArrayRef<EmittedFragmentSection> Fragments,
                              FragmentMap &FragmentsByBlock);
  static Expected<std::vector<Stub>> reserveStubs(jitlink::LinkGraph &G,
                                                  const FragmentMap &Fragments,
                                                  raw_ostream *Log = nullptr);
  static Error compactStubs(BinaryContext &BC, jitlink::LinkGraph &G,
                            ArrayRef<Stub> Reservations,
                            const FragmentMap &Fragments,
                            const BOLTLinker::SectionsMapper &Plan,
                            SymbolResolver Resolve);

private:
  BinaryContext &BC;
  jitlink::LinkGraph &G;
  FragmentMap Fragments;
  std::vector<Stub> Stubs;
};

} // namespace llvm::bolt

#endif
