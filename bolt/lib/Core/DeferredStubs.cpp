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
#include "bolt/Utils/CommandLineOpts.h"

namespace llvm {
namespace bolt {

bool usesDeferredStubs(const BinaryContext &BC) {
  return opts::DeferredStubs && BC.isELF() && BC.isAArch64();
}

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

/// Deferred stubs can turn any direct branch into BR x16. Prepare their known
/// destinations before local relaxation, while adding instructions is still
/// safe. The linker validates the exact destination (including addends) later.
void prepareDeferredBTITargets(BinaryContext &BC) {
  DenseMap<const MCSymbol *, BinaryBasicBlock *> BlocksBySymbol;
  DenseSet<BinaryBasicBlock *> Targets;
  for (BinaryFunction *BF : BC.getOutputBinaryFunctions()) {
    // Non-simple functions and fixed-size patches must retain their layout.
    if (!BC.shouldEmit(*BF) || !BF->isSimple() || BF->isPatch())
      continue;
    for (FunctionFragment &FF : BF->getLayout().fragments()) {
      if (FF.empty())
        continue;
      // Synthetic fall-through guards may also need a stub to the next
      // fragment, even if no explicit branch refers to its entry.
      Targets.insert(FF.front());
      if (FF.isMainFragment())
        for (const MCSymbol *Symbol : BF->getSymbols())
          BlocksBySymbol[Symbol] = FF.front();
      for (BinaryBasicBlock *BB : FF) {
        BlocksBySymbol[BB->getLabel()] = BB;
        if (const MCSymbol *Symbol = BF->getSecondaryEntryPointSymbol(*BB))
          BlocksBySymbol[Symbol] = BB;
      }
    }
  }

  for (const BinaryFunction *BF : BC.getOutputBinaryFunctions()) {
    if (!BC.shouldEmit(*BF))
      continue;
    for (const BinaryBasicBlock &BB : *BF)
      for (const MCInst &Inst : BB) {
        if ((!BC.MIB->isBranch(Inst) && !BC.MIB->isCall(Inst)) ||
            BC.MIB->isIndirectBranch(Inst) || BC.MIB->isIndirectCall(Inst) ||
            BC.MIB->getTargetAddend(Inst))
          continue;
        if (auto *Target = BlocksBySymbol.lookup(BC.MIB->getTargetSymbol(Inst)))
          Targets.insert(Target);
      }
  }

  for (BinaryBasicBlock *BB : Targets) {
    auto First = BB->getFirstNonPseudo();
    if (First != BB->end() && (BC.MIB->isBTILandingPad(*First, BTIKind::C) ||
                               BC.MIB->isBTILandingPad(*First, BTIKind::J) ||
                               BC.MIB->isBTILandingPad(*First, BTIKind::JC)))
      continue;
    MCInst BTI;
    BC.MIB->createBTI(BTI, BTIKind::C);
    BB->insertInstruction(First, BTI);
  }
}

} // namespace bolt
} // namespace llvm
