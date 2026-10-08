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
#include "llvm/ADT/MapVector.h"
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
    if (JITLinkLinker::sectionSize(Section) == 0)
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

  JITLinkLinker::assignBlockAddresses(*JLSection, Address);
}

Error materializeAArch64Stub(jitlink::Symbol &SlotSym, jitlink::Symbol &Target,
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

    outs() << "BOLT-INFO: short jump stub was materialized.\n";
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
    outs() << "BOLT-INFO: long jump stub was materialized.\n";
  }
  return Error::success();
}

} // anonymous namespace

struct JITLinkLinker::Context : jitlink::JITLinkContext {
  JITLinkLinker &Linker;
  JITLinkLinker::SectionsMapper MapSections;

  struct StubRecord {
    jitlink::Block *SourceBlock;
    jitlink::Edge::OffsetT SourceOffset;
    jitlink::Symbol *SlotSym;
  };
  std::vector<StubRecord> StubRecords;

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

    if (opts::DoNotUseStubs && G.getTargetTriple().isAArch64()) {
      if (Linker.BC.isELF() && !Linker.MM->ObjectsLoaded) {
        Config.PostPrunePasses.push_back([this](auto &G) {
          return mergeFragmentSections(G, Linker.BC.EmittedFragmentSections);
        });
      }
      Config.PostPrunePasses.push_back([this](auto &G) {
        return addThunkSpace(G); });

      Config.PreFixupPasses.push_back([this](auto &G) {
        return repairEdges(G); });
    }

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
    for (auto &Record : StubRecords) {
      for (jitlink::Edge &Edge :
           Record.SourceBlock->edges_at(Record.SourceOffset)) {
        if (Edge.getKind() != jitlink::aarch64::Branch26PCRel)
          continue;

        const auto SourceAddress = Record.SourceBlock->getFixupAddress(Edge);
        const auto TargetAddress =
            Edge.getTarget().getAddress() + Edge.getAddend();
        const int64_t Displacement = TargetAddress - SourceAddress;
        outs() << "BOLT-INFO: edge for "
               << (Edge.getTarget().hasName() ? *Edge.getTarget().getName()
                                              : "<anonymous>")
               << " has " << Displacement << " displacement.\n";

        if (isInt<28>(Displacement))
          continue;

        const int64_t SlotDisplacement = Record.SlotSym->getAddress() -
                                         SourceAddress;
        outs() << "BOLT-INFO: that is out of range. And slot displacement is "
               << SlotDisplacement << "\n";

        if (!isInt<28>(SlotDisplacement))
          return make_error<jitlink::JITLinkError>(
              "Reserved AArch64 branch stub is out of Branch26 range");

        if (Error Err = materializeAArch64Stub(
                *Record.SlotSym, Edge.getTarget(), Edge.getAddend(),
                Linker.BC.HasFixedLoadAddress))
          return Err;

        Edge.setTarget(*Record.SlotSym);
        Edge.setAddend(0);
      }
    }

    return Error::success();
  }

  Error addThunkSpace(jitlink::LinkGraph &G) {
    for (jitlink::Section &Section : G.sections()) {
      auto Blocks = JITLinkLinker::orderedBlocks(Section);
      for (jitlink::Block *Block : Blocks) {
        std::vector<jitlink::Edge::OffsetT> Offsets;
        for (const jitlink::Edge &Edge : Block->edges()) {
          if (Edge.getKind() != jitlink::aarch64::Branch26PCRel)
            continue;

          outs() << "BOLT-INFO: reserve stub slot for "
                 << (Edge.getTarget().hasName() ? *Edge.getTarget().getName()
                                                : "<anonymous>")
                 << "\n";

          Offsets.emplace_back(Edge.getOffset());
        }

        uint64_t Branch26Num = Offsets.size();
        if (!Branch26Num)
          continue;

        jitlink::Block &stubBlock = G.createMutableContentBlock(Section,
                                                                G.allocateBuffer(20 * Branch26Num),
                                                                Block->getAddress() + Block->getSize(),
                                                                4, 0);

        outs() << "BOLT-INFO: created " << Branch26Num << " slots after block"
                  " for stubs\n";

        auto Content = stubBlock.getAlreadyMutableContent();
        for (size_t Offset = 0; Offset < Content.size(); Offset += 4)
          support::endian::write32le(Content.data() + Offset, /*aarch64 nop code*/0xd503201f);

        for (uint64_t SymbolNum = 0; SymbolNum < Branch26Num; ++SymbolNum) {

          StubRecords.push_back(StubRecord{Block, Offsets[SymbolNum],
                                           &G.addAnonymousSymbol(stubBlock, 20 * SymbolNum,
                                                                 20, true, true)});
        }
      }
    }

    return Error::success();
  }

  Error verifyAArch64Branch26Range(BinaryContext &BC,
                                   jitlink::LinkGraph &G) {
    uint64_t Total = 0;
    uint64_t OutOfRange = 0;
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
            outs() << "BOLT-WARNING: branch is out of range\n"
                   << "edge at 0x"
                   << Twine::utohexstr(Block->getFixupAddress(Edge).getValue())
                   << " in " << Section.getName() << " targets 0x"
                   << Twine::utohexstr(Edge.getTarget().getAddress().getValue())
                   << " and addend is "
                   << Twine::utohexstr(Edge.getAddend()) << "\n";
          }
        }
      }
    }

    outs() << "BOLT-INFO: " << Total << " branches detected before"
              "fixups.\n";
    outs() << "BOLT-INFO: " << OutOfRange << " branch destinations are "
              "unreachable.\n";

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

size_t JITLinkLinker::sectionSize(const jitlink::Section &Section) {
  size_t Size = 0;

  for (const auto *Block : orderedBlocks(Section)) {
    Size = jitlink::alignToBlock(Size, *Block);
    Size += Block->getSize();
  }

  return Size;
}

namespace {

struct FragmentSectionLayout {
  SmallVector<jitlink::Block *> Blocks;
  uint64_t Alignment = 1;
};
using FragmentSectionLayouts =
    MapVector<jitlink::Section *, FragmentSectionLayout>;

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
                           FragmentSectionLayouts &Layouts) {
  if (Fragment.HasFallThrough)
    return make_error<jitlink::JITLinkError>(
        "Cannot independently place AArch64 fragment with fall-through: " +
        Fragment.Name);
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
  auto [It, Inserted] = Layouts.try_emplace(Output);
  auto &Layout = It->second;
  if (Inserted)
    llvm::append_range(Layout.Blocks, JITLinkLinker::orderedBlocks(*Output));
  Layout.Alignment = std::max(Layout.Alignment, Fragment.OutputAlignment);
  llvm::append_range(Layout.Blocks, JITLinkLinker::orderedBlocks(Input));
  G.mergeSections(*Output, Input);
  return Error::success();
}

Error assignFragmentOrderKeys(jitlink::Section &Section,
                              FragmentSectionLayout &Layout) {
  if (Layout.Blocks.empty())
    return Error::success();
  for (auto *Block : Layout.Blocks)
    Layout.Alignment = std::max(Layout.Alignment, Block->getAlignment());
  // The memory manager gets the section alignment from the first block.
  // Only promote that block, not every function to the text section alignment.
  if (Layout.Blocks.front()->getAlignmentOffset())
    return make_error<jitlink::JITLinkError>(
        "Unsupported first block alignment offset in " + Section.getName());
  Layout.Blocks.front()->setAlignment(Layout.Alignment);

  uint64_t Offset = 0;
  for (auto *Block : Layout.Blocks) {
    Offset = jitlink::alignToBlock(Offset, *Block);
    Block->setAddress(orc::ExecutorAddr(Offset));
    // These are ordering keys, not final addresses. Even empty marker blocks
    // need distinct keys, and the existing reservation pass will insert a
    // block at each source block's end. Leave room for those 20-byte slots.
    Offset += std::max<uint64_t>(1, Block->getSize());
    uint64_t NumBranches = llvm::count_if(Block->edges(), [](const auto &E) {
      return E.getKind() == jitlink::aarch64::Branch26PCRel;
    });
    if (NumBranches)
      Offset = alignTo(Offset, 4) + 20 * NumBranches;
  }
  return Error::success();
}

} // namespace

Error JITLinkLinker::mergeFragmentSections(
    jitlink::LinkGraph &G, ArrayRef<EmittedFragmentSection> Fragments) {
  FragmentSectionLayouts Layouts;
  DenseSet<jitlink::Block *> FragmentBlocks;
  SmallVector<jitlink::Section *> Sections;
  for (const auto &Fragment : Fragments) {
    auto *Section = G.findSectionByName(Fragment.Name);
    if (!Section)
      return make_error<jitlink::JITLinkError>(
          "Missing emitted fragment section " + Fragment.Name);
    Sections.push_back(Section);
    for (auto *Block : Section->blocks())
      FragmentBlocks.insert(Block);
  }

  if (Error Err = validateFragmentEdges(G, FragmentBlocks))
    return Err;

  for (auto [Index, Fragment] : llvm::enumerate(Fragments))
    if (Error Err =
            mergeFragmentSection(G, Fragment, *Sections[Index], Layouts))
      return Err;

  for (auto &[Section, Layout] : Layouts)
    if (Error Err = assignFragmentOrderKeys(*Section, Layout))
      return Err;
  return Error::success();
}

void JITLinkLinker::assignBlockAddresses(jitlink::Section &Section,
                                         uint64_t Address) {
  uint64_t BlockOffset = 0;
  for (auto *Block : orderedBlocks(Section)) {
    // Mirror the memory manager's section-relative packing.
    BlockOffset = jitlink::alignToBlock(BlockOffset, *Block);
    Block->setAddress(orc::ExecutorAddr(Address + BlockOffset));
    BlockOffset += Block->getSize();
  }
}

} // namespace bolt
} // namespace llvm
