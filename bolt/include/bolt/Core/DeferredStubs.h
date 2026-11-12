//===- DeferredStubs.h - MC preparation for deferred stubs ------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef BOLT_CORE_DEFERRED_STUBS_H
#define BOLT_CORE_DEFERRED_STUBS_H

#include "llvm/ADT/ArrayRef.h"
#include <cstddef>
#include <cstdint>
#include <string>

namespace llvm {
class MCInst;
class MCStreamer;
class MCSymbol;
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

/// Whether to emit independently placed fragments and defer global relaxation.
bool usesDeferredStubs(const BinaryContext &BC);

/// Private, non-allocatable records consumed before JITLink pruning. Each
/// 32-byte record contains three Pointer64 relocations (source, destination,
/// optional fallback), the original instruction with a zero displacement,
/// the bit width of the signed byte displacement, the inline size, and two
/// zero bytes.
inline constexpr char DeferredBranchSectionName[] = ".bolt.branch_info";
struct DeferredBranchRecord {
  uint64_t Source;
  uint64_t Target;
  uint64_t LongBranch;
  uint32_t Encoding;
  uint8_t DisplacementBits;
  uint8_t ReservedSize;
  uint16_t Padding;
};
inline constexpr unsigned DeferredBranchRecordSize =
    sizeof(DeferredBranchRecord);

enum class BranchExpansion { Inline, Tail, None };

/// Emit a direct branch without asking MC to resolve its real destination.
/// Return a tail trampoline label for the caller to emit, if requested.
/// Measurement emission uses the same code forms but omits the linker record.
MCSymbol *emitDeferredBranch(MCStreamer &Streamer, BinaryContext &BC,
                             const MCInst &Branch, BranchExpansion Expansion,
                             bool EmitRecord = true);

/// Whether fragment emission must append a guard before inserting islands.
bool fragmentMayFallThrough(const FunctionFragment &Fragment,
                            const BinaryContext &BC);

} // namespace bolt
} // namespace llvm

#endif // BOLT_CORE_DEFERRED_STUBS_H
