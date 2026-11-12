//===- PrepareDeferredBTI.h - BTI preparation ------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef BOLT_PASSES_PREPAREDEFERREDBTI_H
#define BOLT_PASSES_PREPAREDEFERREDBTI_H

#include "bolt/Passes/BinaryPasses.h"

namespace llvm {
namespace bolt {

/// Add landing pads before CFG finalization; deferred stubs may use BR x16.
class PrepareDeferredBTI : public BinaryFunctionPass {
public:
  PrepareDeferredBTI() : BinaryFunctionPass(false) {}
  const char *getName() const override { return "prepare deferred BTI"; }
  Error runOnFunctions(BinaryContext &BC) override;
};

} // namespace bolt
} // namespace llvm

#endif
