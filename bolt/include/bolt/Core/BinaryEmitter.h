//===- bolt/Core/BinaryEmitter.h - Emit code and data -----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains declarations of functions for emitting code and data into
// a binary file.
//
//===----------------------------------------------------------------------===//

#ifndef BOLT_CORE_BINARY_EMITTER_H
#define BOLT_CORE_BINARY_EMITTER_H

#include "llvm/ADT/StringRef.h"
#include <memory>

namespace llvm {
class MCStreamer;

namespace bolt {
class BinaryContext;
class BinaryFunction;
class FunctionFragment;

/// Final, read-only fragment planning, used only by
/// RewriteInstance::emitAndLink. Symbol creation and applying the plans are
/// left to the emission thread.
class BinaryEmissionPlan {
public:
  struct Impl;

  explicit BinaryEmissionPlan(BinaryContext &BC);
  ~BinaryEmissionPlan();

private:
  std::unique_ptr<Impl> State;
  friend void emitBinaryContext(MCStreamer &, BinaryContext &, StringRef,
                                BinaryEmissionPlan &);
};

/// Emit all code and data from the BinaryContext \p BC into the \p Streamer.
///
/// \p OrgSecPrefix is used to modify name of emitted original sections
/// contained in \p BC. This is done to distinguish them from sections emitted
/// by LLVM backend.
void emitBinaryContext(MCStreamer &Streamer, BinaryContext &BC,
                       StringRef OrgSecPrefix = "");

void emitBinaryContext(MCStreamer &Streamer, BinaryContext &BC,
                       StringRef OrgSecPrefix, BinaryEmissionPlan &Plan);

/// Emit \p BF function code. The caller is responsible for emitting function
/// symbol(s) and setting the section to emit the code to.
void emitFunctionBody(MCStreamer &Streamer, BinaryFunction &BF,
                      FunctionFragment &FF, bool EmitCodeOnly);

} // namespace bolt
} // namespace llvm

#endif
