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
#include "bolt/Rewrite/JITLinkAArch64.h"
#include "bolt/Utils/CommandLineOpts.h"
#include "llvm/ExecutionEngine/JITLink/ELF_riscv.h"
#include "llvm/ExecutionEngine/Orc/Shared/ExecutorSymbolDef.h"

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

} // namespace

struct JITLinkLinker::Context : jitlink::JITLinkContext {
  JITLinkLinker &Linker;
  SectionsMapper MapSections;
  std::unique_ptr<JITLinkAArch64> AArch64;

  Context(JITLinkLinker &Linker, SectionsMapper MapSections)
      : JITLinkContext(&Linker.Dylib), Linker(Linker),
        MapSections(MapSections) {}

  jitlink::JITLinkMemoryManager &getMemoryManager() override {
    return *Linker.MM;
  }

  bool shouldAddDefaultTargetPasses(const Triple &) const override {
    // Default DWARF passes are incompatible with BOLT's metadata rewriting.
    return false;
  }

  Error modifyPassConfig(jitlink::LinkGraph &G,
                         jitlink::PassConfiguration &Config) override {
    Config.PrePrunePasses.push_back(markSectionsLive);
    // Only the main ELF object has fragment sections and fall-through guards.
    if (opts::DoNotUseStubs && G.getTargetTriple().isAArch64() &&
        Linker.BC.isELF() && !Linker.MM->ObjectsLoaded) {
      AArch64 = std::make_unique<JITLinkAArch64>(Linker.BC, G);
      Config.PostPrunePasses.push_back(
          [this](auto &) { return AArch64->prepare(); });
      if (!Linker.PlanSections)
        Config.PreFixupPasses.push_back(
            [this](auto &) { return AArch64->finalize(); });
    }
    Config.PostPrunePasses.push_back([this](auto &G) {
      for (auto &Section : G.sections())
        if (!AArch64 || !JITLinkAArch64::isExecutable(Section))
          layoutBlocks(orderedBlocks(Section));
      return Error::success();
    });
    Config.PostAllocationPasses.push_back([this](auto &G) -> Error {
      if (AArch64)
        return AArch64->mapSections(MapSections, Linker.PlanSections,
                                    [this](StringRef Name, const auto *Bases) {
                                      return resolveExternal(Name, Bases);
                                    });
      MapSections([&](const BinarySection &Section, uint64_t Address) {
        assignSectionAddress(G, Section, Address);
      });
      return Error::success();
    });
    if (G.getTargetTriple().isRISCV())
      Config.PostAllocationPasses.push_back(
          jitlink::createRelaxationPass_ELF_riscv());
    if (opts::VerifyBranch26Range && G.getTargetTriple().isAArch64())
      Config.PreFixupPasses.push_back(JITLinkAArch64::verifyBranch26Range);
    return Error::success();
  }

  void notifyFailed(Error Err) override {
    errs() << "BOLT-ERROR: JITLink failed: " << Err << '\n';
    exit(1);
  }

  uint64_t resolveExternal(
      StringRef Name,
      const DenseMap<const BinarySection *, uint64_t> *Bases = nullptr) {
    if (auto Info = Linker.lookupSymbolInfo(Name))
      return Info->Address;
    const BinaryData *Data = Linker.BC.getBinaryDataByName(Name);
    if (!Data && Linker.BC.isGOTSymbol(Name))
      Data = Linker.BC.getGOTSymbol();
    if (!Data)
      return 0;
    if (!Data->isMoved() || Data->isJumpTable())
      return Data->getAddress();
    if (Bases) {
      if (auto It = Bases->find(&Data->getOutputSection()); It != Bases->end())
        return It->second + Data->getOutputOffset();
    }
    return Data->getOutputAddress();
  }

  void
  lookup(const LookupMap &Symbols,
         std::unique_ptr<jitlink::JITLinkAsyncLookupContinuation> LC) override {
    jitlink::AsyncLookupResult Results;
    for (const auto &Symbol : Symbols)
      Results[Symbol.first] = orc::ExecutorSymbolDef(
          orc::ExecutorAddr(resolveExternal(*Symbol.first)), JITSymbolFlags());
    LC->run(std::move(Results));
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

void JITLinkLinker::assignSectionAddress(jitlink::LinkGraph &G,
                                         const BinarySection &Section,
                                         uint64_t Address) {
  auto *JLSection = G.findSectionByName(Section.getSectionID());
  assert(JLSection && "cannot find section in LinkGraph");
  assignBlockAddresses(*JLSection, Section.getOutputContents().data(), Address);
}

} // namespace bolt
} // namespace llvm
