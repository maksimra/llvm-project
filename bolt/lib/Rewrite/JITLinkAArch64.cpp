//===- bolt/Rewrite/JITLinkAArch64.cpp - AArch64 JITLink support ---------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "bolt/Rewrite/JITLinkAArch64.h"
#include "bolt/Core/BinaryContext.h"
#include "bolt/Core/BinaryData.h"
#include "bolt/Core/BinaryFunction.h"
#include "bolt/Core/BinarySection.h"
#include "bolt/Core/FunctionLayout.h"
#include "bolt/Utils/CommandLineOpts.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ExecutionEngine/JITLink/JITLink.h"
#include "llvm/ExecutionEngine/JITLink/aarch64.h"
#include "llvm/ExecutionEngine/Orc/Shared/ExecutorAddress.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/MathExtras.h"
#include <limits>

namespace opts {
llvm::cl::opt<bool> VerifyBranch26Range(
    "verify-branch26-range",
    llvm::cl::desc("report AArch64 Branch26 edges after BOLT section mapping"),
    llvm::cl::Hidden, llvm::cl::cat(BoltCategory));
} // namespace opts

namespace llvm {
namespace bolt {

namespace {

constexpr uint64_t AArch64InstructionSize = 4;
constexpr uint64_t FixedBranch26ThunkInstructionCount = 5;
constexpr uint64_t Branch26ThunkSlotSize =
    FixedBranch26ThunkInstructionCount * AArch64InstructionSize;
constexpr StringLiteral Branch26AliasPrefix = "__BOLT_jitlink_branch26_target_";

unsigned getBranch26EncodingBits(const BinaryContext &BC) {
  const int Bits = BC.MIB->getUncondBranchEncodingSize();
  assert(Bits > 0 && Bits < 64 && "invalid unconditional branch width");
  return Bits;
}

int64_t getBranch26MinDisplacement(const BinaryContext &BC) {
  return -(1LL << (getBranch26EncodingBits(BC) - 1));
}

int64_t getBranch26MaxDisplacement(const BinaryContext &BC) {
  return (1LL << (getBranch26EncodingBits(BC) - 1)) - AArch64InstructionSize;
}

bool isBranch26InRange(const BinaryContext &BC, int64_t Displacement) {
  return isIntN(getBranch26EncodingBits(BC), Displacement) &&
         (static_cast<uint64_t>(Displacement) & (AArch64InstructionSize - 1)) ==
             0;
}

int64_t getBranch26Displacement(const jitlink::Block &Block,
                                const jitlink::Edge &Edge) {
  return Edge.getTarget().getAddress() - Block.getFixupAddress(Edge) +
         Edge.getAddend();
}

StringRef getBranch26TargetName(const jitlink::Symbol &Target) {
  if (!Target.hasName())
    return "<anonymous>";
  StringRef Name = *Target.getName();
  StringRef Alias = Name;
  const size_t Prefix = Alias.find(Branch26AliasPrefix);
  if (Prefix == StringRef::npos)
    return Name;
  Alias = Alias.drop_front(Prefix + Branch26AliasPrefix.size());
  const size_t Separator = Alias.find('_');
  if (Separator == StringRef::npos)
    return Name;
  Alias = Alias.drop_front(Separator + 1);
  const size_t LengthSeparator = Alias.find('_');
  if (LengthSeparator == StringRef::npos)
    return Name;
  uint64_t OriginalLength = 0;
  if (Alias.take_front(LengthSeparator).getAsInteger(10, OriginalLength))
    return Name;
  Alias = Alias.drop_front(LengthSeparator + 1);
  return OriginalLength <= Alias.size() ? Alias.take_front(OriginalLength)
                                        : Name;
}

struct Branch26ThunkEntry {
  jitlink::Symbol *Target{nullptr};
  jitlink::Edge::AddendT Addend{0};
  jitlink::Symbol *Thunk{nullptr};
  bool Used{false};
};

using Branch26ThunkKey = std::pair<jitlink::Symbol *, jitlink::Edge::AddendT>;

struct Branch26ThunkGroup {
  jitlink::Block *Block{nullptr};
  std::string Name;
  SmallVector<Branch26ThunkEntry> Entries;
  DenseMap<Branch26ThunkKey, unsigned> EntryIndices;

  void reserve(jitlink::Symbol &Target, jitlink::Edge::AddendT Addend) {
    const Branch26ThunkKey Key(&Target, Addend);
    if (EntryIndices.try_emplace(Key, Entries.size()).second)
      Entries.push_back({&Target, Addend, nullptr, false});
  }

  Branch26ThunkEntry *find(jitlink::Symbol &Target,
                           jitlink::Edge::AddendT Addend) {
    auto It = EntryIndices.find(std::make_pair(&Target, Addend));
    return It == EntryIndices.end() ? nullptr : &Entries[It->second];
  }
};

struct Branch26SemanticBlock {
  jitlink::Block *Block{nullptr};
  Branch26ThunkGroup *PrimaryGroup{nullptr};
  std::string Name;
  uint64_t OriginalAddress{0};
  unsigned BatchIndex{0};
  uint64_t EarliestRequiredSource{std::numeric_limits<uint64_t>::max()};
  SmallVector<Branch26ThunkKey> RequiredThunks;
};

struct Branch26Source {
  jitlink::Edge *Edge{nullptr};
  unsigned SemanticBlockIndex{0};
};

struct Branch26PlanImpl {
  SmallVector<std::unique_ptr<Branch26ThunkGroup>> Groups;
  SmallVector<Branch26SemanticBlock> SemanticBlocks;
  SmallVector<Branch26Source> Sources;
  uint64_t BatchCount{0};
  uint64_t UniqueTargets{0};
  uint64_t AliasTargets{0};
};

struct SemanticSectionLayout {
  jitlink::Section *Section{nullptr};
  SmallVector<jitlink::Block *> OriginalBlocks;
  SmallVector<unsigned> SemanticBlockIndices;
  SmallVector<Branch26ThunkGroup *> ThunkGroups;
  uint64_t Alignment{1};
};

struct SemanticMarker {
  jitlink::Symbol *Symbol{nullptr};
  std::string DestinationName;
  std::string Description;
};

void writeAArch64Instruction(MutableArrayRef<char> Content, uint64_t Offset,
                             uint32_t Instruction) {
  support::endian::write32le(Content.data() + Offset, Instruction);
}

void materializeBranch26Thunk(Branch26ThunkGroup &Group,
                              Branch26ThunkEntry &Entry,
                              bool FixedLoadAddress) {
  assert(Entry.Thunk && "thunk slot has no symbol");
  const uint64_t Offset = Entry.Thunk->getOffset();
  MutableArrayRef<char> Content = Group.Block->getAlreadyMutableContent();
  if (FixedLoadAddress) {
    // movz x16, #0, lsl #48
    // movk x16, #0, lsl #32
    // movk x16, #0, lsl #16
    // movk x16, #0
    // br   x16
    constexpr uint32_t Instructions[] = {0xd2e00010, 0xf2c00010, 0xf2a00010,
                                         0xf2800010, 0xd61f0200};
    for (unsigned I = 0; I != std::size(Instructions); ++I)
      writeAArch64Instruction(Content, Offset + I * AArch64InstructionSize,
                              Instructions[I]);
    for (unsigned I = 0; I != 4; ++I)
      Group.Block->addEdge(jitlink::aarch64::MoveWide16,
                           Offset + I * AArch64InstructionSize, *Entry.Target,
                           Entry.Addend);
  } else {
    // adrp x16, target
    // add  x16, x16, :lo12:target
    // br   x16
    constexpr uint32_t Instructions[] = {0x90000010, 0x91000210, 0xd61f0200};
    for (unsigned I = 0; I != std::size(Instructions); ++I)
      writeAArch64Instruction(Content, Offset + I * AArch64InstructionSize,
                              Instructions[I]);
    Group.Block->addEdge(jitlink::aarch64::Page21, Offset, *Entry.Target,
                         Entry.Addend);
    Group.Block->addEdge(jitlink::aarch64::PageOffset12,
                         Offset + AArch64InstructionSize, *Entry.Target,
                         Entry.Addend);
  }
}

Error normalizeReferencesAcrossSplits(
    jitlink::LinkGraph &G,
    const DenseMap<jitlink::Block *, SmallVector<jitlink::Edge::OffsetT>>
        &SplitOffsets) {
  DenseMap<jitlink::Block *, DenseMap<uint64_t, jitlink::Symbol *>>
      ExactSymbols;
  for (jitlink::Symbol *Symbol : G.defined_symbols()) {
    jitlink::Block *Block = &Symbol->getBlock();
    auto SplitIt = SplitOffsets.find(Block);
    if (SplitIt == SplitOffsets.end())
      continue;

    const uint64_t Begin = Symbol->getOffset();
    const uint64_t Size = Symbol->getSize();
    if (Begin > Block->getSize() || Size > Block->getSize() - Begin)
      return make_error<jitlink::JITLinkError>(formatv(
          "symbol {0} lies outside a BOLT code block that must be split",
          Symbol->hasName() ? *Symbol->getName() : StringRef("<anonymous>")));
    if (Size) {
      auto It = llvm::upper_bound(SplitIt->second, Begin);
      if (It != SplitIt->second.end() && *It < Begin + Size)
        return make_error<jitlink::JITLinkError>(formatv(
            "symbol {0} crosses a BOLT semantic block boundary at offset "
            "{1:x}",
            Symbol->hasName() ? *Symbol->getName() : StringRef("<anonymous>"),
            *It));
    }
    ExactSymbols[Block].try_emplace(Begin, Symbol);
  }

  auto partition = [](ArrayRef<jitlink::Edge::OffsetT> Offsets,
                      uint64_t Offset) {
    return llvm::upper_bound(Offsets, Offset) - Offsets.begin();
  };

  for (jitlink::Section &Section : G.sections()) {
    for (jitlink::Block *Block : Section.blocks()) {
      for (jitlink::Edge &Edge : Block->edges()) {
        if (!Edge.getTarget().isDefined())
          continue;
        jitlink::Block *TargetBlock = &Edge.getTarget().getBlock();
        auto SplitIt = SplitOffsets.find(TargetBlock);
        if (SplitIt == SplitOffsets.end())
          continue;

        const uint64_t TargetOffset = Edge.getTarget().getOffset();
        if (TargetOffset > static_cast<uint64_t>(INT64_MAX))
          return make_error<jitlink::JITLinkError>(formatv(
              "relocation target {0} + {1} falls outside a BOLT code block "
              "that must be split",
              Edge.getTarget().hasName() ? *Edge.getTarget().getName()
                                         : StringRef("<anonymous>"),
              Edge.getAddend()));
        auto [EffectiveOffset, Overflow] =
            AddOverflow(static_cast<int64_t>(TargetOffset), Edge.getAddend());
        if (Overflow || EffectiveOffset < 0 ||
            static_cast<uint64_t>(EffectiveOffset) > TargetBlock->getSize())
          return make_error<jitlink::JITLinkError>(formatv(
              "relocation target {0} + {1} falls outside a BOLT code block "
              "that must be split",
              Edge.getTarget().hasName() ? *Edge.getTarget().getName()
                                         : StringRef("<anonymous>"),
              Edge.getAddend()));

        if (partition(SplitIt->second, TargetOffset) ==
            partition(SplitIt->second, EffectiveOffset))
          continue;

        jitlink::Symbol *&Exact = ExactSymbols[TargetBlock][EffectiveOffset];
        if (!Exact)
          Exact = &G.addAnonymousSymbol(*TargetBlock, EffectiveOffset,
                                        /*Size=*/0, /*IsCallable=*/false,
                                        /*IsLive=*/true);
        Edge.setTarget(*Exact);
        Edge.setAddend(0);
      }
    }
  }
  return Error::success();
}

Error splitCodeAtSemanticMarkers(
    jitlink::LinkGraph &G, ArrayRef<SemanticMarker> Markers,
    StringMap<SemanticSectionLayout> &Layouts,
    SmallVectorImpl<Branch26SemanticBlock> &Blocks) {
  DenseMap<jitlink::Block *, SmallVector<jitlink::Edge::OffsetT>> SplitOffsets;
  for (const SemanticMarker &Marker : Markers) {
    jitlink::Block &Block = Marker.Symbol->getBlock();
    const uint64_t Offset = Marker.Symbol->getOffset();
    if (Offset && Offset < Block.getSize())
      SplitOffsets[&Block].push_back(Offset);
  }

  for (auto &Entry : SplitOffsets) {
    llvm::sort(Entry.second);
    Entry.second.erase(llvm::unique(Entry.second), Entry.second.end());
  }

  if (Error Err = normalizeReferencesAcrossSplits(G, SplitOffsets))
    return Err;

  // ELF graph construction produces one block per emitted object section.
  // Split that block only at actual MC marker offsets. The resulting semantic
  // function/fragment blocks remain indivisible during batch planning.
  for (auto &Entry : SplitOffsets)
    G.splitBlock(*Entry.first, Entry.second);

  DenseMap<jitlink::Block *, unsigned> SemanticIndices;
  for (const SemanticMarker &Marker : Markers) {
    jitlink::Block *Block = &Marker.Symbol->getBlock();
    if (SemanticIndices.contains(Block))
      return make_error<jitlink::JITLinkError>(formatv(
          "multiple BOLT semantic markers resolve to one block in section {0}",
          Marker.DestinationName));
    const unsigned Index = Blocks.size();
    SemanticIndices[Block] = Index;
    Blocks.push_back(
        {Block, nullptr, Marker.Description, Block->getAddress().getValue()});
    Layouts[Marker.DestinationName].SemanticBlockIndices.push_back(Index);
    G.removeDefinedSymbol(*Marker.Symbol);
  }

  for (auto &Entry : Layouts) {
    SemanticSectionLayout &Layout = Entry.second;
    Layout.Section = G.findSectionByName(Entry.getKey());
    if (!Layout.Section)
      return make_error<jitlink::JITLinkError>(
          formatv("missing BOLT code section {0}", Entry.getKey()));
    Layout.OriginalBlocks.assign(Layout.Section->blocks().begin(),
                                 Layout.Section->blocks().end());
    for (jitlink::Block *Block : Layout.OriginalBlocks)
      if (!SemanticIndices.contains(Block))
        return make_error<jitlink::JITLinkError>(
            formatv("unmarked block in BOLT code section {0}", Entry.getKey()));
    llvm::sort(Layout.OriginalBlocks,
               [](const jitlink::Block *LHS, const jitlink::Block *RHS) {
                 return LHS->getAddress() < RHS->getAddress();
               });
    if (!Layout.OriginalBlocks.empty())
      Layout.Alignment = Layout.OriginalBlocks.front()->getAlignment();
    llvm::sort(Layout.SemanticBlockIndices, [&](unsigned LHS, unsigned RHS) {
      return Blocks[LHS].Block->getAddress() < Blocks[RHS].Block->getAddress();
    });
  }
  return Error::success();
}

Error reserveBranch26Thunks(const BinaryContext &BC, jitlink::LinkGraph &G,
                            StringMap<SemanticSectionLayout> &Layouts,
                            Branch26PlanImpl &Plan) {
  auto createGroup = [&](Twine Name) {
    auto Group = std::make_unique<Branch26ThunkGroup>();
    Group->Name = Name.str();
    Branch26ThunkGroup *Result = Group.get();
    Plan.Groups.push_back(std::move(Group));
    return Result;
  };

  SmallPtrSet<jitlink::Symbol *, 16> UniqueTargets;
  SmallPtrSet<jitlink::Symbol *, 16> AliasTargets;
  for (unsigned BlockIndex = 0; BlockIndex != Plan.SemanticBlocks.size();
       ++BlockIndex) {
    Branch26SemanticBlock &SemanticBlock = Plan.SemanticBlocks[BlockIndex];
    DenseSet<Branch26ThunkKey> RequiredThunkSet;
    for (jitlink::Edge &Edge : SemanticBlock.Block->edges()) {
      if (Edge.getKind() != jitlink::aarch64::Branch26PCRel)
        continue;
      Plan.Sources.push_back({&Edge, BlockIndex});
      UniqueTargets.insert(&Edge.getTarget());
      if (Edge.getTarget().hasName() &&
          StringRef(*Edge.getTarget().getName()).contains(Branch26AliasPrefix))
        AliasTargets.insert(&Edge.getTarget());
      if (Edge.getTarget().isDefined() &&
          &Edge.getTarget().getBlock() == SemanticBlock.Block &&
          isBranch26InRange(
              BC, getBranch26Displacement(*SemanticBlock.Block, Edge)))
        continue;
      const Branch26ThunkKey Key(&Edge.getTarget(), Edge.getAddend());
      if (RequiredThunkSet.insert(Key).second)
        SemanticBlock.RequiredThunks.push_back(Key);
      SemanticBlock.EarliestRequiredSource =
          std::min(SemanticBlock.EarliestRequiredSource,
                   SemanticBlock.Block->getFixupAddress(Edge).getValue());
    }
  }
  Plan.UniqueTargets = UniqueTargets.size();
  Plan.AliasTargets = AliasTargets.size();

  unsigned NextBatchIndex = 0;
  for (auto &Entry : Layouts) {
    SemanticSectionLayout &Layout = Entry.second;
    // Keep the placement boundary at the end of the batch (D == C in mold's
    // terminology). Moving a group across later semantic blocks would insert
    // storage inside their batch and invalidate the uniform translation used
    // for same-batch reservation elision.
    SmallVector<unsigned> BatchBlocks;
    SmallVector<Branch26ThunkKey> BatchKeys;
    DenseMap<Branch26ThunkKey, bool> BatchKeySet;
    uint64_t BatchEarliestSource = std::numeric_limits<uint64_t>::max();

    auto finishBatch = [&]() -> Error {
      if (BatchBlocks.empty())
        return Error::success();

      DenseMap<jitlink::Block *, bool> BatchBlockSet;
      const unsigned BatchIndex = NextBatchIndex++;
      for (unsigned Index : BatchBlocks) {
        BatchBlockSet[Plan.SemanticBlocks[Index].Block] = true;
        Plan.SemanticBlocks[Index].BatchIndex = BatchIndex;
      }

      DenseMap<Branch26ThunkKey, bool> OutOfRangeKeys;
      for (unsigned Index : BatchBlocks) {
        const Branch26SemanticBlock &SemanticBlock = Plan.SemanticBlocks[Index];
        for (const jitlink::Edge &Edge : SemanticBlock.Block->edges())
          if (Edge.getKind() == jitlink::aarch64::Branch26PCRel &&
              !isBranch26InRange(
                  BC, getBranch26Displacement(*SemanticBlock.Block, Edge)))
            OutOfRangeKeys[std::make_pair(&Edge.getTarget(),
                                          Edge.getAddend())] = true;
      }

      SmallVector<Branch26ThunkKey> ReservedKeys;
      for (const Branch26ThunkKey &Key : BatchKeys) {
        // No group is placed within a batch. A defined target in the same
        // batch therefore retains its exact MC-computed displacement after
        // the whole batch is moved, including the target addend normalized
        // by splitCodeAtSemanticMarkers.
        if (Key.first->isDefined() &&
            BatchBlockSet.contains(&Key.first->getBlock()) &&
            !OutOfRangeKeys.contains(Key))
          continue;
        ReservedKeys.push_back(Key);
      }

      if (!ReservedKeys.empty()) {
        const unsigned Last = BatchBlocks.back();
        Branch26ThunkGroup *Group =
            createGroup(formatv("after_batch_{0}", Last));
        for (const Branch26ThunkKey &Key : ReservedKeys)
          Group->reserve(*Key.first, Key.second);

        DenseMap<Branch26ThunkKey, bool> ReservedKeySet;
        for (const Branch26ThunkKey &Key : ReservedKeys)
          ReservedKeySet[Key] = true;
        uint64_t EarliestSource = std::numeric_limits<uint64_t>::max();
        for (unsigned Index : BatchBlocks) {
          const Branch26SemanticBlock &SemanticBlock =
              Plan.SemanticBlocks[Index];
          for (const jitlink::Edge &Edge : SemanticBlock.Block->edges())
            if (Edge.getKind() == jitlink::aarch64::Branch26PCRel &&
                ReservedKeySet.contains(
                    std::make_pair(&Edge.getTarget(), Edge.getAddend())))
              EarliestSource = std::min(
                  EarliestSource,
                  SemanticBlock.Block->getFixupAddress(Edge).getValue());
        }
        assert(EarliestSource != std::numeric_limits<uint64_t>::max() &&
               "reserved group has no Branch26 source");

        const Branch26SemanticBlock &LastBlock = Plan.SemanticBlocks[Last];
        const uint64_t BatchEnd = LastBlock.Block->getAddress().getValue() +
                                  LastBlock.Block->getSize();
        const uint64_t SourceToBatchEnd = BatchEnd - EarliestSource;
        const uint64_t GroupSize =
            alignTo(Group->Entries.size() * Branch26ThunkSlotSize,
                    AArch64InstructionSize);
        const uint64_t MaxDisplacement = getBranch26MaxDisplacement(BC);
        // The group is placed immediately after the batch. Starting with the
        // earliest relevant source, the last reserved slot is no farther than
        // the source-to-end span, the worst group-alignment gap, and the full
        // pessimistic group size. Later sources and earlier slots are closer.
        if (SourceToBatchEnd > MaxDisplacement ||
            GroupSize > MaxDisplacement - SourceToBatchEnd ||
            AArch64InstructionSize - 1 >
                MaxDisplacement - SourceToBatchEnd - GroupSize)
          return make_error<jitlink::JITLinkError>(formatv(
              "BOLT-ERROR: experimental JITLink Branch26 relaxation cannot "
              "place a post-batch thunk group for semantic block {0}: batch "
              "source-to-end span {1}, group size {2}, Branch26 positive "
              "limit {3}",
              Plan.SemanticBlocks[BatchBlocks.front()].Name, SourceToBatchEnd,
              GroupSize, MaxDisplacement));

        Layout.ThunkGroups.push_back(Group);
        for (unsigned Index : BatchBlocks)
          Plan.SemanticBlocks[Index].PrimaryGroup = Group;
      }

      BatchBlocks.clear();
      BatchKeys.clear();
      BatchKeySet.clear();
      BatchEarliestSource = std::numeric_limits<uint64_t>::max();
      return Error::success();
    };

    for (unsigned Index : Layout.SemanticBlockIndices) {
      Branch26SemanticBlock &SemanticBlock = Plan.SemanticBlocks[Index];
      SmallVector<Branch26ThunkKey> NewKeys;
      for (const Branch26ThunkKey &Key : SemanticBlock.RequiredThunks)
        if (!BatchKeySet.contains(Key))
          NewKeys.push_back(Key);

      const uint64_t CandidateEnd =
          SemanticBlock.Block->getAddress().getValue() +
          SemanticBlock.Block->getSize();
      const uint64_t CandidateEarliestSource =
          std::min(BatchEarliestSource, SemanticBlock.EarliestRequiredSource);
      const uint64_t CandidateGroupSize =
          alignTo((BatchKeys.size() + NewKeys.size()) * Branch26ThunkSlotSize,
                  AArch64InstructionSize);
      const uint64_t CandidateSourceToEnd =
          CandidateEarliestSource == std::numeric_limits<uint64_t>::max()
              ? 0
              : CandidateEnd - CandidateEarliestSource;
      // The group immediately after the batch is reachable from every source
      // when the earliest source can reach the last pessimistic slot. Use the
      // exact positive Branch26 limit: all source-to-group branches are
      // forward. Include the worst possible instruction-alignment gap.
      const uint64_t MaxDisplacement = getBranch26MaxDisplacement(BC);
      const bool Fits =
          CandidateSourceToEnd <= MaxDisplacement &&
          CandidateGroupSize <= MaxDisplacement - CandidateSourceToEnd &&
          AArch64InstructionSize - 1 <=
              MaxDisplacement - CandidateSourceToEnd - CandidateGroupSize;
      if (!BatchBlocks.empty() && !Fits) {
        if (Error Err = finishBatch())
          return Err;
        NewKeys.assign(SemanticBlock.RequiredThunks.begin(),
                       SemanticBlock.RequiredThunks.end());
      }

      BatchEarliestSource =
          std::min(BatchEarliestSource, SemanticBlock.EarliestRequiredSource);
      BatchBlocks.push_back(Index);
      for (const Branch26ThunkKey &Key : NewKeys) {
        BatchKeySet[Key] = true;
        BatchKeys.push_back(Key);
      }
    }
    if (Error Err = finishBatch())
      return Err;

    for (Branch26ThunkGroup *Group : Layout.ThunkGroups) {
      const uint64_t Size =
          alignTo(Group->Entries.size() * Branch26ThunkSlotSize,
                  AArch64InstructionSize);
      Group->Block = &G.createMutableContentBlock(
          *Layout.Section, Size, orc::ExecutorAddr(),
          /*Alignment=*/AArch64InstructionSize,
          /*AlignmentOffset=*/0);
      // Linker veneers own no frame or FDE. They preserve SP and LR, so normal
      // call/exception semantics are preserved. Async unwind initiated inside
      // a veneer may report missing unwind information, as with lld veneers.
      MutableArrayRef<char> Content = Group->Block->getAlreadyMutableContent();
      for (uint64_t Offset = 0; Offset != Size;
           Offset += AArch64InstructionSize)
        writeAArch64Instruction(Content, Offset, 0xd503201f);
      for (unsigned I = 0; I != Group->Entries.size(); ++I) {
        Branch26ThunkEntry &Thunk = Group->Entries[I];
        const uint64_t Offset = I * Branch26ThunkSlotSize;
        Thunk.Thunk = &G.addDefinedSymbol(
            *Group->Block, Offset,
            formatv("__BOLT_branch26_thunk_{0}_{1}", Group->Name, I).str(),
            Branch26ThunkSlotSize, jitlink::Linkage::Strong,
            jitlink::Scope::Local, /*IsCallable=*/true, /*IsLive=*/true);
      }
    }
  }
  Plan.BatchCount = NextBatchIndex;
  return Error::success();
}

Error layoutCodeWithBranch26Groups(
    StringMap<SemanticSectionLayout> &Layouts,
    ArrayRef<Branch26SemanticBlock> SemanticBlocks) {
  DenseMap<jitlink::Block *, unsigned> SemanticIndices;
  for (unsigned I = 0; I != SemanticBlocks.size(); ++I)
    SemanticIndices[SemanticBlocks[I].Block] = I;

  for (auto &Entry : Layouts) {
    SemanticSectionLayout &Layout = Entry.second;
    DenseMap<unsigned, unsigned> Positions;
    for (unsigned I = 0; I != Layout.SemanticBlockIndices.size(); ++I)
      Positions[Layout.SemanticBlockIndices[I]] = I;

    SmallVector<jitlink::Block *> OrderedBlocks;
    for (jitlink::Block *Block : Layout.OriginalBlocks) {
      auto It = SemanticIndices.find(Block);
      OrderedBlocks.push_back(Block);
      if (It != SemanticIndices.end()) {
        const unsigned Position = Positions[It->second];
        Branch26ThunkGroup *Group = SemanticBlocks[It->second].PrimaryGroup;
        if (Group && Group->Block &&
            (Position + 1 == Layout.SemanticBlockIndices.size() ||
             SemanticBlocks[Layout.SemanticBlockIndices[Position + 1]]
                     .PrimaryGroup != Group))
          OrderedBlocks.push_back(Group->Block);
      }
    }
    if (OrderedBlocks.empty())
      continue;

    if (OrderedBlocks.front()->getAlignment() < Layout.Alignment) {
      OrderedBlocks.front()->setAlignment(Layout.Alignment);
      OrderedBlocks.front()->setAlignmentOffset(0);
    }
    uint64_t Offset = 0;
    for (jitlink::Block *Block : OrderedBlocks) {
      Offset = jitlink::alignToBlock(Offset, *Block);
      Block->setAddress(orc::ExecutorAddr(Offset));
      Offset += Block->getSize();
    }

    DenseMap<unsigned, int64_t> BatchTranslations;
    for (unsigned Index : Layout.SemanticBlockIndices) {
      const Branch26SemanticBlock &SemanticBlock = SemanticBlocks[Index];
      const int64_t Translation =
          static_cast<int64_t>(SemanticBlock.Block->getAddress().getValue()) -
          static_cast<int64_t>(SemanticBlock.OriginalAddress);
      auto [It, Inserted] =
          BatchTranslations.try_emplace(SemanticBlock.BatchIndex, Translation);
      if (!Inserted && It->second != Translation)
        return make_error<jitlink::JITLinkError>(formatv(
            "BOLT semantic-block layout changed an intra-batch displacement "
            "in section {0}",
            Entry.getKey()));
    }
  }
  return Error::success();
}

int64_t getThunkTargetPageDisplacement(const Branch26ThunkEntry &Entry) {
  const uint64_t TargetPage =
      (Entry.Target->getAddress().getValue() + Entry.Addend) & ~0xfffULL;
  const uint64_t ThunkPage = Entry.Thunk->getAddress().getValue() & ~0xfffULL;
  return static_cast<int64_t>(TargetPage - ThunkPage);
}

bool isThunkTargetReachable(const BinaryContext &BC,
                            const Branch26ThunkEntry &Entry) {
  return BC.HasFixedLoadAddress ||
         isInt<33>(getThunkTargetPageDisplacement(Entry));
}

enum class Branch26TargetKind {
  External,
  OldText,
  PLT,
  BOLTCode,
  Other,
  Count
};

StringRef getBranch26TargetKindName(Branch26TargetKind Kind) {
  switch (Kind) {
  case Branch26TargetKind::External:
    return "external";
  case Branch26TargetKind::OldText:
    return "old-text";
  case Branch26TargetKind::PLT:
    return "PLT";
  case Branch26TargetKind::BOLTCode:
    return "BOLT-code";
  case Branch26TargetKind::Other:
    return "other";
  case Branch26TargetKind::Count:
    break;
  }
  llvm_unreachable("invalid Branch26 target kind");
}

bool isPLTTarget(const BinaryContext &BC, const jitlink::Symbol &Target) {
  if (Target.hasName()) {
    StringRef Name = *Target.getName();
    if (Name.contains("@PLT") || Name.ends_with("$plt") ||
        BC.getPLTBinaryDataByName(Name))
      return true;
  }
  if (ErrorOr<const BinarySection &> Section =
          BC.getSectionForAddress(Target.getAddress().getValue()))
    return Section->getName().contains(".plt");
  return false;
}

Branch26TargetKind classifyBranch26Target(const BinaryContext &BC,
                                          const jitlink::Symbol &Target) {
  if (isPLTTarget(BC, Target))
    return Branch26TargetKind::PLT;
  const uint64_t Address = Target.getAddress().getValue();
  if (Address >= BC.OldTextSectionAddress &&
      Address - BC.OldTextSectionAddress < BC.OldTextSectionSize)
    return Branch26TargetKind::OldText;
  if (Target.isExternal())
    return Branch26TargetKind::External;
  if (Target.isDefined() && (Target.getSection().getMemProt() &
                             orc::MemProt::Exec) != orc::MemProt::None)
    return Branch26TargetKind::BOLTCode;
  return Branch26TargetKind::Other;
}

StringRef getBranch26Opcode(const jitlink::Block &Block,
                            const jitlink::Edge &Edge) {
  if (Block.isZeroFill() ||
      Edge.getOffset() + AArch64InstructionSize > Block.getSize())
    return "unknown";
  uint32_t Instr =
      support::endian::read32le(Block.getContent().data() + Edge.getOffset());
  switch (Instr & 0xfc000000) {
  case 0x94000000:
    return "CALL26";
  case 0x14000000:
    return "JUMP26";
  default:
    return "unknown";
  }
}

} // namespace

class AArch64JITLinkBranch26RelaxationPlan::Impl : public Branch26PlanImpl {};

AArch64JITLinkBranch26RelaxationPlan::AArch64JITLinkBranch26RelaxationPlan()
    : P(std::make_unique<Impl>()) {}

AArch64JITLinkBranch26RelaxationPlan::~AArch64JITLinkBranch26RelaxationPlan() =
    default;

Error prepareAArch64JITLinkBranch26Relaxation(
    BinaryContext &BC, jitlink::LinkGraph &G,
    AArch64JITLinkBranch26RelaxationPlan &Plan) {
  Branch26PlanImpl &P = *Plan.P;
  SmallVector<SemanticMarker> Markers;
  StringMap<SemanticSectionLayout> Layouts;
  for (BinaryFunction *BF : BC.getOutputBinaryFunctions()) {
    if (!BC.shouldEmit(*BF) || BF->isPatch())
      continue;
    for (const FragmentNum Fragment : BF->getJITLinkCodeFragments()) {
      const std::string MarkerName = BF->getJITLinkCodeStartName(Fragment);
      jitlink::Symbol *Marker = G.findDefinedSymbolByName(G.intern(MarkerName));
      if (!Marker)
        return make_error<jitlink::JITLinkError>(
            formatv("missing BOLT semantic marker {0}", MarkerName));
      const std::string DestinationName =
          BF->getCodeSectionName(Fragment).str().str();
      if (Marker->getSection().getName() != DestinationName)
        return make_error<jitlink::JITLinkError>(formatv(
            "BOLT semantic marker {0} is in {1}, expected {2}", MarkerName,
            Marker->getSection().getName(), DestinationName));
      Layouts[DestinationName];
      Markers.push_back(
          {Marker, DestinationName,
           formatv("{0}[fragment {1}]", BF->getPrintName(), Fragment.get())
               .str()});
    }
  }

  if (Error Err =
          splitCodeAtSemanticMarkers(G, Markers, Layouts, P.SemanticBlocks))
    return Err;
  if (Error Err = reserveBranch26Thunks(BC, G, Layouts, P))
    return Err;
  return layoutCodeWithBranch26Groups(Layouts, P.SemanticBlocks);
}

Error relaxAArch64JITLinkBranch26(BinaryContext &BC,
                                  AArch64JITLinkBranch26RelaxationPlan &Plan) {
  Branch26PlanImpl &P = *Plan.P;
  uint64_t DirectEdges = 0;
  uint64_t RelaxedEdges = 0;
  for (Branch26Source &Source : P.Sources) {
    jitlink::Edge &Edge = *Source.Edge;
    Branch26SemanticBlock &SemanticBlock =
        P.SemanticBlocks[Source.SemanticBlockIndex];
    jitlink::Block &Block = *SemanticBlock.Block;
    if (isBranch26InRange(BC, getBranch26Displacement(Block, Edge))) {
      ++DirectEdges;
      continue;
    }

    const orc::ExecutorAddr FixupAddress = Block.getFixupAddress(Edge);
    Branch26ThunkGroup *Group = SemanticBlock.PrimaryGroup;
    Branch26ThunkEntry *Selected =
        Group ? Group->find(Edge.getTarget(), Edge.getAddend()) : nullptr;

    const StringRef TargetName = getBranch26TargetName(Edge.getTarget());
    if (!Selected || !Selected->Thunk)
      return make_error<jitlink::JITLinkError>(formatv(
          "BOLT-ERROR: stable same-batch Branch26 edge from {0}+{1:x} to "
          "{2} + {3} became out of range after final mapping",
          SemanticBlock.Name, Edge.getOffset(), TargetName, Edge.getAddend()));

    const int64_t ThunkDisplacement =
        Selected->Thunk->getAddress() - FixupAddress;
    const int64_t TargetPageDisplacement =
        getThunkTargetPageDisplacement(*Selected);
    if (!isThunkTargetReachable(BC, *Selected))
      return make_error<jitlink::JITLinkError>(formatv(
          "BOLT-ERROR: experimental JITLink Branch26 relaxation cannot "
          "materialize PIC target {0} + {1} from {2}+{3:x}: the post-batch "
          "thunk group ADRP page displacement is {4}; the supported Page21 "
          "range is +/-4 GiB",
          TargetName, Edge.getAddend(), SemanticBlock.Name, Edge.getOffset(),
          TargetPageDisplacement));
    if (!isBranch26InRange(BC, ThunkDisplacement))
      return make_error<jitlink::JITLinkError>(formatv(
          "BOLT-ERROR: experimental JITLink Branch26 relaxation produced an "
          "unreachable post-batch thunk from address {0:x} ({1}+{2:x}) "
          "targeting {3} + {4}: displacement {5}; Branch26 range is "
          "[{6}, {7}] with 4-byte alignment",
          FixupAddress.getValue(), SemanticBlock.Name, Edge.getOffset(),
          TargetName, Edge.getAddend(), ThunkDisplacement,
          getBranch26MinDisplacement(BC), getBranch26MaxDisplacement(BC)));

    // PreFixup runs after BOLT's MapSections pass. Mutating allocated content
    // and adding edges is safe before JITLink applies fixups; block size and
    // section extent remain unchanged.
    if (!Selected->Used) {
      materializeBranch26Thunk(*Group, *Selected, BC.HasFixedLoadAddress);
      Selected->Used = true;
    }
    Edge.setTarget(*Selected->Thunk);
    Edge.setAddend(0);
    ++RelaxedEdges;
  }

  uint64_t UsedEntries = 0;
  uint64_t ReservedEntries = 0;
  uint64_t GroupBytes = 0;
  uint64_t UnusedPICOutOfRange = 0;
  for (const std::unique_ptr<Branch26ThunkGroup> &Group : P.Groups)
    if (Group->Block) {
      GroupBytes += Group->Block->getSize();
      for (const Branch26ThunkEntry &Entry : Group->Entries) {
        ++ReservedEntries;
        UsedEntries += Entry.Used;
        UnusedPICOutOfRange += !BC.HasFixedLoadAddress && !Entry.Used &&
                               !isThunkTargetReachable(BC, Entry);
      }
    }
  BC.errs() << "BOLT-INFO: JITLink Branch26 relaxation: batches="
            << P.BatchCount << ", groups=" << P.Groups.size()
            << ", reserved=" << ReservedEntries << ", used=" << UsedEntries
            << ", direct=" << DirectEdges << ", relaxed=" << RelaxedEdges
            << ", semantic-blocks=" << P.SemanticBlocks.size()
            << ", group-bytes=" << GroupBytes
            << ", unused-pic-out-of-range=" << UnusedPICOutOfRange
            << ", unique-targets=" << P.UniqueTargets
            << ", aliases=" << P.AliasTargets << '\n';
  return Error::success();
}

bool shouldVerifyAArch64JITLinkBranch26Range() {
  return opts::VerifyBranch26Range;
}

Error verifyAArch64JITLinkBranch26Range(BinaryContext &BC,
                                        jitlink::LinkGraph &G) {
  uint64_t Total = 0;
  uint64_t OutOfRange = 0;
  uint64_t Calls = 0;
  uint64_t Jumps = 0;
  uint64_t TargetCounts[static_cast<unsigned>(Branch26TargetKind::Count)] = {};
  for (jitlink::Section &Section : G.sections()) {
    for (jitlink::Block *Block : Section.blocks()) {
      for (const jitlink::Edge &Edge : Block->edges()) {
        if (Edge.getKind() != jitlink::aarch64::Branch26PCRel)
          continue;
        ++Total;
        StringRef Opcode = getBranch26Opcode(*Block, Edge);
        Calls += Opcode == "CALL26";
        Jumps += Opcode == "JUMP26";
        const Branch26TargetKind TargetKind =
            classifyBranch26Target(BC, Edge.getTarget());
        ++TargetCounts[static_cast<unsigned>(TargetKind)];
        const uint64_t FixupAddress = Block->getFixupAddress(Edge).getValue();
        const int64_t Displacement = getBranch26Displacement(*Block, Edge);
        if (isBranch26InRange(BC, Displacement))
          continue;
        ++OutOfRange;
        BC.errs() << "BOLT-WARNING: out-of-range Branch26PCRel " << Opcode
                  << " edge at 0x" << Twine::utohexstr(FixupAddress) << " in "
                  << Section.getName() << " targets 0x"
                  << Twine::utohexstr(Edge.getTarget().getAddress().getValue())
                  << " (" << getBranch26TargetKindName(TargetKind);
        if (Edge.getTarget().hasName())
          BC.errs() << ", " << getBranch26TargetName(Edge.getTarget());
        BC.errs() << "), displacement " << Displacement << ", addend "
                  << Edge.getAddend() << '\n';
      }
    }
  }

  BC.errs() << "BOLT-INFO: AArch64 Branch26PCRel edges: total=" << Total
            << ", out-of-range=" << OutOfRange << ", CALL26=" << Calls
            << ", JUMP26=" << Jumps;
  for (unsigned I = 0; I != std::size(TargetCounts); ++I)
    BC.errs() << ", "
              << getBranch26TargetKindName(static_cast<Branch26TargetKind>(I))
              << '=' << TargetCounts[I];
  BC.errs() << '\n';
  return Error::success();
}

} // namespace bolt
} // namespace llvm
