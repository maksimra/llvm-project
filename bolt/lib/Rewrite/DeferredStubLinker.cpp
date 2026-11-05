//===- DeferredStubLinker.cpp - Deferred AArch64 branch stubs -------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "bolt/Rewrite/DeferredStubLinker.h"
#include "bolt/Core/BinaryContext.h"
#include "bolt/Rewrite/JITLinkLinker.h"
#include "bolt/Utils/CommandLineOpts.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ExecutionEngine/JITLink/aarch64.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/MathExtras.h"
#include <limits>

#define DEBUG_TYPE "bolt"

namespace llvm {
namespace bolt {
namespace {

/// Target's {Addressable + Offset}
using AArch64StubTarget = std::pair<const jitlink::Addressable *, uint64_t>;

AArch64StubTarget getAArch64StubTarget(const jitlink::Edge &Edge) {
  const auto &Target = Edge.getTarget();
  if (Target.isAbsolute())
    return {nullptr, (Target.getAddress() + Edge.getAddend()).getValue()};
  return {&Target.getAddressable(), Target.getOffset() + Edge.getAddend()};
}

/// Return true for a short (ADRP) stub and false for a long (MOV) stub.
Expected<bool> materializeAArch64Stub(jitlink::Symbol &SlotSym,
                                      jitlink::Symbol &Target,
                                      jitlink::Edge::AddendT Addend,
                                      bool HasFixedLoadAddress) {
  const auto TargetAddress = Target.getAddress() + Addend;

  const int64_t PageDelta =
      static_cast<int64_t>(TargetAddress.getValue() >> 12) -
      static_cast<int64_t>(SlotSym.getAddress().getValue() >> 12);
  const bool UseADRP = isInt<21>(PageDelta);
  if (!UseADRP && !HasFixedLoadAddress)
    return make_error<jitlink::JITLinkError>(
        "Cannot materialize AArch64 branch stub at 0x" +
        Twine::utohexstr(SlotSym.getAddress().getValue()) + " for target 0x" +
        Twine::utohexstr(TargetAddress.getValue()) +
        ": target is outside ADRP range and the binary has no fixed load "
        "address");

  auto &SlotBlock = SlotSym.getBlock();
  const auto SlotOffset = SlotSym.getOffset();
  auto Slot = SlotBlock.getAlreadyMutableContent().slice(SlotOffset, 20);
  if (UseADRP) {
    support::endian::write32le(Slot.data(), 0x90000010);     // ADRP x16
    support::endian::write32le(Slot.data() + 4, 0x91000210); // ADD x16, x16
    support::endian::write32le(Slot.data() + 8, 0xd61f0200); // BR x16
    SlotBlock.addEdge(jitlink::aarch64::Page21, SlotOffset, Target, Addend);
    SlotBlock.addEdge(jitlink::aarch64::PageOffset12, SlotOffset + 4, Target,
                      Addend);

    // Leave the final eight bytes as reserved NOPs.
  } else {
    for (unsigned I = 0; I != 4; ++I) {
      const uint32_t Imm16 = (TargetAddress.getValue() >> (16 * I)) & 0xffff;
      // MOVZ x16 for the first chunk, MOVK x16 for the rest.
      const uint32_t Opcode = I == 0 ? 0xd2800000 : 0xf2800000;
      const uint32_t Instr = Opcode | (I << 21) | (Imm16 << 5) | 16;
      support::endian::write32le(Slot.data() + 4 * I, Instr);
    }
    support::endian::write32le(Slot.data() + 16, 0xd61f0200); // BR x16
  }
  LLVM_DEBUG(dbgs() << "BOLT-DEBUG: " << (UseADRP ? "short" : "long")
                    << " jump stub was materialized at 0x"
                    << Twine::utohexstr(SlotSym.getAddress().getValue())
                    << ", target 0x"
                    << Twine::utohexstr(TargetAddress.getValue()) << '\n');
  return UseADRP;
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
                           DeferredStubLinker::FragmentMap &FragmentsByBlock) {
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

Error DeferredStubLinker::mergeFragmentSections(
    jitlink::LinkGraph &G, ArrayRef<EmittedFragmentSection> Fragments,
    FragmentMap &FragmentsByBlock) {
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
/// The last continuation is placed when its guard is processed.
jitlink::Block *
addFallThroughGuards(jitlink::LinkGraph &G, ArrayRef<jitlink::Block *> Blocks,
                     const DeferredStubLinker::FragmentMap &FragmentsByBlock) {
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
  std::vector<DeferredStubLinker::Stub> &Stubs;
  DenseMap<AArch64StubTarget, size_t> SharedStubs;
  const DeferredStubLinker::FragmentMap &FragmentsByBlock;

public:
  AArch64StubPlacer(jitlink::LinkGraph &G, jitlink::Section &Section,
                    std::vector<DeferredStubLinker::Stub> &Stubs,
                    const DeferredStubLinker::FragmentMap &FragmentsByBlock)
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
        const uint64_t End = placeBlock(*Block, Offset);
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
      if (Error Err = reserveBatch(Batch, Offset, EndMarker))
        return Err;
      ++NumBatches;
      NumIslands += Stubs.size() != FirstNewStub;
      B = C;
    }
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
  uint64_t placeBlock(jitlink::Block &Block, uint64_t Offset) const {
    Offset = jitlink::alignToBlock(Offset, Block);
    if (auto It = FragmentsByBlock.find(&Block); It != FragmentsByBlock.end()) {
      const auto &Fragment = *It->second;
      // The block's ELF alignment preserves every alignment already resolved
      // inside its body by MC. Only the entry preference has a byte budget.
      assert(Block.getAlignmentOffset() == 0 &&
             "Emitted fragments must have zero alignment offset");
      const uint64_t Padding =
          offsetToAlignment(Offset, Align(Fragment.PreferredAlignment));
      if (Padding <= Fragment.MaxAlignmentBytes)
        Offset += Padding;
    }
    Block.setAddress(orc::ExecutorAddr(Offset));
    return Offset + Block.getSize();
  }

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

  Error reserveBatch(ArrayRef<jitlink::Block *> Blocks, uint64_t &Offset,
                     jitlink::Block *EndMarker) {
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
        if (Edge->getTarget().isDefined() &&
            &Edge->getTarget().getBlock() == EndMarker) {
          // The section-end guard is the last edge of the last batch. All
          // other reservations are final, so its target can be placed now.
          JITLinkLinker::layoutBlocks(
              {EndMarker}, Island ? IslandOffset + Island->getSize() : Offset);
        }
        if (isProvenDirect(*Edge, Source))
          continue;
        auto [It, Inserted] =
            SharedStubs.try_emplace(getAArch64StubTarget(*Edge), Stubs.size());
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
      for (size_t I = 0; I < Content.size(); I += 4)
        support::endian::write32le(Content.data() + I, 0xd503201f); // NOP
      Island->setMutableContent(Content);
      Offset = IslandOffset + Content.size();
    }
    return Error::success();
  }
};

} // namespace

Expected<std::vector<DeferredStubLinker::Stub>>
DeferredStubLinker::reserveStubs(jitlink::LinkGraph &G,
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

Error DeferredStubLinker::materialize(jitlink::LinkGraph &G) {
  uint64_t NumShortStubs = 0, NumLongStubs = 0, NumSharedStubs = 0;

  DenseMap<uint64_t, SmallVector<jitlink::Symbol *, 1>> MaterializedStubs;
  for (auto &Record : Stubs) {
    for (const auto &Branch : Record.Branches) {
      for (jitlink::Edge &Edge :
           Branch.SourceBlock->edges_at(Branch.SourceOffset)) {
        if (Edge.getKind() != jitlink::aarch64::Branch26PCRel)
          continue;

        const auto SourceAddress = Branch.SourceBlock->getFixupAddress(Edge);
        const auto TargetAddress =
            Edge.getTarget().getAddress() + Edge.getAddend();
        const int64_t Displacement = TargetAddress - SourceAddress;
        if (isInt<28>(Displacement))
          continue;

        auto &TargetStubs = MaterializedStubs[TargetAddress.getValue()];
        auto It =
            llvm::find_if(llvm::reverse(TargetStubs), [&](const auto *Stub) {
              return isInt<28>(int64_t(Stub->getAddress() - SourceAddress));
            });
        if (It != TargetStubs.rend()) {
          Edge.setTarget(**It);
          Edge.setAddend(0);
          ++NumSharedStubs;
          continue;
        }

        const int64_t SlotDisplacement =
            Record.SlotSym->getAddress() - SourceAddress;
        if (!isInt<28>(SlotDisplacement))
          return make_error<jitlink::JITLinkError>(
              "Reserved AArch64 branch stub is out of Branch26 range");

        auto IsShort =
            materializeAArch64Stub(*Record.SlotSym, Edge.getTarget(),
                                   Edge.getAddend(), BC.HasFixedLoadAddress);
        if (!IsShort)
          return IsShort.takeError();
        if (*IsShort)
          ++NumShortStubs;
        else
          ++NumLongStubs;
        TargetStubs.push_back(Record.SlotSym);

        Edge.setTarget(*Record.SlotSym);
        Edge.setAddend(0);
      }
    }
  }

  if (opts::PrintStubStats)
    BC.outs() << "BOLT-INFO: JITLink stubs: slots=" << Stubs.size()
              << " unused=" << Stubs.size() - NumShortStubs - NumLongStubs
              << " short-adrp=" << NumShortStubs << " long-mov=" << NumLongStubs
              << " shared=" << NumSharedStubs << '\n';

  return Error::success();
}

void DeferredStubLinker::addPasses(jitlink::PassConfiguration &Config) {
  Config.PostPrunePasses.push_back([this](jitlink::LinkGraph &G) -> Error {
    FragmentMap FragmentsByBlock;
    if (Error Err = mergeFragmentSections(G, BC.EmittedFragmentSections,
                                          FragmentsByBlock))
      return Err;
    auto Reserved = reserveStubs(G, FragmentsByBlock, &BC.outs());
    if (!Reserved)
      return Reserved.takeError();
    Stubs = std::move(*Reserved);

    // Executable sections were placed with their islands. Data uses the
    // ordinary layout, shared with the non-deferred linker path.
    for (auto &Section : G.sections())
      if ((Section.getMemProt() & orc::MemProt::Exec) == orc::MemProt::None)
        JITLinkLinker::layoutBlocks(JITLinkLinker::orderedBlocks(Section));
    return Error::success();
  });
  Config.PreFixupPasses.push_back(
      [this](jitlink::LinkGraph &G) { return materialize(G); });
}

} // namespace bolt
} // namespace llvm
