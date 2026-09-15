//===- bolt/Rewrite/JITLinkAArch64.h - AArch64 JITLink support -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef BOLT_REWRITE_JITLINK_AARCH64_H
#define BOLT_REWRITE_JITLINK_AARCH64_H

#include "llvm/Support/Error.h"
#include <memory>

namespace llvm {
namespace jitlink {
class LinkGraph;
}

namespace bolt {
class BinaryContext;

class AArch64JITLinkBranch26RelaxationPlan {
public:
  AArch64JITLinkBranch26RelaxationPlan();
  ~AArch64JITLinkBranch26RelaxationPlan();

private:
  class Impl;
  std::unique_ptr<Impl> P;

  friend Error prepareAArch64JITLinkBranch26Relaxation(
      BinaryContext &, jitlink::LinkGraph &,
      AArch64JITLinkBranch26RelaxationPlan &);
  friend Error
  relaxAArch64JITLinkBranch26(BinaryContext &,
                              AArch64JITLinkBranch26RelaxationPlan &);
};

/// Split emitted code at BOLT function/fragment markers and reserve all
/// storage that Branch26 relaxation may need. This is the last growing pass.
Error prepareAArch64JITLinkBranch26Relaxation(
    BinaryContext &BC, jitlink::LinkGraph &G,
    AArch64JITLinkBranch26RelaxationPlan &Plan);

/// Select and materialize reserved thunks after final addresses are known.
Error relaxAArch64JITLinkBranch26(BinaryContext &BC,
                                  AArch64JITLinkBranch26RelaxationPlan &Plan);

bool shouldVerifyAArch64JITLinkBranch26Range();
Error verifyAArch64JITLinkBranch26Range(BinaryContext &BC,
                                        jitlink::LinkGraph &G);

} // namespace bolt
} // namespace llvm

#endif // BOLT_REWRITE_JITLINK_AARCH64_H
