## A synthetic fall-through guard must be relaxed when alignment puts its
## continuation outside Branch26 range, even if there are no other branches.

# REQUIRES: system-linux

# RUN: llvm-mc -filetype=obj -triple=aarch64-unknown-linux %s -o %t.o
# RUN: ld.lld --emit-relocs -e first %t.o -o %t.exe
# RUN: llvm-bolt %t.exe -o %t.bolt --turn-off-stubs --lite=0 --split-functions \
# RUN:   --align-functions=268435456 --verify-branch26-range
# RUN: llvm-objdump -d --no-show-raw-insn --disassemble-symbols=first,second \
# RUN:   %t.bolt | FileCheck %s --check-prefixes=UNIQUE,CHECK

# CHECK-LABEL: <first>:
# CHECK-NEXT: blr x0
# UNIQUE-NEXT: b 0x[[#%x,STUB:]]
# CHECK-NEXT: {{0*}}[[#STUB]]: adrp x16, 0x[[#%x,DEST:]]
# CHECK-NEXT: add x16, x16, #0x0
# CHECK-NEXT: br x16
# CHECK-NEXT: nop
# CHECK-NEXT: nop
# CHECK: {{0*}}[[#DEST]] <second>:
# CHECK-NEXT: ret

.text
.globl first
.type first, %function
first:
  blr x0
.size first, .-first

.globl second
.type second, %function
second:
  ret
.size second, .-second

.reloc 0, R_AARCH64_NONE
