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
#include "llvm/ExecutionEngine/JITLink/aarch64.h"
#include "llvm/ExecutionEngine/JITLink/ELF_riscv.h"
#include "llvm/ExecutionEngine/JITLink/JITLink.h"
#include "llvm/ExecutionEngine/Orc/Shared/ExecutorAddress.h"
#include "llvm/ExecutionEngine/Orc/Shared/ExecutorSymbolDef.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/MathExtras.h"

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

  auto BlockAddress = Address;
  for (auto *Block : JITLinkLinker::orderedBlocks(*JLSection)) {
    // FIXME it would seem to make sense to align here. However, in
    // non-relocation mode, we simply use the original address of functions
    // which might not be aligned with the minimum alignment used by
    // BinaryFunction (2). Example failing test when aligning:
    // bolt/test/X86/addr32.s
    Block->setAddress(orc::ExecutorAddr(BlockAddress));
    BlockAddress += Block->getSize();
  }
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
        outs() << "BOLT-INFO: edge for " << Edge.getTarget().getName()
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
                 << Edge.getTarget().getName() << "\n";

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

} // namespace bolt
} // namespace llvm
