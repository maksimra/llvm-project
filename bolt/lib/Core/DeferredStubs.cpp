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
#include "llvm/MC/MCCodeEmitter.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCSectionELF.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/Support/Endian.h"

namespace llvm {
namespace bolt {

bool usesDeferredStubs(const BinaryContext &BC) {
  return opts::DeferredStubs && BC.isELF() && BC.isAArch64();
}

namespace {

MCInst withZeroBranchDisplacement(const MCInst &Branch) {
  MCInst Placeholder = Branch;
  // AArch64's branch displacement is the last non-annotation operand.
  Placeholder.getOperand(MCPlus::getNumPrimeOperands(Placeholder) - 1) =
      MCOperand::createImm(0);
  return Placeholder;
}

uint32_t encodeBranchTemplate(BinaryContext &BC, const MCInst &Placeholder) {
  SmallVector<char, sizeof(uint32_t)> Bytes;
  SmallVector<MCFixup, 1> Fixups;
  BC.MCE->encodeInstruction(Placeholder, Bytes, Fixups, *BC.STI);
  assert(Bytes.size() == sizeof(uint32_t) && Fixups.empty() &&
         "Expected one AArch64 instruction without fixups");
  return support::endian::read32le(Bytes.data());
}

unsigned emitConditionalExpansion(MCStreamer &Streamer, BinaryContext &BC,
                                  const MCInst &Branch, MCSymbol *LongBranch) {
  MCContext &Context = Streamer.getContext();
  MCSymbol *Continuation = Context.createTempSymbol();
  unsigned EmittedSize = 0;
  auto Emit = [&](const MCInst &Inst) {
    Streamer.emitInstruction(Inst, *BC.STI);
    EmittedSize += BC.MIB->getInstructionSize(Inst).value();
  };
  auto EmitJump = [&](MCSymbol *Target) {
    MCInst Jump;
    BC.MIB->createUncondBranch(Jump, Target, &Context);
    Emit(Jump);
  };

  if (BC.MIB->isReversibleBranch(Branch, /*MustPreserveFlags=*/true)) {
    // The false path skips the long branch; the true path executes it.
    const auto Inverted = BC.MIB->reverseBranchCondition(
        Branch, Continuation, &Context, /*MustPreserveFlags=*/true);
    for (const MCInst &Inst : Inverted)
      Emit(Inst);
  } else {
    // Keep the original condition and flags when inversion is unavailable.
    MCInst TakenBranch = Branch;
    BC.MIB->replaceBranchTarget(TakenBranch, LongBranch, &Context);
    Emit(TakenBranch);
    EmitJump(Continuation);
  }
  Streamer.emitLabel(LongBranch);
  EmitJump(LongBranch); // Placeholder; JITLink supplies the real destination.
  Streamer.emitLabel(Continuation);
  return EmittedSize;
}

void emitBranchRecord(MCStreamer &Streamer, BinaryContext &BC,
                      const MCInst &Branch, MCSymbol *Source,
                      MCSymbol *LongBranch,
                      const DeferredBranchRecord &Record) {
  MCContext &Context = Streamer.getContext();
  const MCSymbol *Target = BC.MIB->getTargetSymbol(Branch);
  const int64_t TargetAddend = BC.MIB->getTargetAddend(Branch);
  assert(Target && "Expected a symbolic direct branch");

  Streamer.pushSection();
  Streamer.switchSection(
      Context.getELFSection(DeferredBranchSectionName, ELF::SHT_PROGBITS, 0));
  Streamer.emitSymbolValue(Source, sizeof(Record.Source));
  const MCExpr *Destination = MCSymbolRefExpr::create(Target, Context);
  if (TargetAddend)
    Destination = MCBinaryExpr::createAdd(
        Destination, MCConstantExpr::create(TargetAddend, Context), Context);
  Streamer.emitValue(Destination, sizeof(Record.Target));
  if (LongBranch)
    Streamer.emitSymbolValue(LongBranch, sizeof(Record.LongBranch));
  else
    Streamer.emitIntValue(0, sizeof(Record.LongBranch));
  Streamer.emitIntValue(Record.Encoding, sizeof(Record.Encoding));
  Streamer.emitIntValue(Record.DisplacementBits,
                        sizeof(Record.DisplacementBits));
  Streamer.emitIntValue(Record.ReservedSize, sizeof(Record.ReservedSize));
  Streamer.emitIntValue(0, sizeof(Record.Padding));
  Streamer.popSection();
}

} // namespace

MCSymbol *emitDeferredBranch(MCStreamer &Streamer, BinaryContext &BC,
                             const MCInst &Branch, BranchExpansion Expansion,
                             bool EmitRecord) {
  MCContext &Context = Streamer.getContext();
  const MCInst Placeholder = withZeroBranchDisplacement(Branch);
  DeferredBranchRecord Record{};
  Record.DisplacementBits = BC.MIB->getPCRelEncodingSize(Branch);
  Record.ReservedSize = BC.MIB->getInstructionSize(Branch).value();
  MCSymbol *Source = Context.createTempSymbol();
  MCSymbol *LongBranch = nullptr;
  MCSymbol *TailTrampoline = nullptr;

  Streamer.emitLabel(Source);
  if (!BC.MIB->isConditionalBranch(Branch)) {
    // B/BL already have the long encoding; no extra instruction is needed.
    LongBranch = Source;
    Streamer.emitInstruction(Placeholder, *BC.STI);
  } else if (Expansion == BranchExpansion::Inline) {
    LongBranch = Context.createTempSymbol();
    Record.ReservedSize =
        emitConditionalExpansion(Streamer, BC, Branch, LongBranch);
  } else {
    Streamer.emitInstruction(Placeholder, *BC.STI);
    if (Expansion == BranchExpansion::Tail)
      TailTrampoline = LongBranch = Context.createTempSymbol();
  }

  if (EmitRecord) {
    // An immediate zero produces no symbolic fixup. This keeps MC from
    // resolving the real destination, even for CMPBR, which has no ELF branch
    // relocation. JITLink restores the displacement from the separate record.
    Record.Encoding = encodeBranchTemplate(BC, Placeholder);
    emitBranchRecord(Streamer, BC, Branch, Source, LongBranch, Record);
  }
  return TailTrampoline;
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

} // namespace bolt
} // namespace llvm
