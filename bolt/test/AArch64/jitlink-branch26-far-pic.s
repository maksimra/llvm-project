## A position-independent Branch26 thunk uses ADRP and cannot materialize a
## target more than 4 GiB away. Diagnose this experimental limitation before
## JITLink's generic Page21 fixup failure.

# REQUIRES: system-linux

# RUN: llvm-mc -filetype=obj -triple=aarch64-linux %s -o %t.o
# RUN: ld.lld %t.o -o %t.exe -q -pie --entry=_start \
# RUN:   --section-start=.oldtext=0x10000 --section-start=.text=0x20000 \
# RUN:   --section-start=.high=0x100000000
# RUN: not llvm-bolt %t.exe -o %t.bolt --lite=0 \
# RUN:   --jitlink-branch26-relaxation --skip-funcs=far_target 2>&1 \
# RUN:   | FileCheck %s

# CHECK: BOLT-ERROR: experimental JITLink Branch26 relaxation cannot materialize
# CHECK-SAME: PIC target far_target + 0 from _start[fragment 0]+0x0:
# CHECK-SAME: the post-batch thunk group ADRP page displacement is
# CHECK-SAME: {{-?[0-9]+}}; the supported Page21 range is +/-4 GiB
# CHECK-NOT: out of range of Page21

  .text
  .globl _start
  .type _start, %function
_start:
  bl far_target
  ret
  .size _start, .-_start

  .section .oldtext,"ax",@progbits
  .globl far_target
  .type far_target, %function
far_target:
  ret
  .size far_target, .-far_target

## Raise BOLT's first alloc address without creating a large physical file.
  .section .high,"a",@progbits
  .byte 0

  .reloc 0, R_AARCH64_NONE
