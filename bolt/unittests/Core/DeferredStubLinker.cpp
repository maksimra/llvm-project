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

  void checkReachability(ArrayRef<DeferredStubLinker::Stub> Stubs) {
    for (const auto &Stub : Stubs)
      for (const auto &Branch : Stub.Branches)
        EXPECT_TRUE(isInt<28>(
            int64_t(Stub.SlotSym->getAddress() -
                    (Branch.SourceBlock->getAddress() + Branch.SourceOffset))));
  }
};

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
