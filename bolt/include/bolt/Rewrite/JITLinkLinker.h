//===- bolt/Rewrite/JITLinkLinker.h - Linker using JITLink ------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// BOLTLinker using JITLink.
//
//===----------------------------------------------------------------------===//

#ifndef BOLT_REWRITE_JITLINK_LINKER_H
#define BOLT_REWRITE_JITLINK_LINKER_H

#include "bolt/Core/Linker.h"
#include "bolt/Rewrite/ExecutableFileMemoryManager.h"
#include "llvm/ExecutionEngine/JITLink/JITLink.h"
#include "llvm/ExecutionEngine/JITLink/JITLinkDylib.h"

#include <memory>
#include <vector>

namespace llvm {
namespace bolt {

class BinaryContext;

class JITLinkLinker : public BOLTLinker {
private:
  struct Context;
  friend struct Context;

  BinaryContext &BC;
  std::unique_ptr<ExecutableFileMemoryManager> MM;
  jitlink::JITLinkDylib Dylib{"main"};
  std::vector<ExecutableFileMemoryManager::FinalizedAlloc> Allocs;
  StringMap<SymbolInfo> Symtab;

public:
  JITLinkLinker(BinaryContext &BC,
                std::unique_ptr<ExecutableFileMemoryManager> MM);
  ~JITLinkLinker();

  void loadObject(MemoryBufferRef Obj, SectionsMapper MapSections) override;
  std::optional<SymbolInfo> lookupSymbolInfo(StringRef Name) const override;

  static SmallVector<jitlink::Block *, 2>
  orderedBlocks(const jitlink::Section &Section);
  /// Assign section-relative block addresses once, before allocation.
  static uint64_t layoutBlocks(ArrayRef<jitlink::Block *> Blocks,
                               uint64_t Offset = 0);
  /// Required allocation size from placed blocks' section-relative addresses.
  /// Call after layout and before rebasing to virtual addresses.
  static size_t getSectionAllocationSize(const jitlink::Section &Section);
  /// Rebase from the blocks' actual positions in the allocated section buffer.
  static void assignBlockAddresses(jitlink::Section &Section,
                                   const char *Buffer, uint64_t Address);
};

} // namespace bolt
} // namespace llvm

#endif // BOLT_REWRITE_JITLINK_LINKER_H
