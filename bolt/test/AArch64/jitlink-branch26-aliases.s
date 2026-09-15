## Branch26 relocation-preserving aliases scale with unique emitted targets,
## not edges, and collision-proof linker-private aliases do not leak into the
## final symbol table.

# REQUIRES: system-linux

# RUN: llvm-mc -filetype=obj -triple=aarch64-linux %s -o %t.o
# RUN: ld.lld %t.o -o %t.exe -q --entry=_start
# RUN: llvm-bolt %t.exe -o %t.bolt --jitlink-branch26-relaxation \
# RUN:   --verify-branch26-range 2>&1 | FileCheck %s --check-prefix=STATS
# RUN: llvm-readelf -sW %t.bolt | FileCheck %s --check-prefix=SYMBOLS
# RUN: llvm-objdump -d --show-all-symbols %t.bolt \
# RUN:   | FileCheck %s --check-prefix=UNUSED
# RUN: llvm-objdump -d --show-all-symbols %t.bolt \
# RUN:   | FileCheck %s --check-prefix=NO-THUNK

# STATS: BOLT-INFO: JITLink Branch26 relaxation: batches=1, groups=1, reserved=1, used=0, direct=5002, relaxed=0, semantic-blocks=3, group-bytes=20
# STATS-SAME: unique-targets=2, aliases=1
# STATS: BOLT-INFO: AArch64 Branch26PCRel edges: total=5002, out-of-range=0, CALL26=5001, JUMP26=1
# STATS-SAME: PLT=1

# SYMBOLS: __BOLT_jitlink_branch26_target_0_common
# SYMBOLS-NOT: .L__BOLT_jitlink_branch26_target_

## The pessimistic fixed-address slot is unused and remains NOP-filled.
# UNUSED: Disassembly of section .text:
# UNUSED: nop
# NO-THUNK-NOT: movz x16
# NO-THUNK-NOT: movk x16
# NO-THUNK-NOT: br x16

  .text
  .globl _start
  .type _start, %function
_start:
  .rept 5000
  bl common
  .endr
  bl fake_plt
  b common
  .size _start, .-_start

  .globl common
  .type common, %function
common:
  ret
  .size common, .-common

## This exact spelling collided with the old getOrCreateSymbol scheme.
  .globl __BOLT_jitlink_branch26_target_0_common
  .type __BOLT_jitlink_branch26_target_0_common, %function
__BOLT_jitlink_branch26_target_0_common:
  ret
  .set collision_size, .-__BOLT_jitlink_branch26_target_0_common
  .size __BOLT_jitlink_branch26_target_0_common, collision_size

  .section .plt,"ax",@progbits
  .globl fake_plt
  .type fake_plt, %function
fake_plt:
  ret
  .size fake_plt, .-fake_plt

  .reloc 0, R_AARCH64_NONE
