//===- JITLinkAArch64.cpp - Late AArch64 branch relaxation
//-----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "bolt/Rewrite/JITLinkAArch64.h"
#include "bolt/Core/BinaryContext.h"
#include "bolt/Core/BinarySection.h"
#include "bolt/Rewrite/JITLinkLinker.h"
#include "bolt/Utils/CommandLineOpts.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ExecutionEngine/JITLink/aarch64.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/MathExtras.h"
#include <limits>

#define DEBUG_TYPE "bolt"

using namespace llvm;
using namespace llvm::bolt;

namespace {
using FragmentMap = JITLinkAArch64::FragmentMap;
using Stub = JITLinkAArch64::Stub;
using SectionBases = JITLinkAArch64::SectionBases;
using SymbolResolver = JITLinkAArch64::SymbolResolver;
using SectionsMapper = BOLTLinker::SectionsMapper;

bool inBranchRange(orc::ExecutorAddr Source, orc::ExecutorAddr Target) {
  const int64_t Delta = Target - Source;
  return isInt<28>(Delta) && (Delta & 3) == 0;
}

unsigned stubSize(orc::ExecutorAddr Source, orc::ExecutorAddr Target) {
  if (inBranchRange(Source, Target))
    return 4;
  const int64_t Pages =
      int64_t(Target.getValue() >> 12) - int64_t(Source.getValue() >> 12);
  return isInt<21>(Pages) ? 12 : 20;
}

void fillNops(MutableArrayRef<char> Bytes) {
  for (size_t I = 0; I + 4 <= Bytes.size(); I += 4)
    support::endian::write32le(Bytes.data() + I, 0xd503201f);
}

jitlink::Edge &branchEdge(const JITLinkAArch64::Branch &Branch) {
  auto Edges = Branch.SourceBlock->edges_at(Branch.SourceOffset);
  auto It = llvm::find_if(Edges, [](const auto &Edge) {
    return Edge.getKind() == jitlink::aarch64::Branch26PCRel;
  });
  assert(It != Edges.end() && "Missing original branch");
  return *It;
}

uint64_t placeFragment(jitlink::Block &Block, uint64_t Offset,
                       const FragmentMap &Fragments) {
  // MC has already resolved internal body alignment. Only entry preferences
  // have a byte budget; the same rule applies to reservation and compaction.
  Offset = jitlink::alignToBlock(Offset, Block);
  if (auto It = Fragments.find(&Block); It != Fragments.end()) {
    assert(!Block.getAlignmentOffset() &&
           "Emitted fragments must have zero alignment offset");
    const auto &Fragment = *It->second;
    const uint64_t Pad =
        offsetToAlignment(Offset, Align(Fragment.PreferredAlignment));
    if (Pad <= Fragment.MaxAlignmentBytes)
      Offset += Pad;
  }
  Block.setAddress(orc::ExecutorAddr(Offset));
  return Offset + Block.getSize();
}

/// Target's {Addressable + Offset}
using StubTarget = std::pair<const jitlink::Addressable *, uint64_t>;

StubTarget stubTarget(const jitlink::Edge &Edge) {
  const auto &Target = Edge.getTarget();
  if (Target.isAbsolute())
    return {nullptr, (Target.getAddress() + Edge.getAddend()).getValue()};
  return {&Target.getAddressable(), Target.getOffset() + Edge.getAddend()};
}

/// Materialize a selected form, or choose the shortest form when Size is zero.
Expected<unsigned> materializeStub(jitlink::Symbol &SlotSym,
                                   jitlink::Symbol &Target,
                                   jitlink::Edge::AddendT Addend,
                                   bool HasFixedLoadAddress,
                                   unsigned Size = 0) {
  const auto TargetAddress = Target.getAddress() + Addend;
  const unsigned MinimumSize = stubSize(SlotSym.getAddress(), TargetAddress);
  if (!Size)
    Size = MinimumSize;
  if (Size < MinimumSize || (Size == 20 && !HasFixedLoadAddress))
    return make_error<jitlink::JITLinkError>(
        "Cannot materialize a reachable AArch64 branch stub at 0x" +
        Twine::utohexstr(SlotSym.getAddress().getValue()) + " for target 0x" +
        Twine::utohexstr(TargetAddress.getValue()));

  auto &Block = SlotSym.getBlock();
  const uint64_t Offset = SlotSym.getOffset();
  assert(Size <= SlotSym.getSize() && "Stub exceeds slot capacity");
  auto Bytes = Block.getAlreadyMutableContent().slice(Offset, Size);
  if (Size == 4) {
    support::endian::write32le(Bytes.data(), 0x14000000); // B
    Block.addEdge(jitlink::aarch64::Branch26PCRel, Offset, Target, Addend);
  } else if (Size == 12) {
    support::endian::write32le(Bytes.data(), 0x90000010);     // ADRP x16
    support::endian::write32le(Bytes.data() + 4, 0x91000210); // ADD x16, x16
    support::endian::write32le(Bytes.data() + 8, 0xd61f0200); // BR x16
    Block.addEdge(jitlink::aarch64::Page21, Offset, Target, Addend);
    Block.addEdge(jitlink::aarch64::PageOffset12, Offset + 4, Target, Addend);
  } else {
    assert(Size == 20 && "Unexpected AArch64 stub size");
    for (unsigned I = 0; I != 4; ++I) {
      const uint32_t Imm = (TargetAddress.getValue() >> (16 * I)) & 0xffff;
      const uint32_t Opcode = I == 0 ? 0xd2800000 : 0xf2800000;
      support::endian::write32le(Bytes.data() + 4 * I,
                                 Opcode | (I << 21) | (Imm << 5) | 16);
    }
    support::endian::write32le(Bytes.data() + 16, 0xd61f0200); // BR x16
  }
  return Size;
}

} // namespace

namespace {

using FragmentBlocksBySection =
    DenseMap<jitlink::Section *, SmallVector<jitlink::Block *>>;

Error validateFragmentEdge(const jitlink::Block &Source,
                           const jitlink::Edge &Edge) {
  const auto &Target = Edge.getTarget();
  int64_t Addend = Edge.getAddend();
  if (Edge.getKind() == jitlink::aarch64::NegDelta32 ||
      Edge.getKind() == jitlink::aarch64::NegDelta64) {
    if (Addend == std::numeric_limits<int64_t>::min())
      return make_error<jitlink::JITLinkError>(
          "Unsupported addend in emitted fragment section");
    Addend = -Addend;
  }
  const uint64_t Offset = Target.getOffset();
  const uint64_t Size = Target.getBlock().getSize();
  // emitLSDA may use LPStart-1 to distinguish a landing pad at offset zero
  // from "no landing pad". This is an arithmetic base, not a code target.
  if (Edge.getKind() == jitlink::aarch64::Delta32 && Offset == 0 &&
      Addend == -1 && Source.getSection().getName() == ".gcc_except_table")
    return Error::success();
  if (Offset > Size || (Addend >= 0 && uint64_t(Addend) > Size - Offset) ||
      (Addend < 0 && uint64_t(-(Addend + 1)) + 1 > Offset))
    return make_error<jitlink::JITLinkError>(
        "Relocation target leaves emitted fragment section " +
        Target.getSection().getName());
  // One-past is useful for metadata, but a branch there would enter the
  // reserved island instead of the next function. Reject this case.
  if ((Edge.getKind() == jitlink::aarch64::Branch26PCRel ||
       Edge.getKind() == jitlink::aarch64::CondBranch19PCRel ||
       Edge.getKind() == jitlink::aarch64::TestAndBranch14PCRel) &&
      Offset + Addend == Size)
    return make_error<jitlink::JITLinkError>(
        "Branch target is at the end of emitted fragment section " +
        Target.getSection().getName());
  return Error::success();
}

Error validateFragmentEdges(jitlink::LinkGraph &G,
                            const DenseSet<jitlink::Block *> &FragmentBlocks) {
  LLVM_DEBUG(dbgs() << "BOLT-DEBUG: Fragment Edges validation.\n");

  // S+A must stay in the original fragment. In particular, a section symbol
  // must not be reinterpreted as the start of the merged output section.
  for (auto *Block : G.blocks()) {
    for (const auto &Edge : Block->edges()) {
      const auto &Target = Edge.getTarget();
      if (!Edge.isRelocation() || !Target.isDefined() ||
          !FragmentBlocks.contains(&Target.getBlock()))
        continue;
      if (Error Err = validateFragmentEdge(*Block, Edge))
        return Err;
    }
  }
  return Error::success();
}

Error mergeFragmentSection(jitlink::LinkGraph &G,
                           const EmittedFragmentSection &Fragment,
                           jitlink::Section &Input,
                           FragmentBlocksBySection &BlocksBySection,
                           FragmentMap &FragmentsByBlock) {
  auto *Output = G.findSectionByName(Fragment.OutputSection);
  if (!Output) {
    Output = &G.createSection(Fragment.OutputSection, Input.getMemProt());
    Output->setMemLifetime(Input.getMemLifetime());
    Output->setOrdinal(Input.getOrdinal());
  }
  if (Output == &Input || Output->getMemProt() != Input.getMemProt() ||
      Output->getMemLifetime() != Input.getMemLifetime())
    return make_error<jitlink::JITLinkError>(
        "Incompatible logical output section for " + Fragment.Name);
  auto [It, Inserted] = BlocksBySection.try_emplace(Output);
  auto &Blocks = It->second;
  if (Inserted)
    Blocks = JITLinkLinker::orderedBlocks(*Output);
  auto *Block = *Input.blocks().begin(); // One block per emitted fragment.
  Blocks.push_back(Block);

  // Carry the logical section alignment on its first block. The memory
  // manager also accounts for every block's own alignment.
  if (Blocks.front()->getAlignmentOffset())
    return make_error<jitlink::JITLinkError>(
        "Unsupported first block alignment offset in " + Output->getName());
  Blocks.front()->setAlignment(
      std::max(Blocks.front()->getAlignment(), Fragment.OutputAlignment));
  if (Fragment.HasFallThrough)
    assert(Block->getSize() >= 4 && "Missing emitted fall-through branch");
  FragmentsByBlock[Block] = &Fragment;
  G.mergeSections(*Output, Input);
  return Error::success();
}

} // namespace

Error JITLinkAArch64::mergeFragments(jitlink::LinkGraph &G,
                                     ArrayRef<EmittedFragmentSection> Fragments,
                                     FragmentMap &FragmentsByBlock) {
  LLVM_DEBUG(dbgs() << "BOLT-DEBUG: begin of Fragment Sections merging.\n");

  FragmentBlocksBySection BlocksBySection;
  DenseSet<jitlink::Block *> FragmentBlocks;
  SmallVector<jitlink::Section *> Sections;
  for (const auto &Fragment : Fragments) {
    auto *Section = G.findSectionByName(Fragment.Name);
    if (!Section)
      return make_error<jitlink::JITLinkError>(
          "Missing emitted fragment section " + Fragment.Name);
    // ELFLinkGraphBuilder creates one block per input section. Enforce this
    // before merging: batching must never split an MC-resolved fragment.
    if (Section->blocks_size() != 1)
      return make_error<jitlink::JITLinkError>(
          "Expected one block in emitted fragment section " + Fragment.Name);
    Sections.push_back(Section);
    FragmentBlocks.insert(*Section->blocks().begin());
  }

  if (Error Err = validateFragmentEdges(G, FragmentBlocks))
    return Err;

  for (auto [Index, Fragment] : llvm::enumerate(Fragments))
    if (Error Err = mergeFragmentSection(G, Fragment, *Sections[Index],
                                         BlocksBySection, FragmentsByBlock))
      return Err;

  // Preserve emission order only. Byte placement happens in the linker.
  for (auto &[Section, Blocks] : BlocksBySection)
    for (auto [Index, Block] : llvm::enumerate(Blocks))
      Block->setAddress(orc::ExecutorAddr(Index));
  return Error::success();
}

namespace {

/// Guards jump to the next original fragment, over any intervening islands.
/// The last continuation is placed after all tail islands by the caller.
jitlink::Block *addFallThroughGuards(jitlink::LinkGraph &G,
                                     ArrayRef<jitlink::Block *> Blocks,
                                     const FragmentMap &FragmentsByBlock) {
  jitlink::Block *EndMarker = nullptr;
  for (auto [Index, Block] : llvm::enumerate(Blocks)) {
    const auto It = FragmentsByBlock.find(Block);
    if (It == FragmentsByBlock.end() || !It->second->HasFallThrough)
      continue;
    jitlink::Block *Next;
    if (Index + 1 < Blocks.size()) {
      Next = Blocks[Index + 1];
    } else {
      EndMarker = &G.createZeroFillBlock(Blocks.back()->getSection(), 0,
                                         orc::ExecutorAddr(), 1, 0);
      Next = EndMarker;
    }
    const auto NextFragment = FragmentsByBlock.find(Next);
    const uint64_t EntryOffset = NextFragment == FragmentsByBlock.end()
                                     ? 0
                                     : NextFragment->second->EntryOffset;
    auto &Target = G.addAnonymousSymbol(*Next, EntryOffset, 0, true, true);
    const auto Offset = Block->getSize() - 4;
    support::endian::write32le(Block->getMutableContent(G).data() + Offset,
                               0x14000000);
    Block->addEdge(jitlink::aarch64::Branch26PCRel, Offset, Target, 0);
  }
  return EndMarker;
}

/// One forward sweep over whole fragments. Only block addresses carry layout;
/// each reservation already has its final island block and slot symbol.
class AArch64StubPlacer {
  static constexpr uint64_t BranchDistance = 128 * 1024 * 1024;
  static constexpr uint64_t BatchSize = 32 * 1024 * 1024;
  static constexpr uint64_t MaxIslandSize = 16 * 1024 * 1024;
  static constexpr uint64_t StubSize = 20;
  static constexpr uint64_t Unplaced = std::numeric_limits<uint64_t>::max();

  jitlink::LinkGraph &G;
  jitlink::Section &Section;
  std::vector<Stub> &Stubs;
  DenseMap<StubTarget, size_t> SharedStubs;
  const FragmentMap &FragmentsByBlock;

public:
  AArch64StubPlacer(jitlink::LinkGraph &G, jitlink::Section &Section,
                    std::vector<Stub> &Stubs,
                    const FragmentMap &FragmentsByBlock)
      : G(G), Section(Section), Stubs(Stubs),
        FragmentsByBlock(FragmentsByBlock) {}

  Error run(raw_ostream *Log) {
    auto Blocks = JITLinkLinker::orderedBlocks(Section);
    const size_t FirstStub = Stubs.size();
    auto *EndMarker = addFallThroughGuards(G, Blocks, FragmentsByBlock);
    for (auto *Block : Section.blocks())
      Block->setAddress(orc::ExecutorAddr(Unplaced));

    uint64_t Offset = 0;
    uint64_t NumBatches = 0, NumIslands = 0;
    for (size_t B = 0, D = 0; B < Blocks.size();) {
      // B..C: current batch. C..D: placed look-ahead. New island goes at D.
      // First extend D, leaving room for the island within reach of B.
      while (D < Blocks.size()) {
        auto *Block = Blocks[D];
        const uint64_t End = placeFragment(*Block, Offset, FragmentsByBlock);
        if (D != B && alignTo(End, 4) + MaxIslandSize >
                          Blocks[B]->getAddress().getValue() + BranchDistance) {
          Block->setAddress(orc::ExecutorAddr(Unplaced));
          break;
        }
        Offset = End;
        ++D;
      }

      // Select C by code size and an upper bound on slots for branches not
      // already proven direct in the placed prefix. Keep large fragments
      // whole; reserveBatch checks every actual slot's reach and island size.
      size_t C = B + 1;
      uint64_t NumCandidates = countStubCandidates(*Blocks[B]);
      while (C < D &&
             Blocks[C]->getAddress().getValue() + Blocks[C]->getSize() <
                 Blocks[B]->getAddress().getValue() + BatchSize) {
        const uint64_t Next = countStubCandidates(*Blocks[C]);
        if (NumCandidates + Next > MaxIslandSize / StubSize)
          break;
        NumCandidates += Next;
        ++C;
      }
      const auto Batch = ArrayRef(Blocks).slice(B, C - B);
      const size_t FirstNewStub = Stubs.size();
      if (Error Err = reserveBatch(Batch, Offset))
        return Err;
      ++NumBatches;
      NumIslands += Stubs.size() != FirstNewStub;
      B = C;
    }
    if (EndMarker)
      JITLinkLinker::layoutBlocks({EndMarker}, Offset);
    if (Log) {
      uint64_t EmittedBytes = 0, GuardBytes = 0;
      for (const auto *Block : Blocks) {
        EmittedBytes += Block->getSize();
        auto It = FragmentsByBlock.find(Block);
        if (It != FragmentsByBlock.end() && It->second->HasFallThrough)
          GuardBytes += 4;
      }
      const uint64_t NumSlots = Stubs.size() - FirstStub;
      const uint64_t StubBytes = NumSlots * StubSize;
      const uint64_t Total = JITLinkLinker::getSectionAllocationSize(Section);
      *Log << "BOLT-INFO: JITLink batches: section=" << Section.getName()
           << " batches=" << NumBatches << " fragments=" << Blocks.size()
           << " islands=" << NumIslands << " slots=" << NumSlots << '\n'
           << "BOLT-INFO: JITLink layout: section=" << Section.getName()
           << " emitted-bytes=" << EmittedBytes << " guard-bytes=" << GuardBytes
           << " stub-bytes=" << StubBytes
           << " alignment-bytes=" << Total - EmittedBytes - StubBytes
           << " total-bytes=" << Total << '\n';
    }
    return Error::success();
  }

private:
  uint64_t countStubCandidates(const jitlink::Block &Block) const {
    return llvm::count_if(Block.edges(), [&](const auto &Edge) {
      return Edge.getKind() == jitlink::aarch64::Branch26PCRel &&
             !isProvenDirect(Edge,
                             Block.getAddress().getValue() + Edge.getOffset());
    });
  }

  bool isProvenDirect(const jitlink::Edge &Edge, uint64_t Source) const {
    const auto &Target = Edge.getTarget();
    return Target.isDefined() && &Target.getSection() == &Section &&
           Target.getBlock().getAddress().getValue() != Unplaced &&
           isInt<28>(int64_t(
               (Target.getAddress() + Edge.getAddend()).getValue() - Source));
  }

  Error reserveBatch(ArrayRef<jitlink::Block *> Blocks, uint64_t &Offset) {
    const uint64_t IslandOffset = alignTo(Offset, 4);
    jitlink::Block *Island = nullptr;
    SmallVector<const jitlink::Edge *, 8> Branches;
    for (auto *Block : Blocks) {
      Branches.clear();
      for (const auto &Edge : Block->edges())
        if (Edge.getKind() == jitlink::aarch64::Branch26PCRel)
          Branches.push_back(&Edge);
      // Source order makes the latest slot sufficient: if it is too far
      // backward, all older slots are too. This replaces mold's A sweep.
      llvm::stable_sort(Branches, [](const auto *L, const auto *R) {
        return L->getOffset() < R->getOffset();
      });
      for (const auto *Edge : Branches) {
        const uint64_t Source =
            Block->getAddress().getValue() + Edge->getOffset();
        if (isProvenDirect(*Edge, Source))
          continue;
        auto [It, Inserted] =
            SharedStubs.try_emplace(stubTarget(*Edge), Stubs.size());
        if (Inserted ||
            !isInt<28>(int64_t(
                Stubs[It->second].SlotSym->getAddress().getValue() - Source))) {
          const uint64_t SlotOffset = Island ? Island->getSize() : 0;
          if (!isInt<28>(int64_t(IslandOffset + SlotOffset - Source)))
            return make_error<jitlink::JITLinkError>(
                "Cannot place a reachable AArch64 stub island in " +
                Section.getName() + ": emitted fragment is too large");
          if (SlotOffset + StubSize > MaxIslandSize)
            return make_error<jitlink::JITLinkError>(
                "AArch64 stub island exceeds its reserved size in " +
                Section.getName());
          if (!Island)
            Island = &G.createZeroFillBlock(
                Section, 0, orc::ExecutorAddr(IslandOffset), 4, 0);
          // Grow the block without allocating bytes until its size is known.
          Island->setZeroFillSize(SlotOffset + StubSize);
          It->second = Stubs.size();
          Stubs.push_back(
              {&G.addAnonymousSymbol(*Island, SlotOffset, StubSize, true, true),
               {}});
        }
        Stubs[It->second].Branches.push_back({Block, Edge->getOffset()});
      }
    }
    if (Island) {
      auto Content = G.allocateBuffer(Island->getSize());
      fillNops(Content);
      Island->setMutableContent(Content);
      Offset = IslandOffset + Content.size();
    }
    return Error::success();
  }
};

} // namespace

Expected<std::vector<Stub>>
JITLinkAArch64::reserveStubs(jitlink::LinkGraph &G,
                             const FragmentMap &FragmentsByBlock,
                             raw_ostream *Log) {
  std::vector<Stub> Stubs;
  for (jitlink::Section &Section : G.sections()) {
    if ((Section.getMemProt() & orc::MemProt::Exec) == orc::MemProt::None)
      continue;
    if (Error Err =
            AArch64StubPlacer(G, Section, Stubs, FragmentsByBlock).run(Log))
      return std::move(Err);
  }
  return Stubs;
}

namespace {

/// A tentative linker layout. Bytes and edge targets change only in emit().
class StubCompactor {
  struct Slot {
    jitlink::Symbol *Symbol, *Target;
    int64_t Addend;
    jitlink::Block *OriginalBlock;
    uint64_t OriginalOffset;
    unsigned Size = 0;
    int Next = -1; // A negative link uses Target + Addend directly.
    bool Used = false;
    bool Bridge = false; // Chain hops must remain B, preserving registers.
  };
  struct Island {
    MutableArrayRef<char> OriginalContent;
    SmallVector<char, 0> Storage;
    uint64_t Size = 0;
  };
  struct BlockPlacement {
    jitlink::Block *Block;
    uint64_t OriginalOffset;
  };
  struct SectionLayout {
    jitlink::Section *Section;
    BinarySection *Binary;
    SmallVector<BlockPlacement> Blocks;
    uint64_t OriginalSize, Size, Base = 0;
  };
  struct Branch {
    jitlink::Block *Source;
    jitlink::Edge *Edge;
    int Slot = -1;
    orc::ExecutorAddr source() const { return Source->getFixupAddress(*Edge); }
    orc::ExecutorAddr target() const {
      return Edge->getTarget().getAddress() + Edge->getAddend();
    }
    bool isCall() const {
      return support::endian::read32le(Source->getContent().data() +
                                       Edge->getOffset()) &
             0x80000000;
    }
  };

  BinaryContext &BC;
  jitlink::LinkGraph &G;
  ArrayRef<Stub> Reservations;
  const FragmentMap &Fragments;
  const SectionsMapper &Plan;
  SymbolResolver Resolve;
  SmallVector<SectionLayout> Sections;
  SmallVector<Slot> Slots;
  SmallVector<Branch> Branches;
  DenseMap<jitlink::Block *, Island> Islands;
  DenseMap<StubTarget, SmallVector<unsigned, 1>> Candidates;
  SmallVector<jitlink::Block *> Anchors;
  SmallVector<jitlink::Symbol *> Externals;
  SectionBases Bases;
  unsigned Iterations = 0;
  uint64_t Shared = 0;
  bool Fallback = false, Changed = false;

public:
  StubCompactor(BinaryContext &BC, jitlink::LinkGraph &G,
                ArrayRef<Stub> Reservations, const FragmentMap &Fragments,
                const SectionsMapper &Plan, SymbolResolver Resolve)
      : BC(BC), G(G), Reservations(Reservations), Fragments(Fragments),
        Plan(Plan), Resolve(Resolve), Externals(G.external_symbols()) {
    snapshot();
    addAnchors();
  }

  Error run() {
    // Start with empty reservations. Grow selected forms and discard unused
    // capacity until every original edge survives the same packed layout.
    for (; Iterations != 16; ++Iterations) {
      if (Error Err = layout()) {
        consumeError(std::move(Err));
        break;
      }
      if (!route())
        break;
      if (Changed)
        continue;
      for (Slot &S : Slots)
        if (!S.Used && S.Size) {
          S.Size = 0;
          Changed = true;
        }
      if (!Changed)
        return emit();
    }
    Fallback = true;
    if (Error Err = restoreReservations())
      return Err;
    return emit();
  }

  Error mapSections(const SectionsMapper &Map) {
    bool Mismatch = false;
    Map([&](const BinarySection &Section, uint64_t Address) {
      if (auto It = Bases.find(&Section); It != Bases.end())
        Mismatch |= It->second != Address;
      JITLinkLinker::assignSectionAddress(G, Section, Address);
    });
    if (Mismatch)
      return make_error<jitlink::JITLinkError>(
          "Final section mapping differs from validated AArch64 stub layout");
    for (const auto *Symbol : Externals)
      if (Symbol->getAddress().getValue() !=
          Resolve(*Symbol->getName(), nullptr))
        return make_error<jitlink::JITLinkError>(
            "Final external symbol address differs from AArch64 stub plan: " +
            *Symbol->getName());
    return Error::success();
  }

private:
  void snapshot() {
    DenseMap<StringRef, BinarySection *> BinarySections;
    for (BinarySection &Section : BC.sections())
      if (Section.hasValidSectionID())
        BinarySections[Section.getSectionID()] = &Section;
    for (auto &Section : G.sections()) {
      auto Blocks = JITLinkLinker::orderedBlocks(Section);
      // Empty continuation markers precede content at the same address.
      llvm::stable_sort(Blocks, [](const auto *L, const auto *R) {
        return L->getAddress() != R->getAddress()
                   ? L->getAddress() < R->getAddress()
                   : L->getSize() == 0 && R->getSize() != 0;
      });
      const uint64_t Size = JITLinkLinker::getSectionAllocationSize(Section);
      Sections.push_back(
          {&Section, BinarySections.lookup(Section.getName()), {}, Size, Size});
      for (auto *Block : Blocks) {
        Sections.back().Blocks.push_back(
            {Block, Block->getAddress().getValue()});
        if (!JITLinkAArch64::isExecutable(Section))
          continue;
        for (auto &Edge : Block->edges()) {
          if (Edge.getKind() != jitlink::aarch64::Branch26PCRel)
            continue;
          // An in-range reference within an indivisible body cannot move.
          if (Edge.getTarget().isDefined() &&
              &Edge.getTarget().getBlock() == Block &&
              isInt<28>(int64_t(Edge.getTarget().getOffset() +
                                Edge.getAddend() - Edge.getOffset())))
            continue;
          Branches.push_back({Block, &Edge});
        }
      }
    }
    for (const Stub &Reservation : Reservations) {
      auto &Edge = branchEdge(Reservation.Branches.front());
      auto *Symbol = Reservation.SlotSym;
      Candidates[stubTarget(Edge)].push_back(Slots.size());
      Slots.push_back({Symbol, &Edge.getTarget(), Edge.getAddend(),
                       &Symbol->getBlock(), Symbol->getOffset()});
      Islands[&Symbol->getBlock()].OriginalContent =
          Symbol->getBlock().getAlreadyMutableContent();
    }
  }

  void addAnchors() {
    if (Slots.empty())
      return;
    for (auto &Section : Sections) {
      if (!JITLinkAArch64::isExecutable(*Section.Section) || !Section.Binary)
        continue;
      DenseSet<jitlink::Block *> Boundaries;
      uint64_t Last = 0;
      jitlink::Block *Previous = nullptr;
      // Keep both sides of large indivisible bodies. A boundary only after
      // each 64 MiB could leave a gap longer than the B instruction's range.
      for (auto [Block, Offset] : Section.Blocks) {
        if (!Fragments.contains(Block))
          continue;
        if (!Previous) {
          Boundaries.insert(Block);
          Last = Offset;
        } else if (Offset - Last >= 64 * 1024 * 1024) {
          const uint64_t Before = Previous->getAddress().getValue();
          if (Before > Last) {
            Boundaries.insert(Previous);
            Last = Before;
          }
          if (Offset - Last >= 64 * 1024 * 1024) {
            Boundaries.insert(Block);
            Last = Offset;
          }
        }
        Previous = Block;
      }
      if (Previous && Section.OriginalSize - Last >= 64 * 1024 * 1024)
        Boundaries.insert(Previous);
      SmallVector<BlockPlacement> Blocks, TailMarkers;
      auto Add = [&](uint64_t Offset) {
        auto &Block = G.createMutableContentBlock(
            *Section.Section, G.allocateBuffer(1).take_front(0),
            orc::ExecutorAddr(Offset), 1, 0);
        Islands.try_emplace(&Block);
        Anchors.push_back(&Block);
        Blocks.push_back({&Block, Offset});
      };
      for (auto Placement : Section.Blocks) {
        if (!Placement.Block->getSize() &&
            Placement.OriginalOffset == Section.OriginalSize) {
          TailMarkers.push_back(Placement);
          continue;
        }
        if (Boundaries.contains(Placement.Block))
          Add(Placement.OriginalOffset);
        Blocks.push_back(Placement);
      }
      Add(Section.OriginalSize);
      llvm::append_range(Blocks, TailMarkers);
      Section.Blocks = std::move(Blocks);
    }
  }

  void sizeIslands() {
    for (auto &[Block, Island] : Islands)
      Island.Size = 0;
    for (const Slot &S : Slots)
      Islands[&S.Symbol->getBlock()].Size += S.Size;
    for (auto &[Block, Island] : Islands) {
      if (Island.OriginalContent.empty()) {
        Island.Storage.resize(std::max<uint64_t>(1, Island.Size), 0);
        Block->setMutableContent(Island.Storage);
        Block->setAlignment(Island.Size ? 4 : 1);
      } else {
        Block->setMutableContent(Island.OriginalContent);
      }
      Island.Size = 0;
    }
    for (Slot &S : Slots) {
      auto &Offset = Islands[&S.Symbol->getBlock()].Size;
      S.Symbol->setOffset(Fallback ? S.OriginalOffset : Offset);
      S.Symbol->setSize(S.Size);
      Offset += S.Size;
    }
    for (auto &[Block, Island] : Islands) {
      auto Content = Island.OriginalContent.empty()
                         ? MutableArrayRef<char>(Island.Storage)
                         : Island.OriginalContent;
      Block->setMutableContent(Content.take_front(
          Fallback && !Island.OriginalContent.empty() ? Content.size()
                                                      : Island.Size));
    }
  }

  Error layout() {
    sizeIslands();
    for (auto &Section : Sections) {
      uint64_t Offset = 0;
      for (auto [Block, OriginalOffset] : Section.Blocks)
        if (Fallback || !JITLinkAArch64::isExecutable(*Section.Section))
          Block->setAddress(orc::ExecutorAddr(OriginalOffset));
        else
          Offset = placeFragment(*Block, Offset, Fragments);
      Section.Size = JITLinkLinker::getSectionAllocationSize(*Section.Section);
      if (Section.Size > Section.OriginalSize + Reservations.size() * 20)
        return make_error<jitlink::JITLinkError>(
            "AArch64 compaction exceeded original section capacity");
      if (Section.Binary)
        Section.Binary->updateContents(Section.Binary->getOutputData(),
                                       Section.Size);
    }
    Bases.clear();
    Plan([&](const BinarySection &Section, uint64_t Address) {
      Bases[&Section] = Address;
    });
    for (auto &Section : Sections) {
      Section.Base = Bases.lookup(Section.Binary);
      for (auto [Block, Offset] : Section.Blocks)
        Block->setAddress(Block->getAddress() + Section.Base);
    }
    for (auto *Symbol : Externals)
      Symbol->getAddressable().setAddress(
          orc::ExecutorAddr(Resolve(*Symbol->getName(), &Bases)));
    return Error::success();
  }

  unsigned form(const Slot &S) const {
    const auto Target = S.Next < 0 ? S.Target->getAddress() + S.Addend
                                   : Slots[S.Next].Symbol->getAddress();
    return stubSize(S.Symbol->getAddress(), Target);
  }

  int buildChain(const Branch &B) {
    SmallVector<jitlink::Block *, 4> Path;
    auto Current = B.source(), Target = B.target();
    while (!inBranchRange(Current, Target)) {
      if (B.isCall() && !Path.empty())
        return -1; // Match relax-exp's default one-bridge call budget.
      jitlink::Block *Best = nullptr;
      for (auto *Anchor : Anchors) {
        const auto Address = Anchor->getAddress();
        const bool Between = Current < Target
                                 ? Current < Address && Address < Target
                                 : Target < Address && Address < Current;
        if (Between && inBranchRange(Current, Address) &&
            (!Best || (Current < Target ? Address > Best->getAddress()
                                        : Address < Best->getAddress())))
          Best = Anchor;
      }
      if (!Best)
        return -1;
      Path.push_back(Best);
      Current = Best->getAddress();
    }
    if (Path.empty())
      return -1;
    auto &ForTarget = Candidates[stubTarget(*B.Edge)];
    auto Free = llvm::find_if(ForTarget, [&](unsigned Index) {
      return !Slots[Index].Size && Slots[Index].OriginalBlock;
    });
    const int First = Free == ForTarget.end() ? -1 : int(*Free);
    int Next = -1;
    for (size_t P = Path.size(); P-- != 0;) {
      int Index = First;
      if (P == 0 && First >= 0) {
        G.transferDefinedSymbol(*Slots[Index].Symbol, *Path[P], 0, 0);
      } else {
        auto &Symbol = G.addAnonymousSymbol(*Path[P], 0, 0, true, true);
        Index = Slots.size();
        Slots.push_back(
            {&Symbol, &B.Edge->getTarget(), B.Edge->getAddend(), nullptr, 0});
        ForTarget.push_back(Index);
      }
      Slot &S = Slots[Index];
      S.Size = 4;
      S.Bridge = true;
      S.Next = Next;
      Next = Index;
    }
    Changed = true;
    return Next;
  }

  int selectStub(const Branch &B) {
    int Best = -1;
    unsigned BestForm = 21;
    if (auto It = Candidates.find(stubTarget(*B.Edge)); It != Candidates.end())
      for (unsigned Index : It->second) {
        const Slot &S = Slots[Index];
        if ((Fallback && !S.OriginalBlock) ||
            !inBranchRange(B.source(), S.Symbol->getAddress()))
          continue;
        const unsigned Size = Fallback ? form(S) : std::max(form(S), S.Size);
        if (Size < BestForm) {
          Best = Index;
          BestForm = Size;
        }
      }
    if (!Fallback && BestForm > 4)
      if (int Chain = buildChain(B); Chain >= 0)
        return Chain;
    return Best;
  }

  bool route() {
    Changed = false;
    Shared = 0;
    for (Slot &S : Slots)
      S.Used = false;
    SmallVector<std::pair<unsigned, uint64_t>> Work;
    for (auto [Index, B] : llvm::enumerate(Branches)) {
      B.Slot = -1;
      if (!inBranchRange(B.source(), B.target()))
        Work.emplace_back(Index, B.source() < B.target()
                                     ? B.target() - B.source()
                                     : B.source() - B.target());
    }
    // Far references establish reusable chains before nearer callers.
    llvm::stable_sort(
        Work, [](const auto &L, const auto &R) { return L.second > R.second; });
    DenseMap<uint64_t, SmallVector<unsigned, 1>> Active;
    for (auto [Index, Distance] : Work) {
      Branch &B = Branches[Index];
      auto &Available = Active[B.target().getValue()];
      auto Reuse = llvm::find_if(llvm::reverse(Available), [&](unsigned I) {
        return inBranchRange(B.source(), Slots[I].Symbol->getAddress()) &&
               (B.isCall() || Slots[I].Size == 4 || Fallback);
      });
      B.Slot = Reuse != Available.rend() ? int(*Reuse) : selectStub(B);
      if (B.Slot < 0)
        return false;
      Shared += Slots[B.Slot].Used;
      // Keep and expose the complete suffix, including a reused chain entry.
      for (int I = B.Slot; I >= 0; I = Slots[I].Next) {
        Slot &S = Slots[I];
        const unsigned Needed = form(S);
        if ((S.Bridge && Needed != 4) ||
            (Needed == 20 && !BC.HasFixedLoadAddress))
          return false;
        if (!Fallback && Needed > S.Size) {
          S.Size = Needed;
          Changed = true;
        }
        if (!S.Used)
          Available.push_back(I);
        S.Used = true;
      }
    }
    return true;
  }

  Error restoreReservations() {
    for (auto &[Block, Island] : Islands)
      if (!Island.OriginalContent.empty())
        Block->setMutableContent(Island.OriginalContent);
    for (Slot &S : Slots) {
      S.Size = S.OriginalBlock ? 20 : 0;
      S.Bridge = false;
      S.Next = -1;
      if (S.OriginalBlock)
        G.transferDefinedSymbol(*S.Symbol, *S.OriginalBlock, S.OriginalOffset,
                                20);
    }
    if (Error Err = layout())
      return Err;
    if (!route())
      return make_error<jitlink::JITLinkError>(
          "Cannot preserve AArch64 branch reach in original reserved layout");
    return Error::success();
  }

  void pack() {
    for (auto &Section : Sections) {
      if (!JITLinkAArch64::isExecutable(*Section.Section) || !Section.Binary)
        continue;
      auto *Buffer = reinterpret_cast<char *>(
          const_cast<uint8_t *>(Section.Binary->getOutputData()));
      // Copy once through a temporary buffer: in-place packing could destroy
      // later source blocks. Initialize only inter-block gaps as NOPs.
      SmallVector<char, 0> Content(Section.Size, 0);
      fillNops(Content);
      for (auto [Block, OriginalOffset] : Section.Blocks)
        if (Block->getSize())
          memcpy(Content.data() +
                     (Block->getAddress().getValue() - Section.Base),
                 Block->getContent().data(), Block->getSize());
      if (Section.Size)
        memcpy(Buffer, Content.data(), Section.Size);
      for (auto [Block, OriginalOffset] : Section.Blocks)
        Block->setMutableContent(
            {Buffer + (Block->getAddress().getValue() - Section.Base),
             Block->getSize()});
    }
  }

  Error emit() {
    pack();
    unsigned Direct = 0, Short = 0, Long = 0, Used = 0;
    uint64_t StubBytes = 0, Before = 0, After = 0;
    for (const Slot &S : Slots) {
      StubBytes += S.Size;
      if (!S.Used)
        continue;
      auto Size = materializeStub(
          *S.Symbol, *(S.Next < 0 ? S.Target : Slots[S.Next].Symbol),
          S.Next < 0 ? S.Addend : 0, BC.HasFixedLoadAddress,
          Fallback ? form(S) : S.Size);
      if (!Size)
        return Size.takeError();
      ++Used;
      Direct += *Size == 4;
      Short += *Size == 12;
      Long += *Size == 20;
    }
    for (Branch &B : Branches)
      if (B.Slot >= 0) {
        B.Edge->setTarget(*Slots[B.Slot].Symbol);
        B.Edge->setAddend(0);
      }
    for (auto *Symbol : Externals)
      G.makeAbsolute(*Symbol, Symbol->getAddress());
    for (auto &Section : Sections) {
      if (JITLinkAArch64::isExecutable(*Section.Section) && Section.Binary) {
        Before += Section.OriginalSize;
        After += Section.Size;
      }
      for (auto [Block, Offset] : Section.Blocks)
        Block->setAddress(Block->getAddress() - Section.Base);
    }
    BC.outs() << "BOLT-INFO: JITLink compaction: passes=" << Iterations + 1
              << " before-bytes=" << Before << " after-bytes=" << After
              << " removed-bytes=" << int64_t(Before - After)
              << " stub-bytes=" << StubBytes << " fallback=" << Fallback
              << '\n';
    if (opts::PrintStubStats)
      BC.outs() << "BOLT-INFO: JITLink stubs: slots=" << Reservations.size()
                << " unused="
                << llvm::count_if(
                       ArrayRef(Slots).take_front(Reservations.size()),
                       [](const Slot &S) { return !S.Used; })
                << " short-adrp=" << Short << " long-mov=" << Long
                << " shared=" << Shared << " direct-b=" << Direct
                << " retained=" << Used << " final-bytes=" << StubBytes << '\n';
    return Error::success();
  }
};

} // namespace

Error JITLinkAArch64::compactStubs(BinaryContext &BC, jitlink::LinkGraph &G,
                                   ArrayRef<Stub> Reservations,
                                   const FragmentMap &Fragments,
                                   const SectionsMapper &Plan,
                                   SymbolResolver Resolve) {
  return StubCompactor(BC, G, Reservations, Fragments, Plan, Resolve).run();
}

Error JITLinkAArch64::prepare() {
  if (Error Err = mergeFragments(G, BC.EmittedFragmentSections, Fragments))
    return Err;
  auto Reserved = reserveStubs(G, Fragments, &BC.outs());
  if (!Reserved)
    return Reserved.takeError();
  Stubs = std::move(*Reserved);
  return Error::success();
}

Error JITLinkAArch64::mapSections(const SectionsMapper &Map,
                                  const SectionsMapper &Plan,
                                  SymbolResolver Resolve) {
  if (!Plan) {
    Map([&](const BinarySection &Section, uint64_t Address) {
      JITLinkLinker::assignSectionAddress(G, Section, Address);
    });
    return Error::success();
  }
  StubCompactor Compactor(BC, G, Stubs, Fragments, Plan, Resolve);
  if (Error Err = Compactor.run())
    return Err;
  return Compactor.mapSections(Map);
}

Error JITLinkAArch64::finalize() {
  unsigned Direct = 0, Short = 0, Long = 0, Shared = 0;
  DenseMap<uint64_t, SmallVector<jitlink::Symbol *, 1>> Materialized;
  for (const Stub &Reservation : Stubs)
    for (const auto &Branch : Reservation.Branches) {
      auto &Edge = branchEdge(Branch);
      const auto Source = Branch.SourceBlock->getFixupAddress(Edge);
      const auto Target = Edge.getTarget().getAddress() + Edge.getAddend();
      if (inBranchRange(Source, Target))
        continue;
      auto &Available = Materialized[Target.getValue()];
      auto Reuse = llvm::find_if(llvm::reverse(Available), [&](const auto *S) {
        return inBranchRange(Source, S->getAddress());
      });
      auto *Symbol = Reservation.SlotSym;
      if (Reuse != Available.rend()) {
        Symbol = *Reuse;
        ++Shared;
      } else {
        if (!inBranchRange(Source, Symbol->getAddress()))
          return make_error<jitlink::JITLinkError>(
              "Reserved AArch64 branch stub is out of Branch26 range");
        auto Size = materializeStub(*Symbol, Edge.getTarget(), Edge.getAddend(),
                                    BC.HasFixedLoadAddress);
        if (!Size)
          return Size.takeError();
        Direct += *Size == 4;
        Short += *Size == 12;
        Long += *Size == 20;
        Available.push_back(Symbol);
      }
      Edge.setTarget(*Symbol);
      Edge.setAddend(0);
    }
  if (opts::PrintStubStats)
    BC.outs() << "BOLT-INFO: JITLink stubs: slots=" << Stubs.size()
              << " unused=" << Stubs.size() - Direct - Short - Long
              << " short-adrp=" << Short << " long-mov=" << Long
              << " shared=" << Shared << " direct-b=" << Direct << '\n';
  return Error::success();
}

Error JITLinkAArch64::verifyBranch26Range(jitlink::LinkGraph &G) {
  LLVM_DEBUG({
    uint64_t Total = 0, OutOfRange = 0;
    for (auto *Block : G.blocks())
      for (const auto &Edge : Block->edges())
        if (Edge.getKind() == jitlink::aarch64::Branch26PCRel) {
          ++Total;
          OutOfRange +=
              !inBranchRange(Block->getFixupAddress(Edge),
                             Edge.getTarget().getAddress() + Edge.getAddend());
        }
    dbgs() << "BOLT-DEBUG: " << Total << " branches detected before fixups.\n"
           << "BOLT-DEBUG: " << OutOfRange
           << " branch destinations are unreachable.\n";
  });
  return Error::success();
}
