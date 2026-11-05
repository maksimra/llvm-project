//===- DeferredStubs.cpp - MC preparation for deferred stubs --------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "bolt/Core/DeferredStubs.h"
#include "bolt/Core/BinaryContext.h"
#include "bolt/Core/BinaryFunction.h"

namespace llvm {
namespace bolt {

bool fragmentMayFallThrough(const FunctionFragment &Fragment,
                            const BinaryContext &BC) {
  for (const BinaryBasicBlock *BB : llvm::reverse(Fragment)) {
    const MCInst *Last = BB->getLastNonPseudoInstr();
    if (!Last)
      continue;

    // Tail-call annotations may have been removed by InstructionLowering.
    // Inspect the opcode, not BOLT's isCall/isTailCall predicates. isTerminator
    // uses opcode properties and also honors the terminal-trap option.
    const MCInstrDesc &Desc = BC.MII->get(Last->getOpcode());
    return !BC.MIB->isTerminator(*Last) || Desc.isConditionalBranch() ||
           Desc.isCall();
  }
  return false;
}

} // namespace bolt
} // namespace llvm
