## CMPBR has no ELF branch relocation and some conditions cannot be inverted
## without clobbering NZCV. Preserve its condition in a 12-byte reservation.

# REQUIRES: system-linux, asserts
# RUN: llvm-mc -filetype=obj -triple=aarch64-unknown-linux -mattr=+cmpbr \
# RUN:   %s -o %t.o
# RUN: ld.lld --emit-relocs -e _start %t.o -o %t.exe
# RUN: link_fdata --no-lbr %s %t.exe %t.fdata
# RUN: llvm-bolt %t.exe -o %t.bolt --turn-off-stubs --lite=0 \
# RUN:   --data=%t.fdata --align-blocks --align-blocks-threshold=0 \
# RUN:   --block-alignment=32 --verify-branch26-range
# RUN: llvm-objdump -d --no-show-raw-insn --section=.text %t.bolt | FileCheck %s

# CHECK-LABEL: <_start>:
# CHECK-NEXT: cmp x0, #0x3f
# CHECK-NEXT: [[#%x,BRANCH:]]:
# CHECK-SAME: cbgt x1, #0x3f, 0x[[#BRANCH + 8]]
# CHECK-NEXT: b 0x[[#BRANCH + 12]]
# CHECK-NEXT: b 0x[[#BRANCH + 1052]]
# CHECK: [[#BRANCH + 1052]]: csel x0, x1, x2, le
# CHECK-NOT: cmp

.text
.globl _start
.type _start, %function
_start:
.entry:
# FDATA: 1 _start #.entry# 100
  cmp x0, #63
  cbgt x1, #63, .target
.body:
# FDATA: 1 _start #.body# 1
  .rept 253
    add x2, x2, #1
  .endr
.target:
# FDATA: 1 _start #.target# 100
  csel x0, x1, x2, le
  .rept 7
    add x0, x0, #1
  .endr
  ret
.size _start, .-_start
.reloc 0, R_AARCH64_NONE
