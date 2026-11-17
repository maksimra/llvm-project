## Local relaxation must use MC's exact block padding, including the section
## prefix and the maximum-byte budget, rather than summing worst-case padding.

# REQUIRES: system-linux
# RUN: llvm-mc -filetype=obj -triple=aarch64-unknown-linux -mattr=+cmpbr %s -o %t.o
# RUN: ld.lld --emit-relocs -e near %t.o -o %t.exe
# RUN: link_fdata %s %t.exe %t.fdata
# RUN: llvm-strip --strip-unneeded %t.exe

## The target is already aligned. The branch displacement is exactly the
## positive CMPBR limit, 1020 bytes. Charging another 31 bytes would expand it.
# RUN: llvm-bolt %t.exe -o %t.near --turn-off-stubs --lite=0 --keep-nops \
# RUN:   --preserve-blocks-alignment --align-functions=4 --verify-branch26-range
# RUN: llvm-objdump -d --no-show-raw-insn --mattr=+cmpbr \
# RUN:   --disassemble-symbols=near %t.near | FileCheck %s --check-prefix=NEAR
# NEAR: [[#%x,START:]] <near>:
# NEAR-NEXT: mov x9, x0
# NEAR-NEXT: cbgt x0, #0x0, 0x[[#START + 1024]]
# NEAR-NEXT: nop
# NEAR: {{0*}}[[#START + 1024]]: ret

## A four-byte section prefix changes the phase of the 32-byte alignment.
## Its unlimited budget adds padding, putting the target beyond branch reach.
## Expansion absorbs four bytes of that padding on the next layout iteration.
# RUN: llvm-bolt %t.exe -o %t.prefix --turn-off-stubs --lite=0 --keep-nops \
# RUN:   --preserve-blocks-alignment --align-functions=4 --pad-funcs-before=near:4 \
# RUN:   --verify-branch26-range
# RUN: llvm-objdump -d --no-show-raw-insn --mattr=+cmpbr \
# RUN:   --disassemble-symbols=near %t.prefix | FileCheck %s --check-prefix=PREFIX
# PREFIX: [[#%x,START:]] <near>:
# PREFIX-NEXT: mov x9, x0
# PREFIX-NEXT: cblt x0, #0x1, 0x[[#START + 12]]
# PREFIX-NEXT: b 0x[[#START + 1052]]
# PREFIX-NEXT: {{0*}}[[#START + 12]]: nop
# PREFIX: {{0*}}[[#START + 1052]]: ret

## With a four-byte budget for the RET block, the required 28 padding bytes
## must be skipped altogether. The original branch remains in range.
# RUN: llvm-bolt %t.exe -o %t.bounded --turn-off-stubs --lite=0 --keep-nops \
# RUN:   --data=%t.fdata --align-blocks --block-alignment=32 \
# RUN:   --align-blocks-threshold=0 --align-functions=4 --pad-funcs-before=near:4 \
# RUN:   --print-after-lowering --print-only=near --verify-branch26-range \
# RUN:   | FileCheck %s --check-prefix=ALIGNED
# ALIGNED: (1 instructions, align : 32)
# RUN: llvm-objdump -d --no-show-raw-insn --mattr=+cmpbr \
# RUN:   --disassemble-symbols=near %t.bounded | FileCheck %s --check-prefix=NEAR

.text
.globl near
.type near, %function
near:
# FDATA: 0 [unknown] 0 1 near 0 0 100
  mov x9, x0
.branch:
# FDATA: 1 near #.branch# 1 near #.target# 0 100
  cbgt x0, #0, .target
  .rept 254
  nop
  .endr
.target:
  ret
.size near, .-near
.reloc 0, R_AARCH64_NONE
