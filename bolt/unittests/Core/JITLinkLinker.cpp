//===- bolt/unittest/Core/JITLinkLinker.cpp -------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "bolt/Rewrite/JITLinkLinker.h"
#include "bolt/Core/BinaryContext.h"
#include "bolt/Core/BinarySection.h"
#include "bolt/Rewrite/ExecutableFileMemoryManager.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/DebugInfo/DWARF/DWARFContext.h"
#include "llvm/ExecutionEngine/JITLink/JITLink.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/TargetSelect.h"
#include "gtest/gtest.h"

using namespace llvm;
using namespace llvm::bolt;
using namespace llvm::jitlink;
using namespace llvm::object;

namespace {

class JITLinkLinkerTest : public testing::Test {
protected:
  void SetUp() override {
#define BOLT_TARGET(target)                                                    \
  LLVMInitialize##target##TargetInfo();                                        \
  LLVMInitialize##target##TargetMC();                                          \
  LLVMInitialize##target##AsmParser();                                         \
  LLVMInitialize##target##Disassembler();                                      \
  LLVMInitialize##target##Target();                                            \
  LLVMInitialize##target##AsmPrinter();

#include "bolt/Core/TargetConfig.def"

    memcpy(ElfBuf, "\177ELF", 4);
    auto *EHdr = reinterpret_cast<ELF::Elf64_Ehdr *>(ElfBuf);
    EHdr->e_ident[ELF::EI_CLASS] = ELF::ELFCLASS64;
    EHdr->e_ident[ELF::EI_DATA] = ELF::ELFDATA2LSB;
    EHdr->e_machine = ELF::EM_X86_64;
    MemoryBufferRef Source(StringRef(ElfBuf, sizeof(ElfBuf)), "ELF");
    ObjFile = cantFail(ObjectFile::createObjectFile(Source));

    const Triple TheTriple = Triple::x86_64;
    Relocation::Arch = TheTriple.getArch();
    BC = cantFail(BinaryContext::createBinaryContext(
        TheTriple, std::make_shared<orc::SymbolStringPool>(),
        ObjFile->getFileName(), nullptr, true, DWARFContext::create(*ObjFile),
        {llvm::outs(), llvm::errs()}));
  }

  char ElfBuf[sizeof(ELF::Elf64_Ehdr)] = {};
  std::unique_ptr<ObjectFile> ObjFile;
  std::unique_ptr<BinaryContext> BC;
};

} // namespace

TEST_F(JITLinkLinkerTest, AssignBlockAddressesMatchesAllocatedLayout) {
  LinkGraph G("test", std::make_shared<orc::SymbolStringPool>(),
              Triple("x86_64-unknown-linux"), SubtargetFeatures(),
              getGenericEdgeKindName);
  auto &Section =
      G.createSection(".data", orc::MemProt::Read | orc::MemProt::Write);

  auto &First = G.createContentBlock(Section, ArrayRef<char>("aaaaa", 5),
                                     orc::ExecutorAddr(0), 16, 0);
  auto &Second = G.createContentBlock(Section, ArrayRef<char>("bbbb", 4),
                                      orc::ExecutorAddr(20), 16, 4);

  EXPECT_EQ(JITLinkLinker::sectionSize(Section), 24U);

  ExecutableFileMemoryManager MemMgr(*BC);
  auto InFlightAlloc =
      cantFail(static_cast<jitlink::JITLinkMemoryManager &>(MemMgr).allocate(
          nullptr, G));
  ASSERT_TRUE(InFlightAlloc);

  auto BinarySectionOrErr = BC->getUniqueSectionByName(".data");
  ASSERT_TRUE(BinarySectionOrErr);
  BinarySection &OutputSection = *BinarySectionOrErr;
  const char *SectionBuffer =
      reinterpret_cast<const char *>(OutputSection.getOutputData());
  ASSERT_NE(SectionBuffer, nullptr);
  EXPECT_EQ(OutputSection.getOutputSize(), 24U);
  EXPECT_EQ(OutputSection.getAlignment(), 16U);
  EXPECT_EQ(First.getContent().data(), SectionBuffer);
  EXPECT_EQ(Second.getContent().data(), SectionBuffer + 20);

  constexpr uint64_t SectionAddress = 0x1000;
  JITLinkLinker::assignBlockAddresses(Section, SectionAddress);
  EXPECT_EQ(First.getAddress().getValue(), SectionAddress);
  EXPECT_EQ(Second.getAddress().getValue(), SectionAddress + 20);
  EXPECT_EQ(Second.getAddress().getValue() % Second.getAlignment(),
            Second.getAlignmentOffset());
}
