//===- bolt/Rewrite/JITLinkLinker.cpp - BOLTLinker using JITLink ----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "bolt/Rewrite/JITLinkLinker.h"
#include "bolt/Core/BinaryContext.h"
#include "bolt/Core/BinaryData.h"
#include "bolt/Core/BinarySection.h"
#include "bolt/Utils/CommandLineOpts.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ExecutionEngine/JITLink/ELF_riscv.h"
#include "llvm/ExecutionEngine/JITLink/JITLink.h"
#include "llvm/ExecutionEngine/JITLink/aarch64.h"
#include "llvm/ExecutionEngine/Orc/Shared/ExecutorAddress.h"
#include "llvm/ExecutionEngine/Orc/Shared/ExecutorSymbolDef.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/MathExtras.h"
#include <limits>

#define DEBUG_TYPE "bolt"

namespace llvm {
namespace bolt {

namespace {

[[maybe_unused]] std::string formatSignedHex(int64_t Value) {
  const uint64_t Magnitude = Value < 0 ? -uint64_t(Value) : uint64_t(Value);
  return ((Value < 0 ? "-0x" : "0x") + Twine::utohexstr(Magnitude)).str();
}

/// Target's {Addressable + Offset}
using AArch64StubTarget = std::pair<const jitlink::Addressable *, uint64_t>;

AArch64StubTarget getAArch64StubTarget(const jitlink::Edge &Edge) {
  const auto &Target = Edge.getTarget();
  if (Target.isAbsolute())
    return {nullptr, (Target.getAddress() + Edge.getAddend()).getValue()};
  return {&Target.getAddressable(), Target.getOffset() + Edge.getAddend()};
}

bool hasSymbols(const jitlink::Block &B) {
  return llvm::any_of(B.getSection().symbols(),
                      [&B](const auto &S) { return &S->getBlock() == &B; });
}

/// Liveness in JITLink is based on symbols so sections that do not contain
/// any symbols will always be pruned. This pass adds anonymous symbols to
/// needed sections to prevent pruning.
Error markSectionsLive(jitlink::LinkGraph &G) {
  for (auto &Section : G.sections()) {
    // We only need allocatable sections.
    if (Section.getMemLifetime() == orc::MemLifetime::NoAlloc)
      continue;

    // Skip empty sections.
    if (llvm::none_of(Section.blocks(),
                      [](const auto *Block) { return Block->getSize() != 0; }))
      continue;

    for (auto *Block : Section.blocks()) {
      // No need to add symbols if it already has some.
      if (hasSymbols(*Block))
        continue;

      G.addAnonymousSymbol(*Block, /*Offset=*/0, /*Size=*/0,
                           /*IsCallable=*/false, /*IsLive=*/true);
    }
  }

  return jitlink::markAllSymbolsLive(G);
}

void reassignSectionAddress(jitlink::LinkGraph &LG,
                            const BinarySection &BinSection, uint64_t Address) {
  auto *JLSection = LG.findSectionByName(BinSection.getSectionID());
  assert(JLSection && "cannot find section in LinkGraph");

  JITLinkLinker::assignBlockAddresses(
      *JLSection, BinSection.getOutputContents().data(), Address);
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

} // anonymous namespace

struct JITLinkLinker::Context : jitlink::JITLinkContext {
  JITLinkLinker &Linker;
  JITLinkLinker::SectionsMapper MapSections;

  std::vector<AArch64Stub> StubRecords;

  Context(JITLinkLinker &Linker, JITLinkLinker::SectionsMapper MapSections)
      : JITLinkContext(&Linker.Dylib), Linker(Linker),
        MapSections(MapSections) {}

  jitlink::JITLinkMemoryManager &getMemoryManager() override {
    return *Linker.MM;
  }

  bool shouldAddDefaultTargetPasses(const Triple &TT) const override {
    // The default passes manipulate DWARF sections in a way incompatible with
    // BOLT.
    // TODO check if we can actually use these passes to remove some of the
    // DWARF manipulation done in BOLT.
    return false;
  }

  Error modifyPassConfig(jitlink::LinkGraph &G,
                         jitlink::PassConfiguration &Config) override {
    Config.PrePrunePasses.push_back(markSectionsLive);

    // Only the main ELF object has BOLT's fragment sections and fall-through
    // guards. Runtime objects must retain their original block layout.
    const bool DeferredStubs = opts::DoNotUseStubs &&
                               G.getTargetTriple().isAArch64() &&
                               Linker.BC.isELF() && !Linker.MM->ObjectsLoaded;
    Config.PostPrunePasses.push_back([this, DeferredStubs](auto &G) -> Error {
      if (DeferredStubs) {
        DenseSet<jitlink::Block *> FallThroughBlocks;
        if (Error Err = mergeFragmentSections(
                G, Linker.BC.EmittedFragmentSections, FallThroughBlocks))
          return Err;
        auto Stubs =
            reserveAArch64Stubs(G, FallThroughBlocks, &Linker.BC.outs());
        if (!Stubs)
          return Stubs.takeError();
        StubRecords = std::move(*Stubs);
      }

      // Allocation copies these final section-relative placements verbatim.
      for (auto &Section : G.sections())
        if (!DeferredStubs ||
            (Section.getMemProt() & orc::MemProt::Exec) == orc::MemProt::None)
          layoutBlocks(orderedBlocks(Section));
      return Error::success();
    });

    if (DeferredStubs)
      Config.PreFixupPasses.push_back(
          [this](auto &G) { return repairEdges(G); });

    Config.PostAllocationPasses.push_back([this](auto &G) {
      MapSections([&G](const BinarySection &Section, uint64_t Address) {
        reassignSectionAddress(G, Section, Address);
      });
      return Error::success();
    });

    if (G.getTargetTriple().isRISCV()) {
      Config.PostAllocationPasses.push_back(
          jitlink::createRelaxationPass_ELF_riscv());
    }

    if (opts::VerifyBranch26Range && G.getTargetTriple().isAArch64()) {
      Config.PreFixupPasses.push_back([this](auto &G) {
        return verifyAArch64Branch26Range(Linker.BC, G);
      });
    }

    return Error::success();
  }

  Error repairEdges(jitlink::LinkGraph &G) {
    uint64_t NumShortStubs = 0, NumLongStubs = 0, NumSharedStubs = 0;

    DenseMap<uint64_t, SmallVector<jitlink::Symbol *, 1>> MaterializedStubs;
    for (auto &Record : StubRecords) {
      for (const auto &Branch : Record.Branches) {
        for (jitlink::Edge &Edge :
             Branch.SourceBlock->edges_at(Branch.SourceOffset)) {
          if (Edge.getKind() != jitlink::aarch64::Branch26PCRel)
            continue;

          const auto SourceAddress = Branch.SourceBlock->getFixupAddress(Edge);
          const auto TargetAddress =
              Edge.getTarget().getAddress() + Edge.getAddend();
          const int64_t Displacement = TargetAddress - SourceAddress;
          LLVM_DEBUG(dbgs() << "BOLT-DEBUG: edge for "
                            << (Edge.getTarget().hasName()
                                    ? *Edge.getTarget().getName()
                                    : "<anonymous>")
                            << " has " << formatSignedHex(Displacement)
                            << " displacement.\n");

          if (isInt<28>(Displacement))
            continue;

          auto &Stubs = MaterializedStubs[TargetAddress.getValue()];
          auto It = llvm::find_if(llvm::reverse(Stubs), [&](const auto *Stub) {
            return isInt<28>(int64_t(Stub->getAddress() - SourceAddress));
          });
          if (It != Stubs.rend()) {
            Edge.setTarget(**It);
            Edge.setAddend(0);
            ++NumSharedStubs;
            continue;
          }

          const int64_t SlotDisplacement =
              Record.SlotSym->getAddress() - SourceAddress;
          LLVM_DEBUG(dbgs() << "BOLT-DEBUG: that is out of range. And slot "
                               "displacement is "
                            << formatSignedHex(SlotDisplacement) << "\n");

          if (!isInt<28>(SlotDisplacement))
            return make_error<jitlink::JITLinkError>(
                "Reserved AArch64 branch stub is out of Branch26 range");

          auto IsShort = materializeAArch64Stub(
              *Record.SlotSym, Edge.getTarget(), Edge.getAddend(),
              Linker.BC.HasFixedLoadAddress);
          if (!IsShort)
            return IsShort.takeError();
          if (*IsShort)
            ++NumShortStubs;
          else
            ++NumLongStubs;
          Stubs.push_back(Record.SlotSym);

          Edge.setTarget(*Record.SlotSym);
          Edge.setAddend(0);
        }
      }
    }

    if (opts::PrintStubStats)
      Linker.BC.outs() << "BOLT-INFO: JITLink stubs: slots="
                       << StubRecords.size() << " unused="
                       << StubRecords.size() - NumShortStubs - NumLongStubs
                       << " short-adrp=" << NumShortStubs
                       << " long-mov=" << NumLongStubs
                       << " shared=" << NumSharedStubs << '\n';

    return Error::success();
  }

  Error verifyAArch64Branch26Range(BinaryContext &BC,
                                   jitlink::LinkGraph &G) {
    [[maybe_unused]] uint64_t Total = 0;
    [[maybe_unused]] uint64_t OutOfRange = 0;
    for (jitlink::Section &Section : G.sections()) {
      for (jitlink::Block *Block : Section.blocks()) {
        for (const jitlink::Edge &Edge : Block->edges()) {
          if (Edge.getKind() != jitlink::aarch64::Branch26PCRel)
            continue;

          ++Total;
          int64_t Displacement = Edge.getTarget().getAddress() -
                                 Block->getFixupAddress(Edge) +
                                 Edge.getAddend();
          if (!isIntN(BC.MIB->getUncondBranchEncodingSize(), Displacement)) {
            ++OutOfRange;
            LLVM_DEBUG(dbgs() << "BOLT-DEBUG: branch is out of range\n"
                              << "edge at 0x"
                              << Twine::utohexstr(
                                     Block->getFixupAddress(Edge).getValue())
                              << " in " << Section.getName() << " targets 0x"
                              << Twine::utohexstr(
                                     Edge.getTarget().getAddress().getValue())
                              << " and addend is "
                              << Twine::utohexstr(Edge.getAddend()) << "\n");
          }
        }
      }
    }

    LLVM_DEBUG(dbgs() << "BOLT-DEBUG: " << Total
                      << " branches detected before fixups.\n";
               dbgs() << "BOLT-DEBUG: " << OutOfRange
                      << " branch destinations are unreachable.\n");

    return Error::success();
  }

  void notifyFailed(Error Err) override {
    errs() << "BOLT-ERROR: JITLink failed: " << Err << '\n';
    exit(1);
  }

  void
  lookup(const LookupMap &Symbols,
         std::unique_ptr<jitlink::JITLinkAsyncLookupContinuation> LC) override {
    jitlink::AsyncLookupResult AllResults;

    for (const auto &Symbol : Symbols) {
      std::string SymName = (*Symbol.first).str();
      LLVM_DEBUG(dbgs() << "BOLT: looking for " << SymName << "\n");

      if (auto SymInfo = Linker.lookupSymbolInfo(SymName)) {
        LLVM_DEBUG(dbgs() << "Resolved to address 0x"
                          << Twine::utohexstr(SymInfo->Address) << "\n");
        AllResults[Symbol.first] = orc::ExecutorSymbolDef(
            orc::ExecutorAddr(SymInfo->Address), JITSymbolFlags());
        continue;
      }

      if (const BinaryData *I = Linker.BC.getBinaryDataByName(SymName)) {
        uint64_t Address = I->isMoved() && !I->isJumpTable()
                               ? I->getOutputAddress()
                               : I->getAddress();
        LLVM_DEBUG(dbgs() << "Resolved to address 0x"
                          << Twine::utohexstr(Address) << "\n");
        AllResults[Symbol.first] = orc::ExecutorSymbolDef(
            orc::ExecutorAddr(Address), JITSymbolFlags());
        continue;
      }

      if (Linker.BC.isGOTSymbol(SymName)) {
        if (const BinaryData *I = Linker.BC.getGOTSymbol()) {
          uint64_t Address =
              I->isMoved() ? I->getOutputAddress() : I->getAddress();
          LLVM_DEBUG(dbgs() << "Resolved to address 0x"
                            << Twine::utohexstr(Address) << "\n");
          AllResults[Symbol.first] = orc::ExecutorSymbolDef(
              orc::ExecutorAddr(Address), JITSymbolFlags());
          continue;
        }
      }

      LLVM_DEBUG(dbgs() << "Resolved to address 0x0\n");
      AllResults[Symbol.first] =
          orc::ExecutorSymbolDef(orc::ExecutorAddr(0), JITSymbolFlags());
    }

    LC->run(std::move(AllResults));
  }

  Error notifyResolved(jitlink::LinkGraph &G) override {
    for (auto *Symbol : G.defined_symbols()) {
      SymbolInfo Info{Symbol->getAddress().getValue(), Symbol->getSize()};
      auto Name =
          Symbol->hasName() ? (*Symbol->getName()).str() : std::string();
      Linker.Symtab.insert({std::move(Name), Info});
    }

    return Error::success();
  }

  void notifyFinalized(
      jitlink::JITLinkMemoryManager::FinalizedAlloc Alloc) override {
    if (Alloc)
      Linker.Allocs.push_back(std::move(Alloc));
    ++Linker.MM->ObjectsLoaded;
  }
};

JITLinkLinker::JITLinkLinker(BinaryContext &BC,
                             std::unique_ptr<ExecutableFileMemoryManager> MM)
    : BC(BC), MM(std::move(MM)) {}

JITLinkLinker::~JITLinkLinker() { cantFail(MM->deallocate(std::move(Allocs))); }

void JITLinkLinker::loadObject(MemoryBufferRef Obj,
                               SectionsMapper MapSections) {
  auto LG = jitlink::createLinkGraphFromObject(Obj, BC.getSymbolStringPool());
  if (auto E = LG.takeError()) {
    errs() << "BOLT-ERROR: JITLink failed: " << E << '\n';
    exit(1);
  }

  if ((*LG)->getTargetTriple().getArch() != BC.TheTriple->getArch()) {
    errs() << "BOLT-ERROR: linking object with arch "
           << (*LG)->getTargetTriple().getArchName()
           << " into context with arch " << BC.TheTriple->getArchName() << "\n";
    exit(1);
  }

  auto Ctx = std::make_unique<Context>(*this, MapSections);
  jitlink::link(std::move(*LG), std::move(Ctx));
}

std::optional<JITLinkLinker::SymbolInfo>
JITLinkLinker::lookupSymbolInfo(StringRef Name) const {
  auto It = Symtab.find(Name.data());
  if (It == Symtab.end())
    return std::nullopt;

  return It->second;
}

SmallVector<jitlink::Block *, 2>
JITLinkLinker::orderedBlocks(const jitlink::Section &Section) {
  SmallVector<jitlink::Block *, 2> Blocks(Section.blocks());
  llvm::sort(Blocks, [](const auto *LHS, const auto *RHS) {
    return LHS->getAddress() < RHS->getAddress();
  });
  return Blocks;
}

uint64_t JITLinkLinker::layoutBlocks(ArrayRef<jitlink::Block *> Blocks,
                                     uint64_t Offset) {
  for (auto *Block : Blocks) {
    Offset = jitlink::alignToBlock(Offset, *Block);
    Block->setAddress(orc::ExecutorAddr(Offset));
    Offset += Block->getSize();
  }
  return Offset;
}

size_t
JITLinkLinker::getSectionAllocationSize(const jitlink::Section &Section) {
  uint64_t Size = 0;
  for (const auto *Block : Section.blocks())
    Size = std::max(Size, Block->getAddress().getValue() + Block->getSize());
  return Size;
}

void JITLinkLinker::assignBlockAddresses(jitlink::Section &Section,
                                         const char *Buffer, uint64_t Address) {
  for (auto *Block : Section.blocks())
    Block->setAddress(
        orc::ExecutorAddr(Address + (Block->getContent().data() - Buffer)));
}

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
                           DenseSet<jitlink::Block *> &FallThroughBlocks) {
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
  if (Fragment.HasFallThrough) {
    assert(Block->getSize() >= 4 && "Missing emitted fall-through branch");
    FallThroughBlocks.insert(Block);
  }
  G.mergeSections(*Output, Input);
  return Error::success();
}

} // namespace

Error JITLinkLinker::mergeFragmentSections(
    jitlink::LinkGraph &G, ArrayRef<EmittedFragmentSection> Fragments,
    DenseSet<jitlink::Block *> &FallThroughBlocks) {
  LLVM_DEBUG(dbgs() << "BOLT-DEBUG: begin of Fragment Sections merging.\n");

  FragmentBlocksBySection BlocksBySection;
  DenseSet<jitlink::Block *> FragmentBlocks;
  SmallVector<jitlink::Section *> Sections;
  for (const auto &Fragment : Fragments) {
    LLVM_DEBUG(dbgs() << "BOLT-DEBUG: Fragment " << Fragment.Name
                      << " and output section " << Fragment.OutputSection
                      << '\n');
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
                                         BlocksBySection, FallThroughBlocks))
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
jitlink::Block *
addFallThroughGuards(jitlink::LinkGraph &G, ArrayRef<jitlink::Block *> Blocks,
                     const DenseSet<jitlink::Block *> &FallThroughBlocks) {
  jitlink::Block *EndMarker = nullptr;
  for (auto [Index, Block] : llvm::enumerate(Blocks)) {
    if (!FallThroughBlocks.contains(Block))
      continue;
    jitlink::Block *Next;
    if (Index + 1 < Blocks.size()) {
      Next = Blocks[Index + 1];
    } else {
      EndMarker = &G.createZeroFillBlock(Blocks.back()->getSection(), 0,
                                         orc::ExecutorAddr(), 1, 0);
      Next = EndMarker;
    }
    auto &Target = G.addAnonymousSymbol(*Next, 0, 0, true, true);
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
  std::vector<JITLinkLinker::AArch64Stub> &Stubs;
  DenseMap<AArch64StubTarget, size_t> SharedStubs;

public:
  AArch64StubPlacer(jitlink::LinkGraph &G, jitlink::Section &Section,
                    std::vector<JITLinkLinker::AArch64Stub> &Stubs)
      : G(G), Section(Section), Stubs(Stubs) {}

  Error run(const DenseSet<jitlink::Block *> &FallThroughBlocks,
            raw_ostream *Log) {
    auto Blocks = JITLinkLinker::orderedBlocks(Section);
    auto *EndMarker = addFallThroughGuards(G, Blocks, FallThroughBlocks);
    for (auto *Block : Section.blocks())
      Block->setAddress(orc::ExecutorAddr(Unplaced));

    uint64_t Offset = 0;
    for (size_t B = 0, D = 0; B < Blocks.size();) {
      // B..C: current batch. C..D: placed look-ahead. New island goes at D.
      // First extend D, leaving room for the island within reach of B.
      while (D < Blocks.size()) {
        auto *Block = Blocks[D];
        const uint64_t End = JITLinkLinker::layoutBlocks({Block}, Offset);
        if (D != B && alignTo(End, 4) + MaxIslandSize >
                          Blocks[B]->getAddress().getValue() + BranchDistance) {
          Block->setAddress(orc::ExecutorAddr(Unplaced));
          break;
        }
        Offset = End;
        ++D;
      }

      // Select C by code size and worst-case slot count. Keep large fragments
      // whole; reserveBatch checks every actual slot's reach and island size.
      size_t C = B + 1;
      uint64_t NumBranches = countBranches(*Blocks[B]);
      while (C < D &&
             Blocks[C]->getAddress().getValue() + Blocks[C]->getSize() <
                 Blocks[B]->getAddress().getValue() + BatchSize) {
        const uint64_t Next = countBranches(*Blocks[C]);
        if (NumBranches + Next > MaxIslandSize / StubSize)
          break;
        NumBranches += Next;
        ++C;
      }
      const auto Batch = ArrayRef(Blocks).slice(B, C - B);
      const size_t FirstNewStub = Stubs.size();
      if (Error Err = reserveBatch(Batch, Offset))
        return Err;
      if (Log)
        printBatch(*Log, Batch, B, D, FirstNewStub);
      B = C;
    }
    if (EndMarker)
      JITLinkLinker::layoutBlocks({EndMarker}, Offset);
    return Error::success();
  }

private:
  void printBatch(raw_ostream &OS, ArrayRef<jitlink::Block *> Batch,
                  size_t Begin, size_t LookAheadEnd,
                  size_t FirstNewStub) const {
    uint64_t CodeSize = 0;
    for (const auto *Block : Batch)
      CodeSize += Block->getSize();
    const size_t NumNewStubs = Stubs.size() - FirstNewStub;
    OS << "BOLT-INFO: JITLink batch: section=" << Section.getName()
       << " fragments=[" << Begin << ", " << Begin + Batch.size()
       << ") look-ahead-end=" << LookAheadEnd << '\n'
       << "BOLT-INFO:   " << Batch.size() << " fragment(s), " << CodeSize
       << " emitted bytes, section-offsets=[0x"
       << Twine::utohexstr(Batch.front()->getAddress().getValue()) << ", 0x"
       << Twine::utohexstr(Batch.back()->getAddress().getValue() +
                           Batch.back()->getSize())
       << ")\n"
       << "BOLT-INFO:   " << NumNewStubs << " new stub slot(s), "
       << NumNewStubs * StubSize << " reserved bytes";
    if (NumNewStubs)
      OS << ", island-offset=0x"
         << Twine::utohexstr(
                Stubs[FirstNewStub].SlotSym->getAddress().getValue());
    OS << '\n';
  }

  static uint64_t countBranches(const jitlink::Block &Block) {
    return llvm::count_if(Block.edges(), [](const auto &Edge) {
      return Edge.getKind() == jitlink::aarch64::Branch26PCRel;
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

Expected<std::vector<JITLinkLinker::AArch64Stub>>
JITLinkLinker::reserveAArch64Stubs(
    jitlink::LinkGraph &G, const DenseSet<jitlink::Block *> &FallThroughBlocks,
    raw_ostream *Log) {
  std::vector<AArch64Stub> Stubs;
  for (jitlink::Section &Section : G.sections()) {
    if ((Section.getMemProt() & orc::MemProt::Exec) == orc::MemProt::None)
      continue;
    if (Error Err =
            AArch64StubPlacer(G, Section, Stubs).run(FallThroughBlocks, Log))
      return std::move(Err);
  }
  return Stubs;
}

} // namespace bolt
} // namespace llvm
