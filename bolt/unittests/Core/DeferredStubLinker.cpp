//===- bolt/unittest/Core/JITLinkLinker.cpp -------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "bolt/Rewrite/DeferredStubLinker.h"
#include "bolt/Rewrite/JITLinkLinker.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ExecutionEngine/JITLink/JITLink.h"
#include "llvm/ExecutionEngine/JITLink/aarch64.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/MathExtras.h"
#include "gtest/gtest.h"

using namespace llvm;
using namespace llvm::bolt;
using namespace llvm::jitlink;

namespace {

class AArch64StubBatchTest : public testing::Test {
protected:
  static constexpr uint64_t MiB = 1024 * 1024;
  LinkGraph G{"test",
              std::make_shared<orc::SymbolStringPool>(),
              Triple("aarch64-unknown-linux"),
              8,
              SubtargetFeatures(),
              aarch64::getEdgeKindName};
  Section &Text =
      G.createSection(".text", orc::MemProt::Read | orc::MemProt::Exec);
  Symbol &Far = G.addAbsoluteSymbol("far", orc::ExecutorAddr(0x100000000), 0,
                                    Linkage::Strong, Scope::Default, true);
  unsigned Order = 0;

  Expected<std::vector<DeferredStubLinker::Stub>>
  reserve(const DenseSet<Block *> &FallThroughBlocks = {}) {
    EmittedFragmentSection Guard{"", "", 1, true};
    DeferredStubLinker::FragmentMap FragmentsByBlock;
    for (const auto *Block : FallThroughBlocks)
      FragmentsByBlock[Block] = &Guard;
    return DeferredStubLinker::reserveStubs(G, FragmentsByBlock);
  }

  Block &block(uint64_t Size, uint64_t Alignment = 4,
               uint64_t AlignmentOffset = 0) {
    // The planner does not read code bytes. Leave these virtual buffers
    // untouched so even very large layouts use little physical memory.
    return G.createMutableContentBlock(Text, G.allocateBuffer(Size),
                                       orc::ExecutorAddr(Order++), Alignment,
                                       AlignmentOffset);
  }

  void branch(Block &B, uint32_t Offset, Symbol &Target, int64_t Addend = 0) {
    B.addEdge(aarch64::Branch26PCRel, Offset, Target, Addend);
  }

  void deferredBranch(Block &Source, uint32_t SourceOffset, Symbol &Target,
                      uint8_t Bits, uint8_t Size, uint32_t Encoding,
                      std::optional<uint32_t> FallbackOffset,
                      int64_t Addend = 0) {
    auto *Info = G.findSectionByName(DeferredBranchSectionName);
    if (!Info) {
      Info = &G.createSection(DeferredBranchSectionName, orc::MemProt::Read);
      Info->setMemLifetime(orc::MemLifetime::NoAlloc);
    }
    auto Bytes = G.allocateBuffer(DeferredBranchRecordSize);
    llvm::fill(Bytes, 0);
    support::endian::write32le(
        Bytes.data() + offsetof(DeferredBranchRecord, Encoding), Encoding);
    Bytes[offsetof(DeferredBranchRecord, DisplacementBits)] = Bits;
    Bytes[offsetof(DeferredBranchRecord, ReservedSize)] = Size;
    auto &Record =
        G.createMutableContentBlock(*Info, Bytes, orc::ExecutorAddr(), 1, 0);
    auto &Anchor = G.addAnonymousSymbol(Source, 0, 0, true, true);
    // Use section-style anchors plus addends, in non-source order.
    Record.addEdge(aarch64::Pointer64, offsetof(DeferredBranchRecord, Target),
                   Target, Addend);
    if (FallbackOffset)
      Record.addEdge(aarch64::Pointer64,
                     offsetof(DeferredBranchRecord, LongBranch), Anchor,
                     *FallbackOffset);
    Record.addEdge(aarch64::Pointer64, offsetof(DeferredBranchRecord, Source),
                   Anchor, SourceOffset);
  }

  void checkReachability(ArrayRef<DeferredStubLinker::Stub> Stubs) {
    for (const auto &Stub : Stubs)
      for (const auto &Branch : Stub.Branches)
        EXPECT_TRUE(isInt<28>(
            int64_t(Stub.SlotSym->getAddress() -
                    (Branch.SourceBlock->getAddress() + Branch.SourceOffset))));
  }
};

TEST_F(AArch64StubBatchTest, DeferredShortBranchRangeLimits) {
  for (uint8_t Bits : {11, 16, 21}) {
    const int64_t Span = int64_t(1) << (Bits - 1);
    for (int64_t Delta : {-Span - 4, -Span, Span - 4, Span}) {
      auto &Source = block(8);
      Source.setAddress(orc::ExecutorAddr(4 * MiB));
      const uint32_t Encoding = Bits == 11   ? 0xf5000000  // CBGT x0, #0
                                : Bits == 16 ? 0x36000000  // TBZ w0, #0
                                             : 0x54000000; // B.EQ
      auto &Target =
          G.addAbsoluteSymbol(("target" + Twine(Bits) + Twine(Delta)).str(),
                              Source.getAddress() + Delta, 0, Linkage::Strong,
                              Scope::Default, true);
      support::endian::write32le(Source.getMutableContent(G).data(), Encoding);
      deferredBranch(Source, 0, Target, Bits, 8, Encoding, 4);
      auto Imported = DeferredStubLinker::importBranches(G);
      ASSERT_TRUE(bool(Imported)) << toString(Imported.takeError());
      ASSERT_EQ(Imported->size(), 1u);
      EXPECT_EQ(G.findSectionByName(DeferredBranchSectionName), nullptr);
      EXPECT_FALSE(bool(DeferredStubLinker::relaxBranches(G, *Imported)));
      const bool Direct = Delta >= -Span && Delta < Span;
      EXPECT_EQ(llvm::count_if(Source.edges(),
                               [](const auto &Edge) {
                                 return Edge.getKind() ==
                                        aarch64::Branch26PCRel;
                               }),
                Direct ? 0u : 1u);
      if (Direct) {
        const uint32_t Mask = ((1u << (Bits - 2)) - 1) << 5;
        EXPECT_EQ(support::endian::read32le(Source.getContent().data()),
                  Encoding | ((uint32_t(Delta >> 2) << 5) & Mask));
        EXPECT_EQ(support::endian::read32le(Source.getContent().data() + 4),
                  0xd503201f);
      }
    }
  }
}

TEST_F(AArch64StubBatchTest, DeferredBranchReservesLongLegBeforePlacement) {
  auto &Source = block(12);
  auto &Code = G.addAnonymousSymbol(Source, 0, 0, true, true);
  deferredBranch(Source, 0, Code, 11, 12, 0xf5000000, 8, 4);
  auto Imported = DeferredStubLinker::importBranches(G);
  ASSERT_TRUE(bool(Imported)) << toString(Imported.takeError());
  ASSERT_EQ(Source.edges_size(), 1u);
  auto &LongLeg = *Source.edges().begin();
  EXPECT_EQ(LongLeg.getKind(), aarch64::Branch26PCRel);
  EXPECT_EQ(LongLeg.getOffset(), 8u);
  EXPECT_EQ(LongLeg.getAddend(), 4);
  auto Stubs = reserve();
  ASSERT_TRUE(bool(Stubs)) << toString(Stubs.takeError());
  EXPECT_TRUE(Stubs->empty());
  const auto Size = JITLinkLinker::getSectionAllocationSize(Text);
  EXPECT_FALSE(bool(DeferredStubLinker::relaxBranches(G, *Imported)));
  EXPECT_EQ(JITLinkLinker::getSectionAllocationSize(Text), Size);
  EXPECT_TRUE(llvm::none_of(
      Source.edges(), [](const auto &Edge) { return Edge.isRelocation(); }));
  EXPECT_EQ(support::endian::read32le(Source.getContent().data() + 4),
            0xd503201f);
  EXPECT_EQ(support::endian::read32le(Source.getContent().data() + 8),
            0xd503201f);
}

TEST_F(AArch64StubBatchTest, DeferredTailTrampolineHasExactRangeCheck) {
  auto &Source = block(1028);
  deferredBranch(Source, 0, Far, 11, 4, 0xf5000000, 1024);
  auto Imported = DeferredStubLinker::importBranches(G);
  ASSERT_TRUE(bool(Imported)) << toString(Imported.takeError());
  auto Err = DeferredStubLinker::relaxBranches(G, *Imported);
  ASSERT_TRUE(bool(Err));
  EXPECT_NE(toString(std::move(Err)).find("tail trampoline"),
            std::string::npos);
}

TEST_F(AArch64StubBatchTest, DeferredTailTrampolinePreservesBody) {
  auto &Source = block(20);
  llvm::fill(Source.getMutableContent(G), 0);
  support::endian::write32le(Source.getMutableContent(G).data() + 4,
                             0xd61f0020);
  deferredBranch(Source, 0, Far, 11, 4, 0xf5000000, 16);
  auto Imported = DeferredStubLinker::importBranches(G);
  ASSERT_TRUE(bool(Imported)) << toString(Imported.takeError());
  auto Stubs = reserve();
  ASSERT_TRUE(bool(Stubs)) << toString(Stubs.takeError());
  ASSERT_EQ(Stubs->size(), 1u);
  EXPECT_FALSE(bool(DeferredStubLinker::relaxBranches(G, *Imported)));
  EXPECT_EQ(support::endian::read32le(Source.getContent().data()), 0xf5000080);
  EXPECT_EQ(support::endian::read32le(Source.getContent().data() + 4),
            0xd61f0020);
  EXPECT_EQ(Source.edges().begin()->getOffset(), 16u);
  EXPECT_EQ(Source.edges().begin()->getKind(), aarch64::Branch26PCRel);
}

TEST_F(AArch64StubBatchTest, DeferredFixedSizeBranchIsDiagnosed) {
  auto &Source = block(4);
  deferredBranch(Source, 0, Far, 16, 4, 0x36000000, std::nullopt);
  auto Imported = DeferredStubLinker::importBranches(G);
  ASSERT_TRUE(bool(Imported)) << toString(Imported.takeError());
  auto Err = DeferredStubLinker::relaxBranches(G, *Imported);
  ASSERT_TRUE(bool(Err));
  EXPECT_NE(toString(std::move(Err)).find("fixed-size patch"),
            std::string::npos);
}

TEST_F(AArch64StubBatchTest, DeferredBranchRejectsUnalignedTarget) {
  auto &Source = block(8);
  deferredBranch(Source, 0, Far, 16, 8, 0x36000000, 4, 2);
  auto Imported = DeferredStubLinker::importBranches(G);
  ASSERT_TRUE(bool(Imported)) << toString(Imported.takeError());
  auto Err = DeferredStubLinker::relaxBranches(G, *Imported);
  ASSERT_TRUE(bool(Err));
  EXPECT_NE(toString(std::move(Err)).find("Unaligned"), std::string::npos);
}

TEST_F(AArch64StubBatchTest, ForwardIslandSharesAcrossMoreThanBranchDistance) {
  // All 15 callers span 224 MiB. A slot immediately after the first caller
  // could not serve them all, but the forward island at 112 MiB can.
  for (unsigned I = 0; I != 15; ++I)
    branch(block(16 * MiB), 0, Far);
  auto Stubs = reserve();
  ASSERT_TRUE(bool(Stubs)) << toString(Stubs.takeError());
  ASSERT_EQ(Stubs->size(), 1u);
  EXPECT_EQ(Stubs->front().Branches.size(), 15u);
  checkReachability(*Stubs);
  EXPECT_EQ(Stubs->front().SlotSym->getAddress().getValue(), 112 * MiB);
}

TEST_F(AArch64StubBatchTest, NegativeLimitIsInclusive) {
  branch(block(4), 0, Far);
  block(112 * MiB - 4);
  auto &Last = block(128 * MiB - 12);
  branch(Last, 128 * MiB - 20, Far);
  branch(Last, 128 * MiB - 16, Far);
  auto Stubs = reserve();
  ASSERT_TRUE(bool(Stubs)) << toString(Stubs.takeError());
  ASSERT_EQ(Stubs->size(), 2u);
  EXPECT_EQ((*Stubs)[0].Branches.size(), 2u);
  EXPECT_EQ((*Stubs)[1].Branches.size(), 1u);
  checkReachability(*Stubs);
  EXPECT_EQ(int64_t((*Stubs)[0].SlotSym->getAddress() -
                    (Last.getAddress() + 128 * MiB - 20)),
            -int64_t(128 * MiB));
}

TEST_F(AArch64StubBatchTest, UnorderedEdgesRetainReachableBackwardSlot) {
  branch(block(4), 0, Far);
  block(112 * MiB - 4);
  auto &Large = block(200 * MiB);
  // Relocation order need not match instruction order. The late call needs
  // a new slot, while the early call can still reach the previous island.
  branch(Large, 200 * MiB - 4, Far);
  branch(Large, 0, Far);
  auto Stubs = reserve();
  ASSERT_TRUE(bool(Stubs)) << toString(Stubs.takeError());
  ASSERT_EQ(Stubs->size(), 2u);
  EXPECT_EQ((*Stubs)[0].Branches.size(), 2u);
  EXPECT_EQ((*Stubs)[1].Branches.size(), 1u);
  checkReachability(*Stubs);
}

TEST_F(AArch64StubBatchTest, PositiveLimitExcludesOnePast) {
  branch(block(128 * MiB - 4), 0, Far);
  auto Stubs = reserve();
  ASSERT_TRUE(bool(Stubs)) << toString(Stubs.takeError());
  ASSERT_EQ(Stubs->size(), 1u);
  checkReachability(*Stubs);
  EXPECT_EQ(Stubs->front().SlotSym->getAddress().getValue(), 128 * MiB - 4);
}

TEST_F(AArch64StubBatchTest, DiagnoseUnplaceableFragmentBeforeAllocation) {
  branch(block(128 * MiB), 0, Far);
  auto Stubs = reserve();
  ASSERT_FALSE(bool(Stubs));
  EXPECT_NE(toString(Stubs.takeError()).find("emitted fragment is too large"),
            std::string::npos);
}

TEST_F(AArch64StubBatchTest, RejectSplitEmittedFragment) {
  auto &Input = G.createSection(".bolt.fragment.0",
                                orc::MemProt::Read | orc::MemProt::Exec);
  G.createContentBlock(Input, ArrayRef<char>("aaaa", 4), orc::ExecutorAddr(0),
                       4, 0);
  G.createContentBlock(Input, ArrayRef<char>("bbbb", 4), orc::ExecutorAddr(4),
                       4, 0);
  EmittedFragmentSection Fragment{".bolt.fragment.0", ".text", 4, false};
  DeferredStubLinker::FragmentMap FragmentsByBlock;
  auto Err = DeferredStubLinker::mergeFragmentSections(G, {Fragment},
                                                       FragmentsByBlock);
  ASSERT_TRUE(bool(Err));
  EXPECT_NE(toString(std::move(Err)).find("Expected one block"),
            std::string::npos);
}

TEST_F(AArch64StubBatchTest, SkipProvenDirectBranches) {
  auto &Caller = block(8);
  auto &Callee = block(8);
  auto &Target = G.addAnonymousSymbol(Callee, 0, 8, true, true);
  branch(Caller, 0, Target);
  branch(Caller, 4, Target, 4);
  auto Stubs = reserve();
  ASSERT_TRUE(bool(Stubs)) << toString(Stubs.takeError());
  EXPECT_TRUE(Stubs->empty());
}

TEST_F(AArch64StubBatchTest, DirectBranchesDoNotConsumeIslandBudget) {
  // One more branch than a 16 MiB island could hold in 20-byte slots. All
  // targets are already placed and reachable, so the two fragments should
  // remain in one batch without reserving any slots.
  constexpr unsigned MaxSlots = 16 * MiB / 20;
  auto &Dense = block(4 * MaxSlots);
  auto &Caller = block(4);
  auto &Target = G.addAnonymousSymbol(Dense, 0, 4, true, true);
  for (unsigned I = 0; I < MaxSlots; ++I)
    branch(Dense, 4 * I, Target);
  branch(Caller, 0, Target);

  std::string Log;
  raw_string_ostream OS(Log);
  auto Stubs = DeferredStubLinker::reserveStubs(G, {}, &OS);
  ASSERT_TRUE(bool(Stubs)) << toString(Stubs.takeError());
  EXPECT_TRUE(Stubs->empty());
  EXPECT_NE(Log.find("batches=1 fragments=2 islands=0 slots=0"),
            std::string::npos);
}

TEST_F(AArch64StubBatchTest, BoundedEntryAlignmentPreservesBodyAlignment) {
  block(4);
  auto &Skip = block(28);
  auto &AtLimit = block(4);
  auto &Disabled = block(4);
  auto &Body = block(4, 16);
  EmittedFragmentSection Preferred{"", "", 1, false, 64, 32};
  EmittedFragmentSection NoPadding{"", "", 1, false, 64, 0};
  EmittedFragmentSection SmallBudget{"", "", 1, false, 64, 4};
  DeferredStubLinker::FragmentMap FragmentsByBlock{{&Skip, &Preferred},
                                                   {&AtLimit, &Preferred},
                                                   {&Disabled, &NoPadding},
                                                   {&Body, &SmallBudget}};
  auto Stubs = DeferredStubLinker::reserveStubs(G, FragmentsByBlock);
  ASSERT_TRUE(bool(Stubs)) << toString(Stubs.takeError());
  EXPECT_TRUE(Stubs->empty());
  EXPECT_EQ(Skip.getAddress().getValue(), 4u);     // 60 exceeds the budget.
  EXPECT_EQ(AtLimit.getAddress().getValue(), 64u); // Exactly 32 is allowed.
  EXPECT_EQ(Disabled.getAddress().getValue(), 68u);
  // Mandatory, even with budget 4.
  EXPECT_EQ(Body.getAddress().getValue(), 80u);
  EXPECT_EQ(Body.getAlignment(), 16u);
}

TEST_F(AArch64StubBatchTest, ShareAliasesButDistinguishAddends) {
  auto &Caller = block(12);
  auto &Alias = G.addAbsoluteSymbol("alias", Far.getAddress(), 0,
                                    Linkage::Strong, Scope::Default, true);
  branch(Caller, 0, Far);
  branch(Caller, 4, Alias);
  branch(Caller, 8, Alias, 4);
  auto Stubs = reserve();
  ASSERT_TRUE(bool(Stubs)) << toString(Stubs.takeError());
  ASSERT_EQ(Stubs->size(), 2u);
  EXPECT_EQ((*Stubs)[0].Branches.size(), 2u);
  EXPECT_EQ((*Stubs)[1].Branches.size(), 1u);
  checkReachability(*Stubs);
}

TEST_F(AArch64StubBatchTest, AlignmentGapsAndEmptyMarkers) {
  branch(block(8), 0, Far);
  auto &Empty = block(0);
  auto &Aligned = block(8, 256 * MiB, 4);
  branch(Aligned, 0, Far);
  auto Stubs = reserve();
  ASSERT_TRUE(bool(Stubs)) << toString(Stubs.takeError());
  ASSERT_EQ(Stubs->size(), 2u);
  checkReachability(*Stubs);
  EXPECT_EQ(Aligned.getAddress().getValue(), 256 * MiB + 4);
  EXPECT_LT(Empty.getAddress(), Aligned.getAddress());
  auto Ordered = JITLinkLinker::orderedBlocks(Text);
  EXPECT_LT(llvm::find(Ordered, &Empty), llvm::find(Ordered, &Aligned));
}

TEST_F(AArch64StubBatchTest, LastGuardNeedsNoSlot) {
  auto &Caller = block(8);
  auto Stubs = reserve({&Caller});
  ASSERT_TRUE(bool(Stubs)) << toString(Stubs.takeError());
  EXPECT_TRUE(Stubs->empty());
  ASSERT_EQ(Caller.edges_size(), 1u);
  auto &Guard = *Caller.edges().begin();
  EXPECT_EQ(Guard.getOffset(), 4u);
  EXPECT_EQ(Guard.getTarget().getAddress().getValue(), 8u);
  EXPECT_EQ(JITLinkLinker::getSectionAllocationSize(Text), 8u);
}

TEST_F(AArch64StubBatchTest, LastGuardSkipsTailIsland) {
  auto &Caller = G.createMutableContentBlock(Text, G.allocateBuffer(8),
                                             orc::ExecutorAddr(0), 4, 0);
  branch(Caller, 0, Far);
  auto Stubs = reserve({&Caller});
  ASSERT_TRUE(bool(Stubs)) << toString(Stubs.takeError());
  ASSERT_EQ(Stubs->size(), 1u);
  checkReachability(*Stubs);
  auto &Guard = *std::next(Caller.edges().begin());
  EXPECT_EQ(Guard.getOffset(), 4u);
  EXPECT_EQ(Guard.getTarget().getAddress().getValue(), 28u);
  EXPECT_EQ(JITLinkLinker::getSectionAllocationSize(Text), 28u);
}

TEST_F(AArch64StubBatchTest, LastGuardSkipsTailIslandsFromEarlierBatches) {
  SmallVector<Block *> Callers;
  for (unsigned I = 0; I != 4; ++I) {
    Callers.push_back(&block(16 * MiB));
    branch(*Callers.back(), 0, Far, 4 * I);
  }
  auto Stubs = reserve({Callers.back()});
  ASSERT_TRUE(bool(Stubs)) << toString(Stubs.takeError());
  ASSERT_EQ(Stubs->size(), 4u);
  checkReachability(*Stubs);
  auto &Guard = *std::next(Callers.back()->edges().begin());
  EXPECT_EQ(Guard.getTarget().getAddress().getValue(), 64 * MiB + 80);
  EXPECT_EQ(JITLinkLinker::getSectionAllocationSize(Text), 64 * MiB + 80);
}

} // namespace
