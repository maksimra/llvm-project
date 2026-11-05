//===- bolt/unittest/Core/JITLinkLinker.cpp -------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "bolt/Rewrite/JITLinkLinker.h"
#include "llvm/ExecutionEngine/JITLink/JITLink.h"
#include "gtest/gtest.h"

using namespace llvm;
using namespace llvm::bolt;
using namespace llvm::jitlink;

TEST(JITLinkLinkerTest, LayoutBlocksRespectsAlignment) {
  LinkGraph G("test", std::make_shared<orc::SymbolStringPool>(),
              Triple("x86_64-unknown-linux"), 8, SubtargetFeatures(),
              getGenericEdgeKindName);
  auto &Section =
      G.createSection(".data", orc::MemProt::Read | orc::MemProt::Write);

  auto &First = G.createContentBlock(Section, ArrayRef<char>("aaaaa", 5),
                                     orc::ExecutorAddr(0), 16, 0);
  auto &Second = G.createContentBlock(Section, ArrayRef<char>("bbbb", 4),
                                      orc::ExecutorAddr(20), 16, 4);

  JITLinkLinker::layoutBlocks({&First, &Second});
  EXPECT_EQ(First.getAddress().getValue() % First.getAlignment(),
            First.getAlignmentOffset());
  EXPECT_EQ(Second.getAddress().getValue() % Second.getAlignment(),
            Second.getAlignmentOffset());
}

TEST(JITLinkLinkerTest, CopyAndRebaseUseBufferOffsets) {
  LinkGraph G("test", std::make_shared<orc::SymbolStringPool>(),
              Triple("x86_64-unknown-linux"), 8, SubtargetFeatures(),
              getGenericEdgeKindName);
  auto &Section =
      G.createSection(".data", orc::MemProt::Read | orc::MemProt::Write);
  auto &First = G.createContentBlock(Section, ArrayRef<char>("first", 5),
                                     orc::ExecutorAddr(0), 16, 0);
  auto &Empty = G.createZeroFillBlock(Section, 0, orc::ExecutorAddr(1), 1, 0);
  auto &Last = G.createContentBlock(Section, ArrayRef<char>("last", 4),
                                    orc::ExecutorAddr(2), 16, 4);

  EXPECT_EQ(JITLinkLinker::layoutBlocks({&First, &Empty, &Last}), 24u);
  EXPECT_EQ(JITLinkLinker::getSectionAllocationSize(Section), 24u);
  EXPECT_EQ(Last.getAddress().getValue(), 20u);

  SmallVector<char> Buffer(JITLinkLinker::getSectionAllocationSize(Section), 0);
  ExecutableFileMemoryManager::copySectionContents(Section, Buffer.data());
  EXPECT_EQ(StringRef(Buffer.data(), 5), "first");
  EXPECT_EQ(StringRef(Buffer.data() + 20, 4), "last");
  EXPECT_EQ(Last.getContent().data(), Buffer.data() + 20);

  JITLinkLinker::assignBlockAddresses(Section, Buffer.data(), 0x1000);
  EXPECT_EQ(First.getAddress().getValue(), 0x1000u);
  EXPECT_EQ(Empty.getAddress().getValue(), 0x1005u);
  EXPECT_EQ(Last.getAddress().getValue(), 0x1014u);
  JITLinkLinker::assignBlockAddresses(Section, Buffer.data(), 0x2000);
  EXPECT_EQ(First.getAddress().getValue(), 0x2000u);
  EXPECT_EQ(Last.getAddress().getValue(), 0x2014u);
}
