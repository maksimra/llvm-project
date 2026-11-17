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
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
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

static cl::list<std::string> BreakFunctionNames(
    "break-funcs", cl::CommaSeparated,
    cl::desc("list of functions to core dump on (debugging)"),
    cl::value_desc("func1,func2,func3,..."), cl::Hidden, cl::cat(BoltCategory));

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

/// Plans contain only references to immutable, lowered instructions. In
/// particular, querying opcode properties, targets, annotations and the
/// surviving successor counts does not allocate in MCContext or MCPlusBuilder.
/// Do not use CFG analyses that interpret lowered tail calls as ordinary edges.
class LocalBranchRelaxation {
  BinaryContext &BC;
  using Destination = std::pair<const MCSymbol *, int64_t>;
  enum Kind { Direct, Reuse, Outline, Swap, Inline };
  static constexpr unsigned Invalid = ~0U;

  struct Block {
    uint64_t Size = 0;
    uint64_t Growth = 0;
    uint64_t Offset = 0;
    Align Alignment = Align(1);
    unsigned MaxPadding = 0;
  };
  struct Site {
    const MCInst *Inst;
    unsigned BlockIndex;
    unsigned CFIEpoch;
    uint64_t InBlock;
    uint64_t Offset = 0;
    unsigned CandidateIndex = Invalid;
    uint64_t ArgsSize = 0;
  };
  struct Candidate {
    unsigned SiteIndex;
    unsigned TargetBlock;
    Destination Target;
    unsigned Bits;
    bool Reversible;
    bool Cold;
    bool ProvenDirect = false;
    Kind Form = Direct;
    unsigned Via = Invalid;
    unsigned Pair = Invalid;
  };
  struct Trampoline {
    unsigned Source;
    uint64_t Offset = 0;
    bool Active = false;
  };
  struct Action {
    Kind Form;
    const MCInst *TargetInst = nullptr;
    MCSymbol *Label = nullptr;
  };
  struct EmittedTrampoline {
    const MCInst *Source;
    uint64_t ArgsSize;
  };
  DenseMap<const MCInst *, Action> Actions;
  // Ordered by source position, independently of task/relaxation order.
  SmallVector<const MCInst *, 0> ReusedBranches;
  SmallVector<EmittedTrampoline, 0> Trampolines;
  DenseMap<const MCInst *, MCSymbol *> Labels;
  DenseMap<const MCInst *, MCSymbol *> ReusedLabels;
  DenseMap<const MCInst *, unsigned> SavedCFI;
  const MCInst *ErrorInst = nullptr;
  const char *ErrorMessage = nullptr;
  bool NeedsFDE = false;
  bool NeedsArgsSize = false;
  bool NeedsGuard = false;
  MCSymbol *TrampolinesEnd = nullptr;

  Destination destination(const MCInst &Inst) const {
    const MCOperand &Op =
        Inst.getOperand(MCPlus::getNumPrimeOperands(Inst) - 1);
    if (!Op.isExpr())
      return {nullptr, 0};
    const MCExpr *Expr = Op.getExpr();
    uint64_t Addend = 0;
    // Decode symbol +/- constants without MCExpr evaluation: evaluating a
    // variable symbol can update MC state and is not safe in these workers.
    // The MIB addend helper only recognizes a single RHS of an addition.
    while (const auto *Binary = dyn_cast<MCBinaryExpr>(Expr)) {
      const auto *Constant = dyn_cast<MCConstantExpr>(Binary->getRHS());
      if (!Constant || (Binary->getOpcode() != MCBinaryExpr::Add &&
                        Binary->getOpcode() != MCBinaryExpr::Sub))
        return {nullptr, 0};
      if (Binary->getOpcode() == MCBinaryExpr::Add)
        Addend += uint64_t(Constant->getValue());
      else
        Addend -= uint64_t(Constant->getValue());
      Expr = Binary->getLHS();
    }
    const auto *Symbol = dyn_cast<MCSymbolRefExpr>(Expr);
    if (!Symbol || Symbol->getSpecifier())
      return {nullptr, 0};
    return {&Symbol->getSymbol(), int64_t(Addend)};
  }

  static bool inRange(unsigned Bits, uint64_t From, uint64_t To,
                      int64_t Addend = 0) {
    // AArch64 branch displacements are signed byte offsets, scaled by four.
    const uint64_t Offset = To + uint64_t(Addend) - From;
    return !(Offset & 3) && isIntN(Bits, Offset);
  }

  MCInst createJump(const MCInst &Inst) const {
    MCInst Jump;
    BC.MIB->createUncondBranch(Jump, BC.MIB->getTargetSymbol(Inst),
                               BC.Ctx.get());
    Jump.getOperand(0) = Inst.getOperand(MCPlus::getNumPrimeOperands(Inst) - 1);
    return Jump;
  }

public:
  explicit LocalBranchRelaxation(BinaryContext &BC) : BC(BC) {}

  void run(const BinaryFunction &BF, const FunctionFragment &FF,
           uint64_t BodyOffset) {
    const bool PreserveLayout = !BF.isSimple() || BF.isPatch();
    NeedsFDE = BF.hasCFI() && !(BF.isPatch() && BF.isAnonymous());
    NeedsArgsSize = BF.usesGnuArgsSize();
    const bool MayFallThrough = fragmentMayFallThrough(FF, BC);
    SmallVector<Block, 0> Blocks;
    SmallVector<Site, 0> Sites;
    SmallVector<Candidate, 0> Candidates;
    SmallVector<unsigned, 0> Jumps;
    SmallVector<Trampoline, 0> Thunks;
    DenseMap<const BinaryBasicBlock *, unsigned> BlockIndices;
    unsigned CFIEpoch = 0;
    bool HasCFIStack = false;
    uint64_t ArgsSize = 0;
    uint64_t MaximumSize = 0;

    // One instruction walk: collect byte sizes, alignment constraints, branch
    // candidates and their original positions. Subsequent iterations visit
    // only blocks and branch sites, never all the instructions again.
    for (const BinaryBasicBlock *BB : FF) {
      const unsigned BI = Blocks.size();
      BlockIndices[BB] = BI;
      Blocks.emplace_back();
      Block &B = Blocks.back();
      if (BC.AlignBlocks || BC.PreserveBlocksAlignment) {
        B.Alignment = BB->getAlign();
        B.MaxPadding = BB->getAlignmentMaxBytes();
      }
      unsigned Previous = Invalid;
      for (const MCInst &Inst : *BB) {
        if (BC.MIB->isCFI(Inst)) {
          ++CFIEpoch;
          const auto Op = BF.getCFIFor(Inst)->getOperation();
          HasCFIStack |= Op == MCCFIInstruction::OpRememberState ||
                         Op == MCCFIInstruction::OpRestoreState ||
                         Op == MCCFIInstruction::OpEscape;
          if (Op == MCCFIInstruction::OpGnuArgsSize)
            ArgsSize = BF.getCFIFor(Inst)->getOffset();
        }
        if (BC.MIB->isPseudo(Inst))
          continue;
        if (BC.MIB->isConditionalBranch(Inst)) {
          const unsigned SI = Sites.size();
          Sites.push_back(
              {&Inst, BI, CFIEpoch, B.Size, 0, unsigned(Candidates.size())});
          Sites.back().ArgsSize = ArgsSize;
          const Destination Target = destination(Inst);
          const BinaryBasicBlock *TargetBB =
              BF.getBasicBlockForLabel(Target.first);
          bool Cold = false;
          // Lowering preserves successor counts, but removes conditional
          // tail-call annotations. Only trust a count for an actual matching
          // CFG edge, with no addend into the middle of a block.
          if (!Target.second && TargetBB && BF.hasValidProfile() &&
              BB->hasProfile() && BB->getSuccessor(Target.first) == TargetBB)
            Cold = BB->getBranchInfo(*TargetBB).Count == 0;
          Candidates.push_back({SI, Invalid, Target,
                                unsigned(BC.MIB->getPCRelEncodingSize(Inst)),
                                BC.MIB->isReversibleBranch(Inst), Cold});
          // Filled after all blocks have been indexed.
          if (PreserveLayout && TargetBB && !Target.second)
            Candidates.back().ProvenDirect = true;
          Previous = SI;
          MaximumSize += Candidates.back().Reversible ? 4 : 8;
        } else if (BC.MII->get(Inst.getOpcode()).isUnconditionalBranch() &&
                   !BC.MIB->isIndirectBranch(Inst)) {
          const unsigned SI = Sites.size();
          Sites.push_back({&Inst, BI, CFIEpoch, B.Size});
          Jumps.push_back(SI);
          if (Previous != Invalid && Sites[Previous].CFIEpoch == CFIEpoch)
            Candidates[Sites[Previous].CandidateIndex].Pair = SI;
          Previous = Invalid;
        } else {
          Previous = Invalid;
        }
        B.Size += 4;
      }
      MaximumSize +=
          B.Size + (B.MaxPadding ? std::min<uint64_t>(B.Alignment.value() - 1,
                                                      B.MaxPadding)
                                 : B.Alignment.value() - 1);
    }
    if (Candidates.empty())
      return;

    for (Candidate &C : Candidates) {
      const auto *BB = BF.getBasicBlockForLabel(C.Target.first);
      auto It = BlockIndices.find(BB);
      if (It != BlockIndices.end())
        C.TargetBlock = It->second;
      // Upper bound includes every possible inline expansion and maximum BB
      // padding (including unlimited budgets). Cross-fragment targets and
      // addends cannot use this fast path. Use each opcode's own range.
      C.ProvenDirect |= C.TargetBlock != Invalid && !C.Target.second &&
                        MaximumSize < (uint64_t(1) << (C.Bits - 1));
    }
    if (llvm::all_of(Candidates,
                     [](const Candidate &C) { return C.ProvenDirect; }))
      return;

    auto extraSize = [&](const Candidate &C) -> unsigned {
      return C.Form == Inline ? (C.Reversible ? 4 : 8) : 0;
    };
    SmallVector<unsigned, 0> ThunkOrder;
    uint64_t CodeSize = 0;
    auto layout = [&]() {
      CodeSize = BodyOffset;
      for (Block &B : Blocks) {
        const uint64_t Pad = offsetToAlignment(CodeSize, B.Alignment);
        if (!B.MaxPadding || Pad <= B.MaxPadding)
          CodeSize += Pad;
        B.Offset = CodeSize;
        CodeSize += B.Size + B.Growth;
      }
      unsigned BI = Invalid;
      uint64_t Growth = 0;
      for (Site &S : Sites) {
        if (BI != S.BlockIndex) {
          BI = S.BlockIndex;
          Growth = 0;
        }
        S.Offset = Blocks[BI].Offset + S.InBlock + Growth;
        if (S.CandidateIndex != Invalid)
          Growth += extraSize(Candidates[S.CandidateIndex]);
      }
      for (Trampoline &T : Thunks)
        T.Active = false;
      for (const Candidate &C : Candidates)
        if (C.Form == Outline)
          Thunks[C.Via].Active = true;
      ThunkOrder.clear();
      for (unsigned I = 0; I < Thunks.size(); ++I)
        if (Thunks[I].Active)
          ThunkOrder.push_back(I);
      llvm::sort(ThunkOrder, [&](unsigned L, unsigned R) {
        return Thunks[L].Source > Thunks[R].Source;
      });
      uint64_t End = CodeSize + (!ThunkOrder.empty() && MayFallThrough ? 4 : 0);
      for (unsigned I : ThunkOrder) {
        Thunks[I].Offset = End;
        End += 4;
      }
    };
    auto jumpDestination = [&](unsigned SI) {
      // Swapping a conditional/B pair changes the B's destination. Users of
      // that B must be revalidated on the next iteration too.
      if (SI && Sites[SI - 1].CandidateIndex != Invalid) {
        const Candidate &C = Candidates[Sites[SI - 1].CandidateIndex];
        if (C.Form == Swap && C.Pair == SI)
          return C.Target;
      }
      return destination(*Sites[SI].Inst);
    };
    bool Changed;
    do {
      layout();
      Changed = false;
      using ReuseKey = std::pair<Destination, unsigned>;
      DenseMap<ReuseKey, SmallVector<unsigned, 1>> Reusable;
      for (unsigned SI : Jumps) {
        const Destination Target = jumpDestination(SI);
        if (Target.first)
          Reusable[{Target, Sites[SI].CFIEpoch}].push_back(SI);
      }
      DenseMap<ReuseKey, SmallVector<unsigned, 1>> Outlined;
      for (unsigned I : ThunkOrder) {
        const Site &Owner = Sites[Thunks[I].Source];
        Outlined[{Candidates[Owner.CandidateIndex].Target, Owner.CFIEpoch}]
            .push_back(I);
      }
      // New sources are visited in increasing order. Keep their ranks apart
      // from the existing reverse-source order, avoiding quadratic insertion
      // and offset updates when many cold edges acquire end trampolines.
      SmallVector<unsigned, 0> NewThunkSources;
      auto thunkOffset = [&](unsigned I) {
        const auto After = llvm::upper_bound(NewThunkSources, Thunks[I].Source);
        return Thunks[I].Offset + 4 * (NewThunkSources.end() - After);
      };

      for (Candidate &C : Candidates) {
        if (C.ProvenDirect || C.Form == Inline)
          continue;
        const Site &S = Sites[C.SiteIndex];
        auto reaches = [&](uint64_t To, int64_t Addend = 0) {
          return inRange(C.Bits, S.Offset, To, Addend);
        };
        if (C.Form == Direct && C.TargetBlock != Invalid &&
            reaches(Blocks[C.TargetBlock].Offset, C.Target.second))
          continue;
        if (C.Form == Reuse && jumpDestination(C.Via) == C.Target &&
            reaches(Sites[C.Via].Offset))
          continue;
        if (C.Form == Outline && reaches(thunkOffset(C.Via)))
          continue;
        if (C.Form == Swap) {
          const Destination Near = destination(*Sites[C.Pair].Inst);
          auto It = BlockIndices.find(BF.getBasicBlockForLabel(Near.first));
          if (It != BlockIndices.end() &&
              reaches(Blocks[It->second].Offset, Near.second))
            continue;
        }

        // Like LongJmp, first reuse a reachable B with the exact destination.
        // Reusing an instruction does not move its labels or its CFIs. A CFI
        // epoch match conservatively proves identical unwind rules, including
        // signed RA state, without interpreting arbitrary DW_CFA_expression.
        if (C.Form == Direct) {
          auto UseJump = [&](unsigned SI) {
            if (jumpDestination(SI) != C.Target)
              return false;
            C.Form = Reuse;
            C.Via = SI;
            Changed = true;
            return true;
          };
          auto ReusableIt = Reusable.find({C.Target, S.CFIEpoch});
          if (ReusableIt != Reusable.end()) {
            const auto &Group = ReusableIt->second;
            auto Mid = llvm::lower_bound(Group, S.Offset,
                                         [&](unsigned SI, uint64_t Offset) {
                                           return Sites[SI].Offset < Offset;
                                         });
            for (auto I = Mid; I != Group.end() && reaches(Sites[*I].Offset);
                 ++I)
              if (UseJump(*I))
                break;
            if (C.Form != Reuse)
              for (auto I = std::make_reverse_iterator(Mid);
                   I != Group.rend() && reaches(Sites[*I].Offset); ++I)
                if (UseJump(*I))
                  break;
          }
          if (C.Form == Reuse)
            continue;

          // Preserve the original hot fall-through for proven zero-count
          // taken edges. Non-simple functions may only grow at their end.
          // Patches have a fixed footprint: no append-only trampoline space.
          // A fall-through continuation after the thunk area still needs the
          // fragment-end CFI row. Without a row interpreter, only outline in
          // that case when the source and end epochs already agree.
          if (!BF.isPatch() && !HasCFIStack && (C.Cold || PreserveLayout) &&
              (!MayFallThrough || !NeedsFDE || CFIEpoch == S.CFIEpoch)) {
            auto &Group = Outlined[{C.Target, S.CFIEpoch}];
            for (unsigned I : Group) {
              if (!C.Target.first)
                break;
              if (reaches(thunkOffset(I))) {
                C.Form = Outline;
                C.Via = I;
                break;
              }
            }
            const auto Insertion = llvm::lower_bound(
                ThunkOrder, C.SiteIndex, [&](unsigned I, unsigned Source) {
                  return Thunks[I].Source > Source;
                });
            const uint64_t Offset = CodeSize + (MayFallThrough ? 4 : 0) +
                                    4 * (Insertion - ThunkOrder.begin());
            if (C.Form != Outline && reaches(Offset)) {
              C.Form = Outline;
              C.Via = Thunks.size();
              Thunks.push_back({C.SiteIndex, Offset, true});
              NewThunkSources.push_back(C.SiteIndex);
              Group.push_back(C.Via);
              // Recompute exact reverse-source ordering before validating.
            }
            if (C.Form == Outline) {
              Changed = true;
              continue;
            }
          }

          // B.cond far; B near -> B.!cond near; B far. Both original PCs and
          // all instruction-address/CFI labels stay in place. Unlike ordinary
          // inline expansion this does not add a second B on the not-taken
          // path.
          if (!PreserveLayout && C.Reversible && C.Pair != Invalid) {
            const Destination Near = destination(*Sites[C.Pair].Inst);
            auto It = BlockIndices.find(BF.getBasicBlockForLabel(Near.first));
            if (It != BlockIndices.end() &&
                reaches(Blocks[It->second].Offset, Near.second)) {
              C.Form = Swap;
              Changed = true;
              continue;
            }
          }
        }
        if (PreserveLayout) {
          ErrorInst = S.Inst;
          ErrorMessage =
              BF.isPatch()
                  ? "fixed-size patch cannot grow for local branch relaxation"
              : HasCFIStack ||
                      (MayFallThrough && NeedsFDE && CFIEpoch != S.CFIEpoch)
                  ? "cannot preserve unwind state for end-of-function "
                    "trampoline"
                  : "end-of-function trampoline is out of reach";
          return;
        }
        // Monotonic fallback: every failed reuse/outline/swap becomes inline.
        C.Form = Inline;
        Blocks[S.BlockIndex].Growth += extraSize(C);
        Changed = true;
      }
    } while (Changed);

    for (const Candidate &C : Candidates) {
      const MCInst *Inst = Sites[C.SiteIndex].Inst;
      switch (C.Form) {
      case Direct:
        break;
      case Inline:
        Actions[Inst] = {Inline};
        break;
      case Swap:
        Actions[Inst] = {Swap, Sites[C.Pair].Inst};
        Actions[Sites[C.Pair].Inst] = {Direct, Inst};
        break;
      case Reuse:
        ReusedBranches.push_back(Sites[C.Via].Inst);
        Actions[Inst] = {Reuse, Sites[C.Via].Inst};
        break;
      case Outline:
        Actions[Inst] = {Outline, Sites[Thunks[C.Via].Source].Inst};
        break;
      }
    }
    // Source order is also the order of CFI remember_state pushes. Emit
    // trampolines in reverse order so restore_state pops the right snapshot.
    for (unsigned I : llvm::reverse(ThunkOrder))
      Trampolines.push_back(
          {Sites[Thunks[I].Source].Inst, Sites[Thunks[I].Source].ArgsSize});
    NeedsGuard = !Trampolines.empty() && MayFallThrough;
  }

  bool prepareForEmission(const BinaryFunction &BF) {
    if (ErrorInst) {
      BC.Ctx->reportError(ErrorInst->getLoc(),
                          Twine("cannot relax branch in function ") +
                              BF.getPrintName() + ": " + ErrorMessage);
      return false;
    }
    for (const MCInst *Inst : ReusedBranches)
      if (!Labels.count(Inst)) {
        Labels[Inst] = BC.Ctx->createTempSymbol("local_reused_branch");
        ReusedLabels[Inst] = Labels.lookup(Inst);
      }
    for (const EmittedTrampoline &T : Trampolines) {
      Labels[T.Source] = BC.Ctx->createTempSymbol("local_trampoline");
      if (NeedsFDE)
        SavedCFI[T.Source] = 1;
    }
    for (auto &[Inst, A] : Actions)
      if (A.Form == Reuse || A.Form == Outline)
        A.Label = Labels.lookup(A.TargetInst);
    if (NeedsGuard)
      TrampolinesEnd = BC.Ctx->createTempSymbol("local_trampolines_end");
    return true;
  }

  void emitBefore(const MCInst &Inst, MCStreamer &Streamer) const {
    // An outlined branch's snapshot is CFI only, with no code-size effect.
    if (SavedCFI.count(&Inst))
      Streamer.emitCFIRememberState(Inst.getLoc());
    // Only reused B sites are instruction labels. An outlined source's label
    // belongs at the end-of-fragment trampoline instead.
    if (MCSymbol *Label = ReusedLabels.lookup(&Inst))
      Streamer.emitLabel(Label);
  }

  bool emit(const MCInst &Inst, MCStreamer &Streamer) const {
    auto It = Actions.find(&Inst);
    if (It == Actions.end())
      return false;
    const Action &A = It->second;
    if (A.Form == Direct) {
      Streamer.emitInstruction(createJump(*A.TargetInst), *BC.STI);
      return true;
    }
    if (A.Form == Swap) {
      const MCInst &Near = *A.TargetInst;
      for (MCInst Branch : BC.MIB->reverseBranchCondition(
               Inst, BC.MIB->getTargetSymbol(Near), BC.Ctx.get())) {
        Branch.getOperand(MCPlus::getNumPrimeOperands(Branch) - 1) =
            Near.getOperand(0);
        Streamer.emitInstruction(Branch, *BC.STI);
      }
      return true;
    }
    if (A.Form == Reuse || A.Form == Outline) {
      MCInst Branch = Inst;
      BC.MIB->replaceBranchTarget(Branch, A.Label, BC.Ctx.get());
      Streamer.emitInstruction(Branch, *BC.STI);
      return true;
    }

    MCSymbol *End = BC.Ctx->createTempSymbol("local_branch_end");
    if (BC.MIB->isReversibleBranch(Inst)) {
      // Preserve flags: liveness analysis is no longer valid after lowering.
      for (const MCInst &Branch :
           BC.MIB->reverseBranchCondition(Inst, End, BC.Ctx.get()))
        Streamer.emitInstruction(Branch, *BC.STI);
    } else {
      MCSymbol *Taken = BC.Ctx->createTempSymbol("local_branch_taken");
      MCInst Branch = Inst;
      BC.MIB->replaceBranchTarget(Branch, Taken, BC.Ctx.get());
      Streamer.emitInstruction(Branch, *BC.STI);
      BC.MIB->createUncondBranch(Branch, End, BC.Ctx.get());
      Streamer.emitInstruction(Branch, *BC.STI);
      Streamer.emitLabel(Taken);
    }
    Streamer.emitInstruction(createJump(Inst), *BC.STI);
    Streamer.emitLabel(End);
    return true;
  }

  void emitTrampolines(MCStreamer &Streamer) const {
    if (TrampolinesEnd) {
      MCInst Jump;
      BC.MIB->createUncondBranch(Jump, TrampolinesEnd, BC.Ctx.get());
      Streamer.emitInstruction(Jump, *BC.STI);
    }
    for (const EmittedTrampoline &T : llvm::reverse(Trampolines)) {
      if (NeedsFDE)
        Streamer.emitCFIRestoreState(T.Source->getLoc());
      // GNU_args_size is not part of the register row saved by
      // DW_CFA_remember_state.
      if (NeedsFDE && NeedsArgsSize)
        Streamer.emitCFIGnuArgsSize(T.ArgsSize);
      Streamer.emitLabel(Labels.lookup(T.Source));
      Streamer.emitInstruction(createJump(*T.Source), *BC.STI);
    }
    if (TrampolinesEnd)
      Streamer.emitLabel(TrampolinesEnd);
  }
};

bool shouldEmitFunction(const BinaryFunction &BF, const BinaryContext &BC) {
  return BC.shouldEmit(BF) && (BF.size() || BF.hasIslandsInfo()) &&
         BF.getState() != BinaryFunction::State::Empty &&
         (BC.HasRelocations || BF.hasNonPseudoInstructions());
}

bool breakFunction(const BinaryFunction &BF) {
  return llvm::any_of(opts::BreakFunctionNames, [&](const std::string &Name) {
    return BF.hasNameRegex(Name);
  });
}

} // namespace

struct BinaryEmissionPlan::Impl {
  struct Fragment {
    BinaryFunction *BF;
    FunctionFragment *FF;
    uint64_t PrefixSize;
    bool Break;
    std::unique_ptr<LocalBranchRelaxation> Relaxation;
  };
  SmallVector<Fragment, 0> Fragments;
  size_t NextFragment = 0;
};

BinaryEmissionPlan::BinaryEmissionPlan(BinaryContext &BC) {
  if (!useFragmentSections(BC) || !BC.HasRelocations)
    return;
  State = std::make_unique<Impl>();
  uint64_t TotalCost = 0;
  SmallVector<uint64_t, 0> Costs;
  // Use the emitter's output order and predicates. OutputFunctions already
  // includes injected functions, unlike runOnEachFunction's input-address map.
  for (BinaryFunction *BF : BC.getOutputBinaryFunctions()) {
    if (!shouldEmitFunction(*BF, BC))
      continue;
    // This serial preparation creates MC symbols and rewrites island operands.
    // Finish it before workers read the final instructions of any fragment.
    if (BF->isSplit() && BF->hasConstantIsland()) {
      assert(BF->getLayout().isHotColdSplit() &&
             "Constant island support only with hot/cold split");
      BF->duplicateConstantIslands();
    }
    const uint64_t Prefix = opts::padFunctionBefore(*BF);
    const bool Break = breakFunction(*BF);
    for (FunctionFragment &FF : BF->getLayout().fragments()) {
      if (FF.isSplitFragment() && FF.empty() && !BF->hasConstantIsland())
        continue;
      State->Fragments.push_back({BF, &FF, Prefix, Break,
                                  std::make_unique<LocalBranchRelaxation>(BC)});
      uint64_t Cost = 1;
      for (const BinaryBasicBlock *BB : FF)
        Cost += BB->size();
      Costs.push_back(Cost);
      TotalCost += Cost;
    }
  }
  auto Work = [&](size_t Begin, size_t End) {
    for (size_t I = Begin; I != End; ++I) {
      Impl::Fragment &F = State->Fragments[I];
      F.Relaxation->run(*F.BF, *F.FF, F.PrefixSize + (F.Break ? 2 : 0));
    }
  };
  // Bound scheduler overhead for small fragments and honor BOLT's thread and
  // task options. Jobs own disjoint plans and only read the finalized input.
  if (opts::NoThreads || opts::ThreadCount == 1 || TotalCost < 32768) {
    Work(0, State->Fragments.size());
    return;
  }
  const uint64_t Jobs =
      std::max<uint64_t>(1, uint64_t(opts::ThreadCount) * opts::TaskCount);
  const uint64_t BatchCost = std::max<uint64_t>(32768, TotalCost / Jobs);
  ThreadPoolInterface &Pool = ParallelUtilities::getThreadPool();
  size_t Begin = 0;
  uint64_t Cost = 0;
  for (size_t I = 0; I != Costs.size(); ++I) {
    Cost += Costs[I];
    if (Cost >= BatchCost) {
      Pool.async(Work, Begin, I + 1);
      Begin = I + 1;
      Cost = 0;
    }
  }
  if (Begin != Costs.size())
    Pool.async(Work, Begin, Costs.size());
  Pool.wait();
}

BinaryEmissionPlan::~BinaryEmissionPlan() = default;

namespace {

class BinaryEmitter {
private:
  BinaryEmitter(const BinaryEmitter &) = delete;
  BinaryEmitter &operator=(const BinaryEmitter &) = delete;

  MCStreamer &Streamer;
  BinaryContext &BC;
  BinaryEmissionPlan::Impl *Plan;

public:
  BinaryEmitter(MCStreamer &Streamer, BinaryContext &BC,
                BinaryEmissionPlan::Impl *Plan = nullptr)
      : Streamer(Streamer), BC(BC), Plan(Plan) {}

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
  // Avoid emitting function without instructions when overwriting the original
  // function in-place. Otherwise, emit the empty function to define the symbol.
  if (!shouldEmitFunction(Function, BC))
    return false;

  BinaryEmissionPlan::Impl::Fragment *Planned = nullptr;
  if (Plan) {
    assert(Plan->NextFragment < Plan->Fragments.size());
    Planned = &Plan->Fragments[Plan->NextFragment++];
    assert(Planned->BF == &Function && Planned->FF == &FF &&
           "planning and emission orders differ");
    if (!Planned->Relaxation->prepareForEmission(Function))
      return false;
  }

  MCSection *Section =
      BC.getCodeSection(Function.getCodeSectionName(FF.getFragmentNum()));
  const bool FragmentSections = useFragmentSections(BC);
  const uint16_t MaxAlignBytes = FF.isSplitFragment()
                                     ? Function.getMaxColdAlignmentBytes()
                                     : Function.getMaxAlignmentBytes();
  const size_t PrefixSize =
      Planned ? Planned->PrefixSize : opts::padFunctionBefore(Function);
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

  // Emit UD2 at the beginning if requested by user.
  if (Planned ? Planned->Break : breakFunction(Function))
    Streamer.emitIntValue(0x0B0F, 2); // UD2: 0F 0B

  // Emit code.
  emitFunctionBody(Function, FF, /*EmitCodeOnly=*/false,
                   Planned ? Planned->Relaxation.get() : nullptr);
  // The CFG will be released after the last fragment. Release references to
  // its instructions and plan storage as soon as this fragment is emitted.
  if (Planned)
    Planned->Relaxation.reset();

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
  // Size estimation and callers outside emitAndLink never plan relaxation.
  const bool RelaxBranches = Relaxation != nullptr;

  if (!EmitCodeOnly && !Relaxation && FF.isSplitFragment() &&
      BF.hasConstantIsland()) {
    assert(BF.getLayout().isHotColdSplit() &&
           "Constant island support only with hot/cold split");
    BF.duplicateConstantIslands();
  }

  // Track the first emitted instruction with debug info.
  bool FirstInstr = true;
  for (BinaryBasicBlock *const BB : FF) {
    if ((BC.AlignBlocks || BC.PreserveBlocksAlignment) &&
        BB->getAlignment() > 1)
      Streamer.emitCodeAlignment(BB->getAlign(), *BC.STI,
                                 BB->getAlignmentMaxBytes());
    Streamer.emitLabel(BB->getLabel());
    if (!EmitCodeOnly) {
      if (MCSymbol *EntrySymbol = BF.getSecondaryEntryPointSymbol(*BB))
        Streamer.emitLabel(EntrySymbol);
    }

    SMLoc LastLocSeen;
    for (auto I = BB->begin(), E = BB->end(); I != E; ++I) {
      MCInst &Instr = *I;

      if (EmitCodeOnly && BC.MIB->isPseudo(Instr))
        continue;

      // Handle pseudo instructions.
      if (BC.MIB->isCFI(Instr)) {
        emitCFIInstruction(*BF.getCFIFor(Instr));
        continue;
      }

      if (!EmitCodeOnly) {
        // A symbol to be emitted before the instruction to mark its location.
        MCSymbol *InstrLabel = BC.MIB->getInstLabel(Instr);

        if (opts::UpdateDebugSections && !BF.getDWARFUnits().empty()) {
          LastLocSeen = emitLineInfo(BF, Instr.getLoc(), LastLocSeen,
                                     FirstInstr, InstrLabel);
          FirstInstr = false;
        }

        // Prepare to tag this location with a label if we need to keep track of
        // an instruction's output address to augment the IO address map (BAT,
        // SDT/probe address translation, or --update-debug-sections DWARF range
        // updates).
        if (BF.requiresPreciseAddressMap() && BC.MIB->getOffset(Instr)) {
          const uint32_t Offset = *BC.MIB->getOffset(Instr);
          if (!InstrLabel)
            InstrLabel = BC.Ctx->createTempSymbol();
          BB->getLocSyms().emplace_back(Offset, InstrLabel);
        }

        if (InstrLabel)
          Streamer.emitLabel(InstrLabel);
      }

      // Emit sized NOPs via MCAsmBackend::writeNopData() interface on x86.
      // This is a workaround for invalid NOPs handling by asm/disasm layer.
      if (BC.isX86() && BC.MIB->isNoop(Instr)) {
        if (std::optional<uint32_t> Size = BC.MIB->getSize(Instr)) {
          SmallString<15> Code;
          raw_svector_ostream VecOS(Code);
          BC.MAB->writeNopData(VecOS, *Size, BC.STI.get());
          Streamer.emitBytes(Code);
          continue;
        }
      }

      if (RelaxBranches)
        Relaxation->emitBefore(Instr, Streamer);
      if (!RelaxBranches || !Relaxation->emit(Instr, Streamer))
        Streamer.emitInstruction(Instr, *BC.STI);

      // A range ending at this instruction must stop before a synthetic branch
      // or a reordered successor. In particular, call-clobbered locations are
      // no longer valid at the B inserted after a call by fixBranches().
      if (RelaxBranches && opts::UpdateDebugSections)
        if (std::optional<uint32_t> Offset = BC.MIB->getOffset(Instr))
          AddressMap::emitInstructionEnd(Streamer, BC,
                                         BF.getAddress() + *Offset + 4);
    }
  }

  if (RelaxBranches)
    Relaxation->emitTrampolines(Streamer);

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
                       StringRef OrgSecPrefix, BinaryEmissionPlan &Plan) {
  BinaryEmitter(Streamer, BC, Plan.State.get()).emitAll(OrgSecPrefix);
  assert((!Plan.State ||
          Plan.State->NextFragment == Plan.State->Fragments.size()) &&
         "not all planned fragments were emitted");
}

void emitFunctionBody(MCStreamer &Streamer, BinaryFunction &BF,
                      FunctionFragment &FF, bool EmitCodeOnly) {
  BinaryEmitter(Streamer, BF.getBinaryContext())
      .emitFunctionBody(BF, FF, EmitCodeOnly);
}

} // namespace bolt
} // namespace llvm
