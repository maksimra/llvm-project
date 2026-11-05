//===- DeferredStubs.h - MC preparation for deferred stubs ------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef BOLT_CORE_DEFERRED_STUBS_H
#define BOLT_CORE_DEFERRED_STUBS_H

#include <cstdint>
#include <string>

namespace llvm {
namespace bolt {
class BinaryContext;
class FunctionFragment;

/// An emitted fragment's private MC section and its logical output section.
struct EmittedFragmentSection {
  std::string Name;
  std::string OutputSection;
  uint64_t OutputAlignment;
  /// The fragment ends with a fall-through guard to be retargeted by JITLink.
  bool HasFallThrough;
  /// Optional entry alignment, applied after the block's mandatory alignment.
  /// Zero MaxAlignmentBytes disables it, as in BinaryEmitter::emitFunction.
  uint64_t PreferredAlignment{1};
  uint16_t MaxAlignmentBytes{0};
  /// Skip any unexecuted --pad-funcs-before bytes when entering this fragment.
  uint64_t EntryOffset{0};
};

/// Whether fragment emission must append a guard before inserting islands.
bool fragmentMayFallThrough(const FunctionFragment &Fragment,
                            const BinaryContext &BC);

} // namespace bolt
} // namespace llvm

#endif // BOLT_CORE_DEFERRED_STUBS_H
