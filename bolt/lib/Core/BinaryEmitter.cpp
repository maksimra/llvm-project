//===- bolt/Core/BinaryEmitter.cpp - Emit code and data -------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements the collection of functions and classes used for
// emission of code and data into object/binary file.
//
//===----------------------------------------------------------------------===//

#include "bolt/Core/BinaryEmitter.h"
#include "bolt/Core/AddressMap.h"
#include "bolt/Core/BinaryContext.h"
#include "bolt/Core/BinaryFunction.h"
#include "bolt/Core/DebugData.h"
#include "bolt/Core/FunctionLayout.h"
#include "bolt/Core/ParallelUtilities.h"
#include "bolt/Utils/CommandLineOpts.h"
#include "bolt/Utils/Utils.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/DebugInfo/DWARF/DWARFCompileUnit.h"
#include "llvm/MC/MCSection.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/LEB128.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/SMLoc.h"

#define DEBUG_TYPE "bolt"

using namespace llvm;
using namespace bolt;

namespace opts {

extern cl::opt<JumpTableSupportLevel> JumpTables;

static cl::list<std::string>
BreakFunctionNames("break-funcs",
  cl::CommaSeparated,
  cl::desc("list of functions to core dump on (debugging)"),
  cl::value_desc("func1,func2,func3,..."),
  cl::Hidden,
  cl::cat(BoltCategory));

static cl::list<std::string>
    FunctionPadSpec("pad-funcs", cl::CommaSeparated,
                    cl::desc("list of functions to pad with amount of bytes"),
                    cl::value_desc("func1:pad1,func2:pad2,func3:pad3,..."),
                    cl::Hidden, cl::cat(BoltCategory));

static cl::list<std::string> FunctionPadBeforeSpec(
    "pad-funcs-before", cl::CommaSeparated,
    cl::desc("list of functions to pad with amount of bytes"),
    cl::value_desc("func1:pad1,func2:pad2,func3:pad3,..."), cl::Hidden,
    cl::cat(BoltCategory));

static cl::opt<bool> MarkFuncs(
    "mark-funcs",
    cl::desc("mark function boundaries with break instruction to make "
             "sure we accidentally don't cross them"),
    cl::ReallyHidden, cl::cat(BoltCategory));

static cl::opt<bool> PrintJumpTables("print-jump-tables",
                                     cl::desc("print jump tables"), cl::Hidden,
                                     cl::cat(BoltCategory));

size_t padFunction(std::map<std::string, size_t> &FunctionPadding,
                   const cl::list<std::string> &Spec,
                   const BinaryFunction &Function) {
  if (FunctionPadding.empty() && !Spec.empty()) {
    for (const std::string &Spec : Spec) {
      size_t N = Spec.find(':');
      if (N == std::string::npos)
        continue;
      std::string Name = Spec.substr(0, N);
      size_t Padding = std::stoull(Spec.substr(N + 1));
      FunctionPadding[Name] = Padding;
    }
  }

  for (auto &FPI : FunctionPadding) {
    std::string Name = FPI.first;
    size_t Padding = FPI.second;
    if (Function.hasNameRegex(Name))
      return Padding;
  }

  return 0;
}

size_t padFunctionBefore(const BinaryFunction &Function) {
  static std::map<std::string, size_t> CacheFunctionPadding;
  return padFunction(CacheFunctionPadding, FunctionPadBeforeSpec, Function);
}
size_t padFunctionAfter(const BinaryFunction &Function) {
  static std::map<std::string, size_t> CacheFunctionPadding;
  return padFunction(CacheFunctionPadding, FunctionPadSpec, Function);
}

} // namespace opts

namespace {
using JumpTable = bolt::JumpTable;

bool useFragmentSections(const BinaryContext &BC) {
  return opts::DoNotUseStubs && BC.isELF() && BC.isAArch64();
}

/// Emit one selected instruction.
uint64_t emitInstruction(BinaryContext &BC, const MCInst &Inst, MCStreamer *S,
                         MCSymbol *Target = nullptr) {
  // We either emit or compute layout
  // (in this case we should know instruction size)
  assert((S || BC.MII->get(Inst.getOpcode()).getSize()) &&
         "local layout requires fixed-size instructions");
  if (S && Target) {
    MCInst Branch = Inst;
    BC.MIB->replaceBranchTarget(Branch, Target, BC.Ctx.get());
    return emitInstruction(BC, Branch, S);
  }
  // Sized x86 NOPs must bypass the asm layer, which cannot encode them.
  if (BC.isX86() && BC.MIB->isNoop(Inst)) {
    if (std::optional<uint32_t> Size = BC.MIB->getSize(Inst)) {
      if (S) {
        SmallString<15> Code;
        raw_svector_ostream OS(Code);
        BC.MAB->writeNopData(OS, *Size, BC.STI.get());
        S->emitBytes(Code);
      }
      return *Size;
    }
  }

  if (S)
    S->emitInstruction(Inst, *BC.STI);
  return BC.MII->get(Inst.getOpcode()).getSize();
}

/// Emit and size the optional trap before the first block.
uint64_t emitFunctionEntryTrap(const BinaryFunction &BF, MCStreamer *S) {
  for (const std::string &Name : opts::BreakFunctionNames) {
    if (BF.hasNameRegex(Name)) {
      constexpr unsigned Size = 2;
      if (S)
        S->emitIntValue(0x0B0F, Size); // UD2: 0F 0B
      return Size;
    }
  }
  return 0;
}

/// Record the section alignment requirement even when Padding is zero.
void emitBlockPadding(MCStreamer &Streamer, BinaryContext &BC,
                      const BinaryBasicBlock &BB, uint64_t Padding) {
  Streamer.getCurrentSectionOnly()->ensureMinAlignment(BB.getAlign());
  if (!Padding)
    return;
  SmallString<32> Nops;
  raw_svector_ostream OS(Nops);
  if (!BC.MAB->writeNopData(OS, Padding, BC.STI.get()))
    BC.Ctx->reportError(SMLoc(), "cannot emit basic block padding");
  Streamer.emitBytes(Nops);
}

uint64_t getBlockPadding(const BinaryBasicBlock &BB, uint64_t Offset) {
  const uint64_t Padding = offsetToAlignment(Offset, BB.getAlign());
  const uint32_t Budget = BB.getAlignmentMaxBytes();
  return Budget && Padding > Budget ? 0 : Padding;
}

/// Share layout between local branch relaxation and code emission.
/// With relaxation, return the body end offset before tail trampolines.
template <typename RelaxationT, typename BlockCallback,
          typename InstructionCallback>
uint64_t layoutFunctionFragment(const FunctionFragment &FF, BinaryContext &BC,
                                const RelaxationT *Relaxation,
                                MCStreamer *Streamer, bool EmitCodeOnly,
                                BlockCallback &&BeginBlock,
                                InstructionCallback &&BeginInstruction) {
  assert((Streamer || Relaxation) && "layout requires local relaxation");
  uint64_t Offset = Relaxation ? Relaxation->getBodyOffset() : 0;
  const bool AlignBlocks = BC.AlignBlocks || BC.PreserveBlocksAlignment;
  const bool TrackDebugSize =
      Relaxation && Streamer && !EmitCodeOnly && opts::UpdateDebugSections;
  for (auto It = FF.begin(), End = FF.end(); It != End; ++It) {
    BinaryBasicBlock *BB = *It;
    if (AlignBlocks && BB->getAlignment() > 1) {
      if (Relaxation) {
        const uint64_t Padding = getBlockPadding(*BB, Offset);
        Offset += Padding;
        if (Streamer)
          emitBlockPadding(*Streamer, BC, *BB, Padding);
      } else {
        Streamer->emitCodeAlignment(BB->getAlign(), *BC.STI,
                                    BB->getAlignmentMaxBytes());
      }
    }
    const uint64_t BlockStart = Offset;
    BeginBlock(*BB, Offset);
    for (const MCInst &Inst : *BB) {
      BeginInstruction(*BB, Inst, Offset);
      if (BC.MIB->isCFI(Inst) || (EmitCodeOnly && BC.MIB->isPseudo(Inst)))
        continue;
      if (Relaxation)
        Offset += Relaxation->emit(Inst, Streamer);
      else
        emitInstruction(BC, Inst, Streamer);
    }
    if (!TrackDebugSize ||
        BB->getInputOffset() == BinaryBasicBlock::INVALID_OFFSET)
      continue;

    uint64_t DebugEnd = Offset;
    // A B added for a displaced fall-through has no input instruction.
    const MCInst *Last = BB->getLastNonPseudoInstr();
    if (Last && BB->succ_size() &&
        BC.MII->get(Last->getOpcode()).isUnconditionalBranch() &&
        !BC.MIB->getOffset(*Last))
      if (const MCSymbol *Target = BC.MIB->getTargetSymbol(*Last))
        if (BB->getSuccessor(Target))
          DebugEnd -= BC.MII->get(Last->getOpcode()).getSize();

    const auto Next = std::next(It);
    if (DebugEnd != Offset || Next == End ||
        (AlignBlocks && getBlockPadding(**Next, Offset)))
      BB->setDebugSize(DebugEnd - BlockStart);
  }
  if (Relaxation && Streamer)
    Relaxation->emitTrampolines(*Streamer);
  return Offset;
}

bool fragmentMayFallThrough(const FunctionFragment &Fragment,
                            const BinaryContext &BC) {
  for (const BinaryBasicBlock *BB : llvm::reverse(Fragment)) {
    const MCInst *Last = BB->getLastNonPseudoInstr();
    if (!Last)
      continue;

    // Tail-call annotations may have been removed by InstructionLowering.
    // Inspect the opcode, not BOLT's isCall/isTailCall predicates.
    const MCInstrDesc &Desc = BC.MII->get(Last->getOpcode());
    return !BC.MIB->isTerminator(*Last) || Desc.isConditionalBranch() ||
           Desc.isCall();
  }
  return false;
}

/// Ensure conditional branches can reach their targets in the final fragment
/// layout by selecting alternative instruction sequences before emission.
class LocalBranchRelaxation {
  BinaryContext &BC;
  const BinaryFunction &BF;
  const FunctionFragment &FF;
  const MCInst &Jump;

  using Target = std::pair<const MCSymbol *, int64_t>;
  // Same target and same preceding CFI directive: no CFI change between sites.
  using TrampolineKey = std::pair<Target, const MCInst *>;

  enum Form { Original, Inline, Redirect, Swap };
  struct BranchForm {
    Form Kind = Original;
    // Redirect: source of the trampoline B. Swap: the other branch in the pair.
    const MCInst *Other = nullptr;
  };
  DenseMap<const MCInst *, BranchForm> Forms;
  struct TrampolineLabel {
    MCSymbol *Symbol = nullptr;
    bool AtEnd = false;
  };
  MapVector<const MCInst *, TrampolineLabel> Labels;
  // In emission order (reverse source order), matching the CFI state stack.
  SmallVector<const MCInst *, 0> EndTrampolines;

  // GNU_args_size at original branches; restore_state does not restore it.
  DenseMap<const MCInst *, uint64_t> ArgsSizes;

  // Unrelaxable branch, reported later on the emission thread.
  const MCInst *Error = nullptr;
  // Whether this fragment emits an FDE, allowing CFI snapshots.
  bool HasCFI = false;
  // Function prefix (--pad-funcs-before or/and --break-funcs option)
  const uint64_t BodyOffset;
  // Whether end trampolines also need to restore GNU_args_size explicitly.
  bool HasArgs = false;
  bool FallThrough = false;
  // Destination of that guard, immediately after the end trampolines.
  MCSymbol *End = nullptr;

  Target target(const MCInst &Inst) const {
    return {BC.MIB->getTargetSymbol(Inst), BC.MIB->getTargetAddend(Inst)};
  }

  MCInst jump(const MCInst &Inst) const {
    MCInst B = Jump;
    B.getOperand(0) = Inst.getOperand(MCPlus::getNumPrimeOperands(Inst) - 1);
    return B;
  }

  struct FragmentInfo {
    DenseMap<const BinaryBasicBlock *, uint64_t> BlockOffsets;
    DenseMap<const MCInst *, uint64_t> BranchOffsets;
    // Last original CFI directive before each branch, or nullptr at entry.
    DenseMap<const MCInst *, const MCInst *> CFIBefore;
    // Existing/inline B sites, rebuilt after branch-form changes.
    DenseMap<TrampolineKey, const MCInst *> BodyTrampolines;
    // End trampolines shared across sources and relaxation iterations.
    DenseMap<TrampolineKey, const MCInst *> SharedEndTrampolines;
    // Only these conditional instructions need to be revisited after selection.
    SmallVector<std::pair<const BinaryBasicBlock *, const MCInst *>, 0>
        Branches;
    uint64_t CodeSize = 0;
    const MCInst *LastCFI = nullptr;
    unsigned MinBranchBits = 63;
    // Outlining cannot model raw CFI or nest snapshots within an existing
    // remember_state/restore_state stack.
    bool HasOpaqueCFI = false;
    bool AllTargetsLocal = true;
    bool PreserveLayout = false;
  };

  // A paired B emits the original conditional's target after a swap.
  Target actualTarget(const MCInst *Inst) const {
    const BranchForm F = Forms.lookup(Inst);
    if (F.Kind == Swap &&
        BC.MII->get(Inst->getOpcode()).isUnconditionalBranch())
      Inst = F.Other;
    return target(*Inst);
  }

  /// Collect branch/CFI information once; subsequent walks only update offsets.
  void updateLayout(FragmentInfo &Info, bool Initial) {
    uint64_t ArgsSize = 0;
    Info.CodeSize = layoutFunctionFragment(
        FF, BC, this, /*Streamer=*/nullptr, /*EmitCodeOnly=*/false,
        [&](const BinaryBasicBlock &BB, uint64_t Offset) {
          Info.BlockOffsets[&BB] = Offset;
        },
        [&](const BinaryBasicBlock &BB, const MCInst &Inst, uint64_t Offset) {
          if (Initial) {
            if (const auto *C = BF.getCFIFor(Inst)) {
              Info.LastCFI = &Inst;
              Info.HasOpaqueCFI |=
                  C->getOperation() == MCCFIInstruction::OpEscape ||
                  C->getOperation() == MCCFIInstruction::OpRememberState ||
                  C->getOperation() == MCCFIInstruction::OpRestoreState;
              if (C->getOperation() == MCCFIInstruction::OpGnuArgsSize)
                ArgsSize = C->getOffset();
            }
          }
          const MCInstrDesc &Desc = BC.MII->get(Inst.getOpcode());
          if (Desc.isPseudo() || !Desc.isBranch() ||
              BC.MIB->isIndirectBranch(Inst))
            return;
          Info.BranchOffsets[&Inst] = Offset;
          if (!Initial)
            return;
          Info.CFIBefore[&Inst] = Info.LastCFI;
          if (BC.MIB->isConditionalBranch(Inst)) {
            Info.Branches.emplace_back(&BB, &Inst);
            if (HasArgs)
              ArgsSizes[&Inst] = ArgsSize;
            const Target Destination = target(Inst);
            const auto *TargetBB = BF.getBasicBlockForLabel(Destination.first);
            Info.AllTargetsLocal &=
                TargetBB && !Destination.second &&
                TargetBB->getFragmentNum() == FF.getFragmentNum();
            Info.MinBranchBits = std::min<unsigned>(
                Info.MinBranchBits, BC.MIB->getPCRelEncodingSize(Inst));
          }
        });
  }

  void rebuildBodyTrampolines(FragmentInfo &Info) {
    // A pair swap can change a B's destination even when no instruction grows.
    Info.BodyTrampolines.clear();
    auto Register = [&](const MCInst *Inst) {
      const Target Destination = actualTarget(Inst);
      if (Destination.first)
        Info.BodyTrampolines.try_emplace(
            TrampolineKey{Destination, Info.CFIBefore.lookup(Inst)}, Inst);
    };
    for (const BinaryBasicBlock *BB : FF) {
      const MCInst *B = BB->getLastNonPseudoInstr();
      if (B && BC.MII->get(B->getOpcode()).isUnconditionalBranch())
        Register(B);
    }
    for (const auto &[BB, Inst] : Info.Branches)
      if (Forms.lookup(Inst).Kind == Inline && !Labels.lookup(Inst).AtEnd)
        Register(Inst);
  }

  size_t endTrampolineIndex(const MCInst *Source,
                            const FragmentInfo &Info) const {
    const auto It =
        llvm::lower_bound(EndTrampolines, Info.BranchOffsets.lookup(Source),
                          [&](const MCInst *Other, uint64_t Offset) {
                            return Info.BranchOffsets.lookup(Other) > Offset;
                          });
    return It - EndTrampolines.begin();
  }

  uint64_t endTrampolineOffset(size_t Index, const FragmentInfo &Info) const {
    // The optional fall-through guard and each trampoline are one AArch64 B.
    return Info.CodeSize +
           (Index + FallThrough) * BC.MII->get(Jump.getOpcode()).getSize();
  }

  uint64_t trampolineOffset(const MCInst *Source,
                            const FragmentInfo &Info) const {
    if (Labels.lookup(Source).AtEnd)
      return endTrampolineOffset(endTrampolineIndex(Source, Info), Info);
    uint64_t Offset = Info.BranchOffsets.lookup(Source);
    // An inline expansion ends with the unconditional B.
    if (Forms.lookup(Source).Kind == Inline)
      Offset += emit(*Source) - BC.MII->get(Jump.getOpcode()).getSize();
    return Offset;
  }

  bool isInRange(const MCInst &Inst, const FragmentInfo &Info, uint64_t Offset,
                 int64_t Addend = 0) const {
    const uint64_t Delta =
        Offset + uint64_t(Addend) - Info.BranchOffsets.lookup(&Inst);
    return !(Delta & 3) && isIntN(BC.MIB->getPCRelEncodingSize(Inst), Delta);
  }

  bool isInRange(const MCInst &Inst, const FragmentInfo &Info,
                 Target Destination) const {
    const auto *BB = BF.getBasicBlockForLabel(Destination.first);
    return BB && Info.BlockOffsets.count(BB) &&
           isInRange(Inst, Info, Info.BlockOffsets.lookup(BB),
                     Destination.second);
  }

  bool tryReuseTrampoline(const MCInst &Inst, const Target &Destination,
                          const FragmentInfo &Info) {
    const TrampolineKey K{Destination, Info.CFIBefore.lookup(&Inst)};
    for (const MCInst *B : {Info.BodyTrampolines.lookup(K),
                            Info.SharedEndTrampolines.lookup(K)}) {
      if (B && actualTarget(B) == Destination &&
          isInRange(Inst, Info, trampolineOffset(B, Info))) {
        Forms[&Inst] = {Redirect, B};
        Labels.try_emplace(B);
        return true;
      }
    }
    return false;
  }

  bool tryOutlineBranch(const BinaryBasicBlock &BB, const MCInst &Inst,
                        const Target &Destination, FragmentInfo &Info) {
    // Lowering preserves saved edges/counts. Read a count only for a matching
    // zero-addend local target and a valid profile.
    BinaryBasicBlock::BinaryBranchInfo Edge;
    const bool Cold =
        BF.hasValidProfile() && BB.hasProfile() && !Destination.second &&
        BB.getSuccessor(Destination.first, Edge) && Edge.Count == 0;
    // A later fall-through must retain its original end CFI state.
    if (BF.isPatch() || (!Cold && !Info.PreserveLayout) || Info.HasOpaqueCFI ||
        (HasCFI && FallThrough && Info.CFIBefore.lookup(&Inst) != Info.LastCFI))
      return false;

    const TrampolineKey K{Destination, Info.CFIBefore.lookup(&Inst)};
    // Reuse was already tried. Do not duplicate an unreachable end trampoline.
    const size_t Index = endTrampolineIndex(&Inst, Info);
    if (Info.SharedEndTrampolines.count(K) ||
        !isInRange(Inst, Info, endTrampolineOffset(Index, Info)))
      return false;
    EndTrampolines.insert(EndTrampolines.begin() + Index, &Inst);
    Labels[&Inst].AtEnd = true;
    if (Destination.first)
      Info.SharedEndTrampolines[K] = &Inst;
    Forms[&Inst] = {Redirect, &Inst};
    return true;
  }

  bool trySwapBranchPair(const BinaryBasicBlock &BB, const MCInst &Inst,
                         const FragmentInfo &Info) {
    if (Info.PreserveLayout || !BC.MIB->isReversibleBranch(Inst))
      return false;

    const auto I = BB.begin() + (&Inst - &*BB.begin());
    const auto NextIt =
        std::find_if(std::next(I), BB.end(),
                     [&](const MCInst &M) { return !BC.MIB->isPseudo(M); });
    const MCInst *Next = NextIt == BB.end() ? nullptr : &*NextIt;
    // B.cond far; B near -> B.!cond near; B far, with the same CFI row.
    if (!Next || Info.CFIBefore.lookup(Next) != Info.CFIBefore.lookup(&Inst) ||
        !BC.MII->get(Next->getOpcode()).isUnconditionalBranch())
      return false;
    if (!isInRange(Inst, Info, target(*Next)))
      return false;
    Forms[&Inst] = {Swap, Next};
    Forms[Next] = {Swap, &Inst};
    return true;
  }

  bool relaxBranches(FragmentInfo &Info, bool &LayoutChanged) {
    // Growth changes reachability; swaps can invalidate users of an existing B.
    bool PlanChanged = false;
    for (const auto &[BB, InstPtr] : Info.Branches) {
      const MCInst &Inst = *InstPtr;
      const BranchForm F = Forms.lookup(&Inst);
      if (F.Kind == Inline)
        continue;
      const Target Destination = target(Inst);
      const BinaryBasicBlock *TargetBB =
          BF.getBasicBlockForLabel(Destination.first);
      if (TargetBB && Info.PreserveLayout && !Destination.second)
        continue;
      if (F.Kind == Original && isInRange(Inst, Info, Destination))
        continue;

      if (F.Kind == Redirect) {
        if (actualTarget(F.Other) == Destination &&
            isInRange(Inst, Info, trampolineOffset(F.Other, Info)))
          continue;
      } else if (F.Kind == Swap) {
        if (isInRange(Inst, Info, target(*F.Other)))
          continue;
      } else if (tryReuseTrampoline(Inst, Destination, Info) ||
                 tryOutlineBranch(*BB, Inst, Destination, Info) ||
                 trySwapBranchPair(*BB, Inst, Info)) {
        PlanChanged = true;
        continue;
      }

      // Last resort: expand at the source under its original CFI row.
      // Once inline, a branch never needs another range check.
      if (Info.PreserveLayout) {
        Error = &Inst;
        return false;
      }
      if (F.Kind == Swap)
        Forms.erase(F.Other);
      Forms[&Inst] = {Inline};
      LayoutChanged = true;
      if (Destination.first && !Labels.lookup(&Inst).AtEnd)
        Info.BodyTrampolines.try_emplace(
            TrampolineKey{Destination, Info.CFIBefore.lookup(&Inst)}, &Inst);
      PlanChanged = true;
    }
    return PlanChanged;
  }

public:
  LocalBranchRelaxation(BinaryFunction &BF, const FunctionFragment &FF,
                        const MCInst &Jump, uint64_t BodyOffset)
      : BC(BF.getBinaryContext()), BF(BF), FF(FF), Jump(Jump),
        BodyOffset(BodyOffset) {}

  uint64_t getBodyOffset() const { return BodyOffset; }

  void run() {
    HasCFI = BF.hasCFI() && !(BF.isPatch() && BF.isAnonymous());
    HasArgs = HasCFI && BF.usesGnuArgsSize();
    FragmentInfo Info;
    Info.PreserveLayout = !BF.isSimple() || BF.isPatch();
    FallThrough = fragmentMayFallThrough(FF, BC);
    updateLayout(Info, /*Initial=*/true);

    // If the shortest encoding spans the whole body, no local branch can
    // initiate growth. Addends disable this shortcut.
    if (Info.AllTargetsLocal &&
        Info.CodeSize - BodyOffset < (uint64_t(1) << (Info.MinBranchBits - 1)))
      return;

    for (;;) {
      rebuildBodyTrampolines(Info);
      bool LayoutChanged = false;
      if (!relaxBranches(Info, LayoutChanged))
        break;
      if (LayoutChanged)
        updateLayout(Info, /*Initial=*/false);
    }
  }

  bool prepare() {
    if (Error) {
      BC.Ctx->reportError(
          Error->getLoc(),
          Twine("cannot relax branch in function ") + BF.getPrintName() +
              (BF.isPatch() ? ": fixed-size patch cannot grow for local branch "
                              "relaxation"
                            : ": end-of-function trampoline is out of reach"));
      return false;
    }
    // Allocate the labels on the emission thread.
    for (TrampolineLabel &Label : Labels.values())
      Label.Symbol = BC.Ctx->createTempSymbol("local_trampoline");
    if (FallThrough && !EndTrampolines.empty())
      End = BC.Ctx->createTempSymbol("local_trampolines_end");
    return true;
  }

  /// A null streamer selects and sizes exactly the same MCInsts as emission.
  uint64_t emit(const MCInst &Inst, MCStreamer *S = nullptr) const {
    if (BC.MIB->isPseudo(Inst))
      return 0;
    if (!BC.MII->get(Inst.getOpcode()).isBranch())
      return emitInstruction(BC, Inst, S);
    const BranchForm F = Forms.lookup(&Inst);
    const TrampolineLabel Label = S ? Labels.lookup(&Inst) : TrampolineLabel{};
    if (S && Label.AtEnd && HasCFI)
      S->emitCFIRememberState(Inst.getLoc());
    if (Label.Symbol && !Label.AtEnd && F.Kind != Inline)
      S->emitLabel(Label.Symbol);
    uint64_t Size = 0;
    if (F.Kind == Swap) {
      if (BC.MIB->isConditionalBranch(Inst)) {
        MCInst B =
            BC.MIB->reverseBranchCondition(Inst, nullptr, nullptr).front();
        B.getOperand(MCPlus::getNumPrimeOperands(B) - 1) =
            F.Other->getOperand(0);
        return emitInstruction(BC, B, S);
      }
      return emitInstruction(BC, jump(*F.Other), S);
    }
    if (F.Kind == Redirect)
      return emitInstruction(BC, Inst, S, Labels.lookup(F.Other).Symbol);
    if (F.Kind != Inline)
      return emitInstruction(BC, Inst, S);

    MCSymbol *Next = S ? BC.Ctx->createTempSymbol("local_branch_end") : nullptr;
    if (BC.MIB->isReversibleBranch(Inst)) {
      for (const MCInst &B :
           BC.MIB->reverseBranchCondition(Inst, nullptr, nullptr))
        Size += emitInstruction(
            BC, B, S, BC.MII->get(B.getOpcode()).isBranch() ? Next : nullptr);
    } else {
      MCSymbol *Taken =
          S ? BC.Ctx->createTempSymbol("local_branch_taken") : nullptr;
      Size += emitInstruction(BC, Inst, S, Taken);
      MCInst Skip = jump(Inst);
      Skip.setLoc(Inst.getLoc());
      Size += emitInstruction(BC, Skip, S, Next);
      if (S)
        S->emitLabel(Taken);
    }
    if (Label.Symbol && !Label.AtEnd)
      S->emitLabel(Label.Symbol);
    Size += emitInstruction(BC, jump(Inst), S);
    if (S)
      S->emitLabel(Next);
    return Size;
  }

  void emitTrampolines(MCStreamer &S) const {
    if (End)
      emitInstruction(BC, Jump, &S, End);
    for (const MCInst *Source : EndTrampolines) {
      if (HasCFI) {
        S.emitCFIRestoreState(Source->getLoc());
        if (HasArgs)
          S.emitCFIGnuArgsSize(ArgsSizes.lookup(Source));
      }
      S.emitLabel(Labels.lookup(Source).Symbol);
      emitInstruction(BC, jump(*Source), &S);
    }
    if (End)
      S.emitLabel(End);
  }
};


class BinaryEmitter {
private:
  BinaryEmitter(const BinaryEmitter &) = delete;
  BinaryEmitter &operator=(const BinaryEmitter &) = delete;

  MCStreamer &Streamer;
  BinaryContext &BC;
  bool RelaxBranches;
  MCInst LocalBranchJump;
  // Per-fragment choices produced before emission and consumed by emitFunction.
  // Keeping them here separates parallel selection from serial symbol/MC
  // output.
  DenseMap<const FunctionFragment *, std::unique_ptr<LocalBranchRelaxation>>
      Relaxations;

  void relaxLocalBranches();

public:
  BinaryEmitter(MCStreamer &Streamer, BinaryContext &BC,
                bool RelaxBranches = false)
      : Streamer(Streamer), BC(BC), RelaxBranches(RelaxBranches) {}

  /// Emit all code and data.
  void emitAll(StringRef OrgSecPrefix);

  /// Emit function code. The caller is responsible for emitting function
  /// symbol(s) and setting the section to emit the code to.
  void emitFunctionBody(BinaryFunction &BF, FunctionFragment &FF,
                        bool EmitCodeOnly = false,
                        LocalBranchRelaxation *Relaxation = nullptr);

private:
  /// Emit function code.
  void emitFunctions();

  /// Emit a single function.
  bool emitFunction(BinaryFunction &BF, FunctionFragment &FF);

  /// Helper for emitFunctionBody to write data inside a function
  /// (used for AArch64)
  void emitConstantIslands(BinaryFunction &BF, bool EmitColdPart,
                           BinaryFunction *OnBehalfOf = nullptr);

  /// Emit jump tables for the function.
  void emitJumpTables(const BinaryFunction &BF);

  /// Emit jump table data. Callee supplies sections for the data.
  void emitJumpTable(const JumpTable &JT, MCSection *HotSection,
                     MCSection *ColdSection);

  void emitCFIInstruction(const MCCFIInstruction &Inst) const;

  /// Emit exception handling ranges for the function fragment.
  void emitLSDA(BinaryFunction &BF, const FunctionFragment &FF);

  /// Emit line number information corresponding to \p NewLoc. \p PrevLoc
  /// provides a context for de-duplication of line number info.
  /// \p FirstInstr indicates if \p NewLoc represents the first instruction
  /// in a sequence, such as a function fragment.
  ///
  /// If \p NewLoc location matches \p PrevLoc, no new line number entry will be
  /// created and the function will return \p PrevLoc while \p InstrLabel will
  /// be ignored. Otherwise, the caller should use \p InstrLabel to mark the
  /// corresponding instruction by emitting \p InstrLabel before it.
  /// If \p InstrLabel is set by the caller, its value will be used with \p
  /// \p NewLoc. If it was nullptr on entry, it will be populated with a pointer
  /// to a new temp symbol used with \p NewLoc.
  ///
  /// Return new current location which is either \p NewLoc or \p PrevLoc.
  SMLoc emitLineInfo(const BinaryFunction &BF, SMLoc NewLoc, SMLoc PrevLoc,
                     bool FirstInstr, MCSymbol *&InstrLabel);

  /// Use \p FunctionEndSymbol to mark the end of the line info sequence.
  /// Note that it does not automatically result in the insertion of the EOS
  /// marker in the line table program, but provides one to the DWARF generator
  /// when it needs it.
  void emitLineInfoEnd(const BinaryFunction &BF, MCSymbol *FunctionEndSymbol,
                       const DWARFUnit &Unit);

  /// Emit debug line info for unprocessed functions from CUs that include
  /// emitted functions.
  void emitDebugLineInfoForOriginalFunctions();

  /// Emit debug line for CUs that were not modified.
  void emitDebugLineInfoForUnprocessedCUs();

  /// Emit data sections that have code references in them.
  void emitDataSections(StringRef OrgSecPrefix);
};

} // anonymous namespace

void BinaryEmitter::emitAll(StringRef OrgSecPrefix) {
  if (useFragmentSections(BC)) {
    if (!BC.HasRelocations) {
      BC.Ctx->reportError(SMLoc(),
                          "--turn-off-stubs requires relocation mode for "
                          "independently placed AArch64 fragments");
      return;
    }
  }

  Streamer.initSections(*BC.STI);
  Streamer.setUseAssemblerInfoForParsing(false);

  if (opts::UpdateDebugSections && BC.isELF()) {
    // Force the emission of debug line info into allocatable section to ensure
    // JITLink will process it.
    //
    // NB: on MachO all sections are required for execution, hence no need
    //     to change flags/attributes.
    MCSectionELF *ELFDwarfLineSection =
        static_cast<MCSectionELF *>(BC.MOFI->getDwarfLineSection());
    ELFDwarfLineSection->setFlags(ELF::SHF_ALLOC);
    MCSectionELF *ELFDwarfLineStrSection =
        static_cast<MCSectionELF *>(BC.MOFI->getDwarfLineStrSection());
    ELFDwarfLineStrSection->setFlags(ELF::SHF_ALLOC);
  }

  if (RuntimeLibrary *RtLibrary = BC.getRuntimeLibrary())
    RtLibrary->emitBinary(BC, Streamer);

  BC.getTextSection()->setAlignment(Align(BC.AlignText));

  // Select forms before writing function bytes so workers never use Streamer.
  if (RelaxBranches)
    relaxLocalBranches();
  emitFunctions();

  if (opts::UpdateDebugSections) {
    emitDebugLineInfoForOriginalFunctions();
    DwarfLineTable::emit(BC, Streamer);
  }

  emitDataSections(OrgSecPrefix);

  // TODO Enable for Mach-O once BinaryContext::getDataSection supports it.
  if (BC.isELF())
    AddressMap::emit(Streamer, BC);
  Streamer.setUseAssemblerInfoForParsing(true);
}

void BinaryEmitter::relaxLocalBranches() {
  BC.MIB->createUncondBranch(LocalBranchJump, BC.Ctx->createTempSymbol(),
                             BC.Ctx.get());
  for (BinaryFunction *BF : BC.getOutputBinaryFunctions()) {
    if (!BC.shouldEmit(*BF) || (BF->empty() && !BF->hasIslandsInfo()) ||
        BF->getState() == BinaryFunction::State::Empty)
      continue;
    const uint64_t BodyOffset = opts::padFunctionBefore(*BF) +
                                emitFunctionEntryTrap(*BF, /*S=*/nullptr);
    if (BF->isSplit() && BF->hasConstantIsland())
      BF->duplicateConstantIslands();
    for (FunctionFragment &FF : BF->getLayout().fragments())
      // Skip only empty fragments without constant islands
      if (!FF.isSplitFragment() || !FF.empty() || BF->hasConstantIsland())
        Relaxations[&FF] = std::make_unique<LocalBranchRelaxation>(
            *BF, FF, LocalBranchJump, BodyOffset);
  }
  const auto &Plans = Relaxations;
  auto Relax = [&](BinaryFunction &BF) {
    for (FunctionFragment &FF : BF.getLayout().fragments())
      if (auto I = Plans.find(&FF); I != Plans.end())
        I->second->run();
  };
  ParallelUtilities::runOnEachFunction(
      BC, ParallelUtilities::SP_INST_LINEAR, Relax,
      [&](const BinaryFunction &BF) {
        return !Plans.count(&BF.getLayout().getMainFragment());
      },
      "RelaxLocalBranches");
  // runOnEachFunction only visits input functions.
  for (BinaryFunction *BF : BC.getInjectedBinaryFunctions())
    Relax(*BF);
}

void BinaryEmitter::emitFunctions() {
  auto emit = [&](const BinaryFunctionListType &Functions) {
    const bool HasProfile = BC.NumProfiledFuncs > 0;
    const bool OriginalAllowAutoPadding = Streamer.getAllowAutoPadding();
    for (BinaryFunction *Function : Functions) {
      if (!BC.shouldEmit(*Function))
        continue;

      LLVM_DEBUG(dbgs() << "BOLT: generating code for function \"" << *Function
                        << "\" : " << Function->getFunctionNumber() << '\n');

      // Was any part of the function emitted.
      bool Emitted = false;

      // Turn off Intel JCC Erratum mitigation for cold code if requested
      if (HasProfile && BC.X86AlignBranchBoundaryHotOnly &&
          !Function->hasValidProfile())
        Streamer.setAllowAutoPadding(false);

      FunctionLayout &Layout = Function->getLayout();
      Emitted |= emitFunction(*Function, Layout.getMainFragment());

      if (Function->isSplit()) {
        if (BC.X86AlignBranchBoundaryHotOnly)
          Streamer.setAllowAutoPadding(false);

        assert((Layout.fragment_size() == 1 || Function->isSimple()) &&
               "Only simple functions can have fragments");
        for (FunctionFragment &FF : Layout.getSplitFragments()) {
          // Skip empty fragments so no symbols and sections for empty fragments
          // are generated
          if (FF.empty() && !Function->hasConstantIsland())
            continue;
          Emitted |= emitFunction(*Function, FF);
        }
      }

      Streamer.setAllowAutoPadding(OriginalAllowAutoPadding);

      if (Emitted)
        Function->setEmitted(/*KeepCFG=*/opts::PrintCacheMetrics);
    }
  };

  // Mark the start of hot text.
  if (opts::HotText) {
    Streamer.switchSection(BC.getTextSection());
    Streamer.emitLabel(BC.getHotTextStartSymbol());
  }

  // Emit functions in sorted order.
  emit(BC.getOutputBinaryFunctions());

  // Mark the end of hot text.
  if (opts::HotText) {
    MCSection *EndSection = BC.HasWarmSection
                                ? BC.getCodeSection(BC.getWarmCodeSectionName())
                                : BC.getTextSection();
    if (useFragmentSections(BC)) {
      for (const auto &Fragment : llvm::reverse(BC.EmittedFragmentSections)) {
        if (Fragment.OutputSection == EndSection->getName()) {
          EndSection = BC.getCodeSection(Fragment.Name);
          break;
        }
      }
      // If no fragment was emitted into this logical section (e.g. an empty
      // warm section), keep the marker in the logical section itself.
    }
    Streamer.switchSection(EndSection);
    Streamer.emitLabel(BC.getHotTextEndSymbol());
  }
}

bool BinaryEmitter::emitFunction(BinaryFunction &Function,
                                 FunctionFragment &FF) {
  if (Function.size() == 0 && !Function.hasIslandsInfo())
    return false;

  if (Function.getState() == BinaryFunction::State::Empty)
    return false;

  // Avoid emitting function without instructions when overwriting the original
  // function in-place. Otherwise, emit the empty function to define the symbol.
  if (!BC.HasRelocations && !Function.hasNonPseudoInstructions())
    return false;

  LocalBranchRelaxation *Relaxation = nullptr;
  if (auto I = Relaxations.find(&FF); I != Relaxations.end()) {
    Relaxation = I->second.get();
    if (!Relaxation->prepare())
      return false;
  }

  MCSection *Section =
      BC.getCodeSection(Function.getCodeSectionName(FF.getFragmentNum()));
  const bool FragmentSections = useFragmentSections(BC);
  const uint16_t MaxAlignBytes = FF.isSplitFragment()
                                     ? Function.getMaxColdAlignmentBytes()
                                     : Function.getMaxAlignmentBytes();
  const size_t PrefixSize = opts::padFunctionBefore(Function);
  const bool HasFallThrough =
      FragmentSections && !Function.isPatch() && fragmentMayFallThrough(FF, BC);
  if (FragmentSections) {
    // Keep the logical section base aligned, but do not turn a preferred
    // function alignment into a mandatory alignment of every fragment. MC
    // records the requirements of the fragment's body (e.g. constant islands)
    // on its private section. JITLink applies the bounded entry alignment.
    Section->ensureMinAlignment(Align(BC.AlignFunctions));
    if (MaxAlignBytes > 0)
      Section->ensureMinAlignment(Function.getAlign());
    std::string Name =
        (Twine(".bolt.fragment.") + Twine(BC.EmittedFragmentSections.size()))
            .str();
    BC.EmittedFragmentSections.push_back(
        {Name, Section->getName().str(), Section->getAlign().value(),
         HasFallThrough, Function.getAlignment(), MaxAlignBytes, PrefixSize});
    Section = BC.getCodeSection(Name);
  }
  Streamer.switchSection(Section);
  Section->setHasInstructions(true);
  BC.Ctx->addGenDwarfSection(Section);

  if (BC.HasRelocations) {
    // Set section alignment to at least maximum possible object alignment.
    // We need this to support LongJmp and other passes that calculates
    // tentative layout.
    if (!FragmentSections)
      Section->ensureMinAlignment(Align(BC.AlignFunctions));

    Streamer.emitCodeAlignment(Function.getMinAlign(), *BC.STI);
    if (!FragmentSections && MaxAlignBytes > 0)
      Streamer.emitCodeAlignment(Function.getAlign(), *BC.STI, MaxAlignBytes);
  } else {
    Streamer.emitCodeAlignment(Function.getAlign(), *BC.STI);
  }

  if (size_t Padding = PrefixSize) {
    // Handle padFuncsBefore after the above alignment logic but before
    // symbol addresses are decided.
    if (!BC.HasRelocations) {
      BC.errs() << "BOLT-ERROR: -pad-before-funcs is not supported in "
                << "non-relocation mode\n";
      exit(1);
    }

    // Preserve Function.getMinAlign().
    if (!isAligned(Function.getMinAlign(), Padding)) {
      BC.errs() << "BOLT-ERROR: user-requested " << Padding
                << " padding bytes before function " << Function
                << " is not a multiple of the minimum function alignment ("
                << Function.getMinAlign().value() << ").\n";
      exit(1);
    }

    LLVM_DEBUG(dbgs() << "BOLT-DEBUG: padding before function " << Function
                      << " with " << Padding << " bytes\n");

    // Since the padding is not executed, it can be null bytes.
    Streamer.emitFill(Padding, 0);
  }

  MCContext &Context = Streamer.getContext();
  const MCAsmInfo &MAI = Context.getAsmInfo();

  MCSymbol *const StartSymbol = Function.getSymbol(FF.getFragmentNum());

  // Emit all symbols associated with the main function entry.
  if (FF.isMainFragment()) {
    for (MCSymbol *Symbol : Function.getSymbols()) {
      Streamer.emitSymbolAttribute(Symbol, MCSA_ELF_TypeFunction);
      Streamer.emitLabel(Symbol);
    }
  } else {
    Streamer.emitSymbolAttribute(StartSymbol, MCSA_ELF_TypeFunction);
    Streamer.emitLabel(StartSymbol);
  }

  const bool NeedsFDE =
      Function.hasCFI() && !(Function.isPatch() && Function.isAnonymous());
  // Emit CFI start
  if (NeedsFDE) {
    Streamer.emitCFIStartProc(/*IsSimple=*/false);
    if (Function.getPersonalityFunction() != nullptr)
      Streamer.emitCFIPersonality(Function.getPersonalityFunction(),
                                  Function.getPersonalityEncoding());
    MCSymbol *LSDASymbol = Function.getLSDASymbol(FF.getFragmentNum());
    if (LSDASymbol)
      Streamer.emitCFILsda(LSDASymbol, BC.LSDAEncoding);
    else
      Streamer.emitCFILsda(0, dwarf::DW_EH_PE_omit);
    // Emit CFI instructions relative to the CIE
    for (const MCCFIInstruction &CFIInstr : Function.cie()) {
      // Only write CIE CFI insns that LLVM will not already emit
      const std::vector<MCCFIInstruction> &FrameInstrs =
          MAI.getInitialFrameState();
      if (!llvm::is_contained(FrameInstrs, CFIInstr))
        emitCFIInstruction(CFIInstr);
    }
  }

  assert((Function.empty() || !(*Function.begin()).isCold()) &&
         "first basic block should never be cold");

  emitFunctionEntryTrap(Function, &Streamer);

  // Emit code.
  emitFunctionBody(Function, FF, /*EmitCodeOnly=*/false, Relaxation);
  Relaxations.erase(&FF);

  // Emit padding if requested.
  if (size_t Padding = opts::padFunctionAfter(Function)) {
    LLVM_DEBUG(dbgs() << "BOLT-DEBUG: padding function " << Function << " with "
                      << Padding << " bytes\n");
    Streamer.emitFill(Padding, MAI.getTextAlignFillValue());
  }

  if (opts::MarkFuncs)
    Streamer.emitBytes(BC.MIB->getTrapFillValue());

  MCSymbol *EndSymbol = Function.getFunctionEndLabel(FF.getFragmentNum());
  if (HasFallThrough) {
    MCInst Branch;
    // Create branch with temporary target.
    // JITLink will retarget it past the reserved stub space.
    BC.MIB->createUncondBranch(Branch, EndSymbol, BC.Ctx.get());
    LLVM_DEBUG(dbgs() << "BOLT-DEBUG: create 'fall through' guard "
                      << "for " << StartSymbol->getName() << "\n");

    Streamer.emitInstruction(Branch, *BC.STI);
  }

  // Emit CFI end
  if (NeedsFDE)
    Streamer.emitCFIEndProc();

  Streamer.emitLabel(EndSymbol);

  if (MAI.hasDotTypeDotSizeDirective()) {
    const MCExpr *SizeExpr = MCBinaryExpr::createSub(
        MCSymbolRefExpr::create(EndSymbol, Context),
        MCSymbolRefExpr::create(StartSymbol, Context), Context);
    Streamer.emitELFSize(StartSymbol, SizeExpr);
  }

  if (opts::UpdateDebugSections && !Function.getDWARFUnits().empty())
    for (const auto &[_, Unit] : Function.getDWARFUnits())
      emitLineInfoEnd(Function, EndSymbol, *Unit);

  // Exception handling info for the function.
  emitLSDA(Function, FF);

  if (FF.isMainFragment() && opts::JumpTables > JTS_NONE)
    emitJumpTables(Function);

  return true;
}

void BinaryEmitter::emitFunctionBody(BinaryFunction &BF, FunctionFragment &FF,
                                     bool EmitCodeOnly,
                                     LocalBranchRelaxation *Relaxation) {
  if (!EmitCodeOnly && !Relaxation && FF.isSplitFragment() &&
      BF.hasConstantIsland()) {
    assert(BF.getLayout().isHotColdSplit() &&
           "Constant island support only with hot/cold split");
    BF.duplicateConstantIslands();
  }

  // These callbacks emit metadata at positions chosen by the shared walk.
  bool FirstInstr = true;
  SMLoc LastLocSeen;
  layoutFunctionFragment(
      FF, BC, Relaxation, &Streamer, EmitCodeOnly,
      [&](BinaryBasicBlock &BB, uint64_t) {
        LastLocSeen = SMLoc();
        Streamer.emitLabel(BB.getLabel());
        if (!EmitCodeOnly)
          if (MCSymbol *EntrySymbol = BF.getSecondaryEntryPointSymbol(BB))
            Streamer.emitLabel(EntrySymbol);
      },
      [&](BinaryBasicBlock &BB, const MCInst &Instr, uint64_t) {
        if (EmitCodeOnly)
          return;

        if (BC.MIB->isCFI(Instr)) {
          emitCFIInstruction(*BF.getCFIFor(Instr));
          return;
        }

        // A symbol to be emitted before the instruction to mark its location.
        MCSymbol *InstrLabel = BC.MIB->getInstLabel(Instr);
        if (opts::UpdateDebugSections && !BF.getDWARFUnits().empty()) {
          LastLocSeen = emitLineInfo(BF, Instr.getLoc(), LastLocSeen,
                                     FirstInstr, InstrLabel);
          FirstInstr = false;
        }

        // Tag the original instruction start for BAT, probes and DWARF ranges.
        // Any instructions added by relaxation follow this label.
        if (BF.requiresPreciseAddressMap() && BC.MIB->getOffset(Instr)) {
          const uint32_t Offset = *BC.MIB->getOffset(Instr);
          if (!InstrLabel)
            InstrLabel = BC.Ctx->createTempSymbol();
          BB.getLocSyms().emplace_back(Offset, InstrLabel);
        }
        if (InstrLabel)
          Streamer.emitLabel(InstrLabel);
      });

  if (!EmitCodeOnly)
    emitConstantIslands(BF, FF.isSplitFragment());
}

void BinaryEmitter::emitConstantIslands(BinaryFunction &BF, bool EmitColdPart,
                                        BinaryFunction *OnBehalfOf) {
  if (!BF.hasIslandsInfo())
    return;

  BinaryFunction::IslandInfo &Islands = BF.getIslandInfo();
  if (Islands.DataOffsets.empty() && Islands.Dependency.empty())
    return;

  // AArch64 requires CI to be aligned to 8 bytes due to access instructions
  // restrictions. E.g. the ldr with imm, where imm must be aligned to 8 bytes.
  const uint16_t Alignment = OnBehalfOf
                                 ? OnBehalfOf->getConstantIslandAlignment()
                                 : BF.getConstantIslandAlignment();
  Streamer.emitCodeAlignment(Align(Alignment), *BC.STI);

  if (!OnBehalfOf) {
    if (!EmitColdPart)
      Streamer.emitLabel(BF.getFunctionConstantIslandLabel());
    else
      Streamer.emitLabel(BF.getFunctionColdConstantIslandLabel());
  }

  assert((!OnBehalfOf || Islands.Proxies[OnBehalfOf].size() > 0) &&
         "spurious OnBehalfOf constant island emission");

  assert(!BF.isInjected() &&
         "injected functions should not have constant islands");
  // Raw contents of the function.
  StringRef SectionContents = BF.getOriginSection()->getContents();

  // Raw contents of the function.
  StringRef FunctionContents = SectionContents.substr(
      BF.getAddress() - BF.getOriginSection()->getAddress(), BF.getMaxSize());

  if (opts::Verbosity && !OnBehalfOf)
    BC.outs() << "BOLT-INFO: emitting constant island for function " << BF
              << "\n";

  // We split the island into smaller blocks and output labels between them.
  auto IS = Islands.Offsets.begin();
  for (auto DataIter = Islands.DataOffsets.begin();
       DataIter != Islands.DataOffsets.end(); ++DataIter) {
    uint64_t FunctionOffset = *DataIter;
    uint64_t EndOffset = 0ULL;

    // Determine size of this data chunk
    auto NextData = std::next(DataIter);
    auto CodeIter = Islands.CodeOffsets.lower_bound(*DataIter);
    if (CodeIter == Islands.CodeOffsets.end() &&
        NextData == Islands.DataOffsets.end())
      EndOffset = BF.getMaxSize();
    else if (CodeIter == Islands.CodeOffsets.end())
      EndOffset = *NextData;
    else if (NextData == Islands.DataOffsets.end())
      EndOffset = *CodeIter;
    else
      EndOffset = (*CodeIter > *NextData) ? *NextData : *CodeIter;

    if (FunctionOffset == EndOffset)
      continue; // Size is zero, nothing to emit

    auto emitCI = [&](uint64_t &FunctionOffset, uint64_t EndOffset) {
      if (FunctionOffset >= EndOffset)
        return;

      for (auto It = Islands.Relocations.lower_bound(FunctionOffset);
           It != Islands.Relocations.end(); ++It) {
        if (It->first >= EndOffset)
          break;

        const Relocation &Relocation = It->second;
        if (FunctionOffset < Relocation.Offset) {
          Streamer.emitBytes(
              FunctionContents.slice(FunctionOffset, Relocation.Offset));
          FunctionOffset = Relocation.Offset;
        }

        LLVM_DEBUG(
            dbgs() << "BOLT-DEBUG: emitting constant island relocation"
                   << " for " << BF << " at offset 0x"
                   << Twine::utohexstr(Relocation.Offset) << " with size "
                   << Relocation::getSizeForType(Relocation.Type) << '\n');

        FunctionOffset += Relocation.emit(&Streamer);
      }

      assert(FunctionOffset <= EndOffset && "overflow error");
      if (FunctionOffset < EndOffset) {
        Streamer.emitBytes(FunctionContents.slice(FunctionOffset, EndOffset));
        FunctionOffset = EndOffset;
      }
    };

    // Emit labels, relocs and data
    while (IS != Islands.Offsets.end() && IS->first < EndOffset) {
      auto NextLabelOffset =
          IS == Islands.Offsets.end() ? EndOffset : IS->first;
      auto NextStop = std::min(NextLabelOffset, EndOffset);
      assert(NextStop <= EndOffset && "internal overflow error");
      emitCI(FunctionOffset, NextStop);
      if (IS != Islands.Offsets.end() && FunctionOffset == IS->first) {
        // This is a slightly complex code to decide which label to emit. We
        // have 4 cases to handle: regular symbol, cold symbol, regular or cold
        // symbol being emitted on behalf of an external function.
        if (!OnBehalfOf) {
          if (!EmitColdPart) {
            LLVM_DEBUG(dbgs() << "BOLT-DEBUG: emitted label "
                              << IS->second->getName() << " at offset 0x"
                              << Twine::utohexstr(IS->first) << '\n');
            if (IS->second->isUndefined())
              Streamer.emitLabel(IS->second);
            else
              assert(BF.hasName(std::string(IS->second->getName())));
          } else if (Islands.ColdSymbols.count(IS->second) != 0) {
            LLVM_DEBUG(dbgs()
                       << "BOLT-DEBUG: emitted label "
                       << Islands.ColdSymbols[IS->second]->getName() << '\n');
            if (Islands.ColdSymbols[IS->second]->isUndefined())
              Streamer.emitLabel(Islands.ColdSymbols[IS->second]);
          }
        } else {
          if (!EmitColdPart) {
            if (MCSymbol *Sym = Islands.Proxies[OnBehalfOf][IS->second]) {
              LLVM_DEBUG(dbgs() << "BOLT-DEBUG: emitted label "
                                << Sym->getName() << '\n');
              Streamer.emitLabel(Sym);
            }
          } else if (MCSymbol *Sym =
                         Islands.ColdProxies[OnBehalfOf][IS->second]) {
            LLVM_DEBUG(dbgs() << "BOLT-DEBUG: emitted label " << Sym->getName()
                              << '\n');
            Streamer.emitLabel(Sym);
          }
        }
        ++IS;
      }
    }
    assert(FunctionOffset <= EndOffset && "overflow error");
    emitCI(FunctionOffset, EndOffset);
  }
  assert(IS == Islands.Offsets.end() && "some symbols were not emitted!");

  if (OnBehalfOf)
    return;
  // Now emit constant islands from other functions that we may have used in
  // this function.
  for (BinaryFunction *ExternalFunc : Islands.Dependency)
    emitConstantIslands(*ExternalFunc, EmitColdPart, &BF);
}

SMLoc BinaryEmitter::emitLineInfo(const BinaryFunction &BF, SMLoc NewLoc,
                                  SMLoc PrevLoc, bool FirstInstr,
                                  MCSymbol *&InstrLabel) {
  if (NewLoc.getPointer() == nullptr ||
      NewLoc.getPointer() == PrevLoc.getPointer())
    return PrevLoc;
  const ClusteredRows *Cluster = ClusteredRows::fromSMLoc(NewLoc);

  auto addToLineTable = [&](DebugLineTableRowRef RowReference,
                            const DWARFUnit &TargetCU, unsigned Flags,
                            MCSymbol &InstrLabel,
                            const DWARFDebugLine::Row &CurrentRow) {
    const uint64_t TargetUnitIndex = TargetCU.getOffset();
    unsigned TargetFilenum = CurrentRow.File;
    const uint32_t CurrentUnitIndex = RowReference.DwCompileUnitIndex;
    // If the CU id from the current instruction location does not
    // match the target CU id, it means that we have come across some
    // inlined code (by BOLT).  We must look up the CU for the instruction's
    // original function and get the line table from that.
    if (TargetUnitIndex != CurrentUnitIndex) {
      // Add filename from the inlined function to the current CU.
      TargetFilenum = BC.addDebugFilenameToUnit(
          TargetUnitIndex, CurrentUnitIndex, CurrentRow.File);
    }
    BC.Ctx->setCurrentDwarfLoc(TargetFilenum, CurrentRow.Line,
                               CurrentRow.Column, Flags, CurrentRow.Isa,
                               CurrentRow.Discriminator);
    const MCDwarfLoc &DwarfLoc = BC.Ctx->getCurrentDwarfLoc();
    BC.Ctx->clearDwarfLocSeen();
    const MCLineSection::MCLineDivisionMap &MapLineEntries =
        BC.getDwarfLineTable(TargetUnitIndex)
            .getMCLineSections()
            .getMCLineEntries();
    const auto *It = MapLineEntries.find(Streamer.getCurrentSectionOnly());
    MCDwarfLineEntry NewLineEntry = MCDwarfLineEntry(&InstrLabel, DwarfLoc);

    // Check if line table exists and has entries before doing comparison.
    if (It != MapLineEntries.end() && !It->second.empty()) {
      // Check if the new line entry has the same debug info as the last one
      // to avoid duplicates. We don't compare labels since different
      // instructions can have the same line info.
      const auto &LastEntry = It->second.back();
      if (LastEntry.getFileNum() == NewLineEntry.getFileNum() &&
          LastEntry.getLine() == NewLineEntry.getLine() &&
          LastEntry.getColumn() == NewLineEntry.getColumn() &&
          LastEntry.getFlags() == NewLineEntry.getFlags() &&
          LastEntry.getIsa() == NewLineEntry.getIsa() &&
          LastEntry.getDiscriminator() == NewLineEntry.getDiscriminator())
        return;
    }

    BC.getDwarfLineTable(TargetUnitIndex)
        .getMCLineSections()
        .addLineEntry(NewLineEntry, Streamer.getCurrentSectionOnly());
  };

  if (!InstrLabel)
    InstrLabel = BC.Ctx->createTempSymbol();
  for (DebugLineTableRowRef RowReference : Cluster->getRows()) {
    const DWARFDebugLine::LineTable *CurrentLineTable =
        BC.DwCtx->getLineTableForUnit(
            BC.DwCtx->getCompileUnitForOffset(RowReference.DwCompileUnitIndex));
    const DWARFDebugLine::Row &CurrentRow =
        CurrentLineTable->Rows[RowReference.RowIndex - 1];
    unsigned Flags = (DWARF2_FLAG_IS_STMT * CurrentRow.IsStmt) |
                     (DWARF2_FLAG_BASIC_BLOCK * CurrentRow.BasicBlock) |
                     (DWARF2_FLAG_PROLOGUE_END * CurrentRow.PrologueEnd) |
                     (DWARF2_FLAG_EPILOGUE_BEGIN * CurrentRow.EpilogueBegin);

    // Always emit is_stmt at the beginning of function fragment.
    if (FirstInstr)
      Flags |= DWARF2_FLAG_IS_STMT;
    const auto &FunctionDwarfUnits = BF.getDWARFUnits();
    auto It = FunctionDwarfUnits.find(RowReference.DwCompileUnitIndex);
    if (It != FunctionDwarfUnits.end()) {
      addToLineTable(RowReference, *It->second, Flags, *InstrLabel, CurrentRow);
      continue;
    }
    // This rows is from CU that did not contain the original function.
    // This might happen if BOLT moved/inlined that instruction from other CUs.
    // In this case, we need to insert it to all CUs that the function
    // originally beloned to.
    for (const auto &[_, Unit] : BF.getDWARFUnits()) {
      addToLineTable(RowReference, *Unit, Flags, *InstrLabel, CurrentRow);
    }
  }

  return NewLoc;
}

void BinaryEmitter::emitLineInfoEnd(const BinaryFunction &BF,
                                    MCSymbol *FunctionEndLabel,
                                    const DWARFUnit &Unit) {
  BC.Ctx->setCurrentDwarfLoc(0, 0, 0, DWARF2_FLAG_END_SEQUENCE, 0, 0);
  const MCDwarfLoc &DwarfLoc = BC.Ctx->getCurrentDwarfLoc();
  BC.Ctx->clearDwarfLocSeen();
  BC.getDwarfLineTable(Unit.getOffset())
      .getMCLineSections()
      .addLineEntry(MCDwarfLineEntry(FunctionEndLabel, DwarfLoc),
                    Streamer.getCurrentSectionOnly());
}

void BinaryEmitter::emitJumpTables(const BinaryFunction &BF) {
  MCSection *ReadOnlySection = BC.MOFI->getReadOnlySection();
  MCSection *ReadOnlyColdSection = BC.MOFI->getContext().getELFSection(
      ".rodata.cold", ELF::SHT_PROGBITS, ELF::SHF_ALLOC);

  if (!BF.hasJumpTables())
    return;

  if (opts::PrintJumpTables)
    BC.outs() << "BOLT-INFO: jump tables for function " << BF << ":\n";

  for (auto &JTI : BF.jumpTables()) {
    JumpTable &JT = *JTI.second;
    // Only emit shared jump tables once, when processing the first parent
    if (JT.Parents.size() > 1 && JT.Parents[0] != &BF)
      continue;
    if (opts::PrintJumpTables)
      JT.print(BC.outs());
    if (opts::JumpTables == JTS_BASIC) {
      JT.updateOriginal();
    } else {
      MCSection *HotSection, *ColdSection;
      if (BF.isSimple()) {
        HotSection = ReadOnlySection;
        ColdSection = ReadOnlyColdSection;
      } else {
        HotSection = BF.hasProfile() ? ReadOnlySection : ReadOnlyColdSection;
        ColdSection = HotSection;
      }
      emitJumpTable(JT, HotSection, ColdSection);
    }
  }
}

void BinaryEmitter::emitJumpTable(const JumpTable &JT, MCSection *HotSection,
                                  MCSection *ColdSection) {
  // Pre-process entries for aggressive splitting.
  // Each label represents a separate switch table and gets its own count
  // determining its destination.
  std::map<MCSymbol *, uint64_t> LabelCounts;
  if (opts::JumpTables > JTS_SPLIT && !JT.Counts.empty()) {
    auto It = JT.Labels.find(0);
    assert(It != JT.Labels.end());
    MCSymbol *CurrentLabel = It->second;
    uint64_t CurrentLabelCount = 0;
    for (unsigned Index = 0; Index < JT.Entries.size(); ++Index) {
      auto LI = JT.Labels.find(Index * JT.EntrySize);
      if (LI != JT.Labels.end()) {
        LabelCounts[CurrentLabel] = CurrentLabelCount;
        CurrentLabel = LI->second;
        CurrentLabelCount = 0;
      }
      CurrentLabelCount += JT.Counts[Index].Count;
    }
    LabelCounts[CurrentLabel] = CurrentLabelCount;
  } else {
    Streamer.switchSection(JT.Count > 0 ? HotSection : ColdSection);
    Streamer.emitValueToAlignment(Align(JT.EntrySize));
  }
  MCSymbol *JTLabel = nullptr;
  uint64_t Offset = 0;
  for (MCSymbol *Entry : JT.Entries) {
    auto LI = JT.Labels.find(Offset);
    if (LI == JT.Labels.end())
      goto emitEntry;
    JTLabel = LI->second;
    LLVM_DEBUG({
      dbgs() << "BOLT-DEBUG: emitting jump table " << JTLabel->getName()
             << " (originally was at address 0x"
             << Twine::utohexstr(JT.getAddress() + Offset)
             << (Offset ? ") as part of larger jump table\n" : ")\n");
    });
    if (!LabelCounts.empty()) {
      const uint64_t JTCount = LabelCounts[JTLabel];
      LLVM_DEBUG(dbgs() << "BOLT-DEBUG: jump table count: " << JTCount << '\n');
      Streamer.switchSection(JTCount ? HotSection : ColdSection);
      Streamer.emitValueToAlignment(Align(JT.EntrySize));
    }
    // Emit all labels registered at the address of this jump table
    // to sync with our global symbol table.  We may have two labels
    // registered at this address if one label was created via
    // getOrCreateGlobalSymbol() (e.g. LEA instructions referencing
    // this location) and another via getOrCreateJumpTable().  This
    // creates a race where the symbols created by these two
    // functions may or may not be the same, but they are both
    // registered in our symbol table at the same address. By
    // emitting them all here we make sure there is no ambiguity
    // that depends on the order that these symbols were created, so
    // whenever this address is referenced in the binary, it is
    // certain to point to the jump table identified at this
    // address.
    if (BinaryData *BD = BC.getBinaryDataByName(JTLabel->getName())) {
      for (MCSymbol *S : BD->getSymbols())
        Streamer.emitLabel(S);
    } else {
      Streamer.emitLabel(JTLabel);
    }
  emitEntry:
    if (JT.Type == JumpTable::JTT_NORMAL) {
      Streamer.emitSymbolValue(Entry, JT.OutputEntrySize);
    } else { // JTT_PIC
      const MCSymbolRefExpr *JTExpr =
          MCSymbolRefExpr::create(JTLabel, Streamer.getContext());
      const MCSymbolRefExpr *E =
          MCSymbolRefExpr::create(Entry, Streamer.getContext());
      const MCBinaryExpr *Value =
          MCBinaryExpr::createSub(E, JTExpr, Streamer.getContext());
      Streamer.emitValue(Value, JT.EntrySize);
    }
    Offset += JT.EntrySize;
  }
}

void BinaryEmitter::emitCFIInstruction(const MCCFIInstruction &Inst) const {
  switch (Inst.getOperation()) {
  default:
    llvm_unreachable("Unexpected instruction");
  case MCCFIInstruction::OpDefCfaOffset:
    Streamer.emitCFIDefCfaOffset(Inst.getOffset());
    break;
  case MCCFIInstruction::OpAdjustCfaOffset:
    Streamer.emitCFIAdjustCfaOffset(Inst.getOffset());
    break;
  case MCCFIInstruction::OpDefCfa:
    Streamer.emitCFIDefCfa(Inst.getRegister(), Inst.getOffset());
    break;
  case MCCFIInstruction::OpDefCfaRegister:
    Streamer.emitCFIDefCfaRegister(Inst.getRegister());
    break;
  case MCCFIInstruction::OpOffset:
    Streamer.emitCFIOffset(Inst.getRegister(), Inst.getOffset());
    break;
  case MCCFIInstruction::OpRegister:
    Streamer.emitCFIRegister(Inst.getRegister(), Inst.getRegister2());
    break;
  case MCCFIInstruction::OpWindowSave:
    Streamer.emitCFIWindowSave();
    break;
  case MCCFIInstruction::OpNegateRAState:
    Streamer.emitCFINegateRAState();
    break;
  case MCCFIInstruction::OpSameValue:
    Streamer.emitCFISameValue(Inst.getRegister());
    break;
  case MCCFIInstruction::OpGnuArgsSize:
    Streamer.emitCFIGnuArgsSize(Inst.getOffset());
    break;
  case MCCFIInstruction::OpEscape:
    Streamer.AddComment(Inst.getComment());
    Streamer.emitCFIEscape(Inst.getValues());
    break;
  case MCCFIInstruction::OpRestore:
    Streamer.emitCFIRestore(Inst.getRegister());
    break;
  case MCCFIInstruction::OpUndefined:
    Streamer.emitCFIUndefined(Inst.getRegister());
    break;
  }
}

// The code is based on EHStreamer::emitExceptionTable().
void BinaryEmitter::emitLSDA(BinaryFunction &BF, const FunctionFragment &FF) {
  const BinaryFunction::CallSitesRange Sites =
      BF.getCallSites(FF.getFragmentNum());
  if (Sites.empty())
    return;

  Streamer.switchSection(BC.MOFI->getLSDASection());

  const unsigned TTypeEncoding = BF.getLSDATypeEncoding();
  const unsigned TTypeEncodingSize = BC.getDWARFEncodingSize(TTypeEncoding);
  const uint16_t TTypeAlignment = 4;

  // Type tables have to be aligned at 4 bytes.
  Streamer.emitValueToAlignment(Align(TTypeAlignment));

  // Emit the LSDA label.
  MCSymbol *LSDASymbol = BF.getLSDASymbol(FF.getFragmentNum());
  assert(LSDASymbol && "no LSDA symbol set");
  Streamer.emitLabel(LSDASymbol);

  // Corresponding FDE start.
  const MCSymbol *StartSymbol = BF.getSymbol(FF.getFragmentNum());

  // Emit the LSDA header.

  // If LPStart is omitted, then the start of the FDE is used as a base for
  // landing pad displacements. Then, if a cold fragment starts with
  // a landing pad, this means that the first landing pad offset will be 0.
  // However, C++ runtime will treat 0 as if there is no landing pad, thus we
  // cannot emit LP offset as 0.
  //
  // As a solution, for fixed-address binaries we set LPStart to 0, and for
  // position-independent binaries we offset LP start by one byte.
  bool NeedsLPAdjustment = false;
  std::function<void(const MCSymbol *)> emitLandingPad;

  // Check if there's a symbol associated with a landing pad fragment.
  const MCSymbol *LPStartSymbol = BF.getLPStartSymbol(FF.getFragmentNum());
  if (!LPStartSymbol) {
    // Since landing pads are not in the same fragment, we fall back to emitting
    // absolute addresses for this FDE.
    if (opts::Verbosity >= 2) {
      BC.outs() << "BOLT-INFO: falling back to generating absolute-address "
                << "exception ranges for " << BF << '\n';
    }

    assert(BC.HasFixedLoadAddress &&
           "Cannot emit absolute-address landing pads for PIE/DSO");

    Streamer.emitIntValue(dwarf::DW_EH_PE_udata4, 1); // LPStart format
    Streamer.emitIntValue(0, 4);                      // LPStart
    emitLandingPad = [&](const MCSymbol *LPSymbol) {
      if (LPSymbol)
        Streamer.emitSymbolValue(LPSymbol, 4);
      else
        Streamer.emitIntValue(0, 4);
    };
  } else {
    std::optional<FragmentNum> LPFN = BF.getLPFragment(FF.getFragmentNum());
    const FunctionFragment &LPFragment = BF.getLayout().getFragment(*LPFN);
    NeedsLPAdjustment =
        (!LPFragment.empty() && LPFragment.front()->isLandingPad());

    // Emit LPStart encoding and optionally LPStart.
    if (NeedsLPAdjustment || LPStartSymbol != StartSymbol) {
      Streamer.emitIntValue(dwarf::DW_EH_PE_pcrel | dwarf::DW_EH_PE_sdata4, 1);
      MCSymbol *DotSymbol = BC.Ctx->createTempSymbol("LPBase");
      Streamer.emitLabel(DotSymbol);

      const MCExpr *LPStartExpr = MCBinaryExpr::createSub(
          MCSymbolRefExpr::create(LPStartSymbol, *BC.Ctx),
          MCSymbolRefExpr::create(DotSymbol, *BC.Ctx), *BC.Ctx);
      if (NeedsLPAdjustment)
        LPStartExpr = MCBinaryExpr::createSub(
            LPStartExpr, MCConstantExpr::create(1, *BC.Ctx), *BC.Ctx);
      Streamer.emitValue(LPStartExpr, 4);
    } else {
      // DW_EH_PE_omit means FDE start (StartSymbol) will be used as LPStart.
      Streamer.emitIntValue(dwarf::DW_EH_PE_omit, 1);
    }
    emitLandingPad = [&](const MCSymbol *LPSymbol) {
      if (LPSymbol) {
        const MCExpr *LPOffsetExpr = MCBinaryExpr::createSub(
            MCSymbolRefExpr::create(LPSymbol, *BC.Ctx),
            MCSymbolRefExpr::create(LPStartSymbol, *BC.Ctx), *BC.Ctx);
        if (NeedsLPAdjustment)
          LPOffsetExpr = MCBinaryExpr::createAdd(
              LPOffsetExpr, MCConstantExpr::create(1, *BC.Ctx), *BC.Ctx);
        Streamer.emitULEB128Value(LPOffsetExpr);
      } else {
        Streamer.emitULEB128IntValue(0);
      }
    };
  }

  Streamer.emitIntValue(TTypeEncoding, 1); // TType format

  MCSymbol *TTBaseLabel = nullptr;
  if (TTypeEncoding != dwarf::DW_EH_PE_omit) {
    TTBaseLabel = BC.Ctx->createTempSymbol("TTBase");
    MCSymbol *TTBaseRefLabel = BC.Ctx->createTempSymbol("TTBaseRef");
    Streamer.emitAbsoluteSymbolDiffAsULEB128(TTBaseLabel, TTBaseRefLabel);
    Streamer.emitLabel(TTBaseRefLabel);
  }

  // Emit encoding of entries in the call site table. The format is used for the
  // call site start, length, and corresponding landing pad.
  if (!LPStartSymbol)
    Streamer.emitIntValue(dwarf::DW_EH_PE_udata4, 1);
  else
    Streamer.emitIntValue(dwarf::DW_EH_PE_uleb128, 1);

  MCSymbol *CSTStartLabel = BC.Ctx->createTempSymbol("CSTStart");
  MCSymbol *CSTEndLabel = BC.Ctx->createTempSymbol("CSTEnd");
  Streamer.emitAbsoluteSymbolDiffAsULEB128(CSTEndLabel, CSTStartLabel);

  Streamer.emitLabel(CSTStartLabel);
  for (const auto &FragmentCallSite : Sites) {
    const BinaryFunction::CallSite &CallSite = FragmentCallSite.second;
    const MCSymbol *BeginLabel = CallSite.Start;
    const MCSymbol *EndLabel = CallSite.End;

    assert(BeginLabel && "start EH label expected");
    assert(EndLabel && "end EH label expected");

    // Start of the range is emitted relative to the start of current
    // function split part.
    if (!LPStartSymbol) {
      Streamer.emitAbsoluteSymbolDiff(BeginLabel, StartSymbol, 4);
      Streamer.emitAbsoluteSymbolDiff(EndLabel, BeginLabel, 4);
    } else {
      Streamer.emitAbsoluteSymbolDiffAsULEB128(BeginLabel, StartSymbol);
      Streamer.emitAbsoluteSymbolDiffAsULEB128(EndLabel, BeginLabel);
    }
    emitLandingPad(CallSite.LP);
    Streamer.emitULEB128IntValue(CallSite.Action);
  }
  Streamer.emitLabel(CSTEndLabel);

  // Write out action, type, and type index tables at the end.
  //
  // For action and type index tables there's no need to change the original
  // table format unless we are doing function splitting, in which case we can
  // split and optimize the tables.
  //
  // For type table we (re-)encode the table using TTypeEncoding matching
  // the current assembler mode.
  for (uint8_t const &Byte : BF.getLSDAActionTable())
    Streamer.emitIntValue(Byte, 1);

  const BinaryFunction::LSDATypeTableTy &TypeTable =
      (TTypeEncoding & dwarf::DW_EH_PE_indirect) ? BF.getLSDATypeAddressTable()
                                                 : BF.getLSDATypeTable();
  assert(TypeTable.size() == BF.getLSDATypeTable().size() &&
         "indirect type table size mismatch");

  Streamer.emitValueToAlignment(Align(TTypeAlignment));

  for (int Index = TypeTable.size() - 1; Index >= 0; --Index) {
    const uint64_t TypeAddress = TypeTable[Index];
    switch (TTypeEncoding & 0x70) {
    default:
      llvm_unreachable("unsupported TTypeEncoding");
    case dwarf::DW_EH_PE_absptr:
      Streamer.emitIntValue(TypeAddress, TTypeEncodingSize);
      break;
    case dwarf::DW_EH_PE_pcrel: {
      if (TypeAddress) {
        const MCSymbol *TypeSymbol =
            BC.getOrCreateGlobalSymbol(TypeAddress, "TI", 0, TTypeAlignment);
        MCSymbol *DotSymbol = BC.Ctx->createNamedTempSymbol();
        Streamer.emitLabel(DotSymbol);
        const MCBinaryExpr *SubDotExpr = MCBinaryExpr::createSub(
            MCSymbolRefExpr::create(TypeSymbol, *BC.Ctx),
            MCSymbolRefExpr::create(DotSymbol, *BC.Ctx), *BC.Ctx);
        Streamer.emitValue(SubDotExpr, TTypeEncodingSize);
      } else {
        Streamer.emitIntValue(0, TTypeEncodingSize);
      }
      break;
    }
    }
  }

  if (TTypeEncoding != dwarf::DW_EH_PE_omit)
    Streamer.emitLabel(TTBaseLabel);

  for (uint8_t const &Byte : BF.getLSDATypeIndexTable())
    Streamer.emitIntValue(Byte, 1);
}

void BinaryEmitter::emitDebugLineInfoForOriginalFunctions() {
  // If a function is in a CU containing at least one processed function, we
  // have to rewrite the whole line table for that CU. For unprocessed functions
  // we use data from the input line table.
  for (auto &It : BC.getBinaryFunctions()) {
    const BinaryFunction &Function = It.second;

    // If the function was emitted, its line info was emitted with it.
    if (Function.isEmitted())
      continue;

    // Loop through all CUs in the function
    for (const auto &[_, Unit] : Function.getDWARFUnits()) {
      const DWARFDebugLine::LineTable *LineTable =
          Function.getDWARFLineTableForUnit(Unit);
      if (!LineTable)
        continue; // nothing to update for this unit

      const uint64_t Address = Function.getAddress();
      std::vector<uint32_t> Results;
      if (!LineTable->lookupAddressRange(
              {Address, object::SectionedAddress::UndefSection},
              Function.getSize(), Results))
        continue;

      if (Results.empty())
        continue;

      // The first row returned could be the last row matching the start
      // address. Find the first row with the same address that is not the end
      // of the sequence.
      uint64_t FirstRow = Results.front();
      while (FirstRow > 0) {
        const DWARFDebugLine::Row &PrevRow = LineTable->Rows[FirstRow - 1];
        if (PrevRow.Address.Address != Address || PrevRow.EndSequence)
          break;
        --FirstRow;
      }

      const uint64_t EndOfSequenceAddress =
          Function.getAddress() + Function.getMaxSize();
      BC.getDwarfLineTable(Unit->getOffset())
          .addLineTableSequence(LineTable, FirstRow, Results.back(),
                                EndOfSequenceAddress);
    }
  }

  // For units that are completely unprocessed, use original debug line contents
  // eliminating the need to regenerate line info program.
  emitDebugLineInfoForUnprocessedCUs();
}

void BinaryEmitter::emitDebugLineInfoForUnprocessedCUs() {
  // Sorted list of section offsets provides boundaries for section fragments,
  // where each fragment is the unit's contribution to debug line section.
  std::vector<uint64_t> StmtListOffsets;
  StmtListOffsets.reserve(BC.DwCtx->getNumCompileUnits());
  for (const std::unique_ptr<DWARFUnit> &CU : BC.DwCtx->compile_units()) {
    DWARFDie CUDie = CU->getUnitDIE();
    auto StmtList = dwarf::toSectionOffset(CUDie.find(dwarf::DW_AT_stmt_list));
    if (!StmtList)
      continue;

    StmtListOffsets.push_back(*StmtList);
  }
  llvm::sort(StmtListOffsets);

  // For each CU that was not processed, emit its line info as a binary blob.
  for (const std::unique_ptr<DWARFUnit> &CU : BC.DwCtx->compile_units()) {
    if (BC.ProcessedCUs.count(CU.get()))
      continue;

    DWARFDie CUDie = CU->getUnitDIE();
    auto StmtList = dwarf::toSectionOffset(CUDie.find(dwarf::DW_AT_stmt_list));
    if (!StmtList)
      continue;

    StringRef DebugLineContents = CU->getLineSection().Data;

    const uint64_t Begin = *StmtList;

    // Statement list ends where the next unit contribution begins, or at the
    // end of the section.
    auto It = llvm::upper_bound(StmtListOffsets, Begin);
    const uint64_t End =
        It == StmtListOffsets.end() ? DebugLineContents.size() : *It;

    BC.getDwarfLineTable(CU->getOffset())
        .addRawContents(DebugLineContents.slice(Begin, End));
  }
}

void BinaryEmitter::emitDataSections(StringRef OrgSecPrefix) {
  for (BinarySection &Section : BC.sections()) {
    if (!Section.hasRelocations())
      continue;

    StringRef Prefix = Section.hasSectionRef() ? OrgSecPrefix : "";
    Section.emitAsData(Streamer, Prefix + Section.getName());
    Section.clearRelocations();
  }
}

namespace llvm {
namespace bolt {

void emitBinaryContext(MCStreamer &Streamer, BinaryContext &BC,
                       StringRef OrgSecPrefix) {
  BinaryEmitter(Streamer, BC).emitAll(OrgSecPrefix);
}

void emitBinaryContext(MCStreamer &Streamer, BinaryContext &BC,
                       StringRef OrgSecPrefix, bool RelaxBranches) {
  BinaryEmitter(Streamer, BC, RelaxBranches && useFragmentSections(BC))
      .emitAll(OrgSecPrefix);
}

void emitFunctionBody(MCStreamer &Streamer, BinaryFunction &BF,
                      FunctionFragment &FF, bool EmitCodeOnly) {
  BinaryEmitter(Streamer, BF.getBinaryContext())
      .emitFunctionBody(BF, FF, EmitCodeOnly);
}

} // namespace bolt
} // namespace llvm
